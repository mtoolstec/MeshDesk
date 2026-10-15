#include "SdrService.h"

#include <esp_event.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <heap_memory_layout.h>
#include <soc/soc.h>
#include <math.h>

#include "ConnectivityService.h"

SdrService sdrService;

namespace {
// See ESPARGOS/esp-sdr and lozaning/ESP32SDR for the S3 capture-register
// findings. Reserve the entire RF SRAM aperture so the heap cannot use it.
SOC_RESERVE_MEMORY_REGION(0x3fcd0000, 0x3fce0000, sdr_iq);
constexpr uintptr_t IQ_BASE = 0x3fcd0000;
constexpr uintptr_t SRAM_OWNER = 0x600c101c;
constexpr uintptr_t RX_CONTROL = 0x60033d5c;
constexpr uintptr_t RX_FORMAT = 0x60033d90;
constexpr uint16_t IQ_COUNT = 1024;
constexpr uint16_t FFT_SIZE = 256;
volatile uint32_t *const iq = reinterpret_cast<volatile uint32_t *>(IQ_BASE);

extern "C" void stop_tx_tone(unsigned);
extern "C" void rom_pbus_workmode(void);
extern "C" void rom_pbus_xpd_tx_off(void);
extern "C" void rom_pbus_xpd_rx_on(unsigned);
extern "C" void rom_set_rxclk_en(unsigned);

int16_t signed10(uint32_t value) {
    value &= 0x3ff;
    return value & 0x200 ? int16_t(value) - 1024 : int16_t(value);
}

void fft256(float *real, float *imag) {
    for (uint16_t i = 1, j = 0; i < FFT_SIZE; ++i) {
        uint16_t bit = FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            const float r = real[i], q = imag[i];
            real[i] = real[j]; imag[i] = imag[j];
            real[j] = r; imag[j] = q;
        }
    }
    for (uint16_t length = 2; length <= FFT_SIZE; length <<= 1) {
        const float phase = -2.0f * PI / length;
        const float wr = cosf(phase), wi = sinf(phase);
        for (uint16_t base = 0; base < FFT_SIZE; base += length) {
            float cr = 1.0f, ci = 0.0f;
            for (uint16_t k = 0; k < length / 2; ++k) {
                const uint16_t even = base + k, odd = even + length / 2;
                const float tr = cr * real[odd] - ci * imag[odd];
                const float ti = cr * imag[odd] + ci * real[odd];
                real[odd] = real[even] - tr; imag[odd] = imag[even] - ti;
                real[even] += tr; imag[even] += ti;
                const float next = cr * wr - ci * wi;
                ci = cr * wi + ci * wr; cr = next;
            }
        }
    }
}
}

bool SdrService::start(uint8_t channel) {
    if (active_) return tune(channel);
    if (channel < 1 || channel > 11) { error_ = "Channel must be 1-11"; return false; }
    connectivity.suspendWifiForSdr();
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) goto fail;
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) goto fail;
    {
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        result = esp_wifi_init(&config);
    }
    if (result != ESP_OK) goto fail;
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (result == ESP_OK) result = esp_wifi_start();
    if (result == ESP_OK) result = esp_wifi_set_ps(WIFI_PS_NONE);
    if (result == ESP_OK) result = esp_wifi_set_promiscuous(true);
    if (result != ESP_OK) {
        esp_wifi_stop(); esp_wifi_deinit();
        goto fail;
    }
    active_ = true;
    if (!tune(channel)) { stop(); return false; }
    error_ = "";
    Serial.printf("SDR: RX started on Wi-Fi channel %u\n", unsigned(channel_));
    return true;
fail:
    error_ = "Wi-Fi PHY initialization failed";
    Serial.printf("SDR: PHY init failed (%d)\n", int(result));
    connectivity.resumeWifiAfterSdr();
    return false;
}

bool SdrService::tune(uint8_t channel) {
    if (!active_ || channel < 1 || channel > 11) { error_ = "Invalid SDR channel"; return false; }
    const esp_err_t result = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (result != ESP_OK) { error_ = "RF tune failed"; return false; }
    stop_tx_tone(1);
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    channel_ = channel;
    error_ = "";
    return true;
}

bool SdrService::capture() {
    for (uint16_t i = 0; i < IQ_COUNT; ++i) iq[i] = 0xa5a0055a;
    const uint32_t owner = REG_READ(SRAM_OWNER);
    REG_WRITE(RX_CONTROL, 0);
    REG_WRITE(RX_FORMAT, 0x000c2040); // Four adjacent RX packing selectors.
    REG_WRITE(SRAM_OWNER, (owner & ~0x0fu) | 4u);
    // Divider 6: roughly 16 MS/s, about 16 MHz of displayed baseband span.
    const uint32_t control = 0x80000000u | (1u << 16) | IQ_COUNT;
    REG_WRITE(RX_CONTROL, control);
    REG_WRITE(RX_CONTROL, control | (1u << 19));
    REG_WRITE(RX_CONTROL, control);
    const int64_t deadline = esp_timer_get_time() + 20000;
    while (!(REG_READ(RX_CONTROL) & (1u << 18)) && esp_timer_get_time() < deadline) {}
    const bool done = (REG_READ(RX_CONTROL) & (1u << 18)) != 0;
    REG_WRITE(RX_CONTROL, 0);
    REG_WRITE(SRAM_OWNER, owner);
    if (!done || iq[0] == 0xa5a0055a || iq[IQ_COUNT - 1] == 0xa5a0055a) {
        error_ = "RF capture timed out";
        return false;
    }
    return true;
}

bool SdrService::spectrum(uint8_t output[BIN_COUNT]) {
    if (!active_) { error_ = "SDR is off"; return false; }
    if (!capture()) return false;
    float powers[BIN_COUNT]{};
    float real[FFT_SIZE], imag[FFT_SIZE];
    for (uint8_t segment = 0; segment < IQ_COUNT / FFT_SIZE; ++segment) {
        int32_t sumI = 0, sumQ = 0;
        for (uint16_t i = 0; i < FFT_SIZE; ++i) {
            const uint32_t word = iq[segment * FFT_SIZE + i];
            sumI += signed10(word); sumQ += signed10(word >> 10);
        }
        const float meanI = float(sumI) / FFT_SIZE, meanQ = float(sumQ) / FFT_SIZE;
        for (uint16_t i = 0; i < FFT_SIZE; ++i) {
            const uint32_t word = iq[segment * FFT_SIZE + i];
            const float window = 0.5f - 0.5f * cosf(2.0f * PI * i / (FFT_SIZE - 1));
            real[i] = (signed10(word) - meanI) * window;
            imag[i] = (signed10(word >> 10) - meanQ) * window;
        }
        fft256(real, imag);
        for (uint16_t i = 0; i < FFT_SIZE; ++i) {
            const uint8_t bin = uint8_t(((i + FFT_SIZE / 2) % FFT_SIZE) / 4);
            powers[bin] += real[i] * real[i] + imag[i] * imag[i];
        }
    }
    for (uint8_t bin = 0; bin < BIN_COUNT; ++bin) {
        const int level = int(10.0f * log10f(powers[bin] / 16.0f + 1.0f));
        output[bin] = uint8_t(constrain(level, 0, 100));
    }
    static uint32_t lastDiagnosticMs = 0;
    if (millis() - lastDiagnosticMs >= 5000) {
        lastDiagnosticMs = millis();
        uint8_t minimum = 100, maximum = 0;
        for (uint8_t bin = 0; bin < BIN_COUNT; ++bin) {
            minimum = min(minimum, output[bin]);
            maximum = max(maximum, output[bin]);
        }
        Serial.printf("SDR: ch=%u spectrum=%u..%u dB rel\n",
                      unsigned(channel_), unsigned(minimum), unsigned(maximum));
    }
    error_ = "";
    return true;
}

void SdrService::stop() {
    if (!active_) return;
    REG_WRITE(RX_CONTROL, 0);
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
    active_ = false;
    connectivity.resumeWifiAfterSdr();
    Serial.println("SDR: RX stopped");
}

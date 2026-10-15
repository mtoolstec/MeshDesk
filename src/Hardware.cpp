#include "Hardware.h"

#include <Adafruit_ADS1X15.h>
#include <AudioBoard.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>

Hardware hardware;
static Adafruit_ADS1115 ads;
static DriverPins codecPins;
static AudioBoard codecBoard(AudioDriverES8311, codecPins);

namespace {
int64_t utcDaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = unsigned(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return int64_t(era) * 146097 + int64_t(doe) - 719468;
}

double nmeaCoordinate(const char *value, const char *hemisphere) {
    if (!value || !value[0]) return 0;
    const double raw = atof(value);
    const int degrees = int(raw / 100.0);
    double result = degrees + (raw - degrees * 100.0) / 60.0;
    if (hemisphere && (*hemisphere == 'S' || *hemisphere == 'W')) result = -result;
    return result;
}

constexpr uint32_t GPS_SATELLITE_AGE_MS = 8000;
constexpr uint32_t GPS_CONFIG_SETTLE_MS = 250;

void writeGpsConfig(const char *payload) {
    uint8_t checksum = 0;
    for (const char *cursor = payload; *cursor; ++cursor) checksum ^= uint8_t(*cursor);
    Serial1.write('$');
    Serial1.print(payload);
    Serial1.printf("*%02X\r\n", unsigned(checksum));
}
}

bool IoExpander::writePair(uint8_t reg, uint16_t value) {
    Wire.beginTransmission(pins::EXPANDER_ADDR);
    Wire.write(reg);
    Wire.write(value & 0xFF);
    Wire.write(value >> 8);
    return Wire.endTransmission() == 0;
}

bool IoExpander::readPair(uint8_t reg, uint16_t &value) {
    Wire.beginTransmission(pins::EXPANDER_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(pins::EXPANDER_ADDR, uint8_t(2)) != 2) return false;
    value = Wire.read();
    value |= uint16_t(Wire.read()) << 8;
    return true;
}

bool IoExpander::begin() {
    uint16_t ignored;
    return readPair(0x00, ignored);
}

bool IoExpander::pinMode(uint8_t pin, bool output) {
    if (output) direction_ &= ~(1U << pin); else direction_ |= 1U << pin;
    return writePair(0x06, direction_);
}

bool IoExpander::write(uint8_t pin, bool high) {
    if (high) output_ |= 1U << pin; else output_ &= ~(1U << pin);
    return writePair(0x02, output_);
}

int IoExpander::read(uint8_t pin) {
    uint16_t input;
    return readPair(0x00, input) ? ((input >> pin) & 1U) : -1;
}

void WioBacklight::writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(0x2C);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

bool WioBacklight::init(uint8_t brightness) {
    writeRegister(0x00, 0x01);
    writeRegister(0x01, 0x01);
    writeRegister(0x02, 0x00);
    writeRegister(0x04, 0x4E);
    writeRegister(0x05, 0xF0);
    for (uint8_t i = 0; i < 4; ++i) writeRegister(0x14 + i, 200);
    writeRegister(0x02, 0x0F);
    writeRegister(0x0F, 0x55);
    setBrightness(brightness);
    return true;
}

void WioBacklight::setBrightness(uint8_t brightness) {
    brightness_ = brightness;
    for (uint8_t i = 0; i < 4; ++i) writeRegister(0x18 + i, brightness);
}

WioDisplay::WioDisplay() {
    auto bus = bus_.config();
    bus.spi_host = SPI3_HOST;
    bus.spi_mode = 3;
    bus.freq_write = 75000000;
    bus.freq_read = 16000000;
    bus.pin_sclk = pins::LCD_SCLK;
    bus.pin_io0 = pins::LCD_IO0;
    bus.pin_io1 = pins::LCD_IO1;
    bus.pin_io2 = pins::LCD_IO2;
    bus.pin_io3 = pins::LCD_IO3;
    bus_.config(bus);
    panel_.setBus(&bus_);

    auto panel = panel_.config();
    panel.pin_cs = pins::LCD_CS;
    panel.pin_rst = -1;
    panel.panel_width = 240;
    panel.panel_height = 320;
    panel.memory_width = 240;
    panel.memory_height = 320;
    panel.offset_rotation = 1;
    panel.invert = true;
    // LVGL supplies standard RGB pixels, while this NV3031B module requires
    // the controller's BGR bit for those pixels to appear in RGB order on the
    // physical glass. Without it red/blue are swapped (cyan -> yellow).
    panel.rgb_order = true;
    panel.bus_shared = false;
    panel_.config(panel);

    auto touch = touch_.config();
    touch.x_min = 0;
    touch.x_max = 239;
    touch.y_min = 0;
    touch.y_max = 319;
    touch.offset_rotation = 2;
    touch.i2c_port = 0;
    touch.i2c_addr = 0x5D;
    touch.pin_sda = pins::I2C_SDA;
    touch.pin_scl = pins::I2C_SCL;
    touch.freq = 100000;
    touch.bus_shared = false;
    touch_.config(touch);
    panel_.setTouch(&touch_);
    panel_.setLight(&light_);
    setPanel(&panel_);
}

bool WioDisplay::init_impl(bool useReset, bool useClear) {
    light_.init(light_.brightness());
    const bool result = LGFX_Device::init_impl(useReset, useClear);
    // The GT911 probe can leave ESP32 I2C0 BUSY. Restore Arduino Wire for
    // the expander, battery ADC and backlight, as done by the DVT firmware.
    Wire.end();
    Wire.begin(pins::I2C_SDA, pins::I2C_SCL);
    Wire.setClock(100000);
    return result;
}

bool Hardware::begin() {
    Wire.begin(pins::I2C_SDA, pins::I2C_SCL);
    Wire.setClock(100000);
    pinMode(pins::EXPANDER_IRQ, INPUT_PULLUP);
    pinMode(0, INPUT_PULLUP);
    if (!io.begin()) return false;

    const uint8_t outputs[] = {pins::CODEC_EN, pins::LCD_POWER, pins::LCD_RESET, pins::GROVE_EN,
        pins::TOUCH_RESET, pins::GNSS_RESET, pins::USER_LED, pins::OTG_EN,
        pins::AUDIO_PA_EN, pins::GNSS_EN, pins::SD_POWER, pins::BAT_ADC_EN};
    for (uint8_t pin : outputs) io.pinMode(pin, true);
    io.pinMode(pins::WAKE_BUTTON, false);
    io.write(pins::OTG_EN, false);
    io.write(pins::GROVE_EN, false);
    io.write(pins::LCD_POWER, true);
    // Keep the external amplifier muted while the codec and I2S clocks settle.
    // Enabling it during boot amplifies the codec's startup transient and idle noise.
    io.write(pins::AUDIO_PA_EN, false);
    io.write(pins::CODEC_EN, true);
    Preferences preferences;
    preferences.begin("mesh-hw", true);
    gpsEnabled_ = preferences.getBool("gps", false);
    volume_ = preferences.getUChar("volume", 62);
    userBrightness_ = preferences.getUChar("brightness", 180);
    preferences.end();
    io.write(pins::GNSS_EN, gpsEnabled_);
    // GNSS_RST is active HIGH on Wio Tracker L2.  Keep a disabled receiver in
    // reset; when enabled, pulse HIGH and release LOW before opening its UART.
    io.write(pins::GNSS_RESET, true);
    if (gpsEnabled_) { delay(10); io.write(pins::GNSS_RESET, false); }
    io.write(pins::BAT_ADC_EN, true);

    io.write(pins::LCD_RESET, true); delay(5);
    io.write(pins::LCD_RESET, false); delay(10);
    io.write(pins::LCD_RESET, true); delay(120);

    // GT911 samples INT during reset to select its I2C address (LOW -> 0x5D).
    io.write(pins::TOUCH_RESET, false); delay(10);
    io.pinMode(pins::TOUCH_INT, true);
    io.write(pins::TOUCH_INT, false); delay(1);
    io.write(pins::TOUCH_RESET, true); delay(60);

    if (!display.init()) return false;
    display.setRotation(0);
    display.fillScreen(TFT_BLACK);
    display.backlight().setBrightness(userBrightness_);
    displayAwake_ = true;
    delay(20);
    audioReady_ = beginAudio();
    if (audioReady_) io.write(pins::AUDIO_PA_EN, true);
    if (audioReady_) {
        toneQueue_ = xQueueCreate(8, sizeof(ToneRequest));
        if (toneQueue_ && xTaskCreatePinnedToCore(audioTask, "audio", 4096, this, 1, nullptr, 0) != pdPASS) {
            vQueueDelete(toneQueue_);
            toneQueue_ = nullptr;
        }
    }
    Serial.printf("Audio: ES8311 %s\n", audioReady_ ? "ready" : "unavailable");
    adsReady_ = ads.begin(0x48, &Wire);
    if (adsReady_) {
        ads.setGain(GAIN_ONE);
        Serial.println("Battery: ADS1115 ready");
    } else {
        Serial.println("Battery: ADS1115 not found");
    }
    if (gpsEnabled_) startGpsSerial(0);
    return true;
}

void Hardware::poll() {
    if (audioFault_.exchange(false)) io.write(pins::AUDIO_PA_EN, false);
    const bool toneLed = toneLedRequested_.load() || int32_t(millis() - fallbackLedUntilMs_) < 0;
    if (toneLed != toneLedApplied_) {
        io.write(pins::USER_LED, toneLed);
        toneLedApplied_ = toneLed;
    }
    if (brightnessDirty_ && millis() - brightnessChangedMs_ >= 700) {
        Preferences preferences; preferences.begin("mesh-hw", false);
        preferences.putUChar("brightness", userBrightness_); preferences.end();
        brightnessDirty_ = false;
    }
    while (gpsEnabled_ && Serial1.available()) { consumeGpsByte(char(Serial1.read())); ++gpsBytesSeen_; }
    if (gpsEnabled_ && !gpsValidSentences_ && millis() - gpsBaudStartedMs_ >= 2500) {
        // Meshtastic-style probing: a receiver may retain a baud rate selected
        // by earlier firmware. Cycle common then rare L76K rates until NMEA is seen.
        startGpsSerial(uint8_t((gpsBaudIndex_ + 1) % 5));
    } else if (gpsEnabled_ && gpsValidSentences_ && !gpsConfigured_) configureGps();
    if (gpsEnabled_) {
        static uint32_t nextGpsDiagnostic = 0;
        if (int32_t(millis() - nextGpsDiagnostic) >= 0) {
            nextGpsDiagnostic = millis() + 5000;
            uint8_t strongSatellites = 0;
            uint8_t maxSnr = 0;
            uint16_t snrTotal = 0;
            for (uint8_t i = 0; i < gpsSatelliteInfoCount_; ++i) {
                if (gpsSatelliteInfo_[i].snr > maxSnr) maxSnr = gpsSatelliteInfo_[i].snr;
                if (gpsSatelliteInfo_[i].snr >= 30) ++strongSatellites;
                snrTotal += gpsSatelliteInfo_[i].snr;
            }
            const uint8_t averageSnr = gpsSatelliteInfoCount_ ? uint8_t(snrTotal / gpsSatelliteInfoCount_) : 0;
            Serial.printf("GPS diag: fix=%u sats=%u used=%u hdop=%.1f snrMax=%u snrAvg=%u snr>=30=%u rmc=%lu gga=%lu gsv=%lu lat=%.6f lon=%.6f time=%u\n",
                          unsigned(gpsHasFix_), unsigned(gpsSatellites_), unsigned(gpsSatellitesUsed_),
                          double(gpsHdop_), unsigned(maxSnr), unsigned(averageSnr), unsigned(strongSatellites),
                          static_cast<unsigned long>(gpsRmcSentences_),
                          static_cast<unsigned long>(gpsGgaSentences_), static_cast<unsigned long>(gpsGsvSentences_),
                          gpsLatitude(), gpsLongitude(), unsigned(gpsTimestamp_));
        }
    }
    const int value = io.read(pins::WAKE_BUTTON);
    if (value < 0) return;
    const uint32_t now = millis();
    const bool pressed = value == 0;
    const bool sidePressed = digitalRead(0) == LOW;
    if (pressed != lastButton_ && now - lastButtonChange_ > 45) {
        lastButtonChange_ = now;
        lastButton_ = pressed;
        if (pressed) powerPressPending_ = true;
        else if (powerPressPending_ && !screenshotChordActive_) buttonPressed_ = true;
    }
    if (sidePressed != lastSideButton_ && now - lastSideButtonChange_ > 45) {
        lastSideButtonChange_ = now;
        lastSideButton_ = sidePressed;
        if (sidePressed) sidePressPending_ = true;
        else if (sidePressPending_ && !screenshotChordActive_) { volumePressed_ = true; sidePressPending_ = false; }
    }
    if (lastButton_ && lastSideButton_ && !screenshotChordActive_) {
        screenshotChordActive_ = true;
        powerPressPending_ = false;
        sidePressPending_ = false;
        screenshotPressed_ = true;
    }
    if (!lastButton_ && !lastSideButton_) {
        screenshotChordActive_ = false;
        powerPressPending_ = false;
    }
}

void Hardware::consumeGpsByte(char value) {
    if (value == '\r') return;
    if (value == '\n') {
        if (gpsLineLength_) { gpsLine_[gpsLineLength_] = 0; parseGpsLine(); }
        gpsLineLength_ = 0;
        return;
    }
    if (gpsLineLength_ < sizeof(gpsLine_) - 1) gpsLine_[gpsLineLength_++] = value;
    else gpsLineLength_ = 0;
}

void Hardware::parseGpsLine() {
    if (strncmp(gpsLine_, "$GNRMC", 6) != 0 && strncmp(gpsLine_, "$GPRMC", 6) != 0 &&
        strncmp(gpsLine_, "$GNGGA", 6) != 0 && strncmp(gpsLine_, "$GPGGA", 6) != 0 &&
        !(gpsLine_[0] == '$' && strlen(gpsLine_) >= 6 && !strncmp(gpsLine_ + 3, "GSV", 3))) return;
    // Reject damaged UART sentences before any position fields are accepted.
    // NMEA checksum is the XOR of bytes between '$' and '*'.
    const char *checksumMark = strrchr(gpsLine_, '*');
    if (!checksumMark || strlen(checksumMark + 1) < 2) return;
    uint8_t checksum = 0;
    for (const char *cursor = gpsLine_ + 1; cursor < checksumMark; ++cursor) checksum ^= uint8_t(*cursor);
    char expectedText[3] = {checksumMark[1], checksumMark[2], 0}; char *checksumEnd = nullptr;
    const unsigned long expected = strtoul(expectedText, &checksumEnd, 16);
    if (!checksumEnd || *checksumEnd || checksum != uint8_t(expected)) return;
    ++gpsValidSentences_;
    char copy[128]; strlcpy(copy, gpsLine_, sizeof(copy));
    char *fields[24]{}; uint8_t count = 0; fields[count++] = copy;
    for (char *cursor = copy; *cursor && count < 24; ++cursor) {
        if (*cursor == ',') { *cursor = 0; fields[count++] = cursor + 1; }
    }
    const bool rmc = strstr(fields[0], "RMC") != nullptr;
    const bool gsv = strstr(fields[0], "GSV") != nullptr;
    if (gsv && count >= 4) {
        ++gpsGsvSentences_;
        const char talker1 = fields[0][1], talker2 = fields[0][2];
        char talker = talker1 == 'G' ? talker2 : talker1;
        if ((talker1 == 'B' && talker2 == 'D') || (talker1 == 'G' && talker2 == 'B')) talker = 'B';
        else if (talker1 == 'G' && talker2 == 'A') talker = 'A';
        else if ((talker1 == 'Q' && talker2 == 'Z') || (talker1 == 'G' && talker2 == 'Q')) talker = 'Q';
        const uint32_t now = millis();
        uint8_t write = 0;
        for (uint8_t i = 0; i < gpsSatelliteInfoCount_; ++i)
            if (now - gpsSatelliteInfo_[i].lastSeenMs <= GPS_SATELLITE_AGE_MS)
                gpsSatelliteInfo_[write++] = gpsSatelliteInfo_[i];
        gpsSatelliteInfoCount_ = write;
        for (uint8_t i = write; i < MAX_GPS_SATELLITES; ++i) gpsSatelliteInfo_[i] = {};
        for (uint8_t group = 0; group < 4; ++group) {
            const uint8_t field = 4 + group * 4;
            if (field + 3 >= count || !fields[field][0]) break;
            const uint16_t id = uint16_t(atoi(fields[field]));
            if (!id) continue;
            char system = talker;
            if (system == 'N') system = id >= 65 && id <= 96 ? 'L' :
                                          (id >= 201 && id <= 237 ? 'B' : (id >= 301 && id <= 336 ? 'A' : 'P'));
            uint8_t slot = gpsSatelliteInfoCount_;
            for (uint8_t i = 0; i < gpsSatelliteInfoCount_; ++i)
                if (gpsSatelliteInfo_[i].system == system && gpsSatelliteInfo_[i].id == id) { slot = i; break; }
            if (slot >= MAX_GPS_SATELLITES) continue;
            if (slot == gpsSatelliteInfoCount_) ++gpsSatelliteInfoCount_;
            gpsSatelliteInfo_[slot].id = id;
            gpsSatelliteInfo_[slot].system = system;
            gpsSatelliteInfo_[slot].elevation = uint8_t(constrain(atoi(fields[field + 1]), 0, 90));
            gpsSatelliteInfo_[slot].azimuth = uint16_t(constrain(atoi(fields[field + 2]), 0, 359));
            gpsSatelliteInfo_[slot].snr = uint8_t(constrain(atoi(fields[field + 3]), 0, 99));
            gpsSatelliteInfo_[slot].lastSeenMs = now;
        }
        gpsSatellites_ = gpsSatelliteInfoCount_;
        return;
    }
    if (rmc && count >= 7) {
        ++gpsRmcSentences_;
        gpsHasFix_ = fields[2][0] == 'A';
        if (count >= 9) {
            gpsSpeedKph_ = float(atof(fields[7]) * 1.852);
            gpsCourseDegrees_ = float(atof(fields[8]));
        }
        // RMC UTC date/time remains useful while status is V (no position fix).
        // Do not unnecessarily block clock synchronization on a location fix.
        if (count >= 10 && strlen(fields[1]) >= 6 && strlen(fields[9]) == 6) {
            bool numericDateTime = true;
            for (uint8_t i = 0; i < 6; ++i)
                if (fields[1][i] < '0' || fields[1][i] > '9' ||
                    fields[9][i] < '0' || fields[9][i] > '9') numericDateTime = false;
            if (numericDateTime) {
                const int hour = (fields[1][0] - '0') * 10 + fields[1][1] - '0';
                const int minute = (fields[1][2] - '0') * 10 + fields[1][3] - '0';
                const int second = (fields[1][4] - '0') * 10 + fields[1][5] - '0';
                const int day = (fields[9][0] - '0') * 10 + fields[9][1] - '0';
                const int month = (fields[9][2] - '0') * 10 + fields[9][3] - '0';
                const int year = 2000 + (fields[9][4] - '0') * 10 + fields[9][5] - '0';
                static const uint8_t monthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
                int maximumDay = month >= 1 && month <= 12 ? monthDays[month - 1] : 0;
                if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) maximumDay = 29;
                if (year >= 2020 && day >= 1 && day <= maximumDay &&
                    hour <= 23 && minute <= 59 && second <= 59) {
                    gpsTimestamp_ = uint32_t(utcDaysFromCivil(year, unsigned(month), unsigned(day)) * 86400LL +
                                             hour * 3600 + minute * 60 + second);
                    gpsTimeValid_ = true;
                    gpsTimeReceivedMs_ = millis();
                }
            }
        }
        if (gpsHasFix_) {
            gpsLatitude_ = nmeaCoordinate(fields[3], fields[4]);
            gpsLongitude_ = nmeaCoordinate(fields[5], fields[6]);
            applyGpsPositionFilter();
        }
    } else if (!rmc && count >= 8) {
        ++gpsGgaSentences_;
        gpsHasFix_ = atoi(fields[6]) > 0;
        gpsSatellitesUsed_ = uint8_t(atoi(fields[7]));
        if (!gpsSatellites_) gpsSatellites_ = gpsSatellitesUsed_;
        if (count >= 11) {
            gpsHdop_ = float(atof(fields[8]));
            const float altitude = float(atof(fields[9]));
            if (gpsHasFix_ && fields[9][0] && !strcmp(fields[10], "M") && isfinite(altitude) &&
                altitude >= -500.0f && altitude <= 10000.0f) {
                gpsAltitudeMeters_ = altitude;
                gpsAltitudeValid_ = true;
            }
        }
        if (gpsHasFix_) {
            gpsLatitude_ = nmeaCoordinate(fields[2], fields[3]);
            gpsLongitude_ = nmeaCoordinate(fields[4], fields[5]);
            applyGpsPositionFilter();
        }
    }
}

void Hardware::applyGpsPositionFilter() {
    if (!gpsHasFix_) {
        gpsLastPositionFixMs_ = 0;
        return;
    }

    const uint32_t now = millis();
    const bool stale = gpsFilterInitialized_ && now - gpsLastPositionFixMs_ > 3000;
    if (!gpsFilterInitialized_ || stale) {
        gpsFilterOriginLat_ = float(gpsLatitude_);
        gpsFilterOriginLon_ = float(gpsLongitude_);
        gpsFilterX_ = 0.0f;
        gpsFilterY_ = 0.0f;
        gpsFilterPx_ = 16.0f;
        gpsFilterPy_ = 16.0f;
        gpsFilteredLatitude_ = gpsLatitude_;
        gpsFilteredLongitude_ = gpsLongitude_;
        gpsFilterInitialized_ = true;
        gpsFilterLastMs_ = now;
        gpsLastPositionFixMs_ = now;
        return;
    }

    float delta = float(now - gpsFilterLastMs_) / 1000.0f;
    if (delta < 0.2f) delta = 0.2f;
    if (delta > 3.0f) delta = 3.0f;

    const float originLatRad = gpsFilterOriginLat_ * float(PI) / 180.0f;
    const float east = (float(gpsLongitude_) - gpsFilterOriginLon_) * 111320.0f * cosf(originLatRad);
    const float north = (float(gpsLatitude_) - gpsFilterOriginLat_) * 110540.0f;
    const float dx = east - gpsFilterX_;
    const float dy = north - gpsFilterY_;

    const float speedMps = gpsSpeedKph_ > 0.0f ? gpsSpeedKph_ / 3.6f : 0.0f;
    const float jumpGate = speedMps * 3.0f + 8.0f;
    if (sqrtf(dx * dx + dy * dy) > jumpGate && gpsHdop_ > 2.0f) {
        gpsFilterLastMs_ = now;
        gpsLastPositionFixMs_ = now;
        return;
    }

    const float processNoise = speedMps > 0.8f ? 6.0f : 0.08f;
    gpsFilterPx_ += processNoise * delta;
    gpsFilterPy_ += processNoise * delta;

    const float hdop = isfinite(gpsHdop_) && gpsHdop_ > 0.0f ? gpsHdop_ : 2.0f;
    const float measurementStd = 2.0f + hdop * 1.8f;
    const float measurementVar = measurementStd * measurementStd;
    const float kx = gpsFilterPx_ / (gpsFilterPx_ + measurementVar);
    const float ky = gpsFilterPy_ / (gpsFilterPy_ + measurementVar);
    gpsFilterX_ += kx * dx;
    gpsFilterY_ += ky * dy;
    gpsFilterPx_ *= 1.0f - kx;
    gpsFilterPy_ *= 1.0f - ky;

    gpsFilteredLatitude_ = double(gpsFilterOriginLat_ + gpsFilterY_ / 110540.0f);
    gpsFilteredLongitude_ = double(gpsFilterOriginLon_ + gpsFilterX_ / (111320.0f * cosf(originLatRad)));
    gpsFilterLastMs_ = now;
    gpsLastPositionFixMs_ = now;
}

void Hardware::setGpsEnabled(bool enabled) {
    if (gpsEnabled_ == enabled) return;
    gpsEnabled_ = enabled;
    Serial1.end();
    gpsLineLength_ = 0;
    gpsBytesSeen_ = 0;
    gpsHasFix_ = false;
    gpsSatellites_ = 0;
    gpsSatellitesUsed_ = 0;
    gpsSatelliteInfoCount_ = 0;
    gpsAltitudeValid_ = false;
    gpsFilterInitialized_ = false;
    gpsFilterLastMs_ = 0;
    gpsLastPositionFixMs_ = 0;
    gpsFilterPx_ = 16.0f;
    gpsFilterPy_ = 16.0f;
    gpsValidSentences_ = 0;
    gpsGgaSentences_ = 0;
    gpsRmcSentences_ = 0;
    gpsGsvSentences_ = 0;
    gpsConfigured_ = false;
    gpsConfigStep_ = 0;
    gpsConfigNextMs_ = 0;
    gpsTimeValid_ = false;
    gpsTimeReceivedMs_ = 0;
    if (enabled) {
        io.write(pins::GNSS_EN, true);
        io.write(pins::GNSS_RESET, true);
        delay(10);
        io.write(pins::GNSS_RESET, false);
        delay(300);
        startGpsSerial(0);
    } else {
        io.write(pins::GNSS_RESET, true);
        io.write(pins::GNSS_EN, false);
    }
    Preferences preferences;
    preferences.begin("mesh-hw", false); preferences.putBool("gps", enabled); preferences.end();
    Serial.printf("GPS: %s\n", enabled ? "enabled" : "disabled");
}

void Hardware::startGpsSerial(uint8_t baudIndex) {
    static const uint32_t baudRates[] = {9600, 115200, 38400, 4800, 57600};
    gpsBaudIndex_ = baudIndex < 5 ? baudIndex : 0;
    Serial1.end();
    Serial1.setRxBufferSize(4096);
    Serial1.begin(baudRates[gpsBaudIndex_], SERIAL_8N1, pins::GPS_RX, pins::GPS_TX);
    gpsBaudStartedMs_ = millis();
    Serial.printf("GPS: probing %lu baud\n", static_cast<unsigned long>(baudRates[gpsBaudIndex_]));
}

void Hardware::configureGps() {
    if (int32_t(millis() - gpsConfigNextMs_) < 0) return;
    // Space configuration commands without blocking the UART receive loop.
    // Keep the multi-constellation 1 Hz fix and decimated GSV output so RMC
    // and GGA stay available while the receiver is acquiring satellites.
    switch (gpsConfigStep_++) {
        case 0: writeGpsConfig("PCAS04,7"); break;
        case 1: writeGpsConfig("PCAS02,1000"); break;
        case 2: writeGpsConfig("PCAS03,1,0,0,3,1,0,0,0,0,0,,,0,0"); break;
        case 3: writeGpsConfig("PCAS11,3"); break;
        default:
            writeCasNavxCommand();
            gpsConfigured_ = true;
            Serial.printf("GPS: L76K configuration sent after %lu valid NMEA sentences\n",
                          static_cast<unsigned long>(gpsValidSentences_));
            return;
    }
    gpsConfigNextMs_ = millis() + GPS_CONFIG_SETTLE_MS;
}

void Hardware::writeCasNavxCommand() {
    uint8_t packet[54] = {};
    packet[0] = 0xBA;
    packet[1] = 0xCE;
    packet[2] = 44;
    packet[4] = 0x06;
    packet[5] = 0x07;

    const uint32_t mask = (1U << 6) | (1U << 13);
    packet[6] = mask & 0xFF;
    packet[7] = (mask >> 8) & 0xFF;
    packet[8] = (mask >> 16) & 0xFF;
    packet[9] = (mask >> 24) & 0xFF;
    packet[17] = 0;
    const float staticHold = 0.3f;
    memcpy(packet + 46, &staticHold, sizeof(staticHold));

    uint32_t checksum = (uint32_t(packet[5]) << 24) | (uint32_t(packet[4]) << 16) | packet[2];
    for (uint8_t i = 0; i < 11; ++i) {
        uint32_t word = 0;
        memcpy(&word, packet + 6 + i * 4, sizeof(word));
        checksum += word;
    }
    packet[50] = checksum & 0xFF;
    packet[51] = (checksum >> 8) & 0xFF;
    packet[52] = (checksum >> 16) & 0xFF;
    packet[53] = (checksum >> 24) & 0xFF;
    Serial1.write(packet, sizeof(packet));
}

bool Hardware::consumeButtonPress() {
    const bool pressed = buttonPressed_;
    buttonPressed_ = false;
    return pressed;
}

bool Hardware::consumeScreenshotPress() {
    const bool pressed = screenshotPressed_;
    screenshotPressed_ = false;
    return pressed;
}

bool Hardware::consumeVolumePress() {
    const bool pressed = volumePressed_;
    volumePressed_ = false;
    return pressed;
}

void Hardware::setVolume(uint8_t value) {
    volume_ = constrain(value, 0, 100);
    if (audioReady_) codecBoard.setVolume(volume_);
    Preferences preferences;
    preferences.begin("mesh-hw", false); preferences.putUChar("volume", volume_); preferences.end();
}

void Hardware::setBrightness(uint8_t value) {
    userBrightness_ = value;
    if (displayAwake_) display.backlight().setBrightness(value);
    brightnessDirty_ = true; brightnessChangedMs_ = millis();
}

void Hardware::setGroveEnabled(bool enabled) {
    if (groveEnabled_ == enabled) return;
    if (io.write(pins::GROVE_EN, enabled)) groveEnabled_ = enabled;
}

void Hardware::recoverI2cBus() {
    Wire.end();
    pinMode(pins::I2C_SDA, OUTPUT_OPEN_DRAIN);
    pinMode(pins::I2C_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(pins::I2C_SDA, HIGH);
    digitalWrite(pins::I2C_SCL, HIGH);
    delayMicroseconds(5);
    for (uint8_t pulse = 0; pulse < 9; ++pulse) {
        digitalWrite(pins::I2C_SCL, LOW); delayMicroseconds(5);
        digitalWrite(pins::I2C_SCL, HIGH); delayMicroseconds(5);
    }
    // STOP: SDA low while SCL high, then release SDA high.
    digitalWrite(pins::I2C_SDA, LOW); delayMicroseconds(5);
    digitalWrite(pins::I2C_SCL, HIGH); delayMicroseconds(5);
    digitalWrite(pins::I2C_SDA, HIGH); delayMicroseconds(5);
    Wire.begin(pins::I2C_SDA, pins::I2C_SCL);
    Wire.setClock(100000);
    Wire.setTimeOut(5);
}

void Hardware::toggleDisplayPower() {
    displayAwake_ = !displayAwake_;
    display.backlight().setBrightness(displayAwake_ ? userBrightness_ : 0);
    Serial.printf("Display: %s\n", displayAwake_ ? "awake" : "sleep");
}

bool Hardware::beginAudio() {
    codecPins.addI2C(PinFunction::CODEC, Wire);
    codecPins.addI2S(PinFunction::CODEC, pins::AUDIO_MCLK, pins::AUDIO_BCLK,
                     pins::AUDIO_LRCLK, pins::AUDIO_DOUT, -1);
    CodecConfig codecConfig;
    codecConfig.input_device = ADC_INPUT_LINE1;
    codecConfig.output_device = DAC_OUTPUT_ALL;
    codecConfig.i2s.bits = BIT_LENGTH_16BITS;
    codecConfig.i2s.rate = RATE_44K;
    if (!codecBoard.begin(codecConfig)) return false;
    codecBoard.setVolume(volume_);

    i2s_config_t config{};
    config.mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = 44100;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.dma_buf_count = 4;
    config.dma_buf_len = 256;
    config.use_apll = true;
    config.tx_desc_auto_clear = true;
    config.fixed_mclk = 11289600;
    if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pinConfig{};
    pinConfig.mck_io_num = pins::AUDIO_MCLK;
    pinConfig.bck_io_num = pins::AUDIO_BCLK;
    pinConfig.ws_io_num = pins::AUDIO_LRCLK;
    pinConfig.data_out_num = pins::AUDIO_DOUT;
    pinConfig.data_in_num = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(I2S_NUM_0, &pinConfig) != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    delay(12);
    return true;
}

void Hardware::playNotificationTone() {
    queueTone(880, 90, 7200, true);
}

void Hardware::audioTask(void *argument) {
    auto *self = static_cast<Hardware *>(argument);
    ToneRequest request{};
    while (true) {
        if (xQueueReceive(self->toneQueue_, &request, portMAX_DELAY) != pdTRUE) continue;
        if (request.led) self->toneLedRequested_ = true;
        self->playTone(request.frequency, request.durationMs, request.amplitude);
        if (request.led) self->toneLedRequested_ = false;
    }
}

void Hardware::queueTone(uint16_t frequency, uint16_t durationMs, uint16_t amplitude, bool led) {
    if (!audioReady_) {
        if (led) fallbackLedUntilMs_ = millis() + durationMs;
        return;
    }
    if (!toneQueue_) {
        if (led) io.write(pins::USER_LED, true);
        playTone(frequency, durationMs, amplitude);
        if (led) io.write(pins::USER_LED, false);
        return;
    }
    const ToneRequest request{frequency, durationMs, amplitude, led};
    if (xQueueSend(toneQueue_, &request, 0) != pdTRUE && led)
        fallbackLedUntilMs_ = millis() + durationMs;
}

void Hardware::playTone(uint16_t frequency, uint16_t durationMs, uint16_t amplitude) {
    if (!audioReady_) return;
    constexpr uint32_t sampleRate = 44100;
    const size_t frameCount = size_t(sampleRate) * durationMs / 1000U;
    const size_t attackFrames = min<size_t>(220, frameCount / 3);
    const size_t releaseFrames = min<size_t>(440, frameCount / 2);
    int16_t samples[256 * 2];
    memset(samples, 0, sizeof(samples));
    size_t written = 0;
    auto writeAudio = [&](const void *data, size_t bytes) {
        written = 0;
        return i2s_write(I2S_NUM_0, data, bytes, &written, pdMS_TO_TICKS(40)) == ESP_OK && written == bytes;
    };
    // Prime the complete DMA ring with silence before connecting the amplifier.
    // This avoids exposing stale/startup samples as an audible burst.
    for (uint8_t i = 0; i < 4; ++i) if (!writeAudio(samples, sizeof(samples))) {
        Serial.println("Audio: DMA write timeout; disabling tones");
        audioReady_ = false; audioFault_ = true; return;
    }
    size_t produced = 0;
    while (produced < frameCount) {
        const size_t frames = min<size_t>(256, frameCount - produced);
        for (size_t i = 0; i < frames; ++i) {
            const size_t position = produced + i;
            const float envelope = attackFrames && position < attackFrames ? float(position) / float(attackFrames) :
                (releaseFrames && frameCount - position < releaseFrames ? float(frameCount - position) / float(releaseFrames) : 1.0f);
            const float phase = 2.0f * PI * float(frequency) * float(position) / float(sampleRate);
            const int16_t value = int16_t(sinf(phase) * float(amplitude) * envelope);
            samples[i * 2] = value;
            samples[i * 2 + 1] = value;
        }
        if (!writeAudio(samples, frames * 2 * sizeof(int16_t))) {
            Serial.println("Audio: DMA write timeout; disabling tones");
            audioReady_ = false; audioFault_ = true; return;
        }
        produced += frames;
    }
    // Queue silence behind the tone. The blocking writes let the queued audio
    // reach the DAC before the amplifier is muted, preserving the full tail.
    memset(samples, 0, sizeof(samples));
    for (uint8_t i = 0; i < 4; ++i) if (!writeAudio(samples, sizeof(samples))) break;
}

void Hardware::playMessageTone(bool directMessage, uint8_t style) {
    switch (style) {
        case 1: // Rising chime.
            queueTone(880, 70, 6000, true); queueTone(1320, 110, 6000, true); break;
        case 2: // Two separated notes; zero frequency queues silence.
            queueTone(1040, 65, 6500, true); queueTone(0, 55, 0);
            queueTone(1040, 65, 6500, true); break;
        case 3: queueTone(540, 130, 3500, true); break;
        case 4: break; // Silent.
        default: queueTone(directMessage ? 1040 : 760, directMessage ? 105 : 85, 7000, true); break;
    }
}

void Hardware::playClickTone() {
    queueTone(1450, 14, 2300);
}

void Hardware::playGameTone(uint16_t frequency, uint16_t durationMs, uint16_t amplitude) {
    queueTone(frequency, durationMs, amplitude);
}

void Hardware::notify() {
    playNotificationTone();
}

uint16_t Hardware::batteryMillivolts() {
    const uint32_t now = millis();
    if (cachedBatteryMv_ && now - lastBatteryRead_ < 3000) return cachedBatteryMv_;
    if (!adsReady_) {
        adsReady_ = ads.begin(0x48, &Wire);
        if (!adsReady_) return 0;
        ads.setGain(GAIN_ONE);
    }

    uint32_t total = 0;
    uint8_t valid = 0;
    for (uint8_t attempt = 0; attempt < 5 && valid < 3; ++attempt) {
        const int16_t raw = ads.readADC_SingleEnded(0);
        const uint16_t mv = raw > 200 ? uint16_t(ads.computeVolts(raw) * 2000.0f) : 0;
        if (mv >= 2500 && mv <= 4500) {
            total += mv;
            ++valid;
        }
        delay(3);
    }
    lastBatteryRead_ = now;
    if (valid) cachedBatteryMv_ = total / valid;
    return cachedBatteryMv_;
}

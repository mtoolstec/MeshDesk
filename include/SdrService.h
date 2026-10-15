#pragma once

#include <Arduino.h>

// Receive-only spectrum snapshots from the ESP32-S3's 2.4 GHz PHY.
// The Wi-Fi PHY is owned exclusively while this service is active.
class SdrService {
public:
    static constexpr uint8_t BIN_COUNT = 64;
    bool start(uint8_t channel);
    bool tune(uint8_t channel);
    bool spectrum(uint8_t output[BIN_COUNT]);
    void stop();
    bool active() const { return active_; }
    uint8_t channel() const { return channel_; }
    const char *error() const { return error_; }

private:
    bool capture();
    bool active_ = false;
    uint8_t channel_ = 1;
    const char *error_ = "";
};

extern SdrService sdrService;

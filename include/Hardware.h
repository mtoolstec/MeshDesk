#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <freertos/queue.h>
#include <atomic>
#include "BoardPins.h"

class IoExpander {
public:
    bool begin();
    bool pinMode(uint8_t pin, bool output);
    bool write(uint8_t pin, bool high);
    int read(uint8_t pin);
private:
    uint16_t output_ = 0xFFFF;
    uint16_t direction_ = 0xFFFF;
    bool writePair(uint8_t reg, uint16_t value);
    bool readPair(uint8_t reg, uint16_t &value);
};

class WioBacklight : public lgfx::v1::ILight {
public:
    bool init(uint8_t brightness) override;
    void setBrightness(uint8_t brightness) override;
    uint8_t brightness() const { return brightness_; }
private:
    uint8_t brightness_ = 180;
    void writeRegister(uint8_t reg, uint8_t value);
};

class WioDisplay : public lgfx::LGFX_Device {
public:
    WioDisplay();
    WioBacklight &backlight() { return light_; }
    bool init_impl(bool useReset, bool useClear) override;
private:
    lgfx::Panel_NV3031B panel_;
    lgfx::Bus_SPI bus_;
    lgfx::Touch_GT911 touch_;
    WioBacklight light_;
};

class Hardware {
public:
    bool begin();
    void poll();
    void setBrightness(uint8_t value);
    void toggleDisplayPower();
    bool displayAwake() const { return displayAwake_; }
    void notify();
    void playMessageTone(bool directMessage, uint8_t style = 0);
    void playClickTone();
    void playGameTone(uint16_t frequency, uint16_t durationMs, uint16_t amplitude = 5200);
    uint16_t batteryMillivolts();
    void setGpsEnabled(bool enabled);
    bool gpsEnabled() const { return gpsEnabled_; }
    uint32_t gpsBytesSeen() const { return gpsBytesSeen_; }
    bool gpsHasFix() const { return gpsHasFix_; }
    double gpsLatitude() const { return gpsFilterInitialized_ ? gpsFilteredLatitude_ : gpsLatitude_; }
    double gpsLongitude() const { return gpsFilterInitialized_ ? gpsFilteredLongitude_ : gpsLongitude_; }
    uint8_t gpsSatellites() const { return gpsSatellites_; }
    uint8_t gpsSatellitesUsed() const { return gpsSatellitesUsed_; }
    float gpsAltitudeMeters() const { return gpsAltitudeMeters_; }
    bool gpsAltitudeValid() const { return gpsAltitudeValid_; }
    float gpsSpeedKph() const { return gpsSpeedKph_; }
    float gpsCourseDegrees() const { return gpsCourseDegrees_; }
    float gpsHdop() const { return gpsHdop_; }
    static constexpr uint8_t MAX_GPS_SATELLITES = 32;
    struct GpsSatellite {
        uint16_t id = 0; uint16_t azimuth = 0; uint8_t elevation = 0; uint8_t snr = 0;
        char system = 0; uint32_t lastSeenMs = 0;
    };
    uint8_t gpsSatelliteInfoCount() const { return gpsSatelliteInfoCount_; }
    const GpsSatellite &gpsSatelliteInfo(uint8_t index) const { return gpsSatelliteInfo_[index < MAX_GPS_SATELLITES ? index : 0]; }
    bool gpsTimeValid() const { return gpsTimeValid_ && millis() - gpsTimeReceivedMs_ < 2500; }
    uint32_t gpsTimestamp() const { return gpsTimestamp_; }
    bool consumeButtonPress();
    bool consumeScreenshotPress();
    bool consumeVolumePress();
    uint8_t volume() const { return volume_; }
    void setVolume(uint8_t value);
    void setGroveEnabled(bool enabled);
    void recoverI2cBus();
    bool groveEnabled() const { return groveEnabled_; }

    IoExpander io;
    WioDisplay display;
private:
    bool lastButton_ = false;
    bool lastSideButton_ = false;
    bool buttonPressed_ = false;
    bool screenshotPressed_ = false;
    bool volumePressed_ = false;
    bool powerPressPending_ = false;
    bool sidePressPending_ = false;
    bool screenshotChordActive_ = false;
    bool adsReady_ = false;
    std::atomic<bool> audioReady_{false};
    struct ToneRequest {
        uint16_t frequency;
        uint16_t durationMs;
        uint16_t amplitude;
        bool led;
    };
    QueueHandle_t toneQueue_ = nullptr;
    std::atomic<bool> toneLedRequested_{false};
    bool toneLedApplied_ = false;
    uint32_t fallbackLedUntilMs_ = 0;
    std::atomic<bool> audioFault_{false};
    uint8_t volume_ = 62;
    bool displayAwake_ = true;
    uint8_t userBrightness_ = 180;
    bool brightnessDirty_ = false;
    uint32_t brightnessChangedMs_ = 0;
    uint16_t cachedBatteryMv_ = 0;
    uint32_t lastButtonChange_ = 0;
    uint32_t lastSideButtonChange_ = 0;
    uint32_t lastBatteryRead_ = 0;
    bool gpsEnabled_ = false;
    uint32_t gpsBytesSeen_ = 0;
    bool gpsHasFix_ = false;
    double gpsLatitude_ = 0;
    double gpsLongitude_ = 0;
    double gpsFilteredLatitude_ = 0;
    double gpsFilteredLongitude_ = 0;
    float gpsFilterX_ = 0;
    float gpsFilterY_ = 0;
    float gpsFilterPx_ = 4.0f;
    float gpsFilterPy_ = 4.0f;
    float gpsFilterOriginLat_ = 0;
    float gpsFilterOriginLon_ = 0;
    uint32_t gpsFilterLastMs_ = 0;
    uint32_t gpsLastPositionFixMs_ = 0;
    bool gpsFilterInitialized_ = false;
    uint8_t gpsSatellites_ = 0;
    uint8_t gpsSatellitesUsed_ = 0;
    float gpsAltitudeMeters_ = 0;
    bool gpsAltitudeValid_ = false;
    float gpsSpeedKph_ = 0;
    float gpsCourseDegrees_ = 0;
    float gpsHdop_ = 0;
    GpsSatellite gpsSatelliteInfo_[MAX_GPS_SATELLITES]{};
    uint8_t gpsSatelliteInfoCount_ = 0;
    uint32_t gpsValidSentences_ = 0;
    uint32_t gpsGgaSentences_ = 0;
    uint32_t gpsRmcSentences_ = 0;
    uint32_t gpsGsvSentences_ = 0;
    uint32_t gpsBaudStartedMs_ = 0;
    uint8_t gpsBaudIndex_ = 0;
    bool gpsConfigured_ = false;
    uint8_t gpsConfigStep_ = 0;
    uint32_t gpsConfigNextMs_ = 0;
    bool groveEnabled_ = false;
    bool gpsTimeValid_ = false;
    uint32_t gpsTimestamp_ = 0;
    uint32_t gpsTimeReceivedMs_ = 0;
    char gpsLine_[128]{};
    uint8_t gpsLineLength_ = 0;
    void consumeGpsByte(char value);
    void parseGpsLine();
    void applyGpsPositionFilter();
    void startGpsSerial(uint8_t baudIndex);
    void configureGps();
    void writeCasNavxCommand();
    bool beginAudio();
    void queueTone(uint16_t frequency, uint16_t durationMs, uint16_t amplitude, bool led = false);
    static void audioTask(void *argument);
    void playNotificationTone();
    void playTone(uint16_t frequency, uint16_t durationMs, uint16_t amplitude);
};

extern Hardware hardware;

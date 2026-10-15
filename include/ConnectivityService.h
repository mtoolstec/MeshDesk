#pragma once

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include <WebServer.h>

enum class StorageJobState : uint8_t {
    Idle,
    Downloading,
    Formatting,
    Complete,
    Failed
};

class ConnectivityService {
public:
    void begin();
    void loop();
    void connectWifi(const char *ssid, const char *password);
    void disconnectWifi();
    void setWifiEnabled(bool enabled);
    bool wifiEnabled() const { return wifiEnabled_; }
    bool wifiConnected() const;
    void suspendWifiForSdr();
    void resumeWifiAfterSdr();
    bool takeNetworkTime(uint32_t &timestamp);
    const char *savedSsid() const { return ssid_; }
    String wifiAddress() const;
    int32_t wifiRssi() const;
    void setHttpProxy(const char *host, uint16_t port);
    const char *proxyHost() const { return proxyHost_; }
    uint16_t proxyPort() const { return proxyPort_; }
    bool proxyEnabled() const { return proxyHost_[0] && proxyPort_; }
    bool startWifiScan();
    int16_t wifiScanComplete() const;
    String wifiScanSsid(int16_t index) const;
    int32_t wifiScanRssi(int16_t index) const;
    void clearWifiScan();
    bool startFtp();
    void stopFtp();
    bool ftpRunning() const { return ftpRunning_; }
    String ftpAddress() const;
    const char *ftpUsername() const { return ftpUsername_; }
    const char *ftpPassword() const { return ftpPassword_; }
    void setFtpCredentials(const char *username, const char *password);

    bool sdMounted() const { return sdMounted_; }
    uint64_t sdTotalBytes() const { return sdTotalBytes_; }
    uint64_t sdUsedBytes() const { return sdUsedBytes_; }

    bool startMapDownload(double latitude, double longitude, uint8_t zoom);
    bool startIpLocation();
    enum class IpLocationState : uint8_t { Idle, Loading, Ready, Failed };
    IpLocationState ipLocationState() const { return ipLocationState_; }
    double ipLatitude() const { return ipLatitude_; }
    double ipLongitude() const { return ipLongitude_; }
    bool startFormatSd();
    StorageJobState jobState() const { return jobState_; }
    uint8_t jobProgress() const { return jobProgress_; }
    uint8_t jobDone() const { return jobDone_; }
    uint8_t jobTotal() const { return jobTotal_; }
    const char *jobError() const { return jobError_; }
    void acknowledgeJob();

private:
    enum class JobType : uint8_t { None, Download, Format };
    char ssid_[33]{};
    char password_[65]{};
    char proxyHost_[64]{};
    uint16_t proxyPort_ = 0;
    volatile bool sdMounted_ = false;
    volatile uint64_t sdTotalBytes_ = 0;
    volatile uint64_t sdUsedBytes_ = 0;
    volatile StorageJobState jobState_ = StorageJobState::Idle;
    volatile uint8_t jobProgress_ = 0;
    volatile uint8_t jobDone_ = 0;
    volatile uint8_t jobTotal_ = 0;
    char jobError_[64]{};
    JobType pendingJob_ = JobType::None;
    double pendingLatitude_ = 0;
    double pendingLongitude_ = 0;
    uint8_t pendingZoom_ = 13;
    volatile IpLocationState ipLocationState_ = IpLocationState::Idle;
    double ipLatitude_ = 0;
    double ipLongitude_ = 0;
    static void ipLocationTask(void *context);
    uint32_t lastReconnectAttempt_ = 0;
    bool networkTimeStarted_ = false;
    bool wifiSuspendedForSdr_ = false;
    uint32_t lastNetworkSyncSeen_ = 0;
    bool wifiEnabled_ = false;
    WebServer webServer_{80};
    bool ftpRunning_ = false;
    File webUploadFile_;
    String webUploadPath_;
    bool webUploadOk_ = false;
    bool webConfigured_ = false;
    bool webRequested_ = true;
    char ftpUsername_[33] = "guest";
    char ftpPassword_[33]{};

    bool mountSd(bool formatOnFailure = false);
    void serviceSerialInstaller();
    void serviceFtp();
    String webPath(const String &relative) const;
    void configureWebManager();
    void handleWebUpload();
    bool installPocketZip(const String &archivePath, const String &fileName); // Legacy; browser installs ZIP entries.
    bool downloadTile(uint8_t zoom, uint32_t x, uint32_t y);
    bool downloadTileThroughProxy(const String &path, const String &uri);
    void runDownload();
    void runFormat();
    void finishJob(bool success, const char *error = nullptr);
    static void jobTask(void *context);
};

extern ConnectivityService connectivity;

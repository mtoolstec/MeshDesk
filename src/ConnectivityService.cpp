#include <Wire.h>
#include "DisplaySnapshot.h"
#include "ConnectivityService.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <ff.h>
#include <diskio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ssl.h>
#include <mbedtls/platform.h>
#include <lgfx/utility/lgfx_miniz.h>
#include <img_converters.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <atomic>
#include <time.h>

#include "Hardware.h"
#include "CommunicationService.h"
#include "MeshtasticService.h"
#include "ChatStore.h"
#include <ArduinoJson.h>
#include "SystemClock.h"
#include "AppUI.h"
#include "WebFilesPage.h"

extern bool queueWebTap(uint16_t x, uint16_t y);
extern bool queueWebSwipe(uint16_t x, uint16_t y, uint16_t endX, uint16_t endY);

ConnectivityService connectivity;

namespace {
std::atomic<uint32_t> networkSyncCount{0};

void *tlsCalloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return nullptr;
    void *memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? memory : heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
}

void tlsFree(void *memory) { heap_caps_free(memory); }

void onNetworkTimeSync(struct timeval *) {
    networkSyncCount.fetch_add(1, std::memory_order_relaxed);
}
constexpr int SD_CLK = 2;
constexpr int SD_CMD = 3;
constexpr int SD_D0 = 1;
constexpr size_t SERIAL_APP_MAX_BYTES = 2 * 1024 * 1024;
// USB diagnostics run on loopTask; keep sector buffers out of its small stack.
uint8_t serialSdBytes[1024]{};
File serialUploadFile;
char serialUploadTarget[128]{};
char serialUploadTemp[128]{};
size_t serialUploadRemaining = 0;
size_t serialUploadTotal = 0;
uint32_t serialUploadLastByte = 0;
char serialCommand[192]{};
size_t serialCommandLength = 0;

struct JpegBuffer {
    uint8_t *data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
};

size_t jpegBufferWriter(void *argument, size_t index, const void *data, size_t length) {
    auto *output = static_cast<JpegBuffer *>(argument);
    if (!output || index > SIZE_MAX - length) return 0;
    const size_t required = index + length;
    if (required > output->capacity) {
        size_t capacity = output->capacity ? output->capacity : 32 * 1024;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) { capacity = required; break; }
            capacity *= 2;
        }
        uint8_t *expanded = static_cast<uint8_t *>(heap_caps_realloc(
            output->data, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!expanded) return 0;
        output->data = expanded;
        output->capacity = capacity;
    }
    memcpy(output->data + index, data, length);
    output->size = max(output->size, required);
    return length;
}

bool captureScreenJpeg(JpegBuffer &output) {
    constexpr uint32_t width = 320;
    constexpr uint32_t height = 240;
    lv_refr_now(nullptr);
    lv_draw_buf_t *snapshot = takeDisplaySnapshot();
    if (!snapshot) return false;
    uint8_t *rgb = static_cast<uint8_t *>(heap_caps_malloc(
        width * height * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!rgb) { lv_draw_buf_destroy(snapshot); return false; }
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *source = snapshot->data + y * snapshot->header.stride;
        for (uint32_t x = 0; x < width; ++x) {
            uint16_t pixel;
            memcpy(&pixel, source + x * 2, sizeof(pixel));
            rgb[(y * width + x) * 3] = uint8_t(((pixel >> 11) & 0x1F) * 255 / 31);
            rgb[(y * width + x) * 3 + 1] = uint8_t(((pixel >> 5) & 0x3F) * 255 / 63);
            rgb[(y * width + x) * 3 + 2] = uint8_t((pixel & 0x1F) * 255 / 31);
        }
    }
    const bool encoded = fmt2jpg_cb(rgb, width * height * 3, width, height, PIXFORMAT_RGB888,
                                    90, jpegBufferWriter, &output);
    heap_caps_free(rgb);
    lv_draw_buf_destroy(snapshot);
    return encoded && output.data && output.size;
}

bool ensureDirectory(const String &path) {
    if (SD_MMC.exists(path)) return true;
    return SD_MMC.mkdir(path);
}

bool ensureDirectoryTree(const String &path) {
    String current;
    for (size_t i = 1; i < path.length(); ++i) if (path[i] == '/') {
        current = path.substring(0, i);
        if (current.length() && !SD_MMC.exists(current) && !SD_MMC.mkdir(current)) return false;
    }
    return SD_MMC.exists(path) || SD_MMC.mkdir(path);
}

bool removeTree(const String &path) {
    File entry = SD_MMC.open(path);
    if (!entry) return false;
    if (!entry.isDirectory()) { entry.close(); return SD_MMC.remove(path); }
    File child;
    while ((child = entry.openNextFile())) {
        String childPath = child.path(); child.close();
        if (!removeTree(childPath)) { entry.close(); return false; }
    }
    entry.close(); return SD_MMC.rmdir(path);
}

String jsonEscape(const char *value) {
    String out;
    for (const char *p = value; p && *p; ++p) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
        else if (*p == '\n') out += "\\n";
        else if (uint8_t(*p) >= 0x20) out += *p;
    }
    return out;
}

uint16_t zip16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t zip32(const uint8_t *p) { return uint32_t(zip16(p)) | uint32_t(zip16(p + 2)) << 16; }

struct ProxyTlsContext {
    WiFiClient socket;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context random;
    mbedtls_entropy_context entropy;
};

int proxyTlsSend(void *context, const unsigned char *data, size_t length) {
    auto *client = static_cast<WiFiClient *>(context);
    const size_t written = client->write(data, length);
    return written ? int(written) : MBEDTLS_ERR_SSL_WANT_WRITE;
}

int proxyTlsReceive(void *context, unsigned char *data, size_t length) {
    auto *client = static_cast<WiFiClient *>(context);
    const int available = client->available();
    if (available <= 0) return client->connected() ? MBEDTLS_ERR_SSL_WANT_READ : 0;
    return client->read(data, min(length, size_t(available)));
}

int proxyTlsRead(ProxyTlsContext &context, uint8_t *data, size_t length, uint32_t timeoutMs) {
    const uint32_t started = millis();
    while (millis() - started < timeoutMs) {
        const int result = mbedtls_ssl_read(&context.ssl, data, length);
        if (result > 0 || result == 0) return result;
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) return result;
        delay(2);
    }
    return -1;
}

bool proxyTlsLine(ProxyTlsContext &context, String &line, uint32_t timeoutMs = 15000) {
    line = "";
    while (line.length() < 96) {
        uint8_t value = 0;
        if (proxyTlsRead(context, &value, 1, timeoutMs) != 1) return false;
        if (value == '\n') return true;
        if (value != '\r') line += char(value);
    }
    return false;
}
}

void ConnectivityService::begin() {
    // TLS records and certificates exceed the free internal heap while LVGL is active.
    // Keep those allocations in PSRAM, leaving internal RAM for Wi-Fi and crypto DMA.
    if (psramFound()) mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
    Preferences preferences;
    preferences.begin("mesh-net", false);
    if (preferences.isKey("ssid")) strlcpy(ssid_, preferences.getString("ssid").c_str(), sizeof(ssid_));
    if (preferences.isKey("pass")) strlcpy(password_, preferences.getString("pass").c_str(), sizeof(password_));
    wifiEnabled_ = preferences.getBool("wifi-on", ssid_[0]);
    if (preferences.isKey("proxy-host")) strlcpy(proxyHost_, preferences.getString("proxy-host").c_str(), sizeof(proxyHost_));
    if (preferences.isKey("ftp-user")) strlcpy(ftpUsername_, preferences.getString("ftp-user", "guest").c_str(), sizeof(ftpUsername_));
    if (preferences.isKey("ftp-pass")) strlcpy(ftpPassword_, preferences.getString("ftp-pass").c_str(), sizeof(ftpPassword_));
    webRequested_ = preferences.getBool("web-files", true);
    proxyPort_ = preferences.getUShort("proxy-port", 0);
    preferences.end();

    hardware.io.write(pins::SD_POWER, true);
    delay(20);
    for (uint8_t attempt = 0; attempt < 3 && !sdMounted_; ++attempt) {
        if (attempt) {
            hardware.io.write(pins::SD_POWER, false);
            delay(120);
            hardware.io.write(pins::SD_POWER, true);
            delay(120);
        }
        mountSd(false);
    }

    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    if (wifiEnabled_ && ssid_[0]) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid_, password_);
        lastReconnectAttempt_ = millis();
    } else {
        WiFi.mode(WIFI_OFF);
    }
}

void ConnectivityService::loop() {
    serviceSerialInstaller();
    if (wifiSuspendedForSdr_) return;
    if (webRequested_ && !ftpRunning_ && wifiConnected() && jobState_ != StorageJobState::Formatting) startFtp();
    serviceFtp();
    if (wifiConnected() && !networkTimeStarted_) {
        // SNTP runs in lwIP; keep UTC in the clock and apply the UI's saved
        // timezone only when formatting the displayed time.
        sntp_set_time_sync_notification_cb(onNetworkTimeSync);
        configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        networkTimeStarted_ = true;
        Serial.println("Time: network synchronization started");
    }
    if (!wifiEnabled_ || !ssid_[0] || WiFi.status() == WL_CONNECTED) return;
    const uint32_t now = millis();
    if (now - lastReconnectAttempt_ >= 15000) {
        lastReconnectAttempt_ = now;
        WiFi.disconnect();
        WiFi.begin(ssid_, password_);
    }
}

void ConnectivityService::suspendWifiForSdr() {
    if (wifiSuspendedForSdr_) return;
    wifiSuspendedForSdr_ = true;
    if (ftpRunning_) { webServer_.stop(); ftpRunning_ = false; }
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    Serial.println("Network: Wi-Fi released for SDR reception");
}

void ConnectivityService::resumeWifiAfterSdr() {
    if (!wifiSuspendedForSdr_) return;
    wifiSuspendedForSdr_ = false;
    if (wifiEnabled_ && ssid_[0]) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid_, password_);
        lastReconnectAttempt_ = millis();
    }
}

bool ConnectivityService::takeNetworkTime(uint32_t &timestamp) {
    const uint32_t count = networkSyncCount.load(std::memory_order_relaxed);
    if (!count || count == lastNetworkSyncSeen_) return false;
    lastNetworkSyncSeen_ = count;
    const time_t now = time(nullptr);
    if (now < 1700000000 || now > 4102444799LL) return false;
    timestamp = uint32_t(now);
    return true;
}

bool ConnectivityService::startFtp() {
    if (!wifiConnected()) return false;
    if (sdMounted_ && !SD_MMC.exists("/apps")) SD_MMC.mkdir("/apps");
    configureWebManager(); webServer_.begin(); ftpRunning_ = true; webRequested_ = true;
    Preferences preferences; preferences.begin("mesh-net", false); preferences.putBool("web-files", true); preferences.end();
    Serial.printf("Web files: started at http://%s/ root=/apps\n", WiFi.localIP().toString().c_str());
    return true;
}

void ConnectivityService::stopFtp() {
    if (webUploadFile_) webUploadFile_.close();
    webServer_.stop(); ftpRunning_ = false; webRequested_ = false;
    Preferences preferences; preferences.begin("mesh-net", false); preferences.putBool("web-files", false); preferences.end();
    Serial.println("Web files: stopped");
}

String ConnectivityService::ftpAddress() const {
    return ftpRunning_ && wifiConnected() ? String("http://") + WiFi.localIP().toString() + "/" : String();
}

void ConnectivityService::setFtpCredentials(const char *username, const char *password) {
    strlcpy(ftpUsername_, username && username[0] ? username : "guest", sizeof(ftpUsername_));
    strlcpy(ftpPassword_, password ? password : "", sizeof(ftpPassword_));
    Preferences preferences; preferences.begin("mesh-net", false);
    preferences.putString("ftp-user", ftpUsername_); preferences.putString("ftp-pass", ftpPassword_); preferences.end();
}

String ConnectivityService::webPath(const String &argument) const {
    String relative = argument; relative.trim();
    if (!relative.length()) relative = "/";
    if (relative[0] != '/') relative = "/" + relative;
    while (relative.indexOf("//") >= 0) relative.replace("//", "/");
    if (relative.indexOf("..") >= 0) return String();
    // Web Files exposes the SD card root. App installation uses explicit paths.
    while (relative.length() > 1 && relative.endsWith("/")) relative.remove(relative.length() - 1);
    return relative;
}

void ConnectivityService::serviceFtp() {
    if (!ftpRunning_) return;
    if (!wifiConnected()) { webServer_.stop(); ftpRunning_ = false; return; }
    webServer_.handleClient();
}

void ConnectivityService::configureWebManager() {
    if (webConfigured_) return;
    webConfigured_ = true;
    auto authorized = [this]() {
        if (!ftpPassword_[0] || webServer_.authenticate(ftpUsername_, ftpPassword_)) return true;
        webServer_.requestAuthentication(); return false;
    };
    webServer_.on("/legacy", HTTP_GET, [this, authorized]() {
        if (!authorized()) return;
        static const char page[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>Wio Apps</title><style>*{box-sizing:border-box}body{font:14px system-ui;margin:0;background:#f4f4f5;color:#18181b}header{padding:16px 22px;background:#111827;color:white;display:flex;gap:12px;align-items:center}header b{font-size:18px}main{max-width:1000px;margin:auto;padding:18px}.bar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:12px}button,input{padding:9px 12px;border:1px solid #d4d4d8;border-radius:8px;background:white}button{cursor:pointer}.drop{border:2px dashed #a1a1aa;border-radius:12px;padding:18px;text-align:center;margin-bottom:12px}.drop.over{border-color:#2563eb;background:#dbeafe}table{width:100%;border-collapse:collapse;background:white;border-radius:12px;overflow:hidden}td,th{padding:11px;text-align:left;border-bottom:1px solid #eee}tr[data-dir]{font-weight:600;cursor:pointer}.actions button{padding:5px 8px;margin-right:4px}dialog{width:min(800px,94vw);border:0;border-radius:12px;padding:16px}textarea{width:100%;height:60vh;font:13px monospace}.muted{color:#71717a}</style><header><b>Wio Tracker L2</b><span>/apps Web Editor</span></header><main><div class=bar><button onclick=up()>← Up</button><b id=path>/</b><button onclick=mkdir()>New folder</button><button onclick=newFile()>New file</button><button onclick=load()>Refresh</button></div><div class=drop id=drop>Drop files here · Drop a PocketJS ZIP to install an App<br><span class=muted>ZIP must contain app.json and app.js</span><input id=pick type=file multiple></div><table><thead><tr><th>Name</th><th>Size</th><th>Actions</th></tr></thead><tbody id=list></tbody></table></main><dialog id=edit><h3 id=editName></h3><textarea id=text></textarea><div class=bar><button onclick=save()>Save</button><button onclick=edit.close()>Cancel</button></div></dialog><script>let cwd='/';const $=x=>document.getElementById(x),enc=x=>encodeURIComponent(x);async function api(u,o){let r=await fetch(u,o);if(!r.ok)throw Error(await r.text());return r}async function load(){try{let a=await(await api('/api/list?path='+enc(cwd))).json();$('path').textContent='/apps'+cwd;$('list').innerHTML=a.map(x=>`<tr ${x.dir?'data-dir':''}><td onclick="${x.dir?`go('${esc(x.name)}')`:''}">${x.dir?'📁':'📄'} ${html(x.name)}</td><td>${x.dir?'—':x.size}</td><td class=actions>${x.dir?'':`<button onclick="openFile('${esc(x.name)}')">Edit</button>`}<button onclick="renameItem('${esc(x.name)}')">Rename</button><button onclick="del('${esc(x.name)}')">Delete</button></td></tr>`).join('')}catch(e){alert(e)}}function html(s){return s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]))}function esc(s){return s.replace(/['\\]/g,'\\$&')}function join(n){return(cwd==='/'?'/':cwd+'/')+n}function go(n){cwd=join(n);load()}function up(){if(cwd!=='/')cwd=cwd.slice(0,cwd.lastIndexOf('/'))||'/';load()}async function post(url,data){await api(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});load()}function mkdir(){let n=prompt('Folder name');if(n)post('/api/mkdir',{path:join(n)})}function newFile(){let n=prompt('File name','app.js');if(n){$('editName').textContent=join(n);$('text').value='';edit.showModal()}}async function openFile(n){let p=join(n);$('editName').textContent=p;$('text').value=await(await api('/api/file?path='+enc(p))).text();edit.showModal()}async function save(){await api('/api/save?path='+enc($('editName').textContent),{method:'POST',body:$('text').value});edit.close();load()}function renameItem(n){let v=prompt('New name',n);if(v&&v!==n)post('/api/rename',{from:join(n),to:(cwd==='/'?'/':cwd+'/')+v})}function del(n){if(confirm('Delete '+n+'?'))post('/api/delete',{path:join(n)})}async function upload(files){for(let f of files){let d=new FormData;d.append('file',f);await api('/api/upload?path='+enc(cwd),{method:'POST',body:d})}load();alert('Upload/install complete')}pick.onchange=()=>upload(pick.files);drop.ondragover=e=>{e.preventDefault();drop.classList.add('over')};drop.ondragleave=()=>drop.classList.remove('over');drop.ondrop=e=>{e.preventDefault();drop.classList.remove('over');upload(e.dataTransfer.files)};load()</script>)HTML";
        webServer_.send_P(200, "text/html; charset=utf-8", page);
    });
    webServer_.on("/", HTTP_GET, [this, authorized]() { if (!authorized()) return; webServer_.send_P(200, "text/html; charset=utf-8", WEB_FILES_PAGE); });
    webServer_.on("/app-config", HTTP_GET, [this, authorized]() {
        if (!authorized()) return;
        String id = webServer_.arg("id");
        if (!id.length() || id.length() > 48) { webServer_.send(400, "text/plain", "Invalid app id"); return; }
        for (size_t i = 0; i < id.length(); ++i)
            if (!isalnum(uint8_t(id[i])) && id[i] != '-' && id[i] != '_') { webServer_.send(400, "text/plain", "Invalid app id"); return; }
        File file = SD_MMC.open("/apps/" + id + "/config.html", FILE_READ);
        if (!file || file.isDirectory() || file.size() > 256 * 1024) {
            if (file) file.close(); webServer_.send(404, "text/plain", "App configuration page not found"); return;
        }
        webServer_.sendHeader("Cache-Control", "no-store, max-age=0");
        webServer_.streamFile(file, "text/html; charset=utf-8"); file.close();
    });
    webServer_.on("/screen", HTTP_GET, [this, authorized]() { if (!authorized()) return; webServer_.send_P(200, "text/html; charset=utf-8", WEB_SCREEN_PAGE); });
    webServer_.on("/api/list", HTTP_GET, [this, authorized]() {
        if (!authorized()) return; String path = webPath(webServer_.arg("path")); File dir = SD_MMC.open(path);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); webServer_.send(404, "text/plain", "Folder not found"); return; }
        String json = "["; File item; bool first = true;
        while ((item = dir.openNextFile())) { const char *raw = item.name(); const char *name = strrchr(raw, '/'); name = name ? name + 1 : raw;
            if (!first) json += ','; first = false; json += "{\"name\":\"" + jsonEscape(name) + "\",\"dir\":" + (item.isDirectory() ? "true" : "false") + ",\"size\":" + String(item.size()) + "}"; item.close(); }
        dir.close(); json += ']'; webServer_.send(200, "application/json", json);
    });
    webServer_.on("/api/file", HTTP_GET, [this, authorized]() {
        if (!authorized()) return; String path = webPath(webServer_.arg("path")); File file = SD_MMC.open(path, FILE_READ);
        if (!file || file.isDirectory() || file.size() > 256 * 1024) { if (file) file.close(); webServer_.send(404, "text/plain", "File unavailable or too large"); return; }
        webServer_.streamFile(file, "text/plain; charset=utf-8"); file.close();
    });
    webServer_.on("/api/download", HTTP_GET, [this, authorized]() {
        if (!authorized()) return;
        String path = webPath(webServer_.arg("path"));
        File file = SD_MMC.open(path, FILE_READ);
        if (!file || file.isDirectory()) { if (file) file.close(); webServer_.send(404, "text/plain", "File unavailable"); return; }
        String name = path.substring(path.lastIndexOf('/') + 1);
        webServer_.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
        webServer_.streamFile(file, "application/octet-stream"); file.close();
    });
    webServer_.on("/api/screenshot", HTTP_GET, [this, authorized]() {
        if (!authorized()) return;
        JpegBuffer image;
        if (!captureScreenJpeg(image)) {
            if (image.data) heap_caps_free(image.data);
            webServer_.send(500, "text/plain", "Screenshot capture failed");
            return;
        }
        webServer_.sendHeader("Cache-Control", "no-store, max-age=0");
        webServer_.sendHeader("Content-Disposition", "inline; filename=screen.jpg");
        webServer_.setContentLength(image.size);
        webServer_.send(200, "image/jpeg", "");
        webServer_.sendContent(reinterpret_cast<const char *>(image.data), image.size);
        heap_caps_free(image.data);
    });
    webServer_.on("/api/click", HTTP_POST, [this, authorized]() {
        if (!authorized()) return;
        if (!webServer_.hasArg("x") || !webServer_.hasArg("y")) {
            webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"x and y are required\"}");
            return;
        }
        const long x = webServer_.arg("x").toInt();
        const long y = webServer_.arg("y").toInt();
        if (x < 0 || x >= 320 || y < 0 || y >= 240) {
            webServer_.send(400, "application/json", "{\"ok\":false,\"error\":\"coordinates out of range\"}");
            return;
        }
        const bool queued = queueWebTap(uint16_t(x), uint16_t(y));
        webServer_.send(queued ? 202 : 409, "application/json",
                        queued ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"click already pending\"}");
    });
    webServer_.on("/api/save", HTTP_POST, [this, authorized]() {
        if (!authorized()) return; String path = webPath(webServer_.arg("path")); String temp = path + ".save";
        if (!path.length() || webServer_.arg("plain").length() > 256 * 1024) { webServer_.send(400, "text/plain", "Invalid file"); return; }
        SD_MMC.remove(temp); File file = SD_MMC.open(temp, FILE_WRITE); String body = webServer_.arg("plain"); bool ok = file && file.print(body) == body.length(); if (file) file.close();
        if (ok) { SD_MMC.remove(path); ok = SD_MMC.rename(temp, path); } if (!ok) SD_MMC.remove(temp); webServer_.send(ok ? 200 : 500, "text/plain", ok ? "Saved" : "Save failed");
    });
    webServer_.on("/api/mkdir", HTTP_POST, [this, authorized]() { if (!authorized()) return; String p=webPath(webServer_.arg("path")); bool ok=p.length()&&ensureDirectoryTree(p); webServer_.send(ok?200:400,"text/plain",ok?"Created":"Create failed"); });
    webServer_.on("/api/rename", HTTP_POST, [this, authorized]() { if (!authorized()) return; String a=webPath(webServer_.arg("from")),b=webPath(webServer_.arg("to")); bool ok=a.length()&&b.length()&&!SD_MMC.exists(b)&&SD_MMC.rename(a,b); if (ok) appUI.requestAppRefresh(); webServer_.send(ok?200:400,"text/plain",ok?"Renamed":"Rename failed"); });
    webServer_.on("/api/delete", HTTP_POST, [this, authorized]() { if (!authorized()) return; String p=webPath(webServer_.arg("path")); bool ok=p.length()&&p!="/apps"&&removeTree(p); if (ok) appUI.requestAppRefresh(); webServer_.send(ok?200:400,"text/plain",ok?"Deleted":"Delete failed"); });
    webServer_.on("/api/install/start", HTTP_POST, [this, authorized]() {
        if (!authorized()) return;
        String id = webServer_.arg("id");
        for (size_t i = 0; i < id.length(); ++i) if (!isalnum(uint8_t(id[i])) && id[i] != '-' && id[i] != '_') { webServer_.send(400, "text/plain", "Invalid App id"); return; }
        String staging = "/apps/.install-" + id;
        // Remove the archive left by firmware versions that decompressed ZIPs
        // on-device. New installs never create this file.
        SD_MMC.remove("/apps/.pocket-install.zip");
        if (SD_MMC.exists(staging)) removeTree(staging);
        const bool ok = id.length() && ensureDirectoryTree(staging);
        webServer_.send(ok ? 200 : 500, "text/plain", ok ? "Ready" : "Staging failed");
    });
    webServer_.on("/api/install/cancel", HTTP_POST, [this, authorized]() {
        if (!authorized()) return;
        String path = "/apps/.install-" + webServer_.arg("id");
        const bool ok = path.length() && (!SD_MMC.exists(path) || removeTree(path));
        webServer_.send(ok ? 200 : 400, "text/plain", ok ? "Cancelled" : "Cancel failed");
    });
    webServer_.on("/api/install/commit", HTTP_POST, [this, authorized]() {
        if (!authorized()) return;
        String id = webServer_.arg("id");
        String staging = "/apps/.install-" + id, destination = "/apps/" + id, backup = "/apps/.backup-" + id;
        bool ok = id.length() && staging.length() && SD_MMC.exists(staging + "/app.json") && SD_MMC.exists(staging + "/app.js");
        const bool hadOld = SD_MMC.exists(destination);
        if (ok && SD_MMC.exists(backup)) removeTree(backup);
        if (ok && hadOld && !SD_MMC.rename(destination, backup)) ok = false;
        if (ok && !SD_MMC.rename(staging, destination)) { ok = false; if (hadOld) SD_MMC.rename(backup, destination); }
        if (ok && SD_MMC.exists(backup)) removeTree(backup);
        if (ok) appUI.requestAppRefresh();
        sdUsedBytes_ = SD_MMC.usedBytes();
        webServer_.send(ok ? 200 : 400, "text/plain", ok ? "Installed" : "Install commit failed");
    });
    webServer_.on("/api/upload", HTTP_POST, [this, authorized]() { if (!authorized()) return; webServer_.send(webUploadOk_?200:400,"text/plain",webUploadOk_?"Uploaded":"Upload/install failed"); }, [this]() { handleWebUpload(); });
}

void ConnectivityService::handleWebUpload() {
    HTTPUpload &upload = webServer_.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String folder = webPath(webServer_.arg("path")); String name = upload.filename;
        name.replace("/", "_"); name.replace("\\", "_");
        webUploadPath_ = folder + (folder.endsWith("/") ? "" : "/") + name;
        SD_MMC.remove(webUploadPath_); webUploadFile_ = SD_MMC.open(webUploadPath_, FILE_WRITE); webUploadOk_ = bool(webUploadFile_);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (webUploadFile_) webUploadOk_ = webUploadOk_ && webUploadFile_.write(upload.buf, upload.currentSize) == upload.currentSize;
    } else if (upload.status == UPLOAD_FILE_END) {
        if (webUploadFile_) webUploadFile_.close();
        sdUsedBytes_ = SD_MMC.usedBytes();
    } else if (upload.status == UPLOAD_FILE_ABORTED) { if (webUploadFile_) webUploadFile_.close(); SD_MMC.remove(webUploadPath_); webUploadOk_ = false; }
}

bool ConnectivityService::installPocketZip(const String &archivePath, const String &fileName) {
    File archive = SD_MMC.open(archivePath, FILE_READ); const size_t size = archive ? archive.size() : 0;
    if (!archive || size < 22 || size > SERIAL_APP_MAX_BYTES) { if (archive) archive.close(); return false; }
    uint8_t *zip = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!zip || archive.read(zip, size) != size) { if (zip) heap_caps_free(zip); archive.close(); return false; } archive.close();
    size_t eocd = size - 22; while (eocd && zip32(zip + eocd) != 0x06054b50) --eocd;
    if (!eocd || eocd + 22 > size) { heap_caps_free(zip); return false; }
    const uint16_t count = zip16(zip + eocd + 10); size_t cursor = zip32(zip + eocd + 16);
    String id = fileName; int dot = id.lastIndexOf('.'); if (dot > 0) id.remove(dot); id.toLowerCase();
    for (size_t i=0;i<id.length();++i) if (!isalnum(uint8_t(id[i])) && id[i]!='-' && id[i]!='_') id.setCharAt(i,'-');
    if (!id.length() || count > 64 || cursor >= size) { heap_caps_free(zip); return false; }
    const String destination = "/apps/" + id;
    const String staging = "/apps/.install-" + id;
    const String backup = "/apps/.backup-" + id;
    if (SD_MMC.exists(staging)) removeTree(staging);
    if (!ensureDirectoryTree(staging)) { heap_caps_free(zip); return false; }
    bool ok = true;
    for (uint16_t index = 0; ok && index < count; ++index) {
        if (cursor + 46 > size || zip32(zip + cursor) != 0x02014b50) { ok=false; break; }
        const uint16_t method=zip16(zip+cursor+10), nameLen=zip16(zip+cursor+28), extraLen=zip16(zip+cursor+30), commentLen=zip16(zip+cursor+32);
        const uint32_t packed=zip32(zip+cursor+20), unpacked=zip32(zip+cursor+24), local=zip32(zip+cursor+42);
        if (cursor+46+nameLen+extraLen+commentLen>size || nameLen>160 || local+30>size) { ok=false; break; }
        String name; for (uint16_t n=0;n<nameLen;++n) name += char(zip[cursor+46+n]); cursor += 46+nameLen+extraLen+commentLen;
        if (name.startsWith("/") || name.indexOf("..")>=0) { ok=false; break; }
        // Finder adds resource-fork records under __MACOSX. They are not app
        // files and some use ZIP features that the small ROM inflater rejects.
        if (name.startsWith("__MACOSX/") || name.startsWith("._")) continue;
        const String prefix = id + "/";
        if (name.startsWith(prefix)) name.remove(0, prefix.length());
        else if (name.indexOf('/') >= 0) continue; // unrelated top-level tree
        if (!name.length() || name.startsWith("._")) continue; String output=staging+"/"+name;
        if (output.endsWith("/")) { ok=ensureDirectoryTree(output.substring(0,output.length()-1)); continue; }
        int lastSlash=output.lastIndexOf('/'); if (lastSlash>0 && !ensureDirectoryTree(output.substring(0,lastSlash))) { ok=false; break; }
        const uint16_t localName=zip16(zip+local+26), localExtra=zip16(zip+local+28); const size_t data=local+30+localName+localExtra;
        if (data+packed>size || unpacked>512*1024 || (method!=0 && method!=8)) { ok=false; break; }
        // LovyanGFX's tiny inflater writes through CPU-local pointers. Keep its
        // output in internal RAM; using PSRAM here resets ESP32-S3 mid-upload.
        uint8_t *decoded=static_cast<uint8_t *>(heap_caps_malloc(max<uint32_t>(1,unpacked), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        if (!decoded) { ok=false; break; }
        size_t actual = method==0 ? (memcpy(decoded,zip+data,unpacked),unpacked) : lgfx_tinfl_decompress_mem_to_mem(decoded,unpacked,zip+data,packed,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (actual!=unpacked) ok=false; else { File out=SD_MMC.open(output,FILE_WRITE); ok=out&&out.write(decoded,unpacked)==unpacked; if(out)out.close(); }
        heap_caps_free(decoded);
    }
    ok = ok && SD_MMC.exists(staging+"/app.json") && SD_MMC.exists(staging+"/app.js");
    if (ok) {
        if (SD_MMC.exists(backup)) removeTree(backup);
        const bool hadOld = SD_MMC.exists(destination);
        if (hadOld && !SD_MMC.rename(destination, backup)) ok = false;
        if (ok && !SD_MMC.rename(staging, destination)) {
            ok = false; if (hadOld) SD_MMC.rename(backup, destination);
        }
        if (ok && SD_MMC.exists(backup)) removeTree(backup);
    }
    if (!ok && SD_MMC.exists(staging)) removeTree(staging);
    heap_caps_free(zip); return ok;
}

void ConnectivityService::serviceSerialInstaller() {
    // HWCDC on this board reliably delivers newline-terminated frames, but can
    // leave an unframed binary packet outside Arduino's receive queue. Transfer
    // data as acknowledged hex lines while a PUT is active.
    if (serialUploadRemaining) {
        while (Serial.available()) {
            const char value = char(Serial.read());
            if (value != '\n' && value != '\r') {
                if (serialCommandLength + 1 < sizeof(serialCommand)) serialCommand[serialCommandLength++] = value;
                else serialCommandLength = 0;
                continue;
            }
            if (!serialCommandLength) continue;
            serialCommand[serialCommandLength] = 0; serialCommandLength = 0;
            if (strncmp(serialCommand, "@WIOCHUNK ", 10)) continue;
            const char *hex = serialCommand + 10; const size_t hexLength = strlen(hex);
            if (!hexLength || (hexLength & 1) || hexLength > 128 || hexLength / 2 > serialUploadRemaining) {
                Serial.println("@WIOERR invalid-chunk"); continue;
            }
            uint8_t decoded[64]; bool valid = true;
            for (size_t i = 0; i < hexLength / 2; ++i) {
                const char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0}; char *end = nullptr;
                decoded[i] = uint8_t(strtoul(pair, &end, 16));
                if (!end || *end) { valid = false; break; }
            }
            const size_t count = hexLength / 2;
            if (!valid || serialUploadFile.write(decoded, count) != count) {
                serialUploadFile.close(); SD_MMC.remove(serialUploadTemp);
                serialUploadRemaining = 0; Serial.println("@WIOERR write-failed"); return;
            }
            serialUploadRemaining -= count; serialUploadLastByte = millis();
            if (serialUploadRemaining) Serial.printf("@WIOACK %u\n", unsigned(serialUploadRemaining));
            else {
                serialUploadFile.close(); SD_MMC.remove(serialUploadTarget);
                if (SD_MMC.rename(serialUploadTemp, serialUploadTarget)) {
                    sdUsedBytes_ = SD_MMC.usedBytes(); Serial.printf("@WIOOK %u\n", unsigned(serialUploadTotal));
                } else { SD_MMC.remove(serialUploadTemp); Serial.println("@WIOERR rename-failed"); }
            }
        }
        if (serialUploadRemaining && millis() - serialUploadLastByte > 60000) {
            serialUploadFile.close(); SD_MMC.remove(serialUploadTemp);
            serialUploadRemaining = 0; Serial.println("@WIOERR timeout");
        }
        return;
    }

    while (Serial.available()) {
        const char value = char(Serial.read());
        if (value != '\n' && value != '\r') {
            if (serialCommandLength + 1 < sizeof(serialCommand)) serialCommand[serialCommandLength++] = value;
            else serialCommandLength = 0;
            continue;
        }
        if (!serialCommandLength) continue;
        serialCommand[serialCommandLength] = 0; serialCommandLength = 0;
        if (!strncmp(serialCommand, "@WIOFATREAD ", 12)) {
            unsigned sector = 0; char trailing;
            if (sscanf(serialCommand + 12, "%u %c", &sector, &trailing) != 1 || !sdMounted_ || uint64_t(sector) * 512 >= SD_MMC.cardSize()) { Serial.println("@WIOERR invalid-sector"); continue; }
            uint8_t *bytes = serialSdBytes; const DRESULT result = disk_read(0, bytes, sector, 1);
            Serial.printf("@WIOFATREAD %u sector=%u status=%u\n", result == RES_OK ? 512 : 0, sector, unsigned(result));
            if (result == RES_OK) Serial.write(bytes, 512);
            Serial.flush(); continue;
        }
        if (!strncmp(serialCommand, "@WIOSDLIST ", 11)) {
            String normalized = serialCommand + 11;
            while (normalized.length() > 1 && normalized.endsWith("/")) normalized.remove(normalized.length() - 1);
            const char *path = normalized.c_str();
            const bool safe = (!strcmp(path, "/apps") || !strcmp(path, "/appdata") || !strcmp(path, "/maps")) || (!strstr(path, "..") && (!strncmp(path, "/appdata/", 9) || !strncmp(path, "/apps/", 6) || !strncmp(path, "/maps/", 6)));
            File directory = safe ? SD_MMC.open(path) : File();
            if (!directory || !directory.isDirectory()) { Serial.println("@WIOERR directory-unavailable"); continue; }
            File item; unsigned count = 0;
            while (count < 64 && (item = directory.openNextFile())) {
                JsonDocument row; row["path"] = item.path(); row["size"] = item.size(); row["directory"] = item.isDirectory();
                Serial.print("@WIOSDFILE "); serializeJson(row, Serial); Serial.println(); item.close(); ++count;
            }
            directory.close(); Serial.printf("@WIOSDEND %u\n", count); continue;
        }
        if (!strncmp(serialCommand, "@WIOSDREAD ", 11)) {
            char path[128]{}, trailing = 0; unsigned offset = 0, length = 0;
            const bool parsed = sscanf(serialCommand + 11, "%127s %u %u %c", path, &offset, &length, &trailing) == 3;
            const bool safe = parsed && !strstr(path, "..") && (!strncmp(path, "/appdata/", 9) || !strncmp(path, "/apps/", 6) || !strncmp(path, "/maps/", 6)) && length && length <= 1024 && offset <= 2 * 1024 * 1024;
            if (!safe) { Serial.println("@WIOERR invalid-read"); continue; }
            char full[144]; snprintf(full, sizeof(full), "/sdcard%s", path);
            const int fd = open(full, O_RDONLY); struct stat info{};
            if (fd < 0) { Serial.printf("@WIOERR open errno=%d\n", errno); continue; }
            fstat(fd, &info); uint8_t *bytes = serialSdBytes; errno = 0;
            const ssize_t read = pread(fd, bytes, length, offset); const int error = errno; close(fd);
            Serial.printf("@WIOSDREAD %d size=%lu errno=%d\n", int(read), (unsigned long)info.st_size, error);
            if (read > 0) Serial.write(bytes, read);
            Serial.flush(); continue;
        }
        if (!strcmp(serialCommand, "@WIOFRAME")) {
            JpegBuffer frame;
            if (captureScreenJpeg(frame)) {
                Serial.printf("@WIOFRAME %u\n", unsigned(frame.size));
                Serial.write(frame.data, frame.size); Serial.flush();
            } else Serial.println("@WIOERR capture-failed");
            if (frame.data) heap_caps_free(frame.data);
            continue;
        }
        if (!strncmp(serialCommand, "@WIOPROTOCOL ", 13)) {
            const bool ok = communication.select(serialCommand + 13);
            Serial.printf("@WIOPROTOCOL %s protocol=%s ready=%u time=%lu error=%s\n",
                ok ? "ok" : "failed", communication.protocol(), unsigned(communication.ready()),
                static_cast<unsigned long>(systemClock.now()), communication.error());
            continue;
        }
        if (!strncmp(serialCommand, "@WIORADIOPRESET ", 16)) {
            unsigned index = 0; char trailing = 0;
            const bool parsed = sscanf(serialCommand + 16, "%u %c", &index, &trailing) == 1;
            const bool ok = parsed && communication.selectRadioPreset(index);
            Serial.printf("@WIORADIOPRESET %s index=%u frequency=%.3f\n", ok ? "ok" : "failed",
                          unsigned(communication.radioPresetIndex()), communication.radioPreset(communication.radioPresetIndex()).frequency);
            continue;
        }
        if (!strncmp(serialCommand, "@WIOMTFORGET ", 13)) {
            unsigned number = 0; char extra;
            const bool valid = sscanf(serialCommand + 13, "%x %c", &number, &extra) == 1;
            Serial.println(valid && meshtasticService.forgetPeer(number) ? "@WIOOK peer-forgotten" : "@WIOERR forget-failed"); continue;
        }
        if (!strncmp(serialCommand, "@WIOMTSETKEY ", 13)) {
            unsigned number = 0; char hex[65]{}, extra; uint8_t key[32]{};
            bool valid = sscanf(serialCommand + 13, "%x %64s %c", &number, hex, &extra) == 2 && strlen(hex) == 64;
            for (unsigned i = 0; valid && i < 32; ++i) {
                unsigned byte = 0; char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
                valid = isxdigit(pair[0]) && isxdigit(pair[1]) && sscanf(pair, "%x", &byte) == 1; key[i] = byte;
            }
            Serial.println(valid && meshtasticService.importPublicKey(number, key) ? "@WIOOK public-key-saved" : "@WIOERR key-import-failed"); continue;
        }
        if (!strcmp(serialCommand, "@WIOMTPUBKEY")) {
            uint8_t key[32];
            if (meshtasticService.publicKey(key)) { Serial.print("@WIOMTPUBKEY "); for (auto byte : key) Serial.printf("%02x", byte); Serial.println(); }
            else Serial.println("@WIOERR identity-unavailable");
            continue;
        }
        if (!strncmp(serialCommand, "@WIOMTINJECT ", 13)) {
            const char *hex = serialCommand + 13; size_t length = strlen(hex); uint8_t frame[88]{}; bool valid = length % 2 == 0 && length / 2 <= sizeof(frame);
            for (size_t i = 0; valid && i < length / 2; ++i) {
                unsigned byte = 0; char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
                valid = isxdigit(pair[0]) && isxdigit(pair[1]) && sscanf(pair, "%x", &byte) == 1;
                frame[i] = byte;
            }
            Serial.println(valid && meshtasticService.receiveFrame(frame, length / 2, -60, 7) ? "@WIOOK injected-frame" : "@WIOERR rejected-frame"); continue;
        }
        if (!strncmp(serialCommand, "@WIOMTROLE ", 11)) {
            const char *role = serialCommand + 11;
            const int index = !strcmp(role, "client") ? 0 : !strcmp(role, "client_mute") ? 1 : -1;
            const bool ok = index >= 0 && meshtasticService.selectRole(index);
            Serial.printf("@WIOMTROLE %s role=%s error=%s\n", ok ? "ok" : "failed", meshtasticService.roleName(meshtasticService.roleIndex()), meshtasticService.error()); continue;
        }
        if (!strcmp(serialCommand, "@WIOCHATSTATUS")) {
            Serial.printf("@WIOCHATSTATUS ready=%u messages=%u peers=%u pending=%u path=/appdata/chats error=%s\n",
                unsigned(chatStore.ready()), unsigned(chatStore.messageCount()), unsigned(chatStore.peerCount()), chatStore.pending(), chatStore.error()); continue;
        }
        if (!strcmp(serialCommand, "@WIOCHATCLEAR ALL")) {
            Serial.println(chatStore.clearHistory() ? "@WIOOK history-cleared" : "@WIOERR history-clear-failed"); continue;
        }
        if (!strcmp(serialCommand, "@WIOCHATTAIL") || !strcmp(serialCommand, "@WIOCHATDATA")) {
            auto *records = static_cast<ChatRecord *>(heap_caps_malloc(3 * sizeof(ChatRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!records) { Serial.println("@WIOERR allocation"); continue; }
            const size_t count = chatStore.history(records, 3, communication.configurationProtocol(), nullptr, -1, 0, !strcmp(serialCommand, "@WIOCHATTAIL"));
            for (size_t i = 0; i < count; ++i) {
                const auto &record = records[i]; JsonDocument item;
                item["id"] = record.id; item["protocol"] = record.protocol; item["peerId"] = record.peerId; item["peer"] = record.peer;
                item["port"] = record.port; item["timestamp"] = record.timestamp; item["direct"] = bool(record.flags & 1);
                char text[241]{}; if (record.port == 1) memcpy(text, record.payload, record.length); item["text"] = text;
                Serial.print("@WIOCHAT "); serializeJson(item, Serial); Serial.println();
            }
            heap_caps_free(records); Serial.printf("@WIOCHATEND %u\n", unsigned(count)); continue;
        }
        if (!strcmp(serialCommand, "@WIOMTSTATUS")) {
            const auto preset = meshtasticService.preset(meshtasticService.presetIndex());
            Serial.printf("@WIOMTSTATUS ready=%u node=%08lx region=%s modem=%s frequency=%.3f channel=%s role=%s signal=%u rssi=%d snr=%.2f tx=%lu rx=%lu rejected=%lu queued=%u lastTx=%08lx ack=%08lx error=%s\n",
                unsigned(meshtasticService.ready()), (unsigned long)meshtasticService.nodeNumber(), meshtasticService.regionName(meshtasticService.regionIndex()),
                preset.name, preset.frequency, meshtasticService.channelName(), meshtasticService.roleName(meshtasticService.roleIndex()), unsigned(meshtasticService.signalKnown()), meshtasticService.rssi(), meshtasticService.snr(), (unsigned long)meshtasticService.txCount(), (unsigned long)meshtasticService.rxCount(),
                (unsigned long)meshtasticService.rejectedCount(), meshtasticService.queuedCount(), (unsigned long)meshtasticService.lastTxId(), (unsigned long)meshtasticService.lastAckId(), meshtasticService.error()); continue;
        }
        if (!strncmp(serialCommand, "@WIOCOMMSEND ", 13)) {
            Serial.println(communication.sendPublic(serialCommand + 13) ? "@WIOOK send-queued" : "@WIOERR send-failed"); continue;
        }
        if (!strcmp(serialCommand, "@WIOADVERTISE")) { Serial.println(communication.advertise() ? "@WIOOK advert-queued" : "@WIOERR advert-failed"); continue; }
        if (!strcmp(serialCommand, "@WIOCOMMSTATUS")) {
            Serial.printf("@WIOCOMMSTATUS protocol=%s ready=%u time=%lu\n", communication.protocol(),
                unsigned(communication.ready()), static_cast<unsigned long>(systemClock.now()));
            continue;
        }
        if (!strcmp(serialCommand, "@WIOPING")) { Serial.println("@WIOPONG app-sdk=3 pocketjs=1"); continue; }
        if (!strcmp(serialCommand, "@WIOMAPSTATUS")) {
            Serial.printf("@WIOMAPSTATUS state=%u done=%u/%u error=%s heap=%u psram=%u\n",
                unsigned(jobState_), unsigned(jobDone_), unsigned(jobTotal_), jobError_,
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
            continue;
        }
        if (!strncmp(serialCommand, "@WIOMAP ", 8)) {
            double latitude = 0, longitude = 0; unsigned zoom = 0;
            const bool parsed = sscanf(serialCommand + 8, "%lf %lf %u", &latitude, &longitude, &zoom) == 3;
            Serial.println(parsed && zoom >= 10 && zoom <= 16 &&
                           startMapDownload(latitude, longitude, uint8_t(zoom)) ?
                           "@WIOOK map-started" : "@WIOERR map-start-failed");
            continue;
        }
        if (!strncmp(serialCommand, "@WIOMAPVIEW ", 12)) {
            double latitude = 0, longitude = 0; unsigned zoom = 0;
            const bool parsed = sscanf(serialCommand + 12, "%lf %lf %u", &latitude, &longitude, &zoom) == 3;
            Serial.println(parsed && zoom >= 10 && zoom <= 16 && appUI.setMapCenter(latitude, longitude, uint8_t(zoom)) ?
                           "@WIOOK map-view" : "@WIOERR map-view-failed"); continue;
        }
        if (!strcmp(serialCommand, "@WIOCONTROL")) {
            appUI.handleVolumeButton(); Serial.println("@WIOOK control"); continue;
        }
        if (!strcmp(serialCommand, "@WIOI2CSCAN")) {
            hardware.setGroveEnabled(true);
            for (uint8_t address = 8; address <= 0x77; ++address) {
                // A NACK can leave the board bus held; match the per-probe recovery
                // used by the app rather than letting one address stall later probes.
                hardware.recoverI2cBus();
                Wire.beginTransmission(address);
                if (Wire.endTransmission() == 0) Serial.printf("@WIOI2C %02X\n", address);
            }
            Wire.setTimeOut(50); Serial.println("@WIOI2CEND"); continue;
        }
        if (!strcmp(serialCommand, "@WIOHOME")) {
            appUI.nextPage(); Serial.println("@WIOOK home"); continue;
        }
        if (!strcmp(serialCommand, "@WIOSCREEN")) {
            if (!hardware.displayAwake()) appUI.toggleDisplayWithEffect();
            appUI.noteUserActivity();
            appUI.takeScreenshot(); Serial.println("@WIOOK screenshot requested"); continue;
        }
        if (!strncmp(serialCommand, "@WIOTAP ", 8)) {
            unsigned x = 0, y = 0;
            const bool valid = sscanf(serialCommand + 8, "%u %u", &x, &y) == 2 && x < 320 && y < 240;
            Serial.println(valid && queueWebTap(uint16_t(x), uint16_t(y)) ?
                           "@WIOOK tap queued" : "@WIOERR tap-failed");
            continue;
        }
        if (!strncmp(serialCommand, "@WIOSWIPE ", 10)) {
            unsigned x = 0, y = 0, endX = 0, endY = 0;
            const bool valid = sscanf(serialCommand + 10, "%u %u %u %u", &x, &y, &endX, &endY) == 4 &&
                x < 320 && y < 240 && endX < 320 && endY < 240;
            Serial.println(valid && queueWebSwipe(x, y, endX, endY) ? "@WIOOK swipe queued" : "@WIOERR swipe-failed");
            continue;
        }
        if (!strncmp(serialCommand, "@WIOOPEN ", 9)) {
            const bool opened = appUI.openInstalledAppById(serialCommand + 9);
            Serial.println(opened ? "@WIOOK opening" : "@WIOERR app-not-found");
            continue;
        }
        if (!strcmp(serialCommand, "@WIOREBOOT")) {
            Serial.println("@WIOOK rebooting"); Serial.flush(); delay(80); ESP.restart();
        }
        if (!strcmp(serialCommand, "@WIOFORMATSD FAT32")) {
            appUI.nextPage(); communication.select("off");
            Serial.println(startFormatSd() ? "@WIOOK format-started" : "@WIOERR format-failed"); continue;
        }
        if (!strncmp(serialCommand, "@WIOCOMMSELECT ", 15)) {
            Serial.println(communication.select(serialCommand + 15) ? "@WIOOK protocol-selected" : "@WIOERR protocol-failed"); continue;
        }
        if (!strncmp(serialCommand, "@WIOAPPREMOVE ", 14)) {
            const char *id = serialCommand + 14;
            bool safe = sdMounted_ && strlen(id) > 0 && strlen(id) <= 24;
            for (const char *c = id; safe && *c; ++c)
                safe = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
            const String path = String("/apps/") + id;
            const bool ok = safe && SD_MMC.exists(path) && removeTree(path);
            if (ok) appUI.requestAppRefresh();
            Serial.println(ok ? "@WIOOK app-removed" : "@WIOERR remove-failed"); continue;
        }
        if (!strncmp(serialCommand, "@WIOMKDIR ", 10)) {
            const char *path = serialCommand + 10;
            const bool safe = sdMounted_ && (!strcmp(path, "/appdata") || !strncmp(path, "/apps/", 6) || !strncmp(path, "/appdata/", 9)) && !strstr(path, "..") && strlen(path) < 120;
            Serial.println(safe && (SD_MMC.exists(path) || SD_MMC.mkdir(path)) ? "@WIOOK mkdir" : "@WIOERR mkdir-failed");
            continue;
        }
        if (!strncmp(serialCommand, "@WIOPUT ", 8)) {
            char path[128]{}; unsigned size = 0;
            const bool parsed = sscanf(serialCommand + 8, "%127s %u", path, &size) == 2;
            const bool safe = parsed && sdMounted_ && (!strncmp(path, "/apps/", 6) || !strncmp(path, "/appdata/", 9)) && !strstr(path, "..") &&
                              size > 0 && size <= SERIAL_APP_MAX_BYTES;
            if (!safe) { Serial.println("@WIOERR invalid-put"); continue; }
            strlcpy(serialUploadTarget, path, sizeof(serialUploadTarget));
            snprintf(serialUploadTemp, sizeof(serialUploadTemp), "%s.upload", path);
            SD_MMC.remove(serialUploadTemp);
            serialUploadFile = SD_MMC.open(serialUploadTemp, FILE_WRITE);
            if (!serialUploadFile) { Serial.println("@WIOERR cannot-open"); continue; }
            serialUploadRemaining = serialUploadTotal = size; serialUploadLastByte = millis();
            Serial.printf("@WIOREADY %u hex-chunks\n", size); Serial.flush();
            return;
        }
    }
}

void ConnectivityService::connectWifi(const char *ssid, const char *password) {
    char requestedSsid[sizeof(ssid_)];
    strlcpy(requestedSsid, ssid ? ssid : "", sizeof(requestedSsid));
    const bool keepSavedPassword = (!password || !password[0]) && strcmp(requestedSsid, ssid_) == 0;
    strlcpy(ssid_, requestedSsid, sizeof(ssid_));
    if (!keepSavedPassword) strlcpy(password_, password ? password : "", sizeof(password_));
    Preferences preferences;
    preferences.begin("mesh-net", false);
    preferences.putString("ssid", ssid_);
    preferences.putString("pass", password_);
    preferences.putBool("wifi-on", true);
    preferences.end();
    if (!ssid_[0]) { disconnectWifi(); return; }
    wifiEnabled_ = true;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.begin(ssid_, password_);
    lastReconnectAttempt_ = millis();
}

void ConnectivityService::disconnectWifi() {
    wifiEnabled_ = false;
    Preferences preferences; preferences.begin("mesh-net", false); preferences.putBool("wifi-on", false); preferences.end();
    if (ftpRunning_) stopFtp();
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
}

void ConnectivityService::setWifiEnabled(bool enabled) {
    wifiEnabled_ = enabled;
    Preferences preferences; preferences.begin("mesh-net", false); preferences.putBool("wifi-on", enabled); preferences.end();
    if (!enabled) { if (ftpRunning_) stopFtp(); WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF); return; }
    if (!ssid_[0]) return;
    WiFi.mode(WIFI_STA); WiFi.begin(ssid_, password_); lastReconnectAttempt_ = millis();
}

bool ConnectivityService::wifiConnected() const { return WiFi.status() == WL_CONNECTED; }
String ConnectivityService::wifiAddress() const { return wifiConnected() ? WiFi.localIP().toString() : String(); }
int32_t ConnectivityService::wifiRssi() const { return wifiConnected() ? WiFi.RSSI() : 0; }

void ConnectivityService::setHttpProxy(const char *host, uint16_t port) {
    strlcpy(proxyHost_, host ? host : "", sizeof(proxyHost_));
    proxyPort_ = proxyHost_[0] ? port : 0;
    Preferences preferences;
    preferences.begin("mesh-net", false);
    preferences.putString("proxy-host", proxyHost_);
    preferences.putUShort("proxy-port", proxyPort_);
    preferences.end();
    Serial.printf("Network: HTTP proxy %s\n", proxyEnabled() ? "enabled" : "disabled");
}

bool ConnectivityService::startWifiScan() {
    WiFi.scanDelete();
    WiFi.mode(WIFI_STA);
    return WiFi.scanNetworks(true, true) == WIFI_SCAN_RUNNING;
}

int16_t ConnectivityService::wifiScanComplete() const { return WiFi.scanComplete(); }
String ConnectivityService::wifiScanSsid(int16_t index) const { return WiFi.SSID(index); }
int32_t ConnectivityService::wifiScanRssi(int16_t index) const { return WiFi.RSSI(index); }
void ConnectivityService::clearWifiScan() { WiFi.scanDelete(); }

bool ConnectivityService::mountSd(bool formatOnFailure) {
    SD_MMC.end();
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
    // This board/card reported SD read CRC errors at the default 20 MHz,
    // including corrupted app source after reboot. Keep a margin on the 1-bit bus.
    sdMounted_ = SD_MMC.begin("/sdcard", true, formatOnFailure, 10000, 8);
    if (sdMounted_) {
        ensureDirectory("/maps");
        ensureDirectory("/maps/osm");
        sdTotalBytes_ = SD_MMC.totalBytes();
        sdUsedBytes_ = SD_MMC.usedBytes();
        Serial.printf("SD: mounted FAT, %llu/%llu bytes used\n", sdUsedBytes_, sdTotalBytes_);
    } else {
        sdTotalBytes_ = sdUsedBytes_ = 0;
        Serial.println("SD: no usable card");
    }
    return sdMounted_;
}

bool ConnectivityService::startMapDownload(double latitude, double longitude, uint8_t zoom) {
    if (jobState_ == StorageJobState::Downloading || jobState_ == StorageJobState::Formatting) return false;
    if (!wifiConnected()) { finishJob(false, "Wi-Fi not connected"); return false; }
    if (!sdMounted_) { finishJob(false, "SD card unavailable"); return false; }
    if (latitude < -85.0 || latitude > 85.0 || longitude < -180.0 || longitude > 180.0) {
        finishJob(false, "Invalid GPS position"); return false;
    }
    pendingLatitude_ = latitude;
    pendingLongitude_ = longitude;
    pendingZoom_ = constrain(zoom, uint8_t(10), uint8_t(16));
    pendingJob_ = JobType::Download;
    jobState_ = StorageJobState::Downloading;
    jobProgress_ = jobDone_ = 0; jobTotal_ = 9; jobError_[0] = 0;
    if (xTaskCreatePinnedToCore(jobTask, "map-download", 8192, this, 1, nullptr, 0) != pdPASS) {
        finishJob(false, "Cannot start download task"); return false;
    }
    return true;
}

bool ConnectivityService::startIpLocation() {
    if (ipLocationState_ == IpLocationState::Loading) return false;
    if (!wifiConnected()) { ipLocationState_ = IpLocationState::Failed; return false; }
    ipLocationState_ = IpLocationState::Loading;
    if (xTaskCreatePinnedToCore(ipLocationTask, "ip-location", 8192, this, 1, nullptr, 0) != pdPASS) {
        ipLocationState_ = IpLocationState::Failed;
        return false;
    }
    return true;
}

void ConnectivityService::ipLocationTask(void *context) {
    auto *self = static_cast<ConnectivityService *>(context);
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(12000);
    bool valid = false;
    if (http.begin("https://ipwho.is/")) {
        if (http.GET() == HTTP_CODE_OK) {
            const String response = http.getString();
            // Read only the two numeric fields; the response is never shown as UI text.
            auto number = [&](const char *key, double &value) {
                const String field = String('"') + key + '"';
                const int start = response.indexOf(field);
                if (start < 0) return false;
                const int colon = response.indexOf(':', start + field.length());
                if (colon < 0) return false;
                const char *begin = response.c_str() + colon + 1;
                char *end = nullptr;
                value = strtod(begin, &end);
                return end != begin && isfinite(value);
            };
            double latitude = 0, longitude = 0;
            valid = response.indexOf("\"success\":true") >= 0 &&
                    number("latitude", latitude) && number("longitude", longitude) &&
                    latitude >= -85.0 && latitude <= 85.0 &&
                    longitude >= -180.0 && longitude <= 180.0;
            if (valid) { self->ipLatitude_ = latitude; self->ipLongitude_ = longitude; }
        }
        http.end();
    }
    self->ipLocationState_ = valid ? IpLocationState::Ready : IpLocationState::Failed;
    vTaskDelete(nullptr);
}

bool ConnectivityService::startFormatSd() {
    if (jobState_ == StorageJobState::Downloading || jobState_ == StorageJobState::Formatting) return false;
    if (!sdMounted_ && !mountSd(false)) { finishJob(false, "No SD card detected"); return false; }
    if (webUploadFile_) webUploadFile_.close();
    webServer_.stop(); ftpRunning_ = false;
    sdMounted_ = false; // Suspend appdata writers until the filesystem is remounted.
    pendingJob_ = JobType::Format;
    jobState_ = StorageJobState::Formatting;
    jobProgress_ = jobDone_ = 0; jobTotal_ = 1; jobError_[0] = 0;
    if (xTaskCreatePinnedToCore(jobTask, "sd-format", 8192, this, 1, nullptr, 0) != pdPASS) {
        sdMounted_ = true; finishJob(false, "Cannot start format task"); return false;
    }
    return true;
}

void ConnectivityService::acknowledgeJob() {
    if (jobState_ == StorageJobState::Complete || jobState_ == StorageJobState::Failed)
        jobState_ = StorageJobState::Idle;
}

void ConnectivityService::jobTask(void *context) {
    auto *self = static_cast<ConnectivityService *>(context);
    if (self->pendingJob_ == JobType::Download) self->runDownload();
    else if (self->pendingJob_ == JobType::Format) self->runFormat();
    self->pendingJob_ = JobType::None;
    vTaskDelete(nullptr);
}

void ConnectivityService::runDownload() {
    const double latitude = constrain(pendingLatitude_, -85.05112878, 85.05112878);
    const uint32_t scale = 1UL << pendingZoom_;
    const double latitudeRadians = latitude * PI / 180.0;
    const int32_t centerX = int32_t(floor((pendingLongitude_ + 180.0) / 360.0 * scale));
    const int32_t centerY = int32_t(floor((1.0 - asinh(tan(latitudeRadians)) / PI) * 0.5 * scale));
    uint8_t completed = 0;
    for (int8_t dy = -1; dy <= 1; ++dy) {
        for (int8_t dx = -1; dx <= 1; ++dx) {
            if (!wifiConnected()) { finishJob(false, "Wi-Fi disconnected"); return; }
            const int32_t x = centerX + dx;
            const int32_t y = centerY + dy;
            bool downloaded = x >= 0 && y >= 0 && x < int32_t(scale) && y < int32_t(scale);
            for (uint8_t attempt = 0; downloaded && attempt < 3; ++attempt) {
                if (downloadTile(pendingZoom_, uint32_t(x), uint32_t(y))) break;
                downloaded = false;
                if (attempt < 2) { downloaded = true; delay(350 * (attempt + 1)); }
            }
            if (!downloaded) {
                finishJob(false, "Tile download failed"); return;
            }
            jobDone_ = ++completed;
            jobProgress_ = uint8_t(completed * 100 / 9);
        }
    }
    sdUsedBytes_ = SD_MMC.usedBytes();
    finishJob(true);
}

bool ConnectivityService::downloadTile(uint8_t zoom, uint32_t x, uint32_t y) {
    const String zoomDir = "/maps/osm/" + String(zoom);
    const String xDir = zoomDir + "/" + String(x);
    if (!ensureDirectory(zoomDir) || !ensureDirectory(xDir)) {
        Serial.printf("Map: cannot create %s\n", xDir.c_str()); return false;
    }
    const String path = xDir + "/" + String(y) + ".png";
    File existing = SD_MMC.open(path, FILE_READ);
    if (existing && existing.size() > 100) { existing.close(); return true; }
    if (existing) existing.close();

    const String uri = "/" + String(zoom) + "/" + String(x) + "/" + String(y) + ".png";
    if (proxyEnabled()) return downloadTileThroughProxy(path, uri);

    HTTPClient http;
    const String url = "https://tile.openstreetmap.org" + uri;
    http.setConnectTimeout(10000);
    http.setTimeout(15000);
    http.setUserAgent("WioTrackerL2-MeshUI/1.0");
    if (!http.begin(url)) { Serial.printf("Map: HTTP begin failed %s\n", url.c_str()); return false; }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) { Serial.printf("Map: GET %s -> %d %s\n", url.c_str(), code, http.errorToString(code).c_str()); http.end(); return false; }
    File output = SD_MMC.open(path, FILE_WRITE);
    if (!output) { Serial.printf("Map: cannot write %s\n", path.c_str()); http.end(); return false; }
    WiFiClient *stream = http.getStreamPtr();
    uint8_t buffer[1024];
    int remaining = http.getSize();
    uint32_t lastData = millis();
    while (http.connected() && (remaining > 0 || remaining == -1)) {
        const size_t available = stream->available();
        if (available) {
            const size_t count = stream->readBytes(buffer, min(available, sizeof(buffer)));
            if (!count || output.write(buffer, count) != count) { Serial.printf("Map: write failed %s\n", path.c_str()); output.close(); http.end(); SD_MMC.remove(path); return false; }
            if (remaining > 0) remaining -= count;
            lastData = millis();
        } else {
            if (millis() - lastData > 15000) { Serial.printf("Map: response timeout %s\n", url.c_str()); output.close(); http.end(); SD_MMC.remove(path); return false; }
            delay(2);
        }
    }
    output.close();
    http.end();
    return SD_MMC.exists(path);
}

bool ConnectivityService::downloadTileThroughProxy(const String &path, const String &uri) {
    constexpr const char *tileHost = "tile.openstreetmap.org";
    ProxyTlsContext context{};
    mbedtls_ssl_init(&context.ssl);
    mbedtls_ssl_config_init(&context.config);
    mbedtls_ctr_drbg_init(&context.random);
    mbedtls_entropy_init(&context.entropy);
    auto cleanup = [&]() {
        mbedtls_ssl_close_notify(&context.ssl);
        context.socket.stop();
        mbedtls_ssl_free(&context.ssl);
        mbedtls_ssl_config_free(&context.config);
        mbedtls_ctr_drbg_free(&context.random);
        mbedtls_entropy_free(&context.entropy);
    };

    Serial.printf("Map: proxy %s:%u -> %s%s\n", proxyHost_, unsigned(proxyPort_), tileHost, uri.c_str());
    if (!context.socket.connect(proxyHost_, proxyPort_, 10000)) { cleanup(); return false; }
    context.socket.setTimeout(10);
    context.socket.printf("CONNECT %s:443 HTTP/1.1\r\nHost: %s:443\r\nProxy-Connection: keep-alive\r\n\r\n",
                          tileHost, tileHost);
    String status = context.socket.readStringUntil('\n');
    if (!status.startsWith("HTTP/1.1 200") && !status.startsWith("HTTP/1.0 200")) {
        Serial.printf("Map: proxy CONNECT failed: %s", status.c_str()); cleanup(); return false;
    }
    while (context.socket.connected()) {
        String header = context.socket.readStringUntil('\n');
        if (header == "\r" || !header.length()) break;
    }

    static const char *personalization = "wio-map-proxy";
    if (mbedtls_ctr_drbg_seed(&context.random, mbedtls_entropy_func, &context.entropy,
                              reinterpret_cast<const unsigned char *>(personalization), strlen(personalization)) != 0 ||
        mbedtls_ssl_config_defaults(&context.config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        cleanup(); return false;
    }
    // Public map tiles are non-sensitive display data. The proxy may terminate
    // TLS, so certificate verification is deliberately disabled for this path.
    mbedtls_ssl_conf_authmode(&context.config, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&context.config, mbedtls_ctr_drbg_random, &context.random);
    if (mbedtls_ssl_setup(&context.ssl, &context.config) != 0 ||
        mbedtls_ssl_set_hostname(&context.ssl, tileHost) != 0) { cleanup(); return false; }
    mbedtls_ssl_set_bio(&context.ssl, &context.socket, proxyTlsSend, proxyTlsReceive, nullptr);
    const uint32_t handshakeStarted = millis();
    int result = 0;
    while ((result = mbedtls_ssl_handshake(&context.ssl)) != 0) {
        if ((result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) ||
            millis() - handshakeStarted > 20000) { cleanup(); return false; }
        delay(2);
    }

    const String request = "GET " + uri + " HTTP/1.1\r\nHost: " + tileHost +
                           "\r\nUser-Agent: WioTrackerL2-MeshUI/1.0\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (sent < request.length()) {
        result = mbedtls_ssl_write(&context.ssl,
            reinterpret_cast<const unsigned char *>(request.c_str() + sent), request.length() - sent);
        if (result > 0) sent += size_t(result);
        else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) { cleanup(); return false; }
    }

    String line;
    if (!proxyTlsLine(context, line) || (!line.startsWith("HTTP/1.1 200") && !line.startsWith("HTTP/1.0 200"))) {
        Serial.printf("Map: upstream failed: %s\n", line.c_str()); cleanup(); return false;
    }
    int contentLength = -1;
    bool chunked = false;
    while (proxyTlsLine(context, line) && line.length()) {
        String lower = line; lower.toLowerCase();
        if (lower.startsWith("content-length:")) contentLength = line.substring(15).toInt();
        else if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) chunked = true;
    }

    SD_MMC.remove(path);
    File output = SD_MMC.open(path, FILE_WRITE);
    if (!output) { cleanup(); return false; }
    uint8_t buffer[1024];
    size_t total = 0;
    bool success = true;
    if (chunked) {
        while (success && proxyTlsLine(context, line)) {
            const size_t chunk = strtoul(line.c_str(), nullptr, 16);
            if (!chunk) break;
            size_t remaining = chunk;
            while (remaining) {
                const int count = proxyTlsRead(context, buffer, min(remaining, sizeof(buffer)), 15000);
                if (count <= 0 || output.write(buffer, count) != size_t(count)) { success = false; break; }
                remaining -= size_t(count); total += size_t(count);
            }
            if (success) { uint8_t crlf[2]; success = proxyTlsRead(context, crlf, 2, 15000) == 2; }
        }
    } else {
        int remaining = contentLength;
        while (success && (remaining > 0 || contentLength < 0)) {
            const size_t wanted = contentLength < 0 ? sizeof(buffer) : min(size_t(remaining), sizeof(buffer));
            const int count = proxyTlsRead(context, buffer, wanted, 15000);
            if (count == 0 && contentLength < 0) break;
            if (count <= 0 || output.write(buffer, count) != size_t(count)) { success = false; break; }
            total += size_t(count); if (remaining > 0) remaining -= count;
        }
        if (contentLength >= 0 && remaining != 0) success = false;
    }
    output.close(); cleanup();
    if (!success || total <= 100) { SD_MMC.remove(path); return false; }
    return true;
}

void ConnectivityService::runFormat() {
    void *work = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!work) { finishJob(false, "No format buffer"); return; }
    const FRESULT result = f_mkfs("0:", FM_FAT32, 0, work, 4096);
    free(work);
    SD_MMC.end(); sdMounted_ = false;
    delay(100);
    if (result != FR_OK || !mountSd(false)) { finishJob(false, "FAT32 format failed"); return; }
    jobDone_ = 1; jobProgress_ = 100;
    finishJob(true);
}

void ConnectivityService::finishJob(bool success, const char *error) {
    Serial.printf("Storage job: %s done=%u/%u %s\n", success ? "complete" : "failed",
                  unsigned(jobDone_), unsigned(jobTotal_), error ? error : "");
    if (error) strlcpy(jobError_, error, sizeof(jobError_)); else jobError_[0] = 0;
    jobState_ = success ? StorageJobState::Complete : StorageJobState::Failed;
}

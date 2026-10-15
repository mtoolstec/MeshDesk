#include "DisplaySnapshot.h"
#include "AppUI.h"
#include "KeyboardLayout.h"

#include <Preferences.h>
#include <ArduinoJson.h>
#include <src/draw/lv_image_decoder_private.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <time.h>
#include <img_converters.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include "ConnectivityService.h"
#include "Hardware.h"
#include "SystemClock.h"
#include "CommunicationService.h"
#include "MeshtasticService.h"
#include "ChatStore.h"

extern "C" {
LV_FONT_DECLARE(lv_font_ui_14);
}

AppUI appUI;

namespace {
bool dark = false;
bool zh = false;
lv_color_t bg() { return lv_color_hex(dark ? 0x000000 : 0xF2F2F7); }
lv_color_t card() { return lv_color_hex(dark ? 0x1C1C1E : 0xFFFFFF); }
lv_color_t text() { return lv_color_hex(dark ? 0xFFFFFF : 0x111111); }
lv_color_t muted() { return lv_color_hex(dark ? 0x98989D : 0x7C7C80); }
constexpr lv_color_t BLUE = LV_COLOR_MAKE(0x00, 0x7A, 0xFF);
constexpr lv_color_t GREEN = LV_COLOR_MAKE(0x34, 0xC7, 0x59);
constexpr lv_color_t ORANGE = LV_COLOR_MAKE(0xFF, 0x95, 0x00);
constexpr int16_t TIMEZONE_VALUES[] = {-720,-480,-420,-360,-300,-240,-180,0,60,120,330,345,480,540,600,720,840};
constexpr const char *TIMEZONE_OPTIONS = "UTC-12\nUTC-8\nUTC-7\nUTC-6\nUTC-5\nUTC-4\nUTC-3\nUTC+0\nUTC+1\nUTC+2\nUTC+5:30\nUTC+5:45\nUTC+8\nUTC+9\nUTC+10\nUTC+12\nUTC+14";
constexpr int SAFE_TOP = 19;
constexpr int SAFE_BOTTOM = 230;
constexpr int APP_CONTENT_TOP = 22;
// The screen sits behind a protective shell lip.  Start the Home gesture well
// above the visible indicator so a finger does not need to reach the glass edge.
constexpr int HOME_GESTURE_TOP = 204;
constexpr int HOME_GESTURE_LEFT = 70;
constexpr int HOME_GESTURE_RIGHT = 250;
const char *tr(const char *english, const char *chinese) { return zh ? chinese : english; }
const char *appName(uint8_t page) {
    static const char *const english[] = {"Home", "Chats", "Contacts", "Radio", "Settings", "Maps", "Power", "Diagnostics", "About", "Files", "GPS", "Speed"};
    static const char *const chinese[] = {"主页", "消息", "联系人", "无线电", "设置", "地图", "电源", "诊断", "关于", "文件", "GPS", "速度"};
    return page < 12 ? (zh ? chinese[page] : english[page]) : "";
}
constexpr const char *LORA_MARK = "LR";
const char *const APP_SYMBOLS[] = {LV_SYMBOL_HOME, LV_SYMBOL_ENVELOPE, LV_SYMBOL_LIST, LORA_MARK, LV_SYMBOL_SETTINGS,
                                  LV_SYMBOL_GPS, LV_SYMBOL_BATTERY_FULL, LV_SYMBOL_WARNING, LV_SYMBOL_FILE, LV_SYMBOL_DIRECTORY,
                                  LV_SYMBOL_GPS, LV_SYMBOL_CHARGE};

const char *manifestIcon(const char *name) {
    if (!strcmp(name, "cloud")) return LV_SYMBOL_UPLOAD;
    if (!strcmp(name, "radio")) return LORA_MARK;
    if (!strcmp(name, "game")) return LV_SYMBOL_PLAY;
    if (!strcmp(name, "folder")) return LV_SYMBOL_DIRECTORY;
    if (!strcmp(name, "message")) return LV_SYMBOL_ENVELOPE;
    if (!strcmp(name, "map")) return LV_SYMBOL_GPS;
    if (!strcmp(name, "settings")) return LV_SYMBOL_SETTINGS;
    if (!strcmp(name, "power")) return LV_SYMBOL_BATTERY_FULL;
    if (!strcmp(name, "warning")) return LV_SYMBOL_WARNING;
    if (!strcmp(name, "home")) return LV_SYMBOL_HOME;
    if (!strcmp(name, "speed")) return LV_SYMBOL_CHARGE;
    return LV_SYMBOL_FILE;
}

bool hasPngExtension(const char *name) {
    const size_t length = name ? strlen(name) : 0;
    return length >= 5 && !strcasecmp(name + length - 4, ".png");
}

bool jsonString(const char *json, const char *key, char *output, size_t outputSize) {
    if (!json || !key || !output || outputSize < 2) return false;
    char needle[48]; snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *cursor = strstr(json, needle);
    if (!cursor || !(cursor = strchr(cursor + strlen(needle), ':'))) return false;
    cursor = strchr(cursor, '"');
    if (!cursor) return false;
    ++cursor; size_t written = 0;
    while (*cursor && *cursor != '"' && written + 1 < outputSize) {
        if (*cursor == '\\' && cursor[1]) {
            ++cursor;
            if (*cursor == 'n') output[written++] = '\n';
            else if (*cursor == 'r') output[written++] = '\r';
            else if (*cursor == 't') output[written++] = '\t';
            else output[written++] = *cursor;
        } else output[written++] = *cursor;
        ++cursor;
    }
    output[written] = 0;
    return written > 0;
}

uint32_t jsonColor(const char *json, const char *key, uint32_t fallback) {
    char value[16]{};
    if (!jsonString(json, key, value, sizeof(value))) return fallback;
    const char *hex = value[0] == '#' ? value + 1 : value;
    char *end = nullptr; const unsigned long parsed = strtoul(hex, &end, 16);
    return end && !*end && strlen(hex) == 6 ? uint32_t(parsed) : fallback;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = unsigned(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return int64_t(era) * 146097 + int64_t(doe) - 719468;
}

void formatDateTime(uint32_t timestamp, char *output, size_t size, bool dateOnly = false, int16_t timezoneMinutes = 0) {
    time_t raw = time_t(int64_t(timestamp) + int64_t(timezoneMinutes) * 60);
    struct tm value{};
    gmtime_r(&raw, &value);
    if (dateOnly) snprintf(output, size, "%04d/%02d/%02d", value.tm_year + 1900, value.tm_mon + 1, value.tm_mday);
    else snprintf(output, size, "%04d/%02d/%02d %02d:%02d", value.tm_year + 1900, value.tm_mon + 1,
                  value.tm_mday, value.tm_hour, value.tm_min);
}

bool hasExtension(const char *path, const char *extension) {
    if (!path || !extension) return false;
    const size_t pathLength = strlen(path), extensionLength = strlen(extension);
    return pathLength >= extensionLength && !strcasecmp(path + pathLength - extensionLength, extension);
}

bool isTextFile(const char *path) {
    return hasExtension(path, ".txt") || hasExtension(path, ".log") || hasExtension(path, ".json") ||
           hasExtension(path, ".csv") || hasExtension(path, ".md");
}

bool isImageFile(const char *path) {
    return hasExtension(path, ".bmp") || hasExtension(path, ".jpg") || hasExtension(path, ".jpeg");
}

void formatFileSize(uint32_t bytes, char *output, size_t size) {
    if (bytes >= 1024U * 1024U) snprintf(output, size, "%lu.%lu MB", (unsigned long)(bytes / (1024U * 1024U)),
                                         (unsigned long)((bytes % (1024U * 1024U)) * 10U / (1024U * 1024U)));
    else if (bytes >= 1024U) snprintf(output, size, "%lu KB", (unsigned long)(bytes / 1024U));
    else snprintf(output, size, "%lu B", (unsigned long)bytes);
}

size_t jpegFileWriter(void *argument, size_t index, const void *data, size_t length) {
    File *output = static_cast<File *>(argument);
    if (!output || !*output) return 0;
    if (output->position() != index && !output->seek(index)) return 0;
    return output->write(static_cast<const uint8_t *>(data), length);
}

bool jpegDimensions(const uint8_t *data, size_t size, uint16_t &width, uint16_t &height) {
    if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    size_t offset = 2;
    while (offset + 9 < size) {
        if (data[offset] != 0xFF) { ++offset; continue; }
        while (offset < size && data[offset] == 0xFF) ++offset;
        if (offset >= size) break;
        const uint8_t marker = data[offset++];
        if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (offset + 2 > size) break;
        const uint16_t length = uint16_t(data[offset] << 8) | data[offset + 1];
        if (length < 2 || offset + length > size) break;
        if ((marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF)) {
            height = uint16_t(data[offset + 3] << 8) | data[offset + 4];
            width = uint16_t(data[offset + 5] << 8) | data[offset + 6];
            return width && height;
        }
        offset += length;
    }
    return false;
}

struct JpegDecodeContext {
    const uint8_t *encoded;
    size_t encodedSize;
    lv_draw_buf_t *target;
};

size_t jpegMemoryReader(void *argument, size_t index, uint8_t *buffer, size_t length) {
    auto *context = static_cast<JpegDecodeContext *>(argument);
    if (!context || index >= context->encodedSize) return 0;
    const size_t available = min(length, context->encodedSize - index);
    if (buffer) memcpy(buffer, context->encoded + index, available);
    return available;
}

bool jpegRgbWriter(void *argument, uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t *data) {
    auto *context = static_cast<JpegDecodeContext *>(argument);
    if (!context || !context->target || !data) return false;
    const uint16_t targetWidth = context->target->header.w, targetHeight = context->target->header.h;
    for (uint16_t row = 0; row < height && y + row < targetHeight; ++row) {
        uint16_t *destination = reinterpret_cast<uint16_t *>(context->target->data +
            uint32_t(y + row) * context->target->header.stride);
        for (uint16_t column = 0; column < width && x + column < targetWidth; ++column) {
            const uint8_t *pixel = data + (uint32_t(row) * width + column) * 3;
            destination[x + column] = uint16_t((pixel[0] >> 3) << 11) |
                                      uint16_t((pixel[1] >> 2) << 5) | uint16_t(pixel[2] >> 3);
        }
    }
    return true;
}

void createSignalBars(lv_obj_t *parent, lv_obj_t **bars, int startX, int baseline, lv_color_t color) {
    static const int heights[] = {3, 5, 8, 11};
    for (uint8_t i = 0; i < 4; ++i) {
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_set_pos(bar, startX + i * 5, baseline - heights[i]);
        lv_obj_set_size(bar, 3, heights[i]);
        lv_obj_set_style_bg_color(bar, color, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        if (bars) bars[i] = bar;
    }
}

void labelStyle(lv_obj_t *obj, const lv_font_t *font, lv_color_t color) {
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, color, 0);
}

void controlStyle(lv_obj_t *obj) {
    lv_obj_set_style_bg_color(obj, card(), 0);
    lv_obj_set_style_text_color(obj, text(), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 13, 0);
}

void setVoltage(lv_obj_t *label, uint16_t mv) {
    if (!mv) lv_label_set_text(label, "-- V");
    else lv_label_set_text_fmt(label, "%u.%02u V", unsigned(mv / 1000), unsigned((mv % 1000) / 10));
}

void setRadioPresetText(lv_obj_t *label, const RadioPreset &preset) {
    const uint32_t frequencyKHz = uint32_t(preset.frequency * 1000.0f + 0.5f);
    const uint32_t bandwidthTenths = uint32_t(preset.bandwidth * 10.0f + 0.5f);
    lv_label_set_text_fmt(label, "%s\n%lu.%03lu MHz  BW%lu.%lu  SF%u / CR%u", preset.name,
                          (unsigned long)(frequencyKHz / 1000), (unsigned long)(frequencyKHz % 1000),
                          (unsigned long)(bandwidthTenths / 10), (unsigned long)(bandwidthTenths % 10),
                          unsigned(preset.spreadingFactor), unsigned(preset.codingRate));
}

}

const char *AppUI::pageName(uint8_t page) const {
    if (page < SYSTEM_PAGE_COUNT) return appName(page);
    const uint8_t index = page - SYSTEM_PAGE_COUNT;
    if (index >= installedAppCount_) return "";
    return zh && installedApps_[index].nameZh[0] ? installedApps_[index].nameZh : installedApps_[index].name;
}

lv_obj_t *AppUI::makeCard(lv_obj_t *parent, int x, int y, int width, int height) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, card(), 0); lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 18, 0); lv_obj_set_style_pad_all(obj, 11, 0);
    // Decorative cards must not intercept a drag intended for their scrollable parent.
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t *AppUI::makePage(uint8_t column, const char *title, const char *symbol) {
    (void)title; (void)symbol;
    lv_obj_t *page = lv_tileview_add_tile(tiles_, column, 0, LV_DIR_HOR);
    lv_obj_set_size(page, 320, 240);
    lv_obj_set_style_bg_color(page, bg(), 0); lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0); lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_anim_duration(page, 180, 0);
    lv_obj_set_style_opa(page, LV_OPA_COVER, 0);
    return page;
}

bool AppUI::buildInstalledAppIcon(uint8_t index, lv_obj_t *parent) {
    if (index >= installedAppCount_ || !parent) return false;
    if (!strcmp(installedApps_[index].icon, "cloud")) {
        // A small vector cloud needs no SD asset or extra font glyph.
        const int parts[][4] = {{7, 17, 13, 13}, {15, 10, 17, 17}, {25, 18, 12, 12}, {9, 22, 26, 9}};
        for (const auto &part : parts) {
            lv_obj_t *shape = lv_obj_create(parent);
            lv_obj_set_pos(shape, part[0], part[1]); lv_obj_set_size(shape, part[2], part[3]);
            lv_obj_set_style_radius(shape, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(shape, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0); lv_obj_set_style_border_width(shape, 0, 0);
            lv_obj_remove_flag(shape, LV_OBJ_FLAG_CLICKABLE); lv_obj_remove_flag(shape, LV_OBJ_FLAG_SCROLLABLE);
        }
        return true;
    }
    if (!hasPngExtension(installedApps_[index].icon)) return false;
    char path[128];
    snprintf(path, sizeof(path), "/apps/%s/%s", installedApps_[index].id, installedApps_[index].icon);
    File input = SD_MMC.open(path, FILE_READ);
    const size_t size = input ? input.size() : 0;
    uint8_t header[24]{};
    if (!input || size < sizeof(header) || size > 16384 || input.read(header, sizeof(header)) != sizeof(header)) {
        if (input) input.close();
        return false;
    }
    static const uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const uint32_t width = uint32_t(header[16]) << 24 | uint32_t(header[17]) << 16 | uint32_t(header[18]) << 8 | header[19];
    const uint32_t height = uint32_t(header[20]) << 24 | uint32_t(header[21]) << 16 | uint32_t(header[22]) << 8 | header[23];
    if (memcmp(header, signature, sizeof(signature)) || width != 32 || height != 32 || !input.seek(0)) {
        input.close();
        return false;
    }
    uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data || input.read(data, size) != size) {
        if (data) heap_caps_free(data);
        input.close();
        return false;
    }
    input.close();
    lv_image_dsc_t &source = installedAppIconSources_[index];
    memset(&source, 0, sizeof(source));
    source.header.magic = LV_IMAGE_HEADER_MAGIC;
    source.header.cf = LV_COLOR_FORMAT_RAW;
    source.header.w = 32;
    source.header.h = 32;
    source.data_size = size;
    source.data = data;
    installedAppIconData_[index] = data;
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, &source);
    lv_obj_center(image);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_SCROLLABLE);
    return true;
}

void AppUI::begin() {
    if (!themeLoaded_) {
        Preferences p; p.begin("mesh-ui", true); darkMode_ = p.getBool("dark", false);
        chinese_ = p.getBool("zh", false);
        screenTimeoutSeconds_ = p.getUShort("screen-timeout", 60);
        timezoneMinutes_ = p.getShort("timezone", 480);
        gpsAutoTime_ = p.getBool("gps-auto-time", true);
        messageSoundEnabled_ = p.getBool("snd-master", true);
        channelSoundEnabled_ = p.getBool("snd-channel", true);
        directSoundEnabled_ = p.getBool("snd-direct", true);
        touchSoundEnabled_ = p.getBool("snd-touch", true);
        channelSoundStyle_ = p.getUChar("snd-ch-style", 0);
        directSoundStyle_ = p.getUChar("snd-dm-style", 0);
        if (channelSoundStyle_ > 4) channelSoundStyle_ = 0;
        if (directSoundStyle_ > 4) directSoundStyle_ = 0;
        fpsEnabled_ = p.getBool("dev-fps", false);
        memoryEnabled_ = p.getBool("dev-memory", false);
        const uint32_t savedTime = p.getULong("system-time", 0); p.end();
        if (savedTime >= 1700000000UL) systemClock.set(savedTime);
        themeLoaded_ = true;
    }
    dark = darkMode_; zh = chinese_; switcherVisible_ = false; indicatorHidden_ = false;
    lastUserActivityMs_ = millis(); communication.setEvents(this);
    Serial.printf("UI: screen timeout %u seconds\n", unsigned(screenTimeoutSeconds_));
    lv_obj_t *screen = lv_screen_active(); lv_obj_set_style_bg_color(screen, bg(), 0);
    tiles_ = lv_tileview_create(screen); lv_obj_set_size(tiles_, 320, 240);
    lv_obj_set_style_bg_color(tiles_, bg(), 0); lv_obj_set_style_bg_opa(tiles_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tiles_, 0, 0); lv_obj_set_style_radius(tiles_, 0, 0);
    lv_obj_set_style_pad_all(tiles_, 0, 0); lv_obj_set_scrollbar_mode(tiles_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tiles_, tileChanged, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_remove_flag(tiles_, LV_OBJ_FLAG_SCROLLABLE);
    scanInstalledApps();
    if (!recentCount_) { appOpened_[0] = true; recentOrder_[recentCount_++] = 0; }
    buildHome();
    ensureKeyboard();
    // Reserved PocketJS replacements are lazy-built so only the app currently
    // on screen owns the single QuickJS realm.
    if (!chatsOverrideAvailable_ || page_ == 1) buildMessages();
    buildNodes(); buildMap(); buildSettings();
    buildNavigation(); buildFiles();
    // GPS is lazy-built because /apps/gps may replace the native implementation
    // with a PocketJS app. This keeps the JS realm unloaded until GPS is opened.
    if (page_ == 10) buildGpsApp();
    // Third-party pages are built on first launch so a JS realm consumes no
    // memory until its SD app is opened.

    systemBar_ = lv_obj_create(screen); lv_obj_set_pos(systemBar_, 0, 0); lv_obj_set_size(systemBar_, 320, 19);
    lv_obj_set_style_bg_color(systemBar_, bg(), 0); lv_obj_set_style_border_width(systemBar_, 0, 0);
    lv_obj_set_style_pad_all(systemBar_, 0, 0); lv_obj_clear_flag(systemBar_, LV_OBJ_FLAG_SCROLLABLE);
    clockLabel_ = lv_label_create(systemBar_); lv_label_set_text(clockLabel_, "--:--");
    labelStyle(clockLabel_, &lv_font_montserrat_12, text()); lv_obj_set_pos(clockLabel_, 9, 2);
    gpsClockIcon_ = lv_label_create(systemBar_); lv_label_set_text(gpsClockIcon_, LV_SYMBOL_GPS);
    labelStyle(gpsClockIcon_, &lv_font_montserrat_12, GREEN); lv_obj_set_pos(gpsClockIcon_, 49, 2);
    lv_obj_add_flag(gpsClockIcon_, LV_OBJ_FLAG_HIDDEN);
    appTitleLabel_ = lv_label_create(systemBar_); lv_label_set_text(appTitleLabel_, pageName(page_));
    lv_obj_set_width(appTitleLabel_, 140); lv_obj_set_style_text_align(appTitleLabel_, LV_TEXT_ALIGN_CENTER, 0);
    labelStyle(appTitleLabel_, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, text());
    lv_obj_align(appTitleLabel_, LV_ALIGN_TOP_MID, 0, zh ? -5 : 2);
    statusIcons_ = lv_obj_create(systemBar_); lv_obj_set_pos(statusIcons_, 238, 0); lv_obj_set_size(statusIcons_, 79, 19);
    lv_obj_set_style_bg_opa(statusIcons_, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(statusIcons_, 0, 0);
    lv_obj_set_style_pad_all(statusIcons_, 0, 0); lv_obj_clear_flag(statusIcons_, LV_OBJ_FLAG_SCROLLABLE);
    createSignalBars(statusIcons_, loraBars_, 1, 15, text());
    wifiIcon_ = lv_label_create(statusIcons_); lv_label_set_text(wifiIcon_, LV_SYMBOL_WIFI);
    labelStyle(wifiIcon_, &lv_font_montserrat_12, text()); lv_obj_set_pos(wifiIcon_, 27, 2);
    lv_obj_add_flag(wifiIcon_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *batteryBody = lv_obj_create(statusIcons_); lv_obj_set_pos(batteryBody, 49, 4); lv_obj_set_size(batteryBody, 24, 11);
    lv_obj_set_style_bg_opa(batteryBody, LV_OPA_TRANSP, 0); lv_obj_set_style_border_color(batteryBody, text(), 0);
    lv_obj_set_style_border_width(batteryBody, 1, 0); lv_obj_set_style_radius(batteryBody, 2, 0);
    lv_obj_set_style_pad_all(batteryBody, 0, 0); lv_obj_clear_flag(batteryBody, LV_OBJ_FLAG_SCROLLABLE);
    batteryFill_ = lv_obj_create(batteryBody); lv_obj_set_pos(batteryFill_, 2, 2); lv_obj_set_size(batteryFill_, 18, 5);
    lv_obj_set_style_bg_color(batteryFill_, GREEN, 0); lv_obj_set_style_border_width(batteryFill_, 0, 0);
    lv_obj_set_style_radius(batteryFill_, 1, 0); lv_obj_clear_flag(batteryFill_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *batteryCap = lv_obj_create(statusIcons_); lv_obj_set_pos(batteryCap, 74, 7); lv_obj_set_size(batteryCap, 2, 5);
    lv_obj_set_style_bg_color(batteryCap, text(), 0); lv_obj_set_style_border_width(batteryCap, 0, 0);
    lv_obj_set_style_radius(batteryCap, 1, 0); lv_obj_clear_flag(batteryCap, LV_OBJ_FLAG_SCROLLABLE);

    // Wio Tracker L2 keeps navigation lightweight: no snapshot-based recent-app cards.
    appSwitcher_ = appCardStrip_ = nullptr;

    notificationPanel_ = lv_obj_create(screen); lv_obj_set_pos(notificationPanel_, 0, 0); lv_obj_set_size(notificationPanel_, 320, 240);
    lv_obj_set_style_bg_color(notificationPanel_, card(), 0); lv_obj_set_style_bg_opa(notificationPanel_, LV_OPA_50, 0);
    lv_obj_set_style_blur_backdrop(notificationPanel_, true, 0); lv_obj_set_style_blur_radius(notificationPanel_, 12, 0);
    lv_obj_set_style_blur_quality(notificationPanel_, LV_BLUR_QUALITY_SPEED, 0);
    lv_obj_set_style_border_width(notificationPanel_, 0, 0); lv_obj_set_style_radius(notificationPanel_, 0, 0);
    lv_obj_clear_flag(notificationPanel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *notificationTitle = lv_label_create(notificationPanel_);
    lv_label_set_text(notificationTitle, tr("Notification Center", "通知中心"));
    labelStyle(notificationTitle, zh ? &lv_font_ui_14 : &lv_font_montserrat_16, text()); lv_obj_set_pos(notificationTitle, 2, 22);
    lv_obj_t *closeNotifications = lv_button_create(notificationPanel_); lv_obj_set_pos(closeNotifications, 247, 17); lv_obj_set_size(closeNotifications, 43, 30);
    lv_obj_set_style_bg_opa(closeNotifications, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(closeNotifications, 0, 0);
    lv_obj_t *closeNotificationText = lv_label_create(closeNotifications); lv_label_set_text(closeNotificationText, LV_SYMBOL_CLOSE);
    labelStyle(closeNotificationText, &lv_font_montserrat_14, BLUE); lv_obj_center(closeNotificationText);
    lv_obj_add_event_cb(closeNotifications, clearNotificationsClicked, LV_EVENT_CLICKED, this);
    notificationList_ = lv_obj_create(notificationPanel_); lv_obj_set_pos(notificationList_, -5, 52); lv_obj_set_size(notificationList_, 298, SAFE_BOTTOM - 52);
    lv_obj_set_style_bg_opa(notificationList_, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(notificationList_, 0, 0);
    lv_obj_set_style_pad_all(notificationList_, 2, 0); lv_obj_set_flex_flow(notificationList_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(notificationList_, LV_SCROLLBAR_MODE_OFF); refreshNotifications();
    lv_obj_add_flag(notificationPanel_, LV_OBJ_FLAG_HIDDEN);

    notificationBanner_ = lv_button_create(screen); lv_obj_set_pos(notificationBanner_, 8, -55);
    lv_obj_set_size(notificationBanner_, 304, 48); lv_obj_set_style_bg_color(notificationBanner_, card(), 0);
    lv_obj_set_style_bg_opa(notificationBanner_, LV_OPA_90, 0); lv_obj_set_style_radius(notificationBanner_, 15, 0);
    lv_obj_set_style_border_width(notificationBanner_, 0, 0); lv_obj_set_style_shadow_width(notificationBanner_, 12, 0);
    lv_obj_set_style_shadow_opa(notificationBanner_, LV_OPA_30, 0); lv_obj_set_style_pad_all(notificationBanner_, 8, 0);
    notificationBannerLabel_ = lv_label_create(notificationBanner_); lv_obj_set_width(notificationBannerLabel_, 282);
    lv_label_set_long_mode(notificationBannerLabel_, LV_LABEL_LONG_DOT); labelStyle(notificationBannerLabel_, &lv_font_ui_14, text());
    lv_obj_center(notificationBannerLabel_); lv_obj_add_event_cb(notificationBanner_, notificationClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(notificationBanner_, LV_OBJ_FLAG_HIDDEN);

    dock_ = lv_obj_create(screen); lv_obj_set_pos(dock_, 0, SAFE_BOTTOM); lv_obj_set_size(dock_, 320, 240 - SAFE_BOTTOM);
    lv_obj_set_style_bg_opa(dock_, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(dock_, 0, 0);
    lv_obj_set_style_pad_all(dock_, 0, 0); lv_obj_clear_flag(dock_, LV_OBJ_FLAG_SCROLLABLE);
    homeIndicator_ = lv_obj_create(dock_); lv_obj_set_size(homeIndicator_, 70, 5);
    lv_obj_align(homeIndicator_, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_set_style_bg_color(homeIndicator_, dark ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_border_width(homeIndicator_, 0, 0); lv_obj_set_style_radius(homeIndicator_, 3, 0);
    lv_obj_clear_flag(homeIndicator_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(homeIndicator_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(homeIndicator_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    fpsLabel_ = lv_label_create(screen);
    lv_label_set_text(fpsLabel_, "FPS --");
    labelStyle(fpsLabel_, &lv_font_montserrat_12, GREEN);
    lv_obj_set_style_bg_color(fpsLabel_, dark ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(fpsLabel_, LV_OPA_70, 0); lv_obj_set_style_pad_hor(fpsLabel_, 3, 0);
    lv_obj_set_style_radius(fpsLabel_, 3, 0);
    lv_obj_align(fpsLabel_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    if (!fpsEnabled_) lv_obj_add_flag(fpsLabel_, LV_OBJ_FLAG_HIDDEN);
    memoryLabel_ = lv_label_create(screen); lv_label_set_text(memoryLabel_, "MEM --%");
    labelStyle(memoryLabel_, &lv_font_montserrat_12, BLUE);
    lv_obj_set_style_bg_color(memoryLabel_, dark ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(memoryLabel_, LV_OPA_70, 0); lv_obj_set_style_pad_hor(memoryLabel_, 3, 0);
    lv_obj_set_style_radius(memoryLabel_, 3, 0);
    lv_obj_align(memoryLabel_, LV_ALIGN_BOTTOM_LEFT, fpsEnabled_ ? 62 : 0, 0);
    if (!memoryEnabled_) lv_obj_add_flag(memoryLabel_, LV_OBJ_FLAG_HIDDEN);
    if (!fpsMonitorAttached_) {
        lv_display_add_event_cb(lv_display_get_default(), displayRefreshed, LV_EVENT_REFR_READY, this);
        fpsMonitorAttached_ = true;
    }
    fpsWindowStarted_ = millis(); fpsFrameCount_ = 0;
    if (page_ == pocketJsPage_ && pocketJsRuntime.fullscreen()) lv_obj_add_flag(systemBar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_tile(tiles_, pages_[page_], LV_ANIM_OFF);
    onNodeChanged(); onRadioState(communication.ready()); setVoltage(homeBattery_, hardware.batteryMillivolts());
}

void AppUI::rebuild() {
    if (notificationBannerTimer_) { lv_timer_delete(notificationBannerTimer_); notificationBannerTimer_ = nullptr; }
    pocketJsRuntime.unload(); pocketJsPage_ = 0xFF;
    if (volumeOverlay_ && lv_obj_is_valid(volumeOverlay_)) lv_obj_delete(volumeOverlay_);
    closeWebFilesPanel(); closeFileActions();
    if (keyboard_) lv_obj_delete(keyboard_);
    if (nodeAction_) lv_obj_delete(nodeAction_);
    closeFilePreview();
    for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
        if (recentSnapshots_[i]) { lv_draw_buf_destroy(recentSnapshots_[i]); recentSnapshots_[i] = nullptr; }
    }
    keyboard_ = nullptr; nodeAction_ = nullptr;
    volumeOverlay_ = volumeBar_ = nullptr;
    controlRebootLabel_ = controlWebFilesLabel_ = nullptr;
    controlWifiButton_ = controlGpsButton_ = nullptr;
    controlWifiIcon_ = controlLoRaIcon_ = controlGpsIcon_ = nullptr;
    if (mapFullscreenOverlay_) {
        lv_obj_set_parent(mapViewport_, mapViewportParent_);
        lv_obj_delete(mapFullscreenOverlay_); mapFullscreenOverlay_ = nullptr;
    }
    lv_obj_clean(lv_screen_active());
    for (unsigned i = 0; i < 9; ++i) {
        lv_image_cache_drop(&mapTileSources_[i]);
        if (mapTileData_[i]) heap_caps_free(mapTileData_[i]);
        mapTileData_[i] = nullptr;
    }
    mapRenderedZoom_ = 0;
    for (uint8_t i = 0; i < MAX_SD_APPS; ++i) {
        if (!installedAppIconData_[i]) continue;
        lv_image_cache_drop(&installedAppIconSources_[i]);
        heap_caps_free(installedAppIconData_[i]);
        installedAppIconData_[i] = nullptr;
        memset(&installedAppIconSources_[i], 0, sizeof(installedAppIconSources_[i]));
    }
    memset(pages_, 0, sizeof(pages_)); begin();
}

void AppUI::requestAppRefresh() { scheduleRebuild(); }

void AppUI::scheduleRebuild() {
    if (rebuildTimer_) return;
    rebuildTimer_ = lv_timer_create(rebuildTimerFinished, 80, this);
    lv_timer_set_repeat_count(rebuildTimer_, 1);
}

void AppUI::rebuildTimerFinished(lv_timer_t *timer) {
    auto *self = static_cast<AppUI *>(lv_timer_get_user_data(timer));
    self->rebuildTimer_ = nullptr;
    self->rebuild();
}

void AppUI::buildHome() {
    pages_[0] = makePage(0, "", "");
    lv_obj_add_flag(pages_[0], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(pages_[0], LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pages_[0], LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *meshWidget = makeCard(pages_[0], 10, 24, 190, 72);
    lv_obj_set_style_pad_all(meshWidget, 7, 0);
    homeStatus_ = lv_label_create(meshWidget); lv_label_set_text(homeStatus_, tr("Connecting...", "正在连接..."));
    labelStyle(homeStatus_, zh ? &lv_font_ui_14 : &lv_font_montserrat_16, text());
    lv_obj_t *identity = lv_label_create(meshWidget); lv_label_set_text_fmt(identity, "%s  |  #Public", communication.nodeName());
    labelStyle(identity, &lv_font_ui_14, muted()); lv_obj_set_pos(identity, 0, 15);
    homeNodes_ = lv_label_create(meshWidget); lv_label_set_text(homeNodes_, tr("0 nodes", "0 个节点"));
    labelStyle(homeNodes_, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, BLUE); lv_obj_set_pos(homeNodes_, 0, zh ? 34 : 39);

    lv_obj_t *battery = makeCard(pages_[0], 207, 24, 103, 72);
    lv_obj_t *batteryTitle = lv_label_create(battery); lv_label_set_text(batteryTitle, tr(LV_SYMBOL_BATTERY_FULL " Battery", "电池"));
    labelStyle(batteryTitle, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, muted());
    homeBattery_ = lv_label_create(battery); lv_label_set_text(homeBattery_, "-- V");
    labelStyle(homeBattery_, &lv_font_montserrat_20, GREEN); lv_obj_align(homeBattery_, LV_ALIGN_BOTTOM_LEFT, 0, -5);

    // Reserve enough vertical content for a future third row. The whole desktop can
    // already be dragged upward, even while only two rows of applications exist.
    const uint8_t desktopIconCount = 8 + installedAppCount_;
    const uint8_t desktopRows = (desktopIconCount + 3) / 4;
    homeGrid_ = lv_obj_create(pages_[0]); lv_obj_set_pos(homeGrid_, 5, 105);
    lv_obj_set_size(homeGrid_, 310, max(200, int(desktopRows) * 74));
    lv_obj_set_style_bg_opa(homeGrid_, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(homeGrid_, 0, 0);
    lv_obj_set_style_pad_all(homeGrid_, 0, 0); lv_obj_clear_flag(homeGrid_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(homeGrid_, LV_OBJ_FLAG_EVENT_BUBBLE); lv_obj_add_flag(homeGrid_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    struct Icon { const char *symbol; const char *name; lv_color_t color; uint8_t page; };
    const Icon icons[] = {
        {LV_SYMBOL_ENVELOPE, tr("Chats", "消息"), BLUE, 1},
        {LV_SYMBOL_LIST, tr("Contacts", "联系人"), GREEN, 2}, {LV_SYMBOL_WARNING, tr("Diagnostics", "诊断"), LV_COLOR_MAKE(0xAF,0x52,0xDE), 7},
        {LV_SYMBOL_DIRECTORY, tr("Files", "文件"), LV_COLOR_MAKE(0x59,0xA8,0xFF), 9}, {LV_SYMBOL_GPS, "GPS", LV_COLOR_MAKE(0x00,0xA8,0x66), 10},
        {LV_SYMBOL_GPS, tr("Maps", "地图"), GREEN, 5},
        {LORA_MARK, tr("Radio", "无线电"), ORANGE, 3}, {LV_SYMBOL_SETTINGS, tr("Settings", "设置"), LV_COLOR_MAKE(0x8E,0x8E,0x93), 4}
    };
    auto addJsBadge = [](lv_obj_t *button) {
        lv_obj_t *badge = lv_label_create(button); lv_label_set_text(badge, "JS");
        labelStyle(badge, &lv_font_montserrat_10, lv_color_white());
        lv_obj_set_style_bg_color(badge, BLUE, 0); lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(badge, 3, 0); lv_obj_set_style_pad_hor(badge, 2, 0); lv_obj_set_style_pad_ver(badge, 0, 0);
        lv_obj_align(badge, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    };
    for (uint8_t i = 0; i < sizeof(icons) / sizeof(icons[0]); ++i) {
        const int x = 17 + (i % 4) * 76; const int y = (i / 4) * 74;
        lv_obj_t *button = lv_button_create(homeGrid_); lv_obj_set_pos(button, x, y); lv_obj_set_size(button, 42, 42);
        lv_obj_set_style_bg_color(button, icons[i].color, 0); lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 8, 0); lv_obj_set_style_shadow_opa(button, LV_OPA_20, 0);
        if (icons[i].page == 3) createSignalBars(button, nullptr, 12, 27, lv_color_white());
        else {
            lv_obj_t *symbol = lv_label_create(button); lv_label_set_text(symbol, icons[i].symbol);
            labelStyle(symbol, &lv_font_montserrat_20, lv_color_white()); lv_obj_center(symbol);
        }
        if ((icons[i].page == 1 && chatsOverrideAvailable_ && !strcmp(chatsOverride_.type, "pocketjs")) ||
            (icons[i].page == 10 && gpsOverrideAvailable_ && !strcmp(gpsOverride_.type, "pocketjs"))) addJsBadge(button);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(uintptr_t(icons[i].page)));
        lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE); lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(button, appIconClicked, LV_EVENT_CLICKED, this);
        lv_obj_t *name = lv_label_create(homeGrid_); lv_label_set_text(name, icons[i].name);
        labelStyle(name, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, text()); lv_obj_set_width(name, 72);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(name, x - 15, y + (zh ? 42 : 44));
    }
    for (uint8_t i = 0; i < installedAppCount_; ++i) {
        const uint8_t slot = 8 + i, targetPage = SYSTEM_PAGE_COUNT + i;
        const int x = 17 + (slot % 4) * 76, y = (slot / 4) * 74;
        lv_obj_t *button = lv_button_create(homeGrid_); lv_obj_set_pos(button, x, y); lv_obj_set_size(button, 42, 42);
        lv_obj_set_style_bg_color(button, lv_color_hex(installedApps_[i].color), 0); lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_pad_all(button, 0, 0); lv_obj_set_style_shadow_width(button, 8, 0);
        lv_obj_set_style_shadow_opa(button, LV_OPA_20, 0);
        if (!buildInstalledAppIcon(i, button)) {
            lv_obj_t *symbol = lv_label_create(button); lv_label_set_text(symbol, manifestIcon(installedApps_[i].icon));
            labelStyle(symbol, &lv_font_montserrat_20, lv_color_white()); lv_obj_center(symbol);
        }
        if (!strcmp(installedApps_[i].type, "pocketjs")) addJsBadge(button);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(uintptr_t(targetPage)));
        lv_obj_add_flag(button, LV_OBJ_FLAG_EVENT_BUBBLE); lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(button, appIconClicked, LV_EVENT_CLICKED, this);
        lv_obj_t *name = lv_label_create(homeGrid_); lv_label_set_text(name, pageName(targetPage));
        labelStyle(name, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, text()); lv_obj_set_width(name, 72);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0); lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(name, x - 15, y + (zh ? 42 : 44));
    }
}

void AppUI::ensureKeyboard() {
    if (keyboard_ && lv_obj_is_valid(keyboard_)) return;
    keyboard_ = lv_keyboard_create(lv_layer_top()); configureKeyboardLayout(keyboard_); lv_obj_set_size(keyboard_, 320, 125);
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(keyboard_, card(), LV_PART_MAIN);
    lv_obj_set_style_border_color(keyboard_, muted(), LV_PART_MAIN);
    lv_obj_set_style_text_color(keyboard_, text(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard_, darkMode_ ? lv_color_hex(0x2C2C2E) : lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard_, BLUE, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard_, lv_color_white(), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard_, keyboardReady, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, keyboardReady, LV_EVENT_CANCEL, this);
}

void AppUI::buildMessages() {
    if (chatsOverrideAvailable_ && !strcmp(chatsOverride_.type, "pocketjs")) {
        if (pocketJsRuntime.active() && pocketJsPage_ != 1) destroyApp(pocketJsPage_);
        pages_[1] = makePage(1, tr("Chats", "消息"), LV_SYMBOL_ENVELOPE);
        char error[128]{};
        Serial.println("UI: launching system Chats override from /apps/chats");
        if (pocketJsRuntime.load(pages_[1], "/apps/chats", error, sizeof(error))) {
            pocketJsPage_ = 1;
            if (systemBar_ && pocketJsRuntime.fullscreen()) lv_obj_add_flag(systemBar_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        Serial.printf("UI: Chats override failed, using native fallback: %s\n", error);
        lv_obj_delete(pages_[1]); pages_[1] = nullptr; pocketJsPage_ = 0xFF;
    }
    pages_[1] = makePage(1, tr("Chats", "消息"), LV_SYMBOL_ENVELOPE);
    lv_obj_t *segment = lv_obj_create(pages_[1]); messageSegment_ = segment;
    lv_obj_set_pos(segment, 10, APP_CONTENT_TOP); lv_obj_set_size(segment, 300, 32);
    lv_obj_set_style_bg_color(segment, lv_color_hex(dark ? 0x2C2C2E : 0xE3E3E8), 0);
    lv_obj_set_style_border_width(segment, 0, 0); lv_obj_set_style_radius(segment, 9, 0); lv_obj_set_style_pad_all(segment, 2, 0);
    channelTab_ = lv_button_create(segment); lv_obj_set_pos(channelTab_, 0, 0); lv_obj_set_size(channelTab_, 146, 28);
    directTabButton_ = lv_button_create(segment); lv_obj_set_pos(directTabButton_, 150, 0); lv_obj_set_size(directTabButton_, 146, 28);
    controlStyle(channelTab_); controlStyle(directTabButton_); lv_obj_set_style_bg_opa(directTabButton_, LV_OPA_TRANSP, 0);
    lv_obj_t *groupText = lv_label_create(channelTab_); lv_label_set_text(groupText, tr("Groups  #Public", "频道  #Public")); lv_obj_center(groupText);
    lv_obj_t *directText = lv_label_create(directTabButton_); lv_label_set_text(directText, tr("Direct", "私信")); lv_obj_center(directText);
    labelStyle(groupText, &lv_font_ui_14, text()); labelStyle(directText, &lv_font_ui_14, text());
    lv_obj_add_event_cb(channelTab_, messageTabChanged, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(directTabButton_, messageTabChanged, LV_EVENT_CLICKED, this);

    channelLabel_ = lv_label_create(pages_[1]); lv_label_set_text(channelLabel_, "# Public");
    labelStyle(channelLabel_, &lv_font_montserrat_14, BLUE); lv_obj_set_pos(channelLabel_, 14, 84);
    lv_obj_add_flag(channelLabel_, LV_OBJ_FLAG_HIDDEN);
    targetDropdown_ = lv_dropdown_create(pages_[1]); lv_obj_set_pos(targetDropdown_, 164, 56); lv_obj_set_size(targetDropdown_, 146, 28);
    controlStyle(targetDropdown_); lv_obj_set_style_text_font(targetDropdown_, &lv_font_ui_14, 0);
    lv_obj_set_style_text_font(targetDropdown_, &lv_font_montserrat_14, LV_PART_INDICATOR);
    lv_obj_add_event_cb(targetDropdown_, [](lv_event_t *event) {
        static_cast<AppUI *>(lv_event_get_user_data(event))->refreshMessages();
    }, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_flag(targetDropdown_, LV_OBJ_FLAG_HIDDEN);

    messageList_ = lv_obj_create(pages_[1]); lv_obj_set_pos(messageList_, 8, 58); lv_obj_set_size(messageList_, 304, 128);
    lv_obj_set_style_bg_opa(messageList_, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(messageList_, 0, 0);
    lv_obj_set_style_pad_all(messageList_, 3, 0); lv_obj_set_style_pad_row(messageList_, 3, 0);
    lv_obj_set_flex_flow(messageList_, LV_FLEX_FLOW_COLUMN); lv_obj_set_scrollbar_mode(messageList_, LV_SCROLLBAR_MODE_OFF);

    compose_ = lv_textarea_create(pages_[1]); lv_obj_set_pos(compose_, 10, 201); lv_obj_set_size(compose_, 250, 27);
    lv_textarea_set_one_line(compose_, true); lv_textarea_set_placeholder_text(compose_, tr("Message...", "输入消息...")); controlStyle(compose_);
    lv_obj_set_style_text_font(compose_, &lv_font_ui_14, 0);
    lv_obj_set_style_pad_ver(compose_, 3, 0);
    lv_obj_set_style_bg_opa(compose_, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_color(compose_, BLUE, LV_PART_CURSOR);
    lv_obj_set_style_border_width(compose_, 1, LV_PART_CURSOR);
    lv_obj_set_style_border_side(compose_, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_add_event_cb(compose_, composeFocused, LV_EVENT_FOCUSED, this);
    sendButton_ = lv_button_create(pages_[1]); lv_obj_set_pos(sendButton_, 266, 201); lv_obj_set_size(sendButton_, 44, 27);
    lv_obj_set_style_bg_color(sendButton_, BLUE, 0); lv_obj_set_style_radius(sendButton_, 13, 0);
    lv_obj_t *send = lv_label_create(sendButton_); lv_label_set_text(send, LV_SYMBOL_UP); lv_obj_center(send);
    lv_obj_add_event_cb(sendButton_, sendClicked, LV_EVENT_CLICKED, this);

    ensureKeyboard(); lv_keyboard_set_textarea(keyboard_, compose_); refreshTargets(); refreshMessages();
}

void AppUI::buildNodes() {
    pages_[2] = makePage(2, tr("Contacts", "联系人"), LV_SYMBOL_LIST);
    nodeList_ = lv_list_create(pages_[2]); lv_obj_set_pos(nodeList_, 8, APP_CONTENT_TOP);
    lv_obj_set_size(nodeList_, 304, SAFE_BOTTOM - APP_CONTENT_TOP);
    controlStyle(nodeList_); lv_obj_set_style_pad_all(nodeList_, 4, 0);
    lv_obj_set_style_pad_bottom(nodeList_, 26, 0); lv_obj_set_style_pad_row(nodeList_, 3, 0); refreshNodes();

    nodeAction_ = lv_obj_create(lv_layer_top()); lv_obj_set_size(nodeAction_, 300, 211); lv_obj_align(nodeAction_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nodeAction_, card(), 0); lv_obj_set_style_border_width(nodeAction_, 0, 0);
    lv_obj_set_style_radius(nodeAction_, 22, 0); lv_obj_set_style_pad_all(nodeAction_, 15, 0);
    nodeActionLabel_ = lv_label_create(nodeAction_); labelStyle(nodeActionLabel_, &lv_font_ui_14, text());
    lv_obj_set_pos(nodeActionLabel_, 4, 1);
    lv_obj_t *chat = lv_button_create(nodeAction_); lv_obj_set_pos(chat, 4, 55); lv_obj_set_size(chat, 262, 40);
    lv_obj_set_style_bg_color(chat, BLUE, 0); lv_obj_set_style_radius(chat, 14, 0);
    lv_obj_t *chatText = lv_label_create(chat); lv_label_set_text(chatText, tr("Message", "发送私信"));
    labelStyle(chatText, &lv_font_ui_14, lv_color_white()); lv_obj_center(chatText);
    lv_obj_add_event_cb(chat, chatWithNode, LV_EVENT_CLICKED, this);
    lv_obj_t *contact = lv_button_create(nodeAction_); lv_obj_set_pos(contact, 4, 101); lv_obj_set_size(contact, 262, 40);
    lv_obj_set_style_bg_color(contact, GREEN, 0); lv_obj_set_style_radius(contact, 14, 0);
    contactActionLabel_ = lv_label_create(contact); lv_obj_center(contactActionLabel_);
    labelStyle(contactActionLabel_, &lv_font_ui_14, lv_color_white());
    lv_obj_add_event_cb(contact, contactAction, LV_EVENT_CLICKED, this);
    lv_obj_t *close = lv_button_create(nodeAction_); lv_obj_set_pos(close, 4, 147); lv_obj_set_size(close, 262, 34);
    controlStyle(close); lv_obj_t *closeText = lv_label_create(close); lv_label_set_text(closeText, tr("Cancel", "取消"));
    labelStyle(closeText, &lv_font_ui_14, text()); lv_obj_center(closeText);
    lv_obj_add_event_cb(close, closeNodeAction, LV_EVENT_CLICKED, this); lv_obj_add_flag(nodeAction_, LV_OBJ_FLAG_HIDDEN);
}

void AppUI::buildMap() {
    pages_[3] = makePage(3, tr("Radio", "无线电"), LORA_MARK);
    lv_obj_t *status = makeCard(pages_[3], 10, APP_CONTENT_TOP, 300, 42);
    radioLabel_ = lv_label_create(status); labelStyle(radioLabel_, &lv_font_montserrat_14, text());
    radioProtocolDropdown_ = lv_dropdown_create(pages_[3]); lv_obj_set_pos(radioProtocolDropdown_, 10, 70); lv_obj_set_size(radioProtocolDropdown_, 300, 28);
    lv_dropdown_set_options(radioProtocolDropdown_, "Off\nMeshCore\nMeshtastic"); controlStyle(radioProtocolDropdown_);
    lv_obj_add_event_cb(radioProtocolDropdown_, radioProtocolChanged, LV_EVENT_VALUE_CHANGED, this);
    radioDropdown_ = lv_dropdown_create(pages_[3]); lv_obj_set_pos(radioDropdown_, 10, 103); lv_obj_set_size(radioDropdown_, 300, 28);
    controlStyle(radioDropdown_); lv_obj_add_event_cb(radioDropdown_, radioPresetChanged, LV_EVENT_VALUE_CHANGED, this);
    radioRegionDropdown_ = lv_dropdown_create(pages_[3]); lv_obj_set_pos(radioRegionDropdown_, 10, 136); lv_obj_set_size(radioRegionDropdown_, 98, 28);
    char regions[256]{};
    for (size_t i = 0; i < meshtasticService.regionCount(); ++i) { if (i) strlcat(regions, "\n", sizeof(regions)); strlcat(regions, meshtasticService.regionName(i), sizeof(regions)); }
    lv_dropdown_set_options(radioRegionDropdown_, regions); controlStyle(radioRegionDropdown_);
    lv_obj_add_event_cb(radioRegionDropdown_, radioRegionChanged, LV_EVENT_VALUE_CHANGED, this);
    radioRoleDropdown_ = lv_dropdown_create(pages_[3]); lv_obj_set_pos(radioRoleDropdown_, 116, 136); lv_obj_set_size(radioRoleDropdown_, 194, 28);
    lv_dropdown_set_options(radioRoleDropdown_, "Client\nClient Mute"); controlStyle(radioRoleDropdown_);
    lv_obj_add_event_cb(radioRoleDropdown_, radioRoleChanged, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_t *advert = lv_button_create(pages_[3]); lv_obj_set_pos(advert, 10, 169); lv_obj_set_size(advert, 145, 34);
    lv_obj_set_style_bg_color(advert, BLUE, 0); lv_obj_set_style_radius(advert, 12, 0);
    radioAdvertLabel_ = lv_label_create(advert); labelStyle(radioAdvertLabel_, &lv_font_ui_14, lv_color_white()); lv_obj_center(radioAdvertLabel_);
    lv_obj_add_event_cb(advert, advertiseClicked, LV_EVENT_CLICKED, this);
    lv_obj_t *telemetry = makeCard(pages_[3], 163, 169, 147, 34);
    radioSignalLabel_ = lv_label_create(telemetry);
    lv_label_set_text(radioSignalLabel_, "RSSI -- / SNR --"); labelStyle(radioSignalLabel_, &lv_font_montserrat_12, muted());
    radioNote_ = lv_label_create(pages_[3]); labelStyle(radioNote_, &lv_font_montserrat_12, muted()); lv_obj_set_pos(radioNote_, 14, 208);
    refreshRadioControls();
}
void AppUI::refreshRadioControls() {
    if (!radioDropdown_) return;
    strlcpy(radioConfiguration_, communication.configurationProtocol(), sizeof(radioConfiguration_));
    char options[640]{};
    for (size_t i = 0; i < communication.radioPresetCount(); ++i) {
        if (i) strlcat(options, "\n", sizeof(options)); char option[72]; const auto p = communication.radioPreset(i);
        snprintf(option, sizeof(option), "%s | %.3f MHz", p.name, p.frequency); strlcat(options, option, sizeof(options));
    }
    lv_dropdown_set_options(radioDropdown_, options); lv_dropdown_set_selected(radioDropdown_, communication.radioPresetIndex());
    const bool mt = !strcmp(communication.configurationProtocol(), "meshtastic");
    if (mt) lv_obj_remove_flag(radioRegionDropdown_, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(radioRegionDropdown_, LV_OBJ_FLAG_HIDDEN);
    if (mt) lv_obj_remove_flag(radioRoleDropdown_, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(radioRoleDropdown_, LV_OBJ_FLAG_HIDDEN);
    lv_dropdown_set_selected(radioRoleDropdown_, meshtasticService.roleIndex());
    lv_dropdown_set_selected(radioRegionDropdown_, meshtasticService.regionIndex());
    lv_dropdown_set_selected(radioProtocolDropdown_, !strcmp(communication.protocol(), "off") ? 0 : mt ? 2 : 1);
    setRadioPresetText(radioLabel_, communication.radioPreset(communication.radioPresetIndex()));
    lv_label_set_text(radioAdvertLabel_, mt ? "Send NodeInfo" : tr("Send Advert", "发送广播"));
    lv_label_set_text(radioNote_, mt ? (meshtasticService.roleIndex() == 0 ? "Client: relay on | PKI DM" : "Client Mute: relay off | PKI DM") : "MeshCore");
}
void AppUI::radioRoleChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const bool ok = meshtasticService.selectRole(lv_dropdown_get_selected(lv_event_get_target_obj(event)));
    self->refreshRadioControls(); if (!ok) lv_label_set_text(self->radioLabel_, meshtasticService.error());
}
void AppUI::radioRegionChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const bool ok = meshtasticService.selectRegion(lv_dropdown_get_selected(lv_event_get_target_obj(event)));
    self->refreshRadioControls(); if (!ok) lv_label_set_text(self->radioLabel_, meshtasticService.error());
}

void AppUI::buildSettings() {
    pages_[4] = makePage(4, tr("Settings", "设置"), LV_SYMBOL_SETTINGS);
    settingsRoot_ = lv_obj_create(pages_[4]); lv_obj_set_pos(settingsRoot_, 8, APP_CONTENT_TOP);
    lv_obj_set_size(settingsRoot_, 304, SAFE_BOTTOM - APP_CONTENT_TOP);
    lv_obj_set_style_bg_opa(settingsRoot_, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(settingsRoot_, 0, 0);
    lv_obj_set_style_pad_all(settingsRoot_, 2, 0); lv_obj_set_scrollbar_mode(settingsRoot_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(settingsRoot_, LV_DIR_VER);
    const char *titles[] = {tr("My Settings", "个人"), tr("General", "通用"), tr("Display", "显示与亮度"),
                            tr("Radio & GPS", "无线电与定位"), "Wi-Fi", tr("Sounds", "声音与触感"),
                            tr("Storage", "存储空间"), "Developer"};
    const char *icons[] = {LV_SYMBOL_EDIT, LV_SYMBOL_SETTINGS, LV_SYMBOL_EYE_OPEN, LV_SYMBOL_GPS, LV_SYMBOL_WIFI,
                           LV_SYMBOL_AUDIO, LV_SYMBOL_SD_CARD, LV_SYMBOL_SETTINGS};
    for (uint8_t i = 0; i < 8; ++i) {
        lv_obj_t *row = lv_button_create(settingsRoot_); lv_obj_set_pos(row, 0, i * 40); lv_obj_set_size(row, 300, 35);
        controlStyle(row); lv_obj_set_style_radius(row, 12, 0); lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_t *icon = lv_label_create(row); lv_label_set_text(icon, icons[i]);
        labelStyle(icon, &lv_font_montserrat_14, BLUE);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_FLOATING); lv_obj_align(icon, LV_ALIGN_LEFT_MID, 11, 0);
        lv_obj_t *title = lv_label_create(row); lv_label_set_text(title, titles[i]);
        labelStyle(title, &lv_font_ui_14, text()); lv_obj_add_flag(title, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 43, 0);
        lv_obj_t *chevron = lv_label_create(row); lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
        labelStyle(chevron, &lv_font_montserrat_14, muted()); lv_obj_add_flag(chevron, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_set_user_data(row, reinterpret_cast<void *>(uintptr_t(i)));
        lv_obj_add_event_cb(row, settingsCategoryClicked, LV_EVENT_CLICKED, this);
    }

    settingsDetail_ = lv_obj_create(pages_[4]); lv_obj_set_pos(settingsDetail_, 8, APP_CONTENT_TOP);
    lv_obj_set_size(settingsDetail_, 304, SAFE_BOTTOM - APP_CONTENT_TOP); lv_obj_set_style_bg_opa(settingsDetail_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settingsDetail_, 0, 0); lv_obj_set_style_pad_all(settingsDetail_, 0, 0);
    lv_obj_clear_flag(settingsDetail_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *back = lv_button_create(settingsDetail_); lv_obj_set_pos(back, 0, 0); lv_obj_set_size(back, 300, 34);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *backText = lv_label_create(back); lv_label_set_text(backText, tr("< Categories", "< 设置分类"));
    labelStyle(backText, &lv_font_ui_14, BLUE); lv_obj_align(backText, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(back, settingsBackClicked, LV_EVENT_CLICKED, this);
    settingsDetailList_ = lv_obj_create(settingsDetail_); lv_obj_set_pos(settingsDetailList_, 0, 36);
    lv_obj_set_size(settingsDetailList_, 304, SAFE_BOTTOM - APP_CONTENT_TOP - 36);
    lv_obj_set_style_bg_opa(settingsDetailList_, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(settingsDetailList_, 0, 0);
    lv_obj_set_style_pad_all(settingsDetailList_, 2, 0); lv_obj_set_scrollbar_mode(settingsDetailList_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(settingsDetailList_, LV_DIR_VER);
    lv_obj_add_flag(settingsDetail_, LV_OBJ_FLAG_HIDDEN);
}

void AppUI::buildSettingsDetail(uint8_t category) {
    if (!settingsDetailList_) return;
    lv_obj_clean(settingsDetailList_);
    nameEdit_ = brightnessValue_ = gpsStatus_ = loraStatus_ = wifiStatus_ = wifiSsid_ = wifiPassword_ = nullptr;
    dateTimeEdit_ = timezoneDropdown_ = gpsTimeStatus_ = nullptr;
    wifiNetworkDropdown_ = wifiScanLabel_ = sdStatus_ = formatSdLabel_ = nullptr;
    proxyHostEdit_ = proxyPortEdit_ = proxySaveLabel_ = nullptr;
    lv_obj_t *list = settingsDetailList_;
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0); lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scroll_dir(list, LV_DIR_VER);

    lv_obj_t *identity = makeCard(list, 0, 0, 300, 52);
    nameEdit_ = lv_textarea_create(identity); lv_obj_set_pos(nameEdit_, 0, -4); lv_obj_set_size(nameEdit_, 205, 36);
    lv_textarea_set_one_line(nameEdit_, true); lv_textarea_set_text(nameEdit_, communication.nodeName()); controlStyle(nameEdit_);
    lv_obj_set_style_text_font(nameEdit_, &lv_font_ui_14, 0);
    lv_obj_set_style_bg_color(nameEdit_, bg(), 0); lv_obj_add_event_cb(nameEdit_, nameFocused, LV_EVENT_FOCUSED, this);
    lv_obj_t *save = lv_button_create(identity); lv_obj_set_pos(save, 213, -4); lv_obj_set_size(save, 65, 36);
    lv_obj_set_style_bg_color(save, BLUE, 0); lv_obj_set_style_radius(save, 12, 0);
    lv_obj_t *saveText = lv_label_create(save); lv_label_set_text(saveText, tr("Save", "保存"));
    labelStyle(saveText, &lv_font_ui_14, lv_color_white()); lv_obj_center(saveText);
    lv_obj_add_event_cb(save, nameSaved, LV_EVENT_CLICKED, this);

    lv_obj_t *broadcast = makeCard(list, 0, 59, 300, 52);
    lv_obj_t *broadcastTitle = lv_label_create(broadcast);
    lv_label_set_text(broadcastTitle, tr("Broadcast GPS position", "广播 GPS 位置"));
    labelStyle(broadcastTitle, &lv_font_ui_14, text()); lv_obj_align(broadcastTitle, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *broadcastSwitch = lv_switch_create(broadcast); lv_obj_set_size(broadcastSwitch, 48, 27);
    lv_obj_align(broadcastSwitch, LV_ALIGN_RIGHT_MID, 0, 0);
    if (meshService.broadcastLocation()) lv_obj_add_state(broadcastSwitch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(broadcastSwitch, broadcastGpsChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *lora = makeCard(list, 0, 0, 300, 48);
    lv_obj_t *loraTitle = lv_label_create(lora); lv_label_set_text(loraTitle, "LoRa"); labelStyle(loraTitle, &lv_font_ui_14, text());
    loraStatus_ = lv_label_create(lora); lv_label_set_text(loraStatus_, (strcmp(communication.protocol(), "off") != 0) ? tr("On", "开启") : tr("Off", "关闭"));
    labelStyle(loraStatus_, &lv_font_ui_14, muted()); lv_obj_set_pos(loraStatus_, 42, 3);
    lv_obj_t *loraSwitch = lv_switch_create(lora); lv_obj_set_size(loraSwitch, 48, 27); lv_obj_align(loraSwitch, LV_ALIGN_RIGHT_MID, 0, 0);
    if ((strcmp(communication.protocol(), "off") != 0)) lv_obj_add_state(loraSwitch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(loraSwitch, loraChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *power = makeCard(list, 0, 59, 300, 48);
    lv_obj_t *powerTitle = lv_label_create(power); lv_label_set_text(powerTitle, tr("Transmit Power", "发射功率")); labelStyle(powerTitle, &lv_font_ui_14, text());
    lv_obj_t *powerDrop = lv_dropdown_create(power); lv_obj_set_pos(powerDrop, 168, -5); lv_obj_set_size(powerDrop, 110, 36);
    lv_dropdown_set_options(powerDrop, "10 dBm\n14 dBm\n17 dBm\n20 dBm\n22 dBm");
    const int8_t powers[] = {10,14,17,20,22}; uint16_t pi = 4;
    for (uint16_t i = 0; i < 5; ++i) if (powers[i] == meshService.txPower()) pi = i;
    lv_dropdown_set_selected(powerDrop, pi); controlStyle(powerDrop); lv_obj_set_style_bg_color(powerDrop, bg(), 0);
    lv_obj_add_event_cb(powerDrop, txPowerChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *gps = makeCard(list, 0, 114, 300, 48);
    lv_obj_t *gpsTitle = lv_label_create(gps); lv_label_set_text(gpsTitle, "GPS"); labelStyle(gpsTitle, &lv_font_ui_14, text());
    gpsStatus_ = lv_label_create(gps); lv_label_set_text(gpsStatus_, hardware.gpsEnabled() ? tr("On", "开启") : tr("Off", "关闭"));
    labelStyle(gpsStatus_, &lv_font_ui_14, muted()); lv_obj_set_pos(gpsStatus_, 42, 3);
    lv_obj_t *gpsSwitch = lv_switch_create(gps); lv_obj_set_size(gpsSwitch, 48, 27); lv_obj_align(gpsSwitch, LV_ALIGN_RIGHT_MID, 0, 0);
    if (hardware.gpsEnabled()) lv_obj_add_state(gpsSwitch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(gpsSwitch, gpsChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *appearance = makeCard(list, 0, 169, 300, 73);
    lv_obj_t *darkTitle = lv_label_create(appearance); lv_label_set_text(darkTitle, tr("Dark Mode", "深色模式")); labelStyle(darkTitle, &lv_font_ui_14, text());
    lv_obj_t *darkSwitch = lv_switch_create(appearance); lv_obj_set_size(darkSwitch, 48, 27); lv_obj_align(darkSwitch, LV_ALIGN_TOP_RIGHT, 0, -2);
    if (darkMode_) lv_obj_add_state(darkSwitch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(darkSwitch, themeChanged, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_t *brightness = lv_slider_create(appearance); lv_obj_set_pos(brightness, 0, 41); lv_obj_set_size(brightness, 278, 8);
    const uint8_t savedBrightness = hardware.display.backlight().brightness();
    lv_slider_set_range(brightness, 20, 255); lv_slider_set_value(brightness, savedBrightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(brightness, BLUE, LV_PART_INDICATOR);
    lv_obj_add_event_cb(brightness, brightnessChanged, LV_EVENT_VALUE_CHANGED, this);
    brightnessValue_ = lv_label_create(appearance); lv_label_set_text_fmt(brightnessValue_, "%u%%", unsigned(savedBrightness) * 100 / 255);
    labelStyle(brightnessValue_, &lv_font_montserrat_12, muted()); lv_obj_set_pos(brightnessValue_, 198, 2);
    lv_obj_t *timeout = makeCard(list, 0, 249, 300, 48);
    lv_obj_t *timeoutTitle = lv_label_create(timeout); lv_label_set_text(timeoutTitle, tr("Screen Timeout", "屏幕超时"));
    labelStyle(timeoutTitle, &lv_font_ui_14, text()); lv_obj_set_pos(timeoutTitle, 0, 3);
    lv_obj_t *timeoutDrop = lv_dropdown_create(timeout); lv_obj_set_pos(timeoutDrop, 145, -5); lv_obj_set_size(timeoutDrop, 133, 36);
    lv_dropdown_set_options(timeoutDrop, tr("30 seconds\n60 seconds\n2 minutes\n5 minutes\nNever", "30 秒\n60 秒\n2 分钟\n5 分钟\n永不"));
    const uint16_t timeoutValues[] = {30, 60, 120, 300, 0}; uint16_t timeoutIndex = 1;
    for (uint16_t i = 0; i < 5; ++i) if (timeoutValues[i] == screenTimeoutSeconds_) timeoutIndex = i;
    lv_dropdown_set_selected(timeoutDrop, timeoutIndex); controlStyle(timeoutDrop); lv_obj_set_style_bg_color(timeoutDrop, bg(), 0);
    lv_obj_set_style_text_font(timeoutDrop, &lv_font_ui_14, 0);
    lv_obj_set_style_text_font(timeoutDrop, &lv_font_montserrat_14, LV_PART_INDICATOR);
    lv_dropdown_set_dir(timeoutDrop, LV_DIR_TOP);
    lv_obj_add_event_cb(timeoutDrop, screenTimeoutChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *language = makeCard(list, 0, 304, 300, 48);
    lv_obj_t *languageTitle = lv_label_create(language); lv_label_set_text(languageTitle, tr("Language", "语言"));
    labelStyle(languageTitle, &lv_font_ui_14, text()); lv_obj_set_pos(languageTitle, 0, 3);
    lv_obj_t *languageDrop = lv_dropdown_create(language); lv_obj_set_pos(languageDrop, 145, -5); lv_obj_set_size(languageDrop, 133, 36);
    lv_dropdown_set_options(languageDrop, "English\n简体中文"); lv_dropdown_set_selected(languageDrop, chinese_ ? 1 : 0);
    controlStyle(languageDrop); lv_obj_set_style_bg_color(languageDrop, bg(), 0);
    lv_obj_set_style_text_font(languageDrop, &lv_font_ui_14, 0);
    lv_obj_set_style_text_font(languageDrop, &lv_font_montserrat_14, LV_PART_INDICATOR);
    if (lv_obj_t *languageList = lv_dropdown_get_list(languageDrop))
        lv_obj_set_style_text_font(languageList, &lv_font_ui_14, 0);
    lv_dropdown_set_dir(languageDrop, LV_DIR_TOP);
    lv_obj_add_event_cb(languageDrop, languageChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *dateTime = makeCard(list, 0, 359, 300, 160);
    lv_obj_t *dateTitle = lv_label_create(dateTime); lv_label_set_text(dateTitle, tr("Date & Time", "日期与时间"));
    labelStyle(dateTitle, &lv_font_ui_14, text()); lv_obj_set_pos(dateTitle, 0, -5);
    dateTimeEdit_ = lv_textarea_create(dateTime); lv_obj_set_pos(dateTimeEdit_, 0, 23); lv_obj_set_size(dateTimeEdit_, 200, 34);
    char currentDate[32]; formatDateTime(systemClock.now(), currentDate, sizeof(currentDate), false, timezoneMinutes_);
    lv_textarea_set_text(dateTimeEdit_, currentDate); lv_textarea_set_one_line(dateTimeEdit_, true); controlStyle(dateTimeEdit_);
    lv_obj_set_style_bg_color(dateTimeEdit_, bg(), 0); lv_obj_set_style_text_font(dateTimeEdit_, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(dateTimeEdit_, dateTimeFocused, LV_EVENT_FOCUSED, this);
    lv_obj_t *dateSave = lv_button_create(dateTime); lv_obj_set_pos(dateSave, 208, 23); lv_obj_set_size(dateSave, 70, 34);
    lv_obj_set_style_bg_color(dateSave, BLUE, 0); lv_obj_set_style_radius(dateSave, 12, 0);
    lv_obj_t *dateSaveText = lv_label_create(dateSave); lv_label_set_text(dateSaveText, tr("Set", "设置"));
    labelStyle(dateSaveText, &lv_font_ui_14, lv_color_white()); lv_obj_center(dateSaveText);
    lv_obj_add_event_cb(dateSave, dateTimeSaved, LV_EVENT_CLICKED, this);
    lv_obj_t *timezoneTitle = lv_label_create(dateTime); lv_label_set_text(timezoneTitle, tr("Time Zone", "时区"));
    labelStyle(timezoneTitle, &lv_font_ui_14, text()); lv_obj_set_pos(timezoneTitle, 0, 68);
    timezoneDropdown_ = lv_dropdown_create(dateTime); lv_obj_set_pos(timezoneDropdown_, 143, 59); lv_obj_set_size(timezoneDropdown_, 135, 34);
    lv_dropdown_set_options_static(timezoneDropdown_, TIMEZONE_OPTIONS); controlStyle(timezoneDropdown_);
    lv_obj_set_style_bg_color(timezoneDropdown_, bg(), 0); lv_obj_set_style_text_font(timezoneDropdown_, &lv_font_montserrat_14, 0);
    uint16_t timezoneIndex = 12;
    for (uint16_t i = 0; i < sizeof(TIMEZONE_VALUES) / sizeof(TIMEZONE_VALUES[0]); ++i)
        if (TIMEZONE_VALUES[i] == timezoneMinutes_) timezoneIndex = i;
    lv_dropdown_set_selected(timezoneDropdown_, timezoneIndex);
    lv_obj_add_event_cb(timezoneDropdown_, timezoneChanged, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_t *autoTimeTitle = lv_label_create(dateTime); lv_label_set_text(autoTimeTitle, tr("GPS automatic time", "GPS 自动校时"));
    labelStyle(autoTimeTitle, &lv_font_ui_14, text()); lv_obj_set_pos(autoTimeTitle, 0, 107);
    gpsTimeStatus_ = lv_label_create(dateTime);
    lv_label_set_text(gpsTimeStatus_, networkClockSynced_ ? tr("Network synchronized", "网络已校时") :
                                          (gpsClockSynced_ ? tr("GPS synchronized", "GPS 已校时") :
                                           (hardware.gpsTimeValid() ? tr("Ready", "已就绪") : tr("Waiting", "等待校时"))));
    labelStyle(gpsTimeStatus_, &lv_font_ui_14, muted()); lv_obj_set_pos(gpsTimeStatus_, 0, 129);
    lv_obj_set_width(gpsTimeStatus_, 210); lv_label_set_long_mode(gpsTimeStatus_, LV_LABEL_LONG_DOT);
    lv_obj_t *autoTimeSwitch = lv_switch_create(dateTime); lv_obj_set_size(autoTimeSwitch, 48, 27); lv_obj_set_pos(autoTimeSwitch, 230, 98);
    if (gpsAutoTime_) lv_obj_add_state(autoTimeSwitch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(autoTimeSwitch, gpsAutoTimeChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *wifi = makeCard(list, 0, 359, 300, 270);
    lv_obj_t *wifiTitle = lv_label_create(wifi); lv_label_set_text(wifiTitle, "Wi-Fi");
    labelStyle(wifiTitle, &lv_font_montserrat_14, text()); lv_obj_set_pos(wifiTitle, 0, -3);
    wifiStatus_ = lv_label_create(wifi); lv_label_set_text(wifiStatus_, tr("Disconnected", "未连接"));
    labelStyle(wifiStatus_, &lv_font_ui_14, muted()); lv_obj_set_pos(wifiStatus_, 53, -7);
    lv_obj_set_width(wifiStatus_, 225); lv_label_set_long_mode(wifiStatus_, LV_LABEL_LONG_DOT);
    wifiSsid_ = lv_textarea_create(wifi); lv_obj_set_pos(wifiSsid_, 0, 24); lv_obj_set_size(wifiSsid_, 185, 34);
    lv_textarea_set_one_line(wifiSsid_, true); lv_textarea_set_placeholder_text(wifiSsid_, "SSID");
    lv_textarea_set_text(wifiSsid_, connectivity.savedSsid()); controlStyle(wifiSsid_);
    lv_obj_set_style_bg_color(wifiSsid_, bg(), 0); lv_obj_set_style_text_font(wifiSsid_, &lv_font_ui_14, 0);
    lv_obj_add_event_cb(wifiSsid_, wifiFieldFocused, LV_EVENT_FOCUSED, this);
    lv_obj_t *scan = lv_button_create(wifi); lv_obj_set_pos(scan, 193, 24); lv_obj_set_size(scan, 85, 34);
    lv_obj_set_style_bg_color(scan, GREEN, 0); lv_obj_set_style_radius(scan, 12, 0);
    wifiScanLabel_ = lv_label_create(scan); lv_label_set_text(wifiScanLabel_, tr("Scan", "扫描"));
    labelStyle(wifiScanLabel_, &lv_font_ui_14, lv_color_white()); lv_obj_center(wifiScanLabel_);
    lv_obj_add_event_cb(scan, wifiScanClicked, LV_EVENT_CLICKED, this);
    wifiNetworkDropdown_ = lv_dropdown_create(wifi); lv_obj_set_pos(wifiNetworkDropdown_, 0, 64); lv_obj_set_size(wifiNetworkDropdown_, 278, 34);
    lv_dropdown_set_options(wifiNetworkDropdown_, connectivity.savedSsid()[0] ? connectivity.savedSsid() : tr("Tap Scan to find networks", "点击扫描查找网络"));
    controlStyle(wifiNetworkDropdown_); lv_obj_set_style_bg_color(wifiNetworkDropdown_, bg(), 0);
    lv_obj_set_style_text_font(wifiNetworkDropdown_, &lv_font_ui_14, 0);
    lv_obj_set_style_text_font(wifiNetworkDropdown_, &lv_font_montserrat_14, LV_PART_INDICATOR);
    lv_obj_add_event_cb(wifiNetworkDropdown_, wifiNetworkChanged, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_t *connect = lv_button_create(wifi); lv_obj_set_pos(connect, 193, 104); lv_obj_set_size(connect, 85, 34);
    lv_obj_set_style_bg_color(connect, BLUE, 0); lv_obj_set_style_radius(connect, 12, 0);
    lv_obj_t *connectText = lv_label_create(connect); lv_label_set_text(connectText, tr("Connect", "连接"));
    labelStyle(connectText, &lv_font_ui_14, lv_color_white()); lv_obj_center(connectText);
    lv_obj_add_event_cb(connect, wifiConnectClicked, LV_EVENT_CLICKED, this);
    wifiPassword_ = lv_textarea_create(wifi); lv_obj_set_pos(wifiPassword_, 0, 104); lv_obj_set_size(wifiPassword_, 185, 34);
    lv_textarea_set_one_line(wifiPassword_, true); lv_textarea_set_password_mode(wifiPassword_, true);
    lv_textarea_set_placeholder_text(wifiPassword_, tr("Wi-Fi password", "Wi-Fi 密码")); controlStyle(wifiPassword_);
    lv_obj_set_style_bg_color(wifiPassword_, bg(), 0); lv_obj_set_style_text_font(wifiPassword_, &lv_font_ui_14, 0);
    lv_obj_add_event_cb(wifiPassword_, wifiFieldFocused, LV_EVENT_FOCUSED, this);

    lv_obj_t *proxyTitle = lv_label_create(wifi); lv_label_set_text(proxyTitle, tr("HTTP Proxy", "HTTP 代理"));
    labelStyle(proxyTitle, &lv_font_ui_14, text()); lv_obj_set_pos(proxyTitle, 0, 151);
    lv_obj_t *proxyHint = lv_label_create(wifi);
    lv_label_set_text(proxyHint, connectivity.proxyEnabled() ? tr("Enabled", "已启用") : tr("Empty host disables proxy", "主机留空即关闭代理"));
    labelStyle(proxyHint, &lv_font_ui_14, muted()); lv_obj_set_pos(proxyHint, 87, 151);
    lv_obj_set_width(proxyHint, 191); lv_label_set_long_mode(proxyHint, LV_LABEL_LONG_DOT);
    proxyHostEdit_ = lv_textarea_create(wifi); lv_obj_set_pos(proxyHostEdit_, 0, 178); lv_obj_set_size(proxyHostEdit_, 185, 34);
    lv_textarea_set_one_line(proxyHostEdit_, true); lv_textarea_set_placeholder_text(proxyHostEdit_, tr("Proxy IP / host", "代理 IP / 主机"));
    lv_textarea_set_text(proxyHostEdit_, connectivity.proxyHost()); controlStyle(proxyHostEdit_);
    lv_obj_set_style_bg_color(proxyHostEdit_, bg(), 0); lv_obj_set_style_text_font(proxyHostEdit_, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(proxyHostEdit_, wifiFieldFocused, LV_EVENT_FOCUSED, this);
    proxyPortEdit_ = lv_textarea_create(wifi); lv_obj_set_pos(proxyPortEdit_, 193, 178); lv_obj_set_size(proxyPortEdit_, 85, 34);
    lv_textarea_set_one_line(proxyPortEdit_, true); lv_textarea_set_placeholder_text(proxyPortEdit_, tr("Port", "端口"));
    char proxyPort[8]{}; if (connectivity.proxyPort()) snprintf(proxyPort, sizeof(proxyPort), "%u", unsigned(connectivity.proxyPort()));
    lv_textarea_set_text(proxyPortEdit_, proxyPort); controlStyle(proxyPortEdit_);
    lv_obj_set_style_bg_color(proxyPortEdit_, bg(), 0); lv_obj_set_style_text_font(proxyPortEdit_, &lv_font_montserrat_14, 0);
    lv_obj_add_event_cb(proxyPortEdit_, wifiFieldFocused, LV_EVENT_FOCUSED, this);
    lv_obj_t *proxySave = lv_button_create(wifi); lv_obj_set_pos(proxySave, 0, 219); lv_obj_set_size(proxySave, 278, 34);
    lv_obj_set_style_bg_color(proxySave, BLUE, 0); lv_obj_set_style_radius(proxySave, 12, 0);
    proxySaveLabel_ = lv_label_create(proxySave); lv_label_set_text(proxySaveLabel_, tr("Save Proxy", "保存代理"));
    labelStyle(proxySaveLabel_, &lv_font_ui_14, lv_color_white()); lv_obj_center(proxySaveLabel_);
    lv_obj_add_event_cb(proxySave, proxySaved, LV_EVENT_CLICKED, this);

    lv_obj_t *sounds = makeCard(list, 0, 359, 300, 203);
    const char *soundTitles[] = {tr("Message sounds", "消息通知声音"), tr("Channel #Public", "频道 #Public"),
                                 tr("Direct messages", "私信消息"), tr("Touch feedback", "触控操作声音")};
    const bool soundValues[] = {messageSoundEnabled_, channelSoundEnabled_, directSoundEnabled_, touchSoundEnabled_};
    for (uint8_t i = 0; i < 4; ++i) {
        lv_obj_t *title = lv_label_create(sounds); lv_label_set_text(title, soundTitles[i]);
        labelStyle(title, &lv_font_ui_14, text()); lv_obj_set_pos(title, 0, i * 45 + 7);
        lv_obj_t *toggle = lv_switch_create(sounds); lv_obj_set_size(toggle, 48, 27);
        lv_obj_set_pos(toggle, 230, i * 45 + 1); if (soundValues[i]) lv_obj_add_state(toggle, LV_STATE_CHECKED);
        lv_obj_set_user_data(toggle, reinterpret_cast<void *>(uintptr_t(i)));
        lv_obj_add_event_cb(toggle, soundSettingChanged, LV_EVENT_VALUE_CHANGED, this);
        if (i < 3) {
            lv_obj_t *line = lv_obj_create(sounds); lv_obj_set_pos(line, 0, i * 45 + 40); lv_obj_set_size(line, 278, 1);
            lv_obj_set_style_bg_color(line, muted(), 0); lv_obj_set_style_bg_opa(line, LV_OPA_20, 0);
            lv_obj_set_style_border_width(line, 0, 0); lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    lv_obj_t *storage = makeCard(list, 0, 478, 300, 82);
    lv_obj_t *storageTitle = lv_label_create(storage); lv_label_set_text(storageTitle, tr("SD Card (FAT32)", "SD 卡（FAT32）"));
    labelStyle(storageTitle, &lv_font_ui_14, text()); lv_obj_set_pos(storageTitle, 0, -5);
    sdStatus_ = lv_label_create(storage); lv_label_set_text(sdStatus_, tr("Checking...", "正在检测..."));
    labelStyle(sdStatus_, &lv_font_ui_14, muted()); lv_obj_set_pos(sdStatus_, 0, 25);
    lv_obj_set_width(sdStatus_, 168); lv_label_set_long_mode(sdStatus_, LV_LABEL_LONG_DOT);
    lv_obj_t *format = lv_button_create(storage); lv_obj_set_pos(format, 174, 22); lv_obj_set_size(format, 104, 38);
    lv_obj_set_style_bg_color(format, lv_palette_main(LV_PALETTE_RED), 0); lv_obj_set_style_radius(format, 12, 0);
    formatSdLabel_ = lv_label_create(format); lv_label_set_text(formatSdLabel_, tr("Format", "格式化"));
    labelStyle(formatSdLabel_, &lv_font_ui_14, lv_color_white()); lv_obj_center(formatSdLabel_);
    lv_obj_add_event_cb(format, formatSdClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *webFiles = makeCard(list, 0, 566, 300, 56);
    lv_obj_t *webButton = lv_button_create(webFiles); lv_obj_set_pos(webButton, 0, -2); lv_obj_set_size(webButton, 278, 38);
    controlStyle(webButton); lv_obj_set_style_bg_color(webButton, bg(), 0);
    lv_obj_t *webTitle = lv_label_create(webButton); lv_label_set_text(webTitle, tr("Web Files  >", "网页文件管理  >"));
    labelStyle(webTitle, &lv_font_ui_14, BLUE); lv_obj_center(webTitle);
    lv_obj_add_event_cb(webButton, ftpPanelOpened, LV_EVENT_CLICKED, this);

    lv_obj_t *developer = makeCard(list, 0, 0, 300, 134);
    lv_obj_t *developerTitle = lv_label_create(developer); lv_label_set_text(developerTitle, "FPS overlay");
    labelStyle(developerTitle, &lv_font_montserrat_14, text()); lv_obj_set_pos(developerTitle, 0, -3);
    lv_obj_t *developerDetail = lv_label_create(developer);
    lv_label_set_text(developerDetail, tr("Show frame rate in the lower-left corner", "在左下角显示全局帧率"));
    labelStyle(developerDetail, zh ? &lv_font_ui_14 : &lv_font_montserrat_10, muted()); lv_obj_set_pos(developerDetail, 0, 25);
    lv_obj_set_width(developerDetail, 220); lv_label_set_long_mode(developerDetail, LV_LABEL_LONG_DOT);
    lv_obj_t *fpsSwitch = lv_switch_create(developer); lv_obj_set_size(fpsSwitch, 48, 27);
    lv_obj_set_pos(fpsSwitch, 230, 5);
    if (fpsEnabled_) lv_obj_add_state(fpsSwitch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(fpsSwitch, fpsSettingChanged, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_t *developerLine = lv_obj_create(developer); lv_obj_set_pos(developerLine, 0, 65); lv_obj_set_size(developerLine, 278, 1);
    lv_obj_set_style_bg_color(developerLine, muted(), 0); lv_obj_set_style_bg_opa(developerLine, LV_OPA_20, 0);
    lv_obj_set_style_border_width(developerLine, 0, 0); lv_obj_remove_flag(developerLine, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *memoryTitle = lv_label_create(developer); lv_label_set_text(memoryTitle, tr("Free memory overlay", "剩余内存叠层"));
    labelStyle(memoryTitle, &lv_font_ui_14, text()); lv_obj_set_pos(memoryTitle, 0, 76);
    lv_obj_t *memoryDetail = lv_label_create(developer);
    lv_label_set_text(memoryDetail, tr("Show available RAM percentage", "显示可用 RAM 百分比"));
    labelStyle(memoryDetail, zh ? &lv_font_ui_14 : &lv_font_montserrat_10, muted()); lv_obj_set_pos(memoryDetail, 0, 102);
    lv_obj_set_width(memoryDetail, 220); lv_label_set_long_mode(memoryDetail, LV_LABEL_LONG_DOT);
    lv_obj_t *memorySwitch = lv_switch_create(developer); lv_obj_set_size(memorySwitch, 48, 27);
    lv_obj_set_pos(memorySwitch, 230, 78);
    if (memoryEnabled_) lv_obj_add_state(memorySwitch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(memorySwitch, memorySettingChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *groups[] = {identity, broadcast, language, dateTime, appearance, timeout, lora, power, gps, wifi, sounds, storage, webFiles, developer};
    for (lv_obj_t *group : groups) lv_obj_add_flag(group, LV_OBJ_FLAG_HIDDEN);
    switch (category) {
        case 0:
            lv_obj_remove_flag(identity, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(identity, 0);
            lv_obj_remove_flag(broadcast, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(broadcast, 59); break;
        case 1:
            lv_obj_remove_flag(language, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(language, 0);
            lv_obj_remove_flag(dateTime, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(dateTime, 55); break;
        case 2:
            lv_obj_remove_flag(appearance, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(appearance, 0);
            lv_obj_remove_flag(timeout, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(timeout, 80); break;
        case 3:
            lv_obj_remove_flag(lora, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(lora, 0);
            lv_obj_remove_flag(power, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(power, 55);
            lv_obj_remove_flag(gps, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(gps, 110); break;
        case 4:
            lv_obj_remove_flag(wifi, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(wifi, 0); break;
        case 5:
            lv_obj_remove_flag(sounds, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(sounds, 0); break;
        case 6:
            lv_obj_remove_flag(storage, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(storage, 0);
            lv_obj_remove_flag(webFiles, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(webFiles, 89); break;
        default:
            lv_obj_remove_flag(developer, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(developer, 0); break;
    }
}

void AppUI::buildNavigation() {
    pages_[5] = makePage(5, tr("Maps", "地图"), LV_SYMBOL_GPS);
    lv_obj_t *content = lv_obj_create(pages_[5]); lv_obj_set_pos(content, 8, APP_CONTENT_TOP);
    lv_obj_set_size(content, 304, SAFE_BOTTOM - APP_CONTENT_TOP);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 2, 0); lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    mapViewport_ = lv_obj_create(content); lv_obj_set_pos(mapViewport_, 0, 0); lv_obj_set_size(mapViewport_, 300, 142);
    lv_obj_set_style_pad_all(mapViewport_, 0, 0); lv_obj_set_style_border_width(mapViewport_, 0, 0);
    lv_obj_set_style_radius(mapViewport_, 17, 0); lv_obj_set_style_clip_corner(mapViewport_, true, 0);
    lv_obj_set_scroll_dir(mapViewport_, LV_DIR_ALL); lv_obj_set_scrollbar_mode(mapViewport_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(mapViewport_, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_remove_flag(mapViewport_, LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    mapCanvas_ = lv_obj_create(mapViewport_); lv_obj_set_size(mapCanvas_, 760, 520);
    lv_obj_set_style_pad_all(mapCanvas_, 0, 0); lv_obj_set_style_border_width(mapCanvas_, 0, 0);
    lv_obj_set_style_radius(mapCanvas_, 0, 0);
    lv_obj_remove_flag(mapCanvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mapCanvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(mapCanvas_, dark ? lv_color_hex(0x10251B) : lv_color_hex(0xDDEEDF), 0);
    auto mapButton = [&](int y, const char *glyph, lv_event_cb_t callback) {
        lv_obj_t *button = lv_button_create(content); lv_obj_set_pos(button, 256, y); lv_obj_set_size(button, 36, 32);
        lv_obj_set_style_bg_color(button, card(), 0); lv_obj_set_style_bg_opa(button, LV_OPA_90, 0);
        lv_obj_set_style_radius(button, 11, 0); lv_obj_set_style_shadow_width(button, 5, 0);
        lv_obj_t *label = lv_label_create(button); lv_label_set_text(label, glyph);
        labelStyle(label, &lv_font_montserrat_16, text()); lv_obj_center(label);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, this);
    };
    mapButton(7, "+", mapZoomInClicked); mapButton(43, "-", mapZoomOutClicked);
    mapButton(103, LV_SYMBOL_GPS, mapRecenterClicked);
    lv_obj_t *full = lv_button_create(content); lv_obj_set_pos(full, 208, 7); lv_obj_set_size(full, 40, 32);
    lv_obj_t *fullLabel = lv_label_create(full); lv_label_set_text(fullLabel, "Full"); lv_obj_center(fullLabel);
    lv_obj_add_event_cb(full, mapFullscreenClicked, LV_EVENT_CLICKED, this);
    mapZoomLabel_ = lv_label_create(content); labelStyle(mapZoomLabel_, &lv_font_montserrat_12, text());
    lv_obj_set_style_bg_color(mapZoomLabel_, card(), 0); lv_obj_set_style_bg_opa(mapZoomLabel_, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(mapZoomLabel_, 6, 0); lv_obj_set_style_pad_ver(mapZoomLabel_, 3, 0);
    lv_obj_set_style_radius(mapZoomLabel_, 7, 0); lv_obj_set_pos(mapZoomLabel_, 7, 7);

    navigationStatus_ = lv_label_create(content);
    lv_label_set_text(navigationStatus_, hardware.gpsEnabled() ? tr("Waiting for GPS fix...", "正在等待 GPS 定位...") : tr("GPS is off. Enable it in Settings.", "GPS 已关闭，请在设置中启用。"));
    labelStyle(navigationStatus_, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, text()); lv_obj_set_pos(navigationStatus_, 4, 148);

    lv_obj_t *offline = makeCard(content, 0, 197, 300, 183);
    lv_obj_t *offlineTitle = lv_label_create(offline); lv_label_set_text(offlineTitle, tr("Offline map", "离线地图"));
    labelStyle(offlineTitle, &lv_font_ui_14, text()); lv_obj_set_pos(offlineTitle, 0, -5);
    mapStorageStatus_ = lv_label_create(offline); lv_label_set_text(mapStorageStatus_, tr("Wi-Fi and SD card required", "需要 Wi-Fi 和 SD 卡"));
    labelStyle(mapStorageStatus_, &lv_font_ui_14, muted()); lv_obj_set_pos(mapStorageStatus_, 0, 23);
    lv_obj_set_width(mapStorageStatus_, 278); lv_label_set_long_mode(mapStorageStatus_, LV_LABEL_LONG_DOT);
    mapLatitudeEdit_ = lv_textarea_create(offline); lv_obj_set_pos(mapLatitudeEdit_, 0, 53); lv_obj_set_size(mapLatitudeEdit_, 134, 34);
    lv_textarea_set_one_line(mapLatitudeEdit_, true); lv_textarea_set_placeholder_text(mapLatitudeEdit_, tr("Latitude", "纬度"));
    controlStyle(mapLatitudeEdit_); lv_obj_add_event_cb(mapLatitudeEdit_, mapCoordinateFocused, LV_EVENT_FOCUSED, this);
    mapLongitudeEdit_ = lv_textarea_create(offline); lv_obj_set_pos(mapLongitudeEdit_, 144, 53); lv_obj_set_size(mapLongitudeEdit_, 134, 34);
    lv_textarea_set_one_line(mapLongitudeEdit_, true); lv_textarea_set_placeholder_text(mapLongitudeEdit_, tr("Longitude", "经度"));
    controlStyle(mapLongitudeEdit_); lv_obj_add_event_cb(mapLongitudeEdit_, mapCoordinateFocused, LV_EVENT_FOCUSED, this);
    lv_obj_t *ipButton = lv_button_create(offline); lv_obj_set_pos(ipButton, 0, 94); lv_obj_set_size(ipButton, 278, 34);
    lv_obj_set_style_bg_color(ipButton, card(), 0); lv_obj_set_style_radius(ipButton, 10, 0);
    mapIpButtonLabel_ = lv_label_create(ipButton); lv_label_set_text(mapIpButtonLabel_, tr("Use approximate IP location", "使用 IP 粗略定位"));
    labelStyle(mapIpButtonLabel_, &lv_font_ui_14, text()); lv_obj_center(mapIpButtonLabel_);
    lv_obj_add_event_cb(ipButton, mapIpClicked, LV_EVENT_CLICKED, this);
    mapZoomDropdown_ = lv_dropdown_create(offline); lv_obj_set_pos(mapZoomDropdown_, 0, 135); lv_obj_set_size(mapZoomDropdown_, 91, 34);
    lv_dropdown_set_options(mapZoomDropdown_, "Zoom 12\nZoom 13\nZoom 14\nZoom 15");
    lv_dropdown_set_selected(mapZoomDropdown_, 1); controlStyle(mapZoomDropdown_); lv_obj_set_style_bg_color(mapZoomDropdown_, bg(), 0);
    lv_obj_t *download = lv_button_create(offline); lv_obj_set_pos(download, 98, 135); lv_obj_set_size(download, 180, 34);
    lv_obj_set_style_bg_color(download, BLUE, 0); lv_obj_set_style_radius(download, 12, 0);
    mapDownloadLabel_ = lv_label_create(download); lv_label_set_text(mapDownloadLabel_, tr("Download selected area", "下载选定位置"));
    labelStyle(mapDownloadLabel_, &lv_font_ui_14, lv_color_white()); lv_obj_center(mapDownloadLabel_);
    lv_obj_add_event_cb(download, downloadMapClicked, LV_EVENT_CLICKED, this);
    Preferences saved; saved.begin("map-view", true);
    if (saved.getBool("valid", false)) {
        char value[24]; snprintf(value, sizeof(value), "%.6f", saved.getDouble("lat", 0)); lv_textarea_set_text(mapLatitudeEdit_, value);
        snprintf(value, sizeof(value), "%.6f", saved.getDouble("lon", 0)); lv_textarea_set_text(mapLongitudeEdit_, value);
        mapZoom_ = constrain(saved.getUChar("zoom", 13), uint8_t(10), uint8_t(16));
        lv_dropdown_set_selected(mapZoomDropdown_, constrain(int(mapZoom_) - 12, 0, 3));
    }
    saved.end();
    updateMapView(true);
}

bool AppUI::setMapCenter(double latitude, double longitude, uint8_t zoom) {
    if (!isfinite(latitude) || !isfinite(longitude) || latitude < -85 || latitude > 85 ||
        longitude < -180 || longitude > 180 || zoom < 10 || zoom > 16) return false;
    navigateTo(5);
    if (!mapLatitudeEdit_ || !mapLongitudeEdit_) return false;
    char value[24]; snprintf(value, sizeof(value), "%.6f", latitude); lv_textarea_set_text(mapLatitudeEdit_, value);
    snprintf(value, sizeof(value), "%.6f", longitude); lv_textarea_set_text(mapLongitudeEdit_, value);
    mapZoom_ = zoom;
    lv_dropdown_set_selected(mapZoomDropdown_, constrain(int(zoom) - 12, 0, 3));
    updateMapView(true); return true;
}

void AppUI::updateMapView(bool recenter) {
    if (!mapViewport_ || !mapCanvas_) return;
    if (mapZoomLabel_) lv_label_set_text_fmt(mapZoomLabel_, "Z%u", unsigned(mapZoom_));
    double latitude = hardware.gpsLatitude(), longitude = hardware.gpsLongitude();
    bool positioned = hardware.gpsHasFix();
    if (mapLatitudeEdit_ && mapLongitudeEdit_) {
        const char *lat = lv_textarea_get_text(mapLatitudeEdit_);
        const char *lon = lv_textarea_get_text(mapLongitudeEdit_);
        if (*lat || *lon) {
            char *le = nullptr, *oe = nullptr;
            latitude = strtod(lat, &le); longitude = strtod(lon, &oe);
            positioned = le != lat && oe != lon && !*le && !*oe && isfinite(latitude) &&
                isfinite(longitude) && latitude >= -85 && latitude <= 85 && longitude >= -180 && longitude <= 180;
        }
    }
    if (!positioned) {
        if (navigationStatus_) lv_label_set_text(navigationStatus_, tr("Enter coordinates or wait for GPS", "输入坐标或等待 GPS 定位"));
        return;
    }
    const int job = connectivity.jobState() == StorageJobState::Complete ? 1 : 0;
    if (mapSaveAt_ && millis() - mapSaveAt_ > 1200 &&
        latitude == mapRenderedLatitude_ && longitude == mapRenderedLongitude_ && mapZoom_ == mapRenderedZoom_) {
        Preferences saved; saved.begin("map-view", false);
        saved.putDouble("lat", latitude); saved.putDouble("lon", longitude); saved.putUChar("zoom", mapZoom_);
        saved.putBool("valid", true); saved.end(); mapSaveAt_ = 0;
    }
    if (latitude != mapRenderedLatitude_ || longitude != mapRenderedLongitude_ ||
        mapZoom_ != mapRenderedZoom_ || job != mapRenderedJob_) {
        lv_obj_clean(mapCanvas_);
        for (unsigned i = 0; i < 9; ++i) {
            lv_image_cache_drop(&mapTileSources_[i]);
            if (mapTileData_[i]) heap_caps_free(mapTileData_[i]);
            mapTileData_[i] = nullptr;
        }
        lv_obj_set_size(mapCanvas_, 768, 768);
        const double n = double(1UL << mapZoom_);
        const double tx = (longitude + 180.0) / 360.0 * n;
        const double ty = (1.0 - asinh(tan(latitude * PI / 180.0)) / PI) * .5 * n;
        const int cx = int(floor(tx)), cy = int(floor(ty));
        unsigned loaded = 0;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const unsigned i = (dy + 1) * 3 + dx + 1;
            const int x = (cx + dx + int(n)) % int(n), y = cy + dy;
            if (y < 0 || y >= int(n)) continue;
            char path[96]; snprintf(path, sizeof(path), "/maps/osm/%u/%d/%d.png", unsigned(mapZoom_), x, y);
            if (!SD_MMC.exists(path)) continue;
            File input = SD_MMC.open(path, FILE_READ);
            const size_t size = input ? input.size() : 0;
            if (size < 24 || size > 256 * 1024) { if (input) input.close(); continue; }
            uint8_t *data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!data || input.read(data, size) != size) { if (data) heap_caps_free(data); input.close(); continue; }
            input.close();
            static const uint8_t signature[] = {137,80,78,71,13,10,26,10};
            if (memcmp(data, signature, 8) || data[18] != 1 || data[19] != 0 || data[22] != 1 || data[23] != 0) { heap_caps_free(data); continue; }
            mapTileData_[i] = data;
            auto &source = mapTileSources_[i]; memset(&source, 0, sizeof(source));
            source.header.magic = LV_IMAGE_HEADER_MAGIC; source.header.cf = LV_COLOR_FORMAT_RAW;
            source.header.w = source.header.h = 256; source.data = data; source.data_size = size;
            lv_image_decoder_dsc_t decoded{};
            lv_image_decoder_args_t args{}; args.no_cache = true;
            if (lv_image_decoder_open(&decoded, &source, &args) != LV_RESULT_OK || !decoded.decoded) {
                lv_image_decoder_close(&decoded); heap_caps_free(data); mapTileData_[i] = nullptr; continue;
            }
            const auto *buffer = decoded.decoded;
            uint8_t *pixels = static_cast<uint8_t *>(heap_caps_malloc(buffer->data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (pixels) {
                memcpy(pixels, buffer->data, buffer->data_size);
                source.header = buffer->header;
                source.header.flags &= LV_IMAGE_FLAGS_PREMULTIPLIED;
                source.data_size = buffer->data_size;
            }
            lv_image_decoder_close(&decoded);
            heap_caps_free(data); mapTileData_[i] = pixels;
            if (!pixels) continue;
            source.data = pixels;
            // The source changed from PNG to decoded pixels: discard cached decoder/header.
            lv_image_cache_drop(&source);
            lv_obj_t *image = lv_image_create(mapCanvas_); lv_image_set_src(image, &source);
            lv_obj_set_pos(image, (dx + 1) * 256, (dy + 1) * 256); ++loaded;
        }
        mapPositionPin_ = lv_label_create(mapCanvas_); lv_label_set_text(mapPositionPin_, LV_SYMBOL_GPS);
        labelStyle(mapPositionPin_, &lv_font_montserrat_24, BLUE);
        const int px = 256 + int((tx - floor(tx)) * 256), py = 256 + int((ty - floor(ty)) * 256);
        lv_obj_set_pos(mapPositionPin_, px - 12, py - 12);
        mapCoordinateLabel_ = nullptr;
        char status[96]; snprintf(status, sizeof(status), "%.5f, %.5f | %u/9 tiles", latitude, longitude, loaded);
        if (navigationStatus_) lv_label_set_text(navigationStatus_, status);
        Serial.printf("Map view: %s zoom=%u\n", status, unsigned(mapZoom_));
        mapSaveAt_ = millis();
        mapRenderedLatitude_ = latitude; mapRenderedLongitude_ = longitude;
        mapRenderedZoom_ = mapZoom_; mapRenderedJob_ = job;
        lv_obj_update_layout(mapViewport_);
        lv_obj_scroll_to(mapViewport_, px - lv_obj_get_width(mapViewport_)/2, py - lv_obj_get_height(mapViewport_)/2, LV_ANIM_OFF);
    } else if (recenter) {
        const double n = double(1UL << mapZoom_);
        const double tx = (longitude + 180.0) / 360.0 * n;
        const double ty = (1.0 - asinh(tan(latitude * PI / 180.0)) / PI) * .5 * n;
        lv_obj_scroll_to(mapViewport_, 256 + int((tx-floor(tx))*256) - lv_obj_get_width(mapViewport_)/2,
            256 + int((ty-floor(ty))*256) - lv_obj_get_height(mapViewport_)/2, LV_ANIM_OFF);
    }
    mapCenteredOnGps_ = hardware.gpsHasFix();
}

void AppUI::buildPower() {
    pages_[6] = makePage(6, tr("Power", "电源"), LV_SYMBOL_BATTERY_FULL);
    lv_obj_t *cardTop = makeCard(pages_[6], 10, APP_CONTENT_TOP, 300, 92);
    lv_obj_t *caption = lv_label_create(cardTop); lv_label_set_text(caption, tr("Battery voltage", "电池电压"));
    labelStyle(caption, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, muted());
    powerVoltage_ = lv_label_create(cardTop); lv_label_set_text(powerVoltage_, "-- V");
    labelStyle(powerVoltage_, &lv_font_montserrat_24, GREEN); lv_obj_set_pos(powerVoltage_, 0, 22);
    powerPercent_ = lv_label_create(cardTop); lv_label_set_text(powerPercent_, "--%");
    labelStyle(powerPercent_, &lv_font_montserrat_20, text()); lv_obj_align(powerPercent_, LV_ALIGN_RIGHT_MID, 0, 5);
    powerBar_ = lv_bar_create(pages_[6]); lv_obj_set_pos(powerBar_, 14, 123); lv_obj_set_size(powerBar_, 292, 15);
    lv_bar_set_range(powerBar_, 0, 100); lv_bar_set_value(powerBar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(powerBar_, GREEN, LV_PART_INDICATOR); lv_obj_set_style_radius(powerBar_, 8, 0);
    lv_obj_t *detailsPanel = lv_obj_create(pages_[6]); lv_obj_set_pos(detailsPanel, 10, 145);
    lv_obj_set_size(detailsPanel, 300, SAFE_BOTTOM - 145);
    lv_obj_set_style_bg_opa(detailsPanel, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(detailsPanel, 0, 0);
    lv_obj_set_style_pad_all(detailsPanel, 4, 0); lv_obj_set_scroll_dir(detailsPanel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detailsPanel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *details = lv_label_create(detailsPanel);
    lv_label_set_text(details, tr("ADS1115 channel 0  |  2:1 divider\nValid range 2.50-4.50 V  |  3-sample average",
                                  "ADS1115 通道 0  |  2:1 分压\n有效范围 2.50-4.50 V  |  3 次采样平均"));
    labelStyle(details, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, muted()); lv_obj_set_pos(details, 0, 0);
    lv_obj_set_width(details, 284); lv_label_set_long_mode(details, LV_LABEL_LONG_WRAP);
}

void AppUI::buildDiagnostics() {
    pages_[7] = makePage(7, tr("Diagnostics", "诊断"), LV_SYMBOL_WARNING);
    static const char *diagnosticsSource = R"JS(
const BG = DARK ? "#000000" : "#F2F2F7";
const FG = DARK ? "#FFFFFF" : "#111111";
Pocket.setBackground(BG);
Pocket.setScrollable(true);
Pocket.text(ZH ? "系统诊断" : "SYSTEM DIAGNOSTICS", 10, 8, 300, "#AF52DE");
const telemetry = Pocket.text(ZH ? "正在收集遥测数据..." : "Collecting telemetry...", 10, 38, 300, FG);
let nextUpdate = 0;
function frame(now) {
  if (now < nextUpdate) return;
  nextUpdate = now + 1000;
  const d = Pocket.device();
  const value = ZH ?
    `MeshCore  ${d.meshOnline ? "在线" : "离线"}\n无线电  ${d.radioProfile}\n频率  ${d.frequencyKHz} kHz\n节点  ${d.nodeCount}\n发送  ${d.sentFlood} / ${d.sentDirect}\n信号  ${d.rssi} dBm / ${d.snr} dB\n电池  ${d.batteryMillivolts} mV\nGPS  ${d.gpsEnabled ? "开启" : "关闭"}, ${d.gpsBytes} 字节\n内存  ${d.memoryPercent}% (${Math.floor(d.memoryFree / 1024)}/${Math.floor(d.memoryTotal / 1024)} KiB)` :
    `MeshCore  ${d.meshOnline ? "Online" : "Offline"}\nRadio  ${d.radioProfile}\nFreq  ${d.frequencyKHz} kHz\nNodes  ${d.nodeCount}\nTX  ${d.sentFlood} / ${d.sentDirect}\nSignal  ${d.rssi} dBm / ${d.snr} dB\nBattery  ${d.batteryMillivolts} mV\nGPS  ${d.gpsEnabled ? "On" : "Off"}, ${d.gpsBytes} bytes\nMemory  ${d.memoryPercent}% (${Math.floor(d.memoryFree / 1024)}/${Math.floor(d.memoryTotal / 1024)} KiB)`;
  Pocket.setText(telemetry, value);
}
)JS";
    // PocketJS initialization already has a deep call stack. Keep the source
    // assembly buffer out of Arduino's loopTask stack to avoid a boot loop.
    static char source[4096];
    snprintf(source, sizeof(source), "const DARK=%s; const ZH=%s;\n%s", dark ? "true" : "false",
             zh ? "true" : "false", diagnosticsSource);
    char error[128]{};
    if (pocketJsRuntime.loadSource(pages_[7], "system/diagnostics.js", source,
                                   error, sizeof(error))) {
        pocketJsPage_ = 7;
        return;
    }
    pocketJsPage_ = 0xFF;
    diagnosticLabel_ = lv_label_create(pages_[7]);
    lv_label_set_text(diagnosticLabel_, error[0] ? error : tr("Diagnostics failed to start", "诊断应用启动失败"));
    labelStyle(diagnosticLabel_, &lv_font_ui_14, lv_palette_main(LV_PALETTE_RED));
    lv_obj_set_pos(diagnosticLabel_, 16, 48); lv_obj_set_width(diagnosticLabel_, 288);
}

void AppUI::buildFiles() {
    pages_[9] = makePage(9, tr("Files", "文件"), LV_SYMBOL_DIRECTORY);
    lv_obj_t *back = lv_button_create(pages_[9]); lv_obj_set_pos(back, 8, APP_CONTENT_TOP); lv_obj_set_size(back, 38, 32);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *backIcon = lv_label_create(back); lv_label_set_text(backIcon, LV_SYMBOL_LEFT);
    labelStyle(backIcon, &lv_font_montserrat_14, BLUE); lv_obj_center(backIcon);
    lv_obj_add_event_cb(back, fileBackClicked, LV_EVENT_CLICKED, this);
    filePathLabel_ = lv_label_create(pages_[9]); lv_label_set_text(filePathLabel_, currentFilePath_);
    lv_obj_set_width(filePathLabel_, 265); lv_obj_align_to(filePathLabel_, back, LV_ALIGN_OUT_RIGHT_MID, 2, -5);
    lv_label_set_long_mode(filePathLabel_, LV_LABEL_LONG_DOT); labelStyle(filePathLabel_, &lv_font_ui_14, muted());
    fileList_ = lv_list_create(pages_[9]); lv_obj_set_pos(fileList_, 8, 55); lv_obj_set_size(fileList_, 304, SAFE_BOTTOM - 55);
    controlStyle(fileList_); lv_obj_set_style_pad_all(fileList_, 4, 0); lv_obj_set_style_pad_row(fileList_, 3, 0);
    lv_obj_set_scrollbar_mode(fileList_, LV_SCROLLBAR_MODE_OFF);
    refreshFiles();
}

void AppUI::buildGpsApp() {
    if (gpsOverrideAvailable_ && !strcmp(gpsOverride_.type, "pocketjs")) {
        if (pocketJsRuntime.active() && pocketJsPage_ != 10) destroyApp(pocketJsPage_);
        pages_[10] = makePage(10, "GPS", LV_SYMBOL_GPS);
        char error[128]{};
        Serial.println("UI: launching system GPS override from /apps/gps");
        if (pocketJsRuntime.load(pages_[10], "/apps/gps", error, sizeof(error))) {
            pocketJsPage_ = 10;
            if (systemBar_ && pocketJsRuntime.fullscreen()) lv_obj_add_flag(systemBar_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        Serial.printf("UI: GPS override failed, using native fallback: %s\n", error);
        lv_obj_delete(pages_[10]); pages_[10] = nullptr; pocketJsPage_ = 0xFF;
    }
    pages_[10] = makePage(10, "GPS", LV_SYMBOL_GPS);
    const int16_t contentHeight = SAFE_BOTTOM - APP_CONTENT_TOP;
    lv_obj_t *radar = makeCard(pages_[10], 4, APP_CONTENT_TOP, 144, contentHeight);
    lv_obj_set_style_bg_color(radar, dark ? lv_color_hex(0x0B2218) : lv_color_hex(0xE0F3E7), 0);
    lv_obj_set_style_pad_all(radar, 0, 0); lv_obj_remove_flag(radar, LV_OBJ_FLAG_SCROLLABLE);

    const int16_t centerX = 72, centerY = 78;
    for (int16_t diameter : {128, 86, 44}) {
        lv_obj_t *ring = lv_obj_create(radar); lv_obj_set_size(ring, diameter, diameter);
        lv_obj_set_pos(ring, centerX - diameter / 2, centerY - diameter / 2);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0); lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 1, 0); lv_obj_set_style_border_color(ring, GREEN, 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_50, 0); lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *horizontal = lv_obj_create(radar); lv_obj_set_pos(horizontal, 8, centerY); lv_obj_set_size(horizontal, 128, 1);
    lv_obj_t *vertical = lv_obj_create(radar); lv_obj_set_pos(vertical, centerX, 14); lv_obj_set_size(vertical, 1, 128);
    for (lv_obj_t *line : {horizontal, vertical}) {
        lv_obj_set_style_bg_color(line, GREEN, 0); lv_obj_set_style_bg_opa(line, LV_OPA_40, 0);
        lv_obj_set_style_border_width(line, 0, 0); lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    const struct { const char *text; int16_t x; int16_t y; } directions[] = {
        {"N", 68, 1}, {"E", 133, 72}, {"S", 68, 143}, {"W", 2, 72}
    };
    for (const auto &direction : directions) {
        lv_obj_t *label = lv_label_create(radar); lv_label_set_text(label, direction.text);
        labelStyle(label, &lv_font_montserrat_12, muted()); lv_obj_set_pos(label, direction.x, direction.y);
    }
    for (uint8_t i = 0; i < Hardware::MAX_GPS_SATELLITES; ++i) {
        gpsRadarDots_[i] = lv_obj_create(radar); lv_obj_set_size(gpsRadarDots_[i], 8, 8);
        lv_obj_set_style_radius(gpsRadarDots_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(gpsRadarDots_[i], 1, 0); lv_obj_set_style_border_color(gpsRadarDots_[i], card(), 0);
        lv_obj_add_flag(gpsRadarDots_[i], LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(gpsRadarDots_[i], LV_OBJ_FLAG_CLICKABLE);
    }
    gpsRadarStatus_ = lv_label_create(radar); lv_obj_set_width(gpsRadarStatus_, 132);
    labelStyle(gpsRadarStatus_, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, text());
    lv_obj_set_style_text_align(gpsRadarStatus_, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_pos(gpsRadarStatus_, 6, 164);

    lv_obj_t *table = makeCard(pages_[10], 152, APP_CONTENT_TOP, 164, contentHeight);
    lv_obj_set_style_pad_all(table, 6, 0); lv_obj_remove_flag(table, LV_OBJ_FLAG_SCROLLABLE);
    gpsInfoLabel_ = lv_label_create(table); lv_obj_set_width(gpsInfoLabel_, 152);
    labelStyle(gpsInfoLabel_, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, text());
    lv_obj_set_style_text_align(gpsInfoLabel_, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_pos(gpsInfoLabel_, 0, -1);

    lv_obj_t *satelliteList = lv_obj_create(table); lv_obj_set_pos(satelliteList, 0, 24);
    lv_obj_set_size(satelliteList, 152, contentHeight - 36);
    lv_obj_set_style_bg_opa(satelliteList, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(satelliteList, 0, 0);
    lv_obj_set_style_pad_all(satelliteList, 0, 0); lv_obj_set_scroll_dir(satelliteList, LV_DIR_VER);
    lv_obj_add_flag(satelliteList, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(satelliteList, LV_SCROLLBAR_MODE_AUTO);
    for (uint8_t i = 0; i < Hardware::MAX_GPS_SATELLITES; ++i) {
        lv_obj_t *row = lv_obj_create(satelliteList); gpsSatelliteRows_[i] = row;
        lv_obj_set_pos(row, 0, i * 40); lv_obj_set_size(row, 146, 37);
        lv_obj_set_style_radius(row, 9, 0); lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_color(row, dark ? lv_color_hex(0x2C2C2E) : lv_color_hex(0xF2F2F7), 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        gpsSatelliteLabels_[i] = lv_label_create(row); lv_obj_set_pos(gpsSatelliteLabels_[i], 5, 3);
        lv_obj_set_width(gpsSatelliteLabels_[i], 50); lv_label_set_long_mode(gpsSatelliteLabels_[i], LV_LABEL_LONG_DOT);
        labelStyle(gpsSatelliteLabels_[i], &lv_font_montserrat_12, text());
        gpsSatelliteDetailLabels_[i] = lv_label_create(row); lv_obj_set_pos(gpsSatelliteDetailLabels_[i], 5, 20);
        labelStyle(gpsSatelliteDetailLabels_[i], &lv_font_montserrat_12, muted());
        gpsSatelliteBars_[i] = lv_bar_create(row); lv_obj_set_pos(gpsSatelliteBars_[i], 59, 8);
        lv_obj_set_size(gpsSatelliteBars_[i], 48, 8); lv_bar_set_range(gpsSatelliteBars_[i], 0, 50);
        lv_obj_set_style_radius(gpsSatelliteBars_[i], 4, 0); lv_obj_set_style_radius(gpsSatelliteBars_[i], 4, LV_PART_INDICATOR);
        gpsSatelliteSnrLabels_[i] = lv_label_create(row); lv_obj_set_pos(gpsSatelliteSnrLabels_[i], 111, 2);
        lv_obj_set_width(gpsSatelliteSnrLabels_[i], 31);
        lv_obj_set_style_text_align(gpsSatelliteSnrLabels_[i], LV_TEXT_ALIGN_RIGHT, 0);
        labelStyle(gpsSatelliteSnrLabels_[i], &lv_font_montserrat_12, text());
    }
    updateGpsApp();
}

void AppUI::updateGpsApp() {
    if (!gpsInfoLabel_) return;
    const uint8_t visible = hardware.gpsSatelliteInfoCount();
    uint8_t rowIndex = 0, anglePending = 0;
    for (uint8_t i = 0; i < Hardware::MAX_GPS_SATELLITES; ++i) {
        if (!gpsRadarDots_[i]) continue;
        const Hardware::GpsSatellite &sat = hardware.gpsSatelliteInfo(i);
        if (i >= visible || !sat.id) {
            lv_obj_add_flag(gpsRadarDots_[i], LV_OBJ_FLAG_HIDDEN);
            if (gpsSatelliteRows_[i]) lv_obj_add_flag(gpsSatelliteRows_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (sat.azimuth || sat.elevation) {
            const float radius = 62.0f * (90.0f - sat.elevation) / 90.0f;
            const float radians = float(sat.azimuth) * float(PI) / 180.0f;
            lv_obj_set_pos(gpsRadarDots_[i], 68 + int16_t(sinf(radians) * radius), 74 - int16_t(cosf(radians) * radius));
            lv_obj_set_style_bg_color(gpsRadarDots_[i], sat.snr >= 35 ? GREEN : (sat.snr >= 20 ? ORANGE : BLUE), 0);
            lv_obj_remove_flag(gpsRadarDots_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            ++anglePending;
            lv_obj_add_flag(gpsRadarDots_[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (gpsSatelliteRows_[i]) {
            const char *system = "GPS"; uint16_t prn = sat.id;
            if (sat.system == 'L' || (sat.id >= 65 && sat.id <= 96)) { system = "GLO"; prn = sat.id - 64; }
            else if (sat.id >= 193 && sat.id <= 200) { system = "QZS"; prn = sat.id - 192; }
            else if (sat.system == 'B' || (sat.id >= 201 && sat.id <= 237)) { system = "BDS"; prn = sat.id >= 201 ? sat.id - 200 : sat.id; }
            else if (sat.id >= 301 && sat.id <= 336) { system = "GAL"; prn = sat.id - 300; }
            else if (sat.id >= 33 && sat.id <= 64) system = "SBAS";
            char id[12], detail[24];
            snprintf(id, sizeof(id), "%s %02u", system, unsigned(prn));
            if (!sat.azimuth && !sat.elevation) strlcpy(detail, "Az --  El --", sizeof(detail));
            else snprintf(detail, sizeof(detail), "Az %03u  El %02u", unsigned(sat.azimuth), unsigned(sat.elevation));
            lv_label_set_text(gpsSatelliteLabels_[i], id);
            lv_label_set_text(gpsSatelliteDetailLabels_[i], detail);
            lv_label_set_text_fmt(gpsSatelliteSnrLabels_[i], "S%u", unsigned(sat.snr));
            lv_bar_set_value(gpsSatelliteBars_[i], min(uint8_t(50), sat.snr), LV_ANIM_ON);
            lv_obj_set_style_bg_color(gpsSatelliteBars_[i], sat.snr >= 35 ? GREEN : (sat.snr >= 20 ? ORANGE : muted()), LV_PART_INDICATOR);
            lv_obj_set_y(gpsSatelliteRows_[i], rowIndex++ * 40);
            lv_obj_remove_flag(gpsSatelliteRows_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    char radarStatus[96];
    if (hardware.gpsHasFix())
        snprintf(radarStatus, sizeof(radarStatus), zh ? "已定位  %u/%u\n%.4f, %.4f\n%u 颗等待角度" : "FIXED  %u/%u\n%.4f, %.4f\n%u angles pending",
                 hardware.gpsSatellitesUsed(), hardware.gpsSatellites(), hardware.gpsLatitude(), hardware.gpsLongitude(), anglePending);
    else
        snprintf(radarStatus, sizeof(radarStatus), zh ? "正在搜星  %u/%u\n%u 颗等待角度" : "SEARCHING  %u/%u\n%u angles pending",
                 hardware.gpsSatellitesUsed(), hardware.gpsSatellites(), anglePending);
    if (gpsRadarStatus_) lv_label_set_text(gpsRadarStatus_, radarStatus);
    lv_label_set_text_fmt(gpsInfoLabel_, zh ? "使用 %u/%u  DOP %.1f" : "USED %u/%u  DOP %.1f",
                          hardware.gpsSatellitesUsed(), hardware.gpsSatellites(), hardware.gpsHdop());
}

void AppUI::buildSpeedometerApp() {
    pages_[11] = makePage(11, tr("Speed", "速度"), LV_SYMBOL_CHARGE);

    lv_obj_t *panel = makeCard(pages_[11], 5, APP_CONTENT_TOP, 310, SAFE_BOTTOM - APP_CONTENT_TOP);
    lv_obj_set_style_radius(panel, 20, 0);
    lv_obj_set_style_bg_color(panel, dark ? lv_color_hex(0x051218) : lv_color_hex(0xEAFBFD), 0);
    lv_obj_set_style_bg_grad_color(panel, dark ? lv_color_hex(0x102532) : lv_color_hex(0xD8F6FA), 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x00C7D9), 0);
    lv_obj_set_style_border_opa(panel, dark ? LV_OPA_60 : LV_OPA_30, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    speedScale_ = lv_scale_create(panel);
    lv_obj_set_pos(speedScale_, 53, 1); lv_obj_set_size(speedScale_, 204, 204);
    lv_scale_set_mode(speedScale_, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(speedScale_, 0, 200);
    lv_scale_set_total_tick_count(speedScale_, 41);
    lv_scale_set_major_tick_every(speedScale_, 5);
    lv_scale_set_angle_range(speedScale_, 250);
    lv_scale_set_rotation(speedScale_, 145);
    lv_scale_set_label_show(speedScale_, true);
    lv_obj_set_style_bg_opa(speedScale_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(speedScale_, dark ? lv_color_hex(0xB9F7FF) : lv_color_hex(0x147380), LV_PART_INDICATOR);
    lv_obj_set_style_text_font(speedScale_, &lv_font_montserrat_12, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(speedScale_, lv_color_hex(0x00D7E8), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(speedScale_, 3, LV_PART_INDICATOR);
    lv_obj_set_style_length(speedScale_, 13, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(speedScale_, dark ? lv_color_hex(0x2E6872) : lv_color_hex(0x78C7CE), LV_PART_ITEMS);
    lv_obj_set_style_line_width(speedScale_, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_opa(speedScale_, LV_OPA_70, LV_PART_ITEMS);
    lv_obj_set_style_length(speedScale_, 7, LV_PART_ITEMS);

    speedNeedle_ = lv_line_create(speedScale_);
    lv_obj_set_style_line_color(speedNeedle_, lv_color_hex(0xFF375F), 0);
    lv_obj_set_style_line_width(speedNeedle_, 5, 0);
    lv_obj_set_style_line_rounded(speedNeedle_, true, 0);

    lv_obj_t *hub = lv_obj_create(panel); lv_obj_set_size(hub, 15, 15);
    lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(0xFF375F), 0);
    lv_obj_set_style_border_width(hub, 3, 0);
    lv_obj_set_style_border_color(hub, dark ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x073B46), 0);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_SCROLLABLE); lv_obj_remove_flag(hub, LV_OBJ_FLAG_CLICKABLE);

    speedValue_ = lv_label_create(panel); lv_obj_set_width(speedValue_, 136);
    lv_label_set_text(speedValue_, "--"); labelStyle(speedValue_, &lv_font_montserrat_36, dark ? lv_color_white() : lv_color_hex(0x062F38));
    lv_obj_set_style_text_align(speedValue_, LV_TEXT_ALIGN_CENTER, 0); lv_obj_set_pos(speedValue_, 87, 108);
    lv_obj_t *unit = lv_label_create(panel); lv_label_set_text(unit, "km/h");
    labelStyle(unit, &lv_font_montserrat_12, lv_color_hex(0x00AFC0)); lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 148);
    speedStatus_ = lv_label_create(panel); lv_obj_set_width(speedStatus_, 260);
    labelStyle(speedStatus_, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, dark ? lv_color_hex(0x8EDDE5) : lv_color_hex(0x397D84));
    lv_obj_set_style_text_align(speedStatus_, LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(speedStatus_, LV_ALIGN_BOTTOM_MID, 0, -7);
}

void AppUI::updateSpeedometerApp() {
    if (!speedScale_ || !speedNeedle_ || !speedValue_) return;
    const bool fixed = hardware.gpsHasFix();
    const int speed = fixed ? constrain(int(lroundf(hardware.gpsSpeedKph())), 0, 199) : 0;
    lv_scale_set_line_needle_value(speedScale_, speedNeedle_, 76, speed);
    if (fixed) lv_label_set_text_fmt(speedValue_, "%d", speed);
    else lv_label_set_text(speedValue_, "--");
    if (!speedStatus_) return;
    if (!hardware.gpsEnabled()) lv_label_set_text(speedStatus_, tr("GPS is off", "GPS 已关闭"));
    else if (!fixed) lv_label_set_text_fmt(speedStatus_, tr("ACQUIRING GPS  |  %u SAT", "正在定位  |  %u 颗卫星"), hardware.gpsSatellites());
    else lv_label_set_text_fmt(speedStatus_, tr("GPS LOCK  |  %u SAT  |  %03d deg", "GPS 已定位  |  %u 颗卫星  |  %03d 度"),
                               hardware.gpsSatellitesUsed(), int(lroundf(hardware.gpsCourseDegrees())) % 360);
}

void AppUI::scanInstalledApps() {
    installedAppCount_ = 0;
    memset(installedApps_, 0, sizeof(installedApps_));
    memset(&gpsOverride_, 0, sizeof(gpsOverride_));
    gpsOverrideAvailable_ = false;
    memset(&chatsOverride_, 0, sizeof(chatsOverride_));
    chatsOverrideAvailable_ = false;
    if (!connectivity.sdMounted()) return;
    SD_MMC.mkdir("/apps");
    File root = SD_MMC.open("/apps");
    if (!root || !root.isDirectory()) return;
    File folder;
    while ((folder = root.openNextFile())) {
        if (!folder.isDirectory()) { folder.close(); continue; }
        const char *raw = folder.name(); const char *base = strrchr(raw, '/'); base = base ? base + 1 : raw;
        // File::name() is backed by the File handle. Preserve the directory ID
        // before close() invalidates that storage.
        if (!base[0] || base[0] == '.' || strlen(base) > 24) { folder.close(); continue; }
        char appId[25]; strlcpy(appId, base, sizeof(appId));
        char manifestPath[96]; snprintf(manifestPath, sizeof(manifestPath), "/apps/%s/app.json", appId);
        folder.close();
        File manifest = SD_MMC.open(manifestPath, FILE_READ);
        if (!manifest || !manifest.size() || manifest.size() > 4096) { if (manifest) manifest.close(); continue; }
        const size_t length = manifest.size();
        char *json = static_cast<char *>(heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!json) { manifest.close(); continue; }
        const size_t read = manifest.readBytes(json, length); json[read] = 0; manifest.close();
        // Ignore the legacy firmware-generated Trail Notes demo, including
        // copies renamed from /apps/example to another directory.
        if (strstr(json, "\"title\": \"SD Card Example\"")) { free(json); continue; }
        JsonDocument manifestDoc;
        if (deserializeJson(manifestDoc, json, read) || !manifestDoc.is<JsonObject>()) {
            Serial.printf("UI: invalid manifest /apps/%s/app.json\n", appId); free(json); continue;
        }
        const char *declaredId = manifestDoc["id"].as<const char *>();
        if (declaredId && strcmp(declaredId, appId)) {
            Serial.printf("UI: manifest ID differs from directory /apps/%s\n", appId); free(json); continue;
        }
        InstalledApp candidate{}; strlcpy(candidate.id, appId, sizeof(candidate.id));
        jsonString(json, "type", candidate.type, sizeof(candidate.type));
        const bool valid = jsonString(json, "name", candidate.name, sizeof(candidate.name));
        jsonString(json, "name_zh", candidate.nameZh, sizeof(candidate.nameZh));
        jsonString(json, "icon", candidate.icon, sizeof(candidate.icon));
        jsonString(json, "entry", candidate.entry, sizeof(candidate.entry));
        jsonString(json, "title", candidate.title, sizeof(candidate.title));
        jsonString(json, "title_zh", candidate.titleZh, sizeof(candidate.titleZh));
        jsonString(json, "body", candidate.body, sizeof(candidate.body));
        jsonString(json, "body_zh", candidate.bodyZh, sizeof(candidate.bodyZh));
        candidate.color = jsonColor(json, "color", 0x007AFF); free(json);
        if (!valid) continue;
        if (!candidate.icon[0]) strlcpy(candidate.icon, "file", sizeof(candidate.icon));
        if (!candidate.type[0]) strlcpy(candidate.type, "document", sizeof(candidate.type));
        if (!candidate.entry[0]) strlcpy(candidate.entry, "app.js", sizeof(candidate.entry));
        if (!candidate.title[0]) strlcpy(candidate.title, candidate.name, sizeof(candidate.title));
        if (!candidate.body[0]) strlcpy(candidate.body, "Installed from SD card", sizeof(candidate.body));
        // The reserved /apps/gps package upgrades the built-in GPS slot rather
        // than adding a duplicate desktop icon. Removing it restores native GPS.
        if (!strcasecmp(candidate.id, "gps") && !strcmp(candidate.type, "pocketjs")) {
            gpsOverride_ = candidate;
            gpsOverrideAvailable_ = true;
            Serial.println("UI: SD app id=gps will override the native GPS page");
            continue;
        }
        // /apps/chats replaces the built-in Chats slot. Removing the package
        // restores the native implementation on the next app rescan.
        if (!strcasecmp(candidate.id, "chats") && !strcmp(candidate.type, "pocketjs")) {
            chatsOverride_ = candidate;
            chatsOverrideAvailable_ = true;
            Serial.println("UI: SD app id=chats will override the native Chats page");
            continue;
        }
        if (installedAppCount_ >= MAX_SD_APPS) continue;
        installedApps_[installedAppCount_++] = candidate;
        Serial.printf("UI: SD app id=%s type=%s path=/apps/%s\n", candidate.id, candidate.type, candidate.id);
    }
    root.close();
    for (uint8_t i = 1; i < installedAppCount_; ++i) {
        InstalledApp value = installedApps_[i]; uint8_t position = i;
        while (position && strcasecmp(value.name, installedApps_[position - 1].name) < 0) {
            installedApps_[position] = installedApps_[position - 1]; --position;
        }
        installedApps_[position] = value;
    }
    Serial.printf("UI: discovered %u SD app(s)\n", unsigned(installedAppCount_));
}

void AppUI::buildInstalledApp(uint8_t index) {
    if (index >= installedAppCount_) return;
    if (!strcmp(installedApps_[index].type, "pocketjs")) {
        buildPocketJsApp(index);
        return;
    }
    const uint8_t page = SYSTEM_PAGE_COUNT + index;
    const InstalledApp &app = installedApps_[index];
    pages_[page] = makePage(page, pageName(page), manifestIcon(app.icon));
    lv_obj_add_flag(pages_[page], LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_scroll_dir(pages_[page], LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pages_[page], LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *hero = makeCard(pages_[page], 10, APP_CONTENT_TOP + 4, 300, 54);
    lv_obj_t *icon = lv_obj_create(hero); lv_obj_set_pos(icon, 0, 0); lv_obj_set_size(icon, 34, 34);
    lv_obj_set_style_bg_color(icon, lv_color_hex(app.color), 0); lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_radius(icon, 10, 0); lv_obj_set_style_pad_all(icon, 0, 0); lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *glyph = lv_label_create(icon); lv_label_set_text(glyph, manifestIcon(app.icon));
    labelStyle(glyph, &lv_font_montserrat_16, lv_color_white()); lv_obj_center(glyph);
    lv_obj_t *heading = lv_label_create(hero);
    const char *titleValue = zh && app.titleZh[0] ? app.titleZh : app.title;
    lv_label_set_text(heading, titleValue); lv_obj_set_width(heading, 235); lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    labelStyle(heading, &lv_font_ui_14, text()); lv_obj_align(heading, LV_ALIGN_LEFT_MID, 44, 0);
    lv_obj_t *content = makeCard(pages_[page], 10, APP_CONTENT_TOP + 66, 300, 138);
    lv_obj_set_height(content, LV_SIZE_CONTENT); lv_obj_set_style_min_height(content, 138, 0);
    lv_obj_t *bodyLabel = lv_label_create(content); lv_obj_set_width(bodyLabel, 278);
    const char *bodyValue = zh && app.bodyZh[0] ? app.bodyZh : app.body;
    lv_label_set_text(bodyLabel, bodyValue); lv_label_set_long_mode(bodyLabel, LV_LABEL_LONG_WRAP);
    labelStyle(bodyLabel, &lv_font_ui_14, text());
}

void AppUI::buildPocketJsApp(uint8_t index) {
    const uint8_t page = SYSTEM_PAGE_COUNT + index;
    if (pocketJsRuntime.active() && pocketJsPage_ != page) destroyApp(pocketJsPage_);
    pages_[page] = makePage(page, pageName(page), LV_SYMBOL_PLAY);
    char directory[96]; snprintf(directory, sizeof(directory), "/apps/%s", installedApps_[index].id);
    Serial.printf("UI: launching PocketJS id=%s directory=%s\n", installedApps_[index].id, directory);
    char error[128]{};
    if (pocketJsRuntime.load(pages_[page], directory, error, sizeof(error))) {
        pocketJsPage_ = page;
        if (systemBar_ && pocketJsRuntime.fullscreen()) lv_obj_add_flag(systemBar_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    pocketJsPage_ = 0xFF;
    lv_obj_t *errorCard = makeCard(pages_[page], 10, APP_CONTENT_TOP + 8, 300, 118);
    lv_obj_t *title = lv_label_create(errorCard);
    lv_label_set_text(title, tr("PocketJS app could not start", "PocketJS 应用无法启动"));
    labelStyle(title, &lv_font_ui_14, lv_palette_main(LV_PALETTE_RED)); lv_obj_set_width(title, 278);
    lv_obj_t *detail = lv_label_create(errorCard); lv_label_set_text(detail, error[0] ? error : "Unknown runtime error");
    labelStyle(detail, &lv_font_ui_14, muted()); lv_obj_set_width(detail, 278);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP); lv_obj_set_y(detail, 32);
}


void AppUI::refreshFiles() {
    if (!fileList_) return;
    lv_obj_clean(fileList_); fileEntryCount_ = 0;
    if (filePathLabel_) lv_label_set_text(filePathLabel_, currentFilePath_);
    if (!connectivity.sdMounted()) {
        lv_obj_t *empty = lv_list_add_text(fileList_, tr("SD card unavailable", "SD 卡不可用"));
        labelStyle(empty, &lv_font_ui_14, muted()); return;
    }
    SD_MMC.mkdir("/screenshots");
    File directory = SD_MMC.open(currentFilePath_);
    if (!directory || !directory.isDirectory()) {
        strlcpy(currentFilePath_, "/", sizeof(currentFilePath_));
        directory = SD_MMC.open("/");
    }
    File entry;
    while (fileEntryCount_ < 32 && (entry = directory.openNextFile())) {
        const char *rawName = entry.name();
        const char *baseName = strrchr(rawName, '/'); baseName = baseName ? baseName + 1 : rawName;
        if (!baseName[0]) { entry.close(); continue; }
        const bool isDirectory = entry.isDirectory();
        const uint8_t slot = fileEntryCount_++;
        if (!strcmp(currentFilePath_, "/")) snprintf(fileEntryPaths_[slot], sizeof(fileEntryPaths_[slot]), "/%s", baseName);
        else snprintf(fileEntryPaths_[slot], sizeof(fileEntryPaths_[slot]), "%s/%s", currentFilePath_, baseName);
        fileEntryDirectories_[slot] = isDirectory;
        fileEntrySizes_[slot] = isDirectory ? 0 : uint32_t(entry.size());
        lv_obj_t *button = lv_button_create(fileList_); lv_obj_set_width(button, 292);
        lv_obj_set_height(button, 38); lv_obj_set_style_min_height(button, 38, 0);
        lv_obj_set_style_bg_color(button, card(), 0); lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 0, 0); lv_obj_set_style_radius(button, 11, 0);
        lv_obj_set_style_pad_all(button, 0, 0); lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_t *entryIcon = lv_label_create(button);
        lv_label_set_text(entryIcon, isDirectory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
        labelStyle(entryIcon, &lv_font_montserrat_14, isDirectory ? BLUE : muted());
        lv_obj_add_flag(entryIcon, LV_OBJ_FLAG_FLOATING); lv_obj_align(entryIcon, LV_ALIGN_LEFT_MID, 9, 0);
        lv_obj_t *entryText = lv_label_create(button); lv_label_set_text(entryText, baseName);
        labelStyle(entryText, &lv_font_ui_14, text()); lv_obj_set_width(entryText, isDirectory ? 198 : 183);
        lv_obj_set_height(entryText, 27); lv_obj_set_style_max_height(entryText, 27, 0);
        lv_label_set_long_mode(entryText, LV_LABEL_LONG_DOT); lv_obj_add_flag(entryText, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(entryText, LV_ALIGN_LEFT_MID, 38, 0);
        if (!isDirectory) {
            char sizeText[20]; formatFileSize(fileEntrySizes_[slot], sizeText, sizeof(sizeText));
            lv_obj_t *sizeLabel = lv_label_create(button); lv_label_set_text(sizeLabel, sizeText);
            labelStyle(sizeLabel, &lv_font_montserrat_12, muted()); lv_obj_set_width(sizeLabel, 58);
            lv_obj_set_style_text_align(sizeLabel, LV_TEXT_ALIGN_RIGHT, 0); lv_obj_add_flag(sizeLabel, LV_OBJ_FLAG_FLOATING);
            lv_obj_align(sizeLabel, LV_ALIGN_RIGHT_MID, -8, 0);
        } else {
            lv_obj_t *more = lv_button_create(button); lv_obj_set_size(more, 40, 36);
            lv_obj_add_flag(more, LV_OBJ_FLAG_FLOATING); lv_obj_align(more, LV_ALIGN_RIGHT_MID, -1, 0);
            lv_obj_set_style_bg_opa(more, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(more, 0, 0);
            lv_obj_set_style_pad_all(more, 0, 0);
            lv_obj_t *moreText = lv_label_create(more); lv_label_set_text(moreText, "...");
            labelStyle(moreText, &lv_font_montserrat_16, BLUE); lv_obj_center(moreText);
            lv_obj_set_style_translate_y(moreText, -3, 0);
            lv_obj_set_user_data(more, reinterpret_cast<void *>(uintptr_t(slot)));
            lv_obj_add_event_cb(more, fileMoreClicked, LV_EVENT_CLICKED, this);
        }
        lv_obj_set_user_data(button, reinterpret_cast<void *>(uintptr_t(slot)));
        lv_obj_add_event_cb(button, fileEntryClicked, LV_EVENT_CLICKED, this);
        entry.close();
    }
    directory.close();
    if (!fileEntryCount_) {
        lv_obj_t *empty = lv_list_add_text(fileList_, tr("This folder is empty", "此文件夹为空"));
        labelStyle(empty, &lv_font_ui_14, muted());
    }
}

void AppUI::closeFileActions() {
    if (keyboard_ && fileRenameEdit_ && lv_keyboard_get_textarea(keyboard_) == fileRenameEdit_) {
        lv_keyboard_set_textarea(keyboard_, nullptr); setKeyboardVisible(false);
    }
    if (fileActionPanel_) lv_obj_delete(fileActionPanel_);
    if (fileRenamePanel_) lv_obj_delete(fileRenamePanel_);
    fileActionPanel_ = fileActionTitle_ = fileDeleteLabel_ = nullptr;
    fileRenamePanel_ = fileRenameEdit_ = nullptr; fileDeleteArmedUntil_ = 0;
}

void AppUI::showFileActions(uint8_t index) {
    if (index >= fileEntryCount_) return;
    closeFileActions(); selectedFileEntry_ = index;
    fileActionPanel_ = lv_obj_create(lv_layer_top()); lv_obj_set_pos(fileActionPanel_, 10, 25);
    lv_obj_set_size(fileActionPanel_, 300, 210); lv_obj_set_style_bg_color(fileActionPanel_, card(), 0);
    lv_obj_set_style_bg_opa(fileActionPanel_, LV_OPA_COVER, 0); lv_obj_set_style_border_width(fileActionPanel_, 0, 0);
    lv_obj_set_style_radius(fileActionPanel_, 20, 0); lv_obj_set_style_pad_all(fileActionPanel_, 10, 0);
    lv_obj_clear_flag(fileActionPanel_, LV_OBJ_FLAG_SCROLLABLE);
    const char *base = strrchr(fileEntryPaths_[index], '/'); base = base ? base + 1 : fileEntryPaths_[index];
    fileActionTitle_ = lv_label_create(fileActionPanel_); lv_label_set_text(fileActionTitle_, base);
    lv_obj_set_width(fileActionTitle_, 238); lv_label_set_long_mode(fileActionTitle_, LV_LABEL_LONG_DOT);
    labelStyle(fileActionTitle_, &lv_font_ui_14, text()); lv_obj_set_pos(fileActionTitle_, 8, 4);
    lv_obj_t *close = lv_button_create(fileActionPanel_); lv_obj_set_pos(close, 252, -3); lv_obj_set_size(close, 30, 30);
    lv_obj_set_style_bg_color(close, bg(), 0); lv_obj_set_style_shadow_width(close, 0, 0); lv_obj_set_style_radius(close, 15, 0);
    lv_obj_t *closeText = lv_label_create(close); lv_label_set_text(closeText, LV_SYMBOL_CLOSE);
    labelStyle(closeText, &lv_font_montserrat_12, muted()); lv_obj_center(closeText);
    lv_obj_add_event_cb(close, fileActionClosed, LV_EVENT_CLICKED, this);
    struct Action { const char *label; lv_event_cb_t callback; lv_color_t color; int y; };
    const Action actions[] = {
        {fileEntryDirectories_[index] ? tr("Open", "打开") : tr("Preview", "预览"), fileActionPreviewClicked, BLUE, 39},
        {tr("Rename", "重命名"), fileActionRenameClicked, BLUE, 85},
        {tr("Delete", "删除"), fileActionDeleteClicked, LV_COLOR_MAKE(0xFF,0x3B,0x30), 131}
    };
    for (const auto &action : actions) {
        lv_obj_t *button = lv_button_create(fileActionPanel_); lv_obj_set_pos(button, 8, action.y); lv_obj_set_size(button, 264, 38);
        lv_obj_set_style_bg_color(button, bg(), 0); lv_obj_set_style_shadow_width(button, 0, 0); lv_obj_set_style_radius(button, 12, 0);
        lv_obj_t *label = lv_label_create(button); lv_label_set_text(label, action.label);
        labelStyle(label, &lv_font_ui_14, action.color); lv_obj_center(label);
        lv_obj_add_event_cb(button, action.callback, LV_EVENT_CLICKED, this);
        if (action.callback == fileActionDeleteClicked) fileDeleteLabel_ = label;
    }
}

void AppUI::showFileRename() {
    const uint8_t index = selectedFileEntry_;
    if (index >= fileEntryCount_) return;
    closeFileActions(); selectedFileEntry_ = index;
    if (!keyboard_) ensureApp(1);
    fileRenamePanel_ = lv_obj_create(lv_layer_top()); lv_obj_set_pos(fileRenamePanel_, 8, 20);
    lv_obj_set_size(fileRenamePanel_, 304, 88); lv_obj_set_style_bg_color(fileRenamePanel_, card(), 0);
    lv_obj_set_style_bg_opa(fileRenamePanel_, LV_OPA_COVER, 0); lv_obj_set_style_border_width(fileRenamePanel_, 0, 0);
    lv_obj_set_style_radius(fileRenamePanel_, 18, 0); lv_obj_set_style_pad_all(fileRenamePanel_, 8, 0);
    lv_obj_clear_flag(fileRenamePanel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(fileRenamePanel_); lv_label_set_text(title, tr("Rename file", "重命名文件"));
    labelStyle(title, &lv_font_ui_14, text()); lv_obj_set_pos(title, 4, 0);
    fileRenameEdit_ = lv_textarea_create(fileRenamePanel_); lv_obj_set_pos(fileRenameEdit_, 2, 28); lv_obj_set_size(fileRenameEdit_, 224, 38);
    lv_textarea_set_one_line(fileRenameEdit_, true); controlStyle(fileRenameEdit_); lv_obj_set_style_text_font(fileRenameEdit_, &lv_font_ui_14, 0);
    lv_obj_set_style_bg_color(fileRenameEdit_, BLUE, LV_PART_CURSOR); lv_obj_set_style_bg_opa(fileRenameEdit_, LV_OPA_COVER, LV_PART_CURSOR);
    const char *base = strrchr(fileEntryPaths_[index], '/'); lv_textarea_set_text(fileRenameEdit_, base ? base + 1 : fileEntryPaths_[index]);
    lv_obj_t *save = lv_button_create(fileRenamePanel_); lv_obj_set_pos(save, 232, 28); lv_obj_set_size(save, 56, 38);
    lv_obj_set_style_bg_color(save, BLUE, 0); lv_obj_set_style_radius(save, 12, 0);
    lv_obj_t *saveText = lv_label_create(save); lv_label_set_text(saveText, tr("Save", "保存"));
    labelStyle(saveText, &lv_font_ui_14, lv_color_white()); lv_obj_center(saveText);
    lv_obj_add_event_cb(save, fileRenameSaved, LV_EVENT_CLICKED, this);
    lv_keyboard_set_textarea(keyboard_, fileRenameEdit_); setKeyboardVisible(true); lv_obj_add_state(fileRenameEdit_, LV_STATE_FOCUSED);
}

void AppUI::closeFilePreview() {
    if (filePreview_) lv_obj_delete(filePreview_);
    filePreview_ = filePreviewBack_ = filePreviewTitle_ = filePreviewImage_ = nullptr;
    if (filePreviewBuffer_) { lv_draw_buf_destroy(filePreviewBuffer_); filePreviewBuffer_ = nullptr; }
    if (filePreviewEncoded_) { free(filePreviewEncoded_); filePreviewEncoded_ = nullptr; }
    previewImageCount_ = previewImageIndex_ = 0; filePreviewFullscreen_ = false;
    filePreviewTouchTracking_ = false; filePreviewSuppressClickUntil_ = 0;
}

void AppUI::showFilePreview(const char *path) {
    closeFilePreview();
    filePreview_ = lv_obj_create(lv_layer_top()); lv_obj_set_pos(filePreview_, 0, 0); lv_obj_set_size(filePreview_, 320, 240);
    lv_obj_set_style_bg_color(filePreview_, bg(), 0); lv_obj_set_style_bg_opa(filePreview_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(filePreview_, 0, 0); lv_obj_set_style_radius(filePreview_, 0, 0);
    lv_obj_set_style_pad_all(filePreview_, 0, 0); lv_obj_clear_flag(filePreview_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(filePreview_, LV_OBJ_FLAG_CLICKABLE);
    filePreviewBack_ = lv_button_create(filePreview_); lv_obj_set_pos(filePreviewBack_, 7, 5); lv_obj_set_size(filePreviewBack_, 38, 32);
    lv_obj_set_style_bg_opa(filePreviewBack_, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(filePreviewBack_, 0, 0);
    lv_obj_t *closeIcon = lv_label_create(filePreviewBack_); lv_label_set_text(closeIcon, LV_SYMBOL_LEFT);
    labelStyle(closeIcon, &lv_font_montserrat_14, BLUE); lv_obj_center(closeIcon);
    lv_obj_add_event_cb(filePreviewBack_, filePreviewClosed, LV_EVENT_CLICKED, this);
    filePreviewTitle_ = lv_label_create(filePreview_); lv_obj_set_width(filePreviewTitle_, 260);
    lv_obj_align_to(filePreviewTitle_, filePreviewBack_, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_label_set_long_mode(filePreviewTitle_, LV_LABEL_LONG_DOT); labelStyle(filePreviewTitle_, &lv_font_ui_14, text());

    if (isImageFile(path)) {
        for (uint8_t i = 0; i < fileEntryCount_ && previewImageCount_ < 32; ++i) {
            if (!fileEntryDirectories_[i] && isImageFile(fileEntryPaths_[i])) {
                strlcpy(previewImagePaths_[previewImageCount_], fileEntryPaths_[i], sizeof(previewImagePaths_[0]));
                if (!strcmp(fileEntryPaths_[i], path)) previewImageIndex_ = previewImageCount_;
                ++previewImageCount_;
            }
        }
        filePreviewImage_ = lv_image_create(filePreview_); lv_obj_set_pos(filePreviewImage_, 10, 42);
        lv_obj_add_flag(filePreviewImage_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(filePreviewImage_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(filePreviewImage_, filePreviewImageClicked, LV_EVENT_CLICKED, this);
        showPreviewImage(previewImageIndex_);
        return;
    }

    const char *name = strrchr(path, '/'); lv_label_set_text(filePreviewTitle_, name ? name + 1 : path);
    lv_obj_t *content = lv_obj_create(filePreview_); lv_obj_set_pos(content, 8, 42); lv_obj_set_size(content, 304, 188);
    lv_obj_set_style_bg_color(content, card(), 0); lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 14, 0); lv_obj_set_style_pad_all(content, 9, 0);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF); lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_t *textLabel = lv_label_create(content); lv_obj_set_width(textLabel, 282);
    labelStyle(textLabel, &lv_font_ui_14, text()); lv_label_set_long_mode(textLabel, LV_LABEL_LONG_WRAP);
    if (isTextFile(path)) {
        File input = SD_MMC.open(path, FILE_READ);
        const size_t size = input ? min(size_t(input.size()), size_t(32768)) : 0;
        char *contents = static_cast<char *>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (contents && input) {
            const size_t read = input.readBytes(contents, size); contents[read] = 0;
            lv_label_set_text(textLabel, contents); free(contents);
        } else lv_label_set_text(textLabel, tr("Unable to read this file", "无法读取此文件"));
        if (input) input.close();
    } else lv_label_set_text(textLabel, tr("Preview is available for JPG/BMP images and text files.", "目前支持预览 JPG/BMP 图片和文本文件。"));
}

void AppUI::showPreviewImage(uint8_t index) {
    if (!filePreviewImage_ || index >= previewImageCount_) return;
    previewImageIndex_ = index;
    lv_image_set_src(filePreviewImage_, nullptr);
    if (filePreviewBuffer_) { lv_draw_buf_destroy(filePreviewBuffer_); filePreviewBuffer_ = nullptr; }
    if (filePreviewEncoded_) { free(filePreviewEncoded_); filePreviewEncoded_ = nullptr; }
    File input = SD_MMC.open(previewImagePaths_[index], FILE_READ);
    if (hasExtension(previewImagePaths_[index], ".jpg") || hasExtension(previewImagePaths_[index], ".jpeg")) {
        const size_t encodedSize = input ? size_t(input.size()) : 0;
        if (!input || !encodedSize || encodedSize > 2U * 1024U * 1024U) {
            if (input) input.close(); lv_label_set_text(filePreviewTitle_, tr("JPG is invalid or too large", "JPG 无效或文件过大")); return;
        }
        filePreviewEncoded_ = static_cast<uint8_t *>(heap_caps_malloc(encodedSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!filePreviewEncoded_ || input.readBytes(reinterpret_cast<char *>(filePreviewEncoded_), encodedSize) != encodedSize) {
            if (filePreviewEncoded_) { free(filePreviewEncoded_); filePreviewEncoded_ = nullptr; }
            input.close(); lv_label_set_text(filePreviewTitle_, tr("Unable to load JPG", "无法载入 JPG")); return;
        }
        input.close(); uint16_t sourceWidth = 0, sourceHeight = 0;
        if (!jpegDimensions(filePreviewEncoded_, encodedSize, sourceWidth, sourceHeight)) {
            free(filePreviewEncoded_); filePreviewEncoded_ = nullptr;
            lv_label_set_text(filePreviewTitle_, tr("Invalid JPG image", "JPG 图片无效")); return;
        }
        uint8_t divisor = 1; jpg_scale_t decodeScale = JPG_SCALE_NONE;
        while ((sourceWidth / divisor > 640 || sourceHeight / divisor > 480) && divisor < 8) divisor *= 2;
        if (divisor == 2) decodeScale = JPG_SCALE_2X;
        else if (divisor == 4) decodeScale = JPG_SCALE_4X;
        else if (divisor == 8) decodeScale = JPG_SCALE_8X;
        const uint16_t decodedWidth = (sourceWidth + divisor - 1) / divisor;
        const uint16_t decodedHeight = (sourceHeight + divisor - 1) / divisor;
        if (uint32_t(decodedWidth) * decodedHeight > 350000U) {
            free(filePreviewEncoded_); filePreviewEncoded_ = nullptr;
            lv_label_set_text(filePreviewTitle_, tr("JPG dimensions are too large", "JPG 图片尺寸过大")); return;
        }
        filePreviewBuffer_ = lv_draw_buf_create(decodedWidth, decodedHeight, LV_COLOR_FORMAT_RGB565, 0);
        if (!filePreviewBuffer_) {
            free(filePreviewEncoded_); filePreviewEncoded_ = nullptr;
            lv_label_set_text(filePreviewTitle_, tr("Not enough memory", "内存不足")); return;
        }
        memset(filePreviewBuffer_->data, 0, filePreviewBuffer_->data_size);
        JpegDecodeContext context{filePreviewEncoded_, encodedSize, filePreviewBuffer_};
        const esp_err_t decoded = esp_jpg_decode(encodedSize, decodeScale, jpegMemoryReader, jpegRgbWriter, &context);
        free(filePreviewEncoded_); filePreviewEncoded_ = nullptr;
        if (decoded != ESP_OK) {
            lv_draw_buf_destroy(filePreviewBuffer_); filePreviewBuffer_ = nullptr;
            lv_label_set_text(filePreviewTitle_, tr("JPG decode failed", "JPG 解码失败")); return;
        }
        lv_image_set_src(filePreviewImage_, filePreviewBuffer_);
        const float maxWidth = filePreviewFullscreen_ ? 320.0f : 300.0f;
        const float maxHeight = filePreviewFullscreen_ ? 240.0f : 178.0f;
        const float scale = min(1.0f, min(maxWidth / decodedWidth, maxHeight / decodedHeight));
        lv_image_set_scale(filePreviewImage_, max(1U, uint32_t(scale * LV_SCALE_NONE)));
        if (filePreviewFullscreen_) lv_obj_align(filePreviewImage_, LV_ALIGN_CENTER, 0, 0);
        else lv_obj_align(filePreviewImage_, LV_ALIGN_BOTTOM_MID, 0, -11);
        const char *name = strrchr(previewImagePaths_[index], '/');
        char title[96]; snprintf(title, sizeof(title), "%s  %u/%u", name ? name + 1 : previewImagePaths_[index],
                                 unsigned(index + 1), unsigned(previewImageCount_));
        lv_label_set_text(filePreviewTitle_, title); return;
    }
    lv_image_set_scale(filePreviewImage_, LV_SCALE_NONE);
    uint8_t header[54]{};
    if (!input || input.read(header, sizeof(header)) != sizeof(header) || header[0] != 'B' || header[1] != 'M') {
        if (input) input.close(); lv_label_set_text(filePreviewTitle_, tr("Invalid BMP image", "BMP 图片无效")); return;
    }
    auto get16 = [&](uint8_t offset) { return uint16_t(header[offset]) | uint16_t(header[offset + 1]) << 8; };
    auto get32 = [&](uint8_t offset) { return uint32_t(header[offset]) | uint32_t(header[offset + 1]) << 8 |
                                             uint32_t(header[offset + 2]) << 16 | uint32_t(header[offset + 3]) << 24; };
    const uint32_t dataOffset = get32(10), sourceWidth = get32(18), sourceHeight = get32(22);
    if (!sourceWidth || !sourceHeight || get16(28) != 24 || sourceWidth > 4096 || sourceHeight > 4096) {
        input.close(); lv_label_set_text(filePreviewTitle_, tr("Unsupported BMP format", "不支持此 BMP 格式")); return;
    }
    const float maxWidth = filePreviewFullscreen_ ? 320.0f : 300.0f;
    const float maxHeight = filePreviewFullscreen_ ? 240.0f : 178.0f;
    const float scale = min(maxWidth / sourceWidth, maxHeight / sourceHeight);
    const uint32_t targetWidth = max(1U, uint32_t(sourceWidth * min(scale, 1.0f)));
    const uint32_t targetHeight = max(1U, uint32_t(sourceHeight * min(scale, 1.0f)));
    filePreviewBuffer_ = lv_draw_buf_create(targetWidth, targetHeight, LV_COLOR_FORMAT_RGB565, 0);
    const uint32_t sourceStride = (sourceWidth * 3 + 3) & ~3U;
    uint8_t *sourceRow = static_cast<uint8_t *>(heap_caps_malloc(sourceStride, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!filePreviewBuffer_ || !sourceRow) {
        if (sourceRow) free(sourceRow); input.close(); lv_label_set_text(filePreviewTitle_, tr("Not enough memory", "内存不足")); return;
    }
    for (uint32_t y = 0; y < targetHeight; ++y) {
        const uint32_t sourceY = y * sourceHeight / targetHeight;
        input.seek(dataOffset + (sourceHeight - 1 - sourceY) * sourceStride);
        if (input.read(sourceRow, sourceStride) != sourceStride) break;
        uint16_t *target = reinterpret_cast<uint16_t *>(filePreviewBuffer_->data + y * filePreviewBuffer_->header.stride);
        for (uint32_t x = 0; x < targetWidth; ++x) {
            const uint32_t sourceX = x * sourceWidth / targetWidth;
            const uint8_t *pixel = sourceRow + sourceX * 3;
            target[x] = uint16_t((pixel[2] >> 3) << 11) | uint16_t((pixel[1] >> 2) << 5) | uint16_t(pixel[0] >> 3);
        }
    }
    free(sourceRow); input.close(); lv_image_set_src(filePreviewImage_, filePreviewBuffer_);
    if (filePreviewFullscreen_) lv_obj_align(filePreviewImage_, LV_ALIGN_CENTER, 0, 0);
    else lv_obj_align(filePreviewImage_, LV_ALIGN_BOTTOM_MID, 0, -11);
    const char *name = strrchr(previewImagePaths_[index], '/');
    char title[96]; snprintf(title, sizeof(title), "%s  %u/%u", name ? name + 1 : previewImagePaths_[index],
                             unsigned(index + 1), unsigned(previewImageCount_));
    lv_label_set_text(filePreviewTitle_, title);
}

void AppUI::refreshMessages() {
    if (!messageList_) return;
    lv_obj_clean(messageList_); size_t shown = 0;
    CommunicationPeer selectedNode;
    const char *selectedPeer = nullptr;
    if (directTab_ && targetDropdown_) {
        const uint16_t target = lv_dropdown_get_selected(targetDropdown_);
        if (target && target <= targetCount_) { selectedNode = communication.peer(targetNodeIndexes_[target - 1]); selectedPeer = selectedNode.name; }
    }
    for (size_t i = 0; i < messageCount_; ++i) if (messages_[i].direct == directTab_ && !strcmp(messages_[i].protocol, communication.configurationProtocol())) {
        const Message &message = messages_[i];
        if (strcmp(message.protocol, communication.protocol())) continue;
        if (directTab_ && selectedPeer && strcmp(selectedPeer, message.peer)) continue;
        char display[170];
        if (!directTab_ && !message.outgoing && message.peer[0])
            snprintf(display, sizeof(display), "%s: %s", message.peer, message.text);
        else strlcpy(display, message.text, sizeof(display));
        const uint16_t bubbleWidth = constrain(int(strlen(display) * 7 + 18), 58, 232);
        lv_obj_t *row = lv_obj_create(messageList_); lv_obj_set_width(row, 292); lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 27, 0); lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0); lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE); lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *bubble = lv_obj_create(row); lv_obj_set_width(bubble, bubbleWidth); lv_obj_set_height(bubble, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(bubble, 25, 0);
        const lv_color_t bubbleColor = message.outgoing ? BLUE : (message.contact ? GREEN : card());
        lv_obj_set_style_bg_color(bubble, bubbleColor, 0);
        lv_obj_set_style_border_width(bubble, 0, 0); lv_obj_set_style_radius(bubble, 12, 0);
        lv_obj_set_style_pad_hor(bubble, 7, 0); lv_obj_set_style_pad_ver(bubble, 4, 0);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(bubble, message.outgoing ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *line = lv_label_create(bubble); lv_label_set_text(line, display);
        lv_obj_set_width(line, bubbleWidth - 14); lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
        labelStyle(line, &lv_font_ui_14, (message.outgoing || message.contact) ? lv_color_white() : text()); ++shown;
    }
    if (!shown) {
        lv_obj_t *empty = lv_label_create(messageList_);
        lv_label_set_text(empty, directTab_ ? tr("No direct messages yet", "暂无私信") : tr("No group messages yet", "暂无频道消息"));
        labelStyle(empty, &lv_font_ui_14, muted());
    }
    lv_obj_scroll_to_y(messageList_, LV_COORD_MAX, LV_ANIM_OFF);
}

void AppUI::refreshTargets() {
    if (!targetDropdown_) return;
    const uint16_t old = lv_dropdown_get_selected(targetDropdown_); char options[512];
    strlcpy(options, tr("Select a contact", "选择联系人"), sizeof(options)); targetCount_ = 0;
    for (size_t i = 0; i < communication.peerCount() && targetCount_ < 32; ++i) if (communication.peer(i).contact || !strcmp(communication.configurationProtocol(), "meshtastic")) {
        targetNodeIndexes_[targetCount_++] = i; strlcat(options, "\n", sizeof(options)); strlcat(options, communication.peer(i).name, sizeof(options));
    }
    lv_dropdown_set_options(targetDropdown_, options); lv_dropdown_set_selected(targetDropdown_, old <= targetCount_ ? old : 0);
}

void AppUI::refreshWifiNetworks() {
    if (!wifiNetworkDropdown_) { connectivity.clearWifiScan(); return; }
    const int16_t found = connectivity.wifiScanComplete();
    char options[768]; strlcpy(options, tr("Select a network", "选择 Wi-Fi 网络"), sizeof(options));
    wifiScanCount_ = 0;
    for (int16_t i = 0; i < found && wifiScanCount_ < 16; ++i) {
        const String ssid = connectivity.wifiScanSsid(i);
        if (!ssid.length()) continue;
        bool duplicate = false;
        for (uint8_t j = 0; j < wifiScanCount_; ++j) if (ssid == wifiScanSsids_[j]) { duplicate = true; break; }
        if (duplicate) continue;
        strlcpy(wifiScanSsids_[wifiScanCount_], ssid.c_str(), sizeof(wifiScanSsids_[0]));
        char line[64]; snprintf(line, sizeof(line), "\n%s  |  %ld dBm", wifiScanSsids_[wifiScanCount_], long(connectivity.wifiScanRssi(i)));
        strlcat(options, line, sizeof(options)); ++wifiScanCount_;
    }
    if (!wifiScanCount_) strlcat(options, tr("\nNo networks found", "\n未找到 Wi-Fi 网络"), sizeof(options));
    lv_dropdown_set_options(wifiNetworkDropdown_, options);
    lv_dropdown_set_selected(wifiNetworkDropdown_, 0);
    connectivity.clearWifiScan();
}

void AppUI::refreshNodes() {
    if (!nodeList_) return;
    lv_obj_clean(nodeList_); memset(nodeAgeLabels_, 0, sizeof(nodeAgeLabels_));
    if (!communication.peerCount()) {
        lv_obj_t *empty = lv_list_add_text(nodeList_, tr("Searching for nearby adverts...", "正在搜索附近广播..."));
        labelStyle(empty, &lv_font_ui_14, muted()); return;
    }
    for (size_t i = 0; i < communication.peerCount(); ++i) {
        const CommunicationPeer node = communication.peer(i); char row[88];
        const char *displayName = node.name[0] ? node.name : tr("Unnamed", "未命名");
        if (node.lastSeen) snprintf(row, sizeof(row), "%s  |  %s  |  SNR %d.%u", displayName,
                                    node.contact ? tr("Contact", "联系人") : tr("Nearby", "附近"), node.snrQuarter / 4, abs(node.snrQuarter % 4) * 25);
        else snprintf(row, sizeof(row), zh ? "%s  |  已保存联系人  |  离线" : "%s  |  Saved Contact  |  Offline", displayName);
        lv_obj_t *button = lv_list_add_button(nodeList_, nullptr, row);
        lv_obj_set_height(button, 38); lv_obj_set_style_min_height(button, 38, 0);
        lv_obj_set_style_pad_hor(button, 10, 0); lv_obj_set_style_pad_ver(button, 0, 0);
        lv_obj_set_style_radius(button, 12, 0);
        lv_obj_set_style_text_font(button, &lv_font_ui_14, 0);
        lv_obj_set_style_bg_color(button, dark ? lv_color_hex(0x2C2C2E) : card(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, dark ? 1 : 0, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x3A3A3C), 0);
        lv_obj_set_style_text_color(button, text(), 0);
        lv_obj_t *line = lv_obj_get_child(button, 0);
        if (line) {
            lv_obj_add_flag(line, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_size(line, 232, 20);
            lv_label_set_long_mode(line, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(line, LV_ALIGN_LEFT_MID, 0, 0);
        }
        nodeAgeLabels_[i] = lv_label_create(button); lv_label_set_text(nodeAgeLabels_[i], "--");
        labelStyle(nodeAgeLabels_[i], &lv_font_montserrat_12, BLUE);
        lv_obj_add_flag(nodeAgeLabels_[i], LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(nodeAgeLabels_[i], 36, 16); lv_obj_set_style_text_align(nodeAgeLabels_[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(nodeAgeLabels_[i], LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(i)); lv_obj_add_event_cb(button, nodeClicked, LV_EVENT_CLICKED, this);
    }
    updateNodeAges();
}

void AppUI::updateNodeAges() {
    const uint32_t now = systemClock.now();
    for (size_t i = 0; i < communication.peerCount() && i < 32; ++i) if (nodeAgeLabels_[i]) {
        const uint32_t seen = communication.peer(i).lastSeen;
        if (!seen) lv_label_set_text(nodeAgeLabels_[i], "--");
        else {
            const uint32_t age = now > seen ? now - seen : 0;
            if (age < 60) lv_label_set_text_fmt(nodeAgeLabels_[i], "%lus", (unsigned long)age);
            else if (age < 3600) lv_label_set_text_fmt(nodeAgeLabels_[i], "%lum", (unsigned long)(age / 60));
            else if (age < 86400) lv_label_set_text_fmt(nodeAgeLabels_[i], "%luh", (unsigned long)(age / 3600));
            else lv_label_set_text(nodeAgeLabels_[i], ">1d");
        }
    }
}

void AppUI::refreshDots() {}

void AppUI::setKeyboardVisible(bool visible) {
    if (!keyboard_ || !lv_obj_is_valid(keyboard_)) return;
    lv_obj_t *target = lv_keyboard_get_textarea(keyboard_);
    const bool composingMessage = compose_ && target == compose_;
    if (visible) {
        if (composingMessage) {
            if (messageList_) lv_obj_add_flag(messageList_, LV_OBJ_FLAG_HIDDEN);
            if (targetDropdown_) lv_obj_add_flag(targetDropdown_, LV_OBJ_FLAG_HIDDEN);
            if (channelLabel_) lv_obj_add_flag(channelLabel_, LV_OBJ_FLAG_HIDDEN);
            if (messageSegment_) lv_obj_add_flag(messageSegment_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(compose_, 72); if (sendButton_) lv_obj_set_y(sendButton_, 72);
        }
        lv_obj_remove_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(keyboard_);
    } else {
        lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
        if (messageList_) lv_obj_remove_flag(messageList_, LV_OBJ_FLAG_HIDDEN);
        if (messageSegment_) lv_obj_remove_flag(messageSegment_, LV_OBJ_FLAG_HIDDEN);
        if (targetDropdown_) {
            if (directTab_) lv_obj_remove_flag(targetDropdown_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(targetDropdown_, LV_OBJ_FLAG_HIDDEN);
        }
        if (compose_) { lv_obj_set_y(compose_, 201); lv_obj_clear_state(compose_, LV_STATE_FOCUSED); }
        if (sendButton_) lv_obj_set_y(sendButton_, 201);
        if (nameEdit_) lv_obj_clear_state(nameEdit_, LV_STATE_FOCUSED);
    }
}

void AppUI::loop() {
    if (page_ == pocketJsPage_) pocketJsRuntime.loop();
    const uint32_t fpsNow = millis();
    if (fpsEnabled_ && fpsLabel_ && fpsNow - fpsWindowStarted_ >= 1000) {
        const uint32_t elapsed = fpsNow - fpsWindowStarted_;
        const uint32_t fps = elapsed ? uint32_t(fpsFrameCount_) * 1000U / elapsed : 0;
        lv_label_set_text_fmt(fpsLabel_, "FPS %lu", (unsigned long)fps);
        lv_obj_move_foreground(fpsLabel_); fpsWindowStarted_ = fpsNow; fpsFrameCount_ = 0;
    }
    const uint32_t inactive = lv_display_get_inactive_time(nullptr);
    if (screenTimeoutSeconds_ && hardware.displayAwake() &&
        uint32_t(millis() - lastUserActivityMs_) >= uint32_t(screenTimeoutSeconds_) * 1000U)
        toggleDisplayWithEffect();
    if (inactive > 5000 && !indicatorHidden_) {
        lv_obj_add_flag(homeIndicator_, LV_OBJ_FLAG_HIDDEN); indicatorHidden_ = true;
    } else if (inactive < 300 && indicatorHidden_) {
        lv_obj_remove_flag(homeIndicator_, LV_OBJ_FLAG_HIDDEN); indicatorHidden_ = false;
    }
    static uint32_t next = 0; if (int32_t(millis() - next) < 0) return; next = millis() + 1000;
    if (memoryEnabled_ && memoryLabel_) {
        const uint32_t totalMemory = ESP.getHeapSize() + ESP.getPsramSize();
        const uint32_t freeMemory = ESP.getFreeHeap() + ESP.getFreePsram();
        lv_label_set_text_fmt(memoryLabel_, "MEM %lu%%", (unsigned long)(totalMemory ? freeMemory * 100U / totalMemory : 0));
        lv_obj_align(memoryLabel_, LV_ALIGN_BOTTOM_LEFT, fpsEnabled_ ? 62 : 0, 0);
        lv_obj_move_foreground(memoryLabel_);
    }
    if (radioDropdown_ && (strcmp(radioConfiguration_, communication.configurationProtocol()) ||
        (radioRoleDropdown_ && lv_dropdown_get_selected(radioRoleDropdown_) != meshtasticService.roleIndex()))) refreshRadioControls();
    if (radioProtocolDropdown_) lv_dropdown_set_selected(radioProtocolDropdown_, !strcmp(communication.protocol(), "off") ? 0 : !strcmp(communication.protocol(), "meshtastic") ? 2 : 1);
    if (radioDropdown_ && lv_dropdown_get_selected(radioDropdown_) != communication.radioPresetIndex()) {
        lv_dropdown_set_selected(radioDropdown_, communication.radioPresetIndex());
        setRadioPresetText(radioLabel_, communication.radioPreset(communication.radioPresetIndex()));
    }
    if (nodeList_) updateNodeAges();
    const uint16_t mv = hardware.batteryMillivolts(); setVoltage(homeBattery_, mv);
    const uint16_t pct = mv <= 3300 ? 0 : (mv >= 4200 ? 100 : uint16_t((mv - 3300) * 100 / 900));
    uint32_t networkTimestamp = 0;
    if (connectivity.takeNetworkTime(networkTimestamp)) {
        systemClock.set(networkTimestamp);
        networkClockSynced_ = true;
        lastNetworkSyncMs_ = millis();
        gpsClockSynced_ = false;
        Serial.printf("Time: network synchronized UTC=%lu\n", static_cast<unsigned long>(networkTimestamp));
        if (gpsTimeStatus_) lv_label_set_text(gpsTimeStatus_, tr("Network synchronized", "网络已校时"));
        if (!lastClockTimeSaved_ || networkTimestamp < lastClockTimeSaved_ ||
            networkTimestamp - lastClockTimeSaved_ >= 3600) {
            Preferences syncPreferences; syncPreferences.begin("mesh-ui", false);
            syncPreferences.putULong("system-time", networkTimestamp); syncPreferences.end();
            lastClockTimeSaved_ = networkTimestamp;
        }
    } else if (gpsAutoTime_ && (!networkClockSynced_ || !connectivity.wifiConnected() ||
                               millis() - lastNetworkSyncMs_ >= 6UL * 60UL * 60UL * 1000UL) &&
               hardware.gpsTimeValid() && hardware.gpsTimestamp() != lastGpsTimeApplied_) {
        lastGpsTimeApplied_ = hardware.gpsTimestamp();
        const int64_t difference = int64_t(lastGpsTimeApplied_) - int64_t(systemClock.now());
        if (difference < -2 || difference > 2) systemClock.set(lastGpsTimeApplied_);
        gpsClockSynced_ = true;
        networkClockSynced_ = false;
        if (gpsTimeStatus_) lv_label_set_text(gpsTimeStatus_, tr("GPS synchronized", "GPS 已校时"));
        if (!lastClockTimeSaved_ || lastGpsTimeApplied_ < lastClockTimeSaved_ ||
            lastGpsTimeApplied_ - lastClockTimeSaved_ >= 3600) {
            Preferences syncPreferences; syncPreferences.begin("mesh-ui", false);
            syncPreferences.putULong("system-time", lastGpsTimeApplied_); syncPreferences.end();
            lastClockTimeSaved_ = lastGpsTimeApplied_;
        }
    }
    time_t currentTimestamp = time_t(int64_t(systemClock.now()) + int64_t(timezoneMinutes_) * 60);
    struct tm currentDate{}; gmtime_r(&currentTimestamp, &currentDate);
    const lv_color_t clockColor = networkClockSynced_ ? GREEN : (gpsClockSynced_ ? BLUE : text());
    if (clockLabel_) {
        lv_label_set_text_fmt(clockLabel_, "%02d:%02d", currentDate.tm_hour, currentDate.tm_min);
        lv_obj_set_style_text_color(clockLabel_, clockColor, 0);
    }
    if (gpsClockIcon_) lv_obj_add_flag(gpsClockIcon_, LV_OBJ_FLAG_HIDDEN);
    if (appTitleLabel_ && notificationPanel_ && !lv_obj_has_flag(notificationPanel_, LV_OBJ_FLAG_HIDDEN)) {
        char date[16]; formatDateTime(systemClock.now(), date, sizeof(date), true, timezoneMinutes_); lv_label_set_text(appTitleLabel_, date);
    }
    const int loraRssi = communication.rssi();
    if (radioSignalLabel_) {
        if (communication.signalKnown()) lv_label_set_text_fmt(radioSignalLabel_, "RSSI %d / SNR %d", loraRssi, int(communication.snr()));
        else lv_label_set_text(radioSignalLabel_, "RSSI -- / SNR --");
    }
    const uint8_t loraStrength = !communication.signalKnown() ? 0 :
        (loraRssi >= -70 ? 4 : (loraRssi >= -85 ? 3 : (loraRssi >= -100 ? 2 : 1)));
    for (uint8_t i = 0; i < 4; ++i) if (loraBars_[i]) {
        lv_obj_set_style_bg_color(loraBars_[i], i < loraStrength ? text() : muted(), 0);
        lv_obj_set_style_bg_opa(loraBars_[i], i < loraStrength ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    if (wifiIcon_) {
        if (connectivity.wifiConnected()) {
            const int32_t wifiRssi = connectivity.wifiRssi();
            const lv_opa_t opacity = wifiRssi >= -55 ? LV_OPA_COVER :
                (wifiRssi >= -70 ? LV_OPA_80 : (wifiRssi >= -85 ? LV_OPA_50 : LV_OPA_30));
            lv_obj_set_style_text_opa(wifiIcon_, opacity, 0);
            lv_obj_remove_flag(wifiIcon_, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(wifiIcon_, LV_OBJ_FLAG_HIDDEN);
    }
    if (batteryFill_) {
        const int batteryWidth = pct ? int(pct * 18 / 100) : 1;
        lv_obj_set_width(batteryFill_, batteryWidth > 0 ? batteryWidth : 1);
        lv_obj_set_style_bg_color(batteryFill_, pct <= 10 ? lv_palette_main(LV_PALETTE_RED) : (pct <= 20 ? ORANGE : GREEN), 0);
    }
    if (gpsStatus_) lv_label_set_text_fmt(gpsStatus_, hardware.gpsEnabled() ? tr("On  |  %lu bytes", "开启  |  %lu 字节") : tr("Off", "关闭"), hardware.gpsBytesSeen());
    if (wifiStatus_) {
        if (connectivity.wifiConnected()) {
            const String address = connectivity.wifiAddress();
            lv_label_set_text_fmt(wifiStatus_, "%s  %s  %ld dBm", connectivity.savedSsid(), address.c_str(), long(connectivity.wifiRssi()));
        } else if (connectivity.savedSsid()[0]) lv_label_set_text(wifiStatus_, tr("Connecting...", "正在连接..."));
        else lv_label_set_text(wifiStatus_, tr("Not configured", "尚未配置"));
    }
    if (wifiScanActive_) {
        const int16_t scanResult = connectivity.wifiScanComplete();
        if (scanResult >= 0) {
            refreshWifiNetworks(); wifiScanActive_ = false;
            if (wifiScanLabel_) lv_label_set_text(wifiScanLabel_, tr("Scan", "扫描"));
        } else if (scanResult == WIFI_SCAN_FAILED) {
            wifiScanActive_ = false;
            if (wifiScanLabel_) lv_label_set_text(wifiScanLabel_, tr("Retry", "重试"));
        }
    }
    if (sdStatus_) {
        if (connectivity.sdMounted()) lv_label_set_text_fmt(sdStatus_, tr("%llu MB free", "可用 %llu MB"),
            (connectivity.sdTotalBytes() - connectivity.sdUsedBytes()) / (1024ULL * 1024ULL));
        else lv_label_set_text(sdStatus_, tr("No usable card", "未检测到可用卡"));
    }
    const StorageJobState storageJob = connectivity.jobState();
    if (mapIpButtonLabel_) {
        const auto ipState = connectivity.ipLocationState();
        if (ipState == ConnectivityService::IpLocationState::Ready && !mapIpLocationApplied_) {
            char value[24];
            snprintf(value, sizeof(value), "%.6f", connectivity.ipLatitude());
            if (mapLatitudeEdit_) lv_textarea_set_text(mapLatitudeEdit_, value);
            snprintf(value, sizeof(value), "%.6f", connectivity.ipLongitude());
            if (mapLongitudeEdit_) lv_textarea_set_text(mapLongitudeEdit_, value);
            mapIpLocationApplied_ = true;
        }
        lv_label_set_text(mapIpButtonLabel_, ipState == ConnectivityService::IpLocationState::Loading ?
            tr("Locating...", "定位中...") : ipState == ConnectivityService::IpLocationState::Failed ?
            tr("IP lookup failed; retry", "IP 定位失败，点击重试") :
            tr("Use approximate IP location", "使用 IP 粗略定位"));
    }
    if (mapStorageStatus_) {
        if (storageJob == StorageJobState::Downloading)
            lv_label_set_text_fmt(mapStorageStatus_, tr("Downloading %u/%u  %u%%", "正在下载 %u/%u  %u%%"),
                                  connectivity.jobDone(), connectivity.jobTotal(), connectivity.jobProgress());
        else if (storageJob == StorageJobState::Complete && connectivity.jobTotal() == 9)
            lv_label_set_text(mapStorageStatus_, tr("Current area saved to SD card", "当前位置地图已保存到 SD 卡"));
        else if (storageJob == StorageJobState::Failed)
            lv_label_set_text_fmt(mapStorageStatus_, "%s: %s", tr("Failed", "失败"), connectivity.jobError());
        else if (!connectivity.sdMounted()) lv_label_set_text(mapStorageStatus_, tr("Insert a FAT32 SD card", "请插入 FAT32 SD 卡"));
        else if (!connectivity.wifiConnected()) lv_label_set_text(mapStorageStatus_, tr("Connect Wi-Fi in Settings", "请先在设置中连接 Wi-Fi"));
        else if (!hardware.gpsHasFix() && (!mapLatitudeEdit_ || !lv_textarea_get_text(mapLatitudeEdit_)[0]))
            lv_label_set_text(mapStorageStatus_, tr("Enter coordinates or use IP location", "输入坐标或使用 IP 定位"));
        else lv_label_set_text(mapStorageStatus_, tr("Ready to download 3 x 3 tiles", "可以下载 3 x 3 地图瓦片"));
    }
    if (formatSdLabel_) {
        if (storageJob == StorageJobState::Formatting) lv_label_set_text(formatSdLabel_, tr("Formatting...", "格式化中..."));
        else if (storageJob == StorageJobState::Complete && connectivity.jobTotal() == 1)
            lv_label_set_text(formatSdLabel_, tr("Done", "已完成"));
        else if (formatArmedUntil_ && int32_t(millis() - formatArmedUntil_) < 0)
            lv_label_set_text(formatSdLabel_, tr("Tap again", "再次点击"));
        else {
            formatArmedUntil_ = 0;
            lv_label_set_text(formatSdLabel_, tr("Format", "格式化"));
        }
    }
    if (powerVoltage_) setVoltage(powerVoltage_, mv);
    if (powerPercent_) lv_label_set_text_fmt(powerPercent_, "%u%%", unsigned(pct));
    if (powerBar_) lv_bar_set_value(powerBar_, pct, LV_ANIM_ON);
    if (navigationStatus_) updateMapView(hardware.gpsHasFix() && !mapCenteredOnGps_);
    updateGpsApp();
    updateSpeedometerApp();
    if (diagnosticLabel_) {
        const RadioPreset &preset = communication.radioPreset(communication.radioPresetIndex());
        lv_label_set_text_fmt(diagnosticLabel_, zh ?
            "MeshCore       %s\n无线电配置     %s\n频率           %lu kHz\n附近/联系人    %u\n广播/私信发送  %lu / %lu\nRSSI / SNR     %d dBm / %d dB\n电池           %u mV\nGPS            %s, %lu 字节" :
            "MeshCore       %s\nRadio profile  %s\nFrequency      %lu kHz\nNearby/contact %u\nTX flood/direct %lu / %lu\nRSSI / SNR     %d dBm / %d dB\nBattery        %u mV\nGPS            %s, %lu bytes",
            communication.ready() ? tr("Online", "在线") : tr("Offline", "离线"), preset.name, uint32_t(preset.frequency * 1000),
            unsigned(communication.peerCount()), meshService.sentFloodCount(), meshService.sentDirectCount(),
            communication.rssi(), int(communication.snr()), mv, hardware.gpsEnabled() ? tr("On", "开启") : tr("Off", "关闭"), hardware.gpsBytesSeen());
    }
}

void AppUI::handleTouch(uint16_t x, uint16_t y, bool pressed) {
    if ((volumeOverlay_ && !lv_obj_has_flag(volumeOverlay_, LV_OBJ_FLAG_HIDDEN)) || ftpPanel_) {
        if (pressed) noteUserActivity();
        edgeGestureTracking_ = topGestureTracking_ = notificationGestureTracking_ = false;
        return;
    }
    // Image paging is handled here instead of relying on LVGL's child gesture
    // bubbling. A fullscreen image covers the panel and otherwise consumes the
    // swipe as a click, so the parent never receives a reliable gesture event.
    if (filePreview_ && filePreviewImage_) {
        if (pressed) {
            noteUserActivity();
            if (!filePreviewTouchTracking_) {
                filePreviewTouchTracking_ = true;
                filePreviewTouchStartX_ = filePreviewTouchLastX_ = x;
                filePreviewTouchStartY_ = filePreviewTouchLastY_ = y;
            } else {
                filePreviewTouchLastX_ = x; filePreviewTouchLastY_ = y;
            }
            return;
        }
        if (filePreviewTouchTracking_) {
            filePreviewTouchTracking_ = false;
            const int dx = int(filePreviewTouchLastX_) - int(filePreviewTouchStartX_);
            const int dy = int(filePreviewTouchLastY_) - int(filePreviewTouchStartY_);
            if (abs(dx) >= 28 && abs(dx) > abs(dy)) {
                if (dx < 0 && previewImageIndex_ + 1 < previewImageCount_) showPreviewImage(previewImageIndex_ + 1);
                else if (dx > 0 && previewImageIndex_ > 0) showPreviewImage(previewImageIndex_ - 1);
                filePreviewSuppressClickUntil_ = millis() + 300;
            }
        }
        return;
    }
    if (pressed) {
        noteUserActivity();
        const bool emptyNotificationsVisible = notificationPanel_ &&
            !lv_obj_has_flag(notificationPanel_, LV_OBJ_FLAG_HIDDEN) && !notificationCount_;
        if (emptyNotificationsVisible && y > SAFE_TOP) {
            if (!notificationGestureTracking_) {
                notificationGestureTracking_ = true;
                gestureStartX_ = gestureLastX_ = x; gestureStartY_ = gestureLastY_ = y;
            } else { gestureLastX_ = x; gestureLastY_ = y; }
            return;
        }
        if (!topGestureTracking_ && !edgeGestureTracking_ && y <= SAFE_TOP) {
            topGestureTracking_ = true;
            gestureStartX_ = gestureLastX_ = x; gestureStartY_ = gestureLastY_ = y; gestureStartMs_ = millis();
        } else if (topGestureTracking_) {
            gestureLastX_ = x; gestureLastY_ = y;
        }
        if (!topGestureTracking_ && !edgeGestureTracking_ && y >= HOME_GESTURE_TOP &&
            x >= HOME_GESTURE_LEFT && x <= HOME_GESTURE_RIGHT) {
            edgeGestureTracking_ = true;
            gestureStartX_ = gestureLastX_ = x;
            gestureStartY_ = gestureLastY_ = y;
            gestureStartMs_ = millis();
        } else if (edgeGestureTracking_) {
            gestureLastX_ = x;
            gestureLastY_ = y;
            const int dx = constrain((int(gestureLastX_) - int(gestureStartX_)) / 3, -48, 48);
            const int dy = constrain((int(gestureLastY_) - int(gestureStartY_)) / 6, -11, 0);
            if (homeIndicator_) lv_obj_align(homeIndicator_, LV_ALIGN_BOTTOM_MID, dx, -3 + dy);
        }
        return;
    }
    if (notificationGestureTracking_) {
        notificationGestureTracking_ = false;
        if (int(gestureLastY_) - int(gestureStartY_) <= -24) showNotifications(false);
        return;
    }
    if (topGestureTracking_) {
        topGestureTracking_ = false;
        if (int(gestureLastY_) - int(gestureStartY_) >= 30) showNotifications(true);
        return;
    }
    if (!edgeGestureTracking_) return;
    edgeGestureTracking_ = false;
    if (homeIndicator_) lv_obj_align(homeIndicator_, LV_ALIGN_BOTTOM_MID, 0, -3);
    const int dx = int(gestureLastX_) - int(gestureStartX_);
    const int dy = int(gestureLastY_) - int(gestureStartY_);
    // readTouch runs inside LVGL's input callback. Dispose pages only after
    // input processing has released its keyboard/textarea pointers.
    if ((abs(dx) >= 38 && abs(dx) > abs(dy)) || dy <= -32) {
        const uintptr_t action = abs(dx) >= 38 && abs(dx) > abs(dy) ? (dx < 0 ? 2 : 3) : 1;
        lv_async_call([](void *data) {
            lv_indev_reset(nullptr, nullptr);
            pocketJsRuntime.dismissKeyboard();
            const uintptr_t action = reinterpret_cast<uintptr_t>(data);
            if (action == 1) appUI.navigateTo(0);
            else appUI.navigateRunning(action == 2);
        }, reinterpret_cast<void *>(action));
    }
}

void AppUI::handleSystemClick() {
    if (touchSoundEnabled_ && hardware.displayAwake()) hardware.playClickTone();
}

void AppUI::handleVolumeButton() {
    if (!hardware.displayAwake()) {
        hardware.toggleDisplayPower();
        noteUserActivity();
    }
    if (!volumeOverlay_) {
        volumeOverlay_ = lv_obj_create(lv_layer_top());
        lv_obj_set_pos(volumeOverlay_, 0, 0); lv_obj_set_size(volumeOverlay_, 320, 240);
        lv_obj_set_style_bg_color(volumeOverlay_, lv_color_black(), 0); lv_obj_set_style_bg_opa(volumeOverlay_, LV_OPA_50, 0);
        lv_obj_set_style_border_width(volumeOverlay_, 0, 0); lv_obj_set_style_radius(volumeOverlay_, 0, 0);
        lv_obj_set_style_pad_all(volumeOverlay_, 0, 0); lv_obj_clear_flag(volumeOverlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *panel = lv_obj_create(volumeOverlay_);
        lv_obj_set_pos(panel, 8, 22); lv_obj_set_size(panel, 304, 204);
        lv_obj_set_style_bg_color(panel, card(), 0); lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(panel, 0, 0); lv_obj_set_style_radius(panel, 20, 0);
        lv_obj_set_style_pad_all(panel, 0, 0); lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *title = lv_label_create(panel); lv_label_set_text(title, tr("Control Center", "控制中心"));
        labelStyle(title, &lv_font_ui_14, text()); lv_obj_set_pos(title, 14, 10);
        lv_obj_t *close = lv_button_create(panel); lv_obj_set_pos(close, 264, 5); lv_obj_set_size(close, 34, 30);
        lv_obj_set_style_bg_color(close, bg(), 0); lv_obj_set_style_shadow_width(close, 0, 0);
        lv_obj_t *closeText = lv_label_create(close); lv_label_set_text(closeText, LV_SYMBOL_CLOSE); labelStyle(closeText, &lv_font_montserrat_14, text()); lv_obj_center(closeText);
        lv_obj_add_event_cb(close, controlPanelClosed, LV_EVENT_CLICKED, this);
        auto toggleButton = [&](int x) {
            lv_obj_t *button = lv_button_create(panel);
            lv_obj_set_pos(button, x, 38); lv_obj_set_size(button, 92, 36);
            controlStyle(button); lv_obj_set_style_bg_color(button, bg(), 0);
            return button;
        };
        controlWifiButton_ = toggleButton(14);
        controlWifiIcon_ = lv_label_create(controlWifiButton_); lv_label_set_text(controlWifiIcon_, LV_SYMBOL_WIFI);
        labelStyle(controlWifiIcon_, &lv_font_montserrat_14, connectivity.wifiEnabled() ? BLUE : muted()); lv_obj_align(controlWifiIcon_, LV_ALIGN_LEFT_MID, 7, 0);
        lv_obj_t *wifiTitle = lv_label_create(controlWifiButton_); lv_label_set_text(wifiTitle, "Wi-Fi");
        labelStyle(wifiTitle, &lv_font_montserrat_14, text()); lv_obj_align(wifiTitle, LV_ALIGN_LEFT_MID, 33, 0);
        lv_obj_add_event_cb(controlWifiButton_, controlWifiChanged, LV_EVENT_CLICKED, this);
        lv_obj_t *controlLoRaButton = toggleButton(110);
        controlLoRaIcon_ = lv_label_create(controlLoRaButton); lv_label_set_text(controlLoRaIcon_, LORA_MARK);
        labelStyle(controlLoRaIcon_, &lv_font_montserrat_14, (strcmp(communication.protocol(), "off") != 0) ? ORANGE : muted()); lv_obj_align(controlLoRaIcon_, LV_ALIGN_LEFT_MID, 7, 0);
        lv_obj_t *loraTitle = lv_label_create(controlLoRaButton); lv_label_set_text(loraTitle, "LoRa");
        labelStyle(loraTitle, &lv_font_montserrat_14, text()); lv_obj_align(loraTitle, LV_ALIGN_LEFT_MID, 33, 0);
        lv_obj_add_event_cb(controlLoRaButton, controlLoRaChanged, LV_EVENT_CLICKED, this);
        controlGpsButton_ = toggleButton(206);
        controlGpsIcon_ = lv_label_create(controlGpsButton_); lv_label_set_text(controlGpsIcon_, LV_SYMBOL_GPS);
        labelStyle(controlGpsIcon_, &lv_font_montserrat_14, hardware.gpsEnabled() ? GREEN : muted()); lv_obj_align(controlGpsIcon_, LV_ALIGN_LEFT_MID, 7, 0);
        lv_obj_t *gpsTitle = lv_label_create(controlGpsButton_); lv_label_set_text(gpsTitle, "GPS");
        labelStyle(gpsTitle, &lv_font_montserrat_14, text()); lv_obj_align(gpsTitle, LV_ALIGN_LEFT_MID, 33, 0);
        lv_obj_add_event_cb(controlGpsButton_, controlGpsChanged, LV_EVENT_CLICKED, this);
        lv_obj_t *speaker = lv_label_create(panel); lv_label_set_text(speaker, LV_SYMBOL_VOLUME_MAX);
        labelStyle(speaker, &lv_font_montserrat_14, text()); lv_obj_set_pos(speaker, 14, 94);
        volumeBar_ = lv_slider_create(panel); lv_obj_set_pos(volumeBar_, 42, 97); lv_obj_set_size(volumeBar_, 238, 8);
        lv_slider_set_range(volumeBar_, 0, 100); lv_slider_set_value(volumeBar_, hardware.volume(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(volumeBar_, BLUE, LV_PART_INDICATOR);
        lv_obj_add_event_cb(volumeBar_, controlVolumeChanged, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_t *sun = lv_label_create(panel); lv_label_set_text(sun, LV_SYMBOL_EYE_OPEN);
        labelStyle(sun, &lv_font_montserrat_14, text()); lv_obj_set_pos(sun, 14, 124);
        lv_obj_t *brightness = lv_slider_create(panel); lv_obj_set_pos(brightness, 42, 127); lv_obj_set_size(brightness, 238, 8);
        lv_slider_set_range(brightness, 20, 255); lv_slider_set_value(brightness, hardware.display.backlight().brightness(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(brightness, ORANGE, LV_PART_INDICATOR);
        lv_obj_add_event_cb(brightness, controlBrightnessChanged, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_t *advert = lv_button_create(panel); lv_obj_set_pos(advert, 14, 156); lv_obj_set_size(advert, 132, 40);
        lv_obj_set_style_bg_color(advert, BLUE, 0); controlRebootLabel_ = lv_label_create(advert); lv_label_set_text(controlRebootLabel_, "Reboot"); lv_obj_center(controlRebootLabel_);
        lv_obj_add_event_cb(advert, controlRebootClicked, LV_EVENT_CLICKED, this);
        lv_obj_t *flood = lv_button_create(panel); lv_obj_set_pos(flood, 158, 156); lv_obj_set_size(flood, 132, 40);
        lv_obj_set_style_bg_color(flood, LV_COLOR_MAKE(0xAF,0x52,0xDE), 0); controlWebFilesLabel_ = lv_label_create(flood); lv_label_set_text(controlWebFilesLabel_, "Web Files"); lv_obj_center(controlWebFilesLabel_);
        lv_obj_add_event_cb(flood, controlWebFilesClicked, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(volumeOverlay_, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_obj_has_flag(volumeOverlay_, LV_OBJ_FLAG_HIDDEN)) {
        setKeyboardVisible(false); pocketJsRuntime.dismissKeyboard();
        closeWebFilesPanel(); closeFileActions();
        if (nodeAction_) lv_obj_add_flag(nodeAction_, LV_OBJ_FLAG_HIDDEN);
        showNotifications(false); showAppSwitcher(false);
        edgeGestureTracking_ = topGestureTracking_ = notificationGestureTracking_ = false;
        lv_slider_set_value(volumeBar_, hardware.volume(), LV_ANIM_OFF);
        if (controlWifiIcon_) lv_obj_set_style_text_color(controlWifiIcon_, connectivity.wifiEnabled() ? BLUE : muted(), 0);
        if (controlLoRaIcon_) lv_obj_set_style_text_color(controlLoRaIcon_, (strcmp(communication.protocol(), "off") != 0) ? ORANGE : muted(), 0);
        if (controlGpsIcon_) lv_obj_set_style_text_color(controlGpsIcon_, hardware.gpsEnabled() ? GREEN : muted(), 0);
        if (controlRebootLabel_) lv_label_set_text(controlRebootLabel_, "Reboot");
        if (controlWebFilesLabel_) lv_label_set_text(controlWebFilesLabel_, "Web Files");
        lv_obj_remove_flag(volumeOverlay_, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(volumeOverlay_);
    } else lv_obj_add_flag(volumeOverlay_, LV_OBJ_FLAG_HIDDEN);
}

void AppUI::controlPanelClosed(lv_event_t *event) { lv_obj_add_flag(static_cast<AppUI *>(lv_event_get_user_data(event))->volumeOverlay_, LV_OBJ_FLAG_HIDDEN); }
void AppUI::controlVolumeChanged(lv_event_t *event) {
    hardware.setVolume(lv_slider_get_value(lv_event_get_target_obj(event)));
}
void AppUI::controlBrightnessChanged(lv_event_t *event) { hardware.setBrightness(lv_slider_get_value(lv_event_get_target_obj(event))); }
void AppUI::controlGpsChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); hardware.setGpsEnabled(!hardware.gpsEnabled());
    if (self->controlGpsIcon_) lv_obj_set_style_text_color(self->controlGpsIcon_, hardware.gpsEnabled() ? GREEN : muted(), 0);
}
void AppUI::controlWifiChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); connectivity.setWifiEnabled(!connectivity.wifiEnabled());
    if (self->controlWifiIcon_) lv_obj_set_style_text_color(self->controlWifiIcon_, connectivity.wifiEnabled() ? BLUE : muted(), 0);
}
void AppUI::controlLoRaChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    communication.select(communication.ready() ? "off" : communication.configurationProtocol());
    if (self->controlLoRaIcon_) lv_obj_set_style_text_color(self->controlLoRaIcon_, (strcmp(communication.protocol(), "off") != 0) ? ORANGE : muted(), 0);
}
void AppUI::controlRebootClicked(lv_event_t *) {
    lv_timer_t *timer = lv_timer_create([](lv_timer_t *) { ESP.restart(); }, 150, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}
void AppUI::controlWebFilesClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    lv_obj_add_flag(self->volumeOverlay_, LV_OBJ_FLAG_HIDDEN);
    self->openWebFilesPanel();
}

void AppUI::noteUserActivity() {
    lastUserActivityMs_ = millis();
}

void AppUI::takeScreenshot() {
    if (!hardware.displayAwake()) return;
    if (!connectivity.sdMounted()) {
        Serial.println("Screenshot: SD card unavailable");
        return;
    }
    SD_MMC.mkdir("/screenshots");
    lv_refr_now(nullptr);
    lv_draw_buf_t *snapshot = takeDisplaySnapshot();
    if (!snapshot) {
        Serial.println("Screenshot: snapshot allocation failed");
        return;
    }
    char stamp[32];
    time_t raw = time_t(int64_t(systemClock.now()) + int64_t(timezoneMinutes_) * 60);
    struct tm value{}; gmtime_r(&raw, &value);
    snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d", value.tm_year + 1900, value.tm_mon + 1,
             value.tm_mday, value.tm_hour, value.tm_min, value.tm_sec);
    char path[96]; snprintf(path, sizeof(path), "/screenshots/screenshot_%s_%lu.jpg", stamp, (unsigned long)(millis() % 1000));
    File output = SD_MMC.open(path, FILE_WRITE);
    bool saved = bool(output);
    if (saved) {
        constexpr uint32_t width = 320, height = 240;
        // LVGL's RGB565 byte order is display-oriented and does not match the
        // camera encoder on this board. Convert explicitly to RGB888 so color
        // channels remain correct, then use a higher JPEG quality.
        uint8_t *rgb = static_cast<uint8_t *>(heap_caps_malloc(width * height * 3,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        saved = rgb != nullptr;
        for (uint32_t y = 0; saved && y < height; ++y) {
            const uint8_t *source = snapshot->data + y * snapshot->header.stride;
            for (uint32_t x = 0; x < width; ++x) {
                uint16_t pixel; memcpy(&pixel, source + x * 2, sizeof(pixel));
                rgb[(y * width + x) * 3] = uint8_t(((pixel >> 11) & 0x1F) * 255 / 31);
                rgb[(y * width + x) * 3 + 1] = uint8_t(((pixel >> 5) & 0x3F) * 255 / 63);
                rgb[(y * width + x) * 3 + 2] = uint8_t((pixel & 0x1F) * 255 / 31);
            }
        }
        if (saved) saved = fmt2jpg_cb(rgb, width * height * 3, width, height, PIXFORMAT_RGB888,
                                      94, jpegFileWriter, &output);
        if (rgb) free(rgb);
        output.close();
        if (!saved) SD_MMC.remove(path);
    }
    lv_draw_buf_destroy(snapshot);
    if (!saved) {
        Serial.println("Screenshot: write failed");
        return;
    }
    lv_obj_t *flash = lv_obj_create(lv_layer_top()); lv_obj_set_pos(flash, 0, 0); lv_obj_set_size(flash, 320, 240);
    lv_obj_set_style_bg_color(flash, lv_color_white(), 0); lv_obj_set_style_bg_opa(flash, LV_OPA_70, 0);
    lv_obj_set_style_border_width(flash, 0, 0); lv_obj_set_style_radius(flash, 0, 0);
    lv_obj_clear_flag(flash, LV_OBJ_FLAG_SCROLLABLE); lv_obj_clear_flag(flash, LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t fade; lv_anim_init(&fade); lv_anim_set_var(&fade, flash); lv_anim_set_values(&fade, LV_OPA_70, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade, 220); lv_anim_set_exec_cb(&fade, [](void *object, int32_t opacity) {
        lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(object), lv_opa_t(opacity), 0);
    }); lv_anim_start(&fade); lv_obj_delete_delayed(flash, 240);
    if (fileList_) refreshFiles();
    Serial.printf("Screenshot: saved %s\n", path);
}

void AppUI::toggleDisplayWithEffect() {
    if (displayEffectTimer_) return;
    // Waking is immediate. The CRT collapse is intentionally a power-off-only
    // transition so the screen never replays the closing effect in reverse.
    if (!hardware.displayAwake()) {
        for (lv_obj_t *object : {displayEffectTop_, displayEffectBottom_, displayEffectLine_})
            if (object) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        displayEffectLinePhase_ = false;
        hardware.toggleDisplayPower();
        noteUserActivity();
        return;
    }
    if (!displayEffectTop_) {
        displayEffectTop_ = lv_obj_create(lv_layer_top());
        displayEffectBottom_ = lv_obj_create(lv_layer_top());
        displayEffectLine_ = lv_obj_create(lv_layer_top());
        for (lv_obj_t *object : {displayEffectTop_, displayEffectBottom_}) {
            lv_obj_set_width(object, 320); lv_obj_set_style_bg_color(object, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0); lv_obj_set_style_border_width(object, 0, 0);
            lv_obj_set_style_radius(object, 0, 0); lv_obj_set_style_pad_all(object, 0, 0);
            lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE); lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_set_height(displayEffectLine_, 2); lv_obj_set_style_bg_color(displayEffectLine_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(displayEffectLine_, LV_OPA_COVER, 0); lv_obj_set_style_border_width(displayEffectLine_, 0, 0);
        lv_obj_set_style_radius(displayEffectLine_, 1, 0); lv_obj_clear_flag(displayEffectLine_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(displayEffectLine_, LV_OBJ_FLAG_CLICKABLE);
    }
    displayEffectLinePhase_ = false;
    for (lv_obj_t *object : {displayEffectTop_, displayEffectBottom_}) {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(object);
    }
    lv_obj_add_flag(displayEffectLine_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(displayEffectTop_, 0, 0); lv_obj_set_height(displayEffectTop_, 0);
    lv_obj_set_height(displayEffectBottom_, 120); lv_obj_set_y(displayEffectBottom_, 240);

    lv_anim_t animation;
    lv_anim_init(&animation); lv_anim_set_var(&animation, displayEffectTop_); lv_anim_set_values(&animation, 0, 120);
    lv_anim_set_duration(&animation, 240); lv_anim_set_path_cb(&animation, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&animation, [](void *object, int32_t value) { lv_obj_set_height(static_cast<lv_obj_t *>(object), value); }); lv_anim_start(&animation);
    lv_anim_init(&animation); lv_anim_set_var(&animation, displayEffectBottom_); lv_anim_set_values(&animation, 240, 120);
    lv_anim_set_duration(&animation, 240); lv_anim_set_path_cb(&animation, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&animation, [](void *object, int32_t value) { lv_obj_set_y(static_cast<lv_obj_t *>(object), value); }); lv_anim_start(&animation);
    displayEffectTimer_ = lv_timer_create(displayEffectFinished, 250, this);
    lv_timer_set_repeat_count(displayEffectTimer_, 1);
}

void AppUI::displayEffectFinished(lv_timer_t *timer) {
    auto *self = static_cast<AppUI *>(lv_timer_get_user_data(timer)); self->displayEffectTimer_ = nullptr;
    if (!self->displayEffectLinePhase_) {
        // Make the frame fully black before exposing the phosphor line. This
        // prevents any live pixels from remaining visible behind the line.
        lv_obj_set_height(self->displayEffectTop_, 120); lv_obj_set_y(self->displayEffectBottom_, 120);
        lv_obj_set_width(self->displayEffectLine_, 300);
        lv_obj_align(self->displayEffectLine_, LV_ALIGN_TOP_MID, 0, 119);
        lv_obj_remove_flag(self->displayEffectLine_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(self->displayEffectLine_);
        lv_anim_t line;
        lv_anim_init(&line); lv_anim_set_var(&line, self->displayEffectLine_); lv_anim_set_values(&line, 300, 8);
        lv_anim_set_duration(&line, 140); lv_anim_set_path_cb(&line, lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&line, [](void *object, int32_t value) {
            lv_obj_t *lineObject = static_cast<lv_obj_t *>(object);
            lv_obj_set_width(lineObject, value); lv_obj_align(lineObject, LV_ALIGN_TOP_MID, 0, 119);
        });
        lv_anim_start(&line);
        self->displayEffectLinePhase_ = true;
        self->displayEffectTimer_ = lv_timer_create(displayEffectFinished, 155, self);
        lv_timer_set_repeat_count(self->displayEffectTimer_, 1);
        return;
    }
    lv_obj_add_flag(self->displayEffectLine_, LV_OBJ_FLAG_HIDDEN);
    if (hardware.displayAwake()) hardware.toggleDisplayPower();
}

void AppUI::ensureApp(uint8_t page) {
    if (page >= PAGE_COUNT || pages_[page]) return;
    switch (page) {
        case 0: buildHome(); break;
        case 1: buildMessages(); break;
        case 2: buildNodes(); break;
        case 3: buildMap(); break;
        case 4: buildSettings(); break;
        case 5: buildNavigation(); break;
        case 6: buildPower(); break;
        case 7: buildDiagnostics(); break;
        case 8: navigateTo(0); break;
        case 9: buildFiles(); break;
        case 10: buildGpsApp(); break;
        default: buildInstalledApp(page - SYSTEM_PAGE_COUNT); break;
    }
}

void AppUI::destroyApp(uint8_t page) {
    if (!page || page >= PAGE_COUNT || !pages_[page]) return;
    if (page_ == page) {
        page_ = 0;
        if (appTitleLabel_) lv_label_set_text(appTitleLabel_, pageName(0));
        lv_obj_set_tile(tiles_, pages_[0], LV_ANIM_OFF);
    }
    if (page == 2 && nodeAction_) { lv_obj_delete(nodeAction_); nodeAction_ = nullptr; }
    if (page == pocketJsPage_) { pocketJsRuntime.unload(); pocketJsPage_ = 0xFF; }
    if (page == 5) {
    if (mapFullscreenOverlay_) {
        lv_obj_set_parent(mapViewport_, mapViewportParent_);
        lv_obj_delete(mapFullscreenOverlay_); mapFullscreenOverlay_ = nullptr;
    }
    }
    lv_obj_delete(pages_[page]); pages_[page] = nullptr;
    switch (page) {
        case 1:
            messageList_ = targetDropdown_ = channelLabel_ = messageSegment_ = channelTab_ = directTabButton_ = nullptr;
            compose_ = sendButton_ = nullptr; break;
        case 2:
            nodeList_ = nodeActionLabel_ = contactActionLabel_ = nullptr; break;
        case 3: radioSignalLabel_ = radioRoleDropdown_ = radioNote_ = radioRegionDropdown_ = radioProtocolDropdown_ = nullptr; radioLabel_ = radioDropdown_ = radioAdvertLabel_ = nullptr; break;
        case 4:
            brightnessValue_ = nameEdit_ = gpsStatus_ = loraStatus_ = nullptr;
            dateTimeEdit_ = timezoneDropdown_ = gpsTimeStatus_ = nullptr;
            wifiStatus_ = wifiSsid_ = wifiPassword_ = wifiNetworkDropdown_ = wifiScanLabel_ = nullptr;
            proxyHostEdit_ = proxyPortEdit_ = proxySaveLabel_ = nullptr;
            settingsRoot_ = settingsDetail_ = settingsDetailList_ = nullptr;
            sdStatus_ = formatSdLabel_ = nullptr; break;
        case 5:
            navigationStatus_ = mapStorageStatus_ = mapZoomDropdown_ = mapDownloadLabel_ = nullptr;
            for (unsigned i = 0; i < 9; ++i) {
                lv_image_cache_drop(&mapTileSources_[i]);
                if (mapTileData_[i]) heap_caps_free(mapTileData_[i]);
                mapTileData_[i] = nullptr;
            }
            mapRenderedZoom_ = 0;
            mapLatitudeEdit_ = mapLongitudeEdit_ = mapIpButtonLabel_ = nullptr;
            mapIpLocationApplied_ = false;
            mapViewport_ = mapCanvas_ = mapPositionPin_ = mapCoordinateLabel_ = mapZoomLabel_ = nullptr;
            mapCenteredOnGps_ = false; break;
        case 6: powerVoltage_ = powerPercent_ = powerBar_ = nullptr; break;
        case 7: diagnosticLabel_ = nullptr; break;
        case 9: closeFileActions(); closeFilePreview(); filePathLabel_ = fileList_ = nullptr; break;
        case 10:
            gpsInfoLabel_ = gpsRadarStatus_ = nullptr;
            memset(gpsRadarDots_, 0, sizeof(gpsRadarDots_));
            memset(gpsSatelliteRows_, 0, sizeof(gpsSatelliteRows_));
            memset(gpsSatelliteLabels_, 0, sizeof(gpsSatelliteLabels_));
            memset(gpsSatelliteDetailLabels_, 0, sizeof(gpsSatelliteDetailLabels_));
            memset(gpsSatelliteBars_, 0, sizeof(gpsSatelliteBars_));
            memset(gpsSatelliteSnrLabels_, 0, sizeof(gpsSatelliteSnrLabels_)); break;
        default: break;
    }
}

void AppUI::navigateRunning(bool forward) {
    if (recentCount_ < 2) return;
    int current = -1;
    for (uint8_t i = 0; i < recentCount_; ++i) if (recentOrder_[i] == page_) { current = i; break; }
    if (current < 0) { navigateTo(recentOrder_[recentCount_ - 1]); return; }
    const int next = forward ? (current + recentCount_ - 1) % recentCount_ : (current + 1) % recentCount_;
    navigateTo(recentOrder_[next]);
}

void AppUI::navigateTo(uint8_t page) {
    if (page >= PAGE_COUNT) return;
    if (loadingTimer_) return;
    if (volumeOverlay_) lv_obj_add_flag(volumeOverlay_, LV_OBJ_FLAG_HIDDEN);
    closeWebFilesPanel(); closeFileActions(); closeFilePreview();
    if (keyboard_ && !lv_obj_has_flag(keyboard_, LV_OBJ_FLAG_HIDDEN)) setKeyboardVisible(false);
    if (nodeAction_) lv_obj_add_flag(nodeAction_, LV_OBJ_FLAG_HIDDEN);
    showNotifications(false);
    showAppSwitcher(false);
    if (!pages_[page]) {
        pendingPage_ = page;
        loadingOverlay_ = lv_obj_create(lv_layer_top()); lv_obj_set_size(loadingOverlay_, 320, 240); lv_obj_set_pos(loadingOverlay_, 0, 0);
        lv_obj_set_style_bg_color(loadingOverlay_, bg(), 0); lv_obj_set_style_bg_opa(loadingOverlay_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(loadingOverlay_, 0, 0); lv_obj_set_style_radius(loadingOverlay_, 0, 0);
        lv_obj_clear_flag(loadingOverlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *spinner = lv_spinner_create(loadingOverlay_); lv_obj_set_size(spinner, 38, 38);
        lv_obj_set_style_arc_color(spinner, BLUE, LV_PART_INDICATOR); lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -10);
        lv_obj_t *label = lv_label_create(loadingOverlay_); lv_label_set_text(label, tr("Loading...", "正在加载..."));
        labelStyle(label, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, muted()); lv_obj_align(label, LV_ALIGN_CENTER, 0, 28);
        lv_obj_move_foreground(loadingOverlay_);
        loadingTimer_ = lv_timer_create(loadingFinished, 80, this);
        lv_timer_set_repeat_count(loadingTimer_, 1);
        return;
    }
    if (page == 1 && pocketJsPage_ != 1) applyNativeChatTarget();
    const uint8_t previousPage = page_;
    appOpened_[page] = true;
    uint8_t write = 0;
    for (uint8_t i = 0; i < recentCount_; ++i) if (recentOrder_[i] != page) recentOrder_[write++] = recentOrder_[i];
    recentCount_ = write; recentOrder_[recentCount_++] = page;
    page_ = page;
    if (systemBar_) {
        if (page == pocketJsPage_ && pocketJsRuntime.fullscreen()) lv_obj_add_flag(systemBar_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(systemBar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (appTitleLabel_) lv_label_set_text(appTitleLabel_, pageName(page_));
    lv_obj_set_tile(tiles_, pages_[page_], LV_ANIM_OFF);
    lv_obj_set_style_opa(pages_[page_], LV_OPA_TRANSP, 0);
    lv_obj_fade_in(pages_[page_], 150, 0);
    // Native tile pages stay resident until their PocketJS migration is
    // complete. Re-adding a deleted tile at the same coordinate can leave
    // LVGL's tileview focused on a stale layout. SD apps are safe to tombstone
    // because their page and JS realm are recreated together.
    if (previousPage != page_ && previousPage == pocketJsPage_) destroyApp(previousPage);
}

void AppUI::loadingFinished(lv_timer_t *timer) {
    auto *self = static_cast<AppUI *>(lv_timer_get_user_data(timer));
    const uint8_t page = self->pendingPage_;
    if (!self->pages_[page]) {
        self->ensureApp(page);
        lv_obj_update_layout(self->tiles_);
        self->loadingTimer_ = lv_timer_create(loadingFinished, 20, self);
        lv_timer_set_repeat_count(self->loadingTimer_, 1);
        return;
    }
    self->loadingTimer_ = nullptr;
    self->pendingPage_ = 0xFF;
    if (self->loadingOverlay_) { lv_obj_delete(self->loadingOverlay_); self->loadingOverlay_ = nullptr; }
    self->navigateTo(page);
}
void AppUI::showAppSwitcher(bool show) {
    if (!appSwitcher_) return;
    switcherVisible_ = show;
    if (show) {
        rebuildRecentCards();
        if (systemBar_) lv_obj_add_flag(systemBar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(appSwitcher_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(appSwitcher_); lv_obj_move_foreground(dock_);
        lv_obj_update_layout(appCardStrip_);
        lv_obj_scroll_to_y(appCardStrip_, 0, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(appSwitcher_, LV_OBJ_FLAG_HIDDEN);
        if (systemBar_) lv_obj_remove_flag(systemBar_, LV_OBJ_FLAG_HIDDEN);
        recentSwipeCard_ = nullptr;
        if (appCardStrip_) lv_obj_clean(appCardStrip_);
        for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
            if (recentSnapshots_[i]) { lv_draw_buf_destroy(recentSnapshots_[i]); recentSnapshots_[i] = nullptr; }
        }
    }
}

void AppUI::rebuildRecentCards() {
    if (!appCardStrip_) return;
    lv_obj_clean(appCardStrip_);
    for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
        if (recentSnapshots_[i]) { lv_draw_buf_destroy(recentSnapshots_[i]); recentSnapshots_[i] = nullptr; }
    }
    for (uint8_t offset = 0; offset < recentCount_; ++offset) {
        const uint8_t app = recentOrder_[recentCount_ - 1 - offset];
        lv_obj_t *appCard = lv_button_create(appCardStrip_); lv_obj_set_size(appCard, 145, 109);
        lv_obj_set_style_pad_all(appCard, 0, 0); lv_obj_set_style_bg_color(appCard, card(), 0);
        lv_obj_set_style_radius(appCard, 14, 0); lv_obj_set_style_clip_corner(appCard, true, 0);
        lv_obj_set_style_shadow_width(appCard, 5, 0); lv_obj_set_style_shadow_opa(appCard, LV_OPA_20, 0);
        lv_obj_clear_flag(appCard, LV_OBJ_FLAG_SCROLLABLE);
        recentSnapshots_[app] = lv_snapshot_take(pages_[app], LV_COLOR_FORMAT_RGB565);
        if (recentSnapshots_[app]) {
            lv_obj_t *preview = lv_image_create(appCard); lv_obj_set_pos(preview, 0, 0); lv_obj_set_size(preview, 145, 109);
            lv_image_set_src(preview, recentSnapshots_[app]); lv_image_set_inner_align(preview, LV_IMAGE_ALIGN_CONTAIN);
            lv_obj_clear_flag(preview, LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
        } else {
            const char *fallbackSymbol = app < SYSTEM_PAGE_COUNT ? APP_SYMBOLS[app] :
                manifestIcon(installedApps_[app - SYSTEM_PAGE_COUNT].icon);
            lv_obj_t *symbol = lv_label_create(appCard); lv_label_set_text(symbol, fallbackSymbol);
            labelStyle(symbol, &lv_font_montserrat_24, app ? BLUE : GREEN); lv_obj_center(symbol);
        }
        lv_obj_set_user_data(appCard, reinterpret_cast<void *>(uintptr_t(app)));
        lv_obj_add_event_cb(appCard, recentCardGesture, LV_EVENT_ALL, this);
    }
}

void AppUI::showNotifications(bool show) {
    if (!notificationPanel_) return;
    if (show) {
        refreshNotifications(); showAppSwitcher(false);
        if (notificationBannerTimer_) { lv_timer_delete(notificationBannerTimer_); notificationBannerTimer_ = nullptr; }
        if (notificationBanner_) lv_obj_set_y(notificationBanner_, -55);
        bannerNotificationId_ = 0;
        if (appTitleLabel_) {
            char date[16]; formatDateTime(systemClock.now(), date, sizeof(date), true, timezoneMinutes_);
            lv_label_set_text(appTitleLabel_, date); labelStyle(appTitleLabel_, &lv_font_montserrat_12, text());
            lv_obj_align(appTitleLabel_, LV_ALIGN_TOP_MID, 0, 2);
        }
        lv_obj_remove_flag(notificationPanel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(notificationPanel_); lv_obj_move_foreground(systemBar_); lv_obj_move_foreground(dock_);
    } else {
        lv_obj_add_flag(notificationPanel_, LV_OBJ_FLAG_HIDDEN);
        if (appTitleLabel_) {
            lv_label_set_text(appTitleLabel_, pageName(page_));
            labelStyle(appTitleLabel_, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, text());
            lv_obj_align(appTitleLabel_, LV_ALIGN_TOP_MID, 0, zh ? -5 : 2);
        }
    }
}

void AppUI::refreshNotifications() {
    if (!notificationList_) return;
    lv_obj_clean(notificationList_);
    if (!notificationCount_) {
        lv_obj_scroll_to_y(notificationList_, 0, LV_ANIM_OFF);
        lv_obj_clear_flag(notificationList_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(notificationList_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_t *empty = lv_label_create(notificationList_); lv_label_set_text(empty, tr("No new notifications", "没有新通知"));
        labelStyle(empty, &lv_font_ui_14, muted()); return;
    }
    lv_obj_add_flag(notificationList_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(notificationList_, LV_SCROLLBAR_MODE_OFF);
    const size_t first = notificationCount_ > 4 ? notificationCount_ - 4 : 0;
    for (size_t offset = notificationCount_; offset > first; --offset) {
        const Notification &notification = notifications_[offset - 1];
        lv_obj_t *item = lv_obj_create(notificationList_); lv_obj_set_width(item, 285); lv_obj_set_height(item, 38);
        lv_obj_set_style_bg_color(item, bg(), 0);
        lv_obj_set_style_border_width(item, 0, 0); lv_obj_set_style_radius(item, 13, 0);
        lv_obj_set_style_pad_hor(item, 6, 0); lv_obj_set_style_pad_ver(item, 0, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(item, reinterpret_cast<void *>(uintptr_t(notification.id)));
        lv_obj_add_event_cb(item, notificationClicked, LV_EVENT_CLICKED, this);
        lv_obj_t *line = lv_label_create(item); lv_label_set_text(line, notification.text);
        lv_obj_set_width(line, 270); lv_obj_set_height(line, 27); lv_obj_set_style_max_height(line, 27, 0);
        lv_label_set_long_mode(line, LV_LABEL_LONG_DOT);
        labelStyle(line, &lv_font_ui_14, text()); lv_obj_align(line, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

void AppUI::addNotification(const char *value, uint8_t targetPage, bool showBanner) {
    size_t slot = notificationCount_;
    if (slot >= 16) { memmove(notifications_, notifications_ + 1, sizeof(Notification) * 15); slot = 15; }
    else ++notificationCount_;
    strlcpy(notifications_[slot].text, value, sizeof(notifications_[slot].text));
    notifications_[slot].id = nextNotificationId_++;
    notifications_[slot].targetPage = targetPage;
    refreshNotifications();
    const bool notificationCenterOpen = notificationPanel_ && !lv_obj_has_flag(notificationPanel_, LV_OBJ_FLAG_HIDDEN);
    if (showBanner && !notificationCenterOpen) showNotificationBanner(notifications_[slot].id);
}

void AppUI::showNotificationBanner(uint32_t id) {
    if (!notificationBanner_ || !id) return;
    for (size_t i = 0; i < notificationCount_; ++i) if (notifications_[i].id == id) {
        bannerNotificationId_ = id; lv_label_set_text(notificationBannerLabel_, notifications_[i].text);
        if (notificationBannerTimer_) { lv_timer_delete(notificationBannerTimer_); notificationBannerTimer_ = nullptr; }
        lv_obj_remove_flag(notificationBanner_, LV_OBJ_FLAG_HIDDEN); lv_obj_set_y(notificationBanner_, -55);
        lv_obj_move_foreground(notificationBanner_);
        lv_anim_t animation; lv_anim_init(&animation); lv_anim_set_var(&animation, notificationBanner_);
        lv_anim_set_values(&animation, -55, 20); lv_anim_set_duration(&animation, 260); lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&animation, [](void *object, int32_t value) { lv_obj_set_y(static_cast<lv_obj_t *>(object), value); });
        lv_anim_start(&animation);
        notificationBannerTimer_ = lv_timer_create(notificationBannerTimeout, 4000, this);
        lv_timer_set_repeat_count(notificationBannerTimer_, 1);
        return;
    }
}

void AppUI::dismissNotification(uint32_t id, bool openTarget) {
    uint8_t targetPage = 0; size_t write = 0;
    for (size_t i = 0; i < notificationCount_; ++i) {
        if (notifications_[i].id == id) targetPage = notifications_[i].targetPage;
        else notifications_[write++] = notifications_[i];
    }
    notificationCount_ = write; refreshNotifications();
    if (bannerNotificationId_ == id) {
        bannerNotificationId_ = 0;
        if (notificationBannerTimer_) { lv_timer_delete(notificationBannerTimer_); notificationBannerTimer_ = nullptr; }
        if (notificationBanner_) lv_obj_set_y(notificationBanner_, -55);
    }
    if (openTarget && targetPage) navigateTo(targetPage);
}

void AppUI::nextPage() { navigateTo(0); }

bool AppUI::openInstalledAppById(const char *id) {
    if (!id || !id[0]) return false;
    for (uint8_t i = 0; i < installedAppCount_; ++i) {
        if (!strcmp(installedApps_[i].id, id)) {
            if (!hardware.displayAwake()) toggleDisplayWithEffect();
            noteUserActivity();
            navigateTo(SYSTEM_PAGE_COUNT + i);
            return true;
        }
    }
    return false;
}

bool AppUI::takeChatTarget(char *peerId, size_t size) {
    if (!peerId || !size) return false;
    const bool valid = pendingChatPeerId_[0] && !strcmp(pendingChatProtocol_, communication.configurationProtocol());
    strlcpy(peerId, valid ? pendingChatPeerId_ : "", size);
    pendingChatPeerId_[0] = 0; return valid;
}
void AppUI::setChatView(bool direct, const char *peerId, bool visible) {
    chatReadState_.view(communication.configurationProtocol(), direct, peerId, visible);
}
bool AppUI::chatUnread(bool direct) const { return chatReadState_.unread(communication.configurationProtocol(), direct); }
void AppUI::markChatRead(bool direct) { chatReadState_.read(communication.configurationProtocol(), direct); }

bool AppUI::setNotificationSound(bool direct, uint8_t style) {
    if (style > 4) return false;
    if (notificationSound(direct) == style) return true;
    Preferences p;
    if (!p.begin("mesh-ui", false)) return false;
    const bool saved = p.putUChar(direct ? "snd-dm-style" : "snd-ch-style", style) == 1;
    p.end();
    if (saved) { if (direct) directSoundStyle_ = style; else channelSoundStyle_ = style; }
    return saved;
}

void AppUI::onMeshMessage(const char *value, uint32_t timestamp, bool directMessage, bool outgoing,
                          bool contact, const char *peer, const char *peerId, uint32_t packetId) {
    const uint32_t savedId = chatStore.append(communication.protocol(), peerId, peer, timestamp, 1,
        reinterpret_cast<const uint8_t *>(value), strlen(value), directMessage, outgoing, contact, packetId);
    if (!savedId) Serial.printf("Chats: persistence warning %s\n", chatStore.error());
    const bool localEcho = outgoing || suppressNextMessagePopup_;
    suppressNextMessagePopup_ = false;
    size_t slot = messageCount_;
    if (slot >= MAX_MESSAGES) { memmove(messages_, messages_ + 1, sizeof(Message) * (MAX_MESSAGES - 1)); slot = MAX_MESSAGES - 1; }
    else ++messageCount_;
    messages_[slot].id = savedId;
    strlcpy(messages_[slot].peerId, peerId, sizeof(messages_[slot].peerId));
    strlcpy(messages_[slot].protocol, communication.protocol(), sizeof(messages_[slot].protocol));
    strlcpy(messages_[slot].text, value, sizeof(messages_[slot].text)); messages_[slot].timestamp = timestamp;
    strlcpy(messages_[slot].peer, peer ? peer : "", sizeof(messages_[slot].peer));
    messages_[slot].direct = directMessage; messages_[slot].outgoing = outgoing; messages_[slot].contact = contact;
    ++messageRevision_;
    char notice[128];
    if (directMessage) snprintf(notice, sizeof(notice), "%s  %s%s%s", tr("Direct message", "私信"),
                                peer && peer[0] ? peer : "", peer && peer[0] ? ": " : "", value);
    else snprintf(notice, sizeof(notice), "[Public] %s%s%s", peer && peer[0] ? peer : "",
                  peer && peer[0] ? ": " : "", value);
    refreshMessages();
    if (!localEcho) {
        const bool chatForeground = page_ == pocketJsPage_ && pocketJsRuntime.active() &&
            !strcmp(pocketJsRuntime.appDirectory(), "/apps/chats") && hardware.displayAwake() &&
            !switcherVisible_ && (!notificationPanel_ || lv_obj_has_flag(notificationPanel_, LV_OBJ_FLAG_HIDDEN));
        const bool seen = chatReadState_.receive(communication.protocol(), directMessage, peerId, chatForeground);
        if (!seen) addNotification(notice, 1, true);
        if (messageSoundEnabled_ && (directMessage ? directSoundEnabled_ : channelSoundEnabled_))
            hardware.playMessageTone(directMessage, notificationSound(directMessage));
    }
}

void AppUI::onMeshData(const char *peer, const char *peerId, uint32_t timestamp, uint16_t port,
                       const uint8_t *data, size_t size, bool direct, uint32_t packetId) {
    if (!chatStore.append(communication.protocol(), peerId, peer, timestamp, port, data, size, direct, false, false, packetId))
        Serial.printf("Chats: data persistence warning %s\n", chatStore.error());
}
void AppUI::restoreChatMessages() {
    auto *records = static_cast<ChatRecord *>(heap_caps_malloc(24 * sizeof(ChatRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!records) return;
    const size_t count = chatStore.history(records, 24, nullptr, nullptr, -1, 0, true);
    messageCount_ = count;
    for (size_t i = 0; i < count; ++i) {
        const auto &record = records[count - i - 1]; auto &message = messages_[i]; message = {};
        message.id = record.id; message.timestamp = record.timestamp;
        strlcpy(message.protocol, record.protocol, sizeof(message.protocol));
        strlcpy(message.peerId, record.peerId, sizeof(message.peerId)); strlcpy(message.peer, record.peer, sizeof(message.peer));
        memcpy(message.text, record.payload, record.length); message.text[record.length] = 0;
        message.direct = record.flags & 1; message.outgoing = record.flags & 2; message.contact = record.flags & 4;
    }
    heap_caps_free(records); ++messageRevision_; refreshMessages();
}
bool AppUI::deleteHistoryMessage(uint32_t id) {
    if (!chatStore.erase(id)) return false;
    for (size_t i = 0; i < messageCount_; ++i) if (messages_[i].id == id) {
        memmove(messages_ + i, messages_ + i + 1, (--messageCount_ - i) * sizeof(Message)); break;
    }
    ++messageRevision_; refreshMessages(); return true;
}
bool AppUI::chatMessageAt(size_t index, ChatMessageView &message) const {
    if (index >= messageCount_) return false;
    const Message &stored = messages_[index];
    message.protocol = stored.protocol;
    message.text = stored.text; message.peer = stored.peer; message.timestamp = stored.timestamp;
    message.direct = stored.direct; message.outgoing = stored.outgoing; message.contact = stored.contact;
    return true;
}

bool AppUI::deleteChatMessage(size_t index) {
    if (index >= messageCount_) return false;
    if (messages_[index].id && !chatStore.erase(messages_[index].id)) return false;
    if (index + 1 < messageCount_)
        memmove(messages_ + index, messages_ + index + 1, sizeof(Message) * (messageCount_ - index - 1));
    --messageCount_; ++messageRevision_; refreshMessages();
    return true;
}
void AppUI::onAdvertNotification(const char *name, int16_t snrQuarter) {
    const int whole = snrQuarter / 4; const int fraction = abs(snrQuarter % 4) * 25;
    char notice[128]; snprintf(notice, sizeof(notice), "Advert  %s  |  SNR %d.%02d dB",
                               name, whole, fraction);
    addNotification(notice, 2, false);
}
void AppUI::onAdvertTransmitted(bool flood) {
    if (radioAdvertLabel_) lv_label_set_text(radioAdvertLabel_, !strcmp(communication.protocol(), "meshtastic") ? "NodeInfo Sent" : tr("Advert Sent", "广播已发送"));

}
void AppUI::onNodeChanged() {
    if (chatStore.ready()) for (size_t i = 0; i < communication.peerCount(); ++i) {
        StoredPeer saved{}; strlcpy(saved.protocol, communication.configurationProtocol(), sizeof(saved.protocol));
        saved.peer = communication.peer(i);
        if (!strcmp(saved.protocol, "meshtastic")) meshtasticService.peerPublicKey(i, saved.publicKey);
        chatStore.upsertPeer(saved);
    }
    ++messageRevision_; // Peer names and active protocol affect the JS chat view.
    if (homeNodes_) lv_label_set_text_fmt(homeNodes_, zh ? "%u 个附近/联系人" : "%u nearby / contacts", unsigned(communication.peerCount()));
    refreshNodes(); refreshTargets(); refreshMessages();
}
void AppUI::onRadioState(bool ready) {
    if (!homeStatus_) return;
    if (ready) {
        lv_label_set_text(homeStatus_, tr("Online", "在线"));
        lv_obj_set_style_text_color(homeStatus_, GREEN, 0);
    } else if (!(strcmp(communication.protocol(), "off") != 0)) {
        lv_label_set_text(homeStatus_, tr("LoRa Off", "LoRa 已关闭"));
        lv_obj_set_style_text_color(homeStatus_, muted(), 0);
    } else {
        lv_label_set_text(homeStatus_, tr("Radio Unavailable", "无线电不可用"));
        lv_obj_set_style_text_color(homeStatus_, lv_palette_main(LV_PALETTE_RED), 0);
    }
}

void AppUI::sendClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); const char *value = lv_textarea_get_text(self->compose_);
    const uint16_t target = lv_dropdown_get_selected(self->targetDropdown_);
    self->suppressNextMessagePopup_ = true;
    const bool sent = self->directTab_ ? (target && target <= self->targetCount_ &&
        communication.sendDirect(self->targetNodeIndexes_[target - 1], value)) : communication.sendPublic(value);
    if (sent) lv_textarea_set_text(self->compose_, "");
    else self->suppressNextMessagePopup_ = false;
}
void AppUI::composeFocused(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); lv_keyboard_set_textarea(self->keyboard_, self->compose_); self->setKeyboardVisible(true);
}
void AppUI::keyboardReady(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY) {
        lv_obj_t *target = lv_keyboard_get_textarea(self->keyboard_);
        if (target == self->compose_) sendClicked(event);
        else if (target == self->nameEdit_) self->saveDeviceName();
        else if (target == self->dateTimeEdit_) self->saveDateTime();
    }
    self->setKeyboardVisible(false);
}
void AppUI::tileChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); lv_obj_t *active = lv_tileview_get_tile_active(self->tiles_);
    for (uint8_t i = 0; i < PAGE_COUNT; ++i) if (active == self->pages_[i]) self->page_ = i;
    if (self->appTitleLabel_) lv_label_set_text(self->appTitleLabel_, self->pageName(self->page_));
    if (self->keyboard_ && !lv_obj_has_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN)) self->setKeyboardVisible(false);
}
void AppUI::brightnessChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); const int value = lv_slider_get_value(lv_event_get_target_obj(event));
    hardware.setBrightness(value); if (self->brightnessValue_) lv_label_set_text_fmt(self->brightnessValue_, "%d%%", value * 100 / 255);
}
void AppUI::radioProtocolChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const unsigned selected = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    const bool ok = communication.select(selected == 2 ? "meshtastic" : selected == 1 ? "meshcore" : "off");
    self->refreshRadioControls(); if (!ok) lv_label_set_text(self->radioLabel_, communication.error());
}
void AppUI::radioPresetChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); const size_t index = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (!communication.selectRadioPreset(index)) {
        lv_dropdown_set_selected(self->radioDropdown_, communication.radioPresetIndex());
        lv_label_set_text(self->radioLabel_, tr("Could not save radio settings", "无法保存无线电设置")); return;
    }
    self->refreshRadioControls();
}
void AppUI::themeChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); self->darkMode_ = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    Preferences p; p.begin("mesh-ui", false); p.putBool("dark", self->darkMode_); p.end();
    lv_obj_add_state(lv_event_get_target_obj(event), LV_STATE_DISABLED); self->scheduleRebuild();
}
void AppUI::languageChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    self->chinese_ = lv_dropdown_get_selected(lv_event_get_target_obj(event)) == 1;
    Preferences p; p.begin("mesh-ui", false); p.putBool("zh", self->chinese_); p.end();
    lv_obj_add_state(lv_event_get_target_obj(event), LV_STATE_DISABLED);
    self->scheduleRebuild();
}
void AppUI::screenTimeoutChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    static const uint16_t values[] = {30, 60, 120, 300, 0};
    const uint16_t index = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    self->screenTimeoutSeconds_ = index < 5 ? values[index] : 60;
    Preferences p; p.begin("mesh-ui", false); p.putUShort("screen-timeout", self->screenTimeoutSeconds_); p.end();
    self->noteUserActivity();
    lv_display_trigger_activity(nullptr);
}
void AppUI::messageTabChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); self->directTab_ = lv_event_get_target_obj(event) == self->directTabButton_;
    lv_obj_set_style_bg_opa(self->channelTab_, self->directTab_ ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(self->directTabButton_, self->directTab_ ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (self->directTab_) {
        lv_obj_remove_flag(self->targetDropdown_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(self->messageList_, 8, 87); lv_obj_set_size(self->messageList_, 304, 99);
    } else {
        lv_obj_add_flag(self->targetDropdown_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(self->messageList_, 8, 58); lv_obj_set_size(self->messageList_, 304, 128);
    }
    self->refreshMessages();
}
void AppUI::nodeClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); self->showNodeAction(reinterpret_cast<size_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
}
void AppUI::showNodeAction(size_t index) {
    if (index >= communication.peerCount()) return; selectedNode_ = index; const CommunicationPeer node = communication.peer(index);
    lv_label_set_text_fmt(nodeActionLabel_, "%s\n%s", node.name, node.lastSeen ? tr("Nearby now", "当前在附近") : tr("Saved contact | Offline", "已保存联系人 | 离线"));
    lv_label_set_text(contactActionLabel_, node.contact ? tr("Remove from Contacts", "从联系人移除") : tr("Add to Contacts", "添加到联系人"));
    lv_obj_remove_flag(nodeAction_, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(nodeAction_);
}
void AppUI::chatWithNode(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self->selectedNode_ >= communication.peerCount()) return;
    const auto peer = communication.peer(self->selectedNode_);
    strlcpy(self->pendingChatPeerId_, peer.id, sizeof(self->pendingChatPeerId_));
    strlcpy(self->pendingChatProtocol_, communication.configurationProtocol(), sizeof(self->pendingChatProtocol_));
    if (!peer.contact) communication.setContact(self->selectedNode_, true);
    if (self->nodeAction_) lv_obj_add_flag(self->nodeAction_, LV_OBJ_FLAG_HIDDEN);
    self->navigateTo(1);
}
void AppUI::applyNativeChatTarget() {
    if (!pendingChatPeerId_[0] || !channelTab_ || !directTabButton_ || !targetDropdown_ ||
        strcmp(pendingChatProtocol_, communication.configurationProtocol())) return;
    directTab_ = true; refreshTargets();
    lv_obj_set_style_bg_opa(channelTab_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(directTabButton_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(targetDropdown_, LV_OBJ_FLAG_HIDDEN);
    for (size_t i = 0; i < targetCount_; ++i) {
        if (!strcmp(communication.peer(targetNodeIndexes_[i]).id, pendingChatPeerId_)) {
            lv_dropdown_set_selected(targetDropdown_, uint16_t(i + 1)); break;
        }
    }
    pendingChatPeerId_[0] = 0; refreshMessages();
}

void AppUI::contactAction(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self->selectedNode_ < communication.peerCount()) communication.setContact(self->selectedNode_, !communication.peer(self->selectedNode_).contact);
    lv_obj_add_flag(self->nodeAction_, LV_OBJ_FLAG_HIDDEN);
}
void AppUI::closeNodeAction(lv_event_t *event) { lv_obj_add_flag(static_cast<AppUI *>(lv_event_get_user_data(event))->nodeAction_, LV_OBJ_FLAG_HIDDEN); }
void AppUI::nameFocused(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); lv_keyboard_set_textarea(self->keyboard_, self->nameEdit_); self->setKeyboardVisible(true);
}
void AppUI::saveDeviceName() { if (communication.setNodeName(lv_textarea_get_text(nameEdit_))) lv_textarea_set_text(nameEdit_, communication.nodeName()); }
void AppUI::nameSaved(lv_event_t *event) { auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); self->saveDeviceName(); self->setKeyboardVisible(false); }
void AppUI::dateTimeFocused(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (!self->keyboard_) return;
    if (self->settingsDetailList_) {
        lv_obj_set_style_pad_bottom(self->settingsDetailList_, 125, 0);
        lv_obj_update_layout(self->settingsDetailList_);
        lv_obj_scroll_to_y(self->settingsDetailList_, 55, LV_ANIM_ON);
    }
    lv_keyboard_set_textarea(self->keyboard_, self->dateTimeEdit_);
    self->setKeyboardVisible(true);
}
void AppUI::dateTimeSaved(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); self->saveDateTime(); self->setKeyboardVisible(false);
}
void AppUI::timezoneChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const uint16_t index = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (index >= sizeof(TIMEZONE_VALUES) / sizeof(TIMEZONE_VALUES[0])) return;
    self->timezoneMinutes_ = TIMEZONE_VALUES[index];
    Preferences preferences; preferences.begin("mesh-ui", false); preferences.putShort("timezone", self->timezoneMinutes_); preferences.end();
    if (self->dateTimeEdit_) {
        char value[32]; formatDateTime(systemClock.now(), value, sizeof(value), false, self->timezoneMinutes_);
        lv_textarea_set_text(self->dateTimeEdit_, value);
    }
}
void AppUI::gpsAutoTimeChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    self->gpsAutoTime_ = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    if (!self->gpsAutoTime_) self->gpsClockSynced_ = false;
    self->lastGpsTimeApplied_ = 0;
    Preferences preferences; preferences.begin("mesh-ui", false); preferences.putBool("gps-auto-time", self->gpsAutoTime_); preferences.end();
}
void AppUI::broadcastGpsChanged(lv_event_t *event) {
    meshService.setBroadcastLocation(lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED));
}
void AppUI::saveDateTime() {
    if (!dateTimeEdit_) return;
    int year, month, day, hour, minute;
    const char *input = lv_textarea_get_text(dateTimeEdit_);
    const bool parsed = sscanf(input, "%d/%d/%d %d:%d", &year, &month, &day, &hour, &minute) == 5;
    static const uint8_t monthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int maximumDay = (month >= 1 && month <= 12) ? monthDays[month - 1] : 0;
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) maximumDay = 29;
    if (!parsed || year < 2020 || year > 2099 || day < 1 || day > maximumDay || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        lv_textarea_set_text(dateTimeEdit_, tr("Invalid date", "日期格式错误")); return;
    }
    const int64_t epoch = daysFromCivil(year, unsigned(month), unsigned(day)) * 86400LL + hour * 3600 + minute * 60 -
                          int64_t(timezoneMinutes_) * 60;
    systemClock.set(uint32_t(epoch));
    Preferences preferences; preferences.begin("mesh-ui", false); preferences.putULong("system-time", uint32_t(epoch)); preferences.end();
    char normalized[32]; formatDateTime(uint32_t(epoch), normalized, sizeof(normalized), false, timezoneMinutes_); lv_textarea_set_text(dateTimeEdit_, normalized);
}
void AppUI::gpsChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event)); const bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    hardware.setGpsEnabled(enabled); if (self->gpsStatus_) lv_label_set_text(self->gpsStatus_, enabled ? tr("On", "开启") : tr("Off", "关闭"));
}
void AppUI::loraChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    lv_obj_t *target = lv_event_get_target_obj(event);
    const bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    if (!communication.select(enabled ? "meshcore" : "off")) {
        lv_obj_clear_state(target, LV_STATE_CHECKED);
        return;
    }
    if (self->loraStatus_) lv_label_set_text(self->loraStatus_, enabled ? tr("On", "开启") : tr("Off", "关闭"));
    if (self->controlLoRaIcon_) lv_obj_set_style_text_color(self->controlLoRaIcon_, enabled ? ORANGE : muted(), 0);
}
void AppUI::txPowerChanged(lv_event_t *event) {
    static const int8_t powers[] = {10,14,17,20,22}; const uint16_t i = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (i < 5) meshService.setTxPower(powers[i]);
}
void AppUI::settingsCategoryClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const uint8_t category = uint8_t(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
    self->buildSettingsDetail(category);
    lv_obj_add_flag(self->settingsRoot_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(self->settingsDetail_, LV_OBJ_FLAG_HIDDEN);
}
void AppUI::settingsBackClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self->keyboard_ && !lv_obj_has_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN)) self->setKeyboardVisible(false);
    self->wifiScanActive_ = false;
    connectivity.clearWifiScan();
    lv_obj_add_flag(self->settingsDetail_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(self->settingsRoot_, LV_OBJ_FLAG_HIDDEN);
}
void AppUI::wifiScanClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self->wifiScanActive_) return;
    self->wifiScanActive_ = connectivity.startWifiScan();
    if (self->wifiScanLabel_) lv_label_set_text(self->wifiScanLabel_, self->wifiScanActive_ ? tr("Scanning...", "扫描中...") : tr("Retry", "重试"));
}
void AppUI::wifiNetworkChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const uint16_t selected = lv_dropdown_get_selected(lv_event_get_target_obj(event));
    if (selected && selected <= self->wifiScanCount_ && self->wifiSsid_)
        lv_textarea_set_text(self->wifiSsid_, self->wifiScanSsids_[selected - 1]);
}
void AppUI::wifiFieldFocused(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (!self->keyboard_) return;
    lv_obj_t *field = lv_event_get_target_obj(event);
    if (self->settingsDetailList_) {
        lv_obj_set_style_pad_bottom(self->settingsDetailList_, 112, 0);
        lv_obj_update_layout(self->settingsDetailList_);
        const int32_t scrollY = (field == self->proxyHostEdit_ || field == self->proxyPortEdit_) ? 168 :
                                (field == self->wifiPassword_ ? 94 : 18);
        lv_obj_scroll_to_y(self->settingsDetailList_, scrollY, LV_ANIM_ON);
    }
    lv_keyboard_set_textarea(self->keyboard_, field);
    self->setKeyboardVisible(true);
}
void AppUI::wifiConnectClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    connectivity.connectWifi(lv_textarea_get_text(self->wifiSsid_), lv_textarea_get_text(self->wifiPassword_));
    self->setKeyboardVisible(false);
    if (self->wifiStatus_) lv_label_set_text(self->wifiStatus_, tr("Connecting...", "正在连接..."));
}
void AppUI::proxySaved(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (!self->proxyHostEdit_ || !self->proxyPortEdit_) return;
    const char *host = lv_textarea_get_text(self->proxyHostEdit_);
    const unsigned long port = strtoul(lv_textarea_get_text(self->proxyPortEdit_), nullptr, 10);
    if (host[0] && (!port || port > 65535)) {
        if (self->proxySaveLabel_) lv_label_set_text(self->proxySaveLabel_, tr("Invalid port", "端口无效"));
        return;
    }
    connectivity.setHttpProxy(host, uint16_t(port));
    self->setKeyboardVisible(false);
    if (self->proxySaveLabel_) lv_label_set_text(self->proxySaveLabel_,
        connectivity.proxyEnabled() ? tr("Proxy Saved", "代理已保存") : tr("Proxy Disabled", "代理已关闭"));
}
void AppUI::soundSettingChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const uint8_t setting = uint8_t(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
    const bool enabled = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    const char *key = nullptr;
    switch (setting) {
        case 0: self->messageSoundEnabled_ = enabled; key = "snd-master"; break;
        case 1: self->channelSoundEnabled_ = enabled; key = "snd-channel"; break;
        case 2: self->directSoundEnabled_ = enabled; key = "snd-direct"; break;
        case 3: self->touchSoundEnabled_ = enabled; key = "snd-touch"; break;
        default: return;
    }
    Preferences preferences; preferences.begin("mesh-ui", false); preferences.putBool(key, enabled); preferences.end();
}
void AppUI::fpsSettingChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    self->fpsEnabled_ = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    if (self->fpsLabel_) {
        if (self->fpsEnabled_) lv_obj_remove_flag(self->fpsLabel_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(self->fpsLabel_, LV_OBJ_FLAG_HIDDEN);
    }
    self->fpsWindowStarted_ = millis(); self->fpsFrameCount_ = 0;
    if (self->memoryLabel_) lv_obj_align(self->memoryLabel_, LV_ALIGN_BOTTOM_LEFT, self->fpsEnabled_ ? 62 : 0, 0);
    Preferences preferences; preferences.begin("mesh-ui", false);
    preferences.putBool("dev-fps", self->fpsEnabled_); preferences.end();
}
void AppUI::memorySettingChanged(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    self->memoryEnabled_ = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    if (self->memoryLabel_) {
        if (self->memoryEnabled_) lv_obj_remove_flag(self->memoryLabel_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(self->memoryLabel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(self->memoryLabel_, LV_ALIGN_BOTTOM_LEFT, self->fpsEnabled_ ? 62 : 0, 0);
    }
    Preferences preferences; preferences.begin("mesh-ui", false);
    preferences.putBool("dev-memory", self->memoryEnabled_); preferences.end();
}
void AppUI::displayRefreshed(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self && self->fpsEnabled_ && self->fpsFrameCount_ < UINT16_MAX) ++self->fpsFrameCount_;
}
void AppUI::formatSdClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const uint32_t now = millis();
    if (!self->formatArmedUntil_ || int32_t(now - self->formatArmedUntil_) >= 0) {
        self->formatArmedUntil_ = now + 5000;
        lv_label_set_text(self->formatSdLabel_, tr("Tap again", "再次点击"));
        return;
    }
    self->formatArmedUntil_ = 0;
    connectivity.acknowledgeJob();
    connectivity.startFormatSd();
}
void AppUI::ftpToggleClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (connectivity.ftpRunning()) connectivity.stopFtp(); else connectivity.startFtp();
    const bool running = connectivity.ftpRunning();
    lv_obj_set_style_bg_color(lv_event_get_target_obj(event), running ? GREEN : BLUE, 0);
    if (self->ftpButtonLabel_) lv_label_set_text(self->ftpButtonLabel_, running ? tr("Stop Web", "停止网页") : tr("Start Web", "启动网页"));
    if (self->ftpStatus_) lv_label_set_text(self->ftpStatus_, running ? connectivity.ftpAddress().c_str() :
        (connectivity.wifiConnected() ? tr("Web Files stopped", "网页文件管理已停止") : tr("Connect Wi-Fi first", "请先连接 Wi-Fi")));
}
void AppUI::ftpPanelOpened(lv_event_t *event) {
    static_cast<AppUI *>(lv_event_get_user_data(event))->openWebFilesPanel();
}
void AppUI::openWebFilesPanel() {
    auto *self = this;
    if (self->ftpPanel_) return;
    setKeyboardVisible(false); pocketJsRuntime.dismissKeyboard();
    self->ftpPanel_ = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(self->ftpPanel_, 0, 0); lv_obj_set_size(self->ftpPanel_, 320, 240);
    lv_obj_set_style_bg_color(self->ftpPanel_, lv_color_black(), 0); lv_obj_set_style_bg_opa(self->ftpPanel_, LV_OPA_50, 0);
    lv_obj_set_style_border_width(self->ftpPanel_, 0, 0); lv_obj_set_style_radius(self->ftpPanel_, 0, 0);
    lv_obj_set_style_pad_all(self->ftpPanel_, 0, 0); lv_obj_remove_flag(self->ftpPanel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *panel = lv_obj_create(self->ftpPanel_); lv_obj_set_pos(panel, 8, 22); lv_obj_set_size(panel, 304, 204);
    lv_obj_set_style_bg_color(panel, card(), 0); lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0); lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 8, 0); lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(panel); lv_label_set_text(title, tr("Web Files", "网页文件管理"));
    labelStyle(title, zh ? &lv_font_ui_14 : &lv_font_montserrat_16, text()); lv_obj_set_pos(title, 8, 5);
    lv_obj_t *close = lv_button_create(panel); lv_obj_set_pos(close, 250, -3); lv_obj_set_size(close, 38, 32);
    lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0); lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_t *closeLabel = lv_label_create(close); lv_label_set_text(closeLabel, LV_SYMBOL_CLOSE);
    labelStyle(closeLabel, &lv_font_montserrat_14, BLUE); lv_obj_center(closeLabel);
    lv_obj_add_event_cb(close, ftpPanelClosed, LV_EVENT_CLICKED, self);
    self->ftpStatus_ = lv_label_create(panel);
    lv_label_set_text(self->ftpStatus_, connectivity.ftpRunning() ? connectivity.ftpAddress().c_str() :
        (connectivity.wifiConnected() ? tr("Web editor stopped", "网页编辑器已停止") : tr("Connect Wi-Fi first", "请先连接 Wi-Fi")));
    labelStyle(self->ftpStatus_, &lv_font_montserrat_12, text()); lv_obj_set_pos(self->ftpStatus_, 8, 32); lv_obj_set_width(self->ftpStatus_, 272);
    self->ftpUsernameEdit_ = lv_textarea_create(panel); lv_obj_set_pos(self->ftpUsernameEdit_, 8, 55); lv_obj_set_size(self->ftpUsernameEdit_, 130, 34);
    lv_textarea_set_one_line(self->ftpUsernameEdit_, true); lv_textarea_set_max_length(self->ftpUsernameEdit_, 32);
    lv_textarea_set_placeholder_text(self->ftpUsernameEdit_, tr("Username", "用户名")); lv_textarea_set_text(self->ftpUsernameEdit_, connectivity.ftpUsername());
    controlStyle(self->ftpUsernameEdit_); lv_obj_set_style_bg_opa(self->ftpUsernameEdit_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(self->ftpUsernameEdit_, 1, 0); lv_obj_set_style_border_color(self->ftpUsernameEdit_, muted(), 0);
    lv_obj_add_event_cb(self->ftpUsernameEdit_, wifiFieldFocused, LV_EVENT_FOCUSED, self);
    self->ftpPasswordEdit_ = lv_textarea_create(panel); lv_obj_set_pos(self->ftpPasswordEdit_, 146, 55); lv_obj_set_size(self->ftpPasswordEdit_, 142, 34);
    lv_textarea_set_one_line(self->ftpPasswordEdit_, true); lv_textarea_set_max_length(self->ftpPasswordEdit_, 32);
    lv_textarea_set_password_mode(self->ftpPasswordEdit_, true); lv_textarea_set_placeholder_text(self->ftpPasswordEdit_, tr("Password (optional)", "密码（可选）"));
    lv_textarea_set_text(self->ftpPasswordEdit_, connectivity.ftpPassword()); controlStyle(self->ftpPasswordEdit_);
    lv_obj_set_style_bg_opa(self->ftpPasswordEdit_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(self->ftpPasswordEdit_, 1, 0); lv_obj_set_style_border_color(self->ftpPasswordEdit_, muted(), 0);
    lv_obj_add_event_cb(self->ftpPasswordEdit_, wifiFieldFocused, LV_EVENT_FOCUSED, self);
    lv_obj_t *save = lv_button_create(panel); lv_obj_set_pos(save, 120, 97); lv_obj_set_size(save, 104, 30);
    lv_obj_set_style_bg_color(save, BLUE, 0); lv_obj_set_style_radius(save, 9, 0);
    lv_obj_t *saveLabel = lv_label_create(save); lv_label_set_text(saveLabel, tr("Save login", "保存登录"));
    labelStyle(saveLabel, &lv_font_ui_14, lv_color_white()); lv_obj_center(saveLabel);
    lv_obj_add_event_cb(save, ftpCredentialsSaved, LV_EVENT_CLICKED, self);
    lv_obj_t *toggle = lv_button_create(panel); lv_obj_set_pos(toggle, 8, 97); lv_obj_set_size(toggle, 104, 30);
    lv_obj_set_style_bg_color(toggle, connectivity.ftpRunning() ? GREEN : BLUE, 0); lv_obj_set_style_radius(toggle, 10, 0);
    self->ftpButtonLabel_ = lv_label_create(toggle);
    lv_label_set_text(self->ftpButtonLabel_, connectivity.ftpRunning() ? tr("Stop Web", "停止网页") : tr("Start Web", "启动网页"));
    labelStyle(self->ftpButtonLabel_, &lv_font_ui_14, lv_color_white()); lv_obj_center(self->ftpButtonLabel_);
    lv_obj_add_event_cb(toggle, ftpToggleClicked, LV_EVENT_CLICKED, self);
    lv_obj_t *guest = lv_label_create(panel);
    lv_label_set_text(guest, tr("Open the address in a browser\nDrag a PocketJS ZIP to install", "用浏览器打开上方地址\n拖入 PocketJS ZIP 即可安装"));
    labelStyle(guest, zh ? &lv_font_ui_14 : &lv_font_montserrat_12, muted()); lv_obj_set_pos(guest, 8, 139); lv_obj_set_width(guest, 272);
}
void AppUI::ftpPanelClosed(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    self->closeWebFilesPanel();
}
void AppUI::closeWebFilesPanel() {
    auto *self = this;
    if (!self->ftpPanel_) return;
    if (self->keyboard_ && (lv_keyboard_get_textarea(self->keyboard_) == self->ftpUsernameEdit_ ||
                           lv_keyboard_get_textarea(self->keyboard_) == self->ftpPasswordEdit_)) {
        lv_keyboard_set_textarea(self->keyboard_, nullptr); self->setKeyboardVisible(false);
    }
    lv_obj_delete(self->ftpPanel_); self->ftpPanel_ = nullptr; self->ftpStatus_ = self->ftpButtonLabel_ = nullptr;
    self->ftpUsernameEdit_ = self->ftpPasswordEdit_ = nullptr;
}
void AppUI::ftpCredentialsSaved(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (!self->ftpUsernameEdit_ || !self->ftpPasswordEdit_) return;
    connectivity.setFtpCredentials(lv_textarea_get_text(self->ftpUsernameEdit_), lv_textarea_get_text(self->ftpPasswordEdit_));
    self->setKeyboardVisible(false);
    if (self->ftpStatus_) lv_label_set_text(self->ftpStatus_, tr("Login saved; restart Web", "登录已保存；请重启网页服务"));
}
void AppUI::downloadMapClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    double latitude = hardware.gpsLatitude(), longitude = hardware.gpsLongitude();
    const char *latText = self->mapLatitudeEdit_ ? lv_textarea_get_text(self->mapLatitudeEdit_) : "";
    const char *lonText = self->mapLongitudeEdit_ ? lv_textarea_get_text(self->mapLongitudeEdit_) : "";
    if (latText[0] || lonText[0]) {
        char *latEnd = nullptr, *lonEnd = nullptr;
        latitude = strtod(latText, &latEnd); longitude = strtod(lonText, &lonEnd);
        if (latEnd == latText || lonEnd == lonText || *latEnd || *lonEnd ||
            !isfinite(latitude) || !isfinite(longitude) || latitude < -85 || latitude > 85 ||
            longitude < -180 || longitude > 180) {
            lv_label_set_text(self->mapStorageStatus_, tr("Invalid latitude or longitude", "经纬度无效"));
            return;
        }
    } else if (!hardware.gpsHasFix()) {
        lv_label_set_text(self->mapStorageStatus_, tr("Enter coordinates or use IP location", "输入坐标或使用 IP 定位"));
        return;
    }
    connectivity.acknowledgeJob();
    const uint8_t zoom = 12 + lv_dropdown_get_selected(self->mapZoomDropdown_);
    self->mapZoom_ = zoom; self->updateMapView(true);
    connectivity.startMapDownload(latitude, longitude, zoom);
}

void AppUI::mapIpClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    self->mapIpLocationApplied_ = false;
    connectivity.startIpLocation();
}

void AppUI::mapCoordinateFocused(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    self->ensureKeyboard();
    lv_keyboard_set_textarea(self->keyboard_, lv_event_get_target_obj(event));
    self->setKeyboardVisible(true);
}

void AppUI::mapZoomInClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self->mapZoom_ < 16) ++self->mapZoom_;
    self->updateMapView(true);
}

void AppUI::mapZoomOutClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self->mapZoom_ > 10) --self->mapZoom_;
    self->updateMapView(true);
}

void AppUI::mapFullscreenClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (!self->mapFullscreenOverlay_) {
        self->setKeyboardVisible(false);
        self->mapViewportParent_ = lv_obj_get_parent(self->mapViewport_);
        self->mapFullscreenOverlay_ = lv_obj_create(lv_layer_top());
        lv_obj_set_pos(self->mapFullscreenOverlay_, 0, 0); lv_obj_set_size(self->mapFullscreenOverlay_, 320, 240);
        lv_obj_set_style_pad_all(self->mapFullscreenOverlay_, 0, 0); lv_obj_set_style_border_width(self->mapFullscreenOverlay_, 0, 0);
        lv_obj_set_style_radius(self->mapFullscreenOverlay_, 0, 0); lv_obj_remove_flag(self->mapFullscreenOverlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_parent(self->mapViewport_, self->mapFullscreenOverlay_);
        lv_obj_set_pos(self->mapViewport_, 0, 0); lv_obj_set_size(self->mapViewport_, 320, 240);
        lv_obj_t *exit = lv_button_create(self->mapFullscreenOverlay_); lv_obj_set_pos(exit, 264, 8); lv_obj_set_size(exit, 48, 34);
        lv_obj_t *label = lv_label_create(exit); lv_label_set_text(label, LV_SYMBOL_CLOSE); lv_obj_center(label);
        lv_obj_add_event_cb(exit, mapFullscreenClicked, LV_EVENT_CLICKED, self);
    } else {
        lv_obj_set_parent(self->mapViewport_, self->mapViewportParent_);
        lv_obj_set_pos(self->mapViewport_, 0, 0); lv_obj_set_size(self->mapViewport_, 300, 142);
        lv_obj_move_to_index(self->mapViewport_, 0);
        lv_obj_update_layout(self->mapViewportParent_);
        lv_obj_scroll_to(self->mapViewportParent_, 0, 0, LV_ANIM_OFF);
        lv_obj_delete(self->mapFullscreenOverlay_); self->mapFullscreenOverlay_ = nullptr;
    }
    lv_obj_update_layout(self->mapViewport_); self->updateMapView(true);
}

void AppUI::mapRecenterClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    self->updateMapView(true);
}
void AppUI::fileEntryClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const uint8_t index = uint8_t(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
    if (index >= self->fileEntryCount_) return;
    if (self->fileEntryDirectories_[index]) {
        strlcpy(self->currentFilePath_, self->fileEntryPaths_[index], sizeof(self->currentFilePath_));
        self->refreshFiles();
    } else self->showFileActions(index);
}
void AppUI::fileMoreClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const uint8_t index = uint8_t(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
    self->showFileActions(index);
}
void AppUI::fileBackClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (!strcmp(self->currentFilePath_, "/")) { self->navigateTo(0); return; }
    char *slash = strrchr(self->currentFilePath_, '/');
    if (!slash || slash == self->currentFilePath_) strlcpy(self->currentFilePath_, "/", sizeof(self->currentFilePath_));
    else *slash = 0;
    self->refreshFiles();
}
void AppUI::fileActionPreviewClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self->selectedFileEntry_ >= self->fileEntryCount_) return;
    char path[128]; strlcpy(path, self->fileEntryPaths_[self->selectedFileEntry_], sizeof(path));
    const bool directory = self->fileEntryDirectories_[self->selectedFileEntry_];
    self->closeFileActions();
    if (directory) { strlcpy(self->currentFilePath_, path, sizeof(self->currentFilePath_)); self->refreshFiles(); }
    else self->showFilePreview(path);
}
void AppUI::fileActionRenameClicked(lv_event_t *event) {
    static_cast<AppUI *>(lv_event_get_user_data(event))->showFileRename();
}
void AppUI::fileActionDeleteClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self->selectedFileEntry_ >= self->fileEntryCount_) return;
    const uint32_t now = millis();
    if (!self->fileDeleteArmedUntil_ || int32_t(now - self->fileDeleteArmedUntil_) > 0) {
        self->fileDeleteArmedUntil_ = now + 4000;
        if (self->fileDeleteLabel_) lv_label_set_text(self->fileDeleteLabel_, tr("Tap again to delete", "再次点击确认删除"));
        return;
    }
    char path[128]; strlcpy(path, self->fileEntryPaths_[self->selectedFileEntry_], sizeof(path));
    const bool directory = self->fileEntryDirectories_[self->selectedFileEntry_];
    const bool removed = directory ? SD_MMC.rmdir(path) : SD_MMC.remove(path);
    if (!removed) {
        self->fileDeleteArmedUntil_ = 0;
        if (self->fileDeleteLabel_) lv_label_set_text(self->fileDeleteLabel_,
            directory ? tr("Folder must be empty", "只能删除空文件夹") : tr("Delete failed", "删除失败"));
        return;
    }
    self->closeFileActions(); self->refreshFiles();
    Serial.printf("Files: delete %s %s\n", path, removed ? "ok" : "failed");
}
void AppUI::fileActionClosed(lv_event_t *event) {
    static_cast<AppUI *>(lv_event_get_user_data(event))->closeFileActions();
}
void AppUI::fileRenameSaved(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (!self->fileRenameEdit_ || self->selectedFileEntry_ >= self->fileEntryCount_) return;
    const char *name = lv_textarea_get_text(self->fileRenameEdit_);
    if (!name[0] || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/') || strchr(name, '\\')) {
        lv_obj_set_style_border_color(self->fileRenameEdit_, LV_COLOR_MAKE(0xFF,0x3B,0x30), 0);
        lv_obj_set_style_border_width(self->fileRenameEdit_, 2, 0); return;
    }
    const char *oldPath = self->fileEntryPaths_[self->selectedFileEntry_];
    char newPath[128]; const char *slash = strrchr(oldPath, '/');
    if (!slash || slash == oldPath) snprintf(newPath, sizeof(newPath), "/%s", name);
    else snprintf(newPath, sizeof(newPath), "%.*s/%s", int(slash - oldPath), oldPath, name);
    if ((strcmp(oldPath, newPath) && SD_MMC.exists(newPath)) || !SD_MMC.rename(oldPath, newPath)) {
        lv_obj_set_style_border_color(self->fileRenameEdit_, LV_COLOR_MAKE(0xFF,0x3B,0x30), 0);
        lv_obj_set_style_border_width(self->fileRenameEdit_, 2, 0); return;
    }
    self->closeFileActions(); self->refreshFiles();
}
void AppUI::filePreviewClosed(lv_event_t *event) {
    static_cast<AppUI *>(lv_event_get_user_data(event))->closeFilePreview();
}
void AppUI::filePreviewImageClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (!self->filePreviewImage_) return;
    if (self->filePreviewSuppressClickUntil_ &&
        int32_t(millis() - self->filePreviewSuppressClickUntil_) < 0) return;
    self->filePreviewSuppressClickUntil_ = 0;
    self->filePreviewFullscreen_ = !self->filePreviewFullscreen_;
    lv_obj_set_style_bg_color(self->filePreview_, self->filePreviewFullscreen_ ? lv_color_black() : bg(), 0);
    if (self->filePreviewFullscreen_) {
        lv_obj_add_flag(self->filePreviewBack_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(self->filePreviewTitle_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(self->filePreviewBack_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(self->filePreviewTitle_, LV_OBJ_FLAG_HIDDEN);
    }
    self->showPreviewImage(self->previewImageIndex_);
}
void AppUI::filePreviewGesture(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    if (self->previewImageCount_ < 2) return;
    const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    if (direction == LV_DIR_LEFT && self->previewImageIndex_ + 1 < self->previewImageCount_)
        self->showPreviewImage(self->previewImageIndex_ + 1);
    else if (direction == LV_DIR_RIGHT && self->previewImageIndex_ > 0)
        self->showPreviewImage(self->previewImageIndex_ - 1);
}
void AppUI::appIconClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const uint8_t page = uint8_t(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
    if (page == 9) {
        strlcpy(self->currentFilePath_, "/", sizeof(self->currentFilePath_));
        self->refreshFiles();
    }
    self->navigateTo(page);
}
void AppUI::homeClicked(lv_event_t *event) { static_cast<AppUI *>(lv_event_get_user_data(event))->navigateTo(0); }
void AppUI::advertiseClicked(lv_event_t *event) {
    const bool queued = communication.advertise();
    lv_obj_t *button = lv_event_get_target_obj(event); lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label) lv_label_set_text(label, queued ? tr("Sending...", "正在发送…") : tr("Advert Failed", "广播发送失败"));
}
void AppUI::dockGesture(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
    if (direction == LV_DIR_TOP) {
        if (self->notificationPanel_ && !lv_obj_has_flag(self->notificationPanel_, LV_OBJ_FLAG_HIDDEN)) self->showNotifications(false);
        else self->navigateTo(0);
    }
    else if (direction == LV_DIR_LEFT) self->navigateRunning(true);
    else if (direction == LV_DIR_RIGHT) self->navigateRunning(false);
}
void AppUI::recentCardGesture(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    lv_obj_t *cardObject = lv_event_get_current_target_obj(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED) {
        if (!self->recentSwipeMoved_) {
            const uint8_t app = uint8_t(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(cardObject)));
            self->navigateTo(app);
        }
        self->recentSwipeMoved_ = false;
        return;
    }

    lv_point_t point{};
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &point);
    if (code == LV_EVENT_PRESSED) {
        self->recentSwipeCard_ = cardObject;
        self->recentSwipeStartX_ = point.x;
        self->recentSwipeStartY_ = point.y;
        self->recentSwipeMoved_ = false;
        lv_obj_set_style_opa(cardObject, LV_OPA_COVER, 0);
        return;
    }
    if (self->recentSwipeCard_ != cardObject) return;
    const int32_t dx = point.x - self->recentSwipeStartX_;
    const int32_t dy = point.y - self->recentSwipeStartY_;
    if (code == LV_EVENT_PRESSING) {
        if (abs(dx) > 6 || abs(dy) > 6) self->recentSwipeMoved_ = true;
        if (dy < 0 && abs(dy) > abs(dx)) {
            const int32_t offset = LV_MAX(dy, -105);
            lv_obj_set_style_translate_y(cardObject, offset, 0);
            lv_obj_set_style_opa(cardObject, lv_opa_t(LV_MAX(70, 255 + offset * 2)), 0);
        }
        return;
    }
    if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;

    const int32_t offset = lv_obj_get_style_translate_y(cardObject, LV_PART_MAIN);
    const bool dismiss = offset <= -28;
    lv_anim_t move;
    lv_anim_init(&move); lv_anim_set_var(&move, cardObject);
    lv_anim_set_values(&move, offset, dismiss ? -112 : 0); lv_anim_set_duration(&move, dismiss ? 180 : 140);
    lv_anim_set_path_cb(&move, dismiss ? lv_anim_path_ease_in : lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&move, [](void *object, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(object), value, 0);
    });
    lv_anim_start(&move);

    lv_anim_t fade;
    lv_anim_init(&fade); lv_anim_set_var(&fade, cardObject);
    lv_anim_set_values(&fade, lv_obj_get_style_opa(cardObject, LV_PART_MAIN), dismiss ? LV_OPA_TRANSP : LV_OPA_COVER);
    lv_anim_set_duration(&fade, dismiss ? 160 : 120);
    lv_anim_set_exec_cb(&fade, [](void *object, int32_t value) {
        lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), lv_opa_t(value), 0);
    });
    lv_anim_start(&fade);

    if (!dismiss) {
        self->recentSwipeCard_ = nullptr;
        return;
    }
    const uint8_t app = uint8_t(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(cardObject)));
    self->appOpened_[app] = false;
    uint8_t write = 0;
    for (uint8_t i = 0; i < self->recentCount_; ++i) if (self->recentOrder_[i] != app) self->recentOrder_[write++] = self->recentOrder_[i];
    self->recentCount_ = write;
    if (self->page_ == app) {
        const uint8_t fallback = self->recentCount_ ? self->recentOrder_[self->recentCount_ - 1] : 0;
        self->ensureApp(fallback);
        self->page_ = fallback;
        if (self->appTitleLabel_) lv_label_set_text(self->appTitleLabel_, self->pageName(fallback));
        lv_obj_set_tile(self->tiles_, self->pages_[fallback], LV_ANIM_OFF);
    }
    self->destroyApp(app);
    self->recentSwipeCard_ = nullptr;
    lv_obj_remove_flag(cardObject, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_delete_delayed(cardObject, 190);
    lv_timer_t *settle = lv_timer_create([](lv_timer_t *timer) {
        auto *ui = static_cast<AppUI *>(lv_timer_get_user_data(timer));
        if (!ui->switcherVisible_) return;
        if (!ui->recentCount_) ui->navigateTo(0);
        else {
            ui->rebuildRecentCards();
            lv_obj_update_layout(ui->appCardStrip_);
            lv_obj_scroll_to_x(ui->appCardStrip_, 0, LV_ANIM_ON);
        }
    }, 220, self);
    lv_timer_set_repeat_count(settle, 1);
}
void AppUI::clearNotificationsClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    self->notificationCount_ = 0;
    memset(self->notifications_, 0, sizeof(self->notifications_));
    self->bannerNotificationId_ = 0;
    if (self->notificationBannerTimer_) { lv_timer_delete(self->notificationBannerTimer_); self->notificationBannerTimer_ = nullptr; }
    if (self->notificationBanner_) lv_obj_set_y(self->notificationBanner_, -55);
    self->refreshNotifications();
    self->showNotifications(false);
}
void AppUI::notificationClicked(lv_event_t *event) {
    auto *self = static_cast<AppUI *>(lv_event_get_user_data(event));
    lv_obj_t *target = lv_event_get_current_target_obj(event);
    const uint32_t id = target == self->notificationBanner_ ? self->bannerNotificationId_ :
        uint32_t(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(target)));
    if (id) self->dismissNotification(id, true);
}
void AppUI::notificationBannerTimeout(lv_timer_t *timer) {
    auto *self = static_cast<AppUI *>(lv_timer_get_user_data(timer));
    self->notificationBannerTimer_ = nullptr;
    if (!self->notificationBanner_) return;
    lv_anim_t animation; lv_anim_init(&animation); lv_anim_set_var(&animation, self->notificationBanner_);
    lv_anim_set_values(&animation, lv_obj_get_y(self->notificationBanner_), -55); lv_anim_set_duration(&animation, 220);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&animation, [](void *object, int32_t value) { lv_obj_set_y(static_cast<lv_obj_t *>(object), value); });
    lv_anim_start(&animation);
}

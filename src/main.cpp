#include <Arduino.h>
#include <lvgl.h>

#include "AppUI.h"
#include "ConnectivityService.h"
#include "Hardware.h"
#include "MeshService.h"
#include "CommunicationService.h"
#include "MeshtasticService.h"
#include "SystemClock.h"
#include "ChatStore.h"

namespace {
constexpr uint16_t SCREEN_WIDTH = 320;
constexpr uint16_t SCREEN_HEIGHT = 240;
constexpr uint16_t DRAW_LINES = 48;
constexpr uint16_t FALLBACK_DRAW_LINES = 24;
static lv_color_t *drawBuffer;
static uint16_t drawLines = DRAW_LINES;
static uint32_t lastLvTick;
static uint16_t webTapX;
static uint16_t webTapY;
static uint8_t webTapPhase;
static bool webSwipe;
static uint16_t webSwipeEndX, webSwipeEndY;
static uint32_t webSwipeStarted;

void flushDisplay(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const uint32_t width = area->x2 - area->x1 + 1;
    const uint32_t height = area->y2 - area->y1 + 1;
    hardware.display.startWrite();
    hardware.display.setAddrWindow(area->x1, area->y1, width, height);
    hardware.display.pushPixels(reinterpret_cast<uint16_t *>(pixels), width * height, true);
    hardware.display.endWrite();
    lv_display_flush_ready(display);
}

void readTouch(lv_indev_t *, lv_indev_data_t *data) {
    if (!hardware.displayAwake()) {
        data->state = LV_INDEV_STATE_RELEASED;
        appUI.handleTouch(0, 0, false);
        return;
    }
    if (webTapPhase) {
        if (webSwipe) {
            const uint32_t elapsed = millis() - webSwipeStarted;
            if (elapsed <= 350) {
                data->state = LV_INDEV_STATE_PRESSED;
                data->point.x = webTapX + (int(webSwipeEndX) - webTapX) * int(elapsed) / 350;
                data->point.y = webTapY + (int(webSwipeEndY) - webTapY) * int(elapsed) / 350;
                appUI.handleTouch(data->point.x, data->point.y, true);
            } else {
                data->state = LV_INDEV_STATE_RELEASED;
                appUI.handleTouch(0, 0, false);
                webTapPhase = 0; webSwipe = false;
            }
            return;
        }
        if (webTapPhase <= 2) {
            data->state = LV_INDEV_STATE_PRESSED;
            data->point.x = webTapX; data->point.y = webTapY;
            appUI.handleTouch(webTapX, webTapY, true);
            ++webTapPhase;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
            appUI.handleTouch(0, 0, false);
            webTapPhase = 0;
        }
        return;
    }
    uint16_t x, y;
    if (hardware.display.getTouch(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        appUI.handleTouch(x, y, true);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        appUI.handleTouch(0, 0, false);
    }
}
}

bool queueWebTap(uint16_t x, uint16_t y) {
    if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT || webTapPhase) return false;
    webTapX = x; webTapY = y; webTapPhase = 1;
    return true;
}

bool queueWebSwipe(uint16_t x, uint16_t y, uint16_t endX, uint16_t endY) {
    if (!queueWebTap(x, y)) return false;
    if (endX >= SCREEN_WIDTH || endY >= SCREEN_HEIGHT) { webTapPhase = 0; return false; }
    webSwipe = true; webSwipeEndX = endX; webSwipeEndY = endY; webSwipeStarted = millis();
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(100);
    if (!hardware.begin()) {
        Serial.println("Hardware initialization failed");
        return;
    }
    connectivity.begin();

    lv_init();
    drawBuffer = static_cast<lv_color_t *>(heap_caps_malloc(
        SCREEN_WIDTH * DRAW_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!drawBuffer) {
        drawLines = FALLBACK_DRAW_LINES;
        drawBuffer = static_cast<lv_color_t *>(heap_caps_malloc(
            SCREEN_WIDTH * drawLines * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    if (!drawBuffer) {
        Serial.println("Display DMA buffer allocation failed");
        return;
    }
    lv_display_t *display = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_flush_cb(display, flushDisplay);
    lv_display_set_buffers(display, drawBuffer, nullptr,
                           SCREEN_WIDTH * drawLines * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_timer_set_period(lv_display_get_refr_timer(display), 16);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, readTouch);
    lv_timer_set_period(lv_indev_get_read_timer(touch), 16);
    lv_indev_add_event_cb(touch, [](lv_event_t *) { appUI.handleSystemClick(); }, LV_EVENT_RELEASED, nullptr);
    if (connectivity.sdMounted()) chatStore.begin();
    meshService.loadConfiguration();
    meshtasticService.loadConfiguration();
    communication.begin();
    appUI.begin();
    appUI.onNodeChanged();
    chatStore.importLegacy("/sdcard/apps/chats/legacy-messages.json");
    chatStore.importLegacy("/sdcard/system/legacy-messages.json");
    appUI.restoreChatMessages();
    lv_timer_handler();
    lastLvTick = millis();
}

void loop() {
    const uint32_t now = millis();
    lv_tick_inc(now - lastLvTick);
    lastLvTick = now;
    lv_timer_handler();
    hardware.poll();
    connectivity.loop();
    if (hardware.consumeScreenshotPress()) {
        if (hardware.displayAwake()) appUI.takeScreenshot();
    } else if (hardware.consumeVolumePress()) {
        appUI.handleVolumeButton();
    } else if (hardware.consumeButtonPress()) {
        const bool waking = !hardware.displayAwake();
        appUI.toggleDisplayWithEffect();
        if (waking) {
            appUI.noteUserActivity();
            lv_display_trigger_activity(nullptr);
        }
    }
    systemClock.now();
    communication.poll();
    chatStore.poll(connectivity.sdMounted(), now);
    appUI.loop();
    static uint32_t nextDiagnostic = 0;
    if (int32_t(now - nextDiagnostic) >= 0) {
        nextDiagnostic = now + 10000;
        const RadioPreset &preset = MeshService::radioPreset(meshService.radioPresetIndex());
        Serial.printf("Status: mesh=%s preset=%s nodes=%u battery=%u mV tx=%lu/%lu\n",
                      meshService.ready() ? "online" : "offline", preset.name,
                      unsigned(meshService.nodeCount()), hardware.batteryMillivolts(),
                      meshService.sentFloodCount(), meshService.sentDirectCount());
    }
    delay(4);
}

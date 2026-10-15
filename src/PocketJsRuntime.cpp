#include "PocketJsRuntime.h"
#include "KeyboardLayout.h"

#include <FS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "Hardware.h"
#include "MeshService.h"
#include "CommunicationService.h"
#include "MeshtasticService.h"
#include "ChatStore.h"
#include "AppUI.h"
#include "ConnectivityService.h"
#include "SdrService.h"

extern "C" {
#include "quickjs.h"
LV_FONT_DECLARE(lv_font_ui_14);
}

PocketJsRuntime pocketJsRuntime;

namespace {
constexpr size_t MAX_SOURCE_BYTES = 256 * 1024;
constexpr size_t JS_HEAP_LIMIT = 2 * 1024 * 1024;
constexpr size_t JS_STACK_LIMIT = 64 * 1024;
constexpr uint32_t EVAL_BUDGET_MS = 900;
constexpr uint32_t CALLBACK_BUDGET_MS = 200;
constexpr uint16_t MAX_WIDGETS = 256;
constexpr uint8_t MAX_BUTTONS = 64;
constexpr uint8_t MAX_GRIDS = 4;

uint32_t colorValue(JSContext *ctx, JSValueConst value, uint32_t fallback) {
    if (JS_IsNumber(value)) {
        uint32_t result = fallback;
        if (JS_ToUint32(ctx, &result, value) == 0) return result & 0xFFFFFF;
    }
    const char *text = JS_ToCString(ctx, value);
    if (!text) return fallback;
    const char *hex = text[0] == '#' ? text + 1 : text;
    char *end = nullptr;
    const unsigned long parsed = strtoul(hex, &end, 16);
    const bool valid = end && !*end && strlen(hex) == 6;
    JS_FreeCString(ctx, text);
    return valid ? uint32_t(parsed) : fallback;
}

int32_t numberArg(JSContext *ctx, int argc, JSValueConst *argv, int index, int32_t fallback) {
    int32_t result = fallback;
    return index < argc && JS_ToInt32(ctx, &result, argv[index]) == 0 ? result : fallback;
}

void *qjsMalloc(JSMallocState *, size_t size) {
    if (!size) return nullptr;
    void *result = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return result ? result : heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void qjsFree(JSMallocState *, void *pointer) { heap_caps_free(pointer); }

void *qjsRealloc(JSMallocState *, void *pointer, size_t size) {
    if (!pointer) return qjsMalloc(nullptr, size);
    if (!size) { heap_caps_free(pointer); return nullptr; }
    void *result = heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return result ? result : heap_caps_realloc(pointer, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t qjsUsableSize(const void *pointer) {
    return pointer ? heap_caps_get_allocated_size(const_cast<void *>(pointer)) : 0;
}

const JSMallocFunctions QJS_ALLOCATOR = {qjsMalloc, qjsFree, qjsRealloc, qjsUsableSize};
}

struct PocketJsRuntime::State {
    struct ButtonBinding {
        State *state = nullptr;
        lv_obj_t *object = nullptr;
        JSValue callback = JS_UNDEFINED;
        uint32_t pressedAt = 0;
        uint32_t lastRepeat = 0;
        uint16_t repeatDelay = 0;
        uint16_t repeatInterval = 0;
        int16_t startX = 0;
        int16_t startY = 0;
    };
    struct GridBinding {
        lv_obj_t *object = nullptr;
        lv_draw_buf_t *buffer = nullptr;
        uint8_t columns = 0;
        uint8_t rows = 0;
        uint8_t cellSize = 0;
        uint8_t gap = 0;
    };

    JSRuntime *runtime = nullptr;
    JSContext *context = nullptr;
    lv_obj_t *root = nullptr;
    lv_obj_t *keyboard = nullptr;
    lv_obj_t *widgets[MAX_WIDGETS]{};
    ButtonBinding buttons[MAX_BUTTONS]{};
    GridBinding grids[MAX_GRIDS]{};
    uint16_t widgetCount = 0;
    uint8_t buttonCount = 0;
    uint8_t gridCount = 0;
    char directory[96]{}, dataDirectory[96]{};
    uint32_t deadline = 0;
    uint32_t nextFrame = 0;
    bool runningCallback = false;
    bool fullscreen = false;
    int32_t keyboardRootY = 20;

    static State *from(JSContext *context) {
        return static_cast<State *>(JS_GetContextOpaque(context));
    }

    static int interrupt(JSRuntime *, void *opaque) {
        auto *self = static_cast<State *>(opaque);
        return self && self->deadline && int32_t(millis() - self->deadline) >= 0;
    }

    void printException(const char *stage) {
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        Serial.printf("PocketJS: %s: %s\n", stage, message ? message : "JavaScript exception");
        if (message) JS_FreeCString(context, message);
        JSValue stack = JS_GetPropertyStr(context, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *trace = JS_ToCString(context, stack);
            if (trace) { Serial.printf("%s\n", trace); JS_FreeCString(context, trace); }
        }
        JS_FreeValue(context, stack);
        JS_FreeValue(context, exception);
    }

    int addWidget(lv_obj_t *object) {
        if (!object || widgetCount >= MAX_WIDGETS) return -1;
        widgets[widgetCount] = object;
        return widgetCount++;
    }

    static JSValue jsLog(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        Serial.print("PocketJS app:");
        for (int i = 0; i < argc; ++i) {
            const char *value = JS_ToCString(ctx, argv[i]);
            Serial.printf(" %s", value ? value : "<value>");
            if (value) JS_FreeCString(ctx, value);
        }
        Serial.println();
        return JS_UNDEFINED;
    }

    static JSValue jsSetBackground(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 1) return JS_UNDEFINED;
        lv_obj_set_style_bg_color(self->root, lv_color_hex(colorValue(ctx, argv[0], 0xF2F2F7)), 0);
        return JS_UNDEFINED;
    }

    static JSValue jsSetScrollable(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root) return JS_FALSE;
        if (argc > 0 && JS_ToBool(ctx, argv[0])) {
            lv_obj_add_flag(self->root, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(self->root, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(self->root, LV_SCROLLBAR_MODE_OFF);
        } else {
            lv_obj_clear_flag(self->root, LV_OBJ_FLAG_SCROLLABLE);
        }
        return JS_TRUE;
    }

    static JSValue jsSetFullscreen(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root) return JS_FALSE;
        self->fullscreen = argc > 0 && JS_ToBool(ctx, argv[0]);
        lv_obj_set_pos(self->root, 0, self->fullscreen ? 0 : 20);
        // The home indicator floats transparently above app content; it must
        // not reserve layout space or leave an artificial strip at the bottom.
        lv_obj_set_size(self->root, 320, self->fullscreen ? 240 : 220);
        return JS_NewBool(ctx, self->fullscreen);
    }

    static JSValue jsText(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 1 || self->widgetCount >= MAX_WIDGETS) return JS_NewInt32(ctx, -1);
        const char *value = JS_ToCString(ctx, argv[0]);
        if (!value) return JS_EXCEPTION;
        lv_obj_t *label = lv_label_create(self->root);
        lv_label_set_text(label, value); JS_FreeCString(ctx, value);
        lv_obj_set_pos(label, numberArg(ctx, argc, argv, 1, 12), numberArg(ctx, argc, argv, 2, 12));
        lv_obj_set_width(label, max<int32_t>(20, numberArg(ctx, argc, argv, 3, 296)));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(label, &lv_font_ui_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(argc > 4 ? colorValue(ctx, argv[4], 0x111111) : 0x111111), 0);
        return JS_NewInt32(ctx, self->addWidget(label));
    }

    static JSValue jsNumber(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 4 || self->widgetCount >= MAX_WIDGETS) return JS_NewInt32(ctx, -1);
        lv_obj_t *label = lv_label_create(self->root);
        lv_label_set_text(label, "--");
        lv_obj_set_pos(label, numberArg(ctx, argc, argv, 0, 0), numberArg(ctx, argc, argv, 1, 0));
        lv_obj_set_size(label, max<int32_t>(20, numberArg(ctx, argc, argv, 2, 160)),
                        max<int32_t>(24, numberArg(ctx, argc, argv, 3, 48)));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(colorValue(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, 0xFFFFFF)), 0);
        return JS_NewInt32(ctx, self->addWidget(label));
    }

    static JSValue jsQrCode(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 4 || self->widgetCount >= MAX_WIDGETS) return JS_NewInt32(ctx, -1);
        const char *text = JS_ToCString(ctx, argv[0]);
        if (!text) return JS_EXCEPTION;
        if (strlen(text) > 256) { JS_FreeCString(ctx, text); return JS_NewInt32(ctx, -1); }
        lv_obj_t *qr = lv_qrcode_create(self->root);
        lv_qrcode_set_size(qr, constrain(numberArg(ctx, argc, argv, 3, 96), 64, 192));
        lv_qrcode_set_quiet_zone(qr, true);
        lv_qrcode_set_dark_color(qr, lv_color_black()); lv_qrcode_set_light_color(qr, lv_color_white());
        lv_obj_set_pos(qr, numberArg(ctx, argc, argv, 1, 0), numberArg(ctx, argc, argv, 2, 0));
        const bool ok = lv_qrcode_update(qr, text, strlen(text)) == LV_RESULT_OK;
        JS_FreeCString(ctx, text);
        if (!ok) { lv_obj_delete(qr); return JS_NewInt32(ctx, -1); }
        return JS_NewInt32(ctx, self->addWidget(qr));
    }
    static JSValue jsSetQrCode(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 2 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount ||
            !lv_obj_check_type(self->widgets[handle], &lv_qrcode_class)) return JS_FALSE;
        const char *text = JS_ToCString(ctx, argv[1]); if (!text) return JS_EXCEPTION;
        const bool ok = strlen(text) <= 256 && lv_qrcode_update(self->widgets[handle], text, strlen(text)) == LV_RESULT_OK;
        JS_FreeCString(ctx, text); return JS_NewBool(ctx, ok);
    }

    static JSValue jsRect(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || self->widgetCount >= MAX_WIDGETS) return JS_NewInt32(ctx, -1);
        lv_obj_t *object = lv_obj_create(self->root);
        lv_obj_set_pos(object, numberArg(ctx, argc, argv, 0, 0), numberArg(ctx, argc, argv, 1, 0));
        lv_obj_set_size(object, max<int32_t>(1, numberArg(ctx, argc, argv, 2, 20)),
                        max<int32_t>(1, numberArg(ctx, argc, argv, 3, 20)));
        lv_obj_set_style_bg_color(object, lv_color_hex(argc > 4 ? colorValue(ctx, argv[4], 0x007AFF) : 0x007AFF), 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0); lv_obj_set_style_border_width(object, 0, 0);
        lv_obj_set_style_radius(object, max<int32_t>(0, numberArg(ctx, argc, argv, 5, 8)), 0);
        lv_obj_set_style_pad_all(object, 0, 0); lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
        return JS_NewInt32(ctx, self->addWidget(object));
    }

    static JSValue jsScrollArea(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || self->widgetCount >= MAX_WIDGETS) return JS_NewInt32(ctx, -1);
        lv_obj_t *area = lv_obj_create(self->root);
        lv_obj_set_pos(area, numberArg(ctx, argc, argv, 0, 0), numberArg(ctx, argc, argv, 1, 0));
        lv_obj_set_size(area, max<int32_t>(20, numberArg(ctx, argc, argv, 2, 100)),
                        max<int32_t>(20, numberArg(ctx, argc, argv, 3, 100)));
        lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(area, 0, 0);
        lv_obj_set_style_pad_all(area, 0, 0); lv_obj_set_scroll_dir(area, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_pad_right(area, 0, LV_PART_SCROLLBAR);
        lv_obj_set_style_width(area, 3, LV_PART_SCROLLBAR);
        return JS_NewInt32(ctx, self->addWidget(area));
    }

    static JSValue jsSetParent(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t child = -1, parent = -1;
        if (!self || argc < 2 || JS_ToInt32(ctx, &child, argv[0]) || JS_ToInt32(ctx, &parent, argv[1]) ||
            child < 0 || parent < 0 || child >= self->widgetCount || parent >= self->widgetCount) return JS_FALSE;
        lv_obj_set_parent(self->widgets[child], self->widgets[parent]);
        return JS_TRUE;
    }

    static JSValue jsScrollTo(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1, y = 0;
        if (!self || argc < 2 || JS_ToInt32(ctx, &handle, argv[0]) || JS_ToInt32(ctx, &y, argv[1]) ||
            handle < 0 || handle >= self->widgetCount) return JS_FALSE;
        lv_obj_t *object = self->widgets[handle];
        lv_obj_update_layout(object);
        lv_obj_scroll_to_y(object, max<int32_t>(0, y), LV_ANIM_OFF);
        return JS_TRUE;
    }

    static JSValue jsGetScrollY(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 1 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_NewInt32(ctx, 0);
        return JS_NewInt32(ctx, lv_obj_get_scroll_y(self->widgets[handle]));
    }

    static void buttonPressed(lv_event_t *event) {
        auto *binding = static_cast<ButtonBinding *>(lv_event_get_user_data(event));
        if (!binding || !binding->state || !binding->state->context || JS_IsUndefined(binding->callback)) return;
        State *self = binding->state;
        self->deadline = millis() + CALLBACK_BUDGET_MS; self->runningCallback = true;
        JSValue result = JS_Call(self->context, binding->callback, JS_UNDEFINED, 0, nullptr);
        self->runningCallback = false; self->deadline = 0;
        if (JS_IsException(result)) self->printException("button callback");
        JS_FreeValue(self->context, result);
        self->drainJobs();
    }

    static void repeatButtonEvent(lv_event_t *event) {
        auto *binding = static_cast<ButtonBinding *>(lv_event_get_user_data(event));
        if (!binding) return;
        const uint32_t now = millis();
        const lv_event_code_t code = lv_event_get_code(event);
        if (code == LV_EVENT_PRESSED) {
            binding->pressedAt = binding->lastRepeat = now;
            buttonPressed(event);
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            binding->pressedAt = 0;
        }
    }

    static JSValue jsButton(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 6 || self->widgetCount >= MAX_WIDGETS || self->buttonCount >= MAX_BUTTONS)
            return JS_NewInt32(ctx, -1);
        const char *labelText = JS_ToCString(ctx, argv[0]);
        if (!labelText) return JS_EXCEPTION;
        lv_obj_t *button = lv_button_create(self->root);
        lv_obj_set_pos(button, numberArg(ctx, argc, argv, 1, 12), numberArg(ctx, argc, argv, 2, 12));
        lv_obj_set_size(button, max<int32_t>(28, numberArg(ctx, argc, argv, 3, 100)),
                        max<int32_t>(28, numberArg(ctx, argc, argv, 4, 42)));
        lv_obj_set_style_bg_color(button, lv_color_hex(argc > 6 ? colorValue(ctx, argv[6], 0x007AFF) : 0x007AFF), 0);
        lv_obj_set_style_radius(button, 12, 0); lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_t *label = lv_label_create(button); lv_label_set_text(label, labelText); JS_FreeCString(ctx, labelText);
        lv_obj_set_style_text_font(label, &lv_font_ui_14, 0); lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
        ButtonBinding &binding = self->buttons[self->buttonCount++];
        binding.state = self; binding.object = button; binding.callback = JS_DupValue(ctx, argv[5]);
        lv_obj_add_event_cb(button, buttonPressed, LV_EVENT_CLICKED, &binding);
        return JS_NewInt32(ctx, self->addWidget(button));
    }

    static JSValue jsRepeatButton(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 6 || self->widgetCount >= MAX_WIDGETS || self->buttonCount >= MAX_BUTTONS)
            return JS_NewInt32(ctx, -1);
        const char *labelText = JS_ToCString(ctx, argv[0]);
        if (!labelText) return JS_EXCEPTION;
        lv_obj_t *button = lv_button_create(self->root);
        lv_obj_set_pos(button, numberArg(ctx, argc, argv, 1, 12), numberArg(ctx, argc, argv, 2, 12));
        lv_obj_set_size(button, max<int32_t>(28, numberArg(ctx, argc, argv, 3, 100)),
                        max<int32_t>(28, numberArg(ctx, argc, argv, 4, 42)));
        lv_obj_set_style_bg_color(button, lv_color_hex(argc > 6 ? colorValue(ctx, argv[6], 0x007AFF) : 0x007AFF), 0);
        lv_obj_set_style_radius(button, 12, 0); lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_t *label = lv_label_create(button); lv_label_set_text(label, labelText); JS_FreeCString(ctx, labelText);
        lv_obj_set_style_text_font(label, &lv_font_ui_14, 0); lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
        ButtonBinding &binding = self->buttons[self->buttonCount++];
        binding.state = self; binding.object = button; binding.callback = JS_DupValue(ctx, argv[5]);
        binding.repeatDelay = constrain(numberArg(ctx, argc, argv, 7, 220), 80, 1000);
        binding.repeatInterval = constrain(numberArg(ctx, argc, argv, 8, 70), 25, 500);
        lv_obj_add_event_cb(button, repeatButtonEvent, LV_EVENT_ALL, &binding);
        return JS_NewInt32(ctx, self->addWidget(button));
    }

    static JSValue jsTouchArea(JSContext *ctx, int argc, JSValueConst *argv, lv_event_code_t trigger) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 5 || self->widgetCount >= MAX_WIDGETS || self->buttonCount >= MAX_BUTTONS)
            return JS_NewInt32(ctx, -1);
        lv_obj_t *area = lv_obj_create(self->root);
        lv_obj_set_pos(area, numberArg(ctx, argc, argv, 0, 0), numberArg(ctx, argc, argv, 1, 0));
        lv_obj_set_size(area, max<int32_t>(1, numberArg(ctx, argc, argv, 2, 20)),
                        max<int32_t>(1, numberArg(ctx, argc, argv, 3, 20)));
        lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(area, 0, 0);
        lv_obj_set_style_pad_all(area, 0, 0); lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);
        ButtonBinding &binding = self->buttons[self->buttonCount++];
        binding.state = self; binding.object = area; binding.callback = JS_DupValue(ctx, argv[4]);
        lv_obj_add_event_cb(area, buttonPressed, trigger, &binding);
        return JS_NewInt32(ctx, self->addWidget(area));
    }

    static JSValue jsHitArea(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        return jsTouchArea(ctx, argc, argv, LV_EVENT_CLICKED);
    }

    static JSValue jsPressArea(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        return jsTouchArea(ctx, argc, argv, LV_EVENT_PRESSED);
    }

    static JSValue jsLongPressArea(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 5 || self->widgetCount >= MAX_WIDGETS || self->buttonCount >= MAX_BUTTONS)
            return JS_NewInt32(ctx, -1);
        lv_obj_t *area = lv_obj_create(self->root);
        lv_obj_set_pos(area, numberArg(ctx, argc, argv, 0, 0), numberArg(ctx, argc, argv, 1, 0));
        lv_obj_set_size(area, max<int32_t>(1, numberArg(ctx, argc, argv, 2, 20)),
                        max<int32_t>(1, numberArg(ctx, argc, argv, 3, 20)));
        lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(area, 0, 0);
        lv_obj_set_style_pad_all(area, 0, 0); lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);
        // Touch-only message rows should scroll their parent without focus
        // outlines or scroll-to-focus jumps when the recycled row moves.
        lv_obj_clear_flag(area, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_style_outline_width(area, 0, LV_STATE_FOCUSED);
        ButtonBinding &binding = self->buttons[self->buttonCount++];
        binding.state = self; binding.object = area; binding.callback = JS_DupValue(ctx, argv[4]);
        lv_obj_add_event_cb(area, buttonPressed, LV_EVENT_LONG_PRESSED, &binding);
        return JS_NewInt32(ctx, self->addWidget(area));
    }

    static void swipeEvent(lv_event_t *event) {
        auto *binding = static_cast<ButtonBinding *>(lv_event_get_user_data(event));
        if (!binding || !binding->state || !binding->state->context || JS_IsUndefined(binding->callback)) return;
        lv_indev_t *indev = lv_indev_active(); lv_point_t point{};
        if (indev) lv_indev_get_point(indev, &point);
        const lv_event_code_t code = lv_event_get_code(event);
        if (code == LV_EVENT_PRESSED) { binding->startX = point.x; binding->startY = point.y; return; }
        if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
        const int dx = point.x - binding->startX, dy = point.y - binding->startY;
        if (max(abs(dx), abs(dy)) < 22) return;
        const char *direction = abs(dx) > abs(dy) ? (dx < 0 ? "left" : "right") : (dy < 0 ? "up" : "down");
        State *self = binding->state; JSValue argument = JS_NewString(self->context, direction);
        self->deadline = millis() + CALLBACK_BUDGET_MS; self->runningCallback = true;
        JSValue result = JS_Call(self->context, binding->callback, JS_UNDEFINED, 1, &argument);
        self->runningCallback = false; self->deadline = 0; JS_FreeValue(self->context, argument);
        if (JS_IsException(result)) self->printException("swipe callback");
        JS_FreeValue(self->context, result); self->drainJobs();
    }

    static JSValue jsSwipeArea(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 5 || self->widgetCount >= MAX_WIDGETS || self->buttonCount >= MAX_BUTTONS)
            return JS_NewInt32(ctx, -1);
        lv_obj_t *area = lv_obj_create(self->root);
        lv_obj_set_pos(area, numberArg(ctx, argc, argv, 0, 0), numberArg(ctx, argc, argv, 1, 0));
        lv_obj_set_size(area, max<int32_t>(1, numberArg(ctx, argc, argv, 2, 20)), max<int32_t>(1, numberArg(ctx, argc, argv, 3, 20)));
        lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(area, 0, 0);
        lv_obj_set_style_pad_all(area, 0, 0); lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE);
        ButtonBinding &binding = self->buttons[self->buttonCount++];
        binding.state = self; binding.object = area; binding.callback = JS_DupValue(ctx, argv[4]);
        lv_obj_add_event_cb(area, swipeEvent, LV_EVENT_ALL, &binding);
        return JS_NewInt32(ctx, self->addWidget(area));
    }

    static void textAreaFocused(lv_event_t *event) {
        auto *self = static_cast<State *>(lv_event_get_user_data(event));
        if (!self || !self->root) return;
        if (!self->keyboard) {
            self->keyboard = lv_keyboard_create(lv_layer_top()); configureKeyboardLayout(self->keyboard);
            lv_obj_set_size(self->keyboard, 320, 125);
            lv_obj_align(self->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
            Preferences preferences; preferences.begin("mesh-ui", true);
            const bool dark = preferences.getBool("dark", false); preferences.end();
            lv_obj_set_style_bg_color(self->keyboard, dark ? lv_color_hex(0x1C1C1E) : lv_color_hex(0xF2F2F7), LV_PART_MAIN);
            lv_obj_set_style_border_color(self->keyboard, dark ? lv_color_hex(0x48484A) : lv_color_hex(0xC7C7CC), LV_PART_MAIN);
            lv_obj_set_style_text_color(self->keyboard, dark ? lv_color_white() : lv_color_hex(0x111111), LV_PART_ITEMS);
            lv_obj_set_style_bg_color(self->keyboard, dark ? lv_color_hex(0x2C2C2E) : lv_color_white(), LV_PART_ITEMS);
            lv_obj_set_style_bg_color(self->keyboard, lv_color_hex(0x007AFF), LV_PART_ITEMS | LV_STATE_PRESSED);
            lv_obj_add_event_cb(self->keyboard, [](lv_event_t *keyboardEvent) {
                auto *state = static_cast<State *>(lv_event_get_user_data(keyboardEvent));
                if (state && state->keyboard) { lv_obj_add_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN); if (state->root) lv_obj_set_y(state->root, state->keyboardRootY); }
            }, LV_EVENT_READY, self);
            lv_obj_add_event_cb(self->keyboard, [](lv_event_t *keyboardEvent) {
                auto *state = static_cast<State *>(lv_event_get_user_data(keyboardEvent));
                if (state && state->keyboard) { lv_obj_add_flag(state->keyboard, LV_OBJ_FLAG_HIDDEN); if (state->root) lv_obj_set_y(state->root, state->keyboardRootY); }
            }, LV_EVENT_CANCEL, self);
        }
        self->keyboardRootY = self->fullscreen ? 0 : 20;
        lv_keyboard_set_textarea(self->keyboard, lv_event_get_target_obj(event));
        const int32_t bottom = lv_obj_get_y(lv_event_get_target_obj(event)) + lv_obj_get_height(lv_event_get_target_obj(event));
        // The keyboard begins at absolute screen y=115. Keep the focused field
        // nine pixels above it; root coordinates already include the 20px bar.
        if (!self->fullscreen && self->keyboardRootY + bottom > 106) lv_obj_set_y(self->root, 106 - bottom);
        lv_obj_remove_flag(self->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(self->keyboard);
    }

    static void textAreaDefocused(lv_event_t *event) {
        auto *self = static_cast<State *>(lv_event_get_user_data(event));
        if (!self || !self->keyboard || !self->root) return;
        if (lv_keyboard_get_textarea(self->keyboard) != lv_event_get_target_obj(event)) return;
        lv_obj_add_flag(self->keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(self->keyboard, nullptr);
        lv_obj_set_y(self->root, self->keyboardRootY);
    }

    static JSValue jsTextArea(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || self->widgetCount >= MAX_WIDGETS) return JS_NewInt32(ctx, -1);
        lv_obj_t *area = lv_textarea_create(self->root);
        lv_obj_set_pos(area, numberArg(ctx, argc, argv, 0, 12), numberArg(ctx, argc, argv, 1, 12));
        const int32_t width = max<int32_t>(40, numberArg(ctx, argc, argv, 2, 296));
        const int32_t height = max<int32_t>(24, numberArg(ctx, argc, argv, 3, 120));
        lv_textarea_set_one_line(area, argc > 5 && JS_ToBool(ctx, argv[5]));
        // one_line applies the theme's content height, so enforce the app's
        // requested dimensions afterwards.
        lv_obj_set_size(area, width, height);
        lv_textarea_set_max_length(area, constrain(numberArg(ctx, argc, argv, 4, 8192), 1, 65535));
        lv_obj_set_scrollbar_mode(area, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_text_font(area, &lv_font_ui_14, 0);
        lv_obj_set_style_pad_all(area, 0, 0);
        lv_obj_set_style_border_width(area, 0, 0);
        Preferences preferences; preferences.begin("mesh-ui", true);
        const bool dark = preferences.getBool("dark", false); preferences.end();
        lv_obj_set_style_bg_color(area, dark ? lv_color_hex(0x1C1C1E) : lv_color_white(), 0);
        lv_obj_set_style_text_color(area, dark ? lv_color_white() : lv_color_hex(0x111111), 0);
        lv_obj_set_style_border_color(area, dark ? lv_color_hex(0x48484A) : lv_color_hex(0xC7C7CC), 0);
        lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(area, lv_color_hex(0x007AFF), LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(area, 1, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_border_side(area, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_anim_duration(area, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_pad_top(area, -2, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_set_style_pad_bottom(area, -2, LV_PART_CURSOR | LV_STATE_FOCUSED);
        lv_obj_add_event_cb(area, textAreaFocused, LV_EVENT_FOCUSED, self);
        lv_obj_add_event_cb(area, textAreaDefocused, LV_EVENT_DEFOCUSED, self);
        return JS_NewInt32(ctx, self->addWidget(area));
    }

    static JSValue jsSetTextAreaOneLine(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 2 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_FALSE;
        lv_obj_t *object = self->widgets[handle];
        if (!object || !lv_obj_check_type(object, &lv_textarea_class)) return JS_FALSE;
        const int32_t width = lv_obj_get_width(object);
        const int32_t height = lv_obj_get_height(object);
        lv_textarea_set_one_line(object, JS_ToBool(ctx, argv[1]));
        lv_obj_set_size(object, width, height);
        return JS_TRUE;
    }

    static JSValue jsIcon(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 5 || self->widgetCount >= MAX_WIDGETS) return JS_NewInt32(ctx, -1);
        const char *name = JS_ToCString(ctx, argv[0]);
        if (!name) return JS_EXCEPTION;
        const int32_t width = max<int32_t>(8, numberArg(ctx, argc, argv, 3, 24));
        const int32_t height = max<int32_t>(8, numberArg(ctx, argc, argv, 4, 24));
        const lv_color_t color = lv_color_hex(colorValue(ctx, argc > 5 ? argv[5] : JS_UNDEFINED, 0x111111));
        lv_obj_t *icon = lv_obj_create(self->root);
        lv_obj_set_pos(icon, numberArg(ctx, argc, argv, 1, 0), numberArg(ctx, argc, argv, 2, 0));
        lv_obj_set_size(icon, width, height);
        lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        auto addLine = [icon, color](const lv_point_precise_t *points, uint32_t count) {
            lv_obj_t *line = lv_line_create(icon);
            lv_line_set_points(line, points, count);
            lv_obj_set_style_line_color(line, color, 0);
            lv_obj_set_style_line_width(line, 2, 0);
            lv_obj_set_style_line_rounded(line, true, 0);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        };

        if (!strcmp(name, "fullscreen") || !strcmp(name, "expand")) {
            static const lv_point_precise_t topLeft[] = {{4, 10}, {4, 4}, {10, 4}};
            static const lv_point_precise_t topRight[] = {{26, 10}, {26, 4}, {20, 4}};
            static const lv_point_precise_t bottomLeft[] = {{4, 18}, {4, 24}, {10, 24}};
            static const lv_point_precise_t bottomRight[] = {{26, 18}, {26, 24}, {20, 24}};
            addLine(topLeft, 3); addLine(topRight, 3); addLine(bottomLeft, 3); addLine(bottomRight, 3);
        } else if (!strcmp(name, "collapse")) {
            static const lv_point_precise_t topLeft[] = {{10, 4}, {10, 10}, {4, 10}};
            static const lv_point_precise_t topRight[] = {{20, 4}, {20, 10}, {26, 10}};
            static const lv_point_precise_t bottomLeft[] = {{10, 24}, {10, 18}, {4, 18}};
            static const lv_point_precise_t bottomRight[] = {{20, 24}, {20, 18}, {26, 18}};
            addLine(topLeft, 3); addLine(topRight, 3); addLine(bottomLeft, 3); addLine(bottomRight, 3);
        } else {
            JS_FreeCString(ctx, name); lv_obj_delete(icon); return JS_NewInt32(ctx, -1);
        }
        JS_FreeCString(ctx, name);
        return JS_NewInt32(ctx, self->addWidget(icon));
    }

    static JSValue jsGetText(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 1 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_NULL;
        lv_obj_t *object = self->widgets[handle];
        if (!object) return JS_NULL;
        if (lv_obj_check_type(object, &lv_textarea_class)) return JS_NewString(ctx, lv_textarea_get_text(object));
        if (lv_obj_check_type(object, &lv_label_class)) return JS_NewString(ctx, lv_label_get_text(object));
        return JS_NULL;
    }

    bool storagePath(JSContext *ctx, JSValueConst value, char *path, size_t pathSize, bool appdata = false) {
        const char *name = JS_ToCString(ctx, value);
        if (!name) return false;
        bool valid = name[0] && strlen(name) <= 48 && strcmp(name, ".") && strcmp(name, "..");
        for (const char *cursor = name; valid && *cursor; ++cursor)
            valid = isalnum(static_cast<unsigned char>(*cursor)) || *cursor == '.' || *cursor == '-' || *cursor == '_';
        if (valid) snprintf(path, pathSize, "%s/%s", appdata ? dataDirectory : directory, name);
        JS_FreeCString(ctx, name);
        return valid;
    }

    static JSValue readStorageFile(JSContext *ctx, int argc, JSValueConst *argv, bool appdata) {
        State *self = from(ctx); char path[160];
        if (!self || argc < 1 || !self->storagePath(ctx, argv[0], path, sizeof(path), appdata)) return JS_NULL;
        File file = SD_MMC.open(path, FILE_READ);
        if (!file && appdata) { char backup[168]; snprintf(backup, sizeof(backup), "%s.bak", path); file = SD_MMC.open(backup, FILE_READ); }
        if (!file || file.size() > 64 * 1024) { if (file) file.close(); return JS_NULL; }
        const size_t size = file.size();
        char *data = static_cast<char *>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!data) { file.close(); return JS_NULL; }
        const size_t read = file.readBytes(data, size); file.close(); data[read] = 0;
        JSValue result = JS_NewStringLen(ctx, data, read); heap_caps_free(data); return result;
    }

    static JSValue writeStorageFile(JSContext *ctx, int argc, JSValueConst *argv, bool appdata) {
        State *self = from(ctx); char path[160];
        if (!self || argc < 2 || !self->storagePath(ctx, argv[0], path, sizeof(path), appdata)) return JS_FALSE;
        if (appdata && ((!SD_MMC.exists("/appdata") && !SD_MMC.mkdir("/appdata")) ||
            (!SD_MMC.exists(self->dataDirectory) && !SD_MMC.mkdir(self->dataDirectory)))) return JS_FALSE;
        const char *data = JS_ToCString(ctx, argv[1]);
        if (!data) return JS_EXCEPTION;
        const size_t size = strlen(data);
        if (size > 64 * 1024) { JS_FreeCString(ctx, data); return JS_FALSE; }
        char temporary[168]; snprintf(temporary, sizeof(temporary), "%s.tmp", path);
        SD_MMC.remove(temporary);
        File file = SD_MMC.open(temporary, FILE_WRITE);
        const bool written = file && file.write(reinterpret_cast<const uint8_t *>(data), size) == size;
        if (file) file.close(); JS_FreeCString(ctx, data);
        if (!written) { SD_MMC.remove(temporary); return JS_FALSE; }
        char backup[168]; snprintf(backup, sizeof(backup), "%s.bak", path);
        if (appdata && SD_MMC.exists(path)) {
            SD_MMC.remove(backup);
            if (!SD_MMC.rename(path, backup)) { SD_MMC.remove(temporary); return JS_FALSE; }
        } else if (!appdata) SD_MMC.remove(path);
        const bool saved = SD_MMC.rename(temporary, path);
        if (!saved) {
            if (appdata && SD_MMC.exists(backup)) SD_MMC.rename(backup, path);
            SD_MMC.remove(temporary);
        }
        return JS_NewBool(ctx, saved);
    }

    static JSValue jsReadFile(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) { return readStorageFile(ctx, argc, argv, false); }
    static JSValue jsWriteFile(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) { return writeStorageFile(ctx, argc, argv, false); }
    static JSValue jsReadDataFile(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) { return readStorageFile(ctx, argc, argv, true); }
    static JSValue jsWriteDataFile(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) { return writeStorageFile(ctx, argc, argv, true); }
    static JSValue jsListFiles(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        State *self = from(ctx); JSValue result = JS_NewArray(ctx);
        if (!self) return result;
        File directory = SD_MMC.open(self->directory);
        if (!directory || !directory.isDirectory()) { if (directory) directory.close(); return result; }
        uint32_t index = 0; File entry;
        while (index < 24 && (entry = directory.openNextFile())) {
            if (!entry.isDirectory()) {
                const char *raw = entry.name(); const char *base = strrchr(raw, '/'); base = base ? base + 1 : raw;
                const size_t length = strlen(base);
                if (length > 4 && !strcasecmp(base + length - 4, ".txt"))
                    JS_SetPropertyUint32(ctx, result, index++, JS_NewString(ctx, base));
            }
            entry.close();
        }
        directory.close(); return result;
    }

    static JSValue jsRenameFile(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); char oldPath[160], newPath[160];
        if (!self || argc < 2 || !self->storagePath(ctx, argv[0], oldPath, sizeof(oldPath)) ||
            !self->storagePath(ctx, argv[1], newPath, sizeof(newPath))) return JS_FALSE;
        if (!strcmp(oldPath, newPath)) return JS_TRUE;
        if (SD_MMC.exists(newPath)) return JS_FALSE;
        return JS_NewBool(ctx, SD_MMC.rename(oldPath, newPath));
    }

    static JSValue jsSetText(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 2 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_FALSE;
        lv_obj_t *object = self->widgets[handle];
        if (!object || (!lv_obj_check_type(object, &lv_label_class) && !lv_obj_check_type(object, &lv_textarea_class))) return JS_FALSE;
        const char *value = JS_ToCString(ctx, argv[1]);
        if (!value) return JS_EXCEPTION;
        if (lv_obj_check_type(object, &lv_textarea_class)) lv_textarea_set_text(object, value);
        else lv_label_set_text(object, value);
        JS_FreeCString(ctx, value);
        return JS_TRUE;
    }

    static JSValue jsSetTextAlign(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 2 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount) return JS_FALSE;
        const char *value = JS_ToCString(ctx, argv[1]); if (!value) return JS_EXCEPTION;
        const lv_text_align_t align = !strcmp(value, "center") ? LV_TEXT_ALIGN_CENTER : (!strcmp(value, "right") ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT);
        JS_FreeCString(ctx, value); lv_obj_set_style_text_align(self->widgets[handle], align, 0); return JS_TRUE;
    }

    // Single-line label fitted to a sibling rectangle; use the font's actual
    // line height so both Latin and CJK text share the same button alignment.
    static JSValue jsFitText(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t label = -1, box = -1;
        if (!self || argc < 2 || JS_ToInt32(ctx, &label, argv[0]) || JS_ToInt32(ctx, &box, argv[1]) ||
            label < 0 || box < 0 || label >= self->widgetCount || box >= self->widgetCount) return JS_FALSE;
        lv_obj_t *text = self->widgets[label], *container = self->widgets[box];
        if (!text || !container || !lv_obj_check_type(text, &lv_label_class)) return JS_FALSE;
        lv_obj_update_layout(container);
        const lv_font_t *font = lv_obj_get_style_text_font(text, LV_PART_MAIN);
        lv_obj_set_size(text, max<int32_t>(1, lv_obj_get_width(container) - 8), font->line_height);
        lv_label_set_long_mode(text, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_update_layout(text);
        lv_obj_align_to(text, container, LV_ALIGN_CENTER, 0, 0);
        return JS_TRUE;
    }

    // Size a wrapping label with the same font metrics used for rendering.
    static JSValue jsWrapText(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 3 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_NewInt32(ctx, -1);
        lv_obj_t *label = self->widgets[handle];
        if (!lv_obj_check_type(label, &lv_label_class)) return JS_NewInt32(ctx, -1);
        const char *value = JS_ToCString(ctx, argv[1]);
        if (!value) return JS_EXCEPTION;
        const int32_t width = constrain(numberArg(ctx, argc, argv, 2, 296), 20, 320);
        const int32_t spacing = constrain(numberArg(ctx, argc, argv, 3, 0), -10, 20);
        const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        lv_point_t size{};
        lv_text_get_size(&size, value, font, lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), spacing, width, LV_TEXT_FLAG_NONE);
        const int32_t height = max<int32_t>(font->line_height, size.y);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(label, spacing, 0);
        lv_obj_set_size(label, width, height);
        lv_label_set_text(label, value);
        JS_FreeCString(ctx, value);
        return JS_NewInt32(ctx, height);
    }

    static JSValue jsSetColor(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 2 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_FALSE;
        lv_obj_t *object = self->widgets[handle];
        if (!object) return JS_FALSE;
        const lv_color_t color = lv_color_hex(colorValue(ctx, argv[1], 0x000000));
        if (lv_obj_check_type(object, &lv_label_class) || lv_obj_check_type(object, &lv_textarea_class))
            lv_obj_set_style_text_color(object, color, 0);
        else
            lv_obj_set_style_bg_color(object, color, 0);
        return JS_TRUE;
    }

    static JSValue jsSetPosition(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 3 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_FALSE;
        lv_obj_t *object = self->widgets[handle];
        if (!object) return JS_FALSE;
        lv_obj_set_pos(object, numberArg(ctx, argc, argv, 1, 0), numberArg(ctx, argc, argv, 2, 0));
        return JS_TRUE;
    }

    static JSValue jsSetSize(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 3 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_FALSE;
        lv_obj_t *object = self->widgets[handle];
        if (!object) return JS_FALSE;
        lv_obj_set_size(object, max<int32_t>(1, numberArg(ctx, argc, argv, 1, 1)),
                        max<int32_t>(1, numberArg(ctx, argc, argv, 2, 1)));
        return JS_TRUE;
    }

    static JSValue jsFocus(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 1 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_FALSE;
        lv_obj_t *object = self->widgets[handle];
        if (!object || !lv_obj_check_type(object, &lv_textarea_class)) return JS_FALSE;
        lv_obj_add_state(object, LV_STATE_FOCUSED);
        lv_obj_send_event(object, LV_EVENT_FOCUSED, nullptr);
        return JS_TRUE;
    }

    static JSValue jsSetVisible(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 2 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_FALSE;
        if (JS_ToBool(ctx, argv[1])) lv_obj_remove_flag(self->widgets[handle], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(self->widgets[handle], LV_OBJ_FLAG_HIDDEN);
        return JS_TRUE;
    }

    static JSValue jsGrid(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 7 || self->widgetCount >= MAX_WIDGETS || self->gridCount >= MAX_GRIDS)
            return JS_NewInt32(ctx, -1);
        const int32_t columns = constrain(numberArg(ctx, argc, argv, 2, 10), 1, 32);
        const int32_t rows = constrain(numberArg(ctx, argc, argv, 3, 20), 1, 32);
        // Game boards on the 320 x 240 display legitimately need cells much
        // larger than the small diagnostic grids this API started with.
        const int32_t cellSize = constrain(numberArg(ctx, argc, argv, 4, 10), 2, 64);
        const int32_t gap = constrain(numberArg(ctx, argc, argv, 5, 1), 0, cellSize - 1);
        const int32_t width = columns * cellSize, height = rows * cellSize;
        lv_draw_buf_t *buffer = lv_draw_buf_create(width, height, LV_COLOR_FORMAT_RGB565, 0);
        if (!buffer) return JS_NewInt32(ctx, -1);
        lv_obj_t *canvas = lv_canvas_create(self->root);
        lv_canvas_set_draw_buf(canvas, buffer);
        lv_obj_set_pos(canvas, numberArg(ctx, argc, argv, 0, 0), numberArg(ctx, argc, argv, 1, 0));
        lv_canvas_fill_bg(canvas, lv_color_hex(colorValue(ctx, argv[6], 0x24262A)), LV_OPA_COVER);
        GridBinding &grid = self->grids[self->gridCount++];
        grid.object = canvas; grid.buffer = buffer; grid.columns = columns; grid.rows = rows;
        grid.cellSize = cellSize; grid.gap = gap;
        return JS_NewInt32(ctx, self->addWidget(canvas));
    }

    static JSValue jsSetGridCell(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx); int32_t handle = -1;
        if (!self || argc < 4 || JS_ToInt32(ctx, &handle, argv[0])) return JS_FALSE;
        GridBinding *grid = nullptr;
        for (uint8_t i = 0; i < self->gridCount; ++i)
            if (self->grids[i].object && handle < self->widgetCount && self->widgets[handle] == self->grids[i].object) grid = &self->grids[i];
        if (!grid) return JS_FALSE;
        const int32_t column = numberArg(ctx, argc, argv, 1, -1), row = numberArg(ctx, argc, argv, 2, -1);
        if (column < 0 || column >= grid->columns || row < 0 || row >= grid->rows) return JS_FALSE;
        const lv_color_t color = lv_color_hex(colorValue(ctx, argv[3], 0x24262A));
        const int32_t startX = column * grid->cellSize, startY = row * grid->cellSize;
        const int32_t paintedSize = grid->cellSize - grid->gap;
        lv_display_enable_invalidation(nullptr, false);
        for (int32_t y = 0; y < grid->cellSize; ++y) for (int32_t x = 0; x < grid->cellSize; ++x)
            lv_canvas_set_px(grid->object, startX + x, startY + y,
                             x < paintedSize && y < paintedSize ? color : lv_color_hex(0x111827), LV_OPA_COVER);
        lv_display_enable_invalidation(nullptr, true); lv_obj_invalidate(grid->object);
        return JS_TRUE;
    }

    static JSValue jsGauge(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || !self->root || argc < 4 || self->widgetCount >= MAX_WIDGETS) return JS_NewInt32(ctx, -1);
        const int32_t x = numberArg(ctx, argc, argv, 0, 0);
        const int32_t y = numberArg(ctx, argc, argv, 1, 0);
        const int32_t width = max<int32_t>(40, numberArg(ctx, argc, argv, 2, 160));
        const int32_t height = max<int32_t>(40, numberArg(ctx, argc, argv, 3, 160));
        const lv_color_t arcColor = lv_color_hex(colorValue(ctx, argc > 4 ? argv[4] : JS_UNDEFINED, 0x00D7E8));
        const lv_color_t tickColor = lv_color_hex(colorValue(ctx, argc > 5 ? argv[5] : JS_UNDEFINED, 0x38BDF8));
        const lv_color_t textColor = lv_color_hex(colorValue(ctx, argc > 6 ? argv[6] : JS_UNDEFINED, 0xFFFFFF));

        lv_obj_t *scale = lv_scale_create(self->root);
        lv_obj_set_pos(scale, x, y);
        lv_obj_set_size(scale, width, height);
        lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
        lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(scale, 0, 0);
        lv_scale_set_range(scale, 0, 200);
        lv_scale_set_total_tick_count(scale, 41);
        lv_scale_set_major_tick_every(scale, 5);
        lv_scale_set_angle_range(scale, 250);
        lv_scale_set_rotation(scale, 145);
        lv_scale_set_label_show(scale, true);

        lv_obj_set_style_text_color(scale, textColor, LV_PART_INDICATOR);
        lv_obj_set_style_text_font(scale, &lv_font_montserrat_12, LV_PART_INDICATOR);
        lv_obj_set_style_line_color(scale, arcColor, LV_PART_INDICATOR);
        lv_obj_set_style_line_width(scale, 3, LV_PART_INDICATOR);
        lv_obj_set_style_length(scale, 14, LV_PART_INDICATOR);
        lv_obj_set_style_line_color(scale, tickColor, LV_PART_ITEMS);
        lv_obj_set_style_line_width(scale, 2, LV_PART_ITEMS);
        lv_obj_set_style_length(scale, 7, LV_PART_ITEMS);

        lv_obj_t *needle = lv_line_create(scale);
        lv_obj_set_style_line_color(needle, lv_color_hex(0xFF375F), 0);
        lv_obj_set_style_line_width(needle, 5, 0);
        lv_obj_set_style_line_rounded(needle, true, 0);
        lv_obj_set_user_data(scale, needle);
        return JS_NewInt32(ctx, self->addWidget(scale));
    }

    static JSValue jsSetGauge(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        int32_t handle = -1;
        if (!self || argc < 2 || JS_ToInt32(ctx, &handle, argv[0]) || handle < 0 || handle >= self->widgetCount)
            return JS_FALSE;
        lv_obj_t *scale = self->widgets[handle];
        if (!scale) return JS_FALSE;
        lv_obj_t *needle = static_cast<lv_obj_t *>(lv_obj_get_user_data(scale));
        if (!needle) return JS_FALSE;
        const int32_t value = constrain(numberArg(ctx, argc, argv, 1, 0), 0, 200);
        lv_scale_set_line_needle_value(scale, needle, 70, value);
        return JS_TRUE;
    }

    static JSValue jsTone(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        const int32_t frequency = constrain(numberArg(ctx, argc, argv, 0, 660), 80, 4000);
        const int32_t duration = constrain(numberArg(ctx, argc, argv, 1, 40), 5, 500);
        hardware.playGameTone(uint16_t(frequency), uint16_t(duration));
        return JS_UNDEFINED;
    }

    static bool i2cAddress(JSContext *ctx, JSValueConst value, uint8_t &address) {
        int32_t parsed = -1;
        if (JS_ToInt32(ctx, &parsed, value) != 0 || parsed < 0x08 || parsed > 0x77) return false;
        address = uint8_t(parsed);
        return true;
    }

    static bool i2cBytes(JSContext *ctx, JSValueConst value, uint8_t *buffer, size_t capacity, size_t &length) {
        length = 0;
        if (!JS_IsArray(ctx, value)) return false;
        JSValue lengthValue = JS_GetPropertyStr(ctx, value, "length");
        uint32_t requested = 0;
        const bool validLength = !JS_IsException(lengthValue) && JS_ToUint32(ctx, &requested, lengthValue) == 0;
        JS_FreeValue(ctx, lengthValue);
        if (!validLength || requested > capacity) return false;
        for (uint32_t i = 0; i < requested; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx, value, i);
            int32_t byte = -1;
            const bool valid = !JS_IsException(item) && JS_ToInt32(ctx, &byte, item) == 0 && byte >= 0 && byte <= 255;
            JS_FreeValue(ctx, item);
            if (!valid) return false;
            buffer[length++] = uint8_t(byte);
        }
        return true;
    }

    static JSValue i2cResult(JSContext *ctx, const uint8_t *buffer, size_t length) {
        JSValue result = JS_NewArray(ctx);
        for (size_t i = 0; i < length; ++i) JS_SetPropertyUint32(ctx, result, uint32_t(i), JS_NewInt32(ctx, buffer[i]));
        return result;
    }

    static JSValue jsI2cOpen(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        hardware.setGroveEnabled(true);
        return JS_NewBool(ctx, hardware.groveEnabled());
    }

    static JSValue jsSdrStart(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        const int32_t channel = numberArg(ctx, argc, argv, 0, 1);
        return JS_NewBool(ctx, channel >= 1 && channel <= 11 && sdrService.start(uint8_t(channel)));
    }

    static JSValue jsSdrTune(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        const int32_t channel = numberArg(ctx, argc, argv, 0, 1);
        return JS_NewBool(ctx, channel >= 1 && channel <= 11 && sdrService.tune(uint8_t(channel)));
    }

    static JSValue jsSdrSpectrum(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        uint8_t bins[SdrService::BIN_COUNT];
        if (!sdrService.spectrum(bins)) return JS_NULL;
        JSValue result = JS_NewArray(ctx);
        for (uint8_t i = 0; i < SdrService::BIN_COUNT; ++i)
            JS_SetPropertyUint32(ctx, result, i, JS_NewInt32(ctx, bins[i]));
        return result;
    }

    static JSValue jsSdrError(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        return JS_NewString(ctx, sdrService.error());
    }

    static JSValue jsSdrStop(JSContext *, JSValueConst, int, JSValueConst *) {
        sdrService.stop();
        return JS_UNDEFINED;
    }

    static JSValue jsI2cClose(JSContext *, JSValueConst, int, JSValueConst *) {
        hardware.setGroveEnabled(false);
        return JS_UNDEFINED;
    }

    static JSValue jsI2cScan(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        hardware.setGroveEnabled(true);
        // A disconnected Grove cable can leave the bus without a responding
        // peripheral. Keep a full 7-bit scan bounded so an app cannot remain
        // stuck in its loading screen for the default 50 ms timeout per address.
        Wire.setTimeOut(5);
        JSValue result = JS_NewArray(ctx); uint32_t output = 0;
        for (uint8_t address = 0x08; address <= 0x77; ++address) {
            Wire.beginTransmission(address);
            if (Wire.endTransmission() == 0) JS_SetPropertyUint32(ctx, result, output++, JS_NewInt32(ctx, address));
        }
        Wire.setTimeOut(50);
        return result;
    }

    static JSValue jsI2cProbe(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        uint8_t address = 0;
        if (argc < 1 || !i2cAddress(ctx, argv[0], address)) return JS_FALSE;
        hardware.setGroveEnabled(true); hardware.recoverI2cBus();
        Wire.beginTransmission(address);
        const bool present = Wire.endTransmission() == 0;
        Wire.setTimeOut(50);
        return JS_NewBool(ctx, present);
    }

    static JSValue jsI2cWrite(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        uint8_t address = 0; uint8_t buffer[64]; size_t length = 0;
        if (argc < 2 || !i2cAddress(ctx, argv[0], address) || !i2cBytes(ctx, argv[1], buffer, sizeof(buffer), length)) return JS_NewInt32(ctx, -1);
        hardware.setGroveEnabled(true); Wire.beginTransmission(address); Wire.write(buffer, length);
        return JS_NewInt32(ctx, Wire.endTransmission());
    }

    static JSValue jsI2cRead(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        uint8_t address = 0; int32_t requested = 0;
        if (argc < 2 || !i2cAddress(ctx, argv[0], address) || JS_ToInt32(ctx, &requested, argv[1]) != 0 || requested < 0 || requested > 64) return JS_NULL;
        hardware.setGroveEnabled(true); const size_t received = Wire.requestFrom(address, uint8_t(requested));
        uint8_t buffer[64]; size_t length = 0; while (Wire.available() && length < sizeof(buffer)) buffer[length++] = uint8_t(Wire.read());
        return received == size_t(requested) ? i2cResult(ctx, buffer, length) : JS_NULL;
    }

    static JSValue jsI2cWriteRead(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        uint8_t address = 0; uint8_t buffer[64]; size_t length = 0; int32_t requested = 0;
        if (argc < 3 || !i2cAddress(ctx, argv[0], address) || !i2cBytes(ctx, argv[1], buffer, sizeof(buffer), length) ||
            JS_ToInt32(ctx, &requested, argv[2]) != 0 || requested < 0 || requested > 64) return JS_NULL;
        hardware.setGroveEnabled(true); Wire.beginTransmission(address); Wire.write(buffer, length);
        if (Wire.endTransmission(false) != 0) return JS_NULL;
        const size_t received = Wire.requestFrom(address, uint8_t(requested));
        uint8_t result[64]; size_t resultLength = 0; while (Wire.available() && resultLength < sizeof(result)) result[resultLength++] = uint8_t(Wire.read());
        return received == size_t(requested) ? i2cResult(ctx, result, resultLength) : JS_NULL;
    }

    static JSValue jsDevice(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "platform", JS_NewString(ctx, "wio-tracker-l2"));
        JS_SetPropertyStr(ctx, result, "ipAddress", JS_NewString(ctx, WiFi.localIP().toString().c_str()));
        JS_SetPropertyStr(ctx, result, "width", JS_NewInt32(ctx, 320));
        JS_SetPropertyStr(ctx, result, "height", JS_NewInt32(ctx, 240));
        Preferences uiPreferences; uiPreferences.begin("mesh-ui", true);
        JS_SetPropertyStr(ctx, result, "darkMode", JS_NewBool(ctx, uiPreferences.getBool("dark", false)));
        JS_SetPropertyStr(ctx, result, "language", JS_NewString(ctx, uiPreferences.getBool("zh", false) ? "zh" : "en"));
        uiPreferences.end();
        JS_SetPropertyStr(ctx, result, "batteryMillivolts", JS_NewInt32(ctx, hardware.batteryMillivolts()));
        const RadioPreset &preset = communication.radioPreset(communication.radioPresetIndex());
        JS_SetPropertyStr(ctx, result, "meshOnline", JS_NewBool(ctx, communication.ready()));
        JS_SetPropertyStr(ctx, result, "radioProfile", JS_NewString(ctx, preset.name));
        JS_SetPropertyStr(ctx, result, "frequencyKHz", JS_NewInt32(ctx, int32_t(preset.frequency * 1000.0f + 0.5f)));
        JS_SetPropertyStr(ctx, result, "nodeCount", JS_NewInt32(ctx, communication.peerCount()));
        JS_SetPropertyStr(ctx, result, "sentFlood", JS_NewInt64(ctx, meshService.sentFloodCount()));
        JS_SetPropertyStr(ctx, result, "sentDirect", JS_NewInt64(ctx, meshService.sentDirectCount()));
        JS_SetPropertyStr(ctx, result, "rssi", JS_NewInt32(ctx, communication.rssi()));
        JS_SetPropertyStr(ctx, result, "snr", JS_NewInt32(ctx, int(communication.snr())));
        JS_SetPropertyStr(ctx, result, "gpsEnabled", JS_NewBool(ctx, hardware.gpsEnabled()));
        JS_SetPropertyStr(ctx, result, "gpsBytes", JS_NewInt64(ctx, hardware.gpsBytesSeen()));
        const uint32_t totalMemory = ESP.getHeapSize() + ESP.getPsramSize();
        const uint32_t freeMemory = ESP.getFreeHeap() + ESP.getFreePsram();
        JS_SetPropertyStr(ctx, result, "memoryFree", JS_NewInt64(ctx, freeMemory));
        JS_SetPropertyStr(ctx, result, "memoryTotal", JS_NewInt64(ctx, totalMemory));
        JS_SetPropertyStr(ctx, result, "memoryPercent", JS_NewInt32(ctx, totalMemory ? freeMemory * 100U / totalMemory : 0));
        JS_SetPropertyStr(ctx, result, "gpsFix", JS_NewBool(ctx, hardware.gpsHasFix()));
        JS_SetPropertyStr(ctx, result, "gpsSatellites", JS_NewInt32(ctx, hardware.gpsSatellites()));
        JS_SetPropertyStr(ctx, result, "gpsSatellitesUsed", JS_NewInt32(ctx, hardware.gpsSatellitesUsed()));
        JS_SetPropertyStr(ctx, result, "gpsHdop", JS_NewFloat64(ctx, hardware.gpsHdop()));
        JS_SetPropertyStr(ctx, result, "gpsAltitudeMeters", JS_NewFloat64(ctx, hardware.gpsAltitudeMeters()));
        JS_SetPropertyStr(ctx, result, "gpsAltitudeValid", JS_NewBool(ctx, hardware.gpsAltitudeValid()));
        JS_SetPropertyStr(ctx, result, "gpsSpeedKph", JS_NewFloat64(ctx, hardware.gpsSpeedKph()));
        JS_SetPropertyStr(ctx, result, "gpsCourseDegrees", JS_NewFloat64(ctx, hardware.gpsCourseDegrees()));
        JS_SetPropertyStr(ctx, result, "gpsTimeValid", JS_NewBool(ctx, hardware.gpsTimeValid()));
        JS_SetPropertyStr(ctx, result, "gpsTimestamp", JS_NewInt64(ctx, hardware.gpsTimestamp()));
        if (hardware.gpsHasFix()) {
            JS_SetPropertyStr(ctx, result, "latitude", JS_NewFloat64(ctx, hardware.gpsLatitude()));
            JS_SetPropertyStr(ctx, result, "longitude", JS_NewFloat64(ctx, hardware.gpsLongitude()));
        }
        JSValue satellites = JS_NewArray(ctx);
        const uint8_t satelliteCount = hardware.gpsSatelliteInfoCount();
        for (uint8_t i = 0; i < satelliteCount; ++i) {
            const Hardware::GpsSatellite &sat = hardware.gpsSatelliteInfo(i);
            JSValue item = JS_NewObject(ctx);
            char system[2] = {sat.system, 0};
            JS_SetPropertyStr(ctx, item, "system", JS_NewString(ctx, system));
            JS_SetPropertyStr(ctx, item, "id", JS_NewInt32(ctx, sat.id));
            JS_SetPropertyStr(ctx, item, "azimuth", JS_NewInt32(ctx, sat.azimuth));
            JS_SetPropertyStr(ctx, item, "elevation", JS_NewInt32(ctx, sat.elevation));
            JS_SetPropertyStr(ctx, item, "snr", JS_NewInt32(ctx, sat.snr));
            JS_SetPropertyUint32(ctx, satellites, i, item);
        }
        JS_SetPropertyStr(ctx, result, "gpsSatelliteInfo", satellites);
        return result;
    }

    static JSValue jsNotificationSound(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        if (!argc) return JS_FALSE;
        const bool direct = JS_ToBool(ctx, argv[0]);
        if (argc > 1) {
            int32_t style;
            if (JS_ToInt32(ctx, &style, argv[1]) < 0) return JS_EXCEPTION;
            return JS_NewBool(ctx, style >= 0 && style <= 4 && appUI.setNotificationSound(direct, uint8_t(style)));
        }
        return JS_NewInt32(ctx, appUI.notificationSound(direct));
    }
    static JSValue jsPreviewNotificationSound(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        if (argc < 2) return JS_FALSE;
        int32_t style;
        if (JS_ToInt32(ctx, &style, argv[1]) < 0) return JS_EXCEPTION;
        if (style < 0 || style > 4) return JS_FALSE;
        hardware.playMessageTone(JS_ToBool(ctx, argv[0]), uint8_t(style)); return JS_TRUE;
    }
    static JSValue jsChatStorage(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "ready", JS_NewBool(ctx, chatStore.ready() && connectivity.sdMounted()));
        JS_SetPropertyStr(ctx, result, "pending", JS_NewUint32(ctx, chatStore.pending()));
        JS_SetPropertyStr(ctx, result, "messages", JS_NewUint32(ctx, chatStore.messageCount()));
        JS_SetPropertyStr(ctx, result, "error", JS_NewString(ctx, chatStore.error()));
        JS_SetPropertyStr(ctx, result, "path", JS_NewString(ctx, "/appdata/chats")); return result;
    }
    static JSValue jsTakeChatTarget(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        State *self = from(ctx); char peer[70]{};
        if (!self || strcmp(self->directory, "/apps/chats") || !appUI.takeChatTarget(peer, sizeof(peer))) return JS_NULL;
        return JS_NewString(ctx, peer);
    }
    static JSValue jsChatUnread(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "groups", JS_NewBool(ctx, appUI.chatUnread(false)));
        JS_SetPropertyStr(ctx, result, "direct", JS_NewBool(ctx, appUI.chatUnread(true)));
        return result;
    }
    static JSValue jsMarkChatRead(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        if (!argc) return JS_FALSE;
        appUI.markChatRead(JS_ToBool(ctx, argv[0])); return JS_TRUE;
    }
    static JSValue jsSetChatView(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        State *self = from(ctx);
        if (!self || strcmp(self->directory, "/apps/chats") || argc < 3) return JS_FALSE;
        const char *peer = JS_ToCString(ctx, argv[1]); if (!peer) return JS_EXCEPTION;
        appUI.setChatView(JS_ToBool(ctx, argv[0]), peer, JS_ToBool(ctx, argv[2]));
        JS_FreeCString(ctx, peer); return JS_TRUE;
    }
    static JSValue jsChatPeers(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue result = JS_NewArray(ctx); unsigned output = 0;
        const char *protocol = communication.configurationProtocol();
        for (size_t i = 0; i < chatStore.peerCount(); ++i) {
            const auto *saved = chatStore.peer(i); if (strcmp(saved->protocol, protocol)) continue;
            JSValue item = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, item, "id", JS_NewString(ctx, saved->peer.id));
            JS_SetPropertyStr(ctx, item, "name", JS_NewString(ctx, saved->peer.name));
            JS_SetPropertyStr(ctx, item, "contact", JS_NewBool(ctx, saved->peer.contact));
            JS_SetPropertyUint32(ctx, result, output++, item);
        }
        // New peers remain usable before their contact snapshot reaches SD.
        for (size_t i = 0; i < communication.peerCount(); ++i) {
            const auto peer = communication.peer(i); bool found = false;
            for (size_t j = 0; j < chatStore.peerCount(); ++j) {
                const auto *saved = chatStore.peer(j); if (!strcmp(saved->protocol, protocol) && !strcmp(saved->peer.id, peer.id)) { found = true; break; }
            }
            if (found) continue; JSValue item = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, item, "id", JS_NewString(ctx, peer.id)); JS_SetPropertyStr(ctx, item, "name", JS_NewString(ctx, peer.name));
            JS_SetPropertyStr(ctx, item, "contact", JS_NewBool(ctx, peer.contact)); JS_SetPropertyUint32(ctx, result, output++, item);
        }
        return result;
    }
    static JSValue jsChatHistory(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        int limit = 3, direct = -1; uint32_t before = 0; const char *peerId = nullptr; bool includeData = false;
        if (argc && JS_IsObject(argv[0])) {
            JSValue value = JS_GetPropertyStr(ctx, argv[0], "limit");
            if (!JS_IsUndefined(value) && JS_ToInt32(ctx, &limit, value)) { JS_FreeValue(ctx, value); return JS_EXCEPTION; } JS_FreeValue(ctx, value);
            value = JS_GetPropertyStr(ctx, argv[0], "before"); int64_t number = 0;
            if (!JS_IsUndefined(value) && (JS_ToInt64(ctx, &number, value) || number < 0 || number > UINT32_MAX)) { JS_FreeValue(ctx, value); return JS_ThrowRangeError(ctx, "Invalid history cursor"); }
            before = number; JS_FreeValue(ctx, value);
            value = JS_GetPropertyStr(ctx, argv[0], "direct"); if (!JS_IsUndefined(value)) direct = JS_ToBool(ctx, value); JS_FreeValue(ctx, value);
            value = JS_GetPropertyStr(ctx, argv[0], "includeData"); includeData = JS_ToBool(ctx, value); JS_FreeValue(ctx, value);
            value = JS_GetPropertyStr(ctx, argv[0], "peerId"); if (!JS_IsUndefined(value)) peerId = JS_ToCString(ctx, value); JS_FreeValue(ctx, value);
        }
        if (limit < 1 || limit > 50 || (peerId && strlen(peerId) >= 70)) { if (peerId) JS_FreeCString(ctx, peerId); return JS_ThrowRangeError(ctx, "History limit must be 1-50; invalid peer ID"); }
        auto *records = static_cast<ChatRecord *>(heap_caps_malloc(limit * sizeof(ChatRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!records) { if (peerId) JS_FreeCString(ctx, peerId); return JS_ThrowInternalError(ctx, "History allocation failed"); }
        const size_t count = chatStore.history(records, limit, communication.configurationProtocol(), peerId, direct, before, !includeData);
        if (peerId) JS_FreeCString(ctx, peerId);
        JSValue result = JS_NewArray(ctx);
        for (size_t i = 0; i < count; ++i) {
            const auto &record = records[count - i - 1]; JSValue item = JS_NewObject(ctx); const char *name = record.peer;
            for (size_t j = 0; j < chatStore.peerCount(); ++j) {
                const auto *saved = chatStore.peer(j);
                if (!strcmp(saved->protocol, record.protocol) && !strcmp(saved->peer.id, record.peerId)) { name = saved->peer.name; break; }
            }
            JS_SetPropertyStr(ctx, item, "id", JS_NewUint32(ctx, record.id));
            JS_SetPropertyStr(ctx, item, "peerId", JS_NewString(ctx, record.peerId)); JS_SetPropertyStr(ctx, item, "peer", JS_NewString(ctx, name));
            JS_SetPropertyStr(ctx, item, "protocol", JS_NewString(ctx, record.protocol)); JS_SetPropertyStr(ctx, item, "port", JS_NewUint32(ctx, record.port));
            JS_SetPropertyStr(ctx, item, "packetId", JS_NewUint32(ctx, record.packetId)); JS_SetPropertyStr(ctx, item, "timestamp", JS_NewUint32(ctx, record.timestamp));
            JS_SetPropertyStr(ctx, item, "direct", JS_NewBool(ctx, record.flags & 1)); JS_SetPropertyStr(ctx, item, "outgoing", JS_NewBool(ctx, record.flags & 2));
            JS_SetPropertyStr(ctx, item, "contact", JS_NewBool(ctx, record.flags & 4));
            JS_SetPropertyStr(ctx, item, "text", record.port == 1 ? JS_NewStringLen(ctx, reinterpret_cast<const char *>(record.payload), record.length) : JS_NewString(ctx, ""));
            char hex[481]{}; for (unsigned j = 0; j < record.length; ++j) snprintf(hex + j * 2, 3, "%02x", record.payload[j]);
            JS_SetPropertyStr(ctx, item, "dataHex", JS_NewString(ctx, hex)); JS_SetPropertyUint32(ctx, result, i, item);
        }
        heap_caps_free(records); return result;
    }
    static JSValue jsDeleteChatHistory(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        int64_t id = 0; if (!argc || JS_ToInt64(ctx, &id, argv[0]) || id < 1 || id > UINT32_MAX) return JS_ThrowRangeError(ctx, "Message ID required");
        return JS_NewBool(ctx, appUI.deleteHistoryMessage(uint32_t(id)));
    }
    static JSValue jsMeshMessages(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue result = JS_NewArray(ctx);
        const size_t count = appUI.chatMessageCount();
        for (size_t i = 0; i < count; ++i) {
            AppUI::ChatMessageView message;
            if (!appUI.chatMessageAt(i, message)) continue;
            if (message.protocol && strcmp(message.protocol, communication.protocol())) continue;
            JSValue item = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, item, "index", JS_NewInt32(ctx, int32_t(i)));
            JS_SetPropertyStr(ctx, item, "text", JS_NewString(ctx, message.text ? message.text : ""));
            JS_SetPropertyStr(ctx, item, "peer", JS_NewString(ctx, message.peer ? message.peer : ""));
            JS_SetPropertyStr(ctx, item, "timestamp", JS_NewInt64(ctx, message.timestamp));
            JS_SetPropertyStr(ctx, item, "direct", JS_NewBool(ctx, message.direct));
            JS_SetPropertyStr(ctx, item, "outgoing", JS_NewBool(ctx, message.outgoing));
            JS_SetPropertyStr(ctx, item, "contact", JS_NewBool(ctx, message.contact));
            JS_SetPropertyUint32(ctx, result, uint32_t(i), item);
        }
        return result;
    }

    static JSValue jsMeshMessageRevision(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        return JS_NewUint32(ctx, appUI.chatMessageRevision() + chatStore.revision());
    }

    static JSValue jsMeshContacts(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue result = JS_NewArray(ctx); uint32_t output = 0;
        for (size_t i = 0; i < communication.peerCount(); ++i) {
            const CommunicationPeer node = communication.peer(i);
            if (node.contact || !strcmp(communication.configurationProtocol(), "meshtastic")) JS_SetPropertyUint32(ctx, result, output++, JS_NewString(ctx, node.name));
        }
        return result;
    }

    static JSValue jsCommunication(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "protocol", JS_NewString(ctx, communication.protocol()));
        JS_SetPropertyStr(ctx, result, "configurationProtocol", JS_NewString(ctx, communication.configurationProtocol()));
        JS_SetPropertyStr(ctx, result, "ready", JS_NewBool(ctx, communication.ready()));
        JS_SetPropertyStr(ctx, result, "error", JS_NewString(ctx, communication.error()));
        JSValue available = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, available, 0, JS_NewString(ctx, "off"));
        JS_SetPropertyUint32(ctx, available, 1, JS_NewString(ctx, "meshcore"));
        JS_SetPropertyUint32(ctx, available, 2, JS_NewString(ctx, "meshtastic"));
        JS_SetPropertyStr(ctx, result, "available", available);
        return result;
    }
    static JSValue jsRadioPresets(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue result = JS_NewArray(ctx);
        for (size_t i = 0; i < communication.radioPresetCount(); ++i) {
            const auto &preset = communication.radioPreset(i); JSValue entry = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, entry, "index", JS_NewUint32(ctx, i));
            JS_SetPropertyStr(ctx, entry, "name", JS_NewString(ctx, preset.name));
            JS_SetPropertyStr(ctx, entry, "frequencyMHz", JS_NewFloat64(ctx, preset.frequency));
            JS_SetPropertyStr(ctx, entry, "bandwidthKHz", JS_NewFloat64(ctx, preset.bandwidth));
            JS_SetPropertyStr(ctx, entry, "spreadingFactor", JS_NewInt32(ctx, preset.spreadingFactor));
            JS_SetPropertyStr(ctx, entry, "codingRate", JS_NewInt32(ctx, preset.codingRate));
            JS_SetPropertyUint32(ctx, result, i, entry);
        }
        return result;
    }
    static JSValue jsSelectRadioPreset(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        int32_t index = -1;
        if (argc < 1 || JS_ToInt32(ctx, &index, argv[0]) != 0 || index < 0 || size_t(index) >= communication.radioPresetCount())
            return JS_ThrowRangeError(ctx, "Invalid radio preset index");
        if (!communication.selectRadioPreset(size_t(index))) return JS_ThrowInternalError(ctx, "Could not save radio preset");
        return JS_TRUE;
    }
    static JSValue jsCommunicationPeers(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue array = JS_NewArray(ctx);
        for (size_t i = 0; i < communication.peerCount(); ++i) {
            const auto peer = communication.peer(i); JSValue item = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, item, "index", JS_NewUint32(ctx, i)); JS_SetPropertyStr(ctx, item, "id", JS_NewString(ctx, peer.id));
            JS_SetPropertyStr(ctx, item, "name", JS_NewString(ctx, peer.name)); JS_SetPropertyStr(ctx, item, "contact", JS_NewBool(ctx, peer.contact));
            JS_SetPropertyStr(ctx, item, "hasPublicKey", JS_NewBool(ctx, peer.hasPublicKey)); JS_SetPropertyStr(ctx, item, "lastSeen", JS_NewUint32(ctx, peer.lastSeen));
            JS_SetPropertyUint32(ctx, array, i, item);
        }
        return array;
    }
    static JSValue jsAdvertise(JSContext *ctx, JSValueConst, int, JSValueConst *) { return JS_NewBool(ctx, communication.advertise()); }
    static JSValue jsMeshtasticConfig(JSContext *ctx, JSValueConst, int, JSValueConst *) {
        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "role", JS_NewString(ctx, meshtasticService.roleName(meshtasticService.roleIndex())));
        JS_SetPropertyStr(ctx, result, "signalKnown", JS_NewBool(ctx, communication.signalKnown()));
        JS_SetPropertyStr(ctx, result, "rssi", JS_NewInt32(ctx, communication.rssi()));
        JS_SetPropertyStr(ctx, result, "snr", JS_NewFloat64(ctx, communication.snr()));
        JS_SetPropertyStr(ctx, result, "region", JS_NewString(ctx, meshtasticService.regionName(meshtasticService.regionIndex())));
        JS_SetPropertyStr(ctx, result, "channel", JS_NewString(ctx, meshtasticService.channelName()));
        const auto preset = meshtasticService.preset(meshtasticService.presetIndex());
        JS_SetPropertyStr(ctx, result, "modem", JS_NewString(ctx, preset.name));
        JS_SetPropertyStr(ctx, result, "frequencyMHz", JS_NewFloat64(ctx, preset.frequency));
        JS_SetPropertyStr(ctx, result, "nodeNumber", JS_NewUint32(ctx, meshtasticService.nodeNumber()));
        JS_SetPropertyStr(ctx, result, "tx", JS_NewUint32(ctx, meshtasticService.txCount()));
        JS_SetPropertyStr(ctx, result, "rx", JS_NewUint32(ctx, meshtasticService.rxCount()));
        JS_SetPropertyStr(ctx, result, "rejected", JS_NewUint32(ctx, meshtasticService.rejectedCount()));
        JS_SetPropertyStr(ctx, result, "queued", JS_NewUint32(ctx, meshtasticService.queuedCount()));
        JS_SetPropertyStr(ctx, result, "lastTxId", JS_NewUint32(ctx, meshtasticService.lastTxId()));
        JS_SetPropertyStr(ctx, result, "lastAckId", JS_NewUint32(ctx, meshtasticService.lastAckId()));
        return result;
    }
    static JSValue jsMeshtasticRole(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        const char *name = argc ? JS_ToCString(ctx, argv[0]) : nullptr;
        if (!name) return JS_ThrowTypeError(ctx, "Role name required");
        const int index = !strcmp(name, "Client") || !strcmp(name, "CLIENT") ? 0 :
            !strcmp(name, "Client Mute") || !strcmp(name, "CLIENT_MUTE") ? 1 : -1;
        JS_FreeCString(ctx, name);
        if (index < 0) return JS_ThrowRangeError(ctx, "Supported roles: Client, Client Mute");
        if (!meshtasticService.selectRole(index)) return JS_ThrowInternalError(ctx, "%s", meshtasticService.error());
        return JS_TRUE;
    }
    static JSValue jsMeshtasticRegion(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        const char *name = argc ? JS_ToCString(ctx, argv[0]) : nullptr;
        if (!name) return JS_ThrowTypeError(ctx, "Region name required");
        bool found = false, ok = false;
        for (size_t i = 0; i < meshtasticService.regionCount(); ++i) if (!strcmp(name, meshtasticService.regionName(i))) {
            found = true; ok = meshtasticService.selectRegion(i); break;
        }
        JS_FreeCString(ctx, name);
        if (!found) return JS_ThrowRangeError(ctx, "Unknown Meshtastic region");
        if (!ok) return JS_ThrowInternalError(ctx, "%s", meshtasticService.error());
        return JS_TRUE;
    }
    static JSValue jsMeshtasticFrequency(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        double frequency = 0;
        if (!argc || JS_ToFloat64(ctx, &frequency, argv[0]) || !isfinite(frequency)) return JS_ThrowTypeError(ctx, "Frequency MHz required; 0 uses automatic frequency");
        if (!meshtasticService.setFrequency(frequency)) return JS_ThrowRangeError(ctx, "%s", meshtasticService.error());
        return JS_TRUE;
    }
    static JSValue jsMeshtasticChannel(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        if (argc < 2) return JS_ThrowTypeError(ctx, "Channel name and base64 PSK required");
        const char *name = JS_ToCString(ctx, argv[0]), *key = JS_ToCString(ctx, argv[1]);
        const bool ok = name && key && meshtasticService.setChannel(name, key);
        if (name) JS_FreeCString(ctx, name); if (key) JS_FreeCString(ctx, key);
        if (!ok) return JS_ThrowRangeError(ctx, "Invalid channel or configuration busy");
        return JS_TRUE;
    }
    static JSValue jsSelectProtocol(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        if (argc < 1) return JS_ThrowTypeError(ctx, "Protocol name required");
        const char *name = JS_ToCString(ctx, argv[0]);
        if (!name) return JS_EXCEPTION;
        const bool selected = communication.select(name); JS_FreeCString(ctx, name);
        if (!selected) return JS_ThrowInternalError(ctx, "%s", communication.error());
        return JS_TRUE;
    }

    static JSValue jsMeshSend(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        if (argc < 3) return JS_FALSE;
        const bool direct = JS_ToBool(ctx, argv[0]);
        const char *peer = JS_ToCString(ctx, argv[1]);
        const char *message = JS_ToCString(ctx, argv[2]);
        if (!peer || !message) {
            if (peer) JS_FreeCString(ctx, peer);
            if (message) JS_FreeCString(ctx, message);
            return JS_EXCEPTION;
        }
        bool sent = false;
        if (!direct) sent = communication.sendPublic(message);
        else for (size_t i = 0; i < communication.peerCount(); ++i) {
            if (!strcmp(communication.peer(i).name, peer) || !strcmp(communication.peer(i).id, peer)) { sent = communication.sendDirect(i, message); break; }
        }
        JS_FreeCString(ctx, peer); JS_FreeCString(ctx, message);
        return JS_NewBool(ctx, sent);
    }

    static JSValue jsMeshDeleteMessage(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        int32_t index = -1;
        if (argc < 1 || JS_ToInt32(ctx, &index, argv[0]) || index < 0) return JS_FALSE;
        return JS_NewBool(ctx, appUI.deleteChatMessage(size_t(index)));
    }

    static JSValue jsHttpGet(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
        if (argc < 1) return JS_NULL;
        const char *url = JS_ToCString(ctx, argv[0]);
        if (!url || !url[0]) {
            if (url) JS_FreeCString(ctx, url);
            return JS_NULL;
        }
        State *self = from(ctx);
        const uint32_t originalDeadline = self ? self->deadline : 0;
        if (self) self->deadline = 0;
        struct DeadlineRestore {
            State *state;
            uint32_t original;
            ~DeadlineRestore() {
                if (state) state->deadline = original ? millis() + 200 : 0;
            }
        } restore{self, originalDeadline};
        int32_t maximumBytes = 64 * 1024;
        if (argc > 1) {
            int32_t requested = maximumBytes;
            if (JS_ToInt32(ctx, &requested, argv[1]) == 0 && requested >= 1024 && requested <= 128 * 1024)
                maximumBytes = requested;
        }

        HTTPClient http;
        WiFiClientSecure secureClient;
        http.setConnectTimeout(4000);
        http.setTimeout(6000);
        http.setUserAgent("WioTrackerL2-PocketJS/1.0");
        // Optional third argument is a small headers object.  Keeping header
        // support here lets sandboxed apps call authenticated JSON APIs
        // without exposing a native, app-specific networking surface.
        if (argc > 2 && JS_IsObject(argv[2])) {
            JSValue authorization = JS_GetPropertyStr(ctx, argv[2], "authorization");
            if (!JS_IsException(authorization)) {
                const char *header = JS_ToCString(ctx, authorization);
                if (header && header[0]) http.addHeader("Authorization", header);
                if (header) JS_FreeCString(ctx, header);
            }
            JS_FreeValue(ctx, authorization);
        }
        const bool https = !strncmp(url, "https://", 8);
        if (https) secureClient.setInsecure();
        if (!(https ? http.begin(secureClient, url) : http.begin(url))) {
            JS_FreeCString(ctx, url);
            return JS_NULL;
        }
        const int status = http.GET();
        const int contentLength = http.getSize();
        if (status < 0 || contentLength > maximumBytes) {
            http.end(); JS_FreeCString(ctx, url);
            return JS_NULL;
        }
        WiFiClient *stream = http.getStreamPtr();
        uint8_t *buffer = static_cast<uint8_t *>(heap_caps_malloc(maximumBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!stream || !buffer) {
            if (buffer) heap_caps_free(buffer);
            http.end(); JS_FreeCString(ctx, url);
            return JS_NULL;
        }
        size_t used = 0;
        const uint32_t deadline = millis() + 6000;
        while (used < size_t(maximumBytes) && millis() < deadline) {
            // Many JSON APIs keep the TCP connection alive. Once the declared
            // response body is complete, return immediately instead of waiting
            // for the socket timeout on every PocketJS request.
            if (contentLength >= 0 && used >= size_t(contentLength)) break;
            const int available = stream->available();
            if (available <= 0) {
                if (!stream->connected()) break;
                delay(2);
                continue;
            }
            const int wanted = min<int>(available, int(maximumBytes - used));
            const int read = stream->read(buffer + used, wanted);
            if (read <= 0) break;
            used += size_t(read);
        }
        JSValue result = JS_NewStringLen(ctx, reinterpret_cast<const char *>(buffer), used);
        heap_caps_free(buffer);
        http.end();
        JS_FreeCString(ctx, url);
        return result;
    }

    void installApi() {
        JSValue global = JS_GetGlobalObject(context);
        JSValue pocket = JS_NewObject(context);
        auto add = [&](const char *name, JSCFunction *function, int argc) {
            JS_SetPropertyStr(context, pocket, name, JS_NewCFunction(context, function, name, argc));
        };
        add("log", jsLog, 1); add("setBackground", jsSetBackground, 1); add("setScrollable", jsSetScrollable, 1);
        add("setFullscreen", jsSetFullscreen, 1);
        add("text", jsText, 5); add("rect", jsRect, 6); add("scrollArea", jsScrollArea, 4); add("setParent", jsSetParent, 2);
        add("scrollTo", jsScrollTo, 2); add("getScrollY", jsGetScrollY, 1);
        add("number", jsNumber, 5);
        add("button", jsButton, 7); add("repeatButton", jsRepeatButton, 9);
        add("hitArea", jsHitArea, 5); add("pressArea", jsPressArea, 5); add("longPressArea", jsLongPressArea, 5); add("swipeArea", jsSwipeArea, 5); add("textArea", jsTextArea, 6);
        add("setTextAreaOneLine", jsSetTextAreaOneLine, 2); add("icon", jsIcon, 6);
        add("fitText", jsFitText, 2); add("setText", jsSetText, 2); add("setTextAlign", jsSetTextAlign, 2); add("getText", jsGetText, 1); add("setColor", jsSetColor, 2); add("setPosition", jsSetPosition, 3); add("setSize", jsSetSize, 3); add("focus", jsFocus, 1); add("setVisible", jsSetVisible, 2);
        add("wrapText", jsWrapText, 4);
        add("qrCode", jsQrCode, 4); add("setQrCode", jsSetQrCode, 2);
        add("grid", jsGrid, 7); add("setGridCell", jsSetGridCell, 4);
        add("gauge", jsGauge, 7); add("setGauge", jsSetGauge, 2);
        add("readDataFile", jsReadDataFile, 1); add("writeDataFile", jsWriteDataFile, 2);
        add("readFile", jsReadFile, 1); add("writeFile", jsWriteFile, 2); add("listFiles", jsListFiles, 0); add("renameFile", jsRenameFile, 2);
        add("tone", jsTone, 2); add("device", jsDevice, 0); add("httpGet", jsHttpGet, 2);
        add("i2cOpen", jsI2cOpen, 0); add("i2cClose", jsI2cClose, 0); add("i2cScan", jsI2cScan, 0);
        add("i2cProbe", jsI2cProbe, 1);
        add("i2cWrite", jsI2cWrite, 2); add("i2cRead", jsI2cRead, 2); add("i2cWriteRead", jsI2cWriteRead, 3);
        add("sdrStart", jsSdrStart, 1); add("sdrTune", jsSdrTune, 1);
        add("sdrSpectrum", jsSdrSpectrum, 0); add("sdrError", jsSdrError, 0); add("sdrStop", jsSdrStop, 0);
        add("takeChatTarget", jsTakeChatTarget, 0); add("chatUnread", jsChatUnread, 0); add("markChatRead", jsMarkChatRead, 1); add("setChatView", jsSetChatView, 3);
        add("chatHistory", jsChatHistory, 1); add("chatPeers", jsChatPeers, 0);
        add("notificationSound", jsNotificationSound, 2); add("previewNotificationSound", jsPreviewNotificationSound, 2);
        add("chatStorage", jsChatStorage, 0); add("deleteChatMessage", jsDeleteChatHistory, 1);
        add("meshMessages", jsMeshMessages, 0); add("meshMessageRevision", jsMeshMessageRevision, 0);
        add("communication", jsCommunication, 0);
        add("peers", jsCommunicationPeers, 0); add("advertise", jsAdvertise, 0);
        add("meshtasticConfig", jsMeshtasticConfig, 0); add("setMeshtasticRole", jsMeshtasticRole, 1); add("setMeshtasticRegion", jsMeshtasticRegion, 1);
        add("setMeshtasticFrequency", jsMeshtasticFrequency, 1); add("setMeshtasticChannel", jsMeshtasticChannel, 2); add("selectProtocol", jsSelectProtocol, 1);
        add("radioPresets", jsRadioPresets, 0); add("selectRadioPreset", jsSelectRadioPreset, 1);
        add("meshContacts", jsMeshContacts, 0); add("meshSend", jsMeshSend, 3); add("meshDeleteMessage", jsMeshDeleteMessage, 1);
        JS_SetPropertyStr(context, global, "Pocket", pocket);
        JS_FreeValue(context, global);
    }

    void drainJobs() {
        JSContext *jobContext = nullptr;
        for (uint8_t count = 0; count < 16; ++count) {
            const int result = JS_ExecutePendingJob(runtime, &jobContext);
            if (result <= 0) { if (result < 0) printException("pending job"); break; }
        }
    }
};

bool PocketJsRuntime::load(lv_obj_t *parent, const char *appDirectory, char *error, size_t errorSize) {
    if (!parent || !appDirectory) return false;
    char sourcePath[128]; snprintf(sourcePath, sizeof(sourcePath), "%s/app.js", appDirectory);
    File file = SD_MMC.open(sourcePath, FILE_READ);
    if (!file || !file.size() || file.size() > MAX_SOURCE_BYTES) {
        if (error && errorSize) snprintf(error, errorSize, "Missing/invalid %s", sourcePath);
        if (file) file.close(); return false;
    }
    const size_t length = file.size();
    char *source = static_cast<char *>(heap_caps_malloc(length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!source) { file.close(); if (error && errorSize) strlcpy(error, "Not enough PSRAM for app source", errorSize); return false; }
    const size_t read = file.readBytes(source, length); file.close(); source[read] = 0;
    if (read != length) {
        Serial.printf("PocketJS: incomplete SD read %s expected=%u actual=%u\n", sourcePath, unsigned(length), unsigned(read));
        heap_caps_free(source);
        if (error && errorSize) strlcpy(error, "SD app read failed", errorSize);
        return false;
    }

    const bool loaded = loadSource(parent, sourcePath, source, error, errorSize, appDirectory);
    if (loaded && state_) strlcpy(state_->directory, appDirectory, sizeof(state_->directory));
    heap_caps_free(source);
    return loaded;
}

bool PocketJsRuntime::loadSource(lv_obj_t *parent, const char *sourceName, const char *source,
                                 char *error, size_t errorSize, const char *appDirectory) {
    unload();
    if (!parent || !source || !source[0]) return false;
    const size_t length = strlen(source);
    if (length > MAX_SOURCE_BYTES) {
        if (error && errorSize) strlcpy(error, "PocketJS source is too large", errorSize);
        return false;
    }
    state_ = new State();
    if (!state_) { if (error && errorSize) strlcpy(error, "Runtime allocation failed", errorSize); return false; }
    strlcpy(state_->directory, appDirectory ? appDirectory : "/system", sizeof(state_->directory));
    const char *id = appDirectory ? strrchr(appDirectory, '/') : nullptr;
    snprintf(state_->dataDirectory, sizeof(state_->dataDirectory), "/appdata/%s", id ? id + 1 : "system");
    state_->root = lv_obj_create(parent); lv_obj_set_pos(state_->root, 0, 20); lv_obj_set_size(state_->root, 320, 220);
    lv_obj_set_style_bg_color(state_->root, lv_color_hex(0xF2F2F7), 0); lv_obj_set_style_bg_opa(state_->root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(state_->root, 0, 0); lv_obj_set_style_radius(state_->root, 0, 0);
    lv_obj_set_style_pad_all(state_->root, 0, 0); lv_obj_clear_flag(state_->root, LV_OBJ_FLAG_SCROLLABLE);

    state_->runtime = JS_NewRuntime2(&QJS_ALLOCATOR, state_);
    if (state_->runtime) {
        JS_SetMemoryLimit(state_->runtime, JS_HEAP_LIMIT); JS_SetMaxStackSize(state_->runtime, JS_STACK_LIMIT);
        JS_SetInterruptHandler(state_->runtime, State::interrupt, state_);
        state_->context = JS_NewContext(state_->runtime);
    }
    if (!state_->context) {
        if (error && errorSize) strlcpy(error, "QuickJS initialization failed", errorSize); unload(); return false;
    }
    JS_SetContextOpaque(state_->context, state_); state_->installApi();
    state_->deadline = millis() + EVAL_BUDGET_MS;
    JSValue result = JS_Eval(state_->context, source, length, sourceName ? sourceName : "system.js", JS_EVAL_TYPE_GLOBAL);
    state_->deadline = 0;
    if (JS_IsException(result)) {
        state_->printException("app load");
        if (error && errorSize) strlcpy(error, "JavaScript load failed; see serial log", errorSize);
        JS_FreeValue(state_->context, result); unload(); return false;
    }
    JS_FreeValue(state_->context, result); state_->drainJobs(); state_->nextFrame = millis();
    Serial.printf("PocketJS: loaded %s, PSRAM free=%u, heap free=%u\n", sourceName ? sourceName : "system.js",
                  unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), unsigned(ESP.getFreeHeap()));
    return true;
}

void PocketJsRuntime::loop() {
    if (!state_ || !state_->context || int32_t(millis() - state_->nextFrame) < 0 || state_->runningCallback) return;
    state_->nextFrame = millis() + 16;
    const uint32_t loopNow = millis();
    for (uint8_t i = 0; i < state_->buttonCount; ++i) {
        State::ButtonBinding &binding = state_->buttons[i];
        if (!binding.repeatInterval || !binding.pressedAt || !binding.object ||
            !lv_obj_has_state(binding.object, LV_STATE_PRESSED) ||
            loopNow - binding.pressedAt < binding.repeatDelay ||
            loopNow - binding.lastRepeat < binding.repeatInterval) continue;
        binding.lastRepeat = loopNow;
        state_->deadline = loopNow + CALLBACK_BUDGET_MS; state_->runningCallback = true;
        JSValue repeated = JS_Call(state_->context, binding.callback, JS_UNDEFINED, 0, nullptr);
        state_->runningCallback = false; state_->deadline = 0;
        if (JS_IsException(repeated)) state_->printException("repeat button callback");
        JS_FreeValue(state_->context, repeated); state_->drainJobs();
    }
    JSValue global = JS_GetGlobalObject(state_->context);
    JSValue frame = JS_GetPropertyStr(state_->context, global, "frame");
    if (JS_IsFunction(state_->context, frame)) {
        JSValue now = JS_NewInt64(state_->context, int64_t(millis()));
        state_->deadline = millis() + CALLBACK_BUDGET_MS;
        JSValue result = JS_Call(state_->context, frame, global, 1, &now);
        state_->deadline = 0;
        if (JS_IsException(result)) state_->printException("frame");
        JS_FreeValue(state_->context, result); JS_FreeValue(state_->context, now); state_->drainJobs();
    }
    JS_FreeValue(state_->context, frame); JS_FreeValue(state_->context, global);
}

void PocketJsRuntime::dismissKeyboard() {
    if (!state_ || !state_->keyboard || !lv_obj_is_valid(state_->keyboard)) return;
    lv_obj_add_flag(state_->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(state_->keyboard, nullptr);
    if (state_->root) lv_obj_set_y(state_->root, state_->keyboardRootY);
}

void PocketJsRuntime::unload() {
    appUI.setChatView(false, "", false);
    if (!state_) return;
    sdrService.stop();
    hardware.setGroveEnabled(false);
    if (state_->keyboard && lv_obj_is_valid(state_->keyboard)) { lv_keyboard_set_textarea(state_->keyboard, nullptr); lv_obj_delete(state_->keyboard); state_->keyboard = nullptr; }
    if (state_->root && lv_obj_is_valid(state_->root)) { lv_obj_delete(state_->root); state_->root = nullptr; }
    if (state_->context) {
        for (uint8_t i = 0; i < state_->buttonCount; ++i) JS_FreeValue(state_->context, state_->buttons[i].callback);
        JS_FreeContext(state_->context); state_->context = nullptr;
    }
    for (uint8_t i = 0; i < state_->gridCount; ++i) if (state_->grids[i].buffer) {
        lv_draw_buf_destroy(state_->grids[i].buffer); state_->grids[i].buffer = nullptr;
    }
    if (state_->runtime) { JS_FreeRuntime(state_->runtime); state_->runtime = nullptr; }
    delete state_; state_ = nullptr;
}

bool PocketJsRuntime::active() const { return state_ && state_->context; }
bool PocketJsRuntime::fullscreen() const { return active() && state_->fullscreen; }
const char *PocketJsRuntime::appDirectory() const { return state_ ? state_->directory : ""; }

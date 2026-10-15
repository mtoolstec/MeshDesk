#pragma once
#include <lvgl.h>
#include <initializer_list>

// Snapshot the screen plus display overlays (full-screen apps, menus, keyboard).
// LVGL's object snapshot alone excludes the top and system layers.
inline lv_draw_buf_t *takeDisplaySnapshot() {
    lv_draw_buf_t *screen = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!screen) return nullptr;
    for (lv_obj_t *layer : {lv_layer_top(), lv_layer_sys()}) {
        bool visible = false;
        for (uint32_t i = 0; i < lv_obj_get_child_count(layer); ++i)
            if (!lv_obj_has_flag(lv_obj_get_child(layer, i), LV_OBJ_FLAG_HIDDEN)) { visible = true; break; }
        if (!visible) continue;
        lv_draw_buf_t *overlay = lv_snapshot_take(layer, LV_COLOR_FORMAT_ARGB8888);
        if (!overlay) { lv_draw_buf_destroy(screen); return nullptr; }
        const uint32_t width = screen->header.w < overlay->header.w ? screen->header.w : overlay->header.w;
        const uint32_t height = screen->header.h < overlay->header.h ? screen->header.h : overlay->header.h;
        for (uint32_t y = 0; y < height; ++y) {
            auto *dest = reinterpret_cast<uint16_t *>(screen->data + y * screen->header.stride);
            const uint8_t *src = overlay->data + y * overlay->header.stride;
            for (uint32_t x = 0; x < width; ++x, src += 4) {
                const unsigned alpha = src[3];
                if (!alpha) continue;
                const uint16_t pixel = dest[x];
                const unsigned r = (unsigned(src[2]) * alpha + ((pixel >> 11) * 255 / 31) * (255 - alpha) + 127) / 255;
                const unsigned g = (unsigned(src[1]) * alpha + (((pixel >> 5) & 63) * 255 / 63) * (255 - alpha) + 127) / 255;
                const unsigned b = (unsigned(src[0]) * alpha + ((pixel & 31) * 255 / 31) * (255 - alpha) + 127) / 255;
                dest[x] = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
            }
        }
        lv_draw_buf_destroy(overlay);
    }
    return screen;
}

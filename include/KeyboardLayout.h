#pragma once
#include <lvgl.h>

// Keep every default symbol and place decimal point next to the digits.
// Shared by native and PocketJS keyboards (LVGL symbol mode is the 1# key).
inline void configureKeyboardLayout(lv_obj_t *keyboard) {
    static const char * const map[] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", ".", LV_SYMBOL_BACKSPACE, "\n",
        "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
        "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
        LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
    };
    constexpr auto control = [](unsigned flags) { return static_cast<lv_buttonmatrix_ctrl_t>(flags); };
    constexpr auto special = LV_KEYBOARD_CTRL_BUTTON_FLAGS;
    constexpr auto key = control(LV_BUTTONMATRIX_CTRL_POPOVER | 1);
    static const lv_buttonmatrix_ctrl_t controls[] = {
        key,key,key,key,key,key,key,key,key,key,key,control(LV_BUTTONMATRIX_CTRL_CHECKED|2),
        control(special|2),key,key,key,key,key,key,key,key,key,key,key,
        key,key,key,key,key,key,key,key,key,key,key,key,
        control(special|2),control(LV_BUTTONMATRIX_CTRL_CHECKED|2),control(6),control(LV_BUTTONMATRIX_CTRL_CHECKED|2),control(special|2)
    };
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_SPECIAL, map, controls);
}

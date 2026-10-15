#pragma once

#include <Arduino.h>
#include <lvgl.h>

class PocketJsRuntime {
public:
    bool load(lv_obj_t *parent, const char *appDirectory, char *error, size_t errorSize);
    bool loadSource(lv_obj_t *parent, const char *sourceName, const char *source,
                    char *error, size_t errorSize, const char *appDirectory = nullptr);
    void loop();
    void unload();
    void dismissKeyboard();
    bool active() const;
    bool fullscreen() const;
    const char *appDirectory() const;

private:
    struct State;
    State *state_ = nullptr;
};

extern PocketJsRuntime pocketJsRuntime;

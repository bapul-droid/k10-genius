#pragma once
#include <lvgl.h>

// K10 Muse pixel avatar with state and emotion transitions.
// All functions run under the LVGL lock.
struct MuseLvgl {
    lv_obj_t* canvas = nullptr;
    lv_timer_t* timer = nullptr;
    uint16_t* pixels = nullptr;
    uint32_t bg_color = 0x000000;
    bool awaiting_reply = false;
    int last_device_state = -1;
    int last_mode = -1;
    uint32_t mode_started_ms = 0;
    uint32_t happy_until_ms = 0;
};

bool muse_lvgl_start(MuseLvgl& avatar, lv_obj_t* parent, uint32_t bg_color = 0x000000);
void muse_lvgl_stop(MuseLvgl& avatar);
void muse_lvgl_set_bg_color(MuseLvgl& avatar, uint32_t bg_rgb);
void muse_lvgl_trigger_happy(MuseLvgl& avatar, uint32_t duration_ms = 1800);

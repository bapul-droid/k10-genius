#pragma once
#include <lvgl.h>
#include "anime_data.h"

// K10 Customer Service Anime pixel avatar with state and emotion transitions.
// All functions run under the LVGL lock.
struct AnimeLvgl {
    lv_obj_t* canvas = nullptr;
    lv_timer_t* timer = nullptr;
    uint16_t* pixels = nullptr;
    uint32_t bg_color = 0x000000;
    bool awaiting_reply = false;
    int last_device_state = -1;
    int last_mode = -1;
    uint32_t mode_started_ms = 0;
    uint32_t happy_until_ms = 0;
    uint32_t wink_until_ms = 0;
    uint32_t thinking_until_ms = 0;
    uint32_t error_until_ms = 0;

    // Lip-sync ROI tracking
    anime_mouth_t current_mouth = ANIME_MOUTH_CLOSED;
    anime_mouth_t last_drawn_mouth = ANIME_MOUTH_CLOSED;
    uint32_t mouth_hold_until_ms = 0;
    int last_rendered_frame = -1;

    // Natural random blink state machine
    uint32_t next_blink_ms = 0;
    uint32_t blink_start_ms = 0;
    bool is_blinking = false;
    bool is_double_blink = false;
    bool last_drawn_eyes_closed = false;
};

bool anime_lvgl_start(AnimeLvgl& avatar, lv_obj_t* parent, uint32_t bg_color = 0x000000);
void anime_lvgl_stop(AnimeLvgl& avatar);
void anime_lvgl_set_bg_color(AnimeLvgl& avatar, uint32_t bg_rgb);
void anime_lvgl_trigger_happy(AnimeLvgl& avatar, uint32_t duration_ms = 1800);
void anime_lvgl_trigger_wink(AnimeLvgl& avatar, uint32_t duration_ms = 1200);
void anime_lvgl_trigger_thinking(AnimeLvgl& avatar, uint32_t duration_ms = 15000);
void anime_lvgl_trigger_error(AnimeLvgl& avatar, uint32_t duration_ms = 3000);
void anime_lvgl_clear_thinking(AnimeLvgl& avatar);
void anime_lvgl_clear_error(AnimeLvgl& avatar);

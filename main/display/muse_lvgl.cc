#include "muse_lvgl.h"
#include "muse_pixel.h"
#include "muse_avatar_adapter.h"
#include "application.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cmath>
#include <cstring>

namespace {
constexpr int kSize = 160;
constexpr uint32_t kPeriodMs = 100;
constexpr uint32_t kHappyDurationMs = 1800; // 1.8s happy reaction (1.4s full + 0.4s ease out)

void render(lv_timer_t* timer) {
    auto* a = static_cast<MuseLvgl*>(lv_timer_get_user_data(timer));
    if (!a || !a->canvas || !a->pixels) return;
    // Camera preview hides the emoji parent. Avoid rendering while it is hidden.
    if (lv_obj_has_flag(lv_obj_get_parent(a->canvas), LV_OBJ_FLAG_HIDDEN)) return;

    const uint32_t now = lv_tick_get();
    const auto device_state = Application::GetInstance().GetDeviceState();

    if (a->last_device_state != static_cast<int>(device_state)) {
        // Transisi selesai speaking: picu reaksi Happy layaknya Muse Gadget!
        if (a->last_device_state == static_cast<int>(kDeviceStateSpeaking)) {
            a->happy_until_ms = now + kHappyDurationMs;
            a->thinking_until_ms = 0;
            ESP_LOGI("MuseLvgl", "Speaking finished -> trigger happy transition (%u ms)", (unsigned)kHappyDurationMs);
        } else if (a->last_device_state == static_cast<int>(kDeviceStateListening) &&
                   device_state == kDeviceStateIdle) {
            // Pengguna selesai berbicara dan menunggu respons (thinking / mencari layanan)
            a->thinking_until_ms = now + 15000;
            ESP_LOGI("MuseLvgl", "Listening finished -> thinking phase started");
        } else if (device_state == kDeviceStateSpeaking) {
            // Jawaban AI mulai diputar, fase thinking selesai
            a->thinking_until_ms = 0;
        } else if (device_state == kDeviceStateListening) {
            // Siklus listening baru dimulai
            a->thinking_until_ms = 0;
            a->error_until_ms = 0;
        }

        a->awaiting_reply = false;
        a->last_device_state = static_cast<int>(device_state);
    }

    muse_mode_t mode = MUSE_MODE_IDLE;
    if (device_state == kDeviceStateFatalError || now < a->error_until_ms) {
        mode = MUSE_MODE_ERROR;
    } else if (device_state == kDeviceStateSpeaking) {
        mode = MUSE_MODE_SPEAKING;
    } else if (device_state == kDeviceStateListening) {
        mode = MUSE_MODE_LISTENING;
    } else if (device_state == kDeviceStateConnecting || now < a->thinking_until_ms) {
        mode = MUSE_MODE_THINKING;
    } else {
        switch (MuseAvatarStateFromDeviceState(device_state)) {
            case MuseAvatarState::Boot: mode = MUSE_MODE_BOOT; break;
            default: mode = MUSE_MODE_IDLE; break;
        }
    }

    if (a->last_mode != static_cast<int>(mode)) {
        a->last_mode = static_cast<int>(mode);
        a->mode_started_ms = now;
        ESP_LOGI("MuseLvgl", "Mode -> %d", static_cast<int>(mode));
    }

    // Hitung kurva happy (0.0 .. 1.0)
    float happy = 0.0f;
    if (now < a->happy_until_ms && mode != MUSE_MODE_ERROR) {
        float left = static_cast<float>(a->happy_until_ms - now) / 1000.0f;
        // Penuh 1.0 sampai sisa 0.4s, lalu ease out halus ke 0.0
        happy = (left > 0.4f) ? 1.0f : (left / 0.4f);
    }

    // Level modulasi suara saat speaking agar mulut avatar bergerak aktif
    float level = 0.0f;
    if (mode == MUSE_MODE_SPEAKING) {
        float t = now / 1000.0f;
        level = 0.35f + 0.35f * fabsf(sinf(t * 11.0f) * cosf(t * 5.3f));
    }

    muse_pose_t pose{};
    pose.mode = mode;
    pose.t = now / 1000.0f;
    pose.mode_t = static_cast<uint32_t>(now - a->mode_started_ms) / 1000.0f;
    pose.level = level;
    pose.happy = happy;

    muse_pixel_set_bg_color(a->bg_color);
    muse_pixel_render(&pose);
    muse_pixel_scale(a->pixels, kSize, 0, kSize - 1, 0, kSize - 1);
    lv_obj_invalidate(a->canvas);
}
}

void muse_lvgl_trigger_happy(MuseLvgl& a, uint32_t duration_ms) {
    a.happy_until_ms = lv_tick_get() + duration_ms;
}

void muse_lvgl_trigger_thinking(MuseLvgl& a, uint32_t duration_ms) {
    a.thinking_until_ms = lv_tick_get() + duration_ms;
    a.error_until_ms = 0;
    ESP_LOGI("MuseLvgl", "Trigger thinking state (%u ms)", (unsigned)duration_ms);
}

void muse_lvgl_trigger_error(MuseLvgl& a, uint32_t duration_ms) {
    a.error_until_ms = lv_tick_get() + duration_ms;
    a.thinking_until_ms = 0;
    a.happy_until_ms = 0;
    ESP_LOGI("MuseLvgl", "Trigger error state (%u ms)", (unsigned)duration_ms);
}

void muse_lvgl_clear_thinking(MuseLvgl& a) {
    a.thinking_until_ms = 0;
}

void muse_lvgl_clear_error(MuseLvgl& a) {
    a.error_until_ms = 0;
}

void muse_lvgl_set_bg_color(MuseLvgl& a, uint32_t bg_rgb) {
    a.bg_color = bg_rgb & 0xFFFFFF;
    muse_pixel_set_bg_color(a.bg_color);
    if (a.canvas && a.pixels) {
        const uint16_t bg565 = lv_color_to_u16(lv_color_hex(a.bg_color));
        for (int i = 0; i < kSize * kSize; ++i) {
            a.pixels[i] = bg565;
        }
        lv_obj_invalidate(a.canvas);
    }
}

bool muse_lvgl_start(MuseLvgl& a, lv_obj_t* parent, uint32_t bg_color) {
    if (!parent || a.canvas) return false;
    a.pixels = static_cast<uint16_t*>(heap_caps_malloc(kSize * kSize * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!a.pixels) {
        ESP_LOGW("MuseLvgl", "PSRAM unavailable, keeping XiaoZhi emoji");
        return false;
    }
    a.bg_color = bg_color & 0xFFFFFF;
    muse_pixel_set_bg_color(a.bg_color);
    const uint16_t bg565 = lv_color_to_u16(lv_color_hex(a.bg_color));
    for (int i = 0; i < kSize * kSize; ++i) {
        a.pixels[i] = bg565;
    }
    muse_pixel_set_size(kSize);
    a.canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(a.canvas, a.pixels, kSize, kSize, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(a.canvas);
    lv_obj_remove_flag(a.canvas, LV_OBJ_FLAG_SCROLLABLE);
    a.timer = lv_timer_create(render, kPeriodMs, &a);
    if (!a.timer) {
        lv_obj_delete(a.canvas);
        a.canvas = nullptr;
        heap_caps_free(a.pixels);
        a.pixels = nullptr;
        return false;
    }
    render(a.timer);
    ESP_LOGI("MuseLvgl", "Muse avatar started: %dx%d at %u ms with happy transition", kSize, kSize, (unsigned)kPeriodMs);
    return true;
}

void muse_lvgl_stop(MuseLvgl& a) {
    if (a.timer) { lv_timer_delete(a.timer); a.timer = nullptr; }
    if (a.canvas) { lv_obj_delete(a.canvas); a.canvas = nullptr; }
    if (a.pixels) { heap_caps_free(a.pixels); a.pixels = nullptr; }
}

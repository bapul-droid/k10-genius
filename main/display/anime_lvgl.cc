#include "anime_lvgl.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cmath>
#include <cstring>
#include "anime_pixel.h"
#include "application.h"

namespace {
constexpr int kW = ANIME_PX_W;               // 240
constexpr int kH = ANIME_PX_H;               // 320
constexpr uint32_t kPeriodMs = 50;           // 20 FPS for responsive, natural lip sync
constexpr uint32_t kHappyDurationMs = 1800;  // 1.8s happy reaction when speaking finishes

void render(lv_timer_t* timer) {
    auto* a = static_cast<AnimeLvgl*>(lv_timer_get_user_data(timer));
    if (!a || !a->canvas || !a->pixels)
        return;
    // Don't render while canvas is hidden (e.g. camera preview)
    if (lv_obj_has_flag(a->canvas, LV_OBJ_FLAG_HIDDEN))
        return;

    const uint32_t now = lv_tick_get();
    const auto device_state = Application::GetInstance().GetDeviceState();

    if (a->last_device_state != static_cast<int>(device_state)) {
        if (a->last_device_state == static_cast<int>(kDeviceStateSpeaking)) {
            a->happy_until_ms = now + kHappyDurationMs;
            a->thinking_until_ms = 0;
            ESP_LOGI("AnimeLvgl", "Speaking finished -> trigger happy transition (%u ms)",
                     (unsigned)kHappyDurationMs);
        } else if (a->last_device_state == static_cast<int>(kDeviceStateListening) &&
                   device_state == kDeviceStateIdle) {
            a->thinking_until_ms = now + 15000;
            ESP_LOGI("AnimeLvgl", "Listening finished -> thinking phase started");
        } else if (device_state == kDeviceStateSpeaking) {
            a->thinking_until_ms = 0;
        } else if (device_state == kDeviceStateListening) {
            a->thinking_until_ms = 0;
            a->error_until_ms = 0;
        }

        a->awaiting_reply = false;
        a->last_device_state = static_cast<int>(device_state);
    }

    anime_mode_t mode = ANIME_MODE_IDLE;
    if (device_state == kDeviceStateFatalError || now < a->error_until_ms) {
        mode = ANIME_MODE_ERROR;
    } else if (device_state == kDeviceStateSpeaking || device_state == kDeviceStateNotifying) {
        mode = ANIME_MODE_SPEAKING;
    } else if (device_state == kDeviceStateListening) {
        mode = ANIME_MODE_LISTENING;
    } else if (device_state == kDeviceStateConnecting || now < a->thinking_until_ms) {
        mode = ANIME_MODE_THINKING;
    } else if (device_state == kDeviceStateStarting) {
        mode = ANIME_MODE_BOOT;
    } else {
        mode = ANIME_MODE_IDLE;
    }

    bool mode_changed = false;
    if (a->last_mode != static_cast<int>(mode)) {
        a->last_mode = static_cast<int>(mode);
        a->mode_started_ms = now;
        mode_changed = true;
        ESP_LOGI("AnimeLvgl", "Mode -> %d", static_cast<int>(mode));
    }

    // Compute happy transition curve (0.0 .. 1.0)
    float happy = 0.0f;
    if (now < a->happy_until_ms && mode != ANIME_MODE_ERROR) {
        float left = static_cast<float>(a->happy_until_ms - now) / 1000.0f;
        happy = (left > 0.4f) ? 1.0f : (left / 0.4f);
    }

    bool wink = (now < a->wink_until_ms);

    if (mode == ANIME_MODE_SPEAKING) {
        // Base image for speaking is IDLE (stationary, clean hair and face)
        if (mode_changed || a->last_rendered_frame != ANIME_FRAME_IDLE) {
            anime_pixel_render_240x320(a->pixels, mode, now, 0.0f, 0.0f, false, a->bg_color);
            a->last_rendered_frame = ANIME_FRAME_IDLE;
            a->current_mouth = ANIME_MOUTH_CLOSED;
            a->last_drawn_mouth = ANIME_MOUTH_CLOSED;
            a->mouth_hold_until_ms = 0;
            lv_obj_invalidate(a->canvas);
        }

        // Real-time PCM TTS audio playback lip-sync
        float level = anime_get_audio_playback_level();
        anime_mouth_t target_mouth = ANIME_MOUTH_CLOSED;
        if (level >= 0.50f) {
            target_mouth = ANIME_MOUTH_OPEN;
        } else if (level >= 0.15f) {
            target_mouth = ANIME_MOUTH_HALF;
        } else {
            target_mouth = ANIME_MOUTH_CLOSED;
        }

        // Anti-jitter smoothing: enforce minimum hold time (~60ms) when opening mouth
        if (target_mouth > a->current_mouth) {
            a->current_mouth = target_mouth;
            a->mouth_hold_until_ms = now + 60;
        } else if (target_mouth < a->current_mouth) {
            if (now >= a->mouth_hold_until_ms) {
                a->current_mouth = target_mouth;
            }
        }

        // Only patch and invalidate the mouth ROI when mouth shape changes!
        if (a->current_mouth != a->last_drawn_mouth) {
            anime_pixel_patch_mouth(a->pixels, a->current_mouth);
            a->last_drawn_mouth = a->current_mouth;
            lv_obj_invalidate(a->canvas);
        }
    } else {
        // Non-speaking mode: ensure mouth state is reset
        if (a->last_drawn_mouth != ANIME_MOUTH_CLOSED) {
            anime_pixel_patch_mouth(a->pixels, ANIME_MOUTH_CLOSED);
            a->current_mouth = ANIME_MOUTH_CLOSED;
            a->last_drawn_mouth = ANIME_MOUTH_CLOSED;
            lv_obj_invalidate(a->canvas);
        }

        float level = 0.0f;
        if (mode == ANIME_MODE_LISTENING) {
            float t = now / 1000.0f;
            level = 0.05f + 0.05f * sinf(t * 3.0f);
            anime_pixel_render_240x320(a->pixels, mode, now, level, happy, wink, a->bg_color);
            a->last_rendered_frame = ANIME_FRAME_LISTEN;
            lv_obj_invalidate(a->canvas);
        } else {
            // Check frame to render
            anime_frame_t target_frame = ANIME_FRAME_IDLE;
            if (mode == ANIME_MODE_THINKING || mode == ANIME_MODE_BOOT) {
                target_frame = ANIME_FRAME_THINK;
            } else if (mode == ANIME_MODE_ERROR) {
                target_frame = ANIME_FRAME_SURPRISED;
            } else {
                // IDLE mode
                if (happy > 0.3f) {
                    target_frame = ANIME_FRAME_SMILE;
                } else if (wink) {
                    target_frame = ANIME_FRAME_WINK;
                } else {
                    uint32_t blink_cycle = now % 3500;
                    if (blink_cycle < 160) {
                        target_frame = ANIME_FRAME_BLINK;
                    } else {
                        target_frame = ANIME_FRAME_IDLE;
                    }
                }
            }

            if (mode_changed || target_frame != a->last_rendered_frame || happy > 0.0f || wink) {
                anime_pixel_render_240x320(a->pixels, mode, now, level, happy, wink, a->bg_color);
                a->last_rendered_frame = target_frame;
                lv_obj_invalidate(a->canvas);
            }
        }
    }
}
}  // namespace

void anime_lvgl_trigger_happy(AnimeLvgl& a, uint32_t duration_ms) {
    a.happy_until_ms = lv_tick_get() + duration_ms;
}

void anime_lvgl_trigger_wink(AnimeLvgl& a, uint32_t duration_ms) {
    a.wink_until_ms = lv_tick_get() + duration_ms;
}

void anime_lvgl_trigger_thinking(AnimeLvgl& a, uint32_t duration_ms) {
    a.thinking_until_ms = lv_tick_get() + duration_ms;
    a.error_until_ms = 0;
    ESP_LOGI("AnimeLvgl", "Trigger thinking state (%u ms)", (unsigned)duration_ms);
}

void anime_lvgl_trigger_error(AnimeLvgl& a, uint32_t duration_ms) {
    a.error_until_ms = lv_tick_get() + duration_ms;
    a.thinking_until_ms = 0;
    a.happy_until_ms = 0;
    a.wink_until_ms = 0;
    ESP_LOGI("AnimeLvgl", "Trigger error state (%u ms)", (unsigned)duration_ms);
}

void anime_lvgl_clear_thinking(AnimeLvgl& a) { a.thinking_until_ms = 0; }

void anime_lvgl_clear_error(AnimeLvgl& a) { a.error_until_ms = 0; }

void anime_lvgl_set_bg_color(AnimeLvgl& a, uint32_t bg_rgb) {
    a.bg_color = bg_rgb & 0xFFFFFF;
    if (a.canvas && a.pixels) {
        const uint16_t bg565 = lv_color_to_u16(lv_color_hex(a.bg_color));
        for (int i = 0; i < kW * kH; ++i) {
            a.pixels[i] = bg565;
        }
        lv_obj_invalidate(a.canvas);
    }
}

bool anime_lvgl_start(AnimeLvgl& a, lv_obj_t* parent, uint32_t bg_color) {
    if (!parent || a.canvas)
        return false;

    // Initialize & decompress frames into PSRAM
    if (!anime_pixel_init()) {
        ESP_LOGE("AnimeLvgl", "Failed to initialize anime frames in PSRAM");
        return false;
    }

    a.pixels = static_cast<uint16_t*>(
        heap_caps_malloc(kW * kH * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!a.pixels) {
        ESP_LOGW("AnimeLvgl", "PSRAM unavailable for canvas buffer");
        anime_pixel_deinit();
        return false;
    }

    a.bg_color = bg_color & 0xFFFFFF;
    const uint16_t bg565 = lv_color_to_u16(lv_color_hex(a.bg_color));
    for (int i = 0; i < kW * kH; ++i) {
        a.pixels[i] = bg565;
    }

    a.canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(a.canvas, a.pixels, kW, kH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(a.canvas, 0, 0);
    lv_obj_set_size(a.canvas, kW, kH);
    lv_obj_remove_flag(a.canvas, LV_OBJ_FLAG_SCROLLABLE);

    a.timer = lv_timer_create(render, kPeriodMs, &a);
    if (!a.timer) {
        lv_obj_delete(a.canvas);
        a.canvas = nullptr;
        heap_caps_free(a.pixels);
        a.pixels = nullptr;
        anime_pixel_deinit();
        return false;
    }

    a.current_mouth = ANIME_MOUTH_CLOSED;
    a.last_drawn_mouth = ANIME_MOUTH_CLOSED;
    a.mouth_hold_until_ms = 0;
    a.last_rendered_frame = -1;
    a.last_mode = -1;
    a.last_device_state = -1;

    render(a.timer);
    ESP_LOGI("AnimeLvgl", "Fullscreen Anime avatar started: %dx%d at %u ms", kW, kH,
             (unsigned)kPeriodMs);
    return true;
}

void anime_lvgl_stop(AnimeLvgl& a) {
    if (a.timer) {
        lv_timer_delete(a.timer);
        a.timer = nullptr;
    }
    if (a.canvas) {
        lv_obj_delete(a.canvas);
        a.canvas = nullptr;
    }
    if (a.pixels) {
        heap_caps_free(a.pixels);
        a.pixels = nullptr;
    }
    anime_pixel_deinit();
}

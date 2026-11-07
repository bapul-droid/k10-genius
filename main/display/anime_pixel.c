/*
 * anime_pixel.c - Fullscreen 240x320 Mascot Renderer for DFRobot K10
 * Based on Hermes Minji architecture.
 */

#include "anime_pixel.h"
#include <miniz.h>
#include "anime_data.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG "AnimePixel"

#define W ANIME_IMG_W  /* 240 */
#define H ANIME_IMG_H  /* 320 */
#define PIXELS (W * H) /* 76800 */

static uint8_t* s_decomp_psram = NULL;
static const uint16_t* s_frames_rgb[ANIME_FRAME_COUNT] = {NULL};

static inline uint16_t rgb565(uint32_t c) {
    uint8_t r = (c >> 16) & 0xff;
    uint8_t g = (c >> 8) & 0xff;
    uint8_t b = c & 0xff;
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static inline uint16_t blend_rgb565(uint16_t bg, uint16_t fg, uint8_t alpha) {
    if (alpha == 0)
        return bg;
    if (alpha >= 250)
        return fg;
    uint32_t bg_r = (bg >> 11) & 0x1f;
    uint32_t bg_g = (bg >> 5) & 0x3f;
    uint32_t bg_b = bg & 0x1f;

    uint32_t fg_r = (fg >> 11) & 0x1f;
    uint32_t fg_g = (fg >> 5) & 0x3f;
    uint32_t fg_b = fg & 0x1f;

    uint32_t inv_a = 255 - alpha;
    uint32_t r = (fg_r * alpha + bg_r * inv_a) / 255;
    uint32_t g = (fg_g * alpha + bg_g * inv_a) / 255;
    uint32_t b = (fg_b * alpha + bg_b * inv_a) / 255;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

bool anime_pixel_init(void) {
    if (s_decomp_psram) {
        return true;
    }

    size_t total_bytes = (size_t)ANIME_FRAME_COUNT * ANIME_RAW_FRAME_SIZE;
    s_decomp_psram = (uint8_t*)heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_decomp_psram) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM for anime frames",
                 (unsigned)total_bytes);
        return false;
    }

    /* Allocate decompressor on heap to avoid task main stack overflow.
     * tinfl_decompressor is ~11KB which exceeds the 8KB task stack if placed as a local variable.
     */
    tinfl_decompressor* decomp = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
    if (!decomp) {
        ESP_LOGE(TAG, "Failed to allocate memory for tinfl_decompressor");
        heap_caps_free(s_decomp_psram);
        s_decomp_psram = NULL;
        return false;
    }

    for (int i = 0; i < ANIME_FRAME_COUNT; ++i) {
        uint8_t* frame_dst = s_decomp_psram + ((size_t)i * ANIME_RAW_FRAME_SIZE);
        tinfl_init(decomp);
        size_t in_buf_size = g_anime_comp_size[i];
        size_t out_buf_size = ANIME_RAW_FRAME_SIZE;
        mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        tinfl_status status =
            tinfl_decompress(decomp, (const mz_uint8*)g_anime_comp[i], &in_buf_size,
                             (mz_uint8*)frame_dst, (mz_uint8*)frame_dst, &out_buf_size, flags);
        if (status != TINFL_STATUS_DONE || out_buf_size != ANIME_RAW_FRAME_SIZE) {
            ESP_LOGE(TAG, "Frame %d decompression failed (status=%d, written=%u, expected=%u)", i,
                     (int)status, (unsigned)out_buf_size, (unsigned)ANIME_RAW_FRAME_SIZE);
            free(decomp);
            heap_caps_free(s_decomp_psram);
            s_decomp_psram = NULL;
            return false;
        }
        s_frames_rgb[i] = (const uint16_t*)frame_dst;
    }

    free(decomp);

    ESP_LOGI(TAG, "Decompressed %d fullscreen 240x320 anime frames into PSRAM (%u KB)",
             ANIME_FRAME_COUNT, (unsigned)(total_bytes / 1024));
    return true;
}

void anime_pixel_deinit(void) {
    if (s_decomp_psram) {
        heap_caps_free(s_decomp_psram);
        s_decomp_psram = NULL;
        for (int i = 0; i < ANIME_FRAME_COUNT; ++i) {
            s_frames_rgb[i] = NULL;
        }
        ESP_LOGI(TAG, "Anime PSRAM frames freed");
    }
}

bool anime_pixel_is_ready(void) { return s_decomp_psram != NULL; }

/*
 * Sound wave arc coordinates for listening mode indicator ')))' (240x320 coordinates)
 * Situated in empty margin to the left of the headset earpiece (X: 10..30, Y: 73..131).
 */
static const int16_t s_wave_inner[][2] = {
    {30, 86},  {28, 87},  {29, 87},  {27, 88},  {28, 88},  {26, 89},  {27, 89},  {26, 90},
    {25, 91},  {24, 92},  {25, 92},  {24, 93},  {23, 94},  {24, 94},  {23, 95},  {23, 96},
    {22, 97},  {23, 97},  {22, 98},  {22, 99},  {22, 100}, {22, 101}, {22, 102}, {22, 103},
    {22, 104}, {22, 105}, {22, 106}, {22, 107}, {23, 107}, {23, 108}, {23, 109}, {23, 110},
    {24, 110}, {24, 111}, {24, 112}, {25, 112}, {25, 113}, {26, 114}, {26, 115}, {27, 115},
    {27, 116}, {28, 116}, {28, 117}, {29, 117}, {30, 118},
};
#define WAVE_INNER_COUNT (sizeof(s_wave_inner) / sizeof(s_wave_inner[0]))

static const int16_t s_wave_middle[][2] = {
    {27, 79},  {26, 80},  {27, 80},  {25, 81},  {26, 81},  {24, 82},  {25, 82},  {23, 83},
    {22, 84},  {21, 85},  {22, 85},  {20, 86},  {21, 86},  {20, 87},  {19, 88},  {20, 88},
    {19, 89},  {18, 90},  {19, 90},  {18, 91},  {18, 92},  {17, 93},  {17, 94},  {17, 95},
    {17, 96},  {16, 97},  {16, 98},  {16, 99},  {16, 100}, {16, 101}, {16, 102}, {16, 103},
    {16, 104}, {16, 105}, {16, 106}, {16, 107}, {17, 108}, {17, 109}, {17, 110}, {17, 111},
    {18, 112}, {18, 113}, {18, 114}, {19, 114}, {19, 115}, {19, 116}, {20, 116}, {20, 117},
    {20, 118}, {21, 118}, {21, 119}, {22, 119}, {22, 120}, {23, 121}, {24, 122}, {25, 123},
    {26, 123}, {26, 124}, {27, 124}, {27, 125}, {28, 125},
};
#define WAVE_MIDDLE_COUNT (sizeof(s_wave_middle) / sizeof(s_wave_middle[0]))

static const int16_t s_wave_outer[][2] = {
    {24, 73},  {25, 73},  {22, 74},  {23, 74},  {21, 75},  {22, 75},  {20, 76},  {21, 76},
    {19, 77},  {20, 77},  {18, 78},  {19, 78},  {17, 79},  {18, 79},  {17, 80},  {16, 81},
    {15, 82},  {16, 82},  {15, 83},  {14, 84},  {15, 84},  {14, 85},  {13, 86},  {14, 86},
    {13, 87},  {12, 88},  {13, 88},  {12, 89},  {12, 90},  {11, 91},  {12, 91},  {11, 92},
    {11, 93},  {11, 94},  {11, 95},  {10, 96},  {11, 96},  {10, 97},  {10, 98},  {10, 99},
    {10, 100}, {10, 101}, {10, 102}, {10, 103}, {10, 104}, {10, 105}, {10, 106}, {10, 107},
    {10, 108}, {11, 109}, {11, 110}, {11, 111}, {11, 112}, {11, 113}, {12, 113}, {12, 114},
    {12, 115}, {12, 116}, {13, 116}, {13, 117}, {13, 118}, {14, 119}, {14, 120}, {15, 121},
    {15, 122}, {16, 122}, {16, 123}, {17, 124}, {17, 125}, {18, 125}, {18, 126}, {19, 126},
    {19, 127}, {20, 127}, {20, 128}, {21, 128}, {21, 129}, {22, 129}, {22, 130}, {23, 130},
    {24, 131}, {25, 131},
};
#define WAVE_OUTER_COUNT (sizeof(s_wave_outer) / sizeof(s_wave_outer[0]))

static void render_listening_waves(uint16_t* dst, uint32_t elapsed_ms, float mic_level,
                                   uint16_t bg_565) {
    uint32_t dr = (bg_565 >> 11) & 0x1f;
    uint32_t dg = (bg_565 >> 5) & 0x3f;
    uint32_t db = bg_565 & 0x1f;

    uint16_t wave_color;
    if (dr + dg + db > 70) {
        wave_color = (uint16_t)(((0 >> 3) << 11) | ((125 >> 2) << 5) | (220 >> 3));
    } else {
        wave_color = (uint16_t)(((0 >> 3) << 11) | ((229 >> 2) << 5) | (255 >> 3));
    }

    float a_inner = 0.0f, a_mid = 0.0f, a_outer = 0.0f;

    if (mic_level < 0.06f) {
        float phase = fmodf((float)elapsed_ms * 0.0035f, 3.0f);
        float d_in = fabsf(phase - 2.0f);
        float d_mid = fabsf(phase - 1.0f);
        float d_out = fabsf(phase - 0.0f);

        a_inner = (d_in < 1.0f) ? (1.0f - d_in) * 130.0f : 30.0f;
        if (a_inner < 30.0f)
            a_inner = 30.0f;
        a_mid = (d_mid < 1.0f) ? (1.0f - d_mid) * 105.0f : 0.0f;
        a_outer = (d_out < 1.0f) ? (1.0f - d_out) * 85.0f : 0.0f;
    } else {
        a_inner = 175.0f + mic_level * 80.0f;
        if (a_inner > 255.0f)
            a_inner = 255.0f;

        if (mic_level > 0.10f) {
            float norm_mid = (mic_level - 0.10f) / 0.35f;
            if (norm_mid > 1.0f)
                norm_mid = 1.0f;
            float pulse = 0.82f + 0.18f * sinf((float)elapsed_ms * 0.022f);
            a_mid = (135.0f + norm_mid * 120.0f) * pulse;
            if (a_mid > 255.0f)
                a_mid = 255.0f;
        }

        if (mic_level > 0.28f) {
            float norm_out = (mic_level - 0.28f) / 0.45f;
            if (norm_out > 1.0f)
                norm_out = 1.0f;
            float pulse = 0.78f + 0.22f * sinf((float)elapsed_ms * 0.026f + 1.2f);
            a_outer = (115.0f + norm_out * 140.0f) * pulse;
            if (a_outer > 255.0f)
                a_outer = 255.0f;
        }
    }

    uint8_t alpha_inner = (uint8_t)(a_inner > 255.0f ? 255 : (a_inner < 0.0f ? 0 : a_inner));
    uint8_t alpha_mid = (uint8_t)(a_mid > 255.0f ? 255 : (a_mid < 0.0f ? 0 : a_mid));
    uint8_t alpha_outer = (uint8_t)(a_outer > 255.0f ? 255 : (a_outer < 0.0f ? 0 : a_outer));

    if (alpha_inner > 0) {
        for (size_t i = 0; i < WAVE_INNER_COUNT; ++i) {
            int x = s_wave_inner[i][0];
            int y = s_wave_inner[i][1];
            if (x >= 0 && x < W && y >= 0 && y < H) {
                dst[y * W + x] = blend_rgb565(dst[y * W + x], wave_color, alpha_inner);
            }
        }
    }
    if (alpha_mid > 0) {
        for (size_t i = 0; i < WAVE_MIDDLE_COUNT; ++i) {
            int x = s_wave_middle[i][0];
            int y = s_wave_middle[i][1];
            if (x >= 0 && x < W && y >= 0 && y < H) {
                dst[y * W + x] = blend_rgb565(dst[y * W + x], wave_color, alpha_mid);
            }
        }
    }
    if (alpha_outer > 0) {
        for (size_t i = 0; i < WAVE_OUTER_COUNT; ++i) {
            int x = s_wave_outer[i][0];
            int y = s_wave_outer[i][1];
            if (x >= 0 && x < W && y >= 0 && y < H) {
                dst[y * W + x] = blend_rgb565(dst[y * W + x], wave_color, alpha_outer);
            }
        }
    }
}

void anime_pixel_render_240x320(uint16_t* dst, anime_mode_t mode, uint32_t elapsed_ms, float level,
                                float happy, bool wink, bool blink, uint32_t bg_rgb) {
    if (!dst)
        return;

    const uint16_t bg_565 = rgb565(bg_rgb);

    if (!s_decomp_psram) {
        for (int i = 0; i < PIXELS; ++i) {
            dst[i] = bg_565;
        }
        return;
    }

    if (level < 0.0f)
        level = 0.0f;
    else if (level > 1.0f)
        level = 1.0f;

    anime_frame_t frame = ANIME_FRAME_IDLE;

    switch (mode) {
        case ANIME_MODE_LISTENING: {
            // Hand on headset, character completely stationary
            frame = ANIME_FRAME_LISTEN;
            break;
        }

        case ANIME_MODE_THINKING:
        case ANIME_MODE_BOOT:
            frame = ANIME_FRAME_THINK;
            break;

        case ANIME_MODE_SPEAKING: {
            // Per user instruction: maintain stable stationary base avatar (IDLE) during speaking
            frame = ANIME_FRAME_IDLE;
            break;
        }

        case ANIME_MODE_ERROR:
            frame = ANIME_FRAME_SURPRISED;
            break;

        case ANIME_MODE_IDLE:
        default: {
            if (happy > 0.3f) {
                frame = ANIME_FRAME_SMILE;
            } else if (wink) {
                frame = ANIME_FRAME_WINK;
            } else if (blink) {
                frame = ANIME_FRAME_BLINK;
            } else {
                frame = ANIME_FRAME_IDLE;
            }
            break;
        }
    }

    // Direct, ultra-fast 16-bit copy of authentic RGB565 frame (no alpha degrading hair edges)
    memcpy(dst, s_frames_rgb[frame], PIXELS * sizeof(uint16_t));

    // In listening mode, if blinking, overlay the closed eyes patch (keeping hand on headset stationary)
    if (mode == ANIME_MODE_LISTENING && blink) {
        anime_pixel_patch_closed_eyes(dst);
    }

    // In listening mode, render dynamic cyan wave arcs near the headset
    if (mode == ANIME_MODE_LISTENING) {
        render_listening_waves(dst, elapsed_ms, level, bg_565);
    }
}

void anime_pixel_patch_closed_eyes(uint16_t* dst) {
    if (!dst) {
        return;
    }
    for (int y = 0; y < ANIME_EYE_H; ++y) {
        int dst_idx_l = (ANIME_EYE_LEFT_Y + y) * ANIME_IMG_W + ANIME_EYE_LEFT_X;
        int src_idx = y * ANIME_EYE_W;
        memcpy(&dst[dst_idx_l], &g_anime_eye_listen_left[src_idx], ANIME_EYE_W * sizeof(uint16_t));

        int dst_idx_r = (ANIME_EYE_RIGHT_Y + y) * ANIME_IMG_W + ANIME_EYE_RIGHT_X;
        memcpy(&dst[dst_idx_r], &g_anime_eye_listen_right[src_idx], ANIME_EYE_W * sizeof(uint16_t));
    }
}

void anime_pixel_restore_open_eyes(uint16_t* dst) {
    if (!dst || !s_frames_rgb[ANIME_FRAME_IDLE]) {
        return;
    }
    const uint16_t* idle_src = s_frames_rgb[ANIME_FRAME_IDLE];
    for (int y = 0; y < ANIME_EYE_H; ++y) {
        int dst_idx_l = (ANIME_EYE_LEFT_Y + y) * ANIME_IMG_W + ANIME_EYE_LEFT_X;
        memcpy(&dst[dst_idx_l], &idle_src[dst_idx_l], ANIME_EYE_W * sizeof(uint16_t));

        int dst_idx_r = (ANIME_EYE_RIGHT_Y + y) * ANIME_IMG_W + ANIME_EYE_RIGHT_X;
        memcpy(&dst[dst_idx_r], &idle_src[dst_idx_r], ANIME_EYE_W * sizeof(uint16_t));
    }
}

void anime_pixel_patch_listening_eyes(uint16_t* dst) {
    anime_pixel_patch_closed_eyes(dst);
}

void anime_pixel_patch_mouth(uint16_t* dst, anime_mouth_t mouth) {
    if (!dst || mouth >= ANIME_MOUTH_COUNT) {
        return;
    }
    const uint16_t* src = g_anime_mouth[mouth];
    for (int y = 0; y < ANIME_MOUTH_H; ++y) {
        int dst_idx = (ANIME_MOUTH_Y + y) * ANIME_IMG_W + ANIME_MOUTH_X;
        int src_idx = y * ANIME_MOUTH_W;
        memcpy(&dst[dst_idx], &src[src_idx], ANIME_MOUTH_W * sizeof(uint16_t));
    }
}

/* Real-time PCM audio playback envelope tracking */
static volatile uint16_t s_playback_energy = 0;
static volatile uint32_t s_last_playback_tick = 0;
static float s_smoothed_speech_level = 0.0f;

void anime_feed_playback_pcm(const int16_t* samples, int count) {
    if (!samples || count <= 0) {
        return;
    }
    const int step = count > 128 ? count / 128 : 1;
    int64_t sum_sq = 0;
    int n = 0;
    int16_t peak = 0;
    for (int i = 0; i < count; i += step) {
        int32_t v = samples[i];
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = (int16_t)v;
        sum_sq += (int64_t)v * v;
        n++;
    }
    uint32_t rms = n ? (uint32_t)sqrtf((float)(sum_sq / n)) : 0;
    // Combine RMS (70%) with peak (30%) for responsive vowel tracking
    uint32_t energy = (rms * 70 + (uint32_t)peak * 30) / 100;
    if (energy > 32767)
        energy = 32767;

    s_playback_energy = (uint16_t)energy;
    s_last_playback_tick = (uint32_t)(esp_timer_get_time() / 1000);
}

float anime_get_audio_playback_level(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t last_ms = s_last_playback_tick;
    uint32_t age_ms = (now_ms >= last_ms) ? (now_ms - last_ms) : 0;

    float raw_norm = 0.0f;
    // Audio chunks arrive every ~20-50ms. If written within last 120ms, audio is actively playing!
    if (age_ms < 120) {
        uint16_t energy = s_playback_energy;
        if (energy > 200) {
            raw_norm = (float)(energy - 200) / 5500.0f;
            if (raw_norm > 1.0f)
                raw_norm = 1.0f;
        }
    }

    // Fast attack (open mouth quickly on syllables), smooth decay (close cleanly between words)
    if (raw_norm > s_smoothed_speech_level) {
        s_smoothed_speech_level = s_smoothed_speech_level * 0.30f + raw_norm * 0.70f;
    } else {
        s_smoothed_speech_level = s_smoothed_speech_level * 0.65f + raw_norm * 0.35f;
        if (s_smoothed_speech_level < 0.04f || age_ms >= 120) {
            s_smoothed_speech_level = 0.0f;
        }
    }
    return s_smoothed_speech_level;
}

/*
 * anime_pixel.h - Fullscreen 240x320 Mascot Renderer for DFRobot K10
 * Based on Hermes Minji architecture adapted for 240x320 LCD.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "anime_data.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANIME_PX_W ANIME_IMG_W /* 240 */
#define ANIME_PX_H ANIME_IMG_H /* 320 */

typedef enum {
    ANIME_MODE_BOOT = 0,
    ANIME_MODE_IDLE,
    ANIME_MODE_LISTENING,
    ANIME_MODE_THINKING,
    ANIME_MODE_SPEAKING,
    ANIME_MODE_ERROR,
    ANIME_MODE_OFF,
    ANIME_MODE_COUNT
} anime_mode_t;

/*
 * Allocate PSRAM and decompress all 8 anime frames into PSRAM using ROM tinfl.
 * Must be called under display / application setup.
 * Returns true on success.
 */
bool anime_pixel_init(void);

/*
 * Free PSRAM buffers allocated for anime frames.
 */
void anime_pixel_deinit(void);

/*
 * Check if anime frames are loaded in PSRAM.
 */
bool anime_pixel_is_ready(void);

/*
 * Render K10 Anime Customer Service Mascot directly to destination RGB565 canvas (240x320).
 * Character is completely stationary (no lateral movement or sway).
 */
void anime_pixel_render_240x320(uint16_t* dst, anime_mode_t mode, uint32_t elapsed_ms, float level,
                                float happy, bool wink, uint32_t bg_rgb);

/*
 * Patch one of the 3 mouth shapes (closed, half, open) into destination 240x320 RGB565 buffer.
 * Only updates the 30x24 mouth ROI at (137, 152).
 */
void anime_pixel_patch_mouth(uint16_t* dst, anime_mouth_t mouth);

/*
 * Audio playback energy tracking for real-time PCM lip-sync.
 */
float anime_get_audio_playback_level(void);
void anime_feed_playback_pcm(const int16_t* samples, int count);

#ifdef __cplusplus
}
#endif

/*
 * anime_data.h - Compressed Fullscreen 240x320 Frames for K10 Anime Mascot
 * Generated from 3x3 Anime Customer Support Expressions Grid
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANIME_IMG_W 240
#define ANIME_IMG_H 320
#define ANIME_IMG_PIXELS (ANIME_IMG_W * ANIME_IMG_H)
#define ANIME_RAW_FRAME_SIZE 153600

typedef enum {
    ANIME_FRAME_IDLE,
    ANIME_FRAME_LISTEN,
    ANIME_FRAME_SPEAK,
    ANIME_FRAME_THINK,
    ANIME_FRAME_BLINK,
    ANIME_FRAME_HAPPY,
    ANIME_FRAME_WINK,
    ANIME_FRAME_SURPRISED,
    ANIME_FRAME_SMILE,
    ANIME_FRAME_COUNT
} anime_frame_t;

extern const uint8_t* const g_anime_comp[ANIME_FRAME_COUNT];
extern const size_t g_anime_comp_size[ANIME_FRAME_COUNT];

/* Mouth ROI for speech lip-sync */
#define ANIME_MOUTH_X 137
#define ANIME_MOUTH_Y 152
#define ANIME_MOUTH_W 30
#define ANIME_MOUTH_H 24
#define ANIME_MOUTH_PIXELS (ANIME_MOUTH_W * ANIME_MOUTH_H)

typedef enum {
    ANIME_MOUTH_CLOSED = 0,
    ANIME_MOUTH_HALF = 1,
    ANIME_MOUTH_OPEN = 2,
    ANIME_MOUTH_COUNT = 3
} anime_mouth_t;

extern const uint16_t g_anime_mouth[ANIME_MOUTH_COUNT][ANIME_MOUTH_PIXELS];

#ifdef __cplusplus
}
#endif

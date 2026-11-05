#!/usr/bin/env python3
"""
generate_k10_anime_fullscreen.py
Generates compressed 240x320 anime frames for DFRobot K10 from the 3x3 Anime Customer Support Expressions Grid.
9 authentic poses:
1. Idle (ANIME_FRAME_IDLE)
2. Listening (ANIME_FRAME_LISTEN)
3. Speaking (ANIME_FRAME_SPEAK)
4. Thinking (ANIME_FRAME_THINK)
5. Blink (ANIME_FRAME_BLINK)
6. Happy (ANIME_FRAME_HAPPY)
7. Wink (ANIME_FRAME_WINK)
8. Surprised (ANIME_FRAME_SURPRISED)
9. Smile (ANIME_FRAME_SMILE)

Pure RGB565 compressed with standard zlib/deflate, decompressed into PSRAM via ROM tinfl.
"""

import os
import sys
import zlib
from PIL import Image

W = 240
H = 320
RAW_FRAME_SIZE = W * H * 2  # 153600 bytes RGB565

# 3x3 Grid configuration
ROW_CROPS = [(3, 483), (485, 965), (968, 1448)]
COL_CROPS = [(0, 360), (362, 722), (723, 1083)]

POSES = [
    ("idle", "ANIME_FRAME_IDLE", 0, 0),
    ("listening", "ANIME_FRAME_LISTEN", 0, 1),
    ("speaking", "ANIME_FRAME_SPEAK", 0, 2),
    ("thinking", "ANIME_FRAME_THINK", 1, 0),
    ("blink", "ANIME_FRAME_BLINK", 1, 1),
    ("happy", "ANIME_FRAME_HAPPY", 1, 2),
    ("wink", "ANIME_FRAME_WINK", 2, 0),
    ("surprised", "ANIME_FRAME_SURPRISED", 2, 1),
    ("smile", "ANIME_FRAME_SMILE", 2, 2),
]

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(script_dir, ".."))
    master_image_path = os.path.join(root, "assets", "Anime Customer Support Expressions Grid.png")
    assets_dir = os.path.join(root, "assets", "k10_anime")
    header_path = os.path.join(root, "main", "display", "anime_data.h")
    source_path = os.path.join(root, "main", "display", "anime_data.c")

    if not os.path.exists(master_image_path):
        raise FileNotFoundError(f"Missing master grid asset: {master_image_path}")

    os.makedirs(assets_dir, exist_ok=True)
    img = Image.open(master_image_path)
    print(f"Reading master grid from: {master_image_path} ({img.size[0]}x{img.size[1]})")

    compressed_data = []

    for idx, (pose_name, enum_val, r, c) in enumerate(POSES, 1):
        x0, x1 = COL_CROPS[c]
        y0, y1 = ROW_CROPS[r]
        cropped = img.crop((x0, y0, x1, y1))
        resized = cropped.resize((W, H), Image.Resampling.LANCZOS)

        # Save individual PNG assets: both named and numbered
        out_png = os.path.join(assets_dir, f"{pose_name}.png")
        num_png = os.path.join(assets_dir, f"{idx}_{pose_name}.png")
        resized.save(out_png)
        resized.save(num_png)

        # Convert to pure RGB565 little-endian
        pix = resized.load()
        raw = bytearray(RAW_FRAME_SIZE)
        raw_idx = 0
        for y in range(H):
            for x in range(W):
                r_c, g_c, b_c = pix[x, y][:3]
                c565 = ((r_c & 0xF8) << 8) | ((g_c & 0xFC) << 3) | (b_c >> 3)
                # Little-endian for ESP32:
                raw[raw_idx] = c565 & 0xFF
                raw[raw_idx + 1] = (c565 >> 8) & 0xFF
                raw_idx += 2

        assert len(raw) == RAW_FRAME_SIZE, f"Bad raw size {len(raw)}"
        comp = zlib.compress(bytes(raw), level=9)
        decomp = zlib.decompress(comp)
        assert decomp == bytes(raw), f"Decompression verification failed for {pose_name}!"
        compressed_data.append((pose_name, enum_val, comp))
        print(f"  Pose {idx}. {pose_name:12s} ({enum_val}): raw={len(raw)} -> comp={len(comp)} bytes ({len(comp)/1024:.1f} KB)")

    total_comp_size = sum(len(c) for _, _, c in compressed_data)
    print(f"Total compressed flash footprint: {total_comp_size} bytes ({total_comp_size/1024:.1f} KB)")

    # 3 Mouth Shapes for speech lip-sync (stationary base pose, mouth patch only)
    mouth_x, mouth_y, mouth_w, mouth_h = 137, 152, 30, 24
    idle_img = Image.open(os.path.join(assets_dir, "idle.png"))
    spk_img = Image.open(os.path.join(assets_dir, "speaking.png"))

    m_closed = idle_img.crop((mouth_x, mouth_y, mouth_x + mouth_w, mouth_y + mouth_h))
    m_open_raw = spk_img.crop((mouth_x, mouth_y, mouth_x + mouth_w, mouth_y + mouth_h))

    # Feather mask for seamless outer edge matching idle.png skin tone
    mask = Image.new("L", (mouth_w, mouth_h), 0)
    for y in range(mouth_h):
        for x in range(mouth_w):
            dx = min(x, mouth_w - 1 - x)
            dy = min(y, mouth_h - 1 - y)
            d = min(dx, dy)
            if d == 0:
                val = 0
            elif d == 1:
                val = 130
            else:
                val = 255
            mask.putpixel((x, y), val)

    m_open = Image.composite(m_open_raw, m_closed, mask)
    m_half = Image.blend(m_closed, m_open, 0.45)

    m_closed.save(os.path.join(assets_dir, "mouth_0_closed.png"))
    m_half.save(os.path.join(assets_dir, "mouth_1_half.png"))
    m_open.save(os.path.join(assets_dir, "mouth_2_open.png"))

    mouth_names = ["closed", "half", "open"]
    mouth_shapes = [m_closed, m_half, m_open]
    mouth_rgb565 = []
    for m in mouth_shapes:
        mpix = m.load()
        mraw = []
        for y in range(mouth_h):
            for x in range(mouth_w):
                r_c, g_c, b_c = mpix[x, y][:3]
                c565 = ((r_c & 0xF8) << 8) | ((g_c & 0xFC) << 3) | (b_c >> 3)
                mraw.append(c565)
        mouth_rgb565.append(mraw)
    print(f"Generated 3 lip-sync mouth shapes ({mouth_w}x{mouth_h} = {mouth_w*mouth_h} px each)")

    # 1. Generate anime_data.h
    with open(header_path, "w", encoding="utf-8") as f:
        f.write("/*\n")
        f.write(" * anime_data.h - Compressed Fullscreen 240x320 Frames for K10 Anime Mascot\n")
        f.write(" * Generated from 3x3 Anime Customer Support Expressions Grid\n")
        f.write(" */\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <stddef.h>\n\n")
        f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        f.write(f"#define ANIME_IMG_W {W}\n")
        f.write(f"#define ANIME_IMG_H {H}\n")
        f.write(f"#define ANIME_IMG_PIXELS (ANIME_IMG_W * ANIME_IMG_H)\n")
        f.write(f"#define ANIME_RAW_FRAME_SIZE {RAW_FRAME_SIZE}\n\n")
        f.write("typedef enum {\n")
        for pose_name, enum_val, _, _ in POSES:
            f.write(f"    {enum_val},\n")
        f.write("    ANIME_FRAME_COUNT\n")
        f.write("} anime_frame_t;\n\n")
        f.write("extern const uint8_t* const g_anime_comp[ANIME_FRAME_COUNT];\n")
        f.write("extern const size_t g_anime_comp_size[ANIME_FRAME_COUNT];\n\n")
        f.write("/* Mouth ROI for speech lip-sync */\n")
        f.write(f"#define ANIME_MOUTH_X {mouth_x}\n")
        f.write(f"#define ANIME_MOUTH_Y {mouth_y}\n")
        f.write(f"#define ANIME_MOUTH_W {mouth_w}\n")
        f.write(f"#define ANIME_MOUTH_H {mouth_h}\n")
        f.write(f"#define ANIME_MOUTH_PIXELS (ANIME_MOUTH_W * ANIME_MOUTH_H)\n\n")
        f.write("typedef enum {\n")
        f.write("    ANIME_MOUTH_CLOSED = 0,\n")
        f.write("    ANIME_MOUTH_HALF = 1,\n")
        f.write("    ANIME_MOUTH_OPEN = 2,\n")
        f.write("    ANIME_MOUTH_COUNT = 3\n")
        f.write("} anime_mouth_t;\n\n")
        f.write("extern const uint16_t g_anime_mouth[ANIME_MOUTH_COUNT][ANIME_MOUTH_PIXELS];\n\n")
        f.write("#ifdef __cplusplus\n}\n#endif\n")

    print(f"Wrote header: {header_path}")

    # 2. Generate anime_data.c
    with open(source_path, "w", encoding="utf-8") as f:
        f.write("/*\n")
        f.write(" * anime_data.c - Compressed Fullscreen 240x320 Frames for K10 Anime Mascot\n")
        f.write(" * Generated from 3x3 Anime Customer Support Expressions Grid\n")
        f.write(" */\n\n")
        f.write('#include "anime_data.h"\n\n')

        for pose_name, enum_val, comp in compressed_data:
            f.write(f"static const uint8_t s_anime_{pose_name}_zlib[{len(comp)}] = {{\n")
            for i in range(0, len(comp), 16):
                chunk = comp[i:i+16]
                f.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
            f.write("};\n\n")

        f.write("const uint8_t* const g_anime_comp[ANIME_FRAME_COUNT] = {\n")
        for pose_name, enum_val, _ in compressed_data:
            f.write(f"    s_anime_{pose_name}_zlib,\n")
        f.write("};\n\n")

        f.write("const size_t g_anime_comp_size[ANIME_FRAME_COUNT] = {\n")
        for pose_name, enum_val, comp in compressed_data:
            f.write(f"    sizeof(s_anime_{pose_name}_zlib),\n")
        f.write("};\n\n")

        f.write("/* 3 Lip-sync mouth shapes in RGB565 */\n")
        f.write("const uint16_t g_anime_mouth[ANIME_MOUTH_COUNT][ANIME_MOUTH_PIXELS] = {\n")
        for idx, (m_name, m_vals) in enumerate(zip(mouth_names, mouth_rgb565)):
            f.write(f"    /* {idx}: {m_name.capitalize()} */\n    {{\n")
            for i in range(0, len(m_vals), 8):
                chunk = m_vals[i:i+8]
                f.write("        " + ", ".join(f"0x{v:04x}" for v in chunk) + ",\n")
            f.write("    },\n")
        f.write("};\n")

    print(f"Wrote source: {source_path}")

if __name__ == "__main__":
    main()

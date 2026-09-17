#pragma once

#include <cJSON.h>
#include <string>

// ESP32-S3 + external simplex I2S + optional SPI LCD. Pin settings are
// independent of the flash/PSRAM configuration, which belongs to the image.
struct GeniusDeviceConfig {
    int mic_ws = 4, mic_bclk = 5, mic_din = 6;
    int spk_bclk = 15, spk_ws = 16, spk_dout = 7;
    int boot = 0, volume_up = 39, volume_down = 38, led = 48;
    int lcd_mosi = 47, lcd_clk = 21, lcd_dc = 40, lcd_rst = 45, lcd_cs = 41, lcd_bl = 42;
    int width = 128, height = 160, offset_x = 0, offset_y = 0;
    bool display = true, mirror_x = false, mirror_y = false, swap_xy = false;
    bool invert = false, bgr = false, backlight_invert = false;
    std::string ToJson() const;
    static bool Parse(const cJSON* json, GeniusDeviceConfig& config, std::string& error);
    bool Validate(std::string& error) const;
    static GeniusDeviceConfig Load();
    bool Save(std::string& error) const;
};

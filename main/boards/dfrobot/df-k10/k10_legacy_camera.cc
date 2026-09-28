#include "k10_legacy_camera.h"

#include "config.h"
#include "esp32_camera.h"

#include <esp_camera.h>
#include <esp_log.h>

#define TAG "K10LegacyCamera"

Camera* CreateK10LegacyCamera() {
    camera_config_t config = {};
    config.pin_pwdn = CAMERA_PIN_PWDN;
    config.pin_reset = CAMERA_PIN_RESET;
    config.pin_xclk = CAMERA_PIN_XCLK;
    // K10 already owns I2C port 1 for the TCA9555 and sensors.
    // esp32-camera can attach SCCB to that existing bus instead of trying
    // to create a second master bus on the same controller.
    config.pin_sccb_sda = -1;
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = 1;

    config.pin_d7 = CAMERA_PIN_D9;
    config.pin_d6 = CAMERA_PIN_D8;
    config.pin_d5 = CAMERA_PIN_D7;
    config.pin_d4 = CAMERA_PIN_D6;
    config.pin_d3 = CAMERA_PIN_D5;
    config.pin_d2 = CAMERA_PIN_D4;
    config.pin_d1 = CAMERA_PIN_D3;
    config.pin_d0 = CAMERA_PIN_D2;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_pclk = CAMERA_PIN_PCLK;

    // DFRobot's K10 camera stack defaults GC2145 XCLK to 10 MHz.
    // 20 MHz probes correctly but produces severe horizontal frame corruption
    // on this board with the esp32-camera DMA path.
    config.xclk_freq_hz = 10000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;

    // Match the DFRobot UNIHIKER K10 path that was physically verified
    // indoors and outdoors on this board.
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    ESP_LOGI(TAG, "Starting DFRobot-compatible esp32-camera backend: RGB565 QVGA");
    auto* camera = new Esp32Camera(config);
    // GC2145 RGB565 data is byte-swapped before XiaoZhi preview/JPEG encoding.
    camera->SetSwapBytes(true);
    return camera;
}

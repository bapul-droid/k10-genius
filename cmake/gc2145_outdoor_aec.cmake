# GC2145 outdoor exposure experiment.
# Patch the managed esp_cam_sensor SVGA exposure ladder after IDF resolves dependencies.
set(GC2145_SETTINGS "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_cam_sensor/sensors/gc2145/private_include/gc2145_dvp_8bit_20Minput_800x600_rgb565_be_20fps.h")

if(EXISTS "${GC2145_SETTINGS}")
    file(READ "${GC2145_SETTINGS}" GC2145_CONTENT)

    set(GC2145_OLD [=[
    {0x25, 0x01},
    {0x26, 0xac},
    {0x27, 0x05},
    {0x28, 0x04},
    {0x29, 0x05},
    {0x2a, 0x04},
    {0x2b, 0x05},
    {0x2c, 0x04},
    {0x2d, 0x05},
    {0x2e, 0x04},
]=])

    set(GC2145_NEW [=[
    /* K10 outdoor AEC: allow shorter exposure and restore progressive 50 Hz ladder */
    {0x25, 0x00},
    {0x26, 0xfa},
    {0x27, 0x04},
    {0x28, 0xe2},
    {0x29, 0x05},
    {0x2a, 0xdc},
    {0x2b, 0x06},
    {0x2c, 0xd6},
    {0x2d, 0x0b},
    {0x2e, 0xb8},
]=])

    string(FIND "${GC2145_CONTENT}" "${GC2145_OLD}" GC2145_OLD_POS)
    string(FIND "${GC2145_CONTENT}" "K10 outdoor AEC: allow shorter exposure" GC2145_PATCHED_POS)

    if(NOT GC2145_OLD_POS EQUAL -1)
        string(REPLACE "${GC2145_OLD}" "${GC2145_NEW}" GC2145_CONTENT "${GC2145_CONTENT}")
        file(WRITE "${GC2145_SETTINGS}" "${GC2145_CONTENT}")
        message(STATUS "K10 GC2145 outdoor exposure ladder patch applied")
    elseif(NOT GC2145_PATCHED_POS EQUAL -1)
        message(STATUS "K10 GC2145 outdoor AEC patch already applied")
    else()
        message(FATAL_ERROR "GC2145 settings changed upstream; refusing unsafe outdoor AEC patch")
    endif()
else()
    message(FATAL_ERROR "GC2145 settings file not found: ${GC2145_SETTINGS}")
endif()

# Stage 2: use the proven GC2145 SVGA AEC profile used by Linux camera drivers.
# Espressif's table exposes no runtime V4L2 AEC controls, so tune the sensor init table itself.
file(READ "${GC2145_SETTINGS}" GC2145_CONTENT)

set(GC2145_AEC_OLD [=[
    {0xfe, 0x01},
    {0x01, 0x04},
    {0x02, 0x60},
    {0x03, 0x02},
    {0x04, 0x48},
    {0x05, 0x18},
    {0x06, 0x50},
    {0x07, 0x10},
    {0x08, 0x38},
    {0x0a, 0x80},
    {0x21, 0x04},
    {0xfe, 0x00},
]=])

set(GC2145_AEC_NEW [=[
    {0xfe, 0x01},
    /* K10 outdoor AEC stage 2: GC2145 SVGA daylight profile */
    {0x01, 0x04},
    {0x02, 0x60},
    {0x03, 0x02},
    {0x04, 0x48},
    {0x05, 0x18},
    {0x06, 0x4c},
    {0x07, 0x14},
    {0x08, 0x36},
    {0x0a, 0xc0},
    {0x21, 0x14},
    {0xfe, 0x00},
]=])

string(FIND "${GC2145_CONTENT}" "${GC2145_AEC_OLD}" GC2145_AEC_OLD_POS)
string(FIND "${GC2145_CONTENT}" "K10 outdoor AEC stage 2" GC2145_AEC_PATCHED_POS)

if(NOT GC2145_AEC_OLD_POS EQUAL -1)
    string(REPLACE "${GC2145_AEC_OLD}" "${GC2145_AEC_NEW}" GC2145_CONTENT "${GC2145_CONTENT}")
    file(WRITE "${GC2145_SETTINGS}" "${GC2145_CONTENT}")
    message(STATUS "K10 GC2145 outdoor AEC stage 2 applied")
elseif(NOT GC2145_AEC_PATCHED_POS EQUAL -1)
    message(STATUS "K10 GC2145 outdoor AEC stage 2 already applied")
else()
    message(FATAL_ERROR "GC2145 SVGA AEC block changed upstream; refusing unsafe stage 2 patch")
endif()


# Stage 4 diagnostic: log the GC2145's live exposure and gain after each Capture().
# P0:03/04 are live exposure; P0:B1/B2 are analog/digital gain readbacks.
set(GC2145_DRIVER "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_cam_sensor/sensors/gc2145/gc2145.c")
if(EXISTS "${GC2145_DRIVER}")
    file(READ "${GC2145_DRIVER}" GC2145_DRIVER_CONTENT)
    set(GC2145_DIAG_NEEDLE [=[
static esp_err_t gc2145_set_stream(esp_cam_sensor_device_t *dev, int enable)
]=])
    set(GC2145_DIAG_CODE [=[
static void gc2145_log_live_exposure(esp_cam_sensor_device_t *dev)
{
    uint8_t exp_hi = 0, exp_lo = 0, again = 0, dgain = 0;
    esp_err_t ret = gc2145_select_page(dev, 0x00);
    ret |= gc2145_read(dev->sccb_handle, 0x03, &exp_hi);
    ret |= gc2145_read(dev->sccb_handle, 0x04, &exp_lo);
    ret |= gc2145_read(dev->sccb_handle, 0xb1, &again);
    ret |= gc2145_read(dev->sccb_handle, 0xb2, &dgain);
    if (ret == ESP_OK) {
        uint16_t exposure = ((uint16_t)(exp_hi & 0x1f) << 8) | exp_lo;
        ESP_LOGI(TAG, "K10 LIVE EXP=%u (0x%04x) AGAIN=0x%02x DGAIN=0x%02x",
                 exposure, exposure, again, dgain);
    } else {
        ESP_LOGW(TAG, "K10 live exposure read failed: %s", esp_err_to_name(ret));
    }
}

static void gc2145_exposure_diag_task(void *arg)
{
    esp_cam_sensor_device_t *dev = (esp_cam_sensor_device_t *)arg;
    while (true) {
        gc2145_log_live_exposure(dev);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static esp_err_t gc2145_set_stream(esp_cam_sensor_device_t *dev, int enable)
]=])

    set(GC2145_STREAM_OLD [=[
    if (ret == ESP_OK) {
        dev->stream_status = enable;
    }
    ESP_LOGD(TAG, "Stream=%d", enable);
]=])
    set(GC2145_STREAM_NEW [=[
    if (ret == ESP_OK) {
        dev->stream_status = enable;
        if (enable) {
            static bool diag_started = false;
            if (!diag_started) {
                diag_started = true;
                xTaskCreate(gc2145_exposure_diag_task, "gc2145_exp_diag", 3072, dev, 3, NULL);
            }
        }
    }
    ESP_LOGD(TAG, "Stream=%d", enable);
]=])
    string(FIND "${GC2145_DRIVER_CONTENT}" "K10 LIVE EXP=" GC2145_DIAG_DONE)
    string(FIND "${GC2145_DRIVER_CONTENT}" "${GC2145_DIAG_NEEDLE}" GC2145_DIAG_POS)
    if(GC2145_DIAG_DONE EQUAL -1 AND NOT GC2145_DIAG_POS EQUAL -1)
        string(REPLACE "${GC2145_DIAG_NEEDLE}" "${GC2145_DIAG_CODE}" GC2145_DRIVER_CONTENT "${GC2145_DRIVER_CONTENT}")
        string(FIND "${GC2145_DRIVER_CONTENT}" "${GC2145_STREAM_OLD}" GC2145_STREAM_POS)
        if(GC2145_STREAM_POS EQUAL -1)
            message(FATAL_ERROR "GC2145 stream block changed upstream; refusing incomplete diagnostic patch")
        endif()
        string(REPLACE "${GC2145_STREAM_OLD}" "${GC2145_STREAM_NEW}" GC2145_DRIVER_CONTENT "${GC2145_DRIVER_CONTENT}")
        file(WRITE "${GC2145_DRIVER}" "${GC2145_DRIVER_CONTENT}")
        message(STATUS "K10 GC2145 live exposure diagnostic helper applied")
    elseif(NOT GC2145_DIAG_DONE EQUAL -1)
        message(STATUS "K10 GC2145 live exposure diagnostic helper already applied")
    else()
        message(FATAL_ERROR "GC2145 driver changed upstream; refusing unsafe diagnostic patch")
    endif()
else()
    message(FATAL_ERROR "GC2145 driver not found: ${GC2145_DRIVER}")
endif()

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
        message(STATUS "K10 GC2145 outdoor AEC patch applied")
    elseif(NOT GC2145_PATCHED_POS EQUAL -1)
        message(STATUS "K10 GC2145 outdoor AEC patch already applied")
    else()
        message(FATAL_ERROR "GC2145 settings changed upstream; refusing unsafe outdoor AEC patch")
    endif()
else()
    message(FATAL_ERROR "GC2145 settings file not found: ${GC2145_SETTINGS}")
endif()

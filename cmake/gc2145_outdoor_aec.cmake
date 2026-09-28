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


# Stage 5: force manual minimum exposure/gain.
# Diagnostic only: if daylight is still white, AEC/AGC is not the root cause.
file(READ "${GC2145_SETTINGS}" GC2145_CONTENT)
set(GC2145_MANUAL_NEEDLE [=[
    {0x0a, 0xc0},
    {0x21, 0x14},
    {0xfe, 0x00},
};
]=])
set(GC2145_MANUAL_REPLACEMENT [=[
    {0x0a, 0xc0},
    {0x21, 0x14},
    {0xfe, 0x00},

    /* K10 diagnostic: disable AEC and force minimum exposure/gain */
    {0xb6, 0x00},
    {0x03, 0x00},
    {0x04, 0x01},
    {0xb0, 0x40},
    {0xb1, 0x20},
    {0xb2, 0x40},
};
]=])
string(FIND "${GC2145_CONTENT}" "K10 diagnostic: disable AEC and force minimum exposure/gain" GC2145_MANUAL_DONE)
string(FIND "${GC2145_CONTENT}" "${GC2145_MANUAL_NEEDLE}" GC2145_MANUAL_POS)
if(NOT GC2145_MANUAL_DONE EQUAL -1)
    message(STATUS "K10 GC2145 manual minimum exposure diagnostic already applied")
elseif(NOT GC2145_MANUAL_POS EQUAL -1)
    string(REPLACE "${GC2145_MANUAL_NEEDLE}" "${GC2145_MANUAL_REPLACEMENT}" GC2145_CONTENT "${GC2145_CONTENT}")
    file(WRITE "${GC2145_SETTINGS}" "${GC2145_CONTENT}")
    message(STATUS "K10 GC2145 manual minimum exposure diagnostic applied")
else()
    message(FATAL_ERROR "GC2145 final AEC block changed upstream; refusing unsafe manual exposure patch")
endif()

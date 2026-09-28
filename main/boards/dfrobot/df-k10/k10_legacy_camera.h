#pragma once

#include "camera.h"

// Build the K10 camera with the same esp32-camera backend family used by
// DFRobot's UNIHIKER firmware. Ownership is transferred to the caller.
Camera* CreateK10LegacyCamera();

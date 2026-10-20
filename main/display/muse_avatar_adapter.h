#ifndef MUSE_AVATAR_ADAPTER_H
#define MUSE_AVATAR_ADAPTER_H

#include "device_state.h"

enum class MuseAvatarState {
    Boot,
    Idle,
    Listening,
    Thinking,
    Speaking,
    Error,
};

MuseAvatarState MuseAvatarStateFromDeviceState(DeviceState state);

#endif  // MUSE_AVATAR_ADAPTER_H

#include "muse_avatar_adapter.h"

MuseAvatarState MuseAvatarStateFromDeviceState(DeviceState state) {
    switch (state) {
        case kDeviceStateStarting:
            return MuseAvatarState::Boot;
        case kDeviceStateConnecting:
            return MuseAvatarState::Thinking;
        case kDeviceStateIdle:
        case kDeviceStateWifiConfiguring:
        case kDeviceStateNotifying:
        case kDeviceStateUpgrading:
        case kDeviceStateActivating:
        case kDeviceStateAudioTesting:
            return MuseAvatarState::Idle;
        case kDeviceStateFatalError:
            return MuseAvatarState::Error;
        case kDeviceStateListening:
            return MuseAvatarState::Listening;
        case kDeviceStateSpeaking:
            return MuseAvatarState::Speaking;
        case kDeviceStateUnknown:
        default:
            return MuseAvatarState::Idle;
    }
}

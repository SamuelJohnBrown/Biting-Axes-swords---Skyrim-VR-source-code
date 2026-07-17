#pragma once

#include <cstdint>

// Minimal OpenVR 1.0.x controller-state layout for reading grip input from the
// game's IVRSystem pointer (CommonLib's openvr.h is forward-declaration only).
namespace OpenVRCompat
{
    inline constexpr std::uint32_t kInvalidDevice = 0xFFFFFFFFu;
    inline constexpr std::uint64_t kGripButtonMask = 1ull << 2;      // k_EButton_Grip
    inline constexpr std::uint64_t kTriggerButtonMask = 1ull << 33;  // k_EButton_SteamVR_Trigger

    struct ControllerAxis
    {
        float x;
        float y;
    };

    struct ControllerState
    {
        std::uint32_t unPacketNum;
        std::uint64_t ulButtonPressed;
        std::uint64_t ulButtonTouched;
        ControllerAxis rAxis[5];
    };

    // IVRSystem::GetControllerState vtable slot for OpenVR 1.0.x (Skyrim VR / SKSE proxy).
    inline constexpr std::size_t kGetControllerStateVtableIndex = 34;

    using GetControllerStateFn = bool (*)(void* self, std::uint32_t deviceIndex, ControllerState* state,
                                          std::uint32_t stateSize);

    inline bool GetControllerState(void* ivrSystem, std::uint32_t deviceIndex, ControllerState* outState)
    {
        if (!ivrSystem || !outState) {
            return false;
        }
        auto** vtable = *reinterpret_cast<void***>(ivrSystem);
        if (!vtable) {
            return false;
        }
        auto fn = reinterpret_cast<GetControllerStateFn>(vtable[kGetControllerStateVtableIndex]);
        if (!fn) {
            return false;
        }
        return fn(ivrSystem, deviceIndex, outState, static_cast<std::uint32_t>(sizeof(ControllerState)));
    }
}

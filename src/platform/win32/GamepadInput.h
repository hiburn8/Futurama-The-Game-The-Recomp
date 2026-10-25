// Gamepad input platform layer (Phase 3): PC XInput controllers and a keyboard fallback, delivered as the Xbox
// XINPUT_GAMEPAD-shaped state the ported pad logic (src/game/input/Pad.cpp) polls once per update.
//
// Mapping (the Xbox pad has six analog face buttons; PC XInput has four digital ones plus shoulders): A, B, X, Y ->
// 255 / 0; Black <- right shoulder, White <- left shoulder (the usual Xbox-to-360 layout); triggers, sticks and the
// eight digital bits (d-pad, Start, Back, stick clicks - the same bit values on both) pass through.
#pragma once

#include "core/Types.h"
#include "game/input/Pad.h"

namespace fut::platform {

// XINPUT_GAMEPAD as the PC XInput API reports it (layout-compatible, kept free of <windows.h>).
struct PcGamepad {
    u16 buttons = 0;              // d-pad 0x1-0x8, Start 0x10, Back 0x20, thumbs 0x40 / 0x80, LB 0x100, RB 0x200,
                                  // A 0x1000, B 0x2000, X 0x4000, Y 0x8000
    u8 leftTrigger = 0, rightTrigger = 0;
    s16 thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
};

input::GamepadState MapPcGamepad(const PcGamepad& pc);

class Window;

// Keyboard stand-in for a pad: WASD left stick, IJKL right stick, arrows d-pad, Space A, LeftCtrl X, E B, R Y,
// Q White, F Black, Shift / Z triggers, Enter Start, Escape Back.
input::GamepadState KeyboardGamepad(const Window& window);

class XInputPads {
public:
    // Reads every port; connections are tracked so the pads' device-open flag follows plug / unplug (an input system
    // without the App: the viewer's free camera).
    void Poll(input::InputSystem& system);
    // Reads every port; the connection changes are kept for TakeChanges (the App's XGetDeviceChanges service,
    // AppServices::deviceChanges: Input_HandleDeviceChanges 0x0009C1D0 opens / closes the pads). Port 0 never changes:
    // without a controller it is the keyboard (KeyboardGamepad), connected from App::Init on.
    void Poll();
    // XGetDeviceChanges: the ports inserted / removed since the last call; false when none.
    bool TakeChanges(u32& insertions, u32& removals);
    // The state of `port` from the last Poll, or nullptr when no controller is connected (= XInputGetState failed).
    const input::GamepadState* State(u32 port) const { return connected_[port] ? &states_[port] : nullptr; }
    // For InputSystem::onRumble: Xbox motor speeds 0..65535 go to the PC motors unchanged.
    void SetMotors(u32 port, u16 left, u16 right);

private:
    input::GamepadState states_[4];
    bool connected_[4] = {};
    u32 insertions_ = 0, removals_ = 0;
};

}  // namespace fut::platform

// Gamepad input platform layer on SDL3 (docs/analysis/sdl3_plan.md; replaced the XInput layer of src/platform/win32
// on 2026-11-05): controllers and a keyboard fallback, delivered as the Xbox XINPUT_GAMEPAD-shaped state the ported
// pad logic (src/game/input/Pad.cpp) polls once per update.
//
// Mapping (the Xbox pad has six analog face buttons; PC pads have four digital ones plus shoulders): A, B, X, Y ->
// 255 / 0; Black <- right shoulder, White <- left shoulder (the usual Xbox-to-360 layout); triggers, sticks and the
// eight digital bits (d-pad, Start, Back, stick clicks - the same bit values on both) pass through. An SDL gamepad's
// values become XInput's exactly (MapSdlGamepad: SDL's XInput driver reports the trigger byte b as b * 32767 / 255
// and a stick's Y as ~y; the inverse of both), so a recording made through SDL is the same as one made through
// XInput for the same hands.
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

// An SDL gamepad's state as SDL_GetGamepadButton / SDL_GetGamepadAxis give it (kept free of <SDL3/SDL.h>).
struct SdlGamepad {
    bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
    bool start = false, back = false, leftStick = false, rightStick = false;
    bool leftShoulder = false, rightShoulder = false;
    bool south = false, east = false, west = false, north = false;   // A, B, X, Y
    s16 leftTrigger = 0, rightTrigger = 0;             // 0..32767
    s16 leftX = 0, leftY = 0, rightX = 0, rightY = 0;  // -32768..32767, Y positive down
};

// The XInput state those values came from (exact: see above), then MapPcGamepad.
PcGamepad SdlToPcGamepad(const SdlGamepad& sdl);
inline input::GamepadState MapSdlGamepad(const SdlGamepad& sdl) { return MapPcGamepad(SdlToPcGamepad(sdl)); }

class Window;

// Keyboard stand-in for a pad: WASD left stick, IJKL right stick, arrows d-pad, Space A, LeftCtrl X, E B, R Y,
// Q White, F Black, Shift / Z triggers, Enter Start, Escape Back (physical keys; Enter with Alt held is not Start:
// that is the full-screen toggle).
input::GamepadState KeyboardGamepad(const Window& window);

// The controllers on ports 0-3: SDL gamepads opened as they are connected (a new one takes the first free port),
// closed when they go.
class Gamepads {
public:
    Gamepads();
    ~Gamepads();
    Gamepads(const Gamepads&) = delete;
    Gamepads& operator=(const Gamepads&) = delete;
    // Reads every port; connections are tracked so the pads' device-open flag follows plug / unplug (an input system
    // without the App: the viewer's free camera).
    void Poll(input::InputSystem& system);
    // Reads every port; the connection changes are kept for TakeChanges (the App's XGetDeviceChanges service,
    // AppServices::deviceChanges: Input_HandleDeviceChanges 0x0009C1D0 opens / closes the pads). Port 0 never changes:
    // without a controller it is the keyboard (KeyboardGamepad), connected from App::Init on.
    void Poll();
    // XGetDeviceChanges: the ports inserted / removed since the last call; false when none.
    bool TakeChanges(u32& insertions, u32& removals);
    // The state of `port` from the last Poll, or nullptr when no controller is connected.
    const input::GamepadState* State(u32 port) const { return connected_[port] ? &states_[port] : nullptr; }
    // For InputSystem::onRumble: Xbox motor speeds 0..65535 go to the PC motors unchanged (kept running until
    // changed, as XInput's: Poll refreshes SDL's timed effect).
    void SetMotors(u32 port, u16 left, u16 right);

private:
    struct Impl;
    Impl* impl_;
    input::GamepadState states_[4];
    bool connected_[4] = {};
    u32 insertions_ = 0, removals_ = 0;
    void Update(bool trackChanges, input::InputSystem* system);
};

}  // namespace fut::platform

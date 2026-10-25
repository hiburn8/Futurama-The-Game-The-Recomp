// Controller input, decompiled from default.xbe (0x0009B160-0x0009C4C0).
//
// Four pad objects (0x19C bytes, `g_pads` 0x002E7024) read the Xbox gamepad state once per update and map it onto
// 16 game "buttons" (analog 0..1, with a press threshold, the previous value, a one-shot latch and a disable counter)
// and two sticks with a dead zone. On PC the platform layer supplies the same XINPUT_GAMEPAD-shaped state (see
// GamepadState); everything above that is the original logic, verified bit-exactly against the original under
// emulation over 20,000 random frames (tests/game/PadTests.cpp).
#pragma once

#include "core/Types.h"

#include <array>
#include <functional>

namespace fut::input {

// Xbox XINPUT_GAMEPAD.
struct GamepadState {
    u16 buttons = 0;              // kDPadUp 0x01, kDPadDown 0x02, kDPadLeft 0x04, kDPadRight 0x08, kStart 0x10,
                                  // kBack 0x20, kLeftThumb 0x40, kRightThumb 0x80
    std::array<u8, 8> analog{};   // A, B, X, Y, Black, White, left trigger, right trigger (0..255)
    s16 thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
};

// Game button indices (orig: the order the poll fills them, 0x0009C460).
enum Button : u32 {
    kDPadLeft, kDPadRight, kDPadUp, kDPadDown,
    kButtonX, kButtonB, kButtonY, kButtonA,
    kLeftTrigger, kRightTrigger, kStart, kBack, kLeftThumb, kRightThumb, kWhite, kBlack,
    kButtonCount
};

struct ButtonState {          // orig: 16 bytes at pad +0x04 + i*0x10
    f32 threshold = 1.0f / 256.0f;
    f32 value = 0.0f;
    f32 previous = 0.0f;
    bool latched = false;     // set by Latch(); cleared when the value drops below the threshold
    u8 disabled = 0;          // counter; non-zero hides the button from the "unlatched" queries
};

struct StickState {           // orig: 0x18 bytes at pad +0x104 + i*0x18
    f32 deadZoneX = 0.15f, deadZoneY = 0.15f;
    f32 x = 0.0f, previousX = 0.0f, y = 0.0f, previousY = 0.0f;
};

class InputSystem;

class Pad {                   // orig: base vtable 0x002A1584 (ctor 0x0009B860), pad vtable 0x002A1590 (ctor 0x0009BFE0)
public:
    Pad(InputSystem& system, u32 port);

    // orig: 0x0009C460 (vtable slot 0). `state` is the result of XInputGetState, nullptr if it failed. Does nothing
    // and returns false while no device is open.
    bool Poll(const GamepadState* state);
    // orig: 0x0009BBE0: every value -> previous, value 0 (as after a disconnect).
    void Reset();

    // Queries (false / 0 for an index >= 16).
    bool IsDown(u32 b) const;                 // orig: 0x0009B5E0
    bool JustReleased(u32 b) const;           // orig: 0x0009B610
    bool JustPressed(u32 b) const;            // orig: 0x0009B650
    bool IsDownUnlatched(u32 b) const;        // orig: 0x0009B960 (also requires "not disabled")
    bool JustPressedUnlatched(u32 b) const;   // orig: 0x0009BA20
    f32 Value(u32 b) const;                   // orig: 0x0009BA60 (0 when latched or disabled)
    void Latch(u32 b);                        // orig: 0x0009B9A0
    bool SetDisabled(u32 b, u8 count);        // orig: 0x0009B9C0, returns whether it was disabled before
    void DecrementDisabled(u8 n);             // orig: 0x0009B9F0
    // orig: 0x0009B690 / 0x0009B6C0: dead-zone remapped stick axes. The value stays on the x87 stack in the original,
    // hence double. StickY's positive side is not clamped (the original compares it with -1): up to 1.3.
    double StickX(s32 stick) const;
    double StickY(s32 stick) const;
    // orig: 0x0009BB50: no active button and both sticks within 0.25 on both axes.
    bool IsIdle() const;
    // orig: 0x0009B740: values outside [0,1] are ignored; returns false when rumble is off and not forced.
    bool SetRumble(f32 left, f32 right, bool force);

    void SetDisconnected(bool d) { disconnected_ = d ? 1 : 0; }   // orig: 0x0009B810
    u32 Disconnected() const { return disconnected_; }            // orig: 0x0009B820
    void SetDeviceOpen(bool open) { deviceOpen_ = open; }         // orig: XInputOpen / XInputClose handle at +0x150
    bool DeviceOpen() const { return deviceOpen_; }
    u32 Port() const { return port_; }                            // orig: 0x0009C450

    std::array<ButtonState, kButtonCount> buttons;
    std::array<StickState, 2> sticks;
    f32 idleTime = 0.0f;                      // orig: +0x13C, seconds without input (wraps by 3600 above 7200)
    f32 rumbleLeft = 0.0f, rumbleRight = 0.0f;   // orig: +0x140 / +0x144
    bool rumbleDirty = true;                  // orig: +0x148
    u16 motorLeft = 0, motorRight = 0;        // orig: +0x196 / +0x198, last speeds sent
    u32 feedbackStatus = 0;                   // orig: +0x154 (XINPUT_FEEDBACK header; 997 = still sending)

private:
    void SetButton(u32 b, f32 v);             // orig: 0x0009B830
    void SendRumble();                        // orig: 0x0009C2B0

    InputSystem& system_;
    u32 disconnected_ = 1;                    // orig: +0x138
    u32 port_;                                // orig: +0x14C
    bool deviceOpen_ = false;
};

// Global input state (orig: globals 0x002D663C, 0x002E7024-0x002E7044, 0x002E79B0).
class InputSystem {
public:
    InputSystem();

    // orig: 0x0009BEA0. `readState(port)` returns the pad's XInputGetState result (nullptr = failed).
    void Update(f32 dt, const std::function<const GamepadState*(u32 port)>& readState);
    // orig: 0x0009BAD0
    void SetRumbleEnabled(bool enable, f32 delay);
    // orig: Input_HandleDeviceChanges 0x0009C1D0 (App vf28 before Update): XGetDeviceChanges(gamepad) as bit masks
    // (bit n = port n, `changes` returns false for none; unset = none); per port 0..3 a removal closes the device,
    // resets the pad and marks it disconnected, then an insertion (re)opens it (XInputOpen: the port's platform
    // layer always succeeds), resets it and marks it connected.
    void HandleDeviceChanges(const std::function<bool(u32& insertions, u32& removals)>& changes);

    std::array<Pad*, 4> pads{};               // orig: g_pads 0x002E7024 (also the update order: construction order)
    Pad* activePad = nullptr;                 // orig: 0x002E7044, the first pad that produced input
    bool activePadFound = false;              // orig: 0x002E7040
    f32 idleTime = 0.0f;                      // orig: 0x002E7038, seconds without input on any pad
    f32 rumbleTimer = 0.0f;                   // orig: 0x002E703C, delayed rumble enable
    bool rumbleEnabled = true;                // orig: 0x002D663C (1 in the image)
    bool vibrationOption = false;             // orig: 0x002E79B0 (options screen)
    // Called for every motor update (orig: XInputSetState).
    std::function<void(const Pad&)> onRumble;
};

// Menu auto-repeat (orig: 0x0009B160-0x0009B3D0, 0x52 bytes): for the active pad, fires a held button on the first
// frame and then every `period` frames; stick axes fire per direction the same way.
class InputRepeat {
public:
    explicit InputRepeat(u16 period);         // orig: 0x0009B380
    void Update(const InputSystem& system);   // orig: 0x0009B3D0
    void Reset();                             // orig: 0x0009B260

    bool Fired(u32 b) const { return buttons[b].fired; }   // orig: 0x0009B160
    // orig: 0x0009B170 / 0x0009B180 / 0x0009B190 / 0x0009B1A0
    bool StickLeft(u32 s) const { return sticks[s].negX; }
    bool StickRight(u32 s) const { return sticks[s].posX; }
    bool StickDown(u32 s) const { return sticks[s].negY; }
    bool StickUp(u32 s) const { return sticks[s].posY; }

    struct ButtonRepeat {
        u16 count = 0;
        bool fired = false;
    };
    struct StickRepeat {
        s16 countX = 0, countY = 0;
        bool negX = false, posX = false, negY = false, posY = false;
    };
    std::array<ButtonRepeat, kButtonCount> buttons;
    std::array<StickRepeat, 2> sticks;
    u16 period;

private:
    void UpdateButtons(const Pad& pad);       // orig: 0x0009B2A0
    void UpdateSticks(const Pad& pad);        // orig: 0x0009B300
    void UpdateAxis(s16& count, bool& neg, bool& pos, f32 value);   // orig: 0x0009B1B0
};

}  // namespace fut::input

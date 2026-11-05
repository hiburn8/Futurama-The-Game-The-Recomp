// Gamepad input platform layer on SDL3; see GamepadInput.h.
#include "platform/sdl/GamepadInput.h"

#include "platform/sdl/Window.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fut::platform {

input::GamepadState MapPcGamepad(const PcGamepad& pc) {
    input::GamepadState s;
    s.buttons = pc.buttons & 0x00FF;                  // d-pad, Start, Back, stick clicks: same bits on the Xbox
    auto analog = [&](u16 mask) { return static_cast<u8>((pc.buttons & mask) ? 255 : 0); };
    s.analog[0] = analog(0x1000);                     // A
    s.analog[1] = analog(0x2000);                     // B
    s.analog[2] = analog(0x4000);                     // X
    s.analog[3] = analog(0x8000);                     // Y
    s.analog[4] = analog(0x0200);                     // Black <- right shoulder
    s.analog[5] = analog(0x0100);                     // White <- left shoulder
    s.analog[6] = pc.leftTrigger;
    s.analog[7] = pc.rightTrigger;
    s.thumbLX = pc.thumbLX;
    s.thumbLY = pc.thumbLY;
    s.thumbRX = pc.thumbRX;
    s.thumbRY = pc.thumbRY;
    return s;
}

PcGamepad SdlToPcGamepad(const SdlGamepad& g) {
    PcGamepad pc;
    auto bit = [&](bool on, u16 mask) {
        if (on) pc.buttons |= mask;
    };
    bit(g.dpadUp, 0x0001), bit(g.dpadDown, 0x0002), bit(g.dpadLeft, 0x0004), bit(g.dpadRight, 0x0008);
    bit(g.start, 0x0010), bit(g.back, 0x0020), bit(g.leftStick, 0x0040), bit(g.rightStick, 0x0080);
    bit(g.leftShoulder, 0x0100), bit(g.rightShoulder, 0x0200);
    bit(g.south, 0x1000), bit(g.east, 0x2000), bit(g.west, 0x4000), bit(g.north, 0x8000);
    // the trigger byte b came as b * 32767 / 255 (truncated): back with rounding, exact for every b
    auto trigger = [](s16 v) {
        const s32 t = v < 0 ? 0 : v;
        return static_cast<u8>((t * 255 + 16383) / 32767);
    };
    pc.leftTrigger = trigger(g.leftTrigger);
    pc.rightTrigger = trigger(g.rightTrigger);
    pc.thumbLX = g.leftX;
    pc.thumbRX = g.rightX;
    pc.thumbLY = static_cast<s16>(~g.leftY);          // (SDL's Y is ~XInput's: positive down)
    pc.thumbRY = static_cast<s16>(~g.rightY);
    return pc;
}

input::GamepadState KeyboardGamepad(const Window& w) {
    PcGamepad pc;
    auto bit = [&](int key, u16 mask) {
        if (w.KeyDown(key)) pc.buttons |= mask;
    };
    const bool alt = w.KeyDown(SDL_SCANCODE_LALT) || w.KeyDown(SDL_SCANCODE_RALT);
    bit(SDL_SCANCODE_UP, 0x0001);
    bit(SDL_SCANCODE_DOWN, 0x0002);
    bit(SDL_SCANCODE_LEFT, 0x0004);
    bit(SDL_SCANCODE_RIGHT, 0x0008);
    if (!alt) bit(SDL_SCANCODE_RETURN, 0x0010);
    bit(SDL_SCANCODE_ESCAPE, 0x0020);
    bit(SDL_SCANCODE_Q, 0x0100);
    bit(SDL_SCANCODE_F, 0x0200);
    bit(SDL_SCANCODE_SPACE, 0x1000);
    bit(SDL_SCANCODE_E, 0x2000);
    bit(SDL_SCANCODE_LCTRL, 0x4000);
    bit(SDL_SCANCODE_RCTRL, 0x4000);
    bit(SDL_SCANCODE_R, 0x8000);
    pc.leftTrigger = w.KeyDown(SDL_SCANCODE_Z) ? 255 : 0;
    pc.rightTrigger = (w.KeyDown(SDL_SCANCODE_LSHIFT) || w.KeyDown(SDL_SCANCODE_RSHIFT)) ? 255 : 0;
    auto axis = [&](int neg, int pos) {
        return static_cast<s16>((w.KeyDown(pos) ? 32767 : 0) - (w.KeyDown(neg) ? 32767 : 0));
    };
    pc.thumbLX = axis(SDL_SCANCODE_A, SDL_SCANCODE_D);
    pc.thumbLY = axis(SDL_SCANCODE_S, SDL_SCANCODE_W);
    pc.thumbRX = axis(SDL_SCANCODE_J, SDL_SCANCODE_L);
    pc.thumbRY = axis(SDL_SCANCODE_K, SDL_SCANCODE_I);
    return MapPcGamepad(pc);
}

struct Gamepads::Impl {
    SDL_Gamepad* pads[4] = {};
    SDL_JoystickID ids[4] = {};
    u16 motors[4][2] = {};
};

Gamepads::Gamepads() : impl_(new Impl) {
}

Gamepads::~Gamepads() {
    for (SDL_Gamepad* p : impl_->pads)
        if (p) SDL_CloseGamepad(p);
    delete impl_;
}

namespace {

SdlGamepad ReadSdl(SDL_Gamepad* p) {
    SdlGamepad g;
    auto b = [&](SDL_GamepadButton k) { return SDL_GetGamepadButton(p, k); };
    g.dpadUp = b(SDL_GAMEPAD_BUTTON_DPAD_UP), g.dpadDown = b(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    g.dpadLeft = b(SDL_GAMEPAD_BUTTON_DPAD_LEFT), g.dpadRight = b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    g.start = b(SDL_GAMEPAD_BUTTON_START), g.back = b(SDL_GAMEPAD_BUTTON_BACK);
    g.leftStick = b(SDL_GAMEPAD_BUTTON_LEFT_STICK), g.rightStick = b(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    g.leftShoulder = b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), g.rightShoulder = b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    g.south = b(SDL_GAMEPAD_BUTTON_SOUTH), g.east = b(SDL_GAMEPAD_BUTTON_EAST);
    g.west = b(SDL_GAMEPAD_BUTTON_WEST), g.north = b(SDL_GAMEPAD_BUTTON_NORTH);
    g.leftTrigger = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    g.rightTrigger = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    g.leftX = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), g.leftY = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
    g.rightX = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX), g.rightY = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY);
    return g;
}

}  // namespace

void Gamepads::Update(bool trackChanges, input::InputSystem* system) {
    Impl& d = *impl_;
    // the connections: SDL's list against the ports (the event pump, Window::Pump, keeps the list current)
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    bool present[4] = {};
    for (int i = 0; ids && i < count; ++i) {
        u32 port = 4;
        for (u32 p = 0; p < 4; ++p)
            if (d.pads[p] && d.ids[p] == ids[i]) port = p;
        if (port == 4) {                              // new: the first free port
            for (u32 p = 0; p < 4 && port == 4; ++p)
                if (!d.pads[p]) port = p;
            if (port == 4) continue;
            d.pads[port] = SDL_OpenGamepad(ids[i]);
            if (!d.pads[port]) continue;
            d.ids[port] = ids[i];
            d.motors[port][0] = d.motors[port][1] = 0;
        }
        present[port] = true;
    }
    SDL_free(ids);
    for (u32 port = 0; port < 4; ++port) {
        if (d.pads[port] && !present[port]) {
            SDL_CloseGamepad(d.pads[port]);
            d.pads[port] = nullptr;
        }
        const bool now = d.pads[port] != nullptr;
        if (now) {
            states_[port] = MapSdlGamepad(ReadSdl(d.pads[port]));
            if (d.motors[port][0] || d.motors[port][1])   // (XInput's motors run until changed: refreshed)
                SDL_RumbleGamepad(d.pads[port], d.motors[port][0], d.motors[port][1], 100);
        }
        if (now != connected_[port]) {
            if (system && system->pads[port]) {
                system->pads[port]->SetDeviceOpen(now);
                if (!now) system->pads[port]->Reset();
            }
            if (trackChanges && port != 0) {          // (port 0: the keyboard without a controller)
                const u32 bit = 1u << port;
                if (now) insertions_ |= bit, removals_ &= ~bit;
                else removals_ |= bit, insertions_ &= ~bit;
            }
        }
        connected_[port] = now;
    }
}

void Gamepads::Poll(input::InputSystem& system) { Update(false, &system); }

void Gamepads::Poll() { Update(true, nullptr); }

bool Gamepads::TakeChanges(u32& insertions, u32& removals) {
    insertions = insertions_, removals = removals_;
    insertions_ = removals_ = 0;
    return (insertions | removals) != 0;
}

void Gamepads::SetMotors(u32 port, u16 left, u16 right) {
    Impl& d = *impl_;
    if (port >= 4 || !d.pads[port]) return;
    d.motors[port][0] = left, d.motors[port][1] = right;
    SDL_RumbleGamepad(d.pads[port], left, right, (left || right) ? 100 : 0);
}

}  // namespace fut::platform

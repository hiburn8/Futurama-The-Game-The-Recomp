// Gamepad input platform layer; see GamepadInput.h.
#include "platform/win32/GamepadInput.h"

#include "platform/win32/Window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

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

input::GamepadState KeyboardGamepad(const Window& w) {
    PcGamepad pc;
    auto bit = [&](int key, u16 mask) {
        if (w.KeyDown(key)) pc.buttons |= mask;
    };
    bit(VK_UP, 0x0001);
    bit(VK_DOWN, 0x0002);
    bit(VK_LEFT, 0x0004);
    bit(VK_RIGHT, 0x0008);
    bit(VK_RETURN, 0x0010);
    bit(VK_ESCAPE, 0x0020);
    bit('Q', 0x0100);
    bit('F', 0x0200);
    bit(VK_SPACE, 0x1000);
    bit('E', 0x2000);
    bit(VK_CONTROL, 0x4000);
    bit('R', 0x8000);
    pc.leftTrigger = w.KeyDown('Z') ? 255 : 0;
    pc.rightTrigger = w.KeyDown(VK_SHIFT) ? 255 : 0;
    auto axis = [&](int neg, int pos) {
        return static_cast<s16>((w.KeyDown(pos) ? 32767 : 0) - (w.KeyDown(neg) ? 32767 : 0));
    };
    pc.thumbLX = axis('A', 'D');
    pc.thumbLY = axis('S', 'W');
    pc.thumbRX = axis('J', 'L');
    pc.thumbRY = axis('K', 'I');
    return MapPcGamepad(pc);
}

void XInputPads::Poll(input::InputSystem& system) {
    for (u32 port = 0; port < 4; ++port) {
        XINPUT_STATE st = {};
        const bool now = XInputGetState(port, &st) == ERROR_SUCCESS;
        if (now) {
            PcGamepad pc;
            pc.buttons = st.Gamepad.wButtons;
            pc.leftTrigger = st.Gamepad.bLeftTrigger;
            pc.rightTrigger = st.Gamepad.bRightTrigger;
            pc.thumbLX = st.Gamepad.sThumbLX;
            pc.thumbLY = st.Gamepad.sThumbLY;
            pc.thumbRX = st.Gamepad.sThumbRX;
            pc.thumbRY = st.Gamepad.sThumbRY;
            states_[port] = MapPcGamepad(pc);
        }
        if (now != connected_[port] && system.pads[port]) {
            system.pads[port]->SetDeviceOpen(now);
            if (!now) system.pads[port]->Reset();
        }
        connected_[port] = now;
    }
}

void XInputPads::Poll() {
    for (u32 port = 0; port < 4; ++port) {
        XINPUT_STATE st = {};
        const bool now = XInputGetState(port, &st) == ERROR_SUCCESS;
        if (now) {
            PcGamepad pc;
            pc.buttons = st.Gamepad.wButtons;
            pc.leftTrigger = st.Gamepad.bLeftTrigger;
            pc.rightTrigger = st.Gamepad.bRightTrigger;
            pc.thumbLX = st.Gamepad.sThumbLX;
            pc.thumbLY = st.Gamepad.sThumbLY;
            pc.thumbRX = st.Gamepad.sThumbRX;
            pc.thumbRY = st.Gamepad.sThumbRY;
            states_[port] = MapPcGamepad(pc);
        }
        if (now != connected_[port] && port != 0) {   // (port 0: the keyboard without a controller)
            const u32 bit = 1u << port;
            if (now) insertions_ |= bit, removals_ &= ~bit;
            else removals_ |= bit, insertions_ &= ~bit;
        }
        connected_[port] = now;
    }
}

bool XInputPads::TakeChanges(u32& insertions, u32& removals) {
    insertions = insertions_, removals = removals_;
    insertions_ = removals_ = 0;
    return (insertions | removals) != 0;
}

void XInputPads::SetMotors(u32 port, u16 left, u16 right) {
    if (port >= 4 || !connected_[port]) return;
    XINPUT_VIBRATION v = {left, right};
    XInputSetState(port, &v);
}

}  // namespace fut::platform

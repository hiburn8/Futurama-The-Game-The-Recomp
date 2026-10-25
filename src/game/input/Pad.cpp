// Decompiled from default.xbe 0x0009B160-0x0009C4C0; see Pad.h. Float comparisons follow the x87 flag tests of the
// original (they only differ from the obvious C++ for NaN, which the inputs never produce).
#include "game/input/Pad.h"

#include <cmath>

namespace fut::input {
namespace {

constexpr f32 kAnalogScale = 1.0f / 256.0f;        // orig: 0x002A15A4
constexpr f32 kThumbScale = 1.0f / 32768.0f;       // orig: 0x002A159C
constexpr f32 kActivateAnalog = 0.1171875f;        // orig: 0x002A15A0 (also the default button threshold)
constexpr f32 kActivateThumb = 0.4f;               // orig: 0x00293B28 (also the default stick dead zone)
constexpr f32 kStickGain = 1.3f;                   // orig: 0x002A1580
constexpr f32 kIdleStick = 0.25f;                  // orig: 0x00293B50
constexpr f32 kIdleWrapAbove = 7200.0f;            // orig: 0x002A158C
constexpr f32 kIdleWrapBy = 3600.0f;               // orig: 0x00294378
constexpr f32 kMotorScale = 65535.0f;              // orig: 0x002A1598

f32 F(double v) { return static_cast<f32>(v); }

// orig: 0x0009B470 / 0x0009B510 (one axis): the dead zone is halved when the other axis is outside its own.
double RemapAxis(f32 value, f32 deadZone, f32 other, f32 otherDeadZone, bool clampPositive) {
    double dz = deadZone;
    if (other > otherDeadZone || other < -otherDeadZone) dz *= 0.5;
    if (dz < value) {                                       // positive side
        const double r = (double(value) - dz) * kStickGain / (1.0 - dz);
        if (clampPositive) return r > 1.0 ? 1.0 : r;
        return r < -1.0 ? -1.0 : r;                         // orig: StickY compares with -1 here
    }
    if (!(-dz > value)) return 0.0;                         // inside the dead zone
    const double r = (dz + value) * kStickGain / (1.0 - dz);
    return r < -1.0 ? -1.0 : r;
}

}  // namespace

// ---- pad --------------------------------------------------------------------------------------------------------------
Pad::Pad(InputSystem& system, u32 port) : system_(system), port_(port) {
    // orig: base ctor 0x0009B860 (thresholds 1/256, dead zones 0.15, rumble dirty) then pad ctor 0x0009BFE0.
    for (u32 b = 0; b < kButtonCount; ++b)
        if (b != kLeftThumb && b != kRightThumb) buttons[b].threshold = kActivateAnalog;
    for (StickState& s : sticks) s.deadZoneX = s.deadZoneY = kActivateThumb;
    Reset();
    SetDisconnected(true);
}

void Pad::SetButton(u32 b, f32 v) {                 // orig: 0x0009B830
    ButtonState& s = buttons[b];
    s.previous = s.value;
    s.value = v;
    if (v < s.threshold) s.latched = false;
}

void Pad::Reset() {                                 // orig: 0x0009BBE0
    for (u32 b = 0; b < kButtonCount; ++b) SetButton(b, 0.0f);
    for (StickState& s : sticks) {
        s.previousX = s.x;
        s.x = 0.0f;
        s.previousY = s.y;
        s.y = 0.0f;
    }
}

bool Pad::Poll(const GamepadState* st) {            // orig: 0x0009C460
    if (!deviceOpen_) return false;
    if (!st) {
        Reset();
        return false;
    }
    auto analog = [&](u32 i) { return F(double(st->analog[i]) * kAnalogScale); };
    auto thumb = [&](s16 v) { return F(double(v) * kThumbScale); };
    auto digital = [&](u16 mask) { return (st->buttons & mask) ? 1.0f : 0.0f; };
    if (!system_.activePad) {
        bool any = std::fabs(thumb(st->thumbRY)) > kActivateThumb || (st->buttons & 0xFF) != 0;
        for (u32 i = 0; i < 8; ++i) any = any || analog(i) > kActivateAnalog;
        any = any || std::fabs(thumb(st->thumbLX)) > kActivateThumb || std::fabs(thumb(st->thumbLY)) > kActivateThumb ||
              std::fabs(thumb(st->thumbRX)) > kActivateThumb;
        if (any) {
            system_.activePadFound = true;
            system_.activePad = this;
        }
    }
    SetButton(kDPadLeft, digital(0x04));
    SetButton(kDPadRight, digital(0x08));
    SetButton(kDPadUp, digital(0x01));
    SetButton(kDPadDown, digital(0x02));
    SetButton(kButtonX, analog(2));
    SetButton(kButtonB, analog(1));
    SetButton(kButtonY, analog(3));
    SetButton(kButtonA, analog(0));
    SetButton(kLeftTrigger, analog(6));
    SetButton(kRightTrigger, analog(7));
    SetButton(kStart, digital(0x10));
    SetButton(kBack, digital(0x20));
    SetButton(kLeftThumb, digital(0x40));
    SetButton(kRightThumb, digital(0x80));
    SetButton(kWhite, analog(5));
    SetButton(kBlack, analog(4));
    const s16 axes[2][2] = {{st->thumbLX, st->thumbLY}, {st->thumbRX, st->thumbRY}};
    for (u32 i = 0; i < 2; ++i) {                   // orig: 0x0009B410 / 0x0009B430
        sticks[i].previousX = sticks[i].x;
        sticks[i].x = thumb(axes[i][0]);
        sticks[i].previousY = sticks[i].y;
        sticks[i].y = thumb(axes[i][1]);
    }
    if (rumbleDirty) SendRumble();
    SetDisconnected(false);
    return true;
}

void Pad::SendRumble() {                            // orig: 0x0009C2B0
    if (feedbackStatus == 997) return;              // ERROR_IO_PENDING: previous update still in flight
    if (system_.rumbleEnabled) {
        motorLeft = static_cast<u16>(static_cast<s32>(double(rumbleLeft) * kMotorScale));   // _ftol truncates
        motorRight = static_cast<u16>(static_cast<s32>(double(rumbleRight) * kMotorScale));
    } else {
        motorLeft = motorRight = 0;
    }
    if (system_.onRumble) system_.onRumble(*this);
    rumbleDirty = false;
}

bool Pad::IsDown(u32 b) const {
    return b < kButtonCount && !(buttons[b].value < buttons[b].threshold);
}
bool Pad::JustReleased(u32 b) const {
    if (b >= kButtonCount) return false;
    const ButtonState& s = buttons[b];
    return s.value < s.threshold && !(s.previous < s.threshold);
}
bool Pad::JustPressed(u32 b) const {
    if (b >= kButtonCount) return false;
    const ButtonState& s = buttons[b];
    return !(s.value < s.threshold) && s.previous < s.threshold;
}
bool Pad::IsDownUnlatched(u32 b) const {
    if (b >= kButtonCount) return false;
    const ButtonState& s = buttons[b];
    return !s.disabled && !s.latched && !(s.value < s.threshold);
}
bool Pad::JustPressedUnlatched(u32 b) const {
    if (b >= kButtonCount) return false;
    const ButtonState& s = buttons[b];
    return !s.disabled && !s.latched && !(s.value < s.threshold) && s.previous < s.threshold;
}
f32 Pad::Value(u32 b) const {
    if (b >= kButtonCount || buttons[b].disabled || buttons[b].latched) return 0.0f;
    return buttons[b].value;
}
void Pad::Latch(u32 b) {
    if (b < kButtonCount) buttons[b].latched = true;
}
bool Pad::SetDisabled(u32 b, u8 count) {
    if (b >= kButtonCount) return false;
    const bool was = buttons[b].disabled != 0;
    buttons[b].disabled = count;
    return was;
}
void Pad::DecrementDisabled(u8 n) {
    for (ButtonState& s : buttons) s.disabled = n < s.disabled ? static_cast<u8>(s.disabled - n) : 0;
}

double Pad::StickX(s32 i) const {
    if (i < 0 || i >= 2) return 0.0;
    const StickState& s = sticks[i];
    return RemapAxis(s.x, s.deadZoneX, s.y, s.deadZoneY, true);
}
double Pad::StickY(s32 i) const {
    if (i < 0 || i >= 2) return 0.0;
    const StickState& s = sticks[i];
    return RemapAxis(s.y, s.deadZoneY, s.x, s.deadZoneX, false);
}

bool Pad::IsIdle() const {                          // orig: 0x0009BB50
    for (const ButtonState& s : buttons)
        if (!s.disabled && !s.latched && !(s.value < s.threshold)) return false;
    for (s32 i = 0; i < 2; ++i) {
        if (std::fabs(StickX(i)) > kIdleStick) return false;
        if (std::fabs(StickY(i)) > kIdleStick) return false;
    }
    return true;
}

bool Pad::SetRumble(f32 left, f32 right, bool force) {   // orig: 0x0009B740
    if (!force && !system_.rumbleEnabled) return false;
    if ((left < 1.0f || left == 1.0f) && !(left < 0.0f) && rumbleLeft != left) {
        rumbleLeft = left;
        rumbleDirty = true;
    }
    if ((right < 1.0f || right == 1.0f) && !(right < 0.0f) && rumbleRight != right) {
        rumbleRight = right;
        rumbleDirty = true;
    }
    return true;
}

// ---- system -----------------------------------------------------------------------------------------------------------
InputSystem::InputSystem() = default;

void InputSystem::Update(f32 dt, const std::function<const GamepadState*(u32 port)>& readState) {   // 0x0009BEA0
    if (rumbleTimer > 0.0f) {                       // delayed rumble enable
        const double t = double(rumbleTimer) - dt;
        rumbleTimer = F(t);
        if (t <= 0.0 && !rumbleEnabled) {
            rumbleTimer = 0.0f;
            for (Pad* p : pads)
                if (p) p->rumbleDirty = true;
            rumbleEnabled = vibrationOption;
        }
    }
    for (Pad* p : pads) {                           // orig: the pad list, i.e. construction order
        if (!p) continue;
        p->Poll(p->DeviceOpen() ? readState(p->Port()) : nullptr);
        if (p->IsIdle()) {
            const double sum = double(dt) + p->idleTime;
            p->idleTime = sum > kIdleWrapAbove ? F(sum - kIdleWrapBy) : F(sum);
        }
    }
    for (Pad* p : pads)
        if (p && !p->IsIdle()) {
            idleTime = 0.0f;
            return;
        }
    // Unlike the per-pad timer, the wrap subtracts from the already rounded value.
    const double sum = double(idleTime) + dt;
    idleTime = F(sum);
    if (sum > kIdleWrapAbove) idleTime = F(double(idleTime) - kIdleWrapBy);
}

void InputSystem::HandleDeviceChanges(const std::function<bool(u32& insertions, u32& removals)>& changes) {
    // orig: 0x0009C1D0
    u32 insertions = 0, removals = 0;
    if (!changes || !changes(insertions, removals)) return;   // XGetDeviceChanges(&g_DeviceType_Gamepad, ...) == 0
    for (u32 port = 0; port < 4; ++port) {            // g_pads 0x002E7024 [port] (no null test in the original)
        Pad* p = pads[port];
        if (!p) continue;
        const u32 bit = 1u << port;
        if ((removals & bit) == bit) {
            p->SetDeviceOpen(false);                  // +0x150: XInputClose, 0
            p->Reset();                               // 0x0009BBE0
            p->SetDisconnected(true);                 // 0x0009B810(1)
        }
        if ((insertions & bit) == bit) {
            // +0x150: XInputClose when open, then XInputOpen(gamepad, the port table 0x002D6640 [+0x14C], 0, 0)
            p->SetDeviceOpen(true);
            p->Reset();
            p->SetDisconnected(false);                // 0x0009B810(0)
        }
    }
}

void InputSystem::SetRumbleEnabled(bool enable, f32 delay) {   // orig: 0x0009BAD0
    if (!enable) rumbleTimer = 0.0f;
    if (rumbleEnabled == enable) return;
    if (enable) {
        rumbleTimer = delay;
        if (delay > 0.0f) return;                   // enabled later by Update
    }
    for (Pad* p : pads)
        if (p) p->rumbleDirty = true;
    rumbleEnabled = enable && vibrationOption;
}

// ---- menu repeat ------------------------------------------------------------------------------------------------------
InputRepeat::InputRepeat(u16 p) : period(p) {}

void InputRepeat::Reset() {                         // orig: 0x0009B260
    for (ButtonRepeat& b : buttons) b = {};
    for (StickRepeat& s : sticks) s = {};
}

void InputRepeat::Update(const InputSystem& system) {   // orig: 0x0009B3D0
    const Pad* pad = system.activePad;
    if (!pad || pad->Disconnected() != 0) {
        Reset();
        return;
    }
    UpdateButtons(*pad);
    UpdateSticks(*pad);
}

void InputRepeat::UpdateButtons(const Pad& pad) {   // orig: 0x0009B2A0
    for (u32 b = 0; b < kButtonCount; ++b) {
        ButtonRepeat& r = buttons[b];
        r.fired = false;
        if (!pad.IsDownUnlatched(b)) {
            r.count = 0;
            continue;
        }
        if (period != 0 && r.count % period == 0) r.fired = true;   // port-only guard: the original divides
        r.count = static_cast<u16>(r.count + 1);
    }
}

void InputRepeat::UpdateAxis(s16& count, bool& neg, bool& pos, f32 value) {   // orig: 0x0009B1B0
    auto reset = [&] {
        neg = pos = false;
        count = 0;
    };
    if (value < 0.0f) {
        if (count > 0) return reset();
        if (period != 0 && (-s32(count)) % s32(period) == 0) neg = true;
        count = static_cast<s16>(count - 1);
        return;
    }
    if (!(value > 0.0f)) return reset();
    if (count < 0) return reset();
    if (period != 0 && s32(count) % s32(period) == 0) pos = true;
    count = static_cast<s16>(count + 1);
}

void InputRepeat::UpdateSticks(const Pad& pad) {    // orig: 0x0009B300
    for (s32 i = 0; i < 2; ++i) {
        const f32 x = F(pad.StickX(i));
        const f32 y = F(pad.StickY(i));
        StickRepeat& s = sticks[i];
        s.negX = s.posX = s.negY = s.posY = false;
        UpdateAxis(s.countX, s.negX, s.posX, x);
        UpdateAxis(s.countY, s.negY, s.posY, y);
    }
}

}  // namespace fut::input

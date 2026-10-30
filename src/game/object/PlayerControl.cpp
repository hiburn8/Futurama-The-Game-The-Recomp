// The player's per-step code: Player::Control (input, steering, gravity), the action map and the action / animation
// state machine; see Character.h and docs/analysis/character_update.md (sections 3 and 4). x87 conventions as in
// CharacterUpdate.cpp. Paths not ported yet report themselves once (NOT PORTED reached: ...) and carry on.
#include "game/object/Character.h"

#include "core/CrtMath.h"
#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "engine/ni/NiCollision.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiRenderer.h"
#include "engine/ni/NiKeyframeManager.h"
#include "game/GameTime.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/level/Camera.h"
#include "game/level/Dialogue.h"
#include "game/level/Hud.h"
#include "game/level/LevelData.h"
#include "game/level/ScriptTriggers.h"
#include "game/level/Stage.h"
#include "game/object/Cannon.h"
#include "game/object/ChickenWalker.h"
#include "game/object/Destructable.h"
#include "game/object/GenericNpc.h"
#include "game/object/Impact.h"
#include "game/object/NodeUpdate.h"
#include "game/object/Pickup.h"
#include "game/object/RatRide.h"
#include "game/sound/GameSound.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

const NiPoint3 kUnitX{1, 0, 0};                       // orig: 0x002D77E0
const NiPoint3 kUnitY{0, 1, 0};                       // orig: 0x002D77EC
const NiPoint3 kUnitZ{0, 0, 1};                       // orig: 0x002D77F8

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: 0x00103290: M^T * v (each output the dot product of a column with v), x87, each component stored once.
NiPoint3 TransposeMul(const ni::NiMatrix3& m, const NiPoint3& v) {
    return {F((double(m.m[2][0]) * v.z + double(m.m[1][0]) * v.y) + double(v.x) * m.m[0][0]),
            F((double(m.m[0][1]) * v.x + double(m.m[2][1]) * v.z) + double(m.m[1][1]) * v.y),
            F((double(m.m[0][2]) * v.x + double(m.m[2][2]) * v.z) + double(m.m[1][2]) * v.y)};
}

// The inline M * v of 0x000536D0 / 0x00054AE0 (row r: (M[r][2]*v.z + M[r][1]*v.y) + M[r][0]*v.x, one store).
NiPoint3 InlineMul(const ni::NiMatrix3& m, const NiPoint3& v) {
    auto row = [&](int r) {
        return F((double(m.m[r][2]) * v.z + double(m.m[r][1]) * v.y) + double(m.m[r][0]) * v.x);
    };
    return {row(0), row(1), row(2)};
}

// orig: CharBody vf50 0x000AF7D0 (AddVelocity).
void AddBodyVelocity(phys::CharBody& b, const NiPoint3& a) {
    NiPoint3& v = b.velocity;
    v = {F(double(v.x) + a.x), F(double(a.y) + v.y), F(double(a.z) + v.z)};
}

// orig: 0x0009BE70 Input_AllPadsIdle.
bool AllPadsIdle(const input::InputSystem& in) {
    for (const input::Pad* p : in.pads)
        if (p && !p->IsIdle()) return false;
    return true;
}


// The action map's default bindings (orig: rows 1, 2, 4, 7 written by the constructor; 2, 3 = 1; 5, 6 = 4; 8, 9 = 7).
constexpr std::array<u8, 33> kRow1 = {0,    0,    0,    0xFF, 1,    1,    0,    0,    1,    0,    2,
                                      3,    0,    1,    8,    9,    0xFF, 0xFF, 0xFF, 0xFF, 0x0D, 0x0C,
                                      0,    1,    5,    6,    8,    0x0E, 0x0F, 0x0B, 0x0A, 7,    4};

void Push(std::array<u8, 10>& h, u8 v) {
    for (int i = 9; i > 0; --i) h[i] = h[i - 1];
    h[0] = v;
}

}  // namespace

// orig: 0x00034100: acos for |c| < 1 (CRT acos 0x001673E4), PI (DAT_002F9274) at or below -1, 0 at or above 1.
double AcosClamped(f32 c) {
    if (c <= -1.0f) return 3.14159274101257324;      // 0x40490FDB
    if (c < 1.0f) return crt::Acos(double(c));      // CRT acos 0x001673E4 -> 0x00167401 (core/CrtMath.h)
    return 0.0;
}

// ---- 3.1 the action map -------------------------------------------------------------------------------------------
ActionMap::ActionMap() {                              // orig: 0x0005E2B0
    for (int r = 1; r <= 9; ++r) rows[r] = kRow1;
    for (int r = 7; r <= 9; ++r) rows[r][29] = 0;     // (row 7 has DPadLeft where row 1 has Back)
    rows[0] = rows[1];                                // orig: 0x0005E010(1)
}

void ActionMap::Update(const input::Pad* p) {         // orig: 0x0005E0D0
    hold.fill(0);                                     // (33 shifts of the hold history)
    lastHold = clock;
    for (u32 i = 9; i <= 32; ++i) {
        if (p && p->IsDownUnlatched(rows[0][i])) {
            if (!held[i]) {
                holdTime[i] = 0;
                Push(press, static_cast<u8>(i));
                lastPress = clock;
            }
            if (holdTime[i] > 0.2f) {                 // orig: 0x00293AB4
                Push(hold, static_cast<u8>(i));
                lastHold = clock;
            }
            held[i] = 1;
            holdTime[i] = F(double(g_frameStep) + holdTime[i]);
        } else {
            holdTime[i] = 0;
            if (held[i]) {
                Push(release, static_cast<u8>(i));
                lastRelease = clock;
            }
            held[i] = 0;
        }
    }
    if (double(lastPress) + 0.25f < clock) {          // orig: 0x00293B50
        Push(press, 0);
        lastPress = clock;
    }
    if (double(lastRelease) + 0.25f < clock) {
        Push(release, 0);
        lastRelease = clock;
    }
    clock = F(double(g_frameStep) + clock);
}

// ---- predicates ---------------------------------------------------------------------------------------------------
bool Player::IsJumpAnim() const {                     // orig: vf12C 0x00050010
    static constexpr s32 kIds[] = {0x1B, 0x21, 0x1E, 0x7C, 0x76, 0x79, 0x98, 0x9E, 0x9B, 0xD1, 0xB7};
    for (s32 k : kIds)
        if (IsAnimPlaying(k)) return true;
    return false;
}

bool Player::IsFallAnim() const {                     // orig: vf140 0x000501A0
    static constexpr s32 kIds[] = {0x1C, 0x22, 0x1F, 0x77, 0x7D, 0x99, 0x9F, 0xD2};
    for (s32 k : kIds)
        if (IsAnimPlaying(k)) return true;
    return false;
}

bool Player::IsAttackAnim() const {                   // orig: vf124 0x0004FBB0
    if (type == 1) return WeaponOverlayPlaying();     // 0x0004FD7C: jmp vf128
    if (type == 2) {
        static constexpr s32 kIds[] = {0xA9, 0xAA, 0xAB, 0xB3, 0xAD, 0xAF, 0xB1, 0xAE, 0xB0, 0xB2, 0xB4, 0xB5, 0xB6, 0xB7};
        for (s32 k : kIds)
            if (IsAnimPlaying(k)) return true;
        return false;
    }
    if (type == 3) {
        static constexpr s32 kIds[] = {0xD1, 0xD2, 0xD3, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD};
        for (s32 k : kIds)
            if (IsAnimPlaying(k)) return true;
    }
    return false;
}

bool Player::OnGround() const {                       // orig: vf70 0x0004FFE0
    return (body && body->supported) || airTime < 0.05f;   // orig: 0x00293BDC (TEST AH,5; JP)
}

bool Player::JumpingOrFalling() const {               // orig: vf130 0x000500F0
    if (IsJumpAnim()) return true;                    // vf12C
    if (!(GetWorldPosition().y >= spawnY)) return false;   // vf34; FCOMP +0xB84; TEST AH,1; JNE (below or NaN)
    return IsFallAnim();                              // vf140
}

bool Player::IsDeathFall() const {                    // orig: vf134 0x000500C0
    return IsAnimPlaying(0x66) || (IsDead() && deathKind == 7);   // 0x00033E20(0x66); vfA8; +0x778
}

bool Player::HasInventoryLike(const GameObject* o) const {   // orig: vf158 0x000511B0
    if (!o || !o->def) return false;                  // o, o+0x10 (and D+0x10, the name pointer: never null here)
    for (const GameObject* it = inventory.NextObject(nullptr); it; it = inventory.NextObject(it))   // 0x00097670(+0x38)
        if (it->def && std::strcmp(it->def->name.c_str(), o->def->name.c_str()) == 0) return true;   // inline strcmp
    return false;
}

bool Player::SpendCharge(f32 v) {                     // orig: vf160 0x00050460
    if (!(v < charges || v == charges)) return false; // FCOMP +0xAF8; TEST AH,0x41; JNE (NaN: false)
    charges = F(double(charges) - v);                 // FLD +0xAF8; FSUB v; FSTP
    return true;
}

bool Player::AmmoBelowMax(s32 slot) const {           // orig: vf170 0x00050540
    return ammo[static_cast<size_t>(slot)] < kMaxAmmo[slot];   // +0xAD0 + 4 slot against [0x0029A254 + 4 slot] (SETL)
}

void Player::ToggleInvulnerable() {                   // orig: vf174 0x000505D0
    health = std::bit_cast<u32>(health) == 0x47590300u ? 100.0f : std::bit_cast<f32>(0x47590300u);   // dword compare
}

void Player::CheatWeapons() {                         // orig: vf178 0x00055B50 (no caller)
    bool has[4] = {};                                 // BL, esp+0x11..+0x13
    // (the list from the held weapon on, next +8: the original calls every object's vf7C as the weapon type; for
    // another pickup that is 0x00077220 with a stray stack argument, UB: the port only asks the weapons)
    if (weapon && weapon->owner)
        for (GameObject* o = weapon; o; o = o->owner->NextObject(o))
            if (auto* w = dynamic_cast<Weapon*>(o)) {
                const u32 t = w->WeaponType();        // vf7C (four calls in the original, one per type)
                if (t < 4) has[t] = true;
            }
    static const char* const kNames[4] = {"hammer", "pistol", "shotgun", "railgun"};   // 0x0029A50C / 504 / 4FC / 4F4
    // esp+0x14 (the point) and esp+0x20 (the matrix: only the rows' fourth dwords are zeroed) are never written:
    // L zeros
    const NiPoint3 at{};
    const ni::NiMatrix3 rot{};
    for (int k = 0; k < 4; ++k) {
        if (!has[k] || !level.defs) continue;
        Def* d = level.defs->Find(kNames[k]);         // Session::FindDef 0x000C3F10 (twice in the original)
        if (!d) continue;
        Owner* stage = level.currentStage;            // S+0x5C ? S+0x5C + 4 : 0
        if (GameObject* o = d->Spawn(level, stage, at, rot, nullptr))   // vf14(owner, &at, &rot, 0)
            o->SetOwner(&inventory);                  // vf10(P+0x38) (the result used unchecked)
    }
    for (s32& a : ammo) a = 999;                      // +0xAD0..+0xAE0 = 0x3E7 (no HUD update)
}

s32 Player::WeaponAmmo(const Weapon* w) const {       // orig: vfC4 0x0004F7F0
    if (!w) return 0;
    const u32 t = w->WeaponType();                    // vf7C; jump table 0x0004F83C
    return t >= 1 && t <= 4 ? ammo[t - 1] : 0;        // +0xAD0 / +0xAD4 / +0xAD8 / +0xADC
}

s32 Player::WeaponMaxAmmo(const Weapon* w) {          // orig: 0x0004F850
    if (!w) return 0;
    const u32 t = w->WeaponType();                    // vf7C; jump table 0x0004F890
    return t >= 1 && t <= 4 ? kMaxAmmo[t - 1] : 0;    // 0x1F4 / 0xC8 / 0x12C / 0x190
}

// ---- 3.6 Player::Control ------------------------------------------------------------------------------------------
void Player::Think(f32 dt) {                          // orig: 0x000530C0 (the argument is not read)
    (void)dt;
    const f32 step = g_frameStep;
    {
        const double t = double(step) + sinceAttack;
        sinceAttack = F(t);
        if (t > 5.0f) sinceAttack = 5.0f;             // orig: 0x00293C24
    }
    if (physic) physic->SetActive(false);
    if (body) {
        body->halveXZ = 1;                            // orig: vf2C(1)
        body->shape.pinned = 0;
        body->shape.noPush = 0;
        body->integratePinned = 0;
    }
    NiPoint3 vel{};
    ni::NiMatrix3 R = rotation;                       // orig: vf38
    const ni::NiMatrix3 R0 = R;
    inputDir = {};
    SampleInput(vel, R);
    if (body && (body->shape.flags & 0x80))           // orig: vf4C
        vel = {F(double(vel.x) * 0.0), F(double(vel.y) * 0.0), F(double(vel.z) * 0.0)};
    ni::NiMatrix3 R2 = R0;
    LadderCheck(vel, R2);
    LedgeCheck(vel, R2);
    if (!(ladder || ledge || locked || IsAnimPlaying(0x53))) {
        b93 = b94 = 0;
        ledgeTimerA = ledgeTimerB = 0;
        NiPoint3 g{0, -95.0f, 0};                     // orig: DAT_002D660C
        if (type == 4) g.y = 0.0f < velBefore.y ? -130.0f : -180.0f;
        if (body) body->SetForce(g);
        if (stowedWeapon) {                           // 0x00053337: +0xC1C taken out again
            (void)SetWeapon(stowedWeapon);            // vfB4
            stowedWeapon = nullptr;
        }
    } else {
        if (weapon) {                                 // 0x0005335B: put away while hanging / climbing / locked / shocked
            stowedWeapon = weapon;                    // +0xC1C
            attacking504 = attacking505 = 0;
            pendingWeapon = nullptr;                  // +0x524
            (void)SetWeapon(nullptr);                 // vfB4(0)
            ClearOverlay();                           // 0x000363C0
        }
        if (body) {
            body->velocity = {};
            body->SetForce({});
        }
        if (ledge) vel = {};
        R = locked ? R0 : R2;
    }
    if (ladder || ledge || locked) {
        if (level.camera) level.camera->recentre = 1; // orig: 0x00053427: 0x000111A0 on (DAT_002DAC64)+0x8C (no null test)
        if (!locked && body) body->shape.noPush = 1;
    }
    if (!chicken) {                                   // orig: 0x0005344E (P+0x7B0, not its active flag)
        directRoot = 0;
        PostHook(vel, R);                             // orig: vfF8
    }
    vel = {F(double(vel.x) * 1.0f), F(double(vel.y) * 1.0f), F(double(vel.z) * 1.0f)};   // orig: DAT_002D6210
    thinkVel = vel;
    if (body)                                         // orig: vf50 AddVelocity 0x000AF7D0
        body->velocity = {F(double(body->velocity.x) + vel.x), F(double(vel.y) + body->velocity.y),
                          F(double(vel.z) + body->velocity.z)};
    SetRotation(R);                                   // orig: vf24
}

// ---- the rumble channels (camera_modes_bender_port.md 5) ----------------------------------------------------------
void Rumble::Add(f32 l, f32 r, f32 duration, s32 k) {   // orig: 0x0005F000
    // FCOMP [0x00293AB8]; TEST AH,0x41; JNP: returns on < and ==, a NaN duration goes on.
    if (!(duration > 0.0f) && !std::isnan(duration)) return;
    for (int i = 0; i < 10; ++i) {
        if (!(time[i] == 0.0f)) continue;             // TEST AH,0x44; JNP: a free channel compares equal (NaN: busy)
        left[i] = l;                                  // +0x00
        kind[i] = k;                                  // +0xA0
        right[i] = r;                                 // +0x28
        time[i] = duration;                           // +0x50
        invDuration[i] = F(1.0 / double(duration));   // FLD [0x00293A98]; FDIV; FSTP +0x78
        return;
    }                                                 // all ten busy: dropped
}

// orig: 0x0005F070 (thiscall(Pad*), RET 4). The two sums are x87 accumulators (left ST1, right ST0); the port's
// doubles can differ in the last bit of a motor value (pad output only, KNOWN_ISSUES #49).
void Rumble::Update(input::Pad& pad) {
    if (disabled) {
        pad.SetRumble(0.0f, 0.0f, false);             // orig: Pad::SetRumble 0x0009B740(0, 0, 0)
        return;
    }
    double sl = 0.0, sr = 0.0;
    for (int i = 0; i < 10; ++i) {
        if (time[i] == 0.0f) continue;                // TEST AH,0x44; JNP
        auto add = [&](double s) {                    // FLD ST0; FMUL left; FADDP ST3; FMUL right; FADDP ST1
            sl += s * left[i];
            sr += s * right[i];
        };
        const double u = double(time[i]) * invDuration[i];   // FLD time; FMUL inv (kinds 1-3)
        switch (static_cast<u32>(kind[i])) {          // unsigned, jump table 0x0005F194
        case 0:                                       // constant (0x0005F0CF)
            sl += left[i];
            sr += right[i];
            break;
        case 1: add(std::sin(u * double(std::bit_cast<f32>(0x40490FD8u)))); break;   // [0x0029B70C]; FSIN (#20)
        case 2: add(1.0 - u); break;                  // FSUBR [0x00293A98]
        case 3: add(u); break;
        default: break;                               // other kinds add nothing
        }
        const double t = double(time[i]) - g_frameStep;   // DAT_002D67E8; FST
        time[i] = F(t);
        if (t < 0.0) time[i] = 0.0f;                  // FCOMP 0; TEST AH,5; JP (the unrounded value)
    }
    f32 l = F(sl), r = F(sr);                         // FST [esp] (right) / FST [esp+4] (left)
    if (sl > 1.0) l = 1.0f;                           // compared on the extended sum
    if (r > 1.0f) r = 1.0f;                           // compared on the stored float
    pad.SetRumble(l, r, false);                       // push 0, R, L
}

// ---- 3.2 / 3.3 input sampling and dispatch -----------------------------------------------------------------------
void Player::SampleInput(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfFC 0x00057960
    const input::InputSystem* in = level.input;
    pad = in ? (in->activePad ? in->activePad : in->pads[0]) : nullptr;   // orig: Input_GetActivePad 0x0009B720
    thinkVel = {};
    roll98 = roll95 = roll96 = roll97 = b8B = jumpRequest = 0;
    attack1 = attack2 = special50A = special50B = special50C = b9D = 0;
    b44 = 0;
    xValue = aValue = 0;
    if (hasControl && pad) {
        auto b = [&](u32 i) { return actions.Binding(i); };
        auto V = [&](u32 i) { return double(pad->Value(b(i))); };
        auto down = [&](u32 i) { return pad->IsDownUnlatched(b(i)); };
        auto clamp = [](f32& v) {
            if (v > 1.0f) v = 1.0f;                   // TEST AH,0x41; JNE
            if (v < -1.0f) v = -1.0f;                 // TEST AH,5; JP
        };
        const double inv = 1.0;                       // orig: 0x0005E0B0 (the invert-look option DAT_002E79AA, off)
        if (b(1) != 0xFF) {
            moveY = F(pad->StickY(b(1)));
            moveY = F((V(10) - V(11)) + moveY);
        } else {
            moveY = F(V(10) - V(11));
        }
        clamp(moveY);
        if (b(2) != 0xFF) {
            moveX = F(pad->StickX(b(2)));
            moveX = F((V(13) - V(12)) + moveX);
        } else {
            moveX = F(V(13) - V(12));
        }
        clamp(moveX);
        strafe = b(3) != 0xFF ? F(pad->StickX(b(3))) : F(V(15) - V(14));
        lookY = b(4) != 0xFF ? F(inv * F(pad->StickY(b(4)))) : F(inv * F(V(16) - V(17)));
        if (b(6) != 0xFF) lookY = F(inv * F(pad->StickY(b(6))) + lookY);
        clamp(lookY);
        lookX = b(5) != 0xFF ? F(pad->StickX(b(5))) : F(V(19) - V(18));
        if (b(7) != 0xFF) lookX = F(pad->StickX(b(7)) + lookX);
        clamp(lookX);
        if (b(8) != 0xFF) {
            orbitX = F(pad->StickX(b(8)));
            orbitY = F(pad->StickY(b(8)));
        } else {
            orbitY = 0;
            orbitX = F(V(23) - V(22));
        }
        auto bit = [&](u32 i, int sh) { return (down(i) ? 1 : 0) << sh; };
        actionBits = static_cast<u16>(bit(30, 4) | bit(28, 3) | bit(27, 2) | bit(26, 1) | bit(24, 0));
        if (down(b(8) != 0xFF ? 20 : (b(20) != 0xFF ? 20 : 16))) actionBits |= 0x20;
        strafe = 0;
        aValue = F(V(31));
        xValue = F(V(32));
    }
    if (agent) agent->Update(g_frameStep);            // orig: 0x00057FD7 (the player's AI agent, DAT_002D67E8)
    actions.Update(pad);                              // orig: 0x0005E0D0 (P+0x7BC)
    // orig: 0x00058009 (P+0xA04)->0x0005F070(P+0x7B8): the pad is not null-checked (the port runs without pads).
    if (pad) rumble.Update(const_cast<input::Pad&>(*pad));
    // orig: 0x0005800E: the level2-5 cannon (P+0x7A8) while a gun is active: its step, then the ambient light's colour
    // a step towards the global ambient (0x000C0410(&out, P+0x30 +0xE4, dt); +0xCC += 1)
    if (cannon && cannon->IsActive()) {               // 0x00032E40
        cannon->Update();                             // 0x00032FB0
        if (ambientLight) {
            ambientLight->ambient = AmbientStep(ambientLight->ambient, g_frameStep, level.globalAmbient);
            ++ambientLight->revision;
        }
    }
    Camera* cam = level.camera.get();
    // orig: 0x00058093: the level1-6 chicken walker (P+0x7B0) is activated every step ("ChickenNet" 0x00295AC4)
    if (crt::StrICmp(level.name.c_str(), "level1-6") == 0 && chicken) chicken->Activate("ChickenNet");   // 0x00044500
    if (chicken && chicken->active) {                 // orig: 0x00042690
        chicken->Control(vel, rot);                   // orig: 0x000445A0 (P, vel, rot); nothing of the normal branch
    } else if (ride) {                                // orig: 0x000580F0 (P+0x7AC): nothing of the normal branch runs
        PreHook(vel, rot);                            // orig: vfEC (ZoidbergRat 0x000944F0); moveX / moveY are kept
    } else if (!flag28 && !IsDead() && hasControl && !(cannon && cannon->IsActive())) {   // (0x00058137)
        firstPerson = cam && cam->mode == 3;
        PreHook(vel, rot);                            // orig: vfEC
        Walk(vel, rot);                               // orig: vf100
        thinkVel = vel;
        const f32 thr = F((double(cdef.runSpeed) - cdef.walkSpeed) * 0.5f + cdef.walkSpeed);
        NiPoint3 hv{vel.x, 0, vel.z};
        const f32 h = F(ni::Unitize(hv));
        const ni::NiSequence* s = sumSeq;
        if (summing && (s == SeqById(0x15) || s == SeqById(0x72) || s == SeqById(0x87) || s == SeqById(0x94)) &&
            h > thr) {
            prevRunTime = runTime;
            runTime = F(double(g_frameStep) + runTime);
        } else {
            if (runTime == 0.0f) prevRunTime = 0;
            runTime = 0;
        }
        LockOn(rot);                                  // orig: vf150 (every class; acts for Fry only)
        if (actionBits & 1) TryJump();                // orig: vf104
        if (actionBits & 2) Recentre();               // orig: vf108
        if (actionBits & 4) WeaponCycle(false);       // orig: vf10C (White / Black)
        if (actionBits & 8) WeaponCycle(true);
        // (bit 0x10: vf110, a nop in every player class)
        Attack(false, aValue, rot);                   // orig: vf114
        Attack(true, xValue, rot);                    // orig: vf118
        Aim(rot);                                     // orig: 0x00058324..0x000585B8 (Leela / Bender)
        FirstPersonToggle();                          // orig: vf11C(+0xB28, +0xB2C) (the arguments are not read)
        // (vf120(+0xB38): a nop in every player class)
        // 0x000585E1: in first person with a weapon (+0x528) whose definition is a weapon (vf0C(8)) of type 3 (the
        // railgun, vf7C): half the look speed ([0x00293AA0])
        if (firstPerson && weapon && weapon->def && weapon->def->IsType(8) && weapon->WeaponType() == 3) {
            lookY = F(double(lookY) * 0.5f);
            lookX = F(double(lookX) * 0.5f);
        }
    } else {
        moveX = moveY = strafe = 0;
        runTime = prevRunTime = 0;
    }
    if (!hasControl) return;                          // +0x630
    // 0x00058665: in first person the body faces the scene camera's horizontal direction (its local rotation's
    // column 0; the columns v, UNIT_Y, -(UNIT_Y x v), as LeaveFirstPerson's)
    if (firstPerson && cam && cam->sceneCamera) {     // (S+0x8C and its NiCamera +0x30 are not null-tested)
        const ni::NiMatrix3& C = cam->sceneCamera->local.rotate;   // NiCamera +0x30
        NiPoint3 v{C.m[0][0], 0.0f, C.m[2][0]};       // esp+0x44
        (void)ni::Unitize(v);                         // 0x000116A0
        const NiPoint3 u = kUnitY;                    // esp+0x28 (dword copies of 0x002D77EC)
        NiPoint3 w{F(double(u.y) * v.z - double(u.z) * v.y), F(double(u.z) * v.x - double(v.z) * u.x),
                   F(double(u.x) * v.y - double(u.y) * v.x)};   // esp+0x1C
        (void)ni::Unitize(w);
        rot.m[0][0] = v.x, rot.m[1][0] = v.y, rot.m[2][0] = v.z;   // 12 dwords from esp+0x50 into *rot
        rot.m[0][1] = u.x, rot.m[1][1] = u.y, rot.m[2][1] = u.z;
        rot.m[0][2] = -w.x, rot.m[1][2] = -w.y, rot.m[2][2] = -w.z;   // FCHS; FSTP
    }
    // 0x00058793: out of first person when jumping / falling / dead / climbing / hanging / in the air / held, and for
    // Leela and Bender while attacking (+0x506 / +0x507), in a special (+0x50A..+0x50C) or a roll (+0xB95..+0xB98)
    bool leave = IsJumpAnim() || IsFallAnim() || IsDead() || OnLadder() || OnLedge() || !OnGround() || heldBy;
    if (!leave && (type == 2 || type == 3))
        leave = attack1 || attack2 || special50A || special50B || special50C || roll98 || roll97 || roll96 || roll95;
    if (leave) {
        if (!firstPerson) return;
        LeaveFirstPerson();                           // 0x00054DA0(0)
    }
    if (!firstPerson || !cam) return;                 // +0xB88 (S+0x8C is not null-tested)
    // 0x0005888D: the field of view: Leela / Bender 90; else by the weapon's type (vf7C, 0 without one; table
    // 0x00058920): the railgun (3) 45, the others 90; a type above 4 leaves it
    if (type == 2 || type == 3) {
        cam->SetFov(90.0f);                           // 0x000110E0(0x42B40000)
        return;
    }
    const u32 wt = weapon ? weapon->WeaponType() : 0u;
    if (wt > 4) return;
    cam->SetFov(wt == 3 ? 45.0f : 90.0f);             // 0x42340000 / 0x42B40000
}

void Player::Walk(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vf100 0x00054AE0 (3.4.1)
    if (flag28 || firstPerson || IsDead() || !hasControl) return;
    if (cannon && cannon->IsActive()) return;          // (0x00054B24: P+0x7A8 && 0x00032E40)
    cdef.rotSpeed = 16.0f;                            // (the definition's value is overwritten every step)
    f32 rate = 16.0f;
    if (IsAttackAnim() && type == 2) rate = 2.6666667f;   // orig: 0x402AAAAB
    const bool slope = body && (body->shape.flags & 0x40);
    if (!(ladder || ledge) && slope && !(slopeTime < 0.25f)) {
        // sliding on a steep slope for 0.25 s: no steering
    } else {
        const f32 ts = F(double(g_frameStep) * rate);
        if (actions.tank) NotPorted("Player: tank steering (0x000534F0)");
        const NiPoint3 o = Steer(ts, rot);
        vel = {F(double(o.x) + vel.x), F(double(o.y) + vel.y), F(double(o.z) + vel.z)};   // orig: 0x00011500
    }
    if (!(ladder || ledge) && slope) {
        const double t = double(g_frameStep) + slopeTime;
        slopeTime = F(t);
        if (t > 0.25f) slopeTime = 0.25f;
    } else {
        slopeTime = 0;
    }
    if (body && body->supported) {
        airTime = 0;
    } else {
        const double t = double(g_frameStep) + airTime;
        airTime = F(t);
        if (t > 1.0f) airTime = 1.0f;
    }
    double len = ni::Unitize(vel);
    if (len > 1.0) len = 1.0;
    f32 speed;
    if (!(len > actions.walkThreshold)) speed = cdef.sneakSpeed;   // TEST AH,0x41; JP
    else if (len < actions.runThreshold) speed = F((double(cdef.runSpeed) - cdef.sneakSpeed) * len);
    else speed = cdef.runSpeed;
    const NiPoint3 fwd = ni::MulVector(rotation, kUnitX);
    const double dot = (double(fwd.y) * vel.y + double(fwd.z) * vel.z) + double(fwd.x) * vel.x;
    if (dot < -0.5f) speed = cdef.walkSpeed;          // orig: 0x00297414
    vel = {F(double(speed) * vel.x), F(double(speed) * vel.y), F(double(speed) * vel.z)};
}

NiPoint3 Player::Steer(f32 ts, ni::NiMatrix3& rot) {  // orig: 0x000536D0 (3.4.2)
    if (lockOn) ts = 120.0f;
    Camera* cam = level.camera.get();
    if (!cam || !cam->sceneCamera) return {};
    const ni::NiMatrix3& C = cam->sceneCamera->local.rotate;   // NiCamera +0x30
    NiPoint3 A = InlineMul(C, kUnitX);
    A.y = 0;
    NiPoint3 B = InlineMul(C, kUnitZ);
    B.y = 0;
    ni::Unitize(A);
    ni::Unitize(B);
    const f32 X = moveX, Y = moveY;
    const double xBx = double(B.x) * X;               // (kept on the x87 stack)
    const NiPoint3 d{F(double(F(double(A.x) * Y)) + xBx), F(double(F(double(A.y) * Y)) + F(double(X) * B.y)),
                     F(double(F(double(A.z) * Y)) + F(double(X) * B.z))};
    inputDir = d;
    NiPoint3 D = d;
    const f32 mag = F(std::sqrt((double(d.x) * d.x + double(d.z) * d.z) + double(d.y) * d.y));
    ni::Unitize(inputDir);
    const ni::NiMatrix3 R = rotation;                 // orig: vf38
    NiPoint3 Fw = InlineMul(R, kUnitX);
    NiPoint3 Rt = InlineMul(R, kUnitZ);
    ni::Unitize(Fw);
    ni::Unitize(Rt);
    const double l = ni::Unitize(D);
    double ang = 0.0;
    if (l > 0.0) {
        f32 c = F(double(Fw.z) * D.z + double(Fw.x) * D.x);
        const double s = double(Rt.z) * D.z + double(Rt.x) * D.x;
        if (c > 1.0f) c = 1.0f;
        else if (c < -1.0f) c = -1.0f;
        const double a = AcosClamped(c);
        ang = s > 0.0 ? a : -a;
    }
    f32 yaw = 0;
    if (ts != 0.0f) {
        double q = ang / ts;
        if (q > 1.5) {                                // orig: 0x00293BEC
            yaw = F(1.5 * ts);
        } else {
            if (q < -1.5) q = -1.5;
            yaw = F(q * ts);
        }
    }
    rot = ni::Multiply(R, ni::MakeYRotation(yaw));   // orig: MakeYRotation 0x001030B0, 0x00013B30
    NiPoint3 o{};
    if (!lockOn && !(std::fabs(yaw) < 0.45f)) return o;   // orig: 0x0029A4F0
    if (Y != 0.0f || X != 0.0f) {
        const NiPoint3 v = ni::MulVector(rot, kUnitX);
        o = {v.x, F(0.0 + v.y), F(0.0 + v.z)};
    }
    // (the strafe part needs |+0x4E0| > 0.1: always 0 for the player)
    return {F(double(o.x) * mag), F(double(o.y) * mag), F(double(o.z) * mag)};
}

void Player::TryJump() {                              // orig: vf104 0x00050D10 (3.5.1)
    if (ledge) return;
    if (ladder && !actions.MatchNewest(0x18)) return;
    if (ladderB91 || ledgeB8F || ladderB92 || locked || IsAnimPlaying(0x65)) return;
    const bool ok = (OnGround() && !firstPerson && !(body->velocity.y > 0.0f)) || ledge || ladder;
    if (!ok) return;
    if (body->shape.flags & 0x80) return;
    if (type != 4 && (IsJumpAnim() || IsFallAnim())) return;
    if (!actions.MatchNewest(0x18)) return;
    jumpRequest = 1;
    jumpPending = 0;
    // (type 4 && body vf48: body vf50((0, 50.0, 0)) at 0x00050D10: dead code, vf104 is only called from the normal
    //  vfFC branch, which a rodent with its ride (+0x7AC, every rodent) never takes)
    if (type == 4 && body->supported) NotPorted("Player: the rodent's jump impulse (without a ride)");
}

void Player::Recentre() {                             // orig: vf108 0x00050E80
    if (OnLadder() || OnLedge() || locked || IsAttackAnim()) return;
    if (actions.MatchNewest(0x1A) && level.camera && level.camera->mode == 0)
        level.camera->recentre = 1;                   // 0x000111A0 (camera byte +1)
}

void Player::Attack(bool second, f32 value, ni::NiMatrix3& rot) {   // orig: vf114 / vf118
    u8& held = second ? xHeld : aHeld;
    if (type == 3) {                                  // Bender 0x00080FD0 / 0x000811D0
        if (second) AttackBenderX(value);
        else AttackBenderA(value);
        return;
    }
    if (type == 1) {                                  // Fry 0x00084B90 / 0x00084C50
        if (second) AttackFryX(value, rot);
        else AttackFryA(value);
        return;
    }
    if (type == 2) {                                  // Leela 0x0008A190 / 0x0008A6B0
        if (second) AttackLeelaX(value);
        else AttackLeelaA(value);
        return;
    }
    // base 0x0004F910 / 0x0004F9D0 (the rodent)
    if (attacking504 || attacking505 || OnLadder() || OnLedge() || locked) return;
    if (value <= 0.1f) {
        held = 0;
        return;
    }
    if (!IsAttackAnim()) (second ? attack2 : attack1) = 1;
    if (!second && !IsAttackAnim()) {
        sinceAttack = 0;
        NotPorted("Player: the attack start time (+0xB68)");
    }
    held = 1;
}

void Player::FirstPersonToggle() {                    // orig: vf11C 0x00058940
    static u8 latch = 0;                              // orig: DAT_002DD85C
    if (!(actionBits & 0x20)) {
        latch = 0;
    } else {
        if (!IsJumpAnim() && !IsFallAnim() && !IsDead() && !OnLadder() && !OnLedge() && OnGround() && !locked &&
            !heldBy && !latch) {                      // (+0x7A0: 0x000589B0)
            if (!firstPerson) EnterFirstPerson();     // +0xB88: 0x00054DA0(1)
            else LeaveFirstPerson();                  // 0x00054DA0(0)
        }
        latch = 1;
    }
    // the eye: the camera at vf34 + (0, D+0xA0, 0) (0x00011120; S+0x8C is tested here)
    if (Camera* cam = level.camera.get(); cam && cam->mode == 3) {
        const f32 eye = cdef.fpsHeight;               // FLD D+0xA0; FSTP esp+0x14
        const NiPoint3 p = GetWorldPosition();        // vf34
        cam->SetPosition({p.x, F(double(eye) + p.y), p.z});
    }
}

void Player::EnterFirstPerson() {                     // orig: 0x00054DA0(1) (0x00054DB5..0x00054E00)
    if (level.camera) level.camera->EnterFirstPerson(this);   // S+0x8C (no null test): 0x00018050(P)
    if (view) view->SetActive(1);                     // +0x7B4: 0x00045ED0(1)
    if (type != 1 && level.hud) level.hud->SetActiveAll(false);   // 0x000E5A60(0) on S+0x118
    firstPerson = 1;                                  // +0xB88
}

// orig: 0x00054A80: vf34 with y = F(D+0xA0 + y) (x and z copied).
ni::NiPoint3 Player::EyePoint() const {
    const f32 eye = cdef.fpsHeight;                   // FLD D+0xA0; FSTP esp+4
    const NiPoint3 p = GetWorldPosition();            // vf34
    return {p.x, F(double(eye) + p.y), p.z};
}

// ---- aiming (docs/analysis/player_moves_port.md 4) -----------------------------------------------------------------
void Player::Aim(ni::NiMatrix3& rot) {                // orig: vfFC 0x00058324..0x000585B8
    if (type == 2 && (attack1 || attack2)) {          // 0x00058324 (Leela)
        const ni::NiMatrix3 saved = rotation;         // vf38 -> esp+0x80
        const double x = moveX, y = moveY;            // FLD +0x4DC; FLD +0x4D8
        Camera* cam = level.camera.get();
        if (x * x + y * y > 0.25f && cam && cam->sceneCamera) {   // FCOMP [0x00293B50]; TEST AH,0x41; JNE
            const ni::NiMatrix3& C = cam->sceneCamera->local.rotate;   // NiCamera +0x30 (as Steer)
            const NiPoint3 A = ni::MulVector(C, kUnitX), B = ni::MulVector(C, kUnitZ);   // 0x00011770 x2
            const f32 X = moveX, Y = moveY;
            const double bx = double(B.x) * X;        // (stays on the x87 stack)
            const f32 by = F(double(B.y) * X), bz = F(double(B.z) * X);
            const f32 ax = F(double(A.x) * Y), ay = F(double(A.y) * Y), az = F(double(A.z) * Y);
            const NiPoint3 d{F(double(ax) + bx), F(double(ay) + by), F(double(az) + bz)};
            const NiPoint3 p = GetWorldPosition();    // vf34
            TurnToPoint({F(double(d.x) + p.x), F(double(d.y) + p.y), F(double(d.z) + p.z)});   // 0x0003ABA0
        }
        f32 dot = 0;                                  // (written only by a successful search)
        Character* t = FindTarget(&dot, 0, false);    // 0x00053D60(&dot, 0, 0)
        if (t && dot > 0.25f) {                       // FCOMP [0x00293B50]; TEST AH,0x41; JNE
            aimPoint = t->CharacterPoint();           // vf78 -> +0xB04
            TurnToPoint(aimPoint);
            rot = rotation;                           // vf38, 12 dwords into *rot
        } else {
            SetRotation(saved);                       // vf24: the stick turn is undone; *rot keeps Steer's value
        }
    }
    if (type == 3 && attack1) {                       // 0x00058534 (Bender, A only)
        f32 dot = 0;
        Character* t = FindTarget(&dot, 0, false);
        if (t && dot > 0.75f) {                       // FCOMP [0x00293A94]; TEST AH,0x41; JNE
            aimPoint = t->CharacterPoint();           // +0xB04
            TurnToward(aimPoint, 0.5f);               // 0x0003AD90(&+0xB04, 0x3F000000)
            rot = rotation;
        }
    }
}

void Character::TurnToPoint(const NiPoint3& p) {      // orig: 0x0003ABA0
    const NiPoint3 me = GetWorldPosition();           // vf34
    NiPoint3 d{F(double(p.x) - me.x), 0.0f, F(double(p.z) - me.z)};   // (y = F(p.y - me.y) is stored, then 0)
    const double l = ni::Unitize(d);                  // 0x000116A0
    if (l < 9.999999747378752e-05 || l == 9.999999747378752e-05) {   // FCOMP [0x00293B1C]; TEST AH,5; JP skips
        const ni::NiMatrix3 R = rotation;             // vf38: the facing, row . UX ((y term + z term) + x term)
        d = {F((double(kUnitX.y) * R.m[0][1] + double(kUnitX.z) * R.m[0][2]) + double(kUnitX.x) * R.m[0][0]), 0.0f,
             F((double(kUnitX.y) * R.m[2][1] + double(kUnitX.z) * R.m[2][2]) + double(kUnitX.x) * R.m[2][0])};
        (void)ni::Unitize(d);                         // FSTP ST0 (the y dot is computed and overwritten with 0)
    }
    // e = UY x d with the zero products kept ([0x00293AB8] = 0.0)
    const double k = double(d.y) * 0.0;
    NiPoint3 e{F(double(d.z) - k), F(double(d.x) * 0.0 - double(d.z) * 0.0), F(k - d.x)};
    (void)ni::Unitize(e);                             // FSTP ST0
    ni::NiMatrix3 M;                                  // esp+0x30: columns d, (0, 1, 0), -e (pads 0)
    M.m[0][0] = d.x, M.m[1][0] = d.y, M.m[2][0] = d.z;
    M.m[0][1] = 0.0f, M.m[1][1] = 1.0f, M.m[2][1] = 0.0f;
    M.m[0][2] = -e.x, M.m[1][2] = -e.y, M.m[2][2] = -e.z;   // FCHS on the stored floats
    SetRotation(M);                                   // vf24
}

void Character::TurnToward(const NiPoint3& p, f32 rate) {   // orig: 0x0003AD90
    const NiPoint3 me = GetWorldPosition();           // vf34
    NiPoint3 d{F(double(p.x) - me.x), 0.0f, F(double(p.z) - me.z)};
    if (!(ni::Unitize(d) > 0.0)) return;              // FCOMP [0x00293AB8]; TEST AH,0x41; JNE -> 0x0003B075
    const ni::NiMatrix3 R = rotation;                 // vf38 (asked three times)
    NiPoint3 f{F((double(kUnitX.y) * R.m[0][1] + double(kUnitX.z) * R.m[0][2]) + double(kUnitX.x) * R.m[0][0]), 0.0f,
               F((double(kUnitX.y) * R.m[2][1] + double(kUnitX.z) * R.m[2][2]) + double(kUnitX.x) * R.m[2][0])};
    NiPoint3 r{F((double(kUnitZ.y) * R.m[0][1] + double(kUnitZ.z) * R.m[0][2]) + double(kUnitZ.x) * R.m[0][0]), 0.0f,
               F((double(kUnitZ.y) * R.m[2][1] + double(kUnitZ.z) * R.m[2][2]) + double(kUnitZ.x) * R.m[2][0])};
    (void)ni::Unitize(f);                             // FSTP ST0
    (void)ni::Unitize(r);
    f32 c = F(double(f.z) * d.z + double(f.x) * d.x); // FSTP [esp+0xC]
    const double s = double(r.z) * d.z + double(r.x) * d.x;   // (kept on the x87 stack)
    if (c > 1.0f) c = 1.0f;                           // TEST AH,0x41; JNE
    else if (c < -1.0f) c = -1.0f;                    // [0x00293BF0]; TEST AH,5; JP
    f32 a = F(s > 0.0 ? AcosClamped(c) : -AcosClamped(c));   // 0x00034100; FCHS when not > 0 (NaN: negative)
    if (!(rate == 0.0f)) {                            // TEST AH,0x44; JNP skips on == (NaN divides)
        double q = double(a) / rate;
        if (q > 1.5) q = 1.5;                         // FCOM [0x00293BEC]; TEST AH,0x41; JNE
        else if (q < -1.5) q = -1.5;                  // FCOM [0x00293BE8]; TEST AH,5; JP
        a = F(q * rate);
    }
    SetRotation(ni::Multiply(rotation, ni::MakeYRotation(a)));   // 0x001030B0, vf38, 0x00013B30, vf24
}

Character* Player::FindTarget(f32* dot, s32 skip, bool setAim) {   // orig: 0x00053D60
    const NiPoint3 me = CharacterPoint();             // vf78 (esp+0x54)
    NiPoint3 fwd;                                     // esp+0x30
    if (firstPerson) {                                // vf148 0x00050AD0 (+0xB88): the game camera's +0x70
        fwd = InlineMul(level.camera->rotation, kUnitX);   // (S+0x8C)+0x70, 12 dwords; (z + y) + x terms, each F
    } else {
        fwd = InlineMul(rotation, kUnitX);            // vf38 (a second vf38 follows, not read)
    }
    std::array<Character*, 200> cand{};               // esp+0xC0 (no bound check in the original)
    std::array<f32, 200> score{};                     // esp+0x3E0 (reused as the status array)
    s32 count = 0;                                    // esp+0x18
    for (Character* c : level.characters) {           // S->0x000C26E0 walk of S+0x90
        if (c == this || !c->IsTargetable()) continue;   // vfB0
        const NiPoint3 a = CharacterPoint(), b = c->CharacterPoint();   // vf78 (this first)
        NiPoint3 d{F(double(b.x) - a.x), F(double(b.y) - a.y), F(double(b.z) - a.z)};
        const f32 len = F(ni::Unitize(d));            // FSTP [esp+0x2C]
        if (std::fabs(double(d.y)) > 0.6f) continue;  // FCOMP [0x002976C0]; TEST AH,0x41; JE
        if (len > 100.0f) continue;                   // FCOMP [0x00295610]; TEST AH,0x41; JE
        if ((double(d.x) * fwd.x + double(fwd.y) * d.y) + double(d.z) * fwd.z < 0.25f) continue;   // TEST AH,5; JNP
        NiPoint3 h{d.x, 0.0f, d.z};
        (void)ni::Unitize(h);                         // FSTP ST0
        NiPoint3 f{fwd.x, 0.0f, fwd.z};
        (void)ni::Unitize(f);
        const double fh = (double(f.x) * h.x + double(f.y) * h.y) + double(f.z) * h.z;
        const double hd = (double(h.x) * d.x + double(h.y) * d.y) + double(h.z) * d.z;
        const double s = (1.0 - (fh * 0.85f + hd * 0.15f)) * len;   // [0x0029A268] / [0x00293AA8]; FSUBR [1.0]
        if (s > 3.4028234663852886e+38) continue;     // FCOM [0x00295604]; TEST AH,0x41; JE
        // sorted insert: before the first stored score the new one is below (FCOM; TEST AH,5; JNP)
        s32 i = 0;
        while (i < count && !(s < score[i])) ++i;
        for (s32 k = count; k > i; --k) cand[k] = cand[k - 1], score[k] = score[k - 1];
        score[i] = F(s);
        cand[i] = c;
        ++count;
    }
    if (count == 0) return nullptr;                   // (*dot is not written)
    std::array<s32, 200> status{};                    // REP STOSD over score[0..count) with EAX = 0
    s32 left = count, k = 0, toSkip = skip;           // esp+0x4C / EBX / esp+0x1C
    f32 dist = 0;                                     // esp+0x50
    NiPoint3 d;                                       // esp+0xC
    for (;;) {
        Character* c = cand[k];
        f32 len = 1.0f;                               // esp+0x2C
        if (setAim) {
            d = fwd;
            const NiPoint3 cp = c->CharacterPoint();  // vf78
            const double vx = double(cp.x) - me.x, vy = double(cp.y) - me.y, vz = double(cp.z) - me.z;
            const double l = std::sqrt((vx * vx + vz * vz) + vy * vy);   // FSQRT (x2 + z2 first, then y2)
            len = F(l);                               // FST [esp+0x2C]
            dist = F(l);                              // FSTP [esp+0x50]
        } else {
            const NiPoint3 cp = c->CharacterPoint();
            d = {F(double(cp.x) - me.x), F(double(cp.y) - me.y), F(double(cp.z) - me.z)};
            dist = len;                               // 1.0
        }
        if (status[k] == 0) {
            // DAT_002E9A7C vf10 0x000AE8D0(&me, &d, &len, 1): every physics object (vf0C without the exclusion)
            const phys::PhysObject* hit = level.physic ? level.physic->RayCastObjects(nullptr, me, d, len, true) : nullptr;
            if (hit != static_cast<const phys::PhysObject*>(c->body.get())) {   // != [c+0x5C]
                --left;
                status[k] = 1;
            }
            if (status[k] == 0) {
                const u8 old = ni::SetTriangleMask(0x20);   // SetTriMask 0x000F9D80
                const bool wall = static_cast<bool>(ni::RayCast(me, d, dist, true, 7));   // 0x000FB780(.., 1, 7)
                ni::SetTriangleMask(old);
                if (wall) {
                    --left;
                    status[k] = 1;
                }
            }
        }
        if (status[k] != 1) {
            status[k] = 2;
            if (toSkip == 0) break;                   // FOUND (0x0005431E)
            --toSkip;
        }
        if (++k == count) k = 0;
        if (left == 0) return nullptr;
    }
    // FOUND: the unit direction and the dot with fwd
    const double l = std::sqrt((double(d.x) * d.x + double(d.z) * d.z) + double(d.y) * d.y);
    double ux;                                        // (stays on the x87 stack)
    if (l > 9.999999974752427e-07) {                  // FCOM [0x00293ACC]; TEST AH,0x41; JNE
        const f32 inv = F(1.0 / l);                   // FDIV; FSTP [esp+0x1C]
        ux = double(inv) * d.x;
        d.y = F(double(inv) * d.y);
        d.z = F(double(d.z) * inv);
    } else {
        ux = 0.0;                                     // FLD [0x00293AB8]
        d.y = d.z = 0.0f;
    }
    *dot = F((double(fwd.x) * ux + double(fwd.y) * d.y) + double(d.z) * fwd.z);   // FSTP [arg 1]
    tapCount = k;                                     // +0xC34 = the chosen index (EBX)
    if (setAim)                                       // +0xB04 = me + unit * dist
        aimPoint = {F(ux * dist + me.x), F(double(dist) * d.y + me.y), F(double(F(double(d.z) * dist)) + me.z)};
    return cand[k];
}

// ---- the class hooks (docs/analysis/fry_leela_port.md) -------------------------------------------------------------
namespace {

// orig: 0x00011700 NiPoint3::Cross: each component a double difference of two float products, stored once.
NiPoint3 CrossX87(const NiPoint3& a, const NiPoint3& b) {
    return {F(double(a.y) * b.z - double(a.z) * b.y), F(double(a.z) * b.x - double(b.z) * a.x),
            F(double(a.x) * b.y - double(a.y) * b.x)};
}

// A matrix built on the stack from three columns.
ni::NiMatrix3 FromColumns(const NiPoint3& c0, const NiPoint3& c1, const NiPoint3& c2) {
    ni::NiMatrix3 m;
    const NiPoint3* c[3] = {&c0, &c1, &c2};
    for (int k = 0; k < 3; ++k) {
        m.m[0][k] = c[k]->x;
        m.m[1][k] = c[k]->y;
        m.m[2][k] = c[k]->z;
    }
    return m;
}

s32 s_leelaHitsA = 0;                                 // orig: DAT_002E3880 (shared by all instances)
s32 s_leelaHitsX = 0;                                 // orig: DAT_002E387C

// orig: `if (v && Voice_Exists(v)) { Voice_Stop(v); v = 0; }` (0x000C9900 / 0x000C9B10; Fry's charge voices).
void StopVoice(LevelData& level, u32& v) {
    if (!v || !level.Sound().VoiceExists(static_cast<s32>(v))) return;
    level.Sound().StopVoice(static_cast<s32>(v));
    v = 0;
}

constexpr s32 kNoClimbAnims[] = {0xD1, 0xD2, 0xBB, 0xBD, 0xBC, 0xB8, 0xB6, 0x61, 0x60, 0x5E, 0x5F};

// The HUD's weapon box (S+0x118): 0x000E5BC0("weapons", "select", W's definition name (W+0x10 +0x10) or "") and
// 0x000E5B90("weapons", "ammo", (float)ammo[W+0x74] (FILD), or -1 without W or for slot -1) (0x0029A45C / 0x0029A464 /
// 0x00295254 / 0x00297214).
void ShowWeaponBox(LevelData& level, const Weapon* W, const std::array<s32, 5>& ammo) {
    if (!level.hud) return;
    level.hud->Command("weapons", "select", W ? W->def->name.c_str() : "");
    const s32 slot = W ? W->ammoSlot : -1;
    level.hud->Command("weapons", "ammo", slot != -1 ? static_cast<f32>(ammo[static_cast<size_t>(slot)]) : -1.0f);
}

}  // namespace

// ---- the ladder / ledge scans (docs/analysis/player_moves_port.md 7 / 8) -------------------------------------------
// vf188 scans the stages' "Ladders" records (+0xB8C ladder, +0xB91 climbing off at the top, +0xB92 getting on from
// above); vf18C the edge sets (+0xB8D ledge, +0xB8F hanging, +0xB90 grabbing). Neither draws rand() or allocates.
void Player::LadderCheck(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vf188 0x00055F00
    const NiPoint3 Z{};                               // dword copies of NiPoint3::ZERO (0x002F915C)
    ladderSpeed = 0;                                  // +0xB40
    ladderGate = F(double(g_frameStep) + ladderGate); // +0xBE4
    if (IsDead() || ledge) return;                    // vfA8; +0xB8D
    if (ladderGate < 1.0f) return;                    // FCOMP [0x00293A98]; TEST AH,5; JNP (NaN goes on)
    for (s32 k : kNoClimbAnims)
        if (IsAnimPlaying(k)) return;
    (void)GetWorldPosition();                         // vf34 -> esp+0x174, not read
    ladderGate = 1.0f;
    const ni::NiMatrix3 saved = rot;                  // REP MOVSD from *rot (esp+0x140)
    f32 range = 3.0f;                                 // esp+0x1C
    if (weapon)                                       // 0x0005605C: by W->vf7C() (jump table 0x0005697C)
        switch (weapon->WeaponType()) {
        case 0: range = 3.0f; break;
        case 1: case 2: range = 4.0f; break;          // 0x40800000
        case 3: range = 5.0f; break;                  // 0x40A00000
        case 4: range = 4.25f; break;                 // 0x40880000
        default: break;
        }
    if (ladderB91) {                                  // +0xB91: climbing off at the top
        rot = rotation;                               // vf38
        vel = Z;
        ladder = 1;
        return;
    }
    if (ladderB92) {                                  // +0xB92: getting on from above
        rot = rotation;
        vel = Z;
        if (finished && IsAnimPlaying(0xF7)) ladderB92 = 0;   // byte +0x9B; onto_ladder
        ladder = 1;
        return;
    }
    for (const auto& st : level.stages) {             // Session::NextStage 0x000C26C0 from 0
        for (const StageLadder& r : st->ladders) {    // stage +0x70 count, +0x74 records of 0x1C bytes
            rot = saved;                              // restored at the top of every record
            const f32 A = F(double(r.v1) - (double(heightOffset) + 2.7f));   // [rec+0x14]; +0x4D0; [0x0029A518]
            if (!r.active) continue;                  // byte rec+0x18
            if (A < 0.0f) continue;                   // FCOMP 0.0; TEST AH,5; JNP (NaN goes on)
            const NiPoint3 P = r.position;            // esp+0x28
            NiPoint3 Q = r.position;                  // esp+0x10
            NiPoint3 u = inputDir;                    // +0xB1C (esp+0x64)
            const NiPoint3 me = GetWorldPosition();   // vf34 (esp+0x3C)
            if (double(P.y) - 0.4f > me.y) continue;  // FSUB [0x00293B28]; FCOMP; TEST AH,0x41; JE
            if (((double(heightOffset) + 2.7f) + P.y) + A < me.y) continue;   // TEST AH,5; JNP
            rot = ni::MakeYRotation(r.yaw);           // 0x001030B0 on *rot (rec+0x0C)
            const NiPoint3 nrm = ni::MulVector(rot, kUnitX);   // 0x00011770 with (1, 0, 0) on the stack
            (void)ni::Unitize(u);                     // FSTP ST0
            const NiPoint3 v{F(double(P.x) - me.x), 0.0f, F(double(P.z) - me.z)};
            const double sq = (double(v.x) * v.x + double(v.y) * v.y) + double(v.z) * v.z;   // 0x00011680
            if (!(double(range) * range > sq)) continue;   // FLD r; FMUL r; FCOMPP; TEST AH,0x41; JNE
            const NiPoint3 off = ni::MulVector(rot, {cdef.boundDepth, 0.0f, 0.0f});   // def+0x98
            Q = {F(double(P.x) - off.x), F(double(P.y) - off.y), F(double(P.z) - off.z)};
            f32 dot = F((double(u.z) * nrm.z + double(u.x) * nrm.x) + double(u.y) * nrm.y);   // esp+0x20
            if (std::fabs(std::sqrt((double(u.x) * u.x + double(u.y) * u.y) + double(u.z) * u.z)) < 0.1f)
                dot = 0;                              // 0x00011650; FABS; FCOMP [0x00293AB0]; TEST AH,5; JP
            if (jumpRequest && ladder) {              // 0x00056405: jump off backwards
                vel = Z;
                body->velocity.x = F(double(nrm.x) * 5.0f);   // [0x00293C24]
                body->velocity.y = 2.0f;              // 0x40000000
                body->velocity.z = F(double(nrm.z) * 5.0f);
                ladder = 0;
                jumpRequest = 0;
                b93 = b94 = 0;
                const NiPoint3 a{F(double(nrm.x) * -1.0f), F(double(nrm.y) * -1.0f), F(double(nrm.z) * -1.0f)};
                const NiPoint3 c = CrossX87(kUnitY, a);   // 0x00011700, this = a UY copy
                rot = FromColumns(a, kUnitY, {-c.x, -c.y, -c.z});   // FCHS on the stored floats
                ladderGate = 0;
            } else if (dot > 0.75f || b93) {          // 0x0005677A: up (FCOMP [0x00293A94]; TEST AH,0x41; JE)
                const bool on = ladder;               // CL
                if (!on && double(P.y) + A < me.y) continue;    // TEST AH,5; JNP -> 0x00056923 (ladder 0: the next record)
                if (!b93 && !b94) b93 = 1;            // (b94 = 0 again)
                if (b93) {
                    ladderSpeed = cdef.ladderClimbSpeed;   // def+0x80 (FST)
                    const double y = double(cdef.ladderClimbSpeed) + me.y;
                    Q.y = F(y);                       // FST [esp+0x14]
                    const f32 topY = F(double(P.y) + A);   // FSTP [esp+0x20]
                    if (!(y >= topY) || !on) {              // FCOMP; TEST AH,1; JNE (< or NaN), then TEST CL
                        SetPosition(Q);               // vf1C
                    } else {                          // 0x00056802: at the top
                        Q.y = topY;
                        rot = rotation;               // vf38 into *rot
                        if (type == 1) {              // Fry: 0.75 back along his facing
                            const NiPoint3 f = ni::MulVector(rot, kUnitX);
                            const double fx = double(f.x) * 0.75f, fy = double(f.y) * 0.75f;   // [0x00293A94]
                            const f32 fz = F(double(f.z) * 0.75f);
                            Q = {F(double(Q.x) - fx), F(double(Q.y) - fy), F(double(Q.z) - fz)};
                        }
                        SetPosition(Q);
                        ladderB91 = 1;
                    }
                    ladder = 1;
                }
                vel = Z;
            } else if (!(dot < -0.75f) && !b94) {     // [0x0029A514]; TEST AH,5; JNP: the stick neither up nor back
                if (!ladder) continue;
                Q.y = GetWorldPosition().y;           // vf34 again: hold the height
                vel = Z;
                SetPosition(Q);
            } else if (ladder) {                      // 0x00056610: back (or sliding): down
                if (!b94) {
                    b94 = 1;
                    b93 = 0;
                }
                ladderSpeed = -cdef.ladderClimbSpeed; // FLD; FCHS; FST +0xB40
                const double y = -double(cdef.ladderClimbSpeed) + me.y;
                Q.y = F(y);                           // FST [esp+0x14]
                if (!(y < P.y || y == P.y)) {         // FCOMP [esp+0x2C]; TEST AH,0x41; JP (> or NaN)
                    SetPosition(Q);
                    ladder = 1;
                } else {                              // the bottom: let go
                    ladder = 0;
                    ladderGate = 0;
                }
                vel = Z;
            } else {                                  // 0x000566CD: not on it: getting on from above
                if (!SeqById(0xF7)) continue;         // onto_ladder (-> 0x00056923 with ladder 0)
                Q.y = me.y;
                b93 = b94 = 0;
                const double topY = double(P.y) + A;
                if (topY - 1.0 < me.y || topY - 1.0 == me.y) {   // FSUBR [1.0]; FCOMP; TEST AH,0x41; JP (> / NaN skip)
                    Q.y = F(topY);
                    SetPosition(Q);
                    ladder = 1;
                    ladderB92 = 1;
                    vel = Z;
                }
            }
            if (ladder) return;                       // 0x00056923 (*rot keeps this record's value)
        }
    }
    ladder = 0;                                       // 0x0005696C
}

void Player::LedgeCheck(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vf18C 0x00056990
    const NiPoint3 Z{};
    (void)GetWorldPosition();                         // vf34 -> esp+0x18C, not read
    f32 range = 3.0f;                                 // esp+0x14
    if (weapon && weapon->WeaponType() <= 4) range = 4.0f;   // 0x000569AB (table 0x000574D0: 4.0 for 0..4)
    ledgeGate = F(double(g_frameStep) + ledgeGate);   // +0xBE8
    auto leave = [&] { ledge = ledgeB90 = ledgeB8F = 0; };   // 0x000574B0 (+0xB70 kept)
    if (IsDead() || ladder) return leave();           // vfA8; +0xB8C
    if (ledgeGate < 1.0f) return leave();             // TEST AH,5; JNP (NaN goes on)
    for (s32 k : kNoClimbAnims)
        if (IsAnimPlaying(k)) return leave();
    if (ledgeB8F) {                                   // +0xB8F: hanging (0x00056AF7)
        ledgeB90 = 0;
        ledge = 1;
        vel = Z;
        return;
    }
    ledgeGate = 1.0f;
    for (const auto& set : level.edgeSets) {          // DAT_002EAF0C, the list +0 next, +4 edges, +8 count
        for (const Edge& e : set->edges) {
            if (!e.active) continue;                  // +0x20
            NiPoint3 u = inputDir;                    // +0xB1C (esp+0xAC)
            (void)ni::Unitize(u);                     // FSTP ST0
            const NiPoint3 a = e.p0, b = e.p1;        // esp+0x5C / esp+0x6C
            if (a.x == b.x && a.y == b.y && a.z == b.z) continue;   // 0x000114C0 (FCOMP x3; TEST AH,0x44; JP)
            const NiPoint3 me = GetWorldPosition();   // vf34 (esp+0x28)
            const f32 top = F(double(me.y) + 9.0f);   // [0x00295654] (esp+0x10)
            f32 hi, lo;                               // esp+0x34 / esp+0x44
            if (a.y < b.y) {                          // FCOM; TEST AH,5; JP
                lo = a.y;
                hi = b.y;
            } else {
                hi = a.y;
                lo = b.y;
            }
            if (body->velocity.y > 0.0f) continue;    // body +0x2C; TEST AH,0x41; JE
            const double kt = ((type == 2 || type == 1 || type == 3) ? 2.0 : 0.0) + top;   // [0x00293B20]
            if (kt > hi && double(top) - cdef.ledgeGrabDistance > hi) continue;   // def+0xA4; TEST AH,0x41; JE
            if (kt < lo && double(top) - cdef.ledgeGrabDistance < lo) continue;   // TEST AH,5; JNP
            NiPoint3 m = GetWorldPosition();          // vf34 again (esp+0x48)
            // the parameter of m on a->b in xz (one x87 chain; dx is stored, the squares use the unrounded value)
            const double dz = double(b.z) - a.z, dxu = double(b.x) - a.x;
            const f32 dx = F(dxu);                    // FST [esp+0x10] (top is overwritten)
            const double num = (double(m.z) - a.z) * dz + (double(m.x) - a.x) * dx;
            const double den = std::fabs(dxu * dxu + dz * dz);
            const f32 t = F(num / den);               // esp+0x24
            if (t < 0.0f) continue;                   // TEST AH,5; JNP
            if (t > 1.0f) continue;                   // TEST AH,0x41; JE
            const NiPoint3 v{dx, 0.0f, F(dz)};        // esp+0x114
            const NiPoint3 tv{F(double(t) * v.x), F(double(t) * v.y), F(double(t) * v.z)};   // 0x000115C0 (cdecl)
            const NiPoint3 S{F(double(a.x) + tv.x), F(double(a.y) + 1.5f), F(double(a.z) + tv.z)};   // [0x00293BEC]
            m.y = S.y;                                // (FST [esp+0x4C]: m.y's slot keeps S.y)
            NiPoint3 n{F(double(m.x) - S.x), 0.0f, F(double(m.z) - S.z)};   // esp+0x18
            (void)ni::Unitize(n);
            NiPoint3 Q = S;                           // esp+0x28
            const NiPoint3 w{F(double(S.x) - m.x), 0.0f, F(double(S.z) - m.z)};
            const double sq = (double(w.x) * w.x + double(w.y) * w.y) + double(w.z) * w.z;   // 0x00011680
            if (double(range) * range < sq) continue; // FCOMPP; TEST AH,5; JNP
            const f32 d = F((double(u.z) * n.z + double(u.y) * n.y) + double(u.x) * n.x);   // esp+0x24
            const NiPoint3 c0{F(double(n.x) * -1.0f), F(double(n.y) * -1.0f), F(double(n.z) * -1.0f)};
            const NiPoint3 cr = CrossX87(kUnitY, c0); // 0x00011700 (this = a UY copy at esp+0xA0)
            rot = FromColumns(c0, kUnitY, {-cr.x, -cr.y, -cr.z});   // every edge in range writes *rot
            if (type == 2) {                          // the hang position: 0.5 out from the edge ([0x00293AA0])
                Q = {F(double(n.x) * 0.5f + S.x), F(double(n.y) * 0.5f + S.y), F(double(F(double(n.z) * 0.5f)) + S.z)};
            } else if (type == 3) {                   // 1.2 out ([0x0029A520])
                Q = {F(double(n.x) * 1.2f + S.x), F(double(n.y) * 1.2f + S.y), F(double(F(double(n.z) * 1.2f)) + S.z)};
            } else if (type == 1) {                   // 1.0 out
                Q = {F(double(S.x) + n.x), F(double(n.y) + S.y), F(double(n.z) + S.z)};
            }
            Q.y = F(double(hi) - (type == 2 ? 9.6f : cdef.ledgeGrabHeight));   // [0x0029A51C] / def+0xA8
            const NiPoint3 cp = CharacterPoint();     // vf78
            f32 h = F(std::fabs((double(cp.y) - lo) - 0.1f));   // [0x00293AB0] (esp+0x84)
            const NiPoint3 cp2 = CharacterPoint();    // vf78 again (esp+0x144)
            if (h < 10.0f) {                          // [0x00293C2C]; TEST AH,5; JP
                if (ni::RayCast(cp2, kUnitY, h, false, 3)) continue;   // 0x000FB780: something above the head
            }
            if (jumpRequest && ledge) {               // 0x000571F2: jump off the ledge
                body->velocity.x = F(double(n.x) * 10.0f);   // [0x00293C2C]
                jumpRequest = 0;
                ledge = ledgeB90 = ledgeB8F = 0;
                ledgeGate = 0;
                body->velocity.y = 2.0f;
                body->velocity.z = F(double(n.z) * 10.0f);
                rot = FromColumns(c0, kUnitY, {-cr.x, -cr.y, -cr.z});   // recomputed from the same values
            } else if (d < -0.5f) {                   // [0x00297414]; TEST AH,5; JP: pushing at the edge: grab
                if (!ledge) {
                    ledgeTimerB = 0;                  // +0xB74
                    ledgeB90 = 1;
                    ledgeB8F = 0;
                }
                ledge = 1;
                ledgeTimerA = 0;                      // +0xB70
                const double tb = double(g_frameStep) + ledgeTimerB;
                ledgeTimerB = F(tb);                  // FST
                if (tb > 0.2f && (!IsAnimPlaying(0x24) || finished)) {   // [0x00293AB4]; ledgegrab
                    ledgeB90 = 0;
                    ledgeB8F = 1;
                }
            } else {
                ledgeTimerB = 0;
                if (d > 0.5f) {                       // [0x00293AA0]: pulling away
                    if (!ledge) continue;
                    const double ta = double(g_frameStep) + ledgeTimerA;
                    ledgeTimerA = F(ta);
                    if (ta > 0.2f) {                  // let go
                        ledgeB90 = 0;
                        ledge = 0;
                        ledgeGate = 0;
                        body->velocity.y = -25.0f;    // 0xC1C80000
                    }
                } else {
                    ledgeTimerA = 0;
                }
            }
            if (!ledge) continue;                     // 0x0005742E
            if (!ledgeB8F) SetPosition(Q);            // vf1C
            vel = Z;
            return;
        }
    }
    ledgeTimerA = 0;                                  // 0x000574AA
    leave();
}

bool Player::IsLandAnim() const {                     // orig: vf144 0x00050220
    static constexpr s32 kIds[] = {0x1D, 0x23, 0x20, 0x65, 0x78, 0x7E, 0x7F, 0x9A, 0xA0, 0xA1};
    for (s32 k : kIds)
        if (IsAnimPlaying(k)) return true;
    return false;
}

bool Player::WeaponOverlayPlaying() const {           // orig: vf128 0x00050F00
    if (!weapon || !weapon->def->IsType(8) || !overlaySeq) return false;   // +0x528, vf0C(8), +0xB0
    for (u32 i = 1; i <= 4; ++i)                      // FindSequence 0x00039BA0 of W->vf78(1..4)
        if (overlaySeq == FindSequence(weapon->AnimName(i))) return true;
    return overlaySeq == SeqById(0x8B) || overlaySeq == SeqById(0x8C);
}

// ---- Fry's weapon code (docs/analysis/inventory_batch1_port.md 5.3-5.9; checked against the assembly of 0x00035F70,
// 0x00050560, 0x00050AF0, 0x00083330, 0x00083380, 0x00083570, 0x00084EF0, 0x00083860 and 0x000836F0) --------------
bool Player::SetWeapon(Weapon* W) {                   // orig: vfB4 0x00035F70
    ni::NiAVObject* hand = GetObjectByName(node.Get(), "R_HAND");   // node vf54(0x00295A88)
    auto* handNode = dynamic_cast<ni::NiNode*>(hand);
    if (!handNode) return false;                      // (the original treats the found object as a node)
    if (weapon) {                                     // +0x528: put the old one away
        if (weapon->def->IsType(8)) (void)weapon->PutAway();   // vf60
        (void)handNode->DetachChild(weapon->GetNode());   // hand vfA4(&tmp, vf30()); the temporary is released
        weapon = nullptr;
    }
    if (!W) return true;
    weapon = W;
    if (W->GetNode()) {                               // vf30
        handNode->AttachChild(W->GetNode(), true);    // vfA0(node, 1)
        (void)W->TakeOut();                           // vf64
    }
    return true;
}

void Player::UseAmmo(s32 slot, s32 n) {               // orig: vf90 0x00050560
    if (slot < 0 || slot >= static_cast<s32>(ammo.size())) return;   // (port guard: the slots 0..4 only)
    ammo[static_cast<size_t>(slot)] -= n;
    if (ammo[static_cast<size_t>(slot)] < 0) ammo[static_cast<size_t>(slot)] = 0;   // SUB; JNS
}

void Player::SetAmmo(s32 slot, s32 n) {               // orig: 0x00051D50
    if (slot < 0 || slot >= static_cast<s32>(ammo.size())) return;   // (port guard: FsQuickLoad passes 0..4)
    ammo[static_cast<size_t>(slot)] = n;              // +0xAD0 + 4 slot
    // 0x00051D5F: +0x524 && its +0x74 != -1: the HUD S+0x118 0x000E5B90("weapons", "ammo", (float)ammo[+0x74])
    if (pendingWeapon && pendingWeapon->ammoSlot != -1 && level.hud)
        level.hud->Command("weapons", "ammo", static_cast<f32>(ammo[static_cast<size_t>(pendingWeapon->ammoSlot)]));
}

void Player::ClearInventory() {                       // orig: 0x00050580
    (void)SetWeapon(nullptr);                         // vfB4(0)
    FinishTransitions();                              // 0x000363F0
    attacking505 = attacking504 = 0;                  // +0x505 / +0x504
    pendingWeapon = nullptr;                          // +0x524
    while (GameObject* o = inventory.firstObject) delete o;   // while P+0x3C: vf00(1) (each unlinks itself)
}

s32 Player::GetAmmo(s32 slot) const {                 // orig: vf94 0x00050AF0
    if (slot < 0 || slot >= static_cast<s32>(ammo.size())) return 0;   // (port guard)
    return ammo[static_cast<size_t>(slot)];
}

bool Player::IsChargingAnim() const {                 // orig: 0x00083330
    return IsAnimPlaying(0x90) || IsAnimPlaying(0x83) || IsAnimPlaying(0xA5) || IsAnimPlaying(0xA7);
}

bool Player::IsChargeAnimPlaying() const {            // orig: 0x00083380
    return IsChargingAnim() || IsAnimPlaying(0x91) || IsAnimPlaying(0x84) || IsAnimPlaying(0xA6) || IsAnimPlaying(0xA8);
}

bool Player::RequestFire(u32 kind) {                  // orig: 0x00083570 (kind 1 = A, 2 = X)
    if (WeaponOverlayPlaying() || !weapon || !weapon->def->IsType(8)) return false;   // vf128
    const u32 t = weapon->WeaponType();               // vf7C
    if (kind != 1 && kind != 2) return false;
    if (t == WeaponDef::kHammer) {
        if (kind == 1) {
            ++kickCount;                              // +0xC00
            attack1 = 1;                              // +0x506
        } else {
            ++punchCount;                             // +0xC04
            attack2 = 1;                              // +0x507
        }
        return true;
    }
    bool have;
    switch (t) {
    case WeaponDef::kPistol: have = GetAmmo(0) >= 1; break;   // vf94(0)
    case WeaponDef::kShotgun: have = GetAmmo(1) >= 5; break;
    case WeaponDef::kRailgun: have = GetAmmo(2) >= 1; break;
    case WeaponDef::kMachineGun: have = GetAmmo(3) >= 1; break;
    default: return false;
    }
    if (have) {
        (kind == 1 ? attack1 : attack2) = 1;
        return true;
    }
    const WeaponDef& wd = static_cast<const WeaponDef&>(*weapon->def);
    if (!level.Sound().HasVoices(wd.clickSound))      // the click: 0x000836AC / 0x000836D2
        (void)level.Sound().Play3D(wd.clickSound, GetWorldPosition(), false);
    return false;
}

void Player::FireKey() {                              // orig: 0x00084EF0 (5.5)
    Weapon* W = weapon;                               // +0x528 (esp+0xAC)
    if (!W) return;
    const NiPoint3 up{0.0f, 1.0f, 0.0f};              // dwords of 0x002D77EC (esp+0x3C)
    ni::NiMatrix3 R;                                  // esp+0x70 (pads zeroed at entry)
    NiPoint3 m, dir;                                  // esp+0x54 (the muzzle), esp+0x20 (the direction)
    AiNoise(level, GetWorldPosition(), 1.0f);         // (S+0x114)->0x00031420(vf34, 1.0)
    auto side = [&](const NiPoint3& d) {              // s = unit(up x d) (esp+0x2C)
        NiPoint3 s{F(double(d.z) * up.y - double(up.z) * d.y), F(double(up.z) * d.x - double(d.z) * up.x),
                   F(double(up.x) * d.y - double(up.y) * d.x)};
        (void)ni::Unitize(s);
        return s;
    };
    if (firstPerson) {                                // +0xB88
        NiPoint3 e = GetWorldPosition();              // vf34 -> esp+0x48 (a copy at esp+0x54 for the muzzle)
        e.y = F(double(e.y) + cdef.fpsHeight);        // D+0xA0
        const NiPoint3 f = ni::MulVector(level.camera->rotation, {1.0f, 0.0f, 0.0f});   // camera +0x70 (a copy) * UNIT_X
        const NiPoint3 farP{F(double(e.x) + double(f.x) * 20.0f), F(double(f.y) * 20.0f + e.y),   // [0x00295AB4]
                            F(double(e.z) + double(F(double(f.z) * 20.0f)))};
        const NiPoint3 o = ni::MulVector(level.camera->sceneCamera->world.rotate, {0.0f, -1.0f, 1.0f});   // NiCamera +0x70
        m = {F(double(e.x) + double(o.x) * 0.5f), F(double(o.y) * 0.5f + e.y),   // [0x00293AA0]
             F(double(F(double(o.z) * 0.5f)) + e.z)};
        f32 t = 1000.0f;                              // esp+0x38 = 0x447A0000
        const ni::RayHit hit = ni::RayCast(e, f, t, true, 1);   // 0x000FB780(&e, &f, &t, 1, 1)
        if (hit) hitTip = {F(double(e.x) + double(f.x) * t), F(double(f.y) * t + e.y),
                           F(double(e.z) + double(F(double(f.z) * t)))};   // P+0x5D8
        if (lockOn) {                                 // +0xB00
            NiPoint3 d{F(double(aimPoint.x) - e.x), F(double(aimPoint.y) - e.y), F(double(aimPoint.z) - e.z)};
            if (!(ni::Unitize(d) > t)) hitTip = aimPoint;   // FCOMP t; TEST AH,0x41; JE: keep the hit when farther
        } else if (!hit) {
            hitTip = farP;
        }
        dir = {F(double(hitTip.x) - m.x), F(double(hitTip.y) - m.y), F(double(hitTip.z) - m.z)};
        (void)ni::Unitize(dir);
    } else {
        UpdateNodeAt(node.Get(), animTime);           // P+0x2C Update(+0x78, 1)
        m = W->TipPosition();                         // vf80
        dir = ni::MulVector(GetWorldRotation(), {1.0f, 0.0f, 0.0f});   // vf38 * UNIT_X
        if (lockOn) {
            NiPoint3 d{F(double(aimPoint.x) - m.x), F(double(aimPoint.y) - m.y), F(double(aimPoint.z) - m.z)};
            if (auto* lt = static_cast<Character*>(lockTarget)) {   // +0xC3C: lead the target
                const f32 len = F(ni::Unitize(d));    // FSTP [esp+0x38]
                const NiPoint3 v = lt->body ? lt->body->velocity : NiPoint3{};   // body vf0C 0x000AED00
                constexpr double c = double(0.0166666675f);   // [0x0029E430]
                const f32 qx = F(double(F(double(v.x) * len * c)) + aimPoint.x);
                const f32 qy = F(double(v.y) * len * c + aimPoint.y);
                const double qz = double(F(double(v.z) * len)) * c + aimPoint.z;
                d = {F(double(qx) - m.x), F(double(qy) - m.y), F(qz - m.z)};
            }
            NiPoint3 u = d;                           // esp+0x48
            (void)ni::Unitize(u);
            dir.y = u.y;                              // the pitch only
            if (std::fabs(double(u.y)) > 0.6f)        // [0x002976C0]; TEST AH,0x41; JNE
                dir.y = u.y < 0.0f ? -0.6f : 0.6f;    // 0xBF19999A / 0x3F19999A (TEST AH,5; JNP)
            if (ni::Unitize(dir) == 0.0f) dir = ni::MulVector(GetWorldRotation(), {1.0f, 0.0f, 0.0f});   // TEST AH,0x44; JP
        }
    }
    NiPoint3 s = side(dir);
    // (the first frame (dir | s x dir | -s) is written to esp+0x70 here and overwritten below)
    const f32 ux = F(double(s.y) * dir.z - double(s.z) * dir.y);   // the FST copies (esp+0x14 / +0x18 / +0x1C)
    const f32 uy = F(double(s.z) * dir.x - double(dir.z) * s.x);
    const f32 uz = F(double(s.x) * dir.y - double(s.y) * dir.x);
    // the spread: two CRT draws (0x000852FC / 0x00085314 in first person, 0x000857B5 / 0x000857CD else)
    const s32 ra = 50 - crt::Rand() % 100;            // CDQ; IDIV 100; 50 - remainder
    const s32 rb = 50 - crt::Rand() % 100;
    constexpr double k = double(0.000833333354f);     // [0x00297238]
    const double ka = double(ra) * fireHold * k;      // FILD; FMUL +0xD60; FMUL k (the first draw: along u)
    const double kb = double(rb) * fireHold * k;      // (the second draw: along s)
    dir = {F(double(F(double(ux) * ka + dir.x)) + double(F(double(s.x) * kb))),
           F((double(uy) * ka + dir.y) + double(F(double(s.y) * kb))),
           F(double(F(double(F(double(uz) * ka)) + dir.z)) + kb * s.z)};
    (void)ni::Unitize(dir);
    s = side(dir);                                    // (FSTP ST0 at 0x0008590D)
    R.m[0][0] = dir.x, R.m[1][0] = dir.y, R.m[2][0] = dir.z;   // chunk 0 = dir
    R.m[0][1] = F(double(s.y) * dir.z - double(s.z) * dir.y);  // chunk 1 = s x dir
    R.m[1][1] = F(double(s.z) * dir.x - double(dir.z) * s.x);
    R.m[2][1] = F(double(s.x) * dir.y - double(s.y) * dir.x);
    R.m[0][2] = -s.x, R.m[1][2] = -s.y, R.m[2][2] = -s.z;      // chunk 2 = -s (FCHS)
    if (IsPlayingName(W->AnimName(1)) || IsPlayingName(W->AnimName(3))) {   // attack 1 / moving attack 1
        ++shots;                                      // +0xBF8
        (void)weapon->Fire(m, R, 0, 0.0f);            // (+0x528) vf70(&m, &R, 0, 0)
        if (W->WeaponType() == WeaponDef::kRailgun) rumble.Add(0.5f, 0.5f, 0.3f, 0);
        return;
    }
    if (!IsPlayingName(W->AnimName(2)) && !IsPlayingName(W->AnimName(4))) return;
    f32 c = chargeTime;                               // +0xD64 (FST [esp+0x38])
    if (c > 2.0f) c = 2.0f;                           // [0x00293B20]; TEST AH,0x41; JNE
    ++shots;
    if (!firstPerson) {
        UpdateNodeAt(node.Get(), animTime);
        if (W->WeaponType() == WeaponDef::kMachineGun) R = W->TipRotation();   // vf84 (12 dwords)
        m = W->TipPosition();                         // vf80
    }
    (void)weapon->Fire(m, R, 1, c);                   // (+0x528 re-read) vf70(&m, &R, 1, c)
    if (W->WeaponType() == WeaponDef::kShotgun || W->WeaponType() == WeaponDef::kRailgun) {
        const f32 r = F((double(c) * 0.5f + 1.0f) * 0.5f);   // FMUL 0.5; FADD 1.0; FMUL 0.5
        rumble.Add(r, r, 0.3f, 0);
    }
}

void Player::CycleWeapon(bool next) {                 // orig: vfE4 0x00083860 (next) / 0x000836F0 (previous)
    pendingWeapon = weapon;                           // +0x524
    Weapon* slots[5] = {};                            // hammer, pistol, shotgun, machine gun, railgun
    static constexpr s32 kSlot[5] = {0, 1, 2, 4, 3};  // jump tables 0x000839A8 / 0x0008382C: type -> slot
    s32 cur = 4;
    if (weapon && weapon->WeaponType() <= 4) cur = kSlot[weapon->WeaponType()];
    for (GameObject* o = inventory.NextObject(nullptr); o; o = inventory.NextObject(o)) {   // P+0x38, 0x00097670
        if (!o->def->IsType(8)) continue;
        auto* w = dynamic_cast<Weapon*>(o);
        if (!w) continue;                             // (the original treats every IsType(8) object as a weapon)
        const u32 t = w->WeaponType();                // vf7C
        if (t > 4) continue;
        if (next && t == WeaponDef::kPistol && !FindSequence("pistol_takeout")) continue;   // vfE4 only (0x0029D6F0)
        slots[kSlot[t]] = w;                          // (tables 0x000839BC / 0x00083840)
    }
    s32 k;
    if (next) {
        k = (cur + 1) % 5;                            // CDQ; IDIV 5
        while (!slots[k]) {
            if (k == cur) goto done;
            k = (k + 1) % 5;
        }
    } else {
        k = (cur - 1) % 5;
        if (k < 0) k += 5;                            // JGE; ADD 5
        while (!slots[k]) {
            if (k == cur) goto done;
            if (--k < 0) k += 5;                      // DEC; JNS
        }
    }
    if (weapon) attacking504 = 1;
    pendingWeapon = slots[k];
    attacking505 = 1;
done:
    if (weapon == pendingWeapon) {                    // nothing else found
        pendingWeapon = nullptr;
        attacking504 = attacking505 = 0;
    }
}

// ---- Fry's lock-on (vf150 0x00058A70 and its helpers) --------------------------------------------------------------
namespace {

// orig: 0x00050600 (cdecl(me, fwd, q)): the unit direction d = q - me (its length kept on the x87 stack) and the dot
// c = (d.y fwd.y + d.z fwd.z) + d.x fwd.x; below 0.85 ([0x0029A268]; FCOM; TEST AH,5; JP) -1 ([0x00293BF0]), else
// the length ((1 - c) 5 + 1) ([0x00293C24]) (the register).
double LockScore(const NiPoint3& me, const NiPoint3& fwd, const NiPoint3& q) {
    NiPoint3 d{F(double(q.x) - me.x), F(double(q.y) - me.y), F(double(q.z) - me.z)};
    const double len = ni::Unitize(d);                // 0x000116A0
    const double c = (double(d.y) * fwd.y + double(d.z) * fwd.z) + double(d.x) * fwd.x;
    if (c < 0.85f) return -1.0;                       // (a NaN goes on)
    return len * ((1.0 - c) * 5.0f + 1.0f);           // FSUB ST(1); FMUL; FADD; FMULP ST(2)
}

// orig: 0x00050730 (cdecl(count*, kind, kinds, score, scores, object, objects)): the candidate inserted before the
// first stored score it is below (FCOMP; TEST AH,5; JNP), else appended; the arrays have no bound in the original.
void InsertCandidate(s32& count, u8 kind, u8* kinds, f32 s, f32* scores, void* o, void** objects) {
    if (count >= 200) return;                         // (UB guard: the original's arrays hold 200)
    s32 i = 0;
    while (i < count && !(s < scores[i])) ++i;
    for (s32 k = count; k >= i && k > 0; --k) {       // (k == i copies [i - 1] once more, then [i] is overwritten)
        kinds[k] = kinds[k - 1];
        scores[k] = scores[k - 1];
        objects[k] = objects[k - 1];
    }
    kinds[i] = kind;
    scores[i] = s;
    objects[i] = o;
    ++count;
}

// orig: 0x00063620 (Destructable, thiscall(NiPoint3* out), RET 4): the definition's centre D+0x30 + the node's world
// translation +0xA0, each sum stored once.
NiPoint3 DestructableCentre(const Destructable& d) {
    const auto& c = static_cast<const DestructableDef*>(d.def)->center;
    const NiPoint3 w = d.GetNode() ? d.GetNode()->world.translate : NiPoint3{};
    return {F(double(c[0]) + w.x), F(double(c[1]) + w.y), F(double(c[2]) + w.z)};
}

// The aim point pulled back towards the player: p - 2 (p - from) / |p - from| (0x00058C85.. / 0x00054FFB..: the
// doubled unit vector stays on the x87 stack but for its z, which is stored first).
NiPoint3 PullBack(const NiPoint3& p, const NiPoint3& d) {
    const double x2 = double(d.x) + d.x, y2 = double(d.y) + d.y;   // FADD ST(0), ST(0)
    const f32 z2 = F(double(d.z) + d.z);
    return {F(double(p.x) - x2), F(double(p.y) - y2), F(double(p.z) - z2)};
}

}  // namespace

bool Player::FindObjectTarget(f32* dot, NiPoint3& out, bool setAim) {   // orig: 0x00054440
    const NiPoint3 me = CharacterPoint();             // vf78 (esp+0x48)
    // the facing (esp+0x54): first person (vf148) the game camera's +0x70, else vf38; (z + y) + x terms, each F
    const NiPoint3 fwd = InlineMul(firstPerson ? level.camera->rotation : rotation, kUnitX);
    s32 count = 0;                                    // esp+0x2C
    std::array<u8, 200> kinds{};                      // esp+0xA8 (1 a destructable, 0 a health trigger)
    std::array<void*, 200> objects{};                 // esp+0x170
    std::array<f32, 200> scores{};                    // esp+0x490
    for (const auto& st : level.stages)               // Session::NextStage 0x000C26C0 from 0
        for (GameObject* o = st->NextObject(nullptr); o; o = st->NextObject(o)) {   // Owner::NextObject 0x00097670
            if (!o->def->IsType(0x40)) continue;      // vf0C(0x40): a destructable
            auto* d = dynamic_cast<Destructable*>(o);
            if (!d || d->broken) continue;            // +0x48
            const double s = LockScore(me, fwd, DestructableCentre(*d));
            if (s == -1.0) continue;                  // FST; FCOMP [0x00293BF0]; TEST AH,0x44; JNP
            InsertCandidate(count, 1, kinds.data(), F(s), scores.data(), d, objects.data());
        }
    if (level.scriptTriggers)                         // DAT_002EA000 (newest first): the health triggers (+8 == 8)
        for (auto& tp : level.scriptTriggers->list) {
            ScriptTrigger& t = *tp;
            if (t.kind != ScriptTrigger::Health) continue;
            if (!(def->IsType(2) ? t.flag65 : t.flag64)) continue;   // (this->def) vf0C(2): +0x65, else +0x64
            if (t.health <= 0.0f) continue;           // FCOMP 0; TEST AH,0x41; JNP (a NaN goes on)
            const double s = LockScore(me, fwd, t.centre);   // +0xC
            if (s == -1.0) continue;
            InsertCandidate(count, 0, kinds.data(), F(s), scores.data(), &t, objects.data());
        }
    for (s32 k = 0; k < count; ++k) {                 // EBX (nothing found: false)
        auto* dst = kinds[k] ? static_cast<Destructable*>(objects[k]) : nullptr;
        auto* trg = kinds[k] ? nullptr : static_cast<ScriptTrigger*>(objects[k]);
        const NiPoint3 q = dst ? DestructableCentre(*dst) : trg->centre;   // esp+0x3C
        NiPoint3 d;                                   // esp+0x30
        f32 dist;                                     // esp+0x18
        if (setAim) {                                 // the facing, the distance of the unrounded differences
            d = fwd;
            const double vx = double(q.x) - me.x, vy = double(q.y) - me.y, vz = double(q.z) - me.z;
            dist = F(std::sqrt((vz * vz + vy * vy) + vx * vx));   // FSQRT; FST [esp+0x18]
        } else {                                      // the unit direction (inline: (z2 + y2) + x2)
            d = {F(double(q.x) - me.x), F(double(q.y) - me.y), F(double(q.z) - me.z)};
            const double l = std::sqrt((double(d.z) * d.z + double(d.y) * d.y) + double(d.x) * d.x);
            double r = 0.0;
            if (l > 9.999999974752427e-07) {          // FCOM [0x00293ACC]; TEST AH,0x41; JNE
                const double inv = 1.0 / l;           // FLD 1.0; FDIV ST(1) (kept)
                d = {F(double(d.x) * inv), F(double(d.y) * inv), F(double(d.z) * inv)};
                r = l;
            } else {
                d = {};
            }
            dist = F(r);                              // FST [esp+0x18]
        }
        f32 len = dist;                               // FSTP [esp+0x28]
        if (dst) {                                    // DAT_002E9A7C vf10(&me, &d, &dist, 1): the box itself
            const phys::PhysObject* hit = level.physic ? level.physic->RayCastObjects(nullptr, me, d, dist, true) : nullptr;
            if (hit != static_cast<const phys::PhysObject*>(dst->box.get())) continue;   // != +0x3C
        } else if (!HealthRayTest(*trg, me, d, dist)) {   // 0x000B7F00 (shortens dist)
            continue;
        }
        len = dist;
        const u8 old = ni::SetTriangleMask(0x20);     // SetTriMask 0x000F9D80
        const bool wall = static_cast<bool>(ni::RayCast(me, d, len, true, 7));   // 0x000FB780(&me, &d, &len, 1, 7)
        ni::SetTriangleMask(old);
        if (wall && len < dist) continue;             // FCOMP; TEST AH,5; JP: a nearer wall hides it
        // found: *out = me + d dist, *dot = the facing . unit(d)
        out = {F(double(d.x) * dist + me.x), F(double(d.y) * dist + me.y), F(double(F(double(d.z) * dist)) + me.z)};
        const double l = std::sqrt((double(d.z) * d.z + double(d.y) * d.y) + double(d.x) * d.x);
        double nx = 0.0, ny = 0.0, nz = 0.0;
        if (l > 9.999999974752427e-07) {              // FCOM [0x00293ACC]; TEST AH,0x41; JNE
            const f32 inv = F(1.0 / l);               // FSTP [esp+0x2C]
            nx = double(inv) * d.x, ny = double(d.y) * inv, nz = double(d.z) * inv;
        }
        *dot = F((double(fwd.z) * nz + double(fwd.y) * ny) + double(fwd.x) * nx);
        return true;
    }
    return false;
}

bool Player::LockRetarget() {                         // orig: 0x000552B0 (first person)
    (void)CharacterPoint();                           // vf78 (esp+0x14, not read)
    f32 dot = 0;                                      // esp+0x10
    lockTarget = FindTarget(&dot, tapCount, true);    // 0x00053D60(&dot, +0xC34, 1): aimPoint, tapCount
    if (lockTarget) {
        if (!lockOn) lockTime = 0;                    // +0xC2C
        lockOn = 1;
        lockHasTarget = 1;
        return true;
    }
    if (FindObjectTarget(&dot, aimPoint, true)) {     // 0x00054440(&dot, &+0xB04, 1)
        if (!lockOn) lockTime = 0;
        lockOn = 1;
        lockTarget = nullptr;
        lockHasTarget = 1;
        return true;
    }
    lockOn = 0;
    return false;
}

void Player::LockAcquire(bool held, u8& aimed) {      // orig: 0x00054F90
    f32 dot = 0;                                      // esp+0xC
    lockTarget = FindTarget(&dot, tapCount, false);   // 0x00053D60(&dot, +0xC34, 0)
    if (lockTarget && held) {
        aimPoint = lockTarget->CharacterPoint();      // vf78 -> +0xB04
        const NiPoint3 cp = CharacterPoint();
        NiPoint3 v{F(double(aimPoint.x) - cp.x), F(double(aimPoint.y) - cp.y), F(double(aimPoint.z) - cp.z)};
        (void)ni::Unitize(v);                         // FSTP ST0
        const NiPoint3 off{};                         // NiPoint3::ZERO (0x002F915C)
        const NiPoint3 tp = lockTarget->CharacterPoint();
        aimPoint = {F(double(off.x) + tp.x), F(double(off.y) + tp.y), F(double(off.z) + tp.z)};
        aimPoint = PullBack(aimPoint, v);
        lockHasTarget = lockOn = aimed = 1;
        lockB02 = 0;
    } else {
        lockTarget = nullptr;                         // 0x00055106
        const ni::NiMatrix3 R = rotation;             // vf38: +0xC40 = R UX, (y + z) + x terms
        const auto& m = R.m;
        lockDir = {F((double(kUnitX.y) * m[0][1] + double(kUnitX.z) * m[0][2]) + double(kUnitX.x) * m[0][0]),
                   F((double(kUnitX.y) * m[1][1] + double(kUnitX.z) * m[1][2]) + double(kUnitX.x) * m[1][0]),
                   F((double(kUnitX.y) * m[2][1] + double(kUnitX.z) * m[2][2]) + double(kUnitX.x) * m[2][0])};
        const bool found = FindObjectTarget(&dot, aimPoint, false);   // 0x00054440(&dot, &+0xB04, 0)
        aimed = found;
        if (found && dot > 0.9f && held) {            // FCOMP [0x00293C08]; TEST AH,0x41; JNE
            lockB02 = lockHasTarget = lockOn = aimed = 1;
            const NiPoint3 cp = CharacterPoint();
            NiPoint3 v{F(double(aimPoint.x) - cp.x), F(double(aimPoint.y) - cp.y), F(double(aimPoint.z) - cp.z)};
            (void)ni::Unitize(v);
            aimPoint = PullBack(aimPoint, v);
        }
    }
    if (lockOn) lockTime = 0;                         // 0x0005528B: +0xC2C
}

bool Player::LockAim(u8& aimed) {                     // orig: 0x00050FE0
    f32 len = 25.0f;                                  // esp+0x18 (0x41C80000)
    const NiPoint3 cp = CharacterPoint();             // vf78 (esp+0x38)
    const ni::NiMatrix3 R = rotation;                 // vf38: the facing, (y + z) + x terms, z first on the stack
    const auto& m = R.m;
    const double fz = (double(kUnitX.y) * m[2][1] + double(kUnitX.z) * m[2][2]) + double(kUnitX.x) * m[2][0];
    const double fy = (double(kUnitX.y) * m[1][1] + double(kUnitX.z) * m[1][2]) + double(kUnitX.x) * m[1][0];
    const double fx = (double(kUnitX.y) * m[0][1] + double(kUnitX.z) * m[0][2]) + double(kUnitX.x) * m[0][0];
    const NiPoint3 fwd{F(fx), F(fy), F(fz)};          // esp+0x20
    if (aimed) {                                      // the distance to the aim point
        NiPoint3 d{F(double(aimPoint.x) - cp.x), F(double(aimPoint.y) - cp.y), F(double(aimPoint.z) - cp.z)};
        len = F(ni::Unitize(d));
    }
    const u8 old = ni::SetTriangleMask(0x20);         // SetTriMask 0x000F9D80
    (void)ni::RayCast(cp, fwd, len, true, 7);         // 0x000FB780(&cp, &fwd, &len, 1, 7): along the facing
    ni::SetTriangleMask(old);
    if (!aimed) {                                     // the aim point straight ahead, at the wall or 25
        const double ax = double(fwd.x) * len, ay = double(fwd.y) * len;
        const f32 az = F(double(fwd.z) * len);
        aimPoint = CharacterPoint();                  // vf78 (dword copies)
        aimPoint = {F(ax + aimPoint.x), F(ay + aimPoint.y), F(double(az) + aimPoint.z)};
    }
    if (!lockOn) return false;
    TurnToward(aimPoint, 0.1f);                       // 0x0003AD90(&+0xB04, 0x3DCCCCCD)
    return true;
}

void Player::LockOn(ni::NiMatrix3& rot) {             // orig: vf150 0x00058A70
    if (type != 1) return;                            // 0x00058A85
    if (!weapon || weapon->WeaponType() == WeaponDef::kHammer) {   // 0x00058A93 / vf7C == 0 -> 0x00058F78
        lockOn = 0;
        lockHasTarget = 0;
        lockB02 = 0;
        tapTimer = 0;
        return;
    }
    // the pad (P+0x7B8; the original has no null test): the lock button (action 0xF) held / just pressed, action 0xE
    // just pressed (0x0005DFA0 bindings)
    const bool held = pad && pad->IsDownUnlatched(actions.Binding(0xF));   // 0x0009B960 -> [esp+0x18]
    const bool tapF = pad && pad->JustPressedUnlatched(actions.Binding(0xF));   // 0x0009BA20
    const bool tapE = pad && pad->JustPressedUnlatched(actions.Binding(0xE));
    if (tapF || tapE) {
        lockOn = 0;                                   // +0xB00
        if (tapTimer < 0.2f) ++tapCount;              // FCOMP [0x00293AB4]; TEST AH,5; JP (a double tap: the next one)
        else tapCount = 0;
    }
    if (held) tapTimer = 0;
    else tapTimer = F(double(g_frameStep) + tapTimer);
    if (tapTimer > 0.2f) tapTimer = 0.2f;             // TEST AH,0x41; JNE (0x3E4CCCCD)
    u8 aimed = 0;                                     // [esp+0x17]
    lockHasTarget = 0;
    if (held && lockOn && !firstPerson) {             // vf148
        if (!lockTarget) {
            f32 dot = 0;                              // esp+0x1C
            if (Character* t = FindTarget(&dot, 0, false)) {   // 0x00053D60(&dot, 0, 0)
                if (dot > 0.95f) {                    // FCOMP [0x00294FDC]; TEST AH,0x41; JNE
                    lockTarget = t;
                    lockTime = 0;                     // +0xC2C
                    lockHasTarget = 1;
                    lockB02 = 0;
                }
                goto locked;
            }
        }
        if (lockTarget && (!level.IsCharacter(lockTarget) || !lockTarget->IsTargetable()))   // 0x00033EE0; vfB0
            lockTarget = nullptr;
        else if (!lockTarget) {
            if (!lockB02) goto noObject;
            f32 dot = 0;
            if (FindObjectTarget(&dot, aimPoint, false)) {   // 0x00054440(&dot, &+0xB04, 0)
                lockHasTarget = 1;
                const NiPoint3 cp = CharacterPoint();  // vf78
                NiPoint3 v{F(double(aimPoint.x) - cp.x), F(double(aimPoint.y) - cp.y), F(double(aimPoint.z) - cp.z)};
                (void)ni::Unitize(v);                 // 0x00011530, 0x000116A0
                aimPoint = PullBack(aimPoint, v);
            } else {
                lockHasTarget = 0;
            }
        }
    locked:
        if (!lockB02) {
        noObject:
            lockHasTarget = lockTarget != nullptr;    // SETNE
        }
        lockOn = 1;
        aimed = 1;
        TurnToward(aimPoint, 0.1f);                   // 0x0003AD90(&+0xB04, 0x3DCCCCCD)
        rot = rotation;                               // vf38, 12 dwords into *rot
        if (lockTarget) {
            aimPoint = lockTarget->CharacterPoint();  // vf78 -> +0xB04
            NiPoint3 v;                               // esp+0x20
            if (firstPerson) {                        // (never here: the branch needs !vf148) the eye height D+0xA0
                const f32 h = cdef.fpsHeight;
                const NiPoint3 p = GetWorldPosition();   // vf34
                const double py = double(h) + p.y;
                v = {F(double(aimPoint.x) - p.x), F(double(aimPoint.y) - py), F(double(aimPoint.z) - p.z)};
            } else {
                const NiPoint3 cp = CharacterPoint();
                v = {F(double(aimPoint.x) - cp.x), F(double(aimPoint.y) - cp.y), F(double(aimPoint.z) - cp.z)};
            }
            (void)ni::Unitize(v);
            if (std::fabs(double(v.y)) > 0.6f) {      // FABS; FCOMP [0x002976C0]; TEST AH,0x41; JNE: too steep
                lockOn = 0;
                aimed = 0;
                lockTarget = nullptr;
            } else {
                const NiPoint3 off{};                 // NiPoint3::ZERO (0x002F915C)
                const NiPoint3 tp = lockTarget->CharacterPoint();
                aimPoint = {F(double(off.x) + tp.x), F(double(off.y) + tp.y), F(double(off.z) + tp.z)};
                aimPoint = PullBack(aimPoint, v);
            }
        }
    } else if (firstPerson) {                         // 0x00058EE5: vf148
        (void)LockRetarget();                         // 0x000552B0(&+0xB04)
    } else {
        lockOn = 0;
        if (held) LockAcquire(held, aimed);           // 0x00054F90(held, &aimed, &+0xB04)
    }
    if (firstPerson || !held) return;                 // 0x00058F2A: vf148
    if (LockAim(aimed)) rot = rotation;               // 0x00050FE0(&aimed, &+0xB04); vf38, 12 dwords
}

void Player::WeaponCycle(bool next) {                 // orig: vf10C (Fry 0x00085B70; other classes 0x000AEDC0, nop)
    if (type != 1) return;
    if (attacking504 || attacking505 || OnLadder() || OnLedge() || locked || stowedWeapon) return;
    StopVoice(level, fryVoiceD68);                    // 0x00085BD8
    StopVoice(level, fryVoiceD6C);
    const Weapon* old = weapon;                       // edi (+0x528)
    CycleWeapon(next);                                // next ? vfE4 0x00083860 : 0x000836F0
    if (weapon == old) return;                        // 0x00085C56 (the cycle changes only +0x524: always)
    ShowWeaponBox(level, pendingWeapon ? pendingWeapon : weapon, ammo);   // 0x00085C5C: +0x524, else +0x528
}

void Player::PreHook(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfEC
    if (className == "Bender") {
        PreHookBender();
    } else if (className == "Fry") {
        PreHookFry();
    } else if (className == "Leela") {
        PreHookLeela();
    } else if (className == "ZoidbergRat") {
        PreHookRat(vel, rot);
    }
}

void Player::PreHookBender() {                        // orig: 0x000805B0 (vel and rot are not read; RET 8)
    // the special attack (A and X): [0x00293AB0] 0.1 (TEST AH,0x41; JNE: only '>'), none of the attack / fall /
    // climb states, not in first person (vf148), a charge left (+0xAF8 > [0x00293AB8] 0)
    if (aValue > 0.1f && xValue > 0.1f && !IsAnimPlaying(0xBE) && !IsAnimPlaying(0x65) && !IsAnimPlaying(0xBD) &&
        !IsAnimPlaying(0xBB) && !IsAnimPlaying(0xBC) && !IsJumpAnim() && !IsFallAnim() && !IsAnimPlaying(0xD3) &&
        !OnLadder() && !OnLedge() && !firstPerson && charges > 0.0f) {   // vf12C / vf140 / vf138 / vf13C / vf148
        DeleteInventoryEffects();                     // 0x00034A50
        charges = F(double(charges) - 1.0f);          // FSUB [0x00293A98]
        aValue = xValue = 0.0f;                       // +0xB30 / +0xB34
        special50A = 1;                               // +0x50A
        special50B = special50C = 0;
        attack1 = attack2 = 0;                        // +0x506 / +0x507
        (void)SoundAt(0x3E, nullptr, 3, 0, node.Get());   // def 0x0003FFC0(0x3E, node, 3, 0)
    }
}

void Player::PreHookRat(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: ZoidbergRat vfEC 0x000944F0
    orbitX = 0;                                       // +0xB38 (the camera stick never reaches the follow camera)
    ride->Start();                                    // 0x00062EE0: started in the ctor, returns at once
    if (ride->started) {                              // 0x000611E0
        ride->Update(vel, rot);                       // 0x00061B00
        runTime = prevRunTime = 0;                    // +0xBD8 / +0xBDC
    }
    if (ni::NiAVObject* r = GetObjectByName(node.Get(), "raeigns"))   // node (+0x2C) vf54; 0x002A0FE0
        r->flags |= ni::kNiAVHidden;                  // +0xB0 |= 1 (the reins stay hidden)
}

void Player::PreHookFry() {                           // orig: 0x00083450 (vel and rot are not touched)
    if (IsAttackAnim()) {                             // vf124 -> vf128: a weapon fire overlay
        const double t = double(g_frameStep) + fireHold;
        fireHold = F(t);
        if (t > 3.0f) fireHold = 3.0f;                // 0x0029563C, TEST AH,0x41; JNE
        return;
    }
    if (aValue < 0.1f && xValue < 0.1f) fireHold = 0; // 0x00293AB0, TEST AH,5; JP (both)
    if (weapon && weapon->def->IsType(8)) {           // 0x000834C6
        const u32 t = weapon->WeaponType();           // vf7C
        const s32 slot = t == WeaponDef::kMachineGun ? 3 : t == WeaponDef::kPistol ? 0 : t == WeaponDef::kShotgun ? 1 : -1;
        if (slot >= 0 && GetAmmo(slot) == 0) {        // vf94 (the railgun and the hammer are not tested)
            const WeaponDef& wd = static_cast<const WeaponDef&>(*weapon->def);   // (EDI: the weapon before the cycle)
            WeaponCycle(false);                       // vf10C(0)
            if (!level.Sound().HasVoices(wd.clickSound))   // the click: 0x00083530 / 0x00083556
                (void)level.Sound().Play3D(wd.clickSound, GetWorldPosition(), false);
        }
    }
}

void Player::PreHookLeela() {                         // orig: 0x0008AC00 (vel / rot are not written)
    if (!hasControl) return;                          // +0x630
    // ---- the special (wind kick): A and X together
    if (aValue > 0.1f && xValue > 0.1f && !IsAnimPlaying(0xB5) && body->supported && charges > 0.0f) {
        DeleteInventoryEffects();                     // 0x00034A50 (the inventory's def IsType(0x8000) objects)
        ++specialCount;
        charges = F(double(charges) - 1.0f);
        special50A = 1;
        special50B = special50C = 0;
        attack1 = attack2 = 0;
        aValue = xValue = 0;
        const NiPoint3 pos = GetWorldPosition();
        SoundAt(0x3B, &pos, 3);
        // orig: 0x0008ACF5..0x0008AD7F: Session::FindDef 0x000C3F10("Leela_Wind" 0x0029F3C8); def vf14(this+0x38, vf34,
        // vf38, "RHOUSE" 0x002962D0); then on the object 0x0004F580(NiPointer of +0x2C), 0x00073720(1.0) and vf1C((0, 6, 0))
        if (Def* d = level.defs ? level.defs->Find("Leela_Wind") : nullptr) {
            const NiPoint3 wp = GetWorldPosition();   // vf34
            const ni::NiMatrix3 R = rotation;         // vf38
            if (GameObject* o = d->Spawn(level, &inventory, wp, R, "RHOUSE"))   // vf14
                if (auto* im = dynamic_cast<Impact*>(o)) {   // (port guard: the original calls Impact's methods untested)
                    im->SetFollow(node.Get());        // 0x0004F580 (a NiPointer temp of +0x2C, 0x00013B10)
                    SetNodeScale(*im, 1.0f);          // 0x00073720
                    im->SetPosition({0.0f, 6.0f, 0.0f});   // vf1C (0x40C00000)
                }
        }
    }
    // ---- the dodge roll: the RAW right trigger (pad button 9), not the action map
    if (!pad || !pad->IsDownUnlatched(input::kRightTrigger)) return;   // (the original has no null check)
    if (IsAnimPlaying(0x61) || IsAnimPlaying(0x5E) || IsAnimPlaying(0x5F) || IsAnimPlaying(0x60)) return;
    if (!body->supported || !interruptible || (body->shape.flags & 0x80)) return;
    if (!level.camera || !level.camera->sceneCamera) return;   // (always present in the original)
    const ni::NiMatrix3& C = level.camera->sceneCamera->world.rotate;   // NiCamera +0x70 (WORLD; Steer: +0x30)
    const NiPoint3 A = ni::MulVector(C, kUnitX);
    const NiPoint3 B = ni::MulVector(C, kUnitZ);
    const f32 X = moveX, Y = moveY;
    NiPoint3 d{F(double(F(double(A.x) * Y)) + double(B.x) * X), 0.0f,
               F(double(F(double(A.z) * Y)) + double(F(double(B.z) * X)))};
    const NiPoint3 Fw = ni::MulVector(rotation, kUnitX);
    const NiPoint3 Rt = ni::MulVector(rotation, kUnitZ);
    if (std::fabs(moveY) > 0.5f || std::fabs(moveX) > 0.5f) {   // TEST AH,0x41 (both)
        (void)ni::Unitize(d);
        NiPoint3 sd = CrossX87(d, kUnitY);
        (void)ni::Unitize(sd);
        const double fd = (double(d.x) * Fw.x + double(Fw.y) * d.y) + double(Fw.z) * d.z;   // ST0 (FCOM, kept)
        const f32 rd = F((double(Rt.x) * d.x + double(Rt.y) * d.y) + double(Rt.z) * d.z);
        const NiPoint3 nd{-d.x, -d.y, -d.z}, ns{-sd.x, -sd.y, -sd.z};
        bool rolled = true;
        if (fd > 0.5f) {                              // FCOM 0x00293AA0; TEST AH,0x41; JNE
            SetRotation(FromColumns(d, kUnitY, sd));
            roll98 = 1;                               // rollf 0x61
        } else if (fd < -0.5f) {                      // FCOMP 0x00297414; TEST AH,5; JP
            SetRotation(FromColumns(nd, kUnitY, ns));
            roll97 = 1;                               // rollb 0x60
        } else if (rd > 0.5f) {
            SetRotation(FromColumns(ns, kUnitY, d));
            roll96 = 1;                               // rollr 0x5F
        } else if (rd < -0.5f) {
            SetRotation(FromColumns(sd, kUnitY, nd));
            roll95 = 1;                               // rolll 0x5E
        } else {
            rolled = false;
        }
        if (rolled) sinceAttack = 0;                  // 0x0008B19E
    }
    strafe = moveY = moveX = 0;                       // 0x0008B1A4: always once the RT gates passed
}

void Player::PostHook(NiPoint3& vel, ni::NiMatrix3& rot) {  // orig: vfF8
    if (className == "Bender") {
        PostHookBender(vel, rot);                     // 0x00080720
    } else if (className == "Fry") {
        PostHookFry(vel, rot);
    } else if (className == "Leela") {
        PostHookLeela(vel, rot);
    }
}

void Player::PostHookClimbBlocks(NiPoint3& vel, ni::NiMatrix3& rot) {   // Fry 0x00084650.. / Leela (the same code)
    const NiPoint3 Z{};
    if (IsAnimPlaying(0x29)) {                        // ledgeclimboff
        if (finished) {
            ledge = 0;                                // +0xB8D (the ledge state; player_moves_port.md 7)
            ledgeB8F = 0;                            // +0xB8F
            vel = Z;
        } else {
            vel = Z;
            if (physic) physic->SetActive(true);
            body->halveXZ = 0;                        // vf2C(0)
            body->SetForce(Z);
            body->velocity = Z;
        }
    }
    if (IsAnimPlaying(0xF7)) {                        // onto_ladder
        vel = Z;
        body->SetForce(Z);
        body->velocity = Z;
        rot = rotation;
    }
    if (IsAnimPlaying(0x2E)) {                        // ladderclimboff
        (void)GetWorldPosition();                     // vf34, result unused
        directRoot = 1;                               // +0x638 -> ApplyRootMotion 0x0003E913
        if (finished) {
            ladder = 0;                               // +0xB8C (the ladder state)
            ladderB91 = 0;                             // +0xB91
            vel = Z;
        } else {
            vel = Z;
            if (physic) physic->SetActive(true);
            body->halveXZ = 0;
            body->SetForce(Z);
            body->velocity = Z;
        }
    }
}

void Player::PostHookFry(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x00084460
    const NiPoint3 Z{};
    static constexpr s32 kJump[] = {0x1B, 0x21, 0x1E, 0x76, 0x7C, 0x79, 0x98, 0x9E, 0x9B};
    bool jump = false;
    for (s32 k : kJump)
        if (IsAnimPlaying(k)) {
            jump = true;
            break;
        }
    if (jump && !finished) {                          // 0x0008450C: the root motion carries the jump
        body->velocity.y = 0;
        if (physic) physic->SetActive(true);
        body->SetForce(Z);
    }
    if (IsAnimPlaying(0x65) || IsAnimPlaying(0x7F) || IsAnimPlaying(0xA1) || IsAnimPlaying(0x53)) {
        vel = Z;                                      // high_land (+ shotgun / railgun) / electric_shock
        rot = rotation;
    }
    if (IsAnimPlaying(0x6A)) {                        // skid
        vel = Z;
        rot = rotation;
        if (physic) physic->SetActive(true);
    }
    if (IsAnimPlaying(0x1D) && b9A) vel = Z;          // land after a strafing landing
    if (IsAnimPlaying(0x24) && finished) ledgeB90 = 0;   // ledgegrab done: +0xB90
    PostHookClimbBlocks(vel, rot);
    for (s32 anim : {0xDD, 0xDE, 0xDF}) {             // 0x0008480C: the side attacks' auto-aim (not gated on a weapon)
        if (!IsAnimPlaying(anim)) continue;
        f32 dot = 0;
        if (Character* t = FindTarget(&dot, 0, false))   // 0x00053D60(&dot, 0, 0)
            if (dot > 0.9f) {                         // [0x00293C08]; TEST AH,0x41; JNE
                aimPoint = t->CharacterPoint();       // vf78 -> +0xB04
                TurnToward(aimPoint, 1.0f);           // 0x0003AD90
                rot = GetWorldRotation();             // vf38 -> *rot (12 dwords)
            }
    }
    if (IsPlayingName("machine_gun_attack1")) rumble.Add(0.5f, 0.5f, 0.3f, 0);   // 0x0029D5E8; 0x0005F000
    for (s32 anim : {0xD9, 0xDA}) {                   // tentacle_attack2_forward / _back
        if (!IsAnimPlaying(anim)) continue;
        if (finished) {
            NotPorted("Fry vfF8: tentacle push end (EndRootMotion + SetRotation with stale columns, 0x000849D9)");
        } else {
            body->velocity.z = 0;
            body->velocity.x = 0;
            vel = Z;
            rot = rotation;
            if (physic) physic->SetActive(true);
            body->halveXZ = 0;
        }
    }
}

void Player::PostHookLeela(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x000897E0
    const NiPoint3 Z{};
    if ((IsAnimPlaying(0x1B) || IsAnimPlaying(0x21)) && !finished) {   // only jump / jumprun
        body->velocity.y = 0;
        if (physic) physic->SetActive(true);
        body->SetForce(Z);
    }
    if (IsAnimPlaying(0x65) || IsAnimPlaying(0x53)) {
        vel = Z;
        rot = rotation;
    }
    if (IsAnimPlaying(0x6A)) {
        vel = Z;
        rot = rotation;
        if (physic) physic->SetActive(true);
    }
    if (IsAnimPlaying(0x1D) && b9A) vel = Z;
    if (IsAnimPlaying(0x24) && finished) ledgeB90 = 0;
    PostHookClimbBlocks(vel, rot);
    auto restoreBox = [&] {                           // the def's box back, the feet kept in place
        const NiPoint3 pos = GetWorldPosition();
        heightOffset = cdef.boundCenter;              // +0x4D0 = def+0x9C
        boundHeight = cdef.boundHeight;               // +0x4D4 = def+0x94
        body->shape.ext.y = cdef.boundCenter;         // shape+0x94 = def+0x9C
        SetPosition(pos);                             // vf1C
    };
    if (IsAnimPlaying(0xB6)) {                        // runkick
        if (finished) {
            restoreBox();
        } else {
            body->velocity.y = 0;
            body->SetForce(Z);
            vel = Z;
            rot = rotation;
            if (physic) physic->SetActive(true);
        }
    }
    if (IsAnimPlaying(0xB7) && body->velocity.y > 0.0f) body->velocity.y = 0;   // jumpkick
    if (IsAnimPlaying(0x60) || IsAnimPlaying(0x5E) || IsAnimPlaying(0x5F) || IsAnimPlaying(0x61)) {   // rolls
        if (finished) {
            restoreBox();
            (void)GetWorldPosition();                 // 0x00089CB5, unused
        } else {
            body->velocity.x = 0;
            if (body->velocity.y > 0.0f) body->velocity.y = 0;
            body->velocity.z = 0;
            body->halveXZ = 0;
            vel = Z;
            rot = rotation;
            if (physic) physic->SetActive(true);
            if (!IsAnimPlaying(0x60)) {               // not rollb: the box crouches to 2.5 (0x40200000)
                u32 bits;
                std::memcpy(&bits, &boundHeight, 4);
                if (bits != 0x40200000u) {            // integer compare
                    const NiPoint3 pos = GetWorldPosition();
                    heightOffset = boundHeight = body->shape.ext.y = 2.5f;
                    SetPosition(pos);
                } else {
                    heightOffset = boundHeight = body->shape.ext.y = 2.5f;
                }
            }
        }
    }
    if (IsAnimPlaying(0xB5)) {                        // RHOUSE
        body->velocity = Z;
        vel = Z;
        rot = rotation;
    }
    auto push = [&] {                                 // the forward step of a finished combo move (fwd / dt)
        const NiPoint3 f = ni::MulVector(rotation, kUnitX);
        const double inv = 1.0 / double(g_frameStep);
        const f32 invF = F(inv);                      // FST
        const NiPoint3 v{F(double(invF) * f.x), F(double(invF) * f.y), F(inv * f.z)};   // z: the unrounded quotient
        body->velocity = {F(double(body->velocity.x) + v.x), F(double(v.y) + body->velocity.y),
                          F(double(v.z) + body->velocity.z)};   // vf50 AddVelocity 0x000AF7D0
    };
    auto slow = [&] {
        body->velocity = Z;
        vel = {F(double(vel.x) * 0.1f), F(double(vel.y) * 0.1f), F(double(vel.z) * 0.1f)};   // 0x00293AB0
        rot = rotation;
    };
    if (IsAnimPlaying(0xA9) || IsAnimPlaying(0xAA) || IsAnimPlaying(0xAB) || IsAnimPlaying(0xAC)) {   // kicks (A)
        slow();
        if (finished) {
            if (s_leelaHitsA == hitsA - 1) push();
        } else {
            s_leelaHitsA = hitsA;
        }
    }
    if (IsAnimPlaying(0xB3)) {                        // CHKICK
        body->velocity = Z;
        vel = Z;
        rot = rotation;
    }
    if (IsAnimPlaying(0xAD) || IsAnimPlaying(0xAF) || IsAnimPlaying(0xB1) || IsAnimPlaying(0xAE) ||
        IsAnimPlaying(0xB0) || IsAnimPlaying(0xB2)) { // punches (X)
        slow();
        if (finished) {
            if (s_leelaHitsX == hitsX - 1) push();
        } else if (double(animTime) - g_frameStep <= animStart) {   // TEST AH,0x41; JP: only the first step
            s_leelaHitsX = hitsX;
        }
    }
    if (IsAnimPlaying(0xB4)) {                        // chpunch
        body->velocity = Z;
        body->halveXZ = 0;
        vel = Z;
        rot = rotation;
        if (physic) physic->SetActive(true);
    }
}

// orig: 0x00080720 (thiscall(NiPoint3* vel, NiMatrix3* rot), RET 8; camera_modes_bender_port.md 7). Compared with
// Fry: only jump / jumprun in the jump list, the charge-punch, dizzy, CHARGE and SPINFLAME branches and a tail.
void Player::PostHookBender(NiPoint3& vel, ni::NiMatrix3& rot) {
    const NiPoint3 Z{};                               // esp+0x2C: copy of NiPoint3::ZERO (0x002F915C)
    body->b1A = 0;                                    // body+0x1A (no null check)
    if ((IsAnimPlaying(0x1B) || IsAnimPlaying(0x21)) && !finished) {   // jump / jumprun: the root motion carries it
        body->velocity.y = 0;                         // body+0x2C
        if (physic) physic->SetActive(true);          // P+0x560 (no null check in the original)
        body->SetForce(Z);                            // vf10
    }
    if (IsAnimPlaying(0xD1) && !finished) {           // charge_punch_jump: the same three writes (0x000807B4)
        body->velocity.y = 0;
        if (physic) physic->SetActive(true);
        body->SetForce(Z);
    }
    if (IsAnimPlaying(0xD2)) body->SetForce({0.0f, -300.0f, 0.0f});   // charge_punch_fall: (0, 0xC3960000, 0)
    if (IsAnimPlaying(0xD3)) {                        // charge_punch_impact
        vel = Z;                                      // (dword copies of NiPoint3::ZERO)
        rot = rotation;                               // vf38, 12 dwords
    }
    if (IsAnimPlaying(0x65) || IsAnimPlaying(0x53)) { // high_land / electric_shock (0x0008086B)
        vel = Z;
        rot = rotation;
    }
    if (IsAnimPlaying(0x6A)) {                        // skid
        vel = Z;
        rot = rotation;
        if (physic) physic->SetActive(true);
    }
    if (IsAnimPlaying(0x1D) && b9A) vel = Z;          // land after a strafing landing (0x000808F9)
    // (0x00080925: IsAnimPlaying(0x23), result unused)
    if (IsAnimPlaying(0x24) && finished) ledgeB90 = 0;   // ledgegrab done (+0xB90)
    // (IsAnimPlaying(0x25), result unused)
    PostHookClimbBlocks(vel, rot);                    // 0x00080955..0x00080B21: Fry's 0x29 / 0xF7 / 0x2E blocks
    // (IsAnimPlaying(0xBA) spinair and (0xB9) SPIN, results unused)
    if (IsAnimPlaying(0xBE)) {                        // dizzy
        vel = Z;
        rot = rotation;
    }
    if (IsAnimPlaying(0xB8) && !finished) {           // CHARGE (0x00080B9A)
        body->velocity.z = 0;                         // body+0x30
        body->velocity.x = 0;                         // body+0x28
        vel = Z;
        rot = rotation;
        body->halveXZ = 0;                            // vf2C(0)
        if (physic) physic->SetActive(true);
    }
    if (IsAnimPlaying(0xBB) || IsAnimPlaying(0xBD)) { // SPINFLAME_powerup / _powerdown
        vel = Z;
        rot = rotation;
    }
    if (IsAnimPlaying(0xBC)) SpinFlameLoop(vel);      // SPINFLAME_loop (0x00080C5B)
    // 0x00080F90: vfA8 IsDead, then the model's (+0x564) flags +0xB0 bit 0 (hidden; no null check): body vf34(0).
    if (IsDead() && model && (model->flags & ni::kNiAVHidden)) body->SetEnabled(false);
}

// orig: 0x00080C5B..0x00080F8A (inline in Bender's vfF8): the flaming spin steers the body with the chest's facing.
void Player::SpinFlameLoop(NiPoint3& vel) {
    NiPoint3 a = velBefore;                           // +0x4E4, dword copy (esp+0x20)
    NiPoint3 t;                                       // esp+0x14
    if (GetObjectByName(model.Get(), "B_ChestFrame")) {   // 0x0029DB10, model vf54 (looked up twice)
        ni::NiAVObject* chest = GetObjectByName(model.Get(), "B_ChestFrame");
        const NiPoint3 d = ni::MulVector(chest->world.rotate, kUnitX);   // node +0x70, 0x00011770 with 0x002D77E0
        t = {d.x, 0.0f, d.z};                         // (y stored, then overwritten with 0)
        (void)ni::Unitize(t);                         // 0x000116A0; FSTP ST0
        t = {F(double(t.x) * 2.5f), F(double(t.y) * 2.5f), F(double(t.z) * 2.5f)};   // [0x00293B10]
        a = {F(double(a.x) * 0.95f), F(double(a.y) * 0.95f), F(double(a.z) * 0.95f)};   // [0x00294FDC]; local only
    } else {
        // The original leaves esp+0x14..+0x1C unwritten (the caller's stale stack): an original bug.
        NotPorted("Bender vfF8: SPINFLAME_loop without a B_ChestFrame bone (stale stack, 0x00080C87)");
    }
    // 0x00080D5A: all three components are loaded (and scaled by [0x00293B50] 0.25) before any store.
    const double vx = double(vel.x) * 0.25f, vy = double(vel.y) * 0.25f;
    const f32 qz = F(double(vel.z) * 0.25f);          // FSTP [esp+0x40]
    const f32 sx = F(vx + a.x);                       // FADD [esp+0x20]; FSTP [esp+0x44]
    const double sz = double(qz) + a.z;               // FLD; FADD [esp+0x28] (stays on the stack)
    vel.x = F(double(sx) + t.x);
    vel.y = F(vy + t.y);                              // (a.y is never added)
    vel.z = F(sz + t.z);
    const double len = std::sqrt((double(vel.z) * vel.z + double(vel.y) * vel.y) + double(vel.x) * vel.x);
    if (len > 30.0f) {                                // FCOM [0x0029763C]; TEST AH,0x41; JNE
        const double inv = 1.0 / len;                 // FDIVR [0x00293A98]
        const double ux = inv * vel.x;                // (unrounded)
        const f32 uy = F(inv * vel.y), uz = F(inv * vel.z);
        vel = {F(ux * 30.0f), F(double(uy) * 30.0f), F(double(uz) * 30.0f)};
    }
    rumble.Add(0.5f, 0.5f, 0.3f, 0);                  // 0x00080E5D (every step of the loop)
    // The hit list is cleared once per half second of the overlay clock (0.5, 1.0, ..., 3.0 s).
    const double d0 = double(animTime) - overlayStart;   // +0x78 - +0x4C0
    const f32 d = F(d0);                              // FST [esp+0x10]
    bool clear = !(d0 < 0.5) && double(g_frameStep) + 0.5 > d;   // the first test reads the unrounded value
    for (f32 k : {1.0f, 1.5f, 2.0f, 2.5f, 3.0f})     // [0x00293A98] [0x00293BEC] [0x00293B20] [0x00293B10] [0x0029563C]
        if (!clear && !(d < k) && double(g_frameStep) + k > d) clear = true;   // TEST AH,1; JNE / TEST AH,0x41; JE
    if (clear) hitList.clear();                       // entries zeroed, +0x5C4 = +0x5C8 = 0
}

void Player::AttackFryA(f32 value) {                  // orig: 0x00084B90 (inventory_batch1_port.md 5.6)
    if (value < 0.1f) {                               // TEST AH,5; JP: >= 0.1 or NaN is "pressed"
        aHeld = 0;                                    // unconditionally
        return;
    }
    // (vf138 / vf13C / vf124 are called once before the gate with the results discarded: pure)
    if (!weapon || attacking504 || attacking505 || OnLadder() || OnLedge() || locked || IsAttackAnim()) return;
    (void)RequestFire(1);                             // 0x00083570(1)
    sinceAttack = 0;                                  // +0xB54
    aHeld = 1;                                        // +0xB9B
    attackStart = F(double(animTime) + 0.5f);         // +0xB68 (FADD [0x00293AA0]; also when the request failed)
}

void Player::AttackFryX(f32 value, ni::NiMatrix3& rot) {   // orig: 0x00084C50
    if (value < 0.1f) {                               // released
        StopVoice(level, fryVoiceD68);                // 0x00084C7F
        StopVoice(level, fryVoiceD6C);
        if (IsChargingAnim()) {                       // 0x00083330: 0x90 / 0x83 / 0xA5 / 0xA7 playing
            f32 dot = 0;
            if (Character* t = FindTarget(&dot, 0, false))   // 0x00053D60(&dot, 0, 0)
                if (dot > 0.9f) {                     // [0x00293C08]; TEST AH,0x41; JNE
                    aimPoint = t->CharacterPoint();   // vf78 -> +0xB04
                    TurnToward(aimPoint, 1.0f);       // 0x0003AD90
                    rot = GetWorldRotation();         // vf38 (12 dwords into *rot)
                }
        }
        xHeld = 0;                                    // +0xB9C
        return;
    }
    // (vf138 / vf13C / vf124 are called once before the gate with the results discarded: pure)
    if (!weapon || attacking504 || attacking505 || OnLadder() || OnLedge() || locked || IsAttackAnim()) return;
    if (IsChargeAnimPlaying()) {                      // 0x00083380
        // 0x00084DF0: Voice_Exists without null tests; the charged loop W->def+0x68 attached to the player's node
        sound::GameSound& snd = level.Sound();
        if (!snd.VoiceExists(static_cast<s32>(fryVoiceD68)) && !snd.VoiceExists(static_cast<s32>(fryVoiceD6C)))
            fryVoiceD6C = static_cast<u32>(snd.PlayAttached(static_cast<const WeaponDef&>(*weapon->def).chargedLoopSound,
                                                            node.Get(), true));   // Sound_PlayAttached 0x00084E38
        if (IsChargingAnim()) chargeTime = F(double(g_frameStep) + chargeTime);   // +0xD64
        return;
    }
    chargeTime = 0;                                   // +0xD64
    const bool ok = RequestFire(2);                   // 0x00083570(2)
    xHeld = 1;
    if (!ok) return;
    sinceAttack = 0;                                  // +0xB54
    attack2Start = F(double(animTime) + 0.5f);        // +0xB6C (FADD [0x00293AA0])
    if (weapon->WeaponType() != 0)                    // vf7C: the charging sound W->def+0x64 (0x00084ECB)
        fryVoiceD68 = static_cast<u32>(level.Sound().PlayAttached(
            static_cast<const WeaponDef&>(*weapon->def).chargingSound, node.Get(), false));
}

void Player::AttackLeelaA(f32 value) {                // orig: 0x0008A190
    if (value < 0.1f) {
        aHeld = 0;
        return;
    }
    f32 dur = 0;
    if (IsAnimPlaying(0x65) || attacking504 || attacking505 || OnLadder() || OnLedge() || locked || IsLandAnim() ||
        (body->shape.flags & 0x80) || IsAttackAnim() || aHeld)
        return;
    // (0x00050B90 / 0x00050C50: the stick . forward / . right, results discarded)
    auto spawn = [&](const char* defName) {           // def->vf14(P+0x38, GetWorldPosition, rotation, "KICK")
        Def* d = level.defs ? level.defs->Find(defName) : nullptr;   // Session::FindDef 0x000C3F10
        if (!d) return;
        const NiPoint3 p = GetWorldPosition();        // vf34
        const ni::NiMatrix3 R = rotation;             // vf38
        if (GameObject* o = d->Spawn(level, &inventory, p, R, "KICK")) {   // vf14 (0x0029F344); ImpactDef 0x0004F710
            if (auto* im = dynamic_cast<Impact*>(o))  // (the Leela_* definitions are ImpactDefs)
                im->speed = F(double(im->duration) / dur);   // FLD [o+0x58]; FDIV [esp+0x14]; FSTP [o+0x50]
        }
    };
    NiPoint3 pos;
    if (runTime > 0.5f && body->supported) {          // TEST AH,0x41; JNE
        special50B = 1;
        dur = F(SequenceDuration(SeqById(0xB6)));
        ++specialCount;
        pos = GetWorldPosition();
        SoundAt(0x37, &pos, 3);
        spawn("Leela_RunKick");
    } else if ((!body->supported && IsAnimPlaying(0x1B) && double(animStart) + 0.25f < animTime &&
                double(animStart) + 0.5f > animTime) ||
               (IsAnimPlaying(0x21) && double(animStart) + 0.1f < animTime && double(animStart) + 0.4f > animTime)) {
        special50C = 1;                               // the jump kick (0x0008A3F1)
        dur = F(SequenceDuration(SeqById(0xB7)));
        ++specialCount;
        pos = GetWorldPosition();
        SoundAt(0x37, &pos, 3);
        spawn("Leela_JumpKick");
    } else {
        if (!body->supported) return;                 // 0x0008A482: no end stores
        attack1 = 1;
        if (double(attackStart) + 0.25f > animTime) { // within 0.25 s of the previous kick's end
            if (++kickIndex == 5) kickHits = 0;
            if (kickIndex > kickHits) {
                kickIndex = 0;
                kickRow = static_cast<u8>(crt::Rand() % 2);
                kickSteps = 0;
            }
        } else {
            const s32 r = crt::Rand() % 2;
            kickIndex = 0;
            kickHits = 0;
            kickRow = static_cast<u8>(r);
            kickSteps = 0;
        }
        ++kickSteps;
        static constexpr s32 kId[5] = {0xA9, 0xAA, 0xAB, 0xAC, 0xB3};
        static constexpr const char* kFx[5] = {"Leela_HKL", "Leela_HKR", "Leela_MKL", "Leela_MKR", "Leela_ChargeKick"};
        const u8 k = kickTable[kickRow][kickIndex];
        if (k <= 4) {
            dur = F(SequenceDuration(SeqById(kId[k])));
            spawn(kFx[k]);
        }
        pos = GetWorldPosition();
        SoundAt(k == 4 ? 0x38 : 0x37, &pos, 3);
        ++kickCount;
    }
    aHeld = 1;                                        // 0x0008A671
    sinceAttack = 0;
    attackStart = F(double(dur) + animTime);          // the move's end time
}

void Player::AttackLeelaX(f32 value) {                // orig: 0x0008A6B0
    if (value < 0.1f) {
        xHeld = 0;
        return;
    }
    f32 dur = 0;
    if (IsAnimPlaying(0x65) || attacking504 || attacking505 || OnLadder() || OnLedge() || locked || IsAttackAnim() ||
        !body->supported || attack2 || xHeld)
        return;
    attack2 = 1;
    if (double(attack2Start) + 0.5f > animTime) {     // 0x00293AA0
        if (++punchIndex == 5) punchHits = 0;
        if (punchIndex > punchHits) {
            punchIndex = 0;
            punchSteps = 0;
            punchRow = static_cast<u8>(crt::Rand() % 4);
        }
    } else {
        const s32 r = crt::Rand() % 4;
        punchIndex = 0;
        punchSteps = 0;
        punchHits = 0;
        punchRow = static_cast<u8>(r);
    }
    ++punchSteps;
    static constexpr s32 kId[7] = {0xB1, 0xB2, 0xAF, 0xB0, 0xAD, 0xAE, 0xB4};
    static constexpr const char* kFx[7] = {"Leela_JPL", "Leela_JPR", "Leela_SPL", "Leela_SPR", "Leela_UPL", "Leela_UPR",
                                           "Leela_ChargePunch"};
    const u8 k = punchTable[punchRow][punchIndex];
    if (k <= 6) {
        dur = F(SequenceDuration(SeqById(kId[k])));   // FSTP [esp+0x10]
        if (Def* d = level.defs ? level.defs->Find(kFx[k]) : nullptr) {   // Session::FindDef 0x000C3F10
            const NiPoint3 p = GetWorldPosition();    // vf34
            const ni::NiMatrix3 R = rotation;         // vf38
            if (GameObject* o = d->Spawn(level, &inventory, p, R, "RHOUSE"))   // vf14 (0x002962D0)
                if (auto* im = dynamic_cast<Impact*>(o))
                    im->speed = F(double(im->duration) / dur);   // FLD [o+0x58]; FDIV [esp+0x10]; FSTP [o+0x50]
        }
    }
    const NiPoint3 pos = GetWorldPosition();
    SoundAt(k == 6 ? 0x3A : 0x39, &pos, 3);
    attack2Start = F(double(dur) + animTime);
    ++punchCount;
    xHeld = 1;
    sinceAttack = 0;
}

// orig: 0x00080FD0 (thiscall(f32 value, NiMatrix3* rot), RET 8; rot is not read). 0x0003FFC0(id, node, flags, index)
// is CharacterDef 0x0003FEA0(id, nullptr, flags, index, node) on the definition (+0x10).
void Player::AttackBenderA(f32 value) {
    if (value < 0.1f) {                               // FCOMP [0x00293AB0]; TEST AH,5; JP: >= 0.1 or NaN is "pressed"
        aHeld = 0;                                    // +0xB9B
        return;
    }
    if (IsAnimPlaying(0xBE) || IsAnimPlaying(0x65) || attacking504 || attacking505 || OnLadder() || OnLedge() ||
        locked || (body->shape.flags & 0x80) || IsAttackAnim())   // vf138 / vf13C / +0xB8E / body vf4C / vf124
        return;
    if (IsJumpAnim()) {                               // vf12C: the charge punch out of a jump (0x0008109C)
        if (!curSeq) return;                          // (no null check in the original: a jump is never an overlay)
        const double t = double(curSeq->offset) + animTime;   // FLD [cur+0xA8]; FADD [+0x78]
        if (!(t > 0.25f)) return;                     // FCOM [0x00293B50]; TEST AH,0x41; JNE -> FSTP ST0
        if (!(t < 0.6f)) return;                      // FCOMP [0x002976C0]; TEST AH,5; JP
        DeleteInventoryEffects();                     // 0x00034A50
        attackStart = F(SequenceDuration(SeqById(0xD1)) + double(animTime));   // FADD [+0x78]; FSTP +0xB68
        ++kickCount;                                  // +0xC00
        sinceAttack = 0;                              // +0xB54
        aHeld = 1;                                    // +0xB9B
        b9D = 1;                                      // +0xB9D: AttackStates plays charge_punch_jump 0xD1
        (void)GetWorldPosition();                     // vf34 into a local, not read
        (void)SoundAt(0x44, nullptr, 3, 0, node.Get());   // 0x0003FFC0(0x44, node, 3, 0)
        return;
    }
    if (IsFallAnim()) return;                         // vf140
    if (aHeld) return;                                // +0xB9B
    DeleteInventoryEffects();                         // 0x00034A50
    (void)GetWorldPosition();                         // vf34, not read
    (void)SoundAt(0x3D, nullptr, 3, 0, node.Get());   // 0x0003FFC0(0x3D, node, 3, 0)
    attack1 = 1;                                      // +0x506: AttackStates plays CHARGE 0xB8 + BeginRootMotion
    attackStart = F(SequenceDuration(SeqById(0xB8)) + double(animTime));   // +0xB68
    sinceAttack = 0;
    ++kickCount;                                      // +0xC00
    aHeld = 1;
}

// orig: 0x000811D0 (thiscall(f32 value, NiMatrix3* rot), RET 8; rot is not read).
void Player::AttackBenderX(f32 value) {
    if (value < 0.1f) {                               // TEST AH,5; JP
        xHeld = 0;                                    // +0xB9C
        return;
    }
    if (IsAnimPlaying(0xBE) || IsAnimPlaying(0x65) || attack1 || attacking504 || attacking505 || OnLadder() ||
        OnLedge() || locked || (body->shape.flags & 0x80) || IsAttackAnim())
        return;
    if (!xHeld) {                                     // (held: straight to the xHeld store at 0x00081422)
        DeleteInventoryEffects();                     // 0x00034A50
        ++punchCount;                                 // +0xC04
        attack2 = 1;                                  // +0x507: AttackStates plays SPIN 0xB9 or the spinair overlay 0xBA
        const bool air = IsJumpAnim() || IsFallAnim();   // vf12C, vf140
        const f32 dur = F(SequenceDuration(SeqById(air ? 0xBA : 0xB9)));   // FSTP [esp+0x14]
        attack2Start = F(double(dur) + animTime);     // +0xB6C (FLD dur; FADD [+0x78])
        sinceAttack = 0;                              // +0xB54
        (void)SoundAt(0x42, nullptr, 3, 0, node.Get());   // 0x0003FFC0(0x42, node, 3, 0)
        const bool air2 = IsJumpAnim() || IsFallAnim();   // asked again (0x00081332)
        Def* d = level.defs ? level.defs->Find(air2 ? "Bender_SpinAir" : "Bender_Spin") : nullptr;   // 0x0029DB20 / 30
        if (d) {                                      // Session::FindDef 0x000C3F10
            const NiPoint3 pos = GetWorldPosition();  // vf34
            const NiPoint3 fwd = ni::MulVector(rotation, kUnitX);   // vf38, 0x00011770 with 0x002D77E0
            // 0x0004EE90(owner P+0x38, pos, fwd, "SPIN!!!" 0x0029A4C8): Bender_Spin / _SpinAir are ImpactDefs
            auto* idef = dynamic_cast<ImpactDef*>(d);  // (an ImpactDef in every effect library: EffectDefs.cpp 67 / 68)
            if (!idef) NotPorted("Bender vf118: Bender_Spin is not an ImpactDef");
            if (Impact* o = idef ? idef->SpawnUp(level, &inventory, pos, fwd, "SPIN!!!") : nullptr) {
                o->SetFollow(node.Get());             // 0x0004F580 (a temporary NiPointer copy of P+0x2C, 0x00013B10)
                SetNodeScale(*o, 1.0f);               // 0x00073720
                if (IsJumpAnim() || IsFallAnim()) o->SetPosition({0.0f, 5.7f, 0.0f});   // vf1C (0x40B66666)
                o->speed = F(double(o->duration) / dur);   // FLD [o+0x58]; FDIV [esp+0x14]; FSTP [o+0x50]
            }
        }
    }
    xHeld = 1;                                        // 0x00081422
}

// orig: 0x000839D0 (thiscall(u8* flag, hurt, reload), RET 0xC; inventory_batch1_port.md 5.8, checked against the
// assembly). `flag` is never read or written; the result is not read by the caller.
bool Player::AttackStatesFry(bool hurt, bool reload) {
    bool result = false;                              // esp+0x13
    Weapon* W = weapon;                               // edi (read at entry)
    if (heldBy) return true;                          // 0x000839D6: P+0x7A0 (the holder)
    // the take-out / put-away scale: the overlay's elapsed fraction (0x00083A8C / 0x0008437C)
    auto scaleNode = [&](Weapon* w, bool shrink) {
        f32 el = F(double(animTime) - overlayStart);  // FLD +0x78; FSUB +0x4C0; FSTP
        const double dur = SequenceDuration(overlaySeq);   // 0x00036210 (ST0)
        if (double(el) > dur) el = F(dur);            // FCOMP ST1; TEST AH,0x41; JNE; FST
        const double q = double(el) / dur;            // FDIVR
        w->GetNode()->local.scale = F(std::fabs(shrink ? 1.0 - q : q));   // FSUBR [0x00293A98] (put away only); FABS
    };
    auto fullScale = [](Weapon* w) { w->GetNode()->local.scale = F(std::fabs(1.0)); };   // FLD QWORD [0x00293CC0]
    auto tail = [&] {                                 // 0x000843C9
        if (overlayFinished && overlaySeq) {
            keyframes->DeactivateSequence(overlaySeq);   // 0x0012FAB0
            overlaySeq = nullptr;
            attacking504 = attacking505 = 0;
        }
        return result;
    };
    if (attacking504) {                               // the put-away (+0x504)
        if (W && IsPlayingName(W->AnimName(6)) && overlayFinished) {   // 0x00039D60; +0x9C
            attacking504 = 0;
            fullScale(W);
            return tail();
        }
        if (!weapon || !weapon->def->IsType(8)) return tail();
        PlayOverlay(FindSequence(W->AnimName(6)), false);   // 0x00036780(0x00039BA0(name), 0)
        result = true;
        scaleNode(W, true);
        return result;
    }
    if (attacking505 || pendingWeapon) {              // the take-out (0x000842E5)
        if (W && IsPlayingName(W->AnimName(5)) && overlayFinished) {
            attacking505 = 0;
            fullScale(W);
            return tail();
        }
        attacking505 = 1;
        if (pendingWeapon && pendingWeapon->def->IsType(8)) {
            W = pendingWeapon;                        // (edi)
            PlayOverlay(FindSequence(W->AnimName(5)), false);
            result = true;
        }
        if (W) scaleNode(W, false);
        if (result) return result;                    // 0x000843C1
        return tail();
    }
    if (W && W->def->IsType(8)) {                     // 0x00083B22: the weapon states
        W = weapon;                                   // (+0x528 again)
        fullScale(W);
        const u32 t = W->WeaponType();                // vf7C (called up to three times)
        if ((t == 2 || t == 4 || t == 3) && (IsAnimPlaying(0x7F) || IsAnimPlaying(0xA1)) && !finished) {
            ClearOverlay();                           // 0x000363C0 (the shotgun / railgun high landing)
            return tail();
        }
        if (IsChargeAnimPlaying()) {                  // 0x00083380
            ni::NiSequence *loop = nullptr, *release = nullptr, *side = nullptr;   // ebp, edi, ebx
            switch (weapon->WeaponType()) {           // jump table 0x00084410 (type - 1)
            case 1: loop = SeqById(0x90); release = SeqById(0x91); side = SeqById(0xDF); break;
            case 2: loop = SeqById(0x83); release = SeqById(0x84); side = SeqById(0xDE); break;
            case 3: loop = SeqById(0xA5); release = SeqById(0xA6); side = SeqById(0xDD); break;
            case 4: loop = SeqById(0xA7); release = SeqById(0xA8); side = SeqById(0xDC); break;
            default: break;
            }
            if (xHeld) {                              // +0xB9C: still charging
                if (overlaySeq == release && !overlayFinished) return true;
                PlayOverlay(loop, false);
                return true;
            }
            if (overlaySeq == release && !overlayFinished) {
                ClearOverlay();
                return tail();
            }
            if (weapon->WeaponType() == 4) rumble.Add(0.9f, 0.9f, 0.8f, 0);   // 0x0005F000
            PlayOverlay(side, false);
            return true;
        }
        if (WeaponOverlayPlaying() && (!WeaponOverlayPlaying() || !overlayFinished)) {   // vf128 twice (pure)
            if (finished) return tail();              // +0x9B
            return true;
        }
        if (attack1) {                                // +0x506
            const char* name;
            if (WalkRunPlaying()) {                   // vf9C
                if (IsAnimPlaying(0x16) || IsAnimPlaying(0x17)) {
                    PlayOverlay(FindSequence("sidemove_hammer_attack1"), false);   // 0x0029E418
                    return true;
                }
                if (W->WeaponType() == 1 && (IsAnimPlaying(0x15) || IsAnimPlaying(0x8A) || IsAnimPlaying(0xD)))
                    name = W->AnimName(1);
                else
                    name = W->AnimName(3);            // the moving attack
            } else {
                name = W->AnimName(1);
            }
            if (!name) return tail();
            PlayOverlay(FindSequence(name), false);
            return true;
        }
        if (attack2) {                                // +0x507, jump table 0x00084420 (type 0..4)
            switch (weapon->WeaponType()) {
            case 0: {
                const char* name = (WalkRunPlaying() || IsJumpAnim()) ? W->AnimName(4) : W->AnimName(2);   // vf12C
                if (IsPlayingName(name)) return tail();
                PlayOverlay(FindSequence(name), false);
                return true;
            }
            case 1: PlayOverlay(SeqById(0x91), false); return true;
            case 2: PlayOverlay(SeqById(0x84), false); return true;
            case 3: PlayOverlay(SeqById(0xA6), false); return true;
            case 4: PlayOverlay(SeqById(0xA8), false); return true;
            default: return tail();
            }
        }
        if (reload && health > 0.0f && weapon) {      // 0x00083EDD (TEST AH,0x41; JNE)
            Weapon* R = weapon;                       // ebx
            const NiPoint3 pos = GetWorldPosition();  // vf34 (esp+0x18)
            switch (R->WeaponType()) {                // jump table 0x00084434 (type - 1)
            case 1: PlayOverlay(SeqById(0x8D), false); result = true; break;
            case 2: case 4: PlayOverlay(SeqById(0x80), false); result = true; break;
            case 3: PlayOverlay(SeqById(0xA2), false); result = true; break;
            default: break;
            }
            // 0x00083F53 (every type, the default too): the reload sound D+0x60 at the position
            if (const u32 s = static_cast<const WeaponDef&>(*R->def).reloadSound)
                (void)level.Sound().Play3D(s, pos, false);   // Sound_Play3D 0x00083F69
            if (result) return result;                // 0x000843C1
            return tail();
        }
        if (hurt && health > 0.0f && !OnLadder() && !OnLedge()) {   // jump table 0x00084444 (type 0..4)
            const bool left = damageSide < 0.0f;      // +0xBE0; TEST AH,5; JP
            s32 anim;
            switch (weapon->WeaponType()) {
            case 0: anim = left ? 0x6B : 0x6C; break;
            case 1: anim = left ? 0x8E : 0x8F; break;
            case 2: case 4: anim = left ? 0x81 : 0x82; break;
            case 3: anim = left ? 0xA3 : 0xA4; break;
            default: return tail();
            }
            PlayOverlay(SeqById(anim), false);
            return true;
        }
        // the idle overlay (0x0008407B)
        const u32 t2 = W->WeaponType();
        if ((t2 == 2 || t2 == 4) &&
            (WalkRunPlaying() || IsJumpAnim() || IsFallAnim() || IsAnimPlaying(0x78) || IsAnimPlaying(0x7E))) {
            PlayOverlay(nullptr, false);              // 0x00036780(0, 0)
            return tail();
        }
        for (s32 k : {0x8D, 0x80, 0xA2, 0x8E, 0x8F, 0x81, 0x82, 0xA3, 0xA4})   // the reload / hurt overlays
            if (IsAnimPlaying(k)) {
                if (!overlayFinished) return true;
                break;
            }
        if (!WalkRunPlaying() && W->WeaponType() == 1) {
            PlayOverlay(nullptr, false);
            return tail();
        }
        if ((WalkRunPlaying() || IsJumpAnim() || IsFallAnim() || IsLandAnim()) && W->WeaponType() == 0) {   // vf144
            PlayOverlay(SeqById(0xE1), false);
            return true;
        }
        const char* idle = W->AnimName(0);
        if (!idle || W->WeaponType() == 2 || W->WeaponType() == 4) return tail();
        PlayOverlay(FindSequence(idle), true);        // (looping)
        return true;
    }
    if (hurt && health > 0.0f && !OnLadder() && !OnLedge()) {   // no weapon (0x00084277 -> 0x00083FD8)
        PlayOverlay(SeqById(damageSide < 0.0f ? 0x6B : 0x6C), false);   // takedamage_left / _right
        return true;
    }
    return tail();
}

void Player::AttackStates(u8& flag, bool hurt, bool reload) {   // orig: vf184 (Player 0x00052480, Fry 0x000839D0)
    if (type == 1) {
        (void)AttackStatesFry(hurt, reload);          // (the result is not read by UpdateAnimation)
        return;
    }
    auto play = [&](s32 anim, bool once, bool intr, bool force) { PlayAnimById(anim, once, intr, force, 0.0f); };
    if (IsAnimPlaying(0xBB)) {                        // Bender's spin start
        if (finished) {                               // 0x000524C1: the spin loop overlay, the flame, the sound
            PlayOverlay(SeqById(0xBC), true);         // 0x00036780(0x00033E00(0xBC), 1)
            Def* d = level.defs ? level.defs->Find("Bender_SpinFlame") : nullptr;   // 0x000C3F10 (0x0029A4D0)
            if (d) {
                const NiPoint3 pos = GetWorldPosition();   // vf34
                const NiPoint3 fwd = ni::MulVector(rotation, kUnitX);   // vf38, 0x00011770 with 0x002D77E0
                // 0x0004EE90(owner P+0x38, pos, fwd, "SPIN!!!" 0x0029A4C8): an ImpactDef in every effect library
                auto* idef = dynamic_cast<ImpactDef*>(d);
                if (Impact* o = idef ? idef->SpawnUp(level, &inventory, pos, fwd, "SPIN!!!") : nullptr) {
                    o->SetFollow(node.Get());         // 0x0004F580 (a temporary NiPointer copy of P+0x2C, 0x00013B10)
                    o->duration = 5.0f;               // +0x58 = 0x40A00000
                }
            }
            spinVoice = static_cast<u32>(SoundAt(0x3F, nullptr, 4, 0, node.Get()));   // 0x0003FFC0(0x3F, node, 4, 0)
            return;
        }
        flag = 0;
    } else if (IsAnimPlaying(0xBC)) {                 // 0x0005258A: the spin loop
        const double el = double(animTime) - overlayStart;   // +0x78 - +0x4C0
        if (el > 4.0f && !(el > double(g_frameStep) + 4.0f)) {   // FCOM [0x00293C1C]; FCOMPP with dt + 4 (once)
            level.Sound().StopVoice(static_cast<s32>(spinVoice));   // Voice_Stop 0x000C9B10 (no Voice_Exists)
            spinVoice = static_cast<u32>(SoundAt(0x40, nullptr, 3, 0, node.Get()));   // 0x0003FFC0(0x40, node, 3, 0)
        }
        if (!(double(animTime) - overlayStart > 5.0f)) {   // FCOMP [0x00293C24]; TEST AH,0x41; JNE
            hitActive = 1;                            // +0x5D1
            return;
        }
        level.Sound().StopVoice(static_cast<s32>(spinVoice));
        spinVoice = 0;
        (void)SoundAt(0x41, nullptr, 3, 0, node.Get());   // 0x0003FFC0(0x41, node, 3, 0) (the voice not kept)
        ClearOverlay();                               // 0x000363C0
        play(0xBD, true, false, true);                // 0x0003A130(0x00033E00(0xBD), 1, 0, 1, 0): SPINFLAME_powerdown
        if (body->velocity.y > 0.0f) body->velocity.y = 0.0f;   // B+0x2C: TEST AH,0x41; JNE
        flag = 0;
    } else if (IsAnimPlaying(0xBD)) {
        if (finished) play(0xBE, true, false, false); // dizzy
        flag = 0;
    } else if (attack1) {
        if (type == 2) {
            static constexpr s32 kId[5] = {0xA9, 0xAA, 0xAB, 0xAC, 0xB3};   // table 0x00052BEC
            const u8 k = kickTable[kickRow][kickIndex];
            if (k <= 4 && !IsAnimPlaying(kId[k])) play(kId[k], true, false, true);
        } else if (type == 3) {
            if (!IsAnimPlaying(0xB8)) {
                play(0xB8, true, false, true);
                BeginRootMotion();                    // after the play
            }
        } else {
            play(0x2F, true, false, false);           // attack1
        }
        flag = 0;
    } else if (attack2) {
        if (type == 2) {
            static constexpr s32 kId[7] = {0xB1, 0xB2, 0xAF, 0xB0, 0xAD, 0xAE, 0xB4};   // table 0x00052C00
            const u8 k = punchTable[punchRow][punchIndex];
            if (k <= 6 && !IsAnimPlaying(kId[k])) {
                if (k == 6) BeginRootMotion();        // chpunch: before the play
                play(kId[k], true, false, true);
            }
        } else if (type == 3) {
            if (IsJumpAnim() || IsFallAnim()) {
                if (!IsAnimPlaying(0xBA)) PlayOverlay(SeqById(0xBA), false);
            } else if (!IsAnimPlaying(0xB9)) {
                play(0xB9, true, false, true);
            }
        } else {
            play(0x34, true, false, false);           // attack2
        }
        flag = 0;
    } else if (b9D) {
        if (!IsAnimPlaying(0xD1)) {
            play(0xD1, true, false, true);
            BeginRootMotion();
        }
        flag = 0;
    } else if (special50A) {
        if (type == 2) {
            if (!IsAnimPlaying(0xB5)) play(0xB5, true, false, true);   // RHOUSE
        } else if (type == 3) {
            if (!IsAnimPlaying(0xBB)) play(0xBB, true, false, true);
        }
        flag = 0;
    } else if (special50B) {
        if (!IsAnimPlaying(0xB6)) {                   // runkick
            BeginRootMotion();
            play(0xB6, true, false, true);
        }
        flag = 0;
    } else if (special50C) {
        if (!IsAnimPlaying(0xB7)) play(0xB7, true, false, true);   // jumpkick
        flag = 0;
    } else if (b44 > 0.2f) {                          // 0x00293AB4
        play(0x1A, false, true, false);               // turnr
        flag = 0;
    } else if (b44 < -0.2f) {                         // 0x00297640
        play(0x19, false, true, false);               // turnl
        flag = 0;
    } else if (IsAnimPlaying(0xD1)) {
        if (finished) {
            play(0xD2, true, true, true);
            body->velocity.y = -50.0f;                // 0xC2480000
        }
        flag = 0;
    } else if (IsAttackAnim() && !finished) {
        flag = 0;                                     // wait for the attack to finish
    }
    if (overlayFinished && overlaySeq) {              // the tail 0x00052BA7
        keyframes->DeactivateSequence(overlaySeq);
        overlaySeq = nullptr;
        attacking504 = attacking505 = 0;
    }
}

void Player::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 (0x00052190; Fry 0x00085D40)
    (void)keyTime;
    if (type == 1) {                                  // Fry 0x00085D40, then the base
        if (crt::StrICmp(text, "morph:firew") == 0) { // 0x002974C0
            FireKey();                                // 0x00084EF0
            return;
        }
        if (crt::StrICmp(text, "morph:putaway") == 0) {   // 0x0029E444
            (void)SetWeapon(nullptr);                 // vfB4(0)
            return;
        }
        if (crt::StrICmp(text, "morph:takeout") == 0) {   // 0x0029E434
            const NiPoint3 pos = GetWorldPosition();  // vf34
            SoundAt(0x47, &pos, 3);                   // def 0x0003FEA0
            (void)SetWeapon(pendingWeapon);           // vfB4(+0x524)
            pendingWeapon = nullptr;
            ShowWeaponBox(level, weapon, ammo);       // 0x00085DDA (+0x528)
            return;
        }
    }
    const bool left = crt::StrICmp(text, "morph:lfdown") == 0;   // orig: 0x002974DC
    if (left || crt::StrICmp(text, "morph:rfdown") == 0) {   // orig: 0x002974CC
        const u32 kind = level.footstepMaterials[body->shape.contactFlags[0] & 3];   // S+0x160[shape+0x54 & 3]
        AiNoise(level, GetWorldPosition(), 0.2f);     // (S+0x114)->0x00031420(vf34(), 0.2f) (0x000521E7 / 0x0005225D)
        Footstep(kind, left);                         // orig: vf17C
        footstepRight = left ? 1 : 0;                 // orig: +0xBEC
        b93 = b94 = 0;
        return;
    }
    if (crt::StrICmp(text, "morph:hit") == 0) {       // 0x002974A8
        if (weapon && weapon->WeaponType() == 0 &&    // 0x0005229F: the hammer swing sound
            (IsPlayingName("hammer_attack1") || IsPlayingName("move_hammer_attack1") ||   // 0x0029A4B8 / 0x0029A4A4
             IsPlayingName("hammer_attack2"))) {      // 0x0029A494
            const NiPoint3 pos = GetWorldPosition();  // vf34
            (void)SoundAt(0x3C, &pos, 3, 0, nullptr); // def 0x0003FEA0
        }
        HitSpheres(1);                                // vfD8(1)
        return;
    }
    if (crt::StrICmp(text, "morph:hitstart") == 0) {  // 0x00297498
        HitStart();                                   // vfDC 0x00033DF0
        return;
    }
    if (crt::StrICmp(text, "morph:hitend") == 0) {    // 0x00297488
        HitEnd();                                     // vfE0 0x000360B0
        return;
    }
    if (crt::StrICmp(text, "morph:blowstart") == 0) { // 0x0029A484
        ni::NiAVObject* n = GetObjectByName(node.Get(), "smokenull_X");   // node vf54 (0x0029A478)
        Def* sd = level.defs ? level.defs->Find("effect_smoke") : nullptr;   // 0x002976C4
        auto* e = dynamic_cast<SpriteEmitterDef*>(sd);
        if (!sd || !n) return;
        if (!e) {
            NotPorted("Player: morph:blowstart with a non-sprite effect_smoke");
            return;
        }
        const NiPoint3 pos = n->world.translate;      // +0xA0
        const NiPoint3 dir = ni::MulVector(n->world.rotate, kUnitX);   // +0x70 copy, 0x00011770
        e->current.gravity = -2.0f;                   // vf90 0x0006CB70 (0xC0000000)
        e->current.value4C = 2.0f;                    // vf8C 0x0006CB60
        e->current.range28 = {3.0f, 10.0f};           // vf68 0x0006CA90
        e->current.range40 = {0.0f, 0.5f};            // vf80 0x0006CB30
        e->current.count = 20;                        // vf88 0x0006CB50
        e->current.range38 = {0.1f, 0.1f};            // vf7C 0x0006CAE0 -> vf78
        e->current.range30 = {0.5f, 1.0f};            // vf70 0x0006CAC0
        (void)e->SpawnEffect(level, pos, dir);        // vf20
        return;
    }
    (void)crt::StrICmp(text, "morph:land");           // 0x0029A46C: compared, nothing else
}

// ---- the hit spheres (docs/analysis/player_moves_port.md 6) ----------------------------------------------------------
void Player::HitSpheres(u32 n) {                      // orig: vfD8 (Leela 0x0008A9F0 / Bender 0x00081440 / 0x000514E0)
    ni::NiAVObject* tip0 = tips.empty() ? nullptr : tips[0];   // (+0x78C)[0]
    if (type == 2) {                                  // Leela 0x0008A9F0 (0x0003E220(name, a, n) = HitSweep(a, a, n, 0))
        TipToKeyTime(animTime, tip0);                 // 0x00034A10(+0x78, tip)
        if (IsAnimPlaying(0xAA) || IsAnimPlaying(0xAC)) return HitSweep("tip", 2.5f, 2.5f, n, false);   // hkr / mkr
        if (IsAnimPlaying(0xA9) || IsAnimPlaying(0xAB)) return HitSweep("tip", 2.5f, 2.5f, n, false);   // hkl / mkl
        if (IsAnimPlaying(0xAE) || IsAnimPlaying(0xB0) || IsAnimPlaying(0xB2) || IsAnimPlaying(0xB4))
            return HitSweep("tip", 3.0f, 3.0f, n, false);   // upr / spr / jpr / chpunch
        if (IsAnimPlaying(0xAD) || IsAnimPlaying(0xAF) || IsAnimPlaying(0xB1))
            return HitSweep("tip", 3.0f, 3.0f, n, false);   // upl / spl / jpl
        if (IsAnimPlaying(0xB5)) {                    // RHOUSE
            TipToKeyTime(animTime, GetObjectByName(node.Get(), "joint14"));   // node vf54 (0x0029F3C0)
            return HitSweep("joint14", 8.0f, 8.0f, n, false);
        }
        if (IsAnimPlaying(0xB3)) return HitSweep("tip", 3.0f, 3.0f, n, false);   // CHKICK
        if (IsAnimPlaying(0xB6)) {                    // runkick
            TipToKeyTime(animTime, GetObjectByName(node.Get(), "R_ANKLE"));   // 0x0029F3B8
            return HitSweep("R_ANKLE", 2.5f, 2.5f, n, false);
        }
        if (IsAnimPlaying(0xB7)) return HitSweep("tip", 2.5f, 2.5f, n, false);   // jumpkick
        return;
    }
    if (type == 3) {                                  // Bender 0x00081440 (no tip update)
        if (IsAnimPlaying(0xD2) || IsAnimPlaying(0xD3)) return HitSweep("bottom", 15.0f, 15.0f, 4, false);   // n = 4
        if (IsAnimPlaying(0xB8)) {                    // CHARGE
            HitSweep("center", 4.0f, 4.0f, n, false); // 0x0003E220("center" 0x00295D90, 4.0, n)
            if (!body->supported) return;             // body vf48
            const u32 kind = level.footstepMaterials[body->shape.contactFlags[0] & 3];   // S+0x160[shape+0x54 & 3]
            const NiPoint3 g = GetWorldPosition();    // vf34
            const NiPoint3 at{g.x, F(double(g.y) + 0.25f), g.z};   // [0x00293B50]
            const NiPoint3 fwd = ni::MulVector(rotation, kUnitX);   // vf38, 0x00011770
            if (kind == 1 || kind == 2 || kind == 3) {
                if (level.defs && level.defs->Find("Punch_Particles"))   // 0x0029A5EC (looked up twice)
                    (void)level.defs->Find("Punch_Particles")->SpawnEffect(level, at, fwd);   // vf20
                return;
            }
            Def* sd = level.defs ? level.defs->Find("effect_smoke") : nullptr;   // 0x002976C4
            if (!sd) return;
            auto* e = dynamic_cast<SpriteEmitterDef*>(sd);
            if (!e) {
                NotPorted("Bender vfD8: a non-sprite effect_smoke");
                return;
            }
            switch (kind) {                           // vfC4 0x0006D140 (configured and current colour)
            case 5: e->SetColor(0.3f, 0.12f, 0.9f); break;
            case 9: e->SetColor(0.5f, 0.4f, 0.12f); break;
            case 10: e->SetColor(0.65f, 0.52f, 0.266f); break;
            default: e->SetColor(0.3f, 0.12f, 0.12f); break;
            }
            e->current.count = 20;                    // vf88 0x0006CB50
            e->current.range38 = {2.0f, 2.0f};        // vf7C 0x0006CAE0 -> vf78
            e->current.range40 = {0.0f, 0.0f};        // vf84 0x0006CB10 -> vf80
            e->current.gravity = 0.5f;                // vf90
            e->spriteCurrent.size = {1.5f, 3.0f};     // vfD0 0x0006CCD0 (+0xE0 / +0xE4)
            e->current.range28 = {1.0f, 5.0f};        // vf68
            e->current.range30 = {0.2f, 0.4f};        // vf70
            (void)e->SpawnEffect(level, at, fwd);     // vf20 0x0006F2A0
            return;
        }
        if (IsAnimPlaying(0xB9) || IsAnimPlaying(0xBA)) return HitSweep("upper", 6.0f, 6.0f, n, false);   // SPIN
        if (IsAnimPlaying(0xBC)) return HitSweep("center", 8.0f, 8.0f, n, false);   // SPINFLAME_loop
        return;
    }
    TipToKeyTime(animTime, tip0);                     // the base 0x000514E0 (Fry, the rodent)
    HitSweep("tip", 3.0f, 3.0f, n, false);
}

void Player::DealDamage(Character* t, f32 amount) {   // orig: vfCC 0x000503E0
    if (!t || !level.IsCharacter(t)) return;          // 0x00033EE0
    if (!(amount > 0.0f)) return;                     // FCOMP [0x00293AB8]; TEST AH,0x41; JNE
    t->Damage(amount, this);                          // vf7C(amount, this)
}

bool Player::IsPunchSeq(const ni::NiSequence* seq) const {   // orig: 0x0004FEB0
    for (s32 k : {0xAE, 0xAD, 0xB0, 0xAF, 0xB2, 0xB1, 0xB4})
        if (seq == SeqById(k)) return true;
    return false;
}

bool Player::IsKickSeq(const ni::NiSequence* seq) const {   // orig: 0x0004FF40
    for (s32 k : {0xAA, 0xA9, 0xAC, 0xAB, 0xB6, 0xB7, 0xB3, 0xB5})
        if (seq == SeqById(k)) return true;
    return false;
}

bool Player::IsSecondAttack(const ni::NiSequence* seq) const {   // orig: 0x0004FD90
    if (type == 3) return IsAnimPlaying(0xD1) || IsAnimPlaying(0xD2) || IsAnimPlaying(0xD3) || IsAnimPlaying(0xB8);
    if (type == 2) {
        for (s32 k : {0xAD, 0xAF, 0xB1, 0xAE, 0xB0, 0xB2, 0xB4})
            if (IsAnimPlaying(k)) return true;
        return false;
    }
    if (type == 1 && weapon)                          // 0x0004FE61 (a null seq matches a missing sequence)
        return seq == FindSequence(weapon->AnimName(2)) || seq == FindSequence(weapon->AnimName(4));
    return false;
}

void Player::HitTrigger(ScriptTrigger& t, NiPoint3& dir) {   // orig: vfD0 0x000519F0 (object_gaps.md 21.2)
    (void)dir;                                        // (not written)
    ni::NiSequence* seq = curSeq;                     // +0xA4
    if (weapon && overlaySeq) seq = overlaySeq;       // +0x528 && +0xB0
    f32 damage = 0;                                   // esp+8
    if (seq) {                                        // CharacterDef 0x0003FD20, AnimEntry::GetDamage 0x0003EE90; FSTP
        const AnimEntry* e = EntryOf(seq);            // (a missing entry is dereferenced by the original)
        damage = e && e->hasDamage ? e->damage : 0.0f;
    }
    double x = damage;                                // FLD [esp+8]
    if (IsAnimPlaying(0xBC)) {                        // 0x00033E20: SPINFLAME_loop takes the whole health
        x = t.health;                                 // FLD t+0x6C
        // the trigger leaves the hit list: the first entry equal to t is zeroed (+0x5C8 decremented), the count
        // shrinks when it was the last one
        for (std::size_t i = 0; i < hitList.size(); ++i)
            if (hitList[i] == &t) {
                hitList[i] = nullptr;
                if (i + 1 == hitList.size()) hitList.pop_back();
                break;
            }
    }
    const double h = double(t.health) - x;            // FSUBR t+0x6C; FST
    t.health = F(h);
    if (h < 0.0) t.health = 0.0f;                     // FCOMP 0 (unrounded); TEST AH,5; JP
}

void Player::ApplyHit(GameObject* t, NiPoint3& dir, u32 n) {   // orig: vfD4 0x00051520
    ni::NiSequence* seq = curSeq;                     // +0xA4
    if ((weapon && overlaySeq) || (type == 3 && overlaySeq)) seq = overlaySeq;   // +0xB0
    f32 damage = 0;                                   // esp+0x10
    if (seq) {
        // CharacterDef 0x0003FD20, AnimEntry::GetDamage 0x0003EE90 (the block +0x40, else 0.0; a missing entry is
        // dereferenced by the original)
        const AnimEntry* e = EntryOf(seq);
        damage = e && e->hasDamage ? e->damage : 0.0f;
        if (IsPunchSeq(seq)) ++punchHits;             // +0xBAC
        else if (IsKickSeq(seq)) ++kickHits;          // +0xBB0
    }
    if (!t) return;                                   // the wall: only the combo counters
    if (t->def->IsType(1)) {                          // a character
        auto* c = static_cast<Character*>(t);
        NiPoint3 imp{F(double(dir.x) * 10.0f), F(double(dir.y) * 10.0f), F(double(dir.z) * 10.0f)};   // [0x00293C2C]
        if (n & 4) c->Vf80(this);                     // vf80
        if (n & 2) {
            if (c->CanBePushed() && c->body) AddBodyVelocity(*c->body, imp);   // vfA4; body vf50
        } else {
            if (IsAnimPlaying(0xBC)) {                // SPINFLAME_loop: the whole health, 0 against 55555
                damage = c->health;                   // FST
                if (c->health == 55555.0f) damage = 0;   // [0x00295594]; TEST AH,0x44; JP
            }
            bool pushed = false;
            if (c->CanBePushed()) {
                pushed = true;
                double s = double(damage) * 5.0f;     // [0x00293C24]
                if (s < 5.0f) s = 5.0f;               // FCOM; TEST AH,5; JP
                else if (s > 300.0f) s = 300.0f;      // [0x002976F0]; TEST AH,0x41; JNE
                if (c->body) AddBodyVelocity(*c->body, {F(s * dir.x), F(s * dir.y), F(s * dir.z)});   // body vf50
            }
            c->Damage(damage, this);                  // vf7C
            if (c->health < 0.0f || c->health == 0.0f) {   // TEST AH,0x41; JP (NaN: alive)
                c->deathKind = 0;                     // +0x778
                ++kills;                              // +0xBF0
                if (pushed) c->TurnToPoint(GetWorldPosition());   // vf34; 0x0003ABA0
            }
            if (type == 3) {
                if (IsAnimPlaying(0xBC)) c->deathKind = 2;
                if (IsAnimPlaying(0xD2) || IsAnimPlaying(0xD3)) {
                    c->deathKind = 3;
                } else if (IsAnimPlaying(0xB8)) {
                    rumble.Add(0.5f, 0.5f, 0.2f, 0);  // 0x0005F000
                    c->deathKind = 2;
                }
            }
            const NiPoint3 p = GetWorldPosition();    // vf34 (in each branch)
            auto skel = [&] {                         // _stricmp(D+0x10, "skel_hth" 0x00297728 / "skel_ranged" 0x0029771C)
                return crt::StrICmp(c->def->name.c_str(), "skel_hth") == 0 ||
                       crt::StrICmp(c->def->name.c_str(), "skel_ranged") == 0;
            };
            if (IsPunchSeq(curSeq)) (void)SoundAt(skel() ? 0x28 : 0x27, &p, 3, 0, nullptr);
            else if (IsKickSeq(curSeq)) (void)SoundAt(skel() ? 0x2C : 0x2B, &p, 3, 0, nullptr);
            else (void)SoundAt(0x24, &p, 3, 0, nullptr);
        }
    } else if (t->def->IsType(0x40)) {                // a destructable: vf68 (0x00063A50 Break)
        if (auto* d = dynamic_cast<Destructable*>(t)) d->Break();
        else NotPorted("Player::ApplyHit: vf68 of a 0x40 object");
    }
    if (IsSecondAttack(type == 1 ? overlaySeq : curSeq)) ++hitsX;   // 0x0004FD90 -> +0xC10
    else ++hitsA;                                     // +0xC0C
    switch (type) {
    case 1:                                           // 0x000519AA: the hammer
        if (weapon && weapon->WeaponType() == 0) rumble.Add(0.6f, 0.6f, 0.3f, 0);
        break;
    case 2:
        if (IsPunchSeq(curSeq)) rumble.Add(0.4f, 0.4f, 0.3f, 0);
        if (IsKickSeq(curSeq)) rumble.Add(0.4f, 0.4f, 0.3f, 0);
        break;
    case 3:
        if (IsAnimPlaying(0xB9) || IsAnimPlaying(0xBA)) rumble.Add(0.6f, 0.6f, 0.3f, 0);
        break;
    default: break;
    }
}

// ---- damage (docs/analysis/combat_ai_port.md 9.6) ----------------------------------------------------------------
void Player::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x00051AD0
    if (level.playerNoDamage694 || level.playerNoDamage695) return;   // DAT_002DD694 / DAT_002DD695
    if (type == 3 && attacker && attacker != this &&  // Bender spinning
        (IsAnimPlaying(0xD2) || IsAnimPlaying(0xBC) || IsAnimPlaying(0xBB) || IsAnimPlaying(0xBD)))
        return;
    if (type == 2 && IsAnimPlaying(0xB5) && attacker) return;
    if (type == 1 && chicken && chicken->active) {    // orig: 0x00051B7B (P+0x7B0 && 0x00042690)
        chicken->Impact(amount, attacker);            // orig: K->0x000436C0(amount, attacker)
        Character::Damage(amount, attacker);          // 0x000354B0
        return;
    }
    const bool alive = health > 0.0f;                 // TEST AH,0x41; JE
    Character::Damage(amount, attacker);              // 0x000354B0
    if (amount > 10.0f) rumble.Add(0.5f, 0.5f, 0.3f, 0);   // [0x00293C2C]; 0x00051C09 (0x3E99999A)
    if (health < 0.0f || health == 0.0f) {            // 0x00051C0E: TEST AH,0x41; JP (NaN: alive)
        if (alive) ++deaths;                          // 0x00051C21: +0xBF4
        deathKind = 0;                                // +0x778
        (void)SetWeapon(nullptr);                     // vfB4(0)
        ClearOverlay();                               // 0x000363C0
        if (view) view->SetActive(0);                 // +0x7B4: 0x00045ED0(0)
        if (body->velocity.y > 0.0f) body->velocity.y = 0;   // B+0x2C: TEST AH,0x41; JNE (only on >)
    }
    if (level.IsCharacter(attacker)) {                // 0x00033EE0
        NiPoint3 f = ni::MulVector(rotation, {0.0f, 0.0f, 1.0f});   // vf38, 0x002D77F8
        const NiPoint3 me = GetWorldPosition(), at = attacker->GetWorldPosition();   // vf34 (this first)
        NiPoint3 u{F(double(at.x) - me.x), F(double(at.y) - me.y), F(double(at.z) - me.z)};
        (void)ni::Unitize(f);
        (void)ni::Unitize(u);
        damageSide = F((double(f.z) * u.z + double(f.x) * u.x) + double(f.y) * u.y);   // +0xBE0
    }
    StopVoice(level, spinVoice);                      // 0x00051D0B: +0xC18 (Voice_Exists, Voice_Stop, 0)
}

bool Player::CanBePushed() const {                    // orig: vfA4 0x000502C0
    if (chicken && chicken->active) return false;     // orig: P+0x7B0 && 0x00042690
    if (cannon && cannon->IsActive()) return false;   // orig: P+0x7A8 && 0x00032E40 (the level2-5 cannon)
    if (type == 2 && IsAnimPlaying(0xB4)) return false;
    return Character::CanBePushed();                  // 0x00033D60
}

void Player::Footstep(u32 kind, bool left) {          // orig: vf17C 0x00051DA0 (Player / Bender / rodent)
    const f32 side = left ? 1.0f : -1.0f;             // FILD byte; FCOMP 0.0; TEST AH,0x44; JP (0 -> -1.0)
    // the right vector (row . UZ: (y term + z term) + x term; x / y stay on the x87 stack) times the side
    const ni::NiMatrix3 R = rotation;                 // vf38
    const double rx = (double(kUnitZ.y) * R.m[0][1] + double(kUnitZ.z) * R.m[0][2]) + double(kUnitZ.x) * R.m[0][0];
    const double ry = (double(kUnitZ.y) * R.m[1][1] + double(kUnitZ.z) * R.m[1][2]) + double(kUnitZ.x) * R.m[1][0];
    const f32 rz = F((double(kUnitZ.y) * R.m[2][1] + double(kUnitZ.z) * R.m[2][2]) + double(kUnitZ.x) * R.m[2][0]);
    const NiPoint3 off{F(rx * side), F(ry * side), F(double(rz) * side)};
    const NiPoint3 p = GetWorldPosition();            // vf34
    const NiPoint3 pos{F(double(p.x) - off.x), F((double(p.y) - off.y) + 0.2f), F(double(p.z) - off.z)};   // [0x00293AB4]
    if (type == 4) {                                  // 0x00051E9C
        if (kind == 7) {                              // the surface kind from S+0x160
            if (Def* sd = level.defs ? level.defs->Find("Water_Splash") : nullptr)   // 0x00295CB8
                if (auto* splash = dynamic_cast<SplashDef*>(sd)) {   // (the original calls 0x00073880 directly)
                    const NiPoint3 wp = GetWorldPosition();   // vf34 -> esp+0x38
                    NiPoint3 up{0.0f, 1.0f, 0.0f};    // esp+0x20 (becomes UNIT_Z: 0x00073880)
                    Owner* stageOwner = level.currentStage;   // S+0x5C ? +4 : 0
                    SetNodeScale(*splash->SpawnDir(level, stageOwner, wp, up, "Splashive"), 2.0f);   // 0x00073720(2.0)
                }
            return;
        }
        rumble.Add(0.4f, 0.4f, 0.15f, 3);             // 0x00051F4D: 0x0005F000(+0xA04; 0.4, 0.4, 0.15, 3)
        return;
    }
    if (type != 3) return;                            // 0x00051F72
    const bool a = IsAnimPlaying(0x2D), b = IsAnimPlaying(0x2C);   // (all three evaluated)
    if (!(a || b || body->supported)) return;
    Def* sd = level.defs ? level.defs->Find("effect_smoke") : nullptr;   // orig: Session::FindDef 0x000C3F10
    if (!sd) return;
    auto* e = dynamic_cast<SpriteEmitterDef*>(sd);
    auto color = [&](f32 r, f32 g, f32 bl) {          // vfC4 0x0006D140 (configured and current)
        if (e) e->SetColor(r, g, bl);
    };
    switch (kind) {                                   // orig: jump table 0x00052160 (kind - 1, 0..9)
    case 1: SoundAt(0x2E, &pos, 3); return;
    case 3: SoundAt(0x31, &pos, 3); return;
    case 5: FootstepPlay(level, kind, pos); color(0.3f, 0.12f, 0.9f); break;
    case 6: FootstepPlay(level, kind, pos); color(0.3f, 0.12f, 0.12f); break;
    case 8: SoundAt(0x2D, &pos, 3); color(0.3f, 0.12f, 0.12f); break;
    case 9: FootstepPlay(level, kind, pos); color(0.5f, 0.4f, 0.12f); break;
    case 10: SoundAt(0x2D, &pos, 3); color(0.65f, 0.52f, 0.266f); break;
    default: FootstepPlay(level, kind, pos); return;  // 2, 4, 7, 0, > 10 (0x00052146)
    }
    if (!e) {
        NotPorted("Player: Bender's footstep smoke with a non-sprite effect_smoke");
        return;
    }
    e->current.count = 2;                             // vf88
    e->current.range38 = {2.0f, 2.0f};                // vf7C -> vf78
    e->current.range40 = {0.0f, 0.0f};                // vf84 -> vf80
    e->current.gravity = 0.5f;                        // vf90
    e->spriteCurrent.size = {1.0f, 2.0f};             // vfD0
    e->current.range28 = {1.0f, 5.0f};                // vf68
    e->current.range30 = {0.3f, 0.4f};                // vf70
    (void)e->SpawnEffect(level, pos, kUnitY);         // vf20(&pos, &UY copy)
}

// ---- 4 the action / animation state machine ----------------------------------------------------------------------
void Player::UpdateAnimation(f32 dt) {                // orig: 0x0005A790
    if (level.input && level.input->activePad && !AllPadsIdle(*level.input)) idleTime = 0;
    if (type == 1 && weapon && !OnLadder() && !OnLedge() && !chicken) {   // 0x0005A7D2 (P+0x7B0: the object itself)
        // 4.1.1 WeaponClearance (0x0005A81F..0x0005AA7A, character_update.md 4.1.1): the gun is pushed out of
        // physics objects and walls
        f32 dist = 4.0f, k = 0.5f;                    // esp+0x24 / esp+0x38
        switch (weapon->WeaponType()) {               // vf7C, jump table 0x0005D680
        case 0: dist = 2.5f, k = 0.2f; break;
        case 1: case 2: case 4: dist = 4.25f, k = 0.2f; break;
        case 3: dist = 5.0f, k = 0.2f; break;
        default: break;
        }
        const NiPoint3 fw = ni::MulVector(rotation, kUnitX);   // vf38 (0x00033D40 copy) * UNIT_X -> esp+0x40
        const NiPoint3 S = ni::MulVector(rotation, {0.0f, 0.0f, k});   // -> esp+0x4C
        const NiPoint3 P = CharacterPoint();          // vf78
        const NiPoint3 O{F(double(S.x) + P.x), F(double(S.y) + P.y), F(double(S.z) + P.z)};   // 0x00011500 -> esp+0x94
        dist = F(double(dist) + 1.5f);                // [0x00293BEC]; FST (dist) / FSTP (len)
        f32 len = dist;
        if (phys::PhysObject* hit = level.physic ? level.physic->RayCastObjects(nullptr, O, fw, len, true) : nullptr)
            if (!hit->owner || !hit->b1A) {           // Physic vf04 0x000AE8D0 (every object); +0x20 / +0x1A
                const f32 pen = F(double(dist) - len);
                const f32 px = F(double(-fw.x) * pen), py = F(double(-fw.y) * pen), pz = F(double(-fw.z) * pen);
                const f32 inv = F(1.0 / double(g_frameStep));   // FLD 1.0; FDIV [0x002D67E8]
                AddBodyVelocity(*body, {F(double(px) * inv), F(double(py) * inv), F(double(pz) * inv)});   // body vf50
            }
        const f32 maxLen = F(double(dist) - 1.25f);   // [0x00297024]; FST (dist) / FSTP (len)
        f32 len2 = maxLen;
        const ni::RayHit wall = ni::RayCast(O, fw, len2, true, 3);   // 0x000FB780
        if (len2 < maxLen && wall) {                  // TEST AH,5; JP
            NiPoint3 N = wall.Normal();               // 0x000FA8C0
            N.y = 0.0f;
            (void)ni::Unitize(N);
            const double pd = double(maxLen) - len2;  // (not rounded)
            const f32 qx = F(double(N.x) * pd), qy = F(double(N.y) * pd), qz = F(double(N.z) * pd);
            const f32 inv = F(1.0 / double(g_frameStep));
            AddBodyVelocity(*body, {F(double(qx) * inv), F(double(qy) * inv), F(double(qz) * inv)});
        }
    }
    u8 flag = 1;
    const NiPoint3 vel = body->velocity;
    const f32 velY = vel.y;
    NiPoint3 D{thinkVel.x, 0, thinkVel.z};
    const f32 posY = GetWorldPosition().y;
    const f32 dSpeed = F(ni::Unitize(D));
    NiPoint3 hv{vel.x, 0, vel.z};
    const f32 hSpeed = F(ni::Unitize(hv));
    const NiPoint3 local = TransposeMul(rotation, D);
    const f32 fwd = F((double(kUnitX.y) * local.y + double(kUnitX.z) * local.z) + double(kUnitX.x) * local.x);
    const f32 side = F((double(kUnitZ.y) * local.y + double(kUnitZ.z) * local.z) + double(kUnitZ.x) * local.x);
    if (!IsDead() && model && !firstPerson) model->flags &= ~ni::kNiAVHidden;
    desiredSpeed = dSpeed;
    // 4.1.3 the HUD notifications (0x0005ABE9..0x0005ACE6; the HUD's state is render only)
    if (!(health4F8 == health) && level.hud)          // TEST AH,0x44; JNP: HUD("health", "set", health / max)
        level.hud->Command("health", "set", F(double(health) / maxHealth));   // 0x000E5B90 (0x00297208, 0x0029A6A0)
    bool reload = false;                              // [esp+0x20]: an ammo count went up
    for (u32 i = 0; i < 5; ++i) {                     // +0xAD0[i] against +0xAE4[i] (the fifth slot: FsQuickLoad only)
        const s32 cur = ammo[i];
        if (ammoShown[i] == cur) continue;
        if (weapon && weapon->ammoSlot == static_cast<s32>(i) && level.hud)   // 0x0005AC3B: (+0x528)+0x74 == i
            level.hud->Command("weapons", "ammo", static_cast<f32>(cur));   // 0x000E5B90 (0x00297214): FILD
        if (ammoShown[i] < cur) reload = true;        // JGE
        ammoShown[i] = cur;
    }
    if (!(charges == chargesShown)) {                 // TEST AH,0x44; JNP: HUD("charge", "charge", +0xAF8)
        if (level.hud) level.hud->Command("charge", "charge", charges);   // 0x000E5B90 (0x0029A698 twice)
        chargesShown = charges;
    }
    const bool hurt = health < health4F8;
    health4F8 = health;
    const f32 thrRun = F((double(cdef.runSpeed) - cdef.walkSpeed) * 0.5f + cdef.walkSpeed);
    const f32 thrWalk = F((double(cdef.walkSpeed) - cdef.sneakSpeed) * 0.5f + cdef.sneakSpeed);
    const f32 thrMove = F(double(cdef.sneakSpeed) * 0.5f);
    if (chicken && chicken->active) return;           // orig: 0x0005AD27 -> 0x0005D667 (riding: no animation state)
    const double fallH = double(spawnY) - posY;
    (void)hSpeed;

    // ---- phase A
    if (type == 4) {
        RodentStates(flag, dSpeed, velY, side);       // orig: 0x0005AD5A (clears the flag on every path)
    } else if (locked) {                              // 0x0005AD64: the interaction state (natives 123-125, 0x0003AAC0)
        // it lasts while one of the three animations plays (0x00039D60: current or overlay) until it has finished;
        // (sic) the third name is "push_button" while FsPushButton plays "press_button": that state ends at once
        if (IsPlayingName("wheel_turn") || IsPlayingName("pulllever") || IsPlayingName("push_button")) {
            if (finished) locked = 0;                 // +0x9B
        } else {                                      // 0x00295D4C / 0x00295D30 / 0x00297298
            locked = 0;
        }
    } else if (heldBy) {                              // 0x0005ADC3: vf180 (object_gaps.md 16.3)
        HeldUpdate(flag);
    } else if (tentacle) {                            // 0x0005ADE1: the sewer tentacle's hit (0x00050360)
        if (tentacle == 1) {
            BeginRootMotion();                        // 0x000368A0
            PlayAnimById(0xD9, true, false, true, 0.0f);   // 0x0003CE70(0xD9, 1, 0, 1, 0)
        } else if (tentacle == 2) {
            BeginRootMotion();
            PlayAnimById(0xDA, true, false, true, 0.0f);
        }
        tentacle = 0;                                 // (the flag is kept)
    } else if (body->shape.flags & 0x80) {            // body vf4C 0x000AEDA0 (IsSinking: shape +0x9C & 0x80)
        if (!IsAnimPlaying(0x52)) {                   // 0x0005AE4D (0x00033E20)
            PlayAnimById(0x52, false, true, false, 0.0f);   // 0x0003CE70(0x52, 0, 1, 0, 0.0)
            DeleteInventoryEffects();                 // 0x00034A50
        }
        flag = 0;                                     // [esp+0x1B] = 0 -> 0x0005B44A
    } else if (fallH > 41.0f && velY < -40.0f) {      // 0x0005AE7C: the fall death ([0x0029A694] / [0x0029764C])
        if (fallH > 100.0f) {                         // FCOMP [0x00295610] on the kept fallH; TEST AH,0x41; JNE
            Damage(std::bit_cast<f32>(0x47590300u), nullptr);   // vf7C(55555.0, 0)
            if (health < 0.0f || health == 0.0f) deathKind = 7;   // TEST AH,0x41; JP; +0x778 = EDI (7)
        }
        if (!IsAnimPlaying(0x66)) PlayAnimById(0x66, true, false, true, 0.0f);   // death_fall: 0x0003CE70(0x66, 1, 0, 1, 0)
        const u32 camMode = level.camera ? level.camera->mode : 0;   // (S+0x8C)+0x15C
        if (camMode == 7 || camMode == 8) LeaveFirstPerson();   // 0x00054DA0(0)
        hasControl = 0;                               // +0x630
        flag = 0;
    } else if (IsDead()) {                            // vfA8 (0x0005AF39)
        DyingStates();
        flag = 0;
    } else if (body->supported && (releaseDizzy || (IsAnimPlaying(0xBE) && !finished))) {   // 0x0005B3D2 (dizzy)
        releaseDizzy = 0;                             // +0x7A5 (Bender released by a holder, 0x00055880)
        PlayAnimation(SeqById(0xBE), true, false, false, 0.0f);   // 0x0003A130(0x00033E00(0xBE), 1, 0, 0, 0)
        flag = 0;
    } else if (flag) {
        AttackStates(flag, hurt, reload);
    }
    // 0x0005B44A: the electric-shock voice (+0xC14) is stopped once 0x53 no longer plays
    if (VoiceLive(shockVoice) && !IsAnimPlaying(0x53)) {
        level.Sound().StopVoice(static_cast<s32>(shockVoice));
        shockVoice = 0;
    }

    // ---- phase B
    // Fry's walk / strafe / walk-back animation by the weapon (0x0005CA4D / 0x0005CBAB / 0x0005CEDC): `base` without a
    // weapon, `b` for the shotgun / machine gun, `c` for the railgun, `d` for the pistol and for the hammer while its
    // attack overlay plays (vf128 twice, then +0x9C), `other` for a type beyond 4
    auto fryWalkAnim = [&](s32 base, s32 b, s32 c, s32 d, s32 other) {
        const Weapon* W = weapon;                     // (+0x528)
        if (!W) return base;
        const u32 t = W->WeaponType();                // vf7C
        if (t == 2 || t == 4) return b;
        if (t == 3) return c;
        if (t == 1) return d;
        if (t == 0) return WeaponOverlayPlaying() && !overlayFinished ? d : base;
        return other;
    };
    // orig: the idle chatter test (0x0005D435, inline again at 0x0005D195 / 0x0005D25D with idle > 25 first): not
    // speaking (the lip-sync player +0x50 == 1), in control (+0x630), +0x634 above 20 ([0x00295AB4]; TEST AH,0x41)
    // and no active cannon (+0x7A8: 0x00032E40), then the definition's IngameEvent category 7 with an empty argument
    // (0x000984D0; (D+0x28)->0x000F28F0(7, &arg), its dtor 0x000F0C00).
    auto chatterAllowed = [&] {
        return lip.state == 1 && hasControl != 0 && timer634 > 20.0f && !(cannon && cannon->IsActive());
    };
    auto chatter = [&] { cdef.ingameEvents.Trigger(7, IngameEventArg{}, IngameContext()); };
    auto locomotion = [&] {
        if (!flag) return;
        if (!(dSpeed <= thrMove)) {                   // TEST AH,0x41; JNE: walk / run (4.2.2)
            if (side > 0.3f) {                        // orig: 0x00293B18 (TEST AH,0x41; JNE)
                if (type == 1) PlayAnimById(fryWalkAnim(0x0F, 0x74, 0x96, 0x89, 0x0F), false, true, false, 0.0f);   // walkright
                else if (type == 2 || type == 3) SumBlend(SeqById(0x15), SeqById(0x17), side);   // run / runright
                else PlayAnimById(0x17, false, true, false, 0.0f);
            } else if (side < -0.3f) {                // orig: 0x00297644 (TEST AH,5; JP)
                if (type == 1) PlayAnimById(fryWalkAnim(0x0E, 0x73, 0x95, 0x88, 0x0E), false, true, false, 0.0f);   // walkleft
                else if (type == 2 || type == 3) SumBlend(SeqById(0x16), SeqById(0x15), F(double(side) + 1.0f));
                else PlayAnimById(0x16, false, true, false, 0.0f);
            } else if (fwd > 0.0f) {                  // TEST AH,0x41; JNE
                const double thr = (double(cdef.walkSpeed) - cdef.sneakSpeed) * 0.3f + cdef.sneakSpeed;
                const f32 w = F((double(dSpeed) < thr ? 0.0 : double(dSpeed)) / cdef.runSpeed);
                if (type == 1) {
                    const bool landL = IsAnimPlaying(0x9D) || IsAnimPlaying(0x7B) || IsAnimPlaying(0x20);
                    const Weapon* W = weapon;         // 0x0005CD86
                    const u32 t = W ? W->WeaponType() : 0;
                    if (W && (t == 2 || t == 4)) SumBlend(SeqById(0x71), SeqById(0x72), w);   // the gun walk / run
                    else if (W && t == 3) SumBlend(SeqById(0x93), SeqById(0x94), w);          // the railgun's
                    else SumBlend(SeqById(0x0D), SeqById(0x15), w);   // walk / run
                    if (landL) NotPorted("Player: the left-foot re-time after a landing (0x000364C0)");
                } else if (type == 2 || type == 3) {
                    SumBlend(SeqById(0x0D), SeqById(0x15), w);
                } else if (type == 4) {
                    PlayAnimById(0x15, false, true, false, 0.0f);
                } else if (dSpeed > thrRun) {         // TEST AH,0x41; JNE
                    PlayAnimById(0x15, false, true, false, 0.0f);
                } else if (!(dSpeed <= thrWalk)) {    // TEST AH,0x41; JP (unordered: walk)
                    PlayAnimById(0x0D, false, true, false, 0.0f);
                } else {
                    PlayAnimById(0x11, false, true, false, 0.0f);   // sneak
                }
            } else if (type == 1) {                   // 0x0005CEC9 (dSpeed > thrMove: always here)
                PlayAnimation(SeqById(fryWalkAnim(0x10, 0x75, 0x97, 0x8A, 0x8A)), false, true, false, 0.0f);
            } else {
                PlayAnimation(SeqById(0x10), false, true, false, 0.0f);   // walkback
            }
            flag = 0;
            return;
        }
        // 4.2.1 the skid check
        NiPoint3 u{velBefore.x, 0, velBefore.z};
        ni::Unitize(u);
        const NiPoint3 l = TransposeMul(rotation, u);
        const double vf = (double(kUnitX.y) * l.y + double(kUnitX.z) * l.z) + double(kUnitX.x) * l.x;
        if (prevRunTime > 1.5f && vf > 0.75f) {       // 0x00293BEC / 0x00293A94, TEST AH,0x41; JNE (both)
            if (!IsAnimPlaying(0x6A) && SeqById(0x6A)) {   // orig: 0x0005D024 / 0x0005D031 (skid)
                BeginRootMotion();                    // 0x000368A0, before the play
                PlayAnimById(0x6A, true, false, true, 0.0f);   // 0x0003A130(seq, once 1, intr 0, force 1, 0.0)
            }
            flag = 0;                                 // byte [esp+0x1B] = 0, then 0x0005D4A5
            return;
        }
        // idle proper
        idleTime = F(double(dt) + idleTime);
        if (!interruptible) return;
        if (IsAnimPlaying(0x1B) || IsAnimPlaying(0x21) || IsAnimPlaying(0x1E)) return;
        static constexpr s32 kLand[] = {0x1D, 0x23, 0x20, 0x78, 0x7E, 0x7B, 0x9A, 0xA0, 0x9D};
        for (s32 k : kLand)
            if (IsAnimPlaying(k) && !finished) return;
        if (IsAnimPlaying(0x1C)) return;
        if (weapon && (weapon->WeaponType() == 1 || weapon->WeaponType() == 2 || weapon->WeaponType() == 4)) {
            // 0x0005D166 / 0x0005D21E: the pistol / shotgun / machine-gun idles; after 25 s the IngameEvent category 7
            // chatter (Character +0x50 == 1, +0x630, +0x634 > 20, +0x7A8: idleTime = 0, 0x000F28F0(7))
            if (idleTime > 25.0f && chatterAllowed()) {   // 0x0005D182 / 0x0005D24A: FCOMP [0x00297240] (25); TEST AH,0x41
                idleTime = 0;                         // +0x4C8 (before the trigger)
                chatter();
            }
            if (weapon->WeaponType() == 1) PlayAnimById(0xE0, false, true, false, 0.0f);   // (0, 1, 0, 0)
            else PlayAnimation(FindSequence("shotgun_idle"), false, true, false, 0.0f);   // 0x0029A5B8
            return;
        }
        if (IsAttackAnim()) {
            ni::NiSequence* s = SeqById(0x62);
            if (s) PlayAnimation(s, false, true, false, 0.0f);
            else PlayAnimById(0, false, true, false, 0.0f);
            return;
        }
        if (WeaponOverlayPlaying()) {                 // vf128 (every type)
            PlayAnimById(0, false, true, false, 0.0f);
            return;
        }
        ni::NiSequence* cidle = SeqById(0x62);
        if (cidle && sinceAttack < 5.0f) {           // TEST AH,5; JP
            PlayAnimation(cidle, false, true, false, 0.0f);
            return;
        }
        if (!IsIdling()) idleTime = 0;
        else if (!finished && curSeq) return;
        const s32 n = static_cast<s32>(static_cast<s64>(double(idleTime)));   // orig: _ftol
        if (idleTime >= 30.0f && n % 30 == 0 && cdef.longIdleCount != 0) {
            const s32 id = crt::Rand() % cdef.longIdleCount + 9;
            if (!IsAnimPlaying(id)) PlayAnimById(id, false, true, true, 0.0f);
            idleTime = 0;                             // 0x0005D42B
            if (chatterAllowed()) chatter();          // 0x0005D435
            return;
        }
        if (idleTime >= 12.0f && (n % 12 == 0 || cdef.longIdleCount == 0)) {
            const s32 id = crt::Rand() % cdef.idleCount;
            if (!IsAnimPlaying(id)) PlayAnimById(id, false, true, true, 0.0f);
            if (type != 1 && chatterAllowed()) chatter();   // +0x774 == 1 skips; else 0x0005D435 (idleTime kept)
            return;
        }
        if (idleTime >= 30.0f) {                      // 0x0005D5CF -> 0x0005D42B
            idleTime = 0;
            if (chatterAllowed()) chatter();
            return;
        }
        if (!IsAnimPlaying(0)) PlayAnimById(0, false, true, true, 0.0f);
    };
    if (flag) {
        if (type == 4) {
            locomotion();
        } else if (ledge) {                           // orig: 0x0005B4A3 (+0xB8D; every path ends with the flag cleared)
            if (!SeqById(0x24)) ledgeB90 = 0;         // (the byte gets AL = 0 of the null result)
            if (ledgeB8F) {                           // climbing up
                if (!IsAnimPlaying(0x29)) {           // (playing: 0x0005C973)
                    BeginRootMotion();                // 0x000368A0, before the play
                    PlayAnimById(0x29, true, false, false, 0.0f);
                    const NiPoint3 p = GetWorldPosition();   // vf34
                    SoundAt(0x46, &p, 3, 0, nullptr); // 0x0003FEA0 on P+0x10
                }
            } else if (ledgeB90) {                    // grabbing
                if (!IsAnimPlaying(0x24)) {           // ledgegrab
                    PlayAnimById(0x24, true, false, true, 0.0f);
                    const NiPoint3 p = GetWorldPosition();
                    SoundAt(0x45, &p, 3, 0, nullptr);
                }
            } else {                                  // hanging: 0x25, else the grab's 0x24
                PlayAnimById(SeqById(0x25) ? 0x25 : 0x24, false, true, false, 0.0f);
            }
            flag = 0;
        } else if (ladder) {                          // orig: 0x0005B5AE (+0xB8C)
            if (ladderB91) {                          // off at the top
                if (!IsAnimPlaying(0x2E)) {
                    StopAnimation();                  // 0x00039D90
                    PlayAnimById(0x2E, false, true, false, 0.0f);
                    BeginRootMotion();                // 0x000368A0, after the play
                }
            } else if (ladderB92) {                   // on from above
                if (!IsAnimPlaying(0xF7)) {           // onto_ladder
                    StopAnimation();
                    PlayAnimById(0xF7, true, false, true, 0.0f);
                }
            } else if (b93) {                         // up
                SetFrequency(SeqById(0x2C), 1.0f);    // 0x00036240 (cdecl)
                PlayAnimById(0x2C, false, true, false, 0.0f);
            } else if (b94) {                         // down: 0x2D, else 0x2C backwards
                if (SeqById(0x2D)) {
                    PlayAnimById(0x2D, false, true, false, 0.0f);
                } else {
                    SetFrequency(SeqById(0x2C), -1.0f);
                    PlayAnimById(0x2C, false, true, false, 0.0f);
                }
            } else {                                  // holding still
                if (!IsAnimPlaying(0x2C) && !IsAnimPlaying(0x2D)) PlayAnimById(0x2C, false, true, false, 0.0f);
                HoldAnimation(nullptr, 0.0f);         // 0x000364C0(0, 0)
            }
            flag = 0;
        } else if (shock || IsAnimPlaying(0x53)) {   // 0x0005C97D (object_gaps.md 18.3)
            shock = 0;                                // +0xB9E
            if (!IsAnimPlaying(0x53)) {               // electric_shock
                FaceKeyDialogue(2);                   // 0x000574F0(2): "electric.key"
                PlayAnimById(0x53, true, false, false, 0.0f);   // 0x0003CE70(0x53, 1, 0, 0, 0)
                flag = 0;
                const NiPoint3 p = GetWorldPosition();   // vf34
                rumble.Add(std::bit_cast<f32>(0x3F666666u), std::bit_cast<f32>(0x3F666666u),
                           std::bit_cast<f32>(0x3E4CCCCDu), 1);   // (+0xA04)->0x0005F000(0.9, 0.9, 0.2, 1)
                if (!VoiceLive(shockVoice))           // 0x0005C9D8
                    shockVoice = static_cast<u32>(SoundAt(0x4D, &p, 4, 0, nullptr));   // 0x0003FEA0 (index 0)
            } else if (finished) {
                locomotion();                         // 0x0005CA0A
            } else {
                flag = 0;
            }
        } else if (roll95 || roll96 || roll97 || roll98) {
            NotPorted("Player: rolls");
            flag = 0;
        } else if (IsAnimPlaying(0x5E) || IsAnimPlaying(0x5F) || IsAnimPlaying(0x60) || IsAnimPlaying(0x61)) {
            if (finished) locomotion();               // WAIT_END
            else flag = 0;
        } else if (hurt && SeqById(0x47)) {
            if (!IsAnimPlaying(0x47)) PlayAnimById(0x47, true, false, false, 0.0f);
            flag = 0;
        } else {
            // 4.2.6 a landing in progress
            const bool landTimeOk = !(dSpeed < thrMove) && !(double(animStart) + 0.05f > double(animTime));
            auto any = [&](std::initializer_list<s32> ids) {
                for (s32 k : ids)
                    if (IsAnimPlaying(k)) return true;
                return false;
            };
            const bool speedUp = (type == 1 && any({0x1D, 0x78, 0x65, 0x7F, 0x9A, 0xA1}) && !landTimeOk) ||
                                 any({0x23, 0x20, 0x7E, 0x7B, 0xA0, 0x9D}) ||
                                 ((type == 3 || type == 2) && IsAnimPlaying(0x1D) && !landTimeOk);
            const bool onGround = body->supported || (body->shape.flags & 0x40);
            const NiPoint3 pos = GetWorldPosition();
            if (speedUp) {                            // SPEEDUP 0x0005C8DF
                const AnimEntry* e = curSeq ? EntryOf(curSeq) : nullptr;
                f32 f = e ? e->speed : 1.0f;
                if (jumpRequest || jumpPending) {
                    jumpPending = 1;
                    f = F(double(f) * 3.0f);
                }
                SetFrequency(curSeq, f);
                if (finished) locomotion();           // WAIT_END
                else flag = 0;
            } else if (jumpRequest || jumpPending) {  // 4.2.4 the jump start (0x0005C6D8)
                jumpPending = 0;
                startY = pos.y;
                if (type == 1) {
                    if (IsJumpAnim()) {
                        flag = 0;
                    } else {
                        const Weapon* W = weapon;     // 0x0005C716
                        BeginRootMotion();
                        const bool slow = !(hSpeed >= thrWalk);   // TEST AH,1; JNE (< or NaN)
                        const u32 t = W ? W->WeaponType() : 0;
                        if (!W || t == 0 || t == 1) PlayAnimById(slow ? 0x1B : 0x21, true, false, true, 0.0f);
                        else if (t == 2 || t == 4) PlayAnimById(slow ? 0x76 : 0x7C, true, false, true, 0.0f);
                        else if (t == 3) PlayAnimById(slow ? 0x98 : 0x9E, true, false, true, 0.0f);
                        SoundAt(0x33, &pos, 3);
                    }
                } else {
                    const bool run = hSpeed > thrRun && fwd > 0.5f && SeqById(0x21);
                    if (IsAnimPlaying(0x21) || IsAnimPlaying(0x1B)) {
                        flag = 0;
                    } else {
                        BeginRootMotion();
                        PlayAnimById(run ? 0x21 : 0x1B, true, false, true, 0.0f);
                        SoundAt(0x33, &pos, 3);
                    }
                }
                flag = 0;
            } else if (onGround && (IsAnimPlaying(0x66) || velBefore.y < -40.0f || IsFallAnim())) {
                Landing(flag, fallH, hSpeed, thrWalk, fwd, side, pos);   // 4.2.6
            } else if (IsJumpAnim()) {                // 4.2.5 jump -> fall
                if (finished) {
                    const Weapon* W = weapon;         // 0x0005BC0D
                    const u32 t = W ? W->WeaponType() : 0;
                    if (!(hSpeed >= thrWalk)) {       // TEST AH,1; JNE (< or NaN)
                        if (!W || t == 0 || t == 1) PlayAnimById(0x1C, false, true, false, 0.0f);   // fall
                        else if (t == 2 || t == 4) PlayAnimById(0x77, true, false, true, 0.0f);
                        else if (t == 3) PlayAnimById(0x99, true, false, true, 0.0f);
                    } else {
                        if (!W || t == 0 || t == 1) PlayAnimById(0x22, true, false, true, 0.0f);   // runfall
                        else if (t == 2 || t == 4) PlayAnimById(0x7D, true, false, true, 0.0f);
                        else if (t == 3) PlayAnimById(0x9F, true, false, true, 0.0f);
                    }
                    if (velBefore.y > -20.0f) velBefore.y = -30.0f;   // orig: 0x0029A4E8 / 0xC1F00000
                    body->velocity.y = velBefore.y;
                }
                flag = 0;
            } else if (IsFallAnim()) {
                flag = 0;
            } else if (!(velY < -50.0f)) {            // TEST AH,5; JP
                locomotion();
            } else {                                  // 4.2.5 start falling (the weapon at 0x0005BD90)
                const Weapon* W = weapon;
                const u32 t = W ? W->WeaponType() : 0;
                if (hSpeed >= thrWalk && SeqById(0x22)) {   // TEST AH,1; JNE (< or NaN: the standing fall)
                    if (W && (t == 2 || t == 4)) PlayAnimById(0x7D, true, false, true, 0.0f);
                    else if (W && t == 3) PlayAnimById(0x9F, true, false, true, 0.0f);
                    else PlayAnimById(0x22, true, false, true, 0.0f);
                } else if (SeqById(0x1C)) {
                    if (W && (t == 2 || t == 4)) PlayAnimById(0x77, true, false, true, 0.0f);
                    else if (W && t == 3) PlayAnimById(0x99, true, false, true, 0.0f);
                    else PlayAnimById(0x1C, false, true, false, 0.0f);
                }
                flag = 0;
            }
        }
    }

    // ---- tail (4.2.8)
    if (!animUpdated) {
        idleTime = 0;
    } else {
        const s32 n = static_cast<s32>(static_cast<s64>(double(idleTime)));
        if (idleTime >= 5.0f && n % 5 == 0) {
            const s32 cnt = (cdef.keyFiles.count(0) ? 1 : 0) + (cdef.keyFiles.count(1) ? 1 : 0);
            if (cnt) FaceKeyDialogue(static_cast<u32>(crt::Rand() % cnt));   // 0x000574F0(rand() % cnt)
        }
    }
    AnimTick(dt);
    if (body->supported || (body->shape.flags & 0x80) || (body->shape.flags & 0x40) || ledge || ladder) spawnY = posY;
}

// orig: 0x000574F0 (thiscall(u32 mode), RET 4): the definition's key file of `mode` (D+0x134, 0x00108220) as a
// dialogue (a lip track on +0x40, priority 4) for the dialogue display 0x000D7BE0, unless the player speaks.
void Player::FaceKeyDialogue(u32 mode) {
    const auto it = cdef.keyFiles.find(mode);         // 0x00108220 (the lookup fails: nothing)
    if (it == cdef.keyFiles.end() || !it->second) return;
    if (lip.state == 0) return;                       // +0x50: playing
    DialogueBuilder b(level, level.text, nullptr);    // 0x000D7FF0
    b.NewDialogue();                                  // 0x000D8200
    b.NewSegment(0.0f);                               // 0x000D8270
    (void)b.AddLipKeyFile(&lip, it->second.Get());    // (looked up again) 0x000D8600(this + 0x40, key file)
    b.CloseSegment(-1.0f);                            // 0x000D8A00
    if (ni::NiPointer<Dialogue> d = b.Build()) {      // 0x000D8AB0
        d->priority = 4;                              // +0x18
        (void)level.dialogue->Play(d.Get());          // DAT_002EBDAC->0x000D7BE0
    }
}

void Player::Landing(u8& flag, double fallH, f32 hSpeed, f32 thrWalk, f32 fwd, f32 side, const NiPoint3& pos) {
    // orig: LANDING 0x0005BEDF (4.2.6; the weapon variants 0x0005C291 / 0x0005C3AC / 0x0005C554)
    b9A = 0;
    if (fallH > 40.0f || IsAnimPlaying(0x66)) {       // [0x0029723C] (TEST AH,0x41; JE) or death_fall playing
        // orig: 0x0005C61F, the deadly landing
        Damage(std::bit_cast<f32>(0x47590300u), nullptr);   // vf7C(55555.0, 0)
        if (health < 0.0f || health == 0.0f) deathKind = 7;   // [0x00293AB8]; TEST AH,0x41; JP; +0x778
        const NiPoint3 at = GetWorldPosition();       // vf34 -> esp+0x4C (the sound position)
        AiNoise(level, GetWorldPosition(), 1.0f);     // (S+0x114)->0x00031420(vf34(), 1.0f) at 0x0005C679
        if (level.camera) level.camera->SetShake(0.5f, 0.4f);   // 0x00011290(0x3F000000, 0x3ECCCCCD)
        rumble.Add(0.9f, 0.9f, 0.2f, 1);              // 0x0005F000(+0xA04; 0x3F666666, 0x3F666666, 0x3E4CCCCD, 1)
        SoundAt(0x4E, &at, 3);                        // 0x0003FEA0(0x4E, &pos, 3, 0, 0)
        flag = 0;
        return;
    }
    if (type == 3 && (IsAnimPlaying(0xD2) || IsAnimPlaying(0xD1))) {
        // orig: 0x0005BF5E..0x0005C26F, Bender's charge-punch impact (the ground pound)
        const NiPoint3 g = GetWorldPosition();        // vf34
        const NiPoint3 at{g.x, F(double(g.y) + 0.5f), g.z};   // FADD [0x00293AA0] -> esp+0x40
        NiPoint3 dir = ni::MulVector(rotation, kUnitX);   // vf38; 0x00011770 (0x002D77E0) -> esp+0x2C
        const u8 surf = body->shape.contactFlags[0];  // MOVZX byte (B+0x40)+0x54 (the raw byte: no S+0x160 table)
        if (Def* sd = level.defs ? level.defs->Find("effect_smoke") : nullptr) {   // 0x002976C4
            if (surf >= 5 && surf <= 10 && surf != 6 && surf != 7) {   // jump table 0x0005D6D0 (surf - 5, 0..5)
                auto* e = dynamic_cast<SpriteEmitterDef*>(sd);
                if (!e) {
                    NotPorted("Player: Bender's charge-punch smoke with a non-sprite effect_smoke");
                } else {
                    switch (surf) {                   // vfC4 0x0006D140 (configured and current colour)
                    case 5: e->SetColor(0.3f, 0.12f, 0.9f); break;   // 0x0005C00F
                    case 8: e->SetColor(0.3f, 0.12f, 0.12f); break;  // 0x0005C02A
                    case 9: e->SetColor(0.5f, 0.4f, 0.12f); break;   // 0x0005BFFE
                    default: e->SetColor(0.65f, 0.52f, 0.266f); break;   // 10: 0x0005BFE3
                    }
                    e->current.count = 20;            // vf88(0x14)
                    e->current.range38 = {2.0f, 2.0f};   // vf7C(2.0) -> vf78
                    e->current.range40 = {0.0f, 0.0f};   // vf84(0) -> vf80
                    e->current.gravity = 0.5f;        // vf90(0.5)
                    e->spriteCurrent.size = {2.0f, 4.0f};   // vfD0(2.0, 4.0)
                    e->current.range28 = {1.0f, 5.0f};   // vf68(1.0, 5.0)
                    e->current.range30 = {0.5f, 0.7f};   // vf70(0.5, 0.7)
                    (void)e->SpawnEffect(level, at, dir);   // vf20(&at, &dir)
                }
            }
        }
        SoundAt(0x43, &at, 3);                        // 0x0003FEA0(0x43, &at, 3, 0, 0)
        dir = kUnitY;                                 // 0x002D77EC copied into esp+0x2C
        if (level.defs && level.defs->Find("Punch_Particles"))   // 0x0029A5EC (looked up twice)
            (void)level.defs->Find("Punch_Particles")->SpawnEffect(level, at, dir);   // vf20(&at, &UNIT_Y)
        if (level.defs && level.defs->Find("Bender_Shockwave")) {   // 0x0029A5D8 (looked up twice)
            const ni::NiMatrix3 I{};                  // 0x00102FF0 (identity; the pad words zeroed)
            // vf14(S+0x5C ? S+0x5C + 4 : 0, &at, &I, "bendwave" 0x0029A5CC)
            if (GameObject* o = level.defs->Find("Bender_Shockwave")->Spawn(level, level.currentStage, at, I, "bendwave"))
                SetNodeScale(*o, 2.0f);               // 0x00073720(2.0)
        }
        PlayAnimById(0xD3, true, false, true, 0.0f);  // 0x0003CE70 (pushes 0, 1, 0, 1, 0xD3): charge_punch_land
        HitSpheres(1);                                // vfD8(1)
        if (fallH > 21.0f) Damage(25.0f, nullptr);    // FLD +0xB84; FSUB; FCOMP [0x0029A5C8]; TEST AH,0x41; JNE
        AiNoise(level, GetWorldPosition(), 1.0f);     // (S+0x114)->0x00031420(vf34(), 1.0f) at 0x0005C215
        if (level.camera) level.camera->SetShake(0.5f, 0.4f);   // 0x00011290(0x3F000000, 0x3ECCCCCD)
        SoundAt(0x32, &at, 3);                        // 0x0003FEA0(0x32, &at, 3, 0, 0)
        rumble.Add(0.5f, 0.5f, 0.3f, 0);              // 0x0005F000(+0xA04; 0.5, 0.5, 0.3, 0)
        flag = 0;
        return;
    }
    if (fallH > 21.0f) {                              // orig: 0x0029A5C8 (the high landing 0x0005C28F)
        const Weapon* W = weapon;                     // 0x0005C291 (read before the damage)
        Damage(25.0f, nullptr);                       // vf7C(25.0, 0)
        rumble.Add(0.5f, 0.5f, 0.3f, 0);              // 0x0005C2BA (0x3F000000, 0x3F000000, 0x3E99999A, 0)
        if (SeqById(0x65)) {
            const u32 t = W ? W->WeaponType() : 0;
            if (W && (t == 2 || t == 4)) PlayAnimById(0x7F, true, false, true, 0.0f);   // the gun's high_land
            else if (W && t == 3) PlayAnimById(0xA1, true, false, true, 0.0f);
            else PlayAnimById(0x65, true, false, true, 0.0f);
        } else {
            PlayAnimById(0x1D, true, false, true, 0.0f);
        }
        AiNoise(level, GetWorldPosition(), 1.0f);     // 0x00031420(vf34(), 1.0f) at 0x0005C36C
        if (level.camera) level.camera->SetShake(0.5f, 0.4f);   // 0x0005C38F: 0x00011290(0x3F000000, 0x3ECCCCCD)
        SoundAt(0x32, &pos, 3);                       // 0x0005C5F4
        flag = 0;
        return;
    }
    const Weapon* W = weapon;                         // 0x0005C3AC / 0x0005C554
    const u32 t = W ? W->WeaponType() : 0;
    if (!(hSpeed >= thrWalk)) {                       // standing landing 0x0005C51E (TEST AH,1; JNE: < or NaN)
        if (IsAnimPlaying(0x1D) || IsAnimPlaying(0x78) || IsAnimPlaying(0x9A)) {
            flag = 0;
            return;
        }
        if (W && (t == 2 || t == 4)) PlayAnimById(0x78, true, false, true, 0.0f);
        else if (W && t == 3) PlayAnimById(0x9A, true, false, true, 0.0f);
        else PlayAnimById(0x1D, true, false, true, 0.0f);
    } else if (W && (t == 2 || t == 4)) {             // the moving landing with a gun
        const bool l = IsAnimPlaying(0x9C) || IsAnimPlaying(0x7A) || IsAnimPlaying(0x1F);
        PlayAnimById(l ? 0x7B : 0x7E, true, false, true, 0.0f);
    } else if (W && t == 3) {
        const bool l = IsAnimPlaying(0x9C) || IsAnimPlaying(0x7A) || IsAnimPlaying(0x1F);
        PlayAnimById(l ? 0x9D : 0xA0, true, false, true, 0.0f);
    } else if (fwd > 0.5f) {                          // moving landing 0x0005C492
        PlayAnimById(IsAnimPlaying(0x1F) ? 0x20 : 0x23, true, false, true, 0.0f);
    } else if (side > 0.5f || side < -0.5f) {
        PlayAnimById(0x1D, true, false, true, 0.0f);
        b9A = 1;
    } else {
        PlayAnimById(0x1D, true, false, true, 0.0f);
    }
    AiNoise(level, GetWorldPosition(), 1.0f);         // 0x00031420(vf34(), 1.0f) at 0x0005C5EF
    SoundAt(0x32, &pos, 3);
    flag = 0;
}

// orig: 0x0005AF39..0x0005B3D0 (docs/analysis/explore_leftovers_port.md 1.3): the caller clears the flag after it.
void Player::DyingStates() {
    if ((body->shape.flags & 0x80) && SeqById(0x52)) {   // body vf4C (sinking); 0x00033E00(0x52)
        if (!IsAnimPlaying(0x52)) PlayAnimById(0x52, false, true, false, 0.0f);   // 0x0003CE70(0x52, 0, 1, 0, 0.0)
        return;                                       // (playing: 0x0005B182)
    }
    for (s32 k = 0x4D; k <= 0x51; ++k)               // dying1..3, 0x50, 0x51 playing: 0x0005B182
        if (IsAnimPlaying(k)) return;
    if (type == 3 && model && (model->flags & ni::kNiAVHidden)) return;   // +0x564 +0xB0 & 1 (no null test)
    auto dying1 = [&] { PlayAnimById(0x4D, true, false, true, 0.0f); };   // 0x0003CE70(0x4D, 1, 0, 1, 0.0)
    auto findWaypoint = [&](const char* name) -> const StageWaypoint* {   // Session::FindWaypointAnyStage 0x000C2A40
        for (const auto& st : level.stages)
            if (const StageWaypoint* wp = st->FindWaypoint(name)) return wp;
        return nullptr;
    };
    auto grind = [&](const char* wpName, const char* defName) {   // 0x0005B1C6 / 0x0005B275
        if (const StageWaypoint* wp = findWaypoint(wpName)) {
            const NiPoint3 p = wp->position;          // esp+0x40
            SetRotation(ni::MakeYRotation(wp->yaw));  // 0x001030B0 (pad lanes zeroed), vf24
            SetPosition(p);                           // vf1C
        }
        dying1();
        if (model) model->flags |= ni::kNiAVHidden;   // +0x564 +0xB0 |= 1
        const NiPoint3 at = GetWorldPosition();       // vf34
        SoundAt(0x4C, &at, 3);                        // 0x0003FEA0(0x4C, &pos, 3, 0, 0), before the effect
        BenderDeathEffect(defName, false);            // -> 0x0005B119 (FindDef was called before the sound)
    };
    switch (deathKind) {                              // +0x778 - 7, jump table 0x0005D694 (0..0xE; else 0x0005B3BA)
    case 7:
    case 8:                                           // 0x0005B0F1
        if (type == 3) {
            BenderDeathEffect("Bender_Explode", true);   // 0x0029A650 (no animation)
        } else if (ni::NiSequence* s = SeqById(0x4D)) {   // 0x0005B18C (no null test in the original)
            PlayAnimation(s, true, false, true, 0.0f);   // 0x0003A130(s, 1, 0, 1, 0.0)
            s->offset = F(SequenceDuration(s) - double(std::bit_cast<f32>(0x38D1B717u)));   // FSUB [0x00293B1C]
        }
        return;
    case 0xB:                                         // 0x0005B08B
        LeaveFirstPerson();
        dying1();
        {
            const NiPoint3 at = GetWorldPosition();   // vf34 -> esp+0x4C
            SoundAt(0x66, &at, 3);
        }
        return;
    case 0xC:                                         // 0x0005B018
        LeaveFirstPerson();
        if (const StageWaypoint* wp = findWaypoint("benderCrush")) {   // 0x0029A688
            const NiPoint3 w = GetWorldPosition();    // vf34
            SetPosition({wp->position.x, wp->position.y, w.z});   // vf1C
        }
        dying1();
        BenderDeathEffect("Bender_Crush_Side", true);   // 0x0029A674
        return;
    case 0xD:                                         // 0x0005B0D0
        LeaveFirstPerson();
        dying1();
        BenderDeathEffect("Bender_Crush_Top", true);  // 0x0029A660
        return;
    case 0xE: grind("benderGrind1", "Bender_Grinded"); return;          // 0x0029A640 / 0x0029A630
    case 0xF: grind("benderGrind2", "Bender_Second_Grinded"); return;   // 0x0029A620 / 0x0029A608
    case 0x10:                                        // 0x0005B358
        dying1();
        BenderDeathEffect("Bender_Explode", true);
        return;
    case 0x15:                                        // 0x0005B36E
        if (const StageWaypoint* wp = findWaypoint("leelaCrush")) {   // 0x0029A5FC
            const NiPoint3 w = GetWorldPosition();    // vf34
            SetPosition({w.x, w.y, wp->position.z});  // vf1C
        }
        dying1();
        return;
    default:                                          // 9, 0xA, 0x11..0x14 and the rest: 0x0005B3BA
        dying1();
        return;
    }
}

void Player::BenderDeathEffect(const char* defName, bool hide) {   // orig: 0x0005B0FF..0x0005B180
    if (hide && model) model->flags |= ni::kNiAVHidden;   // +0x564 +0xB0 |= 1 (no null test in the original)
    Def* d = level.defs ? level.defs->Find(defName) : nullptr;   // Session::FindDef 0x000C3F10
    if (!d) return;
    const NiPoint3 pos = GetWorldPosition();          // vf34 -> esp+0x94
    const NiPoint3 fwd = ni::MulVector(rotation, kUnitX);   // vf38, 0x00011770 with 0x002D77E0
    auto* idef = dynamic_cast<ImpactDef*>(d);         // (the original calls 0x0004EE90 on any definition)
    if (!idef) {
        NotPorted("Player: a Bender death effect that is not an ImpactDef");
        return;
    }
    if (Impact* o = idef->SpawnUp(level, &inventory, pos, fwd, nullptr)) {   // 0x0004EE90(P+0x38, &pos, &fwd, 0)
        o->hold = 1;                                  // +0x54
        o->SetLit(level.envTextureEffect.Get());      // 0x0004F430(S+0xE4)
    }
}

void Player::LeaveFirstPerson() {                     // orig: 0x00054DA0(0) (0x00054E03..0x00054F7F)
    Camera* cam = level.camera.get();                 // S+0x8C (no null test in the original)
    if (cam) {
        const ni::NiMatrix3 C = cam->rotation;        // +0x70 copied (chunk 0 = column 0)
        NiPoint3 v{C.m[0][0], 0.0f, C.m[2][0]};       // esp+0x10
        (void)ni::Unitize(v);                         // 0x000116A0
        const NiPoint3 u = kUnitY;                    // esp+0x1C (dword copies of 0x002D77EC)
        NiPoint3 w{F(double(u.y) * v.z - double(u.z) * v.y), F(double(u.z) * v.x - double(v.z) * u.x),
                   F(double(u.x) * v.y - double(u.y) * v.x)};   // u x v (esp+0x34, copied to esp+0x28)
        (void)ni::Unitize(w);
        ni::NiMatrix3 R;                              // esp+0x40: columns v, u, -w (FCHS; FSTP)
        R.m[0][0] = v.x, R.m[1][0] = v.y, R.m[2][0] = v.z;
        R.m[0][1] = u.x, R.m[1][1] = u.y, R.m[2][1] = u.z;
        R.m[0][2] = -w.x, R.m[1][2] = -w.y, R.m[2][2] = -w.z;
        SetRotation(R);                               // vf24
        cam->SetFollowTarget(this);                   // 0x00018FB0
        cam->SetFov(90.0f);                           // 0x000110E0(0x42B40000)
    }
    if (view) view->SetActive(0);                     // +0x7B4: 0x00045ED0(0)
    if (type != 1 && level.hud) level.hud->SetActiveAll(true);   // 0x00054F57: 0x000E5A60(1) on S+0x118
    firstPerson = 0;                                  // +0xB88
}

void Player::Reset() {                                // orig: 0x00057680 (docs/analysis/explore_leftovers_port.md 1.6)
    StopAnimation();                                  // 0x00039D90
    ClearOverlay();                                   // 0x000363C0
    node->flags &= ~ni::kNiAVHidden;                  // +0x2C +0xB0 &= 0xFE
    body->velocity = {};                              // B+0x28 = 0x002F915C (zero)
    velBefore = body->velocity;                       // +0x4E4
    health = maxHealth;                               // +0x4F4 = +0x4FC
    runTime = prevRunTime = 0;                        // +0xBD8 / +0xBDC
    stowedWeapon = nullptr;                           // +0xC1C
    if (ride) ride->introTime = 0;                    // +0x7AC: (+0x7AC)+0x44 = 0
    Release();                                        // 0x00055880 (the held state, object_gaps.md 16.3)
    (void)SetWeapon(nullptr);                         // vfB4(0)
    lastSurfaceDamage = sinceAttack = -10.0f;         // +0xB58 / +0xB54 = 0xC1200000
    b9D = b8B = jumpRequest = jumpPending = 0;        // +0xB9D, +0xB8B, +0xB89, +0xB8A
    roll95 = roll96 = roll97 = roll98 = 0;            // +0xB95..+0xB98
    ledgeGate = ladderGate = 1.0f;                    // +0xBE8 / +0xBE4
    pendingWeapon = nullptr;                          // +0x524
    heldBy = nullptr;                                 // +0x7A0
    heldHurt = releaseDizzy = grabStart = grabTurd = 0;   // +0x7A4..+0x7A7
    tentacle = lockOn = lockHasTarget = lockB02 = b9A = 0;   // +0xB9F, +0xB00, +0xB01, +0xB02, +0xB9A
    attacking504 = attacking505 = 0;                  // +0x504 / +0x505
    // (+0xC58 = 0, +0xC54 / +0xC55 = 0: no member in the port)
    xValue = aValue = 0;                              // +0xB34 / +0xB30
    ladderB92 = ladderB91 = ledgeB90 = ledgeB8F = ledge = ladder = firstPerson = locked = 0;   // +0xB92..+0xB8C, +0xB88, +0xB8E
    b94 = b93 = aHeld = xHeld = shock = 0;            // +0xB94, +0xB93, +0xB9B, +0xB9C, +0xB9E
    idleTime = desiredSpeed = 0;                      // +0x4C8 / +0x4F0
    slopeTime = airTime = 0;                          // +0xB78 / +0xB7C
    damageSide = 0;                                   // +0xBE0
    tapTimer = 0;                                     // +0xC30
    tapCount = 0;                                     // +0xC34
    thinkVel = {};                                    // +0xB48
    b44 = 0;                                          // +0xB44 (the rolls and the jump bytes are cleared a second time)
    attack1 = attack2 = 0;                            // +0x506 / +0x507
    special50A = special50B = special50C = 0;         // +0x50A..+0x50C
    lockDir = {};                                     // +0xC40 = NiPoint3::ZERO (0x002F915C, dword copies)
    body->shape.ResetContacts();                     // (B+0x40)->vf04 0x000A9190
    ShowWeaponBox(level, nullptr, ammo);              // 0x000578E8: ("weapons", "select", ""), ("weapons", "ammo", -1)
}

void Player::Respawn(const NiPoint3& pos, const ni::NiMatrix3& rot) {   // orig: 0x00057930
    SetPosition(pos);                                 // vf1C
    SetRotation(rot);                                 // vf24
    Reset();                                          // 0x00057680
    spawnY = pos.y;                                   // +0xB84 = pos +4
}

// orig: 0x00052C20 (docs/analysis/rodent_port.md 4.2): the flag is cleared on every path, so phase B never runs.
void Player::RodentStates(u8& flag, f32 dSpeed, f32 velY, f32 side) {
    const NiPoint3 pos = GetWorldPosition();          // vf34 (the sound position)
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0x4D) && !IsAnimPlaying(0x4E) && !IsAnimPlaying(0x4F)) {
            switch (deathKind) {                      // +0x778
            case 0x11:
            case 0x14: PlayAnimById(0x4F, true, false, true, 0.0f); break;   // dying3
            case 0x12:
                PlayAnimById(0x4D, true, false, true, 0.0f);                  // dying1
                SoundAt(0x62, &pos, 3);
                break;
            default: PlayAnimById(0x4D, true, false, true, 0.0f); break;
            }
        }
        flag = 0;
        return;
    }
    if (IsFallAnim() && velY < -300.0f) {             // vf140; 0x0029A4EC, TEST AH,5; JP
        u32 b = 0x47590300u;                          // 55555.0f
        f32 amount;
        std::memcpy(&amount, &b, 4);
        Damage(amount, nullptr);                      // vf7C(55555.0, 0) (0x00052D23)
        if (health < 0.0f || health == 0.0f) deathKind = 7;   // TEST AH,0x41; JP
        flag = 0;
        return;
    }
    if (jumpRequest) {                                // +0xB89 (set by the ride)
        PlayAnimById(0x21, true, false, true, 0.0f);  // jumprun
        startY = GetWorldPosition().y;                // +0xB80
        SoundAt(0x33, &pos, 3);
        flag = 0;
        return;
    }
    if (body->supported &&                            // vf48; 0x0029764C / 0x0029A4E8
        (velBefore.y < -40.0f || IsFallAnim() || (velBefore.y < -20.0f && IsJumpAnim()))) {
        // (the rumble 0x0005F000(+0xA04; 0.5, 0.5, 0.2, 0): pad only, NOT PORTED)
        PlayAnimById(0x23, true, false, true, 0.0f);  // runland
        AiNoise(level, GetWorldPosition(), 1.0f);     // (S+0x114)->0x00031420(vf34(), 1.0f) at 0x00052E57
        SoundAt(0x32, &pos, 3);
        flag = 0;
        return;
    }
    if (IsJumpAnim()) {                               // vf12C
        if (finished) PlayAnimById(0x1C, false, true, false, 0.0f);   // fall
        flag = 0;
        return;
    }
    if (IsFallAnim()) {
        if (velY < -100.0f && velBefore.y > -100.0f) SoundAt(0x69, &pos, 3);   // 0x0029A4E4
        flag = 0;
        return;
    }
    if (velY < -50.0f && !IsJumpAnim()) {             // 0x00297648 (TEST AH,5; JP)
        if (velY < -100.0f && velBefore.y > -100.0f) SoundAt(0x69, &pos, 3);
        PlayAnimById(0x1C, false, true, false, 0.0f);
        flag = 0;
        return;
    }
    if ((IsAnimPlaying(0x1D) || IsAnimPlaying(0x23)) && !finished) {   // land / runland still playing
        flag = 0;
        return;
    }
    if (dSpeed > 0.0f) {                              // TEST AH,0x41; JNE (NaN: idle)
        if (side > 0.1f) {                            // 0x00293AB0
            if (!IsAnimPlaying(0x6E)) PlayAnimation(SeqById(0x6E), false, true, true, 0.0f);   // lean_right
        } else if (side < -0.1f) {                    // 0x00293BF4
            if (!IsAnimPlaying(0x6D)) PlayAnimation(SeqById(0x6D), false, true, true, 0.0f);   // lean_left
        } else {
            PlayAnimById(0x15, false, true, false, 0.0f);   // run
        }
        flag = 0;
        return;
    }
    if (flag) {
        if (attack1) {                                // +0x506 (the ride sets it after 2.5 s of intro: dead)
            if (!IsAnimPlaying(0x6F)) PlayAnimation(SeqById(0x6F), true, false, true, 0.0f);   // rear_up
        } else {
            PlayAnimById(0, false, true, false, 0.0f);   // idle
        }
    }
    flag = 0;
}

void Player::RenderView(f32 realDt) {
    if (!view || !view->fpView) return;               // (+0x7B4 is the FPView; the level1-6 cockpit is +0x7B0)
    if (level.camera && level.camera->mode == 3) {    // (S+0x8C is not null-tested)
        // orig: 0x00045F10: while active (+0x18) the timer +0x1C advances by the render frame time DAT_002E7C28, the
        // node +0x10 is updated at it (NiAVObject::Update(t, 1)), the camera +0x0C clears the depth (0x000FEF20(4):
        // its renderer's vf44(port, 4)) and is Clicked (0x00100CE0, a tail jump) on its scene +0x15C = +0x10.
        // (Then for Bender 0x000B0420 on the view: an empty profiler mark.)
        if (!view->active) return;
        view->time = F(double(realDt) + view->time);  // FLD [0x002E7C28]; FADD +0x1C; FST arg; FSTP +0x1C
        if (view->node) view->node->UpdateDownwardPass(view->time, true);   // (no null tests in the original)
        if (ni::NiCamera* c = view->camera.Get()) {
            if (c->renderer) c->renderer->ClearBuffer(c->port, 4);
            ni::VisitForCulling(*c, view->cameraScene.Get());
        }
        return;
    }
    view->variant = crt::Rand() % 2;                  // orig: 0x00045EF0
    view->dirty = 1;
}

void Player::RenderUpdate() {                         // orig: vf0C 0x00050B00
    if (heldBy && curSeq) return;                     // 0x00050B03: held with a current sequence (returns 1)
    PrepareRender();
    UpdateLockMarker();                               // vf154 0x00058FA0
    if (chicken) chicken->RenderUpdate();             // orig: 0x00042D80 (P+0x7B0)
    // the FPView (P+0x7B4): off (0x00045ED0(0)) unless the camera (S+0x8C, not null-tested) is in mode 3, where
    // 0x000AEDC0(P+0x78) is an empty function
    if (view && !(level.camera && level.camera->mode == 3)) view->SetActive(0);
    RenderShadow(!firstPerson);                      // 0x0003B960(!vf148 0x00050AD0 (+0xB88))
}

// ---- the lock-on marker (vf154 0x00058FA0) ------------------------------------------------------------------------
namespace {

// orig: DAT_002DD860, the weapon the crosshair texture was last chosen for (a process global, only compared).
const Weapon* g_markerWeapon = nullptr;

// orig: 0x001164E0 (new(0xE0) NiTriShape on new(0x30) NiTriShapeData(n, v, 0, 0, uv, triangles, t)): the data's
// bound from the points (NiGeometryData's ctor 0x00143910: 0x0014EA10).
ni::NiPointer<ni::NiTriShape> MarkerShape(std::vector<NiPoint3> v, std::vector<ni::NiPoint2> uv, std::vector<u16> t) {
    ni::NiPointer<ni::NiTriShapeData> data(new ni::NiTriShapeData);
    data->vertexCount = static_cast<u16>(v.size());
    data->vertices = std::move(v);
    data->textureSets = 1;
    data->uvs = std::move(uv);
    data->triangleCount = static_cast<u16>(t.size() / 3);
    data->triangleList = std::move(t);
    data->bound = ni::BoundOfPoints(data->vertices);
    ni::NiPointer<ni::NiTriShape> s(new ni::NiTriShape);
    s->modelData = data.Get();
    return s;
}

}  // namespace

void Player::BuildLockMarker() {                      // orig: 0x00058FE8..0x00059E9A
    // the crosshair: a 3 x 3 vertex grid in the x = 0 plane (new(0x6C) / new(0x48) / new(0x30)), 8 triangles
    ni::NiPointer<ni::NiTriShape> cross = MarkerShape(
        {{0, 1, -1}, {0, 1, 0}, {0, 1, 1}, {0, 0, -1}, {0, 0, 0}, {0, 0, 1}, {0, -1, -1}, {0, -1, 0}, {0, -1, 1}},
        {{0, 0}, {1, 0}, {0, 0}, {0, 1}, {1, 1}, {0, 1}, {0, 0}, {1, 0}, {0, 0}},
        {0, 1, 3, 3, 1, 4, 1, 2, 4, 4, 2, 5, 3, 4, 6, 6, 4, 7, 4, 5, 7, 7, 5, 8});
    // the brackets: 16 vertices (new(0xC0) / new(0x80) / new(0x30)), 8 triangles, "ZoomHair" (0x0029A5AC)
    ni::NiPointer<ni::NiTriShape> hair = MarkerShape(
        {{0, 1, -1}, {0, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, -1}, {0, 0, 0}, {0, 0, 0}, {0, 0, 1},
         {0, 0, -1}, {0, 0, 0}, {0, 0, 0}, {0, 0, 1}, {0, -1, -1}, {0, -1, 0}, {0, -1, 0}, {0, -1, 1}},
        {{0, 0}, {1, 0}, {1, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 1}, {0, 1},
         {0, 1}, {1, 1}, {1, 1}, {0, 1}, {0, 0}, {1, 0}, {1, 0}, {0, 0}},
        {0, 1, 4, 4, 1, 5, 2, 3, 6, 6, 3, 7, 8, 9, 12, 12, 9, 13, 10, 11, 14, 14, 11, 15});
    hair->name = "ZoomHair";                          // NiObjectNET::SetName 0x000FE300
    // every property goes onto both shapes (AddHead 0x00013A50, the crosshair first)
    auto both = [&](ni::NiProperty* p) {
        AttachProperty(*cross, p);
        AttachProperty(*hair, p);
    };
    markerMaterial = new ni::NiMaterialProperty;      // +0xC28 = new(0x5C) 0x00114B10
    markerMaterial->emissive = {1.0f, 0.0f, 0.0f};    // +0x40..+0x48
    markerMaterial->revision += 2;                    // (+0x54 twice)
    markerMaterial->alpha = 0.5f;                     // +0x50 = 0x3F000000
    both(markerMaterial.Get());
    auto* alpha = new ni::NiAlphaProperty;            // new(0x1C), vtbl 0x00293B54: flags 0xEC, +0x18 = 0
    u16 fl = alpha->flags;
    fl |= 1;                                          // byte +0x14 |= 1
    fl |= 0x200;                                      // byte +0x15 |= 2
    fl &= ~0x2000;                                    // byte +0x15 &= 0xDF
    fl &= 0xFFE1;                                     // byte +0x14 &= 0xE1
    fl &= 0xFE1F;                                     // word &= 0xFE1F
    fl &= 0xE3FF;                                     // byte +0x15 &= 0xE3 (0x201: ONE / ONE, the test on)
    alpha->flags = fl;
    alpha->alphaTestRef = 0;
    both(alpha);
    {                                                 // new(0x3C) 0x00109DC0, SetBaseTexture("crosshairG.tga") 0x0001D610,
        ni::NiPointer<ni::NiTexturingProperty> t = MakeTexturing(level.FindTexture("crosshairG.tga"));   // apply 2,
        t->maps[0].clampMode = 0;                     // filter 1 (0x0001D6B0), clamp 0 (0x000506A0(0))
        markerTexturing = t.Get();
        both(t.Get());
    }
    auto* z = new ni::NiZBufferProperty;              // new(0x1C), vtbl 0x00296C80: +0x14 = 3, +0x18 = 3, then
    z->flags = u16(3 & 0xFFFC);                       //  byte +0x14 &= 0xFC: no test, no write
    z->testFunction = 3;
    both(z);
    auto* st = new ni::NiStencilProperty;             // new(0x38) 0x00026B60
    st->enabled = false;                              // byte +0x18 = 0
    st->drawMode = 3;                                 // +0x34 = 3: both faces
    both(st);
    auto* fog = new ni::NiFogProperty;                // new(0x28) 0x00013DC0 (vtbl 0x00293B88): depth 1, colour 0
    fog->flags = u16(fog->flags & 0xE5 & 0xFD);       // (the ctor's & 0xE5, then & 0xFD: fog off)
    both(fog);
    auto* vc = new ni::NiVertexColorProperty;         // new(0x20), vtbl 0x00293AD0 (+0x1C = 1), then +0x1C = +0x18 = 0
    vc->sourceVertexMode = 0;
    vc->lightingMode = 0;
    both(vc);
    cross->modelData->bound.center = {};              // the crosshair's model bound: centre ZERO (0x002F915C),
    cross->modelData->bound.radius = 100000.0f;       //  radius 0x47C35000
    marker = new ni::NiNode;                          // +0xC20 = new(0xE0) 0x00105C10, "Targeting" (0x0029A590)
    marker->name = "Targeting";
    ni::NiPointer<ni::NiNode> crossNode(new ni::NiNode);
    crossNode->name = "CrossHairNode";                // 0x0029A580
    crossNode->AttachChild(cross.Get(), true);        // vfA0(shape, 1)
    marker->AttachChild(crossNode.Get(), true);
    crossNode->flags &= ~ni::kNiAVHidden;             // byte +0xB0 &= 0xFE
    ni::NiPointer<ni::NiNode> zoomNode(new ni::NiNode);
    zoomNode->name = "ZoomTargetNode";                // 0x0029A570
    zoomNode->AttachChild(hair.Get(), true);
    marker->AttachChild(zoomNode.Get(), true);
    zoomNode->flags &= ~ni::kNiAVHidden;
    marker->UpdateProperties();                       // 0x00101EA0
    marker->UpdateEffects();                          // 0x00102190
    UpdateNodeAt(marker.Get(), 0.0f);                 // NiAVObject::Update(0.0, 1)
    if (level.scene) level.scene->AttachChild(marker.Get(), true);   // S+0x18 (0x00013E20) vfA0(+0xC20, 1)
}

void Player::UpdateLockMarker() {                     // orig: vf154 0x00058FA0
    if (type != 1) return;                            // +0x774: Fry only
    if (!marker) BuildLockMarker();                   // +0xC20 == 0
    if (!marker) return;                              // 0x00059EA8
    if (!lockOn && !firstPerson) {                    // +0xB00; vf148 0x00050AD0 (+0xB88)
        marker->flags |= ni::kNiAVHidden;             // 0x0005A74F
        return;
    }
    Weapon* w = weapon;                               // +0x528
    if (w != g_markerWeapon && w) {
        // the crosshair by the weapon type (vf7C): 2 crosshairG, 1 crosshairS, 4 crosshairL, 3 crosshairR, else kept
        // (the original finds the texturing property through "CrossHairNode" child 0, property 8)
        const u32 k = w->WeaponType();
        const char* name = k == 2 ? "crosshairG.tga" : k == 1 ? "crosshairS.tga" : k == 4 ? "crosshairL.tga"
                         : k == 3 ? "crosshairR.tga" : nullptr;   // 0x0029A59C / 560 / 550 / 540
        if (name && markerTexturing) markerTexturing->SetBaseTexture(level.FindTexture(name));   // 0x000C2A80
    }
    g_markerWeapon = w;                               // 0x00059FEA (every frame)
    const NiPoint3 p = CharacterPoint();              // vf78 (esp+0x60)
    NiPoint3 aim = aimPoint;                          // +0xB04 (esp+0x38)
    ni::NiMatrix3 M;                                  // esp+0x70 (3 chunks)
    // esp+0x18: 0 in first person (0x0005A16A); on the other path nothing writes it before the alpha below reads it (a stale stack
    // word in the original: the crosshair's pointer on a frame whose weapon changed, else an earlier call's leftover;
    // L: 0, the first-person value; the tests inject the original's: the MARKERUNINIT lines)
    f32 range = 0.0f;
    if (!firstPerson) {                               // the level aim: the horizontal direction to the aim point
        const NiPoint3 cp = CharacterPoint();         // vf78 (esp+0x54)
        NiPoint3 d{F(double(aimPoint.x) - cp.x), 0.0f, F(double(aimPoint.z) - cp.z)};   // (y computed, then 0)
        (void)ni::Unitize(d);                         // 0x000116A0 (FSTP ST0)
        const NiPoint3 up{0.0f, 1.0f, 0.0f};          // 0x002D77EC
        const NiPoint3 c = ni::Cross(up, d);          // 0x00011700(&c, &d) on up
        for (int r = 0; r < 3; ++r) {                 // the chunks d, up, -c (FCHS each)
            M.m[r][0] = r == 0 ? d.x : r == 1 ? d.y : d.z;
            M.m[r][1] = r == 0 ? up.x : r == 1 ? up.y : up.z;
            M.m[r][2] = -(r == 0 ? c.x : r == 1 ? c.y : c.z);
        }
    } else {                                          // first person: 12 ahead along the camera's x axis
        M = level.camera ? level.camera->rotation : ni::NiMatrix3{};   // (S+0x8C)+0x70 (12 dwords, twice)
        NiPoint3 v = ni::MulVector(M, {1.0f, 0.0f, 0.0f});   // 0x00011770 (0x002D77E0)
        (void)ni::Unitize(v);
        const double kx = double(v.x) * 12.0f, ky = double(v.y) * 12.0f;   // FMUL [0x00297638] (on the stack)
        const f32 kz = F(double(v.z) * 12.0f);        // FSTP [esp+0x5C]
        aim = {F(kx + p.x), F(double(p.y) + ky), F(double(p.z) + kz)};
    }
    if (ni::NiMaterialProperty* m = markerMaterial.Get()) {   // +0xC28: red with a target, else blue; the alpha
        // (0x0005A202: one injected word per reach; first person: the 0 of 0x0005A16A)
        u32 stale = 0;                                // HEAP-MIRROR: a stale word in the original; 0 here (won't do:
                                                      // KNOWN_ISSUES "Heap layout")
        if (!firstPerson) range = std::bit_cast<f32>(stale);
        // FLD; FLD; FCOMP 15.0 [0x00297250]; TEST AH,0x41; JNE: kept unless above 15
        const double s = range > 15.0f ? 15.0 : double(range);
        const double t = s * std::bit_cast<f32>(0x3D888889u);   // FMUL [0x0029A53C] (1 / 15)
        m->emissive.g = 0.0f;                         // +0x44
        if (lockHasTarget) {                          // +0xB01
            m->emissive.r = 1.0f;
            m->emissive.b = 0.0f;
        } else {
            m->emissive.r = 0.0f;
            m->emissive.b = 1.0f;
        }
        ++m->revision;
        m->alpha = F(t + 0.5f);                       // FADD [0x00293AA0]; FSTP +0x50
        ++m->revision;
    }
    ni::NiAVObject* crossNode = GetObjectByName(marker.Get(), "CrossHairNode");   // vf54
    if (crossNode) {                                  // (no test in the original)
        crossNode->local.rotate = M;                  // +0x30 (12 dwords)
        crossNode->local.scale = firstPerson ? F(std::fabs(double(fireHold) * std::bit_cast<f32>(0x3EAAAAABu) + 0.5f))
                                             : 1.0f;  // +0x6C: |+0xD60 / 3 + 0.5| ([0x0029A538]) / |1.0| (0x00293CC0)
        crossNode->local.translate = aim;             // +0x60
    }
    if (weapon && weapon->WeaponType() != 0) marker->flags &= ~ni::kNiAVHidden;   // vf7C
    else marker->flags |= ni::kNiAVHidden;
    lockTime = F(double(g_frameStep) * 3.0f + lockTime);   // FLD [0x002D67E8]; FMUL [0x0029563C]; FADD +0xC2C
    ni::NiAVObject* zoomNode = GetObjectByName(marker.Get(), "ZoomTargetNode");
    ni::NiAVObject* hair = zoomNode ? GetObjectByName(zoomNode, "ZoomHair") : nullptr;
    if (hair) {
        zoomNode->local.translate = aim;
        zoomNode->local.rotate = M;
        // FLD +0xC2C; FCOMP 1.0; TEST AH,1; JE: the brackets have closed (>= 1, not NaN), or no lock: hidden
        if (lockTime >= 1.0f || !lockOn) {
            lockTime = 1.0f;                          // 0x0005A6F8
            zoomNode->flags |= ni::kNiAVHidden;
        } else {
            // the brackets close from (16, 14) (first person (16, 7.5)) to (1, 0) over the lock time: a = 16 - 15 t
            // (rounded), b = 14 - 14 t (7.5 - 7.5 t) on the stack, stored as F(b)
            const f32 a = F(16.0f - double(lockTime) * 15.0f);   // FMUL [0x00297250]; FSUBR [0x00293BE4]; FSTP
            const f32 b = firstPerson ? F(7.5f - double(lockTime) * 7.5f)   // [0x0029A534]
                                      : F(14.0f - double(lockTime) * 14.0f);   // [0x0029A530]
            auto* g = static_cast<ni::NiTriShape*>(hair);   // (vf54's result used as the shape: +0xC8)
            ni::NiGeometryData* data = g->modelData.Get();
            if (!data || data->vertices.size() < 16) {      // (+0x1C null: 0x0005A6E9)
                zoomNode->flags &= ~ni::kNiAVHidden;
            } else {
                // (y, z) per vertex (x = 0): rows a, b, -b, -a by columns -a, -b, b, a
                const f32 ys[4] = {a, b, -b, -a}, zs[4] = {-a, -b, b, a};
                for (int i = 0; i < 16; ++i) data->vertices[size_t(i)] = {0.0f, ys[i / 4], zs[i % 4]};
                data->revisionFlags |= 1;             // +0x2E |= 1
                zoomNode->flags &= ~ni::kNiAVHidden;
            }
        }
    }
    marker->UpdateProperties();                       // 0x00101EA0
    marker->UpdateEffects();                          // 0x00102190
    UpdateNodeAt(marker.Get(), animTime);             // NiAVObject::Update(+0x78, 1)
}

}  // namespace fut::game

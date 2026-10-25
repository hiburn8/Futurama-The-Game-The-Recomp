// The level2-5 cannons; see Cannon.h and docs/analysis/natives_batch_a_port.md 9 (each routine checked against its
// assembly: 0x000334D0, 0x00033980, 0x00032FB0, 0x00032ED0, 0x00032E50).
#include "game/object/Cannon.h"

#include "game/GameTime.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/level/Camera.h"
#include "game/level/Hud.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/object/Character.h"
#include "game/object/Laser.h"
#include "game/object/NodeUpdate.h"
#include "game/object/Pickup.h"
#include "game/input/Pad.h"
#include "game/sound/GameSound.h"

#include <bit>
#include <cstdio>
#include <memory>
#include <string>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

f32 Bits(u32 b) { return std::bit_cast<f32>(b); }
constexpr u32 kHalfPi = 0x3FC90FDBu;                  // [0x002F926C] (the engine's float global)

// orig: the trail emitter of each laser (new(0x114) SpriteEmitterDef(name) 0x0006E090, then the virtual setters in
// this order; the same calls for both lasers but the texture).
SpriteEmitterDef* NewTrail(LevelData& level, const char* name, const char* texture) {
    auto owned = std::make_unique<SpriteEmitterDef>();
    SpriteEmitterDef* e = owned.get();
    e->name = name;
    level.defs->Add(std::move(owned));                // (Def::Def 0x000962E0)
    e->SetSpriteTexture(texture, level);              // vfA4
    e->SetColor(1.0f, 1.0f, Bits(0x3F333333));        // vfC4 (1, 1, 0.7)
    e->SetGravity(0.0f);                              // vf50
    e->SetSize(2.0f, 6.0f);                           // vfB0
    e->SetRangeB4(0.5f, 0.0f);                        // vfB8
    e->SetRange28(0.0f, 3.0f);                        // vf28
    e->SetRange38(0.0f, 1.0f);                        // vf3C (the max only)
    e->SetRange40(0.0f, 0.5f);                        // vf40
    e->SetRange30(Bits(0x3E4CCCCD), Bits(0x3F333333));   // vf30 (0.2, 0.7)
    e->SetFlagA8(1);                                  // vfAC
    e->SetCount(20);                                  // vf48
    e->SetFlag54(1);                                  // vf54
    return e;
}

// orig: 0x000CE4A0 (thiscall stage, RET 4): the first of the stage's rooms (+0x2C, count +0x34; empty slots skipped)
// whose vf54 GetObjectByName finds `name`; a NULL name finds nothing.
ni::NiAVObject* FindInRooms(const Stage& st, const char* name) {
    if (!name) return nullptr;
    for (const auto& r : st.rooms)
        if (r)
            if (ni::NiAVObject* o = GetObjectByName(r.Get(), name)) return o;
    return nullptr;
}

}  // namespace

CannonCtrl::CannonCtrl(LevelData& lvl, Player& p) : level(lvl), player(p) {   // orig: 0x000334D0
    // (+0 / +4 / +8 = 0, the matrices' pad lanes zeroed, +0x7C..+0x88 = 0, +0xAC = 0)
    if (lvl.defs) {
        // orig: new(0xA8) LaserDef("Cannon_Laser") 0x00079BF0 (its spark emitter "Something" follows it in the list)
        redLaser = NewLaser<LaserDef>(lvl, "Cannon_Laser");
        redLaser->value68[0] = 1.0f;                  // +0x68
        redLaser->value68[1] = 0.25f;                 // +0x6C
        redLaser->value68[2] = 0.25f;                 // +0x70
        redLaser->value80 = 25.0f;                    // +0x80
        redLaser->flag50 = 1;                         // +0x50
        redLaser->emitter34 = NewTrail(lvl, "Cannon_Laser_Trail", "Cannon_Particle_Red.tga");   // +0x34
        blueLaser = NewLaser<LaserDef>(lvl, "Cannon_ShieldDeactivator");
        blueLaser->value68[0] = 0.25f;
        blueLaser->value68[1] = 1.0f;
        blueLaser->value68[2] = 0.25f;
        blueLaser->value80 = 20000.0f;                // 0x469C4000
        blueLaser->flag50 = 1;
        blueLaser->emitter34 = NewTrail(lvl, "Cannon_ShieldDeactivator_Trail", "Cannon_Particle_Blue.tga");
        // orig: new(0x34) RippleDef("Ripple_CannonLaser", "Ripple_white_circle.tga", 0) 0x00071710 -> +0x8C
        RippleDef* rp = NewRippleDef(lvl, "Ripple_CannonLaser", "Ripple_white_circle.tga", 0.0f);
        blueLaser->ripple8C = rp;
        if (rp) rp->value20 = 2.0f;                   // ripple +0x20 (no NULL test in the original)
        blueLaser->value90 = 0.75f;                   // +0x90
        blueLaser->value94 = 30.0f;                   // +0x94
    }
    // (+0x70 = +0x71 = 0; +0 / +4 released; +0xA0 = +0xA4 = 0.25, +0x9C = 1; +0xA8 = 0; +0x90 = -1: the initialisers)
}

CannonCtrl::~CannonCtrl() {                           // orig: 0x00032E50
    if (player.pad) const_cast<input::Pad*>(player.pad)->SetRumble(0.0f, 0.0f, false);   // P+0x7B8 (tested)
    // (+8 and the array +0 / +4 released by the members)
}

void CannonCtrl::Deactivate() {                       // orig: 0x00032ED0
    if (!gun) return;
    if (ni::NiNode* n = player.GetNode()) n->flags = static_cast<u16>(n->flags & ~ni::kNiAVHidden);   // vf30; +0xB0
    if (level.camera) level.camera->SetFollowTarget(&player);   // S+0x8C, 0x00018FB0(P)
    if (gun->name == "Gun_KA") {                      // GetName 0x000E2F70; REPE CMPSB 7 (0x002956A0)
        pitchKA = pitch;                              // +0x7C = +0x74
        yawKA = yaw;                                  // +0x84 = +0x78
    } else if (gun->name == "Gun_KA1") {              // (0x00295698)
        pitchKA1 = pitch;                             // +0x80
        yawKA1 = yaw;                                 // +0x88
    }
    gun = nullptr;                                    // +8 released
    for (auto& f : fire) f = nullptr;                 // +0 / +4
    // P+0x7B8 SetRumble(0, 0, 0) 0x0009B740 (the pad is not null-checked in the original)
    if (player.pad) const_cast<input::Pad*>(player.pad)->SetRumble(0.0f, 0.0f, false);
}

void CannonCtrl::Activate(const char* name) {         // orig: 0x00033980 (thiscall, RET 4)
    Deactivate();                                     // 0x00032ED0
    if (!name || !level.currentStage) return;         // S+0x5C
    gun = FindInRooms(*level.currentStage, name);     // 0x000CE4A0 (+8, reference counted)
    if (!gun) return;
    if (level.hud) level.hud->SetActiveAll(true);     // S+0x118 0x000E5A60(1)
    const std::string n = name;                       // (REPE CMPSB with each name's terminator: equality)
    if (n == "Gun") {                                 // 0x0029580C
        colour = {1.0f, 0.25f, 0.25f};                // +0x9C..+0xA4
    } else if (n == "Gun_2") {                        // 0x00295804
        colour = {0.25f, 1.0f, 0.25f};
    } else if (n == "Gun_KA") {                       // 0x002956A0
        sound = level.Sound().Load("sounds/weapons/Cannon_Left.wav", 0);   // Sound_Load 0x000C9860 -> +0xAC
        pitch = pitchKA;                              // +0x74 = +0x7C
        yaw = yawKA;                                  // +0x78 = +0x84
        if (!haveKA) {
            baseKA = gun->local.rotate;               // +0x10 = node +0x30 (12 dwords)
            haveKA = 1;
        }
    } else if (n == "Gun_KA1") {                      // 0x00295698
        sound = level.Sound().Load("sounds/weapons/Cannon_Right.wav", 0);
        pitch = pitchKA1;                             // +0x74 = +0x80
        yaw = yawKA1;                                 // +0x78 = +0x88
        if (!haveKA1) {
            baseKA1 = gun->local.rotate;              // +0x40
            haveKA1 = 1;
        }
    }
    // the camera behind the gun: (0, 2.5, -3.25) in the gun's world frame, turned by pi / 2 about y
    const ni::NiMatrix3 R = gun->world.rotate;        // +0x70 (copy at esp+0x60)
    const NiPoint3 o = ni::MulVector(R, {0.0f, 2.5f, Bits(0xC0500000u)});   // 0x00011770
    const NiPoint3& t = gun->world.translate;         // +0xA0
    const NiPoint3 pos{F(double(t.x) + o.x), F(double(t.y) + o.y), F(double(t.z) + o.z)};
    const ni::NiMatrix3 rot = ni::Multiply(R, ni::MakeYRotation(Bits(kHalfPi)));   // 0x001030B0, 0x00013B30
    if (level.camera) level.camera->SetStatic(&pos, &rot);   // 0x00015040(&pos, &rot, 0)
    if (auto* node = dynamic_cast<ni::NiNode*>(gun.Get())) {   // NiObject::DynamicCast 0x00011470 (NiNode 0x002F9230)
        for (u32 i = 1; i <= 2; ++i) {                // the two fire points: "%sfire_%d" (0x002957B8), else "%sFire_%d"
            char buf[256];
            std::snprintf(buf, sizeof buf, "%sfire_%u", name, i);   // sprintf 0x00166FE4
            fire[i - 1] = GetObjectByName(node, buf); // vf54
            if (!fire[i - 1]) {
                std::snprintf(buf, sizeof buf, "%sFire_%u", name, i);   // (0x002957AC)
                fire[i - 1] = GetObjectByName(node, buf);
            }
        }
    }
    cooldown = 0.0f;                                  // +0x8C
    if (ni::NiNode* pn = player.GetNode()) pn->flags = static_cast<u16>(pn->flags | ni::kNiAVHidden);   // vf30
}

void CannonCtrl::Update() {                           // orig: 0x00032FB0
    if (!gun) return;
    const double dt = g_frameStep;
    // the stick turns the gun: yaw by moveX, pitch by moveY, half a radian per second at full deflection
    yaw = F((dt * player.moveX) * 0.5 + yaw);         // FLD dt; FMUL P+0x4D8; FMUL [0x00293AA0]; FADD +0x78
    pitch = F((dt * player.moveY) * 0.5 + pitch);     // P+0x4DC; +0x74
    const double halfPi = Bits(kHalfPi);
    if (halfPi - 0.5 < yaw) yaw = F(halfPi - 0.5);    // FCOM; TEST AH,5; JP (NaN kept)
    else if (0.5 - halfPi > yaw) yaw = F(0.5 - halfPi);   // TEST AH,0x41; JNE
    if (halfPi * 0.25 < pitch) pitch = F(halfPi * 0.25);   // [0x00293B50]
    else if (halfPi * double(Bits(0xBE6147AEu)) > pitch) pitch = F(halfPi * double(Bits(0xBE6147AEu)));   // -0.22
    const ni::NiMatrix3 Y = ni::MakeYRotation(yaw);   // 0x001030B0
    const ni::NiMatrix3 X = ni::MakeXRotation(pitch); // 0x00103070
    if (gun->name == "Gun_KA" || gun->name == "Gun_KA1") {   // GetName; REPE CMPSB 7 / 8
        const ni::NiMatrix3& base = gun->name == "Gun_KA" ? baseKA : baseKA1;   // +0x10 / +0x40
        gun->local.rotate = ni::Multiply(ni::Multiply(base, Y), X);   // 0x00013B30 twice: (base Y) X
        UpdateNodeAt(gun.Get(), 0.0f);                // NiAVObject::Update(0, 1) 0x001018D0
    }
    const ni::NiMatrix3 turn = ni::MakeYRotation(Bits(kHalfPi));   // esp+0x90
    // the camera: W (2.5 col1 - 3.25 col2 + 0 col0) + the gun's world point; z stays on the x87 stack
    const ni::NiMatrix3 W = gun->world.rotate;        // +0x70 (copy at esp+0x30)
    const auto& m = W.m;
    const double k2 = Bits(0x40500000u);              // [0x002956E4] 3.25
    // [0x00293B10] 2.5, [0x00293AB8] 0.0
    const double cz = (double(m[2][1]) * 2.5 - double(m[2][2]) * k2) + double(m[2][0]) * 0.0;
    const f32 cy = F((double(m[1][1]) * 2.5 - double(m[1][2]) * k2) + double(m[1][0]) * 0.0);
    const f32 cx = F((double(m[0][1]) * 2.5 - double(m[0][2]) * k2) + double(m[0][0]) * 0.0);
    const NiPoint3& t = gun->world.translate;         // +0xA0
    const NiPoint3 pos{F(double(cx) + t.x), F(double(cy) + t.y), F(cz + t.z)};
    const ni::NiMatrix3 rot = ni::Multiply(W, turn);  // 0x00013B30
    if (level.camera) level.camera->SetStatic(&pos, &rot);   // 0x00015040(&pos, &rot, 0)
    // the fire button: binding 0x1F of the player's action map (button 7, A) on the player's pad (P+0x7B8)
    const f32 v = player.pad ? player.pad->Value(player.actions.Binding(0x1F)) : 0.0f;   // 0x0005DFA0, 0x0009BA60
    if (cooldown > 0.0f) cooldown = F(double(cooldown) - dt);   // FCOMP 0; TEST AH,0x41; JNE (<= 0 and NaN: 0)
    else cooldown = 0.0f;
    if (!(v == 0.0f) && cooldown <= 0.0f && redLaser && (ammo > 0 || ammo == -1)) {   // TEST AH,0x44; JNP / AH,0x41; JP
        for (const auto& fp : fire) {
            if (!fp) continue;
            const NiPoint3 at = fp->world.translate;  // +0xA0 (esp+0x18)
            const ni::NiMatrix3 frame = ni::Multiply(fp->world.rotate, turn);   // +0x70 * the pi / 2 turn
            redLaser->value68[0] = colour[0];         // +0x68..+0x70 = +0x9C..+0xA4 (always the red one)
            redLaser->value68[1] = colour[1];
            redLaser->value68[2] = colour[2];
            LaserDef* d = activeAmmo == 0 ? redLaser : activeAmmo == 1 ? blueLaser : nullptr;   // (2: no shot)
            if (!d) continue;
            GameObject* g = d->Spawn(level, &level, at, frame, "Cannon-Laser");   // vf14(S, &at, &frame, 0x002956D4)
            if (auto* pr = dynamic_cast<Projectile*>(g)) pr->shooter = &player;   // +0x40 = P
        }
        if (ammo != -1) --ammo;                       // +0x90
        if (level.camera) level.camera->SetShake(Bits(0x3DCCCCCDu), Bits(0x3DCCCCCDu));   // 0x00011290(0.1, 0.1)
        pitch = F(double(pitch) + Bits(0x3D4CCCCDu)); // the recoil: FADD [0x00293BDC] (0.05)
        sound::GameSound& snd = level.Sound();
        if (sound) {
            snd.PlayAttached(sound, gun.Get(), false);   // Sound_PlayAttached 0x000CAB80(+0xAC, node, 0)
        } else {
            const u32 s = snd.Load("sounds/weapons/laser shotgun fire 2.wav", 0);   // 0x002956AC
            snd.SetPitch(static_cast<s32>(s), Bits(0x3E4CCCCDu));   // Voice_SetPitch 0x000C9BD0(sample, 0.2)
            snd.PlayAttached(s, gun.Get(), false);
        }
        cooldown = Bits(0x3E99999Au);                 // +0x8C = 0.3
    }
    if (cooldown >= 0.0f) {                           // FCOMP 0; TEST AH,1; JNE (below 0 and NaN: no rumble)
        const double c = cooldown;
        const f32 r = F(c * c * double(Bits(0x4131C71Cu)));   // FMUL ST1; FMUL [0x002956A8] (11.111111)
        // (P+0x7B8)->SetRumble(r, r, 0) 0x0009B740 (no NULL test in the original)
        if (player.pad) const_cast<input::Pad*>(player.pad)->SetRumble(r, r, false);
    }
}

}  // namespace fut::game

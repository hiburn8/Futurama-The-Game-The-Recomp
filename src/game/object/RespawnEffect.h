// The respawn effect object (vtbl 0x0029C750, 0x58 bytes; spawned by RespawnEffectDef 0x000715D0 for FsAtRespawn, native
// 328): two camera-facing billboards, each with a 20 x 20 textured quad ("respawn.tga", the second a clone of the first),
// that spin in opposite directions, pulse in size and fade with |sin|; the player's node is hidden from its Init until
// the effect's clock passes 1.0, and the effect ends after 1.75 (its clock runs at twice the frame step). Decompiled
// from default.xbe; docs/analysis/quicksave_port.md 4.
//
// The pickup recharge effect (vtbl 0x0029C688, 0x5C bytes; RechargeEffectDef 0x000708D0 for the ammo / health pickups
// in Stage::Update): one camera-facing 2 x 2 quad with the definition's texture n (Health_25 .. Ammo_100), above the
// player, rising 3 units a second from 12 above where it started while following the player's x / z, fading out by
// 1 - t / 2 and pulsing in size with |sin(0.8 t)| + 1; it ends after 2.0 (its clock runs at twice the frame step).
//
// The stencil properties are the inline constructor's (NiStencilProperty's 0x00026B60 fields, then +0x18 = 0, +0x34 = 3,
// as the ripples' and scorch marks').
#pragma once

#include "game/object/GameObject.h"

#include <array>

namespace fut::game {

class RespawnEffectDef;
class RechargeEffectDef;
class ChargeUpEffectDef;

class RespawnEffect : public GameObject {
public:
    RespawnEffect(LevelData& level, RespawnEffectDef& def);   // orig: inline in 0x000715D0 (GameObject 0x00096CA0)
    ~RespawnEffect() override;                        // orig: 0x00070AF0
    bool Init() override;                             // orig: vf04 0x00070E30
    bool Tick() override;                             // orig: vf08 0x00070BE0
    bool PostUpdate() override;                       // orig: vf0C 0x00070D80

    ni::NiPointer<ni::NiTriShape> quad;               // orig: +0x38
    ni::NiPointer<ni::NiTriShape> quadClone;          // orig: +0x3C (NiObject::Clone of +0x38, before its material)
    ni::NiPointer<ni::NiMaterialProperty> material;   // orig: +0x40 (on +0x38)
    ni::NiPointer<ni::NiMaterialProperty> cloneMaterial;   // orig: +0x44 (on +0x3C)
    ni::NiPointer<ni::NiAlphaProperty> alphaProp;     // orig: +0x48
    f32 value4C = 1.0f;                               // orig: +0x4C (ctor 1.0, Init 0.4; no reader found)
    ni::NiPointer<ni::NiBillboardNode> billboard;     // orig: +0x50 (holds +0x38)
    ni::NiPointer<ni::NiBillboardNode> cloneBillboard;   // orig: +0x54 (holds +0x3C)
};

class RechargeEffect : public GameObject {
public:
    RechargeEffect(LevelData& level, RechargeEffectDef& def);   // orig: inline in 0x000708D0 (GameObject 0x00096CA0)
    ~RechargeEffect() override;                       // orig: 0x0006FE90 (vf00 0x000709C0)
    bool Init() override { return false; }            // orig: vf04 0x000AEDD0 (xor al, al; not called by the spawn)
    bool Tick() override;                             // orig: vf08 0x0006FF40
    bool PostUpdate() override;                       // orig: vf0C 0x00070070
    void SetUp(s32 n);                                // orig: vf50 0x00070120 (thiscall(n), RET 4)

    ni::NiPointer<ni::NiTriShape> quad;               // orig: +0x38
    ni::NiPointer<ni::NiMaterialProperty> material;   // orig: +0x3C
    ni::NiPointer<ni::NiAlphaProperty> alphaProp;     // orig: +0x40
    f32 value44 = 1.0f;                               // orig: +0x44 (ctor 1.0, SetUp 0.4; no reader found)
    ni::NiPointer<ni::NiBillboardNode> billboard;     // orig: +0x48 (holds +0x38)
    s32 level4C = 0;                                  // orig: +0x4C (the texture index)
    ni::NiPoint3 start;                               // orig: +0x50 (the player's position at SetUp)
};

// The charge pickup's effect (vtbl 0x0029BC48, 0x78 bytes; ChargeUpEffectDef 0x00066290 from Charge::OnAttach): a
// screen polygon of the definition's texture at the pickup's screen position (0.4 x 0.53 of the screen at the start)
// that moves to its HUD charge slot (+0x50 + 8 n) by a tenth of the distance a step while its size pulses with
// |cos(3.3 t)|; at the slot it leaves the scene camera and the HUD's "charge" shows n + 1.
class ChargeUpEffect : public GameObject {
public:
    ChargeUpEffect(LevelData& level, ChargeUpEffectDef& def);   // orig: 0x000660E0
    ~ChargeUpEffect() override;                       // orig: 0x00066150 (vf00 0x00066360)
    bool Init() override;                             // orig: vf04 0x00066380
    bool Tick() override;                             // orig: vf08 0x000661E0
    bool PostUpdate() override { return true; }       // orig: vf0C 0x000A32A0 (mov al, 1)
    // orig: 0x00065E60 (thiscall(f32 rate), RET 4): true at the slot (x within 0.01), else the step and the quad.
    bool Move(f32 rate);

    ni::NiPointer<ni::NiScreenPolygon> polygon;       // orig: +0x38
    ni::NiPointer<ni::NiMaterialProperty> material;   // orig: +0x3C
    ni::NiPointer<ni::NiAlphaProperty> alphaProp;     // orig: +0x40 (released by the dtor; no writer found)
    f32 value44 = 1.0f;                               // orig: +0x44 (ctor 1.0; no reader found)
    f32 x = 0, y = 0;                                 // orig: +0x48 / +0x4C (the screen position)
    std::array<std::array<f32, 2>, 4> slots{};        // orig: +0x50 (the ctor's four HUD charge slots)
    f32 size = 256.0f;                                // orig: +0x70
    u8 slot = 0;                                      // orig: +0x74 (the definition's +0x2C at Init)
};

}  // namespace fut::game

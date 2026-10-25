// The Impact effect object (vtbl 0x0029A200, ctor 0x0004EF70, 0x60 bytes): a clone of an ImpactDef's animated mesh
// played once (or held at its end) at a position, optionally following a node or facing the camera. Spawned by
// ImpactDef vf14 0x0004F710 (Spawn), 0x0004EE90 (SpawnUp: the effect's z axis along a direction) and 0x0004F650
// (SpawnBillboard). Users: Leela's kicks / punches, Bender's spin (Bender_Spin / Bender_SpinAir), HitSweep's
// Smack_Impact sparks. Decompiled from default.xbe; docs/analysis/player_moves_port.md 5.
//
// The lit variant 0x0004F430 (SetLit: Bender's death effects) puts the level's environment texture effect on the
// clone and a dim ambient light on the node.
#pragma once

#include "game/object/GameObject.h"

namespace fut::game {

class ImpactDef;
class Stage;

class Impact : public GameObject {
public:
    Impact(LevelData& level, ImpactDef& def);         // orig: 0x0004EF70
    ~Impact() override;                               // orig: 0x0004F040
    bool Init() override;                             // orig: vf04 0x0004F110
    bool Tick() override;                             // orig: vf08 0x0004EAA0
    bool PostUpdate() override;                       // orig: vf0C 0x0004F2E0
    void SetPosition(const ni::NiPoint3& p) override; // orig: vf1C 0x0004EAD0 (no node update, no UpdateRoom)
    // orig: 0x0004F580 (thiscall(NiPointer<NiAVObject> by value), RET 4): the node's local translation becomes zero
    // and the node takes the followed node's world transform in every PostUpdate.
    void SetFollow(ni::NiAVObject* n);
    // orig: 0x0004F430 (thiscall(NiDynamicEffect*), RET 4; Player::BenderDeathEffect with S+0xE4): nothing without
    // the effect; else +0x44 = it, the clone's properties prepared (0x0003F8D0 / 0x0003FB30), a new ambient light
    // (0.7) on the node, the effect on the clone, both updated, the clone's lighting 0x000966A0.
    void SetLit(ni::NiDynamicEffect* fx);

    // orig: +0x3C, the billboard spin angle (0x0004F650; never written for the vf14 / SpawnUp objects, whose word
    // keeps the heap's leftover bytes: read only by billboards, the port starts it at 0).
    f32 angle = 0;
    u32 f40 = 0;                                      // orig: +0x40 (zeroed by the ctor; no other writer seen)
    ni::NiPointer<ni::NiDynamicEffect> lit;           // orig: +0x44 (SetLit's effect: S+0xE4, a texture effect)
    ni::NiPointer<ni::NiNode> clone;                  // orig: +0x48, the clone of the definition's mesh (Init)
    ni::NiPointer<ni::NiAVObject> follow;             // orig: +0x4C (SetFollow)
    f32 speed = 0;                                    // orig: +0x50 (animation time = time * speed)
    u8 hold = 0;                                      // orig: +0x54 (hold the last pose instead of dying)
    u8 billboard = 0;                                 // orig: +0x55 (face the camera; vf14 writes 0, 0x0004F650 1)
    f32 duration = 0;                                 // orig: +0x58
    Stage* stage = nullptr;                           // orig: +0x5C (the stage Init found, or null)
};

// orig: 0x00073720 (thiscall(f32), RET 4, a GameObject helper): the node's scale = |s| (no update).
void SetNodeScale(GameObject& o, f32 s);

}  // namespace fut::game

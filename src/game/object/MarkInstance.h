// The scorch-mark factory's instance (SMarkFactoryDef vf20 0x00072ED0; MarkInstance vtbl 0x0029C910, ctor 0x00072D60,
// 0x50 bytes; base EffectInstance 0x00065CE0 = GameObject + an Owner at +0x38): created once per level by the effects
// set-up (0x000C59E0), owned by the level object, its node an empty child of "Session:m_spDynamicNode".
// docs/analysis/player_spawn_deps.md 2. The marks it places (ScorchMark: the projectiles' impacts) and their per-frame
// code: docs/analysis/fireball_port.md 9.2.
//
// NOT PORTED: the instance's own Tick 0x00072E90 -> 0x00065E20; the marks' render state is built (the quad and its
// six properties) but not drawn.
#pragma once

#include "game/object/GameObject.h"

#include <vector>

namespace fut::game {

class ScorchMarkDef;
class SMarkFactoryDef;

// A scorch mark (orig: vtbl 0x0029C850, 0x54 bytes; spawned by ScorchMarkDef 0x00072590).
class ScorchMark : public GameObject {
public:
    ScorchMark(LevelData& level, ScorchMarkDef& def);   // orig: the GameObject ctor + 0x000725FB
    ~ScorchMark() override;                           // orig: 0x000722F0
    bool Init() override { return false; }            // orig: vf04 0x000AEDD0 (the spawn calls Build instead)
    bool Tick() override;                             // orig: vf08 0x00072400
    bool PostUpdate() override;                       // orig: vf0C 0x000724C0 (the fade)
    // orig: vf50 0x00072740: the lifetime, the quad (2 RandRange draws) and its properties.
    bool Build(u32 type);

    u32 type = 0;                                     // orig: +0x38
    u8 dying = 0;                                     // orig: +0x3C (deleted by the next Tick)
    ni::NiPointer<ni::NiTriShape> shape;              // orig: +0x40
    ni::NiPointer<ni::NiMaterialProperty> material;   // orig: +0x44
    ni::NiPointer<ni::NiAlphaProperty> alpha;         // orig: +0x48
    f32 value4C = 1.0f;                               // orig: +0x4C (ctor 1.0, Build 0.4; no reader found (L))
    f32 lifetime = 0;                                 // orig: +0x50
};

// orig: ScorchMarkDef::Spawn 0x00072590 (thiscall(type, owner, pos, rot, label, n by value), RET 0x20): nullptr when the
// definition has no texture.
ScorchMark* SpawnScorchMark(LevelData& level, ScorchMarkDef& def, u32 type, Owner* owner, const ni::NiPoint3& pos,
                            const ni::NiMatrix3& rot, const char* label, const ni::NiPoint3& n);

class MarkInstance : public GameObject {
public:
    MarkInstance(LevelData& level, SMarkFactoryDef& def, u32 capacity);   // orig: 0x00072D60
    // orig: 0x00072E00: every mark in a slot flagged dying (+0x3C = 1: deleted by its next Tick), the slots freed.
    ~MarkInstance() override;
    bool Init() override { return true; }            // orig: 0x000A32A0
    void OnAttach() override;                         // orig: 0x00072F70: node update at the object time

    // orig: 0x000730C0 (thiscall(type, pos, n by value), RET 0x14): a mark of `type` at `pos` facing `n`.
    bool AddScorch(u32 type, const ni::NiPoint3& pos, ni::NiPoint3 n);   // (returns AddMark's result)
    // orig: 0x00072F90 (thiscall(type, pos, rot*, n by value), RET 0x18): a live mark closer than the two sizes is
    // refreshed instead (false); otherwise the next slot's mark is replaced (true).
    bool AddMark(u32 type, const ni::NiPoint3& pos, const ni::NiMatrix3& rot, const ni::NiPoint3& n);

    u32 capacity = 0;                                 // orig: +0x40 (n, else 0x10)
    u32 next = 0;                                     // orig: +0x44, the slot the next mark takes
    std::vector<ScorchMark*> marks;                   // orig: +0x48 (capacity null slots)
    ScorchMarkDef* markDef = nullptr;                 // orig: +0x4C
};

// orig: SMarkFactoryDef vf20 0x00072ED0 (thiscall(def; owner, n, label)).
MarkInstance* SpawnMarkInstance(LevelData& level, SMarkFactoryDef& def, Owner* owner, u32 capacity, const char* label);

}  // namespace fut::game

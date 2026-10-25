// The effect-instance base and the spell effect. Decompiled from default.xbe (checked against the assembly of
// 0x00065CE0, 0x00065D40, 0x00065E20, 0x00073560, 0x00073390, 0x00073220, 0x00073310 and 0x00073630).
//
// EffectInstance (vtbl 0x0029BBA0, 0x40 bytes) is the base of the effect objects (the explosion, the muzzle flare, the
// fire, the scorch marks, the spell); the older ports (Explosion, Flare, Fire, FireCircle, MarkInstance) derive from
// GameObject directly and keep its Tick inline (their +0x38 owner never holds an object). The spell effect is the
// first to use the class itself.
#pragma once

#include "game/object/GameObject.h"

#include <array>

namespace fut::game {

class EmitterDef;
class SpellDef;

class EffectInstance : public GameObject {            // orig: vtbl 0x0029BBA0
public:
    EffectInstance(LevelData& level, Def& def);       // orig: 0x00065CE0 (then the Owner +0x38, vtbl 0x0029BBF0)
    ~EffectInstance() override;                       // orig: 0x00065D40 (the Owner dtor 0x00097600, the GameObject's)
    // vf08: every object of the owner +0x38 ticks (vf08, the result ignored), then time += g_frameStep; alive.
    bool Tick() override;                             // orig: 0x00065E20

    // orig: +0x38 (vtbl 0x0029BBF0; the spell's 0x0029C994): AttachObject / DetachObject do nothing (0x000AEDC0),
    // IsOwnerType answers false for every type (0x00160680), GetNode is the holder's node (0x0003D800).
    class Children : public Owner {
    public:
        explicit Children(EffectInstance& e) : holder(e) {}
        void AttachObject(GameObject& object) override { (void)object; }   // orig: 0x000AEDC0
        void DetachObject(GameObject& object) override { (void)object; }   // orig: 0x000AEDC0
        bool IsOwnerType(u32 t) const override { (void)t; return false; }  // orig: 0x00160680
        ni::NiNode* GetNode() const override { return holder.node.Get(); }  // orig: 0x0003D800
        EffectInstance& holder;
    };
    Children children{*this};
};

// The spell effect (orig: vtbls 0x0029C9A8 / 0x0029C994, 0x7C bytes), spawned by SpellDef vf20 0x00073630 at two
// points: "spell_summon" by the AI's skeleton revival (AiAgent::Tick 0x0002F254, not ported) and "spell_warp" by the
// high priest's morph:hit_floor key (0x00087639, not ported), both with one point twice. Each of its two slots is a
// new node at the slot's point carrying a light-manager light (yellow, radius 10, type 12) and the source (+0x40) of
// an emitter the definition's slot emitter spawns there; Tick moves the node by the slot's kind (1: up at 4 units / s
// on a circle of radius 3 around the point, 2: straight up) and ends the effect after 5 s.
class SpellEffect : public EffectInstance {
public:
    // orig: 0x00073560 (thiscall(def, a, b), RET 0xC): +0x44 / +0x50 = a / b, the slots' emitters +0x6C = D+0x28 /
    // D+0x2C and kinds +0x74 = D+0x30 / D+0x34, +0x64 / +0x68 = 0, +0x40 = 0.
    SpellEffect(LevelData& level, SpellDef& def, const ni::NiPoint3& a, const ni::NiPoint3& b);
    ~SpellEffect() override;                          // orig: 0x00073310 (the two node references released, [1] first)
    bool Init() override;                             // orig: vf04 0x00073390
    bool Tick() override;                             // orig: vf08 0x00073220

    f32 age = 0;                                      // orig: +0x40
    std::array<ni::NiPoint3, 2> points;               // orig: +0x44 / +0x50
    std::array<ni::NiPointer<ni::NiNode>, 2> nodes;   // orig: +0x5C / +0x60 (the vector ctor 0x00166E90)
    std::array<GameObject*, 2> emitters{};            // orig: +0x64 / +0x68 (level-owned: not deleted here)
    std::array<EmitterDef*, 2> emitterDefs{};         // orig: +0x6C / +0x70
    std::array<u32, 2> kinds{};                       // orig: +0x74 / +0x78
};

}  // namespace fut::game

// Moveable / Debris: the dynamic physics props (a Moveable is a MoveableDef's object; the Debris subclass is what a
// breaking destructable throws: Glasspiece1-3 in level1-1, Metal_particle in level1-5 / 1-7). Decompiled from
// default.xbe (Moveable vtbl 0x0029CAA0, ctor 0x00073F00, 0x4C bytes; Debris vtbl 0x0029D7B0, ctor 0x0007F900, 0x50
// bytes); docs/analysis/destructable_moveable_port.md 4 and simplenpc_moveable_port.md 3.
//
// The object's dynamic CollisionBox (Physics.h) moves by its own integrate (gravity, the stage triangles), box-box,
// the characters' push (char-box) and its impulses; the post-update copies the box's position and rotation to the node.
// No shipped MoveableDef exists (only Debris occurs in play). NOT PORTED: the "sphere" shape (0x000B0040: the
// original leaves the body uninitialised and crashes).
#pragma once

#include "game/object/GameObject.h"
#include "game/physics/Physics.h"

#include <memory>

namespace fut::game {

class MoveableDef;

class Moveable : public GameObject {
public:
    Moveable(LevelData& level, MoveableDef& def);     // orig: 0x00073F00
    ~Moveable() override;                             // orig: 0x00073F80
    bool Init() override;                             // orig: vf04 0x00074050
    bool Tick() override;                             // orig: vf08 0x00073DA0
    bool PostUpdate() override;                       // orig: vf0C 0x00073DC0
    void SetPosition(const ni::NiPoint3& p) override; // orig: vf1C 0x00073D80
    // orig: vf50 0x00073D70: the impulse at a world point (CollisionBox vf40 0x000AF880; no null test of the body
    // in the original).
    void ApplyImpulse(const ni::NiPoint3& point, const ni::NiPoint3& impulse);

    std::unique_ptr<phys::CollisionBox> body;         // orig: +0x38
    u32 voice = 0;                                    // orig: +0x3C (no writer in the Moveable code)
    f32 cooldown = 0;                                 // orig: +0x40 (the impact sound's cool-down)
    ni::NiPointer<ni::NiNode> model;                  // orig: +0x44 (the definition's model clone)
    f32 sinceUpdate = 0;                              // orig: +0x48 (Tick adds the step, PostUpdate clears it)

protected:
    // orig: the model clone of 0x00074050 / 0x0007F9F0: attached, shown, identity, the collision bits cleared,
    // UpdateProperties and Update(time, 1) of the clone.
    void CloneModel();
};

class Debris : public Moveable {
public:
    Debris(LevelData& level, MoveableDef& def) : Moveable(level, def) {}   // orig: 0x0007F900 (+0x4C = 0)
    ~Debris() override {}                             // orig: 0x0007F940 (jmp 0x00073F80)
    bool Init() override;                             // orig: vf04 0x0007F9F0
    bool Tick() override { return GameObject::Tick(); }   // orig: vf08 = GameObject's 0x000630D0
    bool PostUpdate() override;                       // orig: vf0C 0x0007F950
    s32 restFrames = 0;                               // orig: +0x4C (post-updates at rest)
};

}  // namespace fut::game

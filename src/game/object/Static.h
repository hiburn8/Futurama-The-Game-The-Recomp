// Static: the object a StaticDef spawns (level props: crates, junk, seesaws, emitter placeholders). Decompiled from
// default.xbe (vtbl 0x002A0FF0, ctor 0x00095110, 0xF0 bytes); docs/analysis/game_objects.md section 4.
//
// Ported: construction, SetOwner, SetPosition (with the collision box), SetRotation (with the rotation offset),
// Init, the per-frame Tick (rotational / vertical springs, rest kicks, standing torque, ground probe, splash) and
// PostUpdate, ResetPhysics, SetFollowTarget, StaticDef::Spawn, the static collision box (docs/analysis/
// static_tick_port.md), the Polygon shape's collision mesh records (0x000FC110 / 0x000FC2D0), the jump / idle
// sounds through the level's sound service. NOT PORTED: the Ripple object (RippleDef::Spawn 0x00071B10), the cull traversal's
// frames-not-drawn counter (NiAVObject +0xB2).
#pragma once

#include "game/object/GameObject.h"
#include "game/physics/Physics.h"

#include <memory>

namespace fut::game {

class StaticDef;

class Static : public GameObject {
public:
    Static(LevelData& level, StaticDef& def);         // orig: 0x00095110
    ~Static() override;                               // orig: 0x00095220
    bool Init() override;                             // orig: 0x00095320
    bool Tick() override;                             // orig: vf08 0x000945F0 (Stage::Update)
    bool PostUpdate() override;                       // orig: vf0C 0x00094F50 (Stage::Render)
    void SetOwner(Owner* newOwner) override;          // orig: 0x00094560 (stage owners only)
    void SetPosition(const ni::NiPoint3& p) override; // orig: 0x00094FF0
    void SetRotation(const ni::NiMatrix3& m) override;   // orig: 0x000955D0
    void ResetPhysics();                              // orig: vf50 0x00094590 (Stage::Reset)
    void SetFollowTarget(ni::NiAVObject* target);     // orig: 0x000956C0 (native 304)

    std::unique_ptr<phys::CollisionBox> box;          // orig: +0x38 (Box shape: a static collision box)
    ni::NiPointer<ni::NiNode> model;                  // orig: +0x3C, the clone of the definition's model ("Mesh")
    ni::NiPointer<ni::NiAVObject> follow;             // orig: +0x40 (native 304)
    ni::NiPoint3 followOffset;                        // orig: +0x44 (uninitialised until SetFollowTarget)
    ni::NiPoint3 basePosition;                        // orig: +0x50 (the SetPosition argument)
    f32 verticalOffset = 0;                           // orig: +0x5C (bob / physics displacement)
    f32 verticalVelocity = 0;                         // orig: +0x60
    f32 prevAcceleration = 0;                         // orig: +0x64 (written only while the ripple gate is open)
    f32 splash = 0;                                   // orig: +0x68 (peak |v| of a rebound, <= 1, decays)
    f32 rippleTimer = 3.4028234663852886e38f;         // orig: +0x6C (FLT_MAX: ground not probed yet)
    f32 groundOffset = 0;                             // orig: +0x70
    ni::NiMatrix3 baseRotation;                       // orig: +0x80 (the SetRotation argument)
    ni::NiPoint3 angularOffset;                       // orig: +0xB0 (axis * angle on top of the base rotation)
    ni::NiPoint3 angularVelocity;                     // orig: +0xBC
    bool seesaw = false;                              // orig: +0xC8 (definition name starts "seesaw_")
    ni::NiPoint3 seesawAxis;                          // orig: +0xCC (unset in the original until Init)
    // orig: +0xD8 / +0xDC, never initialised (heap bytes); only compared with live voice ids (Voice_Exists).
    s32 idleVoice = 0, jumpVoice = 0;
    bool jumpSoundPlayed = false;                     // orig: +0xE0
};

}  // namespace fut::game

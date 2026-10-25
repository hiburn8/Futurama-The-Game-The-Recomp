// The explosion object (vtbls 0x0029BDE8 / 0x0029BDD4, ctor 0x000669E0, 0x60 bytes; base EffectInstance 0x00065CE0 =
// GameObject + an Owner at +0x38): a clone of one of the level ExplosionDef's eleven meshes, three spark emitters
// ("FastParticles" / "MediumParticles" / "SlowParticles", created by the constructor and registered in the level's
// definition list), a shockwave mesh for some kinds, a camera shake and a fading light-manager source. Spawned by
// ExplosionDef vf20 0x000676C0 (the projectiles' ProjectileExplosion, the destructables' parts and mines). Decompiled
// from default.xbe; docs/analysis/explore_leftovers_port.md 3.
//
// The explosion sound plays through the level's sound service (ExplosionDef::Spawn).
#pragma once

#include "game/object/GameObject.h"

namespace fut::game {

class ExplosionDef;
class LMSource;
class SpriteEmitterDef;

class Explosion : public GameObject {
public:
    Explosion(LevelData& level, ExplosionDef& def);   // orig: 0x000669E0
    ~Explosion() override;                            // orig: 0x00066D70
    bool Init() override;                             // orig: vf04 0x00066E90
    bool Tick() override;                             // orig: vf08 0x000673D0
    bool PostUpdate() override;                       // orig: vf0C 0x000674D0

    f32 life = 0;                                     // orig: +0x40 (Init: 2.0, 0.99 for the kinds with sparks)
    ni::NiPointer<ni::NiAVObject> model;              // orig: +0x44, the clone of the kind's mesh
    ni::NiPointer<ni::NiAVObject> shockwave;          // orig: +0x48, the clone of mesh slot 4 (kinds 0 / 5 / 6 / 8)
    s32 kind = 0;                                     // orig: +0x4C (the mesh slot, 0..10)
    SpriteEmitterDef* fast = nullptr;                 // orig: +0x50 (owned by the level's definition list)
    SpriteEmitterDef* medium = nullptr;               // orig: +0x54
    SpriteEmitterDef* slow = nullptr;                 // orig: +0x58
    ni::NiPointer<LMSource> light;                    // orig: +0x5C (a child of the node too)
};

}  // namespace fut::game

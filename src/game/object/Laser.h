// The projectiles: the base Projectile (orig: vtbl 0x0029D0A8, ctor 0x0007A4A0, 0x100 bytes; what a plain
// ProjectileDef spawns: the NPCs' "snot", "eyestalk", "granate", ... shots) and the laser / fireball derived from it
// (orig: Laser vtbl 0x0029CEE8, ctor 0x00077D40, 0x160 bytes; native 305 FsCreateFireball, the NPCs' and the weapons'
// shots). Decompiled from default.xbe; docs/analysis/fireball_port.md, docs/analysis/npc_combat2_port.md 1.7 / 1.8.
//
// Ported: the spawns (ProjectileDef vf20 0x0007BC00, LaserDef vf20 0x000798C0), the constructors, the Projectile's
// Init / Tick / PostUpdate (the step with gravity, the health triggers, the stage / mesh bounce with restitution, the
// object ray, the move, the fire trail, the spin step) / Tumble / Explode (the emitter, the three blast radii through
// the shooter's DealDamage, the destructables), the Laser's Init model path (the clone, the stage ray, the light
// source), Tick, PostUpdate's reached paths (the health-trigger scan of the first update, the step with the
// physics-object sweep ignoring the shooter's body, the character hit with its sparks / noise / damage / push, the
// model spin, the move, the stage impact with effect_metal / effect_generic, effect_smoke and the scorch mark), the
// deletion; the beam Init of a laser without a model (the NiTriStrips quads, object_gaps.md 21.1), the
// destructable hit (vf64) and the sounds (the attached loop and its stop, the impact sounds).
//
// NOT PORTED (reported when reached; all unreachable with the shipped definitions, KNOWN_ISSUES #45 / #52): the
// Projectile's sphere sweep (D+0x54 > 0: no writer), Projectile::Explode's scorch marks (D+0x64 != 1: only LaserDef
// writes it, and a Laser never calls 0x00079E70), the laser's / RailLaser's D+0x86 explosion and D+0x88 shock (no
// writer).
#pragma once

#include "engine/ni/NiCollision.h"
#include "game/object/GameObject.h"

#include <array>

namespace fut::phys {
class PhysObject;
}

namespace fut::game {

class Character;
class LaserDef;
class ProjectileDef;
struct ScriptTrigger;

class Projectile : public GameObject {                // orig: vtbl 0x0029D0A8, ctor 0x0007A4A0, 0x100 bytes
public:
    Projectile(LevelData& level, ProjectileDef& def); // orig: 0x0007A4A0
    ~Projectile() override;                           // orig: 0x0007A550
    bool Init() override;                             // orig: vf04 0x0007B8B0
    bool Tick() override;                             // orig: vf08 0x0007A5E0
    bool PostUpdate() override;                       // orig: vf0C 0x0007A690
    virtual void Tumble();                            // orig: vf54 0x0007BAD0
    void Explode();                                   // orig: 0x00079E70

    f32 trailTimer = 0;                               // orig: +0x38 (seconds since the last trail emitter)
    u32 loopVoice = 0;                                // orig: +0x3C
    Character* shooter = nullptr;                     // orig: +0x40 (GenericNpc vf104 sets it)
    ni::NiPointer<ni::NiNode> model;                  // orig: +0x44
    f32 speed = 0, restitution = 0, gravity = 0, lifetime = 0;   // orig: +0x48 / +0x4C / +0x50 / +0x54 (D+0x1C..+0x28)
    u8 explodeOnContact = 0;                          // orig: +0x58 (D+0x2C)
    std::array<f32, 6> blast{};                       // orig: +0x5C..+0x70: (damage, radius) x 3 (D+0x38..+0x4C)
    u32 arg74 = 0;                                    // orig: +0x74 (the spawn's arg4; vf104 kind 0x11 sets 1)
    ni::NiPoint3 start;                               // orig: +0x78 (the node's translate at Init)
    ni::NiPoint3 velocity;                            // orig: +0x84
    ni::NiMatrix3 tumble;                             // orig: +0x90 (the tumble rotation; chunk k = column k)
    ni::NiPoint3 spinAxis{0.0f, 1.0f, 0.0f};          // orig: +0xC0
    f32 spinSpeed = 0, spinAngle = 0;                 // orig: +0xCC / +0xD0
    f32 lastTime = 0;                                 // orig: +0xD4
    u8 hit = 0;                                       // orig: +0xD8
    ni::NiPoint3 hitPoint;                            // orig: +0xDC
    ni::RayHit hitTri;                                // orig: +0xE8 (pass 2: &g_hit, overwritten by later rays)
    phys::PhysObject* hitObject = nullptr;            // orig: +0xEC
    u8 exploded = 0;                                  // orig: +0xF0 (Explode: Tick deletes the object)
};

class Laser : public Projectile {
public:
    Laser(LevelData& level, LaserDef& def);           // orig: 0x00077D40 (Projectile 0x0007A4A0)
    ~Laser() override;                                // orig: 0x00077CF0 -> Projectile 0x0007A550
    bool Init() override;                             // orig: vf04 0x00078FB0
    bool Tick() override;                             // orig: vf08 0x00077D90
    bool PostUpdate() override;                       // orig: vf0C 0x00077EE0
    void FindHealthTrigger();                         // orig: 0x00077DC0
    // orig: vf50 0x00079E10 (geometry, hit triangle): the character owning the mesh (0x00036A20) reacts.
    void OnMeshHit(ni::NiTriShape* geom, const ni::RayHit& hit);

    std::array<f32, 4> colour{};                      // orig: +0x100..+0x10C
    f32 radius = 0;                                   // orig: +0x118
    u8 spin = 0;                                      // orig: +0x11C
    u8 firstUpdate = 1;                               // orig: +0x11D
    ni::NiPoint3 origin, direction, normal;           // orig: +0x120, +0x12C, +0x138
    f32 stageDist = 0;                                // orig: +0x144
    f32 damage = 0;                                   // orig: +0x148
    ScriptTrigger* hitTrigger = nullptr;              // orig: +0x14C
    f32 triggerDist = 0;                              // orig: +0x150
    u8 done = 0;                                      // orig: +0x154
    f32 frameTime = 0;                                // orig: +0x158
};

// orig: RailLaser (vtbl 0x0029D170, new(0x170) in RailDef vf20 0x0007CDB0: Laser 0x00077D40 + its vtable, +0x160 /
// +0x164 / +0x168 = 0; dtor 0x0007BF10): the railgun's beam, a hit-scan shot. Init finds the hit at once (the stage
// ray with both triangle kinds, the inventory owner's health triggers, the physics objects' ray), spawns the impact
// effects there and builds the beam (the laser's three crossed quads as long as the hit distance); PostUpdate hands
// the damage to the hit object once and fades the beam over 0.3 s; Tick deletes it after 0.3 s.
class RailLaser : public Laser {
public:
    RailLaser(LevelData& level, LaserDef& def);       // orig: 0x0007CDE8..0x0007CE08
    ~RailLaser() override;                            // orig: 0x0007BF10
    bool Init() override;                             // orig: vf04 0x0007C0E0
    bool Tick() override;                             // orig: vf08 0x0007BEE0
    bool PostUpdate() override;                       // orig: vf0C 0x0007BF90

    ni::NiPointer<ni::NiMaterialProperty> beamMaterial;   // orig: +0x160 (the strips' colour)
    ni::NiPointer<ni::NiMaterialProperty> coreMaterial;   // orig: +0x164 (the clone's white, alpha 0.3)
    GameObject* target = nullptr;                     // orig: +0x168 (the object the ray hit, until PostUpdate)
};

}  // namespace fut::game

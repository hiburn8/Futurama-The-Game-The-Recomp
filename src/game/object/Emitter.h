// Particle-emitter objects: what the sprite / mesh emitter definitions spawn (steam, spark fountains, debris
// pieces). Decompiled from default.xbe; docs/analysis/emitter_spawns.md.
//
// Ported: the spawn (SpriteEmitterDef 0x0006F2A0 / MeshEmitterDef 0x0006F4C0), the instance constructors, Init
// (particle records with the exact rand() sequence, the basis vectors, the particle objects, the mesh pieces), Tick,
// InitParticle and the piece spin, the per-frame simulation (PostUpdate 0x0006D2D0 / 0x0006EC30). NOT PORTED: the
// damage applied to players (Player::Damage), the cull counter (node +0xB2), the loop sound helper 0x0006D0B0, the
// embedded Owner (+0x38, unused).
#pragma once

#include "game/object/GameObject.h"

#include <vector>

namespace fut::game {

class EmitterDef;
class FlareDef;

struct EmitterParticle {                              // orig: 0x3C bytes
    ni::NiPoint3 position;                            // +0x00
    ni::NiPoint3 origin;                              // +0x0C (mesh: the node's world translate at birth)
    ni::NiPoint3 velocity;                            // +0x18
    ni::NiPoint3 spinAxis;                            // +0x24 (mesh pieces)
    f32 spinSpeed = 0;                                // +0x30
    f32 age = 0;                                      // +0x34 (negative: start delay)
    f32 lifetime = 0;                                 // +0x38
};

class EmitterInstance : public GameObject {           // orig: base vtbl 0x0029C380, ctor 0x0006DB50
public:
    EmitterInstance(LevelData& level, EmitterDef& def);
    ~EmitterInstance() override;                      // orig: 0x0006DC20 (the loop voice +0x48 stopped)
    // orig: 0x0006D0B0 (thiscall(sample, max, min), RET 0xC): a running loop voice is stopped, then a looping 3D
    // voice at the node's world translate (a fixed position: it does not follow the node), min distance unless
    // -1.0, max distance.
    void PlayLoopSound(u32 sample, f32 maxDistance, f32 minDistance);
    bool Init() override;                             // orig: 0x0006DCF0
    bool Tick() override;                             // orig: 0x0006CEC0
    void InitParticle(u32 i);                         // orig: vf50 0x0006CEF0

    // Copies of the definition's current parameters (orig: +0x4C..+0x80).
    std::array<f32, 2> speed{}, lifetime{}, elevation{}, delay{};   // orig: range28 / 30 / 38 / 40
    u32 count = 0;                                    // orig: +0x6C
    f32 drag = 0, gravity = 0;                        // orig: +0x70, +0x74
    u8 loop = 0;                                      // orig: +0x78 (flag55)
    f32 damage = 0, reach = 0;                        // orig: +0x7C, +0x80 (value58 / value5C)
    ni::NiPoint3 direction;                           // orig: +0x84 (unitised by Init)
    ni::NiPoint3 basisU, basisV;                      // orig: +0x90, +0x9C
    f32 stagger = 0;                                  // orig: +0xA8 (1 / count)
    std::vector<EmitterParticle> particles;           // orig: +0xAC
    ni::NiPointer<ni::NiParticles> textureParticles;  // orig: +0xB0 (only with the definition's texture)
    u8 emitting = 1, finished = 0;                    // orig: +0xB4, +0xB5
    f32 frameTime = 0;                                // orig: +0xB8
    u32 voice = 0;                                    // orig: +0x48 (looping sound)
    ni::NiPointer<ni::NiAVObject> sourceA;            // orig: +0x40 (emission source node; lerp with B at birth)
    ni::NiPointer<ni::NiAVObject> sourceB;            // orig: +0x44 (GenericNpc::PostThink's "sword2")
};

class SpriteEmitter : public EmitterInstance {        // orig: vtbl 0x0029C4E0, ctor 0x0006E4C0, 0xF4 bytes
public:
    SpriteEmitter(LevelData& level, EmitterDef& def);
    ~SpriteEmitter() override;                        // orig: 0x0006E5B0 (the sprites detached from the node)
    bool Init() override;                             // orig: 0x0006E680
    bool PostUpdate() override;                       // orig: vf0C 0x0006D2D0 (Session::Render)
    ni::NiPointer<ni::NiParticles> sprites;           // orig: +0xBC
    std::array<f32, 2> size{}, alpha{};               // orig: +0xC0, +0xC8
    std::array<f32, 4> startColor{}, endColor{};      // orig: +0xD0, +0xE0
    u8 randomBrightness = 0;                          // orig: +0xF0 (flagDC)
};

class MeshEmitter : public EmitterInstance {          // orig: vtbl 0x0029C548 (built in 0x0006F4C0), 0xC8 bytes
public:
    MeshEmitter(LevelData& level, EmitterDef& def);
    bool Init() override;                             // orig: 0x0006EA30
    bool PostUpdate() override;                       // orig: vf0C 0x0006EC30 (Session::Render)
    void InitPieceSpin(u32 i, bool reinit);           // orig: vf5C 0x0006D840
    u32 orient = 0;                                   // orig: +0xBC (definition currentBC)
    ni::NiPointer<ni::NiAVObject> follow;             // orig: +0xC0 (vf54)
    u8 randomMesh = 1;                                // orig: +0xC4 (vf58 0x0006E930)
};

// orig: the muzzle flare (vtbls 0x0029C0E8 / 0x0029C0D4, 0x58 bytes; EffectInstance ctor 0x00065CE0): a clone of the
// FlareDef's mesh under a node turned along the direction with a random roll; it lives one frame (PostUpdate marks
// it, the next Tick deletes it). Spawned by FlareDef vf20 0x0006A480 (GenericNpc vf104's D+0x160).
// docs/analysis/npc_combat2_port.md 1.8.1.
class Flare : public GameObject {
public:
    Flare(LevelData& level, FlareDef& def);          // orig: 0x0006A4C4 (EffectInstance 0x00065CE0)
    ~Flare() override;                                // orig: 0x0006A0F0
    bool Init() override;                             // orig: vf04 0x0006A180
    bool Tick() override;                             // orig: vf08 0x00069FC0
    bool PostUpdate() override;                       // orig: vf0C 0x00069FE0
    f32 value40 = 0;                                  // orig: +0x40 (D+0x2C at the spawn; no reader seen)
    u8 shown = 0;                                     // orig: +0x44
    ni::NiPoint3 dir;                                 // orig: +0x48
    ni::NiPointer<ni::NiNode> spinNode;               // orig: +0x54
};

}  // namespace fut::game

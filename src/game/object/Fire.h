// The Fire effect object (vtbl 0x0029C040, Owner part 0x0029C02C; ctor 0x00068AD0, 0x74 bytes): two particle layers
// (flames, smoke) of camera-facing quads plus a rotating four-quad core, spawned by FireDef::Spawn 0x00069600 (native
// FsCreateFire 295 0x000C4C30). Decompiled from default.xbe; docs/analysis/natives_batch_a_port.md 3.
//
// Also the fire circle (FireCircleDef::Spawn 0x00068B90: FireCircle, vtbl 0x0029BF08, Owner part 0x0029BEF4, 0x54
// bytes): rings of flame particles round a point (level3-4's sungod fight, natives 296-300), and the sungod's head fire
// (natives 301 / 302). NOT PORTED: the embedded Owner (+0x38, unused).
#pragma once

#include "game/object/GameObject.h"

#include <array>
#include <vector>

namespace fut::game {

class FireDef;

class Fire : public GameObject {
public:
    struct Particle {                                 // orig: 0x38 bytes (ctor 0x00067C90, dtor 0x00067CB0)
        u32 type = 0;                                 // +0x00: 0 flame, 1 smoke
        FireDef* def = nullptr;                       // +0x04
        ni::NiPointer<ni::NiBillboardNode> billboard; // +0x08
        ni::NiPointer<ni::NiTriShape> shape;          // +0x0C
        // (orig: the material is reached through the shape's property state, vf7C -> state +0x14, 0x00067EA0 /
        // 0x00067EF0: after Init's UpdateProperties that state's material is the shape's own (L, Open questions 2))
        ni::NiPointer<ni::NiMaterialProperty> material;
        f32 age = 0;                                  // +0x10 (negative: the start delay)
        f32 spin = 0;                                 // +0x14 (not set by the ctor; Fire::Init writes it first)
        f32 life = 0, invLife = 0;                    // +0x18 / +0x1C
        ni::NiPoint3 vel;                             // +0x20 (per tick, no dt; not set by the ctor: Respawn writes it)
        ni::NiPoint3 offset;                          // +0x2C (the billboard's rest translation)
        void Init(FireDef& d, const ni::NiPoint3& off, ni::NiTexturingProperty* tex, f32 delay);   // 0x00068CF0
        void Respawn();                               // orig: 0x00067D50 (4 rand())
        void Update();                                // orig: 0x00069280 (type 0) / 0x00069100 (type 1)
    };

    Fire(LevelData& level, FireDef& def, const ni::NiPoint3& pos);   // orig: 0x00068AD0
    ~Fire() override;                                 // orig: 0x000696C0
    bool Init() override;                             // orig: vf04 0x00069770
    bool Tick() override;                             // orig: vf08 0x00069EB0 (PostUpdate: the base, true)

    std::vector<Particle> flames, smoke;              // orig: +0x40 / +0x44 (new[] with the count at [-4])
    ni::NiPointer<ni::NiBillboardNode> coreNode;      // orig: +0x48 (core ctor 0x00068490)
    std::array<ni::NiPointer<ni::NiTriShape>, 4> coreShapes;   // orig: +0x4C..+0x58
    f32 coreTime = 0;                                 // orig: +0x5C
    u32 flameCount = 0, smokeCount = 0;               // orig: +0x60 / +0x64 (= def +0x28 / +0x50)
    ni::NiPoint3 position;                            // orig: +0x68 (a raw copy of the spawn position)
};

class FireCircleDef;

// orig: vtbl 0x0029BF08 (ctor inline in FireCircleDef::Spawn 0x00068B90 over EffectInstance 0x00065CE0; dtor
// 0x00068C70).
class FireCircle : public GameObject {
public:
    FireCircle(LevelData& level, FireCircleDef& def, const ni::NiPoint3& pos);
    bool Init() override;                             // orig: vf04 0x00069B40
    bool Tick() override;                             // orig: vf08 0x00069F40 (PostUpdate: the base, true)

    ni::NiPoint3 position;                            // orig: +0x40 (a raw copy of the spawn position)
    std::vector<Fire::Particle> particles;            // orig: +0x4C (new[] with the count at [-4]) / +0x50 the count
};

// The level script's fire-circle and sungod-fire natives (their bodies; GameNatives.cpp passes the script's words).
FireCircleDef* FireCircleGetDef(LevelData& level, const char* name);      // orig: native 296 0x000C4E20
// orig: native 297 0x000C4E60 (*created: a new definition was made)
FireCircleDef* FireCircleCreateDef(LevelData& level, const char* name, bool* created = nullptr);
void FireCircleCopyDef(FireCircleDef* dst, const FireCircleDef* src);   // orig: native 298 0x000C2080
// orig: native 299 0x000C20D0 (thiscall S)
void FireCircleCreate(LevelData& level, const char* label, const char* nodeName, FireCircleDef* def);
void FireCircleDestroy(LevelData& level, const char* label);            // orig: native 300 0x000C3270 (thiscall S)
void SetFireToSungodsHead(LevelData& level, const char* name);          // orig: native 301 0x000C4F00 (thiscall S)
void PutOutFireInSungodsHead(LevelData& level);                         // orig: native 302 0x000C50F0 (thiscall S)

}  // namespace fut::game

// Effect definitions and the effect library, decompiled from default.xbe.
//
// Level loading (Session stream kind 3) reads Effects\<level>.nif (LevelData::LoadEffectsStream): the effect meshes go
// under the level's effects root, the textures into its texture array, and EffectList.txt names the effects the
// level uses. Each name goes to the effect library (registry 0x000BF1C0, creator 0x000BB5A0), a 101-case switch that
// builds the effect's definition from the meshes / textures with hard-coded parameters.
//
// Ported: every effect except RainFeathers (a mesh emitter whose quad is a full NiTriShape under a NiNode with six
// properties and an Update pass; no file of the disc names it, so the shipped data never creates it;
// EffectLibrary::Create reports it as unported): impact, splash, shockwave, liquid and ripple definitions, sprite /
// mesh-piece particle emitters, fire effects, summon / warp spells, lasers and the recharge / respawn / charge-up
// effects.
//
// Verified against the original creator under emulation after a real level load (tests/game/EffectDefsTests.cpp,
// oracle tools/difftest/effects_oracle.py).
#pragma once

#include "game/defs/Defs.h"

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fut::game {

class Impact;
class LevelData;

class ImpactDef : public Def {                         // orig: 0x2C bytes, ctor 0x0004EB20
public:
    std::string_view ClassName() const override { return "ImpactDef"; }
    bool IsType(u32 t) const override { return (t & 0x8000) == t; }   // orig: 0x0004EBE0
    const char* TypeName() const override { return "Definition"; }
    ni::NiPointer<ni::NiNode> mesh;                    // orig: +0x18, from the effects root
    // orig: +0x1C. Multiplied by the mesh controllers' frequency by ComputeTimeRange (meaning not identified yet).
    f32 timeScale = 0.0f;
    f32 duration = 0.0f;                               // orig: +0x20, max - min key time after ComputeTimeRange
    u32 unknown24 = 0;                                 // orig: +0x24
    u8 flag = 0;                                       // orig: +0x28
    // orig: 0x0004EA20 with 0x0004EC90 (vtbl+0x1C): the key-time range and frequency of every controller of the
    // mesh, its properties and its descendants; every such controller's cycle type becomes 2 (clamp) when
    // `clampBits` is non-zero, else 0 (loop). The frequency travels back through the argument's own slot, so with no
    // controller timeScale is multiplied by the argument's bits read as a float (1 -> 1.4e-45).
    void ComputeTimeRange(u32 clampBits);
    // orig: vf14 0x0004F710 (Impact.cpp): a new Impact object with the rotation; Init only with a mesh.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
    // orig: 0x0004EE90 (thiscall(owner, NiPoint3* pos, NiPoint3* dir, label), RET 0x10): vf14 with the rotation
    // whose columns are (-(UY x dir), UY, dir).
    Impact* SpawnUp(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiPoint3& dir,
                          const char* label);
    // orig: 0x0004F650 (thiscall(owner, NiPoint3* pos, label), RET 0xC): the camera-facing variant (rand() % 100).
    Impact* SpawnBillboard(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const char* label);
};

class SplashDef : public Def {                         // orig: 0x24 bytes, ctor 0x00073740
public:
    std::string_view ClassName() const override { return "SplashDef"; }
    ni::NiPointer<ni::NiNode> mesh;                    // orig: +0x18
    f32 value1C = 0.0f, value20 = 0.0f;                // orig: +0x1C (the animation speed), +0x20 (the duration)
    // orig: vf14 0x00073CA0 (Splash.cpp): a new Splash object with the rotation; Init only with a mesh.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
    // orig: 0x00073880 (thiscall(owner, NiPoint3* pos, NiPoint3* dir, label), RET 0x10): vf14 with the constant
    // rotation (-UNIT_X, UNIT_Y, UNIT_Z); *dir is overwritten with UNIT_Z (the caller's variable changes).
    GameObject* SpawnDir(LevelData& level, Owner* owner, const ni::NiPoint3& pos, ni::NiPoint3& dir,
                         const char* label);
};

class ShockwaveDef : public Def {                      // orig: 0x24 bytes, ctor 0x0005F1F0
public:
    std::string_view ClassName() const override { return "ShockwaveDef"; }
    bool IsType(u32 t) const override { return (t & 0x2000) == t; }   // orig: 0x0005F2A0
    const char* TypeName() const override { return "Definition"; }
    // orig: vf20 0x0005F560: the shockwave object (vtable 0x0029B744, its set-up 0x0005F3D0 with the collision meshes
    // 0x0005F509). NOT PORTED: no script, effect list or code path spawns the three Shockwave definitions (only the
    // effect library names them; KNOWN_ISSUES #30, U); reported if reached.
    GameObject* SpawnEffect(LevelData& level, const ni::NiPoint3& pos, const ni::NiPoint3& dir) override {
        (void)level, (void)pos, (void)dir;
        std::fprintf(stderr, "NOT PORTED reached: ShockwaveDef::SpawnEffect 0x0005F560\n");
        return nullptr;
    }
    ni::NiPointer<ni::NiNode> mesh;                    // orig: +0x18
    f32 value1C = 0.0f, value20 = 0.0f;                // orig: +0x1C, +0x20
};

class LiquidDef : public Def {                         // orig: 0x1C bytes, ctor 0x0006A580
public:
    ~LiquidDef() override {}                           // orig: 0x0006A5A0 (the vtable, jmp 0x00096370)
    std::string_view ClassName() const override { return "LiquidDef"; }
    bool IsType(u32 t) const override { return (t & 0x20000) == t; }   // orig: 0x0006A5F0
    const char* TypeName() const override { return "Definition"; }
    // orig: +0x18: 0 lava, 1 slime, 2 water, 3 oil, 4 swamp, 5 sand, 6 alien (from the effect names).
    u32 liquid = 0;
};

class RippleDef : public Def {                         // orig: 0x34 bytes, ctor 0x00071710
public:
    std::string_view ClassName() const override { return "RippleDef"; }
    bool IsType(u32 t) const override { return (t & 0x10000) == t; }   // orig: 0x000717C0
    const char* TypeName() const override { return "Definition"; }
    ni::NiPointer<ni::NiObjectNET> texture;            // orig: +0x18, level texture by name
    f32 value1C = 0.0f;                                // orig: +0x1C, the ctor's third argument: the y offset
                                                       //  SpawnRipple uses for FLT_MAX (Character_Ripple 0.1)
    f32 value20 = 0.3f;                                // orig: +0x20 (the ripple's growth rate)
    std::array<f32, 3> value24{1.0f, 1.0f, 1.0f};      // orig: +0x24..+0x2C (the material colour)
    u8 value30 = 0;                                    // orig: +0x30 (the alpha property's blend variant)
    // orig: 0x00071B10 (thiscall(owner, pos*, rot*, alpha, sx, sz, gx, gz, yOffset), RET 0x24; Ripple.cpp): a new
    // Ripple object (half sizes (sx, sz) growing to (gx, gz)) at pos + (0, yOffset, 0), yOffset = FLT_MAX taking
    // value1C; nullptr without a texture.
    GameObject* SpawnRipple(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                            f32 alpha, f32 sx, f32 sz, f32 gx, f32 gz, f32 yOffset);
};

// orig: new(0x34) + RippleDef::RippleDef 0x00071710(name, texture name, y offset), registered in the level's
// definition list by Def::Def (Ripple.cpp).
RippleDef* NewRippleDef(LevelData& level, const char* name, const char* texture, f32 yOffset);

// Particle emitter definition base (orig: vtable 0x0029C2C8, ctor 0x0006D8F0, 0xA4 bytes; the class names
// EmitterDef / SpriteEmitterDef are descriptive - the PS2 build's "ParticlesDef" is a candidate, not confirmed). It
// keeps the configured parameters and a current copy: every setter writes both (the configured value, then the
// current one through a second virtual), Reset copies configured -> current. What each parameter does is not
// identified yet (the effect cases suggest ranges as {min, max}); gravity is a hypothesis from its default 9.82.
class EmitterDef : public Def {
public:
    struct Params {                                    // orig: configured +0x28..+0x60, current +0x64..+0x9C
        std::array<f32, 2> range28{1.0f, 1.0f};        // orig: +0x28 / +0x64
        std::array<f32, 2> range30{1.0f, 1.0f};        // orig: +0x30 / +0x6C
        std::array<f32, 2> range38{0.0f, 0.5f};        // orig: +0x38 / +0x74
        std::array<f32, 2> range40{0.0f, 0.5f};        // orig: +0x40 / +0x7C
        u32 count = 20;                                // orig: +0x48 / +0x84 (integer)
        f32 value4C = 0.5f;                            // orig: +0x4C / +0x88
        f32 gravity = 9.82f;                           // orig: +0x50 / +0x8C (0x411D1EB8)
        u8 flag54 = 0, flag55 = 0;                     // orig: +0x54, +0x55 / +0x90, +0x91
        f32 value58 = 0.0f, value5C = 0.0f;            // orig: +0x58, +0x5C / +0x94, +0x98
        f32 value60 = 1.0f;                            // orig: +0x60 / +0x9C (no setter, Reset copies it)
    };
    Params config, current;
    ni::NiPointer<ni::NiTexturingProperty> texturing;  // orig: +0xA0, set by SetTexture

    std::string_view ClassName() const override { return "EmitterDef"; }
    bool IsType(u32 t) const override { return (t & 0x4000) == t; }   // orig: 0x0006DA20
    const char* TypeName() const override { return "EffectDef"; }
    virtual void Reset() { current = config; }        // orig: 0x0006CA00 (vtbl+0x64)
    // orig: vtbl+0x28..+0x60 (0x0006C8A0..0x0006C9F0), each followed by its current-value twin (vtbl+0x68..+0xA0).
    void SetRange28(f32 a, f32 b) { config.range28 = current.range28 = {a, b}; }
    void SetRange30(f32 a, f32 b) { config.range30 = current.range30 = {a, b}; }
    void SetRange38(f32 a, f32 b) { config.range38 = current.range38 = {a, b}; }
    void SetRange40(f32 a, f32 b) { config.range40 = current.range40 = {a, b}; }
    void SetCount(u32 v) { config.count = current.count = v; }
    void SetValue4C(f32 v) { config.value4C = current.value4C = v; }
    void SetGravity(f32 v) { config.gravity = current.gravity = v; }
    void SetFlag54(u8 v) { config.flag54 = current.flag54 = v; }
    void SetFlag55(u8 v) { config.flag55 = current.flag55 = v; }
    void SetValue58(f32 v) { config.value58 = current.value58 = v; }
    void SetValue5C(f32 v) { config.value5C = current.value5C = v; }
    void SetValue60(f32 v) { config.value60 = current.value60 = v; }   // orig: 0x0006CEA0 (non-virtual)
    // orig: 0x0006F140 (non-virtual). Only when the level has the texture: a new texturing property whose base map is
    // that texture (see MakeTexturing).
    void SetTexture(const char* textureName, const LevelData& level);
};

// Sprite particle emitter (orig: vtable 0x0029C3E0, ctor 0x0006E090, 0x114 bytes): a textured sprite per particle.
class SpriteEmitterDef : public EmitterDef {
public:
    struct SpriteParams {                              // orig: configured +0xA8..+0xDC, current +0xDD..+0x110
        u8 flagA8 = 0;                                 // orig: +0xA8 / +0xDD
        std::array<f32, 2> size{0.1f, 0.1f};           // orig: +0xAC / +0xE0
        std::array<f32, 2> rangeB4{1.0f, 1.0f};        // orig: +0xB4 / +0xE8
        std::array<f32, 4> startColor{1.0f, 1.0f, 1.0f, 1.0f};   // orig: +0xBC / +0xF0 (rgba)
        std::array<f32, 4> endColor{1.0f, 1.0f, 1.0f, 1.0f};     // orig: +0xCC / +0x100
        u8 flagDC = 0;                                 // orig: +0xDC / +0x110
    };
    ni::NiPointer<ni::NiTexturingProperty> spriteTexturing;   // orig: +0xA4
    SpriteParams spriteConfig, spriteCurrent;

    std::string_view ClassName() const override { return "SpriteEmitterDef"; }
    // orig: 0x0006F2A0 (Emitter.cpp).
    GameObject* SpawnEffect(LevelData& level, const ni::NiPoint3& pos, const ni::NiPoint3& dir) override;
    void Reset() override {                            // orig: 0x0006D1C0
        EmitterDef::Reset();
        spriteCurrent = spriteConfig;
    }
    // orig: 0x0006E430 (vtbl+0xA4). Always a new texturing property (+0xA4), its base map texture null when the level
    // has no such texture.
    void SetSpriteTexture(const char* textureName, const LevelData& level);
    // orig: 0x0006CB90. Unlike every other setter this one leaves the current value alone.
    void SetFlagA8(u8 v) { spriteConfig.flagA8 = v; }
    void SetSize(f32 a, f32 b) { spriteConfig.size = spriteCurrent.size = {a, b}; }
    void SetRangeB4(f32 a, f32 b) { spriteConfig.rangeB4 = spriteCurrent.rangeB4 = {a, b}; }
    void SetColor(f32 r, f32 g, f32 b) {               // orig: 0x0006D140 / 0x0006D270: start = end = (r, g, b, 1)
        spriteConfig.startColor = spriteConfig.endColor = {r, g, b, 1.0f};
        spriteCurrent.startColor = spriteCurrent.endColor = {r, g, b, 1.0f};
    }
    void SetFlagDC(u8 v) { spriteConfig.flagDC = spriteCurrent.flagDC = v; }
};

// Mesh-piece particle emitter (orig: vtable 0x0029C5B0, ctor 0x0006F390, 0xD0 bytes): pieces of effects-root meshes
// (splinters, shards, pig parts...).
class MeshEmitterDef : public EmitterDef {
public:
    // orig: NiTArray at +0xA4 (initial size 1, grow 1). AddMesh (vtbl+0xA4, 0x0006F790) appends the lookup result
    // even when the mesh is missing (a null entry).
    std::vector<ni::NiPointer<ni::NiAVObject>> meshes;
    u32 valueBC = 0, currentBC = 0;                    // orig: +0xBC / +0xC0 (vtbl+0xAC, 0x0006CD90)
    std::array<f32, 2> rangeC4{1.0f, 1.0f};            // orig: +0xC4 (vtbl+0xB4, 0x0006CDC0; no configured copy)
    u8 flagCC = 1;                                     // orig: +0xCC (vtbl+0xB8, 0x0006F440; Reset sets 1)

    std::string_view ClassName() const override { return "MeshEmitterDef"; }
    // orig: 0x0006F4C0 (Emitter.cpp).
    GameObject* SpawnEffect(LevelData& level, const ni::NiPoint3& pos, const ni::NiPoint3& dir) override;
    void Reset() override {                            // orig: 0x0006CDE0
        EmitterDef::Reset();
        currentBC = valueBC;
        flagCC = 1;
    }
    void AddMesh(ni::NiAVObject* mesh) { meshes.emplace_back(mesh); }
    void SetValueBC(u32 v) { valueBC = currentBC = v; }
    void SetRangeC4(f32 a, f32 b) { rangeC4 = {a, b}; }
    void SetFlagCC(u8 v) { flagCC = v; }
};

// Fire effect (orig: vtable 0x0029BEAC, ctor 0x00069460, 0x94 bytes): two particle layers sharing a generated quad
// mesh and render properties. That the layers are flames (Fire4.tga) and smoke (Smoke.tga) is a hypothesis from the
// two textures; what each parameter does is not identified yet.
class FireDef : public Def {
public:
    struct Layer {                                     // orig: +0x28 (flames) / +0x50 (smoke), 0x28 bytes
        u32 count = 0;                                 // +0x00
        // +0x04. The constructor leaves [1] unset (the oracle's fresh heap reads 0; the port starts at 0).
        std::array<f32, 2> range04{1.0f, 0.0f};
        std::array<f32, 2> range0C{1.0f, 0.0f};        // +0x0C
        std::array<f32, 2> range14{0.0f, 1.0f};        // +0x14
        f32 value1C = 0.0f, value20 = 0.0f;            // +0x1C, +0x20 (default: the frame step)
        f32 value24 = 3.14159274f;                     // +0x24 (default: pi, 0x40490FDB)
    };
    Layer flames, smoke;
    // orig: built by 0x00067F70: a 4-vertex quad (+-1.5 in x/y, 2 triangles, one UV set) and its properties.
    ni::NiPointer<ni::NiTriShapeData> quad;            // orig: +0x78
    ni::NiPointer<ni::NiAlphaProperty> alpha7C, alpha80;   // orig: +0x7C (flags 0x0D), +0x80 (flags 0xED)
    ni::NiPointer<ni::NiTexturingProperty> flameTexturing, smokeTexturing;   // orig: +0x84 / +0x88, null if missing
    ni::NiPointer<ni::NiVertexColorProperty> vertexColor;  // orig: +0x8C
    ni::NiPointer<ni::NiZBufferProperty> zBuffer;      // orig: +0x90

    std::string_view ClassName() const override { return "FireDef"; }
    bool IsType(u32 t) const override { return (t & 0x40000) == t; }   // orig: 0x00067B70
    const char* TypeName() const override { return "EffectDef"; }
    // orig: the constructor's defaults (flames 0x30 / smoke 0x20 particles, frame-step values) and 0x00067F70.
    void Init(const LevelData& level);
    // orig: vf14 0x00069600: a new Fire (Fire.cpp; FsCreateFire 295); the rotation is not read.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
};

class FireCircleDef : public FireDef {                 // orig: vtable 0x0029BED4, ctor 0x00069AA0, 0xA0 bytes
public:
    std::string_view ClassName() const override { return "FireCircleDef"; }
    bool IsType(u32 t) const override { return (t & 0x80000) == t; }   // orig: 0x00067C60
    // orig: its own vf14 0x00068B90: a new FireCircle (Fire.cpp; FsFireCircleCreate 299); rotation and label not read.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
    f32 value94 = 3.0f, value98 = 1.0f;                // orig: +0x94 the radius, +0x98 the height (class 16 3-6)
    u32 value9C = 16;                                  // orig: +0x9C the density: flames per ring (class 16 7 / 8)
    void InitCircle(const LevelData& level);           // orig: FireDef ctor + the 0x00069AA0 defaults
};

// Spell effect (orig: vtable 0x0029C96C, ctor 0x000731B0, 0x38 bytes): two (emitter, value) pairs; both cases use
// their sprite emitter twice, with 1 and 2.
class SpellDef : public Def {
public:
    std::string_view ClassName() const override { return "SpellDef"; }
    bool IsType(u32 t) const override { return (t & 0x20) == t; }   // orig: 0x00065DB0
    const char* TypeName() const override { return "EffectDef"; }
    // orig: vf20 0x00073630 (thiscall(a, b), RET 8; Spell.cpp): a new SpellEffect at a and b (the base's dir is the
    // second point here), owned by the level.
    GameObject* SpawnEffect(LevelData& level, const ni::NiPoint3& a, const ni::NiPoint3& b) override;
    EmitterDef* emitter28 = nullptr;                   // orig: +0x28 (not reference counted)
    EmitterDef* emitter2C = nullptr;                   // orig: +0x2C
    u32 value30 = 0, value34 = 0;                      // orig: +0x30, +0x34
};

// Projectile definition base (orig: ctor 0x0007BCB0, 0x68 bytes; the lasers and the NPCs' weapons). What the
// parameters do is not identified yet; they are named by offset.
class ProjectileDef : public Def {
public:
    struct Args {                                      // the constructor's arguments after the name and the model
        f32 value1C = 0.0f;
        u8 flag2C = 0;
        f32 value20 = 0.0f, value28 = 0.0f, value24 = 0.0f;
        f32 value4C = 0.0f;                            // also scaled into +0x3C (x 0.3f) and +0x44 (x 0.6f)
        f32 value38 = 0.0f, value40 = 0.0f, value48 = 0.0f;
    };
    std::string_view ClassName() const override { return "ProjectileDef"; }
    bool IsType(u32 t) const override { return (t & 0x10) == t; }   // orig: 0x00079A30
    const char* TypeName() const override { return "Definition"; }
    void Construct(ni::NiNode* projectileModel, const Args& a);   // orig: 0x0007BCB0
    // orig: vf14 0x000798A0 -> vf20 0x0007BC00 (arg4 = 0): a new Projectile (Laser.cpp); Init only with a model.
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
    // orig: vf20 0x0007BC00 (thiscall(owner, pos, rot, arg4, label), RET 0x14): vf14 passes arg4 = 0.
    virtual GameObject* Spawn20(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                                u32 arg4, const char* label);

    ni::NiPointer<ni::NiNode> model;                   // orig: +0x18 (an effects-root mesh)
    f32 value1C = 0.0f;                                // orig: +0x1C
    f32 value20 = 0.0f, value24 = 0.0f;                // orig: +0x20, +0x24
    f32 value28 = 0.0f;                                // orig: +0x28
    u8 flag2C = 0;                                     // orig: +0x2C
    SpriteEmitterDef* emitter30 = nullptr;             // orig: +0x30 (set by the NPC definitions; owned by the list)
    SpriteEmitterDef* emitter34 = nullptr;             // orig: +0x34
    f32 value38 = 0.0f, value3C = 0.0f, value40 = 0.0f, value44 = 0.0f, value48 = 0.0f, value4C = 0.0f;   // +0x38..
    u8 flag50 = 0;                                     // orig: +0x50
    f32 value54 = -1.0f;                               // orig: +0x54
    u32 sound58 = 0, sound5C = 0, sound60 = 0;         // orig: +0x58..+0x60 (sound handles set by the NPC definitions)
    u32 value64 = 1;                                   // orig: +0x64
};

// Laser projectile (orig: vtable 0x0029D050, ctor 0x00079BF0, 0xA8 bytes): beam / glow texturing and a spark emitter.
class LaserDef : public ProjectileDef {
public:
    std::string_view ClassName() const override { return "LaserDef"; }
    // orig: ctor 0x00079BF0. Also creates the spark emitter "Something" (appended to `defs` after the laser).
    void Construct(const LevelData& level, std::vector<std::unique_ptr<Def>>& defs);
    // orig: vtbl+0x24, 0x00079A60 (beam, glow): a texturing property per texture (apply mode 2, filter 1), none for
    // a missing texture.
    void SetTextures(ni::NiObjectNET* beam, ni::NiObjectNET* glow);
    // orig: ProjectileDef vf14 0x000798A0 -> LaserDef vf20 0x000798C0 (arg4 = 0): a new Laser (Laser.cpp).
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
    GameObject* Spawn20(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                        u32 arg4, const char* label) override;   // orig: vf20 0x000798C0

    std::array<f32, 6> value68{1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 8.0f};   // orig: +0x68..+0x7C
    f32 value80 = 100.0f;                              // orig: +0x80
    u8 flag84 = 0, flag85 = 0, flag86 = 0;             // orig: +0x84..+0x86
    u32 value88 = 0;                                   // orig: +0x88
    Def* ripple8C = nullptr;                           // orig: +0x8C (a RippleDef: the pistol weapon's "RippleLaser")
    f32 value90 = 0.35f, value94 = 3.0f, value98 = 3000.0f;   // orig: +0x90, +0x94, +0x98
    ni::NiPointer<ni::NiTexturingProperty> glowTexturing;   // orig: +0x9C ("las_glow.tga" by default)
    ni::NiPointer<ni::NiTexturingProperty> beamTexturing;   // orig: +0xA0 ("las_beam.tga" by default)
    SpriteEmitterDef* sparks = nullptr;                // orig: +0xA4 (owned by the definition list)
};

// orig: RailDef (ctor 0x0007BE80 = LaserDef 0x00079BF0 + vtable 0x0029D13C, 0xA8 bytes, no new fields; type name
// "RailDef"): the railgun's laser definitions (natives_batch_b_port.md 4). Its spawn (vf20 0x0007CDB0) creates a
// RailLaser (new(0x170), Laser 0x00077D40 + vtable 0x0029D170; Laser.h).
class RailDef : public LaserDef {
public:
    std::string_view ClassName() const override { return "RailDef"; }   // orig: vf1C 0x0007BEB0
    // orig: ProjectileDef vf14 0x000798A0 -> RailDef vf20 0x0007CDB0 (arg4 = 0): a new RailLaser (Laser.cpp).
    GameObject* Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                      const char* label) override;
};

// Muzzle flare (orig: vtable 0x0029C0A4, ctor 0x0006A000, 0x34 bytes; PS2 "FlareDef" is a candidate name).
class FlareDef : public Def {
public:
    std::string_view ClassName() const override { return "FlareDef"; }
    bool IsType(u32 t) const override { return (t & 0x20) == t; }   // orig: 0x00065DB0
    const char* TypeName() const override { return "EffectDef"; }
    f32 value28 = 1.0f, value2C = 1.0f;                // orig: +0x28, current copy +0x2C
    ni::NiPointer<ni::NiAVObject> mesh;                // orig: +0x30 (an effects-root mesh)
    void SetValue(f32 v) { value28 = value2C = v; }    // orig: vtbl+0x24 (0x00069FA0, then vtbl+0x2C)
    // orig: vf20 0x0006A480 (Emitter.cpp): a new Flare at `pos` along `dir`, then vf28 0x00069FB0 (+0x2C = +0x28).
    GameObject* SpawnEffect(LevelData& level, const ni::NiPoint3& pos, const ni::NiPoint3& dir) override;
};

// Scorch mark (orig: vtable 0x0029C834, ctor 0x000721D0, 0x24 bytes).
class ScorchMarkDef : public Def {
public:
    std::string_view ClassName() const override { return "ScorchMarkDef"; }
    std::array<f32, 2> sizes{0.3f, 0.3f};              // orig: +0x18, +0x1C (0x3E99999A): the size per mark type
    f32 Size(u32 type) const { return sizes[type]; }   // orig: def+0x18 + 4 * type
    ni::NiPointer<ni::NiObjectNET> texture;            // orig: +0x20, a level texture by name
};

// Scorch-mark factory (orig: vtable 0x0029C8C4, ctor 0x00072C70, 0x2C bytes, name "SMarkFactory"): created once per
// level by the kind-3 set-up (0x000C59E0) with its "Scorch_Mark" mark ("Scorch.TGA").
class SMarkFactoryDef : public Def {
public:
    std::string_view ClassName() const override { return "SMarkFactoryDef"; }
    bool IsType(u32 t) const override { return (t & 0x20) == t; }   // orig: 0x00065DB0
    const char* TypeName() const override { return "EffectDef"; }
    ScorchMarkDef* mark = nullptr;                     // orig: +0x28 (also in the definition list)
};

// Particle emitter setter calls as data (the effect cases and the NPC definitions configure emitters this way).
enum class Op {
    Range28, Range28Both, Range30, Range38, Range38Max, Range40, Range40Max, Count, Value4C, Gravity, Flag54, Flag55,
    Value58, Value5C, Value60, Texture,
    FlagA8, Size, SizeBoth, RangeB4, Color, FlagDC,    // sprite
    Mesh, ValueBC, RangeC4, FlagCC                     // mesh
};
struct EmitterCall {
    Op op;
    std::array<u32, 3> args;                           // float bits, or integers (Count / flags / ValueBC)
    const char* text = nullptr;                        // Texture: texture name; Mesh: effects-root object name
};
// One setter call (orig: the emitter's vtable slot for that setter; see the Op comments in EffectDefs.cpp).
void ApplyEmitterCall(EmitterDef& d, const EmitterCall& c, const LevelData& level);

// orig: the texturing property built by 0x0006E430 / 0x0006F140 / 0x00072740: NiTexturingProperty ctor 0x00109DC0 (7
// map slots, apply mode 2), SetBaseTexture 0x0001D610 (Map ctor 0x0001D570: clamp 3, PS2 K 0xFFB5), then filter mode 1.
ni::NiPointer<ni::NiTexturingProperty> MakeTexturing(ni::NiObjectNET* texture);

// Pickup recharge effect (orig: vtable 0x0029C66C, ctor 0x00070710, 0x30 bytes): six level textures.
class RechargeEffectDef : public Def {
public:
    std::string_view ClassName() const override { return "RechargeEffectDef"; }
    // orig: +0x18..+0x2C: Health_25 / Health_50 / Health_100 / Ammo_25 / Ammo_50 / Ammo_100 .tga (null when missing).
    std::array<ni::NiPointer<ni::NiObjectNET>, 6> textures;
    // orig: 0x000708D0 (thiscall(owner, label, n), RET 0xC): a RechargeEffect (RespawnEffect.h) 12 above the player
    // with texture n, or nullptr without that texture.
    GameObject* SpawnAt(LevelData& level, Owner* owner, const char* label, s32 n);
};

class RespawnEffectDef : public Def {                  // orig: vtable 0x0029C734, ctor 0x000709E0, 0x1C bytes
public:
    std::string_view ClassName() const override { return "RespawnEffectDef"; }
    ni::NiPointer<ni::NiObjectNET> texture;            // orig: +0x18
    // orig: 0x000715D0 (thiscall(owner, NiPoint3* pos, label), RET 0xC; FsAtRespawn): a RespawnEffect at `pos`
    // (RespawnEffect.cpp), or nullptr without the texture.
    GameObject* SpawnAt(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const char* label);
};

// orig: vtable 0x0029BC28, ctor 0x00065F50, 0x30 bytes (the ctor leaves +0x20..+0x2F unset).
class ChargeUpEffectDef : public Def {
public:
    std::string_view ClassName() const override { return "ChargeUpEffectDef"; }
    ni::NiPointer<ni::NiObjectNET> texture, texture2;  // orig: +0x18, +0x1C
    ni::NiPoint3 pos;                                  // orig: +0x20 (SpawnAt's position)
    u8 slot = 0;                                       // orig: +0x2C (SpawnAt's n - 1: the HUD charge slot)
    // orig: 0x00066290 (thiscall(owner, NiPoint3* pos, n, label), RET 0x10; Charge::OnAttach): a ChargeUpEffect
    // (RespawnEffect.h) flying from `pos` on the screen to charge slot n - 1, or nullptr without both textures.
    GameObject* SpawnAt(LevelData& level, Owner* owner, const ni::NiPoint3& pos, s32 n, const char* label);
};

// The 101 registered effect names, by index (orig: 0x000BF1C0).
extern const std::array<const char*, 101> kEffectNames;

class EffectLibrary {
public:
    enum class Result { Created, NotCreated, Unknown, NotPorted };
    // orig: 0x000BB5A0. Index of `name` (strcmp), then that index's case. The created definitions are appended to
    // `out` in creation order (the original's constructors append them to the level's definition list; the spell
    // cases create an emitter and a spell). NotCreated: the case's mesh or texture is missing (nothing is created);
    // NotPorted: a class not ported yet.
    static Result Create(const char* name, LevelData& level, std::vector<std::unique_ptr<Def>>& out);
};

}  // namespace fut::game

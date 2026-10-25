// The Fire effect object; see Fire.h and docs/analysis/natives_batch_a_port.md 3 (checked against the assembly of
// 0x00069600, 0x00068AD0, 0x00069770, 0x00068CF0, 0x00067D50, 0x00069100 / 0x00069280, 0x00069EB0, 0x000684F0 and
// 0x000689E0); the fire circle and the sungod's fire (0x00068B90, 0x00069B40, 0x00069F40, natives 296-302: 0x000C4E20,
// 0x000C4E60, 0x000C2080, 0x000C20D0, 0x000C3270, 0x000C4F00, 0x000C50F0).
#include "game/object/Fire.h"

#include "core/CrtRand.h"
#include "engine/ni/NiMath.h"
#include "game/GameTime.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/level/TriggerManager.h"
#include "game/object/Character.h"
#include "game/object/NodeUpdate.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

constexpr u32 kRandUnit = 0x38000100u;               // 1/32767 (0x002955E8)
constexpr u32 kRandTwo = 0x38800100u;                // 2/32767 (0x002955EC)
constexpr u32 kRandNegUnit = 0xB8000100u;            // -1/32767 (0x0029BF58)
f32 Bits(u32 b) { return std::bit_cast<f32>(b); }

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: new(0xF0) NiBillboardNode inline in 0x00068CF0 / 0x000684F0 (NiNode ctor 0x00105C10, vtbl 0x0029BF60,
// +0xE0 = 0, +0xE4 = 1): mode 1 (flags & 0xE7FF | 0x400).
ni::NiPointer<ni::NiBillboardNode> NewBillboard() {
    ni::NiPointer<ni::NiBillboardNode> b(new ni::NiBillboardNode);
    b->flags = static_cast<u16>((b->flags & 0xE7FF) | 0x0400);
    return b;
}

ni::NiPointer<ni::NiTriShape> NewQuad(const FireDef& d) {   // orig: new(0xE0) + 0x00116570(def->quad, 0x000679B0)
    ni::NiPointer<ni::NiTriShape> s(new ni::NiTriShape);
    s->modelData = d.quad.Get();
    return s;
}

// orig: new(0x5C) NiMaterialProperty 0x00114B10, then ambient / diffuse / specular (1, 1, 1) (0x002D7BD0), shininess 0,
// emissive, alpha: one revision bump per store.
ni::NiPointer<ni::NiMaterialProperty> NewMaterial(const ni::NiColor& emissive, f32 alpha) {
    ni::NiPointer<ni::NiMaterialProperty> m(new ni::NiMaterialProperty);
    m->ambient = m->diffuse = m->specular = {1.0f, 1.0f, 1.0f};
    m->shininess = 0;
    m->emissive = emissive;
    m->alpha = alpha;
    m->revision += 6;
    return m;
}

}  // namespace

// ---- FireDef::Spawn ---------------------------------------------------------------------------------------------------
GameObject* FireDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                           const char* label) {   // orig: vf14 0x00069600 (RET 0x10)
    (void)rot;                                        // (not read)
    auto* o = new Fire(level, *this, pos);            // new(0x74)
    if (!o->Init()) {                                 // vf04
        delete o;                                     // vf00(1)
        return nullptr;
    }
    o->SetOwner(owner ? owner : &level);              // vf10 (S without an owner)
    o->SetName(label);                                // 0x000968C0 (FsCreateFire passes 0: nothing)
    return o;
}

GameObject* FireCircleDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                                 const char* label) {   // orig: vf14 0x00068B90 (RET 0x10)
    (void)rot, (void)label;                           // (neither is read: FsFireCircleCreate names the object itself)
    auto* o = new FireCircle(level, *this, pos);      // new(0x54)
    if (!o->Init()) {                                 // vf04
        delete o;                                     // vf00(1)
        return nullptr;
    }
    o->SetOwner(owner ? owner : &level);              // vf10 (S without an owner)
    return o;
}

// ---- Fire -------------------------------------------------------------------------------------------------------------
Fire::~Fire() {                                       // orig: 0x000696C0
    flames.clear();                                   // +0x40: delete[] (0x00166E30 with the particle dtor 0x00067CB0)
    smoke.clear();                                    // +0x44: likewise
    // (then the core +0x48 0x00067B90: the members; the EffectInstance dtor 0x00065D40)
}

Fire::Fire(LevelData& lvl, FireDef& d, const NiPoint3& pos)   // orig: 0x00068AD0 (EffectInstance 0x00065CE0)
    : GameObject(lvl, d), flameCount(d.flames.count), smokeCount(d.smoke.count), position(pos) {}

bool Fire::Init() {                                   // orig: vf04 0x00069770
    auto& d = static_cast<FireDef&>(*def);
    flames.assign(flameCount, Particle{});            // orig: new[] (0x00166E90 with the ctor 0x00067C90)
    NiPoint3 off{};                                   // (x and z stay 0)
    for (u32 i = 0; i < flameCount; ++i) {
        Particle& p = flames[i];
        off.y = F(double(crt::Rand()) * Bits(kRandTwo));   // rand 0x00069825
        const double u = double(crt::Rand()) * Bits(kRandUnit);   // rand 0x0006983C (x87)
        p.type = 0;
        const double x = (1.0 - u) * d.flames.range04[1] + u * d.flames.range04[0];   // FSUB ST1; FMUL; FXCH; FMUL; FADDP
        p.life = F(x);                                // FST (x stays unrounded)
        p.invLife = F(1.0 / x);                       // FLD 1.0; FDIV ST1
        p.spin = (i % 2) ? -d.flames.value24 : d.flames.value24;   // FCHS for odd i
        p.Init(d, off, d.flameTexturing.Get(), 5.0f); // orig: 0x00068CF0 (the texturing AddRef'd by 0x000679F0)
        node->AttachChild(p.billboard.Get(), true);   // vfA0
    }
    smoke.assign(smokeCount, Particle{});
    off = {0.0f, 2.5f, 0.0f};                         // 0x40200000 (set once)
    for (u32 i = 0; i < smokeCount; ++i) {
        Particle& p = smoke[i];
        const double u = double(crt::Rand()) * Bits(kRandUnit);   // rand 0x00069985
        p.type = 1;
        const double x = (1.0 - u) * d.smoke.range04[1] + u * d.smoke.range04[0];
        p.life = F(x);
        p.invLife = F(1.0 / x);
        p.spin = (i % 2) ? -d.smoke.value24 : d.smoke.value24;
        p.Init(d, off, d.smokeTexturing.Get(), 5.0f); // (0x00067A10)
        node->AttachChild(p.billboard.Get(), true);
    }
    // orig: the core 0x000684F0
    coreNode = NewBillboard();
    AttachProperty(*coreNode, NewMaterial({0.5f, 0.5f, 0.5f}, Bits(0x3F4CCCCDu)).Get());   // AddHead 0x00013A50
    for (size_t k = 0; k < 4; ++k) {
        coreShapes[k] = NewQuad(d);
        coreShapes[k]->local.rotate = {};             // 0x002D7820
        AttachProperty(*coreShapes[k], d.alpha7C.Get());          // 0x000679D0
        AttachProperty(*coreShapes[k], d.flameTexturing.Get());   // 0x000679F0
        AttachProperty(*coreShapes[k], d.vertexColor.Get());      // 0x00067A30
        AttachProperty(*coreShapes[k], d.zBuffer.Get());          // 0x00067A50
        coreNode->AttachChild(coreShapes[k].Get(), true);
    }
    // (after the loop: scale F(|1.0|) and the translations 0x3E4CCCCD / 0xBE4CCCCD, 0x3ECCCCCD / 0x3F800000)
    static constexpr NiPoint3 kCore[4] = {{0.2f, 0.4f, 0.0f}, {-0.2f, 0.4f, 0.0f}, {0.2f, 1.0f, 0.0f}, {-0.2f, 1.0f, 0.0f}};
    for (size_t k = 0; k < 4; ++k) {
        coreShapes[k]->local.translate = kCore[k];
        coreShapes[k]->local.scale = 1.0f;
    }
    UpdateNodeAt(coreNode.Get(), 0.0f);               // orig: NiAVObject::Update(0, 1) 0x001018D0
    coreNode->UpdateProperties();                     // 0x000689C6
    node->AttachChild(coreNode.Get(), true);
    node->local.translate = position;                 // (raw: no SetPosition / UpdateRoom)
    UpdateNodeAt(node.Get(), 0.0f);                   // orig: 0x001018D0
    node->UpdateProperties();                         // 0x00069A6F
    node->UpdateEffects();                            // 0x00069A77
    return true;
}

void Fire::Particle::Init(FireDef& d, const NiPoint3& off, ni::NiTexturingProperty* tex, f32 delay) {
    def = &d;                                         // orig: 0x00068CF0 (thiscall, RET 0x10)
    offset = off;
    shape = NewQuad(d);
    shape->local.scale = 1.0f;                        // F(|1.0|) (0x00293CC0)
    shape->local.rotate = {};                         // 0x002D7820
    shape->local.translate = {};                      // 0x002F915C
    AttachProperty(*shape, d.alpha7C.Get());          // AddHead 0x00013A50 each (the list ends reversed)
    AttachProperty(*shape, tex);
    AttachProperty(*shape, d.vertexColor.Get());
    AttachProperty(*shape, d.zBuffer.Get());
    material = NewMaterial({1.0f, 1.0f, 1.0f}, 1.0f);
    AttachProperty(*shape, material.Get());
    billboard = NewBillboard();
    age = F(double(crt::Rand()) * delay * Bits(kRandNegUnit));   // rand 0x0006908A: FILD; FMUL delay; FMUL [0x0029BF58]
    Respawn();                                        // 0x00067D50
    billboard->AttachChild(shape.Get(), true);
    billboard->flags = static_cast<u16>(billboard->flags | ni::kNiAVHidden);
}

void Fire::Particle::Respawn() {                      // orig: 0x00067D50
    const s32 q1 = crt::Rand();                       // 0x00067D5D
    const f32 a = F(double(crt::Rand()) * Bits(q1 % 2 ? kRandNegUnit : kRandUnit));   // 0x00067D85 / 0x00067D70
    const s32 q2 = crt::Rand();                       // 0x00067D9C
    const double b = double(crt::Rand()) * Bits(q2 % 2 ? kRandNegUnit : kRandUnit);   // 0x00067DC4 / 0x00067DAF (x87)
    const FireDef::Layer& L = type == 1 ? def->smoke : def->flames;   // +0x50 / +0x28
    vel = {F(double(a) * L.value20), L.value1C, F(b * L.value20)};
    billboard->local.scale = 1.0f;                    // F(|1.0|)
    billboard->local.rotate = {};
    billboard->local.translate = offset;
}

void Fire::Particle::Update() {                       // orig: 0x00069280 (flame) / 0x00069100 (smoke)
    const double dt = g_frameStep;
    if (age >= 0.0f) {                                // FCOMP 0; TEST AH,1; JNE done (NaN skips)
        if (double(age) - dt < 0.0)                   // FSUB dt; FCOMP 0; TEST AH,5; JP (the first visible tick)
            billboard->flags = static_cast<u16>(billboard->flags & ~ni::kNiAVHidden);
        if (age >= life) {                            // FCOMP +0x18; TEST AH,1; JNE update (< and NaN update)
            age = 0.0f;
            Respawn();
            age = F(dt + age);
            return;
        }
        shape->local.rotate = ni::MakeZRotation(F(double(spin) * age));   // 0x001030F0
        NiPoint3& t = billboard->local.translate;
        t = {F(double(t.x) + vel.x), F(double(t.y) + vel.y), F(double(t.z) + vel.z)};
        vel.x = F(double(vel.x) - dt * dt * t.x);     // FLD dt; FMUL dt; FMUL t.x; FSUBR vel.x
        vel.z = F(double(vel.z) - dt * dt * t.z);
        const FireDef::Layer& L = type == 1 ? def->smoke : def->flames;
        const double k = double(invLife) * age;
        const double m = type == 1 ? 1.0 - k : double(F(1.0 - k));   // the flame rounds 1 - k to a float
        billboard->local.scale = F(std::fabs(m * L.range0C[0] + k * L.range0C[1]));
        material->alpha = F(m * L.range14[0] + k * L.range14[1]);   // 0x00067EA0: +0x50, +0x54++
        ++material->revision;
        if (type == 0) {                              // 0x00067EF0 (flames only): +0x40..+0x48, +0x54++
            ni::NiColor c{1.0f, 1.0f, F(m)};
            if (F(m) < 0.75f) c.g = F(m * Bits(0x3FAAAAABu));   // [0x00293A94]; TEST AH,5; JP; [0x0029C090]
            material->emissive = c;
            ++material->revision;
        }
    }
    age = F(dt + age);
}

bool Fire::Tick() {                                   // orig: vf08 0x00069EB0
    for (Particle& p : flames) p.Update();            // (the type selects 0x00069100 / 0x00069280)
    for (Particle& p : smoke) p.Update();
    // orig: the core tick 0x000689E0: s = F(pi + pi) once (static 0x002DEA7C, guard bit 0x002DEA80)
    static const f32 s = F(double(Bits(0x40490FDBu)) + Bits(0x40490FDBu));   // [0x0029BEA0]; FADD ST0, ST0
    const ni::NiMatrix3 r1 = ni::MakeZRotation(F(-(double(s) * coreTime)));
    const ni::NiMatrix3 r2 = ni::MakeZRotation(F(double(s) * coreTime));
    coreShapes[0]->local.rotate = r1;
    coreShapes[1]->local.rotate = r2;
    coreShapes[2]->local.rotate = r1;
    coreShapes[3]->local.rotate = r2;
    coreTime = F(double(g_frameStep) + coreTime);
    UpdateNodeAt(node.Get(), 0.0f);                   // orig: NiAVObject::Update(0, 1) 0x001018D0
    time = F(double(g_frameStep) + time);             // +0x24
    return true;
}

// ---- FireCircle (docs/analysis/natives_batch_a_port.md 8) -------------------------------------------------------------
FireCircle::FireCircle(LevelData& lvl, FireCircleDef& d, const NiPoint3& pos)   // orig: inline in 0x00068B90
    : GameObject(lvl, d), position(pos) {}            // (+0x4C / +0x50 = 0)

bool FireCircle::Init() {                             // orig: vf04 0x00069B40
    auto& d = static_cast<FireCircleDef&>(*def);
    particles.clear();                                // (an earlier array: delete[] 0x00166E30)
    const s32 density = static_cast<s32>(d.value9C);  // +0x9C, the flames per ring
    const double pi = Bits(0x40490FDBu);              // [0x0029BEA0]
    // FLD +0x98; FILD +0x9C; FLD pi; FMUL [0x0029C094] (6.0); FDIVP: density / (6 pi) above the height
    const double perUnit = double(density) / (pi * 6.0);
    // FLD pi; FMUL radius; FADD ST0, ST0; FMUL ST1; _ftol (truncates)
    const s32 rings = static_cast<s32>((pi * d.value94 + pi * d.value94) * perUnit);
    const double h = double(d.value98) * 0.75;        // FSTP ST0; FMUL [0x00293A94] (the height on the stack)
    const f32 lifeA = F(h);                           // FST [esp+0x3C]
    const f32 lifeB = F(h * Bits(0x3FD55555u));       // FMUL [0x0029C098] (1.6666666); FSTP [esp+0x40]
    const s32 total = rings * density;
    particles.assign(static_cast<size_t>(total > 0 ? total : 0), Fire::Particle{});   // +0x50; new[] (0x00067C90)
    if (rings > 0) {                                  // (JLE: no ring; the angle step's division is not used)
        const f32 step = F((pi + pi) / double(rings)); // FLD pi; FADD ST0, ST0; FIDIV rings; FSTP [esp+0x2C]
        const f32 one = 1.0f;                         // F(|1.0|) (0x00293CC0)
        s32 k = 0;                                    // [esp+0x10]: the particle index
        for (s32 i = 0; i < rings; ++i) {
            ni::NiPointer<ni::NiNode> ring(new ni::NiNode);   // new(0xE0), NiNode 0x00105C10
            ring->local.scale = one;
            const double a = double(i) * step;        // FILD i; FMUL step
            ring->local.translate = {F(std::sin(a) * d.value94), 0.0f, F(std::cos(a) * d.value94)};   // FSIN / FCOS
            ring->local.rotate = {};                  // 0x002D7820
            for (s32 j = 0; j < density; ++j) {
                Fire::Particle& p = particles[static_cast<size_t>(k)];
                const double u = double(crt::Rand()) * Bits(kRandUnit);   // rand 0x00069D13 (x87)
                p.type = 0;
                const double x = (1.0 - u) * lifeB + u * lifeA;   // FLD 1.0; FSUB ST1; FMUL B; FXCH; FMUL A; FADDP
                p.life = F(x);                        // FST +0x18 (x stays unrounded)
                p.invLife = F(1.0 / x);               // FLD 1.0; FDIV ST1
                p.spin = (k % 2) ? -d.flames.value1C : d.flames.value1C;   // +0x44 (FCHS for an odd index)
                p.Init(d, {}, d.flameTexturing.Get(), 5.0f);   // 0x00068CF0 (offset 0x002F915C; 0x000679F0)
                ring->AttachChild(p.billboard.Get(), true);    // vfA0
                if (++k == total) break;              // (the last particle)
            }
            node->AttachChild(ring.Get(), true);      // vfA0
        }
    }
    node->local.scale = 1.0f;                         // F(|1.0|) (0x00293CC0)
    node->local.rotate = {};                          // 0x002D7820
    node->local.translate = position;                 // +0x40 (raw: no SetPosition / UpdateRoom)
    UpdateNodeAt(node.Get(), 0.0f);                   // orig: NiAVObject::Update(0, 1) 0x001018D0
    node->UpdateProperties();                         // 0x00101EA0
    node->UpdateEffects();                            // 0x00102190
    return true;
}

bool FireCircle::Tick() {                             // orig: vf08 0x00069F40
    for (Fire::Particle& p : particles) p.Update();   // (the type selects 0x00069100 / 0x00069280)
    UpdateNodeAt(node.Get(), 0.0f);                   // orig: NiAVObject::Update(0, 1) 0x001018D0
    time = F(double(g_frameStep) + time);             // +0x24
    return true;
}

// ---- the natives' bodies --------------------------------------------------------------------------------------------
FireCircleDef* FireCircleGetDef(LevelData& level, const char* name) {   // orig: 0x000C4E20 (RET 4)
    Def* d = name && level.defs ? level.defs->Find(name) : nullptr;     // Session::FindDef 0x000C3F10
    if (!d || !d->IsType(0x80000)) return nullptr;    // vf0C
    auto* c = dynamic_cast<FireCircleDef*>(d);
    if (!c) NotPorted("FsFireCircleGetDef: a definition of type 0x80000 that is not a FireCircleDef");
    return c;
}

FireCircleDef* FireCircleCreateDef(LevelData& level, const char* name, bool* created) {   // orig: 0x000C4E60
    if (created) *created = false;
    if (!name || !level.defs) return nullptr;         // (a NULL name faults in FindDef in the original)
    if (Def* d = level.defs->Find(name)) {            // 0x000C3F10
        if (!d->IsType(0x80000)) return nullptr;      // NEG AL; SBB; AND: the found one or 0
        auto* c = dynamic_cast<FireCircleDef*>(d);
        if (!c) NotPorted("FsFireCircleCreateDef: a definition of type 0x80000 that is not a FireCircleDef");
        return c;
    }
    auto d = std::make_unique<FireCircleDef>();       // new(0xA0), FireCircleDef::FireCircleDef(name) 0x00069AA0
    d->name = name;
    d->InitCircle(level);
    if (created) *created = true;
    return static_cast<FireCircleDef*>(level.defs->Add(std::move(d)));   // (the Def ctor 0x000962E0 appends it)
}

void FireCircleCopyDef(FireCircleDef* dst, const FireCircleDef* src) {   // orig: 0x000C2080 (RET 8)
    if (!dst || !src) return;
    dst->value94 = src->value94;                      // 0x00067960 -> 0x00067950 (FLD; FSTP)
    dst->value98 = src->value98;                      // 0x00067980 -> 0x00067970
    dst->value9C = src->value9C;                      // 0x000679A0 -> 0x00067990
}

void FireCircleCreate(LevelData& level, const char* label, const char* nodeName, FireCircleDef* def) {   // 0x000C20D0
    if (!def || !level.triggers) return;
    TriggerRec* r = level.triggers->FindOrCreate(nodeName);   // S+0xC0, 0x000DB390 (a NULL name returns 0)
    if (!r) return;
    const NiPoint3 pos = level.triggers->Position(*r);   // 0x000DA7E0
    const ni::NiMatrix3 rot{};                        // 12 dwords of 0x002D7820
    GameObject* o = def->Spawn(level, &level, pos, rot, nullptr);   // vf14(S, pos, rot, 0)
    if (o) o->SetName(label);                         // 0x000968C0 (no NULL test of the object in the original)
}

void FireCircleDestroy(LevelData& level, const char* label) {   // orig: 0x000C3270 (thiscall S, RET 4)
    if (!label) return;
    for (GameObject* o = level.NextObject(nullptr); o; o = level.NextObject(o))   // 0x00097670 on S
        if (o->label && std::strcmp(label, o->label->c_str()) == 0 && o->def->IsType(0x80000)) {
            delete o;                                 // vf00(1)
            return;
        }
}

namespace {

// orig: the stage walk of 0x000C4F00 / 0x000C50F0 (S+0x58, next +0x10; each stage's objects at +4): the first object
// whose label equals `name` (strcmp; 0x000C50F0's REPE CMPSB of "mr_sungod" with its terminator is the same test) and
// whose definition is a character's (vf0C(1)).
Character* FindStageCharacter(LevelData& level, const char* name) {
    for (const auto& st : level.stages)
        for (GameObject* o = st->NextObject(nullptr); o; o = st->NextObject(o))
            if (o->label && std::strcmp(name, o->label->c_str()) == 0 && o->def->IsType(1))
                return static_cast<Character*>(o);
    return nullptr;
}

}  // namespace

void SetFireToSungodsHead(LevelData& level, const char* name) {   // orig: 0x000C4F00 (thiscall S, RET 4)
    if (!name) return;
    Character* c = FindStageCharacter(level, name);
    if (!c) return;
    // vf30 -> vf54("burn") (0x002A357C), then NiObject::DynamicCast 0x00011470 to NiNode (0x002F9230); AddRef'd
    ni::NiPointer<ni::NiNode> burn(dynamic_cast<ni::NiNode*>(GetObjectByName(c->GetNode(), "burn")));
    if (!burn) return;
    Def* d = level.defs ? level.defs->Find("effect_SungodHairFire") : nullptr;   // 0x000C3F10 (0x002A2C40)
    if (!d) {                                         // (no NULL test in the original)
        NotPorted("FsSetFireToSungodsHead without effect_SungodHairFire");
        return;
    }
    // vf14(the character's inventory +0x38, NiPoint3::ZERO 0x002F915C, identity, "sungod_hair_on_fire" 0x002A3568)
    GameObject* o = d->Spawn(level, &c->inventory, NiPoint3{}, ni::NiMatrix3{}, "sungod_hair_on_fire");
    if (!o) return;                                   // (no NULL test in the original)
    burn->AttachChild(o->GetNode(), true);            // vf30; vfA0(node, 1)
    UpdateNodeAt(burn.Get(), 0.0f);                   // NiAVObject::Update(0, 1) 0x001018D0
}

void PutOutFireInSungodsHead(LevelData& level) {      // orig: 0x000C50F0 (thiscall S)
    Character* c = FindStageCharacter(level, "mr_sungod");   // 0x002A3584
    if (!c) return;
    for (GameObject* o = c->inventory.NextObject(nullptr); o; o = c->inventory.NextObject(o))   // +0x38
        if (o->label && *o->label == "sungod_hair_on_fire" && o->def->IsType(0x40000)) {   // REPE CMPSB 0x14; vf0C
            if (ni::NiNode* n = o->GetNode(); n && n->parent)
                (void)n->parent->DetachChild(n);      // vfA4 (the returned pointer released)
            delete o;                                 // vf00(1)
            return;
        }
}

}  // namespace fut::game

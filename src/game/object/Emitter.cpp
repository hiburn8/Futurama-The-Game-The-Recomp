// Particle-emitter objects; see Emitter.h.
#include "game/object/Emitter.h"

#include "core/CrtRand.h"
#include "engine/ni/NiClone.h"
#include "game/GameRandom.h"
#include "game/GameTime.h"
#include "game/defs/EffectDefs.h"
#include "game/level/LevelData.h"
#include "game/object/Character.h"
#include "game/object/NodeUpdate.h"
#include "game/physics/Physics.h"
#include "game/sound/GameSound.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace fut::game {
namespace {

using ni::detail::F;

constexpr f32 kHalfPi = std::bit_cast<f32>(0x3FC90E56u);   // orig: 0x0029C2B8 (1.5707500f)
constexpr f32 kTwoPi = std::bit_cast<f32>(0x40C90E56u);    // orig: 0x0029C2B4 (6.2830000f)

// orig: NiParticles(count, verts, normals, colors) 0x0010F710 with its data 0x0010F170.
ni::NiPointer<ni::NiParticles> MakeParticles(u16 count) {
    ni::NiPointer<ni::NiParticles> p(new ni::NiParticles);
    auto* d = new ni::NiParticlesData;
    d->vertexCount = count;
    d->vertices.assign(count, {});
    d->colors.assign(count, {});
    d->activeCount = count;
    d->sizes.assign(count, 1.0f);
    // (the constructor computes the bound from the vertices: a single vertex at the origin gives (origin, 0);
    // the sprite emitters overwrite theirs)
    p->modelData = d;
    return p;
}

ni::NiParticlesData& Data(ni::NiParticles& p) { return static_cast<ni::NiParticlesData&>(*p.modelData); }

// The shared sprite-particle properties (orig: static objects 0x002DF008 / 0x002DEFD0 / 0x002DEE58 / 0x002DEF34,
// configured by the first SpriteEmitterDef constructor 0x0006E171).
struct SpriteProperties {
    ni::NiPointer<ni::NiAlphaProperty> alphaBlend, alphaAdd;
    ni::NiPointer<ni::NiVertexColorProperty> vertexColor;
    ni::NiPointer<ni::NiZBufferProperty> zbuffer;
};
const SpriteProperties& SharedSpriteProperties() {
    static const SpriteProperties props = [] {
        SpriteProperties s;
        s.alphaBlend = new ni::NiAlphaProperty;
        s.alphaBlend->flags = 0x00ED;                 // blend SRCALPHA / INVSRCALPHA
        s.alphaAdd = new ni::NiAlphaProperty;
        s.alphaAdd->flags = 0x000D;                   // additive
        s.vertexColor = new ni::NiVertexColorProperty;
        s.vertexColor->sourceVertexMode = 1;          // orig: +0x18 = 1
        s.vertexColor->lightingMode = 0;              // orig: +0x1C = 0
        s.zbuffer = new ni::NiZBufferProperty;
        s.zbuffer->flags = 0x0001;
        s.zbuffer->testFunction = 3;
        return s;
    }();
    return props;
}

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: the x87 exp sequence of 0x0006D447 / 0x0006EC3F (FLDL2E; FRNDINT; F2XM1; FSCALE): exp(-drag * frameTime).
// FLDL2E pushes the 64-bit constant 0xB8AA3B295C17F0BC * 2^-63 = kLog2eHi + kLog2eLo (both exact doubles).
f32 Decay(f32 drag, f32 frameTime) {
    constexpr double kLog2eHi = 1.4426950408889634;              // 0x3FF71547652B82FE
    constexpr double kLog2eLo = 2.0383000842727483e-17;          // 0x3C77800000000000 (188 * 2^-63)
    const double x = -(double(drag) * frameTime);
    const double y = std::fma(x, kLog2eHi, x * kLog2eLo);       // FMULP (round53 of the 64-bit product)
    const double n = std::nearbyint(y);                          // FRNDINT (RC = nearest)
    const double m = std::exp2(y - n);                           // F2XM1; FLD1; FADDP (C library: #20)
    return F(std::ldexp(m, static_cast<int>(n)));                // FSCALE; FSTP dword
}

// orig: 0x00103960: the matrix with columns a, b, c.
ni::NiMatrix3 FromColumns(const ni::NiPoint3& a, const ni::NiPoint3& b, const ni::NiPoint3& c) {
    ni::NiMatrix3 m;
    m.m[0][0] = a.x; m.m[1][0] = a.y; m.m[2][0] = a.z;
    m.m[0][1] = b.x; m.m[1][1] = b.y; m.m[2][1] = b.z;
    m.m[0][2] = c.x; m.m[1][2] = c.y; m.m[2][2] = c.z;
    return m;
}


// The integration shared by both PostUpdates (render state): position, gravity, friction.
void Integrate(EmitterParticle& p, f32 ft, f32 gravity, f32 decay) {
    const f32 dy = F(double(ft) * p.velocity.y), dz = F(double(ft) * p.velocity.z);
    p.position.x = F(double(ft) * p.velocity.x + p.position.x);   // product kept in ST0
    p.position.y = F(double(dy) + p.position.y);
    p.position.z = F(double(dz) + p.position.z);
    p.velocity.y = F(double(p.velocity.y) - double(gravity) * ft);   // FLD g; FMUL ft; FSUBR vel.y
    p.velocity.x = F(double(decay) * p.velocity.x);
    p.velocity.y = F(double(decay) * p.velocity.y);
    p.velocity.z = F(double(decay) * p.velocity.z);
}

}  // namespace

// ---- EmitterInstance ------------------------------------------------------------------------------------------------
EmitterInstance::~EmitterInstance() {                 // orig: 0x0006DC20
    if (voice) level.Sound().StopVoice(static_cast<s32>(voice));   // Voice_Stop 0x0006DC5A (no Voice_Exists)
}

void EmitterInstance::PlayLoopSound(u32 sample, f32 maxDistance, f32 minDistance) {   // orig: 0x0006D0B0
    sound::GameSound& snd = level.Sound();
    if (voice) snd.StopVoice(static_cast<s32>(voice));   // 0x0006D0BE
    const ni::NiPoint3 p = node->world.translate;     // +0x2C +0xA0 (copied: a fixed position)
    voice = static_cast<u32>(snd.Play3D(sample, p, true));   // Sound_Play3D 0x0006D0EE (loop 1)
    if (!(minDistance == -1.0f))                      // FCOMP [0x00293BF0]; TEST AH,0x44; JNP
        snd.SetMinDistance(static_cast<s32>(voice), minDistance);   // 0x0006D112
    snd.SetMaxDistance(static_cast<s32>(voice), maxDistance);       // 0x0006D123
}

EmitterInstance::EmitterInstance(LevelData& lvl, EmitterDef& d) : GameObject(lvl, d) {   // orig: 0x0006DB50
    const EmitterDef::Params& c = d.current;
    speed = c.range28;
    lifetime = c.range30;
    elevation = c.range38;
    delay = c.range40;
    count = c.count;
    drag = c.value4C;
    gravity = c.gravity;
    loop = c.flag55;
    damage = c.value58;
    reach = c.value5C;
}

bool EmitterInstance::Tick() {                        // orig: 0x0006CEC0
    frameTime = F(double(g_frameStep) + frameTime);
    if (finished) return false;
    time = F(double(g_frameStep) + time);
    return true;
}

void EmitterInstance::InitParticle(u32 i) {           // orig: 0x0006CEF0 (x87 sequence of the listing)
    EmitterParticle& p = particles[i];
    const auto& d = static_cast<EmitterDef&>(*def);
    const double r0 = RandUnit();
    const f32 el = F(((1.0 - r0) * elevation[1] + r0 * elevation[0]) * kHalfPi);
    const f32 az = F(RandUnit() * kTwoPi);
    const double ce = std::cos(double(el));
    f32 ax = F(ce * direction.x), ay = F(ce * direction.y), az1 = F(ce * direction.z);
    const f32 se = F(std::sin(double(el)));
    const double r2 = RandUnit();
    const double spd = (1.0 - r2) * speed[1] + r2 * speed[0];
    const double t = std::cos(double(az)) * se;
    const double qx = t * basisV.x + ax;
    const double qy = double(F(t * basisV.y)) + ay;
    az1 = F(double(F(t * basisV.z)) + az1);
    const double s = std::sin(double(az)) * se;
    const f32 vx = F(s * basisU.x + qx);
    const double wy = double(F(s * basisU.y)) + qy;
    const f32 vz = F(double(F(s * basisU.z)) + az1);
    p.velocity = {F(double(vx) * spd), F(wy * spd), F(double(vz) * spd)};
    const double u = d.current.flag54 ? double(i) * stagger : RandUnit();
    p.lifetime = F((1.0 - u) * lifetime[1] + u * lifetime[0]);
    const double r4 = RandUnit();
    p.age = F(-((1.0 - r4) * delay[1] + r4 * delay[0]));
    p.position = {};
}

bool EmitterInstance::Init() {                        // orig: 0x0006DCF0
    const auto& d = static_cast<EmitterDef&>(*def);
    particles.assign(count, {});                      // (uninitialised in the original)
    ni::Unitize(direction);
    const ni::NiPoint3 seed = direction.x > 0.9f ? ni::NiPoint3{0, 0, 1} : ni::NiPoint3{1, 0, 0};   // TEST AH,0x41
    basisV = ni::UnitCross(direction, seed);
    basisU = ni::UnitCross(basisV, direction);
    stagger = F(1.0 / double(s32(count)));            // FILD; FDIVR [1.0]
    for (u32 i = 0; i < count; ++i) InitParticle(i);
    if (d.texturing) {
        textureParticles = MakeParticles(1);
        ni::NiParticles& p = *textureParticles;
        p.local.translate = {};
        Data(p).sizes[0] = d.current.value60;
        Data(p).value34 = 1.0f;
        Data(p).colors[0] = {1, 1, 1, 1};
        AttachProperty(p, d.texturing.Get());
        auto* a = new ni::NiAlphaProperty;            // flags 0xEC -> 0x000D
        a->flags = 0x000D;
        AttachProperty(p, a);
        auto* z = new ni::NiZBufferProperty;          // flags 3 -> 0x0001, test 3
        z->flags = 0x0001;
        z->testFunction = 3;
        AttachProperty(p, z);
        Data(p).activeCount = static_cast<u16>(std::min<u32>(1, Data(p).vertexCount));   // orig: vf38(1)
        p.UpdateProperties();                         // 0x0006E031
        node->AttachChild(&p, true);
    }
    return true;
}

// ---- SpriteEmitter --------------------------------------------------------------------------------------------------
SpriteEmitter::~SpriteEmitter() {                     // orig: 0x0006E5B0
    if (sprites) node->DetachChild(sprites.Get());    // +0xBC: (+0x2C)->vfA4(&tmp, +0xBC), the temporary released
    sprites = nullptr;                                // (then the EmitterInstance dtor 0x0006DC20)
}

SpriteEmitter::SpriteEmitter(LevelData& lvl, EmitterDef& d) : EmitterInstance(lvl, d) {   // orig: 0x0006E4C0
    const auto& sd = static_cast<SpriteEmitterDef&>(d).spriteCurrent;
    size = sd.size;
    alpha = sd.rangeB4;
    startColor = sd.startColor;
    endColor = sd.endColor;
    randomBrightness = sd.flagDC;
}

bool SpriteEmitter::Init() {                          // orig: 0x0006E680
    EmitterInstance::Init();
    const auto& d = static_cast<SpriteEmitterDef&>(*def);
    sprites = MakeParticles(static_cast<u16>(count));   // (vertices uninitialised in the original, colours zero)
    ni::NiParticles& p = *sprites;
    for (f32& s : Data(p).sizes) s = d.spriteCurrent.size[0];
    Data(p).value34 = 1.0f;
    const SpriteProperties& shared = SharedSpriteProperties();
    AttachProperty(p, d.spriteTexturing.Get());
    AttachProperty(p, d.spriteCurrent.flagA8 ? shared.alphaAdd.Get() : shared.alphaBlend.Get());
    AttachProperty(p, shared.vertexColor.Get());
    AttachProperty(p, shared.zbuffer.Get());
    p.modelData->bound = {{}, 100.0f};
    Data(p).activeCount = 0;                          // orig: vf38(0)
    node->local.rotate = {};
    node->AttachChild(&p, true);
    node->UpdateProperties();                         // 0x0006E8F4
    node->UpdateDownwardPass(0.0f, true);
    return true;
}

GameObject* SpriteEmitterDef::SpawnEffect(LevelData& level, const ni::NiPoint3& pos, const ni::NiPoint3& dir) {
    auto* o = new SpriteEmitter(level, *this);        // orig: 0x0006F2A0
    o->direction = dir;
    o->SetOwner(&level);
    o->SetPosition(pos);
    o->SetRotation(ni::NiMatrix3{});
    o->Init();
    Reset();                                          // orig: def vf64
    return o;
}

// ---- MeshEmitter ----------------------------------------------------------------------------------------------------
MeshEmitter::MeshEmitter(LevelData& lvl, EmitterDef& d) : EmitterInstance(lvl, d) {
    orient = static_cast<MeshEmitterDef&>(d).currentBC;
}

void MeshEmitter::InitPieceSpin(u32 i, bool reinit) {   // orig: 0x0006D840
    if (reinit) InitParticle(i);
    EmitterParticle& p = particles[i];
    const f32 a = F(RandUnit() * 2.0 - 1.0);          // FADD ST0,ST0; FSUB [1.0]
    const f32 b = F(RandUnit() * 2.0 - 1.0);
    const f32 c = F(RandUnit() * double(0.001f));     // FMUL [0x00293AC4]
    ni::NiPoint3 axis{c, b, a};
    ni::Unitize(axis);
    p.spinAxis = axis;
    p.spinSpeed = F((RandUnit() * 2.0 - 1.0) * 30.0f);   // FMUL [0x0029763C]
}

bool MeshEmitter::Init() {                            // orig: 0x0006EA30
    const auto& d = static_cast<MeshEmitterDef&>(*def);
    EmitterInstance::Init();
    if (textureParticles) node->DetachChild(textureParticles.Get());
    const u32 meshCount = static_cast<u32>(d.meshes.size());
    for (u32 i = 0; i < count; ++i) {
        u32 idx;
        if (randomMesh) {
            idx = meshCount ? static_cast<u32>(crt::Rand()) % meshCount : 0;   // (DIV by 0 in the original)
        } else {
            idx = i < meshCount ? i : meshCount - 1;
        }
        ni::NiAVObject* src = idx < meshCount ? d.meshes[idx].Get() : nullptr;
        ni::NiPointer<ni::NiObject> co = src ? ni::Clone(src) : nullptr;
        auto* c = dynamic_cast<ni::NiAVObject*>(co.Get());
        ni::NiPointer<ni::NiNode> n(new ni::NiNode);
        const double r = RandUnit();                  // orig: RandRange 0x000B6FB0
        const f32 sc = F(std::fabs((double(d.rangeC4[1]) - d.rangeC4[0]) * r + d.rangeC4[0]));
        if (c) c->local.scale = sc;                   // (a missing mesh faults in the original)
        node->AttachChild(n.Get(), true);
        if (c) n->AttachChild(c, true);
        n->flags |= ni::kNiAVHidden;
        InitPieceSpin(i, false);
    }
    if (textureParticles) node->AttachChild(textureParticles.Get(), true);
    node->local.rotate = {};
    node->UpdateProperties();                         // 0x0006EC0A
    node->UpdateDownwardPass(0.0f, true);
    return true;
}

GameObject* MeshEmitterDef::SpawnEffect(LevelData& level, const ni::NiPoint3& pos, const ni::NiPoint3& dir) {
    auto* o = new MeshEmitter(level, *this);          // orig: 0x0006F4C0
    o->direction = dir;
    o->SetOwner(&level);
    o->SetPosition(pos);
    o->SetRotation(ni::NiMatrix3{});
    o->randomMesh = flagCC;                           // orig: vf58
    o->Init();
    Reset();
    return o;
}

// ---- the per-frame simulation (docs/analysis/emitter_update_port.md 3 / 4) ----------------------------------------
bool SpriteEmitter::PostUpdate() {                    // orig: 0x0006D2D0
    if (emitting && damage != 0.0f) {                 // TEST AH,0x44; JNP (NaN proceeds)
        const f32 ox = F(double(reach) * direction.x), oy = F(double(reach) * direction.y),
                  oz = F(double(reach) * direction.z);
        const ni::NiPoint3 w = GetWorldPosition();    // vf34
        const ni::NiPoint3 P{F(double(ox) + w.x), F(double(oy) + w.y), F(double(oz) + w.z)};
        for (Player* pl : level.players) {            // orig: 0x000C2730 (S+0x98 via +0xC4C)
            if (!pl->body) continue;                  // (players always have one)
            const phys::CharShape& h = pl->body->shape;   // (P+0x5C)+0x40
            const double minX = double(h.pos.x) - h.ext.x;   // kept in ST0
            const f32 minY = F(double(h.pos.y) - h.ext.y), minZ = F(double(h.pos.z) - h.ext.z);
            const f32 maxX = F(double(h.pos.x) + h.ext.x);
            const f32 maxY = F(double(h.ext.y) + h.pos.y), maxZ = F(double(h.ext.z) + h.pos.z);
            if (minX < P.x && P.x < maxX && minY < P.y && P.y < maxY && minZ < P.z && P.z < maxZ)
                pl->Damage(damage, nullptr);          // vf7C 0x00051AD0
        }
    }
    if (textureParticles) Data(*textureParticles).activeCount = 0;   // vf38(0) [render]
    const f32 decay = Decay(drag, frameTime);
    u32 dead = 0, live = 0;
    ni::NiParticlesData& D = Data(*sprites);
    for (u32 i = 0; i < count; ++i) {                 // (count > 0 signed: JLE)
        EmitterParticle* p = &particles[i];
        if (p->age > p->lifetime) {                   // TEST AH,0x41; JNE
            if (!loop) {
                ++dead;
                continue;
            }
            InitParticle(i);                          // vf50: 4 | 5 rand()
            p = &particles[i];
        }
        const bool wasDelayed = p->age < 0.0f;        // TEST AH,5; JP
        if (emitting || !wasDelayed) p->age = F(double(p->age) + frameTime);
        if (p->age < 0.0f) continue;                  // TEST AH,5; JNP
        if (wasDelayed && sourceA) {
            const ni::NiPoint3 a = sourceA->world.translate;
            if (sourceB) {
                const ni::NiPoint3 b = sourceB->world.translate;
                const double r = RandUnit();          // RNG
                const double q = 1.0 - r;
                p->position = {F(r * a.x + F(q * b.x)), F(r * a.y + F(q * b.y)), F(double(F(r * a.z)) + q * b.z)};
            } else {
                p->position = a;
            }
            const ni::NiPoint3& n = node->world.translate;
            p->position = {F(double(p->position.x) - n.x), F(double(p->position.y) - n.y),
                           F(double(p->position.z) - n.z)};
            if (textureParticles) {                   // [render]
                ni::NiParticlesData& T = Data(*textureParticles);
                T.activeCount = static_cast<u16>(std::min<u32>(1, T.vertexCount));
                T.vertices[0] = p->position;
            }
        }
        Integrate(*p, frameTime, gravity, decay);     // [render]
        D.vertices[live] = p->position;
        const double t = double(p->age) / p->lifetime;   // FDIV, kept
        const double u = 1.0 - t;
        ni::NiColorA& c = D.colors[live];
        c.r = F(u * startColor[0] + t * endColor[0]);
        c.g = F(u * startColor[1] + t * endColor[1]);
        c.b = F(u * startColor[2] + t * endColor[2]);
        D.sizes[live] = F(u * size[0] + t * size[1]);
        double al = u * alpha[0] + t * alpha[1];
        if (al < 0.0) al = 0.0;                       // FCOM 0; TEST AH,5; JP
        c.a = F(al);
        if (randomBrightness) {
            const double r = RandUnit();              // RNG: every live particle
            c.r = F(r * c.r);
            c.g = F(r * c.g);
            c.b = F(r * c.b);
        }
        ++live;
    }
    D.activeCount = static_cast<u16>(std::min<u32>(live, D.vertexCount));   // vf38(live)
    D.revisionFlags = 0xFF;                           // orig: u16 +0x2E = 0xFFFF [render]
    frameTime = 0.0f;
    if (dead == count) finished = 1;
    return true;
}

bool MeshEmitter::PostUpdate() {                      // orig: 0x0006EC30
    const f32 decay = Decay(drag, frameTime);
    if (textureParticles) Data(*textureParticles).activeCount = 0;   // [render]
    if (follow) node->local.translate = follow->world.translate;     // [render]
    u32 dead = 0;
    const auto& kids = node->Children();
    for (u32 i = 0; i < count; ++i) {
        EmitterParticle* p = &particles[i];
        ni::NiNode* piece = i < kids.size() ? dynamic_cast<ni::NiNode*>(kids[i].Get()) : nullptr;
        if (!piece) {                                 // (faults in the original)
            NotPorted("MeshEmitter::PostUpdate: a missing piece node");
            continue;
        }
        if (p->age > p->lifetime) {
            if (loop) {
                InitPieceSpin(i, true);               // vf5C(i, 1): 8 | 9 rand()
                piece->flags |= ni::kNiAVHidden;
            } else {
                piece->flags |= ni::kNiAVHidden;
                ++dead;
            }
            continue;
        }
        if (emitting || !(p->age < 0.0f)) p->age = F(double(p->age) + frameTime);
        if (p->age < 0.0f) {
            piece->flags |= ni::kNiAVHidden;
            continue;
        }
        if (piece->flags & ni::kNiAVHidden) {         // first visible frame
            p->origin = node->world.translate;        // orig: p+0x0C
            if (textureParticles) {
                ni::NiParticlesData& T = Data(*textureParticles);
                T.activeCount = static_cast<u16>(std::min<u32>(1, T.vertexCount));
            }
        }
        piece->flags &= ~ni::kNiAVHidden;
        // ---- render state ----
        if (orient == 1) {
            const ni::NiPoint3& nw = node->world.translate;
            const ni::NiPoint3 d{F(double(nw.x) - p->origin.x), F(double(nw.y) - p->origin.y),
                                 F(double(nw.z) - p->origin.z)};
            Integrate(*p, frameTime, gravity, decay);
            ni::NiPoint3 dv = p->velocity;
            ni::Unitize(dv);
            piece->local.translate = {F(double(p->position.x) - d.x), F(double(p->position.y) - d.y),
                                      F(double(p->position.z) - d.z)};
            const ni::NiPoint3 h{p->velocity.x, 0.0f, p->velocity.z};
            ni::NiPoint3 up, side;
            if (double(h.z) * h.z + double(h.x) * h.x != 0.0) {
                side = ni::UnitCross(h, {0.0f, 1.0f, 0.0f});
                up = ni::UnitCross(side, p->velocity);
            } else {
                if (p->velocity.y == 0.0f) continue;
                up = p->position;
                ni::Unitize(up);
                side = ni::UnitCross(dv, up);
            }
            piece->local.rotate = FromColumns(dv, up, side);
        } else {
            Integrate(*p, frameTime, gravity, decay);
            piece->local.translate = p->position;
            piece->local.rotate = ni::AxisAngle(F(double(p->spinSpeed) * p->age), p->spinAxis.x, p->spinAxis.y,
                                            p->spinAxis.z);
        }
    }
    if (node->framesNotDrawn == 0) UpdateNodeAt(node.Get(), 0.0f);   // 0x001018D0 Update(0.0f, 1), only when drawn
    frameTime = 0.0f;
    if (dead == count) finished = 1;
    return true;
}

}  // namespace fut::game

// ---- the muzzle flare (docs/analysis/npc_combat2_port.md 1.8.1) ------------------------------------------------------
namespace fut::game {

GameObject* FlareDef::SpawnEffect(LevelData& level, const ni::NiPoint3& pos, const ni::NiPoint3& dir) {   // 0x0006A480
    auto* o = new Flare(level, *this);                // new(0x58), EffectInstance 0x00065CE0
    o->value40 = value2C;                             // +0x40 = D+0x2C (+0x44 = 0, +0x54 = 0)
    o->dir = dir;                                     // +0x48
    o->SetOwner(&level);                              // vf10
    o->SetPosition(pos);                              // vf1C
    o->SetRotation(ni::NiMatrix3{});                  // vf24 (0x002D7820)
    o->Init();                                        // vf04
    value2C = value28;                                // vf28 0x00069FB0
    return o;
}

Flare::Flare(LevelData& lvl, FlareDef& d) : GameObject(lvl, d) {}

Flare::~Flare() { spinNode = nullptr; }               // orig: 0x0006A0F0 (release +0x54)

bool Flare::Init() {                                  // orig: 0x0006A180
    const auto& d = static_cast<const FlareDef&>(*def);
    (void)ni::Unitize(dir);                           // in place (+0x48; FSTP ST0)
    // [0x00293C08] = 0.9f; TEST AH,0x41; JNE -> UY (<, == or NaN)
    const ni::NiPoint3 up = dir.y > 0.9f ? ni::NiPoint3{0.0f, 0.0f, 1.0f} : ni::NiPoint3{0.0f, 1.0f, 0.0f};
    ni::NiPoint3 c{F(double(up.y) * dir.z - double(up.z) * dir.y), F(double(up.z) * dir.x - double(up.x) * dir.z),
                   F(double(up.x) * dir.y - double(up.y) * dir.x)};
    (void)ni::Unitize(c);                             // 0x000116A0 (FSTP ST0)
    ni::NiMatrix3 m;                                  // chunks c, c x d, d (chunk k = column k)
    m.m[0][0] = c.x, m.m[1][0] = c.y, m.m[2][0] = c.z;
    m.m[0][1] = F(double(c.y) * dir.z - double(c.z) * dir.y);
    m.m[1][1] = F(double(c.z) * dir.x - double(c.x) * dir.z);
    m.m[2][1] = F(double(c.x) * dir.y - double(c.y) * dir.x);
    m.m[0][2] = dir.x, m.m[1][2] = dir.y, m.m[2][2] = dir.z;
    spinNode = new ni::NiNode;                        // +0x54 (new(0xE0) 0x00105C10)
    spinNode->local.rotate = m;                       // (+0x54)+0x30, read back as Multiply's argument
    const f32 a = F(RandRange(0.0f, 3.0f));           // Script_RandRange 0x000B6FB0 (FSTP by the caller); rand 0x0006A35D
    spinNode->local.rotate = ni::Multiply(ni::AxisAngle(a, dir.x, dir.y, dir.z), m);   // 0x00103130; 0x00013B30 (this = R)
    UpdateNodeAt(spinNode.Get(), 0.0f);               // NiAVObject::Update(0, 1)
    node->AttachChild(spinNode.Get(), true);          // vfA0
    ni::NiPointer<ni::NiObject> cl = d.mesh ? ni::Clone(d.mesh.Get()) : nullptr;   // 0x000FE120 (no null test)
    spinNode->AttachChild(dynamic_cast<ni::NiAVObject*>(cl.Get()), true);   // DynamicCast 0x002F9190
    UpdateNodeAt(node.Get(), 0.0f);
    node->UpdateProperties();                         // 0x0006A431
    node->UpdateEffects();                            // 0x0006A439
    return true;
}

bool Flare::Tick() {                                  // orig: 0x00069FC0
    if (shown) return false;                          // +0x44
    time = F(double(g_frameStep) + time);             // FLD [0x002D67E8]; FADD +0x24
    return true;
}

bool Flare::PostUpdate() {                            // orig: 0x00069FE0
    shown = 1;
    UpdateNodeAt(spinNode.Get(), time);               // (+0x54)->Update(+0x24, 1)
    return true;
}

}  // namespace fut::game

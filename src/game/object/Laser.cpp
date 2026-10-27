// The projectiles; see Laser.h, docs/analysis/fireball_port.md (the Laser, section numbers) and
// docs/analysis/npc_combat2_port.md 1.7 / 1.8 (the Projectile base, the laser's character hit; checked against the
// assembly statement by statement, the corrections in that file's status block).
#include "game/object/Laser.h"

#include "core/CrtString.h"
#include "engine/ni/NiClone.h"
#include "engine/ni/NiCollision.h"
#include "engine/ni/NiMath.h"
#include "game/GameRandom.h"
#include "game/GameTime.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/EffectDefs.h"
#include "game/defs/ExplosionDef.h"
#include "game/level/LevelData.h"
#include "game/level/LightManager.h"
#include "game/level/ScriptTriggers.h"
#include "game/level/Stage.h"
#include "game/object/Character.h"
#include "game/object/Moveable.h"
#include "game/object/Destructable.h"
#include "game/object/Emitter.h"
#include "game/object/GenericNpc.h"
#include "game/object/Impact.h"
#include "game/object/MarkInstance.h"
#include "game/object/NodeUpdate.h"
#include "game/physics/Physics.h"
#include "game/sound/GameSound.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

// orig: 0x00078294..0x00078320 / 0x00078C89..0x00078D15: Shoot_Splash (SplashDef 0x00073880) at p - 0.02 dir (the x
// and y products stay on the x87 stack, z is stored first) with (0, 1, 0), owned by the current stage (S+0x5C ? +4 :
// 0), "Splashive", node scale 2 (0x00073720).
void ShootSplash(LevelData& level, Def* sd, const ni::NiPoint3& p, const ni::NiPoint3& dir);

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

void AddBodyVelocity(phys::CharBody& b, const NiPoint3& a) {   // orig: CharBody vf50 0x000AF7D0
    NiPoint3& v = b.velocity;
    v = {F(double(v.x) + a.x), F(double(a.y) + v.y), F(double(a.z) + v.z)};
}

// orig: (+0x40) ? (+0x40)+0x5C : 0, the shooter's body. The original dereferences +0x40 without testing that the
// character still exists; the port asks the character list first (a deleted shooter: no body, UB-avoiding).
const phys::PhysObject* ShooterBody(const LevelData& level, const Character* shooter) {
    return shooter && level.IsCharacter(shooter) ? shooter->body.get() : nullptr;
}

void ShootSplash(LevelData& level, Def* sd, const ni::NiPoint3& p, const ni::NiPoint3& dir) {
    auto* splash = dynamic_cast<SplashDef*>(sd);      // (the original calls 0x00073880 directly)
    if (!splash) return;
    const double dx = double(dir.x) * 0.02f, dy = double(dir.y) * 0.02f;   // FMUL [0x00294360]
    const f32 dz = F(double(dir.z) * 0.02f);          // FSTP
    const ni::NiPoint3 at{F(double(p.x) - dx), F(double(p.y) - dy), F(double(p.z) - dz)};
    ni::NiPoint3 up{0.0f, 1.0f, 0.0f};                // (becomes UNIT_Z: 0x00073880)
    SetNodeScale(*splash->SpawnDir(level, level.currentStage, at, up, "Splashive"), 2.0f);   // 0x00295CD8
}

// The beam's shared render properties: process globals created by the static initialisers 0x001B61E0 (0x002E0BEC,
// new(0x1C) NiZBufferProperty vtbl 0x00296C80: flags 3, +0x18 = 3), 0x001B6260 (0x002E0BE8, new(0x1C)
// NiAlphaProperty vtbl 0x00293B54: flags 0xEC, +0x18 = 0) and 0x001B62E0 (0x002E0C38, new(0x20)
// NiVertexColorProperty vtbl 0x00293AD0: flags 0, +0x18 = 0, +0x1C = 1), configured by the first beam (the byte
// 0x002D6634, 1 until then). Render state only.
struct BeamProperties {
    ni::NiPointer<ni::NiZBufferProperty> zbuffer;     // 0x002E0BEC
    ni::NiPointer<ni::NiAlphaProperty> alpha;         // 0x002E0BE8
    ni::NiPointer<ni::NiVertexColorProperty> vertexColor;   // 0x002E0C38
    bool unconfigured = true;                         // 0x002D6634
    BeamProperties() : zbuffer(new ni::NiZBufferProperty), alpha(new ni::NiAlphaProperty),
                       vertexColor(new ni::NiVertexColorProperty) {
        zbuffer->flags = 3;
        zbuffer->testFunction = 3;
        alpha->flags = 0xEC;
        alpha->alphaTestRef = 0;
        vertexColor->flags = 0;
        vertexColor->sourceVertexMode = 0;            // +0x18
        vertexColor->lightingMode = 1;                // +0x1C
    }
};

BeamProperties& SharedBeamProperties() {
    static BeamProperties p;
    return p;
}

// orig: Laser::Init 0x00079043..0x0007959E (a laser definition without a model): three crossed quads (120 degrees
// apart, D+0x78 from the axis, D+0x7C long) as NiTriStrips, a clone of them, two materials and the shared
// properties, both under a new NiNode (the object's +0x44).
ni::NiPointer<ni::NiNode> BuildBeam(const LaserDef& d, const std::array<f32, 4>& colour) {
    ni::NiPointer<ni::NiTriStripsData> data(new ni::NiTriStripsData);   // new(0x44) 0x0011BB60
    std::vector<NiPoint3> v(12);                      // new(0x90): 12 x NiPoint3 (0x00011740 with 0x000114B0)
    std::vector<ni::NiPoint2> uv(12);                 // new(0x60)
    const f32 kPi = std::bit_cast<f32>(0x40490FDBu);  // [0x002F9274] (PI at run time)
    const f32 kThird = std::bit_cast<f32>(0x3F2AAAABu);   // [0x0029BAD0] (2/3)
    const f32 r = d.value68[4], len = d.value68[5];   // D+0x78, D+0x7C
    for (int i = 0; i < 3; ++i) {
        const double a = double(i) * kPi * kThird;    // FILD; FMUL [0x2F9274]; FMUL [0x29BAD0]
        const f32 c = F(std::cos(a) * r);             // FLD ST0; FCOS; FMUL D+0x78; FSTP
        const f32 s = F(std::sin(a) * r);             // FSIN; FMUL D+0x78; FST (then FCHS for -s)
        v[4 * i + 0] = {0.0f, s, c};
        v[4 * i + 1] = {len, s, c};
        v[4 * i + 2] = {0.0f, -s, -c};
        v[4 * i + 3] = {len, -s, -c};
        uv[4 * i + 0] = {0.0f, 0.0f};
        uv[4 * i + 1] = {1.0f, 0.0f};
        uv[4 * i + 2] = {0.0f, 1.0f};
        uv[4 * i + 3] = {1.0f, 1.0f};
    }
    data->vertexCount = 12;
    data->vertices = std::move(v);
    data->bound = ni::BoundOfPoints(data->vertices);  // (the data ctor's bound; replaced below)
    data->textureSets = 1;
    data->uvs = std::move(uv);
    data->triangleCount = 12;
    data->stripCount = 3;
    data->stripLengths = {6, 6, 6};                   // new(6)
    for (u16 k = 0; k < 3; ++k)                       // new(0x24): 4k, 4k+1, 4k+2, 4k+3, 4k, 4k+1
        for (int j : {0, 1, 2, 3, 0, 1}) data->stripLists.push_back(static_cast<u16>(4 * k + j));
    ni::NiPointer<ni::NiTriStrips> strips(new ni::NiTriStrips);   // new(0xE0) 0x0011C1F0(12, v, 0, 0, uv, 12, 3, ..)
    strips->modelData = data.Get();
    ni::NiPointer<ni::NiTriStrips> clone;
    {
        ni::NiPointer<ni::NiObject> c = ni::Clone(strips.Get());   // NiObject::Clone 0x000FE120 (no cast)
        clone = dynamic_cast<ni::NiTriStrips*>(c.Get());
    }
    BeamProperties& sp = SharedBeamProperties();
    if (sp.unconfigured) {                            // 0x0007929A
        sp.zbuffer->flags |= 1;                       // +0x14 |= 1
        sp.zbuffer->flags &= ~0x2;                    // byte +0x14 &= 0xFD
        sp.alpha->flags |= 1;
        sp.alpha->flags &= ~0x200;                    // byte +0x15 &= 0xFD
        sp.alpha->flags = static_cast<u16>((sp.alpha->flags & 0xFFED) | 0xC);
        sp.alpha->flags &= 0xFE1F;
        sp.vertexColor->lightingMode = 0;             // +0x1C
        sp.vertexColor->sourceVertexMode = 0;         // +0x18
        sp.unconfigured = false;
    }
    {                                                 // new(0x5C) 0x00114B10: the colour, alpha +0x10C
        ni::NiPointer<ni::NiMaterialProperty> m(new ni::NiMaterialProperty);
        m->emissive = {colour[0], colour[1], colour[2]};   // +0x40..+0x48 = +0x100..+0x108
        m->revision += 2;                             // +0x54 incremented twice
        m->alpha = colour[3];                         // +0x50 = +0x10C
        AttachProperty(*strips, m.Get());             // AddHead 0x00013A50, in this order
        AttachProperty(*strips, sp.alpha.Get());
        AttachProperty(*strips, d.glowTexturing.Get());   // D+0x9C
        AttachProperty(*strips, sp.zbuffer.Get());
        AttachProperty(*strips, sp.vertexColor.Get());
    }
    if (clone) {                                      // (port guard: the original has no null test)
        ni::NiPointer<ni::NiMaterialProperty> m(new ni::NiMaterialProperty);   // white ([0x002D7BD0]), alpha 0.3
        m->emissive = {1.0f, 1.0f, 1.0f};
        m->alpha = std::bit_cast<f32>(0x3E99999Au);
        m->revision += 2;
        AttachProperty(*clone, m.Get());
        AttachProperty(*clone, sp.zbuffer.Get());
        AttachProperty(*clone, d.beamTexturing.Get());   // D+0xA0
        AttachProperty(*clone, sp.alpha.Get());
        AttachProperty(*clone, sp.vertexColor.Get());
    }
    data->bound.center = {0.0f, 0.0f, 0.0f};          // (+0xC8)+0x0C = [0x002F915C] (zero), +0x18 = 10.0
    data->bound.radius = 10.0f;
    // strips->vf30(): 0x000B0420 (the NiAVObject slot, a nop)
    ni::NiPointer<ni::NiNode> root(new ni::NiNode);   // new(0xE0) 0x00105C10
    root->AttachChild(strips.Get(), true);            // vfA0(strips, 1)
    root->AttachChild(clone.Get(), true);             // vfA0(clone, 1) (a NULL child is ignored)
    // root->vf30(): NiNode 0x001062E0 (no visible effect)
    return root;
}

// orig: 0x00077D00 (thiscall(f32 x), RET 4; the laser's damage fall-off with its flown distance x): 1.1235298
// ([0x0029CF40]) - x 0.0041176 ([0x0029CF44]) (the register), at most 1, at least 0.3 ([0x00293B18]).
double DamageFalloff(f32 x) {
    const double v = double(std::bit_cast<f32>(0x3F8FCFD0u)) - double(x) * std::bit_cast<f32>(0x3B86ED54u);
    if (v > 1.0f) return 1.0f;                        // FCOM [0x00293A98]; TEST AH,0x41; JNE
    if (v < 0.3f) return 0.3f;                        // FCOM [0x00293B18]; TEST AH,5; JP (NaN kept)
    return v;
}

}  // namespace

// ---- npc_combat2_port.md 1.7: the Projectile base ---------------------------------------------------------------------
GameObject* ProjectileDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                                 const char* label) {   // orig: vf14 0x000798A0 -> vf20 (arg4 = 0)
    return Spawn20(level, owner, pos, rot, 0, label);
}

GameObject* ProjectileDef::Spawn20(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                                   u32 arg4, const char* label) {   // orig: vf20 0x0007BC00
    auto* o = new Projectile(level, *this);           // orig: new(0x100) + 0x0007A4A0
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C
    o->SetRotation(rot);                              // vf24
    o->arg74 = arg4;                                  // orig: +0x74 = arg4
    if (model) o->Init();                             // orig: D+0x18 ? vf04
    return o;
}

Projectile::Projectile(LevelData& lvl, ProjectileDef& d) : GameObject(lvl, d) {   // orig: 0x0007A4A0
    // +0x44 / +0x9C / +0xAC / +0xBC / +0x40 = 0 (the members' defaults); the rest is written by Init (a projectile
    // without a model keeps heap leftovers there in the original: the port's members start at 0).
    trailTimer = (d.flag50 == 0 && d.emitter34 == nullptr) ? -1000.0f : 10.0f;   // orig: 0xC47A0000 / 10.0f
    exploded = 0;                                     // +0xF0
    // +0x3C = Sound_PlayAttached(D+0x5C, node, 1) 0x0007A52A (unconditional: a NULL sample gives 0)
    loopVoice = static_cast<u32>(level.Sound().PlayAttached(d.sound5C, node.Get(), true));
}

Projectile::~Projectile() {                           // orig: 0x0007A550
    if (loopVoice && level.Sound().VoiceExists(static_cast<s32>(loopVoice))) {   // 0x0007A583
        level.Sound().StopVoice(static_cast<s32>(loopVoice));   // Voice_Stop 0x000C9B10
        loopVoice = 0;
    }
    model = nullptr;                                  // orig: release +0x44
}

bool Projectile::Init() {                             // orig: vf04 0x0007B8B0
    auto& d = static_cast<ProjectileDef&>(*def);
    ni::NiPointer<ni::NiObject> c = ni::Clone(d.model.Get());   // orig: NiObject::Clone 0x000FE120
    model = dynamic_cast<ni::NiNode*>(c.Get());       // orig: DynamicCast NiNode (0x002F9230)
    node->AttachChild(model.Get(), true);             // orig: vfA0
    if (model) model->flags &= ~ni::kNiAVHidden;      // (the original has no null test)
    node->UpdateProperties();                         // 0x0007B93E
    lastTime = 0.0f;                                  // +0xD4
    speed = d.value1C;                                // +0x48..+0x58 = D+0x1C..+0x2C
    restitution = d.value20;
    gravity = d.value24;
    lifetime = d.value28;
    explodeOnContact = d.flag2C;
    blast = {d.value38, d.value3C, d.value40, d.value44, d.value48, d.value4C};   // +0x5C..+0x70 = D+0x38..+0x4C
    start = node->local.translate;                    // +0x78 = node +0x60
    hit = 0;                                          // +0xD8
    // a = R^T UX, b = R^T UY (0x00103290, R = the node's world rotation +0x70; SetRotation updated the node).
    const NiPoint3 a = ni::TransposeMulVector(node->world.rotate, {1.0f, 0.0f, 0.0f});
    const NiPoint3 b = ni::TransposeMulVector(node->world.rotate, {0.0f, 1.0f, 0.0f});
    // (H) as written: (a.x, b.x, -a.z) = (R[0][0], R[1][0], -R[0][2]); for GenericNpc vf100's aim matrix (columns d,
    // c x d, -c) that is (d.x, d.y, c.x) (npc_combat2_port.md open question 3).
    velocity = {a.x, b.x, F(double(a.z) * -1.0f)};    // FMUL [0x00293BF0]
    const double s = speed;                           // FLD +0x48 (kept)
    velocity = {F(s * velocity.x), F(s * velocity.y), F(s * velocity.z)};
    UpdateNodeAt(node.Get(), time);                   // orig: NiAVObject::Update(+0x24, 1) 0x001018D0
    tumble = ni::NiMatrix3{};                         // orig: 0x00102FF0 (identity)
    spinAxis = {0.0f, 1.0f, 0.0f};                    // +0xC0
    spinSpeed = 0;                                    // +0xCC
    spinAngle = 0;                                    // +0xD0
    Tumble();                                         // vf54
    return true;
}

void Projectile::Tumble() {                           // orig: vf54 0x0007BAD0
    const auto& d = static_cast<const ProjectileDef&>(*def);
    if (d.value54 < 0.0f || d.value54 == 0.0f) return;   // FCOMP 0; TEST AH,0x41; JNP (NaN goes on)
    tumble = ni::Multiply(tumble, ni::AxisAngle(spinAngle, spinAxis.x, spinAxis.y, spinAxis.z));   // 0x00103130 / 0x00013B30
    // RandUnit; FADD ST0,ST0; FSUB [1.0]; FMUL [0x00293AA0] (0.5); FADD axis; FSTP
    spinAxis.x = F((RandUnit() * 2.0 - 1.0) * 0.5f + spinAxis.x);   // rand 0x0007BB52
    spinAxis.y = F((RandUnit() * 2.0 - 1.0) * 0.5f + spinAxis.y);   // rand 0x0007BB6F
    spinAxis.z = F((RandUnit() * 2.0 - 1.0) * 0.5f + spinAxis.z);   // rand 0x0007BB8E
    (void)ni::Unitize(spinAxis);                      // 0x000116A0 (FSTP ST0)
    spinAngle = 0;                                    // (stored between the RandUnit and its FSUB)
    spinSpeed = F((RandUnit() * 2.0 - 1.0) * 30.0f);  // rand 0x0007BBB6; FMUL [0x0029763C]
}

bool Projectile::Tick() {                             // orig: vf08 0x0007A5E0 (Session::Update step 7)
    if (lastTime == 0.0f && level.physic) {           // FCOMP 0; TEST AH,0x44; JP (-0.0 counts as 0)
        const f32 r = 0.0f;
        const auto& hits = level.physic->QueryObjects(ShooterBody(level, shooter), start, r);   // vf14
        if (!hits.empty()) {                          // count +0xC (JBE)
            hit = 1;                                  // +0xD8
            explodeOnContact = 1;                     // +0x58
            hitObject = hits[0];                      // +0xEC
            hitPoint = start;                         // +0xDC = +0x78
            hitTri = {};                              // +0xE8 = 0
            Explode();                                // 0x00079E70
        }
    }
    if (exploded) return false;                       // -> vf00(1)
    time = F(double(g_frameStep) + time);             // FLD [0x2D67E8]; FADD +0x24
    return true;
}

// (each F() is one FSTP; the sums written without F stay on the x87 stack)
bool Projectile::PostUpdate() {                       // orig: vf0C 0x0007A690 (Session::Render; result ignored)
    if (exploded) return false;
    if (time < 0.0f) return true;                     // FCOMP 0; TEST AH,5; JNP (NaN goes on)
    const auto& d = static_cast<const ProjectileDef&>(*def);
    bool hitNow = false;                              // [esp+0x1B]
    const f32 dt = F(double(time) - lastTime);        // [esp+0x70]
    const f32 g = -gravity;                           // FLD +0x50; FCHS; FSTP
    const double z = double(dt) * 0.0;                // FMUL [0x00293AB8] (kept)
    velocity.x = F(z + velocity.x);
    velocity.y = F(double(F(double(g) * dt)) + velocity.y);
    velocity.z = F(double(F(z)) + velocity.z);
    const NiPoint3 P = node->local.translate;         // +0x60 (dword copies)
    const double A = double(dt) * velocity.x, B = double(dt) * velocity.y;
    const f32 C = F(double(dt) * velocity.z);
    const double inv = 1.0 / double(g_frameStep);     // FLD [0x00293A98]; FDIV [0x002D67E8]
    const f32 Fi = F(inv);                            // FST [esp+0x54]
    NiPoint3 N{F(double(P.x) + F(inv * A)), F(double(P.y) + B * Fi), F(double(P.z) + double(C) * Fi)};
    NiPoint3 dir{F(double(N.x) - P.x), F(double(N.y) - P.y), F(double(N.z) - P.z)};
    f32 len = F(ni::Unitize(dir));                    // 0x000116A0 (FSTP the length)
    if (level.scriptTriggers)                         // the health triggers (DAT_002EA000, newest first)
        for (auto& tp : level.scriptTriggers->list) {
            ScriptTrigger& t = *tp;
            if (t.kind != ScriptTrigger::Health) continue;
            if (owner && owner->IsOwnerType(4)) {     // 0x0007A864: an inventory's holder (owner - 0x38)
                const Character& holder = static_cast<Character::Inventory*>(owner)->holder;
                if (!(holder.def->IsType(2) ? t.flag65 : t.flag64)) continue;
            }
            if (!(t.health > 0.0f)) continue;         // TEST AH,0x41; JNE
            if ((lastTime == 0.0f && HealthContainsPoint(t, P, 0.0f)) ||   // TEST AH,0x44; JP; 0x000B86F0
                HealthRayTest(t, P, dir, len)) {      // 0x000B7F00 (shortens len)
                const double h = double(t.health) - blast[0];   // FLD +0x5C; FSUBR [t+0x6C]; FST
                t.health = F(h);
                if (h < 0.0) t.health = 0.0f;         // FCOMP 0 (unrounded); TEST AH,5; JP
                hitNow = true;
                break;
            }
        }
    f32 len0 = len;                                   // [esp+0x78]
    hitTri = {};                                      // +0xE8 = 0
    ni::RayHit tri = ni::RayCast(P, dir, len, true, 3);   // 0x000FB780(&P, &dir, &len, 1, 3)
    const f32 k54 = d.value54;                        // [esp+0x50]
    if (!hitNow) {                                    // (a trigger hit jumps to the move 0x0007B6CC)
        if (tri) {                                    // 0x0007A975
            hitNow = true;
            for (s32 left = 3;;) {
                if (--left <= 0) {                    // 0x0007ACFE
                    explodeOnContact = 1;
                    break;
                }
                const f32 hz = F(double(dir.z) * len);    // (FSTP first)
                const NiPoint3 H{F(double(dir.x) * len + P.x), F(double(dir.y) * len + P.y), F(double(hz) + P.z)};
                const double r = double(len0) - len;
                const NiPoint3 R{F(double(dir.x) * r), F(double(dir.y) * r), F(double(dir.z) * r)};
                const NiPoint3 n = tri.Normal();      // 0x000FA8C0
                const double k = double(restitution) + 1.0;   // FADD [0x00293A98]
                const double s = (double(n.z) * (k * velocity.z) + double(n.y) * (k * velocity.y)) +
                                 double(n.x) * (k * velocity.x);
                velocity = {F(double(velocity.x) - double(n.x) * s),
                            F(double(velocity.y) - double(F(double(n.y) * s))),
                            F(double(velocity.z) - double(F(double(n.z) * s)))};
                const double q = ((double(R.z) + R.z) * n.z + (double(R.y) + R.y) * n.y) + (double(R.x) + R.x) * n.x;
                const NiPoint3 R2{F(double(R.x) - double(n.x) * q), F(double(R.y) - double(F(double(n.y) * q))),
                                  F(double(R.z) - double(F(double(n.z) * q)))};
                Tumble();                             // vf54 (no rand for D+0x54 <= 0)
                if ((double(velocity.x) * velocity.x + double(velocity.y) * velocity.y) +
                        double(velocity.z) * velocity.z < 1.0)   // FCOMP [0x00293A98]; TEST AH,5; JP
                    spinSpeed = 0;                    // +0xCC
                const f32 ez = F(double(n.z) * 0.01f);    // [0x00293AAC]
                N = {F((double(R2.x) + H.x) + double(n.x) * 0.01f),
                     F(double(F(double(R2.y) + H.y)) + double(n.y) * 0.01f),
                     F(double(F(double(R2.z) + H.z)) + double(ez))};
                const double L = std::sqrt((double(R.z) * R.z + double(R.y) * R.y) + double(R.x) * R.x);   // FSQRT
                double Lk = L;
                if (L > 1e-6f) {                      // FCOM [0x00293ACC]; TEST AH,0x41; JNE
                    const double i = 1.0 / L;
                    dir = {F(double(R.x) * i), F(double(R.y) * i), F(double(R.z) * i)};   // the unreflected remainder
                } else {
                    dir = {0.0f, 0.0f, 0.0f};         // 0x0007AC85
                    Lk = 0.0;
                }
                len = len0 = F(Lk);                   // FST [esp+0x34]; FSTP [esp+0x78]
                hitPoint = H;                         // +0xDC
                hit = 1;                              // +0xD8
                hitTri = tri;                         // +0xE8
                tri = ni::RayCast(P, dir, len, true, 3);   // (H) from the frame's start P
                if (!tri) break;
            }
        }
        if (k54 > 0.0f) NotPorted("Projectile: the sphere sweep (D+0x54 > 0, 0x0007AD17)");   // TEST AH,0x41; JNE
        hitObject = nullptr;                          // +0xEC (0x0007B649)
        if (level.physic) {
            phys::PhysObject* obj = level.physic->RayCastObjects(ShooterBody(level, shooter), P, dir, len, true);   // vf0C
            if (obj && obj->owner != shooter) {       // vf24
                hitPoint = P;                         // +0xDC
                hit = 1;
                hitNow = true;
                explodeOnContact = 1;
                hitObject = obj;
            }
        }
    }
    node->local.translate = N;                        // 0x0007B6CC
    UpdateNodeAt(node.Get(), time);                   // orig: NiAVObject::Update(+0x24, 1)
    const double left = double(lifetime) - time;      // FLD +0x54; FSUB +0x24
    if (left < 0.0 || left == 0.0 || (hitNow && explodeOnContact)) Explode();   // TEST AH,0x41; JNP / 0x0007B707
    if (trailTimer > -100.0f) {                       // 0x0007B71D: FCOMP [0x0029A4E4]; TEST AH,0x41; JNE
        const double x = double(dt) + trailTimer;     // FLD dt; FADD +0x38; FST +0x38
        trailTimer = F(x);
        if (x > 0.5f) {                               // FCOMP [0x00293AA0] (unrounded); TEST AH,0x41; JNE
            const NiPoint3 p = node->world.translate; // +0xA0
            if (d.emitter34) {                        // the fire trail: D+0x34->vf20(&p, &UY)
                auto* e = static_cast<EmitterInstance*>(d.emitter34->SpawnEffect(level, p, {0.0f, 1.0f, 0.0f}));
                e->sourceA = node.Get();              // NiPointer::Assign e+0x40 (no null test)
            }
            trailTimer = 0;
        }
    }
    if (k54 > 0.0f) {                                 // 0x0007B7E2: the spin step
        spinAngle = F(double(dt) * spinSpeed + spinAngle);   // FLD dt; FMUL +0xCC; FADD +0xD0; FST; FSTP
        SetRotation(ni::Multiply(tumble, ni::AxisAngle(spinAngle, spinAxis.x, spinAxis.y, spinAxis.z)));   // vf24
    }
    lastTime = time;                                  // +0xD4 = +0x24
    return true;
}

void Projectile::Explode() {                          // orig: 0x00079E70
    auto& d = static_cast<ProjectileDef&>(*def);
    exploded = 1;                                     // +0xF0
    const NiPoint3 P = node->local.translate;         // +0x60
    if (d.emitter30) {
        NiPoint3 u = velocity;
        (void)ni::Unitize(u);                         // (FSTP ST0)
        NiPoint3 nrm{-u.x, -u.y, -u.z};               // FCHS
        if (hitTri) nrm = hitTri.Normal();            // +0xE8 -> 0x000FA8C0
        const double dd = ((double(nrm.y) * u.y + double(nrm.z) * u.z) + double(nrm.x) * u.x) * 2.0;   // FADD ST0,ST0
        NiPoint3 r{F(double(u.x) - double(nrm.x) * dd + nrm.x), F(double(u.y) - double(F(double(nrm.y) * dd)) + nrm.y),
                   F(double(F(double(u.z) - double(F(double(nrm.z) * dd)))) + nrm.z)};
        (void)ni::Unitize(r);                         // (FSTP ST0)
        (void)d.emitter30->SpawnEffect(level, P, r);  // vf20
    } else if (arg74 == 0 || arg74 == 1) {            // orig: 0x00079FA5
        if (arg74 == 0) {                             // a 10-unit downward ray (RayCast 0x000FB780(&P, &(0, -1, 0),
            f32 len = 10.0f;                          //  &len, 1, 3)); its hit distance is compared with 10.0 but
            (void)ni::RayCast(P, {0.0f, -1.0f, 0.0f}, len, true, 3);   //  the flags are not used
        }
        // S+0x44->vf20(S, &P, arg74 ? 9 : 0, "ProjectileExplosion" 0x0029D090, 1) (no null test in the original)
        if (level.defs && level.defs->explosion)
            (void)level.defs->explosion->Spawn(level, &level, P, arg74 == 1 ? 9 : 0, "ProjectileExplosion", true);
    }
    bool some = false;                                // BL
    for (size_t i = 0; i < level.characters.size(); ++i) {   // 0x000C26E0 (the S+0x90 order)
        Character* c = level.characters[i];
        f32 dmg = 0;                                  // [esp+0xC]
        const NiPoint3 a = c->GetWorldPosition();     // (vf34 is called four times: same value)
        const f32 dx = F(double(P.x) - a.x), dz = F(double(P.z) - a.z), dy = F(double(P.y) - a.y);
        const f32 top = F(double(F(double(c->boundHeight) + c->heightOffset)) + a.y);   // +0x4D4 + +0x4D0
        double v = double(std::fabs(dy)) - (double(top) - a.y);   // FABS; FSUBR
        if (v < 0.0) v = 0.0;                         // FCOM 0; TEST AH,5; JP
        double e = std::sqrt((double(dz) * dz + double(dx) * dx) + v * v) - c->cdef.boundWidth;   // D+0x90
        if (e < 0.0) e = 0.0;
        if (e < blast[1] || e == blast[1]) dmg = blast[0];   // FCOM +0x60; TEST AH,0x41; JP (NaN: no)
        if (e < blast[3] || e == blast[3]) dmg = blast[2];
        if (e < blast[5] || e == blast[5]) dmg = blast[4];
        if (shooter && level.IsCharacter(shooter) && !shooter->body->noRay) {   // 0x00033EE0; (sh+0x5C)+0x1D
            shooter->DealDamage(c, dmg);              // vfCC
            some = true;
        } else {
            c->Damage(dmg, nullptr);                  // vf7C
        }
    }
    for (Destructable* o = level.firstDestructable; o; o = o->nextDestructable) {   // DAT_002DE13C; vf5C
        f32 dmg = 0;
        const NiPoint3& t = o->GetNode()->local.translate;   // vf30 + 0x60
        const double dx = double(P.x) - t.x, dy = double(P.y) - t.y, dz = double(P.z) - t.z;
        const double e = std::sqrt((dz * dz + dy * dy) + dx * dx);
        if (e < blast[1] || e == blast[1]) dmg = blast[0];
        if (e < blast[3] || e == blast[3]) dmg = blast[2];
        if (e < blast[5] || e == blast[5]) dmg = blast[4];
        (void)o->Damage(dmg);                         // vf64
    }
    if (hitTri && d.value64 != 1) NotPorted("Projectile::Explode: the scorch marks (0x000730C0)");
    if (!some) (void)level.Sound().Play3D(d.sound58, P, false);   // 0x0007A487 (no sample test)
}

// ---- fireball_port.md: the Laser --------------------------------------------------------------------------------------
// orig: vf50 0x00079E10: a collision-mesh hit on a character's own geometry (types 10 / 14, hit flags 0x80).
void Laser::OnMeshHit(ni::NiTriShape* geom, const ni::RayHit& mh) {
    Character* c = CharacterOwningMesh(level, geom);   // 0x00036A20
    if (!c || (c->type != 10 && c->type != 14)) return;   // +0x774
    if (!mh || mh.Flags() != 0x80) return;            // 0x000FA320
    // c->vfB8(+0x40, 100.0f, 1) (GenericNpc 0x00046DF0); a shooter no longer in the character list counts as none
    // (the port's guard of KNOWN_ISSUES #52)
    c->OnMeshContact(level.IsCharacter(shooter) ? shooter : nullptr, 100.0f, true);
}

// ---- 2.2 the spawn --------------------------------------------------------------------------------------------------
GameObject* LaserDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                            const char* label) {      // orig: ProjectileDef vf14 0x000798A0 -> vf20 0x000798C0
    return Spawn20(level, owner, pos, rot, 0, label);
}

GameObject* LaserDef::Spawn20(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                              u32 arg4, const char* label) {   // orig: vf20 0x000798C0
    auto* o = new Laser(level, *this);                // orig: new(0x160) + 0x00077D40
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C
    o->SetRotation(rot);                              // vf24
    o->arg74 = arg4;                                  // orig: +0x74 = arg4
    o->colour = {value68[0], value68[1], value68[2], value68[3]};   // orig: +0x100..+0x10C = D+0x68..+0x74
    o->Init();                                        // vf04
    return o;
}

GameObject* RailDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                           const char* label) {       // orig: vf14 0x000798A0 -> vf20 0x0007CDB0 (arg4 = 0)
    auto* o = new RailLaser(level, *this);            // orig: new(0x170) + 0x00077D40, vtable 0x0029D170
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C
    o->SetRotation(rot);                              // vf24
    o->arg74 = 0;                                     // orig: +0x74 = arg4
    o->colour = {value68[0], value68[1], value68[2], value68[3]};   // orig: +0x100..+0x10C = D+0x68..+0x74
    o->Init();                                        // vf04
    return o;
}

// ---- the railgun's beam (RailLaser, vtable 0x0029D170) --------------------------------------------------------------
namespace {

// orig: 0x0007C3B8.. / 0x0007C5E4..: the mirror of d about n, halved with n and unitized (the impact effects'
// direction): k = 2 ((n.z d.z + n.y d.y) + d.x n.x) unrounded; k n.y and k n.x stored, k n.z kept on the x87 stack.
NiPoint3 RailReflect(const NiPoint3& n, const NiPoint3& d) {
    const double k = ((double(n.z) * d.z + double(n.y) * d.y) + double(d.x) * n.x) * 2.0;   // FADD ST0,ST0
    const f32 ky = F(k * n.y), kx = F(k * n.x);       // FSTP [esp+0x24] / [esp+0x44]
    const double rz = double(d.z) - k * n.z;          // FSUBR d.z (unrounded)
    const double ry = double(d.y) - ky;
    const f32 rx = F(double(d.x) - kx);               // FSTP [esp+0x38]
    const f32 az = F(rz + n.z), ay = F(ry + n.y), ax = F(double(rx) + n.x);
    NiPoint3 h{F(double(ax) * 0.5f), F(double(ay) * 0.5f), F(double(az) * 0.5f)};   // [0x00293AA0]
    (void)ni::Unitize(h);                             // 0x000116A0 (FSTP ST0)
    return h;
}

// orig: the hit point origin + t dir (each product stored, then each sum).
NiPoint3 RailPoint(const NiPoint3& o, const NiPoint3& d, f32 t) {
    const f32 z = F(double(t) * d.z), y = F(double(t) * d.y), x = F(double(t) * d.x);
    return {F(double(x) + o.x), F(double(y) + o.y), F(double(z) + o.z)};
}

}  // namespace

RailLaser::RailLaser(LevelData& lvl, LaserDef& d) : Laser(lvl, d) {}   // (+0x160 / +0x164 / +0x168 = 0)

RailLaser::~RailLaser() {                             // orig: 0x0007BF10
    coreMaterial = nullptr;                           // +0x164 released first
    beamMaterial = nullptr;                           // +0x160
}

bool RailLaser::Init() {                              // orig: vf04 0x0007C0E0
    auto& d = static_cast<LaserDef&>(*def);
    radius = d.value68[5];                            // +0x118 = D+0x7C
    damage = d.value80;                               // +0x148 = D+0x80
    spin = d.flag84;                                  // +0x11C = D+0x84
    // the direction as Projectile::Init's (0x00103290 with UNIT_X / UNIT_Y on the node's world rotation +0x70)
    const NiPoint3 a = ni::TransposeMulVector(node->world.rotate, {1.0f, 0.0f, 0.0f});
    const NiPoint3 b = ni::TransposeMulVector(node->world.rotate, {0.0f, 1.0f, 0.0f});
    velocity = {a.x, b.x, F(double(a.z) * -1.0f)};    // +0x84 (FMUL [0x00293BF0])
    direction = velocity;                             // +0x12C
    // +0x84 = +0x48 dir: the speed is written only at the end of this function (the heap's leftover word in the
    // original, L: the port's 0); no RailLaser path reads the velocity
    const double spd = speed;
    velocity = {F(spd * velocity.x), F(spd * velocity.y), F(spd * velocity.z)};
    origin = node->local.translate;                   // +0x120 = node +0x60
    stageDist = 1000.0f;                              // +0x144 (0x447A0000)
    const ni::RayHit stageHit = ni::RayCast(origin, direction, stageDist, true, 3);   // 0x000FB780(.., 1, 3)
    normal = stageHit ? stageHit.Normal() : NiPoint3{-direction.x, -direction.y, -direction.z};   // 0x000FA8C0 / FCHS
    hitTrigger = nullptr;                             // +0x14C
    // the health triggers (DAT_002EA000, newest first) of an inventory owner's shot: the ray shortens +0x144
    if (level.scriptTriggers) {
        for (auto& tp : level.scriptTriggers->list) {
            ScriptTrigger* t = tp.get();
            if (!t || t->kind != ScriptTrigger::Health) continue;   // +0x08 == 8
            if (!owner || !owner->IsOwnerType(4)) continue;   // +4 vf0C(4)
            auto* inv = dynamic_cast<Character::Inventory*>(owner);   // owner - 0x38
            const bool player = inv && inv->holder.def->IsType(2);   // (no null test in the original)
            if (!(t->health > 0.0f)) continue;        // FCOMP 0; TEST AH,0x41; JNE (NaN skips)
            if (!(player ? t->flag65 : t->flag64)) continue;
            if (HealthRayTest(*t, origin, direction, stageDist)) hitTrigger = t;   // 0x000B7F00
        }
    }
    // the physics objects (Physic vf04 0x000AE8D0: every object, front faces only; the last that accepted)
    phys::PhysObject* hitObj =
        level.physic ? level.physic->RayCastObjects(nullptr, origin, direction, stageDist, true) : nullptr;
    if (hitObj && hitObj->owner) {                    // vf24
        const NiPoint3 p = RailPoint(origin, direction, stageDist);   // esp+0x50
        normal = {-direction.x, -direction.y, -direction.z};   // +0x138 (FCHS)
        const NiPoint3 h = RailReflect(normal, direction);   // esp+0x38
        if (d.sparks) (void)d.sparks->SpawnEffect(level, p, h);   // D+0xA4 vf20(&p, &h)
        if (GameObject* o = hitObj->owner) {          // vf24 again
            if (d.flag85) damage = F(DamageFalloff(F(double(time) * speed)) * damage);   // 0x00077D00 * +0x148
            target = o;                               // +0x168
        }
        hitTrigger = nullptr;                         // +0x14C = 0 (an object hit takes the shot)
    }
    if (ScriptTrigger* t = hitTrigger) {              // 0x0007C542
        const double hp = double(t->health) - damage; // FLD +0x148; FSUBR t+0x6C; FST
        t->health = F(hp);
        if (hp < 0.0) t->health = 0.0f;               // FCOMP 0 (unrounded); TEST AH,5; JP
    }
    if (!hitObj && !hitTrigger) {                     // 0x0007C56C: the stage (or nothing within 1000)
        if (ni::g_lastMeshHit) OnMeshHit(ni::g_lastMeshHit, stageHit);   // vf50(DAT_002F8F14, the ray's triangle)
        const NiPoint3 p = RailPoint(origin, direction, stageDist);   // esp+0x38
        const NiPoint3 h = RailReflect(normal, direction);   // esp+0x2C
        if (d.sparks) (void)d.sparks->SpawnEffect(level, p, h);   // D+0xA4 vf20
        // "effect_smoke" (0x002976C4; Session::FindDef 0x000C3F10, no null test in the original)
        if (Def* smoke = level.defs ? level.defs->Find("effect_smoke") : nullptr) (void)smoke->SpawnEffect(level, p, h);
        if (!ni::g_lastMeshHit)                       // the scorch mark: (S+0xA4) 0x000730C0(0, &p, normal)
            if (auto* mi = static_cast<MarkInstance*>(level.markInstance)) (void)mi->AddScorch(0, p, normal);
        if (d.flag86) NotPorted("RailLaser: the explosion at the impact (D+0x86 && S+0x44: (S+0x44)->vf20(S, &p, 0, 0, 1))");
        if (d.value88 && normal.y > std::bit_cast<f32>(0x3F6064EDu))   // [0x0029CF54]; TEST AH,0x41; JNE
            NotPorted("RailLaser: the shock (D+0x88 vf14(S, &p, identity 0x00102FF0, \"RailShock\"))");
    }
    radius = stageDist;                               // +0x118 = +0x144 (the beam's length)
    // the beam (as Laser::Init's 0x00079043.., the length +0x118): three crossed quads, a clone, two materials
    ni::NiPointer<ni::NiTriStripsData> data(new ni::NiTriStripsData);   // (NiTriStrips 0x0011C1F0's data)
    std::vector<NiPoint3> v(12);                      // new(0x90)
    std::vector<ni::NiPoint2> uv(12);                 // new(0x60)
    const f32 kPi = std::bit_cast<f32>(0x40490FDBu);  // [0x002F9274]
    const f32 kThird = std::bit_cast<f32>(0x3F2AAAABu);   // [0x0029BAD0]
    const f32 r = d.value68[4], len = radius;         // D+0x78, +0x118
    for (int i = 0; i < 3; ++i) {
        const double ang = double(i) * kPi * kThird;  // FILD; FMUL; FMUL
        const f32 c = F(std::cos(ang) * r);           // FCOS; FMUL D+0x78; FSTP
        const f32 s = F(std::sin(ang) * r);           // FSIN; FMUL D+0x78; FST (FCHS for -s)
        v[4 * i + 0] = {0.0f, s, c};
        v[4 * i + 1] = {len, s, c};
        v[4 * i + 2] = {0.0f, -s, -c};
        v[4 * i + 3] = {len, -s, -c};
        uv[4 * i + 0] = {0.0f, 0.0f};
        uv[4 * i + 1] = {1.0f, 0.0f};
        uv[4 * i + 2] = {0.0f, 1.0f};
        uv[4 * i + 3] = {1.0f, 1.0f};
    }
    data->vertexCount = 12;
    data->vertices = std::move(v);
    data->bound = ni::BoundOfPoints(data->vertices);
    data->textureSets = 1;
    data->uvs = std::move(uv);
    data->triangleCount = 12;
    data->stripCount = 3;
    data->stripLengths = {6, 6, 6};                   // new(6)
    for (u16 k = 0; k < 3; ++k)                       // new(0x24)
        for (int j : {0, 1, 2, 3, 0, 1}) data->stripLists.push_back(static_cast<u16>(4 * k + j));
    ni::NiPointer<ni::NiTriStrips> strips(new ni::NiTriStrips);   // new(0xE0) 0x0011C1F0
    strips->modelData = data.Get();
    ni::NiPointer<ni::NiTriStrips> clone;
    {
        ni::NiPointer<ni::NiObject> c = ni::Clone(strips.Get());   // NiObject::Clone 0x000FE120 (no cast)
        clone = dynamic_cast<ni::NiTriStrips*>(c.Get());
    }
    BeamProperties& sp = SharedBeamProperties();     // (configured by the first beam, 0x002D6634)
    if (sp.unconfigured) {
        sp.zbuffer->flags |= 1;
        sp.zbuffer->flags &= ~0x2;
        sp.alpha->flags |= 1;
        sp.alpha->flags &= ~0x200;
        sp.alpha->flags = static_cast<u16>((sp.alpha->flags & 0xFFED) | 0xC);
        sp.alpha->flags &= 0xFE1F;
        sp.vertexColor->lightingMode = 0;             // +0x1C
        sp.vertexColor->sourceVertexMode = 0;         // +0x18
        sp.unconfigured = false;
    }
    beamMaterial = new ni::NiMaterialProperty;        // new(0x5C) 0x00114B10 -> +0x160
    beamMaterial->emissive = {colour[0], colour[1], colour[2]};   // +0x40..+0x48 = +0x100..+0x108
    beamMaterial->revision += 2;                      // +0x54 incremented twice
    beamMaterial->alpha = colour[3];                  // +0x50 = +0x10C
    AttachProperty(*strips, beamMaterial.Get());      // AddHead 0x00013A50, in this order (not the laser's)
    AttachProperty(*strips, sp.vertexColor.Get());
    AttachProperty(*strips, sp.alpha.Get());
    AttachProperty(*strips, d.glowTexturing.Get());   // D+0x9C
    AttachProperty(*strips, sp.zbuffer.Get());
    if (clone) {                                      // (port guard: the original has no null test)
        coreMaterial = new ni::NiMaterialProperty;    // -> +0x164: white ([0x002D7BD0]), alpha 0.3
        coreMaterial->emissive = {1.0f, 1.0f, 1.0f};
        coreMaterial->alpha = std::bit_cast<f32>(0x3E99999Au);
        coreMaterial->revision += 2;
        AttachProperty(*clone, coreMaterial.Get());
        AttachProperty(*clone, sp.zbuffer.Get());
        AttachProperty(*clone, d.beamTexturing.Get());   // D+0xA0
        AttachProperty(*clone, sp.alpha.Get());
        AttachProperty(*clone, sp.vertexColor.Get());
    }
    data->bound.center = {0.0f, 0.0f, 0.0f};          // (+0xC8)+0x0C = [0x002F915C], +0x18 = 100.0 (0x42C80000)
    data->bound.radius = 100.0f;
    model = new ni::NiNode;                           // new(0xE0) 0x00105C10 -> +0x44
    model->AttachChild(strips.Get(), true);           // vfA0(strips, 1)
    model->AttachChild(clone.Get(), true);            // vfA0(clone, 1) (a NULL child is ignored)
    node->AttachChild(model.Get(), true);             // (+0x2C) vfA0(+0x44, 1)
    model->flags &= ~ni::kNiAVHidden;                 // +0xB0 &= 0xFE
    node->UpdateEffects();                            // 0x00102190
    node->UpdateProperties();                         // 0x00101EA0
    UpdateNodeAt(node.Get(), 0.0f);                   // NiAVObject::Update(0, 1)
    lastTime = 0.0f;                                  // +0xD4
    speed = d.value1C;                                // +0x48..+0x58 = D+0x1C..+0x2C
    restitution = d.value20;
    gravity = d.value24;
    lifetime = d.value28;
    explodeOnContact = d.flag2C;
    blast = {d.value38, d.value3C, d.value40, d.value44, d.value48, d.value4C};   // +0x5C..+0x70
    start = node->local.translate;                    // +0x78
    hit = 0;                                          // +0xD8
    return true;
}

bool RailLaser::Tick() {                              // orig: vf08 0x0007BEE0
    if (time > 0.3f) return false;                    // FCOMP [0x00293B18]; TEST AH,0x41; JNE (-> vf00(1))
    time = F(double(g_frameStep) + time);             // FLD [0x2D67E8]; FADD +0x24
    return true;
}

bool RailLaser::PostUpdate() {                        // orig: vf0C 0x0007BF90 (Session::Render)
    if (GameObject* t = target) {                     // +0x168: the damage, once
        if (t->def->IsType(1)) {                      // a character: vf7C(+0x148, +0x40)
            static_cast<Character*>(t)->Damage(damage, level.IsCharacter(shooter) ? shooter : nullptr);
        } else if (t->def->IsType(0x40)) {            // a destructable: vf64(+0x148)
            if (auto* ds = dynamic_cast<Destructable*>(t)) (void)ds->Damage(damage);
        } else {
            (void)t->def->IsType(0x80);               // (tested, nothing done)
        }
        target = nullptr;
    }
    if (spin && model)                                // +0x11C: the beam's roll, 5 rad / s ([0x00293C24])
        model->local.rotate = ni::MakeXRotation(F(double(time) * 5.0f));   // 0x00103070 -> (+0x44)+0x30
    // the fade over 0.3 s: 1 - 3.33 t ([0x0029D1C8]) times the colour's alpha, and 0.3 of it for the core
    const double k = 1.0 - double(time) * std::bit_cast<f32>(0x40555555u);   // FMUL; FSUBR [0x00293A98]
    f32 a1 = F(k * colour[3]);                        // FMUL +0x10C; FSTP
    double a2 = k * double(std::bit_cast<f32>(0x3E99999Au));   // FMUL [0x00293B18] (kept)
    if (a1 < 0.0f) a1 = 0.0f;                         // FCOMP 0; TEST AH,5; JP
    if (a2 < 0.0) a2 = 0.0;                           // FCOM 0; TEST AH,5; JP
    if (beamMaterial) {                               // +0x160 (no null tests in the original)
        beamMaterial->alpha = a1;
        ++beamMaterial->revision;
    }
    if (coreMaterial) {                               // +0x164
        coreMaterial->alpha = F(a2);
        ++coreMaterial->revision;
    }
    node->UpdateProperties();                         // (+0x2C) 0x00101EA0
    UpdateNodeAt(node.Get(), time);                   // NiAVObject::Update(+0x24, 1)
    return true;
}

// ---- 3.2 the constructors -------------------------------------------------------------------------------------------
Laser::Laser(LevelData& lvl, LaserDef& d) : Projectile(lvl, d) {   // orig: 0x00077D40 -> Projectile 0x0007A4A0
    done = 0;                                         // +0x154
    frameTime = 0;                                    // +0x158
    firstUpdate = 1;                                  // +0x11D
}

Laser::~Laser() = default;                            // orig: 0x00077CF0 -> Projectile 0x0007A550

// ---- 4 Init ---------------------------------------------------------------------------------------------------------
bool Laser::Init() {                                  // orig: vf04 0x00078FB0
    auto& d = static_cast<LaserDef&>(*def);
    radius = d.value68[5];                            // orig: +0x118 = D+0x7C
    damage = d.value80;                               // orig: +0x148 = D+0x80
    spin = d.flag84;                                  // orig: +0x11C = D+0x84
    if (!d.model) {
        model = BuildBeam(d, colour);                 // 0x00079043..0x0007959E
    } else {
        ni::NiPointer<ni::NiObject> c = ni::Clone(d.model.Get());   // orig: NiObject::Clone 0x000FE120
        model = dynamic_cast<ni::NiNode*>(c.Get());   // orig: DynamicCast NiNode (0x002F9230)
    }
    node->AttachChild(model.Get(), true);             // orig: vfA0 (a NULL child is ignored)
    if (model) model->flags &= ~ni::kNiAVHidden;
    node->UpdateEffects();                            // 0x000795C4
    node->UpdateProperties();                         // 0x000795CC
    UpdateNodeAt(node.Get(), 0.0f);                   // orig: NiAVObject::Update(0, 1) 0x001018D0
    lastTime = 0.0f;
    speed = d.value1C;
    restitution = d.value20;
    gravity = d.value24;
    lifetime = d.value28;
    explodeOnContact = d.flag2C;
    blast = {d.value38, d.value3C, d.value40, d.value44, d.value48, d.value4C};
    start = node->local.translate;
    hit = 0;
    // dir = R * (1, 0, 0) (the vector at 0x002D77E0). The original's matrix chunks +0x70 / +0x80 / +0x90 are the
    // port's columns: FLD [0x2D77E4]; FMUL m[c][1]; FLD [0x2D77E8]; FMUL m[c][2]; FADDP; FLD [0x2D77E0]; FMUL m[c][0];
    // FADDP.
    const ni::NiMatrix3& R = node->world.rotate;
    auto comp = [&](int c) { return F((0.0 * R.m[c][1] + 0.0 * R.m[c][2]) + 1.0 * R.m[c][0]); };
    NiPoint3 dir;
    dir.z = comp(2);                                  // (x87 order z, y, x; the sign of zero is kept)
    dir.y = comp(1);
    dir.x = comp(0);
    direction = dir;
    velocity = {F(double(speed) * dir.x), F(double(speed) * dir.y), F(double(speed) * dir.z)};
    origin = node->local.translate;
    stageDist = 1000.0f;                              // orig: 0x447A0000
    const ni::RayHit h = ni::RayCast(origin, direction, stageDist, false, 1);   // orig: 0x000FB780
    normal = h ? h.Normal() : NiPoint3{-dir.x, -dir.y, -dir.z};   // 0x000FA8C0 / FCHS
    triggerDist = stageDist;
    hitTrigger = nullptr;
    if (level.lightManager) {                         // orig: DAT_002EA824 vf04 0x000C08F0 (not attached)
        ni::NiPointer<LMSource> light =
            level.lightManager->AddLight(level, {0, 0, 0}, {colour[0], colour[1], colour[2]}, 30.0f, 8, false);
        node->AttachChild(light.Get(), true);         // (the node keeps the only reference)
    }
    return true;
}

// ---- 5 Tick ---------------------------------------------------------------------------------------------------------
bool Laser::Tick() {                                  // orig: vf08 0x00077D90 (Session::Update step 7)
    if (done) return false;                           // -> vf00(1)
    frameTime = F(double(g_frameStep) + frameTime);   // FLD [0x2D67E8]; FADD +0x158
    time = F(double(g_frameStep) + time);             // FLD [0x2D67E8]; FADD +0x24
    return true;
}

// ---- 7.1 the health-trigger scan ------------------------------------------------------------------------------------
void Laser::FindHealthTrigger() {                     // orig: 0x00077DC0 (the list DAT_002EA000, newest first)
    if (!level.scriptTriggers) return;
    for (auto& tp : level.scriptTriggers->list) {
        ScriptTrigger& t = *tp;
        if (t.kind != ScriptTrigger::Health) continue;
        if (!(t.health > 0.0f)) continue;             // FCOMP 0; TEST AH,0x41; JNE skip (NaN skips)
        bool may = true;
        if (owner && owner->IsOwnerType(4)) {         // 0x00077E18: (owner - 0x38)->def vf0C(2) ? +0x65 : +0x64
            // (as the Projectile's 0x0007A864; unreached: every Laser spawn passes the session as its owner)
            const Character& holder = static_cast<Character::Inventory*>(owner)->holder;
            if (!(holder.def->IsType(2) ? t.flag65 : t.flag64)) may = false;   // BL = 0 (the shooter test still runs)
        }
        if (shooter && shooter->def->IsType(1)) {
            if (!(shooter->def->IsType(2) ? t.flag65 : t.flag64)) continue;
        }
        if (!may) continue;
        if (HealthContainsPoint(t, origin, 0.0f)) {   // orig: 0x000B86F0
            hitTrigger = &t;
            triggerDist = 0.0f;
            return;
        }
        if (HealthRayTest(t, origin, direction, triggerDist)) hitTrigger = &t;   // orig: 0x000B7F00 (goes on)
    }
}

// ---- 6 PostUpdate ---------------------------------------------------------------------------------------------------
bool Laser::PostUpdate() {                            // orig: vf0C 0x00077EE0 (Session::Render; result ignored)
    if (firstUpdate) {
        FindHealthTrigger();                          // orig: 0x00077DC0
        firstUpdate = 0;
    }
    if (done) return true;
    if (time < 0.0f) return true;                     // FCOMP 0; TEST AH,5; JNP exit (NaN continues)
    const auto& d = static_cast<const LaserDef&>(*def);
    const double s = double(speed) * time;            // (kept in ST0)
    if (s > d.value98) {                              // TEST AH,0x41; JNE (NaN continues)
        done = 1;
        return true;
    }
    if (hitTrigger && s + radius > triggerDist) {     // FLD ST0; FADD +0x118; FCOMP +0x150; TEST AH,0x41; JNE
        // orig: 0x00077F78, the health trigger's impact
        ScriptTrigger& t = *hitTrigger;
        const double h = double(t.health) - damage;   // 0x00050800(+0x148): FLD [t+0x6C]; FSUB; FST
        t.health = F(h);
        if (h < 0.0) t.health = 0.0f;                 // FCOMP 0 (unrounded); TEST AH,5; JP
        normal ={-direction.x, -direction.y, -direction.z};   // +0x138 (FCHS each)
        const NiPoint3 T = node->local.translate;     // (+0x2C)+0x60 (copied before the product)
        const f32 over = F(double(s) + radius - triggerDist);   // FLD s; FADD +0x118; FSUB +0x150; FSTP (the argument)
        const NiPoint3 q{F(double(over) * direction.x), F(double(over) * direction.y), F(double(over) * direction.z)};   // 0x000115C0
        const NiPoint3 P{F(double(T.x) + q.x), F(double(T.y) + q.y), F(double(T.z) + q.z)};
        if (label) {                                  // +0x34
            f32 scale = 1.0f;
            const char* fx = nullptr;
            if (crt::StrICmp(label->c_str(), "Tank_Laser_shot") == 0) fx = "Tank_Impact";   // 0x0029D040 / 0x0029D034
            else if (crt::StrICmp(label->c_str(), "Igner_Laser") == 0) fx = "Igner_Impact", scale = 0.5f;   // 0x0029D024
            else if (crt::StrICmp(label->c_str(), "Destructor_Laser") == 0)
                fx = "Destructor_Impact", scale = std::bit_cast<f32>(0x3F333333u);   // 0x0029D010 (0.7)
            if (auto* idef = fx && level.defs ? dynamic_cast<ImpactDef*>(level.defs->Find(fx)) : nullptr) {   // 0x000C3F10
                Owner* stageOwner = level.currentStage;   // S+0x5C ? S+0x5C + 4 : 0 (the stage's Owner part)
                if (Impact* o = idef->SpawnUp(level, stageOwner, P, normal, "Impressive"))   // 0x0004EE90 (0x0029D004)
                    SetNodeScale(*o, scale);          // 0x00073720 (no null test in the original)
            }
        }
        done = 1;                                     // +0x154
        (void)level.Sound().Play3D(d.sound58, P, false);   // Sound_Play3D 0x0007810D (every path)
        return true;
    }
    // 6.2 the step
    const NiPoint3 T = node->local.translate;         // orig: [E+0x70]
    const f32 sy = F(s * direction.y), sz = F(s * direction.z);   // FSTP [E+0x4C] / [E+0x50] first
    NiPoint3 p{F(s * direction.x + origin.x), F(double(sy) + origin.y), F(double(sz) + origin.z)};
    bool solid = false;                               // orig: byte [E+0x3B]
    f32 len = F(double(frameTime) * speed + radius);  // FLD frameTime; FMUL speed; FADD radius
    const ni::RayHit mesh = ni::RayCast(T, direction, len, false, 2);   // orig: 0x000FB780 (collision meshes)
    if (mesh) {                                       // 6.6 b (0x000781CF)
        normal = mesh.Normal();                       // 0x000FA8C0 into +0x138
        const f32 pz = F(double(len) * direction.z);  // (FSTP [E+0x44] first)
        p = {F(double(len) * direction.x + T.x), F(double(len) * direction.y + T.y), F(double(pz) + T.z)};
        if (ni::g_lastMeshHit) OnMeshHit(ni::g_lastMeshHit, mesh);   // 0x00078241: vf50
        const u32 kind = level.footstepMaterials[mesh.Flags() & 3];   // 0x000FA320; S+0x160
        if (kind >= 4 && kind <= 6) {
            if (Def* sd = level.defs ? level.defs->Find("Shoot_Splash") : nullptr)   // 0x0029CFF4
                ShootSplash(level, sd, p, direction);   // 0x00078294
        } else {
            solid = true;                             // [E+0x3B] = 1
        }
    }
    // 0x0007832C: the shooter's body is not tested (an NPC's own shot does not hit it)
    phys::PhysObject* hitObj = level.physic ? level.physic->RayCastObjects(ShooterBody(level, shooter), T, direction,
                                                                           len, false)
                                            : nullptr;
    if (hitObj && hitObj->owner != shooter) {         // vf24 (0x0007836D): npc_combat2_port.md 1.8 (b)
        const NiPoint3& dv = direction;               // (esi = +0x12C)
        const NiPoint3 q{F(double(len) * dv.x), F(double(len) * dv.y), F(double(len) * dv.z)};   // 0x000115C0
        const NiPoint3 P2{F(double(T.x) + q.x), F(double(T.y) + q.y), F(double(T.z) + q.z)};
        normal = {-dv.x, -dv.y, -dv.z};               // +0x138 (FCHS)
        const NiPoint3& n = normal;
        const f32 k = F(((double(n.z) * dv.z + double(n.y) * dv.y) + double(n.x) * dv.x) * 2.0);   // FADD ST0,ST0; FSTP
        const NiPoint3 r{F(double(k) * n.x), F(double(k) * n.y), F(double(k) * n.z)};   // 0x000115C0
        const NiPoint3 w{F(double(dv.x) - r.x), F(double(dv.y) - r.y), F(double(dv.z) - r.z)};   // 0x00011530
        const double sx = double(w.x) + n.x, sy2 = double(w.y) + n.y;   // (kept on the x87 stack)
        const f32 szr = F(double(w.z) + n.z);         // FSTP [esp+0x44]
        NiPoint3 e{F(sx * 0.5f), F(sy2 * 0.5f), F(double(szr) * 0.5f)};   // [0x00293AA0]
        (void)ni::Unitize(e);                         // 0x000116A0 (FSTP ST0)
        if (d.sparks) (void)d.sparks->SpawnEffect(level, P2, e);   // D+0xA4 vf20
        AiNoise(level, P2, 1.0f);                     // (S+0x114)->0x00031420(&P2, 1.0f)
        if (GameObject* c = hitObj->owner) {          // vf24
            if (d.flag85) damage = F(DamageFalloff(F(double(speed) * time)) * damage);   // 0x00077D00(F(+0x48 +0x24)) +0x148
            if (c->def->IsType(1)) {                  // a character
                auto* ch = static_cast<Character*>(c);
                if (level.IsCharacter(shooter)) shooter->DealDamage(ch, damage);   // 0x00033EE0; vfCC
                else ch->Damage(damage, nullptr);     // vf7C
                if (ch->CanBePushed()) {              // vfA4
                    f32 m = damage;                   // FLD +0x148; FST [esp+0x14]
                    if (m > 250.0f) m = 250.0f;       // FCOMP [0x0029CFF0]; TEST AH,0x41; JNE
                    const NiPoint3 v{F(2.0 * dv.x), F(2.0 * dv.y), F(2.0 * dv.z)};   // 0x000115C0(2.0f, &d)
                    NiPoint3 push{F(double(v.x) * m), F((double(v.y) + 0.25f) * m), F(double(v.z) * m)};   // [0x00293B50]
                    if (ch->def->IsType(2)) {
                        push.y = 0.0f;                // the player
                    } else if (push.y > 1.0f) {       // TEST AH,0x41; JNE
                        push.y = 1.0f;
                    } else if (push.y < 0.0f) {       // TEST AH,5; JP
                        push.y = 0.0f;
                    }
                    if (ch->body) {
                        const NiPoint3 vel = ch->body->velocity;   // body vf0C
                        const f32 vz = F(double(vel.z) * 0.25f);   // (FSTP first)
                        push = {F(double(push.x) - double(vel.x) * 0.25f), F(double(push.y) - double(vel.y) * 0.25f),
                                F(double(push.z) - vz)};
                        AddBodyVelocity(*ch->body, push);   // body vf50
                    }
                }
            } else if (c->def->IsType(0x40)) {        // 0x00078679 (only DestructableDef answers 0x40)
                if (auto* ds = dynamic_cast<Destructable*>(c)) (void)ds->Damage(damage);   // vf64(+0x148)
                (void)level.Sound().Play3D(d.sound58, P2, false);   // Sound_Play3D 0x000786DA
            } else if (c->def->IsType(0x80)) {        // 0x0007869C (L: no shipped definition answers 0x80: the
                                                      //  Moveable / Debris definitions' slot 3 is false for all)
                const NiPoint3 imp{F(double(1000.0f) * dv.x), F(double(1000.0f) * dv.y), F(double(1000.0f) * dv.z)};
                if (auto* mv = dynamic_cast<Moveable*>(c)) mv->ApplyImpulse(P2, imp);   // vf50(&P2, &imp) 0x000115C0
                (void)level.Sound().Play3D(d.sound58, P2, false);   // Sound_Play3D 0x000786DA
            }
        }
        if (d.flag86) NotPorted("Laser: the camera shake (D+0x86 && S+0x44: (S+0x44)->vf20(S, &P2, 0, 0, 1))");
        if (label) {                                  // 0x00078717: the labelled shots' impact effect and its scale
            const char* impact = nullptr;
            f32 scale = 1.0f;                         // esp+0x14
            if (crt::StrICmp(label->c_str(), "Tank_Laser_shot") == 0) {            // 0x0029D040
                impact = "Tank_Impact";                                            // 0x0029D034
            } else if (crt::StrICmp(label->c_str(), "Igner_Laser") == 0) {         // 0x00297688
                impact = "Igner_Impact";                                           // 0x0029D024
                scale = 0.5f;
            } else if (crt::StrICmp(label->c_str(), "Destructor_Laser") == 0) {    // 0x00297694
                impact = "Destructor_Impact";                                      // 0x0029D010
                scale = std::bit_cast<f32>(0x3F333333u);                           // 0.7
            }
            // Session::FindDef 0x000C3F10, then ImpactDef 0x0004EE90(S+0x5C ? S+0x5C + 4 : 0, &P2, &+0x138,
            // "Impressive" 0x0029D004) and 0x00073720(scale) on its result (no null check)
            if (impact && level.defs)
                if (auto* idef = dynamic_cast<ImpactDef*>(level.defs->Find(impact)))
                    SetNodeScale(*idef->SpawnUp(level, level.currentStage, P2, normal, "Impressive"), scale);
        }
        done = 1;                                     // +0x154
        return true;
    }
    if (double(speed) * time + radius > stageDist || mesh) {   // TEST AH,0x41; JE (NaN: no impact)
        // 6.4 the stage impact
        if (!mesh) {                                  // orig: 0x00078A99: 0x000115C0, then 0x00011500
            const double t = stageDist;
            const f32 ox = F(t * direction.x), oy = F(t * direction.y), oz = F(t * direction.z);
            p = {F(double(ox) + origin.x), F(double(oy) + origin.y), F(double(oz) + origin.z)};
        }
        const NiPoint3& n = normal;
        const NiPoint3& dv = direction;
        const double k = ((double(n.z) * dv.z + double(n.y) * dv.y) + double(dv.x) * n.x) * 2.0;   // FADD ST0,ST0
        const double kx = k * n.x;
        const f32 ky = F(k * n.y), kz = F(k * n.z);
        const double rx = double(dv.x) - kx;          // FSUBR d.x
        const double ry = double(dv.y) - ky;
        const f32 rz = F(double(dv.z) - kz);
        const f32 ex = F(rx + n.x);                   // FSTP [E+0x64]
        NiPoint3 e{F(double(ex) * 0.5f), F((ry + n.y) * 0.5f), F((double(rz) + n.z) * 0.5f)};   // [0x00293AA0]
        ni::Unitize(e);                               // orig: 0x000116A0 (length dropped)
        ni::RayHit tri = ni::RayCast(T, direction, len, false, 1);   // orig: 0x00078B84 (the stage)
        if (!tri && solid) tri = mesh;
        if (tri) {
            const u32 kind = level.footstepMaterials[tri.Flags() & 3];   // orig: S+0x160 (0x000FA320)
            if (level.defs) {
                if (Def* fx = level.defs->Find(kind == 1 ? "effect_metal" : "effect_generic"))
                    (void)fx->SpawnEffect(level, p, e);   // vf20
                Def* smoke = level.defs->Find("effect_smoke");   // (no NULL check in the original)
                if (smoke) (void)smoke->SpawnEffect(level, p, e);
            }
            if (!mesh && d.value64 != 1) {
                if (kind == 4 || kind == 5 || kind == 6) {
                    if (Def* sd = level.defs ? level.defs->Find("Shoot_Splash") : nullptr)   // 0x00078C71
                        ShootSplash(level, sd, p, direction);
                } else if (auto* mi = static_cast<MarkInstance*>(level.markInstance)) {
                    mi->AddScorch(d.value64, p, normal);   // orig: (S+0xA4) 0x000730C0
                }
            }
            if (label) {                              // 0x00078D2B: an impact effect for these labels only
                Def* fd = nullptr;                    // [esp+0x24] = 0
                f32 scale = 1.0f;                     // [esp+0x34]
                auto is = [&](const char* s) { return crt::StrICmp(label->c_str(), s) == 0; };
                auto find = [&](const char* s) { return level.defs ? level.defs->Find(s) : nullptr; };   // 0x000C3F10
                if (is("Pistol-Laser")) fd = find("Pistol_Impact");           // 0x0029CFA0 / 0x0029CF90
                else if (is("Shotgun-Laser")) fd = find("Shotgun_Impact");    // 0x002976D8 / 0x0029CF58
                else if (is("Tommy-Laser")) fd = find("Tommygun_Impact");     // 0x0029CF84 / 0x0029CF74
                else if (is("RailLaser")) fd = find("Shotgun_Impact");        // 0x0029CF68
                else if (is("Tank_Laser_shot")) fd = find("Tank_Impact");     // 0x0029D040 / 0x0029D034
                else if (is("Igner_Laser")) fd = find("Igner_Impact"), scale = 0.5f;   // 0x00297688 / 0x0029D024
                else if (is("Destructor_Laser"))
                    fd = find("Destructor_Impact"), scale = std::bit_cast<f32>(0x3F333333u);   // 0x00297694 / 0x0029D010
                if (kind != 5 && kind != 6 && kind != 4 && fd) {   // (not on liquids)
                    const double ax = double(direction.x) * std::bit_cast<f32>(0x3CA3D70Au);   // FMUL [0x00294360] 0.02
                    const double ay = double(direction.y) * std::bit_cast<f32>(0x3CA3D70Au);
                    const f32 az = F(double(direction.z) * std::bit_cast<f32>(0x3CA3D70Au));   // FSTP [esp+0x6C]
                    const NiPoint3 q{F(double(p.x) - ax), F(double(p.y) - ay), F(double(p.z) - az)};   // backed off
                    if (auto* idef = dynamic_cast<ImpactDef*>(fd))   // (no type test in the original)
                        if (Impact* o = idef->SpawnUp(level, level.currentStage, q, normal, "Impressive"))   // 0x0004EE90
                            SetNodeScale(*o, scale);  // 0x00073720 (no null test in the original)
                }
            }
        }
        if (d.flag86) NotPorted("Laser: the camera shake (D+0x86 && S+0x44: (S+0x44)->vf20(S, &p, 0, 0, 1))");
        if (d.value88 && normal.y > 0.87654f) NotPorted("Laser: the LaserShock impact (D+0x88 vf14)");
        done = 1;
        (void)level.Sound().Play3D(d.sound58, p, false);   // Sound_Play3D 0x00078F9C (a NULL sample returns at once)
        return true;
    }
    // 6.3 the move
    if (spin && model) model->local.rotate = ni::MakeXRotation(F(double(time) * 5.0f));   // 0x00078831 (no null test)
    node->local.translate = p;
    UpdateNodeAt(node.Get(), time);                   // orig: NiAVObject::Update(time, 1) 0x001018D0
    if (d.ripple8C) {                                 // 0x000788BE: the pistol's "RippleLaser"
        auto* rd = dynamic_cast<RippleDef*>(d.ripple8C);
        if (rd && model) {                            // (+0x44 is not null-checked by the original)
            rd->value24 = {colour[0], colour[1], colour[2]};   // D+0x8C +0x24..+0x2C = +0x100..+0x108 (FLD / FSTP)
            rd->value30 = 1;                          // +0x30
            const f32 kPi = std::bit_cast<f32>(0x40490FDBu);   // [0x002F9274] (PI at run time)
            const ni::NiMatrix3 Rz = ni::MakeZRotation(F(double(kPi) * 0.5f));   // FMUL [0x00293AA0]; 0x001030F0
            const NiPoint3 s2{F(2.0 * direction.x), F(2.0 * direction.y), F(2.0 * direction.z)};   // 0x000115C0(.., 2.0, +0x12C)
            const NiPoint3& mt = model->world.translate;   // +0x44 -> +0xA0
            const NiPoint3 at{F(double(s2.x) + mt.x), F(double(s2.y) + mt.y), F(double(s2.z) + mt.z)};   // 0x00011500
            const ni::NiMatrix3 R = ni::Multiply(model->world.rotate, Rz);   // 0x00013B30 (+0x70 * Rz)
            (void)rd->SpawnRipple(level, owner, at, R, d.value90, 0.5f, 0.5f, d.value94, d.value94,
                                  3.40282347e38f);    // 0x00071B10 (owner +4)
        }
        // (rd: the original calls 0x00071B10 on D+0x8C without a type test; a model always exists since Init)
    }
    if (trailTimer > -100.0f) {                       // 0x000789AF: FCOMP [0x29A4E4]; TEST AH,0x41; JNE
        // the trail: a "Cannon_" laser's own emitter (D+0x34) every 0.33 s, any other's effect_firetrail every 0.5 s
        const double x = double(trailTimer) + frameTime;   // FLD +0x38; FADD +0x158; FST +0x38
        trailTimer = F(x);
        const char* n = def->name.empty() ? nullptr : def->name.c_str();   // D+0x10
        Def* e = nullptr;
        bool reached = false;
        if (x > double(std::bit_cast<f32>(0x3EA8F5C3u)) && n && std::strncmp(n, "Cannon_", 7) == 0) {   // [0x29CFEC]
            e = static_cast<const LaserDef&>(*def).emitter34;   // 0x0029CFE4
            reached = true;
        } else if (trailTimer > 0.5f) {               // FLD +0x38 (stored); FCOMP [0x00293AA0]; TEST AH,0x41; JNE
            e = level.defs ? level.defs->Find("effect_firetrail") : nullptr;   // 0x000C3F10 (0x0029CFD0)
            reached = true;
        }
        if (reached) {
            trailTimer = 0.0f;                        // +0x38 = 0 (also without a definition)
            if (e) {                                  // vf20(&the node's world point, &UNIT_Y 0x002D77EC)
                auto* o = static_cast<EmitterInstance*>(e->SpawnEffect(level, node->world.translate, {0.0f, 1.0f, 0.0f}));
                if (o) o->sourceA = node.Get();       // NiPointer::Assign(+0x40, +0x2C) 0x0002A400 (no null test)
            }
        }
    }
    lastTime = time;
    frameTime = 0.0f;
    return true;
}

}  // namespace fut::game

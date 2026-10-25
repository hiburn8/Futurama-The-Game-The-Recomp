// Static objects; see Static.h.
#include "game/object/Static.h"

#include "core/CrtRand.h"
#include "engine/ni/NiClone.h"
#include "engine/ni/NiCollision.h"
#include "engine/ni/NiMath.h"
#include "game/GameRandom.h"
#include "game/GameTime.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/level/LevelData.h"
#include "game/level/LightManager.h"
#include "game/level/Stage.h"
#include "game/object/Character.h"
#include "game/sound/GameSound.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fut::game {

using ni::detail::F;

namespace {
int StrNICmp(const char* a, const char* b, size_t n) {   // orig: _strnicmp (0x00170B4A; C locale)
    for (; n; --n, ++a, ++b) {
        int ca = static_cast<unsigned char>(*a), cb = static_cast<unsigned char>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

void NotPorted(const char* what) {                    // (the per-file pattern of Pickup.cpp / Stage.cpp)
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}


constexpr f32 kSmall = 0.03f;                         // orig: 0x00293AA4
constexpr f32 kRest = 0.001f;                         // orig: 0x00293AC4
constexpr u32 kFltMaxBits = 0x7F7FFFFFu;

using ni::Cross;                                      // orig: 0x00011700 NiPoint3::Cross (NiMath.h)

// (x*x + y*y) + z*z, the x87 order of 0x000945F0's rest tests
double Len2(const ni::NiPoint3& v) { return (double(v.x) * v.x + double(v.y) * v.y) + double(v.z) * v.z; }
}  // namespace

Static::Static(LevelData& lvl, StaticDef& d) : GameObject(lvl, d) {   // orig: 0x00095110
    // +0xD8 / +0xDC are never written by the constructor: heap leftovers in the original (the tests' hook), 0 here
}

Static::~Static() {                                   // orig: 0x00095220
    // orig: 0x00095256: a Polygon static's records leave its stage's NiBsp (0x000FC2D0).
    if (owner && static_cast<StaticDef&>(*def).shape == StaticDef::kPolygon)
        if (auto* stage = dynamic_cast<Stage*>(owner); stage && stage->bsp)
            ni::RemoveCollisionMeshes(*stage->bsp, node.Get());
}

void Static::SetOwner(Owner* newOwner) {              // orig: 0x00094560
    // (the test is on the current owner: an object owned by anything but a stage keeps its owner)
    if (owner && !owner->IsOwnerType(2)) return;
    GameObject::SetOwner(newOwner);
}


bool Static::Init() {                                 // orig: 0x00095320
    auto& d = static_cast<StaticDef&>(*def);
    if (model) return true;
    ni::NiPointer<ni::NiObject> c = ni::Clone(d.model.Get());   // orig: 0x000FE120
    model = dynamic_cast<ni::NiNode*>(c.Get());
    node->AttachChild(model.Get(), true);
    model->flags &= ~ni::kNiAVHidden;
    model->local.translate = {};
    model->local.rotate = {};
    model->flags = static_cast<u16>(model->flags & 0xFFF1);   // orig: 0x00101B80(0)
    model->UpdateProperties();                        // orig: 0x000953ED
    node->UpdateDownwardPass(time, true);             // orig: NiAVObject::Update(time, true) 0x001018D0
    if (d.shape == StaticDef::kBox) {
        const ni::NiMatrix3 r = GetWorldRotation();
        if (StrNICmp(d.name.c_str(), "seesaw_", 7) == 0) seesaw = true;   // _strnicmp 0x00170B4A
        seesawAxis = ni::MulVector(r, {1, 0, 0});    // orig: 0x00011770
        ni::Unitize(seesawAxis);                      // orig: 0x000116A0 (length discarded)
        // orig: +0x38 = new(0x30) 0x000AF9B0(D+0x1C, D+0x20, D+0x24, 10.0); SetOwner(this), SetRotation(world
        // rotation), SetPosition(world position + the definition's box centre); body+0x150 = 1 (static).
        const ni::NiPoint3 wp = GetWorldPosition();   // orig: vf34
        box = level.NewCollisionBox(d.boxExtents[0], d.boxExtents[1], d.boxExtents[2], 10.0f);
        box->owner = this;
        box->SetRotation(r);
        box->SetPosition({F(double(wp.x) + d.boxCenter[0]), F(double(d.boxCenter[1]) + wp.y),
                          F(double(d.boxCenter[2]) + wp.z)});
        box->box.isStatic = 1;
    }
    // orig: 0x00095522: a Polygon static's triangles become collision mesh records of its stage's NiBsp (owner +0x20 =
    // Stage +0x24), flagged with the definition's byte +0x35; the next RefreshAllBsps inserts them.
    if (owner && d.shape == StaticDef::kPolygon)
        if (auto* stage = dynamic_cast<Stage*>(owner); stage && stage->bsp)   // (port guards)
            ni::AddCollisionMeshes(*stage->bsp, node.Get(), d.shapeFlag);   // 0x000FC110
    CreateAmbientLight();
    InitAmbientLight();
    if (level.lightManager) level.lightManager->AddTarget({0, 0, 0}, node.Get());   // orig: DAT_002EA824 vf00
    return true;
}

GameObject* StaticDef::Spawn(LevelData& level, Owner* owner, const ni::NiPoint3& pos, const ni::NiMatrix3& rot,
                             const char* label) {     // orig: 0x00095780
    if (!owner || !owner->IsOwnerType(2)) return nullptr;
    if (!Ready()) return nullptr;
    auto* o = new Static(level, *this);
    o->SetOwner(owner);
    o->SetName(label);
    o->SetPosition(pos);
    o->SetRotation(rot);
    if (model) o->Init();
    return o;
}

void Static::SetPosition(const ni::NiPoint3& p) {    // orig: 0x00094FF0
    basePosition = p;
    const ni::NiPoint3 local{p.x, F(double(p.y) + verticalOffset), p.z};   // FLD p.y; FADD [+0x5C]; FSTP
    GameObject::SetPosition(local);
    if (!box) return;
    const auto& c = static_cast<StaticDef&>(*def).boxCenter;
    const ni::NiMatrix3 r = GetWorldRotation();       // vf38
    const auto& R = r.m;
    const double X = (double(c[1]) * R[0][1] + double(c[2]) * R[0][2]) + double(c[0]) * R[0][0];   // kept
    const double Y = (double(c[1]) * R[1][1] + double(c[0]) * R[1][0]) + double(c[2]) * R[1][2];   // kept
    const f32 Z = F((double(c[1]) * R[2][1] + double(c[0]) * R[2][0]) + double(c[2]) * R[2][2]);
    box->SetPosition({F(double(local.x) + X), F(Y + local.y), F(double(local.z) + Z)});   // vf04
}

void Static::SetRotation(const ni::NiMatrix3& m) {   // orig: 0x000955D0
    ni::NiMatrix3 tmp = m;
    baseRotation = m;
    ni::NiPoint3 axis = angularOffset;
    const double len = ni::Unitize(axis);             // 0x000116A0 (ST0)
    if (len > double(1e-4f))                          // FCOMP [0x00293B1C]; TEST AH,0x41; JNE
        tmp = ni::Multiply(ni::AxisAngle(F(len), axis.x, axis.y, axis.z), m);   // 0x00103130, 0x00013B30
    GameObject::SetRotation(tmp);
    if (box) box->SetRotation(tmp);                   // vf38
}

void Static::ResetPhysics() {                         // orig: vf50 0x00094590
    verticalOffset = verticalVelocity = prevAcceleration = splash = groundOffset = 0.0f;
    rippleTimer = 3.4028234663852886e38f;
    angularOffset = {};
    angularVelocity = {};
}

void Static::SetFollowTarget(ni::NiAVObject* t) {    // orig: 0x000956C0
    if (follow && t) return;
    if (follow.Get() != t) follow = t;
    if (!t) return;
    const ni::NiPoint3& w = t->world.translate;
    followOffset = {F(double(basePosition.x) - w.x), F(double(basePosition.y) - w.y),
                    F(double(basePosition.z) - w.z)};
}

bool Static::PostUpdate() {                           // orig: 0x00094F50
    const auto& d = static_cast<StaticDef&>(*def);
    if (follow) {
        const ni::NiPoint3& t = follow->world.translate;
        SetPosition({F(double(t.x) + followOffset.x), F(double(t.y) + followOffset.y),
                     F(double(t.z) + followOffset.z)});   // vf1C
        UpdateNode();                                 // node->Update(time, 1)
        return true;
    }
    if (d.animated || (d.physicsOn && box)) UpdateNode();
    return true;
}

bool Static::Tick() {                                 // orig: 0x000945F0
    const auto& d = static_cast<StaticDef&>(*def);
    if (d.physicsOn && box) {
        const auto& P = d.physics;                    // +0x60..+0x7C
        const double dt = g_frameStep;
        phys::RigidBox& B = box->box;                 // (box+0x28)

        // 2.1 rotational spring
        const f32 kr = F(double(P[5]) * P[5] + double(P[4]) * P[4]);
        ni::NiPoint3& w = angularVelocity;
        ni::NiPoint3& o = angularOffset;
        const double w2 = Len2(w);
        const double damp = w2 < double(kSmall) ? 0.0 : double(P[4]);   // TEST AH,5; JP
        const double d2 = damp + damp;
        const double tx = d2 * w.x;                   // kept
        const f32 ty = F(d2 * w.y), tz = F(d2 * w.z);
        const double nk = -double(kr);
        ni::NiPoint3 acc{F(F(nk * o.x) - tx), F(double(F(nk * o.y)) - ty), F(double(F(nk * o.z)) - tz)};

        // 2.2 rest kick
        if (Len2(w) < double(kRest) && Len2(o) < double(kRest) &&
            (double(acc.x) * acc.x + double(acc.z) * acc.z) + double(acc.y) * acc.y < double(kRest)) {
            const f32 r1 = F(RandUnit());             // orig: 0x00094762 (RNG)
            const double r2 = RandUnit();             // orig: 0x0009476B (RNG)
            const ni::NiPoint3 kick{F((r2 + 1.0) * 10.0), 0.0f, F(double(r1) * 10.0)};
            const ni::NiPoint3 t{F(double(P[6]) * kick.x), F(double(P[6]) * kick.y), F(double(P[6]) * kick.z)};
            acc = {F(double(acc.x) - t.x), F(double(acc.y) - t.y), F(double(acc.z) - t.z)};   // 0x00011620
        }

        // 2.3 a character standing on the box
        if ((B.flags & 0x10) && B.standing) {
            const ni::NiPoint3 wp = GetWorldPosition();   // vf34
            const ni::NiPoint3& c = B.standing->pos;
            const ni::NiPoint3 dv{F(double(c.x) - wp.x), F(double(c.y) - wp.y), F(double(c.z) - wp.z)};
            const ni::NiPoint3 x = Cross(dv, {0.0f, 1.0f, 0.0f});
            const f32 nx = -x.x, ny = -x.y, nz = -x.z;   // FCHS
            acc.x = F(double(acc.x) - F(double(nx) * P[6]));
            acc.y = F(double(acc.y) - double(ny) * P[6]);
            acc.z = F(double(acc.z) - double(nz) * P[6]);
        }

        // 2.4 rotational integration
        {
            const double ax = acc.x * dt, ay = acc.y * dt;
            const f32 az = F(acc.z * dt);
            w = {F(ax + w.x), F(ay + w.y), F(double(az) + w.z)};
        }
        auto project = [&](ni::NiPoint3& v) {         // seesaw: onto the axis
            const ni::NiPoint3& n = seesawAxis;
            const double s = (double(n.z) * v.z + double(n.y) * v.y) + double(v.x) * n.x;
            v = {F(s * n.x), F(s * n.y), F(s * n.z)};
        };
        if (seesaw) project(w);
        {
            const double ox = dt * w.x, oy = dt * w.y;
            const f32 oz = F(dt * w.z);
            o = {F(ox + o.x), F(oy + o.y), F(double(oz) + o.z)};
        }
        if (seesaw) project(o);
        SetRotation(baseRotation);                    // vf24 (copies the argument first)

        const bool gate = d.ripple && node->framesNotDrawn == 0;   // orig: D+0x3C && node+0xB2 == 0

        // 2.5 sounds
        if (gate) {
            if ((B.flags & 0x10) && B.standing) {
                if (!seesaw && !jumpSoundPlayed) {
                    jumpSoundPlayed = true;
                    if (!jumpVoice || !level.Sound().VoiceExists(jumpVoice)) {   // +0xDC; Voice_Exists 0x00094A3A
                        const ni::NiPoint3 wp = GetWorldPosition();
                        const u32 n = static_cast<u32>(d.jumpSounds.size());   // orig: NiTArray count +0x50
                        const u32 i = static_cast<u32>(crt::Rand()) % n;       // orig: 0x00094A58 (RNG; DIV)
                        jumpVoice = level.Sound().Play3D(d.jumpSounds[i], wp, false);   // Sound_Play3D 0x00094A72
                    }
                }
            } else {
                jumpSoundPlayed = false;
            }
            const ni::NiPoint3 wp = GetWorldPosition();
            const ni::NiPoint3 pp = level.player->GetWorldPosition();   // S+0x94 vf34
            const double dx = double(pp.x) - wp.x, dy = double(pp.y) - wp.y, dz = double(pp.z) - wp.z;
            if ((dz * dz + dy * dy) + dx * dx < 3600.0) {   // [0x00294378]; TEST AH,5; JP
                if (!idleVoice || !level.Sound().VoiceExists(idleVoice)) {   // +0xD8; 0x00094AED
                    idleVoice = level.Sound().Play3D(d.idleSound, wp, true);   // D+0x40, loop; 0x00094B07
                    level.Sound().SetMaxDistance(idleVoice, 15.0f);   // Voice_SetMaxDistance 0x00094B18 (0x41700000)
                }
            } else if (idleVoice && level.Sound().VoiceExists(idleVoice)) {   // 0x00094B27
                level.Sound().StopVoice(idleVoice);       // Voice_Stop 0x00094B3A
                idleVoice = 0;
            }
        }

        // 2.6 vertical spring
        const f32 kv = F(double(P[1]) * P[1] + double(P[0]) * P[0]);
        const double dampV = std::fabs(verticalVelocity) < 1.0f ? 0.0 : double(P[0]);
        f32 accV = F(-(double(kv) * verticalOffset) - (dampV * verticalVelocity) * 2.0);
        if (B.flags & 0x10) accV = F(double(accV) - P[2]);
        if (std::fabs(verticalVelocity) < kSmall && std::fabs(accV) < kSmall && std::fabs(verticalOffset) < kSmall)
            accV = F(double(accV) - P[2]);
        const double vd = double(accV) * dt + verticalVelocity;
        verticalVelocity = F(vd);                     // FST: vd stays on the stack
        const double yd = vd * dt + verticalOffset;
        verticalOffset = F(yd);
        if (yd < -double(P[3])) {                     // FCOMPP with the unrounded yd
            verticalOffset = F(double(1e-5f) - P[3]); // [0x002A0FE8]
            if (verticalVelocity < 0.0f) verticalVelocity = F(double(verticalVelocity) * -0.6f);   // [0x0029F990]
        }

        // 2.7 ground probe, splash, ripple
        if (gate) {
            u32 bits;
            std::memcpy(&bits, &rippleTimer, 4);
            if (bits == kFltMaxBits) {
                rippleTimer = 0.0f;
                f32 maxd = F(double(B.ext.y) + B.ext.y);
                const ni::NiPoint3 start{basePosition.x, F(double(B.ext.y) + basePosition.y), basePosition.z};
                (void)ni::RayCast(start, {0.0f, -1.0f, 0.0f}, maxd, true, 3);   // orig: 0x000FB780
                groundOffset = F((double(B.ext.y) - maxd) + 0.1f);
            }
            const f32 av = std::fabs(verticalVelocity);
            if (av > kSmall && accV > 0.0f && prevAcceleration < 0.0f) {
                if (av > splash) splash = av;
                if (splash > 1.0f) splash = 1.0f;
            }
            if (splash > 0.1f && !(rippleTimer > 0.0f)) {   // TEST AH,0x41; JP
                // orig: 0x00094D71..0x00094E52 (static_tick_port.md 2.7; inventory_batch1_port.md 4.5)
                ni::NiMatrix3 R = GetWorldRotation(); // vf38
                const f32 sx = F(double(B.ext.x) * 1.1f);   // B +0x160; FMUL [0x00295644]
                const f32 sz = F(double(B.ext.z) * 1.1f);   // B +0x168
                const f32 ga = F((double(B.ext.x) * splash) * 1.5f + sx);   // FMUL +0x68; FMUL [0x00293BEC]; FADD
                const f32 gb = F((double(B.ext.z) * splash) * 1.5f + sz);
                f32 ex = 0, ey = 0, ez = 0;
                (void)ni::ToEulerAnglesXYZ(R, ex, ey, ez);   // 0x00103680
                R = ni::FromEulerAnglesXYZ(ex, 0.0f, 0.0f);   // 0x00103FA0
                const ni::NiPoint3 pos{basePosition.x, F(double(basePosition.y) + groundOffset), basePosition.z};
                rippleTimer = 1.0f;                   // +0x6C (before the spawn)
                if (RippleDef* rd = d.ripple)         // D+0x3C
                    (void)rd->SpawnRipple(level, owner, pos, R, splash, sx, sz, ga, gb, 3.40282347e38f);   // 0x00071B10
            }
            if (splash > 0.0f) splash = F(double(splash) - dt);
            prevAcceleration = accV;
            rippleTimer = F(double(rippleTimer) - dt);
        }

        // 2.8 placement
        const ni::NiPoint3 local{basePosition.x, F(double(basePosition.y) + verticalOffset), basePosition.z};
        node->local.translate = local;                // no Update, no UpdateRoom
        const ni::NiPoint3 c = ni::MulVector(GetWorldRotation(), {d.boxCenter[0], d.boxCenter[1], d.boxCenter[2]});
        box->SetPosition({F(double(local.x) + c.x), F(double(local.y) + c.y), F(double(local.z) + c.z)});   // vf04
    }
    time = F(double(g_frameStep) + time);             // 0x00094F39
    return true;
}

}  // namespace fut::game

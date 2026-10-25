// Moveable / Debris; see Moveable.h and docs/analysis/destructable_moveable_port.md 4 (section numbers below).
#include "game/object/Moveable.h"

#include "engine/ni/NiClone.h"
#include "engine/ni/NiMath.h"
#include "game/GameRandom.h"
#include "game/GameTime.h"
#include "game/defs/Defs.h"
#include "game/level/LevelData.h"
#include "game/sound/GameSound.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

}  // namespace

// ---- the definitions' spawns (1.4) --------------------------------------------------------------------------------
// orig: MoveableDef::Spawn 0x00074230 / DebrisDef::Spawn 0x0007FCB0 (the same shape): owner->vf0C(2) (a stage) and
// Ready, the object, SetOwner, SetName, SetPosition, SetRotation, Init when the definition has a model.
namespace {

template <class T>
GameObject* SpawnMoveable(MoveableDef& d, LevelData& level, Owner* owner, const NiPoint3& pos,
                          const ni::NiMatrix3& rot, const char* label) {
    if (!owner) return nullptr;                       // (not null-checked in the original: a crash)
    if (!owner->IsOwnerType(2) || !d.Ready()) return nullptr;
    auto* o = new T(level, d);                        // new(0x4C) 0x00073F00 / new(0x50) 0x0007F900
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C (Moveable 0x00073D80)
    o->SetRotation(rot);                              // vf24
    if (d.model) o->Init();                           // D+0x2C: vf04
    return o;
}

}  // namespace

GameObject* MoveableDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                               const char* label) {   // orig: 0x00074230
    return SpawnMoveable<Moveable>(*this, level, owner, pos, rot, label);
}

GameObject* DebrisDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                             const char* label) {     // orig: 0x0007FCB0
    return SpawnMoveable<Debris>(*this, level, owner, pos, rot, label);
}

// ---- Moveable (4.1, 4.2) ------------------------------------------------------------------------------------------
Moveable::Moveable(LevelData& lvl, MoveableDef& d) : GameObject(lvl, d) {}   // orig: 0x00073F00 (+0x38..+0x48 = 0)

Moveable::~Moveable() {                               // orig: 0x00073F80
    if (model) {
        (void)node->DetachChild(model.Get());         // vfA4 (the returned reference released)
        model = nullptr;
    }
    body.reset();                                     // +0x38 vf00(1); +0x38 = 0
}

void Moveable::CloneModel() {
    auto& d = static_cast<MoveableDef&>(*def);
    ni::NiPointer<ni::NiObject> c = ni::Clone(d.model.Get());   // NiObject::Clone 0x000FE120
    model = dynamic_cast<ni::NiNode*>(c.Get());       // DynamicCast NiNode (0x002F9230)
    node->AttachChild(model.Get(), true);             // vfA0(model, 1) (no null test of the clone)
    model->flags &= ~ni::kNiAVHidden;                 // AND byte [+0xB0], 0xFE
    model->local.translate = {};                      // ZERO (0x002F915C)
    model->local.rotate = {};                         // identity (0x002D7820)
    model->flags = static_cast<u16>(model->flags & 0xFFF1);   // SetCollisionBits 0x00101B80(0)
    model->UpdateProperties();                        // 0x00101EA0
    model->UpdateDownwardPass(time, true);            // NiAVObject::Update(+0x24, 1) 0x001018D0 (the clone only)
}

bool Moveable::Init() {                               // orig: vf04 0x00074050
    if (model) return true;                           // +0x44
    CloneModel();
    auto& d = static_cast<MoveableDef&>(*def);
    if (d.shape == MoveableDef::kBox)                 // new(0x30) 0x000AF9B0(D+0x1C, D+0x20, D+0x24, D+0x28)
        body = level.NewCollisionBox(d.size[0], d.size[1], d.size[2], d.mass);
    else if (d.shape == MoveableDef::kSphere)
        NotPorted("Moveable: the sphere shape 0x000B0040 (the original leaves its body uninitialised)");
    if (!body) return true;                           // (the original calls through a null +0x38: a UB guard)
    body->owner = this;                               // vf28
    body->box.noRotation = 1;                         // body +0x151
    const ni::NiMatrix3 r = GetWorldRotation();       // vf38
    const NiPoint3 p = GetWorldPosition();            // vf34
    body->SetRotation(r);                             // vf38
    body->SetPosition(p);                             // vf04
    return true;
}

bool Moveable::Tick() {                               // orig: vf08 0x00073DA0
    sinceUpdate = F(double(g_frameStep) + sinceUpdate);   // +0x48
    time = F(double(g_frameStep) + time);             // +0x24
    return true;
}

void Moveable::SetPosition(const NiPoint3& p) {       // orig: vf1C 0x00073D80
    GameObject::SetPosition(p);                       // 0x00097140
    if (body) body->SetPosition(p);                   // vf04
}

void Moveable::ApplyImpulse(const NiPoint3& point, const NiPoint3& impulse) {   // orig: vf50 0x00073D70
    if (body) body->ApplyImpulseAt(point, impulse);   // vf40 (no null test in the original: a UB guard)
}

bool Moveable::PostUpdate() {                         // orig: vf0C 0x00073DC0 (4.2)
    if (!body) {
        sinceUpdate = 0;
        return true;
    }
    const ni::NiMatrix3 r = body->GetRotation();      // vf3C
    const NiPoint3 p = body->GetPosition();           // vf08
    node->local.translate = p;
    node->local.rotate = r;
    node->UpdateDownwardPass(time, true);             // 0x001018D0(+0x24, 1)
    // (H) the box's 0x20 (a hard impact) is set and cleared inside the physics step: never seen here
    // (simplenpc_moveable_port.md 3.1); kept as written.
    if (body->box.flags & 0x20) {
        sound::GameSound& snd = level.Sound();
        (void)snd.Play3D(snd.Load("sounds/generic/movable_impact.wav", 0), p, false);   // 0x000C9860 / 0x000C9AC0
    }
    const NiPoint3& v = body->box.v;                  // body +0x110
    const f32 v2 = F((double(v.x) * v.x + double(v.y) * v.y) + double(v.z) * v.z);   // FSTP [esp+0x10]
    if (body->box.flags & 4) {                        // (likewise cleared by the step's solver)
        const double cd = double(cooldown) - g_frameStep;   // FLD [+0x40]; FSUB; FST
        cooldown = F(cd);
        if (std::sqrt(double(v2)) > 1.0f) {           // FCOMP [1.0]; TEST AH,0x41; JNE
            cooldown = 0.2f;                          // 0x3E4CCCCD
            sinceUpdate = 0;
            return true;
        }
        if (cd < 0.0) {                               // (the unrounded value; TEST AH,5; JP)
            level.Sound().StopVoice(static_cast<s32>(voice));   // Voice_Stop 0x000C9B10 (no Voice_Exists)
            voice = 0;
        }
    } else {
        level.Sound().StopVoice(static_cast<s32>(voice));
        voice = 0;
    }
    sinceUpdate = 0;
    return true;
}

// ---- Debris (4.3) -------------------------------------------------------------------------------------------------
bool Debris::Init() {                                 // orig: vf04 0x0007F9F0
    if (model) return true;                           // +0x44
    CloneModel();
    auto& d = static_cast<MoveableDef&>(*def);
    if (d.shape == MoveableDef::kBox) {               // (D+0x28 is not used: the mass from the size)
        const f32 mass = F(((double(d.size[2]) * d.size[1]) * d.size[0]) * 8.0f);   // [0x00293C04]
        body = level.NewCollisionBox(d.size[0], d.size[1], d.size[2], mass);   // 0x000AF9B0
    } else if (d.shape == MoveableDef::kSphere) {
        NotPorted("Debris: the sphere shape 0x000B0040 (the original leaves its body uninitialised)");
    }
    ni::NiMatrix3 r = node->local.rotate;             // node +0x30
    const NiPoint3 p = node->local.translate;         // node +0x60
    const f32 az = F(RandUnit() * 6.0f);              // rand #1 -> the 3rd argument ([0x0029C094])
    const f32 ay = F(RandUnit() * 6.0f);              // rand #2 -> the 2nd
    const f32 ax = F(RandUnit() * 6.0f);              // rand #3 -> the 1st
    r = ni::EulerXYZProduct(ax, ay, az);              // 0x00103E10
    if (!body) return true;                           // (a null +0x38 crashes the original: a UB guard)
    body->SetRotation(r);                             // vf38
    body->SetPosition(p);                             // vf04
    return true;                                      // (no owner, no +0x151, no +0x150: a rotating dynamic box)
}

bool Debris::PostUpdate() {                           // orig: vf0C 0x0007F950
    if (!body) return true;
    const ni::NiMatrix3 r = body->GetRotation();      // vf3C
    const NiPoint3 p = body->GetPosition();           // vf08
    node->local.translate = p;
    node->local.rotate = r;
    node->UpdateDownwardPass(time, true);             // 0x001018D0(+0x24, 1)
    if (body->IsAtRest()) {                           // vf44 0x000AF910
        const s32 n = restFrames++;
        if (n > 10) body.reset();                     // JLE: the box deleted on the 12th post-update at rest
    }
    return true;
}

}  // namespace fut::game

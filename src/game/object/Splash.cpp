// The Splash effect object; see Splash.h and docs/analysis/inventory_batch1_port.md 4.1 (checked against the assembly
// of 0x00073880, 0x00073CA0, 0x00073970, 0x00073A80, 0x00073700, 0x00073B90 and 0x000739E0).
#include "game/object/Splash.h"

#include "engine/ni/NiClone.h"
#include "engine/ni/NiMath.h"
#include "game/GameTime.h"
#include "game/defs/EffectDefs.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/object/NodeUpdate.h"

#include <cmath>
#include <utility>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

// ---- the spawns ------------------------------------------------------------------------------------------------------
GameObject* SplashDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                             const char* label) {     // orig: vf14 0x00073CA0 (RET 0x10)
    auto* o = new Splash(level, *this);               // operator_new(0x54), ctor 0x00073970 (no null check after)
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C 0x00097140 (UpdateRoom)
    o->billboard = 0;                                 // +0x48 (byte), before vf24
    o->SetRotation(rot);                              // vf24 0x00096A30
    if (mesh) o->Init();                              // D+0x18 -> vf04
    return o;
}

// orig: 0x00073880 (thiscall(owner, NiPoint3* pos, NiPoint3* dir, label), RET 0x10). The direction is not used: *dir
// is overwritten with UNIT_Z (0x002D77F8) and the rotation's columns are (-UNIT_X, UNIT_Y, the new *dir).
GameObject* SplashDef::SpawnDir(LevelData& level, Owner* owner, const NiPoint3& pos, NiPoint3& dir,
                                const char* label) {
    ni::NiMatrix3 R;                                  // esp+0x1C (pads +0x28 / +0x38 / +0x48 = 0)
    R.m[0][0] = -1.0f, R.m[1][0] = -0.0f, R.m[2][0] = -0.0f;   // chunk 0: FLD UNIT_X; FCHS per element
    R.m[0][1] = 0.0f, R.m[1][1] = 1.0f, R.m[2][1] = 0.0f;      // chunk 1: UNIT_Y (dwords)
    dir = {0.0f, 0.0f, 1.0f};                         // *dir = UNIT_Z (dwords; the caller's variable changes)
    R.m[0][2] = dir.x, R.m[1][2] = dir.y, R.m[2][2] = dir.z;   // chunk 2: the new *dir (dword / FLD-FSTP: exact)
    return Spawn(level, owner, pos, R, label);        // vf14
}

// ---- the object -------------------------------------------------------------------------------------------------------
Splash::Splash(LevelData& lvl, SplashDef& d) : GameObject(lvl, d) {}   // orig: 0x00073970 (+0x3C / +0x40 / +0x50 = 0)

Splash::~Splash() {                                   // orig: 0x000739E0: +0x40 released (twice), then +0x3C
    follow = nullptr;
    clone = nullptr;
}

bool Splash::Init() {                                 // orig: vf04 0x00073A80 (no "built already" test)
    const auto& d = static_cast<const SplashDef&>(*def);
    ni::NiPointer<ni::NiObject> c = ni::Clone(d.mesh.Get());   // NiObject::Clone 0x000FE120
    clone = dynamic_cast<ni::NiNode*>(c.Get());       // the RTTI walk against 0x002F9230 (NiNode), else null
    if (clone) {                                      // (the original has no null checks: a NiNode mesh always)
        node->AttachChild(clone.Get(), true);         // vfA0(clone, 1)
        clone->flags &= ~ni::kNiAVHidden;             // +0xB0 &= 0xFE
        clone->local.translate = {};                  // ZERO 0x002F915C (dwords)
        clone->local.rotate = ni::NiMatrix3{};        // identity 0x002D7820 (12 dwords)
        clone->UpdateProperties();                    // 0x00073B3A
        UpdateNodeAt(clone.Get(), 0.0f);              // NiAVObject::Update(0.0, 1)
    }
    node->UpdateProperties();                         // 0x00073B4E
    node->UpdateEffects();
    node->local.scale = F(std::fabs(1.0));            // FLD QWORD [0x00293CC0]; FABS; FSTP +0x6C
    speed = d.value1C;                                // +0x44 = D+0x1C (dword)
    duration = d.value20;                             // +0x4C = D+0x20 (dword)
    UpdateNodeAt(node.Get(), F(double(time) * speed));   // FLD +0x24; FMUL +0x44; NiAVObject::Update(t, 1)
    return true;
}

bool Splash::Tick() {                                 // orig: vf08 0x00073700
    const double t = double(g_frameStep) + time;      // FLD [0x002D67E8]; FADD +0x24
    time = F(t);                                      // FST: the unrounded sum is compared
    return !(t * speed > duration);                   // FMUL +0x44; FCOMP +0x4C; TEST AH,0x41; JNE -> 1 (NaN: alive)
}

bool Splash::PostUpdate() {                           // orig: vf0C 0x00073B90 (the result is ignored)
    if (follow) {                                     // +0x40: the followed node's world transform
        node->local.translate = follow->world.translate;   // +0xA0 -> +0x60 (dwords)
        node->local.rotate = follow->world.rotate;         // +0x70 -> +0x30 (12 dwords via the stack)
    }
    if (billboard) {                                  // +0x48: the camera's world rotation (S+0x8C -> +0x30 -> +0x70)
        ni::NiMatrix3 Cs = level.camera->sceneCamera->world.rotate;   //  with chunks 0 and 2 swapped (FLD / FSTP,
        for (int j = 0; j < 3; ++j) std::swap(Cs.m[j][0], Cs.m[j][2]);   //  dword moves)
        node->local.rotate = Cs;
    }
    UpdateNodeAt(node.Get(), F(double(speed) * time));   // FLD +0x44; FMUL +0x24; NiAVObject::Update(t, 1)
    return true;
}

}  // namespace fut::game

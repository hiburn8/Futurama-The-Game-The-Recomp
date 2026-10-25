// The Impact effect object; see Impact.h and docs/analysis/player_moves_port.md 5 (checked against the assembly of
// 0x0004F710, 0x0004EE90, 0x0004F650, 0x0004EF70, 0x0004F040, 0x0004F110, 0x0004EAA0, 0x0004F2E0, 0x0004EAD0,
// 0x0004F580 and 0x00073720).
#include "game/object/Impact.h"

#include "core/CrtRand.h"
#include "engine/ni/NiClone.h"
#include "engine/ni/NiMath.h"
#include "game/GameTime.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/EffectDefs.h"
#include "game/level/Hud.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/object/NodeUpdate.h"

#include <bit>
#include <cmath>
#include <utility>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

const NiPoint3 kUnitY{0, 1, 0};                       // orig: 0x002D77EC

}  // namespace

// ---- the spawns ------------------------------------------------------------------------------------------------------
GameObject* ImpactDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                             const char* label) {     // orig: vf14 0x0004F710 (RET 0x10)
    auto* o = new Impact(level, *this);               // operator_new(0x60), ctor 0x0004EF70
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C = 0x0004EAD0 (no node update)
    o->billboard = 0;                                 // +0x55
    o->SetRotation(rot);                              // vf24 0x00096A30 (the node update)
    if (mesh) o->Init();                              // vf04, only with def+0x18
    return o;
}

// orig: 0x0004EE90. c = UY x dir: x stays on the x87 stack (FCHS of the unrounded value), y / z are stored first.
Impact* ImpactDef::SpawnUp(LevelData& level, Owner* owner, const NiPoint3& pos, const NiPoint3& dir,
                           const char* label) {
    const NiPoint3 U = kUnitY;                        // dword copies of 0x002D77EC (esp+8)
    const double cx = double(U.y) * dir.z - double(U.z) * dir.y;
    const f32 cy = F(double(U.z) * dir.x - double(U.x) * dir.z);   // FSTP [esp+0x18]
    const f32 cz = F(double(U.x) * dir.y - double(U.y) * dir.x);   // FSTP [esp+0x1C]
    ni::NiMatrix3 M;                                  // esp+0x20: chunk k = column k (pads +0x2C / +0x3C / +0x4C = 0)
    M.m[0][0] = F(-cx), M.m[1][0] = -cy, M.m[2][0] = -cz;   // FCHS; FSTP
    M.m[0][1] = U.x, M.m[1][1] = U.y, M.m[2][1] = U.z;      // dword copies
    M.m[0][2] = dir.x, M.m[1][2] = dir.y, M.m[2][2] = dir.z;
    return static_cast<Impact*>(Spawn(level, owner, pos, M, label));   // vf14 through the vtable
}

Impact* ImpactDef::SpawnBillboard(LevelData& level, Owner* owner, const NiPoint3& pos, const char* label) {
    auto* o = new Impact(level, *this);               // orig: 0x0004F650: new(0x60), ctor 0x0004EF70
    o->SetOwner(owner);                               // vf10
    o->SetName(label);
    o->SetPosition(pos);                              // vf1C
    o->billboard = 1;                                 // +0x55
    o->angle = F(double(crt::Rand() % 100) * 1.8f);   // rand 0x0004F6C2; CDQ; IDIV 100; FILD; FMUL [0x0029A250]
    if (mesh) o->Init();                              // vf04 (no SetRotation)
    return o;
}

// ---- the object -------------------------------------------------------------------------------------------------------
Impact::~Impact() {                                   // orig: 0x0004F040
    lit = nullptr;                                    // +0x44 released, 0
    follow = nullptr;                                 // +0x4C released, 0 (then the members: +0x48; the GameObject's)
}

Impact::Impact(LevelData& lvl, ImpactDef& d) : GameObject(lvl, d) {   // orig: 0x0004EF70
    time = std::bit_cast<f32>(d.unknown24);           // +0x24 = def+0x24 (dword copy)
}

bool Impact::Init() {                                 // orig: vf04 0x0004F110
    const auto& d = static_cast<const ImpactDef&>(*def);
    ni::NiPointer<ni::NiObject> c = ni::Clone(d.mesh.Get());   // NiObject::Clone 0x000FE120
    clone = dynamic_cast<ni::NiNode*>(c.Get());       // the RTTI walk against 0x002F9230 (NiNode); else null
    if (clone) {                                      // (the original has no null checks: a NiNode mesh always)
        node->AttachChild(clone.Get(), true);         // vfA0(clone, 1)
        clone->flags &= ~ni::kNiAVHidden;             // +0xB0 &= ~1
    }
    node->UpdateProperties();                         // 0x0004F1C6
    node->UpdateEffects();
    node->local.scale = F(std::fabs(1.0));            // FLD QWORD [0x00293CC0]; FABS; FSTP +0x6C
    NiPoint3 p = GetWorldPosition();                  // vf34 (slot 13: the node's world translation)
    p.y = F(double(p.y) + 1.0f);                      // FADD [0x00293A98]
    // S+0x20 0x0011F1D0 (the room's reference is released at once; the raw pointer is kept)
    const ni::NiRoom* room = level.roomGroup ? level.roomGroup->FindRoomContaining(p) : nullptr;
    stage = nullptr;                                  // +0x5C
    for (const auto& st : level.stages)               // Session::NextStage 0x000C26C0 from 0
        if (st->ContainsRoom(room)) {                 // 0x000CE410
            stage = st.get();
            SetOwner(stage);                          // vf10(stage + 4)
            break;
        }
    speed = d.timeScale;                              // +0x50 = def+0x1C
    duration = d.duration;                            // +0x58 = def+0x20
    UpdateNodeAt(node.Get(), F(double(time) * speed));   // NiAVObject::Update 0x001018D0(t, 1)
    return true;
}

bool Impact::Tick() {                                 // orig: vf08 0x0004EAA0
    const double t = double(g_frameStep) + time;      // FLD [0x002D67E8]; FADD +0x24
    time = F(t);                                      // FST: the unrounded sum is compared
    if (!(t * speed > duration)) return true;         // FMUL +0x50; FCOMP +0x58; TEST AH,0x41; JNE (NaN: alive)
    if (!hold) return false;                          // +0x54
    time = F(double(duration) / speed);               // FLD +0x58; FDIV +0x50; FSTP +0x24
    return true;
}

bool Impact::PostUpdate() {                           // orig: vf0C 0x0004F2E0
    if (lit) lit->switchOn = true;                    // +0x44: byte +0xC0 = 1 (L: the light's switch)
    if (follow) {                                     // +0x4C: the followed node's world transform
        node->local.translate = follow->world.translate;   // +0xA0 -> node +0x60
        node->local.rotate = follow->world.rotate;         // +0x70 -> node +0x30 (12 dwords via the stack)
    }
    if (billboard) {                                  // +0x55 (0x0004F35C)
        // the camera's world rotation (S+0x8C -> +0x30 -> +0x70) with columns 0 and 2 swapped (dword moves), then
        // the spin about z (0x001030F0(+0x3C), 0x00013B30)
        ni::NiMatrix3 Cs = level.camera->sceneCamera->world.rotate;
        for (int j = 0; j < 3; ++j) std::swap(Cs.m[j][0], Cs.m[j][2]);
        node->local.rotate = ni::Multiply(Cs, ni::MakeZRotation(angle));
    }
    UpdateNodeAt(node.Get(), F(double(speed) * time));   // FLD +0x50; FMUL +0x24; NiAVObject::Update(t, 1)
    return true;
}

void Impact::SetLit(ni::NiDynamicEffect* fx) {        // orig: 0x0004F430
    if (!fx) return;
    lit = fx;                                         // +0x44 (NiPointer assign)
    PrepareCharacterProperties(clone.Get());          // 0x0003F8D0(+0x48)
    PrepareCharacterPartProperties(clone.Get());      // 0x0003FB30(+0x48)
    ni::NiPointer<ni::NiAmbientLight> light(new ni::NiAmbientLight);   // new(0x110) 0x0010EFC0
    const f32 k = std::bit_cast<f32>(0x3F333333u);   // 0.7
    light->ambient = {k, k, k};                       // +0xE4 / +0xE8 / +0xEC
    ++light->revision;                                // +0xCC
    light->switchOn = true;                           // +0xC0 = 1
    light->AttachAffectedNode(node.Get());            // 0x001419B0(+0x2C)
    node->AttachChild(light.Get(), true);             // vfA0(light, 1)
    fx->AttachAffectedNode(clone.Get());              // 0x001419B0(+0x48)
    fx->UpdateProperties();                           // 0x00101EA0
    fx->UpdateEffects();                              // 0x00102190
    UpdateNodeAt(fx, 0.0f);                           // NiAVObject::Update(0, 1)
    PrepareLitModel(clone.Get());                     // 0x000966A0 (cdecl)
    node->UpdateProperties();
    node->UpdateEffects();
    UpdateNodeAt(node.Get(), 0.0f);
}

void Impact::SetPosition(const NiPoint3& p) {         // orig: vf1C 0x0004EAD0
    if (follow) clone->local.translate = p;           // +0x48 +0x60: an offset from the followed node
    else node->local.translate = p;                   // +0x2C +0x60
}

void Impact::SetFollow(ni::NiAVObject* n) {           // orig: 0x0004F580
    node->local.translate = {};                       // +0x2C +0x60 = (0, 0, 0)
    follow = n;                                       // +0x4C
}

void SetNodeScale(GameObject& o, f32 s) {             // orig: 0x00073720
    if (o.node) o.node->local.scale = F(std::fabs(double(s)));   // FLD; FABS; FSTP +0x6C
}

}  // namespace fut::game

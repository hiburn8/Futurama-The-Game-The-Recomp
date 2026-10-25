// Destructables and drop lists; see Destructable.h and docs/analysis/destructable_moveable_port.md (sections 2-3).
#include "game/object/Destructable.h"

#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "engine/ni/NiClone.h"
#include "game/defs/Defs.h"
#include "game/defs/ExplosionDef.h"
#include "game/level/LevelData.h"
#include "game/level/LightManager.h"
#include "game/object/NodeUpdate.h"
#include "game/object/Character.h"
#include "game/object/Moveable.h"
#include "game/object/Pickup.h"
#include "game/level/Stage.h"
#include "game/sound/GameSound.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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

// ---- the drop list (3.9) ------------------------------------------------------------------------------------------
bool DropList::Add(const char* cls, const char* name, const char* label, const LevelData& level) {   // 0x00076170
    if (!name) return false;
    u32 mask;
    if (crt::StrICmp(cls, "Weapon") == 0) mask = 8;
    else if (crt::StrICmp(cls, "Pickup") == 0) mask = 4;
    else if (crt::StrICmp(cls, "Powerup") == 0) mask = 0x400;
    else if (crt::StrICmp(cls, "Ammo") == 0) mask = 0x800;
    else if (crt::StrICmp(cls, "Charge") == 0) mask = 0x100000;
    else return false;
    if (!level.defs) return false;
    for (const auto& d : level.defs->Defs())          // S+0x34 in creation order (0x000C26A0)
        if (d && d->IsType(mask) && std::strcmp(name, d->name.c_str()) == 0) {   // (case-sensitive)
            Entry e;
            if (label) e.label = label;
            e.def = d.get();
            entries.push_back(std::move(e));          // new(8) entry (+ label copy), array growth +1
            return true;
        }
    return false;
}

bool DropList::Parse(const char* s, const LevelData& level) {   // orig: 0x00076310
    entries.clear();                                  // 0x00076020
    if (!s || std::strlen(s) <= 2) {
        mode = 1;
        return true;
    }
    std::string copy(s);                              // new(strlen + 1); strcpy
    char* p = copy.data() + 1;                        // skips '{'
    auto fail = [&] {
        mode = 1;
        entries.clear();
        return false;
    };
    char* semi = std::strchr(p, ';');
    if (!semi) {
        entries.clear();
        return false;
    }
    *semi = 0;
    if (std::strcmp(p, "mode_all") == 0) mode = 0;
    else if (std::strcmp(p, "mode_random") == 0) mode = 1;
    else {
        entries.clear();
        return false;
    }
    char* e = semi + 1;
    bool done = false;
    while (!done && e) {
        char* cls = e;
        char* colon = std::strchr(e, ':');
        if (!colon) return fail();
        colon[0] = colon[1] = 0;
        char* name = colon + 2;                       // "Class::Name"
        char* lp = std::strchr(name, '(');
        if (!lp) return fail();
        *lp = 0;
        char* label = lp + 1;
        char* rp = std::strchr(label, ')');
        if (!rp) return fail();
        *rp = 0;
        e = std::strchr(rp + 1, ';');
        if (!e) {
            e = std::strchr(rp + 1, '}');
            done = true;
        }
        if (!e) return fail();
        *e++ = 0;
        (void)Add(cls, name, std::strlen(label) ? label : nullptr, level);   // (result ignored)
    }
    return true;
}

// ---- the destructable ---------------------------------------------------------------------------------------------
Destructable::Destructable(LevelData& lvl, DestructableDef& d) : GameObject(lvl, d) {   // orig: 0x00063840
    nextDestructable = lvl.firstDestructable;         // head insert into DAT_002DE13C
    lvl.firstDestructable = this;
}

Destructable::~Destructable() {                       // orig: 0x00063890
    drops.reset();
    box.reset();
    if (model && model->parent) model->parent->DetachChild(model.Get());
    model = nullptr;
    for (Destructable** p = &level.firstDestructable; *p; p = &(*p)->nextDestructable)
        if (*p == this) {
            *p = nextDestructable;
            break;
        }
}

bool Destructable::Init() {                           // orig: 0x00064E60
    auto& d = static_cast<DestructableDef&>(*def);
    if (d.value13 == 3) NotPorted("Destructable kind 3 (laser fan) 0x00063EB0");
    else if (d.value13 == 2) NotPorted("Destructable kind 2 (trip wire) 0x00064820");
    if (d.value13 != 0 && d.unknown94 == 0 && level.loadSound)   // D+0x60, D+0x94
        d.unknown94 = level.loadSound("Sounds/Mine_Activate.wav", 0);   // Sound_Load 0x000C9860
    health = d.health;                                // +0x44 = D+0x3C
    ni::NiPointer<ni::NiObject> c = ni::Clone(d.model.Get());   // NiObject::Clone 0x000FE120
    model = dynamic_cast<ni::NiSwitchNode*>(c.Get());
    if (!model) return true;
    node->AttachChild(model.Get(), true);             // vfA0 (first free slot: child 0)
    model->flags &= ~ni::kNiAVHidden;
    model->local.translate = {};
    model->local.rotate = {};
    model->flags = static_cast<u16>(model->flags & 0xFFF1);   // 0x00101B80(0)
    if (model->Children().size() < 1) broken = 1;     // CMP [sw+0xCC], 1; JAE
    node->UpdateProperties();                         // 0x00064FA8
    node->UpdateEffects();                            // 0x00064FB0
    node->UpdateDownwardPass(model->Children().size() == 1 ? 0.0f : time, true);   // 0x001018D0
    // new(0x30) CollisionBox(D+0x24.., 5.0f): centre = the node's LOCAL translate + the offset (not rotated)
    box = level.NewCollisionBox(d.extents[0], d.extents[1], d.extents[2], 5.0f);
    const NiPoint3& t = node->local.translate;
    box->SetPosition({F(double(t.x) + d.center[0]), F(double(t.y) + d.center[1]), F(double(t.z) + d.center[2])});
    box->SetRotation(node->local.rotate);             // vf38
    box->box.isStatic = 1;                            // +0x150
    box->owner = this;                                // vf28
    CreateAmbientLight();                             // vf44
    InitAmbientLight();                               // vf4C
    ni::NiAVObject* act = GetObjectByName(model.Get(), "Activated");
    ni::NiAVObject* dor = GetObjectByName(model.Get(), "Dormant");
    if (act) act->flags |= ni::kNiAVHidden;
    if (dor) dor->flags &= ~ni::kNiAVHidden;
    if (d.value13 == 1)
        if (ni::NiAVObject* m = GetObjectByName(model.Get(), "Mine_Explode")) m->flags |= ni::kNiAVHidden;
    if (level.lightManager) level.lightManager->AddTarget({0, 0, 0}, node.Get());   // DAT_002EA824 vf00
    return true;
}

bool Destructable::PostUpdate() {                     // orig: 0x00064E00 (Stage::Render; result ignored)
    if (model) model->UpdateDownwardPass(model->ChildCount() == 1 ? 0.0f : time, true);   // +0xD0
    if (!broken) {
        const s32 kind = static_cast<DestructableDef&>(*def).value13;
        if (kind == 3) NotPorted("Destructable kind 3 update 0x000644E0");
        else if (kind == 2) NotPorted("Destructable kind 2 update 0x00063680");
        else if (kind == 1) UpdateMine();
    }
    return true;
}

void Destructable::UpdateMine() {                     // orig: 0x00063780
    if (mineArmed) {
        if (time > 0.5f) Break();                     // FCOMP [0x00293AA0]; TEST AH,0x41; JNE
        return;
    }
    const NiPoint3 p = GetWorldPosition();
    for (Character* c : level.characters) {           // S+0x90 (0x000C26E0)
        const NiPoint3 q = c->GetWorldPosition();
        const double dx = double(p.x) - q.x, dy = double(p.y) - q.y, dz = double(p.z) - q.z;
        if ((dz * dz + dy * dy) + dx * dx < 100.0) {  // FCOMP [0x00295610]; TEST AH,5; JP (unrounded)
            time = 0.0f;
            mineArmed = 1;
            ActivateMine();                           // (every character in range re-activates)
        }
    }
}

void Destructable::ActivateMine() {                   // orig: 0x00063420
    const u32 activate = static_cast<DestructableDef&>(*def).unknown94;   // D+0x94
    if (activate) (void)level.Sound().Play3D(activate, GetWorldPosition(), false);   // vf34; Sound_Play3D 0x0006344E
    ni::NiAVObject* act = GetObjectByName(model.Get(), "Activated");
    ni::NiAVObject* dor = GetObjectByName(model.Get(), "Dormant");
    if (act) act->flags &= ~ni::kNiAVHidden;
    if (dor) dor->flags |= ni::kNiAVHidden;
    ClampLoopingControllers(GetObjectByName(model.Get(), "Mine_Explode"));
}

void Destructable::ApplyInstanceString(const char* s) {   // orig: 0x00063550
    if (!s || std::strlen(s) <= 2) return;
    if (!drops) drops = std::make_unique<DropList>(id);   // new(0x20) 0x00075D90
    (void)drops->Parse(s, level);                     // 0x00076310
}

void Destructable::SetPositionAndUpdate(NiPoint3 p) { // orig: 0x00063660
    GameObject::SetPosition(p);                       // 0x00097140 (Update + UpdateRoom)
    node->UpdateDownwardPass(time, true);
}

bool Destructable::Damage(f32 amount) {               // orig: 0x000635E0
    if (!model || broken) return false;
    const double h = double(health) - amount;         // FLD; FSUB
    health = F(h);                                    // FST (the compare uses the unrounded ST0)
    if (!(h < 0.0)) return false;                     // TEST AH,5; JP
    Break();
    return true;
}

// ---- inventory_batch1_port.md 3 (checked against the assembly of 0x000634C0, 0x000630E0, 0x00063A50, 0x000760E0,
// 0x00075E60 and 0x00075DD0; destructable_moveable_port.md 3.6 / 3.8 / 3.9) ----------------------------------------
void Destructable::AddDrop(Def* d, const char* dropLabel) {   // orig: vf58 0x000634C0
    if (!d->IsType(4)) return;                        // vf0C(4): the pickup family
    if (!drops) drops = std::make_unique<DropList>(id);   // new(0x20) + 0x00075D90(+0x20): its ctor new(4)s the array
    drops->AddEntry(d, dropLabel);                    // 0x000760E0
    drops->mode = 0;                                  // 0x0015C260(0): mode_all
}

void Destructable::SpawnParts(ni::NiAVObject* obj, s32& counter) {   // orig: vf60 0x000630E0
    if (!obj) return;
    if (auto* n = dynamic_cast<ni::NiNode*>(obj)) {   // NiObject::DynamicCast 0x00011470 (NiNode 0x002F9230)
        const size_t count = n->Children().size();    // +0xCC, read once
        for (size_t i = 0; i < count; ++i)
            SpawnParts(i < n->Children().size() ? n->Children()[i].Get() : nullptr, counter);   // this->vf60
    }
    if (obj->name.empty() || std::strncmp(obj->name.c_str(), "Part", 4) != 0) return;   // strncmp 0x001675D0
    // dir = world rotation * UNIT_Z (0x002D77F8), per row: (Z.y * c1 + Z.z * c2) + Z.x * c0, FSTP each
    const ni::NiMatrix3& m = obj->world.rotate;       // +0x70
    NiPoint3 dir{F((0.0 * m.m[0][1] + 1.0 * m.m[0][2]) + 0.0 * m.m[0][0]),
                 F((0.0 * m.m[1][1] + 1.0 * m.m[1][2]) + 0.0 * m.m[1][0]),
                 F((0.0 * m.m[2][1] + 1.0 * m.m[2][2]) + 0.0 * m.m[2][0])};
    constexpr double kR = double(3.0518509447574615e-05f);   // [0x002955E8] (1 / 32767)
    const f32 a = F(double(crt::Rand()) * kR);        // rand #1 (0x000631F7): FILD; FMUL; FSTP
    const f32 b = F(double(crt::Rand()) * kR - 0.5f); // rand #2 (0x0006320E): FSUB [0x00293AA0]; FSTP
    const f32 a2 = F(double(a) - 0.5f);
    const double c = double(crt::Rand()) * kR - 0.5f; // rand #3 (0x00063239): stays on the stack
    const double bx = double(b) * 0.4f;               // FMUL [0x00293B28] (not rounded)
    const f32 ay = F(double(a2) * 0.4f);              // FSTP [esp+0x38]
    const f32 cz = F(c * 0.4f);                       // FSTP [esp+0x3C]
    dir.x = F(bx + dir.x);                            // the jitter: x <- rand #2, y <- rand #1, z <- rand #3
    dir.y = F(double(ay) + dir.y);
    dir.z = F(double(cz) + dir.z);
    (void)ni::Unitize(dir);                           // 0x000116A0 (FSTP ST0)
    const double sp = ((double(crt::Rand() % 100) * 0.01f + 1.0f) * 10.0f) * 0.5f;   // rand #4 (0x000632A1): CDQ;
                                                      //  IDIV 100; FILD; FMUL [0x00293AAC]; FADD 1.0; FMUL 10; FMUL 0.5
    dir = {F(dir.x * sp), F(dir.y * sp), F(dir.z * sp)};   // FLD; FMUL ST1; FSTP (x, y, z)
    Stage* stage = level.stages.empty() ? nullptr : level.stages.front().get();   // Session::NextStage(0) 0x000C26C0
    const NiPoint3 pos = obj->world.translate;        // +0xA0 (dwords)
    auto& d = static_cast<DestructableDef&>(*def);
    if (!d.effects1.empty()) {                        // D+0x4C > 0: the debris names
        const s32 k = crt::Rand() % static_cast<s32>(d.effects1.size());   // rand #5 (drawn even for an unknown name)
        if (Def* dd = level.defs ? level.defs->Find(d.effects1[k].c_str()) : nullptr) {   // 0x000C3F10
            // vf14(stage ? stage + 4 : 0, &pos, &identity 0x002D7820, 0), then the object's +0x38 (its physics
            // object) ->+0x28 SetVelocity(&dir) 0x000AD8A0 (no null check on the object: a UB guard here)
            const ni::NiMatrix3 ident;                // 0x002D7820
            GameObject* o = dd->Spawn(level, stage, pos, ident, nullptr);
            if (auto* mv = dynamic_cast<Moveable*>(o); mv && mv->body) mv->body->box.SetVelocity(dir);
        }
    }
    (void)ni::Unitize(dir);                           // 0x000116A0 again (both paths)
    for (const std::string& e : d.effects2)           // D+0x50 / +0x54, in order
        if (Def* ed = level.defs ? level.defs->Find(e.c_str()) : nullptr)
            (void)ed->SpawnEffect(level, pos, dir);   // vf20(&pos, &dir)
    if (d.hasValue12)                                 // D+0x58
        if (Def* ed = level.defs ? level.defs->Find("ExplosionDef") : nullptr) {   // 0x0029BA00
            // vf20(S, &pos, D+0x5C, 0, counter == 0) 0x000676C0 (the found definition's slot, no type test)
            if (auto* x = dynamic_cast<ExplosionDef*>(ed))
                (void)x->Spawn(level, &level, pos, static_cast<s32>(d.value12), nullptr, counter == 0);
            else
                NotPorted("Destructable::SpawnParts: an \"ExplosionDef\" that is not the explosion definition");
        }
    ++counter;                                        // only for "Part" nodes (also when nothing was spawned)
}

void Destructable::Break() {                          // orig: vf68 0x00063A50
    if (!model) return;
    level.SetObjFlags(id, level.GetObjFlags(id) | 8); // 0x000C1A60 / 0x000C1A40 (id +0x20): "destroyed"
    if (model->Children().size() > 1) model->index = 1;   // switch +0xCC > 1 (JLE) -> +0xE0 = 1
    broken = 1;                                       // +0x48
    time = 0.0f;                                      // +0x24
    UpdateNodeAt(model.Get(), 0.0f);                  // NiAVObject::Update(0.0, 1)
    const NiPoint3 pos = node->local.translate;       // node +0x60 (LOCAL), dwords
    const ni::NiMatrix3 rot = node->local.rotate;     // node +0x30 (12 dwords)
    auto& d = static_cast<DestructableDef&>(*def);
    if (!d.effect.empty())                            // D+0x40: the replacement definition
        if (Def* r = level.defs ? level.defs->Find(d.effect.c_str()) : nullptr)   // Session::FindDef 0x000C3F10
            (void)r->Spawn(level, &level, pos, rot, nullptr);   // vf14(S, &pos, &rot, 0)
    if (d.hasBreakSound) (void)level.Sound().Play3D(d.breakSound, pos, false);   // D+0x44 set: Sound_Play3D(D+0x18) 0x00063B2E
    s32 parts = 0;                                   // [esp+0x30]
    SpawnParts(model.Get(), parts);                   // vf60(+0x38, &counter)
    if (d.explosive) {                                // D+0x88
        Player* pl = level.player;                    // S+0x94 (no null check in the original)
        if (pl) {
            const NiPoint3 q = pl->GetWorldPosition();   // vf34
            const double dx = double(q.x) - pos.x, dy = double(q.y) - pos.y, dz = double(q.z) - pos.z;
            const double dist = std::sqrt((dz * dz + dy * dy) + dx * dx);   // FSQRT, on the stack
            if (dist < d.explosionA)                  // FCOM D+0x8C; TEST AH,5; JP
                // vf7C(F((1 - dist / D+0x8C) * D+0x90), this): the attacker is the destructable itself (not a
                // character); the port passes none (L: Player::Damage reads a non-character attacker only through its
                // null tests: Bender's spin / Leela's 0xB5 immunity)
                pl->Damage(F((1.0 - dist / d.explosionA) * d.explosionB), nullptr);   // FDIV; FSUBR 1.0; FMUL; FSTP
        }
    }
    box.reset();                                      // +0x3C->vf00(1); +0x3C = 0
    if (spawnDef) {                                   // +0x4C
        const NiPoint3 wp = GetWorldPosition();       // vf34
        const NiPoint3 p{wp.x, F(double(wp.y) + 1.0f), wp.z};   // FLD z; FLD y; FADD [0x00293A98]
        const ni::NiMatrix3 r = GetWorldRotation();   // vf38
        (void)spawnDef->Spawn(level, owner, p, r, spawnLabel);   // vf14(+4, &p, &r, +0x50)
    } else if (drops) {                               // +0x74
        const ni::NiMatrix3 r = GetWorldRotation();   // vf38 first
        const NiPoint3 p = GetWorldPosition();        // then vf34
        drops->SpawnAll(level, owner, r, p);          // 0x00075E60(owner, &rot, &pos)
    }
    // +0x60: detached from its parent (+0x14 ->vfA4) and released; +0x64 released (kinds 2 / 3 only, not ported)
    if (d.value13 != 0) {                             // D+0x60: the mine chain
        const NiPoint3 wp = GetWorldPosition();       // vf34
        // 0x000C2060(&wp): S+0x44->vf20(S, &wp, 0, 0, 1) (no null test in the original)
        if (level.defs && level.defs->explosion) (void)level.defs->explosion->Spawn(level, &level, wp, 0, nullptr, true);
        const NiPoint3 p = GetWorldPosition();        // vf34 (esp+0x18)
        for (Character* ch : std::vector<Character*>(level.characters)) {   // S+0x90 (0x000C26E0)
            const NiPoint3 q = ch->GetWorldPosition();   // vf34
            const double dx = double(p.x) - q.x, dy = double(p.y) - q.y, dz = double(p.z) - q.z;
            const f32 d2 = F((dz * dz + dy * dy) + dx * dx);   // FSTP [esp+0x10]
            f32 dmg = 0;                              // FLD d2; FCOMP k; TEST AH,0x41; JP next (proceeds on <=)
            if (d2 <= 25.0f) dmg = 80.0f;             // [0x00297240] -> 0x42A00000
            else if (d2 <= 100.0f) dmg = 60.0f;       // [0x00295610] -> 0x42700000
            else if (d2 <= 225.0f) dmg = 40.0f;       // [0x00295694] -> 0x42200000
            else if (d2 <= 400.0f) dmg = 20.0f;       // [0x00297210] -> 0x41A00000
            ch->Damage(dmg, nullptr);                 // vf7C(dmg, 0) for every character (0 damage included)
        }
        for (Destructable* o = level.firstDestructable; o; o = o->NextDestructable()) {   // DAT_002DE13C / vf5C
            const NiPoint3& t = o->GetNode()->local.translate;   // vf30 +0x60 (LOCAL)
            const double dx = double(p.x) - t.x, dy = double(p.y) - t.y, dz = double(p.z) - t.z;
            const double d2 = (dz * dz + dy * dy) + dx * dx;   // (not rounded)
            f32 dmg = 0;                              // FCOM k; TEST AH,0x41; JP next
            if (d2 <= 100.0) dmg = 100.0f;            // [0x00295610]
            else if (d2 <= 400.0) dmg = 50.0f;        // [0x00297210]
            else if (d2 <= 1600.0) dmg = 25.0f;       // [0x0029BAAC]
            if (static_cast<DestructableDef&>(*o->def).value13 != 0) (void)o->Damage(dmg);   // vf64: other mines
        }
    }
    ClampLoopingControllers(node.Get());              // 0x00063000(+0x2C), for every kind
}

void DropList::AddEntry(Def* def, const char* label) {   // orig: 0x000760E0
    Entry e;                                          // new(8) entry 0x00075CE0: the label copied (new(strlen + 1))
    if (label) e.label = label;
    e.def = def;
    entries.push_back(e);                             // (the array grows by one through 0x000BA340 when full)
}

void DropList::SpawnAll(LevelData& level, Owner* owner, const ni::NiMatrix3& rot, const NiPoint3& pos) {   // 0x00075E60
    const s32 n = static_cast<s32>(entries.size());   // +0xC
    if (n == 0) return;
    if (mode == 1 || n == 1) {                        // mode_random, or a single entry in either mode
        const s32 k = crt::Rand() % n;                // 0x00075FD5; CDQ; IDIV
        SpawnEntry(level, k, owner, rot, pos, {0.0f, 20.0f, 0.0f});   // (0, [0x002D6628], 0)
        return;
    }
    if (mode != 0) return;
    // mode_all, n >= 2: the start vector MakeXRotation(F(0.0174533f * 65)) * (0, 0, 20), turned by 2 pi / n about y
    // before each spawn; the step rotation is the object's rotation argument
    const ni::NiMatrix3 M = ni::MakeXRotation(F(double(0.0174532924f) * 65.0f));   // [0x0029CD58] * [0x002D6624]
    NiPoint3 v = ni::MulVector(M, {0.0f, 0.0f, 20.0f});   // 0x00011770
    const ni::NiMatrix3 R = ni::MakeYRotation(F(double(6.28318548f) / double(n)));   // FILD n; FDIVR [0x0029CD78]
    for (s32 i = 0; i < n; ++i) {
        const auto& m = R.m;                          // (all three from the old v)
        const f32 x = F((double(m[0][2]) * v.z + double(m[0][1]) * v.y) + double(m[0][0]) * v.x);
        const f32 y = F((double(m[1][2]) * v.z + double(m[1][1]) * v.y) + double(m[1][0]) * v.x);
        const f32 z = F((double(m[2][2]) * v.z + double(m[2][1]) * v.y) + double(m[2][0]) * v.x);
        v = {x, y, z};
        SpawnEntry(level, i, owner, R, pos, v);       // Entry::Spawn(owner, &R, pos, &v, id, i)
    }
}

void DropList::SpawnEntry(LevelData& level, s32 k, Owner* owner, const ni::NiMatrix3& rot, const NiPoint3& pos,
                          const NiPoint3& vel) {      // orig: Entry::Spawn 0x00075DD0
    const Entry& e = entries[static_cast<size_t>(k)];
    auto* pd = dynamic_cast<PickupDef*>(e.def);       // (a pickup-family definition: IsType(4) at the add)
    if (pd && pd->value && level.NibblerState(recordId, static_cast<u32>(k))) return;   // def+0x2C; S+0x1FC 0x00077770
    GameObject* g = e.def->Spawn(level, owner, pos, rot, e.label ? e.label->c_str() : nullptr);   // vf14
    auto* o = dynamic_cast<Pickup*>(g);               // (no null check in the original)
    if (!o) {
        NotPorted("DropList::SpawnEntry: a drop that is not a pickup object");
        return;
    }
    o->SetKey(static_cast<s32>(recordId), k);         // 0x00076590: +0x68 / +0x6C
    o->fallDt = 0;                                    // 0x00076550: +0x58 / +0x5C = 0, +0x4C..+0x54 = *vel,
    o->airTime = 0;                                   //  +0x60 = 0, +0x38 = 2
    o->velocity = vel;
    o->bounces = 0;
    o->state = Pickup::kFalling;
    if (crt::StrICmp(level.name.c_str(), "level1-6") == 0) o->ignoreRange = 1;   // 0x0029A524; 0x00076580: +0x41
}


void ClampLoopingControllers(ni::NiAVObject* obj) {   // orig: 0x00063000
    if (!obj) return;
    if (auto* n = dynamic_cast<ni::NiNode*>(obj)) {
        const size_t count = n->Children().size();    // +0xCC, read once
        for (size_t i = 0; i < count; ++i)
            ClampLoopingControllers(i < n->Children().size() ? n->Children()[i].Get() : nullptr);
    }
    auto clamp = [](ni::NiTimeController* c) {
        for (; c; c = c->nextControllerPtr.Get())     // 0x0013EC10
            if (c->CycleType() == 0) c->flags = static_cast<u16>((c->flags & 0xFFF9) | (2 << 1));   // 0x0013EBA0
    };
    clamp(obj->controller.Get());
    if (ni::NiProperty* p = GetProperty(*obj, 3)) clamp(p->controller.Get());   // the material property
}

GameObject* DestructableDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                                   const char* label) {   // orig: 0x00065200 (no owner type check)
    auto* o = new Destructable(level, *this);         // new(0x78)
    o->SetOwner(owner);                               // vf10 (base)
    o->SetName(label);                                // 0x000968C0
    o->SetPositionAndUpdate(pos);                     // vf54 0x00063660
    o->SetRotation(rot);                              // vf24 0x00096A30
    if (model) o->Init();                             // D+0x1C
    return o;
}

}  // namespace fut::game

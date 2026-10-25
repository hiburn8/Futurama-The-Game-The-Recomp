// Trigger manager; see TriggerManager.h.
#include "game/level/TriggerManager.h"

#include "game/defs/Defs.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/object/Character.h"
#include "game/object/Destructable.h"
#include "game/object/GameObject.h"

#include <cstring>

namespace fut::game {
namespace {

int Lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }   // orig: tolower 0x00167DF5 (C locale)

int StrICmp(const char* a, const char* b) {          // orig: _stricmp (0x001684D1)
    for (;; ++a, ++b) {
        const int ca = Lower(static_cast<unsigned char>(*a)), cb = Lower(static_cast<unsigned char>(*b));
        if (ca != cb || !ca) return ca - cb;
    }
}

int StrNICmp(const char* a, const char* b, size_t n) {   // orig: _strnicmp (0x00170B4A)
    for (; n; --n, ++a, ++b) {
        const int ca = Lower(static_cast<unsigned char>(*a)), cb = Lower(static_cast<unsigned char>(*b));
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

bool IsObjectKind(u32 k) { return k == 1 || k == 5 || k == 6; }

// The "Triggers" node of a stage (orig: GetNode()->vf54("Triggers"), depth-first strcmp).
ni::NiNode* TriggersOf(Stage& st) {
    return st.node ? dynamic_cast<ni::NiNode*>(GetObjectByName(st.node.Get(), "Triggers")) : nullptr;
}

// orig: 0x000C2B00: the first "Triggers" child of a loaded stage whose name after "<x>:" + 2 matches (_stricmp).
ni::NiAVObject* FindTriggerNode(LevelData& level, const char* name) {
    if (!name) return nullptr;
    for (const auto& st : level.stages) {
        if (!st->IsLoaded()) continue;
        ni::NiNode* t = TriggersOf(*st);
        if (!t) continue;
        const size_t n = t->Children().size();
        for (size_t i = 0; i < n; ++i) {
            ni::NiAVObject* c = i < t->Children().size() ? t->Children()[i].Get() : nullptr;
            if (!c || c->name.empty()) continue;
            const size_t colon = c->name.find(':');
            if (colon == std::string::npos) continue;
            if (colon + 2 > c->name.size()) continue;   // (the original reads past the end)
            if (StrICmp(c->name.c_str() + colon + 2, name) == 0) return c;
        }
    }
    return nullptr;
}

}  // namespace

// ---- TriggerRec -----------------------------------------------------------------------------------------------------
void TriggerRec::Unbind() {                           // orig: 0x000DA6D0
    void* old = target;
    target = nullptr;
    if (IsObjectKind(kind) && old) {
        auto* o = static_cast<GameObject*>(old);
        if (o->triggerRec == this) o->SetTriggerRec(nullptr);
    }
}

void TriggerRec::Bind(void* newTarget) {              // orig: 0x000DA710
    Unbind();
    target = newTarget;
}

// ---- TriggerManager -------------------------------------------------------------------------------------------------
s32 TriggerManager::Hash(const char* s, bool caseSensitive) {   // orig: 0x000DA500
    static constexpr s32 kW[16] = {3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59};   // orig: 0x002D6DD0
    if (!s) return 0;
    const s32 n = static_cast<s32>(std::strlen(s));
    const s32 u = n - 15 < 0 ? 0 : n - 15;
    u32 h = static_cast<u32>(n);
    for (s32 i = u; i < n; ++i) {
        const s32 c = caseSensitive ? static_cast<s32>(static_cast<signed char>(s[i]))
                                    : Lower(static_cast<unsigned char>(s[i]));
        h += static_cast<u32>(kW[n - i] * c);         // (32-bit wraparound)
    }
    return static_cast<s32>(h);
}

TriggerRec* TriggerManager::Find(const char* name) const {   // orig: 0x000DAFA0
    if (!name) return nullptr;
    const s32 h = Hash(name, true);
    for (const auto& r : records)
        if (r->hasName && r->hash == h && StrICmp(r->name.c_str(), name) == 0) return r.get();
    return nullptr;
}

TriggerRec* TriggerManager::FindOrCreate(const char* name) {   // orig: 0x000DB390
    if (!name) return nullptr;
    if (TriggerRec* r = Find(name)) return r;         // (an existing record is not re-resolved)
    records.push_front(std::make_unique<TriggerRec>());
    TriggerRec& r = *records.front();
    r.name = name;                                    // orig: SetName 0x000DA580
    r.hasName = true;
    r.hash = Hash(name, true);
    Resolve(r);
    return &r;
}

void TriggerManager::SetKind(u32 k, TriggerRec& rec) {   // orig: 0x000DA660
    if (rec.kindNext) rec.kindNext->kindPrev = rec.kindPrev;
    if (rec.kindPrev) rec.kindPrev->kindNext = rec.kindNext;
    if (kindHeads[rec.kind] == &rec) kindHeads[rec.kind] = rec.kindNext;
    if (k == 0) {
        rec.kindNext = rec.kindPrev = nullptr;
        rec.kind = 0;
        return;
    }
    rec.kindPrev = nullptr;
    rec.kindNext = kindHeads[k];
    if (rec.kindNext) rec.kindNext->kindPrev = &rec;
    kindHeads[k] = &rec;
    rec.kind = k;
}

TriggerRec* TriggerManager::FindByTarget(const void* target) const {   // orig: 0x000DAF70
    for (const auto& r : records)
        if (r->target == target) return r.get();
    return nullptr;
}

void TriggerManager::RegisterObject(GameObject& o) {   // orig: 0x000DBF30
    if (!o.label) return;                             // (NULL only; "" passes)
    const bool isCharacter = o.def->IsType(1);
    TriggerRec* rec = isCharacter ? FindOrCreate(o.label->c_str()) : Find(o.label->c_str());
    if (!rec || rec->target) return;                  // the first binder wins
    rec->id = static_cast<s32>(o.id);
    rec->Bind(&o);
    SetKind(isCharacter ? 5 : o.def->IsType(0x40) ? 6 : 1, *rec);
    o.SetTriggerRec(rec);
}

void TriggerManager::Resolve(TriggerRec& rec) {       // orig: 0x000DA9E0
    const u32 k = rec.kind;
    if (k == 0 || k == 2 || k == 3 || k == 4 || k == 7) rec.target = nullptr;   // re-looked-up every time
    const char* name = rec.hasName ? rec.name.c_str() : nullptr;
    if ((rec.kind == 0 || rec.kind == 7) && name && StrICmp(name, "$cam") == 0) {
        rec.Bind(level_.camera.get());                // orig: S+0x8C
        SetKind(7, rec);
    }
    if (rec.kind == 0 || rec.kind == 5) {             // the character list S+0x90 (link +0x76C)
        for (Character* c : level_.characters)
            if (c->label && name && StrICmp(c->label->c_str(), name) == 0) {
                rec.id = static_cast<s32>(c->id);
                rec.Bind(c);
                SetKind(5, rec);
                c->SetTriggerRec(&rec);
                break;
            }
    }
    if (rec.kind == 0 || rec.kind == 6) {             // orig: 0x000DAB2D: the destructable list DAT_002DE13C
        for (Destructable* d = level_.firstDestructable; d; d = d->nextDestructable)   // (vf5C)
            if (d->label && name && StrICmp(d->label->c_str(), name) == 0) {
                rec.id = static_cast<s32>(d->id);
                rec.Bind(d);
                SetKind(6, rec);
                d->SetTriggerRec(&rec);
                break;
            }
    }
    if (rec.kind == 0 || rec.kind == 1) {
        auto bindIn = [&](const Owner& owner) {
            for (GameObject* o = owner.NextObject(nullptr); o; o = owner.NextObject(o))
                if (o->label && name && StrICmp(o->label->c_str(), name) == 0) {
                    rec.id = static_cast<s32>(o->id);
                    rec.Bind(o);
                    SetKind(1, rec);
                    o->SetTriggerRec(&rec);
                    break;
                }
        };
        bindIn(level_);                               // the level's own objects, then the first three stages':
        for (size_t s = 0; s < level_.stages.size() && s < 3; ++s) bindIn(*level_.stages[s]);   // last match wins
    }
    if (rec.kind == 0 || rec.kind == 2) {
        const StageWaypoint* wp = nullptr;            // orig: Session::FindWaypointAnyStage 0x000C2A40
        for (const auto& st : level_.stages)
            if (name && (wp = st->FindWaypoint(name))) break;
        if (wp) {
            rec.Bind(const_cast<StageWaypoint*>(wp));
            SetKind(2, rec);
        }
    }
    if (rec.kind == 0 || rec.kind == 3 || rec.kind == 4) {
        if (ni::NiAVObject* n = FindTriggerNode(level_, name)) {
            rec.Bind(n);
            if (rec.kind == 0) SetKind(n->name.find("Box") != std::string::npos ? 4 : 3, rec);   // strstr
        }
    }
}

ni::NiAVObject* TriggerManager::Node(const TriggerRec& rec) const {   // orig: 0x000DA5F0 (jump table 0x000DA620)
    if (!rec.target) return nullptr;
    switch (rec.kind) {
    case 1:
    case 6: return static_cast<const GameObject*>(rec.target)->GetNode();   // vf30
    case 5: return static_cast<const Character*>(rec.target)->GetNode();
    case 3:
    case 4: return static_cast<ni::NiAVObject*>(rec.target);
    default: return nullptr;                          // (2: the waypoint; 7 and others: outside the table)
    }
}

ni::NiPoint3 TriggerManager::Position(const TriggerRec& rec) const {   // orig: 0x000DA7E0
    if (!rec.target) return {};
    switch (rec.kind) {
    case 1:
    case 6: return static_cast<const GameObject*>(rec.target)->GetWorldPosition();   // vf34
    case 5: return static_cast<const Character*>(rec.target)->GetWorldPosition();
    case 2: return static_cast<const StageWaypoint*>(rec.target)->position;
    case 3:
    case 4: return static_cast<const ni::NiAVObject*>(rec.target)->world.translate;
    case 7: {
        const Camera* cam = level_.camera.get();
        return cam && cam->sceneCamera ? cam->sceneCamera->world.translate : ni::NiPoint3{};
    }
    default: return {};
    }
}

ni::NiMatrix3 TriggerManager::Rotation(TriggerRec& rec) const {   // orig: 0x000DB090
    if (!rec.target) return {};
    switch (rec.kind) {
    case 1:
    case 6: return static_cast<const GameObject*>(rec.target)->GetWorldRotation();   // vf38
    case 5: return static_cast<const Character*>(rec.target)->GetNode()->world.rotate;   // vf30 -> +0x70
    case 2:
        if (!rec.rotationCached) {                    // orig: new(0x30) MakeYRotation(yaw), kept in +0x14
            rec.waypointRotation = ni::MakeYRotation(static_cast<const StageWaypoint*>(rec.target)->yaw);
            rec.rotationCached = true;
        }
        return rec.waypointRotation;
    case 3:
    case 4: return static_cast<const ni::NiAVObject*>(rec.target)->world.rotate;
    case 7: return level_.camera ? level_.camera->rotation : ni::NiMatrix3{};
    default: return {};
    }
}

void TriggerManager::SetRotation(TriggerRec& rec, const ni::NiMatrix3& R) const {   // orig: 0x000DB1E0
    if (!rec.target) return;
    switch (rec.kind) {
    case 1:
    case 6: static_cast<GameObject*>(rec.target)->SetRotation(R); return;   // vf24
    case 5: static_cast<Character*>(rec.target)->SetRotation(R); return;    // vf24 (0x00033D20)
    case 3:
    case 4: static_cast<ni::NiAVObject*>(rec.target)->local.rotate = R; return;   // +0x30 (rep movsd 12 dwords)
    case 2: {
        // 0x00103290(out, &(0, 0, 1), R): out.z = F((chunk2[0] * 0 + chunk2[2] * 1) + chunk2[1] * 0)
        auto* wp = static_cast<StageWaypoint*>(rec.target);
        using ni::detail::F;
        wp->yaw = F((double(R.m[0][2]) * 0.0f + double(R.m[2][2]) * 1.0f) + double(R.m[1][2]) * 0.0f);   // +0x0C
        rec.waypointRotation = ni::MakeYRotation(wp->yaw);   // (+0x14: new(0x30) on first use, pads 0)
        rec.rotationCached = true;
        return;
    }
    default: return;
    }
}

void TriggerManager::SetPosition(TriggerRec& rec, const ni::NiPoint3& p) const {   // orig: 0x000DA8C0
    if (!rec.target) return;
    switch (rec.kind) {
    case 1: static_cast<GameObject*>(rec.target)->SetPosition(p); return;   // vf1C (tail jump)
    case 5: static_cast<Character*>(rec.target)->SetPosition(p); return;    // vf1C (0x000353B0)
    case 2: static_cast<StageWaypoint*>(rec.target)->position = p; return;  // +0x00
    case 3:
    case 4: static_cast<ni::NiAVObject*>(rec.target)->local.translate = p; return;   // +0x60
    case 6: static_cast<Destructable*>(rec.target)->SetPositionAndUpdate(p); return;   // vf54 (by value)
    default: return;                                  // (kind 7: nothing)
    }
}

void TriggerManager::Translate(TriggerRec& rec, const ni::NiPoint3& d) const {   // orig: 0x000DA960
    if (!rec.target) return;
    if (rec.kind == 5) {                              // +0x18 == 5: vf20 with the copy at esp+4
        static_cast<Character*>(rec.target)->Translate(d);
        return;
    }
    using ni::detail::F;
    const ni::NiPoint3 p = Position(rec);             // 0x000DA7E0
    // FLD dz; FADD p.z; FLD dy; FADD p.y; FLD dx; FADD p.x; FSTP x / y / z
    SetPosition(rec, {F(double(d.x) + p.x), F(double(d.y) + p.y), F(double(d.z) + p.z)});   // 0x000DA8C0
}

ni::NiPoint3 TriggerManager::BoxSize(const TriggerRec& rec) const {   // orig: 0x000DA770
    if (!rec.target || rec.kind != 4) return {};
    return VectorExtra(*static_cast<const ni::NiAVObject*>(rec.target));
}

f32 TriggerManager::Radius(const TriggerRec& rec) const {   // orig: 0x000DA7B0
    if (!rec.target || (rec.kind != 3 && rec.kind != 4)) return 0.0f;
    return VectorExtra(*static_cast<const ni::NiAVObject*>(rec.target)).x;
}

void TriggerManager::ResolveAll() {                   // orig: 0x000DAF40
    for (const auto& r : records) Resolve(*r);
}

void TriggerManager::ApplyTriggerFlags(Stage& stage) {   // orig: 0x000C2BF0
    if (!stage.IsLoaded()) return;
    ni::NiNode* t = TriggersOf(stage);
    if (!t) return;
    for (size_t i = 0; i < t->Children().size(); ++i) {
        ni::NiAVObject* c = t->Children()[i].Get();
        if (!c) continue;
        const size_t colon = c->name.find(':');
        if (colon == std::string::npos || colon + 2 > c->name.size()) continue;
        const char* p = c->name.c_str() + colon + 2;  // "Sphere::Magnet1" -> "Magnet1"
        TriggerRec* rec = FindOrCreate(p);
        if (rec && StrNICmp(p, "magnet", 6) == 0) rec->magnet = 1;
    }
}

}  // namespace fut::game

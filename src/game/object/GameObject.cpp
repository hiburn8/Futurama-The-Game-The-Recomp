// Game objects; see GameObject.h.
#include "game/object/GameObject.h"

#include "engine/ni/NiCollision.h"
#include "game/GameTime.h"
#include "game/defs/Defs.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/level/TriggerManager.h"
#include "game/sound/GameSound.h"

#include <cstdio>

namespace fut::game {

using ni::detail::F;

// ---- Owner ----------------------------------------------------------------------------------------------------------
Owner::~Owner() { DeleteObjects(); }

void Owner::DeleteObjects() {                         // orig: 0x00097600
    while (firstObject) delete firstObject;           // (each destructor unlinks its object)
}

void Owner::AttachObject(GameObject& object) {        // orig: 0x000C2750
    GetNode()->AttachChild(object.GetNode(), true);
}

GameObject* Owner::NextObject(const GameObject* prev) const {   // orig: 0x00097670
    return prev ? prev->ownerNext : firstObject;
}

// ---- GameObject -----------------------------------------------------------------------------------------------------
GameObject::GameObject(LevelData& lvl, Def& d) : level(lvl), def(&d) {   // orig: 0x00096CA0
    // Appended to the definition's instance list.
    if (!d.firstInstance) {
        d.firstInstance = this;
    } else {
        GameObject* tail = d.firstInstance;
        while (tail->defNext) tail = tail->defNext;
        tail->defNext = this;
        defPrev = tail;
    }
    node = new ni::NiNode;
}

GameObject::~GameObject() {                           // orig: 0x00096D90
    level.SetObjFlags(id, level.GetObjFlags(id) & ~1u);
    if (owner) {
        OnDetach();                                   // (the base's: the vtable is the base one here)
        owner->DetachObject(*this);
        if (ownerNext) ownerNext->ownerPrev = ownerPrev;
        if (ownerPrev) {
            ownerPrev->ownerNext = ownerNext;
        } else {
            owner->firstObject = ownerNext;
        }
    }
    ownerNext = ownerPrev = nullptr;
    owner = nullptr;
    if (node && node->parent) node->parent->DetachChild(node.Get());
    node = nullptr;
    if (defNext) defNext->defPrev = defPrev;
    if (defPrev) {
        defPrev->defNext = defNext;
    } else {
        def->firstInstance = defNext;
    }
    // the sounds playing on the object's targetable are stopped (0x00096EA3 FindByTarget 0x000DAF70, then
    // SoundSystem::StopAllOnTargetable 0x000CA690; the manager is not null-tested in the original)
    if (level.triggers)
        if (TriggerRec* r = level.triggers->FindByTarget(this)) level.Sound().StopAllOnTargetable(r);
    if (triggerRec) {
        TriggerRec* rec = triggerRec;
        triggerRec = nullptr;
        rec->Unbind();                                // orig: 0x000DA6D0
    }
}

void GameObject::SetTriggerRec(TriggerRec* rec) {     // orig: 0x00096890
    TriggerRec* old = triggerRec;
    triggerRec = rec;
    if (old) old->Unbind();
}

bool GameObject::Tick() {                             // orig: 0x000630D0
    time = F(double(g_frameStep) + time);
    return true;
}

void GameObject::SetOwner(Owner* newOwner) {          // orig: 0x000967E0
    if (owner) {
        OnDetach();
        owner->DetachObject(*this);
        if (ownerNext) ownerNext->ownerPrev = ownerPrev;
        if (ownerPrev) {
            ownerPrev->ownerNext = ownerNext;
        } else {
            owner->firstObject = ownerNext;
        }
    }
    ownerNext = ownerPrev = nullptr;
    owner = newOwner;
    if (!newOwner) return;
    if (!newOwner->firstObject) {                     // appended at the tail
        newOwner->firstObject = this;
    } else {
        GameObject* tail = newOwner->firstObject;
        while (tail->ownerNext) tail = tail->ownerNext;
        tail->ownerNext = this;
        ownerPrev = tail;
    }
    owner->AttachObject(*this);
    OnAttach();
}

void GameObject::SetId(u32 newId) {                   // orig: 0x00096970
    id = newId;
    if (label && level.triggers) level.triggers->RegisterObject(*this);   // orig: 0x000DBF30
}

void GameObject::UpdateNode() { node->UpdateDownwardPass(time, true); }   // orig: 0x000969A0

void GameObject::SetPosition(const ni::NiPoint3& p) {   // orig: 0x00097140
    node->local.translate = p;
    node->UpdateDownwardPass(time, true);             // orig: NiAVObject::Update(time, true)
    UpdateRoom();
}

void GameObject::Translate(const ni::NiPoint3& d) {   // orig: 0x000969C0
    const ni::NiPoint3 w = GetWorldPosition();
    SetPosition({F(double(w.x) + d.x), F(double(w.y) + d.y), F(double(w.z) + d.z)});
}

void GameObject::SetRotation(const ni::NiMatrix3& m) {   // orig: 0x00096A30
    node->local.rotate = m;
    node->UpdateDownwardPass(time, true);
}

ni::NiPoint3 GameObject::GetWorldPosition() const { return node->world.translate; }   // orig: 0x0004EFF0
ni::NiMatrix3 GameObject::GetWorldRotation() const { return node->world.rotate; }     // orig: 0x0004F020

void GameObject::CreateAmbientLight() {               // orig: 0x00097040
    ambientLight = new ni::NiAmbientLight;
    ambientLight->ambient = {1, 1, 1};
    ++ambientLight->revision;
    ambientLight->switchOn = true;
    ambientLight->AttachAffectedNode(node.Get());
    node->AttachChild(ambientLight.Get(), true);
    node->UpdateProperties();                         // 0x00101EA0
    node->UpdateEffects();                            // 0x00102190
}

void GameObject::InitAmbientLight() {                 // orig: 0x00096BC0
    if (!ambientLight) return;
    ambientLight->ambient = {1, 1, 1};                // orig: (1, 1, 1) at 0x002D7BD0
    ++ambientLight->revision;
    const ni::NiPoint3& w = node->world.translate;
    const ni::NiPoint3 start{w.x, F(double(w.y) + 2.0f), w.z};
    f32 dist = 30.0f;                                 // orig: 0x41F00000
    const ni::RayHit hit = ni::RayCast(start, {0, -1, 0}, dist, true, 3);   // orig: 0x000FB780
    if (!hit) return;
    (void)hit.Surface();                              // orig: HitSurface 0x000FA330 (result unused)
    ambientLight->dimmer = 1.0f;
    ++ambientLight->revision;
    ambientLight->UpdateDownwardPass(0.0f, true);     // orig: NiAVObject::Update(0, true)
}

void GameObject::SetName(const char* newLabel) {      // orig: 0x000968C0
    label.reset();
    if (!newLabel) return;
    label = newLabel;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s::%s(%s)", def->TypeName(), def->name.c_str(), label->c_str());
    node->name = buf;                                 // orig: NiObjectNET::SetName 0x000FE300
    if (id && level.triggers) level.triggers->RegisterObject(*this);   // orig: 0x000DBF30
}

bool GameObject::UpdateRoom() {                       // orig: 0x00096F20
    ni::NiPoint3 p = GetWorldPosition();
    if (def->IsType(2)) return false;                 // players stay under their owner's node
    if (def->IsType(1)) {
        p = CharacterPoint();
    } else {
        p.y = F(double(p.y) + 0.1f);                  // FLD; FADD [0x00293AB0] (0.1f); FSTP
    }
    ni::NiNode* parent = node->parent;
    ni::NiRoom* current = dynamic_cast<ni::NiRoom*>(parent);   // orig: 0x00096A60
    ni::NiRoom* room = level.roomGroup ? level.roomGroup->FindRoomContaining(p) : nullptr;
    if (room == current || !room) return false;
    room->AttachChild(node.Get(), true);
    if (owner && owner->IsOwnerType(2)) {
        Stage* holder = nullptr;                      // orig: 0x000C26C0 / Stage::ContainsRoom 0x000CE410
        for (const auto& st : level.stages)
            if (st->ContainsRoom(room)) {
                holder = st.get();
                break;
            }
        if (!holder) return true;
        if (owner != static_cast<Owner*>(holder)) SetOwner(holder);
    }
    return true;
}

}  // namespace fut::game

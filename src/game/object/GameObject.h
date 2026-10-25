// Game objects: what definitions spawn into a level (statics, pickups, destructables, characters, effects).
// Decompiled from default.xbe; structure and addresses: docs/analysis/game_objects.md.
//
// An Owner (the level object, a Stage, a character's inventory) keeps a list of objects and puts their scene nodes
// under its own node; an object's node then moves into the level room that contains it (UpdateRoom).
//
// Ported so far: the Owner list, the GameObject base (owner / definition lists, id, name, position, rotation, room
// placement, the ambient light, the trigger records), Static (Static.h). NOT PORTED yet: the ground ray cast and
// per-frame update of the ambient light, the sounds stopped on deletion,
// the other derived classes (Pickup, Destructable, characters, effects, ...).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <optional>
#include <string>

namespace fut::game {

class Def;
class GameObject;
class LevelData;
struct TriggerRec;

class Owner {                                         // orig: vtbl 0x002A12B4, ctor 0x000975F0, 8 bytes in its holder
public:
    Owner() = default;
    Owner(const Owner&) {}                            // (a copy owns no objects)
    Owner& operator=(const Owner&) { return *this; }
    virtual ~Owner();                                 // orig: 0x00097600: deletes every object of the list
    // orig: vf04 0x000C2750: the object's node under this owner's node (first free slot).
    virtual void AttachObject(GameObject& object);
    // orig: vf08 (the base does nothing).
    virtual void DetachObject(GameObject& object) { (void)object; }
    // orig: vf0C: `(t & mask) == t` with mask 1 = level object, 2 = stage, 4 = character inventory.
    virtual bool IsOwnerType(u32 t) const = 0;
    virtual ni::NiNode* GetNode() const = 0;          // orig: vf10

    // orig: 0x00097670: the first object (prev = nullptr) or the one after prev.
    GameObject* NextObject(const GameObject* prev) const;
    void DeleteObjects();                             // the destructor's loop (every object unlinks itself)

    GameObject* firstObject = nullptr;                // orig: +4
};

class GameObject {                                    // orig: vtbl 0x002A11E0, ctor 0x00096CA0, 0x38 bytes
public:
    GameObject(LevelData& level, Def& def);           // orig: 0x00096CA0
    virtual ~GameObject();                            // orig: 0x00096D90
    GameObject(const GameObject&) = delete;
    GameObject& operator=(const GameObject&) = delete;

    // slot 1: build the model (called by the spawns); the base's slot is _purecall (the C++ pure virtual).
    virtual bool Init() = 0;                          // orig: 0x00166946
    virtual bool Tick();                              // slot 2 0x000630D0: time += g_frameStep; alive
    virtual bool PostUpdate() { return true; }        // slot 3 0x000A32A0
    virtual void SetOwner(Owner* newOwner);           // slot 4 0x000967E0
    virtual void SetId(u32 newId);                    // slot 5 0x00096970
    virtual void UpdateNode();                        // slot 6 0x000969A0: node->Update(time)
    virtual void SetPosition(const ni::NiPoint3& p);  // slot 7 0x00097140 (then UpdateRoom)
    virtual void Translate(const ni::NiPoint3& d);    // slot 8 0x000969C0
    virtual void SetRotation(const ni::NiMatrix3& m); // slot 9 0x00096A30 (no UpdateRoom)
    virtual void SetFlag28(u8 v) { flag28 = v; }      // orig: slot 10 0x000968B0
    virtual void ApplyInstanceString(const char* s) { (void)s; }   // slot 11 (no-op in the base)
    ni::NiNode* GetNode() const { return node.Get(); }               // slot 12 0x00141ED0
    virtual ni::NiPoint3 GetWorldPosition() const;    // slot 13 0x0004EFF0: the node's world translation
    virtual ni::NiMatrix3 GetWorldRotation() const;   // slot 14 0x0004F020
    virtual void OnAttach() {}                        // slot 15 (after the owner attached the object)
    virtual void OnDetach() {}                        // slot 16 (before the owner detaches it)
    // slot 17 0x00097040: a white ambient light (switched on) affecting the node, as the node's child.
    virtual void CreateAmbientLight();
    // slot 19 0x00096BC0: the light's colour reset to white; then a ray cast from 2 units above the object straight
    // down (30 units, stage triangles facing up): on a hit the dimmer becomes 1.0 and the light is updated at time 0.
    virtual void InitAmbientLight();
    // slot 18 0x00096AA0 (per frame, living characters drawn the last frame): the light's colour a step towards the
    // global ambient (0x000C0410), then the ray of InitAmbientLight: on a hit the dimmer eases towards 1 (x 0.95 +
    // 0.05) and the light is updated at time 0. (Every class keeps the base slot.) Defined in CharacterUpdate.cpp.
    virtual void UpdateAmbientLight();
    // Characters' own reference point (their vtable +0x78), used by UpdateRoom for character definitions.
    virtual ni::NiPoint3 CharacterPoint() const { return GetWorldPosition(); }

    // orig: 0x00096890: the object's trigger record (the old one is unbound).
    void SetTriggerRec(TriggerRec* rec);
    // orig: 0x000968C0: keeps a copy of the label and names the node "<type>::<definition>(<label>)"; a null label
    // changes nothing.
    void SetName(const char* newLabel);
    // orig: 0x00096F20: moves the node into the level room containing the object (players never; characters by
    // their own point, others 0.1 above their position) and, for a stage-owned object, hands the object to the
    // stage that holds that room. Returns whether the node moved.
    bool UpdateRoom();

    LevelData& level;                                 // (the original uses the global level object)
    Owner* owner = nullptr;                           // orig: +0x04
    GameObject* ownerNext = nullptr;                  // orig: +0x08
    GameObject* ownerPrev = nullptr;                  // orig: +0x0C
    Def* def;                                         // orig: +0x10
    GameObject* defNext = nullptr;                    // orig: +0x14
    GameObject* defPrev = nullptr;                    // orig: +0x18
    TriggerRec* triggerRec = nullptr;                 // orig: +0x1C (TriggerManager.h)
    u32 id = 0;                                       // orig: +0x20, the level's object flag slot
    f32 time = 0;                                     // orig: +0x24, passed to every node update
    u8 flag28 = 0;                                    // orig: +0x28
    ni::NiPointer<ni::NiNode> node;                   // orig: +0x2C (unnamed until SetName)
    ni::NiPointer<ni::NiAmbientLight> ambientLight;   // orig: +0x30
    std::optional<std::string> label;                 // orig: +0x34
};

}  // namespace fut::game

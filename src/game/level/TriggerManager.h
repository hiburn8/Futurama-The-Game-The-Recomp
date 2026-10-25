// The trigger manager (level object +0xC0, ctor 0x000DC070): a name -> "targetable" registry that scripts and game
// code use to address anything with a position (objects, characters, destructables, stage waypoints, the trigger
// nodes of the stages' "Triggers" nodes, the camera). Records are created by name, bound to their target by name and
// live until the level dies. Decompiled from default.xbe; docs/analysis/trigger_manager.md.
//
// NOT PORTED yet: the character / destructable lists searched by Resolve (no such objects exist in the port yet), the
// camera, the script triggers of RefreshTriggers, the transform / position helpers and the script natives.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiObjects.h"

#include <array>
#include <deque>
#include <memory>
#include <string>

namespace fut::game {

class GameObject;
class LevelData;
class Stage;

struct TriggerRec {                                   // orig: 0x24 bytes
    u8 magnet = 0;                                    // +0x00
    std::string name;                                 // +0x04 (heap copy)
    bool hasName = false;
    s32 hash = 0;                                     // +0x08 (TriggerManager::Hash(name, true))
    void* target = nullptr;                           // +0x0C: by kind (see TriggerManager)
    s32 id = -1;                                      // +0x10: the bound object's id
    bool rotationCached = false;                      // +0x14 (waypoint rotation, lazily created)
    ni::NiMatrix3 waypointRotation;                   // (the +0x14 matrix: MakeYRotation of the waypoint's yaw)
    u32 kind = 0;                                     // +0x18
    TriggerRec* kindNext = nullptr;                   // +0x1C
    TriggerRec* kindPrev = nullptr;                   // +0x20

    // orig: 0x000DA710 Bind / 0x000DA6D0 Unbind: for object kinds (1 / 5 / 6) the old object's back link is cleared.
    void Bind(void* newTarget);
    void Unbind();
};

class TriggerManager {                                // orig: 0x38 bytes, no vtable
public:
    // Kinds: 0 unbound, 1 GameObject, 2 stage waypoint, 3 trigger node, 4 trigger node named "...Box...",
    // 5 character, 6 destructable, 7 camera ("$cam").
    explicit TriggerManager(LevelData& level) : level_(level) {}

    // orig: 0x000DA500: length + weighted sum of the last 15 characters (signed bytes when case-sensitive).
    static s32 Hash(const char* s, bool caseSensitive);
    TriggerRec* Find(const char* name) const;         // orig: 0x000DAFA0 (hash, then _stricmp; newest first)
    // orig: 0x000DB390: an existing record as it is, else a new one at the head, resolved.
    TriggerRec* FindOrCreate(const char* name);
    void SetKind(u32 kind, TriggerRec& rec);          // orig: 0x000DA660 (front of the kind list; 0 = none)
    TriggerRec* FindByTarget(const void* target) const;   // orig: 0x000DAF70
    void RegisterObject(GameObject& object);          // orig: 0x000DBF30
    void Resolve(TriggerRec& rec);                    // orig: 0x000DA9E0
    void ResolveAll();                                // orig: 0x000DAF40
    // orig: Session::ApplyTriggerFlags 0x000C2BF0: one record per "Triggers" child "<Box|Sphere>::<name>" of a
    // loaded stage; names starting "magnet" (any case) set the magnet flag.
    void ApplyTriggerFlags(Stage& stage);
    // orig: 0x000DA7E0 by kind: objects / characters vf34, the waypoint position, the trigger node's world translate,
    // the camera's NiCamera world translate; zero without a target.
    ni::NiPoint3 Position(const TriggerRec& rec) const;
    // orig: 0x000DA5F0 by kind: objects / characters / destructables vf30 (their node), a trigger node itself; null
    // for a waypoint, the camera or without a target.
    ni::NiAVObject* Node(const TriggerRec& rec) const;
    // orig: 0x000DB090 by kind: objects vf38, the waypoint's yaw (cached), the node's / character node's world rotate,
    // the camera's +0x70; identity without a target.
    ni::NiMatrix3 Rotation(TriggerRec& rec) const;
    // orig: 0x000DB1E0 by kind: objects / characters / destructables vf24, the trigger node's local rotate (+0x30),
    // a waypoint's yaw = (R^T * UNIT_Z).z (sic) with its cached matrix rebuilt; nothing without a target.
    void SetRotation(TriggerRec& rec, const ni::NiMatrix3& R) const;
    // orig: 0x000DA8C0 by kind: objects / characters vf1C, the waypoint position, the trigger node's local translate
    // (+0x60), a destructable's vf54 (SetPositionAndUpdate); nothing for the camera or without a target.
    void SetPosition(TriggerRec& rec, const ni::NiPoint3& p) const;
    // orig: 0x000DA960 (thiscall(dx, dy, dz), RET 0xC): a character's vf20 (Character::Translate 0x00035400: the body's
    // velocity), else SetPosition(Position + d); nothing without a target.
    void Translate(TriggerRec& rec, const ni::NiPoint3& d) const;
    // orig: 0x000DA770: a kind-4 record's node "#V#" vector (the box size), else zero.
    ni::NiPoint3 BoxSize(const TriggerRec& rec) const;
    // orig: 0x000DA7B0: a kind-3 / 4 record's node "#V#" x (the radius), else 0.
    f32 Radius(const TriggerRec& rec) const;

    std::deque<std::unique_ptr<TriggerRec>> records;  // orig: pointer list +0x04 (head = newest)
    std::array<TriggerRec*, 8> kindHeads{};           // orig: +0x18 + 4k

private:
    LevelData& level_;
};

}  // namespace fut::game

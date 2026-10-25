// The level scripts' trigger objects (orig: the global list 0x002E9FFC, base constructor 0x000B8930; natives 189-217;
// docs/analysis/script_triggers.md). Scripts create triggers (boxes, spheres, buttons, health, NULL, groups, ...) and
// poll them with native 189, which runs the trigger's test; nothing evaluates them per frame. RefreshTriggers (after
// every stage stream) re-binds their trigger records and caches the box / sphere geometry once.
//
// Each trigger is also a block of the original size in the game (VM) heap whose first dword is the original vtable
// address: the scripts hold that address as the handle and the differential trace decodes it; the logic lives in
// ScriptTrigger. The list pool's blocks (new(8) + new(0xC00) per 256 nodes) and the names' heap copies are mirrored
// the same way so the scripts' heap traffic matches.
//
// The health triggers' geometry tests used by the projectiles (fireball_port.md 7): ContainsPoint 0x000B86F0, the
// box ray test 0x000B7F00 and the sphere ray test 0x000B7D80; the laser's trigger damage 0x00050800 (Laser.cpp).
//
// NOT PORTED: the pickup and at-sight tests (their state stays unchanged; reported), the explosions' trigger damage
// 0x0007A690.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiMath.h"
#include "game/script/ScriptVM.h"

#include <deque>
#include <memory>
#include <vector>

namespace fut::game {

class LevelData;
struct TriggerRec;

struct ScriptTrigger {
    enum Kind : u32 { Null = 0, Sphere, Box, Pickup, Button, AtSight, Destructable, Animation, Health, Group };
    script::VmAddr vm = 0;                            // the object (vtable at +0, state +4, kind +8)
    u32 vtbl = 0, kind = Null, state = 0;
    script::VmAddr name0 = 0, name1 = 0;              // heap copies (Sphere +0x14 / +0x18, Box +0x0C / +0x10, ...)
    TriggerRec* rec0 = nullptr;                       // Sphere +0x1C, Box +0x14, AtSight +0x0C, Destr. +0x0C, Health +0x5C
    TriggerRec* rec1 = nullptr;                       // Sphere +0x20, Box +0x18, AtSight +0x10
    f32 distance = 3.4028234663852886e38f;            // Sphere +0x0C (FLT_MAX)
    f32 radius = -1.0f;                               // Sphere +0x10, Health +0x68
    ni::NiPoint3 centre;                              // Sphere +0x24, Box +0x1C, Health +0x0C
    ni::NiMatrix3 rotation;                           // Box +0x30, Health +0x20
    ni::NiPoint3 half;                                // x / y / z = Box +0x64 / +0x60 / +0x68, Health +0x54 / +0x50 / +0x58
    f32 health = 0;                                   // Health +0x6C
    u8 flag64 = 0, flag65 = 0;                        // Health +0x64 / +0x65
    u32 action = 0;                                   // Button +0x0C
    script::VmAddr groupData = 0;                     // Group: the array (+0x10 data, +0x14 capacity, +0x18 size)
    u32 capacity = 1, count = 0;
    std::vector<script::VmAddr> children;
};

// orig: HealthTrigger::ContainsPoint 0x000B86F0 (thiscall(p, margin), RET 8): a sphere when the radius is set (also
// with a box size), else the rotated box when half.y > 0 (NaN included).
bool HealthContainsPoint(const ScriptTrigger& t, const ni::NiPoint3& p, f32 margin);
// orig: HealthTrigger::RayTest 0x000B7F00 (thiscall(o, dir, dist*), RET 0xC). With a radius: the sphere test
// 0x000B7D80 (an origin inside: dist = 0). The box path ignores the rotation: the
// segment o -> o + dist * dir against five faces of the axis-aligned box around the centre (the sixth face's hit is
// computed and never compared, as in the original); a closer hit shortens `dist`.
bool HealthRayTest(const ScriptTrigger& t, const ni::NiPoint3& o, const ni::NiPoint3& dir, f32& dist);

class ScriptTriggers {
public:
    explicit ScriptTriggers(LevelData& level) : level_(level) {}
    ~ScriptTriggers();                                // orig: 0x000B88B0 + 0x0012D2F0 (from ~Session)
    ScriptTriggers(const ScriptTriggers&) = delete;
    ScriptTriggers& operator=(const ScriptTriggers&) = delete;

    // orig: new(size) + the base constructor 0x000B8930 (the list node from the pool, prepended).
    ScriptTrigger& Create(u32 kind, u32 size, u32 vtbl);
    ScriptTrigger* Get(script::VmAddr t);            // nullptr for an unknown handle
    bool Test(script::VmAddr t);                      // orig: native 189 0x000B6FD0 (vf04, then state != 0)
    void RefreshAll();                                // orig: the loop of RefreshTriggers 0x000B72D0 (newest first)
    void Refresh(ScriptTrigger& t);                   // orig: vf08 (0x000B7380 / 0x000B76D0 / 0x000B7A40)
    void Nullify(script::VmAddr t);                   // orig: native 190 0x000B8900 (unlink, delete)
    script::VmAddr CopyName(const char* s);           // orig: new(strlen + 1) + copy; 0 for NULL

    std::deque<std::unique_ptr<ScriptTrigger>> list;  // front = newest (orig: prepend)

private:
    void RunTest(ScriptTrigger& t);                   // orig: vf04 by kind
    void Destroy(ScriptTrigger& t);                   // orig: vf00(1) (the names, the group array, the object)
    LevelData& level_;
    u32 freeNodes_ = 0;
    std::vector<std::pair<void*, void*>> blocks_;     // (header new(8), nodes new(0xC00))
};

}  // namespace fut::game

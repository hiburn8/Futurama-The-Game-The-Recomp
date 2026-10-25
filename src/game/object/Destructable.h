// Destructable: breakable props (boxes, bottles, windows, barrels, pots, mines) and the drop lists they share with the
// NPCs. Decompiled from default.xbe (vtbl 0x0029BA40, ctor 0x00063840, 0x78 bytes; DropList vtbl 0x0029CD74, 0x20 bytes);
// docs/analysis/destructable_moveable_port.md.
//
// Ported: the spawn, Init (switch clone, static collision box, lights), the post-update, the proximity mine's
// arming, the drop-list parse, Damage, Break (the parts' rand() and effects, the replacement definition, the explosive
// player damage, the drops, the mine chain), the break and mine-activation sounds and AddDrop (docs/analysis/
// inventory_batch1_port.md 3). NOT PORTED: the debris objects (Moveable / Debris), kinds 2 / 3 (trip wire, laser fan:
// no shipped definition).
#pragma once

#include "game/object/GameObject.h"
#include "game/physics/Physics.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fut::game {

class DestructableDef;

// orig: the drop list (ctor 0x00075D90(id), dtor 0x00076080): the pickups an object drops (the instance string).
struct DropList {
    explicit DropList(u32 recordId) : recordId(recordId) {}
    struct Entry {                                    // orig: 8 bytes, ctor 0x00075CE0
        std::optional<std::string> label;
        Def* def = nullptr;
    };
    bool Parse(const char* s, const LevelData& level);   // orig: 0x00076310
    bool Add(const char* cls, const char* name, const char* label, const LevelData& level);   // orig: 0x00076170
    // orig: 0x000760E0 (thiscall(def, label), RET 8): a new entry {label copy or none, def} without the class lookup.
    void AddEntry(Def* def, const char* label);
    // orig: 0x00075E60 (thiscall(owner, rot*, pos*), RET 0xC): one random entry (mode_random or a single entry) thrown
    // up at 20, or every entry thrown out on a cone (mode_all; the rotation argument is then unused).
    void SpawnAll(LevelData& level, Owner* owner, const ni::NiMatrix3& rot, const ni::NiPoint3& pos);
    // orig: Entry::Spawn 0x00075DD0 (thiscall(owner, rot*, pos*, vel*, id, index), RET 0x18).
    void SpawnEntry(LevelData& level, s32 k, Owner* owner, const ni::NiMatrix3& rot, const ni::NiPoint3& pos,
                    const ni::NiPoint3& vel);
    std::vector<Entry> entries;                       // orig: +0x04 array / +0x0C size
    u32 mode = 1;                                     // orig: +0x18 (0 = mode_all, 1 = mode_random)
    u32 recordId;                                     // orig: +0x1C
};

class Destructable : public GameObject {
public:
    Destructable(LevelData& level, DestructableDef& def);   // orig: 0x00063840
    ~Destructable() override;                                // orig: 0x00063890
    bool Init() override;                                    // orig: vf04 0x00064E60
    bool PostUpdate() override;                              // orig: vf0C 0x00064E00
    void ApplyInstanceString(const char* s) override;        // orig: vf2C 0x00063550
    void SetPositionAndUpdate(ni::NiPoint3 p);               // orig: vf54 0x00063660 (by value)
    // orig: vf58 0x000634C0 (thiscall(def, label), RET 8): a pickup-family definition appended to the drop list
    // (mode_all); native 76.
    void AddDrop(Def* d, const char* label);
    Destructable* NextDestructable() const { return nextDestructable; }   // orig: vf5C 0x00062FF0
    // orig: vf60 0x000630E0 (thiscall(obj, s32* counter), RET 8): every "Part*" node of the subtree (post-order):
    // four rand() (the throw direction and speed), the debris (D+0x48), the effects (D+0x50), the explosion (D+0x58).
    void SpawnParts(ni::NiAVObject* obj, s32& counter);
    bool Damage(f32 amount);                                 // orig: vf64 0x000635E0
    void Break();                                            // orig: vf68 0x00063A50

    ni::NiPointer<ni::NiSwitchNode> model;            // orig: +0x38
    std::unique_ptr<phys::CollisionBox> box;          // orig: +0x3C
    Destructable* nextDestructable = nullptr;         // orig: +0x40 (list head: LevelData::firstDestructable)
    f32 health = 0;                                   // orig: +0x44 (uninitialised until Init)
    u8 broken = 0;                                    // orig: +0x48
    Def* spawnDef = nullptr;                          // orig: +0x4C (Break spawns it instead of the drops; no writer
    const char* spawnLabel = nullptr;                 // orig: +0x50  found in the functions read, L: CreateFromString)
    u8 mineArmed = 0;                                 // orig: +0x70
    std::unique_ptr<DropList> drops;                  // orig: +0x74

private:
    void UpdateMine();                                // orig: 0x00063780 (kind 1)
    void ActivateMine();                              // orig: 0x00063420
};

// orig: 0x00063000 (cdecl, recursive): every LOOP controller of the subtree (children first, then the object's chain,
// then its material property's chain) becomes CLAMP.
void ClampLoopingControllers(ni::NiAVObject* obj);

}  // namespace fut::game

// SimpleNpc: the level5 boss's wasp ("kamikaze", SimpleNPCDef). Decompiled from default.xbe (ctor 0x0005F860, vtbl
// 0x0029B888, 0x800 bytes); docs/analysis/simplenpc_moveable_port.md 2.
//
// The wasp flies a chain of quadratic Bezier segments between random points of a box 10 units inside the level's
// "Kami_Bound" record, then dives along a ray cast at the first Fry player and explodes at its end (or when it touches
// a character, or a lethal hit). Its only creator is native 0:308 FsLevel5SpawnWasp (the Destructor's RunFry2 thread
// in level5-3). It has no AI agent and no text keys; its Update (vf58) replaces Character::Update entirely.
#pragma once

#include "game/defs/CharacterDef.h"
#include "game/object/Character.h"

namespace fut::game {

// orig: DAT_002EAA44, the box record name of native 309 (a process global; null = "Kami_Bound").
extern const char* g_waspBox;

class SimpleNpc : public Character {
public:
    SimpleNpc(LevelData& level, SimpleNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);   // 0x0005F860
    ~SimpleNpc() override;                            // orig: 0x000603E0
    bool Init() override;                             // orig: vf04 0x0005FEE0
    bool PostUpdate() override;                       // orig: vf0C 0x00060220
    void Think(f32 dt) override;                      // orig: vf54 0x00060200
    bool Update(f32 dt, int pass) override;           // orig: vf58 0x00060480
    void UpdateAnimation(f32 dt) override { AnimTick(dt); }   // orig: vf5C = Character's AnimTick 0x0003B5E0
    void UnloadModel() override;                      // orig: vf68 0x0005FE40
    void Damage(f32 amount, Character* attacker) override;   // orig: vf7C 0x00060110
    void ApplyHit(GameObject* t, ni::NiPoint3& dir, u32 n) override;   // orig: vfD4 0x00060030
    virtual void Fly(f32 dt);                         // orig: vfEC 0x00060740
    // orig: vfF0 0x0005F750 (thiscall(key), RET 4; no caller in the game): _stricmp(key, "morph:lfdown" 0x002974DC),
    // the raw result (0 = the key).
    virtual s32 CompareLeftFootKey(const char* key) const;
    void Die();                                       // orig: 0x0005FF00
    ni::NiPoint3 RandomPoint() const;                 // orig: 0x0005F7A0 (thiscall(out), RET 4)
    SimpleNPCDef& simpleDef() const { return static_cast<SimpleNPCDef&>(cdef); }

    u32 voiceHover = 0, voiceWarning = 0, voiceDive = 0;   // orig: +0x7A0 / +0x7A4 / +0x7A8
    ni::NiPoint3 home;                                // orig: +0x7B0 (the centre the flight points are drawn around)
    ni::NiPoint3 p0, p1, p2;                          // orig: +0x7BC / +0x7C8 / +0x7D4 (the segment's start, control, end)
    f32 t = 0;                                        // orig: +0x7E0 (the segment parameter)
    f32 speed = 0;                                    // orig: +0x7E4 (per second)
    u32 state = 0;                                    // orig: +0x7E8 (0 new, 1 hovering, 2 diving)
    s32 segmentsLeft = 0;                             // orig: +0x7EC
    u32 shown = 0;                                    // orig: +0x7F0 (the model parts switched)
};

// orig: native 0:308 FsLevel5SpawnWasp 0x000C4810 (thiscall S): a "kamikaze" at the first "Destructor" character's
// "booom" node, owned by the Destructor's owner.
void SpawnWasp(LevelData& level);
// orig: native 0:310 FsLevel5DeleteWasps 0x000C3030 (thiscall S): every wasp of S+0xA0 deleted.
void DeleteWasps(LevelData& level);
// orig: 0x000C2F70 (thiscall S, (name)): an explosion of kind 1 at the record (a kind-5 record: its character's point
// 0x0003D7C0), with its sound; nothing without the record (FindOrCreate(NULL) returns NULL).
void ExplodeAtCharacterRecord(LevelData& level, const char* name);

}  // namespace fut::game

// Generic NPCs: the characters GenericNPCDef spawns (orig: base ctor 0x00047B80, vtbl 0x00297508) and the two classes
// of level2-5's opening, Nibbler (0x0008B1D0) and Tank (0x00091F60); their AI agent (0x0002E810). Decompiled from
// default.xbe; docs/analysis/npc_spawn_port.md.
//
// Ported: the spawn, construction, Init / BuildModel, SetId (the enemy list), the think with the agent's states
// (idle 1, alert 2 and combat 3 with the line-of-sight cast, the steering, the AI-net path finder, the noise / hearing
// memory and the melee attack request; docs/analysis/combat_ai_port.md), the idle animation, the close attacks with
// their hit window and damage, the tank's animation hold and Move, the render prep, the classes of
// docs/analysis/npc_classes_batch2_port.md (Stormtrooper, ToxicWarrior4, WeaselRoll, Junkbot2, Destructor, HighPriest,
// SkeletonGuard), GenericNpc / SkeletonRanged damage and the dying animation, the ranged attacks (vf100 / vfFC / vf104,
// the attack and idle-tail animation blocks, "morph:firew") and the damage slots of docs/analysis/npc_combat2_port.md.
// The script-spawned classes of docs/analysis/npc_scripted_port.md (TurdManBig, Warthog, PatrolGuard, LavaMan,
// ChickenWalkerNpc, Crocodile, SpinBot, StatueHead, IgnerBoss) and the Tank's AimController::Update 0x0003B210.
// NOT PORTED: vf104's pellet kinds, the voices.
#pragma once

#include "engine/ni/NiObjects.h"
#include "game/object/Character.h"
#include "game/object/Destructable.h"                // DropList

#include <array>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fut::game {

struct TriggerRec;

class GenericNPCDef;
class GenericNpc;
struct AiGroup;
struct AiGroupMember;

// orig: AimController (ctor 0x00037520, vtbl 0x00295AD0, 0x8C bytes): turns target nodes towards a character at render.
class AimController : public ni::NiTimeController {
public:
    static constexpr std::string_view kClassName = "AimController";
    std::string_view ClassName() const override { return kClassName; }
    explicit AimController(Character& c);            // orig: 0x00037520
    // orig: vf54 0x0003CF90 (thiscall(towards, node), RET 8): nothing for a null node; else the character to face
    // (+0x3C, every call) and the node appended to +0x40 unless it is there already.
    void AddTarget(Character* towards, ni::NiAVObject* node);
    // orig: vf14 0x0003B210 (thiscall(f32), RET 4; the time is not read): every node's local rotation becomes the
    // horizontal heading towards `towards` in the character's frame (times the node's own rotation when it has
    // controllers).
    void Update(f32 time) override;
    Character* character;                             // orig: +0x38
    Character* towards = nullptr;                     // orig: +0x3C
    std::vector<ni::NiAVObject*> nodes;               // orig: the array +0x40 (data +0x44, count +0x4C)
    // (the arrays +0x58 / +0x70 are created by the constructor and never filled by the ported code, H: 0x0003B210)
};

// orig: a sequence entry / the agent's current event (0x20 bytes; A+0x1D8 is the current one).
struct AiEvent {
    u32 kind = 0;                                     // +0x00 (1 Wait .. 0xF; 0xE = fetch the next entry)
    u32 arg = 0;                                      // +0x04 (low byte: Guard / PlayerRun)
    f32 value = 0;                                    // +0x08
    u32 w0C = 0;                                      // +0x0C (never written)
    u32 anim = 0;                                     // +0x10 (Animate: the script's name pointer, a VM address)
    ni::NiPoint3 point;                               // +0x14
};
static_assert(sizeof(AiEvent) == 0x20);
inline const AiEvent kAiZeroEvent{};                 // orig: 0x002DC554

struct AiSequence {                                   // orig: 0x210 bytes, ctor 0x00032A30
    u8 loop = 1, pingPong = 0;                        // +0x00 / +0x01
    s32 step = 1, cur = -1, count = 0;                // +0x04 / +0x08 / +0x0C
    std::array<AiEvent, 16> entries{};                // +0x10
    void Reset() { step = 1; cur = -1; count = 0; }   // orig: 0x00032730
    const AiEvent& Current() const {                  // orig: 0x000329A0
        return cur < count && cur >= 0 ? entries[static_cast<size_t>(cur)] : kAiZeroEvent;
    }
    const AiEvent& Next() {                           // orig: 0x000329C0
        const s32 oldStep = step;
        cur += oldStep;
        if (cur >= count || cur < 0) {
            if (loop) {
                if (pingPong) {
                    step = -oldStep;
                    cur = step > 0 ? 1 : count - 2;
                } else {
                    step = 1;
                    cur = 0;
                }
            }
        }
        return Current();
    }
    AiEvent* Append() { return count < 16 ? &entries[static_cast<size_t>(count)] : nullptr; }   // (count++ by the adders)
    void AddWait(f32 v) { if (AiEvent* e = Append()) { e->kind = 1; e->value = v; ++count; } }      // 0x00032750
    void AddPrecision(f32 v) { if (AiEvent* e = Append()) { e->kind = 0xC; e->value = v; ++count; } }   // 0x00032780
    void AddGoto(const ni::NiPoint3& p, f32 prec) {                                                // 0x000327B0
        if (AiEvent* e = Append()) { e->kind = 2; e->point = p; e->value = prec; ++count; }
    }
    void AddWarp(const ni::NiPoint3& p) { if (AiEvent* e = Append()) { e->kind = 7; e->point = p; ++count; } }   // 0x00032800
    void AddLook(const ni::NiPoint3& p) {                                                          // 0x00032840
        if (AiEvent* e = Append()) { e->kind = 3; e->point = p; e->value = 0.0f; ++count; }
    }
    void AddLookTarget(f32 v) { if (AiEvent* e = Append()) { e->kind = 4; e->value = v; ++count; } }   // 0x00032890
    void AddGuard(u8 b) { if (AiEvent* e = Append()) { e->kind = 5; SetLowByte(e->arg, b); ++count; } }   // 0x000328C0
    void AddPlayerRun(u8 b) { if (AiEvent* e = Append()) { e->kind = 8; SetLowByte(e->arg, b); ++count; } }   // 0x000328F0
    void AddPlayerFreeze() { if (AiEvent* e = Append()) { e->kind = 9; ++count; } }                // 0x00032920
    void AddReset() { if (AiEvent* e = Append()) { e->kind = 0xA; ++count; } }                     // 0x00032940
    void AddAnimate(u32 id) { if (AiEvent* e = Append()) { e->kind = 6; e->anim = id; ++count; } }   // 0x00032960
    static void SetLowByte(u32& w, u8 b) { w = (w & 0xFFFFFF00u) | b; }   // (a byte store: +5..+7 keep their bytes)
};

// The level's AI nets (Stage::LoadAINets 0x000CE760; FindAINet 0x000313E0 in LevelData).
struct AiNetNode {                                    // orig: 0x50 bytes, ctor 0x00031C50
    s32 count = 0;                                    // +0x00 links set
    struct Link {
        s32 node = -1;
        f32 cost = -1.0f;
    };
    std::array<Link, 8> links{};                      // +0x04 + 8k (stored in their file slot)
    ni::NiPoint3 pos;                                 // +0x44
};
struct AiNet {                                        // orig: 0x20 bytes, ctor 0x00031C80
    std::vector<AiNetNode> nodes;                     // +0x00 count, +0x04 new(count * 0x50)
    std::array<char, 16> name{};                      // +0x08 strncpy(.., 15)
    // orig: 0x00031D80: the node nearest to `p` (squared distance; ties and NaN take the later node), null if none.
    AiNetNode* Nearest(const ni::NiPoint3& p);
};

// orig: 0x00031C00: link slot i's node index, or the static dummy 0x002D6208 {-1, -1.0f} when i is outside [0, count).
inline s32 LinkNode(const AiNetNode& nd, s32 i) {
    return i >= 0 && i < nd.count && i < static_cast<s32>(nd.links.size()) ? nd.links[static_cast<size_t>(i)].node
                                                                           : -1;
}
// orig: 0x00031C20: the node, or null (the walks dereference it without a test).
inline AiNetNode* NodeAt(AiNet& net, s32 i) {
    return i >= 0 && i < static_cast<s32>(net.nodes.size()) ? &net.nodes[static_cast<size_t>(i)] : nullptr;
}
inline const AiNetNode* NodeAt(const AiNet& net, s32 i) { return NodeAt(const_cast<AiNet&>(net), i); }

// orig: the 0x10-byte record of the path finder's pool. After a successful search the `parent` links point from the
// start towards the goal ("next").
struct AiPathRec {
    AiNetNode* node = nullptr;                        // +0x0
    f32 g = 0, f = 0;                                 // +0x4 / +0x8
    AiPathRec* parent = nullptr;                      // +0xC
};

// orig: the containers of the path finder (vtbls 0x00295658 / 0x00295664 / 0x00295670): {vtbl, u8 grow, count, cap,
// data}. Every access clamps an out-of-range index to element 0, as the original's accessors do.
template <class T>
struct AiArray {
    AiArray(bool g, u32 cap) : grow(g), data(cap) {}
    T& At(u32 i) { return i < data.size() ? data[i] : data[0]; }
    T* Push(const T& v) {                             // orig: 0x00031E50 (records) / 0x00031FE0 (pointers)
        ++count;                                      // (before the capacity test: stays incremented on failure)
        if (count > data.size()) {
            if (!grow) return nullptr;
            data.resize(count + 5);                   // 0x00031ED0 / 0x0002CD90: new(n * size), copy, delete
        }
        T& e = At(count - 1);
        e = v;
        return &e;
    }
    void RemoveAtSwap(u32 i) {                        // orig: 0x00031F90 / 0x00032050
        if (count <= i) return;
        --count;
        At(i) = At(count);
    }
    void Clear() { count = 0; }                       // orig: 0x00031E40
    bool grow;                                        // +0x04
    u32 count = 0;                                    // +0x08
    std::vector<T> data;                              // +0x10 (size() = the capacity +0x0C)
};

// orig: the agent's path finder A+0x84 (ctor 0x00032580 -> 0x00032240(200), vtbl 0x00295684): an A* search over an
// AI net with a min-heap open list.
struct AiPathFinder {
    AiNetNode* goal = nullptr;                        // +0x04 (A+0x88)
    AiNetNode* start = nullptr;                       // +0x08 (A+0x8C)
    AiPathRec* path = nullptr;                        // +0x0C (A+0x90): the found path (its start record)
    // (the pool never grows: its records stay where the open / closed lists point)
    AiArray<AiPathRec> pool{false, 200};              // +0x10
    AiArray<AiPathRec*> open{true, 200};              // +0x24 (a binary min-heap on f)
    AiArray<AiPathRec*> closed{true, 200};            // +0x38
    AiNet* net = nullptr;                             // +0x4C (= A+0xD0; AISetMap 0x00031E10)

    bool Search(AiNetNode* s, AiNetNode* g);          // orig: 0x0002E450
    AiPathRec* Step(u8& exhausted, s32 n);            // orig: 0x0002DD10
    void Expand(AiPathRec* r);                        // orig: vf0 0x00032480
    void Consider(const AiPathRec& n);                // orig: 0x00032320
    void HeapPush(AiPathRec* r);                      // orig: 0x0002DDD0
    void HeapRemoveAt(u32 i);                         // orig: 0x000320F0
    void SiftDown(u32 i);                             // orig: 0x00032130
    static bool Find(AiArray<AiPathRec*>& a, const AiPathRec& r, u32& idx);   // orig: 0x00032090
};

struct AiHeard {                                      // orig: 0x14 bytes at A+0x144 + 0x14 i
    f32 strength = 0;                                 // +0x00
    ni::NiPoint3 pos;                                 // +0x04
    u8 picked = 0;                                    // +0x10
};

// orig: the AI agent (0x8A8 bytes, ctor 0x0002E810); one per character with an agent, listed newest first at
// (S+0x114)+4 (next +0x884, prev +0x888).
struct AiAgent {
    AiAgent(LevelData& level, Character& owner);     // orig: 0x0002E810
    ~AiAgent();                                       // (unlinks itself; the original dtor is not traced)
    AiAgent(const AiAgent&) = delete;
    AiAgent& operator=(const AiAgent&) = delete;
    void Update(f32 dt);                              // orig: 0x0002FDE0
    void ClearIntents();                              // orig: 0x0002CB10
    bool Tick();                                      // orig: 0x0002ECB0 (reached path)
    void State1();                                    // orig: 0x0002F570
    Character* Hunt();                                // orig: 0x0002E2E0 (the target found, or nullptr)
    bool Process();                                   // orig: 0x0002E9C0
    // orig: 0x0002D360 (thiscall(const NiPoint3*), RET 4; ST0): turn towards a point (the direction is cached), then
    // TurnToDir; returns TurnToDir's remaining angle.
    f32 TurnTo(const ni::NiPoint3& p);
    // orig: 0x0002EEA1..0x0002EEED (= 0x0002EF37..0x0002EF83): Tick's turn of the types 0xE / 0xC towards the player.
    void TurnToPlayer(Player& pl);
    // orig: 0x0002DF50 (thiscall(const NiPoint3* from, NiMatrix3* rot, flag), RET 0xC; the flag is not read): the shot
    // matrix of the types 0xE (the Destructor: at the current event's point + 5 up while the event is 3, else at the
    // player) and 0xC (the sungod: at the player, + its def's +0x9C up): columns (d, S x d, -S) with S = unit(up x d),
    // up = (0, 0, 1) when d.y > 0.9 else (0, 1, 0); then the NPC's +0x7BE = 1. The other types: nothing.
    void Aim(const ni::NiPoint3& from, ni::NiMatrix3& rot);
    void State2();                                    // orig: 0x0002F5F0 (reached path)
    void State3();                                    // orig: 0x0002F720 (reached path)
    void Engage(Character& t);                        // orig: 0x0002CC80
    // orig: 0x0002DEC0 (thiscall(f32 amount, Character* attacker), RET 8): the NPC damage slots' hook: +0x230 adds
    // the amount; a player attacker alerts an idle agent, profile[17] engages the attacker.
    void OnDamage(f32 amount, Character* attacker);
    void AlertEngage(Character& p);                   // orig: 0x0002D980
    void Calm();                                      // orig: 0x0002CD30
    f32 MoveTo(const ni::NiPoint3& goal);             // orig: 0x0002E700
    bool CanSee(const ni::NiPoint3& t);               // orig: 0x0002D030
    void SetProfile(const std::array<u32, 26>& p);    // orig: 0x0002CF00 (native 372)
    f32 TurnToDir(const ni::NiPoint3& d);             // orig: 0x0002D1B0 (the result is ST0)
    f32 Steer(const ni::NiPoint3& goal);              // orig: 0x0002D4D0
    bool Repath(const ni::NiPoint3& goal);            // orig: 0x0002E560
    ni::NiPoint3 ProbeGoal(const ni::NiPoint3& goal, AiNetNode* node);   // orig: 0x0002DAA0
    bool AdvancePath();                               // orig: 0x0002E3C0
    bool NextPathNode();                              // orig: 0x0002CD70
    bool Accepts() const;                             // orig: 0x0002D000
    void Hear(const ni::NiPoint3& p, f32 loud);       // orig: 0x0002CF70
    void HeardAdd(const ni::NiPoint3& p, f32 f);      // orig: 0x00032AD0
    void HeardDecay(const ni::NiPoint3& at, f32 amount);   // orig: 0x00032C30
    bool HeardPick(f32 threshold, ni::NiPoint3& out); // orig: 0x00032A60
    bool Check();                                     // orig: 0x0002EC20 (State1's hearing check)

    LevelData& level;
    ni::NiPoint3 pos, axisX, axisZ, axisY;            // +0x00 / +0x0C / +0x18 / +0x24
    f32 acc = 0;                                      // +0x30
    u8 canSee = 1;                                    // +0x34
    ni::NiPoint3 seenTarget, seenFrom;                // +0x38 / +0x44
    ni::NiPoint3 turnDir{1.0f, 0.0f, 0.0f};           // +0x50 (TurnTo's cached direction; ctor 0x0002E8F8)
    ni::NiPoint3 turnTarget, turnFrom;                // +0x5C / +0x68 (TurnTo's cache keys; ctor: zero)
    u8 b80 = 0;                                       // +0x80
    AiPathFinder finder;                              // +0x84..+0xD3 (its net is A+0xD0)
    std::array<u32, 26> profile;                      // +0xD4 (0x000325A0 defaults)
    s32 state = 1;                                    // +0x13C (also the hearing memory's first field)
    s32 smCount = 0;                                  // +0x140 (the hearing memory's count)
    std::array<AiHeard, 5> heard{};                   // +0x144
    f32 f1B0 = 0;                                     // +0x1B0 (the avoidance timer; MoveTo re-paths at 2.0)
    u8 enabled = 1;                                   // +0x1A8
    u8 b1B4 = 0;                                      // +0x1B4
    AiEvent event;                                    // +0x1D8
    f32 timer200 = 0, f204 = 1.0f, f208 = 5.0f, f20C = 5.0f;   // +0x200..+0x20C
    f32 f238 = 1.0f;                                  // +0x238
    ni::NiPoint3 pos23C;                              // +0x23C
    AiSequence seq248, seq458, seq668;                // +0x248 / +0x458 / +0x668 (seq458.loop = 0 in the ctor)
    u8 b74 = 0;                                       // +0x74 (AISetDebug)
    u8 guard = 0, playerRun = 0;                      // +0x1A9 / +0x1AA
    f32 loseTimer = 0;                                // +0x1B8
    ni::NiPoint3 targetPos;                           // +0x1C8
    Character* target = nullptr;                      // +0x1D4
    ni::NiPoint3 jitter;                              // +0x1BC (the ranged aim jitter)
    f32 attackTime = 0;                               // +0x1FC (the attack request time)
    ni::NiPoint3 strafePos;                           // +0x210
    f32 strafeTime = 0, strafeWait = 0;               // +0x21C / +0x220
    f32 f228 = 0, f22C = 0;                           // +0x228 / +0x22C
    f32 f230 = 0;                                     // +0x230 (the damage taken: OnDamage; Tick's def +0x180 branch
                                                      //  warps above 10)
    // +0x1AC / +0x234: the def +0x180 branch's summon timer and last warp index (the high priest). The constructor
    // writes neither (heap garbage in the original: the tests inject the AGENTUNINIT lines; else 0, L).
    f32 summonTimer = 0;                              // +0x1AC
    s32 warpIndex = 0;                                // +0x234
    AiPathRec* pathRec = nullptr;                     // +0x88C (the current path record)
    // +0x890 (the ground-probed goal) / +0x89C (the last re-path's goal): the constructor writes neither (heap garbage
    // in the original: the tests inject the AGENTGOALUNINIT lines; else 0, L)
    ni::NiPoint3 goalProbe, lastGoal;
    Character* character = nullptr;                   // +0x878
    GenericNpc* npc = nullptr;                        // +0x87C
    Player* player = nullptr;                         // +0x880
    AiGroup* group = nullptr;                         // +0x78 (its AI group; cleared by the ctor 0x0002E8B9)
    AiGroupMember* member = nullptr;                  // +0x7C (its member record; setter 0x0015E3D0)
};

// orig: an AI group's member record (0x3C bytes, ctor 0x00030060, unlink 0x000300B0); natives_batch_a_port.md 4.1.
struct AiGroupMember {
    ni::NiPoint3 slot;                                // +0x00 (the role-4 formation slot; not set by the ctor: 0 here,
                                                      //  written by Formation before any read)
    f32 w2 = 0, w3 = 0, w4 = 0;                       // +0x0C / +0x10 / +0x14 (native 364's a / b / c, raw dwords)
    f32 cap4 = 0;                                     // +0x18 (364's d: the capacity as a role-4 leader)
    s32 role = 0;                                     // +0x1C (0..4)
    f32 sum4 = 0, fill4 = 0;                          // +0x20 / +0x24 (as a leader)
    AiAgent* agent = nullptr;                         // +0x28
    AiAgent* leader = nullptr;                        // +0x2C (role 4)
    AiGroup* group = nullptr;                         // +0x38 (+0x30 / +0x34: the group's list)
    void* block = nullptr;                            // (the original's new(0x3C) in the game heap; freed by Detach)
};

// orig: an AI group (0x34 bytes, ctor 0x000301B0, dtor 0x00030220): natives 363-367, updated every play step by the AI
// manager 0x00031850 (newest first); natives_batch_a_port.md 4.
struct AiGroup {
    explicit AiGroup(const char* n);                  // orig: 0x000301B0 (+0x20 = new(strlen + 1) copy, 0x0002FFD0)
    ~AiGroup();                                       // orig: 0x00030220 (frees the name) + the group's operator_delete
    AiGroup(const AiGroup&) = delete;
    AiGroup& operator=(const AiGroup&) = delete;
    void Update(LevelData& level);                    // orig: 0x00030E30
    // orig: 0x00030270 (thiscall, RET 0x14): the agent joins (its record is found or made at the head); the four raw
    // dwords go to w2 / w3 / w4 / cap4.
    void AddMember(AiAgent* a, u32 w2, u32 w3, u32 w4, u32 cap4);
    void Detach(AiAgent& a);                          // orig: 0x000304F0 (native 365, the agent's dtor 0x0002E640)

    u8 active = 0;                                    // +0x00 (367)
    f32 timer = 0;                                    // +0x04 (the evaluation every 4 s)
    s32 w08 = 1;                                      // +0x08 (no reader found)
    s32 mode = 0;                                     // +0x0C (1: role 2 falls back to 1 at an evaluation; no writer
                                                      //  besides the ctor found, L)
    f32 cap3 = 0, cap2 = 0;                           // +0x10 / +0x14 (366's 2nd / 1st argument)
    f32 fill3 = 0, fill2 = 0;                         // +0x18 / +0x1C (not set by the ctor: 0 in the fresh block, O)
    std::optional<std::string> name;                  // +0x20 (none for a NULL name: never found by 0x00031870)
    std::list<std::unique_ptr<AiGroupMember>> members;   // +0x24 (head insert: newest first)
    u8 debug = 0;                                     // +0x30 (the debug lines; no writer besides the ctor found, L)
    void* block = nullptr;                            // (the original's new(0x34) / name blocks in the game heap)
    void* nameBlock = nullptr;

private:
    void Evaluate(const ni::NiPoint3& pp);            // orig: 0x00030E77..0x00030F55 (the 4-second part of Update)
    void Fill3();                                     // orig: 0x00030300
    void Fill2();                                     // orig: 0x00030380
    void Fill4(AiGroupMember* m);                     // orig: 0x00030400
    AiGroupMember* Pick2(const ni::NiPoint3& pp);     // orig: 0x00030980
    AiGroupMember* Pick3(const ni::NiPoint3& pp);     // orig: 0x000307D0
    AiGroupMember* Pick4(AiGroupMember* leader, const ni::NiPoint3& pp);   // orig: 0x00030B30
    void Formation(AiGroupMember& m, const ni::NiPoint3& pp);   // orig: 0x00030540
    AiGroupMember* NearestFollower(const AiGroupMember& leader, const ni::NiPoint3& p);   // orig: 0x00030100
};

// orig: the AI manager's noise 0x00031420 (thiscall(S+0x114, const NiPoint3*, f32), RET 8): every accepting agent
// (head first) hears it.
void AiNoise(LevelData& level, const ni::NiPoint3& pos, f32 loud);

// orig: 0x000325A0, the agent's default profile (script_triggers.md 5).
extern const std::array<u32, 26> kAIDefaultProfile;
// orig: 0x00032630: the profile copy; [4] / [5] / [20] / [21] become the squares of [2] / [3] / [18] / [19].
std::array<u32, 26> CopyAIProfile(const std::array<u32, 26>& src);

// orig: Session 0x000C2DE0 (thiscall(name), RET 4; native 285 FsCreateGroundExplosion, kind 0) and its copies with
// another mesh slot: 0x000C2E30 (286 Big, kind 6), 0x000C2ED0 (287 Huge, 8), 0x000C2E80 (288 Green, 7), 0x000C2F20 (3):
// the explosion (with its sound) at the trigger record named `name`, which FindOrCreate creates when it is missing
// (also without an explosion definition).
void ExplodeAtRecord(LevelData& level, const char* name, s32 kind = 0);

class GenericNpc : public Character {                 // orig: 0x00047B80 (0x8C0 bytes)
public:
    GenericNpc(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    ~GenericNpc() override;
    GenericNPCDef& npcDef() const;
    bool Init() override;                             // orig: vf04 0x00047FE0
    void SetId(u32 newId) override;                   // orig: vf14 0x00046770
    void ApplyInstanceString(const char* s) override; // orig: vf2C 0x00047AE0
    bool PostUpdate() override;                       // orig: vf0C 0x000478A0
    void Think(f32 dt) override;                      // orig: vf54 0x0004A000
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00048190
    void AfterAnimation() override;                   // orig: vf60 0x000467E0
    void BuildModel() override;                       // orig: vf64 0x00047850
    bool ShouldUnloadModel() const override;          // orig: vf6C 0x000467B0
    virtual void AiTick();                            // orig: vfF0 0x00046580
    virtual void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot);   // orig: vfF4 (GenericNpc 0x00090F50)
    virtual void PostThink();                         // orig: vfF8 0x00049040
    void IdleStep(const GenericNPCDef& d);            // orig: 0x00048E19..0x00049022 (the idle tail of 0x00048190)
    void OnTextKey(f32 keyTime, const char* text) override;   // orig: vfE8 0x00047550
    void UnloadModel() override;                      // orig: vf68 0x000466A0 (stop the voices, then 0x0003B7D0)
    bool CanBePushed() const override;                // orig: vfA4 0x00047880
    // orig: vfB0 0x00047830: the base test and the definition's byte +0x18D (flags170[0x1D], 1 by default).
    bool IsTargetable() const override;
    void DealDamage(Character* t, f32 amount) override;   // orig: vfCC 0x00046C70
    void ApplyHit(GameObject* t, ni::NiPoint3& dir, u32 n) override;   // orig: vfD4 0x00049B90
    void HitSpheres(u32 n) override;                  // orig: vfD8 0x00046C40
    void Damage(f32 amount, Character* attacker) override;   // orig: vf7C 0x00047240
    // orig: vfB8 0x00046DF0 (inventory_batch1_port.md 7.1): a character on this NPC's collision mesh or a laser hit on
    // it: the D+0x18C knock-back, the Destructor's / type 10's hit, the tentacle dude's and the aquafish's damage.
    void OnMeshContact(Character* other, f32 amount, bool flag) override;
    // orig: vfBC 0x00049190 (name L; no `call [reg+0xBC]` site on a character found): vfFC from the first tip.
    virtual void AttackFromTip(u32 kind);
    // orig: vfEC 0x00047A40 (thiscall(def, label), RET 8): Destructable::AddDrop's body on +0x8AC (native 76).
    virtual void AddDrop(Def* d, const char* label);
    // orig: vf110 0x000465F0 (name L): full health and IngameEvent category 10 unless the revive animation 99 plays;
    // true once it has finished.
    virtual bool Revive();
    // The ranged attack (docs/analysis/npc_combat2_port.md 1): vf100 builds the aim matrix (columns dir, c x dir, -c
    // with c = unit(up x dir)) at the first tip and calls vfFC, which stores the transform and requests attack 1 / 2
    // (+0x506 / +0x507) unless an attack animation plays; the animation's "morph:firew" key calls vf104.
    virtual void StartAttack(u32 kind, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);   // orig: vfFC 0x00046AA0
    virtual void Fire(u32 kind, const ni::NiPoint3& dir);                                   // orig: vf100 0x00049260
    // orig: vf104 0x00049420 (kind, pos, rot, silent, t0): the definition's projectile D+0x158 (D+0x15C for kind
    // 0x11) with the shoot sound 4, then the muzzle flare D+0x160 and the particles D+0x164.
    virtual void Shoot(u32 kind, const ni::NiPoint3& pos, const ni::NiMatrix3& rot, bool silent, f32 t0);

    u32 count7B0 = 0, count7B4 = 0;                   // orig: +0x7B0 / +0x7B4 (attack 1 / 2 counters; ctor 0)
    u8 b7D4 = 0;                                      // orig: +0x7D4 (vf100: the shot comes from a tip; not set by
                                                      //  the ctor: 0 here)
    ni::NiPoint3 aimPos;                              // orig: +0x7D8 (vfFC / firew; not set by the ctor: 0 here)
    ni::NiMatrix3 aimRot{{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};   // orig: +0x7F0 (3 chunks; not set by the ctor: 0 here)
    u8 b7BE = 0;                                      // orig: +0x7BE
    f32 f7A0 = 0, timer7A4 = -10.0f, f7A8 = 3.0f, timer7AC = 0;   // orig: +0x7A0..+0x7AC
    u8 b7BC = 0, b7BD = 0, b7BF = 0, b7C0 = 0, b7C1 = 0, b7C2 = 0, b7C3 = 0, b7C4 = 1;   // orig: +0x7BC..+0x7C4
    f32 goalHeight = 0;                               // orig: +0x7C8 (Steer: the height to the goal)
    u32 u7CC = 0, serial = 0;                         // orig: +0x7CC / +0x7D0 (DAT_002DD144++)
    u32 voice874 = 0, voice878 = 0, voice87C = 0;     // orig: +0x874 / +0x878 / +0x87C
    u32 voice880 = 0;                                 // orig: +0x880 (ctor 0x00047D5E; the Destructor's door alarm)
    u32 voice884 = 0;                                 // orig: +0x884 (the death sound; the damage slots)
    u8 b870 = 0;                                      // orig: +0x870 (ctor 0x00047DCB; HighPriest: a warp is pending)
    // orig: +0x830 / +0x840: the pending warp's point and a facing rotation (3 chunks: x x up, up, -x with x =
    // unit(up x d), d = the point - the agent's position), both written by AiAgent::Tick's def +0x180 branch (not by
    // the ctor);
    // the warp (HighPriest's morph:hit_floor) reads the point only, nothing reads the rotation.
    ni::NiPoint3 warpPos;
    ni::NiMatrix3 warpRot{{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};
    u32 voice890 = 0;                                 // orig: +0x890 (the contact voice)
    u32 voice894 = 0;                                 // orig: +0x894 (ctor 0x00047D8E; the miss sound)
    std::unique_ptr<DropList> drops;                  // orig: +0x8AC (ApplyInstanceString 0x00047AE0)
    u8 b898 = 0;                                      // orig: +0x898 (the contact sound latch)
    f32 timer89C = 0;                                 // orig: +0x89C (the idle-sound timer; f8A0 = its interval)
    f32 f8A0 = 5.0f;                                  // orig: +0x8A0
};

class Nibbler : public GenericNpc {                   // orig: 0x0008B1D0 (0x8D0 bytes, vtbl 0x0029F408)
public:
    ~Nibbler() override { Nibbler::UnloadModel(); }   // orig: 0x0008B5C0 (vf68 0x0008B410's body, then 0x00047E60)
    Nibbler(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: 0x0008B650
    void AfterAnimation() override;                   // orig: 0x0008B450
    void BuildModel() override;                       // orig: 0x0008B330
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: 0x0008B4D0
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x0008B4A0
    bool IsTargetable() const override { return b8C4 == 0; }   // orig: vfB0 0x0008B1C0 (no base test)
    void UnloadModel() override;                      // orig: vf68 0x0008B410
    void SpawnBoom();                                 // orig: 0x0008B733 / 0x0008B845 (the cage's "Nibbler_Boom")
    void FreedStep();                                 // orig: 0x0008B7D7..0x0008BE30 (the freed nibbler)
    s32 mode = 0;                                     // orig: +0x8C0
    u8 b8C4 = 0, b8C5 = 0;                            // orig: +0x8C4 / +0x8C5
    f32 f8C8 = -1.0f;                                 // orig: +0x8C8
    // orig: +0x8CC, NOT written by the ctor 0x0008B1D0 (a heap leftover until the freed-nibbler branch 0x0008B954
    // stores a voice there; the port starts it at 0, KNOWN_ISSUES #47's convention).
    u32 voice8CC = 0;
};

// orig: 0x0008BE90 (0x8C0 bytes, vtbl 0x0029F598): a GenericNpc with its own idle variants and a Move that keeps the
// body still. docs/analysis/owl_weapon_port.md.
class Owl : public GenericNpc {
public:
    ~Owl() override { GenericNpc::UnloadModel(); }    // orig: 0x0008BED0 (0x000466A0, then 0x00047E60)
    Owl(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot)
        : GenericNpc(level, def, pos, rot) {}         // orig: 0x0008BE90 (only the vtables)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x0008BFC0
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x0008BF30
};

// orig: 0x0008C340 (0x8D0 bytes, vtbl 0x0029F830). docs/analysis/npc_classes_l12_l21_l31_port.md 4.1.
class SewerTentacle : public GenericNpc {
public:
    // orig: 0x0008C4F0 (vf68 0x0008C300's body, +0x8C8 = 0; 0x00047E60 runs 0x000466A0)
    ~SewerTentacle() override { StopVoiceIfExists(voice8CC, true); retractSound = 0; }
    SewerTentacle(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void Think(f32 dt) override;                      // orig: 0x0008CA90
    void UpdateAnimation(f32 dt) override;            // orig: 0x0008C580
    void UnloadModel() override;                      // orig: 0x0008C300
    void OnTextKey(f32 keyTime, const char* text) override;   // orig: vfE8 0x0008CB60 (object_gaps.md 16.2)
    u8 attacking = 0, holding = 0;                    // orig: +0x8C0 (a missed grab) / +0x8C1 (pulling)
    f32 cooldown = 0;                                 // orig: +0x8C4
    u32 retractSound = 0;                             // orig: +0x8C8 (Sound_Load handle)
    u32 voice8CC = 0;                                 // orig: +0x8CC
};

// orig: 0x00082FA0 (0x8D0 bytes, vtbl 0x0029E148). 4.2.
class EyeStalk : public GenericNpc {
public:
    ~EyeStalk() override { GenericNpc::UnloadModel(); }  // orig: 0x00083010 (0x000466A0, then 0x00047E60)
    EyeStalk(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: 0x000830B0
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: 0x00083240
    // (vf7C 0x00081B50 is `jmp 0x00047240`: GenericNpc::Damage, no override)
    f32 hideTimer = 1.5f;                             // orig: +0x8C0
    f32 showTimer = 0;                                // orig: +0x8C4
};

// orig: 0x00092F90 (0x8D0 bytes, vtbl 0x002A0BA0; ":2" labels are WeaselRoll 0x00093510). 4.3.
class Weasel : public GenericNpc {
public:
    ~Weasel() override { GenericNpc::UnloadModel(); } // orig: 0x00093030 (0x000466A0, then 0x00047E60)
    Weasel(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: 0x000931E0
    void BuildModel() override;                       // orig: 0x000933F0
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: 0x00093090
    void OnTextKey(f32 keyTime, const char* text) override;      // orig: 0x00093370
    void Vf80(Character* attacker) override;          // orig: 0x00093430
    void Kill(Character* attacker) override;          // orig: vf84 0x000934E0
    u8 dizzy = 0, flattened = 0;                      // orig: +0x8C0 / +0x8C1
};

// orig: 0x0008E330 (0x8C0 bytes, vtbl 0x0029FC50). 4.4.
class SkeletonRanged : public GenericNpc {
public:
    ~SkeletonRanged() override { GenericNpc::UnloadModel(); }  // orig: 0x0008E370 (0x000466A0, then 0x00047E60)
    using GenericNpc::GenericNpc;
    void UpdateAnimation(f32 dt) override;            // orig: 0x0008E510
    void PostThink() override;                        // orig: 0x0008E410
    void Damage(f32 amount, Character* attacker) override;   // orig: vf7C 0x0008E7A0
    void ApplyHit(GameObject* t, ni::NiPoint3& dir, u32 n) override;   // orig: vfD4 0x0008E5A0
};

// orig: 0x0008D8C0 (0x8D0 bytes, vtbl 0x0029FAE8). 4.5.
class SkeletonKamikaze : public GenericNpc {
public:
    // orig: 0x0008D9A0 (vf68 0x0008D940's body, then 0x00047E60)
    ~SkeletonKamikaze() override { SkeletonKamikaze::UnloadModel(); }
    SkeletonKamikaze(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void Think(f32 dt) override;                      // orig: 0x0008E090
    void UpdateAnimation(f32 dt) override;            // orig: 0x0008DB80
    void UnloadModel() override;                      // orig: 0x0008D940
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: 0x0008D920 (the Tank's)
    void PostThink() override;                        // orig: 0x0008DA30
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x0008DF40
    void ApplyHit(GameObject* t, ni::NiPoint3& dir, u32 n) override;   // orig: vfD4 0x0008DDE0
    u8 running = 0;                                   // orig: +0x8C0
    f32 fuse = 0, beep = 0;                           // orig: +0x8C4 / +0x8C8 (+0x8C8 not written by the ctor)
    u32 voice8CC = 0;                                 // orig: +0x8CC (NOT written by the ctor: KNOWN_ISSUES)
};

// orig: 0x00088230 (0x8D0 bytes, vtbl 0x0029ED98; ":2" labels are Junkbot2 0x000886F0). docs/analysis/level2-2_port.md 5.
class Junkbot : public GenericNpc {
public:
    ~Junkbot() override { GenericNpc::UnloadModel(); }  // orig: 0x00088270 (0x000466A0, then 0x00047E60)
    using GenericNpc::GenericNpc;                     // orig: 0x00088230 (the vtables, +0x8C0 = 0)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x000883B0
    void BuildModel() override;                       // orig: vf64 0x00088CC0
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x000882D0
    bool WalkRunPlaying() const override { return true; }        // orig: vf9C 0x000A32A0 (mov al, 1)
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x000885C0 (with the kill blast)
    u8 b8C0 = 0;                                      // orig: +0x8C0
};

// The classes of docs/analysis/npc_classes_batch2_port.md (sections S, T, W, J). "ToxicWarrior4" and "Junkbot2" are
// the port's names (L: from the label suffixes); "WeaselRoll" is the symbol table's.

// orig: 0x00090580 (0x8D0 bytes, vtbl 0x002A0290): "stormtrooper" (section S). "stormtrooper_mk2" (StormtrooperMk2
// below) derives from it.
class Stormtrooper : public GenericNpc {
public:
    ~Stormtrooper() override { GenericNpc::UnloadModel(); }  // orig: 0x00090410 (0x000466A0, then 0x00047E60)
    Stormtrooper(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void Think(f32 dt) override;                      // orig: vf54 0x000907C0 (no Move, no PostThink)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00090740
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x00090470 (no caller for this class)
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x00090600
    f32 phase = 0;                                    // orig: +0x8C0 (written by the ctor only; no reader found)
};

// orig: "stormtrooper_mk2" (new(0x8E0) 0x0008FEF0 = Stormtrooper 0x00090580 + vtbls 0x002A0150 / 0x002A013C /
// 0x002A0138, dtor 0x0008FE70; docs/analysis/object_gaps.md 21.4): the grenade thrower among the stormtroopers. Its
// vfFC / vfE8 / vf104 are goonsneak's 0x00086220 / 0x00086750 / 0x00086980 with the target at +0x8D0, no grenade
// clone to hide and the label "STMK2-GRANATE"; its vf5C 0x00090150 is GenericNpc's (with a model) without the
// stormtrooper's death animation. "StormtrooperMk2" is the port's name (L).
class StormtrooperMk2 : public Stormtrooper {
public:
    // orig: 0x0008FE70 (0x000466A0, then the Stormtrooper dtor 0x00090410)
    ~StormtrooperMk2() override { GenericNpc::UnloadModel(); }
    using Stormtrooper::Stormtrooper;
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00090150
    void OnTextKey(f32 keyTime, const char* text) override;    // orig: vfE8 0x00090010
    void StartAttack(u32 kind, const ni::NiPoint3& pos, const ni::NiMatrix3& rot) override;   // orig: vfFC 0x0008FF50
    // orig: vf104 0x00090170
    void Shoot(u32 kind, const ni::NiPoint3& pos, const ni::NiMatrix3& rot, bool silent, f32 t0) override;
    ni::NiPoint3 target;                              // orig: +0x8D0 (StartAttack: the player's vf78)
};

// orig: 0x00090FF0 (0x8D0 bytes, vtbl 0x002A04F0): "toxicwarrior" labels with ":4" (section T), with its own
// ranged attack: vfFC 0x00091060 keeps the player's point and the aim, vf104 0x000910F0 fires D+0x15C (vf20).
class ToxicWarrior4 : public GenericNpc {
public:
    ~ToxicWarrior4() override { GenericNpc::UnloadModel(); }  // orig: 0x00090EF0 (0x000466A0, then 0x00047E60)
    ToxicWarrior4(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00091180
    void StartAttack(u32 kind, const ni::NiPoint3& pos, const ni::NiMatrix3& rot) override;   // orig: vfFC 0x00091060
    // orig: vf104 0x000910F0
    void Shoot(u32 kind, const ni::NiPoint3& pos, const ni::NiMatrix3& rot, bool silent, f32 t0) override;
    ni::NiPoint3 target;                              // orig: +0x8C0 (StartAttack: the player's vf78; no reader)
};

// orig: 0x00093510 (0x8D0 bytes, vtbl 0x002A0D18): "Weasel" labels with ":2" (section W). BuildModel / OnTextKey are
// Weasel's (0x000933F0 / 0x00093370), UnloadModel SkeletonKamikaze's (0x0008D940).
class WeaselRoll : public GenericNpc {
public:
    WeaselRoll(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    ~WeaselRoll() override;                           // orig: 0x00093600 (the voice +0x8CC)
    void Vf80(Character* attacker) override;          // orig: 0x00093DC0
    void Kill(Character* attacker) override { Vf80(attacker); }   // orig: vf84 0x000935D0 (tail jump to vf80)
    void Think(f32 dt) override;                      // orig: vf54 0x00094130
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x000938B0
    void BuildModel() override;                       // orig: vf64 0x000933F0 (Weasel's)
    void UnloadModel() override;                      // orig: vf68 0x0008D940 (SkeletonKamikaze's)
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x000936C0
    bool CanBePushed() const override { return rolling ? false : GenericNpc::CanBePushed(); }   // orig: vfA4 0x000935B0
    void OnTextKey(f32 keyTime, const char* text) override;      // orig: vfE8 0x00093370 (Weasel's)
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x00093D40
    void ApplyHit(GameObject* t, ni::NiPoint3& dir, u32 n) override;   // orig: vfD4 0x00093E80
    void HitSpheres(u32 n) override;                  // orig: vfD8 0x00093D10
    u8 rollRequest = 0, rolling = 0, stopRoll = 0, dizzy = 0, flattened = 0;   // orig: +0x8C0 / +0x8C1 / +0x8C2 / +0x8C3 / +0x8C4
    f32 rollTimer = 0;                                // orig: +0x8C8
    u32 voice8CC = 0;                                 // orig: +0x8CC (NOT written by the ctor: KNOWN_ISSUES)
};

// orig: 0x000886F0 (0x8D0 bytes, vtbl 0x0029EED8): "junkbot" labels with ":2" (section J). BuildModel 0x00088CC0, the
// damage 0x000885C0 and OnTextKey are Junkbot's; vf9C stays GenericNpc's.
class Junkbot2 : public GenericNpc {
public:
    ~Junkbot2() override { GenericNpc::UnloadModel(); }  // orig: 0x00088770 (0x000466A0, then 0x00047E60)
    Junkbot2(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x000888E0
    void BuildModel() override;                       // orig: vf64 0x00088CC0 (Junkbot's)
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x000887D0
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x000885C0 (Junkbot's, with the kill blast)
    u8 deployed = 0;                                  // orig: +0x8C0
};

// The classes of docs/analysis/npc_classes_batch2_port.md part 2 (sections D and K). "SkeletonGuard" is the port's name
// (L: from the CY*_SWGUARD labels); "Destructor" and "HighPriest" are the symbol table's.

// orig: 0x00082220 (0x8E0 bytes, vtbl 0x0029DF70): "destructor" (section D), a boss with a second physics box at its
// "vortex" node. Its dtor 0x00082370 runs vf68 0x000466A0, deletes +0x8C0, then GenericNpc's 0x00047E60.
class Destructor : public GenericNpc {
public:
    // orig: 0x00082370 (0x000466A0; then +0x8C0 vf00(1): the member)
    ~Destructor() override { GenericNpc::UnloadModel(); }
    Destructor(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x000827C0
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x000827A0
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x00082590
    void OnTextKey(f32 keyTime, const char* text) override;      // orig: vfE8 0x00082AB0
    bool CanBePushed() const override { return false; }          // orig: vfA4 0x000AEDD0
    std::unique_ptr<phys::CharBody> vortex;           // orig: +0x8C0 (new(0x48) 0x000AF0D0)
    s32 rocket8C4 = 0;                                // orig: +0x8C4 (0x000823E0 tests == 4)
    u8 missilesShown = 0;                             // orig: +0x8C8 (set by 0x000829D0)
    f32 missileTimer = 0;                             // orig: +0x8CC
    ni::NiPoint3 fireTarget;                          // orig: +0x8D0 (0x000824F0; read by the firew keys)
    // orig: 0x000824F0 (thiscall(anim, rec), RET 8): native 140 FsFirePrimary.
    void FirePrimary(const char* anim, TriggerRec& rec);
    // orig: 0x000823E0 (thiscall(rec), RET 4): native 139 FsFireDestructorMissile: the next of the four rockets'
    // "fire_rocket_<n>" animations at `rec` (none after the fourth; not while a rocket animation plays unfinished).
    void FireMissile(TriggerRec& rec);
    // orig: vf114 0x000829D0: native 138 FsRearmDestructor: the four "missileN" nodes shown again at 0.01 (scaled up by
    // UpdateAnimation while +0x8CC runs down).
    void Rearm();
};

// orig: 0x000817A0 (0x8C0 bytes, vtbl 0x0029DB78, dtor 0x000817E0: vf68 0x000466A0, then GenericNpc's):
// "carnivorous_plant" (docs/analysis/object_gaps.md 16.4). Only vf5C and vfE8 differ from GenericNpc.
class CarnivorousPlant : public GenericNpc {
public:
    ~CarnivorousPlant() override { GenericNpc::UnloadModel(); }  // orig: 0x000817E0 (0x000466A0, then 0x00047E60)
    CarnivorousPlant(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot)
        : GenericNpc(level, def, pos, rot) {}         // orig: 0x000817A0 (only the vtables)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00081880
    void OnTextKey(f32 keyTime, const char* text) override;   // orig: vfE8 0x000818C0
};

// orig: 0x00087300 (0x8C0 bytes, vtbl 0x0029EAF0, dtor 0x00087280): "high_priest" (section K.5.2).
class HighPriest : public GenericNpc {
public:
    ~HighPriest() override { GenericNpc::UnloadModel(); }  // orig: 0x00087280 (0x000466A0, then 0x00047E60)
    HighPriest(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x000873A0
    void OnTextKey(f32 keyTime, const char* text) override;   // orig: vfE8 0x00087530 (the spell keys NOT PORTED)
};

// orig: 0x0008D250 (0x8C0 bytes, vtbl 0x0029F9B8): "skel_hth" labels with ":2" / ":3" (model variant 1 / 2; section
// K.5.1). Its dtor 0x0008D2E0 runs vf68 0x000466A0, then GenericNpc's.
class SkeletonGuard : public GenericNpc {
public:
    ~SkeletonGuard() override { GenericNpc::UnloadModel(); }  // orig: 0x0008D2E0 (0x000466A0, then 0x00047E60)
    SkeletonGuard(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void Think(f32 dt) override;                      // orig: vf54 0x0008D630 (no SetForce, no pinning)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x0008D420
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x0008D340
    void ApplyHit(GameObject* t, ni::NiPoint3& dir, u32 n) override;   // orig: vfD4 0x0008D4D0
};

class Tank : public GenericNpc {                      // orig: 0x00091F60 (0x8F0 bytes, vtbl 0x002A08C0)
public:
    // orig: 0x00092060 (0x000466A0; then +0x8D0 / +0x8CC released: the members)
    ~Tank() override { GenericNpc::UnloadModel(); }
    Tank(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: 0x00092100
    void BuildModel() override;                       // orig: 0x00092150
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: 0x0008D920
    void PostThink() override {}                      // orig: 0x000B0420
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x00091F20
    // orig: vfE8 0x000926C0: "morph:firew" fires from both tips in mode 1 (no base call), else GenericNpc's.
    void OnTextKey(f32 keyTime, const char* text) override;
    // orig: 0x00092350 (thiscall(mode, const NiPoint3* target), RET 8; native 10:3 FireWeapon): the fire mode and
    // the target; the turret's elevation blend of the aim animations 0xF5 / 0xF6 and the hold timer reset.
    void FireWeapon(s32 mode, const ni::NiPoint3& at);
    void AimAt();                                     // orig: 0x000924D0 (aimRot: columns dir, (dir x Y) x dir, dir x Y)
    void FireFrom(const ni::NiPoint3& p);             // orig: 0x00092600 (thiscall(const NiPoint3*), RET 4): one shot
    s32 u8C0 = 1;                                     // orig: +0x8C0 (the fire mode: 0..3, -1 invalid)
    ni::NiPoint3 target;                              // orig: +0x8D4 (FireWeapon's)
    f32 holdTimer = 0;                                // orig: +0x8C4
    u8 b8C8 = 0;                                      // orig: +0x8C8
    ni::NiPointer<ni::NiAVObject> tip1, tip2;         // orig: +0x8CC / +0x8D0
    Def* laser = nullptr;                             // orig: +0x8E0
    Def* nozzle = nullptr;                            // orig: +0x8E4
    Def* laserPart = nullptr;                         // orig: +0x8E8
    // (its AimController is Character::aim55C, +0x55C)
};

// The level1-5 goons (docs/analysis/object_gaps.md 3): "goonbox" (new(0x8C0) 0x00085EB0, vtbl 0x0029E470; label ":3" /
// ":2" / none: model variants 2 / 1 / 0), "goonbox:4" (0x00086C40, vtbl 0x0029E830, variant 3) and "goonskinny"
// (0x00085FB0, vtbl 0x0029E5B0, variant 0) are GenericNpcs whose vtables differ only in vf7C 0x000865B0 (the scrap
// blast at the death; vf5C / vfE8 are thunks to the base). "Goon" is the port's name (L).
class Goon : public GenericNpc {                      // orig: 0x00085EB0 / 0x00085FB0 / 0x00086C40 (ctors)
public:
    // orig: 0x00085EF0 / 0x00085FF0 (goonskinny) / 0x00086C80 (goonbox:4): 0x000466A0, then 0x00047E60
    ~Goon() override { GenericNpc::UnloadModel(); }
    using GenericNpc::GenericNpc;
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x000865B0
};

// orig: "toxicwarrior" with ":3" (new(0x8C0) 0x00090AE0, vtbl 0x002A03C0; object_gaps.md 6): 200 health; its Think
// always turns, moves at 28 when the stick is past 0.75 (D+0x74 below) and has no gravity set-up; its animation
// adds the death 0x4E and the b508 attack 0x3B. "ToxicWarrior3" is the port's name (L).
class ToxicWarrior3 : public GenericNpc {
public:
    ~ToxicWarrior3() override { GenericNpc::UnloadModel(); }  // orig: 0x00090B30 (0x000466A0, then 0x00047E60)
    ToxicWarrior3(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void Think(f32 dt) override;                      // orig: vf54 0x00090C60
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00090BD0
};

// orig: "rockmonster" (new(0x8C0) 0x0008C1A0, vtbl 0x0029F6D8; object_gaps.md 5): 200 health, scale 1.4, body +0x1C
// set; the dying animation 0x4D + rand() % D+0xE0, the physics bone "TM_Bones:Animation (Translational)" and the
// move 0x000912C0 (shared with "turdman") that holds the root-motion animations 0x4F / 0x50 in place.
class RockMonster : public GenericNpc {
public:
    ~RockMonster() override { GenericNpc::UnloadModel(); }  // orig: 0x0008C120 (0x000466A0, then 0x00047E60)
    RockMonster(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x0008C250
    void BuildModel() override;                       // orig: vf64 0x0008C2C0
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x000912C0
protected:
    struct SharedSlots {};                            // (port) the GenericNpc ctor only: turdman shares vf64 / vfF4
    RockMonster(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot, SharedSlots)
        : GenericNpc(level, def, pos, rot) {}
};

// orig: "turdman" without ":2" (new(0x8C0) 0x00091210, vtbls 0x002A0620 / 0x002A0608 / 0x002A0604, dtor 0x00091260;
// model variant 1; docs/analysis/object_gaps.md 21.5): 100 health; rockmonster's vf64 0x0008C2C0 and vfF4 0x000912C0,
// GenericNpc's OnTextKey (the thunk 0x00085F60); its vf5C 0x00091410 adds the death 0x50 and, every frame of every
// fifth second of the character time, an "effect_fart" behind it with the sound 0x60. "Turdman" is the port's name.
class Turdman : public RockMonster {
public:
    // orig: 0x00091260 (0x000466A0, then 0x00047E60; not RockMonster's)
    ~Turdman() override { GenericNpc::UnloadModel(); }
    Turdman(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00091410
};

// orig: "turdman" with ":2" (new(0x8D0) 0x000915A0, vtbls 0x002A0758 / 0x002A0744 / 0x002A0740, dtor 0x000917F0; model
// variant 0; docs/analysis/npc_scripted_port.md 2.1): the big turdman that grabs and shakes the player. Rockmonster's
// vf64 0x0008C2C0, scale 1.25 and the GenericNpc health. "TurdManBig" is the symbol table's name.
class TurdManBig : public RockMonster {
public:
    ~TurdManBig() override { TurdManBig::UnloadModel(); }  // orig: 0x000917F0 (vf68 0x00091790's body, then 0x00047E60)
    TurdManBig(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00091880
    void UnloadModel() override;                      // orig: vf68 0x00091790
    void OnTextKey(f32 keyTime, const char* text) override;   // orig: vfE8 0x00091AF0
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x00091640
    u32 shakeSound = 0;                               // orig: +0x8C0 (Sound_Load handle)
    u32 voice8C4 = 0;                                 // orig: +0x8C4 (the shake voice)
    u8 missed = 0;                                    // orig: +0x8C8 (a missed grab: the next 0x3E plays 0x68)
};

// orig: "warthog" (new(0x8D0) 0x00092730, vtbls 0x002A0A20 / 0x002A0A08 / 0x002A0A04, dtor 0x000927B0;
// docs/analysis/npc_scripted_port.md 2.2): the charging pig. Attack 1 starts a 20-second charge at 22 units / s that
// ends in its own death on contact with the player or when the time runs out (the blast, the blood, the parts).
// Its vfF4 is SkeletonKamikaze's 0x0008D920, its vfE8 GenericNpc's (the thunk 0x00085F60).
class Warthog : public GenericNpc {
public:
    ~Warthog() override { GenericNpc::UnloadModel(); }  // orig: 0x000927B0 (0x000466A0, then 0x00047E60)
    Warthog(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void Think(f32 dt) override;                      // orig: vf54 0x00092D00 (no SetForce, no pin, no PostThink)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00092850
    void BuildModel() override;                       // orig: vf64 0x00092CC0
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x00092AC0
    void ApplyHit(GameObject* t, ni::NiPoint3& dir, u32 n) override;   // orig: vfD4 0x00092960
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x0008D920 (SkeletonKamikaze's)
    u8 charging = 0;                                  // orig: +0x8C0
    f32 chargeTimer = 0;                              // orig: +0x8C4
};

// orig: "patrol_guard" (new(0x8C0) 0x0008F020, vtbls 0x0029FED8 / 0x0029FEC0 / 0x0029FEBC, dtor 0x0008F060; the
// symbol table's "SpinCreature"; docs/analysis/npc_scripted_port.md 2.3): the spinning level5-1 guard. Model variant 1
// is the small guard (":2", heightOffset / boundHeight 1.0); the big one (scale 0.8) puts its model's collision meshes
// into the stage BSP (flag60) and is hurt only by Bender's 0xD2 / 0xD3 attacks.
class PatrolGuard : public GenericNpc {
public:
    // orig: 0x0008F060 (Voice_Exists(+0x880), no null test, no clear; 0x000466A0)
    ~PatrolGuard() override { StopVoiceIfExists(voice880, false); GenericNpc::UnloadModel(); }
    using GenericNpc::GenericNpc;                     // orig: 0x0008F020 (only the vtables)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x0008F440
    void BuildModel() override;                       // orig: vf64 0x0008F150
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x0008F940
    void OnMeshContact(Character* other, f32 amount, bool flag) override;   // orig: vfB8 0x0008F0F0
    void OnTextKey(f32 keyTime, const char* text) override;      // orig: vfE8 0x0008F860
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x0008F1B0
};

// orig: "spinbot" (new(0x8D0) 0x0008E8B0, vtbls 0x0029FD80 / 0x0029FD68 / 0x0029FD64, dtor 0x0008E980;
// docs/analysis/npc_scripted_port.md 2.5): the level5-1 spinning robot. Its model's collision meshes go into the stage
// BSP (flag60, the chickenwalker's vf64 0x00081C00); Bender's 0xB8 attack flips it over for 3 seconds (the spikes and
// blades leave the BSP), his 0xD2 / 0xD3 attacks kill a flipped one.
class SpinBot : public GenericNpc {
public:
    ~SpinBot() override { SpinBot::UnloadModel(); }   // orig: 0x0008E980 (vf68 0x0008E900's body, then 0x00047E60)
    SpinBot(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot)
        : GenericNpc(level, def, pos, rot) {}         // orig: 0x0008E8B0 (the four fields 0)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x0008EA50
    // orig: vf64 0x00081C00 (the chickenwalker's): flag60 = 1, then GenericNpc's.
    void BuildModel() override {
        flag60 = 1;
        GenericNpc::BuildModel();
    }
    void UnloadModel() override;                      // orig: vf68 0x0008E900
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x0008EEC0
    void OnMeshContact(Character* other, f32 amount, bool flag) override;   // orig: vfB8 0x0008E930
    void OnTextKey(f32 keyTime, const char* text) override;      // orig: vfE8 0x0008EE60
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x0008EA00
    u8 flipRequest = 0;                               // orig: +0x8C1 (Damage: flip it over)
    u8 hitFlipped = 0;                                // orig: +0x8C2 (hit while flipped; Move clears it)
    f32 flipTimer = 0;                                // orig: +0x8C4 (the time left on its back)
    f32 f8C8 = 0;                                     // orig: +0x8C8 (= step while not flag28; no reader found, H)
};

// orig: "igner_boss" (new(0x8E0) 0x00087960, vtbls 0x0029EC60 / 0x0029EC4C / 0x0029EC48, dtor 0x000878D0;
// docs/analysis/npc_scripted_port.md 2.9): the level2-1 boss. No think (vf54 `ret 4`), no PostThink; only native 141
// FsFireCannon drives it: every second call (vf114) blends the 3 x 3 aim grid towards the mean of the last two
// directions and starts the shot, whose "morph:firew" key fires two "Igner_Laser"s from the muzzles beside the tip.
class IgnerBoss : public GenericNpc {
public:
    ~IgnerBoss() override { GenericNpc::UnloadModel(); }  // orig: 0x000878D0 (0x000466A0, then 0x00047E60)
    IgnerBoss(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot)
        : GenericNpc(level, def, pos, rot) {}         // orig: 0x00087960 (+0x8DC / +0x8D8 = 0)
    void Think(f32 dt) override { (void)dt; }         // orig: vf54 0x000AEDC0 (`ret 4`)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00087D40
    void OnTextKey(f32 keyTime, const char* text) override;      // orig: vfE8 0x00087DC0
    void PostThink() override {}                      // orig: vfF8 0x000B0420 (`ret`)
    virtual void AimAndFire(const ni::NiPoint3& dir); // orig: vf114 0x000879D0 (native 141's call; name L)
    // orig: vf118 0x00087930 (thiscall(u32, const NiMatrix3*), RET 8; no caller found, H): vf24 with the matrix.
    virtual void Vf118(u32 unused, const ni::NiMatrix3& r) {
        (void)unused;
        SetRotation(r);
    }
    // orig: +0x8C0 / +0x8CC, the last two directions of vf114. NOT initialised by the constructor (heap garbage);
    // vf114 writes [0] before [1] is read, and the fire key reads both only after a second vf114 (L: the key needs the
    // aim animation, which only vf114 starts), so 0 here.
    std::array<ni::NiPoint3, 2> ring{};
    u32 ringIndex = 0;                                // orig: +0x8D8
    f32 fireTimer = 0;                                // orig: +0x8DC
};

// orig: "statuehead" (new(0x8D0) 0x0008FA50, vtbls 0x002A0018 / 0x002A0000 / 0x0029FFFC, dtor 0x0008FA90;
// docs/analysis/npc_scripted_port.md 2.8): the level3-3 statue head that only turns and fires an "Enemy-Laser" from
// its "TIP_01" node at most once a second. It never animates (no AnimTick) and has no text keys (vfE8 `ret 8`).
// "StatueHead" is the port's name.
class StatueHead : public GenericNpc {
public:
    ~StatueHead() override { GenericNpc::UnloadModel(); }  // orig: 0x0008FA90 (0x000466A0, then 0x00047E60)
    StatueHead(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot)
        : GenericNpc(level, def, pos, rot) {}         // orig: 0x0008FA50 (+0x8C0 = 0)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x0008FCD0
    void OnTextKey(f32 keyTime, const char* text) override { (void)keyTime, (void)text; }   // orig: vfE8 0x00011280
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x0008FB80
    void StartAttack(u32 kind, const ni::NiPoint3& pos, const ni::NiMatrix3& rot) override;   // orig: vfFC 0x0008FB30
    f32 fireTimer = 0;                                // orig: +0x8C0
    // orig: +0x8C4, the turn sound's voice: never initialised (heap garbage). Move tests it with `!= 0 &&
    // Voice_Exists`; no voice ever exists (in the port and under emulation: Voice_Exists walks the empty voice
    // list), so the garbage cannot change the result: 0 here.
    u32 voice8C4 = 0;
};

// orig: "chickenwalker" (new(0x8D0) 0x00081B80, vtbls 0x0029DD00 / 0x0029DCEC / 0x0029DCE8, dtor 0x00081AF0;
// docs/analysis/npc_scripted_port.md 2.6): the enemy walker of level1-6 (not the cockpit object of ChickenWalker.h).
// vf5C / vf7C / vfE8 are thunks to GenericNpc's; its type 0xA comes from GenericNpc::Init. "ChickenWalkerNpc" is the
// port's name.
class ChickenWalkerNpc : public GenericNpc {
public:
    ~ChickenWalkerNpc() override { GenericNpc::UnloadModel(); }  // orig: 0x00081AF0 (0x000466A0, then 0x00047E60)
    ChickenWalkerNpc(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot)
        : GenericNpc(level, def, pos, rot) {          // orig: 0x00081B80
        body->noRay = 0;                              // (+0x5C)+0x1D
    }
    // orig: vf64 0x00081C00: flag60 = 1 (0x000350B0 adds no meshes for this name), then GenericNpc's.
    void BuildModel() override {
        flag60 = 1;
        GenericNpc::BuildModel();
    }
    bool CanBePushed() const override { return false; }   // orig: vfA4 0x000AEDD0 (xor al, al)
    u32 u8C0 = 0, u8C8 = 0;                           // orig: +0x8C0 / +0x8C8 (ctor 0; no reader found, H)
    u8 b8C4 = 0, b8C6 = 1;                            // orig: +0x8C4 (0) / +0x8C6 (1) (no reader found, H)
};

// orig: "crocodile" (new(0x8D0) 0x00081C10, vtbls 0x0029DE38 / 0x0029DE24 / 0x0029DE20, dtor 0x00081CD0;
// docs/analysis/npc_scripted_port.md 2.7): the level1-2 sewer crocodile, a floating platform that carries a player
// standing on it and leaves ripples. Its vf7C / vfE8 are GenericNpc's.
class Crocodile : public GenericNpc {
public:
    ~Crocodile() override { GenericNpc::UnloadModel(); }  // orig: 0x00081CD0 (0x000466A0, then 0x00047E60)
    Crocodile(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    void SetTurned(bool b) { turned = b; }            // orig: 0x00081D30 (RET 4)
    void Think(f32 dt) override;                      // orig: vf54 0x00082140
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00081EE0
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x00081D80
    f32 rippleTimer = 0;                              // orig: +0x8C0
    f32 stepTimer = -1.0f;                            // orig: +0x8C4
    u8 stepping = 0;                                  // orig: +0x8C8 ("step_on" plays)
    u8 turned = 0;                                    // orig: +0x8C9 (the spawn: label "CSewB_2")
};

// orig: "aquafish" (new(0x8C0) 0x0007FFA0, vtbls 0x0029D850 / 0x0029D838 / 0x0029D834, dtor 0x0007FFE0): a swimmer that
// picks walk or run by its speed, bites what it touches and blows up when killed. No stage of the shipped levels
// places one (GenericNPCDef::Spawn's name table only).
class AquaFish : public GenericNpc {
public:
    AquaFish(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot)
        : GenericNpc(level, def, pos, rot) {}         // orig: 0x0007FFA0 (only the vtables)
    ~AquaFish() override { GenericNpc::UnloadModel(); }   // orig: 0x0007FFE0 (0x000466A0, then 0x00047E60)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00080080 (RET 4)
    void Damage(f32 amount, Character* attacker) override;   // orig: vf7C 0x00080390 (RET 8)
    // orig: vfB8 0x00080350 (RET 0xC): a player (IsType(1)) touching it takes 50 and the fish its own +0x4FC.
    void OnMeshContact(Character* other, f32 amount, bool flag) override;
};

// orig: "gunturret" (new(0x8D0) 0x00086D20, vtbls 0x0029E960 / 0x0029E948 / 0x0029E944, dtor 0x00086D70): a turret
// with a front shield (FsShieldTurret 134: 0x00086E00) that blends its aim animations towards the player and fires
// from "Tip_01" / "Tip_02". No stage of the shipped levels places one (level1-6's script shields four by name).
class GunTurret : public GenericNpc {
public:
    GunTurret(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot)
        : GenericNpc(level, def, pos, rot) {}         // orig: 0x00086D20 (+0x8C0..+0x8C2 = 0, the vtables)
    ~GunTurret() override { GenericNpc::UnloadModel(); }   // orig: 0x00086D70 (0x000466A0, then 0x00047E60)
    // orig: 0x00086E00 (thiscall(b), RET 4): +0x8C0 = b, +0x8C1 = +0x8C2 = 0 (the turn-on / turn-off played again).
    void SetShield(u8 b) {
        shield = b;
        shieldOn = shieldOff = 0;
    }
    void Damage(f32 amount, Character* attacker) override;   // orig: vf7C 0x00086DE0 (RET 8)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00086E60 (RET 4)
    void OnTextKey(f32 keyTime, const char* text) override;   // orig: vfE8 0x00087150 (RET 8)
    u8 shield = 0;                                    // orig: +0x8C0
    u8 shieldOn = 0, shieldOff = 0;                   // orig: +0x8C1 / +0x8C2 (the turn-on / turn-off finished)
};

class LMSource;

// orig: "lavaman" (new(0x8E0) 0x00088D40, vtbls 0x0029F008 / 0x0029EFF0 / 0x0029EFEC, dtor 0x00088E10;
// docs/analysis/npc_scripted_port.md 2.4): the level3-3 lava man. It carries a flickering fire light and heats up and
// cools down in a cycle (the definition's Liquid on its mesh shows it); only a cool lava man takes damage, a hot one
// burns the player who punches it. Its vfE8 is GenericNpc's (the thunk 0x00085F60). "LavaMan" is the port's name (L).
class LavaMan : public GenericNpc {
public:
    LavaMan(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    ~LavaMan() override;                              // orig: 0x00088E10 (the light detached and released)
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x00089070
    void BuildModel() override;                       // orig: vf64 0x00089330
    void UnloadModel() override;                      // orig: vf68 0x00088D00
    void Damage(f32 amount, Character* attacker) override;      // orig: vf7C 0x000890F0
    void Move(ni::NiPoint3& vel, ni::NiMatrix3& rot) override;   // orig: vfF4 0x00088F20
    void PostThink() override;                        // orig: vfF8 0x000893E0
    u8 cool = 0;                                      // orig: +0x8C0 (damage applies)
    u8 phase = 0;                                     // orig: +0x8C1 (1: cooling down)
    f32 phaseTimer = 0;                               // orig: +0x8C4
    u32 voice8C8 = 0;                                 // orig: +0x8C8 (the burn loop)
    ni::NiPointer<LMSource> light;                    // orig: +0x8D0
};

// orig: "goonsneak" (new(0x8E0) 0x00086090, vtbl 0x0029E6E0; object_gaps.md 4): the grenade thrower. A clone of the
// effects object "Granate" sits in the "R_Hand" bone; StartAttack only aims at the player within 100 units, the
// throw animations 0xE3 / 0xE6 / 0xE2 run while +0x8C0 is set and "morph:throw_grenade" throws the definition's
// projectile D+0x158 on a lob towards the stored target (vf104).
class GoonSneak : public Goon {
public:
    GoonSneak(LevelData& level, GenericNPCDef& def, const ni::NiPoint3& pos, const ni::NiMatrix3& rot);
    ~GoonSneak() override;                            // orig: 0x000868B0
    void UpdateAnimation(f32 dt) override;            // orig: vf5C 0x000862E0
    void BuildModel() override;                       // orig: vf64 0x00086170
    void UnloadModel() override;                      // orig: vf68 0x000861C0
    void OnTextKey(f32 keyTime, const char* text) override;    // orig: vfE8 0x00086750
    void StartAttack(u32 kind, const ni::NiPoint3& pos, const ni::NiMatrix3& rot) override;   // orig: vfFC 0x00086220
    // orig: vf104 0x00086980
    void Shoot(u32 kind, const ni::NiPoint3& pos, const ni::NiMatrix3& rot, bool silent, f32 t0) override;
    u8 throwing = 0;                                  // orig: +0x8C0
    f32 throwTime = 0;                                // orig: +0x8C4 (not set by the ctor; written before it is read)
    ni::NiPoint3 target;                              // orig: +0x8C8 (StartAttack: the player's vf78)
    ni::NiPointer<ni::NiAVObject> grenade;            // orig: +0x8D4
};

}  // namespace fut::game

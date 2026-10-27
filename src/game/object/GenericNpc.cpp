// Generic NPCs; see GenericNpc.h and docs/analysis/npc_spawn_port.md. x87 conventions as in CharacterUpdate.cpp.
#include "game/object/GenericNpc.h"
#include "game/level/TriggerManager.h"

#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "core/memory/MemoryManager.h"
#include "engine/ni/NiKeyframeManager.h"
#include "game/GameRandom.h"
#include "game/GameGlobals.h"
#include "game/GameTime.h"
#include "game/defs/CharacterDef.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/defs/ExplosionDef.h"
#include "game/defs/IngameEvent.h"
#include "engine/ni/NiClone.h"
#include "engine/ni/NiCollision.h"
#include "game/object/Destructable.h"
#include "game/object/Emitter.h"
#include "game/object/Impact.h"
#include "game/object/Laser.h"
#include "game/object/Liquid.h"
#include "game/level/LightManager.h"
#include "game/object/NodeUpdate.h"
#include "game/object/SimpleNpc.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/sound/GameSound.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

const std::array<u32, 26> kAIDefaultProfile = {      // orig: 0x000325A0
    0x00000001, 0x3F800000, 0x420C0000, 0x42960000, 0x44992000, 0x45AFC800, 0x00000032, 0, 0x40A00000,
    1,          1,          0,          0,          0,          1,          1,          1, 0,
    0x40A00000, 0x41C80000, 0x41C80000, 0x441C4000, 0x40400000, 0x3F800000, 0x3F000000, 0x41200000};

std::array<u32, 26> CopyAIProfile(const std::array<u32, 26>& src) {   // orig: 0x00032630
    std::array<u32, 26> d = src;
    for (auto [dst, from] : {std::pair{4, 2}, {5, 3}, {20, 18}, {21, 19}}) {   // FLD; FLD ST0; FMUL ST1; FSTP
        const f32 v = std::bit_cast<f32>(d[static_cast<size_t>(from)]);
        d[static_cast<size_t>(dst)] = std::bit_cast<u32>(F(double(v) * v));
    }
    return d;
}

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

void AddVelocity(phys::CharBody& b, const NiPoint3& a) {   // orig: body vf50 0x000AF7D0
    NiPoint3& v = b.velocity;
    v = {F(double(v.x) + a.x), F(double(a.y) + v.y), F(double(a.z) + v.z)};
}

u32 g_npcSerial = 0;                                  // orig: DAT_002DD144 (process-wide, never reset)
const NiPoint3 kGravity{0.0f, -100.0f, 0.0f};         // orig: 0x002DD308 (static, guard 0x002DD314)

// orig: the engine's float globals written by the static initialisers 0x002268B0 / 0x002268D0 / 0x002268F0 (the
// image holds zeros there): PI = F(atan(1.0) * 4), HALF_PI = F(PI * 0.5), TWO_PI = F(PI + PI).
constexpr f32 kPi = std::bit_cast<f32>(0x40490FDBu);      // 0x002F9274
constexpr f32 kHalfPi = std::bit_cast<f32>(0x3FC90FDBu);  // 0x002F926C
constexpr f32 kTwoPi = std::bit_cast<f32>(0x40C90FDBu);   // 0x002F9270

double Rand11() { return double(crt::Rand()) * double(6.103702e-05f) - 1.0; }     // orig: 0x0002CE50 (FSUB 1.0)
f32 Pf(const std::array<u32, 26>& p, int i) { return std::bit_cast<f32>(p[static_cast<size_t>(i)]); }   // a float word
NiPoint3 Sub(const NiPoint3& a, const NiPoint3& b) {      // orig: 0x00011530 (this - arg; FSTP x, y, z)
    return {F(double(a.x) - b.x), F(double(a.y) - b.y), F(double(a.z) - b.z)};
}
double LengthSq(const NiPoint3& v) { return (double(v.x) * v.x + double(v.y) * v.y) + double(v.z) * v.z; }   // 0x00011680

}  // namespace

// ---- AI groups (docs/analysis/natives_batch_a_port.md 4; checked against 0x0002FFD0..0x000313E0) ---------------------
namespace {

bool Valid(const AiGroupMember& m) {                  // the members' recurring test (this order)
    return m.agent && m.agent->Accepts() && m.agent->character && !m.agent->character->IsDead();   // 0x0002D000 / vfA8
}

NiPoint3 PlayerPos(const LevelData& level) {          // orig: player ? player->vf34() : (0, 0, 0)
    return level.player ? level.player->GetWorldPosition() : NiPoint3{};
}

double Dist(const NiPoint3& a, const NiPoint3& pp) {  // x87: sqrt((dz*dz + dy*dy) + dx*dx), d = a - pp unrounded
    const double dx = double(a.x) - pp.x, dy = double(a.y) - pp.y, dz = double(a.z) - pp.z;
    return std::sqrt((dz * dz + dy * dy) + dx * dx);
}

}  // namespace

AiGroup::AiGroup(const char* n) {                     // orig: 0x000301B0
    if (n) {                                          // 0x0002FFD0: new(strlen + 1) copy (nothing for NULL)
        const u32 len = static_cast<u32>(std::strlen(n)) + 1;
        nameBlock = mem::OperatorNew(len);
        std::memcpy(nameBlock, n, len);
        name = n;
    }
}

AiGroup::~AiGroup() {                                 // orig: 0x00030220 (+ operator_delete from 0x00031800)
    if (nameBlock) mem::OperatorDelete(nameBlock);
    if (block) mem::OperatorDelete(block);
    // (the member records are not freed by the original's teardown: their blocks stay allocated. The port's level
    // teardown deletes every agent, which detaches itself, before the groups.)
}

void AiGroup::AddMember(AiAgent* a, u32 w2, u32 w3, u32 w4, u32 cap4) {   // orig: 0x00030270
    if (!a) return;
    a->group = this;                                  // +0x78
    AiGroupMember* rec = nullptr;
    for (auto& m : members)
        if (m->agent == a) {
            rec = m.get();
            break;
        }
    if (!rec) {
        void* blk = mem::OperatorNew(0x3C);           // orig: new(0x3C) + ctor 0x00030060 (head insert)
        members.push_front(std::make_unique<AiGroupMember>());
        rec = members.front().get();
        rec->block = blk;
        rec->group = this;
        rec->agent = a;                               // (the old agent's +0x7C clear: never for a new record)
        a->member = rec;                              // 0x0015E3D0
    }
    std::memcpy(&rec->w2, &w2, 4);                    // +0x0C..+0x18: raw dwords, also for an existing record
    std::memcpy(&rec->w3, &w3, 4);
    std::memcpy(&rec->w4, &w4, 4);
    std::memcpy(&rec->cap4, &cap4, 4);
}

void AiGroup::Detach(AiAgent& a) {                    // orig: 0x000304F0
    if (a.group != this) return;
    for (auto& m : members) {
        if (m->leader == &a) m->leader = nullptr;     // +0x2C
        if (m->agent == &a) m->agent = nullptr;       // +0x28
    }
    if (AiGroupMember* rec = a.member) {              // 0x000300B0: leader = agent = 0, unlinked from its own group
        rec->leader = nullptr;
        if (rec->agent) rec->agent->member = nullptr;
        rec->agent = nullptr;
        if (rec->block) mem::OperatorDelete(rec->block);   // operator_delete
        rec->group->members.remove_if([rec](const auto& m) { return m.get() == rec; });
    }
    a.member = nullptr;                               // +0x7C
    a.group = nullptr;                                // +0x78
}

void AiGroup::Update(LevelData& level) {              // orig: 0x00030E30
    if (!active || !level.player) return;
    timer = F(double(g_frameStep) + timer);           // FST +4
    if (timer > 4.0f) {                               // FCOMP [0x00293C1C]; TEST AH,0x41; JNE
        timer = 0;
        Evaluate(PlayerPos(level));
    }
    const NiPoint3 pp = PlayerPos(level);
    for (auto& mp : members) {
        AiGroupMember& m = *mp;
        AiAgent* A = m.agent;
        if (!A || !A->character) continue;            // +0x28 / +0x878
        if (!(A->Accepts() && A->character && !A->character->IsDead())) {
            m.role = 0;                               // (the debug point's +1 / +9 is for the debug lines only)
            m.leader = nullptr;
        }
        A->Engage(*level.player);                     // 0x0002CC80 (S+0x94)
        A->seq668.Reset();                            // 0x00032730
        A->guard = 1;                                 // +0x1A9
        A->seq668.loop = 1;                           // byte +0x668
        switch (m.role) {                             // jump table 0x000313AC
        case 1: {                                     // 0x0003107B
            const double dx = double(A->pos.x) - pp.x;
            const double dz = double(A->pos.z) - pp.z;
            const f32 dzf = F(dz);                    // FST
            if (dz * dzf + dx * dx > 2500.0f) break;  // [0x00295650]; TEST AH,0x41; JE skip (NaN goes on)
            // (the player-to-agent vector is not normalised: Open questions 3)
            A->seq668.AddGoto({F(dx * 50.0f + pp.x), pp.y, F(double(dzf) * 50.0f + pp.z)}, 1.0f);   // 0x000327B0
            break;
        }
        case 2: A->guard = 0; break;                  // 0x00031238
        case 3: {                                     // 0x0003126A
            const f32 dxf = F(double(A->pos.x) - pp.x);
            const double dz = double(A->pos.z) - pp.z;
            if (dz * dz + double(dxf) * dxf > std::bit_cast<f32>(A->profile[20])) {   // +0x124; TEST AH,0x41; JNE
                A->guard = 0;
                break;
            }
            const double R = std::bit_cast<f32>(A->profile[18]);   // +0x11C
            const double k = std::bit_cast<f32>(0x3F8CCCCDu);       // 1.1f (0x00295644)
            const f32 r0 = F(R * 0.0f), rz = F(dz * R);
            A->seq668.AddGoto({F(double(dxf) * R * k + pp.x), F(double(r0) * k + pp.y), F(double(F(double(rz) * k)) + pp.z)},
                              1.0f);
            break;
        }
        case 4: {                                     // 0x00031153
            AiAgent* T = m.leader;
            if (T && T->character && !T->character->IsDead() && T->Accepts()) {
                const NiPoint3 v = Sub(m.slot, T->pos);   // 0x00011530
                if (LengthSq(v) < 49.0f) {            // 0x00011680; [0x00295648]; TEST AH,5; JP
                    A->seq668.AddGoto(m.slot, 7.0f);
                } else {
                    A->seq668.AddGoto(m.slot, 4.0f);
                    A->seq668.AddLookTarget(0.0f);    // 0x00032890
                }
                A->seq668.loop = 0;
            } else {
                m.role = 0;
            }
            break;
        }
        default: break;                               // 0 (and > 4): only the debug lines
        }
        // NOT PORTED: the debug lines (DAT_002DB83C 0x0002C9F0 / 0x0002C9E0) while `debug` is set (never, H).
    }
}

void AiGroup::Evaluate(const NiPoint3& pp) {          // orig: 0x00030E77..0x00030F55
    for (auto& m : members) {
        if (!Valid(*m)) {
            m->role = 0;
            continue;
        }
        if (m->role == 1) m->role = 0;
        if (mode == 1 && m->role == 2) m->role = 1;
    }
    Fill3();                                          // 0x00030300
    while (fill3 < 1.0f) {                            // 0x00030CE0: TEST AH,5; JP (NaN ends)
        AiGroupMember* c = Pick3(pp);
        if (!c) break;
        c->role = 3;
        Fill4(nullptr), Fill2(), Fill3();
    }
    Fill2();                                          // 0x00030380
    while (fill2 < 1.0f) {                            // 0x00030D30
        AiGroupMember* c = Pick2(pp);
        if (!c) break;
        c->role = 2;
        Fill4(nullptr), Fill2(), Fill3();
    }
    Fill4(nullptr);                                   // 0x00030400(0)
    for (auto& mp : members) {                        // 0x00030D80
        AiGroupMember& m = *mp;
        if (!Valid(m) || m.cap4 <= 0.0f || !(m.fill4 < 1.0f)) continue;   // JNP (> or NaN go on); JP (< goes on)
        do {
            AiGroupMember* c = Pick4(&m, pp);
            if (!c) break;
            c->role = 4;
            c->leader = m.agent;
            Fill4(&m), Fill2(), Fill3();
        } while (m.fill4 < 1.0f);
    }
    for (auto& m : members)
        if (m->cap4 > 0.0f && Valid(*m)) Formation(*m, pp);   // TEST AH,0x41; JNE (> only)
}

void AiGroup::Fill3() {                               // orig: 0x00030300
    if (cap3 <= 0.0f) fill3 = 1.0f;                   // TEST AH,0x41; JP (< or ==; a dead store: see the division)
    f32 s = 0;
    for (auto& m : members)
        if (Valid(*m) && m->role == 3) s = F(double(s) + m->w3);
    fill3 = F(double(s) / cap3);                      // cap3 0: 0/0 = the x87 default NaN
}

void AiGroup::Fill2() {                               // orig: 0x00030380
    if (cap2 <= 0.0f) fill2 = 1.0f;
    f32 s = 0;
    for (auto& m : members)
        if (Valid(*m) && m->role == 2) s = F(double(s) + m->w2);
    fill2 = F(double(s) / cap2);
}

void AiGroup::Fill4(AiGroupMember* m) {               // orig: 0x00030400 (RET 4)
    if (!m) {
        for (auto& o : members)
            if (o->cap4 > 0.0f && Valid(*o)) Fill4(o.get());   // TEST AH,0x41; JNE
        return;
    }
    m->sum4 = 0;
    m->fill4 = 0;
    if (m->cap4 <= 0.0f) return;                      // TEST AH,0x41; JNP (NaN goes on)
    for (auto& o : members)
        if (Valid(*o) && o->role == 4 && o->leader == m->agent) m->sum4 = F(double(o->w4) + m->sum4);
    m->fill4 = F(double(m->sum4) / m->cap4);
}

// The Pick helpers' `s <= best` / `R <= 0` are the original's TEST AH,0x41; JNP skips (< or == skip; NaN goes on); the
// `w == 0` skips are TEST AH,0x44; JNP.
AiGroupMember* AiGroup::Pick2(const NiPoint3& pp) {   // orig: 0x00030980
    const f32 need = F((1.0 - fill2) * cap2);
    f32 best = 0;
    AiGroupMember* pick = nullptr;
    for (auto& mp : members) {
        AiGroupMember& m = *mp;
        if (!Valid(m) || m.w2 == 0.0f) continue;
        double s = need - std::fabs(double(m.w2) - need);   // FSUB; FABS; FSUBR
        if (s <= best) continue;
        switch (m.role) {                             // table 0x00030B10
        case 0: break;
        case 1: s *= 0.5f; break;                     // [0x00293AA0]
        case 3: s *= double(fill3) - 0.5f; break;
        case 4:
            if (m.leader && m.leader->member) s *= m.leader->member->fill4;
            break;
        default: s = 0.0; break;                      // 2 and > 4
        }
        if (s <= best) continue;
        const f32 R = std::bit_cast<f32>(m.agent->profile[18]);   // +0x11C
        if (R <= 0.0f) continue;
        const double d = Dist(m.agent->pos, pp);
        if (d > R) s *= double(R) / d;                // TEST AH,0x41; JNE (> only)
        if (s <= best) continue;
        best = F(s);
        pick = &m;
    }
    return pick;
}

AiGroupMember* AiGroup::Pick3(const NiPoint3& pp) {   // orig: 0x000307D0
    const f32 need = F((1.0 - fill3) * cap3);
    f32 best = 0;
    AiGroupMember* pick = nullptr;
    for (auto& mp : members) {
        AiGroupMember& m = *mp;
        if (!Valid(m) || m.w3 == 0.0f) continue;
        double s = need - std::fabs(double(m.w3) - need);
        if (s <= best) continue;
        switch (m.role) {                             // table 0x0003096C
        case 0: case 1: break;
        case 2: s *= fill2; break;
        case 4:
            if (m.leader && m.leader->member) s *= m.leader->member->fill4;
            break;
        default: s = 0.0; break;                      // 3 and > 4
        }
        if (s <= best) continue;
        const f32 R = std::bit_cast<f32>(m.agent->profile[19]);   // +0x120
        if (R <= 0.0f) continue;
        const double d = Dist(m.agent->pos, pp);
        if (d > R) s *= double(R) / d;
        const double h = double(R) * 0.5f;
        if (h > d) s *= d / h;                        // FCOM ST1; TEST AH,0x41; JNE (> only)
        if (s <= best) continue;
        best = F(s);
        pick = &m;
    }
    return pick;
}

AiGroupMember* AiGroup::Pick4(AiGroupMember* leader, const NiPoint3& pp) {   // orig: 0x00030B30 (RET 4)
    if (!leader) return nullptr;
    const f32 need = F((1.0 - leader->fill4) * leader->cap4);
    f32 best = 0;
    AiGroupMember* pick = nullptr;
    for (auto& mp : members) {
        AiGroupMember& m = *mp;
        if (!Valid(m) || m.w4 == 0.0f) continue;
        double s = need - std::fabs(double(m.w4) - need);
        if (s <= best) continue;
        switch (m.role) {                             // table 0x00030CC8
        case 0: case 1: break;
        case 2: s *= fill2; break;
        case 4:
            if (m.leader == leader->agent) s = 0.0;
            else if (m.leader && m.leader->member) s *= m.leader->member->fill4;
            break;
        default: s *= fill3; break;                   // 3 and > 4
        }
        if (s <= best) continue;
        const double d = Dist(m.agent->pos, pp);
        if (d > 5.0f) s = s / d * 5.0f;               // [0x00293C24]; TEST AH,0x41; JNE
        if (s <= best) continue;
        best = F(s);
        pick = &m;
    }
    return pick;
}

void AiGroup::Formation(AiGroupMember& r, const NiPoint3& pp) {   // orig: 0x00030540 (this = the leader's record)
    if (r.cap4 <= 0.0f || !r.group || !r.agent) return;   // TEST AH,0x41; JNP (> or NaN go on)
    auto follower = [&r](const AiGroupMember& o) { return o.role == 4 && o.leader == r.agent; };
    u32 n = 0;
    for (auto& o : r.group->members) n += follower(*o) ? 1 : 0;
    const NiPoint3 L = r.agent->pos;                  // (raw)
    NiPoint3 d{F(double(pp.x) - L.x), 0.0f, F(double(pp.z) - L.z)};
    const double len = ni::Unitize(d);                // 0x000116A0
    if (len < 8.0f || len == 8.0f) {                  // [0x00293C04]; TEST AH,0x41; JP (> and NaN go on)
        for (auto& o : r.group->members)
            if (follower(*o)) o->slot = L;
        return;
    }
    for (auto& o : r.group->members)
        if (follower(*o)) o->slot = {FLT_MAX, FLT_MAX, FLT_MAX};   // 0x7F7FFFFF
    // (0, 1, 0) x d, x87: cx unrounded
    const double a0 = double(d.y) * 0.0f;
    const double cx = double(d.z) - a0;
    const f32 cy = F(double(d.x) * 0.0f - double(d.z) * 0.0f);
    const f32 cz = F(a0 - d.x);
    const NiPoint3 step{F(cx * 8.0f), F(double(cy) * 8.0f), F(double(cz) * 8.0f)};   // [0x00293C04]
    const double dx8 = double(d.x) * 8.0f, dy8 = double(d.y) * 8.0f;
    const f32 dz8 = F(double(d.z) * 8.0f);            // (stored back over d.z)
    const double h = double(n) * 0.5f;                // FILD (+2^32 when negative); [0x00293AA0]
    const f32 syh = F(double(step.y) * h), szh = F(double(step.z) * h);
    NiPoint3 pos{F(double(F(dx8 - double(step.x) * h)) + L.x), F((dy8 - syh) + L.y), F((double(dz8) - szh) + L.z)};
    while (n--) {
        if (AiGroupMember* o = NearestFollower(r, pos)) o->slot = pos;
        pos = {F(double(pos.x) + step.x), F(double(pos.y) + step.y), F(double(pos.z) + step.z)};
    }
}

AiGroupMember* AiGroup::NearestFollower(const AiGroupMember& leader, const NiPoint3& p) {   // orig: 0x00030100
    f32 best = FLT_MAX;
    AiGroupMember* pick = nullptr;
    for (auto& mp : leader.group->members) {
        AiGroupMember& o = *mp;
        if (o.role != 4 || o.leader != leader.agent || std::bit_cast<u32>(o.slot.y) != 0x7F7FFFFFu || !o.agent) continue;
        const double dx = double(o.agent->pos.x) - p.x, dy = double(o.agent->pos.y) - p.y, dz = double(o.agent->pos.z) - p.z;
        const double s = (dz * dz + dy * dy) + dx * dx;
        if (s > best) continue;                       // FCOM; TEST AH,0x41; JE skip (ties and NaN take the later one)
        best = F(s);
        pick = &o;
    }
    return pick;
}

void AiNoise(LevelData& level, const NiPoint3& pos, f32 loud) {   // orig: 0x00031420
    if (!level.aiEnabled) return;                     // byte M+0
    // (M+4, head first, next +0x884; Hear does not change the list)
    for (AiAgent* a : level.aiAgents)
        if (a->Accepts()) a->Hear(pos, loud);
}

// ---- AimController ----------------------------------------------------------------------------------------------
AimController::AimController(Character& c) : character(&c) {   // orig: 0x00037520
    flags = 0x0C;                                     // (NiTimeController ctor 0x00140FD0)
    loKeyTime = FLT_MAX;
    hiKeyTime = -FLT_MAX;
    computeScaledFirst = false;                       // byte +0x34 = 0
}

void AimController::AddTarget(Character* c, ni::NiAVObject* nd) {   // orig: 0x0003CF90
    if (!nd) return;
    towards = c;                                      // +0x3C
    if (std::find(nodes.begin(), nodes.end(), nd) != nodes.end()) return;
    nodes.push_back(nd);                              // (SetSize grows by +0x54; 0x000A4A20)
}

void AimController::Update(f32 time) {               // orig: vf14 0x0003B210 (RET 4)
    (void)time;
    NiPoint3 d;
    // the default direction: (-x, 20 - y, -z) of the character's position ([0x00295AB4]; FCHS)
    auto fallback = [&] {
        const NiPoint3 p = character->GetWorldPosition();   // vf34
        d = {F(-double(p.x)), F(double(20.0f) - p.y), F(-double(p.z))};
    };
    if (!towards) {
        fallback();
    } else {
        const NiPoint3 p = character->GetWorldPosition();   // vf34 (the character first)
        const NiPoint3 q = towards->GetWorldPosition();
        const double dx = double(q.x) - p.x;              // FST (kept for the test)
        d = {F(dx), F(double(q.y) - p.y), F(double(q.z) - p.z)};
        if (dx == 0.0 && d.z == 0.0f) fallback();      // TEST AH,0x44; JP (each: != or NaN keeps d)
    }
    d.y = 0;                                          // (stored before the count test)
    if (nodes.empty()) return;                        // +0x4C
    (void)ni::Unitize(d);                             // 0x000116A0 (FSTP ST0)
    const ni::NiMatrix3 Ri = ni::InverseOrZero(character->rotation);   // vf38; 0x00103BB0
    auto row = [&](int r) { return F((double(d.y) * Ri.m[r][1] + double(d.z) * Ri.m[r][2]) + double(d.x) * Ri.m[r][0]); };
    NiPoint3 e{row(0), row(1), row(2)};               // the heading in the character's frame
    (void)ni::Unitize(e);
    const NiPoint3 U{0.0f, 1.0f, 0.0f};               // 0x002D77EC (dword copies)
    const f32 sx = F(double(U.z) * e.y - double(U.y) * e.z);   // s = e x U (the listing's products)
    const f32 sy = F(double(e.z) * U.x - double(U.z) * e.x);
    const f32 sz = F(double(U.y) * e.x - double(e.y) * U.x);
    ni::NiMatrix3 M;                                  // chunks (columns) e, U, s
    M.m[0][0] = e.x, M.m[1][0] = e.y, M.m[2][0] = e.z;
    M.m[0][1] = U.x, M.m[1][1] = U.y, M.m[2][1] = U.z;
    M.m[0][2] = sx, M.m[1][2] = sy, M.m[2][2] = sz;
    for (size_t i = 0; i < nodes.size(); ++i) {       // (the count re-read every pass)
        ni::NiAVObject* n = nodes[i];
        if (!n) continue;
        n->local.rotate = n->controller ? ni::Multiply(M, n->local.rotate) : M;   // 0x0013EC10; 0x00013B30; +0x30
    }
}

// ---- AiAgent ----------------------------------------------------------------------------------------------------
AiAgent::AiAgent(LevelData& lvl, Character& c) : level(lvl), profile(kAIDefaultProfile) {   // orig: 0x0002E810
    seq458.loop = 0;                                  // byte +0x458 = 0 after 0x00032A30
    character = &c;                                   // 0x0002CEB0
    if (c.cdef.IsType(2)) player = static_cast<Player*>(&c);
    else npc = static_cast<GenericNpc*>(&c);
    level.aiAgents.insert(level.aiAgents.begin(), this);   // head of (S+0x114)+4
    // (the +0x84 path finder's arrays new 0xC80 / 0x320 / 0x320 are the AiArray members; NOT PORTED: the global
    // matrix 0x002DBB50)
}

AiAgent::~AiAgent() {                                 // orig: the destructor part 0x0002E640
    std::erase(level.aiAgents, this);                 // (M+4 unlink; NOT PORTED: clearing M+0x10 / M+0x14 when this
                                                      //  agent is selected: GameNatives' selection)
    if (group) group->Detach(*this);                  // orig: 0x000304F0 when +0x78 is set
}

void AiAgent::Update(f32 dt) {                        // orig: 0x0002FDE0
    if (!enabled || !level.aiEnabled || !character || character->flag28) return;
    acc = F(double(dt) + acc);
    pos = character->GetWorldPosition();              // vf34
    pos.y = F(double(character->cdef.boundCenter) + pos.y + 1.0f);   // FLD D+0x9C; FADD; FADD 1.0; FSTP
    const ni::NiMatrix3 r = character->rotation;      // vf38
    axisX = ni::MulVector(r, {1, 0, 0});              // 0x00011770 with 0x002D77E0
    axisZ = ni::MulVector(r, {0, 0, 1});              // 0x002D77F8
    axisY = ni::MulVector(r, {0, 1, 0});              // 0x002D77EC
    if (f238 == 1.0f) pos23C = pos;                   // FCOMP [0x00295690]; TEST AH,0x44; JP
    if (npc) ClearIntents();
    if (Tick()) {
        timer200 = timer200 > 0.0f ? F(double(timer200) - acc) : 0.0f;   // TEST AH,0x41; JNE
        switch (state) {
        case 1: State1(); break;
        case 2: State2(); break;
        case 3: State3(); break;
        default: break;
        }
    }
    acc = 0;
}

void AiAgent::ClearIntents() {                        // orig: 0x0002CB10
    f208 = 5.0f;
    character->moveY = character->moveX = character->strafe = 0;   // +0x4DC / +0x4D8 / +0x4E0
    character->attack1 = character->attack2 = character->b508 = character->b509 = 0;   // +0x506..+0x509
    if (npc) npc->goalHeight = 0.0f;                  // +0x7C8
    if (Player* p = player) {                         // orig: 0x0002CB8E: the player's own input state (A+0x880)
        p->thinkVel = {0.0f, 0.0f, 0.0f};             // +0xB48 = NiPoint3::ZERO DAT_002F915C (3 dword copies)
        p->roll98 = 0;                                // +0xB98
        p->roll95 = p->roll96 = p->roll97 = 0;        // +0xB95 / +0xB96 / +0xB97
        p->b44 = 0.0f;                                // +0xB44 (a dword 0)
        p->b8B = 0;                                   // +0xB8B
        p->jumpRequest = 0;                           // +0xB89
        p->attack1 = p->attack2 = 0;                  // +0x506 / +0x507 (again, through +0x880)
        p->special50A = p->special50B = p->special50C = 0;   // +0x50A..+0x50C
        p->xValue = p->aValue = 0.0f;                 // +0xB34 / +0xB30 (dword 0s)
        p->actionBits = 0;                            // +0xBA0 (a word store)
    }
    if (b80 && event.kind != 0xF) b80 = 0;
}

bool AiAgent::Tick() {                                // orig: 0x0002ECB0 (reached path)
    if (player) {                                     // 0x0002ECCB
        level.aiBlind = player->hasControl == 0;      // byte DAT_002DBB40 = SETE(P+0x630 == 0)
        seq248.loop = 0;
        if (event.kind == 0 || event.kind == 0xE) event = seq248.Next();
        if (event.kind == 9) event = seq248.Current();
        if (event.kind != 0) {                        // orig: 0x0002ED38: a scripted command (level3-4's room intros)
            ClearIntents();                           // 0x0002CB10 (ecx kept)
            (void)Process();                          // 0x0002E9C0 (the result is dropped)
            level.aiBlind = 1;                        // byte DAT_002DBB40 = 1
        }
        return false;
    }
    if (!npc) return true;
    const GenericNPCDef& d = npc->npcDef();
    if (d.flags170[0x1C]) {                           // orig: 0x0002ED73 (byte D+0x18C): the patrol / alarm cycle
        ClearIntents();                               // 0x0002CB10
        const double t = double(f228) + acc;          // FLD +0x228; FADD +0x30; FST
        f228 = F(t);
        // FLD profile[24] (+0x134); FXCH; FCOMPP (the unrounded sum against it); TEST AH,1; JNE: patrolling on
        if (!(t < Pf(profile, 24)) && !std::isnan(t)) {
            f22C = F(double(f22C) + acc);             // FLD +0x22C; FADD +0x30; FSTP
            npc->b7C4 = 1;                            // the alarm
            // FCOMP +0x22C (profile[23], +0x130, against the rounded alarm time); TEST AH,0x41; JP: still alarmed
            if (!(Pf(profile, 23) > f22C) && !std::isnan(Pf(profile, 23)) && !std::isnan(f22C)) {
                npc->b7C4 = 0;
                f228 = 0;
                f22C = 0;
            }
        }
        return npc->b7C4 == 0;                        // SETE
    }
    if (npc->type == 0xE) {                           // orig: 0x0002EE0D (the Destructor)
        ClearIntents();                               // 0x0002CB10 (the second call of this step)
        AiSequence& s = state == 2 ? seq458 : state == 3 ? seq668 : seq248;
        if (event.kind == 0 || event.kind == 0xE) event = s.Next();   // 0x000329C0 (rep movsd 8 dwords)
        if (event.kind != 0 && event.kind != 0xE) {
            (void)Process();                          // 0x0002E9C0 (the result is dropped)
        } else if (state == 3) {                      // 0x0002EE92
            if (level.player) TurnToPlayer(*level.player);   // S+0x94
        }
        if (event.kind == 0 || event.kind == 0xE) event = s.Next();   // 0x0002EEEF
        return false;
    }
    if (npc->type == 0xC) {                           // orig: 0x0002EF1A (sungod; no ClearIntents here)
        if ((state == 2 || state == 3) && level.player) TurnToPlayer(*level.player);
        return false;
    }
    if (d.flags170[0x10]) {                           // orig: 0x0002EF8E: 0x00046740 (byte D+0x180): the high priest
        // more than 10 damage taken: a warp to a random "<label>_w<k>" (k != the last one), played out by its
        // morph:hit_floor key (FLD +0x230; FCOMP [0x00293C2C] 10.0; TEST AH,0x41; JNE: <=, == and NaN skip)
        if (f230 > 10.0f) {
            f230 = 0;                                 // (ESI = 0)
            npc->b508 = 1;
            if (!npc->b870) {
                const char* lb = npc->label ? npc->label->c_str() : "<noname>";   // +0x34 (null: 0x0029562C)
                const s32 n = static_cast<s32>(profile[7]);   // +0xF0
                s32 k = 0;
                if (n != 0) k = crt::Rand() % n;      // orig: rand 0x0002EFF6 (CDQ; IDIV)
                if (k == warpIndex) {                 // +0x234 (the last warp)
                    k = k == 0 ? 1 : k - 1;
                    if (k >= n) k = n - 1;            // (JL)
                    if (k < 0) k = 0;                 // (JGE)
                }
                char name[128];
                std::snprintf(name, sizeof name, "%s_w%d", lb, k);   // sprintf 0x00166FE4 ("%s_w%d" 0x00295624)
                const StageWaypoint* wp = nullptr;    // Session::FindWaypointAnyStage 0x000C2A40
                for (const auto& st : level.stages)
                    if ((wp = st->FindWaypoint(name))) break;
                if (wp) {
                    warpIndex = k;
                    npc->warpPos = wp->position;      // +0x830 (3 dwords)
                    const NiPoint3 dv = Sub(npc->warpPos, pos);   // 0x00011530 (+0x830 - A+0x00)
                    const NiPoint3 up{0.0f, 1.0f, 0.0f};          // 0x002D77EC
                    NiPoint3 x = ni::Cross(up, dv);   // 0x00011700
                    (void)ni::Unitize(x);             // 0x000116A0 (FSTP ST0)
                    const NiPoint3 y = ni::Cross(x, up);
                    for (int r = 0; r < 3; ++r) {     // +0x840 = y, +0x850 = up, +0x860 = -x (FCHS each)
                        npc->warpRot.m[r][0] = r == 0 ? y.x : r == 1 ? y.y : y.z;
                        npc->warpRot.m[r][1] = r == 0 ? up.x : r == 1 ? up.y : up.z;
                        npc->warpRot.m[r][2] = -(r == 0 ? x.x : r == 1 ? x.y : x.z);
                    }
                    npc->b870 = 1;
                    return false;
                }
                npc->b870 = 0;                        // 0x0002F175
            }
        }
        if (npc->b870) {                              // 0x0002F182: the warp still pending
            npc->b508 = 1;
            return false;
        }
        // 0x0002F1A2: every profile[8] (+0xF4) seconds the dead revivable NPCs it sees rise (at most 3), each with a
        // spell_summon at it, then one at itself (FLD +0x1AC; FCOMP 0; TEST AH,0x41; JP: <= 0, not NaN)
        if (summonTimer <= 0.0f) {
            summonTimer = Pf(profile, 8);             // +0x1AC = +0xF4 (a dword copy)
            s32 left = 3;
            for (AiAgent* a : level.aiAgents) {       // (S+0x114)+4, next +0x884
                GenericNpc* o = a->npc;               // +0x87C
                if (!o || !o->IsDead() || !o->npcDef().flags170[0x11]) continue;   // vfA8; 0x00046750 (D+0x181)
                const NiPoint3 p = o->GetWorldPosition();   // vf34
                const NiPoint3 q{p.x, F(double(p.y) + 1.0f), p.z};   // FLD +4; FADD [0x00293A98]; FSTP
                if (!CanSee(q)) continue;             // 0x0002D030 (this agent's)
                (void)o->Revive();                    // vf110
                npc->b7C0 = 1;                        // (+0x7C0: the cast)
                const NiPoint3 at = o->GetWorldPosition(), dir = o->GetWorldPosition();   // vf34 twice
                if (Def* s = level.defs ? level.defs->Find("spell_summon") : nullptr)   // 0x000C3F10 (0x00295614)
                    (void)s->SpawnEffect(level, at, dir);   // vf20(&at, &dir) (the original calls it unchecked)
                if (--left <= 0) break;
            }
            if (npc->b7C0) {                          // 0x0002F2BB: one at itself
                const NiPoint3 at = npc->GetWorldPosition(), dir = npc->GetWorldPosition();
                if (Def* s = level.defs ? level.defs->Find("spell_summon") : nullptr)
                    (void)s->SpawnEffect(level, at, dir);
                return false;
            }
        }
        summonTimer = F(double(summonTimer) - acc);   // 0x0002F30F: FLD +0x1AC; FSUB +0x30; FSTP
    }
    if (!profile[12]) return true;                    // 0x0002F320: +0x104 == 0 -> 0x0002F55E (AL = 1)
    // 0x0002F32C: the scripted walk of the main sequence (+0x248): place (7), face (2), walk (13) until near
    if (event.kind == 0 || event.kind == 0xE) event = seq248.Next();   // 0x000329C0 (rep movsd 8 dwords)
    if (event.kind == 7) {                            // the character placed at the point
        character->SetPosition(event.point);          // vf1C(+0x1EC)
        event.kind = 0;
    }
    if (event.kind == 2) {                            // 0x0002F387: the character turned to face the point
        NiPoint3 d = Sub(event.point, pos);           // 0x00011530 (point - A+0x00)
        (void)ni::Unitize(d);                         // 0x000116A0 (FSTP ST0)
        const NiPoint3 up = d.y > 0.9f ? NiPoint3{0.0f, 0.0f, 1.0f} : NiPoint3{0.0f, 1.0f, 0.0f};   // [0x00293C08];
        // (0x002D77F8 / 0x002D77EC; TEST AH,0x41; JNE). x = up x d is NOT unitized and y = x x d (the second
        // 0x00011700 takes d, not up: 0x0002F40F); the port had a Unitize(x) and x x up until 2026-10-18 (O:
        // level1-2_croc_ride, the crocodile's +0x570)
        const NiPoint3 x = ni::Cross(up, d);          // 0x00011700(&x, &d) on up
        const NiPoint3 y = ni::Cross(x, d);           // 0x00011700(&y, &d) on x
        ni::NiMatrix3 R;                              // the chunks d, -y, -x (FCHS each) = the columns
        for (int r = 0; r < 3; ++r) {
            R.m[r][0] = r == 0 ? d.x : r == 1 ? d.y : d.z;
            R.m[r][1] = -(r == 0 ? y.x : r == 1 ? y.y : y.z);
            R.m[r][2] = -(r == 0 ? x.x : r == 1 ? x.y : x.z);
        }
        character->SetRotation(R);                    // vf24
        event.kind = 0xD;
    }
    const bool walking = event.kind == 0xD;           // SETE
    const NiPoint3 p = character->GetWorldPosition(); // vf34
    character->moveY = walking ? 1.0f : 0.0f;         // C+0x4DC
    const NiPoint3 v{F(double(p.x) - event.point.x),
                     F((double(character->cdef.boundCenter) + p.y) - event.point.y),   // (C+0x10)+0x9C
                     F(double(p.z) - event.point.z)};
    if (LengthSq(v) <= 4.0f) event.kind = 0;          // 0x00011680; FCOMP [0x00293C1C]; TEST AH,0x41; JP (NaN kept)
    return false;
}

void AiAgent::TurnToPlayer(Player& pl) {              // orig: 0x0002EEA1..0x0002EEED (= 0x0002EF37..0x0002EF83)
    const NiPoint3 p = pl.GetWorldPosition();         // vf34
    NiPoint3 d;
    d.x = F(double(p.x) - pos.x);                     // FLD; FSUB [A+0]; FSTP
    const double dy = double(p.y) - pos.y;            // (stays on the x87 stack)
    d.z = F(double(p.z) - pos.z);                     // FSTP
    d.y = F(dy + pl.cdef.boundCenter);                // FADD [def+0x9C]; FSTP
    (void)ni::Unitize(d);                             // 0x000116A0 (FSTP ST0)
    (void)TurnToDir(d);                               // 0x0002D1B0 (FSTP ST0)
}

void AiAgent::Aim(const NiPoint3& from, ni::NiMatrix3& rot) {   // orig: 0x0002DF50 (RET 0xC)
    const s32 t = character->type;                    // (A+0x878)+0x774
    bool atPoint = false, atPlayer = false;           // CL / [esp+0xF]
    if (t == 0xE) {
        if (event.kind == 3) atPoint = true;          // A+0x1D8
        else atPlayer = true;
    }
    if (t == 0xC) atPlayer = true;
    // the matrix of a direction d (the same code twice, 0x0002DFE1.. / 0x0002E189..): the local matrix's pad lanes
    // zeroed, 12 dwords copied to *rot (rep movsd)
    auto build = [&](NiPoint3 d) {
        (void)ni::Unitize(d);                         // 0x000116A0 (FSTP ST0)
        const NiPoint3 up = d.y > std::bit_cast<f32>(0x3F666666u)   // FCOMP [0x00293C08] 0.9; TEST AH,0x41; JNE
                                ? NiPoint3{0.0f, 0.0f, 1.0f}        // 0x002D77F8
                                : NiPoint3{0.0f, 1.0f, 0.0f};       // 0x002D77EC (also NaN)
        NiPoint3 s = ni::Cross(up, d);                // (inline, FSTP each)
        (void)ni::Unitize(s);
        const NiPoint3 c = ni::Cross(s, d);           // (inline; z stored first)
        rot.m[0][0] = d.x, rot.m[1][0] = d.y, rot.m[2][0] = d.z;   // chunk 0
        rot.m[0][1] = c.x, rot.m[1][1] = c.y, rot.m[2][1] = c.z;   // chunk 1
        rot.m[0][2] = -s.x, rot.m[1][2] = -s.y, rot.m[2][2] = -s.z;   // chunk 2 (FCHS)
        static_cast<GenericNpc*>(character)->b7BE = 1;   // (A+0x878)+0x7BE
    };
    if (atPoint) {                                    // A+0x1EC (the event's point; the y difference + 5 unrounded)
        const NiPoint3& p = event.point;
        build({F(double(p.x) - from.x), F((double(p.y) - from.y) + 5.0f), F(double(p.z) - from.z)});   // [0x00293C24]
    }
    if (atPlayer && level.player) {                   // S+0x94
        const NiPoint3 p = level.player->GetWorldPosition();   // vf34
        build({F(double(p.x) - from.x), F((double(p.y) - from.y) + level.player->cdef.boundCenter),   // FADD [def+0x9C]
               F(double(p.z) - from.z)});
    }
}

f32 AiAgent::TurnTo(const NiPoint3& p) {              // orig: 0x0002D360 (ST0: TurnToDir's result)
    if (!profile[10]) return 0.0f;                    // +0xFC
    // FLD; FCOMP; TEST AH,0x44; JP per component (NaN recomputes)
    if (!(p.x == turnTarget.x && p.y == turnTarget.y && p.z == turnTarget.z && pos.x == turnFrom.x &&
          pos.y == turnFrom.y && pos.z == turnFrom.z)) {
        turnTarget = p;                               // +0x5C (dword copies)
        turnFrom = pos;                               // +0x68
        turnDir = {F(double(p.x) - pos.x), F(double(p.y) - pos.y), F(double(p.z) - pos.z)};   // +0x50 (via locals)
        const double q = (double(turnDir.x) * turnDir.x + double(turnDir.y) * turnDir.y) +
                         double(turnDir.z) * turnDir.z;   // FLD z, y, x: x*x + y*y, then + z*z
        if (q == 0.0) {                               // FCOM [0x00293AB8]; TEST AH,0x44; JP (NaN: the sqrt path)
            turnDir = {1.0f, 0.0f, 0.0f};
        } else {
            const double i = 1.0 / std::sqrt(q);      // FSQRT; FDIVR [0x00293A98]
            turnDir = {F(i * turnDir.x), F(i * turnDir.y), F(i * turnDir.z)};
        }
    }
    return TurnToDir(turnDir);                        // jmp 0x0002D1B0 (&A+0x50)
}


void AiAgent::State1() {                              // orig: 0x0002F570
    b1B4 = 1;
    if (Hunt()) return;
    if (event.kind == 0xE) event = seq248.Current();  // 0x000329A0
    else if (event.kind == 0) event = seq248.Next();  // 0x000329C0
    if (Check()) {                                    // 0x0002EC20: something heard
        event = kAiZeroEvent;
        event.kind = 0xE;
        return;
    }
    Process();                                        // tail call
}

void AiAgent::State2() {                              // orig: 0x0002F5F0
    b1B4 = 1;
    ClearIntents();
    smCount = 0;                                      // 0x00032A50
    if (event.kind == 0 || event.kind == 0xE) event = seq458.Next();
    if (event.kind == 0) {
        Character* t = target;
        seq458.Reset();
        if (!t) Calm();
        else Engage(*t);
        return;
    }
    if (profile[14]) {                                // orig: 0x0002F672 (A+0x10C)
        Character* t = target;                        // +0x1D4
        if (!t) t = Hunt();                           // 0x0002E2E0 (may Engage itself)
        if (!t) t = level.player;                     // S+0x94
        if (t) {
            const NiPoint3 q = t->GetWorldPosition(); // vf34
            const double dx = double(q.x) - pos.x;    // (all three kept on the x87 stack)
            const double dy = (double(q.y) - pos.y) + t->cdef.boundCenter;   // FADD [def+0x9C]
            const double dz = double(q.z) - pos.z;
            const double d2 = (dx * dx + dz * dz) + dy * dy;
            const double r = double(Pf(profile, 18)) * 5.0f;   // FLD [A+0x11C]; FMUL [0x00293C24]
            if (r > d2 || r == d2) {                  // FCOMPP; TEST AH,1; JNE (skips on < and NaN)
                Engage(*t);                           // 0x0002CC80
                return;
            }
        }
    }
    (void)Process();                                  // 0x0002E9C0
}

void AiAgent::State3() {                              // orig: 0x0002F720
    ClearIntents();
    b1B4 = 0;
    smCount = 0;
    if (!npc) return;                                 // orig: 0x0002F748 -> 0x0002FDCB (the epilogue)
    if (event.kind == 0 || event.kind == 0xE) event = seq668.Next();
    if (event.kind == 5) Process();
    bool keep = true;
    if (Character* t = target) {
        const f32 bc = t->cdef.boundCenter;          // FLD [def+0x9C]; FSTP local
        NiPoint3 tp = t->GetWorldPosition();          // vf34
        tp.y = F(double(bc) + tp.y);
        if (!CanSee(tp)) {
            keep = false;
            if (guard) target = nullptr;
            else (void)MoveTo(targetPos);             // (FSTP ST0)
        }
    }
    bool hunt = true;
    if (target) {
        if (target->IsDead()) target = nullptr;       // vfA8
        else hunt = !keep;
    }
    if (hunt && Hunt()) keep = true;
    if (!target || !keep) {                           // 0x0002FD93
        if (loseTimer > 0.0f) {                       // TEST AH,0x41; JNE
            loseTimer = F(double(loseTimer) - acc);
            Process();
            return;
        }
        Calm();
        return;
    }
    // ---- 0x0002F83D: the target is visible and alive (docs/analysis/combat_ai_port.md 6)
    Character* t = target;
    loseTimer = Pf(profile, 25);                      // +0x1B8 = +0x138
    if (guard && Process()) return;                   // 0x0002F853
    targetPos = t->GetWorldPosition();                // vf34 -> +0x1C8
    if (profile[11] && !guard) {                      // 0x0002F8A0: strafe around the target
        b1B4 = 1;
        const double w = double(strafeWait) - acc;    // FLD; FSUB; FST (compared unrounded)
        strafeWait = F(w);
        const f32 iv = Pf(profile, 23);
        const double lo = double(iv) * 0.2f, hi = double(iv) * 0.8f;   // [0x00293AB4] / [0x00293C18]
        if ((w < 0.0 || w == 0.0) && (strafeTime < 0.0f || strafeTime == 0.0f) &&   // TEST AH,0x41; JP (x3)
            (lo < timer200 || lo == timer200) && hi >= timer200) {   // TEST AH,1; JNE
            if (RandUnit() < 0.75f) {                 // rand 0x0002F918; [0x00293A94]; TEST AH,5; JP
                strafeTime = F((RandUnit() + 1.0) * 1.5f);           // rand 0x0002F92E
                const double r = Rand11() * kHalfPi;                  // rand 0x0002F945
                // (L) the FCOM's condition bits are read after FLD / FMUL (combat_ai_port.md 14.3)
                const f32 ang = F(r < 0.0 ? r - double(kHalfPi) * 0.5f : double(kHalfPi) * 0.5f + r);
                strafePos = Sub(pos, targetPos);                      // 0x00011530
                (void)ni::Unitize(strafePos);
                const double c = std::cos(double(ang)), sn = std::sin(double(ang));   // FCOS / FSIN (extended)
                const double nx = c * strafePos.x - sn * strafePos.z;
                strafePos.x = F(nx);
                strafePos.z = F(c * strafePos.z + nx * sn);
                const f32 k = F((RandUnit() + 1.0) * Pf(profile, 19));   // rand 0x0002F9D7
                strafePos = {F(double(k) * strafePos.x), F(double(k) * strafePos.y), F(double(k) * strafePos.z)};
                strafePos = {F(double(targetPos.x) + strafePos.x), F(double(targetPos.y) + strafePos.y),
                             F(double(targetPos.z) + strafePos.z)};   // 0x000115F0 (arg + this)
            }
            strafeWait = F(RandUnit() * 2.5f + 3.0f); // rand 0x0002F9FD
        }
        if (strafeTime > 0.0f) {                      // TEST AH,0x41; JNE
            strafeTime = F(double(strafeTime) - acc);
            const f32 r = MoveTo(strafePos);
            if (r < 1.0f || r == 1.0f) strafeTime = 0.0f;   // TEST AH,0x41; JP
            strafeWait = F(RandUnit() * 2.5f + 3.0f); // rand 0x0002FA57
            return;
        }
    }
    if (t->def->IsType(2) && level.aiBlind) return;   // 0x0002CE70 (AsNpc) / DAT_002DBB40
    NiPoint3 v = Sub(targetPos, pos);                 // 0x00011530
    v.y = F(double(v.y) + t->cdef.boundCenter);
    const f32 d2 = F(LengthSq(v));                    // 0x00011680; FSTP
    bool ranged = true;
    f32 r2 = Pf(profile, 21), prec = Pf(profile, 19);
    if (!profile[15]) {
        if (!profile[14]) return;
        ranged = false;
    } else if (profile[14] && d2 < Pf(profile, 20)) { // TEST AH,5; JP
        ranged = false;
    }
    bool junk = false;
    if (d2 < r2 && static_cast<const Def*>(&character->cdef) == level.junkbotDef &&   // DAT_002DBB44
        !(character->label && std::strstr(character->label->c_str(), ":2"))) {   // (a NULL label crashes the original)
        junk = true;
        if (timer200 > 0.0f) return;                  // TEST AH,0x41; JE
    }
    if (!ranged) {
        r2 = Pf(profile, 20);
        prec = Pf(profile, 18);
    }
    bool aligned = false;
    if (d2 > r2 || f1B0 > 0.0f) {                     // TEST AH,0x41; JE (both)
        if (guard) return;
        const f32 keep208 = f208;
        f208 = prec;
        (void)MoveTo(targetPos);                      // (FSTP ST0)
        f208 = keep208;
        if (!junk) return;
        aligned = true;
    }
    // ---- 0x0002FBE4: the attack decision
    f238 = 1.0f;                                      // [0x00295690]
    if (d2 < 0.0f || d2 == 0.0f) return;              // TEST AH,0x41; JNP
    (void)ni::Unitize(v);
    if (!aligned) {
        const f32 r = TurnToDir(v);
        if (!(r < 0.01f || r == 0.01f)) return;       // [0x00293AAC]; TEST AH,0x41; JP
    }
    if (!(timer200 < 0.0f || timer200 == 0.0f)) return;   // TEST AH,0x41; JP
    if (attackTime < Pf(profile, 24)) {               // TEST AH,5; JP
        if (ranged) {
            jitter.x = F(Rand11() * 0.1f);            // rand 0x0002FC6D
            jitter.y = F(Rand11() * 0.05f);           // rand 0x0002FC7A
            jitter.z = F(Rand11() * 0.1f);            // rand 0x0002FC8B
            v = {F(double(jitter.x) + v.x), F(double(jitter.y) + v.y), F(double(jitter.z) + v.z)};   // 0x000115F0
            (void)ni::Unitize(v);
            npc->Fire(0, v);                          // vf100 0x00049260 (0x0002FCC0)
        } else if (npc && npc->npcDef().flags170[0x10]) {   // 0x00046740 (byte D+0x180)
            npc->b509 = 1;
        } else {
            npc->b508 = 1;                            // the close-attack request (skel_hth)
        }
    } else if (character->IsIdling() || character->WalkRunPlaying() || !character->curSeq) {   // vfA0 / vf9C / +0xA4
        attackTime = 0.0f;
        timer200 = Pf(profile, 23);
    }
    attackTime = F(double(acc) + attackTime);         // FLD +0x30; FADD +0x1FC
}

void AiAgent::Engage(Character& t) {                  // orig: 0x0002CC80
    state = 3;
    target = &t;
    targetPos = t.GetWorldPosition();
    event = kAiZeroEvent;
    event.kind = 0xE;
    attackTime = 0;                                   // +0x1FC
    loseTimer = std::bit_cast<f32>(profile[25]);      // +0x1B8 = +0x138
    f228 = f22C = 0;
    if (npc) npc->b7C4 = 0;
    ClearIntents();
    seq668.cur = -1;                                  // 0x00032990
}

void AiAgent::OnDamage(f32 amount, Character* attacker) {   // orig: 0x0002DEC0 (npc_combat2_port.md 3.1)
    f230 = F(double(amount) + f230);                  // FLD arg; FADD [A+0x230]; FSTP
    if (!attacker || attacker == character || player || !profile[0]) return;   // +0x878 / +0x880 / +0xD4
    if (state != 2 && state != 3) {
        for (Player* p : level.players)               // 0x000C2730
            if (p == attacker) {
                AlertEngage(*p);                      // 0x0002D980
                return;
            }
    }
    if (profile[17]) Engage(*attacker);               // +0x118; 0x0002CC80
}

void AiAgent::AlertEngage(Character& p) {             // orig: 0x0002D980
    state = 2;
    target = &p;
    targetPos = p.GetWorldPosition();
    event = kAiZeroEvent;
    event.kind = 0xE;
    f228 = f22C = 0;
    ClearIntents();
    seq458.cur = -1;
    if (npc) {
        if (npc->SeqById(0x68)) npc->b7BD = 1;        // 0x00033E00
        npc->cdef.ingameEvents.Trigger(9, {&p, nullptr}, npc->IngameContext());   // D+0x28 0x000F28F0
        npc->b7C4 = 0;
    }
}

void AiAgent::Calm() {                                // orig: 0x0002CD30
    state = 1;
    target = nullptr;
    event = kAiZeroEvent;
    event.kind = 0xE;
    ClearIntents();
}

f32 AiAgent::MoveTo(const NiPoint3& goal) {           // orig: 0x0002E700
    if (!profile[9] || !profile[10]) return 0.0f;     // +0xF8 / +0xFC
    if (!finder.net) return Steer(goal);              // A+0xD0 (tail call)
    if (!(goal.x == lastGoal.x && goal.y == lastGoal.y && goal.z == lastGoal.z)) {   // 0x0002DE70 (TEST AH,0x44)
        f1B0 = 0.0f;
        Repath(goal);
    }
    if (f1B0 >= 2.0f) {                               // [0x00293B20]; TEST AH,1; JNE skips (< / NaN)
        f1B0 = 0.0f;
        Repath(goal);
    }
    if (!pathRec && !AdvancePath()) return FLT_MAX;   // (the search is still running) [0x00295604]
    const bool mid = pathRec && pathRec->parent;      // +0xC: the next record
    const f32 prec = mid ? f20C : f208;
    const f32 r = Steer(mid ? pathRec->node->pos : goalProbe);   // node +0x44 / +0x890
    if (r < prec && !NextPathNode()) return 0.0f;     // FCOMP; TEST AH,5; JP; 0x0002CD70
    return FLT_MAX;
}

bool AiAgent::NextPathNode() {                        // orig: 0x0002CD70
    if (!pathRec || !pathRec->parent) return false;
    pathRec = pathRec->parent;
    return true;
}

bool AiAgent::Repath(const NiPoint3& goal) {          // orig: 0x0002E560
    if (!finder.net) return false;
    AiNetNode* s = finder.net->Nearest(pos);          // 0x00031D80
    AiNetNode* g = finder.net->Nearest(goal);
    lastGoal = goal;                                  // +0x89C
    goalProbe = ProbeGoal(lastGoal, g);               // +0x890 = 0x0002DAA0
    if (!s || !g) return false;
    if (s != finder.start || g != finder.goal) {      // P+8 / P+4
        pathRec = nullptr;
        (void)finder.Search(s, g);                    // 0x0002E450
    }
    return true;
}

NiPoint3 AiAgent::ProbeGoal(const NiPoint3& goal, AiNetNode* node) {   // orig: 0x0002DAA0
    if (!finder.net) return goal;
    if (!node && !(node = finder.net->Nearest(goal))) return goal;
    const NiPoint3 c = node->pos;                     // +0x44
    const f32 dz = F(double(goal.z) - c.z);
    NiPoint3 o{F(double(goal.x) - c.x), 0.0f, dz};
    const double q = double(o.x) * o.x + double(dz) * dz;
    const f32 l0c = F(q);                             // FST
    if (q > 100.0) {                                  // [0x00295610]; TEST AH,0x41; JNE (clamp the offset to 10)
        const double inv = 1.0 / std::sqrt(double(l0c));
        o = {F((double(o.x) * 10.0f) * inv), F(0.0 * inv), F((double(dz) * 10.0f) * inv)};
    }
    f32 t = 1.0f;
    (void)ni::RayCast({c.x, F(double(c.y) + 5.0f), c.z}, o, t, true, 3);   // 0x000FB780
    if (t < 1.0f) return c;                           // TEST AH,5; JNP: something between the node and the goal
    for (int k = 0; k < 3; ++k) {
        const NiPoint3 st{F(double(o.x) + c.x), F((double(o.y) + c.y) + 5.0f), F(double(o.z) + c.z)};
        f32 u = 1.0f;
        const ni::RayHit h = ni::RayCast(st, {0.0f, -8.0f, 0.0f}, u, true, 3);
        const u32 fl = h ? h.Flags() : 0;            // 0x000FA320 (flags byte 0)
        if (u < 1.0f && h && !(fl & 4) && !(fl & 8))      // TEST AH,5; JP
            return {F(double(o.x) + c.x), F(double(o.y) + c.y), F(double(o.z) + c.z)};
        o = {F(double(o.x) * 0.5f), F(double(o.y) * 0.5f), F(double(o.z) * 0.5f)};
    }
    return c;
}

bool AiAgent::AdvancePath() {                         // orig: 0x0002E3C0
    u8 exhausted = 0;
    pathRec = finder.Step(exhausted, 10);             // 0x0002DD10
    if (exhausted) return false;
    if (pathRec && pathRec->parent) {
        const NiPoint3& a0 = pathRec->node->pos;
        const NiPoint3& b0 = pathRec->parent->node->pos;
        const double ax = double(a0.x) - pos.x, ay = double(a0.y) - pos.y, az = double(a0.z) - pos.z;
        const double bx = double(b0.x) - pos.x, by = double(b0.y) - pos.y, bz = double(b0.z) - pos.z;
        if ((bz * az + bx * ax) + by * ay < 0.0) pathRec = pathRec->parent;   // TEST AH,5; JP: between the two
    }
    return true;
}

f32 AiAgent::TurnToDir(const NiPoint3& d) {           // orig: 0x0002D1B0 (the result is ST0)
    if (player || profile[13] || !profile[10]) return 0.0f;
    f32 sgn = 1.0f;                                   // (a float over the argument slot)
    const double t = 1.0 - ((double(axisX.z) * d.z + double(axisX.y) * d.y) + double(axisX.x) * d.x);
    const double side = (double(axisZ.z) * d.z + double(axisZ.y) * d.y) + double(axisZ.x) * d.x;
    if (!(side > 0.0)) sgn = -1.0f;                   // FCOMP 0; TEST AH,0x41; JE (NaN: -1)
    f32& mx = character->moveX;                       // ch+0x4D8
    if (!(t > double(0.001f))) {                      // [0x00293AC4]; TEST AH,0x41; JNE
        mx = 0.0f;
        return 0.0f;
    }
    const f32 inv = F(1.0 / double(character->cdef.rotSpeed));   // FLD 1.0; FDIV [D+0x88]; FSTP local
    // FPATAN(d.x, d.z) - FPATAN(X.x, X.z) (KNOWN_ISSUES #35: the C library's atan2 in double)
    const double a = std::atan2(double(d.x), double(d.z)) - std::atan2(double(axisX.x), double(axisX.z));
    mx = F(std::fabs(a));
    const f32 r = mx;                                 // FLD [+0x4D8]: left on the x87 stack, the result
    while (mx > kTwoPi) mx = F(double(mx) - kTwoPi);  // TEST AH,0x41; JNE / JE
    if (mx > kPi) mx = F(double(kTwoPi) - mx);
    mx = F(((double(mx) / acc) * inv) * sgn);         // FDIV [A+0x30]; FMUL inv; FMUL sgn
    if (mx > 1.0f) mx = 1.0f;                         // TEST AH,0x41; JNE
    else if (mx < -1.0f) mx = -1.0f;                  // [0x00293BF0]; TEST AH,5; JP
    return r;
}

f32 AiAgent::Steer(const NiPoint3& goal) {            // orig: 0x0002D4D0
    if (!profile[9] || !profile[10]) return 0.0f;
    const double gx = double(goal.x) - pos.x;
    const double gy = double(goal.y) - pos.y;
    const f32 l30 = F(double(goal.z) - pos.z);
    if (npc) npc->goalHeight = F(gy);                 // +0x7C8 (else popped)
    const double d2 = double(l30) * l30 + gx * gx;
    if (d2 < 1.0) {                                   // FCOM [0x00293A98]; TEST AH,5; JP (NaN goes on)
        f1B0 = 0.0f;
        return 0.0f;
    }
    const f32 l18 = F(std::sqrt(d2));
    const double inv = 1.0 / double(l18);
    NiPoint3 dir{F(gx * inv), F(0.0 * inv), F(double(l30) * inv)};   // L1C / L20 / L24
    if (npc) {                                        // (a player's agent skips the loop and the decrement)
        bool avoid = false;
        for (AiAgent* o : level.aiAgents) {           // M+4, head first
            if (o == this) continue;
            if (!b1B4 && (!o->character || o->player)) continue;   // players only in states 1 / 2
            Character* c = o->character;
            if (c == target || !c || c->IsDead()) continue;   // vfA8 (a null character crashes the original)
            NiPoint3 v = Sub(o->pos, pos);            // 0x00011530 (this = o)
            v.y = 0.0f;
            const double q = LengthSq(v);             // 0x00011680
            const f32 l14 = F(q);                     // FST
            if (q > 64.0) continue;                   // [0x0029560C]; TEST AH,0x41; JE
            if (l14 < 0.0f || l14 == 0.0f) continue;  // TEST AH,0x41; JNP
            const double r = 1.0 / std::sqrt(double(l14));
            const double dot =
                ((double(dir.z) * (double(v.z) * r)) + ((r * 0.0) * dir.y)) + double(dir.x) * (double(v.x) * r);
            if (dot < double(0.9f) || dot == double(0.9f)) continue;   // [0x00293C08]; TEST AH,0x41; JP
            const f32 sc = F(((double(dir.z) * axisX.z + double(dir.y) * axisX.y) + double(dir.x) * axisX.x) + 0.25f);
            const NiPoint3 w{F(double(sc) * axisZ.x), F(double(sc) * axisZ.y), F(double(sc) * axisZ.z)};   // 0x00011560
            const f32 wz2 = F(2.0 * w.z);             // FADD ST0,ST0; FSTP local
            dir = {F(2.0 * w.x + dir.x), F(2.0 * w.y + dir.y), F(double(wz2) + dir.z)};
            avoid = true;
            f1B0 = 1.0f;
            break;                                    // (only the first agent ahead counts)
        }
        if (!avoid) f1B0 = F(double(f1B0) - acc);
        if (f1B0 > 1.0f) f1B0 = 1.0f;                 // TEST AH,0x41; JNE
        else if (f1B0 < 0.0f) f1B0 = 0.0f;            // TEST AH,5; JP
        if (avoid && (l18 < 8.0f || l18 == 8.0f)) {   // [0x00293C04]; TEST AH,0x41; JP
            f1B0 = F(double(f1B0) - acc);
            return 0.0f;
        }
    }
    f32& my = character->moveY;                       // ch+0x4DC
    if (!npc) {
        // orig: 0x0002D85D (both f1B0 branches): a player agent's Goto: the direction in the scene camera's frame
        // ((S+0x8C)+0x30 +0x30: its local rotation, 12 dwords copied), forward = R X, side = R Z (0x00011770 with
        // 0x002D77E0 / 0x002D77F8); the stick words are the dot products (z, y, x order); walking (A+0x1AA == 0) halves
        // them ([0x00293AA0] 0.5f)
        ni::NiMatrix3 r{};
        if (level.camera && level.camera->sceneCamera) r = level.camera->sceneCamera->local.rotate;
        const NiPoint3 fw = ni::MulVector(r, {1, 0, 0});
        const NiPoint3 sd = ni::MulVector(r, {0, 0, 1});
        my = F((double(fw.z) * dir.z + double(fw.y) * dir.y) + double(fw.x) * dir.x);
        character->moveX = F((double(sd.z) * dir.z + double(sd.y) * dir.y) + double(sd.x) * dir.x);
        if (!playerRun) {
            my = F(double(my) * 0.5f);
            character->moveX = F(double(character->moveX) * 0.5f);
        }
    } else if (f1B0 > 0.0f) {                         // TEST AH,0x41; JNE
        const double n = 1.0 / std::sqrt((double(dir.z) * dir.z + double(dir.y) * dir.y) + double(dir.x) * dir.x);
        (void)TurnToDir({F(double(dir.x) * n), F(double(dir.y) * n), F(double(dir.z) * n)});
        my = 1.0f;
    } else {
        const f32 r = TurnToDir(dir);
        my = r < 0.7f ? F(1.0 - double(r)) : 0.0f;   // FCOM [0x00295608]; TEST AH,5; JP
    }
    my = F(double(f204) * my);                        // +0x204
    return f1B0 > 0.0f ? FLT_MAX : l18;               // [0x00295604]
}

// ---- the AI-net path finder (combat_ai_port.md 4) -------------------------------------------------------------------
AiNetNode* AiNet::Nearest(const NiPoint3& p) {        // orig: 0x00031D80
    f32 best = FLT_MAX;                               // (a float local)
    s32 bi = -1;
    for (s32 i = 0; i < static_cast<s32>(nodes.size()); ++i) {
        const NiPoint3& q = nodes[static_cast<size_t>(i)].pos;
        const double dx = double(q.x) - p.x, dy = double(q.y) - p.y, dz = double(q.z) - p.z;
        const double d2 = (dz * dz + dx * dx) + dy * dy;
        if (!(d2 > best)) {                           // FCOM m32; TEST AH,0x41; JE
            best = F(d2);
            bi = i;
        }
    }
    return bi < 0 ? nullptr : &nodes[static_cast<size_t>(bi)];
}

bool AiPathFinder::Search(AiNetNode* s, AiNetNode* g) {   // orig: 0x0002E450
    goal = g;
    start = s;
    open.Clear();                                     // vf8 each
    closed.Clear();
    pool.Clear();
    path = nullptr;
    AiPathRec* p = pool.Push({s, 0.0f, 0.0f, nullptr});   // vf0 0x00031E50
    if (!p) return false;
    HeapPush(p);                                      // 0x0002DDD0
    return true;
}

AiPathRec* AiPathFinder::Step(u8& exhausted, s32 n) { // orig: 0x0002DD10
    if (path) return path;
    while (open.count) {
        AiPathRec* cur = open.data[0];                // (unclamped: the capacity is never 0)
        HeapRemoveAt(0);                              // vf4
        if (cur->node == goal) {                      // the goal: the parents become "next" links
            AiPathRec* prev = cur;
            AiPathRec* nx = cur->parent;
            cur->parent = nullptr;
            while (nx) {
                AiPathRec* t = nx->parent;
                nx->parent = prev;
                prev = nx;
                nx = t;
            }
            exhausted = 0;
            path = prev;
            return path;
        }
        Expand(cur);                                  // vf0
        closed.Push(cur);                             // 0x00031FE0
        if (n == 0) {
            exhausted = 1;
            return nullptr;
        }
        if (n > 0) --n;
    }
    exhausted = 0;
    return path;
}

void AiPathFinder::Expand(AiPathRec* r) {             // orig: 0x00032480
    AiNetNode* a = r->node;
    for (s32 i = 0; i < a->count; ++i) {              // (count re-read every pass)
        if (i >= static_cast<s32>(a->links.size())) {   // (the loader stores at most 8 links)
            NotPorted("AiPathFinder::Expand: a link count above 8");
            break;
        }
        AiNetNode::Link& l = a->links[static_cast<size_t>(i)];
        const s32 k = l.node;
        AiNetNode* b = k >= 0 && k < static_cast<s32>(net->nodes.size()) ? &net->nodes[static_cast<size_t>(k)] : nullptr;
        if (!b) {                                     // (the original reads address 0x44: a gap below `count`)
            NotPorted("AiPathFinder::Expand: a link slot without a node (address 0x44)");
            continue;
        }
        if (l.cost < 0.0f || l.cost == 0.0f) {        // FCOMP 0; TEST AH,0x41; JP (the cost is cached in the net)
            const double dx = double(a->pos.x) - b->pos.x, dy = double(a->pos.y) - b->pos.y,
                         dz = double(a->pos.z) - b->pos.z;
            l.cost = F(std::sqrt((dz * dz + dy * dy) + dx * dx));
        }
        AiPathRec n;
        n.node = b;
        n.g = F(double(l.cost) + r->g);
        const double ex = double(goal->pos.x) - b->pos.x, ey = double(goal->pos.y) - b->pos.y,
                     ez = double(goal->pos.z) - b->pos.z;
        n.f = F(((ez * ez + ey * ey) + ex * ex) + n.g);   // the squared distance as the heuristic
        n.parent = r;
        Consider(n);                                  // 0x00032320
    }
}

bool AiPathFinder::Find(AiArray<AiPathRec*>& arr, const AiPathRec& r, u32& idx) {   // orig: 0x00032090
    for (u32 i = 0; i < arr.count; ++i) {
        const AiPathRec* e = arr.At(i);
        if (e && r.node == e->node) {
            idx = i;
            return true;
        }
    }
    return false;
}

void AiPathFinder::Consider(const AiPathRec& n) {     // orig: 0x00032320
    u32 i = 0;
    if (Find(closed, n, i)) {
        AiPathRec* e = closed.At(i);
        if (e && n.f < e->f) {                        // TEST AH,5; JP
            *e = n;
            closed.RemoveAtSwap(i);                   // vf4 0x00032050
            HeapPush(e);
        }
        return;
    }
    if (Find(open, n, i)) {
        AiPathRec* e = open.At(i);
        if (e && n.f < e->f) {
            *e = n;
            if (open.count > i) {
                open.At(i) = e;
                SiftDown(i);                          // vf0C (a sift down after a decrease: the original's choice)
            }
        }
        return;
    }
    if (AiPathRec* p = pool.Push(n)) HeapPush(p);     // vf0 0x00031E50
}

void AiPathFinder::HeapPush(AiPathRec* r) {           // orig: 0x0002DDD0
    ++open.count;
    if (open.count > open.data.size()) open.data.resize(open.count + 5);   // 0x0002CD90 (no grow-flag test)
    u32 i = open.count - 1;
    while (i != 0) {
        const u32 p = (i - 1) >> 1;
        AiPathRec* q = open.At(p);
        if (q && r && q->f < r->f) break;             // FLD q+8; FCOMP r+8; TEST AH,5; JNP
        open.At(i) = q;
        i = p;
    }
    open.At(i) = r;
}

void AiPathFinder::HeapRemoveAt(u32 i) {              // orig: 0x000320F0
    if (open.count <= i) return;
    --open.count;
    open.At(i) = open.At(open.count);
    SiftDown(i);                                      // vf0C (tail jump)
}

void AiPathFinder::SiftDown(u32 i) {                  // orig: 0x00032130
    AiPathRec* x = open.At(i);
    if (i >= open.count) return;
    for (;;) {
        u32 c = 2 * i + 1;
        if (c >= open.count) break;
        if (c + 1 < open.count) {
            AiPathRec* l = open.At(c);
            AiPathRec* r = open.At(c + 1);
            if (r && l && r->f < l->f) ++c;           // TEST AH,5; JP
        }
        AiPathRec* y = open.At(c);
        if (!y || !x || !(y->f < x->f)) break;        // TEST AH,5; JP
        open.At(i) = y;
        i = c;
    }
    open.At(i) = x;
}

// ---- the AI noise and the hearing memory (combat_ai_port.md 5) ------------------------------------------------------
bool AiAgent::Accepts() const { return enabled && level.aiEnabled; }   // orig: 0x0002D000 (+0x1A8, S->M, byte M+0)

void AiAgent::Hear(const NiPoint3& p, f32 loud) {     // orig: 0x0002CF70
    if (state == 2 || state == 3) return;
    const double dx = double(pos.x) - p.x, dy = double(pos.y) - p.y, dz = double(pos.z) - p.z;
    const double d2 = (dx * dx + dz * dz) + dy * dy;
    const double r2 = Pf(profile, 5);                 // +0xE8
    if (d2 > r2) return;                              // FCOM; TEST AH,0x41; JE
    HeardAdd(p, F((1.0 - d2 / r2) * loud));           // 0x00032AD0 (the position by value)
}

void AiAgent::HeardAdd(const NiPoint3& p, f32 f) {    // orig: 0x00032AD0
    f32 best = 225.0f;                                // (a float local)
    s32 bi = -1;
    for (s32 i = 0; i < smCount; ++i) {
        const AiHeard& e = heard[static_cast<size_t>(i)];
        const double dx = double(p.x) - e.pos.x, dy = double(p.y) - e.pos.y, dz = double(p.z) - e.pos.z;
        const double d2 = (dz * dz + dy * dy) + dx * dx;
        if (!(best < d2)) {                           // FLD best; FCOMP ST1; TEST AH,5; JNP
            best = F(d2);
            bi = i;
        }
    }
    if (bi >= 0) {                                    // within 15 units: merged (picked unchanged)
        AiHeard& e = heard[static_cast<size_t>(bi)];
        e.strength = F(double(f) + e.strength);
        e.pos = p;
        return;
    }
    if (smCount < 5) {
        heard[static_cast<size_t>(smCount)] = {f, p, 0};
        ++smCount;
        return;
    }
    s32 mi = -1;
    double m = f;
    for (s32 i = 0; i < smCount; ++i)
        if (!(m < heard[static_cast<size_t>(i)].strength)) {   // FCOM; TEST AH,5; JNP
            m = heard[static_cast<size_t>(i)].strength;
            mi = i;
        }
    if (mi >= 0) heard[static_cast<size_t>(mi)] = {f, p, 0};   // (the raw argument)
}

void AiAgent::HeardDecay(const NiPoint3& at, f32 amount) {   // orig: 0x00032C30
    for (s32 i = 0; i < smCount; ++i) {
        AiHeard& e = heard[static_cast<size_t>(i)];
        const double dx = double(at.x) - e.pos.x, dy = double(at.y) - e.pos.y, dz = double(at.z) - e.pos.z;
        if ((dz * dz + dx * dx) + dy * dy < 225.0)    // [0x00295694]; TEST AH,5; JP
            e.strength = F(double(e.strength) - double(amount) * 10.0f);
        if (!e.picked) e.strength = F(double(e.strength) - amount);
        if (e.strength < 0.0f) {                      // TEST AH,5; JP
            for (s32 k = i; k < smCount - 1; ++k) heard[static_cast<size_t>(k)] = heard[static_cast<size_t>(k + 1)];
            --smCount;                                // (i is not revisited: the shifted entry is skipped)
        }
    }
}

bool AiAgent::HeardPick(f32 threshold, NiPoint3& out) {   // orig: 0x00032A60
    double best = threshold;
    s32 bi = -1;
    for (s32 i = 0; i < smCount; ++i) {
        AiHeard& e = heard[static_cast<size_t>(i)];
        e.picked = 0;
        if (!(best > e.strength)) {                   // FCOM; TEST AH,0x41; JE
            best = e.strength;
            bi = i;
        }
    }
    if (bi < 0) return false;
    heard[static_cast<size_t>(bi)].picked = 1;
    out = heard[static_cast<size_t>(bi)].pos;
    return true;
}

bool AiAgent::Check() {                               // orig: 0x0002EC20
    if (player) return false;
    HeardDecay(pos, F(double(acc) * 0.05f));          // [0x00293BDC]
    NiPoint3 q;
    if (!HeardPick(F(double(static_cast<s32>(profile[6])) * 0.01f), q)) return false;   // FILD; FMUL [0x00293AAC]
    if (guard) (void)TurnTo(q);                       // 0x0002D360 (FSTP ST0)
    else (void)MoveTo(q);
    return true;
}

bool AiAgent::Process() {                             // orig: 0x0002E9C0 (table 0x0002EBD8)
    switch (event.kind) {
    case 1: {                                         // Wait 0x0002EAA3
        const double x = double(event.value) - acc;   // FLD [+0x1E0]; FSUB [A+0x30]; FST
        event.value = F(x);
        if (x < 0.0 || x == 0.0) event.kind = 0;      // FCOMP 0; TEST AH,0x41; JP (NaN keeps waiting)
        return false;
    }
    case 2:                                           // Goto 0x0002E9E8
        f208 = event.value;                           // (+0x1E0 -> +0x208, a dword move)
        if (!(MoveTo(event.point) == 0.0f)) return true;   // FCOMP 0; TEST AH,0x44; JP (NaN: not arrived)
        event.kind = 0;
        return false;
    case 4:                                           // LookTarget 0x0002EA21
        event.point = targetPos;                      // +0x1C8 -> +0x1EC (dword copies)
        [[fallthrough]];
    case 3: {                                         // Look 0x0002EA3D
        const f32 r = TurnTo(event.point);
        // FCOMP [0x00293BDC] (0.05f); TEST AH,0x41; JP (NaN: not done) / the value: TEST AH,0x41; JNP
        if ((r < 0.05f || r == 0.05f) && (event.value < 0.0f || event.value == 0.0f)) {
            event.kind = 0;
            return false;
        }
        if (event.value > 0.0f) {                     // TEST AH,0x41; JNE
            event.value = F(double(event.value) - acc);
            return true;
        }
        event.value = 0.0f;                           // (also NaN)
        return true;
    }
    case 6: {                                         // Animate 0x0002EAD1 (only native 357 AIAddCommandAnimate adds it)
        // anim (+0x1E8) is the script's name pointer, read now (the original passes it unchecked to 0x00039BA0;
        // a word that is no VM address is reported: crash guard)
        const char* name = nullptr;
        if (event.anim && mem::IsVmAddr(event.anim)) name = static_cast<const char*>(mem::FromVmAddr(event.anim));
        else if (event.anim) NotPorted("AiAgent::Process: Animate's name pointer outside the VM (0x0002EAD1)");
        ni::NiSequence* s = name ? character->FindSequence(name) : nullptr;   // 0x00039BA0 (ecx = A+0x878)
        if (!s) {
            event.kind = 0;
            return false;
        }
        b80 = 1;                                      // byte A+0x80 (ClearIntents keeps it while the kind is 0xF)
        character->PlaySequenceDirect(s, false);      // 0x0003A4B0(s, 0)
        event.kind = 0xF;
        return true;
    }
    case 15:                                          // 0x0002EB0B: the Animate sequence played to its end
        if (!character->finished) return true;        // byte ch+0x9B
        b80 = 0;
        event.kind = 0;
        return false;
    case 5: guard = static_cast<u8>(event.arg); event.kind = 0; return false;
    case 7: event.kind = 0; return true;
    case 8: playerRun = static_cast<u8>(event.arg); event.kind = 0; return false;
    case 9: return true;
    case 10:
        if (state == 1) seq248.Reset();
        else if (state == 2) seq458.Reset();
        else if (state == 3) seq668.Reset();
        event.kind = 0;
        return false;
    case 11: case 13: case 14: return false;
    case 12: f20C = event.value; event.kind = 0; return false;   // (a dword move)
    default: return false;                            // 0 and > 15
    }
}

Character* AiAgent::Hunt() {                          // orig: 0x0002E2E0
    if (player || !profile[0] || level.aiBlind) return nullptr;   // DAT_002DBB40
    for (Player* p : level.players) {                 // 0x000C2730 (S+0x98 via +0xC4C)
        if (p->IsDead()) continue;
        const f32 bc = p->cdef.boundCenter;           // FLD; FSTP local
        NiPoint3 t = p->GetWorldPosition();
        t.y = F(double(bc) + t.y);
        if (CanSee(t)) {
            if (state == 1) AlertEngage(*p);
            else if (state == 2) Engage(*p);
            else target = p;
            return p;
        }
    }
    return nullptr;
}

bool AiAgent::CanSee(const NiPoint3& t) {             // orig: 0x0002D030
    if (t.x == seenTarget.x && t.y == seenTarget.y && t.z == seenTarget.z && pos.x == seenFrom.x &&
        pos.y == seenFrom.y && pos.z == seenFrom.z)
        return canSee != 0;                           // (NaN compares unequal)
    seenTarget = t;
    seenFrom = pos;
    const double dx = double(t.x) - pos.x;
    const f32 dy = F(double(t.y) - pos.y);
    const double dz = double(t.z) - pos.z;
    const double d2 = (dz * dz + dx * dx) + double(dy) * dy;
    if (d2 > double(std::bit_cast<f32>(profile[4]))) {   // FCOM [+0xE4]; TEST AH,0x41; JNE
        canSee = 0;
        return false;
    }
    const f32 d = F(std::sqrt(d2));                   // FSQRT; FSTP (the argument slot)
    const double inv = 1.0 / double(d);               // FLD 1.0; FDIV m32
    const NiPoint3 u{F(dx * inv), F(double(dy) * inv), F(dz * inv)};
    if (state != 3) {                                 // the field of view (not in combat)
        const double c = 1.0 - ((double(u.z) * axisX.z + double(u.y) * axisX.y) + double(u.x) * axisX.x);
        if (c > double(std::bit_cast<f32>(profile[1]))) {   // FCOMP [+0xD8]; TEST AH,0x41; JNE
            canSee = 0;
            return false;
        }
    }
    if (profile[13]) {                                // sees without a ray
        canSee = 1;
        return true;
    }
    f32 dist = d;
    canSee = ni::RayCast(pos, u, dist, true, 3) ? 0 : 1;   // 0x000FB780(&pos, &u, &d, 1, 3); NEG; SBB; INC
    return canSee != 0;
}

void AiAgent::SetProfile(const std::array<u32, 26>& p) {   // orig: 0x0002CF00 (native 372)
    profile = CopyAIProfile(p);                       // 0x00032630
    timer200 = std::bit_cast<f32>(profile[23]);       // +0x200 = +0x130
    if (!profile[12] && character && crt::StrICmp(character->cdef.name.c_str(), "crocodile") == 0) profile[12] = 1;
    level.junkbotDef = level.defs ? level.defs->Find("junkbot") : nullptr;   // DAT_002DBB44 (Session::FindDef)
}

// ---- GenericNpc -------------------------------------------------------------------------------------------------
GenericNpc::GenericNpc(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : Character(lvl, d) {                             // orig: 0x00047B80
    // (H) +0x500 is written by no constructor (heap garbage until the first pass 0's UpdateCameraDistance)
    lvl.genericNpcs.push_back(this);                  // S+0x9C (+0x8B0 next, +0x8B4 prev)
    body = std::make_unique<phys::CharBody>(d.boundWidth, d.boundHeight, d.boundDepth,
                                            lvl.physic ? &lvl.physic->world : nullptr);   // new(0x48) 0x000AF0D0
    // (H) the body constructor never writes +0x45 nor its shape's +0x9C: heap garbage in the original (KNOWN_ISSUES #41)
    body->shape.b1 = flag60;                          // shape+0xB1
    if (flag60) body->b1A = 1;
    body->owner = this;                               // vf28
    Character::SetPosition(pos);                      // 0x000353B0
    Character::SetRotation(rot);                      // 0x00033D20
    UpdateRoom();                                     // 0x00096F20 (CharacterPoint; no owner yet)
    serial = g_npcSerial++;                           // +0x7D0
    agent = std::make_unique<AiAgent>(lvl, *this);    // +0x58 = new(0x8A8)
}

GenericNpc::~GenericNpc() {                           // orig: 0x00047E60 (the deleting dtor: 0x00049FE0)
    GenericNpc::UnloadModel();                        // 0x00047E9D: 0x000466A0 (a no-op after the class dtor's)
    // +0x824 (detached from its parent and released) and +0x8A8 (operator delete): only zeroed by the ctor
    // (0x00047BD4 / 0x00047E0D), never set
    drops.reset();                                    // +0x8AC: 0x00076080, operator delete
    std::erase(level.genericNpcs, this);              // +0x8B0 / +0x8B4 (head S+0x9C)
    // (DAT_002DC9F4, a heap statistic of the bytes the drop list freed: not kept; then the Character dtor 0x0003D810)
}

GenericNPCDef& GenericNpc::npcDef() const { return static_cast<GenericNPCDef&>(cdef); }

bool GenericNpc::Init() {                             // orig: 0x00047FE0
    Character::Init();
    const GenericNPCDef& d = npcDef();
    if (d.flags170[0x18] || d.flags170[0x1A] || d.flags170[0x19]) {   // +0x188 / +0x18A / +0x189
        physic = new PhysicController(*this);         // new(0x70) 0x00037730
        ni::NiTimeController* c = keyframes.Get();    // appended at the end of the KFM's chain (0x0013EC10 / 0x0013EDD0)
        while (c->nextControllerPtr) c = c->nextControllerPtr.Get();
        c->nextControllerPtr = physic.Get();
    }
    const char* n = d.name.c_str();
    if (crt::StrICmp(n, "sungod") == 0) type = 0xC, BuildModel();
    if (crt::StrICmp(n, "destructor") == 0) type = 0xE, BuildModel();
    if (crt::StrICmp(n, "tank") == 0) type = 0xB, BuildModel();
    if (crt::StrICmp(n, "igner_boss") == 0) BuildModel();
    if (crt::StrICmp(n, "chickenwalker") == 0) type = 0xA;
    updateDistance = d.value14C;                      // +0x64 = D+0x14C
    return true;
}

void GenericNpc::SetId(u32 newId) {                   // orig: 0x00046770
    GameObject::SetId(newId);
    if (npcDef().flag1A0) level.AddEnemy(static_cast<u16>(id));   // 0x000C7850
}

void GenericNpc::ApplyInstanceString(const char* s) { // orig: 0x00047AE0
    if (!s || std::strlen(s) <= 2) return;            // (JBE: unsigned length)
    if (!drops) drops = std::make_unique<DropList>(id);   // +0x8AC = new(0x20) 0x00075D90(+0x20)
    (void)drops->Parse(s, level);                     // 0x00076310 (result ignored; used at death 0x0004794A)
}

void GenericNpc::BuildModel() {                       // orig: 0x00047850
    if (body) {
        body->shape.b1 = flag60;
        if (flag60) body->b1A = 1;
    }
    Character::BuildModel();                          // 0x0003DF70
}

bool GenericNpc::ShouldUnloadModel() const {          // orig: 0x000467B0
    if (level.player && level.player->IsHeldBy(this)) return false;   // S+0x94 && 0x000503B0(this): holding it
    return Character::ShouldUnloadModel();            // 0x00035020
}

void GenericNpc::Think(f32 dt) {                      // orig: 0x0004A000
    (void)dt;
    const NiPoint3 zero{};
    if (npcDef().flags170[0xC]) {                     // D+0x17C
        body->SetForce(zero);
        type = 7;
    } else {
        body->SetForce(kGravity);
        body->shape.pinned = 0;                       // shape+0xB8
    }
    if (!model) return;
    NiPoint3 vel{};
    b7BE = 0;
    if (flag28) return;
    if (cameraDistance > updateDistance) return;      // FCOMP; TEST AH,0x41; JE (only '>' returns)
    AiTick();                                         // vfF0
    if (!(flag28 || IsDead() || !hasControl)) {
        ni::NiMatrix3 rot = rotation;                 // vf38
        if (!(moveX == 0.0f)) {                       // FCOMP [0x00293AB8]; TEST AH,0x44; JNP (NaN turns)
            const f32 a = F(double(npcDef().rotSpeed) * moveX * g_frameStep);   // FLD D+0x88; FMUL; FMUL; FSTP
            rot = ni::Multiply(rotation, ni::MakeYRotation(a));   // 0x001030B0, 0x00013B30 (ECX = vf38's matrix)
        }
        if (moveY == 0.0f && strafe == 0.0f) {        // +0x4DC / +0x4E0
            vel = zero;
            if (body->supported) {
                body->SetForce(zero);
                body->shape.pinned = 1;
            }
        } else {                                      // orig: 0x0004A249
            const GenericNPCDef& d = npcDef();
            const NiPoint3 X = ni::MulVector(rot, {1, 0, 0});    // 0x00011770 (0x002D77E0)
            const NiPoint3 Z = ni::MulVector(rot, {0, 0, 1});    // (0x002D77F8)
            const double zx = double(Z.x) * strafe;   // stays on the x87 stack
            const f32 ay = F(double(Z.y) * strafe), az = F(double(Z.z) * strafe);
            const f32 bx = F(double(X.x) * moveY), by = F(double(X.y) * moveY), bz = F(double(X.z) * moveY);
            vel = {F(double(bx) + zx), F(double(by) + ay), F(double(bz) + az)};
            const double len = ni::Unitize(vel);      // 0x000116A0
            f32 m = F(len);                           // FST
            if (len > 1.0f) m = 1.0f;                 // FCOMP [0x00293A98]; TEST AH,0x41; JNE
            f32 speed = d.runSpeed;                   // FLD D+0x78
            if (m <= 0.25f) speed = d.sneakSpeed;     // [0x00293B50]; TEST AH,0x41; JP (NaN: run)
            else if (m <= 0.7f) speed = d.walkSpeed;  // [0x00295608]
            vel = {F(double(vel.x) * speed), F(double(vel.y) * speed), F(double(vel.z) * speed)};
        }
        Move(vel, rot);                               // vfF4
    }
    PostThink();                                      // vfF8
}

void GenericNpc::AiTick() {                           // orig: 0x00046580
    if (!agent || IsDead()) {
        attack2 = attack1 = b509 = b508 = 0;
        moveY = moveX = strafe = 0;
    }
    if (!IsDead() && agent) agent->Update(g_frameStep);   // 0x0002FDE0(DAT_002D67E8)
}

void GenericNpc::UnloadModel() {                      // orig: 0x000466A0
    StopVoiceAndClear(voice87C);                      // (each: non-zero && Voice_Exists -> Voice_Stop, 0)
    StopVoiceAndClear(voice878);
    StopVoiceAndClear(voice874);
    Character::UnloadModel();                         // jmp 0x0003B7D0
}

void Nibbler::UnloadModel() {                         // orig: vf68 0x0008B410
    GenericNpc::UnloadModel();                        // 0x000466A0
    StopVoiceAndClear(voice8CC);                      // (+0x8CC && Voice_Exists: Voice_Stop, 0)
}

// ---- inventory_batch1_port.md 7 (checked against the assembly of 0x00046DF0, 0x00049190, 0x00047A40, 0x000465F0) ----
void GenericNpc::OnMeshContact(Character* other, f32 amount, bool flag) {   // orig: vfB8 0x00046DF0 (RET 0xC)
    const GenericNPCDef& d = npcDef();
    if (d.flags170[0x1C]) {                           // D+0x18C (byte): the knock-back
        Damage(F(double(maxHealth) * 0.125f * 3.0f), other);   // FLD +0x4FC; FMUL [0x00293C34]; FMUL [0x0029563C]; vf7C
        if (IsDead()) return;                         // vfA8
        const s32 r1 = crt::Rand();                   // 0x00046E40 (other is not null-checked)
        other->body->velocity.x = static_cast<f32>(50 - r1 % 100);   // CDQ; IDIV 100; FILD; FSTP (+0x5C ->+0x28)
        other->body->velocity.y = 40.0f;              // 0x42200000
        const s32 r2 = crt::Rand();                   // 0x00046E6C
        other->body->velocity.z = static_cast<f32>(50 - r2 % 100);
        return;
    }
    if (type == 0xE && flag) {                        // +0x774: the Destructor
        Character::Damage(amount, other);             // 0x000354B0 (the base, not the slot)
        if (IsDead()) {                               // vfA8
            PlayAnimById(0x4D, true, false, false, 0.0f);   // 0x0003CE70(0x4D, 1, 0, 0, 0)
            return;
        }
        PlayAnimById(0x47, true, false, false, 0.0f);
        if (health <= 0.0f) {                         // TEST AH,0x41; JP (unreachable after IsDead: kept)
            StopVoiceAndClear(voice878);              // (each: non-zero && Voice_Exists -> Voice_Stop, 0)
            StopVoiceAndClear(voice874);
            StopVoiceAndClear(voice87C);
            StopVoiceAndClear(voice880);
            if (voice884) return;
            const NiPoint3 pos = GetWorldPosition();  // vf34
            voice884 = static_cast<u32>(SoundAt(0, &pos, 3, 0, nullptr));
            VoiceMaxDistance(static_cast<s32>(voice884), d.value198);   // 0x00046FF3
            return;
        }
        const NiPoint3 pos = GetWorldPosition();      // vf34
        VoiceMaxDistance(SoundAt(2, &pos, 3, 0, nullptr), d.value198);   // (0x00047024 -> 0x00046FE8)
        return;
    }
    if (type == 10 && flag) {
        Character::Damage(amount, other);             // 0x000354B0
        if (amount > 1.0f) {                          // [0x00293A98]; TEST AH,0x41; JNE
            if (health <= 0.0f) {                     // TEST AH,0x41; JP
                StopVoiceAndClear(voice878);
                StopVoiceAndClear(voice874);
                StopVoiceAndClear(voice87C);
                if (!voice884) {
                    const NiPoint3 pos = GetWorldPosition();   // vf34
                    voice884 = static_cast<u32>(SoundAt(0, &pos, 3, 0, nullptr));
                    VoiceMaxDistance(static_cast<s32>(voice884), d.value198);   // 0x0004712F
                }
                RemoveModelCollisionMeshes();         // 0x00035230 (0x000FC2D0 when the owner is a stage)
                flag60 = 0;                           // +0x60
            } else {
                const NiPoint3 pos = GetWorldPosition();
                VoiceMaxDistance(SoundAt(2, &pos, 3, 0, nullptr), d.value198);   // 0x0004716D
            }
        }
        if (agent && other && other->def->IsType(1)) agent->OnDamage(amount, other);   // 0x0002DEC0 (+0x58)
        return;
    }
    if (crt::StrICmp(def->name.c_str(), "tentacle_dude") == 0) {   // 0x00295A24
        Character::Damage(amount, other);
        if (agent && other && other->def->IsType(1)) agent->OnDamage(amount, other);
        return;
    }
    if (crt::StrICmp(def->name.c_str(), "aquafish") == 0 && other && other->def->IsType(1)) {   // 0x00297450
        other->Damage(50.0f, this);                   // other->vf7C(50.0, this)
        Damage(maxHealth, other);                     // this->vf7C(+0x4FC, other)
    }
}

void GenericNpc::AttackFromTip(u32 kind) {            // orig: vfBC 0x00049190 (RET 4)
    b7BE = 1;                                         // +0x7BE
    ni::NiNode* tip = tips.empty() ? nullptr : tips[0];   // +0x78C -> [0] (the original reads data[0] unchecked)
    NiPoint3 p;
    ni::NiMatrix3 R;
    if (tip) {
        UpdateNodeAt(tip, animTime);                  // NiAVObject::Update(+0x78, 1)
        p = tip->world.translate;                     // +0xA0
        R = tip->world.rotate;                        // +0x70 (12 dwords)
    } else {
        p = CharacterPoint();                         // vf78
        R = GetWorldRotation();                       // vf38
    }
    StartAttack(kind, p, R);                          // vfFC 0x00046AA0
}

void GenericNpc::AddDrop(Def* d, const char* dropLabel) { // orig: vfEC 0x00047A40 (RET 8)
    if (!d->IsType(4)) return;                        // vf0C(4): the pickup family
    if (!drops) drops = std::make_unique<DropList>(id);   // new(0x20) + 0x00075D90(+0x20)
    drops->AddEntry(d, dropLabel);                    // 0x000760E0
    drops->mode = 0;                                  // 0x0015C260(0): mode_all
}

bool GenericNpc::Revive() {                           // orig: vf110 0x000465F0
    if (!IsAnimPlaying(99)) {                         // 0x00033E20(0x63)
        health = maxHealth;                           // +0x4F4 = +0x4FC (dword)
        b7C1 = 1;                                     // +0x7C1
        u7CC = 0;                                     // +0x7CC
        cdef.ingameEvents.Trigger(10, {nullptr, nullptr}, IngameContext());   // 0x000984D0 arg (both words 0);
        return false;                                 //  D+0x28 0x000F28F0(10, &arg); the arg's dtor 0x000F0C00
    }
    return finished != 0;                             // +0x9B (the revive animation 99 has ended)
}

void GenericNpc::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x00090F50
    SetRotation(rot);                                 // vf24
    if (IsIdling() || IsAnimPlaying(0xD) || IsAnimPlaying(0x15) || IsAnimPlaying(0x19) || IsAnimPlaying(0x1A) ||
        IsAnimPlaying(0x16) || IsAnimPlaying(0x17))
        AddVelocity(*body, vel);                      // vf50
}

void GenericNpc::PostThink() {                        // orig: 0x00049040
    const u32 kind = npcDef().value144;
    if (kind == 0xE || kind != 0xF) return;
    if (!(health > 0.0f)) return;                     // TEST AH,0x41; JNE
    const double x = double(g_frameStep) + f7A0;      // FLD step; FADD [+0x7A0]; FST
    f7A0 = F(x);
    if (!(x > 1.0f)) return;                          // FCOMP [0x00293A98] (unrounded); TEST AH,0x41; JNE
    ni::NiAVObject* tip = GetObjectByName(model.Get(), "tip");      // model vf54
    ni::NiAVObject* sword = GetObjectByName(model.Get(), "sword2");
    if (tip && sword) {
        Def* fx = level.defs->Find("skeleton_particles");   // Session::FindDef 0x000C3F10 (no null check)
        auto* e = static_cast<EmitterInstance*>(fx->SpawnEffect(level, tip->world.translate, {0, 1, 0}));   // vf20
        e->sourceA = tip;                             // NiPointer::Assign +0x40
        e->sourceB = sword;                           // NiPointer::Assign +0x44
    }                                                 // (else: an empty debug print 0x000B0420)
    f7A0 = 0;
}

void GenericNpc::OnTextKey(f32 keyTime, const char* text) {   // orig: 0x00047550
    (void)keyTime;
    if (crt::StrICmp(text, "morph:lfdown") == 0 || crt::StrICmp(text, "morph:rfdown") == 0) {
        AiNoise(level, GetWorldPosition(), 0.2f);     // (S+0x114)->0x00031420(vf34(), 0.2f)
        return;
    }
    if (crt::StrICmp(text, "morph:firew") == 0) {     // the ranged attack's shot (0x000475EE)
        ni::NiNode* tip = tips.empty() ? nullptr : tips[0];   // (+0x78C)[0]
        if (!b7BE) {                                  // vf100 did not run this frame: the tip's transform
            if (!tip) return;
            TipToKeyTime(keyTime, tip);               // 0x00034A10
            aimPos = tip->world.translate;            // +0xA0
            aimRot = tip->world.rotate;               // +0x70 (12 dwords)
        } else if (tip) {                             // (aimRot keeps vfFC's matrix)
            TipToKeyTime(keyTime, tip);
            aimPos = tip->world.translate;
        }
        if (agent) agent->Aim(aimPos, aimRot);        // 0x0002DF50(&+0x7D8, &+0x7F0, 0) (nothing but for 0xE / 0xC)
        Shoot(npcDef().value144, aimPos, aimRot, false, 0.0f);   // vf104(D+0x144, &+0x7D8, &+0x7F0, 0, 0.0f)
        return;
    }
    const bool push = crt::StrICmp(text, "morph:push") == 0;   // 0x002974B4 (object_gaps.md 14)
    if (push || crt::StrICmp(text, "morph:hit") == 0) {   // 0x002974A8
        if (agent) agent->Aim(aimPos, aimRot);        // 0x0002DF50(&+0x7D8, &+0x7F0, 0) (nothing but for 0xE / 0xC)
        TipToKeyTime(keyTime, tips.empty() ? nullptr : tips[0]);   // 0x00034A10(keyTime, (+0x78C)[0]; no test)
        HitSpheres(push ? 2 : 1);                     // vfD8
        return;
    }
    if (crt::StrICmp(text, "morph:hitstart") == 0) { HitStart(); return; }   // vfDC 0x00033DF0
    if (crt::StrICmp(text, "morph:hitend") == 0) { HitEnd(); return; }       // vfE0 0x000360B0
    if (crt::StrICmp(text, "morph:shaking") == 0) return;
    if (crt::StrICmp(text, "morph:swing") == 0) { HitStart(); return; }
    if (crt::StrICmp(text, "morph:swing2") == 0) HitEnd();
}

// ---- the ranged attack (docs/analysis/npc_combat2_port.md 1.1 / 1.2 / 1.6) -------------------------------------------
void GenericNpc::Fire(u32 kind, const NiPoint3& dir) {   // orig: vf100 0x00049260
    b7BE = 1;
    NiPoint3 p;
    if (ni::NiNode* tip = tips.empty() ? nullptr : tips[0]) {   // (+0x78C)[0]
        b7D4 = 1;
        UpdateNodeAt(tip, animTime);                  // NiAVObject::Update(+0x78, 1) 0x001018D0
        p = tip->world.translate;                     // +0xA0
    } else {
        b7D4 = 0;
        p = CharacterPoint();                         // vf78
    }
    // FLD d.y; FCOMP [0x00293C08] (0.9f); TEST AH,0x41; JNE -> UY (<, == or NaN)
    const NiPoint3 up = dir.y > 0.9f ? NiPoint3{0.0f, 0.0f, 1.0f} : NiPoint3{0.0f, 1.0f, 0.0f};
    NiPoint3 c{F(double(up.y) * dir.z - double(up.z) * dir.y), F(double(up.z) * dir.x - double(up.x) * dir.z),
               F(double(up.x) * dir.y - double(up.y) * dir.x)};
    (void)ni::Unitize(c);                             // 0x000116A0 (FSTP ST0)
    ni::NiMatrix3 m;                                  // chunk k = column k: (dir, c x dir, -c)
    m.m[0][0] = dir.x, m.m[1][0] = dir.y, m.m[2][0] = dir.z;
    m.m[0][1] = F(double(c.y) * dir.z - double(c.z) * dir.y);
    m.m[1][1] = F(double(c.z) * dir.x - double(c.x) * dir.z);
    m.m[2][1] = F(double(c.x) * dir.y - double(c.y) * dir.x);
    m.m[0][2] = -c.x, m.m[1][2] = -c.y, m.m[2][2] = -c.z;   // FCHS
    StartAttack(kind, p, m);                          // vfFC
}

void GenericNpc::StartAttack(u32 kind, const NiPoint3& pos, const ni::NiMatrix3& rot) {   // orig: vfFC 0x00046AA0
    if (attack2 || attack1) return;                   // +0x507 / +0x506
    const GenericNPCDef& d = npcDef();
    const auto busy = [&] {                           // 0x00033E20 in this order
        return IsAnimPlaying(0x2F) || IsAnimPlaying(0x30) || IsAnimPlaying(0x34) || IsAnimPlaying(0x35);
    };
    if (kind == 0) {
        if (d.flags170[0x1A]) {                       // byte D+0x18A: the request without a transform
            attack1 = 1;
            return;
        }
        if (!d.flags170[0x1B]) {                      // byte D+0x18B
            if (!SeqById(0x2F) || busy()) return;
        }
        aimPos = pos;                                 // +0x7D8 (3 dwords)
        aimRot = rot;                                 // +0x7F0 (12 dwords)
        attack1 = 1;
    } else if (kind == 1) {
        if (!d.flags170[0x1B]) {
            if (!SeqById(0x34) || busy()) return;
        }
        aimPos = pos;
        aimRot = rot;
        attack2 = 1;
    }
}

void GenericNpc::Shoot(u32 kind, const NiPoint3& pos, const ni::NiMatrix3& rot, bool silent, f32 t0) {   // 0x00049420
    if (!curSeq || !EntryOf(curSeq)) return;          // CharacterDef::FindAnimationBySequence 0x0003FD20
    // (the second FindAnimationBySequence's GetDamage 0x0003EE90 is dropped: FSTP ST0)
    const GenericNPCDef& d = npcDef();
    ProjectileDef* pd = kind == 0x11 ? d.projectile15C : d.weapon158;
    if (pd) {
        if (d.value144 == 4 || d.value144 == 0xA) {
            // (4: no definition writes it, U; 0xA: the flower plant's five random shots, no instance in the shipped
            //  stages)
            NotPorted("GenericNpc::Shoot: the shotgun / spread kinds (0x000494B4 / 0x0004972F)");
        } else {
            ni::NiMatrix3 m = rot;                    // esp+0x40 (12 dwords of rot)
            const bool goon = d.value144 == 0xB && crt::StrICmp(d.name.c_str(), "goonsneak") == 0;   // 0x002976A8
            if (d.value144 == 0xB && goon) {          // 0x000498C2: the goon's lob, 0.75 up from the facing
                NiPoint3 f = ni::MulVector(rot, {1.0f, 0.0f, 0.0f});   // 0x00011770 (UNIT_X 0x002D77E0) -> esp+0x20
                f.y = 0.75f;                          // 0x3F400000
                (void)ni::Unitize(f);                 // 0x000116A0
                NiPoint3 r = ni::Cross({0.0f, 1.0f, 0.0f}, f);   // 0x00011700 on UNIT_Y (0x002D77EC copies)
                (void)ni::Unitize(r);
                NiPoint3 u = ni::Cross(r, f);
                (void)ni::Unitize(u);
                m.m[0][0] = f.x, m.m[1][0] = f.y, m.m[2][0] = f.z;   // the columns f, u, -r (FCHS)
                m.m[0][1] = u.x, m.m[1][1] = u.y, m.m[2][1] = u.z;
                m.m[0][2] = -r.x, m.m[1][2] = -r.y, m.m[2][2] = -r.z;
            } else if (d.value144 == 0xB) {           // 0x000499BB (stormtrooper_mk2): rolled pi / 4 about the facing
                const NiPoint3 a = ni::MulVector(rot, {1.0f, 0.0f, 0.0f});   // esp+0x10
                m = ni::Multiply(m, ni::AxisAngle(std::bit_cast<f32>(0x3F490E56u), a.x, a.y, a.z));   // 0x00103130
            }
            // 0x00297694 / 0x00297688 / 0x002976B4
            const char* name = kind == 0x10 ? "Destructor_Laser" : kind == 0xC ? "Igner_Laser" : "Enemy-Laser";
            GameObject* o = pd->Spawn(level, &level, pos, m, name);   // vf14(S, pos, &m, name): 0x000798A0 -> vf20
            if (auto* pr = dynamic_cast<Projectile*>(o)) {
                pr->time = F(-double(t0));            // +0x24: FLD; FCHS; FSTP (-0.0 for t0 = 0)
                pr->shooter = this;                   // +0x40
                if (kind == 0x11) pr->arg74 = 1;      // +0x74
                if (d.value144 == 0xB) {              // 0x00049A90
                    if (goon) pr->speed = 40.0f;      // +0x48 = 0x42200000 (after the Init)
                    pr->arg74 = 0;                    // +0x74
                }
                if (!silent) {
                    const NiPoint3 p = GetWorldPosition();   // vf34
                    VoiceMaxDistance(SoundAt(4, &p, 3, 0, nullptr), d.value198);   // rand 0x0003FEFE; 0x00049719
                }
            }
        }
        // 0x00049B06 (only with a projectile: no D+0x158 jumps to 0x00049B6A): MulVector(rot, UX) 0x00011770 for
        // each tail (the same value)
        const NiPoint3 dir = ni::MulVector(rot, {1.0f, 0.0f, 0.0f});
        if (d.flare160) (void)d.flare160->SpawnEffect(level, pos, dir);         // vf20 0x0006A480
        if (d.particles164) (void)d.particles164->SpawnEffect(level, pos, dir); // SpriteEmitterDef 0x0006F2A0
    }
    if (kind == 0xE) f7A0 = -2.5f;                    // 0xC0200000
}

bool GenericNpc::CanBePushed() const {                // orig: vfA4 0x00047880
    return npcDef().flags170[0x1E] && Character::CanBePushed();   // byte D+0x18E; 0x00033D60
}

bool GenericNpc::IsTargetable() const {               // orig: vfB0 0x00047830
    return Character::IsTargetable() && npcDef().flags170[0x1D] != 0;   // 0x00033DA0; byte D+0x18D
}

void GenericNpc::HitSpheres(u32 n) {                  // orig: vfD8 0x00046C40
    if (type != 0xA) HitSweep("tip", npcDef().value174, npcDef().value174, n, false);   // 0x0003E220 (D+0x174)
}

void GenericNpc::ApplyHit(GameObject* t, NiPoint3& dir, u32 n) {   // orig: vfD4 0x00049B90 (combat_ai_port.md 9.4)
    if (!t) {                                         // the miss sound 0x1D unless voice +0x894 plays
        if (VoiceLive(voice894)) return;              // 0x00049BAC
        const NiPoint3 p = GetWorldPosition();
        voice894 = static_cast<u32>(SoundAt(0x1D, &p, 3, 0, nullptr));
        VoiceMaxDistance(static_cast<s32>(voice894), npcDef().value198);   // 0x00049BEE
        return;
    }
    const AnimEntry* e = curSeq ? EntryOf(curSeq) : nullptr;   // CharacterDef::FindAnimationBySequence 0x0003FD20
    if (!e) return;
    f32 dmg = e->hasDamage ? e->damage : 0.0f;        // AnimEntry::GetDamage 0x0003EE90
    if (type == 0xA && IsAnimPlaying(0x4D)) dmg = 100.0f;
    if (t->def->IsType(1)) {
        auto* c = static_cast<Character*>(t);
        f32 k = F(double(dmg) * 5.0f);                // FMUL [0x00293C24]; FST
        if (k < 5.0f) k = 5.0f;                       // TEST AH,5; JP
        else if (k > 300.0f) k = 300.0f;              // [0x002976F0]; TEST AH,0x41; JNE
        if (c->CanBePushed()) {                       // vfA4
            dir.y = 0.0f;
            (void)ni::Unitize(dir);
            NiPoint3 v;
            if (n & 2) {
                (void)c->def->IsType(2);              // (the result is not used)
                v = {F(double(dir.x) * 150.0f), F(double(dir.y) * 150.0f), F(double(dir.z) * 150.0f)};   // [0x002976EC]
            } else {
                v = {F(double(k) * dir.x), F(double(k) * dir.y), F(double(k) * dir.z)};   // 0x00011560
            }
            if (c->body) AddVelocity(*c->body, v);    // body vf50 0x000AF7D0
        }
        if (n & 1) DealDamage(c, dmg);                // vfCC
    } else if (t->def->IsType(0x40)) {
        if (auto* d = dynamic_cast<Destructable*>(t)) (void)d->Damage(dmg);   // vf64(dmg) 0x00049D87 (the 0xA override included)
    }
}

void GenericNpc::DealDamage(Character* t, f32 amount) {   // orig: vfCC 0x00046C70 (combat_ai_port.md 9.5)
    if (!t || !level.IsCharacter(t)) return;          // 0x00033EE0
    if (!(amount > 0.0f)) return;                     // TEST AH,0x41; JNE
    const NiPoint3 p1 = GetWorldPosition(), p2 = t->GetWorldPosition();
    t->Damage(amount, this);                          // vf7C
    const bool player = t->def->IsType(2);
    if (player) {
        b7BC = 1;
        (void)SoundAt(0x17, &p2, 3, 0, nullptr);      // D->0x0003FEA0(0x17, &p2, 3, 0, 0): rand with two sounds
    }
    NiPoint3 d{F(double(p2.x) - p1.x), F(double(p2.y) - p1.y), F(double(p2.z) - p1.z)};
    double l = ni::Unitize(d);                        // ST0 (unrounded)
    if (l > 25.0) l = 25.0;                           // FCOM [0x00297240]; TEST AH,0x41; JNE
    const NiPoint3 q{F(double(p2.x) - F(double(d.x) * l)), F(double(p2.y) - F(double(d.y) * l)),
                     F(double(p2.z) - double(d.z) * l)};
    if (!t->def->IsType(2)) return;
    VoiceMaxDistance(SoundAt(t->health > 0.0f ? 0x12 : 0x13, &q, 3, 0, nullptr), npcDef().value198);   // 0x00046DD3
}

// docs/analysis/npc_classes_batch2_port.md K.4.1.
void GenericNpc::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x00047240
    const GenericNPCDef& d = npcDef();
    if (amount >= health && d.flags170[0x13]) {       // FCOMP [+0x4F4]; TEST AH,1; JNE (< / NaN skip); D+0x183
        ExplodeAtCharacterRecord(level, label ? label->c_str() : nullptr);   // [0x002DAC64]->0x000C2F70(+0x34)
        HitSweep("center", 5.0f, 5.0f, 2, false);     // 0x0003E220("center" 0x00295D90, 5.0f, 2) -> 0x0003C090
    }
    if (level.IsCharacter(attacker)) {                // 0x00033EE0
        if (!attacker->def->IsType(2)) {              // an NPC attacker: one of the two agents must have profile[17]
            const bool mine = agent && agent->profile[17];                       // A->0x0002CB00()+0x44
            const bool theirs = attacker->agent && attacker->agent->profile[17];
            if (!mine && !theirs) return;             // 0x0004753F
        }
    } else {
        attacker = nullptr;                           // 0x000472D6
    }
    if (type == 0xE && !IsPlayingName("door_idle")) return;   // 0x0029617C
    Character::Damage(amount, attacker);              // 0x000354B0
    if (amount > 0.0f) {                              // FCOMP [0x00293AB8]; TEST AH,0x41; JNE
        if (!(health > 0.0f) && !std::isnan(health)) {   // TEST AH,0x41; JP ('>' / NaN: the hurt sound)
            StopVoiceAndClear(voice878);              // (non-zero && Voice_Exists: Voice_Stop, then 0)
            StopVoiceAndClear(voice874);
            StopVoiceAndClear(voice87C);
            // `!v || !Voice_Exists(v)` with the compiler's known 0: Voice_Exists(0) is called (0x000473CF)
            if (!voice884 && !level.Sound().VoiceExists(0)) {
                const NiPoint3 p = GetWorldPosition();   // vf34
                voice884 = static_cast<u32>(SoundAt(0, &p, 3, 0, nullptr));   // 0x0003FEA0
                VoiceMaxDistance(static_cast<s32>(voice884), d.value198);   // (0x00047406 -> 0x0004750D)
            }
        } else {
            const NiPoint3 p = GetWorldPosition();
            VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), d.value198);   // the hurt sound; 0x00047437
            if (attacker && attacker->def->IsType(2) && attacker->type == 3) {   // Bender's special moves
                u32 s = 0;
                if (attacker->IsAnimPlaying(0xB8)) s = 0x19;
                else if (attacker->IsAnimPlaying(0xB9) || attacker->IsAnimPlaying(0xBA)) s = 0x18;
                else if (attacker->IsAnimPlaying(0xD3) || attacker->IsAnimPlaying(0xD2)) s = 0x1A;
                if (s) {
                    const NiPoint3 q = GetWorldPosition();
                    VoiceMaxDistance(SoundAt(s, &q, 3, 0, nullptr), d.value198);   // 0x00047518
                }
            }
        }
    }
    if (agent && level.IsCharacter(attacker))         // 0x00033EE0
        agent->OnDamage(amount, attacker);            // 0x0002DEC0
}

void GenericNpc::UpdateAnimation(f32 dt) {            // orig: 0x00048190 (the reached path)
    if (!model) return;
    const NiPoint3 v = body->velocity;                // vf0C
    NiPoint3 a{v.x, 0.0f, v.z}, b{velBefore.x, 0.0f, velBefore.z};
    const f32 la = F(ni::Unitize(a));                 // 0x000116A0, FSTP
    const double lb = ni::Unitize(b);                 // (ST0, unrounded)
    f32 speed = desiredSpeed;
    if (!(std::fabs(double(la) - lb) > 0.5f)) speed = desiredSpeed = la;   // FABS; FCOMP 0.5; TEST AH,0x41; JNE
    // (b = R^T a twice and fwd = dot(Z, b): read only by the walk / strafe branch, not ported)
    const GenericNPCDef& d = npcDef();
    // (0x00103290 runs twice with the same arguments; one call suffices)
    const NiPoint3 bR = ni::TransposeMulVector(rotation, a);   // M^T a
    const f32 fwd = F((0.0 * double(bR.y) + 1.0 * double(bR.z)) + 0.0 * double(bR.x));   // (0, 0, 1) at 0x002D77F8
    const f32 t1 = F((double(d.runSpeed) - d.walkSpeed) * 0.5f + d.walkSpeed);    // FLD; FSUB; FMUL m32; FADD; FSTP
    const f32 t2 = F((double(d.walkSpeed) - d.sneakSpeed) * 0.5f + d.sneakSpeed);
    const f32 t3 = F(double(d.sneakSpeed) * 0.5f);
    bool hurt = false;                                // BL (npc_classes_batch2_port.md S.7)
    if (!(health4F8 == health)) {                     // 0x000482D2: FCOMP; TEST AH,0x44; JNP (NaN: changed)
        if (health < health4F8)                       // 0x000482E5: TEST AH,5; JP
            hurt = d.flags170[0x1C] ||                // byte D+0x18C
                   double(d.value194) * maxHealth > health ||   // FLD D+0x194; FMUL +0x4FC; FCOMP +0x4F4; TEST AH,0x41; JE
                   double(health4F8) - health >= d.value190;    // FLD; FSUB; FCOMP D+0x190; TEST AH,1; JNE skips
        f7A8 = 0;                                     // 0x00048336
    }
    health4F8 = health;                               // +0x4F8 = +0x4F4 (raw)
    if (timer7AC > 0.0f) {                            // 0x00048340: TEST AH,0x41; JNE
        const double x = double(timer7AC) - g_frameStep;   // FLD; FSUB [0x002D67E8]; FST
        timer7AC = F(x);
        if (x < 0.0f) timer7AC = 0;                   // FCOMP 0 (unrounded); TEST AH,5; JP
    }
    if (animUpdated) {
        if (IsDead()) {                               // orig: 0x000483A8 (vfA8)
            if (!(IsAnimPlaying(0x4D) || IsAnimPlaying(0x4E) || IsAnimPlaying(0x4F) || IsAnimPlaying(0x50) ||
                  IsAnimPlaying(0x51)) && d.dyingCount > 0) {   // D+0xE0 (JLE, signed)
                const s32 k = crt::Rand() % d.dyingCount + 0x4D;   // orig: rand 0x00048402 (CDQ; IDIV D+0xE0)
                if (SeqById(k)) {
                    PlayAnimById(k, true, false, true, 0.0f);   // 0x0003CE70 (pushes 0, 1, 0, 1, k)
                    VoiceMaxDistance(SoundAt(1, nullptr, 3, 0, node.Get()), npcDef().value198);   // 0x0003FFC0(1, node, 3, 0); SetMaxDistance(D+0x198)
                    if (type == 0xA) hitActive = 1;   // byte +0x5D1
                }
            }
            animUpdated = 0;                          // 0x0004846C (then AnimTick and return)
        } else if (body->supported && (velBefore.y < -40.0f || IsAnimPlaying(0x1C)) && SeqById(0x1D)) {
            // 0x00048481: body vf48; +0x4E8 < -40 [0x0029764C] (TEST AH,5; JNP) or falling 0x1C playing; then the
            // landing 0x1D (0x00033E00) is played (pushes 0, 1, 0, 1, 0x1D) -> +0x9F = 0, AnimTick, return
            PlayAnimById(0x1D, true, false, true, 0.0f);   // 0x000484C6 -> 0x0003CE70
            animUpdated = 0;                          // 0x000484D1
        } else if (v.y < -50.0f && SeqById(0x1C)) {  // 0x000484E6: FCOMP [0x00297648]; TEST AH,5; JP (NaN: no)
            PlayAnimById(0x1C, true, false, false, 0.0f);   // 0x00048510 (pushes 0, 0, 0, 1, 0x1C): falling
            animUpdated = 0;                          // 0x0004851B
        } else if (IsAnimPlaying(0x1D) && !finished) {   // 0x00048530: the landing still runs -> 0x00048466
            animUpdated = 0;                          // 0x0004846C (+0x9F = 0)
        } else if (b7C1 || b7BD || b7C0) {            // 0x0004854B / 0x000485F2 / 0x00048699 (in this order)
            u8& flag = b7C1 ? b7C1 : b7BD ? b7BD : b7C0;
            const s32 anim = b7C1 ? 0x63 : b7BD ? 0x68 : 0x64;
            if (!IsAnimPlaying(anim)) {
                PlayAnimById(anim, true, false, true, 0.0f);    // 0x0003CE70 (pushes 0, 1, 0, 1, anim)
                const NiPoint3 p = GetWorldPosition();        // vf34
                VoiceMaxDistance(SoundAt(b7C1 ? 0x4F : b7BD ? 0x5E : 0x50, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; SetMaxDistance(D+0x198)
            } else if (finished) {                    // +0x9B
                flag = 0;
            }
            animUpdated = 0;                          // (then AnimTick and return)
        } else if (hurt && timer7AC == 0.0f) {        // 0x00048740 / 0x00048748: TEST AH,0x44; JP (only '==')
            if (!IsAnimPlaying(0x47) && !IsAnimPlaying(0x48) && !IsAnimPlaying(0x49) && d.damageCount > 0) {   // JLE
                const s32 k = crt::Rand() % d.damageCount + 0x47;   // orig: rand 0x00048793 (CDQ; IDIV D+0xE4)
                if (SeqById(k)) {
                    PlayAnimById(k, true, false, true, 0.0f);   // 0x0003CE70 (pushes 0, 1, 0, 1, k)
                    timer7AC = F(SequenceDuration(curSeq) + 0.5f);   // 0x00036210(+0xA4); FADD [0x00293AA0]; FSTP
                }
            }
            animUpdated = 0;                          // 0x000487E1 (then AnimTick and return)
        } else if (IsAnimPlaying(0xC0)) {
        } else if (IsAnimPlaying(0x59)) {             // 0x0004880A (prepare_attack1)
            if (finished) {
                b7BC = 0;
                PlayAnimById(0x2F, true, false, true, 0.0f);   // (pushes 0, 1, 0, 1, 0x2F)
                ++count7B0;
                timer7A4 = animTime;                  // +0x7A4 = +0x78 (a dword copy)
            }
            animUpdated = 0;                          // (then AnimTick and return)
        } else if (IsAnimPlaying(0x5A)) {             // 0x0004886A (prepare_attack2)
            if (finished) {
                b7BC = 0;
                PlayAnimById(0x34, true, false, true, 0.0f);
                ++count7B4;
                timer7A4 = animTime;
            }
            animUpdated = 0;
        } else if (IsAnimPlaying(0x2F) && !attack1 && d.flags170[0x16]) {   // 0x000488CA (byte D+0x186)
            if (finished) PlayAnimById(0x5B, true, false, true, 0.0f);       // end_attack1
            animUpdated = 0;
        } else if (IsAnimPlaying(0x34) && !attack2 && d.flags170[0x17]) {   // 0x00048924 (byte D+0x187)
            if (finished) PlayAnimById(0x5C, true, false, true, 0.0f);       // end_attack2
            animUpdated = 0;
        } else if (!finished && !IsIdling() && curSeq) {   // 0x0004897E: a non-idle animation still runs
        } else if (attack1) {                         // 0x000489B2
            const s32 n = static_cast<s32>(++count7B0);
            timer7A4 = animTime;
            if (d.flags170[0xE] && n % d.value168 == 0) {   // byte D+0x17E; CDQ; IDIV D+0x168
                if (SeqById(0x5D) && !IsAnimPlaying(0x5D)) {
                    PlayAnimById(0x5D, true, false, true, 0.0f);   // reload
                    const NiPoint3 p = CharacterPoint();          // vf78
                    VoiceMaxDistance(SoundAt(0x48, &p, 3, 0, nullptr), npcDef().value198);   // rand 0x0003FEFE; SetMaxDistance(D+0x198)
                }
            } else if (d.flags170[0x14]) {            // byte D+0x184
                if (!IsAnimPlaying(0x59)) PlayAnimById(0x59, true, false, true, 0.0f);
            } else if (d.attack1Count) {              // D+0xEC
                b7BC = 0;
                PlayAnimById(0x2F + n % d.attack1Count, true, false, true, 0.0f);   // CDQ; IDIV
            }
            animUpdated = 0;                          // 0x00048E04
        } else if (attack2) {                         // 0x00048A9C
            timer7A4 = animTime;
            const s32 n = static_cast<s32>(++count7B4);
            if (d.flags170[0xE] && n % d.value168 == 0) {
                if (SeqById(0x5D) && !IsAnimPlaying(0x5D)) PlayAnimation(SeqById(0x5D), true, false, true, 0.0f);   // 0x0003A130
            } else if (d.attack2Count) {              // D+0xF0
                b7BC = 0;
                // (H) attack 1's counter and count: an IDIV by zero in the original when D+0xEC is 0 (the port
                // plays nothing then)
                if (d.attack1Count)
                    PlayAnimById(0x34 + static_cast<s32>(count7B0) % d.attack1Count, true, false, true, 0.0f);
            }
            animUpdated = 0;
        } else if (b508 || b509) {                    // 0x00048B3F / 0x00048B9B: the close attacks
            const s32 cnt = b508 ? d.closeAttack1Count : d.closeAttack2Count;   // D+0xF4 / D+0xF8
            if (cnt) {
                b7BC = 0;
                const s32 k = crt::Rand() % cnt;      // rand 0x00048B61 / 0x00048BBD (CDQ; IDIV)
                PlayAnimById((b508 ? 0x3B : 0x3D) + k, true, false, true, 0.0f);
                const NiPoint3 p = GetWorldPosition();   // vf34
                VoiceMaxDistance(SoundAt(b508 ? 0xA : 0xB, &p, 3, 0, nullptr), npcDef().value198);   // the shout (Voice_SetMaxDistance D+0x198)
            }
            animUpdated = 0;                          // 0x00048E04
        } else if (b7BF) {
            PlayAnimById(0x1B, true, false, true, 0.0f);
            animUpdated = 0;
        } else if (speed > t3 && SeqById(0xD)) {      // TEST AH,0x41; JNE
            if (fwd > 0.3f && SeqById(0x17)) PlayAnimById(0x17, false, true, false, 0.0f);
            else if (fwd < -0.3f && SeqById(0x16)) PlayAnimById(0x16, false, true, false, 0.0f);   // TEST AH,5; JP
            else if (speed >= t1) {                   // TEST AH,1; JNE (NaN: the t2 test)
                if (SeqById(0x15)) {
                    PlayAnimById(0x15, false, true, false, 0.0f);
                } else {
                    PlayAnimById(0xD, false, true, false, 0.0f);
                    if (type == 0xA) hitActive = 1;
                    const f32 r = d.walkSpeed > 0.0f ? F(double(d.runSpeed) / d.walkSpeed) : 2.0f;
                    SetFrequency(SeqById(0xD), F(double(r) * EntryOf(SeqById(0xD))->speed));   // 0x0003FD20, 0x00036240
                }
            } else if (speed >= t2 || !SeqById(0x11)) {   // TEST AH,1; JE
                PlayAnimById(0xD, false, true, false, 0.0f);
                if (type == 0xA) hitActive = 1;
            } else {
                PlayAnimById(0x11, false, true, false, 0.0f);
            }
            animUpdated = 0;
        } else if (moveX > 0.2f) {                    // TEST AH,0x41; JNE
            PlayAnimById(0x1A, false, true, false, 0.0f);
            animUpdated = 0;
        } else if (moveX < -0.2f) {                   // TEST AH,5; JP
            PlayAnimById(0x19, false, true, false, 0.0f);
            animUpdated = 0;
        } else {
            IdleStep(d);
        }
    }
    AnimTick(dt);                                     // 0x0003B5E0
}

void GenericNpc::IdleStep(const GenericNPCDef& d) {   // orig: 0x00048E19..0x00049022
    for (s32 anim : {0x2F, 0x34, 0x30, 0x35, 0x31, 0x36, 0x32, 0x37, 0x33, 0x38})
        if (IsAnimPlaying(anim)) {                    // 0x00048FE6: the idle after an attack
            if (ni::NiSequence* c = SeqById(0x62)) PlayAnimation(c, false, true, false, 0.0f);   // cidle (0, 1, 0, 0)
            else PlayAnimById(0, false, true, false, 0.0f);
            idleTime = F(double(g_frameStep) + idleTime);   // 0x00049016
            return;
        }
    ni::NiSequence* c = SeqById(0x62);                // (looked up before the comparison)
    if (double(animTime) - timer7A4 < 5.0f && c) {    // FLD +0x78; FSUB +0x7A4; FCOMP [0x00293C24]; TEST AH,5; JP
        PlayAnimation(c, false, true, false, 0.0f);   // 0x00048EE4: cidle within 5 s of the last attack
        idleTime = F(double(g_frameStep) + idleTime);
        return;
    }
    bool pick = false;
    if (!IsIdling()) idleTime = 0, pick = true;       // +0x4C8
    else if (finished || !curSeq) pick = true;
    if (pick) {
        const s32 k = static_cast<s32>(idleTime) % 12;   // _ftol 0x00167110 (truncation); CDQ; IDIV 12
        if (idleTime > 30.0f && d.longIdleCount) {    // FCOMP [0x0029763C]; TEST AH,0x41; JNE
            PlayAnimById(9 + crt::Rand() % d.longIdleCount, false, true, false, 0.0f);   // rand 0x00048F6A
            idleTime = 0;
        } else if (!(idleTime >= 12.0f) || (k != 0 && d.longIdleCount)) {   // FCOMP [0x00297638]; TEST AH,1; JNE
            PlayAnimById(0, false, true, false, 0.0f);   // 0x0003CE70
        } else if (d.idleCount) {                     // D+0xD8
            const s32 r = crt::Rand() % d.idleCount;  // rand 0x00048FC1
            if (r != 0) idleTime = 0;
            PlayAnimById(r, false, true, false, 0.0f);
        }
    }
    idleTime = F(double(g_frameStep) + idleTime);
}

void GenericNpc::AfterAnimation() {                   // orig: 0x000467E0
    if (!model) return;
    const GenericNPCDef& dd = npcDef();
    const double half = double(updateDistance) * 0.5f;   // FLD +0x64; FMUL [0x00293AA0]
    if (!IsIdling() || half < cameraDistance) StopVoiceAndClear(voice874);   // TEST AH,5; JP
    if (!WalkRunPlaying() || !(half > cameraDistance)) StopVoiceAndClear(voice878);   // vf9C; TEST AH,0x41; JE
    if (half < cameraDistance) {                      // TEST AH,5; JP
        if (level.Sound().VoiceExists(static_cast<s32>(voice87C))) {   // 0x000468B2 (no null test)
            level.Sound().StopVoice(static_cast<s32>(voice87C));
            voice87C = 0;
        }
        return;
    }
    // ---- near (0x000468DD) ----
    if (IsIdling() && !VoiceLive(voice874)) {         // vfA0; `!v || !Voice_Exists(v)`
        voice874 = static_cast<u32>(SoundAt(0x14, nullptr, 6, 0, node.Get()));   // 0x0003FFC0 -> 0x0003FEA0
        VoiceMaxDistance(static_cast<s32>(voice874), dd.value19C);   // 0x00046924
    }
    if (WalkRunPlaying() && !VoiceLive(voice878)) {   // vf9C
        voice878 = static_cast<u32>(SoundAt(0x15, nullptr, 6, 0, node.Get()));
        VoiceMaxDistance(static_cast<s32>(voice878), dd.value19C);   // 0x00046973
    }
    const double x = double(g_frameStep) + timer89C;  // FLD [step]; FADD [+0x89C]; FST
    timer89C = F(x);
    if (x > f8A0) {                                   // FCOMP [+0x8A0]; TEST AH,0x41; JNE
        const NiPoint3 p = GetWorldPosition();        // vf34
        VoiceMaxDistance(SoundAt(0x10, &p, 3, 0, nullptr), dd.value19C);   // 0x0003FEA0 (rand pick); 0x000469C5
        timer89C = 0;
        const GenericNPCDef& d = npcDef();
        f8A0 = F(double(d.value154) - d.value150);    // FLD; FSUB; FSTP
        const s32 q = crt::Rand() % 100;              // orig: rand 0x000469E8 (CDQ; IDIV 0x64)
        f8A0 = F((double(q) * double(0.01f)) * f8A0 + d.value150);   // FILD; FMUL [0x00293AAC]; FMUL; FADD; FSTP
    }
    const phys::CharShape& sh = body->shape;          // body+0x40
    if (!sh.partnerIsBox && sh.partner) {             // byte +0xB2 == 0 && +0xB4 != 0
        if (b898) return;
        b898 = 1;
        if (VoiceLive(voice890)) return;              // 0x00046A46
        const NiPoint3 p = GetWorldPosition();
        voice890 = static_cast<u32>(SoundAt(0x11, &p, 3, 0, nullptr));
        VoiceMaxDistance(static_cast<s32>(voice890), dd.value198);   // 0x00046A82
        return;
    }
    b898 = 0;
}

bool GenericNpc::PostUpdate() {                       // orig: 0x000478A0
    if (!level.camera) return true;
    if (!g_decomposeCorpses) deathFade = 0;           // orig: byte 0x002D6214 (0x000478CB)
    const bool fadeType = type == 0 || type == 0xC || type == 7 || type == 0xA;
    if (cameraDistance < updateDistance) {            // TEST AH,5; JP
        if (IsDead() && IsDyingAnim() && !(deathFade < 1.0f) && fadeType) {
            // 0x0004794A: the faded corpse's drops. (+0x8A4, a definition spawned at the position + 1.0 with the
            // label +0x8A8, is only written 0 by the ctor 0x00047E07: that branch never runs.)
            if (drops) {                              // +0x8AC
                const ni::NiMatrix3 r = GetWorldRotation();   // vf38 first
                const NiPoint3 p = GetWorldPosition();        // then vf34
                drops->SpawnAll(level, owner, r, p);  // 0x00075E60(+4, &rot, &pos)
            }
            return false;
        }
    } else if (!(deathFade < 1.0f) && IsDead() && fadeType) {
        return false;
    }
    PrepareRender();                                  // 0x00039890 (returns 1)
    return true;
}

// ---- Nibbler ----------------------------------------------------------------------------------------------------
Nibbler::Nibbler(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x0008B1D0
    if (crt::StrICmp(d.name.c_str(), "nibbler_4") == 0 || crt::StrICmp(lvl.name.c_str(), "level4-1") == 0) {
        mode = 1;                                     // orig: 0x0008B2C0
        body->SetEnabled(false);                      // vf34(0) 0x000AED40
        Character::SetPosition({pos.x, F(double(pos.y) - 4.5f), pos.z});   // 0x002947A0; the ctor's pos argument
    } else if (crt::StrICmp(lvl.name.c_str(), "level1-6") == 0) {
        mode = 2;
        body->b1A = 1;
    } else {
        mode = crt::StrICmp(lvl.name.c_str(), "level2-6") == 0 ? 3 : 0;
    }
}

void Nibbler::BuildModel() {                          // orig: 0x0008B330
    const bool first = !model;
    GenericNpc::BuildModel();
    if (!model || !first) return;
    ni::NiAVObject* mesh = GetObjectByName(node.Get(), "Nibbler_MESH");   // (no null check in the original)
    mesh->flags &= ~ni::kNiAVHidden;
    body->shape.ext.y = cdef.boundHeight;             // shape+0x94 = D+0x94
    model->local.translate = {0, 0, 0};
    if (auto* alpha = dynamic_cast<ni::NiAlphaProperty*>(GetProperty(*mesh, 0)))
        alpha->flags = static_cast<u16>(alpha->flags & ~1u);
}

void Nibbler::UpdateAnimation(f32 dt) {               // orig: 0x0008B650
    if (!model) return;
    if (mode == 1 && !b8C4) {                         // orig: 0x0008B697
        const NiPoint3 me = GetWorldPosition();       // vf34
        const f32 y3 = F(double(me.y) + 3.0f);        // 0x0029563C
        const NiPoint3 pp = level.player->GetWorldPosition();   // S+0x94 (no null test in the original)
        const double dx = double(pp.x) - me.x, dy = double(pp.y) - y3, dz = double(pp.z) - me.z;
        if ((dx * dx + dy * dy) + dz * dz < 36.0f) {  // 0x0029F57C, TEST AH,5; JP
            b8C4 = 1;                                 // orig: 0x0008B71B (object_gaps.md 10.1)
            (void)SoundAt(0x79, nullptr, 3, 0, nullptr);   // def 0x0003FEA0
            SpawnBoom();
            VoiceMaxDistance(SoundAt(0, nullptr, 3, 0, node.Get()), npcDef().value198);   // 0x0003FFC0(0, node, 3, 0); Voice_SetMaxDistance(D+0x198)
        }
    }
    if (b8C4) FreedStep(); // orig: 0x0008B7D7
    if (animUpdated) PlayAnimById(0, false, true, false, 0.0f);   // 0x0008BE32
    AnimTick(dt);
}

void Nibbler::SpawnBoom() {                           // orig: 0x0008B733 / 0x0008B845 (object_gaps.md 10.1)
    if (!level.defs || !level.defs->Find("Nibbler_Boom")) return;   // Session::FindDef 0x000C3F10 (0x0029F56C)
    const NiPoint3 p = GetWorldPosition();            // vf34
    const ni::NiMatrix3 R = GetWorldRotation();       // vf38
    // owner: S+0x5C ? S+0x5C + 4 : 0; FindDef again; vf14(owner, &p, &R, "bah" 0x0029F568)
    GameObject* o = level.defs->Find("Nibbler_Boom")->Spawn(level, level.currentStage, p, R, "bah");
    if (o) SetNodeScale(*o, 1.0f);                    // 0x00073720(1.0)
}

void Nibbler::FreedStep() {                           // orig: 0x0008B7D7..0x0008BE30 (object_gaps.md 10.2)
    const GenericNPCDef& d = npcDef();
    if (!IsAnimPlaying(0x4D) && !IsAnimPlaying(0x4E) && !IsAnimPlaying(0x4F) && !IsPlayingName("teleport")) {
        if (!IsAnimPlaying(0x47)) {                   // the cage bursts
            PlayAnimById(0x47, true, false, true, 0.0f);
            animUpdated = 0;
            SpawnBoom();
            VoiceMaxDistance(SoundAt(0, nullptr, 3, 0, node.Get()), npcDef().value198);   // 0x0003FFC0(0, node, 3, 0); Voice_SetMaxDistance(D+0x198)
            return;
        }
        animUpdated = 0;                              // (0x0008B8D3: IsAnimPlaying(0x47) again, always true here)
        if (!finished) return;                        // byte +0x9B
        if (!(animStart > 0.0f)) return;              // FLD +0x80; FCOMP [0x00293AB8]; TEST AH,0x41; JNE
        const s32 r = crt::Rand();                    // orig: rand 0x0008B911 (CDQ; IDIV D+0xE0)
        if (d.dyingCount == 0) return;                // (IDIV by zero in the original: UB guard)
        const s32 k = r % d.dyingCount + 0x4D;
        if (!SeqById(k)) return;                      // 0x00033E00
        PlayAnimById(k, false, true, true, 0.0f);     // (push 0, 1, 1, 0, k)
        voice8CC = static_cast<u32>(SoundAt(0x16, nullptr, 4, 0, node.Get()));   // 0x0003FFC0(0x16, node, 4, 0)
        VoiceMaxDistance(static_cast<s32>(voice8CC), d.value198);   // 0x0008B965
        return;
    }
    animUpdated = 0;                                  // 0x0008B972
    if (std::bit_cast<u32>(f8C8) == 0xBF800000u) {    // CMP +0x8C8, -1.0f (the bits)
        if (mode == 0) {
            const NiPoint3 me = GetWorldPosition();   // vf34 (this first)
            const NiPoint3 pp = level.player->GetWorldPosition();   // S+0x94 (no null test in the original)
            const double dz = double(pp.z) - me.z, dy = double(pp.y) - me.y, dx = double(pp.x) - me.x;
            if (!((dx * dx + dy * dy) + dz * dz < 81.0f)) return;   // [0x0029F558]; TEST AH,5; JP
        }
        StopVoiceAndClear(voice8CC);                  // 0x0008B9FD
        animUpdated = 0;
        ++level.nibblersPickedUp;                     // S+0x18C
        if (Player* pl = level.player) {              // S+0x94 (no null test): (P->def +0x28)->0x000F28F0(8, &arg)
            const IngameEventArg ev{this, nullptr};   // 0x000984D0, 0x000D8150(this); 0x000F0C00 clears it
            pl->cdef.ingameEvents.Trigger(8, ev, pl->IngameContext());
        }
        level.nibblerPending.push_back(id);           // (S+0x1FC)->0x00077CC0(+0x20): the pending id (SetAtGrow)
        f8C8 = 0;
        if (mode != 1) (void)SoundAt(0x79, nullptr, 3, 0, nullptr);   // def 0x0003FEA0
        if (Def* td = level.defs ? level.defs->Find("Nibbler_Teleport") : nullptr) {   // 0x0029F544
            const NiPoint3 p = GetWorldPosition();    // vf34
            const NiPoint3 dir = ni::MulVector(GetWorldRotation(), {1.0f, 0.0f, 0.0f});   // vf38 * (1, 0, 0) [0x002D77E0]
            if (auto* idef = dynamic_cast<ImpactDef*>(td))   // 0x0004EE90 (called without a type test)
                (void)idef->SpawnUp(level, &inventory, p, dir, nullptr);   // owner: this+0x38
        }
        PlayAnimation(FindSequence("teleport"), true, false, true, 0.0f);   // 0x00039BA0 (0x0029F55C); 0x0003A130
        return;
    }
    f8C8 = F(double(g_frameStep) + f8C8);             // 0x0008BB22: FLD [0x002D67E8]; FADD +0x8C8; FSTP
    const double len = SequenceDuration(FindSequence("teleport"));   // 0x00036210
    if (len < f8C8 || len == f8C8) {                  // FCOMP +0x8C8; TEST AH,0x41; JP
        StopVoiceAndClear(voice8CC);                  // 0x0008BB61
        f8C8 = F(SequenceDuration(FindSequence("teleport")));
        GetObjectByName(node.Get(), "Nibbler_MESH")->flags |= ni::kNiAVHidden;   // vf54 (0x0029F51C; no null test)
        return;
    }
    if (VoiceLive(voice8CC)) level.Sound().FadeOut(static_cast<s32>(voice8CC), 0.2f);   // 0x0008BBD7 (0x3E4CCCCD)
    ni::NiAVObject* mesh = GetObjectByName(node.Get(), "Nibbler_MESH");   // (no null test in the original)
    ni::NiPointer<ni::NiAmbientLight> light;          // vf54("NIBBLERLIGHT" 0x0029F534), DynamicCast 0x002F9610
    if (ni::NiAVObject* l = GetObjectByName(mesh, "NIBBLERLIGHT")) light = dynamic_cast<ni::NiAmbientLight*>(l);
    ni::NiPointer<ni::NiAlphaProperty> alpha;         // GetProperty(0) 0x00101DA0, DynamicCast 0x002F91F0
    if (ni::NiProperty* pr = GetProperty(*mesh, 0)) alpha = dynamic_cast<ni::NiAlphaProperty*>(pr);
    if (!alpha) {
        alpha = new ni::NiAlphaProperty;              // new(0x1C) 0x00013D80
        alpha->flags = 0xEC;
        alpha->alphaTestRef = 0;
        mesh->properties.insert(mesh->properties.begin(), ni::NiPointer<ni::NiProperty>(alpha.Get()));   // 0x00013C40
        mesh->UpdateProperties();                     // 0x0008BCC2
    }
    if (!light) {
        light = new ni::NiAmbientLight;               // new(0x110) 0x0010EFC0
        light->name = "NIBBLERLIGHT";                 // NiObjectNET::SetName 0x000FE300
        light->ambient = {1.0f, 1.0f, 1.0f};          // 0x00034490 (stores +0xE4..+0xEC, ++revision)
        ++light->revision;
        if (auto* mn = dynamic_cast<ni::NiNode*>(mesh)) mn->AttachChild(light.Get(), true);   // vfA0(light, 1)
        mesh->UpdateEffects();                        // 0x0008BD54
    }
    alpha->flags |= 1;                                // OR byte +0x14, 1 (blending on)
    alpha->flags = static_cast<u16>(alpha->flags & 0xFFE1u);   // AND byte +0x14, 0xE1 (source factor 0)
    alpha->flags = static_cast<u16>(alpha->flags & 0xFE1Fu);   // AND word +0x14, 0xFE1F (destination factor 0)
    alpha->flags |= 0x2000;                           // OR byte +0x15, 0x20
    const f32 rest = F(2.0f - double(f8C8));          // FLD [0x00293B20]; FSUB +0x8C8; FSTP
    const double ratio = double(rest) / SequenceDuration(FindSequence("teleport"));   // FDIVR
    if (ratio < light->dimmer) {                      // FCOM +0xE0; TEST AH,5; JP
        if (light->dimmer > 0.0f) {                   // TEST AH,0x41; JNE
            light->dimmer = F(ratio);
            ++light->revision;                        // +0xCC
        }
        if (light->dimmer < 0.0f) {                   // TEST AH,5; JP
            light->dimmer = 0.0f;
            ++light->revision;
        }
    }
}

void Nibbler::AfterAnimation() {                      // orig: 0x0008B450
    if (IsAnimPlaying(0)) {
        GenericNpc::AfterAnimation();                 // jmp 0x000467E0
        return;
    }
    StopVoiceAndClear(voice874);                      // 0x0008B471
}

void Nibbler::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x0008B4D0
    (void)vel, (void)rot;
    if (!model) return;
    if (mode == 1) {
        if (ni::NiAVObject* pl = GetObjectByName(model.Get(), "Plinth")) pl->flags |= ni::kNiAVHidden;   // vf54
        body->SetEnabled(false);                      // vf34(0), every step
        return;
    }
    if (mode == 2) {
        body->b1A = 1;
        return;
    }
    if (!(f8C8 >= 1.0f)) return;                      // FCOMP [0x00293A98]; TEST AH,1; JNE (< and NaN return)
    body->shape.ext.y = cdef.boundHeight;             // 0x0008B53B: (body+0x40)+0x94 = D+0x94
    if (!b8C5) {
        b8C5 = 1;
        NiPoint3 p = body->GetPosition();             // vf08
        p.y = F(double(p.y) - 2.0f);                  // FSUB [0x00293B20]
        body->SetPosition(p);                         // vf04
    }
    model->local.translate = {0.0f, 2.0f, 0.0f};      // 0x00034210(0, 2.0, 0): model +0x60
}

void Nibbler::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x0008B4A0 (npc_combat2_port.md 3.3)
    (void)amount;                                     // (no Character::Damage: a nibbler cannot be hurt)
    if (level.IsCharacter(attacker) && attacker->def->IsType(2)) b8C4 = 1;   // 0x00033EE0; vf0C(2): a player frees it
}

// ---- Owl --------------------------------------------------------------------------------------------------------
void Owl::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x0008BF30 (vel is ignored)
    (void)vel;
    const NiPoint3 z{};                               // local copy of 0x002F915C
    body->SetForce(z);                                // vf10 0x000AEC70
    NiPoint3& v = body->velocity;                     // vf50 AddVelocity 0x000AF7D0 (a -0.0 becomes +0.0)
    v = {F(double(v.x) + z.x), F(double(z.y) + v.y), F(double(z.z) + v.z)};
    SetRotation(rot);                                 // vf24 0x00033D20
}

void Owl::UpdateAnimation(f32 dt) {                   // orig: 0x0008BFC0
    if (!model) return;
    if (IsDead()) {                                   // vfA8 (health 55555 in level1-1)
        if (!IsAnimPlaying(0x4D)) {                   // orig: 0x0008BFF8 (0x00033E20(0x4D))
            const NiPoint3 p = GetWorldPosition();    // vf34 (esp+0x14)
            const NiPoint3 dir = ni::MulVector(GetWorldRotation(), {1.0f, 0.0f, 0.0f});   // vf38 * [0x002D77E0]
            PlayAnimById(0x4D, false, true, false, 0.0f);   // 0x0003CE70 (pushes 0,0,1,0,0x4D): dying1
            // Session::FindDef("RainFeathers" 0x0029F6AC); then vf20(&p, &dir). (L) U: no level's EffectList.txt
            // and no script names RainFeathers (effect library case 83, 0x0006F820), so FindDef returns 0 in every
            // level (checked over every effects file and script of the disc, 2026-10-19).
            if (level.defs && level.defs->Find("RainFeathers")) {
                (void)p, (void)dir;
                NotPorted("Owl::UpdateAnimation: the RainFeathers definition's vf20 (case 83, 0x0006F820)");
            }
        }
        animUpdated = 0;                              // byte +0x9F
        GenericNpc::UpdateAnimation(dt);
        return;
    }
    const s32 n = static_cast<s32>(idleTime);         // _ftol 0x00167110 (truncation), before the add
    const double x = double(g_frameStep) + idleTime;  // FLD [step]; FADD [+0x4C8]; FST
    idleTime = F(x);
    const s32 r = n % 3;                              // CDQ; IDIV 3
    if (finished) {                                   // byte +0x9B
        if (x > 3.0f && r == 0) {                     // FCOMP [0x0029563C] (unrounded x); TEST AH,0x41; JNE
            const s32 k = crt::Rand() % (npcDef().idleCount - 1) + 1;   // orig: rand 0x0008C0B4 (CDQ; IDIV; INC)
            PlayAnimById(k, true, false, false, 0.0f);   // 0x0003CE70 (pushes 0,0,0,1,k)
            idleTime = 0;
            GenericNpc::UpdateAnimation(dt);
            return;
        }
        PlayAnimById(0, true, false, false, 0.0f);    // the idle again
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

// ---- SewerTentacle ------------------------------------------------------------------------------------------------
SewerTentacle::SewerTentacle(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x0008C340
    SetScale(1.0f);                                   // 0x00035080
    const ni::NiMatrix3& R = rotation;                // vf38 0x00033D40
    const double x = (0.0 * R.m[0][1] + 0.0 * R.m[0][2]) + 1.0 * R.m[0][0];   // the inline R * (1, 0, 0)
    const double y = (0.0 * R.m[1][1] + 0.0 * R.m[1][2]) + 1.0 * R.m[1][0];
    const f32 z = F((0.0 * R.m[2][1] + 0.0 * R.m[2][2]) + 1.0 * R.m[2][0]);
    const NiPoint3 dd{F(x * 4.5f), F(y * 4.5f), F(double(z) * 4.5f)};   // [0x002947A0]
    const NiPoint3 p = GetWorldPosition();            // 0x00035440
    Character::SetPosition({F(double(p.x) - dd.x), F(double(p.y) - dd.y), F(double(p.z) - dd.z)});
    if (lvl.loadSound) retractSound = lvl.loadSound("sounds/level1/npc/sewertentacle/retract.wav", 0);   // 0x000C9860
}

void SewerTentacle::UnloadModel() {                   // orig: 0x0008C300
    StopVoiceIfExists(voice8CC, true);                // (Voice_Exists(+0x8CC) without a null test; Voice_Stop, 0)
}

void SewerTentacle::Think(f32 dt) {                   // orig: 0x0008CA90
    (void)dt;
    body->SetForce({});
    body->b1A = 1;
    body->integratePinned = 1;                        // +0x1B
    type = 7;
    if (!model) return;
    b7BE = 0;
    if (flag28) return;
    if (cameraDistance > updateDistance) return;      // FCOMP; TEST AH,0x41; JE
    body->SetEnabled(!(IsAnimPlaying(0x47) || IsAnimPlaying(0xF4)));   // vf34 0x000AED40
}

void SewerTentacle::UpdateAnimation(f32 dt) {         // orig: 0x0008C580 (4.1.2; object_gaps.md 16.2)
    if (!model) return;
    b508 = 0;                                         // byte +0x508 (the attack request)
    model->local.translate = {4.5f, 0.0f, 0.0f};      // model+0x60
    bool hurt = false;
    if (!(health4F8 == health)) {                     // TEST AH,0x44; JNP
        hurt = health < health4F8;                    // TEST AH,5; JP
        f7A8 = 0;
        health = maxHealth;
    }
    health4F8 = health;
    if (IsAnimPlaying(0)) {
        Player* P = level.player;
        if (P && !P->IsDead() && !P->IsHeldBy(nullptr)) {   // 0x000503B0(0): held by anything
            const NiPoint3 c = CharacterPoint();      // vf78
            const NiPoint3 pp = P->CharacterPoint();  // vf78 = 0x00054A50 (4.8)
            NiPoint3 dv{F(double(pp.x) - c.x), F(double(pp.y) - c.y), F(double(pp.z) - c.z)};   // 0x00011530
            const NiPoint3 X = ni::MulVector(rotation, {1, 0, 0});
            const f32 dist = F(ni::Unitize(dv));
            const double dot = (double(X.z) * dv.z + double(X.y) * dv.y) + double(X.x) * dv.x;
            // the cooldown: FCOMP [0x00293AB8]; TEST AH,0x41; JP (only < or ==: NaN does not attack)
            if (dist < 12.0f && (dot > 0.5f || dot < -0.5f || dist < std::bit_cast<f32>(0x40A6F4DFu)) &&
                (cooldown < 0.0f || cooldown == 0.0f)) {
                b508 = 1;                             // the attack request (+0x508)
                cooldown = 1.0f;
            }
        }
        const double x = double(cooldown) - g_frameStep;   // FLD; FSUB; FST
        cooldown = F(x);
        if (x < 0.0f) cooldown = 0;                   // TEST AH,5; JP
    }
    if (holding) {                                    // 0x0008C75E (+0x8C1: "morph:pull_through_pipe")
        Player* P = level.player;
        if (P && P->IsHeldBy(this)) {
            P->Damage(P->maxHealth, this);            // vf7C(P+0x4FC, this)
            P->deathKind = 2;                         // +0x778
            P->GetNode()->flags |= ni::kNiAVHidden;   // vf30()->+0xB0 |= 1
        }
        idleTime = 0;                                 // +0x4C8
        animUpdated = 0;                              // +0x9F
        if (finished && !IsAnimPlaying(0x2F)) PlayAnimById(0xF4, true, false, true, 0.0f);   // retract
    } else if (animUpdated) {                         // byte +0x9F
        if ((IsAnimPlaying(0x2F) && finished) || IsAnimPlaying(0xF4)) {   // 0x0008C815: retract, then idle after 5 s
            if (!IsAnimPlaying(0xF4)) PlayAnimById(0xF4, true, false, true, 0.0f);
            const double x = double(g_frameStep) + idleTime;   // FLD step; FADD +0x4C8; FST
            idleTime = F(x);
            if (x > 5.0f) PlayAnimById(0, false, true, true, 0.0f);   // FCOMP [0x00293C24]; TEST AH,0x41; JNE
            animUpdated = 0;
        } else if (!finished && (IsAnimPlaying(0x2F) || IsAnimPlaying(0x30) || IsAnimPlaying(0x31))) {
            if (attacking) {                          // 0x0008C8AE: "morph:grab" missed
                if (IsAnimPlaying(0x2F)) PlayAnimById(0xF2, true, false, true, 0.0f);
                if (IsAnimPlaying(0x30)) PlayAnimById(0xF2, true, false, true, 0.0f);
                if (IsAnimPlaying(0x31)) PlayAnimById(0xF3, true, false, true, 0.0f);
                animUpdated = 0;
            }                                         // (else: tick only)
        } else if (hurt) {                            // 0x0008C91C
            PlayAnimById(0x47, true, false, true, 0.0f);   // "hit"
            animUpdated = 0;
        } else if (!finished && !IsIdling() && curSeq) {   // a non-idle animation still runs
        } else if (b508) {                            // 0x0008C964: the attack
            idleTime = 0;
            if (!IsAnimPlaying(0x2F) && !IsAnimPlaying(0x30) && !IsAnimPlaying(0x31)) {
                const s32 r = crt::Rand() % 10;       // orig: rand 0x0008C99D (CDQ; IDIV 10)
                count7B0 = r < 1 ? 0u : r < 7 ? 1u : 2u;   // +0x7B0 (JGE 1 / JGE 7)
                const s32 n = cdef.attack1Count;      // D+0xEC (IDIV without a zero test: UB guard)
                if (n != 0) PlayAnimById(static_cast<s32>(count7B0) % n + 0x2F, true, false, true, 0.0f);
                const NiPoint3 p = GetWorldPosition();   // vf34
                VoiceMaxDistance(SoundAt(0x35, &p, 3, 0, nullptr), npcDef().value198);   // (+ Voice_SetMaxDistance(D+0x198))
            }
            animUpdated = 0;
        } else {                                      // 0x0008CA2D
            if (!IsIdling()) {
                idleTime = 0;                         // +0x4C8
                PlayAnimById(0, false, true, false, 0.0f);
            }
            idleTime = F(double(g_frameStep) + idleTime);
        }
    }
    attacking = 0;                                    // byte +0x8C0
    AnimTick(dt);
}

// orig: vfE8 0x0008CB60 (thiscall(keyTime, text), RET 8; object_gaps.md 16.2): the grab and the hits of the held player.
void SewerTentacle::OnTextKey(f32 keyTime, const char* text) {
    (void)keyTime;
    Player* P = level.player;                         // S+0x94
    Camera* cam = level.camera.get();                 // S+0x8C
    if (crt::StrICmp(text, "morph:grab") == 0) {      // 0x0029F994
        if (!P) return;
        const NiPoint3 c = CharacterPoint();          // vf78 (this first)
        const NiPoint3 pp = P->CharacterPoint();
        NiPoint3 d{F(double(pp.x) - c.x), F(double(pp.y) - c.y), F(double(pp.z) - c.z)};
        const NiPoint3 X = ni::MulVector(rotation, {1, 0, 0});   // vf38, 0x002D77E0
        const f32 dist = F(ni::Unitize(d));
        const double dot = (double(X.z) * d.z + double(X.y) * d.y) + double(X.x) * d.x;
        // FCOMP 12 (TEST AH,5; JP) / FCOM 0.6 (TEST AH,0x41; JE) / FCOMP -0.6 (TEST AH,5; JNP) / FCOMP 4.8 (AH,5; JP)
        bool reach = dist < 12.0f && (dot > 0.6f || dot < -0.6f || dist < 4.8f);
        if (reach) {
            const f32 topY = P->TopPoint().y;         // 0x000345B0
            const f32 h = F(double(topY) - P->GetWorldPosition().y);   // vf34
            const f32 py = P->GetWorldPosition().y;
            reach = std::fabs(double(CharacterPoint().y) - py) < h &&   // vf78; FSUB; FABS; FCOMP; TEST AH,5; JP
                    !P->IsDead() && !P->IsHeldBy(nullptr);   // vfA8; 0x000503B0(0)
        }
        if (!reach) {                                 // 0x0008CD85: the miss (UpdateAnimation's follow-ups)
            attacking = 1;                            // +0x8C0
            return;
        }
        ni::NiAVObject* t = GetObjectByName(node.Get(), "tip");   // vf54 0x00295D98 (twice)
        P->Grab(this, t ? dynamic_cast<ni::NiNode*>(t) : nullptr);   // DynamicCast NiNode 0x002F9230; 0x00055370
        if (!cam) return;
        const NiPoint3 at = cam->position;            // C+0x48 (a copy)
        TriggerRec* rec = level.triggers ? level.triggers->FindOrCreate(label ? label->c_str() : nullptr) : nullptr;
        cam->SetStaticTarget(&at, rec, std::bit_cast<f32>(0x3F7AE148u));   // S+0xC0 0x000DB390(+0x34); 0x00014550
        cam->SetShake(0.5f, 2.0f);                    // 0x00011290
        return;
    }
    if (crt::StrICmp(text, "morph:hit") == 0) {       // 0x002974A8: the player not held
        if (!P) return;
        const NiPoint3 c = GetWorldPosition();        // vf34 (this first)
        const NiPoint3 pp = P->GetWorldPosition();
        NiPoint3 d{F(double(pp.x) - c.x), F(double(pp.y) - c.y), F(double(pp.z) - c.z)};
        const NiPoint3 X = ni::MulVector(rotation, {1, 0, 0});
        const f32 dist = F(ni::Unitize(d));
        const double dot = (double(X.z) * d.z + double(X.y) * d.y) + double(X.x) * d.x;
        if (!(dist < 12.0f)) return;                  // [0x00297638]
        if (!(dot > 0.75f || dot < -0.75f || dist < 6.0f)) return;   // [0x00293A94] / [0x0029A514] / [0x0029C094]
        if (P->IsDead() || P->IsHeldBy(nullptr)) return;
        const NiPoint3 f = ni::MulVector(P->rotation, {1, 0, 0});   // P vf38
        const NiPoint3 g = ni::MulVector(rotation, {0, 0, 1});      // vf38, 0x002D77F8
        const f32 facing = F((double(f.z) * g.z + double(f.y) * g.y) + double(f.x) * g.x);
        const AnimEntry* e = curSeq ? EntryOf(curSeq) : nullptr;    // 0x0003FD20 (no null test: UB guard)
        if (!e) return;
        const f32 dmg = F(e->hasDamage ? e->damage : 0.0f);         // AnimEntry::GetDamage 0x0003EE90
        P->TentacleHit(dmg, this, facing < 0.0f);                   // 0x00050360 (TEST AH,5; JP: only <)
        const NiPoint3 v = ni::MulVector(rotation, {0, 0, 1});
        const NiPoint3 u = facing > 0.0f ? v          // TEST AH,0x41; JNE
                                         : NiPoint3{F(double(v.x) * -1.0f), F(double(v.y) * -1.0f), F(double(v.z) * -1.0f)};
        const NiPoint3 up{0.0f, 1.0f, 0.0f};          // 0x002D77EC
        const NiPoint3 cr = ni::Cross(u, up);         // 0x00011700 (ECX = u)
        ni::NiMatrix3 M;                              // column 0 = u, column 1 = up, column 2 = cr
        const f32 ur[3] = {u.x, u.y, u.z}, yr[3] = {up.x, up.y, up.z}, crr[3] = {cr.x, cr.y, cr.z};
        for (int r = 0; r < 3; ++r) M.m[r][0] = ur[r], M.m[r][1] = yr[r], M.m[r][2] = crr[r];
        P->SetRotation(M);                            // vf24
        return;
    }
    if (crt::StrICmp(text, "morph:punch") == 0) {     // 0x0029F980
        if (!P || !P->IsHeldBy(this)) return;
        if (cam) cam->SetShake(0.5f, std::bit_cast<f32>(0x3ECCCCCDu));   // (0.5, 0.4)
        P->Damage(2.0f, this);                        // vf7C(2.0, this)
        return;
    }
    if (crt::StrICmp(text, "morph:hit_head") == 0) {  // 0x0029F970
        if (!P || !P->IsHeldBy(this)) return;
        P->Damage(2.0f, this);
        if (cam) cam->SetShake(std::bit_cast<f32>(0x3F666666u), std::bit_cast<f32>(0x3F4CCCCDu));   // (0.9, 0.8)
        const NiPoint3 p = GetWorldPosition();        // vf34
        VoiceMaxDistance(SoundAt(0x35, &p, 3, 0, nullptr), npcDef().value198);   // (+ Voice_SetMaxDistance(D+0x198))
        return;
    }
    if (crt::StrICmp(text, "morph:release") == 0) {   // 0x0029F960
        if (P && P->IsHeldBy(this)) P->Release();     // 0x00055880
        return;
    }
    if (crt::StrICmp(text, "morph:pull_through_pipe") == 0) {   // 0x0029F948
        if (P && P->IsHeldBy(this)) holding = 1;      // +0x8C1
    }
}

// ---- CarnivorousPlant (object_gaps.md 16.4) ------------------------------------------------------------------------
void CarnivorousPlant::UpdateAnimation(f32 dt) {      // orig: vf5C 0x00081880
    if (level.player && level.player->IsDead()) {     // S+0x94 vfA8
        attack1 = 0;                                  // +0x506
        b508 = 0;                                     // +0x508
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

void CarnivorousPlant::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 0x000818C0 (thiscall, RET 8)
    GenericNpc::OnTextKey(keyTime, text);             // 0x00047550 first
    Player* P = level.player;                         // S+0x94
    if (crt::StrICmp(text, "morph:chomp") == 0) {     // 0x0029DCDC
        if (P && P->IsHeldBy(this)) {                 // 0x000503B0(this)
            const NiPoint3 p = GetWorldPosition();    // vf34
            VoiceMaxDistance(SoundAt(0x7D, &p, 3, 0, nullptr), npcDef().value198);   // (+ Voice_SetMaxDistance(D+0x198))
        }
        return;
    }
    if (crt::StrICmp(text, "morph:move_begin") == 0) {   // 0x0029DCC8: the bite
        if (!P || P->IsDead()) return;
        const NiPoint3 me = GetWorldPosition();       // vf34 (this first)
        const NiPoint3 pp = P->GetWorldPosition();
        NiPoint3 d{F(double(pp.x) - me.x), F(double(pp.y) - me.y), F(double(pp.z) - me.z)};
        if (!(ni::Unitize(d) < 5.0f)) return;         // FCOMP [0x00293C24]; TEST AH,5; JP
        if (P->IsHeldBy(nullptr)) return;             // 0x000503B0(0)
        ni::NiAVObject* t = GetObjectByName(node.Get(), "Bender_PIN");   // vf54 0x0029DCBC (twice)
        P->Grab(this, t ? dynamic_cast<ni::NiNode*>(t) : nullptr);      // DynamicCast NiNode 0x002F9230; 0x00055370
        return;
    }
    if (crt::StrICmp(text, "morph:switch_to_small") == 0) {   // 0x0029DCA4
        if (P && P->IsHeldBy(this)) P->HeldHurt();    // 0x00050390
        return;
    }
    if (crt::StrICmp(text, "morph:switch_to_normal") == 0) {  // 0x0029DC8C: the spit
        if (!P || !P->IsHeldBy(this)) return;
        P->Release();                                 // 0x00055880
        if (level.camera) level.camera->SetShake(std::bit_cast<f32>(0x3F4CCCCDu), std::bit_cast<f32>(0x3F4CCCCDu));
        const NiPoint3 p = GetWorldPosition();        // vf34
        VoiceMaxDistance(SoundAt(0x7E, &p, 3, 0, nullptr), npcDef().value198);   // (+ Voice_SetMaxDistance(D+0x198))
    }
}

// ---- EyeStalk -----------------------------------------------------------------------------------------------------
EyeStalk::EyeStalk(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00082FA0
    const s32 r = crt::Rand();                        // orig: rand 0x00082FDC
    showTimer = F(double(r % 100) * double(0.1f) + 10.0f);   // FILD; FMUL m32; FADD m32; FSTP
}

void EyeStalk::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x00083240 (vel ignored)
    (void)vel;
    const bool hidden = hideTimer > 0.0f;             // TEST AH,0x41; JNE
    body->b1A = hidden ? 1 : 0;
    body->SetEnabled(!hidden);                        // vf34
    SetRotation(rot);                                 // vf24
}

void EyeStalk::UpdateAnimation(f32 dt) {              // orig: 0x000830B0
    if (!model) {
        hideTimer = 5.0f;
        return;
    }
    if (!IsDead()) {
        if (hideTimer > 0.0f) {                       // TEST AH,0x41; JNE
            if (!IsPlayingName("disappear") || finished) {   // 0x00039D60
                hideTimer = F(double(hideTimer) - g_frameStep);
                model->flags |= ni::kNiAVHidden;      // +0xB0 |= 1
                return;                               // (no AnimTick)
            }
            animUpdated = 0;                          // 0x00083206
        } else {                                      // [NOT REACHED in the 120 frames]
            if (model->flags & ni::kNiAVHidden) {
                const s32 r = crt::Rand() % 50;       // orig: rand 0x00083146
                hideTimer = 0;
                showTimer = F(double(r) * double(0.1f) + 10.0f);
                model->flags &= ~ni::kNiAVHidden;
                PlayAnimation(FindSequence("appear"), true, false, true, 0.0f);
                animUpdated = 0;
            }
            const double x = double(showTimer) - g_frameStep;   // FST
            showTimer = F(x);
            if (!(x > 0.0f)) {                        // TEST AH,0x41; JP
                PlayAnimation(FindSequence("disappear"), true, false, true, 0.0f);
                showTimer = 0;
                hideTimer = F(double(crt::Rand() % 50) * double(0.1f) + 5.0f);   // orig: rand 0x000831DF
                animUpdated = 0;                      // 0x00083206
            }
        }
    }
    if (animUpdated) GenericNpc::UpdateAnimation(dt); // 0x0008320C
    else AnimTick(dt);
}


// ---- Weasel -------------------------------------------------------------------------------------------------------
Weasel::Weasel(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00092F90
    SetScale(std::bit_cast<f32>(0x3FCCCCCDu));        // 1.6f
    maxHealth = health = 40.0f;                       // +0x4FC / +0x4F4
    Character::SetPosition(pos);                      // 0x000353B0 with the new height offset
}

void Weasel::BuildModel() {                           // orig: 0x000933F0
    const bool first = !model;
    GenericNpc::BuildModel();
    if (model && first && physic) physic->SetBone("Weasel_01:Animation (Translational)");   // vf54 0x00037820
}

void Weasel::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x00093090
    const NiPoint3 z{};
    if (IsAnimPlaying(0x4D) || IsAnimPlaying(0x4F)) {
        if (finished) {
            EndRootMotion();                          // 0x000369E0
            body->SetForce({0.0f, -100.0f, 0.0f});    // &0x002D682C
        } else {
            vel = z;
            body->velocity.y = 0;                     // body+0x2C
            body->SetForce(z);
        }
    }
    if (dizzy) vel = z;
    SetRotation(rot);
    if (IsIdling() || IsAnimPlaying(0xD) || IsAnimPlaying(0x15) || IsAnimPlaying(0x19) || IsAnimPlaying(0x1A))
        AddVelocity(*body, vel);
}

void Weasel::UpdateAnimation(f32 dt) {                // orig: 0x000931E0
    if (!model) return;
    if (IsDead()) {                                   // 0x00093207 (object_gaps.md 18.6): by the death kind +0x778
        const s32 k = deathKind == 3 ? 0x4E : deathKind == 2 ? 0x4F : 0x4D;
        if (!IsAnimPlaying(k)) PlayAnimById(k, true, false, true, 0.0f);   // 0x0003CE70(k, 1, 0, 1, 0)
        animUpdated = 0;                              // +0x9F
    } else if (flattened) {                           // 0x000932C8 (+0x8C1)
        if (!IsPlayingName("flatten"))                // 0x002A0CB4
            PlayAnimation(FindSequence("flatten"), true, false, true, 0.0f);   // 0x0003A130(0x00039BA0(..), 1, 0, 1, 0)
        if (!finished) animUpdated = 0;               // (+0x9B)
        else flattened = 0;
    } else if (dizzy) {                               // 0x00093311 (+0x8C0)
        if (!IsPlayingName("dizzy")) {                // 0x00296264
            PlayAnimation(FindSequence("dizzy"), true, false, true, 0.0f);
            animUpdated = 0;
        } else if (!finished) {
            animUpdated = 0;
        } else {
            dizzy = 0;
        }
    }
    if (body->supported && velBefore.y < -40.0f) {    // body vf48 (+0x45); +0x4E8; [0x0029764C]; TEST AH,5; JP
        const NiPoint3 p = GetWorldPosition();        // vf34 (0x00093282)
        VoiceMaxDistance(SoundAt(0x32, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0 (rand pick); Voice_SetMaxDistance(D+0x198)
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

void Weasel::OnTextKey(f32 keyTime, const char* text) {   // orig: 0x00093370
    if (crt::StrICmp(text, "morph:lfdown") == 0 && IsAnimPlaying(0x15)) {
        const NiPoint3 p = GetWorldPosition();        // vf34
        VoiceMaxDistance(SoundAt(0x33, &p, 3, 0, nullptr), npcDef().value198);       // 0x0003FEA0 (rand pick with two or more)
    }
    GenericNpc::OnTextKey(keyTime, text);             // 0x00047550
}

// ---- SkeletonRanged -----------------------------------------------------------------------------------------------
void SkeletonRanged::UpdateAnimation(f32 dt) {        // orig: 0x0008E510
    if (!model) return;
    if (finished || IsIdling() || !curSeq) {
        if (IsAnimPlaying(0x2F) || IsAnimPlaying(0x3B)) {
            if (b7BC) PlayAnimById(0x62, true, false, false, 0.0f);
            else PlayAnimById(0, false, true, false, 0.0f);
            animUpdated = 0;
        }
    }
    GenericNpc::UpdateAnimation(dt);
}

void SkeletonRanged::PostThink() {                    // orig: 0x0008E410
    if (!(health > 0.0f)) return;
    const double x = double(g_frameStep) + f7A0;
    f7A0 = F(x);
    if (!(x > 1.0f)) return;                          // FCOMP [0x00293A98] (unrounded); TEST AH,0x41; JNE
    if (ni::NiNode* tip = tips.empty() ? nullptr : tips[0]) {   // (+0x78C)[0] (no "sword2", no D+0x144 test)
        const NiPoint3 p = tip->world.translate;      // +0xA0 (dword copies)
        Def* fx = level.defs->Find("skeleton_particles");   // 0x000C3F10 (0x0029766C; no null check in the original)
        auto* e = static_cast<EmitterInstance*>(fx->SpawnEffect(level, p, {0.0f, 1.0f, 0.0f}));   // vf20; UY 0x002D77EC
        e->sourceA = tip;                             // NiPointer::Assign +0x40
    }                                                 // (else: an empty debug print 0x000B0420)
    f7A0 = 0;
}

// orig: vfD4 0x0008E5A0 (object_gaps.md 14): the animation's damage, the push without CanBePushed / unitizing, the
// player's hit sounds (SkeletonGuard 0x0008D4D0's pattern).
void SkeletonRanged::ApplyHit(GameObject* t, NiPoint3& dir, u32 n) {
    if (!t || !curSeq) return;                        // (no miss sound)
    const AnimEntry* e = EntryOf(curSeq);             // CharacterDef::FindAnimationBySequence 0x0003FD20 (twice)
    if (!e) return;
    const f32 dmg = e->hasDamage ? e->damage : 0.0f;  // AnimEntry::GetDamage 0x0003EE90; FSTP
    if (t->def->IsType(1)) {
        auto* c = static_cast<Character*>(t);
        const double k0 = double(dmg) * 5.0f;         // FMUL [0x00293C24]; FST
        f32 k = F(k0);
        if (k0 < 5.0f) k = 5.0f;                      // FCOMP (unrounded); TEST AH,5; JP
        else if (k > 300.0f) k = 300.0f;              // FLD (rounded); FCOMP [0x002976F0]; TEST AH,0x41; JNE
        NiPoint3 v;
        if (n & 2) v = {F(double(dir.x) * 150.0f), F(double(dir.y) * 150.0f), F(double(dir.z) * 150.0f)};   // [0x002976EC]
        else v = {F(double(k) * dir.x), F(double(k) * dir.y), F(double(k) * dir.z)};   // 0x00011560
        if (c->body) AddVelocity(*c->body, v);        // t->body vf50 (no null test in the original)
        if (n & 1) c->Damage(dmg, this);              // vf7C(dmg, this)
        const NiPoint3 p = GetWorldPosition();        // vf34
        if (c->def->IsType(2)) {
            b7BC = 1;
            VoiceMaxDistance(SoundAt(0x17, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; SetMaxDistance(D+0x198)
        }
        if (c->def->IsType(2))
            VoiceMaxDistance(SoundAt(c->health > 0.0f ? 0x12 : 0x13, &p, 3, 0, nullptr), npcDef().value198);   // TEST AH,0x41; JNE (NaN: 0x13)
    } else if (t->def->IsType(0x40)) {
        if (auto* d = dynamic_cast<Destructable*>(t)) (void)d->Damage(dmg);   // vf64(dmg)
    }
}

void SkeletonRanged::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x0008E7A0 (batch2 K.4.2)
    if (health == 0.0f) return;                       // FCOMP [0x00293AB8]; TEST AH,0x44; JNP (NaN goes on)
    Character::Damage(amount, attacker);              // 0x000354B0 (no attacker filter of its own)
    if (amount > 0.0f) {                              // TEST AH,0x41; JNE
        if (!(health > 0.0f) && !std::isnan(health)) {   // TEST AH,0x41; JP ('>' / NaN: the hurt sound)
            if (VoiceLive(voice874)) level.Sound().StopVoice(static_cast<s32>(voice874));   // (+0x874; no clear)
            if (!voice884) {                          // (no Voice_Exists(0) test here)
                const NiPoint3 p = GetWorldPosition();   // vf34
                voice884 = static_cast<u32>(SoundAt(0, &p, 3, 0, nullptr));   // 0x0003FEA0; SetMaxDistance(D+0x198)
                VoiceMaxDistance(static_cast<s32>(voice884), npcDef().value198);
            }
        } else {
            const NiPoint3 p = GetWorldPosition();
            VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), npcDef().value198);   // (Voice_SetMaxDistance(D+0x198))
        }
    }
    if (agent && level.IsCharacter(attacker))         // 0x00033EE0
        agent->OnDamage(amount, attacker);            // 0x0002DEC0
}

// ---- SkeletonKamikaze ---------------------------------------------------------------------------------------------
SkeletonKamikaze::SkeletonKamikaze(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x0008D8C0
    health = maxHealth = 1.0f;                        // +0x4F4 / +0x4FC (+0x4F8 follows at Init)
    // +0x8C8 / +0x8CC are not written (H): 0 under emulation (7); voice8CC keeps the member default 0.
}

void SkeletonKamikaze::UnloadModel() {                // orig: 0x0008D940
    StopVoiceIfExists(voice8CC, true);                // (Voice_Exists(+0x8CC) without a null test; Voice_Stop, 0)
    GenericNpc::UnloadModel();                        // jmp 0x000466A0
}

void SkeletonKamikaze::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x0008D920
    SetRotation(rot);
    AddVelocity(*body, vel);
}

static double MoveDirection(const GenericNpc& c, NiPoint3& vel, ni::NiMatrix3& rot);   // (below: ToxicWarrior3)

void SkeletonKamikaze::Think(f32 dt) {                // orig: 0x0008E090 (no SetForce at all)
    (void)dt;
    if (!model) return;
    b7BE = 0;
    if (flag28) return;                               // O: every frame in level3-1 (inactive)
    if (cameraDistance > updateDistance) return;      // FCOMP; TEST AH,0x41; JE
    AiTick();                                         // vfF0
    if (running) {
        moveY = 1.0f;                                 // +0x4DC
        moveX = F(double(moveX) * 0.2f);              // +0x4D8, FMUL [0x00293AB4]
    }
    if (!(flag28 || IsDead() || !hasControl)) {       // orig: 0x0008E12E
        NiPoint3 vel;
        ni::NiMatrix3 rot;
        (void)MoveDirection(*this, vel, rot);         // (the length is dropped: FSTP ST0)
        const f32 speed = running ? 27.5f : npcDef().walkSpeed;   // [0x0029FC30] / D+0x74
        vel = {F(double(vel.x) * speed), F(double(vel.y) * speed), F(double(vel.z) * speed)};
        Move(vel, rot);                               // vfF4 0x0008D920
    }
    PostThink();                                      // vfF8
}

void SkeletonKamikaze::UpdateAnimation(f32 dt) {      // orig: 0x0008DB80 (reached path)
    if (!model) return;
    const NiPoint3 v = body->velocity;
    NiPoint3 a{v.x, 0.0f, v.z}, b{velBefore.x, 0.0f, velBefore.z};
    const f32 la = F(ni::Unitize(a));
    const double lb = ni::Unitize(b);
    f32 speed = desiredSpeed;
    if (!(std::fabs(double(la) - lb) > 0.5f)) speed = desiredSpeed = la;
    if (!running) {
        if (attack1) {                                // orig: 0x0008DC2D (object_gaps.md 13): the fuse is lit
            fuse = beep = 7.5f;                       // 0x40F00000
            running = 1;
            voice8CC = static_cast<u32>(SoundAt(0x68, nullptr, 1, 0, node.Get()));   // 0x0003FFC0(0x68, node, 1, 0)
            VoiceMaxDistance(static_cast<s32>(voice8CC), npcDef().value198);   // (0x0008DC66 -> 0x0008DD33)
        }
    } else {                                          // orig: 0x0008DC6B: the fuse burns
        const double x = double(fuse) - g_frameStep;  // FLD +0x8C4; FSUB [0x002D67E8]; FST
        fuse = F(x);
        const phys::CharShape& sh = body->shape;      // body+0x40
        if (x < 0.0 || x == 0.0 || (!sh.partnerIsBox && sh.partner)) {   // TEST AH,0x41; JNP / a character touched
            fuse = 0;
            running = 0;
            Damage(health, nullptr);                  // vf7C(+0x4F4, 0): the blast
        }
        double r = double(fuse) * double(std::bit_cast<f32>(0x3EAAAAABu));   // FMUL [0x0029A538] (1/3)
        if (r < 0.1f) r = 0.1f;                       // FCOM [0x00293AB0]; TEST AH,5; JP
        else if (r > 1.0f) r = 1.0f;                  // FCOM [0x00293A98]; TEST AH,0x41; JNE
        if (double(fuse) < double(beep) - r) {        // FLD +0x8C8; FSUB ST1; FLD +0x8C4; FCOMPP; TEST AH,5; JP
            beep = fuse;                              // the beep, faster as the fuse burns down
            VoiceMaxDistance(SoundAt(0x67, nullptr, 3, 0, node.Get()), npcDef().value198);   // 0x0003FFC0(0x67, node, 3, 0); SetMaxDistance
        }
    }
    const f32 t = F(double(npcDef().sneakSpeed) * 0.5f);
    if (IsDead()) {                                   // orig: 0x0008DD5A
        if (!IsAnimPlaying(0x4D)) PlayAnimById(0x4D, true, false, true, 0.0f);   // dying1
        animUpdated = 0;
    } else if (running) {                             // orig: 0x0008DD79
        PlayAnimation(FindSequence("kamikazerun"), false, true, false, 0.0f);   // 0x00039BA0 (0x0029FC20); 0x0003A130
        animUpdated = 0;
    } else if (speed > t) {                           // TEST AH,0x41; JNE
        PlayAnimById(0xD, false, true, false, 0.0f);
        animUpdated = 0;
    }
    if (animUpdated) PlayAnimById(0, false, true, false, 0.0f);
    AnimTick(dt);
}

// orig: vfD4 0x0008DDE0 (object_gaps.md 13): SkeletonGuard's 0x0008D4D0 with 200 / 150 and 40 damage.
void SkeletonKamikaze::ApplyHit(GameObject* t, NiPoint3& dir, u32 n) {
    if (!t) return;                                   // (no miss sound)
    if (t->def->IsType(1)) {
        auto* c = static_cast<Character*>(t);
        const f32 k = (n & 2) ? 150.0f : 200.0f;      // [0x002976EC] / [0x0029FC2C]
        const NiPoint3 v{F(double(dir.x) * k), F(double(dir.y) * k), F(double(dir.z) * k)};   // dir as given
        if (c->body) AddVelocity(*c->body, v);        // t->body vf50 (no CanBePushed test; a null body crashes)
        if (n & 1) c->Damage(40.0f, this);            // vf7C(0x42200000, this)
        const NiPoint3 p = GetWorldPosition();        // vf34 of the skeleton
        if (c->def->IsType(2)) {
            b7BC = 1;
            VoiceMaxDistance(SoundAt(0x17, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; SetMaxDistance(D+0x198)
        }
        if (c->def->IsType(2))
            VoiceMaxDistance(SoundAt(c->health > 0.0f ? 0x12 : 0x13, &p, 3, 0, nullptr), npcDef().value198);   // TEST AH,0x41; JNE (NaN: 0x13)
    } else if (t->def->IsType(0x40)) {
        if (auto* d = dynamic_cast<Destructable*>(t)) (void)d->Damage(40.0f);   // vf64(0x42200000)
    }
}

void SkeletonKamikaze::PostThink() {                  // orig: 0x0008DA30
    if (IsDead()) return;
    const double x = double(g_frameStep) + f7A0;
    f7A0 = F(x);
    if (!(x > 1.0f)) return;
    if (ni::NiAVObject* n = GetObjectByName(model.Get(), "polySurface6")) {   // orig: 0x0008DA70 (+0x564 vf54)
        // The node's world translation is loaded into the position and then overwritten by vf38() * (-0.5, 0, 0)
        // (0x00011770's result is stored over all three words): the particles start at that offset from the origin.
        const NiPoint3 p = ni::MulVector(rotation, {-0.5f, 0.0f, 0.0f});   // 0xBF000000
        if (Def* fx = level.defs ? level.defs->Find("skeleton_particles") : nullptr) {   // 0x000C3F10 (no null test)
            auto* e = static_cast<EmitterInstance*>(fx->SpawnEffect(level, p, {0.0f, 1.0f, 0.0f}));   // vf20; UY
            if (e) e->sourceA = n;                    // NiPointer::Assign +0x40 (no null test)
        }
    }                                                 // (else: a debug print 0x000B0420)
    f7A0 = 0;
}

void SkeletonKamikaze::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x0008DF40 (npc_combat2_port.md 3.3)
    if (health == 0.0f) return;                       // FCOMP 0; TEST AH,0x44; JNP (only ==)
    Character::Damage(amount, attacker);              // 0x000354B0
    if (amount > 0.0f) {                              // TEST AH,0x41; JNE
        if (!(health > 0.0f) && !std::isnan(health)) {   // TEST AH,0x41; JP ('>' / NaN: the hurt sound)
            // the kill blast before the voice stop and the death sound (object_gaps.md 6)
            ExplodeAtRecord(level, label ? label->c_str() : nullptr);   // [0x002DAC64]->0x000C2DE0(+0x34)
            HitSweep("center", 10.0f, 10.0f, 3, false);   // 0x0003E220("center" 0x00295D90, 10.0f, 3)
            if (VoiceLive(voice874)) level.Sound().StopVoice(static_cast<s32>(voice874));   // (+0x874; no clear)
            if (!voice884) {
                const NiPoint3 p = GetWorldPosition();   // vf34
                voice884 = static_cast<u32>(SoundAt(0, &p, 3, 0, nullptr));   // 0x0003FEA0; SetMaxDistance(D+0x198)
                VoiceMaxDistance(static_cast<s32>(voice884), npcDef().value198);
            }
        } else {
            const NiPoint3 p = GetWorldPosition();
            VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), npcDef().value198);      // rand 0x0003FEFE
        }
    }
    if (agent && level.IsCharacter(attacker)) agent->OnDamage(amount, attacker);   // 0x00033EE0; 0x0002DEC0
}

// ---- Tank -------------------------------------------------------------------------------------------------------
Tank::Tank(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00091F60
    body->b1C = 1;
    if (lvl.defs) {
        laser = lvl.defs->Find("TankFx_Laser");
        nozzle = lvl.defs->Find("TankFx_LaserNozzle");
        laserPart = lvl.defs->Find("TankFx_LaserPart");
    }
}

void Tank::BuildModel() {                             // orig: 0x00092150
    if (model) return;
    GenericNpc::BuildModel();
    if (!model) return;
    tip1 = GetObjectByName(model.Get(), "tip1");      // model vf54
    tip2 = GetObjectByName(model.Get(), "tip2");
    if (ni::NiAVObject* turret = GetObjectByName(node.Get(), "turret_rotate")) {
        auto* aim = new AimController(*this);         // new(0x8C) 0x00037520
        aim55C = aim;                                 // +0x55C
        aim->AddTarget(level.player, turret);         // 0x0003CF90
        aim->AddTarget(level.player, GetObjectByName(node.Get(), "pod_rotate"));
        ni::NiTimeController* c = keyframes.Get();    // appended after the KFM (0x0013EC10 / 0x0013EDD0)
        while (c->nextControllerPtr) c = c->nextControllerPtr.Get();
        c->nextControllerPtr = aim;
    }
}

void Tank::UpdateAnimation(f32 dt) {                  // orig: 0x00092100
    if (!model) return;
    const double x = double(g_frameStep) + holdTimer; // FLD step; FADD; FST
    holdTimer = F(x);
    if (x > 0.266f) {                                 // FCOMP [0x002A09D4] (unrounded x); TEST AH,0x41; JNE
        holdTimer = std::bit_cast<f32>(0x3E883127u);
        HoldAnimation(nullptr, 0.0f);                 // 0x000364C0
    }
    GenericNpc::UpdateAnimation(dt);
}

void Tank::Move(NiPoint3& vel, ni::NiMatrix3& rot) {  // orig: 0x0008D920
    SetRotation(rot);                                 // vf24
    NiPoint3& v = body->velocity;                     // vf50 AddVelocity 0x000AF7D0
    v = {F(double(v.x) + vel.x), F(double(vel.y) + v.y), F(double(vel.z) + v.z)};
}

void Tank::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x00091F20 (npc_combat2_port.md 3.3)
    if (!attacker) return;
    if (attacker != this) b8C8 = 1;                   // byte +0x8C8 (natives HasTakenDamage / ClearDamageFlag)
    GenericNpc::Damage(amount, attacker);             // jmp 0x00047240
}

void Tank::FireWeapon(s32 mode, const NiPoint3& at) {   // orig: 0x00092350 (RET 8)
    if (mode < 0 || mode >= 4) {                      // (signed)
        u8C0 = -1;                                    // +0x8C0 = 0xFFFFFFFF
        return;
    }
    u8C0 = mode;
    target = at;                                      // +0x8D4 (3 dwords)
    if (mode == 3) return;
    NiPoint3 c = CharacterPoint();                    // vf78
    if (tip1) c.y = tip1->world.translate.y;          // (+0x8CC)+0xA4 (no null test in the original)
    const NiPoint3 v = ni::MulVector(rotation, {0.0f, 1.0f, 0.0f});   // vf38; 0x00011770(&UNIT_Y 0x002D77EC)
    NiPoint3 d{F(double(target.x) - c.x), F(double(target.y) - c.y), F(double(target.z) - c.z)};
    (void)ni::Unitize(d);                             // 0x000116A0 (FSTP ST0)
    const double dot = (double(v.z) * d.z + double(v.y) * d.y) + double(d.x) * v.x;
    const double x = (dot + dot) + 1.0f;              // FADD ST0,ST0; FADD [0x00293A98]
    f32 w = F(x);                                     // FST
    if (x > 1.0f) w = 1.0f;                           // FCOMP [0x00293A98] (unrounded); TEST AH,0x41; JNE
    else if (w < 0.0f) w = 0.0f;                      // FLD w; FCOMP 0; TEST AH,5; JP
    ni::NiSequence* b = SeqById(0xF6);                // (the second id first)
    SumBlend(SeqById(0xF5), b, w);                    // 0x00039ED0: the turret's elevation
    holdTimer = 0;                                    // +0x8C4
}

void Tank::AimAt() {                                  // orig: 0x000924D0
    NiPoint3 d{F(double(target.x) - aimPos.x), F(double(target.y) - aimPos.y), F(double(target.z) - aimPos.z)};
    (void)ni::Unitize(d);                             // (FSTP ST0)
    const double ux = 0.0f, uy = 1.0f, uz = 0.0f;     // UNIT_Y 0x002D77EC / F0 / F4 (read from memory)
    // S = d x Y: x and y stay on the x87 stack, z is stored (rounded) first
    const double sx = double(d.y) * uz - double(d.z) * uy;
    const double sy = double(d.z) * ux - uz * double(d.x);
    const f32 sz = F(uy * double(d.x) - double(d.y) * ux);
    // T = S x d (with the unrounded sx / sy)
    const f32 tz = F(double(d.y) * sx - sy * double(d.x));
    const f32 tx = F(double(d.z) * sy - double(sz) * d.y);
    const f32 ty = F(double(sz) * d.x - double(d.z) * sx);
    // chunk 0 (+0x7F0) = d, chunk 1 (+0x800) = T, chunk 2 (+0x810) = S (each chunk a column)
    aimRot.m[0][0] = d.x, aimRot.m[1][0] = d.y, aimRot.m[2][0] = d.z;
    aimRot.m[0][1] = tx, aimRot.m[1][1] = ty, aimRot.m[2][1] = tz;
    aimRot.m[0][2] = F(sx), aimRot.m[1][2] = F(sy), aimRot.m[2][2] = sz;
}

void Tank::FireFrom(const NiPoint3& p) {              // orig: 0x00092600 (RET 4)
    aimPos = p;                                       // +0x7D8
    AimAt();                                          // 0x000924D0
    const NiPoint3 dir{aimRot.m[0][0], aimRot.m[1][0], aimRot.m[2][0]};   // chunk 0 (a local copy)
    if (laser) {                                      // +0x8E0 vf14(S, &aimPos, &aimRot, "Tank_Laser_shot" 0x0029D040)
        GameObject* o = laser->Spawn(level, &level, aimPos, aimRot, "Tank_Laser_shot");
        if (auto* pr = dynamic_cast<Projectile*>(o)) pr->shooter = this;   // +0x40 (no null test in the original)
    }
    if (nozzle) (void)nozzle->SpawnEffect(level, aimPos, dir);        // +0x8E4 vf20(&aimPos, &dir)
    if (laserPart) (void)laserPart->SpawnEffect(level, aimPos, dir);  // +0x8E8 vf20
    VoiceMaxDistance(SoundAt(4, &aimPos, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; Voice_SetMaxDistance(D+0x198)
}

void Tank::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 0x000926C0 (RET 8)
    if (crt::StrICmp(text, "morph:firew") != 0) {     // 0x002974C0
        GenericNpc::OnTextKey(keyTime, text);         // 0x00047550
        return;
    }
    if (u8C0 != 1) return;                            // (mode 0 tested first: also nothing)
    if (tip1) FireFrom(tip1->world.translate);        // (+0x8CC)+0xA0 (no null tests in the original)
    if (tip2) FireFrom(tip2->world.translate);        // (+0x8D0)+0xA0
}

// ---- Junkbot (docs/analysis/level2-2_port.md 5) -------------------------------------------------------------------
void Junkbot::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x000882D0
    directRoot = 0;                                   // byte +0x638
    if (!IsAnimPlaying(0xC5) && !IsAnimPlaying(0xC6) && !IsAnimPlaying(0xC7) && !IsAnimPlaying(0xC8)) {
        EndRootMotion();                              // 0x000369E0
        body->SetForce({0.0f, -100.0f, 0.0f});        // body vf10(&0x002D682C)
    } else {
        body->velocity.y = 0;                         // body+0x2C
        if (finished) EndRootMotion();                // byte +0x9B
    }
    SetRotation(rot);                                 // vf24
    AddVelocity(*body, vel);                          // body vf50 0x000AF7D0 (unconditional)
}

void Junkbot::UpdateAnimation(f32 dt) {               // orig: 0x000883B0
    if (!model) return;                               // (no AnimTick either)
    if (!(health4F8 == health)) f7A8 = 0;             // FCOMP +0x4F4; TEST AH,0x44; JNP (NaN: cleared)
    health4F8 = health;                               // (raw)
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0xC6)) {
            PlayAnimation(SeqById(0xC6), true, false, true, 0.0f);   // 0x0003A130 (push 0; 1; 0; 1)
            BeginRootMotion();                        // 0x000368A0
        }
        animUpdated = 0;                              // byte +0x9F
    } else if (b508) {                                // byte +0x508
        timer7A4 = animTime;                          // +0x7A4 = +0x78
        if (!IsAnimPlaying(0xC1) || finished) {       // (0x00033E20 called twice)
            PlayAnimById(0xC1, true, false, false, 0.0f);   // 0x0003CE70 (push 0; 0; 0; 1)
            const NiPoint3 p = GetWorldPosition();    // vf34
            VoiceMaxDistance(SoundAt(0xA, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0 (Voice_SetMaxDistance(v, D+0x198): )
        }
        animUpdated = 0;
    } else if ((finished || !curSeq) && animUpdated) {   // +0x9B / +0xA4 / +0x9F
        ni::NiSequence* ca = SeqById(0xCA);
        if (double(animTime) - timer7A4 < 5.0f && ca) {  // FLD +0x78; FSUB +0x7A4; FCOMP [0x00293C24]; TEST AH,5; JP
            if (!IsAnimPlaying(0xCA) || finished) PlayAnimation(ca, false, true, true, 0.0f);   // (push 0; 1; 1; 0)
        } else if (!IsAnimPlaying(0) || finished) {
            PlayAnimById(0, false, true, false, 0.0f);   // (push 0; 0; 1; 0)
        }
        // (animUpdated is not cleared on this path)
    }
    AnimTick(dt);                                     // 0x0003B5E0
}

void Junkbot::BuildModel() {                          // orig: 0x00088CC0
    const bool first = !model;
    GenericNpc::BuildModel();                         // 0x00047850
    if (model && first && physic) physic->SetBone("junkbot_root");   // +0x560 vf54 0x00037820 (string 0x0029EEAC)
}

// orig: vf7C 0x000885C0 (Junkbot and Junkbot2; npc_combat2_port.md 3.2).
static void JunkbotDamage(GenericNpc& c, f32 amount, Character* attacker) {
    if (amount >= c.health) {                         // FLD arg; FCOMP [+0x4F4]; TEST AH,1; JNE (< / NaN skip)
        ExplodeAtCharacterRecord(c.level, c.label ? c.label->c_str() : nullptr);   // [0x002DAC64]->0x000C2F70(+0x34)
        c.HitSweep("center", 7.5f, 7.5f, 2, false);   // 0x0003E220("center", 7.5f 0x40F00000, 2) -> 0x0003C090
    }
    c.Character::Damage(amount, attacker);            // 0x000354B0
    if (amount > 0.0f) {                              // TEST AH,0x41; JNE
        if (!(c.health > 0.0f) && !std::isnan(c.health)) {   // TEST AH,0x41; JP (NaN: the hurt sound)
            if (c.VoiceLive(c.voice874)) c.level.Sound().StopVoice(static_cast<s32>(c.voice874));   // (+0x874; no clear)
            if (!c.voice884) {
                const NiPoint3 p = c.GetWorldPosition();   // vf34
                c.voice884 = static_cast<u32>(c.SoundAt(0, &p, 3, 0, nullptr));   // Voice_SetMaxDistance(D+0x198)
                c.VoiceMaxDistance(static_cast<s32>(c.voice884), c.npcDef().value198);
            }
        } else {
            const NiPoint3 p = c.GetWorldPosition();
            c.VoiceMaxDistance(c.SoundAt(2, &p, 3, 0, nullptr), c.npcDef().value198);    // rand 0x0003FEFE
        }
    }
    if (c.agent && c.level.IsCharacter(attacker)) c.agent->OnDamage(amount, attacker);   // 0x0002DEC0
}

void Junkbot::Damage(f32 amount, Character* attacker) { JunkbotDamage(*this, amount, attacker); }   // 0x000885C0

void ExplodeAtRecord(LevelData& level, const char* name, s32 kind) {   // orig: 0x000C2DE0 (and the variants)
    TriggerRec* rec = level.triggers ? level.triggers->FindOrCreate(name) : nullptr;   // S+0xC0, 0x000DB390
    if (level.defs && level.defs->explosion && rec) { // S+0x44
        const NiPoint3 p = level.triggers->Position(*rec);   // 0x000DA7E0
        (void)level.defs->explosion->Spawn(level, &level, p, kind, nullptr, true);   // vf20(S, &p, kind, 0, 1)
    }
}

// ---- Goon (object_gaps.md 3) --------------------------------------------------------------------------------------
void Goon::Damage(f32 amount, Character* attacker) {  // orig: vf7C 0x000865B0
    if (health == 0.0f) return;                       // FCOMP [0x00293AB8]; TEST AH,0x44; JNP (only ==)
    if (amount >= health) {                           // FLD arg; FCOMP [+0x4F4]; TEST AH,1; JNE (< / NaN skip)
        ExplodeAtRecord(level, label ? label->c_str() : nullptr);   // [0x002DAC64]->0x000C2DE0(+0x34)
        if (Def* sd = level.defs ? level.defs->Find("effect_scrap") : nullptr) {   // 0x000C3F10 (0x0029E584)
            NiPoint3 p = GetWorldPosition();          // vf34
            p.y = F(double(p.y) + 5.0f);              // FADD [0x00293C24]
            (void)sd->SpawnEffect(level, p, {0.0f, 1.0f, 0.0f});   // vf20(&p, &(0, 1, 0))
        }
        HitSweep("center", 5.0f, 5.0f, 2, false);     // 0x0003E220("center" 0x00295D90, 5.0f, 2)
    }
    Character::Damage(amount, attacker);              // 0x000354B0
    if (amount > 0.0f) {                              // TEST AH,0x41; JNE
        if (!(health > 0.0f) && !std::isnan(health)) {   // TEST AH,0x41; JP (NaN: the hurt sound)
            if (VoiceLive(voice874)) level.Sound().StopVoice(static_cast<s32>(voice874));   // (+0x874; no clear)
            if (!voice884) {
                const NiPoint3 p = GetWorldPosition();   // vf34
                voice884 = static_cast<u32>(SoundAt(0, &p, 3, 0, nullptr));   // Voice_SetMaxDistance(+0x10 +0x198)
                VoiceMaxDistance(static_cast<s32>(voice884), npcDef().value198);
            }
        } else {
            const NiPoint3 p = GetWorldPosition();
            VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), npcDef().value198);   // (the same SetMaxDistance)
        }
    }
    if (agent && level.IsCharacter(attacker)) agent->OnDamage(amount, attacker);   // 0x00033EE0; 0x0002DEC0
}

// The move set-up shared by SkeletonKamikaze::Think 0x0008E12E and ToxicWarrior3::Think 0x00090CD8 (object_gaps.md
// 6): GenericNpc::Think's turn and direction code without the zero tests (the turn always applies; the direction is
// unitised whatever its length). Returns the length; `vel` is the unit direction, `rot` the turned rotation.
static double MoveDirection(const GenericNpc& c, NiPoint3& vel, ni::NiMatrix3& rot) {
    const f32 a = F(double(c.npcDef().rotSpeed) * c.moveX * g_frameStep);   // FLD D+0x88; FMUL +0x4D8; FMUL; FSTP
    rot = ni::Multiply(c.rotation, ni::MakeYRotation(a));   // 0x001030B0, vf38 0x00013B30 (12 dwords to esp+0x40)
    const NiPoint3 X = ni::MulVector(rot, {1, 0, 0});     // 0x00011770 (0x002D77E0)
    const NiPoint3 Z = ni::MulVector(rot, {0, 0, 1});     // (0x002D77F8)
    const double zx = double(Z.x) * c.strafe;             // FLD +0x4E0; FLD; FMUL ST1 (kept)
    const f32 ay = F(double(Z.y) * c.strafe), az = F(double(Z.z) * c.strafe);
    const f32 bx = F(double(X.x) * c.moveY), by = F(double(X.y) * c.moveY), bz = F(double(X.z) * c.moveY);   // +0x4DC
    vel = {F(double(bx) + zx), F(double(by) + ay), F(double(bz) + az)};
    return ni::Unitize(vel);                              // 0x000116A0
}

// ---- Warthog (npc_scripted_port.md 2.2) ----------------------------------------------------------------------------
Warthog::Warthog(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00092730
    SetScale(1.0f);                                   // 0x00035080(0x3F800000)
    charging = 0;                                     // +0x8C0
    chargeTimer = 0;                                  // +0x8C4
}

void Warthog::Think(f32 dt) {                         // orig: vf54 0x00092D00
    (void)dt;
    if (!model) return;                               // +0x564
    b7BE = 0;
    if (flag28) return;
    if (cameraDistance > updateDistance) return;      // FLD +0x500; FCOMP +0x64; TEST AH,0x41; JE
    AiTick();                                         // vfF0
    if (charging) {
        moveY = 1.0f;                                 // +0x4DC (stored first)
        moveX = F(double(moveX) * std::bit_cast<f32>(0x3F2AAAABu));   // FMUL [0x0029BAD0] (2/3); FSTP +0x4D8
    }
    if (flag28 || IsDead() || !hasControl) return;    // vfA8; +0x630
    NiPoint3 vel;
    ni::NiMatrix3 rot;
    (void)MoveDirection(*this, vel, rot);             // 0x00092DF4..0x00092F1E (the length is dropped: FSTP ST0)
    const f32 speed = charging ? 22.0f : npcDef().walkSpeed;   // [0x002A0B80] / D+0x74
    vel = {F(double(vel.x) * speed), F(double(vel.y) * speed), F(double(vel.z) * speed)};
    Move(vel, rot);                                   // vfF4 0x0008D920
}

void Warthog::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfF4 0x0008D920 (SkeletonKamikaze's)
    SetRotation(rot);
    AddVelocity(*body, vel);
}

void Warthog::UpdateAnimation(f32 dt) {               // orig: vf5C 0x00092850
    if (!model) return;                               // +0x564
    if (!charging) {
        if (attack1) {                                // +0x506: the charge starts
            charging = 1;
            chargeTimer = 20.0f;                      // 0x41A00000
            attack1 = 0;                              // (0x00092928)
        }
    } else {
        const f32 x = F(double(chargeTimer) - g_frameStep);   // FLD; FSUB; FSTP local (stored to +0x8C4)
        chargeTimer = x;
        const phys::CharShape& sh = body->shape;      // body+0x40
        const bool expired = x < 0.0f || x == 0.0f;   // FCOMP 0; TEST AH,0x41; JNP (< or ==; NaN is not)
        if (expired || (!sh.partnerIsBox && sh.partner)) {
            // 0x000928D0: the contact (0x00034570: the partner unless a box) is the player's shape (S+0x94, no null
            // test), or the time ran out (the second FCOMP; TEST AH,0x41; JP)
            const void* other = sh.partnerIsBox ? nullptr : sh.partner;
            if (other == &level.player->body->shape || expired) {
                chargeTimer = 0;
                charging = 0;
                Damage(health, nullptr);              // vf7C(+0x4F4, 0): the warthog blows itself up
            }
        }
        if (attack1) attack1 = 0;
    }
    if (b508) b508 = 0;                               // 0x00092936
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

void Warthog::BuildModel() {                          // orig: vf64 0x00092CC0
    const bool first = !model;
    GenericNpc::BuildModel();                         // 0x00047850
    if (model && first && physic) physic->SetBone("WartHog_Root:Animation (Translational)");   // +0x560 vf54 (0x002A0B58)
}

void Warthog::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x00092AC0 (thiscall, RET 8)
    if (health == 0.0f) return;                       // FCOMP 0; TEST AH,0x44; JNP (only ==)
    Character::Damage(amount, attacker);              // 0x000354B0
    if (amount > 0.0f) {                              // TEST AH,0x41; JNE
        if (!(health > 0.0f) && !std::isnan(health)) {   // TEST AH,0x41; JP ('>' / NaN: the hurt sound)
            ExplodeAtRecord(level, label ? label->c_str() : nullptr);   // [0x002DAC64]->0x000C2DE0(+0x34)
            if (Def* fx = level.defs ? level.defs->Find("effect_PigBlood") : nullptr) {   // 0x000C3F10 (0x002A0B48)
                NiPoint3 p = GetWorldPosition();      // vf34
                p.y = F(double(p.y) + 1.0f);          // FADD [0x00293A98]
                (void)fx->SpawnEffect(level, p, {0.0f, 1.0f, 0.0f});   // vf20(&p, &up)
            }
            if (Def* fx = level.defs ? level.defs->Find("effect_pigparts") : nullptr) {   // (0x002A0B38)
                NiPoint3 p = GetWorldPosition();
                p.y = F(double(p.y) + 2.0f);          // FADD [0x00293B20]
                (void)fx->SpawnEffect(level, p, {0.0f, 1.0f, 0.0f});
            }
            model->flags |= ni::kNiAVHidden;          // +0x564 ->+0xB0 |= 1
            HitSweep("center", 10.0f, 10.0f, 3, false);   // 0x0003E220("center" 0x00295D90, 10.0f, 3)
            if (VoiceLive(voice874)) level.Sound().StopVoice(static_cast<s32>(voice874));   // (+0x874; no clear)
            if (!voice884) {
                const NiPoint3 p = GetWorldPosition();   // vf34
                voice884 = static_cast<u32>(SoundAt(0, &p, 3, 0, nullptr));   // 0x0003FEA0; SetMaxDistance(D+0x198)
                VoiceMaxDistance(static_cast<s32>(voice884), npcDef().value198);
            }
        } else {
            const NiPoint3 p = GetWorldPosition();
            VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), npcDef().value198);   // (+ SetMaxDistance)
        }
    }
    if (agent && level.IsCharacter(attacker)) agent->OnDamage(amount, attacker);   // 0x00033EE0; 0x0002DEC0
}

// orig: vfD4 0x00092960 (thiscall(t, dir*, n), RET 0xC): SkeletonGuard's 0x0008D4D0 with 150 / 175 and 35.
void Warthog::ApplyHit(GameObject* t, NiPoint3& dir, u32 n) {
    if (!t) return;
    if (t->def->IsType(1)) {
        auto* c = static_cast<Character*>(t);
        const f32 k = (n & 2) ? 150.0f : 175.0f;      // [0x002976EC] / [0x002A0B34]
        const NiPoint3 v{F(double(dir.x) * k), F(double(dir.y) * k), F(double(dir.z) * k)};
        if (c->body) AddVelocity(*c->body, v);        // t->body vf50 (no null test: a null body crashes)
        if (n & 1) c->Damage(35.0f, this);            // vf7C(0x420C0000, this)
        const NiPoint3 p = GetWorldPosition();        // vf34 of the warthog
        if (c->def->IsType(2)) {
            b7BC = 1;
            VoiceMaxDistance(SoundAt(0x17, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; SetMaxDistance(D+0x198)
        }
        if (c->def->IsType(2))
            VoiceMaxDistance(SoundAt(c->health > 0.0f ? 0x12 : 0x13, &p, 3, 0, nullptr), npcDef().value198);   // TEST AH,0x41; JNE (NaN: 0x13)
    } else if (t->def->IsType(0x40)) {
        if (auto* d = dynamic_cast<Destructable*>(t)) (void)d->Damage(35.0f);   // vf64(0x420C0000)
    }
}

// orig: Session 0x000C2700 (thiscall(S, shape), RET 4): the character of S+0x90 (next +0x76C) whose body's shape is
// `shape` (the body is not tested for null).
static Character* CharacterByShape(const LevelData& level, const void* shape) {
    for (Character* c : level.characters)
        if (c->body && &c->body->shape == shape) return c;
    return nullptr;
}

// ---- PatrolGuard (npc_scripted_port.md 2.3) ------------------------------------------------------------------------
void PatrolGuard::BuildModel() {                      // orig: vf64 0x0008F150
    if (modelVariant != 1) flag60 = 1;                // +0x784; +0x60 (the big guard's meshes go into the stage BSP)
    GenericNpc::BuildModel();                         // jmp 0x00047850
}

void PatrolGuard::OnMeshContact(Character* other, f32 amount, bool flag) {   // orig: vfB8 0x0008F0F0 (RET 0xC)
    (void)amount, (void)flag;
    if (IsAnimPlaying(0xC9)) return;
    if (!level.IsCharacter(other)) return;            // 0x00033EE0
    (void)GetWorldPosition();                         // vf34 (the result is not used)
    if (modelVariant != 1) Damage(maxHealth, other);  // vf7C(F(+0x4FC), other)
}

void PatrolGuard::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfF4 0x0008F1B0 (RET 8)
    const f32 e = modelVariant == 1 ? 1.5f : 9.0f;    // 0x3FC00000 / 0x41100000
    phys::CharShape& sh = body->shape;                // body+0x40
    sh.ext.x = e;                                     // +0x90
    sh.ext.y = boundHeight;                           // +0x94 = +0x4D4
    sh.ext.z = e;                                     // +0x98
    // the spin voice +0x87C (0x0008F1F0..0x0008F2F5; Voice_Exists without a null test): stopped when b7C4 / 0xC4 /
    // 0xCF, else its volume ramps by 2 * step towards 0 (0xCE) or 1
    sound::GameSound& snd = level.Sound();
    const s32 spin = static_cast<s32>(voice87C);
    if (b7C4 || IsAnimPlaying(0xC4) || IsAnimPlaying(0xCF)) {
        if (snd.VoiceExists(spin)) {
            snd.StopVoice(spin);
            voice87C = 0;
        }
    } else if (IsAnimPlaying(0xCE)) {
        if (snd.VoiceExists(spin)) {
            const double v = snd.GetVolume(spin);     // (ST0)
            if (v > 0.0f) snd.SetVolume(spin, F(v - (double(g_frameStep) + g_frameStep)));   // TEST AH,0x41; FSUBR
        }
    } else if (snd.VoiceExists(spin)) {
        const double v = snd.GetVolume(spin);
        if (v < 1.0f) snd.SetVolume(spin, F((double(g_frameStep) + g_frameStep) + v));   // TEST AH,5; JP; FADD
    }
    if (!b7C4 && !IsAnimPlaying(0xC4)) {
        f32 k = 1.0f;
        if (Player* P = level.player) {               // S+0x94
            const NiPoint3 pp = P->GetWorldPosition();   // vf34 (P first)
            const NiPoint3 me = GetWorldPosition();
            NiPoint3 d{F(double(me.x) - pp.x), 0.0f, F(double(me.z) - pp.z)};
            const double len = ni::Unitize(d);        // 0x000116A0 (ST0)
            const f32 lim = modelVariant == 1 ? 1.0f : 5.0f;   // [0x00293A98] / [0x00293C24]
            if (len < lim) {                          // FLD ST1; FCOMP ST1; TEST AH,5; JP
                k = 0.0f;
            } else {
                const double x = len * 0.05f;         // FMUL [0x00293BDC]
                k = F(x);                             // FST
                if (x > 1.0f) k = 1.0f;               // FCOMP [0x00293A98] (unrounded); TEST AH,0x41; JNE
            }
        }
        SetRotation(rot);                             // vf24
        const f32 vx = F(double(k) * vel.x);          // (vel.y = 0 is stored before the products)
        vel.y = 0;
        const f32 vy = F(double(k) * 0.0f);           // FMUL [0x00293AB8]
        const f32 vz = F(double(k) * vel.z);
        vel = {vx, vy, vz};
        AddVelocity(*body, vel);                      // body vf50
    }
    body->integratePinned = 1;                        // body+0x1B
    sh.pinned = 0;                                    // shape+0xB8
}

void PatrolGuard::UpdateAnimation(f32 dt) {           // orig: vf5C 0x0008F440 (RET 4)
    if (!model) return;                               // +0x564
    bool hurt = false;                                // BL
    if (!(health4F8 == health)) {                     // FLD +0x4F8; FCOMP +0x4F4; TEST AH,0x44; JNP
        hurt = health < health4F8;                    // TEST AH,5; JP
        f7A8 = 0;
    }
    health4F8 = health;                               // (raw)
    const bool small = modelVariant == 1;             // +0x784
    if (IsDead()) {                                   // vfA8
        StopVoiceIfExists(voice880, false);           // 0x0008F4A7 (no null test, no clear)
        if (IsAnimPlaying(0xC9)) {
            if (finished) PlayAnimById(0x4D, true, false, true, 0.0f);   // (pushes 0, 1, 0, 1, 0x4D)
        } else if (hurt) {
            PlayAnimById(0xC9, true, false, true, 0.0f);
        }
    } else if (!hurt && !(IsAnimPlaying(0xC9) && !finished)) {
        if (flag28) {
            PlayAnimById(0xC4, false, true, false, 0.0f);   // (pushes 0, 0, 1, 0, 0xC4)
        } else if (!b7C4) {                           // patrolling: the spin up, then the spin
            if ((IsAnimPlaying(0xC2) && finished) || IsAnimPlaying(0xC3)) {
                PlayAnimation(SeqById(0xC3), false, true, false, 0.0f);   // 0x0003A130(0x00033E00(0xC3), 0, 1, 0, 0)
            } else if (!IsAnimPlaying(0xC2)) {
                PlayAnimation(SeqById(0xC2), true, false, true, 0.0f);
                voice87C = static_cast<u32>(SoundAt(small ? 0x70 : 0x6A, nullptr, 4, 0, node.Get()));   // 0x0003FFC0
                VoiceMaxDistance(static_cast<s32>(voice87C), npcDef().value198);   // 0x0008F612 / 0x0008F668
                level.Sound().SetVolume(static_cast<s32>(voice87C), 0.0f);        // (faded in by Move)
                StopVoiceIfExists(voice880, false);   // (no null test, no clear)
            }
        } else {                                      // alerted: the alarm
            if ((IsAnimPlaying(0xCE) && finished) || IsAnimPlaying(0xCF)) {
                PlayAnimation(SeqById(0xCF), false, true, false, 0.0f);
            } else {
                if (!IsAnimPlaying(0xCE)) PlayAnimation(SeqById(0xCE), true, false, true, 0.0f);
                if (!level.Sound().VoiceExists(static_cast<s32>(voice880))) {   // 0x0008F722 (no null test)
                    voice880 = static_cast<u32>(SoundAt(0x71, nullptr, 4, 0, node.Get()));
                    VoiceMaxDistance(static_cast<s32>(voice880), npcDef().value198);   // 0x0008F751
                }
            }
        }
    } else {                                          // 0x0008F758: hurt, or the hurt animation still running
        if (!IsAnimPlaying(0xC9)) {
            PlayAnimById(0xC9, true, false, false, 0.0f);   // (pushes 0, 0, 0, 1, 0xC9)
            SetFrequency(SeqById(0xC9), 3.0f);        // 0x00036240(seq, 0x40400000)
        }
    }
    animUpdated = 0;                                  // 0x0008F796 (every path)
    if (small && IsAnimPlaying(0xC3)) {               // the small guard's spinning blades
        const s32 r = static_cast<s32>(static_cast<s64>(double(animTime) * 10.0f)) % 5;   // FMUL [0x00293C2C]; _ftol; IDIV 5
        const phys::CharShape& sh = body->shape;
        if (!sh.partnerIsBox && sh.partner && r == 0) {
            Character* c = CharacterByShape(level, sh.partner);   // 0x00034570; 0x000C2700
            if (c && c->def->IsType(2) && !c->IsAnimPlaying(0xD2) && !c->IsAnimPlaying(0xD3))
                c->Damage(5.0f, this);                // vf7C(0x40A00000, this)
        }
    }
    AnimTick(dt);                                     // 0x0003B5E0 (no GenericNpc::UpdateAnimation)
}

void PatrolGuard::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 0x0008F860 (RET 8)
    GenericNpc::OnTextKey(keyTime, text);             // 0x00047550
    if (crt::StrICmp(text, "morph:explosion") != 0) return;   // 0x0029FFEC
    ExplodeAtRecord(level, label ? label->c_str() : nullptr);   // [0x002DAC64]->0x000C2DE0(+0x34)
    const NiPoint3 p = GetWorldPosition();            // vf34
    if (modelVariant == 1) {
        HitSweep("center", 5.0f, 5.0f, 2, false);     // 0x0003E220("center" 0x00295D90, 5.0f, 2)
        VoiceMaxDistance(SoundAt(0x6F, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; SetMaxDistance(D+0x198)
    } else {
        HitSweep("center", 12.0f, 12.0f, 2, false);   // (0x41400000)
        VoiceMaxDistance(SoundAt(0x6E, &p, 3, 0, nullptr), npcDef().value198);
        RemoveModelCollisionMeshes();                 // 0x00035230
        flag60 = 0;                                   // +0x60
    }
}

void PatrolGuard::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x0008F940 (RET 8)
    if (!attacker || !attacker->def->IsType(2)) return;
    if (!attacker->IsAnimPlaying(0xD2) && !attacker->IsAnimPlaying(0xD3)) return;   // only Bender's spin attacks
    if (modelVariant == 1) {
        const f32 h = health;                         // +0x4F4 (a dword copy)
        const NiPoint3 p = GetWorldPosition();        // vf34 (before the damage)
        GenericNpc::Damage(amount, attacker);         // 0x00047240
        if (!(health == h)) VoiceMaxDistance(SoundAt(0x6D, &p, 3, 0, nullptr), npcDef().value198);   // TEST AH,0x44; JNP (NaN plays)
    } else {
        GenericNpc::Damage(amount, attacker);
    }
}

// ---- SpinBot (npc_scripted_port.md 2.5) ----------------------------------------------------------------------------
void SpinBot::OnMeshContact(Character* other, f32 amount, bool flag) {   // orig: vfB8 0x0008E930 (RET 0xC)
    (void)flag;
    if (level.IsCharacter(other)) Damage(amount, other);   // 0x00033EE0; vf7C(amount, other)
}

void SpinBot::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfF4 0x0008EA00 (RET 8)
    hitFlipped = 0;                                   // +0x8C2
    if (IsPlayingName("flip")) return;                // 0x00039D60 (0x0029FE98)
    if (!(flipTimer == 0.0f)) return;                 // FCOMP [0x00293AB8]; TEST AH,0x44; JP (NaN: no move)
    SetRotation(rot);                                 // vf24
    AddVelocity(*body, vel);                          // body vf50
}

void SpinBot::UpdateAnimation(f32 dt) {               // orig: vf5C 0x0008EA50 (RET 4)
    if (!model) return;                               // +0x564
    if (!(health4F8 == health)) f7A8 = 0;             // TEST AH,0x44; JNP
    health4F8 = health;                               // (raw)
    if (!flag28) f8C8 = g_frameStep;                  // (raw dword)
    // the alarm voice +0x880 (Voice_Exists without a null test): stopped (0x0008EBAD / 0x0008ED0B / 0x0008EDB6) when
    // the alarm animation 0xCF no longer plays
    auto stopAlarm = [this] {
        if (level.Sound().VoiceExists(static_cast<s32>(voice880)) && !IsAnimPlaying(0xCF)) {
            level.Sound().StopVoice(static_cast<s32>(voice880));
            voice880 = 0;
        }
    };
    if (IsDead()) {                                   // vfA8
        StopVoiceIfExists(voice880, false);           // 0x0008EAAF (no null test), then 0 unconditionally
        voice880 = 0;
        if (!IsAnimPlaying(0x4D)) {
            PlayAnimById(0x4D, true, false, true, 0.0f);   // 0x0003CE70 (pushes 0, 1, 0, 1, 0x4D)
            RemoveModelCollisionMeshes();             // 0x00035230
        }
        animUpdated = 0;                              // +0x9F
        AnimTick(dt);
        return;
    }
    if (flipRequest) {                                // 0x0008EDAF: flipped over by Bender
        stopAlarm();
        PlayAnimation(FindSequence("flip"), true, false, true, 0.0f);   // 0x00039BA0; 0x0003A130
        ni::NiBsp* bsp = static_cast<Stage*>(owner)->bsp.Get();   // (+4 ? +4 - 4 : 0)->+0x24 (no tests)
        ni::RemoveCollisionMeshes(*bsp, GetObjectByName(model.Get(), "Spikes"));   // model vf54 (0x00295A58); 0x000FC2D0
        ni::RemoveCollisionMeshes(*bsp, GetObjectByName(model.Get(), "blades"));   // (0x00295A68)
    } else if ((IsPlayingName("flip") && !finished) || hitFlipped) {
        // 0x0008EDA3: flipRequest is 0 here (nothing)
    } else if (flipTimer > 0.0f || (IsPlayingName("flip_back") && !finished)) {   // TEST AH,0x41; JE (0x0029FEA0)
        const double x = double(flipTimer) - g_frameStep;   // FSUB [0x002D67E8]
        flipTimer = F(x);                             // FST
        if (x > 0.0f || std::isnan(x)) {              // FCOMP [0x00293AB8]; TEST AH,0x41; JP 0x0008EE48
            animUpdated = 0;                          // (flipRequest untouched)
            AnimTick(dt);
            return;
        }
        flipTimer = 0;
        if (!IsPlayingName("flip_back")) {
            stopAlarm();                              // 0x0008ED04
            PlayAnimation(FindSequence("flip_back"), true, false, true, 0.0f);
            ni::NiBsp* bsp = static_cast<Stage*>(owner)->bsp.Get();
            ni::AddCollisionMeshes(*bsp, GetObjectByName(model.Get(), "Spikes"), 6);   // 0x000FC110
            ni::AddCollisionMeshes(*bsp, GetObjectByName(model.Get(), "blades"), 0xC6);
        }
    } else if (flag28) {
        PlayAnimById(0xC4, false, true, false, 0.0f); // (pushes 0, 0, 1, 0, 0xC4)
        AnimTick(dt);                                 // (no +0x9F = 0)
        return;
    } else if (!b7C4) {                               // patrolling: go_active, then active
        stopAlarm();                                  // 0x0008EBA6
        if (!IsPlayingName("active") && !IsPlayingName("go_active"))   // 0x00296230 / 0x00296238
            PlayAnimation(FindSequence("go_active"), true, false, true, 0.0f);
        else if (!(IsPlayingName("go_active") && !finished))
            PlayAnimation(FindSequence("active"), false, true, false, 0.0f);
        animUpdated = 0;                              // 0x0008EE48 (flipRequest untouched)
        AnimTick(dt);
        return;
    } else {                                          // alerted: the alarm
        PlayAnimation(SeqById(0xCF), false, true, false, 0.0f);   // 0x00033E00; 0x0003A130
        if (!VoiceLive(voice880)) {                   // 0x0008EC70
            voice880 = static_cast<u32>(SoundAt(0x71, nullptr, 4, 0, node.Get()));   // def 0x0003FFC0
            VoiceMaxDistance(static_cast<s32>(voice880), npcDef().value198);   // 0x0008ECAD
        }
        AnimTick(dt);                                 // (no +0x9F = 0)
        return;
    }
    flipRequest = 0;                                  // 0x0008EE42
    animUpdated = 0;
    AnimTick(dt);                                     // 0x0003B5E0 (no GenericNpc::UpdateAnimation)
}

void SpinBot::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 0x0008EE60 (RET 8)
    GenericNpc::OnTextKey(keyTime, text);             // 0x00047550
    if (crt::StrICmp(text, "morph:explode") == 0)     // 0x0029FEAC
        ExplodeAtRecord(level, label ? label->c_str() : nullptr);   // [0x002DAC64]->0x000C2DE0(+0x34)
}

void SpinBot::UnloadModel() {                         // orig: vf68 0x0008E900
    GenericNpc::UnloadModel();                        // 0x000466A0
    StopVoiceIfExists(voice880, false);               // (Voice_Exists(+0x880) without a null test; Voice_Stop, no clear)
}

void SpinBot::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x0008EEC0 (RET 8)
    (void)amount;
    if (!attacker || !attacker->def->IsType(2)) return;
    if (!flipRequest && !IsPlayingName("flip")) {     // Bender's 0xB8 against the alarmed spinbot flips it
        if (!IsAnimPlaying(0xCF) || !attacker->IsAnimPlaying(0xB8)) return;
        StopVoiceIfExists(voice880, true);            // 0x0008EF3F (no null test)
        const ni::NiMatrix3 Y = ni::MakeYRotation(std::bit_cast<f32>(0x3FC95326u));   // 0x001030B0 (1.57285)
        SetRotation(ni::Multiply(attacker->rotation, Y));   // the attacker's vf38 * Y (0x00013B30); vf24
        flipRequest = 1;                              // +0x8C1
        flipTimer = 3.0f;                             // +0x8C4 = 0x40400000
        return;
    }
    if (attacker->IsAnimPlaying(0xD2) || attacker->IsAnimPlaying(0xD3))
        GenericNpc::Damage(health, attacker);         // 0x00047240(+0x4F4 raw, attacker)
    hitFlipped = 1;                                   // +0x8C2
}

void Weasel::Kill(Character* attacker) {              // orig: vf84 0x000934E0
    Damage(health, attacker);                         // vf7C(+0x4F4, attacker)
    deathKind = 3;                                    // +0x778
}

// ---- IgnerBoss (npc_scripted_port.md 2.9) --------------------------------------------------------------------------
namespace {
// R * v inline (the vectors at 0x002D77E0 / 0x002D77EC / 0x002D77F8): row r = (v.y * m[r][1] + v.z * m[r][2]) +
// v.x * m[r][0], unrounded.
double RowTimes(const ni::NiMatrix3& m, int r, const NiPoint3& v) {
    return (double(v.y) * m.m[r][1] + double(v.z) * m.m[r][2]) + double(v.x) * m.m[r][0];
}
constexpr f32 kIgnerFire = std::bit_cast<f32>(0x3E29FBE7u);   // orig: [0x0029ED7C] (0.166)
}  // namespace

void IgnerBoss::AimAndFire(const NiPoint3& dir) {     // orig: vf114 0x000879D0 (thiscall(const NiPoint3*), RET 4)
    const NiPoint3 kZ{0.0f, 0.0f, 1.0f}, kY{0.0f, 1.0f, 0.0f};   // 0x002D77F8 / 0x002D77EC
    const ni::NiMatrix3& R = rotation;                // vf38 (twice; the same +0x570)
    const NiPoint3 Zr{F(RowTimes(R, 0, kZ)), F(RowTimes(R, 1, kZ)), F(RowTimes(R, 2, kZ))};   // esp+0x28..
    const NiPoint3 Yr{F(RowTimes(R, 0, kY)), F(RowTimes(R, 1, kY)), F(RowTimes(R, 2, kY))};   // esp+0x1C..
    ring[ringIndex & 1] = dir;                        // +0x8C0 + 12 * +0x8D8 (0 or 1; the mask is a port guard)
    if (++ringIndex == 1) return;
    ringIndex = 0;
    // the mean of the two directions: x / y halved from the unrounded sums, z from the rounded one
    const f32 sz = F(double(ring[0].z) + ring[1].z);  // FSTP [esp+0x3C]
    NiPoint3 a{F((double(ring[0].x) + ring[1].x) * 0.5f), F((double(ring[0].y) + ring[1].y) * 0.5f),
               F(double(sz) * 0.5f)};                 // FMUL [0x00293AA0]
    (void)ni::Unitize(a);                             // 0x000116A0 (FSTP ST0)
    const f32 v = F((double(a.z) * Yr.z + double(a.x) * Yr.x) + double(Yr.y) * a.y);   // the vertical part
    const double u2 = ((double(a.z) * Zr.z + double(a.x) * Zr.x) + double(Zr.y) * 0.0f) * 2.0;   // FMUL [0]; FADD ST0,ST0
    f32 w = F(u2);                                    // FST
    if (u2 < -1.0f) w = -1.0f;                        // FCOMP [0x00293BF0] (unrounded); TEST AH,5; JP
    else if (w > 1.0f) w = 1.0f;                      // FLD w; FCOMP [0x00293A98]; TEST AH,0x41; JNE
    StopAnimation();                                  // 0x00039D90
    animTime = F(double(g_frameStep) + animTime);     // +0x78
    // the 3 x 3 aim grid: rows by v (> 0 / < -0.5 / else), columns by w (> 0: the right pair with w, else the left
    // pair with F(w + 1)); SeqById of the second sequence is evaluated first (no side effects)
    const s32 base = v > 0.0f ? 0xE9 : v < -0.5f ? 0xEF : 0xEC;   // [0x00293AB8] AH,0x41 JNE / [0x00297414] AH,5 JP
    if (w > 0.0f) SumBlend(SeqById(base + 1), SeqById(base + 2), w);   // 0x00039ED0
    else SumBlend(SeqById(base), SeqById(base + 1), F(double(w) + 1.0f));
    SetCycleType(curSeq, false);                      // 0x000362D0(+0xA4, 0)
    SetCycleType(sumSeq, false);                      // 0x000362D0(+0xAC, 0)
    animTime = F(double(animTime) - g_frameStep);
    attack1 = 1;                                      // +0x506
    fireTimer = 0;                                    // +0x8DC
}

void IgnerBoss::UpdateAnimation(f32 dt) {             // orig: vf5C 0x00087D40 (RET 4)
    if (!model) return;                               // +0x564
    const double t = double(g_frameStep) + fireTimer; // FLD step; FADD +0x8DC
    fireTimer = F(t);                                 // FST
    if (t > kIgnerFire) fireTimer = kIgnerFire;       // FCOMP [0x0029ED7C] (unrounded); TEST AH,0x41; JNE
    if (!attack1 || !(fireTimer < kIgnerFire)) {      // TEST AH,5; JNP (NaN holds)
        HoldAnimation("morph:firew", 0.0f);           // 0x000364C0 (0x002974C0): the pose held at the fire key
        attack1 = 0;
    }
    AnimTick(dt);                                     // 0x0003B5E0 (no GenericNpc::UpdateAnimation)
}

void IgnerBoss::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 0x00087DC0 (RET 8)
    if (crt::StrICmp(text, "morph:firew") != 0) {     // 0x002974C0
        GenericNpc::OnTextKey(keyTime, text);         // 0x00047550
        return;
    }
    // (+0x78C)[0] (the original reads the array's first slot without a count test; the port guards it)
    if (ni::NiAVObject* tip = tips.empty() ? nullptr : tips[0]) {
        TipToKeyTime(keyTime, tip);                   // 0x00034A10
        aimPos = tip->world.translate;                // +0x7D8 = tip+0xA0
    }
    // the muzzles: aimPos -/+ 0.5 * aimRot * (0, 0, 1) (inline, 0x002D77F8); x halved and stored, y unrounded, z from
    // the stored row; the first muzzle's z stays on the x87 stack until Q1
    const NiPoint3 kZ{0.0f, 0.0f, 1.0f};
    const f32 hx = F(RowTimes(aimRot, 0, kZ) * 0.5f); // FMUL [0x00293AA0]; FSTP
    const double hy = RowTimes(aimRot, 1, kZ) * 0.5f;
    const double hz = double(F(RowTimes(aimRot, 2, kZ))) * 0.5f;
    const f32 p1x = F(double(aimPos.x) - hx), p1y = F(double(aimPos.y) - hy);
    const double p1z = double(aimPos.z) - hz;
    const NiPoint3 P2{F(double(hx) + aimPos.x), F(hy + aimPos.y), F(hz + aimPos.z)};   // (the same rows again)
    // the aim: D = ring[0] + 0 * ring[1] (z: the product rounded first), normalised by 1 / sqrt((z*z + y*y) + x*x)
    // when that is > 1e-6 ([0x00293ACC]; NaN: 0); x / y of the result stay unrounded
    const f32 Dx = F(double(ring[0].x) + double(ring[1].x) * 0.0f);   // [0x00293AB8]
    const f32 Dy = F(double(ring[0].y) + double(ring[1].y) * 0.0f);
    const f32 Dz = F(double(ring[0].z) + double(F(double(ring[1].z) * 0.0f)));
    const double L = std::sqrt((double(Dz) * Dz + double(Dy) * Dy) + double(Dx) * Dx);   // FSQRT
    double nx = 0.0, ny = 0.0;
    f32 nz = 0.0f;
    if (L > 1e-6f) {                                  // FCOM; TEST AH,0x41; JNE
        const f32 inv = F(1.0 / L);                   // FLD [0x00293A98]; FDIV ST1; FSTP
        nx = double(Dx) * inv;
        ny = double(Dy) * inv;
        nz = F(double(Dz) * inv);
    }
    // aimRot := columns (D, S x D, S) with S = D x (0, 1, 0) (0x002D77EC): 0x00088045..0x000880FA
    const f32 Sx = F(double(0.0f) * ny - double(nz) * 1.0f);
    const f32 Sy = F(double(nz) * 0.0f - nx * 0.0f);
    const f32 Sz = F(nx * 1.0f - ny * 0.0f);
    const f32 Tx = F(double(Sy) * nz - double(Sz) * ny);
    const f32 Ty = F(double(Sz) * nx - double(nz) * Sx);
    const f32 Tz = F(double(Sx) * ny - double(Sy) * nx);
    aimRot.m[0][0] = F(nx);                           // +0x7F0 chunk 0 (FSTP)
    aimRot.m[1][0] = F(ny);
    aimRot.m[2][0] = nz;
    aimRot.m[0][1] = Tx, aimRot.m[1][1] = Ty, aimRot.m[2][1] = Tz;   // chunk 1
    aimRot.m[0][2] = Sx, aimRot.m[1][2] = Sy, aimRot.m[2][2] = Sz;   // chunk 2
    // c = 0.5 * column 0 + 0 * (column 2 + column 1) per row (computed twice, the same values)
    auto c = [&](int r) { return (double(aimRot.m[r][2]) + aimRot.m[r][1]) * 0.0f + double(aimRot.m[r][0]) * 0.5f; };
    const f32 c2 = F(c(2));
    const NiPoint3 Q1{F(c(0) + p1x), F(c(1) + p1y), F(double(c2) + p1z)};
    const NiPoint3 Q2{F(c(0) + P2.x), F(c(1) + P2.y), F(double(c2) + P2.z)};
    Shoot(npcDef().value144, Q1, aimRot, false, 0.0f);   // vf104(D+0x144, &Q1, &aimRot, 0, 0) (kind 12: "Igner_Laser")
    Shoot(npcDef().value144, Q2, aimRot, true, 0.0f);    // vf104(D+0x144, &Q2, &aimRot, 1, 0) (silent)
}

// ---- StatueHead (npc_scripted_port.md 2.8) -------------------------------------------------------------------------
void StatueHead::StartAttack(u32 kind, const NiPoint3& pos, const ni::NiMatrix3& rot) {   // orig: vfFC 0x0008FB30
    (void)kind;
    if (attack2 || attack1) return;                   // +0x507 / +0x506
    aimPos = pos;                                     // +0x7D8 (3 dwords)
    aimRot = rot;                                     // +0x7F0 (12 dwords)
    attack1 = 1;
}

void StatueHead::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfF4 0x0008FB80 (RET 8): turn only
    (void)vel;
    // the old heading (vf38) and the new one: R * (1, 0, 0) inline with the vector 0x002D77E0, every row
    // (0 * m[r][1] + 0 * m[r][2]) + 1 * m[r][0] kept unrounded on the x87 stack
    auto x = [](const ni::NiMatrix3& m, int r) {
        return (double(0.0f) * m.m[r][1] + double(0.0f) * m.m[r][2]) + double(1.0f) * m.m[r][0];
    };
    const ni::NiMatrix3& a = rotation;                // vf38 (a copy of +0x570)
    const double d = (x(rot, 2) * x(a, 2) + x(rot, 1) * x(a, 1)) + x(rot, 0) * x(a, 0);
    if (d < std::bit_cast<f32>(0x3F7EB852u)) {        // FCOMP [0x002A012C] (0.995); TEST AH,5; JP
        if (!VoiceLive(voice8C4)) {                   // 0x0008FC75 (+0x8C4 is never initialised: KNOWN_ISSUES, the port's 0)
            voice8C4 = static_cast<u32>(SoundAt(0x5A, nullptr, 3, 0, node.Get()));   // def 0x0003FFC0
            VoiceMaxDistance(static_cast<s32>(voice8C4), npcDef().value198);   // 0x0008FCAF
        }
    }
    SetRotation(rot);                                 // vf24
}

void StatueHead::UpdateAnimation(f32 dt) {            // orig: vf5C 0x0008FCD0 (RET 4): no AnimTick, no base call
    (void)dt;
    if (!model) return;                               // +0x564 (the timer is not advanced either)
    if (!flag28 && attack1 && fireTimer > 1.0f) {     // FCOMP [0x00293A98]; TEST AH,0x41; JNE
        fireTimer = 0;
        if (ni::NiAVObject* tip = GetObjectByName(node.Get(), "TIP_01")) {   // node vf54 (0x002A0130)
            TipToKeyTime(animTime, tip);              // 0x00034A10(+0x78, tip)
            aimPos = tip->world.translate;            // +0xA0
            aimRot = tip->world.rotate;               // +0x70 (through a stack copy)
            const NiPoint3 X = ni::MulVector(aimRot, {1.0f, 0.0f, 0.0f});   // 0x00011770 (0x002D77E0)
            const f32 zz = F(double(X.z) + X.z);      // FADD ST0,ST0 (z stored first)
            aimPos = {F((double(X.x) + X.x) + aimPos.x), F((double(X.y) + X.y) + aimPos.y), F(double(zz) + aimPos.z)};
            if (agent) agent->Aim(aimPos, aimRot);    // 0x0002DF50(&aimPos, &aimRot, 1) (nothing but for 0xE / 0xC)
            if (ProjectileDef* w = npcDef().weapon158) {   // D+0x158
                GameObject* o = w->Spawn(level, &level, aimPos, aimRot, "Enemy-Laser");   // vf14(S, ..) (0x002976B4)
                if (auto* p = dynamic_cast<Projectile*>(o)) {
                    p->shooter = this;                // +0x40
                    p->speed = 100.0f;                // +0x48 = 0x42C80000
                }
                VoiceMaxDistance(SoundAt(4, &aimPos, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; Voice_SetMaxDistance(D+0x198)
            }
        }
    }
    fireTimer = F(double(g_frameStep) + fireTimer);   // FLD step; FADD; FSTP
}

// ---- Crocodile (npc_scripted_port.md 2.7) --------------------------------------------------------------------------
// orig: the process globals DAT_002E1E30 (the "RippleCroc" definition) and DAT_002E1E40 (a y rotation by pi / 2),
// written by every crocodile construction (L: never reset between levels in the original; each construction
// overwrites them before any read, so a per-process static is equivalent).
static Def* g_rippleCroc = nullptr;
static ni::NiMatrix3 g_rotY90;

Crocodile::Crocodile(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00081C10
    g_rippleCroc = lvl.defs ? lvl.defs->Find("RippleCroc") : nullptr;   // 0x000C3F10 (0x0029DE14)
    g_rotY90 = ni::MakeYRotation(F(double(kPi) * 0.5f));   // FLD [0x002F9274]; FMUL [0x00293AA0]; FSTP; 0x001030B0
    SetScale(std::bit_cast<f32>(0x3F99999Au));        // 0x00035080 (1.2)
    rippleTimer = 0;                                  // +0x8C0
    stepping = 0;                                     // +0x8C8
    turned = 0;                                       // +0x8C9
    stepTimer = -1.0f;                                // +0x8C4 = 0xBF800000
}

// ---- AquaFish (no stage places one: ported, not reached) ----------------------------------------------------------
void AquaFish::UpdateAnimation(f32 dt) {              // orig: vf5C 0x00080080
    if (!model) return;                               // +0x564 (no AnimTick either)
    const GenericNPCDef& d = npcDef();
    NiPoint3 v = body->velocity;                      // body vf0C 0x000AED00 (+0x28)
    NiPoint3 b = velBefore;                           // +0x4E4
    v.y = 0;
    const f32 s1 = F(ni::Unitize(v));                 // 0x000116A0; FSTP
    b.y = 0;
    const double s2 = ni::Unitize(b);                 // (ST0)
    f32 speed;
    if (std::fabs(double(s1) - s2) > 0.5f) speed = desiredSpeed;   // FSUBR; FABS; FCOMP [0x00293AA0]; TEST AH,0x41; JNE
    else desiredSpeed = speed = s1;                   // +0x4F0
    // (two 0x00103290 transposed products of v by vf38's matrix follow; their results are not read)
    const f32 thr = F((double(d.runSpeed) - d.walkSpeed) * 0.5f + d.walkSpeed);   // D+0x78 - D+0x74; [0x00293AA0]
    bool hurt = false;
    if (!(health4F8 == health)) {                     // FCOMP +0x4F4; TEST AH,0x44; JNP (NaN: enters)
        hurt = health < health4F8;                    // TEST AH,5; JP (NaN: not hurt)
        f7A8 = 0;                                     // +0x7A8
    }
    health4F8 = health;                               // (a dword copy)
    if (IsDead()) {                                   // vfA8
        if (!(IsAnimPlaying(0x4D) || IsAnimPlaying(0x4E) || IsAnimPlaying(0x4F) || IsAnimPlaying(0x50) ||
              IsAnimPlaying(0x51)) && d.dyingCount > 0) {   // D+0xE0 (JLE, signed)
            const s32 k = crt::Rand() % d.dyingCount + 0x4D;   // orig: rand 0x00080232 (CDQ; IDIV)
            if (SeqById(k)) PlayAnimById(k, true, false, true, 0.0f);   // 0x0003CE70 (pushes 0, 1, 0, 1, k)
        }
        animUpdated = 0;                              // 0x0008030F
    } else if (hurt) {
        if (!(IsAnimPlaying(0x47) || IsAnimPlaying(0x48) || IsAnimPlaying(0x49)) && d.damageCount > 0) {   // D+0xE4
            const s32 k = crt::Rand() % d.damageCount + 0x47;   // orig: rand 0x000802A1
            if (SeqById(k)) PlayAnimById(k, true, false, true, 0.0f);
        }
        animUpdated = 0;
    } else if (finished || IsIdling() || !curSeq) {   // +0x9B / vfA0 / +0xA4
        PlayAnimById(speed < thr ? 0xD : 0x15, false, true, false, 0.0f);   // FCOMP; TEST AH,1; JNE: walk, else run
        animUpdated = 0;
    }
    if (animUpdated) PlayAnimById(0xD, false, true, false, 0.0f);   // byte +0x9F (pushes 0, 0, 1, 0, 0xD)
    AnimTick(dt);                                     // 0x0003B5E0
}

void AquaFish::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x00080390
    if (health == 0.0f) return;                       // FCOMP [0x00293AB8]; TEST AH,0x44; JNP (only ==)
    if (amount >= health) {                           // FLD arg; FCOMP [+0x4F4]; TEST AH,1; JNE (< / NaN skip)
        ExplodeAtRecord(level, label ? label->c_str() : nullptr, 3);   // [0x002DAC64]->0x000C2F20(+0x34): kind 3
        HitSweep("center", 5.0f, 5.0f, 2, false);     // 0x0003E220("center" 0x00295D90, 5.0f, 2)
    }
    Character::Damage(amount, attacker);              // 0x000354B0
    if (amount > 0.0f) {                              // TEST AH,0x41; JNE
        if (!(health > 0.0f) && !std::isnan(health)) {   // TEST AH,0x41; JP (NaN: the hurt sound)
            if (VoiceLive(voice874)) level.Sound().StopVoice(static_cast<s32>(voice874));   // (+0x874; no clear)
            if (!voice884) {
                const NiPoint3 p = GetWorldPosition();   // vf34
                voice884 = static_cast<u32>(SoundAt(0, &p, 3, 0, nullptr));   // 0x0003FEA0(0, &p, 3, 0, 0)
                VoiceMaxDistance(static_cast<s32>(voice884), npcDef().value198);   // Voice_SetMaxDistance(D+0x198)
            }
        } else {
            const NiPoint3 p = GetWorldPosition();
            VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), npcDef().value198);
        }
    }
    if (agent && level.IsCharacter(attacker)) agent->OnDamage(amount, attacker);   // 0x00033EE0, 0x0002DEC0
}

void AquaFish::OnMeshContact(Character* other, f32 amount, bool flag) {   // orig: vfB8 0x00080350
    (void)amount, (void)flag;
    if (!other || !other->def->IsType(1)) return;     // (+0x10)->vf0C(1)
    other->Damage(50.0f, this);                       // other->vf7C(50.0f 0x42480000, this)
    Damage(maxHealth, other);                         // this->vf7C(+0x4FC, other)
}

// ---- GunTurret (no stage places one: ported, not reached) ---------------------------------------------------------
void GunTurret::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x00086DE0
    if (shield) return;                               // byte +0x8C0: the shield takes it (RET 8)
    GenericNpc::Damage(amount, attacker);             // jmp 0x00047240
}

void GunTurret::UpdateAnimation(f32 dt) {             // orig: vf5C 0x00086E60
    if (!model) return;                               // +0x564 (no AnimTick either)
    auto show = [&](const char* name, bool shown) {   // node (+0x2C) vf54; +0xB0 bit 0
        if (ni::NiAVObject* o = GetObjectByName(node.Get(), name)) {
            if (shown) o->flags &= ~ni::kNiAVHidden;
            else o->flags |= ni::kNiAVHidden;
        }
    };
    bool clear9F = true;                              // (the paths that end at 0x000870FF)
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0x4D)) {                   // dying1
            PlayAnimById(0x4D, true, false, true, 0.0f);   // 0x0003CE70 (pushes 0, 1, 0, 1, 0x4D)
            animUpdated = 0;                          // +0x9F
        }
        show("Turret_Explode", true);                 // 0x0029EAC0
        show("Turret_Point", false);                  // 0x0029EAB0
        clear9F = false;                              // (-> 0x00087105)
    } else if (shield && !shieldOn) {                 // +0x8C0 / +0x8C1
        if (!IsPlayingName("Shield_turn_on_front"))   // 0x0029EA98
            PlayAnimation(FindSequence("Shield_turn_on_front"), true, false, true, 0.0f);   // 0x00039BA0, 0x0003A130
        if (finished) shieldOn = 1;                   // +0x9B
    } else if (!shield && !shieldOff) {               // +0x8C2
        if (!IsPlayingName("Shield_turn_off_front"))  // 0x0029EA80
            PlayAnimation(FindSequence("Shield_turn_off_front"), true, false, true, 0.0f);
        if (finished) shieldOff = 1;
    } else {                                          // 0x00086F86: the aim
        show("Turret_Explode", false);
        show("Turret_Point", true);
        if (!level.player) {                          // S+0x94 (-> 0x00087105)
            clear9F = false;
        } else if (ni::NiAVObject* tip = GetObjectByName(node.Get(), "Tip_01")) {   // 0x0029EA78
            UpdateNodeAt(tip, animTime);              // NiAVObject::Update(+0x78, 1)
            const NiPoint3 p = CharacterPoint();      // vf78
            const NiPoint3 up = ni::MulVector(GetWorldRotation(), {0.0f, 1.0f, 0.0f});   // vf38 * 0x002D77EC
            const NiPoint3 q = level.player->CharacterPoint();   // (S+0x94) vf78
            NiPoint3 d{F(double(q.x) - p.x), F(double(q.y) - p.y), F(double(q.z) - p.z)};
            (void)ni::Unitize(d);                     // 0x000116A0 (FSTP ST0)
            // ((up.z d.z + up.y d.y) + d.x up.x + 0.1) * 0.90909 ([0x00293AB0], [0x0029EA74] 0x3F68BA2E)
            const double s = (double(up.z) * d.z + double(up.y) * d.y) + double(d.x) * up.x;
            f32 w = F((s + 0.1f) * std::bit_cast<f32>(0x3F68BA2Eu));   // FST
            if (w > 1.0f) w = 1.0f;                   // FCOMP [0x00293A98]; TEST AH,0x41; JNE
            else if (w < 0.0f) w = 0.0f;              // FCOMP [0x00293AB8]; TEST AH,5; JP
            SumBlend(SeqById(0xE7), SeqById(0xE8), w);   // 0x00039ED0
            if (attack1) {                            // byte +0x506
                timer7A4 = animTime;                  // +0x7A4 = +0x78 (a dword copy)
                ++count7B0;                           // +0x7B0
            } else {
                HoldAnimation(nullptr, 0.0f);         // 0x000364C0(0, 0)
            }
        }
    }
    if (clear9F) animUpdated = 0;                     // 0x000870FF: byte +0x9F = 0
    if (!holdPose && IsAnimPlaying(0xE7)) FireTextKeys(SeqById(0xE7));   // byte +0x9E; 0x00039C50
    AnimTick(dt);                                     // 0x0003B5E0
}

void GunTurret::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 0x00087150
    if (crt::StrICmp(text, "morph:firew") != 0) {     // 0x002974C0
        GenericNpc::OnTextKey(keyTime, text);         // 0x00047550
        return;
    }
    UpdateNodeAt(node.Get(), animTime);               // (+0x2C)->Update(+0x78, 1)
    for (const char* name : {"Tip_01", "Tip_02"}) {   // 0x0029EA78 / 0x0029EAD0 (each found one: the same steps)
        ni::NiAVObject* tip = GetObjectByName(node.Get(), name);   // vf54
        if (!tip) continue;
        aimPos = tip->world.translate;                // +0x7D8 = tip +0xA0
        aimRot = tip->world.rotate;                   // +0x7F0 = tip +0x70 (12 dwords)
        Shoot(npcDef().value144, aimPos, aimRot, false, 0.0f);   // vf104(D+0x144, &+0x7D8, &+0x7F0, 0, 0)
        if (agent) agent->Aim(aimPos, aimRot);        // 0x0002DF50(&+0x7D8, &+0x7F0, 1), after the shot
    }
}

void Crocodile::Think(f32 dt) {                       // orig: vf54 0x00082140 (RET 4)
    phys::CharShape& sh = body->shape;                // body+0x40
    const ni::NiMatrix3 R = rotation;                 // vf38
    // FLD |m00|; FLD |m20|; FXCH; FXCH; FCOMPP (|m20| against |m00|); TEST AH,5; JP: the heading mostly along x
    if (std::fabs(R.m[2][0]) < std::fabs(R.m[0][0])) {
        if (sh.ext.x < sh.ext.z) std::swap(sh.ext.x, sh.ext.z);   // FCOMP; TEST AH,5; JP (the long side along x)
    } else {                                          // (NaN takes this branch)
        if (sh.ext.z < sh.ext.x) std::swap(sh.ext.x, sh.ext.z);
    }
    GenericNpc::Think(dt);                            // 0x0004A000
}

void Crocodile::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfF4 0x00081D80 (RET 8)
    const NiPoint3 me = GetWorldPosition();           // vf34
    Player* on = nullptr;
    for (Player* p : level.players) {                 // 0x000C2730: S+0x98, next +0xC4C
        if (p->IsDead()) continue;                    // vfA8
        const NiPoint3 pp = p->GetWorldPosition();
        const phys::CharShape& sh = body->shape;
        // FLD |d|; FLD ext; FADD c; FCOMPP; TEST AH,5; JNP next: inside on <=, == and NaN
        auto in = [](double d, f32 e, f32 c) { return !(double(e) + c < std::fabs(d)); };
        if (!in(double(pp.x) - me.x, sh.ext.x, std::bit_cast<f32>(0x3FC66666u))) continue;   // [0x00295D60] 1.55
        if (!in(double(pp.z) - me.z, sh.ext.z, std::bit_cast<f32>(0x3FC66666u))) continue;
        if (!in(double(pp.y) - me.y, sh.ext.y, 4.0f)) continue;   // [0x00293C1C]
        on = p;
        break;
    }
    SetRotation(rot);                                 // vf24
    if (!stepping && on && stepTimer < 0.0f) {        // FCOMP 0; TEST AH,5; JP
        if (ni::NiSequence* s = FindSequence("step_on")) {   // 0x00039BA0 (0x0029DF4C)
            stepping = 1;
            PlaySequenceDirect(s, false);             // 0x0003A4B0(s, 0)
        } else {
            stepTimer = 2.5f;                         // 0x40200000
        }
    }
    const f32 vx = vel.x;                             // (FLD vel.x before the y store)
    vel.y = 0;
    vel.x = F(double(vx) * 0.75f);                    // FMUL [0x00293A94]
    vel.z = F(double(vel.z) * 0.75f);
    AddVelocity(*body, vel);                          // body vf50
    if (on) AddVelocity(*on->body, vel);              // the rider moves with it
}

void Crocodile::UpdateAnimation(f32 dt) {             // orig: vf5C 0x00081EE0 (RET 4)
    if (!model) return;                               // +0x564
    if (!stepping) stepTimer = F(double(stepTimer) - dt);
    else if (finished) {                              // +0x9B
        stepTimer = 2.5f;
        stepping = 0;
    }
    const NiPoint3 v = body->velocity;                // body vf0C
    const NiPoint3 d{F(double(dt) * v.x), F(double(dt) * v.y), F(double(dt) * v.z)};
    const NiPoint3 p = GetWorldPosition();            // vf34
    const NiPoint3 np{F(double(d.x) + p.x), F(double(d.y) + p.y), F(double(d.z) + p.z)};
    SetPosition(np);                                  // vf1C: a kinematic float
    body->velocity = {};                              // body+0x28 = NiPoint3::ZERO
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
    if (!g_rippleCroc || stepping) return;
    if (!(rippleTimer < 0.0f)) {                      // FCOMP 0; TEST AH,5; JP
        rippleTimer = F(double(rippleTimer) - dt);
        return;
    }
    ni::NiMatrix3 R = rotation;                       // vf38
    const double c0 = double(R.m[0][0]) * 4.0f, c2 = double(R.m[2][0]) * 4.0f;   // column 0 x / z; FMUL [0x00293C1C]
    // x = F(c0 + np.x), z = F(np.z + c2) with c0 / c2 unrounded; y adds 0.75 to the rounded np.y (stored last)
    const NiPoint3 q{F(c0 + np.x), F(double(np.y) + 0.75f), F(double(np.z) + c2)};   // FADD [0x00293A94]
    rippleTimer = std::bit_cast<f32>(0x3F733333u);    // 0.95
    const phys::CharShape& sh = body->shape;
    const f32 a = F(double(sh.ext.z) * std::bit_cast<f32>(0x3F8CCCCDu));   // [0x00295644] 1.1
    const f32 b = F(double(sh.ext.x) * std::bit_cast<f32>(0x3F8CCCCDu));
    const f32 c = F(double(sh.ext.z) * 3.0f + a);     // [0x0029563C]
    const f32 e = F(double(sh.ext.x) * 3.0f + b);
    if (turned) R = ni::Multiply(R, g_rotY90);        // 0x00013B30 (the DAT_002E1E40 rotation)
    if (auto* rd = dynamic_cast<RippleDef*>(g_rippleCroc))   // (no type test in the original)
        (void)rd->SpawnRipple(level, owner, q, R, 1.0f, a, b, c, e, std::numeric_limits<f32>::max());   // 0x00071B10
}

// ---- LavaMan (npc_scripted_port.md 2.4) ----------------------------------------------------------------------------
LavaMan::LavaMan(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00088D40
    light = nullptr;                                  // +0x8D0
    health = maxHealth = 100.0f;                      // +0x4F4 = +0x4FC = 0x42C80000
    phase = 0;                                        // +0x8C1
    cool = 0;                                         // +0x8C0
    phaseTimer = 0;                                   // +0x8C4
    SetScale(std::bit_cast<f32>(0x3F99999Au));        // 0x00035080 (1.2)
    voice8C8 = 0;
    body->b1C = 1;                                    // (+0x5C)+0x1C (no null test)
}

LavaMan::~LavaMan() {                                 // orig: 0x00088E10
    if (light && light->parent) (void)light->parent->DetachChild(light.Get());   // (+0x8D0)+0x14 ->vfA4(&tmp, light)
    light = nullptr;
    GenericNpc::UnloadModel();                        // 0x00088EA8: 0x000466A0
    StopVoiceAndClear(voice8C8);                      // +0x8C8 non-zero && Voice_Exists: Voice_Stop, 0 (0x00088EB8)
    // (then +0x8D0 released again (null) and the GenericNpc dtor 0x00047E60)
}

void LavaMan::UnloadModel() {                         // orig: vf68 0x00088D00
    GenericNpc::UnloadModel();                        // 0x000466A0 first
    StopVoiceAndClear(voice8C8);                      // 0x00088D13
}

void LavaMan::BuildModel() {                          // orig: vf64 0x00089330
    const bool first = !model;
    GenericNpc::BuildModel();                         // 0x00047850
    if (model && first && physic) physic->SetBone("TM_Bones:Animation (Translational)");   // +0x560 vf54 (0x0029F11C)
    if (Liquid* lq = npcDef().liquid1A4) {            // D+0x1A4
        // +0x74 = model->vf54("TM_MeshShape:0" 0x00297788) (no null test of the model; no DynamicCast: the shape is
        // a geometry)
        ni::NiAVObject* m = model ? GetObjectByName(model.Get(), "TM_MeshShape:0") : nullptr;
        lq->geometry = dynamic_cast<ni::NiGeometry*>(m);
    }
}

void LavaMan::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfF4 0x00088F20 (RockMonster::Move's with `cool`)
    const NiPoint3 z{};                               // NiPoint3::ZERO 0x002F915C (copied first)
    if (IsAnimPlaying(0x4F) || IsAnimPlaying(0x50)) {
        if (finished) {                               // +0x9B
            EndRootMotion();                          // 0x000369E0
            body->SetForce({0.0f, -100.0f, 0.0f});    // body vf10(&0x002D682C)
        } else {
            rot = {};                                 // identity: 12 dwords from 0x002D7820
            vel = z;
            body->velocity.y = 0;                     // body+0x2C
            body->SetForce(z);
        }
    }
    SetRotation(rot);                                 // vf24
    if (IsIdling() || IsAnimPlaying(0xD) || IsAnimPlaying(0x15) || IsAnimPlaying(0x19) || IsAnimPlaying(0x1A)) {
        if (cool)                                     // +0x8C0: the cool lava man stands still
            vel = {F(double(vel.x) * 0.0f), F(double(vel.y) * 0.0f), F(double(vel.z) * 0.0f)};   // FMUL [0x00293AB8]
        AddVelocity(*body, vel);                      // body vf50 0x000AF7D0
    }
}

void LavaMan::UpdateAnimation(f32 dt) {               // orig: vf5C 0x00089070
    if (!model) return;                               // +0x564 (no base call)
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0x4D) && !IsAnimPlaying(0x4E)) {
            const s32 n = npcDef().dyingCount;        // D+0xE0
            const s32 r = crt::Rand();                // orig: rand 0x000890A7 (CDQ; IDIV D+0xE0, no zero test)
            if (n != 0) PlayAnimById(r % n + 0x4D, true, false, true, 0.0f);   // (UB guard: IDIV by 0 faults)
            BeginRootMotion();                        // 0x000368A0
        }
        animUpdated = 0;                              // +0x9F
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

void LavaMan::PostThink() {                           // orig: vfF8 0x000893E0 (no GenericNpc::PostThink)
    if (!model) return;                               // +0x564
    if (!light) {                                     // +0x8D0 (DAT_002EA824: no null test of the manager)
        if (level.lightManager) {
            light = level.lightManager->AddLight(level, {0.0f, 5.0f, 0.0f}, {1.0f, 0.25f, 0.0f}, 100.0f, 2, false);   // vf04
            node->AttachChild(light.Get(), true);     // +0x2C vfA0(light, 1)
            light->UpdateDownwardPass(0.0f, true);    // NiAVObject::Update(0, 1) 0x001018D0
        }
    }
    Liquid* lq = npcDef().liquid1A4;                  // D+0x1A4
    // (L+0x3C)+0x6C: the liquid's resource and its fourth layer's material (the lavaman's "text" layer)
    if (!lq || !lq->resource || !lq->resource->material[3]) return;
    if (phase) {                                      // +0x8C1: cooling down
        const double t = double(g_frameStep) + phaseTimer;
        phaseTimer = F(t);                            // FST
        if (!(t < 6.0f) && !std::isnan(t)) {          // FCOMP [0x0029C094] (unrounded); TEST AH,1; JNE (< / NaN)
            phase = 0;
            phaseTimer = 0;
        }
    } else {
        if (IsAnimPlaying(0xD) || IsAnimPlaying(0x15)) phaseTimer = F(double(g_frameStep) + phaseTimer);
        if (!(phaseTimer < 4.0f) && !std::isnan(phaseTimer)) {   // FCOMP [0x00293C1C]; TEST AH,1; JNE (< / NaN)
            StopVoiceAndClear(voice8C8);              // 0x00089593
            phase = 1;
            phaseTimer = 0;
        } else if (!VoiceLive(voice8C8)) {            // 0x000895C8: the burn loop restarts when it ended
            voice8C8 = static_cast<u32>(SoundAt(0x7C, nullptr, 3, 0, node.Get()));   // 0x0003FFC0(0x7C, node, 3, 0)
            VoiceMaxDistance(static_cast<s32>(voice8C8), npcDef().value198);   // 0x000895F6
        }
    }
    const double x = phase ? (double(phaseTimer) * double(std::bit_cast<f32>(0x3E2AAAABu)) - 0.5f) * 2.0   // FADD ST0,ST0
                           : 2.0f - (double(phaseTimer) * 0.25f - 0.5f) * 4.0f;   // [0x00293B50] / [0x00293C1C]; FSUBR 2
    f32 a = F(x);                                     // FSTP local
    u8 c;
    if (a > 1.0f) {                                   // FCOMP [0x00293A98]; TEST AH,0x41; JNE
        a = 1.0f;
        c = 0;
    } else if (a < 0.0f) {                            // TEST AH,5; JP
        a = 0.0f;
        c = 1;
    } else {
        c = (a > 0.25f || std::isnan(a)) ? 0 : 1;     // FCOMP [0x00293B50]; TEST AH,0x41; JP
    }
    cool = c;                                         // +0x8C0
    if (light)                                        // (port guard: no light without a light manager)
        light->radius = F((double(a) * std::bit_cast<f32>(0x3F666666u) + std::bit_cast<f32>(0x3DCCCCCDu)) * 100.0f);   // 0x000BFC90
    ni::NiMaterialProperty& m = *lq->resource->material[3];   // (L+0x3C)+0x6C
    m.alpha = F(1.0 - double(a));                     // FLD [1.0]; FSUB a; FSTP +0x50
    ++m.revision;                                     // +0x54
}

void LavaMan::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x000890F0 (RET 8)
    if (!attacker) return;
    if (attacker->def->IsType(2)) {
        const bool punch = static_cast<Player*>(attacker)->IsPunchSeq(attacker->curSeq);   // 0x0004FEB0(+0xA4)
        if (!cool && punch) {
            attacker->Damage(40.0f, this);            // vf7C(0x42200000, this): the hot lava man burns the puncher
            if (Def* fd = level.defs ? level.defs->Find("effect_firetrail") : nullptr) {   // 0x0029CFD0
                const NiPoint3 pp = attacker->GetWorldPosition();   // vf34
                auto* sd = dynamic_cast<SpriteEmitterDef*>(fd);   // (no type test in the original: D+0xE0 / +0xE4)
                const std::array<f32, 2> keep = sd ? sd->spriteCurrent.size : std::array<f32, 2>{};
                if (sd) sd->spriteCurrent.size = {0.5f, 1.5f};   // 0x3F000000 / 0x3FC00000
                GameObject* o = fd->SpawnEffect(level, pp, {0.0f, 1.0f, 0.0f});   // vf20(&p, &UNIT_Y 0x002D77EC)
                if (sd) sd->spriteCurrent.size = keep;
                if (o && attacker->GetNode()) {       // vf30
                    if (auto* e = dynamic_cast<EmitterInstance*>(o)) {
                        e->sourceA = GetObjectByName(attacker->GetNode(), "tip");   // +0x40 (NiPointer::Assign 0x0002A400)
                        e->loop = 0;                  // +0x78
                        e->elevation = {0.5f, 1.5f};  // +0x5C / +0x60
                    }
                }
            }
        }
    }
    if (cool) {                                       // 0x0008922D
        Character::Damage(amount, attacker);          // 0x000354B0
        if (agent && attacker->def->IsType(1) && level.IsCharacter(attacker)) agent->OnDamage(amount, attacker);   // 0x0002DEC0
    }
    if (amount > 0.0f) {                              // TEST AH,0x41; JNE (the sounds also when nothing was applied)
        if (!(health > 0.0f) && !std::isnan(health)) {   // TEST AH,0x41; JP
            if (VoiceLive(voice874)) level.Sound().StopVoice(static_cast<s32>(voice874));   // (+0x874; no clear)
            if (!voice884) {
                const NiPoint3 p = GetWorldPosition();
                voice884 = static_cast<u32>(SoundAt(0, &p, 3, 0, nullptr));   // 0x0003FEA0; SetMaxDistance(D+0x198)
                VoiceMaxDistance(static_cast<s32>(voice884), npcDef().value198);
            }
        } else {
            const NiPoint3 p = GetWorldPosition();
            VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), npcDef().value198);   // (0x00089314)
        }
    }
}

// ---- ToxicWarrior3 (object_gaps.md 6) -----------------------------------------------------------------------------
ToxicWarrior3::ToxicWarrior3(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00090AE0 (vtbls 0x002A03C0 / 0x002A03A8 / 0x002A03A4)
    health = maxHealth = 200.0f;                      // +0x4F4 = +0x4FC = 0x43480000
}

void ToxicWarrior3::Think(f32 dt) {                   // orig: vf54 0x00090C60 (no SetForce)
    (void)dt;
    if (!model) return;
    b7BE = 0;
    if (flag28) return;
    if (cameraDistance > updateDistance) return;      // FCOMP +0x64; TEST AH,0x41; JE
    AiTick();                                         // vfF0
    if (!(flag28 || IsDead() || !hasControl)) {       // +0x28, vfA8, +0x630
        NiPoint3 vel;
        ni::NiMatrix3 rot;
        const double len = MoveDirection(*this, vel, rot);
        // FCOM [0x00293A98]; TEST AH,0x41; JE (> 1: 28) / FCOMP [0x00293A94]; TEST AH,1; JE (>= 0.75: 28; NaN: D+0x74)
        const f32 speed = (len > 1.0f || len >= 0.75f) ? 28.0f : npcDef().walkSpeed;   // [0x002A04D4] / D+0x74
        vel = {F(double(vel.x) * speed), F(double(vel.y) * speed), F(double(vel.z) * speed)};
        Move(vel, rot);                               // vfF4 (GenericNpc 0x00090F50)
    }
    PostThink();                                      // vfF8
}

void ToxicWarrior3::UpdateAnimation(f32 dt) {         // orig: vf5C 0x00090BD0
    if (!model) return;
    if (attack1) attack1 = 0;                         // +0x506
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0x4E)) PlayAnimById(0x4E, true, false, true, 0.0f);
        animUpdated = 0;
    } else if (b508) {                                // +0x508
        if (!IsAnimPlaying(0x3B)) PlayAnimById(0x3B, true, false, true, 0.0f);
        animUpdated = 0;
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

// ---- RockMonster (object_gaps.md 5) -------------------------------------------------------------------------------
RockMonster::RockMonster(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x0008C1A0 (vtbls 0x0029F6D8 / 0x0029F6C0 / 0x0029F6BC)
    health = maxHealth = 200.0f;                      // +0x4F4 = +0x4FC = 0x43480000
    body->b1C = 1;                                    // (+0x5C)+0x1C (no null test)
    SetScale(1.4f);                                   // 0x00035080(0x3FB33333)
}

void RockMonster::UpdateAnimation(f32 dt) {           // orig: vf5C 0x0008C250
    if (!model) return;                               // +0x564 (no base call either)
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0x4D) && !IsAnimPlaying(0x4E)) {
            const s32 n = npcDef().dyingCount;        // D+0xE0
            const s32 r = crt::Rand();                // rand 0x0008C287 (CDQ; IDIV D+0xE0, no zero test)
            if (n != 0) PlayAnimById(r % n + 0x4D, true, false, true, 0.0f);   // (UB guard: IDIV by 0 faults)
        }
        animUpdated = 0;                              // +0x9F
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

void RockMonster::BuildModel() {                      // orig: vf64 0x0008C2C0 (also turdman:2's)
    const bool first = !model;
    GenericNpc::BuildModel();                         // 0x00047850
    if (model && first && physic) physic->SetBone("TM_Bones:Animation (Translational)");   // +0x560 vf54 (0x0029F11C)
}

void RockMonster::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfF4 0x000912C0
    if (IsAnimPlaying(0x4F) || IsAnimPlaying(0x50)) {
        if (finished) {                               // +0x9B
            EndRootMotion();                          // 0x000369E0
            body->SetForce({0.0f, -100.0f, 0.0f});    // body vf10(&0x002D682C)
        } else {
            rot = {};                                 // identity: 12 dwords from 0x002D7820
            vel = {};                                 // NiPoint3::ZERO 0x002F915C (copied to a local first)
            body->velocity.y = 0;                     // body+0x2C
            body->SetForce({});                       // body vf10(&local zero)
        }
    }
    SetRotation(rot);                                 // vf24
    if (IsIdling() || IsAnimPlaying(0xD) || IsAnimPlaying(0x15) || IsAnimPlaying(0x19) || IsAnimPlaying(0x1A))
        AddVelocity(*body, vel);                      // body vf50 0x000AF7D0
}

// ---- Turdman (object_gaps.md 21.5) ---------------------------------------------------------------------------------
Turdman::Turdman(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : RockMonster(lvl, d, pos, rot, SharedSlots{}) {  // orig: 0x00091210 (GenericNpc 0x00047B80)
    health = maxHealth = 100.0f;                      // +0x4F4 = +0x4FC = 0x42C80000
}

void Turdman::UpdateAnimation(f32 dt) {               // orig: vf5C 0x00091410
    if (!model) return;                               // +0x564
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0x50)) PlayAnimById(0x50, true, false, true, 0.0f);   // 0x0003CE70
        animUpdated = 0;                              // +0x9F
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
    Def* fx = level.defs ? level.defs->Find("effect_fart") : nullptr;   // Session::FindDef 0x002A0734
    if (!fx) return;
    if (!(animTime >= 5.0f)) return;                  // FLD +0x78; FCOMP [0x00293C24]; TEST AH,1; JNE (< / NaN)
    if (static_cast<s32>(static_cast<s64>(animTime)) % 5 != 0) return;   // _ftol; CDQ; IDIV 5
    const NiPoint3 a = ni::MulVector(GetWorldRotation(), {-3.0f, 0.0f, 0.0f});   // vf38; 0x00011770
    const NiPoint3 b = CharacterPoint();              // vf78
    const double y = double(a.y) + b.y;               // (kept on the x87 stack)
    const f32 z = F(double(a.z) + b.z);               // FSTP first
    const NiPoint3 p{F(double(a.x) + b.x), F(y - 2.0f), z};   // FSUB [0x00293B20]
    const NiPoint3 q = ni::MulVector(GetWorldRotation(), {-1.0f, 0.0f, 0.0f});   // vf38; 0x00011770
    (void)fx->SpawnEffect(level, p, q);               // FindDef again; vf20(&p, &q)
    VoiceMaxDistance(SoundAt(0x60, nullptr, 3, 0, node.Get()), npcDef().value19C);   // 0x0003FFC0(0x60, node, 3, 0); 0x00091585
}

// ---- TurdManBig (npc_scripted_port.md 2.1) -------------------------------------------------------------------------
TurdManBig::TurdManBig(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : RockMonster(lvl, d, pos, rot, SharedSlots{}) {  // orig: 0x000915A0 (GenericNpc 0x00047B80 only)
    SetScale(1.25f);                                  // 0x00035080(0x3FA00000)
    Character::SetPosition(pos);                      // 0x000353B0 again (the new heightOffset)
    if (lvl.loadSound) shakeSound = lvl.loadSound("sounds/level1/npc/mutant/shake.wav", 0);   // 0x000C9860 (0x00299E84)
    voice8C4 = 0;
    missed = 0;
}

void TurdManBig::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: vfF4 0x00091640
    const NiPoint3 z{};                               // NiPoint3::ZERO 0x002F915C (copied before the first call)
    if (IsAnimPlaying(0x4F) || IsAnimPlaying(0x50)) { // (= RockMonster::Move's block)
        if (finished) {                               // +0x9B
            EndRootMotion();                          // 0x000369E0
            body->SetForce({0.0f, -100.0f, 0.0f});    // body vf10(&0x002D682C)
        } else {
            rot = {};                                 // identity: 12 dwords from 0x002D7820
            vel = z;
            body->velocity.y = 0;                     // body+0x2C
            body->SetForce(z);                        // body vf10(&local zero)
        }
    }
    if (IsAnimPlaying(0x3D)) {                        // 0x000916E1 (the second close attack: no body changes)
        if (finished) EndRootMotion();
        else {
            rot = {};
            vel = z;
        }
    }
    SetRotation(rot);                                 // vf24
    if (IsIdling() || IsAnimPlaying(0xD) || IsAnimPlaying(0x15) || IsAnimPlaying(0x19) || IsAnimPlaying(0x1A))
        AddVelocity(*body, vel);                      // body vf50 0x000AF7D0
}

void TurdManBig::UnloadModel() {                      // orig: vf68 0x00091790
    StopVoiceIfExists(voice8C4, true);                // (Voice_Exists(+0x8C4) without a null test; Voice_Stop, 0)
    GenericNpc::UnloadModel();                        // 0x000466A0 (jmp)
}

void TurdManBig::UpdateAnimation(f32 dt) {            // orig: vf5C 0x00091880
    if (!model) return;                               // +0x564 (no base call)
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0x4F)) {
            Player* P = level.player;                 // S+0x94
            if (P && P->IsHeldBy(this)) P->Release(); // 0x000503B0 / 0x00055880: drop the held player
            PlayAnimById(0x4F, true, false, true, 0.0f);   // 0x0003CE70 (pushes 0, 1, 0, 1, 0x4F)
        }
        animUpdated = 0;                              // +0x9F
    } else if (IsAnimPlaying(0x3E)) {                 // the grab
        if (missed) {                                 // +0x8C8
            PlayAnimById(0x68, true, false, true, 0.0f);   // the recovery
            const NiPoint3 p = GetWorldPosition();    // vf34
            VoiceMaxDistance(SoundAt(0x5E, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; Voice_SetMaxDistance(D+0x198)
            animUpdated = 0;
            missed = 0;
        } else if (!finished) {
            animUpdated = 0;
        } else {
            StopVoiceIfExists(voice8C4, true);        // 0x0009197D (no null test)
        }
    }
    if (animUpdated && (finished || IsIdling() || !curSeq) && b508) {   // 0x000919A2..0x000919DE (vfA0; +0xA4)
        timer7A4 = animTime;                          // +0x7A4 = +0x78 (a dword copy)
        ++count7B0;                                   // +0x7B0
        s32 a = 0x3E;                                 // (no player / a dead player: 0x3E)
        Player* P = level.player;
        if (P && !P->IsDead()) {                      // vfA8
            const NiPoint3 me = GetWorldPosition();   // vf34 (this first)
            const NiPoint3 pp = P->GetWorldPosition();
            NiPoint3 d{F(double(pp.x) - me.x), F(double(pp.y) - me.y), F(double(pp.z) - me.z)};   // 0x00011530
            const NiPoint3 X = ni::MulVector(rotation, {1.0f, 0.0f, 0.0f});   // vf38; 0x002D77E0
            const double len = ni::Unitize(d);        // 0x000116A0 (ST0, not stored)
            a = 0x3D;
            // FCOMP [0x0029A530] 14.0; TEST AH,5; JP / FCOMP qword [0x002A0870] 0.75; TEST AH,0x41; JNE
            if (len < 14.0f && ((double(X.z) * d.z + double(X.y) * d.y) + double(X.x) * d.x) > 0.75 &&
                !P->IsHeldBy(nullptr)) {              // 0x000503B0(0)
                missed = 0;                           // (the byte store of AL = 0)
                a = 0x3E;
            }
        }
        PlayAnimById(a, true, false, true, 0.0f);     // 0x0003CE70 (pushes 0, 1, 0, 1, a)
        animUpdated = 0;
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

void TurdManBig::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 0x00091AF0 (thiscall, RET 8)
    GenericNpc::OnTextKey(keyTime, text);             // 0x00047550 first
    Player* P = level.player;                         // S+0x94
    if (crt::StrICmp(text, "morph:move_begin") == 0) {   // 0x0029DCC8: the grab
        if (!P || P->IsDead()) return;                // vfA8
        const NiPoint3 me = GetWorldPosition();       // vf34 (this first)
        const NiPoint3 pp = P->GetWorldPosition();
        NiPoint3 d{F(double(pp.x) - me.x), F(double(pp.y) - me.y), F(double(pp.z) - me.z)};
        const NiPoint3 X = ni::MulVector(rotation, {1.0f, 0.0f, 0.0f});   // vf38; 0x002D77E0
        const f32 len = F(ni::Unitize(d));            // FSTP
        const f32 dot = F((double(X.z) * d.z + double(X.y) * d.y) + double(X.x) * d.x);   // FSTP
        const f32 topY = P->TopPoint().y;             // 0x000345B0
        const f32 h = F(double(topY) - P->GetWorldPosition().y);   // vf34; FSUB; FSTP
        const f32 py = P->GetWorldPosition().y;       // vf34 again
        // [0x0029A530] 14.0 (TEST AH,5; JP) / [0x00293A94] 0.75 (TEST AH,0x41; JNE) / vf78; FSUB; FABS; FCOMP h (AH,5; JP)
        bool ok = len < 14.0f && dot > 0.75f && std::fabs(double(CharacterPoint().y) - py) < h;
        if (ok) {
            const NiPoint3 c = CharacterPoint();      // vf78 (this)
            const NiPoint3 pc = P->CharacterPoint();  // vf78 (P)
            NiPoint3 dir{F(double(pc.x) - c.x), F(double(pc.y) - c.y), F(double(pc.z) - c.z)};   // 0x00011530
            f32 rl = F(ni::Unitize(dir));             // FSTP
            const NiPoint3 start = CharacterPoint();  // vf78 a third time
            const bool hit = static_cast<bool>(ni::RayCast(start, dir, rl, true, 3));   // 0x000FB780(&s, &d, &l, 1, 3)
            ok = !P->IsHeldBy(nullptr) && !hit;       // 0x000503B0(0) first
        }
        if (!ok) {
            missed = 1;                               // 0x00091D50
            return;
        }
        if (level.camera->mode == 3) P->LeaveFirstPerson();   // S+0x8C (no null test) +0x15C; 0x00054DA0(0)
        ni::NiAVObject* t = GetObjectByName(node.Get(), "tip");   // vf54 0x00295D98 (twice)
        P->Grab(this, t ? dynamic_cast<ni::NiNode*>(t) : nullptr);   // DynamicCast NiNode 0x002F9230; 0x00055370
        return;
    }
    if (crt::StrICmp(text, "morph:switch_to_small") == 0 || crt::StrICmp(text, "morph:switch_to_normal") == 0) {
        if (P) (void)P->IsHeldBy(this);               // 0x0029DCA4 / 0x0029DC8C: the result is not used
        return;
    }
    if (crt::StrICmp(text, "morph:shake") == 0) {     // 0x002A0888
        if (!P || !P->IsHeldBy(this)) return;
        P->Damage(std::bit_cast<f32>(0x3FE3D70Au), this);   // vf7C(1.78, this)
        if (level.camera) level.camera->SetShake(0.5f, std::bit_cast<f32>(0x3ECCCCCDu));   // 0x00011290(0.5, 0.4)
        if (!VoiceLive(voice8C4)) {                   // 0x00091E2F
            const NiPoint3 p = TopPoint();            // 0x000345B0 of this
            voice8C4 = static_cast<u32>(SoundAt(0x35, &p, 3, 0, nullptr));   // 0x0003FEA0
            VoiceMaxDistance(static_cast<s32>(voice8C4), npcDef().value198);   // 0x00091E7C
        }
        return;
    }
    if (crt::StrICmp(text, "morph:putdown") == 0) {   // 0x002A0878
        if (P && P->IsHeldBy(this)) P->Release();     // 0x00055880
        StopVoiceIfExists(voice8C4, true);            // 0x00091EC8 (no null test)
    }
}

// ---- GoonSneak (object_gaps.md 4) ---------------------------------------------------------------------------------
GoonSneak::GoonSneak(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : Goon(lvl, d, pos, rot) {                        // orig: 0x00086090 (vtbls 0x0029E6E0 / 0x0029E6C8 / 0x0029E6C4)
    // +0x8D4 = 0, +0x8C0 = 0; S->0x000C2AF0("Granate" 0x00298F34) cloned (no null test in the original)
    ni::NiAVObject* g = lvl.FindEffectObject("Granate");
    ni::NiPointer<ni::NiObject> c = g ? ni::Clone(g) : nullptr;   // NiObject::Clone 0x000FE120
    grenade = dynamic_cast<ni::NiAVObject*>(c.Get());
}

GoonSneak::~GoonSneak() {                             // orig: 0x000868B0
    GoonSneak::UnloadModel();                         // 0x000868EB: vf68 0x000861C0 (not the virtual)
    // (+0x8D4 is not null-tested in the original: the grenade exists from the ctor on)
    if (grenade && grenade->parent) (void)grenade->parent->DetachChild(grenade.Get());   // (+0x14)->vfA4
    grenade = nullptr;                                // the reference released
}

void GoonSneak::BuildModel() {                        // orig: vf64 0x00086170
    if (model) return;                                // +0x564
    Character::BuildModel();                          // 0x0003DF70 (not GenericNpc's 0x00047850)
    auto* hand = dynamic_cast<ni::NiNode*>(GetObjectByName(node.Get(), "R_Hand"));   // +0x2C vf54 (0x0029E7F4)
    if (grenade && hand) hand->AttachChild(grenade.Get(), true);   // vfA0(+0x8D4, 1)
    node->UpdateProperties();                         // 0x000861AD
    node->UpdateEffects();                            // (tail jump 0x00102190)
}

void GoonSneak::UnloadModel() {                       // orig: vf68 0x000861C0
    auto* hand = dynamic_cast<ni::NiNode*>(GetObjectByName(node.Get(), "R_Hand"));
    if (grenade && hand) (void)hand->DetachChild(grenade.Get());   // vfA4 (the returned pointer released)
    GenericNpc::UnloadModel();                        // 0x000466A0
}

void GoonSneak::StartAttack(u32 kind, const NiPoint3& pos, const ni::NiMatrix3& rot) {   // orig: vfFC 0x00086220
    (void)kind;
    if (attack1 || attack2) return;                   // +0x506 / +0x507
    if (!level.player) return;                        // (UB guard: the original reads S+0x94 without a test)
    target = level.player->CharacterPoint();          // (S+0x94)->vf78 -> +0x8C8
    const NiPoint3 me = GetWorldPosition();           // vf34
    const double dx = double(target.x) - me.x, dz = double(target.z) - me.z;
    if (!(dx * dx + dz * dz < 10000.0f)) return;      // FCOMP [0x0029E7FC]; TEST AH,5; JP (>= / NaN)
    aimPos = pos;                                     // +0x7D8
    aimRot = rot;                                     // +0x7F0 (12 dwords)
    attack1 = 1;
}

void GoonSneak::UpdateAnimation(f32 dt) {             // orig: vf5C 0x000862E0
    if (!model) return;                               // +0x564
    {
        const NiPoint3 v = body->velocity;            // body vf0C
        NiPoint3 a{v.x, 0.0f, v.z}, b{velBefore.x, 0.0f, velBefore.z};
        const f32 la = F(ni::Unitize(a));             // FSTP [esp+0x14]
        const double lb = ni::Unitize(b);
        if (!(std::fabs(double(la) - lb) > 0.5f)) desiredSpeed = la;   // FABS; FCOMP [0x00293AA0]; TEST AH,0x41; JE
        // (two 0x00103290(&w, &a, vf38()) calls follow; their result is not read)
    }
    bool hurt = false;                                // [esp+0x13]
    if (!(health4F8 == health)) {                     // TEST AH,0x44; JNP (only == skips)
        if (health < health4F8) hurt = true;          // TEST AH,5; JP
        f7A8 = 0;                                     // +0x7A8 (dword 0)
    }
    health4F8 = health;
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0x4D)) PlayAnimById(0x4D, true, false, true, 0.0f);
        animUpdated = 0;
    } else if (attack1 && !throwing) {                // the throw starts
        throwing = 1;
        throwTime = 0;
        PlayAnimById(0xE3, true, false, true, 0.0f);
        animUpdated = 0;
    } else if (throwing) {                            // 0x00086460
        throwTime = F(double(g_frameStep) + throwTime);
        if (finished && attack1) {
            if (!IsAnimPlaying(0xE6)) {
                PlayAnimById(0xE6, true, false, true, 0.0f);
                animUpdated = 0;
                if (grenade) grenade->flags &= ~ni::kNiAVHidden;   // +0x8D4 +0xB0 &= 0xFE
            } else {
                throwTime = 5.0f;
            }
        }
        if (IsAnimPlaying(0xE6) && finished) throwTime = 5.0f;
        if (throwTime > 5.0f) {                       // FCOMP [0x00293C24]; TEST AH,0x41; JNE
            throwing = 0;
            if (!IsAnimPlaying(0xE2)) PlayAnimById(0xE2, true, false, true, 0.0f);
            animUpdated = 0;
        } else if (hurt) {
            PlayAnimById(0xE5, true, false, false, 0.0f);
            animUpdated = 0;
        } else if (!curSeq || animUpdated) {          // (+0xA4 set and +0x9F clear: nothing)
            if (!IsAnimPlaying(0xE4) || finished) PlayAnimById(0xE4, true, false, false, 0.0f);
            animUpdated = 0;
        }
    }
    attack1 = 0;                                      // 0x0008656B
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
    if (!IsAnimPlaying(0xE6) && grenade) grenade->flags &= ~ni::kNiAVHidden;
}

void GoonSneak::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 0x00086750
    if (crt::StrICmp(text, "morph:throw_grenade") != 0) {   // 0x0029E800
        GenericNpc::OnTextKey(keyTime, text);         // 0x00047550
        return;
    }
    if (grenade) grenade->flags |= ni::kNiAVHidden;   // +0xB0 |= 1
    ni::NiNode* tip = tips.empty() ? nullptr : tips[0];   // (+0x78C)[0]
    if (!b7BE) {                                      // (as GenericNpc's "morph:firew")
        if (!tip) return;
        TipToKeyTime(keyTime, tip);                   // 0x00034A10
        aimPos = tip->world.translate;                // +0xA0
        aimRot = tip->world.rotate;                   // +0x70 (12 dwords)
    } else if (tip) {
        TipToKeyTime(keyTime, tip);
        aimPos = tip->world.translate;
    }
    if (agent) agent->Aim(aimPos, aimRot);            // 0x0002DF50(&+0x7D8, &+0x7F0, 1) (nothing but for 0xE / 0xC)
    Shoot(npcDef().value144, aimPos, aimRot, false, 0.0f);   // vf104(D+0x144, &+0x7D8, &+0x7F0, 0, 0)
}

void GoonSneak::Shoot(u32 kind, const NiPoint3& pos, const ni::NiMatrix3& rot, bool silent, f32 t0) {   // vf104 0x00086980
    (void)kind, (void)rot, (void)silent, (void)t0;
    if (!curSeq || !EntryOf(curSeq)) return;          // CharacterDef::FindAnimationBySequence 0x0003FD20
    ProjectileDef* pd = npcDef().weapon158;           // D+0x158
    if (!pd) return;
    f32 speed = 40.0f;                                // [esp+0x18] = 0x42200000
    NiPoint3 d{F(double(target.x) - aimPos.x), 0.0f, F(double(target.z) - aimPos.z)};   // +0x8C8 - +0x7D8
    const double L = ni::Unitize(d);                  // (kept on the x87 stack)
    double lift;
    if (!(L - 10.0f > 5.0f)) {                        // FLD [0x00293C2C]; FSUBR ST1; FCOMP [0x00293C24]; TEST AH,0x41; JP
        lift = 5.0f;                                  // FSTP ST0; FLD [0x00293C24]
    } else {
        lift = L;
        if (L > 40.0f) speed = F(L);                  // FCOM [0x0029723C]; TEST AH,0x41; JNE; FST
    }
    NiPoint3 f{d.x, F(lift * 0.0125f), d.z};          // FMUL [0x0029E814]
    (void)ni::Unitize(f);
    const NiPoint3 u{0.0f, 1.0f, 0.0f};               // UNIT_Y 0x002D77EC
    NiPoint3 c{F(double(f.z) * u.y - double(u.z) * f.y), F(double(u.z) * f.x - double(f.z) * u.x),
               F(double(u.x) * f.y - double(u.y) * f.x)};   // u x f
    (void)ni::Unitize(c);
    NiPoint3 g{F(double(c.y) * f.z - double(c.z) * f.y), F(double(c.z) * f.x - double(f.z) * c.x),
               F(double(c.x) * f.y - double(c.y) * f.x)};   // c x f
    (void)ni::Unitize(g);
    ni::NiMatrix3 m;                                  // the columns f, g, -c
    m.m[0][0] = f.x, m.m[1][0] = f.y, m.m[2][0] = f.z;
    m.m[0][1] = g.x, m.m[1][1] = g.y, m.m[2][1] = g.z;
    m.m[0][2] = F(-double(c.x)), m.m[1][2] = F(-double(c.y)), m.m[2][2] = F(-double(c.z));
    GameObject* o = pd->Spawn(level, &level, pos, m, "Enemy-Laser");   // vf14(S, pos, &m, 0x002976B4)
    if (!o) return;
    if (auto* pr = dynamic_cast<Projectile*>(o)) {
        pr->speed = speed;                            // +0x48
        pr->shooter = this;                           // +0x40
        pr->arg74 = 0;                                // +0x74
    }
    const NiPoint3 p = GetWorldPosition();            // vf34
    VoiceMaxDistance(SoundAt(4, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; Voice_SetMaxDistance(D+0x198)
}

// ---- Stormtrooper (docs/analysis/npc_classes_batch2_port.md S) -----------------------------------------------------
Stormtrooper::Stormtrooper(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00090580 (vtbls 0x002A0290 / 0x002A0278 / 0x002A0274)
    const s32 r = crt::Rand();                        // orig: rand 0x000905B2
    phase = F(double(r) * kPi * double(std::bit_cast<f32>(0x38800100u)));   // FILD; FMUL [0x002F9274]; FMUL [0x002955EC]
}

void Stormtrooper::Think(f32 dt) {                    // orig: 0x000907C0
    (void)dt;
    if (!model) return;                               // (no force set-up: the body keeps the ctor's gravity at first)
    b7BE = 0;
    if (flag28) return;
    if (cameraDistance > updateDistance) return;      // FLD +0x500; FCOMP +0x64; TEST AH,0x41; JE (only '>' returns)
    AiTick();                                         // vfF0
    if (!(flag28 || IsDead() || !hasControl)) {
        const GenericNPCDef& d = npcDef();
        const f32 a = F(double(d.rotSpeed) * moveX * g_frameStep);   // FLD D+0x88; FMUL +0x4D8; FMUL step; FSTP
        const ni::NiMatrix3 rot = ni::Multiply(rotation, ni::MakeYRotation(a));   // 0x001030B0, 0x00013B30: always
        const NiPoint3 X = ni::MulVector(rot, {1, 0, 0});    // 0x00011770 (0x002D77E0)
        const NiPoint3 Z = ni::MulVector(rot, {0, 0, 1});    // (0x002D77F8)
        const double zx = double(Z.x) * strafe;       // stays on the x87 stack
        const f32 ay = F(double(Z.y) * strafe), az = F(double(Z.z) * strafe);
        const f32 bx = F(double(X.x) * moveY), by = F(double(X.y) * moveY), bz = F(double(X.z) * moveY);
        NiPoint3 vel{F(double(bx) + zx), F(double(by) + ay), F(double(bz) + az)};
        const double len = ni::Unitize(vel);          // 0x000116A0 (ST0; no FST)
        f32 speed = d.runSpeed;                       // > 1: run (FCOM [0x00293A98]; TEST AH,0x41; JE)
        if (!(len > 1.0f)) {
            if (len <= 0.25f) speed = d.sneakSpeed;   // FCOM [0x00293B50]; TEST AH,0x41; JP (NaN: the next test)
            else if (!(len >= 0.75f)) speed = d.walkSpeed;   // FCOMP [0x00293A94]; TEST AH,1; JE (NaN: walk)
        }
        vel = {F(double(vel.x) * speed), F(double(vel.y) * speed), F(double(vel.z) * speed)};
        const f32 gh = goalHeight;                    // +0x7C8 (read before the body calls)
        body->SetForce({});                           // vf10 (a local copy of 0x002F915C)
        vel.y = F(double(gh) - body->velocity.y);     // vf0C; FLD; FSUB; FSTP
        SetRotation(rot);                             // vf24
        AddVelocity(*body, vel);                      // vf50: velocity.y becomes gh
    }
    if (IsDead()) body->SetForce({0.0f, -100.0f, 0.0f});   // &0x002D682C
}

void Stormtrooper::UpdateAnimation(f32 dt) {         // orig: 0x00090740
    if (!model) return;
    if (IsDead() && double(maxHealth) * double(std::bit_cast<f32>(0x3EAAAAABu)) < double(health4F8) - health) {
        // FLD +0x4F8; FSUB +0x4F4; FLD +0x4FC; FMUL [0x0029A538]; FCOMPP; TEST AH,5; JP (only '<': the killing hit > max / 3)
        if (!IsAnimPlaying(0x4F)) {
            PlayAnimById(0x4F, true, false, true, 0.0f);   // 0x0003CE70 (pushes 0, 1, 0, 1, 0x4F)
            BeginRootMotion();                        // 0x000368A0
        }
        animUpdated = 0;                              // byte +0x9F
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

void Stormtrooper::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x00090470 (no caller for this class)
    if (IsAnimPlaying(0x4F)) {
        if (finished) {
            EndRootMotion();                          // 0x000369E0
        } else {
            rot = ni::NiMatrix3{};                    // the identity at 0x002D7820 (3 x 16 bytes)
            vel = {};
        }
    }
    SetRotation(rot);                                 // vf24
    if (IsIdling() || IsAnimPlaying(0xD) || IsAnimPlaying(0x15) || IsAnimPlaying(0x19) || IsAnimPlaying(0x1A) ||
        IsAnimPlaying(0x16) || IsAnimPlaying(0x17))
        AddVelocity(*body, vel);                      // vf50
}

void Stormtrooper::Damage(f32 amount, Character* attacker) {   // orig: 0x00090600
    if (health == 0.0f) return;                       // FCOMP [0x00293AB8]; TEST AH,0x44; JNP (NaN goes on)
    if (amount >= health) {                           // FLD amount; FCOMP +0x4F4; TEST AH,1; JNE skips
        ExplodeAtRecord(level, label ? label->c_str() : nullptr);   // [0x002DAC64]->0x000C2DE0(+0x34) 0x0009062E
        HitSweep("center", 7.5f, 7.5f, 2, false);     // 0x0003E220("center" 0x00295D90, 7.5f 0x40F00000, 2)
    }
    Character::Damage(amount, attacker);              // 0x000354B0
    if (amount > 0.0f) {                              // TEST AH,0x41; JNE skips
        if (health > 0.0f || std::isnan(health)) {   // TEST AH,0x41; JP (> or NaN): the impact sound
            const NiPoint3 p = GetWorldPosition();    // vf34
            VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0 (rand pick; Voice_SetMaxDistance(h, D+0x198))
        } else {
            if (VoiceLive(voice874)) level.Sound().StopVoice(static_cast<s32>(voice874));   // (+0x874; no clear)
            if (!voice884) {
                const NiPoint3 p = GetWorldPosition();
                voice884 = static_cast<u32>(SoundAt(0, &p, 3, 0, nullptr));   // the die sound (none in the def)
                VoiceMaxDistance(static_cast<s32>(voice884), npcDef().value198);
            }
        }
    }
    if (agent && level.IsCharacter(attacker))         // 0x00033EE0
        agent->OnDamage(amount, attacker);            // 0x0002DEC0
}

// ---- StormtrooperMk2 (docs/analysis/object_gaps.md 21.4): goonsneak's attack with +0x8D0 ---------------------------
void StormtrooperMk2::UpdateAnimation(f32 dt) {       // orig: vf5C 0x00090150
    if (model) GenericNpc::UpdateAnimation(dt);       // +0x564 ? JMP 0x00048190 : RET 4
}

void StormtrooperMk2::StartAttack(u32 kind, const NiPoint3& pos, const ni::NiMatrix3& rot) {   // orig: vfFC 0x0008FF50
    (void)kind;
    if (attack1 || attack2) return;                   // +0x506 / +0x507
    if (!level.player) return;                        // (UB guard: the original reads S+0x94 without a test)
    target = level.player->CharacterPoint();          // (S+0x94)->vf78 -> +0x8D0
    const NiPoint3 me = GetWorldPosition();           // vf34
    const double dx = double(target.x) - me.x, dz = double(target.z) - me.z;
    if (!(dx * dx + dz * dz < 10000.0f)) return;      // FCOMP [0x0029E7FC]; TEST AH,5; JP (>= / NaN)
    aimPos = pos;                                     // +0x7D8
    aimRot = rot;                                     // +0x7F0 (12 dwords)
    attack1 = 1;
}

void StormtrooperMk2::OnTextKey(f32 keyTime, const char* text) {   // orig: vfE8 0x00090010
    if (crt::StrICmp(text, "morph:throw_grenade") != 0) {   // 0x0029E800
        GenericNpc::OnTextKey(keyTime, text);         // 0x00047550
        return;
    }
    ni::NiNode* tip = tips.empty() ? nullptr : tips[0];   // (+0x78C)[0]
    if (!b7BE) {
        if (!tip) return;
        TipToKeyTime(keyTime, tip);                   // 0x00034A10
        aimPos = tip->world.translate;                // +0xA0
        aimRot = tip->world.rotate;                   // +0x70 (12 dwords)
    } else if (tip) {
        TipToKeyTime(keyTime, tip);
        aimPos = tip->world.translate;
    }
    if (agent) agent->Aim(aimPos, aimRot);            // 0x0002DF50(&+0x7D8, &+0x7F0, 1) (nothing but for 0xE / 0xC)
    Shoot(npcDef().value144, aimPos, aimRot, false, 0.0f);   // vf104(D+0x144, &+0x7D8, &+0x7F0, 0, 0)
}

void StormtrooperMk2::Shoot(u32 kind, const NiPoint3& pos, const ni::NiMatrix3& rot, bool silent, f32 t0) {   // vf104 0x00090170
    (void)kind, (void)rot, (void)silent, (void)t0;
    if (!curSeq || !EntryOf(curSeq)) return;          // CharacterDef::FindAnimationBySequence 0x0003FD20
    ProjectileDef* pd = npcDef().weapon158;           // D+0x158
    if (!pd) return;
    f32 speed = 40.0f;                                // [esp+0x18] = 0x42200000
    NiPoint3 d{F(double(target.x) - aimPos.x), 0.0f, F(double(target.z) - aimPos.z)};   // +0x8D0 - +0x7D8
    const double L = ni::Unitize(d);                  // (kept on the x87 stack)
    double lift;
    if (!(L - 10.0f > 5.0f)) {                        // FLD [0x00293C2C]; FSUBR ST1; FCOMP [0x00293C24]; TEST AH,0x41; JP
        lift = 5.0f;                                  // FSTP ST0; FLD [0x00293C24]
    } else {
        lift = L;
        if (L > 40.0f) speed = F(L);                  // FCOM [0x0029723C]; TEST AH,0x41; JNE; FST
    }
    NiPoint3 f{d.x, F(lift * 0.0125f), d.z};          // FMUL [0x0029E814]
    (void)ni::Unitize(f);
    const NiPoint3 u{0.0f, 1.0f, 0.0f};               // UNIT_Y 0x002D77EC
    NiPoint3 c{F(double(f.z) * u.y - double(u.z) * f.y), F(double(u.z) * f.x - double(f.z) * u.x),
               F(double(u.x) * f.y - double(u.y) * f.x)};   // u x f
    (void)ni::Unitize(c);
    NiPoint3 g{F(double(c.y) * f.z - double(c.z) * f.y), F(double(c.z) * f.x - double(f.z) * c.x),
               F(double(c.x) * f.y - double(c.y) * f.x)};   // c x f
    (void)ni::Unitize(g);
    ni::NiMatrix3 m;                                  // the columns f, g, -c
    m.m[0][0] = f.x, m.m[1][0] = f.y, m.m[2][0] = f.z;
    m.m[0][1] = g.x, m.m[1][1] = g.y, m.m[2][1] = g.z;
    m.m[0][2] = F(-double(c.x)), m.m[1][2] = F(-double(c.y)), m.m[2][2] = F(-double(c.z));
    GameObject* o = pd->Spawn(level, &level, pos, m, "STMK2-GRANATE");   // vf14(S, pos, &m, 0x002A0264)
    if (!o) return;
    if (auto* pr = dynamic_cast<Projectile*>(o)) {
        pr->speed = speed;                            // +0x48
        pr->shooter = this;                           // +0x40
        pr->arg74 = 0;                                // +0x74
    }
    const NiPoint3 p = GetWorldPosition();            // vf34
    VoiceMaxDistance(SoundAt(4, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; Voice_SetMaxDistance(D+0x198)
}

// ---- ToxicWarrior4 (section T) -------------------------------------------------------------------------------------
ToxicWarrior4::ToxicWarrior4(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00090FF0
    health = maxHealth = 75.0f;                       // +0x4F4 / +0x4FC (0x42960000); +0x4F8 follows at Init
}

void ToxicWarrior4::StartAttack(u32 kind, const NiPoint3& pos, const ni::NiMatrix3& rot) {   // orig: vfFC 0x00091060
    (void)kind;
    if (attack1 || attack2) return;                   // +0x506 / +0x507
    if (!level.player) return;                        // (UB guard: the original reads S+0x94 without a test)
    target = level.player->CharacterPoint();          // (S+0x94)->vf78 -> +0x8C0
    aimPos = pos;                                     // +0x7D8
    aimRot = rot;                                     // +0x7F0 (12 dwords)
    attack1 = 1;                                      // +0x506
}

void ToxicWarrior4::Shoot(u32 kind, const NiPoint3& pos, const ni::NiMatrix3& rot, bool silent, f32 t0) {   // 0x000910F0
    (void)kind, (void)t0;
    if (!curSeq || !EntryOf(curSeq)) return;          // CharacterDef::FindAnimationBySequence 0x0003FD20
    ProjectileDef* pd = npcDef().projectile15C;       // D+0x15C
    if (!pd) return;
    GameObject* o = pd->Spawn20(level, &level, pos, rot, 1, nullptr);   // vf20(S, pos, rot, 1, 0)
    if (!o) return;
    if (auto* pr = dynamic_cast<Projectile*>(o)) pr->shooter = this;   // +0x40 (no type test in the original)
    if (silent) return;
    VoiceMaxDistance(SoundAt(6, &pos, 3, 0, nullptr), 250.0f);   // 0x0003FEA0(6, pos, 3, 0, 0); 0x437A0000
}

void ToxicWarrior4::UpdateAnimation(f32 dt) {         // orig: 0x00091180
    if (!model) return;
    if (b508) b508 = 0;                               // byte +0x508
    if (IsDead()) {                                   // vfA8
        if (!IsAnimPlaying(0x4E)) PlayAnimById(0x4E, true, false, true, 0.0f);   // 0x0003CE70 (pushes 0, 1, 0, 1, id)
        animUpdated = 0;                              // +0x9F
    } else if (attack1) {                             // +0x506
        if (!IsAnimPlaying(0x2F)) PlayAnimById(0x2F, true, false, true, 0.0f);
        animUpdated = 0;
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

// ---- WeaselRoll (section W) ----------------------------------------------------------------------------------------
WeaselRoll::WeaselRoll(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00093510 (vtbls 0x002A0D18 / 0x002A0D04 / 0x002A0D00)
    SetScale(1.0f);                                   // 0x00035080 (no change: 3.5)
    // +0x8C0..+0x8C4 / +0x8C8 = 0 (the member defaults); +0x8CC is not written (KNOWN_ISSUES: 0 under emulation)
}

WeaselRoll::~WeaselRoll() {                           // orig: 0x00093600
    WeaselRoll::UnloadModel();                        // its body inline: Voice_Exists(+0x8CC) unguarded, Voice_Stop, 0; 0x000466A0
    StopVoiceAndClear(voice8CC);                      // 0x0009366C: the guarded stop again (finds the cleared field)
}

void WeaselRoll::UnloadModel() {                      // orig: 0x0008D940
    StopVoiceIfExists(voice8CC, true);                // (Voice_Exists(+0x8CC) without a null test; Voice_Stop, 0)
    GenericNpc::UnloadModel();                        // jmp 0x000466A0
}

void WeaselRoll::BuildModel() {                       // orig: 0x000933F0 (Weasel's)
    const bool first = !model;
    GenericNpc::BuildModel();
    if (model && first && physic) physic->SetBone("Weasel_01:Animation (Translational)");   // vf54 0x00037820
}

void WeaselRoll::OnTextKey(f32 keyTime, const char* text) {   // orig: 0x00093370 (Weasel's)
    if (crt::StrICmp(text, "morph:lfdown") == 0 && IsAnimPlaying(0x15)) {
        const NiPoint3 p = GetWorldPosition();        // vf34
        VoiceMaxDistance(SoundAt(0x33, &p, 3, 0, nullptr), npcDef().value198);       // 0x0003FEA0 (rand pick)
    }
    GenericNpc::OnTextKey(keyTime, text);             // 0x00047550
}

// (npc_combat2_port.md 4.2; checked against the assembly)
void WeaselRoll::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x00093D40
    // a rolling weasel is hurt only by Bender's charge punch (charge_punch_fall 0xD2 / charge_punch_impact 0xD3)
    if (attacker && level.IsCharacter(attacker) && attacker->def->IsType(2) &&
        (attacker->IsAnimPlaying(0xD2) || attacker->IsAnimPlaying(0xD3))) {
        GenericNpc::Damage(amount, attacker);         // 0x00047240
        return;
    }
    if (!rolling) GenericNpc::Damage(amount, attacker);   // byte +0x8C1
}

void WeaselRoll::ApplyHit(GameObject* t, NiPoint3& dir, u32 n) {   // orig: vfD4 0x00093E80
    if (!rolling) {                                   // byte +0x8C1
        GenericNpc::ApplyHit(t, dir, n);              // 0x00049B90
        return;
    }
    if (!t || !curSeq) return;
    const AnimEntry* e = EntryOf(curSeq);             // CharacterDef::FindAnimationBySequence 0x0003FD20
    if (!e) return;
    const f32 dmg = e->hasDamage ? e->damage : 0.0f;  // AnimEntry::GetDamage 0x0003EE90 (FSTP)
    if (t->def->IsType(1)) {                          // a character
        auto* c = static_cast<Character*>(t);
        (void)GetWorldRotation();                     // (vf38 runs twice; the first result is unused)
        const NiPoint3 f = ni::MulVector(GetWorldRotation(), {0.0f, 0.0f, 1.0f});   // vf38 * UZ (0x00011770)
        if ((double(f.y) * dir.y + double(f.z) * dir.z) + double(f.x) * dir.x > 1.0f)   // [0x00293A98]; TEST AH,0x41; JNE
            dir = {F(double(f.x) * -1.0f), F(double(f.y) * -1.0f), F(double(f.z) * -1.0f)};   // FMUL [0x00293BF0]
        else
            dir = f;
        if (!c->body || !c->body->supported) return;  // (t+0x5C)->vf48
        const NiPoint3 p = GetWorldPosition();        // vf34
        AddVelocity(*c->body, {F(double(dir.x) * 150.0f), F(double(dir.y) * 150.0f), F(double(dir.z) * 150.0f)});   // [0x002976EC]
        if (n & 1) c->Damage(dmg, this);              // vf7C
        if (c->def->IsType(2)) VoiceMaxDistance(SoundAt(0x17, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; Voice_SetMaxDistance(D+0x198)
        if (c->def->IsType(2)) VoiceMaxDistance(SoundAt(c->health > 0.0f ? 0x12 : 0x13, &p, 3, 0, nullptr), npcDef().value198);   // TEST AH,0x41; JNE
    } else if (t->def->IsType(0x40)) {
        if (auto* d = dynamic_cast<Destructable*>(t)) (void)d->Damage(dmg);   // vf64(dmg) 0x00094103 (no n & 1 test)
    }
}

void WeaselRoll::HitSpheres(u32 n) {                  // orig: vfD8 0x00093D10
    if (rolling) HitSweep("center", npcDef().value174, npcDef().value174, n, false);   // 0x0003E220("center", D+0x174, n)
}

// orig: the charge-punch hit's effect of 0x00093DC0 / 0x00093430: the "Rotating_Gloria" billboard at vf78 following
// the "Head" node (0x0004F650: rand() % 100).
static void SpawnGloria(GenericNpc& c, f32 scale) {
    Def* d = c.level.defs ? c.level.defs->Find("Rotating_Gloria") : nullptr;   // Session::FindDef 0x000C3F10
    if (!d || !c.model) return;                       // +0x564
    const NiPoint3 p = c.CharacterPoint();            // vf78
    (void)c.GetWorldRotation();                       // vf38 (unused)
    auto* idef = dynamic_cast<ImpactDef*>(d);
    if (Impact* o = idef ? idef->SpawnBillboard(c.level, &c.inventory, p, "AJAJAJ") : nullptr) {   // 0x0004F650
        SetNodeScale(*o, scale);                      // 0x00073720
        o->SetFollow(GetObjectByName(c.node.Get(), "Head"));   // node vf54 (a NiPointer temp); 0x0004F580
    }
}

void WeaselRoll::Vf80(Character* attacker) {          // orig: 0x00093DC0
    (void)attacker;
    if (dizzy) return;                                // byte +0x8C3
    dizzy = 1;
    SpawnGloria(*this, 1.0f);
}

void Weasel::Vf80(Character* attacker) {              // orig: 0x00093430
    (void)attacker;
    dizzy = 1;                                        // byte +0x8C0 (no test)
    SpawnGloria(*this, 1.5f);                         // 0x3FC00000
}

void WeaselRoll::Think(f32 dt) {                      // orig: 0x00094130 (no SetForce at all)
    (void)dt;
    if (!model) return;
    b7BE = 0;                                         // (stored before the +0x28 branch)
    if (flag28) return;
    if (cameraDistance > updateDistance) return;      // FCOMP; TEST AH,0x41; JE
    AiTick();                                         // vfF0
    if (rolling) {                                    // +0x8C1
        moveY = 1.0f;                                 // +0x4DC
        moveX = F(double(moveX) * 0.1f);              // +0x4D8, FMUL [0x00293AB0]
    }
    if (!(flag28 || IsDead() || !hasControl)) {
        const GenericNPCDef& d = npcDef();
        const f32 a = F(double(d.rotSpeed) * moveX * g_frameStep);   // always (no zero test)
        ni::NiMatrix3 rot = ni::Multiply(rotation, ni::MakeYRotation(a));   // 0x001030B0, 0x00013B30
        const NiPoint3 X = ni::MulVector(rot, {1, 0, 0}), Z = ni::MulVector(rot, {0, 0, 1});
        const double zx = double(Z.x) * strafe;
        const f32 ay = F(double(Z.y) * strafe), az = F(double(Z.z) * strafe);
        const f32 bx = F(double(X.x) * moveY), by = F(double(X.y) * moveY), bz = F(double(X.z) * moveY);
        NiPoint3 vel{F(double(bx) + zx), F(double(by) + ay), F(double(bz) + az)};
        const double len = ni::Unitize(vel);          // 0x000116A0 (ST0, never stored)
        const double run = rolling ? double(d.runSpeed) * 1.75f : double(d.runSpeed);   // FMUL [0x00293C28] (unrounded)
        double speed = run;                           // > 1: FCOM [0x00293A98]; TEST AH,0x41; JE
        if (!(len > 1.0f)) {
            if (len <= 0.25f) speed = d.sneakSpeed;   // [0x00293B50]; TEST AH,0x41; JP (NaN: the next test)
            else if (!(len >= 0.75f)) speed = d.walkSpeed;   // FCOMP [0x00293A94]; TEST AH,1; JE (NaN: walk)
        }
        vel = {F(double(vel.x) * speed), F(double(vel.y) * speed), F(double(vel.z) * speed)};
        Move(vel, rot);                               // vfF4
    }
    PostThink();                                      // vfF8
}

void WeaselRoll::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x000936C0
    const NiPoint3 z{};                               // a local copy of 0x002F915C
    auto hold = [&] {
        if (finished) {
            EndRootMotion();                          // 0x000369E0
        } else {
            rot = ni::NiMatrix3{};                    // the identity at 0x002D7820 (12 dwords)
            vel = z;
            body->velocity.y = 0;                     // body+0x2C
        }
    };
    if (IsAnimPlaying(0xD4) || IsAnimPlaying(0xD6)) hold();
    if (IsAnimPlaying(0x4D) || IsAnimPlaying(0x50)) hold();
    if (rolling) {
        heightOffset = F(double(node->local.scale) * npcDef().boundHeight * 0.5f);   // FLD; FMUL [D+0x94]; FMUL [0x00293AA0]
    } else if (!(IsAnimPlaying(0xD6) || IsAnimPlaying(0xD4))) {
        SetRotation(rot);                             // vf24
        if (model) model->local.translate = z;        // model+0x60
    }
    if (dizzy) vel = z;                               // +0x8C3
    if (IsIdling() || IsAnimPlaying(0xD) || IsAnimPlaying(0x15) || IsAnimPlaying(0x19) || IsAnimPlaying(0x1A) ||
        IsAnimPlaying(0xD5))
        AddVelocity(*body, vel);                      // vf50 0x000AF7D0
}

void WeaselRoll::UpdateAnimation(f32 dt) {            // orig: 0x000938B0
    if (!model) return;
    const u8 wasRolling = rolling;
    stopRoll = 0;
    rollRequest = 0;
    if (IsDead()) {                                   // vfA8
        rolling = 0;
        const s32 anim = deathKind == 3 ? 0x4E : deathKind == 2 ? 0x4F : 0x4D;   // +0x778
        if (!IsAnimPlaying(anim)) PlayAnimById(anim, true, false, true, 0.0f);   // (pushes 0, 1, 0, 1, id)
        animUpdated = 0;
    } else if (flattened) {                           // +0x8C4
        if (!IsPlayingName("flatten")) PlayAnimation(FindSequence("flatten"), true, false, true, 0.0f);   // 0x002A0CB4
        if (finished) flattened = 0;
        else animUpdated = 0;
    } else if (dizzy) {                               // +0x8C3
        if (!IsPlayingName("dizzy")) {                // 0x00296264
            DeleteInventoryEffects();                 // 0x00034A50
            rollRequest = stopRoll = rolling = 0;
            PlayAnimation(FindSequence("dizzy"), true, false, true, 0.0f);
            animUpdated = 0;
        } else if (finished) {
            dizzy = 0;
        } else {
            animUpdated = 0;
        }
    } else if (b508) {                                // +0x508 (the attack request)
        if (!rolling && !IsDead()) {
            rollRequest = 1;
            rollTimer = 1.5f;                         // 0x3FC00000
        } else {
            b508 = 0;
        }
    } else {
        const double x = double(rollTimer) - g_frameStep;   // FLD; FSUB [0x002D67E8]; FST
        rollTimer = F(x);
        if (rolling && x <= 0.0f) stopRoll = 1;       // FCOMP 0 (unrounded); TEST AH,0x41; JP (NaN: no)
        if (rollTimer < 0.0f) rollTimer = 0;          // TEST AH,5; JP
    }
    if (animUpdated) {
        if (stopRoll) {
            rolling = 0;
            if (!IsAnimPlaying(0xD6)) {
                PlayAnimById(0xD6, true, false, true, 0.0f);
                BeginRootMotion();                    // 0x000368A0
                HitEnd();                             // vfE0
            }
            animUpdated = 0;
        } else if (rolling) {
            if (!IsAnimPlaying(0xD5)) {
                voice8CC = static_cast<u32>(SoundAt(0x34, nullptr, 3, 0, node.Get()));   // 0x0003FFC0 (rand pick)
                VoiceMaxDistance(static_cast<s32>(voice8CC), npcDef().value198);   // 0x00093B3F
                PlayAnimById(0xD5, false, true, true, 0.0f);   // (pushes 0, 1, 1, 0, 0xD5)
                HitStart();                           // vfDC
            }
            animUpdated = 0;
        } else if (rollRequest) {
            rolling = 1;
            if (!IsAnimPlaying(0xD4)) {
                PlayAnimById(0xD4, true, false, true, 0.0f);
                const NiPoint3 p = GetWorldPosition();   // vf34
                VoiceMaxDistance(SoundAt(0x5C, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0 (rand pick; Voice_SetMaxDistance D+0x198)
                BeginRootMotion();
                HitEnd();                             // vfE0
                if (Def* wd = level.defs ? level.defs->Find("Weasel_Wind") : nullptr) {   // 0x000C3F10 (0x002A0E2C)
                    const NiPoint3 wp = GetWorldPosition();                    // vf34 (0x00093BFE)
                    const NiPoint3 f = ni::MulVector(GetWorldRotation(), {1.0f, 0.0f, 0.0f});   // vf38 * UX 0x00011770
                    auto* idef = dynamic_cast<ImpactDef*>(wd);
                    if (Impact* o = idef ? idef->SpawnUp(level, &inventory, wp, f, nullptr) : nullptr) {   // 0x0004EE90
                        o->SetFollow(node.Get());                              // 0x0004F580 (a NiPointer temp of +0x2C)
                        SetNodeScale(*o, 1.0f);                                // 0x00073720
                        o->SetPosition({0.0f, 6.0f, 0.0f});                    // vf1C (0x40C00000)
                    }
                }
            }
            animUpdated = 0;
        }
    }
    if (!rolling) {
        StopVoiceIfExists(voice8CC, true);            // 0x00093C9F (no null test)
        if (wasRolling) {
            const NiPoint3 p = GetWorldPosition();    // vf34
            heightOffset = F(double(node->local.scale) * npcDef().boundHeight);   // no 0.5 here
            SetPosition(p);                           // vf1C with the new height offset
        }
    }
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

// ---- Junkbot2 (section J) ------------------------------------------------------------------------------------------
Junkbot2::Junkbot2(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x000886F0 (vtbls 0x0029EED8 / 0x0029EEC0 / 0x0029EEBC)
    SetScale(1.0f);                                   // 0x00035080 (after +0x8C0 = 0)
}

void Junkbot2::BuildModel() {                         // orig: 0x00088CC0 (Junkbot's)
    const bool first = !model;
    GenericNpc::BuildModel();
    if (model && first && physic) physic->SetBone("junkbot_root");   // string 0x0029EEAC
}

void Junkbot2::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x000885C0 (Junkbot's)
    JunkbotDamage(*this, amount, attacker);
}

void Junkbot2::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x000887D0
    directRoot = 0;                                   // +0x638
    if (!IsAnimPlaying(0xC5) && !IsAnimPlaying(0xC6) && !IsAnimPlaying(0xC7) && !IsAnimPlaying(0xC8)) {
        EndRootMotion();                              // 0x000369E0
        body->SetForce({0.0f, -100.0f, 0.0f});        // body vf10(&0x002D682C): overrides the think's SetForce(0)
    } else {
        body->velocity.y = 0;                         // body+0x2C
        if (finished) EndRootMotion();
    }
    if (IsAnimPlaying(0) || IsAnimPlaying(0xCA) || IsAnimPlaying(0xCB)) {   // (unlike Junkbot: conditional)
        SetRotation(rot);                             // vf24
        AddVelocity(*body, vel);                      // vf50 0x000AF7D0
    }
}

void Junkbot2::UpdateAnimation(f32 dt) {              // orig: 0x000888E0
    if (!model) return;
    bool hurt = false;
    if (!(health4F8 == health)) {                     // TEST AH,0x44; JNP (NaN enters)
        hurt = health < health4F8;                    // TEST AH,5; JP
        f7A8 = 0;
    }
    health4F8 = health;
    if (deployed) {                                   // +0x8C0
        timer7A4 = animTime;                          // +0x7A4 = +0x78
        bool clear = false;
        if (IsDead()) {
            if (!IsAnimPlaying(0xC6)) {
                PlayAnimation(SeqById(0xC6), true, false, true, 0.0f);   // 0x0003A130 (pushes 0, 1, 0, 1)
                BeginRootMotion();
            }
            clear = true;
        } else if (IsAnimPlaying(0xCC) && !finished) {
            clear = true;
        } else if (IsAnimPlaying(0xC5)) {
            if (finished) {
                const s32 anim = b7C2 ? (IsDead() ? 0xC6 : 0xC7) : 0xC8;   // +0x7C2
                PlayAnimById(anim, true, false, false, 0.0f);   // (pushes 0, 0, 0, 1, id)
                BeginRootMotion();
                animUpdated = 0;
            }
            if (hurt) b7C2 = 1;
        } else if (b508) {
            if (finished) {
                if (!IsAnimPlaying(0xCD)) PlayAnimation(SeqById(0xCD), true, false, true, 0.0f);
                deployed = 0;
                clear = true;
            }
        } else if (attack1) {                         // +0x506
            // 0x00088A86: JNE 0x00088A9E while 0xCB plays, else +0x9B (object_gaps.md 19.1: the port had the test
            // inverted, so a deployed junkbot whose 0xCB still ran never attacked)
            if (IsAnimPlaying(0xCB) || finished) {
                if (!IsAnimPlaying(0xC5)) {
                    PlayAnimation(SeqById(0xC5), true, false, true, 0.0f);
                    BeginRootMotion();
                    b7C2 = 0;
                    const NiPoint3 p = GetWorldPosition();   // vf34
                    VoiceMaxDistance(SoundAt(0xB, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0 (rand pick; Voice_SetMaxDistance D+0x198)
                }
                clear = true;
            }
        }
        if (clear) animUpdated = 0;
        if (animUpdated && finished) PlayAnimation(SeqById(0xCB), true, false, false, 0.0f);   // (pushes 0, 0, 0, 1)
        AnimTick(dt);                                 // 0x0003B5E0
        return;
    }
    if (IsDead()) {                                   // 0x00088B52
        if (!IsAnimPlaying(0xC6)) {
            PlayAnimation(SeqById(0xC6), true, false, true, 0.0f);
            BeginRootMotion();
        }
        animUpdated = 0;
        AnimTick(dt);
        return;
    }
    bool testFinished = true;
    if (attack1) {
        if (!IsAnimPlaying(0xCD) || finished) {       // (0x00033E20(0xCD) is called twice)
            PlayAnimation(SeqById(0xCC), true, false, false, 0.0f);   // (pushes 0, 0, 0, 1)
            deployed = 1;
            animUpdated = 0;
            AnimTick(dt);
            return;
        }
        testFinished = false;                         // (0x00088BD9 -> 0x00088C1D: only the curSeq test)
    }
    if (((testFinished && finished) || !curSeq) && animUpdated) {   // +0x9B / +0xA4 / +0x9F
        ni::NiSequence* ca = SeqById(0xCA);
        if (double(animTime) - timer7A4 < 5.0f && ca) {   // FLD +0x78; FSUB +0x7A4; FCOMP [0x00293C24]; TEST AH,5; JP
            if (!IsAnimPlaying(0xCA) || finished) PlayAnimation(ca, false, true, true, 0.0f);   // (pushes 0, 1, 1, 0)
        } else if (!IsAnimPlaying(0) || finished) {
            PlayAnimById(0, false, true, false, 0.0f);    // 0x0003CE70 (pushes 0, 0, 1, 0, 0)
        }
    }
    AnimTick(dt);                                     // 0x0003B5E0
}

// ---- Destructor (docs/analysis/npc_classes_batch2_port.md D) -------------------------------------------------------
Destructor::Destructor(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00082220 (vtbls 0x0029DF70 / 0x0029DF58 / 0x0029DF54)
    body->b1C = 1;                                    // +0x1C: shape +0xB9 at integrate (immovable in CharChar)
    body->b1A = 1;                                    // +0x1A
    body->noRay = 1;                                  // +0x1D
    vortex = std::make_unique<phys::CharBody>(5.0f, 5.0f, 5.0f,   // new(0x48) 0x000822A8; CharBody 0x000AF0D0
                                              lvl.physic ? &lvl.physic->world : nullptr);   // (the mass 10.0f: unread)
    // (H) +0x19 / +0x45 of the new body and its shape's +0x9C are never written (heap garbage, KNOWN_ISSUES #41; +0x45
    // is written by the first integrate before any read, +0x19 has no reader, +0x9C is read by the first integrate)
    vortex->SetEnabled(true);                         // vf34(1): the ctor already enabled and listed the shape
    vortex->b1A = 1;                                  // +0x1A
    vortex->integratePinned = 1;                      // +0x1B
    vortex->SetForce({0.0f, 0.0f, 0.0f});             // vf10 (a local copy of 0x002F915C)
    vortex->owner = this;                             // vf28
    // +0x8C4 = 0, +0x8C8 = 0, +0x8CC = 0 (the member initialisers)
}

void Destructor::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x000827A0
    body->noRay = 1;                                  // +0x1D (UpdateBodyShape cleared it this step)
    body->b1A = 1;                                    // +0x1A
    GenericNpc::Move(vel, rot);                       // jmp 0x00090F50
}

void Destructor::UpdateAnimation(f32 dt) {            // orig: 0x000827C0
    if (!model) return;
    if ((IsPlayingName("fire_rocket_1") || IsPlayingName("fire_rocket_2") || IsPlayingName("fire_rocket_3") ||
         IsPlayingName("fire_rocket_4") || IsAnimPlaying(0xF6) || IsAnimPlaying(0xF5)) &&
        !finished)                                    // byte +0x9B
        animUpdated = 0;                              // +0x9F
    AnimTick(dt);                                     // 0x0003B5E0 (never 0x00048190)
    if (ni::NiAVObject* v = GetObjectByName(node.Get(), "vortex")) {   // node vf54 (0x0029E120)
        UpdateNodeAt(node.Get(), animTime);           // NiAVObject::Update(node, +0x78, 1) 0x001018D0
        NiPoint3 p = v->world.translate;              // +0xA0 (dword copies)
        p.y = F(double(p.y) + 15.0f);                 // FLD; FADD [0x00297250]; FSTP
        vortex->SetPosition(p);                       // vf04 0x000AEC90
    }
    if (IsPlayingName("door_open")) {
        if (!voice880) {
            voice880 = static_cast<u32>(SoundAt(0x5C, nullptr, 1, 0, node.Get()));   // 0x0003FFC0(0x5C, node, 1, 0)
            VoiceMaxDistance(static_cast<s32>(voice880), npcDef().value198);   // 0x000828E4
        }
    } else {
        voice880 = 0;                                 // (no Voice_Stop)
    }
    if (missilesShown) {                              // byte +0x8C8
        const double x = double(missileTimer) - g_frameStep;   // FLD; FSUB [0x002D67E8]; FST
        missileTimer = F(x);
        if (x < 0.0 || x == 0.0) {                    // FCOMP 0 (unrounded); TEST AH,0x41; JP
            missileTimer = 0;
            rocket8C4 = 0;
            missilesShown = 0;
        }
        for (const char* n : {"missile1", "missile2", "missile3", "missile4"})
            if (ni::NiAVObject* o = GetObjectByName(node.Get(), n))   // node vf54
                o->local.scale = F(std::fabs(1.0 - double(missileTimer)));   // FLD 1.0; FSUB; FABS; FSTP +0x6C
    }
}

void Destructor::Damage(f32 amount, Character* attacker) {   // orig: vf7C 0x00082590 (object_gaps.md 11)
    if (level.IsCharacter(attacker) && !attacker->def->IsType(2)) {   // 0x00033EE0; vf0C(2): an NPC attacker
        const bool mine = agent && agent->profile[17];                       // A->0x0002CB00()+0x44
        const bool theirs = attacker->agent && attacker->agent->profile[17];
        if (!mine && !theirs) return;                 // 0x0008278B (the attacker is not cleared otherwise)
    }
    const NiPoint3 cp = CharacterPoint();             // vf78 (the bounce sound's position)
    bool bounce;
    if (IsPlayingName("door_open") && double(animTime) - animStart < 1.1f) {   // 0x0029E0E4; [0x00295644]; TEST AH,5; JP
        bounce = true;
    } else if (IsPlayingName("door_close") && double(animTime) - animStart > 0.25f) {   // 0x0029E0D8; [0x00293B50]
        bounce = true;                                // TEST AH,0x41; JE
    } else {
        bounce = !IsPlayingName("door_open") && !IsPlayingName("door_close") && !IsPlayingName("door_idle");
    }
    if (bounce) {
        VoiceMaxDistance(SoundAt(3, &cp, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; Voice_SetMaxDistance(D+0x198)
        return;
    }
    Character::Damage(amount, attacker);              // 0x000354B0
    if (amount > 0.0f) {                              // FCOMP [0x00293AB8]; TEST AH,0x41; JNE
        if (!(health > 0.0f) && !std::isnan(health)) {   // TEST AH,0x41; JP ('>' / NaN: the hurt sound)
            if (VoiceLive(voice874)) level.Sound().StopVoice(static_cast<s32>(voice874));   // 0x000826EA (no clear)
            if (!voice884) {
                const NiPoint3 p = GetWorldPosition();   // vf34
                voice884 = static_cast<u32>(SoundAt(0, &p, 3, 0, nullptr));
                VoiceMaxDistance(static_cast<s32>(voice884), npcDef().value198);   // 0x00082764
            }
        } else {
            const NiPoint3 p = GetWorldPosition();
            VoiceMaxDistance(SoundAt(2, &p, 3, 0, nullptr), npcDef().value198);   // (the same SetMaxDistance)
        }
    }
    if (agent && level.IsCharacter(attacker)) agent->OnDamage(amount, attacker);   // 0x00033EE0; 0x0002DEC0
}

void Destructor::OnTextKey(f32 keyTime, const char* text) {   // orig: 0x00082AB0 (npc_combat2_port.md 6)
    if (crt::StrICmp(text, "morph:firew") == 0) {     // 0x002974C0 (0x00082AD7)
        // the rockets: the playing "fire_rocket_<n>" hides "missile<n>" ((+0x2C)->vf54, +0xB0 |= 1), then the shot of
        // D+0x148 from the tip aimed at +0x8D0 and sound 5; no rocket animation: the laser D+0x144 from the tip, two
        // units along the aim. Both: nothing more without a tip ((+0x78C)[0]; the original reads the slot unchecked).
        static const char* const kAnims[4] = {"fire_rocket_1", "fire_rocket_2", "fire_rocket_3",
                                              "fire_rocket_4"};   // 0x0029E0C8 / 0B8 / 0A8 / 098
        static const char* const kMissiles[4] = {"missile1", "missile2", "missile3", "missile4"};   // 0x0029E114 / ..
        u32 k = 0;
        while (k < 4 && !IsPlayingName(kAnims[k])) ++k;   // 0x00039D60
        if (k < 4)
            if (ni::NiAVObject* m = GetObjectByName(node.Get(), kMissiles[k])) m->flags = u16(m->flags | 1);   // OR byte
        ni::NiAVObject* tip = tips.empty() ? nullptr : tips[0];
        if (!tip) return;
        TipToKeyTime(keyTime, tip);                   // 0x00034A10
        aimPos = tip->world.translate;                // +0x7D8 = tip+0xA0
        // (the laser: +0x7F0's 12 dwords copied from tip+0x70 first; the three chunks below overwrite all but the
        // padding)
        // d = unit(+0x8D0 - aimPos) (FSUB, FSTP: 0x00011530 in the laser's path); S = unit((0, 1, 0) x d) (0x002D77EC;
        // inline in the rockets' path, 0x00011700 in the laser's: the same products); T = S x d
        NiPoint3 d = Sub(fireTarget, aimPos);
        (void)ni::Unitize(d);                         // 0x000116A0 (the result popped)
        NiPoint3 s = ni::Cross(NiPoint3{0.0f, 1.0f, 0.0f}, d);
        (void)ni::Unitize(s);
        const NiPoint3 t = ni::Cross(s, d);
        // chunk 0 (+0x7F0) = d, chunk 1 (+0x800) = T, chunk 2 (+0x810) = -S (FCHS) (each chunk a column)
        aimRot.m[0][0] = d.x, aimRot.m[1][0] = d.y, aimRot.m[2][0] = d.z;
        aimRot.m[0][1] = t.x, aimRot.m[1][1] = t.y, aimRot.m[2][1] = t.z;
        aimRot.m[0][2] = -s.x, aimRot.m[1][2] = -s.y, aimRot.m[2][2] = -s.z;
        if (k < 4) {
            Shoot(npcDef().value148, aimPos, aimRot, false, 0.0f);   // vf104(D+0x148, &+0x7D8, &+0x7F0, 0, 0)
            VoiceMaxDistance(SoundAt(5, &aimPos, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; Voice_SetMaxDistance
            return;
        }
        const NiPoint3 v = ni::MulVector(aimRot, NiPoint3{1.0f, 0.0f, 0.0f});   // 0x00011770 (0x002D77E0)
        const NiPoint3 w{F(double(v.x) + v.x), F(double(v.y) + v.y), F(double(v.z) + v.z)};   // FADD ST(0), ST(0)
        aimPos = {F(double(w.x) + aimPos.x), F(double(w.y) + aimPos.y), F(double(w.z) + aimPos.z)};   // 0x000115F0
        Shoot(npcDef().value144, aimPos, aimRot, false, 0.0f);   // vf104(D+0x144, &+0x7D8, &+0x7F0, 0, 0)
        return;
    }
    if (crt::StrICmp(text, "morph:lfdown") == 0 || crt::StrICmp(text, "morph:rfdown") == 0) {   // 0x002974DC / 0x002974CC
        const NiPoint3 p = GetWorldPosition();        // vf34
        VoiceMaxDistance(SoundAt(0x2E, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0 (rand); Voice_SetMaxDistance(D+0x198)
        if (level.player) {                           // S+0x94 (no player: no camera shake either)
            level.player->rumble.Add(0.5f, 0.5f, 0.3f, 0);   // P+0xA04 0x0005F000
            if (level.camera) level.camera->SetShake(0.1f, 0.4f);   // S+0x8C 0x00011290
        }
        return;
    }
    GenericNpc::OnTextKey(keyTime, text);             // 0x00047550
}

// ---- HighPriest (section K.5.2) ------------------------------------------------------------------------------------
HighPriest::HighPriest(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x00087300 (vtbls 0x0029EAF0 / 0x0029EADC / 0x0029EAD8)
    SetScale(1.25f);                                  // 0x00035080(0x3FA00000): the body keeps pos + boundCenter
    body->b1C = 1;                                    // byte body+0x1C
}

void HighPriest::UpdateAnimation(f32 dt) {            // orig: 0x000873A0
    if (!model) return;
    const GenericNPCDef& d = npcDef();
    if (IsDead()) {                                   // vfA8
        if (!(IsAnimPlaying(0x4D) || IsAnimPlaying(0x4E) || IsAnimPlaying(0x4F) || IsAnimPlaying(0x50) ||
              IsAnimPlaying(0x51)) && d.dyingCount > 0) {   // D+0xE0 (JLE, signed)
            const s32 k = crt::Rand() % d.dyingCount + 0x4D;   // orig: rand 0x00087428 (CDQ; IDIV)
            if (SeqById(k)) {
                PlayAnimById(k, true, false, true, 0.0f);   // 0x0003CE70 (pushes 0, 1, 0, 1, k)
                VoiceMaxDistance(SoundAt(1, nullptr, 3, 0, node.Get()), npcDef().value198);   // 0x0003FFC0(1, node, 3, 0); SetMaxDistance(D+0x198)
            }
        }
        animUpdated = 0;                              // 0x000874F7
    } else if (finished || IsIdling() || !curSeq) {   // +0x9B / vfA0 / +0xA4
        if (b509) {
            b509 = 0;                                 // (+0x9F kept)
        } else if (b508) {                            // the close attack (always 0x3B: D+0xF4 not read, no rand)
            b7BC = 0;
            PlayAnimById(0x3B, true, false, true, 0.0f);
            const NiPoint3 p = GetWorldPosition();    // vf34
            VoiceMaxDistance(SoundAt(0xA, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; SetMaxDistance(D+0x198)
            animUpdated = 0;
        }
    }
    if (IsAnimPlaying(0x3B) && !finished) animUpdated = 0;
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190
}

void HighPriest::OnTextKey(f32 keyTime, const char* text) {   // orig: 0x00087530
    GenericNpc::OnTextKey(keyTime, text);             // 0x00047550 first
    if (crt::StrICmp(text, "morph:cast_spell") == 0) {   // 0x0029EC34: a shot from the first tip, 7 up
        if (ni::NiNode* tip = tips.empty() ? nullptr : tips[0]) {   // (+0x78C)[0]
            TipToKeyTime(keyTime, tip);               // 0x00034A10
            aimPos = tip->world.translate;            // +0x7D8 = tip +0xA0
            aimPos.y = F(double(aimPos.y) + 7.0f);    // FLD +0x7DC; FADD [0x0029EC30]; FSTP
        }
        if (agent) agent->Aim(aimPos, aimRot);        // 0x0002DF50(&+0x7D8, &+0x7F0, 1)
        Shoot(npcDef().value144, aimPos, aimRot, false, 0.0f);   // vf104(D+0x144, &+0x7D8, &+0x7F0, 0, 0)
        return;
    }
    if (crt::StrICmp(text, "morph:hit_floor") != 0 || !b870) return;   // 0x0029EC20; byte +0x870
    // 0x00087639: the warp: spell_warp from its position to its top point, a ring of 15 HP_ripples rising up the
    // body, then the move to +0x830
    const NiPoint3 at = GetWorldPosition();           // vf34
    const NiPoint3 top = TopPoint();                  // 0x000345B0
    if (Def* w = level.defs ? level.defs->Find("spell_warp") : nullptr)   // 0x000C3F10 (0x0029EC14)
        (void)w->SpawnEffect(level, at, top);         // vf20(&at, &top) (the original calls it unchecked)
    Def* found = level.defs ? level.defs->Find("HP_ripples") : nullptr;   // 0x0029EC08
    RippleDef* rd = found ? dynamic_cast<RippleDef*>(found) : nullptr;    // (no type test in the original)
    if (!found) rd = NewRippleDef(level, "HP_ripples", "Ripple_white_circle.tga", 0.0f);   // new(0x34) 0x00071710
    if (rd) {
        rd->value24 = {1.0f, 0.0f, 0.0f};             // +0x24 = 1.0, +0x28 = +0x2C = 0
        rd->value30 = 1;                              // byte +0x30
        const f32 growth = rd->value20;               // (+0x20, restored after the ring)
        ni::NiMatrix3 step = ni::MakeYRotation(F(double(kPi) * std::bit_cast<f32>(0x3D888889u)));   // [0x002F9274] *
                                                      //  [0x0029A53C] (pi / 15); FSTP; 0x001030B0
        ni::NiMatrix3 R{};                            // 0x00102FF0 (identity)
        const NiPoint3 ext = body->shape.ext;         // (+0x5C)+0x40 +0x90
        NiPoint3 c = GetWorldPosition();              // vf34
        const f32 dy = F(double(ext.y) * std::bit_cast<f32>(0x3E088889u));   // FMUL [0x0029EC04] (2 / 15)
        // FLD ext.z; FCOMP ext.x; TEST AH,0x41; JNE: ext.x when ext.z <= ext.x (or NaN)
        double r = ext.z > ext.x ? double(ext.z) * 5.0f : double(ext.x) * 5.0f;   // FMUL [0x00293C24] (on the stack)
        f32 i = 0.0f;                                 // [esp+0x10]
        for (;;) {
            const double t = double(i) * std::bit_cast<f32>(0x3D888889u);   // FMUL [0x0029A53C]
            rd->value20 = F(t * 0.75f + 1.0f);        // FLD [0x00293A94]; FMUL ST(1); FADD 1.0; FSTP +0x20
            const double g = t + rd->value24[1];      // FLD ST0; FADD +0x28; FST +0x28; FCOMP 1.0 (unrounded)
            rd->value24[1] = F(g);
            if (g > 1.0f) rd->value24[1] = 1.0f;      // TEST AH,0x41; JNE
            const f32 s = F(t + t + r);               // FADD ST0,ST0; FADD ST(1); FSTP (then FSTP ST0: r popped)
            (void)rd->SpawnRipple(level, owner, c, R, 0.5f, 0.5f, 0.5f, s, s, std::numeric_limits<f32>::max());
            R = ni::Multiply(R, step);                // 0x00013B30 (this = R, the step: R * step)
            c.y = F(double(c.y) + dy);                // FLD c.y; FADD dy; FSTP
            i = F(double(i) + 1.0f);                  // FLD; FADD 1.0; FST
            if (!(i < 15.0f)) break;                  // FCOMP [0x00297250]; TEST AH,5; JNP: loops while < 15
            r = s;                                    // 0x000877BF: FLD [esp+0x14] (the last size)
        }
        rd->value20 = growth;                         // +0x20 restored
    }
    SetPosition(warpPos);                             // vf1C(&+0x830)
    b870 = 0;
}

// ---- SkeletonGuard (section K.5.1) ---------------------------------------------------------------------------------
SkeletonGuard::SkeletonGuard(LevelData& lvl, GenericNPCDef& d, const NiPoint3& pos, const ni::NiMatrix3& rot)
    : GenericNpc(lvl, d, pos, rot) {                  // orig: 0x0008D250 (vtbls 0x0029F9B8 / 0x0029F9A4 / 0x0029F9A0)
    health = maxHealth = 120.0f;                      // +0x4F4 / +0x4FC (0x42F00000); +0x4F8 follows at Init
    SetScale(std::bit_cast<f32>(0x3FB33333u));        // 0x00035080 (1.4f): the height offset 6.3
    Character::SetPosition(pos);                      // 0x000353B0 with the new height offset
}

void SkeletonGuard::Think(f32 dt) {                   // orig: 0x0008D630 (no SetForce, no pinning)
    (void)dt;
    if (!model) return;
    b7BE = 0;
    if (flag28) return;
    if (cameraDistance > updateDistance) return;      // FLD +0x500; FCOMP +0x64; TEST AH,0x41; JE (only '>' returns)
    AiTick();                                         // vfF0
    if (!(flag28 || IsDead() || !hasControl)) {
        const f32 a = F(double(npcDef().rotSpeed) * moveX * g_frameStep);   // FLD D+0x88; FMUL; FMUL; FSTP (always)
        ni::NiMatrix3 rot = ni::Multiply(rotation, ni::MakeYRotation(a));   // 0x001030B0, 0x00013B30
        const NiPoint3 X = ni::MulVector(rot, {1, 0, 0});    // 0x00011770 (0x002D77E0)
        const NiPoint3 Z = ni::MulVector(rot, {0, 0, 1});    // (0x002D77F8)
        const double zx = double(Z.x) * strafe;       // stays on the x87 stack
        const f32 ay = F(double(Z.y) * strafe), az = F(double(Z.z) * strafe);
        const f32 bx = F(double(X.x) * moveY), by = F(double(X.y) * moveY), bz = F(double(X.z) * moveY);
        NiPoint3 vel{F(double(bx) + zx), F(double(by) + ay), F(double(bz) + az)};
        const double len = ni::Unitize(vel);          // 0x000116A0 (ST0)
        // FCOM [0x00293A98]; TEST AH,0x41; JE / FCOMP [0x00293A94]; TEST AH,1; JE: 20 [0x00295AB4], else 10
        // [0x00293C2C] (NaN: 10)
        const f32 speed = (len > 1.0f || len >= 0.75f) ? 20.0f : 10.0f;
        vel = {F(double(vel.x) * speed), F(double(vel.y) * speed), F(double(vel.z) * speed)};
        Move(vel, rot);                               // vfF4 0x0008D340
    }
    PostThink();                                      // vfF8 (GenericNpc 0x00049040)
}

void SkeletonGuard::UpdateAnimation(f32 dt) {         // orig: 0x0008D420
    if (!model) return;
    const NiPoint3 v = body->velocity;                // body vf0C
    NiPoint3 a{v.x, 0.0f, v.z}, b{velBefore.x, 0.0f, velBefore.z};
    const f32 la = F(ni::Unitize(a));                 // 0x000116A0; FSTP
    const double lb = ni::Unitize(b);                 // (ST0)
    if (!(std::fabs(double(la) - lb) > 0.5f)) desiredSpeed = la;   // FSUBR; FABS; FCOMP [0x00293AA0]; TEST AH,0x41; JE
    GenericNpc::UpdateAnimation(dt);                  // 0x00048190 (repeats the test on the same inputs)
}

void SkeletonGuard::Move(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x0008D340
    if (!(IsAnimPlaying(0x3B) || IsAnimPlaying(0x3C))) SetRotation(rot);   // vf24 (not during closeattack1 / 1b)
    if (IsIdling() || IsAnimPlaying(0xD) || IsAnimPlaying(0x15) || IsAnimPlaying(0x19) || IsAnimPlaying(0x1A) ||
        IsAnimPlaying(0x16) || IsAnimPlaying(0x17))
        AddVelocity(*body, vel);                      // vf50
}

void SkeletonGuard::ApplyHit(GameObject* t, NiPoint3& dir, u32 n) {   // orig: vfD4 0x0008D4D0
    if (!t) return;                                   // (no miss sound, unlike 0x00049B90)
    if (t->def->IsType(1)) {
        auto* c = static_cast<Character*>(t);
        const f32 k = (n & 2) ? 150.0f : 75.0f;       // [0x002976EC] / [0x00295A08]
        const NiPoint3 v{F(double(dir.x) * k), F(double(dir.y) * k), F(double(dir.z) * k)};   // dir as given
        if (c->body) AddVelocity(*c->body, v);        // t->body vf50 (no CanBePushed test; a null body crashes)
        if (n & 1) c->Damage(15.0f, this);            // vf7C(0x41700000, this)
        const NiPoint3 p = GetWorldPosition();        // vf34 of the skeleton
        if (c->def->IsType(2)) {
            b7BC = 1;
            VoiceMaxDistance(SoundAt(0x17, &p, 3, 0, nullptr), npcDef().value198);   // 0x0003FEA0; SetMaxDistance(D+0x198)
        }
        if (c->def->IsType(2))
            VoiceMaxDistance(SoundAt(c->health > 0.0f ? 0x12 : 0x13, &p, 3, 0, nullptr), npcDef().value198);   // TEST AH,0x41; JNE (NaN: 0x13)
    } else if (t->def->IsType(0x40)) {
        if (auto* d = dynamic_cast<Destructable*>(t)) (void)d->Damage(15.0f);   // vf64(0x41700000) 0x0008D625
    }
}

// ---- GenericNPCDef::Spawn ---------------------------------------------------------------------------------------
GameObject* GenericNPCDef::Spawn(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                                 const char* label) { // orig: 0x0004A430
    RequestStream();
    const char* n = name.c_str();
    auto is = [&](const char* s) { return crt::StrICmp(n, s) == 0; };
    auto has = [&](const char* s) { return std::strstr(label, s) != nullptr; };   // (no null check in the original)
    GenericNpc* o = nullptr;
    for (const char* nib : {"nibbler_1-2", "nibbler_1-4", "nibbler_1-6", "nibbler_2", "nibbler_3", "nibbler_4",
                            "nibbler_5"})
        if (!o && is(nib)) o = new Nibbler(level, *this, pos, rot);
    if (!o) {
        // the chain order of 0x0004A514..0x0004B381 (game_objects.md 5); unported classes report and return nullptr
        if (is("stormtrooper_mk2")) {                 // orig: 0x0004A514
            o = new StormtrooperMk2(level, *this, pos, rot);   // new(0x8E0) 0x0008FEF0 (-> tail 0x0004B3E5: no variant)
        } else if (is("stormtrooper")) {
            o = new Stormtrooper(level, *this, pos, rot);   // new(0x8D0) 0x00090580 (0x0004A561; no variant)
        } else if (is("chickenwalker")) {             // orig: 0x0004A5AE (0x00295A34)
            o = new ChickenWalkerNpc(level, *this, pos, rot);   // new(0x8D0) 0x00081B80 (-> tail 0x0004B3E5)
        } else if (is("carnivorous_plant")) {
            o = new CarnivorousPlant(level, *this, pos, rot);   // new(0x8C0) 0x000817A0 (0x0004A5F8 -> tail 0x0004B3E5)
        } else if (is("high_priest")) {
            o = new HighPriest(level, *this, pos, rot);   // new(0x8C0) 0x00087300 (0x0004A648 -> tail 0x0004B3E5)
        } else if (is("junkbot")) {                   // orig: 0x0004A692
            if (has(":2")) {
                o = new Junkbot2(level, *this, pos, rot);   // new(0x8D0) 0x000886F0
                o->SetModelVariant(1);                // 0x00033CF0 (0x0004B203)
            } else {
                o = new Junkbot(level, *this, pos, rot);    // new(0x8D0) 0x00088230
                o->SetModelVariant(0);                // 0x00033CF0 (0x0004AB4F)
            }
        } else if (is("patrol_guard")) {              // orig: 0x0004A727 (0x00295A78)
            o = new PatrolGuard(level, *this, pos, rot);   // new(0x8C0) 0x0008F020
            if (has(":2")) {                          // (0x00295638)
                o->SetModelVariant(1);                // 0x00033CF0
                o->heightOffset = o->boundHeight = 1.0f;   // +0x4D0 = +0x4D4 = 0x3F800000 (raw stores, no SetScale)
            } else {
                o->SetScale(std::bit_cast<f32>(0x3F4CCCCDu));   // 0x00035080 (0.8)
                o->SetModelVariant(0);                // 0x00033CF0
            }
        } else if (is("spinbot")) {                   // orig: 0x0004A7FD (0x00295A60)
            o = new SpinBot(level, *this, pos, rot);  // new(0x8D0) 0x0008E8B0 (-> tail 0x0004B3E5: no variant)
        } else if (is("igner_boss")) {                // orig: 0x0004A84A (0x0029761C)
            o = new IgnerBoss(level, *this, pos, rot);    // new(0x8E0) 0x00087960 (-> tail 0x0004B3E5: no variant)
        } else if (is("tentacle_dude")) o = new SewerTentacle(level, *this, pos, rot);    // new(0x8D0) 0x0008C340
        else if (is("tank")) o = new Tank(level, *this, pos, rot);
        else if (is("rockmonster")) o = new RockMonster(level, *this, pos, rot);   // new(0x8C0) 0x0008C1A0 (0x0004A931)
        else if (is("lavaman")) {                     // orig: 0x0004A981 (0x002977A8)
            if (!level.defs->Find("Liquid_LavaMan")) {   // 0x000C3F10 (0x00297798): the first lavaman of the level
                ni::NiNode* root = model ? dynamic_cast<ni::NiNode*>(model.Get()) : nullptr;   // D+0x20, DynamicCast
                ni::NiAVObject* c0 = root && !root->Children().empty() ? root->Children()[0].Get() : nullptr;   // GetAt 0x0001D550
                // child 0 ->vf54("TM_MeshShape:0" 0x00297788) (no null test of the child)
                ni::NiAVObject* m = c0 ? GetObjectByName(c0, "TM_MeshShape:0") : nullptr;
                if (m) {
                    ni::NiPointer<ni::NiAlphaProperty> a;   // GetProperty(0) 0x00101DA0, DynamicCast 0x002F91F0
                    if (ni::NiProperty* pr = GetProperty(*m, 0)) a = dynamic_cast<ni::NiAlphaProperty*>(pr);
                    if (a) a->flags &= ~1u;           // AND byte +0x14, 0xFE (no null test in the original)
                    m->UpdateProperties();            // 0x00101EA0
                    m->UpdateEffects();               // 0x00102190
                    m->UpdateDownwardPass(0.0f, true);   // NiAVObject::Update(0, 1) 0x001018D0
                    auto ld = std::make_unique<LiquidDef>();   // new(0x1C) LiquidDef 0x0006A580("Liquid_LavaMan", 7)
                    ld->name = "Liquid_LavaMan";
                    ld->liquid = 7;
                    level.defs->Add(std::move(ld));   // (Def::Def 0x000962E0 appends itself to the level's list)
                    auto* found = static_cast<LiquidDef*>(level.defs->Find("Liquid_LavaMan"));
                    liquid1A4 = SpawnLiquid(level, *found, &level, dynamic_cast<ni::NiGeometry*>(m), 0, 0, 0);   // 0x0006B490
                }
            }
            o = new LavaMan(level, *this, pos, rot);  // new(0x8E0) 0x00088D40 (-> tail 0x0004B3E5: no variant)
        } else if (is("turdman")) {                   // orig: 0x0004AAF4 (0x00297780)
            if (has(":2")) {                          // (0x00295638)
                o = new TurdManBig(level, *this, pos, rot);   // new(0x8D0) 0x000915A0
                o->SetModelVariant(0);                // 0x00033CF0 (0x0004AB4F)
            } else {
                o = new Turdman(level, *this, pos, rot);   // new(0x8C0) 0x00091210 (0x0004AB67)
                o->SetModelVariant(1);                // 0x00033CF0 (0x0004B203)
            }
        } else if (is("goonbox")) {                   // orig: 0x0004AB9F (0x00297778)
            o = new Goon(level, *this, pos, rot);     // new(0x8C0): ":4" 0x00086C40, else 0x00085EB0
            o->SetModelVariant(has(":4") ? 3 : has(":3") ? 2 : has(":2") ? 1 : 0);   // 0x00033CF0 (the strstr order)
        } else if (is("goonskinny")) {                // orig: 0x0004ACC9 (0x00297764)
            o = new Goon(level, *this, pos, rot);     // new(0x8C0) 0x00085FB0
            o->SetModelVariant(0);                    // 0x00033CF0 (0x0004AB4F)
        } else if (is("goonsneak")) {                 // orig: 0x0004AD16 (0x002976A8)
            o = new GoonSneak(level, *this, pos, rot);   // new(0x8E0) 0x00086090 (-> tail 0x0004B3E5: no variant)
        } else if (is("aquafish")) {                  // orig: 0x0004AD63 (0x00297450)
            o = new AquaFish(level, *this, pos, rot);  // new(0x8C0) 0x0007FFA0 (-> tail 0x0004B3E5: no variant)
        } else if (is("warthog")) {                   // orig: 0x0004ADB3 (0x0029775C)
            o = new Warthog(level, *this, pos, rot);  // new(0x8D0) 0x00092730 (-> tail 0x0004B3E5: no variant)
        } else if (is("toxicwarrior")) {              // orig: 0x0004AE00
            if (has(":4")) {
                o = new ToxicWarrior4(level, *this, pos, rot);   // new(0x8D0) 0x00090FF0
                o->SetModelVariant(3);                // 0x00033CF0 (0x0004AE5B)
            } else if (has(":3")) {                   // orig: 0x0004AE73 (0x00297770)
                o = new ToxicWarrior3(level, *this, pos, rot);   // new(0x8C0) 0x00090AE0
                o->SetModelVariant(2);                // 0x00033CF0 (0x0004B12D)
            } else {
                value144 = 4;                         // orig: 0x0004AED2 (the shared definition, before the ":2" test)
                o = new GenericNpc(level, *this, pos, rot);   // new(0x8C0) 0x00047B80
                if (has(":2")) o->SetModelVariant(1); // 0x0004B203 (without ":2": 0x0004B39A, no variant)
            }
        } else if (is("weasel")) {
            if (AnimEntry* e = FindAnimation("roll")) {   // 0x0003F340 (called twice in the original)
                e->hasDamage = true;                  // AnimEntry::SetDamage 0x0003EE60: new(8) the first time
                e->damage = 20.0f;
            }
            if (has(":2")) {
                o = new WeaselRoll(level, *this, pos, rot);   // new(0x8D0) 0x00093510
                o->SetModelVariant(1);                // 0x00033CF0 (0x0004B203): vf68 = 0x0008D940
            } else {
                o = new Weasel(level, *this, pos, rot);   // new(0x8D0) 0x00092F90
            }
        } else if (is("gunturret")) {                 // orig: 0x0004AFE8 (0x00295AB8)
            o = new GunTurret(level, *this, pos, rot);   // new(0x8D0) 0x00086D20 (-> tail 0x0004B3E5: no variant)
        } else if (is("owl")) o = new Owl(level, *this, pos, rot);
        else if (is("statuehead")) {                  // orig: 0x0004B085 (0x00297734)
            o = new StatueHead(level, *this, pos, rot);   // new(0x8D0) 0x0008FA50 (-> tail 0x0004B3E5: no variant)
        } else if (is("skel_hth")) {                  // orig: 0x0004B0D2
            if (has(":3")) {                          // (0x00297770)
                o = new SkeletonGuard(level, *this, pos, rot);   // new(0x8C0) 0x0008D250 (0x0004B0FA)
                o->SetModelVariant(2);                // 0x00033CF0 (0x0004B12D)
            } else if (has(":2")) {                   // (0x00295638)
                o = new SkeletonGuard(level, *this, pos, rot);   // (0x0004B15C)
                o->SetModelVariant(1);                // (0x0004B203)
            } else {
                o = new GenericNpc(level, *this, pos, rot);   // new(0x8C0) 0x00047B80 (0x0004B18C -> 0x0004B39A)
            }
        } else if (is("skel_ranged")) {
            if (has(":2")) {
                o = new SkeletonKamikaze(level, *this, pos, rot);   // new(0x8D0) 0x0008D8C0
                o->SetModelVariant(1);                // 0x00033CF0 (0x0004B203)
            } else {
                o = new SkeletonRanged(level, *this, pos, rot);     // new(0x8C0) 0x0008E330
            }
        } else if (is("destructor")) {
            o = new Destructor(level, *this, pos, rot);   // new(0x8E0) 0x00082220 (0x0004B256 -> tail 0x0004B3E5)
        } else if (is("eyestalk")) {
            o = new EyeStalk(level, *this, pos, rot); // new(0x8D0) 0x00082FA0
            o->SetScale(1.5f);                        // 0x00035080 (0x0004B2E8)
        } else if (is("crocodile")) {                 // orig: 0x0004B306 (0x002955F8)
            auto* c = new Crocodile(level, *this, pos, rot);   // new(0x8D0) 0x00081C10
            c->SetTurned(label && crt::StrICmp(label, "CSewB_2") == 0);   // 0x00081D30 (0x00297708)
            o = c;
        } else {
            o = new GenericNpc(level, *this, pos, rot);   // the default (0x0004B381)
        }
    }
    o->SetOwner(owner);                               // vf10
    if (model) o->Init();                             // vf04
    if (label) o->SetName(label);
    return o;
}

void Destructor::FireMissile(TriggerRec& rec) {      // orig: 0x000823E0
    if (rocket8C4 >= 4) return;                       // +0x8C4 (JGE)
    if (rec.kind == 5 && rec.target) fireTarget = static_cast<Character*>(rec.target)->CharacterPoint();   // vf78
    else fireTarget = level.triggers->Position(rec);  // 0x000DA7E0 -> +0x8D0
    if ((IsPlayingName("fire_rocket_1") || IsPlayingName("fire_rocket_2") || IsPlayingName("fire_rocket_3") ||
         IsPlayingName("fire_rocket_4")) && !finished)   // 0x00039D60 (0x0029E0C8 .. 0x0029E098); +0x9B
        return;
    ++rocket8C4;
    char name[0x100];
    std::snprintf(name, sizeof name, "fire_rocket_%d", static_cast<int>(rocket8C4));   // sprintf 0x0029E088
    PlayAnimation(FindSequence(name), true, false, true, F(-double(g_frameStep)));   // 0x0003A130(seq, 1, 0, 1, -dt)
}

void Destructor::Rearm() {                            // orig: vf114 0x000829D0
    if (missilesShown) return;                        // +0x8C8
    missilesShown = 1;
    for (const char* n : {"missile1", "missile2", "missile3", "missile4"})   // 0x0029E114 / 08 / 0FC / 0F0
        if (ni::NiAVObject* o = GetObjectByName(node.Get(), n)) {   // (+0x2C)->vf54
            o->flags &= 0xFFFE;                       // +0xB0 &= ~1
            o->local.scale = F(std::fabs(0.009999999776482582));   // FLD qword [0x0029E128]; FABS; FSTP +0x6C
        }
    missileTimer = 1.0f;                              // +0x8CC
}

void Destructor::FirePrimary(const char* anim, TriggerRec& rec) {   // orig: 0x000824F0
    if (IsPlayingName(anim) && !finished) return;    // 0x00039D60; +0x9B
    if (rec.kind == 5 && rec.target) fireTarget = static_cast<Character*>(rec.target)->CharacterPoint();   // vf78
    else fireTarget = level.triggers->Position(rec);  // 0x000DA7E0
    PlayAnimation(FindSequence(anim), true, false, true, F(-double(g_frameStep)));   // 0x0003A130(seq, 1, 0, 1, -dt)
}

}  // namespace fut::game

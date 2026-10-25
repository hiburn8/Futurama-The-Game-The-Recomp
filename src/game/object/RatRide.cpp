// The Zoidberg rat ride; see RatRide.h and docs/analysis/rodent_port.md (sections 1.2-2.6, checked against the
// assembly). x87 conventions as in CharacterUpdate.cpp: F() = FSTP to a float, intermediates are double.
#include "game/object/RatRide.h"

#include "core/CrtString.h"
#include "engine/ni/NiCollision.h"
#include "game/GameTime.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/level/Stage.h"
#include "game/level/TriggerManager.h"
#include "game/object/Character.h"
#include "game/object/GenericNpc.h"
#include "game/sound/GameSound.h"

#include <cfloat>
#include <cstdio>
#include <cstring>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

f32 g_ratSteer = 0.0f;                                // orig: DAT_002DDF78

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

f32 Bits(u32 b) {
    f32 f;
    std::memcpy(&f, &b, 4);
    return f;
}

const NiPoint3 kUnitX{1, 0, 0};                       // orig: 0x002D77E0
const NiPoint3 kUnitY{0, 1, 0};                       // orig: 0x002D77EC

// orig: 0x00011700 NiPoint3::Cross (this = a, b): each component a double difference of two float products.
NiPoint3 CrossX87(const NiPoint3& a, const NiPoint3& b) {
    return {F(double(a.y) * b.z - double(a.z) * b.y), F(double(a.z) * b.x - double(b.z) * a.x),
            F(double(a.x) * b.y - double(a.y) * b.x)};
}

// The rotation argument of vfFC: column k at +0x10 * k of the 0x30-byte matrix (0x00011770's layout).
ni::NiMatrix3 FromColumns(const NiPoint3& c0, const NiPoint3& c1, const NiPoint3& c2) {
    ni::NiMatrix3 m;
    const NiPoint3* c[3] = {&c0, &c1, &c2};
    for (int k = 0; k < 3; ++k) {
        m.m[0][k] = c[k]->x;
        m.m[1][k] = c[k]->y;
        m.m[2][k] = c[k]->z;
    }
    return m;
}

// orig: 0x00060F50 / 0x00060F95 (cdecl (i, k, knots, t), ST0): the Cox - de Boor basis on integer knots.
double Basis(const s32* K, s32 i, s32 k, f32 t) {
    if (k == 1) return (double(K[i]) <= t && double(K[i + 1]) > t) ? 1.0 : 0.0;   // FILD/FCOMP: <= then >
    const s32 a = K[i + k - 1], ki = K[i], b = K[i + k], ki1 = K[i + 1];
    if (a == ki) {
        if (b == ki1) return 0.0;
        return Basis(K, i + 1, k - 1, t) * ((double(b) - t) / (b - ki1));   // FILD b; FSUB t; FIDIV (b - ki1)
    }
    if (b == ki1) return Basis(K, i, k - 1, t) * ((double(t) - ki) / (a - ki));   // FILD ki; FSUBR t; FIDIV
    // both terms: the right one first, stored as a float local, then added to the unrounded left one
    const f32 r = F(Basis(K, i + 1, k - 1, t) * ((double(b) - t) / (b - ki1)));
    return Basis(K, i, k - 1, t) * ((double(t) - ki) / (a - ki)) + r;
}

// orig: 0x000610B0 (cdecl (knots, n, order, t, ctrl, out)).
NiPoint3 SplinePoint(const s32* K, s32 n, s32 k, f32 t, const NiPoint3* ctrl) {
    NiPoint3 o{};
    const s32 m = static_cast<s32>(t);                // _ftol 0x00167110 (truncation)
    const s32 lo = m - 5 < 0 ? 0 : m - 5;
    const s32 hi = m + 5 > n ? n : m + 5;
    for (s32 i = lo; i <= hi; ++i) {
        const double w = Basis(K, i, k, t);           // ST0, kept
        o.x = F(w * ctrl[i].x + o.x);
        o.y = F(w * ctrl[i].y + o.y);
        o.z = F(w * ctrl[i].z + o.z);
    }
    return o;
}

// orig: 0x00061140 (cdecl (ctrl, n, knots, order, out, samples)).
void SampleSpline(const NiPoint3* ctrl, s32 n, const s32* K, s32 k, NiPoint3* out, s32 samples) {
    const f32 stepT = F(double(n - k + 2) / (samples - 1));   // FILD; FIDIV; FSTP
    f32 t = 0;
    for (s32 j = 0; j < samples - 1; ++j) {
        out[j] = SplinePoint(K, n, k, t, ctrl);
        t = F(double(stepT) + t);                     // FLD step; FADD t
    }
    out[samples - 1] = ctrl[n];                       // the last sample is the last point
}

// (LinkNode 0x00031C00 / NodeAt 0x00031C20: GenericNpc.h)

}  // namespace

const std::array<u32, 10> RatRide::kCamHeight = {0x4192FB7F, 0x41F4E944, 0x4186B852, 0x4100978D, 0x412F5048,
                                                 0x41CC2B02, 0x4166F2E5, 0x41400000, 0x412C9CE0, 0x41428C15};
const std::array<u32, 10> RatRide::kCamDistance = {0x41D8BBCD, 0x42193852, 0x41F420C5, 0x41F00000, 0x41EE47AE,
                                                   0x42200000, 0x41CB0794, 0x41E447AE, 0x41D9A5E3, 0x41FCD254};

RatRide::RatRide(LevelData& lvl, Player& rider) : level(lvl), player(rider) {}   // orig: 0x00061240

void RatRide::Start() {                               // orig: 0x00062EE0
    if (started) return;                              // (vfEC calls it every step)
    started = 1;
    points.assign(500, {});                           // orig: delete +0x60; new(0x1770) + vector ctor 0x00011740
    pointCapacity = 500;                              // +0x64 = 0x1F4
    pointCount = 0;                                   // +0x5C
    stagesAdded = 0;                                  // +0x74
    for (size_t i = 0; i < level.stages.size(); ++i) AddStage(*level.stages[i]);   // Session::NextStage order
    Camera* cam = level.camera.get();                 // (no null test in the original)
    cam->presetHeight = Bits(0x4192FB7F);             // cam+0x16C = 18.3728
    cam->distance = Bits(0x41D8BBCD);                 // cam+0x168 = 27.0917
    cam->pivotHeight = 10.0f;                         // cam+0x174
    introTime = 0;                                    // +0x44
}

void RatRide::AddStage(Stage& st) {                   // orig: 0x000628E0 (thiscall, RET 4)
    s32 index = 0;                                    // the non-null names before
    for (const auto& nm : names) {
        if (!nm) continue;
        ++index;
        if (std::strcmp(nm->c_str(), st.name.c_str()) == 0) return;   // inline byte compare: case-sensitive
    }
    const size_t slot = names.size();                 // NiTArray::SetAtGrow(size, strdup(name)): new(0x40) + copy
    names.push_back(std::make_unique<std::string>(st.name));
    const AiNet* net = level.FindAINet(st.name.c_str());   // 0x000313E0 (S+0x114)
    const StageWaypoint* wp = net ? st.FindWaypoint("Start") : nullptr;   // 0x000CE450 (called twice)
    if (!net || !wp) {
        names[slot].reset();                          // entry = 0, counts fixed, operator_delete(name)
        if (slot + 1 == names.size()) names.pop_back();
        return;
    }
    const AiNetNode* first = const_cast<AiNet*>(net)->Nearest(wp->position);   // 0x00031D80
    ++stagesAdded;
    // The walk from `first` to the next end node (a node with one link).
    auto next = [&](const AiNetNode* cur, const AiNetNode*& prev) {
        const AiNetNode* nd;
        if (cur->count == 1) {
            prev = cur;
            nd = NodeAt(*net, LinkNode(*cur, 0));     // 0x00031C00 / 0x00031C20
        } else {
            nd = NodeAt(*net, LinkNode(*cur, 0));
            if (nd == prev) nd = NodeAt(*net, LinkNode(*cur, 1));
            prev = cur;
        }
        return nd;
    };
    s32 count = 1;
    {
        const AiNetNode* cur = first;
        const AiNetNode* prev = nullptr;
        do {
            cur = next(cur, prev);
            ++count;
        } while (cur->count != 1);
    }
    const s32 start = pointCount;
    pointCount += count;
    if (pointCapacity < pointCount) {                 // new(pointCount * 12), copy `start` points, delete the old
        points.resize(static_cast<size_t>(pointCount));
        pointCapacity = pointCount;
    }
    {
        const AiNetNode* cur = first;
        const AiNetNode* prev = nullptr;
        for (s32 j = 0; j < count;) {
            points[static_cast<size_t>(start + j)] = cur->pos;   // node +0x44
            cur = next(cur, prev);
            ++j;
            if (cur->count == 1) {
                points[static_cast<size_t>(start + j)] = cur->pos;
                break;
            }
        }
    }
    NiPoint3 before{};
    if (index > 0) before = Eval(t, u);               // (0x00062CA3)
    stageBase[static_cast<size_t>(index)] = sampleCount;   // +0x94[index]
    const s32 nS = 10 * count;
    sampleCount += nS;
    if (sampleCount >= 5000) {                        // printf "Panic! splinepoints ..." and new arrays without a copy
        NotPorted("RatRide::AddStage: more than 5000 spline samples (0x00062CF3)");
        return;
    }
    s32* K = &knots[static_cast<size_t>(stageBase[static_cast<size_t>(index)])];
    const s32 n = pointCount - start - 1;
    K[0] = K[1] = K[2] = 0;
    s32 j = 3;
    for (s32 v = 1; j <= n; ++j, ++v) K[j] = v;
    for (; j <= n + 3; ++j) K[j] = n - 1;             // rep stosd
    SampleSpline(&points[static_cast<size_t>(start)], n, K, 3,
                 &samples[static_cast<size_t>(stageBase[static_cast<size_t>(index)])], nS);   // 0x00061140
    if (index == 0) {
        u = 0;
        t = 0;
        NiPoint3 p0 = Eval(0.0f, 0.0f);
        const NiPoint3 p1 = Eval(1.0f, 0.0f);
        player.SetPosition(p0);                       // vf1C (Character::SetPosition 0x000353B0)
        if (sampleCount > 1) {
            tangent = {F(double(p1.x) - p0.x), F(double(p1.y) - p0.y), F(double(p1.z) - p0.z)};   // (y is kept)
            (void)ni::Unitize(tangent);
        } else {
            tangent = kUnitX;                         // 0x002D77E0
        }
    } else if (index > 1) {                           // (JBE: index 1 does nothing more)
        const NiPoint3 q = Eval(t, u);
        const double dx = double(q.x) - before.x, dy = double(q.y) - before.y, dz = double(q.z) - before.z;
        if ((dx * dx + dz * dz) + dy * dy > 1.0f) FindNearest(before, t, u, 1000.0f);   // TEST AH,0x41; JNE
    }
}

NiPoint3 RatRide::Eval(f32 tt, f32 uu) const {        // orig: 0x000616D0 (thiscall(out, t, u), RET 0xC)
    const NiPoint3 up = kUnitY;                       // local copy of 0x002D77EC
    const s32 i = static_cast<s32>(tt);               // _ftol 0x00167110
    const s32 n = sampleCount;
    if (n == 0) return {};                            // NiPoint3::ZERO
    // the cross product D x UNIT_Y inline: two components kept on the x87 stack, the last one stored
    auto cross = [&](const NiPoint3& D, double& a, double& b, f32& c) {
        a = double(up.z) * D.y - double(D.z) * up.y;
        b = double(D.z) * up.x - double(up.z) * D.x;
        c = F(double(up.y) * D.x - double(D.y) * up.x);
    };
    if (i < n - 1) {
        const NiPoint3& P0 = samples[static_cast<size_t>(i)];
        const NiPoint3& P1 = samples[static_cast<size_t>(i + 1)];
        const f32 f = F(double(tt) - i);              // FILD i; FSUBR t; FSTP (into the t slot)
        NiPoint3 D{F(double(P1.x) - P0.x), F(double(P1.y) - P0.y), F(double(P1.z) - P0.z)};
        (void)ni::Unitize(D);
        double a, b;
        f32 c;
        cross(D, a, b, c);
        // the differences are reloaded: x and y unrounded, z stored as a float first
        const f32 ox = F((double(P1.x) - P0.x) * f);
        const f32 oy = F((double(P1.y) - P0.y) * f);
        const f32 oz = F(double(F(double(P1.z) - P0.z)) * f);
        const f32 x0 = F(double(F(a * uu)) + P0.x);
        const f32 y0 = F(b * uu + P0.y);
        const double z0 = double(c) * uu + P0.z;      // kept on the stack
        return {F(double(x0) + ox), F(double(y0) + oy), F(z0 + oz)};
    }
    NiPoint3 D = kUnitX;                              // 0x002D77E0 (n <= 2)
    if (n > 2) {
        const NiPoint3& A = samples[static_cast<size_t>(n - 3)];
        const NiPoint3& B = samples[static_cast<size_t>(n - 2)];
        D = {F(double(B.x) - A.x), F(double(B.y) - A.y), F(double(B.z) - A.z)};
        (void)ni::Unitize(D);
    }
    double a, b;
    f32 c;
    cross(D, a, b, c);
    const NiPoint3& L = samples[static_cast<size_t>(n - 1)];
    return {F(double(F(a * uu)) + L.x), F(b * uu + L.y), F(double(c) * uu + L.z)};
}

void RatRide::FindNearest(NiPoint3& pos, f32& tt, f32 uu, f32 range) const {   // orig: 0x00061990 (RET 0x10)
    NiPoint3 best = Eval(tt, uu);                     // (the result if nothing scores)
    f32 bestT = tt;
    f32 bestD = FLT_MAX;                              // 0x7F7FFFFF
    f32 t0 = F(double(tt) - range);
    f32 t1 = F(double(range) + tt);
    if (t0 < 0.0f) t0 = 0.0f;                         // TEST AH,5; JP
    const double last = double(sampleCount - 1);      // FILD
    if (double(t1) > last) t1 = F(last);              // TEST AH,0x41; JNE
    if (t0 < t1) {                                    // TEST AH,5; JP
        double nx;
        do {
            const NiPoint3 p = Eval(t0, uu);
            const double dx = double(pos.x) - p.x, dz = double(pos.z) - p.z;   // (dy is stored and not used)
            const double d = dz * dz + dx * dx;
            if (d < bestD) {                          // FCOM [L14]; TEST AH,5; JP
                bestD = F(d);
                best = p;
                bestT = t0;
            }
            nx = double(t0) + 0.5f;                   // FADD 0.5; FST; FCOMP t1: the unrounded sum
            t0 = F(nx);
        } while (nx < t1);                            // TEST AH,5; JNP loop
    }
    pos = best;
    tt = bestT;
}

void RatRide::SetCameraPreset(s32 n) {                // orig: 0x00061680
    const s32 i = n % 10;                             // CDQ; IDIV 10
    if (i < 0) {                                      // (the original reads before the tables)
        NotPorted("RatRide::SetCameraPreset: a negative preset");
        return;
    }
    Camera* cam = level.camera.get();                 // (no null test in the original)
    cam->presetHeight = Bits(kCamHeight[static_cast<size_t>(i)]);    // FLD R+0x1FC[i]; FSTP cam+0x16C
    cam->distance = Bits(kCamDistance[static_cast<size_t>(i)]);      // FLD R+0x224[i]; FSTP cam+0x168
}

// ---- 2.3 the per-step update --------------------------------------------------------------------------------------
void RatRide::Update(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x00061B00 (thiscall(vel*, rot*), RET 8)
    if (!started || !player.model) return;            // +0x04, P+0x564
    time48 = F(double(g_frameStep) + time48);         // +0x48
    const double it = double(g_frameStep) + introTime;
    introTime = F(it);                                // FST
    if (it < 1.0f) {                                  // FCOMP [0x00293A98]; TEST AH,5; JP: the unrounded sum
        UpdateIntro(rot);
        return;
    }
    UpdateMain(vel, rot);
}

void RatRide::UpdateIntro(ni::NiMatrix3& rot) {       // orig: 0x00061B5B..0x00061F1B
    Player& P = player;
    NiPoint3 where = P.GetWorldPosition();            // L34 (vf34)
    FindNearest(where, t, 0.0f, 10.0f);               // 0x00061990(&L34, &+0x3C, &L40 = 0, 10.0)
    const NiPoint3 a = Eval(t, 0.0f);
    where = a;
    const NiPoint3 b = Eval(F(double(t) + 1.0f), 0.0f);   // FLD t; FADD 1.0; FSTP into the argument slot
    tangent = {F(double(b.x) - a.x), F(double(b.y) - a.y), F(double(b.z) - a.z)};
    tangent.y = 0;                                    // +0x6C = 0
    (void)ni::Unitize(tangent);                       // 0x000116A0
    NiPoint3 up{0, 1, 0};                             // L48
    (void)ni::Unitize(up);
    // the side vector inline (up.z*T.y - up.y*T.z, ...): the products are exact, so it equals Cross(T, up)
    NiPoint3 side = CrossX87(tangent, up);            // L28
    (void)ni::Unitize(side);
    rot = FromColumns(tangent, up, side);
    Camera* cam = level.camera.get();                 // S+0x8C
    if (introTime <= g_frameStep && cam) {            // FLD +0x44; FCOMP dt; TEST AH,0x41; JP: the first step only
        const double tx5 = double(tangent.x) * 5.0f;  // 0x00293C24, kept on the stack
        const f32 ty5 = F(double(tangent.y) * 5.0f), tz5 = F(double(tangent.z) * 5.0f);
        where = {F(double(a.x) - tx5), F((double(a.y) - ty5) + 20.0f), F(double(a.z) - tz5)};   // 0x00295AB4
        cam->position = where;                        // SetPosition 0x00011120: +0x48, byte +0xA1 = 1
        cam->dirtyTranslate = 1;
    }
    if (cam && cam->mode == 0) cam->ScriptPlaceBehindTarget();   // 0x00015350 (every intro step)
    if (introTime > 2.5f) P.attack1 = 1;              // 0x00293B10 (+0x506; TEST AH,0x41; JNE)
    // The stage that holds `where` (the camera position on the first step, else the spline point).
    s32 idx = 0;                                      // L40 (zeroed before the room query)
    const ni::NiRoom* room = level.roomGroup ? level.roomGroup->FindRoomContaining(where) : nullptr;   // S+0x20
    for (const auto& st : level.stages) {
        if (!st->ContainsRoom(room)) continue;
        for (size_t k = 0; k < names.size(); ++k)
            if (names[k] && crt::StrICmp(names[k]->c_str(), st->name.c_str()) == 0) {
                idx = static_cast<s32>(k);
                break;
            }
        break;                                        // only the first stage holding the room
    }
    t = static_cast<f32>(stageBase[static_cast<size_t>(idx)]);   // FILD +0x94[idx]; FSTP +0x3C
    u = 0;                                            // +0x40
    // The ride sound (0x00061E89): started muted while no voice exists (under the emulated service every intro step,
    // each with its rand() pick), else faded in by 2 dt per step up to 1.
    sound::GameSound& snd = level.Sound();
    if (!snd.VoiceExists(static_cast<s32>(voice))) {  // Voice_Exists 0x00061E89 (no null test)
        voice = static_cast<u32>(P.SoundAt(0x14, &where, 3));   // def 0x0003FEA0: rand() % count
        snd.SetVolume(static_cast<s32>(voice), 0.0f);   // Voice_SetVolume 0x00061EB3
        return;
    }
    const f32 v = snd.GetVolume(static_cast<s32>(voice));   // Voice_GetVolume 0x00061EC8 (ST0)
    if (!(v < 1.0f)) return;                          // FCOM [0x00293A98]; TEST AH,5; JP (NaN: no fade)
    const double nv = double(g_frameStep) + g_frameStep + v;   // FLD dt; FADD ST0,ST0; FADDP
    f32 out = F(nv);                                  // FST [esp+0x40]
    if (nv > 1.0) out = 1.0f;                         // FCOMP 1.0 (unrounded); TEST AH,0x41; JNE
    snd.SetVolume(static_cast<s32>(voice), out);      // Voice_SetVolume 0x00061F0D
}

void RatRide::UpdateMain(NiPoint3& vel, ni::NiMatrix3& rot) {   // orig: 0x00061F1E..0x000628CF
    Player& P = player;
    phys::CharBody& body = *P.body;
    introTime = 3.0f;                                 // 0x40400000
    {                                                 // the ride sound fades out by 2 dt per step, then stops
        sound::GameSound& snd = level.Sound();
        if (snd.VoiceExists(static_cast<s32>(voice))) {   // Voice_Exists 0x00061F29
            const f32 v = snd.GetVolume(static_cast<s32>(voice));   // 0x00061F39
            if (v > 0.0f)                             // FCOM 0; TEST AH,0x41; JNE (<= 0 or NaN: stop)
                snd.SetVolume(static_cast<s32>(voice), F(double(v) - (double(g_frameStep) + g_frameStep)));   // FSUBR
            else
                snd.StopVoice(static_cast<s32>(voice));   // Voice_Stop 0x00061F72 (+0x14 is kept)
        }
    }
    P.attack1 = 0;                                    // +0x506
    if (P.IsDead()) {                                 // vfA8: the respawn (0x00061F97)
        if (!P.IsDyingAnim()) return;                 // vfAC
        // 0x000373A0("PLAYER" 0x00293BD4): FsIsCharacterSpeaking's test (the record's character, lip-sync state 0)
        if (TriggerRec* rec = level.triggers ? level.triggers->FindOrCreate("PLAYER") : nullptr;
            rec && rec->kind == 5 && rec->target && static_cast<Character*>(rec->target)->lip.state == 0)
            return;
        if (!stage) {                                 // +0x90
            stage = level.stages.empty() ? nullptr : level.stages.front().get();   // Session::NextStage(0) 0x000C26C0
            if (!stage) stage = level.currentStage;  // S+0x5C
        }
        if (!stage) return;
        bool live = false;                            // (port guard: +0x90 keeps the identity of a stage that may be
        for (const auto& st : level.stages) live |= st.get() == stage;   //  unloaded; the original reads it)
        if (!live) {
            NotPorted("RatRide: the respawn with an unloaded stage at +0x90");
            return;
        }
        const StageWaypoint* wp = stage->FindWaypoint("Start");   // 0x000CE450 (0x0029B9C4)
        if (!wp) return;
        const NiPoint3 pos = wp->position;            // esp+0x1C
        const ni::NiMatrix3 R = ni::MakeYRotation(wp->yaw);   // 0x001030B0 (pad lanes zeroed)
        P.Respawn(pos, R);                            // 0x00057930(&pos, &R)
        return;
    }
    bool anyName = false;                             // 0x00062068: the number of non-null names
    for (const auto& nm : names) anyName |= nm != nullptr;
    if (!anyName) return;
    const NiPoint3 pos = P.GetWorldPosition();        // L54
    const ni::NiRoom* room = level.roomGroup ? level.roomGroup->FindRoomContaining(pos) : nullptr;
    for (const auto& st : level.stages)
        if (st->ContainsRoom(room)) {
            stage = st.get();                         // +0x90
            break;
        }
    f32 len = 10.0f;                                  // L40
    if (ni::RayHit h = ni::RayCast(pos, {0, -1, 0}, len, true, 3))
        (void)h.Normal();                             // 0x000FA8C0 into L1C: overwritten at once (dead)
    // The contact bytes (only the first four: the loop is unrolled by 4 and runs once).
    u8 groundOk = 1;                                  // L1B
    bool all10 = true;                                // CL
    s32 n = 0;
    for (; n < 4; ++n) {
        const u8 m = body.shape.contactFlags[static_cast<size_t>(n)];   // shape +0x54
        if (m == 0xFF) break;
        if (m & 0x08) groundOk = 0;
        if (!(m & 0x10)) all10 = false;
    }
    const bool onBox = body.shape.partnerIsBox && body.shape.partner;   // 0x00034590: byte +0xB2 ? +0xB4 : 0
    if (all10 && !onBox && n != 0) {
        groundOk = 0;                                 // (AL of the 0 return)
        P.Damage(F(double(g_frameStep) * 100.0f), nullptr);   // vf7C(F(dt * 100) [0x00295610], 0)
        if (P.health < 0.0f || P.health == 0.0f) P.deathKind = 0x11;   // TEST AH,0x41; JP
    }
    groundDist = len;                                 // +0x28
    // (0x00062268: pad->GetValue(Binding(15)) and pad->GetValue(Binding(14)); both results are dropped)
    if (body.supported && groundOk) {                 // vf48
        airTime = 0;
    } else {
        airTime = F(double(g_frameStep) + airTime);
    }
    if (airTime > 5.0f) airTime = 5.0f;               // TEST AH,0x41; JNE (the stored float)
    // The jump (0x0005DFC0 on a copy of the action map: the same press history).
    if (P.actions.MatchNewest(0x18) && (body.velocity.y < 0.0f || body.velocity.y == 0.0f) && airTime < 0.1f) {
        airTime = 0;
        P.jumpRequest = 1;                            // +0xB89 (the rodent states play jumprun)
        body.velocity.y = 0;                          // body +0x2C
        const NiPoint3 j{0, F(double(jumpSpeed) * Bits(0x3FB33333)), 0};   // [0x0029B9B4] 1.4f
        body.velocity = {F(double(body.velocity.x) + j.x), F(double(j.y) + body.velocity.y),
                         F(double(j.z) + body.velocity.z)};   // vf50 AddVelocity 0x000AF7D0
    }
    NiPoint3 p = P.GetWorldPosition();                // L1C
    FindNearest(p, t, u, 10.0f);                      // 0x00061AE0
    const NiPoint3 C = Eval(t, u);                    // L28
    NiPoint3 d{};                                     // L28 again (ZERO at 0x000624C8)
    if (double(sampleCount) - 1.0f > t) {             // FILD; FSUB 1.0; FCOMP t; TEST AH,0x41; JNE
        t = F(double(step) * speed + t);              // FLD +0x30; FMUL +0x2C; FADD t
        if (!(body.shape.flags & 0x80) && !P.IsDead())   // vf4C, vfA8
            g_ratSteer = F((double(P.moveX) * steerGain) * 0.25f + double(g_ratSteer) * 0.75f);
        if (g_ratSteer > 1.5f) g_ratSteer = 1.5f;     // 0x00293BEC, TEST AH,0x41; JNE
        else if (g_ratSteer < -1.5f) g_ratSteer = -1.5f;   // 0x00293BE8, TEST AH,5; JP
        const double uu = double(g_ratSteer) + u;
        u = F(uu);                                    // FST
        if (uu < -15.0f) u = -15.0f;                  // 0x0029B9C0 (the unrounded sum)
        if (u > 15.0f) u = 15.0f;                     // 0x00297250 (reloaded)
        if (double(sampleCount) - 1.0f > t) {         // TEST AH,0x41; JP 0x0006266D
            const NiPoint3 q = Eval(t, 0.0f);
            tangent = {F(double(q.x) - C.x), F(double(q.y) - C.y), F(double(q.z) - C.z)};
            tangent.y = 0;                            // +0x6C
            (void)ni::Unitize(tangent);
            const NiPoint3 r = Eval(t, u);
            d = {F(double(r.x) - C.x), 0.0f, F(double(r.z) - C.z)};   // (y computed, then 0)
            (void)ni::Unitize(d);
        } else {
            t = static_cast<f32>(sampleCount - 1);    // DEC; FILD; FSTP
        }
    }
    // 0x000624E5: the velocity along d (ZERO at the end of the path)
    const double ax = double(d.x) * step;             // kept on the stack
    const f32 ay = F(double(step) * d.y), az = F(double(step) * d.z);
    vel = {F(ax * speed), F(double(ay) * speed), F(double(az) * speed)};
    u = 0;                                            // +0x40 (every step)
    if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f) {  // TEST AH,0x44; JP x3: the end of the path
        vel = {};
        const NiPoint3 e = Eval(t, u);
        const NiPoint3 w = P.GetWorldPosition();
        f32 len20 = 20.0f;
        const NiPoint3 o{e.x, F(double(w.y) + 10.0f), e.z};   // 0x00293C2C
        const ni::RayHit h = ni::RayCast(o, {0, -1, 0}, len20, true, 3);
        f32 y = w.y;
        if (h && double(w.y) + 3.0f > o.y) y = o.y;   // TEST AH,0x41; JNE (never true: o.y = w.y + 10)
        P.SetPosition({e.x, F(double(y) + 0.5f), e.z});   // vf1C
    } else {
        tangent.y = 0;                                // 0x00062757
    }
    // 0x0006275A: the facing, twisted by the steering
    (void)ni::Unitize(tangent);
    NiPoint3 up{0, 1, 0};                             // L48 (set at 0x0006214B)
    (void)ni::Unitize(up);
    NiPoint3 side = CrossX87(tangent, up);            // 0x00011700
    (void)ni::Unitize(side);
    const f32 s = g_ratSteer;
    tangent = {F(double(F((double(side.x) * s) * 0.25f)) + tangent.x),
               F((double(side.y) * s) * 0.25f + tangent.y),
               F(double(F(double(side.z) * s)) * 0.25f + tangent.z)};
    (void)ni::Unitize(tangent);
    side = CrossX87(tangent, up);
    (void)ni::Unitize(side);
    rot = FromColumns(tangent, up, side);
    body.velocity.x = 0;                              // body +0x28
    body.velocity.z = 0;                              // body +0x30
    body.halveXZ = 0;                                 // vf2C(0) (Control set it to 1 before vfFC)
    vel.y = 0;
    // 0x00011590(vel, 1/60): x and y with the float quotient, z with the unrounded one
    const double invD = 1.0 / double(Bits(0x3C888889));
    const f32 inv = F(invD);
    vel = {F(double(inv) * vel.x), F(double(inv) * vel.y), F(invD * vel.z)};
}

}  // namespace fut::game

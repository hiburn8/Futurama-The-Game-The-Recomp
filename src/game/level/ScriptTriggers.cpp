// The level scripts' trigger objects; see ScriptTriggers.h and docs/analysis/script_triggers.md (section numbers).
#include "game/level/ScriptTriggers.h"

#include "core/CrtString.h"
#include "core/memory/MemoryManager.h"
#include "engine/ni/NiCollision.h"
#include "engine/ni/NiMath.h"
#include "game/defs/CharacterDef.h"
#include "game/input/Pad.h"
#include "game/level/LevelAnimation.h"
#include "game/level/LevelData.h"
#include "game/level/TriggerManager.h"
#include "game/object/Character.h"
#include "game/object/Destructable.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

void Put(script::VmAddr a, u32 v) { std::memcpy(mem::FromVmAddr(a), &v, 4); }

// orig: 0x000B70B0 cdecl(ax, ay, bx, by, px, py, qx, qy, f32* ox, f32* oy): the 2D intersection of the line a-b with
// the line p-q. A vertical line has the slope FLT_MAX (0x00295604); the sentinel tests (TEST AH,0x44; JNP) take it on
// == only. m1 / c1 stay on the x87 stack, m2 / c2 are stored into argument slots (floats).
void LineIntersect2D(f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py, f32 qx, f32 qy, f32& ox, f32& oy) {
    constexpr f32 S = 3.40282347e38f;
    const double dx1 = double(bx) - ax;
    const double m1 = dx1 == 0.0 ? double(S) : (double(by) - ay) / dx1;
    const double dx2 = double(qx) - px;
    const f32 m2 = dx2 == 0.0 ? S : F((double(qy) - py) / dx2);
    const double c1 = m1 == double(S) ? double(S) : double(ay) - double(ax) * m1;
    const f32 c2 = m2 == S ? S : F(double(py) - double(m2) * px);
    const double inv = m1 == double(m2) ? double(S) : 1.0 / ((m1 * -1.0) - (double(m2) * -1.0));   // [0x00293BF0]
    if (c1 == double(S)) {
        if (c2 == S) {
            ox = oy = S;
            return;
        }
        ox = ax;
        oy = F(double(m2) * ax + c2);
        return;
    }
    if (c2 == S) {
        ox = px;
        oy = F(m1 * px + c1);
        return;
    }
    ox = F(((double(c2) * -1.0) - (c1 * -1.0)) * inv);
    oy = F((c1 * m2 - double(c2) * m1) * inv);
}

// orig: 0x000B7B80 cdecl(a, b, p0, p1, out, clip): the segment p0-p1 against the axis-aligned rectangle a..b (one axis
// constant): x and y from the XY projection, x (again) and z from the XZ projection, each against the rectangle's
// diagonal min -> max; then clipped to the rectangle +- 0.01 (FLT_MAX x 3 when outside).
NiPoint3 FaceHit(const NiPoint3& a, const NiPoint3& b, const NiPoint3& p0, const NiPoint3& p1, bool clip) {
    auto lo = [](f32 u, f32 v) { return u < v ? u : v; };   // TEST AH,5; JP -> v (NaN: v)
    auto hi = [](f32 u, f32 v) { return u > v ? u : v; };   // TEST AH,0x41; JNE -> v (NaN: v)
    const NiPoint3 mn{lo(a.x, b.x), lo(a.y, b.y), lo(a.z, b.z)};
    const NiPoint3 mx{hi(a.x, b.x), hi(a.y, b.y), hi(a.z, b.z)};
    f32 x1, y1, x2, z2;
    LineIntersect2D(mn.x, mn.y, mx.x, mx.y, p0.x, p0.y, p1.x, p1.y, x1, y1);
    LineIntersect2D(mn.x, mn.z, mx.x, mx.z, p0.x, p0.z, p1.x, p1.z, x2, z2);   // (x2 overwrites x1)
    NiPoint3 r{x2, y1, z2};
    if (clip) {
        const double e = 0.01f;                       // orig: 0x00293AAC
        const bool out = double(mn.x) - e > r.x || double(mx.x) + e < r.x || double(mn.y) - e > r.y ||
                         double(mx.y) + e < r.y || double(mn.z) - e > r.z || double(mx.z) + e < r.z;
        if (out) r = {3.40282347e38f, 3.40282347e38f, 3.40282347e38f};
    }
    return r;
}

}  // namespace

bool HealthContainsPoint(const ScriptTrigger& t, const NiPoint3& p, f32 margin) {   // orig: 0x000B86F0
    const bool noRadius = std::bit_cast<u32>(t.radius) == 0xBF800000u;
    const bool boxed = !(t.half.y <= 0.0f);           // FCOMP 0; TEST AH,0x41; JP box (> 0 or NaN)
    if (!boxed && noRadius) return false;
    if (!noRadius) {                                  // the sphere (also with a box size): all double
        const double dx = double(p.x) - t.centre.x, dy = double(p.y) - t.centre.y, dz = double(p.z) - t.centre.z;
        const double r = double(margin) + t.radius;
        return r * r >= (dz * dz + dy * dy) + dx * dx;   // FCOMPP; TEST AH,1; JE true (NaN: false)
    }
    const NiPoint3 d{F(double(p.x) - t.centre.x), F(double(p.y) - t.centre.y), F(double(p.z) - t.centre.z)};
    // The original's chunks +0x20 / +0x30 / +0x40 are the port's columns: FLD c0; FLD c1; FLD c2; FMUL d.z; FXCH; FMUL
    // d.y; FADDP; FXCH; FMUL d.x; FADDP; FABS.
    const auto& m = t.rotation.m;
    auto proj = [&](int c) {
        return std::fabs((double(m[2][c]) * d.z + double(m[1][c]) * d.y) + double(m[0][c]) * d.x);
    };
    if (double(margin) + t.half.x < proj(0)) return false;   // chunk +0x20 vs +0x54 (TEST AH,5; JNP false)
    if (double(margin) + t.half.z < proj(2)) return false;   // chunk +0x40 vs +0x58
    if (double(margin) + t.half.y < proj(1)) return false;   // chunk +0x30 vs +0x50
    return true;
}

// orig: HealthTrigger 0x000B7D80 (thiscall(o*, dir*, dist*), RET 0xC): the sphere (centre +0x0C, radius +0x68)
// against the ray; an origin inside gives dist = 0. The x87 order of the listing: every sum and product in double,
// only the stores below round.
static bool HealthRaySphere(const ScriptTrigger& t, const NiPoint3& o, const NiPoint3& dir, f32& dist) {
    const NiPoint3& c = t.centre;
    const double ax = double(c.x) - o.x, ay = double(c.y) - o.y, az = double(c.z) - o.z;
    const f32 r2 = F(double(t.radius) * t.radius);    // FLD r; FLD ST0; FMUL ST1; FSTP [esp]
    if ((az * az + ay * ay) + ax * ax < r2) {         // FCOMP [esp]; TEST AH,5; JP
        dist = 0.0f;
        return true;
    }
    const double p = (az * dir.z + ay * dir.y) + ax * dir.x;   // the projection of c - o on the ray
    if (p < 0.0f) return false;                       // FCOM 0; TEST AH,5; JNP false (NaN goes on)
    const f32 qz = F(p * dir.z);                      // FSTP [esp+0x10]
    const f32 cx = F(p * dir.x + o.x);                // FSTP [esp+0x14]
    const double cy = p * dir.y + o.y;                // (unrounded)
    const double cz = double(qz) + o.z;
    const f32 ex = F(double(cx) - c.x), ey = F(cy - c.y);   // FSTP [esp+8] / [esp+0xC]
    const double ez = cz - c.z;
    if (!((ez * ez + double(ey) * ey) + double(ex) * ex < r2)) return false;   // FCOMP [esp+4]; TEST AH,5; JP
    const double bx = double(o.x) - c.x, by = double(o.y) - c.y, bz = double(o.z) - c.z;
    const double B = (bx * dir.x + bz * dir.z) + by * dir.y;
    const f32 Bf = F(B);                              // FST [esp+0x24]
    const double disc = (B * Bf + r2) - ((bz * bz + by * by) + bx * bx);   // FMUL [Bf]; FADD r2; FSUBP
    const double sq = std::sqrt(disc);                // FSQRT
    const f32 t1 = F(std::fabs(double(Bf) + sq));     // FADD ST1; FABS; FSTP [esp+0x28]
    const double t2 = std::fabs(double(Bf) - sq);     // FSUBR [Bf]; FABS (unrounded)
    const double T = t1 < t2 ? double(t1) : t2;       // FLD t1; FCOMP ST1; TEST AH,5; JP (keeps t2)
    if (!(T < dist)) return false;                    // FLD ST0; FCOMP [dist]; TEST AH,5; JP
    dist = F(T);                                      // FSTP [dist]
    return true;
}

bool HealthRayTest(const ScriptTrigger& t, const NiPoint3& o, const NiPoint3& dir, f32& dist) {   // orig: 0x000B7F00
    if (std::bit_cast<u32>(t.radius) != 0xBF800000u) return HealthRaySphere(t, o, dir, dist);   // 0x000B7D80
    const double L = dist;
    const NiPoint3 end{F(double(o.x) + L * dir.x), F(double(o.y) + double(F(L * dir.y))),
                       F(double(o.z) + double(F(L * dir.z)))};
    const NiPoint3& c = t.centre;
    const f32 mnx = F(double(c.x) - t.half.x), mny = F(double(c.y) - t.half.y), mnz = F(double(c.z) - t.half.z);
    const f32 mxx = F(double(t.half.x) + c.x), mxy = F(double(t.half.y) + c.y), mxz = F(double(t.half.z) + c.z);
    const NiPoint3 hits[6] = {
        FaceHit({mnx, mny, mxz}, {mxx, mxy, mxz}, o, end, true),   // z = max
        FaceHit({mnx, mny, mnz}, {mxx, mxy, mnz}, o, end, true),   // z = min
        FaceHit({mnx, mny, mnz}, {mnx, mxy, mxz}, o, end, true),   // x = min
        FaceHit({mxx, mny, mnz}, {mxx, mxy, mxz}, o, end, true),   // x = max
        FaceHit({mnx, mny, mnz}, {mxx, mny, mxz}, o, end, true),   // y = min
        FaceHit({mnx, mxy, mnz}, {mxx, mxy, mxz}, o, end, true)};  // y = max: computed, never compared (orig)
    double best = L * L;                              // FLD L; FLD ST0; FMULP
    bool hit = false;
    for (int i = 0; i < 5; ++i) {
        const double dx = double(hits[i].x) - o.x, dy = double(hits[i].y) - o.y, dz = double(hits[i].z) - o.z;
        const double dd = (dz * dz + dy * dy) + dx * dx;
        if (dd < best) {                              // TEST AH,5; JP skip (NaN skips)
            best = F(dd);                             // (the FST copy is reloaded: a float)
            hit = true;
        }
    }
    if (hit) dist = F(std::sqrt(best));
    return hit;
}

ScriptTriggers::~ScriptTriggers() {                   // orig: 0x000B88B0 (head first) + 0x0012D2F0 (the blocks)
    while (!list.empty()) {
        Destroy(*list.front());
        list.pop_front();
    }
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it) {   // newest block first
        mem::OperatorDelete(it->second);
        mem::OperatorDelete(it->first);
    }
}

ScriptTrigger& ScriptTriggers::Create(u32 kind, u32 size, u32 vtbl) {   // orig: new(size) + 0x000B8930
    void* p = mem::OperatorNew(size);
    std::memset(p, 0, size);
    auto t = std::make_unique<ScriptTrigger>();
    t->vm = mem::ToVmAddr(p);
    t->vtbl = vtbl;
    t->kind = kind;
    Put(t->vm, vtbl);
    Put(t->vm + 8, kind);
    if (freeNodes_ == 0) {                            // orig: 0x000B84B0: a block of 0x100 nodes
        void* header = mem::OperatorNew(8);
        void* nodes = mem::OperatorNew(0x100 * 0xC);  // orig: 0x000B7270
        blocks_.emplace_back(header, nodes);
        freeNodes_ = 0x100;
    }
    --freeNodes_;
    list.push_front(std::move(t));
    return *list.front();
}

ScriptTrigger* ScriptTriggers::Get(script::VmAddr t) {
    if (!t) return nullptr;
    for (const auto& x : list)
        if (x->vm == t) return x.get();
    return nullptr;
}

script::VmAddr ScriptTriggers::CopyName(const char* s) {
    if (!s) return 0;
    const size_t n = std::strlen(s) + 1;
    void* p = mem::OperatorNew(static_cast<u32>(n));
    std::memcpy(p, s, n);
    return mem::ToVmAddr(p);
}

void ScriptTriggers::Destroy(ScriptTrigger& t) {
    if (t.name0) mem::OperatorDelete(mem::FromVmAddr(t.name0));
    if (t.name1) mem::OperatorDelete(mem::FromVmAddr(t.name1));
    if (t.groupData) mem::OperatorDelete(mem::FromVmAddr(t.groupData));
    mem::OperatorDelete(mem::FromVmAddr(t.vm));
}

void ScriptTriggers::Nullify(script::VmAddr h) {      // orig: 0x000B8900
    for (auto it = list.begin(); it != list.end(); ++it)
        if ((*it)->vm == h) {
            ++freeNodes_;                             // orig: 0x00140720 (the node back to the free list)
            Destroy(**it);
            list.erase(it);
            return;
        }
}

bool ScriptTriggers::Test(script::VmAddr h) {         // orig: 0x000B6FD0
    ScriptTrigger* t = Get(h);
    if (!t) return false;
    RunTest(*t);
    return t->state != 0;
}

void ScriptTriggers::RefreshAll() {                   // orig: RefreshTriggers 0x000B72D0 (after ResolveAll)
    for (const auto& t : list) Refresh(*t);
}

void ScriptTriggers::Refresh(ScriptTrigger& t) {      // orig: vf08 (2.3)
    TriggerManager& tm = *level_.triggers;
    auto foc = [&](script::VmAddr name) {
        return name ? tm.FindOrCreate(static_cast<const char*>(mem::FromVmAddr(name))) : nullptr;
    };
    switch (t.kind) {
    case ScriptTrigger::Sphere:                       // orig: 0x000B7380
        t.rec0 = foc(t.name0);
        t.rec1 = foc(t.name1);
        if (t.rec1 && t.radius <= 0.0f) t.radius = tm.Radius(*t.rec1);   // FCOMP 0; TEST AH,0x41; JP (NaN skips)
        return;
    case ScriptTrigger::Box:                          // orig: 0x000B76D0
        t.rec0 = foc(t.name0);
        t.rec1 = foc(t.name1);
        if (t.rec1 && t.half.y == 0.0f) {             // (a snapshot: never recomputed once set)
            const NiPoint3 s = tm.BoxSize(*t.rec1);   // orig: 0x000DA770
            t.half = {F(double(s.x) * 0.5f), F(double(s.y) * 0.5f), F(double(s.z) * 0.5f)};
            t.centre = tm.Position(*t.rec1);
            t.rotation = tm.Rotation(*t.rec1);
        }
        return;
    case ScriptTrigger::Health:                       // orig: 0x000B7A40
        t.rec0 = foc(t.name0);
        if (t.half.y == 0.0f && t.rec0 && t.rec0->target) {
            if (t.rec0->kind == 4) {
                const NiPoint3 s = tm.BoxSize(*t.rec0);
                t.half = {F(double(s.x) * 0.5f), F(double(s.y) * 0.5f), F(double(s.z) * 0.5f)};
                t.radius = -1.0f;
                t.centre = tm.Position(*t.rec0);
                t.rotation = tm.Rotation(*t.rec0);
            } else if (t.rec0->kind == 3) {
                t.radius = tm.Radius(*t.rec0);
                t.centre = tm.Position(*t.rec0);
                t.rotation = tm.Rotation(*t.rec0);
            }
        }
        return;
    default: return;                                  // (the others' vf08 is a no-op)
    }
}

void ScriptTriggers::RunTest(ScriptTrigger& t) {      // orig: vf04 (2.5)
    TriggerManager& tm = *level_.triggers;
    Player* pl = level_.player;                       // orig: S+0x94
    switch (t.kind) {
    case ScriptTrigger::Box: {                        // orig: 0x000B8590
        TriggerRec* a = t.rec0;
        if (!a || !a->target || t.half.y == 0.0f) return;
        const double off = pl && a->kind == 5 ? double(pl->cdef.fpsHeight) : 0.0;   // (S+0x94)->def+0xA0
        const NiPoint3 p = tm.Position(*a);
        const double dx = F(double(p.x) - t.centre.x);
        const double dy = F((double(p.y) + off) - t.centre.y);
        const double dz = F(double(p.z) - t.centre.z);
        auto proj = [&](int c) {
            const auto& m = t.rotation.m;
            return std::fabs((double(m[1][c]) * dy + double(m[2][c]) * dz) + double(m[0][c]) * dx);
        };
        t.state = (proj(0) > t.half.x || proj(2) > t.half.z || proj(1) > t.half.y) ? 0 : 1;   // NaN: inside
        break;
    }
    case ScriptTrigger::Sphere: {                     // orig: 0x000B73F0
        if (!t.rec0) return;
        if (t.rec1) t.centre = tm.Position(*t.rec1);
        else if (t.name1) return;
        const double off = pl ? double(pl->cdef.boundCenter) : 0.0;   // (S+0x94)->def+0x9C
        const f32 cy = F(double(t.centre.y) - off);
        const NiPoint3 p = tm.Position(*t.rec0);
        const double dx = double(p.x) - t.centre.x, dy = double(p.y) - cy, dz = double(p.z) - t.centre.z;
        const double d = std::sqrt((dz * dz + dy * dy) + dx * dx);
        t.distance = F(d);
        t.state = d < t.radius ? 1 : 0;
        break;
    }
    case ScriptTrigger::Health:                       // orig: 0x000B7070
        if (t.half.y == 0.0f && std::bit_cast<u32>(t.radius) == 0xBF800000u) return;
        t.state = t.health <= 0.0f ? 1 : 0;
        break;
    case ScriptTrigger::Button: {                     // orig: 0x000B74F0
        if (!pl || !pl->pad) {
            t.state = 0;
            break;
        }
        const u32 btn = pl->actions.Binding(static_cast<u8>(t.action));   // (a copy of the map in the original)
        auto& pad = const_cast<input::Pad&>(*pl->pad);
        pad.SetDisabled(btn, 0);
        t.state = pad.IsDownUnlatched(btn) ? 1 : 0;
        pad.SetDisabled(btn, 0x14);                   // (every poll keeps the button disabled for the player)
        break;
    }
    case ScriptTrigger::Group: {                      // orig: 0x000B8410 (And) / 0x000B8470 (Or)
        const bool isAnd = t.vtbl == 0x002A24C0u;
        t.state = isAnd ? 1 : 0;
        for (script::VmAddr c : t.children) {
            ScriptTrigger* ct = Get(c);
            if (ct) RunTest(*ct);
            if (isAnd && (!ct || !ct->state)) {
                t.state = 0;
                break;
            }
            if (!isAnd && ct && ct->state) t.state = 1;   // (no short-circuit: every child is tested)
        }
        if (isAnd && t.children.empty()) t.state = 0;
        break;
    }
    case ScriptTrigger::Pickup: {                     // orig: 0x000B75E0 (inventory_batch1_port.md 6.1)
        if (!pl || !t.name0) return;                  // S+0x94, +0x0C: state unchanged
        const char* label = static_cast<const char*>(mem::FromVmAddr(t.name0));
        t.state = 0;                                  // +4 = 0 first
        for (GameObject* o = pl->inventory.NextObject(nullptr); o; o = pl->inventory.NextObject(o))   // P+0x38, 0x00097670
            if (o->label && crt::StrICmp(o->label->c_str(), label) == 0) {   // +0x34 non-NULL; _stricmp(+0x34, +0x0C)
                t.state = 1;
                break;
            }
        break;
    }
    case ScriptTrigger::Destructable: {               // orig: 0x000B7900: the record's destructable is broken (+0x48)
        const Destructable* d = nullptr;
        if (t.rec0 && t.rec0->target) {
            if (t.rec0->kind == 6) d = static_cast<const Destructable*>(static_cast<const GameObject*>(t.rec0->target));
            else NotPorted("script trigger: the destructable test on a non-destructable record (byte +0x48)");
        }
        t.state = d && d->broken ? 1 : 0;
        break;
    }
    case ScriptTrigger::Animation: {                  // orig: 0x000B6FF0
        if (!t.name0 && !t.name1) return;             // (state unchanged)
        // (both names or neither: native 211. With only a clip the original calls FindByName(NULL) and faults.)
        const char* anim = t.name0 ? static_cast<const char*>(mem::FromVmAddr(t.name0)) : nullptr;
        const char* clip = t.name1 ? static_cast<const char*>(mem::FromVmAddr(t.name1)) : nullptr;
        // orig: 0x000C2040: FindByName(anim), then GetCurrentClipName 0x000BA7F0 (0 without the animation)
        level::LevelAnimation* la = anim && level_.animations ? level_.animations->FindByName(anim) : nullptr;
        const char* cur = la ? la->CurrentClipName() : nullptr;
        t.state = 0;                                  // (+4 = 0 before the name test)
        if (cur && clip && crt::StrICmp(cur, clip) == 0) {   // (_stricmp(current, clip); NULL faults)
            const f32 pos = la->time;                 // orig: 0x000C2D70 (FindByName again; +0x20, FSTP to a local)
            const level::LevelAnimClip* c = la->FindClip(cur);   // orig: 0x000C2DB0(anim, the current clip's name)
            const f32 len = c ? c->duration : 0.0f;   // (+0xC; fld 0.0 without the clip)
            if (len <= pos) t.state = 1;              // FCOMP; TEST AH,0x41; JP (NaN: 0)
        }
        break;
    }
    case ScriptTrigger::AtSight: {                    // orig: 0x000B77D0 (inventory_batch1_port.md 6.2)
        if (!t.rec0 || !t.rec1) return;               // (port guard: the original tests neither record)
        TriggerRec& w = *t.rec0;                      // +0x0C, the watcher
        NiPoint3 eye = tm.Position(w);                // 0x000DA7E0 -> esp+0x18
        if (w.kind == 5 && w.target)                  // +0x18, +0x0C
            eye.y = F(double(eye.y) + static_cast<Character*>(w.target)->cdef.fpsHeight);   // (target+0x10)+0xA0
        const NiPoint3 q = tm.Position(*t.rec1);      // +0x10, the target
        const double dzu = double(q.z) - eye.z;       // FST: the product uses the unrounded z
        NiPoint3 d{F(double(q.x) - eye.x), F(double(q.y) - eye.y), F(dzu)};
        f32 len = F(std::sqrt((dzu * d.z + double(d.y) * d.y) + double(d.x) * d.x) + 0.2f);   // FSQRT; FADD [0x00293AB4]
        (void)ni::Unitize(d);                         // 0x000116A0 (FSTP ST0)
        (void)ni::RayCast(eye, d, len, true, 3);      // 0x000FB780: the result is not read
        const ni::NiMatrix3 R = tm.Rotation(w);       // 0x000DB090 -> esp+0x30
        // f_r = (chunk2[r] + chunk1[r]) * 0 + chunk0[r]: the watcher's x axis (column 0) unless column 1 / 2 holds an
        // infinity or a NaN
        auto f = [&](int r) { return (double(R.m[r][2]) + R.m[r][1]) * 0.0f + R.m[r][0]; };
        const double f0 = f(0), f1 = f(1), f2 = f(2);
        const double dot = (double(d.z) * f2 + double(d.y) * f1) + double(d.x) * f0;   // FMUL ST1 / FXCH order
        t.state = dot > 0.4 ? 1 : 0;                  // FCOMP QWORD [0x002A2488] (0.4); TEST AH,0x41; JNE -> 0
        break;
    }
    default: return;                                  // NULL: fixed by the constructor
    }
    Put(t.vm + 4, t.state);
}

}  // namespace fut::game

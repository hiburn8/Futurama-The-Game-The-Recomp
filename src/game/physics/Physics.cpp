// The physics world; see Physics.h and docs/analysis/physics.md (section numbers below).
#include "game/physics/Physics.h"

#include "core/CrtQsort.h"
#include "engine/ni/NiCollision.h"
#include "game/GameTime.h"
#include "game/object/GameObject.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fut::phys {

using ni::NiPoint3;
using ni::detail::F;

PhysObject* g_objects = nullptr;

namespace {

constexpr f32 kNoAxis = 100000.0f;                    // orig: 0x0029D104
const NiPoint3 kAxes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // orig: 0x002D67FC
const NiPoint3 kUp{0, 1, 0};                          // orig: 0x002D77EC

f32 Comp(const NiPoint3& p, int i) { return i == 0 ? p.x : i == 1 ? p.y : p.z; }
f32& Comp(NiPoint3& p, int i) { return i == 0 ? p.x : i == 1 ? p.y : p.z; }
f32 AsFloat(u32 bits) {
    f32 f;
    std::memcpy(&f, &bits, 4);
    return f;
}
// orig: _ftol 0x00167110 (VC7.1 _ftol2): truncation toward zero, the low 32 bits.
s32 Ftol(double v) { return static_cast<s32>(static_cast<s64>(v)); }

// The substep count of char-char / char-box / slide: |d| over half the extent per axis (the y quotient of the slide
// times 2.5), at least 1, at most 10.
u32 SubSteps(const NiPoint3& d, const NiPoint3& ext, bool slide) {
    u32 steps = 1;
    for (int i = 0; i < 3; ++i) {
        double q = std::fabs(double(Comp(d, i))) / (double(Comp(ext, i)) * 0.5);
        if (slide && i == 1) q = q * 2.5;
        if (!(q < 1.0)) {                             // TEST AH,5; JNP skips only "less"
            const u32 n = static_cast<u32>(Ftol(q + 1.0));
            if (steps < n) steps = n;
        }
    }
    return steps > 10 ? 10 : steps;
}

// ---- the gathered triangle list (orig: DAT_002F8EC4 / count DAT_002F8EB8) ----
std::vector<TriRef> g_gathered;

NiPoint3 g_boxMin, g_boxMax;                          // orig: 0x002F907C / 0x002F9018

void BoxWalk(ni::NiBsp& b, const ni::NiBsp::TreeNode& node) {   // orig: inner 0x000FADA0 / leaf 0x000FA3F0
    if (node.type == 3) {
        if (ni::g_wantRecs) ni::AppendRecords(node);  // the records first
        if (ni::g_wantTris)
            for (u32 off : node.indices) g_gathered.push_back({&b, off / 0x14});   // no per-triangle test
        return;
    }
    if (node.children.size() < 2) return;
    const int axis = static_cast<int>(node.type);
    const f32 split = AsFloat(node.value);
    if (Comp(g_boxMin, axis) < split) BoxWalk(b, node.children[0]);   // TEST AH,5; JP skips (>= / NaN)
    if (Comp(g_boxMax, axis) > split) BoxWalk(b, node.children[1]);   // TEST AH,0x41; JNE skips (<= / NaN)
    if (ni::g_wantRecs) ni::AppendRecords(node);      // the node's own records (post-order)
}

// orig: the qsort comparator 0x000A9170: (int)(a->flags - b->flags).
int CompareFlags(const void* a, const void* b) {
    const TriRef& x = *static_cast<const TriRef*>(a);
    const TriRef& y = *static_cast<const TriRef*>(b);
    return static_cast<int>(x.Flags() - y.Flags());
}

}  // namespace

u32& TriRef::Flags() const { return bsp ? bsp->triangles[index].b : ni::g_scratch.tris[index].flags; }

const NiPoint3& TriRef::V(int k) const {
    // (a scratch triangle's vertex pointers point at its own slot's vertices once it has been accepted)
    return bsp ? bsp->vertices[bsp->triangles[index].v[k]] : ni::g_scratch.verts[3 * index + static_cast<u32>(k)];
}

NiPoint3 TriRef::Normal() const {
    if (bsp) return ni::TriangleNormal(*bsp, bsp->triangles[index]);
    return ni::TriangleNormal(V(0), V(1), V(2), ni::g_scratch.tris[index].invCross);
}

// The NiBsp triangle arrays' addresses (NiBsp::origTriAddr: the play oracle's, or the default order: newer stages
// lower) and the scratch block's (ni::g_scratch.origAddr: the oracle's, or the default above every NiBsp, L).
u64 TriRef::OrigAddr() const {
    return (bsp ? u64(bsp->origTriAddr) : u64(ni::g_scratch.origAddr)) + 0x14ull * index;
}

const std::vector<TriRef>& QuerySphere(const NiPoint3& c, f32 r, u32 flags) {   // orig: 0x000FB410 (3.6)
    g_gathered.clear();                               // DAT_002F8EB8 = 0
    ni::g_scratch.count = 0;                          // DAT_002F4698 = 0
    for (ni::NiBsp* b = ni::g_bspList; b; b = b->nextInWorld) {
        if (b->tree.type != 3 && b->tree.children.empty()) continue;   // orig: no tree (+0xE4)
        ni::g_candCount = 0;
        ni::g_wantTris = (flags & 1) != 0;
        ni::g_wantRecs = (flags & 2) != 0;
        if (ni::g_wantRecs || ni::g_wantTris) {
            g_boxMin = {F(double(c.x) - r), F(double(c.y) - r), F(double(c.z) - r)};   // orig: BeginBoxQuery 0x000FAA60
            g_boxMax = {F(double(r) + c.x), F(double(r) + c.y), F(double(r) + c.z)};
            BoxWalk(*b, b->tree);
            if (ni::g_wantRecs) ni::QueryMeshes(*b, c, r, flags);   // vfA4
        }
        ni::g_candCount = 0;
        ni::g_wantTris = true;
        ni::g_wantRecs = false;
    }
    // orig: AppendMeshTris 0x000FA510: the scratch triangles after every stage triangle, in slot order
    for (u32 i = 0; i < ni::g_scratch.count; ++i) g_gathered.push_back({nullptr, i, true});
    // orig: RadixSortPtrs 0x000FA020 (ascending addresses) and the duplicate removal.
    std::stable_sort(g_gathered.begin(), g_gathered.end(),
                     [](const TriRef& a, const TriRef& b) { return a.OrigAddr() < b.OrigAddr(); });
    g_gathered.erase(std::unique(g_gathered.begin(), g_gathered.end(),
                                 [](const TriRef& a, const TriRef& b) {
                                     return a.bsp == b.bsp && a.mesh == b.mesh && a.index == b.index;
                                 }),
                     g_gathered.end());
    return g_gathered;
}

// ---- objects ----------------------------------------------------------------------------------------------------
PhysObject::PhysObject() {                            // orig: 0x000AF020
    next = g_objects;
    if (g_objects) g_objects->prev = this;
    g_objects = this;
}

PhysObject::~PhysObject() {                           // orig: 0x000AF080
    if (next) next->prev = prev;
    if (prev) prev->next = next;
    else if (g_objects == this) g_objects = next;
}

CharBody::CharBody(f32 ex, f32 ey, f32 ez, PhysWorld* w) : world(w) {   // orig: 0x000AF0D0
    shape.ext = {ex, ey, ez};
    if (world) world->AddShape(shape);
}

CharBody::~CharBody() {                               // orig: 0x000B0090 -> shape dtor 0x000A90E0
    if (shape.world) shape.world->RemoveShape(shape);
}

ni::NiAVObject* CharBody::OwnerNode() const { return owner ? owner->GetNode() : nullptr; }

void CharBody::SetEnabled(bool on) {                  // orig: 0x000AED40
    if (bool(enabled) == on) return;
    enabled = on;
    if (!world) return;
    if (on) world->AddShape(shape);
    else world->RemoveShape(shape);
}

void CharBody::Integrate(f32 dt) {                    // orig: 0x000AF200 (2.4)
    if (!enabled) return;
    CharShape& s = shape;
    s.b1 = b1A;
    s.b9 = b1C;
    if (s.b1) s.excludeNode = OwnerNode();
    supported = 0;
    const u32 f = s.flags;                            // (the previous step's contacts)
    NiPoint3& v = velocity;
    if (f & 0x11) {
        supported = 1;
        if (v.y <= 0.0f) v.y = 0;                     // TEST AH,0x41; JP skips (greater / NaN)
    } else if (f & 0x40) {
        if (v.y < -50.0f) v.y = -50.0f;               // orig: 0x00297648
    } else if (f & 0x80) {
        supported = 1;
        if (v.y < -1.0f) v.y = -1.0f;                 // orig: 0x00293BF0
    }
    if (!integratePinned && s.pinned) return;
    if (halveXZ) {
        v.x = F(double(v.x) * 0.5);
        v.z = F(double(v.z) * 0.5);
    }
    v.x = F(double(dt) * force.x + v.x);
    v.y = F(double(dt) * force.y + v.y);
    v.z = F(double(F(double(dt) * force.z)) + v.z);
    lastPos = s.pos;
    s.slideStart = s.pos;
    s.pos.x = F(double(dt) * v.x + s.pos.x);
    s.pos.y = F(double(dt) * v.y + s.pos.y);
    s.pos.z = F(double(F(double(dt) * v.z)) + s.pos.z);
    if (integratePinned && s.pinned) {
        lastPos = s.pos;
        s.slideStart = s.pos;
    }
    s.descending = (supported != 0 || v.y < 0.0f) ? 1 : 0;   // TEST AH,5; JNP (NaN -> 0)
}

bool CharBody::OverlapsSphere(const NiPoint3& c, const f32& r) const {   // orig: vf1C 0x000AF5F0
    if (!enabled) return false;                       // +0x24
    const CharShape& s = shape;                       // +0x40
    const NiPoint3 lo{F(double(s.pos.x) - s.ext.x), F(double(s.pos.y) - s.ext.y), F(double(s.pos.z) - s.ext.z)};
    const NiPoint3 hi{F(double(s.ext.x) + s.pos.x), F(double(s.ext.y) + s.pos.y), F(double(s.ext.z) + s.pos.z)};
    for (int k = 0; k < 3; ++k) {
        if (double(Comp(c, k)) - r > Comp(hi, k)) return false;   // FCOMP hi; TEST AH,0x41; JE (NaN passes)
        if (double(Comp(c, k)) + r < Comp(lo, k)) return false;   // FCOMP lo; TEST AH,5; JNP
    }
    return true;
}

bool CollisionBox::OverlapsSphere(const NiPoint3& c, const f32& r) const {   // orig: vf1C 0x000AFF20
    if (!enabled) return false;                       // +0x24
    const RigidBox& b = box;                          // +0x28
    const double dx = double(c.x) - b.pos.x, dy = double(c.y) - b.pos.y, dz = double(c.z) - b.pos.z;   // (x87)
    // +0x90: three chunks of 4 floats, the port's columns (chunk k element j = m[j][k], as MakeYRotation 0x001030B0).
    const auto& m = b.rot.m;
    const f32 local[3] = {F((double(m[0][2]) * dz + double(m[0][1]) * dy) + double(m[0][0]) * dx),
                          F((double(m[1][2]) * dz + double(m[1][0]) * dx) + double(m[1][1]) * dy),
                          F((double(m[2][2]) * dz + double(m[2][0]) * dx) + double(m[2][1]) * dy)};
    for (int k = 0; k < 3; ++k) {
        const f32 half = Comp(b.ext, k);              // +0x160
        if (double(local[k]) - r > half) return false;            // TEST AH,0x41; JE
        if (-double(half) > double(local[k]) + r) return false;  // FLD half; FCHS; FCOMPP; TEST AH,0x41; JE
    }
    return true;
}

namespace {

// The slab loop shared by the ray tests 0x000AF400 / 0x000AFC90 (fireball_port.md 8.2).
bool SlabTest(const f32* mn, const f32* mx, const f32* o, const f32* d, f32& len, bool frontOnly) {
    f32 tmin = -3.40282347e38f, tmax = 3.40282347e38f;   // FLD [0x2A6984]; FCHS / the dword copy
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-5f) {                // FABS; FCOMP [0x2A0FE8]; TEST AH,5; JP -> the slab
            if (o[i] < mn[i]) return false;           // TEST AH,5; JNP
            if (o[i] > mx[i]) return false;           // TEST AH,0x41; JE
            continue;
        }
        const f32 inv = F(1.0 / d[i]);
        const double t1 = (double(mn[i]) - o[i]) * inv;
        const double t2 = (double(mx[i]) - o[i]) * inv;
        double nearT = t1, farT = t2;
        if (t2 < t1) {                                // FCOM ST1; TEST AH,5; JP (the swap through two float stores)
            nearT = F(t2);
            farT = F(t1);
        }
        if (tmax > farT) tmax = F(farT);              // TEST AH,0x41; JNE keep
        if (tmin < nearT) tmin = F(nearT);            // TEST AH,5; JP keep
        if (tmax < tmin) return false;
        if (tmax < 0.0f) return false;
        if (tmin > len) return false;                 // TEST AH,0x41; JE false
    }
    if (tmin > 0.0f) {
        len = tmin;
        return true;
    }
    if (frontOnly) return false;
    len = tmax;                                       // the origin is inside the box
    return true;
}

}  // namespace

bool CharBody::RayTest(const NiPoint3& o, const NiPoint3& d, f32& len, bool frontOnly) const {   // orig: 0x000AF400
    if (!enabled || noRay) return false;              // +0x24 / +0x1D
    const NiPoint3& c = shape.pos;                    // shape +0x14
    const NiPoint3& e = shape.ext;                    // shape +0x90
    const f32 mn[3] = {F(double(c.x) - e.x), F(double(c.y) - e.y), F(double(c.z) - e.z)};   // FLD pos; FSUB ext
    const f32 mx[3] = {F(double(e.x) + c.x), F(double(e.y) + c.y), F(double(e.z) + c.z)};   // FLD ext; FADD pos
    const f32 oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    return SlabTest(mn, mx, oo, dd, len, frontOnly);
}

bool CollisionBox::RayTest(const NiPoint3& o, const NiPoint3& d, f32& len, bool frontOnly) const {   // orig: 0x000AFC90
    const auto& m = box.rot.m;                        // +0x90 (chunk k element j = m[j][k], as in OverlapsSphere)
    const double dx = double(o.x) - box.pos.x, dy = double(o.y) - box.pos.y, dz = double(o.z) - box.pos.z;
    const f32 lo[3] = {F((double(m[0][2]) * dz + double(m[0][1]) * dy) + dx * m[0][0]),
                       F((double(m[1][2]) * dz + double(m[1][1]) * dy) + double(m[1][0]) * dx),
                       F((double(m[2][2]) * dz + double(m[2][1]) * dy) + double(m[2][0]) * dx)};
    const f32 ld[3] = {F((double(m[0][2]) * d.z + double(m[0][1]) * d.y) + double(m[0][0]) * d.x),
                       F((double(m[1][0]) * d.x + double(m[1][2]) * d.z) + double(m[1][1]) * d.y),
                       F((double(m[2][0]) * d.x + double(m[2][2]) * d.z) + double(m[2][1]) * d.y)};
    const f32 mn[3] = {-box.ext.x, -box.ext.y, -box.ext.z};   // FCHS (+0x160)
    const f32 mx[3] = {box.ext.x, box.ext.y, box.ext.z};
    return SlabTest(mn, mx, lo, ld, len, frontOnly);
}

CollisionBox::CollisionBox(f32 ex, f32 ey, f32 ez, f32 mass, PhysWorld* w) : world(w) {   // orig: 0x000AF9B0
    // (the body's ctor 0x000AB290 / 0x000AD980 zeroes the state, rot = identity; the material 0x000AB180(0) = (0.85,
    // 0.75, 0.57): RigidBox's initialisers)
    box.ext = {ex, ey, ez};                           // vf18 0x000AB250 (raw)
    box.mass = mass;                                  // vf08 0x000AD560 (raw)
    box.InitInertia();                                // vf0C 0x000AB330
    if (world) world->AddBox(box);                    // (DAT_002E9A7C +0x20)->0x000ADF30
}

CollisionBox::~CollisionBox() {
    if (box.world) box.world->RemoveBox(box);
}

void CollisionBox::ApplyImpulseAt(const NiPoint3& p, const NiPoint3& impulse) {   // orig: vf40 0x000AF880
    const NiPoint3 r{F(double(p.x) - box.pos.x), F(double(p.y) - box.pos.y), F(double(p.z) - box.pos.z)};
    box.ApplyImpulse(r, impulse);                     // 0x000AD720 (both by value)
}

bool CollisionBox::IsAtRest() const {                 // orig: vf44 0x000AF910
    const NiPoint3 v = GetVelocity();                 // vf0C
    // FCOMP [0x00293AB4] (0.2); TEST AH,0x41; JNE: only a greater sum is moving (NaN: at rest)
    if ((double(v.z) * v.z + double(v.y) * v.y) + double(v.x) * v.x > 0.2f) return false;
    const NiPoint3& w = box.w;                        // body +0x11C
    if ((double(w.z) * w.z + double(w.y) * w.y) + double(w.x) * w.x > 0.1f) return false;   // [0x00293AB0]
    return !(box.flags & 0x20);                       // SHR 5; NOT; AND 1
}

void CollisionBox::Integrate(f32 dt) {                // orig: 0x000AFAC0 (4.2)
    if (!enabled) return;                             // +0x24
    (void)GetRotation();                              // vf3C (the copy is not used)
    RigidBox& B = box;                                // +0x28
    if (B.isStatic) return;                           // +0x150
    const f32 r = F(B.BoundingRadius() * 1.01f);   // vf04 0x000AB400 (ST0); FMUL [0x002A20B0]
    const std::vector<TriRef>& tris = QuerySphere(B.pos, r, 3);   // 0x000FB410(&pos, r, 3)
    B.flags = 0;                                      // +0x154
    B.Step(tris);                                     // vf10(DAT_002F8EC4, DAT_002F8EB8)
    B.ClearAccumulators();                            // 0x000AD5A0
    const NiPoint3& w = B.w;
    if ((double(w.x) * w.x + double(w.y) * w.y) + double(w.z) * w.z < 1000000.0f)   // [0x002A20AC]; TEST AH,5; JP
        B.L = {F(double(B.L.x) * 0.9f), F(double(B.L.y) * 0.9f), F(double(B.L.z) * 0.9f)};   // [0x00293C08]
    const NiPoint3& v = B.v;
    const double k = (double(v.x) * v.x + double(v.y) * v.y) + double(v.z) * v.z < 1.0f ? double(0.9f)
                                                                                         : double(0.98f);   // [0x00293C14]
    B.P = {F(double(B.P.x) * k), F(double(B.P.y) * k), F(double(B.P.z) * k)};
    B.UpdateDerived();                                // 0x000ADA00
    Gravity(dt);                                      // 0x000AF800 (the force of the next step)
}

void CollisionBox::Gravity(f32 dt) {                  // orig: 0x000AF800 (thiscall(dt), RET 4: dt unused)
    (void)dt;
    if (!enabled) return;                             // +0x24
    const double m = box.mass;                        // body +0x10
    const NiPoint3 f{F(m * force.x), F(m * force.y), F(m * force.z)};   // this +0x0C
    box.AddForceAtPoint(f, {0.0f, 0.0f, 0.0f});       // 0x000AD680 (r = 0)
}

// ---- the dynamic rigid box (simplenpc_moveable_port.md 4) ---------------------------------------------------------
namespace {

ni::NiMatrix3 Transpose(const ni::NiMatrix3& a) {     // orig: NiMatrix3::Transpose 0x00103C60
    ni::NiMatrix3 t;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) t.m[r][c] = a.m[c][r];
    return t;
}

// orig: 0x001039B0 (thiscall(out, other), RET 8): every element F(other + this).
ni::NiMatrix3 MatrixAdd(const ni::NiMatrix3& a, const ni::NiMatrix3& b) {
    ni::NiMatrix3 o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o.m[r][c] = F(double(b.m[r][c]) + a.m[r][c]);
    return o;
}

// orig: 0x000AD900 (SSE mulps): every element times dt in single precision.
ni::NiMatrix3 MatrixScale(const ni::NiMatrix3& a, f32 dt) {
    ni::NiMatrix3 o;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o.m[r][c] = a.m[r][c] * dt;
    return o;
}

// The column c of a matrix (the original's 16-byte chunk c).
NiPoint3 Col(const ni::NiMatrix3& m, int c) { return {m.m[0][c], m.m[1][c], m.m[2][c]}; }
void SetCol(ni::NiMatrix3& m, int c, const NiPoint3& v) { m.m[0][c] = v.x, m.m[1][c] = v.y, m.m[2][c] = v.z; }

// orig: 0x001034A0 (in place; the result is ignored by UpdateDerived): Gram-Schmidt over the three columns. (The
// pad lanes of each chunk take part lane by lane but never reach x, y, z.)
bool Orthonormalize(ni::NiMatrix3& m) {
    NiPoint3 c0 = Col(m, 0), c1 = Col(m, 1), c2 = Col(m, 2);
    const auto len = [](const NiPoint3& v) {          // sqrt((x*x + y*y) + z*z)
        return std::sqrt((double(v.x) * v.x + double(v.y) * v.y) + double(v.z) * v.z);
    };
    // orig: 0x00102F90 (thiscall(out, s)): F(s * v[k]) per lane
    const auto scaled = [](const NiPoint3& v, f32 s) {
        return NiPoint3{F(double(s) * v.x), F(double(s) * v.y), F(double(s) * v.z)};
    };
    const auto sub = [](NiPoint3& a, const NiPoint3& b) {
        a = {F(double(a.x) - b.x), F(double(a.y) - b.y), F(double(a.z) - b.z)};
    };
    const auto put = [&] {
        SetCol(m, 0, c0), SetCol(m, 1, c1), SetCol(m, 2, c2);
    };
    const double l0 = len(c0);
    if (l0 < 1e-5f) {                                 // FCOM [0x002A0FE8]; TEST AH,5; JNP (only <)
        put();
        return false;
    }
    const double i0 = 1.0 / l0;
    const double qx = i0 * c0.x, qy = i0 * c0.y, qz = i0 * c0.z;   // (FST: the stack keeps the unrounded values)
    c0 = {F(qx), F(qy), F(qz)};
    const f32 d1 = F((qy * c1.y + qx * c1.x) + qz * c1.z);
    sub(c1, scaled(c0, d1));
    const double l1 = len(c1);
    if (l1 < 1e-5f) {
        put();
        return false;
    }
    const double i1 = 1.0 / l1;
    c1 = {F(i1 * c1.x), F(i1 * c1.y), F(i1 * c1.z)};
    const f32 d2 = F((double(c2.z) * c0.z + double(c2.x) * c0.x) + double(c0.y) * c2.y);
    sub(c2, scaled(c0, d2));
    const f32 d3 = F((double(c2.z) * c1.z + double(c1.x) * c2.x) + double(c1.y) * c2.y);
    sub(c2, scaled(c1, d3));
    const double l2 = len(c2);
    if (l2 < 1e-5f) {                                 // TEST AH,5; JP (not < goes on)
        put();
        return false;
    }
    const f32 s = F(1.0 / l2);
    c2 = {F(double(s) * c2.x), F(double(s) * c2.y), F(double(s) * c2.z)};   // 0x00102F40 (*= in place)
    put();
    return true;
}

}  // namespace

double RigidBox::BoundingRadius() const {             // orig: vf04 0x000AB400 (ST0; the caller multiplies first)
    return std::sqrt((double(ext.x) * ext.x + double(ext.y) * ext.y) + double(ext.z) * ext.z);
}

void RigidBox::InitInertia() {                        // orig: vf0C 0x000AB330
    if (mass < 1e-5f) mass = 1.0f;                    // FCOMP [0x002A0FE8]; TEST AH,5; JP
    invMass = F(1.0 / mass);
    const f32 xx = F(double(ext.x) * ext.x * mass);   // [esp+0x1C]
    const double yy = double(ext.y) * ext.y * mass, zz = double(ext.z) * ext.z * mass;   // (on the stack)
    inertia = ni::NiMatrix3{{{F(zz + yy), 0, 0}, {0, F(double(xx) + zz), 0}, {0, 0, F(yy + xx)}}};   // 0x00103030
    invInertia = inertia;
    invInertia.m[0][0] = F(1.0 / invInertia.m[0][0]); // +0x50
    invInertia.m[1][1] = F(1.0 / invInertia.m[1][1]); // +0x64
    invInertia.m[2][2] = F(1.0 / invInertia.m[2][2]); // +0x78
    UpdateDerived();                                  // (tail jump 0x000ADA00)
}

void RigidBox::UpdateDerived() {                      // orig: 0x000ADA00 (4.5)
    const ni::NiMatrix3 T = Transpose(rot);           // 0x00103C60, before the orthonormalisation
    (void)Orthonormalize(rot);                        // 0x001034A0
    invInertiaWorld = ni::Multiply(ni::Multiply(rot, invInertia), T);   // 0x00013B30 twice
    const double im = invMass;
    v = {F(im * P.x), F(im * P.y), F(im * P.z)};
    const auto& I = invInertiaWorld.m;
    w = {F((double(I[0][2]) * L.z + double(I[0][1]) * L.y) + double(I[0][0]) * L.x),
         F((double(I[1][2]) * L.z + double(I[1][1]) * L.y) + double(I[1][0]) * L.x),
         F((double(I[2][2]) * L.z + double(I[2][1]) * L.y) + double(I[2][0]) * L.x)};
}

void RigidBox::IntegrateBy(f32 dt) {                  // orig: 0x000ADB20 (4.4)
    if (!noRotation) {                                // +0x151
        // the skew matrix: the columns (0, w.z, -w.y), (-w.z, 0, w.x), (w.y, -w.x, 0) (FCHS: exact)
        const ni::NiMatrix3 W{{{0, -w.z, w.y}, {w.z, 0, -w.x}, {-w.y, w.x, 0}}};
        rot = MatrixAdd(rot, ni::Multiply(MatrixScale(W, dt), rot));   // 0x001039B0(rot, (W*dt) * rot)
    }
    const double im = invMass, t = dt;
    const f32 vy = F(im * P.y), vz = F(im * P.z);
    const f32 dz = F(double(vz) * t);
    pos = {F(im * P.x * t + pos.x), F(double(vy) * t + pos.y), F(double(dz) + pos.z)};
    const f32 ay = F(im * force.y), az = F(im * force.z);
    const f32 azt = F(double(az) * t);
    const f32 hx = F(double(F(im * force.x * t * t)) * 0.5f);   // FMUL [0x00293AA0]
    const f32 hy = F(double(ay) * t * t * 0.5f);
    const double hz = double(azt) * t * 0.5f;
    pos = {F(double(hx) + pos.x), F(double(hy) + pos.y), F(hz + pos.z)};
    const f32 pz = F(t * force.z);
    P = {F(t * force.x + P.x), F(t * force.y + P.y), F(double(pz) + P.z)};
    const f32 lz = F(t * torque.z);
    L = {F(t * torque.x + L.x), F(t * torque.y + L.y), F(double(lz) + L.z)};
}

NiPoint3 RigidBox::PointVelocity(const NiPoint3& r) const {   // orig: 0x000AD600
    const f32 cx = F(double(r.z) * w.y - double(r.y) * w.z);
    const f32 cy = F(double(r.x) * w.z - double(r.z) * w.x);
    const double cz = double(r.y) * w.x - double(r.x) * w.y;
    return {F(double(cx) + v.x), F(double(cy) + v.y), F(cz + v.z)};
}

void RigidBox::AddForceAtPoint(const NiPoint3& f, const NiPoint3& r) {   // orig: 0x000AD680
    force = {F(double(f.x) + force.x), F(double(f.y) + force.y), F(double(f.z) + force.z)};
    const double tx = double(r.y) * f.z - double(r.z) * f.y;
    const double ty = double(r.z) * f.x - double(f.z) * r.x;
    const f32 tz = F(double(r.x) * f.y - double(r.y) * f.x);
    torque = {F(tx + torque.x), F(ty + torque.y), F(double(tz) + torque.z)};
}

void RigidBox::ApplyImpulse(const NiPoint3& r, const NiPoint3& J) {   // orig: 0x000AD720 (4.7)
    P = {F(double(J.x) + P.x), F(double(J.y) + P.y), F(double(J.z) + P.z)};
    const double lx = double(r.y) * J.z - double(r.z) * J.y;
    const double ly = double(r.z) * J.x - double(J.z) * r.x;
    const f32 lz = F(double(J.y) * r.x - double(r.y) * J.x);
    L = {F(lx + L.x), F(ly + L.y), F(double(lz) + L.z)};
    const double im = invMass;
    v = {F(im * P.x), F(im * P.y), F(im * P.z)};
    const auto& I = invInertiaWorld.m;
    w = {F((double(I[0][2]) * L.z + double(I[0][1]) * L.y) + double(I[0][0]) * L.x),
         F((double(I[1][0]) * L.x + double(I[1][2]) * L.z) + double(I[1][1]) * L.y),
         F((double(I[2][0]) * L.x + double(I[2][2]) * L.z) + double(I[2][1]) * L.y)};
}

void RigidBox::SetVelocity(const NiPoint3& vel) {     // orig: 0x000AD8A0
    v = vel;                                          // (raw; w and L unchanged)
    const double m = mass;
    P = {F(m * v.x), F(m * v.y), F(m * v.z)};
}

void RigidBox::Step(const std::vector<TriRef>& tris) {   // orig: vf10 0x000AB430 (4.3)
    f32 h = F(double(game::g_frameStep) * 0.5f);      // [0x00293AA0]
    f32 t = h;
    NiPoint3 hitN{};
    f32 hitDepth = -1.0f, hitT = -1.0f;
    for (int it = 0; it < 6; ++it) {
        h = F(double(h) * 0.5f);
        const NiPoint3 sPos = pos, sP = P, sL = L;
        const ni::NiMatrix3 sRot = rot;
        Advance(t);                                   // 0x000ADD90
        size_t i = 0;
        for (; i < tris.size(); ++i) {
            NiPoint3 N;
            f32 depth = 0;
            if (TriangleTest(tris[i], N, depth, false)) {   // vf14(.., NULL)
                hitN = N;
                hitDepth = depth;
                hitT = t;
                break;
            }
        }
        t = i == tris.size() ? F(double(t) + h) : F(double(t) - h);   // no hit: later, else earlier
        pos = sPos, rot = sRot, P = sP, L = sL;
        UpdateDerived();
    }
    if (hitDepth < 0.0f) {                            // FCOMP [0.0]; TEST AH,5; JP (NaN: a hit)
        Advance(game::g_frameStep);
        return;
    }
    Advance(hitT);
    for (const TriRef& tr : tris) {
        NiPoint3 N;
        f32 depth = 0;
        if (!TriangleTest(tr, N, depth, true)) continue;   // vf14(.., &hitN): the response
        depth = F(double(depth) + 0.01f);             // [0x00293AAC]
        flags |= 4;
        const f32 z = F(double(N.z) * depth);
        pos = {F(double(N.x) * depth + pos.x), F(double(N.y) * depth + pos.y), F(double(z) + pos.z)};
    }
    Advance(F(double(game::g_frameStep) - hitT));
}

namespace {

// orig: 0x000AE400 (cdecl(a, b, n, p, vrel, e, cap)), the one-body branch (b = NULL at both call sites): the impulse
// magnitude along n at the world point p (ST0).
double ImpulseMagnitude(const RigidBox& a, const NiPoint3& n, const NiPoint3& p, const NiPoint3& vr, f32 e, f32 cap) {
    double num = -(((double(n.z) * vr.z + double(n.y) * vr.y) + double(n.x) * vr.x) * (double(e) + 1.0f));
    if (std::fabs(num) < cap) num = cap;              // FCOMP cap; TEST AH,5; JP (only <)
    const double rx = double(p.x) - a.pos.x, ry = double(p.y) - a.pos.y, rz = double(p.z) - a.pos.z;
    const f32 rzf = F(double(p.z) - a.pos.z);         // [esp+0x2C]
    const f32 cx = F(ry * n.z - double(rzf) * n.y);
    const f32 cy = F(double(rzf) * n.x - double(n.z) * rx);
    const f32 cz = F(double(n.y) * rx - ry * n.x);
    const auto& I = a.invInertiaWorld.m;
    const f32 kx = F((double(cz) * I[0][2] + double(cy) * I[0][1]) + double(cx) * I[0][0]);
    const f32 ky = F((double(cz) * I[1][2] + double(cy) * I[1][1]) + double(cx) * I[1][0]);
    const double kz = (double(cz) * I[2][2] + double(cy) * I[2][1]) + double(cx) * I[2][0];
    const f32 ux = F(double(ky) * rz - kz * ry);
    const f32 uy = F(kz * rx - rz * kx);
    const double uz = ry * kx - double(ky) * rx;
    return num / (((uz * n.z + double(uy) * n.y) + double(ux) * n.x) + a.invMass);
}

}  // namespace

bool RigidBox::TriangleTest(const TriRef& tr, NiPoint3& outN, f32& outDepth, bool respond) {   // orig: 0x000AB6D0 (4.6)
    NiPoint3 d[3];
    for (int k = 0; k < 3; ++k) {
        const NiPoint3& p = tr.V(k);
        d[k] = {F(double(p.x) - pos.x), F(double(p.y) - pos.y), F(double(p.z) - pos.z)};
    }
    const ni::NiMatrix3 Tm = Transpose(rot);          // 0x00103C60
    const auto& t = Tm.m;
    NiPoint3 V[3];
    V[0] = {F((double(t[0][2]) * d[0].z + double(t[0][1]) * d[0].y) + double(t[0][0]) * d[0].x),
            F((double(t[1][2]) * d[0].z + double(t[1][1]) * d[0].y) + double(t[1][0]) * d[0].x),
            F((double(t[2][2]) * d[0].z + double(t[2][1]) * d[0].y) + double(t[2][0]) * d[0].x)};
    for (int k = 1; k < 3; ++k)
        V[k] = {F((double(t[0][2]) * d[k].z + double(t[0][1]) * d[k].y) + double(t[0][0]) * d[k].x),
                F((double(t[1][0]) * d[k].x + double(t[1][2]) * d[k].z) + double(t[1][1]) * d[k].y),
                F((double(t[2][0]) * d[k].x + double(t[2][2]) * d[k].z) + double(t[2][1]) * d[k].y)};
    const auto sub = [](const NiPoint3& a, const NiPoint3& b) {
        return NiPoint3{F(double(a.x) - b.x), F(double(a.y) - b.y), F(double(a.z) - b.z)};
    };
    const NiPoint3 E[3] = {sub(V[2], V[1]), sub(V[0], V[2]), sub(V[1], V[0])};   // E_k is opposite V_k
    f32 depth[13];
    NiPoint3 normal[13];
    // axes 0..2: the box faces
    for (int j = 0; j < 3; ++j) {
        const f32 e = Comp(ext, j);
        f32 lo = Comp(V[0], j), hi = Comp(V[1], j);
        if (Comp(V[0], j) > Comp(V[1], j)) lo = Comp(V[1], j), hi = Comp(V[0], j);   // TEST AH,0x41; JNE
        const f32 c2 = Comp(V[2], j);
        if (lo > c2) lo = c2;
        else if (hi < c2) hi = c2;                    // TEST AH,5; JP
        const f32 pPos = F(double(hi) - (-double(e)));
        const double pNeg = double(e) - lo;
        if (pPos < 0.0f || pNeg < 0.0) return false;  // TEST AH,5; JNP (only <)
        if (double(pPos) < pNeg) {
            depth[j] = pPos;
            normal[j] = kAxes[j];                     // 0x002D67FC
        } else {
            depth[j] = F(pNeg);
            normal[j] = {-kAxes[j].x, -kAxes[j].y, -kAxes[j].z};   // FCHS
        }
    }
    // axis 3: the triangle normal in box space
    f32 tmpSign = 1.0f;                               // [esp+0x7C] (s)
    const NiPoint3 Nw = tr.Normal();                  // 0x000FA8C0
    const NiPoint3 nb{F((double(t[0][0]) * Nw.x + double(t[0][2]) * Nw.z) + double(t[0][1]) * Nw.y),
                      F((double(t[1][2]) * Nw.z + double(t[1][1]) * Nw.y) + double(t[1][0]) * Nw.x),
                      F((double(t[2][2]) * Nw.z + double(t[2][1]) * Nw.y) + double(t[2][0]) * Nw.x)};
    {
        const NiPoint3 a{std::fabs(nb.x), std::fabs(nb.y), std::fabs(nb.z)};   // 0x000A91B0
        const double rr = (double(ext.z) * a.z + double(ext.y) * a.y) + double(ext.x) * a.x;
        const double dd = (double(nb.z) * V[0].z + double(nb.y) * V[0].y) + double(nb.x) * V[0].x;
        const double pen = rr - std::fabs(dd);
        if (pen < 0.0) return false;                  // TEST AH,5; JP
        depth[3] = F(pen);
        const f32 s = rr < 0.0 ? -1.0f : 1.0f;        // (rr >= 0 for the shipped extents)
        normal[3] = {F(double(nb.x) * s), F(double(nb.y) * s), F(double(nb.z) * s)};
        tmpSign = s;
    }
    // axes 4..12: box axis j x triangle edge k
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) {
            const int idx = 4 + 3 * j + k;
            const NiPoint3& a = kAxes[j];
            const NiPoint3& Ek = E[k];
            const NiPoint3 c{F(double(Ek.z) * a.y - double(a.z) * Ek.y), F(double(a.z) * Ek.x - double(a.x) * Ek.z),
                             F(double(a.x) * Ek.y - double(Ek.x) * a.y)};
            const double L0 = std::sqrt((double(c.z) * c.z + double(c.y) * c.y) + double(c.x) * c.x);
            NiPoint3 n{};
            double len = 0.0;
            if (L0 > double(1e-6f)) {                 // [0x00293ACC]; TEST AH,0x41; JNE
                const double inv = 1.0 / L0;
                n = {F(double(c.x) * inv), F(double(c.y) * inv), F(double(c.z) * inv)};
                len = L0;
            }
            if (len < 1e-4) {                           // a double constant (0x002A1F30)
                depth[idx] = kNoAxis;
                normal[idx] = {-kUp.x, -kUp.y, -kUp.z};   // -(0x002D77EC)
                continue;
            }
            const double rr = (double(ext.z) * std::fabs(n.z) + double(ext.y) * std::fabs(n.y)) +
                              double(ext.x) * std::fabs(n.x);
            const NiPoint3& Vk = V[k];
            const NiPoint3& Vo = V[(k + 1) % 3];
            const double p = (double(n.x) * Vk.x + double(n.z) * Vk.z) + double(n.y) * Vk.y;
            const double q = (double(n.z) * Vo.z + double(n.y) * Vo.y) + double(n.x) * Vo.x;
            double hi = q, lo = p;
            if (p > q) hi = F(p), lo = F(q);          // (the swapped pair goes through float stores)
            const f32 pPos = F(hi - (-rr));
            const double pNeg = rr - lo;
            if (pPos < 0.0f || pNeg < 0.0) return false;
            if (double(pPos) < pNeg) {
                depth[idx] = pPos;
                normal[idx] = n;
            } else {
                depth[idx] = F(pNeg);
                normal[idx] = {-n.x, -n.y, -n.z};
            }
        }
    // the least penetration
    f32 m = kNoAxis;
    int best = -1;
    for (int i = 0; i < 13; ++i)
        if (depth[i] < m) {                           // TEST AH,5; JP
            m = depth[i];
            best = i;
        }
    if (best < 0) return false;
    outDepth = m;
    const auto& R = rot.m;
    const NiPoint3& nn = normal[best];
    outN = {F((double(R[0][2]) * nn.z + double(R[0][1]) * nn.y) + double(R[0][0]) * nn.x),
            F((double(R[1][2]) * nn.z + double(R[1][0]) * nn.x) + double(R[1][1]) * nn.y),
            F((double(R[2][2]) * nn.z + double(R[2][0]) * nn.x) + double(R[2][1]) * nn.y)};
    if (!respond) return true;
    // the contact point
    NiPoint3 cp{};
    // the box-space average of the counted points to world (the face and normal branches)
    const auto average = [&](const NiPoint3& sum, s32 count, bool yRound) {
        const double inv = 1.0 / double(count);       // FILD; FDIVR [1.0]
        const double a = double(sum.x) * inv, b = inv * sum.y, c = double(sum.z) * inv;
        const f32 x = F((c * R[0][2] + b * R[0][1]) + a * R[0][0]);
        const f32 y = F((c * R[1][2] + b * R[1][1]) + a * R[1][0]);
        const f32 z = F((c * R[2][2] + b * R[2][1]) + a * R[2][0]);
        cp.x = F(double(x) + pos.x);
        cp.z = F(double(z) + pos.z);
        // (the normal branch: FADD [1.0]; FSUB [1.0] before the store)
        cp.y = yRound ? F(((double(y) + pos.y) + 1.0f) - 1.0f) : F(double(y) + pos.y);
    };
    if (best >= 4) {                                  // edge-edge
        const int jj = (best - 4) / 3, kk = (best - 4) % 3;
        const NiPoint3 A = V[(kk + 1) % 3], B = V[(kk + 2) % 3];
        NiPoint3 S0{F(double(normal[best].x > 0.0f ? -1.0f : 1.0f) * ext.x),
                    F(double(normal[best].y > 0.0f ? -1.0f : 1.0f) * ext.y),
                    F(double(normal[best].z > 0.0f ? -1.0f : 1.0f) * ext.z)};   // [0x00293BF0] / [1.0]
        NiPoint3 S1 = S0;
        Comp(S1, jj) = -Comp(S1, jj);                 // FCHS
        const auto toWorldA = [&](const NiPoint3& v) {   // (A.y, A.z, A.x) / (A.y, A.x, A.z) orders
            return NiPoint3{F((double(v.y) * R[0][1] + double(v.z) * R[0][2]) + double(v.x) * R[0][0]),
                            F((double(v.y) * R[1][1] + double(v.x) * R[1][0]) + double(v.z) * R[1][2]),
                            F((double(v.y) * R[2][1] + double(v.x) * R[2][0]) + double(v.z) * R[2][2])};
        };
        const auto toWorldS = [&](const NiPoint3& v) {   // the (z, y, x) order, then + pos
            const f32 x = F((double(v.z) * R[0][2] + double(v.y) * R[0][1]) + double(v.x) * R[0][0]);
            const f32 y = F((double(v.z) * R[1][2] + double(v.y) * R[1][1]) + double(v.x) * R[1][0]);
            const f32 z = F((double(v.z) * R[2][2] + double(v.y) * R[2][1]) + double(v.x) * R[2][0]);
            return NiPoint3{F(double(x) + pos.x), F(double(y) + pos.y), F(double(z) + pos.z)};
        };
        const NiPoint3 Aw = toWorldA(A), Bw = toWorldA(B);
        const double ax = double(Aw.x) + pos.x, ay0 = double(Aw.y) + pos.y;
        const f32 az = F(double(Aw.z) + pos.z);
        const double bx = double(Bw.x) + pos.x;
        const f32 by1 = F((double(Bw.y) + pos.y) + 0.01f);   // [0x00293AAC]
        const f32 bz = F(double(Bw.z) + pos.z);
        const NiPoint3 S0w = toWorldS(S0), S1w = toWorldS(S1);
        const f32 ay = F((ay0 + 0.01f) + 0.01f);
        const f32 by = F(double(by1) + 0.01f);
        const NiPoint3 dP = sub(S1w, S0w);
        const double dTx = bx - ax, dTy = double(by) - ay, dTzu = double(bz) - az;
        const f32 dTz = F(dTzu);                      // (FST: the products below use the unrounded value first)
        const double b = (dTzu * dP.z + dTy * dP.y) + dTx * dP.x;
        const double a = (double(dP.z) * dP.z + double(dP.y) * dP.y) + double(dP.x) * dP.x;
        const f32 wx = F(ax - S0w.x), wy = F(double(ay) - S0w.y);
        const double wzu = double(az) - S0w.z;
        const f32 wz = F(wzu);
        const double dd = (wzu * dP.z + double(wy) * dP.y) + double(wx) * dP.x;
        const double e = (double(wz) * dTz + double(wy) * dTy) + double(wx) * dTx;
        const double c = (double(dTz) * dTz + dTy * dTy) + dTx * dTx;
        const f32 tt = F((dd * b - e * a) / (c * a - b * b));   // (no denominator test)
        cp = {F(double(F(dTx * tt)) + ax), F(dTy * tt + ay), F(double(F(double(dTz) * tt)) + az)};
    } else if (best < 3) {                            // a box face
        const double k = (double(Comp(ext, best)) - depth[best]) * Comp(normal[best], best);
        NiPoint3 sum{};
        s32 count = 0;
        for (int vi = 0; vi < 3; ++vi)
            if (std::fabs(double(Comp(V[vi], best)) - k) < 0.1f) {   // [0x00293AB0]; TEST AH,5; JP
                ++count;
                sum = {F(double(sum.x) + V[vi].x), F(double(sum.y) + V[vi].y), F(double(sum.z) + V[vi].z)};
            }
        if (count > 0) average(sum, count, false);
    } else {                                          // the triangle normal: the box corners on its plane
        const double k = -(double(tmpSign) * depth[3]);
        NiPoint3 sum{};
        s32 count = 0;
        for (int ci = 0; ci < 8; ++ci) {
            NiPoint3 C = ext;
            if (ci & 1) C.x = -C.x;
            if (ci & 2) C.y = -C.y;
            if (ci & 4) C.z = -C.z;
            const double val = (((double(C.z) - V[0].z) * nb.z + (double(C.y) - V[0].y) * nb.y) +
                                (double(C.x) - V[0].x) * nb.x) - k;
            if (std::fabs(val) < 0.1f) {
                ++count;
                sum = {F(double(sum.x) + C.x), F(double(sum.y) + C.y), F(double(sum.z) + C.z)};
            }
        }
        if (count > 0) average(sum, count, true);
    }
    // the response
    UpdateDerived();                                  // 0x000ADA00
    const NiPoint3 r{F(double(cp.x) - pos.x), F(double(cp.y) - pos.y), F(double(cp.z) - pos.z)};
    const NiPoint3 vp = PointVelocity(r);             // 0x000AD600
    const NiPoint3& N = outN;
    const double vn = std::fabs((double(vp.y) * N.y + double(vp.z) * N.z) + double(vp.x) * N.x);
    double j;
    if (vn < 20.0f) {                                 // [0x00295AB4]; TEST AH,5; JP
        j = ImpulseMagnitude(*this, N, cp, vp, 0.0f, 0.05f);
    } else {                                          // a hard impact
        j = ImpulseMagnitude(*this, N, cp, vp, 0.2f, 0.05f);
        flags |= 0x20;
    }
    if (j > 0.0) {                                    // TEST AH,0x41; JNE
        const NiPoint3 J{F(j * N.x), F(j * N.y), F(j * N.z)};
        ApplyImpulse({F(double(cp.x) - pos.x), F(double(cp.y) - pos.y), F(double(cp.z) - pos.z)}, J);   // 0x000AD720
        const double fn = (double(force.z) * N.z + double(force.y) * N.y) + double(force.x) * N.x;
        const f32 fy = F(fn * N.y), fz = F(fn * N.z);
        force = {F(double(force.x) - fn * N.x), F(double(force.y) - fy), F(double(force.z) - fz)};   // along N removed
    }
    UpdateDerived();
    flags |= N.y > 0.4f ? 1u : 2u;                    // [0x00293B28]; TEST AH,0x41; JNE
    return true;
}

void RigidBox::BoxBox(RigidBox& Y) {                  // orig: 0x000ACCD0 (4.11)
    RigidBox& X = *this;
    if (X.isStatic && Y.isStatic) return;
    const ni::NiMatrix3 TAm = Transpose(X.rot), TBm = Transpose(Y.rot);   // 0x00103C60
    const auto& TA = TAm.m;
    const auto& TB = TBm.m;
    double m = kNoAxis;                               // (on the x87 stack; only replaced by floats)
    NiPoint3 push{0.0f, 1.0f, 0.0f};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) {
            NiPoint3 a;
            if (i >= 3) {
                if (k >= 3) break;
                a = Col(Y.rot, k);                    // B_k (raw)
            } else {
                a = Col(X.rot, i);                    // A_i
                if (k < 3) {
                    const NiPoint3 b = Col(Y.rot, k);
                    a = {F(double(a.y) * b.z - double(a.z) * b.y), F(double(a.z) * b.x - double(a.x) * b.z),
                         F(double(a.x) * b.y - double(a.y) * b.x)};
                }
                const double len = std::sqrt((double(a.x) * a.x + double(a.z) * a.z) + double(a.y) * a.y);
                if (!(len > double(1e-6f))) continue; // [0x00293ACC]; TEST AH,0x41; JNE
                const double inv = 1.0 / len;
                a = {F(double(a.x) * inv), F(double(a.y) * inv), F(double(a.z) * inv)};
                if (len < double(1e-7f)) continue;      // [0x002A1F38] (never: L > 1e-6 here)
            }
            const f32 tA0 = F((double(a.z) * TA[0][2] + double(a.y) * TA[0][1]) + double(TA[0][0]) * a.x);
            const double tA1 = (double(a.z) * TA[1][2] + double(a.y) * TA[1][1]) + double(TA[1][0]) * a.x;
            const double tA2 = (double(a.y) * TA[2][1] + double(TA[2][0]) * a.x) + double(a.z) * TA[2][2];
            const f32 tB0 = F((double(TB[0][0]) * a.x + double(a.z) * TB[0][2]) + double(a.y) * TB[0][1]);
            const f32 tB1 = F((double(TB[1][0]) * a.x + double(a.z) * TB[1][2]) + double(a.y) * TB[1][1]);
            const f32 tB2 = F((double(TB[2][0]) * a.x + double(a.z) * TB[2][2]) + double(a.y) * TB[2][1]);
            const f32 cA = F((double(a.z) * X.pos.z + double(a.y) * X.pos.y) + double(a.x) * X.pos.x);
            const f32 cB = F((double(a.z) * Y.pos.z + double(a.y) * Y.pos.y) + double(a.x) * Y.pos.x);
            const f32 rA = F((std::fabs(tA2) * X.ext.z + std::fabs(tA1) * X.ext.y) + std::fabs(double(tA0)) * X.ext.x);
            const f32 rB = F((std::fabs(double(tB2)) * Y.ext.z + std::fabs(double(tB1)) * Y.ext.y) +
                             std::fabs(double(tB0)) * Y.ext.x);
            const double p1 = (double(rA) + cA) - (double(cB) - rB);
            if (p1 < m) {                             // TEST AH,5; JP (compared unrounded)
                const f32 mf = F(p1);
                m = mf;
                push = {F(double(a.x) * mf), F(double(a.y) * mf), F(double(a.z) * mf)};
            }
            const double p2 = (double(rB) + cB) - (double(cA) - rA);
            if (p2 < m) {
                const f32 mf = F(p2);
                m = mf;
                push = {F(-double(a.x) * mf), F(-double(a.y) * mf), F(-double(a.z) * mf)};
            }
            if (m < 0.0) return;                      // separated
        }
    if (m >= 10000.0f) return;                        // [0x0029E7FC]; TEST AH,1; JE (NaN goes on)
    if (Y.isStatic) {
        X.pos = {F(double(X.pos.x) - push.x), F(double(X.pos.y) - push.y), F(double(X.pos.z) - push.z)};
    } else if (X.isStatic) {
        Y.pos = {F(double(push.x) + Y.pos.x), F(double(push.y) + Y.pos.y), F(double(push.z) + Y.pos.z)};
    } else {                                          // half each (the halves are exact)
        const double hx = double(push.x) * 0.5f, hy = double(push.y) * 0.5f, hz = double(push.z) * 0.5f;
        X.pos = {F(double(X.pos.x) - hx), F(double(X.pos.y) - hy), F(double(X.pos.z) - hz)};
        Y.pos = {F(hx + Y.pos.x), F(hy + Y.pos.y), F(hz + Y.pos.z)};
    }
    const double len = std::sqrt((double(push.x) * push.x + double(push.y) * push.y) + double(push.z) * push.z);
    const double ny = len > double(1e-6f) ? (1.0 / len) * push.y : 0.0;
    if (ny > 0.4f) X.flags |= 0x10;                   // [0x00293B28]; TEST AH,0x41; JNE
    else if (-ny > 0.4f) Y.flags |= 0x10;
    X.flags |= 8;
    Y.flags |= 8;
}

// ---- the world ----------------------------------------------------------------------------------------------------
PhysWorld::~PhysWorld() {
    for (CharShape* s = shapes; s;) {
        CharShape* n = s->next;
        s->next = nullptr;
        s->world = nullptr;
        s = n;
    }
    for (RigidBox* b = boxes; b;) {
        RigidBox* n = b->next;
        b->next = nullptr;
        b->world = nullptr;
        b = n;
    }
}

void PhysWorld::AddShape(CharShape& s) {              // orig: 0x000ADFE0
    s.next = nullptr;
    s.world = this;
    CharShape** p = &shapes;
    while (*p) p = &(*p)->next;
    *p = &s;
    ++count;
}

void PhysWorld::RemoveShape(CharShape& s) {           // orig: 0x000AE020
    for (CharShape** p = &shapes; *p; p = &(*p)->next)
        if (*p == &s) {
            *p = s.next;
            s.next = nullptr;
            s.world = nullptr;
            --count;
            return;
        }
}

void PhysWorld::AddBox(RigidBox& b) {                 // orig: 0x000ADF30
    b.next = nullptr;
    b.world = this;
    RigidBox** p = &boxes;
    while (*p) p = &(*p)->next;
    *p = &b;
    ++count;
}

void PhysWorld::RemoveBox(RigidBox& b) {              // orig: 0x000ADF80
    for (RigidBox** p = &boxes; *p; p = &(*p)->next)
        if (*p == &b) {
            *p = b.next;
            b.next = nullptr;
            b.world = nullptr;
            --count;
            return;
        }
}

void CharShape::ResetContacts() {                     // orig: 0x000A9190 (0x000A9100 + flags)
    contactFlags.fill(0xFF);
    contacts.fill({});
    flags = 0;
}

void PhysWorld::Solve() {                             // orig: 0x000AE160 (3.2)
    if (!enabled) return;
    for (CharShape* s = shapes; s; s = s->next) {
        s->ResetContacts();
        if (s->kind == 1) {
            s->flags = 0;
            s->partner = nullptr;
        }
    }
    for (RigidBox* b = boxes; b; b = b->next)
        if (b->kind == 4) {
            b->standing = nullptr;
            b->flags = 0;
        }
    for (CharShape* a = shapes; a; a = a->next)
        for (CharShape* b = a->next; b; b = b->next)
            if (a->kind == 1 && b->kind == 1) a->CharChar(*b);
    for (CharShape* c = shapes; c; c = c->next)
        for (RigidBox* x = boxes; x; x = x->next)
            if (c->kind == 1 && x->kind == 4) c->CharBox(*x);
    for (RigidBox* x = boxes; x; x = x->next)        // the box pairs 0x000ACCD0 (two static boxes: nothing)
        for (RigidBox* y = x->next; y; y = y->next)
            if (x->kind == 4 && y->kind == 4) x->BoxBox(*y);
    for (CharShape* s = shapes; s; s = s->next) {
        if (s->kind != 1 || s->pinned) continue;
        const NiPoint3 save = s->ext;
        ni::g_exclude = s->excludeNode;               // orig: 0x000AE2BE DAT_002F8EEC = shape +0xC8 (QueryMeshes)
        NiPoint3 q;
        if (s->b1 && benderFeet && benderFeet(q)) {
            const double dx = double(q.x) - s->pos.x, dy = double(q.y) - s->pos.y, dz = double(q.z) - s->pos.z;
            if ((dz * dz + dy * dy) + dx * dx < 400.0) {   // TEST AH,5; JP skips (>= / NaN)
                s->ext.x = F(double(s->ext.x) + 3.0f);
                s->ext.z = F(double(s->ext.z) + 3.0f);
            }
        }
        const f32 r = F(std::sqrt((double(s->ext.x) * s->ext.x + double(s->ext.y) * s->ext.y) +
                                  double(s->ext.z) * s->ext.z));
        std::vector<TriRef> tris = QuerySphere(s->pos, r, 3);
        s->Slide(tris);
        s->ext = save;
        ni::g_exclude = nullptr;                      // 0x000AE3D6
    }
}

void Physic::Accumulate(f32 dt) { acc = F(double(dt) + acc); }   // orig: 0x000AE860

const std::vector<PhysObject*>& Physic::QueryObjects(const PhysObject* exclude, const NiPoint3& c, const f32& r) {
    queryResult.clear();                              // orig: 0x000AEB10 (entries zeroed, count / used = 0)
    for (PhysObject* o = g_objects; o; o = o->next)   // DAT_002E9C40, next +4
        if (o != exclude && o->OverlapsSphere(c, r)) queryResult.push_back(o);   // vf1C; 0x000A4A20
    return queryResult;
}

PhysObject* Physic::RayCastObjects(const PhysObject* ignore, const NiPoint3& o, const NiPoint3& d, f32& len,
                                   bool frontOnly) const {   // orig: vf03 0x000AE920
    PhysObject* hit = nullptr;
    for (PhysObject* p = g_objects; p; p = p->next)   // DAT_002E9C40, next +4
        if (p != ignore && p->RayTest(o, d, len, frontOnly)) hit = p;   // vf18
    return hit;
}

void Physic::Step() {                                 // orig: 0x000AE870 (3.1)
    for (PhysObject* o = g_objects; o;) {
        PhysObject* n = o->next;
        o->Integrate(acc);
        o = n;
    }
    world.Solve();
    acc = 0;
}

// ---- the pair tests ----------------------------------------------------------------------------------------------
void CharShape::CharChar(CharShape& b) {              // orig: 0x000A9E30 (3.3)
    CharShape& a = *this;
    if ((a.pinned || a.b9) && !b.pinned && !b.b9) b.CharChar(a);   // (then continues)
    u32 steps = 1;
    bool bothFree = false;
    NiPoint3 d{};
    if (!a.pinned && !a.b9) {
        if (!b.pinned && !b.b9) bothFree = true;
        d = {F(double(a.pos.x) - a.slideStart.x), F(double(a.pos.y) - a.slideStart.y), F(double(a.pos.z) - a.slideStart.z)};
        steps = SubSteps(d, a.ext, false);
        const double inv = 1.0 / double(steps);
        a.pos = a.slideStart;
        d = {F(double(d.x) * inv), F(double(d.y) * inv), F(double(d.z) * inv)};
    } else {
        a.slideStart = a.pos;
        d = {};
    }
    for (u32 step = 0; step < steps; ++step) {
        a.pos = {F(double(d.x) + a.pos.x), F(double(d.y) + a.pos.y), F(double(d.z) + a.pos.z)};
        f32 m = kNoAxis;                              // (on the x87 stack; only replaced by floats)
        NiPoint3 dir{};
        const NiPoint3 rel{F(double(b.pos.x) - a.pos.x), F(double(b.pos.y) - a.pos.y), F(double(b.pos.z) - a.pos.z)};
        bool separated = false;
        for (int i = 0; i < 3 && !separated; ++i) {
            const double S = i == 0 ? double(Comp(b.ext, i)) + Comp(a.ext, i) : double(Comp(a.ext, i)) + Comp(b.ext, i);
            const f32 Sf = F(S);
            const double p1 = S - Comp(rel, i);
            if (p1 <= 0.0) {                          // TEST AH,0x41; JNP (NaN continues)
                separated = true;
                break;
            }
            if (p1 < double(m)) {
                m = F(p1);
                dir = {-0.0f, -0.0f, -0.0f};          // FCHS of the axis: the zero components become -0.0
                Comp(dir, i) = -1.0f;
            }
            const double p2 = double(Sf) + Comp(rel, i);
            if (p2 <= 0.0) {
                separated = true;
                break;
            }
            if (!(bothFree && i == 1) && F(p2) < m) {
                m = F(p2);
                dir = kAxes[i];
            }
        }
        if (separated) continue;
        a.partnerIsBox = 0;
        a.partner = &b;
        b.partnerIsBox = 0;
        b.partner = &a;
        if (a.b1 || b.b1 || a.noPush || b.noPush) continue;
        if (m < kNoAxis) {                            // TEST AH,5; JP skips
            const double mm = double(m) + double(1e-4f);
            if (!b.pinned && !b.b9) {
                if (!a.pinned) {                      // both move, half each
                    const double h = mm * 0.5;
                    a.pos.x = F(double(dir.x) * h + a.pos.x);
                    a.pos.y = F(double(dir.y) * h + a.pos.y);
                    a.pos.z = F(double(F(double(dir.z) * h)) + a.pos.z);
                    b.pos.x = F(double(F(-double(dir.x) * h)) + b.pos.x);
                    b.pos.y = F(-double(dir.y) * h + b.pos.y);
                    b.pos.z = F(double(F(-double(dir.z) * h)) + b.pos.z);
                } else {                              // a pinned: b moves the full amount
                    b.pos.x = F(double(b.pos.x) - double(dir.x) * mm);
                    b.pos.y = F(double(b.pos.y) - double(F(double(dir.y) * mm)));
                    b.pos.z = F(double(b.pos.z) - double(F(double(dir.z) * mm)));
                }
            } else {                                  // b pinned: a moves the full amount
                a.pos.x = F(double(dir.x) * mm + a.pos.x);
                a.pos.y = F(double(F(double(dir.y) * mm)) + a.pos.y);
                a.pos.z = F(double(F(double(dir.z) * mm)) + a.pos.z);
            }
            if (dir.y > 0.8f) a.flags |= 0x10;        // orig: 0x00293C18
            else if (-dir.y > 0.8f) b.flags |= 0x10;
        }
        a.flags |= 8;
        b.flags |= 8;
    }
}

void CharShape::CharBox(RigidBox& x) {                // orig: 0x000AA8A0 (3.4)
    CharShape& c = *this;
    if (c.pinned && x.isStatic) return;
    NiPoint3 d{F(double(c.pos.x) - c.slideStart.x), F(double(c.pos.y) - c.slideStart.y), F(double(c.pos.z) - c.slideStart.z)};
    const u32 steps = SubSteps(d, c.ext, false);
    const double inv = 1.0 / double(steps);
    c.pos = c.slideStart;
    d = {F(double(d.x) * inv), F(double(d.y) * inv), F(double(d.z) * inv)};
    // the box axes u_k = the 16-byte vector k of the rotation (+0x90 + 0x10 * k) = column k (NiMath.h layout)
    auto boxAxis = [&](int k) { return NiPoint3{x.rot.m[0][k], x.rot.m[1][k], x.rot.m[2][k]}; };
    for (u32 step = 0; step < steps; ++step) {
        c.pos = {F(double(d.x) + c.pos.x), F(double(d.y) + c.pos.y), F(double(d.z) + c.pos.z)};
        f32 m = 10000.0f;                             // orig: 0x0029E7FC
        NiPoint3 push{0, 1, 0};
        bool separated = false;
        for (int i = 0; i < 4 && !separated; ++i)
            for (int k = 0; k < 4; ++k) {
                if (i == 3 && k == 3) break;
                NiPoint3 a = i < 3 ? kAxes[i] : boxAxis(k);
                if (i < 3 && k < 3) {
                    const NiPoint3 u = boxAxis(k);
                    const NiPoint3 cr{F(double(a.y) * u.z - double(a.z) * u.y), F(double(a.z) * u.x - double(a.x) * u.z),
                                      F(double(a.x) * u.y - double(a.y) * u.x)};
                    const double L2 = (double(cr.x) * cr.x + double(cr.z) * cr.z) + double(cr.y) * cr.y;
                    if (L2 < double(1e-5f)) continue; // orig: 0x002A0FE8 (parallel: the axis is skipped)
                    const double L = std::sqrt(L2);
                    if (L <= double(1e-6f)) {
                        a = {};
                    } else {
                        const double il = 1.0 / L;
                        a = {F(double(cr.x) * il), F(double(cr.y) * il), F(double(cr.z) * il)};
                    }
                } else if (i < 3) {
                    a = kAxes[i];
                }
                const NiPoint3 u0 = boxAxis(0), u1 = boxAxis(1), u2 = boxAxis(2);
                const f32 t0 = F((double(u0.x) * a.x + double(a.z) * u0.z) + double(a.y) * u0.y);
                const double t1 = (double(u1.x) * a.x + double(a.z) * u1.z) + double(a.y) * u1.y;
                const double t2 = (double(u2.x) * a.x + double(a.z) * u2.z) + double(a.y) * u2.y;
                const f32 pc = F((double(a.z) * c.pos.z + double(a.y) * c.pos.y) + double(a.x) * c.pos.x);
                const f32 pb = F((double(a.z) * x.pos.z + double(a.y) * x.pos.y) + double(a.x) * x.pos.x);
                const f32 rc = F((std::fabs(double(a.z)) * c.ext.z + std::fabs(double(a.y)) * c.ext.y) +
                                 std::fabs(double(a.x)) * c.ext.x);
                const f32 rb = F((std::fabs(t2) * x.ext.z + std::fabs(t1) * x.ext.y) + std::fabs(double(t0)) * x.ext.x);
                const double p1 = (double(rc) + pc) - (double(pb) - rb);
                if (p1 < double(m)) {
                    m = F(p1);
                    push = {F(double(a.x) * m), F(double(a.y) * m), F(double(a.z) * m)};
                }
                const double p2 = (double(rb) + pb) - (double(pc) - rc);
                if (p2 < double(m)) {
                    m = F(p2);
                    push = {F(-double(a.x) * m), F(-double(a.y) * m), F(-double(a.z) * m)};
                }
                if (m < 0.0f) {
                    separated = true;
                    break;
                }
            }
        if (separated) continue;
        if (c.pinned) {                               // a pinned character: the box takes the whole push
            x.pos = {F(double(push.x) + x.pos.x), F(double(push.y) + x.pos.y), F(double(push.z) + x.pos.z)};
            const double rs = 1.0 / game::g_frameStep;   // FLD [1.0]; FDIV [0x002D67E8] (unrounded)
            x.SetVelocity({F(double(push.x) * rs), F(double(push.y) * rs), F(double(push.z) * rs)});   // 0x000AD8A0
        } else if (!x.isStatic) {                     // a dynamic box: each side moves 0.6 of the push
            const double hx = double(push.x) * 0.6f, hy = double(push.y) * 0.6f;   // [0x002976C0]
            const f32 hz = F(double(push.z) * 0.6f);
            c.pos = {F(double(c.pos.x) - hx), F(double(c.pos.y) - hy), F(double(c.pos.z) - hz)};
            x.pos = {F(hx + x.pos.x), F(hy + x.pos.y), F(double(hz) + x.pos.z)};
            const double rs = 1.0 / game::g_frameStep;
            const f32 frs = F(rs);                    // (FST)
            x.SetVelocity({F(hx * rs), F(hy * frs), F(double(hz) * frs)});
        } else {                                      // static box: the character takes the full push
            c.pos = {F(double(c.pos.x) - push.x), F(double(c.pos.y) - push.y), F(double(c.pos.z) - push.z)};
            NiPoint3 n{F(-double(push.x)), F(-double(push.y)), F(-double(push.z))};
            const double L = std::sqrt((double(n.x) * n.x + double(n.z) * n.z) + double(n.y) * n.y);
            if (L > double(1e-6f)) {
                const double il = 1.0 / L;
                n = {F(double(n.x) * il), F(double(n.y) * il), F(double(n.z) * il)};
                if (n.y < -0.4f || n.y > 0.4f) {      // orig: 0x002951DC / 0x00293B28
                    c.flags |= 0x10;
                    x.flags |= 0x10;
                    x.standing = &c;
                }
            }
        }
        c.flags |= 8;
        x.flags |= 8;
        c.partnerIsBox = 1;
        c.partner = &x;
    }
}

// ---- the slide ----------------------------------------------------------------------------------------------------
bool CharShape::TriBox(const TriRef& tr, NiPoint3& outDir, f32& outDepth) {   // orig: 0x000A9240 (3.8)
    CharShape& s = *this;
    const u32 tb = tr.Flags();
    const NiPoint3& v0 = tr.V(0);
    const NiPoint3& v1 = tr.V(1);
    const NiPoint3& v2 = tr.V(2);
    const NiPoint3& c = s.pos;
    const NiPoint3& e = s.ext;
    const NiPoint3* V[3] = {&v0, &v1, &v2};
    NiPoint3 r[3];
    for (int k = 0; k < 3; ++k)
        r[k] = {F(double(V[k]->x) - c.x), F(double(V[k]->y) - c.y), F(double(V[k]->z) - c.z)};
    auto sub = [](const NiPoint3& a, const NiPoint3& b) {
        return NiPoint3{F(double(a.x) - b.x), F(double(a.y) - b.y), F(double(a.z) - b.z)};
    };
    const NiPoint3 E[3] = {sub(r[2], r[1]), sub(r[0], r[2]), sub(r[1], r[0])};   // edge k is opposite vertex k
    const u32 b0 = tb & 0xFF;
    bool is10 = (b0 & 0x10) != 0, is40 = (b0 & 0x40) != 0, both = false;
    if (is10 && is40) {
        both = true;
        is10 = is40 = false;
    }
    const bool bit16 = (tb >> 16) & 1, bit17 = (tb >> 17) & 1, bit18 = (tb >> 18) & 1;
    const bool edgeOn[3] = {bit17, bit18, bit16};
    const bool vtxOn[3] = {bit16 || bit18, bit16 || bit17, bit17 || bit18};
    f32 pen[13];
    NiPoint3 dir[13];
    for (int i = 0; i < 3; ++i) {                     // the world axes, from the world vertex coordinates
        f32 mn = Comp(*V[0], i), mx = Comp(*V[1], i);
        int iMin = 0, iMax = 1;
        if (Comp(*V[0], i) > Comp(*V[1], i)) {        // TEST AH,0x41; JNE (swap only when greater)
            mn = Comp(*V[1], i);
            iMin = 1;
            mx = Comp(*V[0], i);
            iMax = 0;
        }
        const f32 w2 = Comp(*V[2], i);
        if (mn > w2) {
            mn = w2;
            iMin = 2;
        } else if (mx < w2) {
            mx = w2;
            iMax = 2;
        }
        const f32 A = F((double(mx) - Comp(c, i)) - (-double(Comp(e, i))));
        const f32 B = F(double(Comp(e, i)) - (double(mn) - Comp(c, i)));
        if (A < 0.0f || B < 0.0f) return false;
        if (A < B) {
            pen[i] = vtxOn[iMax] ? A : kNoAxis;
            dir[i] = kAxes[i];
        } else {
            pen[i] = vtxOn[iMin] ? B : kNoAxis;
            dir[i] = {-0.0f, -0.0f, -0.0f};
            Comp(dir[i], i) = -1.0f;
        }
    }
    // axis 3: the triangle normal (one-sided)
    const NiPoint3 n = tr.Normal();
    const f32 dd = F((double(n.x) * r[0].x + double(r[0].y) * n.y) + double(n.z) * r[0].z);
    const double R = (std::fabs(double(n.z)) * e.z + std::fabs(double(n.y)) * e.y) + std::fabs(double(n.x)) * e.x;
    const double sD = R - std::fabs(double(dd));
    const f32 sN = F(sD);
    if (sD < 0.0) return false;
    pen[3] = dd < 0.0f ? sN : F((double(dd) + dd) + sN);   // TEST AH,5; JNP (NaN: the second form)
    dir[3] = n;
    // axes 4..12: box axis j x edge k
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) {
            const int idx = 4 + 3 * j + k;
            const NiPoint3& a = kAxes[j];
            const NiPoint3& Ek = E[k];
            const NiPoint3 cr{F(double(a.y) * Ek.z - double(Ek.y) * a.z), F(double(a.z) * Ek.x - double(a.x) * Ek.z),
                              F(double(Ek.y) * a.x - double(a.y) * Ek.x)};
            const double L = std::sqrt((double(cr.x) * cr.x + double(cr.z) * cr.z) + double(cr.y) * cr.y);
            if (L <= double(1e-6f)) {
                pen[idx] = kNoAxis;
                dir[idx] = kUp;
                continue;
            }
            const double il = 1.0 / L;
            const NiPoint3 u{F(double(cr.x) * il), F(double(cr.y) * il), F(double(cr.z) * il)};
            if (L < double(1e-4f)) {
                pen[idx] = kNoAxis;
                dir[idx] = kUp;
                continue;
            }
            const double Ru = (std::fabs(double(u.z)) * e.z + std::fabs(double(u.y)) * e.y) + std::fabs(double(u.x)) * e.x;
            const NiPoint3& rk = r[k];
            const NiPoint3& ro = r[(k + 1) % 3];
            const double p1 = (double(u.y) * rk.y + double(u.z) * rk.z) + double(u.x) * rk.x;
            const double p2 = (double(u.y) * ro.y + double(u.z) * ro.z) + double(u.x) * ro.x;
            double mx, mn;
            bool fMx, fMn;
            if (p1 > p2) {
                mx = F(p1);
                mn = F(p2);
                fMx = vtxOn[k];
                fMn = edgeOn[k];
            } else {
                mx = p2;
                mn = p1;
                fMx = edgeOn[k];
                fMn = vtxOn[k];
            }
            const f32 A = F(mx - (-Ru));
            const double B = Ru - mn;
            if (A < 0.0f || B < 0.0) return false;
            if (double(A) < B) {
                pen[idx] = fMx ? A : kNoAxis;
                dir[idx] = u;
            } else {
                pen[idx] = fMn ? F(B) : kNoAxis;
                dir[idx] = {-u.x, -u.y, -u.z};
            }
        }
    // choose
    auto normal = [&] {
        outDepth = pen[3];
        outDir = n;
        return true;
    };
    if (is40) return normal();
    if (both) {
        s.flags |= 0x80;
        return normal();
    }
    if (is10) {
        s.flags |= 0x40;
        if (n.y > 0.0f) return normal();
    }
    if (s.descending && !is10 && n.y > 0.4f) {        // step up
        f32 m = kNoAxis;
        for (int i = 0; i < 13; ++i)
            if (dir[i].y > 1e-4f) {
                const double q = double(pen[i]) / dir[i].y;
                if (q < double(m)) m = F(q);
            }
        if (m < s.stepLeft) {                         // FCOM [s+0xAC]; TEST AH,5; JP skips (>= / NaN)
            s.flags |= 1;
            outDepth = m;
            outDir = kUp;
            s.stepLeft = F(double(s.stepLeft) - m);
            return true;
        }
    }
    f32 m = kNoAxis;
    int best = -1;
    for (int i = 0; i < 13; ++i)
        if (pen[i] < m) {
            m = pen[i];
            best = i;
        }
    if (best < 0) return false;
    outDepth = m;
    outDir = dir[best];
    if (!is10) s.flags |= outDir.y > 0.4f ? 1u : 2u;
    return true;
}

void CharShape::Slide(std::vector<TriRef>& T) {       // orig: 0x000AA510 (3.7)
    CharShape& s = *this;
    if (s.pinned) return;
    NiPoint3 d{F(double(s.pos.x) - s.slideStart.x), F(double(s.pos.y) - s.slideStart.y), F(double(s.pos.z) - s.slideStart.z)};
    const u32 steps = SubSteps(d, s.ext, true);
    const double inv = 1.0 / double(steps);
    s.pos = s.slideStart;
    d = {F(double(d.x) * inv), F(double(d.y) * inv), F(double(d.z) * inv)};
    // classification, with s.pos == s.slideStart: bits into byte 3 of the triangles' own flags
    for (const TriRef& tr : T) {
        u32& tb = tr.Flags();
        const NiPoint3& v0 = tr.V(0);
        const NiPoint3 nrm = tr.Normal();
        u32 bits = 0;
        if (tb & 0x10) bits |= 8;
        if (double(v0.y) - double(Ftol(double(s.pos.y) - s.ext.y)) <= double(s.stepHeight)) bits |= 4;
        if (((double(s.pos.z) - v0.z) * nrm.z + (double(s.pos.x) - v0.x) * nrm.x) + (double(s.pos.y) - v0.y) * nrm.y <= 0.0)
            bits |= 2;
        if (!(nrm.y > 0.4f)) bits |= 1;
        tb |= bits << 24;
    }
    if (!T.empty()) crt::Qsort(T.data(), T.size(), sizeof(TriRef), CompareFlags);   // orig: 0x001679F0
    for (u32 step = 0; step < steps; ++step) {
        s.pos = {F(double(d.x) + s.pos.x), F(double(d.y) + s.pos.y), F(double(d.z) + s.pos.z)};
        s.stepLeft = s.stepHeight;
        u32 slot = 0;
        for (const TriRef& tr : T) {
            NiPoint3 dir;
            f32 depth = 0;
            if (!s.TriBox(tr, dir, depth)) continue;
            const u8 fl = static_cast<u8>(tr.Flags() & 0xFF);
            if (!(fl & 0x40) && !s.noPush) {
                depth = F(double(depth) + 0.01f);     // orig: 0x00293AAC
                s.flags |= 4;
                s.pos.x = F(double(dir.x) * depth + s.pos.x);
                s.pos.y = F(double(dir.y) * depth + s.pos.y);
                s.pos.z = F(double(F(double(dir.z) * depth)) + s.pos.z);
            }
            if (slot < 8) {
                s.contacts[slot] = tr;
                s.contactFlags[slot] = fl;
                ++slot;
            } else if (fl & 0xC) {
                for (u32 j = 0; j < 8; ++j)
                    if (!(s.contactFlags[j] & 0xC)) {
                        s.contacts[j] = tr;
                        s.contactFlags[j] = fl;
                        break;
                    }
            }
        }
    }
    for (const TriRef& tr : T) tr.Flags() &= 0x00FFFFFFu;   // byte 3 (+0x13) = 0
    s.slideStart = s.pos;
}

}  // namespace fut::phys

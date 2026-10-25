// Collision ray cast; see NiCollision.h. x87 order of the original kept (dbl = stays on the x87 stack, F() = FSTP
// float); each comparison follows its FNSTSW idiom, NaN included.
#include "engine/ni/NiCollision.h"

#include "core/CrtQsort.h"
#include "core/CrtString.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace fut::ni {
namespace {

using detail::F;

constexpr f32 kFltMax = 3.4028234663852886e38f;      // orig: 0x002A6984

// The ray state of pass 1 (orig: globals; persistent between casts).
NiPoint3 g_org;                                       // orig: 0x002F8F54
NiPoint3 g_dir;                                       // orig: 0x002F8F80
f32 g_tMax = 0;                                       // orig: 0x002F4290
f32 g_inv[3] = {0, 0, 0};                             // orig: 0x002F9094 (refreshed only when |dir * t| > 1e-5)
bool g_cull = false;                                  // orig: 0x002F8EA4
u8 g_triMask = 0;                                     // orig: 0x002F8EF0
u32 g_rayCount = 0;                                   // orig: 0x002F8EA0

f32 Comp(const NiPoint3& p, int i) { return i == 0 ? p.x : i == 1 ? p.y : p.z; }
f32 AsFloat(u32 bits) {
    f32 f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void RefreshInverses() {                              // the tail of 0x000FAEE0 / the start of 0x000FB5C0
    for (int i = 0; i < 3; ++i) {
        const double p = double(Comp(g_dir, i)) * g_tMax;
        if (std::fabs(p) > double(1e-5f)) g_inv[i] = F(1.0 / p);   // TEST AH,0x41; JNZ skips (<= or NaN)
    }
}

// orig: 0x000FA950: the segment against a node box (min / max), in units of the current segment.
bool SlabTest(const NiPoint3& mn, const NiPoint3& mx) {
    f32 tmax = kFltMax;
    f32 tmin = F(-double(kFltMax));
    for (int i = 0; i < 3; ++i) {
        const double p = std::fabs(double(g_tMax) * Comp(g_dir, i));
        const f32 o = Comp(g_org, i), lo3 = Comp(mn, i), hi3 = Comp(mx, i);
        if (p < double(1e-5f)) {                      // TEST AH,5; JP (not less / NaN -> slab)
            if (o < lo3) return false;                // inclusive bounds
            if (o > hi3) return false;
            continue;
        }
        const f32 a = F((double(lo3) - o) * g_inv[i]);
        const double b = (double(hi3) - o) * g_inv[i];
        f32 lo;
        double hi;
        if (b < double(a)) {                          // FCOM; TEST AH,5; JP (not less / NaN keeps the order)
            lo = F(b);
            hi = a;
        } else {
            lo = a;
            hi = b;
        }
        if (double(tmax) > hi) tmax = F(hi);          // TEST AH,0x41; JNZ keeps (NaN keeps)
        if (tmax < 0.0f) return false;
        if (tmin < lo) tmin = lo;
        if (tmax < tmin) return false;
    }
    return true;
}

}  // namespace

// orig: 0x000FA8C0: the triangle normal scaled by its stored 1/|cross|.
NiPoint3 TriangleNormal(const NiPoint3& v0, const NiPoint3& v1, const NiPoint3& v2, u32 invCross) {
    const double e1x = double(v1.x) - v0.x, e1y = double(v1.y) - v0.y, e1z = double(v1.z) - v0.z;
    const f32 e2x = F(double(v2.x) - v0.x), e2y = F(double(v2.y) - v0.y);
    const double e2z = double(v2.z) - v0.z;
    const f32 cx = F(e2z * e1y - double(e2y) * e1z);
    const f32 cy = F(e1z * e2x - e2z * e1x);
    const double cz = e1x * e2y - double(e2x) * e1y;
    const f32 inv = AsFloat(invCross);
    return {F(double(cx) * inv), F(double(cy) * inv), F(cz * inv)};
}

NiPoint3 TriangleNormal(const NiBsp& b, const NiBsp::Triangle& t) {
    return TriangleNormal(b.vertices[t.v[0]], b.vertices[t.v[1]], b.vertices[t.v[2]], t.a);
}

// orig: 0x000FA810 (writes +0xC): the same edges and cross product as 0x000FA8C0, then 1 / |cross|.
u32 InvCrossBits(const NiPoint3& v0, const NiPoint3& v1, const NiPoint3& v2) {
    const double e1x = double(v1.x) - v0.x, e1y = double(v1.y) - v0.y, e1z = double(v1.z) - v0.z;
    const f32 e2x = F(double(v2.x) - v0.x), e2y = F(double(v2.y) - v0.y);
    const double e2z = double(v2.z) - v0.z;
    const f32 cx = F(e2z * e1y - double(e2y) * e1z);
    const f32 cy = F(e1z * e2x - e2z * e1x);
    const double cz = e1x * e2y - double(e2x) * e1y;
    const double L = std::sqrt((cz * cz + double(cy) * cy) + double(cx) * cx);
    if (L < double(1e-6f)) return 0;                  // FCOM [0x00293ACC]; TEST AH,5; JP (NaN: 1 / L)
    const f32 inv = F(1.0 / L);
    u32 bits;
    std::memcpy(&bits, &inv, 4);
    return bits;
}

const NiPoint3& RayHit::Vertex(int k) const { return triangle ? bsp->vertices[triangle->v[k]] : *mesh->v[k]; }

NiPoint3 RayHit::Normal() const {
    if (triangle) return TriangleNormal(*bsp, *triangle);
    return TriangleNormal(*mesh->v[0], *mesh->v[1], *mesh->v[2], mesh->invCross);
}

namespace {

NiPoint3 Normal(const NiBsp& b, const NiBsp::Triangle& t) { return TriangleNormal(b, t); }

const NiBsp::Triangle* LeafRayCast(const NiBsp& b, const NiBsp::TreeNode& node) {   // orig: 0x000FAEE0
    if (!SlabTest(node.boundMin, node.boundMax)) return nullptr;
    const NiBsp::Triangle* best = nullptr;
    for (u32 off : node.indices) {
        const size_t index = off / 0x14;              // (byte offsets into the triangle array)
        if (index >= b.triangles.size()) continue;
        const NiBsp::Triangle& T = b.triangles[index];
        if (static_cast<u8>(T.b) & g_triMask) continue;
        const NiPoint3 n = Normal(b, T);
        const f32 d = F((double(n.z) * g_dir.z + double(n.y) * g_dir.y) + double(g_dir.x) * n.x);
        if (g_cull) {
            if (d > -1e-6f) continue;                 // TEST AH,0x41; JZ (NaN passes)
        } else {
            if (std::fabs(d) < 1e-6f) continue;       // TEST AH,5; JNP (NaN passes)
        }
        const NiPoint3& v0 = b.vertices[T.v[0]];
        const double dx = double(v0.x) - g_org.x, dy = double(v0.y) - g_org.y, dz = double(v0.z) - g_org.z;
        const double t_ = ((dz * n.z + dy * n.y) + dx * n.x) / d;
        const f32 t = F(t_);
        if (t_ < 0.0) continue;                       // the dbl value (TEST AH,5; JNP)
        if (t > g_tMax) continue;                     // TEST AH,0x41; JZ (NaN passes)
        const f32 pz0 = F(double(g_dir.z) * t);
        const NiPoint3 p{F(double(g_dir.x) * t + g_org.x), F(double(g_dir.y) * t + g_org.y),
                         F(double(pz0) + g_org.z)};
        const double e = 1e-4f;
        if (double(node.boundMin.x) - e > p.x || double(node.boundMin.y) - e > p.y ||
            double(node.boundMin.z) - e > p.z)
            continue;                                 // TEST AH,0x41; JZ (NaN passes)
        if (double(node.boundMax.x) + e < p.x || double(node.boundMax.y) + e < p.y ||
            double(node.boundMax.z) + e < p.z)
            continue;                                 // TEST AH,5; JNP (NaN passes)
        const NiPoint3& v1 = b.vertices[T.v[1]];
        const NiPoint3& v2 = b.vertices[T.v[2]];
        const NiPoint3 q{F(double(p.x) - v0.x), F(double(p.y) - v0.y), F(double(p.z) - v0.z)};   // 0x00011620
        const NiPoint3 e1{F(double(v1.x) - v0.x), F(double(v1.y) - v0.y), F(double(v1.z) - v0.z)};   // 0x00011530
        const NiPoint3 e2{F(double(v2.x) - v0.x), F(double(v2.y) - v0.y), F(double(v2.z) - v0.z)};
        const f32 A = F((double(e1.z) * e1.z + double(e1.y) * e1.y) + double(e1.x) * e1.x);
        const f32 B = F((double(e2.z) * e2.z + double(e2.y) * e2.y) + double(e2.x) * e2.x);
        const f32 C = F((double(e2.z) * e1.z + double(e2.y) * e1.y) + double(e2.x) * e1.x);
        const double D = (double(e1.z) * q.z + double(e1.y) * q.y) + double(e1.x) * q.x;
        const double E = (double(e2.z) * q.z + double(e2.y) * q.y) + double(e2.x) * q.x;
        const double u = double(A) * E - D * C;
        const f32 uf = F(u);
        if (u < 0.0) continue;                        // TEST AH,5; JNP
        const double v = D * B - E * C;
        if (v < 0.0) continue;
        if ((double(B) * A - double(C) * C) < (double(uf) + v)) continue;   // FCOMPP; TEST AH,5; JNP
        g_tMax = t;
        best = &T;
    }
    if (best) RefreshInverses();
    return best;
}

const NiBsp::Triangle* NodeRayCast(const NiBsp& b, const NiBsp::TreeNode& node) {
    if (node.type == 3) return LeafRayCast(b, node);
    if (!SlabTest(node.boundMin, node.boundMax)) return nullptr;   // orig: inner 0x000FAD40
    if (node.children.size() < 2) return nullptr;
    const u32 axis = node.type < 3 ? node.type : 0;
    const bool lowFirst = Comp(g_org, static_cast<int>(axis)) < AsFloat(node.value);   // TEST AH,5; JP (NaN: high)
    const NiBsp::TreeNode& first = node.children[lowFirst ? 0 : 1];
    const NiBsp::TreeNode& second = node.children[lowFirst ? 1 : 0];
    if (const NiBsp::Triangle* r = NodeRayCast(b, first)) return r;
    return NodeRayCast(b, second);
}

// orig: NiBsp::RayCast vfA8 0x000FB5C0 (pass 1 of one BSP).
const NiBsp::Triangle* BspRayCast(const NiBsp& b, const NiPoint3& start, const NiPoint3& dir, f32& dist, bool cull) {
    g_org = start;
    g_dir = dir;
    g_tMax = dist;
    for (int i = 0; i < 3; ++i) {
        const double p = double(Comp(dir, i)) * dist;
        if (std::fabs(p) > double(1e-5f)) g_inv[i] = F(1.0 / p);
    }
    g_cull = cull;
    const NiBsp::Triangle* r = NodeRayCast(b, b.tree);
    dist = g_tMax;                                    // always written back
    return r;
}

}  // namespace

u8 SetTriangleMask(u8 mask) {                         // orig: 0x000F9D80 (a dword; only byte values are set)
    const u8 old = g_triMask;
    g_triMask = mask;
    return old;
}

// ---- the collision mesh records (collision_meshes_port.md 1 / 2) ----------------------------------------------------
using Rec = NiBsp::MeshRecord;
using Node = NiBsp::TreeNode;

NiStringExtraData* TagOnSelfOrAncestor(NiAVObject* obj, const char* prefix) {   // orig: 0x000CEAD0
    if (!obj || !prefix) return nullptr;
    const size_t len = std::strlen(prefix);
    for (NiAVObject* o = obj; o; o = o->parent)
        for (NiExtraData* e = o->extraData.Get(); e; e = e->nextData.Get())
            if (auto* s = dynamic_cast<NiStringExtraData*>(e))
                if (crt::StrNICmp(s->string.c_str(), prefix, len) == 0) return s;
    return nullptr;
}

namespace {

bool HasTree(const NiBsp& b) { return b.tree.type == 3 || !b.tree.children.empty(); }   // orig: +0xE4 != 0

// orig: GetName 0x000E2F70 non-null, then strncmp 0x001675D0 (case-sensitive; an empty name never matches).
bool NamePrefix(const NiAVObject* g, const char* p, size_t n) {
    return g && !g->name.empty() && std::strncmp(g->name.c_str(), p, n) == 0;
}

void KdRemove(Node& node, Rec& rec) {                 // orig: 0x000FA370 (not found: nothing changes)
    Rec* prev = nullptr;
    for (Rec* r = node.records; r; prev = r, r = r->next)
        if (r == &rec) {
            (prev ? prev->next : node.records) = r->next;
            rec.node = nullptr;
            return;
        }
}

void KdInsert(Node& root, Rec& rec) {                 // orig: vf18, inner 0x000FAE10 / leaf 0x000FA350
    Node* n = &root;
    while (n->type != 3 && n->children.size() == 2) {
        const int a = static_cast<int>(n->type);
        const double c = Comp(rec.center, a);
        const f32 split = AsFloat(n->value);
        if (c + rec.radius < split) {                 // FLD c; FADD r; FCOMP split; TEST AH,5; JP
            n = &n->children[0];
        } else if (c - rec.radius > split) {          // FLD c; FSUB r; FCOMP split; TEST AH,0x41; JNE stays
            n = &n->children[1];
        } else {
            break;                                    // (NaN: stays here)
        }
    }
    rec.next = n->records;                            // prepend
    n->records = &rec;
    rec.node = n;
}

}  // namespace

NiBsp::MeshRecord::~MeshRecord() {                    // orig: 0x000FBE80
    if (node) KdRemove(*node, *this);
}

void RecomputeMeshTypes(NiBsp& b) {                   // orig: 0x000FB6B0
    const size_t n = b.meshes.size();                 // (+0x118, read once)
    for (size_t i = 0; i < n; ++i) {
        Rec* rec = b.meshes[i].get();
        if (!rec) continue;
        NiTriShape* g = rec->geom.Get();              // (no null test in the original; every probed record is linked)
        rec->type = (TagOnSelfOrAncestor(g, "NiData_CamFade") || TagOnSelfOrAncestor(g, "NiData_CamNoColl") ||
                     NamePrefix(g, "sewer_fan_", 10) || NamePrefix(g, "Spinner", 7)) ? 2 : 1;   // 0x002A3E8C ..
    }
    b.typesDirty = false;                             // +0x124
}

void AddCollisionMeshes(NiBsp& b, NiAVObject* obj, u32 flags) {   // orig: 0x000FC110
    if (!obj) return;
    if (auto* n = dynamic_cast<NiNode*>(obj)) {       // DynamicCast 0x002F9230: every child slot, null ones too
        for (size_t i = 0; i < n->Children().size(); ++i) AddCollisionMeshes(b, n->Children()[i].Get(), flags);
        return;
    }
    auto* s = dynamic_cast<NiTriShape*>(obj);         // DynamicCast 0x0030EC38
    if (!s) return;
    auto rec = std::make_unique<Rec>();               // new(0x50) + 0x000FBDB0(s)
    rec->geom = s;
    Rec& r = *rec;
    b.meshes.push_back(std::move(rec));               // 0x000A5850
    r.type = (TagOnSelfOrAncestor(s, "NiData_CamFade") || TagOnSelfOrAncestor(s, "NiData_CamNoColl") ||
              NamePrefix(s, "sewer_fan_", 10)) ? 2 : 1;   // (no "Spinner" test here)
    const auto* data = dynamic_cast<const NiTriBasedGeomData*>(s->modelData.Get());
    const u16 count = data ? data->triangleCount : 0;   // (u16 data +0x34; the original has no null test)
    for (u16 i = 0; i < count; ++i) {
        r.triIndex.push_back(i);
        r.triFlags.push_back(static_cast<u16>(flags));
    }
}

void RemoveCollisionMeshes(NiBsp& b, NiAVObject* obj) {   // orig: 0x000FC2D0
    if (!obj) return;
    if (auto* n = dynamic_cast<NiNode*>(obj)) {
        for (size_t i = 0; i < n->Children().size(); ++i) RemoveCollisionMeshes(b, n->Children()[i].Get());
        return;
    }
    auto* s = dynamic_cast<NiTriShape*>(obj);
    if (!s) return;
    for (size_t i = 0; i < b.meshes.size(); ++i)
        if (b.meshes[i] && b.meshes[i]->geom.Get() == s) {   // the first one only
            std::unique_ptr<Rec> rec = std::move(b.meshes[i]);
            b.meshes.erase(b.meshes.begin() + static_cast<std::ptrdiff_t>(i));   // SetAt(i, 0) + Compact 0x000FA6C0
            return;                                   // ~MeshRecord (leaves its node) + delete
        }
}

void RefreshMeshes(NiBsp& b) {                        // orig: 0x000FBB10
    const size_t count = b.meshes.size();             // (+0x118, read once)
    for (size_t i = 0; i < count; ++i) {
        Rec* rec = b.meshes[i].get();
        if (!rec || !rec->geom) continue;
        const NiBound& wb = rec->geom->worldBound;    // G+0x20 / +0x2C
        const double dx = double(rec->center.x) - wb.center.x, dy = double(rec->center.y) - wb.center.y,
                     dz = double(rec->center.z) - wb.center.z, dr = double(rec->radius) - wb.radius;
        if (!(((dz * dz + dy * dy) + dx * dx) + dr * dr > double(0.01f))) continue;   // [0x002A61A0]; TEST AH,0x41; JNE
        rec->center = wb.center;
        rec->radius = wb.radius;
        if (rec->node) KdRemove(*rec->node, *rec);
        if (HasTree(b)) KdInsert(b.tree, *rec);       // vf18 (the original has no null test of the tree)
    }
}

void RefreshAllBsps() {                               // orig: 0x000FCF10
    for (NiBsp* b = g_bspList; b; b = b->nextInWorld) RefreshMeshes(*b);
}

// ---- the sphere query's mesh part (collision_meshes_port.md 3.3) -----------------------------------------------------
MeshScratch g_scratch;
NiBsp::MeshRecord* g_cand[0x400] = {};
u32 g_candCount = 0;
bool g_wantTris = true;                               // (the data section's 1)
bool g_wantRecs = false;
NiAVObject* g_exclude = nullptr;
NiTriShape* g_lastMeshHit = nullptr;

void ResetMeshScratch(u32 origAddr, s32 defaultRank) {
    g_scratch.count = 0;
    g_scratch.allocated = false;
    g_scratch.origAddr = origAddr ? origAddr : 0xF0000000u;   // (L) default: above every NiBsp's array
    g_scratch.defaultRank = origAddr ? -1 : defaultRank;
}

namespace {
// (port) the default sort key of the scratch triangles at their allocation: above `rank` of the linked NiBsp arrays
// (ascending by their keys) and below the others; no array's triangles [key, key + 0x14 * count) are overlapped.
u32 DefaultScratchKey(s32 rank) {
    std::vector<std::pair<u32, u32>> arrays;          // (key, end)
    for (const NiBsp* b = g_bspList; b; b = b->nextInWorld)
        arrays.emplace_back(b->origTriAddr, b->origTriAddr + 0x14u * u32(b->triangles.size()));
    std::sort(arrays.begin(), arrays.end());
    if (rank < 0 || size_t(rank) >= arrays.size()) return 0xF0000000u;
    if (rank == 0) return (arrays[0].first - 0x14u * MeshScratch::kCapacity - 0x10u) & ~0xFu;
    return (arrays[size_t(rank) - 1].second + 0x1Fu) & ~0xFu;
}
}  // namespace

void AppendRecords(const NiBsp::TreeNode& n) {        // orig: 0x000FA3C0
    for (Rec* r = n.records; r && g_candCount != 0x400; r = r->next) g_cand[g_candCount++] = r;
}

void QueryMeshes(NiBsp& b, const NiPoint3& c, f32 r, u32 flags) {   // orig: vfA4 0x000FC3A0
    if (b.typesDirty) RecomputeMeshTypes(b);
    for (u32 k = 0; k < g_candCount; ++k) {
        Rec* rec = g_cand[k];
        if (!rec) continue;
        NiTriShape* G = rec->geom.Get();
        if (!G || !G->modelData) continue;            // +0xC8
        if ((flags & 4) && rec->type == 2) continue;
        const NiBound& wb = G->worldBound;
        const double dx = double(wb.center.x) - c.x, dy = double(wb.center.y) - c.y, dz = double(wb.center.z) - c.z;
        const double R = double(wb.radius) + r;       // FLD G.r; FADD [r]
        if (((dz * dz + dy * dy) + dx * dx) > R * R) continue;   // FCOMP st1; TEST AH,0x41; JE (NaN proceeds)
        if (rec->triIndex.empty()) continue;          // +0x2C
        if (g_exclude) {                              // the character's own geometry (parent chain +0x14)
            NiAVObject* a = G;
            while (a && a != g_exclude) a = a->parent;
            if (a) continue;
        }
        MeshScratch& S = g_scratch;
        if (!S.allocated && S.defaultRank >= 0) S.origAddr = DefaultScratchKey(S.defaultRank);   // (port: the key)
        S.allocated = true;                          // orig: new(40000) + 2000 ctors, new(72000) on the first pass
        NiMatrix3 Rs;                                 // orig: 0x000AD900 (SSE mulps: each word rounded once)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) Rs.m[i][j] = F(double(G->world.rotate.m[i][j]) * G->world.scale);
        const NiPoint3 T = G->world.translate;        // +0xA0
        const auto& V = G->modelData->vertices;       // data +0x1C
        const auto* data = dynamic_cast<const NiTriShapeData*>(G->modelData.Get());
        if (!data) continue;                          // (port guard: a NiTriShape's vfB4 data is a NiTriShapeData)
        const auto& list = data->triangleList;
        for (size_t j = 0; j < rec->triIndex.size(); ++j) {
            if (S.count >= MeshScratch::kCapacity) break;   // JGE to the next record
            const u32 i = S.count++;
            HitTri& tri = S.tris[i];
            NiPoint3* w = &S.verts[3 * i];
            tri.flags = u32(rec->triFlags[j]) | 0x70000u;   // (zero-extended; byte 2 = 7: all three edges active)
            const u16 t3 = static_cast<u16>(3u * rec->triIndex[j]);   // GetTriangle 0x00115EB0 (u16 arithmetic)
            const u16 idx[3] = {list[t3], list[static_cast<u16>(t3 + 1)], list[static_cast<u16>(t3 + 2)]};
            for (int n = 0; n < 3; ++n) {
                const NiPoint3 m = MulVector(Rs, V[idx[n]]);   // 0x00011770, then FLD T; FADD; FSTP
                w[n] = {F(double(T.x) + m.x), F(double(T.y) + m.y), F(double(T.z) + m.z)};
            }
            bool reject = false;
            for (int a = 0; a < 3 && !reject; ++a) {   // the triangle's box against the query cube
                const f32 p0 = Comp(w[0], a), p1 = Comp(w[1], a), p2 = Comp(w[2], a);
                f32 mx = -1e6f, mn = 1e6f;            // orig: 0x002A61A4 / 0x49742400
                if (-1e6f < p0) mx = p0;              // TEST AH,5; JP (NaN keeps)
                if (1e6f > p0) mn = p0;               // TEST AH,0x41; JNE (NaN keeps)
                if (mx < p1) mx = p1;
                if (mn > p1) mn = p1;
                if (mx < p2) mx = p2;
                if (mn > p2) mn = p2;
                if (double(Comp(c, a)) - r > mx) reject = true;          // FCOMP st1; TEST AH,0x41; JE
                else if (double(r) + Comp(c, a) < mn) reject = true;     // FCOMP mn; TEST AH,5; JNP
            }
            if (reject) {
                --S.count;                            // (the slot is reused; its flags / vertices stay written)
                continue;
            }
            tri.v[0] = &w[0];
            tri.v[1] = &w[1];
            tri.v[2] = &w[2];
            tri.invCross = InvCrossBits(w[0], w[1], w[2]);   // 0x000FA810
        }
    }
}

// ---- ray-cast pass 2 (collision_meshes_port.md 4) ---------------------------------------------------------------------
namespace {

NiPoint3 g_segA, g_segB;                              // orig: 0x002F905C / 0x002F9068
struct SortRec {                                      // orig: 12 bytes at 0x002F56A0 [1024]
    NiBsp* bsp;
    u32 idx;
    f32 t;
};
SortRec g_sortRec[1024];
HitTri g_hit;                                         // orig: 0x002F8F18
NiPoint3 g_hitVerts[3];                               // orig: 0x002F8FF0 / 0x002F8FFC / 0x002F9008

void CollectSegment(const Node& n) {                  // orig: vf10, inner 0x000FAAF0 / leaf 0x000FA3C0
    if (n.type == 3 || n.children.size() < 2) {
        AppendRecords(n);
        return;
    }
    const int axis = static_cast<int>(n.type);
    const f32 split = AsFloat(n.value);
    const f32 a = Comp(g_segA, axis), b = Comp(g_segB, axis);
    const bool far = a > b;                           // FCOMP; TEST AH,0x41; JNE (<= or NaN: 0)
    const f32 hi = far ? a : b, lo = far ? b : a;
    if (hi < split) {                                 // TEST AH,5; JP
        CollectSegment(n.children[0]);
        AppendRecords(n);
        return;
    }
    if (lo >= split) {                                // TEST AH,1; JNE: < or NaN take the split case
        CollectSegment(n.children[1]);
        AppendRecords(n);
        return;
    }
    // the segment crosses the split: mid = A*f + B*(1-f), f = (split - b) / (a - b)
    const double f = (double(split) - b) / (double(a) - b);
    const double g = 1.0 - f;                         // FLD [0x00293A98]; FSUB st1
    const NiPoint3 A = g_segA, B = g_segB;
    const NiPoint3 mid{F(double(A.x) * f + double(F(double(B.x) * g))),
                       F(double(A.y) * f + double(F(double(B.y) * g))),
                       F(double(F(double(A.z) * f)) + double(B.z) * g)};   // (z: the A product is rounded)
    g_segB = mid;
    CollectSegment(n.children[far ? 1 : 0]);          // the A side first
    g_segA = mid;
    g_segB = B;
    CollectSegment(n.children[far ? 0 : 1]);
    g_segA = A;
    AppendRecords(n);
}

int CmpT(const void* x, const void* y) {              // orig: 0x000FA5F0
    const f32 a = static_cast<const SortRec*>(x)->t, b = static_cast<const SortRec*>(y)->t;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// orig: vfAC 0x000FC8C0: the record's triangles in the shape's model space.
const HitTri* RayCastMesh(u32 idx, const NiPoint3& start, const NiPoint3& dir, f32& dist, bool cull) {
    Rec& rec = *g_cand[idx];
    NiTriShape& G = *rec.geom;
    const NiMatrix3& Rw = G.world.rotate;             // +0x70 (12 words)
    const NiPoint3 T = G.world.translate;             // +0xA0
    const f32 s = G.world.scale;                      // +0xAC
    const NiPoint3 d{F(double(start.x) - T.x), F(double(start.y) - T.y), F(double(start.z) - T.z)};
    const NiPoint3 q = TransposeMulVector(Rw, d);     // 0x00103290
    const double is = 1.0 / double(s);                // (kept on the stack)
    const f32 isF = F(is);
    const NiPoint3 sl{F(double(q.x) * is), F(double(q.y) * is), F(double(q.z) * is)};   // the local start
    const NiPoint3 p = TransposeMulVector(Rw, dir);
    const NiPoint3 dl{F(double(p.x) * isF), F(double(p.y) * isF), F(double(p.z) * isF)};   // the local direction
    NiMatrix3 Rs;                                     // 0x000AD900
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) Rs.m[i][j] = F(double(Rw.m[i][j]) * s);
    if (!G.modelData) return nullptr;
    const auto* data = dynamic_cast<const NiTriShapeData*>(G.modelData.Get());
    if (!data) return nullptr;                        // (port guard: the vfB4 data of a NiTriShape)
    const auto& V = G.modelData->vertices;
    const auto& list = data->triangleList;
    const HitTri* result = nullptr;
    for (size_t k = 0; k < rec.triIndex.size(); ++k) {
        if (g_triMask && (u32(g_triMask) & rec.triFlags[k])) continue;   // (the dword mask against the u16)
        const u16 t3 = static_cast<u16>(3u * rec.triIndex[k]);
        const NiPoint3* vs[3] = {&V[list[t3]], &V[list[static_cast<u16>(t3 + 1)]], &V[list[static_cast<u16>(t3 + 2)]]};
        const NiPoint3 &v0 = *vs[0], &v1 = *vs[1], &v2 = *vs[2];
        const NiPoint3 n = TriangleNormal(v0, v1, v2, InvCrossBits(v0, v1, v2));   // 0x000FA810 / 0x000FA8C0
        const f32 dd = F((double(n.y) * dl.y + double(n.z) * dl.z) + double(n.x) * dl.x);   // ORDER y, z, x
        if (cull) {
            if (dd > -1e-6f) continue;                // [0x002A6174]; TEST AH,0x41; JE (NaN proceeds)
        } else {
            if (std::fabs(dd) < 1e-6f) continue;      // TEST AH,5; JNP
        }
        const double tt =
            (((double(v0.z) - sl.z) * n.z + (double(v0.y) - sl.y) * n.y) + (double(v0.x) - sl.x) * n.x) / dd;
        const f32 t = F(tt);
        if (tt < 0.0) continue;                       // (the double; TEST AH,5; JNP)
        if (t > dist) continue;                       // TEST AH,0x41; JE
        // the hit point: the LOCAL start plus t times the WORLD direction (as the original)
        const f32 px = F(double(t) * dir.x + sl.x);
        const double py = double(t) * dir.y + sl.y;
        const double pz = double(F(double(t) * dir.z)) + sl.z;
        const f32 qx = F(double(px) - v0.x), qy = F(py - v0.y);
        const double qz = pz - v0.z;
        const double e1x = double(v1.x) - v0.x, e1y = double(v1.y) - v0.y, e1z = double(v1.z) - v0.z;
        const f32 e2x = F(double(v2.x) - v0.x), e2y = F(double(v2.y) - v0.y), e2z = F(double(v2.z) - v0.z);
        const f32 A = F((e1z * e1z + e1y * e1y) + e1x * e1x);
        const f32 B = F((double(e2z) * e2z + double(e2y) * e2y) + double(e2x) * e2x);
        const f32 C = F((double(e2z) * e1z + double(e2y) * e1y) + double(e2x) * e1x);
        const f32 D = F((e1z * qz + e1y * qy) + e1x * qx);   // (stored; pass 1 keeps it)
        const double E = (qz * e2z + double(e2y) * qy) + double(e2x) * qx;
        const double u = double(A) * E - double(D) * C;
        const f32 uf = F(u);
        if (u < 0.0) continue;                        // TEST AH,5; JNP
        const double v = double(D) * B - E * C;
        if (v < 0.0) continue;
        if ((double(B) * A - double(C) * C) < (double(uf) + v)) continue;   // FCOMPP; TEST AH,5; JNP
        for (int m = 0; m < 3; ++m) {                 // world copies of the model vertices: 0x00011770 + T
            const NiPoint3 wv = MulVector(Rs, *vs[m]);
            g_hitVerts[m] = {F(double(T.x) + wv.x), F(double(T.y) + wv.y), F(double(T.z) + wv.z)};
        }
        g_hit.v[0] = &g_hitVerts[0];
        g_hit.v[1] = &g_hitVerts[1];
        g_hit.v[2] = &g_hitVerts[2];
        g_hit.flags = rec.triFlags[k];                // (zero-extended)
        dist = t;
        result = &g_hit;
    }
    if (result) {
        g_hit.invCross = InvCrossBits(g_hitVerts[0], g_hitVerts[1], g_hitVerts[2]);
        g_lastMeshHit = &G;
    }
    return result;
}

}  // namespace

RayHit RayCast(const NiPoint3& start, const NiPoint3& dir, f32& dist, bool cullBack, u32 flags) {   // orig: 0x000FB780
    g_lastMeshHit = nullptr;                          // (before the length test)
    const double lenSq = (double(dir.x) * dir.x + double(dir.y) * dir.y) + double(dir.z) * dir.z;
    if (lenSq < double(1e-6f)) return {};             // FCOM; TEST AH,5; JP (not less / NaN continue)
    const f32 invLenSq = F(1.0 / lenSq);
    ++g_rayCount;
    RayHit result;
    if (flags & 1)
        for (NiBsp* b = g_bspList; b; b = b->nextInWorld) {
            if (const NiBsp::Triangle* r = BspRayCast(*b, start, dir, dist, cullBack)) result = {b, r, nullptr};
        }
    // pass 2: the collision mesh records along the segment (0x000FB828-0x000FBB08)
    u32 nRec = 0;
    if (flags & 2) {
        g_candCount = 0;
        u32 i = 0;                                    // (the candidates accumulate over all NiBsps)
        for (NiBsp* b = g_bspList; b; b = b->nextInWorld) {
            if (b->typesDirty) RecomputeMeshTypes(*b);
            g_segA = start;
            const double L = dist;                    // (FLD [dist], kept)
            g_segB = {F(L * dir.x + start.x), F(double(F(L * dir.y)) + start.y), F(double(F(L * dir.z)) + start.z)};
            if (HasTree(*b)) {
                CollectSegment(b->tree);
                for (; i < g_candCount && i < 1024; ++i) g_sortRec[i] = {b, i, 0.0f};   // (t unset)
            }
        }
        for (u32 k = 0; k < i; ++k) {                 // the sphere prefilter, compacting in place
            Rec* rec = g_cand[g_sortRec[k].idx];
            if (!rec || !rec->geom) continue;
            if ((flags & 4) && rec->type == 2) continue;
            const NiBound& wb = rec->geom->worldBound;   // (the geometry's current bound, not the cached sphere)
            const double dx = double(start.x) - wb.center.x, dy = double(start.y) - wb.center.y,
                         dz = double(start.z) - wb.center.z, R = wb.radius;
            const f32 bq = F(((dx * invLenSq) * dir.x + double(F(dz * invLenSq)) * dir.z) + (dy * invLenSq) * dir.y);
            const double cc = (((dz * dz + dy * dy) + dx * dx) - R * R) * invLenSq;
            if (cc < 0.0) {                           // the start inside the sphere (TEST AH,5; JP; NaN: else)
                g_sortRec[nRec++] = {g_sortRec[k].bsp, g_sortRec[k].idx, 0.0f};
                continue;
            }
            const double disc = double(bq) * bq - cc;
            if (disc < 0.0 || disc == 0.0) continue;  // TEST AH,0x41; JNP (NaN proceeds)
            const double sq = std::sqrt(disc);
            const f32 t0 = F(-double(bq) - sq);
            if (sq - bq < 0.0) continue;              // TEST AH,5; JNP
            if (t0 > dist) continue;                  // TEST AH,0x41; JE (NaN proceeds)
            g_sortRec[nRec++] = {g_sortRec[k].bsp, g_sortRec[k].idx, t0};
        }
    }
    crt::Qsort(g_sortRec, nRec, sizeof(SortRec), CmpT);   // orig: 0x001679F0 (also with nothing to sort)
    for (u32 k = 0; k < nRec; ++k) {
        if (dist < g_sortRec[k].t) break;             // TEST AH,5; JNP (NaN goes on)
        if (const HitTri* h = RayCastMesh(g_sortRec[k].idx, start, dir, dist, cullBack)) result = {nullptr, nullptr, h};
    }
    return result;
}

}  // namespace fut::ni

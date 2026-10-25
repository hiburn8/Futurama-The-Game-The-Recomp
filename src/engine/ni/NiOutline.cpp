// NiOutline: the silhouette outline of a node's tri-based geometry (the HUD head's model, made an outline by
// 0x0013E5E0; the files' NiOutline nodes): the outline data 0x0013DAA0, the extra data 0x0013CA40, the display vf33
// 0x0013E8D0, the silhouette 0x00153F20 with the projection 0x00153DC0 and the edge direction 0x00153EA0, and the
// outline screen polygons 0x0013D440 / 0x0013E3F0 / 0x0013CEF0 / 0x0013C7E0 (renderer_lighting_port.md 8.2,
// renderer_plan.md 16). The x87 evaluation order is the original's (double intermediates, F() at the stores).
#include "engine/ni/NiCull.h"

#include "core/CrtQsort.h"
#include "engine/ni/NiMath.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fut::ni {

using detail::F;

NiPoint3* g_outlineScratch = nullptr;                 // orig: DAT_0030EBE0
NiGeometry* g_outlineGeometry = nullptr;              // orig: DAT_002FA500

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

f32 Bits(u32 u) { return std::bit_cast<f32>(u); }
const f32 kMinWidth = Bits(0x38D1B717);               // [0x00293B1C] 1e-4f
const f32 kWeldDistance2 = Bits(0x358637BE);          // [0x002A9E44] 1e-6f
const f32 kWeldRangeY = Bits(0x3A83126F);             // [0x00293AC4] 0.001f
const f32 kLineWidth = Bits(0x3B449BA6);              // [0x002D7B50] 0.003f (a static)
const f32 kAspect = Bits(0x3F28F5C3);                 // [0x002AAED8] 0.66f
const f32 kDepthBias = Bits(0x3727C5AC);              // [0x002D7B4C] 1e-5f (a static)
const f32 kClipLow = Bits(0xBDCCCCCD);                // [0x00293BF4] -0.1f
const f32 kClipHigh = Bits(0x3F8CCCCD);               // [0x00295644] 1.1f
const f32 kInv255 = Bits(0x3B808081);                 // [0x002A5FA0] 1 / 255
const f32 kPolygonAlpha = Bits(0x3F19999A);           // 0x0013D672: 0.6f
constexpr s32 kPolygonVertices = 0x194;               // the outline polygons' size
constexpr s32 kPoolSize = 0x2800;                     // DAT_00304608..DAT_0030E608

// The outline renderer's globals.
std::vector<NiPoint3> g_scratch;                      // DAT_0030F71C (capacity DAT_0030F724): the world vertices
std::vector<u8> g_vertexMark;                         // DAT_0030F720 (the same capacity)
std::vector<u8> g_drawn;                              // DAT_002FA4FC (capacity DAT_002FA4F8): per listed geometry
std::vector<u8> g_classify;                           // DAT_002FA47C (256 entries, built once)
s32 g_thirdVertex = 0;                                // DAT_002FA4D4 (0x0013C8A0's face test)
std::vector<NiPointer<NiScreenPolygon>> g_pool;       // DAT_00304608 (kPoolSize slots, kept across frames)
s32 g_poolIndex = 0;                                  // DAT_002FA4E0
NiScreenPolygon* g_polygon = nullptr;                 // DAT_002FA4E8 (the pool holds the reference)
s32 g_polygonCount = 0;                               // DAT_002FA4EC
u32 g_silhouetteEdges = 0;                            // DAT_002FA4DC (a statistic)
f32 g_near = 0, g_far = 0;                            // DAT_0030F718 / DAT_0030F714 (the camera's near / far)

// orig: the five shared properties of the outline polygons, made by static initialisers.
struct OutlineProperties {
    NiPointer<NiVertexColorProperty> vertexColor;     // DAT_00304568 (0x0022D340: +0x18 = 0, +0x1C = 1)
    NiPointer<NiZBufferProperty> zBuffer;             // DAT_0030E608 (0x0022D3D0: flags 3, function 3)
    NiPointer<NiTexturingProperty> texturing;         // DAT_0030455C (0x0022D450: NiTexturingProperty 0x00109DC0)
    NiPointer<NiAlphaProperty> alpha;                 // DAT_00304588 (0x0022D4B0: flags 0xEC, reference 0)
    NiPointer<NiShadeProperty> shade;                 // DAT_00304540 (0x0022D530: flags 1)
};
OutlineProperties& Properties() {
    static OutlineProperties p = [] {
        OutlineProperties o;
        o.vertexColor = new NiVertexColorProperty;
        o.vertexColor->flags = 0;
        o.vertexColor->sourceVertexMode = 0, o.vertexColor->lightingMode = 1;
        o.zBuffer = new NiZBufferProperty;
        o.zBuffer->flags = 3;
        o.zBuffer->testFunction = 3;
        o.texturing = new NiTexturingProperty;
        o.alpha = new NiAlphaProperty;                // flags 0xEC
        o.alpha->alphaTestRef = 0;
        o.shade = new NiShadeProperty;
        o.shade->flags = 1;
        return o;
    }();
    return p;
}

// orig: 0x0013C810, the weld sort's comparator: by y (+4); -1 / 1 / 0 (equal or NaN).
struct WeldRecord {
    f32 x, y, z;
    s32 index;
};
static_assert(sizeof(WeldRecord) == 16);
int CompareY(const void* pa, const void* pb) {
    const f32 a = static_cast<const WeldRecord*>(pa)->y, b = static_cast<const WeldRecord*>(pb)->y;
    if (a < b) return -1;                             // FCOMP; TEST AH,5; JP
    if (a > b) return 1;                              // TEST AH,0x41
    return 0;
}

// orig: vf38 0x001449D0 / vf3C 0x00144A60 (the geometry's world vertices +0xD0, recomputed while +0xD8 is set: every
// world-data update sets it) with the transform DAT_002D7E04 = 0x00150F80: per row
// F((m2 z + F(m1 y + F(m0 x))) s + t). The port computes them on demand (the array has no other reader).
std::vector<NiPoint3> WorldVertices(const NiGeometry& g, const NiGeometryData& d) {
    std::vector<NiPoint3> out(d.vertexCount);         // count: the data's vf30 (+8)
    const auto& M = g.world.rotate.m;                 // 0x000AB270(r, c): chunk c, element r = the port's m[r][c]
    const double s = g.world.scale;
    const NiPoint3& T = g.world.translate;
    auto row = [&](int r, const NiPoint3& v, f32 t) {
        const f32 a = F(double(M[r][0]) * v.x);
        const f32 b = F(double(M[r][1]) * v.y + a);
        return F((double(M[r][2]) * v.z + b) * s + t);
    };
    for (size_t i = 0; i < out.size() && i < d.vertices.size(); ++i) {
        const NiPoint3& v = d.vertices[i];
        out[i] = {row(0, v, T.x), row(1, v, T.y), row(2, v, T.z)};
    }
    return out;
}

// orig: 0x00115EB0 NiTriShapeData vf40 (the triangle's three indices; 3 t in 16 bits).
void TriangleIndices(const NiTriShapeData& d, u32 t, u16& i0, u16& i1, u16& i2) {
    const u16 k = static_cast<u16>(3 * t);
    auto at = [&](u16 i) { return i < d.triangleList.size() ? d.triangleList[i] : u16(0); };
    i0 = at(k), i1 = at(static_cast<u16>(k + 1)), i2 = at(static_cast<u16>(k + 2));
}

// orig: 0x00153DC0 (cdecl: the camera's projection rows +0xC4, the point, the result): x / y in [0, 1] by the w row,
// z by (far - near).
NiPoint3 Project(const f32 (&M)[16], const NiPoint3& p) {
    const double x = p.x, y = p.y, z = p.z;
    const double w = ((z * M[14] + y * M[13]) + x * M[12]) + M[15];
    const double invW = 1.0 / w;                      // FDIVR qword [0x00293CC0] (1.0): kept
    const f32 qx = F(((z * M[2] + y * M[1]) + x * M[0]) + M[3]);
    const f32 qy = F(((z * M[6] + y * M[5]) + x * M[4]) + M[7]);
    const f32 qz = F(((z * M[10] + y * M[9]) + x * M[8]) + M[11]);
    NiPoint3 q;
    q.x = F((double(qx) * invW + 1.0) * 0.5);         // + qword 1.0, x qword [0x0029BE38] 0.5
    q.y = F((double(qy) * invW + 1.0) * 0.5);
    q.z = F((double(qz) - double(g_near) * 0.0f) / (double(g_far) - g_near));   // near x [0x00293AB8] 0
    return q;
}

// orig: 0x00153DC0's caller's records (0x38 bytes each on the stack, index 50 = the chain's first edge): the vertex
// +0, the edge to the next record +4, the projected point +8, the direction to the next record +0x14 (z +0x1C = 0),
// the quad's two corners +0x20 / +0x2C.
struct ChainRecord {
    s32 vertex;
    s32 edge;
    f32 p[3];
    f32 dir[3];
    f32 a[3];
    f32 b[3];
};
static_assert(sizeof(ChainRecord) == 0x38);

// orig: 0x00153EA0 (cdecl, the record): the unit direction (x, y) to the next record's point; (1, 0) for none.
void Direction(ChainRecord& r, const ChainRecord& next) {
    const double dx = double(next.p[0]) - r.p[0];
    const double dy = double(next.p[1]) - r.p[1];
    const f32 dyF = F(dy);                            // FST [esp+8]: the product uses the unrounded dy once
    const double len = std::sqrt(dy * dyF + dx * dx);
    r.dir[2] = 0.0f;                                  // +0x1C
    if (len == 0.0) {                                 // FCOM [0]; TEST AH,0x44; JP (NaN: the division)
        r.dir[0] = 1.0f, r.dir[1] = 0.0f;
        return;
    }
    const f32 inv = F(1.0 / len);
    r.dir[0] = F(dx * inv);
    r.dir[1] = F(double(inv) * dyF);
}

// The quad corners of a chain's end record: the side offset (-kx d.y, ky d.x) of the direction d.
void EndCorners(ChainRecord& r, const f32 (&d)[3], f32 kx, f32 ky) {
    const double ny = double(ky) * d[0];
    const double nx = -(double(kx) * d[1]);           // FCHS
    r.a[0] = F(nx + r.p[0]), r.a[1] = F(ny + r.p[1]), r.a[2] = r.p[2];
    r.b[0] = F(double(r.p[0]) - nx), r.b[1] = F(double(r.p[1]) - ny), r.b[2] = r.p[2];
}

}  // namespace

// ---- the outline data ------------------------------------------------------------------------------------------
s32 NiOutline::FindOrAddEdge(s32 a, s32 b) {          // orig: 0x0013C8A0 (thiscall: the outline)
    NiOutlineData& d = *data;
    for (size_t i = 0; i < d.edges.size(); ++i) {
        const NiOutlineData::Edge& e = d.edges[i];
        if (s16(e.a) != b || s16(e.b) != a || e.d != 0xFFFF) continue;
        if (e.c == 0xFFFF) {                          // (an edge of the triangle being added: faces[-1], L: UB)
            NotPorted("NiOutline::FindOrAddEdge: a degenerate triangle reads the face before the array (UB)");
            continue;
        }
        const NiOutlineData::Face& f = d.faces[s16(e.c)];
        if (s16(f.v[0]) != g_thirdVertex && s16(f.v[1]) != g_thirdVertex && s16(f.v[2]) != g_thirdVertex)
            return static_cast<s32>(i);
    }
    NiOutlineData::Edge e;                            // (0x0013C050: faces -1, flags 0)
    e.a = static_cast<u16>(a), e.b = static_cast<u16>(b), e.c = e.d = 0xFFFF;
    d.edges.push_back(e);
    return static_cast<s32>(d.edges.size() - 1);
}

void NiOutline::BuildData() {                         // orig: 0x0013DAA0
    CollectGeometry();                                // 0x0013D8D0
    data = new NiOutlineData;                         // new(0x2C) 0x0013C070 (+0x28 = 1), the old one released
    NiOutlineData& d = *data;
    const size_t n = geometry.size();                 // +0xF8 sampled once
    std::vector<std::vector<NiPoint3>> worldVerts(n);
    s32 vertexTotal = 0, faceTotal = 0;               // +0x24 / +0x20
    for (size_t g = 0; g < n; ++g) {
        NiGeometry* geo = geometry[g].Get();
        auto* md = dynamic_cast<NiTriBasedGeomData*>(geo->modelData.Get());
        if (!md) {
            NotPorted("NiOutline::BuildData: a listed geometry without tri-based data");
            continue;
        }
        vertexTotal += md->vertexCount;               // (data +8)
        faceTotal += md->triangleCount;               // (data +0x34)
        geo->UpdateDownwardPass(0.0f, true);          // NiAVObject::Update(0, 1) 0x001018D0
        worldVerts[g] = WorldVertices(*geo, *md);          // vf38 / vf3C
        // (then the lip-sync meshes DAT_002D7B60 / count DAT_002D7B68 with the same vertex counts: no code writes
        // the list, L: always empty; 0x0013C930 clears it at the end)
    }
    std::vector<NiPoint3> points;                     // (new(12 x vertices): the world vertices in list order)
    points.reserve(vertexTotal);
    for (const auto& w : worldVerts) points.insert(points.end(), w.begin(), w.end());
    points.resize(vertexTotal);
    d.faces.assign(faceTotal, {});                    // (new(16 x faces): +0xE never written, L: 0)
    d.edges.clear();
    d.edges.reserve(3 * static_cast<size_t>(faceTotal));   // (new(48 x faces) of 0x0013C050 edges; count +0x1C = 0)
    // the weld: the vertices sorted by y (0x001679F0 with 0x0013C810); a later vertex within 1e-3 of an earlier one
    // takes its index; the scan of a vertex stops at the first one more than 0.001 above it in y
    std::vector<s32> remap(vertexTotal);
    std::vector<WeldRecord> sorted(vertexTotal);
    for (s32 i = 0; i < vertexTotal; ++i) {
        sorted[i] = {points[i].x, points[i].y, points[i].z, i};
        remap[i] = i;
    }
    crt::Qsort(sorted.data(), sorted.size(), sizeof(WeldRecord), CompareY);
    for (s32 i = 0; i + 1 < vertexTotal; ++i) {
        const WeldRecord& r = sorted[i];
        for (s32 j = i + 1; j < vertexTotal; ++j) {
            WeldRecord& q = sorted[j];
            const double dz = double(r.z) - q.z, dy = double(r.y) - q.y, dx = double(r.x) - q.x;
            if ((dx * dx + dy * dy) + dz * dz < double(kWeldDistance2)) {   // FCOMP; TEST AH,5; JP
                remap[q.index] = r.index;
                q.index = r.index;
            }
            if (std::fabs(double(r.y) - q.y) > double(kWeldRangeY)) break;   // FABS; FCOMP; TEST AH,0x41
        }
    }
    // the faces: the welded indices of each triangle, the geometry's index +6
    s32 base = 0, k = 0;
    for (size_t g = 0; g < n; ++g) {
        auto* md = dynamic_cast<NiTriShapeData*>(geometry[g]->modelData.Get());
        if (!md) {
            if (geometry[g]->modelData) NotPorted("NiOutline::BuildData: the triangles of a non-NiTriShapeData");
            continue;
        }
        for (u32 t = 0; t < md->triangleCount; ++t, ++k) {   // vfB4 (the data) -> vf40 0x00115EB0
            u16 i0, i1, i2;
            TriangleIndices(*md, t, i0, i1, i2);
            NiOutlineData::Face& f = d.faces[k];
            f.v[0] = static_cast<u16>(remap[i0 + base]);
            f.v[1] = static_cast<u16>(remap[i1 + base]);
            f.v[2] = static_cast<u16>(remap[i2 + base]);
            f.v[3] = static_cast<u16>(g);
        }
        base += md->vertexCount;
    }
    // the edges (0x0013C8A0 with the third vertex in DAT_002FA4D4) and each edge's two faces
    for (s32 f = 0; f < faceTotal; ++f) {
        const s32 v0 = s16(d.faces[f].v[0]), v1 = s16(d.faces[f].v[1]), v2 = s16(d.faces[f].v[2]);
        g_thirdVertex = v2;
        const s32 e0 = FindOrAddEdge(v0, v1);
        g_thirdVertex = v0;
        const s32 e1 = FindOrAddEdge(v1, v2);
        g_thirdVertex = v1;
        const s32 e2 = FindOrAddEdge(v2, v0);
        NiOutlineData::Face& face = d.faces[f];
        face.v[4] = static_cast<u16>(e0), face.v[5] = static_cast<u16>(e1), face.v[6] = static_cast<u16>(e2);
        for (s32 e : {e0, e1, e2}) {
            NiOutlineData::Edge& x = d.edges[e];
            if (x.c == 0xFFFF) x.c = static_cast<u16>(f);
            else if (x.d == 0xFFFF) x.d = static_cast<u16>(f);
        }
    }
    d.edges.shrink_to_fit();                          // (the array of +0x1C edges copied: 0x0013C050 + memcpy)
    // (sprintf " edges: %d" into a local buffer, unused)
    d.lists.assign(vertexTotal, {});                  // +0x10 / +0x14: each vertex's edges in edge order
    for (size_t e = 0; e < d.edges.size(); ++e) {     // 0x0013C030 twice
        d.lists[s16(d.edges[e].a)].push_back(static_cast<u16>(e));
        d.lists[s16(d.edges[e].b)].push_back(static_cast<u16>(e));
    }
}

void NiOutline::ApplyExtraData() {                    // orig: 0x0013CA40
    NiOutlineData& d = *data;
    if (!d.extraDataPending) return;                  // +0x28
    const size_t n = geometry.size();
    d.colors.assign(n, NiColorA{0, 0, 0, 1});         // new(16 n) 0x00012FA0, then (0, 0, 0, 1) each
    zPriority.assign(n, 0);                           // +0x104 = new(4 n), each 0
    std::vector<s32> mode(n, 1);                      // (a 1000-entry stack array; more geometry overflows it)
    for (size_t i = 0; i < n; ++i)
        for (NiExtraData* e = geometry[i]->extraData.Get(); e; e = e->nextData.Get()) {   // GetExtraData, +0x10
            auto* s = dynamic_cast<NiStringExtraData*>(e);
            if (!s) continue;
            const char* str = s->string.c_str();     // +0x14
            if (std::strncmp(str, "NiData_OutlineInterior", 0x16) == 0) mode[i] = 2;
            else if (std::strncmp(str, "NiData_OutlineNoInterior", 0x18) == 0) mode[i] = 3;
            else if (std::strncmp(str, "NiData_OutlineNone", 0x12) == 0) mode[i] = 0;
            else if (std::strncmp(str, "NiData_Zprio", 0xC) == 0) {
                f32 z = 0;                            // sscanf("%f") into the int slot
                if (std::sscanf(str + 0xC, "%f", &z) == 1) zPriority[i] = std::bit_cast<u32>(z);
            } else if (std::strncmp(str, "NiData_OutlineColor=", 0x14) == 0) {
                const s32 c = std::atoi(str + 0x14);   // RRRGGGBBB
                NiColorA& col = d.colors[i];
                col.a = 1.0f;
                col.r = F(double(c / 1000000) * kInv255);
                col.b = F(double(c % 1000) * kInv255);
                col.g = F(double((c % 1000000) / 1000) * kInv255);
            }
        }
    for (NiOutlineData::Edge& e : d.edges) {          // the interior / never flags by the faces' geometry modes
        if (s16(e.c) == -1) continue;
        const s32 ga = s16(d.faces[s16(e.c)].v[3]);
        if (s16(e.d) == -1) {
            if (mode[ga] == 0) e.flags = 0x20;
            else if (mode[ga] == 2) e.flags = 0x10;
        } else {
            const s32 gb = s16(d.faces[s16(e.d)].v[3]);
            const s32 m = mode[ga] < mode[gb] ? mode[gb] : mode[ga];
            if (m == 0) e.flags = 0x20;
            else if (m == 2 && ga != gb) e.flags = 0x10;
        }
    }
    d.extraDataPending = false;
}

// ---- the display -----------------------------------------------------------------------------------------------
void NiOutline::OnVisible(NiCullPass& pass) {         // orig: 0x0013E8D0
    // (+0xE8 is read without a null test: a NiOutline without data counts as one without edges, a UB guard)
    const bool edges = data && !data->edges.empty();  // (+0xE8)+0x1C
    if (!edges || g_outlineMode == 0 || !(kMinWidth <= width)) {
        NiNode::OnVisible(pass);                      // 0x001054E0
        return;
    }
    CollectGeometry();                                // 0x0013D8D0
    ApplyExtraData();                                 // 0x0013CA40
    // the children that are not listed first (the list is walked alongside: no bounds test in the original)
    size_t k = 0;
    for (size_t i = 0; i < children_.size(); ++i) {   // (+0xCC re-read)
        NiAVObject* c = children_[i].Get();
        if (!c) continue;
        if (k < geometry.size() && c == geometry[k].Get()) ++k;
        else pass.Cull(c);                            // 0x00101AC0
    }
    const size_t count = data->lists.size();          // +0x24
    if (g_scratch.size() < count) {                   // (signed compare with DAT_0030F724)
        g_scratch.assign(count, NiPoint3{});          // new(12 n) (0x00011740 / 0x000114B0: not initialised, L: 0)
        g_vertexMark.assign(count, 0);                // new(n)
    }
    camera = const_cast<NiCamera*>(&pass.camera);     // +0xE0 (refcounted there)
    if (g_drawn.size() < geometry.size()) g_drawn.assign(geometry.size(), 0);   // DAT_002FA4F8 / DAT_002FA4FC
    NiPoint3* scratch = g_scratch.data();
    k = 0;
    for (size_t i = 0; i < children_.size(); ++i) {
        NiAVObject* c = children_[i].Get();
        if (!c || k >= geometry.size() || c != geometry[k].Get()) continue;
        NiGeometry* g = geometry[k].Get();
        g_outlineGeometry = g;                        // DAT_002FA500
        g_outlineScratch = scratch;                   // DAT_0030EBE0: the display writes the world vertices
        if (g->skinInstance) NotPorted("NiOutline::OnVisible: a skinned geometry (skin +0x14 = 1, 0x0014F7A0)");
        pass.Cull(c);
        g_outlineGeometry = nullptr;
        g_drawn[k] = g_outlineScratch == nullptr;     // (the display clears it once written)
        g_outlineScratch = nullptr;
        if (g->modelData) scratch += g->modelData->vertexCount;
        ++k;
    }
    DrawSilhouette(*camera);                          // 0x00153F20
}

void WriteOutlineWorldVertices(const NiGeometry& g, NiPoint3* out) {   // orig: 0x001167E5..0x00116BB5
    const NiGeometryData* d = g.modelData.Get();
    if (!d || !out) return;
    NiMatrix3 Ms;                                     // 0x000AD900: the rotation times the scale (SSE: each word once)
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) Ms.m[r][c] = F(double(g.world.rotate.m[r][c]) * g.world.scale);
    const auto& M = Ms.m;
    const NiPoint3& T = g.world.translate;
    const s32 n = d->vertexCount;                     // (data +8)
    // The unrolled loop (4 vertices a step while 4 remain) groups the sums differently per slot; the rest loop again.
    const s32 unrolled = n / 4 * 4;
    for (s32 i = 0; i < n && i < static_cast<s32>(d->vertices.size()); ++i) {
        const NiPoint3& v = d->vertices[i];
        const double xx0 = double(M[0][0]) * v.x, xy0 = double(M[0][1]) * v.y, xz0 = double(M[0][2]) * v.z;
        const double yx = double(M[1][0]) * v.x, yy = double(M[1][1]) * v.y, yz = double(M[1][2]) * v.z;
        const double zx = double(M[2][0]) * v.x, zy = double(M[2][1]) * v.y, zz = double(M[2][2]) * v.z;
        double sx, sy, sz;
        if (i < unrolled && i % 4 == 1) {             // 0x00116900
            sx = (xz0 + xy0) + xx0, sy = (yz + yx) + yy, sz = (zz + zx) + zy;
        } else if (i < unrolled) {                    // 0x00116868 / 0x00116997 / 0x00116A35
            sx = (xy0 + xz0) + xx0, sy = (yx + yy) + yz, sz = (zx + zy) + zz;
        } else {                                      // 0x00116B10
            sx = (xy0 + xx0) + xz0, sy = (yx + yy) + yz, sz = (zx + zy) + zz;
        }
        out[i] = {F(double(T.x) + sx), F(double(T.y) + sy), F(double(T.z) + F(sz))};   // z: FSTP first
    }
}

// ---- the silhouette ----------------------------------------------------------------------------------------------
void NiOutline::DrawSilhouette(NiCamera& cam) {       // orig: 0x00153F20
    if (g_outlineMode == 0) return;
    if (g_classify.empty()) {                         // new(0x100): a silhouette (front + back face: 5) unless 0x20;
        g_classify.resize(0x100);                     //  one front / back face (4 / 1) or two front faces (8) with
        for (u32 i = 0; i < 0x100; ++i) {             //  0x10
            u32 v = i;
            switch (i & 0xF) {
            case 4:
            case 8:
            case 1:
                if (i & 0x10) v |= 0x80;
                break;
            case 5:
                if (!(i & 0x20)) v |= 0x80;
                break;
            default: break;
            }
            g_classify[i] = static_cast<u8>(v);
        }
    }
    NiOutlineData& d = *data;
    const s32 vertexCount = static_cast<s32>(d.lists.size());   // +0x24
    const s32 faceCount = static_cast<s32>(d.faces.size());     // +0x20
    const s32 edgeCount = static_cast<s32>(d.edges.size());     // +0x1C
    if (vertexCount == 0) return;
    g_near = cam.frustum[4];                          // cam+0x144
    g_far = cam.frustum[5];                           // cam+0x148
    NiPoint3* P = g_scratch.data();
    for (auto& e : d.edges) e.flags &= 0x70;
    // the facing test: (E - v0) . ((v1 - v0) x (v2 - v0)) by the x87 order; >= 0 (or NaN) adds 4 to the triangle's
    // edges, < 0 adds 1 (the loop is unrolled by 4, the same arithmetic)
    const NiPoint3 E = cam.world.translate;           // cam+0xA0
    for (s32 f = 0; f < faceCount; ++f) {
        const NiOutlineData::Face& face = d.faces[f];
        const NiPoint3 v0 = P[s16(face.v[0])], v1 = P[s16(face.v[1])], v2 = P[s16(face.v[2])];
        const double a1 = double(v1.x) - v0.x, a2 = double(v1.y) - v0.y, a3 = double(v1.z) - v0.z;
        const f32 b1 = F(double(v2.x) - v0.x), b2 = F(double(v2.y) - v0.y);
        const double b3 = double(v2.z) - v0.z;
        const f32 n0 = F(b3 * a2 - double(b2) * a3);
        const f32 n1 = F(a3 * b1 - b3 * a1);
        const double n2 = a1 * b2 - a2 * b1;
        const double c1 = double(E.x) - v0.x, c2 = double(E.y) - v0.y, c3 = double(E.z) - v0.z;
        const double s = (c3 * n2 + c2 * n1) + c1 * n0;
        const u16 add = s < 0.0 ? 1 : 4;              // FCOMP [0]; TEST AH,5; JP
        for (int j = 4; j < 7; ++j) d.edges[s16(face.v[j])].flags = static_cast<u16>(d.edges[s16(face.v[j])].flags + add);
    }
    for (auto& e : d.edges) e.flags = g_classify[static_cast<u8>(s16(e.flags))];   // (movsx index, movzx byte)
    const f32 kx = F((double(kLineWidth) / (double(cam.port[1]) - cam.port[0])) * kAspect);   // cam+0x150 - +0x14C
    const f32 ky = F(double(kLineWidth) / (double(cam.port[2]) - cam.port[3]));              // cam+0x154 - +0x158
    f32 M[16];                                        // the 16 words of cam+0xC4
    std::memcpy(M, cam.projection, sizeof M);
    for (s32 v = 0; v < vertexCount; ++v) g_vertexMark[v] = 0;
    for (const auto& e : d.edges)
        if ((e.flags & 0x80) == 0x80) {
            g_vertexMark[s16(e.a)] = 1;
            g_vertexMark[s16(e.b)] = 1;
            ++g_silhouetteEdges;
        }
    // the geometry not drawn (DAT_002FA4FC 0): its vertices unmarked and their edges cleared
    s32 start = 0;
    for (size_t g = 0; g < geometry.size(); ++g) {    // +0xF8
        const s32 n = geometry[g]->modelData ? geometry[g]->modelData->vertexCount : 0;
        if (!g_drawn[g])
            for (s32 v = start; v < start + n; ++v) {
                g_vertexMark[v] = 0;
                for (u16 e : d.lists[v]) d.edges[e].flags &= 0x70;
            }
        start += n;
    }
    // the marked vertices projected (0x00153DC0) in place; one outside [-0.1, 1.1] clears its edges instead
    for (s32 v = 0; v < vertexCount; ++v) {
        if (!g_vertexMark[v]) continue;
        const NiPoint3 q = Project(M, P[v]);
        if (q.x < kClipLow || q.x > kClipHigh || q.y < kClipLow || q.y > kClipHigh) {   // (NaN passes)
            for (u16 e : d.lists[v]) d.edges[e].flags &= 0x70;
        } else {
            P[v] = q;
        }
    }
    // the chains: from each silhouette edge left (in edge order) forward from its second vertex (records 51..99)
    // and backward from its first (50..0), each next edge the first of the vertex's list that turns by less than
    // 90 degrees; each chain one strip of quads into the outline polygons
    static ChainRecord rec[100];                      // (the original's stack frame: not initialised, L)
    for (s32 scan = 0; scan < edgeCount; ++scan) {
        s32 back = 0x32, fwd = 0x33;
        while (scan < edgeCount && (d.edges[scan].flags & 0x80) != 0x80) ++scan;
        if (scan == edgeCount) return;
        NiOutlineData::Edge& e0 = d.edges[scan];
        const s32 va = s16(e0.a), vb = s16(e0.b);
        rec[50].vertex = va;
        std::memcpy(rec[50].p, &P[va], 12);
        rec[51].vertex = vb;
        std::memcpy(rec[51].p, &P[vb], 12);
        e0.flags &= 0x70;
        rec[50].edge = scan;
        Direction(rec[50], rec[51]);
        for (s32 at = 51;;) {                         // forward (0x00154B30)
            ChainRecord& r = rec[at];
            const std::vector<u16>& list = d.lists[r.vertex];
            size_t j = 0;
            for (; j < list.size(); ++j) {
                NiOutlineData::Edge& c = d.edges[list[j]];
                if ((c.flags & 0x80) != 0x80) continue;
                s32 other = s16(c.a);
                if (other == r.vertex) other = s16(c.b);
                rec[at + 1].vertex = other;
                std::memcpy(rec[at + 1].p, &P[other], 12);
                r.edge = list[j];
                Direction(r, rec[at + 1]);
                const double dot = double(rec[at - 1].dir[0]) * r.dir[0] + double(rec[at - 1].dir[1]) * r.dir[1];
                if (dot > 0.0) {                      // FCOMP [0]; TEST AH,0x41; JE
                    c.flags &= 0x70;
                    ++fwd, ++at;
                    break;
                }
            }
            if (j == list.size() || fwd >= 0x63) break;
        }
        for (s32 at = 50;;) {                         // backward (0x00154C53)
            ChainRecord& r = rec[at];
            const std::vector<u16>& list = d.lists[r.vertex];
            size_t j = 0;
            for (; j < list.size(); ++j) {
                NiOutlineData::Edge& c = d.edges[list[j]];
                if ((c.flags & 0x80) != 0x80) continue;
                s32 other = s16(c.a);
                if (other == r.vertex) other = s16(c.b);
                ChainRecord& p = rec[at - 1];
                p.vertex = other;
                std::memcpy(p.p, &P[other], 12);
                p.edge = list[j];
                Direction(p, r);
                const double dot = double(p.dir[0]) * r.dir[0] + double(p.dir[1]) * r.dir[1];
                if (dot > 0.0) {
                    c.flags &= 0x70;
                    --back, --at;
                    break;
                }
            }
            if (j == list.size() || !(at > 0)) break;  // (TEST EDX,EDX; JG: the record offset)
        }
        // the quads: the ends offset across their edge, the inner records by the miter of their two edges
        EndCorners(rec[back], rec[back].dir, kx, ky);
        EndCorners(rec[fwd], rec[fwd - 1].dir, kx, ky);
        for (s32 i = back + 1; i < fwd; ++i) {        // (unrolled by 4: the same arithmetic)
            const f32 pdx = rec[i - 1].dir[0], pdy = rec[i - 1].dir[1];
            const f32 cdx = rec[i].dir[0], cdy = rec[i].dir[1];
            const double det = double(cdy) * pdx - double(cdx) * pdy;
            double X, Y;
            if (det == 0.0) {                         // FCOM [0]; TEST AH,0x44; JNP (NaN: the division)
                const double t = -(double(pdy) * kx);
                Y = double(ky) * t;
                X = t * kAspect;
            } else {
                const f32 inv = F(1.0 / det);
                X = ((double(cdx) - pdx) * inv) * kx * kAspect;
                Y = ((double(cdy) - pdy) * inv) * ky;
            }
            ChainRecord& r = rec[i];
            r.a[0] = F(X + r.p[0]), r.a[1] = F(Y + r.p[1]), r.a[2] = r.p[2];
            r.b[0] = F(double(r.p[0]) - X), r.b[1] = F(double(r.p[1]) - Y), r.b[2] = r.p[2];
        }
        NiPoint3* V = nullptr;
        NiPoint2* T = nullptr;
        AddPolygonRun(2 * (fwd - back) + 2, 0, 0, 0, &V, &T, nullptr);   // 0x0013D440
        if (!V || !T) continue;
        for (s32 i = back, k = 0; i <= fwd; ++i, k += 2) {
            const ChainRecord& r = rec[i];
            V[k] = {r.a[0], r.a[1], F(double(r.a[2]) - kDepthBias)};
            V[k + 1] = {r.b[0], r.b[1], F(double(r.b[2]) - kDepthBias)};
            T[k] = {0.5f, 0.0f};
            T[k + 1] = {0.5f, 1.0f};
        }
    }
}

// ---- the outline polygons ---------------------------------------------------------------------------------------
void NiOutline::AddPolygonRun(s32 count, u32 r, u32 g, u32 b, NiPoint3** vertices, NiPoint2** uvs,
                              NiColorA** colors) {    // orig: 0x0013D440
    if (g_polygon) {
        const s32 cur = g_polygonCount, end = cur + count;
        if (end <= kPolygonVertices) {
            auto& c = g_polygon->colors;
            for (s32 k = 0; k < count - 2; ++k) c[cur + k] = {Bits(r), Bits(g), Bits(b), 1.0f};
            c[end - 2] = {0, 0, 0, 0};
            c[end - 1] = {0, 0, 0, 0};
            g_polygonCount = end;
            *vertices = &g_polygon->vertices[cur];
            *uvs = &g_polygon->uvs[cur];
            if (colors) *colors = &g_polygon->colors[cur];
            return;
        }
        if (g_outlineMode == 2) {                     // full: the rest from the previous run's last two cleared,
            s32 from = cur - 2;                       //  the polygon attached to the outline's camera
            if (from < 0) from = 0;
            for (s32 k = from; k < kPolygonVertices; ++k) {
                g_polygon->colors[k] = {0, 0, 0, 0};
                g_polygon->vertices[k] = {0, 0, 0};
            }
            g_polygon->flags |= 7;
            if (camera) camera->AttachScreenPolygon(g_polygon);   // 0x00019460 (ecx = +0xE0)
        }
        g_polygon = nullptr;
    }
    if (g_pool.empty()) g_pool.resize(kPoolSize);
    if (g_poolIndex >= kPoolSize) {                   // (the pool exhausted: the first polygon's arrays, L)
        NiScreenPolygon* p = g_pool[0].Get();
        *vertices = p ? p->vertices.data() : nullptr;
        *uvs = p ? p->uvs.data() : nullptr;
        if (colors) *colors = p ? p->colors.data() : nullptr;
        return;
    }
    if (!g_pool[g_poolIndex]) {
        // the arrays of the creating frame's stack: the colours (0, 0, 0, 0.6); the vertices and texture coordinates
        // are not initialised (L: 0; every vertex of the runs is written and the rest cleared at the attach, the
        // texture coordinates past the last run keep these)
        std::vector<NiColorA> c(kPolygonVertices, NiColorA{0, 0, 0, kPolygonAlpha});
        std::vector<NiPoint3> v(kPolygonVertices, NiPoint3{0, 0, 0});
        std::vector<NiPoint2> t(kPolygonVertices, NiPoint2{0, 0});
        OutlineProperties& props = Properties();      // (DAT_00304568 null: 0x0013CEF0(0) first; the statics exist)
        NiPointer<NiScreenPolygon> p(new NiScreenPolygon(kPolygonVertices, v.data(), t.data(), c.data()));   // 0x00106DE0
        p->strip = 1;                                 // 0x00106AB0(1)
        p->drawOnce = 1;                              // 0x00106AD0(1)
        p->AttachProperty(props.vertexColor.Get());   // (each in its type's slot of +8)
        p->AttachProperty(props.zBuffer.Get());
        p->AttachProperty(props.texturing.Get());
        p->AttachProperty(props.alpha.Get());
        p->AttachProperty(props.shade.Get());
        g_pool[g_poolIndex] = p;
    }
    g_polygon = g_pool[g_poolIndex++].Get();
    g_polygonCount = 0;
    AddPolygonRun(count, r, g, b, vertices, uvs, colors);
}

void SetOutlineTexture(NiObject* texture) {           // orig: 0x0013CEF0 (cdecl)
    OutlineProperties& p = Properties();
    if (texture) {
        p.texturing->SetBaseTexture(texture);         // 0x0001D610
        p.texturing->applyMode = 2;                   // +0x18
        p.texturing->maps[0].filterMode = 1;          // 0x0001D6B0(1)
    }
    p.alpha->flags |= 1;                              // blending, SRCALPHA / INVSRCALPHA
    p.alpha->flags = static_cast<u16>((p.alpha->flags & 0xFEFF) | 0xE0);
    p.alpha->flags = static_cast<u16>((p.alpha->flags & 0xFFED) | 0xC);
    p.zBuffer->flags |= 1;                            // test on, write off
    p.zBuffer->flags &= ~2;
    p.vertexColor->sourceVertexMode = 1;              // +0x18
    p.vertexColor->lightingMode = 0;                  // +0x1C
    p.shade->flags &= ~1;                             // flat
}

void BeginOutlineClick() {                            // orig: 0x0013C7E0 (Click's start)
    g_silhouetteEdges = 0;                            // DAT_002FA4DC
    g_poolIndex = 0;                                  // DAT_002FA4E0 (also DAT_002FA4D0 / DAT_002FA4D8, statistics)
    g_polygon = nullptr;                              // DAT_002FA4E8 (and DAT_002FA4F0: never set, L)
}

void FinishOutlineClick(NiCamera& camera) {           // orig: 0x0013E3F0 (cdecl, after the cull)
    if (g_outlineMode != 2) return;
    if (g_polygon) {                                  // the rest from the last run's last two cleared, attached
        const s32 from = g_polygonCount - 2;          // (no clamp here)
        for (s32 k = from; k < kPolygonVertices; ++k) {
            if (k < 0) continue;                      // (a polygon has a run of at least 4: unreachable, a UB guard)
            g_polygon->colors[k] = {0, 0, 0, 0};
            g_polygon->vertices[k] = {0, 0, 0};
        }
        g_polygon->flags |= 7;
        camera.AttachScreenPolygon(g_polygon);        // 0x00019460
        g_polygon = nullptr;
    }
    // (then the outline geometry DAT_002FA4F0: no code sets it, L)
}

void ShutdownOutlines() {                             // orig: 0x0013D020
    g_pool.clear();                                   // (and the 0x2FA520..0x304520 array: not ported, L)
    g_poolIndex = 0;
    g_polygon = nullptr;
    g_polygonCount = 0;
    g_scratch.clear();
    g_vertexMark.clear();
    g_drawn.clear();
    Properties().texturing->SetBaseTexture(nullptr);
    g_classify.clear();
}

}  // namespace fut::ni

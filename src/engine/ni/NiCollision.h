// The collision world's ray cast (default.xbe 0x000FB780; docs/analysis/collision.md) and the collision mesh records
// (docs/analysis/collision_meshes_port.md): every linked NiBsp (global list, newest first) is tested.
//
// Ported: pass 1 (the stage triangles through each NiBsp's kd-tree), with the original's persistent ray state (the
// per-axis inverses are only refreshed when large enough, so they carry over between casts); pass 2 (the collision
// mesh records the per-frame refresh RefreshAllBsps keeps in the kd-trees: CollectSegment, the sphere prefilter, the
// CRT qsort, RayCastMesh); the records themselves (file groups, AddCollisionMeshes / RemoveCollisionMeshes, the
// types); the mesh part of the sphere query (QueryMeshes and its scratch triangles; the stage part and the sort are
// in game/physics/Physics.cpp).
#pragma once

#include "engine/ni/NiObjects.h"

#include <array>
#include <functional>

namespace fut::ni {

extern NiBsp* g_bspList;                              // orig: DAT_002F8ED4

// A collision triangle in the original's 0x14-byte layout with vertex pointers (orig: v0 +0, v1 +4, v2 +8, invCross
// +0xC, flags +0x10): the mesh scratch triangles and the pass-2 hit record.
struct HitTri {
    const NiPoint3* v[3] = {};
    u32 invCross = 0;                                 // float bits (1 / |cross|, 0 when degenerate)
    u32 flags = 0;
};

struct RayHit {                                       // the hit "triangle" (orig: a 0x14-byte record pointer)
    const NiBsp* bsp = nullptr;                       // pass 1: the NiBsp and its triangle
    const NiBsp::Triangle* triangle = nullptr;
    const HitTri* mesh = nullptr;                     // pass 2: &g_hit (orig: 0x002F8F18)
    explicit operator bool() const { return triangle != nullptr || mesh != nullptr; }
    u32 FlagWord() const { return triangle ? triangle->b : mesh ? mesh->flags : 0; }   // the +0x10 word
    u8 Surface() const { return static_cast<u8>(FlagWord() >> 8); }   // orig: HitSurface 0x000FA330
    u8 Flags() const { return static_cast<u8>(FlagWord()); }          // orig: 0x000FA320 (byte 0)
    const NiPoint3& Vertex(int k) const;              // [hit + 4k]
    NiPoint3 Normal() const;                          // orig: 0x000FA8C0
};

// orig: 0x000FB780. `dir` is not normalised (t in units of dir); `dist` in = the maximum t, out = the hit's t
// (unchanged without a hit). cullBack: only triangles facing the ray. flags: 1 = stage triangles, 2 = collision
// meshes, 4 = skip camera-transparent meshes.
RayHit RayCast(const NiPoint3& start, const NiPoint3& dir, f32& dist, bool cullBack, u32 flags);

// orig: 0x000FA8C0: the triangle normal (cross of the edges from v0) scaled by its stored 1/|cross|, per component F.
NiPoint3 TriangleNormal(const NiBsp& b, const NiBsp::Triangle& t);
NiPoint3 TriangleNormal(const NiPoint3& v0, const NiPoint3& v1, const NiPoint3& v2, u32 invCross);
// orig: 0x000FA810: the triangle's 1 / |cross| as stored at +0xC (0 below 1e-6).
u32 InvCrossBits(const NiPoint3& v0, const NiPoint3& v1, const NiPoint3& v2);

// orig: SetTriMask 0x000F9D80 (returns the old mask): triangles whose flags share a bit are skipped.
u8 SetTriangleMask(u8 mask);

// ---- the collision mesh records (collision_meshes_port.md 1 / 2) ----
// orig: 0x000CEAD0: the first NiStringExtraData starting with `prefix` (_strnicmp) on the object, else on its parents.
NiStringExtraData* TagOnSelfOrAncestor(NiAVObject* obj, const char* prefix);
void RecomputeMeshTypes(NiBsp& b);                    // orig: 0x000FB6B0
// orig: 0x000FC110: every NiTriShape under `obj` becomes a record (all its triangles, `flags` each).
void AddCollisionMeshes(NiBsp& b, NiAVObject* obj, u32 flags);
// orig: 0x000FC2D0: the first record of each NiTriShape under `obj` is removed (the order of the others kept).
void RemoveCollisionMeshes(NiBsp& b, NiAVObject* obj);
void RefreshMeshes(NiBsp& b);                         // orig: 0x000FBB10
void RefreshAllBsps();                                // orig: 0x000FCF10 (the first statement of Session::Render)

// ---- the sphere query's mesh part (collision_meshes_port.md 3) ----
// The query scratch (orig: DAT_002F8ECC triangles, DAT_002F8ED0 vertices, count DAT_002F4698, capacity 2000). The
// original allocates both blocks at the first QueryMeshes that gets past the sphere and exclusion tests and keeps them
// for the session; `origAddr` is the triangle block's address there (the query's sort key, 7.5).
struct MeshScratch {
    static constexpr u32 kCapacity = 2000;            // orig: DAT_002F8EC8
    std::array<HitTri, kCapacity> tris{};
    std::array<NiPoint3, 3 * kCapacity> verts{};
    u32 count = 0;
    bool allocated = false;
    u32 origAddr = 0;
    s32 defaultRank = -1;                             // (no origAddr: how many NiBsp arrays lie below the block)
};
extern MeshScratch g_scratch;
// Resets the scratch for a new session (orig: ~Session 0x000C83F0 frees it) with its sort key: `origAddr` (the play
// oracle's COLLSCRATCH), or 0 for the default: with `defaultRank` >= 0 the block is placed at its allocation (the
// first query that fills it) above that many of the linked NiBsp arrays (the order the original's heap gives the
// level, Session.cpp's table; KNOWN_ISSUES #36), else above every NiBsp's array.
void ResetMeshScratch(u32 origAddr, s32 defaultRank = -1);
extern NiBsp::MeshRecord* g_cand[0x400];             // orig: 0x002F46A0 (the candidate records)
extern u32 g_candCount;                               // orig: 0x002F8EE4
extern bool g_wantTris, g_wantRecs;                   // orig: 0x002D7720 (initially 1) / 0x002F8EE8
extern NiAVObject* g_exclude;                         // orig: 0x002F8EEC (the solver's shape +0xC8)
// orig: 0x000FA3C0 (and the record part of the box walk 0x000FADA0 / 0x000FA3F0): the node's records, head first,
// while fewer than 0x400 candidates.
void AppendRecords(const NiBsp::TreeNode& n);
// orig: vfA4 0x000FC3A0: the candidates' triangles that reach the query cube, into the scratch.
void QueryMeshes(NiBsp& b, const NiPoint3& c, f32 r, u32 flags);

extern NiTriShape* g_lastMeshHit;                     // orig: 0x002F8F14 (0 at every RayCast entry)

}  // namespace fut::ni

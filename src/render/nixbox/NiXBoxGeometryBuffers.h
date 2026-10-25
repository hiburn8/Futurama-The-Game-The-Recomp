// The NiXBoxRenderer's geometry buffers (docs/analysis/renderer_plan.md 8; class names (L) after Gamebryo's):
//   * the geometry-group manager (0x0015C270, the global DAT_0030EC70; NiXBoxRenderer +0x5AC) and its groups: type 0
//     "static" (0x00165A40, vtable 0x002AC1D8: the stages' groups, vertex buffers shared per vertex format), type 1
//     "unshared" (0x00165BE0, vtable 0x002AC208: the renderer's own +0x5B0, one buffer per geometry); type 2
//     "dynamic" (0x00166740, vtable 0x002AC24C) is NOT PORTED (no caller in the code ported);
//   * a static group's buffers: per vertex format a set (0x3C bytes, 0x00165870 / 0x001658F0) of 64 KB blocks (0x4C,
//     0x00165660; free list sorted by free size), each cut into chips (0x20, 0x00145DA0) from its end;
//   * the per-geometry renderer data NiGeometryBufferData (0x40 bytes, 0x00146110; NiGeometryData +0x30);
//   * the vertex buffer manager's layout (0x001578C0) and packing (0x00157C90) with the software skinning
//     (NiSkinInstance deform 0x0014F7A0), the index buffer manager's CreateIndexBuffer (0x0015CBC0).
// Members carry the original's offsets.
#pragma once

#include "engine/ni/NiObjects.h"
#include "engine/ni/NiRenderer.h"
#include "render/xd3d/XD3DDevice.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fut::render::nixbox {

class NiXBoxGeometryGroup;
struct NiVBBlock;
struct NiVBSet;

// orig: a vertex-buffer chip (0x20 bytes: 0x00145DA0 in a block; 0x00147D10 a buffer of its own).
struct NiVBChip {
    u32 index = 0;                                    // +0x00 its slot in the block's chip array
    NiVBBlock* block = nullptr;                       // +0x04 (null: a buffer of its own)
    xd3d::Resource* vb = nullptr;                     // +0x08
    u32 offset = 0;                                   // +0x0C (bytes)
    u32 lockFlags = 0;                                // +0x10
    u32 size = 0;                                     // +0x14
};

// orig: a vertex-buffer block (0x4C bytes from a pool 0x00165550; 0x00165660). Its heap address keys the renderer's
// pending-pack map (+0x508, hash = address % 37): the order the index buffers are created in at the next BeginScene.
struct NiVBBlock {
    NiVBSet* set = nullptr;                           // +0x00
    u32 index = 0;                                    // +0x04 its slot in the set's block array
    xd3d::Resource* vb = nullptr;                     // +0x08
    u32 size = 0;                                     // +0x0C
    u32 used = 0;                                     // +0x20 (the end of the last chip)
    u32 free = 0;                                     // +0x24 (size - used)
    u32 unused = 0;                                   // +0x28 (size minus the live chips' sizes)
    std::vector<NiVBChip*> chips;                     // +0x2C (NiTPrimitiveArray: null slots are reused)
    NiVBBlock* next = nullptr;                        // +0x44 / +0x48: the set's free list
    NiVBBlock* prev = nullptr;
    u32 address = 0;                                  // (the original's heap address: injected or made up)
};

// orig: a static group's vertex-buffer set of one vertex format (0x3C bytes, 0x00165870; 0x001658F0(0x10000, device,
// fvf, 8, 0)).
struct NiVBSet {
    u32 fvf = 0;                                      // +0x04
    u32 flags = 8;                                    // +0x08 (0x10000000: no minimum block size)
    u32 blockSize = 0x10000;                          // +0x0C
    std::vector<NiVBBlock*> blocks;                   // +0x1C (NiTPrimitiveArray)
    NiVBBlock* freeList = nullptr;                    // +0x34 (ascending free size)
    u32 threshold = 0x80;                             // +0x38 DAT_002D868C: a block with more free bytes stays listed
};

// orig: NiGeometryBufferData (0x40 bytes, ctor 0x00146110): the renderer's data of a geometry data (+0x30).
struct NiGeometryBufferData : ni::NiGeometryRendererData {
    ~NiGeometryBufferData() override;                 // orig: renderer vfE8 0x00146F30 -> manager vf24 0x0015C760
    u32 flags = 0;                                    // +0x04: bit 0 static, bit 23 normals, bits 24-31 texture sets
    NiXBoxGeometryGroup* group = nullptr;             // +0x08
    u32 fvf = 0;                                      // +0x0C
    u32 stride = 0;                                   // +0x10
    u32 vertexCount = 0;                              // +0x14
    NiVBChip* chip = nullptr;                         // +0x18
    u32 indexCount = 0;                               // +0x1C
    u32 ibSize = 0;                                   // +0x20
    xd3d::Resource* ib = nullptr;                     // +0x24
    u32 baseVertex = 0;                               // +0x28 (SetIndices' base vertex / DrawVertices' start)
    u32 primitive = xd3d::PT_TRIANGLELIST;            // +0x2C (5 list, 6 strip)
    u32 primitiveCount = 0;                           // +0x30
    u32 stripCount = 0;                               // +0x34
    // +0x38 / +0x3C: the strip lengths and the index array (pointers into the data's arrays in the original; the port
    // reads the data's current arrays through `data`, the same memory as long as the data does not replace them).
    bool hasStripLengths = false;
    bool hasIndices = false;
    ni::NiGeometryData* data = nullptr;               // (the data whose +0x30 this is)
    const u16* Indices() const;
    const u16* StripLengths() const;
};

// The vertex layout of 0x001578C0 (no skin partition).
struct VertexLayout {
    u32 fvf = 0, stride = 0, normalOffset = 0, colorOffset = 0, uvOffset = 0, sets = 0;
};
VertexLayout Layout(const ni::NiGeometryData& data);

// orig: a geometry group (vtables 0x002AC1D8 / 0x002AC208; base ctor 0x001667C0, +0xC the device).
class NiXBoxGeometryGroup : public ni::NiGeometryGroup {
public:
    NiXBoxGeometryGroup(xd3d::XD3DDevice& device, u32 type) : d3d(device), type(type) {}
    ~NiXBoxGeometryGroup() override;
    xd3d::XD3DDevice& d3d;
    const u32 type;
    std::set<NiGeometryBufferData*> members;          // +0x04 counts them (0x001667E0 / 0x001667F0)
    // vf0C 0x00165C50 (both types): the data's renderer data, primitive and index array (skin partitions: NOT PORTED)
    void CreateData(ni::NiGeometryData& data);
    // vf04 0x00165E80 (the unshared group's; the manager's vf10 0x0015C4B0): a screen texture's renderer data (+0x1C):
    // a triangle list of two triangles per rect, one strip, no index array of the data's
    void CreateScreenTextureData(ni::NiScreenTexture& texture);
    // vf1C 0x00166560: detaches the data's renderer data, frees its chip (vf24), releases its index buffer, deletes it
    void ReleaseData(NiGeometryBufferData& rd);
    virtual NiVBChip* AllocChip(NiGeometryBufferData& rd) = 0;   // vf20
    virtual void FreeChip(NiGeometryBufferData& rd) = 0;         // vf24
    virtual void ReleaseAll() {}                                  // vf00
    // The heap address of a new block (the renderer's: unique over all groups).
    std::function<u32()> blockAddress;
};

class NiXBoxStaticGroup : public NiXBoxGeometryGroup {   // orig: 0x00165A40 (vtable 0x002AC1D8)
public:
    explicit NiXBoxStaticGroup(xd3d::XD3DDevice& device) : NiXBoxGeometryGroup(device, 0) {}
    ~NiXBoxStaticGroup() override;
    NiVBChip* AllocChip(NiGeometryBufferData& rd) override;     // orig: 0x00165AC0
    void FreeChip(NiGeometryBufferData& rd) override;           // orig: 0x001657F0
    void ReleaseAll() override;                                 // orig: 0x00165760
private:
    std::map<u32, std::unique_ptr<NiVBSet>> sets_;    // +0x10 (NiTMap by vertex format, 37 buckets)
    NiVBChip* SetAlloc(NiVBSet& set, u32 size);       // orig: 0x00165700
    NiVBBlock* NewBlock(NiVBSet& set, u32 size);      // orig: 0x00165660
    void SetFree(NiVBSet& set, u32 blockIndex, u32 chipIndex);   // orig: 0x001654F0
    void ReleaseBlock(NiVBBlock* block);              // orig: 0x001652B0
    std::vector<std::unique_ptr<NiVBBlock>> blocks_;  // (owner of the blocks)
};

class NiXBoxUnsharedGroup : public NiXBoxGeometryGroup {   // orig: 0x00165BE0 (vtable 0x002AC208)
public:
    explicit NiXBoxUnsharedGroup(xd3d::XD3DDevice& device) : NiXBoxGeometryGroup(device, 1) {}
    ~NiXBoxUnsharedGroup() override;
    NiVBChip* AllocChip(NiGeometryBufferData& rd) override;     // orig: 0x00165F60
    void FreeChip(NiGeometryBufferData& rd) override;           // orig: 0x00165F90
};

// orig: the vertex buffer manager's (+0x5B4) packing of a geometry's vertices into its chip: 0x00157C90 (data, skin,
// isNew, chip, dest); dest null: the chip is (re)allocated if needed and locked here. Returns the chip written.
NiVBChip* PackVertices(xd3d::XD3DDevice& d3d, u32 frame, ni::NiGeometryData& data, ni::NiSkinInstance* skin,
                       bool isNew, NiVBChip* chip, u8* dest);
// orig: the vertex buffer manager's 0x00157F30 (renderer data, count, positions (x, y), colours (r, g, b, a floats),
// texture coordinates, chip, dest 0): pre-transformed vertices (XYZRHW: x, y, 0, 1; DIFFUSE; TEX1) into `chip` when it
// is big enough (D3DVertexBuffer_GetDesc), else a new chip of the group (vf20); locked here. Returns the chip.
NiVBChip* PackScreenVertices(xd3d::XD3DDevice& d3d, NiGeometryBufferData& rd, u16 count, const f32* positions,
                             const f32* colors, const f32* uvs, NiVBChip* chip);
// orig: NiXBoxIndexBufferManager::CreateIndexBuffer 0x0015CBC0 (indices, count, old buffer, &size, 1, 0): the old
// buffer when it is big enough, else a new one; the indices copied in.
xd3d::Resource* CreateIndexBuffer(xd3d::XD3DDevice& d3d, const u16* indices, u32 count, xd3d::Resource* old,
                                  u32& size);
// orig: 0x0014F7A0 (thiscall on the skin instance): the software deform of the data's positions (and normals when
// both exist) into the vertex buffer: sum over the bones of weight * (skin-to-world of the bone)(v).
void SkinDeform(const ni::NiSkinInstance& skin, const ni::NiPoint3* vertices, const ni::NiPoint3* normals, u32 count,
                u8* pos, u8* nrm, u32 stride);

}  // namespace fut::render::nixbox

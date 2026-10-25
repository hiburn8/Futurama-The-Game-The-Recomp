// The NiXBoxRenderer's geometry buffers; see NiXBoxGeometryBuffers.h. x87 arithmetic in the original's order (double =
// the extended register, F() = a float store).
#include "render/nixbox/NiXBoxGeometryBuffers.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fut::render::nixbox {

using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

constexpr u32 kMaxTextureSets = 4;                    // DAT_002D8670

// NiTPrimitiveArray::AddFirstEmpty 0x000C5360: the first null slot, else appended.
template <typename T>
u32 AddFirstEmpty(std::vector<T*>& a, T* p) {
    for (u32 i = 0; i < a.size(); ++i)
        if (!a[i]) {
            a[i] = p;
            return i;
        }
    a.push_back(p);
    return u32(a.size() - 1);
}

// NiTPrimitiveArray::RemoveAt 0x001651F0: the slot cleared; the size shrinks only when it was the last one.
template <typename T>
void RemoveAt(std::vector<T*>& a, u32 i) {
    if (i >= a.size()) return;
    a[i] = nullptr;
    if (i == a.size() - 1) a.pop_back();
}

// 0x001579B0: `count` strided copies of `bytes` bytes (a null source: the zero bytes of address 0 under the oracle's
// emulator, whose page 0 is mapped; L on a console).
void StridedCopy(u8* dst, const void* src, u32 stride, u32 bytes, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        if (src) std::memcpy(dst + size_t(i) * stride, static_cast<const u8*>(src) + size_t(i) * bytes, bytes);
        else std::memset(dst + size_t(i) * stride, 0, bytes);
    }
}

// orig: the fast square-root table (DAT_0030F038, built by the static initialiser 0x00230FF0 -> 0x00101860): 256
// mantissas, [i] = sqrt(1 + i / 128) and [128 + i] = sqrt(2 + i / 64) (FSQRT, FSTP float, the low 23 bits).
const u32* FastSqrtTable() {
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 0x80; ++i) {
            const f32 a = std::bit_cast<f32>((i | 0x3F80u) << 16), b = std::bit_cast<f32>((i | 0x4000u) << 16);
            t[i] = std::bit_cast<u32>(F(std::sqrt(double(a)))) & 0x7FFFFFu;
            t[i + 0x80] = std::bit_cast<u32>(F(std::sqrt(double(b)))) & 0x7FFFFFu;
        }
        return t;
    }();
    return table.data();
}

// orig: 0x00152070 (cdecl(normals, count, stride)): each normal times 1 / the table square root of its squared length
// F((x*x + y*y) + z*z) (the exponent halved arithmetically, the table half by its parity; a zero length gives 0).
void FastNormalize(u8* nrm, u32 count, u32 stride) {
    const u32* table = FastSqrtTable();
    for (u32 i = 0; i < count; ++i) {
        f32* n = reinterpret_cast<f32*>(nrm + size_t(i) * stride);
        const double x = n[0], y = n[1], z = n[2];
        const u32 bits = std::bit_cast<u32>(F((x * x + y * y) + z * z));
        double inv = 0.0;                             // [0x00293AB8] = 0.0f
        if (bits != 0) {
            s16 e = s16(s32(bits >> 23) - 0x7F);      // (cx)
            u32 m = bits & 0x7FFFFFu;
            if (e & 1) m |= 0x800000u;
            e = s16(e >> 1);                          // SAR cx, 1
            const f32 root = std::bit_cast<f32>(table[m >> 16] | (u32(s32(e) + 0x7F) << 23));
            inv = 1.0 / double(root);                 // FLD 1.0 [0x00293A98]; FDIV
        }
        n[0] = F(inv * n[0]);
        n[1] = F(inv * n[1]);
        n[2] = F(inv * n[2]);
    }
}

u32 ColorByte(f32 v) {                                // FSTP(v * 255.0f) [0x002A1564], FISTP (round to nearest)
    return static_cast<u32>(static_cast<s32>(std::nearbyint(F(double(v) * 255.0f))));
}

}  // namespace

// ---- the renderer data ------------------------------------------------------------------------------------------
NiGeometryBufferData::~NiGeometryBufferData() {      // orig: 0x00146F30 -> 0x0015C760 (no skin partition)
    if (!group) return;                               // (the group is gone: the renderer was destroyed first)
    NiXBoxGeometryGroup* g = group;
    g->ReleaseData(*this);                            // group vf1C 0x00166560
    if (g->members.empty()) g->ReleaseAll();          // group vf00 when its last data left
}

const u16* NiGeometryBufferData::Indices() const {
    if (!hasIndices || !data) return nullptr;
    if (auto* t = dynamic_cast<const ni::NiTriShapeData*>(data))
        return t->triangleList.empty() ? nullptr : t->triangleList.data();
    if (auto* s = dynamic_cast<const ni::NiTriStripsData*>(data))
        return s->stripLists.empty() ? nullptr : s->stripLists.data();
    return nullptr;
}

const u16* NiGeometryBufferData::StripLengths() const {
    if (!hasStripLengths || !data) return nullptr;
    auto* s = dynamic_cast<const ni::NiTriStripsData*>(data);
    return s && !s->stripLengths.empty() ? s->stripLengths.data() : nullptr;
}

VertexLayout Layout(const ni::NiGeometryData& d) {   // orig: 0x001578C0 (no skin partition: XYZ)
    VertexLayout l;
    l.fvf = 0x002;
    l.stride = 12;
    if (!d.normals.empty()) {                         // data +0x20
        l.fvf |= 0x010;
        l.normalOffset = l.stride;
        l.stride += 12;
    }
    if (!d.colors.empty()) {                          // data +0x24
        l.fvf |= 0x040;
        l.colorOffset = l.stride;
        l.stride += 4;
    }
    u32 sets = d.textureSets;                         // data +0x0A: none counts as one
    sets = sets == 0 ? 1 : std::min(sets, kMaxTextureSets);
    l.fvf |= sets << 8;
    l.uvOffset = l.stride;
    l.stride += sets * 8;
    l.sets = sets;
    return l;
}

// ---- groups -----------------------------------------------------------------------------------------------------
NiXBoxGeometryGroup::~NiXBoxGeometryGroup() {
    // (UB-avoiding: data outliving its group keeps no pointers into it; in the original the group outlives them)
    for (NiGeometryBufferData* rd : members) rd->group = nullptr, rd->chip = nullptr;
}

void NiXBoxGeometryGroup::CreateData(ni::NiGeometryData& data) {   // orig: 0x00165C50 (vf0C, no skin partition)
    if (data.rendererData) return;
    auto rd = std::make_unique<NiGeometryBufferData>();   // 0x00146110: zeros, primitive 5
    rd->data = &data;
    if (auto* t = dynamic_cast<ni::NiTriShapeData*>(&data)) {   // IsKindOf 0x002F9954
        rd->primitive = xd3d::PT_TRIANGLELIST;
        rd->primitiveCount = t->triangleCount;        // vf3C & 0xFFFF
        rd->stripCount = 1;
        rd->hasIndices = true;                        // +0x3C = data +0x3C
    } else if (auto* s = dynamic_cast<ni::NiTriStripsData*>(&data)) {   // 0x002F9BFC
        rd->primitive = xd3d::PT_TRIANGLESTRIP;
        rd->stripCount = s->stripCount;               // data +0x38
        rd->primitiveCount = u32(u16(s->triangleCount + s->stripCount * 2)) - 2u * s->stripCount;
        rd->hasStripLengths = true;                   // +0x38 = data +0x3C
        rd->hasIndices = true;                        // +0x3C = data +0x40
    } else {
        // NiTrianglesData 0x002F9B3C (list, no indices) / NiTriStripData 0x002F9BA0 (one strip): not in the port
        NotPorted("NiXBoxGeometryGroup::CreateData: data other than NiTriShapeData / NiTriStripsData");
    }
    rd->group = this;                                 // 0x001667E0
    members.insert(rd.get());
    data.rendererData = std::move(rd);
}

void NiXBoxGeometryGroup::CreateScreenTextureData(ni::NiScreenTexture& t) {   // orig: 0x00165E80
    if (t.rendererData) return;
    auto rd = std::make_unique<NiGeometryBufferData>();   // new(0x40): 0x00146110's zeros, primitive 5
    rd->primitive = xd3d::PT_TRIANGLELIST;            // +0x2C
    rd->primitiveCount = u32(t.rects.size()) << 1;    // +0x30 = +0x10 << 1
    rd->stripCount = 1;                               // +0x34; +0x38 = +0x3C = 0 (no strip lengths, no indices)
    rd->group = this;                                 // 0x001667E0
    members.insert(rd.get());
    t.rendererData = std::move(rd);
}

void NiXBoxGeometryGroup::ReleaseData(NiGeometryBufferData& rd) {   // orig: 0x00166560 (the delete: the owner's)
    members.erase(&rd);                               // 0x001667F0
    FreeChip(rd);                                     // vf24
    rd.chip = nullptr;
    if (rd.ib) d3d.Release(rd.ib);
    rd.ib = nullptr;
    rd.group = nullptr;
}

NiVBChip* NiXBoxStaticGroup::AllocChip(NiGeometryBufferData& rd) {   // orig: 0x00165AC0
    std::unique_ptr<NiVBSet>& set = sets_[rd.fvf];
    if (!set) {                                       // 0x001658F0(0x10000, device, fvf, 8, 0)
        set = std::make_unique<NiVBSet>();
        set->fvf = rd.fvf;
    }
    NiVBChip* chip = SetAlloc(*set, rd.vertexCount * rd.stride);
    if (!chip) return nullptr;                        // (the original divides by the null chip's offset)
    rd.baseVertex = rd.stride ? chip->offset / rd.stride : 0;   // +0x28
    return chip;
}

NiVBChip* NiXBoxStaticGroup::SetAlloc(NiVBSet& set, u32 size) {   // orig: 0x00165700
    NiVBBlock* block = nullptr;
    for (NiVBBlock* b = set.freeList; b; b = b->next)  // 0x00165310: the first with room (best fit: ascending)
        if (size <= b->free) {
            block = b;
            break;
        }
    if (block) {
        if (block->next) block->next->prev = block->prev;
        if (block->prev) block->prev->next = block->next;
        if (block == set.freeList) set.freeList = block->next;
    } else {
        block = NewBlock(set, size);
        if (!block) return nullptr;
    }
    NiVBChip* chip = nullptr;                         // 0x00145DA0
    if (block->free >= size) {
        chip = new NiVBChip;
        chip->block = block;
        chip->vb = block->vb;
        chip->offset = block->used;
        chip->size = size;
        block->free -= size;
        block->unused -= size;
        block->used += size;
        chip->index = AddFirstEmpty(block->chips, chip);
    }
    if (block->free > set.threshold) {                // 0x00165360: sorted by free size (after equal ones)
        NiVBBlock* prev = nullptr;
        NiVBBlock* at = set.freeList;
        while (at && at->free <= block->free) prev = at, at = at->next;
        block->prev = prev;
        block->next = at;
        if (prev) prev->next = block;
        else set.freeList = block;
        if (at) at->prev = block;
    }
    return chip;
}

NiVBBlock* NiXBoxStaticGroup::NewBlock(NiVBSet& set, u32 size) {   // orig: 0x00165660
    const u32 length = !(set.flags & 0x10000000) && size <= set.blockSize ? set.blockSize : size;
    xd3d::Resource* vb = d3d.CreateVertexBuffer2(length);
    if (!vb) return nullptr;
    blocks_.push_back(std::make_unique<NiVBBlock>());  // (a block of the pool 0x00310034)
    NiVBBlock* b = blocks_.back().get();
    b->set = &set;
    b->vb = vb;
    b->size = b->free = b->unused = length;
    b->address = blockAddress ? blockAddress() : 0;
    b->index = AddFirstEmpty(set.blocks, b);
    return b;
}

void NiXBoxStaticGroup::FreeChip(NiGeometryBufferData& rd) {   // orig: 0x001657F0
    NiVBChip* chip = rd.chip;
    if (!chip || !chip->block) return;
    SetFree(*chip->block->set, chip->block->index, chip->index);
}

void NiXBoxStaticGroup::SetFree(NiVBSet& set, u32 blockIndex, u32 chipIndex) {   // orig: 0x001654F0
    NiVBBlock* b = blockIndex < set.blocks.size() ? set.blocks[blockIndex] : nullptr;
    if (!b) return;
    const u32 oldFree = b->free;
    // 0x00145C60: the chip's bytes come back; the end moves back only when it was the last chip, to the end of the
    // chips in the slots before it
    NiVBChip* chip = chipIndex < b->chips.size() ? b->chips[chipIndex] : nullptr;
    if (chip) {
        if (b->used - chip->size == chip->offset) {
            b->used = 0;
            for (u32 k = 0; k < chipIndex; ++k)
                if (const NiVBChip* c = b->chips[k]; c && c->offset + c->size > b->used) b->used = c->offset + c->size;
        }
        b->unused += chip->size;
        b->free = b->size - b->used;
        b->chips[chipIndex] = nullptr;                // (SetAt(index, 0); the chip back to its pool)
        delete chip;
    }
    if (oldFree == b->free) return;
    if (b->next) b->next->prev = b->prev;             // 0x001651A0 (only when listed: its neighbours / the head)
    if (b->prev) b->prev->next = b->next;
    if (b == set.freeList) set.freeList = b->next;
    b->next = b->prev = nullptr;
    if (b->unused == b->size) {                       // every chip gone
        RemoveAt(set.blocks, blockIndex);
        ReleaseBlock(b);
        return;
    }
    NiVBBlock* prev = nullptr;                        // 0x00165360
    NiVBBlock* at = set.freeList;
    while (at && at->free <= b->free) prev = at, at = at->next;
    b->prev = prev;
    b->next = at;
    if (prev) prev->next = b;
    else set.freeList = b;
    if (at) at->prev = b;
}

void NiXBoxStaticGroup::ReleaseBlock(NiVBBlock* b) {   // orig: 0x001652B0 (back to the pool)
    if (b->vb) d3d.Release(b->vb);
    for (NiVBChip* c : b->chips) delete c;
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it)
        if (it->get() == b) {
            blocks_.erase(it);
            break;
        }
}

void NiXBoxStaticGroup::ReleaseAll() {                // orig: 0x00165760 (each set: its blocks' buffers released)
    // (L: the release order of the sets; it changes no handle numbers)
    for (auto& [fvf, set] : sets_)
        for (NiVBBlock* b : set->blocks)
            if (b) ReleaseBlock(b);
    sets_.clear();
    for (NiGeometryBufferData* rd : members) rd->chip = nullptr;   // (UB-avoiding: the original keeps dangling chips)
}

NiXBoxStaticGroup::~NiXBoxStaticGroup() {
    for (auto& b : blocks_)
        for (NiVBChip* c : b->chips) delete c;
}

NiVBChip* NiXBoxUnsharedGroup::AllocChip(NiGeometryBufferData& rd) {   // orig: 0x00165F60
    const u32 length = rd.vertexCount * rd.stride;
    xd3d::Resource* vb = d3d.CreateVertexBuffer2(length);
    auto* chip = new NiVBChip;                        // 0x00147D10(0, vb, 0, length, 0)
    chip->vb = vb;
    chip->size = length;
    return chip;
}

void NiXBoxUnsharedGroup::FreeChip(NiGeometryBufferData& rd) {   // orig: 0x00165F90
    if (!rd.chip) return;
    if (rd.chip->vb) d3d.Release(rd.chip->vb);
    delete rd.chip;
}

NiXBoxUnsharedGroup::~NiXBoxUnsharedGroup() {
    for (NiGeometryBufferData* rd : members) delete rd->chip;
}

// ---- packing ----------------------------------------------------------------------------------------------------
NiVBChip* PackVertices(xd3d::XD3DDevice& d3d, u32 frame, ni::NiGeometryData& data, ni::NiSkinInstance* skin,
                       bool isNew, NiVBChip* chip, u8* dest) {   // orig: 0x00157C90
    const u32 count = data.vertexCount;               // vf30
    if (!count) return nullptr;
    auto* rd = static_cast<NiGeometryBufferData*>(data.rendererData.get());
    if (!rd) return nullptr;
    const u32 sets = std::min<u32>(data.textureSets, kMaxTextureSets);
    const VertexLayout l = Layout(data);
    rd->stride = l.stride;                            // +0x10
    rd->fvf = l.fvf;                                  // +0x0C
    if (!dest) {
        bool fits = false;
        if (chip) {
            d3d.GetDesc(chip->vb);                    // (Format D3DFMT_VERTEXDATA 100: always)
            fits = chip->size >= count * l.stride;
        }
        if (!fits) {
            chip = rd->group ? rd->group->AllocChip(*rd) : nullptr;   // vf20
            isNew = true;
        }
        if (!chip || !chip->vb) return chip;          // (the original locks the null chip)
        dest = d3d.Lock2(chip->vb, chip->lockFlags) + chip->offset;
    }
    const ni::NiPoint3* verts = data.vertices.empty() ? nullptr : data.vertices.data();
    const ni::NiPoint3* normals = data.normals.empty() ? nullptr : data.normals.data();
    if (!skin) {
        StridedCopy(dest, verts, l.stride, 12, count);
        if (normals) StridedCopy(dest + l.normalOffset, normals, l.stride, 12, count);
    } else if (skin->deformFrame != frame) {
        skin->deformFrame = frame;
        SkinDeform(*skin, verts, normals, count, dest, normals ? dest + l.normalOffset : nullptr, l.stride);
        if (!isNew) return chip;
    }
    if (!data.colors.empty())
        for (u32 i = 0; i < count && i < data.colors.size(); ++i) {
            const ni::NiColorA& c = data.colors[i];
            const u32 a = ColorByte(c.a), r = ColorByte(c.r), g = ColorByte(c.g), b = ColorByte(c.b);
            const u32 argb = ((((a << 8) | r) << 8 | g) << 8) | b;
            std::memcpy(dest + size_t(i) * l.stride + l.colorOffset, &argb, 4);
        }
    for (u32 s = 0; s < sets; ++s) {                  // 0x00143840: the set's coordinates (null past the data's)
        const bool have = data.uvs.size() >= size_t(s + 1) * count;
        StridedCopy(dest + l.uvOffset + 8 * s, have ? &data.uvs[size_t(s) * count] : nullptr, l.stride, 8, count);
    }
    return chip;
}

NiVBChip* PackScreenVertices(xd3d::XD3DDevice& d3d, NiGeometryBufferData& rd, u16 count, const f32* positions,
                             const f32* colors, const f32* uvs, NiVBChip* chip) {   // orig: 0x00157F30
    if (count == 0 || !positions) return nullptr;
    u32 fvf = 0x004;                                  // D3DFVF_XYZRHW
    u32 stride = 16, colorOffset = 0, uvOffset = 0;
    if (colors) {
        colorOffset = 16;
        fvf = 0x044;                                  // | DIFFUSE
        stride = 20;
    }
    if (uvs) {
        fvf |= 0x100;                                 // TEX1
        uvOffset = stride;
        stride += 8;
    }
    rd.stride = stride;                               // +0x10
    rd.fvf = fvf;                                     // +0x0C
    bool fits = false;
    if (chip) {
        d3d.GetDesc(chip->vb);                        // (Format D3DFMT_VERTEXDATA 100: always)
        fits = chip->size >= u32(count) * stride;
    }
    if (!fits) chip = rd.group ? rd.group->AllocChip(rd) : nullptr;   // vf20
    if (!chip || !chip->vb) return chip;              // (the original locks the null chip)
    u8* dest = d3d.Lock2(chip->vb, chip->lockFlags) + chip->offset;
    if (size_t(count) * stride > chip->vb->data.size() - chip->offset) return chip;   // (UB-avoiding: not seen)
    for (u16 i = 0; i < count; ++i) {
        f32* v = reinterpret_cast<f32*>(dest + size_t(i) * stride);
        v[0] = positions[2 * i], v[1] = positions[2 * i + 1];
        v[2] = 0.0f, v[3] = 1.0f;
    }
    if (colors)
        for (u16 i = 0; i < count; ++i) {
            const f32* c = colors + 4 * i;
            const u32 argb = ((((ColorByte(c[3]) << 8) | ColorByte(c[0])) << 8 | ColorByte(c[1])) << 8) |
                             ColorByte(c[2]);
            std::memcpy(dest + size_t(i) * stride + colorOffset, &argb, 4);
        }
    if (uvs)
        for (u16 i = 0; i < count; ++i) std::memcpy(dest + size_t(i) * stride + uvOffset, uvs + 2 * i, 8);
    return chip;
}

xd3d::Resource* CreateIndexBuffer(xd3d::XD3DDevice& d3d, const u16* indices, u32 count, xd3d::Resource* old,
                                  u32& size) {        // orig: 0x0015CBC0
    if (!indices) return nullptr;
    const u32 bytes = count * 2;
    xd3d::Resource* ib = nullptr;
    if (old) {                                        // 0x0023A5C0 GetDesc: format 0x65, type 7
        NotPorted("CreateIndexBuffer: the old index buffer's D3DIndexBuffer_GetDesc (not recorded)");
        if (bytes <= size) ib = old;
    }
    if (!ib) {
        ib = d3d.CreateIndexBuffer2(bytes);
        if (!ib) return nullptr;
        size = bytes;
    }
    if (bytes) std::memcpy(ib->data.data(), indices, std::min<size_t>(bytes, ib->data.size()));
    return ib;
}

// ---- skinning ---------------------------------------------------------------------------------------------------
void SkinDeform(const ni::NiSkinInstance& skin, const ni::NiPoint3* vertices, const ni::NiPoint3* normals, u32 count,
                u8* pos, u8* nrm, u32 stride) {      // orig: 0x0014F7A0
    const ni::NiSkinData* sd = skin.skinData.Get();
    if (!sd || !skin.rootParent) return;
    // world -> skin: the skin data's root-parent-to-skin after the inverse of the root parent's world transform
    const ni::NiTransform toSkin = ni::Compose(sd->rootParentToSkin, ni::Invert(skin.rootParent->world));
    const bool withNormals = normals && nrm;
    auto at = [stride](u8* base, u32 v) { return reinterpret_cast<f32*>(base + size_t(v) * stride); };
    for (u32 v = 0; v < count; ++v) {                 // the destination cleared (stride 12: memset; else ZERO)
        std::memset(at(pos, v), 0, 12);
        if (withNormals) std::memset(at(nrm, v), 0, 12);
    }
    for (size_t b = 0; b < sd->bones.size(); ++b) {
        const ni::NiSkinData::Bone& bone = sd->bones[b];
        const ni::NiAVObject* node = b < skin.bones.size() ? skin.bones[b] : nullptr;
        if (!node) continue;                          // (the original reads the null bone's +0x70)
        const ni::NiTransform t = ni::Compose(ni::Compose(toSkin, node->world), bone.skinToBone);
        const auto& M = t.rotate.m;
        f32 Ms[3][3];                                 // each 16-byte column times the scale (SSE mulps)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) Ms[r][c] = M[r][c] * t.scale;
        const u32 n = std::min<u32>(bone.vertexCount, u32(bone.weights.size()));
        for (u32 k = 0; k < n; ++k) {
            const u32 vi = bone.weights[k].vertex;
            const double w = bone.weights[k].weight;
            if (vi >= count) continue;                // (the original writes past the buffer)
            const ni::NiPoint3 P = vertices ? vertices[vi] : ni::NiPoint3{};
            f32* d = at(pos, vi);
            if (withNormals) {
                const double px = P.x, py = P.y, pz = P.z;
                const double x = ((Ms[0][0] * px + Ms[0][1] * py) + Ms[0][2] * pz) + t.translate.x;
                const double y = ((Ms[1][0] * px + Ms[1][1] * py) + Ms[1][2] * pz) + t.translate.y;
                const f32 z = F(((Ms[2][0] * px + Ms[2][1] * py) + Ms[2][2] * pz) + t.translate.z);
                const ni::NiPoint3 N = normals[vi];
                const double qx = N.x, qy = N.y, qz = N.z;
                const f32 nx = F((M[0][1] * qy + M[0][2] * qz) + M[0][0] * qx);
                const f32 ny = F((M[1][1] * qy + M[1][2] * qz) + M[1][0] * qx);
                const f32 nz = F((M[2][1] * qy + M[2][2] * qz) + M[2][0] * qx);
                const f32 xw = F(x * w);
                d[0] = F(double(xw) + d[0]);
                d[1] = F(y * w + d[1]);
                d[2] = F(double(z) * w + d[2]);
                f32* e = at(nrm, vi);
                const f32 nyw = F(double(ny) * w), nzw = F(double(nz) * w);
                e[0] = F(double(nx) * w + e[0]);
                e[1] = F(double(nyw) + e[1]);
                e[2] = F(double(nzw) + e[2]);
            } else {
                const double px = P.x, py = P.y, pz = P.z;
                const double x = ((Ms[0][2] * pz + Ms[0][0] * px) + Ms[0][1] * py) + t.translate.x;
                const double y = ((Ms[1][2] * pz + Ms[1][0] * px) + Ms[1][1] * py) + t.translate.y;
                const f32 z = F(((Ms[2][2] * pz + Ms[2][0] * px) + Ms[2][1] * py) + t.translate.z);
                const f32 xw = F(x * w), zw = F(double(z) * w);
                d[0] = F(double(xw) + d[0]);
                d[1] = F(y * w + d[1]);
                d[2] = F(double(zw) + d[2]);
            }
        }
    }
    if (withNormals) FastNormalize(nrm, count, stride);   // 0x001501D0: 0x00152070(nrm, count, stride)
}

}  // namespace fut::render::nixbox

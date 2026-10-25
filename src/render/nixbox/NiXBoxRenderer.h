// NiXBoxRenderer (NetImmerse 4.2's Xbox renderer: vtable 0x002AA860, 0x6D0 bytes, factory 0x0014C200;
// docs/analysis/renderer_d3d_map.md 1-4, renderer_plan.md) on the port's D3D8 (src/render/xd3d): the frame
// structure (ClearBuffer, SetCameraData, BeginScene / EndScene, DisplayFrame), the game's NiAlphaAccumulator and
// the static-mesh draw path (0x0014C580 -> 0x00149B70). Members carry the original's offsets.
//
// Ported so far (stage 1): the slots above, the world transform 0x00148A30, the draw loop of 0x00149B70 with its
// stream / index / vertex-shader calls and the buffer update 0x00146FB0, the geometry groups / vertex / index buffer
// managers with the pre-cache (vf9C 0x0014B1D0 -> 0x001480B0) and BeginScene's packing 0x00148390, the software
// skinning (0x00157C90 -> 0x0014F7A0; the hardware path 0x0014BCC0 needs +0x504 >= 4, which stays 0: never taken),
// the accumulator (renderer +8: the game's NiAlphaAccumulator with its registration, the unsorted list,
// NiClusterAccumulator's sorter 0x0010ED00 / 0x00150C80 with the CRT qsort; or another the game installs), render
// targets (vf3C 0x00146490), rendered textures (vf108 0x00147410), screen textures (vf55 0x0014C600 with
// 0x0014C080 / 0x0014A0E0 / 0x00157F30) and particles (vf52 0x00148B70 with the dynamic vertex buffers 0x001584D0 /
// 0x00157A10 and the shared quad index buffer 0x0015CC80), the render-state cache NiXBoxRenderState (+0x5C0), the
// light manager (+0x5C4) and the texture pipeline (+0x5C8) with the pass loop (round 7, renderer_plan.md 13). NOT
// PORTED (reported once each): the texture manager (+0x5BC: a minimal stand-in), the pixel shader 0x001477D0, skin
// partitions, the dynamic group (type 2), the other geometry classes' display (lines, billboards, outlines). Screen
// polygons (vf54 0x0014B680 with 0x00149FC0 / 0x00158630): ported, their property state not.
#pragma once

#include "engine/ni/NiRenderer.h"
#include "render/nixbox/NiXBoxGeometryBuffers.h"
#include "render/nixbox/NiXBoxLightManager.h"
#include "render/nixbox/NiXBoxRenderState.h"
#include "render/nixbox/NiXBoxTexturePipeline.h"
#include "render/xd3d/XD3DDevice.h"

#include <functional>
#include <map>
#include <memory>

#include <vector>

namespace fut::ni {
class NiAVObject;
class NiAlphaProperty;
class NiScreenPolygon;
class NiScreenTexture;
class NiTriShapeData;
}  // namespace fut::ni

namespace fut::render::nixbox {

class NiXBoxRenderer : public ni::NiRenderer {
public:
    // orig: the factory 0x0014C200 (App::Init: 640x480): the device (InitializeD3dState), the frame buffers'
    // descriptions (0x0014AB40), the clear viewport.
    explicit NiXBoxRenderer(xd3d::XD3DDevice& device);
    ~NiXBoxRenderer() override;

    void DisplayFrame() override;                     // orig: 0x001464F0
    // orig: 0x001473E0 (slot 65): the texture manager's source texture data unless the texture has renderer data.
    bool PrecacheTexture(ni::NiSourceTexture& texture) override;
    // orig: NiTexture's dtor 0x00118490: the texture's renderer data (+0x20) deleted, its D3D texture released.
    void PurgeTexture(const ni::NiTexture& texture) override;
    xd3d::XD3DDevice* D3DDevice() override { return &d3d_; }   // (+0x60)
    void SetSourceTextureD3D(ni::NiSourceTexture& texture, xd3d::Resource* d3d) override;
    void ClearBuffer(const f32* rect, u32 flags) override;   // orig: 0x00146520
    void SetBackgroundColor(const ni::NiColor& c) override;  // orig: 0x0014B150
    u32 TargetWidth() const override { return current_->width; }     // orig: 0x00146710 ((+0x58)+0)
    u32 TargetHeight() const override { return current_->height; }   // orig: 0x00146720 ((+0x58)+4)
    void BeginScene() override;                       // orig: 0x001489E0
    void EndScene() override;                         // orig: 0x00146A50
    void SetCameraData(const ni::NiPoint3& loc, const ni::NiPoint3& dir, const ni::NiPoint3& up,
                       const ni::NiPoint3& right, const f32 frustum[6], const f32 port[4]) override;   // 0x00146A70
    void DisplayTriShape(ni::NiGeometry& geometry, const ni::NiCamera& camera) override;   // 0x00116760
    void DisplayParticles(ni::NiGeometry& particles, const ni::NiCamera& camera) override;   // 0x0010F8B0
    void DrawScreenTexture(ni::NiScreenTexture& texture) override;   // orig: 0x0014C600 (slot 55)
    void DrawScreenPolygon(ni::NiScreenPolygon& polygon) override;   // orig: 0x0014B680 (slot 54)
    // Click's use of renderer +8: vf2C (false: +8 released), vf30.
    bool StartAccumulating(const ni::NiCamera& camera) override;
    void FinishAccumulating(ni::NiCamera& camera) override;
    ni::NiAccumulator* GetAccumulator() override { return accumulator_; }
    void SetAccumulator(ni::NiAccumulator* a) override { accumulator_ = a; }   // orig: 0x00153490
    bool SetRenderTarget(ni::NiRenderedTexture* texture) override;           // orig: 0x00146490
    bool CreateRenderedTextureData(ni::NiRenderedTexture& texture) override;  // orig: 0x00147410
    ni::NiGeometryGroup* CreateGeometryGroup(u32 type) override;    // orig: 0x0015C5C0
    void DestroyGeometryGroup(ni::NiGeometryGroup* group) override;   // orig: group vf00, 0x0015C3D0
    void AddToGeometryGroup(ni::NiGeometryGroup* group, ni::NiGeometryData& data) override;   // orig: 0x0015C7B0
    void PrecacheGeometry(ni::NiGeometry& geometry, bool isStatic) override;   // orig: 0x0014B1D0

    xd3d::XD3DDevice& device() { return d3d_; }
    NiXBoxRenderState& renderState() { return *state_; }
    NiXBoxTexturePipeline& pipeline() { return *pipeline_; }
    // The heap address of the original's n-th vertex-buffer block (the render oracle's VBBLOCK lines, in order; 0:
    // unknown, a made-up one in allocation order): it orders BeginScene's packing (renderer_plan.md 8).
    std::function<u32(u32 n)> blockAddress;

private:
    // orig: 0x0014C580 (slot 48): the geometry's renderer data (group manager vf18), then 0x00149B70.
    void DrawTriShape(ni::NiGeometry& geometry);
    // orig: 0x00149B70 (data, skin, world, bound, renderer data, 0); data null: a screen texture's buffers
    void DrawGeometry(ni::NiGeometryData* data, ni::NiSkinInstance* skin, const ni::NiTransform& world,
                      NiGeometryBufferData& rd);
    // orig: 0x00148B70 (slot 52: data, world, bound): the particles' camera-facing quads in a dynamic vertex buffer
    void DrawParticles(ni::NiGeometryData& data, const ni::NiTransform& world, const ni::NiBound& bound);
    // orig: the vertex buffer manager's (+0x5B4) dynamic buffers: 0x00158610 -> 0x001584D0 (length): the current
    // slot's buffer, (re)created when smaller than max(length, the largest length asked so far +0x10)
    xd3d::Resource* DynamicVertexBuffer(u32 length);
    // orig: the index buffer manager's (+0x5B8) quad list 0x0015CC80 (quads, 1, 0, 1): one buffer (+0xC, its size
    // +0x10) of (0, 1, 2), (0, 2, 3) per quad, re-created when too small, refilled every call
    xd3d::Resource* QuadIndexBuffer(u32 quads);
    // orig: 0x00149FC0 (polygon, near +0x34C, range +0x350): the polygon's vertices into a dynamic buffer (the
    // vertex buffer manager's 0x00158630) and the renderer's own buffer data +0x690; false without a buffer
    bool PackScreenPolygon(ni::NiScreenPolygon& polygon);
    xd3d::D3DMATRIX ScreenProjection() const;         // +0x394
    // orig: 0x0014C080: a screen texture's buffers made, or rebuilt when its +0x18 dirty bits are set
    void PackScreenTexture(ni::NiScreenTexture& texture);
    // orig: 0x0014A0E0 (texture, &chip, &index buffer): the rects' vertices (pixel positions, colours, texture
    // coordinates; 0x00157F30) and their two triangles each (0x0015CBC0)
    void BuildScreenTexture(ni::NiScreenTexture& texture, NiVBChip*& chip, xd3d::Resource*& ib);
    // orig: 0x00146FB0 (&renderer data, data, skin, shader, properties): the buffers made or refreshed
    bool UpdateBuffers(NiGeometryBufferData& rd, ni::NiGeometryData& data, ni::NiSkinInstance* skin);
    void SetWorldTransform(const ni::NiTransform& world);   // orig: 0x00148A30
    void RenderImmediate(ni::NiGeometry& geometry, const ni::NiCamera& camera);   // orig: 0x00140310
    void EnsureRenderData(ni::NiGeometry& geometry);
    // orig: 0x0015C640 (group, data, skin, ...) without skin partitions (+0x18 = +0x504 = 0 < 4): the data's renderer
    // data in `group` unless it has one; false when it had one.
    bool AddData(NiXBoxGeometryGroup& group, ni::NiGeometryData& data);
    // orig: 0x001480B0 (data, ..., static, properties 0, shader 0, 0): the vertex chip now, the packing at BeginScene
    void QueuePack(ni::NiGeometryData& data, bool isStatic);
    void PackPending();                               // orig: 0x00148390 (BeginScene)

    // The geometry-group manager (+0x5AC; DAT_0030EC70) and its groups (+4 / +0xC), the renderer's own +0x5B0.
    std::vector<std::unique_ptr<NiXBoxGeometryGroup>> groups_;
    NiXBoxGeometryGroup* unshared_ = nullptr;         // +0x5B0 = manager vf04(1, 0)
    u32 vbFrame_ = 0;                                 // the vertex buffer manager +0x20 (0x00157A00 at DisplayFrame)
    // The vertex buffer manager's ring of dynamic buffers (0x00157A40: [0x002D7EA8] = 10 slots of 0x14 bytes {vb,
    // size, fence, next, prev}, +0x14 the first, +0x1C the current; the slot ctor 0x00157810: L, empty). 0x00157A10
    // (after each dynamic draw) puts a fence into the current slot and moves on (the last slot's next is the first);
    // 0x001584D0 blocks on a pending fence first (D3DDevice_IsFencePending / BlockOnFence: never pending without a
    // GPU, L) and counts its statistics (+0x24..+0x40: no D3D effect, not kept).
    struct DynamicSlot {
        xd3d::Resource* vb = nullptr;                 // +0x00
        u32 size = 0;                                 // +0x04
    };
    DynamicSlot dynamic_[10];
    u32 dynamicCurrent_ = 0;                          // +0x1C (as an index)
    u32 dynamicMinSize_ = 0x400;                      // +0x10 = [0x002D7EAC] (the ctor 0x00157B80), then the largest
    xd3d::Resource* quadIB_ = nullptr;                // the index buffer manager +0x0C
    NiGeometryBufferData polyData_;                   // +0x690 (the screen polygons' buffer data)
    std::unique_ptr<NiVBChip> polyChip_;              // +0x6A8 (0x00147D10: a chip of no block)
    u32 quadIBSize_ = 0;                              // +0x10 (the bytes of the last request, not the buffer's)
    u32 blocks_ = 0;                                  // (vertex-buffer blocks created: blockAddress' n)
    // The pending packs (+0x508: NiTMap of 37 buckets, hash = key % 37, a new key at its bucket's head): key = the
    // chip's block (its address; 0: a buffer of its own), the records newest first (0x2C bytes each).
    struct PendingPack {
        ni::NiPointer<ni::NiGeometryData> data;       // +0x00
        u32 flags = 0;                                // +0x0C (bit 0 static)
        NiVBChip* chip = nullptr;                     // +0x24
    };
    struct PendingKey {
        u32 key = 0;
        NiVBBlock* block = nullptr;
        std::vector<PendingPack> packs;               // (front = the newest: the list head, +0x28 links)
    };
    std::vector<PendingKey> pending_[37];

    xd3d::XD3DDevice& d3d_;

    // The render-target set (+0x14 the back buffer's, +0x58 the current one): +0 width, +4 height, +0x10 (L: a
    // rendered texture's; 0 for the back buffer), +0x14 colour surface, +0x20 depth surface.
    // A rendered texture's (0x24 bytes, its renderer data +0x80): +8 the NiRenderedTexture, +0x10 the D3D texture.
    struct TargetSet {
        u32 width = 0, height = 0;
        u32 format = 0;                               // (+8)+0x24: what ClearBuffer's 0x00158A20 tests (L, below)
        xd3d::Resource* texture = nullptr;            // +0x10 (0 for the back buffer: DisplayFrame swaps)
        xd3d::Resource* color = nullptr;
        xd3d::Resource* depth = nullptr;
    };
    TargetSet backBuffer_;                            // +0x14
    TargetSet* current_ = &backBuffer_;               // +0x58
    // The rendered textures' renderer data (NiXBoxRenderedTextureData L, 0x84 bytes, 0x0015E4C0; the texture's +0x20).
    std::map<const ni::NiRenderedTexture*, std::unique_ptr<TargetSet>> renderedTextures_;
    u32 field64_ = 0;                                 // +0x64 (reset by Clear and DisplayFrame)
    u32 backgroundColor_ = 0;                         // +0x2CC
    f32 depthClear_ = 1.0f;                           // +0x2D0
    u32 stencilClear_ = 0;                            // +0x2D4
    u8 flags2D8_ = 0;                                 // +0x2D8
    ni::NiPoint3 camRight0_, camUp0_, camRight1_, camUp1_;   // +0x2FC / +0x308 / +0x314 / +0x320
    f32 nearPlane_ = 0, depthRange_ = 0;              // +0x34C / +0x350
    xd3d::D3DMATRIX screenView_ = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};   // +0x354 (identity)
    // +0x3BC / +0x3CC: elements (3, 3) / (4, 3) of the screen projection +0x394, written by SetCameraData (copies of
    // the projection's)
    f32 proj33_ = 0, proj43_ = 0;
    xd3d::D3DVIEWPORT8 clearViewport_;                // +0x414
    bool inScene_ = false;                            // +0x500
    xd3d::D3DMATRIX world_{};                         // +0x5D0
    xd3d::D3DMATRIX view_{};                          // +0x610
    xd3d::D3DMATRIX projection_{};                    // +0x650
    // The render-state cache (+0x5C0, 0x00159CB0), the light manager (+0x5C4, 0x0015BD40) and the texture pipeline
    // manager (+0x5C8, 0x0015B690), created in that order by the factory.
    std::unique_ptr<NiXBoxRenderState> state_;
    std::unique_ptr<NiXBoxLightManager> lights_;
    std::unique_ptr<NiXBoxTextureManager> textures_;  // +0x5BC (minimal: NiXBoxTexturePipeline.h)
    std::unique_ptr<NiXBoxTexturePipeline> pipeline_;
    u32 stateFlip1FC_ = 0;                            // NiXBoxRenderState +0x1FC (L: 0)
    // The screen textures' property state +0x5A8 (a new one per draw in the original, 0x0014C600).
    ni::NiPointer<ni::NiPropertyState> screenTextureState_;

    // The game's NiAlphaAccumulator (vtable 0x0029521C), the renderer's own +8.
    struct Accumulator : ni::NiAccumulator {
        explicit Accumulator(NiXBoxRenderer& r) : renderer(r) {}
        bool StartAccumulating(const ni::NiCamera& camera) override;   // orig: 0x0010EB50
        void FinishAccumulating(ni::NiCamera& camera) override;        // orig: 0x0002A590
        bool RegisterObject(ni::NiGeometry& geometry) override;        // orig: 0x0002ADE0
        NiXBoxRenderer& renderer;
        // +0x08 / +0x14 = camera (also the sorter's camera: sorter +0x10 +4)
        bool unsortedFlag = true;                     // +0x28 (ctor 0x0002A4A0: 1)
        std::vector<ni::NiGeometry*> sorted;          // the list +0x0C (0x0002B0D0: appended)
        std::vector<ni::NiGeometry*> unsorted;        // the array +0x2C (0x0002AED0)
    };
    Accumulator acc_{*this};
    ni::NiAccumulator* accumulator_ = &acc_;          // renderer +8

};

}  // namespace fut::render::nixbox

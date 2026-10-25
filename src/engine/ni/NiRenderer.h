// The renderer as the engine and the game call it: the NiRenderer virtual slots NiCamera::Click 0x00100CE0, the
// geometry classes' display (vf84) and App::Frame use (decomp/renderer_vtable.tsv, renderer_d3d_map.md 1-2). The
// implementation is the decompiled NiXBoxRenderer (src/render/nixbox). A camera without one (NiCamera::renderer
// null) runs Click's cull pass alone, as under the play oracle's fake renderer, whose slots are no-ops.
#pragma once

#include "engine/ni/NiMath.h"

namespace fut::render::xd3d {
class XD3DDevice;
struct Resource;
}  // namespace fut::render::xd3d

namespace fut::ni {

class NiCamera;
struct NiColor;
class NiGeometry;
class NiGeometryData;
class NiSkinInstance;

class NiRenderedTexture;
class NiTexture;
class NiScreenTexture;
class NiScreenPolygon;

// A geometry group of the renderer (opaque to the engine and the game).
class NiGeometryGroup {
public:
    virtual ~NiGeometryGroup() = default;
};

// orig: NiAccumulator (base ctor 0x00140F50): what a camera's Click hands the visible geometry to (renderer +8). The
// renderer's own is the game's NiAlphaAccumulator (vtable 0x0029521C); the game also installs others for a pass
// (MasterPropertyAccumulator 0x00038000: the player's shadow).
class NiAccumulator {
public:
    virtual ~NiAccumulator() = default;
    // vf2C: NiAccumulator's 0x00140F70: false when already accumulating (+8 set), else +8 = the camera.
    virtual bool StartAccumulating(const NiCamera& cam) {
        if (camera) return false;
        camera = &cam;
        return true;
    }
    virtual void FinishAccumulating(NiCamera& camera) = 0;           // vf30
    // vf34 (via 0x001401D0): true when the accumulator took the object (drawn later, or drawn by it).
    virtual bool RegisterObject(NiGeometry& geometry) = 0;
    const NiCamera* camera = nullptr;                                // +8
};

class NiPropertyState;
class NiDynamicEffectState;

class NiSourceTexture;

class NiRenderer {
public:
    // orig: NiRenderer's ctor 0x001405E0 adds the renderer to the list DAT_002D7BBC (the destructor removes it).
    NiRenderer();
    virtual ~NiRenderer();
    NiRenderer(const NiRenderer&) = delete;
    NiRenderer& operator=(const NiRenderer&) = delete;
    // Slot 65 (vf104) 0x001473E0: the source texture's renderer data (the D3D texture with its texels) unless it has
    // one (+0x20); false when it cannot be made.
    virtual bool PrecacheTexture(NiSourceTexture& texture) { (void)texture; return true; }
    // orig: NiTexture's dtor 0x00118490 deletes the texture's renderer data (+0x20, vf00: the D3D texture released):
    // the renderer forgets the texture (its data is keyed by the object in the port).
    virtual void PurgeTexture(const NiTexture& texture) { (void)texture; }
    // orig: renderer +0x60, the D3D device (the movie player's 0x000EDED0 takes it; null: none).
    virtual render::xd3d::XD3DDevice* D3DDevice() { return nullptr; }
    // orig: the movie player's 0x000EE59A..0x000EE5D9: the source texture's renderer data (+0x20) takes `d3d` as its
    // D3D texture (+0x3C: the old one released, `d3d` AddRef'd).
    virtual void SetSourceTextureD3D(NiSourceTexture& texture, render::xd3d::Resource* d3d) {
        (void)texture;
        (void)d3d;
    }
    // +0xC / +0x10: the property / effect state of the object being displayed (the display sets them from the
    // geometry's +0xC0 / +0xC4; 0x00106B80 from a screen polygon's own state; read by the accumulators and the draw).
    NiPropertyState* propertyState = nullptr;
    NiDynamicEffectState* effectState = nullptr;
    virtual void DisplayFrame() = 0;                                 // slot 16 (vf40) 0x001464F0
    virtual void ClearBuffer(const f32* rect, u32 flags) = 0;        // slot 17 (vf44) 0x00146520
    virtual void SetBackgroundColor(const NiColor& c) = 0;           // slot 27 (vf6C) 0x0014B150
    // Slots 21 / 22 (vf54 / vf58) 0x00146710 / 0x00146720: the current target's width / height in pixels.
    virtual u32 TargetWidth() const = 0;
    virtual u32 TargetHeight() const = 0;
    virtual void BeginScene() = 0;                                   // slot 45 (vfB4) 0x001489E0
    virtual void EndScene() = 0;                                     // slot 46 (vfB8) 0x00146A50
    // slot 47 (vfBC) 0x00146A70: the camera's world location, direction, up, right, frustum (l, r, t, b, n, f) and
    // viewport port (l, r, t, b).
    virtual void SetCameraData(const NiPoint3& loc, const NiPoint3& dir, const NiPoint3& up, const NiPoint3& right,
                               const f32 frustum[6], const f32 port[4]) = 0;
    // NiTriShape's display 0x00116760 (the part before the renderer slot): the geometry's property / effect state
    // (renderer +0xC / +0x10), the accumulator's RegisterObject (0x001401D0; true: kept for later), then slot 48
    // (vfC0) 0x0014C580.
    virtual void DisplayTriShape(NiGeometry& geometry, const NiCamera& camera) = 0;
    // NiParticles' display 0x0010F8B0: the same before the slot, then slot 52 (vfD0) 0x00148B70 (data +0xC8, world
    // +0x70, bound +0x20).
    virtual void DisplayParticles(NiGeometry& particles, const NiCamera& camera) = 0;
    // Slot 55 (vfDC) 0x0014C600: a screen texture (NiScreenTexture vf.. 0x0010B000: Click's loop over the camera's
    // +0x190 after the scene).
    virtual void DrawScreenTexture(NiScreenTexture& texture) = 0;
    // Slot 54 (vfD8) 0x0014B680 via 0x00106B80 (renderer +0xC = the polygon's property state +8, +0x10 = 0): a screen
    // polygon (Click's two loops over the camera's +0x160 after the scene).
    virtual void DrawScreenPolygon(NiScreenPolygon& polygon) = 0;
    // The camera's accumulator (renderer +8: the game's NiAlphaAccumulator): StartAccumulating (vf2C) before the cull,
    // FinishAccumulating (vf30) after it.
    virtual bool StartAccumulating(const NiCamera& camera) = 0;
    virtual void FinishAccumulating(NiCamera& camera) = 0;
    // renderer +8 (the AddRef'd pointer; 0x00153490 sets it): null while an object is drawn at once.
    virtual NiAccumulator* GetAccumulator() = 0;
    virtual void SetAccumulator(NiAccumulator* accumulator) = 0;
    // Slot 15 (vf3C) 0x00146490: the target of the following draws (a rendered texture; null: the back buffer);
    // false inside a scene or for a texture without renderer data.
    virtual bool SetRenderTarget(NiRenderedTexture* texture) = 0;
    // Slot 66 (vf108) 0x00147410: the renderer data of a rendered texture (NiRenderedTexture::Create 0x00140070).
    virtual bool CreateRenderedTextureData(NiRenderedTexture& texture) = 0;

    // The geometry-group manager (DAT_0030EC70: created by the renderer factory, NiXBoxRenderer +0x5AC): a group owns
    // the vertex buffers of the geometry data added to it (type 0: shared vertex buffers per vertex format, the
    // stages' +0xAC; 1: one buffer per geometry, the renderer's own +0x5B0).
    virtual NiGeometryGroup* CreateGeometryGroup(u32 type) = 0;              // manager vf04 0x0015C5C0 (type, 0)
    virtual void DestroyGeometryGroup(NiGeometryGroup* group) = 0;           // group vf00, manager vf08 0x0015C3D0
    virtual void AddToGeometryGroup(NiGeometryGroup* group, NiGeometryData& data) = 0;   // vf18 0x0015C7B0 (no skin)
    // Slot 39 (vf9C) 0x0014B1D0 (geometry, static, 0, 0, 0): the geometry's buffers, packed at the next BeginScene.
    virtual void PrecacheGeometry(NiGeometry& geometry, bool isStatic) = 0;
};

// orig: DAT_002D79A0 (static 1): textures are precached when they are loaded or made.
extern bool g_precacheTextures;
// orig: 0x00118050 (NiSourceTexture): each renderer of the list DAT_002D7BBC precaches it (vf104), stopping at the
// first that fails; called by PostLinkObject 0x00118200 and the creators 0x001185B0 / 0x00118690.
bool PrecacheTexture(NiSourceTexture& texture);

}  // namespace fut::ni

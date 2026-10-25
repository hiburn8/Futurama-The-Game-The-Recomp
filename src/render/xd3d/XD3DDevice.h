// The Direct3D 8 the original renderer sees (XDK 5233, statically linked; docs/analysis/renderer_d3d_map.md 5-6):
// the library state the game reads and writes inline (D3D__RenderState, D3D__TextureState, D3D__IndexData), the
// resources in their Xbox layouts (ordinary memory: vertex bytes, u16 indices, texels) and the call surface the
// decompiled renderer (src/render/nixbox) calls where the original calls D3D.
//
// The base class is the library's memory-only part: every call updates the state as the XDK code does (the state
// arrays, the bindings, the resources); what reaches the GPU is the backends' business: D3D11Device executes the
// draws (the tests' recording backend writes the render oracle's normalised lines). Calls are virtual so a backend
// sees each one; a backend calls the base first. Inline state writes (the game's inlined deferred SetRenderState /
// SetTextureStageState) go straight into renderState / textureState, as in the original: the recording backend
// finds them by diffing the arrays at the next call or draw, as the oracle does.
#pragma once

#include "render/xd3d/XD3DTypes.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fut::render::xd3d {

enum class ResourceKind : u8 { kVertexBuffer, kIndexBuffer, kTexture, kSurface, kPalette, kCubeTexture };

// A D3D resource. `id` is the oracle's handle name: the kind's prefix and its number in creation order (vb12, ib4,
// tex3, surf2, pal1), so 1:1 ported managers produce the oracle's numbers.
struct Resource {
    ResourceKind kind = ResourceKind::kVertexBuffer;
    std::string id;
    std::vector<u8> data;                             // the contents in the Xbox layout
    u32 refs = 1;                                     // Common bits 0-15 (the creator's reference)
    u32 width = 0, height = 0, format = 0, levels = 0;   // pixel containers
    Resource* parent = nullptr;                       // a surface's texture
    u32 level = 0;                                    // a texture surface's level
    bool rendered = false;                            // a render target's texture (GPU-written content)
    const void* source = nullptr;                     // a texture's engine object (the NiSourceTexture: the backend's texels)
    u32 revision = 0;                                 // a texture's LockRect count (a backend re-uploads changed texels)
};

class XD3DDevice {
public:
    XD3DDevice();
    virtual ~XD3DDevice();
    XD3DDevice(const XD3DDevice&) = delete;
    XD3DDevice& operator=(const XD3DDevice&) = delete;

    // ---- the library state (H: the globals of renderer_d3d_map.md 6) ----
    u32 renderState[kNumRenderStates] = {};           // D3D__RenderState 0x00244770
    u32 textureState[kNumStages][kNumTextureStates] = {};   // D3D__TextureState 0x00244570
    u32 dirtyFlags = 0;                               // D3D__DirtyFlags 0x00244568 (written by the game; unread here)
    const u16* indexData = nullptr;                   // D3D__IndexData 0x00244564 (SetIndices: the buffer's data)

    // the bindings (what the oracle tracks)
    D3DMATRIX transforms[kNumTransforms] = {};
    bool transformSet[kNumTransforms] = {};
    D3DVIEWPORT8 viewport;
    bool viewportSet = false;
    Resource* textures[kNumStages] = {};
    Resource* palettes[kNumStages] = {};
    Resource* streamSource = nullptr;                 // stream 0 (the renderer uses no other)
    u32 streamStride = 0;
    Resource* indices = nullptr;
    u32 baseVertex = 0;
    u32 vertexShader = 0;                             // an FVF (the renderer creates no vertex shader programs)
    u32 pixelShader = 0;                              // 0 or a handle of CreatePixelShader
    Resource* renderTarget = nullptr;
    Resource* depthStencil = nullptr;
    D3DMATERIAL8 material{};
    bool materialSet = false;
    std::map<u32, D3DLIGHT8> lights;                  // SetLight by index
    std::map<u32, bool> lightEnabled;                 // LightEnable by index
    f32 screenSpaceOffset[2] = {};

    Resource* backBuffer() const { return frameBuffers_[0]; }
    Resource* depthBuffer() const { return frameBuffers_[1]; }

    // ---- the call surface (renderer_d3d_map.md 5) ----
    // InitializeD3dState 0x00239660 (run by Direct3D_CreateDevice): the identity transforms, the default render
    // states (table 0x002427E8 for 0x39..0xA5, skipping 0x7E, 0x7F, 0x9A), ZENABLE from the auto depth buffer, the
    // texture-stage defaults (byte table 0x002427C8), TEXCOORDINDEX = stage, no textures, stage 0 COLOROP 4 /
    // ALPHAOP 2, SetScreenSpaceOffset(0, 0). (L) the NotInline setters store their value unchanged.
    virtual void InitializeState();

    virtual Resource* CreateVertexBuffer2(u32 length);
    virtual Resource* CreateIndexBuffer2(u32 length);
    // D3DDevice_CreateTexture2 0x00237170 (width, height, depth 1, levels, usage, format, type 3): a texture (usage
    // bit 0, D3DUSAGE_RENDERTARGET: `rendered`); its texels in `data` in the Xbox layout (the levels one after the
    // other, each LevelBytes; swizzled formats in the swizzled order).
    virtual Resource* CreateTexture2(u32 width, u32 height, u32 levels, u32 usage, u32 format);
    // D3DSurface_GetDesc + D3DSurface_LockRect (surface, rect 0, flags 0): a texture level's texels.
    virtual u8* LockRect(Resource* surface);
    // D3D_CreateStandAloneSurface 0x0023CF80 (width, height, 2, format): a surface of its own (the render targets'
    // depth buffers).
    virtual Resource* CreateStandAloneSurface(u32 width, u32 height, u32 format);
    // D3DTexture_GetSurfaceLevel2 0x00237240 (texture, level): the level's surface (a new handle per call, as in the
    // oracle; its parent the texture).
    virtual Resource* GetSurfaceLevel2(Resource* texture, u32 level);
    virtual u8* Lock2(Resource* vb, u32 flags);       // D3DVertexBuffer_Lock2: the data
    virtual void GetDesc(Resource* vb);               // D3DVertexBuffer_GetDesc (recorded only)
    virtual void AddRef(Resource* r);
    virtual void Release(Resource* r);                // (memory is never reused: handles are never reused either)

    virtual void SetRenderTarget(Resource* color, Resource* depth);
    virtual void SetViewport(const D3DVIEWPORT8& vp);
    virtual void Clear(u32 count, const D3DRECT* rects, u32 flags, u32 color, f32 z, u32 stencil);
    virtual void Swap(u32 flags);
    virtual void SetScreenSpaceOffset(f32 x, f32 y);
    virtual void SetTransform(u32 type, const D3DMATRIX& m);
    virtual void SetMaterial(const D3DMATERIAL8& m);
    virtual void SetLight(u32 index, const D3DLIGHT8& l);
    virtual void LightEnable(u32 index, bool enable);
    virtual void SetTexture(u32 stage, Resource* texture);
    virtual void SetStreamSource(u32 stream, Resource* vb, u32 stride);
    virtual void SetIndices(Resource* ib, u32 base);
    virtual void SetVertexShader(u32 handle);
    virtual void SetPixelShader(u32 handle);
    // SetRenderState_Simple (fastcall: the state's NV2A method, value) together with the caller's inline store into
    // D3D__RenderState that follows every simple call in the renderer (renderer_d3d_map.md 0).
    virtual void SetRenderState_Simple(u32 state, u32 value);
    // The complex setters (D3DDevice_SetRenderState_<Name>, state >= 0x88: the setter table 0x00242150); (L) each
    // stores its value into D3D__RenderState.
    virtual void SetRenderState_Complex(u32 state, u32 value);
    virtual void SetTextureState_TexCoordIndex(u32 stage, u32 value);
    virtual void SetTextureState_BumpEnv(u32 stage, u32 state, u32 value);
    virtual void DrawVertices(u32 primitive, u32 start, u32 count);
    // `indices` points into the current index buffer's data (the renderer passes D3D__IndexData + 2 * start).
    virtual void DrawIndexedVertices(u32 primitive, u32 count, const u16* indices);

    // The resource whose data holds `p` (nullptr: none).
    Resource* FindResource(const void* p, ResourceKind kind) const;

protected:
    Resource* NewResource(ResourceKind kind);
    std::vector<std::unique_ptr<Resource>> resources_;
    u32 counts_[5] = {};                              // per handle prefix: vb, ib, tex (cube textures too), surf, pal
    Resource* frameBuffers_[2] = {};
};

}  // namespace fut::render::xd3d

// NiXBoxRenderState (renderer +0x5C0, 0xB7C bytes, vtable 0x002AB7A0, ctor 0x00158E70, created by 0x00159CB0): the
// renderer's caches of the D3D render / texture-stage states and bindings, the per-property setters and the
// material helper (docs/analysis/renderer_lighting_port.md 3). Every write goes through its cache: the device is
// written (the call, or the inline store of the XDK's deferred states with D3D__DirtyFlags) only when the cached
// value differs. The caches start from the renderer's own tables 0x002D83D8 / 0x002D8358, not from the device: a
// state the renderer believes set is never written (DIFFUSEMATERIALSOURCE, stage 0 COLORARG2: section 3.1).
#pragma once

#include "engine/ni/NiObjects.h"
#include "render/xd3d/XD3DDevice.h"

namespace fut::ni {
class NiAlphaProperty;
class NiFogProperty;
class NiMaterialProperty;
class NiPropertyState;
class NiStencilProperty;
class NiShadeProperty;
class NiZBufferProperty;
struct NiColor;
}  // namespace fut::ni

namespace fut::render::nixbox {

class NiXBoxRenderState {
public:
    // orig: 0x00159CB0 (device, caps, 1, 1, DAT_0030EE84, (renderer +0x2D8 >> 7) & 1): the ctor 0x00158E70, the
    // flags +0xE0 (0x00158DB0 from the caps), then the defaults 0x001594A0.
    explicit NiXBoxRenderState(xd3d::XD3DDevice& device);
    NiXBoxRenderState(const NiXBoxRenderState&) = delete;
    NiXBoxRenderState& operator=(const NiXBoxRenderState&) = delete;

    void BeginScene();                                // orig: 0x00158C20
    void UpdateRenderState(const ni::NiPropertyState& state);   // orig: 0x00159D60
    void ApplyAlpha(const ni::NiAlphaProperty* alpha);   // orig: 0x00159300
    // orig: 0x00159290 (from the world transform 0x00148A30): NORMALIZENORMALS outside [0.99, 1.01]
    void SetNormalize(f32 worldScale);
    // orig: 0x00147C00 (count): VERTEXBLEND = the table +0xE8 [count] when it differs from the cache
    void SetVertexBlend(u32 count);
    // orig: 0x00147950 (state, value, saveDefault): the cached SetRenderState of any state
    void SetRenderState(u32 state, u32 value, bool saveDefault = false);
    // The inline deferred store (the XDK's deferred states 0x5C..0x87): the cache, then D3D__RenderState and
    // D3D__DirtyFlags when it differs.
    void SetDeferred(u32 state, u32 value, u32 dirty);
    // The simple ones (< 0x5C): SetRenderState_Simple and the inline store, when the cache differs.
    void SetSimple(u32 state, u32 value);
    // The complex ones (>= 0x88): their setter, when the cache differs.
    void SetComplex(u32 state, u32 value);
    // The texture-stage cache (+0x76C + 0x100 stage + 8 state): the inline store into D3D__TextureState with the
    // dirty bits, when it differs.
    void SetStage(u32 stage, u32 state, u32 value, u32 dirty);
    void SetTexture(u32 stage, xd3d::Resource* texture);   // the cache +0xB6C (SetTexture when it differs)
    void SetPixelShader(u32 handle);                  // +0x75C (0x00163D60)
    void SetVertexShader(u32 handle);                 // +0x764 (0x00163DA0)

    u32 rs[xd3d::kNumRenderStates][2] = {};           // +0x22C: [0] the value believed set, [1] the saved default
    u32 tss[xd3d::kNumStages][xd3d::kNumTextureStates][2] = {};   // +0x76C
    u32 pixelShader[2] = {}, vertexShader[2] = {};    // +0x75C / +0x764
    xd3d::Resource* textures[xd3d::kNumStages] = {};  // +0xB6C
    u32 flags = 0;                                    // +0xE0 (bit 0 fog table caps, 1, 3: the creation flags, 4)
    bool flagE4 = false;                              // +0xE4 (DAT_0030EE84)
    u32 vertexBlendTable[5] = {0, 1, 1, 5, 5};        // +0xE8 (count -> D3DVERTEXBLENDFLAGS)
    u32 blend[11] = {};                               // +0x100 (Gamebryo AlphaFunction -> D3DBLEND)
    u32 test[8] = {};                                 // +0x12C (Gamebryo TestFunction -> D3DCMPFUNC)
    const ni::NiAlphaProperty* lastAlpha = nullptr;   // +0x164
    u32 fogColor = 0;                                 // +0x16C
    f32 nearPlane = 0, farPlane = 0, range = 0;       // +0x178 / +0x17C / +0x180 (SetCameraData)
    f32 fogScale = 0;                                 // +0x184
    bool fogOffWithout = false;                       // +0x18C (L: never set)
    const ni::NiFogProperty* lastFog = nullptr;       // +0x190
    // The last property bits (bytes; not written by the ctor: L 0)
    u8 dither = 0, shade = 0, specular = 0, stencilOn = 0, wireframe = 0, zWrite = 0, zTest = 0;   // +0x195..+0x19C
    u32 stencilTest[8] = {}, stencilAction[6] = {};   // +0x1A0 / +0x1C0
    u32 cullTable[8] = {};                            // +0x1D8 [flip + 2 draw mode]
    u32 lastCull = 0x900;                             // +0x1F8
    u32 cullFlip = 0;                                 // +0x1FC
    bool normalize = false;                           // +0x220
    bool reapply = false;                             // +0x228 (alpha / fog applied again)

    // The material helper (+0x200, ctor 0x001619D0 / 0x00161B90): the current renderer data and its dirty flag.
    struct MaterialData : ni::NiPropertyRendererData {   // (0x48 bytes: D3DMATERIAL8 + the revision +0x44)
        xd3d::D3DMATERIAL8 mat{};
        u32 revision = 0;
    };
    void UpdateMaterial(ni::NiMaterialProperty& m);   // orig: 0x00161B10
    void ApplyMaterial();                             // orig: 0x00161910 (SetMaterial when dirty)
    const MaterialData* currentMaterial = nullptr;    // +0x218
    bool materialDirty = false;                       // +0x21C

    xd3d::XD3DDevice& d3d;

private:
    void SetDefaults();                               // orig: 0x001594A0
    void SetStageDefaults();                          // orig: 0x00158C40
    void ApplyShade(const ni::NiShadeProperty* p);    // orig: 0x00159A10
    void ApplyZBuffer(const ni::NiZBufferProperty* p);   // orig: 0x00159BF0
    void ApplyFog(const ni::NiFogProperty* p);        // orig: 0x00159790
    void ApplyFogColor(const ni::NiColor& c);         // orig: 0x00159990
    void ApplyStencil(const ni::NiStencilProperty* p);   // orig: 0x00159A70
};

}  // namespace fut::render::nixbox

// The configurable texture pipeline (renderer +0x5C8: the manager, 0xB50 bytes, ctor 0x0015B570 from 0x0015B690)
// with its default pipeline (manager +0xC, 0x2FE0 bytes, ctor 0x00164090, vtable 0x002AC1A4): the texture stages,
// the passes, the texture effects' stages and coordinate generators, the stage apply (docs/analysis/
// renderer_lighting_port.md 4-5). The manager and the default pipeline are one class here; members carry the offsets
// of the object they belong to ("mgr" / "pipe"). The only other pipeline object is the screen textures' (0x001602B0,
// vtable 0x002ABE50: the default pipeline's ctor 0x00164090 with a clipper texture of its own, the passes 0x00160300;
// renderer_census_port.md 3.1): an instance of this class made by MakeScreenTexturePipeline that the manager's Begin
// 0x0015AAF0 selects (mgr +8) from a property state's renderer-specific property (NiXBoxPipelineProperty).
//
// Ported: the creation (0x0015ABD0, the tables 0x0015A1B0, the clipper texture 0x00163AD0), the pass builder
// 0x001643F0 (every branch; the projected lights' clipper 0x00162B60, the bump stage 0x001629A0 and the world-
// perspective / specular generators 0x00162EF0 / 0x001631F0 / 0x00163310 report NOT PORTED), the effect lists
// 0x00164290, the effect stages 0x00163FA0 with the generators 0x00162D20 / 0x001630D0 and the shadow clipper
// 0x00162C40, the next pass 0x00162940, the pass apply 0x00160070 / 0x00161D60 and the stage apply 0x001620B0
// (0x00162000 / 0x00161ED0 / 0x00161DF0), the shaders' caches 0x00163D60 / 0x00163DA0.
#pragma once

#include "engine/ni/NiObjects.h"
#include "render/nixbox/NiXBoxRenderState.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace fut::render::nixbox {

// The texture manager (renderer +0x5BC). Precache: the source texture data 0x0015E0A0 (NiXBoxSourceTextureData,
// new(0x70) 0x0015DA70) made when a texture is loaded or made (0x00118050): the format of the renderer's list
// matching the pixel data's (0x00157360; the image converter 0x0030F004 converts nothing: equal formats only),
// CreateSurf 0x0015D9B0 (CreateTexture2(w, h, 1, levels, 0, 0x000EDD00(format), 3)) and CopyDataToSurface
// 0x0015DB90 (each level through its surface, swizzled for the swizzled formats). Prepare (vf08, 0x0015F980..: NOT
// PORTED as such): a texture's D3D texture; one not precached (a creator not ported) is made at its first use (L).
// `set`: the texture is bound by the stage; `mips`: it has mip levels (0x00161ED0's choice).
class NiXBoxTextureManager {
public:
    explicit NiXBoxTextureManager(xd3d::XD3DDevice& device) : d3d_(device) {}
    bool Precache(ni::NiSourceTexture& texture);     // orig: 0x001473E0 / 0x0015E0A0
    bool Prepare(ni::NiObject* texture, xd3d::Resource*& d3d, bool& set, bool& mips);
    // orig: the source texture data's dtor (NiTexture 0x00118490 deletes +0x20): the D3D texture released, the
    // texture forgotten (a later texture at the same address is precached anew).
    void Purge(const ni::NiTexture& texture);
    // orig: the movie player's (0x000EE59A): the source texture data's D3D texture (+0x3C) released and replaced by
    // `d3d` (AddRef'd; a texture without data gets it as its data).
    void Replace(const ni::NiTexture& texture, xd3d::Resource* d3d);
    // The rendered textures' D3D textures (the renderer's +0x80 data): registered by CreateRenderedTextureData.
    std::map<const ni::NiObject*, xd3d::Resource*> rendered;

private:
    xd3d::XD3DDevice& d3d_;
    std::map<const ni::NiObject*, xd3d::Resource*> textures_;
};

class NiXBoxTexturePipeline;

// orig: NiRendererSpecificProperty (new(0x1C) 0x001176A0, +0x18 = the holder: new(0x10) 0x00147C30, vtable 0x002AA854,
// vf04 0x00147CC0 = type 6, +4 = the pipeline object (0x00147CD0)): DrawScreenTexture's state carries its pipeline
// object in it (the holder folded in). The object is reference counted (the manager's +8 keeps it alive too).
class NiXBoxPipelineProperty : public ni::NiProperty {
public:
    std::string_view ClassName() const override { return "NiRendererSpecificProperty"; }
    std::shared_ptr<NiXBoxTexturePipeline> pipeline;  // (holder +4)
};

class NiXBoxTexturePipeline {
public:
    // A texture stage (0xCC bytes, ctor 0x00161BB0).
    struct Stage {
        u32 index = 0;                                // +0x08
        ni::NiPointer<ni::NiObject> texture;          // +0x0C
        u32 colorOp = 2, colorArg1 = 2, colorArg2 = 0;   // +0x10 / +0x14 / +0x18
        u32 alphaOp = 2, alphaArg1 = 2, alphaArg2 = 0;   // +0x1C / +0x20 / +0x24
        u32 tci = 0;                                  // +0x28
        u32 addressU = 3, addressV = 3, addressW = 3;  // +0x2C / +0x30 / +0x34
        u32 filter = 0, mag = 1, min = 1, mip = 0;    // +0x38 / +0x3C / +0x40 / +0x44 (filter 0's)
        u32 ttf = 0;                                  // +0x48
        xd3d::D3DMATRIX matrix{};                     // +0x4C
        u32 textureFactor = 0xFFFFFFFFu;              // +0x8C
        bool lit = true;                              // +0x90
        u32 type = 9;                                 // +0x94
        bool bumpOn = false;                          // +0x98
        u32 bump[6] = {0, 0, 0, 0, 0x3F800000u, 0};   // +0x9C..+0xB0 (BUMPENVMAT00, 01, 11, 10, LSCALE, LOFFSET)
        const ni::NiObject* source = nullptr;         // +0xB4 (the map or the effect)
    };
    // A pass (0x44 bytes, ctor 0x0015FEB0).
    struct Pass {
        u32 next = 0;                                 // +0x08 (the next stage slot)
        u32 count = 0xFFFFFFFFu;                      // +0x0C (0xFFFFFFFF: unused)
        u32 textures = 0;                             // +0x10
        Stage* stages[8] = {};                        // +0x14..
        ni::NiPointer<ni::NiAlphaProperty> alpha;     // +0x38
    };

    NiXBoxTexturePipeline(xd3d::XD3DDevice& device, NiXBoxRenderState& state, NiXBoxTextureManager& textures);
    ~NiXBoxTexturePipeline();
    // orig: 0x001602B0 (new(0x2FE0)): the screen textures' pipeline object for this manager: the pipeline object part
    // of the ctor (0x00164090: the stage arrays' fixed fields 0x00163480, its own clipper texture 0x00163AD0, which
    // precaches a 2x1 texture), then vtable 0x002ABE50 (BuildPasses = 0x00160300). The manager's tables (mgr) are
    // the manager's: copied.
    std::shared_ptr<NiXBoxTexturePipeline> MakeScreenTexturePipeline() const;

    // orig: 0x00159F00 (from SetCameraData 0x00146A70 @0x00146BD6): mgr +0xB10 right, +0xB20 up, +0xB30 dir, +0xB40 loc
    void SetCameraData(const ni::NiPoint3& dir, const ni::NiPoint3& up, const ni::NiPoint3& right,
                       const ni::NiPoint3& loc);
    void BeginScene();                                // orig: 0x00159F80 (vf18 0x001624B0, mgr +0xB08 = 0)
    // orig: 0x0015AAF0: mgr +8 = the pipeline object of the property state's renderer-specific property (type 6),
    // else the default pipeline (+0xC); on a change the new one's vf18 (ResetEffects) and mgr +0xB08 = 0; then its
    // vf10 (0x001624A0, no-op)
    void Begin(const ni::NiPropertyState& props);
    // orig: 0x00159EE0 -> vf08 0x001643F0 (props, effects, ..., the texture sets): the passes; the flags
    u32 BuildPasses(const ni::NiPropertyState& props, const ni::NiDynamicEffectState* effects, u32 textureSets);
    u32 NextPass();                                   // orig: 0x00159EF0 -> vf0C 0x00162940
    void ApplyPixelShader();                          // orig: 0x00159FA0 -> 0x00163D60
    void ApplyVertexShader(u32 fvf);                  // orig: 0x00159FB0 -> 0x00163DA0
    void End() {}                                     // orig: 0x0015AB90 (mgr +8 null: the default; vf14 0x001624A0: no-op)

    xd3d::Resource* clipperTexture() const { return clipper_; }
    // mgr +0xFC, set from the device caps at the manager's creation (0x0015A041..0x0015A0A5): bit 0 = caps
    // TextureAddressCaps & 0x10 (D3DPTADDRESSCAPS_INDEPENDENTUV: ADDRESSV of its own; O: set, the census's CLAMP /
    // WRAP stages of level4-1), bit 7 = & 4, bit 8 (and +0x310 = 0x103) = TextureCaps & 0x400. Bits 7 / 8 are not
    // read by the ported code (L: their values on the XDK device; left 0).
    u32 flags = 1;

private:
    struct ScreenTextureTag {};
    NiXBoxTexturePipeline(const NiXBoxTexturePipeline& manager, ScreenTextureTag);   // orig: 0x001602B0
    void InitPipelineObject();                        // orig: 0x00164090's 0x00163480 and 0x00163AD0
    NiXBoxTexturePipeline& Obj() { return active_ ? *active_ : *this; }   // mgr +8
    u32 BuildDefaultPasses(const ni::NiPropertyState& props, const ni::NiDynamicEffectState* effects, u32 sets);
    u32 BuildScreenTexturePasses(const ni::NiPropertyState& props, u32 sets);   // orig: 0x00160300
    void CreateTables();                              // orig: 0x0015A1B0 (+ 0x00159FD0 the filter table)
    void Create();                                    // orig: 0x0015ABD0
    void ResetPasses();                               // orig: 0x00162900
    void ResetEffects();                              // orig: vf18 0x001624B0
    void UpdateEffects(const ni::NiDynamicEffectState* effects);   // orig: 0x00164290
    bool EffectStage(ni::NiTextureEffect& e, Stage& st);   // orig: 0x00163FA0
    void GenWorldParallel(Stage& st, const ni::NiTextureEffect& e);   // orig: 0x00162D20 (type 0)
    void GenSphereMap(Stage& st, const ni::NiTextureEffect& e);       // orig: 0x001630D0 (type 2)
    void GenShadowClipper(const ni::NiTextureEffect& e);              // orig: 0x00162C40
    Stage* PoolStage(ni::NiObject* texture, u32 clamp, u32 filter, u32 type, u32 set, u32 sets, const void* source);
    Pass* CurrentPass();                              // (the +0x14 pass made on demand: the prologue of 0x001625B0 / 0x00162700 / 0x001627D0)
    Pass* PassWithRoom(u32 stages, u32 textures);     // orig: 0x001625B0
    bool NeedsNewPass(u32 type, u32 stages, u32 textures);   // orig: 0x00162700
    void NewPass();                                   // orig: 0x001627D0
    static void Room(const Pass& p, u32& stages, u32& textures);   // orig: 0x0015FE60
    bool AddStage(Pass& p, Stage* st);                // orig: 0x00160100
    u32 AddStages(Pass& p, Stage* a, Stage* b, bool needBoth);   // orig: 0x00160170
    bool AddToPass(Stage* st, u32 stages, u32 textures);   // orig: 0x00163EB0
    u32 AddPairToPass(Stage* a, Stage* b, bool needBoth, u32 stages, u32 textures);   // orig: 0x00163F30
    void SetPassAlpha(Pass& p);                       // orig: 0x00163430 with mgr +0x2E8 [first stage's type]
    void SetOps(Stage* st, bool later);               // orig: 0x00162A50
    u32 ApplyPass(Pass& p);                           // orig: pass vf04 0x00160070
    void ApplyStage(Stage& st);                       // orig: 0x001620B0
    void DisableStage(u32 s);                         // orig: 0x00161D60

    xd3d::XD3DDevice& d3d_;
    NiXBoxRenderState& state_;                        // pipe +0x2FD4
    NiXBoxTextureManager& textures_;                  // pipe +0x2FDC

    // ---- the manager (0xB50) ----
    u32 firstOps_[10][6] = {};                        // mgr +0x108 + 0x18 type
    u32 laterOps_[10][6] = {};                        // mgr +0x1F8 + 0x18 type
    ni::NiPointer<ni::NiAlphaProperty> passAlpha_[10];   // mgr +0x2E8 + 4 type
    u32 bumpOp_ = 0;                                  // mgr +0x104
    ni::NiPoint3 right_, up_, dir_, loc_;             // mgr +0xB10 / +0xB20 / +0xB30 / +0xB40
    const ni::NiDynamicEffectState* lastEffects_ = nullptr;   // mgr +0xB08 (only compared)
    std::unique_ptr<Stage> scratch_[3];               // mgr +0x10..+0x18

    // ---- the default pipeline (0x2FE0) ----
    u32 current_ = 0;                                 // pipe +0x08 (the pass NextPass hands out)
    u32 passCount_ = 0;                               // pipe +0x0C
    Pass* currentPass_ = nullptr;                     // pipe +0x14
    std::vector<std::unique_ptr<Pass>> passes_;       // pipe +0x1C (NiTPrimitiveArray of passes)
    u32 builtPasses_ = 0;                             // pipe +0x34
    u32 passKind_ = 0;                                // pipe +0x30 (L: 7 / 8 in the projected-light branch)
    Stage pool_[8];                                   // pipe +0x40 (the textures' maps)
    u32 poolNext_ = 0;                                // pipe +0x6A0
    Stage empty_;                                     // pipe +0x6A4
    u32 effectStages_ = 0;                            // pipe +0x770
    Stage projLights_[8], clippedLights_[8], lightClippers_[8];   // pipe +0x788 / +0xDF0 / +0x1450
    Stage clippedShadows_[8], shadowClippers_[8], shadows_[8];    // pipe +0x1AB4 / +0x2114 / +0x2778
    u32 projLightCount_ = 0, clippedLightCount_ = 0;  // pipe +0xDE8 / +0x1AB0
    u32 clippedShadowCount_ = 0, shadowCount_ = 0;    // pipe +0x2774 / +0x2DD8
    Stage envStage_;                                  // pipe +0x2DDC
    bool envOn_ = false;                              // pipe +0x2EA8
    Stage fogStage_;                                  // pipe +0x2EAC
    bool fogOn_ = false;                              // pipe +0x2F78
    u32 applyMode_ = 0;                               // pipe +0x2F7C
    const ni::NiDynamicEffectState* effects_ = nullptr;   // pipe +0x2F80
    xd3d::Resource* clipper_ = nullptr;               // the 2x1 clipper texture (0x00163AD0, pipe +0xDEC)
    ni::NiPointer<ni::NiObject> clipperObject_;       // (its NiSourceTexture)
    // mgr +8, the current pipeline object (null: this default pipeline, mgr +0xC)
    std::shared_ptr<NiXBoxTexturePipeline> active_;
    bool screenTexture_ = false;                      // (vtable 0x002ABE50: a screen textures' pipeline object)
};

}  // namespace fut::render::nixbox

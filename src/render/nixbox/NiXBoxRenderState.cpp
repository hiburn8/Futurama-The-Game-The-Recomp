// NiXBoxRenderState; see NiXBoxRenderState.h. x87 arithmetic in the original's order (double = the extended
// register, F() = a float store).
#include "render/nixbox/NiXBoxRenderState.h"

#include "core/XbeImage.h"
#include "engine/ni/NiObjects.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace fut::render::nixbox {

using ni::detail::F;
using namespace xd3d;

namespace {

// (H) the renderer's default render states (u32 table 0x002D83D8 + 4 state, used for 57..165) and texture-stage
// states (u32 table 0x002D8358), NetImmerse's data; D3D__DirtyFlags of the deferred states 0x5C..0x87 (u32 table
// 0x002936A8 + 4 state), the XDK D3D8 library's. Read from the user's default.xbe (core/XbeImage.h).
constexpr u32 kTableRs = 0x002D83D8, kTableTss = 0x002D8358, kDeferredDirty = 0x002936A8;

// orig: _ftol 0x00167110 (truncation toward zero)
u32 Ftol(double v) { return static_cast<u32>(static_cast<s32>(v)); }

u32 Bits(f32 f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

}  // namespace

NiXBoxRenderState::NiXBoxRenderState(XD3DDevice& device) : d3d(device) {   // orig: 0x00159CB0 -> 0x00158E70
    for (auto& r : rs) r[0] = r[1] = 0x7FFFFFFFu;
    pixelShader[0] = pixelShader[1] = vertexShader[0] = vertexShader[1] = 0x7FFFFFFFu;
    for (auto& s : tss)
        for (auto& t : s) t[0] = t[1] = 0x7FFFFFFFu;
    vertexBlendTable[0] = 0, vertexBlendTable[1] = 1, vertexBlendTable[2] = 1, vertexBlendTable[3] = 5;
    vertexBlendTable[4] = 5;                          // (+0xE8..+0xF8; +0xFC = 4)
    const u32 blendTable[11] = {1, 0, 0x300, 0x301, 0x306, 0x307, 0x302, 0x303, 0x304, 0x305, 0x308};
    std::memcpy(blend, blendTable, sizeof blend);
    const u32 testTable[8] = {0x207, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x200};
    std::memcpy(test, testTable, sizeof test);
    for (u32 i = 0; i < 8; ++i) stencilTest[i] = 0x200 + i;   // +0x1A0..+0x1BC
    const u32 actions[6] = {0x1E00, 0, 0x1E01, 0x1E02, 0x1E03, 0x150A};
    std::memcpy(stencilAction, actions, sizeof actions);
    const u32 cull[8] = {0x900, 0x901, 0x900, 0x901, 0x901, 0x900, 0, 0};
    std::memcpy(cullTable, cull, sizeof cull);
    lastCull = 0x900, cullFlip = 0;
    fogColor = 0, lastFog = nullptr, lastAlpha = nullptr, fogScale = 0;
    // (+0x170 / +0x174 = -1.0f, +0x188 = 1.0f: L, not read by the ported setters; +0xC.. the caps (0x35 dwords))
    // 0x00159CB0: +0xE0 |= 2 (its first flag 1); 0x00158DB0: bit 0 = the caps' table fog (RasterCaps 0x100 with
    // 0x100000: L, the Xbox has both), the blend masks +0x14C / +0x150, bit 4 = RasterCaps 0x40000 (L: set); |= 8 (its
    // second flag 1); +0xE4 = DAT_0030EE84 (0); (renderer +0x2D8 >> 7) & 1 -> |= 4 (L: 0)
    flags = 2 | 1 | 0x10 | 8;
    flagE4 = false;
    SetDefaults();
}

void NiXBoxRenderState::SetSimple(u32 s, u32 v) {
    if (rs[s][0] == v) return;
    rs[s][0] = v;
    d3d.SetRenderState_Simple(s, v);                  // (the call and the caller's inline store)
}

void NiXBoxRenderState::SetDeferred(u32 s, u32 v, u32 dirty) {
    if (rs[s][0] == v) return;
    rs[s][0] = v;
    d3d.dirtyFlags |= dirty;
    d3d.renderState[s] = v;
}

void NiXBoxRenderState::SetComplex(u32 s, u32 v) {
    if (rs[s][0] == v) return;
    rs[s][0] = v;
    d3d.SetRenderState_Complex(s, v);
}

void NiXBoxRenderState::SetRenderState(u32 s, u32 v, bool saveDefault) {   // orig: 0x00147950
    if (s >= kNumRenderStates || rs[s][0] == v) return;
    if (saveDefault) rs[s][1] = rs[s][0];
    rs[s][0] = v;
    if (s < kFirstDeferredState) {
        d3d.SetRenderState_Simple(s, v);
    } else if (s < kFirstComplexState) {
        d3d.renderState[s] = v;
        d3d.dirtyFlags |= xbe::GameImage().Word(kDeferredDirty + 4 * s);
    } else {
        d3d.SetRenderState_Complex(s, v);             // 0x88..0xA5: each its own setter
    }
}

void NiXBoxRenderState::SetStage(u32 stage, u32 state, u32 v, u32 dirty) {
    if (tss[stage][state][0] == v) return;
    tss[stage][state][0] = v;
    d3d.dirtyFlags |= dirty;
    d3d.textureState[stage][state] = v;
}

void NiXBoxRenderState::SetTexture(u32 stage, Resource* t) {
    if (textures[stage] == t) return;
    textures[stage] = t;
    d3d.SetTexture(stage, t);
}

void NiXBoxRenderState::SetPixelShader(u32 h) {       // orig: 0x00163D60 (the cache +0x75C)
    if (pixelShader[0] == h) return;
    pixelShader[0] = h;
    d3d.SetPixelShader(h);
}

void NiXBoxRenderState::SetVertexShader(u32 h) {      // orig: 0x00163DA0 (the cache +0x764)
    if (vertexShader[0] == h) return;
    vertexShader[0] = h;
    d3d.SetVertexShader(h);
}

void NiXBoxRenderState::SetStageDefaults() {          // orig: 0x00158C40
    for (u32 s = 0; s < kNumStages; ++s)
        for (u32 t = 0; t < kNumTextureStates; ++t) {
            u32 v = xbe::GameImage().Word(kTableTss + 4 * t);
            if (t == TSS_TEXCOORDINDEX) v = s;        // (H: the stage number, not the table)
            else if ((t == TSS_COLOROP || t == TSS_ALPHAOP) && s != 0) v = 1;
            tss[s][t][0] = tss[s][t][1] = v;
        }
}

void NiXBoxRenderState::SetDefaults() {               // orig: 0x001594A0
    for (u32 i = 57; i < kNumRenderStates; ++i) rs[i][0] = rs[i][1] = xbe::GameImage().Word(kTableRs + 4 * i);
    pixelShader[1] = pixelShader[0] = 0;
    vertexShader[1] = vertexShader[0] = 2;
    SetStageDefaults();
    for (auto& t : textures) t = nullptr;
    // 0x00161B90: the material helper: +0x18 = 0, Update(the default material DAT_002F98C4), dirty
    currentMaterial = nullptr;
    UpdateMaterial(*static_cast<ni::NiMaterialProperty*>(ni::NiPropertyState::Default(ni::NiPropertyState::kMaterial)));
    materialDirty = true;
    SetComplex(RS_CULLMODE, 0x900);
    SetComplex(RS_MULTISAMPLEANTIALIAS, 0);
    SetComplex(RS_FILLMODE, 0x1B02);
    SetSimple(RS_SHADEMODE, 0x1D01);
    SetSimple(RS_ALPHABLENDENABLE, 0);
    SetSimple(RS_SRCBLEND, blend[6]);                 // +0x118
    SetSimple(RS_DESTBLEND, blend[7]);                // +0x11C
    SetSimple(RS_ALPHATESTENABLE, 0);
    SetSimple(RS_ALPHAFUNC, 0x207);
    SetSimple(RS_ALPHAREF, 0);
    SetSimple(RS_ZFUNC, 0x207);
    SetSimple(RS_ZWRITEENABLE, 0);
    SetSimple(RS_DITHERENABLE, 0);
    SetDeferred(RS_SPECULARENABLE, 0, 0x3000);
    SetDeferred(RS_FOGENABLE, 0, 0x2000);
    SetDeferred(RS_FOGTABLEMODE, (flags & 1) ? 3u : 0u, 0x2000);
    SetRenderState(RS_RANGEFOGENABLE, 0, false);
    SetRenderState(RS_FOGCOLOR, 0, false);
    SetRenderState(RS_NORMALIZENORMALS, 0, false);
    SetRenderState(RS_ZENABLE, (flagE4 && ((flags >> 4) & 1)) ? 2u : 1u, false);
    SetRenderState(RS_COLORVERTEX, 1, false);
}

void NiXBoxRenderState::BeginScene() {                // orig: 0x00158C20
    lastFog = nullptr;
    lastAlpha = nullptr;
}

// ---- the per-property setters (0x00159D60) ------------------------------------------------------------------------
void NiXBoxRenderState::UpdateRenderState(const ni::NiPropertyState& s) {   // orig: 0x00159D60
    using S = ni::NiPropertyState;
    const ni::NiProperty* wire = s.slot[S::kWireframe].Get();
    const u8 w = wire ? u8(wire->flags & 1) : 0;
    if (w != wireframe) {
        SetComplex(RS_FILLMODE, w ? 0x1B01u : 0x1B02u);
        wireframe = w;
    }
    ApplyShade(static_cast<const ni::NiShadeProperty*>(s.slot[S::kShade].Get()));
    ApplyAlpha(static_cast<const ni::NiAlphaProperty*>(s.slot[S::kAlpha].Get()));
    ApplyZBuffer(static_cast<const ni::NiZBufferProperty*>(s.slot[S::kZBuffer].Get()));
    const auto* fog = static_cast<const ni::NiFogProperty*>(s.slot[S::kFog].Get());
    ApplyFog(fog);
    if (fog) ApplyFogColor(fog->color);
    const ni::NiProperty* dith = s.slot[S::kDither].Get();
    const u8 d = dith ? u8(dith->flags & 1) : 0;
    if (dither != d) {
        SetSimple(RS_DITHERENABLE, d);
        dither = d;
    }
    const ni::NiProperty* spec = s.slot[S::kSpecular].Get();
    const u8 sp = spec ? u8(spec->flags & 1) : 0;
    if (specular != sp) {
        SetDeferred(RS_SPECULARENABLE, sp, 0x3000);
        specular = sp;
    }
    ApplyStencil(static_cast<const ni::NiStencilProperty*>(s.slot[S::kStencil].Get()));
    if (auto* m = static_cast<ni::NiMaterialProperty*>(s.slot[S::kMaterial].Get())) UpdateMaterial(*m);
    ApplyMaterial();                                  // 0x00161910 (SetMaterial @0x00159E80)
    reapply = false;
}

void NiXBoxRenderState::ApplyShade(const ni::NiShadeProperty* p) {   // orig: 0x00159A10
    const u8 b = p ? u8(p->flags & 1) : 0;
    if (shade == b) return;
    SetSimple(RS_SHADEMODE, b ? 0x1D01u : 0x1D00u);
    shade = b;
}

void NiXBoxRenderState::ApplyAlpha(const ni::NiAlphaProperty* p) {   // orig: 0x00159300
    if (p == lastAlpha && !reapply) return;
    lastAlpha = p;
    if (!p) return;                                   // (the original reads the null property)
    const u32 f = p->flags;
    if (f & 1) {
        SetSimple(RS_ALPHABLENDENABLE, 1);
        // (an index above 10 reads the next table: UB-avoiding clamp)
        SetSimple(RS_SRCBLEND, blend[std::min<u32>(((f & 0xFF) >> 1) & 0xF, 10)]);
        SetSimple(RS_DESTBLEND, blend[std::min<u32>((f >> 5) & 0xF, 10)]);
    } else {
        SetSimple(RS_ALPHABLENDENABLE, 0);
    }
    if ((f >> 9) & 1) {
        SetSimple(RS_ALPHATESTENABLE, 1);
        SetSimple(RS_ALPHAFUNC, test[(f >> 10) & 7]);
        SetSimple(RS_ALPHAREF, p->alphaTestRef);
    } else {
        SetSimple(RS_ALPHATESTENABLE, 0);
    }
}

void NiXBoxRenderState::ApplyZBuffer(const ni::NiZBufferProperty* p) {   // orig: 0x00159BF0
    const u32 f = p ? p->flags : 0;
    const u8 t = u8(f & 1), wr = u8((f >> 1) & 1);
    if (zTest != t) {
        SetSimple(RS_ZFUNC, t ? 0x203u : 0x207u);
        zTest = t;
    }
    if (zWrite != wr) {
        SetSimple(RS_ZWRITEENABLE, wr);
        zWrite = wr;
    }
}

void NiXBoxRenderState::ApplyFog(const ni::NiFogProperty* p) {   // orig: 0x00159790
    if (p == lastFog && !reapply) return;
    lastFog = p;
    if (!p) return;                                   // (the original reads the null property)
    const u32 f = p->flags;
    if (!((f & 0xFF) >> 1 & 1)) {
        SetDeferred(RS_FOGENABLE, 0, 0x2000);
        return;
    }
    SetDeferred(RS_FOGENABLE, 1, 0x2000);
    const f32 a = F(double(p->depth) * range);        // FLD +0x18; FMUL +0x180; FSTP
    const double start = double(farPlane) - a;        // FLD +0x17C; FSUB a (kept)
    const f32 end = F(double(a) * fogScale + farPlane);   // FLD a; FMUL +0x184; FADD +0x17C; FSTP
    const f32 density = F(1.0 / double(a));           // FLD 1.0f [0x00293A98]; FDIV a; FSTP
    SetDeferred(RS_FOGDENSITY, Bits(density), 0x2000);
    SetDeferred(RS_FOGSTART, Bits(F(start)), 0x2000);
    SetDeferred(RS_FOGEND, Bits(end), 0x2000);
    const u32 fn = ((f & 0xFF) >> 3) & 3;
    if (fn == 0) {
        SetDeferred(RS_FOGTABLEMODE, 3, 0x2000);      // (the +0xE0 test's result is not used)
        SetDeferred(RS_RANGEFOGENABLE, 0, 0x2000);
    } else if (fn == 1) {
        if (flags & 1) SetDeferred(RS_FOGTABLEMODE, 1, 0x2000);
        SetDeferred(RS_RANGEFOGENABLE, 1, 0x2000);
    } else if (fogOffWithout) {
        SetDeferred(RS_FOGENABLE, 0, 0x2000);
    }
}

void NiXBoxRenderState::ApplyFogColor(const ni::NiColor& c) {   // orig: 0x00159990
    u32 v = (Ftol(double(c.r) * 255.0f) & 0xFF) | 0xFFFFFF00u;   // [0x002A1564] = 255.0f
    v = (v << 8) | (Ftol(double(c.g) * 255.0f) & 0xFF);
    v = (v << 8) | (Ftol(double(c.b) * 255.0f) & 0xFF);
    if (v == fogColor) return;
    SetComplex(RS_FOGCOLOR, v);
    fogColor = v;
}

void NiXBoxRenderState::ApplyStencil(const ni::NiStencilProperty* p) {   // orig: 0x00159A70
    if (!p) return;                                   // (the original reads the null property)
    if (p->enabled) {
        stencilOn = 1;
        SetComplex(RS_STENCILENABLE, 1);
        SetSimple(RS_STENCILFUNC, stencilTest[p->function & 7]);
        SetSimple(RS_STENCILREF, p->reference);
        SetSimple(RS_STENCILMASK, p->mask);
        SetComplex(RS_STENCILFAIL, stencilAction[std::min<u32>(p->failAction, 5)]);
        SetSimple(RS_STENCILZFAIL, stencilAction[std::min<u32>(p->zFailAction, 5)]);
        SetSimple(RS_STENCILPASS, stencilAction[std::min<u32>(p->passAction, 5)]);
    } else if (stencilOn) {
        stencilOn = 0;
        SetComplex(RS_STENCILENABLE, 0);
    }
    const u32 cull = cullTable[(cullFlip + 2 * p->drawMode) & 7];   // +0x1D8 [flip + 2 draw mode]
    if (cull == lastCull) return;
    lastCull = cull;
    SetComplex(RS_CULLMODE, cull);                    // @0x00159E64 (the tail call)
}

void NiXBoxRenderState::SetNormalize(f32 s) {         // orig: 0x00159290
    // [0x002AB7A4] = 0.99f, [0x002A20B0] = 1.01f (FCOMP: NaN normalises)
    const bool v = !(s >= 0.99f) || !(s <= 1.01f);
    if (v == normalize) return;
    SetComplex(RS_NORMALIZENORMALS, v ? 1u : 0u);
    normalize = v;
}

void NiXBoxRenderState::SetVertexBlend(u32 count) {   // orig: 0x00147C00
    const u32 v = count < 5 ? vertexBlendTable[count] : 4u;
    SetComplex(RS_VERTEXBLEND, v);
}

// ---- the material helper ------------------------------------------------------------------------------------------
void NiXBoxRenderState::UpdateMaterial(ni::NiMaterialProperty& m) {   // orig: 0x00161B10
    auto fill = [](MaterialData& d, const ni::NiMaterialProperty& p) {   // orig: 0x00161890
        if (d.revision == p.revision) return false;
        d.revision = p.revision;
        const f32 a = p.alpha;                        // +0x50 into every colour's alpha
        d.mat.emissive[0] = p.emissive.r, d.mat.emissive[1] = p.emissive.g, d.mat.emissive[2] = p.emissive.b;
        d.mat.emissive[3] = a;
        d.mat.ambient[0] = p.ambient.r, d.mat.ambient[1] = p.ambient.g, d.mat.ambient[2] = p.ambient.b;
        d.mat.ambient[3] = a;
        d.mat.diffuse[0] = p.diffuse.r, d.mat.diffuse[1] = p.diffuse.g, d.mat.diffuse[2] = p.diffuse.b;
        d.mat.diffuse[3] = a;
        d.mat.specular[0] = p.specular.r, d.mat.specular[1] = p.specular.g, d.mat.specular[2] = p.specular.b;
        d.mat.specular[3] = a;
        d.mat.power = p.shininess;                    // +0x4C
        return true;
    };
    if (!m.rendererData) {                            // new(0x48), +0x44 = 0, filled
        auto d = std::make_unique<MaterialData>();
        fill(*d, m);
        m.rendererData = std::move(d);
        materialDirty = true;
    }
    auto* data = static_cast<MaterialData*>(m.rendererData.get());
    if (fill(*data, m)) materialDirty = true;
    if (data != currentMaterial) {
        materialDirty = true;
        currentMaterial = data;
    }
}

void NiXBoxRenderState::ApplyMaterial() {             // orig: 0x00161910
    if (!materialDirty) return;
    materialDirty = false;
    if (currentMaterial) d3d.SetMaterial(currentMaterial->mat);
}

}  // namespace fut::render::nixbox

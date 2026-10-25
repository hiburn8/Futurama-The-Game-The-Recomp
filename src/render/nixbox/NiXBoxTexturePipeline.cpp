// The texture pipeline; see NiXBoxTexturePipeline.h. The branch structure of 0x001643F0 is the decompile's (map: the
// read-only Ghidra copy; the integer logic only), the generators' x87 orders the disassembly's
// (renderer_lighting_port.md 4.5).
#include "render/nixbox/NiXBoxTexturePipeline.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace fut::render::nixbox {

using ni::detail::F;
using namespace xd3d;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// (H) 0x00159FD0's filter table (0x0030FF98: {MIN, MAG, MIP} per Gamebryo FilterMode) and 0x00159E90's clamp
// table (0x0030FF78: {ADDRESSU, ADDRESSV} per ClampMode)
constexpr u32 kFilter[6][3] = {{1, 1, 0}, {2, 2, 0}, {2, 2, 2}, {1, 1, 1}, {1, 1, 2}, {2, 2, 1}};
constexpr u32 kClamp[4][2] = {{3, 3}, {3, 1}, {1, 3}, {1, 1}};

void SetFilter(NiXBoxTexturePipeline::Stage& st, u32 f) {   // orig: 0x00159EB0
    st.filter = f;
    const u32* t = kFilter[f < 6 ? f : 0];
    st.mag = t[1], st.min = t[0], st.mip = t[2];
}
void SetClamp(NiXBoxTexturePipeline::Stage& st, u32 c) {     // orig: 0x00159E90
    const u32* t = kClamp[c < 4 ? c : 0];
    st.addressU = t[0], st.addressV = t[1];
}

}  // namespace

// ---- the texture manager --------------------------------------------------------------------------------------------
namespace {

// orig: 0x000EDD00: the D3DFORMAT of a pixel format (+0 the format, +0x14 the bits per pixel, +0x18 the channel
// descriptor: its field (mask DAT_002D7C0C, shift DAT_002D7C1C: runtime tables, H: the green channel's bits, checked
// against the creation oracle's formats)). -1: none.
s32 D3DFormatOf(const ni::NiPixelData::Format& f) {
    u32 green = 0;
    for (u32 m = f.channelMasks[1]; m; m &= m - 1) ++green;
    switch (f.format) {
    case 0:                                           // RGB
        if (f.bitsPerPixel == 16) return green == 6 ? 5 : 3;   // R5G6B5 / X1R5G5B5
        if (f.bitsPerPixel == 24 || f.bitsPerPixel == 32) return 7;   // X8R8G8B8
        return -1;
    case 1:                                           // RGBA
        if (f.bitsPerPixel == 16) return green == 6 ? 5 : green == 4 ? 4 : 2;   // A4R4G4B4 / A1R5G5B5
        if (f.bitsPerPixel == 24) return 7;           // (a debug string first)
        if (f.bitsPerPixel == 32) return 6;           // A8R8G8B8
        return -1;
    case 2:
    case 3: return 0xB;                               // P8
    case 4: return 0xC;                               // DXT1
    case 5: return 0xE;                               // DXT3
    case 6: return 0xF;                               // DXT5
    case 8: return f.bitsPerPixel == 16 ? 0x28 : -1;
    case 9: return f.bitsPerPixel == 16 ? 0x27 : f.bitsPerPixel == 32 ? 7 : -1;
    default: return -1;
    }
}

bool PowerOfTwo(u32 v) { return v && !(v & (v - 1)); }   // (0x0015E120: the lowest set bit is the only one)

}  // namespace

bool NiXBoxTextureManager::Precache(ni::NiSourceTexture& texture) {   // orig: 0x001473E0 / 0x0015E0A0
    if (textures_.count(&texture)) return true;       // +0x20: the texture has renderer data
    if (texture.external) {                           // +0x30: the image converter's vf08 reads the file
        NotPorted("NiXBoxTextureManager::Precache: an external texture (the image converter's file read)");
        return true;
    }
    const ni::NiPixelData* pix = texture.pixelDataObj.Get();   // +0x34
    const s32 fmt = pix ? D3DFormatOf(pix->format) : -1;
    if (!pix || pix->mips.empty() || !PowerOfTwo(pix->mips[0].width) || !PowerOfTwo(pix->mips[0].height) ||
        fmt < 0) {
        // (0x00157360 without a match, or no pixel data: the renderer's default pixel data +0x5A0 / format +0x598)
        NotPorted("NiXBoxTextureManager::Precache: the renderer's default pixel data (+0x5A0 / +0x598)");
        return true;
    }
    if (pix->palette != ni::kNullLink) NotPorted("NiXBoxTextureManager::Precache: the palette 0x0015DFF0");
    // (+0x54 the mipmap flag: the preferences' mipmap 1, or 2 with DAT_0030F024 (0); the converter's vf10 0x00151540
    // returns the pixel data itself for the same format: its levels are the texture's)
    const u32 w = pix->mips[0].width, h = pix->mips[0].height, levels = u32(pix->mips.size());
    Resource* r = d3d_.CreateTexture2(w, h, levels, 0, u32(fmt));   // 0x0015D9B0
    if (!r) return false;
    r->source = &texture;
    // 0x0015DB90: per level its surface, GetDesc, LockRect; the swizzled formats (0x00158A20) with at least 8 bits a
    // texel are written texel by texel at the swizzled address (the Swizzler 0x001556F0: x bits then y bits, from
    // the lowest), the others copied whole (the pitch equals the level's rows here)
    const u32 bits = xd3d::FormatBits(u32(fmt));
    const bool swizzled = xd3d::FormatSwizzled(u32(fmt));
    for (u32 i = 0; i < levels; ++i) {
        Resource* s = d3d_.GetSurfaceLevel2(r, i);
        u8* dst = d3d_.LockRect(s);
        const ni::NiPixelData::MipLevel& m = pix->mips[i];
        const size_t begin = m.offset;
        const size_t end = i + 1 < pix->mips.size() ? pix->mips[i + 1].offset : pix->pixels.size();
        if (dst && swizzled && bits >= 8) {
            u32 xmask = 0, ymask = 0;
            for (u32 k = 1, bit = 1;; k <<= 1) {
                u32 used = 0;
                if (k < m.width) xmask |= bit, bit <<= 1, used = 1;
                if (k < m.height) ymask |= bit, bit <<= 1, used = 1;
                if (!used) break;
            }
            const u32 bpp = bits / 8;
            const size_t pitch = size_t(m.width) * pix->bytesPerPixel;   // the source rows (+0x44 bytes a texel)
            u32 yoff = 0;
            for (u32 y = 0; y < m.height; ++y) {
                u32 xoff = 0;
                const size_t row = begin + y * pitch;
                for (u32 x = 0; x < m.width; ++x) {
                    const size_t src = row + size_t(x) * bpp, at = size_t(xoff | yoff) * bpp;
                    for (u32 b = 0; b < bpp; ++b)     // (UB guard: bytes past either array are not touched)
                        if (src + b < pix->pixels.size() && dst + at + b < s->parent->data.data() + s->parent->data.size())
                            dst[at + b] = pix->pixels[src + b];
                    xoff = (xoff - xmask) & xmask;
                }
                yoff = (yoff - ymask) & ymask;
            }
        } else if (dst) {
            const size_t room = size_t(s->parent->data.data() + s->parent->data.size() - dst);
            const size_t n = std::min(end > begin ? end - begin : 0, room);
            if (begin + n <= pix->pixels.size()) std::memcpy(dst, pix->pixels.data() + begin, n);
        }
        d3d_.Release(s);                              // D3DResource_Release
    }
    textures_.emplace(&texture, r);
    return true;
}

void NiXBoxTextureManager::Purge(const ni::NiTexture& texture) {
    if (auto it = textures_.find(&texture); it != textures_.end()) {
        if (it->second) {
            it->second->source = nullptr;             // (the object dies: no texels to decode from it)
            d3d_.Release(it->second);
        }
        textures_.erase(it);
    }
    rendered.erase(&texture);                         // (the rendered texture's: released by the renderer)
}

void NiXBoxTextureManager::Replace(const ni::NiTexture& texture, Resource* d3d) {
    Resource*& slot = textures_[&texture];
    if (slot) d3d_.Release(slot);                     // D3DResource_Release (+0x3C; the texture cache off: 0x0030EE74)
    slot = d3d;
    if (d3d) d3d_.AddRef(d3d);                        // D3DResource_AddRef
}

bool NiXBoxTextureManager::Prepare(ni::NiObject* texture, Resource*& d3d, bool& set, bool& mips) {
    set = false, mips = false, d3d = nullptr;
    if (!texture) return false;
    if (auto it = rendered.find(texture); it != rendered.end()) {
        d3d = it->second;
        set = true;
        return d3d != nullptr;
    }
    auto it = textures_.find(texture);
    if (it == textures_.end()) {
        // (L) the source texture's D3D texture at its first use: its size and levels from the pixel data (the
        // formats and the texel conversion of NiXBoxSourceTextureData 0x0015D9B0 / 0x0015DB90: NOT PORTED, the
        // backend decodes the NiPixelData it keeps)
        u32 w = 1, h = 1, levels = 1;
        const auto* src = dynamic_cast<const ni::NiSourceTexture*>(texture);
        if (src && src->pixelDataObj && !src->pixelDataObj->mips.empty()) {
            w = src->pixelDataObj->mips[0].width, h = src->pixelDataObj->mips[0].height;
            levels = u32(src->pixelDataObj->mips.size());
        }
        Resource* r = d3d_.CreateTexture2(w, h, levels, 0, 6);
        if (r) r->source = texture;
        it = textures_.emplace(texture, r).first;
    }
    d3d = it->second;
    set = true;
    if (d3d) mips = d3d->levels > 1;
    return d3d != nullptr;
}

// ---- creation -------------------------------------------------------------------------------------------------------
NiXBoxTexturePipeline::NiXBoxTexturePipeline(XD3DDevice& device, NiXBoxRenderState& state,
                                             NiXBoxTextureManager& textures)
    : d3d_(device), state_(state), textures_(textures) {
    // 0x0015B690: new(0xB50) 0x0015B570 (the tables 0x0015A1B0, the default pipeline 0x0015AA60 = new(0x2FE0)
    // 0x00164090(renderer): 0x00163480 (the stage arrays' fixed fields), 0x00163AD0 (the clipper texture)), then
    // 0x00159FD0, 0x0015ABD0 (the stage caches)
    CreateTables();
    InitPipelineObject();
    Create();
}

NiXBoxTexturePipeline::NiXBoxTexturePipeline(const NiXBoxTexturePipeline& m, ScreenTextureTag)
    : d3d_(m.d3d_), state_(m.state_), textures_(m.textures_) {   // orig: 0x001602B0
    // (the manager's tables, read through the pipeline's manager pointer in the original)
    std::memcpy(firstOps_, m.firstOps_, sizeof firstOps_);
    std::memcpy(laterOps_, m.laterOps_, sizeof laterOps_);
    for (u32 t = 0; t < 10; ++t) passAlpha_[t] = m.passAlpha_[t];
    bumpOp_ = m.bumpOp_;
    flags = m.flags;
    InitPipelineObject();                             // 0x00164090
    screenTexture_ = true;                            // vtable 0x002ABE50
}

std::shared_ptr<NiXBoxTexturePipeline> NiXBoxTexturePipeline::MakeScreenTexturePipeline() const {
    return std::shared_ptr<NiXBoxTexturePipeline>(new NiXBoxTexturePipeline(*this, ScreenTextureTag{}));
}

void NiXBoxTexturePipeline::InitPipelineObject() {   // orig: 0x00164090 (0x00163480, 0x00163AD0)
    empty_.colorOp = 4, empty_.colorArg1 = 2, empty_.colorArg2 = 0;   // pipe +0x6A4 (0x00163480)
    empty_.alphaOp = 4, empty_.alphaArg1 = 2, empty_.alphaArg2 = 0;
    empty_.tci = 0, empty_.addressU = 1, empty_.addressV = 1, empty_.addressW = 1;
    SetFilter(empty_, 0);
    empty_.ttf = 0, empty_.textureFactor = 0, empty_.lit = true, empty_.type = 2;
    for (Stage& s : shadows_) s.addressU = 3, s.addressV = 3, s.filter = 0, s.tci = 0, s.ttf = 0, s.type = 2;
    for (Stage& s : clippedLights_) s.type = 0;
    for (Stage& s : clippedShadows_) s.type = 2;
    envStage_.type = 2;                               // +0x2E70
    fogStage_.type = 1;                               // +0x2F40
    // 0x00163AD0: a 2x1 NiPixelData of the renderer's format (+0x598: 32-bit), texel 0 = 0, texel 1 = 0xFFFFFFFF, its
    // NiSourceTexture (0x00118690) at +0xDEC; every clipper stage gets it (clamp, filter 0, TCI 0x20000, TTF 2, type 0)
    auto* pixels = new ni::NiPixelData;
    pixels->mips.resize(1);
    pixels->mips[0].width = 2, pixels->mips[0].height = 1;
    // (the renderer's 32-bit format: L the masks of +0x598, A8R8G8B8; all-zero / all-one texels read the same in any)
    pixels->format.format = 1, pixels->format.bitsPerPixel = 32, pixels->bytesPerPixel = 4;
    pixels->format.channelMasks[0] = 0x00FF0000u, pixels->format.channelMasks[1] = 0x0000FF00u;
    pixels->format.channelMasks[2] = 0x000000FFu, pixels->format.channelMasks[3] = 0xFF000000u;
    pixels->pixels = {0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    auto* src = new ni::NiSourceTexture;
    src->pixelDataObj = pixels;
    src->hasPixelData = true;
    clipperObject_ = src;
    textures_.Precache(*src);                         // (0x00118690 -> 0x00118050: this renderer's vf104)
    for (Stage* arr : {shadowClippers_, lightClippers_})
        for (u32 i = 0; i < 8; ++i) {
            Stage& s = arr[i];
            s.texture = clipperObject_.Get();
            s.addressU = 3, s.addressV = 3, s.filter = 0, s.tci = 0x20000, s.ttf = 2, s.bumpOn = false;
            s.source = nullptr, s.type = 0;
        }
    bool set = false, mips = false;
    textures_.Prepare(clipperObject_.Get(), clipper_, set, mips);
}

NiXBoxTexturePipeline::~NiXBoxTexturePipeline() = default;

void NiXBoxTexturePipeline::CreateTables() {          // orig: 0x0015A1B0
    // the pass alpha properties (new NiAlphaProperty: flags 0xEC, then each type's blend)
    const u16 alphaFlags[10] = {0x0001, 0x00ED, 0x0043, 0x00EC, 0x1243, 0x00EC, 0x00ED, 0x00EC, 0x0001, 0x00EC};
    for (u32 t = 0; t < 10; ++t) {
        auto* a = new ni::NiAlphaProperty;
        a->flags = alphaFlags[t];
        a->alphaTestRef = t == 4 ? 0x80 : 0;
        passAlpha_[t] = a;
    }
    bumpOp_ = 0x1A;                                   // (the caps' bump-env op: L, D3DTOP_BUMPENVMAPLUMINANCE)
    const u32 first[10][6] = {{2, 2, 0, 2, 2, 0}, {4, 2, 0, 4, 2, 0}, {4, 2, 0, 4, 2, 0}, {0, 0, 0, 0, 0, 0},
                              {4, 2, 0, 4, 2, 0}, {0, 0, 0, 0, 0, 0}, {4, 2, 0, 2, 2, 0}, {0, 0, 0, 0, 0, 0},
                              {bumpOp_, 2, 1, 3, 2, 1}, {0, 0, 0, 0, 0, 0}};
    const u32 later[10][6] = {{7, 2, 1, 3, 2, 1}, {14, 2, 1, 3, 2, 1}, {4, 2, 1, 4, 2, 1}, {5, 2, 1, 3, 2, 1},
                              {2, 2, 1, 2, 2, 1}, {2, 2, 1, 3, 2, 1}, {4, 2, 1, 2, 2, 1}, {4, 2, 1, 3, 2, 1},
                              {bumpOp_, 2, 1, 3, 2, 1}, {4, 2, 1, 4, 2, 1}};
    std::memcpy(firstOps_, first, sizeof firstOps_);
    std::memcpy(laterOps_, later, sizeof laterOps_);
}

void NiXBoxTexturePipeline::Create() {                // orig: 0x0015ABD0
    // stage 0 (COLOROP, ARG1, ARG2, ALPHAOP, ARG1, ARG2) = (2, 2, 0, 2, 2, 0), stage 1 (2, 2, 1, 2, 2, 1), stages 2-3
    // (1, 2, 1, 1, 2, 1), each through the cache (dirty 0x800)
    const u32 ops[4][6] = {{2, 2, 0, 2, 2, 0}, {2, 2, 1, 2, 2, 1}, {1, 2, 1, 1, 2, 1}, {1, 2, 1, 1, 2, 1}};
    const u32 which[6] = {TSS_COLOROP, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2};
    for (u32 s = 0; s < 4; ++s)
        for (u32 k = 0; k < 6; ++k) state_.SetStage(s, which[k], ops[s][k], 0x800);
    // per stage (H, decompile 0x0015AD..: L the order inside a stage): the texture 0, BumpEnv (0.5, 0, 0, 0.5, 1, 0),
    // TCI 0, ADDRESSU 1, MAG 1, MIN 1, MIP 0, TTF 0
    for (u32 s = 0; s < 4; ++s) {
        state_.SetTexture(s, nullptr);
        const u32 bumpState[6] = {TSS_BUMPENVMAT00, TSS_BUMPENVMAT01, TSS_BUMPENVMAT10, TSS_BUMPENVMAT11,
                                  TSS_BUMPENVLSCALE, TSS_BUMPENVLOFFSET};
        const u32 bumpValue[6] = {0x3F000000u, 0, 0, 0x3F000000u, 0x3F800000u, 0};
        for (u32 k = 0; k < 6; ++k)
            if (state_.tss[s][bumpState[k]][0] != bumpValue[k]) {
                state_.tss[s][bumpState[k]][0] = bumpValue[k];
                d3d_.SetTextureState_BumpEnv(s, bumpState[k], bumpValue[k]);
            }
        if (state_.tss[s][TSS_TEXCOORDINDEX][0] != 0) {
            state_.tss[s][TSS_TEXCOORDINDEX][0] = 0;
            d3d_.SetTextureState_TexCoordIndex(s, 0);
        }
        state_.SetStage(s, TSS_ADDRESSU, 1, 1u << s);
        state_.SetStage(s, TSS_MAGFILTER, 1, 1u << s);
        state_.SetStage(s, TSS_MINFILTER, 1, 1u << s);
        state_.SetStage(s, TSS_MIPFILTER, 0, 1u << s);
        state_.SetStage(s, TSS_TEXTURETRANSFORMFLAGS, 0, 0x400);
    }
    state_.SetDeferred(RS_WRAP0, 0, 0xF);
    state_.SetDeferred(RS_WRAP1, 0, 0xF);
    // the three scratch stages (types 0 / 1 / 2) into the scratch pass, applied (0x001620B0 / 0x00161ED0 /
    // 0x00161DF0 each)
    const u32 scratchOps[3][6] = {{2, 2, 0, 2, 2, 0}, {1, 2, 1, 1, 2, 1}, {1, 1, 0, 1, 1, 0}};
    for (u32 i = 0; i < 3; ++i) {
        scratch_[i] = std::make_unique<Stage>();
        Stage& st = *scratch_[i];
        st.index = i;
        st.colorOp = scratchOps[i][0], st.colorArg1 = scratchOps[i][1], st.colorArg2 = scratchOps[i][2];
        st.alphaOp = scratchOps[i][3], st.alphaArg1 = scratchOps[i][4], st.alphaArg2 = scratchOps[i][5];
        st.tci = 0, st.addressU = 1, st.addressV = 1;
        SetFilter(st, 0);
        st.ttf = 0, st.bumpOn = false;
        ApplyStage(st);                               // 0x001620B0, then 0x00161ED0(.., 1, 1) and 0x00161DF0
        const u32* t = kFilter[0];
        state_.SetStage(i, TSS_MAGFILTER, t[1], 1u << i);
        state_.SetStage(i, TSS_MINFILTER, t[0], 1u << i);
        state_.SetStage(i, TSS_MIPFILTER, t[2], 1u << i);
        state_.SetStage(i, TSS_ADDRESSU, st.addressU, 1u << i);
        state_.SetStage(i, TSS_ADDRESSV, (flags & 1) ? st.addressV : st.addressU, 1u << i);
    }
}

// ---- per frame ------------------------------------------------------------------------------------------------------
void NiXBoxTexturePipeline::SetCameraData(const ni::NiPoint3& dir, const ni::NiPoint3& up, const ni::NiPoint3& right,
                                          const ni::NiPoint3& loc) {   // orig: 0x00159F00
    dir_ = dir, up_ = up, right_ = right, loc_ = loc;
}

void NiXBoxTexturePipeline::BeginScene() {            // orig: 0x00159F80
    Obj().ResetEffects();                             // (the current pipeline's vf18, via 0x001624F0)
    lastEffects_ = nullptr;
}

void NiXBoxTexturePipeline::Begin(const ni::NiPropertyState& props) {   // orig: 0x0015AAF0
    // the renderer-specific property +0x18, its holder +0x18 of type 6 (vf04): the holder's pipeline object +4
    std::shared_ptr<NiXBoxTexturePipeline> want;
    if (const auto* rsp = dynamic_cast<const NiXBoxPipelineProperty*>(
            props.slot[ni::NiPropertyState::kRendererSpecific].Get()))
        want = rsp->pipeline;
    if (active_ != want) {                            // (NiPointer::Assign; or back to the default +0xC)
        active_ = std::move(want);
        Obj().ResetEffects();                         // vf18 0x001624B0
        lastEffects_ = nullptr;                       // mgr +0xB08 = 0
    }
    // (vf10 0x001624A0: no-op)
}

void NiXBoxTexturePipeline::ResetEffects() {          // orig: vf18 0x001624B0
    projLightCount_ = clippedLightCount_ = shadowCount_ = clippedShadowCount_ = 0;
    envOn_ = false, fogOn_ = false;
    effects_ = nullptr;
    effectStages_ = 0;
}

void NiXBoxTexturePipeline::ResetPasses() {           // orig: 0x00162900
    for (auto& p : passes_)
        if (p) p->next = 0, p->count = 0xFFFFFFFFu, p->textures = 0;
    currentPass_ = nullptr;
    passCount_ = 0;
}

// ---- the texture effects ---------------------------------------------------------------------------------------------
void NiXBoxTexturePipeline::UpdateEffects(const ni::NiDynamicEffectState* e) {   // orig: 0x00164290
    if (e == lastEffects_) return;
    ResetEffects();                                   // (vf18)
    lastEffects_ = e;
    effects_ = e;
    if (!e) return;
    for (const auto& t : e->projectedLights) {
        if (!t->clippingPlaneEnabled) {
            if (projLightCount_ < 8 && EffectStage(*t, projLights_[projLightCount_])) ++projLightCount_;
        } else if (clippedLightCount_ < 8 && EffectStage(*t, clippedLights_[clippedLightCount_])) {
            NotPorted("NiXBoxTexturePipeline: the projected light's clipper 0x00162B60");
            ++clippedLightCount_;
        }
    }
    for (const auto& t : e->projectedShadows) {
        if (!t->clippingPlaneEnabled) {
            if (shadowCount_ < 8 && EffectStage(*t, shadows_[shadowCount_])) ++shadowCount_;
        } else if (clippedShadowCount_ < 8 && EffectStage(*t, clippedShadows_[clippedShadowCount_])) {
            GenShadowClipper(*t);
            ++clippedShadowCount_;
        }
    }
    if (e->environmentMap) envOn_ = EffectStage(*e->environmentMap, envStage_);
    if (e->fogMap) fogOn_ = EffectStage(*e->fogMap, fogStage_);
}

bool NiXBoxTexturePipeline::EffectStage(ni::NiTextureEffect& e, Stage& st) {   // orig: 0x00163FA0
    if (effectStages_ > 7) return false;
    if (!e.switchOn || !e.sourceTextureObj) return false;
    // (the texture manager's vf10 type 0xE: a cube map: not used by this effect kind; L)
    st.tci = 1;
    st.texture = e.sourceTextureObj.Get();
    st.bumpOn = false;
    switch (e.coordGenType) {                         // +0x774 [+0x16C](worldProjection, its translate, stage)
    case 0: GenWorldParallel(st, e); break;
    case 2: GenSphereMap(st, e); break;
    default: NotPorted("NiXBoxTexturePipeline: the coordinate generators 0x00162EF0 / 0x001631F0 / 0x00163310"); break;
    }
    ++effectStages_;
    st.source = &e;
    st.lit = false;
    SetFilter(st, e.textureFiltering);                // +0x160
    SetClamp(st, e.textureClamping);                  // +0x164
    return true;
}

// P[k][c] (the original's 16-byte row k at +0x120, element c) = the port's worldProjection.m[c][k]; T = +0x150.
void NiXBoxTexturePipeline::GenWorldParallel(Stage& st, const ni::NiTextureEffect& e) {   // orig: 0x00162D20
    st.tci = 0x20000, st.ttf = 2;                     // CAMERASPACEPOSITION, COUNT2
    auto P = [&e](int k, int c) { return double(e.worldProjection.m[c][k]); };
    const ni::NiPoint3 &R = right_, &U = up_, &D = dir_, &L = loc_;
    const ni::NiPoint3& T = e.worldProjectionTranslate;
    auto& m = st.matrix.m;
    m[0][0] = F((double(R.x) * P(0, 0) + double(R.z) * P(2, 0)) + double(R.y) * P(1, 0));
    m[1][0] = F((double(U.z) * P(2, 0) + double(U.y) * P(1, 0)) + P(0, 0) * U.x);
    m[2][0] = F((P(1, 0) * D.y + P(0, 0) * D.x) + P(2, 0) * D.z);
    m[0][1] = F((double(R.z) * P(2, 1) + double(R.y) * P(1, 1)) + P(0, 1) * R.x);
    m[1][1] = F((double(U.z) * P(2, 1) + double(U.y) * P(1, 1)) + P(0, 1) * U.x);
    m[2][1] = F((P(0, 1) * D.x + double(D.z) * P(2, 1)) + double(D.y) * P(1, 1));
    m[0][2] = F((double(R.y) * P(1, 2) + double(R.x) * P(0, 2)) + double(R.z) * P(2, 2));
    m[1][2] = F((double(U.y) * P(1, 2) + double(U.x) * P(0, 2)) + double(U.z) * P(2, 2));
    m[2][2] = F((double(D.z) * P(2, 2) + double(D.y) * P(1, 2)) + double(D.x) * P(0, 2));
    m[3][0] = F(((double(L.x) * P(0, 0) + P(1, 0) * L.y) + P(2, 0) * L.z) + T.x);
    m[3][1] = F(((double(L.z) * P(2, 1) + double(L.x) * P(0, 1)) + double(L.y) * P(1, 1)) + T.y);
    m[3][2] = F(((double(L.x) * P(0, 2) + P(1, 2) * L.y) + double(L.z) * P(2, 2)) + T.z);
}

void NiXBoxTexturePipeline::GenSphereMap(Stage& st, const ni::NiTextureEffect& e) {   // orig: 0x001630D0
    st.tci = 0x10000, st.ttf = 2;                     // CAMERASPACENORMAL, COUNT2
    auto P = [&e](int k, int c) { return double(e.worldProjection.m[c][k]); };
    const ni::NiPoint3 &R = right_, &U = up_, &D = dir_;
    const ni::NiPoint3& T = e.worldProjectionTranslate;
    auto& m = st.matrix.m;
    m[0][0] = F((double(R.z) * P(2, 0) + double(R.x) * P(0, 0)) + double(R.y) * P(1, 0));
    m[1][0] = F((double(U.z) * P(2, 0) + double(U.y) * P(1, 0)) + P(0, 0) * U.x);
    m[2][0] = F((double(D.x) * P(0, 0) + P(2, 0) * D.z) + P(1, 0) * D.y);
    m[0][1] = F((double(R.x) * P(0, 1) + double(R.y) * P(1, 1)) + P(2, 1) * R.z);
    m[1][1] = F((double(U.y) * P(1, 1) + P(0, 1) * U.x) + P(2, 1) * U.z);
    m[2][1] = F((P(2, 1) * D.z + double(D.x) * P(0, 1)) + double(D.y) * P(1, 1));
    m[0][2] = F((double(R.z) * P(2, 2) + P(0, 2) * R.x) + double(R.y) * P(1, 2));
    m[1][2] = F((P(0, 2) * U.x + double(U.z) * P(2, 2)) + double(U.y) * P(1, 2));
    m[2][2] = F((double(D.y) * P(1, 2) + double(D.x) * P(0, 2)) + double(D.z) * P(2, 2));
    m[3][0] = T.x, m[3][1] = T.y, m[3][2] = T.z;      // copied
}

void NiXBoxTexturePipeline::GenShadowClipper(const ni::NiTextureEffect& e) {   // orig: 0x00162C40
    Stage& st = shadowClippers_[clippedShadowCount_];
    const double nx = -double(e.worldClippingPlane.normal.x), ny = -double(e.worldClippingPlane.normal.y);
    const double nz = -double(e.worldClippingPlane.normal.z), d = e.worldClippingPlane.constant;
    const ni::NiPoint3 &R = right_, &U = up_, &D = dir_, &L = loc_;
    const f32 fnx = F(nx), fny = F(ny), fnz = F(nz);  // (FCHS on the loaded floats: exact)
    (void)fnx, (void)fny, (void)fnz;
    auto& m = st.matrix.m;                            // column 0 only (TTF 2: v = 0)
    m[0][0] = F((double(R.z) * nz + double(R.y) * ny) + nx * R.x);
    m[1][0] = F((double(U.z) * nz + double(U.y) * ny) + double(U.x) * nx);
    m[2][0] = F((double(D.z) * nz + double(D.y) * ny) + double(D.x) * nx);
    m[3][0] = F((((nz * L.z) + ny * L.y) + nx * L.x + 0.5) - (-d));   // FADD [0x00293AA0] = 0.5f; FCHS; FSUBP
    ++effectStages_;
    st.source = &e;
}

// ---- passes ---------------------------------------------------------------------------------------------------------
NiXBoxTexturePipeline::Stage* NiXBoxTexturePipeline::PoolStage(ni::NiObject* texture, u32 clamp, u32 filter,
                                                              u32 type, u32 set, u32 sets, const void* source) {
    // orig: 0x00163DF0
    if (!texture) return nullptr;
    Stage& st = pool_[poolNext_ < 8 ? poolNext_ : 7];
    ++poolNext_;
    if (sets == 0) return nullptr;
    st.texture = texture;
    SetClamp(st, clamp);
    SetFilter(st, filter);
    st.type = type;
    st.tci = set < sets ? set : sets - 1;
    st.source = static_cast<const ni::NiObject*>(source);
    st.ttf = 0, st.lit = false, st.bumpOn = false;
    return &st;
}

NiXBoxTexturePipeline::Pass* NiXBoxTexturePipeline::CurrentPass() {
    if (!currentPass_) {
        if (passes_.size() <= passCount_) passes_.resize(passCount_ + 1);
        if (!passes_[passCount_]) passes_[passCount_] = std::make_unique<Pass>();
        currentPass_ = passes_[passCount_].get();
    }
    return currentPass_;
}

void NiXBoxTexturePipeline::Room(const Pass& p, u32& stages, u32& textures) {   // orig: 0x0015FE60
    stages = p.count == 0xFFFFFFFFu ? 4u : 4u - p.count;   // DAT_002D8670 / DAT_002D8674 = 4
    textures = 4u - p.textures;
}

NiXBoxTexturePipeline::Pass* NiXBoxTexturePipeline::PassWithRoom(u32 needStages, u32 needTextures) {
    // orig: 0x001625B0 (stages, textures)
    Pass* p = CurrentPass();
    u32 st = 0, tx = 0;
    Room(*p, st, tx);
    if (st < needStages || tx < needTextures) {
        ++passCount_;
        if (passes_.size() <= passCount_) passes_.resize(passCount_ + 1);
        if (!passes_[passCount_]) passes_[passCount_] = std::make_unique<Pass>();
        currentPass_ = passes_[passCount_].get();
        p = currentPass_;
    }
    return p;
}

bool NiXBoxTexturePipeline::NeedsNewPass(u32 type, u32 stages, u32 textures) {   // orig: 0x00162700
    if (!currentPass_) {
        CurrentPass();
        return false;
    }
    if (passCount_ == 0 || (currentPass_->stages[0] && currentPass_->stages[0]->type == type)) {
        u32 st = 0, tx = 0;
        Room(*currentPass_, st, tx);
        if (stages <= st && textures <= tx) return false;
    }
    return true;
}

void NiXBoxTexturePipeline::NewPass() {               // orig: 0x001627D0
    Pass* p = CurrentPass();
    if (p->count == 0xFFFFFFFFu) return;              // (an unused pass is reused)
    ++passCount_;
    if (passes_.size() <= passCount_) passes_.resize(passCount_ + 1);
    if (!passes_[passCount_]) passes_[passCount_] = std::make_unique<Pass>();
    currentPass_ = passes_[passCount_].get();
}

bool NiXBoxTexturePipeline::AddStage(Pass& p, Stage* st) {   // orig: 0x00160100
    if (!st || p.next >= 8) return false;
    st->index = p.next;
    p.stages[p.next] = st;
    if (p.next != 0) {
        st->colorArg2 = 1;                            // CURRENT
        st->colorArg1 = st->texture ? 2u : 0u;
    }
    ++p.next;
    if (p.count < p.next || p.count == 0xFFFFFFFFu) p.count = p.next;
    if (st->texture) ++p.textures;
    return true;
}

u32 NiXBoxTexturePipeline::AddStages(Pass& p, Stage* a, Stage* b, bool needBoth) {   // orig: 0x00160170
    const u32 used = p.textures;
    if (4 <= used) return 0;
    auto put = [&p](Stage* s) {
        s->index = p.next;
        p.stages[p.next] = s;
        ++p.next;
        if (p.count < p.next || p.count == 0xFFFFFFFFu) p.count = p.next;
        if (s->texture) ++p.textures;
    };
    if (!b) {
        if (!a) return 0;
        put(a);
        return 1;
    }
    if (!a) {
        if (needBoth) return 0;
        put(b);
        return 2;
    }
    put(a);
    if (used < 3) {
        put(b);
        return 3;
    }
    return 1;
}

void NiXBoxTexturePipeline::SetPassAlpha(Pass& p) {   // orig: 0x00163430 (mgr +0x2E8 [the first stage's type])
    const Stage* first = p.stages[0];
    const u32 t = first ? first->type : 9;
    p.alpha = passAlpha_[t < 10 ? t : 9];
}

bool NiXBoxTexturePipeline::AddToPass(Stage* st, u32 stages, u32 textures) {   // orig: 0x00163EB0
    if (!st) return false;
    Pass* p = PassWithRoom(stages, textures);
    if (!p) return false;
    const bool ok = AddStage(*p, st);
    if (passCount_ != 0 && p->count != 0xFFFFFFFFu) SetPassAlpha(*p);
    return ok;
}

u32 NiXBoxTexturePipeline::AddPairToPass(Stage* a, Stage* b, bool needBoth, u32 stages, u32 textures) {
    // orig: 0x00163F30
    Pass* p = PassWithRoom(stages, textures);
    if (!p) return 0;
    const u32 r = AddStages(*p, a, b, needBoth);
    if (passCount_ != 0 && p->count != 0xFFFFFFFFu) SetPassAlpha(*p);
    return r;
}

void NiXBoxTexturePipeline::SetOps(Stage* st, bool later) {   // orig: 0x00162A50
    if (!st) return;
    const u32 t = st->type < 10 ? st->type : 9;
    if (st->index == 0 && !later) {
        const u32* o = firstOps_[t];
        st->colorOp = o[0], st->colorArg1 = o[1], st->colorArg2 = o[2];
        st->alphaOp = o[3], st->alphaArg1 = o[4], st->alphaArg2 = o[5];
        if (!st->lit) {                               // (effects: SELECTARG1 texture)
            st->colorOp = 2, st->colorArg1 = 2, st->alphaOp = 2, st->alphaArg1 = 2;
            return;
        }
        if (!st->texture) {                           // SELECTARG2 diffuse
            st->colorOp = 3, st->alphaOp = 3, st->colorArg2 = 0, st->alphaArg2 = 0;
        }
        return;
    }
    const u32* o = laterOps_[t];
    st->colorOp = o[0], st->colorArg1 = o[1], st->colorArg2 = o[2];
    st->alphaOp = o[3], st->alphaArg1 = o[4], st->alphaArg2 = o[5];
}

u32 NiXBoxTexturePipeline::BuildPasses(const ni::NiPropertyState& props, const ni::NiDynamicEffectState* effects,
                                       u32 sets) {   // orig: 0x00159EE0 (mgr +8 vf08)
    NiXBoxTexturePipeline& o = Obj();
    return o.screenTexture_ ? o.BuildScreenTexturePasses(props, sets) : o.BuildDefaultPasses(props, effects, sets);
}

// orig: 0x00160300 (vtable 0x002ABE50 vf08): 0x001643F0's simple path without the effects; no world transform for
// the draw (0x800000)
u32 NiXBoxTexturePipeline::BuildScreenTexturePasses(const ni::NiPropertyState& props, u32 sets) {
    ResetPasses();                                    // 0x00162900
    poolNext_ = 0, current_ = 0, builtPasses_ = 0;    // +0x6A0 / +0x3C / +0x34
    const auto* tex = static_cast<const ni::NiTexturingProperty*>(props.slot[ni::NiPropertyState::kTexturing].Get());
    static const ni::NiTexturingProperty kNone;
    if (!tex) tex = &kNone;                           // (the original reads props +0x28 without a test)
    const auto* m0 = tex->HasMap(0) ? &tex->maps[0] : nullptr;   // (maps +0x24 [0])
    applyMode_ = tex->applyMode;                      // +0x2F7C
    const bool lit = tex->applyMode != 0;
    // 0x00163DF0(texture, clamp (3 without a map), filter (0), type 2, set 0, the texture sets, the texturing)
    Stage* s0 = PoolStage(m0 ? m0->textureObj.Get() : nullptr, m0 ? m0->clampMode : 3, m0 ? m0->filterMode : 0, 2, 0,
                          sets, tex);
    // (+0x38 = 0: L, its meaning)
    if (!AddToPass(s0, 1, 1)) {                       // 0x00163EB0
        empty_.lit = lit;                             // +0x734
        AddToPass(&empty_, 1, 1);
        s0 = &empty_;
    } else {
        s0->lit = lit;                                // +0x90
    }
    SetOps(s0, false);                                // 0x00162A50
    current_ = 0;                                     // +0x3C = +8 = 0
    builtPasses_ = passCount_;                        // +0x34 = +0xC
    return 0x800000u;
}

u32 NiXBoxTexturePipeline::BuildDefaultPasses(const ni::NiPropertyState& props,
                                              const ni::NiDynamicEffectState* effects, u32 sets) {   // orig: 0x001643F0
    ResetPasses();
    bool flag4 = true;
    UpdateEffects(effects);
    poolNext_ = 0, current_ = 0, builtPasses_ = 0;
    const auto* tex = static_cast<const ni::NiTexturingProperty*>(props.slot[ni::NiPropertyState::kTexturing].Get());
    static const ni::NiTexturingProperty kNone;
    if (!tex) tex = &kNone;
    auto map = [tex](u32 slot) -> const ni::NiTexturingProperty::Map* {
        return tex->HasMap(slot) ? &tex->maps[slot] : nullptr;
    };
    const auto* baseMap = map(0);
    applyMode_ = tex->applyMode;
    const bool lit = tex->applyMode != 0;
    Stage* s0 = PoolStage(baseMap ? baseMap->textureObj.Get() : nullptr, baseMap ? baseMap->clampMode : 3,
                          baseMap ? baseMap->filterMode : 0, 2, baseMap ? baseMap->textureIndex : 0, sets, baseMap);
    if (effectStages_ == 0 && !tex->hasMultiTexture) {   // the simple case
        if (!AddToPass(s0, 1, 1)) {
            s0 = &empty_;
            AddToPass(s0, 1, 1);
        } else if (s0) {
            s0->lit = lit;
        }
        SetOps(s0, false);
    } else {
        const ni::NiTexturingProperty::Map* glossMap = nullptr;
        Stage *dark = nullptr, *detail = nullptr, *glow = nullptr;
        u32 decals = 0;
        if (sets != 0) {
            decals = tex->maps.size() > 6 ? u32(tex->maps.size()) - 6 : 0;   // (+0x2C - 6)
            if (decals == 1 && !map(6)) decals = 0;
            glossMap = map(3);
            if (const auto* m = map(2))
                detail = PoolStage(m->textureObj.Get(), m->clampMode, m->filterMode, 3, m->textureIndex, sets, m);
            if (const auto* m = map(4))
                glow = PoolStage(m->textureObj.Get(), m->clampMode, m->filterMode, 0, m->textureIndex, sets, m);
            if (const auto* m = map(1))
                dark = PoolStage(m->textureObj.Get(), m->clampMode, m->filterMode, 2, m->textureIndex, sets, m);
        }
        Stage* tail = dark;                           // (local_30: the dark map's stage, applied last)
        if (projLightCount_ == 0 && clippedLightCount_ == 0) {
            if (AddPairToPass(s0, detail, true, 2, 2) == 0) {
                if (!dark) {
                    AddToPass(&empty_, 1, 1);
                    SetOps(&empty_, false);
                } else {
                    dark->lit = lit;
                }
            } else {
                s0->lit = lit;
                SetOps(s0, false);
                SetOps(detail, false);
            }
            AddToPass(tail, 1, 1);
        } else {
            NotPorted("NiXBoxTexturePipeline: the projected lights' passes (0x001643F0..)");
            AddToPass(s0, 1, 1);
            if (s0) s0->lit = lit, SetOps(s0, false);
            flag4 = false;
        }
        SetOps(tail, false);
        for (u32 i = 0; i < decals; ++i) {            // each decal its own stage
            const auto* m = map(6 + i);
            if (!m) continue;
            Stage* d = PoolStage(m->textureObj.Get(), m->clampMode, m->filterMode, 1, m->textureIndex, sets, m);
            if (!d) continue;
            d->lit = lit;
            NewPass();
            AddToPass(d, 1, 1);
            SetOps(d, false);
        }
        // the projected shadows, only without blending (bit 0) and alpha test (bit 9) of the property state's alpha
        const auto* alpha = static_cast<const ni::NiAlphaProperty*>(props.slot[ni::NiPropertyState::kAlpha].Get());
        const u32 af = alpha ? alpha->flags : 0;
        u32 unclipped = shadowCount_;
        if ((unclipped != 0 || clippedShadowCount_ != 0) && !((af >> 9) & 1) && !(af & 1)) {
            const u32 room = 4;                       // max(DAT_002D8674, DAT_002D8670)
            bool none = clippedShadowCount_ == 0;
            for (u32 k = 0; k < clippedShadowCount_; ++k) {
                flag4 = false;
                NewPass();
                u32 r = 0;
                Pass* p = PassWithRoom(1, 1);
                if (p) {
                    r = AddStages(*p, &clippedShadows_[k], &shadowClippers_[k], true);
                    if (passCount_ != 0 && p->count != 0xFFFFFFFFu) SetPassAlpha(*p);
                }
                SetOps(&clippedShadows_[k], false);
                if (r & 2) SetOps(&shadowClippers_[k], false);
                for (u32 left = room; left != 0 && unclipped != 0; --left) {
                    Stage& u = shadows_[--unclipped];
                    u.type = 2;
                    Pass* q = PassWithRoom(1, 1);
                    if (q && AddStage(*q, &u) && passCount_ != 0 && q->count != 0xFFFFFFFFu) SetPassAlpha(*q);
                    SetOps(&u, false);
                }
                none = clippedShadowCount_ == 0;
            }
            if (none && NeedsNewPass(2, 1, 1)) NewPass();
            while (unclipped != 0) {
                Stage& u = shadows_[--unclipped];
                u.type = 2;
                Pass* q = PassWithRoom(1, 1);
                if (q && AddStage(*q, &u) && passCount_ != 0 && q->count != 0xFFFFFFFFu) SetPassAlpha(*q);
                SetOps(&u, false);
            }
        }
        if (envOn_) {                                 // the environment map (with the gloss map)
            const auto* bumpMap = map(5);
            Stage* bump = nullptr;
            u32 need = glossMap ? 2 : 1;
            bool newPass = false;
            if (bumpMap)
                bump = PoolStage(bumpMap->textureObj.Get(), bumpMap->clampMode, bumpMap->filterMode, 8,
                                 bumpMap->textureIndex, sets, bumpMap);
            if (bump) {
                NotPorted("NiXBoxTexturePipeline: the bump-env stage 0x001629A0");
                if (!glossMap) need = 2;
            }
            if (!bump || !glossMap || true) newPass = NeedsNewPass(0, need, need);
            if (newPass || (glossMap && bump)) NewPass();
            bool done = false;
            if (glossMap) {
                Stage* g = PoolStage(glossMap->textureObj.Get(), glossMap->clampMode, glossMap->filterMode, 2,
                                     glossMap->textureIndex, sets, glossMap);
                if (g) g->lit = lit;
                Pass* p = PassWithRoom(2, 2);
                if (p) {
                    AddStages(*p, &envStage_, g, true);
                    if (passCount_ != 0 && p->count != 0xFFFFFFFFu) SetPassAlpha(*p);
                }
                SetOps(&envStage_, false);
                SetOps(g, false);
                done = true;
            }
            if (!done) {
                AddToPass(&envStage_, 1, 1);
                SetOps(&envStage_, false);
            }
        }
        if (glow) {                                   // the glow map
            if (NeedsNewPass(glow->type, 1, 1)) NewPass();
            AddToPass(glow, 1, 1);
            SetOps(glow, false);
        }
        if (fogOn_) {                                 // the fog map: a pass of its own
            if (current_ != 0) NewPass();
            AddToPass(&fogStage_, 1, 1);
            SetOps(&fogStage_, false);
        }
    }
    builtPasses_ = passCount_;                        // (+0x34 = +0xC: the index of the last pass used, L)
    current_ = 0;
    const u32 count = passCount_ + 1;                 // (the passes in use: up to the current one)
    return (count > 1 && flag4) ? 0x80000000u : 0u;
}

u32 NiXBoxTexturePipeline::NextPass() {               // orig: 0x00159EF0 -> mgr +8 vf0C 0x00162940
    if (&Obj() != this) return Obj().NextPass();
    if (current_ >= passes_.size() || !passes_[current_]) return 0;
    Pass* p = passes_[current_].get();
    currentPass_ = p;
    if (current_ != 0) state_.ApplyAlpha(p->alpha.Get());   // 0x00159300(pass +0x38)
    u32 f = ApplyPass(*p);                            // vf04 0x00160070
    const u32 left = passCount_ - current_;
    if (left != 0) f |= left;
    ++current_;
    return f;
}

u32 NiXBoxTexturePipeline::ApplyPass(Pass& p) {       // orig: 0x00160070
    if (p.count == 0xFFFFFFFFu) return 0x40000000u;
    u32 n = 0;
    for (; n < p.count && n < 8; ++n)
        if (p.stages[n]) ApplyStage(*p.stages[n]);
    if (n <= 3) DisableStage(n);                      // 0x00161D60
    for (u32 s = n; s < 4; ++s)                       // the stages after: TEXCOORDINDEX back to the stage number
        if (state_.tss[s][TSS_TEXCOORDINDEX][0] != s) {
            state_.tss[s][TSS_TEXCOORDINDEX][0] = s;
            d3d_.SetTextureState_TexCoordIndex(s, s);   // @0x001600DD
        }
    return 0;
}

void NiXBoxTexturePipeline::DisableStage(u32 s) {     // orig: 0x00161D60
    state_.SetTexture(s, nullptr);                    // @0x00161D8E
    state_.SetStage(s, TSS_COLOROP, 1, 0x800);
    state_.SetStage(s, TSS_ALPHAOP, 1, 0x800);
}

void NiXBoxTexturePipeline::ApplyStage(Stage& st) {   // orig: 0x001620B0
    const u32 s = st.index & 3;
    if (!st.texture) {
        state_.SetTexture(s, nullptr);                // @0x001621EF
    } else {
        Resource* d3d = nullptr;
        bool set = false, mips = false;
        if (!textures_.Prepare(st.texture.Get(), d3d, set, mips)) d3d = nullptr;
        if (set) {
            state_.SetTexture(s, d3d);                // @0x00162151 (a palette: NOT PORTED, none in level1-1)
        }
        if (d3d) {
            // 0x00162000: the matrix with TTF (always written), TTF, TCI (a call)
            if (st.ttf) d3d_.SetTransform(TS_TEXTURE0 + s, st.matrix);   // @0x00162025
            state_.SetStage(s, TSS_TEXTURETRANSFORMFLAGS, st.ttf, 0x400);
            if (state_.tss[s][TSS_TEXCOORDINDEX][0] != st.tci) {
                state_.tss[s][TSS_TEXCOORDINDEX][0] = st.tci;
                d3d_.SetTextureState_TexCoordIndex(s, st.tci);   // @0x00162080
            }
            // 0x00161ED0 (stage, mips, 1): the filter adjusted to the texture
            u32 f = st.filter;
            if (!mips) {
                if (f == 3 || f == 4) f = 0;
                else if (f == 5 || f == 2) f = 1;
            }
            const u32* t = kFilter[f < 6 ? f : 0];
            state_.SetStage(s, TSS_MAGFILTER, t[1], 1u << s);
            state_.SetStage(s, TSS_MINFILTER, t[0], 1u << s);
            state_.SetStage(s, TSS_MIPFILTER, t[2], 1u << s);
            // 0x00161DF0: ADDRESSU, ADDRESSV (= U unless the manager's +0xFC bit 0)
            state_.SetStage(s, TSS_ADDRESSU, st.addressU, 1u << s);
            state_.SetStage(s, TSS_ADDRESSV, (flags & 1) ? st.addressV : st.addressU, 1u << s);
        }
    }
    state_.SetStage(s, TSS_COLOROP, st.colorOp, 0x800);
    state_.SetStage(s, TSS_COLORARG1, st.colorArg1, 0x800);
    state_.SetStage(s, TSS_COLORARG2, st.colorArg2, 0x800);
    state_.SetStage(s, TSS_ALPHAOP, st.alphaOp, 0x800);
    state_.SetStage(s, TSS_ALPHAARG1, st.alphaArg1, 0x800);
    state_.SetStage(s, TSS_ALPHAARG2, st.alphaArg2, 0x800);
    state_.SetComplex(RS_TEXTUREFACTOR, st.textureFactor);   // @0x00162346
    if (st.bumpOn) {
        const u32 bs[6] = {TSS_BUMPENVMAT00, TSS_BUMPENVMAT01, TSS_BUMPENVMAT11, TSS_BUMPENVMAT10,
                           TSS_BUMPENVLSCALE, TSS_BUMPENVLOFFSET};
        for (u32 k = 0; k < 6; ++k)
            if (state_.tss[s][bs[k]][0] != st.bump[k]) {
                state_.tss[s][bs[k]][0] = st.bump[k];
                d3d_.SetTextureState_BumpEnv(s, bs[k], st.bump[k]);
            }
    }
}

void NiXBoxTexturePipeline::ApplyPixelShader() {      // orig: 0x00163D60 (no pass has a pixel shader object)
    if (&Obj() != this) return Obj().ApplyPixelShader();   // (0x00159FA0: mgr +8)
    state_.SetPixelShader(0);
}

void NiXBoxTexturePipeline::ApplyVertexShader(u32 fvf) {   // orig: 0x00163DA0 (no pass vertex shader: the FVF)
    if (&Obj() != this) return Obj().ApplyVertexShader(fvf);   // (0x00159FB0: mgr +8)
    if (fvf) state_.SetVertexShader(fvf);
}

}  // namespace fut::render::nixbox

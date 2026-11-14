// SDL3 GPU backend (Metal on macOS); see SdlGpuDevice.h.
#include "render/xd3d/SdlGpuDevice.h"

#include "engine/ni/NiObjects.h"
#include "render/TextureDecode.h"
#include "render/xd3d/SdlGpuShaders.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fut::render::xd3d {
namespace {

void NotPorted(const char* what) {
    static std::set<std::string> seen;
    if (seen.insert(what).second) std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

struct LightCB {
    f32 diffuse[4], specular[4], ambient[4], pos[4], dir[4], att[4], spot[4];
};
struct DrawCB {
    D3DMATRIX world, view, proj, texm[4];
    f32 vp[4], ssOffset[4];
    f32 matDiffuse[4], matAmbient[4], matSpecular[4], matEmissive[4];
    f32 ambient[4], misc0[4], misc1[4], misc2[4], misc3[4], fogParams[4], fogColor[4], tfactor[4], alphaTest[4];
    s32 tci[4][4], colorOp[4][4], alphaOp[4][4], stageMisc[4][4], lightCount[4];
    LightCB lights[8];
    f32 texScale[4][4];
};
struct ClearCB {
    f32 rect[4], color[4], z[4];
};

struct CVertex {
    f32 pos[4], nrm[3], d0[4], d1[4], uv[4][4];
};

void Color(u32 argb, f32 out[4]) {
    out[0] = f32((argb >> 16) & 0xFF) / 255.0f;
    out[1] = f32((argb >> 8) & 0xFF) / 255.0f;
    out[2] = f32(argb & 0xFF) / 255.0f;
    out[3] = f32(argb >> 24) / 255.0f;
}

f32 AsFloat(u32 v) {
    f32 f;
    std::memcpy(&f, &v, 4);
    return f;
}

SDL_GPUBlendFactor Blend(u32 v) {
    switch (v) {
    case 0: return SDL_GPU_BLENDFACTOR_ZERO;
    case 1: return SDL_GPU_BLENDFACTOR_ONE;
    case 0x300: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 0x301: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 0x303: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case 0x305: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return SDL_GPU_BLENDFACTOR_DST_COLOR;
    case 0x307: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: case 0x8003: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
    case 0x8002: case 0x8004: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return SDL_GPU_BLENDFACTOR_ONE;
    }
}

SDL_GPUBlendFactor AlphaBlend(SDL_GPUBlendFactor b) {
    switch (b) {
    case SDL_GPU_BLENDFACTOR_SRC_COLOR: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case SDL_GPU_BLENDFACTOR_DST_COLOR: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    case SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    default: return b;
    }
}

SDL_GPUBlendOp BlendOp(u32 v) {
    switch (v) {
    case 0x800A: return SDL_GPU_BLENDOP_SUBTRACT;
    case 0x800B: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
    case 0x8007: return SDL_GPU_BLENDOP_MIN;
    case 0x8008: return SDL_GPU_BLENDOP_MAX;
    default: return SDL_GPU_BLENDOP_ADD;
    }
}

SDL_GPUCompareOp Compare(u32 v) {
    static constexpr SDL_GPUCompareOp m[8] = {
        SDL_GPU_COMPAREOP_NEVER,         SDL_GPU_COMPAREOP_LESS,
        SDL_GPU_COMPAREOP_EQUAL,         SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
        SDL_GPU_COMPAREOP_GREATER,       SDL_GPU_COMPAREOP_NOT_EQUAL,
        SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS,
    };
    return m[(v - 0x200) & 7];
}

SDL_GPUStencilOp StencilOp(u32 v) {
    switch (v) {
    case 0: return SDL_GPU_STENCILOP_ZERO;
    case 0x1E01: return SDL_GPU_STENCILOP_REPLACE;
    case 0x1E02: return SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP;
    case 0x1E03: return SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP;
    case 0x150A: return SDL_GPU_STENCILOP_INVERT;
    case 0x8507: return SDL_GPU_STENCILOP_INCREMENT_AND_WRAP;
    case 0x8508: return SDL_GPU_STENCILOP_DECREMENT_AND_WRAP;
    default: return SDL_GPU_STENCILOP_KEEP;
    }
}

SDL_GPUSamplerAddressMode Address(u32 v) {
    switch (v) {
    case 2: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case 3: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    case 4:
        NotPorted("SdlGpuDevice: sampler border address mode (clamped to edge)");
        return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    }
}

}  // namespace

struct SdlGpuDevice::Impl {
    SDL_GPUDevice* gpu = nullptr;
    SDL_Window* window = nullptr;
    SDL_GPUTextureFormat colorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

    SDL_GPUShader* vs = nullptr;
    SDL_GPUShader* ps = nullptr;
    SDL_GPUShader* clearVs = nullptr;
    SDL_GPUShader* clearPs = nullptr;
    SDL_GPUShader* blitVs = nullptr;
    SDL_GPUShader* blitPs = nullptr;

    SDL_GPUBuffer* vb = nullptr;
    SDL_GPUBuffer* ib = nullptr;
    SDL_GPUTransferBuffer* transferBuf = nullptr;
    u32 vbCapacity = 0;
    u32 ibCapacity = 0;
    u32 transferCapacity = 0;

    SDL_GPUTransferBuffer* texTransferBuf = nullptr;
    u32 texTransferCapacity = 0;

    u32 windowWidth = 0;
    u32 windowHeight = 0;
    SDL_GPUSampler* blitSamplers[2] = {};
    SDL_GPUSampler* defaultSampler = nullptr;
    Filter filter = Filter::Linear;

    double scaleX = 1.0, scaleY = 1.0;
    bool autoScale = true;
    double fixedScale = 1.0;

    struct Target {
        SDL_GPUTexture* tex = nullptr;
        u32 width = 0, height = 0;
        u32 physicalWidth = 0, physicalHeight = 0;
        bool depth = false;
    };
    std::unordered_map<const Resource*, Target> targets;

    struct Tex {
        SDL_GPUTexture* texture = nullptr;
        u32 revision = 0;
    };
    std::unordered_map<const Resource*, Tex> textures;
    SDL_GPUTexture* white = nullptr;

    std::map<std::vector<u32>, SDL_GPUGraphicsPipeline*> pipelines;
    std::map<u32, SDL_GPUGraphicsPipeline*> clearPipelines;
    std::map<u32, SDL_GPUGraphicsPipeline*> blitPipelines;
    std::map<std::vector<u32>, SDL_GPUSampler*> samplers;

    struct DrawItem {
        bool isClearQuad = false;
        // Draw fields
        SDL_GPUGraphicsPipeline* pipeline = nullptr;
        SDL_GPUViewport viewport{};
        SDL_FColor blendColor{};
        u8 stencilRef = 0;
        u32 vertByteOffset = 0;
        u32 idxByteOffset = 0;
        u32 idxCount = 0;
        DrawCB drawCB{};
        SDL_GPUTextureSamplerBinding bindings[4] = {};
        // ClearQuad fields
        ClearCB clearCB{};
    };

    struct PassRecord {
        Target* ct = nullptr;
        Target* dt = nullptr;
        const Resource* ctKey = nullptr;
        SDL_GPULoadOp colorLoadOp = SDL_GPU_LOADOP_LOAD;
        SDL_FColor clearColor{};
        SDL_GPULoadOp depthLoadOp = SDL_GPU_LOADOP_LOAD;
        SDL_GPULoadOp stencilLoadOp = SDL_GPU_LOADOP_LOAD;
        f32 clearDepth = 1.0f;
        u8 clearStencil = 0;
        std::vector<DrawItem> items;
    };

    std::vector<PassRecord> passes;
    std::vector<CVertex> frameVerts;
    std::vector<u32> frameIndices;
    std::unordered_set<const Resource*> pendingTargetWrites;
    std::unordered_set<const Resource*> pendingTexUses;
    std::vector<CVertex> scratchVerts;
    std::vector<u32> scratchIdx;

    static u32 Physical(u32 logical, double scale) { return std::max(u32(std::lround(logical * scale)), 1u); }
    bool MakeTarget(const Resource* key, bool depth, Target& t);
    void ApplyScale(double sx, double sy);
    Target* ColorTarget(Resource* r);
    Target* DepthTarget(Resource* r);
    const Resource* ColorTargetKey(Resource* r) const { return r ? (r->parent ? r->parent : r) : nullptr; }

    SDL_GPUShader* LoadShader(const char* entry, SDL_GPUShaderStage stage, u32 numSamplers, u32 numUniforms,
                              std::string& error);
    SDL_GPUGraphicsPipeline* GetDrawPipeline(const std::vector<u32>& key, SDL_GPUPrimitiveType topo, bool hasDepth,
                                             const u32* rs);
    SDL_GPUGraphicsPipeline* GetClearPipeline(bool hasDepth, bool writeColor, bool writeDepth, bool writeStencil);
    SDL_GPUGraphicsPipeline* GetBlitPipeline(SDL_GPUTextureFormat fmt);
    SDL_GPUSampler* GetSampler(const u32* ts);

    bool UploadTextureData(SDL_GPUTexture* tex, u32 width, u32 height, u32 level, const void* data, u32 bytes);
    SDL_GPUTexture* Texture(Resource* r);
    SDL_GPUTexture* Yuy2Texture(Resource& r);
    bool EnsureFrameBuffers(u32 vertBytes, u32 idxBytes);
    PassRecord& ActivePass(Target* ct, Target* dt, const Resource* ctKey);
    void Flush();
    void Destroy();
};

SDL_GPUShader* SdlGpuDevice::Impl::LoadShader(const char* entry, SDL_GPUShaderStage stage, u32 numSamplers,
                                              u32 numUniforms, std::string& error) {
    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(gpu);
    if (formats & SDL_GPU_SHADERFORMAT_MSL) {
        SDL_GPUShaderCreateInfo info = {};
        info.code = reinterpret_cast<const Uint8*>(sdl_shaders::kMslShaderSource);
        info.code_size = std::strlen(sdl_shaders::kMslShaderSource);
        info.entrypoint = entry;
        info.format = SDL_GPU_SHADERFORMAT_MSL;
        info.stage = stage;
        info.num_samplers = numSamplers;
        info.num_uniform_buffers = numUniforms;
        SDL_GPUShader* sh = SDL_CreateGPUShader(gpu, &info);
        if (!sh) error = std::string("SDL_CreateGPUShader(") + entry + ") failed: " + SDL_GetError();
        return sh;
    }
    error = "SDL_GPU device does not support MSL shaders on this platform";
    return nullptr;
}

bool SdlGpuDevice::Impl::UploadTextureData(SDL_GPUTexture* tex, u32 width, u32 height, u32 level, const void* data,
                                           u32 bytes) {
    if (!texTransferBuf || texTransferCapacity < bytes) {
        if (texTransferBuf) SDL_ReleaseGPUTransferBuffer(gpu, texTransferBuf);
        texTransferCapacity = std::max(bytes, std::max(texTransferCapacity * 2, 256u * 1024u));
        SDL_GPUTransferBufferCreateInfo tbi = {};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = texTransferCapacity;
        texTransferBuf = SDL_CreateGPUTransferBuffer(gpu, &tbi);
        if (!texTransferBuf) return false;
    }
    void* mapped = SDL_MapGPUTransferBuffer(gpu, texTransferBuf, true);
    if (!mapped) return false;
    std::memcpy(mapped, data, bytes);
    SDL_UnmapGPUTransferBuffer(gpu, texTransferBuf);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) return false;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = texTransferBuf;
    src.offset = 0;
    SDL_GPUTextureRegion dst = {};
    dst.texture = tex;
    dst.mip_level = level;
    dst.w = width;
    dst.h = height;
    dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    return SDL_SubmitGPUCommandBuffer(cmd);
}

bool SdlGpuDevice::Impl::MakeTarget(const Resource* key, bool depth, Target& t) {
    t.depth = depth;
    t.width = std::max(key->width, 1u);
    t.height = std::max(key->height, 1u);
    t.physicalWidth = Physical(t.width, scaleX);
    t.physicalHeight = Physical(t.height, scaleY);
    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.width = t.physicalWidth;
    ti.height = t.physicalHeight;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    if (depth) {
        ti.format = depthFormat;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    } else {
        ti.format = colorFormat;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    }
    t.tex = SDL_CreateGPUTexture(gpu, &ti);
    return t.tex != nullptr;
}

SdlGpuDevice::Impl::Target* SdlGpuDevice::Impl::ColorTarget(Resource* r) {
    if (!r) return nullptr;
    const Resource* key = ColorTargetKey(r);
    auto it = targets.find(key);
    if (it != targets.end()) return &it->second;
    Target t;
    if (!MakeTarget(key, false, t)) return nullptr;
    return &(targets[key] = t);
}

SdlGpuDevice::Impl::Target* SdlGpuDevice::Impl::DepthTarget(Resource* r) {
    if (!r) return nullptr;
    auto it = targets.find(r);
    if (it != targets.end()) return &it->second;
    Target t;
    if (!MakeTarget(r, true, t)) return nullptr;
    return &(targets[r] = t);
}

void SdlGpuDevice::Impl::ApplyScale(double sx, double sy) {
    if ((sx == scaleX && sy == scaleY) || !gpu) return;
    Flush();
    scaleX = sx;
    scaleY = sy;

    std::vector<SDL_GPUTexture*> toRelease;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUGraphicsPipeline* pipe = GetBlitPipeline(colorFormat);

    for (auto& [key, old] : targets) {
        Target t;
        if (!MakeTarget(key, old.depth, t)) continue;
        if (!old.depth && old.tex && t.tex && cmd && pipe) {
            SDL_GPUColorTargetInfo ci = {};
            ci.texture = t.tex;
            ci.load_op = SDL_GPU_LOADOP_DONT_CARE;
            ci.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ci, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(rp, pipe);
            SDL_GPUViewport vp{0.0f, 0.0f, f32(t.physicalWidth), f32(t.physicalHeight), 0.0f, 1.0f};
            SDL_SetGPUViewport(rp, &vp);
            SDL_GPUTextureSamplerBinding sb{old.tex, blitSamplers[0]};
            SDL_BindGPUFragmentSamplers(rp, 0, &sb, 1);
            SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
            SDL_EndGPURenderPass(rp);
        }
        if (old.tex) toRelease.push_back(old.tex);
        old = t;
    }
    if (cmd) SDL_SubmitGPUCommandBuffer(cmd);
    for (SDL_GPUTexture* tex : toRelease) SDL_ReleaseGPUTexture(gpu, tex);
}

SDL_GPUGraphicsPipeline* SdlGpuDevice::Impl::GetBlitPipeline(SDL_GPUTextureFormat fmt) {
    auto& p = blitPipelines[u32(fmt)];
    if (p) return p;
    SDL_GPUColorTargetDescription ctd = {};
    ctd.format = fmt;
    SDL_GPUGraphicsPipelineCreateInfo pci = {};
    pci.vertex_shader = blitVs;
    pci.fragment_shader = blitPs;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    pci.rasterizer_state.enable_depth_clip = true;
    pci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets = 1;
    p = SDL_CreateGPUGraphicsPipeline(gpu, &pci);
    return p;
}

SDL_GPUGraphicsPipeline* SdlGpuDevice::Impl::GetClearPipeline(bool hasDepth, bool writeColor, bool writeDepth,
                                                              bool writeStencil) {
    const u32 key = (u32(hasDepth) << 0) | (u32(writeColor) << 1) | (u32(writeDepth) << 2) | (u32(writeStencil) << 3);
    auto& p = clearPipelines[key];
    if (p) return p;

    SDL_GPUColorTargetDescription ctd = {};
    ctd.format = colorFormat;
    ctd.blend_state.enable_color_write_mask = true;
    ctd.blend_state.color_write_mask = writeColor
        ? (SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A)
        : 0;

    SDL_GPUGraphicsPipelineCreateInfo pci = {};
    pci.vertex_shader = clearVs;
    pci.fragment_shader = clearPs;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    pci.rasterizer_state.enable_depth_clip = true;
    pci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pci.depth_stencil_state.enable_depth_test = hasDepth && writeDepth;
    pci.depth_stencil_state.enable_depth_write = hasDepth && writeDepth;
    pci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    pci.depth_stencil_state.enable_stencil_test = hasDepth && writeStencil;
    pci.depth_stencil_state.compare_mask = 0xFF;
    pci.depth_stencil_state.write_mask = writeStencil ? 0xFF : 0x00;
    pci.depth_stencil_state.front_stencil_state = {
        SDL_GPU_STENCILOP_REPLACE, SDL_GPU_STENCILOP_REPLACE, SDL_GPU_STENCILOP_REPLACE, SDL_GPU_COMPAREOP_ALWAYS
    };
    pci.depth_stencil_state.back_stencil_state = pci.depth_stencil_state.front_stencil_state;
    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets = 1;
    pci.target_info.has_depth_stencil_target = hasDepth;
    pci.target_info.depth_stencil_format = depthFormat;
    p = SDL_CreateGPUGraphicsPipeline(gpu, &pci);
    return p;
}

SDL_GPUGraphicsPipeline* SdlGpuDevice::Impl::GetDrawPipeline(const std::vector<u32>& key, SDL_GPUPrimitiveType topo,
                                                             bool hasDepth, const u32* rs) {
    auto& p = pipelines[key];
    if (p) return p;

    static constexpr SDL_GPUVertexAttribute attrs[8] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, u32(offsetof(CVertex, pos))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, u32(offsetof(CVertex, nrm))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, u32(offsetof(CVertex, d0))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, u32(offsetof(CVertex, d1))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, u32(offsetof(CVertex, uv[0]))},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, u32(offsetof(CVertex, uv[1]))},
        {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, u32(offsetof(CVertex, uv[2]))},
        {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, u32(offsetof(CVertex, uv[3]))},
    };
    const SDL_GPUVertexBufferDescription vbDesc{0, u32(sizeof(CVertex)), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};

    SDL_GPUColorTargetDescription ctd = {};
    ctd.format = colorFormat;
    auto& b = ctd.blend_state;
    b.enable_blend = rs[RS_ALPHABLENDENABLE] != 0;
    b.src_color_blendfactor = Blend(rs[RS_SRCBLEND]);
    b.dst_color_blendfactor = Blend(rs[RS_DESTBLEND]);
    b.src_alpha_blendfactor = AlphaBlend(b.src_color_blendfactor);
    b.dst_alpha_blendfactor = AlphaBlend(b.dst_color_blendfactor);
    b.color_blend_op = b.alpha_blend_op = BlendOp(rs[RS_BLENDOP]);
    const u32 cw = rs[RS_COLORWRITEENABLE];
    b.enable_color_write_mask = true;
    b.color_write_mask = u8(((cw & 0x10000) ? SDL_GPU_COLORCOMPONENT_R : 0) |
                            ((cw & 0x100) ? SDL_GPU_COLORCOMPONENT_G : 0) |
                            ((cw & 1) ? SDL_GPU_COLORCOMPONENT_B : 0) |
                            ((cw & 0x1000000) ? SDL_GPU_COLORCOMPONENT_A : 0));

    SDL_GPUGraphicsPipelineCreateInfo pci = {};
    pci.vertex_shader = vs;
    pci.fragment_shader = ps;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbDesc;
    pci.vertex_input_state.num_vertex_buffers = 1;
    pci.vertex_input_state.vertex_attributes = attrs;
    pci.vertex_input_state.num_vertex_attributes = 8;
    pci.primitive_type = topo;

    auto& rz = pci.rasterizer_state;
    rz.fill_mode = rs[RS_FILLMODE] == 0x1B01 ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    const bool swap = rs[RS_FRONTFACE] == 0x901;
    rz.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    rz.cull_mode = rs[RS_CULLMODE] == 0 ? SDL_GPU_CULLMODE_NONE
                 : (rs[RS_CULLMODE] == 0x900) != swap ? SDL_GPU_CULLMODE_FRONT : SDL_GPU_CULLMODE_BACK;
    if (rs[RS_SOLIDOFFSETENABLE]) {
        rz.enable_depth_bias = true;
        rz.depth_bias_constant_factor = f32(s32(AsFloat(rs[RS_POLYGONOFFSETZOFFSET])));
        rz.depth_bias_slope_factor = AsFloat(rs[RS_POLYGONOFFSETZSLOPESCALE]);
    }
    rz.enable_depth_clip = true;

    pci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

    if (hasDepth) {
        auto& ds = pci.depth_stencil_state;
        ds.enable_depth_test = rs[RS_ZENABLE] != 0;
        ds.enable_depth_write = rs[RS_ZWRITEENABLE] != 0;
        ds.compare_op = Compare(rs[RS_ZFUNC]);
        ds.enable_stencil_test = rs[RS_STENCILENABLE] != 0;
        ds.compare_mask = u8(rs[RS_STENCILMASK]);
        ds.write_mask = u8(rs[RS_STENCILWRITEMASK]);
        ds.front_stencil_state.fail_op = StencilOp(rs[RS_STENCILFAIL]);
        ds.front_stencil_state.depth_fail_op = StencilOp(rs[RS_STENCILZFAIL]);
        ds.front_stencil_state.pass_op = StencilOp(rs[RS_STENCILPASS]);
        ds.front_stencil_state.compare_op = Compare(rs[RS_STENCILFUNC]);
        ds.back_stencil_state = ds.front_stencil_state;
    }

    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets = 1;
    pci.target_info.has_depth_stencil_target = hasDepth;
    pci.target_info.depth_stencil_format = depthFormat;
    p = SDL_CreateGPUGraphicsPipeline(gpu, &pci);
    return p;
}

SDL_GPUSampler* SdlGpuDevice::Impl::GetSampler(const u32* ts) {
    std::vector<u32> key = {
        ts[TSS_ADDRESSU], ts[TSS_ADDRESSV], ts[TSS_ADDRESSW], ts[TSS_MAGFILTER], ts[TSS_MINFILTER],
        ts[TSS_MIPFILTER], ts[TSS_MIPMAPLODBIAS], ts[TSS_MAXMIPLEVEL]
    };
    auto& sp = samplers[key];
    if (sp) return sp;
    SDL_GPUSamplerCreateInfo si = {};
    si.min_filter = ts[TSS_MINFILTER] >= 2 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mag_filter = ts[TSS_MAGFILTER] >= 2 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = ts[TSS_MIPFILTER] >= 2 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = Address(ts[TSS_ADDRESSU]);
    si.address_mode_v = Address(ts[TSS_ADDRESSV]);
    si.address_mode_w = Address(ts[TSS_ADDRESSW]);
    si.mip_lod_bias = AsFloat(ts[TSS_MIPMAPLODBIAS]);
    si.min_lod = f32(ts[TSS_MAXMIPLEVEL]);
    si.max_lod = ts[TSS_MIPFILTER] == 0 ? si.min_lod : 1000.0f;
    sp = SDL_CreateGPUSampler(gpu, &si);
    return sp ? sp : defaultSampler;
}

SDL_GPUTexture* SdlGpuDevice::Impl::Texture(Resource* r) {
    if (!r) return nullptr;
    if (r->rendered) {
        Target* t = ColorTarget(r);
        return t ? t->tex : white;
    }
    if (r->format == 0x24) return Yuy2Texture(*r);
    auto it = textures.find(r);
    if (it != textures.end()) return it->second.texture;

    Tex& tex = textures[r];
    const auto* src = static_cast<const ni::NiObject*>(r->source);
    const auto* st = dynamic_cast<const ni::NiSourceTexture*>(src);
    DecodedTexture dt;
    const bool xbox = r->levels && !std::all_of(r->data.begin(), r->data.end(), [](u8 v) { return v == 0; }) &&
                      DecodeXboxTexels(*r, dt);
    if (!xbox && (!st || !st->pixelDataObj || !DecodePixelData(*st->pixelDataObj, dt))) {
        NotPorted("SdlGpuDevice: a texture without decodable pixel data (white)");
        tex.texture = white;
        return tex.texture;
    }

    SDL_GPUTextureFormat fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    u32 block = 0;
    switch (dt.kind) {
    case DecodedTexture::Kind::BC1: fmt = SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM; block = 8; break;
    case DecodedTexture::Kind::BC2: fmt = SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM; block = 16; break;
    case DecodedTexture::Kind::BC3: fmt = SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM; block = 16; break;
    default: break;
    }

    if ((block && (dt.width % 4 || dt.height % 4)) ||
        !SDL_GPUTextureSupportsFormat(gpu, fmt, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        NotPorted("SdlGpuDevice: unsupported texture format or unaligned BC texture (white)");
        tex.texture = white;
        return tex.texture;
    }

    u32 w = dt.width, levels = 0;
    for (size_t i = 0; i < dt.levels.size(); ++i, ++levels) {
        if (block && (std::max(w, 1u) < 4 && i > 0)) break;
        w = std::max(1u, w / 2);
    }
    levels = std::max(levels, 1u);

    SDL_GPUTextureCreateInfo ti = {};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = fmt;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = dt.width;
    ti.height = dt.height;
    ti.layer_count_or_depth = 1;
    ti.num_levels = levels;
    ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
    tex.texture = SDL_CreateGPUTexture(gpu, &ti);
    if (!tex.texture) {
        NotPorted("SdlGpuDevice: a texture the device cannot create (white)");
        tex.texture = white;
        return tex.texture;
    }

    u32 mw = dt.width, mh = dt.height;
    for (u32 i = 0; i < levels; ++i, mw = std::max(1u, mw / 2), mh = std::max(1u, mh / 2)) {
        UploadTextureData(tex.texture, mw, mh, i, dt.levels[i].data(), u32(dt.levels[i].size()));
    }
    return tex.texture;
}

SDL_GPUTexture* SdlGpuDevice::Impl::Yuy2Texture(Resource& r) {
    Tex& tex = textures[&r];
    const u32 w = r.width & ~1u, h = r.height;
    if (!w || !h || r.data.size() < size_t(w) * h * 2) return white;
    if (!tex.texture) {
        SDL_GPUTextureCreateInfo ti = {};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = w;
        ti.height = h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
        tex.texture = SDL_CreateGPUTexture(gpu, &ti);
        if (!tex.texture) return white;
        tex.revision = r.revision - 1;
    }
    if (tex.revision != r.revision) {
        if (pendingTexUses.count(&r)) Flush();
        tex.revision = r.revision;
        std::vector<u8> rgba(size_t(w) * h * 4);
        for (u32 y = 0; y < h; ++y) {
            const u8* s = r.data.data() + size_t(y) * w * 2;
            u8* d = rgba.data() + size_t(y) * w * 4;
            for (u32 x = 0; x < w; x += 2, s += 4, d += 8) {
                d[0] = s[0], d[1] = s[1], d[2] = s[3], d[3] = 0xFF;
                d[4] = s[2], d[5] = s[1], d[6] = s[3], d[7] = 0xFF;
            }
        }
        UploadTextureData(tex.texture, w, h, 0, rgba.data(), u32(rgba.size()));
    }
    return tex.texture;
}

bool SdlGpuDevice::Impl::EnsureFrameBuffers(u32 vertBytes, u32 idxBytes) {
    if (vertBytes > vbCapacity) {
        if (vb) SDL_ReleaseGPUBuffer(gpu, vb);
        vbCapacity = std::max(vertBytes, std::max(vbCapacity * 2, 256u * 1024u));
        SDL_GPUBufferCreateInfo bci = {};
        bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bci.size = vbCapacity;
        vb = SDL_CreateGPUBuffer(gpu, &bci);
        if (!vb) return false;
    }
    if (idxBytes > ibCapacity) {
        if (ib) SDL_ReleaseGPUBuffer(gpu, ib);
        ibCapacity = std::max(idxBytes, std::max(ibCapacity * 2, 128u * 1024u));
        SDL_GPUBufferCreateInfo bci = {};
        bci.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        bci.size = ibCapacity;
        ib = SDL_CreateGPUBuffer(gpu, &bci);
        if (!ib) return false;
    }
    const u32 total = vertBytes + idxBytes;
    if (total > transferCapacity) {
        if (transferBuf) SDL_ReleaseGPUTransferBuffer(gpu, transferBuf);
        transferCapacity = std::max(total, std::max(transferCapacity * 2, 512u * 1024u));
        SDL_GPUTransferBufferCreateInfo tbi = {};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = transferCapacity;
        transferBuf = SDL_CreateGPUTransferBuffer(gpu, &tbi);
        if (!transferBuf) return false;
    }
    return true;
}

SdlGpuDevice::Impl::PassRecord& SdlGpuDevice::Impl::ActivePass(Target* ct, Target* dt, const Resource* ctKey) {
    if (!passes.empty()) {
        PassRecord& last = passes.back();
        if (last.ct == ct && last.dt == dt) return last;
    }
    PassRecord p;
    p.ct = ct;
    p.dt = dt;
    p.ctKey = ctKey;
    passes.push_back(std::move(p));
    return passes.back();
}

void SdlGpuDevice::Impl::Flush() {
    if (!gpu || passes.empty()) return;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    if (!cmd) return;

    const u32 vertBytes = u32(frameVerts.size() * sizeof(CVertex));
    const u32 idxBytes = u32(frameIndices.size() * sizeof(u32));
    if (vertBytes > 0 || idxBytes > 0) {
        if (EnsureFrameBuffers(std::max(vertBytes, 4u), std::max(idxBytes, 4u))) {
            u8* mapped = static_cast<u8*>(SDL_MapGPUTransferBuffer(gpu, transferBuf, true));
            if (mapped) {
                if (vertBytes > 0) std::memcpy(mapped, frameVerts.data(), vertBytes);
                if (idxBytes > 0) std::memcpy(mapped + vertBytes, frameIndices.data(), idxBytes);
                SDL_UnmapGPUTransferBuffer(gpu, transferBuf);

                SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
                if (vertBytes > 0) {
                    SDL_GPUTransferBufferLocation src{transferBuf, 0};
                    SDL_GPUBufferRegion dst{vb, 0, vertBytes};
                    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
                }
                if (idxBytes > 0) {
                    SDL_GPUTransferBufferLocation src{transferBuf, vertBytes};
                    SDL_GPUBufferRegion dst{ib, 0, idxBytes};
                    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
                }
                SDL_EndGPUCopyPass(cp);
            }
        }
    }

    for (const PassRecord& pass : passes) {
        if (!pass.ct || !pass.ct->tex) continue;
        SDL_GPUColorTargetInfo ci = {};
        ci.texture = pass.ct->tex;
        ci.load_op = pass.colorLoadOp;
        ci.store_op = SDL_GPU_STOREOP_STORE;
        ci.clear_color = pass.clearColor;

        SDL_GPUDepthStencilTargetInfo di = {};
        if (pass.dt && pass.dt->tex) {
            di.texture = pass.dt->tex;
            di.load_op = pass.depthLoadOp;
            di.store_op = SDL_GPU_STOREOP_STORE;
            di.stencil_load_op = pass.stencilLoadOp;
            di.stencil_store_op = SDL_GPU_STOREOP_STORE;
            di.clear_depth = pass.clearDepth;
            di.clear_stencil = pass.clearStencil;
        }

        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ci, 1, (pass.dt && pass.dt->tex) ? &di : nullptr);
        for (const DrawItem& item : pass.items) {
            if (!item.pipeline) continue;
            SDL_BindGPUGraphicsPipeline(rp, item.pipeline);
            SDL_SetGPUViewport(rp, &item.viewport);
            if (item.isClearQuad) {
                SDL_SetGPUStencilReference(rp, item.stencilRef);
                SDL_PushGPUVertexUniformData(cmd, 0, &item.clearCB, sizeof item.clearCB);
                SDL_PushGPUFragmentUniformData(cmd, 0, &item.clearCB, sizeof item.clearCB);
                SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
            } else {
                SDL_SetGPUBlendConstants(rp, item.blendColor);
                SDL_SetGPUStencilReference(rp, item.stencilRef);
                SDL_GPUBufferBinding vbb{vb, item.vertByteOffset};
                SDL_GPUBufferBinding ibb{ib, item.idxByteOffset};
                SDL_BindGPUVertexBuffers(rp, 0, &vbb, 1);
                SDL_BindGPUIndexBuffer(rp, &ibb, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_PushGPUVertexUniformData(cmd, 0, &item.drawCB, sizeof item.drawCB);
                SDL_PushGPUFragmentUniformData(cmd, 0, &item.drawCB, sizeof item.drawCB);
                SDL_BindGPUFragmentSamplers(rp, 0, item.bindings, 4);
                SDL_DrawGPUIndexedPrimitives(rp, item.idxCount, 1, 0, 0, 0);
            }
        }
        SDL_EndGPURenderPass(rp);
    }

    SDL_SubmitGPUCommandBuffer(cmd);
    passes.clear();
    frameVerts.clear();
    frameIndices.clear();
    pendingTargetWrites.clear();
    pendingTexUses.clear();
}

void SdlGpuDevice::Impl::Destroy() {
    if (!gpu) return;
    SDL_WaitForGPUIdle(gpu);
    for (auto& [k, p] : pipelines) if (p) SDL_ReleaseGPUGraphicsPipeline(gpu, p);
    for (auto& [k, p] : clearPipelines) if (p) SDL_ReleaseGPUGraphicsPipeline(gpu, p);
    for (auto& [k, p] : blitPipelines) if (p) SDL_ReleaseGPUGraphicsPipeline(gpu, p);
    for (auto& [k, s] : samplers) if (s) SDL_ReleaseGPUSampler(gpu, s);
    for (SDL_GPUSampler* s : blitSamplers) if (s) SDL_ReleaseGPUSampler(gpu, s);
    if (defaultSampler) SDL_ReleaseGPUSampler(gpu, defaultSampler);
    for (auto& [k, t] : targets) if (t.tex) SDL_ReleaseGPUTexture(gpu, t.tex);
    for (auto& [k, t] : textures) if (t.texture && t.texture != white) SDL_ReleaseGPUTexture(gpu, t.texture);
    if (white) SDL_ReleaseGPUTexture(gpu, white);
    if (vb) SDL_ReleaseGPUBuffer(gpu, vb);
    if (ib) SDL_ReleaseGPUBuffer(gpu, ib);
    if (transferBuf) SDL_ReleaseGPUTransferBuffer(gpu, transferBuf);
    if (texTransferBuf) SDL_ReleaseGPUTransferBuffer(gpu, texTransferBuf);
    if (vs) SDL_ReleaseGPUShader(gpu, vs);
    if (ps) SDL_ReleaseGPUShader(gpu, ps);
    if (clearVs) SDL_ReleaseGPUShader(gpu, clearVs);
    if (clearPs) SDL_ReleaseGPUShader(gpu, clearPs);
    if (blitVs) SDL_ReleaseGPUShader(gpu, blitVs);
    if (blitPs) SDL_ReleaseGPUShader(gpu, blitPs);
    if (window) SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
    gpu = nullptr;
    window = nullptr;
}

SdlGpuDevice::SdlGpuDevice() : impl_(std::make_unique<Impl>()) {}
SdlGpuDevice::~SdlGpuDevice() { impl_->Destroy(); }

bool SdlGpuDevice::Init(SDL_Window* window, u32 width, u32 height, std::string& error) {
    Impl& d = *impl_;
    d.window = window;
    d.windowWidth = std::max(width, 1u);
    d.windowHeight = std::max(height, 1u);

    const bool debug = std::getenv("FUT_GPU_DEBUG") != nullptr;
    d.gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, debug, nullptr);
    if (!d.gpu) {
        error = std::string("SDL_CreateGPUDevice failed: ") + SDL_GetError();
        return false;
    }
    if (d.window && !SDL_ClaimWindowForGPUDevice(d.gpu, d.window)) {
        error = std::string("SDL_ClaimWindowForGPUDevice failed: ") + SDL_GetError();
        d.Destroy();
        return false;
    }

    if (SDL_GPUTextureSupportsFormat(d.gpu, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
                                     SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        d.depthFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    } else {
        d.depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    }

    d.vs = d.LoadShader("VSMain", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, error);
    if (!d.vs) return false;
    d.ps = d.LoadShader("PSMain", SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1, error);
    if (!d.ps) return false;
    d.clearVs = d.LoadShader("ClearVS", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, error);
    if (!d.clearVs) return false;
    d.clearPs = d.LoadShader("ClearPS", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1, error);
    if (!d.clearPs) return false;
    d.blitVs = d.LoadShader("BlitVS", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, error);
    if (!d.blitVs) return false;
    d.blitPs = d.LoadShader("BlitPS", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, error);
    if (!d.blitPs) return false;

    for (u32 i = 0; i < 2; ++i) {
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter = i == 0 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        si.mag_filter = i == 0 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = i == 0 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.max_lod = 1000.0f;
        d.blitSamplers[i] = SDL_CreateGPUSampler(d.gpu, &si);
    }
    {
        SDL_GPUSamplerCreateInfo si = {};
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        si.max_lod = 1000.0f;
        d.defaultSampler = SDL_CreateGPUSampler(d.gpu, &si);
    }
    {
        const u32 px = 0xFFFFFFFFu;
        SDL_GPUTextureCreateInfo ti = {};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = ti.height = 1;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
        d.white = SDL_CreateGPUTexture(d.gpu, &ti);
        if (d.white) d.UploadTextureData(d.white, 1, 1, 0, &px, 4);
    }

    if (!d.ColorTarget(backBuffer()) || !d.DepthTarget(depthBuffer())) {
        error = "failed to create initial GPU render targets";
        return false;
    }
    SetRenderScale(d.autoScale ? 0.0 : d.fixedScale);
    return true;
}

void SdlGpuDevice::Clear(u32 count, const D3DRECT* rects, u32 flags, u32 color, f32 z, u32 stencil) {
    XD3DDevice::Clear(count, rects, flags, color, z, stencil);
    Impl& d = *impl_;
    if (!d.gpu) return;
    Impl::Target* ct = d.ColorTarget(renderTarget);
    Impl::Target* dt = d.DepthTarget(depthStencil);
    if (!ct) return;

    f32 c[4];
    Color(color, c);
    D3DRECT r{s32(viewport.X), s32(viewport.Y), s32(viewport.X + viewport.Width), s32(viewport.Y + viewport.Height)};
    if (rects && count) r = rects[0];
    if (count > 1) NotPorted("SdlGpuDevice::Clear: more than one rectangle");
    const bool whole = r.x1 <= 0 && r.y1 <= 0 && u32(r.x2) >= ct->width && u32(r.y2) >= ct->height;
    const Resource* ctKey = d.ColorTargetKey(renderTarget);

    if (whole) {
        if (!d.passes.empty()) {
            Impl::PassRecord& last = d.passes.back();
            if (last.ct == ct && last.dt == dt && last.items.empty()) {
                if (flags & 0xF0) {
                    last.colorLoadOp = SDL_GPU_LOADOP_CLEAR;
                    last.clearColor = {c[0], c[1], c[2], c[3]};
                }
                if ((flags & 1) && dt) {
                    last.depthLoadOp = SDL_GPU_LOADOP_CLEAR;
                    last.clearDepth = z;
                }
                if ((flags & 2) && dt) {
                    last.stencilLoadOp = SDL_GPU_LOADOP_CLEAR;
                    last.clearStencil = u8(stencil);
                }
                d.pendingTargetWrites.insert(ctKey);
                return;
            }
        }
        Impl::PassRecord p;
        p.ct = ct;
        p.dt = dt;
        p.ctKey = ctKey;
        if (flags & 0xF0) {
            p.colorLoadOp = SDL_GPU_LOADOP_CLEAR;
            p.clearColor = {c[0], c[1], c[2], c[3]};
        }
        if ((flags & 1) && dt) {
            p.depthLoadOp = SDL_GPU_LOADOP_CLEAR;
            p.clearDepth = z;
        }
        if ((flags & 2) && dt) {
            p.stencilLoadOp = SDL_GPU_LOADOP_CLEAR;
            p.clearStencil = u8(stencil);
        }
        d.passes.push_back(std::move(p));
        d.pendingTargetWrites.insert(ctKey);
        return;
    }

    const bool writeColor = (flags & 0xF0) != 0;
    const bool writeDepth = ((flags & 1) != 0) && dt != nullptr;
    const bool writeStencil = ((flags & 2) != 0) && dt != nullptr;
    if (!writeColor && !writeDepth && !writeStencil) return;

    Impl::PassRecord& pass = d.ActivePass(ct, dt, ctKey);
    Impl::DrawItem item;
    item.isClearQuad = true;
    item.pipeline = d.GetClearPipeline(dt != nullptr, writeColor, writeDepth, writeStencil);
    item.viewport = {0.0f, 0.0f, f32(ct->physicalWidth), f32(ct->physicalHeight), 0.0f, 1.0f};
    item.stencilRef = u8(stencil);
    item.clearCB.rect[0] = f32(r.x1) / ct->width * 2.0f - 1.0f;
    item.clearCB.rect[1] = 1.0f - f32(r.y1) / ct->height * 2.0f;
    item.clearCB.rect[2] = f32(r.x2) / ct->width * 2.0f - 1.0f;
    item.clearCB.rect[3] = 1.0f - f32(r.y2) / ct->height * 2.0f;
    std::memcpy(item.clearCB.color, c, 16);
    item.clearCB.z[0] = z;
    pass.items.push_back(std::move(item));
    d.pendingTargetWrites.insert(ctKey);
}

void SdlGpuDevice::Swap(u32 flags) {
    XD3DDevice::Swap(flags);
    Impl& d = *impl_;
    if (!d.gpu) return;
    Impl::Target* bb = d.ColorTarget(backBuffer());
    if (!bb || !bb->tex) return;

    d.Flush();

    if (!d.window) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(d.gpu);
    if (!cmd) return;
    SDL_GPUTexture* swapTex = nullptr;
    Uint32 sw = 0, sh = 0;
    if (SDL_WaitAndAcquireGPUSwapchainTexture(cmd, d.window, &swapTex, &sw, &sh) && swapTex && sw && sh) {
        d.windowWidth = sw;
        d.windowHeight = sh;
        SDL_GPUColorTargetInfo ci = {};
        ci.texture = swapTex;
        ci.load_op = SDL_GPU_LOADOP_CLEAR;
        ci.store_op = SDL_GPU_STOREOP_STORE;
        ci.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
        SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ci, 1, nullptr);
        SDL_GPUTextureFormat swapFmt = SDL_GetGPUSwapchainTextureFormat(d.gpu, d.window);
        if (SDL_GPUGraphicsPipeline* pipe = d.GetBlitPipeline(swapFmt)) {
            const double fit = std::min(double(d.windowWidth) / bb->physicalWidth,
                                        double(d.windowHeight) / bb->physicalHeight);
            const u32 w = std::max(u32(std::lround(bb->physicalWidth * fit)), 1u);
            const u32 h = std::max(u32(std::lround(bb->physicalHeight * fit)), 1u);
            SDL_GPUViewport vp{f32((d.windowWidth - w) / 2), f32((d.windowHeight - h) / 2), f32(w), f32(h), 0.0f, 1.0f};
            SDL_BindGPUGraphicsPipeline(rp, pipe);
            SDL_SetGPUViewport(rp, &vp);
            SDL_GPUTextureSamplerBinding sb{bb->tex, d.blitSamplers[d.filter == Filter::Nearest ? 1 : 0]};
            SDL_BindGPUFragmentSamplers(rp, 0, &sb, 1);
            SDL_DrawGPUPrimitives(rp, 4, 1, 0, 0);
        }
        SDL_EndGPURenderPass(rp);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

void SdlGpuDevice::Resize(u32 width, u32 height) {
    Impl& d = *impl_;
    if (!d.window || !width || !height || (width == d.windowWidth && height == d.windowHeight)) return;
    d.windowWidth = width;
    d.windowHeight = height;
    if (d.autoScale) SetRenderScale(0.0);
}

void SdlGpuDevice::SetFilter(Filter filter) { impl_->filter = filter; }

void SdlGpuDevice::SetRenderScale(double factor) {
    Impl& d = *impl_;
    d.autoScale = factor <= 0;
    d.fixedScale = d.autoScale ? 1.0 : factor;
    if (!d.gpu) return;
    if (!d.autoScale) {
        d.ApplyScale(d.fixedScale, d.fixedScale);
    } else if (d.window) {
        const Resource* bb = backBuffer();
        const u32 w = std::max(bb->width, 1u), h = std::max(bb->height, 1u);
        const double s = std::min(double(d.windowWidth) / w, double(d.windowHeight) / h);
        const bool snapX = std::labs(std::lround(w * s) - long(d.windowWidth)) <= 1;
        const bool snapY = std::labs(std::lround(h * s) - long(d.windowHeight)) <= 1;
        d.ApplyScale(snapX ? double(d.windowWidth) / w : s, snapY ? double(d.windowHeight) / h : s);
    } else {
        d.ApplyScale(1.0, 1.0);
    }
}

double SdlGpuDevice::RenderScale() const { return impl_->scaleX; }

void SdlGpuDevice::DrawVertices(u32 primitive, u32 start, u32 count) {
    XD3DDevice::DrawVertices(primitive, start, count);
    Draw(primitive, nullptr, count, start);
}

void SdlGpuDevice::DrawIndexedVertices(u32 primitive, u32 count, const u16* data) {
    XD3DDevice::DrawIndexedVertices(primitive, count, data);
    Draw(primitive, data, count, 0);
}

void SdlGpuDevice::Draw(u32 primitive, const u16* list, u32 count, u32 start) {
    Impl& d = *impl_;
    if (!d.gpu || !count || !streamSource || !streamStride) return;
    if (pixelShader) NotPorted("SdlGpuDevice: a combiner pixel shader program (the texture stages are used)");
    const u32 fvf = vertexShader;
    if (fvf & 0xFFFF0000u & ~0x00FF0000u) NotPorted("SdlGpuDevice: an FVF beyond the texture-coordinate sizes");

    u32 lo = start, hi = start + count - 1;
    if (list) {
        lo = 0xFFFFFFFFu, hi = 0;
        for (u32 i = 0; i < count; ++i) lo = std::min<u32>(lo, list[i]), hi = std::max<u32>(hi, list[i]);
        lo += baseVertex, hi += baseVertex;
    }
    const u32 n = hi - lo + 1;
    const std::vector<u8>& src = streamSource->data;
    if (size_t(hi + 1) * streamStride > src.size()) return;

    const u32 posKind = fvf & 0x00E;
    const bool rhw = posKind == 0x004, hasN = (fvf & 0x010) != 0, hasD = (fvf & 0x040) != 0, hasS = (fvf & 0x080) != 0;
    const u32 texCount = (fvf >> 8) & 0xF;
    u32 dims[8] = {};
    for (u32 i = 0; i < texCount && i < 8; ++i) {
        const u32 sz = (fvf >> (16 + 2 * i)) & 3;
        dims[i] = sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1;
    }

    d.scratchVerts.resize(n);
    for (u32 v = 0; v < n; ++v) {
        const u8* p = src.data() + size_t(lo + v) * streamStride;
        CVertex& c = d.scratchVerts[v];
        std::memset(&c, 0, sizeof c);
        const u32 pc = rhw ? 4 : 3;
        std::memcpy(c.pos, p, pc * 4);
        if (!rhw) c.pos[3] = 1.0f;
        p += pc * 4;
        if (fvf & 0x020) p += 4;
        if (hasN) std::memcpy(c.nrm, p, 12), p += 12;
        if (hasD) {
            u32 a;
            std::memcpy(&a, p, 4);
            Color(a, c.d0);
            p += 4;
        }
        if (hasS) {
            u32 a;
            std::memcpy(&a, p, 4);
            Color(a, c.d1);
            p += 4;
        }
        for (u32 t = 0; t < texCount && t < 4; ++t) {
            std::memcpy(c.uv[t], p, dims[t] * 4);
            p += dims[t] * 4;
        }
    }

    d.scratchIdx.clear();
    auto at = [&](u32 i) -> u32 { return list ? u32(list[i]) + baseVertex - lo : start + i - lo; };
    SDL_GPUPrimitiveType topo = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    switch (primitive) {
    case PT_POINTLIST:
        topo = SDL_GPU_PRIMITIVETYPE_POINTLIST;
        for (u32 i = 0; i < count; ++i) d.scratchIdx.push_back(at(i));
        break;
    case PT_LINELIST:
        topo = SDL_GPU_PRIMITIVETYPE_LINELIST;
        for (u32 i = 0; i < count; ++i) d.scratchIdx.push_back(at(i));
        break;
    case PT_LINESTRIP:
        topo = SDL_GPU_PRIMITIVETYPE_LINESTRIP;
        for (u32 i = 0; i < count; ++i) d.scratchIdx.push_back(at(i));
        break;
    case PT_TRIANGLELIST:
        for (u32 i = 0; i < count; ++i) d.scratchIdx.push_back(at(i));
        break;
    case PT_TRIANGLESTRIP:
        topo = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
        for (u32 i = 0; i < count; ++i) d.scratchIdx.push_back(at(i));
        break;
    case PT_TRIANGLEFAN:
    case PT_POLYGON:
        for (u32 i = 2; i < count; ++i) d.scratchIdx.insert(d.scratchIdx.end(), {at(0), at(i - 1), at(i)});
        break;
    case PT_QUADLIST:
        for (u32 i = 0; i + 3 < count; i += 4)
            d.scratchIdx.insert(d.scratchIdx.end(), {at(i), at(i + 1), at(i + 2), at(i), at(i + 2), at(i + 3)});
        break;
    default: NotPorted("SdlGpuDevice: a primitive type (line loops, quad strips)"); return;
    }
    if (d.scratchIdx.empty()) return;

    // If any sampled texture is a render target with pending writes, flush first so it is resolved before sampling.
    for (u32 s = 0; s < 4; ++s) {
        if (Resource* t = textures[s]; t && t->rendered && d.pendingTargetWrites.count(d.ColorTargetKey(t))) {
            d.Flush();
            break;
        }
    }

    Impl::Target* ct = d.ColorTarget(renderTarget);
    Impl::Target* dt = d.DepthTarget(depthStencil);
    if (!ct) return;

    DrawCB cb{};
    cb.world = transforms[TS_WORLD], cb.view = transforms[TS_VIEW], cb.proj = transforms[TS_PROJECTION];
    for (u32 s = 0; s < 4; ++s) cb.texm[s] = transforms[TS_TEXTURE0 + s];
    cb.vp[0] = f32(viewport.X), cb.vp[1] = f32(viewport.Y), cb.vp[2] = f32(std::max(viewport.Width, 1u));
    cb.vp[3] = f32(std::max(viewport.Height, 1u));
    cb.ssOffset[0] = screenSpaceOffset[0], cb.ssOffset[1] = screenSpaceOffset[1];
    cb.ssOffset[2] = f32(ct->width) / ct->physicalWidth;
    cb.ssOffset[3] = f32(ct->height) / ct->physicalHeight;
    std::memcpy(cb.matDiffuse, material.diffuse, 16), std::memcpy(cb.matAmbient, material.ambient, 16);
    std::memcpy(cb.matSpecular, material.specular, 16), std::memcpy(cb.matEmissive, material.emissive, 16);

    const u32* rs = renderState;
    Color(rs[RS_AMBIENT], cb.ambient);
    cb.misc0[0] = material.power, cb.misc0[1] = f32(rs[RS_LIGHTING] != 0), cb.misc0[2] = f32(rs[RS_COLORVERTEX] != 0);
    cb.misc0[3] = f32(rs[RS_NORMALIZENORMALS] != 0);
    cb.misc1[0] = f32(rs[RS_DIFFUSEMATERIALSOURCE]), cb.misc1[1] = f32(rs[RS_AMBIENTMATERIALSOURCE]);
    cb.misc1[2] = f32(rs[RS_SPECULARMATERIALSOURCE]), cb.misc1[3] = f32(rs[RS_EMISSIVEMATERIALSOURCE]);
    cb.misc2[0] = f32(hasN), cb.misc2[1] = f32(hasD), cb.misc2[2] = f32(hasS), cb.misc2[3] = f32(rhw);
    cb.misc3[0] = f32(rs[RS_SPECULARENABLE] != 0), cb.misc3[1] = f32(rs[RS_LOCALVIEWER] != 0);
    cb.misc3[2] = f32(rs[RS_FOGENABLE] != 0);
    {
        const f32 fogStart = AsFloat(rs[RS_FOGSTART]), end = AsFloat(rs[RS_FOGEND]), dens = AsFloat(rs[RS_FOGDENSITY]);
        u32 gen = rs[RS_RANGEFOGENABLE] == 0 ? 2u : 1u, mode = 0;
        f32 k0 = 1.0f, k1 = 1.0f;
        switch (rs[RS_FOGTABLEMODE]) {
        case 0: gen = 0; break;
        case 3: {
            const double sc = end == fogStart ? 8192.0 : 1.0 / (double(end) - double(fogStart));
            k0 = f32(double(end) * sc + 1.0);
            k1 = f32(-sc);
            break;
        }
        case 1:
            k0 = 1.5f, k1 = f32(double(dens) * double(std::bit_cast<f32>(0xBDB8AA0Au))), mode = 1;
            break;
        default:
            k0 = 1.5f, k1 = f32(double(dens) * double(std::bit_cast<f32>(0xBE596D0Bu))), mode = 2;
            break;
        }
        cb.misc3[3] = f32(mode);
        cb.fogParams[0] = k0, cb.fogParams[1] = k1, cb.fogParams[2] = 0.0f, cb.fogParams[3] = f32(gen);
    }
    Color(rs[RS_FOGCOLOR], cb.fogColor);
    Color(rs[RS_TEXTUREFACTOR], cb.tfactor);
    cb.alphaTest[0] = f32(rs[RS_ALPHATESTENABLE] != 0), cb.alphaTest[1] = f32((rs[RS_ALPHAFUNC] - 0x200) & 7);
    cb.alphaTest[2] = f32(rs[RS_ALPHAREF] & 0xFF) / 255.0f, cb.alphaTest[3] = f32(rs[RS_SHADEMODE] == 0x1D00);

    SDL_GPUTextureSamplerBinding bindings[4] = {};
    for (u32 s = 0; s < 4; ++s) {
        const u32* ts = textureState[s];
        const u32 tci = ts[TSS_TEXCOORDINDEX], ttf = ts[TSS_TEXTURETRANSFORMFLAGS];
        cb.tci[s][0] = s32(tci & 0xFFFF), cb.tci[s][1] = s32((tci >> 16) & 0xF);
        if ((tci >> 16) > 3) NotPorted("SdlGpuDevice: a texture-coordinate generation mode above 3");
        cb.tci[s][2] = s32(ttf & 0xFF), cb.tci[s][3] = (ttf & 0x100) != 0;
        cb.colorOp[s][0] = s32(ts[TSS_COLOROP]), cb.colorOp[s][1] = s32(ts[TSS_COLORARG1]);
        cb.colorOp[s][2] = s32(ts[TSS_COLORARG2]), cb.colorOp[s][3] = s32(ts[TSS_COLORARG0]);
        cb.alphaOp[s][0] = s32(ts[TSS_ALPHAOP]), cb.alphaOp[s][1] = s32(ts[TSS_ALPHAARG1]);
        cb.alphaOp[s][2] = s32(ts[TSS_ALPHAARG2]), cb.alphaOp[s][3] = s32(ts[TSS_ALPHAARG0]);
        if (ts[TSS_COLOROP] == 25 || ts[TSS_COLOROP] == 26) NotPorted("SdlGpuDevice: the bump-env ops");
        cb.stageMisc[s][0] = s32(ts[TSS_RESULTARG]);
        cb.stageMisc[s][2] = s32((tci & 0xFFFF) < 8 ? dims[tci & 0xFFFF] : 2);
        cb.texScale[s][0] = cb.texScale[s][1] = 1.0f, cb.texScale[s][2] = 0.0f;
        bindings[s] = {d.white, d.defaultSampler};
        if (Resource* t = textures[s]) {
            SDL_GPUTexture* gtex = d.Texture(t);
            d.pendingTexUses.insert(t);
            cb.stageMisc[s][1] = gtex != nullptr;
            if (t->format == 0x24 && t->width && t->height) {
                cb.texScale[s][0] = 1.0f / f32(t->width), cb.texScale[s][1] = 1.0f / f32(t->height);
                cb.texScale[s][2] = rs[RS_YUVENABLE] != 0 ? 1.0f : 0.0f;
            }
            if (gtex) bindings[s].texture = gtex;
            bindings[s].sampler = d.GetSampler(ts);
        }
    }

    u32 nl = 0;
    for (u32 index : activeLights) {
        if (nl == 8) break;
        const D3DLIGHT8& l = lights.at(index);
        LightCB& o = cb.lights[nl++];
        std::memcpy(o.diffuse, l.diffuse, 16), std::memcpy(o.specular, l.specular, 16);
        std::memcpy(o.ambient, l.ambient, 16);
        const D3DMATRIX& V = transforms[TS_VIEW];
        for (int c = 0; c < 3; ++c) {
            o.pos[c] = l.position[0] * V.m[0][c] + l.position[1] * V.m[1][c] + l.position[2] * V.m[2][c] + V.m[3][c];
            o.dir[c] = l.direction[0] * V.m[0][c] + l.direction[1] * V.m[1][c] + l.direction[2] * V.m[2][c];
        }
        o.pos[3] = f32(l.type), o.dir[3] = l.range;
        o.att[0] = l.attenuation0, o.att[1] = l.attenuation1, o.att[2] = l.attenuation2, o.att[3] = l.falloff;
        o.spot[0] = std::cos(l.theta * 0.5f), o.spot[1] = std::cos(l.phi * 0.5f);
    }
    cb.lightCount[0] = s32(nl);

    const std::vector<u32> pkey = {
        u32(topo), u32(dt != nullptr),
        rs[RS_ALPHABLENDENABLE], rs[RS_SRCBLEND], rs[RS_DESTBLEND], rs[RS_BLENDOP], rs[RS_COLORWRITEENABLE],
        rs[RS_ZENABLE], rs[RS_ZWRITEENABLE], rs[RS_ZFUNC], rs[RS_STENCILENABLE], rs[RS_STENCILFUNC],
        rs[RS_STENCILMASK], rs[RS_STENCILWRITEMASK], rs[RS_STENCILFAIL], rs[RS_STENCILZFAIL], rs[RS_STENCILPASS],
        rs[RS_CULLMODE], rs[RS_FILLMODE], rs[RS_SOLIDOFFSETENABLE], rs[RS_POLYGONOFFSETZOFFSET],
        rs[RS_POLYGONOFFSETZSLOPESCALE], rs[RS_FRONTFACE],
    };

    const u32 vertByteOffset = u32(d.frameVerts.size() * sizeof(CVertex));
    const u32 idxByteOffset = u32(d.frameIndices.size() * sizeof(u32));
    d.frameVerts.insert(d.frameVerts.end(), d.scratchVerts.begin(), d.scratchVerts.end());
    d.frameIndices.insert(d.frameIndices.end(), d.scratchIdx.begin(), d.scratchIdx.end());

    const Resource* ctKey = d.ColorTargetKey(renderTarget);
    Impl::PassRecord& pass = d.ActivePass(ct, dt, ctKey);

    Impl::DrawItem item;
    item.pipeline = d.GetDrawPipeline(pkey, topo, dt != nullptr, rs);
    const f32 sx = f32(ct->physicalWidth) / ct->width, sy = f32(ct->physicalHeight) / ct->height;
    item.viewport = {viewport.X * sx, viewport.Y * sy, viewport.Width * sx, viewport.Height * sy,
                     viewport.MinZ, viewport.MaxZ};
    f32 bf[4];
    Color(rs[RS_BLENDCOLOR], bf);
    item.blendColor = {bf[0], bf[1], bf[2], bf[3]};
    item.stencilRef = u8(rs[RS_STENCILREF]);
    item.vertByteOffset = vertByteOffset;
    item.idxByteOffset = idxByteOffset;
    item.idxCount = u32(d.scratchIdx.size());
    item.drawCB = cb;
    std::memcpy(item.bindings, bindings, sizeof bindings);
    pass.items.push_back(std::move(item));

    d.pendingTargetWrites.insert(ctKey);
    ++draws_;
}

}  // namespace fut::render::xd3d

// The D3D8 library state; see XD3DDevice.h.
#include "render/xd3d/XD3DDevice.h"

#include "core/XbeImage.h"

#include <algorithm>
#include <cstring>

namespace fut::render::xd3d {

namespace {

// (H) InitializeD3dState's default render states 0x39..0xA5 (u32 table 0x002427E8 + 4 (state - 0x39); 0xDEADBEEF at
// 0x7E, 0x7F, 0x9A, which it skips) and texture-stage states (u8 table 0x002427C8): the XDK D3D8 library's data,
// read from the user's default.xbe (core/XbeImage.h).
constexpr u32 kDefaultRsTable = 0x002427E8, kDefaultTssTable = 0x002427C8;

const char* const kPrefix[5] = {"vb", "ib", "tex", "surf", "pal"};

u32 PrefixIndex(ResourceKind k) {
    switch (k) {
    case ResourceKind::kVertexBuffer: return 0;
    case ResourceKind::kIndexBuffer: return 1;
    case ResourceKind::kTexture:
    case ResourceKind::kCubeTexture: return 2;
    case ResourceKind::kSurface: return 3;
    case ResourceKind::kPalette: return 4;
    }
    return 0;
}

}  // namespace

XD3DDevice::XD3DDevice() {
    // The frame buffers Direct3D_CreateDevice makes for App::Init's 640x480 (back buffer D3DFMT_LIN_X8R8G8B8 0x1E,
    // depth D3DFMT_LIN_D24S8 0x2A: 0x000D276A / 0x0014C200): surf1 / surf2, the first resources (as in the oracle).
    for (int i = 0; i < 2; ++i) {
        Resource* s = NewResource(ResourceKind::kSurface);
        s->width = 640, s->height = 480, s->format = i == 0 ? 0x1E : 0x2A, s->levels = 1;
        frameBuffers_[i] = s;
    }
    renderTarget = frameBuffers_[0];
    depthStencil = frameBuffers_[1];
}

XD3DDevice::~XD3DDevice() = default;

Resource* XD3DDevice::NewResource(ResourceKind kind) {
    auto r = std::make_unique<Resource>();
    r->kind = kind;
    const u32 p = PrefixIndex(kind);
    r->id = std::string(kPrefix[p]) + std::to_string(++counts_[p]);
    resources_.push_back(std::move(r));
    return resources_.back().get();
}

Resource* XD3DDevice::FindResource(const void* p, ResourceKind kind) const {
    const u8* b = static_cast<const u8*>(p);
    for (const auto& r : resources_)
        if (r->kind == kind && !r->data.empty() && b >= r->data.data() && b < r->data.data() + r->data.size())
            return r.get();
    return nullptr;
}

void XD3DDevice::InitializeState() {                  // orig: 0x00239660 InitializeD3dState
    D3DMATRIX identity{};
    for (int i = 0; i < 4; ++i) identity.m[i][i] = 1.0f;   // 0x00242410
    for (u32 t = 0; t < kNumTransforms; ++t) SetTransform(t, identity);
    const xbe::XbeImage& image = xbe::GameImage();
    for (u32 s = 0x39; s < 0xA6; ++s) {
        if (s == 0x7E || s == 0x7F || s == 0x9A) continue;
        const u32 v = image.Word(kDefaultRsTable + 4 * (s - 0x39));
        if (s < kFirstDeferredState) SetRenderState_Simple(s, v);
        else if (s >= kFirstComplexState) SetRenderState_Complex(s, v);
        else renderState[s] = v;                      // SetRenderStateNotInline of a deferred state: the store
    }
    SetRenderState_Complex(RS_ZENABLE, depthStencil ? 1u : 0u);   // device+0x15CC != 0
    for (u32 stage = 0; stage < kNumStages; ++stage) {
        for (u32 i = 0; i < kNumTextureStates; ++i) textureState[stage][i] = image.Byte(kDefaultTssTable + i);
        textureState[stage][TSS_TEXCOORDINDEX] = stage;
        SetTexture(stage, nullptr);
    }
    textureState[0][TSS_COLOROP] = 4;
    textureState[0][TSS_ALPHAOP] = 2;
    SetScreenSpaceOffset(0.0f, 0.0f);
}

Resource* XD3DDevice::CreateVertexBuffer2(u32 length) {
    Resource* r = NewResource(ResourceKind::kVertexBuffer);
    r->data.assign(length, 0);                        // (contiguous memory: zeroed by the oracle's allocator)
    return r;
}

Resource* XD3DDevice::CreateTexture2(u32 width, u32 height, u32 levels, u32 usage, u32 format) {
    Resource* r = NewResource(ResourceKind::kTexture);
    r->width = width, r->height = height, r->levels = levels, r->format = format;
    r->rendered = (usage & 1) != 0;
    size_t bytes = 0;                                 // (every level in the format's layout)
    for (u32 l = 0, w = width, h = height; l < std::max(levels, 1u); ++l, w = std::max(w / 2, 1u), h = std::max(h / 2, 1u))
        bytes += LevelBytes(format, w, h);
    r->data.assign(bytes, 0);
    return r;
}

u8* XD3DDevice::LockRect(Resource* surface) {
    if (!surface || !surface->parent) return nullptr;
    const Resource& t = *surface->parent;
    ++surface->parent->revision;                      // (the caller writes the texels)
    size_t at = 0;                                    // the level's offset: the levels before it
    for (u32 l = 0, w = t.width, h = t.height; l < surface->level; ++l, w = std::max(w / 2, 1u), h = std::max(h / 2, 1u))
        at += LevelBytes(t.format, w, h);
    return at < t.data.size() ? surface->parent->data.data() + at : nullptr;
}

Resource* XD3DDevice::CreateStandAloneSurface(u32 width, u32 height, u32 format) {
    Resource* r = NewResource(ResourceKind::kSurface);
    r->width = width, r->height = height, r->format = format, r->levels = 1;
    return r;
}

Resource* XD3DDevice::GetSurfaceLevel2(Resource* texture, u32 level) {
    if (!texture) return nullptr;
    Resource* r = NewResource(ResourceKind::kSurface);
    r->parent = texture;
    r->level = level;
    r->width = std::max(texture->width >> level, 1u), r->height = std::max(texture->height >> level, 1u);
    r->format = texture->format, r->levels = 1;
    ++texture->refs;                                  // (the surface holds its texture)
    return r;
}

Resource* XD3DDevice::CreateIndexBuffer2(u32 length) {
    Resource* r = NewResource(ResourceKind::kIndexBuffer);
    r->data.assign(length, 0);
    return r;
}

u8* XD3DDevice::Lock2(Resource* vb, u32) { return vb ? vb->data.data() : nullptr; }
void XD3DDevice::GetDesc(Resource*) {}
void XD3DDevice::AddRef(Resource* r) {
    if (r) ++r->refs;
}
void XD3DDevice::Release(Resource* r) {
    if (r && r->refs) --r->refs;
}

void XD3DDevice::SetRenderTarget(Resource* color, Resource* depth) {
    renderTarget = color;
    depthStencil = depth;
    for (Resource* r : {color, depth})
        if (r && r->parent) r->parent->rendered = true;
}

void XD3DDevice::SetViewport(const D3DVIEWPORT8& vp) {
    viewport = vp;
    viewportSet = true;
}
void XD3DDevice::Clear(u32, const D3DRECT*, u32, u32, f32, u32) {}
void XD3DDevice::Swap(u32) {}
void XD3DDevice::SetScreenSpaceOffset(f32 x, f32 y) { screenSpaceOffset[0] = x, screenSpaceOffset[1] = y; }

void XD3DDevice::SetTransform(u32 type, const D3DMATRIX& m) {
    if (type >= kNumTransforms) return;
    transforms[type] = m;
    transformSet[type] = true;
}

void XD3DDevice::SetMaterial(const D3DMATERIAL8& m) {
    material = m;
    materialSet = true;
}
void XD3DDevice::SetLight(u32 index, const D3DLIGHT8& l) { lights[index] = l; }
void XD3DDevice::LightEnable(u32 index, bool enable) { lightEnabled[index] = enable; }

void XD3DDevice::SetTexture(u32 stage, Resource* texture) {
    if (stage < kNumStages) textures[stage] = texture;
}

void XD3DDevice::SetStreamSource(u32 stream, Resource* vb, u32 stride) {
    if (stream != 0) return;
    streamSource = vb;
    streamStride = stride;
}

void XD3DDevice::SetIndices(Resource* ib, u32 base) {   // D3D__IndexData = ib->Data
    indices = ib;
    baseVertex = base;
    indexData = ib ? reinterpret_cast<const u16*>(ib->data.data()) : nullptr;
}

void XD3DDevice::SetVertexShader(u32 handle) { vertexShader = handle; }
void XD3DDevice::SetPixelShader(u32 handle) { pixelShader = handle; }
void XD3DDevice::SetRenderState_Simple(u32 state, u32 value) {
    if (state < kNumRenderStates) renderState[state] = value;
}
void XD3DDevice::SetRenderState_Complex(u32 state, u32 value) {
    if (state == RS_ZBIAS) {                          // D3DDevice_SetRenderState_ZBias 0x00235E80 (H)
        // f = -(float)(s32)value (+2^32 [0x00295640] when negative); POLYGONOFFSETZSLOPESCALE = f * 0.25f
        // [0x00293B50], POLYGONOFFSETZOFFSET = f, the three offset enables = value != 0 (SetRenderStateNotInline)
        double f = double(s32(value));
        if (s32(value) < 0) f += 4294967296.0;
        f = -f;
        const f32 offset = static_cast<f32>(f);
        const f32 slope = static_cast<f32>(double(offset) * 0.25f);
        std::memcpy(&renderState[RS_POLYGONOFFSETZSLOPESCALE], &slope, 4);
        std::memcpy(&renderState[RS_POLYGONOFFSETZOFFSET], &offset, 4);
        renderState[RS_POINTOFFSETENABLE] = renderState[RS_WIREFRAMEOFFSETENABLE] =
            renderState[RS_SOLIDOFFSETENABLE] = value != 0;
    }
    if (state < kNumRenderStates) renderState[state] = value;
}
void XD3DDevice::SetTextureState_TexCoordIndex(u32 stage, u32 value) {
    if (stage < kNumStages) textureState[stage][TSS_TEXCOORDINDEX] = value;
}
void XD3DDevice::SetTextureState_BumpEnv(u32 stage, u32 state, u32 value) {
    if (stage < kNumStages && state < kNumTextureStates) textureState[stage][state] = value;
}
void XD3DDevice::DrawVertices(u32, u32, u32) {}
void XD3DDevice::DrawIndexedVertices(u32, u32, const u16*) {}

}  // namespace fut::render::xd3d

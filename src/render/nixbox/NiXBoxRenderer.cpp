// NiXBoxRenderer; see NiXBoxRenderer.h. x87 arithmetic in the original's order (double = the extended register,
// F() = a float store).
#include "render/nixbox/NiXBoxRenderer.h"

#include "core/CrtQsort.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiObjects.h"

#include <algorithm>
#include <cmath>
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

// orig: _ftol 0x00167110 (truncation toward zero)
u32 Ftol(double v) { return static_cast<u32>(static_cast<s32>(v)); }

// (H) the index count of n primitives: u32 pairs {multiplier, addend} per D3DPRIMITIVETYPE at 0x00293A38.
constexpr u32 kPrimitiveIndices[11][2] = {{0, 0}, {1, 0}, {2, 0}, {1, 1}, {1, 1}, {3, 0},
                                          {1, 2}, {1, 2}, {4, 0}, {2, 2}, {0, 0}};

}  // namespace

NiXBoxRenderer::NiXBoxRenderer(XD3DDevice& device) : d3d_(device) {
    // orig: 0x0014C200 / Direct3D_CreateDevice (InitializeD3dState) / 0x0014AB40 (the frame buffers' descs)
    d3d_.InitializeState();
    backBuffer_.width = 640, backBuffer_.height = 480;
    backBuffer_.color = d3d_.backBuffer();
    backBuffer_.depth = d3d_.depthBuffer();
    backBuffer_.format = 0x1E;                        // (L: the back buffer's (+8)+0x24, D3DFMT_LIN_X8R8G8B8)
    clearViewport_ = {0, 0, 640, 480, 0.0f, 1.0f};    // (O: the oracle's clears)
    // 0x001475E0: the vertex / index buffer managers (+0x5B4 / +0x5B8), the group manager (+0x5AC, 0x0015C370) and
    // the renderer's own group +0x5B0 = vf04(1, 0); +0x504 = 0 (no hardware skinning: 0x0014BCC0 is never taken)
    // 0x0015FB20 the texture manager (+0x5BC: minimal), 0x00159CB0 the render state (+0x5C0, with its defaults
    // 0x001594A0), 0x0015BD40 the light manager (+0x5C4), then the group manager and the renderer's own group
    textures_ = std::make_unique<NiXBoxTextureManager>(d3d_);
    state_ = std::make_unique<NiXBoxRenderState>(d3d_);
    lights_ = std::make_unique<NiXBoxLightManager>(d3d_, *state_);
    unshared_ = static_cast<NiXBoxGeometryGroup*>(CreateGeometryGroup(1));
    // the factory 0x0014C200 then: 0x0015B690 the texture pipeline manager (+0x5C8)
    pipeline_ = std::make_unique<NiXBoxTexturePipeline>(d3d_, *state_, *textures_);
    NotPorted("NiXBoxRenderer factory 0x0014C200: the pixel shader 0x001477D0, the default camera data");
}

bool NiXBoxRenderer::PrecacheTexture(ni::NiSourceTexture& texture) {   // orig: 0x001473E0
    return textures_ ? textures_->Precache(texture) : true;
}

void NiXBoxRenderer::SetSourceTextureD3D(ni::NiSourceTexture& texture, xd3d::Resource* d3d) {
    if (textures_) textures_->Replace(texture, d3d);
}

void NiXBoxRenderer::PurgeTexture(const ni::NiTexture& texture) {   // orig: 0x00118490 (+0x20 vf00)
    if (textures_) textures_->Purge(texture);
    if (auto it = renderedTextures_.find(static_cast<const ni::NiRenderedTexture*>(&texture));
        it != renderedTextures_.end()) {
        if (current_ == it->second.get()) current_ = &backBuffer_;   // (a UB guard: the target dies)
        if (it->second->texture) d3d_.Release(it->second->texture);   // (the data's AddRef)
        renderedTextures_.erase(it);
    }
}

NiXBoxRenderer::~NiXBoxRenderer() {
    // (the groups go first: renderer data that outlives the renderer is left without a group, see ~NiXBoxGeometryGroup)
    for (auto& bucket : pending_) bucket.clear();
    groups_.clear();
}

// ---- frame -----------------------------------------------------------------------------------------------------
void NiXBoxRenderer::ClearBuffer(const f32* rect, u32 flags) {   // orig: 0x00146520
    const u32 w = current_->width, h = current_->height;
    D3DRECT r{0, 0, s32(w), s32(h)};
    // 0x00158A20: the swizzled formats (a swizzled target is cleared whole: the rect is ignored)
    auto swizzled = [](u32 f) {
        for (u32 k : {6u, 7u, 5u, 3u, 2u, 4u, 0x19u, 0x3Au, 0x3Bu, 0x39u, 0x38u, 0x3Cu, 0x29u, 0x28u, 0u, 0x1Au, 1u,
                      0x32u, 0x2Cu, 0x2Au, 0x2Du, 0x2Bu})
            if (f == k) return true;
        return false;
    };
    if (rect && !swizzled(current_->format)) {
        const f32 hf = F(double(h));                  // FSTP [esp+0x24]
        r.x1 = s32(Ftol(double(w) * rect[0]));
        r.y1 = s32(Ftol((1.0 - rect[2]) * hf));
        r.x2 = s32(Ftol(double(w) * rect[1]));
        r.y2 = s32(Ftol((1.0 - rect[3]) * hf));
    }
    u32 d = (flags & 1) ? 0xF0u : 0u;                 // D3DCLEAR_TARGET
    if ((flags & 4) && current_->depth) d |= 1;       // ZBUFFER
    if ((flags & 2) && (flags2D8_ & 0x80) && current_->depth) d |= 2;   // STENCIL
    clearViewport_.Width = w;                         // +0x41C / +0x420
    clearViewport_.Height = h;
    d3d_.SetViewport(clearViewport_);
    d3d_.Clear(1, &r, d, backgroundColor_, depthClear_, stencilClear_);
    field64_ = 0;
}

void NiXBoxRenderer::DisplayFrame() {                 // orig: 0x001464F0
    if (current_->texture != nullptr) return;         // (+0x58)+0x10: a rendered texture is not presented
    d3d_.Swap(0);
    field64_ = 0;
    ++vbFrame_;                                       // 0x00157A00: the vertex buffer manager's frame +0x20
}

void NiXBoxRenderer::SetBackgroundColor(const ni::NiColor& c) {   // orig: 0x0014B150
    u32 v = (Ftol(double(c.r) * 255.0f) & 0xFF) | 0xFFFFFF00u;   // [0x002A1564] = 255.0f
    v = (v << 8) | (Ftol(double(c.g) * 255.0f) & 0xFF);
    v = (v << 8) | (Ftol(double(c.b) * 255.0f) & 0xFF);
    backgroundColor_ = v;
}

bool NiXBoxRenderer::SetRenderTarget(ni::NiRenderedTexture* texture) {   // orig: 0x00146490
    if (inScene_) return false;                       // +0x500
    TargetSet* t = &backBuffer_;                      // +0x14
    if (!texture) {
        d3d_.SetScreenSpaceOffset(0.0f, 0.0f);        // 0x000A19C0: (DAT_002E79C8, DAT_002E79CC) (L: 0, 0, as observed)
    } else {
        auto it = renderedTextures_.find(texture);    // texture +0x20
        if (it == renderedTextures_.end()) return false;
        t = it->second.get();                         // the data's vf30 (+0x80)
        d3d_.SetScreenSpaceOffset(0.0f, 0.0f);        // 0x000A19B0
    }
    d3d_.SetRenderTarget(t->color, t->depth);         // (+0x14, +0x20)
    current_ = t;                                     // +0x58
    return true;
}

bool NiXBoxRenderer::CreateRenderedTextureData(ni::NiRenderedTexture& texture) {   // orig: 0x00147410
    if (renderedTextures_.count(&texture)) return true;   // texture +0x20 set
    // 0x0015E4C0 (texture, renderer): new(0x84) (0x00157750, 0x00157110), then 0x0015E400: CreateTexture2(width,
    // height, 1, 1 level, usage 1 (D3DUSAGE_RENDERTARGET), format, type 3) with the format (renderer +0x53C)+0x24
    // ((O) 6, D3DFMT_A8R8G8B8; the texture's format preferences are read but not used for it)
    const u32 w = texture.width, h = texture.height;
    xd3d::Resource* tex = d3d_.CreateTexture2(w, h, 1, 1, 6);
    if (!tex) return false;                           // (the error text 0x002ABAD0, the data deleted)
    auto t = std::make_unique<TargetSet>();           // new(0x24): +0xC = +0x18 = 0
    // the depth surface: D3D_CreateStandAloneSurface(width, height, 2, format) with the renderer's depth format +0x38
    // (D3DFMT_LIN_D24S8 0x2A), swizzled by 0x00158AA0 since 0x00158A20 lists it: 0x2A -> 0x2E
    t->depth = d3d_.CreateStandAloneSurface(w, h, 0x2E);   // +0x20
    t->width = w, t->height = h;                      // +0 / +4 (texture vf2C / vf30)
    t->format = tex->format;                          // +8 = the NiRenderedTexture (L: its +0x24 the texture's format)
    t->texture = tex;                                 // +0x10 = the data's vf18, AddRef
    d3d_.AddRef(tex);
    t->color = d3d_.GetSurfaceLevel2(tex, 0);         // +0x14 (the error text 0x002ABB18 when null)
    textures_->rendered[&texture] = tex;              // (the texture manager's view of it: the stage apply binds it)
    // (+0x18 released and cleared, +0x1C = 0; the texture manager's registration 0x0015DA40: NOT PORTED, no D3D)
    renderedTextures_[&texture] = std::move(t);
    return true;
}

void NiXBoxRenderer::BeginScene() {                   // orig: 0x001489E0
    PackPending();                                    // 0x00148390
    state_->BeginScene();                             // 0x00158C20
    lights_->BeginScene();                            // 0x0015BA20
    // (0x00157840: the vertex buffer manager's statistics, no D3D)
    pipeline_->BeginScene();                          // 0x00159F80
    // (+0x5CC: null, DAT_0030EE74 = 0)
    inScene_ = true;
}

void NiXBoxRenderer::EndScene() {                     // orig: 0x00146A50
    if (inScene_) inScene_ = false;
}

void NiXBoxRenderer::SetCameraData(const ni::NiPoint3& E, const ni::NiPoint3& D, const ni::NiPoint3& U,
                                   const ni::NiPoint3& R, const f32 f[6], const f32 port[4]) {   // orig: 0x00146A70
    // the view matrix +0x610: the columns R, U, D and the eye's projections
    view_ = {};
    view_.m[0][0] = R.x, view_.m[0][1] = U.x, view_.m[0][2] = D.x;
    view_.m[1][0] = R.y, view_.m[1][1] = U.y, view_.m[1][2] = D.y;
    view_.m[2][0] = R.z, view_.m[2][1] = U.z, view_.m[2][2] = D.z;
    view_.m[3][0] = F(-((double(E.x) * R.x + double(R.z) * E.z) + double(E.y) * R.y));
    view_.m[3][1] = F(-((double(E.z) * U.z + double(E.x) * U.x) + double(E.y) * U.y));
    view_.m[3][2] = F(-((double(E.y) * D.y + double(E.x) * D.x) + double(E.z) * D.z));
    view_.m[3][3] = 1.0f;
    d3d_.SetTransform(TS_VIEW, view_);
    camRight1_ = R, camRight0_ = R;                   // +0x314, +0x2FC
    camUp1_ = U, camUp0_ = U;                         // +0x320, +0x308
    pipeline_->SetCameraData(D, U, R, E);             // 0x00159F00 @0x00146BD6
    // the projection +0x650 from the frustum (l, r, t, b, n, f)
    nearPlane_ = f[4];                                // +0x34C
    const double range = double(f[5]) - f[4];         // FST +0x350: the register keeps the unrounded difference
    depthRange_ = F(range);
    const f32 rl = F(double(f[1]) - f[0]), lr = F(double(f[0]) + f[1]);
    const f32 tb = F(double(f[2]) - f[3]), tpb = F(double(f[2]) + f[3]);
    projection_ = {};
    const double c = 1.0 / range;                     // FDIVR [0x00293A98] = 1.0f
    const double a = 1.0 / double(rl);
    if (stateFlip1FC_ == 0) {
        projection_.m[0][0] = F(a + a);
        projection_.m[2][0] = F(-(a * lr));
    } else {
        projection_.m[0][0] = F(-2.0 * a);            // [0x00293BFC] = -2.0f
        projection_.m[2][0] = F(a * lr);
    }
    const double b = 1.0 / double(tb);
    projection_.m[1][1] = F(b + b);
    projection_.m[2][1] = F(-(b * tpb));
    projection_.m[2][2] = F(c * f[5]);
    proj33_ = projection_.m[2][2];                    // +0x3BC
    projection_.m[2][3] = 1.0f;
    projection_.m[3][2] = F(-((double(f[5]) * f[4]) * c));
    proj43_ = projection_.m[3][2];                    // +0x3CC
    d3d_.SetTransform(TS_PROJECTION, projection_);
    // the viewport from the port and the current target's size (+0x58)
    const double w = double(current_->width), h = double(current_->height);   // FILD (+2^32 when negative)
    D3DVIEWPORT8 vp;
    vp.X = Ftol(w * port[0]);
    vp.Y = Ftol((1.0 - port[2]) * h);
    vp.Width = Ftol((double(port[1]) - port[0]) * w);
    vp.Height = Ftol((double(port[2]) - port[3]) * h);
    vp.MinZ = 0.0f, vp.MaxZ = 1.0f;
    d3d_.SetViewport(vp);
    state_->nearPlane = f[4];                         // NiXBoxRenderState +0x178 / +0x17C / +0x180
    state_->farPlane = f[5];
    state_->range = F(double(f[5]) - f[4]);
}

// ---- the accumulator (the game's NiAlphaAccumulator) -----------------------------------------------------------
// Click 0x00100D86..: the accumulator +8 (AddRef'd) gets vf2C; false releases renderer +8.
bool NiXBoxRenderer::StartAccumulating(const ni::NiCamera& camera) {
    if (!accumulator_) return false;
    if (!accumulator_->StartAccumulating(camera)) {
        accumulator_ = nullptr;
        return false;
    }
    return true;
}

void NiXBoxRenderer::FinishAccumulating(ni::NiCamera& camera) {   // Click 0x00100DDD: renderer +8 vf30
    if (accumulator_) accumulator_->FinishAccumulating(camera);
}

bool NiXBoxRenderer::Accumulator::StartAccumulating(const ni::NiCamera& c) {   // orig: 0x0010EB50
    return NiAccumulator::StartAccumulating(c);       // NiAccumulator 0x00140F70 (+8; +0x14 AddRef)
}

bool NiXBoxRenderer::Accumulator::RegisterObject(ni::NiGeometry& geometry) {   // orig: 0x0002ADE0
    if (!camera) return false;
    (void)geometry;
    const ni::NiPropertyState* ps = renderer.propertyState;   // (renderer+0xC)+8
    const auto* alpha = ps ? static_cast<const ni::NiAlphaProperty*>(ps->slot[ni::NiPropertyState::kAlpha].Get()) : nullptr;
    if (!alpha || !(alpha->flags & 1)) return false;  // no blending: drawn at once
    // (each entry keeps the camera's cull mask +0x228: only the display hint 0x000FF6A0 reads it, a no-op slot)
    if (unsortedFlag && ((alpha->flags >> 13) & 1)) unsorted.push_back(&geometry);   // 0x0002AED0
    else sorted.push_back(&geometry);                 // 0x0002B0D0 (appended at the tail)
    return true;
}

void NiXBoxRenderer::Accumulator::FinishAccumulating(ni::NiCamera& cam) {   // orig: 0x0002A590
    if (!camera) return;
    NiXBoxRenderer& r = renderer;
    for (ni::NiGeometry* g : unsorted) r.RenderImmediate(*g, cam);   // (with the entry's cull mask)
    unsorted.clear();                                 // +0x38 = +0x3C = 0
    // orig: NiClusterAccumulator::FinishAccumulating 0x0010ED00: the sorter at +0x10 (vtable 0x002AAC90; its +4 is
    // the accumulator's camera +0x14): vf04 0x00150C80 gives each list entry its depth and sorts them (the CRT qsort
    // 0x001679F0 with 0x00150B50: ascending), vf08 0x00150B90 / vf0C 0x00150BA0 hand them out from the end
    // (farthest first), each drawn by 0x0010EA50 (with the entry's cull mask: only the no-op 0x000FF6A0 reads it).
    struct Item {                                     // 8 bytes: the entry, its depth (+4)
        u32 entry;
        f32 depth;
    };
    static_assert(sizeof(Item) == 8);
    std::vector<Item> items(sorted.size());
    const ni::NiPoint3 eye = camera->world.translate;   // camera +0xA0
    const ni::NiPoint3 dir = camera->worldDir;         // camera +0x110
    for (size_t i = 0; i < sorted.size(); ++i) {
        const ni::NiBound& b = sorted[i]->worldBound;   // +0x20 centre, +0x2C radius
        const double dx = double(b.center.x) - eye.x, dy = double(b.center.y) - eye.y;
        const double dz = double(b.center.z) - eye.z;
        const double s = ((dz * dir.z + dy * dir.y) + dx * dir.x) + b.radius;
        double v = (s > 0.0 || std::isnan(s)) ? s : double(b.radius);   // FCOM [0x00293AB8] = 0.0f
        if (b.radius > 100.0f) v += 10000.0;          // [0x00295610] = 100.0f, [0x0029E7FC] = 10000.0f
        items[i] = {u32(i), F(v)};
    }
    if (items.size() >= 2)
        crt::Qsort(items.data(), items.size(), sizeof(Item), [](const void* a, const void* b) -> int {
            const f32 x = static_cast<const Item*>(a)->depth, y = static_cast<const Item*>(b)->depth;   // 0x00150B50
            if (x < y) return -1;
            return x > y ? 1 : 0;
        });
    for (size_t i = items.size(); i-- > 0;) r.RenderImmediate(*sorted[items[i].entry], cam);
    sorted.clear();                                   // 0x0010EC50 per entry
    camera = nullptr;                                 // +0x14 released, 0x00031E40 (+8 = 0)
}

// orig: 0x00140310: the object's display with renderer+8 (the accumulator) taken away, so it is drawn at once.
void NiXBoxRenderer::RenderImmediate(ni::NiGeometry& geometry, const ni::NiCamera& camera) {
    ni::NiAccumulator* saved = accumulator_;
    accumulator_ = nullptr;
    if (auto* shape = dynamic_cast<ni::NiTriShape*>(&geometry)) DisplayTriShape(*shape, camera);   // vf84
    else if (auto* st = dynamic_cast<ni::NiTriStrips*>(&geometry)) DisplayTriShape(*st, camera);   // 0x0011C4F0
    else if (auto* p = dynamic_cast<ni::NiParticles*>(&geometry)) DisplayParticles(*p, camera);
    else NotPorted("NiXBoxRenderer::RenderImmediate: a geometry class other than NiTriShape / NiTriStrips / NiParticles");
    accumulator_ = saved;
}

// ---- display ---------------------------------------------------------------------------------------------------
// A geometry displayed before any UpdateProperties reached it: an error (a game call site not ported; the original
// would read a null +0xC0). Reported; it is drawn with the class-default property state (a UB guard, not the
// original's behaviour). Without UpdateEffects the effect state stays null (no lights, no texture effects), as in the
// original.
void NiXBoxRenderer::EnsureRenderData(ni::NiGeometry& g) {
    if (g.propertyState) return;
    NotPorted("NiXBoxRenderer: ERROR: a geometry without render data (an UpdateProperties call site not ported)");
    g.propertyState = new ni::NiPropertyState;
}

void NiXBoxRenderer::DisplayTriShape(ni::NiGeometry& geometry, const ni::NiCamera&) {   // orig: 0x00116760
    EnsureRenderData(geometry);
    propertyState = geometry.propertyState.Get();     // renderer +0xC / +0x10 = the geometry's +0xC0 / +0xC4
    effectState = geometry.effectState.Get();
    if (accumulator_ && accumulator_->RegisterObject(geometry)) return;   // 0x001401D0 (renderer+8 vf34)
    // 0x0013EC90: each of the geometry's controllers' vf40 (the UV controllers apply their keys to the data)
    for (ni::NiTimeController* c = geometry.controller.Get(); c; c = c->nextControllerPtr.Get()) c->OnPreDisplay();
    // 0x000FF6A0: renderer vfE0 = 0x00151E50, a no-op
    DrawTriShape(geometry);                           // vfC0 (data +0xC8, skin +0xCC, world +0x70, bound +0x20)
    // 0x001167D7: an unskinned shape with DAT_0030EBE0 set (NiOutline's display) writes its world vertices there and
    // clears it; then the frame statistics 0x0030EC1C / 0x0030EC20 / 0x0030EC24
    if (!geometry.skinInstance && ni::g_outlineScratch) {
        ni::WriteOutlineWorldVertices(geometry, ni::g_outlineScratch);
        ni::g_outlineScratch = nullptr;
    }
}

void NiXBoxRenderer::DisplayParticles(ni::NiGeometry& particles, const ni::NiCamera&) {   // orig: 0x0010F8B0
    EnsureRenderData(particles);
    propertyState = particles.propertyState.Get();    // renderer +0xC / +0x10 = the geometry's +0xC0 / +0xC4
    effectState = particles.effectState.Get();
    if (accumulator_ && accumulator_->RegisterObject(particles)) return;   // 0x001401D0 (renderer+8 vf34)
    for (ni::NiTimeController* c = particles.controller.Get(); c; c = c->nextControllerPtr.Get())
        c->OnPreDisplay();                            // 0x0013EC90: each controller's vf40
    // 0x000FF6A0: renderer vfE0 = 0x00151E50, a no-op
    if (!particles.modelData) return;                 // (the original hands the null data to vf52, which calls it)
    DrawParticles(*particles.modelData, particles.world, particles.worldBound);   // vfD0 (+0xC8, +0x70, +0x20)
}

void NiXBoxRenderer::DrawTriShape(ni::NiGeometry& geometry) {   // orig: 0x0014C580
    if (!inScene_) return;
    ni::NiGeometryData* data = geometry.modelData.Get();
    if (!data || data->vertexCount == 0) return;      // data vf30 (the active vertex count)
    AddData(*unshared_, *data);                       // group manager vf18 (+0x5AC, +0x5B0, data, skin, 0)
    // (skin && +0x504 >= 4: the hardware-skinned partitions 0x0014BCC0; +0x504 is always 0)
    auto* rd = static_cast<NiGeometryBufferData*>(data->rendererData.get());
    if (rd) DrawGeometry(data, geometry.skinInstance.Get(), geometry.world, *rd);   // 0x00149B70(.., data+0x30, 0)
}

// ---- screen textures --------------------------------------------------------------------------------------------
void NiXBoxRenderer::DrawScreenTexture(ni::NiScreenTexture& t) {   // orig: 0x0014C600
    if (!inScene_ || t.rects.empty()) return;         // +0x500; +0x10
    // +0xC kept as a plain pointer (esp+0x18: no reference; it may be a released geometry's state, e.g. a text
    // string removed after its draw)
    ni::NiPropertyState* const saved = propertyState;
    // the property state +0x5A8 (new each call, released at the end; 0x0014D800): NiVertexColorProperty (0x00011A10:
    // source 1, lighting 0), NiAlphaProperty (0x00013D80: flags (f & 0xFEED) | 0xED), NiZBufferProperty (0x0003F460:
    // flags & ~3), the renderer-specific property 0x001176A0 / 0x001463B0 with the holder 0x00147C30 / 0x00147CD0 of a
    // new pipeline object (0x001602B0: it precaches a 2x1 clipper texture of its own each call); then the texture's
    // texturing property +0x14 (renderer +0xC = the state)
    {
        auto* ps = new ni::NiPropertyState;           // 0x0014D800
        auto* vc = new ni::NiVertexColorProperty;     // 0x00011A10: source 1 (EMISSIVE), lighting 0 (E)
        vc->sourceVertexMode = 1, vc->lightingMode = 0;
        ps->slot[ni::NiPropertyState::kVertexColor] = vc;
        auto* a = new ni::NiAlphaProperty;            // 0x00013D80: (flags & 0xFEED) | 0xED
        a->flags = u16((0xEC & 0xFEED) | 0xED);
        ps->slot[ni::NiPropertyState::kAlpha] = a;
        auto* z = new ni::NiZBufferProperty;          // 0x0003F460: flags & ~3
        z->flags = u16(3 & ~3u);
        ps->slot[ni::NiPropertyState::kZBuffer] = z;
        auto* rsp = new NiXBoxPipelineProperty;       // new(0x1C) 0x001176A0, +0x18 = the holder (0x001463B0)
        rsp->pipeline = pipeline_->MakeScreenTexturePipeline();   // new(0x2FE0) 0x001602B0(renderer)
        ps->slot[ni::NiPropertyState::kRendererSpecific] = rsp;
        if (t.texturing) ps->slot[ni::NiPropertyState::kTexturing] = t.texturing.Get();
        screenTextureState_ = ps;
        propertyState = ps;                           // renderer +0xC
        effectState = nullptr;
    }
    if (!t.rendererData) unshared_->CreateScreenTextureData(t);   // manager vf10 0x0015C4B0(+0x5B0, t, 0)
    PackScreenTexture(t);                             // 0x0014C080
    auto* rd = static_cast<NiGeometryBufferData*>(t.rendererData.get());
    // 0x00149B70(0, 0, the identity transform (0x002D7820, translate 0, scale 1), a zero bound, t +0x1C, 0)
    if (rd) DrawGeometry(nullptr, nullptr, ni::NiTransform{}, *rd);
    // the state released (+0x5A8: its properties; the pipeline object lives on while the manager's +8 holds it)
    screenTextureState_ = nullptr;
    propertyState = saved;                            // +0xC = saved
}

// ---- screen polygons --------------------------------------------------------------------------------------------
void NiXBoxRenderer::DrawScreenPolygon(ni::NiScreenPolygon& poly) {   // orig: 0x0014B680 (slot 54)
    // 0x00106B80: renderer +0xC = the polygon's property state (+8), +0x10 = 0
    propertyState = poly.State();
    effectState = nullptr;
    if (!inScene_ || poly.vertexCount == 0) return;   // +0x500; +0xC
    // +0x32C = +0x33C (the bound for the pipeline)
    if (!PackScreenPolygon(poly)) return;             // 0x00149FC0(poly, +0x34C, +0x350)
    d3d_.SetTransform(TS_VIEW, screenView_);          // +0x354
    d3d_.SetTransform(TS_PROJECTION, ScreenProjection());   // +0x394
    // 0x00149B70(0, 0, the identity transform (0x002D7820, translate 0x002F915C, scale 1), +0x32C, +0x690, 0)
    DrawGeometry(nullptr, nullptr, ni::NiTransform{}, polyData_);
    d3d_.SetTransform(TS_VIEW, view_);                // +0x610
    d3d_.SetTransform(TS_PROJECTION, projection_);    // +0x650
}

xd3d::D3DMATRIX NiXBoxRenderer::ScreenProjection() const {
    // +0x394 (ctor 0x00147EAE..: 2, 2, (-1, -1, +0x3BC, 1), +0x3CC): its +0x3BC / +0x3CC are SetCameraData's
    // projection elements (3, 3) / (4, 3)
    D3DMATRIX m{};
    m.m[0][0] = 2.0f, m.m[1][1] = 2.0f;
    m.m[2][0] = -1.0f, m.m[2][1] = -1.0f, m.m[2][2] = proj33_, m.m[2][3] = 1.0f;
    m.m[3][2] = proj43_;
    return m;
}

bool NiXBoxRenderer::PackScreenPolygon(ni::NiScreenPolygon& poly) {   // orig: 0x00149FC0 (poly, near, range)
    const u16 count = poly.vertexCount;
    const ni::NiPoint3* v = poly.vertices.data();
    const ni::NiPoint2* uv = poly.uvs.empty() ? nullptr : poly.uvs.data();
    const ni::NiColorA* c = poly.colors.empty() ? nullptr : poly.colors.data();
    // the vertex buffer manager's 0x00158630 (count, vertices, uvs, colours, near, range, &fvf, &stride): a dynamic
    // buffer of the ring (the current slot; no fence or advance here) with XYZ (| DIFFUSE) (| TEX1)
    u32 fvf = 2, stride = 12, colorAt = 0, uvAt = 0;  // D3DFVF_XYZ
    if (c) fvf |= 0x40, colorAt = stride, stride = 16;   // D3DFVF_DIFFUSE
    if (uv) fvf |= 0x100, uvAt = stride, stride += 8;    // D3DFVF_TEX1
    Resource* vb = DynamicVertexBuffer(u32(count) * stride);   // 0x001584D0
    if (!vb) return false;
    u8* out = d3d_.Lock2(vb, 0);
    if (!out || vb->data.size() < size_t(count) * stride) return false;   // (UB-avoiding)
    auto put = [](u8* at, f32 x) { std::memcpy(at, &x, 4); };
    for (u16 i = 0; i < count; ++i) {                 // the depth: near + range * z (x87, kept), times x / y
        const double z = double(depthRange_) * v[i].z + nearPlane_;   // FLD range; FMUL z; FADD near
        u8* at = out + size_t(i) * stride;
        put(at, F(z * v[i].x));
        put(at + 4, F(z * v[i].y));
        put(at + 8, F(z));
    }
    if (c)
        for (u16 i = 0; i < count; ++i) {             // A, R, G, B: FISTP(FSTP(c * 255.0f)) [0x002A1564], no masking
            auto byte = [](f32 x) { return static_cast<u32>(static_cast<s32>(std::nearbyint(F(double(x) * 255.0f)))); };
            u32 argb = byte(c[i].a);
            argb = (argb << 8) | byte(c[i].r);
            argb = (argb << 8) | byte(c[i].g);
            argb = (argb << 8) | byte(c[i].b);
            std::memcpy(out + size_t(i) * stride + colorAt, &argb, 4);
        }
    if (uv)
        for (u16 i = 0; i < count; ++i) std::memcpy(out + size_t(i) * stride + uvAt, &uv[i], 8);
    // the renderer's own buffer data +0x690
    NiGeometryBufferData& rd = polyData_;
    rd.flags = 1 & ~0x800000u;                        // +0x694 = 1, normals bit cleared
    rd.flags |= u32(uv != nullptr) << 24;             // one texture set with coordinates
    rd.fvf = fvf;                                     // +0x69C
    rd.stride = stride;                               // +0x6A0
    rd.vertexCount = count;                           // +0x6A4
    if (!polyChip_) polyChip_ = std::make_unique<NiVBChip>();   // +0x6A8: 0x00147D10(0, vb, 0, 0, 0) once
    polyChip_->vb = vb;                               // (+8; +0x14 = 0)
    polyChip_->size = 0;
    rd.chip = polyChip_.get();
    rd.indexCount = 0, rd.ibSize = 0, rd.ib = nullptr, rd.baseVertex = 0;   // +0x6AC..+0x6B8
    rd.primitive = poly.strip ? PT_TRIANGLESTRIP : PT_TRIANGLEFAN;   // +0x6BC: 6 with +0x1E, else 7
    rd.primitiveCount = u32(count) - 2;               // +0x6C0
    rd.stripCount = 1;                                // +0x6C4
    rd.hasStripLengths = false, rd.hasIndices = false;   // +0x6C8 / +0x6CC
    return true;
}

void NiXBoxRenderer::PackScreenTexture(ni::NiScreenTexture& t) {   // orig: 0x0014C080
    auto* rd = static_cast<NiGeometryBufferData*>(t.rendererData.get());
    if (!rd) return;                                  // (the original reads the null data)
    if (rd->chip && (rd->flags & 1)) return;
    const u16 n = u16(t.rects.size());
    const u32 vertices = u16(n * 4);
    rd->vertexCount = vertices;                       // +0x14
    rd->indexCount = (vertices * 3 / 2) & 0xFFFF;     // +0x1C
    rd->primitiveCount = u32(n) << 1;                 // +0x30
    if (!rd->chip) {
        rd->flags = 0x1000000;                        // +4: one texture set
        rd->baseVertex = 0;                           // +0x28
        BuildScreenTexture(t, rd->chip, rd->ib);      // 0x0014A0E0(t, &+0x18, &+0x24)
    } else {
        NiVBChip* chip = rd->chip;
        Resource* ib = nullptr;
        if (t.dirty == 0) {
            ib = rd->ib;
        } else {
            chip = nullptr;
            BuildScreenTexture(t, chip, ib);
        }
        if (rd->chip != chip) {                       // the group's vf24, then the new chip
            if (rd->group) rd->group->FreeChip(*rd);
            rd->chip = chip;
        }
        if (rd->ib != ib) {
            if (rd->ib) d3d_.Release(rd->ib);
            rd->ib = ib;
        }
    }
    t.dirty = 0;                                      // the group's vf28 (0x000AEDD0: false) -> +0x18 = 0
}

void NiXBoxRenderer::BuildScreenTexture(ni::NiScreenTexture& t, NiVBChip*& chipOut, Resource*& ibOut) {
    // orig: 0x0014A0E0
    auto* rd = static_cast<NiGeometryBufferData*>(t.rendererData.get());
    const u16 rects = u16(t.rects.size());
    const u32 count = u32(rects) * 4;
    const u32 vertices = count & 0xFFFF;
    const u32 indices = (vertices * 3) / 2;
    std::vector<f32> pos(size_t(vertices) * 2), col(size_t(vertices) * 4, 0.0f), uv(size_t(vertices) * 2);
    // the base map's texture size (vf2C / vf30: its renderer data's, else its pixel data's level 0; L: the port has
    // no texture renderer data)
    u32 texW = 0, texH = 0;
    if (t.texturing && t.texturing->HasMap(0))
        if (auto* src = dynamic_cast<const ni::NiSourceTexture*>(t.texturing->maps[0].textureObj.Get()))
            if (src->pixelDataObj && !src->pixelDataObj->mips.empty())
                texW = src->pixelDataObj->mips[0].width, texH = src->pixelDataObj->mips[0].height;
    const s32 W = s32(current_->width), H = s32(current_->height);   // (+0x58)+0 / +4
    for (u16 r = 0; r < rects; ++r) {
        const ni::NiScreenTexture::Rect& q = t.rects[r];
        // the position wrapped into the target (16-bit arithmetic)
        u16 x = u16(q.left), y = u16(q.top);
        if (W > 0) {
            while (s16(x) < 0) x = u16(x + W);
            while (s16(x) >= W) x = u16(x - W);
        }
        if (H > 0) {
            while (s16(y) < 0) y = u16(y + H);
            while (s16(y) >= H) y = u16(y - H);
        }
        const double xL = double(s16(x)) - 0.5f;      // FILD; FSUB [0x00293AA0] = 0.5f
        const double yT = double(s16(y)) - 0.5f;
        const f32 x1 = F(double(q.width) + xL);
        const f32 y1 = F(double(q.height) + yT);
        const double iw = 1.0 / double(u16(texW));    // FILD (movzx); FDIVR [0x00293A98] = 1.0f
        const f32 ih = F(1.0 / double(u16(texH)));
        const f32 u0 = F(double(q.texLeft) * iw);
        const f32 v0 = F(double(q.texTop) * ih);
        const f32 u1 = F(double(q.width) * iw + u0);
        const double v1 = double(q.height) * ih + v0; // (kept in the register)
        for (u32 k = 0; k < 4; ++k) {
            const size_t i = size_t(r) * 4 + k;
            const bool right = (k >> 1) != 0, bottom = (k & 1) != 0;
            pos[2 * i] = right ? x1 : F(xL);
            pos[2 * i + 1] = bottom ? y1 : F(yT);
            col[4 * i] = q.color.r, col[4 * i + 1] = q.color.g, col[4 * i + 2] = q.color.b, col[4 * i + 3] = q.color.a;
            uv[2 * i] = right ? u1 : u0;
            uv[2 * i + 1] = bottom ? F(v1) : v0;
        }
    }
    chipOut = PackScreenVertices(d3d_, *rd, u16(count), pos.data(), col.data(), uv.data(), rd->chip);
    std::vector<u16> idx(indices & 0xFFFF);
    u32 at = 0;
    for (u16 s = 1; u16(s - 1) < u16(count); s = u16(s + 4)) {   // (0, 1, 2), (2, 1, 3) per rect
        if (at + 6 > idx.size()) break;
        idx[at] = u16(s - 1), idx[at + 1] = s, idx[at + 2] = u16(s + 1);
        idx[at + 3] = u16(s + 1), idx[at + 4] = s, idx[at + 5] = u16(s + 2);
        at += 6;
    }
    ibOut = CreateIndexBuffer(d3d_, idx.data(), indices & 0xFFFF, rd->ib, rd->ibSize);   // (.., +0x24, +0x20, 1, 0)
}

bool NiXBoxRenderer::AddData(NiXBoxGeometryGroup& group, ni::NiGeometryData& data) {   // orig: 0x0015C640
    if (data.rendererData) return false;
    group.CreateData(data);                           // group vf0C(data, skin, 0, 0)
    return true;
}

// ---- the geometry-group manager ---------------------------------------------------------------------------------
ni::NiGeometryGroup* NiXBoxRenderer::CreateGeometryGroup(u32 type) {   // orig: 0x0015C5C0
    std::unique_ptr<NiXBoxGeometryGroup> g;
    if (type == 0) g = std::make_unique<NiXBoxStaticGroup>(d3d_);   // 0x00165B40
    else if (type == 1) g = std::make_unique<NiXBoxUnsharedGroup>(d3d_);   // 0x00165BE0
    else {
        NotPorted("NiXBoxRenderer: the dynamic geometry group (type 2, 0x00166740)");
        return nullptr;
    }
    g->blockAddress = [this] {
        const u32 n = blocks_++;
        const u32 a = blockAddress ? blockAddress(n) : 0u;
        return a ? a : 0x01000000u + 0x50u * n;       // (made up: allocation order)
    };
    groups_.push_back(std::move(g));                  // (+4 / +0xC, AddRef of the device)
    return groups_.back().get();
}

void NiXBoxRenderer::DestroyGeometryGroup(ni::NiGeometryGroup* group) {   // orig: group vf00; 0x0015C3D0
    auto* g = static_cast<NiXBoxGeometryGroup*>(group);
    if (!g) return;
    g->ReleaseAll();
    if (!g->members.empty()) return;                  // 0x0015C430: a group with data stays (the original leaks it)
    for (auto it = groups_.begin(); it != groups_.end(); ++it)
        if (it->get() == g) {
            groups_.erase(it);                        // vf2C(1)
            return;
        }
}

void NiXBoxRenderer::AddToGeometryGroup(ni::NiGeometryGroup* group, ni::NiGeometryData& data) {   // orig: 0x0015C7B0
    if (auto* g = static_cast<NiXBoxGeometryGroup*>(group)) AddData(*g, data);
}

void NiXBoxRenderer::PrecacheGeometry(ni::NiGeometry& g, bool isStatic) {   // orig: 0x0014B1D0 (g, static, 0, 0, 0)
    ni::NiGeometryData* data = g.modelData.Get();
    if (!data) return;                                // (the original dereferences the null data)
    AddData(*unshared_, *data);                       // 0x0015C640(+0x5B0, data, skin, 0, 0, 0)
    if (g.skinInstance) {
        NotPorted("NiXBoxRenderer::PrecacheGeometry: skinned geometry (0x00148200, skin partitions)");
        return;
    }
    // the exact classes (RTTI 0x002F9990 / 0x002F9C2C, not subclasses)
    if (g.ClassName() == ni::NiTriShape::kClassName || g.ClassName() == ni::NiTriStrips::kClassName)
        QueuePack(*data, isStatic);                   // 0x001480B0(data, indices, count, static, 0, 0, 0)
}

void NiXBoxRenderer::QueuePack(ni::NiGeometryData& data, bool isStatic) {   // orig: 0x001480B0
    auto* rd = static_cast<NiGeometryBufferData*>(data.rendererData.get());
    if (!rd || rd->chip) return;                      // (no renderer data: the original faults)
    const VertexLayout l = Layout(data);              // 0x001578C0 (no shader layout 0x0015D780)
    rd->vertexCount = data.vertexCount;               // vf30
    rd->stride = l.stride;
    rd->fvf = l.fvf;
    rd->flags = isStatic ? 1u : 0u;
    rd->flags = data.normals.empty() ? rd->flags & 0xFF7FFFFFu : rd->flags | 0x800000u;
    rd->flags |= u32(data.textureSets) << 24;
    NiVBChip* chip = rd->group ? rd->group->AllocChip(*rd) : nullptr;   // 0x001578B0: the group's vf20
    rd->chip = chip;
    if (!chip) return;                                // (the original reads the null chip's block)
    const u32 key = chip->block ? chip->block->address : 0;
    std::vector<PendingKey>& bucket = pending_[key % 37];
    PendingKey* k = nullptr;
    for (PendingKey& e : bucket)
        if (e.key == key) k = &e;
    if (!k) k = &*bucket.insert(bucket.begin(), PendingKey{key, chip->block, {}});   // NiTStringMap::SetAt 0x0010BD70
    k->packs.insert(k->packs.begin(), PendingPack{&data, rd->flags, chip});   // +0x28 = the old head
}

void NiXBoxRenderer::PackPending() {                  // orig: 0x00148390
    auto indexCount = [](const NiGeometryBufferData& rd) {
        u32 n = 0;
        if (const u16* lengths = rd.StripLengths()) {
            for (u32 i = rd.stripCount; i != 0; --i) n += lengths[i - 1];
        } else if (rd.primitive == PT_TRIANGLELIST) {
            n = rd.primitiveCount * 3;
        } else if (rd.primitive == PT_TRIANGLESTRIP) {
            n = rd.primitiveCount + 2;
        }
        return n;
    };
    auto indices = [&](NiGeometryBufferData& rd) {    // 0x0015CBC0(rd +0x3C, n, rd +0x24, rd +0x20, 1, 0)
        const u32 n = indexCount(rd);
        if (const u16* idx = rd.Indices()) {
            rd.ib = CreateIndexBuffer(d3d_, idx, n, rd.ib, rd.ibSize);
            rd.indexCount = n;
        }
    };
    for (auto& bucket : pending_) {
        for (PendingKey& k : bucket) {                // (bucket order, each chain from its head)
            if (k.key == 0) {                         // buffers of their own: each packed (and locked) alone
                for (PendingPack& p : k.packs) {
                    auto* rd = static_cast<NiGeometryBufferData*>(p.data->rendererData.get());
                    if (!rd) continue;
                    // (shader packing 0x0015D7D0: no shaders)
                    NiVBChip* chip = PackVertices(d3d_, vbFrame_, *p.data, nullptr, true, p.chip, nullptr);
                    if (rd->chip != chip) {
                        if (rd->group) rd->group->FreeChip(*rd);   // vf24
                        rd->chip = chip;
                    }
                    indices(*rd);
                }
                continue;
            }
            // a shared block: locked once over its records' range, each record packed at its chip's offset
            u32 lo = 0xFFFFFFFFu, hi = 0;
            const NiVBChip* last = nullptr;
            for (const PendingPack& p : k.packs) {
                lo = std::min(lo, p.chip->offset);
                hi = std::max(hi, p.chip->offset + p.chip->size);
                last = p.chip;
            }
            if (!last || hi < lo) continue;
            // 0x00157990: Lock2(vb, flags) + lo; each record's destination is that plus its chip's offset (H: the
            // start is added twice, so records land `lo` bytes late when the lowest chip is not at offset 0)
            u8* base = d3d_.Lock2(last->vb, last->lockFlags);
            if (lo) NotPorted("NiXBoxRenderer::PackPending: a block packed from a chip past its start");
            for (PendingPack& p : k.packs) {
                auto* rd = static_cast<NiGeometryBufferData*>(p.data->rendererData.get());
                if (!rd) continue;
                const size_t at = size_t(lo) + p.chip->offset;
                const size_t need = size_t(rd->vertexCount) * rd->stride;
                if (base && at + need <= last->vb->data.size())   // (UB-avoiding: the original writes past the buffer)
                    PackVertices(d3d_, vbFrame_, *p.data, nullptr, true, p.chip, base + at);
                indices(*rd);
            }
        }
    }
    // the records done (newest processed first): static data gives its CPU copies back (NiGeometryData::Replace
    // (count, 0, 0, 0, 0), the triangles 0x00115E40 / strips 0x0011BC90) -- NOT PORTED: the port keeps them (no D3D
    // effect; the viewer and game code that may read them)
    for (auto& bucket : pending_) bucket.clear();     // 0x0004A3D0 (RemoveAll)
}

// ---- particles ---------------------------------------------------------------------------------------------------
void NiXBoxRenderer::DrawParticles(ni::NiGeometryData& data, const ni::NiTransform& world, const ni::NiBound&) {
    // orig: 0x00148B70
    if (!inScene_) return;                            // +0x500
    if (data.vertexCount == 0 && !data.vertices.empty()) return;   // vf30 (0x0010E5D0: +8) 0 with vertices (+0x1C)
    auto* pd = dynamic_cast<ni::NiParticlesData*>(&data);
    if (!pd) {                                        // (the slot reads NiParticlesData's +0x34 / +0x38 / +0x3C)
        NotPorted("NiXBoxRenderer::DrawParticles: data other than NiParticlesData");
        return;
    }
    const u32 n = pd->activeCount;                    // +0x38
    if (n == 0) return;
    if (pd->vertices.size() < n || pd->sizes.size() < n || (!pd->colors.empty() && pd->colors.size() < n) ||
        (!pd->normals.empty() && pd->normals.size() < n))
        return;                                       // (UB-avoiding: the original reads past the arrays)
    SetWorldTransform(world);                         // 0x00148A30: also the model-space camera vectors +0x314 / +0x320
    // (+0x32C = the bound, for the pipeline)
    if (!propertyState) return;                       // (the original reads the null state)
    state_->UpdateRenderState(*propertyState);        // 0x00159D60
    // 0x00159EE0 / 0x00159EF0: the passes and the first one (the particles' texture sets: 1, L)
    const u32 passFlags = pipeline_->BuildPasses(*propertyState, effectState, 1);
    pipeline_->NextPass();
    if (!(passFlags & 0x10000000)) {                  // 0x0015C010
        const auto* tex = static_cast<const ni::NiTexturingProperty*>(
            propertyState->slot[ni::NiPropertyState::kTexturing].Get());
        const auto* vc = static_cast<const ni::NiVertexColorProperty*>(
            propertyState->slot[ni::NiPropertyState::kVertexColor].Get());
        if (tex && vc) lights_->SetState(effectState, *tex, *vc);
    }
    const u32 vertices = n * 4;
    Resource* ib = QuadIndexBuffer(vertices);         // 0x0015CC80(+0x5B8, n * 4, 1, 0, 1): (H) a quad per vertex
    if (!ib) return;
    // the corner offsets in model space: (up - right) and (up + right) times +0x34 / the world scale (x87: the
    // quotient and the x / y differences stay in registers, the z difference is stored first)
    const double s = double(pd->value34) / world.scale;   // FLD [+0x34]; FDIV [world +0x3C]
    const ni::NiPoint3& R = camRight1_;               // +0x314
    const ni::NiPoint3& U = camUp1_;                  // +0x320
    const f32 a[3] = {F((double(U.x) - R.x) * s), F((double(U.y) - R.y) * s), F(double(F(double(U.z) - R.z)) * s)};
    const f32 b[3] = {F((double(U.x) + R.x) * s), F((double(U.y) + R.y) * s), F(double(F(double(U.z) + R.z)) * s)};
    const bool normals = !pd->normals.empty();        // +0x20
    const bool colors = !pd->colors.empty();          // +0x24
    const u32 stride = normals ? 36 : 24;
    const u32 fvf = normals ? 0x152u : 0x142u;        // XYZ | (NORMAL |) DIFFUSE | TEX1
    Resource* vb = DynamicVertexBuffer(vertices * stride);   // 0x00158610(+0x5B4, length, 0, fvf, normals)
    if (!vb) return;
    u8* out = d3d_.Lock2(vb, 0);
    if (!out || vb->data.size() < size_t(vertices) * stride) return;
    auto put = [](u8* at, f32 v) { std::memcpy(at, &v, 4); };
    auto colorByte = [](f32 v) {                      // FSTP(v * 255.0f) [0x002A1564], FISTP (round to nearest)
        return static_cast<u32>(static_cast<s32>(std::nearbyint(F(double(v) * 255.0f))));
    };
    constexpr f32 kUV[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    for (u32 i = 0; i < n; ++i) {
        const ni::NiPoint3& p = pd->vertices[i];      // +0x1C
        u32 argb = 0xFFFFFFFFu;                       // (no colours: white)
        if (colors) {                                 // A, R, G, B (no masking)
            const ni::NiColorA& c = pd->colors[i];
            argb = colorByte(c.a);
            argb = (argb << 8) | colorByte(c.r);
            argb = (argb << 8) | colorByte(c.g);
            argb = (argb << 8) | colorByte(c.b);
        }
        const double size = pd->sizes[i];             // +0x3C
        // 0x000115C0 (out, size, v): size * v, each component stored
        const f32 sb[3] = {F(size * b[0]), F(size * b[1]), F(size * b[2])};
        const f32 sa[3] = {F(size * a[0]), F(size * a[1]), F(size * a[2])};
        const f32 corner[4][3] = {
            {F(double(p.x) - sb[0]), F(double(p.y) - sb[1]), F(double(p.z) - sb[2])},
            {F(double(p.x) - sa[0]), F(double(p.y) - sa[1]), F(double(p.z) - sa[2])},
            {F(double(sb[0]) + p.x), F(double(sb[1]) + p.y), F(double(sb[2]) + p.z)},
            {F(double(sa[0]) + p.x), F(double(sa[1]) + p.y), F(double(sa[2]) + p.z)}};
        for (u32 k = 0; k < 4; ++k) {
            u8* v = out + size_t(i * 4 + k) * stride;
            put(v, corner[k][0]), put(v + 4, corner[k][1]), put(v + 8, corner[k][2]);
            u32 at = 12;
            if (normals) {                            // the particle's normal on every corner
                const ni::NiPoint3& nn = pd->normals[i];
                put(v + 12, nn.x), put(v + 16, nn.y), put(v + 20, nn.z);
                at = 24;
            }
            std::memcpy(v + at, &argb, 4);
            put(v + at + 4, kUV[k][0]), put(v + at + 8, kUV[k][1]);
        }
    }
    d3d_.SetStreamSource(0, vb, stride);
    d3d_.SetIndices(ib, 0);
    state_->SetVertexShader(fvf);                     // (+0x5C0)+0x764
    state_->SetPixelShader(0);                        // (+0x5C0)+0x75C
    state_->SetVertexBlend(0);                        // 0x00147C00(+0x5C0, 0)
    dynamicCurrent_ = (dynamicCurrent_ + 1) % 10;     // 0x00157A10: InsertFence into the slot, then the next slot
    d3d_.DrawIndexedVertices(PT_TRIANGLELIST, n * 6, d3d_.indexData);   // (D3D__IndexData 0x00244564)
}

Resource* NiXBoxRenderer::DynamicVertexBuffer(u32 length) {   // orig: 0x00158610 -> 0x001584D0
    const u32 want = std::max(length, dynamicMinSize_);
    dynamicMinSize_ = want;                           // (+0x10 grows to the largest length asked)
    DynamicSlot& slot = dynamic_[dynamicCurrent_];
    if (slot.vb) {
        // (the slot's fence: IsFencePending / BlockOnFence; L: never pending)
        if (slot.size >= want) return slot.vb;
        d3d_.Release(slot.vb);
        slot.vb = nullptr;
        slot.size = 0;
    }
    Resource* vb = d3d_.CreateVertexBuffer2(want);
    if (!vb) return nullptr;                          // (the error text 0x002AB0F8)
    slot.vb = vb;                                     // (the slot's old buffer released first: none here)
    d3d_.AddRef(vb);
    slot.size = want;
    d3d_.Release(vb);
    return vb;
}

Resource* NiXBoxRenderer::QuadIndexBuffer(u32 quads) {   // orig: 0x0015CC80 (quads, refill 1, 0, 1)
    if (quads == 0) return nullptr;
    const u32 bytes = quads * 12;
    Resource* ib = quadIB_;
    if (ib) {
        d3d_.GetDesc(ib);                             // 0x0023A5C0: D3DFMT_INDEX16 0x65, D3DRTYPE_INDEXBUFFER 7
        if (quadIBSize_ < bytes) {
            d3d_.Release(ib);
            ib = nullptr;
        }
    }
    if (!ib) {
        ib = d3d_.CreateIndexBuffer2(bytes);
        if (!ib) {                                    // (the error texts 0x002AB898 / 0x002AB918; the original keeps
            quadIB_ = nullptr;                        // the released buffer in +0xC: UB-avoiding)
            return nullptr;
        }
    }
    for (u32 q = 0; q < quads && size_t(q + 1) * 12 <= ib->data.size(); ++q) {   // (0, 1, 2), (0, 2, 3)
        const u16 v = u16(q * 4);
        const u16 idx[6] = {v, u16(v + 1), u16(v + 2), v, u16(v + 2), u16(v + 3)};
        std::memcpy(ib->data.data() + size_t(q) * 12, idx, 12);
    }
    quadIBSize_ = bytes;                              // +0x10 (also when an older, bigger buffer is kept)
    quadIB_ = ib;                                     // +0xC
    return ib;
}

void NiXBoxRenderer::SetWorldTransform(const ni::NiTransform& t) {   // orig: 0x00148A30
    // each 16-byte column of the rotation times the scale is a row of WORLD; the translation the last row
    const auto& M = t.rotate.m;
    world_ = {};
    for (int c = 0; c < 3; ++c) {
        world_.m[c][0] = F(double(M[0][c]) * t.scale);
        world_.m[c][1] = F(double(M[1][c]) * t.scale);
        world_.m[c][2] = F(double(M[2][c]) * t.scale);
    }
    world_.m[3][0] = t.translate.x, world_.m[3][1] = t.translate.y, world_.m[3][2] = t.translate.z;
    world_.m[3][3] = 1.0f;
    d3d_.SetTransform(TS_WORLD, world_);
    state_->SetNormalize(t.scale);                    // 0x00159290
    // the camera's right / up (+0x2FC / +0x308) in model space (+0x314 / +0x320): 0x000AD900 the rotation times the
    // scale (SSE: each element a float product; 16-byte chunk c = column c), 0x00103290 component c = chunk c . v
    // (x87, in this order)
    f32 c[3][3];
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 3; ++i) c[k][i] = F(double(M[i][k]) * t.scale);
    auto toModel = [&c](const ni::NiPoint3& v) {
        const f32 x = F((double(c[0][2]) * v.z + double(c[0][1]) * v.y) + double(v.x) * c[0][0]);
        const f32 y = F((double(c[1][0]) * v.x + double(c[1][2]) * v.z) + double(c[1][1]) * v.y);
        const f32 z = F((double(c[2][0]) * v.x + double(c[2][2]) * v.z) + double(c[2][1]) * v.y);
        return ni::NiPoint3{x, y, z};
    };
    camRight1_ = toModel(camRight0_);
    camUp1_ = toModel(camUp0_);
}

void NiXBoxRenderer::DrawGeometry(ni::NiGeometryData* data, ni::NiSkinInstance* skin, const ni::NiTransform& world,
                                  NiGeometryBufferData& rd) {   // orig: 0x00149B70
    // the texture sets: the data's (+0xA), or the renderer data's (+4 byte 3) for a static chip or without data
    u32 sets = 0;
    if (!rd.chip && !data) return;
    if (!rd.chip || (!(rd.flags & 1) && data)) sets = data->textureSets;
    else sets = (rd.flags >> 24) & 0xFF;
    if (sets > 4) sets = 4;                           // DAT_002D8670
    if (!propertyState) return;                       // (the original reads the null state)
    const ni::NiPropertyState& props = *propertyState;
    // (the property state's renderer-specific property +0x18 with a shader object: none in this game, L)
    pipeline_->Begin(props);                          // 0x0015AAF0
    state_->UpdateRenderState(props);                 // 0x00159D60
    const u32 passFlags = pipeline_->BuildPasses(props, effectState, sets);   // 0x00159EE0 -> vf08 0x001643F0
    state_->SetComplex(RS_VERTEXBLEND, state_->vertexBlendTable[0]);   // (the cache +0x674 vs +0xE8)
    if (!(passFlags & 0x800000)) SetWorldTransform(world);   // 0x00148A30 (+0x32C = the bound)
    if (!(passFlags & 0x10000000)) {                  // 0x0015C010(effects, texturing +0x28, vertex colour +0x2C)
        const auto* tex =
            static_cast<const ni::NiTexturingProperty*>(props.slot[ni::NiPropertyState::kTexturing].Get());
        const auto* vc =
            static_cast<const ni::NiVertexColorProperty*>(props.slot[ni::NiPropertyState::kVertexColor].Get());
        if (tex && vc) lights_->SetState(effectState, *tex, *vc);
    }
    const u32 fogSaved = state_->rs[RS_FOGENABLE][0];   // +0x50C
    u32 passIndex = 0, drawn = 0;
    if (!(passFlags & 0x40000000)) {
        bool more = true;
        while (more) {
            const u32 f = pipeline_->NextPass();      // 0x00159EF0 -> vf0C 0x00162940
            more = (f & 0xFFFF) != 0;
            if (passIndex == 1) state_->SetDeferred(RS_FOGENABLE, 0, 0x2000);   // (the second pass on)
            else ++passIndex;
            if (f & 0x40000000) continue;
            if (drawn++ == 0 && !(f & 0x2000000) && data) UpdateBuffers(rd, *data, skin);   // 0x00146FB0
            if (!(f & 0x8000000)) pipeline_->ApplyPixelShader();   // 0x00159FA0
            if (!rd.chip) break;                      // (the original reads the null chip's buffer)
            if (!(f & 0x20000000)) {
                d3d_.SetStreamSource(0, rd.chip->vb, rd.stride);
                if (rd.ib) d3d_.SetIndices(rd.ib, rd.baseVertex);
            }
            if (!(f & 0x4000000)) pipeline_->ApplyVertexShader(rd.fvf);   // 0x00159FB0 -> 0x00163DA0
            // (!(f & 0x1000000): the shader's +0x30 callback or 0x00159FC0, the pipeline's vf04: none / no-op)
            const u32* k = kPrimitiveIndices[rd.primitive < 11 ? rd.primitive : 0];
            if (rd.ib) {
                u32 at = 0;
                const u16* lengths = rd.StripLengths();
                for (u32 i = 0; i < rd.stripCount; ++i) {
                    const u32 n = lengths ? u32(lengths[i]) - 2 : rd.primitiveCount;
                    d3d_.DrawIndexedVertices(rd.primitive, k[0] * n + k[1], d3d_.indexData + at);
                    // (DAT_0030EE74 == 1: +0x5CC vf10(0), L: a debug hook)
                    at += n + 2;
                }
            } else {
                d3d_.DrawVertices(rd.primitive, rd.baseVertex, k[0] * rd.primitiveCount + k[1]);
            }
        }
    }
    state_->SetDeferred(RS_FOGENABLE, fogSaved, 0x2000);
    pipeline_->End();                                 // 0x0015AB90
}

bool NiXBoxRenderer::UpdateBuffers(NiGeometryBufferData& rd, ni::NiGeometryData& data, ni::NiSkinInstance* skin) {
    // orig: 0x00146FB0 (no shader: the texture effect's shader +0x30 path NOT PORTED with the pipeline)
    if (rd.chip && (rd.flags & 1)) return true;       // static: packed once
    const u32 count = data.vertexCount;               // vf30
    if (!count) return false;
    const auto* shape = dynamic_cast<const ni::NiTriShapeData*>(&data);   // IsKindOf 0x002F9954: data +0x3C
    const u16* idx = shape ? (shape->triangleList.empty() ? nullptr : shape->triangleList.data()) : rd.Indices();
    const u32 triangles = dynamic_cast<const ni::NiTriBasedGeomData*>(&data)
                              ? static_cast<const ni::NiTriBasedGeomData&>(data).triangleCount : 0u;   // vf3C
    const u32 n = rd.primitive == PT_TRIANGLELIST ? triangles * 3 : rd.primitiveCount + rd.stripCount * 2;
    rd.vertexCount = count;                           // +0x14
    rd.indexCount = n;                                // +0x1C
    rd.primitiveCount = triangles & 0xFFFF;           // +0x30
    if (!rd.chip) {
        rd.flags = 0;
        rd.baseVertex = 0;
        rd.chip = PackVertices(d3d_, vbFrame_, data, skin, true, nullptr, nullptr);   // 0x00157C90(.., 1, 0, 0)
        if (idx) rd.ib = CreateIndexBuffer(d3d_, idx, n, nullptr, rd.ibSize);
    } else {
        NiVBChip* chip = nullptr;
        Resource* ib = nullptr;
        bool isNew = true;
        bool pack = true;
        if (data.revisionFlags == 0) {                // +0x2E
            chip = rd.chip;                           // (count <= +0x14: just stored)
            if (rd.ib) ib = rd.ib;                    // (n <= +0x1C: just stored)
            if (!skin) pack = false;
            else isNew = false;
        }
        if (pack) chip = PackVertices(d3d_, vbFrame_, data, skin, isNew, rd.chip, nullptr);
        if (!ib && idx) ib = CreateIndexBuffer(d3d_, idx, n, rd.ib, rd.ibSize);
        if (rd.chip != chip) {                        // 0x0015C240: the group's vf24, then the new chip
            if (rd.group) rd.group->FreeChip(rd);
            rd.chip = chip;
        }
        if (idx && rd.ib != ib) {
            if (rd.ib) d3d_.Release(rd.ib);
            rd.ib = ib;
        }
    }
    data.revisionFlags = 0;                           // the group's vf28 (0x000AEDD0: false) -> +0x2E = 0
    return true;
}

}  // namespace fut::render::nixbox

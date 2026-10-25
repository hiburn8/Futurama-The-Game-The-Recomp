// Liquids; see Liquid.h.
#include "game/object/Liquid.h"

#include "game/GameTime.h"
#include "game/defs/EffectDefs.h"
#include "game/level/LevelData.h"
#include "core/CrtString.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiRenderer.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace fut::game {

using ni::detail::F;

Liquid::Liquid(LevelData& lvl, LiquidDef& d, ni::NiGeometry* g) : GameObject(lvl, d), geometry(g) {}

Liquid::~Liquid() {                                   // orig: 0x0006B540
    if (resource && resource->RefCount() == 2) {      // (+0x3C)+4 == 2
        auto& cache = level.liquidResources;          // 0x002DEDF4 (data 0x002DEDF8, size 0x002DEE00)
        auto it = std::find_if(cache.begin(), cache.end(), [&](const auto& r) { return r.Get() == resource.Get(); });
        if (it != cache.end()) {
            *it = nullptr;                            // SetAt(i, 0) 0x0012B7F0 (the old entry released)
            std::erase_if(cache, [](const auto& r) { return !r; });   // 0x0006B2E0: the entries moved down, size cut
        }
    }
    resource = nullptr;                               // +0x3C
    geometry = nullptr;                               // +0x74 (then the GameObject dtor)
}

Liquid* SpawnLiquid(LevelData& level, LiquidDef& def, Owner* owner, ni::NiGeometry* geometry, f32 a, f32 b,
                    f32 c) {                          // orig: 0x0006B490
    auto* o = new Liquid(level, def, geometry);
    o->SetOwner(owner);                               // (no position, rotation, name or id)
    o->paramA = a;
    o->paramB = b;
    o->waveHeight = c;
    o->Init();
    return o;
}

LiquidResource::LiquidResource(LevelData& level, u32 layers, u32 t, f32 pa, f32 pb)
    : type(t), a(pa), b(pb), layerCount(layers) {     // orig: 0x0006B700 (+0x14 = 0; ++[0x0030EBC8], the object count)
    camera = new ni::NiCamera;                        // new(0x250) 0x001009D0 (never updated: identity axes)
    static const ni::NiPoint3 kV[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};   // [esp+0x40..]
    static const ni::NiPoint2 kUV[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};             // [esp+0x20..]
    for (u32 i = 0; i < layerCount && i < polygons.size(); ++i) {   // (at most 5: Init's counts)
        polygons[i] = new ni::NiScreenPolygon(4, kV, kUV, nullptr);   // new(0x24) 0x00106DE0
        auto* tp = new ni::NiTexturingProperty;       // new(0x3C) 0x00109DC0
        texturing[i] = tp;
        tp->applyMode = 2;                            // +0x18
        if (!tp->HasMap(0)) tp->SetBaseTexture(nullptr);   // SetBaseFilterMode(1) 0x0001D6B0: the base map
        tp->maps[0].filterMode = 1;                   //  (Map::Map 0x0001D570), filter 1
        auto* m = new ni::NiMaterialProperty;         // new(0x5C) 0x00114B10
        m->ambient = m->diffuse = m->specular = m->emissive = {1.0f, 1.0f, 1.0f};   // DAT_002D7BD0
        m->shininess = 0.0f, m->alpha = 1.0f;
        m->revision += 6;                             // (three increments, then +3)
        material[i] = m;
        auto* al = new ni::NiAlphaProperty;           // new(0x1C): NiObjectNET 0x000FE390, vtbl 0x00293B54, flags
        al->flags &= ~1u;                             //  0xEC, ref 0; then flags &= ~1
        alpha[i] = al;
        polygons[i]->AttachProperty(m);               // the polygon's property state +8, slot = vf2C (the type)
        polygons[i]->AttachProperty(al);
        polygons[i]->AttachProperty(tp);
        camera->AttachScreenPolygon(polygons[i].Get());   // 0x00019460
    }
    // NiRenderedTexture::Create 0x00140070 @0x0006BBD1 (prefs {1, 2, 3}), retried @0x0006BC1F (prefs {2, 2, 3})
    texture = ni::CreateRenderedTexture(128, 128, level.renderer);
    if (!texture) texture = ni::CreateRenderedTexture(128, 128, level.renderer);
}

namespace {

// Liquid::Init's per-type layer locals: count [esp+0x14], names [esp+0x38 + 4i], blend modes [esp+0x4C + 4i], UV
// scales [esp+0x60 + 8i].
struct LayerSet {
    u32 count = 3;
    const char* names[5] = {};
    u32 modes[5] = {0, 2, 1, 0, 0};
    ni::NiPoint2 scales[5] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}};
};

// orig: jump table 0x0006C880 (mode - 1): the layer's alpha flags (Gamebryo: bit 0 blending, src bits 1-4, dest
// bits 5-8). `flags |= 1` first in every mode but the default (dx = 1).
void SetLayerBlend(ni::NiAlphaProperty& al, u32 mode) {
    u32 f = al.flags;
    switch (mode) {
        case 1:                                       // 0x0006C762: ONE / ONE
            f |= 1;
            f &= 0xFFE1;                              // (AND byte 0xE1)
            f &= 0xFE1F;
            break;
        case 2:                                       // 0x0006C66D: ZERO / SRCCOLOR
            f |= 1;
            f = (f & 0xFFE3) | 2;
            f = (f & 0xFE5F) | 0x40;
            break;
        case 3:                                       // 0x0006C6F2: ONE / SRCCOLOR
            f |= 1;
            f &= 0xFFE1;                              // (AND byte 0xE1)
            f = (f & 0xFE5F) | 0x40;
            break;
        case 4:                                       // 0x0006C721: SRCALPHA / INVSRCALPHA
            f |= 1;
            f = (f & 0xFFED) | 0x0C;
            f = (f & 0xFEFF) | 0xE0;
            break;
        case 5:                                       // 0x0006C6AE: ZERO / SRCALPHA
            f |= 1;
            f = (f & 0xFFE3) | 2;
            f = (f & 0xFEDF) | 0xC0;
            break;
        default:                                      // 0x0006C784: off (AND byte 0xFE)
            f &= 0xFFFE;
            break;
    }
    al.flags = static_cast<u16>(f);
}

f32 Mul(f32 x, u32 k) { return F(double(x) * std::bit_cast<f32>(k)); }   // FLD x; FMUL [k]; FSTP

}  // namespace

bool Liquid::Init() {                                 // orig: 0x0006BC60
    ni::NiGeometry* g = geometry.Get();
    if (!g) return false;
    auto* tp = dynamic_cast<ni::NiTexturingProperty*>(GetProperty(*g, 8));
    if (!tp) return false;                            // (the liquid object stays alive)
    if (!tp->HasMap(0)) return false;
    const u32 type = static_cast<LiquidDef&>(*def).liquid;
    // The layers (jump table 0x0006C85C; types above 8 keep the defaults). `zero` = 0x0006A5C0(+0x40, (0, 0)): both
    // parameters equal to 0 (FCOMP; TEST AH,0x44; JP: NaN is not equal). The velocities +0x48 + 8i start at 0 (ctor).
    LayerSet L;
    ni::NiPointer<ni::NiObject> ownTexture;           // [esp+0x18] (type 7: the geometry's own base texture)
    const bool zero = paramA == 0.0f && paramB == 0.0f;
    auto setScales3 = [&L] { L.scales[1] = {-1, -1}, L.scales[2] = {-1, 1}; };
    auto setScales5 = [&L] {
        L.scales[0] = {1, 1}, L.scales[1] = {-1, -1}, L.scales[2] = {-1, 1}, L.scales[3] = {1, -1};
        L.scales[4] = {1, 1};
    };
    switch (type) {
        case 0: {                                     // lava 0x0006BDBF
            setScales3();
            L.names[0] = L.names[1] = "LavaGlow.tga", L.names[2] = "LavaBack.tga";   // 0x0029C294 / 0x0029C284
            if (zero) {
                const f32 k = std::bit_cast<f32>(0x3C23D70Au);   // 0.01
                layerVel = {{{k, 0}, {k, 0}, {0, k}, {0, k}, {0, 0}}};
                setScales5();
                L.names[2] = L.names[3] = "LavaGlow.tga", L.names[4] = "LavaBack.tga";
                L.modes[0] = 0, L.modes[1] = 3, L.modes[2] = 3, L.modes[3] = 2, L.modes[4] = 1;
                L.count = 5;
            } else {
                layerVel[0] = {Mul(paramA, 0x3D23D70Au), Mul(paramB, 0x3D23D70Au)};   // [0x00293C38] 0.04
                layerVel[1] = {Mul(paramA, 0xBCF5C28Fu), Mul(paramB, 0xBCF5C28Fu)};   // [0x0029C280] -0.03
                layerVel[2] = {Mul(paramA, 0xBC449BA6u), Mul(paramB, 0x3C449BA6u)};   // [0x0029C27C] / [0x0029C278]
            }
            break;
        }
        case 1:                                       // slime 0x0006BFA3
        case 5:                                       // sand
        case 6: {                                     // alien
            setScales3();
            const char* glow = type == 1 ? "SlimeGlow.tga" : type == 6 ? "AlienGlow.tga" : "SandTop.tga";
            const char* back = type == 1 ? "SlimeBack.tga" : type == 6 ? "AlienBack.tga" : "SandBack.tga";
            L.names[0] = L.names[1] = glow, L.names[2] = back;   // 0x0029C268 / 258, 248 / 238, 22C / 21C
            if (zero) {
                const f32 k = std::bit_cast<f32>(0x3CA3D70Au);   // 0.02
                layerVel = {{{k, 0}, {k, 0}, {0, k}, {0, k}, {0, 0}}};
                setScales5();
                L.names[2] = L.names[3] = glow, L.names[4] = back;
                L.modes[0] = 0, L.modes[1] = L.modes[2] = L.modes[3] = 5, L.modes[4] = 1;
                L.count = 5;
            } else {
                layerVel[0] = {Mul(paramA, 0x3DA3D70Au), Mul(paramB, 0x3DA3D70Au)};   // [0x00295394] 0.08
                layerVel[1] = {Mul(paramA, 0xBD656041u), Mul(paramB, 0xBD656041u)};   // [0x0029C218] -0.056
                layerVel[2] = {Mul(paramA, 0xBCC49BA6u), Mul(paramB, 0xBCC49BA6u)};   // [0x0029C214] -0.024
            }
            break;
        }
        case 2:                                       // water 0x0006C177
        case 4: {                                     // swamp
            setScales3();
            L.names[0] = L.names[1] = type == 4 ? "SwampFog.tga" : "WaterShine.tga";   // 0x0029C204 / 0x0029C1E4
            L.names[2] = type == 4 ? "SwampBack.tga" : "WaterBack.tga";                // 0x0029C1F4 / 0x0029C1D4
            if (zero) {
                const f32 k = std::bit_cast<f32>(0x3DCCCCCDu);   // 0.1
                layerVel[0] = {k, k}, layerVel[1] = {k, k}, layerVel[2] = {0, 0};
            } else {
                layerVel[0] = {Mul(paramA, 0x3ECCCCCDu), Mul(paramB, 0x3ECCCCCDu)};   // [0x00293B28] 0.4
                layerVel[1] = {Mul(paramA, 0xBE8F5C29u), Mul(paramB, 0xBE8F5C29u)};   // [0x0029C1D0] -0.28
                layerVel[2] = {Mul(paramA, 0xBDF5C290u), Mul(paramB, 0xBDF5C290u)};   // [0x0029C1CC] -0.12
            }
            break;
        }
        case 7: {                                     // the lavaman's 0x0006C256 (no `zero` test)
            const f32 k = std::bit_cast<f32>(0x3CA3D70Au);   // 0.02
            layerVel[0] = {k, k}, layerVel[1] = {k, std::bit_cast<f32>(0x3D75C28Fu)};   // (0.02, 0.06)
            layerVel[2] = {0, 0}, layerVel[3] = {0, k};
            L.scales[0] = {-1, -1}, L.scales[1] = {-1, 1}, L.scales[2] = {1, 1}, L.scales[3] = {1, 1};
            L.count = 4;
            L.names[0] = L.names[1] = "LavaGlow.tga", L.names[2] = "LavaBack.tga", L.names[3] = "text";   // 0x00293FB8
            L.modes[0] = 0, L.modes[1] = 2, L.modes[2] = 1, L.modes[3] = 4;
            ownTexture = tp->maps[0].textureObj.Get();   // NiPointer::Assign(m0->texture)
            break;
        }
        case 8:                                       // 0x0006BD8D (no tag spawns it)
            L.names[0] = L.names[1] = "Smoketest.tga";   // 0x0029C2A4
            layerVel[0] = {0, std::bit_cast<f32>(0x3ECCCCCDu)}, layerVel[1] = {0, 0};   // (0, 0.4)
            L.modes[0] = 0, L.modes[1] = 5;
            L.count = 2;
            break;
        default:                                      // 3 oil (0x0006C348) and above 8: the defaults
            break;
    }
    // the colours (+0xA0..+0xC4)
    switch (type) {
        case 0:                                       // lava
            colorBase = {0.9f, 0.25f, 0.0f, 0.0f};
            colorAmp = {0.1f, 0.75f, 1.0f, 1.0f};
            colorMode = waveHeight > 0.0f ? 4 : 3;    // FCOMP; TEST AH,0x41
            if (colorMode == 4 && g->modelData && !g->modelData->vertices.empty())
                baseY = g->modelData->vertices[0].y;
            break;
        case 1:                                       // slime
        case 5:                                       // sand
        case 6:                                       // alien
            colorBase = {0.75f, 0.75f, 0.75f, 0.0f};
            colorAmp = {0.25f, 0.25f, 0.25f, 1.0f};
            colorMode = type == 5 ? 0 : 3;
            break;
        case 2:                                       // water, swamp: +0xA0 = 0
        case 4:
            colorMode = 0;
            break;
        case 7:                                       // the lavaman's (GenericNPCDef::Spawn): 0x0006C256
            colorBase = {std::bit_cast<f32>(0x3F666666u), 0.25f, 0.0f, 0.0f};   // (0.9, 0.25, 0, 0)
            colorAmp = {std::bit_cast<f32>(0x3DCCCCCDu), 0.75f, 1.0f, 1.0f};    // (0.1, 0.75, 1, 1)
            colorMode = 0;                            // +0xA0 = 0 (no vertex colours)
            break;
        default:
            break;
    }
    // 0x0006C348: the last cached resource of the type, else a new one (added to the cache 0x0006B630)
    for (const auto& r : level.liquidResources)
        if (r && r->type == type && resource.Get() != r.Get()) resource = r;
    bool created = false;
    if (!resource) {
        created = true;                               // [esp+0x13]
        resource = new LiquidResource(level, L.count, type, paramA, paramB);   // new(0x74) 0x0006B700
        auto& cache = level.liquidResources;          // 0x0006B630: the first null slot, else appended
        auto slot = std::find_if(cache.begin(), cache.end(), [](const auto& r) { return !r; });
        if (slot != cache.end()) *slot = resource;
        else cache.push_back(resource);
    }
    LiquidResource& res = *resource;
    stamp = res.stamp;                                // 0x0006C431: +0x38 = res+0x14
    tp->maps[0].textureObj = res.texture.Get();       // m0->texture = res+0x1C (the geometry shows the layers)
    if (created)
        for (u32 i = 0; i < L.count && i < res.layerCount; ++i) {
            ni::NiTexturingProperty::Map map;         // new(0x18) Map::Map 0x0001D570 (clamp 3, filter 0, ps2 0 / 0xFFB5)
            map.clampMode = 3, map.filterMode = 0, map.ps2L = 0, map.ps2K = 0xFFB5;
            map.textureIndex = 0;                     // +0x10 = 0
            if (type == 7 && L.names[i] && crt::StrICmp(L.names[i], "text") == 0)   // _stricmp 0x001684D1
                map.textureObj = ownTexture.Get();
            else                                      // Session::FindTexture 0x000C2A80 (L: a null name is never
                map.textureObj = L.names[i] ? level.FindTexture(L.names[i]) : nullptr;   //  reached: type 3)
            map.filterMode = 2;                       // +0xC (TRILERP)
            map.clampMode = 3;                        // +8 (WRAP / WRAP)
            res.texturing[i]->SetMap(0, map);         // (the old base map deleted)
            const ni::NiPoint2 sc = L.scales[i];
            if (std::bit_cast<u32>(sc.u) != 0x3F800000u || std::bit_cast<u32>(sc.v) != 0x3F800000u)
                for (ni::NiPoint2& uv : res.polygons[i]->uvs) {   // FLD; FMUL scale; FADD 1.0; FSTP
                    uv.u = F(double(uv.u) * sc.u + 1.0);
                    uv.v = F(double(uv.v) * sc.v + 1.0);
                }
            SetLayerBlend(*res.alpha[i], L.modes[i]);
        }
    if (colorMode) {
        ni::NiGeometryData& d = *g->modelData;
        if (d.colors.empty()) d.colors.assign(d.vertexCount, {});   // orig: CreateColors 0x00143C40 (zeroed)
        if (auto* vc = dynamic_cast<ni::NiVertexColorProperty*>(GetProperty(*g, 9))) {
            vc->sourceVertexMode = 1;
            UpdateColors();
        } else {
            geometry = nullptr;                       // (the colours stay allocated, the mode unchanged)
        }
    }
    return true;
}

// ---- inventory_batch1_port.md 4.2 (checked against the assembly of 0x0006A640 and 0x0006B010) ----
bool Liquid::Tick() {                                 // orig: vf08 0x0006A640
    if (!geometry) return false;                      // +0x74: deleted by the stage's object loop
    sinceUpdate = F(double(g_frameStep) + sinceUpdate);   // FLD step; FADD +0x70; FSTP
    time = F(double(g_frameStep) + time);             // +0x24
    return true;
}

bool Liquid::PostUpdate() {                           // orig: vf0C 0x0006B010 (the result is ignored)
    ni::NiGeometry* g = geometry.Get();
    if (!g) return false;
    if (g->RefCount() == 1) {                         // +0x74->+4 == 1: the stage geometry is gone; drop the last
        geometry = nullptr;                           //  reference (the next Tick deletes the liquid)
        return false;
    }
    if (!g->modelData) return false;                  // +0xC8
    if (!resource) return false;                      // +0x3C
    LiquidResource& res = *resource;
    if (res.stamp != 0 && stamp != 0 && g->framesNotDrawn != 0) return true;   // res+0x14, +0x38, geometry +0xB2
    u32 layers = 0;                                   // [esp+0xC]
    if (res.stamp == stamp) {                         // the first liquid of this type this frame:
        ++res.stamp;                                  //  res+0x14++, layers = res+0x18
        layers = res.layerCount;
    }
    stamp = res.stamp;                                // +0x38
    // 0x0006B09B: each layer's texture coordinates scroll by this liquid's velocity times +0x70 (x87: the steps du /
    // dv stay in the registers; a wrap of the offset moves them by 1 too)
    for (u32 i = 0; i < layers && i < res.polygons.size(); ++i) {
        ni::NiScreenPolygon& p = *res.polygons[i];
        if (p.uvs.empty()) continue;                  // polygon +0x14 null
        const ni::NiPoint2 v = layerVel[i];           // +0x48 + 8i
        if (v.u == 0.0f && v.v == 0.0f) continue;     // FCOM [0.0]; TEST AH,0x44; JP (NaN scrolls)
        double du = double(v.u) * sinceUpdate;        // FMUL +0x70
        double dv = double(v.v) * sinceUpdate;
        ni::NiPoint2& o = layerOfs[i];                // +0x78 + 8i
        o.u = F(du + o.u);
        o.v = F(dv + o.v);
        if (o.u > 1.0f) {                             // FCOMP [1.0]; TEST AH,0x41; JNE
            du -= 1.0;
            o.u = F(double(o.u) - 1.0);
        } else if (o.u < -1.0f) {                     // FCOMP [-1.0]; TEST AH,5; JP
            du += 1.0;
            o.u = F(double(o.u) + 1.0);
        }
        if (o.v > 1.0f) {
            dv -= 1.0;
            o.v = F(double(o.v) - 1.0);
        } else if (o.v < -1.0f) {
            dv += 1.0;
            o.v = F(double(o.v) + 1.0);
        }
        for (size_t k = 0; k < p.vertexCount && k < p.uvs.size(); ++k) {   // (the u16 count +0xC)
            p.uvs[k].u = F(du + p.uvs[k].u);
            p.uvs[k].v = F(dv + p.uvs[k].v);
        }
        p.flags |= 4;                                 // OR byte +0x1C, 4
    }
    if (layers) {                                     // 0x0006B1C9: the layers into the resource's texture
        ni::NiRenderer* r = level.renderer;           // App->vf14() (the play tests have none: a no-op there)
        if (r) {
            ni::NiCamera& cam = *res.camera;
            cam.renderer = r;                         // 0x00013D40 (+0x1A8)
            r->SetRenderTarget(res.texture.Get());    // vf3C
            ni::VisitForCulling(cam, nullptr);        // NiCamera::Click 0x00100CE0 (+0x15C null: the polygons)
            r->DisplayFrame();                        // 0x000FEF50: vf40 (a rendered texture is not presented)
            r->SetRenderTarget(nullptr);              // vf3C(0): the back buffer
        }
    }
    UpdateColors();                                   // 0x0006A670
    sinceUpdate = 0;                                  // +0x70
    return true;
}

void Liquid::UpdateColors() {                         // orig: 0x0006A670 (x87; FSIN / FCOS: C library, #20)
    if (!geometry || !geometry->modelData) return;
    ni::NiGeometryData& d = *geometry->modelData;
    const double t = time;
    const auto& b = colorBase;
    const auto& a = colorAmp;
    if (colorMode == 1) {
        for (ni::NiColorA& c : d.colors)
            c = {F(double(b[0]) + a[0]), F(double(b[1]) + a[1]), F(double(b[2]) + a[2]), F(double(b[3]) + a[3])};
        d.revisionFlags |= 4;
        colorMode = 0;
        return;
    }
    if (colorMode != 3 && colorMode != 4) return;
    for (size_t i = 0; i < d.colors.size() && i < d.vertices.size(); ++i) {
        const double x = d.vertices[i].x, z = d.vertices[i].z;
        const double s = std::sin(t + x);
        const double c1 = std::cos(s * double(2.14f) + z * double(0.11f));
        const double s2 = std::sin(std::cos(t + z) * double(2.12f) + x * double(0.11f));
        const double f = (c1 + s2) * double(0.2f) + double(0.6f);
        ni::NiColorA& c = d.colors[i];
        c.r = F(double(b[0]) + F(double(a[0]) * f));
        c.g = F(double(b[1]) + F(double(a[1]) * f));
        if (colorMode == 3) {
            c.b = F(double(b[2]) + F(double(a[2]) * f));
            c.a = F(double(b[3]) + double(a[3]) * f);
        } else {
            c.b = F(double(b[2]) + double(a[2]) * f);
            c.a = F(double(b[3]) + F(double(a[3]) * f));
            d.vertices[i].y = F(f * waveHeight + baseY);
        }
    }
    d.revisionFlags |= colorMode == 3 ? 4 : 5;
}

}  // namespace fut::game

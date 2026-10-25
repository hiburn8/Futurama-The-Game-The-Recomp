// The respawn effect object; see RespawnEffect.h and docs/analysis/quicksave_port.md 4 (transcribed from the assembly
// of 0x000715D0, 0x00070E30, 0x00070BE0, 0x00070D80 and 0x00070AF0). The recharge effect: transcribed from 0x000708D0,
// 0x00070120, 0x0006FF40, 0x00070070 and 0x0006FE90 (2026-10-18). The charge-up effect: from 0x00066290, 0x000660E0,
// 0x00066380, 0x000661E0, 0x00065E60 and 0x00066150 (2026-10-18).
#include "game/object/RespawnEffect.h"

#include "engine/ni/NiClone.h"
#include "engine/ni/NiMath.h"
#include "game/GameTime.h"
#include "game/defs/Defs.h"
#include "game/defs/EffectDefs.h"
#include "game/level/Camera.h"
#include "game/level/Hud.h"
#include "game/level/LevelData.h"
#include "game/object/Character.h"
#include "game/object/NodeUpdate.h"

#include <bit>
#include <cmath>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

f32 Bits(u32 b) { return std::bit_cast<f32>(b); }

// orig: new(0xF0) NiBillboardNode inline (NiNode ctor 0x00105C10, vtbl 0x0029BF60, +0xE0 = 0, +0xE4 = 1, flags
// (f & 0xE7FF) | 0x400), then flags &= 0xE3FF (the mode bits cleared again).
ni::NiPointer<ni::NiBillboardNode> NewBillboard() {
    ni::NiPointer<ni::NiBillboardNode> b(new ni::NiBillboardNode);
    b->flags = static_cast<u16>((b->flags & 0xE7FF) | 0x0400);
    b->flags = static_cast<u16>(b->flags & 0xE3FF);
    return b;
}

// orig: new(0x5C) NiMaterialProperty 0x00114B10; +0x40..+0x48 = (0.6, 0.6, 1.0) (L: the emissive colour, as the
// ripple's), +0x54 incremented twice, +0x50 (alpha) = 1.0.
ni::NiPointer<ni::NiMaterialProperty> NewMaterial() {
    ni::NiPointer<ni::NiMaterialProperty> m(new ni::NiMaterialProperty);
    m->emissive = {Bits(0x3F19999Au), Bits(0x3F19999Au), 1.0f};
    ++m->revision;
    ++m->revision;
    m->alpha = 1.0f;
    return m;
}

}  // namespace

// ---- the definition --------------------------------------------------------------------------------------------------
// orig: 0x000715D0 (thiscall def; (owner, NiPoint3* pos, label), RET 0xC).
GameObject* RespawnEffectDef::SpawnAt(LevelData& level, Owner* owner, const NiPoint3& pos, const char* label) {
    if (!texture) return nullptr;                     // D+0x18
    auto* o = new RespawnEffect(level, *this);        // new(0x58) + GameObject 0x00096CA0; vtbl 0x0029C750
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    o->SetPosition(pos);                              // vf1C (UpdateRoom)
    (void)o->Init();                                  // vf04
    return o;
}

// ---- the object -------------------------------------------------------------------------------------------------------
RespawnEffect::RespawnEffect(LevelData& lvl, RespawnEffectDef& d) : GameObject(lvl, d) {}   // +0x38..+0x54 = 0, +0x4C = 1.0

RespawnEffect::~RespawnEffect() {                     // orig: 0x00070AF0: +0x54, +0x50, +0x48, +0x44, +0x40, +0x3C, +0x38
    cloneBillboard = nullptr;
    billboard = nullptr;
    alphaProp = nullptr;
    cloneMaterial = nullptr;
    material = nullptr;
    quadClone = nullptr;
    quad = nullptr;
}

bool RespawnEffect::Init() {                          // orig: vf04 0x00070E30
    if (level.player) level.player->GetNode()->flags |= ni::kNiAVHidden;   // (S+0x94)->vf30 +0xB0 |= 1 (no null test)
    const auto& d = static_cast<const RespawnEffectDef&>(*def);
    value4C = Bits(0x3ECCCCCDu);                      // +0x4C = 0.4
    // NiTriShape(4, v, 0, 0, uv, 2, tris) 0x001164E0 with new(0x30) / new(0x20) / new(0xC): the quad on the xy plane
    ni::NiPointer<ni::NiTriShapeData> data(new ni::NiTriShapeData);
    data->vertexCount = 4;
    data->vertices = {{-10.0f, -10.0f, 0.0f}, {-10.0f, 10.0f, 0.0f}, {10.0f, 10.0f, 0.0f}, {10.0f, -10.0f, 0.0f}};
    data->bound = ni::BoundOfPoints(data->vertices);  // (the data ctor's bound, as FireDef's quad)
    data->textureSets = 1;
    data->uvs = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
    data->triangleCount = 2;
    data->triangleList = {0, 1, 2, 0, 2, 3};
    quad = new ni::NiTriShape;                        // new(0xE0)
    quad->modelData = data.Get();
    alphaProp = new ni::NiAlphaProperty;              // new(0x1C), vtbl 0x00293B54: flags 0xEC, +0x18 = 0
    u16 fl = 0xEC;
    fl |= 1;                                          // +0x14 |= 1
    fl &= ~0x200;                                     // +0x15 &= 0xFD
    fl = static_cast<u16>((fl & 0xFFED) | 0xC);
    fl &= 0xFE1F;
    alphaProp->flags = fl;
    alphaProp->alphaTestRef = 0;
    AttachProperty(*quad, alphaProp.Get());           // AddHead 0x00013A50
    {                                                 // new(0x3C) 0x00109DC0: SetBaseTexture(D+0x18) 0x0001D610,
        ni::NiPointer<ni::NiTexturingProperty> tex = MakeTexturing(d.texture.Get());   // apply mode 2,
        AttachProperty(*quad, tex.Get());             // SetBaseFilterMode(1) 0x0001D6B0
    }
    auto* z = new ni::NiZBufferProperty;              // new(0x1C), vtbl 0x00296C80: +0x14 = 3, +0x18 = 3,
    z->flags = static_cast<u16>(3 & 0xFC);            //  then the flags' low byte &= 0xFC
    z->testFunction = 3;
    AttachProperty(*quad, z);
    // new(0x38) vtbl 0x002949E4 (NiStencilProperty, the inline constructor), then +0x18 = 0, +0x34 = 3
    {                                                 // (the constructor's defaults: NiObjects.h)
        auto* st = new ni::NiStencilProperty;
        st->enabled = false;                          // byte +0x18 = 0
        st->drawMode = 3;                             // +0x34 = 3: both faces (D3DCULL_NONE)
        AttachProperty(*quad, st);
    }
    auto* vc = new ni::NiVertexColorProperty;         // new(0x20), vtbl 0x00293AD0: +0x18 = 0, +0x1C = 0
    vc->sourceVertexMode = 0;
    vc->lightingMode = 0;
    AttachProperty(*quad, vc);
    {
        ni::NiPointer<ni::NiObject> c = ni::Clone(quad.Get());   // NiObject::Clone 0x000FE120
        quadClone = dynamic_cast<ni::NiTriShape*>(c.Get());   // DynamicCast (RTTI 0x002F9990 NiTriShape)
    }
    billboard = NewBillboard();                       // +0x50
    cloneBillboard = NewBillboard();                  // +0x54
    node->AttachChild(billboard.Get(), true);         // vfA0(+0x50, 1)
    node->AttachChild(cloneBillboard.Get(), true);    // vfA0(+0x54, 1)
    material = NewMaterial();                         // +0x40
    AttachProperty(*quad, material.Get());
    cloneMaterial = NewMaterial();                    // +0x44
    if (quadClone) AttachProperty(*quadClone, cloneMaterial.Get());   // (port guard: the original has no null test)
    billboard->AttachChild(quad.Get(), true);         // vfA0(+0x38, 1)
    cloneBillboard->AttachChild(quadClone.Get(), true);   // vfA0(+0x3C, 1)
    quad->flags &= ~ni::kNiAVHidden;                  // +0xB0 &= 0xFE
    quad->local.translate = {};                       // 0x002F915C (zero)
    if (quadClone) quadClone->local.translate = {};
    node->UpdateProperties();                         // 0x00071551
    node->UpdateEffects();
    UpdateNodeAt(node.Get(), 0.0f);                   // NiAVObject::Update(0.0, 1)
    quad->modelData->revisionFlags |= 1;              // (+0x38)->+0xC8 ->+0x2E |= 1
    if (quadClone && quadClone->modelData) quadClone->modelData->revisionFlags |= 1;   // (+0x3C)->+0xC8 ->+0x2E |= 1
    AttachNoFog(*node);                               // 0x00013E40 (+ UpdateProperties)
    UpdateRoom();                                     // 0x00096F20
    return true;
}

bool RespawnEffect::Tick() {                          // orig: vf08 0x00070BE0
    if (time > 1.0f && level.player)                  // FCOMP [0x00293A98]; TEST AH,0x41; JNE (only '>')
        level.player->GetNode()->flags &= ~ni::kNiAVHidden;   // (S+0x94)->vf30 +0xB0 &= 0xFE
    if (time > Bits(0x3FE00000u)) {                   // FCOMP [0x00293C28] (1.75); TEST AH,0x41; JNE
        if (billboard) (void)billboard->DetachChild(quad.Get());   // vfA4(&tmp, +0x38); +0x38 released; tmp released
        quad = nullptr;
        if (cloneBillboard) (void)cloneBillboard->DetachChild(quadClone.Get());   // vfA4(&tmp, +0x3C)
        quadClone = nullptr;
        (void)node->DetachChild(billboard.Get());    // vfA4(&tmp, +0x50) (+0x54 stays under the node)
        billboard = nullptr;
        return false;                                 // deleted by the Session update
    }
    const double t = double(g_frameStep) + time;      // FLD step; FADD +0x24; FST +0x24
    time = F(t);
    time = F(t + g_frameStep);                        // FADD step (the unrounded sum); FSTP +0x24
    return true;
}

bool RespawnEffect::PostUpdate() {                    // orig: vf0C 0x00070D80 (x87 FSIN: the C library, #20)
    const double t = time;
    const double a = std::fabs(std::sin(t * Bits(0x3FE66666u)));   // FMUL [0x0029A250] (1.8); FSIN; FABS (kept)
    const f32 s1 = F(std::sin(t * Bits(0x3FB851EBu)));   // FMUL [0x0029C7AC] (1.44); FSIN; FSTP local 0
    const f32 s2 = F(std::sin(t * Bits(0x3F8A3D71u)));   // FMUL [0x0029C7A8] (1.08); FSIN; FSTP local 1
    ++material->revision;                             // +0x40 ->+0x54
    material->alpha = F(a);                           // FST +0x50
    cloneMaterial->alpha = F(a);                      // FSTP (+0x44)+0x50
    ++cloneMaterial->revision;
    billboard->local.rotate = ni::AxisAngle(F(t * Bits(0x402A3D71u)), 0.0f, 0.0f, 1.0f);   // [0x0029C7A4] (2.66)
    cloneBillboard->local.rotate = ni::AxisAngle(F(t * Bits(0xC02A3D71u)), 0.0f, 0.0f, 1.0f);   // [0x0029C7A0]
    billboard->local.scale = std::fabs(s1);           // FLD local 0; FABS; FABS; FSTP +0x6C
    cloneBillboard->local.scale = std::fabs(s2);
    return true;
}

// ---- the recharge effect ------------------------------------------------------------------------------------------------
// orig: 0x000708D0 (thiscall def; (owner, label, n), RET 0xC).
GameObject* RechargeEffectDef::SpawnAt(LevelData& level, Owner* owner, const char* label, s32 n) {
    if (!textures[static_cast<size_t>(n)]) return nullptr;   // D+0x18 + 4n
    auto* o = new RechargeEffect(level, *this);       // new(0x5C) + GameObject 0x00096CA0; vtbl 0x0029C688
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    NiPoint3 p = level.player->GetWorldPosition();    // (S+0x94)->vf34 (no null test)
    p.y = F(double(p.y) + 12.0f);                     // FADD [0x00297638]
    o->SetPosition(p);                                // vf1C
    o->SetUp(n);                                      // vf50
    return o;
}

RechargeEffect::RechargeEffect(LevelData& lvl, RechargeEffectDef& d) : GameObject(lvl, d) {}   // +0x38..+0x48 = 0, +0x44 = 1

RechargeEffect::~RechargeEffect() {                   // orig: 0x0006FE90: +0x48, +0x40, +0x3C, +0x38
    billboard = nullptr;
    alphaProp = nullptr;
    material = nullptr;
    quad = nullptr;
}

void RechargeEffect::SetUp(s32 n) {                   // orig: vf50 0x00070120
    level4C = n;
    value44 = Bits(0x3ECCCCCDu);                      // +0x44 = 0.4
    start = level.player->GetWorldPosition();         // (S+0x94)->vf34 -> +0x50
    const auto& d = static_cast<const RechargeEffectDef&>(*def);   // (+0x10, kept in the argument slot)
    // NiTriShape(4, v, 0, 0, uv, 2, tris) 0x001164E0 with new(0x30) / new(0x20) / new(0xC): a 2 x 2 quad
    ni::NiPointer<ni::NiTriShapeData> data(new ni::NiTriShapeData);
    data->vertexCount = 4;
    data->vertices = {{-1.0f, -1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}};
    data->bound = ni::BoundOfPoints(data->vertices);
    data->textureSets = 1;
    data->uvs = {{0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
    data->triangleCount = 2;
    data->triangleList = {0, 1, 2, 0, 2, 3};
    quad = new ni::NiTriShape;                        // new(0xE0) -> +0x38
    quad->modelData = data.Get();
    alphaProp = new ni::NiAlphaProperty;              // new(0x1C), vtbl 0x00293B54: flags 0xEC, +0x18 = 0 -> +0x40
    u16 fl = 0xEC;
    fl |= 1;                                          // +0x14 |= 1
    fl &= ~0x200;                                     // +0x15 &= 0xFD
    fl = static_cast<u16>((fl & 0xFFED) | 0xC);
    fl = static_cast<u16>((fl & 0xFEFF) | 0xE0);
    alphaProp->flags = fl;
    alphaProp->alphaTestRef = 0;
    AttachProperty(*quad, alphaProp.Get());           // AddHead 0x00013A50
    {                                                 // new(0x3C) 0x00109DC0: SetBaseTexture(D+0x18 + 4n) 0x0001D610,
        ni::NiPointer<ni::NiTexturingProperty> tex = MakeTexturing(d.textures[static_cast<size_t>(n)].Get());
        AttachProperty(*quad, tex.Get());             // apply mode 2, SetBaseFilterMode(1) 0x0001D6B0
    }
    auto* z = new ni::NiZBufferProperty;              // new(0x1C), vtbl 0x00296C80: +0x14 = 3, +0x18 = 3,
    z->flags = static_cast<u16>(3 & 0xFC);            //  then the flags' low byte &= 0xFC
    z->testFunction = 3;
    AttachProperty(*quad, z);
    // new(0x38) vtbl 0x002949E4 (NiStencilProperty, the inline constructor), then +0x18 = 0, +0x34 = 3
    {                                                 // (the constructor's defaults: NiObjects.h)
        auto* st = new ni::NiStencilProperty;
        st->enabled = false;                          // byte +0x18 = 0
        st->drawMode = 3;                             // +0x34 = 3: both faces (D3DCULL_NONE)
        AttachProperty(*quad, st);
    }
    auto* vc = new ni::NiVertexColorProperty;         // new(0x20), vtbl 0x00293AD0: +0x18 = 0, +0x1C = 0
    vc->sourceVertexMode = 0;
    vc->lightingMode = 0;
    AttachProperty(*quad, vc);
    billboard = NewBillboard();                       // +0x48
    node->AttachChild(billboard.Get(), true);         // vfA0(+0x48, 1)
    material = NewMaterial();                         // +0x3C
    AttachProperty(*quad, material.Get());
    billboard->AttachChild(quad.Get(), true);         // vfA0(+0x38, 1)
    quad->flags &= ~ni::kNiAVHidden;                  // +0xB0 &= 0xFE
    quad->local.translate = {};                       // 0x002F915C (zero)
    node->UpdateProperties();                         // 0x00101EA0
    node->UpdateEffects();                            // 0x00102190
    UpdateNodeAt(node.Get(), 0.0f);                   // NiAVObject::Update(0.0, 1)
    quad->modelData->revisionFlags |= 1;              // (+0x38)->+0xC8 ->+0x2E |= 1
    AttachNoFog(*node);                               // 0x00013E40
    UpdateRoom();                                     // 0x00096F20
}

bool RechargeEffect::Tick() {                         // orig: vf08 0x0006FF40
    if (time > 2.0f) {                                // FCOMP [0x00293B20]; TEST AH,0x41; JNE (only '>')
        if (billboard) (void)billboard->DetachChild(quad.Get());   // vfA4(&tmp, +0x38) (+0x48 not tested)
        quad = nullptr;
        (void)node->DetachChild(billboard.Get());    // vfA4(&tmp, +0x48)
        billboard = nullptr;
        return false;                                 // deleted by the stage update
    }
    UpdateNodeAt(node.Get(), time);                   // NiAVObject::Update(+0x24, 1)
    const double t = double(g_frameStep) + time;      // FLD step; FADD +0x24; FST +0x24
    time = F(t);
    time = F(t + g_frameStep);                        // FADD step (the unrounded sum); FSTP +0x24
    return true;
}

// ---- the charge-up effect ------------------------------------------------------------------------------------------------
// orig: 0x00066290 (thiscall def; (owner, NiPoint3* pos, n, label), RET 0x10).
GameObject* ChargeUpEffectDef::SpawnAt(LevelData& level, Owner* owner, const NiPoint3& p, s32 n, const char* label) {
    if (!texture || !texture2) return nullptr;        // D+0x18 / D+0x1C
    pos = p;                                          // D+0x20
    slot = static_cast<u8>(n - 1);                    // DEC DL -> D+0x2C
    auto* o = new ChargeUpEffect(level, *this);       // new(0x78) 0x000660E0
    o->SetOwner(owner);                               // vf10
    o->SetName(label);                                // 0x000968C0
    (void)o->Init();                                  // vf04
    return o;
}

ChargeUpEffect::ChargeUpEffect(LevelData& lvl, ChargeUpEffectDef& d) : GameObject(lvl, d) {   // orig: 0x000660E0
    slots = {{{Bits(0x3E0B3333u), Bits(0x3F2D5555u)}, {Bits(0x3E366666u), Bits(0x3F333333u)},
              {Bits(0x3E580000u), Bits(0x3F3E6666u)}, {Bits(0x3E64CCCDu), Bits(0x3F4CCCCDu)}}};   // +0x50..+0x6C
}

ChargeUpEffect::~ChargeUpEffect() {                   // orig: 0x00066150: +0x40, +0x3C, +0x38
    alphaProp = nullptr;
    material = nullptr;
    polygon = nullptr;
}

bool ChargeUpEffect::Init() {                         // orig: vf04 0x00066380
    ni::NiCamera* cam = level.camera ? level.camera->sceneCamera.Get() : nullptr;   // (S+0x8C)+0x30
    if (!cam) return false;
    const auto& d = static_cast<const ChargeUpEffectDef&>(*def);
    const f32 w = F(double(size) * Bits(0x3ACCCCCDu));   // FMUL [0x0029BC18] (1 / 640)
    const f32 h = F(double(size) * Bits(0x3B088889u));   // FMUL [0x0029BC14] (1 / 480)
    slot = d.slot;                                    // +0x74 = D+0x2C
    const ni::NiPoint2 uv[4] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    (void)cam->WorldPtToScreenPt(d.pos, x, y);        // 0x000FEB10(D+0x20, +0x48, +0x4C) (the result unused)
    const double x0 = double(x) - double(size) * Bits(0x3A4CCCCDu);   // FMUL [0x0029BC20]; FSUBP
    const double y0 = double(size) * Bits(0x3A888889u) + y;           // FMUL [0x0029BC1C]; FADD
    const double y1 = y0 - h;                         // FSUBR
    const double x1 = x0 + w;
    const NiPoint3 v[4] = {{F(x0), F(y1), 0.0f}, {F(x1), F(y1), 0.0f}, {F(x1), F(y0), 0.0f}, {F(x0), F(y0), 0.0f}};
    polygon = new ni::NiScreenPolygon(4, v, uv, nullptr);   // new(0x24) 0x00106DE0
    {                                                 // new(0x3C) 0x00109DC0: SetBaseTexture(D+0x18) 0x0001D610, apply
        ni::NiPointer<ni::NiTexturingProperty> tex = MakeTexturing(d.texture.Get());   // mode 2, filter 1 (0x0001D6B0)
        polygon->AttachProperty(tex.Get());           // (the polygon's property state, vf2C's slot)
    }
    material = new ni::NiMaterialProperty;            // new(0x5C) 0x00114B10 -> +0x3C
    const ni::NiColor c{Bits(0x3F4CCCCDu), Bits(0x3F4CCCCDu), 1.0f};   // (0.8, 0.8, 1.0)
    material->ambient = c;                            // +0x1C, each change bumps +0x54
    ++material->revision;
    material->diffuse = c;                            // +0x28
    ++material->revision;
    material->specular = c;                           // +0x34
    ++material->revision;
    material->emissive = c;                           // +0x40
    ++material->revision;
    material->shininess = 0.0f;                       // +0x4C
    ++material->revision;
    material->alpha = 1.0f;                           // +0x50
    ++material->revision;
    polygon->AttachProperty(material.Get());
    auto* a = new ni::NiAlphaProperty;                // new(0x1C), vtbl 0x00293B54: flags 0xEC, +0x18 = 0
    a->flags = static_cast<u16>((0xEC & 0xFE0D) | 0xD);
    a->alphaTestRef = 0;
    polygon->AttachProperty(a);
    cam->AttachScreenPolygon(polygon.Get());          // 0x00019460
    return true;
}

bool ChargeUpEffect::Move(f32 rate) {                 // orig: 0x00065E60
    const auto& t = slots[slot];                      // +0x50 + 8 * +0x74
    if (!(double(t[0]) + 0.01f < x)) return true;     // FADD [0x00293AAC]; FCOMP +0x48; TEST AH,1; JNE (NaN: here)
    x = F(double(x) - (double(x) - t[0]) * rate * 0.1f);   // FSUB; FMUL rate; FMUL [0x00293AB0]; FSUBR; FSTP
    const double ny = double(y) - (double(y) - t[1]) * rate * 0.1f;
    const f32 nyf = F(ny);                            // FST esp+4
    y = nyf;                                          // (the unrounded value stored: the same float)
    const double se = std::fabs(std::cos(double(time) * Bits(0x40533333u))) * size;   // [0x0029BC24]; FCOS; FABS
    const f32 s = F(se);                              // FST (the extended value is used once more)
    const double X0 = double(x) - se * Bits(0x3A4CCCCDu);   // FMUL [0x0029BC20]; FSUBR [esp] (the new x)
    const double Y0 = double(s) * Bits(0x3A888889u) + nyf;  // FMUL [0x0029BC1C]; FADD
    const f32 A = F(double(s) * Bits(0x3ACCCCCDu));   // FMUL [0x0029BC18]; FSTP
    const double B = double(s) * Bits(0x3B088889u);   // FMUL [0x0029BC14] (kept)
    const double Y1 = Y0 - B;
    const double X1 = double(A) + X0;
    auto& v = polygon->vertices;                      // +0x10
    v[0].x = F(X0);
    v[0].y = F(Y1);
    v[1].x = F(X1);
    v[1].y = F(Y1);
    v[2].x = F(X1);
    v[2].y = F(Y0);
    v[3].x = F(X0);
    v[3].y = F(Y0);
    polygon->flags |= 1;                              // +0x1C |= 1
    return false;
}

bool ChargeUpEffect::Tick() {                         // orig: vf08 0x000661E0
    if (Move(1.0f)) {                                 // 0x00065E60(1.0)
        if (level.camera && level.camera->sceneCamera)   // (S+0x8C)+0x30 (no tests)
            level.camera->sceneCamera->DetachScreenPolygon(polygon.Get());   // 0x00100FC0
        polygon = nullptr;
        // (S+0x118)->0x000E5B90("charge", "charge", (f32)(+0x74 + 1)) (FILD)
        if (level.hud) level.hud->Command("charge", "charge", static_cast<f32>(static_cast<s32>(slot) + 1));
        return false;
    }
    time = F(double(g_frameStep) + time);             // FLD [0x002D67E8]; FADD +0x24; FSTP
    return true;
}

bool RechargeEffect::PostUpdate() {                   // orig: vf0C 0x00070070 (x87 FSIN: the C library, #20)
    const double t = time;
    f32 a = F(1.0 - t * 0.5f);                        // FMUL [0x00293AA0]; FSUBR [0x00293A98]; FSTP
    const f32 s = F(std::fabs(std::sin(t * Bits(0x3F4CCCCDu))) + 1.0f);   // FMUL [0x00293C18]; FSIN; FABS; FADD 1.0
    NiPoint3 p = level.player->GetWorldPosition();    // (S+0x94)->vf34 (no null test)
    p.y = F((t * 3.0f + start.y) + 12.0f);            // FMUL [0x0029563C]; FADD +0x54; FADD [0x00297638]
    if (a < 0.0f) a = 0.0f;                           // FCOM [0x00293AB8]; TEST AH,5; JP (NaN kept)
    material->alpha = a;                              // +0x3C ->+0x50
    ++material->revision;                             // ->+0x54
    billboard->local.scale = std::fabs(s);            // FABS; FSTP (+0x48)+0x6C
    node->local.translate = p;                        // +0x2C ->+0x60
    return true;
}

}  // namespace fut::game

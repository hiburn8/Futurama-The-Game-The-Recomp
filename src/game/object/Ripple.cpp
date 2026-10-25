// The Ripple effect object; see Ripple.h and docs/analysis/inventory_batch1_port.md 4.3 (checked against the assembly
// of 0x00071B10, 0x00071C50, 0x000716C0, 0x000718E0, 0x00071850 and the RippleDef ctor 0x00071710).
#include "game/object/Ripple.h"

#include "engine/ni/NiMath.h"
#include "game/GameTime.h"
#include "game/defs/EffectDefs.h"
#include "game/level/LevelData.h"
#include "game/object/NodeUpdate.h"

#include <cmath>
#include <memory>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

// ---- the definition --------------------------------------------------------------------------------------------------
// orig: 0x00071B10 (thiscall def; (owner, pos*, rot*, alpha, sx, sz, gx, gz, yOffset), RET 0x24).
GameObject* RippleDef::SpawnRipple(LevelData& level, Owner* owner, const NiPoint3& pos, const ni::NiMatrix3& rot,
                                   f32 alpha, f32 sx, f32 sz, f32 gx, f32 gz, f32 yOffset) {
    if (!texture) return nullptr;                     // D+0x18
    auto* o = new Ripple(level, *this);               // new(0x68) + GameObject 0x00096CA0; vtbl 0x0029C7E0
    if (yOffset == 3.40282347e38f) yOffset = value1C; // FCOMP FLT_MAX [0x00295604]; TEST AH,0x44; JP (dword copy)
    const NiPoint3 p{pos.x, F(double(yOffset) + pos.y), pos.z};   // FLD yOff; FADD pos.y; FSTP (z: FLD / FSTP)
    o->SetOwner(owner);                               // vf10
    o->SetPosition(p);                                // vf1C 0x00097140 (UpdateRoom)
    o->SetRotation(rot);                              // vf24 0x00096A30
    o->sizeX = sx, o->sizeZ = sz;                     // +0x58 / +0x5C (dwords)
    o->endX = gx, o->endZ = gz;                       // +0x60 / +0x64
    o->rate = alpha;                                  // +0x48 (read by Init as the start alpha)
    o->Init();                                        // vf04 0x00071C50
    return o;
}

// orig: new(0x34) + RippleDef::RippleDef 0x00071710(name, texture, yOffset): the texture by name (0x000C2A80),
// +0x1C = yOffset, +0x20 = 0.3, +0x24..+0x2C = 1.0, +0x30 = 0; Def::Def 0x000962E0 appends it to the level's list.
RippleDef* NewRippleDef(LevelData& level, const char* name, const char* texture, f32 yOffset) {
    auto d = std::make_unique<RippleDef>();
    d->name = name;
    d->texture = level.FindTexture(texture);
    d->value1C = yOffset;
    return static_cast<RippleDef*>(level.defs->Add(std::move(d)));
}

// ---- the object -------------------------------------------------------------------------------------------------------
Ripple::Ripple(LevelData& lvl, RippleDef& d) : GameObject(lvl, d) {}   // +0x38 = +0x3C = +0x40 = 0, +0x48 = 1.0f

Ripple::~Ripple() {                                   // orig: 0x00071850: +0x40, +0x3C, +0x38, then ~GameObject
    alphaProp = nullptr;
    material = nullptr;
    quad = nullptr;
}

bool Ripple::Init() {                                 // orig: vf04 0x00071C50
    const auto& d = static_cast<const RippleDef&>(*def);
    acc = 0;                                          // +0x4C
    rangeX = F(double(endX) - sizeX);                 // +0x50 = F(+0x60 - +0x58)
    rangeZ = F(double(endZ) - sizeZ);                 // +0x54 = F(+0x64 - +0x5C)
    alpha = rate;                                     // FLD +0x48; FST +0x44
    if (rate > 1.0f) alpha = 1.0f;                    // FCOMP [0x00293A98]; TEST AH,0x41; JNE
    rate = d.value20;                                 // +0x48 = D+0x20 (dword)
    ni::NiPointer<ni::NiTriShapeData> data(new ni::NiTriShapeData);   // new(0x30) / new(0x20) / new(0xC)
    data->vertexCount = 4;
    data->vertices = {{-sizeX, 0.0f, -sizeZ}, {-sizeX, 0.0f, sizeZ}, {sizeX, 0.0f, sizeZ}, {sizeX, 0.0f, -sizeZ}};   // FCHS
    data->textureSets = 1;
    data->uvs = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}};
    data->triangleCount = 2;
    data->triangleList = {0, 1, 2, 0, 2, 3};
    quad = new ni::NiTriShape;                        // new(0xE0) NiTriShape(4, v, 0, 0, uv, 2, tris) 0x001164E0
    quad->modelData = data.Get();
    material = new ni::NiMaterialProperty;            // new(0x5C) 0x00114B10
    material->emissive = {d.value24[0], d.value24[1], d.value24[2]};   // +0x40..+0x48 = D+0x24..+0x2C (L: emissive)
    ++material->revision;
    ++material->revision;                             // (two increments of +0x54)
    material->alpha = 0.0f;                           // +0x50
    AttachProperty(*quad, material.Get());            // AddHead 0x00013A50
    alphaProp = new ni::NiAlphaProperty;              // new(0x1C), vtbl 0x00293B54: flags 0xEC, +0x18 = 0
    u16 fl = 0xEC;
    fl |= 1;                                          // +0x14 |= 1
    fl &= ~0x200;                                     // +0x15 &= 0xFD
    fl = (fl & 0xFFED) | 0xC;
    fl = d.value30 ? u16(fl & 0xFE1F) : u16((fl & 0xFEFF) | 0xE0);   // D+0x30
    alphaProp->flags = fl;
    alphaProp->alphaTestRef = 0;
    AttachProperty(*quad, alphaProp.Get());
    {                                                 // new(0x3C) 0x00109DC0: SetBaseTexture(D+0x18) 0x0001D610,
        ni::NiPointer<ni::NiTexturingProperty> tex = MakeTexturing(d.texture.Get());   // apply mode 2,
        AttachProperty(*quad, tex.Get());             // SetBaseFilterMode(1) 0x0001D6B0
    }
    auto* z = new ni::NiZBufferProperty;              // new(0x1C), vtbl 0x00296C80: +0x14 = 3, +0x18 = 3,
    z->flags = u16((3 & 0xFFFD) | 1);                 //  then flags = (flags & 0xFFFD) | 1
    z->testFunction = 3;
    AttachProperty(*quad, z);
    // new(0x38) vtbl 0x002949E4 (NiStencilProperty, the inline constructor 0x00072070), then +0x18 = 0, +0x34 = 3
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
    // the model bound: centre ZERO, radius F(sqrt(gz * gz + gx * gx)) (FLD +0x64; FLD +0x60; ...; FSQRT)
    data->bound.center = {};
    data->bound.radius = F(std::sqrt(double(endZ) * endZ + double(endX) * endX));
    node->AttachChild(quad.Get(), true);              // vfA0(+0x38, 1)
    node->UpdateProperties();                         // 0x0007217D
    node->UpdateEffects();
    node->UpdateWorldBound();                         // vf98
    UpdateNodeAt(node.Get(), 0.0f);                   // NiAVObject::Update(0.0, 1)
    data->revisionFlags |= 1;                         // (+0x38)->+0xC8 ->+0x2E |= 1
    UpdateRoom();                                     // 0x00096F20
    return true;
}

bool Ripple::Tick() {                                 // orig: vf08 0x000716C0
    if (rate < 0.0f || rate == 0.0f) return false;    // FCOMP 0; TEST AH,0x41; JNP: < or == stop (NaN goes on)
    if (sizeX >= endX && sizeZ >= endZ) return false; // TEST AH,1 twice: C0 (< or NaN) goes on
    acc = F(double(g_frameStep) * rate + acc);        // FLD step; FMUL +0x48; FADD +0x4C
    time = F(double(g_frameStep) + time);
    return true;
}

bool Ripple::PostUpdate() {                           // orig: vf0C 0x000718E0
    if (rate < 0.0f || rate == 0.0f) return false;    // (the Tick's gates)
    if (sizeX >= endX && sizeZ >= endZ) return false;
    const double sx = double(rangeX) * acc + sizeX;   // FLD +0x50; FMUL +0x4C; FADD +0x58; FST
    sizeX = F(sx);
    sizeZ = F(double(rangeZ) * acc + sizeZ);          // FST [esp+4]; FSTP +0x5C
    if (sx >= double(endX)) sizeX = endX;             // the UNROUNDED x (TEST AH,1; JNE: < or NaN keep)
    if (sizeZ >= endZ) sizeZ = endZ;                  // the rounded z (dword)
    f32& ma = material->alpha;                        // +0x3C -> +0x50
    if (alpha > 0.0f) {                               // fading in (TEST AH,0x41; JNE -> out)
        const double v = double(acc) * alpha * 5.0f + ma;   // FMUL +0x44; FMUL [0x00293C24] (5.0f); FADD
        if (!(v >= double(alpha))) {                  // FCOM +0x44; TEST AH,1; JNE (< or NaN: store v)
            ma = F(v);
        } else {
            const f32 a = alpha;                      // FSTP ST0; FLD alpha; FLD alpha; FCHS; FSTP +0x44
            alpha = -alpha;
            ma = a;
        }
    } else {
        double v = double(ma) - double(-alpha) * acc * 1.375f;   // FCHS; FMUL +0x4C; FMUL [0x0029C830]; FSUBP
        if (v < 0.0) v = 0.0;                         // FCOM 0; TEST AH,5; JP
        ma = F(v);
    }
    ++material->revision;                             // +0x54
    acc = 0;
    auto& v = quad->modelData->vertices;              // +0x38 -> +0xC8 -> +0x1C
    v[0] = {-sizeX, 0.0f, -sizeZ};                    // FCHS of the floats; the 0 words are integer stores
    v[1] = {-sizeX, 0.0f, sizeZ};
    v[2] = {sizeX, 0.0f, sizeZ};
    v[3] = {sizeX, 0.0f, -sizeZ};
    quad->modelData->revisionFlags |= 1;
    UpdateNodeAt(node.Get(), 0.0f);                   // NiAVObject::Update(0.0, 1) (then vf98 up to the first room)
    return true;
}

}  // namespace fut::game

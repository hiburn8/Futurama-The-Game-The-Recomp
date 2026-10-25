// NiXBoxLightManager; see NiXBoxLightManager.h. x87 arithmetic in the original's order (double = the extended
// register, F() = a float store).
#include "render/nixbox/NiXBoxLightManager.h"

#include <cmath>
#include <cfloat>

namespace fut::render::nixbox {

using ni::detail::F;
using namespace xd3d;

namespace {
u32 Ftol(double v) { return static_cast<u32>(static_cast<s32>(v)); }   // orig: _ftol 0x00167110
}  // namespace

NiXBoxLightManager::NiXBoxLightManager(XD3DDevice& device, NiXBoxRenderState& state)   // orig: 0x0015BD40
    : d3d_(device), state_(state) {
    // (+8 the light-data map of 37 buckets: the data is owned by its light here; +0x2C = 2)
    lastAmbient_ = 0;
    state_.SetDeferred(RS_AMBIENT, 0, 0x1000);        // (the cache +0x5C4)
    state_.SetDeferred(RS_LIGHTING, 0, 0x1200);       // (+0x55C)
    lastApply_ = 0, lightingOn_ = false, lightsUsed_ = false, lastSource_ = 0;
    state_.SetDeferred(RS_EMISSIVEMATERIALSOURCE, 0, 0x1000);
    state_.SetDeferred(RS_AMBIENTMATERIALSOURCE, 0, 0x1000);
    state_.SetDeferred(RS_DIFFUSEMATERIALSOURCE, 0, 0x1000);
    lastEffects_ = nullptr, lastLightsUsed_ = false;
}

void NiXBoxLightManager::BeginScene() {               // orig: 0x0015BA20
    for (const auto& l : enabled_) d3d_.LightEnable(l->effectId, false);
    enabled_.clear();
    lastEffects_ = nullptr;
}

void NiXBoxLightManager::LightingOff() {
    if (!lightingOn_) return;
    state_.SetDeferred(RS_LIGHTING, 0, 0x1200);
    lightingOn_ = false;
}

void NiXBoxLightManager::SetState(const ni::NiDynamicEffectState* effects, const ni::NiTexturingProperty& tex,
                                  const ni::NiVertexColorProperty& vc) {   // orig: 0x0015C010
    lastApply_ = tex.applyMode;                       // +0x44 = tex +0x18
    const bool baseTextured = tex.HasMap(0) && tex.maps[0].textureObj;   // (+0x24)[0] and its texture +4
    if (baseTextured && tex.applyMode == 0) {         // APPLY_REPLACE: unlit
        LightingOff();
        return;
    }
    const u32 src = vc.sourceVertexMode;              // +0x18
    if (vc.lightingMode == 0) {                       // LIGHTING_E (+0x1C)
        if (src == 1 || src > 2) {                    // SOURCE_EMISSIVE (or invalid): unlit
            LightingOff();
            return;
        }
        lightsUsed_ = false;
        if (lastSource_ == 1) {
            state_.SetDeferred(RS_EMISSIVEMATERIALSOURCE, 0, 0x1000);
            lastSource_ = 0;
        }
    } else {                                          // LIGHTING_E_A_D
        if (src != lastSource_) {
            if (src == 0) {                           // SOURCE_IGNORE
                state_.SetDeferred(RS_EMISSIVEMATERIALSOURCE, 0, 0x1000);
                state_.SetDeferred(RS_AMBIENTMATERIALSOURCE, 0, 0x1000);
                state_.SetDeferred(RS_DIFFUSEMATERIALSOURCE, 0, 0x1000);
            } else if (src == 1) {                    // SOURCE_EMISSIVE
                state_.SetDeferred(RS_EMISSIVEMATERIALSOURCE, 1, 0x1000);
                state_.SetDeferred(RS_AMBIENTMATERIALSOURCE, 0, 0x1000);
                state_.SetDeferred(RS_DIFFUSEMATERIALSOURCE, 0, 0x1000);
            } else if (src == 2) {                    // SOURCE_AMB_DIFF
                state_.SetDeferred(RS_EMISSIVEMATERIALSOURCE, 0, 0x1000);
                state_.SetDeferred(RS_AMBIENTMATERIALSOURCE, 1, 0x1000);
                state_.SetDeferred(RS_DIFFUSEMATERIALSOURCE, 1, 0x1000);
            }
            lastSource_ = src;
        }
        lightsUsed_ = true;
    }
    if (!lightingOn_) {
        state_.SetDeferred(RS_LIGHTING, 1, 0x1200);
        lightingOn_ = true;
    }
    if (effects == lastEffects_ && lightsUsed_ == lastLightsUsed_) return;
    SetLights(effects);
    lastEffects_ = effects;
    lastLightsUsed_ = lightsUsed_;
}

void NiXBoxLightManager::SetLights(const ni::NiDynamicEffectState* effects) {   // orig: 0x0015BAD0
    for (const auto& l : enabled_) d3d_.LightEnable(l->effectId, false);   // @0x0015BB46 (head first)
    enabled_.clear();
    f32 amb[3] = {0.0f, 0.0f, 0.0f};
    if (lightsUsed_ && effects) {
        for (const auto& lp : effects->lights) {      // +0xC, list order
            ni::NiLight* l = lp.Get();
            // the switch +0xC0, dimmer +0xE0 >= 0.01f ([0x00293AAC]; NaN skips too)
            if (!l || !l->switchOn || !(l->dimmer >= 0.01f)) continue;
            if (l->EffectType() == 0) {               // an ambient light
                const double d = l->dimmer;
                amb[0] = F(d * l->ambient.r + amb[0]);
                amb[1] = F(d * l->ambient.g + amb[1]);
                amb[2] = F(d * l->ambient.b + amb[2]);
                continue;
            }
            auto* data = static_cast<LightData*>(l->rendererData.get());
            if (!data) {                              // new(0x70): +0x68 = 0, +0x6C = the light's id
                auto d = std::make_unique<LightData>();
                d->index = l->effectId;
                data = d.get();
                l->rendererData = std::move(d);
                FillLight(*data, *l);
                d3d_.SetLight(data->index, data->light);   // @0x0015BC06
            } else if (FillLight(*data, *l)) {
                d3d_.SetLight(data->index, data->light);
            }
            d3d_.LightEnable(data->index, true);      // @0x0015BC11
            enabled_.insert(enabled_.begin(), l);     // (0x0015B9A0: linked at the head)
        }
    }
    // FLD acc; FMUL 255.0f [0x002A1564]; _ftol; the low byte only (no clamp)
    const u32 argb = 0xFF000000u | ((Ftol(double(amb[0]) * 255.0f) & 0xFF) << 16) |
                     ((Ftol(double(amb[1]) * 255.0f) & 0xFF) << 8) | (Ftol(double(amb[2]) * 255.0f) & 0xFF);
    if (lastAmbient_ != argb) {
        state_.SetDeferred(RS_AMBIENT, argb, 0x1000);
        lastAmbient_ = argb;
    }
}

bool NiXBoxLightManager::FillLight(LightData& d, const ni::NiLight& l) {   // orig: 0x0015B730
    if (d.revision == l.revision) return false;
    d.revision = l.revision;
    d.light = {};
    switch (l.EffectType()) {
    case 1:                                           // directional
        d.light.type = 3;
        if (auto* dl = dynamic_cast<const ni::NiDirectionalLight*>(&l)) {
            d.light.direction[0] = dl->worldDirection.x;   // +0x110
            d.light.direction[1] = dl->worldDirection.y;
            d.light.direction[2] = dl->worldDirection.z;
        }
        break;
    case 2:                                           // point (spot lights, type 3: no class in the port)
        d.light.type = 1;
        d.light.position[0] = l.world.translate.x;    // +0xA0
        d.light.position[1] = l.world.translate.y;
        d.light.position[2] = l.world.translate.z;
        if (auto* pl = dynamic_cast<const ni::NiPointLight*>(&l)) {
            d.light.attenuation0 = pl->constantAtten;   // +0x110 / +0x114 / +0x118
            d.light.attenuation1 = pl->linearAtten;
            d.light.attenuation2 = pl->quadraticAtten;
        }
        d.light.range = F(std::sqrt(double(FLT_MAX)));   // FLD qword [0x002A6088]; FSQRT; FSTP (0x5F7FFFFF)
        break;
    default:
        break;
    }
    const double dim = l.dimmer;                      // each component one FMUL and one FSTP
    d.light.diffuse[0] = F(l.diffuse.r * dim), d.light.diffuse[1] = F(l.diffuse.g * dim);
    d.light.diffuse[2] = F(l.diffuse.b * dim), d.light.diffuse[3] = 1.0f;
    d.light.specular[0] = F(l.specular.r * dim), d.light.specular[1] = F(l.specular.g * dim);
    d.light.specular[2] = F(l.specular.b * dim), d.light.specular[3] = 1.0f;
    d.light.ambient[0] = F(l.ambient.r * dim), d.light.ambient[1] = F(l.ambient.g * dim);
    d.light.ambient[2] = F(l.ambient.b * dim), d.light.ambient[3] = 1.0f;
    return true;
}

}  // namespace fut::render::nixbox

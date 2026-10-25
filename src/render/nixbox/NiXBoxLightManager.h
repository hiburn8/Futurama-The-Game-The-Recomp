// NiXBoxLightManager (renderer +0x5C4, 0x54 bytes, vtable 0x002AB858, ctor 0x0015BD40): LIGHTING, the material
// sources, the D3D lights and AMBIENT from a geometry's effect state (docs/analysis/renderer_lighting_port.md 2).
#pragma once

#include "engine/ni/NiObjects.h"
#include "render/nixbox/NiXBoxRenderState.h"

#include <vector>

namespace fut::render::nixbox {

class NiXBoxLightManager {
public:
    NiXBoxLightManager(xd3d::XD3DDevice& device, NiXBoxRenderState& state);   // orig: 0x0015BD40
    // orig: 0x0015BA20 (BeginScene): every enabled light disabled (head first), the last effect state forgotten
    void BeginScene();
    // orig: 0x0015C010 (effects, texturing, vertex colour)
    void SetState(const ni::NiDynamicEffectState* effects, const ni::NiTexturingProperty& tex,
                  const ni::NiVertexColorProperty& vc);

    // The light's renderer data (orig: 0x70 bytes, light +0x108): the D3DLIGHT8, +0x68 the revision it was built
    // from, +0x6C the index (the light's +0xC4).
    struct LightData : ni::NiLightRendererData {
        xd3d::D3DLIGHT8 light{};
        u32 revision = 0;
        u32 index = 0;
    };

private:
    void SetLights(const ni::NiDynamicEffectState* effects);   // orig: 0x0015BAD0
    void LightingOff();                               // orig: 0x0015C1E5..
    static bool FillLight(LightData& d, const ni::NiLight& l);   // orig: 0x0015B730

    xd3d::XD3DDevice& d3d_;                           // +0x30
    NiXBoxRenderState& state_;                        // +0x38
    std::vector<ni::NiPointer<ni::NiLight>> enabled_;   // +0x18..+0x24 (front = the head)
    const ni::NiDynamicEffectState* lastEffects_ = nullptr;   // +0x3C (only compared)
    bool lastLightsUsed_ = false;                     // +0x40
    u32 lastApply_ = 0;                               // +0x44
    bool lightingOn_ = false;                         // +0x48
    bool lightsUsed_ = false;                         // +0x49
    u32 lastSource_ = 0;                              // +0x4C
    u32 lastAmbient_ = 0;                             // +0x50
};

}  // namespace fut::render::nixbox

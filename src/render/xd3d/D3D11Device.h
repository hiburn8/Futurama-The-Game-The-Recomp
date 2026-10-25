// The D3D11 backend of the recorded D3D8 stream (renderer_plan.md 2 / 4, renderer_lighting_port.md 9): an XD3DDevice
// that executes every Clear / draw / Swap the decompiled renderer (src/render/nixbox) issues, from the library state
// at that moment (the render-state and texture-stage arrays, the transforms, lights, material, viewport, render
// targets and bindings), with one HLSL program reproducing the D3D8 fixed-function pipeline with the Xbox enum values:
// the transform (row vectors, XYZRHW screen vertices), the per-vertex lighting (the enabled D3DLIGHT8s, AMBIENT, the
// material and its COLORVERTEX sources), the texture-coordinate generation (TEXCOORDINDEX camera-space normal /
// position / reflection) and transforms (TEXTURETRANSFORMFLAGS with TEXTURE0-3), fog (table modes), flat / Gouraud
// shading, the four texture stages (COLOROP / ALPHAOP / ARG0-2 / RESULTARG, TEXTUREFACTOR), the samplers (address and
// filter modes), alpha test, blending, z, stencil, cull, fill and colour-write states.
//
// Vertex and index data are read from the resources' Xbox layouts (the FVF of SetVertexShader; the primitive types
// with the Xbox numbering: fans and quad lists become lists) and uploaded per draw; textures are decoded from their
// engine objects (Resource::source: NiSourceTexture pixel data, TextureDecode.cpp) until the texture manager writes
// the Xbox texels (renderer_plan.md 14), and the render targets of rendered textures are D3D11 render targets.
// NOT PORTED (reported once): the combiner pixel shader programs (SetPixelShader), bump-env ops, VERTEXBLEND
// (unused: the renderer skins on the CPU), point sprites. NV2A precision (combiners, lighting) is not modelled:
// screenshots compare with a tolerance (renderer_plan.md 6, stage 6).
#pragma once

#include "render/xd3d/XD3DDevice.h"

#include <memory>
#include <string>
#include <vector>

struct HWND__;

namespace fut::render::xd3d {

class D3D11Device : public XD3DDevice {
public:
    D3D11Device();
    ~D3D11Device() override;

    // `window` may be null (offscreen). The Xbox back buffer (surf1, 640 x 480) is a D3D11 render target of that
    // size; Swap copies it to the window's swap chain (stretched).
    bool Init(HWND__* window, std::string& error);
    u32 draws() const { return draws_; }

    void Clear(u32 count, const D3DRECT* rects, u32 flags, u32 color, f32 z, u32 stencil) override;
    void Swap(u32 flags) override;
    void DrawVertices(u32 primitive, u32 start, u32 count) override;
    void DrawIndexedVertices(u32 primitive, u32 count, const u16* data) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    u32 draws_ = 0;
    void Draw(u32 primitive, const u16* list, u32 count, u32 start);
};

}  // namespace fut::render::xd3d

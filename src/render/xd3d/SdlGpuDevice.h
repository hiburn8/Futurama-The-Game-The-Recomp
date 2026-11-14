// SDL3 GPU backend (Metal on macOS) implementing XD3DDevice.
#pragma once

#include "render/xd3d/XD3DDevice.h"

#include <memory>
#include <string>

struct SDL_Window;

namespace fut::render::xd3d {

class SdlGpuDevice : public XD3DDevice {
public:
    SdlGpuDevice();
    ~SdlGpuDevice() override;

    bool Init(SDL_Window* window, u32 width, u32 height, std::string& error);
    void Resize(u32 width, u32 height);
    enum class Filter { Linear, Nearest };
    void SetFilter(Filter filter);
    void SetRenderScale(double factor);
    double RenderScale() const;
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

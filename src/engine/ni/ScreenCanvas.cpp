#include "engine/ni/ScreenCanvas.h"

#include "engine/ni/NiObjects.h"

namespace fut::ni {

u32 ScreenCanvas::width = ScreenCanvas::kWidth43;

f32 ScreenCanvas::X(f32 x, ScreenAnchor anchor) {
    if (!Wide() || anchor == ScreenAnchor::None) return x;
    const double k = Expansion();
    switch (anchor) {
    case ScreenAnchor::Left: return f32(double(x) / k);
    case ScreenAnchor::Right: return f32(1.0 - (1.0 - double(x)) / k);
    case ScreenAnchor::Centre: return f32(0.5 + (double(x) - 0.5) / k);
    default: return x;
    }
}

s32 ScreenCanvas::BandLeft(ScreenAnchor anchor) {
    if (!Wide() || anchor == ScreenAnchor::None) return 0;
    const s32 extra = s32(width) - s32(kWidth43);
    switch (anchor) {
    case ScreenAnchor::Left: return 0;
    case ScreenAnchor::Right: return extra;
    case ScreenAnchor::Centre: return extra / 2;
    default: return 0;
    }
}

void ScreenCanvas::Widen(NiCamera& camera) {
    if (!Wide()) return;
    const double k = Expansion();
    f32 f[6];
    for (int i = 0; i < 6; ++i) f[i] = camera.frustum[i];
    f[0] = f32(double(f[0]) * k);
    f[1] = f32(double(f[1]) * k);
    camera.SetFrustum(f);
}

}  // namespace fut::ni

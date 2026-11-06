// The canvas: the size of the picture the game draws into, and the widescreen option's conversions (port-only; the
// original always draws 640 x 480: docs/analysis/widescreen_plan.md). The width is set once at start (GameMain's
// --aspect) and never changes while running; the height is always 480, so every pixel-based number of the original
// keeps its meaning. At 640 x 480 every function here returns its input untouched (an early return, not arithmetic),
// so the 4:3 path is the original's, byte for byte.
//
// A wider canvas changes three things, each in one place:
// * a 3D view: Widen scales a camera's frustum left / right by the expansion (Hor+: the same vertical view, more world
//   at the sides; culling follows the frustum);
// * a 2D object (NiScreenPolygon, NiScreenTexture): its `anchor` says where the original put it on the 640-wide
//   picture (left / right edge, centre, or None = already a fraction of the real canvas); the renderer's packers map
//   the x coordinates through X / BandLeft once when the object is drawn, so the game's placements are untouched;
// * a text object: its camera's frustum carries the anchor (TextObject::SetAnchor).
#pragma once

#include "core/Types.h"

namespace fut::ni {

class NiCamera;

// Where a 2D object sits on the original's 640-wide picture (the HUD's left and right groups, the centred pictures).
enum class ScreenAnchor : u8 {
    None,                                             // a fraction (or pixel) of the real canvas: fades, full layers
    Left,                                             // keeps its distance from the left edge
    Right,                                            // keeps its distance from the right edge
    Centre,                                           // stays centred (the 4:3 pictures: movies, loading screens)
};

struct ScreenCanvas {
    static constexpr u32 kWidth43 = 640, kHeight = 480;
    static u32 width;                                 // 640, or round(480 x aspect) rounded to even (GameMain)
    static bool Wide() { return width != kWidth43; }
    static double Expansion() { return double(width) / kWidth43; }   // k: 1 at 4:3, 1.3344 at 16:9 (854)
    // A fraction of the 640-wide picture as a fraction of the canvas, by the anchor (None: unchanged).
    static f32 X(f32 x, ScreenAnchor anchor);
    static f32 W(f32 w) { return Wide() ? f32(double(w) / Expansion()) : w; }   // a width (its shape kept)
    // The pixel offset of the 640-wide band the anchor keeps (0 at 4:3 and for None).
    static s32 BandLeft(ScreenAnchor anchor);
    // Hor+: the camera's frustum left / right scaled by the expansion, re-set through SetFrustum (its planes and
    // projection follow at the camera's next update, as after any SetFrustum). Nothing at 4:3.
    static void Widen(NiCamera& camera);
};

}  // namespace fut::ni

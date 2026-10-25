// The cull traversal as far as game state sees it: NiCamera::Click 0x00100CE0 without the renderer. It keeps every
// visited object's "frames not drawn" counter (NiAVObject +0xB2), which the game reads (the pickup flare, the
// statics' ripple gate, the mesh emitters' node update), and the render-time side effects of the traversal (the
// switch nodes' child updates, the LOD index, the room hint). docs/analysis/cull_counter_port.md.
#pragma once

#include "engine/ni/NiObjects.h"

#include <bitset>
#include <vector>

namespace fut::ni {

// orig: the Click state on the NiCamera (+0x1AC plane stack, +0x1C4 count, +0x228 mask, +0x240 active); every push is
// popped and every save restored before Click returns.
struct NiCullPass {
    const NiCamera& camera;
    std::vector<const NiPlane*> planes;               // [0..5] = the camera's frustum planes, then portal planes
    std::bitset<128> passed;                          // bit i: plane i fully passed by an ancestor (skipped)
    u32 active = 6;                                   // the planes not passed yet

    NiRenderer* renderer = nullptr;                   // cam+0x1A8 (null: no display)

    explicit NiCullPass(const NiCamera& cam);
    void Cull(NiAVObject* o);                         // orig: 0x00101AC0
    bool OutsideFrustum(const NiAVObject& o);         // orig: 0x00101A40
};

// orig: 0x0014D170 NiBound::WhichSide: 2 outside (behind the plane), 1 completely in front, 0 intersecting / NaN.
int PlaneSide(const NiBound& b, const NiPlane& p);

// orig: NiCamera::Click 0x00100CE0: the scene is culled when it is a visual object. With the camera's renderer
// (NiCamera::renderer) the visible geometry is displayed too: SetCameraData, BeginScene, the accumulator, the cull
// with the geometry classes' display, FinishAccumulating, EndScene; without one, the cull pass alone (the counters).
void VisitForCulling(NiCamera& camera, NiAVObject* scene);

// orig: 0x0013E5E0 (cdecl; the HUD head's model): a node with a NiTriBasedGeom child becomes an outline (with
// controllers: its children move into a new outline child; else a clone of it as a NiOutline takes its place in
// its parent), then the (new) node's first children, recursively.
void ConvertToOutlines(NiAVObject* object);
// orig: DAT_002D7B58, the outline mode (static 2; RenderShadow clears it around the shadow Click, 0x0003BBD3).
extern u32 g_outlineMode;

// The outline renderer (NiOutline.cpp). DAT_0030EBE0: while set, the tri-shape display (0x00116760) writes the
// geometry's world vertices there and clears it (NiOutline's display sets it per listed geometry); DAT_002FA500: the
// geometry being displayed.
extern NiPoint3* g_outlineScratch;
extern NiGeometry* g_outlineGeometry;
// orig: 0x001167E5..0x00116BB5 (the display's tail, an unskinned geometry): the model vertices by the world
// transform (0x000AD900's scaled rotation), in the unrolled loop's sum order.
void WriteOutlineWorldVertices(const NiGeometry& g, NiPoint3* out);
// orig: 0x0013CEF0 (cdecl; the level's "outline.tga" after the effects, 0x000C5D64): the shared outline polygon
// properties (the texture with apply mode 2 / filter 1, blending, no z write, vertex colours, flat shading).
void SetOutlineTexture(NiObject* texture);
// orig: 0x0013C7E0 (Click's start): the outline polygon pool from its first polygon.
void BeginOutlineClick();
// orig: 0x0013E3F0 (Click, after the cull, outline mode 2): the current outline polygon padded and attached.
void FinishOutlineClick(NiCamera& camera);
// orig: 0x0013D020 (the App's shutdown, 0x0002A6A3): the pool, the scratch arrays and the texture released (the
// render test calls it before each scenario: the oracle's process starts with none).
void ShutdownOutlines();

}  // namespace fut::ni

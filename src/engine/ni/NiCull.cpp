// The cull traversal's counter pass; see NiCull.h and docs/analysis/cull_counter_port.md (sections 3-6).
#include "engine/ni/NiCull.h"

#include "engine/ni/NiClone.h"

#include "engine/ni/NiMath.h"
#include "engine/ni/NiRenderer.h"
#include "engine/ni/NiStream.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace fut::ni {

using detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// orig: 0x00141E20 NiPlane(normal, point): constant = (n.z*p.z + n.y*p.y) + n.x*p.x.
NiPlane MakePlane(const NiPoint3& n, const NiPoint3& p) {
    return {n, F((double(n.z) * p.z + double(n.y) * p.y) + double(n.x) * p.x)};
}

}  // namespace

// ---- the camera (3.2) ------------------------------------------------------------------------------------------
void NiCamera::UpdateWorldData() {                    // orig: 0x000FFE30
    NiAVObject::UpdateWorldData();                    // 0x00143770
    const auto& M = world.rotate.m;
    worldDir = {M[0][0], M[1][0], M[2][0]};           // column 0
    worldUp = {M[0][1], M[1][1], M[2][1]};            // column 1
    worldRight = {M[0][2], M[1][2], M[2][2]};         // column 2
    const NiPoint3& D = worldDir;
    const NiPoint3& U = worldUp;
    const NiPoint3& R = worldRight;
    const NiPoint3& E = world.translate;
    auto at = [&](f32 s) {
        return NiPoint3{F(double(s) * D.x + E.x), F(double(s) * D.y + E.y), F(double(F(double(s) * D.z)) + E.z)};
    };
    frustumPlanes[0] = MakePlane(D, at(frustum[4]));  // near 0x000FEF60
    frustumPlanes[1] = MakePlane({F(-double(D.x)), F(-double(D.y)), F(-double(D.z))}, at(frustum[5]));   // far 0x000FEFE0
    auto side = [&](f32 s, const NiPoint3& A, const NiPoint3& B) {   // 0x000FF090.. (a x B, unitised)
        const double ax = F(double(s) * A.x + D.x);
        const double ay = double(s) * A.y + D.y;      // (kept unrounded)
        const double az = double(F(double(s) * A.z)) + D.z;
        NiPoint3 n{F(ay * B.z - az * B.y), F(az * B.x - ax * B.z), F(ax * B.y - ay * B.x)};
        (void)Unitize(n);                             // 0x000116A0
        return MakePlane(n, E);
    };
    const NiPoint3 nU{F(-double(U.x)), F(-double(U.y)), F(-double(U.z))};   // FCHS; FSTP
    const NiPoint3 nR{F(-double(R.x)), F(-double(R.y)), F(-double(R.z))};
    frustumPlanes[2] = side(frustum[0], R, U);        // left
    frustumPlanes[3] = side(frustum[1], R, nU);       // right
    frustumPlanes[4] = side(frustum[2], U, R);        // top
    frustumPlanes[5] = side(frustum[3], U, nR);       // bottom
    UpdateProjection();                               // (tail jump 0x000FF430)
}

bool NiCamera::WorldPtToScreenPt(const NiPoint3& p, f32& x, f32& y) const {   // orig: 0x000FEB10
    const f32* M = projection;                        // +0xC4
    const double w = ((double(p.z) * M[14] + double(p.y) * M[13]) + double(p.x) * M[12]) + M[15];   // +0xF4..+0x100
    if (!(w > double(1e-5f))) return false;           // FCOM [0x002A632C]; TEST AH,0x41; JNE
    const double inv = 1.0 / w;                       // FDIVR [0x00293A98] (kept)
    x = F(((double(p.z) * M[2] + double(p.y) * M[1]) + double(p.x) * M[0]) + M[3]);
    y = F(((double(p.z) * M[6] + double(p.y) * M[5]) + double(p.x) * M[4]) + M[7]);
    x = F(inv * x);
    y = F(inv * y);
    // FCOMP -1.0; TEST AH,1; JNE / FCOMP 1.0; TEST AH,0x41; JP: [-1, 1] (NaN: false)
    if (!(x >= -1.0f) || !(x <= 1.0f) || !(y >= -1.0f) || !(y <= 1.0f)) return false;
    x = F(((double(port[1]) - port[0]) * x) * 0.5f);  // +0x150 - +0x14C
    y = F(((double(port[2]) - port[3]) * y) * 0.5f);  // +0x154 - +0x158
    x = F((double(port[0]) + port[1]) * 0.5f + x);
    y = F((double(port[3]) + port[2]) * 0.5f + y);
    return true;
}

void NiCamera::UpdateProjection() {                   // orig: 0x000FF430 (x87 order; T = the world translate)
    const NiPoint3& T = world.translate;
    const NiPoint3& D = worldDir;                     // +0x110
    const NiPoint3& U = worldUp;                      // +0x11C
    const NiPoint3& R = worldRight;                   // +0x128
    auto offset = [&](const NiPoint3& A) {            // (-A.z T.z + -A.y T.y) + -A.x T.x (FCHS of each word)
        return (double(-A.z) * T.z + double(-A.y) * T.y) + double(-A.x) * T.x;
    };
    const double sR = offset(R);                      // (kept)
    const f32 tU = F(offset(U));
    const double sD = offset(D);                      // (kept)
    const f32 a = F(1.0 / (double(frustum[1]) - frustum[0]));   // 1 / (r - l)
    const double b = 1.0 / (double(frustum[2]) - frustum[3]);   // 1 / (t - b) (kept)
    const double c = 1.0 / (double(frustum[5]) - frustum[4]);   // 1 / (far - near) (kept)
    const double a2 = double(a) + a;
    projectionScale[0] = F(a2);                       // +0x108
    const double b2 = b + b;
    projectionScale[1] = F(b2);                       // +0x10C
    const f32 A2 = F(a2);
    const f32 e = F(-((double(frustum[0]) + frustum[1]) * a));
    const double g = -((double(frustum[2]) + frustum[3]) * b);
    const f32 h = F(c * frustum[5]);
    f32* M = projection;                              // +0xC4
    M[0] = F(double(e) * D.x + double(A2) * R.x);
    M[1] = F(double(A2) * R.y + double(e) * D.y);
    M[2] = F(double(A2) * R.z + double(e) * D.z);
    M[3] = F(double(e) * sD + double(A2) * sR);
    M[4] = F(g * D.x + b2 * U.x);
    M[5] = F(b2 * U.y + g * D.y);
    M[6] = F(b2 * U.z + g * D.z);
    M[7] = F(g * sD + b2 * tU);
    M[8] = F(double(h) * D.x);
    M[9] = F(double(h) * D.y);
    M[10] = F(double(h) * D.z);
    M[11] = F(-(c * frustum[4] * frustum[5]) + double(h) * sD);
    M[12] = D.x, M[13] = D.y, M[14] = D.z;            // +0xF4..+0xFC
    M[15] = F(sD);                                    // +0x100
}

void NiCamera::SetFrustum(const f32 f[6]) {           // orig: 0x00011960
    f32 nearDist = f[4];
    const double lim = double(f[5]) / 10000.0;
    if (nearDist < lim) nearDist = F(lim);
    if (nearDist < 0.1f) nearDist = 0.1f;
    frustum[0] = f[0], frustum[1] = f[1], frustum[2] = f[2], frustum[3] = f[3];
    frustum[4] = nearDist;
    frustum[5] = f[5];
    // (0x000FF6D0: the screen polygons' dirty bits, renderer state; the planes follow at the next UpdateWorldData)
}

// ---- the pass (4) ----------------------------------------------------------------------------------------------
NiCullPass::NiCullPass(const NiCamera& cam) : camera(cam) {
    for (const NiPlane& p : cam.frustumPlanes) planes.push_back(&p);
}

int PlaneSide(const NiBound& b, const NiPlane& p) {   // orig: 0x0014D170
    const double d = ((double(p.normal.z) * b.center.z + double(p.normal.y) * b.center.y) +
                      double(p.normal.x) * b.center.x) - p.constant;   // unrounded
    if (d <= -double(b.radius)) return 2;             // FCOMPP; TEST AH,0x41; JP: d > -r or NaN go on
    if (d >= double(b.radius)) return 1;              // FCOMP; TEST AH,1
    return 0;
}

bool NiCullPass::OutsideFrustum(const NiAVObject& o) {   // orig: 0x00101A40
    for (s32 i = static_cast<s32>(planes.size()) - 1; i >= 0; --i) {   // from the top of the stack down
        if (passed[static_cast<size_t>(i)]) continue;
        switch (PlaneSide(o.worldBound, *planes[static_cast<size_t>(i)])) {
        case 2: return true;
        case 1:
            passed[static_cast<size_t>(i)] = true;
            --active;
            break;
        default: break;
        }
    }
    return false;
}

void NiCullPass::Cull(NiAVObject* o) {                // orig: 0x00101AC0
    if (o->flags & kNiAVHidden) {                     // AppCulled
        o->OnNotDrawn();
        return;
    }
    if (active == 0) {                                // every plane passed: displayed, the counter is NOT written
        o->OnVisible(*this);
        return;
    }
    const std::bitset<128> savedMask = passed;
    const u32 savedActive = active;
    if (OutsideFrustum(*o)) {
        o->OnNotDrawn();
    } else {
        o->framesNotDrawn = 0;                        // 0x00101B33
        o->OnVisible(*this);
    }
    passed = savedMask;
    active = savedActive;
}

void VisitForCulling(NiCamera& camera, NiAVObject* scene) {   // orig: NiCamera::Click 0x00100CE0
    NiRenderer* r = camera.renderer;
    if (!r) {                                         // (the fake renderer: no display, the counters only)
        if (!scene || !scene->IsVisualObject()) return;   // scene->vf70 (NiNode: flags & 0x0100)
        NiCullPass pass(camera);
        pass.Cull(scene);
        return;
    }
    BeginOutlineClick();                              // 0x0013C7E0 (the outline polygons' pool from its start)
    // (DAT_002E8DDC = 0xFE770FA6 for the Click, restored after: L, render statistics; not ported)
    r->SetCameraData(camera.world.translate, camera.worldDir, camera.worldUp, camera.worldRight, camera.frustum,
                     camera.port);                    // vfBC (+0xA0, +0x110, +0x11C, +0x128, +0x134, +0x14C)
    r->BeginScene();                                  // vfB4
    if (scene && scene->IsVisualObject()) {           // cam+0x15C, vf70
        const bool accumulating = r->StartAccumulating(camera);   // renderer+8 vf2C (false: renderer+8 released)
        NiCullPass pass(camera);
        pass.renderer = r;
        pass.Cull(scene);                             // 0x00101AC0
        FinishOutlineClick(camera);                   // 0x0013E3F0 (the last outline polygon attached)
        if (accumulating) r->FinishAccumulating(camera);   // vf30 (then 0x00153490: renderer+8 = the saved sorter)
        // 0x00100E00: the polygons with +0x1E and +0x1F (0x00106AA0 / 0x00106AC0) drawn (0x00106B80: renderer +0xC =
        // the polygon's property state +8, +0x10 = 0, then vfD8) and their slot cleared (SetAt(i, 0) 0x0012B7F0);
        // the size +0x16C re-read every iteration
        for (size_t i = 0; i < camera.screenPolygonList.size(); ++i) {
            NiScreenPolygon* p = camera.screenPolygonList[i].Get();
            if (!p || !p->strip || !p->drawOnce) continue;
            NiPointer<NiScreenPolygon> keep(p);
            r->DrawScreenPolygon(*p);
            camera.screenPolygonList[i] = nullptr;
        }
        // 0x00100E6A: the objects of +0x17C (vf84 with the camera, then removed): none is ever added (L)
    }
    // 0x00100F0D: the screen textures (+0x194, count +0x19C, the size re-read every iteration): 0x0010B000 ->
    // renderer vfDC
    for (size_t i = 0; i < camera.screenTextureList.size(); ++i)
        if (NiScreenTexture* t = camera.screenTextureList[i].Get()) r->DrawScreenTexture(*t);
    // 0x00100F44: every remaining polygon (0x00106B80 -> vfD8), then the camera's +0xC0 (none is ever set, L)
    for (size_t i = 0; i < camera.screenPolygonList.size(); ++i)
        if (NiScreenPolygon* p = camera.screenPolygonList[i].Get()) r->DrawScreenPolygon(*p);
    r->EndScene();                                    // vfB8
}

void NiTriShape::OnVisible(NiCullPass& pass) {        // orig: 0x00116760 (the display)
    if (!pass.renderer) return;                       // (the fake renderer's slots)
    pass.renderer->DisplayTriShape(*this, pass.camera);
}

void NiTriStrips::OnVisible(NiCullPass& pass) {       // orig: 0x0011C4F0 (as 0x00116760: renderer vfCC = vfC0)
    if (!pass.renderer) return;                       // (the fake renderer's slots)
    pass.renderer->DisplayTriShape(*this, pass.camera);
}

// ---- 5. not drawn ----------------------------------------------------------------------------------------------
void NiParticles::OnVisible(NiCullPass& pass) {       // orig: 0x0010F8B0 (the display)
    if (!pass.renderer) return;                       // (the fake renderer's slots)
    pass.renderer->DisplayParticles(*this, pass.camera);
}
void NiAVObject::OnNotDrawn() {                       // orig: 0x00101A00
    if (framesNotDrawn < 0xFF) ++framesNotDrawn;
}

void NiNode::OnNotDrawn() {                           // orig: 0x00105B10
    if (framesNotDrawn == 0) {
        const size_t n = children_.size();            // +0xCC sampled once
        for (size_t i = 0; i < n; ++i)
            if (i < children_.size() && children_[i]) children_[i]->OnNotDrawn();
    }
    NiAVObject::OnNotDrawn();
}

// ---- 6. display ------------------------------------------------------------------------------------------------
void NiNode::OnVisible(NiCullPass& pass) {            // orig: 0x001054E0
    if (!(flags & kNiAVVisualBound)) return;          // byte +0xB1 bit 0
    for (size_t i = 0; i < children_.size(); ++i)     // (the size is re-read every iteration)
        if (children_[i]) pass.Cull(children_[i].Get());
}

void NiWall::OnVisible(NiCullPass& pass) { NiNode::OnVisible(pass); }   // orig: 0x001054E0

void NiBSPNode::OnVisible(NiCullPass& pass) {         // orig: 0x00112390
    NotPorted("NiBSPNode::OnVisible: the camera-side child order 0x00112390");
    NiNode::OnVisible(pass);
}

void NiBillboardNode::OnVisible(NiCullPass& pass) {   // orig: 0x001109E0
    FaceCamera(pass.camera);                          // 0x001103F0
    NiNode::OnVisible(pass);                          // 0x001054E0: the children's cull uses the bounds just updated
}

// orig: 0x001103F0. The world transform is rebuilt from the parent's and the local one, its rotation turned by m,
// and every child is updated again with the last update's arguments.
void NiBillboardNode::FaceCamera(const NiCamera& cam) {
    world = Compose(parent ? parent->world : NiTransform{}, local);   // 0x0010E050 (no parent: identity 0x0014D8E0)
    NiMatrix3 m;                                      // identity (0x002D7820)
    auto col = [&m](int c, const NiPoint3& v) { m.m[0][c] = v.x, m.m[1][c] = v.y, m.m[2][c] = v.z; };
    const NiMatrix3& W = world.rotate;
    const NiPoint3 negD{-cam.worldDir.x, -cam.worldDir.y, -cam.worldDir.z};   // FCHS; FSTP (exact)
    switch ((flags >> 10) & 7) {                     // jump table 0x001109C0
    case 0: {                                         // always face the camera (0x001104E1)
        const NiPoint3 A = TransposeMulVector(W, negD);
        const NiPoint3 U = TransposeMulVector(W, cam.worldUp);
        const NiPoint3 R = TransposeMulVector(W, cam.worldRight);
        const double s = std::sqrt(double(U.y) * U.y + double(R.y) * R.y);
        col(2, A);
        if (s > double(1e-6f)) {                      // FCOM [0x00293ACC]; TEST AH,0x41; JNE (NaN: the else branch)
            const double inv = 1.0 / s;
            const double b = double(U.y) * inv;
            const double c = -(inv * R.y);
            col(0, {F(U.x * c + R.x * b), F(U.y * c + b * R.y), F(U.z * c + R.z * b)});
            col(1, {F(U.x * b - R.x * c), F(b * U.y - c * R.y), F(U.z * b - R.z * c)});
        } else {                                      // 0x001106F6
            col(0, {-R.x, -R.y, -R.z});
            col(1, {-U.x, -U.y, -U.z});
        }
        break;
    }
    case 1: {                                         // rotate about the local up axis (0x00110755)
        const NiPoint3& E = cam.world.translate;
        const NiPoint3& T = world.translate;
        const NiPoint3 v{F(double(E.x) - T.x), F(double(E.y) - T.y), F(double(E.z) - T.z)};
        const NiPoint3 o = TransposeMulVector(W, v);
        const double inv = 1.0 / double(world.scale);
        const f32 x = F(inv * o.x);
        const double zu = inv * o.z;                  // FST: the stack keeps the unrounded product
        const f32 z = F(zu);
        const double s = std::sqrt(zu * z + double(x) * x);
        if (s < double(1e-12f)) break;                // FCOM [0x002A7460]; TEST AH,5; JNP (NaN goes on)
        const f32 k = F(1.0 / s);
        m.m[0][0] = F(double(z) * k), m.m[1][0] = 0, m.m[2][0] = F(-(double(x) * k));
        m.m[0][1] = 0, m.m[1][1] = 1, m.m[2][1] = 0;
        m.m[0][2] = F(double(x) * k), m.m[1][2] = 0, m.m[2][2] = F(double(z) * k);
        break;
    }
    case 2:                                           // rigid face camera (0x0011088D)
        col(0, TransposeMulVector(W, cam.worldRight));
        col(1, TransposeMulVector(W, cam.worldUp));
        col(2, TransposeMulVector(W, negD));
        break;
    case 3:
    case 4:                                           // face the centre: 0x001100E0, then the code of mode 0 / 2
        NotPorted("NiBillboardNode::FaceCamera: modes 3 / 4 (0x001100E0)");
        break;
    default: break;                                   // m stays identity
    }
    world.rotate = Multiply(W, m);                    // 0x00013B30 (W * m)
    for (size_t i = 0; i < children_.size(); ++i)     // size +0xCC re-read every iteration
        if (children_[i]) children_[i]->UpdateDownwardPass(lastUpdateTime, lastUpdateControllers);   // vf64
}

u32 g_outlineMode = 2;                                // orig: DAT_002D7B58 (static 2)

namespace {
bool IsTriBased(const NiAVObject* o) {                // the RTTI chain reaches NiTriBasedGeom (0x0030EC38)
    return dynamic_cast<const NiTriShape*>(o) || dynamic_cast<const NiTriStrips*>(o);
}
}  // namespace

void NiOutline::CollectGeometry() {                   // orig: 0x0013D8D0
    if (!geometry.empty()) return;                    // +0xF8
    const u32 n = childCount_;                        // +0xD0 (the non-null count) as the slot bound
    for (u32 i = 0; i < n; ++i) {
        NiAVObject* c = i < children_.size() ? children_[i].Get() : nullptr;   // (i < +0xCC)
        if (!c || !IsTriBased(c)) continue;
        NiAlphaProperty* alpha = nullptr;             // GetProperty(0): the geometry's own
        for (const auto& p : c->properties)
            if (auto* a = dynamic_cast<NiAlphaProperty*>(p.Get())) {
                alpha = a;
                break;
            }
        for (NiAVObject* o = c; o; o = o->parent)     // the geometry and its ancestors (+0x14)
            for (NiExtraData* e = o->extraData.Get(); e; e = e->nextData.Get())
                if (auto* str = dynamic_cast<NiStringExtraData*>(e);
                    str && std::strncmp(str->string.c_str(), "NiData_OutlineNoSort", 0x14) == 0 && alpha)
                    alpha->flags |= 0x2000;           // byte +0x15 |= 0x20 (no null test in the original)
        if (!alpha || !(alpha->flags & 1) || (alpha->flags & 0x2000))
            geometry.emplace_back(static_cast<NiGeometry*>(c));   // 0x0012B7F0 at +0xF8
    }
}

void ConvertToOutlines(NiAVObject* object) {          // orig: 0x0013E5E0
    auto* node = dynamic_cast<NiNode*>(object);
    if (!node) return;
    const size_t n = node->Children().size();         // +0xCC sampled once
    NiNode* walk = node;
    NiPointer<NiOutline> outline;
    for (size_t i = 0; i < n; ++i) {
        NiAVObject* c = i < node->Children().size() ? node->Children()[i].Get() : nullptr;
        if (!c || !IsTriBased(c)) continue;
        outline = new NiOutline;                      // new(0x110) 0x0013D100
        NiPointer<NiNode> keep(node);
        if (node->controller) {                       // GetControllers: the children move into the outline
            std::vector<NiPointer<NiAVObject>> kids;  // (a 100-entry array of the first childCount slots)
            const u32 count = node->ChildCount();     // +0xD0
            for (u32 j = 0; j < count; ++j)
                kids.emplace_back(j < node->Children().size() ? node->Children()[j].Get() : nullptr);
            for (u32 j = 0; j < count; ++j) {
                node->DetachChild(kids[j].Get());     // vfA4
                outline->AttachChild(kids[j].Get(), true);   // vfA0(child, 1)
            }
            node->AttachChild(outline.Get(), true);   // the outline as the node's child
        } else {                                      // a clone of the node as an outline in the node's place
            CopyNodeMembers(*node, *outline);
            if (NiNode* parent = node->parent) {      // +0x14
                parent->DetachChild(node);            // vfA4
                parent->AttachChild(outline.Get(), true);   // vfA0(outline, 1)
            }
        }
        outline->BuildData();                         // 0x0013DAA0: the list and the outline data +0xE8
        walk = outline.Get();
        break;
    }
    for (size_t i = 0; i < n; ++i)                    // the (new) node's first n slots
        ConvertToOutlines(i < walk->Children().size() ? walk->Children()[i].Get() : nullptr);
}

void NiRoomGroup::OnVisible(NiCullPass& pass) {       // orig: 0x0011F6D0: only the camera's room
    const NiPoint3& p = pass.camera.world.translate;  // cam+0xA0
    NiRoom* room = lastRoom && lastRoom->ContainsPoint(p) ? lastRoom.Get() : FindRoomContaining(p);   // 0x0011F260
    if (room) {
        lastRoom = room;
        pass.Cull(room);
    } else if (lastRoom) {
        pass.Cull(lastRoom.Get());
    }
}

void NiRoom::OnVisible(NiCullPass& pass) {            // orig: 0x0011DB40
    std::vector<bool> saved;                          // (+0x110: the in-portals' active bits)
    for (const auto& e : list1)
        if (e) {
            saved.push_back((e->flags & kNiPortalActive) != 0);
            e->flags = static_cast<u16>(e->flags & ~kNiPortalActive);
        }
    NiNode::OnVisible(pass);
    size_t k = 0;
    for (const auto& e : list1)
        if (e) {
            if (saved[k++]) e->flags |= kNiPortalActive;
            else e->flags = static_cast<u16>(e->flags & ~kNiPortalActive);
        }
}

void NiPortal::LinkObject(NiStream& s) {
    NiAVObject::LinkObject(s);
    adjoinerObject = dynamic_cast<NiAVObject*>(s.ResolveLink(adjoiner));
}

void NiPortal::OnVisible(NiCullPass& pass) {          // orig: 0x001433C0
    if ((flags & kNiPortalInUse) || !(flags & kNiPortalActive)) return;
    flags |= kNiPortalInUse;
    // orig: PushPlanes 0x001430D0: one plane per vertex edge, through the camera
    const NiPoint3& E = pass.camera.world.translate;
    const size_t n = worldVertices.size();            // u16 +0xC0
    if (worldPlanes.size() < std::max<size_t>(n, planeCount)) worldPlanes.resize(std::max<size_t>(n, planeCount));
    for (size_t i = 0; i < n; ++i) {
        const NiPoint3& v0 = worldVertices[i];
        const NiPoint3& v1 = worldVertices[i + 1 < n ? i + 1 : 0];
        const NiPoint3 a{F(double(v0.x) - E.x), F(double(v0.y) - E.y), F(double(v0.z) - E.z)};   // camera -> vertex
        const NiPoint3 b{F(double(v1.x) - v0.x), F(double(v1.y) - v0.y), F(double(v1.z) - v0.z)};   // edge
        NiPlane& pl = worldPlanes[i];
        pl.normal = UnitCross(b, a);                  // 0x0006CE00
        pl.constant = F((double(pl.normal.y) * E.y + double(pl.normal.z) * E.z) + double(pl.normal.x) * E.x);
        pass.planes.push_back(&pl);
        ++pass.active;
    }
    bool re[6] = {};                                  // the six frustum planes are tested again behind the portal
    for (int bit : {1, 0, 2, 3, 4, 5})
        if (pass.passed[static_cast<size_t>(bit)]) {
            pass.passed[static_cast<size_t>(bit)] = false;
            ++pass.active;
            re[bit] = true;
        }
    if (adjoinerObject) adjoinerObject->OnVisible(pass);   // the next room's display: no Cull of the room node
    for (int bit : {1, 0, 2, 3, 4, 5})
        if (re[bit]) {
            pass.passed[static_cast<size_t>(bit)] = true;
            --pass.active;
        }
    for (u16 k = 0; k < planeCount; ++k) {            // PopPlanes 0x00142C30 (the stored plane count)
        --pass.active;
        pass.planes.pop_back();
    }
    flags = static_cast<u16>(flags & ~kNiPortalInUse);
}

void NiSwitchNode::OnVisible(NiCullPass& pass) {      // orig: 0x00113570
    const s32 i = index;                              // +0xE0
    if (i < 0 || static_cast<size_t>(i) >= children_.size()) return;   // (no bounds check in the original)
    NiAVObject* ch = children_[static_cast<size_t>(i)].Get();
    if (!ch) return;
    if (childRevision.size() < children_.size()) childRevision.resize(children_.size(), 0);
    if (childRevision[static_cast<size_t>(i)] != revision) {
        childRevision[static_cast<size_t>(i)] = revision;   // 0x000A4A20
        ch->UpdateDownwardPass(lastUpdateTime, lastUpdateControllers);   // vf64: a render-time update
    }
    pass.Cull(ch);                                    // (the switch node does not test 0x0100)
}

void NiLODNode::OnVisible(NiCullPass& pass) {         // orig: 0x001142E0
    flags = static_cast<u16>(flags & ~kNiAVSwitchUpdateOnlyActive);   // byte +0xB1 &= 0xFB
    const NiPoint3& E = pass.camera.world.translate;
    const double vx = double(worldLodCenter.x) - E.x, vy = double(worldLodCenter.y) - E.y,
                 vz = double(worldLodCenter.z) - E.z;
    const f32 d = F(((vz * vz + vy * vy) + vx * vx) * (double(pass.camera.lodAdjust) * pass.camera.lodAdjust));
    s32 i = static_cast<s32>(std::min(children_.size(), ranges.size())) - 1;   // +0xCC
    for (; i >= 0; --i)
        if (ranges[static_cast<size_t>(i)].worldNearSq <= d && d < ranges[static_cast<size_t>(i)].worldFarSq) break;
    index = i;                                        // -1: nothing displayed
    NiSwitchNode::OnVisible(pass);
}

void NiFltAnimationNode::OnVisible(NiCullPass& pass) {   // orig: 0x00113E50 -> 0x00113D90(lastUpdateTime)
    const u32 n = childCount_;                        // +0xD0
    if (n != 0 && period > 0.0f) {
        double x = std::fmod(double(lastUpdateTime), double(period));   // 0x001674BA (L: CRT fmod)
        if (x < 0.0) x += period;
        double v;
        if (flags & kNiFltPingPong) {
            if (x > double(period) * 0.5) x = double(period) - x;
            v = (double(n) / period) * x;
            v = v + v;
        } else {
            v = (double(n) / period) * x;
        }
        s32 i = static_cast<s32>(v);                  // _ftol
        if (i >= static_cast<s32>(n)) i = static_cast<s32>(n) - 1;
        index = i;
    }
    NiSwitchNode::OnVisible(pass);
}

}  // namespace fut::ni

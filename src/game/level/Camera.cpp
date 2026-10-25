// The game camera; see Camera.h and docs/analysis/camera.md (section numbers below). x87 conventions as in the
// physics code: doubles for values on the x87 stack, F() = an FSTP to float, each comparison by its FNSTSW idiom.
#include "game/level/Camera.h"

#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "engine/ni/NiCollision.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiRenderer.h"
#include "game/GameGlobals.h"
#include "game/GameTime.h"
#include "game/defs/Defs.h"
#include "game/level/ClosestPoint.h"
#include "game/level/LevelData.h"
#include "game/level/TriggerManager.h"
#include "game/object/Character.h"
#include "game/object/GenericNpc.h"
#include "game/object/NodeUpdate.h"
#include "game/physics/Physics.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

const NiPoint3 kUnitX{1, 0, 0};
const NiPoint3 kUnitY{0, 1, 0};
const NiPoint3 kUnitZ{0, 0, 1};

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

f32 Bits(u32 b) {
    f32 f;
    std::memcpy(&f, &b, 4);
    return f;
}

// orig: 0x00011000 / 0x00011070: a culling ray cast under the triangle mask 0x60 (flags 7 / 5).
ni::RayHit RayCastMaskedHit(const NiPoint3& start, const NiPoint3& dir, f32& dist, u32 flags) {
    const u8 old = ni::SetTriangleMask(0x60);
    const ni::RayHit hit = ni::RayCast(start, dir, dist, true, flags);
    ni::SetTriangleMask(old);
    return hit;
}

bool RayCastMasked(const NiPoint3& start, const NiPoint3& dir, f32& dist, u32 flags) {
    return static_cast<bool>(RayCastMaskedHit(start, dir, dist, flags));
}

NiPoint3 Sub(const NiPoint3& a, const NiPoint3& b) {
    return {F(double(a.x) - b.x), F(double(a.y) - b.y), F(double(a.z) - b.z)};
}

// orig: 0x00011700 Cross(a, b).y.
f32 CrossY(const NiPoint3& a, const NiPoint3& b) { return F(double(a.z) * b.x - double(b.z) * a.x); }

// orig: 0x00011700 (this = a, out, b): a x b, each component a double difference of float products, stored once.
NiPoint3 CrossX87(const NiPoint3& a, const NiPoint3& b) {
    return {F(double(a.y) * b.z - double(a.z) * b.y), F(double(a.z) * b.x - double(b.z) * a.x),
            F(double(a.x) * b.y - double(a.y) * b.x)};
}

// orig: 0x00011650: sqrt((x x + y y) + z z) (the register).
double Length(const NiPoint3& v) { return std::sqrt((double(v.x) * v.x + double(v.y) * v.y) + double(v.z) * v.z); }

// orig: the follow mode's lock slide point (0x002D96EC..F4, zero at its first use: guard bit 0 of 0x002D96F8).
NiPoint3 g_lockSlide;
bool g_lockSlideInit = false;

// orig: the first-person mode's smoothed stick values (0x00018250 only; zero in a new process): DAT_002D9700 the
// vertical (P+0xB28), DAT_002D96FC the horizontal (P+0xB2C).
f32 g_fpLookV = 0, g_fpLookH = 0;

// orig: the yaw frame of a rotation's column 0 (0x00018050 from the target's vf38): v = (m00, 0, m20) unitized
// (0x000116A0), u = NiPoint3::UNIT_Y (dword copies of 0x002D77EC), w = u x v (each component F(a b - c d)) unitized;
// the columns v, u, w (the pad lanes zero).
ni::NiMatrix3 YawFrame(const ni::NiMatrix3& C) {
    NiPoint3 v{C.m[0][0], 0.0f, C.m[2][0]};
    (void)ni::Unitize(v);
    const NiPoint3 u = kUnitY;
    NiPoint3 w{F(double(u.y) * v.z - double(u.z) * v.y), F(double(u.z) * v.x - double(v.z) * u.x),
               F(double(u.x) * v.y - double(u.y) * v.x)};
    (void)ni::Unitize(w);
    ni::NiMatrix3 R;
    R.m[0][0] = v.x, R.m[1][0] = v.y, R.m[2][0] = v.z;
    R.m[0][1] = u.x, R.m[1][1] = u.y, R.m[2][1] = u.z;
    R.m[0][2] = w.x, R.m[1][2] = w.y, R.m[2][2] = w.z;
    return R;
}

// orig: 0x000163F9..0x00016DC2 (camera.md 2.6.5, the follow mode with Fry's lock on, not the rodent): the level
// triangles in the sphere around the middle of the desired position and the pivot (QuerySphere 0x000FB410, r = D / 2
// + 4, flags 7) facing along des -> pivot are cut by that line; of the cut points (pulled onto the triangle,
// 0x00011CC0) in front of the pivot the cheapest (near the pivot, along the line, on a large triangle) moves the
// desired position off the wall by the radius, eased from the last slide point; D becomes its distance, at least 4
// (when it was above) and at most the old D.
void LockSlide(NiPoint3& des, f32& D, const NiPoint3& tp, f32 r) {
    const NiPoint3 mU{F(double(tp.x) - des.x), F(double(tp.y) - des.y), F(double(tp.z) - des.z)};   // (kept)
    NiPoint3 mN = mU;
    (void)ni::Unitize(mN);                            // FSTP ST0
    const double sx = double(des.x) + tp.x, sy = double(tp.y) + des.y;
    const f32 sz = F(double(des.z) + tp.z);
    const NiPoint3 c{F(sx * 0.5f), F(sy * 0.5f), F(double(sz) * 0.5f)};   // [0x00293AA0]
    const std::vector<phys::TriRef>& list = phys::QuerySphere(c, F(double(D) * 0.5f + 4.0f), 7);   // [0x00293C1C]
    if (list.empty()) return;                         // (DAT_002F8EB8 <= 0: the slide point is kept)
    f32 best = D;                                     // esp+0xA8
    NiPoint3 bestP = des;                             // esp+0x18
    bool found = false;                               // esp+0x2F
    const phys::TriRef* bestTri = nullptr;            // esp+0x88
    for (const phys::TriRef& tr : list) {
        if (tr.Flags() & 0x60) continue;              // TEST byte +0x10, 0x60
        const NiPoint3 n = tr.Normal();               // 0x000FA8C0
        if ((double(mN.x) * n.x + double(n.z) * mN.z) + double(n.y) * mN.y <= 0.0) continue;   // TEST AH,0x41; JNP
        const NiPoint3 v[3] = {tr.V(0), tr.V(1), tr.V(2)};   // (0x00011740: a copy)
        const f32 lo = F(double(des.y) - r);          // FSUB +0x158
        if (v[0].y < lo && v[1].y < lo && v[2].y < lo) continue;   // TEST AH,5; JP / JNP
        const f32 hi = F(double(des.y) + r);
        if (v[0].y > hi && v[1].y > hi && v[2].y > hi) continue;   // TEST AH,0x41; JNE / JE
        // the line des + t mU against the triangle's plane (0x000115C0: F(t) mU)
        const double ax = double(v[0].x) - des.x, ay = double(v[0].y) - des.y, az = double(v[0].z) - des.z;
        const double num = (ax * n.x + az * n.z) + ay * n.y;
        const double den = (double(mU.x) * n.x + double(mU.z) * n.z) + double(mU.y) * n.y;
        const f32 t = F(num / den);                   // FDIVP; FSTP [esp]
        const NiPoint3 o{F(double(t) * mU.x), F(double(t) * mU.y), F(double(t) * mU.z)};
        NiPoint3 p{F(double(des.x) + o.x), F(double(des.y) + o.y), F(double(des.z) + o.z)};
        ClosestPointOnTriangle(p, v);                 // 0x00011CC0
        const double s = ((double(tp.x) - p.x) * mN.x + (double(tp.z) - p.z) * mN.z) + (double(tp.y) - p.y) * mN.y;
        const f32 sF = F(s);                          // FST [esp+0x10C]
        if (s <= 0.0) continue;                       // FCOMP 0; TEST AH,0x41; JNP (a NaN goes on)
        if (p.x == tp.x && p.y == tp.y && p.z == tp.z) continue;   // 0x000114C0 (exact)
        const double lenQ = Length(Sub(p, tp));       // 0x00011650
        const f32 lq = F(lenQ);                       // FST [esp+0x11C]
        if (!(lenQ * 0.8f < sF)) continue;            // FMUL [0x00293C18]; FCOMP; TEST AH,1; JE
        const NiPoint3 e1 = Sub(v[0], v[2]);          // esp+0xB8
        const NiPoint3 e2 = Sub(v[0], v[1]);          // esp+0x38
        const double area = std::fabs(Length(CrossX87(e2, e1)) * 0.5f) * 0.1f;   // [0x00293AA0]; FABS; [0x00293AB0]
        const double X = std::fabs(double(e2.x));     // (on the stack)
        const f32 A = F(std::fabs(double(v[1].x) - v[2].x)), B = F(std::fabs(double(v[2].x) - v[0].x));
        const f32 E = F(std::fabs(double(e2.z)));
        const f32 C = F(std::fabs(double(v[1].z) - v[2].z)), G = F(std::fabs(double(v[2].z) - v[0].z));
        double extX;                                  // FCOMP ST(1) / FCOMP; TEST AH,0x41; JNE: ties go on
        if (A > X && A > B) extX = A;
        else if (X > B) extX = X;
        else extX = B;
        double extZ;
        if (C > E && C > G) extZ = C;
        else if (E > G) extZ = E;
        else extZ = G;
        double ext = std::sqrt(extZ * extZ + extX * extX) * area;   // FSQRT; FMULP
        if (ext > 10.0f) ext = 10.0f;                 // FCOM [0x00293C2C]; TEST AH,0x41; JNE
        const double cost = ((double(lq) - sF) + (10.0f - ext)) + lq;
        if (cost > best) continue;                    // FCOM [esp+0xA8]; TEST AH,0x41; JE (ties / NaN are taken)
        best = F(cost);
        bestP = p;
        found = true;
        bestTri = &tr;
    }
    if (!g_lockSlideInit) {                           // (the first use: NiPoint3::ZERO 0x002F915C)
        g_lockSlideInit = true;
        g_lockSlide = {};
    }
    if (found) {
        const NiPoint3 n = bestTri->Normal();         // 0x000FA8C0
        const double k = double(r) + 0.01f;           // FADD [0x00293AAC]
        const f32 ky = F(k * n.y), kz = F(k * n.z);
        const NiPoint3 w{F(double(bestP.x) + double(n.x) * k), F(double(bestP.y) + ky), F(double(bestP.z) + kz)};
        const double g = Length(Sub(w, g_lockSlide)) * 0.2f;   // [0x00293AB4]
        const f32 gF = F(g);                          // FST [esp+0x30]
        if (g < 1.0f) {                               // FCOMP [0x00293A98]; TEST AH,5; JP
            const double wx = double(gF) * w.x, wy = double(gF) * w.y;
            const f32 wz = F(double(gF) * w.z);
            const double m = 1.0 - gF;                // FLD 1.0; FSUB
            const f32 my = F(m * g_lockSlide.y), mz = F(m * g_lockSlide.z);
            des = {F(double(g_lockSlide.x) * m + wx), F(double(my) + wy), F(double(mz) + wz)};
        } else {
            des = w;
        }
        const NiPoint3 v{F(double(des.x) - tp.x), F(double(des.y) - tp.y), F(double(des.z) - tp.z)};
        const f32 oldD = D;                           // esp+0x30
        D = F(Length(v));                             // FSTP [esp+0x28]
        if (oldD > 4.0f && D < 4.0f) {                // TEST AH,0x41; JNE / TEST AH,5; JP
            D = 4.0f;
            NiPoint3 u = v;
            (void)ni::Unitize(u);
            const f32 uz = F(double(u.z) * 4.0f);
            des = {F(double(tp.x) + double(u.x) * 4.0f), F(double(u.y) * 4.0f + tp.y), F(double(uz) + tp.z)};
        } else if (D > oldD) {                        // FCOMP; TEST AH,0x41; JNE (equal / below / NaN: kept)
            D = oldD;
            NiPoint3 u = v;
            (void)ni::Unitize(u);
            const f32 uz = F(double(oldD) * u.z);
            des = {F(double(tp.x) + double(oldD) * u.x), F(double(oldD) * u.y + tp.y), F(double(tp.z) + uz)};
        }
    }
    g_lockSlide = des;                                // 0x00016DA1
}

// orig: 0x00103290 M^T v.
NiPoint3 TransposeMul(const ni::NiMatrix3& m, const NiPoint3& v) {
    return {F((double(m.m[2][0]) * v.z + double(m.m[1][0]) * v.y) + double(v.x) * m.m[0][0]),
            F((double(m.m[0][1]) * v.x + double(m.m[2][1]) * v.z) + double(m.m[1][1]) * v.y),
            F((double(m.m[0][2]) * v.x + double(m.m[2][2]) * v.z) + double(m.m[1][2]) * v.y)};
}

// orig: NiCamera::LookAt 0x000FFF40 (2.10): the camera looks along its local +X at `target`, +Y up.
bool LookAt(ni::NiCamera& cam, const NiPoint3& target, const NiPoint3& up) {
    const NiPoint3& W = cam.world.translate;
    NiPoint3 b = Sub(W, target);
    if ((double(b.z) * b.z + double(b.y) * b.y) + double(b.x) * b.x < double(1e-10f)) return false;   // 0x002A6430
    ni::Unitize(b);
    NiPoint3 r = ni::UnitCross(up, b);
    if ((double(r.z) * r.z + double(r.y) * r.y) + double(r.x) * r.x < 0.5) return false;
    NiPoint3 u = ni::UnitCross(b, r);
    if ((double(u.z) * u.z + double(u.y) * u.y) + double(u.x) * u.x < 0.5) return false;
    if (cam.parent) {                                 // into the parent's space
        const ni::NiMatrix3& pr = cam.parent->world.rotate;
        b = TransposeMul(pr, b);
        r = TransposeMul(pr, r);
        u = TransposeMul(pr, u);
    }
    ni::NiMatrix3 A, Bk;
    A.m[0][0] = r.x, A.m[1][0] = r.y, A.m[2][0] = r.z;   // columns r, u, b
    A.m[0][1] = u.x, A.m[1][1] = u.y, A.m[2][1] = u.z;
    A.m[0][2] = b.x, A.m[1][2] = b.y, A.m[2][2] = b.z;
    Bk.m[0][0] = 0, Bk.m[1][0] = 0, Bk.m[2][0] = -1;     // columns (0,0,-1), (0,1,0), (1,0,0)
    Bk.m[0][1] = 0, Bk.m[1][1] = 1, Bk.m[2][1] = 0;
    Bk.m[0][2] = 1, Bk.m[1][2] = 0, Bk.m[2][2] = 0;
    cam.local.rotate = ni::Multiply(A, Bk);           // orig: 0x00013B30
    cam.UpdateWorldData();                            // orig: vf9C
    cam.UpdateWorldBound();                           // orig: vf98
    return true;
}

// orig: 0x00011B50 (cdecl(out, f32 u, a*, b*, c*)): the point at u of the quadratic Bezier curve a, b, c (the lerp of
// the two lerps; level2-2_port.md 4.3).
NiPoint3 QuadraticPath(f32 u, const NiPoint3& a, const NiPoint3& b, const NiPoint3& c) {
    if (u < 0.0f) return a;                           // FCOMP [0x00293AB8]; TEST AH,5; JP
    if (u > 1.0f) return c;                           // FCOMP [0x00293A98]; TEST AH,0x41; JNE
    const double d1x = double(b.x) - a.x, d1y = double(b.y) - a.y;
    const f32 d1z = F(double(b.z) - a.z);
    const f32 px = F(double(F(d1x * u)) + a.x), py = F(d1y * u + a.y);
    const double pz = double(d1z) * u + a.z;          // (stays on the x87 stack)
    const double d2x = double(c.x) - b.x, d2y = double(c.y) - b.y;
    const f32 d2z = F(double(c.z) - b.z);
    const f32 qx = F(double(F(d2x * u)) + b.x), qy = F(d2y * u + b.y);
    const double qz = double(d2z) * u + b.z;          // (unrounded)
    const double ex = double(qx) - px;                // (unrounded)
    const f32 ey = F(double(qy) - py), ez = F(qz - pz);
    const f32 rz = F(double(ez) * u);
    return {F(ex * u + px), F(double(ey) * u + py), F(double(rz) + pz)};
}

}  // namespace

bool NiCameraLookAt(ni::NiCamera& cam, const ni::NiPoint3& target, const ni::NiPoint3& up) {
    return LookAt(cam, target, up);
}

void ResetCameraStatics() {
    g_lockSlide = {};
    g_lockSlideInit = false;
    g_fpLookV = g_fpLookH = 0;
}

// ---- 1.1 construction -------------------------------------------------------------------------------------------
Camera::Camera(LevelData& lvl) : level(lvl) {         // orig: 0x00019DB0
    ApplyPresets(0);                                  // orig: 0x00011380(0)
    // the overlay camera +0x34 (0x0001A2BD: new(0x250) 0x001009D0, +0x1A8 = App->vf14(), UpdateProperties /
    // UpdateEffects, Update(0, 1))
    overlayCamera = new ni::NiCamera;
    overlayCamera->renderer = level.renderer;
    overlayCamera->UpdateProperties();                // 0x0001A323
    overlayCamera->UpdateEffects();
    overlayCamera->UpdateDownwardPass(0.0f, true);
    // 0x000194D0: the fade polygon (once): the fade colours zeroed, a 4-vertex polygon (0, 1) (0, 0) (1, 0) (1, 1)
    // with the colour +0x1FC on each vertex and no texture coordinates, on the overlay camera (+0x23C = 1), with a
    // NiVertexColorProperty (source 1, lighting 0) and a NiAlphaProperty (0x2ED: blend and test)
    if (!fadePolygon) {
        fadeColor = fadeColor20C = fadeColor21C = fadeColor22C = {};
        const ni::NiPoint3 v[4] = {{0, 1, 0}, {0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
        const ni::NiColorA c{fadeColor[0], fadeColor[1], fadeColor[2], fadeColor[3]};
        const ni::NiColorA cs[4] = {c, c, c, c};
        fadePolygon = new ni::NiScreenPolygon(4, v, nullptr, cs);
        overlayCamera->AttachScreenPolygon(fadePolygon.Get());   // 0x00019460
        fadeAttached = 1;
        auto* vc = new ni::NiVertexColorProperty;
        vc->sourceVertexMode = 1, vc->lightingMode = 0;
        fadePolygon->AttachProperty(vc);
        auto* a = new ni::NiAlphaProperty;
        a->flags = 0x02ED;
        fadePolygon->AttachProperty(a);
    }
    // (+0x1EC..+0x1F4 = 0: the wobble, off)
    fogProperty = new ni::NiFogProperty;              // 0x0001A651: new(0x28), vtbl 0x00293B88
    fogProperty->flags = 0;                           // (flags 0, & 0xE5, & ~2, & 0xE7: fog off)
    fogProperty->depth = 1.0f;
    fogProperty->color = {0.0f, 0.0f, 0.0f};
    if (level.scene) AttachProperty(*level.scene, fogProperty.Get());   // S+0x18 (0x00013E20), AddHead 0x00013A50
    fogEnabled = 1;                                   // +0x1E0 = 1, then DisableFog 0x00012DB0
    DisableFog();
    fogProperty->depth = 1.0f;                        // +0x18
    SetFogColor(std::bit_cast<f32>(0x3DCCCCCDu), std::bit_cast<f32>(0x3E4CCCCDu), 0.0f);   // 0x00012CC0
    SetFogDistance(100.0f);                           // 0x00011230 (0x42C80000)
    SetFade(0.0f, 0.0f, 0.0f, 0.5f, 0.1f);            // 0x0001A870: the level-start fade from black
    // orig: one Camera::Update inside the constructor (mode 0 without a target returns at once).
    Update(0.0f);
}

void Camera::ApplyPresets(s32 k) {                    // orig: 0x00011380 (1.2)
    pitchGain = 50.0f;
    preset184 = Bits(0x3DCCCCCD);
    yawGain = 35.0f;
    if (k == 0) {
        presetHeight = Bits(0x40BC7AE1);
        distance = Bits(0x41633333);
    } else if (k == 1) {
        presetHeight = 4.0f;
        distance = 17.0f;
        pivotHeight = 7.0f;
        kind = k;
        return;
    } else if (k == 2 || k == 3) {
        presetHeight = Bits(0x40D2265B);
        distance = Bits(0x4192090F);
    } else if (k == 4) {
        presetHeight = 26.0f;
        distance = 32.0f;
    } else {
        kind = k;
        return;
    }
    pivotHeight = Bits(0x40B0D402);
    preset170 = Bits(0x3F60B0F2);
    preset17C = Bits(0x3F528F5C);
    kind = k;
}

// ---- 2.3 SetFollowTarget ----------------------------------------------------------------------------------------
void Camera::RestoreTargetModel() {
    if (!target || mode != 3) return;                 // +0x44; +0x15C == 3
    ni::NiAVObject* m = target->model.Get();          // +0x564 (no null test in the original)
    if (!m) return;
    if (modelHidden) m->flags |= ni::kNiAVHidden;     // +0x1C0: OR (byte / word) +0xB0, 1
    else m->flags &= ~ni::kNiAVHidden;                // AND +0xB0, 0xFE
}

// The re-entrancy guard DAT_002D94E1 that 0x00014150 and the guarded setters test is set only inside 0x00014150
// around its PopTransform / discard (which call no setter), so the setters always see it clear.
bool Camera::LeaveModeCheck(u32 newMode) {            // orig: 0x00014150
    if (newMode == 3) {                               // entering first person (no caller passes 3)
        PushTransform();                              // 0x000132F0
        prevMode = mode;                              // +0x160 = +0x15C
        return true;
    }
    RestoreTargetModel();
    if (mode != 3 || newMode != 0) return true;
    // leaving first person: the pushed mode decides (byte table 0x00014230 over +0x160 - 1, jump table 0x00014228)
    const u32 k = prevMode - 1;
    const bool restore = k <= 8 && k != 2 && k != 3; // 1, 2, 5..9: back to it; 0, 3, 4 (and others): the follow set-up
    if (restore) {
        PopTransform();                               // 0x00011310
        mode = prevMode;                              // +0x15C = +0x160
        return false;
    }
    if (stackTop >= 0) --stackTop;                    // +0x750 (the pushed transform dropped)
    return true;
}

void Camera::SetFollowTarget(Character* c) {          // orig: 0x00018FB0
    if (!LeaveModeCheck(0)) return;                   // orig: 0x00014150(0)
    RestoreTargetModel();                             // (again: 0x00018FC7)
    follow = Follow{};                                // orig: 0x00011A90
    if (c) {
        const phys::CharShape* box = c->body ? &c->body->shape : nullptr;
        if (box) targetExtent = box->ext.x > box->ext.z ? box->ext.x : box->ext.z;   // FCOM: ties take +0x98
        target = c;
        const NiPoint3 wp = c->GetWorldPosition();    // orig: vf34
        const f32 y = F(double(pivotHeight) + wp.y);
        pivotAtFollow = pivotCopy = {wp.x, y, wp.z};
        positionAtFollow = position;
    }
    if (mode == 0 || mode == 3 || mode == 4) PlaceBehindTarget();
    ApplyPresets(kind);
    yawVelocity = 0;
    mode = 0;
    modeFrames = 0;
    recentre = 0;
}

// ---- the script set-ups (camera natives) -------------------------------------------------------------------------
void Camera::ResetFov() {                             // orig: the FOV lines of 0x00019110 / 0x00018860 / 0x00018B50
    fov = 90.0f;
    unknownB0 = Bits(0xC23DE148);
    dirtyFrustum = 1;
    halfWidth = 1.0f;                                 // F(FPTAN(pi/4)) (the extended 1.00000004371139)
    halfHeight = Bits(0x3F400001);                    // F(tan * 0.75f) of the unrounded tan
}

// orig: 0x000110E0 (thiscall, (f32 deg), RET 4): FPTAN of deg * 0.0087266462f (pi / 360), the half width its rounded
// value, the half height the unrounded tan * 0.75f. The port's tan is the C library's (KNOWN_ISSUES #20; SetFov(50)
// gives the original's 3EEEBFE0 / 3EB30FE8).
void Camera::SetFov(f32 deg) {
    fov = deg;                                        // +0xAC
    unknownB0 = Bits(0xC23DE148);                     // +0xB0
    dirtyFrustum = 1;                                 // +0xA0
    const double t = std::tan(double(deg) * double(Bits(0x3C0EFA35)));   // FMUL [0x00293A90]; FPTAN
    halfWidth = F(t);                                 // FST +0xA4
    halfHeight = F(t * 0.75f);                        // FMUL [0x00293A94]; FSTP +0xA8
}

void Camera::SetStatic(const NiPoint3* pos, const ni::NiMatrix3* rot) {   // orig: 0x00015040
    RestoreTargetModel();                             // the model bit in mode 3 (twice: 0x0001505E guarded, 0x0001508C not)
    if (pos) {
        position = *pos;
        dirtyTranslate = 1;
    }
    if (rot) {
        rotation = *rot;
        dirtyRotate = 1;
    }
    mode = 1;
    modeFrames = 0;
}

void Camera::SetStaticTarget(const NiPoint3* pos, TriggerRec* rec, f32 ease) {   // orig: 0x00014550
    RestoreTargetModel();                             // the model bit in mode 3 (twice, as SetStatic)
    goal = pos ? *pos : position;
    if (rec) tracked = rec;
    easing = ease;
    mode = 2;
    modeFrames = 0;
    if (ease == 0.0f && tracked) {                    // FCOMP [0x00293AB8]: placed at once
        position = *pos;
        dirtyTranslate = 1;
        lookAt = level.triggers->Position(*tracked);  // orig: 0x000DA7E0
        dirtyLookAt = 1;
    }
}

// orig: 0x00014240 (thiscall(a*, b*, rec, time), RET 0x10). level2-2_port.md 2.7.
void Camera::SetPathToStaticTarget(const NiPoint3& a, const NiPoint3& b, TriggerRec* rec, f32 time) {
    // (byte 0x002D94E1, the re-entrancy guard, gates the first-person model bit)
    RestoreTargetModel();                             // the model bit in mode 3
    pathDuration = time;                              // +0x110
    tracked = rec;                                    // +0x40
    pathTime = 0;                                     // +0x10C
    pathStart = position;                             // +0xE8
    pathEnd = b;                                      // +0x100
    const double dx = double(pathEnd.x) - pathStart.x, dy = double(pathEnd.y) - pathStart.y;
    const f32 dz = F(double(pathEnd.z) - pathStart.z);
    const f32 mx = F(double(F(dx * 0.5f)) + pathStart.x);   // FMUL [0x00293AA0]; FSTP; FLD; FADD
    const f32 my = F(dy * 0.5f + pathStart.y);
    const double mz = double(dz) * 0.5f + pathStart.z;     // (unrounded)
    const double cx = double(a.x) - mx;               // (unrounded)
    const f32 cy = F(double(a.y) - my), cz = F(double(a.z) - mz);
    pathControl = {F(cx + a.x), F(double(cy) + a.y), F(double(cz) + a.z)};   // +0xF4: 2a - mid(start, end)
    pathLook = lookAt;                                // +0x124
    mode = 6;                                         // (+0x198 is not reset)
}

// orig: 0x00014990 (thiscall(name, NiPoint3* v), RET 8). docs/analysis/object_gaps.md 12.1.
void Camera::SetFixedVector(const char* name, const NiPoint3& v) {
    ResetFov();                                       // (inline: +0xAC = 90, +0xB0, +0xA0 = 1, FPTAN(pi/4))
    TriggerRec* rec = level.triggers ? level.triggers->FindOrCreate(name) : nullptr;   // 0x000DB390
    if (!rec || !rec->target) return;                 // rec+0xC (the record is not null-tested)
    RestoreTargetModel();                             // the model bit in mode 3 (not guarded)
    follow.unknown30 = 1.0f;                          // (+0xE4)+0x30
    target = static_cast<Character*>(rec->target);   // +0x44
    mode = 8;                                         // (+0x198 is not reset)
    fixedDir = v;                                     // +0xD4
    fixedLength = F(ni::Unitize(fixedDir));           // +0xE0
    fixedHeight = F(-double(v.y));                    // FLD v.y; FCHS; FSTP (+0xE4)+0x40
    const double L = fixedLength;                     // FLD +0xE0
    const double lx = L * fixedDir.x;                 // (unrounded)
    const f32 ly = F(L * fixedDir.y), lz = F(L * fixedDir.z);
    fixedAnchor = {F(double(position.x) + lx), F(double(position.y) + ly), F(double(position.z) + lz)};   // +0x48
}

// orig: 0x00014B00 (thiscall(dt), RET 4). docs/analysis/object_gaps.md 12.2.
// ---- mode 9: the rail (object_gaps.md 17) --------------------------------------------------------------------------
void Camera::SetRail(const char* name, const char* net) {   // orig: 0x00014F40
    ResetFov();                                       // (inline: FPTAN of [0x00293BC0], +0xAC = 90, +0xB0, +0xA0 = 1)
    railNet = level.FindAINet(net);                   // S+0x114 0x000313E0
    if (name && crt::StrICmp(name, "PLAYER") != 0 && crt::StrICmp(name, "FOGNODE") == 0)   // 0x00293BD4 / 0x00293BCC
        railNode = fogNode.Get();                     // 0x00034250(&+0x1E4)
    else
        railNode = target ? target->model.Get() : nullptr;   // (C+0x44)+0x564 (the target is not tested)
    rail148 = 0;                                      // +0x148
    mode = 9;                                         // +0x15C (+0x198 not reset)
}

void Camera::RailMode() {                             // orig: 0x000125B0 (thiscall(dt), RET 4)
    AiNet* net = railNet;
    if (!net || !railNode) return;
    const NiPoint3 T = railNode->world.translate;     // +0xA0
    AiNetNode* a = net->Nearest(T);                   // 0x00031D80
    if (!a || a->count == 0) return;
    AiNetNode* b = NodeAt(*net, LinkNode(*a, 0));     // 0x00031C00 / 0x00031C20
    if (!b) return;
    {
        const NiPoint3 t{F(double(T.x) - a->pos.x), F(double(T.y) - a->pos.y), F(double(T.z) - a->pos.z)};
        const NiPoint3 e{F(double(b->pos.x) - a->pos.x), F(double(b->pos.y) - a->pos.y), F(double(b->pos.z) - a->pos.z)};
        if ((double(t.z) * e.z + double(t.y) * e.y) + double(t.x) * e.x < 0.0f) {   // FCOMP 0; TEST AH,5; JP
            do {                                      // behind the nearest node: back along link 1
                b = a;
                a = NodeAt(*net, LinkNode(*a, 1));
                if (!a) return;
            } while (((double(b->pos.z) - a->pos.z) * (double(T.z) - a->pos.z) +
                      (double(b->pos.y) - a->pos.y) * (double(T.y) - a->pos.y)) +
                         (double(b->pos.x) - a->pos.x) * (double(T.x) - a->pos.x) < 0.0f);   // TEST AH,5; JNP
        }
    }
    NiPoint3 dir{F(double(b->pos.x) - a->pos.x), F(double(b->pos.y) - a->pos.y), F(double(b->pos.z) - a->pos.z)};
    f32 len = F(ni::Unitize(dir));                    // FSTP [esp+0x10]
    f32 proj = F((double(dir.z) * (double(T.z) - a->pos.z) + double(dir.y) * (double(T.y) - a->pos.y)) +
                 double(dir.x) * (double(T.x) - a->pos.x));   // [esp+0x18]
    const NiPoint3 q{F(double(proj) * dir.x), F(double(proj) * dir.y), F(double(proj) * dir.z)};   // 0x000115C0
    const NiPoint3 Q{F(double(q.x) + a->pos.x), F(double(q.y) + a->pos.y), F(double(q.z) + a->pos.z)};   // 0x00011500
    double st = double(len) - proj;                   // FLD; FSUB; FCOM 0; TEST AH,5; JP
    if (st < 0.0f) st = 0.0;
    const f32 ahead = F(double(distance) * 2.25f);    // +0x168 * [0x00293B14]
    f32 acc = 0;                                      // [esp+0x14]
    if (ahead > st) {                                 // FCOMP ST1; TEST AH,0x41; JNE
        double next;
        do {                                          // the next segments along link 0
            acc = F(double(acc) + st);
            a = b;
            b = NodeAt(*net, LinkNode(*b, 0));
            if (!b) return;
            dir = {F(double(b->pos.x) - a->pos.x), F(double(b->pos.y) - a->pos.y), F(double(b->pos.z) - a->pos.z)};
            st = ni::Unitize(dir);                    // FST [esp+0x10] (ST keeps the unrounded length)
            len = F(st);
            proj = 0;
            next = double(acc) + len;
        } while (next < ahead);                       // FCOMP [esp+0x1C]; TEST AH,5; JNP
    }
    const double k = ((double(ahead) - acc) + proj) / len;   // FSUB; FADD; FDIV (unrounded)
    const double X = double(dir.x) * len * k + a->pos.x;
    const f32 zl = F(F(double(dir.z) * len) * k);
    const double Z = double(zl) + a->pos.z;
    const f32 yq = F(double(Q.y) + pivotHeight);      // +0x174
    const double Y = double(presetHeight) * 2.5f + yq;   // +0x16C * [0x00293B10]
    const NiPoint3 d{F(X - position.x), F(Y - position.y), F(Z - position.z)};
    const f32 s = F(1.0 - preset170);                 // FLD [0x00293A98]; FSUB +0x170
    const NiPoint3 o{F(double(s) * d.x), F(double(s) * d.y), F(double(s) * d.z)};   // 0x000115C0
    const NiPoint3 np{F(double(position.x) + o.x), F(double(position.y) + o.y), F(double(position.z) + o.z)};
    const NiPoint3 dl{F(double(Q.x) - lookAt.x), F(double(yq) - lookAt.y), F(double(Q.z) - lookAt.z)};
    const NiPoint3 ol{F(double(s) * dl.x), F(double(s) * dl.y), F(double(s) * dl.z)};
    const NiPoint3 nl{F(double(ol.x) + lookAt.x), F(double(ol.y) + lookAt.y), F(double(ol.z) + lookAt.z)};   // 0x00011500
    position = np;                                    // 0x00011120
    dirtyTranslate = 1;
    lookAt = nl;                                      // 0x00011150
    dirtyLookAt = 1;
}

// ---- mode 5: the path to the chase camera --------------------------------------------------------------------------
void Camera::SetPathToChase(const NiPoint3& a, const NiPoint3& b, Character* c, f32 time, u8 follow) {
    // orig: 0x000143A0 (byte 0x002D94E1, the re-entrancy guard, gates the first-person model bit)
    RestoreTargetModel();                             // the model bit in mode 3
    target = c;                                       // +0x44
    tracked = c->label && level.triggers ? level.triggers->FindOrCreate(c->label->c_str()) : nullptr;   // (c+0x34)
    pathDuration = time;                              // +0x110
    pathTime = 0;                                     // +0x10C
    pathStart = position;                             // +0xE8
    pathEnd = b;                                      // +0x100
    // the control point 2a - mid(start, end) (the same code as 0x00014240's)
    const double dx = double(pathEnd.x) - pathStart.x, dy = double(pathEnd.y) - pathStart.y;
    const f32 dz = F(double(pathEnd.z) - pathStart.z);
    const f32 mx = F(double(F(dx * 0.5f)) + pathStart.x);   // FMUL [0x00293AA0]; FSTP; FLD; FADD
    const f32 my = F(dy * 0.5f + pathStart.y);
    const double mz = double(dz) * 0.5f + pathStart.z;     // (unrounded)
    const double cx = double(a.x) - mx;               // (unrounded)
    const f32 cy = F(double(a.y) - my), cz = F(double(a.z) - mz);
    pathControl = {F(cx + a.x), F(double(cy) + a.y), F(double(cz) + a.z)};   // +0xF4
    mode = 5;                                         // (+0x198 is not reset)
    pathFollow = follow;                              // +0x114
    pathTargetStart = c->GetWorldPosition();          // +0x118 = vf34
    pathLook = lookAt;                                // +0x124
}

void Camera::PathToChaseMode(f32 dt) {                // orig: 0x00019180 (thiscall(f32 dt), RET 4)
    const double t = double(dt) + pathTime;           // FLD dt; FADD +0x10C
    pathTime = F(t);                                  // FST
    if (t >= pathDuration) pathTime = pathDuration;   // FCOMP +0x110 (unrounded sum); TEST AH,1; JNE (NaN: kept)
    const f32 u = F(double(pathTime) / pathDuration); // FLD +0x10C; FDIV +0x110; FSTP (the float argument)
    if (pathFollow) {                                 // +0x114: the end moved with the target (target + end - start)
        const NiPoint3 tp = target->GetWorldPosition();   // vf34
        const f32 ez = F(double(tp.z) + pathEnd.z);
        const NiPoint3 e{F((double(tp.x) + pathEnd.x) - pathTargetStart.x),
                         F((double(tp.y) + pathEnd.y) - pathTargetStart.y), F(double(ez) - pathTargetStart.z)};
        position = QuadraticPath(u, pathStart, pathControl, e);   // 0x00011B50
    } else {
        position = QuadraticPath(u, pathStart, pathControl, pathEnd);
    }
    dirtyTranslate = 1;                               // +0xA1
    NiPoint3 L;
    if (target) {                                     // the target's pivot (+0x174 above its position)
        const f32 ph = pivotHeight;                   // (FSTP: a float copy first)
        const NiPoint3 tp = target->GetWorldPosition();
        L = {tp.x, F(double(ph) + tp.y), tp.z};
    } else {
        L = level.triggers->Position(*tracked);       // 0x000DA7E0 (+0x40; no null test in the original)
    }
    const double mx = double(L.x) - pathLook.x, my = double(L.y) - pathLook.y;
    const f32 mz = F(double(L.z) - pathLook.z);
    const f32 w = F(1.0 - (double(pathDuration) - pathTime) / pathDuration);   // FLD; FSUB; FDIV; FSUBR 1.0; FSTP
    const f32 bx = F(mx * w);
    lookAt = {F(double(bx) + pathLook.x), F(my * w + pathLook.y), F(double(mz) * w + pathLook.z)};
    dirtyLookAt = 1;                                  // +0xA2
    if (pathTime >= pathDuration) SetFollowTarget(target);   // FCOMP +0x110; TEST AH,1; JNE; 0x00018FB0
}

// ---- mode 7: the side camera ---------------------------------------------------------------------------------------
void Camera::SetSide(Character* t, TriggerRec* a, TriggerRec* b) {   // orig: 0x00014690 (thiscall, RET 0xC)
    // (byte 0x002D94E1, the re-entrancy guard, gates the first-person model bit)
    RestoreTargetModel();                             // the model bit in mode 3
    if (!t || !a || !b) return;
    target = t;                                       // +0x44
    sideA = a;                                        // +0x130
    sideB = b;                                        // +0x134
    mode = 7;                                         // (+0x198 is not reset)
}

void Camera::SideMode() {                             // orig: 0x00014700 (thiscall(dt), RET 4; dt not read)
    const TriggerManager& tm = *level.triggers;
    const NiPoint3 a = tm.Position(*sideA);           // 0x000DA7E0 (+0x130)
    NiPoint3 dir = Sub(tm.Position(*sideB), a);       // (+0x134) FLD b; FSUB a; FSTP
    (void)ni::Unitize(dir);                           // 0x000116A0 (FSTP ST0)
    const NiPoint3 tp = target->GetWorldPosition();   // vf34
    const double dy = double(tp.y) - tm.Position(*sideA).y;   // (a read again; the register)
    const NiPoint3 p{F(double(dir.x) * dy), F(double(dir.y) * dy), F(double(dir.z) * dy)};
    const NiPoint3 a3 = tm.Position(*sideA);          // (a third time)
    NiPoint3 q{F(double(p.x) + a3.x), F(double(p.y) + a3.y), F(double(p.z) + a3.z)};   // the line at the target's height
    NiPoint3 v = Sub(q, target->GetWorldPosition());  // vf34
    (void)ni::Unitize(v);
    height = F((double(target->GetWorldPosition().y) + pivotHeight) + presetHeight);   // FADD +0x174, +0x16C; +0xD0
    SideProbe(v, q);                                  // 0x00013360(&v, &q)
    const double dx = double(q.x) - position.x, dyy = double(q.y) - position.y;   // (the registers)
    const f32 dz = F(double(q.z) - position.z);
    const f32 w = F(1.0 - preset170);                 // FLD [0x00293A98]; FSUB +0x170; FSTP
    const f32 sx = F(dx * w);
    position = {F(double(position.x) + sx), F(double(position.y) + dyy * w), F(double(position.z) + double(dz) * w)};
    if (sceneCamera) sceneCamera->local.translate = position;   // (+0x30)+0x60 directly (no +0xA1)
    // the look-at eases to the target's head (def +0xA0 above its position), by 1 - +0x170 (the register)
    const f32 head = target->cdef.fpsHeight;          // (T+0x10)+0xA0 (FSTP: a float copy)
    const NiPoint3 tq = target->GetWorldPosition();   // vf34
    const f32 ax = F(double(tq.x) - lookAt.x);
    const double ay = (double(head) + tq.y) - lookAt.y;
    const double az = double(tq.z) - lookAt.z;
    const double w2 = 1.0 - double(preset170);
    const f32 lx = F(double(ax) * w2), ly = F(ay * w2);
    lookAt = {F(double(lx) + lookAt.x), F(double(ly) + lookAt.y), F(az * w2 + lookAt.z)};
    dirtyLookAt = 1;                                  // +0xA2
}

void Camera::SideProbe(const NiPoint3& dir, NiPoint3& out) {   // orig: 0x00013360 (RET 8)
    const f32 ph = pivotHeight;                       // +0x174 (copied first)
    const NiPoint3 tp = target->GetWorldPosition();   // vf34
    const double cy = double(ph) + tp.y;              // FADD; FST (the register is used below)
    const NiPoint3 c{tp.x, F(cy), tp.z};              // the pivot
    const double D = distance;                        // +0x168
    const f32 ex = F(double(tp.x) - D * dir.x);       // FSUB ST1; FSTP
    const f32 ez = F(double(tp.z) - double(F(D * dir.z)));   // FSTP [esp+0x38]; FSUB
    NiPoint3 r{F(double(ex) - c.x), F(double(height) - cy), F(double(ez) - tp.z)};   // +0xD0 - the register
    f32 t = F(ni::Unitize(r) + radius);               // the length (register) + +0x158; FSTP
    ni::RayHit hit = RayCastMaskedHit(c, r, t, 7);    // 0x00011000(c, r, &t) (by value; mask 0x60, cull)
    f32 tt = F(double(t) - radius);                   // FLD t; FSUB +0x158; FSTP
    auto along = [&] {                                // 0x00013660: c + r t'
        out = {F(double(r.x) * tt + c.x), F(double(r.y) * tt + c.y), F(double(F(double(r.z) * tt)) + c.z)};
    };
    auto lifted = [&](const ni::RayHit& h) {          // 0x00013469 / 0x000135BC: c + r t' + +0x174 n
        const NiPoint3 n = h.Normal();                // 0x000FA8C0
        const double kx = double(ph) * n.x, ky = double(ph) * n.y;
        const f32 kz = F(double(ph) * n.z);
        const f32 hx = F(double(r.x) * tt + c.x);
        const double hy = double(r.y) * tt + c.y;
        const f32 hz = F(double(F(double(r.z) * tt)) + c.z);
        out = {F(double(hx) + kx), F(hy + ky), F(double(hz) + kz)};
    };
    if (!hit) return along();
    if (hit.Flags() & 0x40) return lifted(hit);       // 0x000FA320 (byte 0)
    if (!(hit.Normal().y < std::bit_cast<f32>(0xBF333333u))) return along();   // FCOMP [0x00293B48] -0.7; TEST AH,5; JP
    // a ceiling: the direction again from the hit's height (the register), cast without the mask nor the cull
    const double Y = double(r.y) * tt + c.y;
    r = {F(double(ex) - c.x), F(Y - c.y), F(double(ez) - c.z)};
    t = F(ni::Unitize(r) + radius);
    const ni::RayHit h2 = ni::RayCast(c, r, t, false, 7);   // 0x000FB780(&c, &r, &t, 0, 7)
    tt = F(double(t) - radius);
    if (h2 && (h2.Flags() & 0x40)) return lifted(h2);
    along();
}

void Camera::FixedVectorMode(f32 dt) {
    Follow& H = follow;
    const f32 a = F(double(fixedLength) * fixedDir.x), c = F(double(fixedLength) * fixedDir.z);
    const NiPoint3 tp = target->GetWorldPosition();   // vf34 (no null test)
    const f32 qx = F(double(tp.x) - a), qz = F(double(tp.z) - c);
    if (fixedDir.y < -0.9f || fixedDir.y == -0.9f) H.unknown30 = 0;   // FCOMP [0x00293BC8]; TEST AH,0x41; JP
    if (H.unknown30 > preset170)                      // FCOMP +0x170; TEST AH,0x41; JNE
        H.unknown30 = F(double(H.unknown30) - double(dt) * 0.1f);   // FMUL [0x00293AB0]; FSUBR
    const f32 s = H.unknown30;                        // (FST over the dt slot)
    f32 k = s;
    if (H.unknown30 < 0.0f) H.unknown30 = 0;          // TEST AH,5; JP
    if (s < preset170) k = preset170;                 // (the value before the clamp) TEST AH,5; JP
    presetHeight = F((1.0 - H.unknown30) * (double(fixedHeight) - presetHeight) + presetHeight);   // +0x16C
    Player* P = level.player;                         // S+0x94 (no null test in the original)
    if (!P->JumpingOrFalling()) {                     // vf130 0x000500F0
        const NiPoint3 t2 = target->GetWorldPosition();
        height = F((double(t2.y) + pivotHeight) + presetHeight);   // FADD +0x174; FADD +0x16C; FSTP +0xD0
    } else {
        height = position.y;
    }
    const double dqx = double(qx) - position.x;       // (unrounded)
    const f32 ey = F(double(height) - position.y), ez = F(double(qz) - position.z);
    const f32 m = F(1.0 - k);                         // FLD 1.0; FSUB; FSTP
    const f32 ezm = F(double(ez) * m);
    NiPoint3 n{F(double(position.x) + dqx * m), F(double(position.y) + double(ey) * m), F(double(position.z) + ezm)};
    {
        const double L = fixedLength;
        const f32 ly = F(L * fixedDir.y), lz = F(L * fixedDir.z);
        fixedAnchor = {F(L * fixedDir.x + n.x), F(double(ly) + n.y), F(double(lz) + n.z)};   // (+0xE4)+0x34
    }
    if (fixedDir.y > -0.9f) {                         // TEST AH,0x41; JNE (<, == and NaN: the anchor itself)
        const double ax = double(fixedAnchor.x) - lookAt.x, ay = double(fixedAnchor.y) - lookAt.y;
        const f32 az = F(double(fixedAnchor.z) - lookAt.z);
        const f32 w = F(1.0 - H.unknown30);
        const f32 tx = F(ax * w);
        lookAt = {F(double(tx) + lookAt.x), F(ay * w + lookAt.y), F(double(az) * w + lookAt.z)};   // +0x54
    } else {
        lookAt = fixedAnchor;
    }
    dirtyLookAt = 1;                                  // +0xA2
    NiPoint3 d{F(double(n.x) - position.x), F(double(n.y) - position.y), F(double(n.z) - position.z)};
    const f32 L = F(ni::Unitize(d));
    f32 t = 0;                                        // (the dt slot again)
    if (0.0f < L) {                                   // FLD 0.0; FCOMP L; TEST AH,5; JP
        for (;;) {                                    // the first point along d the push-out moves
            const f32 zz = F(double(d.z) * t);
            NiPoint3 cand{F(double(d.x) * t + position.x), F(double(position.y) + double(d.y) * t),
                          F(double(position.z) + zz)};
            if (PushOut(cand)) {                      // 0x000129C0
                n = cand;
                break;
            }
            const double x = double(radius) * 0.5f + t;   // FLD +0x158; FMUL [0x00293AA0]; FADD; FST
            t = F(x);
            if (!(x < L)) break;                      // TEST AH,5; JNP (loop)
        }
        if (t == 0.0f) PushOut(n);                    // TEST AH,0x44; JP
    } else {
        PushOut(n);
    }
    position = n;                                     // +0x48
    dirtyTranslate = 1;                               // +0xA1
}

void Camera::SetFade(f32 r, f32 g, f32 b, f32 time, f32 a5) {   // orig: 0x00012FB0 (raw dword stores)
    fadeTime = time;                                  // +0x240
    fadeElapsed = -a5;                                // +0x24C (FLD; FCHS; FSTP: -0.0 for FsFadeIn)
    fade244 = fade248 = 0;                            // +0x244 / +0x248
    fadeColor = {r, g, b, 1.0f};                      // +0x1FC
    fadeColor21C = {r, g, b, 1.0f};                   // +0x21C
    fadeColor20C = {};                                // +0x20C
    fadeColor22C = {};                                // +0x22C
}

void Camera::FadeOut(f32 r, f32 g, f32 b, f32 time) {   // orig: 0x00013060 (raw dword stores)
    fadeElapsed = 0.0f;                               // +0x24C
    fadeTime = (time > 0.0f || std::isnan(time)) ? time : g_frameStep;   // TEST AH,0x41; JP (twice): else dt
    fade244 = 0.0f;                                   // +0x244
    fade248 = 0.0f;                                   // +0x248
    fadeColor21C = fadeColor;                         // +0x21C = +0x1FC
    fadeColor20C = {r, g, b, 1.0f};                   // +0x20C
    fadeColor22C = {r, g, b, 1.0f};                   // +0x22C
}

void Camera::FadeTo(f32 r, f32 g, f32 b, f32 a, f32 time, f32 v244, f32 v248) {   // orig: 0x00013130 (raw dwords)
    fadeTime = time;                                  // +0x240
    fade244 = v244;                                   // +0x244
    fade248 = v248;                                   // +0x248
    fadeElapsed = 0.0f;                               // +0x24C
    fadeColor21C = fadeColor;                         // +0x21C = +0x1FC
    fadeColor20C = {r, g, b, a};                      // +0x20C
    fadeColor22C = {r, g, b, 0.0f};                   // +0x22C (+0x238 = EAX = 0, not a)
}

// ---- 2.2 the update -----------------------------------------------------------------------------------------------
void Camera::Update(f32 dt) {                         // orig: 0x00019C80
    ++updates;
    ++modeFrames;
    switch (mode) {
    case 0: FollowMode(dt); break;
    case 1: break;                                    // static
    case 2: StaticTargetMode(); break;                // 0x00012460
    case 6: PathToStaticTargetMode(dt); break;        // 0x00018C00
    case 8: FixedVectorMode(dt); break;               // 0x00014B00
    case 9: RailMode(); break;                        // 0x000125B0
    case 7: SideMode(); break;                        // 0x00014700
    case 5: PathToChaseMode(dt); break;               // 0x00019180
    case 3: FirstPersonMode(); break;                 // 0x00018250
    default: NotPorted("Camera::Update: mode 4 (0x00015110)"); break;   // (and any mode above 9)
    }
    if (flush) {
        NotPorted("Camera::Update: the flush snap (0x000111B0)");
        flush = 0;
    }
    Shake(dt);                                        // orig: 0x00012E40
    UpdateFade(g_frameStep);                          // orig: 0x000198E0 (DAT_002D67E8: also in the ctor's Update)
    // NOT PORTED: the wobble 0x000131E0 (off: +0x1F0 = 0).
}

// ---- mode 3: first person ------------------------------------------------------------------------------------------
void Camera::EnterFirstPerson(Character* c) {         // orig: 0x00018050
    PushTransform();                                  // 0x000132F0 (the guard DAT_002D94E1 is clear: see LeaveModeCheck)
    prevMode = mode;                                  // +0x160 = +0x15C
    RestoreTargetModel();
    if (c) {
        const NiPoint3 p = c->GetWorldPosition();     // vf34
        SetPosition({p.x, F(double(p.y) + c->cdef.fpsHeight), p.z});   // FLD; FADD D+0xA0; +0x48, +0xA1 = 1
        rotation = YawFrame(c->GetWorldRotation());   // vf38's column 0 -> +0x70 (12 dwords)
        dirtyRotate = 1;                              // +0xA3
        target = c;                                   // +0x44
        if (ni::NiAVObject* m = c->model.Get()) {     // +0x564 (no null test in the original)
            modelHidden = m->flags & ni::kNiAVHidden; // +0x1C0 = +0xB0 & 1
            m->flags |= ni::kNiAVHidden;
        }
    }
    fpPitch = fpYaw = 0;                              // +0x190 / +0x194
    mode = 3;
    modeFrames = 0;                                   // +0x198
}

// orig: 0x00018250 (thiscall(dt), RET 4; dt not read). The player S+0x94 gives the stick, the target +0x44 (not
// null-tested) the eye.
void Camera::FirstPersonMode() {
    Player* P = level.player;
    if (!P) return;
    f32 a = 0, b = 0, c = 0;                          // esp+0x18 / +0x1C / +0x2C
    (void)ni::ToEulerAnglesYZX(rotation, a, b, c);    // 0x001037B0 on a copy of +0x70 (c not read)
    if (target) {
        const NiPoint3 p = target->GetWorldPosition();   // vf34
        position = {p.x, F(double(p.y) + target->cdef.fpsHeight), p.z};   // FLD +4; FADD D+0xA0; FSTP
        dirtyTranslate = 1;
    }
    // the dead zone: the smaller of the two stick values is dropped when at most 0.3 of the larger (0x00293B18)
    double v = P->lookY;                              // FLD P+0xB28 (kept)
    f32 h = P->lookX;                                 // P+0xB2C -> esp+0x10
    const double av = std::fabs(v), ah = std::fabs(double(h));
    if (av > ah) {                                    // FCOMP ST(1); TEST AH,0x41; JNE (NaN: the other side)
        if (ah <= av * double(Bits(0x3E99999A))) h = 0;   // FCOMPP; TEST AH,0x41; JP (NaN keeps it)
    } else {
        if (av <= ah * double(Bits(0x3E99999A))) v = 0;   // FLD [0x00293AB8] (NaN keeps it)
    }
    // the smoothed values (process statics): 0.75 old + 0.04 new ([0x00293A94], [0x00293C38]), then clamped to
    // +-1.5 |new| ([0x00293BEC] / [0x00293BE8], the comparisons on the stored statics)
    g_fpLookV = F(double(g_fpLookV) * 0.75f + v * double(Bits(0x3D23D70A)));
    g_fpLookH = F(double(g_fpLookH) * 0.75f + double(h) * double(Bits(0x3D23D70A)));
    {
        const f32 m = F(std::fabs(v));                // FABS; FST esp+0x14
        const double hi = std::fabs(v) * 1.5f;        // (the register)
        if (g_fpLookV > hi) {                         // FCOMP; TEST AH,0x41; JNE
            g_fpLookV = F(hi);
        } else {
            const double lo = double(m) * -1.5f;
            if (g_fpLookV < lo) g_fpLookV = F(lo);    // TEST AH,5; JP
        }
    }
    {
        const f32 m = F(std::fabs(double(h)));
        const double hi = std::fabs(double(h)) * 1.5f;
        if (g_fpLookH > hi) {
            g_fpLookH = F(hi);
        } else {
            const double lo = double(m) * -1.5f;
            if (g_fpLookH < lo) g_fpLookH = F(lo);
        }
    }
    // the camera's own smoothing: half the old + an eighth of the static ([0x00293AA0], [0x00293C34]); the sums
    // with the current angles use the unrounded values
    const double p190 = double(fpPitch) * 0.5f + double(g_fpLookV) * 0.125f;
    fpPitch = F(p190);                                // FST +0x190 (kept)
    const double p194 = double(g_fpLookH) * 0.125f + double(fpYaw) * 0.5f;
    fpYaw = F(p194);                                  // FST +0x194 (kept)
    const f32 yaw = F(p194 + a);                      // FADD esp+0x18; FSTP esp+0x14
    const double pu = p190 + b;                       // FADD esp+0x1C
    f32 pitch = F(pu);                                // FST esp+0x10
    if (pu > 1.5f) {                                  // FCOMP [0x00293BEC]; TEST AH,0x41; JNE (the unrounded sum)
        pitch = 1.5f;                                 // 0x3FC00000
    } else if (pitch < -1.5f) {                       // FLD esp+0x10; FCOMP [0x00293BE8]; TEST AH,5; JP
        pitch = -1.5f;                                // 0xBFC00000
    }
    rotation = ni::Multiply(ni::MakeYRotation(yaw), ni::MakeZRotation(pitch));   // 0x001030B0, 0x001030F0, 0x00013B30
    dirtyRotate = 1;                                  // +0xA3
}

// ---- the shake (camera_modes_bender_port.md 4.1) ----------------------------------------------------------------
void Camera::Shake(f32 dt) {                          // orig: 0x00012E40 (thiscall(f32 dt), RET 4)
    if (!shaking) {                                   // +0x14
        shakeOffset = {};                             // three dword zeros through the stack
        return;
    }
    shakeOffset.x = 0.0f;                             // +0x20 (dword 0)
    // rand() & 0x80001FFF with the sign fix-up = rand() % 8192 (rand() >= 0). FILD; FMUL [0x00293B24] (2^-13);
    // FMUL +0x1C; FADD ST,ST; FSUB +0x1C; FSTP: every product is exact in double (r < 2^13, amp a float).
    const s32 r1 = crt::Rand() % 8192;                // 0x00012E94
    shakeOffset.y = F((double(r1) * double(Bits(0x39000000)) * shakeAmp) * 2.0 - shakeAmp);   // +0x24
    const s32 r2 = crt::Rand() % 8192;                // 0x00012EC0
    shakeOffset.z = F((double(r2) * double(Bits(0x39000000)) * shakeAmp) * 2.0 - shakeAmp);   // +0x28
    shakeOffset = TransposeMul(rotation, shakeOffset);   // orig: 0x00103290(&tmp, &(+0x20), copy of +0x70)
    u32 tb;
    std::memcpy(&tb, &shakeTime, 4);
    if (tb != 0xC23C0000u) {                          // dword compare with -47.0f: forever
        const double t = double(shakeTime) - dt;      // FLD +0x18; FSUB dt; FST +0x18
        shakeTime = F(t);
        if (t < 0.0) shaking = 0;                     // FCOMP [0x00293AB8]; TEST AH,5; JP (the unrounded value)
    }
    f32 a = shakeAmp;
    if (a > 1.0f) a = 1.0f;                           // FCOMP [0x00293A98]; TEST AH,0x41; JNE
    // orig: (S+0x94 + 0xA04)->0x0005F000(a, a, DAT_002D67E8, 0): the player's rumble (P is not null-checked).
    if (level.player) level.player->rumble.Add(a, a, g_frameStep, 0);
    else NotPorted("Camera::Shake without a player (the original dereferences S+0x94)");
}

// ---- modes 2 and 6 (level2-2_port.md 4) ----------------------------------------------------------------------------
void Camera::StaticTargetMode() {                     // orig: 0x00012460 (thiscall(f32 dt), RET 4; dt not read)
    if (!tracked) return;                             // +0x40
    const double dx = double(goal.x) - position.x;    // FLD +0x60; FSUB (kept)
    const double dy = double(goal.y) - position.y;    // (kept)
    const f32 dz = F(double(goal.z) - position.z);    // FSTP [esp+0x20]
    const f32 k = F(1.0 - double(easing));            // FLD [0x00293A98]; FSUB +0x18C; FSTP
    dirtyTranslate = 1;                               // +0xA1
    const f32 ax = F(dx * k);                         // FXCH; FMUL; FSTP
    const double ay = dy * k, az = double(dz) * k;
    position = {F(double(position.x) + ax), F(double(position.y) + ay), F(double(position.z) + az)};
    const NiPoint3 p = level.triggers->Position(*tracked);   // 0x000DA7E0
    const double lx = double(p.x) - lookAt.x, ly = double(p.y) - lookAt.y;
    const f32 lz = F(double(p.z) - lookAt.z);
    const f32 k2 = F(1.0 - double(easing));
    const f32 bx = F(lx * k2);
    lookAt = {F(double(bx) + lookAt.x), F(ly * k2 + lookAt.y), F(double(lz) * k2 + lookAt.z)};
    dirtyLookAt = 1;                                  // +0xA2
}

void Camera::PathToStaticTargetMode(f32 dt) {         // orig: 0x00018C00 (thiscall(f32 dt), RET 4)
    const double t = double(dt) + pathTime;           // FLD dt; FADD +0x10C
    pathTime = F(t);                                  // FST
    if (t >= pathDuration) pathTime = pathDuration;   // FCOMP +0x110 (unrounded sum); TEST AH,1; JNE (NaN: kept)
    const f32 u = F(double(pathTime) / pathDuration); // FLD +0x10C; FDIV +0x110; FSTP (the float argument)
    position = QuadraticPath(u, pathStart, pathControl, pathEnd);   // 0x00011B50
    dirtyTranslate = 1;                               // +0xA1
    if (!tracked) return;                             // (the original reads the record without a null check)
    const NiPoint3 p = level.triggers->Position(*tracked);   // 0x000DA7E0
    // (0x00018C8D..0x00018CF8 first blends +0x54 towards p by F(1 - preset170 (+0x170)): a dead store, the lines below
    //  overwrite all of +0x54 from +0x124)
    const double mx = double(p.x) - pathLook.x, my = double(p.y) - pathLook.y;
    const f32 mz = F(double(p.z) - pathLook.z);
    const f32 w = F(1.0 - (double(pathDuration) - pathTime) / pathDuration);   // FLD; FSUB; FDIV; FSUBR 1.0; FSTP
    const f32 bx = F(mx * w);
    lookAt = {F(double(bx) + pathLook.x), F(my * w + pathLook.y), F(double(mz) * w + pathLook.z)};
    dirtyLookAt = 1;                                  // +0xA2
    if (pathTime >= pathDuration) {                   // FLD +0x10C; FCOMP +0x110; TEST AH,1; JNE (NaN: no)
        const NiPoint3 at = position;
        SetStaticTarget(&at, tracked, 0.0f);          // 0x00014550: mode 2, modeFrames 0
    }
}

// ---- 2.4 PlaceBehindTarget ------------------------------------------------------------------------------------------
void Camera::PlaceBehindTarget() {                    // orig: 0x00015350
    Character* T = target;
    Player* P = level.player;
    if (!T || !P) return;
    const bool rodent = P->type == 4;
    dirtyLookAt = 1;
    const NiPoint3 wp = T->GetWorldPosition();
    const NiPoint3 piv{wp.x, F(double(pivotHeight) + wp.y), wp.z};
    lookAt = piv;
    height = F(double(lookAt.y) + presetHeight);
    f32 best = 0.0f, bestDist = 0.0f;
    NiPoint3 bestDir{wp.x, piv.y, wp.z};
    const f32 D = distance;
    if (rodent) {
        const NiPoint3 f = ni::MulVector(T->rotation, kUnitX);
        bestDist = 1.0f;
        bestDir = {F(-double(f.x) * D), presetHeight, F(-double(f.z) * D)};
    } else {
        const ni::NiMatrix3& R = T->rotation;        // orig: vf38 (copy of +0x570)
        const NiPoint3 c0 = ni::MulVector(R, kUnitX), c2 = ni::MulVector(R, kUnitZ);
        const NiPoint3 dir[4] = {c0, c2, {F(-double(c0.x)), F(-double(c0.y)), F(-double(c0.z))},
                                 {F(-double(c2.x)), F(-double(c2.y)), F(-double(c2.z))}};
        static const u32 kWeights[8] = {0x3F800000, 0x3F19999A, 0x3F000000, 0x3ECCCCCD,
                                        0x3ECCCCCD, 0x3ECCCCCD, 0x3F000000, 0x3F19999A};
        for (int i = 0; i < 8; ++i) {
            const int j = i >> 1;
            const f32 w = Bits(kWeights[i]);
            NiPoint3 v = dir[j];
            if (i & 1) {
                const NiPoint3& a = dir[(j + 1) & 3];
                v = {F(double(a.x) + v.x), F(double(a.y) + v.y), F(double(a.z) + v.z)};
                ni::Unitize(v);
            }
            NiPoint3 c{F(-double(v.x) * D), presetHeight, F(-double(v.z) * D)};
            const double len = ni::Unitize(c);
            f32 t1 = F(len);
            const f32 lenF = F(len);
            RayCastMasked(piv, c, t1, 7);
            if (!(double(t1) * w > double(best))) continue;   // TEST AH,0x41; JNE
            f32 t2 = lenF;
            // QUIRK (0x00015718): the reverse ray starts one unit out (the unit c), not at the full length.
            const bool hit2 = RayCastMasked({F(double(c.x) + piv.x), F(double(c.y) + piv.y), F(double(c.z) + piv.z)},
                                            {F(-double(c.x)), F(-double(c.y)), F(-double(c.z))}, t2, 7);
            if (hit2 && double(lenF) - t2 < double(t1)) t1 = F(double(lenF) - t2);
            if (!(double(t1) * w > double(best))) continue;
            f32 s = 0.0f;
            if (0.0f < t1) {
                do {
                    NiPoint3 p{F(double(c.x) * s + piv.x), F(double(s) * c.y + piv.y),
                               F(double(F(double(c.z) * s)) + piv.z)};
                    if (PushOut(p)) {
                        if (s < t1) t1 = s;
                        break;
                    }
                    s = F(double(radius) * 0.25f + s);
                } while (s < t1);
            }
            const double v2 = double(t1) * w;
            if (v2 > double(best)) {
                best = F(v2);
                bestDir = c;
                bestDist = t1;
                if (hit2) bestDist = F(double(t1) - 0.1f);
            }
        }
    }
    NiPoint3 pos{F(double(bestDir.x) * bestDist + piv.x), F(double(bestDir.y) * bestDist + piv.y),
                 F(double(F(double(bestDir.z) * bestDist)) + piv.z)};
    pitchOffset = yawVelocity = 0;
    if (!rodent) PushOut(pos);
    position = pos;
    dirtyTranslate = 1;
    if (!follow.keep) follow = Follow{};
}

// ---- 2.5 collision helpers ----------------------------------------------------------------------------------------
bool Camera::PushOut(NiPoint3& p) {                   // orig: 0x000129C0
    const f32 r = radius;
    const f32 R2 = F(double(r) + (follow.magnet ? 1.0f : 2.0f));
    const f32 inv = F(1.0 / (double(R2) - r));
    const std::vector<phys::TriRef> list = phys::QuerySphere(p, R2, 7);
    if (list.empty()) return false;
    NiPoint3 acc = p;
    bool pushed = false;
    for (const phys::TriRef& tr : list) {
        if (tr.Flags() & 0x20) continue;              // (stage and mesh scratch triangles alike)
        const NiPoint3 n = tr.Normal();
        const NiPoint3 tri[3] = {tr.V(0), tr.V(1), tr.V(2)};
        const double ax = double(p.x) - tri[0].x, ay = double(p.y) - tri[0].y, az = double(p.z) - tri[0].z;
        const double dist = (double(n.z) * az + double(n.y) * ay) + double(n.x) * ax;
        NiPoint3 proj{F(double(p.x) - double(n.x) * dist), F(double(p.y) - double(F(double(n.y) * dist))),
                      F(double(p.z) - double(F(double(n.z) * dist)))};
        ClosestPointOnTriangle(proj, tri);
        NiPoint3 dlt = Sub(p, proj);
        const double len = ni::Unitize(dlt);
        if (len < double(r)) {
            const double k = (double(r) - len) + double(1e-4f);
            pushed = true;
            acc = {F(double(dlt.x) * k + acc.x), F(double(acc.y) + double(F(double(dlt.y) * k))),
                   F(double(acc.z) + double(F(double(dlt.z) * k)))};
        } else if (len < double(R2)) {
            const double k = (((double(R2) - len) + double(1e-4f)) * inv) * 0.3f;
            acc = {F(double(dlt.x) * k + acc.x), F(double(F(double(dlt.y) * k)) + acc.y),
                   F(double(F(double(dlt.z) * k)) + acc.z)};
        }
    }
    p = acc;
    return pushed;
}

s32 Camera::Magnets(NiPoint3& des) {                  // orig: 0x000136C0
    follow.magnet = 0;
    if (!level.triggers || !target) return 0;
    const bool far = follow.occluded >= 0.25f;
    const NiPoint3 wp = target->GetWorldPosition();
    const NiPoint3 tp{wp.x, F(double(pivotHeight) + wp.y), wp.z};
    NiPoint3 a = Sub(tp, des);
    f32 maxD2 = far ? F(double(distance) * distance)
                    : F((double(a.z) * a.z + double(a.y) * a.y) + double(a.x) * a.x);
    f32 aLen = 0.0f;                                  // [esp+0x14]: |a| once unitized (0: not yet)
    s32 count = 0;
    for (TriggerRec* rec = level.triggers->kindHeads[3]; rec; rec = rec->kindNext) {   // NextOfKind(3) 0x000DA640
        if (!rec->target || !rec->magnet) continue;
        const NiPoint3 pos = level.triggers->Position(*rec);   // orig: 0x000DA7E0
        NiPoint3 b = Sub(tp, pos);
        // TEST AH,0x41; JNE: skipped when <= 0 or unordered
        if (!far && !((double(b.z) * a.z + double(b.y) * a.y) + double(b.x) * a.x > 0.0)) continue;
        const f32 d2 = F((double(b.z) * b.z + double(b.y) * b.y) + double(b.x) * b.x);
        if (!(maxD2 > d2)) continue;                  // FLD maxD2; FCOMP d2; TEST AH,0x41; JNE
        if (!far) {
            if (aLen == 0.0f) aLen = F(ni::Unitize(a));   // FCOMP 0.0; TEST AH,0x44; JP (NaN: not again)
            ni::Unitize(b);
            // TEST AH,5; JNP: skipped only when < 0.65 (unordered is accepted)
            if ((double(b.z) * a.z + double(b.y) * a.y) + double(b.x) * a.x < 0.65f) continue;
        }
        maxD2 = d2;
        des = pos;
        ++count;
        follow.magnet = 1;
    }
    return count;                                     // (the tail's pull has the factor 0.0: no effect)
}

// ---- 2.6 the follow mode ------------------------------------------------------------------------------------------
void Camera::FollowMode(f32 dt) {                     // orig: 0x00015990
    Character* T = target;
    if (!T || T->IsDead()) return;
    Player* P = level.player;
    if (!P) return;
    Follow& H = follow;
    // 2.6.1 flags and timers
    const bool rodent = P->type == 4;
    const bool fA = P->JumpingOrFalling();            // vf130 0x000500F0
    bool fB = false;                                  // orig: vf9C 0x0004FA80 (0x00035AE0: walking / running)
    for (s32 k : {21, 13, 17, 14, 15, 16, 22, 23, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x87, 0x88, 0x89, 0x8A, 0x92,
                  0x93, 0x94, 0x95, 0x96, 0x97})
        fB = fB || P->IsAnimPlaying(k);
    const bool fAir = !P->OnGround();
    const bool fSt = P->OnLadder() || P->OnLedge();
    const bool fLock = P->lockOn != 0;
    const bool fKeepY = P->IsDeathFall();             // vf134 0x000500C0
    const bool fStick = std::fabs(P->orbitX) > 0.0f || std::fabs(P->orbitY) > 0.0f;
    const bool f629 = P->splashed != 0;
    const bool stickRecentPrev = H.off[5] < 0.1f;
    auto timer = [&](int i, bool flag) {
        if (flag) {
            H.off[i] = 0;
            H.on[i] = F(double(dt) + H.on[i]);
        } else {
            H.on[i] = 0;
            H.off[i] = F(double(dt) + H.off[i]);
        }
    };
    timer(0, fA);
    timer(1, fB);
    timer(2, fAir);
    timer(3, fLock);
    timer(4, fSt);
    timer(5, fStick);
    timer(7, f629);
    timer(6, H.off[5] > 1.0f && (P->IsAnimPlaying(0x61) || P->IsAnimPlaying(0x5E) || P->IsAnimPlaying(0x5F) ||
                                 P->IsAnimPlaying(0x60)));
    const bool recentAct = H.off[6] < 1.0f;
    const bool recent629 = !rodent && H.off[7] < 0.25f;
    const bool hold = H.off[5] < 0.1f || fA || recentAct || recent629 || fSt;

    // 2.6.2 pivot, look point, distance
    NiPoint3 tp = T->GetWorldPosition();
    tp.y = F(double(pivotHeight) + tp.y);
    const NiPoint3 oldTgt = lookAt;
    const NiPoint3 fwd = ni::MulVector(T->rotation, kUnitX);
    const double k = Bits(0x3D4CCCD0);                // 0.050000012
    H.facing.x = F(double(F((double(fwd.x) - H.facing.x) * k)) + H.facing.x);
    H.facing.y = F((double(fwd.y) - H.facing.y) * k + H.facing.y);
    H.facing.z = F(double(F(double(fwd.z) - H.facing.z)) * k + H.facing.z);
    NiPoint3 L{F(double(H.facing.x) * 4.375f + tp.x), F(double(H.facing.y) * 4.375f + tp.y),
               F(double(F(double(H.facing.z) * 4.375f)) + tp.z)};
    if (fA || recent629) L.y = oldTgt.y;
    height = F((double(presetHeight) + pitchOffset) + L.y);
    const NiPoint3 cp = position;
    const NiPoint3 e{F(double(L.x) - cp.x), 0.0f, F(double(L.z) - cp.z)};
    NiPoint3 d = e;
    const double len = ni::Unitize(d);
    const f32 lenF = F(len);
    f32 D = distance;

    // 2.6.3 far snap, occlusion
    if (!rodent) {
        if (!hold && double(distance) + distance < double(lenF)) {   // TEST AH,5; JP
            PlaceBehindTarget();
            return;
        }
        if (stickRecentPrev && !hold) {
            H.occluded = 0;
            H.recheck = 0;
        }
        if (H.occluded > 0.0f) H.occluded = F(double(dt) + H.occluded);
        if (H.recheck > 0.0f) {
            H.recheck = F(double(H.recheck) - dt);
        } else {
            H.recheck = 0.1f;
            f32 t = 1.0f;
            const bool hit = RayCastMasked(cp, Sub(tp, cp), t, 7);
            H.occluded = hit ? F(double(dt) + H.occluded) : 0.0f;
        }
    }
    f32 blend = 1.0f;
    if (!rodent) {
        double v = double(cp.y) - (double(L.y) + presetHeight);
        if (v > 10.0f) v = 10.0f;
        if (v > 0.0) {
            const double Dp = double(D) - v * 1.75f;
            D = F(Dp);
            if (Dp < 5.0f) D = 5.0f;
        }
    }

    // 2.6.4 desired position, magnets, snaps
    NiPoint3 des{F(double(L.x) - double(d.x) * D), 0.0f, F(double(L.z) - double(d.z) * D)};
    if (fLock) {                                      // P vf14C (+0xB00): face the lock point
        const NiPoint3 lp = P->aimPoint;              // 0x00011A60 (P+0xB04)
        NiPoint3 q{F(double(lp.x) - L.x), 0.0f, F(double(lp.z) - L.z)};
        (void)ni::Unitize(q);                         // FSTP ST0
        const f32 qz = F(double(q.z) * D);            // FSTP [esp+0x40]
        des = {F(double(L.x) - double(q.x) * D), F(double(L.y) - double(q.y) * D), F(double(L.z) - qz)};
    }
    if ((recentAct || fA) && !rodent) {
        des.x = F(double(L.x) - (double(oldTgt.x) - cp.x));
        des.z = F(double(L.z) - double(F(double(oldTgt.z) - cp.z)));
    }
    des.y = height;
    if (!hold || H.on[1] > 0.0f) {
        if (Magnets(des) != 0) {
            blend = 0.5f;
            D = F(double(distance) * 0.5f);
        }
    }
    if (!rodent) {
        auto snap = [&] {
            const f32 saved = H.occluded;
            H.recheck = 0;
            PlaceBehindTarget();
            H.occluded = saved;
        };
        if (hold) {
            if (H.occluded > 5.0f) return snap();
        } else {
            if (H.occluded > 2.0f) return snap();
            if (H.occluded > 0.0f && H.on[2] > 0.25f) return snap();
            if (H.occluded > 0.0f && !fA && !recentAct &&
                (double(fwd.y) * d.y + double(d.x) * fwd.x) + double(d.z) * fwd.z > -0.25f) {
                H.recheck = 0;
                H.swingTime = 2.0f;
                H.swingDir = fwd;
                recentre = 0;
            }
        }
        if (fLock) LockSlide(des, D, tp, radius);     // 2.6.5 (0x000163F9: P vf14C again)
    }

    // 2.6.6 recentre, swing, yaw target
    f32 cy = 0.0f;
    bool rot = false;
    if (recentre) {
        if (H.occluded > 0.0f) {
            PlaceBehindTarget();
            recentre = 0;
            return;
        }
        H.on[1] = 0;
        H.swingTime = 2.0f;
        H.swingDir = fwd;
        recentre = 0;
    }
    if (H.swingTime > 0.0f) {
        const NiPoint3 s = H.swingDir;
        if ((double(d.z) * s.z + double(d.y) * s.y) + double(d.x) * s.x >= 0.98f) {
            H.swingTime = 0;
        } else {
            auto scale = [&](f32 m) { return NiPoint3{F(double(s.x) * m), F(double(s.y) * m), F(double(s.z) * m)}; };
            NiPoint3 sv = scale(D);
            NiPoint3 X{F(double(tp.x) - sv.x), 0.0f, F(double(tp.z) - sv.z)};
            f32 t = 1.0f;
            if (RayCastMasked(tp, {F(double(X.x) - tp.x), F(double(height) - tp.y), F(double(X.z) - tp.z)}, t, 7)) {
                D = F(double(t) * D);
                sv = scale(D);
                X = {F(double(L.x) - sv.x), 0.0f, F(double(L.z) - sv.z)};
                D = F(double(D) - radius);
            }
            if (H.occluded >= 0.1f) {
                H.swingTime = 0;
                cy = 0;
                des = cp;
            } else {
                H.swingTime = H.on[1] > 0.5f ? 0.0f : F(double(H.swingTime) - dt);
                const f32 c = CrossY({F(double(X.x) - cp.x), 0.0f, F(double(X.z) - cp.z)},
                                     {F(double(tp.x) - cp.x), 0.0f, F(double(tp.z) - cp.z)});
                cy = c < 0.0f ? F(double(c) - 120.0f) : F(double(c) + 120.0f);
                rot = true;
            }
        }
    } else if (fLock) {                               // 2.6.7 LockFollow (0x000170F2: P vf14C, no swing)
        // behind the look point along the target's facing, the hit distance (t - r, the register) or D
        f32 t = F(double(D) + radius);                // FADD +0x158; FSTP [esp+0x88]
        const bool hit = RayCastMasked(L, {F(-double(fwd.x)), F(-double(fwd.y)), F(-double(fwd.z))}, t, 7);   // 0x00011000
        const double Dh = hit ? double(t) - radius : double(D);
        const f32 pz = F(double(fwd.z) * Dh);
        const NiPoint3 p{F(double(L.x) - double(fwd.x) * Dh), F(double(L.y) - double(fwd.y) * Dh), F(double(L.z) - pz)};
        const double kf = Bits(0x3DCCCCD0);           // [0x00293C0C] 0.10000002
        const f32 hy = F(double(height) - cp.y);      // +0xD0
        const f32 dz = F(double(p.z) - cp.z);
        const f32 qz = F(double(dz) * kf);
        NiPoint3 res{F(double(cp.x) + (double(p.x) - cp.x) * kf), F(double(hy) * kf + cp.y), F(double(cp.z) + qz)};
        NiPoint3 m = Sub(res, cp);
        const f32 lenM = F(ni::Unitize(m));           // FSTP [esp+0xA8]
        f32 s = 0.0f;                                 // esp+0x54
        if (0.0f < lenM) {                            // FLD 0; FCOMP; TEST AH,5; JP
            for (;;) {                                // the first point along m the push-out moves
                const f32 sz = F(double(s) * m.z);
                NiPoint3 probe{F(double(cp.x) + double(s) * m.x), F(double(s) * m.y + cp.y), F(double(cp.z) + sz)};
                if (PushOut(probe)) {                 // 0x000129C0
                    res = probe;
                    break;
                }
                const double x = double(radius) * 0.25f + s;   // FMUL [0x00293B50]; FADD; FST
                s = F(x);
                if (!(x < lenM)) break;               // FCOMP; TEST AH,5; JNP (loop)
            }
            if (s == 0.0f) PushOut(res);              // TEST AH,0x44; JP
        } else {
            PushOut(res);
        }
        position = res;                               // +0x48
        dirtyTranslate = 1;                           // +0xA1
        const NiPoint3 old = lookAt;                  // +0x54
        const f32 hx = F((double(L.x) - old.x) * 0.5f);
        const f32 lz = F(double(L.z) - old.z);
        lookAt = {F(double(hx) + old.x), F((double(L.y) - old.y) * 0.5f + old.y), F(double(lz) * 0.5f + old.z)};
        pivotCopy = lookAt;                           // +0x04
        dirtyLookAt = 1;                              // +0xA2
        return;
    } else {
        H.swingTime = 0;
        cy = CrossY({F(double(des.x) - cp.x), 0.0f, F(double(des.z) - cp.z)}, e);
        if ((!recentAct && !fA) || rodent)
            cy = F(double(cy) + double(CrossY({F(double(oldTgt.x) - L.x), 0.0f, F(double(oldTgt.z) - L.z)}, e)) * 0.25f);
    }

    // 2.6.8 stick input, turn damping
    NiPoint3 g{e.x, 0.0f, F(double(L.z) - cp.z)};
    ni::Unitize(g);
    double dotG = (double(fwd.x) * g.x + double(g.y) * fwd.y) + double(g.z) * fwd.z;
    if (rot && dotG < 0.0) dotG = 0.0;
    f32 turn = F(1.0f - std::sqrt(std::fabs(dotG)) * 0.1f);
    f32 yawIn = 0.0f, pitchIn = 0.0f;
    if (!rodent) {
        if (std::fabs(P->orbitY) < std::fabs(P->orbitX)) {   // FCOMPP; TEST AH,5; JP
            double v = P->orbitX;                     // (the invert option DAT_002D94E0: off)
            if (std::fabs(v) < 0.9f) v = v * 0.25f;
            v = v * yawGain;
            yawIn = F(v);
            if (std::fabs(v) < 0.01f) yawIn = 0.0f;
            turn = 1.0f;
        } else {
            const double v = (double(pitchGain) * P->orbitY) * 8.0f;
            pitchIn = F(v);
            if (std::fabs(v) < 0.01f) pitchIn = 0.0f;
        }
    }

    // 2.6.9 integration in 0.01 s sub-steps (the look-point accumulator stays on the x87 stack)
    NiPoint3 R{};
    double acc[3] = {0.0, 0.0, 0.0};
    NiPoint3 tgtD{};
    double rem = dt;
    const f32 cyAbs = F(std::fabs(double(cy)));
    if (dt > 0.0f) {
        do {
            const f32 ex = F(double(L.x) - (double(oldTgt.x) + acc[0]));
            const f32 ey = F(double(L.y) - (acc[1] + oldTgt.y));
            const f32 ez = F(double(L.z) - double(F(acc[2] + oldTgt.z)));
            if (!rodent) {
                acc[0] += F((double(ex) * 5.0f) * 0.01f);
                acc[1] += (double(ey) * 5.0f) * 0.01f;
                acc[2] += F(double(F(double(ez) * 5.0f)) * 0.01f);
            } else {
                acc[0] += F((double(ex) * 0.01f) * 10.0f);
                acc[1] += (double(ey) * 0.01f) * 10.0f;
                acc[2] += F(double(F(double(ez) * 0.01f)) * 10.0f);
            }
            const double dz = double(D) - (double(R.z) + lenF);
            const double dy = double(des.y) - (double(R.y) + cp.y);
            const f32 yv = F((double(yawIn) + yawVelocity) * 0.91f);
            yawVelocity = yv;
            if (double(yv) > double(yawGain) + yawGain) yawVelocity = F(double(yawGain) + yawGain);
            else if (double(yv) < double(yawGain) * -2.0f) yawVelocity = F(double(yawGain) * -2.0f);
            const f32 yr = std::fabs(yawVelocity) > cyAbs ? yawVelocity : cy;
            H.yawRate = F(double(yr) * 0.001f + H.yawRate);
            H.yawRate = F((double(turn) * H.yawRate) * 0.97f);
            H.distRate = F((dz * blend) * 5.0f);
            H.heightRate = F(dy + dy);
            pitchOffset = pitchIn != 0.0f ? F(double(pitchIn) * 0.01f + pitchOffset) : F(double(pitchOffset) * 0.9f);
            R.x = F(double(R.x) + double(H.yawRate) * 0.01f);
            R.y = F(double(R.y) + double(H.heightRate) * 0.01f);
            R.z = F(double(R.z) + double(F(double(H.distRate) * 0.01f)));
            rem = rem - 0.01f;
        } while (rem > 0.0);
        tgtD = {F(acc[0]), F(acc[1]), F(acc[2])};
    }
    if (!rodent) {
        if (R.x > 0.1f) R.x = 0.1f;
        else if (R.x < -0.1f) R.x = -0.1f;
        if (R.y > 1.0f) R.y = 1.0f;
        else if (R.y < -1.0f) R.y = -1.0f;
        if (R.z > 1.5f) R.z = 1.5f;
        else if (R.z < -1.5f) R.z = -1.5f;
    }

    // 2.6.10 new position, collision, outputs
    NiPoint3 b{F(double(cp.x) - L.x), 0.0f, F(double(cp.z) - L.z)};
    if (ni::Unitize(b) <= 1.0) b = kUnitX;
    const NiPoint3 r = ni::MulVector(ni::MakeYRotation(-R.x), b);
    const f32 dist = F(double(R.z) + lenF);
    NiPoint3 newPos{F(double(L.x) + double(dist) * r.x), F(double(R.y) + cp.y), F(double(L.z) + double(dist) * r.z)};
    if (!rodent) {
        NiPoint3 m = Sub(newPos, cp);
        f32 t = 1.0f;
        if (RayCastMasked(cp, m, t, 7)) {
            const double kk = ni::Unitize(m) * t - 0.001f;
            newPos = {F(double(cp.x) + double(m.x) * kk), F(double(F(double(m.y) * kk)) + cp.y),
                      F(double(cp.z) + double(F(double(m.z) * kk)))};
        }
        PushOut(newPos);
        const NiPoint3 w = Sub(newPos, tp);
        f32 t2 = 1.0f;
        if (RayCastMasked(tp, w, t2, 5)) {
            const NiPoint3 hp{F(double(tp.x) + double(F((double(t2) * w.x) * 0.9f))),
                              F((double(t2) * w.y) * 0.9f + tp.y),
                              F(double(F(double(t2) * w.z)) * 0.9f + tp.z)};
            const NiPoint3 dh = Sub(hp, newPos);
            if (!hold && (double(dh.x) * dh.x + double(dh.y) * dh.y) + double(dh.z) * dh.z < 16.0f) {
                newPos = hp;
                if (PushOut(newPos)) H.swingTime = 0;
            } else {
                f32 t3 = 1.0f;
                if (!RayCastMasked(newPos, {F(-double(w.x)), F(-double(w.y)), F(-double(w.z))}, t3, 5)) {
                    newPos = hp;
                    if (PushOut(newPos)) H.swingTime = 0;
                }
            }
        }
    }
    if (fKeepY) newPos.y = cp.y;
    position = newPos;
    dirtyTranslate = 1;
    if (pitchOffset > 10.0f) pitchOffset = 10.0f;
    else if (pitchOffset < -10.0f) pitchOffset = -10.0f;
    lookAt = {F(double(tgtD.x) + lookAt.x), F(double(tgtD.y) + lookAt.y), F(double(tgtD.z) + lookAt.z)};
    pivotCopy = lookAt;
    dirtyLookAt = 1;
}

// ---- 2.10 the apply step ------------------------------------------------------------------------------------------
void Camera::SetFogIntensity(f32 v) {                 // orig: 0x00012C70
    if (v < 0.0f) v = 0.0f;                           // FCOMP; TEST AH,5; JP (-0.0 / NaN unchanged)
    fogDepth = std::isnan(v) ? 0.0f : v;              // FCOMP; TEST AH,1; JNE (only a NaN is still "< 0")
    if (fogProperty) fogProperty->depth = fogDepth;   // (+0x1C8)+0x18 (no null test in the original)
}

void Camera::SetFogColor(f32 r, f32 g, f32 b) {       // orig: 0x00012CC0 (raw copies; also the fog property's)
    fogColor = {r, g, b};
    if (fogProperty) fogProperty->color = {r, g, b};  // +0x1C..+0x24
    if (fogEnabled && level.renderer) level.renderer->SetBackgroundColor({r, g, b});   // App->vf14()->vf6C(+0x1CC)
}

void Camera::SetFogDistance(f32 d) {                  // orig: 0x00011230
    if (d > 1.0f) fogDistance = d;                    // FCOMP; TEST AH,0x41; JNE
    if (fogEnabled && fogDistance > 1.0f) {
        farPlane = fogDistance;                       // +0xB4: the far plane of the next Apply (a culling input)
        dirtyFrustum = 1;
    }
}

void Camera::EnableFog() {                            // orig: 0x00012D20
    if (fogEnabled) return;
    defaultFar = farPlane;                            // +0x1DC = +0xB4 (before the change)
    if (fogDistance > 1.0f) {
        farPlane = fogDistance;
        dirtyFrustum = 1;
    }
    if (fogProperty) fogProperty->color = {fogColor.x, fogColor.y, fogColor.z};   // +0x1C = +0x1CC
    if (level.renderer) level.renderer->SetBackgroundColor({fogColor.x, fogColor.y, fogColor.z});   // vf6C
    if (fogProperty) fogProperty->flags |= 2;         // the fog on
    fogEnabled = 1;
}

void Camera::DisableFog() {                           // orig: 0x00012DB0
    if (!fogEnabled) return;
    if (defaultFar > 1.0f) {
        farPlane = defaultFar;
        dirtyFrustum = 1;
    }
    if (fogProperty) fogProperty->flags &= ~2;
    fogEnabled = 0;
    g_clearFlags = 4;                                 // DAT_002D67E0
}

namespace {
// orig: the distance of 0x00012231 / 0x0001867D: (dz*dz + dy*dy) + dx*dx of unrounded differences, sqrt, + offset.
f32 FogNodeDistance(const ni::NiNode& n, const NiPoint3& cam, f32 offset) {
    const NiPoint3& t = n.world.translate;
    const double dx = double(t.x) - cam.x, dy = double(t.y) - cam.y, dz = double(t.z) - cam.z;
    return F(std::sqrt((dz * dz + dy * dy) + dx * dx) + offset);
}
}  // namespace

void Camera::SetFogRelativeNode(const char* name, const char* child, f32 offset) {   // orig: 0x00018520
    if (!name || !level.scene) return;
    ni::NiNode* n = dynamic_cast<ni::NiNode*>(GetObjectByName(level.scene.Get(), name));   // vf54, DynamicCast 0x002F9230
    if (!n) return;
    if (child) {
        n = dynamic_cast<ni::NiNode*>(GetObjectByName(n, child));
        if (!n) return;
    }
    fogNode = n;
    fogOffset = offset;
    SetFogDistance(FogNodeDistance(*n, position, offset));
}

void Camera::Click() {                                // orig: 0x00012410
    if (sceneCamera && level.scene) ni::VisitForCulling(*sceneCamera, level.scene.Get());   // the scene root S+0x18
}

void Camera::Apply() {                                // orig: 0x00012210
    if (fogEnabled) {                                 // orig: 0x00012217
        if (fogNode) SetFogDistance(FogNodeDistance(*fogNode, position, fogOffset));
        if (sceneCamera && sceneCamera->renderer)
            sceneCamera->renderer->SetBackgroundColor({fogColor.x, fogColor.y, fogColor.z});   // vf6C(&fogColor)
        g_clearFlags = fogDepth != 1.0f ? 4u : 5u;
    } else {
        g_clearFlags = 4;
    }
    ni::NiCamera* cam = sceneCamera.Get();
    if (!cam) return;
    if (dirtyFrustum) {                               // orig: SetFrustum 0x00011960 (a culling input)
        const f32 f[6] = {F(-double(halfWidth)), halfWidth, halfHeight, F(-double(halfHeight)), 0.25f, farPlane};
        cam->SetFrustum(f);
        dirtyFrustum = 0;
    }
    if (dirtyTranslate) {
        cam->local.translate = position;
        dirtyTranslate = 0;
    }
    if (dirtyRotate) {
        cam->local.rotate = rotation;
        dirtyRotate = 0;
    }
    if (dirtyLookAt) {
        UpdateNodeAt(cam, 0.0f);
        LookAt(*cam, lookAt, kUnitY);
        dirtyLookAt = 0;
    }
    if (shaking)
        cam->local.translate = {F(double(position.x) + shakeOffset.x), F(double(position.y) + shakeOffset.y),
                                F(double(position.z) + shakeOffset.z)};
    UpdateNodeAt(cam, 0.0f);
}

}  // namespace fut::game

namespace fut::game {

// ---- the colour fade --------------------------------------------------------------------------------------------
namespace {
using ni::detail::F;
using FadeColor = std::array<f32, 4>;
FadeColor Sub(const FadeColor& a, const FadeColor& b) {   // orig: 0x00011830 (each F(a - b))
    return {F(double(a[0]) - b[0]), F(double(a[1]) - b[1]), F(double(a[2]) - b[2]), F(double(a[3]) - b[3])};
}
FadeColor Add(const FadeColor& a, const FadeColor& b) {   // orig: 0x000117D0
    return {F(double(a[0]) + b[0]), F(double(a[1]) + b[1]), F(double(a[2]) + b[2]), F(double(a[3]) + b[3])};
}
FadeColor Mul(const FadeColor& a, f32 s) {          // orig: 0x00011890
    return {F(double(a[0]) * s), F(double(a[1]) * s), F(double(a[2]) * s), F(double(a[3]) * s)};
}
FadeColor Div(const FadeColor& a, f32 s) {          // orig: 0x000118F0: 1 / s; r times the register, the rest the float
    const double inv = 1.0 / double(s);
    const f32 fi = F(inv);
    return {F(double(a[0]) * inv), F(double(fi) * a[1]), F(double(fi) * a[2]), F(double(fi) * a[3])};
}
}  // namespace

bool Camera::UpdateFade(f32 dt) {                     // orig: 0x000198E0
    const f32 e = fadeElapsed;
    if (!(e < 0.0f)) {                                // TEST AH,5; JNP: nothing while negative (the delay)
        if (e <= fadeTime) {                          // TEST AH,0x41; JP
            fadeColor = Add(fadeColor21C, Div(Mul(Sub(fadeColor20C, fadeColor21C), e), fadeTime));
        } else if (!(double(fade244) + fadeTime < e)) {   // TEST AH,1; JNE
            fadeColor = fadeColor20C;
        } else if (!((double(fade244) + fade248) + fadeTime < e)) {
            const f32 u = F((double(e) - fadeTime) - fade244);
            fadeColor = Sub(fadeColor20C, Div(Mul(Sub(fadeColor20C, fadeColor22C), u), fade248));
        } else {
            fadeColor = fadeColor22C;
            fadeElapsed = F(((double(fade244) + fade248) + fadeTime) + 47.0f);   // [0x00293C3C]
        }
    }
    fadeElapsed = F(double(dt) + fadeElapsed);
    for (int i = 0; i < 3; ++i)
        if (!(fadeColor[i] > 0.0f)) fadeColor[i] = 0.0f;   // TEST AH,0x41; JP
    ni::NiCamera* cam = overlayCamera.Get();
    if (!(fadeColor[3] > 0.0f)) {
        fadeColor = {};
    } else if (!fadeAttached) {
        if (cam) cam->AttachScreenPolygon(fadePolygon.Get());   // 0x00019460
        fadeAttached = 1;
    }
    if (fadeAttached && fadePolygon) {                // the colour on all four vertices (+0x18), +0x1C |= 2
        for (auto& c : fadePolygon->colors) c = {fadeColor[0], fadeColor[1], fadeColor[2], fadeColor[3]};
        fadePolygon->flags |= 2;
    }
    if (!(fadeColor[3] > 0.0f)) {
        if (fadeAttached == 1) {
            if (cam) cam->DetachScreenPolygon(fadePolygon.Get());   // 0x00100FC0
            fadeAttached = 0;
        }
        return false;
    }
    return true;
}

void Camera::DrawOverlay() {                          // orig: 0x00012450
    if (overlayCamera) ni::VisitForCulling(*overlayCamera, nullptr);   // 0x00100CE0 (+0x15C: no scene)
}

}  // namespace fut::game

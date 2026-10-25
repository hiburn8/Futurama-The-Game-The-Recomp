// Closest point on a triangle; see ClosestPoint.h. x87 order of the original kept: `double` values stay on the FPU
// stack, F() = an FSTP to float; each comparison follows its FNSTSW / TEST AH,5 idiom (NaN included). All comparison
// constants are the float 0.0 at 0x00293AB8.
#include "game/level/ClosestPoint.h"

namespace fut::game {

using ni::NiPoint3;
using ni::detail::F;

namespace {

// orig: the three edge tails 0x000120CE / 0x0001213B / 0x00012194. dir (a copy of the edge) is unitized in place
// (length discarded), k = dir . p with the absolute point p, result = origin + dir * k; y and z products are stored
// as floats first, x is added on the stack.
void ProjectOnEdge(NiPoint3& p, NiPoint3 dir, const NiPoint3& origin) {
    ni::Unitize(dir);
    const double k = (double(dir.x) * p.x + double(dir.y) * p.y) + double(dir.z) * p.z;
    const f32 oy = F(dir.y * k);
    const f32 oz = F(dir.z * k);
    const f32 x = F(dir.x * k + origin.x);
    const f32 y = F(double(oy) + origin.y);
    const f32 z = F(double(oz) + origin.z);
    p = {x, y, z};
}

// (a.z*b.z + a.y*b.y) + a.x*b.x, every product of two floats (exact in double), sums on the stack.
double Dot(const NiPoint3& a, const NiPoint3& b) {
    return (double(a.z) * b.z + double(a.y) * b.y) + double(a.x) * b.x;
}

NiPoint3 Sub(const NiPoint3& a, const NiPoint3& b) {
    return {F(double(a.x) - b.x), F(double(a.y) - b.y), F(double(a.z) - b.z)};
}

}  // namespace

void ClosestPointOnTriangle(NiPoint3& p, const NiPoint3 tri[3]) {
    const NiPoint3& v0 = tri[0];
    const NiPoint3& v1 = tri[1];
    const NiPoint3& v2 = tri[2];

    // Barycentric inside test (0x00011CC0..0x00011E39). p - v0 stays on the stack.
    const double dx = double(p.x) - v0.x, dy = double(p.y) - v0.y, dz = double(p.z) - v0.z;
    const NiPoint3 e1 = Sub(v1, v0);                  // [esp+0x1C]
    const NiPoint3 e2 = Sub(v2, v0);                  // [esp+0x28]
    const f32 d11 = F(Dot(e1, e1));
    const f32 d22 = F(Dot(e2, e2));
    const f32 d12 = F(Dot(e2, e1));
    const f32 d1p = F((double(e1.z) * dz + double(e1.y) * dy) + double(e1.x) * dx);
    const double d2p = (double(e2.z) * dz + double(e2.y) * dy) + double(e2.x) * dx;
    const f32 s = F(double(d11) * d2p - double(d1p) * d12);    // weight of e2 (times the determinant)
    const f32 t = F(double(d1p) * d22 - d2p * d12);            // weight of e1
    if (!(s < 0.0f) && !(t < 0.0f)) {                 // JNP: taken only on "less" (NaN falls through)
        const double det = double(d22) * d11 - double(d12) * d12;
        if (!(det < double(t) + s)) return;           // inside (or NaN): p is left unchanged
    }

    // Outside (0x00011E3F).
    const NiPoint3 e12 = Sub(v2, v1);                 // [esp+0x34]
    const NiPoint3 e20{-e2.x, -e2.y, -e2.z};          // [esp+0x40] FCHS of e2: v0 - v2
    const NiPoint3 w0 = Sub(p, v0);                   // [esp+0x4C]

    // Vertex v0: (p-v0).e1 < 0 and (p-v0).e2 < 0 (e2 reloaded as -(-e2), the same bits).
    if (Dot(w0, e1) < 0.0) {
        if ((double(w0.z) * e2.z + double(e2.y) * w0.y) + double(e2.x) * w0.x < 0.0) {
            p = v0;
            return;
        }
    }
    // Vertex v1: (p-v1).(-e1) < 0 and (p-v1).e12 < 0.
    const NiPoint3 w1 = Sub(p, v1);                   // [esp+0x28] (overwrites e2)
    if ((double(w1.z) * -double(e1.z) + -double(e1.y) * w1.y) + -double(e1.x) * w1.x < 0.0) {
        if (Dot(w1, e12) < 0.0) {
            p = v1;
            return;
        }
    }
    // Vertex v2: (p-v2).(v0-v2) < 0 and (p-v2).(-e12) < 0.
    const NiPoint3 w2 = Sub(p, v2);                   // [esp+0x1C] (overwrites e1; the edge copy at +0x58 survives)
    if (Dot(w2, e20) < 0.0) {
        if ((double(w2.z) * -double(e12.z) + -double(e12.y) * w2.y) + -double(e12.x) * w2.x < 0.0) {
            p = v2;
            return;
        }
    }

    // Edge (0x0001205C): the smallest of the three dot products picks the edge.
    const double a = Dot(w1, w0);                     // kept on the stack
    const f32 b = F(Dot(w2, w1));
    const f32 c = F(Dot(w2, w0));
    if (a < b && a < c) {                             // FCOM b / FCOMP c, JP on >= or NaN
        ProjectOnEdge(p, e1, v0);
    } else if (b < c) {
        ProjectOnEdge(p, e12, v1);
    } else {
        ProjectOnEdge(p, e20, v2);
    }
}

}  // namespace fut::game

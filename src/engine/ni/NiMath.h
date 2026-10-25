// NetImmerse 4.2 math used by the scene graph, decompiled from default.xbe.
//
// Matrix convention: NiMatrix3 m[r][c] is the matrix M with p' = M * p, i.e. the 9 floats of a NIF file in order.
// The original keeps rotations column-major in a padded 3x4 block (memory word c*4 + r holds M[r][c]); only the
// interpretation differs, never the values.
//
// Floating point (bit-exact against the original on every shipped NIF, static and animated, docs/NOTES.md): each
// function reproduces the original's precision instruction by instruction.
//   * SSE code (the 3x3 rotation product) -> f32 arithmetic in the same operation order.
//   * x87 code -> `double` for values kept on the FPU stack and an explicit round to f32 wherever the original stores
//     a float (fstp dword). The Xbox runs the x87 unit with control word 0x27F (53-bit precision), so stack values
//     are exactly IEEE doubles. This rule applies to every float function ported from the game.
#pragma once

#include "core/Types.h"

#include <cmath>

namespace fut::ni {

struct NiPoint3 {
    f32 x = 0, y = 0, z = 0;
};

struct NiMatrix3 {
    f32 m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};

struct NiTransform {
    NiMatrix3 rotate;
    NiPoint3 translate;
    f32 scale = 1;
};

struct NiBound {
    NiPoint3 center;
    f32 radius = 0;
};

struct NiPlane {
    NiPoint3 normal;
    f32 constant = 0;
};

namespace detail {
inline f32 F(double v) { return static_cast<f32>(v); }   // x87 fstp dword
}  // namespace detail

// orig: 0x00013B30 (SSE mulps/addps): R = A * B, each column (A.col1*B[1][c] + A.col0*B[0][c]) + A.col2*B[2][c].
inline NiMatrix3 Multiply(const NiMatrix3& a, const NiMatrix3& b) {
    NiMatrix3 r;
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) {
            const f32 p1 = a.m[row][1] * b.m[1][c];
            const f32 p0 = a.m[row][0] * b.m[0][c];
            const f32 p2 = a.m[row][2] * b.m[2][c];
            const f32 s = p1 + p0;
            r.m[row][c] = s + p2;
        }
    return r;
}

// orig: 0x00103130 (x87 FCOS / FSIN -> C library, KNOWN_ISSUES #20).
inline NiMatrix3 AxisAngle(f32 angle, f32 x, f32 y, f32 z) {
    const double co = std::cos(double(angle)), s = std::sin(double(angle));
    const double u = 1.0 - co;
    using detail::F;
    const f32 t = F(u);
    const double ux = u * x;
    const f32 L0 = F(double(y) * ux);
    const double uxz = ux * z;
    const f32 L8 = F(double(t) * y * z);
    const f32 L4 = F(s * x);
    const double sy = s * y, sz = s * z;
    NiMatrix3 m;
    m.m[0][0] = F(double(x) * x * t + co);
    m.m[1][0] = F(double(L0) - sz);
    m.m[2][0] = F(sy + uxz);
    m.m[0][1] = F(sz + L0);
    m.m[1][1] = F(double(y) * y * t + co);
    m.m[2][1] = F(double(L8) - L4);
    m.m[0][2] = F(uxz - sy);
    m.m[1][2] = F(double(L4) + L8);
    m.m[2][2] = F(double(z) * z * t + co);
    return m;
}

// Shared x87 pattern of 0x0010E050 / 0x0014F010: p' = M * v * scale + t, where the x sum is rounded after the
// scale multiply, z before it, and y stays on the FPU stack until the final store.
inline NiPoint3 TransformPointX87(const NiMatrix3& m, const NiPoint3& v, f32 scale, const NiPoint3& t) {
    using detail::F;
    const auto& M = m.m;
    const double vx = v.x, vy = v.y, vz = v.z;
    const double xs = (M[0][1] * vy + M[0][2] * vz) + M[0][0] * vx;
    const double ys = (M[1][1] * vy + M[1][2] * vz) + M[1][0] * vx;
    const f32 zs = F((M[2][1] * vy + M[2][2] * vz) + M[2][0] * vx);
    const f32 x1 = F(xs * scale);
    return {F(double(x1) + t.x), F(ys * scale + t.y), F(double(zs) * scale + t.z)};
}

// orig: 0x0010E050. Result = A * B (A applied after B: world = parent * local).
inline NiTransform Compose(const NiTransform& a, const NiTransform& b) {
    NiTransform r;
    r.scale = b.scale * a.scale;                      // x87 multiply of two floats, stored: exact as f32 product
    r.rotate = Multiply(a.rotate, b.rotate);
    r.translate = TransformPointX87(a.rotate, b.translate, a.scale, a.translate);
    return r;
}

// orig: 0x0014D910 (transpose 0x00103C60). Rotation transposed, scale inverted, translation -(R^T t) / s.
inline NiTransform Invert(const NiTransform& t) {
    using detail::F;
    NiTransform r;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) r.rotate.m[row][col] = t.rotate.m[col][row];
    const double inv = 1.0 / double(t.scale);         // kept on the stack; also stored as the scale
    r.scale = F(inv);
    const auto& R = r.rotate.m;
    const double nx = -double(t.translate.x), ny = -double(t.translate.y), nz = -double(t.translate.z);
    const f32 tx = F((nz * R[0][2] + ny * R[0][1]) + nx * R[0][0]);
    const f32 ty = F((nz * R[1][2] + nx * R[1][0]) + ny * R[1][1]);
    const double tz = (nz * R[2][2] + nx * R[2][0]) + ny * R[2][1];
    r.translate = {F(tx * inv), F(ty * inv), F(tz * inv)};
    return r;
}

// orig: 0x0014F010. Bound in the space of `t` -> parent space.
inline NiBound TransformBound(const NiBound& b, const NiTransform& t) {
    NiBound r;
    r.center = TransformPointX87(t.rotate, b.center, t.scale, t.translate);
    r.radius = b.radius * t.scale;
    return r;
}

// orig: 0x0014EEE0 NiBound::Merge. Smallest sphere around both (when one contains the other, that one).
inline void MergeBound(NiBound& a, const NiBound& b) {
    using detail::F;
    const f32 dx = F(double(a.center.x) - b.center.x);
    const f32 dy = F(double(a.center.y) - b.center.y);
    const f32 dz = F(double(a.center.z) - b.center.z);
    const f32 dist2 = F((double(dx) * dx + double(dz) * dz) + double(dy) * dy);
    const double diffExt = double(b.radius) - a.radius;
    const f32 diff = F(diffExt);
    const bool contained = dist2 <= diffExt * diff;   // orig: st0 (unrounded diff) * stored diff vs dist2
    if (diff < 0.0f) {
        if (contained) return;                        // a contains b
    } else if (contained) {
        a = b;                                        // b contains a
        return;
    }
    const double dist = std::sqrt(double(dist2));
    if (dist > double(9.999999974752427e-07f)) {      // orig: DAT_002D7DA0 (1e-6f); skipped when <= eps
        const double k = (dist - diff) / (dist + dist);
        const f32 oy = F(dy * k), oz = F(dz * k);
        a.center.x = F(dx * k + b.center.x);
        a.center.y = F(double(oy) + b.center.y);
        a.center.z = F(double(oz) + b.center.z);
    }
    a.radius = F(((double(a.radius) + b.radius) + dist) * 0.5);
}

// orig: 0x00107620: polynomial atan2 approximation (|error| ~1e-5), x87, float constants; HALF_PI / PI are the
// engine's float globals 0x002F926C / 0x002F9274 (0x3FC90FDB / 0x40490FDB).
inline double FastAtan2(f32 y, f32 x) {
    constexpr f32 kHalfPi = 1.57079637f, kPi = 3.14159274f;
    if (x == 0.0f && y == 0.0f) return 0.0;
    f32 off = 0.0f;
    double t;
    if (std::fabs(double(x)) < std::fabs(double(y))) {
        t = double(x) / y;
        if (t > 0.0) {
            off = kHalfPi;
        } else if (t < 0.0) {
            off = -kHalfPi;
        }
    } else {
        t = double(y) / x;
    }
    const double t2 = t * t;
    const f32 k1 = 0.0208351f, k2 = 0.085133f, k3 = 0.180141f, k4 = 0.3302995f, k5 = 0.999866f;   // 0x002A6998..88
    double r = ((((k1 * t2 - k2) * t2 + k3) * t2 - k4) * t2 + k5) * t;
    if (off != 0.0f) r = off - r;
    if (y < 0.0f && x < 0.0f) r -= kPi;
    if (y > 0.0f && x < 0.0f) r += kPi;
    return r;
}

// orig: 0x00011770 NiMatrix3 * NiPoint3 (x87, each component rounded once when stored).
inline NiPoint3 MulVector(const NiMatrix3& r, const NiPoint3& v) {
    using detail::F;
    const auto& m = r.m;
    NiPoint3 o;
    o.x = F((double(m[0][2]) * v.z + double(m[0][1]) * v.y) + double(v.x) * m[0][0]);
    o.y = F((double(m[1][2]) * v.z + double(m[1][0]) * v.x) + double(m[1][1]) * v.y);
    o.z = F((double(m[2][2]) * v.z + double(m[2][0]) * v.x) + double(m[2][1]) * v.y);
    return o;
}

// orig: 0x000116A0 NiPoint3::Unitize: length sqrt(x*x + y*y + z*z) (x87); at most 1e-6 (0x00293ACC) the vector
// becomes zero and 0 is returned, otherwise each component is multiplied by 1 / length.
inline double Unitize(NiPoint3& v) {
    using detail::F;
    const double len = std::sqrt((double(v.x) * v.x + double(v.y) * v.y) + double(v.z) * v.z);
    if (!(len > double(1e-6f))) {                     // FCOM; TEST AH,0x41 (also a NaN length)
        v = {};
        return 0.0;
    }
    const double inv = 1.0 / len;
    v.x = F(inv * v.x);
    v.y = F(inv * v.y);
    v.z = F(inv * v.z);
    return len;
}

// orig: 0x00103290 M^T v (x87; each component rounded once when stored).
inline NiPoint3 TransposeMulVector(const NiMatrix3& r, const NiPoint3& v) {
    using detail::F;
    const auto& m = r.m;
    return {F((double(m[2][0]) * v.z + double(m[1][0]) * v.y) + double(v.x) * m[0][0]),
            F((double(m[0][1]) * v.x + double(m[2][1]) * v.z) + double(m[1][1]) * v.y),
            F((double(m[0][2]) * v.x + double(m[2][2]) * v.z) + double(m[1][2]) * v.y)};
}

// orig: 0x0006CE00: the unit cross product a x b (x87); zero when its length is at most 1e-6.
inline NiPoint3 UnitCross(const NiPoint3& a, const NiPoint3& b) {
    using detail::F;
    const double x = double(a.y) * b.z - double(b.y) * a.z;
    const f32 y = F(double(b.x) * a.z - double(a.x) * b.z);
    const double z = double(b.y) * a.x - double(a.y) * b.x;
    const double len = std::sqrt((double(y) * y + z * z) + x * x);
    if (!(len > double(1e-6f))) return {};            // FCOM [0x00293ACC]; TEST AH,0x41
    const double inv = 1.0 / len;
    return {F(inv * x), F(double(y) * inv), F(inv * z)};
}

// orig: 0x001032F0 NiMatrix3::Inverse (thiscall(out), RET 4; x87): the adjugate with each cofactor rounded to float
// (operand order of the original's column chunks; the cofactor formulas are symmetric under the transposition, the
// determinant is not), the determinant from the original's first chunk (= this matrix's first row), false when
// |det| <= 1e-6f (out then holds the adjugate), else every entry F(1/det * adj). (The padding dwords: 0.)
inline bool Inverse(const NiMatrix3& src, NiMatrix3& out) {
    using detail::F;
    const auto& m = src.m;
    auto& o = out.m;
    o[0][0] = F(double(m[2][2]) * m[1][1] - double(m[2][1]) * m[1][2]);   // chunk 0 (column 0)
    o[1][0] = F(double(m[2][0]) * m[1][2] - double(m[2][2]) * m[1][0]);
    o[2][0] = F(double(m[2][1]) * m[1][0] - double(m[2][0]) * m[1][1]);
    o[0][1] = F(double(m[0][2]) * m[2][1] - double(m[2][2]) * m[0][1]);   // chunk 1
    o[1][1] = F(double(m[2][2]) * m[0][0] - double(m[2][0]) * m[0][2]);
    o[2][1] = F(double(m[2][0]) * m[0][1] - double(m[0][0]) * m[2][1]);
    o[0][2] = F(double(m[0][1]) * m[1][2] - double(m[0][2]) * m[1][1]);   // chunk 2
    o[1][2] = F(double(m[1][0]) * m[0][2] - double(m[1][2]) * m[0][0]);
    o[2][2] = F(double(m[0][0]) * m[1][1] - double(m[1][0]) * m[0][1]);
    const double det = (double(m[0][2]) * o[2][0] + double(o[0][0]) * m[0][0]) + double(m[0][1]) * o[1][0];
    if (std::fabs(det) <= double(1e-6f)) return false;   // FCOMP [0x00293ACC]; TEST AH,0x41; JP (NaN goes on)
    const double inv = 1.0 / det;                        // FDIVR [0x00293A98]
    for (auto& row : o)
        for (f32& v : row) v = F(inv * v);
    return true;
}

// orig: 0x00103BB0 (thiscall on the source, out, RET 4): the inverse, or a zero matrix when the source is singular.
inline NiMatrix3 InverseOrZero(const NiMatrix3& src) {
    NiMatrix3 out;
    if (!Inverse(src, out))
        for (auto& row : out.m)
            for (f32& v : row) v = 0.0f;
    return out;
}

// orig: 0x001030B0 NiMatrix3::MakeYRotation: x87 FCOS / FSIN of the float angle, each stored as a float (the
// original's rows are this matrix's columns: [c 0 s], [0 1 0], [-s 0 c]). The port uses the C library cos / sin
// (KNOWN_ISSUES #20).
inline NiMatrix3 MakeYRotation(f32 angle) {
    using detail::F;
    const f32 c = F(std::cos(double(angle))), s = F(std::sin(double(angle)));
    NiMatrix3 r;
    r.m[0][0] = c;
    r.m[0][1] = 0;
    r.m[0][2] = -s;
    r.m[1][0] = 0;
    r.m[1][1] = 1;
    r.m[1][2] = 0;
    r.m[2][0] = s;
    r.m[2][1] = 0;
    r.m[2][2] = c;
    return r;
}

// orig: 0x00103070 NiMatrix3::MakeXRotation, the twin of MakeYRotation: the original's rows [1 0 0], [0 c -s],
// [0 s c] are this matrix's columns (FCHS on the x87 sine before its store: -F(s)). C library cos / sin
// (KNOWN_ISSUES #20).
inline NiMatrix3 MakeXRotation(f32 angle) {
    using detail::F;
    const f32 c = F(std::cos(double(angle))), s = F(std::sin(double(angle)));
    NiMatrix3 r;
    r.m[0][0] = 1;
    r.m[0][1] = 0;
    r.m[0][2] = 0;
    r.m[1][0] = 0;
    r.m[1][1] = c;
    r.m[1][2] = s;
    r.m[2][0] = 0;
    r.m[2][1] = -s;
    r.m[2][2] = c;
    return r;
}

// orig: 0x00011700 NiPoint3::Cross (thiscall a, (out, b), RET 8): each component a double difference of two float
// products, stored once.
inline NiPoint3 Cross(const NiPoint3& a, const NiPoint3& b) {
    using detail::F;
    return {F(double(a.y) * b.z - double(a.z) * b.y), F(double(a.z) * b.x - double(b.z) * a.x),
            F(double(a.x) * b.y - double(a.y) * b.x)};
}

// orig: 0x001030F0 NiMatrix3::MakeZRotation, the twin of MakeYRotation: the original's rows [c -s 0], [s c 0], [0 0 1]
// are this matrix's columns (FCHS on the x87 sine before its store: -F(s)). C library cos / sin (KNOWN_ISSUES #20).
inline NiMatrix3 MakeZRotation(f32 angle) {
    using detail::F;
    const f32 c = F(std::cos(double(angle))), s = F(std::sin(double(angle)));
    NiMatrix3 r;
    r.m[0][0] = c;
    r.m[0][1] = s;
    r.m[0][2] = 0;
    r.m[1][0] = -s;
    r.m[1][1] = c;
    r.m[1][2] = 0;
    r.m[2][0] = 0;
    r.m[2][1] = 0;
    r.m[2][2] = 1;
    return r;
}

// orig: 0x00103FA0 NiMatrix3::FromEulerAnglesXYZ (thiscall(a, b, c), RET 0xC): Y(a) * (X(b) * Z(c)) with
// NiMatrix3::Multiply 0x00013B30 (first X * Z, then Y * that). Y(a) is MakeYRotation(a); X(b) has the columns (1, 0, 0),
// (0, cb, -sb), (0, sb, cb); Z(c) the columns (cc, -sc, 0), (sc, cc, 0), (0, 0, 1) (x87 FCOS / FSIN of each angle, the
// negated sines by FCHS: -F(s)). C library cos / sin (KNOWN_ISSUES #20).
inline NiMatrix3 FromEulerAnglesXYZ(f32 a, f32 b, f32 c) {
    using detail::F;
    const f32 cb = F(std::cos(double(b))), sb = F(std::sin(double(b)));
    const f32 cc = F(std::cos(double(c))), sc = F(std::sin(double(c)));
    NiMatrix3 x;                                     // pre-image esp+0x70 (pads 0)
    x.m[0][0] = 1, x.m[1][0] = 0, x.m[2][0] = 0;
    x.m[0][1] = 0, x.m[1][1] = cb, x.m[2][1] = -sb;
    x.m[0][2] = 0, x.m[1][2] = sb, x.m[2][2] = cb;
    NiMatrix3 z;                                     // esp+0x40
    z.m[0][0] = cc, z.m[1][0] = -sc, z.m[2][0] = 0;
    z.m[0][1] = sc, z.m[1][1] = cc, z.m[2][1] = 0;
    z.m[0][2] = 0, z.m[1][2] = 0, z.m[2][2] = 1;
    return Multiply(MakeYRotation(a), Multiply(x, z));
}

// orig: 0x00103E10 (thiscall(a, b, c), RET 0xC; Debris::Init): X(a) * (Y(b) * Z(c)) with NiMatrix3::Multiply
// 0x00013B30 (first Y * Z, then X * that). X(a) has the rows [1 0 0], [0 ca sa], [0 -sa ca]; Y(b) is MakeYRotation(b)
// (rows [cb 0 -sb], [0 1 0], [sb 0 cb]); Z(c) the rows [cc sc 0], [-sc cc 0], [0 0 1] (x87 FCOS / FSIN of each angle,
// each stored as a float, the negated sines by FCHS: -F(s)). C library cos / sin (KNOWN_ISSUES #20).
inline NiMatrix3 EulerXYZProduct(f32 a, f32 b, f32 c) {
    using detail::F;
    const f32 ca = F(std::cos(double(a))), sa = F(std::sin(double(a)));
    const f32 cc = F(std::cos(double(c))), sc = F(std::sin(double(c)));
    NiMatrix3 x;                                     // B+0x08 (pads 0)
    x.m[0][0] = 1, x.m[0][1] = 0, x.m[0][2] = 0;
    x.m[1][0] = 0, x.m[1][1] = ca, x.m[1][2] = sa;
    x.m[2][0] = 0, x.m[2][1] = -sa, x.m[2][2] = ca;
    NiMatrix3 z;                                     // B+0x38
    z.m[0][0] = cc, z.m[0][1] = sc, z.m[0][2] = 0;
    z.m[1][0] = -sc, z.m[1][1] = cc, z.m[1][2] = 0;
    z.m[2][0] = 0, z.m[2][1] = 0, z.m[2][2] = 1;
    return Multiply(x, Multiply(MakeYRotation(b), z));
}

// orig: 0x00103680 NiMatrix3::ToEulerAnglesXYZ: false at the +-90 degree pole (then c = 0).
inline bool ToEulerAnglesXYZ(const NiMatrix3& m, f32& a, f32& b, f32& c) {
    using detail::F;
    constexpr f32 kHalfPi = 1.57079637f;
    const auto& M = m.m;
    const f32 s = M[1][2];
    double angle;
    if (-1.0f < s) {
        angle = s < 1.0f ? std::asin(double(s)) : kHalfPi;   // orig: CRT asin 0x00168900
    } else {
        angle = -double(kHalfPi);
    }
    b = F(angle);
    if (!(angle < kHalfPi)) {
        a = F(-FastAtan2(F(-double(M[0][1])), M[0][0]));
        c = 0;
        return false;
    }
    if (!(angle > -double(kHalfPi))) {
        a = F(FastAtan2(F(-double(M[0][1])), M[0][0]));
        c = 0;
        return false;
    }
    a = F(-FastAtan2(M[0][2], M[2][2]));
    c = F(-FastAtan2(M[1][0], M[1][1]));
    return true;
}

// orig: 0x001037B0 (thiscall(f32* a, f32* b, f32* c), RET 0xC; L name ToEulerAnglesYZX: Y(a) Z(b) X(c), the
// first-person camera 0x00018250): b = -asin of the console's word 1 (the port's m[1][0]; the CRT asin 0x00168900,
// the C library here, #20), the pole tests on the unrounded -angle; false at a pole (then c = 0).
inline bool ToEulerAnglesYZX(const NiMatrix3& m, f32& a, f32& b, f32& c) {
    using detail::F;
    constexpr f32 kHalfPi = 1.57079637f;             // DAT_002F926C (0x3FC90FDB at run time)
    const auto& M = m.m;
    const f32 s = M[1][0];
    double angle;
    if (-1.0f < s) {                                 // FCOMP; TEST AH,5; JP (NaN: -pi / 2)
        angle = s < 1.0f ? std::asin(double(s)) : kHalfPi;
    } else {
        angle = -double(kHalfPi);
    }
    angle = -angle;                                  // FCHS
    b = F(angle);
    if (!(angle < kHalfPi)) {                        // FCOM; TEST AH,5; JP 0x001038A5
        a = F(-FastAtan2(M[2][1], M[2][2]));
        c = 0;
        return false;
    }
    if (!(angle > -double(kHalfPi))) {               // FCOMPP; TEST AH,0x41; JNE 0x00103872
        a = F(FastAtan2(M[2][1], M[2][2]));
        c = 0;
        return false;
    }
    a = F(-FastAtan2(F(-double(M[2][0])), M[0][0]));
    c = F(-FastAtan2(F(-double(M[1][2])), M[1][1]));
    return true;
}

}  // namespace fut::ni

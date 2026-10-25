// NetImmerse 4.2 animation: controller time, key interpolation and the keyframe / visibility controllers,
// decompiled from default.xbe. Floating point follows the original instruction by instruction (see NiMath.h):
// values kept on the x87 stack are `double`, F() marks every store to a float.
//
// Shipped data uses only Euler rotation keys (type 4) with quadratic (Bezier) float keys per axis, quadratic position
// and scale keys, and linear or quadratic keys in the float / position / UV / morph data of the property controllers
// (tests/engine/NiStreamTests.cpp prints the histogram). TCB keys are never used and not ported. This build interpolates rotation
// keys only for the Euler type (0x00122070 leaves the result unset for the others), so that is all the port has.
#include "engine/ni/NiObjects.h"
#include "engine/ni/NiQuaternion.h"

#include <cmath>
#include <cstring>

namespace fut::ni {
namespace {

using detail::F;
constexpr f32 kMaxFloat = 3.4028234663852886e38f;    // orig: DAT_002A6984

// ---- float keys -----------------------------------------------------------------------------------------------------
// orig: 0x00152E40 (via 0x00120800): cubic Hermite with key 0's outgoing and key 1's incoming tangent, all x87.
double BezierFloat(f32 t, const FloatKey& k0, const FloatKey& k1) {
    const double d = double(k1.value) - k0.value;
    const double a = ((double(k0.outTangent) + k1.inTangent) - (d + d)) * t;
    const double b = d * 3.0 - ((double(k0.outTangent) + k0.outTangent) + k1.inTangent);
    return ((b + a) * t + k0.outTangent) * t + k0.value;
}

// orig: 0x0011FED0. Finds the segment around `t`, starting from the cached index, and interpolates it.
f32 InterpolateFloat(f32 t, const FloatKeys& keys, u32& lastIndex) {
    const auto& k = keys.keys;
    const u32 n = static_cast<u32>(k.size());
    if (n == 0) return 0;
    if (t <= k[0].time) {
        lastIndex = 0;
        return k[0].value;
    }
    const u32 last = n - 1;
    if (t >= k[last].time) {                          // (TEST AH,1: a NaN time goes on into the search)
        lastIndex = last;
        return k[last].value;
    }
    if (lastIndex > last) lastIndex = 0;              // port-only guard (the original trusts the cache)
    f32 t0 = k[lastIndex].time;
    if (t < t0) {
        lastIndex = 0;
        t0 = k[0].time;
    }
    u32 next = lastIndex + 1;
    f32 t1 = t;
    for (; next <= last; ++next) {
        t1 = k[next].time;
        if (t < t1) break;
        t0 = t1;
        ++lastIndex;
    }
    // Only a NaN time runs past the last key; the original then reads one key past the array (the NaN ratio makes
    // the result NaN whatever it holds), the port reuses the last key.
    if (next > last) next = last;
    const f32 ratio = F((double(t) - t0) / (double(t1) - t0));
    switch (keys.type) {
    case KeyType::Linear:                             // orig: 0x001322C0 -> 0x00152E20, x87
        return F((1.0 - ratio) * k[lastIndex].value + double(ratio) * k[next].value);
    case KeyType::Quadratic:
        return F(BezierFloat(ratio, k[lastIndex], k[next]));
    default:
        // NOT PORTED: TCB float keys (0x0013BAD0); no shipped file uses them.
        return k[lastIndex].value;
    }
}

// ---- position keys --------------------------------------------------------------------------------------------------
// orig: 0x00121430. The same cubic per component, with the original's float stores (they differ per component).
NiPoint3 BezierPos(f32 t, const PosKey& k0, const PosKey& k1) {
    const NiPoint3& v0 = k0.value;
    const NiPoint3& o0 = k0.outTangent;
    const NiPoint3& i1 = k1.inTangent;
    const NiPoint3& v1 = k1.value;
    NiPoint3 r;
    {   // x: 2*out0 + in1 and 3*d are stored, A = (in1 + out0) - 2d stored, B stored
        const f32 sx = F((double(o0.x) + o0.x) + i1.x);
        const f32 t3x = F((double(v1.x) - v0.x) * 3.0);
        const f32 bx = F(double(t3x) - sx);
        const f32 d2x = F((double(v1.x) - v0.x) * 2.0);
        const f32 ax = F((double(i1.x) + o0.x) - d2x);
        const double cx = double(ax) * t + bx;
        const f32 dx = F(cx * t + o0.x);
        const f32 dtx = F(double(dx) * t);
        r.x = F(double(dtx) + v0.x);
    }
    {   // y: sums stay on the stack until B, A, C and D*t are stored
        const double sy = (double(o0.y) + o0.y) + i1.y;
        const double dy = double(v1.y) - v0.y;
        const f32 by = F(dy * 3.0 - sy);
        const f32 ay = F((double(o0.y) + i1.y) - (dy + dy));
        const f32 aty = F(double(ay) * t);
        const f32 cy = F(double(aty) + by);
        const double dyv = double(cy) * t + o0.y;
        const f32 dty = F(dyv * t);
        r.y = F(double(dty) + v0.y);
    }
    {   // z: 2*out0 and d are stored first
        const f32 o2z = F(double(o0.z) + o0.z);
        const double sz = double(o2z) + i1.z;
        const f32 dz = F(double(v1.z) - v0.z);
        const f32 t3z = F(double(dz) * 3.0);
        const double bz = double(t3z) - sz;
        const f32 oiz = F(double(o0.z) + i1.z);
        const double az = double(oiz) - (double(dz) + dz);
        const f32 atz = F(az * t);
        const f32 cz = F(double(atz) + bz);
        const f32 ctz = F(double(cz) * t);
        const double dzv = double(ctz) + o0.z;
        r.z = F(dzv * t + v0.z);
    }
    return r;
}

// orig: 0x00120DD0 (same search as InterpolateFloat with position keys).
NiPoint3 InterpolatePos(f32 t, const PosKeys& keys, u32& lastIndex) {
    const auto& k = keys.keys;
    const u32 n = static_cast<u32>(k.size());
    if (n == 0) return {};
    if (t <= k[0].time) {
        lastIndex = 0;
        return k[0].value;
    }
    const u32 last = n - 1;
    if (t >= k[last].time) {                          // (TEST AH,1: a NaN time goes on into the search)
        lastIndex = last;
        return k[last].value;
    }
    if (lastIndex > last) lastIndex = 0;
    f32 t0 = k[lastIndex].time;
    if (t < t0) {
        lastIndex = 0;
        t0 = k[0].time;
    }
    u32 next = lastIndex + 1;
    f32 t1 = t;
    for (; next <= last; ++next) {
        t1 = k[next].time;
        if (t < t1) break;
        t0 = t1;
        ++lastIndex;
    }
    // Only a NaN time runs past the last key; the original then reads one key past the array (the NaN ratio makes
    // the result NaN whatever it holds), the port reuses the last key.
    if (next > last) next = last;
    const f32 ratio = F((double(t) - t0) / (double(t1) - t0));
    switch (keys.type) {
    case KeyType::Linear: {                           // orig: 0x00132660 (y and z products stored as floats)
        const double a = 1.0 - ratio;
        const NiPoint3& v0 = k[lastIndex].value;
        const NiPoint3& v1 = k[next].value;
        const f32 y0 = F(a * v0.y), z0 = F(a * v0.z), y1 = F(double(ratio) * v1.y), z1 = F(double(ratio) * v1.z);
        return {F(double(ratio) * v1.x + a * v0.x), F(double(y1) + y0), F(double(z1) + z0)};
    }
    case KeyType::Quadratic:
        return BezierPos(ratio, k[lastIndex], k[next]);
    default:
        // NOT PORTED: TCB position keys (0x0013A560); no shipped file uses them.
        return k[lastIndex].value;
    }
}

// ---- rotations ------------------------------------------------------------------------------------------------------
using Quat = NiQuaternion;

// orig: 0x001518A0. Angle-axis with x87 fsin/fcos of angle / 2.
Quat AxisAngle(f32 angle, const NiPoint3& axis) {
    const double h = double(angle) * 0.5;
    const double s = std::sin(h), c = std::cos(h);
    return {F(c), F(s * axis.x), F(s * axis.y), F(s * axis.z)};
}

// orig: 0x001517E0 (this = a, argument b): a * b, each component one x87 expression stored as float.
Quat Multiply(const Quat& a, const Quat& b) {
    Quat r;
    r.z = F(((double(b.z) * a.w + double(a.x) * b.y) + double(a.z) * b.w) - double(a.y) * b.x);
    r.y = F(((double(a.z) * b.x + double(a.w) * b.y) + double(a.y) * b.w) - double(b.z) * a.x);
    r.x = F(((double(a.x) * b.w + double(b.z) * a.y) + double(b.x) * a.w) - double(a.z) * b.y);
    r.w = F(((double(b.w) * a.w - double(b.x) * a.x) - double(a.y) * b.y) - double(a.z) * b.z);
    return r;
}

// orig: 0x002D7A38 - for each Euler order, the axis applied first, second, third.
constexpr u8 kEulerOrder[9][3] = {{0, 1, 2}, {0, 2, 1}, {1, 2, 0}, {1, 0, 2}, {2, 0, 1},
                                   {2, 1, 0}, {0, 1, 0}, {1, 2, 1}, {2, 0, 2}};
constexpr NiPoint3 kAxes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // orig: 0x002D77E0 (UNIT_X, UNIT_Y, UNIT_Z)

// orig: 0x00124460 (table entry g_NiRotKeyInterpolators[4]).
Quat InterpolateEuler(f32 t, const EulerRotKey& key) {
    f32 angles[3];
    for (int axis = 0; axis < 3; ++axis) {
        const FloatKeys& keys = key.axes[axis];
        angles[axis] = keys.keys.empty() ? 0.0f : InterpolateFloat(t, keys, key.lastIndex[axis]);
    }
    const Quat q[3] = {AxisAngle(angles[0], kAxes[0]), AxisAngle(angles[1], kAxes[1]), AxisAngle(angles[2], kAxes[2])};
    Quat r;
    const u8 order = key.axisOrder < 9 ? key.axisOrder : 0;   // the original indexes the table unchecked
    for (int i = 0; i < 3; ++i) r = Multiply(q[kEulerOrder[order][i]], r);
    return r;
}

// orig: 0x00129200 - step (bool) keys; the cached index is passed in as 0 by the visibility controller.
bool InterpolateBool(f32 t, const std::vector<BoolKey>& k, u32& index) {
    const u32 n = static_cast<u32>(k.size());
    if (t <= k[0].time) {
        index = 0;
        return k[0].value;
    }
    const u32 last = n - 1;
    if (t < k[last].time) {
        if (index > last || t < k[index].time) index = 0;
        for (u32 i = index + 1; i <= last; ++i) {
            if (t < k[i].time) break;
            ++index;
        }
        return k[index].value;
    }
    index = last;
    return k[last].value;
}

}  // namespace

// orig: 0x00115670 NiQuaternion::ToRotation. Six products are stored as floats, three stay on the stack.
NiMatrix3 QuatToRotation(const NiQuaternion& q) {
    const double tx = double(q.x) + q.x, ty = double(q.y) + q.y;
    const f32 tz = F(double(q.z) + q.z);
    const f32 twx = F(tx * q.w), twy = F(ty * q.w), twz = F(double(tz) * q.w);
    const f32 txx = F(tx * q.x), txy = F(ty * q.x), txz = F(double(tz) * q.x);
    const double tyy = ty * q.y, tyz = double(tz) * q.y, tzz = double(tz) * q.z;
    NiMatrix3 m;
    m.m[0][0] = F(1.0 - (tzz + tyy));
    m.m[1][0] = F(double(txy) + twz);
    m.m[2][0] = F(double(txz) - twy);
    m.m[0][1] = F(double(txy) - twz);
    m.m[1][1] = F(1.0 - (tzz + txx));
    m.m[2][1] = F(double(twx) + tyz);
    m.m[0][2] = F(double(txz) + twy);
    m.m[1][2] = F(tyz - twx);
    m.m[2][2] = F(1.0 - (tyy + txx));
    return m;
}


namespace {
// Constants of the fast slerp / normalise (orig: 0x002AAE68..0x002AAE7C as float bits, and the two products the
// static initializers 0x002314E0 / 0x00231500 compute into 0x0030F6E0 / 0x0030F6DC).
f32 Bits(u32 b) {
    f32 f;
    std::memcpy(&f, &b, 4);
    return f;
}
const f32 kNeighborhood = Bits(0x3F758559);           // 0.959066
const f32 kScale = Bits(0x3F800A31);                  // 1.000311
const f32 kNormLimit1 = Bits(0x3F6A4B55);             // 0.915212: one refinement at or below
const f32 kNormLimit2 = Bits(0x3F26F151);             // 0.652120: two refinements at or below
const f32 kSlerpK3 = Bits(0x3F15E2D1);
const f32 kSlerpK1 = Bits(0x3F52A2D1);
const f32 kAdditive = F(double(kScale) / std::sqrt(double(kNeighborhood)));
const f32 kFactor = F(-0.5 / (std::sqrt(double(kNeighborhood)) * kNeighborhood) * kScale);

double IsqrtStep(double n) { return (n - kNeighborhood) * kFactor + kAdditive; }

void FastNormalize(NiQuaternion& q) {                 // orig: 0x00153880
    const f32 n = F(((double(q.w) * q.w + double(q.x) * q.x) + double(q.y) * q.y) + double(q.z) * q.z);
    double g = IsqrtStep(n);
    if (n <= kNormLimit1) {
        g *= IsqrtStep(g * g * n);
        if (n <= kNormLimit2) g *= IsqrtStep(g * g * n);
    }
    q.x = F(g * q.x);
    q.y = F(g * q.y);
    q.z = F(g * q.z);
    q.w = F(g * q.w);
}
}  // namespace

NiQuaternion QuatFromRotation(const NiMatrix3& m) {   // orig: 0x001519A0
    const auto& M = m.m;
    NiQuaternion q;
    const double trace = (double(M[0][0]) + M[2][2]) + M[1][1];
    if (trace > 0.0) {
        const double root = std::sqrt(trace + 1.0);
        q.w = F(0.5 * root);
        const double r = 0.5 / root;
        q.x = F((double(M[2][1]) - M[1][2]) * r);
        q.y = F((double(M[0][2]) - M[2][0]) * r);
        q.z = F((double(M[1][0]) - M[0][1]) * r);
        return q;
    }
    static constexpr int kNext[3] = {1, 2, 0};       // orig: 0x002D7E24
    int i = M[1][1] > M[0][0] ? 1 : 0;
    if (M[i][i] < M[2][2]) i = 2;
    const int j = kNext[i], k = kNext[j];
    const double root = std::sqrt(((double(M[i][i]) - M[j][j]) - M[k][k]) + 1.0);
    f32* v[3] = {&q.x, &q.y, &q.z};
    *v[i] = F(0.5 * root);
    const double r = 0.5 / root;
    q.w = F((double(M[k][j]) - M[j][k]) * r);
    *v[j] = F((double(M[i][j]) + M[j][i]) * r);
    *v[k] = F((double(M[i][k]) + M[k][i]) * r);
    return q;
}

NiQuaternion QuatSlerp(f32 t, const NiQuaternion& p, const NiQuaternion& q) {   // orig: 0x00153940
    const f32 cosine = F(((double(p.y) * q.y + double(p.x) * q.x) + double(p.w) * q.w) + double(q.z) * p.z);
    const double a = 1.0 - double(cosine) * kSlerpK1;
    double tt;
    if (t <= 0.5f) {
        const double b = ((double(t) + t) - 3.0) * t + 1.0;
        tt = ((a * a * kSlerpK3) * b + 1.0) * t;
    } else {
        const double u = 1.0 - double(t);
        const double b = ((u + u) - 3.0) * u + 1.0;
        tt = 1.0 - u * ((a * a * kSlerpK3) * b + 1.0);
    }
    NiQuaternion r;
    r.z = F((double(q.z) - p.z) * tt + p.z);
    r.y = F((double(q.y) - p.y) * tt + p.y);
    r.x = F((double(q.x) - p.x) * tt + p.x);
    r.w = F((double(q.w) - p.w) * tt + p.w);
    FastNormalize(r);
    return r;
}

// ---- controllers ----------------------------------------------------------------------------------------------------
double NiTimeController::ComputeScaledTime(f32 time) {    // orig: 0x00141260
    if (startTime == -kMaxFloat) startTime = time;
    double delta;
    if (lastTime == -kMaxFloat) {
        accumulatedTime = 0;
        delta = (flags & 1) ? 0.0 : double(time);
    } else {
        delta = double(time) - lastTime;
    }
    lastTime = time;
    const double accumulated = delta * frequency + accumulatedTime;
    accumulatedTime = F(accumulated);
    const f32 scaled = F(accumulated + phase);
    // The original memoizes the last (lo, hi, scaled, cycle) -> result in globals 0x0030EA1C..28; it is a pure cache.
    double r = scaled;
    switch (CycleType()) {
    case 0: {                                         // loop
        const f32 span = F(double(hiKeyTime) - loKeyTime);
        if (span == 0.0f) {
            r = loKeyTime;
        } else {
            r = std::fmod(double(scaled), double(span)) + loKeyTime;
            if (r < 0.0) r += span;
        }
        break;
    }
    case 1: {                                         // reverse (ping-pong)
        const f32 span = F(double(hiKeyTime) - loKeyTime);
        if (span == 0.0f) {
            r = loKeyTime;
        } else {
            const f32 twoSpan = F(double(span) + span);
            r = std::fmod(double(scaled), double(twoSpan));
            if (r < 0.0) r += twoSpan;
            if (r > span) r = double(twoSpan) - r;
            r += loKeyTime;
        }
        break;
    }
    default:                                          // clamp
        break;
    }
    if (r > hiKeyTime) return hiKeyTime;
    if (r < loKeyTime) return loKeyTime;
    return r;
}

void NiSingleDataController::LinkObject(NiStream& s) {
    NiTimeController::LinkObject(s);
    dataPtr = s.ResolveLink(data);
}

NiKeyframeData* NiKeyframeController::Data() const { return dynamic_cast<NiKeyframeData*>(dataPtr.Get()); }

void NiKeyframeController::Update(f32 time) {         // orig: 0x001270B0
    bool update = false;
    if (IsActive() && lastTime != time) {
        if (!computeScaledFirst) {
            update = true;
        } else {
            const double r = ComputeScaledTime(time);
            if (r != double(lastScaledTime)) {
                lastScaledTime = F(r);
                update = true;
            }
        }
    }
    if (!update && (!IsActive() || !forceUpdate)) return;
    NiKeyframeData* kd = Data();
    if (!kd) return;
    const f32 scaled = F(ComputeScaledTime(time));
    lastScaledTime = scaled;
    auto* node = dynamic_cast<NiAVObject*>(targetPtr);
    if (node) {
        if (kd->rotations.type == KeyType::Euler && !kd->rotations.euler.empty())
            node->local.rotate = QuatToRotation(InterpolateEuler(scaled, kd->rotations.euler[0]));
        // Other rotation key types: the original computes nothing usable (see the file comment).
        if (!kd->positions.keys.empty()) node->local.translate = InterpolatePos(scaled, kd->positions, lastPosIndex);
        if (!kd->scales.keys.empty())
            node->local.scale = std::fabs(InterpolateFloat(scaled, kd->scales, lastScaleIndex));
    }
    forceUpdate = false;
}

void NiVisController::Update(f32 time) {              // orig: 0x00129A60
    if (!IsActive() || lastTime == time) return;
    if (computeScaledFirst) {
        const double r = ComputeScaledTime(time);
        if (r == double(lastScaledTime)) return;
        lastScaledTime = F(r);
    }
    auto* vd = dynamic_cast<NiVisData*>(dataPtr.Get());
    if (!vd || vd->keys.empty()) return;
    u32 index = 0;
    const bool visible = InterpolateBool(lastScaledTime, vd->keys, index);
    lastIndex = index;
    if (auto* node = dynamic_cast<NiAVObject*>(targetPtr)) {
        if (visible) {
            node->flags &= ~kNiAVHidden;
        } else {
            node->flags |= kNiAVHidden;
        }
    }
}

// orig: 0x00101C60. The properties' controllers first (property list order, through NiProperty vtbl+0x30 =
// 0x000FDEF0), then this object's own controller chain.
void NiAVObject::UpdateControllers(f32 time, bool updateProperties) {
    if (updateProperties) {
        for (const auto& p : properties) {
            if (!p || !p->controller) continue;
            for (NiTimeController* c = p->controller.Get(); c; c = c->nextControllerPtr.Get()) c->Update(time);
        }
    }
    for (NiTimeController* c = controller.Get(); c; c = c->nextControllerPtr.Get()) c->Update(time);
}

// ---- property controllers -------------------------------------------------------------------------------------------
bool NiTimeController::SkipUpdate(f32 time) {        // orig: 0x00034140
    if (!IsActive() || lastTime == time) return true;
    if (computeScaledFirst) {
        const double r = ComputeScaledTime(time);
        if (r == double(lastScaledTime)) return true;
        lastScaledTime = F(r);
    }
    return false;
}

void NiAlphaController::Update(f32 time) {            // orig: 0x001206E0
    auto* material = dynamic_cast<NiMaterialProperty*>(targetPtr);
    if (!material || SkipUpdate(time)) return;
    auto* fd = dynamic_cast<NiFloatData*>(dataPtr.Get());
    if (!fd || fd->keys.keys.empty()) return;        // the original reads through a null key array here
    material->alpha = InterpolateFloat(lastScaledTime, fd->keys, lastIndex);
    ++material->revision;
}

void NiMaterialColorController::Update(f32 time) {    // orig: 0x00133730
    if (SkipUpdate(time)) return;
    auto* pd = dynamic_cast<NiPosData*>(dataPtr.Get());
    if (!pd || pd->keys.keys.empty()) return;
    const NiPoint3 c = InterpolatePos(lastScaledTime, pd->keys, lastIndex);
    // Clamp to [0, 1] with the original's comparisons (a NaN channel becomes 0).
    f32 r = c.x, g = c.y, b = 0.0f;
    if (0.0f <= c.x) {
        if (1.0f < c.x) r = 1.0f;
    } else {
        r = 0.0f;
    }
    if (0.0f <= c.y) {
        if (1.0f < c.y) g = 1.0f;
    } else {
        g = 0.0f;
    }
    if (0.0f <= c.z) {
        b = c.z;
        if (1.0f < c.z) b = 1.0f;
    }
    auto* material = dynamic_cast<NiMaterialProperty*>(targetPtr);
    if (!material) return;
    NiColor* slot = nullptr;
    switch ((flags & 0x70) >> 4) {
    case 0: slot = &material->ambient; break;
    case 1: slot = &material->diffuse; break;
    case 2: slot = &material->specular; break;
    case 3: slot = &material->emissive; break;
    default: return;
    }
    *slot = {r, g, b};
    ++material->revision;
}

void NiUVController::LinkObject(NiStream& s) {        // orig: 0x00129030
    NiTimeController::LinkObject(s);
    dataPtr = s.ResolveLink(data);
}

void NiUVController::Update(f32 time) {               // orig: 0x001286F0
    if (!SkipUpdate(time)) uvDirty = true;
}

void NiUVController::OnPreDisplay() {                 // orig: 0x00128740
    if (!uvDirty) return;
    uvDirty = false;
    auto* geom = dynamic_cast<NiGeometry*>(targetPtr);
    NiGeometryData* d = geom ? geom->modelData.Get() : nullptr;   // (+0xC)+0xC8 (the original reads it unchecked)
    if (!d || textureSet >= d->textureSets) return;
    const auto* uv = dynamic_cast<const NiUVData*>(dataPtr.Get());   // +0x58
    // each group's value at lastScaledTime (NiFloatKey::GenInterp 0x0011FED0; no keys: 0 / 0 / 1 / 1)
    auto value = [&](u32 g, u32 cache, f32 none) {
        if (!uv || uv->groups[g].keys.empty()) return none;
        return InterpolateFloat(lastScaledTime, uv->groups[g], lastIndex[cache]);
    };
    const f32 offU = value(0, 0, 0.0f), offV = value(1, 1, 0.0f);
    const f32 tileU = value(2, 2, 1.0f), tileV = value(3, 3, 1.0f);
    const double sU = double(tileU) / appliedTiling[0];   // FLD; FDIV +0x64 (FST: the float kept below)
    const f32 sUf = F(sU);
    const f32 aU = F(double(offU) - sU * appliedOffset[0]);   // FMUL +0x5C; FSUBR
    const double sV = double(tileV) / appliedTiling[1];
    const f32 sVf = F(sV);
    const f32 aV = F(double(offV) - sV * appliedOffset[1]);
    const f32 bU = F((1.0 - sUf) * 0.5f + aU);       // [0x00293A98] 1.0f, [0x00293AA0] 0.5f
    const f32 bV = F((1.0 - sVf) * 0.5f + aV);
    const size_t count = d->vertexCount;             // data +8
    const size_t base = size_t(textureSet) * count;   // 0x00143840(set)
    for (size_t i = 0; i < count && base + i < d->uvs.size(); ++i) {
        NiPoint2& t = d->uvs[base + i];
        t.u = F(double(sUf) * t.u + bU);
        t.v = F(double(sVf) * t.v + bV);
    }
    appliedOffset[0] = offU, appliedTiling[0] = tileU;
    appliedOffset[1] = offV, appliedTiling[1] = tileV;
    d->revisionFlags |= 8;                            // +0x2E |= 8
}

void NiGeomMorpherController::LinkObject(NiStream& s) {   // orig: 0x00125D00
    NiTimeController::LinkObject(s);
    dataPtr = s.ResolveLink(data);
    // NOT PORTED YET: the original then calls vtbl+0x58 (allocates the morph state) and, for files up to 4.1.0.12,
    // vtbl+0x50 with the key range.
}

void NiGeomMorpherController::Update(f32 time) {      // orig: 0x00152FD0
    if (!dataPtr) return;
    if (!SkipUpdate(time) || alwaysUpdate) morphDirty = true;
}

void NiFlipController::LinkObject(NiStream& s) {      // orig: 0x00125370
    NiTimeController::LinkObject(s);
    // The original only consumes the source link IDs when the controller has a target; without one the stream's
    // link cursor would fall out of step for the following objects. The port resolves per field, so it cannot desync.
    sourceTextures.clear();
    if (targetPtr)
        for (u32 id : sources) sourceTextures.emplace_back(s.ResolveLink(id));
    const double total = double(static_cast<u32>(sourceTextures.size())) * flipDelta;
    span = F(total);
    hiKeyTime = F(total + flipStartTime);
    if (index >= sourceTextures.size()) index = 0;
}

void NiFlipController::Update(f32 time) {             // orig: 0x00124C60
    if (SkipUpdate(time)) return;
    const u32 count = static_cast<u32>(sourceTextures.size());
    const double v = (double(count) / span) * (double(lastScaledTime) - loKeyTime);
    index = static_cast<u32>(static_cast<s32>(v));   // orig: _ftol (truncation)
    if (index >= count) index = count - 1;
    auto* tex = dynamic_cast<NiTexturingProperty*>(targetPtr);
    if (!tex || count == 0) return;
    NiObject* source = sourceTextures[index].Get();
    // orig quirk: the existence check reads slot (affectedMap != 0), i.e. slot 0 or 1, not affectedMap itself.
    if (affectedMap < tex->present.size() && tex->HasMap(affectedMap != 0 ? 1u : 0u)) {
        if (affectedMap < tex->maps.size()) tex->maps[affectedMap].textureObj = source;
        return;
    }
    NiTexturingProperty::Map m;                       // orig: 0x00109BB0 / 0x00124C00 with these defaults
    m.textureObj = source;
    m.clampMode = 3;
    m.filterMode = 1;
    m.textureIndex = 0;
    m.ps2L = 0;
    m.ps2K = 0xFFB5;
    if (affectedMap == 5) {
        m.isBumpMap = true;
        m.lumaScale = 1.0f;
        m.lumaOffset = 0.0f;
        m.bumpMatrix[0] = 0.5f;
        m.bumpMatrix[1] = 0.0f;
        m.bumpMatrix[2] = 0.0f;
        m.bumpMatrix[3] = 0.5f;
    }
    tex->SetMap(affectedMap, m);
}

}  // namespace fut::ni

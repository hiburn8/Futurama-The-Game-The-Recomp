#include "core/XbeImage.h"
// XDK DirectSound math; see XdsMath.h. Addresses are default.xbe's; "test ah, M; jcc" comments give the x87
// compare the branch implements (C3 C2 C0 of FCOM / FCOMP).
#include "audio/xdsound/XdsMath.h"

#include <emmintrin.h>

#include <cmath>
#include <cstring>

namespace fut::audio::xds {

namespace {

f32 FromBits(u32 b) {
    f32 f;
    std::memcpy(&f, &b, 4);
    return f;
}

// DSOUND / CRT float constants (their bits as read from the XBE)
const f32 kK90 = FromBits(0x3C360B61);           // 0x002A6048 1/90
const f32 kLog2Of10 = FromBits(0x40549A78);      // 0x0024E366 (immediate)
const f32 kPi = FromBits(0x40490FDB);            // 0x00263B20 / 0x00263B24
const f32 kHalfMilli = FromBits(0x3A03126F);     // 0x002A60D8 0.0005
const f32 kInv24k = FromBits(0x382EC33E);        // 0x002A60F0 1/24000
const f32 kInv48k = FromBits(0x37AEC33E);        // 0x002A5FB0 1/48000
const f32 kCentiF = FromBits(0x3C23D70A);        // 0x00293AAC 0.01
const f32 kOneE5 = FromBits(0x3727C5AC);         // 0x002A0FE8 1e-5
const f32 kFifth = FromBits(0x3E4CCCCD);         // 0x00293AB4 0.2
const f32 kPoint8 = FromBits(0x3F4CCCCD);        // 0x00293C18 0.8
const f32 kDecayScale = FromBits(0x3F169696);    // 0x002A60E0 0.5882353
constexpr f32 kFltMax = 3.4028234663852886e+38f; // 0x00295604

// The DSOUND reverb tables 0x00263A68..0x00263B04 (Tables, read from the user's default.xbe): float bits; 0 while
// not loaded.
f32 Tab(const std::vector<u32>& t, u32 i) { return i < t.size() ? FromBits(t[i]) : 0.0f; }

// FISTP m32 with the round-to-nearest control word (indefinite 0x80000000 out of range)
s32 Fistp(double v) { return _mm_cvtsd_si32(_mm_set_sd(v)); }
// orig: 0x00167110 _ftol: FISTP m64 with truncation, low dword (indefinite 0x8000000000000000 -> 0)
u32 Ftol(double v) {
    if (!(v > -9223372036854775808.0 && v < 9223372036854775808.0)) return 0;
    return static_cast<u32>(static_cast<s64>(v));
}
// FILD of a dword followed by "jge; fadd 2^32": the unsigned value
double U2D(u32 v) { return static_cast<double>(v); }
// FYL2X with FLDLG2 (log10) - see the header note on transcendentals
double Log10(double v) { return std::log10(v); }

// orig: 0x00249745 |v| = sqrt((x*x + y*y) + z*z)
double Length3(const f32* v) {
    return std::sqrt((double(v[0]) * v[0] + double(v[1]) * v[1]) + double(v[2]) * v[2]);
}

// orig: 0x0024C051 normalise in place, returns the length (0 for the zero vector, FLT_MAX unnormalised when a
// component is FLT_MAX)
double Normalize(f32* v) {
    if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f) return 0.0;   // test ah, 0x44; jp (all equal 0)
    if (v[0] == kFltMax || v[1] == kFltMax || v[2] == kFltMax) return kFltMax;
    const double len = Length3(v);
    const double inv = 1.0 / len;
    v[0] = static_cast<f32>(inv * v[0]);
    v[1] = static_cast<f32>(inv * v[1]);
    v[2] = static_cast<f32>(inv * v[2]);
    return len;
}

// orig: 0x0024976C no-curve rolloff: x < 0 -> 0, else -2000 * log10(x + 1)
s32 RolloffLevel(f32 x) {
    if (!(x >= 0.0f)) return 0;                      // test ah, 1; jne (less / unordered)
    return Cvtt(static_cast<f32>(Log10(double(x) + 1.0) * -2000.0));
}

const Listener kDefaultListener{};               // 0x00263878
constexpr f32 kDefaultRight[3] = {1, 0, 0};      // 0x00251060

// orig: 0x002498E7 stage 1: direction and distance
void StageDirection(Source& s, const ListenerFrame& lf, const Buffer3D& p) {
    s.direction[0] = p.position[0], s.direction[1] = p.position[1], s.direction[2] = p.position[2];
    if (p.mode != 1) {
        const f32* lp = lf.listener->position;
        s.direction[0] = static_cast<f32>(double(s.direction[0]) - lp[0]);
        s.direction[1] = static_cast<f32>(double(s.direction[1]) - lp[1]);
        s.direction[2] = static_cast<f32>(double(s.direction[2]) - lp[2]);
    }
    s.distance = static_cast<f32>(Normalize(s.direction));
}

// orig: 0x00249E00 CFullHrtfSource_GetHrtfFilterPair
void StageHrtfPair(Source& s, const ListenerFrame& lf, const Tables& t) {
    const f32 eb = !(s.elevation >= 0.0f) ? static_cast<f32>(double(s.elevation) - 3.0)   // test ah, 1; jne
                                          : static_cast<f32>(double(s.elevation) + 3.0);
    const s32 el = (Cvtt(eb) / 6) * 6;
    const double aa = std::fabs(double(s.azimuth));
    const s32 ae = el < 0 ? static_cast<s32>(0u - static_cast<u32>(el)) : el;   // cdq; xor; sub
    s32 az;
    if (ae == 90) az = 0;
    else if (ae > 60) az = (Cvtt(static_cast<f32>(aa + 6.0)) / 12) * 12;
    else if (ae > 30) az = (Cvtt(static_cast<f32>(aa + 3.0)) / 6) * 6;
    else az = (Cvtt(static_cast<f32>(aa + 1.5)) / 3) * 3;
    if (lf.rearSpeakers && az > 90) az = 180 - az;
    u32 ai = static_cast<u32>(az / 3);
    u32 ei = static_cast<u32>(static_cast<s32>(static_cast<u32>(el) + 90u) / 6);
    if (ai >= 61) ai = 0;
    if (ei >= 31) ei = 0;
    const u32 k = t.Loaded() ? t.hrtfIndex[ai * 31 + ei] : 0;
    u32 h0 = k, h1 = k + 1;
    if (!(s.azimuth >= 0.0f)) h0 = k + 1, h1 = k;    // test ah, 1; jne
    if (h0 != s.hrtf0 || h1 != s.hrtf1) {
        s.dirty |= 0x20;
        s.hrtf0 = h0, s.hrtf1 = h1;
    }
}

// orig: 0x00249F34 stage 2: azimuth / elevation in the listener frame (piecewise-linear arctangents)
void StageAngles(Source& s, const ListenerFrame& lf, const Buffer3D& p, const Tables& t) {
    const Listener& l = p.mode == 1 ? kDefaultListener : *lf.listener;
    const f32* right = p.mode == 1 ? kDefaultRight : lf.right;
    const f32* d = s.direction;
    const f32 f = static_cast<f32>((double(l.front[2]) * d[2] + double(l.front[1]) * d[1]) + double(l.front[0]) * d[0]);
    const double r = (double(right[2]) * d[2] + double(right[1]) * d[1]) + double(right[0]) * d[0];
    if (s.distance == 0.0f) {                        // test ah, 0x44; jnp
        s.elevation = 0.0f;
    } else {
        const double tv = (double(l.top[2]) * d[2] + double(l.top[1]) * d[1]) + double(d[0]) * l.top[0];
        const double at = std::fabs(tv);
        const double h = std::sqrt(r * r + double(f) * f);
        if (at < h) s.elevation = static_cast<f32>(at / h * 45.0);    // test ah, 5; jp
        else s.elevation = static_cast<f32>(90.0 - h / at * 45.0);
        if (tv < 0.0) s.elevation = -s.elevation;
    }
    if (s.distance == 0.0f) {
        s.azimuth = 0.0f;
    } else {
        const double af = std::fabs(double(f)), ar = std::fabs(r);
        if (af > ar) s.azimuth = static_cast<f32>(ar / af * 45.0);    // test ah, 0x41; jne
        else if (ar != 0.0) s.azimuth = static_cast<f32>(90.0 - af / ar * 45.0);
        else s.azimuth = 0.0f;
    }
    if (f < 0.0f) s.azimuth = static_cast<f32>(180.0 - double(s.azimuth));
    if (r < 0.0) s.azimuth = -s.azimuth;
    StageHrtfPair(s, lf, t);
}

// orig: 0x00249933 stage 3: distance attenuation (rolloff curve or the rolloff factors)
void StageDistance(Source& s, const ListenerFrame& lf, u32 flags, const Buffer3D& p) {
    double d = s.distance;
    s32 att;
    if (!(d > p.minDistance)) {                      // test ah, 0x41; jne
        att = 0;
    } else if ((flags & 0x20000) && d >= p.maxDistance) {
        att = -10000;
    } else {
        if (d > p.maxDistance) d = p.maxDistance;
        if (p.curve && p.curveCount) {
            const double dm = d - p.minDistance;
            const double seg = (double(p.maxDistance) - p.minDistance) / U2D(p.curveCount);
            const double inv = 1.0 / seg;
            const f32 invF = static_cast<f32>(inv);
            u32 i = Ftol(inv * dm);
            if (i >= p.curveCount) i = p.curveCount - 1;
            f32 lo, hi;
            double x;
            if (i == 0) {
                hi = p.curve[0], lo = 1.0f, x = dm;
            } else {
                lo = p.curve[i - 1], hi = p.curve[i];
                x = dm - U2D(i) * seg;
            }
            const f32 g = static_cast<f32>((double(hi) - lo) * (double(invF) * x) + lo);
            att = AmplitudeToLevel(g);
        } else {
            const double rf = double(lf.listener->rolloffFactor) * p.rolloffFactor;
            att = RolloffLevel(static_cast<f32>((d / p.minDistance - 1.0) * rf));
        }
    }
    if (att != s.distanceAtt) {
        s.dirty |= 1;
        s.distanceAtt = att;
    }
}

// orig: 0x00249A56 stage 4: cone angle between the source direction and the cone orientation
void StageConeAngle(Source& s, const Buffer3D& p) {
    const f32* o = p.coneOrientation;
    const f32* d = s.direction;
    const f32 dif[3] = {static_cast<f32>(double(o[0]) - d[0]), static_cast<f32>(double(o[1]) - d[1]),
                        static_cast<f32>(double(o[2]) - d[2])};
    const f32 a = static_cast<f32>(Length3(dif));
    const f32 sum[3] = {static_cast<f32>(double(o[0]) + d[0]), static_cast<f32>(double(d[1]) + o[1]),
                        static_cast<f32>(double(d[2]) + o[2])};
    const double b = Length3(sum);
    if (b < a) s.coneAngle = static_cast<f32>(b / a * 45.0 * 4.0);   // test ah, 5; jp
    else s.coneAngle = static_cast<f32>((90.0 - double(a) / b * 45.0) * 4.0);
}

// orig: 0x00249AEE stage 5: cone attenuation
void StageConeAtt(Source& s, const Buffer3D& p) {
    const u32 in = p.insideConeAngle, outA = p.outsideConeAngle;
    s32 att;
    if (!(U2D(in) < s.coneAngle)) {                  // test ah, 1; jne
        att = 0;
    } else if (in < outA && !(U2D(outA) > s.coneAngle)) {   // test ah, 0x41; jp
        att = p.coneOutsideVolume;
    } else {
        u32 diff = outA - in;
        if (diff < 1) diff = 1;
        att = Cvtt(static_cast<f32>((double(s.coneAngle) - U2D(in)) * double(p.coneOutsideVolume) / U2D(diff)));
    }
    if (att != s.coneAtt) {
        s.dirty |= 2;
        s.coneAtt = att;
    }
}

// orig: 0x00249B85 stage 6: front / rear split (consoles with rear speakers)
void StageFrontRear(Source& s, const ListenerFrame& lf, const Buffer3D& p) {
    s32 front, rear;
    if (!lf.rearSpeakers) {
        front = 0, rear = -10000;
    } else {
        const double dd = double(p.distanceFactor) * lf.listener->distanceFactor * s.distance;
        const f32 elAbs = std::fabs(s.elevation);
        const double e = 1.0 - double(elAbs) * kK90;
        const double x = e * (std::fabs(double(s.azimuth)) * kK90 - 1.0);
        f32 r = static_cast<f32>((x + 1.0) * 0.5 * 2.0 - 0.5);
        if (r > 1.0f) r = 1.0f;                      // test ah, 0x41; jne
        else if (r < 0.0f) r = 0.0f;                 // test ah, 5; jp
        if (dd < 0.5) r = static_cast<f32>(((double(r) - 0.5) * dd + 1.0) * 0.5);
        front = PowerToLevel(static_cast<f32>(1.0 - double(r)));
        rear = PowerToLevel(r);
    }
    if (front != s.front || rear != s.rear) {
        s.dirty |= 4;
        s.front = front, s.rear = rear;
    }
}

// orig: 0x00249C6A stage 7: centre speaker (voices whose slot 4 is bin 2)
void StageCentre(Source& s, const Tables& t) {
    s32 c0 = 0, c1 = -10000;
    if (s.centreFlag != 0 && t.centreGain.size() == 45 && t.centreCut.size() == 45) {
        const s32 ai = Cvtt(!(s.azimuth >= 0.0f) ? -s.azimuth : s.azimuth);
        const s32 ei = Cvtt(!(s.elevation >= 0.0f) ? -s.elevation : s.elevation);
        if (ai < 45 && ei < 45 && ai >= 0 && ei >= 0) {   // (a negative index needs a NaN angle: not reached)
            s32 g = AmplitudeToLevel(static_cast<f32>(double(t.centreGain[ai]) * t.centreGain[ei]));
            s32 c = AmplitudeToLevel(static_cast<f32>(1.0 - double(t.centreCut[ai]) * t.centreCut[ei]));
            if (c < -10000) c = -10000;
            else if (c > 0) c = 0;
            if (g < -10000) g = -10000;
            else if (g > 0) g = 0;
            c0 = c, c1 = g;
        }
    }
    if (c0 != s.centreFront || c1 != s.centre) {
        s.dirty |= 8;
        s.centreFront = c0, s.centre = c1;
    }
}

// orig: 0x00249D45 stage 8: Doppler pitch
void StageDoppler(Source& s, const ListenerFrame& lf, const Buffer3D& p) {
    const Listener& l = *lf.listener;
    const f32* d = s.direction;
    double v;
    if (p.mode == 1) {
        v = (double(p.velocity[2]) * d[2] + double(p.velocity[1]) * d[1]) + double(p.velocity[0]) * d[0];
    } else {
        v = ((double(p.velocity[2]) - l.velocity[2]) * d[2] + (double(p.velocity[1]) - l.velocity[1]) * d[1]) +
            (double(p.velocity[0]) - l.velocity[0]) * d[0];
    }
    const double w = double(l.distanceFactor) * p.distanceFactor * v * p.dopplerFactor * l.dopplerFactor;
    s32 dop;
    if (w == 0.0) dop = 0;                           // test ah, 0x44; jp
    else if (w >= 342.0) dop = -32767;               // test ah, 1; jne
    else if (w <= -342.0) dop = 4096;                // test ah, 0x41; jp
    else dop = PitchFromRatio(static_cast<f32>(1.0 - w / 342.0));
    if (dop != s.doppler) {
        s.dirty |= 0x10;
        s.doppler = dop;
    }
}

}  // namespace

s32 Cvtt(f32 v) { return _mm_cvttss_si32(_mm_set_ss(v)); }

s32 AmplitudeToLevel(f32 g) {                    // orig: 0x002497EA
    if (g <= 0.0f) return -10000;                // test ah, 0x41; jp
    if (!(g < 1.0f)) return 0;                   // test ah, 1; jne
    return Cvtt(static_cast<f32>(Log10(g) * 2000.0));
}

s32 PowerToLevel(f32 g) {                        // orig: 0x002497A3
    if (g <= 0.0f) return -10000;
    if (!(g < 1.0f)) return 0;
    return Cvtt(static_cast<f32>(Log10(g) * 1000.0));
}

bool Tables::LoadFromXbe(const std::vector<u8>& xbe) {
    *this = Tables{};
    xbe::XbeImage image;
    if (!image.Load(xbe)) return false;
    auto read = [&](u32 va, u32 n, std::vector<u8>& out) {
        const u8* data = image.At(va, n);
        if (!data) return false;
        out.assign(data, data + n);
        return true;
    };
    std::vector<u8> index, gain, cut, rows, signs, delays;
    if (!read(0x00262960, 61 * 31 * 2, index) || !read(kHrtfFilterBase, kHrtfFilterCount * 32, hrtfFilters) ||
        !read(0x00263A38, 32, hrtfDefault) || !read(0x002510C0, 45 * 4, gain) || !read(0x00251178, 45 * 4, cut) ||
        !read(0x00263A68, 20 * 4, rows) || !read(0x00263AB8, 8 * 4, signs) || !read(0x00263AE8, 8 * 4, delays))
        return false;
    auto words = [](const std::vector<u8>& b, std::vector<u32>& out) {
        out.resize(b.size() / 4);
        std::memcpy(out.data(), b.data(), out.size() * 4);
    };
    words(rows, reverbRows);
    words(signs, reverbSigns);
    words(delays, reverbDelays);
    hrtfIndex.resize(61 * 31);
    std::memcpy(hrtfIndex.data(), index.data(), index.size());
    centreGain.resize(45);
    std::memcpy(centreGain.data(), gain.data(), gain.size());
    centreCut.resize(45);
    std::memcpy(centreCut.data(), cut.data(), cut.size());
    return true;
}

void Calculate3D(u32 mask, Source& s, const ListenerFrame& lf, u32 bufferFlags, const Buffer3D& p, const Tables& t) {
    // orig: 0x0024A098
    if (mask & 1) StageDirection(s, lf, p);
    if (mask & 2) StageAngles(s, lf, p, t);
    if (mask & 4) StageDistance(s, lf, bufferFlags, p);
    if (mask & 8) StageConeAngle(s, p);
    if (mask & 0x10) StageConeAtt(s, p);
    if (mask & 0x20) StageFrontRear(s, lf, p);
    if (mask & 0x40) StageCentre(s, t);
    if (mask & 0x80) StageDoppler(s, lf, p);
}

void ListenerRightVector(ListenerFrame& lf, u32 listenerDirty, u32 algorithm) {   // orig: 0x0024984C
    if (!(listenerDirty & 0x400)) return;
    if (algorithm != 0 && algorithm != 1 && algorithm < 3) return;
    const Listener& l = *lf.listener;
    lf.right[0] = static_cast<f32>(double(l.top[1]) * l.front[2] - double(l.top[2]) * l.front[1]);
    lf.right[1] = static_cast<f32>(double(l.top[2]) * l.front[0] - double(l.top[0]) * l.front[2]);
    lf.right[2] = static_cast<f32>(double(l.top[0]) * l.front[1] - double(l.top[1]) * l.front[0]);
}

void VolumeRegisters(const VoiceSettings& v, u32 mode, const Source* s, s32 i3dl2Direct, s32 i3dl2Room, u32 hwVoices,
                     u32* out) {             // orig: 0x0024C46A
    bool is3d = false;
    s32 direct = 0, front = 0, rear = 0, centre = 0, room = 0;
    if ((v.flags & 0x10) && mode != 2) {
        const s32 fr = s->front;
        direct = s->coneAtt + s->distanceAtt;
        front = s->centreFront + i3dl2Direct + fr + direct;
        if (front >= 0) front = 0;
        rear = s->rear + i3dl2Direct + direct;
        if (rear >= 0) rear = 0;
        centre = s->centre + i3dl2Direct + fr + direct;
        if (centre >= 0) centre = 0;
        room = i3dl2Room + direct;
        if (room >= 0) room = 0;
        is3d = true;
    }
    const u32 per = v.binCount / hwVoices;
    for (u32 k = 0; k < hwVoices; ++k) {
        u32 a[8];
        for (u32 i = 0; i < 8; ++i) {
            if (i < k * per || i >= (k + 1) * per) {
                a[i] = 0xFFF;
                continue;
            }
            const u8 bin = v.bins[i];
            s32 x = -(v.binVolume[bin] + v.volume);
            if (is3d) {
                if (bin == 2) x -= s->centreFlag ? centre : direct;
                else if (bin <= 5) x -= direct;
                else if (bin <= 7) x -= front;
                else if (bin <= 9) x -= rear;
                else if (bin == 10) x -= room;
                else x -= direct;
            }
            const u32 q = (static_cast<u32>(x) << 6) / 100;   // shl 6; div 100 (unsigned)
            a[i] = q > 0xFFF ? 0xFFF : q;
        }
        out[k] = ((((a[7] & 0xF) | (a[1] << 4)) << 12 | (a[0] & 0xFFF)) << 4) | (a[6] & 0xF);
        out[3 + k] = ((((a[7] & 0xF0) | (a[3] << 8)) << 8 | (a[2] & 0xFFF)) << 4) | ((a[6] >> 4) & 0xF);
        out[6 + k] = ((((a[7] & 0xF00) | (a[5] << 12)) << 4 | (a[4] & 0xFFF)) << 4) | ((a[6] >> 8) & 0xF);
    }
}

void UnpackVolumes(u32 a, u32 b, u32 c, u16 slots[8]) {
    slots[0] = (a >> 4) & 0xFFF, slots[1] = (a >> 20) & 0xFFF;
    slots[2] = (b >> 4) & 0xFFF, slots[3] = (b >> 20) & 0xFFF;
    slots[4] = (c >> 4) & 0xFFF, slots[5] = (c >> 20) & 0xFFF;
    slots[6] = static_cast<u16>((a & 0xF) | ((b & 0xF) << 4) | ((c & 0xF) << 8));
    slots[7] = static_cast<u16>(((a >> 16) & 0xF) | (((b >> 16) & 0xF) << 4) | (((c >> 16) & 0xF) << 8));
}

void MixBinRegisters(const VoiceSettings& v, Source* s, u32& out0, u32& out1, u8* slots) {   // orig: 0x0024C395
    // (the original reads slot 4 of a 3D voice with fewer than 5 bins from uninitialised stack; here it is 0)
    u32 b[8] = {};
    if (v.flags & 0x10) s->centreFlag = 0;
    u32 n = 0;
    for (; n < v.binCount; ++n) b[n] = v.bins[n];
    if ((v.flags & 0x10) && b[4] == 2) s->centreFlag = 1;
    for (u32 cand = 0; n < 8; ++cand) {
        u32 j = 0;
        while (j < n && b[j] != cand) ++j;
        if (j >= n) b[n++] = cand;
    }
    out0 = b[0] | b[1] << 5 | b[2] << 10 | b[3] << 16 | b[4] << 21 | b[5] << 26;
    out1 = (out1 & ~0x3FFu) | b[6] | b[7] << 5;
    if (slots)
        for (u32 i = 0; i < 8; ++i) slots[i] = static_cast<u8>(b[i]);
}

u32 PitchRegister(const VoiceSettings& v, u32 mode, s32 doppler, const PitchChain* chain) {   // orig: 0x0024C667
    if (v.flags & 0x182000) return 0;
    u32 p = static_cast<u32>(v.pitch);           // (32-bit wrapping adds)
    if ((v.flags & 0x10) && mode != 2) p += static_cast<u32>(doppler);
    if (chain) {
        p += static_cast<u32>(chain->pitch);
        if ((chain->flags & 0x10) && chain->mode != 2) p += static_cast<u32>(chain->doppler);
    }
    s32 q = static_cast<s32>(p);
    if (q < -32767) q = -32767;
    else if (q > 0x1FFF) q = 0x1FFF;
    return static_cast<u32>(q) << 16;
}

s32 PitchFromRatio(f32 ratio) { return Fistp(4096.0 * std::log2(double(ratio))); }   // orig: 0x0024C0C2

s32 CalculatePitch(u32 frequency) {              // orig: 0x00246F71
    if (frequency == 48000) return 0;
    return PitchFromRatio(static_cast<f32>(U2D(frequency) * kInv48k));
}

f32 Pow10Level(s32 level) {                      // orig: 0x0024E35D
    const f32 t = static_cast<f32>(double(level) * kHalfMilli);
    const double x = double(kLog2Of10) * t;
    const double n = std::nearbyint(x);          // FRNDINT
    const f32 nf = static_cast<f32>(n);          // fst dword
    const double p = (std::exp2(x - double(nf)) - 1.0) + 1.0;   // F2XM1, fadd 1.0
    return static_cast<f32>(std::ldexp(p, static_cast<int>(n)));   // FSCALE, fmulp
}

u32 FilterCoefficient(s32 level, s32 hfLevel, f32 frequency, f32 rate) {   // orig: 0x0024E6DF
    if (level <= -10000 || hfLevel == 0) return 0;
    const double c = std::cos(double(kPi) / rate * frequency * 2.0);
    const double g = Pow10Level(static_cast<s32>(static_cast<u32>(hfLevel) * 2u));
    const double a = 1.0 - c * g;
    const double b = (1.0 - c) * g * 2.0;
    const double x = (1.0 - c * c) * g * g;
    const double coef = (a - std::sqrt(b - x)) / (1.0 - g);
    if (coef >= 1.0) return 0xFFFF;              // test ah, 1; jne
    return static_cast<u32>(Cvtt(static_cast<f32>(coef * 65536.0)));
}

void DecayFilter(s32 level, s32 hfLevel, f32 frequency, f32 rate, f32& coefficient, f32& gain) {   // orig: 0x0024E5FD
    const double w = double(kPi) / rate * frequency * 2.0;
    if (level <= -10000) {
        coefficient = 0.0f, gain = 0.0f;
        return;
    }
    if (level == 0) {
        if (hfLevel == 0) {
            coefficient = 0.0f, gain = 1.0f;
            return;
        }
    } else if (hfLevel == 0) {
        coefficient = 0.0f, gain = Pow10Level(level);
        return;
    }
    const double g1 = Pow10Level(level);
    const double g2 = Pow10Level(static_cast<s32>(static_cast<u32>(hfLevel) * 2u));
    const double c = std::cos(w);
    const double a = 1.0 - c * g2;
    const double b = (1.0 - c) * g2 * 2.0;
    const double x = (1.0 - c * c) * g2 * g2;
    const double coef = (a - std::sqrt(b - x)) / (1.0 - g2);
    coefficient = static_cast<f32>(coef);
    gain = static_cast<f32>((1.0 - coef) * g1);
}

void I3DL2SourceLevels(I3DL2Voice& o, const I3DL2Listener& l, const I3DL2Source& s, f32 distance) {   // orig: 0x0024E875
    o.dirty = 0;
    const f32 x = static_cast<f32>(double(distance) * l.roomRolloffFactor * s.roomRolloffFactor);
    const double oLF = double(s.obstructionHF) * s.obstructionLFRatio;
    const f32 cLF = static_cast<f32>(double(s.occlusionHF) * s.occlusionLFRatio);
    const s32 direct = static_cast<s32>(static_cast<u32>(Cvtt(static_cast<f32>(double(cLF) + oLF))) + static_cast<u32>(s.direct));
    if (direct != o.direct) {
        o.dirty |= 1;
        o.direct = direct;
    }
    const double pHF = (1.0 - s.obstructionLFRatio) * double(s.obstructionHF);
    const double qHF = (1.0 - s.occlusionLFRatio) * double(s.occlusionHF);
    const f32 qHFf = static_cast<f32>(qHF);
    s32 directHF = static_cast<s32>(static_cast<u32>(Cvtt(static_cast<f32>(qHF + pHF))) + static_cast<u32>(s.directHF));
    if (directHF < -10000) directHF = -10000;
    else if (directHF > 0) directHF = 0;
    f32 roomIn;
    if (!(x > 1.0f)) {                           // test ah, 0x41; jne
        roomIn = cLF;
    } else {
        const s32 lv = x > kOneE5 ? Cvtt(static_cast<f32>(Log10(x) * 2000.0)) : -10000;   // test ah, 0x41; jp
        roomIn = static_cast<f32>(double(lv) + cLF);
    }
    const s32 room = static_cast<s32>(static_cast<u32>(Cvtt(roomIn)) + static_cast<u32>(s.room));
    if (room != o.room) {
        o.dirty |= 2;
        o.room = room;
    }
    s32 roomHF = static_cast<s32>(static_cast<u32>(Cvtt(qHFf)) + static_cast<u32>(s.room));   // (sic: lRoom)
    if (roomHF < -10000) roomHF = -10000;
    else if (roomHF > 0) roomHF = 0;
    const f32 rate = 48000.0f;
    const u32 c0 = FilterCoefficient(0, directHF, l.hfReference, rate);
    if (c0 != o.directHFCoef) {
        o.dirty |= 4;
        o.directHFCoef = c0;
    }
    const u32 c1 = FilterCoefficient(0, roomHF, l.hfReference, rate);
    if (c1 != o.roomHFCoef) {
        o.dirty |= 8;
        o.roomHFCoef = c1;
    }
}

// ---- reverb conversion (word indices = converter byte offset / 4) -------------------------------------------------
void ReverbResetLengths(u32* c) {                // orig: 0x0024E5C6
    for (u32 i = 0; i < 14; ++i) c[0x2C + 2 * i] = ((c[0x2C + 2 * i] - 1) >> 2) + 1;
    c[0x6A] = 4;
    for (u32 i = 0; i < 10; ++i) c[0x6B + 3 * i] = 0;
}

namespace {

// orig: 0x0024E7CA (out[1], out[2] of a 3-word record)
void DecayRecord(u32* out, u32 length, f32 decay, f32 hfRatio, f32 hfReference) {
    f32 x = static_cast<f32>(U2D(length) * kInv24k / decay * -6000.0);
    const s32 a = Cvtt(x);
    if (hfRatio < 1.0f) x = static_cast<f32>(double(x) / hfRatio);   // test ah, 5; jp
    const s32 b = Cvtt(x);
    f32 c, g;
    DecayFilter(a, static_cast<s32>(static_cast<u32>(b) - static_cast<u32>(a)), hfReference, 24000.0f, c, g);
    out[1] = static_cast<u32>(Cvtt(static_cast<f32>(8388608.0 * c)));
    out[2] = static_cast<u32>(Cvtt(static_cast<f32>(8388608.0 * g)));
}

u32 Round4(u32 v) { return (v + 1) & ~3u; }

}  // namespace

void ConvertI3DL2Reverb(const I3DL2Listener& l, u32* c, const Tables& tables) {   // orig: 0x0024EAB2
    const f32 diffusion = static_cast<f32>(double(l.diffusion) * kCentiF);
    const double v = (2.0 - double(l.density) * kCentiF) * ((double(l.decayTime) - 1.0) * 5.0);
    double sv = v;
    f32 vf = static_cast<f32>(v);
    if (v < 1.0) vf = 1.0f, sv = 1.0;            // test ah, 5; jp
    if (sv > 60.0) vf = 60.0f;                   // test ah, 0x41; jne
    ReverbResetLengths(c);
    {                                            // orig: 0x0024E76D
        f32 coef, gain;
        DecayFilter(0, l.roomHF, l.hfReference, 48000.0f, coef, gain);
        c[0x6C] = static_cast<u32>(Cvtt(static_cast<f32>(8388608.0 * coef)));
        c[0x6D] = static_cast<u32>(Cvtt(static_cast<f32>(8388608.0 * gain)));
    }
    {                                            // orig: 0x0024E43B
        const f32 r = Pow10Level(static_cast<s32>(static_cast<u32>(l.reflections) + static_cast<u32>(l.room)));
        for (u32 i = 0; i < 4; ++i)
            c[0x64 + i] = static_cast<u32>(Cvtt(static_cast<f32>(65536.0 * Tab(tables.reverbRows, 5 * i) * r)));
    }
    c[0x47] = static_cast<u32>(Cvtt(static_cast<f32>(double(l.reflectionsDelay) * 24000.0)));   // orig: 0x0024E46E
    {                                            // orig: 0x0024E48A
        const f32 g = Pow10Level(static_cast<s32>(static_cast<u32>(l.reverb) + static_cast<u32>(l.room)));
        c[0x68] = static_cast<u32>(Cvtt(static_cast<f32>(65536.0 * g)));
        c[0x69] = static_cast<u32>(Cvtt(static_cast<f32>(65536.0 * g)));
    }
    {                                            // orig: 0x0024E4BF
        c[0x4C] = static_cast<u32>(Cvtt(static_cast<f32>(double(l.reverbDelay) * 24000.0)));
        for (u32 i = 0; i < 4; ++i)
            c[0x48 + i] = static_cast<u32>(Cvtt(static_cast<f32>(U2D(c[0x4C]) * Tab(tables.reverbRows, 2 + 5 * i))));
        for (u32 i = 0; i < 4; ++i)
            c[0x55 + i] = static_cast<u32>(Cvtt(static_cast<f32>(
                (double(l.reverbDelay) * Tab(tables.reverbRows, 4 + 5 * i) + Tab(tables.reverbRows, 3 + 5 * i)) * 24000.0)));
    }
    {                                            // orig: 0x0024E3B6
        const f32 t = vf < 2.5f ? static_cast<f32>((double(vf) - kPoint8) * kDecayScale) : 1.0f;
        u32 w = 0x4D;
        for (u32 col = 0; col < 4; ++col)
            for (u32 row = 0; row < 2; ++row)
                c[w++] = static_cast<u32>(Cvtt(static_cast<f32>(double(t) * Tab(tables.reverbDelays, col + 4 * row) * 24000.0)));
        c[0x59] = static_cast<u32>(Cvtt(static_cast<f32>(double(t) * 24000.0)));
    }
    {                                            // orig: 0x0024E9DB
        u32 rec[3] = {};
        DecayRecord(rec, c[0x59] >> 2, l.decayTime, l.decayHFRatio, l.hfReference);
        for (u32 i = 0; i < 3; ++i) c[0x6F + 3 * i] = rec[1], c[0x70 + 3 * i] = rec[2];
        DecayRecord(rec, c[0x59], l.decayTime, l.decayHFRatio, l.hfReference);
        for (u32 i = 0; i < 2; ++i) c[0x84 + 3 * i] = rec[1], c[0x85 + 3 * i] = rec[2];
        for (u32 i = 0; i < 4; ++i) DecayRecord(&c[0x77 + 3 * i], c[0x3C + 2 * i], l.decayTime, l.decayHFRatio, l.hfReference);
    }
    {                                            // orig: 0x0024E54F
        for (u32 k = 0; k < 8; ++k)
            c[0x5A + k] = static_cast<u32>(Cvtt(static_cast<f32>(8388608.0 * Tab(tables.reverbSigns, k) * diffusion)));
        c[0x63] = static_cast<u32>(Cvtt(static_cast<f32>(8388608.0 * diffusion * kFifth)));
        c[0x62] = static_cast<u32>(Cvtt(static_cast<f32>((1.0 - diffusion) * 8388608.0)));
    }
    for (u32 i = 0; i < 14; ++i) c[0x2C + 2 * i] = Round4(c[0x2C + 2 * i]);
    for (u32 i = 0; i < 5; ++i) c[0x47 + i] = Round4(c[0x47 + i]);
    for (u32 i = 0; i < 8; ++i) c[0x4D + i] = Round4(c[0x4D + i]);
    c[0x4C] = Round4(c[0x4C]);
    for (u32 i = 0; i < 4; ++i) c[0x55 + i] = Round4(c[0x55 + i]);
    c[0x59] = Round4(c[0x59]);
    for (u32 i = 0; i < 14; ++i) --c[0x2C + 2 * i];
    for (u32 i = 0; i < 4; ++i) c[0x48 + i] += c[0x47];
    c[0x4C] += c[0x47];
    u32 prev = 0;
    for (u32 i = 0; i < 4; ++i) {
        u32* p = &c[0x4D + 2 * i];
        const u32 c0 = p[0], c1 = p[1];
        const u32 span = (c0 > c1 ? c0 : c1) - prev;
        const u32 lim = c[0x2C + 2 * i];
        if (span > lim) {
            const f32 sc = static_cast<f32>(U2D(lim) / U2D(span));
            p[0] = static_cast<u32>(Cvtt(static_cast<f32>(U2D(c0 - prev) * sc)));
            p[1] = static_cast<u32>(Cvtt(static_cast<f32>(U2D(c1 - prev) * sc)));
            p[0] = Round4(p[0]);
            p[1] = Round4(p[1]);
        } else {
            p[0] = c0 - prev;
            p[1] = c1 - prev;
        }
        prev = c1;
    }
    for (u32 i = 0; i < 4; ++i)
        if (c[0x55 + i] > c[0x34 + 2 * i]) c[0x55 + i] = c[0x34 + 2 * i];
    if (c[0x59] > c[0x44]) c[0x59] = c[0x44];
    for (u32 k = 0; k < 8; ++k) {
        const s32 x = static_cast<s32>(c[0x5A + k]);
        if (x > 0x7FFFFF) c[0x5A + k] = 0x7FFFFF;
        else if (x < -0x7FFFFF) c[0x5A + k] = static_cast<u32>(-0x7FFFFF);
    }
    for (u32 i = 0; i < 5; ++i) c[0x47 + i] <<= 2;
    c[0x4C] <<= 2;
    for (u32 i = 0; i < 8; ++i) c[0x4D + i] <<= 2;
    for (u32 i = 0; i < 4; ++i) c[0x55 + i] <<= 2;
    c[0x59] <<= 2;
}

}  // namespace fut::audio::xds

// The XMV library's WMV2 pixel kernels (default.xbe section XMV), written by hand from what each MMX routine
// computes. Every kernel is integer-exact against the original: the MMX lane widths are kept where they matter
// (16-bit wrap of PADDW, PMADDWD on int16 operands, PACKSSDW / PACKUSWB saturation, PAVGB rounding up).
//
// Coefficient layout used by the transforms (as stored by the XMV block decoders): index b = u * 8 + v, u the
// horizontal and v the vertical frequency (the scan tables at 0x0026CA00.. hold these transposed positions).
// The 8 x 8 inverse transform is the WMV2 one (W_k = 2048 * sqrt(2) * cos(k * pi / 16)): a horizontal pass with
// rounding 128 >> 8, then a vertical pass with (+4) >> 3 inputs and rounding 8192 >> 14.
//
// Constants that the original keeps in data (the IDCT word pairs, the 181 butterfly factor, the rounding words, the
// mspel taps) are read from the user's default.xbe through XmvImage; nothing of the image is embedded here. The
// constants that are instruction immediates in the scalar ABT routines are written as named values.
#include "Wmv2Kernels.h"

#include "XmvImage.h"

#include <algorithm>
#include <cstring>

namespace xmv::kernels {
namespace {

// ---- constants read from the image ----------------------------------------------------------------------------------

// One PMADDWD constant pair (x, sum): the original forms products as cx * x + cs * int16(x + y), so that
// cx * x + cs * (x + y) = (cx + cs) * x + cs * y.
struct Pair {
    int32_t cx = 0;
    int32_t cs = 0;
};

// The IDCT table of one transform routine (0x0026C690 for the intra IDCT, 0x0026C6D8 for the inter IDCT).
struct IdctConsts {
    int32_t round = 0;     // +0x00: 128 (the row pass rounding; the column pass uses it << 6 = 8192)
    int32_t k181 = 0;      // +0x08: 181 = 256 / sqrt(2)
    Pair a6, a2, a3, a5, a7, a1;   // +0x18 .. +0x40
};

struct Consts {
    IdctConsts intra;
    IdctConsts inter;
    int32_t mspelTap = 9;      // 0x0026C7A0 (put) / 0x0026C7B0 (add): the two centre taps
    int32_t mspelRound = 8;    // 0x0026C798 / 0x0026C7A8
    bool loaded = false;
};

Consts g;

int16_t Word(const XmvImage& image, uint32_t va) {
    int16_t v;
    std::memcpy(&v, image.At(va), 2);
    return v;
}

IdctConsts ReadIdct(const XmvImage& image, uint32_t base) {
    IdctConsts c;
    c.round = Word(image, base + 0x00);
    c.k181 = Word(image, base + 0x08);
    auto pair = [&](uint32_t off) { return Pair{Word(image, base + off), Word(image, base + off + 2)}; };
    c.a6 = pair(0x18);
    c.a2 = pair(0x20);
    c.a3 = pair(0x28);
    c.a5 = pair(0x30);
    c.a7 = pair(0x38);
    c.a1 = pair(0x40);
    return c;
}

// ---- integer helpers ------------------------------------------------------------------------------------------------

inline int16_t Wrap16(int32_t v) { return int16_t(uint16_t(uint32_t(v))); }
inline int16_t Sat16(int32_t v) { return int16_t(std::clamp(v, -32768, 32767)); }
inline uint8_t SatU8(int32_t v) { return uint8_t(std::clamp(v, 0, 255)); }
inline int32_t Wrap32(int64_t v) { return int32_t(uint32_t(uint64_t(v))); }

// cx * x + cs * int16(x + y): one PMADDWD lane over the word pair (x, x + y).
inline int32_t Madd(Pair c, int16_t x, int16_t y) { return c.cx * x + c.cs * Wrap16(int32_t(x) + y); }

// 181 * v as the original computes it: PMADDWD on the low 15 bits plus PMADDWD on bits 15..30 (as a signed word)
// shifted back by 15, all modulo 2^32. Equals 181 * v for |v| < 2^30.
inline int32_t Mul181(int32_t k181, int32_t v) {
    const uint32_t u = uint32_t(v);
    const int32_t lo = int32_t(u & 0x7FFF) * k181;
    const int32_t hi = int32_t(int16_t(uint16_t(u >> 15))) * k181;
    return int32_t(uint32_t(hi) << 15) + lo;   // wraps as PSLLD / PADDD do
}

// The eight outputs of one WMV2 butterfly, before the final shift. `x` are the eight inputs (int16), a0 / a4 come
// in already scaled and rounded.
struct Butterfly {
    int32_t out[8];
};

Butterfly Combine(const IdctConsts& c, int32_t a0, int32_t a4, int32_t a1, int32_t a7, int32_t a5, int32_t a3,
                  int32_t a2, int32_t a6) {
    const int32_t b15 = a1 + a5, d15 = a1 - a5;
    const int32_t b37 = a3 + a7, d73 = a7 - a3;
    const int32_t s1 = (Mul181(c.k181, d73 + d15) + c.round) >> 8;
    const int32_t s2 = (Mul181(c.k181, d15 - d73) + c.round) >> 8;
    Butterfly r;
    r.out[0] = a0 + a2 + b15;
    r.out[1] = a4 + a6 + s1;
    r.out[2] = a4 - a6 + s2;
    r.out[3] = a0 - a2 + b37;
    r.out[4] = a0 - a2 - b37;
    r.out[5] = a4 - a6 - s2;
    r.out[6] = a4 + a6 - s1;
    r.out[7] = a0 + a2 - b15;
    return r;
}

// Horizontal pass (orig: the first loop of 0x002676C6 / 0x00267B33): x[0..7] are the coefficients u = 0..7 of one
// v; returns the eight int16 row samples (PACKSSDW of (sum + 128) >> 8).
void RowPass(const IdctConsts& c, const int16_t x[8], int16_t out[8]) {
    const int32_t a1 = Madd(c.a1, x[1], x[7]);
    const int32_t a7 = Madd(c.a7, x[7], x[1]);
    const int32_t a5 = Madd(c.a5, x[5], x[3]);
    const int32_t a3 = Madd(c.a3, x[3], x[5]);
    const int32_t a2 = Madd(c.a2, x[2], x[6]);
    const int32_t a6 = Madd(c.a6, x[6], x[2]);
    const int32_t a0 = x[0] * 2048 + c.round;   // (x << 16) >> 5: int16 * 2048, plus the rounding
    const int32_t a4 = a0 - x[4] * 2048;
    const int32_t a0s = a0 + x[4] * 2048;
    const Butterfly b = Combine(c, a0s, a4, a1, a7, a5, a3, a2, a6);
    for (int i = 0; i < 8; ++i) out[i] = Sat16(b.out[i] >> 8);
}

// Vertical pass (orig: the second loop): x[0..7] are the row-pass samples of one column; returns the 32-bit sums
// shifted by 14 (the callers saturate).
void ColPass(const IdctConsts& c, const int16_t x[8], int32_t out[8]) {
    const int32_t a1 = (Madd(c.a1, x[1], x[7]) + 4) >> 3;
    const int32_t a7 = (Madd(c.a7, x[7], x[1]) + 4) >> 3;
    const int32_t a5 = (Madd(c.a5, x[5], x[3]) + 4) >> 3;
    const int32_t a3 = (Madd(c.a3, x[3], x[5]) + 4) >> 3;
    const int32_t a2 = (Madd(c.a2, x[2], x[6]) + 4) >> 3;
    const int32_t a6 = (Madd(c.a6, x[6], x[2]) + 4) >> 3;
    const int32_t bias = c.round << 6;          // 8192
    const int32_t a0 = x[0] * 256 + bias;       // (x << 16) >> 8
    const int32_t a0s = a0 + x[4] * 256;
    const int32_t a4 = a0 - x[4] * 256;
    const Butterfly b = Combine(c, a0s, a4, a1, a7, a5, a3, a2, a6);
    for (int i = 0; i < 8; ++i) out[i] = b.out[i] >> 14;
}

// The mspel 4-tap filter (-1, 9, 9, -1) with rounding 8 >> 4, in 16-bit lanes, saturated to 0..255.
inline uint8_t Tap4(int32_t m1, int32_t p0, int32_t p1, int32_t p2) {
    const int32_t v = int16_t(g.mspelTap * (p0 + p1) - (m1 + p2) + g.mspelRound) >> 4;
    return SatU8(v);
}
inline uint8_t Avg(uint8_t a, uint8_t b) { return uint8_t((a + b + 1) >> 1); }   // PAVGB

// Horizontal lowpass of one row at columns 0..7 (reads src[-1] .. src[9]).
inline void HLow(const uint8_t* src, uint8_t out[8]) {
    for (int x = 0; x < 8; ++x) out[x] = Tap4(src[x - 1], src[x], src[x + 1], src[x + 2]);
}
// Vertical lowpass of 8 columns with the given byte stride (reads rows -1 .. 2).
inline void VLow(const uint8_t* src, int32_t stride, uint8_t out[8]) {
    for (int x = 0; x < 8; ++x) out[x] = Tap4(src[x - stride], src[x], src[x + stride], src[x + 2 * stride]);
}

// The 8 x 8 mspel prediction (orig: 0x002697F2 put / 0x00269E5F add share the cases; `addCase` selects the add
// routine's reading of fx = fy = 0 with hshift set, which the put routine treats as a plain copy).
void MspelPredict(const uint8_t* src, int32_t stride, int32_t fx, int32_t fy, int32_t hshift, bool addCase,
                  uint8_t pred[64]) {
    if (!fy) {
        if (!fx) {
            for (int y = 0; y < 8; ++y) {
                const uint8_t* s = src + y * stride;
                if (hshift && addCase) {
                    uint8_t h[8];
                    HLow(s, h);
                    for (int x = 0; x < 8; ++x) pred[y * 8 + x] = Avg(h[x], s[x]);
                } else {
                    std::memcpy(pred + y * 8, s, 8);
                }
            }
            return;
        }
        for (int y = 0; y < 8; ++y) {                   // fx = 1: the horizontal half-pel position
            const uint8_t* s = src + y * stride;
            uint8_t h[8];
            HLow(s, h);
            for (int x = 0; x < 8; ++x) pred[y * 8 + x] = hshift ? Avg(h[x], s[x + 1]) : h[x];
        }
        return;
    }
    // fy = 1. halfH: the horizontal lowpass of the source rows -1..9 (11 rows), used by the 2-D cases.
    uint8_t halfH[11 * 8];
    const bool needH = fx || hshift;
    if (needH)
        for (int r = 0; r < 11; ++r) HLow(src + (r - 1) * stride, halfH + r * 8);
    for (int y = 0; y < 8; ++y) {
        uint8_t v[8], hv[8];
        if (!fx && !hshift) {
            VLow(src + y * stride, stride, v);          // the vertical half-pel position
            std::memcpy(pred + y * 8, v, 8);
            continue;
        }
        VLow(halfH + (y + 1) * 8, 8, hv);               // the 2-D lowpass
        if (!hshift) {                                  // fx = 1, hshift 0
            std::memcpy(pred + y * 8, hv, 8);
            continue;
        }
        VLow(src + y * stride + fx, stride, v);         // hshift: average with the vertical lowpass at x + fx
        for (int x = 0; x < 8; ++x) pred[y * 8 + x] = Avg(v[x], hv[x]);
    }
}

// The half-pel bilinear prediction (orig: 0x00268E15 / 0x0026940E rounding, 0x00269096 / 0x0026960D no rounding).
void BilinearPredict(int32_t rnd, const uint8_t* src, int32_t stride, int32_t fx, int32_t fy, uint8_t pred[64]) {
    for (int y = 0; y < 8; ++y) {
        const uint8_t* a = src + y * stride;
        const uint8_t* b = a + stride;
        for (int x = 0; x < 8; ++x) {
            int32_t v;
            if (!fx && !fy) v = a[x];
            else if (fx && !fy) v = (a[x] + a[x + 1] + (rnd ? 0 : 1)) >> 1;
            else if (!fx) v = (a[x] + b[x] + (rnd ? 0 : 1)) >> 1;
            else v = (a[x] + a[x + 1] + b[x] + b[x + 1] + (rnd ? 1 : 2)) >> 2;
            pred[y * 8 + x] = uint8_t(v);
        }
    }
}

// Prediction + residual: PADDW (16-bit wrap) then PACKUSWB.
void AddResidual(const uint8_t pred[64], const int16_t* residual, uint8_t* dst, int32_t dstStride) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            dst[y * dstStride + x] = SatU8(Wrap16(int32_t(pred[y * 8 + x]) + residual[y * 8 + x]));
}

void Store(const uint8_t pred[64], uint8_t* dst, int32_t dstStride) {
    for (int y = 0; y < 8; ++y) std::memcpy(dst + y * dstStride, pred + y * 8, 8);
}

} // namespace

void SetImage(const XmvImage* image) {
    if (!image || !image->Loaded()) {
        g.loaded = false;
        return;
    }
    g.intra = ReadIdct(*image, 0x0026C690);
    g.inter = ReadIdct(*image, 0x0026C6D8);
    g.mspelTap = Word(*image, 0x0026C7A0);
    g.mspelRound = Word(*image, 0x0026C798);
    g.loaded = true;
}

// orig: 0x002651AB.
void Yuy2Block16(const uint8_t* y, const uint8_t* u, const uint8_t* v, int32_t yStride, int32_t cStride, uint8_t* dst,
                 int32_t dstPitch) {
    for (int row = 0; row < 16; ++row) {
        const uint8_t* ys = y + row * yStride;
        const uint8_t* us = u + (row >> 1) * cStride;
        const uint8_t* vs = v + (row >> 1) * cStride;
        uint8_t* d = dst + row * dstPitch;
        for (int k = 0; k < 8; ++k) {
            d[4 * k + 0] = ys[2 * k];
            d[4 * k + 1] = us[k];
            d[4 * k + 2] = ys[2 * k + 1];
            d[4 * k + 3] = vs[k];
        }
    }
}

// orig: 0x002664CD.
void CopyMb(const uint8_t* refY, const uint8_t* refU, const uint8_t* refV, uint8_t* dstY, uint8_t* dstU, uint8_t* dstV,
            int32_t yStride, int32_t cStride) {
    for (int r = 0; r < 16; ++r) std::memcpy(dstY + r * yStride, refY + r * yStride, 16);
    for (int r = 0; r < 8; ++r) {
        std::memcpy(dstU + r * cStride, refU + r * cStride, 8);
        std::memcpy(dstV + r * cStride, refV + r * cStride, 8);
    }
}

// orig: 0x00266536. Each output row is the 16 source bytes with the edge byte replicated over the `shift` bytes
// that fall outside the picture: mode > 0 reads src[min(i + shift, 15)] (right edge), mode < 0 src[max(i - shift,
// 0)] (left edge). The last row written is repeated until 11 rows are filled (with negative strides the buffer is
// filled bottom-up, which replicates the top edge row).
void EdgeCopy(const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t shift, int32_t mode,
              int32_t rows) {
    uint8_t row[16] = {};
    for (int r = 0; r < rows; ++r) {
        const uint8_t* s = src + r * srcStride;
        for (int i = 0; i < 16; ++i) {
            if (mode > 0) row[i] = s[std::min(i + shift, 15)];
            else if (mode < 0) row[i] = s[std::max(i - shift, 0)];
            else row[i] = s[i];
        }
        std::memcpy(dst, row, 16);
        dst += dstStride;
    }
    for (int r = rows; r < 11; ++r) {
        std::memcpy(dst, row, 16);
        dst += dstStride;
    }
}

// orig: 0x002676C6. coef: 64 int32 in the b = u * 8 + v layout; only their low 16 bits enter the transform.
void IdctIntra(uint8_t* dst, int32_t stride, const int32_t* coef) {
    int16_t rows[8][8];   // rows[v][x]
    for (int v = 0; v < 8; ++v) {
        int16_t x[8];
        for (int u = 0; u < 8; ++u) x[u] = Wrap16(coef[u * 8 + v]);
        RowPass(g.intra, x, rows[v]);
    }
    for (int x = 0; x < 8; ++x) {
        int16_t col[8];
        int32_t out[8];
        for (int v = 0; v < 8; ++v) col[v] = rows[v][x];
        ColPass(g.intra, col, out);
        for (int y = 0; y < 8; ++y) dst[y * stride + x] = SatU8(Sat16(out[y]));   // PACKSSDW, PACKUSWB
    }
}

// orig: 0x00267B33. coef: 64 int16 (b = u * 8 + v); out: 8 rows of 8 int16 residuals. `mask` bit v is set when the
// coefficients of row v include one with u >= 1. A pair of rows (v, v + 1) with neither bit set takes the original's
// shortcut: every sample of the row becomes the DC term times 8 in 16-bit arithmetic (PSLLW 3, wrapping), which
// equals the full pass except where the full pass would saturate.
void IdctInter(int16_t* out, const int16_t* coef, uint32_t mask) {
    int16_t rows[8][8];
    for (int v = 0; v < 8; v += 2) {
        const bool shortcut = ((mask >> v) & 3) == 0;
        for (int k = v; k < v + 2; ++k) {
            if (shortcut) {
                const int16_t dc8 = Wrap16(int32_t(coef[k]) << 3);
                for (int x = 0; x < 8; ++x) rows[k][x] = dc8;
                continue;
            }
            int16_t x[8];
            for (int u = 0; u < 8; ++u) x[u] = coef[u * 8 + k];
            RowPass(g.inter, x, rows[k]);
        }
    }
    for (int x = 0; x < 8; ++x) {
        int16_t col[8];
        int32_t res[8];
        for (int v = 0; v < 8; ++v) col[v] = rows[v][x];
        ColPass(g.inter, col, res);
        for (int y = 0; y < 8; ++y) out[y * 8 + x] = Sat16(res[y]);   // PACKSSDW
    }
}

// The ABT constants (instruction immediates in 0x002681B5 / 0x0026842C): the 8-point stage uses the WMV2 IDCT
// weights in the same factored form as the MMX table (W1 - W7, W7, ...), the 4-point stage 2048 * cos(pi / 4),
// 2048 * cos(pi / 8), 2048 * sin(pi / 8).
namespace abt {
constexpr int32_t kW7 = 565, kW1mW7 = 2276, kW1pW7 = 3406;   // W1 = 2841
constexpr int32_t kW3 = 2408, kW3mW5 = 799, kW3pW5 = 4017;   // W5 = 1609
constexpr int32_t kW6 = 1108, kW2mW6 = 1568, kW2pW6 = 3784;  // W2 = 2676
constexpr int32_t kW0 = 2048, k181 = 181;
constexpr int32_t kC4 = 1448, kC2 = 1892, kC6 = 784;

// One 8-point inverse transform with the given pre-shift on the odd / even products, rounding and final shift
// (the row form: no pre-shift, +128 in a0, >> 8; the column form: (+4) >> 3, +8192 in a0, >> 14).
void Idct8(const int32_t x[8], int32_t out[8], bool column) {
    auto pre = [column](int32_t v) { return column ? (v + 4) >> 3 : v; };
    const int32_t t17 = (x[1] + x[7]) * kW7;
    const int32_t a1 = pre(x[1] * kW1mW7 + t17);
    const int32_t a7 = pre(t17 - x[7] * kW1pW7);
    const int32_t t35 = (x[5] + x[3]) * kW3;
    const int32_t a5 = pre(t35 - x[5] * kW3mW5);
    const int32_t a3 = pre(t35 - x[3] * kW3pW5);
    const int32_t t26 = (x[6] + x[2]) * kW6;
    const int32_t a6 = pre(t26 - x[6] * kW2pW6);
    const int32_t a2 = pre(x[2] * kW2mW6 + t26);
    int32_t a0, a4;
    if (column) {
        const int32_t base = (x[0] + 0x20) * 0x100;
        a0 = base + x[4] * 0x100;
        a4 = base - x[4] * 0x100;
    } else {
        const int32_t base = x[0] * kW0 + 0x80;
        a0 = base + x[4] * kW0;
        a4 = base - x[4] * kW0;
    }
    const int32_t b15 = a1 + a5, d15 = a1 - a5, b37 = a3 + a7, d73 = a7 - a3;
    const int32_t s1 = ((d73 + d15) * k181 + 0x80) >> 8;
    const int32_t s2 = ((d15 - d73) * k181 + 0x80) >> 8;
    const int shift = column ? 14 : 8;
    out[0] = (a0 + a2 + b15) >> shift;
    out[1] = (a4 + a6 + s1) >> shift;
    out[2] = (a4 - a6 + s2) >> shift;
    out[3] = (a0 - a2 + b37) >> shift;
    out[4] = (a0 - a2 - b37) >> shift;
    out[5] = (a4 - a6 - s2) >> shift;
    out[6] = (a4 + a6 - s1) >> shift;
    out[7] = (a0 + a2 - b15) >> shift;
}

// One 4-point inverse transform: (x0 +- x2) * C4 and the rotation (C2, C6) of x1 / x3, plus `round`, >> `shift`.
void Idct4(const int32_t x[4], int32_t out[4], int32_t round, int shift) {
    const int32_t e0 = (x[0] + x[2]) * kC4;
    const int32_t e1 = (x[0] - x[2]) * kC4;
    const int32_t o0 = x[1] * kC2 + x[3] * kC6;
    const int32_t o1 = x[1] * kC6 - x[3] * kC2;
    out[0] = (e0 + o0 + round) >> shift;
    out[1] = (e1 + o1 + round) >> shift;
    out[2] = (e1 - o1 + round) >> shift;
    out[3] = (e0 - o0 + round) >> shift;
}
} // namespace abt

// orig: 0x002681B5. coef: 4 rows of 8 int16 (row-major, from the 0x0026CB00 scan); writes rows 4 * half .. + 3 of
// `out` (8 x 8 int16): an 8-point horizontal pass (stored as int16, no saturation), then a 4-point vertical pass
// with rounding 0x8000 >> 16.
void Abt8x4(int16_t* out, const int16_t* coef, int32_t half) {
    int16_t* o = out + half * 32;
    for (int r = 0; r < 4; ++r) {
        int32_t x[8], y[8];
        for (int k = 0; k < 8; ++k) x[k] = coef[r * 8 + k];
        abt::Idct8(x, y, false);
        for (int k = 0; k < 8; ++k) o[r * 8 + k] = Wrap16(y[k]);
    }
    for (int c = 0; c < 8; ++c) {
        int32_t x[4], y[4];
        for (int r = 0; r < 4; ++r) x[r] = o[r * 8 + c];
        abt::Idct4(x, y, 0x8000, 16);
        for (int r = 0; r < 4; ++r) o[r * 8 + c] = Wrap16(y[r]);
    }
}

// orig: 0x0026842C. coef: 8 rows of 4 int16 (from the 0x0026CB24 scan); writes columns 4 * half .. + 3 of `out`:
// a 4-point horizontal pass with rounding 0x40 >> 7, then an 8-point vertical pass in the column form.
void Abt4x8(int16_t* out, const int16_t* coef, int32_t half) {
    int16_t* o = out + half * 4;
    for (int r = 0; r < 8; ++r) {
        int32_t x[4], y[4];
        for (int k = 0; k < 4; ++k) x[k] = coef[r * 4 + k];
        abt::Idct4(x, y, 0x40, 7);
        for (int k = 0; k < 4; ++k) o[r * 8 + k] = Wrap16(y[k]);
    }
    for (int c = 0; c < 4; ++c) {
        int32_t x[8], y[8];
        for (int r = 0; r < 8; ++r) x[r] = o[r * 8 + c];
        abt::Idct8(x, y, true);
        for (int r = 0; r < 8; ++r) o[r * 8 + c] = Wrap16(y[r]);
    }
}

// orig: 0x00268E15 (rnd 0) / 0x00269096 (rnd 1).
void McCopy(int32_t rnd, const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t fx,
            int32_t fy) {
    uint8_t pred[64];
    BilinearPredict(rnd, src, srcStride, fx, fy, pred);
    Store(pred, dst, dstStride);
}

// orig: 0x0026940E (rnd 0) / 0x0026960D (rnd 1).
void McAdd(int32_t rnd, const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t fx, int32_t fy,
           const int16_t* residual) {
    uint8_t pred[64];
    BilinearPredict(rnd, src, srcStride, fx, fy, pred);
    AddResidual(pred, residual, dst, dstStride);
}

// orig: 0x002697F2.
void MspelCopy(const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t fx, int32_t fy,
               int32_t hshift) {
    uint8_t pred[64];
    MspelPredict(src, srcStride, fx, fy, hshift, false, pred);
    Store(pred, dst, dstStride);
}

// orig: 0x00269E5F.
void MspelAdd(const uint8_t* src, int32_t srcStride, uint8_t* dst, int32_t dstStride, int32_t fx, int32_t fy,
              int32_t hshift, const int16_t* residual) {
    uint8_t pred[64];
    MspelPredict(src, srcStride, fx, fy, hshift, true, pred);
    AddResidual(pred, residual, dst, dstStride);
}

} // namespace xmv::kernels

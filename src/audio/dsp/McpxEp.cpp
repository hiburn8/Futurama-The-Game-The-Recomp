#include "audio/dsp/McpxEp.h"

#include "audio/dsp/Dsp56300.h"

#include <cstring>

namespace fut::audio::dsp {

namespace {

u32 U32At(const std::vector<u8>& v, size_t o) {
    u32 x = 0;
    if (o + 4 <= v.size()) std::memcpy(&x, &v[o], 4);
    return x;
}

// File offset of an XBE virtual address with `n` bytes behind it in the same section, or 0.
size_t XbeOffset(const std::vector<u8>& xbe, u32 va, u32 n) {
    if (xbe.size() < 0x200 || std::memcmp(xbe.data(), "XBEH", 4) != 0) return 0;
    const u32 base = U32At(xbe, 0x104), count = U32At(xbe, 0x11C), headers = U32At(xbe, 0x120);
    for (u32 i = 0; i < count; ++i) {
        const size_t h = size_t(headers - base) + size_t(i) * 0x38;
        const u32 sva = U32At(xbe, h + 4), raw = U32At(xbe, h + 12), rsize = U32At(xbe, h + 16);
        if (va >= sva && size_t(va - sva) + n <= rsize && size_t(raw) + rsize <= xbe.size()) return raw + (va - sva);
    }
    return 0;
}

// An overlay module {0xD01B40, size, index << 16 | 0x172 (load address), checksum} + its code words. Runs the
// coefficient set-up from P:0x19C (pairs `move #c,x0` 44F400 c / `move x0,y:$k` 4C7000 k) into y[0..count).
bool ReadCoefficients(const std::vector<u8>& xbe, u32 va, u32 index, u32* y, u32 count) {
    const size_t o = XbeOffset(xbe, va, 16);
    if (!o) return false;
    const u32 size = U32At(xbe, o + 4);
    if (U32At(xbe, o) != 0xD01B40 || U32At(xbe, o + 8) != (index << 16 | 0x172) || size > 0x1000 ||
        !XbeOffset(xbe, va, 16 + 4 * size))
        return false;
    auto word = [&](u32 p) { return p >= 0x172 && p < 0x172 + size ? U32At(xbe, o + 16 + 4 * size_t(p - 0x172)) : 0xFFFFFFFFu; };
    u32 x0 = 0, seen = 0;
    for (u32 p = 0x19C;; p += 2) {
        const u32 w = word(p);
        if (w == 0x44F400) {
            x0 = word(p + 1) & kMask24;          // move #c,x0
        } else if (w == 0x4C7000) {              // move x0,y:$k
            const u32 k = word(p + 1);
            if (k >= count) return false;
            y[k] = x0, seen |= 1u << k;
        } else {
            break;                               // move x:$700,r2: the end of the set-up
        }
    }
    return seen == (1u << count) - 1;
}

u64 Mac(u64 acc, s32 x, u32 y) { return (acc + FracProduct(static_cast<u32>(x) & kMask24, y)) & kMask56; }

}  // namespace

bool McpxEp::LoadFromXbe(const std::vector<u8>& xbe, std::string* error) {
    loaded_ = ReadCoefficients(xbe, kStereoModuleVa, 6, stereo_, 10) && ReadCoefficients(xbe, kMonoModuleVa, 5, mono_, 5);
    if (!loaded_ && error) *error = "default.xbe: EP PCM modules 5 / 6 not found";
    return loaded_;
}

void McpxEp::Stereo(const s32* fl, const s32* c, const s32* fr, const s32* bl, const s32* br, u32 n, s32* outL,
                    s32* outR) const {
    const u32* y = stereo_;
    for (u32 i = 0; i < n; ++i) {
        // module 6, P:0x1D7-0x1E6: the two sums interleaved, the last term with rounding (macr)
        u64 a = FracProduct(static_cast<u32>(fl[i]) & kMask24, y[0]);
        u64 b = FracProduct(static_cast<u32>(fl[i]) & kMask24, y[1]);
        a = Mac(a, c[i], y[2]), b = Mac(b, c[i], y[3]);
        a = Mac(a, fr[i], y[4]), b = Mac(b, fr[i], y[5]);
        a = Mac(a, bl[i], y[6]), b = Mac(b, bl[i], y[7]);
        a = RoundConvergent(Mac(a, br[i], y[8]));
        b = RoundConvergent(Mac(b, br[i], y[9]));
        outL[i] = Sext24(Limit24(a));            // move a,x:(r2)+ / move b,x:(r6)+ through the limiter
        outR[i] = Sext24(Limit24(b));
    }
}

void McpxEp::Mono(const s32* fl, const s32* c, const s32* fr, const s32* bl, const s32* br, u32 n, s32* out) const {
    const u32* y = mono_;
    for (u32 i = 0; i < n; ++i) {
        // module 5, P:0x1CF-0x1D4
        u64 a = FracProduct(static_cast<u32>(fl[i]) & kMask24, y[0]);
        a = Mac(a, c[i], y[1]);
        a = Mac(a, fr[i], y[2]);
        a = Mac(a, bl[i], y[3]);
        a = RoundConvergent(Mac(a, br[i], y[4]));
        out[i] = Sext24(Limit24(a));
    }
}

}  // namespace fut::audio::dsp

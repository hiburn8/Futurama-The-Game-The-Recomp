// The XMV library's WMV2 frame decoder; see Wmv2Decoder.h. Addresses are default.xbe virtual addresses.
#include "Wmv2Decoder.h"

#include "Wmv2Kernels.h"
#include "XmvImage.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace xmv {
namespace {

// Table addresses in section XMV (all read from the user's XBE).
constexpr uint32_t kScanIntraLeft = 0x0026CA40;    // AC prediction from the left block
constexpr uint32_t kScanIntraTop = 0x0026CA00;     // AC prediction from the block above
constexpr uint32_t kScanIntra = 0x0026CA80;        // no AC prediction
constexpr uint32_t kScanInter = 0x0026CAC0;
constexpr uint32_t kScanAbt8x4 = 0x0026CB00;
constexpr uint32_t kScanAbt4x8 = 0x0026CB24;
constexpr uint32_t kVlcIntraCbp = 0x0026CB48;      // I-frame MCBPC
constexpr uint32_t kVlcDcLuma[2] = {0x0026CCB0, 0x0026D890};
constexpr uint32_t kVlcDcChroma[2] = {0x0026D348, 0x0026E428};
constexpr uint32_t kVlcMbTypeP[3] = {0x00271998, 0x00271460, 0x00270B88};   // P-frame MB type + CBP, by CBP table
constexpr uint32_t kVlcMv[2] = {0x0026ECB8, 0x0026FB88};
constexpr uint32_t kMvX[2] = {0x00273F10, 0x00273670};
constexpr uint32_t kMvY[2] = {0x00274360, 0x00273AC0};
constexpr uint32_t kRunLevelSet0 = 0x002747B0;     // intra chroma and every inter block (3 descriptors of 9 dwords)
constexpr uint32_t kRunLevelSet1 = 0x00274820;     // intra luma
constexpr uint32_t kDcEscape = 119;                // 0x77: the DC VLC escape (8 raw bits follow)
constexpr uint32_t kMvEscape = 1099;               // 0x44B: the MV VLC escape (6 + 6 raw bits follow)

uint32_t Load32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t Load16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
// x86 shifts use the count modulo 32.
inline uint32_t Shr(uint32_t v, uint32_t n) { return v >> (n & 31); }
inline uint32_t Shl(uint32_t v, uint32_t n) { return v << (n & 31); }

int32_t Dequant(int32_t level, int32_t q2, int32_t qOdd) { return level > 0 ? level * q2 + qOdd : level * q2 - qOdd; }

} // namespace

// The bit reader at decoder +0 / +4 / +8: MSB first out of little-endian 32-bit words. `bitsLeft` counts the unread
// low bits of `cur`; the plain readers refill lazily (bitsLeft may be 0), the VLC reader eagerly (bitsLeft >= 1 on
// return). The state is kept exactly because a 0-bit read returns `cur >> bitsLeft` (the bits already consumed).
struct Wmv2Decoder::BitReader {
    uint32_t cur = 0;
    uint32_t bitsLeft = 0;
    const uint8_t* ptr = nullptr;

    uint32_t NextWord() {
        const uint32_t v = Load32(ptr);
        ptr += 4;
        return v;
    }
    // orig: 0x002686D6.
    uint32_t Bit() {
        if (bitsLeft != 0) {
            --bitsLeft;
            return (cur >> bitsLeft) & 1;
        }
        cur = NextWord();
        bitsLeft = 31;
        return cur >> 31;
    }
    // orig: 0x00268723. n = 0..32.
    uint32_t Bits(uint32_t n) {
        if (n <= bitsLeft) {
            bitsLeft -= n;
            return Shr(0xFFFFFFFFu, 32 - n) & Shr(cur, bitsLeft);
        }
        const uint32_t old = cur, had = bitsLeft;
        const uint32_t high = (Shr(0xFFFFFFFFu, 32 - had) & old) * (had != 0 ? 1u : 0u);
        cur = NextWord();
        bitsLeft = 32 - (n - had);
        return Shr(cur, bitsLeft) | Shl(high, n - had);
    }
    // orig: 0x0026878B.
    void Skip(uint32_t n) {
        uint32_t left = bitsLeft;
        if (n > left) {
            n -= left;
            ptr += (n >> 5) * 4;
            cur = NextWord();
            left = 32;
            n &= 31;
        }
        bitsLeft = left - n;
    }
    // orig: 0x00268705. "0" -> 0, "10" -> 1, "11" -> 2.
    uint32_t Read012() { return Bit() ? Bit() + 1 : 0; }

    // orig: 0x002687BA. A table is u16 words: word 0 holds the index width n (low nibble); entry i (word 1 + i) holds
    // (value << 4) | length, or length 0 and the offset (in words, from the entry) of a sub-table whose word 0 holds
    // the next index width and, in its upper bits, how many bits of the parent index to consume.
    uint32_t Vlc(const uint16_t* table) {
        int32_t left = int32_t(bitsLeft);
        uint32_t n = table[0] & 0xF;
        for (;;) {
            if (left <= 0) {
                cur = NextWord();
                left += 32;
            }
            uint32_t idx;
            if (left >= int32_t(n)) {
                idx = Shr(cur, uint32_t(left) - n);
            } else {
                // Peek across the word boundary: the low bits of cur and the next word's top 16 bits.
                const uint32_t join = (cur << 16) | Load16(ptr + 2);
                idx = join >> (uint32_t(left) - n + 16);
            }
            idx = Shr(Shl(idx, 32 - n), 32 - n);           // keep n bits (n = 0 keeps all, as on the CPU)
            const uint16_t* entry = table + idx + 1;
            const uint32_t e = *entry;
            if ((e & 0xF) != 0) {
                left -= int32_t(e & 0xF);
                if (left <= 0) {
                    cur = NextWord();
                    left += 32;
                }
                bitsLeft = uint32_t(left);
                return e >> 4;
            }
            table = entry + (e >> 4);
            n = table[0] & 0xF;
            left -= int32_t(table[0] >> 4);
        }
    }
};

// A run / level descriptor (9 dwords, orig: 0x002747B0 / 0x00274820 + 36 * index).
struct Wmv2Decoder::RunLevel {
    const uint16_t* vlc = nullptr;      // +0x00
    uint32_t escape = 0;                // +0x04: the escape symbol
    uint32_t lastFrom = 0;              // +0x08: symbols >= this are the last coefficient of the block
    const uint8_t* run = nullptr;       // +0x0C
    const uint8_t* maxRun[2] = {};      // +0x10 / +0x14: by level, not last / last
    const int8_t* level = nullptr;      // +0x18
    const uint8_t* maxLevel[2] = {};    // +0x1C / +0x20: by run, not last / last
};

// The escape-3 field widths, fixed by the first escape-3 of a frame (orig: two locals of the frame routines).
struct Wmv2Decoder::Escape {
    uint32_t runBits = 0;
    uint32_t levelBits = 0;
};

namespace {

struct RunLevelSymbol {
    uint32_t run;
    int32_t level;
    bool last;
};

} // namespace

const uint16_t* Wmv2Decoder::Vlc(uint32_t va) const { return reinterpret_cast<const uint16_t*>(image_->At(va)); }

Wmv2Decoder::RunLevel Wmv2Decoder::LoadRunLevel(uint32_t setVa, uint32_t index) const {
    const uint8_t* d = image_->At(setVa + 36 * index);
    auto ptr = [&](int i) { return image_->AtPhysical(Load32(d + 4 * i)); };
    RunLevel rl;
    rl.vlc = reinterpret_cast<const uint16_t*>(ptr(0));
    rl.escape = Load32(d + 4);
    rl.lastFrom = Load32(d + 8);
    rl.run = ptr(3);
    rl.maxRun[0] = ptr(4);
    rl.maxRun[1] = ptr(5);
    rl.level = reinterpret_cast<const int8_t*>(ptr(6));
    rl.maxLevel[0] = ptr(7);
    rl.maxLevel[1] = ptr(8);
    return rl;
}

namespace {

// One (run, level, last) symbol: the escape logic shared by the intra (0x00265D81..) and inter (0x0026669F..)
// coefficient loops. Normal symbols and escapes 1 / 2 read the sign after the symbol, escape 3 before the level.
template <class BR, class RL, class ESC>
RunLevelSymbol ReadRunLevel(BR& br, const RL& rl, uint32_t q, ESC& esc) {
    RunLevelSymbol s{};
    const uint32_t sym = br.Vlc(rl.vlc);
    if (sym != rl.escape) {
        s.run = rl.run[sym];
        s.level = rl.level[sym];
        s.last = sym >= rl.lastFrom;
    } else if (br.Bit()) {
        // Escape 1: the level is offset by the largest level of that run.
        const uint32_t e = br.Vlc(rl.vlc);
        s.run = rl.run[e];
        s.last = e >= rl.lastFrom;
        s.level = rl.level[e] + rl.maxLevel[s.last ? 1 : 0][s.run];
    } else if (br.Bit()) {
        // Escape 2: the run is offset by the largest run of that level, plus one.
        const uint32_t e = br.Vlc(rl.vlc);
        s.level = rl.level[e];
        s.last = e >= rl.lastFrom;
        s.run = rl.run[e] + 1 + rl.maxRun[s.last ? 1 : 0][s.level];   // indexed by the signed level, as the original
    } else {
        // Escape 3: last, then (once per frame) the field widths, then run, sign, level as raw bits.
        s.last = br.Bit() != 0;
        if (esc.runBits == 0) {
            if (q < 8) {
                esc.levelBits = br.Bits(3);
                if (esc.levelBits == 0) esc.levelBits = 8 + br.Bit();
            } else {
                uint32_t n = 0, b = 0;
                do {
                    b = br.Bit();
                    ++n;
                } while (!b && n < 6);
                esc.levelBits = b ? n + 1 : 8;
            }
            esc.runBits = br.Bits(2) + 3;
        }
        s.run = br.Bits(esc.runBits);
        const uint32_t sign = br.Bit();
        s.level = int32_t(br.Bits(esc.levelBits));
        if (sign) s.level = -s.level;
        return s;
    }
    if (br.Bit()) s.level = -s.level;
    return s;
}

} // namespace

bool Wmv2Decoder::Create(const XmvImage& image, uint32_t width, uint32_t height, uint32_t firstTrackFormatTag) {
    if (!image.Loaded() || width == 0 || height == 0) return false;
    image_ = &image;
    kernels::SetImage(&image);
    width_ = width;
    height_ = height;
    mbw_ = (width + 15) >> 4;
    mbh_ = (height + 15) >> 4;
    const size_t lumaSize = size_t(mbw_) * 16 * mbh_ * 16, chromaSize = lumaSize / 4;
    planes_.assign(2 * (lumaSize + 2 * chromaSize), 0);   // XMemAlloc leaves the planes undefined; the first frame
    uint8_t* p = planes_.data();                           // is a keyframe, so they are always written before use
    cur_[0] = p;
    cur_[1] = p + lumaSize;
    cur_[2] = cur_[1] + chromaSize;
    ahead_[0] = cur_[2] + chromaSize;
    ahead_[1] = ahead_[0] + lumaSize;
    ahead_[2] = ahead_[1] + chromaSize;
    mbFlags_.assign(mbw_ + 1, 0);
    mbFlags_[0] = uint8_t(firstTrackFormatTag);
    predY_.assign(size_t(mbw_) * 16, 0);
    predU_.assign(size_t(mbw_) * 8, 0);
    predV_.assign(size_t(mbw_) * 8, 0);
    mvRow_.assign((size_t(mbw_) + 2) * 2, 0);
    skip_.assign((size_t(mbw_) * mbh_ + 31) / 32 + 1, 0);
    noRounding_ = 0;
    return true;
}

void Wmv2Decoder::SetExtension(uint32_t ext) {
    mspelBit_ = ext & 1;
    loopFilter_ = (ext >> 1) & 1;
    abtFlag_ = (ext >> 2) & 1;
    jType_ = (ext >> 3) & 1;
    topLeftMv_ = (ext >> 4) & 1;
    perMbRlBit_ = (ext >> 5) & 1;
    sliceCode_ = (ext >> 6) & 7;
}

void Wmv2Decoder::Swap() {
    for (int i = 0; i < 3; ++i) std::swap(cur_[i], ahead_[i]);
}

Wmv2Decoder::Result Wmv2Decoder::DecodeAhead(const uint8_t* data, size_t size) {
    (void)size;
    if (!image_) return Result::NotCreated;
    BitReader br;
    br.ptr = data;            // orig: GetNextFrame sets +8 to the frame data and +4 to 0 (+0 keeps a stale word)
    br.bitsLeft = 0;
    if (br.Bit() == 0) {
        // I-frame: a 7-bit field the decoder skips, the 5-bit quantiser, then the J-frame bit when enabled.
        noRounding_ = 0;
        br.Skip(7);
        const uint32_t q = br.Bits(5);
        if (jType_ && br.Bit()) return Result::Unsupported;   // orig: 0x0026C3AD (IntraX8), unused by these files
        lastIntra_ = true;
        return DecodeIFrame(br, q);
    }
    lastIntra_ = false;
    const Result r = DecodePFrame(br);
    noRounding_ = noRounding_ == 0 ? 1 : 0;
    return r;
}

// orig: 0x00265C81. One intra block: DC (predicted from the left / upper neighbour), the AC run / level loop, the
// first row and column accumulated as levels in the neighbour arrays (AC prediction), dequantised, inverse DCT.
// `top` (this column's array, shared with the block below) and `left` (this row's array, shared with the block to
// the right) hold the DC and the predicted first row / column as levels:
//   top[k]  = level of coefficient b = k * 8 (v = 0: the block's first row, which the block below predicts from)
//   left[k] = level of coefficient b = k     (u = 0: the block's first column, which the block to the right uses)
void Wmv2Decoder::DecodeIntraBlock(BitReader& br, uint8_t* dst, int32_t stride, uint32_t dcScale, uint32_t q,
                                   bool coded, uint32_t acPred, int32_t topLeftDc, int32_t* top, int32_t* left,
                                   uint32_t dcVlc, const RunLevel& rl, Escape& esc) {
    // DC direction: from the left block when the upper pair (top-left, top) differs less.
    const bool fromLeft = std::abs(topLeftDc - top[0]) <= std::abs(topLeftDc - left[0]);
    const int32_t predDc = fromLeft ? left[0] : top[0];
    int32_t dc = int32_t(br.Vlc(Vlc(dcVlc)));
    if (uint32_t(dc) == kDcEscape) dc = int32_t(br.Bits(8));
    if (dc != 0 && br.Bit()) dc = -dc;

    uint32_t scanVa;
    if (acPred) {
        if (fromLeft) {
            std::fill(top, top + 8, 0);     // keep the left block's first column
            scanVa = kScanIntraLeft;
        } else {
            std::fill(left, left + 8, 0);   // keep the upper block's first row
            scanVa = kScanIntraTop;
        }
    } else {
        std::fill(top, top + 8, 0);
        std::fill(left, left + 8, 0);
        scanVa = kScanIntra;
    }
    dc += predDc;
    top[0] = dc;
    left[0] = dc;

    int32_t coef[64] = {};
    coef[0] = dc * int32_t(dcScale);
    const int32_t q2 = int32_t(2 * q), qOdd = int32_t((q & 1) ? q : q - 1);
    const uint8_t* scan = image_->At(scanVa);
    if (coded) {
        uint32_t pos = 1;
        for (;;) {
            const RunLevelSymbol s = ReadRunLevel(br, rl, q, esc);
            pos = std::min<uint32_t>(pos + s.run, 63);
            const uint32_t b = scan[pos];
            if (b < 8) left[b] += s.level;
            else if ((b & 7) == 0) top[b >> 3] += s.level;
            else coef[b] = Dequant(s.level, q2, qOdd);
            ++pos;
            if (s.last) break;
        }
    }
    for (int k = 1; k < 8; ++k) {
        coef[k] = left[k] ? Dequant(left[k], q2, qOdd) : 0;
        coef[k * 8] = top[k] ? Dequant(top[k], q2, qOdd) : 0;
    }
    kernels::IdctIntra(dst, stride, coef);
}

// orig: 0x00265FB1. The six blocks of an intra macroblock. predY: this column's 16 predictor entries (block
// columns 0 / 1 at +0 / +8), leftY: the row's 16 (block rows 0 / 1), topLeft[2]: the top-left DC of block rows 0 / 1.
void Wmv2Decoder::DecodeIntraMb(BitReader& br, uint32_t dcScale, uint32_t q, uint8_t cbp, uint32_t acPred,
                                const RunLevel& lumaRl, const RunLevel& chromaRl, Escape& esc, int32_t col,
                                uint8_t* y, uint8_t* u, uint8_t* v, int32_t* topLeft, int32_t* leftY,
                                int32_t& topLeftU, int32_t* leftU, int32_t& topLeftV, int32_t* leftV,
                                uint32_t lumaDcVlc, uint32_t chromaDcVlc) {
    const int32_t strideY = StrideY(), strideC = StrideC();
    int32_t* predY = &predY_[size_t(col) * 16];
    const int32_t nextTopLeft = predY[8];   // the upper macroblock's block 3: the next macroblock's top-left
    topLeft[1] = leftY[0];                  // the left macroblock's block 1: block 2's top-left
    for (int k = 0; k < 4; ++k) {
        const int bx = k & 1, by = k >> 1;
        int32_t* top = predY + bx * 8;
        const int32_t topDc = top[0];
        DecodeIntraBlock(br, y + (by * strideY + bx) * 8, strideY, dcScale, q, (cbp >> (5 - k)) & 1, acPred,
                         topLeft[by], top, leftY + by * 8, lumaDcVlc, lumaRl, esc);
        topLeft[by] = topDc;
    }
    topLeft[0] = nextTopLeft;
    int32_t* pu = &predU_[size_t(col) * 8];
    const int32_t nextU = pu[0];
    DecodeIntraBlock(br, u, strideC, dcScale, q, (cbp >> 1) & 1, acPred, topLeftU, pu, leftU, chromaDcVlc,
                     chromaRl, esc);
    topLeftU = nextU;
    int32_t* pv = &predV_[size_t(col) * 8];
    const int32_t nextV = pv[0];
    DecodeIntraBlock(br, v, strideC, dcScale, q, cbp & 1, acPred, topLeftV, pv, leftV, chromaDcVlc, chromaRl, esc);
    topLeftV = nextV;
}

// orig: 0x002660DE.
Wmv2Decoder::Result Wmv2Decoder::DecodeIFrame(BitReader& br, uint32_t q) {
    const uint32_t dcScale = q <= 4 ? 8 : (q >> 1) + 6;
    const int32_t dcDefault = int32_t(((dcScale >> 1) + 0x400) / dcScale);   // 1024 / scale, rounded
    std::fill(predY_.begin(), predY_.end(), 0);
    std::fill(predU_.begin(), predU_.end(), 0);
    std::fill(predV_.begin(), predV_.end(), 0);
    for (uint32_t c = 0; c < mbw_; ++c) {
        predY_[c * 16] = predY_[c * 16 + 8] = dcDefault;
        predU_[c * 8] = predV_[c * 8] = dcDefault;
    }
    std::fill(mbFlags_.begin() + 1, mbFlags_.end(), uint8_t(0));   // byte 0 (left of column 0) is never cleared

    const uint32_t perMbRl = perMbRlBit_ ? br.Bit() : 0;
    RunLevel chromaRl, lumaRl;
    if (!perMbRl) {
        chromaRl = LoadRunLevel(kRunLevelSet0, br.Read012());
        lumaRl = LoadRunLevel(kRunLevelSet1, br.Read012());
    }
    const uint32_t dcSel = br.Bit();
    Escape esc;

    const int32_t strideY = StrideY(), strideC = StrideC();
    for (uint32_t row = 0; row < mbh_; ++row) {
        int32_t leftY[16] = {}, leftU[8] = {}, leftV[8] = {};
        leftY[0] = leftY[8] = leftU[0] = leftV[0] = dcDefault;
        int32_t topLeft[2] = {dcDefault, 0};
        int32_t topLeftU = dcDefault, topLeftV = dcDefault;
        uint8_t topLeftFlags = 0;
        for (uint32_t col = 0; col < mbw_; ++col) {
            // The coded block pattern: luma bits predicted from the neighbours' (orig: 0x002662CB..0x002663CC).
            // Flag byte bits 5 / 4 / 3 / 2: luma blocks 0..3, bit 1 Cb, bit 0 Cr.
            const uint32_t code = br.Vlc(Vlc(kVlcIntraCbp));
            uint8_t& cur = mbFlags_[1 + col];
            const uint8_t top = cur, left = mbFlags_[col];
            auto bit = [](uint32_t v, int b) { return (v >> b) & 1u; };
            // Each block: a = left, b = top-left, c = top neighbour bit; predicted a if b == c, else c.
            uint32_t f = top;
            const uint32_t p0 = bit(topLeftFlags, 2) == bit(top, 3) ? bit(left, 4) : bit(top, 3);
            f = (f & ~0x20u) | ((p0 ^ bit(code, 5)) << 5);
            const uint32_t p1 = bit(top, 3) == bit(top, 2) ? bit(f, 5) : bit(top, 2);
            f = (f & ~0x10u) | ((p1 ^ bit(code, 4)) << 4);
            const uint32_t p2 = bit(left, 4) == bit(f, 5) ? bit(left, 2) : bit(f, 5);
            f = (f & ~0x08u) | ((p2 ^ bit(code, 3)) << 3);
            const uint32_t p3 = bit(f, 5) == bit(f, 4) ? bit(f, 3) : bit(f, 4);
            f = (f & 0xF8u) | ((p3 ^ bit(code, 2)) << 2) | (bit(code, 1) << 1) | bit(code, 0);
            cur = uint8_t(f);
            topLeftFlags = top;

            const uint32_t acPred = br.Bit();
            if (perMbRl && cur != 0) {
                const uint32_t idx = br.Read012();
                chromaRl = LoadRunLevel(kRunLevelSet0, idx);
                lumaRl = LoadRunLevel(kRunLevelSet1, idx);
            }
            uint8_t* y = ahead_[0] + row * 16 * strideY + col * 16;
            uint8_t* u = ahead_[1] + row * 8 * strideC + col * 8;
            uint8_t* v = ahead_[2] + row * 8 * strideC + col * 8;
            DecodeIntraMb(br, dcScale, q, cur, acPred, lumaRl, chromaRl, esc, int32_t(col), y, u, v, topLeft, leftY,
                          topLeftU, leftU, topLeftV, leftV, kVlcDcLuma[dcSel], kVlcDcChroma[dcSel]);
        }
    }
    if (loopFilter_) return Result::Unsupported;   // orig: 0x00268CBB, off in every file (extension 0x75)
    return Result::Ok;
}

// orig: 0x002664CD..0x00266672 (the part of 0x0026684C before the kernels) and the kernel dispatch. One 8 x 8
// inter block: motion compensation from the reference plane (through a 16 x 11 edge buffer near the borders),
// plus the residual when coded (8 x 8 DCT, or two ABT halves 8 x 4 / 4 x 8).
void Wmv2Decoder::DecodeInterBlock(BitReader& br, uint32_t q, bool coded, uint32_t abtType, uint32_t perBlockAbt,
                                   const int8_t* mv, uint32_t mspel, uint32_t hshift, const RunLevel& rl,
                                   int32_t xPix, int32_t yPix, int32_t planeW, int32_t planeH, int32_t stride,
                                   uint8_t* dst, const uint8_t* ref, Escape& esc) {
    // Integer source position (MV in half pels) and the 16 x 11 window from (x - 1, y - 1) the filters may read.
    const int32_t xi = (mv[0] + xPix * 2) >> 1;
    const int32_t yi = (mv[1] + yPix * 2) >> 1;
    int32_t x0 = xi - 1, y0 = yi - 1;
    int32_t overRight = x0 - planeW + 16, overBottom = y0 - planeH + 11;
    const uint8_t* src;
    int32_t srcStride;
    uint8_t edge[16 * 11];
    if (x0 < 0 || y0 < 0 || overRight > 0 || overBottom > 0) {
        // orig: 0x002668B5. Clamp the window so that at least one row / column is inside, then copy it with the
        // outside pixels replaced by the nearest edge pixel.
        if (y0 < -10) {
            y0 = -10;
        } else if (overBottom > 10) {
            y0 = planeH - 1;
            overBottom = 10;
        }
        if (x0 < -15) {
            x0 = -15;
        } else if (overRight > 15) {
            x0 = planeW - 1;
            overRight = 15;
        }
        int32_t rows, dir;
        uint8_t* buf;
        if (y0 < 0) {                 // above the picture: fill bottom-up from the last window row
            rows = y0 + 11;
            dir = -1;
            buf = edge + 160;
            y0 += 10;
        } else {
            rows = overBottom > 0 ? 11 - overBottom : 11;
            dir = 1;
            buf = edge;
        }
        const uint8_t* s = ref + y0 * stride;
        int32_t shift, mode;
        if (x0 < 0) {
            shift = -x0;
            mode = -1;
        } else if (overRight <= 0) {
            s += x0;
            shift = 0;
            mode = 0;
        } else {
            s += x0 - overRight;
            shift = overRight;
            mode = 1;
        }
        kernels::EdgeCopy(s, dir * stride, buf, dir * 16, shift, mode, rows);
        src = edge + 17;
        srcStride = 16;
    } else {
        src = ref + yi * stride + xi;
        srcStride = stride;
    }
    const int32_t fx = mv[0] & 1, fy = mv[1] & 1;

    if (!coded) {
        if (mspel) kernels::MspelCopy(src, srcStride, dst, stride, fx, fy, int32_t(hshift));
        else kernels::McCopy(int32_t(noRounding_), src, srcStride, dst, stride, fx, fy);
        return;
    }
    if (perBlockAbt) abtType = br.Read012();
    int16_t residual[64];
    int16_t coef[64];
    if (abtType == 0) {
        const uint32_t mask = DecodeInterCoefs(br, q, rl, kScanInter, coef, esc);
        kernels::IdctInter(residual, coef, mask);
    } else {
        std::fill(residual, residual + 64, int16_t(0));
        // Sub-block pattern: "0" -> second half only, "10" -> both, "11" -> first half only.
        const bool first = br.Bit() != 0;
        const bool second = first ? br.Bit() == 0 : true;
        const uint32_t scan = abtType == 1 ? kScanAbt8x4 : kScanAbt4x8;
        for (int half = 0; half < 2; ++half) {
            if (!(half == 0 ? first : second)) continue;
            DecodeInterCoefs(br, q, rl, scan, coef, esc);
            if (abtType == 1) kernels::Abt8x4(residual, coef, half);
            else kernels::Abt4x8(residual, coef, half);
        }
    }
    if (mspel) kernels::MspelAdd(src, srcStride, dst, stride, fx, fy, int32_t(hshift), residual);
    else kernels::McAdd(int32_t(noRounding_), src, srcStride, dst, stride, fx, fy, residual);
}

// orig: 0x00266672. The run / level loop of an inter block (first coefficient at scan position 0), dequantised
// into 64 int16; returns the mask of rows v that hold a coefficient with u >= 1 (for IdctInter).
uint32_t Wmv2Decoder::DecodeInterCoefs(BitReader& br, uint32_t q, const RunLevel& rl, uint32_t scanVa,
                                       int16_t* coef, Escape& esc) {
    std::fill(coef, coef + 64, int16_t(0));
    const int32_t q2 = int32_t(2 * q), qOdd = int32_t((q & 1) ? q : q - 1);
    const uint8_t* scan = image_->At(scanVa);
    uint32_t mask = 0, pos = 0;
    for (;;) {
        const RunLevelSymbol s = ReadRunLevel(br, rl, q, esc);
        pos = std::min<uint32_t>(pos + s.run, 63);
        const uint32_t b = scan[pos];
        if (b > 7) mask |= 1u << (b & 7);
        // A position past the 32 entries of an ABT scan reads the following table bytes, as the original; an index
        // >= 64 would write outside the coefficient block there (only possible in a corrupt stream): dropped.
        if (b < 64) coef[b] = int16_t(Dequant(s.level, q2, qOdd));
        ++pos;
        if (s.last) return mask;
    }
}

// orig: 0x00266BB8.
Wmv2Decoder::Result Wmv2Decoder::DecodePFrame(BitReader& br) {
    if (sliceCode_ == 0) return Result::Unsupported;   // orig: a division by zero (+0xA8); every file has 1
    const uint32_t sliceRows = mbh_ / sliceCode_;
    const uint32_t q = br.Bits(5);
    const uint32_t dcScale = q <= 4 ? 8 : (q >> 1) + 6;
    std::fill(predY_.begin(), predY_.end(), 0);
    std::fill(predU_.begin(), predU_.end(), 0);
    std::fill(predV_.begin(), predV_.end(), 0);
    const int32_t dcDefault = int32_t(((dcScale >> 1) + 0x400) / dcScale);
    for (uint32_t c = 0; c < mbw_; ++c) {
        predY_[c * 16] = predY_[c * 16 + 8] = dcDefault;
        predU_[c * 8] = predV_[c * 8] = dcDefault;
    }
    std::fill(mvRow_.begin() + 2, mvRow_.begin() + 2 + 2 * mbw_, int8_t(0));   // entries 0 and mbw + 1 stay 0

    // Skipped macroblocks (orig: 0x00266D3B): 0 none, 1 one bit each, 2 per row (1 = whole row skipped, else one
    // bit per macroblock), 3 the same per column.
    std::fill(skip_.begin(), skip_.end(), 0u);
    auto setSkip = [&](uint32_t i, uint32_t b) { skip_[i >> 5] |= b << (i & 31); };
    switch (br.Bits(2)) {
    case 1:
        for (uint32_t i = 0; i < mbw_ * mbh_; ++i) setSkip(i, br.Bit());
        break;
    case 2:
        for (uint32_t r = 0; r < mbh_; ++r) {
            if (br.Bit()) {
                for (uint32_t c = 0; c < mbw_; ++c) setSkip(r * mbw_ + c, 1);
            } else {
                for (uint32_t c = 0; c < mbw_; ++c) setSkip(r * mbw_ + c, br.Bit());
            }
        }
        break;
    case 3:
        for (uint32_t c = 0; c < mbw_; ++c) {
            if (br.Bit()) {
                for (uint32_t r = 0; r < mbh_; ++r) setSkip(r * mbw_ + c, 1);
            } else {
                for (uint32_t r = 0; r < mbh_; ++r) setSkip(r * mbw_ + c, br.Bit());
            }
        }
        break;
    default:
        break;
    }

    // The MB type / CBP table: a 0..2 code mapped by the quantiser.
    uint32_t cbpTable = br.Read012();
    if (q <= 10) cbpTable = (cbpTable + 2) % 3;
    else if (q <= 20) cbpTable = (cbpTable + 1) % 3;
    const uint16_t* mbTypeVlc = Vlc(kVlcMbTypeP[cbpTable]);

    const uint32_t mspel = mspelBit_ ? br.Bit() : 0;
    uint32_t perMbAbt = 0, abtType = 0;
    if (abtFlag_) {
        perMbAbt = br.Bit() == 0 ? 1 : 0;
        abtType = perMbAbt ? 0 : br.Read012();
    }
    const uint32_t perMbRl = perMbRlBit_ ? br.Bit() : 0;
    RunLevel interRl, intraLumaRl;   // set 0 (inter and intra chroma), set 1 (intra luma)
    if (!perMbRl) {
        const uint32_t idx = br.Read012();
        interRl = LoadRunLevel(kRunLevelSet0, idx);
        intraLumaRl = LoadRunLevel(kRunLevelSet1, idx);
    }
    const uint32_t dcSel = br.Bit();
    const uint32_t mvSel = br.Bit();
    const uint16_t* mvVlc = Vlc(kVlcMv[mvSel]);
    const uint8_t* mvXTab = image_->At(kMvX[mvSel]);
    const uint8_t* mvYTab = image_->At(kMvY[mvSel]);

    Escape esc;
    uint32_t topBit = 0;            // the last "left or top" choice bit (orig: [ebp-0x3C], read when diff >= 8)
    const int32_t strideY = StrideY(), strideC = StrideC();
    const int32_t planeW = int32_t(mbw_ * 16), planeH = int32_t(mbh_ * 16);
    for (uint32_t row = 0; row < mbh_; ++row) {
        const bool firstLine = row % sliceRows == 0;
        int32_t leftY[16] = {}, leftU[8] = {}, leftV[8] = {};
        leftY[0] = leftY[8] = leftU[0] = leftV[0] = dcDefault;
        int32_t topLeft[2] = {dcDefault, 0};
        int32_t topLeftU = dcDefault, topLeftV = dcDefault;
        for (uint32_t col = 0; col < mbw_; ++col) {
            const uint32_t mbIndex = row * mbw_ + col;
            const bool skipped = (skip_[mbIndex >> 5] >> (mbIndex & 31)) & 1;
            uint8_t* dy = ahead_[0] + row * 16 * strideY + col * 16;
            uint8_t* du = ahead_[1] + row * 8 * strideC + col * 8;
            uint8_t* dv = ahead_[2] + row * 8 * strideC + col * 8;
            const uint8_t* ry = cur_[0] + row * 16 * strideY + col * 16;
            const uint8_t* ru = cur_[1] + row * 8 * strideC + col * 8;
            const uint8_t* rv = cur_[2] + row * 8 * strideC + col * 8;
            int8_t* mv = &mvRow_[(size_t(col) + 1) * 2];
            bool inter = true;
            if (skipped) {
                kernels::CopyMb(ry, ru, rv, dy, du, dv, strideY, strideC);
            } else {
                const uint32_t code = br.Vlc(mbTypeVlc);
                const uint8_t cbp = uint8_t(code & 0xBF);
                inter = (code & 0x40) != 0;
                if (!inter) {
                    // Intra macroblock (no CBP prediction in P-frames).
                    const uint32_t acPred = br.Bit();
                    if (perMbRl && cbp != 0) {
                        const uint32_t idx = br.Read012();
                        interRl = LoadRunLevel(kRunLevelSet0, idx);
                        intraLumaRl = LoadRunLevel(kRunLevelSet1, idx);
                    }
                    DecodeIntraMb(br, dcScale, q, cbp, acPred, intraLumaRl, interRl, esc, int32_t(col), dy, du, dv,
                                  topLeft, leftY, topLeftU, leftU, topLeftV, leftV, kVlcDcLuma[dcSel],
                                  kVlcDcChroma[dcSel]);
                } else {
                    // Motion vector prediction (orig: 0x00267163): left (A), top (B), top-right (C) of this row's
                    // stored vectors. With the top-left flag, a far-apart A / B costs a bit choosing one of them.
                    const int8_t* a = mv - 2;
                    const int8_t* b = mv;
                    const int8_t* c = mv + 2;
                    bool chooseBit = false;
                    if (topLeftMv_ && col != 0 && !firstLine && !mspel) {
                        const int32_t diff = std::max(std::abs(a[0] - b[0]), std::abs(a[1] - b[1]));
                        chooseBit = diff >= 8;
                    }
                    if (chooseBit) topBit = br.Bit();
                    if (perMbRl && cbp != 0) {
                        const uint32_t idx = br.Read012();
                        interRl = LoadRunLevel(kRunLevelSet0, idx);
                        intraLumaRl = LoadRunLevel(kRunLevelSet1, idx);
                    }
                    uint32_t perBlockAbt = 0;
                    if (perMbAbt && cbp != 0) {
                        perBlockAbt = br.Bit();
                        if (!perBlockAbt) abtType = br.Read012();
                    }
                    int8_t pred[2];
                    if (firstLine || (chooseBit && !topBit)) {
                        pred[0] = a[0];
                        pred[1] = a[1];
                    } else if (chooseBit) {
                        pred[0] = b[0];
                        pred[1] = b[1];
                    } else {
                        auto median = [](int8_t p, int8_t q2, int8_t r) {
                            return std::max(std::min(p, q2), std::min(std::max(p, q2), r));
                        };
                        pred[0] = median(a[0], b[0], c[0]);
                        pred[1] = median(a[1], b[1], c[1]);
                    }
                    int32_t dx, dyv;
                    const uint32_t sym = br.Vlc(mvVlc);
                    if (sym == kMvEscape) {
                        dx = int8_t(br.Bits(6));
                        dyv = int8_t(br.Bits(6));
                    } else {
                        dx = mvXTab[sym];
                        dyv = mvYTab[sym];
                    }
                    auto wrap = [](int32_t v) { return v > 63 ? v - 64 : (v < -63 ? v + 64 : v); };
                    mv[0] = int8_t(wrap(pred[0] + dx - 32));
                    mv[1] = int8_t(wrap(pred[1] + dyv - 32));
                    // Chroma vector: x / 2 when that is odd, else (x + sign(x)) / 2 (both truncating); this keeps a
                    // half-pel position whenever x is not a multiple of 4.
                    auto chroma = [](int32_t x) {
                        const int32_t h = x / 2;
                        if (h & 1) return h;
                        return ((x >> 6 & ~1) + 1 + x) / 2;
                    };
                    const int8_t mvC[2] = {int8_t(chroma(mv[0])), int8_t(chroma(mv[1]))};
                    const uint32_t hshift = (mspel && ((mv[0] & 1) || (mv[1] & 1))) ? br.Bit() : 0;

                    for (int k = 0; k < 4; ++k) {
                        const int bx = k & 1, by = k >> 1;
                        DecodeInterBlock(br, q, (cbp >> (5 - k)) & 1, abtType, perBlockAbt, mv, mspel, hshift,
                                         interRl, int32_t(col * 2 + bx) * 8, int32_t(row * 2 + by) * 8, planeW,
                                         planeH, strideY, dy + (by * strideY + bx) * 8, cur_[0], esc);
                    }
                    DecodeInterBlock(br, q, (cbp >> 1) & 1, abtType, perBlockAbt, mvC, 0, 0, interRl,
                                     int32_t(col) * 8, int32_t(row) * 8, planeW / 2, planeH / 2, strideC, du,
                                     cur_[1], esc);
                    DecodeInterBlock(br, q, cbp & 1, abtType, perBlockAbt, mvC, 0, 0, interRl, int32_t(col) * 8,
                                     int32_t(row) * 8, planeW / 2, planeH / 2, strideC, dv, cur_[2], esc);
                }
            }
            if (skipped || inter) {
                // orig: 0x0026752C. A non-intra macroblock resets the intra predictors at its position.
                std::fill(leftY, leftY + 16, 0);
                std::fill(leftU, leftU + 8, 0);
                std::fill(leftV, leftV + 8, 0);
                int32_t* py = &predY_[size_t(col) * 16];
                int32_t* pu = &predU_[size_t(col) * 8];
                int32_t* pv = &predV_[size_t(col) * 8];
                topLeft[0] = py[8];
                topLeftU = pu[0];
                topLeftV = pv[0];
                leftY[0] = leftY[8] = leftU[0] = leftV[0] = dcDefault;
                std::fill(py, py + 16, 0);
                py[0] = py[8] = dcDefault;
                std::fill(pu, pu + 8, 0);
                pu[0] = dcDefault;
                std::fill(pv, pv + 8, 0);
                pv[0] = dcDefault;
            }
            if (skipped || !inter) {
                mv[0] = 0;
                mv[1] = 0;
            }
        }
    }
    if (loopFilter_) return Result::Unsupported;
    return Result::Ok;
}

// orig: 0x002652A2 for a YUY2 surface (whole macroblocks through 0x002651AB; partial ones through a 16 x 16 copy of
// which only the visible part is stored, which is what this loop does directly).
void Wmv2Decoder::ConvertToYuy2(uint8_t* dst, int32_t pitch) const {
    if (!image_) return;
    const int32_t strideY = StrideY(), strideC = StrideC();
    if (width_ % 16 == 0 && height_ % 16 == 0) {
        for (uint32_t r = 0; r < mbh_; ++r)
            for (uint32_t c = 0; c < mbw_; ++c)
                kernels::Yuy2Block16(cur_[0] + r * 16 * strideY + c * 16, cur_[1] + r * 8 * strideC + c * 8,
                                     cur_[2] + r * 8 * strideC + c * 8, strideY, strideC,
                                     dst + r * 16 * pitch + c * 32, pitch);
        return;
    }
    for (uint32_t y = 0; y < height_; ++y) {
        uint8_t* d = dst + y * pitch;
        for (uint32_t x = 0; x < width_; x += 2) {
            d[2 * x + 0] = cur_[0][y * strideY + x];
            d[2 * x + 1] = cur_[1][(y >> 1) * strideC + (x >> 1)];
            d[2 * x + 2] = cur_[0][y * strideY + x + 1];
            d[2 * x + 3] = cur_[2][(y >> 1) * strideC + (x >> 1)];
        }
    }
}

} // namespace xmv

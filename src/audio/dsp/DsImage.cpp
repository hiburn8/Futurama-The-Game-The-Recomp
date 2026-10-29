#include "core/XbeImage.h"
#include "audio/dsp/DsImage.h"

#include <cstring>

namespace fut::audio::dsp {

namespace {

constexpr u64 kKeySeed = 0x7FA49BCA49DE12BAull;  // orig: 0x0024F750 (pushes 0x49DE12BA, 0x7FA49BCA)
constexpr u32 kKeyRounds = 0x50;

u32 U32At(const std::vector<u8>& v, size_t o) {
    u32 x = 0;
    if (o + 4 <= v.size()) std::memcpy(&x, &v[o], 4);
    return x;
}

bool Fail(std::string* error, const char* why) {
    if (error) *error = why;
    return false;
}

// Bytes [va, va + n) of an XBE section (the file's section headers), or false.
bool ReadXbe(const std::vector<u8>& xbe, u32 va, u32 n, std::vector<u8>& out) {
    xbe::XbeImage image;
    if (!image.Load(xbe)) return false;
    const u8* data = image.At(va, n);
    if (!data) return false;
    out.assign(data, data + n);
    return true;
}

}  // namespace

u64 DsLfsr::Key() {
    Step(kKeySeed);
    for (u32 i = 0; i < kKeyRounds; ++i) Step();
    return state_;
}

void DsLfsr::Decrypt(const u8 key[8], u8* data, size_t n) {
    const u64 k = Key();
    u8 k8[8];
    for (u32 i = 0; i < 8; ++i) k8[i] = static_cast<u8>(k >> (8 * i));   // the state's bytes, little-endian
    Step(k);
    for (size_t j = 0; j < n; ++j) {
        Step();
        const u8 lfsr = static_cast<u8>(state_);
        const u8 kj = k8[j & 7];
        data[j] = static_cast<u8>(kj ^ lfsr ^ static_cast<u8>(data[j] - static_cast<u8>(key[j & 7] * kj)));
    }
}

bool DsImage::Load(const std::vector<u8>& dsstdfx, const std::vector<u8>& xbe, std::string* error) {
    file_.clear();
    effects_.clear();
    // ---- the executive (DSOUND 0x0024A11D copies it to scratch 0)
    std::vector<u8> exe;
    if (!ReadXbe(xbe, kExecutiveVa, kExecutiveBytes, exe)) return Fail(error, "default.xbe: no DSOUND executive");
    for (u32 i = 0; i < kExecutiveBytes; i += 4)
        if (exe[i + 3] != 0) return Fail(error, "default.xbe: the GP executive is not DSP code");
    if (U32At(exe, 0) != 0x0BF080) return Fail(error, "default.xbe: unexpected GP executive");   // jsr $155

    // ---- dsstdfx.bin: header, effect map, key table (orig: 0x0024A3FF / 0x0024A192)
    std::vector<u8> f = dsstdfx;
    if (f.size() < kCodeOffset) return Fail(error, "dsstdfx.bin: too short");
    u32 hdr[6];
    for (u32 i = 0; i < 6; ++i) hdr[i] = U32At(f, kHeaderOffset + 4 * i);
    const u32 codeWords = hdr[1], stateOff = hdr[2], stateWords = hdr[3];
    if (stateOff != kCodeOffset + 4 * codeWords) return Fail(error, "dsstdfx.bin: inconsistent header");
    const size_t mapOff = kCodeOffset + size_t(codeWords + stateWords) * 4;
    const u32 count = U32At(f, mapOff);
    if (count == 0 || count > 64) return Fail(error, "dsstdfx.bin: bad effect count");
    const size_t keysOff = mapOff + size_t(count - 1) * 32 + 0x28;   // DSEFFECTIMAGEDESC holds count maps
    if (keysOff + 8 * size_t(count) > f.size()) return Fail(error, "dsstdfx.bin: truncated");

    DsLfsr lfsr;
    const u64 ctx = lfsr.Key();
    u8 ctxKey[8];
    for (u32 i = 0; i < 8; ++i) ctxKey[i] = static_cast<u8>(ctx >> (8 * i));
    lfsr.Decrypt(ctxKey, &f[keysOff], 8 * size_t(count));            // the per-effect keys
    size_t code = kCodeOffset;
    for (u32 i = 0; i < count; ++i) {
        EffectMapEntry e;
        const size_t m = mapOff + 8 + size_t(i) * 32;
        e.codeOffset = U32At(f, m), e.codeSize = U32At(f, m + 4);
        e.stateOffset = U32At(f, m + 8), e.stateSize = U32At(f, m + 12);
        e.yOffset = U32At(f, m + 16), e.ySize = U32At(f, m + 20);
        e.scratchOffset = U32At(f, m + 24), e.scratchSize = U32At(f, m + 28);
        if (code + e.codeSize > stateOff || e.codeSize % 4) return Fail(error, "dsstdfx.bin: bad effect map");
        lfsr.Decrypt(&f[keysOff + 8 * i], &f[code], e.codeSize);    // effects are stored back to back
        for (size_t w = 0; w < e.codeSize; w += 4)
            if (f[code + w + 3] != 0) return Fail(error, "dsstdfx.bin: descrambling failed (wrong image?)");
        if (e.stateOffset < stateOff || e.stateOffset + e.stateSize > stateOff + 4 * size_t(stateWords))
            return Fail(error, "dsstdfx.bin: bad effect state range");
        code += e.codeSize;
        effects_.push_back(e);
    }
    std::memcpy(header_, hdr, sizeof hdr);
    executive_ = std::move(exe);
    file_ = std::move(f);
    return true;
}

}  // namespace fut::audio::dsp

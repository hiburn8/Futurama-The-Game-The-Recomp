// The GP program, read from the user's files at run time (nothing of it is embedded in the port):
//   - xbox/dsstdfx.bin (the disc's DSP effects image, 25,256 bytes): header, the effects' DSP code (each effect
//     scrambled with a 64-bit LFSR byte cipher and its own key), the effect state ("X data"), the effect map
//     (DSEFFECTIMAGEDESC) and the scrambled key table (docs/analysis/audio_dsp_port.md 1);
//   - the GP executive, a 0x5CC-byte DSP blob in default.xbe's DSOUND section (VA 0x00263BE0), which the CPU puts
//     at scratch offset 0 before the GP starts (2.2).
// Load() descrambles the code as DSOUND does before the download (orig: 0x0024A192 with the LFSR 0x0024F6D7, the
// key schedule 0x0024F750 and the byte cipher 0x0024F77D) and checks the result (every code word has a zero top
// byte). The DSP side gets the bytes unchanged otherwise; the CPU-side map rewrite (host addresses) is not needed.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace fut::audio::dsp {

struct EffectMapEntry {                          // DSEFFECTMAP, file values
    u32 codeOffset = 0, codeSize = 0;            // bytes in the file (4 bytes per DSP word)
    u32 stateOffset = 0, stateSize = 0;
    u32 yOffset = 0, ySize = 0;
    u32 scratchOffset = 0, scratchSize = 0;
};

class DsImage {
public:
    static constexpr u32 kHeaderOffset = 0x800;  // 6 words {state X offset, code words, state byte offset,
                                                 //   state words, mailbox command (template 3), 0}
    static constexpr u32 kCodeOffset = 0x818;
    static constexpr u32 kExecutiveVa = 0x00263BE0;   // DSOUND: the GP executive (copied by 0x0024A11D)
    static constexpr u32 kExecutiveBytes = 0x5CC;

    // `dsstdfx`: the file's bytes; `xbe`: default.xbe's bytes. False (with a reason) if either is not the
    // expected data.
    bool Load(const std::vector<u8>& dsstdfx, const std::vector<u8>& xbe, std::string* error = nullptr);
    bool Loaded() const { return !file_.empty(); }

    // The file with the effect code (and the key table) descrambled in place: what DSOUND leaves in the GP
    // scratch memory at offsets 0x818.. before posting the download command.
    const std::vector<u8>& Bytes() const { return file_; }
    const std::vector<u8>& Executive() const { return executive_; }
    u32 Header(u32 i) const { return header_[i]; }
    u32 CodeWords() const { return header_[1]; }
    u32 StateByteOffset() const { return header_[2]; }
    u32 StateWords() const { return header_[3]; }
    u32 EffectCount() const { return static_cast<u32>(effects_.size()); }
    const EffectMapEntry& Effect(u32 i) const { return effects_[i]; }
    // X address of an effect's state on the DSP: the executive loads the state section at X:(0x80 + header word 0).
    u32 StateX(u32 i) const { return 0x80 + header_[0] + (effects_[i].stateOffset - StateByteOffset()) / 4; }

private:
    std::vector<u8> file_, executive_;
    u32 header_[6] = {};
    std::vector<EffectMapEntry> effects_;
};

// orig: 0x0024F6D7 (64-bit state at 0x002642E8): s = seed when seed != 0; then s = s >> 1 | f << 63 with
// f = bit 0 ^ bit 2 ^ bit 3 ^ bit 63 of s.
class DsLfsr {
public:
    void Step(u64 seed = 0) {
        if (seed) state_ = seed;
        const u64 f = (state_ ^ (state_ >> 2) ^ (state_ >> 3) ^ (state_ >> 63)) & 1;
        state_ = (state_ >> 1) | (f << 63);
    }
    u64 State() const { return state_; }
    // orig: 0x0024F750: reseed with the library's constant, 80 more steps; the state is the key.
    u64 Key();
    // orig: 0x0024F77D (in place, without the IV form): out[j] = k8[j & 7] ^ lfsr ^ (in[j] - k[j & 7] * k8[j & 7])
    // with k8 = Key() and the LFSR reseeded from k8, stepped once per byte.
    void Decrypt(const u8 key[8], u8* data, size_t n);

private:
    u64 state_ = 0;
};

}  // namespace fut::audio::dsp

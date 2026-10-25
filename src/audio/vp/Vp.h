// A software model of the MCPX voice processor (VP): every running voice of the front end's state (VpFrontEnd.h) is
// fetched (PCM 8 / 16, Xbox ADPCM), resampled by its pitch register, HRTF-filtered (3D voices: the 31-tap 8-bit HRIR
// pair and the interaural delay the library uploaded), I3DL2-filtered (filter mode 3: the direct / room one-pole
// coefficients of FCB), scaled by its 8 slot attenuations and accumulated into the 32 integer mix bins of a
// 32-sample frame at 48 kHz, with the per-bin headroom; the bins then go to the GP (src/audio/dsp AudioDsp).
// docs/analysis/audio_port.md 5.
//
// Every hardware behaviour the code of the XBE does not fix is a hypothesis (L) and lives in VpHardware (this file)
// and the functions of Vp.cpp's "hardware hypotheses" section; the list with the reasons is audio_port.md 5.4.
// The voice structures, the register meanings and the HRIR / SGE layouts the library composes are (H) / (O).
#pragma once

#include "audio/vp/VpFrontEnd.h"

#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace fut::audio::vp {

// Where the VP reads sample data: physical addresses as the input SGE table maps them.
class VpMemory {
public:
    virtual ~VpMemory() = default;
    // Copies n bytes from physical address `phys`; false (and zeros) where nothing is mapped.
    virtual bool Read(u32 phys, u8* dst, u32 n) const = 0;
};

// Host buffers mapped at page-aligned stand-in physical addresses (what the port gives the SGE table).
class VpMemoryMap : public VpMemory {
public:
    // Maps `bytes` bytes of `data` (kept by the caller while mapped); returns the physical address of data[0]
    // (page-aligned). `pageOffset` lets the mapping start inside a page like the original's heap data.
    u32 Map(const u8* data, u32 bytes, u32 pageOffset = 0);
    void Unmap(u32 phys);
    // Maps at a given physical address (tests replaying the oracle's addresses).
    void MapAt(u32 phys, const u8* data, u32 bytes);
    bool Read(u32 phys, u8* dst, u32 n) const override;

private:
    struct Range {
        const u8* data;
        u32 bytes;
    };
    std::map<u32, Range> ranges_;                // start -> range
    u32 next_ = 0x01000000;
};

// The voices' notifiers in system memory (NV_PAPU_FENADDR, 0xFE80115C = [0x00264334], H): 16 bytes each, four per voice
// from entry 2 (the library's voice notifiers 0x0024CE73: entry voice * 4 + 2; 0 = SSL A done, 1 = SSL B done, 3 = the
// voice's end), the status byte at +0xF (the library writes 0x80, pending; the VP 1, done). Only the status bytes
// are kept (the timestamp / info words are written by the hardware and not read by the game's paths). The library
// side and the VP both write it (the backend's lock).
class VpNotifierMemory {
public:
    static constexpr u32 kPerVoice = 4;
    u8 Status(u32 voice, u32 n) const { return status_[(voice % kVoices) * kPerVoice + n % kPerVoice]; }
    void SetStatus(u32 voice, u32 n, u8 v) { status_[(voice % kVoices) * kPerVoice + n % kPerVoice] = v; }

private:
    std::array<u8, kVoices * kPerVoice> status_{};
};

// The hypotheses, as parameters (defaults = the model's choice; audio_port.md 5.4).
struct VpHardware {
    bool attenuationFFFMutes = true;             // L1: 0xFFF = silence (else -63.98 dB)
    // L4: the HRIR taps' scale, 1.0 = 2^hrirShift (sum >> 9: the far ears' low-frequency gain ~1.0, audio_port.md
    // 5.6; xemu's reading is >> 7, which gives the set +9 dB of diffuse-field gain)
    u32 hrirShift = 9;
    bool hrirSignMagnitude = true;               // tap bytes are sign + 7-bit magnitude (O; else two's complement)
    bool positiveItdDelaysLeft = true;           // L4: ITD > 0 delays the left (first) ear
    u32 interpolationBits = 16;                  // L2: linear interpolation with this many fraction bits
};

// Level statistics per stage of the mix (tests / level analysis; accumulated while set with Vp::SetLevels).
struct LevelStat {
    double sumSq = 0;
    s64 peak = 0;
    u64 samples = 0, limit = 0;                  // limit: samples at the stage's saturation value
    void Add(s64 x, s64 full) {
        sumSq += static_cast<double>(x) * static_cast<double>(x);
        peak = std::max<s64>(peak, x < 0 ? -x : x);
        limit += x >= full - 1 || x <= -full;
        ++samples;
    }
};
struct Levels {
    LevelStat fetched2D, fetched3D;              // a voice's samples after the fetch and the resampler (16-bit scale)
    LevelStat hrtf;                              // the HRTF FIR's ear outputs (16-bit scale)
    LevelStat slot2D, slot3D;                    // one slot's contribution after its attenuation (24-bit scale)
    std::array<LevelStat, 32> sums;              // a bin's sum before the headroom shift (24-bit scale)
    std::array<LevelStat, 32> bins;              // a bin as the GP reads it (24-bit)
    std::array<LevelStat, 6> speakers;           // after the GP: FL FR C LFE BL BR (filled by the backend)
    std::array<LevelStat, 2> stereo;             // after the EP's down-mix (filled by the backend)
    u32 maxVoices = 0;                           // running voices in one frame
};

class Vp {
public:
    static constexpr u32 kFrame = 32, kBins = 32;
    using Bins = std::array<std::array<s32, kFrame>, kBins>;

    Vp(const VpFrontEnd& fe, const VpMemory& mem, VpHardware hw = {});
    // Accumulates the stages' levels into `levels` from the next frame on (nullptr: off).
    void SetLevels(Levels* levels) { levels_ = levels; }
    // Where the VP writes its notifiers (the stream voices' SSL completions; nullptr: nowhere) and whether it raised
    // the voice-notifier interrupt (NV_PAPU_ISTS bit 6, H: the library's ISR 0x0024B096 queues its DPC for it) since
    // the last TakeInterrupt.
    void SetNotifiers(VpNotifierMemory* n) { notifiers_ = n; }
    bool TakeInterrupt() {
        const bool r = interrupt_;
        interrupt_ = false;
        return r;
    }

    // Mixes one 32-sample frame of every running voice into `bins` (cleared first; 24-bit two's complement).
    void RenderFrame(Bins& bins);
    // A non-looping voice that played past its end buffer offset (the hardware stops it; L8).
    bool Ended(u32 voice) const;
    // The voice's current sample position (integer part), as the hardware's CBO would read.
    u32 Position(u32 voice) const;
    u64 Frames() const { return frames_; }

private:
    struct Run {
        u32 generation = 0;                      // the front end's VOICE_ON count this state belongs to
        u64 pos = 0;                             // sample position, 32.32 fixed point
        bool ended = false;
        s32 hist[2][2] = {};                     // the last two fetched samples per channel (interpolation)
        u64 histIndex = ~0ull;                   // sample index of hist[*][1]
        std::array<s32, 128> fir{};              // resampled mono input history for the HRTF FIR (ring)
        u32 firPos = 0;
        s32 lp[5] = {};                          // one-pole filter states (slots 0-3 direct, slot 4 room)
        s64 adpcmBlock = -1;
        std::array<s16, 128> adpcm{};            // the decoded block (64 samples x channels, interleaved)
        u32 ssl = 0, segment = 0;                // a stream voice: the SSL it plays (0 A, 1 B), the segment in it
    };
    // A stream voice (FMT bit 24): the segment under run.pos (the position relative to the segment's start), after
    // passing completed segments and SSLs; false while the SSL it reached has no segments (starved).
    bool StreamSegment(u32 v, Run& run);
    s32 StreamFetch(u32 v, Run& run, u32 segment, u64 index, u32 ch);
    void RenderVoice(u32 v, Run& run, std::array<std::array<s64, kFrame>, kBins>& acc);
    s32 Fetch(u32 v, Run& run, u64 index, u32 ch, const Format& f);
    bool ReadBytes(const VoiceRegs& r, u32 byteOffset, u8* dst, u32 n) const;
    s32 HrirTap(u32 byte) const;

    const VpFrontEnd& fe_;
    const VpMemory& mem_;
    VpHardware hw_;
    std::array<Run, kVoices> runs_{};
    std::array<s32, 4096> gain_{};               // Q15 gain per 12-bit attenuation (L1)
    u64 frames_ = 0;
    Levels* levels_ = nullptr;
    u32 running_ = 0;
    VpNotifierMemory* notifiers_ = nullptr;
    bool interrupt_ = false;
};

}  // namespace fut::audio::vp

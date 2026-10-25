// The MCPX voice processor's register interface as the XDK DirectSound library drives it (docs/analysis/
// audio_port.md 5): the PIO methods at 0xFE820000 + offset that the library writes, and the state they leave in the
// hardware (current voice, per-voice register words, the voice lists, the HRIR entries, the input SGE table, the
// mix-bin headrooms). `VpFrontEnd` applies a stream of method writes; `Vp` (Vp.h) renders from that state.
//
// What each method means is read from the library's code (H: which object field it writes, in which order, under
// the voice lock) and from the values the register oracle recorded (O: tools/difftest/dsound_reg_oracle.py); the
// hardware semantics of the fields (bit layouts beyond what the library composes, what the VP does with them) are
// (L) and are decoded only in this file and Vp.cpp. Names follow the XDK / NV1BA0 convention (hint: xemu).
#pragma once

#include "core/Types.h"

#include <array>
#include <utility>
#include <vector>

namespace fut::audio::vp {

// PIO method offsets (from 0xFE820000; the library writes 32-bit words).
namespace pio {
constexpr u32 kFree = 0x010;                     // read: FIFO free bytes (the library waits for room)
constexpr u32 kVoiceListHead = 0x104;            // (O: 2 at a voice release, between two 0x10C writes)
constexpr u32 kVoiceListLock = 0x10C;            // (O: 0 / 1 around 0x104)
constexpr u32 kSetAntecedentVoice = 0x120;       // list << 16 | antecedent voice (0xFFFF: head)
constexpr u32 kVoiceOn = 0x124;
constexpr u32 kVoiceOff = 0x128;
constexpr u32 kVoiceRelease = 0x12C;
constexpr u32 kVoicePause = 0x140;               // voice | pause << 18 (L)
constexpr u32 kSetCurrentHrtfEntry = 0x160;
constexpr u32 kSetSubmixHeadroom = 0x200;        // + 4 * bin (0..31), 3 bits
constexpr u32 kSetHrtfHeadroom = 0x280;          // 3 bits (CMcpxAPU 0x0024ABDF)
constexpr u32 kSetHrtfSubmixes = 0x2C0;          // the four HRTF output bins, one per byte (O: 06 08 07 09)
constexpr u32 kSetCurrentSsl = 0x190;            // the 64-entry block of the SSL segment table (index & 0x3FFFC0)
constexpr u32 kSetCurrentVoice = 0x2F8;
constexpr u32 kVoiceLock = 0x2FC;                // 1 / 0 around a voice's parameter updates
constexpr u32 kVbin = 0x300;                     // slot bins 0-5 (5 bits at 0, 5, 10, 16, 21, 26)
constexpr u32 kFmt = 0x304;                      // format + slot bins 6, 7
constexpr u32 kEnv0 = 0x308, kEnvA = 0x30C, kEnv1 = 0x310, kEnvF = 0x314;
constexpr u32 kMisc = 0x318;                     // filter mode bits 16-17 (O: 3 for 3D voices)
constexpr u32 kHrtf = 0x31C;                     // HRIR entry of the voice (0xFFFF: none)
constexpr u32 kLfoEnvA = 0x350, kLfoEnvB = 0x36C, kLfoMod = 0x370;   // (written 0 by the library)
constexpr u32 kVolA = 0x360, kVolB = 0x364, kVolC = 0x368;
constexpr u32 kVoiceSslA = 0x320, kVoiceSslB = 0x35C;   // a stream voice's two SSLs: base << 8 | segment count
constexpr u32 kFca = 0x374, kFcb = 0x378;        // filter coefficients (FCB = room HF << 16 | direct HF)
constexpr u32 kPitch = 0x37C;                    // pitch << 16 (1/4096 octave against 48 kHz)
constexpr u32 kBufBase = 0x3A0, kBufLbo = 0x3A4, kBufCbo = 0x3D8, kBufEbo = 0x3DC;
constexpr u32 kHrir = 0x400;                     // 16 words of the current HRIR entry
constexpr u32 kSslSegmentOffset = 0x600;         // + 8 i (i < 64): segment block + i's physical address
constexpr u32 kSslSegmentLength = 0x604;         // + 8 i: its format << 16 | sample count
constexpr u32 kSetInbufSge = 0x804;              // current input SGE index
constexpr u32 kSetInbufSgeOffset = 0x808;        // physical page of the current SGE
}  // namespace pio

constexpr u32 kVoices = 256;                     // 0-63 3D (HRTF) voices, 64-255 2D voices (O: the library's split)
constexpr u32 kHrirEntries = 512;                // voice * 2 + toggle (the library double-buffers)
constexpr u32 kSges = 2048;                      // the hardware's buffer SGE table
// (port, L) the model's table: entries beyond the hardware's 2048, so that the bridge can map any of the disc's
// static buffers whole (the streams use SSL segments instead: VpStream.h)
constexpr u32 kModelSges = 32768;
constexpr u32 kNoHrtf = 0xFFFF;
// The SSL segment table: two SSLs of 16 segments per voice (the library's index (ssl + 2 voice) 16 + n, CMcpxStream
// 0x0024B4F5 / 0x0024B656, H)
constexpr u32 kSslSegments = kVoices * 2 * 16;
constexpr u32 kFmtStream = 0x01000000;           // FMT bit 24: the voice plays its SSLs (O: CMcpxStream's voices)

// A voice's register words as written (the TAR / CFG words of the voice's structure).
struct VoiceRegs {
    u32 vbin = 0, fmt = 0, env0 = 0, envA = 0, env1 = 0, envF = 0, misc = 0, hrtf = kNoHrtf;
    u32 lfoEnvA = 0, lfoEnvB = 0, lfoMod = 0;
    u32 vol[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    u32 fca = 0, fcb = 0, pitch = 0;
    u32 bufBase = 0, bufLbo = 0, bufCbo = 0, bufEbo = 0;
    u32 ssl[2] = {0, 0};                         // SET_VOICE_SSL_A / _B as written (base << 8 | count)
};

// One SSL segment (O: the library's words; the field meanings are its composition, H, read by the model, L).
struct SslSegment {
    u32 offset = 0;                              // physical address of the segment's first byte
    u32 length = 0;                              // bits 0-15 samples, 16-17 container, 18-22 channels - 1, 23 stereo
};

struct VoiceState {
    VoiceRegs r;
    bool on = false;                             // VOICE_ON .. VOICE_OFF (L: the VP processes it)
    bool paused = true;                          // (L) a voice starts paused; VOICE_PAUSE with bit 18 clear runs it
    u32 list = 0;                                // the list of its VOICE_ON (from SET_ANTECEDENT_VOICE)
    bool cboWritten = false;                     // CBO set since VOICE_ON (else playback starts at 0)
    u32 generation = 0;                          // VOICE_ON count (the VP resets its per-voice state on a change)
};

// Field decoding (L: the hardware's reading; the library composes exactly these fields, H).
struct Format {
    u32 bin6, bin7;
    u32 samplesPerBlock;                         // bits 16-20 (O: 1 for stereo ADPCM, 0 mono)
    bool linked, loop, stereo;                   // bits 22, 25, 27
    u32 sampleSize;                              // bits 28-29 (O: 0 for 8-bit PCM, 1 for 16-bit, 2 for ADPCM)
    u32 container;                               // bits 30-31 (O: 0 8-bit, 1 16-bit, 2 ADPCM)
};
Format DecodeFormat(u32 fmt);
extern const u32 kVbinShift[6];
// The 8 slot bins (slots 0-5 from VBIN, 6-7 from FMT).
void SlotBins(const VoiceRegs& r, u8 bins[8]);
// The 8 slot attenuations (12 bits, 1/64 dB) from VOLA / VOLB / VOLC (same packing as xds::UnpackVolumes).
void SlotAttenuations(const VoiceRegs& r, u16 att[8]);
s32 PitchValue(const VoiceRegs& r);              // signed 1/4096 octave

// Where PIO method writes go: the front end itself, or a FIFO that the audio thread drains at a frame boundary.
class VpPio {
public:
    virtual ~VpPio() = default;
    virtual void Write(u32 offset, u32 value) = 0;
};

class VpFrontEnd : public VpPio {
public:
    VpFrontEnd();
    void Reset();
    // One 32-bit method write at 0xFE820000 + offset (offsets outside the VP's PIO page are ignored).
    void Write(u32 offset, u32 value) override;

    const VoiceState& Voice(u32 v) const { return voices_[v % kVoices]; }
    VoiceState& MutableVoice(u32 v) { return voices_[v % kVoices]; }
    const std::array<u32, 16>& Hrir(u32 entry) const { return hrir_[entry % kHrirEntries]; }
    u32 SgePage(u32 index) const { return sge_[index % kModelSges]; }
    const SslSegment& Segment(u32 index) const { return ssl_[index % kSslSegments]; }
    u32 Headroom(u32 bin) const { return headroom_[bin & 31]; }
    u32 HrtfHeadroom() const { return hrtfHeadroom_; }
    u32 HrtfSubmixes() const { return hrtfSubmixes_; }
    u32 CurrentVoice() const { return current_; }
    u32 Writes() const { return writes_; }
    u32 Unknown() const { return unknown_; }     // writes to offsets this model does not know

private:
    std::array<VoiceState, kVoices> voices_{};
    std::vector<std::array<u32, 16>> hrir_;
    std::vector<u32> sge_;
    std::vector<SslSegment> ssl_;
    std::array<u32, 32> headroom_{};
    u32 hrtfHeadroom_ = 0, hrtfSubmixes_ = 0;
    u32 current_ = 0, currentHrir_ = 0, currentSge_ = 0, antecedent_ = 0, currentSsl_ = 0;
    u32 writes_ = 0, unknown_ = 0;
};

// The method writes of one producer (the game thread), applied in order by the consumer (the audio thread) with
// Drain - the role of the hardware's PIO FIFO. Not thread-safe by itself: Drain and the writes need one lock.
class VpFifo : public VpPio {
public:
    void Write(u32 offset, u32 value) override { pending_.push_back({offset, value}); }
    void Drain(VpFrontEnd& fe) {
        for (const auto& [o, v] : pending_) fe.Write(o, v);
        pending_.clear();
    }
    bool Empty() const { return pending_.empty(); }
    size_t Size() const { return pending_.size(); }
    // Moves `from`'s writes behind this FIFO's (a producer's batch handed to the consumer's queue).
    void Splice(VpFifo& from) {
        pending_.insert(pending_.end(), from.pending_.begin(), from.pending_.end());
        from.pending_.clear();
    }

private:
    std::vector<std::pair<u32, u32>> pending_;
};

}  // namespace fut::audio::vp

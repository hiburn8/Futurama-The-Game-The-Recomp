// The port's path into the VP model: turns the backend's buffer operations and the register values of
// xds::DirectSound (src/audio/xdsound) into the PIO method writes the XDK library makes (VpFrontEnd.h), so that the
// VP renders from the same register state as on the Xbox. docs/analysis/audio_port.md 5.5 (integration).
//
// Exact (compared word for word against the original under Unicorn, audio_port.md 5.3): the parameter registers
// VBIN, the FMT slot bins, VOLA / VOLB / VOLC, PITCH, FCB and the HRIR entry words. Not the original's (not
// observable in the output): the hardware voice numbers, the SGE indices, the order of the set-up writes; the FMT
// format bits of 16-bit PCM (not in the oracle's data; L).
#pragma once

#include "audio/vp/Vp.h"
#include "audio/xdsound/XdsDirectSound.h"

#include <map>
#include <vector>

namespace fut::audio::vp {

class VpBridge {
public:
    // `pio`: the VpFrontEnd the Vp renders from, or a VpFifo drained into it at frame boundaries.
    VpBridge(VpPio& pio, VpMemoryMap& mem, const xds::Tables& tables);

    // The library's hardware set-up as far as the VP uses it (H / O: CMcpxAPU init): mix-bin headroom 1 on bins
    // 0-30, 0 on bin 31; the HRTF output bins {6, 8, 7, 9}.
    void InitHardware();

    struct Buffer {
        u16 formatTag = 1, channels = 1, bits = 16;  // WAVEFORMATEX
        bool is3D = false;
        const u8* data = nullptr;                    // the SetBufferData bytes (kept by the caller while playing)
        u32 bytes = 0;
    };
    // Play: allocates a hardware voice (3D: 0-63, 2D: 64-255, lowest free), maps the data, writes the set-up and
    // the parameters, VOICE_ON and runs it. `id` is the caller's buffer id.
    void Play(u32 id, const Buffer& b, bool loop, const xds::VoiceRegisters& regs);
    // The parameter registers after a DirectSound setter / commit / Play (only while playing; writes what changed).
    void Update(u32 id, const xds::VoiceRegisters& regs);
    void Stop(u32 id);                              // VOICE_OFF, frees the voice and the mapping
    // The current buffer offset (CBO) of a playing voice, in samples: the end of the library's resume path
    // 0x0024DA03 (Play of a buffer whose Stop saved a position: the voice locked, CBO = the saved position, unlocked;
    // L: its silent start on a 0x20-sample stand-in buffer before the real set-up is not modelled, the VP renders
    // the registers of a frame together).
    void SetPosition(u32 id, u32 sample);
    // VOICE_PAUSE with bit 18 set / clear: the voice keeps its position (only while playing; L: the stand-in for a
    // stream's DSSTREAMPAUSE_PAUSE / _RESUME, the port playing streaming samples from static buffers)
    void Pause(u32 id, bool pause);
    void Release(u32 id);                           // Stop (the buffer's filter registers live in xds)
    // A stream's voice (VpStream.h, CMcpxStream): allocated and set up as 0x0024CE8A writes it (FMT with the stream
    // bit 24, MISC 0x00800000, the envelope / LFO / filter words 0, the parameters, HRTF 0xFFFF; no buffer words);
    // the stream's packets reach it as SSL segments. StartStream: 0x0024D621's VOICE_ON (FMT | 0x00800000, the list,
    // the pause bit); StopStream: 0x0024D2BC's VOICE_OFF (FMT without bits 23 / 25), the voice kept; Release frees it.
    bool CreateStream(u32 id, const Buffer& format, const xds::VoiceRegisters& regs);
    void StartStream(u32 id, bool paused);
    void StopStream(u32 id);
    bool Playing(u32 id) const { return voices_.count(id) != 0; }
    // True once a one-shot voice played past its end in `vp` (the backend's GetStatus).
    bool Ended(u32 id, const Vp& vp) const;
    u32 HardwareVoice(u32 id) const;                // kVoices when not playing
    // The APU's count of voices paused by DSSTREAMPAUSE_SYNCHPLAYBACK (CMcpxAPU +0x774: 0x0024AC5B adds a voice's
    // count, at most 0x1D in all; 0x0024AC82 subtracts it; SynchPlayback 0x0024AC93 clears it).
    u32 synchPaused = 0;

    // The register words of a voice from xds's values (also used by the oracle test).
    static VoiceRegs Encode(const xds::VoiceRegisters& regs, bool is3D, u32 fmtFormatBits);
    static std::array<u32, 16> HrirWords(const xds::VoiceRegisters& regs, const xds::Tables& tables);
    static u32 FormatBits(const Buffer& b, bool loop);

private:
    struct Voice {
        u32 hw = 0;
        bool is3D = false;
        u32 phys = 0, fmtBits = 0, miscBits = 0;
        u32 sge = 0, pages = 0;                     // the voice's SGE entries (freed by Stop)
        u32 hrirToggle = 0;
        VoiceRegs last;
        std::array<u32, 16> hrir{};
        bool hrirValid = false;
    };
    void W(u32 offset, u32 value) { fe_.Write(offset, value); }
    u32 AllocateSges(u32 pages);                    // first fit from entry 1; 0 = no room
    void WriteParameters(Voice& v, const xds::VoiceRegisters& regs, bool all);

    VpPio& fe_;
    VpMemoryMap& mem_;
    const xds::Tables& tables_;
    std::map<u32, Voice> voices_;                  // caller id -> voice
    std::array<bool, kVoices> used_{};
    std::vector<bool> sgeUsed_ = std::vector<bool>(kModelSges, false);   // (entry 0 never used)
};

}  // namespace fut::audio::vp

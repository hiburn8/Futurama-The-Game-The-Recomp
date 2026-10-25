// The Xbox audio DSP path behind the voice processor, for the audio backend: the GP (McpxGp: the DSOUND executive
// and dsstdfx.bin effects 0-9 interpreted from the user's files) and the EP's PCM down-mix (McpxEp).
// docs/analysis/audio_dsp_port.md 11 (status, integration).
//
//   AudioDsp dsp;  dsp.Init(dsstdfxBytes, xbeBytes, &why);
//   per 32-sample frame at 48 kHz (1,500 / s):  dsp.ProcessFrame(bins, out)  with bins = the 32 mix bins the VP
//   model produced for those 32 samples (24-bit two's complement); out.speakers = bins 0-5 after the effects
//   (FL FR C LFE BL BR, the AC-3 encoder's input on hardware), out.stereo = module 6 (L, R), out.mono = module 5.
//   on an I3DL2 listener change: dsp.CommitI3DL2Reverb(xds::DirectSound::ReverbParameters()) before the next frame.
#pragma once

#include "audio/dsp/DsImage.h"
#include "audio/dsp/McpxEp.h"
#include "audio/dsp/McpxGp.h"

#include <string>
#include <vector>

namespace fut::audio::dsp {

class AudioDsp {
public:
    static constexpr u32 kSampleRate = 48000, kFrameSamples = 32, kBins = 32;

    struct Frame {
        s32 speakers[6][kFrameSamples];          // bin order: FL FR C LFE BL BR
        s32 stereo[2][kFrameSamples];            // EP module 6: L, R
        s32 mono[kFrameSamples];                 // EP module 5
    };

    AudioDsp() = default;
    AudioDsp(const AudioDsp&) = delete;          // the GP keeps a pointer to image_
    AudioDsp& operator=(const AudioDsp&) = delete;

    bool Init(const std::vector<u8>& dsstdfx, const std::vector<u8>& xbe, std::string* error = nullptr);
    bool Ready() const { return gp_.Ready() && ep_.Loaded(); }

    // One GP frame. `bins[b]` may be null (silent bin). False if the interpreter stopped (gp().Error()).
    bool ProcessFrame(const s32* const bins[kBins], Frame& out);

    // The listener commit's reverb update (orig: 0x0024A997): word 4 | 4 and the 66 converted words 0x46-0x87 of
    // effect 0 through the deferred SetEffectData path and the mailbox; heard from the next frame on, with the
    // DSP's own ~323-frame cross-fade.
    void CommitI3DL2Reverb(const u32 params[66]) { gp_.CommitI3DL2Reverb(params); }

    // The GP's fast path (McpxGp::SetSkipIdleEffects): recommended for the game, whose FX-send bins are silent.
    void SetSkipIdleEffects(bool on) { gp_.SetSkipIdleEffects(on); }

    McpxGp& Gp() { return gp_; }
    const McpxEp& Ep() const { return ep_; }
    const DsImage& Image() const { return image_; }

private:
    DsImage image_;
    McpxGp gp_;
    McpxEp ep_;
};

}  // namespace fut::audio::dsp

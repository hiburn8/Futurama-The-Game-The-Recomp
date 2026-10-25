#include "audio/dsp/AudioDsp.h"

namespace fut::audio::dsp {

bool AudioDsp::Init(const std::vector<u8>& dsstdfx, const std::vector<u8>& xbe, std::string* error) {
    if (!image_.Load(dsstdfx, xbe, error) || !ep_.LoadFromXbe(xbe, error)) return false;
    if (!gp_.Init(image_)) {
        if (error) *error = gp_.Error();
        return false;
    }
    return true;
}

bool AudioDsp::ProcessFrame(const s32* const bins[kBins], Frame& out) {
    for (u32 b = 0; b < kBins; ++b)
        if (bins[b]) gp_.SetBin(b, bins[b]);
    if (!gp_.RunFrame()) return false;
    // the six rings as the EP reads them (AC-3 order FL C FR BL BR LFE)
    s32 ring[6][kFrameSamples];
    for (u32 k = 0; k < 6; ++k) gp_.ReadRing(k, ring[k]);
    for (u32 k = 0; k < 6; ++k)
        for (u32 i = 0; i < kFrameSamples; ++i) out.speakers[McpxGp::kAc3Order[k]][i] = ring[k][i];
    ep_.Stereo(ring[0], ring[1], ring[2], ring[3], ring[4], kFrameSamples, out.stereo[0], out.stereo[1]);
    ep_.Mono(ring[0], ring[1], ring[2], ring[3], ring[4], kFrameSamples, out.mono);
    return true;
}

}  // namespace fut::audio::dsp

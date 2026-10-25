// The MCPX "EP" (encode processor) output stage for the speaker configurations without AC-3 (docs/analysis/
// audio_dsp_port.md 6): DSOUND loads the EP with the DOLBY section's boot / service program and its overlay modules
// and, for stereo (mono), sets configuration word 3 = 2 (1), which makes the EP boot run module 6 (5) on every
// 256-sample block of the GP's six speaker rings. Not interpreted (the DOLBY code is large and only these modules
// matter): the down-mix is written here from the modules' code, with the coefficients read from the user's
// default.xbe (each module's set-up block `move #c,x0; move x0,y:$k`), nothing embedded.
//
//   module 6 (stereo, 0x003175D8): a = FL*Y0 + C*Y2 + FR*Y4 + BL*Y6 + BR*Y8 (mpy, mac x3, macr), b likewise with
//     Y1 Y3 Y5 Y7 Y9; L = lim(a), R = lim(b). The game's words: L = 0x7FFFFF FL + 0x5A8279 C + 0x5A8279 BL,
//     R = 0x7FFFFF FR + 0x5A8279 C + 0x5A8279 BR (LFE, ring 5, is not read).
//   module 5 (mono, 0x003172E8): a = FL*Y0 + C*Y1 + FR*Y2 + BL*Y3 + BR*Y4 (macr last); both outputs = lim(a).
// Each sample is independent (no state), so the down-mix can run per GP frame. Uncertain (L): which module DSOUND's
// configuration selects (H for the code, L for the EP services: channel n = ring n, the output = the AC97 analog
// pair), the EP's 256-sample block latency (not modelled), the coefficient scaling by configuration words 16 / 17
// (word 16 = 0 in DSOUND's default block; not modelled), no SM / scaling on the EP.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace fut::audio::dsp {

class McpxEp {
public:
    static constexpr u32 kStereoModuleVa = 0x003175D8;   // DOLBY: overlay module 6
    static constexpr u32 kMonoModuleVa = 0x003172E8;     // DOLBY: overlay module 5

    bool LoadFromXbe(const std::vector<u8>& xbe, std::string* error = nullptr);
    bool Loaded() const { return loaded_; }

    // Inputs: the EP channels 0-4 = rings 0-4 = FL, C, FR, BL, BR (24-bit two's complement), n samples each.
    void Stereo(const s32* fl, const s32* c, const s32* fr, const s32* bl, const s32* br, u32 n, s32* outL,
                s32* outR) const;
    void Mono(const s32* fl, const s32* c, const s32* fr, const s32* bl, const s32* br, u32 n, s32* out) const;

    const u32* StereoCoefficients() const { return stereo_; }   // Y:0..9
    const u32* MonoCoefficients() const { return mono_; }       // Y:0..4

private:
    u32 stereo_[10] = {}, mono_[5] = {};
    bool loaded_ = false;
};

}  // namespace fut::audio::dsp

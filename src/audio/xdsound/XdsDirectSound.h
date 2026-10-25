// The state side of the XDK DirectSound library for the calls the game makes (docs/analysis/audio_port.md 1.3, 2.1):
// the listener (DS3DLISTENER, I3DL2 listener, right vector, deferred dirty bits), the voices' settings (format rate,
// pitch, volume with headroom, mix bins and their volumes, the 3D block with its rolloff curve and I3DL2 source),
// and per voice the values the original writes to the MCPX voice registers, computed by XdsMath:
// VOLA / VOLB / VOLC (8 slot attenuations per hardware voice), VBIN (the 8 slot bins), PITCH, the I3DL2 filter
// coefficients, the HRTF filter pair; plus the reverb (DSP effect 0) state words from the I3DL2 listener.
//
// Commit semantics: setters store at once (as the original's settings objects); deferred 3D settings are calculated
// at CommitDeferredSettings (0x00247AEB) or at once with DS3D_IMMEDIATE. The port runs every calculator stage on a
// commit (mask 0xFF) where the original runs those of the voice's dirty mask: the masks of the setters the game
// uses cover every stage whose inputs they change (H: position 0x1FF, min / max / curve 4, velocity 0x80, I3DL2
// source 0x100, listener orientation 0x466, distance factor 0xE0, rolloff 4, Doppler 0x80), so the results are the
// same. The I3DL2 source levels are recomputed when the original's bit 0x100 is set (position, I3DL2 source, I3DL2
// listener), as in Commit3dSettings 0x0024D3F9.
// A voice's 3D settings are committed only while it plays (0x0024D3F9 tests the voice client's allocated + playing
// bits): a commit of a stopped voice leaves its dirty mask, the listener's bits included (0x00247274: dirty |=
// listener bits, then &= ~ the mask the commit used), for the commit Play runs (CMcpxBuffer_Play 0x0024E03E ->
// 0x0024D621 -> Commit3dSettings with the voice's mask, not cleared). The register update after a commit
// (0x0024D20D) calls SetFilter (0x0024CC6F: MISC filter mode 3 for 3D voices, FCB from the I3DL2 coefficients) only
// when the I3DL2 coefficients changed; MISC / FCB keep their values over Stop / Play (the voice client's fields).
//
// NOT PORTED (unused by the game, docs/analysis/audio_port.md 1.3): cones / modes / per-buffer factors setters
// (defaults stay; so the commit's DS3DMODE change test, 0x0024D42F, never fires), headroom setters, envelopes /
// LFOs, FX sends, output buffers (submixing), streams' packet queue, the resume position of a stopped buffer
// (0x0024E0EE stores it, Play restarts there: the backends play from the start); the hardware voice numbers.
#pragma once

#include "audio/xdsound/XdsMath.h"

#include <array>
#include <map>
#include <vector>

namespace fut::audio::xds {

struct MixBinVolume {
    u32 bin;
    s32 volume;
};

// The register values of one DirectSound voice (buffer or stream).
struct VoiceRegisters {
    // Hardware voices: ((channels - 1) >> 1) + 1 (0x0024C213), so 1 for mono and stereo buffers (a stereo voice
    // plays both channels; its slots are the buffer's bins in order); voice k owns slots [k*n/h, (k+1)*n/h).
    u32 hwVoices = 1;
    u32 volume[9] = {};                          // VOLA [k], VOLB [3 + k], VOLC [6 + k]
    u8 slotBins[8] = {};                         // VBIN: the bin of each slot (unused slots: the lowest free bins)
    u32 pitch = 0;                               // PITCH: (1/4096 octave vs 48 kHz) << 16
    u32 directHFCoef = 0, roomHFCoef = 0;        // I3DL2 filter coefficients (3D voices; the last commit's)
    u32 misc = 0;                                // MISC (voice client +0x18): filter mode << 16 as SetFilter left it
    u32 fcb = 0;                                 // FCB (+0x3C): room HF << 16 | direct HF as SetFilter left it
    u32 hrtf0 = kHrtfDefault, hrtf1 = kHrtfDefault;   // HRTF filter pair (3D voices)
    f32 azimuth = 0, elevation = 0;              // the calculator's angles (for the HRTF stand-in)
    // Slot attenuation of hardware voice `k`, slot `i` (12 bits, 1/64 dB; 0xFFF the most).
    u16 Attenuation(u32 k, u32 i) const;
    // Playback rate in Hz the pitch register gives (48000 * 2^(pitch / 4096)).
    double Rate() const;
};

class DirectSound {
public:
    explicit DirectSound(const Tables* tables = nullptr);

    // Speaker configuration (XGetAudioFlags: surround / AC3 enable the rear split and the HRTF rear folding).
    void SetRearSpeakers(bool rear) { frame_.rearSpeakers = rear ? 1 : 0; }

    // ---- IDirectSound (apply: 0 DS3D_IMMEDIATE, 1 DS3D_DEFERRED)
    void SetDopplerFactor(f32 f, u32 apply);
    void SetDistanceFactor(f32 f, u32 apply);
    void SetRolloffFactor(f32 f, u32 apply);
    void SetPosition(f32 x, f32 y, f32 z, u32 apply);
    void SetVelocity(f32 x, f32 y, f32 z, u32 apply);
    void SetOrientation(f32 fx, f32 fy, f32 fz, f32 tx, f32 ty, f32 tz, u32 apply);
    void SetI3DL2Listener(const I3DL2Listener& l, u32 apply);
    void CommitDeferredSettings();

    // ---- buffers (ids from CreateVoice; 0 = none)
    // flags: DSBCAPS (CTRL3D 0x10, MUTE3DATMAXDISTANCE 0x20000); the default mix bins (lpMixBins NULL) are set.
    u32 CreateVoice(u32 flags, u16 channels, u32 sampleRate);
    void ReleaseVoice(u32 id);
    // Play: a voice that is not playing yet starts, and a 3D voice commits its pending 3D settings (0x0024D621).
    // Stop: the voice stops (its 3D settings are no longer committed until the next Play). Also for
    // SetBufferData(NULL) and Release, which stop the voice first.
    void Play(u32 id);
    void Stop(u32 id);
    bool Playing(u32 id) const;
    void SetVolume(u32 id, s32 volume);
    void SetFrequency(u32 id, u32 hz);
    void SetMixBins(u32 id, const MixBinVolume* bins, u32 count);
    void SetBufferPosition(u32 id, f32 x, f32 y, f32 z, u32 apply);
    void SetBufferVelocity(u32 id, f32 x, f32 y, f32 z, u32 apply);
    void SetMinDistance(u32 id, f32 d, u32 apply);
    void SetMaxDistance(u32 id, f32 d, u32 apply);
    void SetRolloffCurve(u32 id, const f32* points, u32 count, u32 apply);
    void SetI3DL2Source(u32 id, const I3DL2Source& s, u32 apply);

    const VoiceRegisters* Registers(u32 id) const;
    const Listener& GetListener() const { return listener_; }
    const I3DL2Listener& GetI3DL2Listener() const { return i3dl2_; }
    // Effect 0's converted parameters (converter words 71..136, i.e. DSP state +0x118..+0x21F) and a counter that
    // changes whenever they are recomputed.
    const u32* ReverbParameters() const { return &reverb_[71]; }
    u32 ReverbVersion() const { return reverbVersion_; }
    // DownloadEffectsImage: effect 0's state as downloaded (ReadReverbState). Without it the listener commit skips
    // the conversion, as the original does without an image (effect index -1, 0x0024A9BC).
    void SetReverbState(const u32 words[70]);

private:
    struct Voice {
        VoiceSettings settings;
        u16 channels = 1;
        bool is3D = false;
        Buffer3D p3d;
        std::vector<f32> curve;                  // (the original keeps the caller's pointer; the game's curve is
                                                 //  a global written once, so a copy is equivalent)
        u32 dirty3D = 0x1FF;                     // the 3D block's dirty mask (+0x78)
        bool playing = false;                    // the voice client's allocated + playing bits (+0x12 & 3 == 3)
        Source src;
        I3DL2Voice i3;
        VoiceRegisters regs;
    };
    void CommitListener();
    u32 Commit3D(Voice& v);
    void CommitVoice(Voice& v);
    void SetFilter(Voice& v);
    void UpdateVolume(Voice& v);
    void UpdatePitch(Voice& v);
    void UpdateMixBins(Voice& v);
    void Changed3D(Voice& v, u32 bits, u32 apply);
    void ChangedListener(u32 bits, u32 apply);
    Voice* Find(u32 id);

    const Tables* tables_;
    Tables none_;
    Listener listener_;
    ListenerFrame frame_;
    I3DL2Listener i3dl2_;
    u32 listenerDirty_ = 0;
    std::array<u32, 137> reverb_{};
    std::array<u32, 70> reverbState_{};
    bool haveReverbState_ = false;
    u32 reverbVersion_ = 0;
    std::map<u32, Voice> voices_;
    u32 next_ = 1;
};

// The DSP state words 0..0x45 of dsstdfx.bin effect 0 (the I3DL2 reverb), read from the image bytes (effect map
// at 0x6110, DSEFFECTMAP state offset); false if the image is not the expected one.
bool ReadReverbState(const std::vector<u8>& dsstdfx, u32 words[70]);

}  // namespace fut::audio::xds

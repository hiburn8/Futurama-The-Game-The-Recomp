// Shared realisation of the game sound layer's DirectSound calls (sound::SoundBackend). The DirectSound side is the
// port of the XDK library (src/audio/xdsound: listener / buffer state, the 3D calculator, I3DL2 source levels,
// volume / pitch / mix-bin / filter register values, exact against the original). Two realisations:
//
// Output: XAudio2 on Windows; SDL3 audio stream on Linux. The Windows-only
// LegacyVoices comparison path is not available on Linux. Linux initialization errors
// fail explicitly rather than selecting a silent or approximate fallback.
//
// Mode::Vp (default; docs/analysis/audio_port.md 5.5): the hardware path. The register values become the PIO method
// writes the XDK library makes (audio::vp::VpBridge) into a FIFO that DoWork (called by the game once per frame,
// after CommitDeferredSettings) publishes to the audio thread; the audio thread applies them to the VP front end
// and, per 32-sample frame at 48 kHz, mixes every voice into the 32 integer mix bins (audio::vp::Vp: fetch PCM 8 /
// 16 / Xbox ADPCM, resampler, HRTF FIR + ITD, I3DL2 filters, 12-bit attenuations, bin headroom), runs the GP effects
// on them (audio::dsp::AudioDsp: dsstdfx.bin effects 0-9 interpreted, the I3DL2 reverb committed through
// McpxGp::CommitI3DL2Reverb) and takes the EP's stereo down-mix (or the six speaker bins with `surround`) into one
// 48 kHz float XAudio2 source voice (v / 2^23). XAudio2 buffers of 16 frames (512 samples, 10.7 ms), 3 queued, are
// rendered in chunks of 4 frames under the lock the game thread takes to map / unmap sample data, publish a frame
// or read a voice's status. Latency ~32 ms + up to one game frame (the publication). The hardware behaviour the XBE
// does not fix is the VP model's (L) list (audio_port.md 5.4) and the DSP's (audio_dsp_port.md 11.4).
// Mode::LegacyVoices (for comparison; KNOWN_ISSUES #25): one XAudio2 source voice per buffer with the register
// values as per-bin gains, stand-ins for HRTF (equal-power pan), crosstalk (pass-through), reverb (XAudio2's),
// no I3DL2 filters, XAudio2's resampler.
//
// InitOffline() runs the Vp path without a device: RenderFrames() renders on the calling thread (tests, WAV dumps).
#pragma once

#include "audio/xdsound/XdsDirectSound.h"
#include "game/sound/SoundSystem.h"

#include <memory>

namespace fut::audio::vp {
struct Levels;
}

namespace fut::platform {

class GameSoundBackend : public sound::SoundBackend {
public:
    enum class Mode { Vp, LegacyVoices };
    struct Options {
        Mode mode = Mode::Vp;
        // (L) the console's speaker configuration: false = stereo (the dashboard default: no rear split, the EP's
        // stereo down-mix), true = surround (XGetAudioFlags 2: the 3D rear split, the six speaker bins out).
        bool surround = false;
        // The deterministic voice status (docs/analysis/level_sound_port.md 3.4, Mode::Vp): GetStatus answers from a
        // game-thread cursor instead of the audio thread's VP state, so the game logic that waits for a voice's end
        // (Voice_Exists gates, the scripts' FsIsPlayingSound loops) does not depend on wall-clock timing. At a Play
        // that starts the voice the cursor is 0; every DoWork (one per game step: App vf28 -> the sound system's
        // Update) advances a playing voice by one 1/60 s step at its frequency (SetFrequency's Hz / 60 samples per
        // channel); a one-shot voice reads as stopped once the cursor reaches its length (the SetBufferData bytes in
        // samples per channel: PCM bytes / (channels * bits / 8), Xbox ADPCM bytes / (36 * channels) * 64); a looping
        // one never ends; Stop / SetBufferData end it at once. (L): the console's voices end when the VP passes the
        // end (L8 of audio_port.md 5.4) and the library's GetStatus sees it, relative to the game's step (+-1 step
        // against this model at 60 Hz); the audio thread still renders the voice and is cut when the game deletes it.
        bool stepStatus = false;
    };
    GameSoundBackend();
    explicit GameSoundBackend(const Options& options);
    ~GameSoundBackend() override;
    // Query on the host loop; false after a DSP or output-thread failure.
    bool Healthy() const;
    bool Init();                                     // false without an audio device
    bool InitOffline();                              // Mode::Vp without a device (RenderFrames)
    // Game data the hardware path needs: default.xbe (the HRTF tables, the GP executive, the EP coefficients) and
    // xbox/dsstdfx.bin (the DSP effects; effect 0's state for the reverb conversion). Both before the sound system
    // is created (its DownloadEffectsImage starts the DSP; without them Mode::Vp falls back to LegacyVoices).
    void LoadXbeTables(const std::vector<u8>& xbe);
    void SetEffectsImage(const std::vector<u8>& dsstdfx);
    const audio::xds::DirectSound& Model() const;
    Mode CurrentMode() const;

    // Offline: renders `frames` 32-sample frames of the published state and appends the output samples (24-bit
    // values, interleaved: L R, or FL FR C LFE BL BR with `surround`). False if the DSP stopped.
    bool RenderFrames(u32 frames, std::vector<s32>& out);
    // Mode::Vp: accumulates the mix stages' levels (the VP's, the GP's speaker bins, the EP's stereo) into `levels`
    // from the next frame on (nullptr: off; tests / level analysis).
    void SetLevels(audio::vp::Levels* levels);
    // Mode::Vp: whether the hardware path has the buffer's voice on (the bridge's view, not the step status), and the
    // output's channel count (diagnostics: fut_game's --sound-log / --audio-out).
    bool HardwarePlaying(Buffer b);
    u32 OutputChannels() const;

    void DownloadEffectsImage() override;
    void UseFullHrtf() override {}                   // (the full-HRTF calculator is the one ported)
    void SetDopplerFactor(f32 f, u32 apply) override;
    void SetDistanceFactor(f32 f, u32 apply) override;
    void SetRolloffFactor(f32 f, u32 apply) override;
    void SetI3DL2Listener(u32 preset, u32 apply) override;
    void ListenerPosition(f32 x, f32 y, f32 z, u32 apply) override;
    void ListenerVelocity(f32 x, f32 y, f32 z, u32 apply) override;
    void ListenerOrientation(f32 fx, f32 fy, f32 fz, f32 tx, f32 ty, f32 tz, u32 apply) override;
    void CommitDeferredSettings() override;
    void DoWork() override;
    Buffer CreateBuffer(u32 flags, const std::vector<u8>& format) override;
    void SetBufferData(Buffer b, const u8* data, u32 bytes) override;
    void Play(Buffer b, u32 flags) override;
    void Stop(Buffer b) override;
    // The sound layer's streams (Mode::Vp; LegacyVoices: none): the library's CMcpxStream (audio/vp/VpStream.h): the
    // packets as SSL segments of the stream voice, refilled after the VP's notifier interrupt (serviced at DoWork),
    // completed by DoWork's deferred command (docs/analysis/audio_port.md 5.8).
    Buffer CreateStreamVoice(u32 flags, u32 maxPackets, const std::vector<u8>& format) override;
    void ProcessPacket(Buffer s, const u8* data, u32 size, u32* status) override;
    void PauseStream(Buffer b, bool pause) override;
    u32 GetStatus(Buffer b) override;
    void Release(Buffer b) override;
    void SetVolume(Buffer b, s32 volume) override;
    void SetFrequency(Buffer b, u32 hz) override;
    void SetMixBins(Buffer b, const sound::MixBin* bins, u32 count) override;
    void SetMinDistance(Buffer b, f32 d, u32 apply) override;
    void SetMaxDistance(Buffer b, f32 d, u32 apply) override;
    void SetPosition(Buffer b, f32 x, f32 y, f32 z, u32 apply) override;
    void SetVelocity(Buffer b, f32 x, f32 y, f32 z, u32 apply) override;
    void SetRolloffCurve(Buffer b, const f32* points, u32 count, u32 apply) override;
    void SetI3DL2Source(Buffer b, const std::array<s32, 9>& params, u32 apply) override;
    // The streams (the movie player's audio; GameSoundBackend.cpp XAudio2Stream).
    std::unique_ptr<sound::SoundStream> CreateStream(const std::vector<u8>& format, StreamCallback completed) override;
    s32 SynchPlayback() override;
    void DoStreamWork() override;

    // LegacyVoices: the per-bin linear gains of source channel `channel` of a `channels`-channel buffer (32 bins)
    // as applied to the output (for tests). A stereo buffer is one hardware voice: its even slots carry the left
    // channel, its odd slots the right (L6, as the VP model).
    static void BinGains(const audio::xds::VoiceRegisters& r, u32 channels, u32 channel, f32 gains[32]);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace fut::platform

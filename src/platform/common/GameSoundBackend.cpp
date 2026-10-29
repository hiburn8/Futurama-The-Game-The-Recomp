// Shared Xbox VP/GP/EP sound backend, with XAudio2 or SDL output; see GameSoundBackend.h.
#include "platform/common/GameSoundBackend.h"

#include "audio/Wav.h"
#include "audio/dsp/AudioDsp.h"
#include "audio/vp/Vp.h"
#include "audio/vp/VpBridge.h"
#include "audio/vp/VpStream.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>
#include <xaudio2fx.h>
#else
#include <SDL3/SDL.h>
#endif
#include <chrono>
#include <stdexcept>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fut::platform {

namespace xds = audio::xds;
namespace vp = audio::vp;
namespace dsp = audio::dsp;

namespace {

constexpr u32 kFrame = 32;                           // samples per VP / GP frame (48 kHz)
constexpr u32 kFramesPerBuffer = 16;                 // one XAudio2 buffer: 512 samples, 10.7 ms
constexpr u32 kFramesPerChunk = 4;                   // rendered under the lock (2.7 ms of audio)
constexpr u32 kBuffersQueued = 3;

}  // namespace

class XAudio2Stream;

struct GameSoundBackend::Impl
#ifdef _WIN32
    : public IXAudio2VoiceCallback
#endif
{
    Options opt;
    Mode mode = Mode::Vp;
#ifdef _WIN32
    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    u32 masterChannels = 2, channelMask = 0x3;       // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
    u32 deviceRate = 48000;
    bool comInit = false;
#endif
    bool offline = false;
#ifndef _WIN32
    SDL_AudioStream* output = nullptr;
    bool audioInit = false;
    std::atomic<bool> outputFailed{false};
#endif
    xds::Tables tables;
    xds::DirectSound ds{&tables};
    std::vector<u8> xbe, dsstdfx;
    struct Voice {
        u32 flags = 0;
        u16 format = 1, channels = 1, bits = 16;
        u16 blockAlign = 0;                          // WAVEFORMATEX nBlockAlign (the library's format +0x14)
        u32 rate = 0;
        // Mode::Vp: the position CMcpxBuffer_Stop 0x0024E0EE saved (+0x150, bytes: GetCurrentPosition 0x0024DC37's
        // play cursor), where the next Play resumes (0x0024E0A7 -> 0x0024DA03); 0 = from the start.
        u32 resume = 0;
        bool is3D = false;
        std::vector<u8> raw;                         // Vp: the SetBufferData bytes (ADPCM undecoded; the VP decodes)
        std::vector<s16> pcm;                        // LegacyVoices: decoded
#ifdef _WIN32
        IXAudio2SourceVoice* voice = nullptr;
        f32 maxRatio = 1;
        bool started = false, looping = false;
#endif
        // Options::stepStatus (the deterministic status): the game thread's cursor of the voice.
        bool stepPlaying = false, stepLoop = false;
        double stepCursor = 0, stepLength = 0;
        bool stepPaused = false;                     // (PauseStream: the cursor holds)
        u32 stepHz = 0;
        // A stream of the sound layer (CreateStreamVoice, Mode::Vp): the library's CMcpxStream (VpStream.h) on
        // the bridge's stream voice (game thread; its packet mappings and notifiers under `lock`).
        bool stream = false;
        std::unique_ptr<vp::McpxStream> mcpx;
    };
    std::unordered_map<Buffer, Voice> voices;
#ifdef _WIN32
    std::vector<XAudio2Stream*> streams;             // the movie streams (game thread)

#endif
    // ---- Mode::Vp. The game thread owns ds, the bridge and gameFifo; `lock` guards the rest that both threads
    // touch: the memory map (Map / Unmap at Play / Stop), the published FIFO and reverb words, the front end / VP /
    // DSP (the audio thread's drain + render; GetStatus reads the VP).
    std::mutex lock;
    vp::VpFrontEnd fe;
    vp::VpFifo gameFifo;                             // the method writes since the last DoWork
    vp::VpFifo published;                            // handed over at DoWork, drained before the next chunk
    vp::VpMemoryMap mem;
    vp::VpBridge bridge{gameFifo, mem, tables};
    vp::Vp vpModel{fe, mem};
    vp::VpNotifierMemory notifiers;                  // the voices' notifiers (written by the VP and the streams)
    dsp::AudioDsp dsp;
    u32 reverbVersion = 0;
    std::optional<std::array<u32, 66>> reverbGame, reverbPublished;
    bool dspFailed = false;
    vp::Levels* levels = nullptr;                    // SetLevels
    u32 outChannels = 2;
#ifdef _WIN32
    IXAudio2SourceVoice* out = nullptr;
#endif
    std::vector<std::vector<f32>> ring;              // the XAudio2 buffers' memory (kBuffersQueued + 1)
    u32 ringNext = 0;
    std::vector<s32> chunk;
    std::thread thread;
    std::atomic<bool> running{false};
#ifdef _WIN32
    HANDLE wake = nullptr;

    // ---- LegacyVoices
    IXAudio2SubmixVoice* reverb = nullptr;           // I3DL2 bin 10 (stand-in for dsstdfx.bin effect 0)
#endif
    bool effectsImage = false;                       // DownloadEffectsImage done: the I3DL2 bin is processed

#ifdef _WIN32
    // IXAudio2VoiceCallback (XAudio2's thread): a buffer finished, the audio thread renders the next.
    void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override {
        if (wake) SetEvent(wake);
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}

#endif
    // ---- Mode::Vp -------------------------------------------------------------------------------------------------
    // Starts the hardware path: the DSP from the game's files, the library's VP set-up (published at once).
    bool StartVp() {
        std::string why;
        if (xbe.empty() || dsstdfx.empty()) why = "default.xbe / xbox/dsstdfx.bin not given";
        else if (!tables.Loaded()) why = "default.xbe: no HRTF tables";
        else if (!dsp.Init(dsstdfx, xbe, &why)) why = "AudioDsp: " + why;
        if (!why.empty()) {
            std::fprintf(stderr, "audio: hardware path unavailable (%s); output initialization failed\n",
                         why.c_str());
            return false;
        }
        dsp.SetSkipIdleEffects(true);                // the VP model never writes bins 11-31 for this game
        if (opt.surround) ds.SetRearSpeakers(true);
        bridge.InitHardware();
        std::lock_guard g(lock);
        published.Splice(gameFifo);
        published.Drain(fe);
        return true;
    }

    // Under `lock`: the published writes reach the front end and a published reverb commit the GP (between two
    // frames, as the original's mailbox), then `frames` frames are mixed into `dst` (outChannels per sample).
    bool RenderLocked(u32 frames, s32* dst) {
        published.Drain(fe);
        if (reverbPublished) {
            dsp.CommitI3DL2Reverb(reverbPublished->data());   // the live word 4 | 4 (audio_dsp_port.md 11.2)
            reverbPublished.reset();
        }
        for (u32 f = 0; f < frames; ++f, dst += kFrame * outChannels) {
            vp::Vp::Bins bins;
            vpModel.RenderFrame(bins);
            dsp::AudioDsp::Frame o;
            if (!dspFailed) {
                const s32* in[32];
                for (u32 b = 0; b < 32; ++b) in[b] = bins[b].data();
                if (!dsp.ProcessFrame(in, o)) {
                    dspFailed = true;
                    std::fprintf(stderr, "audio: the DSP stopped: %s\n", dsp.Gp().Error().c_str());
                }
            }
            for (u32 i = 0; i < kFrame; ++i)
                for (u32 c = 0; c < outChannels; ++c)
                    dst[i * outChannels + c] = dspFailed ? 0 : outChannels == 6 ? o.speakers[c][i] : o.stereo[c][i];
            if (levels && !dspFailed)
                for (u32 i = 0; i < kFrame; ++i) {
                    for (u32 c = 0; c < 6; ++c) levels->speakers[c].Add(o.speakers[c][i], 0x800000);
                    for (u32 c = 0; c < 2; ++c) levels->stereo[c].Add(o.stereo[c][i], 0x800000);
                }
        }
        return !dspFailed;
    }

    void RenderBuffer(f32* dst) {
        chunk.resize(static_cast<size_t>(kFramesPerChunk) * kFrame * outChannels);
        for (u32 k = 0; k < kFramesPerBuffer; k += kFramesPerChunk) {
            {
                std::lock_guard g(lock);
                RenderLocked(kFramesPerChunk, chunk.data());
            }
            for (s32 v : chunk) *dst++ = static_cast<f32>(v) / 8388608.0f;   // 24-bit -> float (L 11.4 #11)
        }
    }

#ifdef _WIN32
    void AudioThread() {
        while (running) {
            XAUDIO2_VOICE_STATE st;
            out->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued < kBuffersQueued) {
                std::vector<f32>& b = ring[ringNext];
                ringNext = (ringNext + 1) % static_cast<u32>(ring.size());
                RenderBuffer(b.data());
                XAUDIO2_BUFFER xb = {};
                xb.AudioBytes = static_cast<UINT32>(b.size() * sizeof(f32));
                xb.pAudioData = reinterpret_cast<const BYTE*>(b.data());
                out->SubmitSourceBuffer(&xb);
                continue;
            }
            WaitForSingleObject(wake, 20);
        }
    }

    bool StartOutput() {
        outChannels = opt.surround ? 6 : 2;
        WAVEFORMATEXTENSIBLE wf = {};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = static_cast<WORD>(outChannels);
        wf.Format.nSamplesPerSec = 48000;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = static_cast<WORD>(4 * outChannels);
        wf.Format.nAvgBytesPerSec = 48000 * wf.Format.nBlockAlign;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = outChannels == 6 ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                                                  SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT
                                            : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;   // bin order FL FR C LFE BL BR
        wf.SubFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};   // IEEE float
        if (FAILED(xaudio->CreateSourceVoice(&out, &wf.Format, XAUDIO2_VOICE_NOPITCH, 1.0f, this))) return false;
        ring.assign(kBuffersQueued + 1, std::vector<f32>(static_cast<size_t>(kFramesPerBuffer) * kFrame * outChannels));
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        running = true;
        thread = std::thread([this] { AudioThread(); });
        out->Start(0);
        return true;
    }

#else
    void OutputError() {
        std::fprintf(stderr, "audio: SDL output failed: %s\n", SDL_GetError());
        outputFailed = true;
        running = false;
    }
    void AudioThread() {
        const int bufferBytes = int(kFramesPerBuffer * kFrame * outChannels * sizeof(f32));
        std::vector<f32> buffer(kFramesPerBuffer * kFrame * outChannels);
        while (running) {
            const int queued = SDL_GetAudioStreamQueued(output);
            if (queued < 0) { OutputError(); break; }
            if (queued < int(kBuffersQueued) * bufferBytes) {
                RenderBuffer(buffer.data());
                if (!SDL_PutAudioStreamData(output, buffer.data(), bufferBytes)) { OutputError(); break; }
            } else {
                SDL_Delay(2);
            }
        }
    }
    bool StartOutput() {
        outChannels = opt.surround ? 6 : 2;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, int(outChannels), 48000};
        output = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!output || !SDL_ResumeAudioStreamDevice(output)) {
            std::fprintf(stderr, "audio: SDL device start failed: %s\n", SDL_GetError());
            return false;
        }
        running = true;
        thread = std::thread([this] { AudioThread(); });
        return true;
    }
#endif

#ifdef _WIN32
    // ---- LegacyVoices ---------------------------------------------------------------------------------------------
    void Destroy(Voice& v) {
        if (v.voice) v.voice->DestroyVoice();
        v.voice = nullptr;
        v.started = false;
    }

    // Device channel of speaker bin 0-5 (FL FR C LFE BL BR), or -1 when the device has no such speaker.
    int SpeakerChannel(u32 bin) const {
        static const u32 kSpeaker[6] = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER,
                                        SPEAKER_LOW_FREQUENCY, SPEAKER_BACK_LEFT, SPEAKER_BACK_RIGHT};
        const u32 bit = kSpeaker[bin];
        if (!(channelMask & bit)) return -1;
        const int ch = std::popcount(channelMask & (bit - 1));
        return ch < static_cast<int>(masterChannels) ? ch : -1;
    }

    // Output matrices and rate from the voice's register values.
    void Apply(Buffer b, Voice& v) {
        if (!v.voice) return;
        const xds::VoiceRegisters* r = ds.Registers(b);
        if (!r) return;
        // HRTF stand-in for 3D voices: equal-power pan from the calculator's azimuth (positive = right)
        f32 earL = 1.0f, earR = 1.0f;
        if (v.is3D) {
            const f32 pan = std::clamp(static_cast<f32>(std::sin(r->azimuth * 3.14159265358979 / 180.0)), -1.0f, 1.0f);
            earL = std::sqrt((1.0f - pan) * 0.5f) * 1.41421356f;
            earR = std::sqrt((1.0f + pan) * 0.5f) * 1.41421356f;
        }
        std::vector<f32> m(static_cast<size_t>(v.channels) * masterChannels, 0.0f);
        std::vector<f32> rv(static_cast<size_t>(v.channels) * 2, 0.0f);
        for (u32 c = 0; c < v.channels; ++c) {
            f32 g[32];
            BinGains(*r, v.channels, c, g);
            auto add = [&](int ch, f32 gain) {
                if (ch >= 0) m[static_cast<size_t>(ch) * v.channels + c] += gain;
            };
            for (u32 bin = 0; bin < 6; ++bin) add(SpeakerChannel(bin), g[bin]);
            // 3D bins: 6 front-left / 8 back-left carry the left ear, 7 / 9 the right ear; crosstalk stand-in:
            // straight to the matching speakers
            add(SpeakerChannel(0), g[6] * earL);
            add(SpeakerChannel(1), g[7] * earR);
            add(SpeakerChannel(4), g[8] * earL);
            add(SpeakerChannel(5), g[9] * earR);
            if (effectsImage) rv[0 * v.channels + c] = rv[1 * v.channels + c] = g[10];
        }
        v.voice->SetOutputMatrix(master, v.channels, masterChannels, m.data());
        if (reverb) v.voice->SetOutputMatrix(reverb, v.channels, 2, rv.data());
        if (v.rate)
            v.voice->SetFrequencyRatio(std::clamp(static_cast<f32>(r->Rate() / v.rate), XAUDIO2_MIN_FREQ_RATIO, v.maxRatio));
    }

    void SetReverbParameters() {
        if (!reverb) return;
        const xds::I3DL2Listener& l = ds.GetI3DL2Listener();
        XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3 = {};
        i3.WetDryMix = 100.0f;                       // wet only: the dry path is the voices' own bins
        i3.Room = l.room, i3.RoomHF = l.roomHF, i3.RoomRolloffFactor = l.roomRolloffFactor;
        i3.DecayTime = l.decayTime, i3.DecayHFRatio = l.decayHFRatio, i3.Reflections = l.reflections;
        i3.ReflectionsDelay = l.reflectionsDelay, i3.Reverb = l.reverb, i3.ReverbDelay = l.reverbDelay;
        i3.Diffusion = l.diffusion, i3.Density = l.density, i3.HFReference = l.hfReference;
        XAUDIO2FX_REVERB_PARAMETERS native;
        ReverbConvertI3DL2ToNative(&i3, &native, FALSE);   // (a stereo submix)
        reverb->SetEffectParameters(0, &native, sizeof native);
    }

    void StartLegacy() {
        mode = Mode::LegacyVoices;
        if (!xaudio || reverb) return;
        IUnknown* apo = nullptr;
        if (SUCCEEDED(XAudio2CreateReverb(&apo, 0))) {
            XAUDIO2_EFFECT_DESCRIPTOR fx = {apo, TRUE, 2};
            XAUDIO2_EFFECT_CHAIN chain = {1, &fx};
            if (FAILED(xaudio->CreateSubmixVoice(&reverb, 2, deviceRate, 0, 0, nullptr, &chain))) reverb = nullptr;
            apo->Release();
            SetReverbParameters();
        }
    }

#else
    void Destroy(Voice&) {}
    void Apply(Buffer, Voice&) {}
    void SetReverbParameters() {}
    void StartLegacy() { throw std::runtime_error("XAudio2 legacy voices are Windows-only"); }
#endif

    // ---- both modes: after a DirectSound setter of buffer b
    void Changed(Buffer b) {
        auto it = voices.find(b);
        if (it == voices.end()) return;
        if (mode == Mode::LegacyVoices) {
            Apply(b, it->second);
        } else if (const xds::VoiceRegisters* r = ds.Registers(b)) {
            bridge.Update(b, *r);                    // (only while the bridge has a voice for it)
        }
    }
};

void GameSoundBackend::BinGains(const xds::VoiceRegisters& r, u32 channels, u32 channel, f32 gains[32]) {
    std::fill(gains, gains + 32, 0.0f);
    const bool stereo = channels == 2 && r.hwVoices == 1;
    const u32 k = stereo ? 0 : std::min(channel, r.hwVoices - 1);
    for (u32 i = 0; i < 8; ++i) {
        if (stereo && (i & 1) != channel) continue;  // L6: even slots left, odd slots right
        const u16 att = r.Attenuation(k, i);
        if (att >= 0xFFF) continue;                  // (L) 0xFFF = mute
        gains[r.slotBins[i] & 31] += static_cast<f32>(std::pow(10.0, -double(att) / (64.0 * 20.0)));
    }
}

GameSoundBackend::GameSoundBackend() : GameSoundBackend(Options{}) {}

GameSoundBackend::GameSoundBackend(const Options& options) : impl_(std::make_unique<Impl>()) {
    impl_->opt = options;
    impl_->mode = options.mode;
    impl_->vpModel.SetNotifiers(&impl_->notifiers);
}

GameSoundBackend::~GameSoundBackend() {
    Impl& d = *impl_;
    if (d.thread.joinable()) {
        d.running = false;
#ifdef _WIN32
        SetEvent(d.wake);
#endif
        d.thread.join();
    }
#ifdef _WIN32
    if (d.out) d.out->DestroyVoice();                // (stops the callbacks before the event goes)
    if (d.wake) CloseHandle(d.wake);
#endif
    for (auto& [b, v] : d.voices) {
        v.mcpx.reset();
        d.Destroy(v);
    }
#ifdef _WIN32
    if (d.reverb) d.reverb->DestroyVoice();
    if (d.master) d.master->DestroyVoice();
    if (d.xaudio) d.xaudio->Release();
    if (d.comInit) CoUninitialize();
#else
    if (d.output) SDL_DestroyAudioStream(d.output);
    if (d.audioInit) SDL_QuitSubSystem(SDL_INIT_AUDIO);
#endif
}

bool GameSoundBackend::Init() {
    Impl& d = *impl_;
#ifdef _WIN32
    d.comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(XAudio2Create(&d.xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    if (FAILED(d.xaudio->CreateMasteringVoice(&d.master))) return false;
    XAUDIO2_VOICE_DETAILS det;
    d.master->GetVoiceDetails(&det);
    d.masterChannels = det.InputChannels;
    d.deviceRate = det.InputSampleRate;
    DWORD mask = 0;
    if (SUCCEEDED(d.master->GetChannelMask(&mask)) && mask) d.channelMask = mask;
    if (d.mode == Mode::LegacyVoices) d.StartLegacy();
    return true;
#else
    if (d.mode != Mode::Vp) return false;
    d.audioInit = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!d.audioInit) std::fprintf(stderr, "audio: SDL initialization failed: %s\n", SDL_GetError());
    return d.audioInit;
#endif
}

bool GameSoundBackend::InitOffline() {
    impl_->offline = true;
    impl_->mode = Mode::Vp;
    impl_->outChannels = impl_->opt.surround ? 6 : 2;
    return true;
}

void GameSoundBackend::LoadXbeTables(const std::vector<u8>& xbe) {
    impl_->tables.LoadFromXbe(xbe);
    impl_->xbe = xbe;
}

void GameSoundBackend::SetEffectsImage(const std::vector<u8>& dsstdfx) {
    u32 words[70];
    if (xds::ReadReverbState(dsstdfx, words)) impl_->ds.SetReverbState(words);
    impl_->dsstdfx = dsstdfx;
}

const xds::DirectSound& GameSoundBackend::Model() const { return impl_->ds; }
GameSoundBackend::Mode GameSoundBackend::CurrentMode() const { return impl_->mode; }

bool GameSoundBackend::RenderFrames(u32 frames, std::vector<s32>& out) {
    Impl& d = *impl_;
    if (!d.offline || d.mode != Mode::Vp) return false;
    const size_t at = out.size();
    out.resize(at + static_cast<size_t>(frames) * kFrame * d.outChannels);
    std::lock_guard g(d.lock);
    return d.RenderLocked(frames, out.data() + at);
}

bool GameSoundBackend::HardwarePlaying(Buffer b) {
    Impl& d = *impl_;
    if (d.mode != Mode::Vp) return false;
    std::lock_guard g(d.lock);
    return d.bridge.Playing(b) && !d.bridge.Ended(b, d.vpModel);
}

u32 GameSoundBackend::OutputChannels() const { return impl_->outChannels; }

bool GameSoundBackend::Healthy() const {
    std::lock_guard g(impl_->lock);
#ifndef _WIN32
    if (impl_->outputFailed) return false;
#endif
    return !impl_->dspFailed;
}

void GameSoundBackend::SetLevels(audio::vp::Levels* levels) {
    Impl& d = *impl_;
    std::lock_guard g(d.lock);
    d.levels = levels;
    d.vpModel.SetLevels(levels);
}

// The game calls it once, first (the sound system's ctor 0x000CCD90): the hardware path starts here.
void GameSoundBackend::DownloadEffectsImage() {
    Impl& d = *impl_;
    d.effectsImage = true;
    if (d.mode != Mode::Vp) return;
    if (!d.StartVp() || (!d.offline && !d.StartOutput())) {
#ifdef _WIN32
        if (d.offline) return;                       // (RenderFrames then renders nothing useful; tests check)
#else
        throw std::runtime_error("native audio DSP/output initialization failed");
#endif
        d.StartLegacy();
    }
}

void GameSoundBackend::SetDopplerFactor(f32 f, u32 apply) { impl_->ds.SetDopplerFactor(f, apply); }
void GameSoundBackend::SetDistanceFactor(f32 f, u32 apply) { impl_->ds.SetDistanceFactor(f, apply); }
void GameSoundBackend::SetRolloffFactor(f32 f, u32 apply) { impl_->ds.SetRolloffFactor(f, apply); }

void GameSoundBackend::SetI3DL2Listener(u32 preset, u32 apply) {
    impl_->ds.SetI3DL2Listener(sound::I3DL2Preset(preset), apply);
    if (impl_->mode == Mode::LegacyVoices) impl_->SetReverbParameters();
}

void GameSoundBackend::ListenerPosition(f32 x, f32 y, f32 z, u32 apply) { impl_->ds.SetPosition(x, y, z, apply); }
void GameSoundBackend::ListenerVelocity(f32 x, f32 y, f32 z, u32 apply) { impl_->ds.SetVelocity(x, y, z, apply); }

void GameSoundBackend::ListenerOrientation(f32 fx, f32 fy, f32 fz, f32 tx, f32 ty, f32 tz, u32 apply) {
    impl_->ds.SetOrientation(fx, fy, fz, tx, ty, tz, apply);
}

void GameSoundBackend::CommitDeferredSettings() {
    Impl& d = *impl_;
    d.ds.CommitDeferredSettings();
    for (auto& [b, v] : d.voices)
        if (v.is3D) d.Changed(b);
}

// orig: DirectSoundDoWork, called by the game once per frame after CommitDeferredSettings: the frame's register
// writes and a new reverb parameter set (the listener commit's conversion) go to the audio thread together.
void GameSoundBackend::DoWork() {
    Impl& d = *impl_;
    if (d.mode != Mode::Vp) return;
    {
        // The streams (CMcpxStream): the DPC of a voice-notifier interrupt the VP raised since the last DoWork
        // (0x0024AEB6 -> 0x0024A834: each started voice's vf14 refills its SSLs; L: on the console the DPC runs when
        // the interrupt comes, here at the next DoWork), then DirectSoundDoWork's deferred commands (0x0024AB4A: the
        // streams' command 2 completes the played packets).
        std::lock_guard g(d.lock);
        if (d.vpModel.TakeInterrupt())
            for (auto& [b, v] : d.voices)
                if (v.mcpx) v.mcpx->Service();
        for (auto& [b, v] : d.voices)
            if (v.mcpx) v.mcpx->DoWork();
    }
    if (d.ds.ReverbVersion() != d.reverbVersion) {
        d.reverbVersion = d.ds.ReverbVersion();
        std::array<u32, 66> w;
        std::copy(d.ds.ReverbParameters(), d.ds.ReverbParameters() + 66, w.begin());
        d.reverbGame = w;
    }
    if (d.opt.stepStatus)                             // (Options::stepStatus: one game step per DoWork)
        for (auto& [b, v] : d.voices)
            if (v.stepPlaying && !v.stepPaused) v.stepCursor += double(v.stepHz) / 60.0;
    std::lock_guard g(d.lock);
    d.published.Splice(d.gameFifo);
    if (d.reverbGame) {
        d.reverbPublished = d.reverbGame;
        d.reverbGame.reset();
    }
}

GameSoundBackend::Buffer GameSoundBackend::CreateBuffer(u32 flags, const std::vector<u8>& format) {
    Impl::Voice v;
    v.flags = flags;
    v.is3D = (flags & 0x10) != 0;
    if (format.size() >= 16) {
        std::memcpy(&v.format, &format[0], 2);
        std::memcpy(&v.channels, &format[2], 2);
        std::memcpy(&v.rate, &format[4], 4);
        std::memcpy(&v.blockAlign, &format[12], 2);
        std::memcpy(&v.bits, &format[14], 2);
    }
    const Buffer b = impl_->ds.CreateVoice(flags, v.channels, v.rate);
    impl_->voices.emplace(b, std::move(v));
    return b;
}

void GameSoundBackend::SetBufferData(Buffer b, const u8* data, u32 bytes) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return;
    Impl::Voice& v = it->second;
    if (d.mode == Mode::Vp) {
        // the voice stops before its data changes (the game always stops it first; the bytes are mapped while it
        // plays)
        d.ds.Stop(b);
        {
            std::lock_guard g(d.lock);
            d.bridge.Stop(b);
        }
        v.resume = 0;                                // CMcpxBuffer_SetBufferData 0x0024E1A2: +0x150 = 0
        v.raw.assign(data, data ? data + bytes : data);
        v.stepPlaying = false;
        const u32 ch = v.channels ? v.channels : 1;
        v.stepLength = v.format == 0x69 ? double(bytes / (36u * ch)) * 64.0
                                        : double(bytes) / double(ch * (v.bits ? v.bits / 8u : 1u));
        return;
    }
#ifdef _WIN32
    d.Destroy(v);
    v.pcm.clear();
    if (!data || !d.xaudio || v.channels == 0 || v.channels > 2 || v.rate == 0) return;
    if (!audio::DecodeData(v.format, v.channels, v.bits, std::span<const u8>(data, bytes), v.pcm) || v.pcm.empty())
        return;
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = v.channels;
    wf.nSamplesPerSec = v.rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = static_cast<WORD>(2 * v.channels);
    wf.nAvgBytesPerSec = v.rate * wf.nBlockAlign;
    // the pitch register reaches 48000 * 2^(8191 / 4096) Hz
    v.maxRatio = std::clamp(192000.0f / static_cast<f32>(v.rate) * 1.01f, 1.0f, XAUDIO2_MAX_FREQ_RATIO);
    XAUDIO2_SEND_DESCRIPTOR sends[2] = {{0, d.master}, {0, d.reverb}};
    XAUDIO2_VOICE_SENDS list = {d.reverb ? 2u : 1u, sends};
    if (FAILED(d.xaudio->CreateSourceVoice(&v.voice, &wf, 0, v.maxRatio, nullptr, &list))) v.voice = nullptr;
    d.Apply(b, v);
#else

#endif
}

void GameSoundBackend::Play(Buffer b, u32 flags) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return;
    Impl::Voice& v = it->second;
    if (d.mode == Mode::Vp) {
        bool ended;
        if (d.opt.stepStatus) {
            ended = !v.stepLoop && !(v.stepCursor < v.stepLength);
            if (v.stepPlaying && !ended) return;      // (the model's view of a playing voice)
        } else {
            {
                std::lock_guard g(d.lock);
                ended = d.bridge.Ended(b, d.vpModel);
            }
            if (d.bridge.Playing(b) && !ended) return;   // (orig: a playing voice continues; 0x0024D621 returns)
        }
        v.stepPlaying = true;
        v.stepPaused = false;
        v.stepLoop = (flags & 1) != 0;
        v.stepCursor = 0;
        if (ended) d.ds.Stop(b);                     // (L) the library's view of a voice the hardware ended
        const u32 resume = v.resume;                 // 0x0024E0A7: +0x150 read, then cleared
        v.resume = 0;
        d.ds.Play(b);                                // a 3D voice commits its pending 3D settings (0x0024D621)
        const xds::VoiceRegisters* r = d.ds.Registers(b);
        if (!r || v.raw.empty()) return;
        std::lock_guard g(d.lock);
        d.bridge.Play(b, {v.format, v.channels, v.bits, v.is3D, v.raw.data(), static_cast<u32>(v.raw.size())},
                      (flags & 1) != 0, *r);
        // 0x0024DA03 (a saved position): the voice's CBO = BytesToSamples(+0x150) (0x0024B303: bytes / block
        // align, times 64 for ADPCM)
        if (resume && v.blockAlign)
            d.bridge.SetPosition(b, resume / v.blockAlign * (v.format == 0x69 ? 64u : 1u));
        return;
    }
#ifdef _WIN32
    if (!v.voice) return;
    XAUDIO2_VOICE_STATE st;
    v.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued == 0) {                      // (re)submit; a stopped buffer resumes where it was
        XAUDIO2_BUFFER buf = {};
        buf.AudioBytes = static_cast<UINT32>(v.pcm.size() * 2);
        buf.pAudioData = reinterpret_cast<const BYTE*>(v.pcm.data());
        buf.Flags = XAUDIO2_END_OF_STREAM;
        buf.LoopCount = (flags & 1) ? XAUDIO2_LOOP_INFINITE : 0;
        v.voice->SubmitSourceBuffer(&buf);
    }
    v.looping = (flags & 1) != 0;
    d.Apply(b, v);
    v.voice->Start(0);
    v.started = true;
#else

#endif
}

void GameSoundBackend::Stop(Buffer b) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return;
    if (d.mode == Mode::Vp) {
        Impl::Voice& v = it->second;
        v.stepPlaying = false;
        const bool playing = d.ds.Playing(b);        // the buffer's state bits 3 (0x0024E0FC; else Stop does nothing)
        d.ds.Stop(b);
        std::lock_guard g(d.lock);
        // 0x0024E131: unless the buffer is a mix-in / FX one (desc flags 0x182000), GetCurrentPosition(&+0x150, 0)
        // 0x0024DC37 -> 0x0024CDCA: the hardware voice's position (its +0x58 & 0xFFFFFF, samples) as bytes
        // (0x0024B31C: times the block align, ADPCM per 64 samples); 0 once the voice ended (+0x54 bit 20). (L) The
        // position is the VP model's at the call (the registers reach the model once per frame).
        if (playing && !(v.flags & 0x182000)) {
            u32 bytes = 0;
            if (d.bridge.Playing(b) && !d.bridge.Ended(b, d.vpModel)) {
                const u32 s = d.vpModel.Position(d.bridge.HardwareVoice(b)) & 0xFFFFFF;
                bytes = v.format == 0x69 ? (s >> 6) * v.blockAlign : s * v.blockAlign;
            }
            v.resume = bytes;
        }
        d.bridge.Stop(b);                            // VOICE_OFF (published at DoWork); the data is unmapped now
        return;
    }
#ifdef _WIN32
    if (!it->second.voice) return;
    it->second.voice->Stop(0);
    it->second.started = false;
#else

#endif
}

GameSoundBackend::Buffer GameSoundBackend::CreateStreamVoice(u32 flags, u32 maxPackets,
                                                                     const std::vector<u8>& format) {
    Impl& d = *impl_;
    if (d.mode != Mode::Vp) return 0;                // (LegacyVoices: no streams, the music is silent; L)
    const Buffer b = CreateBuffer(flags, format);
    Impl::Voice& v = d.voices[b];
    v.stream = true;
    const xds::VoiceRegisters* r = d.ds.Registers(b);
    // orig: DirectSoundCreateStream -> CMcpxStream 0x0024B804 / 0x0024B355: a 2D stream's voice is set up now
    if (v.is3D) std::fprintf(stderr, "NOT PORTED reached: a 3D stream (its voice allocated at Process, 0x0024CE8A)\n");
    if (!r || !d.bridge.CreateStream(b, {v.format, v.channels, v.bits, v.is3D, nullptr, 0}, *r)) return b;
    u16 blockAlign = 0;
    if (format.size() >= 14) std::memcpy(&blockAlign, &format[12], 2);
    std::lock_guard g(d.lock);
    v.mcpx = std::make_unique<vp::McpxStream>(d.gameFifo, d.bridge, d.mem, d.notifiers, b, maxPackets, v.format,
                                              v.channels, v.bits, blockAlign);
    // KeQuerySystemTime (the stop wait): the real one (FILETIME: 100 ns since 1601, as the kernel's); the wait loop
    // 0x0024D1F1 runs with the APU's lock released (the hardware keeps mixing): the callers hold d.lock, the audio
    // thread gets it between two queries.
    v.mcpx->SetSystemTime([] {
#ifdef _WIN32
        FILETIME f;
        GetSystemTimePreciseAsFileTime(&f);
        return static_cast<s64>(u64(f.dwHighDateTime) << 32 | f.dwLowDateTime);
#else
        const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::duration<s64, std::ratio<1, 10000000>>>(elapsed).count()
               + 116444736000000000LL;
#endif
    });
    v.mcpx->SetWaitYield([&d] {
        d.lock.unlock();
        std::this_thread::yield();
        d.lock.lock();
    });
    return b;
}

void GameSoundBackend::ProcessPacket(Buffer b, const u8* data, u32 size, u32* status) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end() || !it->second.mcpx) return;
    std::lock_guard g(d.lock);
    it->second.mcpx->Process({data, size, nullptr, status});   // IDirectSoundStream_Process(XMEDIAPACKET, NULL)
}

void GameSoundBackend::PauseStream(Buffer b, bool pause) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return;
    Impl::Voice& v = it->second;
    if (v.stream) {                                  // a stream: DSSTREAMPAUSE_PAUSE / _RESUME
        if (!v.mcpx) return;
        std::lock_guard g(d.lock);
        v.mcpx->Pause(pause ? 1 : 0);                // CMcpxStream_Pause (VOICE_PAUSE published at DoWork)
        return;
    }
    if (d.mode == Mode::Vp) {
        if (!v.stepPlaying) return;
        v.stepPaused = pause;
        std::lock_guard g(d.lock);
        d.bridge.Pause(b, pause);                    // VOICE_PAUSE (published at DoWork)
        return;
    }
#ifdef _WIN32
    if (!v.voice || !v.started) return;
    if (pause) v.voice->Stop(0);                     // (XAudio2: Stop keeps the position, Start resumes)
    else v.voice->Start(0);
#else

#endif
}

u32 GameSoundBackend::GetStatus(Buffer b) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return 0;
    if (d.mode == Mode::Vp) {
        if (d.opt.stepStatus) {
            const Impl::Voice& v = it->second;
            return v.stepPlaying && (v.stepLoop || v.stepCursor < v.stepLength) ? 1 : 0;
        }
        if (!d.bridge.Playing(b)) return 0;
        std::lock_guard g(d.lock);
        return d.bridge.Ended(b, d.vpModel) ? 0 : 1;   // (L8: a one-shot voice ends after its last sample)
    }
#ifdef _WIN32
    if (!it->second.voice || !it->second.started) return 0;
    XAUDIO2_VOICE_STATE st;
    it->second.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return st.BuffersQueued > 0 ? 1 : 0;
#else
    return 0;
#endif
}

void GameSoundBackend::Release(Buffer b) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return;
    if (d.mode == Mode::Vp) {
        std::lock_guard g(d.lock);
        it->second.mcpx.reset();                     // a stream: CMcpxStream's dtor (Flush: its packets FLUSHED)
        d.bridge.Release(b);                         // unmapped before the bytes go
    }
    d.Destroy(it->second);
    d.voices.erase(it);
    d.ds.ReleaseVoice(b);
}

void GameSoundBackend::SetVolume(Buffer b, s32 volume) {
    impl_->ds.SetVolume(b, volume);
    impl_->Changed(b);
}

void GameSoundBackend::SetFrequency(Buffer b, u32 hz) {
    if (auto it = impl_->voices.find(b); it != impl_->voices.end()) it->second.stepHz = hz;
    impl_->ds.SetFrequency(b, hz);
    impl_->Changed(b);
}

void GameSoundBackend::SetMixBins(Buffer b, const sound::MixBin* bins, u32 count) {
    std::vector<xds::MixBinVolume> v(count);
    for (u32 i = 0; i < count; ++i) v[i] = {bins[i].bin, bins[i].volume};
    impl_->ds.SetMixBins(b, v.data(), count);
    impl_->Changed(b);
}

void GameSoundBackend::SetMinDistance(Buffer b, f32 d, u32 apply) {
    impl_->ds.SetMinDistance(b, d, apply);
    if (apply == 0) impl_->Changed(b);
}

void GameSoundBackend::SetMaxDistance(Buffer b, f32 d, u32 apply) {
    impl_->ds.SetMaxDistance(b, d, apply);
    if (apply == 0) impl_->Changed(b);
}

void GameSoundBackend::SetPosition(Buffer b, f32 x, f32 y, f32 z, u32 apply) {
    impl_->ds.SetBufferPosition(b, x, y, z, apply);
    if (apply == 0) impl_->Changed(b);
}

void GameSoundBackend::SetVelocity(Buffer b, f32 x, f32 y, f32 z, u32 apply) {
    impl_->ds.SetBufferVelocity(b, x, y, z, apply);
    if (apply == 0) impl_->Changed(b);
}

void GameSoundBackend::SetRolloffCurve(Buffer b, const f32* points, u32 count, u32 apply) {
    impl_->ds.SetRolloffCurve(b, points, count, apply);
    if (apply == 0) impl_->Changed(b);
}

// ---- the streams (the movie player's audio) -----------------------------------------------------------------------
// One XAudio2 source voice per stream, straight to the mastering voice: each packet's Xbox ADPCM (or PCM) decoded to
// 16-bit PCM (audio/Wav.cpp) and queued as one buffer; its end (XAudio2's thread) is queued with the QPC and handed to
// the game thread by Deliver. (L) The console plays the stream as a hardware voice through the VP / GP / EP like the
// other 2D voices (its mix bins, headroom and the EP's down-mix): not modelled; the volume is 10^(hundredths / 2000).
// Without a device (InitOffline) a packet completes when the real time since the stream's start passes its end.
#ifdef _WIN32
class XAudio2Stream final : public sound::SoundStream, public IXAudio2VoiceCallback {
public:
    XAudio2Stream(GameSoundBackend::Impl& owner, u16 format, u16 channels, u32 rate, u16 bits,
                  sound::SoundBackend::StreamCallback cb)
        : owner_(owner), format_(format), channels_(channels), rate_(rate), bits_(bits), callback_(std::move(cb)) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpf_ = u64(f.QuadPart);
        if (owner.xaudio && !owner.offline) {
            WAVEFORMATEX wf = {};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = channels;
            wf.nSamplesPerSec = rate;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = WORD(2 * channels);
            wf.nAvgBytesPerSec = rate * wf.nBlockAlign;
            if (FAILED(owner.xaudio->CreateSourceVoice(&voice_, &wf, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this)))
                voice_ = nullptr;
        }
        owner_.streams.push_back(this);
    }
    ~XAudio2Stream() override {
        if (voice_) voice_->DestroyVoice();           // (no callback after it returns)
        std::erase(owner_.streams, this);
    }
    bool Valid() const { return voice_ || owner_.offline || !owner_.xaudio; }

    s32 Process(const u8* data, u32 size, u32* completedSize, u32* status, u32 context) override {
        Packet p;
        p.id = ++nextId_;
        p.completedSize = completedSize, p.status = status, p.context = context, p.bytes = size;
        audio::DecodeData(format_, channels_, bits_, std::span<const u8>(data, size), p.pcm);
        const u32 frames = channels_ ? u32(p.pcm.size() / channels_) : 0;
        submitted_ += rate_ ? double(frames) / double(rate_) : 0.0;
        p.endSeconds = submitted_;
        if (status) *status = 0x8000000Au;           // XMEDIAPACKET_STATUS_PENDING
        if (completedSize) *completedSize = 0;
        queue_.push_back(std::move(p));
        Packet& q = queue_.back();
        if (voice_ && !q.pcm.empty()) {
            XAUDIO2_BUFFER xb = {};
            xb.AudioBytes = UINT32(q.pcm.size() * sizeof(s16));
            xb.pAudioData = reinterpret_cast<const BYTE*>(q.pcm.data());
            xb.pContext = reinterpret_cast<void*>(uintptr_t(q.id));
            if (FAILED(voice_->SubmitSourceBuffer(&xb))) Complete(q.id, Now());
        } else if (voice_) {
            Complete(q.id, Now());
        }
        return 0;
    }
    s32 Pause(u32 flags) override {
        if (flags == 2) {                             // DSSTREAMPAUSE_SYNCHPLAYBACK: held until SynchPlayback
            synch_ = true;
            if (voice_) voice_->Stop(0);
            running_ = false;
        } else if (flags == 1) {
            if (voice_) voice_->Stop(0);
            running_ = false;
        } else {
            Start();
        }
        return 0;
    }
    void Start() {
        synch_ = false;
        if (running_) return;
        running_ = true;
        if (voice_) voice_->Start(0);
        else if (!started_) {
            started_ = true;
            start_ = Now();
        }
    }
    bool WaitingSynch() const { return synch_; }
    s32 Discontinuity() override { return 0; }        // (the queued packets play out)
    s32 Flush() override {
        if (voice_) {
            voice_->Stop(0);
            voice_->FlushSourceBuffers();
        }
        for (Packet& p : queue_) {
            if (p.status) *p.status = 0x8000000Bu;    // (L: the flushed status; the library only tests "pending")
            if (voice_) flushed_.push_back(std::move(p));   // (XAudio2 may read its samples until its OnBufferEnd)
        }
        queue_.clear();
        running_ = false;
        return 0;
    }
    u32 GetStatus() override {
        Deliver();
        return queue_.empty() ? 0u : 0x00010000u;     // DSSTREAMSTATUS_PLAYING
    }
    void SetVolume(s32 h) override {
        if (voice_) voice_->SetVolume(f32(std::pow(10.0, double(h) / 2000.0)));
    }
    // (game thread) the completed packets' status words and callbacks, in order
    void Deliver() {
        if (voice_) {
            std::vector<std::pair<u64, u64>> done;
            {
                std::lock_guard g(m_);
                done.swap(done_);
            }
            for (const auto& [id, qpc] : done) Complete(id, qpc);
            return;
        }
        if (!started_ || !running_) return;
        const u64 now = Now();
        while (!queue_.empty()) {
            const u64 end = start_ + u64(queue_.front().endSeconds * double(qpf_));
            if (end > now) break;
            Complete(queue_.front().id, end);
        }
    }

    // IXAudio2VoiceCallback (XAudio2's thread)
    void STDMETHODCALLTYPE OnBufferEnd(void* ctx) noexcept override {
        std::lock_guard g(m_);
        done_.emplace_back(u64(uintptr_t(ctx)), Now());
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
    void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) noexcept override {}

private:
    struct Packet {
        u64 id = 0;
        std::vector<s16> pcm;
        u32* completedSize = nullptr;
        u32* status = nullptr;
        u32 context = 0, bytes = 0;
        double endSeconds = 0;
    };
    static u64 Now() {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return u64(c.QuadPart);
    }
    void Complete(u64 id, u64 qpc) {
        std::erase_if(flushed_, [id](const Packet& p) { return p.id == id; });
        while (!queue_.empty() && queue_.front().id <= id) {   // (a flushed id is no longer queued)
            Packet p = std::move(queue_.front());
            queue_.pop_front();
            if (p.id != id) continue;
            if (p.status) *p.status = 0;              // XMEDIAPACKET_STATUS_SUCCESS
            if (p.completedSize) *p.completedSize = p.bytes;
            if (callback_) callback_(p.context, 0, qpc);
        }
    }

    GameSoundBackend::Impl& owner_;
    u16 format_, channels_;
    u32 rate_;
    u16 bits_;
    sound::SoundBackend::StreamCallback callback_;
    IXAudio2SourceVoice* voice_ = nullptr;
    std::deque<Packet> queue_;
    std::vector<Packet> flushed_;                     // (their buffers until XAudio2 is done with them)
    u64 nextId_ = 0;
    double submitted_ = 0;
    bool synch_ = false, running_ = false, started_ = false;
    u64 start_ = 0, qpf_ = 1;
    std::mutex m_;
    std::vector<std::pair<u64, u64>> done_;
};

#endif
// Mode::Vp: the XMV library's stream (XMVDecoder_EnableAudioStream 0x00264866: DSSTREAMDESC {0, 2 packets, the
// track's format, the packet callback 0x00264815, the decoder}) as a stream voice of the sound layer's kind: the
// library's CMcpxStream on the bridge (VpStream.h; open items round 5, KNOWN_ISSUES #71: register for register
// against the library in tests/audio/VpRegisterTests.cpp). The callback gets the QueryPerformanceCounter of the
// DirectSoundDoWork that completed the packet.
class VpMovieStream final : public sound::SoundStream {
public:
    VpMovieStream(GameSoundBackend& owner, GameSoundBackend::Impl& d, GameSoundBackend::Buffer b)
        : owner_(owner), d_(d), b_(b) {}
    ~VpMovieStream() override { owner_.Release(b_); }   // IDirectSoundStream_Release (the dtor flushes)
    vp::McpxStream* Mcpx() const;
    s32 Process(const u8* data, u32 size, u32* completedSize, u32* status, u32 context) override {
        vp::McpxStream* m = Mcpx();
        if (!m) return s32(0x80004005u);
        std::lock_guard g(d_.lock);
        return s32(m->Process({data, size, completedSize, status, context}));
    }
    s32 Pause(u32 flags) override {
        vp::McpxStream* m = Mcpx();
        if (!m) return 0;
        std::lock_guard g(d_.lock);
        m->Pause(flags);
        return 0;
    }
    s32 Discontinuity() override {
        vp::McpxStream* m = Mcpx();
        if (!m) return 0;
        std::lock_guard g(d_.lock);
        return s32(m->Discontinuity());
    }
    s32 Flush() override {                            // FlushEx(0, DSSTREAMFLUSHEX_ASYNC)
        vp::McpxStream* m = Mcpx();
        if (!m) return 0;
        std::lock_guard g(d_.lock);
        return s32(m->FlushEx(1));
    }
    u32 GetStatus() override {
        vp::McpxStream* m = Mcpx();
        if (!m) return 0;
        std::lock_guard g(d_.lock);
        return m->GetStatus();
    }
    void SetVolume(s32 hundredthsDb) override { owner_.SetVolume(b_, hundredthsDb); }

private:
    GameSoundBackend& owner_;
    GameSoundBackend::Impl& d_;
    GameSoundBackend::Buffer b_;
};

vp::McpxStream* VpMovieStream::Mcpx() const {
    auto it = d_.voices.find(b_);
    return it == d_.voices.end() ? nullptr : it->second.mcpx.get();
}

std::unique_ptr<sound::SoundStream> GameSoundBackend::CreateStream(const std::vector<u8>& format,
                                                                      StreamCallback completed) {
    if (format.size() < 16) return nullptr;
    auto r16 = [&](size_t o) { return u16(format[o] | format[o + 1] << 8); };
    const u16 tag = r16(0), channels = r16(2);
    const u32 rate = u32(r16(4)) | u32(r16(6)) << 16;
    if ((tag != 0x69 && tag != 1) || !channels || !rate) return nullptr;
    if (impl_->mode == Mode::Vp) {
        const Buffer b = CreateStreamVoice(0, 2, format);   // DirectSoundCreateStream (flags 0, 2 packets)
        auto it = impl_->voices.find(b);
        if (!b || it == impl_->voices.end() || !it->second.mcpx) {
            if (b) Release(b);
            return nullptr;
        }
        it->second.mcpx->SetCallback([cb = std::move(completed)](u32 context, u32 status) {
            if (!cb) return;
#ifdef _WIN32
            LARGE_INTEGER c;
            QueryPerformanceCounter(&c);
            cb(context, status, u64(c.QuadPart));
#else
            cb(context, status, SDL_GetPerformanceCounter());
#endif
        });
        return std::make_unique<VpMovieStream>(*this, *impl_, b);
    }
#ifdef _WIN32
    auto s = std::make_unique<XAudio2Stream>(*impl_, tag, channels, rate, r16(14), std::move(completed));
    if (!s->Valid()) return nullptr;
    return s;
#else
    return nullptr;
#endif
}

s32 GameSoundBackend::SynchPlayback() {
    Impl& d = *impl_;
    if (d.mode == Mode::Vp) {                         // CMcpxAPU_SynchPlayback 0x0024AC93 over every stream voice
        std::vector<vp::McpxStream*> all;
        for (auto& [b, v] : d.voices)
            if (v.mcpx) all.push_back(v.mcpx.get());
        std::lock_guard g(d.lock);
        vp::McpxStream::SynchPlayback(d.gameFifo, d.bridge, all);
        return 0;
    }
#ifdef _WIN32
    for (XAudio2Stream* s : d.streams)
        if (s->WaitingSynch()) s->Start();
#endif
    return 0;
}

void GameSoundBackend::DoStreamWork() {
    Impl& d = *impl_;
    if (d.mode == Mode::Vp) {
        // the XMV library's DirectSoundDoWork (CloseDecoder's wait, GetNextFrame's retire): its stream part, as in
        // DoWork (the frame's register writes are published by the game's own DoWork, L)
        std::lock_guard g(d.lock);
        if (d.vpModel.TakeInterrupt())
            for (auto& [b, v] : d.voices)
                if (v.mcpx) v.mcpx->Service();
        for (auto& [b, v] : d.voices)
            if (v.mcpx) v.mcpx->DoWork();
        return;
    }
#ifdef _WIN32
    for (XAudio2Stream* s : d.streams) s->Deliver();
#endif
}

void GameSoundBackend::SetI3DL2Source(Buffer b, const std::array<s32, 9>& p, u32 apply) {
    xds::I3DL2Source s;
    auto f = [&](u32 i) {
        f32 v;
        std::memcpy(&v, &p[i], 4);
        return v;
    };
    s.direct = p[0], s.directHF = p[1], s.room = p[2], s.roomHF = p[3], s.roomRolloffFactor = f(4);
    s.obstructionHF = p[5], s.obstructionLFRatio = f(6), s.occlusionHF = p[7], s.occlusionLFRatio = f(8);
    impl_->ds.SetI3DL2Source(b, s, apply);
    if (apply == 0) impl_->Changed(b);
}

}  // namespace fut::platform

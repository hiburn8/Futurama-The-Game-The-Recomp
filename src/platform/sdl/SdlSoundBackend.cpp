// The SDL sound backend; see SdlSoundBackend.h.
#include "platform/sdl/SdlSoundBackend.h"

#include "audio/Wav.h"
#include "audio/dsp/AudioDsp.h"
#include "audio/vp/Vp.h"
#include "audio/vp/VpBridge.h"
#include "audio/vp/VpStream.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
constexpr u32 kFramesPerChunk = 4;                   // rendered under the lock (2.7 ms of audio)
constexpr u32 kDeviceFrames = 512;                   // the device buffer SDL is asked for (10.7 ms)

// KeQuerySystemTime: FILETIME units (100 ns since 1601) from the system clock.
s64 SystemTimeNow() {
    using namespace std::chrono;
    const s64 since1970 = duration_cast<duration<s64, std::ratio<1, 10000000>>>(system_clock::now().time_since_epoch()).count();
    return since1970 + 116444736000000000LL;
}

}  // namespace

struct SdlSoundBackend::Impl {
    Options opt;
    bool offline = false;
    bool audioInit = false;                          // SDL_INIT_AUDIO is ours to quit
    bool hardware = false;                           // StartVp succeeded: the bridge, VP and DSP are live
    xds::Tables tables;
    xds::DirectSound ds{&tables};
    std::vector<u8> xbe, dsstdfx;
    struct Voice {
        u32 flags = 0;
        u16 format = 1, channels = 1, bits = 16;
        u16 blockAlign = 0;                          // WAVEFORMATEX nBlockAlign (the library's format +0x14)
        u32 rate = 0;
        // The position CMcpxBuffer_Stop 0x0024E0EE saved (+0x150, bytes: GetCurrentPosition 0x0024DC37's play
        // cursor), where the next Play resumes (0x0024E0A7 -> 0x0024DA03); 0 = from the start.
        u32 resume = 0;
        bool is3D = false;
        std::vector<u8> raw;                         // the SetBufferData bytes (ADPCM undecoded; the VP decodes)
        // Options::stepStatus (the deterministic status): the game thread's cursor of the voice.
        bool stepPlaying = false, stepLoop = false;
        double stepCursor = 0, stepLength = 0;
        bool stepPaused = false;                     // (PauseStream: the cursor holds)
        u32 stepHz = 0;
        // A stream of the sound layer (CreateStreamVoice): the library's CMcpxStream (VpStream.h) on the bridge's
        // stream voice (game thread; its packet mappings and notifiers under `lock`).
        bool stream = false;
        std::unique_ptr<vp::McpxStream> mcpx;
    };
    std::unordered_map<Buffer, Voice> voices;

    // The game thread owns ds, the bridge and gameFifo; `lock` guards the rest that both threads touch: the memory
    // map (Map / Unmap at Play / Stop), the published FIFO and reverb words, the front end / VP / DSP (the audio
    // thread's drain + render; GetStatus reads the VP).
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
    SDL_AudioStream* out = nullptr;                  // the output: bound to the default playback device
    std::vector<s32> chunk;
    std::vector<f32> feed;

    // Starts the hardware path: the DSP from the game's files, the library's VP set-up (published at once).
    bool StartVp() {
        std::string why;
        if (xbe.empty() || dsstdfx.empty()) why = "default.xbe / xbox/dsstdfx.bin not given";
        else if (!tables.Loaded()) why = "default.xbe: no HRTF tables";
        else if (!dsp.Init(dsstdfx, xbe, &why)) why = "AudioDsp: " + why;
        if (!why.empty()) {
            std::fprintf(stderr, "audio: hardware path unavailable (%s): the game runs silent\n", why.c_str());
            return false;
        }
        dsp.SetSkipIdleEffects(true);                // the VP model never writes bins 11-31 for this game
        if (opt.surround) ds.SetRearSpeakers(true);
        bridge.InitHardware();
        std::lock_guard g(lock);
        published.Splice(gameFifo);
        published.Drain(fe);
        hardware = true;
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

    // SDL's device thread: the stream wants `additional` more bytes; whole frames are rendered, in chunks under the
    // lock, and put into the stream (float, v / 2^23: L 11.4 #11).
    static void SDLCALL Feed(void* userdata, SDL_AudioStream* stream, int additional, int) {
        Impl& d = *static_cast<Impl*>(userdata);
        if (additional <= 0) return;
        const u32 frameBytes = kFrame * d.outChannels * u32(sizeof(f32));
        const u32 frames = (u32(additional) + frameBytes - 1) / frameBytes;
        d.feed.resize(size_t(frames) * kFrame * d.outChannels);
        d.chunk.resize(size_t(kFramesPerChunk) * kFrame * d.outChannels);
        f32* dst = d.feed.data();
        for (u32 done = 0; done < frames;) {
            const u32 n = std::min(kFramesPerChunk, frames - done);
            {
                std::lock_guard g(d.lock);
                d.RenderLocked(n, d.chunk.data());
            }
            const size_t samples = size_t(n) * kFrame * d.outChannels;
            for (size_t i = 0; i < samples; ++i) dst[i] = static_cast<f32>(d.chunk[i]) / 8388608.0f;
            dst += samples;
            done += n;
        }
        SDL_PutAudioStreamData(stream, d.feed.data(), int(d.feed.size() * sizeof(f32)));
    }

    bool StartOutput() {
        outChannels = opt.surround ? 6 : 2;
        char frames[16];
        std::snprintf(frames, sizeof frames, "%u", kDeviceFrames);
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
        const SDL_AudioSpec spec{SDL_AUDIO_F32, int(outChannels), 48000};   // (6 channels: FL FR C LFE BL BR)
        out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::Feed, this);
        if (!out) {
            std::fprintf(stderr, "audio: no output stream (%s): the game runs silent\n", SDL_GetError());
            return false;
        }
        SDL_ResumeAudioStreamDevice(out);
        return true;
    }

    // After a DirectSound setter of buffer b: its register values to the bridge (only while it has a voice for it).
    void Changed(Buffer b) {
        if (!hardware || !voices.count(b)) return;
        if (const xds::VoiceRegisters* r = ds.Registers(b)) bridge.Update(b, *r);
    }
};

void SdlSoundBackend::BinGains(const xds::VoiceRegisters& r, u32 channels, u32 channel, f32 gains[32]) {
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

SdlSoundBackend::SdlSoundBackend() : SdlSoundBackend(Options{}) {}

SdlSoundBackend::SdlSoundBackend(const Options& options) : impl_(std::make_unique<Impl>()) {
    impl_->opt = options;
    impl_->vpModel.SetNotifiers(&impl_->notifiers);
}

SdlSoundBackend::~SdlSoundBackend() {
    Impl& d = *impl_;
    if (d.out) SDL_DestroyAudioStream(d.out);        // (no Feed after it returns)
    d.out = nullptr;
    for (auto& [b, v] : d.voices) v.mcpx.reset();
    if (d.audioInit) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool SdlSoundBackend::Init() {
    Impl& d = *impl_;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "audio: SDL_InitSubSystem failed (%s)\n", SDL_GetError());
        return false;
    }
    d.audioInit = true;
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    SDL_free(devices);
    return count > 0;                                // (none: the caller runs offline, as before)
}

bool SdlSoundBackend::InitOffline() {
    impl_->offline = true;
    impl_->outChannels = impl_->opt.surround ? 6 : 2;
    return true;
}

void SdlSoundBackend::LoadXbeTables(const std::vector<u8>& xbe) {
    impl_->tables.LoadFromXbe(xbe);
    impl_->xbe = xbe;
}

void SdlSoundBackend::SetEffectsImage(const std::vector<u8>& dsstdfx) {
    u32 words[70];
    if (xds::ReadReverbState(dsstdfx, words)) impl_->ds.SetReverbState(words);
    impl_->dsstdfx = dsstdfx;
}

const xds::DirectSound& SdlSoundBackend::Model() const { return impl_->ds; }
bool SdlSoundBackend::HardwarePath() const { return impl_->hardware; }

bool SdlSoundBackend::RenderFrames(u32 frames, std::vector<s32>& out) {
    Impl& d = *impl_;
    if (!d.offline) return false;
    const size_t at = out.size();
    out.resize(at + static_cast<size_t>(frames) * kFrame * d.outChannels);
    std::lock_guard g(d.lock);
    return d.RenderLocked(frames, out.data() + at);
}

bool SdlSoundBackend::HardwarePlaying(Buffer b) {
    Impl& d = *impl_;
    if (!d.hardware) return false;
    std::lock_guard g(d.lock);
    return d.bridge.Playing(b) && !d.bridge.Ended(b, d.vpModel);
}

u32 SdlSoundBackend::OutputChannels() const { return impl_->outChannels; }

void SdlSoundBackend::SetLevels(audio::vp::Levels* levels) {
    Impl& d = *impl_;
    std::lock_guard g(d.lock);
    d.levels = levels;
    d.vpModel.SetLevels(levels);
}

// The game calls it once, first (the sound system's ctor 0x000CCD90): the hardware path starts here.
void SdlSoundBackend::DownloadEffectsImage() {
    Impl& d = *impl_;
    if (d.hardware) return;
    if (!d.StartVp()) return;                        // (silent: the step status still ends the voices)
    if (!d.offline) d.StartOutput();                 // (RenderFrames renders offline; a failed stream is silent)
}

void SdlSoundBackend::SetDopplerFactor(f32 f, u32 apply) { impl_->ds.SetDopplerFactor(f, apply); }
void SdlSoundBackend::SetDistanceFactor(f32 f, u32 apply) { impl_->ds.SetDistanceFactor(f, apply); }
void SdlSoundBackend::SetRolloffFactor(f32 f, u32 apply) { impl_->ds.SetRolloffFactor(f, apply); }

void SdlSoundBackend::SetI3DL2Listener(u32 preset, u32 apply) {
    impl_->ds.SetI3DL2Listener(sound::I3DL2Preset(preset), apply);
}

void SdlSoundBackend::ListenerPosition(f32 x, f32 y, f32 z, u32 apply) { impl_->ds.SetPosition(x, y, z, apply); }
void SdlSoundBackend::ListenerVelocity(f32 x, f32 y, f32 z, u32 apply) { impl_->ds.SetVelocity(x, y, z, apply); }

void SdlSoundBackend::ListenerOrientation(f32 fx, f32 fy, f32 fz, f32 tx, f32 ty, f32 tz, u32 apply) {
    impl_->ds.SetOrientation(fx, fy, fz, tx, ty, tz, apply);
}

void SdlSoundBackend::CommitDeferredSettings() {
    Impl& d = *impl_;
    d.ds.CommitDeferredSettings();
    for (auto& [b, v] : d.voices)
        if (v.is3D) d.Changed(b);
}

// orig: DirectSoundDoWork, called by the game once per frame after CommitDeferredSettings: the frame's register
// writes and a new reverb parameter set (the listener commit's conversion) go to the audio thread together.
void SdlSoundBackend::DoWork() {
    Impl& d = *impl_;
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

SdlSoundBackend::Buffer SdlSoundBackend::CreateBuffer(u32 flags, const std::vector<u8>& format) {
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

void SdlSoundBackend::SetBufferData(Buffer b, const u8* data, u32 bytes) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return;
    Impl::Voice& v = it->second;
    // the voice stops before its data changes (the game always stops it first; the bytes are mapped while it plays)
    d.ds.Stop(b);
    if (d.hardware) {
        std::lock_guard g(d.lock);
        d.bridge.Stop(b);
    }
    v.resume = 0;                                    // CMcpxBuffer_SetBufferData 0x0024E1A2: +0x150 = 0
    v.raw.assign(data, data ? data + bytes : data);
    v.stepPlaying = false;
    const u32 ch = v.channels ? v.channels : 1;
    v.stepLength = v.format == 0x69 ? double(bytes / (36u * ch)) * 64.0
                                    : double(bytes) / double(ch * (v.bits ? v.bits / 8u : 1u));
}

void SdlSoundBackend::Play(Buffer b, u32 flags) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return;
    Impl::Voice& v = it->second;
    bool ended;
    if (d.opt.stepStatus) {
        ended = !v.stepLoop && !(v.stepCursor < v.stepLength);
        if (v.stepPlaying && !ended) return;          // (the model's view of a playing voice)
    } else {
        ended = true;
        if (d.hardware) {
            std::lock_guard g(d.lock);
            ended = d.bridge.Ended(b, d.vpModel);
        }
        if (d.hardware && d.bridge.Playing(b) && !ended) return;   // (orig: a playing voice continues; 0x0024D621)
    }
    v.stepPlaying = true;
    v.stepPaused = false;
    v.stepLoop = (flags & 1) != 0;
    v.stepCursor = 0;
    if (ended) d.ds.Stop(b);                         // (L) the library's view of a voice the hardware ended
    const u32 resume = v.resume;                     // 0x0024E0A7: +0x150 read, then cleared
    v.resume = 0;
    d.ds.Play(b);                                    // a 3D voice commits its pending 3D settings (0x0024D621)
    const xds::VoiceRegisters* r = d.ds.Registers(b);
    if (!r || v.raw.empty() || !d.hardware) return;
    std::lock_guard g(d.lock);
    d.bridge.Play(b, {v.format, v.channels, v.bits, v.is3D, v.raw.data(), static_cast<u32>(v.raw.size())},
                  (flags & 1) != 0, *r);
    // 0x0024DA03 (a saved position): the voice's CBO = BytesToSamples(+0x150) (0x0024B303: bytes / block align,
    // times 64 for ADPCM)
    if (resume && v.blockAlign)
        d.bridge.SetPosition(b, resume / v.blockAlign * (v.format == 0x69 ? 64u : 1u));
}

void SdlSoundBackend::Stop(Buffer b) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return;
    Impl::Voice& v = it->second;
    v.stepPlaying = false;
    const bool playing = d.ds.Playing(b);            // the buffer's state bits 3 (0x0024E0FC; else Stop does nothing)
    d.ds.Stop(b);
    if (!d.hardware) return;
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
    d.bridge.Stop(b);                                // VOICE_OFF (published at DoWork); the data is unmapped now
}

SdlSoundBackend::Buffer SdlSoundBackend::CreateStreamVoice(u32 flags, u32 maxPackets, const std::vector<u8>& format) {
    Impl& d = *impl_;
    const Buffer b = CreateBuffer(flags, format);
    Impl::Voice& v = d.voices[b];
    v.stream = true;
    const xds::VoiceRegisters* r = d.ds.Registers(b);
    // orig: DirectSoundCreateStream -> CMcpxStream 0x0024B804 / 0x0024B355: a 2D stream's voice is set up now
    if (v.is3D) std::fprintf(stderr, "NOT PORTED reached: a 3D stream (its voice allocated at Process, 0x0024CE8A)\n");
    if (!r || !d.hardware || !d.bridge.CreateStream(b, {v.format, v.channels, v.bits, v.is3D, nullptr, 0}, *r)) return b;
    u16 blockAlign = 0;
    if (format.size() >= 14) std::memcpy(&blockAlign, &format[12], 2);
    std::lock_guard g(d.lock);
    v.mcpx = std::make_unique<vp::McpxStream>(d.gameFifo, d.bridge, d.mem, d.notifiers, b, maxPackets, v.format,
                                              v.channels, v.bits, blockAlign);
    // KeQuerySystemTime (the stop wait): the real one (FILETIME: 100 ns since 1601, as the kernel's); the wait loop
    // 0x0024D1F1 runs with the APU's lock released (the hardware keeps mixing): the callers hold d.lock, the audio
    // thread gets it between two queries.
    v.mcpx->SetSystemTime([] { return SystemTimeNow(); });
    v.mcpx->SetWaitYield([&d] {
        d.lock.unlock();
        std::this_thread::yield();
        d.lock.lock();
    });
    return b;
}

void SdlSoundBackend::ProcessPacket(Buffer b, const u8* data, u32 size, u32* status) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end() || !it->second.mcpx) return;
    std::lock_guard g(d.lock);
    it->second.mcpx->Process({data, size, nullptr, status});   // IDirectSoundStream_Process(XMEDIAPACKET, NULL)
}

void SdlSoundBackend::PauseStream(Buffer b, bool pause) {
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
    if (!v.stepPlaying) return;
    v.stepPaused = pause;
    if (!d.hardware) return;
    std::lock_guard g(d.lock);
    d.bridge.Pause(b, pause);                        // VOICE_PAUSE (published at DoWork)
}

u32 SdlSoundBackend::GetStatus(Buffer b) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return 0;
    if (d.opt.stepStatus) {
        const Impl::Voice& v = it->second;
        return v.stepPlaying && (v.stepLoop || v.stepCursor < v.stepLength) ? 1 : 0;
    }
    if (!d.hardware || !d.bridge.Playing(b)) return 0;
    std::lock_guard g(d.lock);
    return d.bridge.Ended(b, d.vpModel) ? 0 : 1;      // (L8: a one-shot voice ends after its last sample)
}

void SdlSoundBackend::Release(Buffer b) {
    Impl& d = *impl_;
    auto it = d.voices.find(b);
    if (it == d.voices.end()) return;
    {
        std::lock_guard g(d.lock);
        it->second.mcpx.reset();                     // a stream: CMcpxStream's dtor (Flush: its packets FLUSHED)
        if (d.hardware) d.bridge.Release(b);         // unmapped before the bytes go
    }
    d.voices.erase(it);
    d.ds.ReleaseVoice(b);
}

void SdlSoundBackend::SetVolume(Buffer b, s32 volume) {
    impl_->ds.SetVolume(b, volume);
    impl_->Changed(b);
}

void SdlSoundBackend::SetFrequency(Buffer b, u32 hz) {
    if (auto it = impl_->voices.find(b); it != impl_->voices.end()) it->second.stepHz = hz;
    impl_->ds.SetFrequency(b, hz);
    impl_->Changed(b);
}

void SdlSoundBackend::SetMixBins(Buffer b, const sound::MixBin* bins, u32 count) {
    std::vector<xds::MixBinVolume> v(count);
    for (u32 i = 0; i < count; ++i) v[i] = {bins[i].bin, bins[i].volume};
    impl_->ds.SetMixBins(b, v.data(), count);
    impl_->Changed(b);
}

void SdlSoundBackend::SetMinDistance(Buffer b, f32 d, u32 apply) {
    impl_->ds.SetMinDistance(b, d, apply);
    if (apply == 0) impl_->Changed(b);
}

void SdlSoundBackend::SetMaxDistance(Buffer b, f32 d, u32 apply) {
    impl_->ds.SetMaxDistance(b, d, apply);
    if (apply == 0) impl_->Changed(b);
}

void SdlSoundBackend::SetPosition(Buffer b, f32 x, f32 y, f32 z, u32 apply) {
    impl_->ds.SetBufferPosition(b, x, y, z, apply);
    if (apply == 0) impl_->Changed(b);
}

void SdlSoundBackend::SetVelocity(Buffer b, f32 x, f32 y, f32 z, u32 apply) {
    impl_->ds.SetBufferVelocity(b, x, y, z, apply);
    if (apply == 0) impl_->Changed(b);
}

void SdlSoundBackend::SetRolloffCurve(Buffer b, const f32* points, u32 count, u32 apply) {
    impl_->ds.SetRolloffCurve(b, points, count, apply);
    if (apply == 0) impl_->Changed(b);
}

// ---- the streams (the movie player's audio) -----------------------------------------------------------------------
// The XMV library's stream (XMVDecoder_EnableAudioStream 0x00264866: DSSTREAMDESC {0, 2 packets, the track's format,
// the packet callback 0x00264815, the decoder}) as a stream voice of the sound layer's kind: the library's
// CMcpxStream on the bridge (VpStream.h; open items round 5, KNOWN_ISSUES #71: register for register against the
// library in tests/audio/VpRegisterTests.cpp). The callback gets the performance counter of the DirectSoundDoWork
// that completed the packet.
class VpMovieStream final : public sound::SoundStream {
public:
    VpMovieStream(SdlSoundBackend& owner, SdlSoundBackend::Impl& d, SdlSoundBackend::Buffer b)
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
    SdlSoundBackend& owner_;
    SdlSoundBackend::Impl& d_;
    SdlSoundBackend::Buffer b_;
};

vp::McpxStream* VpMovieStream::Mcpx() const {
    auto it = d_.voices.find(b_);
    return it == d_.voices.end() ? nullptr : it->second.mcpx.get();
}

std::unique_ptr<sound::SoundStream> SdlSoundBackend::CreateStream(const std::vector<u8>& format,
                                                                  StreamCallback completed) {
    if (format.size() < 16) return nullptr;
    auto r16 = [&](size_t o) { return u16(format[o] | format[o + 1] << 8); };
    const u16 tag = r16(0), channels = r16(2);
    const u32 rate = u32(r16(4)) | u32(r16(6)) << 16;
    if ((tag != 0x69 && tag != 1) || !channels || !rate) return nullptr;
    const Buffer b = CreateStreamVoice(0, 2, format);   // DirectSoundCreateStream (flags 0, 2 packets)
    auto it = impl_->voices.find(b);
    if (!b || it == impl_->voices.end() || !it->second.mcpx) {
        if (b) Release(b);
        return nullptr;
    }
    it->second.mcpx->SetCallback([cb = std::move(completed)](u32 context, u32 status) {
        if (cb) cb(context, status, SDL_GetPerformanceCounter());
    });
    return std::make_unique<VpMovieStream>(*this, *impl_, b);
}

s32 SdlSoundBackend::SynchPlayback() {
    Impl& d = *impl_;                                 // CMcpxAPU_SynchPlayback 0x0024AC93 over every stream voice
    if (!d.hardware) return 0;
    std::vector<vp::McpxStream*> all;
    for (auto& [b, v] : d.voices)
        if (v.mcpx) all.push_back(v.mcpx.get());
    std::lock_guard g(d.lock);
    vp::McpxStream::SynchPlayback(d.gameFifo, d.bridge, all);
    return 0;
}

void SdlSoundBackend::DoStreamWork() {
    Impl& d = *impl_;
    // the XMV library's DirectSoundDoWork (CloseDecoder's wait, GetNextFrame's retire): its stream part, as in DoWork
    // (the frame's register writes are published by the game's own DoWork, L)
    std::lock_guard g(d.lock);
    if (d.vpModel.TakeInterrupt())
        for (auto& [b, v] : d.voices)
            if (v.mcpx) v.mcpx->Service();
    for (auto& [b, v] : d.voices)
        if (v.mcpx) v.mcpx->DoWork();
}

void SdlSoundBackend::SetI3DL2Source(Buffer b, const std::array<s32, 9>& p, u32 apply) {
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

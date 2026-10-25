// The game's sound layer (App+0xF0): loaded samples, playing voices with integer ids, volume categories, the 3D
// listener and the DirectSound buffer settings computed every frame. Decompiled from default.xbe 0x000C9480-0x000CD110
// (structure: docs/analysis/sound_system.md); the generic and Xbox halves of each class are merged.
//
// The DirectSound calls are kept as a backend interface with the same calls and arguments (SoundBackend), so the
// layer's output can be compared call for call with the original (tests/game/SoundSystemTests.cpp) and an XAudio2
// backend can realise it (src/platform/win32/XAudio2SoundBackend.cpp).
//
// Streaming samples (the music): each voice has its own IDirectSoundStream fed with ten packets of 0.3 s read from the
// file in order and always wrapping (Refill 0x000CB640, once per voice update: the voice's NiUDSStream job pumped by
// Update 0x000CC670; the port reads the disc file the sample keeps, synchronously, L).
// NOT PORTED: asynchronous loading (Sound_Load's NiUDSStream job) - samples load synchronously, so a voice created
// right after Sound_Load starts one update earlier than on the Xbox.
#pragma once

#include "audio/xdsound/XdsMath.h"
#include "core/Types.h"
#include "core/CrtString.h"
#include "engine/ni/NiObjects.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fut::sound {

struct MixBin {
    u32 bin;
    s32 volume;                                      // hundredths of dB
};

// IDirectSoundStream (DirectSoundCreateStream): a voice fed with packets, the movie player's audio (the XMV library's
// streams, docs/analysis/movies_port.md 2.7 / 2.8). A packet's status word is XMEDIAPACKET_STATUS_PENDING (0x8000000A)
// from Process until it completes (0 and the completion callback with its context) or is flushed; the backend writes
// it and calls back on the game thread (DoStreamWork, GetStatus), with the QueryPerformanceCounter of the completion.
class SoundStream {
public:
    virtual ~SoundStream() = default;
    virtual s32 Process(const u8* data, u32 size, u32* completedSize, u32* status, u32 context) = 0;
    virtual s32 Pause(u32 flags) = 0;                // DSSTREAMPAUSE_RESUME 0, _PAUSE 1, _SYNCHPLAYBACK 2
    virtual s32 Discontinuity() = 0;
    virtual s32 Flush() = 0;                         // FlushEx(0, DSSTREAMFLUSHEX_ASYNC)
    virtual u32 GetStatus() = 0;                     // DSSTREAMSTATUS_PLAYING 0x10000
    virtual void SetVolume(s32 hundredthsDb) = 0;
};

// The DirectSound calls the layer makes (XDK names). Buffers are opaque handles, 0 = none.
class SoundBackend {
public:
    using Buffer = u32;
    virtual ~SoundBackend() = default;
    // ctor (0x000CCD90) set-up
    virtual void DownloadEffectsImage() = 0;         // "XBOX\dsstdfx.bin"
    virtual void UseFullHrtf() = 0;
    virtual void SetDopplerFactor(f32 f, u32 apply) = 0;
    virtual void SetDistanceFactor(f32 f, u32 apply) = 0;
    virtual void SetRolloffFactor(f32 f, u32 apply) = 0;
    virtual void SetI3DL2Listener(u32 preset, u32 apply) = 0;   // preset index into the five table entries
    // listener
    virtual void ListenerPosition(f32 x, f32 y, f32 z, u32 apply) = 0;
    virtual void ListenerVelocity(f32 x, f32 y, f32 z, u32 apply) = 0;
    virtual void ListenerOrientation(f32 fx, f32 fy, f32 fz, f32 tx, f32 ty, f32 tz, u32 apply) = 0;
    virtual void CommitDeferredSettings() = 0;
    virtual void DoWork() = 0;
    // buffers; `format` is the WAVEFORMATEX / XBOXADPCMWAVEFORMAT bytes the original passes
    virtual Buffer CreateBuffer(u32 flags, const std::vector<u8>& format) = 0;
    virtual void SetBufferData(Buffer b, const u8* data, u32 bytes) = 0;
    virtual void Play(Buffer b, u32 flags) = 0;
    virtual void Stop(Buffer b) = 0;
    // The layer's streams (a streaming voice's IDirectSoundStream, 0x000CC1E0 / 0x000CB640; the handles share the
    // buffers' numbering and setters: SetFrequency / SetVolume / SetMixBins / the 3D setters / Release are the
    // IDirectSoundStream_* calls on a stream handle). DirectSoundCreateStream(DSSTREAMDESC{flags, maxPackets, the
    // PCM format}): 0 = no stream (the default: the voice submits nothing, as when the creation fails).
    virtual Buffer CreateStreamVoice(u32 flags, u32 maxPackets, const std::vector<u8>& format) {
        (void)flags;
        (void)maxPackets;
        (void)format;
        return 0;
    }
    // IDirectSoundStream_Process(stream, XMEDIAPACKET{data, size, 0, status, 0, 0}, NULL): *status is
    // XMEDIAPACKET_STATUS_PENDING (0x8000000A) until the packet has played (0, written by the backend on the game
    // thread at DoWork). The bytes stay the caller's (the voice's packet buffer) until then.
    virtual void ProcessPacket(Buffer s, const u8* data, u32 size, u32* status) {
        (void)s;
        (void)data;
        (void)size;
        (void)status;
    }
    // IDirectSoundStream_Pause(stream, 1 / 0) (DSSTREAMPAUSE_PAUSE / _RESUME). Default: nothing.
    virtual void PauseStream(Buffer b, bool pause) {
        (void)b;
        (void)pause;
    }
    virtual u32 GetStatus(Buffer b) = 0;             // bit 0 playing
    virtual void Release(Buffer b) = 0;
    virtual void SetVolume(Buffer b, s32 volume) = 0;
    virtual void SetFrequency(Buffer b, u32 hz) = 0;
    virtual void SetMixBins(Buffer b, const MixBin* bins, u32 count) = 0;
    virtual void SetMinDistance(Buffer b, f32 d, u32 apply) = 0;
    virtual void SetMaxDistance(Buffer b, f32 d, u32 apply) = 0;
    virtual void SetPosition(Buffer b, f32 x, f32 y, f32 z, u32 apply) = 0;
    virtual void SetVelocity(Buffer b, f32 x, f32 y, f32 z, u32 apply) = 0;
    virtual void SetRolloffCurve(Buffer b, const f32* points, u32 count, u32 apply) = 0;
    virtual void SetI3DL2Source(Buffer b, const std::array<s32, 9>& params, u32 apply) = 0;
    // The streams (not the layer's calls: the movie player's). `format`: the WAVEFORMATEX bytes; `completed`: (context,
    // status, QPC at the completion). Null: no streams (the stream-less backends of the tests).
    using StreamCallback = std::function<void(u32 context, u32 status, u64 qpc)>;
    virtual std::unique_ptr<SoundStream> CreateStream(const std::vector<u8>& format, StreamCallback completed) {
        (void)format;
        (void)completed;
        return nullptr;
    }
    virtual s32 SynchPlayback() { return 0; }        // IDirectSound_SynchPlayback (the streams paused with flag 2)
    virtual void DoStreamWork() {}                   // DirectSoundDoWork's stream part: completions delivered
};

class SoundSystem;
class Voice;

// The I3DL2 listener of SetI3DL2Listener's preset index (0 Padded Cell, 1 Room, 2 Living Room, 3 Stone Room,
// 4 Concert Hall; SetEnvironment 0x000CC030).
const audio::xds::I3DL2Listener& I3DL2Preset(u32 preset);

class Sample {                                       // orig: 0x70 bytes (ctor 0x000CA1F0 + 0x000CCB00)
public:
    std::string name;                                // orig: +0x20
    bool loaded = false;                             // orig: +0x24
    bool mono = false;                               // orig: +0x25
    bool streaming = false;                          // orig: +0x27
    u32 bytes = 0;                                   // orig: +0x14
    u32 dataLength = 0;                              // orig: +0x18
    u32 sampleRate = 0;                              // orig: +0x1C
    u32 dataOffset = 0;                              // orig: +0x30
    u32 packetSize = 0;                              // orig: +0x4C
    f32 minDistance = 5.0f, maxDistance = 80.0f;     // orig: +0x28 / +0x2C
    bool adpcm = false;                              // orig: +0x58
    std::vector<u8> pcmFormat;                       // orig: +0x34, 18 bytes (cbSize forced to 0x12)
    std::vector<u8> adpcmFormat;                     // orig: +0x59, 20 bytes
    std::vector<u8> data;                            // orig: +0x48 (a streaming sample's: the ten-packet pre-roll)
    std::vector<Voice*> voices;                      // orig: list +0x0C (oldest first)
    // A streaming sample's disc file, which each of its voices reads its packets from (orig: every voice opens its own
    // NiUDSStream on the name; the port keeps the bytes the loader read, L).
    std::vector<u8> file;

    // orig: 0x000CB7B0 ParseRiffMemory (non-streaming) and the file loader 0x000CB9F0 (its streaming branch: a data
    // chunk of at least nAvgBytesPerSec keeps ten packets of pre-roll, +0x18 the chunk's length).
    bool ParseRiff(const u8* file, u32 size, bool streaming = false);
    f32 Duration() const;                            // orig: 0x000CB320
};

class Voice {                                        // orig: 0xBC bytes (ctor 0x000C9E30 + 0x000CB2D0)
public:
    Voice(SoundSystem& sys, Sample& sample, bool is3D);
    ~Voice();                                        // orig: 0x000CC0C0

    void Play2D(bool loop);                          // orig: 0x000CC420
    void Play3D(const ni::NiPoint3& pos, bool loop); // orig: 0x000CC4A0
    void PlayAttached(ni::NiAVObject* node, bool loop);   // orig: 0x000CC540
    void Pause();                                    // orig: 0x000CB5D0
    void Resume();                                   // orig: 0x000CA150
    void Stop();                                     // orig: 0x000CB610
    void Update();                                   // orig: 0x000CC670

    SoundSystem& system;
    Sample& sample;                                  // orig: +0x18
    s32 id;                                          // orig: +0x6C
    const void* targetable = nullptr;                // orig: +0x04
    ni::NiPointer<ni::NiAVObject> node;              // orig: +0x08
    ni::NiPoint3 position;                           // orig: +0x0C
    bool created = false;                            // orig: +0x1C
    bool pending = false;                            // orig: +0x1D
    bool is3D = false;                               // orig: +0x1E
    bool paused = false;                             // orig: +0x1F
    bool finished = false;                           // orig: +0x20
    bool dirty = false;                              // orig: +0x21
    bool loop = false;                               // orig: +0x22
    f32 volume = 1.0f;                               // orig: +0x24
    f32 pan = 0.0f;                                  // orig: +0x28
    f32 pitch = 1.0f;                                // orig: +0x2C
    f32 minDistance, maxDistance;                    // orig: +0x30 / +0x34
    f32 fadeIn = 0, fadeOut = 0, fadeTimer = 0;      // orig: +0x38 / +0x3C / +0x44
    u32 category = 0;                                // orig: +0x70 (0 effects, 1 speech, 2 music)
    SoundBackend::Buffer buffer = 0;                 // orig: +0xA4
    // The stream of a streaming sample's voice (orig: +0x74 IDirectSoundStream, +0x60 the packet buffer of +0xA8
    // bytes, +0xAC the packet size, +0x7C the ten packets' status words, +0xB0 the read cursor relative to the data,
    // +0x64 the file stream: here the sample's file, read synchronously).
    SoundBackend::Buffer stream = 0;
    std::vector<u8> packets;
    u32 packetSize = 0;
    std::array<u32, 10> packetStatus{};              // (the ctor leaves them unset: heap garbage, 0 here, L)
    u32 cursor = 0;
    bool reader = false;                             // orig: +0x64 != 0

private:
    void CreateBufferObject();                       // orig: 0x000CC1E0
    void Refill();                                   // orig: 0x000CB640 (from the job 0x000CB770 the update pumps)
    void ApplyFade(f32& vol);                        // orig: 0x000C9510
    void StartPlay(bool loopIt, bool threeD);
};

class SoundSystem {                                  // orig: 0x7C bytes (ctor 0x000CA3A0 + 0x000CCD90)
public:
    // `backend` may be null (no audio device: every call is a no-op, as when DirectSoundCreate fails).
    explicit SoundSystem(SoundBackend* backend);
    ~SoundSystem();

    // orig: 0x000CCED0 (SoundCache_LoadFile 0x000C9840): a WAVE image in memory (a NiUDSFileObject).
    Sample* LoadSampleFromMemory(const std::string& name, const u8* data, u32 size);
    // orig: 0x000CCF70 (Sound_Load 0x000C9860): `read(path, bytes)` reads a disc file. De-duplicates by prefix.
    template <class Read>
    Sample* LoadSample(const std::string& path, bool streaming, Read&& read);
    void ReleaseSample(Sample* s);                   // orig: 0x000C9880 Sound_Release

    s32 PlaySfx(Sample* s, bool loop);               // orig: 0x000C99A0 (category 0)
    s32 PlaySpeech(Sample* s, bool loop);            // orig: 0x000C9A60 (category 1)
    s32 PlayMusic(Sample* s, bool loop);             // orig: 0x000C9A00 (category 2)
    s32 Play3D(Sample* s, const ni::NiPoint3& pos, bool loop);   // orig: 0x000C9AC0 (refused at 8 voices)
    s32 PlayAttached(Sample* s, ni::NiAVObject* node, bool loop);   // orig: 0x000CAB80
    bool VoiceExists(s32 id) const;                  // orig: 0x000C9900
    bool HasVoices(Sample* s) const;                 // orig: 0x000C98C0
    f32 Duration(Sample* s) const;                   // orig: 0x000C9930
    void VoicePlay2D(s32 id, bool loop);             // orig: 0x000C9970
    void StopVoice(s32 id);                          // orig: 0x000C9B10
    void PauseVoice(s32 id);                         // orig: 0x000C9BA0
    void SetPitch(s32 id, f32 pitch);                // orig: 0x000C9BD0 (clamped 0..4)
    void SetPan(s32 id, f32 pan);                    // orig: 0x000C9C00
    f32 GetVolume(s32 id) const;                     // orig: 0x000C9C30
    void SetVolume(s32 id, f32 volume);              // orig: 0x000C9C70 (clamped 0..1)
    void SetMaxDistance(s32 id, f32 d);              // orig: 0x000C9CA0 (kept >= min)
    void SetMinDistance(s32 id, f32 d);              // orig: 0x000C9CF0 (kept <= max)
    void FadeOut(s32 id, f32 time);                  // orig: 0x000C9D40 (no audible effect, as the original)
    void StopAll();                                  // orig: 0x000C9B40
    void StopSample(Sample* s);                      // orig: 0x000C9B70
    void StopAllOnTargetable(const void* target);    // orig: 0x000CA690 (every voice whose +0x04 is `target`)
    // orig: the tail of FsPlay3DSFX 0x000CAAA0 after its owned-sample test and the record lookup: with a node the
    // sample's vt+0xC (attached, no voice limit), else vt+0x10 at `pos` (refused at 8 voices); then the voice's
    // vt+0x10(0) (category 0) and +0x04 = `target`.
    s32 PlayAtTarget(Sample* s, const void* target, ni::NiAVObject* node, const ni::NiPoint3& pos, bool loop);
    void DeleteAllSamples();                         // orig: vtbl+0x0C 0x000C96C0 (each sample's deleting dtor)
    void PauseAll();                                 // orig: 0x000CAD30
    void ResumeAll();                                // orig: 0x000CA610
    void SetEnvironment(u32 preset);                 // orig: 0x000CC030 (0..4; others ignored)
    f32 CategoryVolume(u32 category) const;          // orig: 0x000C96E0
    void SetCategoryVolume(u32 category, f32 v);     // orig: vtbl+0x28 / +0x30 / +0x38

    // orig: 0x000CBD40, once per rendered frame. The listener is the level camera (position, world rotation),
    // or the origin / identity without one.
    void Update(const ni::NiPoint3* cameraPos, const ni::NiMatrix3* cameraRotate);

    SoundBackend* backend;                           // orig: +0x48 (IDirectSound*)
    std::vector<std::unique_ptr<Sample>> samples;    // orig: list +0x08 (oldest first)
    std::vector<Voice*> voices;                      // orig: list +0x0C (oldest first)
    std::vector<s32> pausedIds;                      // orig: array +0x10
    f32 volumes[3] = {1, 1, 1};                      // orig: +0x34 effects, +0x38 speech, +0x3C music
    f32 rolloffCurve[6] = {};                        // orig: 0x002EADDC (set once the effects image loads)
    static s32 nextVoiceId;                          // orig: 0x002D6880 (initial 1, wraps 0 -> 100)

    // orig: the wrappers' "find sample" walk of the list +0x08 (pointer equality): every game-facing call on a sample
    // first tests that the system holds it.
    bool Owns(const Sample* s) const;

private:
    friend class Voice;
    Voice* FindVoice(s32 id) const;
    s32 Start2D(Sample* s, bool loop, u32 category);
    void DeleteVoice(Voice* v);
};

template <class Read>
Sample* SoundSystem::LoadSample(const std::string& path, bool streaming, Read&& read) {   // orig: 0x000CCF70
    if (path.size() <= 3) return nullptr;
    const size_t n = path.size() - 3;                // extension ignored: _strnicmp (0x000CCFC8) over the rest
    for (const auto& s : samples)
        if (crt::StrNICmp(s->name.c_str(), path.c_str(), n) == 0) return s.get();
    std::vector<u8> bytes;
    if (!read(path, bytes)) return nullptr;
    auto s = std::make_unique<Sample>();
    s->streaming = streaming;                        // orig: LoadFromFile 0x000CCC90: +0x27 before the parse
    if (!s->ParseRiff(bytes.data(), static_cast<u32>(bytes.size()), streaming)) return nullptr;
    s->name = path;
    if (streaming) s->file = std::move(bytes);       // (the voices' packets are read from it)
    samples.push_back(std::move(s));
    return samples.back().get();
}

}  // namespace fut::sound

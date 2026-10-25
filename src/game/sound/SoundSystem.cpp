// The game's sound layer; see SoundSystem.h. Floating point follows the original instruction by instruction
// (NiMath.h): x87 values are `double`, F() marks every store to a float, _ftol truncates toward zero.
#include "game/sound/SoundSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fut::sound {
namespace {

using ni::detail::F;

u32 U32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
u16 U16(const u8* p) {
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}

// orig: _ftol (0x00167110): x87 to integer with truncation; out-of-range and NaN give 0x80000000.
s32 Ftol(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return static_cast<s32>(0x80000000u);
    return static_cast<s32>(v);
}

constexpr ni::NiPoint3 kZero{};

}  // namespace

s32 SoundSystem::nextVoiceId = 1;

// ---- Sample -------------------------------------------------------------------------------------------------------
bool Sample::ParseRiff(const u8* file, u32 size, bool stream) {   // orig: 0x000CB7B0 / 0x000CB9F0
    if (size < 12 || std::memcmp(file, "RIFF", 4) != 0 || std::memcmp(file + 8, "WAVE", 4) != 0) return false;
    pcmFormat.assign(18, 0);
    adpcmFormat.assign(20, 0);
    adpcm = false;
    std::vector<u8> buf;
    bool haveData = false;
    for (u32 pos = 12; pos + 8 <= size;) {
        const u8* c = file + pos;
        const u32 len = U32(c + 4);
        const u32 next = pos + 8 + len;
        if (std::memcmp(c, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataLength = len;
            if (stream && !(len < U32(&pcmFormat[8]))) {   // orig: 0x000CB9F0: streaming and len >= +0x3C
                // the packet from the PCM fields: x = (u32)(+0x38 rate * +0x40 blockAlign * 3) / 10, x - x % align;
                // +0x14 = ten packets, read as the pre-roll (+0x18 keeps the chunk's length)
                const u32 align = U16(&pcmFormat[12]);
                const u32 x = U32(&pcmFormat[4]) * align * 3 / 10;
                packetSize = align ? x - x % align : x;   // (a zero align divides by zero in the original)
                bytes = packetSize * 10;
                if (pos + 8 + bytes > size) return false;   // (the original reads short; L)
                buf.assign(c + 8, c + 8 + bytes);
                haveData = true;
                pos = next;
                continue;
            }
            const u32 align = adpcm ? U16(&adpcmFormat[12]) : U16(&pcmFormat[12]);
            const u32 rate = adpcm ? U32(&adpcmFormat[4]) : U32(&pcmFormat[4]);
            const u32 x = rate * align * 3 / 10;
            packetSize = align ? x - x % align : x;
            if (pos + 8 + len > size) return false;
            buf.assign(c + 8, c + 8 + len);
            bytes = dataLength = len;
            haveData = true;
        } else if (std::memcmp(c, "fmt ", 4) == 0 && len >= 16) {
            std::fill(pcmFormat.begin(), pcmFormat.end(), u8(0));
            std::memcpy(pcmFormat.data(), c + 8, std::min<u32>(len, 18));
            pcmFormat[16] = 0x12;                     // cbSize forced to 0x12
            pcmFormat[17] = 0;
            if (U16(c + 8) == 0x69) {                 // build the XBOXADPCMWAVEFORMAT
                const u16 ch = U16(c + 10);
                const u32 rate = U32(c + 12);
                const u16 blockAlign = static_cast<u16>(ch * 36);
                const s32 t = static_cast<s32>(static_cast<u16>(ch * 36)) * ch;
                const u32 avg = static_cast<u32>((t + ((t >> 31) & 63)) >> 6);   // (literal: not rate based)
                const u16 tag = 0x69, bits = 4, cb = 2, spb = 64;
                std::memcpy(&adpcmFormat[0], &tag, 2);
                std::memcpy(&adpcmFormat[2], &ch, 2);
                std::memcpy(&adpcmFormat[4], &rate, 4);
                std::memcpy(&adpcmFormat[8], &avg, 4);
                std::memcpy(&adpcmFormat[12], &blockAlign, 2);
                std::memcpy(&adpcmFormat[14], &bits, 2);
                std::memcpy(&adpcmFormat[16], &cb, 2);
                std::memcpy(&adpcmFormat[18], &spb, 2);
                adpcm = true;
            }
        }
        pos = next;
    }
    if (!haveData || bytes < 4 || bytes > 0x0FFFFFFF) return false;
    data = std::move(buf);
    mono = (adpcm ? U16(&adpcmFormat[2]) : U16(&pcmFormat[2])) < 2;
    sampleRate = U32(&pcmFormat[4]);
    loaded = true;
    return true;
}

f32 Sample::Duration() const {                        // orig: 0x000CB320
    const u32 ch = U16(&pcmFormat[2]);
    const u32 bits = U16(&pcmFormat[14]);
    if (adpcm) {
        return F(double(s32(dataLength)) /
                 (((double(F(double(sampleRate))) * double(F(double(bits)))) * 0.125) * double(F(double(ch)))));
    }
    const s32 prod = static_cast<s32>((bits >> 3) * ch * sampleRate);
    double denom = prod;
    if (prod < 0) denom += 4294967296.0;              // unsigned fix-up of the FILD
    return F(double(s32(dataLength)) / double(F(denom)));
}

// ---- Voice --------------------------------------------------------------------------------------------------------
Voice::Voice(SoundSystem& sys, Sample& s, bool threeD)   // orig: 0x000C9E30 + 0x000CB2D0
    : system(sys), sample(s), id(SoundSystem::nextVoiceId++), is3D(threeD),
      minDistance(s.minDistance), maxDistance(s.maxDistance) {
    if (SoundSystem::nextVoiceId == 0) SoundSystem::nextVoiceId = 100;
    s.voices.push_back(this);
    sys.voices.push_back(this);
}

Voice::~Voice() {                                     // orig: 0x000CC0C0
    SoundBackend* b = system.backend;
    Pause();
    if (buffer) b->SetBufferData(buffer, nullptr, 0);
    paused = false;
    finished = true;
    while (buffer && b->GetStatus(buffer) != 0) {
    }
    // (a stream: +0x68 = 0, then the job pumped until done: it leaves its loop without another Refill)
    reader = false;                                   // +0x64 released
    if (buffer) b->Release(buffer);
    buffer = 0;
    if (stream) b->Release(stream);                   // +0x74 (vt+4 Release), then +0x60 freed
    stream = 0;
    sample.voices.erase(std::find(sample.voices.begin(), sample.voices.end(), this));
    system.voices.erase(std::find(system.voices.begin(), system.voices.end(), this));
}

void Voice::StartPlay(bool loopIt, bool threeD) {
    // The shared tail of 0x000CC420 / 0x000CC4A0 / 0x000CC540 when the voice was paused: a streaming sample's stream
    // resumed (IDirectSoundStream_Pause(+0x74, 0)), else the buffer played again (IDirectSoundBuffer_Play(+0xA4, 0, 0,
    // loop) with the voice's +0x22).
    (void)loopIt;
    (void)threeD;
    if (sample.streaming) {
        if (stream) system.backend->PauseStream(stream, false);
    } else if (buffer) {
        system.backend->Play(buffer, loop ? 1 : 0);
    }
}

void Voice::Play2D(bool loopIt) {                     // orig: 0x000CC420
    const bool wasPaused = paused;
    paused = false;
    if (wasPaused) {
        StartPlay(loopIt, false);
        return;
    }
    is3D = false;
    loop = loopIt;
    node = nullptr;
    pending = true;
}

void Voice::Play3D(const ni::NiPoint3& pos, bool loopIt) {   // orig: 0x000CC4A0
    const bool wasPaused = paused;
    paused = false;
    if (wasPaused) {
        StartPlay(loopIt, true);
        return;
    }
    is3D = true;
    loop = loopIt;
    node = nullptr;
    position = pos;
    pending = true;
}

void Voice::PlayAttached(ni::NiAVObject* n, bool loopIt) {   // orig: 0x000CC540
    const bool wasPaused = paused;
    paused = false;
    if (wasPaused) {
        StartPlay(loopIt, true);
        return;
    }
    is3D = true;
    loop = loopIt;
    node = n;
    pending = true;
}

void Voice::Pause() {                                 // orig: 0x000CB5D0 (the sample's vt+0x30 first: a nop)
    paused = true;
    if (sample.streaming) {
        if (stream) system.backend->PauseStream(stream, true);   // IDirectSoundStream_Pause(+0x74, 1)
    } else if (buffer) {
        system.backend->Stop(buffer);                 // IDirectSoundBuffer_Stop(+0xA4)
    }
}

void Voice::Resume() {                                // orig: 0x000CA150
    if (!is3D) {
        Play2D(loop);
    } else if (node) {
        PlayAttached(node.Get(), loop);
    } else {
        Play3D(position, loop);
    }
}

void Voice::Stop() {                                  // orig: 0x000CB610
    Pause();
    if (buffer) system.backend->SetBufferData(buffer, nullptr, 0);
    paused = false;
    finished = true;
}

void Voice::CreateBufferObject() {                    // orig: 0x000CC1E0
    SoundBackend* b = system.backend;
    created = true;
    const bool use3D = sample.mono && is3D;
    if (sample.streaming) {
        // DSSTREAMDESC{use3D ? 0x60010 : 0, 10 packets, the PCM header +0x34 (also for ADPCM), no callback, no
        // context, no mix bins}; +0xAC = the packet size, +0xA8 = the sample's ten packets, +0x60 = new(+0xA8),
        // +0x64 = new NiUDSStream(name, voice) (its job's callback 0x000CB770)
        stream = b->CreateStreamVoice(use3D ? 0x60010u : 0u, 10, sample.pcmFormat);
        packetSize = sample.packetSize;
        // (a buffer smaller than ten packets, a "streaming" sample shorter than a second loaded whole, would be
        // overrun by Refill in the original: kept at ten packets, L)
        packets.assign(std::max<size_t>(sample.bytes, size_t(packetSize) * 10), u8(0));
        reader = !sample.file.empty();
    } else {
        buffer = b->CreateBuffer((use3D ? 0x60010u : 0u) | 0x100u,
                                 sample.adpcm ? sample.adpcmFormat : sample.pcmFormat);
        b->SetBufferData(buffer, sample.data.data(), sample.bytes);
    }
    if (use3D) {
        static const MixBin kBins[6] = {{6, 0}, {8, 0}, {7, 0}, {9, 0}, {10, 0}, {3, 0}};
        const SoundBackend::Buffer t = sample.streaming ? stream : buffer;   // (the stream or the buffer variant)
        b->SetMixBins(t, kBins, 6);
        b->SetRolloffCurve(t, system.rolloffCurve, 6, 0);
    }
}

void Voice::Refill() {                                // orig: 0x000CB640 (RET 4: the job's stream, not read here)
    if (!stream) return;
    const u32 dataLength = sample.dataLength;         // sample +0x18: the data chunk's length (not data offset + it)
    const u32 dataOffset = sample.dataOffset;         // sample +0x30
    for (u32 i = 0; i < 10; ++i) {
        if (packetStatus[i] == 0x8000000Au) continue; // XMEDIAPACKET_STATUS_PENDING
        u8* dst = packets.data() + size_t(i) * packetSize;
        u32 n = packetSize;
        // Seek(+0x30 + cursor); the end test compares the file position with the data length, so the data's last
        // +0x30 bytes never play and each lap ends with silence (unsigned compare)
        if (cursor + dataOffset + packetSize > dataLength) {
            n = dataLength - cursor - dataOffset;
            std::memset(dst + n, 0, packetSize - n);
        }
        const u32 at = dataOffset + cursor;           // the stream's read (vf08 of +8): short past the file's end
        const u32 have = at < sample.file.size() ? std::min<u32>(n, static_cast<u32>(sample.file.size() - at)) : 0;
        if (have) std::memcpy(dst, sample.file.data() + at, have);
        const s32 period = static_cast<s32>(dataLength - dataOffset);   // CDQ; IDIV (signed)
        cursor = period ? static_cast<u32>(static_cast<s32>(cursor + n) % period) : 0;   // (0: a fault, L)
        system.backend->ProcessPacket(stream, dst, packetSize, &packetStatus[i]);   // vt+0x10 Process(&pkt, NULL)
    }
}

void Voice::ApplyFade(f32& vol) {                     // orig: 0x000C9510
    (void)vol;
    // Nothing ever sets a fade-in, and the fade-out uses the fade-in time, so no fade changes the volume; the
    // timers still run (fadeTimer += g_frameStep). Not modelled: the timers are not observable.
    if (fadeTimer < fadeIn) {
        // unreachable in the game (fadeIn is always 0)
    } else if (fadeTimer < fadeOut) {
        // vol - (0 / timer) * vol leaves vol unchanged
    }
}

void Voice::Update() {                                // orig: 0x000CC670
    SoundBackend* b = system.backend;
    const bool streaming = sample.streaming;
    if (paused || !sample.loaded || finished) return;
    const auto target = [&] { return streaming ? stream : buffer; };   // (the stream or the buffer variant)
    auto attached = [&](ni::NiPoint3& p, ni::NiPoint3& v) {
        if (node) {
            p = node->world.translate;                // node +0xA0
            v = kZero;                                // 0x000CB590's static zero
        } else {
            p = position;
            v = kZero;
        }
    };
    if (pending) {
        CreateBufferObject();
        if (is3D) {
            if (!sample.mono) {                       // stereo sounds never play in 3D
                finished = true;
                return;
            }
            ni::NiPoint3 p, v;
            attached(p, v);
            if (!streaming) {
                b->SetMinDistance(buffer, minDistance, 1);
                b->SetMaxDistance(buffer, maxDistance, 1);
                b->SetPosition(buffer, p.x, p.y, p.z, 1);
                b->SetVelocity(buffer, v.x, v.y, v.z, 1);
            }
        }
        if (!streaming) b->Play(buffer, loop ? 1 : 0); // IDirectSoundBuffer_Play(+0xA4, 0, 0, loop)
        else b->PauseStream(stream, false);            // IDirectSoundStream_Pause(+0x74, DSSTREAMPAUSE_RESUME): no null test
                                                      //  here (0x000CC79B; a backend ignores stream 0)
        dirty = true;
    }
    if (is3D) {
        ni::NiPoint3 p, v;
        attached(p, v);
        b->SetPosition(target(), p.x, p.y, p.z, 1);
        b->SetVelocity(target(), v.x, v.y, v.z, 1);
        b->SetMinDistance(target(), minDistance, 1);
        b->SetMaxDistance(target(), maxDistance, 1);
    }
    f32 vol = F(double(system.CategoryVolume(category)) * volume);
    ApplyFade(vol);
    s32 db;
    if (vol > 1.0f) {
        vol = 1.0f;
        db = Ftol((1.0 - vol) * -1000.0);
    } else if (vol < 0.0f) {
        vol = 0.0f;
        db = -10000;
    } else if (vol == 0.0f) {
        db = -10000;
    } else {                                          // (NaN lands here)
        db = Ftol((1.0 - vol) * -1000.0);
    }
    const s32 freq = Ftol(double(static_cast<s32>(sample.sampleRate)) * pitch);
    b->SetFrequency(target(), static_cast<u32>(freq));
    b->SetVolume(target(), db);
    if (!is3D) {                                      // pan scales the attenuation of the two front bins
        f32 l = F(double(pan) + 1.0);
        double r = 1.0 - double(pan);
        if (l > 1.0f) l = 1.0f;
        if (r > 1.0) r = 1.0;
        const double dbf = db;
        const MixBin bins[2] = {{0, Ftol(double(l) * dbf)}, {1, Ftol(dbf * r)}};
        b->SetMixBins(target(), bins, 2);
    } else {
        std::array<s32, 9> i3dl2{};
        i3dl2[2] = -600;                              // lRoom
        b->SetI3DL2Source(buffer, i3dl2, 1);          // (on +0xA4 also for a stream, where it is NULL: literal)
    }
    if (streaming || (b->GetStatus(buffer) & 1) || paused || pending) {
        pending = false;
        dirty = false;
        if (reader) Refill();                         // Pump(+0x64): the job 0x000CB770 runs one Refill and yields
    } else {
        finished = true;
    }
}

// ---- SoundSystem --------------------------------------------------------------------------------------------------
SoundSystem::SoundSystem(SoundBackend* b) : backend(b) {   // orig: 0x000CA3A0 + 0x000CCD90
    if (!backend) return;
    backend->DownloadEffectsImage();
    backend->UseFullHrtf();
    backend->SetDopplerFactor(2.0f, 0);
    backend->SetDistanceFactor(0.2f, 1);
    backend->SetRolloffFactor(1.0f, 1);
    backend->ListenerPosition(0, 0, 0, 1);
    backend->SetI3DL2Listener(0, 1);
    const f32 curve[6] = {1.0f, 0.9f, 0.8f, 0.5f, 0.25f, 0.0f};   // orig: 0x002EADDC
    std::copy(curve, curve + 6, rolloffCurve);
}

SoundSystem::~SoundSystem() {                         // orig: 0x000CBCC0
    while (!samples.empty()) ReleaseSample(samples.front().get());
}

bool SoundSystem::Owns(const Sample* s) const {
    for (const auto& x : samples)
        if (x.get() == s) return true;
    return false;
}

Voice* SoundSystem::FindVoice(s32 id) const {
    for (Voice* v : voices)
        if (v->id == id) return v;
    return nullptr;
}

void SoundSystem::DeleteVoice(Voice* v) { delete v; }

Sample* SoundSystem::LoadSampleFromMemory(const std::string& name, const u8* data, u32 size) {   // orig: 0x000CCED0
    auto s = std::make_unique<Sample>();
    if (!s->ParseRiff(data, size)) return nullptr;
    s->name = name;
    samples.push_back(std::move(s));
    return samples.back().get();
}

void SoundSystem::ReleaseSample(Sample* s) {          // orig: 0x000C9880 -> sample dtor 0x000CD070 / 0x000CA2D0
    if (!Owns(s)) return;
    while (!s->voices.empty()) delete s->voices.front();
    for (auto it = samples.begin(); it != samples.end(); ++it)
        if (it->get() == s) {
            samples.erase(it);
            break;
        }
}

s32 SoundSystem::Start2D(Sample* s, bool loop, u32 category) {
    if (!Owns(s)) return 0;                           // (the wrappers' sample-list walk)
    Voice* v = new Voice(*this, *s, false);           // orig: sample vtbl+8 0x000CB390
    v->Play2D(loop);
    v->category = category;                           // orig: voice vtbl+0x10
    return v->id;
}

s32 SoundSystem::PlaySfx(Sample* s, bool loop) { return Start2D(s, loop, 0); }
s32 SoundSystem::PlaySpeech(Sample* s, bool loop) { return Start2D(s, loop, 1); }
s32 SoundSystem::PlayMusic(Sample* s, bool loop) { return Start2D(s, loop, 2); }

s32 SoundSystem::Play3D(Sample* s, const ni::NiPoint3& pos, bool loop) {   // orig: 0x000C9AC0 -> 0x000CB440
    if (!Owns(s) || s->voices.size() >= 8) return 0;   // (the list walk; then 0x000CB440's limit)
    Voice* v = new Voice(*this, *s, true);
    v->Play3D(pos, loop);
    return v->id;
}

s32 SoundSystem::PlayAttached(Sample* s, ni::NiAVObject* node, bool loop) {   // orig: 0x000CAB80 -> 0x000CCB30
    if (!Owns(s)) return 0;
    Voice* v = new Voice(*this, *s, true);
    v->PlayAttached(node, loop);
    return v->id;
}

bool SoundSystem::VoiceExists(s32 id) const { return FindVoice(id) != nullptr; }

bool SoundSystem::HasVoices(Sample* s) const { return Owns(s) && !s->voices.empty(); }

f32 SoundSystem::Duration(Sample* s) const { return Owns(s) ? s->Duration() : 0.0f; }

void SoundSystem::VoicePlay2D(s32 id, bool loop) {
    if (Voice* v = FindVoice(id)) v->Play2D(loop);
}

void SoundSystem::StopVoice(s32 id) {
    if (Voice* v = FindVoice(id)) v->Stop();
}

void SoundSystem::PauseVoice(s32 id) {
    if (Voice* v = FindVoice(id)) v->Pause();
}

void SoundSystem::SetPitch(s32 id, f32 pitch) {       // orig: 0x000CA1A0
    Voice* v = FindVoice(id);
    if (!v) return;
    if (pitch > 4.0f) {
        pitch = 4.0f;
    } else if (pitch < 0.0f) {
        pitch = 0.0f;
    }
    v->pitch = pitch;
    v->dirty = true;
}

void SoundSystem::SetPan(s32 id, f32 pan) {           // orig: 0x000C9FE0 (no clamp)
    Voice* v = FindVoice(id);
    if (!v) return;
    v->pan = pan;
    v->dirty = true;
}

f32 SoundSystem::GetVolume(s32 id) const {
    const Voice* v = FindVoice(id);
    return v ? v->volume : 0.0f;
}

void SoundSystem::SetVolume(s32 id, f32 volume) {     // orig: 0x000C94C0
    Voice* v = FindVoice(id);
    if (!v) return;
    if (volume > 1.0f) {
        volume = 1.0f;
    } else if (volume < 0.0f) {
        volume = 0.0f;
    }
    v->volume = volume;
    v->dirty = true;
}

void SoundSystem::SetMaxDistance(s32 id, f32 d) {     // orig: 0x000C9CA0
    Voice* v = FindVoice(id);
    if (!v) return;
    if (d < v->minDistance) d = v->minDistance;
    v->maxDistance = d;
    v->dirty = true;
}

void SoundSystem::SetMinDistance(s32 id, f32 d) {     // orig: 0x000C9CF0
    Voice* v = FindVoice(id);
    if (!v) return;
    if (v->maxDistance < d) d = v->maxDistance;
    v->minDistance = d;
    v->dirty = true;
}

void SoundSystem::FadeOut(s32 id, f32 time) {         // orig: 0x000C9D40
    Voice* v = FindVoice(id);
    if (!v) return;
    v->fadeOut = time;
    v->fadeIn = 0;
    v->fadeTimer = 0;
}

void SoundSystem::StopAll() {                         // orig: 0x000C9B40
    for (const auto& s : samples)
        for (Voice* v : std::vector<Voice*>(s->voices)) v->Stop();
}

void SoundSystem::StopSample(Sample* s) {             // orig: 0x000C9B70
    if (!Owns(s)) return;
    for (Voice* v : std::vector<Voice*>(s->voices)) v->Stop();
}

void SoundSystem::StopAllOnTargetable(const void* target) {   // orig: 0x000CA690
    for (const auto& s : samples)                     // (each sample's voices +0x0C, through +0x48)
        for (Voice* v : std::vector<Voice*>(s->voices))
            if (v->targetable == target) v->Stop();   // vt+0x28
}

s32 SoundSystem::PlayAtTarget(Sample* s, const void* target, ni::NiAVObject* node, const ni::NiPoint3& pos,
                              bool loop) {            // orig: 0x000CAAFD..0x000CAB7D
    Voice* v = nullptr;
    if (node) {                                       // sample vt+0xC 0x000CCB30 (NiPointer by value)
        v = new Voice(*this, *s, true);
        v->PlayAttached(node, loop);
    } else {                                          // sample vt+0x10 0x000CB440
        if (s->voices.size() >= 8) return 0;
        v = new Voice(*this, *s, true);
        v->Play3D(pos, loop);
    }
    v->category = 0;                                  // voice vt+0x10(0)
    v->targetable = target;                           // +0x04
    return v->id;
}

void SoundSystem::DeleteAllSamples() {                // orig: 0x000C96C0
    while (!samples.empty()) ReleaseSample(samples.front().get());   // (head first; each dtor deletes its voices)
}

void SoundSystem::PauseAll() {                        // orig: 0x000CAD30
    for (const auto& s : samples)
        for (Voice* v : s->voices) {
            v->Pause();
            pausedIds.push_back(v->id);
        }
}

void SoundSystem::ResumeAll() {                       // orig: 0x000CA610
    for (s32 id : pausedIds)
        if (Voice* v = FindVoice(id)) v->Resume();
    pausedIds.clear();
}

void SoundSystem::SetEnvironment(u32 preset) {        // orig: 0x000CC030
    if (backend && preset <= 4) backend->SetI3DL2Listener(preset, 1);
}

const audio::xds::I3DL2Listener& I3DL2Preset(u32 preset) {   // orig: the records SetEnvironment 0x000CC030 picks
    static const u32 kWords[5][12] = {   // the DSI3DL2LISTENER fields' bits (docs/analysis/sound_system.md 4.4)
        {0xFFFFFC18, 0xFFFFE890, 0, 0x3E2E147B, 0x3DCCCCCD, 0xFFFFFB4C, 0x3A83126F, 0x000000CF, 0x3B03126F, 0x42C80000,
         0x42C80000, 0x459C4000},        // 0x002D6924 Padded Cell
        {0xFFFFFC18, 0xFFFFFE3A, 0, 0x3ECCCCCD, 0x3F547AE1, 0xFFFFF992, 0x3B03126F, 0x00000035, 0x3B449BA6, 0x42C80000,
         0x42C80000, 0x459C4000},        // 0x002D6958 Room
        {0xFFFFFC18, 0xFFFFE890, 0, 0x3F000000, 0x3DCCCCCD, 0xFFFFFAA0, 0x3B449BA6, 0xFFFFFBB0, 0x3B83126F, 0x42C80000,
         0x42C80000, 0x459C4000},        // 0x002D69C0 Living Room
        {0xFFFFFC18, 0xFFFFFED4, 0, 0x4013D70A, 0x3F23D70A, 0xFFFFFD39, 0x3C449BA6, 0x00000053, 0x3C8B4396, 0x42C80000,
         0x42C80000, 0x459C4000},        // 0x002D69F4 Stone Room
        {0xFFFFFC18, 0xFFFFFE0C, 0, 0x407AE148, 0x3F333333, 0xFFFFFB32, 0x3CA3D70A, 0xFFFFFFFE, 0x3CED9168, 0x42C80000,
         0x42C80000, 0x459C4000},        // 0x002D6A5C Concert Hall
    };
    static_assert(sizeof(audio::xds::I3DL2Listener) == sizeof(kWords[0]));
    static const auto kPresets = [] {
        std::array<audio::xds::I3DL2Listener, 5> p;
        for (u32 i = 0; i < 5; ++i) std::memcpy(&p[i], kWords[i], sizeof(kWords[i]));
        return p;
    }();
    return kPresets[preset <= 4 ? preset : 0];
}

f32 SoundSystem::CategoryVolume(u32 category) const { return category < 3 ? volumes[category] : 1.0f; }

void SoundSystem::SetCategoryVolume(u32 category, f32 v) {
    if (category < 3) volumes[category] = v;
}

void SoundSystem::Update(const ni::NiPoint3* cameraPos, const ni::NiMatrix3* cameraRotate) {   // orig: 0x000CBD40
    if (!backend) return;
    ni::NiPoint3 pos = kZero;
    // The listener matrix as the original reads it: 12 floats at the camera node's world rotation (vector k =
    // column k, padded to 4), or the identity 0x002D7820.
    f32 m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (cameraPos && cameraRotate) {
        pos = *cameraPos;
        for (int k = 0; k < 3; ++k)
            for (int r = 0; r < 3; ++r) m[4 * k + r] = cameraRotate->m[r][k];
    }
    const double fx = 1, fy = 0, fz = 0, ux = 0, uy = 1, uz = 0;   // orig: 0x002D77E0 / 0x002D77EC
    double front[3];
    for (int k = 0; k < 3; ++k) front[k] = (double(m[8 + k]) * fz + double(m[4 + k]) * fy) + double(m[k]) * fx;
    const f32 f0 = F(front[0] * -1.0), f1 = F(front[1] * -1.0), f2 = F(double(F(front[2])) * -1.0);
    f32 top[3];
    for (int k = 0; k < 3; ++k) top[k] = F((double(m[8 + k]) * uz + double(m[4 + k]) * uy) + double(m[k]) * ux);
    backend->ListenerPosition(pos.x, pos.y, pos.z, 1);
    backend->ListenerVelocity(0, 0, 0, 1);
    backend->ListenerOrientation(f0, f1, f2, top[0], top[1], top[2], 1);
    for (const auto& s : samples) {                   // orig: 0x000C9680 -> sample update 0x000C95F0
        for (size_t i = 0; i < s->voices.size();) {
            Voice* v = s->voices[i];
            v->Update();
            if (v->finished) {
                delete v;                             // (unlinks itself)
            } else {
                ++i;
            }
        }
    }
    backend->CommitDeferredSettings();
    backend->DoWork();
}

}  // namespace fut::sound

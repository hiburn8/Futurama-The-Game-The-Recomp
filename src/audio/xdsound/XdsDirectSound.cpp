// XDK DirectSound state model; see XdsDirectSound.h.
#include "audio/xdsound/XdsDirectSound.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fut::audio::xds {

u16 VoiceRegisters::Attenuation(u32 k, u32 i) const {
    u16 s[8];
    UnpackVolumes(volume[k], volume[3 + k], volume[6 + k], s);
    return s[i];
}

double VoiceRegisters::Rate() const {
    return 48000.0 * std::exp2(static_cast<double>(static_cast<s32>(pitch) >> 16) / 4096.0);
}

DirectSound::DirectSound(const Tables* tables) : tables_(tables ? tables : &none_) {
    frame_.listener = &listener_;
}

DirectSound::Voice* DirectSound::Find(u32 id) {
    auto it = voices_.find(id);
    return it == voices_.end() ? nullptr : &it->second;
}

const VoiceRegisters* DirectSound::Registers(u32 id) const {
    auto it = voices_.find(id);
    return it == voices_.end() ? nullptr : &it->second.regs;
}

void DirectSound::SetReverbState(const u32 words[70]) {
    std::memcpy(reverbState_.data(), words, sizeof(u32) * 70);
    haveReverbState_ = true;
}

// ---- listener (orig: CDirectSound_Set* 0x00248A..; dirty bits docs/analysis/audio_port.md 2.1) --------------------
void DirectSound::ChangedListener(u32 bits, u32 apply) {
    listenerDirty_ |= bits;
    if (apply == 0) CommitDeferredSettings();       // DS3D_IMMEDIATE
}

void DirectSound::SetDopplerFactor(f32 f, u32 apply) {
    listener_.dopplerFactor = f;
    ChangedListener(0x80, apply);
}

void DirectSound::SetDistanceFactor(f32 f, u32 apply) {
    listener_.distanceFactor = f;
    ChangedListener(0xE0, apply);
}

void DirectSound::SetRolloffFactor(f32 f, u32 apply) {
    listener_.rolloffFactor = f;
    ChangedListener(4, apply);
}

void DirectSound::SetPosition(f32 x, f32 y, f32 z, u32 apply) {
    listener_.position[0] = x, listener_.position[1] = y, listener_.position[2] = z;
    ChangedListener(0x1FF, apply);
}

void DirectSound::SetVelocity(f32 x, f32 y, f32 z, u32 apply) {
    listener_.velocity[0] = x, listener_.velocity[1] = y, listener_.velocity[2] = z;
    ChangedListener(0x80, apply);
}

void DirectSound::SetOrientation(f32 fx, f32 fy, f32 fz, f32 tx, f32 ty, f32 tz, u32 apply) {
    listener_.front[0] = fx, listener_.front[1] = fy, listener_.front[2] = fz;
    listener_.top[0] = tx, listener_.top[1] = ty, listener_.top[2] = tz;
    ChangedListener(0x466, apply);
}

void DirectSound::SetI3DL2Listener(const I3DL2Listener& l, u32 apply) {
    i3dl2_ = l;
    ChangedListener(0x300, apply);
}

// orig: 0x0024A997 (listener commit): right vector; on an I3DL2 change (0x200) effect 0's state is read back,
// converted (0x0024EAB2) and written to state +0x10 / +0x118.
void DirectSound::CommitListener() {
    ListenerRightVector(frame_, listenerDirty_, 0);
    if ((listenerDirty_ & 0x200) && haveReverbState_) {
        reverb_[0] = 0;
        std::memcpy(&reverb_[1], reverbState_.data(), sizeof(u32) * 70);
        ConvertI3DL2Reverb(i3dl2_, reverb_.data(), *tables_);
        reverb_[5] |= 4;                         // state +0x10 |= 4 (0x0024AA01)
        ++reverbVersion_;
    }
}

// orig: 0x00247AEB CDirectSound_CommitDeferredSettings: the listener, then every voice's 3D settings (the listener's
// dirty bits stay set during the voices' commits and are cleared afterwards)
void DirectSound::CommitDeferredSettings() {
    CommitListener();
    for (auto& [id, v] : voices_)
        if (v.is3D) CommitVoice(v);
    listenerDirty_ = 0;
}

// orig: 0x00247274 CDirectSoundVoice_CommitDeferredSettings: dirty |= the listener's bits, commit, dirty &= ~ the
// mask committed (0 for a voice that is not playing: its bits wait for its Play)
void DirectSound::CommitVoice(Voice& v) {
    v.dirty3D |= listenerDirty_;
    v.dirty3D &= ~Commit3D(v);
}

// orig: 0x0024D3F9 CMcpxVoiceClient_Commit3dSettings (+ the register update 0x0024D20D). Returns the mask it
// committed: 0 unless the voice is allocated and playing.
u32 DirectSound::Commit3D(Voice& v) {
    if (!v.playing) return 0;
    const u32 mask = v.dirty3D;
    if (mask == 0) return 0;                     // (the calculator runs no stage, no register changes)
    u32 i3dirty = 0;
    if (v.p3d.mode != 2) {
        v.src.dirty = 0;
        Calculate3D(0xFF, v.src, frame_, v.settings.flags, v.p3d, *tables_);
        if (mask & 0x100) {
            I3DL2SourceLevels(v.i3, i3dl2_, v.p3d.i3dl2, v.src.distance);
            i3dirty = v.i3.dirty;
        }
    }
    v.regs.hrtf0 = v.p3d.mode != 2 ? v.src.hrtf0 : kHrtfDefault;
    v.regs.hrtf1 = v.p3d.mode != 2 ? v.src.hrtf1 : kHrtfDefault;
    v.regs.azimuth = v.src.azimuth, v.regs.elevation = v.src.elevation;
    v.regs.directHFCoef = v.i3.directHFCoef, v.regs.roomHFCoef = v.i3.roomHFCoef;
    UpdateVolume(v);
    UpdatePitch(v);
    if (i3dirty & 0xC) SetFilter(v);             // a coefficient changed (0x0024D240)
    return mask;
}

// orig: 0x0024CC6F CMcpxVoiceClient_SetFilter(NULL) for a 3D voice: filter mode (MISC bits 16-17) 3 unless one is
// set, bits 18-20 kept; FCB = the I3DL2 coefficients (low: direct HF; high: room HF, or direct HF when slot 4 is
// the centre bin), 0 with DS3DMODE_DISABLE.
void DirectSound::SetFilter(Voice& v) {
    u32 mode = (v.regs.misc >> 16) & 3;
    const u32 q = (v.regs.misc >> 18) & 7;
    if (mode == 0) mode = 3;
    v.regs.misc = (v.regs.misc & 0xFFE0FFFFu) | ((q << 2 | mode) << 16);
    if (v.p3d.mode == 2) {
        v.regs.fcb = 0;
    } else {
        const u32 high = v.src.centreFlag ? v.i3.directHFCoef : v.i3.roomHFCoef;
        v.regs.fcb = high << 16 | (v.i3.directHFCoef & 0xFFFF);
    }
}

// ---- voices -------------------------------------------------------------------------------------------------------
void DirectSound::UpdateVolume(Voice& v) {      // orig: 0x0024D0C7 CMcpxVoiceClient_SetVolume
    VolumeRegisters(v.settings, v.p3d.mode, &v.src, v.i3.direct, v.i3.room, v.regs.hwVoices, v.regs.volume);
}

void DirectSound::UpdatePitch(Voice& v) {       // orig: 0x0024D15F CMcpxVoiceClient_SetPitch
    v.regs.pitch = PitchRegister(v.settings, v.p3d.mode, v.src.doppler, nullptr);
}

void DirectSound::UpdateMixBins(Voice& v) {     // orig: 0x0024D006 CMcpxVoiceClient_SetMixBins
    u32 b0 = 0, b1 = 0;
    MixBinRegisters(v.settings, &v.src, b0, b1, v.regs.slotBins);
    UpdateVolume(v);
}

// orig: 0x00248B03 settings ctor (headroom 600 unless 3D / mix-in / FX; volume = -headroom; format: pitch from
// the rate 0x00247B67; default mix bins 0x00247120 with lpMixBins NULL: 3D {6, 8, 7, 9, 10} (0x0026392C), mono /
// stereo {0, 1} (0x00250F64)); a 3D block from the DS3DBUFFER / I3DL2 defaults with dirty 0x1FF.
u32 DirectSound::CreateVoice(u32 flags, u16 channels, u32 sampleRate) {
    Voice v;
    v.settings.flags = flags;
    v.settings.headroom = (flags & 0x182000) || (flags & 0x10) ? 0 : 600;
    v.settings.volume = -v.settings.headroom;
    v.settings.sampleRate = sampleRate;
    v.settings.pitch = CalculatePitch(sampleRate);
    v.channels = channels;
    v.is3D = (flags & 0x10) != 0;
    v.regs.hwVoices = ((std::max<u32>(channels, 1) - 1) >> 1) + 1;   // orig: 0x0024C213
    const u32 id = next_++;
    Voice& nv = voices_.emplace(id, std::move(v)).first->second;
    static const MixBinVolume k3D[] = {{6, 0}, {8, 0}, {7, 0}, {9, 0}, {10, 0}};
    static const MixBinVolume k2D[] = {{0, 0}, {1, 0}};
    if (nv.is3D) SetMixBins(id, k3D, 5);
    else SetMixBins(id, k2D, 2);
    UpdatePitch(nv);
    return id;
}

void DirectSound::ReleaseVoice(u32 id) { voices_.erase(id); }

// orig: CMcpxBuffer_Play 0x0024E03E -> 0x0024D621 (returns at once when the voice already plays): the playing bit,
// then for a 3D voice Commit3dSettings with the voice's own dirty mask (not cleared: the next commit includes it)
void DirectSound::Play(u32 id) {
    Voice* v = Find(id);
    if (!v || v->playing) return;
    v->playing = true;
    if (v->is3D) Commit3D(*v);
}

// orig: CMcpxBuffer_Stop 0x0024E0EE (with the voice playing): the voice stops, its state bits leave 3
void DirectSound::Stop(u32 id) {
    if (Voice* v = Find(id)) v->playing = false;
}

bool DirectSound::Playing(u32 id) const {
    auto it = voices_.find(id);
    return it != voices_.end() && it->second.playing;
}

void DirectSound::SetVolume(u32 id, s32 volume) {   // orig: 0x0024723B CDirectSoundVoice_SetVolume
    Voice* v = Find(id);
    if (!v) return;
    v->settings.volume = volume - v->settings.headroom;
    UpdateVolume(*v);
}

void DirectSound::SetFrequency(u32 id, u32 hz) {    // orig: 0x00247CE2 (0 = the buffer's own rate)
    Voice* v = Find(id);
    if (!v) return;
    v->settings.pitch = CalculatePitch(hz ? hz : v->settings.sampleRate);
    UpdatePitch(*v);
}

void DirectSound::SetMixBins(u32 id, const MixBinVolume* bins, u32 count) {   // orig: 0x00247120 + 0x0024D006
    Voice* v = Find(id);
    if (!v) return;
    v->settings.binCount = 0;
    for (u32 i = 0; i < count && v->settings.binCount < 8; ++i) {
        const u8 bin = static_cast<u8>(bins[i].bin);
        v->settings.bins[v->settings.binCount] = bin;
        v->settings.binVolume[bin & 31] = bins[i].volume;
        ++v->settings.binCount;
    }
    UpdateMixBins(*v);
}

void DirectSound::Changed3D(Voice& v, u32 bits, u32 apply) {
    v.dirty3D |= bits;
    if (apply == 0) CommitVoice(v);              // orig: 0x00247274 (the listener's pending bits included)
}

void DirectSound::SetBufferPosition(u32 id, f32 x, f32 y, f32 z, u32 apply) {
    Voice* v = Find(id);
    if (!v || !v->is3D) return;
    v->p3d.position[0] = x, v->p3d.position[1] = y, v->p3d.position[2] = z;
    Changed3D(*v, 0x1FF, apply);
}

void DirectSound::SetBufferVelocity(u32 id, f32 x, f32 y, f32 z, u32 apply) {
    Voice* v = Find(id);
    if (!v || !v->is3D) return;
    v->p3d.velocity[0] = x, v->p3d.velocity[1] = y, v->p3d.velocity[2] = z;
    Changed3D(*v, 0x80, apply);
}

void DirectSound::SetMinDistance(u32 id, f32 d, u32 apply) {
    Voice* v = Find(id);
    if (!v || !v->is3D) return;
    v->p3d.minDistance = d;
    Changed3D(*v, 4, apply);
}

void DirectSound::SetMaxDistance(u32 id, f32 d, u32 apply) {
    Voice* v = Find(id);
    if (!v || !v->is3D) return;
    v->p3d.maxDistance = d;
    Changed3D(*v, 4, apply);
}

void DirectSound::SetRolloffCurve(u32 id, const f32* points, u32 count, u32 apply) {
    Voice* v = Find(id);
    if (!v || !v->is3D) return;
    v->curve.assign(points, points + (points ? count : 0));
    v->p3d.curve = points ? v->curve.data() : nullptr;
    v->p3d.curveCount = count;
    Changed3D(*v, 4, apply);
}

void DirectSound::SetI3DL2Source(u32 id, const I3DL2Source& s, u32 apply) {
    Voice* v = Find(id);
    if (!v || !v->is3D) return;
    v->p3d.i3dl2 = s;
    Changed3D(*v, 0x100, apply);
}

bool ReadReverbState(const std::vector<u8>& fx, u32 words[70]) {
    auto u32At = [&](size_t o) {
        u32 v = 0;
        if (o + 4 <= fx.size()) std::memcpy(&v, &fx[o], 4);
        return v;
    };
    if (fx.size() < 0x6200 || u32At(0x6110) == 0) return false;
    const u32 state = u32At(0x6118 + 8);         // DSEFFECTMAP[0].state (a file offset in this image)
    if (state + 70 * 4 > fx.size()) return false;
    std::memcpy(words, &fx[state], 70 * 4);
    return true;
}

}  // namespace fut::audio::xds

// The game-facing sound API; see GameSound.h.
#include "game/sound/GameSound.h"

#include "core/CrtString.h"
#include "game/sound/SoundSystem.h"

#include <cstdio>
#include <cstring>

namespace fut::sound {

// ---- XboxSound ----------------------------------------------------------------------------------------------------
XboxSound::XboxSound(SoundBackend* backend, ReadFile read)
    : system_(std::make_unique<SoundSystem>(backend)), read_(std::move(read)) {}

XboxSound::~XboxSound() = default;

Sample* XboxSound::Find(u32 h) const {
    const auto it = samples_.find(h);
    return it == samples_.end() ? nullptr : it->second;
}

u32 XboxSound::HandleOf(Sample* s) {
    if (!s) return 0;
    if (const auto it = handles_.find(s); it != handles_.end()) return it->second;   // (a de-duplicated load)
    const u32 h = next_++;
    samples_[h] = s;
    handles_[s] = h;
    return h;
}

void XboxSound::Forget(Sample* s) {
    if (const auto it = handles_.find(s); it != handles_.end()) {
        samples_.erase(it->second);
        handles_.erase(it);
    }
}

u32 XboxSound::Load(const std::string& path, u32 streaming) {   // orig: 0x000C9860 -> 0x000CCF70
    return HandleOf(system_->LoadSample(path, (streaming & 0xFF) != 0, read_));
}

u32 XboxSound::LoadCached(ni::NiUDSFileObject& file) {   // orig: 0x000C9840 -> 0x000CCED0 (no de-dup)
    return HandleOf(system_->LoadSampleFromMemory(file.name, file.data.data(), static_cast<u32>(file.data.size())));
}

void XboxSound::Release(u32 h) {                      // orig: 0x000C9880
    Sample* s = Find(h);
    if (!s) return;
    Forget(s);
    system_->ReleaseSample(s);
}

bool XboxSound::HasVoices(u32 h) { return system_->HasVoices(Find(h)); }
f32 XboxSound::Duration(u32 h) { return system_->Duration(Find(h)); }
s32 XboxSound::PlaySfx(u32 h, bool loop) { return system_->PlaySfx(Find(h), loop); }
s32 XboxSound::PlayMusic(u32 h, bool loop) { return system_->PlayMusic(Find(h), loop); }
s32 XboxSound::PlaySpeech(u32 h, bool loop) { return system_->PlaySpeech(Find(h), loop); }
s32 XboxSound::Play3D(u32 h, const ni::NiPoint3& pos, bool loop) { return system_->Play3D(Find(h), pos, loop); }
s32 XboxSound::PlayAttached(u32 h, ni::NiAVObject* node, bool loop) {
    return system_->PlayAttached(Find(h), node, loop);
}

s32 XboxSound::PlayAtTarget(u32 h, const char* name, bool loop, const std::function<bool(SoundTarget&)>& lookup) {
    (void)name;
    Sample* s = Find(h);                              // orig: 0x000CAAAE: the sample-list walk first
    if (!system_->Owns(s)) return 0;
    SoundTarget t;
    if (!lookup(t)) return 0;                         // 0x000CAAE9 FindOrCreate; 0x000CAAF6 node; 0x000DA7E0 position
    return system_->PlayAtTarget(s, t.record, t.node, t.position, loop);
}

bool XboxSound::VoiceExists(s32 id) { return system_->VoiceExists(id); }
void XboxSound::VoicePlay2D(s32 id, bool loop) { system_->VoicePlay2D(id, loop); }
void XboxSound::StopVoice(s32 id) { system_->StopVoice(id); }
void XboxSound::PauseVoice(s32 id) { system_->PauseVoice(id); }
void XboxSound::SetPitch(s32 id, f32 v) { system_->SetPitch(id, v); }
void XboxSound::SetPan(s32 id, f32 v) { system_->SetPan(id, v); }
f32 XboxSound::GetVolume(s32 id) { return system_->GetVolume(id); }
void XboxSound::SetVolume(s32 id, f32 v) { system_->SetVolume(id, v); }
void XboxSound::SetMaxDistance(s32 id, f32 v) { system_->SetMaxDistance(id, v); }
void XboxSound::SetMinDistance(s32 id, f32 v) { system_->SetMinDistance(id, v); }
void XboxSound::FadeOut(s32 id, f32 v) { system_->FadeOut(id, v); }
void XboxSound::StopAll() { system_->StopAll(); }
void XboxSound::StopSample(u32 h) { system_->StopSample(Find(h)); }
void XboxSound::StopAllOnTargetable(const void* record) { system_->StopAllOnTargetable(record); }
void XboxSound::StopAllOnTarget(const char* name, const std::function<const void*()>& lookup) {
    (void)name;
    if (const void* rec = lookup()) system_->StopAllOnTargetable(rec);   // orig: 0x000CAC90 / 0x000CACA5
}
void XboxSound::PauseAll() { system_->PauseAll(); }
void XboxSound::ResumeAll() { system_->ResumeAll(); }

void XboxSound::SetReverb(const char* name) {         // orig: 0x000C9770 (_stricmp chain, then vt+0x58)
    static const char* const kNames[5] = {"PADDED ROOM", "SMALL ROOM", "LARGE ROOM", "LARGE HALL", "OPERA"};
    for (u32 i = 0; i < 5; ++i)
        if (crt::StrICmp(name, kNames[i]) == 0) {
            system_->SetEnvironment(i);               // vt+0x58 0x000CC030
            return;
        }
}

void XboxSound::DeleteAllSamples() {                  // orig: vt+0x0C 0x000C96C0
    samples_.clear();
    handles_.clear();
    system_->DeleteAllSamples();
}

void XboxSound::SetListenerNode(ni::NiAVObject* node) { listenerNode_ = node; }
f32 XboxSound::CategoryVolume(u32 category) { return system_->CategoryVolume(category); }
void XboxSound::SetCategoryVolume(u32 category, f32 v) { system_->SetCategoryVolume(category, v); }
void XboxSound::Update(const ni::NiPoint3* cameraPos, const ni::NiMatrix3* cameraRotate) {
    system_->Update(cameraPos, cameraRotate);
}

}  // namespace fut::sound

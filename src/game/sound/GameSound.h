// The game-facing sound API: the cdecl wrappers 0x000C9840..0x000CAC80 the game code calls with the App's sound
// system (App+0xF0) and the system's vtable slots it calls directly (docs/analysis/sound_system.md 3.1,
// docs/analysis/level_sound_port.md 2.2). Every original call site calls one of these with the original's arguments
// and uses its answer as the original does; the service object decides what the answers are:
//
//  * XboxSound: the console. A handle table over one sound::SoundSystem (the layer, call-exact against the
//    sound-layer oracle) and its backend. The app uses it.
//
// Handles: the original passes Sample pointers; the port passes u32 handles, 0 = none. XboxSound never reuses a
// handle; a released / deleted sample's handle answers as a pointer the system does not hold (the wrappers' list
// walk fails). L: the original could see a freed sample's address reused by a later load; not modelled.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiObjects.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fut::sound {

class Sample;
class SoundSystem;
class SoundBackend;

// What FsPlay3DSFX 0x000CAAA0 reads of the trigger record TriggerManager::FindOrCreate gave: the record (the voice's
// owner, +0x04), its node (0x000DA5F0) and its position (0x000DA7E0).
struct SoundTarget {
    const void* record = nullptr;
    ni::NiAVObject* node = nullptr;
    ni::NiPoint3 position;
};

class GameSound {
public:
    virtual ~GameSound() = default;

    // ---- samples ----
    virtual u32 Load(const std::string& path, u32 streaming) = 0;   // orig: Sound_Load 0x000C9860 (vt+0x10)
    virtual u32 LoadCached(ni::NiUDSFileObject& file) = 0;            // orig: SoundCache_LoadFile 0x000C9840 (vt+0x14)
    virtual void Release(u32 sample) = 0;                             // orig: Sound_Release 0x000C9880
    virtual bool HasVoices(u32 sample) = 0;                           // orig: Sound_HasVoices 0x000C98C0
    virtual f32 Duration(u32 sample) = 0;                             // orig: Sound_GetDuration 0x000C9930
    // ---- plays (the voice id, 0 = none) ----
    virtual s32 PlaySfx(u32 sample, bool loop) = 0;                   // orig: Sound_PlaySFX 0x000C99A0 (category 0)
    virtual s32 PlayMusic(u32 sample, bool loop) = 0;                 // orig: Sound_PlayMusic 0x000C9A00 (category 2)
    virtual s32 PlaySpeech(u32 sample, bool loop) = 0;                // orig: Sound_PlaySpeech 0x000C9A60 (category 1)
    virtual s32 Play3D(u32 sample, const ni::NiPoint3& pos, bool loop) = 0;   // orig: Sound_Play3D 0x000C9AC0
    virtual s32 PlayAttached(u32 sample, ni::NiAVObject* node, bool loop) = 0;   // orig: Sound_PlayAttached 0x000CAB80
    // orig: FsPlay3DSFX 0x000CAAA0 (cdecl sample, name, loop): an owned sample only, **then**
    // TriggerManager::FindOrCreate(name) (`lookup`: false when it returns null); attached to the record's node or at
    // its position (the 8-voice limit), category 0, the voice's owner = the record. `name` is for the trace.
    virtual s32 PlayAtTarget(u32 sample, const char* name, bool loop,
                             const std::function<bool(SoundTarget&)>& lookup) = 0;
    // ---- voices ----
    virtual bool VoiceExists(s32 id) = 0;                             // orig: Voice_Exists 0x000C9900
    virtual void VoicePlay2D(s32 id, bool loop) = 0;                  // orig: Voice_Play2D 0x000C9970
    virtual void StopVoice(s32 id) = 0;                               // orig: Voice_Stop 0x000C9B10
    virtual void PauseVoice(s32 id) = 0;                              // orig: Voice_Pause 0x000C9BA0
    virtual void SetPitch(s32 id, f32 pitch) = 0;                     // orig: Voice_SetPitch 0x000C9BD0
    virtual void SetPan(s32 id, f32 pan) = 0;                         // orig: FsSetPanSound 0x000C9C00
    virtual f32 GetVolume(s32 id) = 0;                                // orig: Voice_GetVolume 0x000C9C30
    virtual void SetVolume(s32 id, f32 volume) = 0;                   // orig: Voice_SetVolume 0x000C9C70
    virtual void SetMaxDistance(s32 id, f32 d) = 0;                   // orig: Voice_SetMaxDistance 0x000C9CA0
    virtual void SetMinDistance(s32 id, f32 d) = 0;                   // orig: Voice_SetMinDistance 0x000C9CF0
    virtual void FadeOut(s32 id, f32 time) = 0;                       // orig: Voice_FadeOut 0x000C9D40
    // ---- groups ----
    virtual void StopAll() = 0;                                       // orig: FsStopAllSounds 0x000C9B40
    virtual void StopSample(u32 sample) = 0;                          // orig: FsStopSampleAll 0x000C9B70
    virtual void StopAllOnTargetable(const void* record) = 0;        // orig: SoundSystem 0x000CA690 (thiscall)
    // orig: FsStopAllOnTargetable 0x000CAC80: FindOrCreate(name) (`lookup`, unconditional), then 0x000CA690.
    virtual void StopAllOnTarget(const char* name, const std::function<const void*()>& lookup) = 0;
    virtual void PauseAll() = 0;                                      // orig: Sound_PauseAll 0x000CAFD0
    virtual void ResumeAll() = 0;                                     // orig: Sound_ResumeAll 0x000CAA80
    virtual void SetReverb(const char* name) = 0;                     // orig: FsSetReverb 0x000C9770 (-> vt+0x58)
    // ---- the system's slots ----
    virtual void DeleteAllSamples() = 0;                              // orig: vt+0x0C 0x000C96C0
    virtual void SetListenerNode(ni::NiAVObject* node) = 0;           // orig: vt+0x20 0x000CA590 (stored, not read)
    virtual f32 CategoryVolume(u32 category) = 0;                     // orig: vt+0x24 0x000C96E0
    virtual void SetCategoryVolume(u32 category, f32 v) = 0;          // orig: vt+0x28 / +0x30 / +0x38
    // orig: vt+4 Update 0x000CBD40 (App vf28 per fixed step): the listener from the level camera (position, world
    // rotation; null = the origin / identity), every voice's update, the commit, DoWork.
    virtual void Update(const ni::NiPoint3* cameraPos, const ni::NiMatrix3* cameraRotate) = 0;
};

// The console: the sound layer over its backend with a handle table.
class XboxSound : public GameSound {
public:
    using ReadFile = std::function<bool(const std::string& path, std::vector<u8>& out)>;
    // `backend` may be null (no DirectSound: every voice exists for ever, as the original after a failed
    // DirectSoundCreate; the app uses an offline backend instead).
    XboxSound(SoundBackend* backend, ReadFile read);
    ~XboxSound() override;

    u32 Load(const std::string& path, u32 streaming) override;
    u32 LoadCached(ni::NiUDSFileObject& file) override;
    void Release(u32 sample) override;
    bool HasVoices(u32 sample) override;
    f32 Duration(u32 sample) override;
    s32 PlaySfx(u32 sample, bool loop) override;
    s32 PlayMusic(u32 sample, bool loop) override;
    s32 PlaySpeech(u32 sample, bool loop) override;
    s32 Play3D(u32 sample, const ni::NiPoint3& pos, bool loop) override;
    s32 PlayAttached(u32 sample, ni::NiAVObject* node, bool loop) override;
    s32 PlayAtTarget(u32 sample, const char* name, bool loop, const std::function<bool(SoundTarget&)>& lookup) override;
    bool VoiceExists(s32 id) override;
    void VoicePlay2D(s32 id, bool loop) override;
    void StopVoice(s32 id) override;
    void PauseVoice(s32 id) override;
    void SetPitch(s32 id, f32 pitch) override;
    void SetPan(s32 id, f32 pan) override;
    f32 GetVolume(s32 id) override;
    void SetVolume(s32 id, f32 volume) override;
    void SetMaxDistance(s32 id, f32 d) override;
    void SetMinDistance(s32 id, f32 d) override;
    void FadeOut(s32 id, f32 time) override;
    void StopAll() override;
    void StopSample(u32 sample) override;
    void StopAllOnTargetable(const void* record) override;
    void StopAllOnTarget(const char* name, const std::function<const void*()>& lookup) override;
    void PauseAll() override;
    void ResumeAll() override;
    void SetReverb(const char* name) override;
    void DeleteAllSamples() override;
    void SetListenerNode(ni::NiAVObject* node) override;
    f32 CategoryVolume(u32 category) override;
    void SetCategoryVolume(u32 category, f32 v) override;
    void Update(const ni::NiPoint3* cameraPos, const ni::NiMatrix3* cameraRotate) override;

    SoundSystem& System() { return *system_; }
    // (tests) the first handle number: the real-sound play oracle's samples are pointers, which its comparison names
    // in order of appearance (0x04000000..0x07FFFFFF), so the test's handles start in that range
    void SetHandleBase(u32 first) { next_ = first; }
    Sample* Find(u32 handle) const;                   // null for 0, a released handle or a foreign number

private:
    u32 HandleOf(Sample* s);
    void Forget(Sample* s);
    std::unique_ptr<SoundSystem> system_;
    ReadFile read_;
    std::map<u32, Sample*> samples_;
    std::map<const Sample*, u32> handles_;
    u32 next_ = 1;
    ni::NiPointer<ni::NiAVObject> listenerNode_;      // orig: +0x04 (never read by Update)
};

}  // namespace fut::sound

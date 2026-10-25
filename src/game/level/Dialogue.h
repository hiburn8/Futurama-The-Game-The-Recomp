// The dialogues and the dialogue display (DAT_002EBDAC). Decompiled from default.xbe 0x000D7760-0x000D9A90
// (renderer_plan.md 19; docs/analysis/natives_fog_say_rw_port.md 3).
//
// A dialogue is a list of segments sorted by their start; a segment plays its tracks (a lip track: a key file on a
// character's lip-sync player; a sound track: a speech sample) from its start to its end. The builder (0x000D7FF0)
// makes the dialogue of a text id (FsCharacterSay, the IngameEvent lines, the idle blink); the display plays one
// dialogue at a time in its mode 2 (the only mode: the constructor's +8 = 2 is never written again): a new one
// replaces the current one only with a lower priority number, and a dialogue ends once its time passes its
// duration. The display's text object (+4, made with the display at the App start and drawn by App::Render
// 0x000D76C0) shows only the subtitle tracks of the cutscenes (0x000D98E0 from the RTM cutscenes' builder 0x000D3FD0
// -> 0x000D89B0): no in-game dialogue has one, so it holds no string and draws nothing.
//
// The sound tracks play their speech through the level's sound service (LevelData::Sound(), game/sound/GameSound.h).
// NOT PORTED (unreachable, KNOWN_ISSUES #64): the subtitle tracks (only the RTM cutscenes make them: FsPlayRTM 164 has
// no caller and every FsPrepareCutscene name is an .avi), the display modes 0 / 1.
#pragma once

#include "engine/ni/NiRefObject.h"
#include "game/text/TextObject.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fut::text {
class LanguageDb;
class TextManager;
}

namespace fut::game {

class LevelData;
class LipSyncPlayer;
struct LipSyncKeyFile;

// orig: the track base (vtable 0x002A44B8): vf04 Start, vf08 Stop, vf0C Pause, vf10 Update(dt), vf14 the length
// (0x000D9310: -1).
class DialogueTrack {
public:
    virtual ~DialogueTrack() = default;
    virtual void Start() {}
    virtual void Stop() {}
    virtual void Pause() {}
    virtual void Update(f32 dt) { (void)dt; }
    virtual f32 Length() const { return -1.0f; }
};

// orig: the lip track (0x10 bytes, vtable 0x002A44D4, ctor 0x000D9360, dtor 0x000D93C0): +8 the lip-sync player,
// +0xC the key file.
class LipTrack : public DialogueTrack {
public:
    LipTrack(LipSyncPlayer* p, LipSyncKeyFile* k);
    ~LipTrack() override;                             // orig: 0x000D93C0 (the player stopped; its key file cleared)
    void Start() override;                            // orig: 0x000D96F0
    void Stop() override;                             // orig: 0x000D92E0
    void Pause() override;                            // orig: 0x000D92F0
    void Update(f32 dt) override;                     // orig: 0x000D9690
    f32 Length() const override;                      // orig: 0x000D9720
    LipSyncPlayer* player = nullptr;                  // +0x08 (port: cleared by the player's destructor)
    ni::NiPointer<LipSyncKeyFile> keyFile;            // +0x0C
};

// orig: the sound track (0x14 bytes, vtable 0x002A44F0, ctor 0x000D9810, dtor 0x000D9840).
class SoundTrack : public DialogueTrack {
public:
    SoundTrack(const LevelData& level, u32 sample, bool owned) : level_(level), sample(sample), owned(owned) {}
    ~SoundTrack() override;
    void Start() override;                            // orig: 0x000D9750
    void Stop() override;                             // orig: 0x000D97B0
    void Pause() override;                            // orig: 0x000D97D0
    f32 Length() const override;                      // orig: 0x000D9800 (Sound_GetDuration)
    u32 sample = 0;                                   // +0x08
    s32 voice = 0;                                    // +0x0C
    bool owned = false;                               // +0x10 (released with the track)
    bool paused = false;                              // +0x11

private:
    const LevelData& level_;
};

// orig: a segment (0x1C bytes, ctor 0x000D9110(start, length), dtor 0x000D9140).
struct DialogueSegment {
    ~DialogueSegment();                               // orig: 0x000D9140 (the tracks deleted head first)
    std::vector<std::unique_ptr<DialogueTrack>> tracks;   // +0x00 (added at the head: 0x000D9180)
    f32 start = 0.0f;                                 // +0x0C
    f32 end = 0.0f;                                   // +0x10
    f32 time = 0.0f;                                  // +0x14
    bool playing = false;                             // +0x18
    bool paused = false;                              // +0x19
    void Advance(f32 dt);                             // orig: 0x000D9190
    double Stop();                                    // orig: 0x000D9250 (ST0: the time left, -1 when not playing)
    f32 MaxLength() const;                            // orig: 0x000D9290 (-1 without a track that has one)
};

// orig: a dialogue (0x20 bytes, NiRefObject, vtable 0x002A44B4, ctor 0x000D8B10, dtor 0x000D8BA0).
class Dialogue : public ni::NiRefObject {
public:
    ~Dialogue() override;                             // orig: 0x000D8BA0 (the segments deleted head first)
    std::vector<std::unique_ptr<DialogueSegment>> segments;   // +0x08 (by start: 0x000D8C30)
    f32 time = 0.0f;                                  // +0x10
    f32 duration = 0.0f;                              // +0x14
    s32 priority = 3;                                 // +0x18 (a lower number wins)
    // +0x1C, the stop callback (0x000D8B00: vf00(dialogue) at the end of Stop): the random lines' (0x000F0A80)
    std::function<void(Dialogue&)> onStop;
    void Insert(std::unique_ptr<DialogueSegment> s);  // orig: 0x000D8C30
    void Advance(f32 dt);                             // orig: 0x000D8CB0
    void Stop();                                      // orig: 0x000D8CF0
    void ComputeDuration();                           // orig: 0x000D8D20
    void Reset();                                     // orig: 0x000D8D70
};

// orig: the dialogue builder (0x24 bytes on the caller's stack, ctor 0x000D7FF0, dtor 0x000D8560): +0 the language
// database, +4 the lip-sync key files (S+0xE8), +0xC the dialogue sound map (S+0x100), +0x10 the dialogue, +0x14
// the open segment, +0x18 / +0x1C the lip / sound directories (both set to "" by every in-game caller).
class DialogueBuilder {
public:
    DialogueBuilder(const LevelData& level, const text::TextManager* text, const text::LanguageDb* db)
        : level_(level), text_(text), db_(db) {}
    void NewDialogue();                               // orig: 0x000D8200
    void NewSegment(f32 start);                       // orig: 0x000D8270 (closes an open one with -1 first)
    // orig: AddLip 0x000D8340 (the text's third string + ".key" from the level's key files, 0x000D8300 / 0x000D94A0)
    bool AddLip(LipSyncPlayer* player, const std::string& id);
    bool AddLipKeyFile(LipSyncPlayer* player, LipSyncKeyFile* key);   // orig: 0x000D8600
    // orig: AddSound 0x000D87B0 (the text's fourth string + ".wav" from the dialogue sound map)
    bool AddSound(const std::string& id);
    // orig: 0x000D86E0 (the file by value): SoundCache_LoadFile 0x000C9840 of a loaded file (the random lines' sounds
    // from the request loader 0x000F2710), an owned sound track (0x000D8430(handle, 1)); false without an open
    // segment or when the cache load fails.
    bool AddSoundFile(ni::NiUDSFileObject* file);
    void CloseSegment(f32 length);                    // orig: 0x000D8A00
    ni::NiPointer<Dialogue> Build();                  // orig: 0x000D8AB0 (null without a segment or a length)

private:
    const LevelData& level_;
    const text::TextManager* text_;
    const text::LanguageDb* db_;
    ni::NiPointer<Dialogue> dialogue_;                // +0x10
    std::unique_ptr<DialogueSegment> segment_;        // +0x14
};

// orig: the dialogue display DAT_002EBDAC (0xC bytes, ctor 0x000D7760 at the App start, dtor 0x000D7B60): +0 the
// current dialogue, +4 the subtitle text object, +8 the mode (2). The App's display has its text object (CreateText);
// a level made without the App makes its own display without one.
class DialogueDisplay {
public:
    DialogueDisplay() = default;
    ~DialogueDisplay();                               // orig: 0x000D7B60
    // orig: 0x000D7760's text object: TextObject::Create(App+0xFC, 0), SetScale(0.85, 1.1), +0x60 = +0x64 = 0,
    // SetZBufferTest(0), +0x68 = 0, 0x00098F80(App->vf14()).
    void CreateText(const text::TextObject::ReadFileFn& readFile, ni::NiRenderer* renderer);
    void Render();                                    // orig: 0x000D76C0 (App::Render's last call)
    bool Play(Dialogue* d);                           // orig: 0x000D7BE0
    void Update(f32 dt);                              // orig: 0x000D7800 (Session::Update step 9)
    void Release();                                   // orig: 0x000D7A10 (every dialogue let go, unstopped)
    void CancelAll();                                 // orig: FsCancelAllDialogues 0x000D7D70 (stopped, then let go)
    ni::NiPointer<Dialogue> current;                  // +0x00
    std::unique_ptr<text::TextObject> text;           // +0x04
    s32 mode = 2;                                     // +0x08
};

}  // namespace fut::game

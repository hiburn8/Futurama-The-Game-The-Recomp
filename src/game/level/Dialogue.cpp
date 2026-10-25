// The dialogues and the dialogue display; see Dialogue.h.
#include "game/level/Dialogue.h"

#include "core/CrtString.h"
#include "engine/ni/NiMath.h"
#include "game/defs/CharacterDef.h"
#include "game/level/LevelData.h"
#include "game/object/LipSync.h"
#include "game/sound/GameSound.h"
#include "game/text/LanguageDb.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <string>

namespace fut::game {

using ni::detail::F;

namespace {

void NotPorted(const char* what) {
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == what) return;
    seen.emplace_back(what);
    std::fprintf(stderr, "NOT PORTED reached: %s\n", what);
}

// The head-first deletion of the original's singly linked lists (0x000D9140 / 0x000D8BA0): a track's destructor can
// act on its player, so the order is kept.
template <class T>
void DeleteHeadFirst(std::vector<std::unique_ptr<T>>& v) {
    for (auto& p : v) p.reset();
    v.clear();
}

}  // namespace

// ---- the tracks ---------------------------------------------------------------------------------------------------
LipTrack::LipTrack(LipSyncPlayer* p, LipSyncKeyFile* k) : player(p), keyFile(k) {   // orig: 0x000D9360 / 0x000D95F0
    if (player) player->tracks.push_back(this);       // (port: see LipSyncPlayer::tracks)
}

LipTrack::~LipTrack() {                               // orig: 0x000D93C0
    if (!player) return;                              // (port: the player was deleted first)
    auto& v = player->tracks;
    v.erase(std::remove(v.begin(), v.end(), this), v.end());
    player->Stop();                                   // 0x0009D710
    if (player->keyFile.Get() == keyFile.Get()) player->SetKeyFile(nullptr);   // 0x000D9340; 0x0009D9A0(0)
}

void LipTrack::Start() {                              // orig: 0x000D96F0
    if (!player) return;
    player->SetKeyFile(keyFile.Get());                // 0x0009D9A0
    player->Play();                                   // 0x0009DA20 (state 0: "speaking")
}

void LipTrack::Stop() {                               // orig: 0x000D92E0
    if (player) player->Stop();
}

void LipTrack::Pause() {                              // orig: 0x000D92F0
    if (player) player->Pause();
}

void LipTrack::Update(f32 dt) {                       // orig: 0x000D9690
    if (!player || !keyFile) return;
    if (player->keyFile.Get() == keyFile.Get()) player->Update(dt);   // only while the player plays this key file
}

f32 LipTrack::Length() const { return keyFile ? keyFile->Duration() : 0.0f; }   // orig: 0x000D9720 -> 0x0009CA40

SoundTrack::~SoundTrack() {                           // orig: 0x000D9840
    sound::GameSound& snd = level_.Sound();
    if (snd.VoiceExists(voice)) snd.StopVoice(voice); // 0x000D986F
    if (owned) snd.Release(sample);                   // Sound_Release 0x000C9892 (AddSound's tracks are not owned)
}

void SoundTrack::Start() {                            // orig: 0x000D9750
    sound::GameSound& snd = level_.Sound();
    if (snd.VoiceExists(voice) && !paused) Stop();    // vf08 (a playing voice restarts)
    if (paused) {
        snd.VoicePlay2D(voice, false);                // Voice_Play2D 0x000D9795 (the paused voice resumes)
    } else {
        voice = snd.PlaySpeech(sample, false);        // Sound_PlaySpeech 0x000D977E
    }
    paused = false;
}

void SoundTrack::Stop() {                             // orig: 0x000D97B0
    level_.Sound().StopVoice(voice);                  // Voice_Stop 0x000C9B10
    voice = 0;
}

void SoundTrack::Pause() {                            // orig: 0x000D97D0 (only a live voice)
    if (!level_.Sound().VoiceExists(voice)) return;
    level_.Sound().PauseVoice(voice);                 // Voice_Pause 0x000C9BA0
    paused = true;
}

f32 SoundTrack::Length() const {                      // orig: 0x000D9800 (Sound_GetDuration 0x000C9930)
    return level_.soundDuration ? static_cast<f32>(level_.soundDuration(sample)) : 0.0f;
}

// ---- segments -----------------------------------------------------------------------------------------------------
DialogueSegment::~DialogueSegment() { DeleteHeadFirst(tracks); }   // orig: 0x000D9140

void DialogueSegment::Advance(f32 dt) {               // orig: 0x000D9190
    if (paused) return;                               // +0x19
    const double t = double(dt) + time;               // FLD dt; FADD +0x14; FST +0x14 (kept on the stack)
    time = F(t);
    if (!playing && !(t < start) && t < end) {        // TEST AH,1; JNE / TEST AH,5; JP
        playing = true;
        for (auto& k : tracks) k->Start();            // vf04, head first
    }
    if (!playing) return;
    for (auto& k : tracks) k->Update(dt);             // vf10
    if (playing && !(time < end)) {                   // FLD +0x14; FCOMP +0x10; TEST AH,1; JNE
        playing = false;
        for (auto& k : tracks) k->Stop();             // vf08
    }
}

double DialogueSegment::Stop() {                      // orig: 0x000D9250
    if (!playing) return -1.0f;                       // [0x00293BF0]
    playing = false;
    for (auto& k : tracks) k->Stop();                 // vf08
    return double(end) - time;                        // FLD +0x10; FSUB +0x14
}

f32 DialogueSegment::MaxLength() const {              // orig: 0x000D9290
    f32 m = -1.0f;                                    // 0xBF800000
    for (const auto& k : tracks)
        if (k->Length() > m) m = k->Length();         // vf14; FCOMP; TEST AH,0x41; JNE; vf14 again; FSTP
    return m;
}

// ---- dialogues ----------------------------------------------------------------------------------------------------
Dialogue::~Dialogue() { DeleteHeadFirst(segments); }   // orig: 0x000D8BA0

void Dialogue::Insert(std::unique_ptr<DialogueSegment> s) {   // orig: 0x000D8C30
    // before the head when the head starts later; else after the last segment that does not start later
    if (segments.empty() || segments.front()->start > s->start) {   // TEST AH,0x41; JE
        segments.insert(segments.begin(), std::move(s));
        return;
    }
    size_t i = 1;
    while (i < segments.size() && !(segments[i]->start > s->start)) ++i;
    segments.insert(segments.begin() + static_cast<std::ptrdiff_t>(i), std::move(s));
}

void Dialogue::Advance(f32 dt) {                      // orig: 0x000D8CB0
    for (auto& s : segments) s->Advance(dt);          // 0x000D9190
    time = F(double(dt) + time);                      // FADD +0x10; FSTP
}

void Dialogue::Stop() {                               // orig: 0x000D8CF0
    for (auto& s : segments) (void)s->Stop();         // 0x000D9250; FSTP ST(0)
    if (onStop) onStop(*this);                        // +0x1C->vf00(this)
}

void Dialogue::ComputeDuration() {                    // orig: 0x000D8D20
    double d = 0.0;                                   // FLD [0x00293AB8]
    for (const auto& s : segments) {
        const double e = (double(s->end) - s->start) + s->start;   // FSUB +0xC; FADD +0xC; FST [esp]
        if (e > d) d = F(e);                          // FCOMP ST(1); TEST AH,0x41; JNE; FLD [esp]
    }
    duration = F(d);                                  // FSTP +0x14
}

void Dialogue::Reset() {                              // orig: 0x000D8D70
    for (auto& s : segments) {
        if (s->playing) (void)s->Stop();              // 0x000D9250
        s->time = 0.0f;                               // +0x14
    }
    time = 0.0f;                                      // +0x10
}

// ---- the builder --------------------------------------------------------------------------------------------------
void DialogueBuilder::NewDialogue() {                 // orig: 0x000D8200
    dialogue_ = nullptr;                              // (an unbuilt one deleted: vf00(1))
    dialogue_ = new Dialogue;                         // new(0x20) 0x000D8B10 (priority 3)
}

void DialogueBuilder::NewSegment(f32 start) {         // orig: 0x000D8270
    if (!dialogue_) return;
    segment_.reset();                                 // (an open segment deleted: 0x000D9140, delete)
    segment_ = std::make_unique<DialogueSegment>();   // new(0x1C) 0x000D9110(start, 0)
    segment_->start = start;
    segment_->end = F(double(start) + 0.0f);          // FLD start; FADD length; FSTP +0x10
}

bool DialogueBuilder::AddLip(LipSyncPlayer* player, const std::string& id) {   // orig: 0x000D8340
    if (!dialogue_ || !segment_ || !text_ || !text_->Find(id, db_)) return false;
    const char* s3 = text_->GetStr3(id, db_);         // 0x000D9CF0
    if (!s3 || !*s3) return false;
    // "%s\%s.key" with a lip directory (0x002A44A0), "%s.key" without (0x002A4498): every caller sets ""
    const std::string path = std::string(s3) + ".key";
    // orig: 0x000D8300 -> 0x000D94A0(player, path, S+0xE8): new(0x10) LipTrack, new(0x2C) LipSyncKeyFile, the entry
    // of the list (0x000D8F10, _stricmp, head first) loaded (0x0009CBD0); without one (or a failed load) the track is
    // deleted (its destructor stops the player: 0x000D93C0)
    ni::NiPointer<LipSyncKeyFile> key(new LipSyncKeyFile);
    auto track = std::make_unique<LipTrack>(player, key.Get());
    bool ok = false;
    for (const auto& e : level_.lipSync)
        if (crt::StrICmp(e.name.c_str(), path.c_str()) == 0) {
            ok = e.file && key->Load(e.file->data);
            break;
        }
    if (!ok) return false;                            // (the track deleted here)
    segment_->tracks.insert(segment_->tracks.begin(), std::move(track));   // 0x000D9180 (at the head)
    return true;
}

bool DialogueBuilder::AddLipKeyFile(LipSyncPlayer* player, LipSyncKeyFile* key) {   // orig: 0x000D8600
    if (!dialogue_ || !segment_) return false;
    segment_->tracks.insert(segment_->tracks.begin(), std::make_unique<LipTrack>(player, key));   // 0x000D95F0
    return true;
}

bool DialogueBuilder::AddSound(const std::string& id) {   // orig: 0x000D87B0
    if (!dialogue_ || !segment_ || !text_ || !text_->Find(id, db_)) return false;
    const char* s4 = text_->GetStr4(id, db_);         // 0x000D9D10
    if (!s4 || !*s4) return false;
    // "%s\%s" with a sound directory (0x002A44AC), "%s" without (0x00296C7C), then ".wav" (0x002A4088); the map is
    // set, so a name it lacks gives no track (no Sound_Load / SoundCache_LoadFile)
    const auto it = level_.dialogueSounds.find(std::string(s4) + ".wav");   // 0x001077C0
    if (it == level_.dialogueSounds.end() || !it->second) return false;
    // orig: 0x000D8430(handle, 0): new(0x14) SoundTrack (not owned), at the head
    segment_->tracks.insert(segment_->tracks.begin(), std::make_unique<SoundTrack>(level_, it->second, false));
    return true;
}

bool DialogueBuilder::AddSoundFile(ni::NiUDSFileObject* file) {   // orig: 0x000D86E0
    ni::NiPointer<ni::NiUDSFileObject> keep(file);    // (the argument's reference, released at the end)
    if (!dialogue_ || !segment_ || !file) return false;
    const u32 h = level_.Sound().LoadCached(*file);   // SoundCache_LoadFile 0x000C9840
    if (!h) return false;
    // 0x000D8430(handle, 1): new(0x14) SoundTrack (owned: released with the track), at the head
    segment_->tracks.insert(segment_->tracks.begin(), std::make_unique<SoundTrack>(level_, h, true));
    return true;
}

void DialogueBuilder::CloseSegment(f32 length) {      // orig: 0x000D8A00
    if (!dialogue_ || !segment_) return;
    std::unique_ptr<DialogueSegment> s = std::move(segment_);   // (+0x14 = 0 at the end)
    if (s->tracks.empty()) return;                    // deleted
    if (length != -1.0f) {                            // [0x00293BF0]; TEST AH,0x44; JP
        if (!(length > 0.0f)) return;                 // TEST AH,0x41; JNP: deleted
        s->end = F(double(length) + s->start);        // FLD length; FADD +0xC; FSTP +0x10
    } else {
        const f32 d = s->MaxLength();                 // 0x000D9290
        if (d == -1.0f) return;                       // TEST AH,0x44; JP: deleted
        s->end = F(double(d) + s->start);
    }
    dialogue_->Insert(std::move(s));                  // 0x000D8C30
}

ni::NiPointer<Dialogue> DialogueBuilder::Build() {    // orig: 0x000D8AB0
    ni::NiPointer<Dialogue> d = std::move(dialogue_);   // +0x10 = 0
    if (!d) return nullptr;
    d->ComputeDuration();                             // 0x000D8D20
    if (d->segments.empty()) return nullptr;          // deleted (vf00(1))
    if (d->duration == 0.0f) return nullptr;          // TEST AH,0x44; JP: deleted
    return d;
}

// ---- the display --------------------------------------------------------------------------------------------------
bool DialogueDisplay::Play(Dialogue* d) {             // orig: 0x000D7BE0 (the dialogue by value: a reference)
    ni::NiPointer<Dialogue> keep(d);
    if (!d || d->segments.empty()) return false;      // +8
    if (mode != 2) {
        NotPorted("DialogueDisplay::Play: the queue modes 0 / 1");
        return false;
    }
    if (current) {
        if (!(d->priority < current->priority)) return false;   // CMP +0x18; JGE: equal does not interrupt
        current->Stop();                              // 0x000D8CF0
    }
    current = d;                                      // 0x00034250
    d->Reset();                                       // 0x000D8D70
    return true;
}

void DialogueDisplay::Update(f32 dt) {                // orig: 0x000D7800 (mode 2: 0x000D79AD)
    if (mode != 2) {
        NotPorted("DialogueDisplay::Update: the queue modes 0 / 1");
        return;
    }
    if (!current) return;
    current->Advance(dt);                             // 0x000D8CB0
    if (!(current->time > current->duration)) return;   // FLD +0x10; FCOMP +0x14; TEST AH,0x41; JNE
    current->Stop();                                  // 0x000D8CF0
    current = nullptr;                                // (the last reference: the dialogue and its tracks deleted)
}

void DialogueDisplay::Release() {                     // orig: 0x000D7A10
    // the queue (+0 and its +0xC links: always one dialogue in mode 2) let go, each link cleared (0x000D76E0)
    ni::NiPointer<Dialogue> d = std::move(current);
    if (text) text->RemoveAll();                      // 0x00099080 (+4; no string in game)
}

DialogueDisplay::~DialogueDisplay() {                 // orig: 0x000D7B60
    Release();                                        // 0x000D7A10
    if (text) {
        text->RemoveAll();                            // 0x00099080
        text->UpdateCamera();                         // 0x00098D70
    }
}                                                     // (+4 deleted: ~TextObject 0x00099730; +0 released)

void DialogueDisplay::CreateText(const text::TextObject::ReadFileFn& readFile, ni::NiRenderer* renderer) {
    // orig: 0x000D7760 (+0 = 0 first; the original uses the result without a test)
    text.reset(text::TextObject::Create(0, readFile, renderer));   // 0x00098AD0(App+0xFC, 0)
    if (!text) return;                                // (the port's Create fails without the font files)
    text->SetScale(std::bit_cast<f32>(0x3F59999Au), std::bit_cast<f32>(0x3F8CCCCDu));   // 0x00098C00
    text->style.hAlign = 0;                           // +0x60
    text->style.vAlign = 0;                           // +0x64
    text->SetZBufferTest(false);                      // 0x00099660(0)
    text->style.wordWrap = false;                     // +0x68
    text->SetRenderer(renderer);                      // 0x00098F80(App->vf14())
}

void DialogueDisplay::Render() {                      // orig: 0x000D76C0
    if (!text) return;                                // (the original's +4 is never null)
    text->UpdateCamera();                            // 0x00098D70
    text->Click();                                    // 0x00098DB0 (no string: nothing drawn)
}

void DialogueDisplay::CancelAll() {                   // orig: FsCancelAllDialogues 0x000D7D70
    if (current) current->Stop();                     // 0x000D8CF0 for each of the queue
    Release();                                        // 0x000D7A10
}

}  // namespace fut::game

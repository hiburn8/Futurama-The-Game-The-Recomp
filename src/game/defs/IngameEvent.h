// IngameEvent: a character definition's in-game remarks ("IngameEvent v0.30" data file of the .ucf), parsed by the
// first instance's Init (0x000F2A80 on CharacterDef+0x28) and picked from at load for the shuffled (type 2) and
// indexed (type 3) events. Decompiled from default.xbe 0x000F0010-0x000F2A80 with the dialogue builder
// 0x000D7FF0-0x000D8D20; docs/analysis/player_spawn_deps.md 4 and 12.
//
// Ported: the file parser (events, conditions, lines), the level filter, the database lookups (including the
// fallback walk over the level's database map in the original's bucket order), the dialogue-line build rule (a lip
// track from the level's lip-sync key files and / or a sound track from the dialogue sounds; kept when the longest
// track is longer than 0), the live line counter that frees the default database name, Refill / Pick / Select with
// the CRT rand(), the random lines' sound check ("Sounds\Localized\<language>\<sound>.wav") and the sound-request
// queue, the trigger (IngameEventSet::Trigger 0x000F28F0: the condition test of types 1 / 2 and playing the events;
// docs/analysis/combat_ai_port.md 7), the lines' dialogues (Dialogue.h: built with the builder, played by the dialogue
// display 0x000D7BE0; a random line's dialogue 0x000F08F0 (lip-only at once, or with its sound file once the request
// loader 0x000F2710 has read it: StepSoundRequests) with its stop callback 0x000F0A80), the hint lines.
#pragma once

#include "core/Types.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fut::text {
class LanguageDb;
class TextManager;
}

namespace fut::game {

class LevelData;
class LipSyncPlayer;
class IngameEventLine;
class Character;
class GameObject;

// What the parser and the selection read from the level (orig: the global level object and singletons).
struct IngameEventContext {
    const LevelData* level = nullptr;                 // name, databases, lip-sync files, dialogue sounds, nibblers
    const text::TextManager* text = nullptr;
    // orig: NiUDSStream(path) good (0x000F2250: an archive entry or a file of the disc).
    std::function<bool(const std::string& path)> fileExists;
    // orig: Sound_GetDuration 0x000C9930 (0 for a handle the sound system does not hold).
    std::function<double(u32 handle)> soundDuration;
    // orig: the clock 0x000D2C20 (QueryPerformanceCounter ticks; the frequency from QueryPerformanceFrequency, read
    // once): the type-0 conditions. Without a counter the clock reads 0.
    std::function<u64()> queryCounter;
    u64 counterFrequency = 1000000;
};

struct IngameEventCondition {                         // orig: 0x18 / 0x10 / 0xC-byte objects by type
    u32 type = 0;                                     // 0..4 (the map key, vf0C)
    u32 value = 0;                                    // types 0 / 1: the raw u32 (+0x08; type 1 reads it as a float)
    std::optional<std::string> text;                  // types 2 / 3 / 4
    f32 count = 0;                                    // type 1: +0x0C, the calls so far (0 at the ctor 0x000F1970)
    u64 stamp = 0;                                    // type 0: +0x10, the clock at the last play (0 at the ctor
                                                      //  0x000F1A40; vf08 0x000F18E0 stamps it)
};

// orig: the 8-byte trigger argument on the caller's stack (0x000984D0 clears it, 0x000D8150 stores the character,
// 0x000F0C00 clears it again).
struct IngameEventArg {
    // +0 (conditions 2 / 3: 0x000F0C10): the character, or the picked-up object (Player::OnPickup's category 8)
    GameObject* character = nullptr;
    GameObject* object = nullptr;                     // +4 (condition 4: 0x000F0C20; a player's weapon)
};

class IngameEvent {                                   // orig: 0x64 bytes, ctor 0x000F10C0, vtbl 0x002A598C
public:
    ~IngameEvent();
    u32 id = 0xFFFFFFFF;                              // orig: +0x40
    bool random = false;                              // orig: +0x60
    u32 type = 1;                                     // orig: +0x1C (0 all, 1 random each time, 2 shuffled, 3 indexed)
    std::map<u32, std::vector<IngameEventCondition>> conditions;   // orig: map +0x30 by type, chained
    u32 conditionCount = 0;                           // orig: +0x28
    std::vector<std::unique_ptr<IngameEventLine>> lines;   // orig: +0x04
    s32 last = -1;                                    // orig: +0x20
    s32 current = 0;                                  // orig: +0x24
    std::vector<s32> pool;                            // orig: +0x44 (values; a 0 counts as "null")
    u32 left = 0;                                     // orig: +0x5C
    const IngameEventArg* arg = nullptr;              // orig: +0x2C (the trigger argument of the last Test)
    const IngameEventContext* context = nullptr;      // (the port's: the clock of the type-0 conditions)

    bool Test(const IngameEventArg& ev, const IngameEventContext& ctx);   // orig: 0x000F1250
    bool TestCond(u32 type, s32 n);                   // orig: 0x000F1000 (thiscall(type, n), RET 8)
    void Play(const IngameEventContext& ctx);         // orig: 0x000F16C0
    // orig: 0x000F17D0 (type 3 only): the current line's vf10 (a random line drops its dialogue and sound requests),
    // then Select 0x000F15C0 again (with the level's restored nibbler count).
    void ResetIndexed(const IngameEventContext& ctx);
};

class IngameEventSet {                                // orig: CharacterDef+0x28 (ctor 0x000F28B0, vtbl 0x00296D0C)
public:
    // orig: 0x000F2A80. False (nothing built) when events exist already, there is no file or the header differs.
    // lipPlayer: the parsing character's lip-sync player (players only; their lines get lip tracks).
    bool Parse(const std::vector<u8>* file, LipSyncPlayer* lipPlayer, const IngameEventContext& ctx);
    // orig: 0x000F28F0 (thiscall(kind, IngameEventArg*), RET 8): every event of `kind` whose conditions pass plays;
    // kind 9 on a level whose name starts with the case-sensitive "Level3" plays only when rand() % 3 == 0.
    void Trigger(u32 kind, const IngameEventArg& ev, const IngameEventContext& ctx);
    // orig: 0x000F2980 (FsQuickLoad): ResetIndexed on every type-3 event.
    void ResetIndexed(const IngameEventContext& ctx);
    std::vector<std::unique_ptr<IngameEvent>> events; // orig: +0x04
    bool enabled = true;                              // orig: +0x1C
};

// orig: the sound-request loader DAT_002F0680 (ring buffer): {path, requesting line} per valid random-line pick.
struct IngameSoundRequest {
    std::string path;
    IngameEventLine* line = nullptr;                  // (+4: the line's callback interface +0xC)
};
std::vector<IngameSoundRequest>& IngameSoundRequests();
// orig: SoundRequestLoader::Step 0x000F2710 (App::OnIdle, every frame; the singleton 0x000F20E0): nothing without a
// level or while the Session's state is 2; in state 4 only when every stage is loaded (Stage::IsLoaded 0x000CE400).
// With no stream open and a request queued, the front request's file is opened (NiUDSStream(path, loader), +0x20 =
// 1) and its job (0x000F2030: the whole file read) runs: `stream` answers the job (false = it never completes: the
// oracle runner's stream stub, whose jobs never run; the loader then pumps the stream every frame for ever). A
// completed read (0x000F2330, in the same step): a NiUDSFileObject of the bytes named by the path goes to the
// requesting line (its vf of +0xC: the random line's 0x000F08F0, the dialogue with the sound), the stream is
// released, the request deleted.
void StepSoundRequests(const LevelData* level, s32 sessionState,
                       const std::function<bool(const std::string& path, std::vector<u8>& out)>& stream);
// (tests) the loader as a new process has it: no request, no stream (the singleton 0x000F20E0 not made yet).
void ResetSoundRequests();
// orig: 0x000C33F0 (upper-cased exact lookup in the level's database map; names over 127 characters fail).
const text::LanguageDb* FindLanguageDb(const LevelData& level, const std::string& name);
// orig: the S+0xEC walk (0x00034290 / 0x00155B60): the first database (bucket order, newest key first) that has `id`.
const text::LanguageDb* FindLanguageDbWithId(const LevelData& level, const text::TextManager& tm, const std::string& id);
// orig: DAT_002EFEF4, the live count of dialogue / random lines (at 0 the default database name is freed).
s32 IngameLineCount();

}  // namespace fut::game

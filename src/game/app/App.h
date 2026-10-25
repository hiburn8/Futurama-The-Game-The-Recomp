// The App: the game's stream manager and its context machine (orig: main 0x000D2A40, the App class ctor 0x000A22D0 /
// game part, vtable 0x002A3F98: Init vf08 0x000D2730 + 0x000A2730, Frame vf0C 0x000D26C0, OnIdle 0x000A2CB0, the per-
// step services vf28 0x000D2660, PlayMovie vf18 0x000A2410, IsMoviePlaying vf1C 0x000A2550, the movie pump vf20
// 0x000A2530). docs/analysis/frontend_port.md 1.1-1.3, 2.1.
//
// The App owns what outlives a level: the input system and pads (Input_Init runs once), the Intermission (its view
// table is static), the stream manager (the App is the manager), the memory-card manager App+0xF4, the pause menu
// DAT_002E9428, the level records App+0xA4, the requested level name App+0xAC, the carry-over texts DAT_002EAA28..39
// and the contexts registered in construction order: Intro, Frontend, Attract, Ingame, Death (the first one is current
// at boot). A context leaves when its update returns false; OnIdle then runs its exit, the renderer flush, the next
// context's enter, an extra Input_Update and the new context's first update in the same fixed step.
//
// The clock is a tick counter (QueryPerformanceCounter / Frequency): the driver sets `qpc` before every App frame
// (the boot oracle's runner: (f + 1) * 16,667 ticks of 1 MHz during App frame f, constant within the frame; the
// original reads the counter at each call, which on hardware advances within a frame: L for the real-time app).
//
// The step services vf28 run the pads' device changes (0x0009C1D0, AppServices::deviceChanges), the input update and
// the sound system's vf04 Update 0x000CBD40 (the level camera's listener; vf08 is a nop) every fixed step. NOT
// PORTED: the per-switch sorter (render), the fps counter's display. The sound-request loader 0x000F2710 reads its
// files through AppServices::streamFile.
// The movie player (App+0xE4, game/app/MoviePlayer.h) runs on AppServices::movies (the app's plays them).
#pragma once

#include "core/Types.h"
#include "engine/stream/StreamManager.h"
#include "game/app/CheatCodes.h"
#include "game/app/MoviePlayer.h"
#include "game/app/Options.h"
#include "game/input/Pad.h"
#include "game/level/Intermission.h"
#include "game/level/LevelData.h"
#include "game/sound/GameSound.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fut::ni {
class NiRenderer;
}
namespace fut::text {
class TextManager;
}

namespace fut::game {
struct LevelLoadOptions;
struct LoadedLevel;
}  // namespace fut::game

namespace fut::game::app {

class App;
class IntroContext;
class FrontendContext;
class AttractContext;
class IngameContext;
class DeathContext;
class MemoryCards;
class PauseMenu;
class Breakdown;
struct ErrorOverlay;
struct ExtrasData;
class SaveStorage;

// The host services the App runs on.
struct AppServices {
    // A file of the game's file system ("menu\frontend.nif"); false when it does not exist.
    std::function<bool(const std::string& path, std::vector<u8>& out)> readFile;
    // XInputGetState for a port (nullptr = not connected); unset = a neutral pad on port 0.
    std::function<const input::GamepadState*(u32 port)> readPad;
    // NiUDSStream's job for the sound-request loader (0x000F2710: the random lines' sound files, read whole); false =
    // the job never completes (unset: the oracle runner's stream stub, whose jobs never run).
    std::function<bool(const std::string& path, std::vector<u8>& out)> streamFile;
    // XGetDeviceChanges(gamepad) once per fixed step (Input_HandleDeviceChanges 0x0009C1D0): the inserted / removed
    // ports as bit masks, false = no change; unset = none (the pads never change).
    std::function<bool(u32& insertions, u32& removals)> deviceChanges;
    // orig: the sound system App+0xF0 (XboxSoundSystem, App::Init 0x000D2730): every game-facing sound call of the
    // App and the levels it hosts (game/sound/GameSound.h); the app passes an XboxSound.
    sound::GameSound* sound = nullptr;
    ni::NiRenderer* renderer = nullptr;               // orig: App+0x104 (null: no drawing)
    SaveStorage* saves = nullptr;                     // the U:\ save games (MemoryCards.h); null = an empty card
    u64 qpf = 1000000;                                // QueryPerformanceFrequency
    // The movie player's platform (game/app/MoviePlayer.h); the app passes its MovieService.
    MovieService* movies = nullptr;
};

// orig: the context base (vtable 0x00295420; ctor 0x0002C320(name): +4 a heap copy of the name, appended to the
// App's list App+0xB8, the first becomes current App+0xBC). Slots: +4 Enter, +8 Update (false = leave), +0xC Render,
// +0x10 Exit.
class Context {
public:
    Context(App& app, const char* name);
    virtual ~Context() = default;
    bool Enter();                                     // vf04 (traced: CTX <f> enter <name>)
    virtual bool Update() = 0;                        // vf08
    virtual void Render() = 0;                        // vf0C
    void Exit();                                      // vf10 (traced: CTX <f> exit <name>)
    const std::string name;                           // orig: +4

protected:
    virtual bool OnEnter() = 0;
    virtual void OnExit() = 0;
    App& app_;
};

// orig: App+0xA4, new(0xC0) in the App ctor: 24 x {u8 allNibblers, s32 bestTime} (reset 0x000F3BF0: 0 / -1).
struct LevelRecords {
    struct Record {
        u8 allNibblers = 0;
        s32 bestTime = -1;
    };
    std::array<Record, 24> records{};
    static int Index(const char* name);               // orig: 0x000F39D0
    bool Flag(const char* name) const;                // orig: 0x000F3B10 ("always" = 1)
    void SetFlag(const char* name, bool on);          // orig: 0x000F3B40
    s32 BestTime(const char* name) const;             // orig: 0x000F3B60 (-1 none)
    void SetBestTime(const char* name, s32 t);        // orig: 0x000F3B90
};

class App {
public:
    explicit App(AppServices services);
    ~App();
    // orig: the App ctor 0x000A22D0 / 0x000D2A40 and Init vf08 (0x000D2730 + 0x000A2730), the parts ported: Input_Init,
    // the memory-card manager, the contexts, the current one's Enter, the step clocks. NTSC (App+0xE8 = 1).
    // `directLevel`: the oracle runner's direct start (play_oracle.py, boot_oracle.py --level): App+0xAC = the level,
    // the Ingame context current and entered instead of the Intro.
    bool Init(const char* directLevel = nullptr);
    // orig: vf0C 0x000D26C0 (false: the App quits).
    bool Frame();
    // orig: vf10 0x000D29B0 (the current context's exit; the rest is the subsystems' teardown).
    void Shutdown();
    // orig: 0x0002A660 (OnIdle's context switch, Ingame::LoadLevel): the renderer's clear colour black and eight
    // clears + presents (2, the state caches' release, 3, 3), the sorter released.
    void FlushRenderer();

    // ---- the clock (orig: 0x000D2C20 QueryCounter, 0x000D2C10 Seconds, 0x000D2BA0 / 0x000D2BE0 Ticks) ----------
    u64 qpc = 0;                                      // QueryPerformanceCounter (set by the driver per frame)
    s32 frame = -1;                                   // (port) the App frame being run (-1 = Init), for the trace
    // orig: 0x000D2C10: FILD qword ticks; FMUL dword F(1 / QPF) (0x002EB4F0, set by the first 0x000D2C20).
    double Seconds(s64 ticks) const { return double(ticks) * double(invQpf_); }
    // orig: 0x000D2BE0: _ftol(FILD QPF * seconds) (truncation of the extended product).
    s64 Ticks(f32 seconds) const { return static_cast<s64>(double(qpf_) * double(seconds)); }

    // ---- contexts --------------------------------------------------------------------------------------------
    Context* FindContext(const char* name) const;     // orig: 0x000A3120 (_stricmp in registration order)
    // orig: 0x000A3160: App+0xC0 = the named context (false, no change, when unknown). `site` = the original's return
    // address (the trace's "from").
    bool RequestContext(const char* name, u32 site);
    Context* Current() const { return current_; }     // orig: App+0xBC
    Context* Requested() const { return next_; }      // orig: App+0xC0
    // orig: 0x000A23D0(name): App+0xAC cleared and strncpy 8; name == "" clears it (NULL only reads it).
    void SetLevelName(const char* name);
    // orig: FsChangeLevel's App part (native 161 0x000B05B0 from 0x000B05C6): the level record of the finished
    // level, App+0xAC = next, the end-of-level menu made (new(0x94) 0x000A4E70, once) and opened. `seconds` = the
    // Session's play time (its _ftol(S+0x180 + 0.5) + S+0x184 * 3600).
    void EndLevel(const LevelData& level, const char* next, s32 seconds);
    const char* LevelName() const { return levelName_; }   // orig: 0x000A23D0(0)

    // ---- movies (orig: vf18 PlayMovie 0x000A2410, vf1C IsMoviePlaying 0x000A2550, vf20 0x000A2530) --------------
    // PlayMovie(name): the old player deleted; NULL resumes the sounds; else a new player (0x000EE0E0) that fails
    // without a D3D device (AppServices::movies' Device: the emulator's case, no sound pause), or Sound_PauseAll and
    // its Open. IsMoviePlaying: a finished player is deleted (its close presents one black frame), Mem_CoalesceAll,
    // Sound_ResumeAll. `site` as RequestContext (the MOVIE trace line).
    bool PlayMovie(const char* name, u32 site);
    bool IsMoviePlaying();
    void MoviePump();                                 // orig: vf20 0x000A2530 (if IsMoviePlaying: the pump)
    std::unique_ptr<MoviePlayer> movie;               // orig: App+0xE4

    // orig: Input_GetActivePad 0x0009B720 (the active pad, else the first).
    input::Pad* ActivePad() const { return input.activePad ? input.activePad : input.pads[0]; }
    // orig: the sound system App+0xF0 (AppServices::sound or the App's fallback).
    sound::GameSound& Sound() const { return *sound_; }
    // orig: Sound_Load 0x000C9860 for the App's menus.
    u32 LoadSound(const std::string& path, u32 flags) const { return sound_->Load(path, flags); }
    // orig: g_level DAT_002DAC64: the level the Ingame context hosts (null without one).
    const LevelData* CurrentLevel() const;
    // orig: 0x0016694F srand (the front end's reseed), traced as the oracle's SRAND line.
    void Srand(u32 seed, u32 site);

    AppServices services;
    input::InputSystem input;                         // orig: Input_Init 0x0009C330 (globals 0x002E7024..)
    std::array<std::unique_ptr<input::Pad>, 4> pads;
    Intermission intermission;                        // orig: 0x000D4E50 (made by App::Init)
    // orig: the dialogue display DAT_002EBDAC (new(0xC) 0x000D7760 at 0x000A2B4D; game/level/Dialogue.h): every
    // level the App loads uses it; App::Render draws its text object (0x000D76C0).
    std::shared_ptr<DialogueDisplay> dialogue;
    CheatRecorder cheats;                             // orig: DAT_002E7C1C (new(0x38) 0x0002C440 at App init)
    stream::Manager streams;                          // orig: the App's NiStream part (App+0xA0 the request list)
    std::unique_ptr<text::TextManager> text;          // orig: the TextManager singleton DAT_002EC6E0
    std::unique_ptr<ErrorOverlay> errorOverlay;       // orig: 0x000F3470's globals (DAT_002F0940..)
    std::unique_ptr<MemoryCards> memoryCards;         // orig: App+0xF4 (0x000E4B30)
    std::unique_ptr<PauseMenu> pause;                 // orig: DAT_002E9428 (built by the Intro, persistent)
    std::unique_ptr<Breakdown> breakdown;             // orig: DAT_002E8EEC (the end-of-level menu, persistent)
    std::unique_ptr<ExtrasData> extras;               // orig: the extras' process globals (game/app/Extras.h)
    LevelRecords records;                             // orig: App+0xA4
    LevelTargets levelTargets;                        // orig: App+0xA8 (0x000A2290)
    Options options;                                  // orig: the options globals DAT_002E79A0.. (game/app/Options.h)
    // orig: D3DDevice_SetScreenSpaceOffset 0x00235590's last arguments (render: the renderer does not apply them)
    std::array<f32, 2> screenOffset{};
    bool useMemoryCardFlows = true;                   // orig: App+0xF8 (main: 1; the menus' Load / Apply flows)
    // orig: DAT_002EAA28 / 2C (the player-state text the next load check applies), DAT_002EAA30 / 34 (its level-start
    // copy, for Replay), DAT_002EAA38 (keep: the pause's load), DAT_002EAA39 (replay): the levels the App hosts use
    // this one (LevelData::carry).
    CarryOver carry;
    s32 lives = 5;                                    // orig: DAT_002D687C (5 in the image; LevelData::lives in a level)
    bool ntsc = true;                                 // orig: App+0xE8 (sysNTSC.img mounted)
    f32 lastRenderDt = 0;                             // orig: DAT_002E7C28
    u32 lastSteps = 0;                                // (port) the fixed steps of the last frame (STEPS lines)
    IntroContext* intro = nullptr;                    // orig: DAT_002DAE30
    FrontendContext* frontend = nullptr;
    AttractContext* attract = nullptr;                // orig: DAT_002DAF58
    IngameContext* ingame = nullptr;                  // orig: DAT_002DAC60
    DeathContext* death = nullptr;                    // orig: DAT_002DB1A4

private:
    bool OnIdle();                                    // orig: 0x000A2CB0
    void StepServices();                              // orig: vf28 0x000D2660
    sound::GameSound* sound_ = nullptr;               // orig: App+0xF0
    std::vector<std::unique_ptr<Context>> contexts_;  // orig: App+0xB8 list (registration order)
    Context* current_ = nullptr;                      // orig: App+0xBC
    Context* next_ = nullptr;                         // orig: App+0xC0
    char levelName_[9] = {};                          // orig: App+0xAC..+0xB4
    u64 qpf_ = 1000000;
    f32 invQpf_ = 1e-6f;                              // orig: DAT_002EB4F0 = F(1.0f / FILD QPF)
    u64 stepPosition_ = 0;                            // orig: App+0xC8
    u64 fpsStart_ = 0;                                // orig: App+0xD8
    s32 fpsCount_ = 0;                                // orig: App+0xD0
    f32 fps_ = 0;                                     // orig: App+0xE0
    bool renderClockSet_ = false;                     // orig: the guard of DAT_002E7DF0
    u64 lastRender_ = 0;                              // orig: DAT_002E7DF0
};

}  // namespace fut::game::app

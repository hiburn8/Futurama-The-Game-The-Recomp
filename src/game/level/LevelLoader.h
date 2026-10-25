// A level load as the game runs it: Session::Start queues the Session's stream chain (Session.h) on a stream manager
// (engine/stream/StreamManager.h), whose requests (the Session's ten kinds, the explosion meshes, the characters'
// .ucf files, the stages, the player's view files) load one file at a time in creation order; either the manager
// alone runs until every request has finished (the stage oracle's level_runner.run_level), or per frame one manager
// step and the Session update with the neighbour-stage preloading run until the level counts as loaded (the game's
// flow). Verified by tests/game/StageTests.cpp (the built level) and tests/game/StreamOrderTests.cpp (the request
// events, step by step).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiStream.h"
#include "engine/stream/StreamManager.h"
#include "game/level/Intermission.h"
#include "game/input/Pad.h"
#include "game/defs/Defs.h"
#include "game/level/LevelAnimation.h"
#include "game/level/LevelData.h"
#include "game/sound/GameSound.h"
#include "game/text/LanguageDb.h"
#include "engine/ni/NiRenderer.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fut::game {

class Session;

struct LevelLoadOptions {
    // A file of the disc by its game path ("Levels\level1-1\defs.nif"); false when it does not exist.
    std::function<bool(const std::string& path, std::vector<u8>& out)> readFile;
    DefContext defContext;                            // sound loader / rand for the definitions (no-ops by default)
    // The dialogue stream's services (kind 10): the text manager (a new one reading through readFile when unset)
    // and the sound cache.
    LevelServices services;
    // Viewer convenience: also create and load the stages that the stage files' LinkEnd markers chain to (in the
    // game these stream in while playing: Stage streaming 0x000C70C0, not ported).
    bool followStageLinks = false;
    // false: run the stream manager until every request has finished, without the level object's update (the
    // stage oracle's level_runner.run_level); true: the game's flow (the sound events' table, the Session's update in
    // the App's frames: only with `host`, the App's Ingame context; LoadLevel only starts the Session).
    bool ingame = false;
    u32 maxSteps = 100000;
    std::vector<std::string>* streamLog = nullptr;    // stream request events (stream_order_oracle.py lines)
    std::function<void(ni::NiStream& file)> soundLookup;   // kind 8 (Sounds\SoundLookup.nif), when set
    // The gamepad state XInputGetState would return for a port (nullptr = not connected), read once per App frame;
    // unset = a neutral pad on port 0.
    std::function<const input::GamepadState*(u32 port)> readPad;
    // The movie player of the Intermission (App::PlayMovie 0x000A2410 / IsMoviePlaying 0x000A2550); unset = no movie,
    // as under the oracle's emulator (the AVI cutscene lasts one step; PlayMovie(NULL) still resumes the sounds).
    std::function<bool(const char* name)> playMovie;
    std::function<bool()> isMoviePlaying;
    // The App's renderer (App +0x104, App->vf14(): the NiCamera +0x1A8 of the cameras Session::Start makes, the
    // clear / display of App::Frame); null = the play oracle's fake renderer (no display, the cull counters only).
    ni::NiRenderer* renderer = nullptr;
    // orig: the Session's kind-3 handler calls the pause menu's panel reset (0x000C5D2D -> 0x000A81F0: every panel's
    // vf0C; the options panel's sample picker 0x00027460 draws rand()) after the explosion definition and before the
    // effects file is read (docs/analysis/frontend_port.md 2.4). Unset = no pause menu.
    std::function<void()> resetPausePanels;
    // orig: Session::Render 0x000C9211 reads the pause menu's "shown" ((Ingame ctx+0x14)+0x24). Unset = never shown.
    std::function<bool()> pauseShown;
    // (port) The App hosts the level (game/app/App.h): its input system, Intermission and stream manager; LoadLevel
    // then only starts the Session (Session::Start in the caller's step, as the Ingame context's LoadLevel) and the
    // App's frames run the load. Null = the direct load with its own services and frame loop (the play tests).
    struct Host {
        input::InputSystem* input = nullptr;
        Intermission* intermission = nullptr;
        stream::Manager* streams = nullptr;
        std::shared_ptr<DialogueDisplay> dialogue;   // orig: DAT_002EBDAC (App::Init 0x000A2B4D); null = the level's own
    };
    const Host* host = nullptr;
    // orig: the App's sound system (App+0xF0): every game-facing sound call of the level (game/sound/GameSound.h).
    // When the loaders below are unset, defContext.loadSound / services.loadCachedSound / services.soundDuration call
    // it.
    sound::GameSound* sound = nullptr;
};

struct LoadedLevel {
    // Declaration order = destruction order reversed: the level (and its objects, which unlink from their
    // definitions) goes before the definitions, the streams last.
    std::vector<std::unique_ptr<ni::NiStream>> streams;
    std::unique_ptr<stream::Manager> streamManager;   // (the requests outlive their clients' last callbacks)
    DefContext defContext;                            // the definitions' context (the level points at it)
    std::unique_ptr<DefRegistry> defs = std::make_unique<DefRegistry>();
    level::LevelAnimations animations;
    // The text manager the dialogue databases load into when the options give none (orig: the TextManager
    // singleton); declared before the level, so the level's database pointers go first.
    std::unique_ptr<text::TextManager> text;
    // orig: the Intermission singleton 0x000D4E50 (created at App init, before any level).
    Intermission intermission;
    std::unique_ptr<LevelData> level = std::make_unique<LevelData>();
    std::string error;
    LevelLoadOptions options;                         // (as completed by LoadLevel; the Session refers to them)
    // orig: Input_Init 0x0009C330 (at App init): four pads, port 0 connected and open.
    std::unique_ptr<input::InputSystem> input;
    std::array<std::unique_ptr<input::Pad>, 4> pads;
    // The level object's stream client / updates (Session.h); destroyed first.
    std::unique_ptr<Session> session;
    LoadedLevel();
    ~LoadedLevel();
};


// Returns false (with `error`) when a file of the Session chain after the effects file is missing (the original's
// chain then stops there silently). Without an effects file the original builds nothing (testlevel): the level stays
// empty and true is returned.
bool LoadLevel(const std::string& name, LevelLoadOptions options, LoadedLevel& out);

// The stream manager's keepFile for a level's requests (LoadLevel installs it on its own manager; the App's manager
// calls it for the requests of the level it hosts).
void KeepLevelFile(LoadedLevel& level, stream::Request& r, std::unique_ptr<ni::NiStream> file);

}  // namespace fut::game

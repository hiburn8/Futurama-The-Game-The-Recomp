// Level load sequence; see LevelLoader.h.
#include "game/level/LevelLoader.h"

#include "core/CrtRand.h"
#include "core/CrtString.h"
#include "engine/stream/StreamManager.h"
#include "game/defs/CharacterDef.h"
#include "game/level/Session.h"
#include "game/level/Stage.h"
#include "game/sound/SoundEvents.h"

#include <algorithm>

namespace fut::game {

LoadedLevel::LoadedLevel() = default;
LoadedLevel::~LoadedLevel() {
    session.reset();                                  // (before the level it refers to)
}

// The port keeps the loaded files except the characters' (other code points into them without references; a character
// file must go, as the original's does: its sequences' reference counts decide reuse or clone).
void KeepLevelFile(LoadedLevel& out, stream::Request& r, std::unique_ptr<ni::NiStream> f) {
    // (the original's ClearObjects frees the file's lights nothing else holds: their effect ids come back)
    if (f) f->ReleaseDynamicEffects();
    if (r.owner == "Stage") {                         // (released by Stage::Unload with the stage's BSP)
        if (auto* st = dynamic_cast<Stage*>(r.client)) {
            st->file = std::move(f);
            return;
        }
    }
    // (the player's vision file and the explosion meshes: FPView / ExplosionDef keep their roots; the files go, as
    // the original's, so the roots' lights die with their holders, docs/analysis/effect_release_order.md 3 rule B and
    // step 4)
    if (r.owner != "CharacterDef" && r.owner != "FPView" && r.owner != "ExplosionDef") out.streams.push_back(std::move(f));
}

bool LoadLevel(const std::string& name, LevelLoadOptions o, LoadedLevel& out) {
    sound::GameSound* snd = o.sound;
    out.level->sound = snd;
    if (!o.defContext.loadSound)                      // orig: Sound_Load 0x000C9860
        o.defContext.loadSound = [snd](const std::string& p, u32 f) { return snd->Load(p, f); };
    if (!o.defContext.releaseSound)                   // orig: Sound_Release 0x000C9880
        o.defContext.releaseSound = [snd](u32 h) { snd->Release(h); };
    if (!o.services.loadCachedSound)                  // orig: SoundCache_LoadFile 0x000C9840
        o.services.loadCachedSound = [snd](ni::NiUDSFileObject& file) { return snd->LoadCached(file); };
    if (!o.services.soundDuration)                    // orig: Sound_GetDuration 0x000C9930
        o.services.soundDuration = [snd](u32 h) { return double(snd->Duration(h)); };
    if (!o.defContext.rand) o.defContext.rand = [] { return crt::Rand(); };   // the CRT rand 0x0016695C
    LevelData& level = *out.level;
    level.name = name;
    out.defContext = o.defContext;
    DefContext& ctx = out.defContext;
    ctx.level = &level;
    ctx.levelName = name;
    level.loadSound = ctx.loadSound;
    if (!o.services.loadSound) o.services.loadSound = ctx.loadSound;   // (the Materials footstep preload)
    level.animations = &out.animations;
    level.defContext = &ctx;
    level.defs = out.defs.get();
    if (!o.services.text) {
        out.text = std::make_unique<text::TextManager>(o.readFile);
        o.services.text = out.text.get();
    }
    level.text = o.services.text;
    level.fileExists = o.services.fileExists ? o.services.fileExists : [read = o.readFile](const std::string& p) {
        std::vector<u8> data;
        return read(p, data);
    };
    level.soundDuration = o.services.soundDuration;
    // In the game's flow the sound events are real (the play step's animation events draw CRT rand() numbers from
    // them): Sounds\SoundLookup.nif goes into the level's table and the definitions look their events up there.
    if (o.ingame && !o.soundLookup) {
        level.soundEvents = std::make_shared<SoundEventTable>();
        SoundEventTable* table = level.soundEvents.get();
        o.soundLookup = [table](ni::NiStream& s) {    // orig: SoundEvents::LoadLookup(top[0]) 0x000CA7E0
            const auto& top = s.TopLevelLinks();
            if (!top.empty()) table->LoadLookup(dynamic_cast<ni::NiUDSFileObject*>(s.ResolveLink(top[0])));
        };
        ctx.lookupSoundEvent = [table, &level](const std::string& n) { return table->Lookup(n.c_str(), level.loadSound); };
    }

    if (const LevelLoadOptions::Host* host = o.host) {   // (the App's input system and Intermission)
        level.input = host->input;
        level.intermission = host->intermission;
        if (host->dialogue) level.dialogue = host->dialogue;   // (the App's display, with its text object)
    } else {
        // orig: Input_Init 0x0009C330: four pads (each joins the list), XInputOpen for the connected port 0, Reset,
        // SetDisconnected(0).
        out.input = std::make_unique<input::InputSystem>();
        for (u32 port = 0; port < 4; ++port) {
            out.pads[port] = std::make_unique<input::Pad>(*out.input, port);
            out.input->pads[port] = out.pads[port].get();
        }
        out.pads[0]->SetDeviceOpen(true);
        out.pads[0]->Reset();
        out.pads[0]->SetDisconnected(false);
        level.input = out.input.get();
        out.intermission.input = out.input.get();     // (Intermission: the rumble switch, the movie, the clock)
        if (!o.playMovie)                             // App::PlayMovie 0x000A2410 without a player (the oracle's
            o.playMovie = [snd](const char* movie) {  //  emulator): a name fails, NULL resumes the sounds
                if (!movie) snd->ResumeAll();         // Sound_ResumeAll 0x000A2468
                return false;
            };
        out.intermission.playMovie = o.playMovie;
        out.intermission.isMoviePlaying = o.isMoviePlaying;
        out.intermission.queryCounter = [] { return u64{0}; };   // (no frames without the App)
        out.intermission.renderer = o.renderer;       // (App->vf14(): the movie camera's)
        level.intermission = &out.intermission;
    }
    if (!o.readPad) {
        static const input::GamepadState neutral{};
        o.readPad = [](u32 port) { return port == 0 ? &neutral : nullptr; };
    }

    if (!o.host) {
        out.streamManager = std::make_unique<stream::Manager>();
        out.streamManager->readFile = o.readFile;
        out.streamManager->eventLog = o.streamLog;
        out.streamManager->keepFile = [&out](stream::Request& r, std::unique_ptr<ni::NiStream> f) {
            KeepLevelFile(out, r, std::move(f));
        };
    }
    stream::Manager& m = o.host ? *o.host->streams : *out.streamManager;
    stream::Manager* previous = stream::Manager::Current();
    stream::Manager::Current() = &m;
    o.defContext = ctx;                               // (the Session's handlers use the level's context)
    out.options = std::move(o);
    LevelLoadOptions& opt = out.options;
    out.session = std::make_unique<Session>(opt, out, m);
    Session& session = *out.session;
    session.Start(name);
    if (opt.host) {                                   // (the App's frames run the load)
        stream::Manager::Current() = previous;
        return true;
    }
    auto pump = [&] {                                 // (level_runner.run_level: the manager loop alone)
        for (u32 n = 0; n < opt.maxSteps && !m.AllFinished(); ++n) m.Step();
    };
    if (opt.ingame) {                                 // (the game's flow is the App's: game/app/App.h)
        out.error = name + ": the game's flow needs the App (LevelLoadOptions::host)";
        stream::Manager::Current() = previous;
        return false;
    }
    pump();
    // Viewer convenience: the stages the LinkEnd markers chain to (in the game they stream while playing).
    for (size_t i = 0; opt.followStageLinks && i < level.stages.size(); ++i) {
        Stage& st = *level.stages[i];
        if (!st.linkEnd.empty() && !level.FindStage(st.linkEnd)) {
            level.CreateStage(st.linkEnd)->RequestStream();
            pump();
        }
    }
    stream::Manager::Current() = previous;
    // A missing Session file ends its chain silently (the request stays "missing"); without the effects file the
    // original builds nothing (testlevel).
    if (const stream::Request* r = session.SessionRequest(); r && r->state == stream::State::Missing && session.kind > 3) {
        out.error = name + ": missing " + (r->path ? *r->path : std::string("?"));
        return false;
    }
    return true;
}

}  // namespace fut::game

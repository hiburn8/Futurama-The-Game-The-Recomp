// The Intro, Attract, Ingame and Death contexts; see Contexts.h.
#include "game/app/Contexts.h"

#include "core/CrtRand.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiRenderer.h"
#include "game/GameGlobals.h"
#include "game/GameTime.h"
#include "game/app/Breakdown.h"
#include "game/app/ErrorOverlay.h"
#include "game/app/Frontend.h"
#include "game/app/MemoryCards.h"
#include "game/app/PauseMenu.h"
#include "game/level/LevelData.h"
#include "game/level/LevelLoader.h"
#include "game/level/Session.h"
#include "game/text/LanguageDb.h"
#include "engine/ni/NiCull.h"

#include <cstdio>

namespace fut::game::app {

using ni::detail::F;

// ---- the splash screen ---------------------------------------------------------------------------------------------

Splash::Splash(App& app, std::string p, f32 minT, f32 maxT, ni::NiCamera* cam)
    : path(std::move(p)), minTime(minT), maxTime(maxT) {   // orig: 0x000EFBC0
    // the base 0x000EF710(path, camera, 1): the image's tiles (the language's default.nif when the path fails)
    if (app.services.renderer && cam)
        image = std::make_unique<LoadingScreen>(app.services.renderer, &app.services.readFile, path,
                                                "images\\" + (app.text ? app.text->Language() : std::string("English")) +
                                                    "\\default.nif",
                                                cam);
}

bool Splash::Update(App& app, f32 dt) {               // orig: 0x000EED10
    // 0x00100CE0 (+0x18): the camera's Click culls its +0x15C (the extras grid's camera: the grid's root; the splash
    // screens' own cameras: none), then the tiles (its screen textures)
    if (image && image->camera) ni::VisitForCulling(*image->camera, image->camera->sceneRoot);
    const double t = double(dt) + double(time);       // FLD dt; FADD +8; FCOM +0x34; FST +8
    time = F(t);
    if (t < double(minTime)) return true;             // TEST AH,5; JP
    if (maxTime != 0.0f && !(t <= double(maxTime))) return false;   // FCOMP max; TEST AH,0x41; JNE
    input::Pad* pad = app.ActivePad();                // both are read (no short circuit)
    const bool start = pad->JustPressedUnlatched(input::kStart);
    const bool a = pad->JustPressedUnlatched(input::kButtonA);
    return !(start || a);
}

// ---- the Intro -----------------------------------------------------------------------------------------------------

IntroContext::IntroContext(App& app) : Context(app, "Intro") {   // orig: 0x0002B440
    // the font list +0x20 (0x000984D0), the camera new(0x250) 0x001009D0 -> +0x38 with the renderer's +0x1A8
    if (app.services.renderer) {
        camera = new ni::NiCamera;
        camera->renderer = app.services.renderer;     // App->vf14()
    }
    last = app.qpc;                                   // +0x20 = QueryCounter
    totalTime = 0;
    realDt = 0;
    updates = 0;                                      // DAT_002DAE2C
}

bool IntroContext::OnEnter() {                        // orig: 0x0002B240
    done = false;                                     // +0x28
    // +0x34 = TextManager LoadDb("Language\FileNames.ldb") (the movies' subtitles' names: text)
    updates = 0;
    last = app_.qpc;                                  // +0x20
    return true;
}

void IntroContext::OnExit() {                         // orig: 0x0002B330
    // the FileNames database unloaded (text)
    splash.reset();                                   // +0x3C
}

bool IntroContext::ShowSplash(const char* path, f32 minTime, f32 maxTime) {   // orig: 0x0002B360
    if (!splash) splash = std::make_unique<Splash>(app_, path, minTime, maxTime, camera.Get());   // new(0x3C) 0x000EFBC0
    if (!splash->Update(app_, realDt)) {              // vf04(+0x18)
        splash.reset();
        return false;
    }
    return true;
}

bool IntroContext::ShowMovie(const char* movie, s32 forState) {   // orig: 0x0002B290
    if (!app_.IsMoviePlaying()) {
        if (movieState == forState) return false;
        app_.PlayMovie(movie, 0x0002B2CC);
        movieState = forState;
    }
    return true;
}

bool IntroContext::Update() {                         // orig: 0x0002B5F0
    const u64 now = app_.qpc;
    const double d = app_.Seconds(static_cast<s64>(now - last));   // 0x000D2BC0, 0x000D2C10
    realDt = F(d);                                    // FST +0x18
    totalTime = F(d + double(totalTime));             // FADD +0x14; FSTP
    last = now;
    switch (state) {
    case 0:                                           // the legal screen; at the 21st update the pause menu
        (void)ShowSplash(app_.ntsc ? "images\\English\\legal-NTSC.nif" : "images\\English\\legal.nif", 6.0f, 6.0f);
        if (++updates > 20) {                         // DAT_002DAE2C
            app_.pause = std::make_unique<PauseMenu>(app_);   // new(0x6C) 0x000A83F0 -> DAT_002E9428
            app_.pause->Build();                      // 0x000A8640
            app_.input.activePad = nullptr;           // g_activePad = 0
            state = 1;
        }
        break;
    case 1:
        if (!ShowSplash(app_.ntsc ? "images\\English\\legal-NTSC.nif" : "images\\English\\legal.nif", 6.0f, 6.0f))
            state = 2;
        break;
    case 2:
        if (!ShowMovie("fox", 2)) state = 3;
        break;
    case 3:
        if (!ShowMovie(app_.ntsc ? "vug" : "sci", 3)) state = 4;
        break;
    case 4:
        if (!ShowMovie("uds", 4)) state = 5;
        break;
    case 5:
        if (!ShowSplash(app_.ntsc ? "images\\english\\NI_Splash_NTSC.nif" : "images\\english\\NI_Splash_PAL.nif",
                        1.5f, 5.0f))
            state = 8;
        break;
    case 8:
        state = 9;
        break;
    case 9:
        // the clear colour black, clear(0, 1) (render); the Frontend preloads
        app_.frontend->preloaded = true;              // Frontend+0x2C = 1
        app_.frontend->Enter();                       // vf04 (starts its load)
        state = 7;
        break;
    case 7:
        if (app_.frontend->HasLoadingScreen()) app_.frontend->LoadStep();   // 0x00022A00
        else state = 6;
        break;
    case 6:
        if (!ShowMovie("intro", 6)) state = 10;
        break;
    default:                                          // 10
        done = true;                                  // +0x28
        app_.RequestContext("Frontend", 0x0002B983);
        if (app_.services.renderer) app_.services.renderer->ClearBuffer(nullptr, 5);   // App->vf14()->vf44(0, 5)
        app_.frontend->Start();                       // FindContext("Frontend") 0x000A3120, 0x00021A80
        break;
    }
    input::Pad* pad = app_.ActivePad();
    if (pad->JustPressedUnlatched(input::kStart)) {   // skips a movie
        pad->Latch(input::kStart);
        app_.PlayMovie(nullptr, 0x0002B9F3);
    } else if (pad->JustPressedUnlatched(input::kButtonA)) {
        app_.ActivePad()->Latch(input::kButtonA);
        app_.PlayMovie(nullptr, 0x0002B9F3);
    }
    if (double(app_.input.idleTime) > 0.33f) app_.input.activePad = nullptr;   // [0x002D5C58]; TEST AH,0x41; JNE
    return !done;
}

void IntroContext::Render() {                         // orig: 0x0002B300 (state 7: the front end's loading screen)
    if (state == 7 && app_.frontend->HasLoadingScreen()) app_.frontend->Render();
}

// ---- Attract -------------------------------------------------------------------------------------------------------

void AttractContext::SetReturn(const char* to) {      // orig: 0x0002BAD0
    returnTo = app_.FindContext(to ? to : "Frontend");
}

bool AttractContext::OnEnter() {                      // orig: 0x0002BB30
    app_.input.activePad = nullptr;
    app_.PlayMovie("Attract", 0x0002BB4A);
    return true;
}

void AttractContext::OnExit() { app_.PlayMovie(nullptr, 0x0002BB0D); }   // orig: 0x0002BB00

bool AttractContext::Update() {                       // orig: 0x0002BB50
    if (!app_.IsMoviePlaying()) {
        app_.RequestContext(returnTo ? returnTo->name.c_str() : "Frontend", returnTo ? 0x0002BB78 : 0x0002BB8C);
        return false;
    }
    input::Pad* pad = app_.ActivePad();
    if (pad->JustPressedUnlatched(input::kStart) || app_.ActivePad()->JustPressedUnlatched(input::kButtonA)) {
        app_.PlayMovie(nullptr, 0x0002BBC1);
        app_.RequestContext(returnTo ? returnTo->name.c_str() : "Frontend", returnTo ? 0x0002BBD7 : 0x0002BBE5);
        return false;
    }
    return true;
}

// ---- Ingame --------------------------------------------------------------------------------------------------------

IngameContext::IngameContext(App& app) : Context(app, "Ingame") {}   // orig: 0x0002A1F0
IngameContext::~IngameContext() { DeleteLevel(); }

void IngameContext::DeleteLevel() {
    if (!level_) return;
    if (level_->level) app_.lives = level_->level->lives;   // (DAT_002D687C is a global: the level's view of it)
    // orig: the Session dtor's head 0x000C8421..0x000C8453 (the rest is ~LevelData): the active pad's rumble off,
    // Input_SetRumbleEnabled(0, 0), Input_Update, the active pad's DecrementDisabled(0xFF)
    app_.ActivePad()->SetRumble(0.0f, 0.0f, false);
    app_.input.SetRumbleEnabled(false, 0.0f);
    app_.input.Update(g_frameStep, app_.services.readPad);   // Input_Update 0x0009BEA0
    app_.ActivePad()->DecrementDisabled(0xFF);
    level_.reset();
}

void IngameContext::Leave(const char* next) {        // orig: 0x0002A220
    app_.RequestContext(next, 0x0002A233);
    leave = true;                                     // +0x18
}

bool IngameContext::OnEnter() {                       // orig: 0x0002A9F0
    leave = false;
    if (!app_.pause) {                                // DAT_002E9428: built by the Intro on the boot path
        app_.pause = std::make_unique<PauseMenu>(app_);   // new(0x6C) 0x000A83F0
        app_.pause->Build();                          // 0x000A8640
    }
    // ctx+0x14 = the pause menu
    LoadLevel(app_.LevelName());                      // App+0xAC
    app_.SetLevelName("");                            // 0x000A23D0(0x00295254)
    idleTime = 0;                                     // DAT_002DAC68
    return true;
}

bool IngameContext::LoadLevel(const char* levelName) {   // orig: 0x0002A760
    if (!levelName) return false;
    // the old level deleted (the profiler's scene hook first), 0x0002A660 (the renderer flush), the sorter released,
    // SoundEvents::Shutdown 0x000CA980 (the level's table goes with it), Mem_ReallocBinArrays, Mem_CoalesceAll, a new
    // sorter
    DeleteLevel();
    app_.FlushRenderer();                             // 0x0002A660 (unconditional), the sorter released (render)
    app_.Sound().DeleteAllSamples();                  // SoundEvents::Shutdown's first call: sys vt+0x0C 0x000C96C0
    level_ = std::make_unique<LoadedLevel>();         // g_level = new(0x200) Session 0x000C38B0
    LevelLoadOptions o;
    o.readFile = app_.services.readFile;
    o.ingame = true;
    o.readPad = app_.services.readPad;
    o.renderer = app_.services.renderer;
    o.services.text = app_.text.get();                // (the TextManager singleton DAT_002EC6E0: LEVELNAMES.ldb)
    host_.input = &app_.input;
    host_.intermission = &app_.intermission;
    host_.streams = &app_.streams;
    host_.dialogue = app_.dialogue;                   // (DAT_002EBDAC)
    o.host = &host_;
    o.sound = &app_.Sound();                          // (App+0xF0)
    o.resetPausePanels = [this] {                     // the kind-3 handler: 0x000E4C90, then 0x000A81F0
        if (app_.memoryCards) app_.memoryCards->ReloadSounds();   // (App+0xF4) 0x000E4C90
        if (app_.pause) app_.pause->ResetPanels();
    };
    o.pauseShown = [this] { return app_.pause && app_.pause->shown; };
    level_->level->leaveIngame = [this](const char* next) { Leave(next); };   // (DAT_002DAC60's 0x0002A220)
    level_->level->carry = &app_.carry;               // (DAT_002EAA28..39: the App's)
    level_->level->endLevel = [this](const char* next, s32 seconds) {   // (FsChangeLevel's App part)
        app_.EndLevel(*level_->level, next, seconds);
    };
    level_->level->setAppLevelName = [this](const char* name) { app_.SetLevelName(name); };   // 0x000A23D0
    if (!game::LoadLevel(levelName, std::move(o), *level_)) return false;   // Session::Start 0x000C2340
    idleTime = 0;
    return true;
}

bool IngameContext::Update() {                        // orig: 0x0002AAB0
    if (app_.breakdown && app_.breakdown->open) {     // DAT_002E8EEC && its +1 (FsChangeLevel's menu)
        idleTime = 0;                                 // DAT_002DAC68
        app_.ActivePad()->DecrementDisabled(0xFF);
        app_.breakdown->Update();                     // 0x000A59D0
        return true;                                  // (MOV AL,1: the leave flag is not read here)
    }
    if (app_.LevelName()[0] != '\0') {                // App+0xAC: the next level, a replay, a load
        LoadLevel(app_.LevelName());
        app_.SetLevelName("");
    }
    if (!level_ || !level_->session) return !leave;
    Session& s = *level_->session;
    if (!s.IsReady()) {                               // 0x000C3EB0
        idleTime = 0;
        s.Update();                                   // 0x000C8C30
        return true;
    }
    Intermission& im = app_.intermission;             // 0x000D4E50
    input::Pad* pad = app_.ActivePad();
    if (im.active) {                                  // 0x000D31A0
        idleTime = 0;
        pad->DecrementDisabled(0xFF);
        bool tryStart;
        if (!pad->IsDownUnlatched(input::kStart)) {
            skipStartReleased = true;                 // DAT_002DAE29 = 1
            tryStart = true;
        } else {
            tryStart = skipStartReleased;
        }
        if (tryStart && (pad->JustPressedUnlatched(input::kStart) || pad->JustPressedUnlatched(input::kButtonA)) &&
            im.Skippable()) {                         // 0x000D31B0
            im.End();                                 // 0x000D5790
            skipStartReleased = false;
            if (pad->IsDownUnlatched(input::kStart)) pad->Latch(input::kStart);
            if (pad->IsDownUnlatched(input::kButtonA)) pad->Latch(input::kButtonA);
        } else {
            (void)im.Update();                        // vf08 0x000D61B0
        }
        pauseStartReleased = false;                   // DAT_002DAE28 = 0
    }
    if (im.active && !(im.movie && im.fadePhase)) return !leave;   // (0x000D32E0 = !fadePhase)
    PauseMenu& pause = *app_.pause;                   // ctx+0x14
    if (!pause.shown && !app_.memoryCards->running) {
        if (input::Pad* active = app_.input.activePad; active && active->Disconnected() == 1) {
            idleTime = 0;
            pause.Open();                             // 0x000A82B0
        }
        if (!app_.ActivePad()->IsDownUnlatched(input::kStart)) pauseStartReleased = true;   // DAT_002DAE28 = 1
        else if (pauseStartReleased && app_.ActivePad()->IsDownUnlatched(input::kStart)) {   // held after a release
            idleTime = 0;
            pause.Open();
        }
        bool allIdle = true;                          // Input_AllPadsIdle 0x0009BE70
        for (input::Pad* p : app_.input.pads)
            if (p && !p->IsIdle()) allIdle = false;
        if (allIdle) {
            const double sum = double(g_frameStep) + double(idleTime);   // FLD dt; FADD; FST; FCOMP 1800.0
            idleTime = F(sum);
            if (sum > 1800.0f) {                      // [0x00295278]; TEST AH,0x41; JNE
                idleTime = 0;
                pause.Open();
            }
        } else {
            idleTime = 0;
        }
        s.Update();                                   // the game step runs in the step that opened the pause too
    } else {
        idleTime = 0;
        if (pause.shown) {
            app_.ActivePad()->DecrementDisabled(0xFF);
            pause.Update();                           // 0x000A8520 (no Session update)
        }
    }
    return !leave;                                    // !ctx+0x18
}

void IngameContext::Render() {                        // orig: 0x0002A320
    if (level_ && level_->session) level_->session->Render(app_.lastRenderDt);   // Session::Render 0x000C8F20
    // the pad error: with the pause shown, a memory-card flow running or the breakdown open, an active pad that is
    // disconnected (== 1) shows ERRCTRLINGAME with its port + 1 (pad vf04 0x0009C450)
    if ((app_.pause && app_.pause->shown) || app_.memoryCards->running || (app_.breakdown && app_.breakdown->open))
        if (input::Pad* p = app_.input.activePad; p && p->Disconnected() == 1 && app_.errorOverlay)
            app_.errorOverlay->SetMessage("ERRCTRLINGAME", p->Port() + 1);   // 0x000F2CF0 (0x002951FC)
    app_.cheats.enabled = 1;                         // DAT_002E7C1C+0x34 = 1 (the cheat-code recorder)
    if (app_.pause && app_.pause->shown) {            // (+0x14)+0x24
        app_.cheats.enabled = 0;
        app_.pause->Draw();                           // 0x000A8320
    }
    if (app_.breakdown && app_.breakdown->open) {     // DAT_002E8EEC +1
        app_.cheats.enabled = 0;
        app_.breakdown->Draw();                       // 0x000A4C40
    }
}

void IngameContext::OnExit() {                        // orig: 0x0002A240
    app_.cheats.enabled = 0;                          // DAT_002E7C1C+0x34 = 0 (the cheat-code recorder)
    DeleteLevel();                                    // delete g_level
}

// ---- Death ---------------------------------------------------------------------------------------------------------

DeathContext::DeathContext(App& app) : Context(app, "Death") {   // orig: 0x0002BF50
    // the font list +0x18 (0x000984D0), DAT_002DB1A4 = this, the camera new(0x250) 0x001009D0 -> +0x38 with the
    // renderer's +0x1A8
    if (app.services.renderer) {
        camera = new ni::NiCamera;
        camera->renderer = app.services.renderer;
    }
}

bool DeathContext::OnEnter() {                        // orig: 0x0002BEC0
    rendered = false;                                 // +0x40
    done = false;                                     // +0x24
    splash.reset();                                   // +0x3C
    state = 0;                                        // +0x28
    last = app_.qpc;                                  // +0x18
    realDt = 0;                                       // +0x20
    g_clearFlags = 4;                                 // DAT_002D67E0
    return true;
}

bool DeathContext::ShowSplash() {                     // orig: 0x0002C0F0
    if (!splash) {
        rendered = false;
        splash = std::make_unique<Splash>(app_, "images\\English\\Death.nif", 2.5f, 3.0f, camera.Get());   // 0x000EFBC0
    }
    if (rendered) {
        splash.reset();
        return false;
    }
    return true;
}

bool DeathContext::Update() {                         // orig: 0x0002C1C0
    const u64 now = app_.qpc;
    realDt = F(app_.Seconds(static_cast<s64>(now - last)));   // +0x20
    last = app_.qpc;
    if (state == 0) {
        (void)ShowSplash();
        state = 1;
        gameOverSound = app_.LoadSound("Sounds\\Localized\\English\\menu\\voice\\GameOver.wav", 0);   // +0x34
        // (!= -1: Sound_PlaySFX(it, 0): sound)
    } else if (state == 1) {
        if (!ShowSplash()) state = 2;
    } else {
        // Sound_Release(+0x34): sound
        done = true;                                  // +0x24
        app_.RequestContext(next ? next->name.c_str() : "Frontend", next ? 0x0002C233 : 0x0002C242);
    }
    input::Pad* pad = app_.ActivePad();
    if (pad->JustPressedUnlatched(input::kStart) || app_.ActivePad()->JustPressedUnlatched(input::kButtonA))
        app_.PlayMovie(nullptr, 0x0002C301);
    return !done;
}

void DeathContext::Render() {                         // orig: 0x0002BF10
    if (splash && !splash->Update(app_, realDt)) rendered = true;
}

}  // namespace fut::game::app

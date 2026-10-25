// The App's contexts other than the Frontend (Frontend.h): the Intro (boot: legal screen, movies, NI splash, the
// front end's preload), the Attract movie, the Ingame context (the level) and the game-over screen ("Death").
// docs/analysis/frontend_port.md 1.4, 1.6, 1.9, 2.2, 2.3, 2.8.
#pragma once

#include "game/app/App.h"
#include "game/level/LevelLoader.h"
#include "game/level/Session.h"

#include <memory>
#include <string>

namespace fut::game::app {

// orig: the splash screen (ctor 0x000EFBC0(path, camera, min, max), vtable 0x002A5860, base 0x000EF710 = the
// loading screen's image tiles on the camera, LoadingScreen's base part): vf04 0x000EED10(realDt): the camera's
// Click, then the timing. The camera is the caller's (the Intro's / the Death screen's +0x38); without a renderer
// there is no image (the timing is the game's).
struct Splash {
    Splash(App& app, std::string path, f32 minTime, f32 maxTime, ni::NiCamera* camera);
    // orig: vf04 0x000EED10: 1 while shown, 0 when done (after `max`, or a Start / A press after `min`).
    bool Update(App& app, f32 realDt);
    std::string path;
    f32 time = 0;                                     // orig: +0x08
    f32 minTime = 0;                                  // orig: +0x34
    f32 maxTime = 0;                                  // orig: +0x38 (0 = no limit)
    std::unique_ptr<LoadingScreen> image;             // orig: the base's +0x18 camera and +0x1C.. tiles
};

// orig: Intro (ctor 0x0002B440, 0x40 bytes, vtable 0x002952B8): Enter 0x0002B240, Update 0x0002B5F0, Render
// 0x0002B300, Exit 0x0002B330.
class IntroContext : public Context {
public:
    explicit IntroContext(App& app);
    bool Update() override;
    void Render() override;
    f32 totalTime = 0;                                // orig: +0x14
    f32 realDt = 0;                                   // orig: +0x18
    u64 last = 0;                                     // orig: +0x20
    bool done = false;                                // orig: +0x28
    s32 state = 0;                                    // orig: +0x2C
    // orig: +0x30, the state whose movie was started: never initialised by the ctor (new(0x40): heap garbage, not 2
    // under the oracle, which plays "fox"; L: 0 here).
    s32 movieState = 0;
    std::unique_ptr<Splash> splash;                   // orig: +0x3C
    s32 updates = 0;                                  // orig: DAT_002DAE2C (the update count of states 0 / 1)
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x38 (new(0x250) 0x001009D0, the App's renderer)

protected:
    bool OnEnter() override;
    void OnExit() override;

private:
    bool ShowSplash(const char* path, f32 minTime, f32 maxTime);   // orig: 0x0002B360
    bool ShowMovie(const char* name, s32 forState);   // orig: 0x0002B290
};

// orig: Attract (ctor 0x0002BA80, 0x18 bytes, vtable 0x0029537C): Enter 0x0002BB30, Update 0x0002BB50, Exit
// 0x0002BB00; +0x14 the context to return to (0x0002BAD0).
class AttractContext : public Context {
public:
    explicit AttractContext(App& app) : Context(app, "Attract") {}
    bool Update() override;
    void Render() override {}
    void SetReturn(const char* name);                 // orig: 0x0002BAD0
    Context* returnTo = nullptr;                      // orig: +0x14

protected:
    bool OnEnter() override;
    void OnExit() override;
};

// orig: Ingame (ctor 0x0002A1F0, 0x1C bytes, vtable 0x002951E0): Enter 0x0002A9F0, Update 0x0002AAB0, Render
// 0x0002A320, Exit 0x0002A240; LoadLevel 0x0002A760; Leave 0x0002A220.
class IngameContext : public Context {
public:
    explicit IngameContext(App& app);
    ~IngameContext() override;
    bool Update() override;
    void Render() override;
    bool LoadLevel(const char* name);                 // orig: 0x0002A760
    void Leave(const char* next);                     // orig: 0x0002A220 (request + ctx+0x18 = 1)
    LoadedLevel* Level() const { return level_.get(); }
    bool leave = false;                               // orig: +0x18
    f32 idleTime = 0;                                 // orig: DAT_002DAC68
    bool skipStartReleased = false;                   // orig: DAT_002DAE29
    bool pauseStartReleased = false;                  // orig: DAT_002DAE28

protected:
    bool OnEnter() override;
    void OnExit() override;

private:
    void DeleteLevel();
    std::unique_ptr<LoadedLevel> level_;              // orig: g_level DAT_002DAC64 (the Session) and its data
    LevelLoadOptions::Host host_;                     // (the App's services the levels run on)
};

// orig: Death (ctor 0x0002BF50, 0x48 bytes, vtable 0x002953C0): Enter 0x0002BEC0, Update 0x0002C1C0, Render
// 0x0002BF10, Exit 0x0002BF30.
class DeathContext : public Context {
public:
    explicit DeathContext(App& app);                  // orig: 0x0002BF50
    bool Update() override;
    void Render() override;
    f32 realDt = 0;                                   // orig: +0x18
    u64 last = 0;                                     // orig: +0x1C / +0x20
    bool done = false;                                // orig: +0x24
    s32 state = 0;                                    // orig: +0x28
    u32 gameOverSound = 0;                            // orig: +0x2C
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x38 (new(0x250) 0x001009D0, the App's renderer)
    std::unique_ptr<Splash> splash;                   // orig: +0x3C
    bool rendered = false;                            // orig: +0x40 (Render saw the splash end)
    Context* next = nullptr;                          // orig: +0x44

protected:
    bool OnEnter() override;
    void OnExit() override {}                         // orig: 0x0002BF30 (nothing of the game state)

private:
    bool ShowSplash();                                // orig: 0x0002C0F0
};

}  // namespace fut::game::app

// The level object's load sequence and frame updates as the game runs them (orig: Session::Start 0x000C2340, the
// stream callback Session::OnStreamItemLoaded 0x000C5BD0 with its kind chain, Session::Update 0x000C8C30,
// Session::Render 0x000C8F20, UpdateStageStreaming 0x000C70C0 / PreloadNeighbours 0x000C4300, the load check
// 0x000C53D0). docs/analysis/stream_manager.md 4-5, docs/analysis/session_update.md 2-3.
//
// The Session request walks the kinds 0..10 (script, sound cache, HUD, effects, level file, camera path, backdrops,
// sound lookup, definitions, dialogue), one file per callback; requests created by the handlers (the explosion
// definition, the characters' .ucf files, the first stage) queue behind it in creation order. Once every request
// and stage is done the load check starts the loading screen's fade-out and shows the level title (hold, fade in,
// hold, fade out); play updates start when both are gone.
//
// NOT PORTED: the script (kind 0, and the per-step script update), parts of the HUD (kind 2; Hud.h), the title's text (the loading screen's and the title's screen textures
// are drawn with a renderer), the saved-state block of the load check, the room-based stage switching of UpdateStageStreaming (it needs the camera; only the neighbour
// preloading runs), and the play-update pieces listed in UpdatePlay / Render.
#pragma once

#include "engine/stream/StreamManager.h"
#include "game/level/LevelLoader.h"
#include "game/text/TextObject.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fut::game {

// Reads a file of the game's file system (LevelLoadOptions::readFile).
using ReadFileFn = std::function<bool(const std::string& path, std::vector<u8>& out)>;

// orig: the loading screen (Session +0x11C, ctor 0x000EFBF0, vtbl 0x002A58A0; base 0x000EF710 vtbl 0x002A5844): vf04
// Update 0x000EED90 (real frame time; the camera's Click every App frame of the load: it is the BeginScene that packs
// the first stage's pre-cache before the second is loaded, renderer_plan.md 9), vf10 FadeOut 0x000EE8B0, vf14
// IsFading 0x000EE8D0, vf18 IsDone 0x000EE910. The image (images\<language>\<level>.nif, else
// images\<language>\default.nif) is cut into six screen textures (0x000EE9A0) and the spinner's six frames
// (images\slrm00..05.nif) take turns in a seventh, all on the camera; the renderer draws them (vf55 0x0014C600).
struct LoadingScreen {
    // orig: the base ctor 0x000EF710(path, new NiCamera 0x001009D0, 1) and the derived part 0x000EFBF0 (spinner 1):
    // +0x18 the camera (+0x1A8 = App->vf14() when unset), the image's tiles attached and Update(0, 1), then the
    // spinner. `renderer` null: no camera, no images (the play tests: nothing of it reaches the game state).
    LoadingScreen(ni::NiRenderer* renderer = nullptr, const ReadFileFn* readFile = nullptr,
                  const std::string& path = {}, const std::string& defaultPath = {});
    // orig: the base 0x000EF710(path, camera, 1) alone (the splash screens 0x000EFBC0): the image's tiles on a camera
    // the caller owns (the Intro's / the Death screen's +0x38); no spinner.
    LoadingScreen(ni::NiRenderer* renderer, const ReadFileFn* readFile, const std::string& path,
                  const std::string& defaultPath, ni::NiCamera* sharedCamera);
    ~LoadingScreen();                                 // orig: 0x000EFAD0 (the tiles off the camera)
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x18
    f32 time = 0;                                     // orig: +0x08
    f32 fadeLength = 0;                               // orig: +0x10 (-1 once the image is cut)
    f32 fadeTime = 0;                                 // orig: +0x14 (-1 once the image is cut)
    bool attached = false;                            // orig: +0x04 (the tiles are on the camera)
    ni::NiPointer<ni::NiScreenTexture> tiles[6];      // orig: +0x1C..+0x30
    ni::NiPointer<ni::NiSourceTexture> spinnerFrames[6];   // orig: +0x34..+0x48 (the derived class)
    ni::NiPointer<ni::NiScreenTexture> spinnerTexture;     // orig: +0x4C
    s32 spinnerFrame = 0;                             // orig: +0x50 (the derived class)
    bool spinner = true;                              // orig: +0x54 (the ctor's 4th argument: 1)
    void Update(f32 dt);
    void FadeOut(f32 length) { fadeTime = 0; fadeLength = length; }
    bool IsFading() const { return 0.0f < fadeLength && 0.0f <= fadeTime && fadeTime < fadeLength; }
    bool IsDone() const { return 0.0f < fadeLength && 0.0f < fadeTime && fadeTime == fadeLength; }

private:
    void Base(ni::NiRenderer* renderer, const ReadFileFn* readFile, const std::string& path,
              const std::string& defaultPath, ni::NiCamera* camera);   // orig: 0x000EF710
    void Derived(const ReadFileFn* readFile);         // orig: 0x000EFBF0's own part (the spinner)
};

// orig: the level-title image (Session +0x120, ctor 0x000EF400(path, x, y) with "images\levels\<level>.nif" and
// (0.5, 0.42), vtbl 0x002A57F8): vf04 HoldTick 0x000EEFF0 (per step), vf08 Draw 0x000EF010 (fade per render, the
// camera's Click), vf10 FadeOut 0x000EF1F0, vf14 FadeIn 0x000EF230, vf18 SetHold 0x000EE950, vf1C IsFading 0x000EE960,
// vf20 IsVisible 0x000EE980, vf0C SetText 0x000EF140 (the TextObject +0x34, rebuilt by Draw while fading: one new
// NiTriShape a frame).
struct TitleImage {
    TitleImage() = default;
    // orig: 0x000EF400 with a renderer: its camera +0x20 (App->vf14()), the file's NiSourceTexture as a screen
    // texture +0x24 centred on (x, y) of the target, on the camera. `loaded` = it exists.
    TitleImage(ni::NiRenderer& renderer, const ReadFileFn& readFile, const std::string& path, f32 x, f32 y);
    bool loaded = false;                              // orig: +0x24 (the screen object; nothing moves without it)
    f32 hold = 0;                                     // orig: +0x04
    f32 direction = 0;                                // orig: +0x08 (1 fading in, -1 fading out, 0 still)
    f32 fadeLength = 0;                               // orig: +0x0C
    f32 fadeTime = 0;                                 // orig: +0x10
    f32 alpha = 0;                                    // orig: +0x14
    f32 x = 0.5f, y = 0.42f;                          // orig: +0x18 / +0x1C
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x20
    ni::NiPointer<ni::NiScreenTexture> image;         // orig: +0x24
    f32 textX = 0, textY = 0;                         // orig: +0x28 / +0x2C
    const char16_t* label = nullptr;                  // orig: +0x30 (TextManager::GetText(key, 0))
    std::unique_ptr<text::TextObject> text;           // orig: +0x34 (made by the first SetText)
    // orig: vf0C 0x000EF140 (key, x, y): with the image only; the text object made once (font 0, the renderer, hAlign
    // 0, vAlign 3, no wrap, no z test, scale (1, 1.5)), its strings removed, the key's text and position kept.
    void SetText(const text::TextManager* tm, const char* key, f32 x, f32 y, ni::NiRenderer* renderer,
                 const ReadFileFn& readFile);
    void HoldTick(f32 dt) {
        if (loaded) hold = hold - dt;                 // (a float subtraction)
    }
    void Draw(f32 realDt);
    void FadeOut(f32 length);                         // orig: 0x000EF1F0 (the rect's colour (1, 1, 1, 1), +0x18 |= 4)
    void FadeIn(f32 length);                          // orig: 0x000EF230 (the rect's colour (1, 1, 1, 0), +0x18 |= 4)
    bool IsFading() const { return direction != 0.0f; }
    bool IsVisible() const { return 0.0f < alpha; }
};

class Session : public stream::Client {
public:
    Session(LevelLoadOptions& options, LoadedLevel& out, stream::Manager& streams);
    void Start(const std::string& levelName);         // orig: 0x000C2340 (the parts ported)
    bool OnStream(stream::Request& r) override;        // orig: 0x000C5BD0
    // orig: 0x000C8C30, one call per fixed step: the title hold, then the loading state (UpdateStageStreaming and
    // the load check), the overlay state (scripts and the "$cam" binding only) or a play step.
    void Update();
    // orig: 0x000C8F20, once per App frame; `realDt` = the real time since the previous render (DAT_002E7C28).
    void Render(f32 realDt);
    // orig: native 161 FsChangeLevel 0x000B05B0 (its Session / App part; object_gaps.md 21.3).
    void ChangeLevel(const char* name);
    // orig: 0x000C2770 (thiscall S(char** out), RET 4) with out == null: the carry-over text into
    // LevelData::carry->state (DAT_002EAA28); a failed write keeps the old text.
    void CarryOverText();
    // orig: 0x000C2770 with out (the in-game save's text, 0x000DFEE0): "Level=<App+0xAC, else S+0xC>\n", then the
    // carry text when one is set (DAT_002EAA28, appended with its NUL), else the player's lines and a NUL. Returns the
    // size (NUL included; 0 = failed / no player). `save` null: the carry-over form.
    u32 SaveText(std::string* save, const char* appLevel);
    // orig: the load check's apply of DAT_002EAA28 0x000C5554..0x000C59A9 (the level-start copy kept, the text freed).
    void ApplyCarryOver();
    // orig: 0x000C3EB0: stages exist, loaded (state 4 or every stage loaded) and no loading screen / title.
    bool IsReady() const;
    void UpdateStageStreaming();                      // orig: 0x000C70C0 (the preloading part)
    void PreloadNeighbours(bool loadedOnly, bool twoAhead);   // orig: 0x000C4300
    u32 state = 0;                                    // orig: +0x10 (2 loading, 4 loaded)
    s32 kind = -1;                                    // orig: +0xC4
    bool effectsFileExists = false;                   // orig: +0x198
    std::optional<LoadingScreen> loadingScreen;       // orig: +0x11C
    std::optional<TitleImage> title;                  // orig: +0x120
    f32 loadTime = 0;                                 // orig: +0x174 (+= dt per load check)
    f32 playSeconds = 0, playSeconds2 = 0;            // orig: +0x178 / +0x180 (wrap at 3600)
    s32 playHours = 0, playHours2 = 0;                // orig: +0x17C / +0x184
    s32 blackFrames = 0;                              // orig: DAT_002EAA3C (after an intermission; not set yet)
    const stream::Request* SessionRequest() const { return request_.get(); }

private:
    bool Exists(const std::string& path) const;
    void LoadCheck();                                 // orig: 0x000C53D0
    void UpdatePlay();                                // orig: 0x000C8C30, the play branch
    void RenderObjects(f32 realDt);                   // orig: 0x000C9229..0x000C92E7 (not while paused)
    LevelLoadOptions& o_;
    LoadedLevel& out_;
    stream::Manager& streams_;
    std::shared_ptr<stream::Request> request_;        // orig: +0x14
};

}  // namespace fut::game

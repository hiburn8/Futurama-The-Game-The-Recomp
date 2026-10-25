// The front end (orig: Frontend, ctor 0x00022650, 0x1D0 bytes, vtable 0x002942A8, stream client +0x14 vtable
// 0x002942A0 -> 0x00025410): Enter 0x000218F0, the load step 0x00022A00 with its stream chain, the scene setup
// 0x00024DD0 (with the clock-based srand), Start 0x00021A80, Update 0x00022B30, Render 0x00021B60, Exit 0x00022E80;
// the main panel CFrontendMainPanel (vtable 0x00294174). docs/analysis/frontend_port.md 1.5, 2.9.
//
// Ported: the game-state parts: the load chain (menu\frontend.nif, menu\extras\thumbs.nif, menu/extras/Extra_A.nif,
// menu/extras/Extra_B.nif through the App's stream manager), the reseed, the panels' creation order (the options
// panel's sample picker), the timers of the scene (the four bubble slots 0x000223B0 and the background drops
// 0x00022150 draw rand(); their lengths are the file's sequences), the idle / Attract timer, the 30-step guard, the
// main panel's input (up / down / A) with the first-press memory-card chain and the New Game countdown.
// The scene (render, round 4): the scene setup's nodes and keyframe managers (the camera Cam3 and its moves' manager,
// the drops, the logo, the roller, the smokes, the bubbles: 0x00023A30, 0x00023D10, 0x000247A0, 0x00021DB0,
// 0x000238D0), the text object +0x98 and the labels +0x38 (game/app/MenuScene.h), the fade-in polygon 0x00024AA0 /
// 0x00022550, the main panel's items (NiSwitchNodes: the highlight = child 1), Render 0x00021B60's updates and the
// camera's Click.
// Round 5: the extras menus' scenes (game/app/Extras.h: the thumbnails 0x0001CF00, the grid 0x0001E130 and the list
// 0x0001F360 made by the load, deleted by the extras panel's destructor 0x00020A70), the extras panel's Build
// 0x00020DA0, Exit 0x00022E80's release order (the effect ids' free list), Start's clear colour and clear.
// Round 6: the options panel (game/app/OptionsPanel.h: its scene, Update, Show / Hide) and the camera moves
// (MoveCamera 0x00023940, their end in Update).
// NOT PORTED (render / sound, reported when it matters): the music / speech, the load panel's scene and the load /
// extras panels' updates (inactive: reported when chosen), the extras menus' updates and their draw (0x000208E0), the
// profiler's node (DAT_002DB83C +0x2C: no profiler is made).
#pragma once

#include "engine/ni/NiKeyframeManager.h"
#include "engine/ni/NiObjects.h"
#include "engine/stream/StreamManager.h"
#include "game/app/App.h"
#include "game/level/Session.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace fut::text {
class TextObject;
}

namespace fut::game::app {

class FrontendContext;
class FrontendOptionsPanel;
class FrontendLoadPanel;
struct MenuLabels;
class ThumbGrid;
class MusicList;

// orig: the panel base (ctor 0x000298A0(arg), vtable 0x00294FEC): +4 active, +5 opening, +6 closing, +8 selection,
// +0xC scene node, +0x10 the ctor's argument, +0x14 input delay.
struct PanelBase {
    bool active = false;                              // orig: +4
    bool opening = false;                             // orig: +5
    bool closing = false;                             // orig: +6
    s32 selection = 0;                                // orig: +8
    s32 delay = 0;                                    // orig: +0x14
    bool Busy() const { return opening || closing; }  // orig: 0x00029880
};

// orig: CFrontendMainPanel (new(0x50), ctor 0x00020810(0, frontend), Build 0x00020320; vtable 0x00294174: Update
// 0x00020580, Show 0x0001FF10, Hide 0x0001FF40, Activate 0x00020150, Deactivate 0x00020180).
struct FrontendMainPanel : PanelBase {
    explicit FrontendMainPanel(FrontendContext& fe) : fe(fe) {}
    ~FrontendMainPanel();                             // orig: 0x00020270 (items 0..2, then the three sounds)
    bool Update(f32 dt);                              // orig: vf04 0x00020580
    void Show();                                      // orig: vf18 0x0001FF10
    void Hide() { Deactivate(); }                     // orig: vf1C 0x0001FF40 (the hint no-op, then vf24)
    void Activate();                                  // orig: vf20 0x00020150
    void Deactivate();                                // orig: vf24 0x00020180
    void Select(s32 sel);                             // orig: 0x00020000
    bool Countdown();                                 // orig: 0x00020090
    void ContinueChain();                             // orig: 0x000201B0 (the first press's memory-card chain)
    bool StartNextCheck();                            // orig: 0x000200F0
    // orig: 0x00020320 (scene): the items main_newgamenull / main_optionsnull / main_continuenull / main_extras
    // (NiSwitchNode, RTTI 0x002F9820) +0x20..+0x2C, the sounds, +8 = 0.
    void Build(ni::NiNode* scene);
    // orig: the highlight of an item: child 1 when it has more than one (+0xCC > 1), off: child 0 (+0xCC > 0).
    static void Highlight(ni::NiSwitchNode* item, bool on);
    FrontendContext& fe;                              // orig: +0x34
    std::array<ni::NiPointer<ni::NiSwitchNode>, 4> items;   // orig: +0x20..+0x2C
    input::Pad* pad = nullptr;                        // orig: +0x30 (the pad of the first press)
    u32 scrollSound = 0, selectSound = 0, extraSound = 0;   // orig: +0x38 / +0x3C / +0x40
    bool showFlag = false;                            // orig: +0x44
    bool flag45 = false;                              // orig: +0x45
    s32 countdown = 0;                                // orig: +0x48 (New Game: 5 steps)
    s32 pendingSelection = -1;                        // orig: +0x4C
};

// orig: the extras panel ("CFrontendNewgamePanel", new(0x48) 0x00020FE0(2, frontend), vtable 0x002941F0, Build
// 0x00020DA0(scene), dtor 0x00020A70, Update 0x00020B60, Draw 0x000208E0, Show 0x00020A20, Hide 0x00020920, Activate
// 0x00020980, Deactivate 0x000209A0; frontend_panels_port.md 5.2).
struct FrontendExtrasPanel : PanelBase {
    explicit FrontendExtrasPanel(FrontendContext& fe) : fe(fe) {}
    ~FrontendExtrasPanel();                           // orig: 0x00020A70 (the sounds, the items, the two menus)
    void Build(ni::NiNode* scene);                    // orig: 0x00020DA0
    bool Update(f32 dt);                              // orig: vf04 0x00020B60 (the shown menu's update first)
    void Draw();                                      // orig: vf08 0x000208E0 (the shown menus' draws)
    void Show();                                      // orig: vf18 0x00020A20
    void Hide();                                      // orig: vf1C 0x00020920
    void Activate() { active = true; }                // orig: vf20 0x00020980 (the hint no-op)
    void Deactivate() { active = false; }             // orig: vf24 0x000209A0
    void Highlight();                                 // orig: 0x000209C0 (every item off, the selection's on)
    FrontendContext& fe;                              // orig: +0x34
    u32 goToSound = 0, startSound = 0, goBackSound = 0, scrollSound = 0;   // orig: +0x20 / +0x24 / +0x28 / +0x2C
    bool startLevel = false;                          // orig: +0x30
    std::array<ni::NiPointer<ni::NiSwitchNode>, 3> items;   // orig: +0x38..+0x40 (extra_movies / music / gallery)
    f32 t = 0;                                        // orig: +0x44
};

// orig: the bubble slots and the drops of the scene: a sequence's length = (hi - lo) / frequency of its reference
// controller (0x00022411 / 0x00022355 / 0x000249DF ...), 0 without the sequence.
struct SceneSequence {
    ni::NiPointer<ni::NiSequence> seq;
    ni::NiPointer<ni::NiNode> node;                   // (the drops' / bubbles' node)
    ni::NiPointer<ni::NiKeyframeManager> manager;     // (its keyframe manager, new(0xC0) 0x00130D20 on the node)
    double Length() const;                            // (the x87 value: callers round where the original stores)
};

// orig: the menu input 0x00029FA0(action) on the active pad (a disconnected pad gives false): 0 up, 1 down, 2 left,
// 3 right, 4 accept (A / Start just pressed), 5 back (B / Back), 6 Start, 7 X held. DAT_002D60D4 is the mode.
bool MenuInput(App& app, s32 action);
inline s32 g_menuInputMode = 0;                       // orig: DAT_002D60D4 (0x00029F80 / 0x00029F90)

// orig: the first-press chain's flags DAT_002D9DE4 (options flow), DAT_002D9DE8 (New Game's check), DAT_002D9DEC
// (extras flow): 0 not run, 1 started, 2 done. Process-wide (never reset).
inline s32 g_feOptionsChain = 0, g_feCheckChain = 0, g_feExtrasChain = 0;

class FrontendContext : public Context, public stream::Client {
public:
    explicit FrontendContext(App& app);
    ~FrontendContext() override;
    bool Update() override;                           // orig: vf08 0x00022B30
    void Render() override;                           // orig: vf0C 0x00021B60
    bool OnStream(stream::Request& r) override;       // orig: 0x00025410 (client +0x14)
    void LoadStep();                                  // orig: 0x00022A00
    void Start();                                     // orig: 0x00021A80
    bool HasLoadingScreen() const { return loadingScreen; }   // orig: 0x00021320
    // orig: 0x00023940(n): unless a move runs (+0x6C), the camera sequence n (0 / 1 Camera_Newgame_there / back, 2 / 3
    // Camera_Options_there / back, 4 / 5 Camera_Continue_there / back; 1 / 3 / 5 restart the logo first) as the
    // running move +0x110, its end time +0x180 = its length + the scene time, its offset -time, activated on the
    // camera's manager +0x17C.
    bool MoveCamera(s32 n);
    App& App_() { return app_; }

    bool preloaded = false;                           // orig: +0x2C (the Intro's preload)
    bool started = false;                             // orig: +0x2D
    bool flowBusy = false;                            // orig: +0x2E (a memory-card flow of the menus runs)
    bool loaded = false;                              // orig: +0x40
    bool stay = true;                                 // orig: +0x4C (Update's result)
    f32 logoTime = 0, logoEnd = 0;                    // orig: +0x58 / +0x5C
    bool cameraMoving = false;                        // orig: +0x6C
    f32 volumeBackup = 0;                             // orig: +0x74
    f32 time = 0;                                     // orig: +0x78
    f32 idleTime = 0;                                 // orig: +0x7C
    bool guard = true;                                // orig: +0x80
    s32 guardCount = 0;                               // orig: +0x84 (uninitialised until Start: 30)
    // orig: the drops +0x9C.. (sequence, node, manager per drop): Slurmcans +0x9C, Daliclock +0xA8, Kibbles +0xB4,
    // Robothead +0xC0, Furfist +0xCC, Cigarrs +0xD8.
    SceneSequence slurmcans, daliclock, kibbles, robothead, furfist, cigarrs;
    f32 idleAnimEnd = 0;                              // orig: +0xE4
    f32 field_e8 = 0;                                 // orig: +0xE8
    bool idleAnimRunning = false;                     // orig: +0xEC (the running drop's node) is not null
    SceneSequence running;                            // orig: +0xF0 seq / +0xEC node / +0xF4 manager (the drop)
    std::array<SceneSequence, 4> bubbles;             // orig: +0x124.. seq, +0x134.. node, +0x114.. manager
    std::array<f32, 4> bubbleTimer{};                 // orig: +0x144..
    std::array<f32, 4> bubbleField154{};              // orig: +0x154.. (-1 when a bubble starts; no reader found)
    SceneSequence roller;                             // orig: +0x164 seq (Roller1), +0x16C node, +0x168 manager
    f32 shotInterval = 0;                             // orig: +0x170 (Roller1's length)
    f32 shotTimer = 0;                                // orig: +0x174
    f32 timer178 = 0;                                 // orig: +0x178
    f32 cameraEnd = 0;                                // orig: +0x180
    bool fadePending = true;                          // orig: +0x1B8
    u32 musicSound = 0, speechSound = 0;              // orig: +0x1AC / +0x1B0
    s32 musicVoice = 0;                               // orig: +0x1B4 (Sound_PlayMusic(+0x1AC, 1))
    // orig: 0x00021330: the music again (+0x1B4 = Sound_PlayMusic(+0x1AC, 1)), the sound system's vf04 / vf08
    void PlayMusic();
    // orig: 0x0001B6B0 (the extras menus): the music again unless it plays (+0x1B4)
    void RestartMusic() { if (!musicVoice) PlayMusic(); }
    // orig: 0x0001B6F0: Voice_Stop(+0x1B4), +0x1B4 = 0 (when set)
    void StopMusic();
    s32 loadState = 0;                                // orig: +0x1C4
    bool loadingScreen = false;                       // orig: +0x1C8 (images\<lang>\Loadmenu.nif)
    std::shared_ptr<stream::Request> request;         // orig: +0x1CC
    std::unique_ptr<FrontendMainPanel> mainPanel;     // orig: +0x1C
    std::unique_ptr<FrontendExtrasPanel> extrasPanel; // orig: +0x20
    std::unique_ptr<FrontendOptionsPanel> optionsPanel;   // orig: +0x24 (game/app/OptionsPanel.h)
    std::unique_ptr<FrontendLoadPanel> loadPanel;     // orig: +0x28 (game/app/LoadPanel.h)
    PanelBase* currentPanel = nullptr;                // orig: +0x34
    // orig: the extras menus DAT_002D9A70 / DAT_002D9B98 (made by the load's states 4 / 5, deleted by the extras
    // panel's destructor: game/app/Extras.h)
    std::unique_ptr<ThumbGrid> extrasGrid;
    std::unique_ptr<MusicList> extrasList;

protected:
    bool OnEnter() override;                          // orig: vf04 0x000218F0
    void OnExit() override;                           // orig: vf10 0x00022E80

private:
    void SetupScene(ni::NiStream& s);                 // orig: 0x00024DD0
    void SetupCamera();                               // orig: 0x00023A30
    void SetupManagers();                             // orig: 0x00023D10
    void SetupText();                                 // orig: 0x00021CD0
    void SetupPanels();                               // orig: 0x00021FA0
    void SetupBubbles();                              // orig: 0x000247A0
    void SetupNodes();                                // orig: 0x00021DB0
    void SetupFade();                                 // orig: 0x00024AA0
    void StartLogo();                                 // orig: 0x000238D0
    void Fade();                                      // orig: 0x00022550
    void UpdateBubbles();                             // orig: 0x000223B0
    void BackgroundShot(f32 offset);                  // orig: 0x00022150
    bool AllPadsIdle() const;                         // orig: Input_AllPadsIdle 0x0009BE70
    ni::NiPointer<ni::NiSequence> logotype_;          // orig: +0x54 (Logotype)
    ni::NiPointer<ni::NiNode> logoNode_;              // orig: +0x50 (FUTURAMA)
    ni::NiPointer<ni::NiKeyframeManager> logoManager_;   // orig: +0x60
    ni::NiPointer<ni::NiNode> root_;                  // orig: +0x48 (the scene root)
    ni::NiPointer<ni::NiCamera> camera_;              // orig: +0x68 (Cam3)
    ni::NiPointer<ni::NiNode> cameraNode_;            // orig: +0x64 (Cam2:Animation (Translational))
    ni::NiPointer<ni::NiKeyframeManager> cameraManager_;   // orig: +0x17C
    // orig: +0xF8..+0x10C Camera_<Newgame|Options|Continue>_<there|back>, +0x110 the running move
    std::array<ni::NiPointer<ni::NiSequence>, 6> cameraMoves_;
    ni::NiPointer<ni::NiSequence> cameraMove_;
    ni::NiPointer<ni::NiNode> smokeNode_;             // orig: +0x70 (Smokenelectra)
    ni::NiPointer<ni::NiNode> alwaysNode_;            // orig: +0x88 (Allways_animated)
    std::array<SceneSequence, 3> smokes_;             // orig: +0x190.. seq (Smoke2-4), +0x19C.. node, +0x184.. manager
    std::unique_ptr<text::TextObject> text_;          // orig: +0x98
    std::unique_ptr<MenuLabels> labels_;              // orig: +0x38
    ni::NiPointer<ni::NiScreenPolygon> fade_;         // orig: +0x1BC
    std::unique_ptr<LoadingScreen> screen_;           // orig: +0x1C8's object (with a renderer)
};

// orig: 0x00027460 (the options panels' Reset): the music / speech preview samples, `rand() % 10` repeated while 0
// (0x000270D0: the picker's +0x50 is never written) and `rand() % 8` likewise (0x00027110); their Sound_Load calls
// (sound) and the seven menu sounds.
void PickPreviewSamples(App* app);

}  // namespace fut::game::app

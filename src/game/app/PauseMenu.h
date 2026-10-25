// The pause menu (orig: DAT_002E9428, new(0x6C) ctor 0x000A83F0, Build 0x000A8640; persistent: built by the Intro at
// its 21st update, or by Ingame::Enter when none exists): Open 0x000A82B0, Update 0x000A8520, Close 0x000A8220, the
// fade-out start 0x000A8290, the panels' Reset 0x000A81F0 (every level load: the Session's kind-3 handler), the main
// panel (vtable 0x002A1D84: Update 0x000A7570, Show 0x000A7020, Hide 0x000A70B0, Activate 0x000A7150, Deactivate
// 0x000A7190). docs/analysis/frontend_port.md 1.10, 2.4.
//
// The scene's build (render, round 4): the camera, the root with its z-buffer property, the scene-mode text object,
// pause\pause.nif's SceneNode with its lights and camera node, the labels object (game/app/MenuScene.h) and the
// pre-cache 0x000CEAC0 (Build 0x000A8640, Intro frame 20).
// The panels: the main panel, the save-slot list CPauseContinuePanel (game/app/LoadPanel.h: the load flow XBox_L_*) and
// the options CPauseOptionsPanel (game/app/OptionsPanel.h). The yes / no dialog of the Quit confirm is drawn by Draw
// (0x000E33A0 from 0x000A8320, since 2026-10-19).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <array>
#include <memory>

namespace fut::text {
class LanguageDb;
class TextObject;
}  // namespace fut::text

namespace fut::game::app {

class App;
class PauseMenu;
class PauseLoadPanel;
class PauseOptionsPanel;
struct MenuLabels;

// orig: the main panel (new(0x5C) 0x000A7AE0): +4 active, +5 opening, +6 closing, +8 selection, +0x14 input delay,
// +0x18 the animation time, +0x2C the confirm state, +0x40 the pause menu.
struct PausePanel {
    bool Update(f32 dt);                              // orig: vf04 0x000A7570
    void Show();                                      // orig: vf18 0x000A7020
    void Hide();                                      // orig: vf1C 0x000A70B0
    void Activate();                                  // orig: vf20 0x000A7150
    void Deactivate();                                // orig: vf24 0x000A7190
    void Reset();                                     // orig: vf0C 0x000A6F30 (the sounds)
    bool Busy() const { return opening || closing; }  // orig: 0x00029880
    // orig: the highlight of an item (NiSwitchNode): child 1 when it has more than one (+0xCC > 1), off: child 0.
    static void Highlight(ni::NiSwitchNode* item, bool on);
    PauseMenu* pause = nullptr;
    bool active = false, opening = false, closing = false;
    s32 selection = 0;
    s32 delay = 0;
    f32 t = 0;
    s32 confirm = 0;
    ni::NiPointer<ni::NiNode> node;                   // orig: +0xC ("main1", 0x000A72A0)
    std::array<ni::NiPointer<ni::NiSwitchNode>, 4> items;   // orig: +0x30..+0x3C (main_newgamenull .. main_extras)
    // orig: +0x44 scroll.wav, +0x48 select.wav, +0x4C pause_fold_up.wav, +0x50 scrolllimit.wav, +0x54
    // pause_fold_down.wav (0x000A6F30)
    u32 scrollSound = 0, selectSound = 0, foldUpSound = 0, limitSound = 0, foldDownSound = 0;
    const text::LanguageDb* texts = nullptr;          // orig: +0x58 (language\BREAKDOWN.ldb, 0x000A72A0)
};

class PauseMenu {
public:
    explicit PauseMenu(App& app);
    ~PauseMenu();
    void Build();                                     // orig: 0x000A83F0 + 0x000A8640 (the options panel: 2 picks)
    void ResetPanels();                               // orig: 0x000A81F0 (each panel's vf0C)
    void Open();                                      // orig: 0x000A82B0
    void Update();                                    // orig: 0x000A8520
    void Close();                                     // orig: 0x000A8220
    void StartFadeOut();                              // orig: 0x000A8290
    void Draw();                                      // orig: 0x000A8320 (Ingame::Render while shown)
    void SetOverlayAlpha(f32 a);                      // (+0x40)->GetProperty(3): +0x50 = a, +0x54 + 1
    App& app;
    PausePanel main;                                  // orig: +0x18
    std::unique_ptr<PauseLoadPanel> load;             // orig: +0x1C (CPauseContinuePanel: game/app/LoadPanel.h)
    std::unique_ptr<PauseOptionsPanel> options;       // orig: +0x20 (CPauseOptionsPanel: game/app/OptionsPanel.h)
    bool shown = false;                               // orig: +0x24
    bool closing = false;                             // orig: +0x25
    f32 alpha = 0;                                    // orig: +0x28
    f32 rumbleLeft = 0, rumbleRight = 0;              // orig: +0x30 / +0x34
    f32 field38 = 0;                                  // orig: +0x38
    bool fadingIn = false, fadingOut = false;         // orig: +0x3C / +0x3D
    f32 time = 0;                                     // orig: +0x68
    // the scene (0x000A8640)
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x48 (new(0x250) 0x001009D0)
    ni::NiPointer<ni::NiNode> root;                   // orig: +0x44
    std::unique_ptr<text::TextObject> text;           // orig: +0x54
    ni::NiPointer<ni::NiNode> scene;                  // orig: the main panel's +0xC (pause.nif's "SceneNode")
    ni::NiPointer<ni::NiTriShape> fade;               // orig: +0x40 ("fade1Shape", RTTI 0x0030EC38)
    ni::NiPointer<ni::NiNode> camNull;                // orig: +0x4C ("camnull")
    std::array<ni::NiPointer<ni::NiNode>, 3> always;  // orig: +0x5C..+0x64 ("Allways_animated", "allways_animated2/3")
    std::unique_ptr<MenuLabels> labels;               // orig: +0x14 (new(0x10) 0x00025710)
    u32 draws = 0;                                    // orig: +4 (Draw's count)
};

}  // namespace fut::game::app

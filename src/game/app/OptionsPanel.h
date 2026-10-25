// The options panel (orig: the base vtable 0x00294E0C, ctor 0x000286E0, Reset 0x00027460, Build 0x000279C0, Update
// 0x00028960; the front end's vtable 0x00294260: ctor 0x000210A0, Show 0x000211A0, Hide 0x00021200, Activate
// 0x00021290, Deactivate 0x000212F0). docs/analysis/frontend_panels_port.md 2.
//
// The pause's options panel (CPauseOptionsPanel, vtable 0x002A1E44: ctor 0x000A7FA0, Reset 0x000A7F50, Show 0x000A8060,
// Hide 0x000A8160, Activate 0x000A7F90, dtor 0x000A7EE0): the same rows, a time-based fold at half steps.
// NOT PORTED: the row hints (0x000272A0 / HINTS_xxx: no-ops in this build).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"
#include "game/app/Frontend.h"
#include "game/app/Options.h"

#include <array>
#include <memory>
#include <vector>

namespace fut::text {
class TextObject;
}

namespace fut::game::app {

class App;
class PauseMenu;

// orig: the options panel base (0x10FC bytes; PanelBase + +0x18 the fold time).
class OptionsPanel : public PanelBase {
public:
    explicit OptionsPanel(App& app) : app(app) {}
    virtual ~OptionsPanel();
    bool Update(f32 dt);                              // orig: vf04 0x00028960
    virtual void Reset();                             // orig: vf0C 0x00027460 (the picker, its samples, the sounds)
    // orig: vf28 0x000279C0 (camera, scene): the picker, the rows' switches, the sliders' nodes, menu\align.nif's
    // adjust-screen polygon (an alpha and a texturing property in its state), then Reset.
    void Build(ni::NiCamera* camera, ni::NiNode* scene);
    virtual void Show() = 0;                          // vf18
    virtual void Hide() = 0;                          // vf1C
    virtual void Activate() = 0;                      // vf20
    virtual void Deactivate() = 0;                    // vf24
    bool Changed() const;                             // orig: 0x000271B0 (the first 0x1C bytes against the backup)
    bool Backup();                                    // orig: 0x00027150
    bool RestoreBackup();                             // orig: 0x00027230
    void KeepShownScreen();                           // orig: 0x00027440
    void StartAdjusting();                            // orig: 0x00028940
    void StopAdjusting();                             // orig: 0x00027590
    void ReleaseSounds();                             // orig: vf10 0x00027250
    // orig: the switch highlight: child `index` when the switch has more than `index` children (+0xCC > index)
    static void SetIndex(ni::NiSwitchNode* s, s32 index);

    App& app;
    f32 t = 0;                                        // orig: +0x18
    std::vector<u8> backup;                           // orig: +0x24 / +0x28 (inline copy) / +0x1028 (its size)
    bool haveBackup = false;                          // orig: +0x24 != 0
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x102C (Build's first argument)
    ni::NiPointer<ni::NiNode> sliderSfx, sliderSpeech, sliderMusic;   // orig: +0x1058 / +0x105C / +0x1060
    std::array<ni::NiPointer<ni::NiSwitchNode>, 8> items;   // orig: +0x1064..+0x1080
    ni::NiPointer<ni::NiSwitchNode> vibrationSwitch;  // orig: +0x1084 ("main_options_onoff")
    ni::NiPointer<ni::NiSwitchNode> invertSwitch;     // orig: +0x1088 ("main_options_onoff2")
    text::TextObject* text = nullptr;                 // orig: +0x108C (the front end's text object +0x98; no reader)
    f32 rumble = 0, rumblePrev = 0;                   // orig: +0x1090 / +0x1094
    std::unique_ptr<PreviewPicker> picker;            // orig: +0x1098
    bool saveRunning = false;                         // orig: +0x109C
    bool discardAsked = false;                        // orig: +0x109E
    ni::NiPointer<ni::NiScreenPolygon> frame;         // orig: +0x10A0 (the adjust-screen polygon)
    ni::NiPointer<ni::NiSourceTexture> alignTexture;  // orig: +0x10A4 (menu\align.nif)
    f32 shownX = 0, shownY = 0;                       // orig: +0x10AC / +0x10B0
    f32 speech = 0, sfx = 0, music = 0;               // orig: +0x10B4 / +0x10B8 / +0x10BC
    u32 scrollSound = 0, selectSound = 0, toggleOkSound = 0, toggleErrorSound = 0;   // orig: +0x10C0 .. +0x10CC
    u32 speechLoopSound = 0, sfxLoopSound = 0, musicClipSound = 0;   // orig: +0x10D0 / +0x10D8 / +0x10DC
    s32 musicVoice = 0;                               // orig: +0x10E0
    bool adjusting = false;                           // orig: +0x10E4
    f32 adjustX = 0, adjustY = 0;                     // orig: +0x10E8 / +0x10EC

private:
    void UpdateSlider(ni::NiNode* slider, f32 v);     // StartAnimations, NiAVObject::Update(v, 1), StopAnimations
    void ShowSwitches();                              // (the vibration / invert switches from the flags)
};

// orig: the front end's options panel (new(0x10FC) 0x000210A0(2, frontend, +0x98): +0x10F0 the Frontend, +0x10F4
// go_to.wav, +0x10F8 go_back.wav).
class FrontendOptionsPanel : public OptionsPanel {
public:
    FrontendOptionsPanel(App& app, FrontendContext& fe);
    ~FrontendOptionsPanel() override;
    void Show() override;                             // orig: 0x000211A0
    void Hide() override;                             // orig: 0x00021200
    void Activate() override;                         // orig: 0x00021290
    void Deactivate() override;                       // orig: 0x000212F0
    FrontendContext& fe;                              // orig: +0x10F0
    u32 goToSound = 0, goBackSound = 0;               // orig: +0x10F4 / +0x10F8
};

// orig: CPauseOptionsPanel (new(0x10FC) 0x000A7FA0(0, pause, +0x54): +0x10F0 the pause menu, +0x108C its text object,
// +0x18 = 0.8333333, +0x10F4 pause_go_to.wav, +0x10F8 pause_go_back.wav, 0x00027460).
class PauseOptionsPanel : public OptionsPanel {
public:
    PauseOptionsPanel(App& app, PauseMenu& pause, text::TextObject* text);
    ~PauseOptionsPanel() override;                    // orig: 0x000A7EE0
    void Reset() override;                            // orig: 0x000A7F50
    void Show() override;                             // orig: 0x000A8060 (t 0.8333 .. 1.0416666: +4 set from the start)
    void Hide() override;                             // orig: 0x000A8160 (t 1.25 .. 1.4583334)
    void Activate() override;                         // orig: 0x000A7F90 (Backup 0x00027150)
    void Deactivate() override {}                     // orig: vf24 0x000A32A0 (nothing)
    PauseMenu& pause;                                 // orig: +0x10F0
    u32 goToSound = 0, goBackSound = 0;               // orig: +0x10F4 / +0x10F8
    ni::NiPointer<ni::NiNode> node;                   // orig: +0xC ("options1")
};

}  // namespace fut::game::app

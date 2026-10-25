// The options panel; see OptionsPanel.h.
#include "game/app/OptionsPanel.h"

#include "engine/ni/NiKeyframeManager.h"
#include "engine/ni/NiStream.h"
#include "game/GameTime.h"
#include "game/app/App.h"
#include "game/app/MemoryCards.h"
#include "game/app/MenuScene.h"
#include "game/app/PauseMenu.h"

#include <bit>
#include <cstring>

namespace fut::game::app {

using ni::detail::F;

// ---- the panel -------------------------------------------------------------------------------------------------

OptionsPanel::~OptionsPanel() {                       // orig: 0x000275B0
    // (the switches' and the rows' nodes, the rows' strings (RemoveString on +0x108C), +0x1080: their NiPointers)
    // the five sounds again (the inlined 0x00027250: the pause's panel released them already, literal), then the
    // camera's polygon +0x10A0 (0x00100FC0) and the rest
    for (u32 h : {speechLoopSound, sfxLoopSound, musicClipSound, toggleOkSound, toggleErrorSound}) app.Sound().Release(h);
}

void OptionsPanel::SetIndex(ni::NiSwitchNode* s, s32 index) {
    if (!s) return;                                   // (the original reads the switch without a test)
    if (s32(s->Children().size()) > index) s->index = index;   // CMP +0xCC; JLE; +0xE0 = index
}

void OptionsPanel::Reset() {                          // orig: 0x00027460
    if (!picker) picker = std::make_unique<PreviewPicker>(app);   // new(0x70) 0x00026BF0
    PreviewPicker& p = *picker;
    p.sfxPlaying = 0, p.speechPlaying = 0;            // +0x58 / +0x5C
    p.sfxNext = app.LoadSound(p.sfxNames[size_t(p.PickSfx())], 0);       // +0x68
    p.speechNext = app.LoadSound(p.voiceNames[size_t(p.PickSpeech())], 0);   // +0x6C
    scrollSound = app.LoadSound("sounds/menu/scroll.wav", 0);            // +0x10C0 (0x00293C7C)
    selectSound = app.LoadSound("sounds/menu/select.wav", 0);            // +0x10C4 (0x00293EE8)
    toggleOkSound = app.LoadSound("sounds/menu/toggleOK.wav", 0);        // +0x10C8
    toggleErrorSound = app.LoadSound("sounds/menu/toggleError.wav", 0);  // +0x10CC
    speechLoopSound = app.LoadSound("sounds/menu/speechloop.wav", 0);    // +0x10D0
    sfxLoopSound = app.LoadSound("sounds/menu/sfxloop.wav", 0);          // +0x10D8
    musicClipSound = app.LoadSound("sounds/menu/MusicClip.wav", 0);      // +0x10DC
}

void OptionsPanel::ReleaseSounds() {                  // orig: 0x00027250
    app.Sound().Release(speechLoopSound);
    app.Sound().Release(sfxLoopSound);
    app.Sound().Release(musicClipSound);
    app.Sound().Release(toggleOkSound);
    app.Sound().Release(toggleErrorSound);
}

void OptionsPanel::UpdateSlider(ni::NiNode* slider, f32 v) {
    if (!slider) return;                              // (the original reads the node without a test)
    ni::StartAnimations(slider);                      // 0x0013EF40
    slider->UpdateDownwardPass(v, true);              // NiAVObject::Update(v, 1)
    ni::StopAnimations(slider);                       // 0x0013F090
}

void OptionsPanel::ShowSwitches() {                   // (the flags' switches: 0 On, 1 Off)
    SetIndex(vibrationSwitch.Get(), app.options.vibration ? 0 : 1);
    SetIndex(invertSwitch.Get(), app.options.invert ? 0 : 1);
}

void OptionsPanel::Build(ni::NiCamera* cam, ni::NiNode* scene) {   // orig: 0x000279C0
    camera = cam;                                     // +0x102C
    picker = std::make_unique<PreviewPicker>(app);    // new(0x70) 0x00026BF0 -> +0x1098 (an earlier one leaks)
    static const char* const kRows[10] = {"main_options_soundvolume", "main_options_musicvolume",
                                          "main_options_speechvolume", "main_options_vibrations",
                                          "main_options_onoff", "main_options_vibrations1",
                                          "main_options_onoff2", "main_options_adjustscreen",
                                          "main_options_restoredefaultconfig", "main_options_apply"};
    // the rows in this order: items 0-3 (+0x1064..), the vibration switch +0x1084, item 4, the invert switch +0x1088,
    // items 5-7 (each a NiSwitchNode, RTTI 0x002F9820; each item's +0xE0 = 0 as it is found)
    ni::NiPointer<ni::NiSwitchNode>* slots[10] = {&items[0], &items[1], &items[2], &items[3], &vibrationSwitch,
                                                  &items[4], &invertSwitch, &items[5], &items[6], &items[7]};
    for (size_t i = 0; i < 10; ++i) {
        *slots[i] = FindAs<ni::NiSwitchNode>(scene, kRows[i]);
        if (slots[i] != &vibrationSwitch && slots[i] != &invertSwitch) SetIndex(slots[i]->Get(), 0);
    }
    // then the ten slots +0x1064..+0x1088 in memory order: child 0
    for (size_t i = 0; i < 8; ++i) SetIndex(items[i].Get(), 0);
    SetIndex(vibrationSwitch.Get(), 0);
    SetIndex(invertSwitch.Get(), 0);
    selection = 0;                                    // +8
    // the sliders (RTTI 0x002F9230): speech +0x105C, SFX +0x1058, music +0x1060
    sliderSpeech = FindAs<ni::NiNode>(scene, "slider_speechvolume:Animation (Translational)");
    sliderSfx = FindAs<ni::NiNode>(scene, "slider_soundvolume:Animation (Translational)");
    sliderMusic = FindAs<ni::NiNode>(scene, "slider_mucisvolume:Animation (Translational)");
    speech = app.options.speech, sfx = app.options.sfx, music = app.options.music;   // +0x10B4 / +0x10B8 / +0x10BC
    UpdateSlider(sliderSpeech.Get(), speech);
    UpdateSlider(sliderSfx.Get(), sfx);
    UpdateSlider(sliderMusic.Get(), music);
    // the switches from the raw flags: vibration on -> child 1 (when more than one), off -> child 0 (the Update
    // rewrites them as `flag ? 0 : 1` at its top)
    SetIndex(vibrationSwitch.Get(), app.options.vibration ? 1 : 0);
    SetIndex(invertSwitch.Get(), app.options.invert ? 1 : 0);
    Reset();                                          // vf0C 0x00027460
    // menu\align.nif (0x00294E38): its first object as a NiSourceTexture (RTTI 0x002F9AAC) -> +0x10A4
    {
        std::vector<u8> bytes;
        alignTexture = nullptr;
        if (app.services.readFile && app.services.readFile("menu\\align.nif", bytes)) {
            ni::NiStream s;
            if (s.Load(bytes) && !s.TopLevelLinks().empty())
                alignTexture = dynamic_cast<ni::NiSourceTexture*>(s.ResolveLink(s.TopLevelLinks()[0]));
        }
    }
    // the adjust-screen polygon +0x10A0: new(0x24) 0x00106DE0(4, the corners at depth 0.001 (0x3A83126F), the
    // texture's corners, red / green / blue / yellow)
    const f32 z = std::bit_cast<f32>(0x3A83126Fu);
    const ni::NiPoint3 corners[4] = {{0, 0, z}, {1, 0, z}, {1, 1, z}, {0, 1, z}};
    const ni::NiPoint2 uvs[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const ni::NiColorA colors[4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1}};
    frame = new ni::NiScreenPolygon(4, corners, uvs, colors);
    auto* a = new ni::NiAlphaProperty;                // new(0x1C): flags 0xEC, +0x18 = 0; | 1
    a->flags = u16(a->flags | 1);
    auto* tex = new ni::NiTexturingProperty;          // new(0x3C) 0x00109DC0
    tex->SetBaseTexture(alignTexture.Get());          // 0x0001D610(+0x10A4)
    tex->applyMode = 0;                               // +0x18 = 0
    tex->maps[0].filterMode = 0;                      // 0x0001D6B0(0)
    frame->AttachProperty(a);                         // (the state's slot of its type, vf2C)
    frame->AttachProperty(tex);
    discardAsked = false;                             // +0x109E = 0
}

bool OptionsPanel::Backup() {                         // orig: 0x00027150
    std::vector<u8> d;
    const u32 n = SerializeOptions(app, &d);          // 0x000A18F0 into a heap block, copied to +0x28, deleted
    backup.assign(d.begin(), d.begin() + n);          // +0x1028 = the size
    haveBackup = true;                                // +0x24 = +0x28
    return true;
}

bool OptionsPanel::Changed() const {                  // orig: 0x000271B0
    std::vector<u8> now;
    SerializeOptions(app, &now);
    const u32 n = SerializeOptions(app, nullptr) - kOptionsRecordsSize;   // 0x1C
    // (REPE CMPSB against +0x24's copy; without a backup the original reads null: a UB guard compares as changed)
    if (!haveBackup || backup.size() < n) return true;
    return std::memcmp(now.data(), backup.data(), n) != 0;
}

bool OptionsPanel::RestoreBackup() {                  // orig: 0x00027230
    if (haveBackup && backup.size() >= 0x214) DeserializeOptions(app, backup.data());   // 0x000A2070(+0x24)
    return true;
}

void OptionsPanel::KeepShownScreen() {                // orig: 0x00027440
    shownX = app.options.screenX;                     // +0x10AC
    shownY = app.options.screenY;                     // +0x10B0
}

void OptionsPanel::StartAdjusting() {                 // orig: 0x00028940
    if (camera && frame) camera->AttachScreenPolygon(frame.Get());   // 0x00019460(+0x10A0)
    adjusting = true;                                 // +0x10E4
}

void OptionsPanel::StopAdjusting() {                  // orig: 0x00027590
    if (camera && frame) camera->DetachScreenPolygon(frame.Get());   // 0x00100FC0(+0x10A0)
    adjusting = false;
}

bool OptionsPanel::Update(f32 dt) {                   // orig: 0x00028960
    Options& o = app.options;
    sound::GameSound& snd = app.Sound();
    // the sliders follow the globals (FLD g; FCOM +0x10B4; TEST AH,0x44; JNP: unchanged)
    if (o.speech != speech) {
        speech = o.speech;
        UpdateSlider(sliderSpeech.Get(), speech);
    }
    if (o.music != music) {
        music = o.music;
        UpdateSlider(sliderMusic.Get(), music);
    }
    if (o.sfx != sfx) {
        sfx = o.sfx;
        UpdateSlider(sliderSfx.Get(), sfx);
    }
    // the test rumble blocks the rest while it runs
    if (double(rumble) > 0.0) {                       // FCOMP 0.0; TEST AH,0x41; JNE skip: only on >
        rumblePrev = rumble;                          // +0x1094
        const double r = double(rumble) - double(g_frameStep);   // FSUB dt; FST +0x1090 (the unrounded value tested)
        rumble = F(r);
        if (!(r > 0.0)) {                             // TEST AH,0x41; JP: on < or ==
            rumble = 0, rumblePrev = 0;
            app.ActivePad()->SetRumble(0.0f, 0.0f, false);   // Input_GetActivePad, Pad::SetRumble(0, 0, 0)
            app.input.SetRumbleEnabled(false, 0.0f);  // Input_SetRumbleEnabled(0, 0)
            app.input.Update(g_frameStep, app.services.readPad);   // Input_Update 0x0009BEA0
            return true;
        }
        app.ActivePad()->SetRumble(rumblePrev, rumblePrev, false);
        app.input.Update(g_frameStep, app.services.readPad);
        return true;
    }
    ShowSwitches();
    if (opening) {
        t = F(double(dt) + t);                        // FLD dt; FADD +0x18; FSTP
        Show();                                       // vf18
    }
    if (closing) {
        t = F(double(dt) + t);
        Hide();                                       // vf1C
    }
    if (!active) return true;
    if (delay != 0) {
        --delay;
        return true;
    }
    MemoryCards& mc = *app.memoryCards;
    if (saveRunning) {                                // +0x109C: the options save flow
        if (!mc.flag8) return true;                   // (App+0xF4)+8: set by XBox_OS_ExitPoint's exit
        mc.flag8 = false;
        saveRunning = false;
        if (mc.flag9) RestoreBackup();                // (MC+9 is only ever written 0)
        haveBackup = false;                           // +0x24 = 0, +0x1028 = 0
        backup.clear();
        Hide();
        return true;
    }
    if (discardAsked) {                               // +0x109E: the CB_* confirm (MEMCARD_036)
        if (mc.mode == 0) {                           // (App+0xF4)+0x70: 0 yes (discard)
            discardAsked = false;
            SetScreen(app, shownX, shownY);           // 0x000A1A60(+0x10AC, +0x10B0)
            RestoreBackup();                          // 0x00027230
            haveBackup = false;
            backup.clear();
            Hide();
            return true;
        }
        if (mc.mode == 1) discardAsked = false;       // 1 no (stay); 2 running
        return true;
    }
    auto modeOk = [] { return g_menuInputMode == 3 || g_menuInputMode == 0; };   // 0x00029F80
    if (MenuInput(app, 5)) {                          // back
        if (!modeOk()) return true;
        if (Busy()) {                                 // 0x00029880
            delay = 10;
            return true;
        }
        if (selection == 5 && adjusting) {            // leave Adjust Screen, undoing it
            SetScreen(app, adjustX, adjustY);         // 0x000A1A60(+0x10E8, +0x10EC)
            StopAdjusting();
            delay = 10;
            return true;
        }
        if (Changed()) {
            mc.StartConfirm("MEMCARD_036");           // 0x000E4F50(0x00294FE0)
            discardAsked = true;
            return true;
        }
        // (App+0x100 Mem_ReturnTrue: the hint, a no-op)
        o.dirty = false;                              // DAT_002E79A8 = 0
        Hide();
        delay = 10;
        return true;
    }
    if (MenuInput(app, 4)) {                          // accept
        if (!modeOk()) return true;
        if (Busy()) {
            delay = 10;
            return true;
        }
        if (selection == 6) {                         // Restore Defaults
            (void)snd.PlaySfx(selectSound, false);    // +0x10C4
            RestoreDefaults(app);
            speech = o.speech, sfx = o.sfx, music = o.music;
            snd.SetCategoryVolume(0, sfx);            // vf28 (+0x10B8)
            snd.SetCategoryVolume(2, music);          // vf38 (+0x10BC)
            snd.SetCategoryVolume(1, speech);         // vf30 (+0x10B4)
            ni::StartAnimations(sliderMusic.Get());   // 0x0013EF40: music, SFX, speech
            ni::StartAnimations(sliderSfx.Get());
            ni::StartAnimations(sliderSpeech.Get());
            if (sliderSpeech) sliderSpeech->UpdateDownwardPass(speech, true);   // Update speech, SFX, music
            if (sliderSfx) sliderSfx->UpdateDownwardPass(sfx, true);
            if (sliderMusic) sliderMusic->UpdateDownwardPass(music, true);
            ni::StopAnimations(sliderMusic.Get());    // 0x0013F090: music, speech, SFX
            ni::StopAnimations(sliderSpeech.Get());
            ni::StopAnimations(sliderSfx.Get());
            ShowSwitches();
            delay = 10;
        } else if (selection == 7) {                  // Apply
            bool started = false;
            if (app.useMemoryCardFlows && Changed()) {   // App+0xF8 && 0x000271B0
                started = mc.StartOptionsSave();      // 0x000E4D40
                saveRunning = started;                // +0x109C
            }
            if (!started) {
                o.dirty = false;                      // (the hint no-op first)
                Hide();
                delay = 10;
            }
        }
        if (selection == 5) {                         // Adjust Screen
            if (adjusting) {
                StopAdjusting();                      // (the new offset kept)
                delay = 10;
                return true;
            }
            adjustX = o.screenX, adjustY = o.screenY; // +0x10E8 / +0x10EC
            SetScreen(app, o.screenX, o.screenY);
            StartAdjusting();
            delay = 10;
        }
        return true;
    }
    auto moveSelection = [&](bool up) {
        // the old row's switch child 0 (when it has children), the new one's child 1 (when it has two)
        const s32 old = selection;
        selection = up ? (selection > 0 ? selection - 1 : 7) : (selection < 7 ? selection + 1 : 0);
        SetIndex(items[size_t(old)].Get(), 0);
        SetIndex(items[size_t(selection)].Get(), 1);
        (void)snd.PlaySfx(scrollSound, false);        // +0x10C0
        delay = 10;
        // 0x000272A0: the row's hint (a no-op)
    };
    if (MenuInput(app, 0)) {                          // up
        if (!modeOk()) return true;
        if (adjusting) {
            MoveScreenY(app, -2.0f);                  // 0x000A1B50
            delay = 4;
            return true;
        }
        moveSelection(true);
        return true;
    }
    if (MenuInput(app, 1)) {                          // down
        if (!modeOk()) return true;
        if (adjusting) {
            MoveScreenY(app, 2.0f);                   // 0x000A1B60
            delay = 4;
            return true;
        }
        moveSelection(false);
        return true;
    }
    const f32 kStep = std::bit_cast<f32>(0x3D4CCCCDu);   // [0x00293BDC] 0.05
    const f32 kMax = std::bit_cast<f32>(0x3F733333u);    // [0x00294FDC] 0.95
    auto up = [&](f32& v) {                           // FLD v; FADD step; FST v; FCOMP 0.95; TEST AH,0x41; JNE
        const double s = double(v) + kStep;
        v = F(s);
        if (s > kMax) v = kMax;
    };
    auto down = [&](f32& v) {                         // FLD v; FSUB step; FST v; FCOMP 0.0; TEST AH,5; JP
        const double s = double(v) - kStep;
        v = F(s);
        if (s < 0.0) v = 0.0f;
    };
    // REPE CMPSB of the current context's name (App+0xBC +4) with "Frontend" over 9 bytes (its NUL included)
    auto inFrontend = [&] { return app.Current() && app.Current()->name == "Frontend"; };
    auto sfxRow = [&](bool right) {
        right ? up(sfx) : down(sfx);
        UpdateSlider(sliderSfx.Get(), sfx);
        SetSfxVolume(app, sfx);                       // 0x000A1850
        snd.SetCategoryVolume(0, sfx);                // vf28
        delay = 5;
        if (!picker->SfxPlaying()) picker->PreviewSfx();   // 0x00027090 / 0x00027380
    };
    auto musicRow = [&](bool right) {
        right ? up(music) : down(music);
        UpdateSlider(sliderMusic.Get(), music);
        SetMusicVolume(app, music);                   // 0x000A1820
        snd.SetCategoryVolume(2, music);              // vf38
        delay = 5;
        if (!inFrontend() && !snd.VoiceExists(musicVoice))   // (in the pause only)
            musicVoice = snd.PlayMusic(musicClipSound, false);   // +0x10E0
    };
    auto speechRow = [&](bool right) {
        right ? up(speech) : down(speech);
        UpdateSlider(sliderSpeech.Get(), speech);
        SetSpeechVolume(app, speech);                 // 0x000A1880
        snd.SetCategoryVolume(1, speech);             // vf30
        delay = 5;
        if (!picker->SpeechPlaying()) picker->PreviewSpeech();   // 0x000270B0 / 0x000273E0
    };
    const s32 kOn = 0, kOff = 1;                      // (the switches' children)
    if (MenuInput(app, 3)) {                          // right
        if (!modeOk()) return true;
        if (adjusting) {
            MoveScreenX(app, 2.0f);                   // 0x000A1B40
            delay = 4;
            return true;
        }
        if (selection == 0) sfxRow(true);
        if (selection == 1) musicRow(true);
        if (selection == 2) speechRow(true);
        if (selection == 3) {                         // vibration On -> Off
            delay = 10;
            if (vibrationSwitch && vibrationSwitch->index == kOn) {
                SetVibration(app, false);             // 0x000A18B0(0)
                app.input.SetRumbleEnabled(false, 0.0f);
                SetIndex(vibrationSwitch.Get(), kOff);
                (void)snd.PlaySfx(toggleOkSound, false);
            } else {
                (void)snd.PlaySfx(toggleErrorSound, false);
            }
        }
        if (selection == 4) {                         // invert On -> Off
            delay = 10;
            if (invertSwitch && invertSwitch->index == kOn) {
                SetInvert(app, false);                // 0x000A18D0(0)
                SetIndex(invertSwitch.Get(), kOff);
                (void)snd.PlaySfx(toggleOkSound, false);
            } else {
                (void)snd.PlaySfx(toggleErrorSound, false);
            }
        }
        return true;
    }
    if (!MenuInput(app, 2)) return true;              // left
    if (!modeOk()) return true;
    if (adjusting) {
        MoveScreenX(app, -2.0f);                      // 0x000A1B30
        delay = 4;
        return true;
    }
    if (selection == 2) speechRow(false);             // (left tests the rows in the order 2, 1, 0, 3, 4)
    if (selection == 1) musicRow(false);
    if (selection == 0) sfxRow(false);
    if (selection == 3) {                             // vibration Off -> On, with a 1 s test rumble
        delay = 10;
        if (vibrationSwitch && vibrationSwitch->index == kOff) {
            app.input.SetRumbleEnabled(true, 0.0f);   // Input_SetRumbleEnabled(1, 0)
            SetVibration(app, true);                  // 0x000A18B0(1)
            app.input.SetRumbleEnabled(true, 0.0f);
            rumblePrev = 1.0f, rumble = 1.0f;         // +0x1094 / +0x1090
            app.ActivePad()->SetRumble(1.0f, 1.0f, false);
            SetIndex(vibrationSwitch.Get(), kOn);
            (void)snd.PlaySfx(toggleOkSound, false);
        } else {
            (void)snd.PlaySfx(toggleErrorSound, false);
        }
    }
    if (selection == 4) {                             // invert Off -> On
        delay = 10;
        if (invertSwitch && invertSwitch->index == kOff) {
            SetInvert(app, true);                     // 0x000A18D0(1)
            SetIndex(invertSwitch.Get(), kOn);
            (void)snd.PlaySfx(toggleOkSound, false);
        } else {
            (void)snd.PlaySfx(toggleErrorSound, false);
        }
    }
    return true;
}

// ---- the front end's panel -------------------------------------------------------------------------------------

FrontendOptionsPanel::FrontendOptionsPanel(App& a, FrontendContext& f) : OptionsPanel(a), fe(f) {   // 0x000210A0
    // the base 0x000286E0: the sliders' values from the globals, the rest 0
    speech = app.options.speech, sfx = app.options.sfx, music = app.options.music;
    text = nullptr;                                   // +0x108C = the Frontend's +0x98 (set by the caller)
    goBackSound = app.LoadSound("sounds/menu/go_back.wav", 0);   // +0x10F8 (0x002940CC)
    goToSound = app.LoadSound("sounds/menu/go_to.wav", 0);       // +0x10F4 (0x0029410C)
}

FrontendOptionsPanel::~FrontendOptionsPanel() {      // orig: 0x00021130 (then the base 0x000275B0)
    app.Sound().Release(goBackSound);                 // Sound_Release(+0x10F8), (+0x10F4)
    app.Sound().Release(goToSound);
}

void FrontendOptionsPanel::Show() {                   // orig: 0x000211A0
    if (!opening) {
        fe.flowBusy = true;                           // FE+0x2E = 1
        fe.MoveCamera(2);                             // 0x00023940(2): Camera_Options_there
        opening = true;
        (void)app.Sound().PlaySfx(goToSound, false);  // +0x10F4
        return;
    }
    if (!fe.cameraMoving) {                           // FE+0x6C
        opening = false;
        Backup();                                     // 0x00027150
        Activate();                                   // vf20
    }
}

void FrontendOptionsPanel::Hide() {                   // orig: 0x00021200
    if (!closing) {
        fe.flowBusy = false;                          // FE+0x2E = 0
        Deactivate();                                 // vf24
        fe.currentPanel = fe.mainPanel.get();         // FE+0x34 = FE+0x1C
        fe.MoveCamera(3);                             // Camera_Options_back
        (void)app.Sound().PlaySfx(goBackSound, false);   // +0x10F8
        closing = true;
    }
    if (!fe.cameraMoving) {
        closing = false;
        fe.mainPanel->Show();                         // (FE+0x34)->vf18
    }
}

void FrontendOptionsPanel::Activate() {               // orig: 0x00021290
    active = true;
    SetIndex(items[size_t(selection)].Get(), 1);
    // 0x000272A0 (the row's hint) and HINTS_012 (0x0029428C): no-ops
    KeepShownScreen();                                // 0x00027440
    Backup();                                         // 0x00027150
}

void FrontendOptionsPanel::Deactivate() {             // orig: 0x000212F0
    active = false;
    SetIndex(items[size_t(selection)].Get(), 0);
    // (App+0x100 Mem_ReturnTrue: the hint)
}

// ---- the pause's options panel --------------------------------------------------------------------------------------

PauseOptionsPanel::PauseOptionsPanel(App& a, PauseMenu& p, text::TextObject* tx) : OptionsPanel(a), pause(p) {   // 0x000A7FA0
    // the base 0x000286E0(0, text): the sliders' values from the globals, the rest 0
    speech = app.options.speech, sfx = app.options.sfx, music = app.options.music;
    text = tx;                                        // +0x108C = the pause's +0x54
    selection = 0;                                    // +8
    t = std::bit_cast<f32>(0x3F555555u);              // +0x18 = 0.8333333
    goToSound = app.LoadSound("sounds/menu/pause_go_to.wav", 0);      // +0x10F4 (0x002A1DF8)
    goBackSound = app.LoadSound("sounds/menu/pause_go_back.wav", 0);  // +0x10F8 (0x002A1DD8)
    OptionsPanel::Reset();                            // 0x00027460 (the picker's two draws)
}

PauseOptionsPanel::~PauseOptionsPanel() {             // orig: 0x000A7EE0
    app.Sound().Release(goToSound);                   // Sound_Release(+0x10F4), (+0x10F8)
    app.Sound().Release(goBackSound);
    ReleaseSounds();                                  // 0x00027250 (then the base 0x000275B0)
}

void PauseOptionsPanel::Reset() {                     // orig: vf0C 0x000A7F50
    goToSound = app.LoadSound("sounds/menu/pause_go_to.wav", 0);
    goBackSound = app.LoadSound("sounds/menu/pause_go_back.wav", 0);
    OptionsPanel::Reset();                            // 0x00027460
}

void PauseOptionsPanel::Activate() { Backup(); }      // orig: 0x000A7F90 (0x00027150)

void PauseOptionsPanel::Show() {                      // orig: 0x000A8060
    if (!opening) {
        shownX = app.options.screenX;                 // +0x10AC = DAT_002E79A0
        shownY = app.options.screenY;                 // +0x10B0 = DAT_002E79B4
        t = std::bit_cast<f32>(0x3F555555u);          // 0.8333333
        opening = true;
        // the switches from the raw flags (MOVZX; -1 <= v < the children: +0xE0 = v; the Update rewrites them)
        SetIndex(vibrationSwitch.Get(), app.options.vibration ? 1 : 0);
        SetIndex(invertSwitch.Get(), app.options.invert ? 1 : 0);
        SetIndex(items[size_t(selection)].Get(), 1);  // (+0x1064)[+8]
        (void)app.Sound().PlaySfx(goToSound, false);  // +0x10F4
        active = true;                                // +4 (already during the fold)
        return;
    }
    if (!(double(t) > std::bit_cast<f32>(0x3F855555u))) return;   // FCOMP [0x002A1E70] 1.0416666; TEST AH,0x41; JNE
    opening = false;
    Activate();                                       // vf20
    pause.main.Deactivate();                          // ((+0x10F0)+0x18)->vf24
    // 0x000272A0 (the row's hint) and HINTS_012 (0x0029428C): no-ops
    active = true;
}

void PauseOptionsPanel::Hide() {                      // orig: 0x000A8160
    if (!closing) {
        t = std::bit_cast<f32>(0x3FA00000u);          // 1.25
        closing = true;
        active = false;
        SetIndex(items[size_t(selection)].Get(), 0);
        (void)app.Sound().PlaySfx(goBackSound, false);   // +0x10F8
        // (App+0x100 Mem_ReturnTrue)
        delay = 10;
        return;
    }
    if (!(double(t) > std::bit_cast<f32>(0x3FBAAAABu))) return;   // FCOMP [0x002A1E74] 1.4583334
    closing = false;
    active = false;
    pause.main.Activate();                            // ((+0x10F0)+0x18)->vf20
    delay = 10;
}

}  // namespace fut::game::app

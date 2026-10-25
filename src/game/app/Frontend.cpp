// The front end; see Frontend.h.
#include "game/app/Frontend.h"

#include "game/level/LevelData.h"
#include "game/level/Session.h"
#include "game/text/LanguageDb.h"

#include "core/CrtRand.h"
#include "engine/ni/NiMath.h"
#include "engine/ni/NiRenderer.h"
#include "game/GameTime.h"
#include "engine/ni/NiCull.h"
#include "game/app/Contexts.h"
#include "game/app/ErrorOverlay.h"
#include "game/app/Extras.h"
#include "game/app/LoadPanel.h"
#include "game/app/MemoryCards.h"
#include "game/app/MenuScene.h"
#include "game/app/OptionsPanel.h"
#include "game/defs/Defs.h"
#include "game/text/TextObject.h"

#include <bit>
#include <cstdio>
#include <cstring>

namespace fut::game::app {

using ni::detail::F;

double SceneSequence::Length() const {
    if (!seq) return 0.0;                             // FLD [0x00293AB8] 0.0
    const ni::NiTimeController* c = seq->ReferenceController();   // +0x48[+0x90]
    if (!c) return 0.0;
    return (double(c->hiKeyTime) - c->loKeyTime) / c->frequency;  // FLD +0x20; FSUB +0x1C; FDIV +0x14
}

bool MenuInput(App& app, s32 action) {                // orig: 0x00029FA0
    input::Pad* pad = app.ActivePad();
    if (!pad || pad->Disconnected() != 0) return false;
    switch (action) {
    case 0: return pad->IsDownUnlatched(input::kDPadUp) || pad->StickY(0) > 0.4f;      // [0x00293B28]; TEST AH,0x41
    case 1: return pad->IsDownUnlatched(input::kDPadDown) || pad->StickY(0) < -0.4f;   // [0x002951DC]; TEST AH,5; JNP
    case 2: return pad->IsDownUnlatched(input::kDPadLeft) || pad->StickX(0) < -0.4f;
    case 3: return pad->IsDownUnlatched(input::kDPadRight) || pad->StickX(0) > 0.4f;
    case 4: return pad->JustPressedUnlatched(input::kButtonA) || pad->JustPressedUnlatched(input::kStart);
    case 5: return pad->JustPressedUnlatched(input::kButtonB) || pad->JustPressedUnlatched(input::kBack);
    case 6: return pad->JustPressedUnlatched(input::kStart);
    case 7: return pad->IsDownUnlatched(input::kButtonX);
    default: return false;
    }
}

void PickPreviewSamples(App* app) {                   // orig: 0x00027460 (the picker new(0x70) 0x00026BF0 once)
    s32 i = crt::Rand() % 10;                         // 0x000270D0: repeated while equal to +0x50 (never written: 0)
    while (i == 0) i = crt::Rand() % 10;
    s32 j = crt::Rand() % 8;                          // 0x00027110 (the signed AND 0x80000007 idiom; rand() >= 0)
    while (j == 0) j = crt::Rand() % 8;
    if (!app) return;
    // the samples' Sound_Load (+0x68 the sfx 6fr8-<i + 1>, +0x6C the voice j) and the seven menu sounds (scroll,
    // select, toggleOK, toggleError, speechloop, sfxloop, MusicClip): sound (the App's loader)
    (void)app->LoadSound("sounds/menu/scroll.wav", 0);
}

// ---- the main panel -----------------------------------------------------------------------------------------------

void FrontendMainPanel::Highlight(ni::NiSwitchNode* item, bool on) {
    if (!item) return;                                // (the original reads the item without a test)
    const s32 count = s32(item->Children().size());   // +0xCC
    if (on ? count > 1 : count > 0) item->index = on ? 1 : 0;   // +0xE0
}

void FrontendMainPanel::Build(ni::NiNode* scene) {    // orig: 0x00020320
    static const char* const kItems[4] = {"main_newgamenull", "main_optionsnull", "main_continuenull", "main_extras"};
    for (size_t i = 0; i < items.size(); ++i) items[i] = FindAs<ni::NiSwitchNode>(scene, kItems[i]);
    selection = 0;                                    // +8
    App& app = fe.App_();                             // Sound_Load(.., 0): scroll +0x38, select +0x3C, go_to +0x40
    scrollSound = app.LoadSound("sounds/menu/scroll.wav", 0);
    selectSound = app.LoadSound("sounds/menu/select.wav", 0);
    extraSound = app.LoadSound("sounds/menu/go_to.wav", 0);
}

FrontendMainPanel::~FrontendMainPanel() {             // orig: 0x00020270 (vf00 0x000208C0)
    for (size_t i = 0; i < 3; ++i) items[i] = nullptr;   // +0x20..+0x28 (the loop's count is 3; +0x2C goes with the
                                                      //  array's destructor 0x00166E30)
    sound::GameSound& snd = fe.App_().Sound();        // Sound_Release +0x38, +0x3C, +0x40
    for (u32 h : {scrollSound, selectSound, extraSound}) snd.Release(h);
}

void FrontendMainPanel::Activate() {                  // orig: 0x00020150
    active = true;
    // (the hint no-op 0x0001FF60)
    Highlight(items[size_t(selection)].Get(), true);  // the current item's +0xE0 = 1
}

void FrontendMainPanel::Deactivate() {                // orig: 0x00020180
    active = false;
    Highlight(items[size_t(selection)].Get(), false); // +0xE0 = 0
    // (the hint no-op)
}

void FrontendMainPanel::Show() {                      // orig: 0x0001FF10
    if (!showFlag) showFlag = true;
    if (!fe.cameraMoving) showFlag = false;
    Activate();                                       // (both branches tail-jump to vf20)
}

void FrontendMainPanel::Select(s32 sel) {             // orig: 0x00020000
    if (fe.guard) return;                             // Frontend+0x80
    App& app = fe.App_();
    switch (sel) {
    case 0:                                           // New Game
        (void)app.Sound().PlaySfx(selectSound, false);   // Sound_PlaySFX(+0x3C select, 0)
        countdown = 5;                                // +0x48
        return;
    case 1:                                           // Options: +0x34 = +0x24
        fe.currentPanel = fe.optionsPanel.get();
        Hide();                                       // vf1C
        fe.optionsPanel->Show();                      // (+0x34)->vf18
        return;
    case 2:                                           // Load: +0x34 = +0x28; App+0xF8 (1): the load flow 0x000E5170
        fe.currentPanel = fe.loadPanel.get();
        if (app.useMemoryCardFlows) app.memoryCards->StartLoad();
        Hide();                                       // vf1C
        fe.loadPanel->Show();                         // (+0x34)->vf18
        return;
    case 3:                                           // Extras: +0x34 = +0x20
        fe.currentPanel = fe.extrasPanel.get();
        Hide();                                       // vf1C
        fe.extrasPanel->Show();                       // (+0x34)->vf18
        return;
    default:                                          // (the current panel's Show only: the main panel's)
        if (fe.currentPanel == this) Show();
        return;
    }
}

bool FrontendMainPanel::Countdown() {                 // orig: 0x00020090
    if (--countdown == 0) {
        App& app = fe.App_();
        app.RequestContext("Ingame", 0x000200A8);
        app.SetLevelName("Level1-1");                 // 0x00294160
        fe.stay = false;                              // Frontend+0x4C
        countdown = 0;
        return false;
    }
    Hide();                                           // vf1C
    return true;
}

bool FrontendMainPanel::StartNextCheck() {            // orig: 0x000200F0
    App& app = fe.App_();
    if (g_feExtrasChain == 0) {
        app.memoryCards->StartLoadExtras();           // 0x000E4EA0
        g_feExtrasChain = 1;
        return true;
    }
    if (g_feCheckChain == 0 && pendingSelection == 0) {
        app.memoryCards->StartBootCheck();            // 0x000E50C0
        g_feCheckChain = 1;
        return true;
    }
    return false;
}

void FrontendMainPanel::ContinueChain() {             // orig: 0x000201B0
    if (g_feOptionsChain != 1 && g_feCheckChain != 1 && g_feExtrasChain != 1) return;   // (DAT_002D9DE4..2D9DEC)
    App& app = fe.App_();
    if (app.memoryCards->running) return;             // (App+0xF4)+0xA
    if (g_feOptionsChain == 1) {
        g_feOptionsChain = 2;
        if (!StartNextCheck()) Select(pendingSelection);
        return;
    }
    if (g_feExtrasChain == 1) {
        g_feExtrasChain = 2;
        if (!StartNextCheck()) Select(pendingSelection);
        return;
    }
    g_feCheckChain = 2;                               // (the remaining flag is 1)
    if (StartNextCheck()) return;
    Select(pendingSelection);
    if (pad) app.input.activePad = pad;               // g_activePad = +0x30
}

bool FrontendMainPanel::Update(f32 dt) {              // orig: 0x00020580
    (void)dt;
    if (countdown > 0) return Countdown();
    if (!active) return true;
    if (delay != 0) {
        --delay;
        return true;
    }
    App& app = fe.App_();
    ContinueChain();
    if (MenuInput(app, 0)) {                          // up
        if (g_menuInputMode != 0) return true;        // 0x00029F80
        Highlight(items[size_t(selection)].Get(), false);   // the old item off, the new one on
        selection = selection > 0 ? selection - 1 : 3;
        Highlight(items[size_t(selection)].Get(), true);
    } else if (MenuInput(app, 1)) {                   // down
        if (g_menuInputMode != 0) return true;
        Highlight(items[size_t(selection)].Get(), false);
        selection = selection < 3 ? selection + 1 : 0;
        Highlight(items[size_t(selection)].Get(), true);
    } else {
        if (!MenuInput(app, 4)) return true;          // accept
        if (app.memoryCards->running) return true;
        if (g_menuInputMode != 0) return true;
        if (fe.cameraMoving) {                        // Frontend+0x6C
            delay = 10;
            return true;
        }
        if (input::Pad* p = app.input.activePad)      // g_activePad
            p->Latch(p->IsDownUnlatched(input::kButtonA) ? input::kButtonA : input::kStart);
        if (g_feOptionsChain != 0 && g_feCheckChain != 0 && g_feExtrasChain != 0) {
            Select(selection);
            return true;
        }
        pad = app.input.activePad;                    // +0x30
        MemoryCards& mc = *app.memoryCards;
        if (mc.optionsFound && !mc.optionsLoaded) {   // DAT_002ED258 && !DAT_002ED259
            pendingSelection = selection;
            mc.StartLoadOptions();
            g_feOptionsChain = 1;
            return true;
        }
        if (g_feExtrasChain == 0) {
            pendingSelection = selection;
            g_feExtrasChain = 1;
            mc.StartLoadExtras();
            return true;
        }
        if (g_feCheckChain == 0 && selection == 0) {
            pendingSelection = 0;
            g_feCheckChain = 1;
            mc.StartBootCheck();
            return true;
        }
        Select(selection);
        return true;
    }
    (void)app.Sound().PlaySfx(scrollSound, false);    // up / down: Sound_PlaySFX(+0x38 scroll, 0)
    delay = 10;                                       // +0x14
    // (the hint no-op 0x0001FF60)
    return true;
}

// ---- the context --------------------------------------------------------------------------------------------------

FrontendContext::FrontendContext(App& app) : Context(app, "Frontend") {}   // orig: 0x00022650
FrontendContext::~FrontendContext() {                 // (the App's teardown: the original never deletes its contexts)
    mainPanel.reset();                                // (the panels first: the extras panel deletes the extras menus)
    extrasPanel.reset();
    optionsPanel.reset();
    loadPanel.reset();
}

bool FrontendContext::OnEnter() {                     // orig: 0x000218F0
    flowBusy = false;                                 // +0x2E
    if (loaded) return true;                          // +0x40
    loadState = 0;                                    // +0x1C4
    // the loading screen new(0x58) 0x000EFBF0("images\<lang>\Loadmenu.nif", 0, 1, 1) -> +0x1C8 (TextManager::
    // FormatLocalized; its image, spinner and camera with the App's renderer)
    loadingScreen = true;
    if (app_.services.renderer) {
        const std::string lang = app_.text ? app_.text->Language() : std::string("English");
        screen_ = std::make_unique<LoadingScreen>(app_.services.renderer, &app_.services.readFile,
                                                  "images\\" + lang + "\\Loadmenu.nif",
                                                  "images\\" + lang + "\\default.nif");
    }
    app_.ActivePad()->SetRumble(0, 0, false);         // Pad::SetRumble(0, 0, 0)
    app_.input.SetRumbleEnabled(false, 0.0f);         // Input_SetRumbleEnabled(0, 0)
    app_.input.Update(g_frameStep, app_.services.readPad);   // Input_Update 0x0009BEA0
    g_menuInputMode = 0;                              // 0x00029F90(0)
    // (the profiler counter DAT_002E8DDC, QueryCounter: unused)
    time = 0;                                         // +0x78
    field_e8 = 0;                                     // +0xE8
    idleAnimEnd = 0;                                  // +0xE4
    idleTime = 0;                                     // +0x7C
    volumeBackup = 0;                                 // +0x74
    musicSound = app_.LoadSound("Sounds/Music/01 - front-end.wav", 1);   // +0x1AC
    speechSound = app_.LoadSound("Sounds/Localized/English/menu/6br06-01.wav", 0);   // +0x1B0 (FormatLocalized)
    root_ = new ni::NiNode;                           // +0x48 = new(0xE0) NiNode (the scene root)
    return true;
}

void FrontendContext::LoadStep() {                    // orig: 0x00022A00
    if (!loadingScreen) return;
    if (loadState == 0) {                             // the stream request new(0x28) 0x000D2160(App, client +0x14)
        request = app_.streams.Create(*this, "Frontend", "-");
        loadState = 1;
        return;
    }
    if (loadState != 7) return;
    request.reset();                                  // +0x1CC released
    loadingScreen = false;                            // +0x1C8 deleted
    screen_.reset();
    loaded = true;                                    // +0x40
    if (!preloaded) Start();                          // +0x2C
    preloaded = false;
}

bool FrontendContext::OnStream(stream::Request& r) {  // orig: 0x00025410 (this = Frontend + 0x14)
    switch (loadState) {                              // +0x1B0 of the client = Frontend+0x1C4
    case 1:
        r.SetPath("menu\\frontend.nif");
        loadState = 3;
        return true;
    case 3:
        if (ni::NiStream* s = r.Stream()) SetupScene(*s);   // 0x00024DD0
        r.SetPath("menu\\extras\\thumbs.nif");
        loadState = 6;
        return true;
    case 6:
        if (ni::NiStream* s = r.Stream()) LoadExtrasThumbs(app_, *s);   // 0x0001CF00(stream): the thumbnails
        r.SetPath("menu/extras/Extra_A.nif");
        loadState = 4;
        return true;
    case 4:
        if (ni::NiStream* s = r.Stream()) extrasGrid = std::make_unique<ThumbGrid>(app_, *s);   // new(0x110) 0x0001E130
        r.SetPath("menu/extras/Extra_B.nif");
        loadState = 5;
        return true;
    case 5:
        if (ni::NiStream* s = r.Stream()) extrasList = std::make_unique<MusicList>(app_, *s);   // new(0xB8) 0x0001F360
        [[fallthrough]];                              // (then as state 2)
    default:
        // the stream's objects released (0x001084A0), +0x1CC released
        loadState = 7;
        request.reset();
        return false;
    }
}

void FrontendContext::SetupScene(ni::NiStream& s) {   // orig: 0x00024DD0
    // The top-level objects: every NiAVObject without a parent goes under the scene root (vfA0(obj, 1)); the
    // sequences by name (REPE CMPSB: exact).
    ni::NiNode* root = root_.Get();                   // (+0x48, made by Enter)
    for (u32 link : s.TopLevelLinks()) {
        ni::NiObject* o = s.ResolveLink(link);
        if (auto* av = dynamic_cast<ni::NiAVObject*>(o); av && !av->parent && root) root->AttachChild(av, true);
        auto* seq = dynamic_cast<ni::NiSequence*>(o);
        if (!seq) continue;
        const std::string& n = seq->name;
        struct {
            const char* name;
            ni::NiPointer<ni::NiSequence>* slot;
        } const kNames[] = {
            {"Slurmcans_Drop", &slurmcans.seq},          {"Cigarrs_Drop", &cigarrs.seq},
            {"Furfist", &furfist.seq},                   {"Robothead_Drop", &robothead.seq},
            {"Daliclock_Drop", &daliclock.seq},          {"Kibblesandsnouts_Drop", &kibbles.seq},
            {"Camera_Newgame_there", &cameraMoves_[0]},  {"Camera_Newgame_back", &cameraMoves_[1]},
            {"Camera_Options_there", &cameraMoves_[2]},  {"Camera_Options_back", &cameraMoves_[3]},
            {"Camera_Continue_there", &cameraMoves_[4]}, {"Camera_Continue_back", &cameraMoves_[5]},
            {"Bubbles1", &bubbles[0].seq},               {"Bubbles2", &bubbles[1].seq},
            {"Bubbles3", &bubbles[2].seq},               {"Bubbles4", &bubbles[3].seq},
            {"Logotype", &logotype_},                    {"Roller1", &roller.seq},
            {"Smoke2", &smokes_[0].seq},                 {"Smoke3", &smokes_[1].seq},
            {"Smoke4", &smokes_[2].seq},
        };
        for (const auto& k : kNames)
            if (n == k.name) *k.slot = seq;
    }
    SetupCamera();                                    // 0x00023A30
    SetupManagers();                                  // 0x00023D10
    SetupText();                                      // 0x00021CD0
    SetupPanels();                                    // 0x00021FA0
    SetupBubbles();                                   // 0x000247A0
    SetupNodes();                                     // 0x00021DB0
    SetupFade();                                      // 0x00024AA0
    StartLogo();                                      // 0x000238D0
    for (SceneSequence& q : smokes_)                  // (+0x184)[i]->ActivateSequence(+0x190 [i], 0)
        if (q.manager && q.seq) q.manager->ActivateSequence(q.seq.Get(), 0);
    labels_ = std::make_unique<MenuLabels>(root_.Get(), camera_.Get());   // new(0x10) 0x00025710(+0x48, +0x68)
    labels_->Init(app_);                              // 0x00025850
    // the reseed: srand(_ftol(Seconds(QPC) * 5846.0)) (0x0016694F, its only caller)
    const double t = app_.Seconds(static_cast<s64>(app_.qpc));   // 0x000D2C20, 0x000D2C10
    app_.Srand(static_cast<u32>(static_cast<s32>(t * 5846.0f)), 0x000253E5);   // FMUL [0x00294558]; _ftol
    // (the hint text: Mem_ReturnTrue)
}

void FrontendContext::SetupCamera() {                 // orig: 0x00023A30
    camera_ = FindAs<ni::NiCamera>(root_.Get(), "Cam3");   // +0x68 (RTTI 0x002F9120)
    if (camera_) {                                    // (the original writes without a test)
        camera_->renderer = app_.services.renderer;   // +0x15C = the root (the Click's scene), +0x1A8 = App->vf14()
        camera_->UpdateDownwardPass(0.0f, true);      // Update(0, 1)
        camera_->UpdateProperties();
        camera_->UpdateEffects();
    }
    cameraNode_ = FindAs<ni::NiNode>(root_.Get(), "Cam2:Animation (Translational)");   // +0x64
    cameraManager_ = cameraNode_ ? new ni::NiKeyframeManager(cameraNode_.Get()) : nullptr;   // +0x17C
    if (!cameraManager_) return;
    cameraManager_->Update(0.0f);                     // vf38(0)
    for (const auto& m : cameraMoves_) SetSequenceLoop(m.Get(), false);   // 0x00022460(+0xF8.., 0)
    for (const auto& m : cameraMoves_)
        if (m) cameraManager_->AddSequence(m.Get(), true);   // AddSequence(seq, 1)
    cameraManager_->cumulativeAnimations = false;     // +0x4C = 0
}

void FrontendContext::SetupManagers() {               // orig: 0x00023D10
    // the drops' nodes (NiNode) and their keyframe managers (new(0xC0) 0x00130D20 on the node, no null test)
    auto drop = [this](SceneSequence& d, const char* nodeName) {
        d.node = FindAs<ni::NiNode>(root_.Get(), nodeName);
        d.manager = d.node ? new ni::NiKeyframeManager(d.node.Get()) : nullptr;
    };
    drop(slurmcans, "Slurmcans:Animation (Translational)");   // +0xA0 / +0xA4
    drop(daliclock, "daliclock:Animation (Translational)");   // +0xAC / +0xB0
    drop(cigarrs, "Cigarrs:Animation (Translational)");       // +0xDC / +0xE0
    drop(robothead, "Robothead");                              // +0xC4 / +0xC8
    drop(furfist, "Furfist:Animation (Translational)");       // +0xD0 / +0xD4
    drop(kibbles, "kibblesandsnouts");                         // +0xB8 / +0xBC
    for (SceneSequence* d : {&slurmcans, &kibbles, &robothead, &furfist, &cigarrs, &daliclock}) {
        SetSequenceLoop(d->seq.Get(), false);         // 0x00022460(seq, 0)
        if (d->manager) {
            if (d->seq) d->manager->AddSequence(d->seq.Get(), true);
            d->manager->cumulativeAnimations = false; // +0x4C = 0
        }
    }
    // the logo: FUTURAMA's manager +0x60; the sequence's start time -time (F(+0x78 * -1.0)); Update(0)
    ni::NiNode* logoNode = FindAs<ni::NiNode>(root_.Get(), "FUTURAMA");
    logoManager_ = logoNode ? new ni::NiKeyframeManager(logoNode) : nullptr;
    SetSequenceLoop(logotype_.Get(), false);
    if (logotype_) logotype_->offset = F(double(time) * -1.0f);   // FMUL [0x00293BF0]
    if (logoManager_) {
        if (logotype_) logoManager_->AddSequence(logotype_.Get(), true);
        logoManager_->Update(0.0f);                   // vf38(0)
    }
    // Roller1: its node +0x16C, manager +0x168, start time -time, activated; +0x170 = its length
    roller.node = FindAs<ni::NiNode>(root_.Get(), "Roller1");
    roller.manager = roller.node ? new ni::NiKeyframeManager(roller.node.Get()) : nullptr;
    if (roller.seq) roller.seq->offset = F(double(time) * -1.0f);
    if (roller.manager && roller.seq) {
        roller.manager->AddSequence(roller.seq.Get(), true);
        roller.manager->ActivateSequence(roller.seq.Get(), 0);
    }
    shotInterval = F(roller.Length());                // 0x00024568: +0x170
    // the smokes: group5 / group7 / group6 (+0x19C..), their managers +0x184.. with Smoke2-4 from -time, Update(0)
    smokes_[0].node = FindAs<ni::NiNode>(root_.Get(), "group5:Animation (Translational)");
    smokes_[1].node = FindAs<ni::NiNode>(root_.Get(), "group7:Animation (Translational)");
    smokes_[2].node = FindAs<ni::NiNode>(root_.Get(), "group6:Animation (Translational)");
    for (SceneSequence& q : smokes_) {
        q.manager = q.node ? new ni::NiKeyframeManager(q.node.Get()) : nullptr;
        if (q.seq) q.seq->offset = F(double(time) * -1.0f);
        if (!q.manager) continue;
        if (q.seq) q.manager->AddSequence(q.seq.Get(), true);
        q.manager->Update(0.0f);
    }
    if (root_) root_->UpdateDownwardPass(0.0f, true); // (+0x48)->Update(0, 1)
}

void FrontendContext::SetupText() {                   // orig: 0x00021CD0
    text_.reset(text::TextObject::Create(0, app_.services.readFile, app_.services.renderer));   // +0x98
    if (!text_) return;
    text_->field7A = 1;                               // +0x7A
    text_->UseScene(camera_.Get(), root_.Get());      // 0x00099BE0(+0x68, +0x48)
    text_->color = ni::NiColor{1, 1, 1};
    text_->SetRenderer(app_.services.renderer);       // 0x00098F80 (nothing in the scene mode)
    text_->SetScale(std::bit_cast<f32>(0x3FB33333u), std::bit_cast<f32>(0x40066666u));   // (1.4, 2.1)
    text_->style.vAlign = 0;                          // +0x64
    text_->style.hAlign = 0;                          // +0x60
    text_->SetZBufferTest(true);                      // 0x00099660(1)
    text_->style.letterSpacing = std::bit_cast<f32>(0x3BC49BA6u);   // +4 = 0.006
    text_->style.wordWrap = false;                    // +0x68
}

void FrontendContext::SetupPanels() {                 // orig: 0x00021FA0
    // the panels in this order: main new(0x50) ctor(0, this) + Build 0x00020320, load new(0x7C) 0x0001FBE0(3, this,
    // +0x98) + 0x0001AD60 (its +0xC = +0x64), extras new(0x48) ctor(2, this) + 0x00020DA0, options new(0x10FC)
    // 0x000210A0(2, this, +0x98) + vf28 Build (its preview samples: 0x00027460, two rand()); +0x34 = main
    mainPanel = std::make_unique<FrontendMainPanel>(*this);
    mainPanel->Build(root_.Get());
    loadPanel = std::make_unique<FrontendLoadPanel>(app_, *this, text_.get());   // new(0x7C) 0x0001FBE0(3, this, +0x98)
    loadPanel->BuildRows(root_.Get());                // 0x0001AD60(+0x48)
    loadPanel->node = cameraNode_;                    // +0xC = +0x64
    extrasPanel = std::make_unique<FrontendExtrasPanel>(*this);   // new(0x48) 0x00020FE0(2, this)
    extrasPanel->Build(root_.Get());                  // 0x00020DA0(+0x48)
    optionsPanel = std::make_unique<FrontendOptionsPanel>(app_, *this);   // new(0x10FC) 0x000210A0(2, this, +0x98)
    optionsPanel->text = text_.get();                 // +0x108C = +0x98
    optionsPanel->Build(camera_.Get(), root_.Get());  // vf28 0x000279C0(+0x48, +0x68) (its Reset: two rand())
    currentPanel = mainPanel.get();
}

void FrontendContext::SetupBubbles() {                // orig: 0x000247A0
    static const char* const kNodes[4] = {"Bubbles1:Animation (Translational)", "Bubbles2:Animation (Translational)",
                                          "Bubbles3:Animation (Translational)", "Bubbles4:Animation (Translational)"};
    for (size_t i = 0; i < bubbles.size(); ++i) bubbles[i].node = FindAs<ni::NiNode>(root_.Get(), kNodes[i]);   // +0x134..
    for (size_t i = 0; i < bubbles.size(); ++i) {
        SceneSequence& b = bubbles[i];
        bubbleField154[i] = 0;                        // +0x154 [i] = 0
        bubbleTimer[i] = F(b.Length());               // +0x144 [i] = the length
        b.manager = b.node ? new ni::NiKeyframeManager(b.node.Get()) : nullptr;   // +0x114 [i]
        if (b.seq) b.seq->offset = F(double(time) * -1.0f);
        SetSequenceLoop(b.seq.Get(), false);
        if (b.manager && b.seq) b.manager->AddSequence(b.seq.Get(), true);
        if (b.node) b.node->UpdateDownwardPass(0.0f, true);   // Update(0, 1)
    }
}

void FrontendContext::SetupNodes() {                  // orig: 0x00021DB0
    // DAT_002DB83C (the profiler) +0x2C under the root: no profiler is made (NOT PORTED, null)
    if (root_) {
        root_->UpdateDownwardPass(0.0f, true);        // Update(0, 1)
        root_->UpdateProperties();
        root_->UpdateEffects();
    }
    stay = true;                                      // +0x4C = 1
    mainPanel->Show();                                // (+0x1C)->vf18 (which activates it)
    alwaysNode_ = FindAs<ni::NiNode>(root_.Get(), "Allways_animated");   // +0x88
    if (alwaysNode_) alwaysNode_->UpdateDownwardPass(0.0f, true);
    smokeNode_ = FindAs<ni::NiNode>(root_.Get(), "Smokenelectra");       // +0x70
    logoNode_ = FindAs<ni::NiNode>(root_.Get(), "FUTURAMA");             // +0x50
    if (logoNode_) logoNode_->UpdateDownwardPass(0.0f, true);
    cameraMoving = false;                             // +0x6C
    shotTimer = 0;                                    // +0x174
    timer178 = 0;                                     // +0x178
}

void FrontendContext::SetupFade() {                   // orig: 0x00024AA0
    // the fade-in polygon +0x1BC: new(0x24) 0x00106DE0(4, (0,0,0) (1,0,0) (1,1,0) (0,1,0), no uvs, four (0,0,0,1)),
    // an alpha property (0xEC | 1) and a vertex colour property (source 1, lighting 0) in its slots, on the camera
    static const ni::NiPoint3 kQuad[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    static const ni::NiColorA kBlack[4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
    fade_ = new ni::NiScreenPolygon(4, kQuad, nullptr, kBlack);
    auto* a = new ni::NiAlphaProperty;                // new(0x1C): flags 0xEC
    a->flags = u16(a->flags | 1);
    auto* vc = new ni::NiVertexColorProperty;         // new(0x20) (+0x18 = 0, +0x1C = 1), then +0x1C = 0, +0x18 = 1
    vc->lightingMode = 0, vc->sourceVertexMode = 1;
    fade_->AttachProperty(a);
    fade_->AttachProperty(vc);
    if (camera_) camera_->AttachScreenPolygon(fade_.Get());   // 0x00019460
    fadePending = true;                               // +0x1B8 = 1
}

bool FrontendContext::MoveCamera(s32 n) {             // orig: 0x00023940
    if (cameraMoving) return false;                   // +0x6C
    cameraMoving = true;
    if (n >= 0 && n <= 5) {                           // (the jump table; above 5: no new move)
        if (n == 1 || n == 3 || n == 5) StartLogo();  // 0x000238D0
        cameraMove_ = cameraMoves_[size_t(n)];        // 0x00034250(&+0xF8 [n]) -> +0x110
    }
    // FLD 0.0; with +0x110: FSTP ST(0), its reference controller's (hi - lo) / frequency; FADD +0x78; FSTP +0x180
    const double len = cameraMove_ ? SceneSequence{cameraMove_}.Length() : 0.0;
    cameraEnd = F(len + double(time));
    if (cameraMove_) {                                // (the original writes +0x110's offset without a test)
        cameraMove_->offset = F(double(time) * -1.0f);   // +0xA8 = +0x78 * [0x00293BF0] (-1.0)
        if (cameraManager_) cameraManager_->ActivateSequence(cameraMove_.Get(), 0);   // (+0x17C) 0x0012F3A0
    }
    return true;
}

void FrontendContext::StartLogo() {                   // orig: 0x000238D0
    if (logotype_) logotype_->offset = 0.0f;          // +0xA8 = 0
    SetSequenceLoop(logotype_.Get(), false);
    if (logoManager_) {
        logoManager_->Update(0.0f);                   // vf38(0)
        if (logotype_) logoManager_->ActivateSequence(logotype_.Get(), 0);
    }
    logoEnd = F(SceneSequence{logotype_}.Length() + double(time));   // +0x5C
    logoTime = 0;                                     // +0x58
}

void FrontendContext::Fade() {                        // orig: 0x00022550
    if (!fade_ || fade_->colors.empty()) return;
    const f32 a = fade_->colors[0].a;                 // (+0x18)+0xC
    if (double(a) > std::bit_cast<f32>(0x3CA3D70Au)) {   // FCOM [0x00294360] 0.02; TEST AH,0x41; JP: only on >
        const f32 n = F(double(a) - std::bit_cast<f32>(0x3CA3D70Au));
        for (ni::NiColorA& c : fade_->colors) c.a = n;   // (the four alphas, FST)
        fade_->flags = u16(fade_->flags | 2);         // +0x1C |= 2
        return;
    }
    if (camera_) camera_->DetachScreenPolygon(fade_.Get());   // 0x00100FC0
    fade_ = nullptr;                                  // +0x1BC released
    fadePending = false;                              // +0x1B8 = 0
}

void FrontendContext::Start() {                       // orig: 0x00021A80
    if (ni::NiRenderer* r = app_.services.renderer) { // the clear colour black (vf6C) and a clear vf44(0, 1)
        r->SetBackgroundColor(ni::NiColor{0.0f, 0.0f, 0.0f});
        r->ClearBuffer(nullptr, 1);
    }
    app_.memoryCards->StartLoadOptions();             // 0x000E4DF0
    guard = true;                                     // +0x80
    guardCount = 30;                                  // +0x84
    (void)app_.Sound().PlaySpeech(speechSound, false);   // Sound_PlaySpeech(+0x1B0, 0)
    PlayMusic();                                      // (inline: 0x00021330's body)
    started = true;                                   // +0x2D
    if (labels_) labels_->Build(app_);                // (+0x38) 0x00025970
    mainPanel->Activate();                            // (+0x1C)->vf20
    stay = true;                                      // +0x4C
}

void FrontendContext::PlayMusic() {                   // orig: 0x00021330
    musicVoice = app_.Sound().PlayMusic(musicSound, true);   // Sound_PlayMusic(+0x1AC, 1) -> +0x1B4
    UpdateSound(app_.Sound(), app_.CurrentLevel());   // App+0xF0 vf04 (Update 0x000CBD40); vf08 (a nop)
}

void FrontendContext::StopMusic() {                   // orig: 0x0001B6F0
    if (!musicVoice) return;
    app_.Sound().StopVoice(musicVoice);               // Voice_Stop(+0x1B4)
    musicVoice = 0;
}

void FrontendContext::UpdateBubbles() {               // orig: 0x000223B0
    for (size_t i = 0; i < bubbles.size(); ++i) {
        if (bubbles[i].node) bubbles[i].node->UpdateDownwardPass(time, true);   // (+0x134 [i])->Update(time, 1)
        if (!(double(time) > double(bubbleTimer[i]))) continue;   // FCOMP; TEST AH,0x41; JNE
        if (bubbles[i].seq) bubbles[i].seq->offset = F(double(time) * -1.0f);   // FMUL [0x00293BF0]
        bubbleField154[i] = -1.0f;                    // +0x154 [i] = 0xBF800000
        const f32 len = F(bubbles[i].Length());       // FSTP [esp+0xC]
        const s32 r = crt::Rand() % 7 + 3;
        bubbleTimer[i] = F(double(r) + double(time) + double(len));   // FILD; FADD time; FADD len
        if (bubbles[i].manager && bubbles[i].seq) bubbles[i].manager->ActivateSequence(bubbles[i].seq.Get(), 0);
    }
}

void FrontendContext::BackgroundShot(f32 offset) {    // orig: 0x00022150
    const s32 r = crt::Rand() % 6;
    const SceneSequence* drop = nullptr;
    switch (r) {                                      // the jump table 0x00022394 (5 = 0)
    case 1: drop = &daliclock; break;
    case 2: drop = &kibbles; break;
    case 3: drop = &cigarrs; break;
    case 4: drop = &robothead; break;
    default: drop = &slurmcans; break;
    }
    running = *drop;                                  // +0xF0 = seq, +0xEC = node, +0xF4 = manager (0x00034250)
    idleAnimRunning = running.node.Get() != nullptr;
    idleAnimEnd = F(drop->Length() + double(time));   // (the length stays on the x87 stack)
    if (!running.seq) return;                         // (the original writes the sequence without a test)
    running.seq->offset = F(double(time) * -1.0f + double(offset));   // FLD time; FMUL -1; FADD offset
    if (running.manager) running.manager->ActivateSequence(running.seq.Get(), 0);
}

bool FrontendContext::Update() {                      // orig: 0x00022B30
    if (loadingScreen) {
        LoadStep();
        return true;
    }
    if (!started) return true;
    const f32 dt = g_frameStep;
    if (double(time) > 3600.0f) {                     // [0x00294378]; TEST AH,0x41; JNE
        time = 0;
        field_e8 = 0;
        idleAnimEnd = 0;
        idleTime = 0;
    }
    if (flowBusy) idleTime = 0;
    // (DAT_002E8DF4 0x000A32A0: returns true)
    if (double(idleTime) > 57.0f) {                   // [0x00294374]; TEST AH,0x41; JNE: the music lowered
        // the options' music volume DAT_002E79B8 kept in +0x74 once (FCOMP 0; TEST AH,0x44; JP: only when 0.0), then
        // the sound system's vf38 (the music category) = it - 1 / 180 ([0x00294370] = 0x3BB60B61), 0 below 0 (TEST
        // AH,5; JP: the unrounded difference, NaN kept)
        if (volumeBackup == 0.0f) volumeBackup = app_.options.music;
        const double d = double(app_.options.music) - double(std::bit_cast<f32>(0x3BB60B61u));
        f32 v = F(d);                                 // FST [esp+8]
        if (d < 0.0) v = 0.0f;
        app_.Sound().SetCategoryVolume(2, v);         // App+0xF0 vf38
    }
    if (double(idleTime) > 60.0f) {                   // [0x0029436C]: the Attract movie
        app_.Sound().SetCategoryVolume(2, volumeBackup);   // vf38(+0x74): the music's volume back
        if (app_.attract) app_.attract->SetReturn("Frontend");   // 0x0002BAD0
        app_.RequestContext("Attract", 0x00022C32);
        stay = false;
    }
    if (fadePending) Fade();                          // +0x1B8: 0x00022550
    if (guard) {
        const s32 old = guardCount;
        guardCount = old - 1;
        if (old <= 0) guard = false;
    }
    if (idleAnimRunning && double(time) > double(idleAnimEnd)) {   // the drop has ended
        if (running.node) running.node->UpdateDownwardPass(0.0f, true);   // Update(0, 1)
        if (running.manager) running.manager->DeactivateAll();   // 0x00131170
        running = SceneSequence{};                    // +0xEC / +0xF4 / +0xF0 released
        idleAnimRunning = false;
    }
    if (running.node) {                               // the running drop's world bound and its ancestors' (but the root's)
        running.node->UpdateWorldBound();             // vf98
        for (ni::NiNode* p = running.node->parent; p && p->parent; p = p->parent) p->UpdateWorldBound();
    }
    if (double(shotTimer) > double(shotInterval)) {   // FLD +0x174; FCOMP +0x170; TEST AH,0x41; JNE
        if (!idleAnimRunning) BackgroundShot(F(double(shotTimer) - shotInterval));
        shotTimer = F(double(shotTimer) - shotInterval);
    }
    if (cameraMoving) {                               // +0x6C: the camera's move
        if (cameraNode_) cameraNode_->UpdateDownwardPass(time, true);   // (+0x64)->Update(+0x78, 1)
        if (double(time) > double(cameraEnd)) {       // FLD +0x78; FCOMP +0x180; TEST AH,0x41; JNE: only on >
            if (cameraManager_) {
                cameraManager_->Update(0.0f);         // (+0x17C)->vf38(0)
                cameraManager_->DeactivateAll();      // 0x00131170
            }
            cameraMove_ = nullptr;                    // +0x110 released
            cameraMoving = false;
        }
    }
    UpdateBubbles();
    mainPanel->Update(dt);                            // (+0x1C)->vf04
    if (!loadPanel->Update(dt)) {                     // (+0x28)->vf04 0x0001B490 (false: the dead path's load)
        stay = false;                                 // +0x4C = 0
        app_.RequestContext("Ingame", 0x00022DF0);
    }
    optionsPanel->Update(dt);                         // (+0x24)->vf04 0x00028960
    extrasPanel->Update(dt);                          // (+0x20)->vf04 0x00020B60
    if (AllPadsIdle()) idleTime = F(double(dt) + idleTime);   // Input_AllPadsIdle 0x0009BE70
    else idleTime = 0;
    time = F(double(dt) + time);
    shotTimer = F(double(dt) + shotTimer);
    timer178 = F(double(dt) + timer178);
    if (double(logoEnd) > double(time)) logoTime = F(double(dt) + logoTime);
    return stay;
}

bool FrontendContext::AllPadsIdle() const {           // orig: 0x0009BE70
    for (input::Pad* p : app_.input.pads)
        if (p && !p->IsIdle()) return false;
    return true;
}

void FrontendContext::Render() {                      // orig: 0x00021B60
    if (loadingScreen) {                              // +0x1C8: its vf04 0x000EED90(DAT_002E7C28)
        if (screen_) screen_->Update(app_.lastRenderDt);
        return;
    }
    if (!started) return;                             // +0x2D
    if (app_.IsMoviePlaying()) return;                // App->vf1C
    // the active pad idle for more than 0.33 s is dropped (DAT_002E7044 +0x13C, [0x002D5C58])
    if (stay && app_.input.activePad && double(app_.input.activePad->idleTime) > std::bit_cast<f32>(0x3EA8F5C3u))
        app_.input.activePad = nullptr;               // FCOMP; TEST AH,0x41; JNE: only on >
    {                                                 // Input_AllPadsDisconnected 0x0009BAA0 and DAT_002E7040
        bool all = true;
        for (input::Pad* p : app_.input.pads)
            if (p && !p->Disconnected()) all = false;
        if (all && app_.input.activePadFound && app_.errorOverlay)
            app_.errorOverlay->SetMessage("ERRCTRLFRONTEND");   // 0x000F2C80 (0x00294320)
    }
    if (double(idleAnimEnd) > double(time) && running.node)   // FLD +0xE4; FCOMP +0x78; TEST AH,0x41; JNE
        running.node->UpdateDownwardPass(time, true); // (+0xEC)->Update(time, 1) (no test in the original)
    if (smokeNode_) smokeNode_->UpdateDownwardPass(time, true);   // (+0x70)
    if (mainPanel)                                    // the main panel's current item (+0x1C: +0x20 [+8])
        if (ni::NiSwitchNode* item = mainPanel->items[size_t(mainPanel->selection)].Get())
            item->UpdateDownwardPass(time, true);
    if (alwaysNode_) alwaysNode_->UpdateDownwardPass(time, true);   // (+0x88)
    for (SceneSequence& q : smokes_) {                // (+0x19C [i])->Update(time, 1), (+0x184 [i])->vf38(time)
        if (q.node) q.node->UpdateDownwardPass(time, true);
        if (q.manager) q.manager->Update(time);
    }
    if (roller.node) roller.node->UpdateDownwardPass(shotTimer, true);   // (+0x16C)->Update(+0x174, 1)
    if (double(logoEnd) > double(time)) {             // FLD +0x5C; FCOMP +0x78; TEST AH,0x41; JNE
        if (logoManager_) logoManager_->Update(logoTime);   // (+0x60)->vf38(+0x58)
        if (logoNode_) logoNode_->UpdateDownwardPass(logoTime, true);   // (+0x50)->Update(+0x58, 1)
    }
    if (camera_) ni::VisitForCulling(*camera_, root_.Get());   // (+0x68)->Click (+0x15C = the root)
    // (the hint text: Mem_ReturnTrue)
    if (labels_) labels_->Update();                   // (+0x38) 0x00025700
    if (extrasPanel) extrasPanel->Draw();             // (+0x20)->vf08 0x000208E0: the shown extras menu
}

void FrontendContext::OnExit() {                      // orig: 0x00022E80
    loadingScreen = false;                            // +0x1C8
    screen_.reset();
    currentPanel = nullptr;                           // +0x34
    stay = true;                                      // +0x4C
    fadePending = true;                               // +0x1B8
    // (App+0x100: Mem_ReturnTrue)
    // a keyframe manager's release: DeactivateAll 0x00131170, RemoveAllSequences 0x00131010, its target's
    // RemoveController 0x000FE4C0 (the manager's target 0x0013EB60)
    auto stop = [](ni::NiKeyframeManager* m) {
        if (!m) return;
        m->DeactivateAll();
        m->RemoveAllSequences();
        if (ni::NiObjectNET* t = m->targetPtr) t->RemoveController(m);
    };
    auto detach = [](ni::NiNode* n) {                 // (+0x14)->vfA4(&out, node): DetachChild, the result released
        if (n && n->parent) (void)n->parent->DetachChild(n);
    };
    stop(running.manager.Get());                      // +0xF4, then +0xF0 / +0xF4 / +0xEC released
    running.seq = nullptr, running.manager = nullptr, running.node = nullptr;
    idleAnimRunning = false;
    for (SceneSequence* d : {&robothead, &slurmcans, &kibbles, &cigarrs, &furfist, &daliclock})   // +0xC8 .. +0xB0
        stop(d->manager.Get());
    for (SceneSequence& q : smokes_) {                // +0x184 [i] stopped and released, +0x190 [i], +0x19C [i]
        stop(q.manager.Get());
        q.manager = nullptr, q.seq = nullptr, q.node = nullptr;
    }
    for (SceneSequence* d : {&robothead, &slurmcans, &kibbles, &cigarrs, &furfist, &daliclock})   // +0xC4 .. +0xAC
        detach(d->node.Get());
    for (SceneSequence* d : {&robothead, &slurmcans, &kibbles, &cigarrs, &furfist, &daliclock})   // seq, manager, node
        d->seq = nullptr, d->manager = nullptr, d->node = nullptr;
    for (SceneSequence& q : bubbles) {                // +0x114 [i] stopped and released, +0x124 [i], +0x134 [i]
        stop(q.manager.Get());
        q.manager = nullptr, q.seq = nullptr, q.node = nullptr;
    }
    if (fade_) {                                      // +0x1BC: 0x00100FC0 on the camera, released
        if (camera_) camera_->DetachScreenPolygon(fade_.Get());
        fade_ = nullptr;
    }
    stop(cameraManager_.Get());                       // +0x17C
    if (camera_)                                      // (+0x68)+0x15C released; its screen polygons' slots cleared
        for (auto& p : camera_->screenPolygonList) p = nullptr;   // (SetAt(i, 0) each, 0x000225D0)
    cameraNode_ = nullptr, camera_ = nullptr, cameraManager_ = nullptr;   // +0x64, +0x68, +0x17C
    cameraMove_ = nullptr;                            // +0x110
    for (auto& m : cameraMoves_) m = nullptr;         // +0xF8 .. +0x10C
    // (+0x3C released: no writer found)
    stop(roller.manager.Get());                       // +0x168, then +0x16C / +0x164
    roller.manager = nullptr, roller.node = nullptr, roller.seq = nullptr;
    stop(logoManager_.Get());                         // +0x60, then +0x54 / +0x50
    logoManager_ = nullptr, logotype_ = nullptr, logoNode_ = nullptr;
    smokeNode_ = nullptr, alwaysNode_ = nullptr;      // +0x70, +0x88 (+0x8C / +0x90 / +0x94: no writer found)
    app_.Sound().Release(musicSound);                 // Sound_Release(+0x1AC), (+0x1B0)
    app_.Sound().Release(speechSound);
    mainPanel.reset();                                // the panels +0x1C..+0x28 in order, vf00(1): the extras
    extrasPanel.reset();                              //  panel's deletes the extras menus (their scenes, lights)
    optionsPanel.reset();
    loadPanel.reset();
    if (text_) text_->RemoveAll();                    // (+0x98) 0x00099080, deleted
    text_.reset();
    root_ = nullptr;                                  // +0x48: the scene and its lights
    app_.memoryCards->Abort();                        // 0x000E4BE0
    app_.memoryCards->Stop();                         // 0x000E5210
    // SoundEvents::Shutdown 0x000CA980: the sound system's vt+0x0C (every sample deleted: 0x000C96C0), then the event
    // tables DAT_002EAC18 / DAT_002EADC4 (none outside a level in the port: the level owns its SoundEventTable)
    app_.Sound().DeleteAllSamples();
    labels_.reset();                                  // (+0x38) 0x00025900 (the db, the text, +4, +8), 0x000257E0
    loaded = false;                                   // +0x40: the next Enter reloads the front end (and reseeds)
}

// ---- the extras panel -----------------------------------------------------------------------------------------------

void FrontendExtrasPanel::Build(ni::NiNode* scene) {  // orig: 0x00020DA0 (scene)
    static const char* const kItems[3] = {"extra_movies", "extra_music", "extra_gallery"};
    for (size_t i = 0; i < items.size(); ++i) items[i] = FindAs<ni::NiSwitchNode>(scene, kItems[i]);   // +0x38..
    for (const auto& it : items)                      // every item off (+0xE0 = 0 when +0xCC > 0), the current on
        FrontendMainPanel::Highlight(it.Get(), false);
    FrontendMainPanel::Highlight(items[size_t(selection)].Get(), true);
    App& app = fe.App_();
    startSound = app.LoadSound("sounds/menu/startgame.wav", 0);   // +0x24
    goBackSound = app.LoadSound("sounds/menu/go_back.wav", 0);    // +0x28
    goToSound = app.LoadSound("sounds/menu/go_to.wav", 0);        // +0x20
    scrollSound = app.LoadSound("sounds/menu/scroll.wav", 0);     // +0x2C
}

void FrontendExtrasPanel::Highlight() {               // orig: 0x000209C0
    for (const auto& it : items) FrontendMainPanel::Highlight(it.Get(), false);
    FrontendMainPanel::Highlight(items[size_t(selection)].Get(), true);
}

void FrontendExtrasPanel::Show() {                    // orig: 0x00020A20
    if (!opening) {
        Highlight();                                  // 0x000209C0
        fe.flowBusy = true;                           // Frontend+0x2E = 1
        opening = true;
        (void)fe.App_().Sound().PlaySfx(goToSound, false);   // Sound_PlaySFX(+0x20, 0)
        fe.MoveCamera(0);                             // 0x00023940(0): Camera_Newgame_there
        return;
    }
    if (fe.cameraMoving) return;                      // Frontend+0x6C
    Activate();                                       // vf20
    opening = false;
}

void FrontendExtrasPanel::Hide() {                    // orig: 0x00020920
    if (!closing) {
        closing = true;
        fe.flowBusy = false;                          // Frontend+0x2E = 0
        (void)fe.App_().Sound().PlaySfx(goBackSound, false);   // +0x28
        Deactivate();                                 // vf24
        fe.currentPanel = fe.mainPanel.get();         // +0x34 = +0x1C
        fe.MoveCamera(1);                             // Camera_Newgame_back
        return;
    }
    if (fe.cameraMoving) return;
    fe.mainPanel->Show();                             // (+0x34)->vf18
    closing = false;
}

void FrontendExtrasPanel::Draw() {                    // orig: 0x000208E0
    if (fe.extrasGrid && fe.extrasGrid->Shown()) fe.extrasGrid->Draw();   // DAT_002D9A70 vf14 / vf08
    if (fe.extrasList && fe.extrasList->Shown()) fe.extrasList->Draw();   // DAT_002D9B98
}

bool FrontendExtrasPanel::Update(f32 dt) {            // orig: vf04 0x00020B60
    App& app = fe.App_();
    if (fe.extrasGrid && fe.extrasGrid->Shown()) {    // (no null tests in the original: the load made both)
        fe.extrasGrid->Update();
        return true;
    }
    if (fe.extrasList && fe.extrasList->Shown()) {
        fe.extrasList->Update();
        return true;
    }
    if (opening) {
        t = F(double(dt) + t);                        // FLD dt; FADD +0x18 (the base's fold time); FSTP
        Show();                                       // vf18
    }
    if (closing) {
        t = F(double(dt) + t);
        Hide();                                       // vf1C
    }
    if (delay != 0) {
        --delay;
        return true;
    }
    if (!active || fe.cameraMoving || app.memoryCards->running) return true;
    if (MenuInput(app, 5) && !startLevel) {           // back (+0x30 is never set)
        if (app.memoryCards->dialog.open) return true;   // 0x000E5240
        if (fe.cameraMoving) return true;
        Deactivate();                                 // vf24
        Hide();                                       // vf1C
        return true;
    }
    if (MenuInput(app, 4)) {                          // accept
        if (app.memoryCards->dialog.open) return true;
        if (selection == 0) {
            if (fe.extrasGrid) fe.extrasGrid->Open(1);   // vf0C(1): the movies
        } else if (selection == 1) {
            if (fe.extrasList) fe.extrasList->Open(2);   // vf0C(2): the music
        } else if (selection == 2) {
            if (fe.extrasGrid) fe.extrasGrid->Open(3);   // the gallery
        }
        delay = 10;
        return true;
    }
    if (startLevel) {                                 // (dead: +0x30 has no writer) "Level1-1"
        startLevel = false;
        app.RequestContext("Ingame", 0x00020CEB);
        app.SetLevelName("Level1-1");                 // 0x00294160
        fe.stay = false;                              // +0x4C
    }
    if (MenuInput(app, 0)) {                          // up
        selection = selection > 0 ? selection - 1 : 2;
        delay = 10;
        Highlight();
        (void)app.Sound().PlaySfx(scrollSound, false);   // +0x2C
    }
    if (MenuInput(app, 1)) {                          // down
        selection = selection < 2 ? selection + 1 : 0;
        delay = 10;
        Highlight();
        (void)app.Sound().PlaySfx(scrollSound, false);
    }
    return true;
}

FrontendExtrasPanel::~FrontendExtrasPanel() {         // orig: 0x00020A70
    sound::GameSound& snd = fe.App_().Sound();        // Sound_Release +0x24, +0x20, +0x28, +0x2C
    for (u32 h : {startSound, goToSound, goBackSound, scrollSound}) snd.Release(h);
    for (auto& it : items) it = nullptr;              // +0x38..+0x40
    startLevel = false;                               // +0x30
    fe.extrasGrid.reset();                            // DAT_002D9A70 vf00(1), = 0
    fe.extrasList.reset();                            // DAT_002D9B98 vf00(1), = 0
}

}  // namespace fut::game::app

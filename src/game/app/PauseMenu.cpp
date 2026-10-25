// The pause menu; see PauseMenu.h.
#include "game/app/PauseMenu.h"

#include "engine/ni/NiMath.h"
#include "engine/ni/NiStream.h"
#include "game/GameTime.h"
#include "game/app/App.h"
#include "game/app/Contexts.h"
#include "game/app/Frontend.h"
#include "game/app/LoadPanel.h"
#include "game/app/MemoryCards.h"
#include "game/app/MenuScene.h"
#include "game/app/OptionsPanel.h"
#include "game/text/LanguageDb.h"
#include "game/text/TextObject.h"

#include "engine/ni/NiCull.h"
#include "engine/ni/NiRenderer.h"

#include <bit>
#include <cstdio>
#include <string>
#include <vector>

namespace fut::game::app {

using ni::detail::F;

PauseMenu::PauseMenu(App& a) : app(a) {}             // orig: 0x000A83F0 (the fields cleared)
PauseMenu::~PauseMenu() {                             // (the panels first: they use the text object and the sounds)
    load.reset();
    options.reset();
}

void PauseMenu::Build() {                             // orig: 0x000A8640
    shown = false;                                    // +0x24
    // the camera +0x48: new(0x250) 0x001009D0, SetFrustum(-0.77, 0.77, 0.51, -0.51, 0.1, 1000) (0x00011960), the port
    // (0, 1, 1, 0) (+0x14C), the renderer +0x1A8
    camera = new ni::NiCamera;
    const f32 frustum[6] = {std::bit_cast<f32>(0xBF451EB8u), std::bit_cast<f32>(0x3F451EB8u),
                            std::bit_cast<f32>(0x3F028F5Cu), std::bit_cast<f32>(0xBF028F5Cu),
                            std::bit_cast<f32>(0x3DCCCCCDu), 1000.0f};
    camera->SetFrustum(frustum);
    camera->port[0] = 0.0f, camera->port[1] = 1.0f, camera->port[2] = 1.0f, camera->port[3] = 0.0f;
    camera->renderer = app.services.renderer;
    // the root +0x44 (new(0xE0) NiNode) with a z-buffer property at its head: new(0x1C) (flags 3, function 3), flags
    // |= 3, function 0 (AddHead 0x00013A50)
    root = new ni::NiNode;
    {
        auto* z = new ni::NiZBufferProperty;
        z->flags = 3, z->testFunction = 3;
        z->flags = u16(z->flags | 3);
        z->testFunction = 0;
        root->properties.insert(root->properties.begin(), ni::NiPointer<ni::NiProperty>(z));
    }
    // the text object +0x54 (scene mode on the root and the camera)
    text.reset(text::TextObject::Create(0, app.services.readFile, app.services.renderer));
    if (text) {
        text->field7A = 1;                            // +0x7A
        text->UseScene(camera.Get(), root.Get());     // 0x00099BE0(+0x48, +0x44)
        text->color = ni::NiColor{1, 1, 1};
        text->SetRenderer(app.services.renderer);     // 0x00098F80 (nothing in the scene mode)
        text->SetScale(std::bit_cast<f32>(0x3FB33333u), std::bit_cast<f32>(0x40066666u));   // (1.4, 2.1)
        text->style.vAlign = 0;                       // +0x64
        text->style.hAlign = 0;                       // +0x60
        text->SetZBufferTest(false);                  // 0x00099660(0)
        text->style.letterSpacing = std::bit_cast<f32>(0x3BC49BA6u);   // +4 = 0.006
        text->style.wordWrap = false;                 // +0x68
    }
    // pause\pause.nif (a direct load: its textures are created now): the first top-level object as a NiNode, its
    // "SceneNode" under the root
    std::vector<u8> bytes;
    ni::NiStream s;
    ni::NiPointer<ni::NiNode> top;
    if (app.services.readFile && app.services.readFile("pause\\pause.nif", bytes) && s.Load(bytes) &&
        !s.TopLevelLinks().empty())
        top = dynamic_cast<ni::NiNode*>(s.ResolveLink(s.TopLevelLinks()[0]));
    scene = FindAs<ni::NiNode>(top.Get(), "SceneNode");   // (the original reads the top object without a test)
    if (scene) root->AttachChild(scene.Get(), true);  // vfA0(scene, 1)
    // the panels: main new(0x5C) 0x000A7AE0(0, this), +0xC = the scene, vf28 0x000A72A0 (main1 and the four items,
    // (+0xC)->Update(0, 1), +0x18 = +0x2C = 0, language\BREAKDOWN.ldb, its Reset 0x000A6F30: the sounds)
    main = PausePanel{};
    main.pause = this;
    main.node = FindAs<ni::NiNode>(scene.Get(), "main1");   // +0xC
    static const char* const kItems[4] = {"main_newgamenull", "main_optionsnull", "main_continuenull", "main_extras"};
    for (size_t i = 0; i < main.items.size(); ++i) main.items[i] = FindAs<ni::NiSwitchNode>(scene.Get(), kItems[i]);
    if (main.node) main.node->UpdateDownwardPass(0.0f, true);   // (+0xC)->Update(0, 1)
    main.t = 0;                                       // +0x18
    main.confirm = 0;                                 // +0x2C
    // +0x58 = TextManager::LoadDb("language\\BREAKDOWN.ldb") (0x002A1DB0: the Quit dialog's BD_CONFIRM_QUIT is found
    // through it while no breakdown has loaded "Language/BREAKDOWN.ldb")
    main.texts = app.text ? app.text->LoadDb("language\\BREAKDOWN.ldb") : nullptr;
    main.Reset();                                     // 0x000A6F30
    // load new(0x7C) 0x000A7C20(0, this, +0x54): +0xC = "loadmenu1", 0x0001AD60(scene) (the save slots' nodes, their
    // alpha properties and names, the arrows); options new(0x10FC) 0x000A7FA0(0, this, +0x54): +0xC = "options1",
    // vf28 0x000279C0(scene, camera) (the switches, the sliders, menu\align.nif's polygon: NOT PORTED)
    load = std::make_unique<PauseLoadPanel>(app, *this, text.get());
    load->node = FindAs<ni::NiNode>(scene.Get(), "loadmenu1");   // +0xC (0x002A1ED8)
    load->BuildRows(scene.Get());                     // 0x0001AD60(scene)
    options = std::make_unique<PauseOptionsPanel>(app, *this, text.get());   // (its ctor: 0x00027460, two draws)
    options->node = FindAs<ni::NiNode>(scene.Get(), "options1");  // +0xC
    options->Build(camera.Get(), scene.Get());        // vf28 0x000279C0(scene, camera) (its Reset 0x000A7F50: two draws)
    fade = FindAs<ni::NiTriShape>(scene.Get(), "fade1Shape");   // +0x40
    if (auto* l = FindAs<ni::NiNode>(scene.Get(), "pointLight4")) root->AttachChild(l, true);
    if (auto* l = FindAs<ni::NiNode>(scene.Get(), "pointLight3")) root->AttachChild(l, true);
    camNull = FindAs<ni::NiNode>(scene.Get(), "camnull");   // +0x4C
    if (camNull) {                                    // (the original attaches without a test)
        root->AttachChild(camNull.Get(), true);
        camNull->AttachChild(camera.Get(), true);
    }
    // the camera's +0x15C = the root (the Click's scene); its local translate (0, 0, -1) and rotate MakeYRotation(
    // F(pi * -0.5)) (0x001030B0 over 0x00103130's; the copy's three more words are not modelled: L)
    camera->local.translate = ni::NiPoint3{0.0f, 0.0f, -1.0f};
    camera->local.rotate = ni::MakeYRotation(F(double(std::bit_cast<f32>(0x40490FDBu)) * -0.5f));   // [0x002F9274]
    camera->UpdateDownwardPass(0.0f, true);           // Update(0, 1)
    root->UpdateDownwardPass(0.0f, true);
    root->UpdateProperties();
    root->UpdateEffects();
    always[0] = FindAs<ni::NiNode>(root.Get(), "Allways_animated");    // +0x5C
    always[1] = FindAs<ni::NiNode>(root.Get(), "allways_animated2");   // +0x60
    always[2] = FindAs<ni::NiNode>(root.Get(), "allways_animated3");   // +0x64
    labels = std::make_unique<MenuLabels>(root.Get(), camera.Get());   // +0x14 = new(0x10) 0x00025710(+0x44, +0x48)
    labels->Init(app);                                // 0x00025850
    PreCacheScene(app.services.renderer, root.Get());   // 0x000CEAC0(+0x44)
}

void PauseMenu::ResetPanels() {                       // orig: 0x000A81F0
    main.Reset();                                     // vf0C 0x000A6F30 (its sounds)
    if (load) load->Reset();                          // vf0C 0x000A7B80 (its sounds)
    if (options) options->Reset();                    // vf0C 0x000A7F50 (its sounds and the preview samples 0x00027460)
}

void PausePanel::Reset() {                            // orig: 0x000A6F30
    App& app = pause->app;
    scrollSound = app.LoadSound("sounds/menu/scroll.wav", 0);              // +0x44 (0x00293C7C)
    limitSound = app.LoadSound("sounds/menu/scrolllimit.wav", 0);          // +0x50 (0x00293C40)
    foldUpSound = app.LoadSound("sounds/menu/pause_fold_up.wav", 0);       // +0x4C (0x002A1D4C)
    selectSound = app.LoadSound("sounds/menu/select.wav", 0);              // +0x48 (0x00293EE8)
    foldDownSound = app.LoadSound("sounds/menu/pause_fold_down.wav", 0);   // +0x54 (0x002A1D2C)
}

void PauseMenu::Open() {                              // orig: 0x000A82B0
    if (labels) labels->LoadDb(app);                  // (+0x14)->0x000256D0: Language/FRONTEND.ldb
    main.selection = 0;                               // (+0x18)+8
    app.Sound().PauseAll();                           // Sound_PauseAll 0x000CAFD0
    shown = true;
    closing = false;
    fadingIn = true;
    fadingOut = false;
    alpha = 0;
    if (input::Pad* pad = app.ActivePad()) {          // Pad::GetRumble 0x0009B7F0(&+0x30, &+0x34)
        rumbleLeft = pad->rumbleLeft;
        rumbleRight = pad->rumbleRight;
    }
    app.input.SetRumbleEnabled(false, 0.0f);          // Input_SetRumbleEnabled(0, 0)
    app.input.Update(g_frameStep, app.services.readPad);   // Input_Update 0x0009BEA0
    if (labels) labels->Build(app);                   // (+0x14)->0x00025970
    main.Show();                                      // vf18
}

void PauseMenu::Update() {                            // orig: 0x000A8520
    time = F(double(g_frameStep) + time);             // +0x68
    if (fadingIn) {
        alpha = F(double(alpha) + 0.03f);             // [0x00293AA4]
        SetOverlayAlpha(alpha);                       // the overlay's property 3 +0x50
        if (double(alpha) > 0.75f) {                  // [0x00293A94]; TEST AH,0x41; JNE
            alpha = 0.75f;
            fadingIn = false;
        }
    }
    if (fadingOut) {
        alpha = F(double(alpha) - 0.03f);
        SetOverlayAlpha(alpha);
        if (double(alpha) <= 0.0) {                   // FCOMP 0.0; TEST AH,0x41; JP: < or ==
            alpha = 0;
            fadingOut = false;
        }
    }
    if (app.memoryCards->running) {
        // (+0x1C)->vf04: the load panel, then the main panel only while it closes (each FLD dt; FMUL 0.5; FSTP)
        load->Update(F(double(g_frameStep) * 0.5f));  // [0x00293AA0]
        if (main.closing) main.Update(F(double(g_frameStep) * 0.5f));
        return;
    }
    main.Update(F(double(g_frameStep) * 0.5f));       // (+0x18)->vf04, (+0x1C)->vf04, (+0x20)->vf04 0x00028960
    load->Update(F(double(g_frameStep) * 0.5f));
    options->Update(F(double(g_frameStep) * 0.5f));
    if (closing) Close();
}

void PauseMenu::Close() {                             // orig: 0x000A8220 (retried every Update until no panel is busy)
    closing = true;
    if (main.Busy() || load->Busy() || options->Busy()) return;
    if (labels) labels->RemoveAll();                  // (+0x14)->0x000256F0: TextObject::RemoveAll
    app.input.SetRumbleEnabled(true, 0.0f);           // (1, 0)
    app.ActivePad()->SetRumble(rumbleLeft, rumbleRight, false);
    app.Sound().ResumeAll();                          // Sound_ResumeAll 0x000CAA80
    closing = false;
    shown = false;
}

void PauseMenu::StartFadeOut() {                      // orig: 0x000A8290
    fadingIn = false;
    fadingOut = true;
    alpha = 0.75f;
}

void PauseMenu::SetOverlayAlpha(f32 a) {              // (+0x40)->GetProperty(3) (the object's own list): +0x50, +0x54
    if (!fade) return;                                // (the original reads it without a test)
    for (const auto& p : fade->properties)
        if (auto* m = dynamic_cast<ni::NiMaterialProperty*>(p.Get())) {
            m->alpha = a;
            ++m->revision;
            return;
        }
}

void PauseMenu::Draw() {                              // orig: 0x000A8320
    // each panel's scene node at its animation time: (+0xC)->Update(+0x18, 1) of +0x18, +0x1C, +0x20
    if (main.node) main.node->UpdateDownwardPass(main.t, true);
    if (load->node) load->node->UpdateDownwardPass(load->t, true);
    if (options->node) options->node->UpdateDownwardPass(options->t, true);
    for (const auto& n : always)                      // +0x5C / +0x60 / +0x64 at +0x68
        if (n) n->UpdateDownwardPass(time, true);
    if (ni::NiSwitchNode* item = main.items[size_t(main.selection)].Get())   // the main panel's item at +0x68
        item->UpdateDownwardPass(time, true);
    if (camera && camera->renderer) camera->renderer->ClearBuffer(nullptr, 4);   // (+0x48)+0x1A8 vf44(0, 4): the z
    if (camera) ni::VisitForCulling(*camera, root.Get());   // (+0x48)->Click (+0x15C = the root)
    if (labels) labels->Update();                     // (+0x14) 0x00025700
    if (main.confirm == 1 || app.memoryCards->dialog.Animating())   // (+0x18)+0x2C; 0x000E3020
        app.memoryCards->dialog.Draw();               // ((App+0xF4)+0x10) 0x000E33A0
    ++draws;                                          // +4
}

// ---- the main panel -----------------------------------------------------------------------------------------------

void PausePanel::Highlight(ni::NiSwitchNode* item, bool on) {
    if (!item) return;                                // (the original reads the item without a test)
    const s32 count = s32(item->Children().size());   // +0xCC
    if (on ? count > 1 : count > 0) item->index = on ? 1 : 0;   // +0xE0
}

void PausePanel::Show() {                             // orig: 0x000A7020
    if (!opening) {
        t = 0;                                        // +0x18
        for (const auto& it : items) Highlight(it.Get(), false);   // the four items off
        selection = 0;                                // +8
        Highlight(items[0].Get(), true);              // the first item on
        opening = true;
        (void)pause->app.Sound().PlaySfx(foldDownSound, false);   // Sound_PlaySFX(+0x54 fold_down, 0)
    }
    delay = 10;
}

void PausePanel::Hide() {                             // orig: 0x000A70B0
    if (!closing) {
        t = 0.41666666f;                              // 0x3ED55555
        for (const auto& it : items) Highlight(it.Get(), false);
        if (node) node->UpdateDownwardPass(t, true);  // (+0xC)->Update(+0x18, 1)
        closing = true;
        (void)pause->app.Sound().PlaySfx(foldUpSound, false);   // Sound_PlaySFX(+0x4C fold_up, 0); the hint no-op
        pause->StartFadeOut();                        // 0x000A8290
    }
    active = false;
}

void PausePanel::Activate() {                         // orig: 0x000A7150
    Highlight(items[size_t(selection)].Get(), true);  // the selection's item on
    active = true;
    opening = false;
    closing = false;
    delay = 10;
    // 0x000A6F90 (the hint no-op of the selection)
    pause->app.input.Update(g_frameStep, pause->app.services.readPad);   // Input_Update 0x0009BEA0
}

void PausePanel::Deactivate() {                       // orig: 0x000A7190
    Highlight(items[size_t(selection)].Get(), false); // the selection's item off
    active = false;
    closing = false;
    opening = false;
    delay = 10;
}

bool PausePanel::Update(f32 dt) {                     // orig: 0x000A7570
    App& app = pause->app;
    if (opening) {
        const double s = double(dt) + t;              // FLD dt; FADD +0x18; FCOM [0x002A1DD4]
        if (s > 0.22833332f) {                        // TEST AH,0x41; JNE (t not stored)
            g_menuInputMode = 3;                      // 0x00029F80 (result unused), 0x00029F90(3)
            Activate();                               // vf20
            delay = 10;
            return true;
        }
        t = F(s);
    }
    if (closing) {
        const double s = double(dt) + t;              // FCOM [0x002A1DD0]
        if (s > 0.625f) {
            Deactivate();                             // vf24
            if (pause->load->gameLoaded) {            // ((+0x40)+0x1C)+0x20 (the dead App+0xF8 == 0 path)
                app.RequestContext("Ingame", 0x000A75F7);
                pause->load->ApplyLoaded();           // 0x0001A940
                pause->Close();
                pause->load->gameLoaded = false;
                return false;
            }
            pause->Close();                           // 0x000A8220
            return true;
        }
        t = F(s);
    }
    if (!active) return true;
    if (delay != 0) {
        --delay;
        return true;
    }
    Dialog& dlg = app.memoryCards->dialog;            // (App+0xF4)+0x10
    if (confirm == 1 || dlg.Animating()) {            // +0x2C; 0x000E3020
        if (!dlg.Animating()) {
            if (MenuInput(app, 2)) dlg.Select(0);     // left: "yes"
            if (MenuInput(app, 3)) dlg.Select(1);     // right: "no"
            if (MenuInput(app, 4)) {
                confirm = dlg.selection != 0 ? 0 : 2; // ((+0x5C != 0) - 1) & 2
                app.memoryCards->PlayDialogSound();   // ((App+0xF4)+4)+4 -> 0x000E1980
                dlg.Close();                          // 0x000E4980
            }
        }
        return true;
    }
    if (confirm == 2) {                               // Quit confirmed
        app.ingame->Leave("Frontend");                // 0x0002A220
        app.SetLevelName("");                         // App+0xAC = "" (0x00295254)
        t = 0;
        pause->shown = false;                         // +0x24
        pause->alpha = 0;                             // +0x28
        if (pause->root) pause->root->UpdateDownwardPass(t, true);   // (+0x40)+0x44 ->Update(+0x18, 1)
        pause->field38 = 0;                           // +0x38
        pause->fadingIn = false;
        pause->fadingOut = false;
        selection = 0;
        pause->SetOverlayAlpha(pause->alpha);         // (the overlay's material: +0x50 = +0x28, +0x54 + 1)
        for (const auto& it : items) Highlight(it.Get(), false);
        if (node) node->UpdateDownwardPass(t, true);  // (+0xC)->Update(+0x18, 1)
        confirm = 0;
        return true;
    }
    if (MenuInput(app, 0)) {                          // up
        Highlight(items[size_t(selection)].Get(), false);   // the old item off, the new one on
        selection = selection > 0 ? selection - 1 : 3;
        Highlight(items[size_t(selection)].Get(), true);
        (void)app.Sound().PlaySfx(scrollSound, false);   // Sound_PlaySFX(+0x44 scroll, 0); the hint no-op
        delay = 10;
        return true;
    }
    if (MenuInput(app, 1)) {                          // down
        Highlight(items[size_t(selection)].Get(), false);
        selection = selection < 3 ? selection + 1 : 0;
        Highlight(items[size_t(selection)].Get(), true);
        (void)app.Sound().PlaySfx(scrollSound, false);
        delay = 10;
        return true;
    }
    if (MenuInput(app, 5)) {                          // back: Resume
        Deactivate();
        delay = 10;
        Hide();
        return true;
    }
    if (!MenuInput(app, 4)) return true;              // accept
    if (input::Pad* p = app.input.activePad)
        p->Latch(p->IsDownUnlatched(input::kButtonA) ? input::kButtonA : input::kStart);
    switch (selection) {                              // the jump table 0x000A7AC8
    case 0:                                           // Resume
        Deactivate();
        (void)app.Sound().PlaySfx(selectSound, false);   // Sound_PlaySFX(+0x48 select), (+0x4C fold_up)
        (void)app.Sound().PlaySfx(foldUpSound, false);
        Hide();
        return true;
    case 1:                                           // Options
        if (pause->options->Busy()) return true;      // ((+0x40)+0x20) 0x00029880
        Highlight(items[size_t(selection)].Get(), false);   // the item off
        pause->options->Show();                       // vf18
        (void)app.Sound().PlaySfx(selectSound, false);   // Sound_PlaySFX(+0x48, 0)
        Deactivate();                                 // vf24
        return true;
    case 2:                                           // Load (App+0xF8 = 1: the memory-card load flow 0x000E5170)
        if (pause->load->Busy()) return true;         // ((+0x40)+0x1C) 0x00029880
        Highlight(items[size_t(selection)].Get(), false);   // the item off
        if (app.useMemoryCardFlows) {
            app.memoryCards->StartLoad();             // (App+0xF4)->0x000E5170 (its L_EntryPoint shows the panel)
        } else {                                      // (dead: App+0xF8 is 1) the list scrolled to its top, Show
            LoadPanel& lp = *pause->load;
            while (lp.list.Up()) {}                   // 0x00029B70 until false
            lp.ResetList();
            lp.Show();
        }
        (void)app.Sound().PlaySfx(selectSound, false);   // Sound_PlaySFX(+0x48 select, 0)
        Deactivate();                                 // vf24
        return true;
    case 3: {                                         // Quit: the dialog (BD_CONFIRM_QUIT, buttons (1, 2), "no")
        Dialog& d = app.memoryCards->dialog;
        // 0x000E3500(TextManager::GetText("BD_CONFIRM_QUIT", 0)), 0x000E3790(1, 2)
        (void)d.SetString(app.text ? app.text->GetText("BD_CONFIRM_QUIT", nullptr) : nullptr);
        d.SetButtons(1, 2);
        d.Select(1);                                  // 0x000E35A0(1)
        d.Open();                                     // 0x000E4900
        confirm = 1;
        return true;
    }
    default:
        return true;
    }
}

}  // namespace fut::game::app

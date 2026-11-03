// The PAL game's language screen (PORT ADDITION from the PS2 build SLUS_204.39); see LanguageSelect.h.
#include "game/app/LanguageSelect.h"

#include "core/CrtString.h"
#include "engine/ni/NiCull.h"
#include "engine/ni/NiRenderer.h"
#include "game/GameTime.h"
#include "game/app/Contexts.h"
#include "game/app/Frontend.h"
#include "game/text/LanguageDb.h"

namespace fut::game::app {

namespace {

// The flags in the screen's order, as SetLanguage names them (PS2 0x0046BFA8..: "English" .. "Spanish") and as the
// current language is matched on entry (PS2 0x0046BF78..: "english" .. "spanish", 0x004006AC).
constexpr const char* kLanguages[LanguageSelectContext::kFlags] = {"English", "French", "German", "Italian", "Spanish"};
constexpr s16 kFlagTop = 0xB5;                        // PS2 0x00135E44: 181
constexpr s16 kFlagLeft[LanguageSelectContext::kFlags] = {0x45, 0xA9, 0x10D, 0x171, 0x1D5};   // 69 + 100 i
constexpr f32 kPulseStep = 0.035f;                    // PS2 0x3D0F5C29, per update
constexpr f32 kPulseTop = 0.965f;                     // PS2 0x3F770A3D
constexpr f32 kIdleLimit = 25.0f;                     // PS2 0x41C80000: the Attract movie after 25 s untouched
constexpr s32 kRepeatFrames = 15;                     // PS2 0x0013620C

}  // namespace

LanguageSelectContext::LanguageSelectContext(App& app) : Context(app, "LanguageSelect") {}   // PS2 0x001352C8

bool LanguageSelectContext::OnEnter() {               // PS2 0x001355F0
    idleTime = 0;                                     // +0x70
    // the camera (new(0x250), the App's renderer) and the background: PS2 loads menu\langscrn.nif and cuts its picture
    // into six 256 x 256 screen textures on the camera, as the Xbox splash screens' base 0x000EF710 does (the Xbox
    // copy of the file is the same single full-screen picture as the legal screens')
    if (app_.services.renderer) {
        camera = new ni::NiCamera;
        camera->renderer = app_.services.renderer;
        background = std::make_unique<LoadingScreen>(app_.services.renderer, &app_.services.readFile,
                                                     "menu\\langscrn.nif", "menu\\langscrn.nif", camera.Get());
    }
    // the five flags' highlights (PS2 0x00134910(icon_<i>.nif, 181, 69 + 100 i, camera)): the nif's texture in a
    // screen texture rect of its size, modulated (apply mode 2), alpha 0, on the camera
    for (u32 i = 0; i < kFlags; ++i) {
        Flag& f = flags[i];
        f = Flag{};
        char name[32];
        std::snprintf(name, sizeof name, "menu\\icon_%u.nif", i);   // PS2 0x0046BEF8..
        const ni::NiPointer<ni::NiSourceTexture> tex = LoadSourceTexture(app_.services.readFile, name);
        f.texture = new ni::NiScreenTexture(tex.Get());
        const ni::NiPixelData* pix = tex ? tex->pixelDataObj.Get() : nullptr;
        const u16 w = pix && !pix->mips.empty() ? u16(pix->mips[0].width) : 0;
        const u16 h = pix && !pix->mips.empty() ? u16(pix->mips[0].height) : 0;
        f.texture->AddRect(kFlagTop, kFlagLeft[i], w, h, 0, 0, ni::NiColorA{1.0f, 1.0f, 1.0f, 1.0f});
        f.texture->texturing->applyMode = 2;
        SetAlpha(f, 0.0f);
        f.texture->dirty |= 4;
        if (camera) camera->AddScreenTexture(f.texture.Get());   // PS2 0x00134DA0
    }
    // the menu sounds (PS2 "sounds\menu\select.wav" +0x68 / "sounds\menu\scroll.wav" +0x64; the discs' unused
    // lang_enter / lang_toggle are not the code's)
    selectSound = app_.LoadSound("sounds/menu/select.wav", 0);
    scrollSound = app_.LoadSound("sounds/menu/scroll.wav", 0);
    // the current language's flag (PS2 0x00135F84: 0 unless one of the five names matches)
    selected = 0;
    const std::string& current = app_.text ? app_.text->Language() : std::string("English");
    for (u32 i = 0; i < kFlags; ++i)
        if (crt::StrICmp(current.c_str(), kLanguages[i]) == 0) selected = s32(i);
    SetAlpha(flags[0], 0.2f);                         // PS2 0x00136044 (flag 0, whichever is selected)
    return true;
}

void LanguageSelectContext::OnExit() {                // PS2 0x001365E8
    background.reset();                               // the six tiles off the camera
    for (Flag& f : flags) {                           // PS2 0x00134C08 -> 0x00135248: off the camera
        if (camera && f.texture) camera->RemoveScreenTexture(f.texture.Get());
        f = Flag{};
    }
    selected = 0;                                     // +0x4C
    repeatDelay = 0;                                  // +0x20
    camera = nullptr;
}

void LanguageSelectContext::SetAlpha(Flag& flag, f32 alpha) {   // PS2 0x00135270
    flag.alpha = alpha;                               // +0xB0
    if (!flag.texture) return;
    for (ni::NiScreenTexture::Rect& r : flag.texture->rects) r.color.a = alpha;
    flag.texture->dirty |= 0xF;
}

void LanguageSelectContext::Pulse() {                 // PS2 0x001363C8
    for (u32 i = 0; i < kFlags; ++i)                  // the others fade out
        if (s32(i) != selected && kPulseStep < flags[i].alpha) SetAlpha(flags[i], flags[i].alpha - kPulseStep);
    Flag& f = flags[u32(selected)];                   // the selected one pulses between 0.035 and 0.965
    SetAlpha(f, f.rising ? f.alpha + kPulseStep : f.alpha - kPulseStep);
    if (f.alpha < kPulseStep) f.rising = true;
    if (kPulseTop < f.alpha) f.rising = false;
}

bool LanguageSelectContext::Update() {                // PS2 0x001360E8
    bool idle = true;                                 // PS2 0x002315D8 (Input_AllPadsIdle's role)
    for (input::Pad* p : app_.input.pads)
        if (p && !p->IsIdle()) idle = false;
    idleTime = idle ? idleTime + g_frameStep : 0.0f;
    if (kIdleLimit < idleTime) {                      // the Attract movie, then back here (PS2 0x00136148)
        if (app_.attract) app_.attract->SetReturn("LanguageSelect");
        app_.RequestContext("Attract", 0);
        return false;
    }
    Pulse();
    // (PS2 +0x14: its context base's fade-out after the choice (0x00137320); the Xbox contexts switch at once)
    if (repeatDelay > 0) {
        --repeatDelay;
        return true;
    }
    auto move = [&](s32 to) {                         // PS2 0x001361B4 / 0x00136224
        flags[u32(selected)].rising = false;
        selected = to;
        flags[u32(selected)].rising = true;
        (void)app_.Sound().PlaySfx(scrollSound, false);   // 0x00294FD8(+0x64, 0)
        repeatDelay = kRepeatFrames;
    };
    if (MenuInput(app_, 2)) move(selected == 0 ? s32(kFlags) - 1 : selected - 1);   // left
    if (MenuInput(app_, 3)) move(selected == s32(kFlags) - 1 ? 0 : selected + 1);   // right
    if (MenuInput(app_, 4)) {                         // accept (A / Start): the language, then the Intro
        if (app_.text) app_.text->SetLanguage(kLanguages[u32(selected)]);   // PS2 0x002B82B8 (default "English")
        (void)app_.Sound().PlaySfx(selectSound, false);   // 0x00294FD8(+0x68, 0)
        app_.RequestContext("Intro", 0);
        return false;
    }
    return true;
}

void LanguageSelectContext::Render() {                // PS2 0x00136508
    // (PS2: its controller-removed message ERRCTRLFRONTEND_PAL first; the Xbox App shows its own, ErrorOverlay)
    if (!camera) return;
    camera->UpdateDownwardPass(0.0f, true);           // PS2 0x00355078(camera, 0, 1)
    ni::VisitForCulling(*camera, camera->sceneRoot);  // PS2 0x0035D060: the camera's Click (its screen textures)
}

}  // namespace fut::game::app

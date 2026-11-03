// The PAL game's language screen. PORT ADDITION: no Xbox code of it exists (neither the NTSC nor the PAL default.xbe
// has the class or its strings; the Xbox game takes the console's language, App::Init 0x000D2825 / 0x000A2983), but
// its files are on both Xbox discs (menu\langscrn.nif, menu\icon_0..4.nif, the menu sounds) and the PS2 build
// (SLUS_204.39) has the screen: LanguageSelect (new(0x74), ctor 0x001352C8, vtable 0x00468D08), made before the
// Intro on a PAL machine (0x00241498) and so the first context. The behaviour below is the PS2 code's, built from the
// Xbox pieces the port already has (the splash screens' full-screen image, screen textures, MenuInput 0x00029FA0,
// the menu sounds, TextManager::SetLanguage, the context requests). docs/NOTES.md: "The language screen".
#pragma once

#include "game/app/App.h"
#include "game/level/Session.h"

#include <array>
#include <memory>

namespace fut::game::app {

class LanguageSelectContext : public Context {
public:
    explicit LanguageSelectContext(App& app);         // PS2 0x001352C8
    bool Update() override;                           // PS2 vf 0x001360E8
    void Render() override;                           // PS2 vf 0x00136508
    // PS2 icon objects (new(0xB4), ctor 0x00134910(nif, top, left, camera)): a flag's highlight, one screen
    // texture of the nif's texture at (top, left), its alpha pulsed (0x00135270 sets it: +0xB0 and every rect's).
    struct Flag {
        bool rising = true;                           // PS2 +0x00 (the ctor: 1)
        f32 alpha = 0;                                // PS2 +0xB0
        ni::NiPointer<ni::NiScreenTexture> texture;   // PS2 +0xA8 (of the nif's texture +0xA4)
    };
    static constexpr u32 kFlags = 5;                  // English, French, German, Italian, Spanish
    s32 selected = 0;                                 // PS2 +0x4C
    s32 repeatDelay = 0;                              // PS2 +0x20 (frames until the next left / right)
    f32 idleTime = 0;                                 // PS2 +0x70 (the Attract timer)
    std::array<Flag, kFlags> flags;                   // PS2 +0x50..+0x60
    u32 scrollSound = 0, selectSound = 0;             // PS2 +0x64 / +0x68

protected:
    bool OnEnter() override;                          // PS2 vf 0x001355F0
    void OnExit() override;                           // PS2 vf 0x001365E8

private:
    void SetAlpha(Flag& flag, f32 alpha);             // PS2 0x00135270
    void Pulse();                                     // PS2 0x001363C8
    ni::NiPointer<ni::NiCamera> camera;               // PS2 +0x24 (new(0x250), the App's renderer)
    std::unique_ptr<LoadingScreen> background;        // PS2 +0x28 / +0x30..+0x44 (langscrn.nif's six tiles)
};

}  // namespace fut::game::app

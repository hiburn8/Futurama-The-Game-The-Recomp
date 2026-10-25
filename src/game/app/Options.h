// The options (orig: the globals DAT_002E79A0..DAT_002E79CC with their setters 0x000A1820..0x000A1B60, the defaults
// 0x000A1B70, Options.dat's serialisation 0x000A18F0 / 0x000A2070; App+0xA8's 21 records 0x000A2290) and the options
// panels' preview-sample picker (new(0x70) 0x00026BF0, the previews 0x00027380 / 0x000273E0). The panels are
// OptionsPanel.h. docs/analysis/frontend_panels_port.md 1.3, 2.2, 2.5. D3DDevice_SetScreenSpaceOffset is a render
// seam: the App records the offset (App::screenOffset; NOT PORTED in the renderer's present path).
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <array>
#include <string>
#include <vector>

namespace fut::game::app {

class App;

// orig: App+0xA8 (new(0x1FC) 0x000A2290: +0 = 0, then 21 records of 0x18 bytes from +4: {char name[12], u32 +0xC,
// u32 +0x10, u32 +0x14}; the vector ctor 0x00166E90 with 0x000A1800 zeroes +0xC / +0x10 / +0x14 only). Options.dat
// holds the records' 0x1F8 bytes verbatim; no reader of +0x10 / +0x14 is known (frontend_panels_port.md open
// question 1). Name bytes 9..11 are never written (heap bytes in the original: 0 under the boot oracle; the port's 0).
struct LevelTargets {
    std::array<u8, 0x1F8> raw{};                      // +4 .. +0x1FC
    void Reset();                                     // orig: 0x000A1B70's record part
};

// orig: the options globals (frontend_panels_port.md 1.3).
struct Options {
    f32 speech = 0.75f;                               // DAT_002E79C0 (sound category 1, vf30)
    f32 music = 0.75f;                                // DAT_002E79B8 (category 2, vf38)
    f32 sfx = 0.75f;                                  // DAT_002E79AC (category 0, vf28)
    f32 screenX = 0, screenY = 0;                     // DAT_002E79A0 / DAT_002E79B4
    f32 appliedX = 0, appliedY = 0;                   // DAT_002E79C8 / DAT_002E79CC (the offset last applied)
    bool b1 = false;                                  // DAT_002E79B1 (no reader)
    bool subtitles = false;                           // DAT_002E79A9 (the movie subtitles 0x000ED7E0)
    bool vibration = true;                            // DAT_002E79B0 (g_optionVibration)
    bool invert = false;                              // DAT_002E79AA
    bool dirty = false;                               // DAT_002E79A8
};

bool SetSpeechVolume(App& app, f32 v);                // orig: 0x000A1880
bool SetMusicVolume(App& app, f32 v);                 // orig: 0x000A1820
bool SetSfxVolume(App& app, f32 v);                   // orig: 0x000A1850
bool SetVibration(App& app, bool on);                 // orig: 0x000A18B0
bool SetInvert(App& app, bool on);                    // orig: 0x000A18D0
bool MoveScreenX(App& app, f32 d);                    // orig: 0x000A19E0 (0x000A1B30 -2, 0x000A1B40 +2)
bool MoveScreenY(App& app, f32 d);                    // orig: 0x000A1A90 (0x000A1B50 -2, 0x000A1B60 +2)
bool SetScreen(App& app, f32 x, f32 y);               // orig: 0x000A1A60 (no dirty flag)
void ApplyScreen(App& app);                           // orig: 0x000A1B10
bool RestoreDefaults(App& app);                       // orig: 0x000A1B70 (thiscall App+0xA8)
// orig: 0x000A18F0(&out): the 0x214 bytes (the size; `out` null: the size only).
u32 SerializeOptions(const App& app, std::vector<u8>* out);
// orig: 0x000A2070(data): false = a value out of range (the defaults applied).
bool DeserializeOptions(App& app, const u8* data);
constexpr u32 kOptionsRecordsSize = 0x1F8;            // orig: 0x000A1810

// orig: the preview-sample picker (new(0x70) 0x00026BF0): ten SFX names +0x00.., eight voice names +0x28.. (the
// localized paths), +0x50 / +0x54 the excluded indices (0, never written), +0x58 / +0x5C the samples playing,
// +0x60 / +0x64 their voices, +0x68 / +0x6C the next samples.
struct PreviewPicker {
    explicit PreviewPicker(App& app);                 // orig: 0x00026BF0
    s32 PickSfx() const;                              // orig: 0x000270D0
    s32 PickSpeech() const;                           // orig: 0x00027110
    bool SfxPlaying() const;                          // orig: 0x00027090
    bool SpeechPlaying() const;                       // orig: 0x000270B0
    bool PreviewSfx();                                // orig: 0x00027380
    bool PreviewSpeech();                             // orig: 0x000273E0
    App& app;
    std::array<std::string, 10> sfxNames;
    std::array<std::string, 8> voiceNames;
    s32 sfxExcluded = 0, speechExcluded = 0;          // +0x50 / +0x54
    u32 sfxPlaying = 0, speechPlaying = 0;            // +0x58 / +0x5C
    s32 sfxVoice = 0, speechVoice = 0;                // +0x60 / +0x64 (uninitialised in the original: L, 0)
    u32 sfxNext = 0, speechNext = 0;                  // +0x68 / +0x6C
};

}  // namespace fut::game::app

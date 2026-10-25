// The options; see Options.h.
#include "game/app/Options.h"

#include "core/CrtRand.h"
#include "game/app/App.h"
#include "game/text/LanguageDb.h"

#include <bit>
#include <cstdio>
#include <cstring>

namespace fut::game::app {

using ni::detail::F;

namespace {

void PutF32(std::vector<u8>& b, size_t at, f32 v) { std::memcpy(&b[at], &v, 4); }
f32 GetF32(const u8* d, size_t at) {
    f32 v;
    std::memcpy(&v, d + at, 4);
    return v;
}

// orig: D3DDevice_SetScreenSpaceOffset 0x00235590 (render: the App records it)
void SetOffset(App& app, f32 x, f32 y) { app.screenOffset = {x, y}; }

}  // namespace

// ---- the globals -----------------------------------------------------------------------------------------------

void LevelTargets::Reset() {                          // orig: 0x000A1B70 (from 0x000A1BDE)
    // 21 x {the name's first 9 bytes (two dwords and its NUL; 9..11 untouched), +0xC = 0, +0x10, +0x14}; "level3-2"
    // is written twice (0x002A182C at +0x124 and +0x13C), the last two names are 0x002A17F0 / 0x002A17E4
    static constexpr struct {
        const char* name;
        u32 value, count;
    } kRecords[21] = {
        {"level1-1", 0x7D0, 1},  {"level1-2", 0x1388, 5},  {"level1-3", 0x1770, 3},  {"level1-4", 0x1F40, 4},
        {"level1-5", 0x384, 4},  {"level1-6", 0x1F40, 0xC}, {"level2-1", 0x8C0, 3},  {"level2-2", 0x85C, 6},
        {"level2-3", 0xAA0, 3},  {"level2-4", 0x848, 0xD},  {"level2-5", 0x178E, 0xB}, {"level3-1", 0x19C8, 5},
        {"level3-2", 0x7D0, 0xB}, {"level3-2", 0x7D0, 0xC}, {"level3-3", 0x7D0, 0xD}, {"level3-4", 0x805, 0x11},
        {"level3-5", 0x7D3, 0xA}, {"level3-6", 0x1403, 0x35}, {"level4-1", 0x3E9, 7}, {"level5-1", 0x5DC, 5},
        {"level5-2", 0x14B7, 0x13}};
    for (size_t i = 0; i < 21; ++i) {
        u8* r = raw.data() + i * 0x18;
        std::memcpy(r, kRecords[i].name, 9);          // (the 8 characters and the NUL)
        const u32 words[3] = {0, kRecords[i].value, kRecords[i].count};
        std::memcpy(r + 0xC, words, 12);
    }
}

bool SetMusicVolume(App& app, f32 v) {                // orig: 0x000A1820
    if (v != app.options.music) {                     // FCOMP; TEST AH,0x44; JNP: equal -> nothing
        app.options.music = v;
        app.options.dirty = true;
    }
    return true;
}

bool SetSfxVolume(App& app, f32 v) {                  // orig: 0x000A1850
    if (v != app.options.sfx) {
        app.options.sfx = v;
        app.options.dirty = true;
    }
    return true;
}

bool SetSpeechVolume(App& app, f32 v) {               // orig: 0x000A1880
    if (v != app.options.speech) {
        app.options.speech = v;
        app.options.dirty = true;
    }
    return true;
}

bool SetVibration(App& app, bool on) {                // orig: 0x000A18B0 (byte compare)
    if (app.options.vibration != on) {
        app.options.vibration = on;
        app.options.dirty = true;
    }
    return true;
}

bool SetInvert(App& app, bool on) {                   // orig: 0x000A18D0
    if (app.options.invert != on) {
        app.options.invert = on;
        app.options.dirty = true;
    }
    return true;
}

bool MoveScreenX(App& app, f32 d) {                   // orig: 0x000A19E0
    Options& o = app.options;
    o.dirty = true;
    o.screenX = F(double(o.screenX) + d);             // FLD X; FADD d; FSTP X
    if (double(o.screenX) + d > 12.0f) o.screenX = 12.0f;   // FLD X; FADD d; FCOMP [0x00297638]; TEST AH,0x41; JNE
    if (double(o.screenX) + d < -12.0f) o.screenX = -12.0f;   // FLD X; FADD d; FCOMP [0x002A17E0]; TEST AH,5; JP
    o.appliedX = o.screenX, o.appliedY = o.screenY;
    SetOffset(app, o.screenX, o.screenY);
    return true;
}

bool MoveScreenY(App& app, f32 d) {                   // orig: 0x000A1A90
    Options& o = app.options;
    o.dirty = true;
    const double y = double(o.screenY) + d;           // FLD Y; FADD d; FST Y (the unrounded sum stays)
    o.screenY = F(y);
    if (y + d > 12.0f) o.screenY = 12.0f;             // FADD d; FCOMP 12; TEST AH,0x41; JNE
    if (double(o.screenY) + d < -12.0f) o.screenY = -12.0f;   // FLD Y (the stored value); FADD d; FCOMP -12
    o.appliedX = o.screenX, o.appliedY = o.screenY;
    SetOffset(app, o.screenX, o.screenY);
    return true;
}

bool SetScreen(App& app, f32 x, f32 y) {              // orig: 0x000A1A60
    Options& o = app.options;
    o.screenX = x, o.screenY = y;
    o.appliedX = x, o.appliedY = y;
    SetOffset(app, x, y);
    return true;
}

void ApplyScreen(App& app) {                          // orig: 0x000A1B10
    Options& o = app.options;
    o.appliedX = o.screenX, o.appliedY = o.screenY;
    SetOffset(app, o.screenX, o.screenY);
}

bool RestoreDefaults(App& app) {                      // orig: 0x000A1B70
    Options& o = app.options;
    o.music = 0.75f, o.sfx = 0.75f, o.speech = 0.75f;   // 0x3F400000
    // `App+0xE8 == 0 || App+0xE8 == 1` (PAL or NTSC: always)
    o.screenX = 0, o.screenY = 0;
    o.appliedX = o.screenX, o.appliedY = o.screenY;
    SetOffset(app, o.screenX, o.screenY);
    o.b1 = false, o.subtitles = false, o.vibration = true, o.invert = false;
    app.levelTargets.Reset();
    return true;                                      // (the dirty flag untouched)
}

u32 SerializeOptions(const App& app, std::vector<u8>* out) {   // orig: 0x000A18F0
    if (!out) return 0x214;
    const Options& o = app.options;
    std::vector<u8>& b = *out;
    b.assign(0x214, 0);                               // new(0x214)
    PutF32(b, 0x00, o.speech);                        // DAT_002E79C0
    PutF32(b, 0x04, o.music);                         // DAT_002E79B8
    PutF32(b, 0x08, o.sfx);                           // DAT_002E79AC
    PutF32(b, 0x0C, o.screenX);                       // DAT_002E79A0
    PutF32(b, 0x10, o.screenY);                       // DAT_002E79B4
    b[0x14] = o.b1 ? 1 : 0;                           // DAT_002E79B1, 79A9, 79B0, 79AA (each 0 / 1)
    b[0x15] = o.subtitles ? 1 : 0;
    b[0x16] = o.vibration ? 1 : 0;
    b[0x17] = o.invert ? 1 : 0;
    std::memcpy(&b[0x18], app.levelTargets.raw.data(), kOptionsRecordsSize);   // REP MOVSD 0x7E from App+0xA8 + 4
    return 0x214;
}

bool DeserializeOptions(App& app, const u8* d) {      // orig: 0x000A2070
    Options& o = app.options;
    // each value stored, then FCOMP hi; TEST AH,0x41; JE bad (above) and FCOMP lo; TEST AH,5; JNP bad (below); a NaN
    // passes both
    auto bad = [](f32 v, f32 lo, f32 hi) { return double(v) > double(hi) || double(v) < double(lo); };
    const f32 kMax = std::bit_cast<f32>(0x3F733333u);   // [0x00294FDC] 0.95
    o.speech = GetF32(d, 0x00);
    if (bad(o.speech, 0.0f, kMax)) return RestoreDefaults(app), false;
    o.music = GetF32(d, 0x04);
    if (bad(o.music, 0.0f, kMax)) return RestoreDefaults(app), false;
    o.sfx = GetF32(d, 0x08);
    if (bad(o.sfx, 0.0f, kMax)) return RestoreDefaults(app), false;
    o.screenX = GetF32(d, 0x0C);
    if (bad(o.screenX, -12.0f, 12.0f)) return RestoreDefaults(app), false;   // [0x00297638] / [0x002A17E0]
    o.screenY = GetF32(d, 0x10);
    if (bad(o.screenY, -12.0f, 12.0f)) return RestoreDefaults(app), false;
    o.b1 = d[0x14] != 0;
    o.subtitles = d[0x15] != 0;
    o.vibration = d[0x16] != 0;
    o.invert = d[0x17] != 0;
    std::memcpy(app.levelTargets.raw.data(), d + 0x18, kOptionsRecordsSize);
    o.dirty = false;
    app.Sound().SetCategoryVolume(0, o.sfx);          // sound system vf28
    app.Sound().SetCategoryVolume(2, o.music);        // vf38
    app.Sound().SetCategoryVolume(1, o.speech);       // vf30
    ApplyScreen(app);                                 // 0x000A1B10
    return true;
}

// ---- the preview picker ----------------------------------------------------------------------------------------

PreviewPicker::PreviewPicker(App& app) : app(app) {   // orig: 0x00026BF0
    // TextManager::FormatLocalized 0x000D9DF0 (the language for the %s) of 0x00294D04 .. 0x00294B78 and 0x00294B4C ..
    // 0x00294A18, each copied (new(strlen + 1))
    static const char* const kSfx[10] = {"01", "02", "03", "04", "05", "06", "07", "08", "09", "10"};
    static const char* const kVoices[8] = {"6dz1-01", "6fr7-01", "6fr7-02", "6fr7-03",
                                           "6pf1-01", "6zb1-01", "6zb1-02", "6zb1-03"};
    const std::string lang = app.text ? app.text->Language() : std::string("English");
    for (size_t i = 0; i < sfxNames.size(); ++i)
        sfxNames[i] = "sounds/localized/" + lang + "/menu/sfx/6fr8-" + kSfx[i] + ".wav";
    for (size_t i = 0; i < voiceNames.size(); ++i)
        voiceNames[i] = "sounds/localized/" + lang + "/menu/voice/" + kVoices[i] + ".wav";
    sfxExcluded = 0, speechExcluded = 0;              // +0x50 / +0x54
}

s32 PreviewPicker::PickSfx() const {                  // orig: 0x000270D0
    s32 i = crt::Rand() % 10;                         // CDQ; IDIV 10
    while (i == sfxExcluded) i = crt::Rand() % 10;
    return i;
}

s32 PreviewPicker::PickSpeech() const {               // orig: 0x00027110 (AND 0x80000007: the signed % 8)
    s32 j = crt::Rand() % 8;
    while (j == speechExcluded) j = crt::Rand() % 8;
    return j;
}

bool PreviewPicker::SfxPlaying() const { return app.Sound().VoiceExists(sfxVoice); }        // orig: 0x00027090
bool PreviewPicker::SpeechPlaying() const { return app.Sound().VoiceExists(speechVoice); }  // orig: 0x000270B0

bool PreviewPicker::PreviewSfx() {                    // orig: 0x00027380
    if (app.Sound().VoiceExists(sfxVoice)) return false;   // +0x60
    app.Sound().Release(sfxPlaying);                  // Sound_Release(+0x58)
    sfxPlaying = sfxNext;                             // +0x58 = +0x68
    sfxNext = app.LoadSound(sfxNames[size_t(PickSfx())], 0);   // Sound_Load(+0x00[i], 0) -> +0x68
    sfxVoice = app.Sound().PlaySfx(sfxPlaying, false);   // +0x60
    return true;
}

bool PreviewPicker::PreviewSpeech() {                 // orig: 0x000273E0
    if (app.Sound().VoiceExists(speechVoice)) return false;   // +0x64
    app.Sound().Release(speechPlaying);               // +0x5C
    speechPlaying = speechNext;                       // +0x5C = +0x6C
    speechNext = app.LoadSound(voiceNames[size_t(PickSpeech())], 0);   // +0x28[j]
    speechVoice = app.Sound().PlaySpeech(speechPlaying, false);   // +0x64
    return true;
}

}  // namespace fut::game::app

// The pad-combo recorder (cheat codes); see CheatCodes.h.
#include "game/app/CheatCodes.h"

#include "core/CrtString.h"
#include "core/XbeImage.h"
#include "game/app/App.h"
#include "game/app/Contexts.h"
#include "game/app/Extras.h"
#include "game/app/Frontend.h"
#include "game/app/MemoryCards.h"
#include "game/input/Pad.h"
#include "game/level/Camera.h"
#include "game/level/LevelData.h"
#include "game/level/LevelLoader.h"
#include "game/object/Character.h"

#include <bit>
#include <cstring>

namespace fut::game::app {
namespace {

// orig: 0x002D60D8 (29 pointers, then NULL): the codes; their index is 0x0002C530's argument. 0x002D615C (8-byte
// records: the carry-over text, the level's menu name; the names are not read here): the level codes 0..21 (5 and 6
// are 1-7 and 1-6 in the image's order). Both read from the user's default.xbe (core/XbeImage.h).
constexpr u32 kCodeTable = 0x002D60D8, kCodeCount = 29;
constexpr u32 kLevelTextTable = 0x002D615C;

LevelData* Level(App& app) {                          // orig: g_level DAT_002DAC64
    LoadedLevel* l = app.ingame ? app.ingame->Level() : nullptr;
    return l ? l->level.get() : nullptr;
}

}  // namespace

bool CheatRecorder::Update(App& app) {                // orig: 0x0002C890
    input::Pad* pad = app.input.activePad;            // DAT_002E7044
    if (pad && enabled && pad->IsDown(input::kLeftTrigger)) {   // Pad::IsDown(8)
        if (pending >= 0 && pad->JustReleased(static_cast<u32>(pending))) {
            Append(app, Digit(app, pending));         // 0x0002C460, 0x0002C840
            pending = -1;
            return true;
        }
        for (u32 i = 0; i < input::kButtonCount; ++i)   // the first button pressed this step (0..15)
            if (pad->JustPressed(i)) {
                pending = static_cast<s32>(i);
                break;
            }
        return true;
    }
    buffer[0] = 0;
    count = 0;
    pending = -1;
    return true;
}

s32 CheatRecorder::Digit(App& app, s32 button) {      // orig: 0x0002C460
    if (button < 0 || button >= 16) return -1;
    // a local table of 16: -1 but Back (11) 10, White (14) 9, Black (15) 0 and X B Y A (4..7) 4 2 1 3, or with the
    // right trigger held (Pad::IsDown(9) on DAT_002E7044, read unchecked) 8 6 5 7
    s32 t[16] = {-1, -1, -1, -1, 0, 0, 0, 0, -1, -1, -1, 10, -1, -1, 9, 0};
    const bool shift = app.input.activePad && app.input.activePad->IsDown(input::kRightTrigger);
    t[6] = shift ? 5 : 1;
    t[5] = shift ? 6 : 2;
    t[7] = shift ? 7 : 3;
    t[4] = shift ? 8 : 4;
    return t[button];
}

void CheatRecorder::Append(App& app, s32 digit) {     // orig: 0x0002C840
    if (digit == 10) {
        Check(app);                                   // 0x0002C7D0
    } else if (digit >= 0 && count != 0x28) {         // (JL / CMP 0x28; JE: the buffer is cleared)
        buffer[count] = static_cast<char>(digit + '0');   // ADD AL, 0x30
        ++count;
        buffer[count] = 0;
        return;
    }
    buffer[0] = 0;
    count = 0;
    pending = -1;
}

void CheatRecorder::Check(App& app) {                 // orig: 0x0002C7D0
    const xbe::XbeImage& image = xbe::GameImage();
    for (s32 i = 0; i < static_cast<s32>(kCodeCount); ++i)   // (the inline strcmp: byte pairs)
        if (std::strcmp(image.StringAt(kCodeTable + 4 * static_cast<u32>(i)), buffer) == 0) {
            Apply(app, i);                            // 0x0002C530
            return;
        }
}

void CheatRecorder::Apply(App& app, s32 index) {      // orig: 0x0002C530
    f32 r = 0.0f, g = 0.0f, b = 1.0f;                 // esp+0x10 / +0x0C / +0x08: the fade colour
    if (index < 0x16) {                               // the level codes (JGE: signed)
        if (app.IsMoviePlaying()) return;             // vf1C 0x000A2550
        const char* text = xbe::GameImage().StringAt(kLevelTextTable + 8 * static_cast<u32>(index));
        const size_t n = std::strlen(text) + 1;
        const std::optional<std::string> name = CarryLevelValue(text, n);   // 0x000C1E10 (a heap copy)
        SetCarryState(app.carry, reinterpret_cast<const u8*>(text), n);     // 0x000C1DC0
        app.SetLevelName(name ? name->c_str() : nullptr);   // 0x000A23D0
        Context* cur = app.Current();                 // App+0xBC (its name +4 read unchecked)
        if (cur && crt::StrICmp(cur->name.c_str(), "frontend") == 0) {   // _stricmp 0x00295598
            app.RequestContext("Ingame", 0x0002C5E0); // 0x000A3160(0x0029416C)
            if (auto* fe = dynamic_cast<FrontendContext*>(app.Current())) fe->stay = false;   // (App+0xBC)+0x4C = 0
        }
        return;                                       // (the copy freed; no fade)
    }
    LevelData* level = Level(app);
    switch (index - 0x16) {                           // the jump table 0x0002C7B4 (JA: above 6 to the fade)
    case 0:                                           // code 22: the health
        if (!level) return;
        if (level->player) level->player->health = 100.0f;   // +0x4F4 = 0x42C80000
        break;
    case 1:                                           // code 23: every ammo slot 999
        if (!level) return;
        if (level->player)
            for (s32 i = 0; i < 5; ++i) level->player->SetAmmo(i, 999);   // 0x00051D50(i, 0x3E7)
        break;
    case 2:                                           // code 24: five more lives, a white fade
        if (!level) return;
        level->lives = level->lives + 5;              // 0x000C12C0(0x000C12B0() + 5) (DAT_002D687C)
        r = g = b = 1.0f;
        break;
    case 3:                                           // code 25: charge
        if (!level) return;
        if (level->player) {
            (void)level->player->AddCharge(10000.0f); // vf15C(0x461C4000)
            r = 0.0f, g = 1.0f, b = 1.0f;             // (0x0002C6DB, then 0x0002C69E)
        }
        break;
    case 4:                                           // code 26: the invulnerability; falls into case 5
        if (level && level->player) {
            Player* p = level->player;
            p->ToggleInvulnerable();                  // vf174 0x000505D0
            b = 0.0f;
            // FLD +0x4F4; FCOMP [0x00295594] 55555.0; TEST AH,0x44; JNP: green when now invulnerable, else red
            if (p->health == std::bit_cast<f32>(0x47590300u)) r = 0.0f, g = 1.0f;
            else r = 1.0f, g = 0.0f;
        }
        [[fallthrough]];
    case 5:                                           // code 27: the extras' unlock-all flag
        if (app.extras) app.extras->allUnlocked = app.extras->allUnlocked == 0;   // DAT_002D9920 = SETE
        break;
    case 6:                                           // code 28: the intermission's skip flag
        app.intermission.skipAll = !app.intermission.skipAll;   // DAT_002EB874 = SETE
        break;
    default:
        break;
    }
    if (level && level->camera)                       // S+0x8C: 0x00013130(r, g, b, 0.5, 0.1, 0.2, 0.2)
        level->camera->FadeTo(r, g, b, 0.5f, std::bit_cast<f32>(0x3DCCCCCDu), std::bit_cast<f32>(0x3E4CCCCDu),
                              std::bit_cast<f32>(0x3E4CCCCDu));
}

}  // namespace fut::game::app

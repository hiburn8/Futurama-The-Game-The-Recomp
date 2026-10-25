// The Intermission (orig: singleton 0x000D4E50 DAT_002EB868, ctor 0x000D3850, 0x148 bytes, vtbl 0x002A4060): the
// level script's cutscenes. Ported: the AVI path as the frame loop sees it (natives 165 / 166, the per-step update
// vf08 0x000D61B0 -> 0x000D5BB0, the end 0x000D5790, the skip test 0x000D31B0); the movie is played through
// `playMovie` / `isMoviePlaying` (App vtbl+0x18 0x000A2410 / +0x1C 0x000A2550). NOT PORTED: RTM cutscenes (0x000D4AD0,
// the "cutscene" node, 0x000D5C40, FsPlayRTM 0x000D3660), the alpha fade (+0xE4 == 0, never set: its quads in vf0C),
// the SYSTEM events (no handler), Sound_PauseAll / Sound_ResumeAll. The movie camera (+0x3C, 0x000D3DE0) and the
// render vf0C 0x000D6370 are ported (an empty camera's Click). docs/analysis/intermission_port.md.
#pragma once

#include "core/Types.h"
#include "engine/ni/NiObjects.h"

#include <array>
#include <functional>
#include <string>

namespace fut::input { class InputSystem; }
namespace fut::ni { class NiRenderer; }

namespace fut::game {

class Intermission {
public:
    // orig: the view table 0x002D6DB0 (static in the original: it survives levels; here it lives as long as this object)
    struct Views {
        u8 current = 0;
        u32 views = 0;
        const char* name;
    };
    std::array<Views, 2> table{{{0, 0, "1-1-01.avi"}, {0, 0, "1-1-02.avi"}}};

    bool active = false;                              // orig: +0x34 (0x000D31A0)
    bool fadePhase = true;                            // orig: +0xE0 (ctor 1); 0x000D32E0 returns !fadePhase
    u32 fadeMode = 1;                                 // orig: +0xE4 (only the ctor writes it: 1 = no fade)
    bool nullName = false;                            // orig: +0xE8
    bool movie = false;                               // orig: +0x140 (an .avi cutscene)
    bool movieStarted = false;                        // orig: +0x141
    bool skippable = true;                            // orig: +0x2D (ctor 1)
    f32 fade = 0;                                     // orig: +0x11C
    u32 fadeTargetBits = 0x3DCCCD38u;                 // orig: +0x120 (raw dword; 0.125f / 0 written as floats)
    u64 startTicks = 0;                               // orig: +0x138 (QueryPerformanceCounter at 166)
    std::string movieName;                            // orig: +0x90 (char[0x50], the name up to its first '.')
    bool skipAll = false;                             // orig: DAT_002EB874 (debug pad combo 0x0002C530)

    // App services (orig: App vtbl+0x18 PlayMovie, +0x1C IsMoviePlaying; the clock 0x000D2C20). Without a movie
    // player both report false, which is what the original does under the oracle's emulator (no D3D device).
    std::function<bool(const char* name)> playMovie;  // nullptr name = stop (orig: Sound_ResumeAll)
    std::function<bool()> isMoviePlaying;
    std::function<u64()> queryCounter;
    u64 counterFrequency = 1000000;
    input::InputSystem* input = nullptr;              // (Input_SetRumbleEnabled)
    ni::NiRenderer* renderer = nullptr;               // (App->vf14(): the movie camera's renderer)
    ni::NiPointer<ni::NiCamera> camera;               // orig: +0x3C (0x000D3DE0, the AVI path; released by End)

    void Prepare(const char* name);                   // orig: 0x000D5810 (native 165)
    void Start();                                     // orig: 0x000D4950 (native 166)
    bool Update();                                    // orig: vf08 0x000D61B0
    // orig: vf0C 0x000D6370 (Session::Render while active with a movie): the fade quads (+0x118 / +0x119, mode 0:
    // NOT PORTED, never set), the SYSTEM "PRERENDER" event (no handler), the renderer's clear vf44(0, 5) and the
    // outline pool's reset unless +0x140, the camera +0x3C's Click (DAT_002F90D4 = 1 around it), +0x40's (never
    // set), the "POSTRENDER" event.
    void Render();
    void End();                                       // orig: 0x000D5790
    bool Skippable() const;                           // orig: 0x000D31B0
    bool FadeDone() const { return fadeMode == 1; }   // orig: 0x000D3710 (mode 0 NOT PORTED: never set)

private:
    void BeginActive();                               // orig: 0x000D3560
    ni::NiCamera* MakeCamera() const;                 // orig: 0x000D3DE0
    void UpdateMovie();                               // orig: 0x000D5BB0
};

}  // namespace fut::game

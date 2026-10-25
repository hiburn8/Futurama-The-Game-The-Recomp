// The pad-combo recorder (cheat codes), decompiled from default.xbe (0x0002C440-0x0002C920; the App's DAT_002E7C1C).
//
// Every fixed step of the App (OnIdle, after the step services) the recorder looks at the active pad: while it is
// enabled (+0x34: set by the Ingame context's render, cleared while the pause menu or the end-of-level menu shows and
// at the Ingame exit) and the left trigger is held, a button pressed and released appends a digit (Y B A X = 1 2 3 4,
// with the right trigger held 5 6 7 8, White 9, Black 0); Back ends the code, which is compared with the 29 codes of
// the image (0x002D60D8) and applied when one matches: the 22 level codes give the carry-over text of a level start
// and the level name (the front end would leave for Ingame), the others restore the health, fill the ammo, add lives,
// add charge, toggle the invulnerability, the extras' unlock-all flag or the intermission's skip flag; every applied
// code (with a level) fades the camera.
#pragma once

#include "core/Types.h"

namespace fut::game::app {

class App;

class CheatRecorder {                                 // orig: new(0x38) 0x0002C440 (App init, DAT_002E7C1C)
public:
    // orig: 0x0002C890 (thiscall, returns 1): the step's pad check.
    bool Update(App& app);
    // orig: 0x0002C460 (thiscall(button), RET 4): the digit of a pad button (-1 none, 10 the end of a code).
    static s32 Digit(App& app, s32 button);
    // orig: 0x0002C840 (thiscall(digit), RET 4): appended; 10 checks the code, then (and on -1 or a full buffer)
    // the buffer is cleared.
    void Append(App& app, s32 digit);
    // orig: 0x0002C7D0: the first code of 0x002D60D8 equal to the buffer (strcmp) is applied.
    void Check(App& app);
    // orig: 0x0002C530 (thiscall(index), RET 4).
    static void Apply(App& app, s32 index);

    char buffer[0x2C] = {};                           // orig: +0x00 (at most 0x28 digits and the NUL)
    s32 count = 0;                                    // orig: +0x2C
    s32 pending = -1;                                 // orig: +0x30 (the button pressed, waiting for its release)
    u8 enabled = 0;                                   // orig: +0x34
};

}  // namespace fut::game::app

// The game's C runtime rand(), decompiled from default.xbe (0x0016695C): the Microsoft CRT linear congruential
// generator. The state lives in the CRT's per-thread data (+0x14, 0x00169360); the game runs it on one thread, so the
// port keeps one global state. Initial seed 1 (the CRT default; the game's srand calls, if any, set it).
#pragma once

#include "core/Types.h"

namespace fut::crt {

inline u32 g_randSeed = 1;                             // orig: _getptd() +0x14

inline void SeedRand(u32 seed) { g_randSeed = seed; }  // orig: srand

inline s32 Rand() {                                    // orig: 0x0016695C, 0..0x7FFF
    g_randSeed = g_randSeed * 0x343FDu + 0x269EC3u;
    return static_cast<s32>((g_randSeed >> 16) & 0x7FFF);
}

}  // namespace fut::crt

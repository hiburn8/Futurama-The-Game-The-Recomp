// Game timing globals, decompiled from default.xbe (docs/ARCHITECTURE.md section 2).
#pragma once

#include "core/Types.h"

namespace fut::game {

// orig: g_frameStep (DAT_002D67E8), the fixed logic step set by the game init 0x000A2730: 1/60 s (0x3C888889) on
// NTSC, 0.02 s on PAL. More than 100 functions read it directly as dt; the port runs the NTSC step.
inline f32 g_frameStep = 1.0f / 60.0f;

}  // namespace fut::game

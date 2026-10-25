// The game's random helpers over the CRT rand() (core/CrtRand.h), decompiled from default.xbe.
#pragma once

#include "core/CrtRand.h"
#include "core/Types.h"

namespace fut::game {

// orig: RandUnit 0x0002CE30: (int)rand() * [0x002955E8] (3.05185094e-05f, 0x38000100), left on the x87 stack.
inline double RandUnit() { return double(crt::Rand()) * double(3.05185094e-05f); }

// orig: RandRange 0x000B6FB0 (cdecl(lo, hi)): RandUnit(); FLD hi; FSUB lo; FMULP; FADD lo (double in ST0).
inline double RandRange(f32 lo, f32 hi) {
    const double r = RandUnit();
    return (double(hi) - lo) * r + lo;
}

}  // namespace fut::game

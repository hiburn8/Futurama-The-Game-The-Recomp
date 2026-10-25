// The game's C runtime math, decompiled from default.xbe (the Microsoft CRT's x87 routines).
#pragma once

#include <cmath>

namespace fut::crt {

// orig: acos 0x001673E4 -> 0x00167401 for |c| < 1: FPATAN(sqrt((1 + c) * (1 - c)), c), the operands at 53-bit
// precision; the FPATAN result itself is extended (KNOWN_ISSUES #35: the C library atan2 in double).
inline double Acos(double c) { return std::atan2(std::sqrt((1.0 + c) * (1.0 - c)), c); }

}  // namespace fut::crt

// Script natives, decompiled from default.xbe (Script_CallNative 0x000B0CD0: class id, method, &data-stack sp).
//
// Ported so far: the core natives of class 0, methods 3-60 - string compare / search, the int / float / bool
// operators and conversions the script compiler emits, sqrt / sin / cos / abs and the ranged random number. Each pops
// its operands from the calling thread's data stack and pushes one result (32-bit slots, VM addresses).
//
// Verified against the original dispatcher under emulation (tests/game/ScriptNativesTests.cpp, oracle
// tools/difftest/natives_oracle.py).
#pragma once

#include "core/Types.h"
#include "game/script/ScriptVM.h"

namespace fut::script {

// orig: the class-0 cases 3-60 of 0x000B0CD0. Returns false for a method it does not handle (sp unchanged).
bool CallCoreNative(u32 method, VmAddr& sp);

// orig: 0x000B0140, the scripts' string compare: 0 for the same pointer, 1 when one is null, else -1 / 0 / 1 by the
// first differing byte (unsigned).
s32 ScriptStrCmp(VmAddr a, VmAddr b);

// orig: the CRT float -> int conversion 0x00167110 (x87 64-bit integer store with truncation, low 32 bits kept).
s32 FloatToIntCrt(f32 value);

}  // namespace fut::script

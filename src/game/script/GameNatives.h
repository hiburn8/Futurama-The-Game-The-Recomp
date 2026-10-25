// The level scripts' calls into the game: Script_CallNative 0x000B0CD0 beyond the core operators (ScriptNatives.h).
// Each native pops its arguments from the calling thread's data stack and pushes its results (sizes:
// NativeTable.inc, generated from the dispatcher probe). Ported natives are listed in GameNatives.cpp; every other
// legal native keeps the stack balanced (pops its arguments, pushes zeros) and reports "NOT PORTED native <name>"
// once on stderr, so the scripts run on with neutral results.
#pragma once

#include "core/Types.h"
#include "game/script/ScriptVM.h"

#include <functional>

namespace fut::game {
class LevelData;
}

namespace fut::script {

struct NativeInfo {
    u32 cid, method, pop, push;
    const char* name;
};

// The table entry of a legal native, nullptr for an illegal one.
const NativeInfo* FindNative(u32 cid, u32 method);

// The level the natives act on (orig: the global g_level 0x002DAC64); set by Session::Start.
void SetNativeLevel(game::LevelData* level);
game::LevelData* NativeLevel();

// The VM's native handler (orig: Script_CallNative 0x000B0CD0).
void CallGameNative(u32 cid, u32 method, VmAddr& sp);

// The natives' process statics a fresh boot starts with (orig: native 330's line-of-sight cache 0x002E9FD0..
// 0x002E9FEC): for a test that replays several runs in one process.
void ResetNativeStatics();

}  // namespace fut::script

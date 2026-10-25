// Basic fixed-width types and conventions shared by all decompiled code.
//
// Every function ported from the original executable carries a comment of the form
//     // orig: 0x000A45B0 MemoryManager::Alloc
// giving its address in the Xbox default.xbe (NTSC-U, XDK 5233), which is also its name in the Ghidra database.
#pragma once

#include <cstddef>
#include <cstdint>

namespace fut {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
static_assert(sizeof(f32) == 4);

// Reports an unrecoverable error and terminates. Used where the original would hang or crash.
[[noreturn]] void FatalError(const char* fmt, ...);

}  // namespace fut

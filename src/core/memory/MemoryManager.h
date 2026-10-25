// Game heap: decompiled from memory.cpp ("C:\Code\Futurama\Futurama\Memory\memory.cpp"),
// default.xbe 0x000A3190-0x000A49F0. See docs/ARCHITECTURE.md and the Ghidra plate comments.
//
// One contiguous heap (0x2600000 bytes on Xbox) with two bump-allocated regions:
//   low region  grows upward from heapBase    (small requests, < 0x123A bytes)
//   high region grows downward from heapEnd   (large requests)
// Freed blocks go to per-size-class bins (134 classes per region). Each bin is a binary heap of block
// addresses (min-heap for the low region, max-heap for the high region) so allocations prefer the
// lowest/highest address, keeping each region compact. Adjacent free blocks are merged on free and
// periodically by CoalesceLow/CoalesceHigh, which also retract the bump cursors.
//
// Block layout: u32 header at payload-4
//   bit 31     free
//   bit 0      block belongs to the high region (only reliable for free blocks; see FreeBlock)
//   bits 1-30  payload size; total block = payload + 4, a multiple of 16: ((request + 0x13) & ~0xF)
//
// Port notes:
//   * Algorithm, constants and quirks are reproduced exactly, verified by tests/core/MemoryManagerTests.cpp
//     against vectors produced by running the original x86 code (tools/difftest/gen_memory_vectors.py).
//   * Everything stored inside the heap (free-list links, bin arrays, bin descriptors) is a 32-bit offset
//     from the start of a 64 KB-aligned region instead of a pointer, so all in-heap structures keep their
//     original sizes on x64. heapBase sits 0x10 bytes into the region (the original's first block header
//     lives at heapBase-4, inside the Win32 heap entry header that precedes the HeapAlloc block).
//   * Out of memory: the original hangs forever; the port calls FatalError (docs/KNOWN_ISSUES.md).
//   * The heap size is configurable (SetHeapSize) because x64 objects are larger than the originals.
#pragma once

#include "core/Types.h"

namespace fut::mem {

constexpr u32 kXboxHeapSize = 0x2600000;      // orig: ReserveHeap(0x2600000) in MemoryManager::Alloc
constexpr u32 kDefaultAlign = 16;             // orig: g_DefaultAllocAlign (0x002D67F0)

// ---- public API (original free functions) --------------------------------------------------------------
void* OperatorNew(u32 size);                  // orig: 0x000A4990 operator_new
void* AllocAligned(u32 size, u16 align);      // orig: 0x000A4970 Mem_AllocAligned
void OperatorDelete(void* p);                 // orig: 0x000A3910 operator_delete
u32 BlockSize(const void* p);                 // orig: 0x000A3280 Mem_BlockSize (payload + 4)
void FrameTick();                             // orig: 0x000A4760 Mem_FrameTick
int CoalesceAll();                            // orig: 0x000A4950 Mem_CoalesceAll
void ReallocBinArrays();                      // orig: 0x000A4910 Mem_ReallocBinArrays
void SetUnknownFlag(u8 value);                // orig: 0x000A3270 Mem_SetUnknownFlag (reader unknown)
bool ReturnTrue();                            // orig: 0x000A32A0 Mem_ReturnTrue
void GetLargestFreeBlocks(s32* out, s32 count);  // orig: 0x000A3A60 Mem_GetLargestFreeBlocks

// ---- 32-bit VM addresses ----------------------------------------------------------------------------------
// The script VM (and any other code that stored raw 32-bit pointers on Xbox) refers to heap memory through
// 32-bit addresses: VmAddr = (p - regionStart) + bias. 0 is null. The bias is configurable so differential tests
// can reproduce the absolute addresses seen by the original code under emulation.
u32 ToVmAddr(const void* p);
void* FromVmAddr(u32 addr);
void SetVmAddrBias(u32 bias);
bool IsVmAddr(u32 addr);                      // (port) inside the game heap's VM address range

// ---- port helpers -----------------------------------------------------------------------------------------
struct Stats {
    u32 bytesUsed;     // MemoryManager::bytesUsed
    u32 bytesFree;     // MemoryManager::bytesFree
    u32 lowCursor;     // offset from heapBase
    u32 highCursor;    // offset from heapBase
    u32 peakBytesUsed; // g_MemPeakBytesUsed
};
Stats GetStats();
u32 HeapOffset(const void* p);                // p - heapBase (diagnostics and tests)
u32 RawHeader(const void* p);                 // header word at p-4
void SetHeapSize(u32 bytes);                  // must be called before the first allocation
void ResetForTests();                         // release the heap and return to the pristine state

}  // namespace fut::mem

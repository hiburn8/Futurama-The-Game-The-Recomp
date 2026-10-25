// Decompiled from default.xbe memory.cpp (0x000A3190-0x000A49F0). See MemoryManager.h for the design notes.
// Variable names follow the Ghidra database (types MemoryManager / FreeBin, globals g_Mem*).
#include "core/memory/MemoryManager.h"

#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdlib>
#endif

namespace fut::mem {
namespace {

// Offset from the start of the 64 KB-aligned heap region. 0 is never a valid block (heapBase = 0x10).
using Off = u32;

constexpr u32 kHeapSlack = 0x10;              // bytes before heapBase (see header comment)
constexpr u32 kReserveExtra = 0x20;           // orig: HeapAlloc(size + 0x20)
constexpr s32 kNumBins = 0x86;                // orig: InitBins: numBins = 0x86
constexpr s32 kInitialBinCapacity = 0xFF;     // orig: InitBins: capacity = 0xff (array of 0x3fc bytes)
constexpr u32 kBinCountGuard = 0xF00DBEEF;    // orig: InitBins writes this into the last count slot
constexpr s32 kLargeRequest = 0x123A;         // orig: Alloc: requests >= this start in the high region
constexpr s32 kLowInsertsBeforeCoalesce = 2000;   // orig: Alloc: if (2000 < g_MemLowBinInserts)
constexpr s32 kHighInsertsBeforeCoalesce = 0x14;  // orig: Alloc: if (0x14 < g_MemHighBinInserts)
constexpr u32 kSizeMask = 0x7FFFFFFE;
constexpr u32 kFreeBit = 0x80000000;
constexpr u32 kHighRegionBit = 1;

// FreeBin field offsets (struct FreeBin, 0x14 bytes, lives inside the heap).
constexpr u32 kBinHeap = 0x00, kBinPending = 0x04, kBinDefer = 0x08, kBinCapacity = 0x0C, kBinCount = 0x10;
constexpr u32 kBinSize = 0x14;

// ---- state: MemoryManager (0x002E7E00) and the g_Mem* globals ---------------------------------------------
struct State {
    u8* region = nullptr;      // start of the 64 KB-aligned allocation (port only)
    u32 heapSize = kXboxHeapSize;
    // MemoryManager
    u32 bytesUsed = 0, bytesFree = 0, unk08 = 0;
    Off heapBase = 0, heapEnd = 0, lowCursor = 0, highCursor = 0;
    Off lowBins = 0, highBins = 0;
    s32 numBins = 0;
    Off lowBinCounts = 0, highBinCounts = 0;
    // globals
    bool initialized = false;          // g_MemInitialized
    u8 unknownFlag = 0;                // g_MemUnknownFlag
    u32 peakBytesUsed = 0;             // g_MemPeakBytesUsed
    bool coalescing = false;           // g_MemCoalescing
    s32 lowBinInserts = 0;             // g_MemLowBinInserts
    s32 highBinInserts = 0;            // g_MemHighBinInserts
    s32 framesSinceLowCoalesce = 0;    // g_MemFramesSinceLowCoalesce
    s32 framesSinceHighCoalesce = 0;   // g_MemFramesSinceHighCoalesce
};
State g;

// ---- raw heap access --------------------------------------------------------------------------------------
u32& W(Off o) { return *reinterpret_cast<u32*>(g.region + o); }
s32& S(Off o) { return *reinterpret_cast<s32*>(g.region + o); }
u8& B(Off o) { return g.region[o]; }
u32& Hdr(Off block) { return W(block - 4); }
u32 PayloadSize(u32 header) { return header & kSizeMask; }
bool IsFree(u32 header) { return static_cast<s32>(header) < 0; }

Off BinAt(Off bins, u32 sizeClass) { return bins + sizeClass * kBinSize; }
s32& Count(Off counts, u32 sizeClass) { return S(counts + sizeClass * 4); }
Off HeapEntry(Off bin, s32 index) { return W(bin + kBinHeap) + static_cast<u32>(index) * 4; }

bool InLowRegion(Off p) { return p < g.lowCursor && g.heapBase <= p; }
bool InHighRegion(Off p) { return p < g.heapEnd && g.highCursor <= p; }

void* ToPtr(Off o) { return o ? g.region + o : nullptr; }
Off ToOff(const void* p) { return p ? static_cast<Off>(static_cast<const u8*>(p) - g.region) : 0; }

// orig: 0x000A3190 Mem_FloorLog2
s32 FloorLog2(u32 v) {
    s32 r = 0;
    if (v & 0xFFFF0000) { r = 0x10; v &= 0xFFFF0000; }
    if (v & 0xFF00FF00) { r += 8; v &= 0xFF00FF00; }
    if (v & 0xF0F0F0F0) { r += 4; v &= 0xF0F0F0F0; }
    if (v & 0xCCCCCCCC) { r += 2; v &= 0xCCCCCCCC; }
    if (v & 0xAAAAAAAA) { r += 1; }
    return r;
}

// orig: 0x000A31E0 Mem_SizeClass (also inlined in UnlinkFreeBlock, FreeBlock and AllocFrom*Bins)
u32 SizeClass(u32 size) {
    if (size < 0x400) return (size + 3) >> 4;
    s32 l = FloorLog2(size);
    return ((size >> ((l - 2) & 0x1F)) & 3 | static_cast<u32>(l) << 2) + 0x18;
}

Off Alloc(u32 size, u16 align);
s32 FreeBlock(Off ptr);

// Free an internal array the same way operator_delete does (stats + FreeBlock).
void FreeInternal(Off p) {
    g.bytesUsed += static_cast<u32>(-4) - PayloadSize(Hdr(p));
    g.bytesFree += 4 + PayloadSize(Hdr(p));
    FreeBlock(p);
}

// orig: 0x000A32B0 FreeBin::InsertHigh (high=true, max-heap by address)
// orig: 0x000A34D0 FreeBin::InsertLow  (high=false, min-heap by address)
void Insert(Off bin, Off block, bool high) {
    u8 defer = B(bin + kBinDefer);
    for (;;) {
        (high ? g.highBinInserts : g.lowBinInserts) += 1;
        if (defer) {
            // Array is being reallocated: push onto the pending list (head's prev link = itself).
            Off pending = W(bin + kBinPending);
            if (pending) W(pending + 4) = block;
            W(block) = pending;
            W(bin + kBinPending) = block;
            W(block + 4) = block;
            return;
        }
        if (S(bin + kBinCount) == S(bin + kBinCapacity)) {
            B(bin + kBinDefer) = 1;
            Insert(bin, block, high);                 // lands on the pending list
            if (g.coalescing) {                       // quirk: no growth while coalescing
                B(bin + kBinDefer) = 0;
                return;
            }
            Off old = W(bin + kBinHeap);
            s32 capacity = S(bin + kBinCapacity) * 2 + 1;
            S(bin + kBinCapacity) = capacity;
            W(bin + kBinHeap) = Alloc(static_cast<u32>(capacity) * 4, 0x10);  // memory.cpp line 0xDF1 / 0xF10
            for (s32 i = 0; i < S(bin + kBinCount); ++i) W(HeapEntry(bin, i)) = W(old + static_cast<u32>(i) * 4);
            if (old) FreeInternal(old);
            B(bin + kBinDefer) = 0;
        } else {
            Off arr = W(bin + kBinHeap);
            s32 i = S(bin + kBinCount);
            while (0 < i) {                           // sift up
                s32 parent = (i - 1) >> 1;
                Off p = W(arr + static_cast<u32>(parent) * 4);
                if (high ? block <= p : p <= block) break;
                W(arr + static_cast<u32>(i) * 4) = p;
                W(W(HeapEntry(bin, i))) = static_cast<u32>(i);   // block[0] = heap index
                i = parent;
            }
            W(HeapEntry(bin, i)) = block;
            W(block + 4) = 0;                         // block[1] = 0: "in heap"
            W(W(HeapEntry(bin, i))) = static_cast<u32>(i);
            S(bin + kBinCount) += 1;
        }
        block = W(bin + kBinPending);
        if (!block) return;
        W(bin + kBinPending) = W(block);
        defer = B(bin + kBinDefer);
    }
}

// orig: 0x000A3400 FreeBin::RemoveAtHigh / 0x000A3620 FreeBin::RemoveAtLow
void RemoveAt(Off bin, s32 index, bool high) {
    if (index < 0 || S(bin + kBinCount) <= index) return;
    Off arr = W(bin + kBinHeap);
    auto at = [&](s32 i) -> u32& { return W(arr + static_cast<u32>(i) * 4); };
    auto setIndex = [&](s32 i) { W(W(HeapEntry(bin, i))) = static_cast<u32>(i); };
    s32 last = S(bin + kBinCount) - 1;
    S(bin + kBinCount) = last;
    if (last == index) return;
    Off moved = at(last);
    while (0 < index) {                               // sift up
        s32 parent = (index - 1) >> 1;
        Off p = at(parent);
        if (high ? moved <= p : p <= moved) break;
        at(index) = p;
        setIndex(index);
        index = parent;
    }
    s32 child = index * 2 + 1;
    if (child < S(bin + kBinCount)) {                 // sift down
        do {
            s32 right = index * 2 + 2;
            if (right == S(bin + kBinCount)) {
                Off c = at(child);
                if (high ? moved < c : c < moved) {
                    at(index) = c;
                    setIndex(index);
                    index = child;
                }
                break;
            }
            Off l = at(child), r = at(right);
            if (high ? r < l : l < r) {
                if (high ? l <= moved : moved <= l) break;
                at(index) = l;
                setIndex(index);
                right = child;
            } else {
                if (high ? r <= moved : moved <= r) break;
                at(index) = r;
                setIndex(index);
            }
            child = right * 2 + 1;
            index = right;
        } while (child < S(bin + kBinCount));
    }
    at(index) = moved;
    setIndex(index);
}

// orig: 0x000A3A80 MemoryManager::UnlinkFreeBlock
void UnlinkFreeBlock(Off block) {
    u32 cls = SizeClass(PayloadSize(Hdr(block)));
    bool high = (Hdr(block) & kHighRegionBit) != 0;
    Off bins = high ? g.highBins : g.lowBins;
    Off counts = high ? g.highBinCounts : g.lowBinCounts;
    Off prev = W(block + 4);
    if (prev == 0) {                                  // in the bin heap; block[0] = index
        RemoveAt(BinAt(bins, cls), S(block), high);
    } else if (prev == block) {                       // head of the pending list
        Off next = W(block);
        W(BinAt(bins, cls) + kBinPending) = next;
        if (next) W(next + 4) = next;
    } else {                                          // inside the pending list
        W(prev) = W(block);
        if (W(block)) W(W(block) + 4) = W(block + 4);
    }
    Count(counts, cls) -= 1;
}

// orig: 0x000A3F40 MemoryManager::FreeBlock
s32 FreeBlock(Off ptr) {
    Off limit = InLowRegion(ptr) ? g.lowCursor : g.heapEnd;
    u32 h = Hdr(ptr);
    for (;;) {                                        // absorb following free blocks
        Off next = PayloadSize(h) + ptr;              // header of the next block
        if (next == 0 || !(next + 4 < limit) || !IsFree(W(next))) break;
        UnlinkFreeBlock(next + 4);
        u32 hn = W(next);
        u32 hp = Hdr(ptr);
        W(next) = (hn & 1) | 0xFCFCFCFC;
        h = PayloadSize(hp) + 4 + PayloadSize(hn);
        Hdr(ptr) = (Hdr(ptr) & (kFreeBit | 1)) | h;
    }
    W(ptr) = 0;
    W(ptr + 4) = 0;
    h = Hdr(ptr);
    Hdr(ptr) = h | kFreeBit;
    if (g.highCursor == ptr) {                        // top of the high stack: pop it
        g.highCursor = PayloadSize(h) + 4 + ptr;
        return 1;
    }
    u32 cls = SizeClass(PayloadSize(Hdr(ptr)));
    if (InLowRegion(ptr)) {
        Hdr(ptr) = (h & ~1u) | kFreeBit;
        Insert(BinAt(g.lowBins, cls), ptr, false);
        Count(g.lowBinCounts, cls) += 1;
    } else if (InHighRegion(ptr)) {
        Hdr(ptr) = h | kFreeBit | kHighRegionBit;
        Insert(BinAt(g.highBins, cls), ptr, true);
        Count(g.highBinCounts, cls) += 1;
    }
    return 1;
}

// orig: 0x000A3840 MemoryManager::AllocFromLowCursor
Off AllocFromLowCursor(u32 size, s32 align) {
    const u32 mask = static_cast<u32>(align) - 1;
    Off cursor = g.lowCursor;
    Off p = cursor;
    Off padding = 0;
    if (cursor & mask) {
        p = (cursor + static_cast<u32>(align)) & ~mask;
        if (p - 4 - cursor < 8) p += static_cast<u32>(align);
        Hdr(cursor) = (p - 4 - cursor) | (Hdr(cursor) & 1) | kFreeBit;   // padding becomes a free block
        padding = cursor;
    }
    Off newCursor = (p + size + 0x13) & 0xFFFFFFF0;
    if (g.highCursor < newCursor) return 0;
    g.lowCursor = newCursor;
    u32 h = Hdr(p);
    Hdr(p) = h & 0x7FFFFFFF;
    Hdr(p) = (h & 1) | (g.lowCursor - 4 - p);
    if (padding) {
        u32 hp = Hdr(padding);
        Hdr(padding) = hp & ~1u;
        u32 cls = SizeClass(hp & kSizeMask);
        Insert(BinAt(g.lowBins, cls), padding, false);
        Count(g.lowBinCounts, cls) += 1;
    }
    return p;
}

// orig: 0x000A3220 MemoryManager::AllocFromHighCursor
Off AllocFromHighCursor(u32 size, s32 align) {
    Off p = (g.highCursor - 4 - size) & ~(static_cast<u32>(align) - 1);
    if (p < g.lowCursor) return 0;
    u32 h = Hdr(p);
    Hdr(p) = h & 0x7FFFFFFF;
    Hdr(p) = (h & 1) | (g.highCursor - 4 - p);
    g.highCursor = p;
    return p;
}

// orig: 0x000A40B0 MemoryManager::AllocFromLowBins (high=false)
// orig: 0x000A4330 MemoryManager::AllocFromHighBins (high=true)
Off AllocFromBins(u32 size, s32 align, bool high) {
    if (g.lowBins == 0) return 0;                     // both variants test lowBins (set last by InitBins)
    const u32 mask = static_cast<u32>(align) - 1;
    bool split = false;
    u32 cls = SizeClass(size);
    if (static_cast<s32>(cls) >= g.numBins) return 0;
    Off bin = BinAt(high ? g.highBins : g.lowBins, cls);
    do {
        s32 n = S(bin + kBinCount);
        s32 i = 0;
        Off block = 0;
        for (; i < n; ++i) {                          // first fit in address order
            block = W(HeapEntry(bin, i));
            u32 h = Hdr(block);
            if (IsFree(h)) {
                u32 avail = PayloadSize(h);
                if (size <= avail && (block & mask) == 0) break;
                if (static_cast<s32>(size + 0xC + static_cast<u32>(align)) <= static_cast<s32>(avail)) {
                    split = true;
                    break;
                }
            }
        }
        if (i != n && block != 0) {
            UnlinkFreeBlock(block);
            W(block) = 0;
            W(block + 4) = 0;
            Off result = block;
            if (split) {                              // misaligned: split off a free front piece
                result = (block + static_cast<u32>(align) + 0xC) & ~mask;
                u32 h = Hdr(block);
                u32 front = result - 4 - block;
                Hdr(block) = (h & 1) | front | kFreeBit;
                Hdr(result) = (Hdr(result) & 1) | ((PayloadSize(h) - front - 4) & 0x7FFFFFFF);
                u32 frontSize = Hdr(block) & kSizeMask;
                W(block + 4) = 0;
                W(block) = 0;
                u32 frontCls = SizeClass(frontSize);
                if (InLowRegion(block)) {
                    Hdr(block) &= ~1u;
                    Insert(BinAt(g.lowBins, frontCls), block, false);
                    Count(g.lowBinCounts, frontCls) += 1;
                } else if (InHighRegion(block)) {
                    Hdr(block) |= 1;
                    Insert(BinAt(g.highBins, frontCls), block, true);
                    Count(g.highBinCounts, frontCls) += 1;
                }
            }
            // Split off the tail. Quirk: the two variants round the kept size differently.
            u32 keep = high ? (~mask & (size + 3 + static_cast<u32>(align))) : ((size + 0x13) & 0xFFFFFFF0);
            u32 rest = PayloadSize(Hdr(result)) - keep;
            if (8 < static_cast<s32>(rest)) {
                W(result + keep - 4) = (W(result + keep - 4) & 1) | rest | kFreeBit;
                Hdr(result) = (Hdr(result) & 1) | ((keep - 4) & 0x7FFFFFFF);
                FreeBlock(result + keep);
            }
            Hdr(result) &= 0x7FFFFFFF;
            return result;
        }
        ++cls;
        bin += kBinSize;
    } while (static_cast<s32>(cls) < g.numBins);
    return 0;
}

// orig: 0x000A3BA0 MemoryManager::CoalesceLow (high=false) / 0x000A3D70 CoalesceHigh (high=true)
void Coalesce(bool high) {
    if (!g.initialized || g.coalescing) return;
    g.coalescing = true;
    (high ? g.highBinInserts : g.lowBinInserts) = 0;
    Off end = high ? g.heapEnd : g.lowCursor;
    for (Off h = (high ? g.highCursor : g.heapBase) - 4; h != 0 && h + 4 < end; h = h + PayloadSize(W(h)) + 4) {
        if (IsFree(W(h))) {
            Off next = PayloadSize(W(h)) + 4 + h;
            Off merged = 0;
            while (next != 0) {
                if (end <= next + 4 || !IsFree(W(next))) break;
                if (!merged) {
                    UnlinkFreeBlock(h + 4);
                    merged = h + 4;
                }
                UnlinkFreeBlock(next + 4);
                u32 hn = W(next);
                u32 hh = W(h);
                W(next) = 0x4FCFCFCF;
                u32 combined = PayloadSize(hn) + 4 + PayloadSize(hh);
                next = PayloadSize(combined) + 4 + h;
                W(h) = (W(h) & (kFreeBit | 1)) | combined;
            }
            if (merged) {
                W(merged) = 0;
                W(merged + 4) = 0;
                u32 hh = W(h);
                W(h) = hh | kFreeBit;
                if (!high && end == PayloadSize(hh) + 8 + h) {
                    g.lowCursor = h + 4;              // free tail: retract the low cursor
                } else if (high && g.highCursor == merged) {
                    g.highCursor = PayloadSize(Hdr(merged)) + 4 + merged;   // free head: retract high cursor
                } else {
                    W(h) = high ? (hh | kFreeBit | 1) : ((hh & ~1u) | kFreeBit);
                    u32 cls = SizeClass(PayloadSize(Hdr(merged)));
                    Insert(BinAt(high ? g.highBins : g.lowBins, cls), merged, high);
                    Count(high ? g.highBinCounts : g.lowBinCounts, cls) += 1;
                }
            }
        }
        end = high ? g.heapEnd : g.lowCursor;
    }
    g.coalescing = false;
    Off bins = high ? g.highBins : g.lowBins;
    for (s32 i = 0; i < g.numBins; ++i) {             // flush pending lists
        Off bin = BinAt(bins, static_cast<u32>(i));
        Off block = W(bin + kBinPending);
        if (block) {
            W(bin + kBinPending) = W(block);
            Insert(bin, block, high);
        }
    }
}

// orig: 0x000A49B0 MemoryManager::ReserveHeap
void ReserveHeap(u32 size) {
    const u32 total = kHeapSlack + size + kReserveExtra;
#if defined(_WIN32)
    // VirtualAlloc: zero-filled (orig HEAP_ZERO_MEMORY) and 64 KB aligned.
    g.region = static_cast<u8*>(VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    g.region = static_cast<u8*>(std::aligned_alloc(0x10000, (total + 0xFFFF) & ~0xFFFFu));
    if (g.region) std::memset(g.region, 0, total);
#endif
    if (!g.region) FatalError("MemoryManager: cannot reserve %u bytes", size);
    g.heapBase = kHeapSlack;
    g.heapEnd = kHeapSlack + size;
    g.lowCursor = g.heapBase;
    g.highCursor = g.heapEnd;
    g.initialized = true;
}

// orig: 0x000A36F0 MemoryManager::InitBins
void InitBins() {
    g.numBins = kNumBins;
    Off highBins = Alloc(0xC90, kDefaultAlign);
    g.highBins = highBins;
    g.highBinCounts = highBins + static_cast<u32>(g.numBins) * kBinSize;
    Off lowBins = Alloc(static_cast<u32>(g.numBins) * 0x18, kDefaultAlign);
    g.lowBinCounts = lowBins + static_cast<u32>(g.numBins) * kBinSize;
    for (s32 i = 0; i < g.numBins; ++i) {
        Off lb = BinAt(lowBins, static_cast<u32>(i));
        S(lb + kBinCapacity) = kInitialBinCapacity;
        W(lb + kBinHeap) = Alloc(0x3FC, 0x10);        // memory.cpp line 0xEDE
        S(lb + kBinCount) = 0;
        W(lb + kBinPending) = 0;
        B(lb + kBinDefer) = 0;
        Count(g.lowBinCounts, static_cast<u32>(i)) = 0;
        Off hb = BinAt(g.highBins, static_cast<u32>(i));
        S(hb + kBinCapacity) = kInitialBinCapacity;
        W(hb + kBinHeap) = Alloc(0x3FC, 0x10);        // memory.cpp line 0xDC2
        S(hb + kBinCount) = 0;
        W(hb + kBinPending) = 0;
        B(hb + kBinDefer) = 0;
        Count(g.highBinCounts, static_cast<u32>(i)) = 0;
    }
    W(g.lowBinCounts + static_cast<u32>(g.numBins - 1) * 4) = kBinCountGuard;    // quirk
    W(g.highBinCounts + static_cast<u32>(g.numBins - 1) * 4) = kBinCountGuard;
    g.lowBins = lowBins;
}

// orig: 0x000A3960 MemoryManager::GetLargestFreeBlocks (scans the high bins only)
void LargestFreeBlocks(s32* out, s32 count) {
    for (s32 i = 0; i < count; ++i) out[i] = 0;
    if (g.lowBins == 0) return;
    out[count - 1] = static_cast<s32>(g.highCursor - g.lowCursor);
    for (s32 c = g.numBins - 1; c >= 0; --c) {
        Off bin = BinAt(g.highBins, static_cast<u32>(c));
        s32 n = S(bin + kBinCount);
        bool any = false;
        for (s32 i = 0; i < n; ++i) {
            Off block = i < S(bin + kBinCount) ? W(HeapEntry(bin, i)) : 0;
            s32 size = static_cast<s32>(Hdr(block) & kSizeMask);
            if (out[0] <= size) {
                s32 k = 0;
                for (; k < count - 1 && out[k + 1] <= size; ++k) out[k] = out[k + 1];
                out[k] = size;
                any = true;
            }
        }
        if (n > 0 && !any) return;
    }
}

// orig: 0x000A45B0 MemoryManager::Alloc (file/line arguments are ignored in the shipped build)
Off Alloc(u32 size, u16 align) {
    if (!g.initialized) {
        g.bytesUsed = 0;
        g.unk08 = 0;
        ReserveHeap(g.heapSize);
        g.bytesFree = g.heapEnd - g.heapBase;
        InitBins();
    }
    if (static_cast<s32>(size) < 1) return 0;
    const s32 a = static_cast<s16>(align);
    Off p = 0;
    if (size < 8) size = 8;
    if (static_cast<s32>(size) < kLargeRequest) {
        p = AllocFromBins(size, a, false);
        if (!p) p = AllocFromLowCursor(size, a);
        if (!p && kLowInsertsBeforeCoalesce < g.lowBinInserts) {
            Coalesce(false);
            p = AllocFromBins(size, a, false);
            if (!p) p = AllocFromLowCursor(size, a);
        }
        if (!p) p = AllocFromBins(size, a, true);
    } else {
        p = AllocFromBins(size, a, true);
        if (!p) p = AllocFromHighCursor(size, a);
        if (!p && kHighInsertsBeforeCoalesce < g.highBinInserts) {
            Coalesce(true);
            p = AllocFromBins(size, a, true);
            if (!p) p = AllocFromHighCursor(size, a);
        }
        if (!p) p = AllocFromBins(size, a, false);
    }
    if (!p) {
        // Original: Mem_GetLargestFreeBlocks(local_28, 10) then an infinite loop.
        s32 largest[10];
        LargestFreeBlocks(largest, 10);
        FatalError("out of memory: request %u bytes (align %d); largest free %d, gap %d", size, a,
                   largest[8], largest[9]);
    }
    g.bytesUsed += PayloadSize(Hdr(p)) + 4;
    g.bytesFree += static_cast<u32>(-4) - PayloadSize(Hdr(p));
    if (static_cast<s32>(g.peakBytesUsed) < static_cast<s32>(g.bytesUsed)) g.peakBytesUsed = g.bytesUsed;
    return p;
}

// orig: 0x000A47B0 FreeBin::ReallocArrayHigh / 0x000A4860 FreeBin::ReallocArrayLow
void ReallocArray(Off bin, bool high) {
    if (S(bin + kBinCapacity) <= 0xFF) return;
    Off old = W(bin + kBinHeap);
    B(bin + kBinDefer) = 1;
    u32 bytes = static_cast<u32>(S(bin + kBinCapacity)) << 2;
    Off fresh = Alloc(bytes, 0x10);                   // memory.cpp line 0xEC9 / 0xFEA
    std::memmove(g.region + fresh, g.region + W(bin + kBinHeap), bytes & ~3u);   // copies capacity entries
    W(bin + kBinHeap) = fresh;
    if (old) FreeInternal(old);
    Off pending = W(bin + kBinPending);
    B(bin + kBinDefer) = 0;
    if (pending) {
        W(bin + kBinPending) = W(pending);
        Insert(bin, pending, high);
    }
}

}  // namespace

// ---- public API ---------------------------------------------------------------------------------------------

void* OperatorNew(u32 size) { return ToPtr(Alloc(size, kDefaultAlign)); }

void* AllocAligned(u32 size, u16 align) { return ToPtr(Alloc(size, align)); }

void OperatorDelete(void* p) {
    if (!p) return;
    FreeInternal(ToOff(p));
}

u32 BlockSize(const void* p) { return p ? PayloadSize(Hdr(ToOff(p))) + 4 : 0; }

void FrameTick() {
    if (0x28 < ++g.framesSinceHighCoalesce) {
        g.framesSinceHighCoalesce = 0;
        Coalesce(true);
    }
    if (200 < ++g.framesSinceLowCoalesce) {
        g.framesSinceLowCoalesce = 0;
        Coalesce(false);
    }
}

int CoalesceAll() {
    Coalesce(true);
    Coalesce(false);
    return 0;
}

void ReallocBinArrays() {
    for (s32 i = 0; i < g.numBins; ++i) {
        ReallocArray(BinAt(g.lowBins, static_cast<u32>(i)), false);
        ReallocArray(BinAt(g.highBins, static_cast<u32>(i)), true);
    }
}

void SetUnknownFlag(u8 value) { g.unknownFlag = value; }

bool ReturnTrue() { return true; }

void GetLargestFreeBlocks(s32* out, s32 count) { LargestFreeBlocks(out, count); }

Stats GetStats() {
    return {g.bytesUsed, g.bytesFree, g.lowCursor - g.heapBase, g.highCursor - g.heapBase, g.peakBytesUsed};
}

u32 HeapOffset(const void* p) { return ToOff(p) - g.heapBase; }

namespace {
u32 g_vmBias = 0;
}

u32 ToVmAddr(const void* p) {
    if (!p) return 0;
    const u8* b = static_cast<const u8*>(p);
    if (!g.region || b < g.region || b >= g.region + kHeapSlack + g.heapSize + kReserveExtra)
        FatalError("ToVmAddr: pointer %p is outside the game heap", p);
    return static_cast<u32>(b - g.region) + g_vmBias;
}

void* FromVmAddr(u32 addr) { return addr ? g.region + (addr - g_vmBias) : nullptr; }

void SetVmAddrBias(u32 bias) { g_vmBias = bias; }

bool IsVmAddr(u32 addr) {
    if (!g.region || addr < g_vmBias) return false;
    return addr - g_vmBias < kHeapSlack + g.heapSize + kReserveExtra;
}

u32 RawHeader(const void* p) { return Hdr(ToOff(p)); }

void SetHeapSize(u32 bytes) {
    if (g.initialized) FatalError("SetHeapSize after the heap was created");
    g.heapSize = bytes;
}

void ResetForTests() {
    if (g.region) {
#if defined(_WIN32)
        VirtualFree(g.region, 0, MEM_RELEASE);
#else
        std::free(g.region);
#endif
    }
    u32 size = g.heapSize;
    g = State{};
    g.heapSize = size;
}

}  // namespace fut::mem

// The game's C runtime qsort (default.xbe 0x001679F0, the MSVC CRT's: median of three, shortsort 0x00167980 below 9
// elements; not stable). Used by the physics slide (game/physics) and the ray cast's pass 2 (engine/ni/NiCollision),
// whose results depend on the order it leaves equal elements in.
#pragma once

#include <cstddef>
#include <utility>

namespace fut::crt {

namespace detail {
inline void QsortSwap(char* a, char* b, size_t width) {
    if (a == b) return;
    while (width--) std::swap(*a++, *b++);
}

// orig: shortsort 0x00167980: repeatedly moves the largest remaining element to the end.
inline void ShortSort(char* lo, char* hi, size_t width, int (*cmp)(const void*, const void*)) {
    while (hi > lo) {
        char* max = lo;
        for (char* p = lo + width; p <= hi; p += width)
            if (cmp(p, max) > 0) max = p;
        QsortSwap(max, hi, width);
        hi -= width;
    }
}
}  // namespace detail

inline void Qsort(void* base, size_t num, size_t width, int (*cmp)(const void*, const void*)) {   // orig: 0x001679F0
    using detail::QsortSwap;
    if (num < 2 || width == 0) return;
    char* lostk[30];
    char* histk[30];
    int stkptr = 0;
    char* lo = static_cast<char*>(base);
    char* hi = lo + width * (num - 1);
recurse:
    {
        const size_t size = static_cast<size_t>(hi - lo) / width + 1;
        if (size <= 8) {
            detail::ShortSort(lo, hi, width, cmp);
        } else {
            char* mid = lo + (size / 2) * width;
            if (cmp(lo, mid) > 0) QsortSwap(lo, mid, width);
            if (cmp(lo, hi) > 0) QsortSwap(lo, hi, width);
            if (cmp(mid, hi) > 0) QsortSwap(mid, hi, width);
            char* loguy = lo;
            char* higuy = hi;
            for (;;) {
                if (mid > loguy) {
                    do {
                        loguy += width;
                    } while (loguy < mid && cmp(loguy, mid) <= 0);
                }
                if (mid <= loguy) {
                    do {
                        loguy += width;
                    } while (loguy <= hi && cmp(loguy, mid) <= 0);
                }
                do {
                    higuy -= width;
                } while (higuy > mid && cmp(higuy, mid) > 0);
                if (higuy < loguy) break;
                QsortSwap(loguy, higuy, width);
                if (mid == higuy) mid = loguy;
            }
            higuy += width;
            if (mid < higuy) {
                do {
                    higuy -= width;
                } while (higuy > mid && cmp(higuy, mid) == 0);
            }
            if (mid >= higuy) {
                do {
                    higuy -= width;
                } while (higuy > lo && cmp(higuy, mid) == 0);
            }
            if (higuy - lo >= hi - loguy) {
                if (lo < higuy) {
                    lostk[stkptr] = lo;
                    histk[stkptr] = higuy;
                    ++stkptr;
                }
                if (loguy < hi) {
                    lo = loguy;
                    goto recurse;
                }
            } else {
                if (loguy < hi) {
                    lostk[stkptr] = loguy;
                    histk[stkptr] = hi;
                    ++stkptr;
                }
                if (lo < higuy) {
                    hi = higuy;
                    goto recurse;
                }
            }
        }
    }
    --stkptr;
    if (stkptr >= 0) {
        lo = lostk[stkptr];
        hi = histk[stkptr];
        goto recurse;
    }
}

}  // namespace fut::crt

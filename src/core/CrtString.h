// The game's C runtime string compares that differ from the host's: _stricmp (0x001684D1) and _strnicmp (0x00170B4A,
// C-locale path 0x00170C30) fold A-Z to lower case (the game never changes the locale). strncmp (0x001675D0) and
// strcmp are the host's std::strncmp / std::strcmp. docs/analysis/string_compare_audit.md.
#pragma once

#include <cstddef>

namespace fut::crt {

inline int FoldLower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

inline int StrICmp(const char* a, const char* b) {    // orig: _stricmp 0x001684D1
    for (;; ++a, ++b) {
        const int ca = FoldLower(static_cast<unsigned char>(*a)), cb = FoldLower(static_cast<unsigned char>(*b));
        if (ca != cb || !ca) return ca - cb;
    }
}

inline int StrNICmp(const char* a, const char* b, size_t n) {   // orig: _strnicmp 0x00170B4A
    for (; n; --n, ++a, ++b) {
        const int ca = FoldLower(static_cast<unsigned char>(*a)), cb = FoldLower(static_cast<unsigned char>(*b));
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

}  // namespace fut::crt

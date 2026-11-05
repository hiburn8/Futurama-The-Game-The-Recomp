// The handful of Microsoft C-runtime names the ported code uses, for the other platforms (docs/analysis/linux_plan.md
// section 1): on Windows the real ones; elsewhere the POSIX equivalents under the same names, so the game code stays
// as written. Included by core/Types.h, which everything includes.
#pragma once

#ifndef _WIN32

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <strings.h>

inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }

inline int fopen_s(std::FILE** f, const char* name, const char* mode) {
    *f = std::fopen(name, mode);
    return *f ? 0 : errno;
}

inline int localtime_s(std::tm* out, const std::time_t* t) { return localtime_r(t, out) ? 0 : errno; }

#define __forceinline inline __attribute__((always_inline))

#endif

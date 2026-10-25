#include "core/Types.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace fut {

void FatalError(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    std::fputs("FATAL: ", stderr);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    va_end(args);
    std::fflush(stderr);
    std::abort();
}

}  // namespace fut

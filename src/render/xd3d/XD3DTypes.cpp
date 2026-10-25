// The format helpers; see XD3DTypes.h.
#include "render/xd3d/XD3DTypes.h"

#include "core/XbeImage.h"

#include <algorithm>

namespace fut::render::xd3d {

u32 FormatBits(u32 format) {
    switch (format) {
    case 0xC: return 4;                               // DXT1
    case 0xE:
    case 0xF: return 8;                               // DXT3 / DXT5
    case 0x24: return 16;                             // YUY2 (4:2:2, linear: the movie player's texture; 0x00158820's
                                                      //  table has class 3 for it, which that function returns as -1)
    default: break;
    }
    // (H) 0x00158820's byte table 0x00158864 (0: 8, 1: 16, 2: 32) for the formats 0x00..0x65, read from the user's
    // default.xbe (core/XbeImage.h)
    if (format >= 0x66) return 32;
    switch (xbe::GameImage().Byte(0x00158864 + format)) {
    case 0: return 8;
    case 1: return 16;
    case 2: return 32;
    default: return 32;                               // (-1 / beyond the jump table: L, 4 bytes as before)
    }
}

bool FormatSwizzled(u32 format) {                     // orig: 0x00158A20
    switch (format) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x19: case 0x1A:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x32: case 0x38: case 0x39: case 0x3A:
    case 0x3B: case 0x3C: return true;
    default: return false;
    }
}

size_t LevelBytes(u32 format, u32 width, u32 height) {
    if (FormatCompressed(format))
        return size_t(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4) * (format == 0xC ? 8 : 16);
    return size_t(std::max(width, 1u)) * std::max(height, 1u) * FormatBits(format) / 8;
}

}  // namespace fut::render::xd3d

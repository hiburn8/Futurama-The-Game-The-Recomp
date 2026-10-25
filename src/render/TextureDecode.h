// NiPixelData -> GPU-ready texture levels (Phase 3 renderer; replaces the NiXBox texture creation). The shipped data
// uses: format 0 RGB 16 bpp (5:6:5), format 1 RGBA 16 bpp (4:4:4:4) and once RGBA 32 bpp, format 4 DXT1 and format 5
// DXT3 (NetImmerse 4 NiPixelFormat order RGB, RGBA, PAL, PALALPHA, DXT1, DXT3, DXT5). Uncompressed levels are
// expanded to RGBA8 from the channel masks; DXT levels are passed through as BC1 / BC2 blocks. Pixel data is stored
// linear, rows top to bottom (checked on the font atlases: Morton deswizzling scrambles them), so the original must
// swizzle at texture creation.
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace fut::ni {
class NiPixelData;
}

namespace fut::render {

struct DecodedTexture {
    enum class Kind { RGBA8, BC1, BC2, BC3 };
    Kind kind = Kind::RGBA8;
    u32 width = 0, height = 0;
    std::vector<std::vector<u8>> levels;               // tightly packed, level 0 first
};

bool DecodePixelData(const ni::NiPixelData& data, DecodedTexture& out, std::string* error = nullptr);

}  // namespace fut::render

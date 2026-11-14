// NiPixelData decoding; see TextureDecode.h.
#include "render/TextureDecode.h"

#include "engine/ni/NiObjects.h"
#include "render/xd3d/XD3DDevice.h"
#include "render/xd3d/XD3DTypes.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace fut::render {
namespace {

// Expands the masked channel of `pixel` to 8 bits (0xFF when the mask is empty).
u8 Channel(u32 pixel, u32 mask, u8 empty) {
    if (!mask) return empty;
    const int shift = std::countr_zero(mask);
    const u32 bits = static_cast<u32>(std::popcount(mask));
    const u32 v = (pixel & mask) >> shift;
    const u32 max = (1u << bits) - 1;
    return static_cast<u8>((v * 255 + max / 2) / max);
}


}  // namespace

bool DecodePixelData(const ni::NiPixelData& pd, DecodedTexture& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    out = DecodedTexture();
    if (pd.mips.empty()) return fail("no mip levels");
    out.width = pd.mips[0].width;
    out.height = pd.mips[0].height;
    const u32 fmt = pd.format.format;
    if (fmt == 4 || fmt == 5 || fmt == 6) {
        out.kind = fmt == 4 ? DecodedTexture::Kind::BC1 : fmt == 5 ? DecodedTexture::Kind::BC2 : DecodedTexture::Kind::BC3;
        const u32 blockBytes = fmt == 4 ? 8 : 16;
        for (const auto& m : pd.mips) {
            const u32 bw = (m.width + 3) / 4, bh = (m.height + 3) / 4;
            const size_t bytes = static_cast<size_t>(bw) * bh * blockBytes;
            if (m.offset + bytes > pd.pixels.size()) break;
            out.levels.emplace_back(pd.pixels.begin() + m.offset, pd.pixels.begin() + m.offset + bytes);
        }
        return !out.levels.empty() || fail("truncated DXT data");
    }
    if (fmt != 0 && fmt != 1) return fail("unsupported pixel format " + std::to_string(fmt));
    const u32 bpp = pd.format.bitsPerPixel;
    if (bpp != 16 && bpp != 32) return fail("unsupported bits per pixel " + std::to_string(bpp));
    const u32 bytesPer = bpp / 8;
    const u32* masks = pd.format.channelMasks;
    out.kind = DecodedTexture::Kind::RGBA8;
    for (const auto& m : pd.mips) {
        const size_t count = static_cast<size_t>(m.width) * m.height;
        if (m.offset + count * bytesPer > pd.pixels.size()) break;
        std::vector<u8> rgba(count * 4);
        for (u32 y = 0; y < m.height; ++y)
            for (u32 x = 0; x < m.width; ++x) {
                const size_t src = static_cast<size_t>(y) * m.width + x;
                u32 pixel = 0;
                std::memcpy(&pixel, &pd.pixels[m.offset + src * bytesPer], bytesPer);
                u8* d = &rgba[(static_cast<size_t>(y) * m.width + x) * 4];
                d[0] = Channel(pixel, masks[0], 0);
                d[1] = Channel(pixel, masks[1], 0);
                d[2] = Channel(pixel, masks[2], 0);
                d[3] = Channel(pixel, masks[3], 0xFF);
            }
        out.levels.push_back(std::move(rgba));
    }
    return !out.levels.empty() || fail("truncated pixel data");
}

bool DecodeXboxTexels(const xd3d::Resource& r, DecodedTexture& out) {
    struct Masks { u32 a, r, g, b, bytes; };
    Masks m{};
    switch (r.format) {
    case 0x02: m = {0x8000, 0x7C00, 0x03E0, 0x001F, 2}; break;   // A1R5G5B5
    case 0x03: m = {0, 0x7C00, 0x03E0, 0x001F, 2}; break;        // X1R5G5B5
    case 0x04: m = {0xF000, 0x0F00, 0x00F0, 0x000F, 2}; break;   // A4R4G4B4
    case 0x05: m = {0, 0xF800, 0x07E0, 0x001F, 2}; break;        // R5G6B5
    case 0x06: m = {0xFF000000u, 0xFF0000, 0xFF00, 0xFF, 4}; break;   // A8R8G8B8
    case 0x07: m = {0, 0xFF0000, 0xFF00, 0xFF, 4}; break;        // X8R8G8B8
    case 0x0C: case 0x0E: case 0x0F: break;
    default: return false;
    }
    out = DecodedTexture();
    out.width = r.width, out.height = r.height;
    const bool block = r.format == 0x0C || r.format == 0x0E || r.format == 0x0F;
    if (block) out.kind = r.format == 0x0C ? DecodedTexture::Kind::BC1 : r.format == 0x0E ? DecodedTexture::Kind::BC2
                                                                                        : DecodedTexture::Kind::BC3;
    size_t at = 0;
    for (u32 l = 0, w = r.width, h = r.height; l < std::max(r.levels, 1u); ++l, w = std::max(w / 2, 1u),
                                                                          h = std::max(h / 2, 1u)) {
        const size_t bytes = xd3d::LevelBytes(r.format, w, h);
        if (at + bytes > r.data.size()) return false;
        const u8* src = r.data.data() + at;
        if (block) {
            out.levels.emplace_back(src, src + bytes);
        } else {
            u32 xmask = 0, ymask = 0;
            for (u32 k = 1, bit = 1;; k <<= 1) {
                bool used = false;
                if (k < w) xmask |= bit, bit <<= 1, used = true;
                if (k < h) ymask |= bit, bit <<= 1, used = true;
                if (!used) break;
            }
            std::vector<u8> rgba(size_t(w) * h * 4);
            u32 yoff = 0;
            for (u32 y = 0; y < h; ++y) {
                u32 xoff = 0;
                for (u32 x = 0; x < w; ++x) {
                    const u8* p = src + size_t(xoff | yoff) * m.bytes;
                    const u32 v = m.bytes == 2 ? u32(p[0] | p[1] << 8)
                                               : u32(p[0] | p[1] << 8 | p[2] << 16 | u32(p[3]) << 24);
                    u8* d = &rgba[(size_t(y) * w + x) * 4];
                    d[0] = Channel(v, m.r, 0), d[1] = Channel(v, m.g, 0), d[2] = Channel(v, m.b, 0);
                    d[3] = Channel(v, m.a, 0xFF);
                    xoff = (xoff - xmask) & xmask;
                }
                yoff = (yoff - ymask) & ymask;
            }
            out.levels.push_back(std::move(rgba));
        }
        at += bytes;
    }
    return true;
}

}  // namespace fut::render

// The user's default.xbe by virtual address; see XbeImage.h.
#include "core/XbeImage.h"

#include <cstdio>
#include <cstring>

namespace fut::xbe {

namespace {

// The expected builds: Futurama (Xbox), NTSC-U and PAL.
constexpr u32 kBaseAddress = 0x00010000;
constexpr u32 kTitleId = 0x56550008;                  // certificate +0x08 ("VU" 8)
constexpr u32 kRegionNorthAmerica = 1;                // certificate +0xA0 (game region bits)
constexpr u32 kTimeStamp = 0x3F03E9A5;                // header +0x114
constexpr u32 kPalTitleId = 0x46490002;               // "FI" 2
constexpr u32 kRegionRestOfWorld = 4;
constexpr u32 kPalTimeStamp = 0x3F01BB06;             // (2003-07-01)

// The PAL build's layout: .text is 0x90 bytes shorter, so every later section sits 0xA0 lower (the section alignment)
// at the same file offset as NTSC's. Its data is NTSC's but for the pointers into the moved range, which are 0xA0
// lower too (checked for every range below and every table the readers use: identical after the move).
constexpr u32 kPalDelta = 0xA0;
constexpr u32 kPalMovedFrom = 0x002342C0;             // PAL's section D3D (NTSC 0x00234360) ..
constexpr u32 kPalMovedTo = 0x0031A2F8;               // .. the image end (.XTLID's)
// The pointer tables the port follows (NTSC addresses and sizes): each entry inside the moved range moves back.
struct Range {
    u32 va, size;
};
constexpr Range kPalPointerTables[] = {
    {0x002D60D8, 29 * 4},                             // the cheat codes' strings (CheatCodes.cpp)
    {0x002D615C, 8 * 8},                              // the cheats' level texts (two strings an entry)
    {0x002D6228, 248 * 4},                            // the animation names (CharacterDef.cpp)
    {0x0026C9D8, 0x28},                               // XMV: the default mix-bin table (XmvDecoder.cpp)
    {0x002747B0, 3 * 36},                             // XMV: the run / level descriptor sets 0 and 1 (Wmv2Decoder.cpp)
    {0x00274820, 3 * 36},
};
// The .text tables the port reads (NTSC address, PAL address, size): copied to their NTSC place.
struct TextTable {
    u32 ntsc, pal, size;
};
constexpr TextTable kPalTextTables[] = {
    {0x00158864, 0x001587D4, 0x40},                   // FormatBits' format switch (XD3DTypes.cpp)
};

XbeImage g_image;

}  // namespace

bool XbeImage::Load(std::vector<u8> bytes, std::string* error) {
    bytes_.clear();
    sections_.clear();
    auto fail = [&](const std::string& why) {
        if (error) *error = "default.xbe: " + why;
        return false;
    };
    auto u32At = [&](size_t o) -> u32 {
        u32 v = 0;
        if (o + 4 <= bytes.size()) std::memcpy(&v, &bytes[o], 4);
        return v;
    };
    if (bytes.size() < 0x200 || std::memcmp(bytes.data(), "XBEH", 4) != 0) return fail("not an XBE file");
    const u32 base = u32At(0x104);
    const u32 cert = u32At(0x118) - base, count = u32At(0x11C), headers = u32At(0x120) - base;
    const u32 title = u32At(size_t(cert) + 0x08), region = u32At(size_t(cert) + 0xA0), stamp = u32At(0x114);
    const bool ntsc = title == kTitleId && (region & kRegionNorthAmerica) != 0 && stamp == kTimeStamp;
    const bool pal = title == kPalTitleId && (region & kRegionRestOfWorld) != 0 && stamp == kPalTimeStamp;
    if (base != kBaseAddress || (!ntsc && !pal)) {
        char buf[240];
        std::snprintf(buf, sizeof buf,
                      "not a Futurama (Xbox) build the port knows (title id %08X, region %X, time stamp %08X; expected "
                      "NTSC-U %08X, %X, %08X or PAL %08X, %X, %08X)",
                      title, region, stamp, kTitleId, kRegionNorthAmerica, kTimeStamp, kPalTitleId, kRegionRestOfWorld,
                      kPalTimeStamp);
        return fail(buf);
    }
    if (count == 0 || count > 64 || size_t(headers) + size_t(count) * 0x38 > bytes.size())
        return fail("damaged section table");
    for (u32 i = 0; i < count; ++i) {
        const size_t h = size_t(headers) + size_t(i) * 0x38;
        u32 va = u32At(h + 4);
        const u32 vsize = u32At(h + 8), raw = u32At(h + 12), rsize = u32At(h + 16);
        if (rsize > vsize || size_t(raw) + rsize > bytes.size()) return fail("damaged section table");
        if (pal && va >= kPalMovedFrom) {             // the NTSC address, in the readers' header copy too
            va += kPalDelta;
            std::memcpy(&bytes[h + 4], &va, 4);
        }
        sections_.push_back({va, rsize, raw});
    }
    bytes_ = std::move(bytes);
    pal_ = pal;
    if (pal) {
        for (const Range& r : kPalPointerTables)
            for (u32 o = 0; o < r.size; o += 4) {
                u8* p = const_cast<u8*>(At(r.va + o, 4));
                if (!p) return fail("PAL build: a pointer table outside the image");
                u32 v;
                std::memcpy(&v, p, 4);
                if (v >= kPalMovedFrom && v < kPalMovedTo) v += kPalDelta;
                std::memcpy(p, &v, 4);
            }
        for (const TextTable& t : kPalTextTables) {   // (.text did not move as a whole: read at the PAL address)
            const u8* from = At(t.pal, t.size);
            u8* to = const_cast<u8*>(At(t.ntsc, t.size));
            if (!from || !to) return fail("PAL build: a .text table outside the image");
            std::vector<u8> copy(from, from + t.size);
            std::memcpy(to, copy.data(), t.size);
        }
    }
    return true;
}

const u8* XbeImage::At(u32 va, u32 n) const {
    for (const Section& s : sections_)
        if (va >= s.va && va - s.va < s.size && n <= s.size - (va - s.va)) return bytes_.data() + s.raw + (va - s.va);
    return nullptr;
}

u8 XbeImage::Byte(u32 va) const {
    const u8* p = At(va);
    return p ? *p : 0;
}

u32 XbeImage::Word(u32 va) const {
    const u8* p = At(va, 4);
    u32 v = 0;
    if (p) std::memcpy(&v, p, 4);
    return v;
}

const char* XbeImage::String(u32 va) const {
    for (const Section& s : sections_)
        if (va >= s.va && va - s.va < s.size) {
            const u8* p = bytes_.data() + s.raw + (va - s.va);
            if (std::memchr(p, 0, s.size - (va - s.va))) return reinterpret_cast<const char*>(p);
            break;
        }
    return "";
}

bool LoadGameImage(std::vector<u8> bytes, std::string* error) {
    if (g_image.Loaded()) {
        if (error) *error = "default.xbe: already loaded";
        return false;
    }
    return g_image.Load(std::move(bytes), error);
}

const XbeImage& GameImage() { return g_image; }

}  // namespace fut::xbe

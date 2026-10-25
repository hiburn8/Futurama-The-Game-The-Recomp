// The user's default.xbe by virtual address; see XbeImage.h.
#include "core/XbeImage.h"

#include <cstdio>
#include <cstring>

namespace fut::xbe {

namespace {

// The expected build: Futurama (Xbox), NTSC-U.
constexpr u32 kBaseAddress = 0x00010000;
constexpr u32 kTitleId = 0x56550008;                  // certificate +0x08 ("VU" 8)
constexpr u32 kRegionNorthAmerica = 1;                // certificate +0xA0 (game region bits)
constexpr u32 kTimeStamp = 0x3F03E9A5;                // header +0x114

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
    if (base != kBaseAddress || title != kTitleId || (region & kRegionNorthAmerica) == 0 || stamp != kTimeStamp) {
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "not the Futurama (Xbox, NTSC-U) build (title id %08X, region %X, time stamp %08X; expected "
                      "%08X, %X, %08X)",
                      title, region, stamp, kTitleId, kRegionNorthAmerica, kTimeStamp);
        return fail(buf);
    }
    if (count == 0 || count > 64 || size_t(headers) + size_t(count) * 0x38 > bytes.size())
        return fail("damaged section table");
    for (u32 i = 0; i < count; ++i) {
        const size_t h = size_t(headers) + size_t(i) * 0x38;
        const u32 va = u32At(h + 4), vsize = u32At(h + 8), raw = u32At(h + 12), rsize = u32At(h + 16);
        if (rsize > vsize || size_t(raw) + rsize > bytes.size()) return fail("damaged section table");
        sections_.push_back({va, rsize, raw});
    }
    bytes_ = std::move(bytes);
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

// The user's default.xbe by virtual address; see XbeImage.h.
#include "core/XbeImage.h"
#include "core/Sha1.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

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
    palEu_ = false;
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
    if (u32At(0x118) < base || u32At(0x120) < base ||
        size_t(u32At(0x118) - base) + 0xA4 > bytes.size())
        return fail("damaged certificate or section table");
    const u32 cert = u32At(0x118) - base, count = u32At(0x11C), headers = u32At(0x120) - base;
    const u32 title = u32At(size_t(cert) + 0x08), region = u32At(size_t(cert) + 0xA0), stamp = u32At(0x114);
    const bool pal = base == kBaseAddress && title == 0x46490002 && region == 4 && stamp == 0x3F01BB06;
    if (pal && crypto::Sha1::Hex(crypto::Sha1::Of(bytes.data(), bytes.size())) !=
                   "58f76de5bc739bf016d74c1d7f23e348f65ea4b4")
        return fail("PAL/EU retail image hash mismatch");
    if (!pal && (base != kBaseAddress || title != kTitleId || (region & kRegionNorthAmerica) == 0 || stamp != kTimeStamp)) {
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
    palEu_ = pal;
    bytes_ = std::move(bytes);
    return true;
}

const u8* XbeImage::At(u32 va, u32 n) const {
    if (palEu_) {
        // Logical NTSC data addresses used by the reconstructed code -> verified PAL tables.
        // String() receives actual pointers read from these tables, so it must NOT translate.
        // Evidence and exact accepted image: docs/PAL-DATA.md. Never apply a blanket delta.
        struct Range { u32 ntsc, pal, bytes; };
        static constexpr Range ranges[] = {
            {0x00158864, 0x001587D4, 0x66},          // format classes: code-local table (-0x90)
            {0x002427C8, 0x00242728, 32},            // D3D texture-state defaults
            {0x002427E8, 0x00242748, (0xA6 - 0x39) * 4},
            {0x002936A8 + 0x5C * 4, 0x00293608 + 0x5C * 4, (0x88 - 0x5C) * 4},
            {0x002D60D8, 0x002D6038, 29 * 4},       // cheat-code pointer table
            {0x002D615C, 0x002D60BC, 22 * 8},       // level-text pointer pairs
            {0x002D6228, 0x002D6188, 248 * 4},      // animation-name pointers
            {0x002D8358, 0x002D82B8, 32 * 4},       // NetImmerse texture-state defaults
            {0x002D83D8, 0x002D8338, 166 * 4},      // NetImmerse render-state defaults
            {0x00262960, 0x002628C0, 61 * 31 * 2}, // HRTF index
            {0x002513A0, 0x00251300, 2222 * 32},   // HRTF filters
            {0x00263A38, 0x00263998, 32},          // default HRTF filter
            {0x002510C0, 0x00251020, 45 * 4},      // centre gain
            {0x00251178, 0x002510D8, 45 * 4},      // centre cutoff
            {0x00263A68, 0x002639C8, 20 * 4},      // reverb rows
            {0x00263AB8, 0x00263A18, 8 * 4},       // reverb signs
            {0x00263AE8, 0x00263A48, 8 * 4},       // reverb delays
            {0x00263BE0, 0x00263B40, 0x5CC},       // GP DSP executive
        };
        bool found = false;
        for (const auto& r : ranges) {
            if (va >= r.ntsc && va - r.ntsc < r.bytes && n <= r.bytes - (va - r.ntsc)) {
                va = r.pal + (va - r.ntsc);
                found = true;
                break;
            }
        }
        if (!found) throw std::out_of_range("unsupported PAL/EU data-table access");
    }
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

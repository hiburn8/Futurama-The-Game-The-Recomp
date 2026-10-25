// Decompiled from default.xbe 0x0013FCD0 (ImgArchive::Find), 0x0013FEA0 (FindInArchives), 0x0013FF00 (MountArchive).
#include "core/fs/ImgArchive.h"

#include <cstdio>
#include <cstring>

namespace fut::fs {
namespace {

// orig: _stricmp (0x001684D1) in the "C" locale: ASCII case folding only.
int StrICmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        int ca = static_cast<u8>(*a), cb = static_cast<u8>(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 0x20;
        if (cb >= 'A' && cb <= 'Z') cb += 0x20;
        if (ca != cb || ca == 0) return ca - cb;
    }
}

}  // namespace

bool ImgArchive::ReadTable(const std::string& imgPath, std::vector<u8>& table) {
    std::FILE* f = std::fopen(imgPath.c_str(), "rb");
    if (!f) return false;
    u32 size = 0;
    bool ok = std::fread(&size, 4, 1, f) == 1 && size >= 4 && size < (64u << 20);
    if (ok) {
        table.resize(size);
        ok = std::fseek(f, 0, SEEK_SET) == 0 && std::fread(table.data(), 1, size, f) == size;
    }
    std::fclose(f);
    return ok;
}

// orig: 0x0013FCD0 ImgArchive::Find
bool ImgArchive::Find(const char* path, u32& offset, u32& size) const {
    // The original copies the path into a 256-byte stack buffer without a length check; longer paths would
    // overflow it. The port rejects them (docs/KNOWN_ISSUES.md).
    const size_t len = std::strlen(path);
    if (len >= 256) return false;
    char buf[256];
    std::memcpy(buf, path, len + 1);
    for (char* c = buf; *c; ++c) {
        if (*c >= 'A' && *c <= 'Z') *c += 0x20;     // orig: _strlwr (0x00170C07)
        if (*c == '/') *c = '\\';                    // orig: strchr loop replacing '/' with '\'
    }
    if (table_.size() < 4) return false;
    const u8* base = table_.data();
    const size_t tableBytes = table_.size();
    u32 tableSize;
    std::memcpy(&tableSize, base, 4);
    // Entry "pointers" are byte offsets into the table. The original reads raw memory; the port bounds-checks.
    size_t e = 4;
    size_t end = static_cast<size_t>(tableSize);     // = (table + 4) + tableSize - 4
    auto ok = [&](size_t o, size_t n) { return o + n <= tableBytes; };
    auto name = [&](size_t o) { return reinterpret_cast<const char*>(base + o); };
    auto nameLen = [&](size_t o, size_t& out) {      // strlen with bounds check
        const void* nul = std::memchr(base + o, 0, tableBytes - o);
        if (!nul) return false;
        out = static_cast<const u8*>(nul) - (base + o);
        return true;
    };
    auto field = [&](size_t o, s32& v) {
        if (!ok(o, 4)) return false;
        std::memcpy(&v, base + o, 4);
        return true;
    };
    // Advance e past the current entry. Returns false on malformed data.
    auto skip = [&]() {
        size_t n;
        if (!ok(e, 1) || !nameLen(e, n)) return false;
        size_t f = e + ((n + 4) & ~size_t{3});
        if (static_cast<s8>(base[e]) < 0) {         // directory: skip its children
            s32 length;
            if (!field(f, length)) return false;
            e = f + static_cast<size_t>(static_cast<s64>(length));
        } else {
            e = f + 8;                               // file: size, offset
        }
        return true;
    };

    char* p = buf;
    while (*p) {
        char* q = p;
        while (*q != '\\') {
            if (*++q == 0) goto last_component;
        }
        *q = 0;
        *p = static_cast<char>(static_cast<u8>(*p) | 0x80);   // directories are stored with bit 7 set
        if (end <= e) return false;
        while (!ok(e, 1) || StrICmp(name(e), p) != 0) {
            if (!skip()) return false;
            if (end <= e) return false;
        }
        if (end <= e) return false;
        {
            size_t n;
            if (!nameLen(e, n)) return false;
            size_t f = e + ((n + 4) & ~size_t{3});
            s32 length;
            if (!field(f, length)) return false;
            end = f + static_cast<size_t>(static_cast<s64>(length)) - 4;   // orig: &length + length - 4
            e = f + 4;
        }
        p = q + 1;
    }
last_component:
    if (e < end) {
        while (!ok(e, 1) || StrICmp(name(e), p) != 0) {
            if (!skip()) return false;
            if (end <= e) return false;
        }
        if (e < end) {
            size_t n;
            if (!nameLen(e, n)) return false;
            size_t f = e + ((n + 4) & ~size_t{3});
            s32 sz, off;
            if (!field(f, sz) || !field(f + 4, off)) return false;
            size = static_cast<u32>(sz);
            offset = static_cast<u32>(off);
            return true;
        }
    }
    return false;
}

void ArchiveSet::Add(std::string imgPath, ImgArchive archive) {
    paths_.push_back(std::move(imgPath));
    archives_.push_back(std::move(archive));
}

// orig: 0x0013FEA0 FindInArchives (first match in mount order)
bool ArchiveSet::Find(const char* name, Hit& hit) const {
    for (size_t i = 0; i < archives_.size(); ++i) {
        if (archives_[i].Find(name, hit.offset, hit.size)) {
            hit.archive = i;
            return true;
        }
    }
    return false;
}

}  // namespace fut::fs

// XDVDFS reader; see Xdvdfs.h.
#include "core/fs/Xdvdfs.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace fut::fs {
namespace {
constexpr u32 kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr u64 kPartitionOffsets[] = {0x0, 0x18300000, 0xFD90000, 0x2080000};   // XISO, XGD1, XGD2, XGD3
constexpr u8 kAttrDirectory = 0x10;

bool Seek(std::FILE* f, u64 offset) {
#if defined(_WIN32)
    if (offset > static_cast<u64>(std::numeric_limits<s64>::max())) return false;
    return _fseeki64(f, static_cast<s64>(offset), SEEK_SET) == 0;
#else
    static_assert(sizeof(off_t) >= 8, "Xbox disc access requires 64-bit offsets");
    if (offset > static_cast<u64>(std::numeric_limits<off_t>::max())) return false;
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
}  // namespace

std::string Xdvdfs::Normalize(const std::string& path) {
    std::string s;
    s.reserve(path.size());
    for (char c : path) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (c == '/' && s.empty()) continue;
        s += c;
    }
    return s;
}

bool Xdvdfs::Open(const std::string& imagePath, std::string& error) {
    entries_.clear();
    std::FILE* raw = std::fopen(imagePath.c_str(), "rb");
    if (!raw) {
        error = "cannot open " + imagePath;
        return false;
    }
    file_.reset(raw);
    char magic[sizeof kMagic - 1];
    bool found = false;
    for (u64 off : kPartitionOffsets) {
        if (!Seek(raw, off + 32 * kSector) || std::fread(magic, 1, sizeof magic, raw) != sizeof magic) continue;
        if (std::memcmp(magic, kMagic, sizeof magic) == 0) {
            partition_ = off;
            found = true;
            break;
        }
    }
    if (!found) {
        error = "not an Xbox disc image (XDVDFS volume descriptor not found)";
        return false;
    }
    u32 root[2];
    if (std::fread(root, 4, 2, raw) != 2) {
        error = "truncated volume descriptor";
        return false;
    }
    Walk(root[0], root[1], "", 0);
    return true;
}

void Xdvdfs::Walk(u32 sector, u32 size, const std::string& prefix, int depth) {
    if (size == 0 || depth > 64) return;
    std::vector<u8> table(size);
    if (!Read(partition_ + static_cast<u64>(sector) * kSector, table.data(), size)) return;
    // The directory is a binary tree of entries at 4-byte offsets: left / right subtree links (in dwords).
    std::vector<u32> stack{0};
    std::vector<bool> seen(size / 4 + 1, false);
    while (!stack.empty()) {
        const u32 off = stack.back();
        stack.pop_back();
        if (off + 14 > size || seen[off / 4]) continue;
        seen[off / 4] = true;
        u16 left, right;
        u32 entrySector, entrySize;
        std::memcpy(&left, &table[off], 2);
        std::memcpy(&right, &table[off + 2], 2);
        std::memcpy(&entrySector, &table[off + 4], 4);
        std::memcpy(&entrySize, &table[off + 8], 4);
        const u8 attrs = table[off + 12], nameLen = table[off + 13];
        if (left == 0xFFFF && right == 0xFFFF) continue;   // padding
        if (off + 14 + nameLen > size) continue;
        const std::string name(reinterpret_cast<const char*>(&table[off + 14]), nameLen);
        const std::string path = Normalize(prefix.empty() ? name : prefix + "/" + name);
        Entry e;
        e.offset = partition_ + static_cast<u64>(entrySector) * kSector;
        e.size = entrySize;
        e.directory = (attrs & kAttrDirectory) != 0;
        entries_[path] = e;
        if (e.directory) Walk(entrySector, entrySize, path, depth + 1);
        if (left) stack.push_back(static_cast<u32>(left) * 4);
        if (right) stack.push_back(static_cast<u32>(right) * 4);
    }
}

const Xdvdfs::Entry* Xdvdfs::Find(const std::string& path) const {
    const auto it = entries_.find(Normalize(path));
    return it == entries_.end() ? nullptr : &it->second;
}

bool Xdvdfs::Read(u64 offset, void* out, size_t size) const {
    if (!file_ || !Seek(file_.get(), offset)) return false;
    return std::fread(out, 1, size, file_.get()) == size;
}

bool Xdvdfs::ReadFile(const std::string& path, std::vector<u8>& out) const {
    const Entry* e = Find(path);
    if (!e || e->directory) return false;
    out.resize(e->size);
    return e->size == 0 || Read(e->offset, out.data(), e->size);
}

}  // namespace fut::fs

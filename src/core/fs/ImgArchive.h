// .img archive directory lookup: decompiled from default.xbe (NiUDSStream translation unit).
//   orig: 0x0013FCD0 ImgArchive::Find, 0x0013FF00 MountArchive, 0x0013FEA0 FindInArchives
//
// Archive layout (shared by the Xbox and PS2 builds; see tools/img_archive.py):
//   u32 tableSize, then packed entries, names NUL-terminated and padded to 4 bytes:
//     file:      name, u32 size, u32 offset (absolute in the .img)
//     directory: name with bit 7 set on its first character, s32 length; children end at &length + length
//
// The game loads exactly tableSize bytes from offset 0 (so the table's own size word is included and the last 4
// bytes of the table are never loaded) and scans entries in [table + 4, table + tableSize).
#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

namespace fut::fs {

class ImgArchive {
public:
    ImgArchive() = default;

    // `table` = the first tableSize bytes of the .img file, exactly as MountArchive reads them.
    // kGuardBytes zero bytes are appended: the original can match the table's zero padding as an empty-named entry
    // (lookup of "") and then reads size/offset from just past the loaded buffer. The guard makes that read
    // deterministic (zeros) instead of heap garbage. See docs/KNOWN_ISSUES.md.
    explicit ImgArchive(std::vector<u8> table) : table_(std::move(table)) { table_.resize(table_.size() + kGuardBytes, 0); }

    static constexpr size_t kGuardBytes = 16;

    // Reads the directory table from an open .img file the way MountArchive does. Returns false on I/O error.
    static bool ReadTable(const std::string& imgPath, std::vector<u8>& table);

    // orig: 0x0013FCD0 ImgArchive::Find. Case-insensitive; '/' and '\' are both separators.
    // Returns the file's size and absolute offset inside the .img.
    bool Find(const char* path, u32& offset, u32& size) const;

    const std::vector<u8>& Table() const { return table_; }

private:
    std::vector<u8> table_;
};

// orig: g_Archives (0x002D7B8C) + MountArchive / FindInArchives. Lookup is first-match in mount order.
class ArchiveSet {
public:
    struct Hit {
        size_t archive;   // index in mount order
        u32 offset;
        u32 size;
    };
    void Add(std::string imgPath, ImgArchive archive);
    bool Find(const char* name, Hit& hit) const;
    size_t Count() const { return archives_.size(); }
    const std::string& PathOf(size_t index) const { return paths_[index]; }

private:
    std::vector<std::string> paths_;
    std::vector<ImgArchive> archives_;
};

}  // namespace fut::fs

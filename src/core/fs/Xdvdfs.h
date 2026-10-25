// Xbox disc file system (XDVDFS) reader for the user's own disc image - port-only (the Xbox kernel read the disc).
// Accepts a plain XISO or a full (Redump-style) image; the game partition is found by probing the XGD1/XGD2/XGD3
// offsets, as tools/xiso_extract.py does. File names are matched case-insensitively ('/' or '\' separators).
#pragma once

#include "core/Types.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fut::fs {

class Xdvdfs {
public:
    struct Entry {
        u64 offset = 0;                                // absolute byte offset in the image
        u32 size = 0;
        bool directory = false;
    };

    // Opens the image and indexes every file. Returns false (with `error` set) when it is not an Xbox disc image.
    bool Open(const std::string& imagePath, std::string& error);
    // A file or directory by path relative to the disc root (e.g. "FutNTSC00.img", "video/intro.xmv").
    const Entry* Find(const std::string& path) const;
    // Reads `size` bytes at `offset` of the image. False on a short read.
    bool Read(u64 offset, void* out, size_t size) const;
    // Whole-file read.
    bool ReadFile(const std::string& path, std::vector<u8>& out) const;
    size_t FileCount() const { return entries_.size(); }

    static std::string Normalize(const std::string& path);   // lower case, '/' separators, no leading '/'

private:
    struct FileCloser {
        void operator()(std::FILE* f) const { if (f) std::fclose(f); }
    };
    void Walk(u32 sector, u32 size, const std::string& prefix, int depth);

    std::unique_ptr<std::FILE, FileCloser> file_;
    u64 partition_ = 0;
    std::map<std::string, Entry> entries_;
};

}  // namespace fut::fs

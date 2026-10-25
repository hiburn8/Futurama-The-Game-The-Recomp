// The game's file access over the user's own disc: Phase 3 replacement for the Xbox D:\ drive and the NiUDSStream
// open path (NOTES "File system"). The source is a disc image (Xdvdfs) or a folder holding the disc's files.
// Mounting follows the app start-up 0x000D2730: sysPAL.img, else sysNTSC.img; then FutPAL00.img, FutPAL01.img, ...
// up to the first missing one (at most 100), and only when there is none FutNTSC00.img, ... - the region (PAL / NTSC,
// hence the 50 / 60 Hz tick) is the one whose archives exist. Opening a file tries the archives first (first match in
// mount order, ImgArchive::Find), then the disc path itself.
#pragma once

#include "core/Types.h"
#include "core/fs/ImgArchive.h"
#include "core/fs/Xdvdfs.h"

#include <memory>
#include <string>
#include <vector>

namespace fut::fs {

class Vfs {
public:
    // A disc image (.iso) or a directory with the disc's files (default.xbe, *.img, ...).
    bool Open(const std::string& source, std::string& error);
    bool ReadFile(const std::string& name, std::vector<u8>& out) const;
    bool Exists(const std::string& name) const;

    bool IsPal() const { return pal_; }
    const ArchiveSet& Archives() const { return archives_; }
    // A disc file (not an archive's): its size, a part of it (the movie player's streamed reads).
    bool ReadDisc(const std::string& path, u64 offset, void* out, size_t size) const;
    bool DiscFileSize(const std::string& path, u64& size) const;

private:
    bool Mount(const std::string& imgName);            // orig: MountArchive (0x00140040), false if missing

    std::unique_ptr<Xdvdfs> image_;                    // or
    std::string directory_;                            // a folder with the disc's files
    ArchiveSet archives_;
    bool pal_ = false;
};

}  // namespace fut::fs

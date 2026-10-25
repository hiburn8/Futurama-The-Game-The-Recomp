// VFS over the user's disc; see Vfs.h.
#include "core/fs/Vfs.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fut::fs {
namespace {

// A disc path inside a folder source, matched case-insensitively component by component.
bool ResolveInFolder(const std::string& root, const std::string& name, std::filesystem::path& out) {
    namespace sfs = std::filesystem;
    sfs::path cur(root);
    const std::string norm = Xdvdfs::Normalize(name);
    size_t pos = 0;
    while (pos <= norm.size()) {
        const size_t next = norm.find('/', pos);
        const std::string part = norm.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? norm.size() + 1 : next + 1;
        if (part.empty()) continue;
        std::error_code ec;
        bool found = false;
        for (const auto& entry : sfs::directory_iterator(cur, ec)) {
            if (Xdvdfs::Normalize(entry.path().filename().string()) == part) {
                cur = entry.path();
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    out = cur;
    return true;
}

}  // namespace

bool Vfs::Open(const std::string& source, std::string& error) {
    namespace sfs = std::filesystem;
    image_.reset();
    directory_.clear();
    archives_ = ArchiveSet();
    std::error_code ec;
    if (sfs::is_directory(source, ec)) {
        directory_ = source;
    } else {
        image_ = std::make_unique<Xdvdfs>();
        if (!image_->Open(source, error)) {
            image_.reset();
            return false;
        }
    }
    // orig: 0x000D2730.
    pal_ = Mount("sysPAL.img");
    if (!pal_) Mount("sysNTSC.img");
    u32 n = 0;
    char name[32];
    for (; n < 100; ++n) {
        std::snprintf(name, sizeof name, "FutPAL%02u.img", n);
        if (!Mount(name)) break;
    }
    if (n == 0) {
        for (; n < 100; ++n) {
            std::snprintf(name, sizeof name, "FutNTSC%02u.img", n);
            if (!Mount(name)) break;
        }
    } else {
        pal_ = true;
    }
    if (archives_.Count() == 0) {
        error = "no game archives (sys*.img / Fut*.img) on the disc";
        return false;
    }
    return true;
}

bool Vfs::DiscFileSize(const std::string& path, u64& size) const {
    if (image_) {
        const Xdvdfs::Entry* e = image_->Find(path);
        if (!e || e->directory) return false;
        size = e->size;
        return true;
    }
    std::filesystem::path p;
    std::error_code ec;
    if (!ResolveInFolder(directory_, path, p) || !std::filesystem::is_regular_file(p, ec)) return false;
    size = std::filesystem::file_size(p, ec);
    return !ec;
}

bool Vfs::ReadDisc(const std::string& path, u64 offset, void* out, size_t size) const {
    if (image_) {
        const Xdvdfs::Entry* e = image_->Find(path);
        if (!e || e->directory || offset + size > e->size) return false;
        return size == 0 || image_->Read(e->offset + offset, out, size);
    }
    std::filesystem::path p;
    if (!ResolveInFolder(directory_, path, p)) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    return static_cast<size_t>(in.gcount()) == size;
}

bool Vfs::Mount(const std::string& imgName) {          // orig: MountArchive 0x00140040 (table read like ReadTable)
    u64 fileSize;
    if (!DiscFileSize(imgName, fileSize) || fileSize < 4) return false;
    u32 tableSize;
    if (!ReadDisc(imgName, 0, &tableSize, 4) || tableSize < 4 || tableSize > fileSize) return false;
    std::vector<u8> table(tableSize);
    if (!ReadDisc(imgName, 0, table.data(), tableSize)) return false;
    archives_.Add(imgName, ImgArchive(std::move(table)));
    return true;
}

bool Vfs::ReadFile(const std::string& name, std::vector<u8>& out) const {
    ArchiveSet::Hit hit;
    if (archives_.Find(name.c_str(), hit)) {
        out.resize(hit.size);
        return hit.size == 0 || ReadDisc(archives_.PathOf(hit.archive), hit.offset, out.data(), hit.size);
    }
    u64 size;
    if (!DiscFileSize(name, size)) return false;
    out.resize(static_cast<size_t>(size));
    return size == 0 || ReadDisc(name, 0, out.data(), static_cast<size_t>(size));
}

bool Vfs::Exists(const std::string& name) const {
    ArchiveSet::Hit hit;
    u64 size;
    return archives_.Find(name.c_str(), hit) || DiscFileSize(name, size);
}

}  // namespace fut::fs

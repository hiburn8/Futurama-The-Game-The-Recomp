#pragma once
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Scratch-only preview sessions, deliberately separate from installed player data.
class PreviewSaves {
    int directory_ = -1;
    static constexpr std::string_view marker = "FUTURAMA_PREVIEW_SAVES_V1\n";
    static void Check(bool ok, const char* operation) {
        if (!ok) throw std::system_error(errno, std::generic_category(), operation);
    }
public:
    PreviewSaves(const std::filesystem::path& path, bool resume) {
        for (const auto& component : path)
            if (component == "..") throw std::runtime_error("parent traversal is not a scratch-save path");
        auto root = path.lexically_normal();
        while (root != root.root_path() && !root.has_filename()) root = root.parent_path();
        if (!resume && !std::filesystem::create_directory(root))
            throw std::runtime_error("preview requires a new scratch-save directory");
        directory_ = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        Check(directory_ >= 0, "open scratch-save directory");
        int file = -1;
        try {
            Check(::flock(directory_, LOCK_EX | LOCK_NB) == 0, "lock scratch saves");
            const int flags = resume ? O_RDONLY | O_NONBLOCK : O_WRONLY | O_CREAT | O_EXCL;
            file = ::openat(directory_, ".futurama-preview", flags | O_NOFOLLOW | O_CLOEXEC, 0600);
            Check(file >= 0, "open preview ownership marker");
            if (resume) {
                struct stat st{};
                Check(::fstat(file, &st) == 0, "stat preview marker");
                if (!S_ISREG(st.st_mode) || st.st_size != static_cast<off_t>(marker.size()))
                    throw std::runtime_error("invalid preview ownership marker");
                char bytes[64]{};
                const auto n = ::read(file, bytes, sizeof bytes);
                Check(n >= 0, "read preview marker");
                if (std::string_view(bytes, static_cast<size_t>(n)) != marker)
                    throw std::runtime_error("unrecognized preview ownership marker");
            } else {
                Check(::write(file, marker.data(), marker.size()) == static_cast<ssize_t>(marker.size()), "write preview marker");
                Check(::fsync(file) == 0, "flush preview marker");
                Check(::fsync(directory_) == 0, "flush scratch-save directory");
            }
            const int closing = file;
            file = -1;
            Check(::close(closing) == 0, "close preview marker");
        } catch (...) {
            if (file >= 0) ::close(file);
            ::close(directory_);
            directory_ = -1;
            throw;
        }
    }
    ~PreviewSaves() { if (directory_ >= 0) ::close(directory_); }
    PreviewSaves(const PreviewSaves&) = delete;
    PreviewSaves& operator=(const PreviewSaves&) = delete;
};

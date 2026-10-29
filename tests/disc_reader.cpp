#include "core/fs/Xdvdfs.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path path(argv[1]);
    // A sparse, synthetic volume: no original game data. Caller owns its directory.
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.seekp(32 * 2048);
    file.write("MICROSOFT*XBOX*MEDIA", 20);
    const std::array<char, 8> emptyRoot{};
    file.write(emptyRoot.data(), emptyRoot.size());
    constexpr std::uint64_t high = (std::uint64_t{1} << 32) + 123;
    file.seekp(high);
    file.put('Z');
    file.close();
    if (!file) return 3;
    fut::fs::Xdvdfs reader;
    std::string error;
    if (!reader.Open(path.string(), error)) { std::cerr << error; return 4; }
    char value = 0;
    if (!reader.Read(high, &value, 1) || value != 'Z') return 5;
    if (reader.Read(high + 1, &value, 1)) return 6;
    if (reader.Read(std::numeric_limits<std::uint64_t>::max(), &value, 1)) return 7;
    fut::fs::Xdvdfs missing;
    if (missing.Open((path.parent_path() / "absent-file").string(), error)) return 8;
    return 0;
}

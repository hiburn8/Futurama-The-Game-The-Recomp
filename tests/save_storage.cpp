#include "game/app/SaveStorage.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using fut::game::app::HostSaveStorage;
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected scratch parent");
        fs::path parent(argv[1]);
        fs::create_directories(parent);
        // Keep every prior run, including a failed fixture, for inspection.
        fs::path root;
        for (unsigned i = 0; i < 10000; ++i) {
            auto candidate = parent / std::to_string(i);
            if (fs::create_directory(candidate)) { root = candidate; break; }
        }
        Require(!root.empty(), "no fresh test directory available");
        const auto invalid = root / "regular-file";
        std::ofstream(invalid) << "not a directory";
        bool refused = false;
        try { HostSaveStorage saves(invalid); }
        catch (const fs::filesystem_error&) { refused = true; }
        Require(refused, "regular-file save root accepted");
        refused = false;
        try { HostSaveStorage saves(invalid / "child"); }
        catch (const fs::filesystem_error&) { refused = true; }
        Require(refused, "invalid ancestor accepted");

        const auto good = root / "valid";
        std::string path;
        {
            HostSaveStorage saves(good);
            Require(saves.saves.empty(), "new store is not empty");
            Require(saves.CreateSaveGame(u"Fry", 1, 0, path, 0) == 0, "create save");
            auto handle = saves.CreateFile(path + "save.dat", 0, 2);
            const fut::u8 data[] = {1, 2, 3, 4};
            Require(saves.WriteFileEx(handle, data, 4, 1), "write save");
            Require(saves.CloseHandle(handle), "close save");
        }
        HostSaveStorage reopened(good);
        Require(reopened.saves.size() == 1 && reopened.saves[0].name == u"Fry", "reopen metadata");
        auto handle = reopened.CreateFile(path + "save.dat", 0, 3);
        std::vector<fut::u8> data;
        Require(reopened.ReadFile(handle, 4, data) && data == std::vector<fut::u8>({1,2,3,4}), "reopen bytes");
#ifndef _WIN32
        Require(geteuid() != 0, "permission cases require an unprivileged test user");
        auto unreadable = [&](const fs::path& target) {
            const auto previous = fs::status(target).permissions();
            fs::permissions(target, fs::perms::none);
            bool rejected = false;
            try { HostSaveStorage saves(good); }
            catch (const fs::filesystem_error&) { rejected = true; }
            catch (const std::ios_base::failure&) { rejected = true; }
            catch (...) { fs::permissions(target, previous); throw; }
            fs::permissions(target, previous);
            Require(rejected, "unreadable save data accepted");
        };
        unreadable(good);
        unreadable(good / reopened.saves[0].dir / "name.txt");
        unreadable(good / reopened.saves[0].dir / "save.dat");
#endif
        std::cout << "save root/read refusal and normal reopen passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

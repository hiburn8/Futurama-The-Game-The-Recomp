#include "PreviewSaves.h"
#include <fstream>
#include <iostream>
#include <sys/wait.h>

namespace fs = std::filesystem;
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void Refuses(F operation) {
    bool refused = false;
    try { operation(); } catch (const std::exception&) { refused = true; }
    Require(refused, "unsafe preview session accepted");
}
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--resume") {
            PreviewSaves session(argv[2], true);
            std::string payload;
            std::ifstream(fs::path(argv[2]) / "payload.txt") >> payload;
            Require(payload == "preserved", "resume changed payload");
            return 0;
        }
        Require(argc == 2, "expected scratch parent");
        fs::path parent(argv[1]), root;
        fs::create_directories(parent);
        for (unsigned i = 0; i < 10000; ++i) {
            auto p = parent / std::to_string(i);
            if (fs::create_directory(p)) { root = p; break; }
        }
        Require(!root.empty(), "no fresh fixture directory");
        const auto good = root / "good";
        {
            PreviewSaves session(good, false);
            std::ofstream(good / "payload.txt") << "preserved";
            Refuses([&] { PreviewSaves duplicate(good, true); });
            Refuses([&] { PreviewSaves overwrite(good, false); });
        }
        const pid_t pid = ::fork();
        Require(pid >= 0, "fork failed");
        if (pid == 0) {
            ::execl(argv[0], argv[0], "--resume", good.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        int status = 0;
        Require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "fresh-process resume failed");
        Refuses([&] { PreviewSaves missing(root / "absent", true); });
        const auto unmarked = root / "unmarked";
        fs::create_directory(unmarked);
        Refuses([&] { PreviewSaves unknown(unmarked, true); });
        std::ofstream(unmarked / ".futurama-preview") << "not a preview";
        Refuses([&] { PreviewSaves corrupt(unmarked, true); });
        fs::create_directory_symlink(good, root / "link");
        Refuses([&] { PreviewSaves alias(root / "link", true); });
        Refuses([&] { PreviewSaves alias(root / "link" / "", true); });
        Refuses([&] { PreviewSaves alias(root / "link" / ".", true); });
        Refuses([&] { PreviewSaves alias(root / "good" / ".." / "good", true); });
        const auto indirect = root / "indirect";
        fs::create_directory(indirect);
        fs::create_symlink(good / ".futurama-preview", indirect / ".futurama-preview");
        Refuses([&] { PreviewSaves alias(indirect, true); });
        std::cout << "preview isolation, locking and fresh-process resume passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

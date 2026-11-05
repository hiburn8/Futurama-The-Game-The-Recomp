// The launcher side of fut_game; see Launcher.h.
#include "platform/sdl/Launcher.h"

#include "platform/sdl/Window.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <vector>

namespace fut::platform {
namespace {
namespace sfs = std::filesystem;

std::string Utf8(const sfs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

sfs::path PathOf(const std::string& utf8) { return sfs::path(std::u8string(utf8.begin(), utf8.end())); }

// The path as the narrow string Vfs::Open takes (the C library's narrow file names: on Windows the ANSI code page,
// as a command-line argument; a path with characters outside it goes by its short name; "" when neither works).
// Elsewhere file names are UTF-8 already.
std::string Narrow(const sfs::path& p) {
#ifdef _WIN32
    auto acp = [](const std::wstring& w, std::string& out) {
        BOOL lossy = FALSE;
        const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), nullptr,
                                          0, nullptr, &lossy);
        if (n <= 0 || lossy) return false;
        out.assign(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                            nullptr);
        return true;
    };
    std::string s;
    if (acp(p.wstring(), s)) return s;
    std::wstring shortName(32768, L'\0');
    const DWORD n = GetShortPathNameW(p.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    if (n == 0 || n >= shortName.size()) return {};
    shortName.resize(n);
    return acp(shortName, s) ? s : std::string();
#else
    return Utf8(p);
#endif
}

sfs::path SettingsFile() { return PrefFolder() / "settings.ini"; }

sfs::path Remembered(const sfs::path& settings) {
    std::ifstream in(settings, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("disc=", 0) == 0) return PathOf(line.substr(5));
    }
    return {};
}

void Remember(const sfs::path& settings, const sfs::path& disc) {
    if (settings.empty()) return;
    std::error_code ec;
    sfs::create_directories(settings.parent_path(), ec);
    std::ofstream out(settings, std::ios::binary | std::ios::trunc);
    out << "disc=" << Utf8(disc) << "\r\n";
}

sfs::path ExeFolder() {
    const char* base = SDL_GetBasePath();             // (the exe's folder, UTF-8, with a trailing separator)
    return base ? PathOf(base) : sfs::current_path();
}

bool HasXbe(const sfs::path& dir) {
    std::error_code ec;
    return sfs::is_regular_file(dir / "default.xbe", ec);
}

bool NameIs(const sfs::path& p, const char* name) {
    std::string n = Utf8(p);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n == name;
}

// Steps 2 and 3 of the search (Launcher.h), in order.
std::vector<sfs::path> NextToExe(const sfs::path& dir) {
    std::vector<sfs::path> isos, folders;
    std::error_code ec;
    for (auto it = sfs::directory_iterator(dir, ec); !ec && it != sfs::directory_iterator(); it.increment(ec)) {
        const sfs::path& p = it->path();
        if (it->is_regular_file(ec) && NameIs(p.extension(), ".iso")) isos.push_back(p);
        else if (it->is_directory(ec) && !NameIs(p.filename(), "futurama") && HasXbe(p)) folders.push_back(p);
    }
    std::vector<sfs::path> out;
    if (isos.size() == 1) out.push_back(isos[0]);
    if (HasXbe(dir / "futurama")) out.push_back(dir / "futurama");
    if (!folders.empty()) out.push_back(folders[0]);
    if (HasXbe(dir)) out.push_back(dir);
    return out;
}

struct DialogResult {
    std::mutex m;
    bool done = false;
    std::string path;                                 // UTF-8; empty = cancelled or failed
};

void SDLCALL OnDiscPicked(void* userdata, const char* const* files, int) {   // (may run on another thread)
    auto* r = static_cast<DialogResult*>(userdata);
    std::lock_guard g(r->m);
    if (files && files[0]) r->path = files[0];
    r->done = true;
}

// The file-open dialog: an .iso, or the default.xbe of an extracted folder (the folder is the source). Empty when
// cancelled. (SDL's dialog is asynchronous: the events are pumped until its callback has run.)
sfs::path AskDisc(const sfs::path& dir) {
    Window::InitSdl();                                // (the dialog needs the video subsystem)
    static const SDL_DialogFileFilter kFilters[] = {{"Futurama (Xbox) disc image (*.iso)", "iso"},
                                                    {"Extracted disc folder (its default.xbe)", "xbe"},
                                                    {"All files", "*"}};
    DialogResult r;
    const std::string initial = Utf8(dir);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(kFilters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 3);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, initial.c_str());
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Select your Futurama (Xbox) disc image");
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, OnDiscPicked, &r, props);
    SDL_DestroyProperties(props);
    for (;;) {
        SDL_PumpEvents();
        {
            std::lock_guard g(r.m);
            if (r.done) break;
        }
        SDL_Delay(10);
    }
    if (r.path.empty()) return {};
    sfs::path p = PathOf(r.path);
    if (NameIs(p.filename(), "default.xbe")) p = p.parent_path();
    return p;
}

}  // namespace

bool SetUpConsole() {
#ifdef _WIN32
    if (GetConsoleWindow()) return false;              // (the console subsystem, or a console of its own)
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if ((err == nullptr || err == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    return true;
#else
    return false;
#endif
}

void ErrorBox(const std::string& text) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Futurama", text.c_str(), nullptr);
}

std::filesystem::path PrefFolder() {
    char* p = SDL_GetPrefPath("", "FuturamaRecomp");  // (%APPDATA%\FuturamaRecomp\ on Windows)
    if (!p) return sfs::path("FuturamaRecomp");
    const sfs::path folder = PathOf(p);
    SDL_free(p);
    return folder;
}

std::string FindDisc(const std::function<bool(const std::string& source, std::string& error)>& check) {
    std::string error;
    auto test = [&](const sfs::path& p, std::string& source) {
        source = Narrow(p);
        if (source.empty()) {
            error = "the path has characters that cannot be opened here (try a folder with a plain name)";
            return false;
        }
        return check(source, error);
    };
    std::string source;
    const sfs::path settings = SettingsFile();
    std::error_code ec;
    if (const sfs::path p = Remembered(settings); !p.empty() && sfs::exists(p, ec) && test(p, source)) return source;
    const sfs::path dir = ExeFolder();
    for (const sfs::path& p : NextToExe(dir)) {
        if (test(p, source)) return source;
        ErrorBox("Found " + Utf8(p) + " next to the game, but it can't be used:\n\n" + error);
    }
    for (;;) {
        const sfs::path p = AskDisc(dir);
        if (p.empty()) return {};
        if (test(p, source)) {
            Remember(settings, p);
            return source;
        }
        const std::string text = "That one can't be used:\n\n" + error + "\n\nThe game needs your own Futurama (Xbox, "
                                 "NTSC-U or PAL) disc image (.iso), or the default.xbe of the extracted disc.";
        const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
                                                    {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Pick another"}};
        const SDL_MessageBoxData box = {SDL_MESSAGEBOX_WARNING, nullptr, "Futurama", text.c_str(), 2, buttons, nullptr};
        int pressed = 0;
        if (!SDL_ShowMessageBox(&box, &pressed) || pressed != 1) return {};
    }
}

}  // namespace fut::platform

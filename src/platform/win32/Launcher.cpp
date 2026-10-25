// The launcher side of fut_game; see Launcher.h.
#include "platform/win32/Launcher.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fut::platform {
namespace {
namespace sfs = std::filesystem;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The path as the narrow string Vfs::Open takes (the ANSI code page, as a command-line argument); a path with
// characters outside it goes by its short name. "" when neither works.
std::string Narrow(const sfs::path& p) {
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
}

sfs::path SettingsFile() {
    wchar_t* appData = nullptr;
    size_t n = 0;
    sfs::path p;
    if (_wdupenv_s(&appData, &n, L"APPDATA") == 0 && appData) {
        p = sfs::path(appData) / L"FuturamaRecomp" / L"settings.ini";
        std::free(appData);
    }
    return p;
}

sfs::path Remembered(const sfs::path& settings) {
    std::ifstream in(settings, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("disc=", 0) == 0) return sfs::path(Widen(line.substr(5)));
    }
    return {};
}

void Remember(const sfs::path& settings, const sfs::path& disc) {
    if (settings.empty()) return;
    std::error_code ec;
    sfs::create_directories(settings.parent_path(), ec);
    std::ofstream out(settings, std::ios::binary | std::ios::trunc);
    out << "disc=" << Utf8(disc.wstring()) << "\r\n";
}

sfs::path ExeFolder() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return sfs::path(buf).parent_path();
}

bool HasXbe(const sfs::path& dir) {
    std::error_code ec;
    return sfs::is_regular_file(dir / L"default.xbe", ec);
}

// Steps 2 and 3 of the search (Launcher.h), in order.
std::vector<sfs::path> NextToExe(const sfs::path& dir) {
    std::vector<sfs::path> isos, folders;
    std::error_code ec;
    for (auto it = sfs::directory_iterator(dir, ec); !ec && it != sfs::directory_iterator(); it.increment(ec)) {
        const sfs::path& p = it->path();
        if (it->is_regular_file(ec) && _wcsicmp(p.extension().c_str(), L".iso") == 0) isos.push_back(p);
        else if (it->is_directory(ec) && _wcsicmp(p.filename().c_str(), L"futurama") != 0 && HasXbe(p))
            folders.push_back(p);
    }
    std::vector<sfs::path> out;
    if (isos.size() == 1) out.push_back(isos[0]);
    if (HasXbe(dir / L"futurama")) out.push_back(dir / L"futurama");
    if (!folders.empty()) out.push_back(folders[0]);
    if (HasXbe(dir)) out.push_back(dir);
    return out;
}

// The file-open dialog: an .iso, or the default.xbe of an extracted folder (the folder is the source). Empty when
// cancelled.
sfs::path AskDisc(const sfs::path& dir) {
    std::wstring file(32768, L'\0');
    const std::wstring initial = dir.wstring();
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"Futurama (Xbox) disc image (*.iso)\0*.iso\0"
                      L"Extracted disc folder (its default.xbe)\0default.xbe\0"
                      L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrInitialDir = initial.c_str();
    ofn.lpstrTitle = L"Select your Futurama (Xbox) disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    sfs::path p(file.c_str());
    if (_wcsicmp(p.filename().c_str(), L"default.xbe") == 0) p = p.parent_path();
    return p;
}

}  // namespace

bool SetUpConsole() {
    if (GetConsoleWindow()) return false;              // (the console subsystem, or a console of its own)
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if ((err == nullptr || err == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
    return true;
}

void ErrorBox(const std::string& text) {
    MessageBoxW(nullptr, Widen(text).c_str(), L"Futurama", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
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
        ErrorBox("Found " + Utf8(p.wstring()) + " next to the game, but it can't be used:\n\n" + error);
    }
    for (;;) {
        const sfs::path p = AskDisc(dir);
        if (p.empty()) return {};
        if (test(p, source)) {
            Remember(settings, p);
            return source;
        }
        const std::string text = "That one can't be used:\n\n" + error + "\n\nThe game needs your own Futurama (Xbox, "
                                 "NTSC-U) disc image (.iso), or the default.xbe of the extracted disc.\n\nPick another?";
        if (MessageBoxW(nullptr, Widen(text).c_str(), L"Futurama", MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND)
            != IDOK)
            return {};
    }
}

}  // namespace fut::platform

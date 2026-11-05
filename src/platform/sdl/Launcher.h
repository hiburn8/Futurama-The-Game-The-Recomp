// The launcher side of fut_game (platform layer, not the game): a start without a console (the release build is a
// Windows-subsystem program, double-clicked from Explorer) and without a disc path, and the folder the game's own
// files live in. Nothing here touches the game. On SDL3 (the message boxes, the file dialog, the pref folder); the
// console attach is Windows only.
//
// The disc search for a start without a path argument, first hit wins:
//   1. the path remembered in the pref folder's settings.ini (one line `disc=<path>`, UTF-8);
//   2. the one *.iso next to the exe (only when there is exactly one);
//   3. an extracted folder next to the exe: `futurama`, else a subfolder holding default.xbe, else the exe's own
//      folder when it holds default.xbe;
//   4. a file-open dialog (an .iso, or the default.xbe inside an extracted folder), asked again while the pick fails
//      the check; a good pick is remembered (1).
// A candidate that fails the check (an image that is not the NTSC-U or PAL disc, ...) is reported in a message box and the
// search goes on; a remembered one that fails is skipped quietly.
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace fut::platform {

// Started without a console of its own (the Windows subsystem): attaches stdout / stderr to the parent's console when
// it has one (a start from a terminal; inherited handles, e.g. a redirection, are kept). True when the errors should
// also go to message boxes (no console of its own at start). Other platforms: false.
bool SetUpConsole();

// The error in a message box (UTF-8 text), titled like the game window.
void ErrorBox(const std::string& text);

// The game's own folder for settings.ini and the saves: %APPDATA%\FuturamaRecomp on Windows (as before SDL),
// ~/.local/share/FuturamaRecomp on Linux (SDL_GetPrefPath); created if missing.
std::filesystem::path PrefFolder();

// The disc search above. `check(source, error)`: the game's test of a source (the string Vfs::Open takes). The source
// found, or "" when the dialog was cancelled.
std::string FindDisc(const std::function<bool(const std::string& source, std::string& error)>& check);

}  // namespace fut::platform

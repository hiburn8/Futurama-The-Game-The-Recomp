#include "game/text/LanguageDb.h"
// fut_game: the port booted like the original (main 0x000D2A40): the App (src/game/app: the Intro, the front end, the
// Ingame context with the pause menu, the game-over screen) on the user's own disc image / extracted folder, drawn by
// the decompiled renderer (render/nixbox NiXBoxRenderer on the D3D11 backend of its D3D8 stream), the controller on
// port 0 (or the keyboard: Window's KeyboardGamepad).
//
//   fut_game <iso-or-folder> [--size WxH] [--saves <dir>] [--language NAME]
//   fut_game                       (no arguments, e.g. double-clicked: platform/win32/Launcher.h finds the disc)
//
// The release build is a Windows-subsystem program (no console window when double-clicked; started from a terminal
// it writes to that terminal); its errors also go to message boxes.
//
// Saves go to %APPDATA%\FuturamaRecomp\saves (never into the game folder; --saves <dir> another directory;
// game/app/SaveStorage.h: the save games' directories with name.txt, Futurama.dat / Unlock.dat / Options.dat and the
// save images: the in-game save, the options save, the load flow). The clock is the real QueryPerformanceCounter,
// read once per App frame.
//
// Sound: the App's sound system App+0xF0 is the console's (game/sound/GameSound.h XboxSound: the ported sound layer
// over the XAudio2 backend's hardware path VP -> GP -> EP, with default.xbe's tables and xbox/dsstdfx.bin; without an
// audio device the same path renders offline and is dropped, so the game logic still sees voices end), with the
// deterministic step-based voice status (XAudio2SoundBackend::Options::stepStatus; docs/analysis/level_sound_port.md
// 3.4). The level's sounds play (scripts, characters, emitters, explosions, dialogue speech); see KNOWN_ISSUES for the
// sites not restored yet.
//
// Drawn: the Intro's splash screens (legal, NI), the front end's loading screen, main menu scene and its options /
// load / extras panels (the extras menus with the music list's render target), the levels with their loading screens
// and titles, the pause menu with its options and load panels, the breakdown's scene, the yes / no dialog, the Death
// screen, the movies (game/app/MoviePlayer.h on platform/common/MovieService.h: the Intro's fox / vug / uds / intro,
// Attract, the level cutscenes, the extras' clips; YUY2 through the D3D11 backend's YuvEnable stand-in, the ADPCM audio
// through the backend's streams).
//
#include "core/XbeImage.h"
#include "core/fs/Vfs.h"
#include "game/app/App.h"
#include "game/app/MemoryCards.h"
#include "game/app/SaveStorage.h"
#include "game/sound/GameSound.h"
#include "game/text/TextObject.h"
#include "platform/win32/GamepadInput.h"
#include "platform/win32/Launcher.h"
#include "platform/common/MovieService.h"
#include "platform/win32/XAudio2SoundBackend.h"
#include "platform/win32/Window.h"
#include "render/nixbox/NiXBoxRenderer.h"
#include "render/xd3d/D3D11Device.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <exception>
#include <memory>
#include <string>
#include <vector>

using namespace fut;

namespace {

std::filesystem::path SaveFolder() {
    wchar_t* appData = nullptr;
    size_t n = 0;
    if (_wdupenv_s(&appData, &n, L"APPDATA") == 0 && appData) {
        std::filesystem::path p = std::filesystem::path(appData) / L"FuturamaRecomp" / L"saves";
        std::free(appData);
        return p;
    }
    return std::filesystem::path(L"saves");
}

// The launcher's test of a disc it found (main's own checks below, without loading the image): opens, has a
// default.xbe, and that is the NTSC-U build.
bool CheckDisc(const std::string& source, std::string& error) {
    fs::Vfs vfs;
    if (!vfs.Open(source, error)) return false;
    std::vector<u8> data;
    if (!vfs.ReadFile("default.xbe", data)) {
        error = "default.xbe not found (the port needs the Futurama (Xbox, NTSC-U) disc image or its extracted folder)";
        return false;
    }
    xbe::XbeImage probe;
    return probe.Load(std::move(data), &error);
}

}  // namespace

int main(int argc, char** argv) {
    const bool boxes = platform::SetUpConsole();      // (the release build: errors in message boxes too)
    auto fail = [&](const std::string& text, int rc) {
        std::fprintf(stderr, "%s\n", text.c_str());
        if (boxes) platform::ErrorBox(text);
        return rc;
    };
    try {
    std::string source = argc >= 2 ? argv[1] : platform::FindDisc(CheckDisc);   // the disc image / folder
    if (argc < 2 && source.empty()) {                 // (no argument and the dialog cancelled)
        std::fprintf(stderr, "usage: fut_game <iso-or-folder> [--size WxH] [--saves <dir>] [--language NAME]");
        std::fputs("\n", stderr);
        return 2;
    }
    u32 width = 1280, height = 960;
    std::filesystem::path saveDir = SaveFolder();     // (--saves <dir>)
    const char* language = "English";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &width, &height);
        else if (a == "--saves" && i + 1 < argc) saveDir = argv[++i];
        else if (a == "--language" && i + 1 < argc) {
            language = text::TextManager::ParseLanguage(argv[++i]);
            if (!language) return fail("unsupported language", 2);
        }
        else {
            return fail(std::string("unknown option (or its value missing): ") + argv[i], 2);
        }
    }
    std::string error;
    fs::Vfs vfs;
    if (!vfs.Open(source, error)) return fail(source + ": " + error, 1);
    {   // the original's data tables (core/XbeImage.h): the user's default.xbe, NTSC-U only
        std::vector<u8> data;
        if (!vfs.ReadFile("default.xbe", data))
            return fail(source + ": default.xbe not found (the port needs the Futurama (Xbox, NTSC-U) disc image or "
                                 "its extracted folder)", 1);
        if (!xbe::LoadGameImage(std::move(data), &error)) return fail(source + ": " + error, 1);
    }
    const std::vector<u8>& xbeFile = xbe::GameImage().Bytes();
    platform::Window window;
    if (!window.Create("Futurama", width, height, true)) return fail("window creation failed", 1);
    render::xd3d::D3D11Device device;                 // the decompiled renderer's D3D8 stream, executed
    if (!device.Init(window.Handle(), error)) return fail(error, 1);
    render::nixbox::NiXBoxRenderer renderer(device);  // the factory 0x0014C200 (App+0x104)
    platform::XInputPads xinput;
    input::GamepadState pad0{};
    game::app::HostSaveStorage saves(saveDir);        // (one directory per save game, as the U:\ model)
    saves.readDisc = [&](const std::string& path, std::vector<u8>& out) { return vfs.ReadFile(path, out); };
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    // orig: App::Init's XboxSoundSystem 0x000CCD90 (DirectSoundCreate, the effects image): the backend first, its
    // hardware data before the sound system's constructor downloads the effects image.
    platform::XAudio2SoundBackend::Options audioOptions;
    audioOptions.stepStatus = true;
    platform::XAudio2SoundBackend soundBackend(audioOptions);
    bool soundOffline = false;
    if (!soundOffline && !soundBackend.Init()) {
        std::fprintf(stderr, "audio output unavailable: the sound layer runs offline (silent)\n");
        soundOffline = true;
    }
    if (soundOffline) soundBackend.InitOffline();
    std::vector<s32> soundDrop;
    auto finish = [&](int rc) {
        return rc;
    };
    {
        std::vector<u8> data;
        if (vfs.ReadFile("xbox/dsstdfx.bin", data)) soundBackend.SetEffectsImage(data);
        soundBackend.LoadXbeTables(xbeFile);
    }
    // the movie player's platform: the renderer's device, the disc's Video\*.xmv, the backend's streams, the XMV
    // library's tables from default.xbe
    platform::MovieService movies(vfs, &soundBackend, xbeFile);
    sound::XboxSound sound(&soundBackend, [&](const std::string& path, std::vector<u8>& out) {
        return vfs.ReadFile(path, out);
    });
    game::app::AppServices sv;
    sv.language = language;
    sv.readFile = [&](const std::string& path, std::vector<u8>& out) { return vfs.ReadFile(path, out); };
    sv.streamFile = sv.readFile;                      // (the sound-request loader's NiUDSStream job: synchronous, L)
    sv.sound = &sound;
    sv.movies = &movies;
    sv.readPad = [&](u32 port) -> const input::GamepadState* {
        if (port != 0) return xinput.State(port);
        pad0 = xinput.State(0) ? *xinput.State(0) : platform::KeyboardGamepad(window);
        return &pad0;
    };
    sv.deviceChanges = [&](u32& ins, u32& rem) { return xinput.TakeChanges(ins, rem); };   // (XGetDeviceChanges)
    sv.renderer = &renderer;
    sv.saves = &saves;
    sv.qpf = static_cast<u64>(freq.QuadPart);
    text::CreateAppFont(sv.readFile);                 // App::Init's error overlay: font 0 (tex2)
    game::app::App app(sv);
    app.ntsc = !vfs.IsPal(); // Select PAL movie names and splash assets before Init.
    QueryPerformanceCounter(&now);
    const u64 base = static_cast<u64>(now.QuadPart);
    app.qpc = 0;                                      // (the clock counts from the start: QPC - base)
    app.input.onRumble = [&](const input::Pad& p) { xinput.SetMotors(p.Port(), p.motorLeft, p.motorRight); };
    const char* startLevel = nullptr;                 // (null = the boot)
    if (!app.Init(startLevel)) return fail("App::Init failed", 1);
    for (s32 f = 0; window.Pump(); ++f) {
        xinput.Poll();                                // (Input_Update reads the pads through readPad every step)
        QueryPerformanceCounter(&now);
        app.frame = f;
        app.qpc = static_cast<u64>(now.QuadPart) - base;
        if (!app.Frame()) break;                      // (the App quits: no context requested)
        if (soundOffline) {                           // (offline: the published state rendered and dropped)
            soundDrop.clear();
            soundBackend.RenderFrames(25 * app.lastSteps, soundDrop);   // 800 samples per 1/60 s step
        }
    }
    return finish(0);
    } catch (const std::exception& error) {
        return fail(std::string("native host: ") + error.what(), 1);
    }
}

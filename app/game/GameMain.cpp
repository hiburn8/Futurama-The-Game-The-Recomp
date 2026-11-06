// fut_game: the port booted like the original (main 0x000D2A40): the App (src/game/app: the Intro, the front end, the
// Ingame context with the pause menu, the game-over screen) on the user's own disc image / extracted folder, drawn by
// the decompiled renderer (render/nixbox NiXBoxRenderer on the D3D11 backend of its D3D8 stream), the controller on
// port 0 (or the keyboard: Window's KeyboardGamepad).
//
//   fut_game <iso-or-folder> [--size WxH] [--windowed | --fullscreen] [--render-scale auto|<factor>]
//            [--filter linear|nearest] [--aspect auto|4:3|16:9|16:10|21:9|<w>:<h>] [--saves <dir>]
//   fut_game                       (no arguments, e.g. double-clicked: platform/sdl/Launcher.h finds the disc)
//
// The picture: the game renders the Xbox's 640 x 480 frame, which the device draws into the largest 4:3 rectangle of
// the window, centred, black either side (render/xd3d/D3D11Device.h: Swap). The render scale (default auto) draws
// that frame at the rectangle's own resolution (a factor: at factor x 640 x 480, scaled to the rectangle with a
// bilinear filter, or --filter nearest for unfiltered pixels; 1 is the Xbox's picture); the game itself is unchanged
// (docs/analysis/render_scale_plan.md). The window starts borderless full screen on its monitor; Alt+Enter toggles
// a window of the windowed size (--size WxH, default 1280x960; --size or --windowed start windowed;
// platform/sdl/Window.h: SDL3 for the window, the keyboard and the controllers).
//
// --aspect (default 4:3, the original's picture; docs/analysis/widescreen_plan.md): a wider canvas, round(480 x
// aspect) x 480 (auto: the window's own shape, so a 16:9 screen is filled without bars), with more world at the
// sides (Hor+), the HUD at the edges, movies and the 4:3 pictures centred with black beside them
// (engine/ni/ScreenCanvas.h). Opt-in: the levels were built for 4:3 and may show their edges.
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
// deterministic step-based voice status (SdlSoundBackend::Options::stepStatus; docs/analysis/level_sound_port.md
// 3.4). The level's sounds play (scripts, characters, emitters, explosions, dialogue speech); see KNOWN_ISSUES for the
// sites not restored yet.
//
// Drawn: the Intro's splash screens (legal, NI), the front end's loading screen, main menu scene and its options /
// load / extras panels (the extras menus with the music list's render target), the levels with their loading screens
// and titles, the pause menu with its options and load panels, the breakdown's scene, the yes / no dialog, the Death
// screen, the movies (game/app/MoviePlayer.h on platform/sdl/MovieService.h: the Intro's fox / vug / uds / intro,
// Attract, the level cutscenes, the extras' clips; YUY2 through the D3D11 backend's YuvEnable stand-in, the ADPCM audio
// through the backend's streams).
//
#include "core/XbeImage.h"
#include "core/fs/Vfs.h"
#include "engine/ni/ScreenCanvas.h"
#include "game/app/App.h"
#include "game/app/MemoryCards.h"
#include "game/app/SaveStorage.h"
#include "game/sound/GameSound.h"
#include "game/text/TextObject.h"
#include "platform/sdl/GamepadInput.h"
#include "platform/sdl/Launcher.h"
#include "platform/sdl/MovieService.h"
#include "platform/sdl/SdlSoundBackend.h"
#include "platform/sdl/Window.h"
#include "render/nixbox/NiXBoxRenderer.h"
#include "render/xd3d/D3D11Device.h"


#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace fut;

namespace {

std::filesystem::path SaveFolder() { return platform::PrefFolder() / "saves"; }   // (%APPDATA%\FuturamaRecomp\saves)

// The launcher's test of a disc it found (main's own checks below, without loading the image): opens, has a
// default.xbe, and that is the NTSC-U or the PAL build.
bool CheckDisc(const std::string& source, std::string& error) {
    fs::Vfs vfs;
    if (!vfs.Open(source, error)) return false;
    std::vector<u8> data;
    if (!vfs.ReadFile("default.xbe", data)) {
        error = "default.xbe not found (the port needs a Futurama (Xbox, NTSC-U or PAL) disc image or its extracted "
                "folder)";
        return false;
    }
    xbe::XbeImage probe;
    return probe.Load(std::move(data), &error);
}

}  // namespace

int main(int argc, char** argv) {
    platform::Window::SetDpiAware();                  // (before the launcher's dialog and the window)
    const bool boxes = platform::SetUpConsole();      // (the release build: errors in message boxes too)
    auto fail = [&](const std::string& text, int rc) {
        std::fprintf(stderr, "%s\n", text.c_str());
        if (boxes) platform::ErrorBox(text);
        return rc;
    };
    std::string source = argc >= 2 ? argv[1] : platform::FindDisc(CheckDisc);   // the disc image / folder
    if (argc < 2 && source.empty()) {                 // (no argument and the dialog cancelled)
        std::fprintf(stderr, "usage: fut_game <iso-or-folder> [--size WxH] [--windowed | --fullscreen]"
                             " [--render-scale auto|<factor>] [--filter linear|nearest]"
                             " [--aspect auto|4:3|16:9|16:10|21:9|<w>:<h>] [--saves <dir>]");
        std::fputs("\n", stderr);
        return 2;
    }
    u32 width = 1280, height = 960;                   // the windowed size (--size WxH)
    bool fullscreen = true;                           // (--size / --windowed: a window; --fullscreen: full screen)
    render::xd3d::D3D11Device::Filter filter = render::xd3d::D3D11Device::Filter::Linear;   // (--filter)
    double renderScale = 0;                           // (--render-scale: 0 = auto)
    double aspect = 4.0 / 3.0;                        // (--aspect: the canvas's shape; 0 = auto, the window's)
    std::filesystem::path saveDir = SaveFolder();     // (--saves <dir>)
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &width, &height), fullscreen = false;
        else if (a == "--windowed") fullscreen = false;
        else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--render-scale" && i + 1 < argc) {
            const std::string s = argv[++i];
            if (s == "auto") renderScale = 0;
            else if (renderScale = std::atof(s.c_str()); !(renderScale >= 0.25 && renderScale <= 16))
                return fail("--render-scale takes auto or a factor from 0.25 to 16", 2);
        } else if (a == "--filter" && i + 1 < argc) {
            const std::string f = argv[++i];
            if (f == "nearest") filter = render::xd3d::D3D11Device::Filter::Nearest;
            else if (f == "linear") filter = render::xd3d::D3D11Device::Filter::Linear;
            else return fail("--filter takes linear or nearest", 2);
        } else if (a == "--aspect" && i + 1 < argc) {
            const std::string s = argv[++i];
            unsigned w = 0, h = 0;
            if (s == "auto") aspect = 0;
            else if (std::sscanf(s.c_str(), "%u:%u", &w, &h) == 2 && w > 0 && h > 0) aspect = double(w) / h;
            else return fail("--aspect takes auto, 4:3, 16:9, 16:10, 21:9 or <w>:<h>", 2);
        } else if (a == "--saves" && i + 1 < argc) saveDir = argv[++i];
        else {
            return fail(std::string("unknown option (or its value missing): ") + argv[i], 2);
        }
    }
    std::string error;
    fs::Vfs vfs;
    if (!vfs.Open(source, error)) return fail(source + ": " + error, 1);
    {   // the original's data tables (core/XbeImage.h): the user's default.xbe, NTSC-U or PAL
        std::vector<u8> data;
        if (!vfs.ReadFile("default.xbe", data))
            return fail(source + ": default.xbe not found (the port needs a Futurama (Xbox, NTSC-U or PAL) disc image "
                                 "or its extracted folder)", 1);
        if (!xbe::LoadGameImage(std::move(data), &error)) return fail(source + ": " + error, 1);
    }
    const std::vector<u8>& xbeFile = xbe::GameImage().Bytes();
    platform::Window window;
    if (!window.Create("Futurama", width, height, false, fullscreen)) return fail("window creation failed", 1);
#ifndef _WIN32
    setenv("DXVK_WSI_DRIVER", "SDL3", 1);             // (DXVK-native: its window system is SDL3's; read at device creation)
#endif
    render::xd3d::D3D11Device device;                 // the decompiled renderer's D3D8 stream, executed
    {   // the canvas (engine/ni/ScreenCanvas.h): 640 x 480, or round(480 x aspect) rounded to even, 4:3 .. 32:9
        if (aspect <= 0) aspect = double(window.Width()) / std::max(window.Height(), 1u);
        aspect = std::clamp(aspect, 4.0 / 3.0, 32.0 / 9.0);
        const u32 w = u32(std::lround(480.0 * aspect / 2.0) * 2);
        ni::ScreenCanvas::width = w;
        device.SetCanvasSize(w, ni::ScreenCanvas::kHeight);   // (before Init: the frame buffers are made at the size)
    }
    device.SetRenderScale(renderScale);               // (before Init: the targets are made at the scale)
    if (!device.Init(window.NativeHandle(), window.Width(), window.Height(), error)) return fail(error, 1);
    device.SetFilter(filter);
    render::nixbox::NiXBoxRenderer renderer(device);  // the factory 0x0014C200 (App+0x104)
    platform::Gamepads gamepads;
    input::GamepadState pad0{};
    game::app::HostSaveStorage saves(saveDir);        // (one directory per save game, as the U:\ model)
    saves.readDisc = [&](const std::string& path, std::vector<u8>& out) { return vfs.ReadFile(path, out); };
    // orig: App::Init's XboxSoundSystem 0x000CCD90 (DirectSoundCreate, the effects image): the backend first, its
    // hardware data before the sound system's constructor downloads the effects image.
    platform::SdlSoundBackend::Options audioOptions;
    audioOptions.stepStatus = true;
    platform::SdlSoundBackend soundBackend(audioOptions);
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
    sv.readFile = [&](const std::string& path, std::vector<u8>& out) { return vfs.ReadFile(path, out); };
    sv.streamFile = sv.readFile;                      // (the sound-request loader's NiUDSStream job: synchronous, L)
    sv.sound = &sound;
    sv.movies = &movies;
    sv.readPad = [&](u32 port) -> const input::GamepadState* {
        if (port != 0) return gamepads.State(port);
        pad0 = gamepads.State(0) ? *gamepads.State(0) : platform::KeyboardGamepad(window);
        return &pad0;
    };
    sv.deviceChanges = [&](u32& ins, u32& rem) { return gamepads.TakeChanges(ins, rem); };   // (XGetDeviceChanges)
    sv.renderer = &renderer;
    sv.saves = &saves;
    sv.qpf = platform::Window::Frequency();         // (QueryPerformanceFrequency)
    sv.pal = vfs.IsPal();                             // (the disc's archives: sysPAL.img / FutPAL##.img)
    // (XGetLanguage: unset = English, the default; the PAL game's language choice comes later)
    text::CreateAppFont(sv.readFile);                 // App::Init's error overlay: font 0 (tex2)
    game::app::App app(sv);
    const u64 base = platform::Window::Counter();
    app.qpc = 0;                                      // (the clock counts from the start: QPC - base)
    app.input.onRumble = [&](const input::Pad& p) { gamepads.SetMotors(p.Port(), p.motorLeft, p.motorRight); };
    const char* startLevel = nullptr;                 // (null = the boot)
    if (!app.Init(startLevel)) return fail("App::Init failed", 1);
    for (s32 f = 0; window.Pump(); ++f) {
        if (window.Resized()) device.Resize(window.Width(), window.Height());   // (the swap chain follows the window)
        gamepads.Poll();                              // (Input_Update reads the pads through readPad every step)
        app.frame = f;
        app.qpc = platform::Window::Counter() - base;
        if (!app.Frame()) break;                      // (the App quits: no context requested)
        if (f == 0) window.Show();                    // (the first frame is presented: no empty window before it)
        if (soundOffline) {                           // (offline: the published state rendered and dropped)
            soundDrop.clear();
            soundBackend.RenderFrames(25 * app.lastSteps, soundDrop);   // 800 samples per 1/60 s step
        }
    }
    return finish(0);
}

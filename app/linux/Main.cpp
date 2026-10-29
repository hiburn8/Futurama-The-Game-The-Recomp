// Native Linux bring-up host. No launcher, Wine runtime or game data is embedded.
#include <SDL3/SDL.h>
#include "PreviewSaves.h"
#include "core/XbeImage.h"
#include "platform/common/MovieService.h"
#include "platform/common/GameSoundBackend.h"
#include "core/fs/Vfs.h"
#include "game/app/App.h"
#include "game/app/SaveStorage.h"
#include "game/text/TextObject.h"
#include "game/text/LanguageDb.h"
#include "render/nixbox/NiXBoxRenderer.h"
#include "render/xd3d/D3D11Device.h"
#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t stopping = 0;
void Stop(int) { stopping = 1; }

fut::input::GamepadState ReadPad(SDL_Gamepad* gamepad) {
    fut::input::GamepadState pad;
    if (!gamepad) return pad;
    auto button = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(gamepad, b); };
    const SDL_GamepadButton digital[] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    for (unsigned i = 0; i < 8; ++i) if (button(digital[i])) pad.buttons |= 1u << i;
    const SDL_GamepadButton analog[] = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    for (unsigned i = 0; i < 6; ++i) pad.analog[i] = button(analog[i]) ? 255 : 0;
    pad.analog[6] = std::max(0, int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER))) * 255 / 32767;
    pad.analog[7] = std::max(0, int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))) * 255 / 32767;
    pad.thumbLX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    pad.thumbRX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    pad.thumbLY = std::clamp(-int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY)), -32768, 32767);
    pad.thumbRY = std::clamp(-int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY)), -32768, 32767);
    return pad;
}

int Run(int argc, char** argv) {
    if (argc < 6 || std::string(argv[2]) != "--shaders" || (std::string(argv[4]) != "--saves" && std::string(argv[4]) != "--resume-saves")) {
        std::fprintf(stderr, "usage: FuturamaRecomp DATA --shaders DIR (--saves NEW_DIR | --resume-saves PREVIEW_DIR) [--frames N] [--language NAME]\n");
        return 2;
    }
    int frames = 0;
    const char* language = "English";
    bool languageSpecified = false;
    for (int i = 6; i < argc; i += 2) {
        if (i + 1 == argc) { std::fprintf(stderr, "missing option value\n"); return 2; }
        const std::string option(argv[i]), value(argv[i + 1]);
        if (option == "--frames" && frames == 0) {
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), frames);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || frames <= 0) return 2;
        } else if (option == "--language" && !languageSpecified) {
            language = fut::text::TextManager::ParseLanguage(value);
            if (!language) { std::fprintf(stderr, "unsupported language: %s\n", value.c_str()); return 2; }
            languageSpecified = true;
        } else { std::fprintf(stderr, "unknown or repeated option: %s\n", option.c_str()); return 2; }
    }
    std::string error;
    fut::fs::Vfs vfs;
    if (!vfs.Open(argv[1], error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    std::vector<fut::u8> xbe;
    if (!vfs.ReadFile("default.xbe", xbe) || !fut::xbe::LoadGameImage(std::move(xbe), &error)) {
        std::fprintf(stderr, "disc validation failed: %s\n", error.c_str()); return 1;
    }
    // DXVK reads libc environment, not SDL's separate environment snapshot.
    if (::setenv("DXVK_WSI_DRIVER", "SDL3", 1) != 0) {
        std::perror("DXVK_WSI_DRIVER");
        return 1;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(
        SDL_CreateWindow("Futurama native preview", 1280, 960, SDL_WINDOW_VULKAN), SDL_DestroyWindow);
    if (!window) { std::fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    fut::render::xd3d::D3D11Device device;
    if (!device.Init(reinterpret_cast<HWND__*>(window.get()), error, argv[3])) {
        std::fprintf(stderr, "%s\n", error.c_str()); return 1;
    }
    fut::render::nixbox::NiXBoxRenderer renderer(device);
    PreviewSaves saveSession(argv[5], std::string(argv[4]) == "--resume-saves");
    fut::game::app::HostSaveStorage saves(argv[5]);
    saves.readDisc = [&](const std::string& name, std::vector<fut::u8>& out) { return vfs.ReadFile(name, out); };
    auto gamepad = std::unique_ptr<SDL_Gamepad, decltype(&SDL_CloseGamepad)>(nullptr, SDL_CloseGamepad);
    fut::input::GamepadState pad;
    fut::game::app::AppServices services;
    services.language = language;
    services.readFile = saves.readDisc;
    services.streamFile = services.readFile;
    services.readPad = [&](fut::u32 port) { return port == 0 && gamepad ? &pad : nullptr; };
    services.renderer = &renderer;
    services.saves = &saves;
    fut::platform::GameSoundBackend::Options audioOptions;
    audioOptions.stepStatus = true;
    fut::platform::GameSoundBackend soundBackend(audioOptions);
    std::vector<fut::u8> effects;
    if (!vfs.ReadFile("xbox/dsstdfx.bin", effects)) {
        std::fprintf(stderr, "audio effects image missing\n"); return 1;
    }
    soundBackend.LoadXbeTables(fut::xbe::GameImage().Bytes());
    soundBackend.SetEffectsImage(effects);
    if (!soundBackend.Init()) { std::fprintf(stderr, "native audio initialization failed\n"); return 1; }
    fut::platform::MovieService movies(vfs, &soundBackend, fut::xbe::GameImage().Bytes());
    fut::sound::XboxSound sound(&soundBackend, services.readFile);
    services.sound = &sound;
    services.movies = &movies;
    services.qpf = SDL_GetPerformanceFrequency();
    fut::text::CreateAppFont(services.readFile);
    fut::game::app::App app(services);
    const auto base = SDL_GetPerformanceCounter();
    app.input.onRumble = [&](const fut::input::Pad& p) {
        if (gamepad && p.Port() == 0) SDL_RumbleGamepad(gamepad.get(), p.motorLeft, p.motorRight, 100);
    };
    if (!app.Init(nullptr)) { std::fprintf(stderr, "App::Init failed\n"); return 1; }
    std::fprintf(stderr, "PREVIEW: native audio/movie services connected; use isolated test saves only\n");
    for (int frame = 0; !stopping && (!frames || frame < frames); ++frame) {
        if (!soundBackend.Healthy()) throw std::runtime_error("native audio pipeline failed");
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) stopping = 1;
        }
        if (gamepad && !SDL_GamepadConnected(gamepad.get())) gamepad.reset();
        if (!gamepad) {
            int count = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&count);
            if (ids && count > 0) gamepad.reset(SDL_OpenGamepad(ids[0]));
            SDL_free(ids);
        }
        pad = ReadPad(gamepad.get());
        app.frame = frame;
        app.qpc = SDL_GetPerformanceCounter() - base;
        if (!app.Frame()) break;
    }
    std::fprintf(stderr, "native preview exited; draw calls: %u\n", device.draws());
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGTERM, Stop);
    std::signal(SIGINT, Stop);
    try {
        const int result = Run(argc, argv);
        SDL_Quit();
        return result;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "native host: %s\n", error.what());
        SDL_Quit();
        return 1;
    }
}

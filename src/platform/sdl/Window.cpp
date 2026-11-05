// The SDL3 window; see Window.h.
#include "platform/sdl/Window.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <cstdio>

namespace fut::platform {
namespace {
bool g_sdlReady = false;
}  // namespace

bool Window::InitSdl() {
    if (g_sdlReady) return true;
#ifdef _WIN32
    // Xbox-class controllers through XInput only (not GameInput / Windows.Gaming.Input / raw input), so the values the
    // game sees are XInputGetState's own (GamepadInput.cpp maps them back exactly; the recordings stay comparable).
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    g_sdlReady = true;
    return true;
}

Window::~Window() {
    if (window_) SDL_DestroyWindow(window_);
}

void Window::SetDpiAware() {
#ifdef _WIN32
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // (fails harmlessly when already set)
#endif
}

bool Window::Create(const std::string& title, u32 width, u32 height, bool visible, bool fullscreen) {
    if (!InitSdl()) return false;
    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (!visible) flags |= SDL_WINDOW_HIDDEN;
    window_ = SDL_CreateWindow(title.c_str(), int(width), int(height), flags);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    width_ = u32(w), height_ = u32(h);
    if (fullscreen) SetFullscreen(true);
    return true;
}

void Window::Show() {
    if (window_) SDL_ShowWindow(window_);
}

void Window::SetFullscreen(bool on) {
    if (!window_ || on == fullscreen_) return;
    SDL_SetWindowFullscreenMode(window_, nullptr);     // (the borderless desktop mode, never an exclusive one)
    if (!SDL_SetWindowFullscreen(window_, on)) return;
    fullscreen_ = on;
    if (on) SDL_HideCursor();                          // (no mouse pointer over the full-screen game)
    else SDL_ShowCursor();
}

void Window::SetTitle(const std::string& title) {
    if (window_) SDL_SetWindowTitle(window_, title.c_str());
}

HWND__* Window::NativeHandle() const {
#ifdef _WIN32
    if (!window_) return nullptr;
    return static_cast<HWND__*>(SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                                       SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
#else
    return reinterpret_cast<HWND__*>(window_);         // (DXVK-native: the SDL_Window* stands in for the HWND)
#endif
}

bool Window::Pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            closed_ = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (e.window.data1 > 0 && e.window.data2 > 0 &&
                (u32(e.window.data1) != width_ || u32(e.window.data2) != height_)) {
                width_ = u32(e.window.data1), height_ = u32(e.window.data2);
                resized_ = true;
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) {   // Alt+Enter: the full-screen toggle
                if (!e.key.repeat) SetFullscreen(!fullscreen_);
                break;
            }
            if (!e.key.repeat && e.key.scancode < 512) pressed_[e.key.scancode] = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.button == SDL_BUTTON_RIGHT) looking_ = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button == SDL_BUTTON_RIGHT) looking_ = false;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (looking_) dx_ += s32(e.motion.xrel), dy_ += s32(e.motion.yrel);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            looking_ = false;
            break;
        default:
            break;
        }
    }
    return !closed_;
}

bool Window::KeyDown(int scancode) const {
    if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT) return false;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    return keys && keys[scancode];
}

bool Window::KeyPressed(int scancode) {
    if (scancode < 0 || scancode >= 512 || !pressed_[scancode]) return false;
    pressed_[scancode] = false;
    return true;
}

void Window::TakeMouseDelta(s32& dx, s32& dy) {
    dx = dx_;
    dy = dy_;
    dx_ = dy_ = 0;
}

u64 Window::Counter() { return SDL_GetPerformanceCounter(); }
u64 Window::Frequency() { return SDL_GetPerformanceFrequency(); }

}  // namespace fut::platform

// The window, its event pump, the keyboard and mouse state and the clock, on SDL3 (docs/analysis/sdl3_plan.md;
// replaced the Win32 window of src/platform/win32 on 2026-11-05). The same calls as before: Create / Pump / Width /
// Height / Resized / KeyDown / KeyPressed / TakeMouseDelta / SetFullscreen; keys are SDL scancodes (the physical
// keys, so WASD is WASD on any layout). Full screen is SDL's borderless desktop mode (never an exclusive display
// mode), toggled by Alt+Enter; the picture's fit into the client area is the device's (render/xd3d/D3D11Device.h).
// Sizes are pixels: the process is per-monitor DPI aware (SDL declares it; SetDpiAware does it earlier for the
// launcher's dialog), so a 1280 x 960 window is 1280 x 960 pixels on a scaled screen.
#pragma once

#include "core/Types.h"

#include <string>

struct SDL_Window;
struct HWND__;

namespace fut::platform {

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // A window with a client area of width x height pixels (the windowed size; `fullscreen`: full screen instead).
    // `visible` false: created hidden, shown by Show() (the game shows it after its first frame is presented, so no
    // empty window appears while the disc loads). Initialises SDL's video and gamepad subsystems.
    bool Create(const std::string& title, u32 width, u32 height, bool visible, bool fullscreen = false);
    void Show();
    // Borderless full screen on the window's display, or back to the windowed size and place (Alt+Enter does this;
    // Resized() reports the change).
    void SetFullscreen(bool on);
    bool Fullscreen() const { return fullscreen_; }
    void SetTitle(const std::string& title);
    // Per-monitor DPI awareness for the process: call before any window (the launcher's dialog too), so the client
    // area is in physical pixels and Windows never scales the picture itself.
    static void SetDpiAware();
    // SDL's video and gamepad subsystems, once (Create does it; the launcher's dialog needs it before any window).
    static bool InitSdl();
    // Handles pending events; false once the window was closed.
    bool Pump();
    SDL_Window* Handle() const { return window_; }
    // The swap chain's window: the HWND on Windows; elsewhere the SDL_Window* itself, which DXVK-native takes in
    // the HWND's place (its SDL3 WSI).
    HWND__* NativeHandle() const;
    u32 Width() const { return width_; }
    u32 Height() const { return height_; }
    bool Resized() { const bool r = resized_; resized_ = false; return r; }

    bool KeyDown(int scancode) const;                 // (SDL_Scancode)
    bool KeyPressed(int scancode);                    // went down since the last call for this key
    // Mouse movement while the right button is held (camera look), reset by each call.
    void TakeMouseDelta(s32& dx, s32& dy);

    // The clock the game runs on: SDL's performance counter (QueryPerformanceCounter on Windows).
    static u64 Counter();
    static u64 Frequency();

private:
    SDL_Window* window_ = nullptr;
    u32 width_ = 0, height_ = 0;
    bool resized_ = false, closed_ = false;
    bool fullscreen_ = false;
    bool pressed_[512] = {};
    bool looking_ = false;
    s32 dx_ = 0, dy_ = 0;
};

}  // namespace fut::platform

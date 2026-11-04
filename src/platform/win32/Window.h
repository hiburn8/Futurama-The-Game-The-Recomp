// Win32 window + message pump + keyboard / mouse state (Phase 3 platform layer; replaces the Xbox's fixed video
// output). Input for the game proper goes through XInput / Raw Input later; this is enough for the tools.
// Full screen is the borderless kind (a popup window the size of its monitor, never DXGI's exclusive mode), toggled
// by Alt+Enter; the picture's fit into the client area is the device's (render/xd3d/D3D11Device.h: Swap).
#pragma once

#include "core/Types.h"

#include <string>

struct HWND__;

namespace fut::platform {

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // A window with a client area of width x height (the windowed size; `fullscreen`: shown full screen instead).
    bool Create(const std::string& title, u32 width, u32 height, bool visible, bool fullscreen = false);
    // Borderless full screen on the window's monitor, or back to the windowed size and place (Alt+Enter does this;
    // Resized() reports the change).
    void SetFullscreen(bool on);
    bool Fullscreen() const { return fullscreen_; }
    // Per-monitor DPI awareness for the process: call before any window (the launcher's dialog too), so the client
    // area is in physical pixels and Windows never scales the picture itself.
    static void SetDpiAware();
    // Handles pending messages; false once the window was closed.
    bool Pump();
    HWND__* Handle() const { return hwnd_; }
    u32 Width() const { return width_; }
    u32 Height() const { return height_; }
    bool Resized() { const bool r = resized_; resized_ = false; return r; }

    bool KeyDown(int virtualKey) const { return virtualKey >= 0 && virtualKey < 256 && keys_[virtualKey]; }
    bool KeyPressed(int virtualKey);                   // went down since the last call for this key
    // Mouse movement while the right button is held (camera look), reset by each call.
    void TakeMouseDelta(s32& dx, s32& dy);

private:
    static long long __stdcall WndProc(HWND__* hwnd, u32 msg, u64 wParam, s64 lParam);
    long long Handle(u32 msg, u64 wParam, s64 lParam);

    HWND__* hwnd_ = nullptr;
    u32 width_ = 0, height_ = 0;
    bool resized_ = false, closed_ = false;
    bool fullscreen_ = false;
    s32 windowed_[4] = {};                            // the windowed rectangle (left, top, right, bottom) while full screen
    bool keys_[256] = {};
    bool pressed_[256] = {};
    bool looking_ = false;
    s32 lastX_ = 0, lastY_ = 0, dx_ = 0, dy_ = 0;
};

}  // namespace fut::platform

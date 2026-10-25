// Win32 window; see Window.h.
#include "platform/win32/Window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

namespace fut::platform {
namespace {
constexpr wchar_t kClassName[] = L"FuturamaPCWindow";

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
}  // namespace

Window::~Window() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool Window::Create(const std::string& title, u32 width, u32 height, bool visible) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&Window::WndProc);
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))   /* IDC_ARROW */;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);                             // fails harmlessly when already registered

    RECT r = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    hwnd_ = CreateWindowExW(0, kClassName, Widen(title).c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                            r.bottom - r.top, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    width_ = width;
    height_ = height;
    if (visible) ShowWindow(hwnd_, SW_SHOW);
    return true;
}

bool Window::Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

bool Window::KeyPressed(int virtualKey) {
    if (virtualKey < 0 || virtualKey >= 256 || !pressed_[virtualKey]) return false;
    pressed_[virtualKey] = false;
    return true;
}

void Window::TakeMouseDelta(s32& dx, s32& dy) {
    dx = dx_;
    dy = dy_;
    dx_ = dy_ = 0;
}

long long __stdcall Window::WndProc(HWND__* hwnd, u32 msg, u64 wParam, s64 lParam) {
    Window* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self && self->hwnd_ == hwnd) return self->Handle(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

long long Window::Handle(u32 msg, u64 wParam, s64 lParam) {
    switch (msg) {
    case WM_CLOSE:
        closed_ = true;
        return 0;
    case WM_SIZE: {
        const u32 w = LOWORD(lParam), h = HIWORD(lParam);
        if (w && h && (w != width_ || h != height_)) {
            width_ = w;
            height_ = h;
            resized_ = true;
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam < 256) {
            if (!keys_[wParam]) pressed_[wParam] = true;
            keys_[wParam] = true;
        }
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wParam < 256) keys_[wParam] = false;
        break;
    case WM_KILLFOCUS:
        for (bool& k : keys_) k = false;
        looking_ = false;
        ReleaseCapture();
        break;
    case WM_RBUTTONDOWN:
        looking_ = true;
        lastX_ = GET_X_LPARAM(lParam);
        lastY_ = GET_Y_LPARAM(lParam);
        SetCapture(hwnd_);
        return 0;
    case WM_RBUTTONUP:
        looking_ = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (looking_) {
            const s32 x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            dx_ += x - lastX_;
            dy_ += y - lastY_;
            lastX_ = x;
            lastY_ = y;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
}

}  // namespace fut::platform

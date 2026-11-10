#include "GlassBackdrop.h"

#include <Dwmapi.h>

namespace MyDock {
namespace {

constexpr wchar_t kBackdropClassName[] = L"MyDockGlassBackdrop";

// SetWindowCompositionAttribute definitions (user32, Windows 10+). Not in the Windows SDK headers.
enum AccentState : int {
    kAccentDisabled = 0,
    kAccentEnableBlurBehind = 3,
};

constexpr int kAccentFlagUseGradientColor = 2;
constexpr int kWcaAccentPolicy = 19;

struct AccentPolicy {
    int accentState;
    int accentFlags;
    DWORD gradientColor; // AABBGGRR
    int animationId;
};

struct WindowCompositionAttributeData {
    int attribute;
    PVOID data;
    SIZE_T sizeOfData;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

// A whisper of cool tint on the blur itself. Most of the glass tint is painted by the dock so the
// look stays consistent when blur is unavailable.
constexpr DWORD kBlurTint = 0x14261C16; // AABBGGRR: alpha 0x14 over RGB(22, 28, 38)

constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr int kDwmCornerDoNotRound = 1;

} // namespace

GlassBackdrop::GlassBackdrop(HINSTANCE instance)
    : instance_(instance) {}

GlassBackdrop::~GlassBackdrop() {
    Destroy();
}

bool GlassBackdrop::Create() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance_;
    wc.lpfnWndProc = GlassBackdrop::WindowProc;
    wc.lpszClassName = kBackdropClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Black client pixels carry zero alpha in the redirection surface, so the accent blur shows
    // through untouched.
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // Deliberately not owned by the dock: owned windows are kept above their owner, and this one
    // must sit below it.
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kBackdropClassName,
        L"MyDock glass",
        WS_POPUP,
        0,
        0,
        1,
        1,
        nullptr,
        nullptr,
        instance_,
        nullptr);

    if (!hwnd_) {
        return false;
    }

    const int corners = kDwmCornerDoNotRound;
    DwmSetWindowAttribute(hwnd_, kDwmWindowCornerPreference, &corners, sizeof(corners));
    blurEnabled_ = EnableBlur();
    return true;
}

void GlassBackdrop::Destroy() {
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
}

bool GlassBackdrop::EnableBlur() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return false;
    }

    const auto setWindowCompositionAttribute = reinterpret_cast<SetWindowCompositionAttributeFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowCompositionAttribute")));
    if (!setWindowCompositionAttribute) {
        return false;
    }

    AccentPolicy policy = {};
    policy.accentState = kAccentEnableBlurBehind;
    policy.accentFlags = kAccentFlagUseGradientColor;
    policy.gradientColor = kBlurTint;

    WindowCompositionAttributeData data = {};
    data.attribute = kWcaAccentPolicy;
    data.data = &policy;
    data.sizeOfData = sizeof(policy);
    return setWindowCompositionAttribute(hwnd_, &data) != FALSE;
}

void GlassBackdrop::Place(HWND above, const RECT& screenRect, int cornerRadius) {
    if (!hwnd_) {
        return;
    }

    const int width = static_cast<int>(screenRect.right - screenRect.left);
    const int height = static_cast<int>(screenRect.bottom - screenRect.top);
    if (width <= 0 || height <= 0) {
        return;
    }

    if (width != regionSize_.cx || height != regionSize_.cy || cornerRadius != regionRadius_) {
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, cornerRadius * 2, cornerRadius * 2);
        if (region && SetWindowRgn(hwnd_, region, FALSE) == 0) {
            DeleteObject(region); // The system owns the region only on success.
        }
        regionSize_ = {width, height};
        regionRadius_ = cornerRadius;
    }

    SetWindowPos(
        hwnd_,
        above ? above : HWND_TOPMOST,
        screenRect.left,
        screenRect.top,
        width,
        height,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

LRESULT CALLBACK GlassBackdrop::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        // Purely visual. The dock window above handles all input.
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace MyDock

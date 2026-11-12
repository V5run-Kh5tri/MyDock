#include "GlassBackdrop.h"

#include <Dwmapi.h>

#include <algorithm>
#include <cstdint>

namespace MyDock {
namespace {

constexpr wchar_t kBackdropClassName[] = L"MyDockGlassBackdrop";

// SetWindowCompositionAttribute definitions (user32, Windows 10+). Not in the Windows SDK headers.
constexpr int kAccentDisabled = 0;
constexpr int kAccentEnableBlurBehind = 3;
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

// Premultiplied BGRA black at alpha 1: present for the compositor, invisible to the eye.
constexpr std::uint32_t kNearlyClearPixel = 0x01000000;

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
    wc.hbrBackground = nullptr;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    // Layered + transparent: purely visual and always click-through. Deliberately not owned by the
    // dock: owned windows are kept above their owner, and this one must sit below it.
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
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
    return true;
}

void GlassBackdrop::Destroy() {
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    shown_ = false;
    ReleaseSurface();
}

bool GlassBackdrop::SetBlur(bool enabled) {
    blurEnabled_ = false;
    if (!hwnd_) {
        return false;
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto setWindowCompositionAttribute = user32
        ? reinterpret_cast<SetWindowCompositionAttributeFn>(
              reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowCompositionAttribute")))
        : nullptr;
    if (!setWindowCompositionAttribute) {
        return false;
    }

    AccentPolicy policy = {};
    policy.accentState = enabled ? kAccentEnableBlurBehind : kAccentDisabled;
    policy.accentFlags = 0;
    policy.gradientColor = 0x01000000;

    WindowCompositionAttributeData data = {};
    data.attribute = kWcaAccentPolicy;
    data.data = &policy;
    data.sizeOfData = sizeof(policy);
    const bool succeeded = setWindowCompositionAttribute(hwnd_, &data) != FALSE;
    blurEnabled_ = enabled && succeeded;
    return succeeded;
}

bool GlassBackdrop::EnsureSurface(int width, int height) {
    if (surfaceDc_ && width <= surfaceCapacity_.cx && height <= surfaceCapacity_.cy) {
        return true;
    }

    const int capacityWidth = std::max(width + 256, static_cast<int>(surfaceCapacity_.cx));
    const int capacityHeight = std::max(height, static_cast<int>(surfaceCapacity_.cy));
    ReleaseSurface();

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = capacityWidth;
    info.bmiHeader.biHeight = -capacityHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    surfaceDc_ = CreateCompatibleDC(nullptr);
    if (!surfaceDc_) {
        return false;
    }

    void* bits = nullptr;
    surfaceBitmap_ = CreateDIBSection(surfaceDc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!surfaceBitmap_ || !bits) {
        ReleaseSurface();
        return false;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    std::fill(pixels, pixels + static_cast<size_t>(capacityWidth) * static_cast<size_t>(capacityHeight), kNearlyClearPixel);

    previousSurfaceBitmap_ = SelectObject(surfaceDc_, surfaceBitmap_);
    surfaceCapacity_ = {capacityWidth, capacityHeight};
    return true;
}

void GlassBackdrop::ReleaseSurface() {
    if (surfaceDc_) {
        if (previousSurfaceBitmap_) {
            SelectObject(surfaceDc_, previousSurfaceBitmap_);
        }
        DeleteDC(surfaceDc_);
    }
    if (surfaceBitmap_) {
        DeleteObject(surfaceBitmap_);
    }

    surfaceDc_ = nullptr;
    surfaceBitmap_ = nullptr;
    previousSurfaceBitmap_ = nullptr;
    surfaceCapacity_ = {};
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

    const bool sizeChanged = width != placedRect_.right - placedRect_.left ||
                             height != placedRect_.bottom - placedRect_.top;
    const bool moved = screenRect.left != placedRect_.left || screenRect.top != placedRect_.top;

    if (sizeChanged || moved || !shown_) {
        if (!EnsureSurface(width, height)) {
            return;
        }

        POINT source = {0, 0};
        POINT destination = {screenRect.left, screenRect.top};
        SIZE size = {width, height};
        BLENDFUNCTION blend = {};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(hwnd_, nullptr, &destination, &size, surfaceDc_, &source, 0, &blend, ULW_ALPHA);
    }

    if (sizeChanged || cornerRadius != regionRadius_) {
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, cornerRadius * 2, cornerRadius * 2);
        if (region && SetWindowRgn(hwnd_, region, TRUE) == 0) {
            DeleteObject(region); // The system owns the region only on success.
        }
        regionRadius_ = cornerRadius;
    }

    placedRect_ = screenRect;

    // Z-order only: position and size were applied atomically by UpdateLayeredWindow.
    SetWindowPos(
        hwnd_,
        above ? above : HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | (shown_ ? 0 : SWP_SHOWWINDOW));
    shown_ = true;
}

void GlassBackdrop::Hide() {
    if (hwnd_ && shown_) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    shown_ = false;
}

LRESULT CALLBACK GlassBackdrop::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace MyDock

#pragma once

#include <Windows.h>

namespace MyDock {

// A borderless window that shows a live blur of whatever is behind the dock surface.
//
// The dock window itself cannot carry the blur: it also contains the transparent headroom above
// the surface where magnified icons grow, and any blur applied to it would cover that area too.
// This window is shaped to the dock surface and kept directly below the dock window in z-order;
// the dock then paints its translucent glass layers on top of the blur.
//
// The blur uses the (undocumented but long-stable) SetWindowCompositionAttribute accent API on a
// layered window, the same combination used by desktop blur tools. The layered content is
// effectively invisible (alpha 1), so if the blur is unavailable nothing visible is left behind.
class GlassBackdrop {
public:
    explicit GlassBackdrop(HINSTANCE instance);
    ~GlassBackdrop();

    GlassBackdrop(const GlassBackdrop&) = delete;
    GlassBackdrop& operator=(const GlassBackdrop&) = delete;

    bool Create();
    void Destroy();

    // Turns the blur on or off. Returns whether the accent call succeeded.
    bool SetBlur(bool enabled);
    bool BlurEnabled() const {
        return blurEnabled_;
    }

    // Moves the backdrop to `screenRect` (rounded by `cornerRadius`) directly below `above`.
    void Place(HWND above, const RECT& screenRect, int cornerRadius);

    // Hides the backdrop; the next Place() shows it again.
    void Hide();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool EnsureSurface(int width, int height);
    void ReleaseSurface();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    bool blurEnabled_ = false;
    bool shown_ = false;
    RECT placedRect_ = {};
    int regionRadius_ = -1;

    HDC surfaceDc_ = nullptr;
    HBITMAP surfaceBitmap_ = nullptr;
    HGDIOBJ previousSurfaceBitmap_ = nullptr;
    SIZE surfaceCapacity_ = {};
};

} // namespace MyDock

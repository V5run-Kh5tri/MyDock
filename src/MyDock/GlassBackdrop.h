#pragma once

#include <Windows.h>

namespace MyDock {

// A borderless window that shows a live blur of whatever is behind the dock surface.
//
// The dock window itself cannot carry the blur: it also contains the transparent headroom above
// the surface where magnified icons grow, and any blur applied to it would cover that area too.
// This window is shaped exactly to the dock surface and kept directly below the dock window in
// z-order; the dock then paints its translucent glass layers on top of the blur.
//
// The blur uses the (undocumented but long-stable) SetWindowCompositionAttribute accent API, the
// same mechanism used by taskbar styling tools. If it is unavailable the window stays empty and the
// dock still renders as translucent tinted glass, just without blur.
class GlassBackdrop {
public:
    explicit GlassBackdrop(HINSTANCE instance);
    ~GlassBackdrop();

    GlassBackdrop(const GlassBackdrop&) = delete;
    GlassBackdrop& operator=(const GlassBackdrop&) = delete;

    bool Create();
    void Destroy();

    // Moves the backdrop to `screenRect` (rounded by `cornerRadius`) directly below `above`.
    void Place(HWND above, const RECT& screenRect, int cornerRadius);

    bool BlurEnabled() const {
        return blurEnabled_;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool EnableBlur();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    bool blurEnabled_ = false;
    SIZE regionSize_ = {};
    int regionRadius_ = -1;
};

} // namespace MyDock

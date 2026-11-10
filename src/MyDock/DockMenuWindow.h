#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace MyDock {

struct DockMenuItem {
    UINT command = 0;
    std::wstring label;
    wchar_t glyph = 0; // Segoe Fluent Icons / Segoe MDL2 Assets code point, 0 for none.
    bool separator = false;
};

// Dock-styled replacement for the native Win32 popup menu.
//
// The window is owned by the dock, so Windows keeps it above the dock in z-order even though the
// dock re-asserts HWND_TOPMOST on every animation tick. A chosen command is not executed inline:
// it is posted to the owner as `commandMessage` (wParam = command) after the menu has hidden, so
// no dock state is touched from inside this window's message handling.
class DockMenuWindow {
public:
    explicit DockMenuWindow(HINSTANCE instance);
    ~DockMenuWindow();

    DockMenuWindow(const DockMenuWindow&) = delete;
    DockMenuWindow& operator=(const DockMenuWindow&) = delete;

    bool Create(HWND owner, UINT commandMessage);

    // Shows the menu horizontally centred on [anchorLeft, anchorRight] (screen coordinates) with
    // its bottom edge at `bottom`, clamped to the monitor.
    void Show(const std::wstring& title, std::vector<DockMenuItem> items, LONG anchorLeft, LONG anchorRight, LONG bottom);
    void Hide();
    bool IsVisible() const;

    // True briefly after the menu was dismissed by clicking elsewhere, so the owner can swallow
    // that click the way native menus do.
    bool WasJustDismissed() const;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void CreateFonts();
    void DestroyFonts();
    void Paint();
    int HeaderHeight() const;
    int ItemTop(size_t index) const;
    int ItemHeight(size_t index) const;
    int ContentHeight() const;
    int HitTest(POINT point) const;
    void SetHover(int index);
    void MoveHover(int direction);
    void Invoke(int index);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    UINT commandMessage_ = 0;
    HFONT labelFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT glyphFont_ = nullptr;
    std::wstring title_;
    std::vector<DockMenuItem> items_;
    int hoverIndex_ = -1;
    bool trackingMouse_ = false;
    ULONGLONG dismissedAt_ = 0;
};

} // namespace MyDock

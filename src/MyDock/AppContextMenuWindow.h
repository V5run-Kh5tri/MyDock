#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

namespace MyDock {

struct AppContextMenuItem {
    UINT command = 0;
    std::wstring label;
    bool enabled = true;
    bool separator = false;
};

class AppContextMenuWindow {
public:
    explicit AppContextMenuWindow(HINSTANCE instance);

    bool Create(HWND owner);
    void Show(const std::vector<AppContextMenuItem>& items, const RECT& anchor, std::function<void(UINT)> onCommand);
    void Hide();
    bool IsVisible() const;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void Paint();
    int HitTest(POINT point) const;
    SIZE Measure() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    std::vector<AppContextMenuItem> items_;
    std::function<void(UINT)> onCommand_;
    int hoverIndex_ = -1;
};

} // namespace MyDock

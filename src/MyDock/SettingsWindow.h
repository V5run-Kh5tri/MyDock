#pragma once

#include "SettingsStore.h"

#include <functional>

namespace MyDock {

class SettingsWindow {
public:
    SettingsWindow(HINSTANCE instance, SettingsStore& settingsStore, std::function<void()> onSettingsChanged);

    bool Create(HWND owner);
    void ShowNear(const RECT& anchor);
    void Hide();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void Paint();
    RECT AutoHideCheckRect() const;
    RECT ReplaceTaskbarCheckRect() const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    SettingsStore& settingsStore_;
    std::function<void()> onSettingsChanged_;
};

} // namespace MyDock

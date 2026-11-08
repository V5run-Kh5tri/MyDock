#include "ShellActions.h"

#include <Shellapi.h>

namespace MyDock::ShellActions {

namespace {

void SendWinShortcut(WORD key) {
    keybd_event(VK_LWIN, 0, 0, 0);
    keybd_event(static_cast<BYTE>(key), 0, 0, 0);
    keybd_event(static_cast<BYTE>(key), 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);
}

} // namespace

void OpenQuickSettings() {
    SendWinShortcut('A');
}

void OpenNotificationCenter() {
    SendWinShortcut('N');
}

void OpenSettingsHome() {
    ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
}

void ActivateWindow(HWND hwnd) {
    if (!IsWindow(hwnd)) {
        return;
    }

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }

    SetForegroundWindow(hwnd);
}

void ToggleWindowState(HWND hwnd) {
    ToggleWindowState(hwnd, GetAncestor(GetForegroundWindow(), GA_ROOT));
}

void ToggleWindowState(HWND hwnd, HWND activeWindow) {
    if (!IsWindow(hwnd)) {
        return;
    }

    if (activeWindow == hwnd && !IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_MINIMIZE);
        return;
    }

    ActivateWindow(hwnd);
}

} // namespace MyDock::ShellActions

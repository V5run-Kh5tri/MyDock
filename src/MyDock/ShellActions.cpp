#include "ShellActions.h"

#include <Shellapi.h>

namespace MyDock::ShellActions {

void OpenQuickSettings() {
    keybd_event(VK_LWIN, 0, 0, 0);
    keybd_event('A', 0, 0, 0);
    keybd_event('A', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);
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

} // namespace MyDock::ShellActions


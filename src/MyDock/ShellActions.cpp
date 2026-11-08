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

bool IsWindowActive(HWND hwnd) {
    if (!IsWindow(hwnd)) {
        return false;
    }

    const HWND root = GetAncestor(hwnd, GA_ROOT);
    const auto matches = [root](HWND candidate) {
        return candidate && GetAncestor(candidate, GA_ROOT) == root;
    };

    if (matches(GetForegroundWindow())) {
        return true;
    }

    GUITHREADINFO guiThreadInfo = {};
    guiThreadInfo.cbSize = sizeof(guiThreadInfo);
    if (GetGUIThreadInfo(0, &guiThreadInfo)) {
        return matches(guiThreadInfo.hwndActive) ||
               matches(guiThreadInfo.hwndFocus) ||
               matches(guiThreadInfo.hwndCapture) ||
               matches(guiThreadInfo.hwndMenuOwner) ||
               matches(guiThreadInfo.hwndMoveSize) ||
               matches(guiThreadInfo.hwndCaret);
    }

    return false;
}

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

    const bool isActive = (activeWindow && GetAncestor(activeWindow, GA_ROOT) == GetAncestor(hwnd, GA_ROOT)) || IsWindowActive(hwnd);
    if (isActive && !IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_MINIMIZE);
        return;
    }

    ActivateWindow(hwnd);
}

} // namespace MyDock::ShellActions

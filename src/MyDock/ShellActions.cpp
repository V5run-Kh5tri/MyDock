#include "ShellActions.h"

#include <Shellapi.h>

namespace MyDock::ShellActions {

namespace {

INPUT KeyInput(WORD key, bool keyUp) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    return input;
}

// SendInput queues the whole chord atomically (keybd_event is deprecated and can interleave with
// real input).
void SendWinShortcut(WORD key) {
    INPUT inputs[] = {KeyInput(VK_LWIN, false), KeyInput(key, false), KeyInput(key, true), KeyInput(VK_LWIN, true)};
    SendInput(_countof(inputs), inputs, sizeof(INPUT));
}

void SendKeyPress(WORD key) {
    INPUT inputs[] = {KeyInput(key, false), KeyInput(key, true)};
    SendInput(_countof(inputs), inputs, sizeof(INPUT));
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

void OpenStartMenu() {
    SendKeyPress(VK_LWIN);
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

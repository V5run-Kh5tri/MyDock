#include "NativeTaskbarController.h"

#include "Log.h"

#include <algorithm>
#include <cwchar>

namespace MyDock {
namespace {

constexpr int kRestoreHotkeyId = 0x4D54;

bool IsTaskbarWindow(HWND hwnd) {
    wchar_t className[80] = {};
    if (GetClassNameW(hwnd, className, _countof(className)) == 0) {
        return false;
    }

    return wcscmp(className, L"Shell_TrayWnd") == 0 ||
           wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

BOOL CALLBACK CollectTaskbarWindow(HWND hwnd, LPARAM lParam) {
    if (IsTaskbarWindow(hwnd)) {
        auto* windows = reinterpret_cast<std::vector<HWND>*>(lParam);
        windows->push_back(hwnd);
    }

    return TRUE;
}

} // namespace

bool NativeTaskbarController::Initialize(HWND owner) {
    owner_ = owner;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    hotkeyRegistered_ = RegisterHotKey(
                            owner_,
                            kRestoreHotkeyId,
                            MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                            'T') != FALSE;

    if (!hotkeyRegistered_) {
        Log::Write(L"NativeTaskbarController restore hotkey registration failed");
    }

    return taskbarCreatedMessage_ != 0;
}

void NativeTaskbarController::ApplyEnabled(bool enabled) {
    if (enabled) {
        enabled_ = true;
        HideTaskbarWindows();
        return;
    }

    enabled_ = false;
    RestoreTaskbarWindows();
}

void NativeTaskbarController::ReapplyIfEnabled() {
    if (enabled_) {
        HideTaskbarWindows();
    }
}

void NativeTaskbarController::EmergencyRestore() {
    enabled_ = false;
    RestoreTaskbarWindows();
    Log::Write(L"Native taskbar restored by emergency hotkey");
}

void NativeTaskbarController::Shutdown() {
    enabled_ = false;
    RestoreTaskbarWindows();

    if (hotkeyRegistered_ && owner_) {
        UnregisterHotKey(owner_, kRestoreHotkeyId);
        hotkeyRegistered_ = false;
    }
}

bool NativeTaskbarController::IsTaskbarCreatedMessage(UINT message) const {
    return taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_;
}

void NativeTaskbarController::OnTaskbarCreated() {
    originalVisibility_.clear();
    ReapplyIfEnabled();
}

bool NativeTaskbarController::IsRestoreHotkey(WPARAM id) const {
    return id == kRestoreHotkeyId;
}

std::vector<HWND> NativeTaskbarController::DiscoverTaskbarWindows() const {
    std::vector<HWND> windows;
    EnumWindows(CollectTaskbarWindow, reinterpret_cast<LPARAM>(&windows));

    std::vector<HWND> uniqueWindows;
    for (const HWND hwnd : windows) {
        if (hwnd && IsWindow(hwnd) && std::find(uniqueWindows.begin(), uniqueWindows.end(), hwnd) == uniqueWindows.end()) {
            uniqueWindows.push_back(hwnd);
        }
    }

    return uniqueWindows;
}

void NativeTaskbarController::HideTaskbarWindows() {
    for (const HWND hwnd : DiscoverTaskbarWindows()) {
        if (originalVisibility_.find(hwnd) == originalVisibility_.end()) {
            originalVisibility_[hwnd] = IsWindowVisible(hwnd) != FALSE;
        }

        ShowWindow(hwnd, SW_HIDE);
    }
}

void NativeTaskbarController::RestoreTaskbarWindows() {
    for (const HWND hwnd : DiscoverTaskbarWindows()) {
        const auto previous = originalVisibility_.find(hwnd);
        if (previous == originalVisibility_.end() || previous->second) {
            ShowWindow(hwnd, SW_SHOW);
        }
    }

    originalVisibility_.clear();
}

} // namespace MyDock

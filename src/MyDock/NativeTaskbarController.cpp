#include "NativeTaskbarController.h"

#include <ShlObj.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <fstream>

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

std::filesystem::path RecoveryFilePath() {
    PWSTR rawPath = nullptr;
    std::filesystem::path root;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &rawPath))) {
        root = rawPath;
        CoTaskMemFree(rawPath);
    }

    if (root.empty()) {
        root = std::filesystem::temp_directory_path();
    }

    return root / L"MyDock" / L"taskbar-workarea.txt";
}

void SaveRecoveryWorkArea(const RECT& rect) {
    const auto path = RecoveryFilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::wofstream file(path, std::ios::trunc);
    if (file.is_open()) {
        file << rect.left << L" " << rect.top << L" " << rect.right << L" " << rect.bottom << L"\n";
    }
}

bool LoadRecoveryWorkArea(RECT* rect) {
    if (!rect) {
        return false;
    }

    std::wifstream file(RecoveryFilePath());
    if (!file.is_open()) {
        return false;
    }

    RECT loaded = {};
    file >> loaded.left >> loaded.top >> loaded.right >> loaded.bottom;
    if (!file || loaded.right <= loaded.left || loaded.bottom <= loaded.top) {
        return false;
    }

    *rect = loaded;
    return true;
}

void DeleteRecoveryWorkArea() {
    std::error_code ec;
    std::filesystem::remove(RecoveryFilePath(), ec);
}

} // namespace

bool NativeTaskbarController::Initialize(HWND owner) {
    owner_ = owner;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    RestorePersistedWorkArea();

    hotkeyRegistered_ = RegisterHotKey(
                            owner_,
                            kRestoreHotkeyId,
                            MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT,
                            'T') != FALSE;

    if (!hotkeyRegistered_) {
        Log(L"NativeTaskbarController restore hotkey registration failed");
    }

    return taskbarCreatedMessage_ != 0;
}

void NativeTaskbarController::ApplyEnabled(bool enabled) {
    if (enabled) {
        enabled_ = true;
        ReleaseWorkArea();
        HideTaskbarWindows();
        return;
    }

    enabled_ = false;
    RestoreTaskbarWindows();
    RestoreWorkArea();
}

void NativeTaskbarController::ReapplyIfEnabled() {
    if (enabled_) {
        ReleaseWorkArea();
        HideTaskbarWindows();
    }
}

void NativeTaskbarController::EmergencyRestore() {
    enabled_ = false;
    RestoreTaskbarWindows();
    RestoreWorkArea();
    Log(L"Native taskbar restored by emergency hotkey");
}

void NativeTaskbarController::Shutdown() {
    enabled_ = false;
    RestoreTaskbarWindows();
    RestoreWorkArea();

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

void NativeTaskbarController::ReleaseWorkArea() {
    if (!workAreaChanged_) {
        if (LoadRecoveryWorkArea(&originalWorkArea_)) {
            workAreaChanged_ = true;
        } else if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &originalWorkArea_, 0)) {
            SaveRecoveryWorkArea(originalWorkArea_);
            workAreaChanged_ = true;
        } else {
            Log(L"SPI_GETWORKAREA failed before native taskbar replacement");
            return;
        }
    }

    POINT origin = {};
    HMONITOR monitor = owner_ ? MonitorFromWindow(owner_, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) {
        Log(L"GetMonitorInfoW failed before native taskbar replacement");
        return;
    }

    RECT fullWorkArea = monitorInfo.rcMonitor;
    if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &fullWorkArea, SPIF_SENDCHANGE)) {
        Log(L"SPI_SETWORKAREA failed during native taskbar replacement");
    }
}

void NativeTaskbarController::RestoreWorkArea() {
    if (!workAreaChanged_) {
        return;
    }

    if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &originalWorkArea_, SPIF_SENDCHANGE)) {
        Log(L"SPI_SETWORKAREA failed while restoring native taskbar work area");
    }

    DeleteRecoveryWorkArea();
    originalWorkArea_ = {};
    workAreaChanged_ = false;
}

void NativeTaskbarController::RestorePersistedWorkArea() {
    RECT persisted = {};
    if (!LoadRecoveryWorkArea(&persisted)) {
        return;
    }

    if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &persisted, SPIF_SENDCHANGE)) {
        Log(L"SPI_SETWORKAREA failed while restoring persisted taskbar work area");
        return;
    }

    DeleteRecoveryWorkArea();
}

void NativeTaskbarController::Log(const wchar_t* message) const {
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempPathW(_countof(tempPath), tempPath) == 0) {
        return;
    }

    std::wofstream log(std::filesystem::path(tempPath) / L"MyDock.log", std::ios::app);
    if (log.is_open()) {
        log << message << L"\n";
    }
}

} // namespace MyDock

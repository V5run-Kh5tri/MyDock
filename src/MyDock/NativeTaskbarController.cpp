#include "NativeTaskbarController.h"

#include <ShlObj.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <filesystem>
#include <fstream>

namespace MyDock {
namespace {

constexpr int kRestoreHotkeyId = 0x4D54;
constexpr UINT kWorkAreaChangeFlags = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
constexpr auto kAggressiveSuppressDuration = std::chrono::seconds(6);

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

BOOL CALLBACK CollectChildWindow(HWND hwnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::vector<HWND>*>(lParam);
    windows->push_back(hwnd);
    return TRUE;
}

struct MaximizedRefreshState {
    HWND owner = nullptr;
    HMONITOR monitor = nullptr;
};

bool IsRefreshCandidate(HWND hwnd, const MaximizedRefreshState& state) {
    if (!hwnd || hwnd == state.owner || !IsWindowVisible(hwnd) || !IsZoomed(hwnd)) {
        return false;
    }

    if (IsTaskbarWindow(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return false;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }

    return MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == state.monitor;
}

BOOL CALLBACK RefreshMaximizedWindow(HWND hwnd, LPARAM lParam) {
    const auto* state = reinterpret_cast<const MaximizedRefreshState*>(lParam);
    if (!state || !IsRefreshCandidate(hwnd, *state)) {
        return TRUE;
    }

    ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_MAXIMIZE);
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
        aggressiveSuppressUntil_ = std::chrono::steady_clock::now() + kAggressiveSuppressDuration;
        ReleaseWorkArea();
        HideTaskbarWindows();
        return;
    }

    enabled_ = false;
    aggressiveSuppressUntil_ = {};
    RestoreTaskbarWindows();
    RestoreWorkArea();
}

void NativeTaskbarController::ReapplyIfEnabled() {
    if (enabled_) {
        const auto now = std::chrono::steady_clock::now();
        if (now > aggressiveSuppressUntil_) {
            aggressiveSuppressUntil_ = {};
        }

        ReleaseWorkArea();
        HideTaskbarWindows();
    }
}

void NativeTaskbarController::EmergencyRestore() {
    enabled_ = false;
    aggressiveSuppressUntil_ = {};
    RestoreTaskbarWindows();
    RestoreWorkArea();
    Log(L"Native taskbar restored by emergency hotkey");
}

void NativeTaskbarController::Shutdown() {
    enabled_ = false;
    aggressiveSuppressUntil_ = {};
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
    originalWindowState_.clear();
    originalWindowOrder_.clear();
    aggressiveSuppressUntil_ = std::chrono::steady_clock::now() + kAggressiveSuppressDuration;
    ReapplyIfEnabled();
}

bool NativeTaskbarController::IsRestoreHotkey(WPARAM id) const {
    return id == kRestoreHotkeyId;
}

std::vector<HWND> NativeTaskbarController::DiscoverTaskbarWindows() const {
    std::vector<HWND> windows;

    if (HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        windows.push_back(primary);
    }

    HWND secondary = nullptr;
    while ((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        windows.push_back(secondary);
    }

    EnumWindows(CollectTaskbarWindow, reinterpret_cast<LPARAM>(&windows));

    std::vector<HWND> uniqueWindows;
    for (const HWND hwnd : windows) {
        if (hwnd && IsWindow(hwnd) && std::find(uniqueWindows.begin(), uniqueWindows.end(), hwnd) == uniqueWindows.end()) {
            uniqueWindows.push_back(hwnd);
        }
    }

    return uniqueWindows;
}

std::vector<HWND> NativeTaskbarController::DiscoverTaskbarWindowTree() const {
    std::vector<HWND> windows;
    for (const HWND taskbar : DiscoverTaskbarWindows()) {
        if (!taskbar || !IsWindow(taskbar)) {
            continue;
        }

        windows.push_back(taskbar);
        EnumChildWindows(taskbar, CollectChildWindow, reinterpret_cast<LPARAM>(&windows));
    }

    std::vector<HWND> uniqueWindows;
    for (const HWND hwnd : windows) {
        if (hwnd && IsWindow(hwnd) && std::find(uniqueWindows.begin(), uniqueWindows.end(), hwnd) == uniqueWindows.end()) {
            uniqueWindows.push_back(hwnd);
        }
    }

    return uniqueWindows;
}

void NativeTaskbarController::RememberWindowState(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || originalWindowState_.find(hwnd) != originalWindowState_.end()) {
        return;
    }

    WindowSnapshot snapshot;
    snapshot.visible = IsWindowVisible(hwnd) != FALSE;
    snapshot.enabled = IsWindowEnabled(hwnd) != FALSE;
    originalWindowState_[hwnd] = snapshot;
    originalWindowOrder_.push_back(hwnd);
}

void NativeTaskbarController::HideTaskbarWindows() {
    const bool aggressive = enabled_ && std::chrono::steady_clock::now() <= aggressiveSuppressUntil_;
    const auto windows = DiscoverTaskbarWindowTree();
    if (windows.empty()) {
        Log(L"No native taskbar windows discovered during suppression");
        return;
    }

    for (const HWND hwnd : windows) {
        RememberWindowState(hwnd);

        if (aggressive || IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_HIDE);
        }

        if (aggressive) {
            EnableWindow(hwnd, FALSE);
            SetWindowPos(
                hwnd,
                HWND_BOTTOM,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_HIDEWINDOW);
        }
    }
}

void NativeTaskbarController::RestoreTaskbarWindows() {
    for (auto iter = originalWindowOrder_.rbegin(); iter != originalWindowOrder_.rend(); ++iter) {
        const HWND hwnd = *iter;
        const auto state = originalWindowState_.find(hwnd);
        if (state == originalWindowState_.end()) {
            continue;
        }

        if (!hwnd || !IsWindow(hwnd)) {
            continue;
        }

        EnableWindow(hwnd, state->second.enabled);
        if (state->second.visible) {
            SetWindowPos(
                hwnd,
                HWND_TOP,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        } else {
            ShowWindow(hwnd, SW_HIDE);
        }
    }

    originalWindowState_.clear();
    originalWindowOrder_.clear();
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
    RECT currentWorkArea = {};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &currentWorkArea, 0) && EqualRect(&currentWorkArea, &fullWorkArea)) {
        return;
    }

    if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &fullWorkArea, kWorkAreaChangeFlags)) {
        Log(L"SPI_SETWORKAREA failed during native taskbar replacement");
        return;
    }

    RefreshMaximizedWindows();
}

void NativeTaskbarController::RestoreWorkArea() {
    if (!workAreaChanged_) {
        return;
    }

    RECT currentWorkArea = {};
    const bool alreadyRestored = SystemParametersInfoW(SPI_GETWORKAREA, 0, &currentWorkArea, 0) &&
                                 EqualRect(&currentWorkArea, &originalWorkArea_);
    if (!alreadyRestored && !SystemParametersInfoW(SPI_SETWORKAREA, 0, &originalWorkArea_, kWorkAreaChangeFlags)) {
        Log(L"SPI_SETWORKAREA failed while restoring native taskbar work area");
        return;
    }

    if (!alreadyRestored) {
        RefreshMaximizedWindows();
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

    if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &persisted, kWorkAreaChangeFlags)) {
        Log(L"SPI_SETWORKAREA failed while restoring persisted taskbar work area");
        return;
    }

    DeleteRecoveryWorkArea();
}

void NativeTaskbarController::RefreshMaximizedWindows() {
    if (!owner_) {
        return;
    }

    MaximizedRefreshState state;
    state.owner = owner_;
    state.monitor = MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST);
    if (!state.monitor) {
        return;
    }

    EnumWindows(RefreshMaximizedWindow, reinterpret_cast<LPARAM>(&state));
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

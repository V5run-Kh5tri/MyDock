#pragma once

#include <Windows.h>

#include <chrono>
#include <map>
#include <vector>

namespace MyDock {

class NativeTaskbarController {
public:
    bool Initialize(HWND owner);
    void ApplyEnabled(bool enabled);
    void ReapplyIfEnabled();
    void EmergencyRestore();
    void Shutdown();

    bool IsTaskbarCreatedMessage(UINT message) const;
    void OnTaskbarCreated();
    bool IsRestoreHotkey(WPARAM id) const;

private:
    struct WindowSnapshot {
        bool visible = false;
        bool enabled = true;
    };

    std::vector<HWND> DiscoverTaskbarWindows() const;
    std::vector<HWND> DiscoverTaskbarWindowTree() const;
    void RememberWindowState(HWND hwnd);
    void HideTaskbarWindows();
    void RestoreTaskbarWindows();
    void ReleaseWorkArea();
    void RestoreWorkArea();
    void RestorePersistedWorkArea();
    void RefreshMaximizedWindows();
    void Log(const wchar_t* message) const;

    HWND owner_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    bool enabled_ = false;
    bool hotkeyRegistered_ = false;
    bool workAreaChanged_ = false;
    RECT originalWorkArea_ = {};
    std::chrono::steady_clock::time_point aggressiveSuppressUntil_ = {};
    std::map<HWND, WindowSnapshot> originalWindowState_;
    std::vector<HWND> originalWindowOrder_;
};

} // namespace MyDock

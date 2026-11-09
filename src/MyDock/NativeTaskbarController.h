#pragma once

#include <Windows.h>

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
    std::vector<HWND> DiscoverTaskbarWindows() const;
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
    std::map<HWND, bool> originalVisibility_;
};

} // namespace MyDock

#pragma once

#include <Windows.h>

#include <string>
#include <vector>

namespace MyDock {

struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    bool minimized = false;
};

struct IconIdentity {
    std::wstring shellItemPath;
    std::wstring shortcutPath;
    std::wstring appUserModelId;
    std::wstring iconResourcePath;
    int iconResourceIndex = 0;
    std::wstring executablePath;
};

struct PinnedAppDefinition {
    std::wstring id;
    std::wstring displayName;
    std::wstring launchCommand;
    std::wstring iconPath;
    IconIdentity iconIdentity;
};

struct ApplicationGroup {
    std::wstring id;
    std::wstring displayName;
    std::wstring launchCommand;
    std::wstring iconPath;
    IconIdentity iconIdentity;
    bool pinned = false;
    std::vector<WindowInfo> windows;

    int WindowCount() const {
        return static_cast<int>(windows.size());
    }
};

struct Settings {
    bool autoHide = false;
    bool replaceNativeTaskbar = false;
    std::vector<PinnedAppDefinition> pinnedApps;
};

std::wstring ToLower(std::wstring value);
std::wstring Trim(std::wstring value);
std::wstring BasenameFromPath(const std::wstring& path);
std::wstring IdentityFromExecutable(const std::wstring& executable);

} // namespace MyDock

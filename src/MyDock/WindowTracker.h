#pragma once

#include "Models.h"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace MyDock {

class WindowTracker {
public:
    std::vector<ApplicationGroup> Snapshot() const;

    // Shell display name for a packaged app ("Settings", "Calculator", ...), cached per AUMID.
    std::wstring PackagedDisplayNameFor(const std::wstring& appUserModelId) const;

    static bool IsAltTabCandidate(HWND hwnd);
    static std::wstring WindowTitle(HWND hwnd);
    static std::wstring ProcessImagePath(DWORD processId);

private:
    // What a window belongs to never changes during its lifetime, so it is resolved once per window
    // (process image, packaged AUMID) instead of on every once-a-second snapshot.
    struct WindowIdentity {
        DWORD processId = 0;
        std::wstring imagePath;
        std::wstring appImagePath;
        std::wstring appUserModelId;
    };

    // Shell lookups are comparatively expensive and Snapshot() runs every second.
    mutable std::map<std::wstring, std::wstring> packagedDisplayNames_;
    mutable std::unordered_map<HWND, WindowIdentity> windowIdentities_;
};

} // namespace MyDock

#pragma once

#include "Models.h"

#include <map>
#include <string>
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
    // Shell lookups are comparatively expensive and Snapshot() runs every second.
    mutable std::map<std::wstring, std::wstring> packagedDisplayNames_;
};

} // namespace MyDock

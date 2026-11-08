#pragma once

#include "Models.h"

#include <vector>

namespace MyDock {

class WindowTracker {
public:
    std::vector<ApplicationGroup> Snapshot() const;

    static bool IsAltTabCandidate(HWND hwnd);
    static std::wstring WindowTitle(HWND hwnd);
    static std::wstring ProcessImagePath(DWORD processId);
};

} // namespace MyDock

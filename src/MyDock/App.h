#pragma once

#include "DockWindow.h"
#include "IconCache.h"
#include "PinnedApps.h"
#include "SettingsStore.h"
#include "WindowTracker.h"

namespace MyDock {

class App {
public:
    explicit App(HINSTANCE instance);
    int Run();

private:
    HINSTANCE instance_ = nullptr;
    SettingsStore settingsStore_;
    PinnedApps pinnedApps_;
    WindowTracker windowTracker_;
    IconCache iconCache_;
};

} // namespace MyDock


#include "App.h"

#include <CommCtrl.h>

namespace MyDock {

App::App(HINSTANCE instance)
    : instance_(instance),
      pinnedApps_(settingsStore_) {}

int App::Run() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }

    settingsStore_.Load();
    pinnedApps_.EnsureDefaults();

    DockWindow dockWindow(instance_, settingsStore_, pinnedApps_, windowTracker_, iconCache_);
    if (!dockWindow.Create()) {
        CoUninitialize();
        return 1;
    }

    dockWindow.Show();

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}

} // namespace MyDock

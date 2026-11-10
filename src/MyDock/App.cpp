#include "App.h"

#include "Log.h"

#include <CommCtrl.h>
#include <Windows.h>
#include <Dwmapi.h>

namespace MyDock {

App::App(HINSTANCE instance)
    : instance_(instance),
      pinnedApps_(settingsStore_) {}

int App::Run() {
    Log::Write(L"MyDock starting");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        Log::Write(L"CoInitializeEx failed");
        MessageBoxW(nullptr, L"MyDock could not initialize COM.", L"MyDock startup error", MB_ICONERROR | MB_OK);
        return 1;
    }

    settingsStore_.Load();
    pinnedApps_.EnsureDefaults();

    DockWindow dockWindow(instance_, settingsStore_, pinnedApps_, windowTracker_, iconCache_);
    if (!dockWindow.Create()) {
        Log::Write(L"DockWindow::Create failed");
        MessageBoxW(nullptr, L"MyDock could not create the dock window.", L"MyDock startup error", MB_ICONERROR | MB_OK);
        CoUninitialize();
        return 1;
    }

    dockWindow.Show();
    Log::Write(L"MyDock window shown");

    // Idle: block in GetMessage. Animating: drain messages, render one frame, then wait for the
    // compositor's next frame so animation is paced by the display refresh rather than by
    // low-resolution, low-priority WM_TIMER ticks.
    MSG message = {};
    bool running = true;
    while (running) {
        if (!dockWindow.WantsAnimationFrames()) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) {
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }

        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) {
            break;
        }

        dockWindow.AnimationFrame();

        const ULONGLONG frameStart = GetTickCount64();
        if (FAILED(DwmFlush()) || GetTickCount64() - frameStart < 2) {
            // DWM did not block (composition idle or unavailable): avoid spinning, but keep
            // responding to input.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 8, QS_ALLINPUT);
        }
    }

    CoUninitialize();
    Log::Write(L"MyDock exiting");
    return static_cast<int>(message.wParam);
}

} // namespace MyDock

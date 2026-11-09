# MyDock technical approach

## Phase 0 decision

MyDock starts as a native C++ Windows desktop application built with Visual Studio and MSBuild.

The Phase 1 implementation uses Win32 windows with Direct2D and DirectWrite for the dock surface, text, vector controls and indicators. Application icons are drawn from native `HICON` handles, and running windows are tracked through top-level Win32 window enumeration. This keeps the implementation close to the Windows shell APIs that Phase 2 will need while avoiding a dependency on a previous MyDock codebase.

This approach was chosen because it supports:

- Compact, content-sized floating windows.
- Transparent or translucent dock surfaces with no full-width taskbar replacement in Phase 1.
- Precise pointer handling for magnification and dynamic spacing.
- Native process/window enumeration, activation and restoration.
- Native icon extraction.
- DWM thumbnail integration for separate preview panels.
- Visual Studio and MSBuild as the primary development workflow.

## Architecture

The project is split into small responsibilities:

- `App`: application lifetime and message loop.
- `DockWindow`: main dock window, layout, rendering, input, magnification and auto-hide.
- `WindowTracker`: grouping visible top-level windows by application identity.
- `PinnedApps`: pinned app defaults, persistence mapping and launching.
- `SettingsStore`: user settings in `%APPDATA%\MyDock\settings.ini`.
- `IconCache`: native icon loading and lifetime management.
- `PreviewWindow`: separate right-click window preview panel.
- `SettingsWindow`: compact settings panel for auto-hide and native taskbar replacement opt-in.
- `ShellActions`: safe Phase 1 shell entry points such as Quick Settings.
- `NativeTaskbarController`: Phase 2 taskbar discovery, opt-in suppression, restoration and recovery hotkey handling.

Phase 2 taskbar suppression is present but remains explicitly opt-in. The native Windows taskbar is restored when MyDock exits, and `Ctrl+Alt+Shift+T` is reserved as an emergency restore path.

## Current verification limits

This repository was initialized from a Linux environment without Visual Studio, MSBuild or the Windows SDK available. The project files and source are prepared for Windows, but build and runtime verification must be performed on Windows with Visual Studio or Build Tools installed.

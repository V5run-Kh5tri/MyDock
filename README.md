# MyDock

MyDock is a fresh native C++ implementation of a compact, macOS-inspired dock for Windows 11 with Windows Fluent-style presentation.

The repository is currently in Phase 1 development. The native Windows taskbar is intentionally left untouched; taskbar suppression and replacement are Phase 2 work and require explicit user approval after the dock UI/UX is accepted.

## Build

Open `MyDock.sln` in Visual Studio 2022 and build `Debug|x64` or `Release|x64`.

From a Visual Studio Developer PowerShell:

```powershell
msbuild .\MyDock.sln /p:Configuration=Debug /p:Platform=x64
```

## Current implementation

- Native Win32 C++ application with Visual Studio/MSBuild project files.
- Direct2D/DirectWrite dock surface rendering.
- Fixed left system section with clock/date, Quick Settings and MyDock settings buttons.
- Pinned application defaults and settings persistence in `%APPDATA%\MyDock\settings.ini`.
- Running application detection via top-level window enumeration.
- Pinned/running app merge without duplicate running entries.
- Window-count indicators.
- Hover magnification with dynamic spacing.
- App launching and window activation.
- Separate right-click window preview panel with DWM thumbnail registration where supported.
- Configurable auto-hide in the MyDock settings panel.

See `docs/TECHNICAL_APPROACH.md` for the Phase 0 technology decision and architecture summary.

# Phase 1 implementation breakdown

Phase 1 stays focused on a working dock UI/UX while leaving the native Windows taskbar fully functional.

## Implemented in the first pass

- Native dock process and Visual Studio/MSBuild project foundation.
- Fixed system section with clock/date, Quick Settings and MyDock settings buttons.
- Pinned app defaults and persisted settings.
- Running application detection and grouping by executable identity.
- Merged pinned and non-pinned running applications with duplicate suppression.
- Window-count indicators.
- Hover magnification with dynamic spacing.
- Application launch and running-window activation.
- Right-click preview panel with DWM thumbnail registration.
- Configurable auto-hide setting and dock retraction behavior.

## Next verification steps on Windows

1. Build `MyDock.sln` with Visual Studio 2022 or MSBuild.
2. Run the app on Windows 11 and verify the dock remains above the existing taskbar work area.
3. Verify pinned app launching for File Explorer, Edge and Notepad.
4. Open multiple windows per application and verify grouping plus one/two/three indicators.
5. Verify hover magnification spacing at normal and high DPI.
6. Verify right-click preview activation, including minimized windows.
7. Toggle auto-hide in MyDock settings and verify reveal/retract timing.
8. Tune visual details after runtime inspection before requesting Phase 1 UI/UX approval.

## Deferred until after explicit Phase 1 approval

- Native taskbar suppression.
- Explorer/taskbar lifecycle handling.
- Recovery mechanisms for replacing the taskbar.
- System tray replacement work.
- Any live modification of the user's Windows taskbar state.


# Phase 2 testing notes

Native taskbar replacement is opt-in. A fresh build starts with the Windows taskbar untouched.

## Enable replacement

1. Build and run MyDock.
2. Open MyDock settings from the dock.
3. Enable `Replace native taskbar`.
4. Verify the native Windows taskbar hides and MyDock moves to the physical bottom edge.
5. Maximize a normal app window and verify it occupies the old taskbar space, with MyDock floating on top.

## Recovery

- Press `Ctrl+Alt+Shift+T` while MyDock is running to restore the native taskbar and disable replacement.
- Closing MyDock restores the native taskbar.
- If MyDock stops unexpectedly while replacement is enabled, launching MyDock again restores any saved original work-area value before applying the current setting.
- If Explorer recreates its taskbar windows while replacement is enabled, MyDock should hide the recreated taskbar again.

## Manual checks

1. Toggle replacement on and off from MyDock settings.
2. Close MyDock while replacement is enabled and verify the native taskbar returns.
3. Enable replacement again, then press `Ctrl+Alt+Shift+T` and verify the native taskbar returns.
4. Restart Windows Explorer while replacement is enabled and verify MyDock remains usable.
5. Verify `Win`, `Win+A` and `Win+N` still open native Windows shell surfaces.
6. Hover across the dock and verify magnification does not show a delayed redraw or flash.

If the native taskbar is still visible after enabling replacement, inspect `%TEMP%\MyDock.log` and look for taskbar suppression messages.

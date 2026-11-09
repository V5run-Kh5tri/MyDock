#pragma once

#include "Models.h"

namespace MyDock::ShellActions {

void OpenQuickSettings();
void OpenNotificationCenter();
void OpenStartMenu();
void OpenSettingsHome();
void ActivateWindow(HWND hwnd);
bool IsWindowActive(HWND hwnd);
void ToggleWindowState(HWND hwnd);
void ToggleWindowState(HWND hwnd, HWND activeWindow);

} // namespace MyDock::ShellActions

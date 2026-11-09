#include "PinnedApps.h"

#include <Shellapi.h>

namespace MyDock {

PinnedApps::PinnedApps(SettingsStore& settingsStore)
    : settingsStore_(settingsStore) {}

const std::vector<PinnedAppDefinition>& PinnedApps::Apps() const {
    return settingsStore_.Current().pinnedApps;
}

void PinnedApps::EnsureDefaults() {
    if (settingsStore_.Current().pinnedApps.empty()) {
        settingsStore_.Load();
    }
}

bool PinnedApps::Refresh() {
    return settingsStore_.RefreshPinnedAppsFromNativeTaskbar();
}

bool PinnedApps::Pin(const ApplicationGroup& app) {
    return settingsStore_.PinApp(app);
}

bool PinnedApps::Unpin(const ApplicationGroup& app) {
    return settingsStore_.UnpinApp(app);
}

bool PinnedApps::Launch(const ApplicationGroup& app) const {
    if (app.launchCommand.empty()) {
        return false;
    }

    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", app.launchCommand.c_str(), nullptr, nullptr, SW_SHOWNORMAL));

    return result > 32;
}

} // namespace MyDock

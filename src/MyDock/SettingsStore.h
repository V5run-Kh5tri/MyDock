#pragma once

#include "Models.h"

#include <filesystem>

namespace MyDock {

class SettingsStore {
public:
    bool Load();
    bool Save() const;
    bool RefreshPinnedAppsFromNativeTaskbar();
    bool PinApp(const ApplicationGroup& app);
    bool UnpinApp(const ApplicationGroup& app);

    Settings& Current() {
        return settings_;
    }

    const Settings& Current() const {
        return settings_;
    }

    std::filesystem::path SettingsPath() const;

private:
    void RebuildPinnedApps(std::vector<PinnedAppDefinition> nativeTaskbarPins);

    Settings settings_;
    std::vector<PinnedAppDefinition> manualPinnedApps_;
    std::vector<std::wstring> hiddenNativePinIds_;
    std::vector<PinnedAppDefinition> legacyPinnedApps_;
};

} // namespace MyDock

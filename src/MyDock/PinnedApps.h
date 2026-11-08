#pragma once

#include "Models.h"
#include "SettingsStore.h"

namespace MyDock {

class PinnedApps {
public:
    explicit PinnedApps(SettingsStore& settingsStore);

    const std::vector<PinnedAppDefinition>& Apps() const;
    void EnsureDefaults();
    bool Launch(const ApplicationGroup& app) const;

private:
    SettingsStore& settingsStore_;
};

} // namespace MyDock


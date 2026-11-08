#pragma once

#include "Models.h"

#include <filesystem>

namespace MyDock {

class SettingsStore {
public:
    bool Load();
    bool Save() const;

    Settings& Current() {
        return settings_;
    }

    const Settings& Current() const {
        return settings_;
    }

    std::filesystem::path SettingsPath() const;

private:
    Settings settings_;
};

} // namespace MyDock


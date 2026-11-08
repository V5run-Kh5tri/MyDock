#include "SettingsStore.h"

#include <ShlObj.h>

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace MyDock {
namespace {

std::filesystem::path AppDataFolder() {
    PWSTR rawPath = nullptr;
    std::filesystem::path result;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &rawPath))) {
        result = rawPath;
        CoTaskMemFree(rawPath);
    }

    if (result.empty()) {
        result = std::filesystem::temp_directory_path();
    }

    return result / L"MyDock";
}

std::vector<std::wstring> Split(const std::wstring& line, wchar_t delimiter) {
    std::vector<std::wstring> values;
    std::wistringstream stream(line);
    std::wstring item;

    while (std::getline(stream, item, delimiter)) {
        values.push_back(Trim(item));
    }

    return values;
}

PinnedAppDefinition MakePinned(
    std::wstring displayName,
    std::wstring launchCommand,
    std::wstring iconPath = {}) {
    if (iconPath.empty()) {
        iconPath = launchCommand;
    }

    PinnedAppDefinition app;
    app.id = IdentityFromExecutable(launchCommand);
    app.displayName = std::move(displayName);
    app.launchCommand = std::move(launchCommand);
    app.iconPath = std::move(iconPath);
    return app;
}

std::vector<PinnedAppDefinition> DefaultPins() {
    return {
        MakePinned(L"File Explorer", L"explorer.exe"),
        MakePinned(L"Microsoft Edge", L"msedge.exe"),
        MakePinned(L"Notepad", L"notepad.exe"),
    };
}

} // namespace

bool SettingsStore::Load() {
    settings_ = {};

    const auto path = SettingsPath();
    std::wifstream file(path);

    if (!file.is_open()) {
        settings_.pinnedApps = DefaultPins();
        return Save();
    }

    std::wstring line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L'#') {
            continue;
        }

        const auto separator = line.find(L'=');
        if (separator == std::wstring::npos) {
            continue;
        }

        const auto key = ToLower(Trim(line.substr(0, separator)));
        const auto value = Trim(line.substr(separator + 1));

        if (key == L"autohide") {
            settings_.autoHide = value == L"1" || ToLower(value) == L"true";
        } else if (key == L"pin") {
            auto fields = Split(value, L'|');
            if (fields.size() >= 3) {
                PinnedAppDefinition app;
                app.displayName = fields[0];
                app.id = fields[1].empty() ? IdentityFromExecutable(fields[2]) : ToLower(fields[1]);
                app.launchCommand = fields[2];
                app.iconPath = fields.size() >= 4 && !fields[3].empty() ? fields[3] : fields[2];
                settings_.pinnedApps.push_back(std::move(app));
            }
        }
    }

    if (settings_.pinnedApps.empty()) {
        settings_.pinnedApps = DefaultPins();
    }

    return true;
}

bool SettingsStore::Save() const {
    const auto path = SettingsPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::wofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }

    file << L"# MyDock Phase 1 settings\n";
    file << L"autoHide=" << (settings_.autoHide ? L"1" : L"0") << L"\n";

    for (const auto& app : settings_.pinnedApps) {
        file << L"pin=" << app.displayName << L"|"
             << ToLower(app.id) << L"|"
             << app.launchCommand << L"|"
             << app.iconPath << L"\n";
    }

    return true;
}

std::filesystem::path SettingsStore::SettingsPath() const {
    return AppDataFolder() / L"settings.ini";
}

} // namespace MyDock

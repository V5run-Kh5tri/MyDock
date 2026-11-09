#include "SettingsStore.h"

#include <Propkey.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace MyDock {
namespace {

std::filesystem::path RoamingAppDataFolder() {
    PWSTR rawPath = nullptr;
    std::filesystem::path result;

    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &rawPath))) {
        result = rawPath;
        CoTaskMemFree(rawPath);
    }

    if (result.empty()) {
        result = std::filesystem::temp_directory_path();
    }

    return result;
}

std::filesystem::path AppDataFolder() {
    return RoamingAppDataFolder() / L"MyDock";
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

std::wstring ShortcutDisplayName(const std::filesystem::path& shortcutPath) {
    auto displayName = shortcutPath.stem().wstring();
    return displayName.empty() ? shortcutPath.filename().wstring() : displayName;
}

std::wstring ShortcutTarget(const std::filesystem::path& shortcutPath, std::wstring* iconPath, std::wstring* appUserModelId) {
    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shellLink.GetAddressOf())))) {
        return {};
    }

    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink.As(&persistFile)) || FAILED(persistFile->Load(shortcutPath.c_str(), STGM_READ))) {
        return {};
    }

    wchar_t target[MAX_PATH] = {};
    shellLink->GetPath(target, _countof(target), nullptr, SLGP_RAWPATH);

    if (iconPath) {
        wchar_t iconLocation[MAX_PATH] = {};
        int iconIndex = 0;
        if (SUCCEEDED(shellLink->GetIconLocation(iconLocation, _countof(iconLocation), &iconIndex)) && iconLocation[0] != L'\0') {
            *iconPath = iconLocation;
        }
    }

    if (target[0] != L'\0') {
        return target;
    }

    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(shellLink.As(&properties))) {
        PROPVARIANT value = {};
        PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
            if (appUserModelId) {
                *appUserModelId = value.pwszVal;
            }
            PropVariantClear(&value);
            return {};
        }
        PropVariantClear(&value);
    }

    return {};
}

std::vector<PinnedAppDefinition> NativeTaskbarPins() {
    const auto taskbarPinsFolder =
        RoamingAppDataFolder() /
        L"Microsoft" /
        L"Internet Explorer" /
        L"Quick Launch" /
        L"User Pinned" /
        L"TaskBar";

    std::error_code ec;
    if (!std::filesystem::exists(taskbarPinsFolder, ec)) {
        return {};
    }

    std::vector<std::filesystem::path> shortcuts;
    for (const auto& entry : std::filesystem::directory_iterator(taskbarPinsFolder, ec)) {
        if (ec) {
            break;
        }

        if (entry.is_regular_file(ec) && ToLower(entry.path().extension().wstring()) == L".lnk") {
            shortcuts.push_back(entry.path());
        }
    }

    std::sort(shortcuts.begin(), shortcuts.end(), [](const auto& left, const auto& right) {
        return ToLower(left.filename().wstring()) < ToLower(right.filename().wstring());
    });

    std::vector<PinnedAppDefinition> pins;
    for (const auto& shortcut : shortcuts) {
        std::wstring iconPath;
        std::wstring appUserModelId;
        std::wstring launchCommand = ShortcutTarget(shortcut, &iconPath, &appUserModelId);
        if (launchCommand.empty()) {
            launchCommand = shortcut.wstring();
        }
        iconPath = !appUserModelId.empty() ? L"shell:AppsFolder\\" + appUserModelId : shortcut.wstring();

        PinnedAppDefinition app;
        app.displayName = ShortcutDisplayName(shortcut);
        app.launchCommand = launchCommand;
        app.iconPath = iconPath;
        app.id = !appUserModelId.empty() ? appUserModelId : IdentityFromExecutable(launchCommand.empty() ? shortcut.wstring() : launchCommand);
        if (app.id.empty()) {
            app.id = ToLower(shortcut.filename().wstring());
        }
        pins.push_back(std::move(app));
    }

    return pins;
}

} // namespace

bool SettingsStore::Load() {
    settings_ = {};

    const auto path = SettingsPath();
    std::wifstream file(path);

    if (!file.is_open()) {
        settings_.pinnedApps = NativeTaskbarPins();
        if (settings_.pinnedApps.empty()) {
            settings_.pinnedApps = DefaultPins();
        }
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

    auto nativeTaskbarPins = NativeTaskbarPins();
    if (!nativeTaskbarPins.empty()) {
        settings_.pinnedApps = std::move(nativeTaskbarPins);
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

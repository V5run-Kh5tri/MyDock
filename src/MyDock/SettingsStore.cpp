#include "SettingsStore.h"

#include <Propkey.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wrl/client.h>

#include <algorithm>
#include <fstream>
#include <map>
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

bool SamePinnedApps(const std::vector<PinnedAppDefinition>& left, const std::vector<PinnedAppDefinition>& right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t index = 0; index < left.size(); ++index) {
        if (ToLower(left[index].id) != ToLower(right[index].id) ||
            left[index].displayName != right[index].displayName ||
            left[index].launchCommand != right[index].launchCommand ||
            left[index].iconPath != right[index].iconPath) {
            return false;
        }
    }

    return true;
}

bool ContainsId(const std::vector<std::wstring>& ids, const std::wstring& id) {
    const auto normalized = ToLower(id);
    return std::any_of(ids.begin(), ids.end(), [&normalized](const auto& candidate) {
        return ToLower(candidate) == normalized;
    });
}

void RemoveId(std::vector<std::wstring>& ids, const std::wstring& id) {
    const auto normalized = ToLower(id);
    ids.erase(
        std::remove_if(ids.begin(), ids.end(), [&normalized](const auto& candidate) {
            return ToLower(candidate) == normalized;
        }),
        ids.end());
}

PinnedAppDefinition DefinitionFromGroup(const ApplicationGroup& app) {
    PinnedAppDefinition definition;
    definition.id = ToLower(app.id.empty() ? IdentityFromExecutable(app.launchCommand) : app.id);
    definition.displayName = app.displayName;
    definition.launchCommand = app.launchCommand;
    definition.iconPath = app.iconPath.empty() ? app.launchCommand : app.iconPath;
    return definition;
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

bool ShouldLogShortcutDetails(
    const std::filesystem::path& shortcut,
    const std::wstring& target,
    const std::wstring& iconPath,
    const std::wstring& appUserModelId) {
    const auto combined = ToLower(shortcut.wstring() + L" " + target + L" " + iconPath + L" " + appUserModelId);
    return combined.find(L"whatsapp") != std::wstring::npos ||
           combined.find(L"settings") != std::wstring::npos ||
           combined.find(L"systemsettings") != std::wstring::npos ||
           combined.find(L"immersivecontrolpanel") != std::wstring::npos ||
           combined.find(L"windows.immersivecontrolpanel") != std::wstring::npos ||
           combined.find(L"ms-settings") != std::wstring::npos;
}

void LogShortcutDetails(
    const std::filesystem::path& shortcut,
    const std::wstring& target,
    const std::wstring& iconPath,
    const std::wstring& appUserModelId) {
    if (!ShouldLogShortcutDetails(shortcut, target, iconPath, appUserModelId)) {
        return;
    }

    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempPathW(_countof(tempPath), tempPath) == 0) {
        return;
    }

    std::wofstream log(std::filesystem::path(tempPath) / L"MyDock-icons.log", std::ios::app);
    if (!log.is_open()) {
        return;
    }

    log << L"Native taskbar shortcut for tracked app\n";
    log << L"  shortcut: " << shortcut.wstring() << L"\n";
    log << L"  target: " << target << L"\n";
    log << L"  iconPath: " << iconPath << L"\n";
    log << L"  appUserModelId: " << appUserModelId << L"\n";
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

    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(shellLink.As(&properties))) {
        PROPVARIANT value = {};
        PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
            if (appUserModelId) {
                *appUserModelId = value.pwszVal;
            }
        }
        PropVariantClear(&value);
    }

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
        if (iconPath.empty()) {
            iconPath = shortcut.wstring();
        }
        LogShortcutDetails(shortcut, launchCommand, iconPath, appUserModelId);

        PinnedAppDefinition app;
        app.displayName = ShortcutDisplayName(shortcut);
        app.launchCommand = launchCommand;
        app.iconPath = iconPath;
        app.id = (!appUserModelId.empty() && launchCommand == shortcut.wstring())
                     ? appUserModelId
                     : IdentityFromExecutable(launchCommand.empty() ? shortcut.wstring() : launchCommand);
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
    manualPinnedApps_.clear();
    hiddenNativePinIds_.clear();
    legacyPinnedApps_.clear();

    const auto path = SettingsPath();
    std::wifstream file(path);

    if (!file.is_open()) {
        RebuildPinnedApps(NativeTaskbarPins());
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
        } else if (key == L"replacenativetaskbar") {
            settings_.replaceNativeTaskbar = value == L"1" || ToLower(value) == L"true";
        } else if (key == L"hiddennativepin") {
            if (!value.empty()) {
                hiddenNativePinIds_.push_back(ToLower(value));
            }
        } else if (key == L"pin" || key == L"manualpin") {
            auto fields = Split(value, L'|');
            if (fields.size() >= 3) {
                PinnedAppDefinition app;
                app.displayName = fields[0];
                app.id = fields[1].empty() ? IdentityFromExecutable(fields[2]) : ToLower(fields[1]);
                app.launchCommand = fields[2];
                app.iconPath = fields.size() >= 4 && !fields[3].empty() ? fields[3] : fields[2];
                if (key == L"manualpin") {
                    manualPinnedApps_.push_back(std::move(app));
                } else {
                    legacyPinnedApps_.push_back(std::move(app));
                }
            }
        }
    }

    RebuildPinnedApps(NativeTaskbarPins());
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

    file << L"# MyDock settings\n";
    file << L"autoHide=" << (settings_.autoHide ? L"1" : L"0") << L"\n";
    file << L"replaceNativeTaskbar=" << (settings_.replaceNativeTaskbar ? L"1" : L"0") << L"\n";

    for (const auto& id : hiddenNativePinIds_) {
        file << L"hiddenNativePin=" << ToLower(id) << L"\n";
    }

    for (const auto& app : manualPinnedApps_) {
        file << L"manualPin=" << app.displayName << L"|"
             << ToLower(app.id) << L"|"
             << app.launchCommand << L"|"
             << app.iconPath << L"\n";
    }

    return true;
}

bool SettingsStore::RefreshPinnedAppsFromNativeTaskbar() {
    const auto previous = settings_.pinnedApps;
    RebuildPinnedApps(NativeTaskbarPins());
    return !SamePinnedApps(previous, settings_.pinnedApps);
}

bool SettingsStore::PinApp(const ApplicationGroup& app) {
    auto definition = DefinitionFromGroup(app);
    if (definition.id.empty()) {
        return false;
    }

    RemoveId(hiddenNativePinIds_, definition.id);

    const auto existing = std::find_if(manualPinnedApps_.begin(), manualPinnedApps_.end(), [&definition](const auto& pinned) {
        return ToLower(pinned.id) == definition.id;
    });

    if (existing == manualPinnedApps_.end()) {
        manualPinnedApps_.push_back(std::move(definition));
    }

    RebuildPinnedApps(NativeTaskbarPins());
    return Save();
}

bool SettingsStore::UnpinApp(const ApplicationGroup& app) {
    const auto id = ToLower(app.id.empty() ? IdentityFromExecutable(app.launchCommand) : app.id);
    if (id.empty()) {
        return false;
    }

    manualPinnedApps_.erase(
        std::remove_if(manualPinnedApps_.begin(), manualPinnedApps_.end(), [&id](const auto& pinned) {
            return ToLower(pinned.id) == id;
        }),
        manualPinnedApps_.end());

    if (!ContainsId(hiddenNativePinIds_, id)) {
        hiddenNativePinIds_.push_back(id);
    }

    RebuildPinnedApps(NativeTaskbarPins());
    return Save();
}

void SettingsStore::RebuildPinnedApps(std::vector<PinnedAppDefinition> nativeTaskbarPins) {
    std::vector<PinnedAppDefinition> merged;
    std::map<std::wstring, bool> represented;

    for (auto& app : nativeTaskbarPins) {
        app.id = ToLower(app.id);
        if (app.id.empty() || ContainsId(hiddenNativePinIds_, app.id)) {
            continue;
        }

        represented[app.id] = true;
        merged.push_back(std::move(app));
    }

    for (auto app : manualPinnedApps_) {
        app.id = ToLower(app.id);
        if (app.id.empty() || represented.find(app.id) != represented.end()) {
            continue;
        }

        represented[app.id] = true;
        merged.push_back(std::move(app));
    }

    if (merged.empty() && nativeTaskbarPins.empty() && manualPinnedApps_.empty() && hiddenNativePinIds_.empty()) {
        merged = legacyPinnedApps_.empty() ? DefaultPins() : legacyPinnedApps_;
    }

    settings_.pinnedApps = std::move(merged);
}

std::filesystem::path SettingsStore::SettingsPath() const {
    return AppDataFolder() / L"settings.ini";
}

} // namespace MyDock

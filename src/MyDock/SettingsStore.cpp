#include "SettingsStore.h"

#include <Propkey.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <cwchar>
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

// Pin fields are separated by '|'; escape '%' and '|' inside fields so names and paths containing
// them round-trip. Only these two exact sequences are decoded, so older files (which may contain
// e.g. %SystemRoot%) still load unchanged.
std::wstring EscapeField(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t ch : value) {
        if (ch == L'%') {
            escaped += L"%25";
        } else if (ch == L'|') {
            escaped += L"%7C";
        } else {
            escaped += ch;
        }
    }
    return escaped;
}

std::wstring UnescapeField(const std::wstring& value) {
    std::wstring result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value.compare(index, 3, L"%25") == 0) {
            result += L'%';
            index += 2;
        } else if (value.compare(index, 3, L"%7C") == 0 || value.compare(index, 3, L"%7c") == 0) {
            result += L'|';
            index += 2;
        } else {
            result += value[index];
        }
    }
    return result;
}

std::vector<std::wstring> Split(const std::wstring& line, wchar_t delimiter) {
    std::vector<std::wstring> values;
    std::wistringstream stream(line);
    std::wstring item;

    while (std::getline(stream, item, delimiter)) {
        values.push_back(UnescapeField(Trim(item)));
    }

    return values;
}

bool SameIconIdentity(const IconIdentity& left, const IconIdentity& right) {
    return left.shellItemPath == right.shellItemPath &&
           left.shortcutPath == right.shortcutPath &&
           left.appUserModelId == right.appUserModelId &&
           left.iconResourcePath == right.iconResourcePath &&
           left.iconResourceIndex == right.iconResourceIndex &&
           left.executablePath == right.executablePath;
}

bool SamePinnedApps(const std::vector<PinnedAppDefinition>& left, const std::vector<PinnedAppDefinition>& right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t index = 0; index < left.size(); ++index) {
        if (ToLower(left[index].id) != ToLower(right[index].id) ||
            left[index].displayName != right[index].displayName ||
            left[index].launchCommand != right[index].launchCommand ||
            left[index].iconPath != right[index].iconPath ||
            !SameIconIdentity(left[index].iconIdentity, right[index].iconIdentity)) {
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
    definition.iconIdentity = app.iconIdentity;
    if (definition.iconIdentity.executablePath.empty()) {
        definition.iconIdentity.executablePath = definition.launchCommand;
    }
    if (definition.iconIdentity.iconResourcePath.empty()) {
        definition.iconIdentity.iconResourcePath = definition.iconPath;
    }
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
    app.iconIdentity.executablePath = app.launchCommand;
    app.iconIdentity.iconResourcePath = app.iconPath;
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

std::wstring ReadStringProperty(IPropertyStore* properties, REFPROPERTYKEY key) {
    if (!properties) {
        return {};
    }

    PROPVARIANT value = {};
    PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(properties->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    }
    PropVariantClear(&value);
    return result;
}

bool ParseIconResourceString(const std::wstring& resource, std::wstring* iconPath, int* iconIndex) {
    std::wstring value = Trim(resource);
    if (!value.empty() && value.front() == L'@') {
        value.erase(value.begin());
    }

    if (value.empty()) {
        return false;
    }

    int parsedIndex = 0;
    const auto comma = value.rfind(L',');
    if (comma != std::wstring::npos) {
        const auto indexText = Trim(value.substr(comma + 1));
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(indexText.c_str(), &end, 10);
        if (end && *end == L'\0') {
            parsedIndex = static_cast<int>(parsed);
            value = Trim(value.substr(0, comma));
        }
    }

    if (value.empty()) {
        return false;
    }

    if (iconPath) {
        *iconPath = std::move(value);
    }
    if (iconIndex) {
        *iconIndex = parsedIndex;
    }
    return true;
}

std::wstring ShortcutTarget(
    const std::filesystem::path& shortcutPath,
    std::wstring* iconPath,
    int* iconIndex,
    std::wstring* appUserModelId,
    std::wstring* relaunchCommand,
    std::wstring* relaunchIconResource) {
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
        if (appUserModelId) {
            *appUserModelId = ReadStringProperty(properties.Get(), PKEY_AppUserModel_ID);
        }
        if (relaunchCommand) {
            *relaunchCommand = ReadStringProperty(properties.Get(), PKEY_AppUserModel_RelaunchCommand);
        }
        if (relaunchIconResource) {
            *relaunchIconResource = ReadStringProperty(properties.Get(), PKEY_AppUserModel_RelaunchIconResource);
        }
    }

    if (iconPath) {
        wchar_t iconLocation[MAX_PATH] = {};
        int resolvedIconIndex = 0;
        if (SUCCEEDED(shellLink->GetIconLocation(iconLocation, _countof(iconLocation), &resolvedIconIndex)) && iconLocation[0] != L'\0') {
            *iconPath = iconLocation;
            if (iconIndex) {
                *iconIndex = resolvedIconIndex;
            }
        }
    }

    if (target[0] != L'\0') {
        return target;
    }

    return {};
}

std::filesystem::path TaskbarPinsFolder() {
    return RoamingAppDataFolder() / L"Microsoft" / L"Internet Explorer" / L"Quick Launch" / L"User Pinned" / L"TaskBar";
}

// Cheap fingerprint of the pinned-shortcut folder (names, sizes, write times) so the once-a-second
// refresh only re-reads shortcuts through COM when something actually changed.
std::wstring TaskbarPinsSignature() {
    WIN32_FIND_DATAW data = {};
    const auto pattern = (TaskbarPinsFolder() / L"*.lnk").wstring();
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::wstring signature;
    do {
        signature += data.cFileName;
        signature += L"|" + std::to_wstring(data.nFileSizeLow) + L"|" + std::to_wstring(data.ftLastWriteTime.dwLowDateTime) +
                     L"|" + std::to_wstring(data.ftLastWriteTime.dwHighDateTime) + L"\n";
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return signature;
}

std::vector<PinnedAppDefinition> NativeTaskbarPins() {
    const auto taskbarPinsFolder = TaskbarPinsFolder();

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
        int iconIndex = 0;
        std::wstring appUserModelId;
        std::wstring relaunchCommand;
        std::wstring relaunchIconResource;
        std::wstring launchCommand =
            ShortcutTarget(shortcut, &iconPath, &iconIndex, &appUserModelId, &relaunchCommand, &relaunchIconResource);
        std::wstring relaunchIconPath;
        int relaunchIconIndex = 0;
        const bool hasRelaunchIcon = ParseIconResourceString(relaunchIconResource, &relaunchIconPath, &relaunchIconIndex);
        if (launchCommand.empty() && !relaunchCommand.empty()) {
            launchCommand = relaunchCommand;
        }
        if (launchCommand.empty()) {
            launchCommand = shortcut.wstring();
        }
        if (iconPath.empty()) {
            iconPath = shortcut.wstring();
        }

        PinnedAppDefinition app;
        app.displayName = ShortcutDisplayName(shortcut);
        app.launchCommand = launchCommand;
        app.iconPath = iconPath;
        app.iconIdentity.shortcutPath = shortcut.wstring();
        app.iconIdentity.appUserModelId = appUserModelId;
        app.iconIdentity.iconResourcePath = hasRelaunchIcon ? relaunchIconPath : iconPath;
        app.iconIdentity.iconResourceIndex = hasRelaunchIcon ? relaunchIconIndex : iconIndex;
        app.iconIdentity.executablePath = launchCommand == shortcut.wstring() ? std::wstring{} : launchCommand;
        // Packaged apps ("<PackageFamilyName>!<AppId>") are identified by AUMID, matching how
        // WindowTracker identifies their running windows (UWP windows belong to
        // ApplicationFrameHost.exe, so an exe-based id would never match).
        const bool packagedAumid = appUserModelId.find(L'!') != std::wstring::npos &&
                                   appUserModelId.find(L'\\') == std::wstring::npos;
        app.id = (!appUserModelId.empty() && (packagedAumid || launchCommand == shortcut.wstring()))
                     ? appUserModelId
                     : IdentityFromExecutable(launchCommand.empty() ? shortcut.wstring() : launchCommand);
        if (packagedAumid && app.iconIdentity.shellItemPath.empty()) {
            app.iconIdentity.shellItemPath = L"shell:AppsFolder\\" + appUserModelId;
        }
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
                if (fields.size() >= 5) {
                    app.iconIdentity.appUserModelId = fields[4];
                }
                if (fields.size() >= 6) {
                    app.iconIdentity.shortcutPath = fields[5];
                }
                if (fields.size() >= 7) {
                    app.iconIdentity.iconResourcePath = fields[6];
                }
                if (fields.size() >= 8) {
                    app.iconIdentity.iconResourceIndex = std::wcstol(fields[7].c_str(), nullptr, 10);
                }
                if (fields.size() >= 9) {
                    app.iconIdentity.shellItemPath = fields[8];
                }
                if (fields.size() >= 10) {
                    app.iconIdentity.executablePath = fields[9];
                }
                if (app.iconIdentity.iconResourcePath.empty()) {
                    app.iconIdentity.iconResourcePath = app.iconPath;
                }
                if (app.iconIdentity.executablePath.empty()) {
                    app.iconIdentity.executablePath = app.launchCommand;
                }
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
        file << L"manualPin=" << EscapeField(app.displayName) << L"|"
             << EscapeField(ToLower(app.id)) << L"|"
             << EscapeField(app.launchCommand) << L"|"
             << EscapeField(app.iconPath) << L"|"
             << EscapeField(app.iconIdentity.appUserModelId) << L"|"
             << EscapeField(app.iconIdentity.shortcutPath) << L"|"
             << EscapeField(app.iconIdentity.iconResourcePath) << L"|"
             << app.iconIdentity.iconResourceIndex << L"|"
             << EscapeField(app.iconIdentity.shellItemPath) << L"|"
             << EscapeField(app.iconIdentity.executablePath) << L"\n";
    }

    return true;
}

bool SettingsStore::RefreshPinnedAppsFromNativeTaskbar() {
    auto signature = TaskbarPinsSignature();
    if (signature == pinsSignature_) {
        return false;
    }
    pinsSignature_ = std::move(signature);

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

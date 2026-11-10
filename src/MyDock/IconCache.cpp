#include "IconCache.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace MyDock {
namespace {

struct IconCandidate {
    std::wstring source;
    int iconIndex = 0;
    bool useIconIndex = false;
    std::wstring reason;
};

void LogCandidate(std::wofstream& log, const wchar_t* prefix, const IconCandidate& candidate) {
    log << prefix << candidate.source;
    if (candidate.useIconIndex) {
        log << L" [index=" << candidate.iconIndex << L"]";
    }
    if (!candidate.reason.empty()) {
        log << L" [" << candidate.reason << L"]";
    }
    log << L"\n";
}

void LogIconFallback(const ApplicationGroup& app, const std::vector<IconCandidate>& iconSources) {
    wchar_t tempPath[MAX_PATH] = {};
    if (GetTempPathW(_countof(tempPath), tempPath) == 0) {
        return;
    }

    std::wofstream log(std::filesystem::path(tempPath) / L"MyDock-icons.log", std::ios::app);
    if (!log.is_open()) {
        return;
    }

    log << L"Fallback icon for: " << app.displayName << L" [" << app.id << L"]\n";
    for (const auto& source : iconSources) {
        LogCandidate(log, L"  source: ", source);
    }
}

bool ShouldLogIconDetails(const ApplicationGroup& app) {
    const auto combined = ToLower(
        app.displayName + L" " +
        app.id + L" " +
        app.launchCommand + L" " +
        app.iconPath + L" " +
        app.iconIdentity.shellItemPath + L" " +
        app.iconIdentity.shortcutPath + L" " +
        app.iconIdentity.appUserModelId + L" " +
        app.iconIdentity.iconResourcePath + L" " +
        app.iconIdentity.executablePath);
    return combined.find(L"whatsapp") != std::wstring::npos ||
           combined.find(L"settings") != std::wstring::npos ||
           combined.find(L"systemsettings") != std::wstring::npos ||
           combined.find(L"immersivecontrolpanel") != std::wstring::npos ||
           combined.find(L"windows.immersivecontrolpanel") != std::wstring::npos ||
           combined.find(L"ms-settings") != std::wstring::npos ||
           combined.find(L"applicationframehost") != std::wstring::npos;
}

void LogIconResolution(
    const ApplicationGroup& app,
    const std::vector<IconCandidate>& iconSources,
    const wchar_t* method,
    const IconCandidate& source) {
    if (!ShouldLogIconDetails(app)) {
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

    log << L"Icon resolution for: " << app.displayName << L" [" << app.id << L"]\n";
    log << L"  launchCommand: " << app.launchCommand << L"\n";
    log << L"  iconPath: " << app.iconPath << L"\n";
    log << L"  shellItemPath: " << app.iconIdentity.shellItemPath << L"\n";
    log << L"  shortcutPath: " << app.iconIdentity.shortcutPath << L"\n";
    log << L"  appUserModelId: " << app.iconIdentity.appUserModelId << L"\n";
    log << L"  iconResourcePath: " << app.iconIdentity.iconResourcePath << L"\n";
    log << L"  iconResourceIndex: " << app.iconIdentity.iconResourceIndex << L"\n";
    log << L"  executablePath: " << app.iconIdentity.executablePath << L"\n";
    log << L"  method: " << method << L"\n";
    LogCandidate(log, L"  selected: ", source);
    for (const auto& candidate : iconSources) {
        LogCandidate(log, L"  candidate: ", candidate);
    }
}

void LogIconAttempt(const ApplicationGroup& app, const std::vector<IconCandidate>& iconSources) {
    LogIconResolution(app, iconSources, L"attempt", {});
}

std::wstring StableIconCacheKey(const ApplicationGroup& app) {
    const auto& icon = app.iconIdentity;
    if (!icon.shellItemPath.empty()) {
        return ToLower(icon.shellItemPath);
    }

    if (!icon.shortcutPath.empty()) {
        return ToLower(icon.shortcutPath);
    }

    if (!icon.appUserModelId.empty()) {
        return ToLower(icon.appUserModelId);
    }

    if (!app.id.empty()) {
        return ToLower(app.id);
    }

    if (!app.iconPath.empty()) {
        return ToLower(app.iconPath);
    }

    if (!app.launchCommand.empty()) {
        return ToLower(app.launchCommand);
    }

    return ToLower(app.displayName);
}

std::wstring ExpandEnvironment(std::wstring value) {
    if (value.empty()) {
        return {};
    }

    const auto lower = ToLower(value);
    constexpr wchar_t kSystemDirToken[] = L"%systemdir%";
    const auto systemDir = lower.find(kSystemDirToken);
    if (systemDir != std::wstring::npos) {
        wchar_t systemDirectory[MAX_PATH] = {};
        if (GetSystemDirectoryW(systemDirectory, _countof(systemDirectory)) > 0) {
            value.replace(systemDir, std::wstring(kSystemDirToken).size(), systemDirectory);
        }
    }

    DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0) {
        return value;
    }

    std::wstring expanded(required, L'\0');
    DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
    if (written == 0 || written > required) {
        return value;
    }

    expanded.resize(written - 1);
    return expanded;
}

std::wstring StripCommandArguments(std::wstring command) {
    command = Trim(std::move(command));
    if (command.empty()) {
        return {};
    }

    std::error_code ec;
    if (std::filesystem::exists(ExpandEnvironment(command), ec)) {
        return command;
    }

    if (command.front() == L'"') {
        const auto endQuote = command.find(L'"', 1);
        if (endQuote != std::wstring::npos) {
            return command.substr(1, endQuote - 1);
        }
    }

    const auto firstSpace = command.find(L' ');
    if (firstSpace == std::wstring::npos) {
        return command;
    }

    const std::wstring possiblePath = command.substr(0, firstSpace);
    if (std::filesystem::exists(ExpandEnvironment(possiblePath), ec)) {
        return possiblePath;
    }

    return command;
}

bool IsShellNamespacePath(const std::wstring& value) {
    const auto lower = ToLower(value);
    return lower.rfind(L"shell:", 0) == 0 || lower.rfind(L"::{", 0) == 0;
}

void AddCandidate(
    std::vector<IconCandidate>& candidates,
    std::set<std::wstring>& seen,
    std::wstring source,
    std::wstring reason = {},
    int iconIndex = 0,
    bool useIconIndex = false) {
    source = IsShellNamespacePath(source)
                 ? Trim(std::move(source))
                 : ExpandEnvironment(StripCommandArguments(std::move(source)));
    if (source.empty()) {
        return;
    }

    const auto key = ToLower(source) + L"|" + std::to_wstring(useIconIndex ? iconIndex : 0);
    if (seen.insert(key).second) {
        IconCandidate candidate;
        candidate.source = std::move(source);
        candidate.iconIndex = iconIndex;
        candidate.useIconIndex = useIconIndex;
        candidate.reason = std::move(reason);
        candidates.push_back(std::move(candidate));
    }
}

bool LooksLikeAppUserModelId(const std::wstring& value) {
    return value.find(L'!') != std::wstring::npos &&
           value.find(L'\\') == std::wstring::npos &&
           value.find(L'/') == std::wstring::npos;
}

std::wstring AppsFolderPathFromAumid(const std::wstring& appUserModelId) {
    if (!LooksLikeAppUserModelId(appUserModelId)) {
        return {};
    }

    return L"shell:AppsFolder\\" + appUserModelId;
}

std::wstring WideFromUtf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    std::wstring result(static_cast<size_t>(required), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), required);
    if (written <= 0) {
        return std::wstring(value.begin(), value.end());
    }

    result.resize(static_cast<size_t>(written - 1));
    return result;
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::wstring WindowsAppsPackageRootFromPath(const std::wstring& source) {
    const auto lower = ToLower(source);
    constexpr wchar_t kWindowsAppsSegment[] = L"\\windowsapps\\";
    const auto segment = lower.find(kWindowsAppsSegment);
    if (segment == std::wstring::npos) {
        return {};
    }

    const size_t packageStart = segment + std::wstring(kWindowsAppsSegment).size();
    const auto packageEnd = source.find_first_of(L"\\/", packageStart);
    if (packageEnd == std::wstring::npos) {
        return {};
    }

    return source.substr(0, packageEnd);
}

std::wstring PackageFamilyNameFromRoot(const std::wstring& packageRoot) {
    const auto fullName = std::filesystem::path(packageRoot).filename().wstring();
    const auto publisherSeparator = fullName.find(L"__");
    const auto firstSeparator = fullName.find(L'_');
    if (publisherSeparator == std::wstring::npos || firstSeparator == std::wstring::npos) {
        return {};
    }

    const auto packageName = fullName.substr(0, firstSeparator);
    const auto publisherId = fullName.substr(publisherSeparator + 2);
    if (packageName.empty() || publisherId.empty()) {
        return {};
    }

    return packageName + L"_" + publisherId;
}

std::vector<std::wstring> ExtractAttributeValues(const std::string& xml, const std::string& attribute) {
    std::vector<std::wstring> values;
    size_t position = 0;

    while (position < xml.size()) {
        const size_t attributePosition = xml.find(attribute, position);
        if (attributePosition == std::string::npos) {
            break;
        }

        size_t valueStart = xml.find_first_of("\"'", attributePosition + attribute.size());
        if (valueStart == std::string::npos) {
            break;
        }

        const char quote = xml[valueStart];
        ++valueStart;
        const size_t valueEnd = xml.find(quote, valueStart);
        if (valueEnd == std::string::npos) {
            break;
        }

        const auto value = WideFromUtf8(xml.substr(valueStart, valueEnd - valueStart));
        if (!value.empty()) {
            values.push_back(value);
        }

        position = valueEnd + 1;
    }

    return values;
}

std::vector<std::wstring> ExtractApplicationIds(const std::string& xml) {
    std::vector<std::wstring> ids;
    size_t position = 0;

    while (position < xml.size()) {
        const size_t appPosition = xml.find("<Application", position);
        if (appPosition == std::string::npos) {
            break;
        }

        const size_t tagEnd = xml.find('>', appPosition);
        if (tagEnd == std::string::npos) {
            break;
        }

        const auto tag = xml.substr(appPosition, tagEnd - appPosition);
        auto tagIds = ExtractAttributeValues(tag, "Id=");
        ids.insert(ids.end(), tagIds.begin(), tagIds.end());
        position = tagEnd + 1;
    }

    if (ids.empty()) {
        ids.push_back(L"App");
    }

    return ids;
}

bool IsImageAssetFile(const std::wstring& value) {
    const auto extension = ToLower(std::filesystem::path(value).extension().wstring());
    return extension == L".png" ||
           extension == L".jpg" ||
           extension == L".jpeg" ||
           extension == L".bmp" ||
           extension == L".ico";
}

int AssetScore(const std::wstring& filename) {
    const auto lower = ToLower(filename);
    int score = 0;

    if (lower.find(L"targetsize-256") != std::wstring::npos) {
        score += 400;
    } else if (lower.find(L"targetsize-192") != std::wstring::npos) {
        score += 350;
    } else if (lower.find(L"targetsize-128") != std::wstring::npos) {
        score += 300;
    } else if (lower.find(L"targetsize-96") != std::wstring::npos) {
        score += 250;
    } else if (lower.find(L"scale-400") != std::wstring::npos) {
        score += 220;
    } else if (lower.find(L"scale-200") != std::wstring::npos) {
        score += 180;
    } else if (lower.find(L"scale-100") != std::wstring::npos) {
        score += 100;
    }

    if (lower.find(L"unplated") != std::wstring::npos) {
        score += 20;
    }

    if (lower.find(L"logo") != std::wstring::npos) {
        score += 10;
    }

    return score;
}

void AddPackageAssetCandidates(
    std::vector<IconCandidate>& candidates,
    std::set<std::wstring>& seen,
    const std::filesystem::path& packageRoot,
    const std::wstring& relativeAsset) {
    if (relativeAsset.empty()) {
        return;
    }

    const auto assetPath = packageRoot / std::filesystem::path(relativeAsset);
    const auto directory = assetPath.parent_path();
    const auto stem = ToLower(assetPath.stem().wstring());
    const auto extension = ToLower(assetPath.extension().wstring());

    std::vector<std::pair<int, std::wstring>> variants;
    std::error_code ec;
    if (!directory.empty() && std::filesystem::exists(directory, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) {
                break;
            }

            if (!entry.is_regular_file(ec)) {
                continue;
            }

            const auto path = entry.path();
            if (ToLower(path.extension().wstring()) != extension) {
                continue;
            }

            const auto candidateStem = ToLower(path.stem().wstring());
            if (candidateStem.rfind(stem, 0) == 0 && IsImageAssetFile(path.wstring())) {
                variants.emplace_back(AssetScore(path.filename().wstring()), path.wstring());
            }
        }
    }

    std::sort(variants.begin(), variants.end(), [](const auto& left, const auto& right) {
        if (left.first != right.first) {
            return left.first > right.first;
        }

        return ToLower(left.second) < ToLower(right.second);
    });

    for (const auto& [_, path] : variants) {
        AddCandidate(candidates, seen, path, L"package asset");
    }

    AddCandidate(candidates, seen, assetPath.wstring(), L"package asset");
}

void AddGenericPackageLogoCandidates(
    std::vector<IconCandidate>& candidates,
    std::set<std::wstring>& seen,
    const std::filesystem::path& packageRoot) {
    const auto assetsFolder = packageRoot / L"Assets";
    std::error_code ec;
    if (!std::filesystem::exists(assetsFolder, ec)) {
        return;
    }

    std::vector<std::pair<int, std::wstring>> assets;
    for (const auto& entry : std::filesystem::directory_iterator(assetsFolder, ec)) {
        if (ec) {
            break;
        }

        if (!entry.is_regular_file(ec) || !IsImageAssetFile(entry.path().wstring())) {
            continue;
        }

        const auto filename = ToLower(entry.path().filename().wstring());
        if (filename.find(L"logo") == std::wstring::npos) {
            continue;
        }

        assets.emplace_back(AssetScore(filename), entry.path().wstring());
    }

    std::sort(assets.begin(), assets.end(), [](const auto& left, const auto& right) {
        if (left.first != right.first) {
            return left.first > right.first;
        }

        return ToLower(left.second) < ToLower(right.second);
    });

    size_t added = 0;
    for (const auto& [_, path] : assets) {
        AddCandidate(candidates, seen, path, L"package logo");
        if (++added >= 12) {
            break;
        }
    }
}

void AddWindowsAppsPackageCandidates(
    std::vector<IconCandidate>& candidates,
    std::set<std::wstring>& seen,
    const std::wstring& source) {
    const auto packageRootText = WindowsAppsPackageRootFromPath(source);
    if (packageRootText.empty()) {
        return;
    }

    const auto packageRoot = std::filesystem::path(packageRootText);
    const auto packageFamilyName = PackageFamilyNameFromRoot(packageRootText);
    const auto manifest = ReadTextFile(packageRoot / L"AppxManifest.xml");

    if (!packageFamilyName.empty()) {
        for (const auto& appId : ExtractApplicationIds(manifest)) {
            AddCandidate(candidates, seen, L"shell:AppsFolder\\" + packageFamilyName + L"!" + appId, L"package AUMID");
        }
    }

    if (!manifest.empty()) {
        for (const auto& attribute : {"Square44x44Logo=", "Square150x150Logo=", "Logo=", "SmallLogo="}) {
            for (const auto& logo : ExtractAttributeValues(manifest, attribute)) {
                AddPackageAssetCandidates(candidates, seen, packageRoot, logo);
            }
        }
    }

    AddGenericPackageLogoCandidates(candidates, seen, packageRoot);
}

bool MatchesText(const ApplicationGroup& app, const std::wstring& text) {
    const auto combined = ToLower(
        app.displayName + L" " +
        app.id + L" " +
        app.launchCommand + L" " +
        app.iconPath + L" " +
        app.iconIdentity.shellItemPath + L" " +
        app.iconIdentity.shortcutPath + L" " +
        app.iconIdentity.appUserModelId + L" " +
        app.iconIdentity.iconResourcePath + L" " +
        app.iconIdentity.executablePath);
    return combined.find(text) != std::wstring::npos;
}

void AddKnownShellAppCandidates(
    std::vector<IconCandidate>& candidates,
    std::set<std::wstring>& seen,
    const ApplicationGroup& app) {
    // Only genuine Windows Settings identities. A bare "settings" substring would also hit
    // unrelated apps (e.g. "Display Settings.lnk", "...\SettingsSync\app.exe") and give them the
    // Windows Settings icon ahead of their own.
    if (MatchesText(app, L"immersivecontrolpanel") ||
        MatchesText(app, L"ms-settings:") ||
        MatchesText(app, L"systemsettings.exe")) {
        AddCandidate(
            candidates,
            seen,
            L"shell:AppsFolder\\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel",
            L"known Settings AUMID");
        AddCandidate(
            candidates,
            seen,
            L"%SystemRoot%\\ImmersiveControlPanel\\SystemSettings.exe",
            L"known Settings executable");
        AddCandidate(
            candidates,
            seen,
            L"%SystemRoot%\\SystemApps\\Microsoft.Windows.ImmersiveControlPanel_cw5n1h2txyewy\\SystemSettings.exe",
            L"known Settings executable");

        const std::filesystem::path immersiveRoot = ExpandEnvironment(L"%SystemRoot%\\ImmersiveControlPanel");
        const std::filesystem::path systemAppRoot =
            ExpandEnvironment(L"%SystemRoot%\\SystemApps\\Microsoft.Windows.ImmersiveControlPanel_cw5n1h2txyewy");

        AddPackageAssetCandidates(candidates, seen, immersiveRoot, L"Assets\\Square44x44Logo.png");
        AddPackageAssetCandidates(candidates, seen, immersiveRoot, L"Assets\\Square150x150Logo.png");
        AddGenericPackageLogoCandidates(candidates, seen, immersiveRoot);

        AddPackageAssetCandidates(candidates, seen, systemAppRoot, L"Assets\\Square44x44Logo.png");
        AddPackageAssetCandidates(candidates, seen, systemAppRoot, L"Assets\\Square150x150Logo.png");
        AddGenericPackageLogoCandidates(candidates, seen, systemAppRoot);
    }
}

std::wstring SearchExecutableOnPath(const std::wstring& executable) {
    wchar_t buffer[MAX_PATH] = {};
    wchar_t* filePart = nullptr;
    const DWORD length = SearchPathW(nullptr, executable.c_str(), nullptr, _countof(buffer), buffer, &filePart);
    if (length > 0 && length < _countof(buffer)) {
        return buffer;
    }

    return {};
}

std::wstring QueryAppPath(HKEY root, const std::wstring& executable) {
    const std::wstring subkey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + executable;
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = sizeof(buffer);
    DWORD type = REG_SZ;

    if (RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ, &type, buffer, &size) == ERROR_SUCCESS) {
        return buffer;
    }

    return {};
}

std::wstring ResolveShortcutTarget(const std::wstring& shortcutPath) {
    if (ToLower(std::filesystem::path(shortcutPath).extension().wstring()) != L".lnk") {
        return {};
    }

    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shellLink.GetAddressOf())))) {
        return {};
    }

    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink.As(&persistFile)) || FAILED(persistFile->Load(shortcutPath.c_str(), STGM_READ))) {
        return {};
    }

    wchar_t target[MAX_PATH] = {};
    if (FAILED(shellLink->GetPath(target, _countof(target), nullptr, SLGP_RAWPATH))) {
        return {};
    }

    return target;
}

IconCandidate ResolveShortcutIconLocation(const std::wstring& shortcutPath) {
    if (ToLower(std::filesystem::path(shortcutPath).extension().wstring()) != L".lnk") {
        return {};
    }

    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shellLink.GetAddressOf())))) {
        return {};
    }

    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink.As(&persistFile)) || FAILED(persistFile->Load(shortcutPath.c_str(), STGM_READ))) {
        return {};
    }

    wchar_t iconLocation[MAX_PATH] = {};
    int iconIndex = 0;
    if (FAILED(shellLink->GetIconLocation(iconLocation, _countof(iconLocation), &iconIndex)) || iconLocation[0] == L'\0') {
        return {};
    }

    IconCandidate candidate;
    candidate.source = iconLocation;
    candidate.iconIndex = iconIndex;
    candidate.useIconIndex = true;
    candidate.reason = L"shortcut icon location";
    return candidate;
}

std::vector<IconCandidate> ResolveIconSources(const ApplicationGroup& app) {
    std::vector<IconCandidate> candidates;
    std::set<std::wstring> seen;
    const auto& icon = app.iconIdentity;

    AddCandidate(candidates, seen, icon.shellItemPath, L"IconIdentity shell item");
    AddCandidate(candidates, seen, AppsFolderPathFromAumid(icon.appUserModelId), L"IconIdentity AUMID");
    AddCandidate(candidates, seen, AppsFolderPathFromAumid(app.id), L"application id AUMID");
    AddCandidate(candidates, seen, icon.shortcutPath, L"IconIdentity shortcut");
    AddKnownShellAppCandidates(candidates, seen, app);
    AddCandidate(
        candidates,
        seen,
        icon.iconResourcePath,
        L"IconIdentity icon resource",
        icon.iconResourceIndex,
        !icon.iconResourcePath.empty());
    AddCandidate(candidates, seen, icon.executablePath, L"IconIdentity executable");
    AddWindowsAppsPackageCandidates(candidates, seen, app.iconPath);
    AddWindowsAppsPackageCandidates(candidates, seen, app.launchCommand);
    AddCandidate(candidates, seen, app.iconPath, L"legacy iconPath");
    AddCandidate(candidates, seen, app.launchCommand, L"legacy launchCommand");

    const std::wstring executableName = BasenameFromPath(StripCommandArguments(app.launchCommand));
    if (!executableName.empty()) {
        AddCandidate(candidates, seen, SearchExecutableOnPath(executableName), L"PATH search");
        AddCandidate(candidates, seen, QueryAppPath(HKEY_CURRENT_USER, executableName), L"HKCU App Paths");
        AddCandidate(candidates, seen, QueryAppPath(HKEY_LOCAL_MACHINE, executableName), L"HKLM App Paths");

        wchar_t windowsDirectory[MAX_PATH] = {};
        if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) > 0) {
            AddCandidate(candidates, seen, (std::filesystem::path(windowsDirectory) / executableName).wstring(), L"Windows directory");
        }
    }

    const size_t initialCount = candidates.size();
    for (size_t index = 0; index < initialCount; ++index) {
        const auto& candidate = candidates[index];
        std::error_code ec;
        if (!candidate.source.empty() && std::filesystem::exists(candidate.source, ec)) {
            const auto shortcutIcon = ResolveShortcutIconLocation(candidate.source);
            AddCandidate(
                candidates,
                seen,
                shortcutIcon.source,
                shortcutIcon.reason,
                shortcutIcon.iconIndex,
                shortcutIcon.useIconIndex);

            const auto shortcutTarget = ResolveShortcutTarget(candidate.source);
            AddCandidate(candidates, seen, shortcutTarget, L"shortcut target");
        }
    }

    return candidates;
}

HICON IconFromShellItemImageFactory(const std::wstring& iconSource, int size) {
    if (iconSource.empty()) {
        return nullptr;
    }

    std::error_code ec;
    if (!IsShellNamespacePath(iconSource) && !std::filesystem::exists(iconSource, ec)) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(iconSource.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf())))) {
        PIDLIST_ABSOLUTE itemIdList = nullptr;
        SFGAOF attributes = 0;
        if (FAILED(SHParseDisplayName(iconSource.c_str(), nullptr, &itemIdList, 0, &attributes)) || !itemIdList) {
            return nullptr;
        }

        const HRESULT itemResult = SHCreateItemFromIDList(itemIdList, IID_PPV_ARGS(item.GetAddressOf()));
        CoTaskMemFree(itemIdList);
        if (FAILED(itemResult)) {
            return nullptr;
        }
    }

    Microsoft::WRL::ComPtr<IShellItemImageFactory> imageFactory;
    if (FAILED(item.As(&imageFactory))) {
        return nullptr;
    }

    HBITMAP bitmap = nullptr;
    SIZE requestedSize = {size, size};
    constexpr auto kShellImageScaleUp = static_cast<SIIGBF>(0x00000100);
    auto flags = static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK | kShellImageScaleUp);
    if (IsImageAssetFile(iconSource)) {
        flags = static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | kShellImageScaleUp);
    }
    if (FAILED(imageFactory->GetImage(requestedSize, flags, &bitmap)) || !bitmap) {
        return nullptr;
    }

    BITMAP bitmapInfo = {};
    GetObjectW(bitmap, sizeof(bitmapInfo), &bitmapInfo);
    HBITMAP mask = CreateBitmap(bitmapInfo.bmWidth, bitmapInfo.bmHeight, 1, 1, nullptr);
    ICONINFO iconInfo = {};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = bitmap;
    iconInfo.hbmMask = mask;

    HICON icon = CreateIconIndirect(&iconInfo);
    if (mask) {
        DeleteObject(mask);
    }
    DeleteObject(bitmap);
    return icon;
}

HICON IconFromImageFile(const std::wstring& iconSource, int size) {
    if (iconSource.empty()) {
        return nullptr;
    }

    std::error_code ec;
    if (!std::filesystem::exists(iconSource, ec)) {
        return nullptr;
    }

    const auto extension = ToLower(std::filesystem::path(iconSource).extension().wstring());
    if (extension != L".ico") {
        return nullptr;
    }

    return static_cast<HICON>(LoadImageW(nullptr, iconSource.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
}

HICON IconFromShellFileInfo(const std::wstring& iconSource) {
    if (iconSource.empty()) {
        return nullptr;
    }

    SHFILEINFOW fileInfo = {};
    if (SHGetFileInfoW(iconSource.c_str(), FILE_ATTRIBUTE_NORMAL, &fileInfo, sizeof(fileInfo), SHGFI_ICON | SHGFI_LARGEICON)) {
        return fileInfo.hIcon;
    }

    return nullptr;
}

HICON IconFromExtractIcon(const IconCandidate& candidate) {
    const auto& iconSource = candidate.source;
    if (iconSource.empty()) {
        return nullptr;
    }

    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    if (ExtractIconExW(iconSource.c_str(), candidate.useIconIndex ? candidate.iconIndex : 0, &largeIcon, &smallIcon, 1) > 0) {
        if (largeIcon && smallIcon) {
            DestroyIcon(smallIcon);
        }
        return largeIcon ? largeIcon : smallIcon;
    }

    return nullptr;
}

HICON ResolveIcon(const std::vector<IconCandidate>& iconSources, IconCandidate* resolvedSource, const wchar_t** resolvedMethod) {
    for (const auto& candidate : iconSources) {
        if (candidate.useIconIndex) {
            continue;
        }

        if (HICON icon = IconFromShellItemImageFactory(candidate.source, 256)) {
            if (resolvedSource) {
                *resolvedSource = candidate;
            }
            if (resolvedMethod) {
                *resolvedMethod = L"IShellItemImageFactory";
            }
            return icon;
        }
    }

    for (const auto& candidate : iconSources) {
        if (candidate.useIconIndex) {
            continue;
        }

        if (HICON icon = IconFromImageFile(candidate.source, 256)) {
            if (resolvedSource) {
                *resolvedSource = candidate;
            }
            if (resolvedMethod) {
                *resolvedMethod = L"LoadImage ico";
            }
            return icon;
        }
    }

    for (const auto& candidate : iconSources) {
        if (!candidate.useIconIndex) {
            continue;
        }

        if (HICON icon = IconFromExtractIcon(candidate)) {
            if (resolvedSource) {
                *resolvedSource = candidate;
            }
            if (resolvedMethod) {
                *resolvedMethod = L"ExtractIconEx indexed";
            }
            return icon;
        }
    }

    for (const auto& candidate : iconSources) {
        if (candidate.useIconIndex) {
            continue;
        }

        if (HICON icon = IconFromShellFileInfo(candidate.source)) {
            if (resolvedSource) {
                *resolvedSource = candidate;
            }
            if (resolvedMethod) {
                *resolvedMethod = L"SHGetFileInfo";
            }
            return icon;
        }
    }

    for (const auto& candidate : iconSources) {
        if (candidate.useIconIndex) {
            continue;
        }

        if (HICON icon = IconFromExtractIcon(candidate)) {
            if (resolvedSource) {
                *resolvedSource = candidate;
            }
            if (resolvedMethod) {
                *resolvedMethod = L"ExtractIconEx";
            }
            return icon;
        }
    }

    return nullptr;
}

} // namespace

IconCache::~IconCache() {
    Clear();
}

HICON IconCache::IconFor(const ApplicationGroup& app) {
    const std::wstring key = StableIconCacheKey(app);
    const auto existing = icons_.find(key);
    if (existing != icons_.end()) {
        return existing->second;
    }

    const auto iconSources = ResolveIconSources(app);
    LogIconAttempt(app, iconSources);

    IconCandidate resolvedSource;
    const wchar_t* resolvedMethod = L"default";
    HICON icon = ResolveIcon(iconSources, &resolvedSource, &resolvedMethod);
    if (!icon) {
        LogIconFallback(app, iconSources);
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    LogIconResolution(app, iconSources, resolvedMethod, resolvedSource);

    icons_[key] = icon;
    return icon;
}

void IconCache::Clear() {
    for (auto& [_, icon] : icons_) {
        if (icon && icon != LoadIconW(nullptr, IDI_APPLICATION)) {
            DestroyIcon(icon);
        }
    }

    icons_.clear();
}

} // namespace MyDock

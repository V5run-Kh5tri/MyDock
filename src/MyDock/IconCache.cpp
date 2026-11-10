#include "IconCache.h"

#include "Log.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <wrl/client.h>

#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace MyDock {
namespace {

// Largest size the dock draws is 80 px; 128 px keeps headroom for scaling without holding
// 256 px bitmaps for every app.
constexpr int kIconSize = 128;

struct IconCandidate {
    std::wstring source;
    int iconIndex = 0;
    bool useIconIndex = false;
    const wchar_t* reason = L"";
};

std::wstring StableIconCacheKey(const ApplicationGroup& app) {
    const auto& icon = app.iconIdentity;
    for (const std::wstring* value : {&icon.shellItemPath, &icon.shortcutPath, &icon.appUserModelId, &app.id, &app.iconPath, &app.launchCommand}) {
        if (!value->empty()) {
            return ToLower(*value);
        }
    }
    return ToLower(app.displayName);
}

std::wstring ExpandEnvironment(std::wstring value) {
    if (value.empty()) {
        return {};
    }

    // Shell icon resources (e.g. PKEY_AppUserModel_RelaunchIconResource) use %systemdir%, which
    // is not a real environment variable.
    constexpr wchar_t kSystemDirToken[] = L"%systemdir%";
    const auto systemDir = ToLower(value).find(kSystemDirToken);
    if (systemDir != std::wstring::npos) {
        wchar_t systemDirectory[MAX_PATH] = {};
        if (GetSystemDirectoryW(systemDirectory, _countof(systemDirectory)) > 0) {
            value.replace(systemDir, _countof(kSystemDirToken) - 1, systemDirectory);
        }
    }

    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0) {
        return value;
    }

    std::wstring expanded(required, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
    if (written == 0 || written > required) {
        return value;
    }

    expanded.resize(written - 1);
    return expanded;
}

bool PathExists(const std::wstring& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}

std::wstring StripCommandArguments(std::wstring command) {
    command = Trim(std::move(command));
    if (command.empty() || PathExists(ExpandEnvironment(command))) {
        return command;
    }

    if (command.front() == L'"') {
        const auto endQuote = command.find(L'"', 1);
        if (endQuote != std::wstring::npos) {
            return command.substr(1, endQuote - 1);
        }
    }

    const auto firstSpace = command.find(L' ');
    if (firstSpace != std::wstring::npos) {
        std::wstring possiblePath = command.substr(0, firstSpace);
        if (PathExists(ExpandEnvironment(possiblePath))) {
            return possiblePath;
        }
    }

    return command;
}

bool IsShellNamespacePath(const std::wstring& value) {
    const auto lower = ToLower(value);
    return lower.rfind(L"shell:", 0) == 0 || lower.rfind(L"::{", 0) == 0;
}

bool IsImageFile(const std::wstring& value) {
    const auto extension = ToLower(std::filesystem::path(value).extension().wstring());
    return extension == L".png" || extension == L".jpg" || extension == L".jpeg" || extension == L".bmp" || extension == L".ico";
}

class CandidateList {
public:
    void Add(std::wstring source, const wchar_t* reason, int iconIndex = 0, bool useIconIndex = false) {
        source = IsShellNamespacePath(source) ? Trim(std::move(source)) : ExpandEnvironment(StripCommandArguments(std::move(source)));
        if (source.empty()) {
            return;
        }

        const auto key = ToLower(source) + L"|" + std::to_wstring(useIconIndex ? iconIndex : 0);
        if (seen_.insert(key).second) {
            candidates_.push_back({std::move(source), iconIndex, useIconIndex, reason});
        }
    }

    std::vector<IconCandidate>& Items() {
        return candidates_;
    }

private:
    std::vector<IconCandidate> candidates_;
    std::set<std::wstring> seen_;
};

std::wstring AppsFolderPathFromAumid(const std::wstring& appUserModelId) {
    const bool packaged = appUserModelId.find(L'!') != std::wstring::npos &&
                          appUserModelId.find(L'\\') == std::wstring::npos &&
                          appUserModelId.find(L'/') == std::wstring::npos;
    return packaged ? L"shell:AppsFolder\\" + appUserModelId : std::wstring{};
}

std::wstring SearchExecutableOnPath(const std::wstring& executable) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = SearchPathW(nullptr, executable.c_str(), nullptr, _countof(buffer), buffer, nullptr);
    return length > 0 && length < _countof(buffer) ? std::wstring(buffer) : std::wstring{};
}

std::wstring QueryAppPath(HKEY root, const std::wstring& executable) {
    const std::wstring subkey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + executable;
    wchar_t buffer[MAX_PATH] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, subkey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
        return buffer;
    }
    return {};
}

struct ShortcutInfo {
    std::wstring target;
    std::wstring iconLocation;
    int iconIndex = 0;
};

ShortcutInfo ReadShortcut(const std::wstring& shortcutPath) {
    ShortcutInfo info;
    if (ToLower(std::filesystem::path(shortcutPath).extension().wstring()) != L".lnk") {
        return info;
    }

    Microsoft::WRL::ComPtr<IShellLinkW> shellLink;
    Microsoft::WRL::ComPtr<IPersistFile> persistFile;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shellLink.GetAddressOf()))) ||
        FAILED(shellLink.As(&persistFile)) ||
        FAILED(persistFile->Load(shortcutPath.c_str(), STGM_READ))) {
        return info;
    }

    wchar_t buffer[MAX_PATH] = {};
    if (SUCCEEDED(shellLink->GetPath(buffer, _countof(buffer), nullptr, SLGP_RAWPATH))) {
        info.target = buffer;
    }

    buffer[0] = L'\0';
    if (SUCCEEDED(shellLink->GetIconLocation(buffer, _countof(buffer), &info.iconIndex))) {
        info.iconLocation = buffer;
    }
    return info;
}

// Ordered from the most shell-native identity to the most generic fallback.
std::vector<IconCandidate> ResolveIconSources(const ApplicationGroup& app) {
    CandidateList list;
    const auto& icon = app.iconIdentity;

    list.Add(icon.shellItemPath, L"shell item");
    list.Add(AppsFolderPathFromAumid(icon.appUserModelId), L"AUMID");
    list.Add(AppsFolderPathFromAumid(app.id), L"id AUMID");
    list.Add(icon.shortcutPath, L"shortcut");
    list.Add(icon.iconResourcePath, L"icon resource", icon.iconResourceIndex, !icon.iconResourcePath.empty());
    list.Add(icon.executablePath, L"executable");
    list.Add(app.iconPath, L"iconPath");
    list.Add(app.launchCommand, L"launchCommand");

    const std::wstring executableName = BasenameFromPath(StripCommandArguments(app.launchCommand));
    if (!executableName.empty()) {
        list.Add(SearchExecutableOnPath(executableName), L"PATH");
        list.Add(QueryAppPath(HKEY_CURRENT_USER, executableName), L"HKCU App Paths");
        list.Add(QueryAppPath(HKEY_LOCAL_MACHINE, executableName), L"HKLM App Paths");

        wchar_t windowsDirectory[MAX_PATH] = {};
        if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) > 0) {
            list.Add((std::filesystem::path(windowsDirectory) / executableName).wstring(), L"Windows directory");
        }
    }

    // Shortcuts contribute their own icon location and target as further candidates.
    auto& candidates = list.Items();
    const size_t initialCount = candidates.size();
    for (size_t index = 0; index < initialCount; ++index) {
        const std::wstring source = candidates[index].source;
        if (IsShellNamespacePath(source) || !PathExists(source)) {
            continue;
        }

        const auto shortcut = ReadShortcut(source);
        list.Add(shortcut.iconLocation, L"shortcut icon", shortcut.iconIndex, !shortcut.iconLocation.empty());
        list.Add(shortcut.target, L"shortcut target");
    }

    return std::move(list.Items());
}

HICON IconFromShellItemImageFactory(const IconCandidate& candidate) {
    const auto& source = candidate.source;
    if (!IsShellNamespacePath(source) && !PathExists(source)) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf())))) {
        PIDLIST_ABSOLUTE itemIdList = nullptr;
        if (FAILED(SHParseDisplayName(source.c_str(), nullptr, &itemIdList, 0, nullptr)) || !itemIdList) {
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

    constexpr auto kScaleUp = static_cast<SIIGBF>(0x00000100); // SIIGBF_SCALEUP (newer SDKs only)
    const auto flags = static_cast<SIIGBF>(
        (IsImageFile(source) ? 0 : SIIGBF_ICONONLY) | SIIGBF_BIGGERSIZEOK | kScaleUp);
    HBITMAP bitmap = nullptr;
    if (FAILED(imageFactory->GetImage({kIconSize, kIconSize}, flags, &bitmap)) || !bitmap) {
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

HICON IconFromIcoFile(const IconCandidate& candidate) {
    const auto& source = candidate.source;
    if (ToLower(std::filesystem::path(source).extension().wstring()) != L".ico" || !PathExists(source)) {
        return nullptr;
    }
    return static_cast<HICON>(LoadImageW(nullptr, source.c_str(), IMAGE_ICON, kIconSize, kIconSize, LR_LOADFROMFILE));
}

HICON IconFromShellFileInfo(const IconCandidate& candidate) {
    SHFILEINFOW fileInfo = {};
    if (SHGetFileInfoW(candidate.source.c_str(), FILE_ATTRIBUTE_NORMAL, &fileInfo, sizeof(fileInfo), SHGFI_ICON | SHGFI_LARGEICON)) {
        return fileInfo.hIcon;
    }
    return nullptr;
}

HICON IconFromExtractIcon(const IconCandidate& candidate) {
    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    if (ExtractIconExW(candidate.source.c_str(), candidate.useIconIndex ? candidate.iconIndex : 0, &largeIcon, &smallIcon, 1) > 0) {
        if (largeIcon && smallIcon) {
            DestroyIcon(smallIcon);
        }
        return largeIcon ? largeIcon : smallIcon;
    }
    return nullptr;
}

// Tries every candidate with the best method first, then every candidate with the next method, so
// a high-quality icon from a later candidate beats a low-resolution one from an earlier candidate.
HICON ResolveIcon(const std::vector<IconCandidate>& candidates, const IconCandidate** resolvedSource, const wchar_t** resolvedMethod) {
    struct Method {
        HICON (*load)(const IconCandidate&);
        bool indexedCandidates;
        const wchar_t* name;
    };
    static constexpr Method kMethods[] = {
        {IconFromShellItemImageFactory, false, L"IShellItemImageFactory"},
        {IconFromIcoFile, false, L"LoadImage ico"},
        {IconFromExtractIcon, true, L"ExtractIconEx indexed"},
        {IconFromShellFileInfo, false, L"SHGetFileInfo"},
        {IconFromExtractIcon, false, L"ExtractIconEx"},
    };

    for (const auto& method : kMethods) {
        for (const auto& candidate : candidates) {
            if (candidate.useIconIndex != method.indexedCandidates || candidate.source.empty()) {
                continue;
            }
            if (HICON icon = method.load(candidate)) {
                *resolvedSource = &candidate;
                *resolvedMethod = method.name;
                return icon;
            }
        }
    }

    return nullptr;
}

bool IsSharedDefaultIcon(HICON icon) {
    return icon == LoadIconW(nullptr, IDI_APPLICATION);
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

    const auto candidates = ResolveIconSources(app);
    const IconCandidate* source = nullptr;
    const wchar_t* method = L"default";
    HICON icon = ResolveIcon(candidates, &source, &method);
    if (!icon) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    MYDOCK_LOG_DEBUG(
        L"Icon for " + app.displayName + L" [" + app.id + L"]: " + method +
        (source ? L" <- " + source->source + L" (" + source->reason + L")" : std::wstring(L" (no candidate worked)")));

    icons_[key] = icon;
    return icon;
}

std::vector<HICON> IconCache::Retain(const std::vector<ApplicationGroup>& apps) {
    std::set<std::wstring> live;
    for (const auto& app : apps) {
        live.insert(StableIconCacheKey(app));
    }

    std::vector<HICON> removed;
    for (auto it = icons_.begin(); it != icons_.end();) {
        if (live.count(it->first) != 0) {
            ++it;
            continue;
        }

        if (it->second && !IsSharedDefaultIcon(it->second)) {
            removed.push_back(it->second);
            DestroyIcon(it->second);
        }
        it = icons_.erase(it);
    }
    return removed;
}

void IconCache::Clear() {
    for (auto& [_, icon] : icons_) {
        if (icon && !IsSharedDefaultIcon(icon)) {
            DestroyIcon(icon);
        }
    }
    icons_.clear();
}

} // namespace MyDock

#include "IconCache.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <wrl/client.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

namespace MyDock {
namespace {

void LogIconFallback(const ApplicationGroup& app, const std::vector<std::wstring>& iconSources) {
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
        log << L"  source: " << source << L"\n";
    }
}

std::wstring ExpandEnvironment(std::wstring value) {
    if (value.empty()) {
        return {};
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

void AddCandidate(std::vector<std::wstring>& candidates, std::set<std::wstring>& seen, std::wstring candidate) {
    candidate = ExpandEnvironment(StripCommandArguments(std::move(candidate)));
    if (candidate.empty()) {
        return;
    }

    const auto key = ToLower(candidate);
    if (seen.insert(key).second) {
        candidates.push_back(std::move(candidate));
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

std::vector<std::wstring> ResolveIconSources(const ApplicationGroup& app) {
    std::vector<std::wstring> candidates;
    std::set<std::wstring> seen;
    AddCandidate(candidates, seen, app.iconPath);
    AddCandidate(candidates, seen, app.launchCommand);

    const std::wstring executableName = BasenameFromPath(StripCommandArguments(app.launchCommand));
    if (!executableName.empty()) {
        AddCandidate(candidates, seen, SearchExecutableOnPath(executableName));
        AddCandidate(candidates, seen, QueryAppPath(HKEY_CURRENT_USER, executableName));
        AddCandidate(candidates, seen, QueryAppPath(HKEY_LOCAL_MACHINE, executableName));

        wchar_t windowsDirectory[MAX_PATH] = {};
        if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) > 0) {
            AddCandidate(candidates, seen, (std::filesystem::path(windowsDirectory) / executableName).wstring());
        }
    }

    const size_t initialCount = candidates.size();
    for (size_t index = 0; index < initialCount; ++index) {
        const auto& candidate = candidates[index];
        std::error_code ec;
        if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
            const auto shortcutTarget = ResolveShortcutTarget(candidate);
            AddCandidate(candidates, seen, shortcutTarget);
        }
    }

    return candidates;
}

HICON IconFromShellItemImageFactory(const std::wstring& iconSource, int size) {
    if (iconSource.empty()) {
        return nullptr;
    }

    std::error_code ec;
    if (!std::filesystem::exists(iconSource, ec)) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(iconSource.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf())))) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IShellItemImageFactory> imageFactory;
    if (FAILED(item.As(&imageFactory))) {
        return nullptr;
    }

    HBITMAP bitmap = nullptr;
    SIZE requestedSize = {size, size};
    const auto flags = static_cast<SIIGBF>(SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK);
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

HICON IconFromExtractIcon(const std::wstring& iconSource) {
    if (iconSource.empty()) {
        return nullptr;
    }

    HICON largeIcon = nullptr;
    HICON smallIcon = nullptr;
    if (ExtractIconExW(iconSource.c_str(), 0, &largeIcon, &smallIcon, 1) > 0) {
        if (largeIcon && smallIcon) {
            DestroyIcon(smallIcon);
        }
        return largeIcon ? largeIcon : smallIcon;
    }

    return nullptr;
}

HICON ResolveIcon(const std::vector<std::wstring>& iconSources) {
    for (const auto& iconSource : iconSources) {
        if (HICON icon = IconFromShellItemImageFactory(iconSource, 256)) {
            return icon;
        }
    }

    for (const auto& iconSource : iconSources) {
        if (HICON icon = IconFromShellFileInfo(iconSource)) {
            return icon;
        }
    }

    for (const auto& iconSource : iconSources) {
        if (HICON icon = IconFromExtractIcon(iconSource)) {
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
    const auto iconSources = ResolveIconSources(app);
    const std::wstring key = ToLower(!iconSources.empty() ? iconSources.front() : app.id);
    const auto existing = icons_.find(key);
    if (existing != icons_.end()) {
        return existing->second;
    }

    HICON icon = ResolveIcon(iconSources);
    if (!icon) {
        LogIconFallback(app, iconSources);
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }

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

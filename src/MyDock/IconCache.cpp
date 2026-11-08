#include "IconCache.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <wrl/client.h>

#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

namespace MyDock {
namespace {

std::wstring StripCommandArguments(std::wstring command) {
    command = Trim(std::move(command));
    if (command.empty()) {
        return {};
    }

    if (command.front() == L'"') {
        const auto endQuote = command.find(L'"', 1);
        if (endQuote != std::wstring::npos) {
            return command.substr(1, endQuote - 1);
        }
    }

    const auto firstSpace = command.find(L' ');
    return firstSpace == std::wstring::npos ? command : command.substr(0, firstSpace);
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

std::wstring ResolveIconSource(const ApplicationGroup& app) {
    std::vector<std::wstring> candidates = {
        app.iconPath,
        app.launchCommand,
    };

    const std::wstring executableName = BasenameFromPath(StripCommandArguments(app.launchCommand));
    if (!executableName.empty()) {
        candidates.push_back(SearchExecutableOnPath(executableName));
        candidates.push_back(QueryAppPath(HKEY_CURRENT_USER, executableName));
        candidates.push_back(QueryAppPath(HKEY_LOCAL_MACHINE, executableName));

        wchar_t windowsDirectory[MAX_PATH] = {};
        if (GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) > 0) {
            candidates.push_back((std::filesystem::path(windowsDirectory) / executableName).wstring());
        }
    }

    for (auto candidate : candidates) {
        candidate = StripCommandArguments(candidate);
        std::error_code ec;
        if (!candidate.empty() && std::filesystem::exists(candidate, ec)) {
            const auto shortcutTarget = ResolveShortcutTarget(candidate);
            return shortcutTarget.empty() ? candidate : shortcutTarget;
        }
    }

    return StripCommandArguments(app.iconPath.empty() ? app.launchCommand : app.iconPath);
}

HICON ExtractIconFallback(const std::wstring& iconSource) {
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

} // namespace

IconCache::~IconCache() {
    Clear();
}

HICON IconCache::IconFor(const ApplicationGroup& app) {
    const std::wstring iconSource = ResolveIconSource(app);
    const std::wstring key = ToLower(iconSource.empty() ? app.id : iconSource);
    const auto existing = icons_.find(key);
    if (existing != icons_.end()) {
        return existing->second;
    }

    SHFILEINFOW fileInfo = {};
    HICON icon = nullptr;

    if (!iconSource.empty()) {
        if (SHGetFileInfoW(iconSource.c_str(), FILE_ATTRIBUTE_NORMAL, &fileInfo, sizeof(fileInfo), SHGFI_ICON | SHGFI_LARGEICON)) {
            icon = fileInfo.hIcon;
        }
    }

    if (!icon) {
        icon = ExtractIconFallback(iconSource);
    }

    if (!icon) {
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

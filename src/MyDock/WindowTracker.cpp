#include "WindowTracker.h"

#include <Dwmapi.h>
#include <Propkey.h>
#include <Psapi.h>
#include <ShObjIdl.h>
#include <Shellapi.h>
#include <Shlwapi.h>
#include <appmodel.h>
#include <wrl/client.h>

#include <cwchar>
#include <cwctype>
#include <map>
#include <utility>

namespace MyDock {
namespace {

std::wstring DisplayNameFromIdentity(const std::wstring& identity) {
    std::wstring displayName = identity;
    const auto dot = displayName.rfind(L".exe");
    if (dot != std::wstring::npos) {
        displayName.erase(dot);
    }

    if (!displayName.empty()) {
        displayName[0] = static_cast<wchar_t>(std::towupper(displayName[0]));
    }

    return displayName;
}

// Packaged AUMIDs have the form "<PackageFamilyName>!<ApplicationId>". Desktop apps may also put
// free-form AUMIDs on their windows (e.g. "Chrome"); those are ignored so the existing exe-based
// identities, pins and hidden-pin entries keep matching.
bool IsPackagedAppUserModelId(const std::wstring& value) {
    return value.find(L'!') != std::wstring::npos &&
           value.find(L'\\') == std::wstring::npos &&
           value.find(L'/') == std::wstring::npos;
}

bool IsApplicationFrameHost(const std::wstring& imagePath) {
    return IdentityFromExecutable(imagePath) == L"applicationframehost.exe";
}

std::wstring WindowAppUserModelId(HWND hwnd) {
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(properties.GetAddressOf())))) {
        return {};
    }

    PROPVARIANT value = {};
    PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    }
    PropVariantClear(&value);
    return result;
}

std::wstring ProcessAppUserModelId(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return {};
    }

    std::wstring result;
    UINT32 length = 0;
    if (GetApplicationUserModelId(process, &length, nullptr) == ERROR_INSUFFICIENT_BUFFER && length > 0) {
        std::wstring buffer(length, L'\0');
        if (GetApplicationUserModelId(process, &length, buffer.data()) == ERROR_SUCCESS) {
            buffer.resize(std::wcslen(buffer.c_str()));
            result = std::move(buffer);
        }
    }

    CloseHandle(process);
    return result;
}

struct HostedCoreWindowSearch {
    DWORD hostProcessId = 0;
    DWORD appProcessId = 0;
};

BOOL CALLBACK FindHostedCoreWindow(HWND child, LPARAM lParam) {
    auto* search = reinterpret_cast<HostedCoreWindowSearch*>(lParam);

    wchar_t className[64] = {};
    if (GetClassNameW(child, className, _countof(className)) == 0 ||
        std::wcscmp(className, L"Windows.UI.Core.CoreWindow") != 0) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(child, &processId);
    if (processId != 0 && processId != search->hostProcessId) {
        search->appProcessId = processId;
        return FALSE;
    }

    return TRUE;
}

// UWP apps (Settings, Calculator, ...) render inside a frame window owned by
// ApplicationFrameHost.exe, so the window's process says nothing about which app it is.
// Resolve the packaged AUMID the way the shell does: the window's AppUserModel property first,
// then the package identity of the process (for UWP, the process behind the hosted CoreWindow).
std::wstring ResolvePackagedAppUserModelId(
    HWND hwnd,
    DWORD processId,
    const std::wstring& imagePath,
    std::wstring* appImagePath) {
    auto appUserModelId = WindowAppUserModelId(hwnd);
    if (!IsPackagedAppUserModelId(appUserModelId)) {
        appUserModelId.clear();
    }

    DWORD appProcessId = processId;
    if (IsApplicationFrameHost(imagePath)) {
        HostedCoreWindowSearch search;
        search.hostProcessId = processId;
        EnumChildWindows(hwnd, FindHostedCoreWindow, reinterpret_cast<LPARAM>(&search));
        appProcessId = search.appProcessId;
    }

    if (appProcessId != 0) {
        if (appUserModelId.empty()) {
            appUserModelId = ProcessAppUserModelId(appProcessId);
            if (!IsPackagedAppUserModelId(appUserModelId)) {
                appUserModelId.clear();
            }
        }

        if (appImagePath && appProcessId != processId) {
            *appImagePath = WindowTracker::ProcessImagePath(appProcessId);
        }
    }

    return appUserModelId;
}

BOOL CALLBACK CollectCandidateWindow(HWND hwnd, LPARAM lParam) {
    if (WindowTracker::IsAltTabCandidate(hwnd)) {
        reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
    }
    return TRUE;
}

} // namespace

std::vector<ApplicationGroup> WindowTracker::Snapshot() const {
    std::vector<HWND> windows;
    EnumWindows(CollectCandidateWindow, reinterpret_cast<LPARAM>(&windows));

    const DWORD currentProcessId = GetCurrentProcessId();
    std::map<std::wstring, ApplicationGroup> groups;
    std::unordered_map<HWND, WindowIdentity> seen;

    for (const HWND hwnd : windows) {
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId == 0 || processId == currentProcessId) {
            continue;
        }

        // HWND values can be reused, so a cached identity is only trusted for the same process.
        WindowIdentity identity;
        const auto cached = windowIdentities_.find(hwnd);
        if (cached != windowIdentities_.end() && cached->second.processId == processId) {
            identity = cached->second;
        } else {
            identity.processId = processId;
            identity.imagePath = ProcessImagePath(processId);
            if (identity.imagePath.empty()) {
                continue;
            }
            identity.appImagePath = identity.imagePath;
            identity.appUserModelId = ResolvePackagedAppUserModelId(hwnd, processId, identity.imagePath, &identity.appImagePath);
        }

        // A UWP frame may not host its app's CoreWindow yet; retry those on the next snapshot.
        if (!(identity.appUserModelId.empty() && IsApplicationFrameHost(identity.imagePath))) {
            seen[hwnd] = identity;
        }

        const auto& appUserModelId = identity.appUserModelId;
        const auto& imagePath = identity.imagePath;
        const auto groupId = appUserModelId.empty() ? IdentityFromExecutable(imagePath) : ToLower(appUserModelId);
        auto& group = groups[groupId];
        if (group.id.empty()) {
            group.id = groupId;
            if (appUserModelId.empty()) {
                group.displayName = DisplayNameFromIdentity(groupId);
                group.launchCommand = imagePath;
                group.iconPath = imagePath;
                group.iconIdentity.executablePath = imagePath;
                group.iconIdentity.iconResourcePath = imagePath;
            } else {
                const std::wstring appsFolderPath = L"shell:AppsFolder\\" + appUserModelId;
                group.displayName = PackagedDisplayNameFor(appUserModelId);
                if (group.displayName.empty()) {
                    group.displayName = WindowTitle(hwnd);
                }
                group.launchCommand = appsFolderPath;
                group.iconPath = appsFolderPath;
                group.iconIdentity.shellItemPath = appsFolderPath;
                group.iconIdentity.appUserModelId = appUserModelId;
                // Never use ApplicationFrameHost.exe as an icon source: it has a generic icon.
                if (!identity.appImagePath.empty() && !IsApplicationFrameHost(identity.appImagePath)) {
                    group.iconIdentity.executablePath = identity.appImagePath;
                }
            }
        }

        WindowInfo window;
        window.hwnd = hwnd;
        window.title = WindowTitle(hwnd);
        window.minimized = IsIconic(hwnd) != FALSE;
        group.windows.push_back(std::move(window));
    }

    // Forget windows that are gone.
    windowIdentities_ = std::move(seen);

    std::vector<ApplicationGroup> result;
    result.reserve(groups.size());
    for (auto& [_, group] : groups) {
        result.push_back(std::move(group));
    }
    return result;
}

std::wstring WindowTracker::PackagedDisplayNameFor(const std::wstring& appUserModelId) const {
    const auto key = ToLower(appUserModelId);
    const auto cached = packagedDisplayNames_.find(key);
    if (cached != packagedDisplayNames_.end()) {
        return cached->second;
    }

    std::wstring displayName;
    Microsoft::WRL::ComPtr<IShellItem> item;
    const std::wstring appsFolderPath = L"shell:AppsFolder\\" + appUserModelId;
    if (SUCCEEDED(SHCreateItemFromParsingName(appsFolderPath.c_str(), nullptr, IID_PPV_ARGS(item.GetAddressOf())))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
            displayName = name;
            CoTaskMemFree(name);
        }
    }

    packagedDisplayNames_[key] = displayName;
    return displayName;
}

bool WindowTracker::IsAltTabCandidate(HWND hwnd) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;
    }

    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return false;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }

    return !WindowTitle(hwnd).empty();
}

std::wstring WindowTracker::WindowTitle(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(static_cast<size_t>(length));
    return Trim(title);
}

std::wstring WindowTracker::ProcessImagePath(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return {};
    }

    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
        CloseHandle(process);
        return {};
    }

    CloseHandle(process);
    path.resize(size);
    return path;
}

} // namespace MyDock

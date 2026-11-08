#include "WindowTracker.h"

#include <Dwmapi.h>
#include <Psapi.h>
#include <Shlwapi.h>

#include <cwctype>
#include <map>
#include <utility>

namespace MyDock {
namespace {

struct EnumState {
    DWORD currentProcessId = GetCurrentProcessId();
    std::map<std::wstring, ApplicationGroup> groups;
};

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

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* state = reinterpret_cast<EnumState*>(lParam);

    if (!WindowTracker::IsAltTabCandidate(hwnd)) {
        return TRUE;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0 || processId == state->currentProcessId) {
        return TRUE;
    }

    const auto imagePath = WindowTracker::ProcessImagePath(processId);
    if (imagePath.empty()) {
        return TRUE;
    }

    const auto identity = IdentityFromExecutable(imagePath);
    auto& group = state->groups[identity];
    if (group.id.empty()) {
        group.id = identity;
        group.displayName = DisplayNameFromIdentity(identity);
        group.launchCommand = imagePath;
        group.iconPath = imagePath;
    }

    WindowInfo window;
    window.hwnd = hwnd;
    window.title = WindowTracker::WindowTitle(hwnd);
    window.minimized = IsIconic(hwnd) != FALSE;
    group.windows.push_back(std::move(window));
    return TRUE;
}

} // namespace

std::vector<ApplicationGroup> WindowTracker::Snapshot() const {
    EnumState state;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&state));

    std::vector<ApplicationGroup> result;
    result.reserve(state.groups.size());

    for (auto& [_, group] : state.groups) {
        result.push_back(std::move(group));
    }

    return result;
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

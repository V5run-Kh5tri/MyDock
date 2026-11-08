#include "Models.h"

#include <Shlwapi.h>

#include <algorithm>
#include <cwctype>

namespace MyDock {

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring Trim(std::wstring value) {
    const auto isSpace = [](wchar_t ch) {
        return std::iswspace(ch) != 0;
    };

    while (!value.empty() && isSpace(value.front())) {
        value.erase(value.begin());
    }

    while (!value.empty() && isSpace(value.back())) {
        value.pop_back();
    }

    return value;
}

std::wstring BasenameFromPath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    const wchar_t* fileName = PathFindFileNameW(path.c_str());
    return fileName ? std::wstring(fileName) : path;
}

std::wstring IdentityFromExecutable(const std::wstring& executable) {
    std::wstring identity = ToLower(BasenameFromPath(executable));
    if (identity.empty()) {
        identity = ToLower(executable);
    }
    return identity;
}

} // namespace MyDock


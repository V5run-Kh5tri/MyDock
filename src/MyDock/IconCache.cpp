#include "IconCache.h"

#include <Shellapi.h>
#include <ShlObj.h>

namespace MyDock {

IconCache::~IconCache() {
    Clear();
}

HICON IconCache::IconFor(const ApplicationGroup& app) {
    const std::wstring key = ToLower(app.iconPath.empty() ? app.launchCommand : app.iconPath);
    const auto existing = icons_.find(key);
    if (existing != icons_.end()) {
        return existing->second;
    }

    SHFILEINFOW fileInfo = {};
    HICON icon = nullptr;

    const std::wstring iconSource = key.empty() ? app.launchCommand : key;
    if (!iconSource.empty()) {
        if (SHGetFileInfoW(iconSource.c_str(), 0, &fileInfo, sizeof(fileInfo), SHGFI_ICON | SHGFI_LARGEICON)) {
            icon = fileInfo.hIcon;
        }
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


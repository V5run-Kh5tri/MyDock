#pragma once

#include "Models.h"

#include <Windows.h>

#include <map>
#include <string>
#include <vector>

namespace MyDock {

class IconCache {
public:
    IconCache() = default;
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    HICON IconFor(const ApplicationGroup& app);

    // Drops icons for apps that are no longer in the dock. Returns the removed handles (already
    // destroyed) so callers can evict anything keyed by them.
    std::vector<HICON> Retain(const std::vector<ApplicationGroup>& apps);
    void Clear();

private:
    std::map<std::wstring, HICON> icons_;
};

} // namespace MyDock

#pragma once

#include "Models.h"

#include <Windows.h>

#include <map>
#include <string>

namespace MyDock {

class IconCache {
public:
    IconCache() = default;
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    HICON IconFor(const ApplicationGroup& app);
    void Clear();

private:
    std::map<std::wstring, HICON> icons_;
};

} // namespace MyDock


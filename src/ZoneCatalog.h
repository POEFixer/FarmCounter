#pragma once
#include <algorithm>
#include <span>
#include <string_view>

namespace ZoneCatalog {
struct Entry {
    const char* id;
    const char* name;
    const char* category;
    bool isMapArea;
    bool isTown;
    bool isHideout;
    bool isUnused;
};
} // namespace ZoneCatalog

#include "ZoneCatalog.generated.h"

namespace ZoneCatalog {
inline std::span<const Entry> Entries() { return kEntries; }

inline const Entry* Lookup(std::string_view id) {
    const auto entries = Entries();
    const auto found = std::lower_bound(entries.begin(), entries.end(), id,
        [](const Entry& entry, std::string_view value) { return std::string_view(entry.id) < value; });
    return found != entries.end() && found->id == id ? &*found : nullptr;
}
} // namespace ZoneCatalog

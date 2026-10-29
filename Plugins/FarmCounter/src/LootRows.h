#pragma once
#include "FarmTypes.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

// The backpack items without a price as the last read priced them (by the item itself on POEFixer
// 371+, custom prices included), sorted by name: the Settings tab's "Unpriced items in inventory".
// Never asked again by the shown name: an item priced by what it is is not unpriced.
inline std::vector<std::string> UnpricedNames(const std::unordered_map<std::string, InvSnapshot>& snap) {
    std::vector<std::string> names;
    for (const auto& [key, item] : snap)
        if (!(std::isfinite(item.chaosEach) && item.chaosEach > 0.f)) names.push_back(item.name);
    std::sort(names.begin(), names.end());
    return names;
}

struct VisibleLootRows {
    std::vector<const LootEntry*> rows;
    size_t hidden = 0;
};
inline VisibleLootRows SelectLootRows(const std::vector<LootEntry>& loot, bool showUnpriced, int limit) {
    VisibleLootRows selected;
    auto value = [](const LootEntry* e) {
        return std::isfinite(e->chaosEach) && e->chaosEach > 0.f
            ? static_cast<double>(e->chaosEach) * e->stackCount : 0.0;
    };
    for (const auto& entry : loot)
        if (showUnpriced || (std::isfinite(entry.chaosEach) && entry.chaosEach > 0.f))
            selected.rows.push_back(&entry);
    std::stable_sort(selected.rows.begin(), selected.rows.end(), [&](const LootEntry* a, const LootEntry* b) {
        if (value(a) != value(b)) return value(a) > value(b);
        return a->name < b->name;
    });
    const size_t count = static_cast<size_t>(std::clamp(limit, 1, 50));
    if (selected.rows.size() > count) {
        selected.hidden = selected.rows.size() - count;
        selected.rows.resize(count);
    }
    return selected;
}

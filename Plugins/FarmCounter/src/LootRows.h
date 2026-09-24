#pragma once
#include "FarmTypes.h"
#include <algorithm>
#include <cmath>
#include <vector>

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

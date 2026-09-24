#pragma once
// LootDiff.h — pure loot diff/merge. std-only.
#include "FarmTypes.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

inline std::vector<LootEntry> DiffLoot(
    const std::unordered_map<std::string, InvSnapshot>& baseline,
    const std::unordered_map<std::string, InvSnapshot>& current,
    const std::vector<LootEntry>& carryover)
{
    std::unordered_map<std::string, LootEntry> merged;
    for (const auto& e : carryover) merged[e.name] = e;

    std::unordered_set<std::string> allNames;
    for (const auto& kv : current)  allNames.insert(kv.first);
    for (const auto& kv : baseline) allNames.insert(kv.first);

    for (const auto& name : allNames) {
        int curStack = 0, baseStack = 0;
        float chaosEach = 0.0f; int rarity = 0;
        std::string iconPath;
        auto curIt  = current.find(name);
        auto baseIt = baseline.find(name);
        if (curIt != current.end())  { curStack = curIt->second.stackCount;  chaosEach = curIt->second.chaosEach;  rarity = curIt->second.rarity; iconPath = curIt->second.iconPath; }
        if (baseIt != baseline.end()){ baseStack = baseIt->second.stackCount; if (chaosEach==0.0f) chaosEach = baseIt->second.chaosEach; if (!rarity) rarity = baseIt->second.rarity; if (iconPath.empty()) iconPath = baseIt->second.iconPath; }
        int diff = curStack - baseStack;
        auto slotIt = merged.find(name);
        // diff==0 still matters for an existing carryover slot: it may have been
        // captured while the price DB was loading (chaosEach 0) — refresh its
        // price/icon from the freshly priced snapshots instead of skipping.
        if (diff == 0 && slotIt == merged.end()) continue;
        LootEntry& slot = (slotIt != merged.end()) ? slotIt->second : merged[name];
        slot.name = name; slot.stackCount += diff;
        if (rarity) slot.rarity = rarity;
        if (slot.chaosEach == 0.0f) slot.chaosEach = chaosEach;
        if (slot.iconPath.empty()) slot.iconPath = iconPath;
    }

    std::vector<LootEntry> out;
    for (const auto& kv : merged) if (kv.second.stackCount != 0) out.push_back(kv.second);
    return out;
}

// True when two inventory snapshots hold the same items by NAME and STACKCOUNT.
// The game fills the backpack item list a delay after map-enter (~10-15s), so the
// first non-empty read can be a PARTIAL inventory. A baseline is only locked once two
// consecutive reads agree, else late-arriving items would mis-count as loot.
inline bool SnapshotStable(const std::unordered_map<std::string, InvSnapshot>& a,
                           const std::unordered_map<std::string, InvSnapshot>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& kv : a) {
        auto it = b.find(kv.first);
        if (it == b.end() || it->second.stackCount != kv.second.stackCount) return false;
    }
    return true;
}

#include "LootScanner.h"
#include <unordered_set>
#include <algorithm>

using Clock = std::chrono::steady_clock;

// Build a name->InvSnapshot map from the player backpack. Ported from the old
// monolith's BuildSnapshot (FarmCounter.cpp:1686-1717); the price block now goes
// through PriceProvider::Lookup (core price service + local custom-price fallback)
// instead of the deleted in-plugin price DB + custom map.
std::unordered_map<std::string, InvSnapshot>
LootScanner::BuildSnapshot(const PluginSDK::Inventory& inv) {
    std::unordered_map<std::string, InvSnapshot> snap;
    std::unordered_set<uintptr_t> seen;
    for (const auto& item : inv.Items) {
        if (item.Address == 0) continue;
        if (!seen.insert(item.Address).second) continue;
        std::string name;
        if (!item.UniqueName.empty())        name = item.UniqueName;
        else if (!item.BaseTypeName.empty()) name = item.BaseTypeName;
        else continue;
        int stack  = (item.StackCount > 1) ? item.StackCount : 1;
        int rarity = std::clamp(item.Rarity, 0, 3);
        auto& s = snap[name];
        s.name         = name;
        s.uniqueName   = item.UniqueName;
        s.baseTypeName = item.BaseTypeName;
        s.stackCount  += stack;
        s.rarity       = rarity;
        if (s.chaosEach == 0.0f && m_prices) {
            auto p = m_prices->Lookup(name);
            // The host price DB aliases each base type to the most expensive
            // unique on that base (e.g. "Utility Belt" -> Mageblood, "Sapphire"
            // -> Voices). Such a unique-category price is valid ONLY for an
            // actually-Unique item; a Normal/Magic/Rare base sharing the type
            // would otherwise inherit it. Gate on true rarity: a resolved unique
            // name already proves Unique (snapshot item.Rarity is 0 on the
            // enumerate hot path), else read it from the Mods component — and
            // only when a unique-category price actually hit, so non-unique loot
            // (currency/runes/gems) never pays for a memory read.
            if (p.found && PriceProvider::IsUniqueCategory(p.category)) {
                bool isUnique = !item.UniqueName.empty();
                if (!isUnique && m_ctx && item.Address)
                    isUnique = (m_ctx->Inventory.ReadItemRarity(item.Address) == 3);
                if (!isUnique) p.found = false;
            }
            if (p.found) {
                s.chaosEach = p.chaos;
                s.iconPath  = p.iconPath;  // resolved by the same lookup (render does zero price lookups); only for a kept price so a white base never shows the unique's icon
            }
        }
    }
    return snap;
}

// Two-phase targeted scan: phase 1 issues the request (Scan(1)) and arms m_pending;
// phase 2 (>= 50ms later) reads the result and builds the snapshot. Throttle is
// 1000ms (once per second) in all phases. Ported from the old ScanInventory
// request/read loop (FarmCounter.cpp:1715-1738), with the host scan id changed
// from -1 to 1 (THE FIX).
bool LootScanner::RequestAndRead(std::unordered_map<std::string, InvSnapshot>& out) {
    auto now = Clock::now();
    if (!m_pending) {
        constexpr int interval = 1000;
        if (now - m_lastScan < std::chrono::milliseconds(interval)) return false;
        if (m_ctx) m_ctx->Inventory.Scan(1);
        m_lastScan = now;
        m_pending  = true;
        return false;
    }
    if (now - m_lastScan < std::chrono::milliseconds(50)) return false;
    m_pending = false;
    uint64_t stamp = 0;
    if (!ReadPublished(out, true, &stamp)) return false;
    m_lastConsumedStamp = stamp;
    return true;
}

bool LootScanner::ReadCurrent(std::unordered_map<std::string, InvSnapshot>& out, uint64_t* scanStamp) {
    return ReadPublished(out, false, scanStamp);
}

bool LootScanner::ReadPublished(std::unordered_map<std::string, InvSnapshot>& out, bool requireNew, uint64_t* scanStamp) {
    if (!m_ctx) return false;
    auto coherent = m_ctx->Inventory.ReadSnapshot(1);
    std::vector<PluginSDK::Inventory> legacy;
    const PluginSDK::Inventory* player = nullptr;
    if (coherent.Supported) {
        if (!coherent.Ready || coherent.ScanStamp == 0 ||
            !m_hasExpectedArea || coherent.AreaChangeCounter != m_expectedAreaCounter ||
            (requireNew && coherent.ScanStamp == m_lastConsumedStamp)) return false;
        player = &coherent.Value;
    } else {
        // Old hosts do not expose ItemsRead/current-area/completed-scan identity.
        // Retain their nonempty compatibility, but never certify ambiguous empty
        // metadata as a baseline. No separate readiness/GetAll race on new hosts.
        legacy = m_ctx->Inventory.GetAll();
        for (const auto& inv : legacy)
            if (inv.InventoryId == 1) { player = &inv; break; }
        if (!player || player->Items.empty()) return false;
    }
    // Zero dimensions describe unloaded inventories. Grid.Valid describes panel
    // visibility and must not gate a closed backpack. The coherent host producer
    // additionally verifies actual items were read for this published area.
    if (!player || !player->Address || player->TotalBoxesX <= 0 || player->TotalBoxesY <= 0 ||
        player->TotalBoxesX > 1024 || player->TotalBoxesY > 1024 ||
        static_cast<int64_t>(player->TotalBoxesX) * player->TotalBoxesY > 65536) return false;
    out = BuildSnapshot(*player);
    if (!coherent.Supported && out.empty()) return false;
    if (scanStamp) *scanStamp = coherent.Supported ? coherent.ScanStamp : 0;
    return true;
}

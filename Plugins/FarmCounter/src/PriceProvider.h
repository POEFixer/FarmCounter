#pragma once
// PriceProvider.h — thin façade over the host price service (ctx()->Prices) plus
// user custom-price overrides. The plugin performs NO HTTP and owns NO price DB:
// the core app loads poe2scout prices once per session (league + refresh are
// configured in POEFixer Settings) and caches item icons to local PNG files.
//
// Lookup priority:
//   1. Custom prices (user-set, always trusted; stored in EXALTS, converted to
//      chaos at lookup via the live exalt rate)
//   2. Host price DB (fuzzy host-side match; LootScanner applies the
//      IsUniqueCategory rarity gate on top)
#include "../../../POEFixer/plugin_sdk/PluginSDK.h"
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>

class PriceProvider {
public:
    // iconPath is the host-cached local PNG ("" until its background download
    // finishes — callers re-resolve every scan, so icons self-heal).
    struct Priced { bool found = false; float chaos = 0.0f; std::string category; std::string iconPath; };

    void SetContext(const PluginSDK::Context* ctx) { m_ctx = ctx; }

    Priced Lookup(const std::string& name) const {
        // 1. Custom prices (stored in exalts, converted to chaos on lookup). The
        //    icon still comes from the host when it knows the item (an override
        //    of a host-priced item keeps its icon; a truly unknown item has none).
        auto cit = m_custom.find(ToLower(name));
        if (cit != m_custom.end()) {
            std::string icon = m_ctx ? m_ctx->Prices.LookupPrice(name).iconPath : std::string();
            return { true, cit->second * ExaltedInChaos(), {}, std::move(icon) };
        }

        // 2. Host price DB (one host call resolves price + icon together).
        if (m_ctx) {
            auto r = m_ctx->Prices.LookupPrice(name);
            if (r.found) return { true, r.chaos, r.category, std::move(r.iconPath) };
        }
        return {};
    }

    // Rates clamped to >= 1 so display conversion never divides by zero while
    // the core DB is still loading.
    float ExaltedInChaos() const {
        const float v = m_ctx ? m_ctx->Prices.GetRates().exaltedInChaos : 0.f;
        return v > 0.0001f ? v : 1.0f;
    }
    float DivineInChaos() const {
        const float v = m_ctx ? m_ctx->Prices.GetRates().divineInChaos : 0.f;
        return v > 0.0001f ? v : 1.0f;
    }

    PluginSDK::PriceStatus Status() const {
        return m_ctx ? m_ctx->Prices.GetStatus() : PluginSDK::PriceStatus{};
    }

    // Custom price maps (read/write from the Settings tab, persisted by
    // Persistence.cpp). All three are keyed by ToLower(display name).
    std::unordered_map<std::string, float>&       Custom()      { return m_custom; }
    std::unordered_map<std::string, std::string>& CustomNames() { return m_customName; }
    std::unordered_map<std::string, std::string>& CustomBase()  { return m_customBase; }

    static std::string ToLower(const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return r;
    }

    // Host DB categories whose prices belong to UNIQUE items only. The host
    // aliases each base type to the most expensive unique on that base, so a
    // price from these categories must not be applied to a white/magic/rare
    // base — LootScanner verifies true rarity before keeping it.
    static bool IsUniqueCategory(const std::string& category) {
        static const char* const kUnique[] = {
            "accessory", "armour", "flask", "jewel", "map", "weapon", "sanctum" };
        const std::string c = ToLower(category);
        for (const char* u : kUnique) if (c == u) return true;
        return false;
    }

private:
    const PluginSDK::Context* m_ctx = nullptr;

    std::unordered_map<std::string, float>       m_custom;      // key -> exalts
    std::unordered_map<std::string, std::string> m_customName;  // key -> display name
    std::unordered_map<std::string, std::string> m_customBase;  // key -> base type
};

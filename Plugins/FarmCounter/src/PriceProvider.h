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
//      IsUniqueCategory rarity gate on top). Backpack items go through
//      LookupItem: the host prices the item's identity, not its localized name.
#include "../../../POEFixer/plugin_sdk/PluginSDK.h"
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cstdint>

// The host's by-item answers for PriceProvider::LookupItem (desktop 371+), by item address and
// shown name: the backpack is read once a second on the render thread, and every answer is a
// component walk on the host. Forgotten when the area changes (its items are re-created) or the
// host publishes a new price revision; an item the host could not read is never kept (asked
// again on the next read). LootScanner owns one.
class ItemQuoteCache {
public:
    struct Quote {
        std::string english;            // the English name the host priced the item by
        PluginSDK::PriceResult price;
    };
    static constexpr size_t kMax = 4096;

    // Once per read, before the lookups: forgets every answer when the area or the price revision moved.
    void Sync(uint64_t areaCounter, uint64_t priceRevision) {
        if (areaCounter == m_area && priceRevision == m_revision && m_quotes.size() < kMax) return;
        m_area = areaCounter;
        m_revision = priceRevision;
        m_quotes.clear();
    }
    Quote* Find(uintptr_t item, const std::string& name) {
        const auto it = m_quotes.find(Key(item, name));
        return it == m_quotes.end() ? nullptr : &it->second;
    }
    void Store(uintptr_t item, const std::string& name, Quote quote) {
        if (quote.english.empty()) return;   // not read: asked again next time
        m_quotes[Key(item, name)] = std::move(quote);
    }
    size_t Size() const { return m_quotes.size(); }

private:
    static std::string Key(uintptr_t item, const std::string& name) {
        std::string key = std::to_string(item);
        key += '\n';
        key += name;
        return key;
    }
    std::unordered_map<std::string, Quote> m_quotes;
    uint64_t m_area = ~0ull, m_revision = ~0ull;
};

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

    // The price of an item the backpack holds, by its identity (desktop 371+): the host prices
    // the item's English name (its unique, unidentified-unique art or base type), so a game in
    // any language gets the same price as an English one. `name` is the name the game shows
    // (custom prices are keyed by it; a custom price on the English name counts too). On an
    // older host, or when the host could not read the item, this is Lookup(name). `cache` (when
    // given) keeps the host's answers (ItemQuoteCache); custom prices are applied afresh.
    Priced LookupItem(uintptr_t item, const std::string& name, ItemQuoteCache* cache = nullptr) const {
        if (m_custom.count(ToLower(name)) || !m_ctx || !item || !m_ctx->Prices.CanLookupByItem())
            return Lookup(name);
        ItemQuoteCache::Quote* known = cache ? cache->Find(item, name) : nullptr;
        ItemQuoteCache::Quote fresh;
        if (!known) {
            fresh.price = m_ctx->Prices.LookupPriceByItem(item, &fresh.english);
            if (fresh.english.empty()) return Lookup(name);   // the item could not be read
            if (cache) {
                cache->Store(item, name, fresh);
                known = cache->Find(item, name);
            }
        } else if (known->price.found && known->price.iconPath.empty()) {
            // The host caches icons in the background: a kept answer picks its icon up later.
            known->price.iconPath = m_ctx->Prices.LookupPrice(known->english).iconPath;
        }
        const ItemQuoteCache::Quote& q = known ? *known : fresh;
        auto cit = m_custom.find(ToLower(q.english));
        if (cit != m_custom.end()) return { true, cit->second * ExaltedInChaos(), {}, q.price.iconPath };
        if (q.price.found) return { true, q.price.chaos, q.price.category, q.price.iconPath };
        return {};   // read and unpriced: never a guess by the shown name
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

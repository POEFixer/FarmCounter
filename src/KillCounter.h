#pragma once
#include "sdk/PluginSDK.h"
#include <unordered_map>
#include <cstdint>

class KillCounter {
public:
    void Update(const PluginSDK::Snapshot& snap);
    void Reset() { m_normal = m_magic = m_rare = m_unique = m_rogue = 0; m_prev.clear(); }
    int Normal() const { return m_normal; }
    int Magic()  const { return m_magic; }
    int Rare()   const { return m_rare; }
    int Unique() const { return m_unique; }
    int Rogue()  const { return m_rogue; }   // Rogue Exiles (counted apart from Unique)
    int Total()  const { return m_normal + m_magic + m_rare + m_unique + m_rogue; }

private:
    struct Tracked { PluginSDK::NearbyZone zone = PluginSDK::NearbyZone::None; int rarity = 0; bool rogue = false; };
    int m_normal = 0, m_magic = 0, m_rare = 0, m_unique = 0, m_rogue = 0;
    uint64_t m_lastAreaCounter = 0;
    std::unordered_map<uint32_t, Tracked> m_prev;
};

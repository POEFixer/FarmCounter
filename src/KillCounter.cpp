#include "KillCounter.h"
#include <unordered_set>

void KillCounter::Update(const PluginSDK::Snapshot& snap) {
    if (snap.AreaChangeCounter != m_lastAreaCounter) {
        m_lastAreaCounter = snap.AreaChangeCounter;
        m_normal = m_magic = m_rare = m_unique = 0;
        m_prev.clear();
    }
    if (snap.IsTown || snap.IsHideout) return;

    std::unordered_set<uint32_t> currentIds;
    for (const auto& e : snap.Entities) {
        if (e.EntityType != PluginSDK::EntityType::Monster) continue;
        if (e.EntityState == PluginSDK::EntityState::MonsterFriendly) continue;
        currentIds.insert(e.Id);
        auto& t = m_prev[e.Id]; t.zone = e.Zone; t.rarity = e.Rarity;
    }
    for (auto it = m_prev.begin(); it != m_prev.end(); ) {
        if (currentIds.find(it->first) == currentIds.end()) {
            if (it->second.zone == PluginSDK::NearbyZone::InnerCircle ||
                it->second.zone == PluginSDK::NearbyZone::OuterCircle) {
                switch (it->second.rarity) {
                    case 1:  m_magic++;  break;
                    case 2:  m_rare++;   break;
                    case 3:  m_unique++; break;
                    default: m_normal++; break;
                }
            }
            it = m_prev.erase(it);
        } else { ++it; }
    }
}

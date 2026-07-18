#include "KillCounter.h"
#include <unordered_set>

namespace {
// Case-insensitive substring test — the snapshot entity path case isn't
// guaranteed, and Rogue Exiles use "Metadata/Monsters/RogueExiles/...".
bool PathHas(const std::wstring& path, const wchar_t* needleLower) {
    const size_t nlen = [&]{ size_t n = 0; while (needleLower[n]) ++n; return n; }();
    if (nlen == 0 || path.size() < nlen) return false;
    for (size_t i = 0; i + nlen <= path.size(); ++i) {
        size_t j = 0;
        for (; j < nlen; ++j) {
            wchar_t c = path[i + j];
            if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
            if (c != needleLower[j]) break;
        }
        if (j == nlen) return true;
    }
    return false;
}
}

void KillCounter::Update(const PluginSDK::Snapshot& snap) {
    if (snap.AreaChangeCounter != m_lastAreaCounter) {
        m_lastAreaCounter = snap.AreaChangeCounter;
        m_normal = m_magic = m_rare = m_unique = m_rogue = 0;
        m_prev.clear();
    }
    if (snap.IsTown || snap.IsHideout) return;

    std::unordered_set<uint32_t> currentIds;
    for (const auto& e : snap.Entities) {
        if (e.EntityType != PluginSDK::EntityType::Monster) continue;
        if (e.EntityState == PluginSDK::EntityState::MonsterFriendly) continue;
        currentIds.insert(e.Id);
        auto& t = m_prev[e.Id];
        t.zone = e.Zone; t.rarity = e.Rarity;
        // Latch rogue once true — a dying entity's path may be missing on the
        // final frame it's seen, so never clear a previously-detected flag.
        if (!t.rogue && PathHas(e.Path, L"rogueexiles")) t.rogue = true;
    }
    for (auto it = m_prev.begin(); it != m_prev.end(); ) {
        if (currentIds.find(it->first) == currentIds.end()) {
            if (it->second.zone == PluginSDK::NearbyZone::InnerCircle ||
                it->second.zone == PluginSDK::NearbyZone::OuterCircle) {
                if (it->second.rogue) {
                    m_rogue++;   // Rogue Exiles tracked apart from Unique
                } else {
                    switch (it->second.rarity) {
                        case 1:  m_magic++;  break;
                        case 2:  m_rare++;   break;
                        case 3:  m_unique++; break;
                        default: m_normal++; break;
                    }
                }
            }
            it = m_prev.erase(it);
        } else { ++it; }
    }
}

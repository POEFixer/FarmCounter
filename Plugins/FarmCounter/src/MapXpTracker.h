#pragma once
#include <cstdint>
#include <string>
#include <string_view>

class MapXpTracker {
public:
    void Clear() { *this = MapXpTracker{}; }
    // This identity belongs to XP, independently of inventory baselines or runs.
    // Return true on an area boundary so the caller can take a fresh XP sample.
    bool ObserveArea(std::string_view hash, bool isMap, bool isPassThrough) {
        const bool changed = hash != m_areaHash;
        m_areaHash = hash;
        if (hash.empty()) { m_active = false; m_baselineReady = false; return changed; }
        // Some subareas (Delirium_HungerBoss) match BOTH classifiers. Preserve
        // the parent first, matching FarmTracker's pass-through precedence.
        if (isMap && !isPassThrough && hash != m_mapHash) {
            m_mapHash = hash;
            m_gainedXp = 0;
            m_hasSample = false;
            m_baselineReady = false;
        }
        const bool active = isPassThrough ? !m_mapHash.empty() : isMap;
        if (!active || !m_active) m_baselineReady = false;
        m_active = active;
        return changed;
    }
    void Sample(std::string_view character, uint32_t xp, uint8_t level) {
        if (character.empty() || level == 0 || level > 100 || (level > 1 && xp == 0)) return;
        if (character != m_character || level < m_level) {
            m_character = character;
            m_gainedXp = 0;
            m_hasSample = false;
            m_baselineReady = false;
        }
        if (m_active) {
            if (m_baselineReady)
                m_gainedXp += static_cast<int64_t>(xp) - static_cast<int64_t>(m_lastXp);
            m_baselineReady = true;
            m_hasSample = true;
        }
        m_lastXp = xp;
        m_level = level;
    }
    bool HasSample() const { return m_hasSample; }
    int64_t GainedXp() const { return m_gainedXp; }
private:
    std::string m_mapHash, m_areaHash, m_character;
    uint32_t m_lastXp = 0;
    uint8_t m_level = 0;
    int64_t m_gainedXp = 0;
    bool m_active = false, m_baselineReady = false, m_hasSample = false;
};

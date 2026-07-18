#pragma once
// FarmTypes.h — pure POD types + field (de)serialization. std-only (no Windows/ImGui/SDK).
#include <string>
#include <vector>

struct LootEntry {
    std::string name;
    int         stackCount = 0;
    float       chaosEach  = 0.0f;
    int         rarity     = 0;
    std::string iconPath;        // scan-time-resolved icon path (not persisted to map_history)
};

struct MapRun {
    std::string            mapName;
    int                    durationSec   = 0;
    float                  totalChaos    = 0.0f;
    float                  exaltedRate   = 1.0f;
    std::vector<LootEntry> loot;
    bool                   archived      = false;
    int                    sessionId     = 0;
    int                    hivebloodGain = 0;
};

struct InvSnapshot {
    std::string name;
    std::string uniqueName;
    std::string baseTypeName;
    int         stackCount = 0;
    float       chaosEach  = 0.0f;
    int         rarity     = 0;
    std::string iconPath;        // scan-time-resolved icon path (carried onto LootEntry by DiffLoot)
};

// Escape '\' and '|' so a name can be stored in a '|'-delimited field.
inline std::string EscapeField(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) { if (c == '\\') r += "\\\\"; else if (c == '|') r += "\\|"; else r += c; }
    return r;
}
inline std::string UnescapeField(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { r += s[++i]; } else r += s[i];
    }
    return r;
}

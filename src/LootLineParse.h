#pragma once
// LootLineParse.h — pure (de)serialization of a map_history "loot=" line value. std-only.
#include "FarmTypes.h"
#include <string>
#include <utility>
#include <cstdlib>

inline std::string FormatLootLine(const LootEntry& e) {
    return EscapeField(e.name) + "|" + std::to_string(e.stackCount) +
           "|" + std::to_string(e.chaosEach) + "|" + std::to_string(e.rarity);
}

// Split off the substring after the last UNESCAPED '|'. Returns {head, tail}.
// A '|' is escaped iff preceded by an ODD run of backslashes ("a\\|x": the two
// backslashes are one escaped '\', so that '|' IS a separator).
inline std::pair<std::string,std::string> SplitLastUnescaped(const std::string& s) {
    for (int i = (int)s.size() - 1; i >= 0; i--) {
        if (s[i] != '|') continue;
        int bs = 0;
        for (int j = i - 1; j >= 0 && s[j] == '\\'; j--) bs++;
        if ((bs & 1) == 0)
            return { s.substr(0, i), s.substr(i + 1) };
    }
    return { s, std::string() };
}

inline bool ParseLootLine(const std::string& val, LootEntry& out) {
    auto p3 = SplitLastUnescaped(val);          std::string rarStr   = p3.second;
    auto p2 = SplitLastUnescaped(p3.first);     std::string chaosStr = p2.second;
    auto p1 = SplitLastUnescaped(p2.first);     std::string stkStr   = p1.second;
    std::string nameEsc = p1.first;
    out = LootEntry{};
    out.name = UnescapeField(nameEsc);
    if (out.name.empty()) return false;
    try { out.stackCount = std::stoi(stkStr);  } catch (...) {}
    try { out.chaosEach  = std::stof(chaosStr);} catch (...) {}
    try { out.rarity     = std::stoi(rarStr);  } catch (...) {}
    return true;
}

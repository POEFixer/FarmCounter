#pragma once
// ZoneClassify.h — pure zone-name classification. std-only.
#include <string>

struct ZoneKind { bool isHideout = false, isMap = false, isPassThrough = false; };

inline ZoneKind ClassifyZone(const std::string& n) {
    ZoneKind k;
    auto has = [&](const char* s){ return n.find(s) != std::string::npos; };
    k.isHideout     = has("Hideout");
    k.isPassThrough = has("Abyss") || has("abyss") || has("Delirium_HungerBoss");
    k.isMap = !k.isHideout && (
        has("Map") || has("ExpeditionLogBook_") || has("ExpeditionSubArea_") ||
        has("Delirium_") || has("RitualLeagueBoss") || has("ChayulaLeague") ||
        has("IncursionTemple"));
    return k;
}

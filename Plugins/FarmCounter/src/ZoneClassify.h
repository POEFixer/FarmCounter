#pragma once
// ZoneClassify.h — pure zone-name classification. std-only.
#include <string>
#include "ZoneCatalog.h"

struct ZoneKind { bool isHideout = false, isMap = false, isPassThrough = false; };

inline ZoneKind ClassifyZone(const std::string& n) {
    ZoneKind k;
    auto has = [&](const char* s){ return n.find(s) != std::string::npos; };
    if (const auto* entry = ZoneCatalog::Lookup(n)) {
        k.isHideout = entry->isHideout;
        if (k.isHideout || entry->isTown) return k;
        // These areas continue the parent's loot/XP visit. The independently
        // entered pinnacle and the Abyss hub/intro are deliberately excluded.
        k.isPassThrough = n == "Delirium_HungerBoss" || n == "Abyss_Depths1" ||
            n == "Abyss_Depths2" || n == "Abyss_Depths3" || n == "Abyss_Boss1" || n == "Abyss_Boss2";
        if (k.isPassThrough) return k;
        // Existing FarmCounter intent: temples and expedition subareas remain
        // farm visits even where the game's atlas-map flag is false.
        k.isMap = entry->isMapArea || n == "IncursionTemple" || n == "IncursionTemplePresent" ||
            n.starts_with("ExpeditionSubArea_");
        return k;
    }
    k.isHideout     = has("Hideout");
    if (k.isHideout) return k;
    k.isPassThrough = has("Abyss") || has("abyss") || has("Delirium_HungerBoss");
    k.isMap = !k.isHideout && (
        has("Map") || has("ExpeditionLogBook_") || has("ExpeditionSubArea_") ||
        has("Delirium_") || has("RitualLeagueBoss") || has("ChayulaLeague") ||
        has("IncursionTemple"));
    return k;
}

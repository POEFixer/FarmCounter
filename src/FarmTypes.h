#pragma once
// FarmTypes.h — pure POD types shared across the plugin. std-only (no
// Windows/ImGui/SDK). Persisted by FarmDb (SQLite) — see FarmDb.h.
#include <cstdint>
#include <string>
#include <vector>

struct LootEntry {
    std::string name;
    int         stackCount = 0;
    float       chaosEach  = 0.0f;
    int         rarity     = 0;
    std::string iconPath;        // scan-time-resolved local PNG path (persisted for history icons)
};

struct MapRun {
    int64_t                dbId          = 0;    // sqlite rowid; 0 = not inserted yet
    std::string            mapName;
    int64_t                startedAt     = 0;    // unix seconds
    std::string            startedText;          // "YYYY-MM-DD HH:MM" local time
    int                    durationSec   = 0;    // pause-honest active time
    float                  totalChaos    = 0.0f;
    float                  exaltedRate   = 1.0f;
    std::vector<LootEntry> loot;
    bool                   archived      = false;
    int                    sessionId     = 0;
    int                    hivebloodGain = 0;
    int                    beaconGain    = 0;    // Atziri beacons gained this run
    int                    goldGain      = 0;    // gold picked up this run (positive deltas only)
    int                    killsNormal   = 0;    // per-run kill tally (accumulated
    int                    killsMagic    = 0;    // across the run's areas/visits)
    int                    killsRare     = 0;
    int                    killsUnique   = 0;
    int                    killsRogue    = 0;    // Rogue Exiles (tracked apart from Unique)
    std::vector<std::string> mapMods;           // rendered area/map modifier lines (from ctx->Game.GetAreaMods)

    int KillsTotal() const { return killsNormal + killsMagic + killsRare + killsUnique + killsRogue; }
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

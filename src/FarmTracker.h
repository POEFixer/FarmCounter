#pragma once
// FarmTracker.h — core model: zone-change state machine, map-run lifecycle,
// session active-time, loot-diff orchestration (via LootScanner), Hiveblood /
// Incursion map-gain baselines, and periodic + on-exit persistence. Owns a
// LootScanner; consumes PriceProvider and ZoneNames; reads the throttled Hb/It
// states passed into OnFrame. Behavior is a 1:1 port of the old monolith's
// DrawUI()/ScanInventory()/session logic — see docs §4 "Preserved tracking
// semantics" and FarmCounter.cpp:669-823, 1615-1776.
#include "sdk/PluginSDK.h"
#include "FarmTypes.h"
#include "LootScanner.h"
#include "PriceProvider.h"
#include "ZoneNames.h"
#include "ResourceReaders.h"
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <string>
#include <chrono>

class FarmTracker {
public:
    using Clock = std::chrono::steady_clock;

    void Init(const PluginSDK::Context* ctx, PriceProvider* prices, ZoneNames* zones,
              const std::filesystem::path& dir);
    void OnEnable();
    void OnDisable();
    void OnFrame(const PluginSDK::Snapshot& snap,
                 const ResourceReaders::HbState& hb,
                 const ResourceReaders::ItState& it);

    // ── State getters (overlay / settings) ──────────────────────────────────
    bool InMap()          const { return m_InMap; }
    bool BaselineReady()  const { return m_BaselineReady; }
    bool SessionRunning() const { return m_SessionTimerRunning; }
    int  SessionActiveSec() const;  // live total = accumulator + current running interval
    Clock::time_point SessionStart()   const { return m_SessionActiveStart; }
    Clock::time_point ZoneEnterTime()  const { return m_ZoneEnterTime; }
    const std::string& CurrentZone()   const { return m_CurrentZone; }
    const std::string& MapZoneName()   const { return m_MapZoneName; }
    const std::vector<LootEntry>& Loot() const { return m_LootLog; }
    std::vector<MapRun>&       Runs()       { return m_MapRuns; }
    const std::vector<MapRun>& Runs() const { return m_MapRuns; }
    const std::unordered_map<std::string, InvSnapshot>& LastSnapshot() const { return m_LastCurrentSnap; }
    int  CurrentSessionId() const { return m_CurrentSessionId; }

    // Resource display getters
    bool    HbHasBaseline() const { return m_HbHasBaseline; }
    int32_t HbBaseline()    const { return m_HbBaseline; }
    bool    ItHasBaseline() const { return m_ItHasBaseline; }
    int     ItBaseline()    const { return m_ItBaseline; }

    // ── Ops ──────────────────────────────────────────────────────────────────
    void NewSession();

private:
    void ScanInventory();
    void StartLiveRun();
    void UpdateLiveRun();
    void FinalizeMapRun();
    void DiscardLiveRun();
    void Save();  // SaveMapHistory(m_dir, runs, live-active-sec)

    // Dependencies
    const PluginSDK::Context* m_ctx     = nullptr;
    PriceProvider*            m_prices  = nullptr;
    ZoneNames*                m_zoneNames = nullptr;
    std::filesystem::path     m_dir;
    LootScanner               m_scanner;

    // Zone / loot state
    std::string       m_CurrentAreaHash;
    std::string       m_CurrentZone;
    std::string       m_MapZoneName;
    std::string       m_MapAreaHash;
    Clock::time_point m_ZoneEnterTime{};
    Clock::time_point m_HideoutEnterTime{};
    Clock::time_point m_LastPeriodicSave{};
    std::vector<LootEntry>                       m_LootLog;
    std::vector<LootEntry>                       m_CarryoverLoot;   // loot from previous visits to same map
    std::unordered_map<std::string, InvSnapshot> m_BaselineSnap;
    std::unordered_map<std::string, InvSnapshot> m_BaselineCandidate;
    std::unordered_map<std::string, InvSnapshot> m_LastCurrentSnap; // for the "unpriced items" settings tab
    bool m_BaselineReady = false;
    bool m_NeedBaseline  = true;
    bool m_InMap         = false;
    int  m_ActiveRunIdx           = -1;
    int  m_AccumulatedDurationSec = 0;   // sum of previous visits to current map instance
    bool m_IsResume               = false; // skip the 1500ms settle when re-entering the same map
    bool m_WasInPassThrough       = false; // was in Abyss/sub-zone, returning to same map

    // Session active-time (pauses after >60s continuously in hideout)
    int               m_SessionActiveSec    = 0;
    Clock::time_point m_SessionActiveStart{};
    bool              m_SessionTimerRunning = false;
    int               m_CurrentSessionId    = 0;

    // Map run history (persisted to config/map_history.txt)
    std::vector<MapRun> m_MapRuns;

    // Hiveblood map-gain baseline (+ carryover across hideout round-trips)
    int32_t m_HbBaseline    = 0;
    bool    m_HbHasBaseline = false;
    int32_t m_HbCachedTotal = 0;
    bool    m_HbHasCached   = false;
    int32_t m_HbCarryoverBaseline    = 0;
    bool    m_HbCarryoverHasBaseline = false;

    // Incursion-token map-gain baseline (+ carryover)
    int  m_ItBaseline    = 0;
    bool m_ItHasBaseline = false;
    int  m_ItCarryoverBaseline    = 0;
    bool m_ItCarryoverHasBaseline = false;
};

#pragma once
// FarmTracker.h — core model: zone-change state machine, map-run lifecycle,
// session active-time, loot-diff orchestration (via LootScanner), Hiveblood /
// beacon map-gain baselines, per-run kill tally, Escape-pause handling and
// SQLite persistence (FarmDb). Owns a LootScanner + FarmDb; consumes
// PriceProvider, ZoneNames and KillCounter; reads the throttled Hb/It states
// passed into OnFrame.
#include "sdk/PluginSDK.h"
#include "FarmTypes.h"
#include "FarmDb.h"
#include "LootScanner.h"
#include "PriceProvider.h"
#include "ZoneNames.h"
#include "KillCounter.h"
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
                 const ResourceReaders::ItState& it,
                 const ResourceReaders::GdState& gold,
                 const KillCounter* kills);

    // Escape-menu pause: while paused all wall-clock anchors are frozen; on
    // resume every anchor shifts forward by the pause duration, so map timers
    // and session active-time exclude time spent in the Esc menu (the game is
    // actually paused there in solo play). Called by the shell every frame.
    void SetPaused(bool paused);
    bool IsPaused() const { return m_Paused; }

    // ── State getters (overlay / settings) ──────────────────────────────────
    bool InMap()          const { return m_InMap; }
    bool BaselineReady()  const { return m_BaselineReady; }
    bool SessionRunning() const { return m_SessionTimerRunning; }
    int  SessionActiveSec() const;  // live total = accumulator + running interval (pause-honest)
    int  CurrentMapSec()    const;  // current visit elapsed (pause-honest)
    Clock::time_point SessionStart()   const { return m_SessionActiveStart; }
    Clock::time_point ZoneEnterTime()  const { return m_ZoneEnterTime; }
    const std::string& CurrentZone()   const { return m_CurrentZone; }
    const std::string& MapZoneName()   const { return m_MapZoneName; }
    const std::vector<LootEntry>& Loot() const { return m_LootLog; }
    std::vector<MapRun>&       Runs()       { return m_MapRuns; }
    const std::vector<MapRun>& Runs() const { return m_MapRuns; }
    const std::unordered_map<std::string, InvSnapshot>& LastSnapshot() const { return m_LastCurrentSnap; }
    int  CurrentSessionId() const { return m_CurrentSessionId; }
    int  ActiveRunIndex()   const { return m_ActiveRunIdx; }   // -1 = none (live run is not deletable)
    bool DbOpen()           const { return m_db.IsOpen(); }
    // Gold picked up in the current map run (0 when no live run).
    int  CurrentGoldGain()  const {
        return (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size())
            ? m_MapRuns[m_ActiveRunIdx].goldGain : 0;
    }

    // Resource display getters
    bool    HbHasBaseline() const { return m_HbHasBaseline; }
    int32_t HbBaseline()    const { return m_HbBaseline; }
    bool    ItHasBaseline() const { return m_ItHasBaseline; }
    int     ItBaseline()    const { return m_ItBaseline; }

    // ── Ops (Statistics tab) ────────────────────────────────────────────────
    void NewSession();
    void DeleteRunAt(int idx);          // any non-active run (DB + memory)
    void DeleteArchivedSession(int sessionId);
    void DeleteAllArchived();

private:
    void ScanInventory();
    void StartLiveRun();
    void UpdateLiveRun();
    void FinalizeMapRun();
    void DiscardLiveRun();
    void AccumulateKills(const KillCounter* kills);
    void Save();  // live-run row + session meta

    // Dependencies
    const PluginSDK::Context* m_ctx     = nullptr;
    PriceProvider*            m_prices  = nullptr;
    ZoneNames*                m_zoneNames = nullptr;
    std::filesystem::path     m_dir;
    LootScanner               m_scanner;
    FarmDb                    m_db;

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

    // Escape-menu pause
    bool              m_Paused = false;
    Clock::time_point m_PauseStart{};

    // Session active-time (pauses after >60s continuously in hideout)
    int               m_SessionActiveSec    = 0;
    Clock::time_point m_SessionActiveStart{};
    bool              m_SessionTimerRunning = false;
    int               m_CurrentSessionId    = 0;

    // Map run history (persisted to data/farmstats.db via FarmDb)
    std::vector<MapRun> m_MapRuns;

    // Per-area kill counters reset on every area change; the tracker folds their
    // per-frame deltas into the active run so a run's tally survives sub-zones
    // and hideout round-trips.
    int m_KillsLastNormal = 0, m_KillsLastMagic = 0, m_KillsLastRare = 0, m_KillsLastUnique = 0, m_KillsLastRogue = 0;
    bool m_KillsRebaseline = true;   // first sample after enable arms without attributing

    // Gold gain = accumulated POSITIVE deltas of the account gold total while in
    // a map (spending at hideout/town vendors between visits never distorts the
    // run's gain, unlike a baseline-vs-total scheme). -1 = unarmed.
    int m_GoldLast = -1;

    // Hiveblood map-gain baseline (+ carryover across hideout round-trips)
    int32_t m_HbBaseline    = 0;
    bool    m_HbHasBaseline = false;
    int32_t m_HbCachedTotal = 0;
    bool    m_HbHasCached   = false;
    int32_t m_HbCarryoverBaseline    = 0;
    bool    m_HbCarryoverHasBaseline = false;

    // Beacon (Atziri) map-gain baseline (+ carryover), cached last-good reading
    int  m_ItBaseline    = 0;
    bool m_ItHasBaseline = false;
    int  m_ItCachedCur   = 0;
    bool m_ItHasCached   = false;
    int  m_ItCarryoverBaseline    = 0;
    bool m_ItCarryoverHasBaseline = false;
};

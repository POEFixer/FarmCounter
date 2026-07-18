#include "FarmTracker.h"
#include "ZoneClassify.h"
#include "LootDiff.h"
#include "Persistence.h"
#include <cstdio>

using Clock = std::chrono::steady_clock;

// ── Lifecycle ───────────────────────────────────────────────────────────────

void FarmTracker::Init(const PluginSDK::Context* ctx, PriceProvider* prices,
                       ZoneNames* zones, const std::filesystem::path& dir) {
    m_ctx       = ctx;
    m_prices    = prices;
    m_zoneNames = zones;
    m_dir       = dir;
    m_scanner.SetContext(ctx, prices);
}

void FarmTracker::OnEnable() {
    // Restore runs + session (sessionActiveSec is reset from file; the session-id
    // high-water mark is only ever raised). Mirrors old OnEnable():113-118.
    LoadMapHistory(m_dir, m_MapRuns, m_SessionActiveSec, m_CurrentSessionId);
    auto now = Clock::now();
    m_ZoneEnterTime       = now;
    m_HideoutEnterTime    = now;
    m_SessionActiveStart  = now;
    m_SessionTimerRunning = true;
}

void FarmTracker::OnDisable() {
    // Flush the live entry + history on the way out (old OnDisable():124-130).
    UpdateLiveRun();
    Save();
}

// ── Per-frame entry point ────────────────────────────────────────────────────

void FarmTracker::OnFrame(const PluginSDK::Snapshot& snap,
                          const ResourceReaders::HbState& hb,
                          const ResourceReaders::ItState& it) {
    if (!m_ctx) return;
    if (!snap.IsAttached) return;
    if (snap.State != PluginSDK::GameState::InGame) return;

    // ── Zone change detection (old DrawUI():676-788) ─────────────────────────
    if (snap.CurrentAreaHash != m_CurrentAreaHash) {
        m_CurrentAreaHash = snap.CurrentAreaHash;
        m_CurrentZone     = snap.CurrentAreaName;
        if (m_zoneNames) m_zoneNames->Register(m_dir, m_CurrentZone);

        if (m_ctx) {
            char dbg[256];
            std::snprintf(dbg, sizeof(dbg), "[FarmCounter] Zone: \"%s\" hash=%s",
                          m_CurrentZone.c_str(), m_CurrentAreaHash.c_str());
            m_ctx->Log.Info(dbg);
        }

        // Pass-through checked FIRST (Abyss / Delirium_HungerBoss): just mark it,
        // never finalize / touch loot / reset baseline.
        ZoneKind k = ClassifyZone(m_CurrentZone);

        if (k.isPassThrough) {
            // sub-zone inside a map (Abyss, HungerBoss) — don't finalize, just mark
            m_WasInPassThrough = true;
        } else if (k.isMap && m_CurrentAreaHash == m_MapAreaHash) {
            if (m_WasInPassThrough) {
                // returning from a pass-through sub-zone — continue, leave loot/baseline alone
                m_WasInPassThrough = false;
                m_CurrentZone      = snap.CurrentAreaName;
                m_InMap            = true;
            } else {
                // returning to the same map instance after hideout — resume
                m_CurrentZone   = snap.CurrentAreaName;
                m_InMap         = true;
                // Restore carryover immediately so the overlay shows correct profit while re-scanning.
                m_LootLog       = m_CarryoverLoot;
                m_BaselineSnap.clear();
                m_BaselineCandidate.clear();
                m_BaselineReady = false;
                m_NeedBaseline  = true;
                m_scanner.ResetTiming();
                m_IsResume      = true;   // skip the 1500ms settle delay
                m_ZoneEnterTime = Clock::now();
                // Restore Hiveblood/Incursion baselines so map gains keep accumulating.
                m_HbHasBaseline = m_HbCarryoverHasBaseline;
                m_HbBaseline    = m_HbCarryoverBaseline;
                m_ItHasBaseline = m_ItCarryoverHasBaseline;
                m_ItBaseline    = m_ItCarryoverBaseline;
                // m_AccumulatedDurationSec already holds time from previous visits;
                // m_ActiveRunIdx is still valid (we never finalized on exit) — just continue.
            }
        } else if (k.isMap) {
            // new map instance — finalize the previous (suspended) run if any
            if (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size()) {
                const MapRun& prev = m_MapRuns[m_ActiveRunIdx];
                if (!prev.loot.empty() || prev.durationSec >= 10) FinalizeMapRun();
                else DiscardLiveRun();
            }
            m_MapZoneName            = m_CurrentZone;
            m_MapAreaHash            = m_CurrentAreaHash;
            m_ZoneEnterTime          = Clock::now();
            m_AccumulatedDurationSec = 0;
            m_IsResume               = false;
            m_WasInPassThrough       = false;
            m_LootLog.clear();
            m_CarryoverLoot.clear();
            m_BaselineSnap.clear();
            m_BaselineCandidate.clear();
            m_BaselineReady          = false;
            m_NeedBaseline           = true;
            m_scanner.ResetTiming();
            m_InMap                  = true;
            // Reset map-gain baselines for the new map. The old monolith reset the
            // live Hb baseline lazily via UpdateHiveblood's AreaChangeCounter check;
            // since FarmTracker detects the zone change directly we reset it here
            // (same end state: a fresh baseline from the first in-map read).
            m_HbCarryoverHasBaseline = false;
            m_HbCarryoverBaseline    = 0;
            m_HbHasBaseline          = false;
            m_HbBaseline             = 0;
            m_ItCarryoverHasBaseline = false;
            m_ItCarryoverBaseline    = 0;
            m_ItHasBaseline          = false;
            m_ItBaseline             = 0;
            StartLiveRun();
        } else {
            // leaving a map to hideout / other non-map — suspend (do NOT finalize)
            // so a later same-hash re-entry resumes the same entry.
            if (m_InMap) {
                if (!m_BaselineReady) {
                    // never captured a baseline — discard the placeholder
                    DiscardLiveRun();
                    m_LootLog.clear();
                    m_CarryoverLoot.clear();
                    m_BaselineSnap.clear();
                    m_BaselineCandidate.clear();
                    m_MapAreaHash.clear();
                    m_AccumulatedDurationSec = 0;
                } else {
                    // Flush the live entry FIRST: m_InMap is still true and
                    // m_ZoneEnterTime not yet consumed, so UpdateLiveRun writes
                    // accum + this visit exactly once. Folding the visit into the
                    // accumulator BEFORE the flush double-counted it (the run then
                    // persisted 2x duration if the map was never resumed).
                    UpdateLiveRun();
                    // accumulate elapsed time so resuming continues correctly
                    m_AccumulatedDurationSec += (int)std::chrono::duration_cast<std::chrono::seconds>(
                        Clock::now() - m_ZoneEnterTime).count();
                    // carry over loot so the next visit's diff merges on top
                    m_CarryoverLoot          = m_LootLog;
                    // save Hb/It baselines so map gains survive the hideout round-trip
                    m_HbCarryoverHasBaseline = m_HbHasBaseline;
                    m_HbCarryoverBaseline    = m_HbBaseline;
                    m_ItCarryoverHasBaseline = m_ItHasBaseline;
                    m_ItCarryoverBaseline    = m_ItBaseline;
                    // persist current state so it survives a crash/close (save on map exit)
                    Save();
                }
            }
            // keep m_ActiveRunIdx so we can resume; UpdateLiveRun is a no-op while !m_InMap
            m_InMap            = false;
            m_scanner.ResetTiming();
            m_WasInPassThrough = false;
            if (k.isHideout) m_HideoutEnterTime = Clock::now();
        }
    }

    // ── Hiveblood / Incursion baseline capture (old UpdateHiveblood():1304-1309,
    //    DrawBarsRow():1325). Runs every frame after zone detection so the live
    //    baseline reset above is recaptured from the first in-map read. ─────────
    if (hb.ok) {
        m_HbCachedTotal = hb.total;
        m_HbHasCached   = true;
        // Capture the baseline only on a fresh read (matches old "ReadHiveblood
        // succeeds"); a cached/stale frame must not arm a new baseline.
        if (!hb.cached && hb.total > 0 && !m_HbHasBaseline) {
            m_HbBaseline    = hb.total;
            m_HbHasBaseline = true;
        }
    }
    if (m_InMap && it.ok && !m_ItHasBaseline) {
        m_ItBaseline    = it.cur;
        m_ItHasBaseline = true;
    }

    // ── Loot scan + live-run update (old DrawUI():790-791) ────────────────────
    if (m_InMap) ScanInventory();
    UpdateLiveRun();

    // Periodic save (relaxed from ~1s to 15s; map-exit saves are immediate above).
    {
        auto now = Clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - m_LastPeriodicSave).count() >= 15) {
            m_LastPeriodicSave = now;
            Save();
        }
    }

    // ── Session active-time: pause after >60s continuously in hideout, resume on
    //    leaving hideout (old DrawUI():802-819). ────────────────────────────────
    bool inHideout = m_CurrentZone.find("Hideout") != std::string::npos;
    if (inHideout) {
        auto hideoutSec = std::chrono::duration_cast<std::chrono::seconds>(
            Clock::now() - m_HideoutEnterTime).count();
        if (hideoutSec >= 60 && m_SessionTimerRunning) {
            m_SessionActiveSec += (int)std::chrono::duration_cast<std::chrono::seconds>(
                Clock::now() - m_SessionActiveStart).count();
            m_SessionTimerRunning = false;
        }
    } else if (!m_SessionTimerRunning) {
        m_SessionActiveStart  = Clock::now();
        m_SessionTimerRunning = true;
    }
}

// ── Inventory scan orchestration (old ScanInventory():1715-1776 via LootScanner) ─

void FarmTracker::ScanInventory() {
    std::unordered_map<std::string, InvSnapshot> snap;
    if (!m_scanner.RequestAndRead(snap)) return;

    // New-map baseline: only capture once the inventory has settled (>=1500ms since
    // zone-enter). A resume skips this delay (m_IsResume).
    if (m_NeedBaseline && !m_IsResume) {
        auto sinceEnter = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - m_ZoneEnterTime).count();
        if (sinceEnter < 1500) return;
    }

    m_LastCurrentSnap = snap;
    if (m_NeedBaseline) {
        // Stability gate: lock baseline only once two consecutive 1s reads are identical.
        // The game lazily fills the backpack list ~10-15s after map-enter, so the first
        // non-empty read can be partial — locking then would count late items as loot.
        if (m_BaselineCandidate.empty() || !SnapshotStable(snap, m_BaselineCandidate)) {
            m_BaselineCandidate = std::move(snap);
            return;
        }
        m_BaselineSnap      = std::move(snap);
        m_BaselineReady     = true;
        m_NeedBaseline      = false;
        m_IsResume          = false;
        m_BaselineCandidate.clear();
        return;
    }
    // Diff current vs baseline, merged over carryover (drops zero-stack entries).
    m_LootLog = DiffLoot(m_BaselineSnap, snap, m_CarryoverLoot);
}

// ── Map-run lifecycle (old :1622-1678) ───────────────────────────────────────

void FarmTracker::StartLiveRun() {
    MapRun run;
    run.mapName  = m_MapZoneName;
    run.archived = false;
    m_MapRuns.push_back(std::move(run));
    m_ActiveRunIdx = (int)m_MapRuns.size() - 1;
}

void FarmTracker::UpdateLiveRun() {
    if (m_ActiveRunIdx < 0 || m_ActiveRunIdx >= (int)m_MapRuns.size()) return;
    if (!m_InMap) return;
    float totalChaos = 0.f;
    for (const auto& e : m_LootLog)
        totalChaos += e.chaosEach * (float)e.stackCount;
    float exRate = m_prices ? m_prices->ExaltedInChaos() : 1.f;
    MapRun& r = m_MapRuns[m_ActiveRunIdx];
    // Accumulate: prior visits + current visit.
    int curVisitSec = (int)std::chrono::duration_cast<std::chrono::seconds>(
        Clock::now() - m_ZoneEnterTime).count();
    r.durationSec = m_AccumulatedDurationSec + curVisitSec;
    r.totalChaos  = totalChaos;
    r.exaltedRate = exRate;
    r.loot        = m_LootLog;
    if (m_HbHasCached && m_HbHasBaseline && m_HbCachedTotal >= m_HbBaseline)
        r.hivebloodGain = m_HbCachedTotal - m_HbBaseline;
}

void FarmTracker::FinalizeMapRun() {
    if (m_MapZoneName.empty()) return;
    UpdateLiveRun();
    if (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size()) {
        const MapRun& run = m_MapRuns[m_ActiveRunIdx];
        if (run.loot.empty() && run.durationSec < 10)
            m_MapRuns.erase(m_MapRuns.begin() + m_ActiveRunIdx);
        else
            Save();
    }
    m_ActiveRunIdx           = -1;
    m_AccumulatedDurationSec = 0;
    m_CarryoverLoot.clear();
}

void FarmTracker::DiscardLiveRun() {
    if (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size())
        m_MapRuns.erase(m_MapRuns.begin() + m_ActiveRunIdx);
    m_ActiveRunIdx           = -1;
    m_AccumulatedDurationSec = 0;
    m_CarryoverLoot.clear();
}

// ── Session ops ──────────────────────────────────────────────────────────────

int FarmTracker::SessionActiveSec() const {
    int s = m_SessionActiveSec;
    if (m_SessionTimerRunning)
        s += (int)std::chrono::duration_cast<std::chrono::seconds>(
            Clock::now() - m_SessionActiveStart).count();
    return s;
}

void FarmTracker::NewSession() {
    // Archive every active run under a freshly-incremented session id, then reset
    // current-map tracking so the overlay clears (old "New Session" button:418-439).
    m_CurrentSessionId++;
    for (auto& r : m_MapRuns)
        if (!r.archived) { r.archived = true; r.sessionId = m_CurrentSessionId; }
    m_ActiveRunIdx   = -1;
    m_LootLog.clear();
    m_CarryoverLoot.clear();
    m_BaselineSnap.clear();
    m_BaselineCandidate.clear();
    m_BaselineReady  = false;
    m_NeedBaseline   = true;
    m_scanner.ResetTiming();
    m_MapZoneName.clear();
    m_MapAreaHash.clear();
    m_InMap                  = false;
    m_AccumulatedDurationSec = 0;
    auto now = Clock::now();
    m_ZoneEnterTime       = now;
    m_HideoutEnterTime    = now;
    m_SessionActiveSec    = 0;
    m_SessionActiveStart  = now;
    m_SessionTimerRunning = true;
    Save();
}

void FarmTracker::Save() {
    SaveMapHistory(m_dir, m_MapRuns, SessionActiveSec());
}

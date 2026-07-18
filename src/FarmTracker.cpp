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
    // Restore runs + session from SQLite (sessionActiveSec is reset from the DB;
    // the session-id high-water mark is only ever raised).
    if (!m_db.Open(m_dir) && m_ctx)
        m_ctx->Log.Error("[FarmCounter] farmstats.db failed to open — statistics won't persist");
    m_db.LoadAll(m_MapRuns, m_SessionActiveSec, m_CurrentSessionId);
    // The host re-enables the SAME plugin instance, so tracking members persist
    // across disable→enable. Reset them: the current map (if any) re-detects on
    // the next frame and starts a FRESH run — otherwise a stale m_ActiveRunIdx
    // could point into the reloaded vector and stale gold/kill baselines would
    // attribute everything gained while disabled to the next run.
    m_CurrentAreaHash.clear();
    m_MapAreaHash.clear();
    m_MapZoneName.clear();
    m_InMap         = false;
    m_ActiveRunIdx  = -1;
    m_AccumulatedDurationSec = 0;
    m_IsResume         = false;
    m_WasInPassThrough = false;
    m_LootLog.clear();
    m_CarryoverLoot.clear();
    m_BaselineSnap.clear();
    m_BaselineCandidate.clear();
    m_BaselineReady = false;
    m_NeedBaseline  = true;
    m_scanner.ResetTiming();
    m_GoldLast        = -1;    // first fresh read only arms, never attributes
    m_KillsRebaseline = true;  // ditto for the per-area kill counters
    m_Paused = false;
    auto now = Clock::now();
    m_ZoneEnterTime       = now;
    m_HideoutEnterTime    = now;
    m_SessionActiveStart  = now;
    m_SessionTimerRunning = true;
}

void FarmTracker::OnDisable() {
    // Flush the live entry + meta on the way out.
    SetPaused(false);
    UpdateLiveRun();
    Save();
    m_db.Close();
}

// ── Escape-menu pause ───────────────────────────────────────────────────────

void FarmTracker::SetPaused(bool paused) {
    if (paused == m_Paused) return;
    auto now = Clock::now();
    if (paused) {
        m_Paused     = true;
        m_PauseStart = now;
        return;
    }
    // Resume: shift every wall-clock anchor forward by the pause duration so
    // elapsed-time math (map timer, session time, hideout timer, save cadence)
    // never sees the paused interval.
    const auto d = now - m_PauseStart;
    m_ZoneEnterTime      += d;
    m_HideoutEnterTime   += d;
    m_SessionActiveStart += d;
    m_LastPeriodicSave   += d;
    m_Paused = false;
}

int FarmTracker::CurrentMapSec() const {
    if (!m_InMap) return 0;
    const auto end = m_Paused ? m_PauseStart : Clock::now();
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(end - m_ZoneEnterTime).count();
    return (int)(sec < 0 ? 0 : sec);
}

// ── Per-frame entry point ────────────────────────────────────────────────────

void FarmTracker::OnFrame(const PluginSDK::Snapshot& snap,
                          const ResourceReaders::HbState& hb,
                          const ResourceReaders::ItState& it,
                          const ResourceReaders::GdState& gold,
                          const KillCounter* kills) {
    if (!m_ctx) return;
    if (!snap.IsAttached) return;
    if (snap.State != PluginSDK::GameState::InGame) return;

    // ── Zone change detection ────────────────────────────────────────────────
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
                // Restore Hiveblood/beacon baselines so map gains keep accumulating.
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
            // Reset map-gain baselines for the new map — a fresh baseline is
            // captured from the first in-map read below.
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
                    m_AccumulatedDurationSec += CurrentMapSec();
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

    // ── Hiveblood / beacon baseline capture. Runs every frame after zone
    //    detection so the live baseline reset above is recaptured from the
    //    first in-map read. ──────────────────────────────────────────────────
    if (hb.ok) {
        m_HbCachedTotal = hb.total;
        m_HbHasCached   = true;
        // Capture the baseline only on a fresh read; a cached/stale frame must
        // not arm a new baseline.
        if (!hb.cached && hb.total > 0 && !m_HbHasBaseline) {
            m_HbBaseline    = hb.total;
            m_HbHasBaseline = true;
        }
    }
    if (it.ok) {
        m_ItCachedCur = it.cur;
        m_ItHasCached = true;
        if (m_InMap && !m_ItHasBaseline) {
            m_ItBaseline    = it.cur;
            m_ItHasBaseline = true;
        }
    }

    // ── Per-run gold tally: accumulate POSITIVE deltas of the account total on
    //    fresh reads only. Pickups in a map raise the total; spending happens at
    //    vendors (town/hideout) and is deliberately ignored, so a hideout
    //    round-trip never deflates the run's gain. ──────────────────────────────
    if (gold.ok && !gold.cached) {
        if (m_GoldLast >= 0) {
            const int delta = gold.total - m_GoldLast;
            if (delta > 0 && m_InMap &&
                m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size())
                m_MapRuns[m_ActiveRunIdx].goldGain += delta;
        }
        m_GoldLast = gold.total;   // arm/refresh (first read never counts)
    }

    // ── Per-run kill tally (delta over the per-area KillCounter) ─────────────
    AccumulateKills(kills);

    // ── Map modifiers: capture once per run from the host. The core renders them
    //    only inside a real map (empty otherwise) and can lag map-enter, so retry
    //    for a bounded window rather than every frame forever. Not captured in a
    //    pass-through sub-zone (Abyss/HungerBoss) — keep the map's own mods. ─────
    if (m_InMap && !m_WasInPassThrough && m_ctx
        && m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size()
        && m_MapRuns[m_ActiveRunIdx].mapMods.empty()
        && CurrentMapSec() < 20) {   // core has a 10s populate window; stop after 20s
        auto mods = m_ctx->Game.GetAreaMods();
        if (!mods.empty()) {
            auto& dst = m_MapRuns[m_ActiveRunIdx].mapMods;
            dst.clear(); dst.reserve(mods.size());
            for (const auto& m : mods) dst.push_back(m.text);
        }
    }

    // ── Loot scan + live-run update ──────────────────────────────────────────
    if (m_InMap) ScanInventory();
    UpdateLiveRun();

    // Periodic save (live-run row + session meta; map-exit saves are immediate above).
    {
        auto now = Clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - m_LastPeriodicSave).count() >= 15) {
            m_LastPeriodicSave = now;
            Save();
        }
    }

    // ── Session active-time: pause after >60s continuously in hideout, resume on
    //    leaving hideout. ─────────────────────────────────────────────────────
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

void FarmTracker::AccumulateKills(const KillCounter* kills) {
    if (!kills) return;
    if (m_KillsRebaseline) {
        // First sample after enable: sync the last-seen counters without
        // attributing anything (the KillCounter kept counting while disabled).
        m_KillsLastNormal = kills->Normal();
        m_KillsLastMagic  = kills->Magic();
        m_KillsLastRare   = kills->Rare();
        m_KillsLastUnique = kills->Unique();
        m_KillsLastRogue  = kills->Rogue();
        m_KillsRebaseline = false;
        return;
    }
    // The KillCounter resets on every area change (value drops back down) — a
    // drop re-baselines the delta instead of going negative, so kills keep
    // accumulating across a run's sub-zones and hideout round-trips.
    auto delta = [](int cur, int& last) {
        int d = (cur >= last) ? (cur - last) : cur;
        last = cur;
        return d;
    };
    const int dn = delta(kills->Normal(), m_KillsLastNormal);
    const int dm = delta(kills->Magic(),  m_KillsLastMagic);
    const int dr = delta(kills->Rare(),   m_KillsLastRare);
    const int du = delta(kills->Unique(), m_KillsLastUnique);
    const int dg = delta(kills->Rogue(),  m_KillsLastRogue);
    if (!m_InMap) return;   // deltas outside a run are tracked but not attributed
    if (m_ActiveRunIdx < 0 || m_ActiveRunIdx >= (int)m_MapRuns.size()) return;
    MapRun& r = m_MapRuns[m_ActiveRunIdx];
    r.killsNormal += dn;
    r.killsMagic  += dm;
    r.killsRare   += dr;
    r.killsUnique += du;
    r.killsRogue  += dg;
}

// ── Inventory scan orchestration ─────────────────────────────────────────────

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

// ── Map-run lifecycle ────────────────────────────────────────────────────────

void FarmTracker::StartLiveRun() {
    MapRun run;
    run.mapName   = m_MapZoneName;
    run.archived  = false;
    run.startedAt = 0;                 // stamped by FarmDb::InsertRun
    m_MapRuns.push_back(std::move(run));
    m_ActiveRunIdx = (int)m_MapRuns.size() - 1;
    m_db.InsertRun(m_MapRuns.back());  // sets dbId + startedAt/startedText
}

void FarmTracker::UpdateLiveRun() {
    if (m_ActiveRunIdx < 0 || m_ActiveRunIdx >= (int)m_MapRuns.size()) return;
    if (!m_InMap) return;
    float totalChaos = 0.f;
    for (const auto& e : m_LootLog)
        totalChaos += e.chaosEach * (float)e.stackCount;
    float exRate = m_prices ? m_prices->ExaltedInChaos() : 1.f;
    MapRun& r = m_MapRuns[m_ActiveRunIdx];
    // Accumulate: prior visits + current visit (pause-honest).
    r.durationSec = m_AccumulatedDurationSec + CurrentMapSec();
    r.totalChaos  = totalChaos;
    r.exaltedRate = exRate;
    r.loot        = m_LootLog;
    if (m_HbHasCached && m_HbHasBaseline && m_HbCachedTotal >= m_HbBaseline)
        r.hivebloodGain = m_HbCachedTotal - m_HbBaseline;
    if (m_ItHasCached && m_ItHasBaseline && m_ItCachedCur >= m_ItBaseline)
        r.beaconGain = m_ItCachedCur - m_ItBaseline;
}

void FarmTracker::FinalizeMapRun() {
    if (m_MapZoneName.empty()) return;
    UpdateLiveRun();
    if (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size()) {
        const MapRun& run = m_MapRuns[m_ActiveRunIdx];
        if (run.loot.empty() && run.durationSec < 10) {
            m_db.DeleteRun(run.dbId);
            m_MapRuns.erase(m_MapRuns.begin() + m_ActiveRunIdx);
        } else {
            Save();
        }
    }
    m_ActiveRunIdx           = -1;
    m_AccumulatedDurationSec = 0;
    m_CarryoverLoot.clear();
}

void FarmTracker::DiscardLiveRun() {
    if (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size()) {
        m_db.DeleteRun(m_MapRuns[m_ActiveRunIdx].dbId);
        m_MapRuns.erase(m_MapRuns.begin() + m_ActiveRunIdx);
    }
    m_ActiveRunIdx           = -1;
    m_AccumulatedDurationSec = 0;
    m_CarryoverLoot.clear();
}

// ── Session ops ──────────────────────────────────────────────────────────────

int FarmTracker::SessionActiveSec() const {
    int s = m_SessionActiveSec;
    if (m_SessionTimerRunning) {
        const auto end = m_Paused ? m_PauseStart : Clock::now();
        const auto sec = std::chrono::duration_cast<std::chrono::seconds>(
            end - m_SessionActiveStart).count();
        if (sec > 0) s += (int)sec;
    }
    return s;
}

void FarmTracker::NewSession() {
    // Flush the live run's freshest state (loot/kills since the last periodic
    // save) BEFORE archiving — the archive below only flips flags.
    UpdateLiveRun();
    if (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size())
        m_db.UpdateRun(m_MapRuns[m_ActiveRunIdx]);
    // Archive every active run under a freshly-incremented session id, then reset
    // current-map tracking so the overlay clears.
    m_CurrentSessionId++;
    for (auto& r : m_MapRuns)
        if (!r.archived) { r.archived = true; r.sessionId = m_CurrentSessionId; }
    m_db.ArchiveActiveRuns(m_CurrentSessionId);
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
    // While Esc-paused, anchor to the pause start: the resume shift adds the
    // full pause duration to these, which would otherwise push wall-clock-`now`
    // anchors into the future (negative elapsed until real time catches up).
    auto now = m_Paused ? m_PauseStart : Clock::now();
    m_ZoneEnterTime       = now;
    m_HideoutEnterTime    = now;
    m_SessionActiveSec    = 0;
    m_SessionActiveStart  = now;
    m_SessionTimerRunning = true;
    Save();
}

void FarmTracker::DeleteRunAt(int idx) {
    if (idx < 0 || idx >= (int)m_MapRuns.size()) return;
    if (idx == m_ActiveRunIdx) return;   // the live run is not deletable from the UI
    m_db.DeleteRun(m_MapRuns[idx].dbId);
    m_MapRuns.erase(m_MapRuns.begin() + idx);
    if (idx < m_ActiveRunIdx) m_ActiveRunIdx--;
}

void FarmTracker::DeleteArchivedSession(int sessionId) {
    m_db.DeleteSession(sessionId);
    for (int i = (int)m_MapRuns.size() - 1; i >= 0; i--) {
        if (m_MapRuns[i].archived && m_MapRuns[i].sessionId == sessionId) {
            m_MapRuns.erase(m_MapRuns.begin() + i);
            if (i < m_ActiveRunIdx) m_ActiveRunIdx--;
        }
    }
}

void FarmTracker::DeleteAllArchived() {
    m_db.DeleteAllArchived();
    for (int i = (int)m_MapRuns.size() - 1; i >= 0; i--) {
        if (m_MapRuns[i].archived) {
            m_MapRuns.erase(m_MapRuns.begin() + i);
            if (i < m_ActiveRunIdx) m_ActiveRunIdx--;
        }
    }
}

void FarmTracker::Save() {
    if (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size())
        m_db.UpdateRun(m_MapRuns[m_ActiveRunIdx]);
    m_db.SaveMeta(SessionActiveSec(), m_CurrentSessionId);
}

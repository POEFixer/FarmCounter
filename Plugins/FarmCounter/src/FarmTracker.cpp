#include "FarmTracker.h"
#include "ZoneClassify.h"
#include "LootDiff.h"
#include "Persistence.h"
#include <cstdio>
#include <algorithm>
#include <limits>

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
    // A watchdog can stop DrawUI without OnDisable. Preserve the last completed
    // frame before reload, using its observed elapsed time, never the disabled
    // wall-clock gap. Failed writes keep the authoritative in-memory history.
    bool canReload = true;
    if (m_HasRuntimeState) {
        m_SessionActiveSec = m_LastObservedSessionSec;
        m_SessionTimerRunning = false;
        m_MetaDirty = true;
        canReload = Save();
    }
    if (!m_db.IsOpen() && !PersistenceResult(m_db.Open(m_dir))) canReload = false;
    if (canReload && PersistenceResult(m_db.LoadAll(m_MapRuns, m_SessionActiveSec, m_CurrentSessionId))) {
        m_DirtyRunIds.clear();
        m_PendingDiscards.clear();
        m_MetaDirty = false;
        MarkHistoryChanged(true);
    }
    m_HasRuntimeState = true;
    m_LastObservedSessionSec = m_SessionActiveSec;
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
    m_MapModRetry.Reset(Clock::now());
    m_LootLog.clear();
    m_CarryoverLoot.clear();
    m_BaselineSnap.clear();
    m_BaselineCandidate.clear();
    m_LastCurrentSnap.clear();
    m_HasBaselineCandidate = false;
    m_HasLastCurrentSnap = false;
    m_BaselineReady = false;
    m_NeedBaseline  = true;
    m_scanner.ResetTiming();
    m_GoldLast        = -1;    // first fresh read only arms, never attributes
    m_KillsRebaseline = true;  // ditto for the per-area kill counters
    m_Paused = false;
    m_experience.Clear();
    m_mapExperience.Clear();
    m_lastExperienceRead = {};
    auto now = Clock::now();
    m_ZoneEnterTime       = now;
    m_HideoutEnterTime    = now;
    m_SessionActiveStart  = now;
    m_SessionTimerRunning = true;
}

void FarmTracker::OnDisable() {
    // The completed frame already updated its run. OnDisable can also arrive
    // long after a watchdog stopped callbacks, so do not recompute elapsed here.
    PauseExperience();
    m_SessionActiveSec = m_LastObservedSessionSec;
    m_SessionTimerRunning = false;
    m_Paused = false;
    m_MetaDirty = true;
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
    m_MapModRetry.Shift(d);
    m_Paused = false;
}

int FarmTracker::CurrentMapSec() const {
    if (!m_InMap) return 0;
    const auto end = m_Paused ? m_PauseStart : Clock::now();
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(end - m_ZoneEnterTime).count();
    return (int)(sec < 0 ? 0 : sec);
}

void FarmTracker::UpdateExperience(const PluginSDK::Snapshot& snap) {
    const auto now = Clock::now();
    const bool running = m_SessionTimerRunning && !m_Paused && !snap.IsPaused;
    const auto kind = ClassifyZone(snap.CurrentAreaName);
    const bool areaChanged = m_mapExperience.ObserveArea(snap.CurrentAreaHash, kind.isMap, kind.isPassThrough);
    // Clock updates are cheap and keep the display smooth. Player/name memory
    // reads are sampled at 4 Hz, including while the XP block is hidden.
    m_experience.Tick(running, now);
    if (!m_ctx || (!areaChanged && now - m_lastExperienceRead < std::chrono::milliseconds(250))) return;
    m_lastExperienceRead = now;
    const auto player = m_ctx->Components.ReadPlayer(snap.Player.Components.Player);
    if (player.Valid) {
        m_experience.Sample(player.Name, player.Xp, player.Level, running, now);
        m_mapExperience.Sample(player.Name, player.Xp, player.Level);
    }
}

void FarmTracker::PauseExperience(bool discardBaseline) {
    m_experience.Tick(false, Clock::now());
    m_lastExperienceRead = {};
    // A detached game can keep progressing without the overlay. A fresh read
    // after reattachment must not attribute that XP to the old active time.
    if (discardBaseline) { m_experience.Clear(); m_mapExperience.Clear(); }
}

void FarmTracker::ResetExperience() {
    m_experience.Reset();
    m_lastExperienceRead = {};
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
    // The host may publish a newer area between GetSnapshot and inventory reads
    // (or before a settings reset). Never fold that publication into this frame.
    m_scanner.SetAreaCounter(snap.AreaChangeCounter);

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
                m_HasBaselineCandidate = false;
                m_HasLastCurrentSnap = false;
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
                m_MapModRetry.Reset(Clock::now());
                // m_AccumulatedDurationSec already holds time from previous visits;
                // m_ActiveRunIdx is still valid (we never finalized on exit) — just continue.
            }
        } else if (k.isMap) {
            // new map instance — finalize the previous (suspended) run if any
            if (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size()) {
                const MapRun& prev = m_MapRuns[m_ActiveRunIdx];
                if (HasMeaningfulActivity(prev)) FinalizeMapRun();
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
            m_LastCurrentSnap.clear();
            m_HasBaselineCandidate = false;
            m_HasLastCurrentSnap = false;
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
            m_MapModRetry.Reset(Clock::now());
            StartLiveRun();
        } else {
            // leaving a map to hideout / other non-map — suspend (do NOT finalize)
            // so a later same-hash re-entry resumes the same entry.
            if (m_InMap) {
                if (!m_BaselineReady && (m_ActiveRunIdx < 0 ||
                    !HasMeaningfulActivity(m_MapRuns[m_ActiveRunIdx]))) {
                    // never captured a baseline — discard the placeholder
                    DiscardLiveRun();
                    m_LootLog.clear();
                    m_CarryoverLoot.clear();
                    m_BaselineSnap.clear();
                    m_BaselineCandidate.clear();
                    m_HasBaselineCandidate = false;
                    m_HasLastCurrentSnap = false;
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
                m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size()) {
                m_MapRuns[m_ActiveRunIdx].goldGain += delta;
                MarkRunDirty(m_MapRuns[m_ActiveRunIdx]);
            }
        }
        m_GoldLast = gold.total;   // arm/refresh (first read never counts)
    }

    // ── Per-run kill tally (delta over the per-area KillCounter) ─────────────
    AccumulateKills(kills);

    // ── Map modifiers: capture once per run from the host. The core renders them
    //    only inside a real map (empty otherwise) and can lag map-enter, so retry
    //    for a bounded window rather than every frame forever. The host may
    //    publish the stat containers late, so keep a matching bounded window
    //    while throttling this render-thread probe. Not captured in a
    //    pass-through sub-zone (Abyss/HungerBoss) — keep the map's own mods.
    const auto mapModNow = Clock::now();
    if (m_InMap && !m_WasInPassThrough && m_ctx
        && m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size()
        && m_MapRuns[m_ActiveRunIdx].mapMods.empty()
        && CurrentMapSec() < 120
        && m_MapModRetry.ShouldAttempt(mapModNow)) {
        auto mods = m_ctx->Game.GetAreaMods();
        m_MapModRetry.RecordAttempt(Clock::now(), !mods.empty());
        if (!mods.empty()) {
            auto& dst = m_MapRuns[m_ActiveRunIdx].mapMods;
            dst.clear(); dst.reserve(mods.size());
            for (const auto& m : mods) dst.push_back(m.text);
            MarkRunDirty(m_MapRuns[m_ActiveRunIdx]);
        }
    }

    // ── Loot scan + live-run update ──────────────────────────────────────────
    if (m_InMap) ScanInventory();
    UpdateLiveRun();

    // Periodic save (live-run row + session meta; map-exit saves are immediate above).
    {
        auto now = Clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - m_LastPeriodicSave).count() >= 15 ||
            (HasUnsavedChanges() && !m_LastPersistenceError.empty() && now - m_LastSaveAttempt >= std::chrono::seconds(1))) {
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
    const int activeSec = SessionActiveSec();
    if (activeSec != m_LastObservedSessionSec) m_MetaDirty = true;
    m_LastObservedSessionSec = activeSec;
    PublishHistoryChanges();
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
    if (dn || dm || dr || du || dg) MarkRunDirty(r);
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
    m_HasLastCurrentSnap = true;
    if (m_NeedBaseline) {
        // Stability gate: lock baseline only once two consecutive 1s reads are identical.
        // The game lazily fills the backpack list ~10-15s after map-enter, so the first
        // non-empty read can be partial — locking then would count late items as loot.
        if (!m_HasBaselineCandidate || !SnapshotStable(snap, m_BaselineCandidate)) {
            m_BaselineCandidate = std::move(snap);
            m_HasBaselineCandidate = true;
            return;
        }
        m_BaselineSnap      = std::move(snap);
        m_BaselineReady     = true;
        m_NeedBaseline      = false;
        m_IsResume          = false;
        m_BaselineCandidate.clear();
        m_HasBaselineCandidate = false;
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
    MarkHistoryChanged(true);
    MarkRunDirty(m_MapRuns.back());
    Save();
}

void FarmTracker::UpdateLiveRun() {
    if (m_ActiveRunIdx < 0 || m_ActiveRunIdx >= (int)m_MapRuns.size()) return;
    if (!m_InMap) return;
    float totalChaos = 0.f;
    for (const auto& e : m_LootLog)
        totalChaos += e.chaosEach * (float)e.stackCount;
    float exRate = m_prices ? m_prices->ExaltedInChaos() : 1.f;
    MapRun& r = m_MapRuns[m_ActiveRunIdx];
    const int duration = m_AccumulatedDurationSec + CurrentMapSec();
    const int hiveblood = m_HbHasCached && m_HbHasBaseline && m_HbCachedTotal >= m_HbBaseline
        ? m_HbCachedTotal - m_HbBaseline : r.hivebloodGain;
    const int beacons = m_ItHasCached && m_ItHasBaseline && m_ItCachedCur >= m_ItBaseline
        ? m_ItCachedCur - m_ItBaseline : r.beaconGain;
    const bool sameLoot = r.loot.size() == m_LootLog.size() &&
        std::equal(r.loot.begin(), r.loot.end(), m_LootLog.begin(), [](const LootEntry& a, const LootEntry& b) {
            return a.name == b.name && a.stackCount == b.stackCount && a.chaosEach == b.chaosEach &&
                a.rarity == b.rarity && a.iconPath == b.iconPath;
        });
    const bool changed = r.durationSec != duration || r.totalChaos != totalChaos || r.exaltedRate != exRate ||
        r.hivebloodGain != hiveblood || r.beaconGain != beacons || !sameLoot;
    // Accumulate: prior visits + current visit (pause-honest).
    r.durationSec = duration;
    r.totalChaos  = totalChaos;
    r.exaltedRate = exRate;
    r.loot        = m_LootLog;
    r.hivebloodGain = hiveblood;
    r.beaconGain = beacons;
    if (changed) MarkRunDirty(r);
}

void FarmTracker::FinalizeMapRun() {
    if (m_MapZoneName.empty()) return;
    UpdateLiveRun();
    if (m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)m_MapRuns.size()) {
        const MapRun& run = m_MapRuns[m_ActiveRunIdx];
        if (!HasMeaningfulActivity(run)) {
            DiscardLiveRun();
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
        const auto id = m_MapRuns[m_ActiveRunIdx].dbId;
        if (id == 0 || PersistenceResult(m_db.DeleteRun(id))) EraseRunAt(m_ActiveRunIdx);
        else m_PendingDiscards.insert(id);
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

bool FarmTracker::NewSession() {
    // Flush the live run's freshest state (loot/kills since the last periodic
    // save) BEFORE archiving — the archive below only flips flags.
    UpdateLiveRun();
    // Previous maps may also be dirty/id0 after a failed write. Preserve them
    // before the atomic archive; a failed preflight changes no session identity.
    if (!Save()) return false;
    if (m_CurrentSessionId == (std::numeric_limits<int>::max)()) {
        m_LastPersistenceError = "Session identifier limit reached";
        return false;
    }
    const bool restartMap = m_InMap && !m_MapZoneName.empty();
    std::unordered_map<std::string, InvSnapshot> resetSnapshot;
    uint64_t resetStamp = 0;
    const bool ready = restartMap && m_BaselineReady && m_HasLastCurrentSnap && m_scanner.ReadCurrent(resetSnapshot, &resetStamp);
    // Reserve/copy before COMMIT so publishing the next history is a no-throw
    // swap. A reset inside HungerBoss retains the parent map's identity.
    auto nextRuns = m_MapRuns;
    nextRuns.reserve(nextRuns.size() + (restartMap ? 1 : 0));
    auto resetDisplaySnapshot = resetSnapshot;
    MapRun fresh;
    if (restartMap) fresh.mapName = m_MapZoneName;
    MapRun* old = m_ActiveRunIdx >= 0 && m_ActiveRunIdx < (int)nextRuns.size()
        ? &nextRuns[m_ActiveRunIdx] : nullptr;
    if (old && ready) {
        // A completed host scan can arrive between the last periodic diff and
        // the click. Attribute its pre-reset pickups to the archived candidate,
        // while using precisely the same snapshot as the new segment baseline.
        old->loot = DiffLoot(m_BaselineSnap, resetSnapshot, m_CarryoverLoot);
        old->totalChaos = 0.f;
        for (const auto& item : old->loot) old->totalChaos += item.chaosEach * (float)item.stackCount;
    }
    const int nextSession = m_CurrentSessionId + 1;
    if (!PersistenceResult(m_db.BeginNewSession(old, nextSession, restartMap ? &fresh : nullptr))) return false;
    for (auto& run : nextRuns)
        if (!run.archived) { run.archived = true; run.sessionId = nextSession; }
    if (restartMap) nextRuns.push_back(std::move(fresh));
    m_MapRuns.swap(nextRuns);
    m_CurrentSessionId = nextSession;
    m_ActiveRunIdx = restartMap ? (int)m_MapRuns.size() - 1 : -1;
    m_LootLog.clear();
    m_CarryoverLoot.clear();
    m_BaselineSnap = std::move(resetSnapshot);
    m_BaselineCandidate.clear();
    m_HasBaselineCandidate = false;
    m_LastCurrentSnap = std::move(resetDisplaySnapshot);
    m_HasLastCurrentSnap = ready;
    m_BaselineReady = ready;
    m_NeedBaseline = !ready;
    m_scanner.ResetTiming(resetStamp);
    if (!restartMap) {
        m_MapZoneName.clear();
        m_MapAreaHash.clear();
        m_WasInPassThrough = false;
    }
    m_InMap = restartMap;
    m_IsResume = restartMap && !ready; // same area, no new-map settle delay
    m_AccumulatedDurationSec = 0;
    m_HbHasBaseline = restartMap && m_HbHasCached;
    m_HbBaseline = m_HbHasBaseline ? m_HbCachedTotal : 0;
    m_ItHasBaseline = restartMap && m_ItHasCached;
    m_ItBaseline = m_ItHasBaseline ? m_ItCachedCur : 0;
    m_HbCarryoverHasBaseline = false;
    m_ItCarryoverHasBaseline = false;
    // Gold/kills keep their last observed counters as the new delta boundary;
    // clearing them would either recount old gains or swallow the first pickup.
    // While Esc-paused, anchor to the pause start: the resume shift adds the
    // full pause duration to these, which would otherwise push wall-clock-`now`
    // anchors into the future (negative elapsed until real time catches up).
    auto now = m_Paused ? m_PauseStart : Clock::now();
    m_ZoneEnterTime       = now;
    m_HideoutEnterTime    = now;
    m_SessionActiveSec    = 0;
    m_SessionActiveStart  = now;
    m_SessionTimerRunning = true;
    m_LastObservedSessionSec = 0;
    m_DirtyRunIds.clear();
    m_MetaDirty = false;
    MarkHistoryChanged(true);
    return true;
}

bool FarmTracker::DeleteRunAt(int idx) {
    if (idx < 0 || idx >= (int)m_MapRuns.size()) return false;
    if (idx == m_ActiveRunIdx) return false;
    if (!PersistenceResult(m_db.DeleteRun(m_MapRuns[idx].dbId))) return false;
    EraseRunAt(idx);
    return true;
}

bool FarmTracker::DeleteArchivedSession(int sessionId) {
    if (!PersistenceResult(m_db.DeleteSession(sessionId))) return false;
    for (int i = (int)m_MapRuns.size() - 1; i >= 0; i--) {
        if (m_MapRuns[i].archived && m_MapRuns[i].sessionId == sessionId) {
            EraseRunAt(i);
        }
    }
    return true;
}

bool FarmTracker::DeleteAllArchived() {
    if (!PersistenceResult(m_db.DeleteAllArchived())) return false;
    for (int i = (int)m_MapRuns.size() - 1; i >= 0; i--) {
        if (m_MapRuns[i].archived) {
            EraseRunAt(i);
        }
    }
    return true;
}

bool FarmTracker::HasMeaningfulActivity(const MapRun& run) {
    return !run.loot.empty() || run.durationSec >= 10 || run.goldGain > 0 ||
        run.hivebloodGain > 0 || run.beaconGain > 0 || run.KillsTotal() > 0;
}

void FarmTracker::EraseRunAt(int idx) {
    const auto id = m_MapRuns[idx].dbId;
    m_DirtyRunIds.erase(id);
    m_PendingDiscards.erase(id);
    m_MapRuns.erase(m_MapRuns.begin() + idx);
    if (idx == m_ActiveRunIdx) m_ActiveRunIdx = -1;
    else if (idx < m_ActiveRunIdx) --m_ActiveRunIdx;
    // Other pending rows may share id0; retaining the marker is necessary for
    // unsaved-status as well as for a later retry after an index shift.
    for (const auto& run : m_MapRuns) if (run.dbId == 0) m_DirtyRunIds.insert(0);
    MarkHistoryChanged(true);
}

bool FarmTracker::PersistenceResult(bool success) {
    if (!success) {
        m_LastPersistenceError = m_db.LastError();
        if (m_LastPersistenceError.empty()) m_LastPersistenceError = "Statistics could not be saved";
    } else if (!HasUnsavedChanges()) m_LastPersistenceError.clear();
    return success;
}

void FarmTracker::MarkRunDirty(const MapRun& run) {
    m_DirtyRunIds.insert(run.dbId);
    MarkHistoryChanged();
}

void FarmTracker::MarkHistoryChanged(bool structure) {
    if (structure) {
        ++m_HistoryStructureRevision;
        ++m_HistoryRevision;
        m_LastHistoryPublish = Clock::now();
        m_HistoryPending = false;
    } else {
        m_HistoryPending = true;
        PublishHistoryChanges();
    }
}

void FarmTracker::PublishHistoryChanges() {
    const auto now = Clock::now();
    if (m_HistoryPending && now - m_LastHistoryPublish >= std::chrono::seconds(1)) {
        ++m_HistoryRevision;
        m_LastHistoryPublish = now;
        m_HistoryPending = false;
    }
}

bool FarmTracker::Save() {
    m_LastSaveAttempt = Clock::now();
    if (!m_db.IsOpen() && !PersistenceResult(m_db.Open(m_dir))) return false;
    // Automatic placeholder discards also wait for successful deletion before
    // leaving memory. A busy database must not make rows disappear only in UI.
    while (!m_PendingDiscards.empty()) {
        const auto id = *m_PendingDiscards.begin();
        if (!PersistenceResult(m_db.DeleteRun(id))) return false;
        m_PendingDiscards.erase(id);
        for (int i = (int)m_MapRuns.size() - 1; i >= 0; --i)
            if (m_MapRuns[i].dbId == id) EraseRunAt(i);
    }
    const int seconds = SessionActiveSec();
    for (auto& run : m_MapRuns) {
        if (run.dbId != 0 && m_DirtyRunIds.find(run.dbId) == m_DirtyRunIds.end()) continue;
        const auto oldId = run.dbId;
        if (!m_db.SaveState(&run, seconds, m_CurrentSessionId)) {
            m_DirtyRunIds.insert(run.dbId);
            return PersistenceResult(false);
        }
        m_DirtyRunIds.erase(oldId);
        if (oldId == 0) MarkHistoryChanged(true);
    }
    if (!PersistenceResult(m_db.SaveState(nullptr, seconds, m_CurrentSessionId))) return false;
    m_DirtyRunIds.clear();
    m_MetaDirty = false;
    m_LastPersistenceError.clear();
    return true;
}

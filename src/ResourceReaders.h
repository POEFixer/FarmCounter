#pragma once
// ResourceReaders.h — throttled Hiveblood + Atziri-beacon reads with cached values.
//
// Hiveblood comes from the host (ctx()->Game.GetHiveblood — the host maintains the
// pattern-anchored pointer chain across game patches, so the plugin carries no raw
// offsets). The Atziri beacon counter ("N / 60" temple-entry resource) is scraped
// from UI text: a cached element is revalidated every tick and, when stale (area
// change / game patch moving the panel), re-found with an AMORTIZED bounded BFS
// over the UI tree instead of a fixed child-index path — fixed paths broke on
// 0.5.4b and made the counter (and its gain chime) silently die.
//
// Each visited element is probed through two string slots: the host's
// Ui.GetText (StringIdPtr@0x4C0 — the one CE-verified string slot on 0.5.x) and
// the legacy display-text StdWString at +0x390 the pre-0.5.4b plugin read. The
// slot that matched is remembered per cached element, so whichever field the
// current game build actually renders the counter through, we find it.
#include "sdk/PluginSDK.h"
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

class ResourceReaders {
public:
    struct HbState { bool ok = false; int32_t total = 0; bool cached = false; };
    struct ItState { bool ok = false; int cur = 0; int max = 0; };

    // Call once per frame; rate-limits internally (~6 Hz). allowBeaconSearch
    // gates only the BFS *re-find* (pass false in town/hideout so idle areas
    // never pay for tree sweeps); a cached element keeps refreshing regardless.
    void Tick(const PluginSDK::Context* ctx, bool allowBeaconSearch);
    HbState Hiveblood() const { return m_hb; }
    ItState Incursion() const { return m_it; }  // Atziri beacons (legacy name kept for callers)

    // Parses a beacon counter string. Accepted (must END the string, len <= 48):
    //   quality 2:  "<anything>>> N/60"   (the live UI format)
    //   quality 1:  "<anything> N/60"     (fallback if a patch drops the ">>")
    // The cap is pinned to kBeaconCap (60) to reject chat lines / other "N/M"
    // counters. Public + static so it stays testable without a game.
    static bool ParseBeaconText(const std::string& text, int& cur, int& maxv, int& quality);

private:
    void ReadHiveblood(const PluginSDK::Context* ctx);
    void ReadBeacons(const PluginSDK::Context* ctx, bool allowSearch);
    // Probes both string slots of `el`; on match fills cur/max, which slot
    // (probe: 0 = Ui.GetText, 1 = legacy +0x390) and the match quality.
    bool ProbeElement(const PluginSDK::Context* ctx, uintptr_t el,
                      int& cur, int& maxv, int& probe, int& quality) const;
    void ResetSearch();

    HbState m_hb;
    ItState m_it;

    // Cached beacon element (0 = none) + which string slot matched it.
    uintptr_t m_itElement = 0;
    int       m_itProbe   = 0;

    // Amortized BFS sweep state (a few hundred nodes per tick, so a sweep never
    // stalls a render frame; a full no-hit sweep spans a handful of ticks).
    std::deque<std::pair<uintptr_t, int>> m_bfsQueue;
    int  m_bfsVisited = 0;
    bool m_bfsActive  = false;
    // Best bare-quality candidate seen during the current sweep (a ">>"-marked
    // match wins immediately; this is used only if the sweep ends without one).
    uintptr_t m_bareElement = 0;
    int m_bareProbe = 0, m_bareCur = 0, m_bareMax = 0;

    std::chrono::steady_clock::time_point m_lastTick{};
    std::chrono::steady_clock::time_point m_lastBeaconSearch{};
    bool m_primed = false;
    bool m_searchPrimed = false;
};

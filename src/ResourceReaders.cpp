#include "ResourceReaders.h"
#include <cctype>

using Clock = std::chrono::steady_clock;

// Defensive cap mirrored by the overlay (Overlay.cpp kHivebloodCap).
static constexpr int32_t kHivebloodCap = 100000;

// The beacon counter's fixed capacity. Pinning max == 60 is what keeps chat
// lines ("gg >> 5/6") and other "N/M" counters from being captured; bump this
// if a game patch ever changes the temple-entry cap.
static constexpr int kBeaconCap = 60;
// Counter strings are short ("<markup>>> 17/60"); anything longer is chat/log text.
static constexpr size_t kBeaconMaxTextLen = 48;

// Legacy display-text StdWString offset on the UI element (what the pre-0.5.4b
// plugin read). Probed in addition to the host's Ui.GetText slot — see header.
static constexpr uintptr_t kLegacyTextOffset = 0x390;

// BFS bounds: the counter sits a handful of levels below the root, so depth 12 /
// 4096 nodes is generous. The sweep is AMORTIZED at kBeaconNodesPerTick per
// ~150ms tick (~2 RPM string reads per node), so one tick never spends more
// than ~1ms on it and a full no-hit sweep spreads over ~2s of ticks.
static constexpr int kBeaconBfsMaxDepth   = 12;
static constexpr int kBeaconBfsMaxNodes   = 4096;
static constexpr int kBeaconNodesPerTick  = 300;
static constexpr int kBeaconSearchRetryMs = 3000;

void ResourceReaders::Tick(const PluginSDK::Context* ctx, bool allowBeaconSearch) {
    if (!ctx) return;
    auto now = Clock::now();
    if (m_primed && (now - m_lastTick) < std::chrono::milliseconds(150)) return; // <= ~6 Hz
    m_lastTick = now; m_primed = true;

    ReadHiveblood(ctx);
    ReadGold(ctx);
    ReadBeacons(ctx, allowBeaconSearch);
}

void ResourceReaders::ReadHiveblood(const PluginSDK::Context* c) {
    int32_t hb = 0;
    const bool ok = c->Game.GetHiveblood(hb) && hb > 0 && hb <= kHivebloodCap;
    if (ok)                  { m_hb.ok = true; m_hb.total = hb; m_hb.cached = false; }
    else if (m_hb.total > 0) { m_hb.ok = true; m_hb.cached = true; } // keep last-seen, flag stale
    else                     { m_hb.ok = false; }
}

void ResourceReaders::ReadGold(const PluginSDK::Context* c) {
    // Host-side ServerData read (offsets maintained centrally). GetGold returns
    // 0 when the value is unavailable — a genuinely-zero balance therefore shows
    // as the last-seen value until the first positive read, same tradeoff as
    // Hiveblood's cached fallback.
    const int g = c->Game.GetGold();
    if (g > 0)               { m_gd.ok = true; m_gd.total = g; m_gd.cached = false; }
    else if (m_gd.total > 0) { m_gd.ok = true; m_gd.cached = true; } // keep last-seen, flag stale
    else                     { m_gd.ok = false; }
}

// ── Atziri beacon counter ────────────────────────────────────────────────────

bool ResourceReaders::ParseBeaconText(const std::string& text,
                                      int& cur, int& maxv, int& quality) {
    if (text.empty() || text.size() > kBeaconMaxTextLen) return false;

    // Parses "N/M" starting at `pos` (leading spaces ok). The match must consume
    // the REST of the string (trailing spaces ok) and M must equal kBeaconCap.
    auto parseAnchored = [&](size_t pos) -> bool {
        while (pos < text.size() && text[pos] == ' ') pos++;
        if (pos >= text.size() || !std::isdigit((unsigned char)text[pos])) return false;
        long long cc = 0;
        while (pos < text.size() && std::isdigit((unsigned char)text[pos])) {
            cc = cc * 10 + (text[pos++] - '0');
            if (cc > 1000000) return false;
        }
        if (pos >= text.size() || text[pos] != '/') return false;
        pos++;
        if (pos >= text.size() || !std::isdigit((unsigned char)text[pos])) return false;
        long long mm = 0;
        while (pos < text.size() && std::isdigit((unsigned char)text[pos])) {
            mm = mm * 10 + (text[pos++] - '0');
            if (mm > 1000000) return false;
        }
        while (pos < text.size() && text[pos] == ' ') pos++;
        if (pos != text.size()) return false;          // must end the string
        if (mm != kBeaconCap || cc < 0 || cc > mm) return false;
        cur = (int)cc; maxv = (int)mm;
        return true;
    };

    // Primary: the live UI string is "<markup>>> N/60" — parse after the last ">>".
    size_t pos = text.rfind(">>");
    if (pos != std::string::npos && parseAnchored(pos + 2)) { quality = 2; return true; }

    // Fallback: bare end-anchored "N/60" token (the char before the digits must
    // not be another digit or '/'), for a patch that drops the ">>" marker.
    size_t slash = text.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        size_t start = slash;
        while (start > 0 && std::isdigit((unsigned char)text[start - 1])) start--;
        if (start < slash && (start == 0 || (text[start - 1] != '/' &&
                                             !std::isdigit((unsigned char)text[start - 1])))) {
            if (parseAnchored(start)) { quality = 1; return true; }
        }
    }
    return false;
}

bool ResourceReaders::ProbeElement(const PluginSDK::Context* c, uintptr_t el,
                                   int& cur, int& maxv, int& probe, int& quality) const {
    // Probe 0: the host's one verified string slot (StringIdPtr@0x4C0).
    {
        const std::string txt = c->Ui.GetText(el);
        if (ParseBeaconText(txt, cur, maxv, quality)) { probe = 0; return true; }
    }
    // Probe 1: the legacy display-text slot the pre-0.5.4b plugin read. Garbage
    // memory yields an empty/unparsable string, so this is safe on any element.
    {
        const std::wstring ws = c->Memory.ReadStdWString(el + kLegacyTextOffset);
        if (!ws.empty() && ws.size() <= kBeaconMaxTextLen) {
            std::string txt;
            txt.reserve(ws.size());
            for (wchar_t wc : ws) txt.push_back(wc < 0x80 ? (char)wc : '?');
            if (ParseBeaconText(txt, cur, maxv, quality)) { probe = 1; return true; }
        }
    }
    return false;
}

void ResourceReaders::ResetSearch() {
    m_bfsQueue.clear();
    m_bfsVisited  = 0;
    m_bfsActive   = false;
    m_bareElement = 0;
}

void ResourceReaders::ReadBeacons(const PluginSDK::Context* c, bool allowSearch) {
    int cur = 0, mx = 0, probe = 0, quality = 0;

    // Fast path: the cached element still parses through its remembered slot.
    if (m_itElement) {
        if (ProbeElement(c, m_itElement, cur, mx, probe, quality)) {
            m_itProbe = probe;
            m_it.ok = true; m_it.cur = cur; m_it.max = mx;
            return;
        }
        m_itElement = 0;  // area change / patch re-created the subtree — re-find
    }

    if (!allowSearch) {   // town/hideout: never pay for tree sweeps
        m_it.ok = false;
        ResetSearch();
        return;
    }

    auto now = Clock::now();
    if (!m_bfsActive) {
        // Between sweeps: wait out the retry interval, then start a new sweep.
        if (m_searchPrimed &&
            (now - m_lastBeaconSearch) < std::chrono::milliseconds(kBeaconSearchRetryMs)) {
            m_it.ok = false;
            return;
        }
        uintptr_t root = c->Ui.GetUiRoot();
        if (!root) root = c->Ui.GetGameUiRoot();
        if (!root) { m_it.ok = false; return; }
        ResetSearch();
        m_bfsQueue.emplace_back(root, 0);
        m_bfsActive = true;
        m_lastBeaconSearch = now;
        m_searchPrimed = true;
    }

    // Amortized sweep slice.
    int budget = kBeaconNodesPerTick;
    while (!m_bfsQueue.empty() && m_bfsVisited < kBeaconBfsMaxNodes && budget-- > 0) {
        auto [el, depth] = m_bfsQueue.front();
        m_bfsQueue.pop_front();
        m_bfsVisited++;

        if (ProbeElement(c, el, cur, mx, probe, quality)) {
            if (quality >= 2) {                     // ">>"-marked match: take it
                m_itElement = el; m_itProbe = probe;
                m_it.ok = true; m_it.cur = cur; m_it.max = mx;
                ResetSearch();
                return;
            }
            if (!m_bareElement) {                   // remember first bare match
                m_bareElement = el; m_bareProbe = probe;
                m_bareCur = cur; m_bareMax = mx;
            }
        }

        if (depth < kBeaconBfsMaxDepth)
            for (uintptr_t child : c->Ui.GetChildren(el))
                if (child) m_bfsQueue.emplace_back(child, depth + 1);
    }

    if (m_bfsQueue.empty() || m_bfsVisited >= kBeaconBfsMaxNodes) {
        // Sweep finished without a primary match — accept a bare candidate.
        if (m_bareElement) {
            m_itElement = m_bareElement; m_itProbe = m_bareProbe;
            m_it.ok = true; m_it.cur = m_bareCur; m_it.max = m_bareMax;
        } else {
            m_it.ok = false;
        }
        ResetSearch();                              // next sweep after the retry interval
        return;
    }

    m_it.ok = false;                                // sweep still in progress
}

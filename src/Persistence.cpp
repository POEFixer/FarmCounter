#include "Persistence.h"
#include "PriceProvider.h"
#include "LootLineParse.h" // FormatLootLine / ParseLootLine (also pulls in FarmTypes.h)
#include <fstream>
#include <string>
#include <algorithm>
#include <system_error>

namespace fs = std::filesystem;

// Atomic replace: the stream wrote <target>.tmp; rename it over the target so a
// crash mid-write can never leave a truncated file (map_history.txt is the
// plugin's only store and is rewritten every 15s).
static void CommitTmp(const fs::path& target) {
    fs::path tmp = target;
    tmp += ".tmp";
    std::error_code ec;
    fs::rename(tmp, target, ec);   // replaces an existing target on Windows
    if (ec) fs::remove(tmp, ec);   // failed swap: don't leave the orphan behind
}

static fs::path TmpOf(const fs::path& target) {
    fs::path tmp = target;
    tmp += ".tmp";
    return tmp;
}

// ── Settings (config/settings.txt) ──────────────────────────────────────────

void SaveSettings(const fs::path& dir, const OverlaySettings& s) {
    fs::path cfg = dir / "config";
    std::error_code ec;
    fs::create_directories(cfg, ec);
    const fs::path target = cfg / "settings.txt";
    std::ofstream f(TmpOf(target));
    if (!f.is_open()) return;
    f << "WantsOverlay="      << (s.wantsOverlay      ? 1 : 0) << "\n";
    f << "ShowItems="         << (s.showItems         ? 1 : 0) << "\n";
    f << "ShowUnpriced="      << (s.showUnpriced      ? 1 : 0) << "\n";
    f << "ShowProfitPerHour=" << (s.showProfitPerHour ? 1 : 0) << "\n";
    f << "OverlayCurrency="   << s.overlayCurrency             << "\n";
    f << "WindowAlpha="       << s.windowAlpha                 << "\n";
    f << "WindowPosX="        << s.windowPosX                  << "\n";
    f << "WindowPosY="        << s.windowPosY                  << "\n";
    f << "ItShow="            << (s.itShow     ? 1 : 0)        << "\n";
    f << "ItSound="           << (s.itSound    ? 1 : 0)        << "\n";
    f << "ItVolume="          << s.itVolume                    << "\n";
    f << "ItInMain="          << (s.itInMain   ? 1 : 0)        << "\n";
    f << "ItSeparate="        << (s.itSeparate ? 1 : 0)        << "\n";
    f << "HbInMain="          << (s.hbInMain   ? 1 : 0)        << "\n";
    f << "HbShow="            << (s.hbShow     ? 1 : 0)        << "\n";
    f << "ItOverlayX="        << s.itOverlayX                  << "\n";
    f << "ItOverlayY="        << s.itOverlayY                  << "\n";
    // Per-rarity kill display + Hiveblood near-cap flash (new keys; persisted).
    f << "KcShow="            << (s.kcShow         ? 1 : 0)    << "\n";
    f << "KcShowNormal="      << (s.kcShowNormal   ? 1 : 0)    << "\n";
    f << "KcShowMagic="       << (s.kcShowMagic    ? 1 : 0)    << "\n";
    f << "KcShowRare="        << (s.kcShowRare     ? 1 : 0)    << "\n";
    f << "KcShowUnique="      << (s.kcShowUnique   ? 1 : 0)    << "\n";
    f << "HbWarnNearCap="     << (s.hbWarnNearCap  ? 1 : 0)    << "\n";
    f << "HbWarnThreshold="   << s.hbWarnThreshold             << "\n";
    f << "HbShowMapGains="    << (s.hbShowMapGains ? 1 : 0)    << "\n";
    f.close();
    CommitTmp(target);
}

void LoadSettings(const fs::path& dir, OverlaySettings& out) {
    fs::path p = dir / "config" / "settings.txt";
    if (!fs::exists(p)) return;
    std::ifstream f(p);
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if      (key == "WantsOverlay")      out.wantsOverlay      = (val == "1");
        else if (key == "ShowItems")         out.showItems         = (val == "1");
        else if (key == "ShowUnpriced")      out.showUnpriced      = (val == "1");
        else if (key == "ShowProfitPerHour") out.showProfitPerHour = (val == "1");
        else if (key == "OverlayCurrency")   { try { out.overlayCurrency = std::clamp(std::stoi(val), 0, 2); } catch (...) {} }
        else if (key == "WindowAlpha")       { try { out.windowAlpha = std::stof(val); } catch (...) {} }
        else if (key == "WindowPosX")        { try { out.windowPosX  = std::stof(val); } catch (...) {} }
        else if (key == "WindowPosY")        { try { out.windowPosY  = std::stof(val); } catch (...) {} }
        else if (key == "ItShow")            out.itShow     = (val == "1");
        else if (key == "ItSound")           out.itSound    = (val == "1");
        else if (key == "ItVolume")          { try { out.itVolume = std::clamp(std::stof(val), 0.f, 1.f); } catch (...) {} }
        else if (key == "ItInMain")          out.itInMain   = (val == "1");
        else if (key == "ItSeparate")        out.itSeparate = (val == "1");
        else if (key == "HbInMain")          out.hbInMain   = (val == "1");
        else if (key == "HbShow")            out.hbShow     = (val == "1");
        else if (key == "ItOverlayX")        { try { out.itOverlayX = std::stof(val); } catch (...) {} }
        else if (key == "ItOverlayY")        { try { out.itOverlayY = std::stof(val); } catch (...) {} }
        else if (key == "KcShow")            out.kcShow         = (val == "1");
        else if (key == "KcShowNormal")      out.kcShowNormal   = (val == "1");
        else if (key == "KcShowMagic")       out.kcShowMagic    = (val == "1");
        else if (key == "KcShowRare")        out.kcShowRare     = (val == "1");
        else if (key == "KcShowUnique")      out.kcShowUnique   = (val == "1");
        else if (key == "HbWarnNearCap")     out.hbWarnNearCap  = (val == "1");
        else if (key == "HbWarnThreshold")   { try { out.hbWarnThreshold = std::clamp(std::stoi(val), 50000, 100000); } catch (...) {} }
        else if (key == "HbShowMapGains")    out.hbShowMapGains = (val == "1");
        // Unknown keys are ignored (incl. legacy League= / RefreshIntervalMin= —
        // the core app owns league + refresh interval now).
    }
}

// ── Map history (config/map_history.txt) ────────────────────────────────────
//
// Format:
//   session_active_sec=<seconds>
//   [per run:]
//     map=<EscapeField(name)>
//     dur=<seconds>
//     chaos=<float>
//     exrate=<float>
//     archived=<0|1>
//     session=<id>
//     [if hivebloodGain>0] hiveblood=<int>
//     [per loot:] loot=<FormatLootLine(e)>
//     ---

void SaveMapHistory(const fs::path& dir, const std::vector<MapRun>& runs, int sessionActiveSec) {
    fs::path cfg = dir / "config";
    std::error_code ec;
    fs::create_directories(cfg, ec);
    const fs::path target = cfg / "map_history.txt";
    std::ofstream f(TmpOf(target));
    if (!f.is_open()) return;
    f << "session_active_sec=" << sessionActiveSec << "\n";
    for (const auto& r : runs) {
        f << "map="      << EscapeField(r.mapName) << "\n";
        f << "dur="      << r.durationSec          << "\n";
        f << "chaos="    << r.totalChaos           << "\n";
        f << "exrate="   << r.exaltedRate          << "\n";
        f << "archived=" << (r.archived ? 1 : 0)   << "\n";
        f << "session="  << r.sessionId            << "\n";
        if (r.hivebloodGain > 0)
            f << "hiveblood=" << r.hivebloodGain << "\n";
        for (const auto& e : r.loot)
            f << "loot=" << FormatLootLine(e) << "\n";
        f << "---\n";
    }
    f.close();
    CommitTmp(target);
}

void LoadMapHistory(const fs::path& dir, std::vector<MapRun>& runs,
                    int& sessionActiveSec, int& sessionIdMax) {
    fs::path p = dir / "config" / "map_history.txt";
    if (!fs::exists(p)) return;
    std::ifstream f(p);
    if (!f.is_open()) return;
    runs.clear();
    sessionActiveSec = 0; // reset from file; absent line ⇒ 0 (matches old loader)
    MapRun cur;
    bool inRun = false;
    std::string line;
    while (std::getline(f, line)) {
        if (line == "---") {
            if (inRun && !cur.mapName.empty()) runs.push_back(cur);
            cur = MapRun{};
            inRun = false;
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "session_active_sec") {
            try { sessionActiveSec = std::stoi(val); } catch (...) {}
            continue;
        }
        inRun = true;
        if      (key == "map")       { cur.mapName = UnescapeField(val); }
        else if (key == "dur")       { try { cur.durationSec   = std::stoi(val); } catch (...) {} }
        else if (key == "chaos")     { try { cur.totalChaos    = std::stof(val); } catch (...) {} }
        else if (key == "exrate")    { try { cur.exaltedRate   = std::stof(val); } catch (...) {} }
        else if (key == "archived")  { cur.archived = (val == "1"); }
        else if (key == "session")   { try { cur.sessionId     = std::stoi(val); } catch (...) {} }
        else if (key == "hiveblood") { try { cur.hivebloodGain = std::stoi(val); } catch (...) {} }
        else if (key == "loot")      { LootEntry e; if (ParseLootLine(val, e)) cur.loot.push_back(e); }
    }
    // Trailing run with no closing "---".
    if (inRun && !cur.mapName.empty()) runs.push_back(cur);
    // Raise the session-id high-water mark (never lowered).
    for (const auto& r : runs)
        if (r.sessionId > sessionIdMax) sessionIdMax = r.sessionId;
}

// ── Custom prices (config/custom_prices.txt) ────────────────────────────────
//
// Line format: <displayName>|<baseType>=<chaos>

void SaveCustomPrices(const fs::path& dir, const PriceProvider& pp) {
    fs::path cfg = dir / "config";
    std::error_code ec;
    fs::create_directories(cfg, ec);
    const fs::path target = cfg / "custom_prices.txt";
    std::ofstream f(TmpOf(target));
    if (!f.is_open()) return;
    // PriceProvider's map accessors are non-const; we only read through them here.
    PriceProvider& m = const_cast<PriceProvider&>(pp);
    const auto& custom = m.Custom();
    const auto& names  = m.CustomNames();
    const auto& bases  = m.CustomBase();
    for (const auto& [key, chaos] : custom) {
        auto itN = names.find(key);
        auto itB = bases.find(key);
        const std::string& uname = (itN != names.end()) ? itN->second : key;
        std::string        bname = (itB != bases.end()) ? itB->second : std::string();
        f << uname << "|" << bname << "=" << chaos << "\n";
    }
    f.close();
    CommitTmp(target);
}

void LoadCustomPrices(const fs::path& dir, PriceProvider& pp) {
    fs::path p = dir / "config" / "custom_prices.txt";
    if (!fs::exists(p)) return;
    std::ifstream f(p);
    if (!f.is_open()) return;
    auto& custom = pp.Custom();
    auto& names  = pp.CustomNames();
    auto& bases  = pp.CustomBase();
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.rfind('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string namepart = line.substr(0, eq);
        std::string val      = line.substr(eq + 1);
        auto pipe = namepart.find('|');
        std::string uname = (pipe != std::string::npos) ? namepart.substr(0, pipe) : namepart;
        std::string bname = (pipe != std::string::npos) ? namepart.substr(pipe + 1) : std::string();
        if (uname.empty()) continue;
        try {
            float chaos = std::stof(val);
            if (chaos > 0.0f) {
                // CRITICAL: key by PriceProvider::ToLower(name) — PriceProvider::Lookup
                // probes the custom map with ToLower(name), so any other key never matches.
                std::string key = PriceProvider::ToLower(uname);
                custom[key] = chaos;
                names[key]  = uname;
                bases[key]  = bname;
            }
        } catch (...) {}
    }
}

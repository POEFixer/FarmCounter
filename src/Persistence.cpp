#include "Persistence.h"
#include "PriceProvider.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <cmath>
#include <limits>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// Atomic replace: write <target>.tmp, then rename over the target so a crash
// mid-write can never leave a truncated file.
void WriteAtomic(const fs::path& target, const std::string& content) {
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f.is_open()) return;
        f.write(content.data(), (std::streamsize)content.size());
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);   // replaces an existing target on Windows
    if (ec) fs::remove(tmp, ec);   // failed swap: don't leave the orphan behind
}

// Type-checked json getters — nlohmann's value() throws on a type mismatch, and
// config files are user-editable, so never trust the stored type.
bool  GetB(const json& j, const char* k, bool  def) { auto it = j.find(k); return (it != j.end() && it->is_boolean())        ? it->get<bool>()  : def; }
int GetI(const json& j, const char* k, int def) {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return def;
    const double value = it->get<double>();
    if (!std::isfinite(value) || value < (std::numeric_limits<int>::min)() ||
        value > (std::numeric_limits<int>::max)()) return def;
    return static_cast<int>(value);
}
float GetF(const json& j, const char* k, float def) {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_number()) return def;
    const double value = it->get<double>();
    if (!std::isfinite(value) || std::fabs(value) > (std::numeric_limits<float>::max)()) return def;
    return static_cast<float>(value);
}
float LegacyFloat(const std::string& text, float def) {
    try {
        size_t consumed = 0;
        const float value = std::stof(text, &consumed);
        return consumed == text.size() && std::isfinite(value) ? value : def;
    } catch (...) { return def; }
}
std::string GetS(const json& j, const char* k)      { auto it = j.find(k); return (it != j.end() && it->is_string())         ? it->get<std::string>() : std::string(); }

// ── Legacy .txt fallbacks (read-only; kept so an update preserves configs) ───

void LoadSettingsLegacy(const fs::path& p, OverlaySettings& out) {
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
        else if (key == "WindowAlpha")       out.windowAlpha = std::clamp(LegacyFloat(val, out.windowAlpha), 0.1f, 1.f);
        else if (key == "WindowPosX")        out.windowPosX = LegacyFloat(val, out.windowPosX);
        else if (key == "WindowPosY")        out.windowPosY = LegacyFloat(val, out.windowPosY);
        else if (key == "ItShow")            out.itShow     = (val == "1");
        else if (key == "ItSound")           out.itSound    = (val == "1");
        else if (key == "ItVolume")          out.itVolume = std::clamp(LegacyFloat(val, out.itVolume), 0.f, 1.f);
        else if (key == "ItInMain")          out.itInMain   = (val == "1");
        else if (key == "ItSeparate")        out.itSeparate = (val == "1");
        else if (key == "HbInMain")          out.hbInMain   = (val == "1");
        else if (key == "HbShow")            out.hbShow     = (val == "1");
        else if (key == "ItOverlayX")        out.itOverlayX = LegacyFloat(val, out.itOverlayX);
        else if (key == "ItOverlayY")        out.itOverlayY = LegacyFloat(val, out.itOverlayY);
        else if (key == "KcShow")            out.kcShow         = (val == "1");
        else if (key == "KcShowNormal")      out.kcShowNormal   = (val == "1");
        else if (key == "KcShowMagic")       out.kcShowMagic    = (val == "1");
        else if (key == "KcShowRare")        out.kcShowRare     = (val == "1");
        else if (key == "KcShowUnique")      out.kcShowUnique   = (val == "1");
        else if (key == "HbWarnNearCap")     out.hbWarnNearCap  = (val == "1");
        else if (key == "HbWarnThreshold")   { try { out.hbWarnThreshold = std::clamp(std::stoi(val), 50000, 100000); } catch (...) {} }
        else if (key == "HbShowMapGains")    out.hbShowMapGains = (val == "1");
        // Unknown keys are ignored (incl. legacy League= / RefreshIntervalMin=).
    }
}

bool LoadCustomPricesLegacy(const fs::path& p, PriceProvider& pp) {
    std::ifstream f(p);
    if (!f.is_open()) return false;
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
            float exalts = LegacyFloat(val, 0.f);
            if (exalts > 0.0f) {
                const std::string key = PriceProvider::ToLower(uname);
                custom[key] = exalts;
                names[key]  = uname;
                bases[key]  = bname;
            }
        } catch (...) {}
    }
    return !f.bad();
}

} // namespace

// ── Settings (config/settings.json) ─────────────────────────────────────────

void SaveSettings(const fs::path& dir, const OverlaySettings& s) {
    if (dir.empty()) return;   // no plugin root yet — never write into the host's CWD
    fs::path cfg = dir / "config";
    std::error_code ec;
    fs::create_directories(cfg, ec);
    json j;
    j["wantsOverlay"]      = s.wantsOverlay;
    j["showItems"]         = s.showItems;
    j["visibleItemRows"]   = std::clamp(s.visibleItemRows, 1, 50);
    j["showUnpriced"]      = s.showUnpriced;
    j["showProfitPerHour"] = s.showProfitPerHour;
    j["showXpPerHour"]     = s.showXpPerHour;
    j["goldShow"]          = s.goldShow;
    j["overlayCurrency"]   = s.overlayCurrency;
    j["windowAlpha"]       = s.windowAlpha;
    j["windowPosX"]        = s.windowPosX;
    j["windowPosY"]        = s.windowPosY;
    j["itShow"]            = s.itShow;
    j["itSound"]           = s.itSound;
    j["itVolume"]          = s.itVolume;
    j["itInMain"]          = s.itInMain;
    j["itSeparate"]        = s.itSeparate;
    j["itOverlayX"]        = s.itOverlayX;
    j["itOverlayY"]        = s.itOverlayY;
    j["hbInMain"]          = s.hbInMain;
    j["hbShow"]            = s.hbShow;
    j["kcShow"]            = s.kcShow;
    j["kcShowNormal"]      = s.kcShowNormal;
    j["kcShowMagic"]       = s.kcShowMagic;
    j["kcShowRare"]        = s.kcShowRare;
    j["kcShowUnique"]      = s.kcShowUnique;
    j["kcShowRogue"]       = s.kcShowRogue;
    j["hbWarnNearCap"]     = s.hbWarnNearCap;
    j["hbWarnThreshold"]   = s.hbWarnThreshold;
    j["hbShowMapGains"]    = s.hbShowMapGains;
    // error_handler replace: a stray invalid-UTF-8 byte must never throw here —
    // this runs inside the host's ImGui frame (a throw corrupts host UI state).
    WriteAtomic(cfg / "settings.json",
                j.dump(4, ' ', false, json::error_handler_t::replace) + "\n");
}

void LoadSettings(const fs::path& dir, OverlaySettings& out) {
    const fs::path jsonPath = dir / "config" / "settings.json";
    if (fs::exists(jsonPath)) {
        std::ifstream f(jsonPath);
        if (!f.is_open()) return;
        json j = json::parse(f, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) return;
        out.wantsOverlay      = GetB(j, "wantsOverlay",      out.wantsOverlay);
        out.showItems         = GetB(j, "showItems",         out.showItems);
        // Clamp before converting: an edited JSON number may exceed int range.
        if (auto it = j.find("visibleItemRows"); it != j.end() && it->is_number())
            out.visibleItemRows = static_cast<int>(std::clamp(it->get<double>(), 1.0, 50.0));
        out.showUnpriced      = GetB(j, "showUnpriced",      out.showUnpriced);
        out.showProfitPerHour = GetB(j, "showProfitPerHour", out.showProfitPerHour);
        out.showXpPerHour     = GetB(j, "showXpPerHour",     out.showXpPerHour);
        out.goldShow          = GetB(j, "goldShow",          out.goldShow);
        out.overlayCurrency   = std::clamp(GetI(j, "overlayCurrency", out.overlayCurrency), 0, 2);
        out.windowAlpha       = std::clamp(GetF(j, "windowAlpha", out.windowAlpha), 0.1f, 1.f);
        out.windowPosX        = GetF(j, "windowPosX",        out.windowPosX);
        out.windowPosY        = GetF(j, "windowPosY",        out.windowPosY);
        out.itShow            = GetB(j, "itShow",            out.itShow);
        out.itSound           = GetB(j, "itSound",           out.itSound);
        out.itVolume          = std::clamp(GetF(j, "itVolume", out.itVolume), 0.f, 1.f);
        out.itInMain          = GetB(j, "itInMain",          out.itInMain);
        out.itSeparate        = GetB(j, "itSeparate",        out.itSeparate);
        out.itOverlayX        = GetF(j, "itOverlayX",        out.itOverlayX);
        out.itOverlayY        = GetF(j, "itOverlayY",        out.itOverlayY);
        out.hbInMain          = GetB(j, "hbInMain",          out.hbInMain);
        out.hbShow            = GetB(j, "hbShow",            out.hbShow);
        out.kcShow            = GetB(j, "kcShow",            out.kcShow);
        out.kcShowNormal      = GetB(j, "kcShowNormal",      out.kcShowNormal);
        out.kcShowMagic       = GetB(j, "kcShowMagic",       out.kcShowMagic);
        out.kcShowRare        = GetB(j, "kcShowRare",        out.kcShowRare);
        out.kcShowUnique      = GetB(j, "kcShowUnique",      out.kcShowUnique);
        out.kcShowRogue       = GetB(j, "kcShowRogue",       out.kcShowRogue);
        out.hbWarnNearCap     = GetB(j, "hbWarnNearCap",     out.hbWarnNearCap);
        out.hbWarnThreshold   = std::clamp(GetI(j, "hbWarnThreshold", out.hbWarnThreshold), 50000, 100000);
        out.hbShowMapGains    = GetB(j, "hbShowMapGains",    out.hbShowMapGains);
        return;
    }
    // One-time legacy fallback: pre-JSON installs keep their configuration (the
    // next SaveSettings writes settings.json and the .txt is never read again).
    LoadSettingsLegacy(dir / "config" / "settings.txt", out);
}

// ── Custom prices (config/custom_prices.json) ───────────────────────────────
//
// Array of { "name": display name, "base": base type ("" for non-unique),
//            "exalts": price in exalted orbs }.

void SaveCustomPrices(const fs::path& dir, const PriceProvider& pp) {
    if (dir.empty()) return;   // no plugin root yet — never write into the host's CWD
    fs::path cfg = dir / "config";
    std::error_code ec;
    fs::create_directories(cfg, ec);
    // PriceProvider's map accessors are non-const; we only read through them here.
    PriceProvider& m = const_cast<PriceProvider&>(pp);
    json arr = json::array();
    for (const auto& [key, exalts] : m.Custom()) {
        if (!std::isfinite(exalts) || exalts <= 0.f) continue;
        auto itN = m.CustomNames().find(key);
        auto itB = m.CustomBase().find(key);
        json e;
        e["name"]   = (itN != m.CustomNames().end()) ? itN->second : key;
        e["base"]   = (itB != m.CustomBase().end())  ? itB->second : std::string();
        e["exalts"] = exalts;
        arr.push_back(std::move(e));
    }
    // error_handler replace: see SaveSettings — never throw inside the frame.
    WriteAtomic(cfg / "custom_prices.json",
                arr.dump(4, ' ', false, json::error_handler_t::replace) + "\n");
}

void LoadCustomPrices(const fs::path& dir, PriceProvider& pp) {
    // Re-enable reloads this same provider. Build a replacement so deleted
    // overrides disappear, while a malformed/unreadable file preserves values.
    PriceProvider replacement;
    const fs::path jsonPath = dir / "config" / "custom_prices.json";
    std::error_code ec;
    const bool hasJson = fs::exists(jsonPath, ec);
    if (ec) return;
    if (hasJson) {
        std::ifstream f(jsonPath);
        if (!f.is_open()) return;
        json arr = json::parse(f, nullptr, /*allow_exceptions=*/false);
        if (arr.is_discarded() || !arr.is_array()) return;
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string name = GetS(e, "name");
            const float exalts     = GetF(e, "exalts", 0.f);
            if (name.empty() || exalts <= 0.f) continue;
            // CRITICAL: key by PriceProvider::ToLower(name) — Lookup probes the
            // custom map with ToLower(name), so any other key never matches.
            const std::string key = PriceProvider::ToLower(name);
            replacement.Custom()[key]      = exalts;
            replacement.CustomNames()[key] = name;
            replacement.CustomBase()[key]  = GetS(e, "base");
        }
        if (f.bad()) return;
    } else {
        const auto legacyPath = dir / "config" / "custom_prices.txt";
        const bool hasLegacy = fs::exists(legacyPath, ec);
        if (ec || (hasLegacy && !LoadCustomPricesLegacy(legacyPath, replacement))) return;
    }
    pp.Custom().swap(replacement.Custom());
    pp.CustomNames().swap(replacement.CustomNames());
    pp.CustomBase().swap(replacement.CustomBase());
}

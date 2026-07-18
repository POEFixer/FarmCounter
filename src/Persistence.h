#pragma once
// Persistence.h — FarmCounter disk I/O for overlay settings, map-run history and
// custom prices. Every file lives under <dir>/config/. This header is deliberately
// std-only (no ImGui, no Windows, no SDK): overlay window/overlay positions are plain
// floats so the settings struct carries no ImGui dependency.
#include "FarmTypes.h"
#include <filesystem>
#include <string>
#include <vector>

class PriceProvider; // defined in PriceProvider.h (included by Persistence.cpp)

// Overlay / UI settings persisted to config/settings.txt.
//
// This field set round-trips every key the old monolith's SaveSettings/LoadSettings
// handled, EXCEPT League and RefreshIntervalMin which are intentionally dropped (the
// core owns league + refresh interval now). The two "*InMain" fields back legacy keys
// that the old loader still parsed; they are persisted for round-trip fidelity even
// though they drive no overlay logic.
struct OverlaySettings {
    bool  wantsOverlay      = false;
    bool  showItems         = true;
    bool  showUnpriced      = false;
    bool  showProfitPerHour = true;
    int   overlayCurrency   = 0;
    float windowAlpha       = 0.9f;
    float windowPosX        = -1.0f;
    float windowPosY        = -1.0f;
    bool  itShow            = true;
    bool  itSound           = true;
    float itVolume          = 0.5f;
    bool  itInMain          = false; // legacy key "ItInMain" (round-tripped; no live logic)
    bool  itSeparate        = false;
    float itOverlayX        = -1.0f;
    float itOverlayY        = -1.0f;
    bool  hbInMain          = false; // legacy key "HbInMain" (round-tripped; no live logic)
    bool  hbShow            = true;  // "Show Hiveblood in overlay"
    // Per-rarity kill display + Hiveblood near-cap flash. These mirror the old
    // monolith's runtime-only members; now PERSISTED (intended improvement). Old
    // settings files lacking these keys keep the defaults below.
    bool  kcShow            = true;  // master toggle for the kills element
    bool  kcShowNormal      = true;
    bool  kcShowMagic       = true;
    bool  kcShowRare        = true;
    bool  kcShowUnique      = true;
    bool  hbWarnNearCap     = true;  // flash Hiveblood when near cap
    int   hbWarnThreshold   = 95000; // flash trigger (clamped [50000,100000] on load)
    bool  hbShowMapGains    = true;  // show the Hiveblood "(+N)" per-map gain label
};

// config/settings.txt. LoadSettings ignores unknown keys, so an old file that still
// carries League= / RefreshIntervalMin= loads cleanly.
void LoadSettings(const std::filesystem::path& dir, OverlaySettings& out);
void SaveSettings(const std::filesystem::path& dir, const OverlaySettings& s);

// config/map_history.txt. sessionActiveSec is reset from the file (out); sessionIdMax is
// only ever raised to the highest sessionId seen (in/out high-water mark).
void LoadMapHistory(const std::filesystem::path& dir, std::vector<MapRun>& runs,
                    int& sessionActiveSec, int& sessionIdMax);
void SaveMapHistory(const std::filesystem::path& dir, const std::vector<MapRun>& runs,
                    int sessionActiveSec);

// config/custom_prices.txt. The PriceProvider custom maps are keyed by
// PriceProvider::ToLower(name) so PriceProvider::Lookup (which probes with ToLower) finds them.
void LoadCustomPrices(const std::filesystem::path& dir, PriceProvider& pp);
void SaveCustomPrices(const std::filesystem::path& dir, const PriceProvider& pp);

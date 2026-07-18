#pragma once
// Persistence.h — FarmCounter disk I/O for overlay settings and custom prices.
// Both live under <dir>/config/ as JSON (settings.json / custom_prices.json),
// written atomically (tmp + rename). Loaders fall back to the legacy .txt
// formats once, so an upgrade keeps the user's existing configuration.
// Map-run statistics are NOT here anymore — they live in SQLite (FarmDb.h).
// This header is deliberately std-only (no ImGui, no Windows, no SDK).
#include <filesystem>
#include <string>

class PriceProvider; // defined in PriceProvider.h (included by Persistence.cpp)

// Overlay / UI settings persisted to config/settings.json.
struct OverlaySettings {
    bool  wantsOverlay      = false;
    bool  showItems         = true;
    bool  showUnpriced      = false;
    bool  showProfitPerHour = true;
    bool  goldShow          = true;  // gold total + map gain row in the overlay
    int   overlayCurrency   = 0;
    float windowAlpha       = 0.9f;
    float windowPosX        = -1.0f;
    float windowPosY        = -1.0f;
    bool  itShow            = true;
    bool  itSound           = true;
    float itVolume          = 0.5f;
    bool  itInMain          = false; // legacy key (round-tripped; no live logic)
    bool  itSeparate        = false;
    float itOverlayX        = -1.0f;
    float itOverlayY        = -1.0f;
    bool  hbInMain          = false; // legacy key (round-tripped; no live logic)
    bool  hbShow            = true;  // "Show Hiveblood in overlay"
    bool  kcShow            = true;  // master toggle for the kills element
    bool  kcShowNormal      = true;
    bool  kcShowMagic       = true;
    bool  kcShowRare        = true;
    bool  kcShowUnique      = true;
    bool  hbWarnNearCap     = true;  // flash Hiveblood when near cap
    int   hbWarnThreshold   = 95000; // flash trigger (clamped [50000,100000] on load)
    bool  hbShowMapGains    = true;  // show the Hiveblood "(+N)" per-map gain label
};

// config/settings.json (fallback: legacy config/settings.txt).
void LoadSettings(const std::filesystem::path& dir, OverlaySettings& out);
void SaveSettings(const std::filesystem::path& dir, const OverlaySettings& s);

// config/custom_prices.json (fallback: legacy config/custom_prices.txt). The
// PriceProvider custom maps are keyed by PriceProvider::ToLower(name) so
// PriceProvider::Lookup (which probes with ToLower) finds them.
void LoadCustomPrices(const std::filesystem::path& dir, PriceProvider& pp);
void SaveCustomPrices(const std::filesystem::path& dir, const PriceProvider& pp);

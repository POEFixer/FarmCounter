#pragma once
// Configuration tabs and routing to the shell-owned Statistics and Zones views.
// Tracking and database mutations remain FarmTracker's responsibility. The host
// owns league/pricing refresh; custom prices are keyed by PriceProvider::ToLower.
#include "../FarmTracker.h"
#include "../PriceProvider.h"
#include "../ZoneNames.h"
#include "../KillCounter.h"
#include "../ResourceReaders.h"
#include "../IconTextures.h"
#include "../Persistence.h"   // OverlaySettings + SaveSettings/SaveCustomPrices
#include <filesystem>
#include <functional>

class StatisticsView;
class ZonesView;

// Aggregate of everything the settings tabs touch. Pointers are owned by the
// plugin shell. `settings` / `prices` / `zones` / `kills` are non-const so the
// tabs can edit + persist them. `dir` is the plugin root directory (Persistence
// appends config/; ZoneNames writes zone_names.json directly under it).
struct SettingsDeps {
    FarmTracker*          tracker   = nullptr;
    PriceProvider*        prices    = nullptr;
    OverlaySettings*      settings  = nullptr;
    ZoneNames*            zones     = nullptr;
    KillCounter*          kills       = nullptr;
    ResourceReaders*      resources   = nullptr;
    IconTextures*         icons       = nullptr;
    std::filesystem::path dir;
    std::function<void(float)> onTestSound;  // plays test tone at given volume
    StatisticsView* statisticsView = nullptr;
    ZonesView* zonesView = nullptr;
};

void RenderSettings(const SettingsDeps& d);

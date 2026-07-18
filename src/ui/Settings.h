#pragma once
// Settings.h — FarmCounter modernized settings tab bar (icon-labelled tabs).
//
// RenderSettings() draws the plugin's configuration window: an icon-labelled
// ImGui tab bar (Settings · Custom Prices · Statistics · Zones · Hiveblood ·
// Incursion · Kills). It is a behavior-preserving port of the old monolith's
// DrawSettings()/DrawSettingsTab()/DrawCustomPricesTab()/DrawStatisticsTab()/
// DrawZoneNamesTab()/DrawHivebloodTab()/DrawIncursionTab()/DrawKillCountTab()
// (FarmCounter.cpp:132-637) re-skinned with SeparatorText card sections and
// FcGlyph tab icons, with these deliberate changes:
//   - the League combo + Refresh(min) slider are removed (the core owns league
//     and refresh interval now); the price block is a read-only core-status
//     readout (PriceProvider::Status()).
//   - custom prices are sourced from PriceProvider's three custom maps and keyed
//     by PriceProvider::ToLower(name) so PriceProvider::Lookup finds them.
//
// The function is pure render: it READS the model/services and WRITES through the
// (non-const) OverlaySettings / PriceProvider / ZoneNames it is handed, persisting
// each change immediately via the Persistence free functions / ZoneNames::Save().
// The shell owns the pointers; the settings renderer never takes ownership and
// keeps its transient text-edit buffers as function-local statics.
#include "../FarmTracker.h"
#include "../PriceProvider.h"
#include "../ZoneNames.h"
#include "../KillCounter.h"
#include "../ResourceReaders.h"
#include "../Persistence.h"   // OverlaySettings + SaveSettings/SaveCustomPrices
#include <filesystem>
#include <functional>

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
    std::filesystem::path dir;
    std::function<void(float)> onTestSound;  // plays test tone at given volume
};

void RenderSettings(const SettingsDeps& d);

#pragma once
// Overlay.h — FarmCounter in-game HUD overlay.
//
// RenderOverlay() draws the non-interactive farming HUD: an accent header bar
// (map name · timer · profit), a compact stat strip (session · profit/h · kills
// · maps), inline Hiveblood/Incursion resource bars, and the per-item loot list.
// Both windows (the counter and the separate beacons bar) go through the host's
// informational windows (PluginInfoWindow.h): the host keeps their places and
// draws their gear menu, whose Close clears settings->showWindow / itSeparate
// (the shell saves that). All tracking, baselines and resource reads happen
// elsewhere (FarmTracker / ResourceReaders).
//
// The shell gates game state and supplies the cached lifetime map-category
// count, so drawing that count never rescans the saved history.
#include "../FarmTracker.h"
#include "../PriceProvider.h"
#include "../IconTextures.h"
#include "../KillCounter.h"
#include "../ResourceReaders.h"
#include "../ZoneNames.h"
#include "../Persistence.h"

// Aggregate of everything the overlay needs. Pointers are owned by the plugin
// shell; the overlay never takes ownership. `settings` is non-const so a window's
// Close can clear its show flag (the shell persists it).
// `zones` is optional (null-tolerant): when present the overlay resolves display
// names via ZoneNames::Display(); when null it falls back to the raw zone name.
struct OverlayDeps {
    FarmTracker*      tracker   = nullptr;
    PriceProvider*    prices    = nullptr;
    IconTextures*     icons     = nullptr;
    KillCounter*      kills     = nullptr;
    ResourceReaders*  resources = nullptr;
    ZoneNames*        zones     = nullptr;   // optional; for Display() name resolution
    OverlaySettings*  settings  = nullptr;

    // Settings-open handshake (mirrors old m_SettingsOpen capture+reset):
    //  - settingsOpenThisFrame: true if the settings window drew this frame.
    //    When true the in-game counter becomes movable + mouse-interactive (and
    //    shows its gear menu on hover); when false it is click-through
    //    (NoMove|NoMouseInputs). Out of the game the host makes it movable.
    //  - outSettingsConsumed: the shell's persistent flag, reset to false here so
    //    it must be re-set by the settings renderer each frame it is open.
    bool  settingsOpenThisFrame = false;
    bool* outSettingsConsumed   = nullptr;
    size_t currentMapLifetimeRuns = 0;
    const std::string* currentMapCategory = nullptr;
    const PluginSDK::OverlayService* overlay = nullptr;   // ctx()->Overlay: the windows' host
};

void RenderOverlay(const OverlayDeps& d);

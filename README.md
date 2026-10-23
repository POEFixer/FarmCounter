# FarmCounter

Current version: 1.0.2

Requires POE2Fixer 366 or newer.

FarmCounter records map runs, signed loot changes, prices, map time, kills,
gold, Hiveblood and Atziri beacons. XP/hour and current-map XP are independent
overlay measurements.

## Statistics

Choose a map type on the left to compare all recorded runs of that type.
Simulacrum arenas are grouped together while retaining their individual arena
names. Use session and time filters to narrow the history, and sort the run
table by profit, duration, rate or other columns. The optional recent-run chart
shows the latest completed runs; scrolling the table reaches the full selection.

Select a run for its complete loot, resources and map modifiers. Loot totals
combine the selected runs at their stored item values, including spending.
Exalted display uses each run's saved exchange rate. Divine display converts
stored chaos values using the currently available divine rate. Summary values
and individual rows follow the same conversion. Average, best and worst values
exclude the unfinished current run; totals include it.

New session archives current records and starts a fresh counting segment on
the current map. It does not require leaving that map or erase older sessions.
A reset in town/hideout starts recording on the next map. The reset uses the
latest validated inventory boundary, so existing inventory is not counted again.
XP/hour reset remains separate. Deleting history requires inline confirmation;
the active or suspended run cannot be deleted while it is resumable.

The overlay's repeat-arrow icon marks the count of recorded runs in that map category
across saved sessions, including the current run. Returning through hideout
resumes the same record; New session deliberately creates a new record.

## Zones

Zones controls friendly display names in the overlay and history. Current game
names are built in; custom aliases take precedence without changing map identity
or merging unrelated maps. Search for an area and click Edit, then Save or press
Enter. Reset restores the game name. All areas expands the default map/visited
list; unused entries are hidden unless requested. Unknown future area IDs can
be added manually. Existing `zone_names.json` aliases are preserved.

The catalog is generated from the game's own area data and refreshed after
game patches.

## Saved data and host compatibility

History lives in `data/farmstats.db`; settings and custom prices live under
`config/`. Keep these files when replacing the plugin DLL. While the overlay
runs, `farmstats.db-wal` and `farmstats.db-shm` sit next to `farmstats.db`; keep
the three together (or close the overlay first) when you copy the data folder.
Database write errors are shown in Statistics; a failed reset/delete keeps the
previous state.

Use the matching updated Fixer executable and FarmCounter DLL. The current host
supplies a coherent inventory snapshot with readiness, scan stamp and area
counter; this distinguishes an empty backpack from unread or stale data.
Older SDK v6 hosts remain load-compatible, but their conservative fallback
cannot validate empty inventory readings.

## Install

Install it from POE2Fixer → Plugins → Plugin Manager (one click, updates included), or download `FarmCounter.zip` from [Releases](https://github.com/POEFixer/FarmCounter/releases/latest) and extract it into the POE2Fixer folder (it creates `Plugins\FarmCounter\`). Then enable the plugin in the Plugins tab.

## Build from source

This repository is the plugin folder from the POE2Fixer source tree (`Plugins/FarmCounter/`) together with the host files it compiles against, kept at the same relative paths:

- `POEFixer/plugin_sdk/` - the POE2Fixer Plugin SDK v6 headers (`PluginAbi.h`, `PluginSDK.h`)
- `POEFixer/imgui/` - the Dear ImGui sources the DLL compiles against
- `POEFixer/core/` - small self-contained helper headers used by the plugin

1. Install Visual Studio 2022 with the "Desktop development with C++" workload (MSVC v143, Windows 10/11 SDK).
2. Open `FarmCounter.sln`, select **Release | x64** and build the solution.
3. The plugin is written to `x64\Release\Plugins\FarmCounter\FarmCounter.dll` (Release builds carry no debug information, see `Directory.Build.targets`).
4. Copy `FarmCounter.dll` into `Plugins\FarmCounter\` next to the POE2Fixer executable.

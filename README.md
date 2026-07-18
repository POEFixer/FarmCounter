# FarmCounter — POE2Fixer Plugin

A farming session tracker for Path of Exile 2, built on the POE2Fixer v6
Plugin SDK. It diffs your backpack against a per-map baseline to show what
you actually looted, prices it through the host's central price service,
and keeps a persistent per-map / per-session history.

## What It Does

- **Live loot tracking** — on map enter the plugin snapshots your backpack
  (after a stability gate, so the game's lazy inventory fill can't poison the
  baseline) and then shows every item gained or lost, with stack counts and
  item icons.
- **Pricing via the host** — values come from POEFixer's central price
  service (poe2scout DB, league configured in POEFixer Settings). The plugin
  itself performs **no HTTP**. Items the DB misses can be given **custom
  prices** (stored in exalts) from the settings tab.
- **Map-run history & sessions** — per-run duration, total value, loot list;
  archive everything into numbered sessions with top-drops summaries and
  profit/hour. Survives restarts (`config/map_history.txt`).
- **Hideout round-trips handled** — leaving a map suspends the run; coming
  back to the same instance resumes it, carrying loot, duration and resource
  baselines over.
- **Kill counter** — per-rarity (normal/magic/rare/unique) kills near you,
  reset per area.
- **Hiveblood** — Genesis-tree resource total with per-map gains and a
  near-cap warning flash (read via the host, no raw offsets in the plugin).
- **Atziri Beacons** — the temple-entry counter (N / 60) with a chime on each
  gain. The counter element is found by scanning the UI tree for its text
  pattern, so game patches that shuffle panel indices don't break it.
- **Zone display names** — rename raw zone names to friendly labels.

## Requirements

- POEFixer (POE2Fixer) with Plugin SDK **v6**
- The host's price service enabled (league selected in POEFixer Settings)
  for loot pricing — tracking itself works without it

## Build

Open `FarmCounter.sln` in Visual Studio 2022 (v143 toolset) and build
**Release | x64**, or:

```
MSBuild FarmCounter.vcxproj /p:Configuration=Release /p:Platform=x64
```

The DLL is written to `bin\Release\FarmCounter.dll`.

## Install

Copy `FarmCounter.dll` into `Plugins\FarmCounter\` next to your POEFixer
executable and enable the plugin in the Plugins tab. Settings and history
are stored in `Plugins\FarmCounter\config\`.

## Repository Layout

```
FarmCounter.cpp     — plugin shell (lifecycle, chime, module wiring)
src/                — tracker, loot scanner/diff, prices, resources, persistence
src/ui/             — overlay + settings tabs (ImGui)
sdk/                — POEFixer Plugin SDK v6 headers (synced from the main repo)
imgui/              — Dear ImGui sources the DLL compiles against
```

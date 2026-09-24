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
- **Map-run history & sessions in SQLite** — per-run date, duration, value,
  loot (with icons), kills by rarity, Hiveblood/beacon gains; archive everything
  into numbered sessions. Stored in `data/farmstats.db` — survives restarts.
- **Statistics** — pick a map type on the left to compare all recorded runs of
  that type (Simulacrum arenas are grouped but keep their arena names). Filter
  by session and time, sort the run table by profit, duration, rate and more,
  and show an optional recent-run chart. Select a run for its complete loot,
  resources and map modifiers; loot totals combine the selected runs at their
  stored item values, including spending. Averages, best and worst exclude the
  unfinished current run; totals include it. Deleting history asks for an
  inline confirmation, and the active or suspended run cannot be deleted.
- **New session** — archives the current records and starts a fresh counting
  segment on the current map without leaving it; existing inventory is never
  counted again.
- **XP** — XP/hour and current-map XP in the overlay, each measured on its own
  (XP/hour has its own reset).
- **True pause** — while the game is paused in the Esc menu, the map timer and
  session active-time freeze too.
- **Hideout round-trips handled** — leaving a map suspends the run; coming
  back to the same instance resumes it, carrying loot, duration and resource
  baselines over.
- **Kill counter** — per-rarity (normal/magic/rare/unique) kills near you,
  drawn with the same monster icons the Radar uses; per-run and per-session
  totals accumulate in Statistics. Rogue Exiles are counted separately with
  their own icon.
- **Gold** — the character's gold total in the overlay with a "(+N)"
  picked-up-this-map counter; per-run/per-session gains persist in the
  statistics DB (vendor spending between visits doesn't distort them).
- **Map modifiers** — each run records the area's modifiers (the same lines
  the in-game area-info panel shows), collapsible under the run in Statistics.
  Sourced from the host `ctx->Game.GetAreaMods()` SDK call.
- **Hiveblood** — Genesis-tree resource total with per-map gains and a
  near-cap warning flash (read via the host, no raw offsets in the plugin).
- **Atziri Beacons** — the temple-entry counter (N / 60) with a chime on each
  gain. The counter element is found by scanning the UI tree for its text
  pattern, so game patches that shuffle panel indices don't break it.
- **Zones** — current game area names are built in; search an area, click
  Edit, then Save or press Enter to give it a custom display name (Reset
  restores the game name). Custom names never merge unrelated maps. Existing
  `zone_names.json` aliases are kept.

## Requirements

- POEFixer (POE2Fixer) with Plugin SDK **v6**. Use a current POEFixer build: it
  supplies a coherent inventory snapshot (readiness, scan stamp, area counter)
  that tells an empty backpack from unread or stale data. Older SDK v6 hosts
  still load the plugin, but their fallback cannot validate an empty
  inventory reading.
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
executable and enable the plugin in the Plugins tab. Settings live in
`Plugins\FarmCounter\config\` (JSON); statistics in
`Plugins\FarmCounter\data\farmstats.db` (SQLite). Keep both when you replace
the DLL. Database write errors are shown in Statistics; a failed reset or
delete keeps the previous state.

## Repository Layout

```
FarmCounter.cpp     — plugin shell (lifecycle, pause, chime, module wiring)
src/                — tracker, loot scanner/diff, prices, resources, XP, zones catalog,
                      statistics model, SQLite store (+ two small vendored host helpers:
                      RetryGate.h, FileIO.h)
src/ui/             — overlay, settings, statistics and zones views (ImGui)
lib/                — vendored sqlite3 amalgamation + nlohmann/json
sdk/                — POEFixer Plugin SDK v6 headers (synced from the main repo)
imgui/              — Dear ImGui sources the DLL compiles against
```

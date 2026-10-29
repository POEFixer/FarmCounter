# Changelog

## 1.0.3

- Follows the program language (ten languages).
- The notes in the Settings tab wrap at the tab's edge instead of running past it, and the "Visible item rows" slider leaves room for its label in a narrow tab.
- Loot is priced in every game language: items are priced by the item itself, not by the name the game shows (POEFixer 371 and later; an older POEFixer prices by name as before). A custom price set under an item's English name also applies.
- Each backpack item is priced once per area and price update instead of on every backpack read, so a full backpack costs no frame time.
- The Custom Prices tab lists as unpriced only the items the counter could not price: an item priced in a non-English game is no longer listed there (a custom price set on its game name would have replaced its market price).
- The kill counter's monster icons appear on a fresh install as soon as POEFixer has downloaded them, and follow later icon updates without a restart.
- The trend chart's date range no longer cuts a translated "unknown date" text in the middle of a character.

## 1.0.2

- Works with the separate menu window (POEFixer 366): the host hands the plugin the right drawing context for its settings tab and for its overlay.
- The settings tab also sets its drawing context itself, so it draws in the right window with the menu in a separate window.
- Its windows (the counter and the separate Atziri beacons bar) now have the POEFixer gear menu (POEFixer 366): lock them, take them out of the game onto any screen, reset their position or close them; their places are remembered separately in and out of the game.
- New setting "Show the counter window" (Settings tab); the counter's Close turns it off. Closing the separate beacons bar turns "Show as separate window" off, so the beacons go back into the counter window. Out of the game the counter stays movable and does not hide on mouse hover.

## 1.0.1

- Rebuilt with the updated plugin SDK headers (POEFixer 365 adds optional calls for UI control names, stash objects and tabs, wider entity scans and the host automation flag). No change in what the plugin does.

## 1.0.0

- First versioned release.
- The version now shows in the plugin header of the Plugins tab instead of inside the settings.

#pragma once
// Theme.h — FarmCounter overlay palette + FontAwesome-6 glyph aliases.
// Header-only UI module consumed by the overlay/settings tasks.
//
// The plugin renders into the HOST's ImGui context, whose default font already
// has the FontAwesome-6 glyphs merged in. The ICON_FA_* byte sequences below are
// copied VERBATIM from POEFixer/imgui/IconsFontAwesome6.h — do NOT invent or edit
// codepoints (wrong bytes render as tofu boxes). The // U+xxxx comments preserve
// the source codepoint for traceability.
#include <imgui.h>

namespace FcTheme {

// ── Palette ───────────────────────────────────────────────────────────────────
inline const ImVec4 kGain   = ImVec4(0.40f, 1.00f, 0.40f, 1.0f);  // profit / positive delta
inline const ImVec4 kLoss   = ImVec4(1.00f, 0.40f, 0.40f, 1.0f);  // loss / negative delta
inline const ImVec4 kAccent = ImVec4(1.00f, 0.85f, 0.30f, 1.0f);  // highlight / gold accent
inline const ImVec4 kDim    = ImVec4(0.70f, 0.70f, 0.70f, 1.0f);  // secondary / muted text
inline const ImVec4 kHive   = ImVec4(0.78f, 0.45f, 0.95f, 1.0f);  // Hiveblood accent
inline const ImVec4 kIncur  = ImVec4(0.90f, 0.60f, 0.10f, 1.0f);  // Incursion accent
inline const ImVec4 kGold   = ImVec4(0.98f, 0.80f, 0.25f, 1.0f);  // Gold counter accent
inline const ImVec4 kXp     = ImVec4(0.40f, 0.72f, 1.00f, 1.0f);  // experience rate
inline const ImVec4 kMod    = ImVec4(0.53f, 0.68f, 1.00f, 1.0f);  // map-modifier line (in-game blue)

} // namespace FcTheme

// ── FontAwesome-6 glyph macros — copied verbatim from the host header ─────────
// (POEFixer/imgui/IconsFontAwesome6.h). #ifndef-guarded so they coexist safely
// with the real host header should both ever land in one translation unit.
#ifndef ICON_FA_HOURGLASS
#define ICON_FA_HOURGLASS      "\xef\x89\x94"  // U+f254
#endif
#ifndef ICON_FA_ARROW_TREND_UP
#define ICON_FA_ARROW_TREND_UP "\xee\x82\x98"  // U+e098
#endif
#ifndef ICON_FA_SKULL
#define ICON_FA_SKULL          "\xef\x95\x8c"  // U+f54c
#endif
#ifndef ICON_FA_KHANDA
#define ICON_FA_KHANDA         "\xef\x99\xad"  // U+f66d
#endif
#ifndef ICON_FA_MAP
#define ICON_FA_MAP            "\xef\x89\xb9"  // U+f279
#endif
#ifndef ICON_FA_GEM
#define ICON_FA_GEM            "\xef\x8e\xa5"  // U+f3a5
#endif
#ifndef ICON_FA_TROPHY
#define ICON_FA_TROPHY         "\xef\x82\x91"  // U+f091
#endif
#ifndef ICON_FA_COINS
#define ICON_FA_COINS          "\xef\x94\x9e"  // U+f51e
#endif
#ifndef ICON_FA_REPEAT
#define ICON_FA_REPEAT         "\xef\x8d\xa3"  // U+f363
#endif
#ifndef ICON_FA_CIRCLE_CHECK
#define ICON_FA_CIRCLE_CHECK   "\xef\x81\x98"  // U+f058
#endif
#ifndef ICON_FA_PAUSE
#define ICON_FA_PAUSE          "\xef\x81\x8c"  // U+f04c
#endif
#ifndef ICON_FA_PLAY
#define ICON_FA_PLAY           "\xef\x81\x8b"  // U+f04b
#endif

// ── Semantic glyph aliases for the FarmCounter overlay/settings ───────────────
namespace FcGlyph {
    inline constexpr const char* Session = ICON_FA_HOURGLASS;       // session timer
    inline constexpr const char* Profit  = ICON_FA_ARROW_TREND_UP;  // profit / divines per hour
    inline constexpr const char* Kills   = ICON_FA_SKULL;           // kill count
    inline constexpr const char* Sword   = ICON_FA_KHANDA;          // kills (sword motif, alt)
    inline constexpr const char* Map     = ICON_FA_MAP;             // maps run / area
    inline constexpr const char* Item    = ICON_FA_GEM;             // item / drop / currency
    inline constexpr const char* Trophy  = ICON_FA_TROPHY;          // best run / record
    inline constexpr const char* Gold    = ICON_FA_COINS;           // character gold counter
    inline constexpr const char* Visits  = ICON_FA_REPEAT;          // lifetime runs of this map category
    inline constexpr const char* Completed = ICON_FA_CIRCLE_CHECK;  // completed/archived run
    inline constexpr const char* Paused = ICON_FA_PAUSE;            // paused run
    inline constexpr const char* Live = ICON_FA_PLAY;               // live run
} // namespace FcGlyph

// Overlay.cpp — FarmCounter in-game HUD overlay (approved hybrid layout).
//
// Behavior-preserving port of the old monolith's DrawUI() overlay window
// (FarmCounter.cpp:840-1048) re-skinned into the hybrid layout:
//   accent header bar  -> compact stat strip -> inline resource bars -> loot list
//
// Preserved verbatim from the old overlay:
//   - hover-through rect (window vanishes while hovered unless settings open)
//   - SetNextWindowBgAlpha / size constraints / NoTitleBar|NoScrollbar|AlwaysAutoResize
//     (+ NoMove|NoMouseInputs unless settings open)
//   - window-position write-back when settings open
//   - hideout-collapse path (stats + bars only, no header/loot)
//   - separate-window Incursion overlay
//   - the loot-row right-alignment math (old :1006-1019) and ChaosToDisplay (:1059-1063)
//
// New in the hybrid layout: the gold accent header bar, the FcGlyph-iconed stat
// strip, per-item icons in the loot list, and inline (label + bar) resource rows.
//
// Forward contracts honored (Task 11 review):
//   - elapsed time + Profit/h come from FarmTracker::SessionActiveSec() (never
//     SessionStart(), which resets on hideout-resume);
//   - ZoneNames::Display() results are copied into std::string immediately (the
//     `disp` helper returns by value), never bound to a longer-lived reference.
#include "Overlay.h"
#include "Theme.h"
#include "Formatting.h"
#include "../LootRows.h"

#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// Mirrors ResourceReaders.cpp's kHivebloodCap (not exported in a shared header).
constexpr int32_t kHivebloodCap = 100000;

// Port of old ChaosToDisplay (FarmCounter.cpp:1055-1059): 0=exalted, 1=divine, 2=chaos.
inline float ChaosToDisplay(int overlayCurrency, float chaos, float exRate, float divRate) {
    if (overlayCurrency == 1) return chaos / divRate;  // divine
    if (overlayCurrency == 2) return chaos;            // chaos
    return chaos / exRate;                             // exalted (default)
}

// Port of old HbFormatThousands (FarmCounter.cpp:45-49).
std::string FormatThousands(int32_t n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

// Width of an icon scaled to a single text line, or 0 if the texture is invalid.
inline float IconWidthForLine(const IconTex& tex, float lineH) {
    return (tex.valid && tex.h > 0) ? lineH * (float)tex.w / (float)tex.h : 0.f;
}

// Font-independent ▲ / ▼ used by the header profit (avoids missing-glyph tofu).
void DrawTriangle(ImDrawList* dl, ImVec2 c, float r, bool up, ImU32 col) {
    if (up) dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y + r), col);
    else    dl->AddTriangleFilled(ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y - r), col);
}

float ExperienceRateColumnWidth() {
    // Count() emits at most three digits for the rate's compact K/M/B/T form.
    // XpTracker requires a full second, so a uint32 XP delta cannot need the
    // scientific fallback. Reserve the widest glyphs, including a death loss,
    // independently of the current value or number of displayed decimals.
    float digits = 0.f;
    for (char digit = '0'; digit <= '9'; ++digit) {
        const char text[] = {digit, digit, digit, '\0'};
        digits = (std::max)(digits, ImGui::CalcTextSize(text).x);
    }
    float unit = 0.f;
    for (const char* text : {"K", "M", "B", "T"})
        unit = (std::max)(unit, ImGui::CalcTextSize(text).x);
    return std::ceil(digits + unit + ImGui::CalcTextSize("-. XP/h").x) + 1.f;
}

void DrawExperienceLine(const XpTracker& xp, const MapXpTracker& mapXp) {
    ImGui::TextColored(FcTheme::kGold, "L%s", xp.HasSample() ? std::to_string(xp.Level()).c_str() : "--");
    ImGui::SameLine(0.f, 8.f);
    const ImVec2 ratePosition = ImGui::GetCursorScreenPos();
    ImGui::TextColored(xp.XpPerHour() < 0.0 ? FcTheme::kLoss : FcTheme::kXp, "%s XP/h",
                       xp.HasSample() ? FcFormat::Count(xp.XpPerHour()).c_str() : "--");
    ImGui::SameLine(0.f, 8.f);
    ImGui::SetCursorScreenPos(ImVec2(ratePosition.x + ExperienceRateColumnWidth() + 8.f, ratePosition.y));
    ImGui::TextColored(mapXp.GainedXp() < 0 ? FcTheme::kLoss : FcTheme::kDim, "Map %s XP",
                       mapXp.HasSample() ? FcFormat::Count(static_cast<double>(mapXp.GainedXp()), true).c_str() : "--");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Current-map net XP includes death losses.\nRate measurement: %s XP in %s%s",
                          FcFormat::Count(static_cast<double>(xp.GainedXp()), true).c_str(),
                          FcFormat::Duration(xp.ActiveSeconds()).c_str(), xp.IsRunning() ? "" : " (paused)");
}

// Small colored square ("rarity dot") drawn inline at the cursor, advancing past
// it via a Dummy (port of old KcDrawRarityDot, FarmCounter.cpp:1214-1222).
void KcDrawRarityDot(ImVec4 col) {
    float sz  = ImGui::GetTextLineHeight() * 0.75f;
    float pad = (ImGui::GetTextLineHeight() - sz) * 0.5f;
    ImVec2 p  = ImGui::GetCursorScreenPos();
    p.y += pad;
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz),
        ImGui::ColorConvertFloat4ToU32(col), 2.f);
    ImGui::Dummy(ImVec2(sz + 3.f, ImGui::GetTextLineHeight()));
}

// One rarity marker: the radar-atlas monster icon (same sprites the Radar draws
// for mobs) when the atlas is loaded, else the legacy colored dot.
void KcDrawRarityMark(const IconTextures* icons, int rarity, ImVec4 dotCol) {
    if (icons) {
        const AtlasIcon& ic = icons->Monster(rarity);
        if (ic.valid) {
            const float lh = ImGui::GetTextLineHeight();
            ImGui::Image(ic.tex, ImVec2(lh, lh), ic.uv0, ic.uv1);
            return;
        }
    }
    KcDrawRarityDot(dotCol);
}

// Per-rarity kill counts rendered inline. The caller has already drawn the
// Kills glyph + SameLine, so this emits only the value: per-rarity radar
// monster icon (colored-dot fallback) + count for each enabled rarity followed
// by "(total)", or the bare total when all rarities are off. The kcShow master
// gate is handled by the caller.
void DrawKillCountInline(const IconTextures* icons, const KillCounter& kc, const OverlaySettings& s) {
    const int  total = kc.Total();
    const bool any   = s.kcShowNormal || s.kcShowMagic || s.kcShowRare || s.kcShowUnique;
    if (!any) { ImGui::Text("%d", total); return; }
    if (s.kcShowNormal) {                                   // Normal — white
        KcDrawRarityMark(icons, 0, ImVec4(0.85f, 0.85f, 0.85f, 1.f));
        ImGui::SameLine(0.f, 2.f); ImGui::Text("%d", kc.Normal()); ImGui::SameLine(0.f, 8.f);
    }
    if (s.kcShowMagic) {                                    // Magic — blue
        KcDrawRarityMark(icons, 1, ImVec4(0.33f, 0.53f, 1.f, 1.f));
        ImGui::SameLine(0.f, 2.f); ImGui::Text("%d", kc.Magic()); ImGui::SameLine(0.f, 8.f);
    }
    if (s.kcShowRare) {                                     // Rare — yellow
        KcDrawRarityMark(icons, 2, ImVec4(1.f, 0.87f, 0.2f, 1.f));
        ImGui::SameLine(0.f, 2.f); ImGui::Text("%d", kc.Rare()); ImGui::SameLine(0.f, 8.f);
    }
    if (s.kcShowUnique) {                                   // Unique — orange
        KcDrawRarityMark(icons, 3, ImVec4(1.f, 0.5f, 0.1f, 1.f));
        ImGui::SameLine(0.f, 2.f); ImGui::Text("%d", kc.Unique()); ImGui::SameLine(0.f, 8.f);
    }
    if (s.kcShowRogue && kc.Rogue() > 0) {                  // Rogue Exiles — own icon
        if (icons) {
            const AtlasIcon& ri = icons->RogueExile();
            if (ri.valid) {
                const float lh = ImGui::GetTextLineHeight();
                ImGui::Image(ri.tex, ImVec2(lh, lh), ri.uv0, ri.uv1);
            } else {
                KcDrawRarityDot(ImVec4(0.85f, 0.35f, 0.15f, 1.f));
            }
        }
        ImGui::SameLine(0.f, 2.f); ImGui::Text("%d", kc.Rogue()); ImGui::SameLine(0.f, 8.f);
    }
    ImGui::Text("(%d)", total);
}

// Draws one loot name cell: optional item icon + signed/coloured name (+ "xN").
void DrawLootNameCell(const OverlayDeps& d, const LootEntry& e, ImVec4 col, float lineH) {
    bool lost   = e.stackCount < 0;
    int  absStk = std::abs(e.stackCount);
    IconTex itex = d.icons->Item(e.iconPath);  // cached at scan time; render performs zero price lookups
    if (itex.valid && itex.h > 0) {
        ImGui::Image(itex.srv, ImVec2(lineH * (float)itex.w / (float)itex.h, lineH));
        ImGui::SameLine(0.f, 4.f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    if (absStk > 1) ImGui::Text("%s %s  x%d", lost ? "-" : "+", e.name.c_str(), absStk);
    else            ImGui::Text("%s %s",      lost ? "-" : "+", e.name.c_str());
    ImGui::PopStyleColor();
}

// Separate-window Incursion bar (old DrawIncursionBar(separateMode=true), :1474-1520),
// fed from ResourceReaders instead of a direct memory read.
void DrawSeparateIncursionBar(const ResourceReaders::ItState& it) {
    const float barH = 6.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (it.ok) {
        float frac = it.max > 0 ? (float)it.cur / (float)it.max : 0.f;
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, FcTheme::kIncur);
        ImGui::ProgressBar(frac, ImVec2(-1.f, barH), "");
        ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
        if (it.max > 0) {
            float w = rmax.x - rmin.x;
            for (int i = 1; i < it.max; ++i) {
                float x = rmin.x + w * ((float)i / (float)it.max);
                dl->AddLine(ImVec2(x, rmin.y), ImVec2(x, rmax.y), IM_COL32(0, 0, 0, 100), 1.f);
            }
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.3f, 0.3f, 0.3f, 1.f));
        ImGui::ProgressBar(0.f, ImVec2(-1.f, barH), "");
        ImGui::PopStyleColor();
    }
}

// Separate floating Incursion overlay window (old DrawIncursionTokensOverlay, :1522-1546).
void DrawSeparateIncursionOverlay(const OverlayDeps& d) {
    const OverlaySettings& s = *d.settings;
    if (!s.itShow || !s.itSeparate) return;

    if (s.itOverlayX >= 0.f && s.itOverlayY >= 0.f)
        ImGui::SetNextWindowPos(ImVec2(s.itOverlayX, s.itOverlayY), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(230, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::Begin("##IncTokensFC", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoBackground);

    DrawSeparateIncursionBar(d.resources->Incursion());

    ImVec2 np = ImGui::GetWindowPos();
    if (np.x != s.itOverlayX || np.y != s.itOverlayY) {
        d.settings->itOverlayX = np.x;  // written back; shell persists to disk
        d.settings->itOverlayY = np.y;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace

void RenderOverlay(const OverlayDeps& d) {
    if (!d.tracker || !d.prices || !d.icons || !d.kills || !d.resources || !d.settings)
        return;

    const OverlaySettings& s = *d.settings;
    FarmTracker& tr = *d.tracker;

    // Settings-open handshake: capture, then reset the shell's persistent flag
    // (mirrors old "bool settingsOpen = m_SettingsOpen; m_SettingsOpen = false;").
    const bool settingsOpen = d.settingsOpenThisFrame;
    if (d.outSettingsConsumed) *d.outSettingsConsumed = false;

    // Currency rates + display conversion (clamped >= 1 by PriceProvider).
    const int   curIdx = s.overlayCurrency;
    const float exRate = d.prices->ExaltedInChaos();
    const float divRate = d.prices->DivineInChaos();
    auto toDisp = [&](float chaos) { return ChaosToDisplay(curIdx, chaos, exRate, divRate); };
    // Returns by VALUE so the ZoneNames::Display() reference is consumed immediately.
    auto disp = [&](const std::string& raw) -> std::string {
        return d.zones ? d.zones->Display(raw) : raw;
    };

    // Current-map profit (sum of the live loot diff), old :821-833.
    float totalChaos = 0.f;
    for (const auto& e : tr.Loot()) totalChaos += e.chaosEach * (float)e.stackCount;
    const float totalDisplay = toDisp(totalChaos);

    // Separate Incursion overlay is drawn first (old order, :844).
    DrawSeparateIncursionOverlay(d);

    // Hover-through: while the cursor is over last frame's window rect (and
    // settings are closed) skip the main window entirely so clicks pass to the
    // game. Single overlay instance => a function-local static rect is fine.
    static ImVec4 s_overlayRect = ImVec4(0, 0, 0, 0);
    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool mouseOver = (s_overlayRect.z > s_overlayRect.x) &&
                     mouse.x >= s_overlayRect.x && mouse.x <= s_overlayRect.z &&
                     mouse.y >= s_overlayRect.y && mouse.y <= s_overlayRect.w;
    if (mouseOver && !settingsOpen) return;

    if (s.windowPosX >= 0.f)
        ImGui::SetNextWindowPos(ImVec2(s.windowPosX, s.windowPosY), ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(s.windowAlpha);
    ImGui::SetNextWindowSizeConstraints(ImVec2(320, 40), ImVec2(600, 2000));

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_AlwaysAutoResize;
    if (!settingsOpen) wf |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoMouseInputs;

    if (!ImGui::Begin("##FarmCounterWnd", nullptr, wf)) { ImGui::End(); return; }

    // Save window rect for next frame's hover test.
    {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        s_overlayRect = ImVec4(wp.x, wp.y, wp.x + ws.x, wp.y + ws.y);
    }
    // Window-pos write-back while settings are open (old :870-876).
    if (settingsOpen) {
        ImVec2 cp = ImGui::GetWindowPos();
        if (cp.x != s.windowPosX || cp.y != s.windowPosY) {
            d.settings->windowPosX = cp.x;
            d.settings->windowPosY = cp.y;
        }
    }

    const bool  inHideout = tr.CurrentZone().find("Hideout") != std::string::npos;
    const bool  inMap     = tr.InMap();
    const bool  baseReady = tr.BaselineReady();
    const float lineH     = ImGui::GetTextLineHeight();
    ImDrawList* dl        = ImGui::GetWindowDrawList();
    const ImVec4 white(1, 1, 1, 1);

    // ── Accent header bar (map · timer · profit) — skipped in hideout ──────────
    if (!inHideout) {
        bool inSub = !tr.MapZoneName().empty() && tr.CurrentZone() != tr.MapZoneName();
        std::string zoneDisp =
            inSub ? (disp(tr.MapZoneName()) + " - " + disp(tr.CurrentZone()))
                  : (tr.MapZoneName().empty() ? disp(tr.CurrentZone()) : disp(tr.MapZoneName()));
        if (zoneDisp.empty()) zoneDisp = "Unknown Zone";

        const int mapSec = tr.CurrentMapSec();   // pause-honest (Esc menu excluded)

        const float padX = 6.f, padY = 3.f;
        ImVec2 winPos  = ImGui::GetWindowPos();
        float  winW    = ImGui::GetWindowSize().x;
        ImVec2 p0      = ImVec2(winPos.x, winPos.y);
        float  rectH   = lineH + padY * 2.f;
        // Filled accent rect — full window width, flush to top edge, no rounding on top corners.
        ImU32 fill = ImGui::ColorConvertFloat4ToU32(
            ImVec4(FcTheme::kAccent.x, FcTheme::kAccent.y, FcTheme::kAccent.z, 0.18f));
        float winRounding = ImGui::GetStyle().WindowRounding + 3.f;
        dl->AddRectFilled(p0, ImVec2(p0.x + winW, p0.y + rectH), fill, winRounding,
                          ImDrawFlags_RoundCornersTop);

        ImGui::SetCursorScreenPos(ImVec2(p0.x + padX, p0.y + padY));
        ImGui::TextColored(FcTheme::kAccent, "%s", FcGlyph::Map);
        ImGui::SameLine(0.f, 5.f);
        ImGui::TextColored(white, "%s", zoneDisp.c_str());

        if (inMap) {
            ImGui::SameLine(0.f, 6.f);
            ImGui::TextColored(FcTheme::kDim, "(%s)", FcFormat::Duration(mapSec, true).c_str());
            if (!baseReady) {
                ImGui::SameLine(0.f, 8.f);
                ImGui::TextColored(FcTheme::kAccent, "scanning...");
            } else {
                ImGui::SameLine(0.f, 8.f);
                bool gain = totalChaos >= 0.f;
                ImVec4 pc = gain ? FcTheme::kGain : FcTheme::kLoss;
                ImVec2 tcur = ImGui::GetCursorScreenPos();
                float triR = lineH * 0.30f;
                DrawTriangle(dl, ImVec2(tcur.x + triR, tcur.y + lineH * 0.5f), triR, gain,
                             ImGui::ColorConvertFloat4ToU32(pc));
                ImGui::Dummy(ImVec2(triR * 2.f + 2.f, lineH));
                ImGui::SameLine(0.f, 3.f);
                char tb[32]; snprintf(tb, sizeof(tb), "%s%.2f", gain ? "+" : "", totalDisplay);
                ImGui::TextColored(pc, "%s", tb);
                const IconTex& ci = d.icons->Currency(curIdx);
                float iw = IconWidthForLine(ci, lineH);
                if (iw > 0.f) { ImGui::SameLine(0.f, 3.f); ImGui::Image(ci.srv, ImVec2(iw, lineH)); }
            }
        }
        // Advance below the accent rect into the content region.
        ImVec2 contentStart = ImGui::GetWindowPos();
        float  wp           = ImGui::GetStyle().WindowPadding.x;
        ImGui::SetCursorScreenPos(ImVec2(contentStart.x + wp, p0.y + rectH + 6.f));
    }

    // ── Compact stat strip (session · profit/h · kills · maps) ─────────────────
    {
        const int sess = tr.SessionActiveSec();   // contract #1: never SessionStart()
        int sessionMaps = 0;
        float sessionChaos = 0.f;
        for (const auto& r : tr.Runs())
            if (!r.archived) { sessionMaps++; sessionChaos += r.totalChaos; }
        const float pph = sess > 0 ? toDisp(sessionChaos) / (sess / 3600.f) : 0.f;

        ImGui::TextColored(FcTheme::kDim, "%s", FcGlyph::Session);
        ImGui::SameLine(0.f, 4.f);
        ImGui::TextUnformatted(FcFormat::Duration(sess).c_str());

        if (s.showProfitPerHour) {
            ImGui::SameLine(0.f, 8.f); ImGui::TextDisabled("|"); ImGui::SameLine(0.f, 8.f);
            bool gain = pph >= 0.f;
            ImVec4 pc = gain ? FcTheme::kGain : FcTheme::kLoss;
            ImGui::TextColored(pc, "%s", FcGlyph::Profit);
            ImGui::SameLine(0.f, 4.f);
            char pb[32]; snprintf(pb, sizeof(pb), "%.1f", pph);
            ImGui::TextColored(pc, "%s", pb);
            const IconTex& ci = d.icons->Currency(curIdx);
            float iw = IconWidthForLine(ci, lineH);
            if (iw > 0.f) { ImGui::SameLine(0.f, 2.f); ImGui::Image(ci.srv, ImVec2(iw, lineH)); }
            ImGui::SameLine(0.f, 2.f); ImGui::TextColored(pc, "/h");
        }

        ImGui::SameLine(0.f, 8.f); ImGui::TextDisabled("|"); ImGui::SameLine(0.f, 8.f);
        ImGui::TextColored(FcTheme::kDim, "%s", FcGlyph::Map);
        ImGui::SameLine(0.f, 4.f);
        ImGui::Text("%d", sessionMaps);
        if (inMap && d.currentMapCategory && !d.currentMapCategory->empty()) {
            ImGui::SameLine(0.f, 8.f); ImGui::TextDisabled("|"); ImGui::SameLine(0.f, 8.f);
            ImGui::BeginGroup();
            ImGui::TextColored(FcTheme::kDim, "%s", FcGlyph::Visits);
            ImGui::SameLine(0.f, 4.f);
            ImGui::Text("%zu", d.currentMapLifetimeRuns);
            ImGui::EndGroup();
            if (ImGui::IsItemHovered()) {
                const auto& name = d.zones ? d.zones->CategoryDisplay(*d.currentMapCategory) : *d.currentMapCategory;
                ImGui::SetTooltip("%s: recorded runs across all saved sessions, including this run.", name.c_str());
            }
        }

        // Kills on a separate line.
        if (s.kcShow) {
            ImGui::Separator();
            ImGui::TextColored(FcTheme::kDim, "%s", FcGlyph::Kills);
            ImGui::SameLine(0.f, 4.f);
            DrawKillCountInline(d.icons, *d.kills, s);
        }
    }

    if (!tr.SessionRunning())
        ImGui::TextColored(FcTheme::kLoss, "Paused — resumes on next map");

    if (s.showXpPerHour) {
        ImGui::Spacing();
        DrawExperienceLine(tr.Experience(), tr.MapExperience());
    }

    // ── Inline resource rows (Gold / Hiveblood / Atziri beacons) ───────────────
    bool barsDrawn = false;
    {
        ResourceReaders::HbState hb = d.resources->Hiveblood();
        ResourceReaders::ItState it = d.resources->Incursion();
        ResourceReaders::GdState gd = d.resources->Gold();
        const bool showIt = s.itShow && !s.itSeparate;
        const bool showHb = s.hbShow;
        const bool itOk = showIt && it.ok;
        const bool hbOk = showHb && hb.ok;
        const bool gdOk = s.goldShow && gd.ok;

        if (itOk || hbOk || gdOk) {
            ImGui::Separator();
            barsDrawn = true;
            const float barH = lineH * 0.55f;

            if (gdOk) {
                // Text-only row (gold has no cap, so no progress bar): coin glyph,
                // account total, "(+N)" picked up this map. Dimmed on a stale read.
                const ImVec4 col = ImVec4(FcTheme::kGold.x, FcTheme::kGold.y,
                                          FcTheme::kGold.z, gd.cached ? 0.65f : 1.f);
                ImGui::TextColored(col, "%s", FcGlyph::Gold);
                ImGui::SameLine(0.f, 4.f);
                std::string lbl = FormatThousands(gd.total);
                const int gain = tr.CurrentGoldGain();
                if (inMap && gain > 0) lbl += " (+" + FormatThousands(gain) + ")";
                ImGui::TextColored(col, "%s", lbl.c_str());
            }

            if (itOk) {
                int gain = (inMap && tr.ItHasBaseline() && it.cur > tr.ItBaseline())
                    ? (it.cur - tr.ItBaseline()) : 0;
                ImGui::TextColored(FcTheme::kIncur, "%s", FcGlyph::Item);
                ImGui::SameLine(0.f, 4.f);
                std::string lbl = std::to_string(it.cur) + " / " + std::to_string(it.max);
                if (gain > 0) lbl += " (+" + std::to_string(gain) + ")";
                ImGui::TextColored(FcTheme::kIncur, "%s", lbl.c_str());
                ImGui::SameLine(0.f, 6.f);
                float frac = it.max > 0 ? (float)it.cur / (float)it.max : 0.f;
                float w = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (lineH - barH) * 0.5f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, FcTheme::kIncur);
                ImGui::ProgressBar(frac, ImVec2(w, barH), "");
                ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
                if (it.max > 0) {
                    float bw = rmax.x - rmin.x;
                    for (int i = 1; i < it.max; ++i) {
                        float x = rmin.x + bw * ((float)i / (float)it.max);
                        dl->AddLine(ImVec2(x, rmin.y), ImVec2(x, rmax.y), IM_COL32(0, 0, 0, 100), 1.f);
                    }
                }
                ImGui::PopStyleColor();
            }

            if (hbOk) {
                int gain = (inMap && tr.HbHasBaseline() && hb.total > tr.HbBaseline())
                    ? (hb.total - tr.HbBaseline()) : 0;
                // Near-cap flash (old :1345-1352): at/over the warn threshold, blink the
                // bar+label between full and dim alpha on a warm-red color; otherwise the
                // normal purple (Hiveblood accent) with the existing cached-read dim.
                const bool warn    = s.hbWarnNearCap && hb.total >= s.hbWarnThreshold;
                const bool flashOn = warn && std::fmod(ImGui::GetTime(), 1.0) < 0.5;
                ImVec4 col = warn
                    ? ImVec4(1.f, 0.45f, 0.30f, flashOn ? 1.f : 0.4f)
                    : ImVec4(FcTheme::kHive.x, FcTheme::kHive.y, FcTheme::kHive.z, hb.cached ? 0.65f : 1.f);
                ImGui::TextColored(col, "%s", FcGlyph::Item);
                ImGui::SameLine(0.f, 4.f);
                std::string lbl = FormatThousands(hb.total);
                if (inMap && gain > 0 && s.hbShowMapGains) lbl += " (+" + FormatThousands(gain) + ")";
                ImGui::TextColored(col, "%s", lbl.c_str());
                ImGui::SameLine(0.f, 6.f);
                float frac = (float)hb.total / (float)kHivebloodCap;
                float w = ImGui::GetContentRegionAvail().x;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (lineH - barH) * 0.5f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                ImGui::ProgressBar(frac, ImVec2(w, barH), "");
                ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
                float bw = rmax.x - rmin.x;
                for (int i = 1; i < 10; ++i) {
                    float x = rmin.x + bw * (i / 10.f);
                    dl->AddLine(ImVec2(x, rmin.y), ImVec2(x, rmax.y), IM_COL32(0, 0, 0, 100), 1.f);
                }
                ImGui::PopStyleColor();
            }
        }
    }

    // Hideout collapse: stats + bars only, no header/loot (old :918-923).
    if (inHideout) { ImGui::End(); return; }

    // ── Loot list (only in map, baseline ready, items enabled; old :963-1041) ──
    if (inMap && baseReady && s.showItems) {
        const auto visible = SelectLootRows(tr.Loot(), s.showUnpriced, s.visibleItemRows);

        ImGui::Separator();

        if (visible.rows.empty()) {
            ImGui::TextDisabled("No changes yet");
        } else {
            ImGuiTableFlags tfl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody;
            if (ImGui::BeginTable("##loot", 2, tfl)) {
                ImGui::TableSetupColumn("##name",  ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##price", ImGuiTableColumnFlags_WidthFixed, 80.f);

                for (const auto* e : visible.rows) {
                    bool lost   = e->stackCount < 0;
                    int  absStk = std::abs(e->stackCount);
                    ImVec4 col  = lost ? FcTheme::kLoss : FcTheme::kGain;
                    float val   = toDisp(e->chaosEach * absStk);
                    if (lost) val = -val;

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    DrawLootNameCell(d, *e, col, lineH);

                    // Price column: right-aligned value + currency icon (old :1002-1015).
                    ImGui::TableNextColumn();
                    if (e->chaosEach > 0.f && std::isfinite(e->chaosEach)) {
                        const IconTex& ci = d.icons->Currency(curIdx);
                        float iconW = IconWidthForLine(ci, lineH);
                        float gap   = iconW > 0.f ? 3.f : 0.f;
                        char pb[32]; snprintf(pb, sizeof(pb), "%.2f", val);
                        float pw    = ImGui::CalcTextSize(pb).x + gap + iconW;
                        float avail = ImGui::GetContentRegionAvail().x;
                        if (avail > pw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - pw);
                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                        ImGui::TextUnformatted(pb);
                        ImGui::PopStyleColor();
                        if (iconW > 0.f) { ImGui::SameLine(0.f, gap); ImGui::Image(ci.srv, ImVec2(iconW, lineH)); }
                    }
                }

                ImGui::EndTable();
            }
            if (visible.hidden > 0) ImGui::TextDisabled("+%zu more items", visible.hidden);
        }
    }

    ImGui::End();
}

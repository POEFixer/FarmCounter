// Settings.cpp — FarmCounter settings tab bar.
//
// Tabs: Settings · Custom Prices · Statistics · Zones · Hiveblood · Atziri ·
// Kills. Pure render: READS the model/services and WRITES through the
// (non-const) OverlaySettings / PriceProvider / ZoneNames it is handed,
// persisting each change immediately via the Persistence free functions /
// ZoneNames::Save(). Statistics mutations (delete run/session, new session)
// delegate to FarmTracker, which owns the SQLite store.
//
// Destructive actions use INLINE two-step confirm buttons, never popups/modals
// (modals freeze when the host runs in overlay mode).
//
// Texts follow the program language (Text.h): every widget whose label is its
// ImGui id takes I18n::TrId, so its id stays the key's in every language.
#include "Settings.h"
#include "StatisticsView.h"
#include "ZonesView.h"
#include "Theme.h"
#include "Formatting.h"
#include "Text.h"
#include "../LootRows.h"
#include "../../../../POEFixer/plugin_sdk/PluginUi.h"

#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using FcText::I18n::Key;
using FcText::I18n::Tr;
using FcText::I18n::TrF;
using FcText::I18n::TrId;
using FcText::Num;
using FcText::Show;

std::string FormatThousands(long long n) {
    std::string s = std::to_string(n);
    const int stop = (n < 0) ? 1 : 0;   // don't split a leading minus
    for (int i = (int)s.size() - 3; i > stop; i -= 3) s.insert(i, ",");
    return s;
}

// Builds an icon-prefixed tab label ("<glyph>  <text>###<key>"). The glyph bytes are
// pure ASCII escapes (see Theme.h); the ###key keeps the tab's id in every language.
std::string TabLabel(const char* glyph, const char* key) {
    return std::string(glyph) + "  " + TrId(key);
}

// Red SmallButton styling for destructive actions.
struct DangerButtonColors {
    DangerButtonColors() {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(.6f, .15f, .15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.8f, .25f, .25f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(.45f, .1f, .1f, 1.f));
    }
    ~DangerButtonColors() { ImGui::PopStyleColor(3); }
};

// Two short values on one line, one em apart (layout is code, never spacing in a text).
void Gap() { ImGui::SameLine(0.f, ImGui::GetFontSize()); }

// A dimmed note wrapped at the tab's edge (a translation runs longer than the English line).
void Note(const char* text) {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

// Width of a slider whose label sits on its right: `wantEm` wide, narrower when the
// translated label would run past the edge, never below six em.
float WidthBesideLabel(float wantEm, const char* label) {
    const float em = ImGui::GetFontSize();
    const float room = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(label).x -
                       ImGui::GetStyle().ItemInnerSpacing.x;
    return std::clamp(room, em * 6.f, em * wantEm);
}

// ── Tabs ──────────────────────────────────────────────────────────────────────

void DrawSettingsTab(const SettingsDeps& d) {
    OverlaySettings& s = *d.settings;
    auto save = [&] { SaveSettings(d.dir, s); };

    ImGui::SeparatorText(Tr("settings_section_overlay"));
    if (ImGui::Checkbox(TrId("settings_enable_overlay"), &s.wantsOverlay)) save();
    if (PluginUi::Check(Tr("settings_show_window"), "showWindow", &s.showWindow,
                        Tr("settings_show_window_tip")))
        save();
    if (ImGui::Checkbox(TrId("settings_show_item_list"), &s.showItems)) save();
    ImGui::SetNextItemWidth(WidthBesideLabel(12.f, Tr("settings_visible_item_rows")));
    if (ImGui::SliderInt(TrId("settings_visible_item_rows"), &s.visibleItemRows, 1, 50, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        s.visibleItemRows = std::clamp(s.visibleItemRows, 1, 50);
        save();
    }
    PluginUi::Tip(Tr("settings_visible_item_rows_tip"));
    if (ImGui::Checkbox(TrId("settings_show_unpriced"), &s.showUnpriced)) save();
    if (ImGui::Checkbox(TrId("settings_show_profit_per_hour"), &s.showProfitPerHour)) save();
    if (ImGui::Checkbox(TrId("settings_show_gold"), &s.goldShow)) save();

    ImGui::SeparatorText(Tr("settings_section_experience"));
    if (ImGui::Checkbox(TrId("settings_show_xp_per_hour"), &s.showXpPerHour)) save();
    PluginUi::Tip(Tr("settings_show_xp_per_hour_tip"));
    if (ImGui::SmallButton(TrId("settings_reset_xp"))) d.tracker->ResetExperience();
    PluginUi::Tip(Tr("settings_reset_xp_tip"));
    Note(Tr("settings_xp_note"));
    PluginUi::Tip(Tr("settings_xp_note_tip"));
    const auto& xp = d.tracker->Experience();
    if (xp.HasSample()) {
        // "Level 91  |  +1.23M XP  |  45K XP/h  |  1h 05m": the separators are layout (code).
        std::string line = TrF("settings_xp_level", { { "level", std::to_string(static_cast<unsigned>(xp.Level())) } });
        line += "  |  " + TrF("settings_xp_gained", { { "xp", FcFormat::Count(static_cast<double>(xp.GainedXp()), true) } });
        line += "  |  " + TrF("settings_xp_rate", { { "rate", FcFormat::Count(xp.XpPerHour()) } });
        line += "  |  " + FcFormat::Duration(xp.ActiveSeconds());
        if (!xp.IsRunning()) line += std::string(" ") + Tr("settings_xp_paused");
        Show(line);
    } else {
        Note(Tr("settings_xp_waiting"));
    }
    if (d.tracker->MapExperience().HasSample())
        Show(TrF("settings_xp_current_map",
                 { { "xp", FcFormat::Count(static_cast<double>(d.tracker->MapExperience().GainedXp()), true) } }));

    ImGui::SeparatorText(Tr("settings_section_display"));
    ImGui::TextUnformatted(Tr("settings_overlay_currency"));
    const char* currencyTexts[] = { Tr(FcText::kCurrencyKeys[0]), Tr(FcText::kCurrencyKeys[1]),
                                    Tr(FcText::kCurrencyKeys[2]) };
    ImGui::SetNextItemWidth(FcText::ComboWidthFor(ImGui::GetFontSize() * 8.f, currencyTexts, 3));
    if (FcText::KeyCombo("##OvCur", s.overlayCurrency, FcText::kCurrencyKeys, 3)) save();
    if (ImGui::SliderFloat(TrId("settings_opacity"), &s.windowAlpha, 0.1f, 1.0f, "%.1f")) save();

    // Read-only readout of the core price service (the core app owns the league
    // and refresh schedule — Configuration -> Settings in POEFixer).
    ImGui::SeparatorText(Tr("settings_section_prices"));
    {
        const PluginSDK::PriceStatus st = d.prices->Status();
        if (st.loaded) {
            Show(TrF("settings_price_db", { { "count", std::to_string(st.totalItems) } }));
            const float divInChaos = d.prices->DivineInChaos();
            const float exInChaos  = d.prices->ExaltedInChaos();
            if (divInChaos > 1.f && exInChaos > 1.f) {
                Show(TrF("settings_rate_divine", { { "chaos", Num("%.1f", divInChaos) } }));
                Gap();
                Show(TrF("settings_rate_exalted", { { "chaos", Num("%.1f", exInChaos) } }));
            }
            if (st.catsFailed > 0)
                ImGui::TextColored(ImVec4(1.f, .8f, .2f, 1.f), "%s",
                                   TrF("settings_price_categories_failed", { { "count", std::to_string(st.catsFailed) } }).c_str());
        } else if (st.catsPending > 0) {
            ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f), "%s", Tr("settings_prices_fetching"));
        } else {
            Note(Tr("settings_prices_none"));
        }
        Note(Tr("settings_league_note"));
    }
}

void DrawCustomPricesTab(const SettingsDeps& d) {
    PriceProvider& pp = *d.prices;
    const auto& snap = d.tracker->LastSnapshot();
    const float exRate = pp.ExaltedInChaos();   // clamped >= 1 by PriceProvider

    // Transient edit state (single settings window => function-local statics).
    static std::string s_editName, s_editBase;
    static char        s_editBuf[64] = "0.0";
    static bool        s_editMode    = false;

    // ── Unpriced items in inventory ────────────────────────────────────────────
    ImGui::SeparatorText(Tr("prices_section_unpriced"));
    // As the last backpack read priced the items (by the item itself, custom prices included):
    // never asked again by the shown name, which would list an item priced by what it is (and
    // invite a custom price on its localized name that overrides the market price).
    const std::vector<std::string> unpriced = UnpricedNames(snap);

    if (unpriced.empty()) {
        ImGui::TextDisabled("%s", Tr("prices_all_priced"));
    } else {
        float listH = ImGui::GetTextLineHeightWithSpacing()
            * (float)(unpriced.size() < 6 ? unpriced.size() + 1 : 6);
        if (ImGui::BeginTable("##UPT", 2,
                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, listH))) {
            ImGui::TableSetupColumn(TrId("col_item"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, 0.f);   // fits its button
            ImGui::TableHeadersRow();
            for (const auto& name : unpriced) {
                auto it = snap.find(name);
                std::string display = name;
                if (it != snap.end() && !it->second.uniqueName.empty()
                    && !it->second.baseTypeName.empty())
                    display = it->second.uniqueName + " - " + it->second.baseTypeName;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(display.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(name.c_str());
                if (ImGui::SmallButton(TrId("prices_set_price"))) {
                    s_editName = name;
                    // only set base when item is actually unique (has both names)
                    s_editBase = (it != snap.end() && !it->second.uniqueName.empty())
                                 ? it->second.baseTypeName : std::string();
                    snprintf(s_editBuf, sizeof(s_editBuf), "0.0");
                    s_editMode = true;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ── Set / edit a custom price (exalted value -> chaos via current rate) ─────
    if (s_editMode) {
        ImGui::Spacing();
        ImGui::SeparatorText(Tr("prices_section_set"));
        if (!s_editBase.empty()) Show(TrF("prices_item_with_base", { { "name", s_editName }, { "base", s_editBase } }));
        else                     Show(TrF("prices_item", { { "name", s_editName } }));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.f);
        ImGui::InputText(TrId("prices_ex_value"), s_editBuf, sizeof(s_editBuf), ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (ImGui::Button(TrId("prices_save"))) {
            try {
                float valEx = std::stof(s_editBuf);
                if (valEx > 0.f) {
                    const std::string key = PriceProvider::ToLower(s_editName);  // MANDATORY key
                    pp.Custom()[key]      = valEx;  // stored in exalts, converted at lookup
                    pp.CustomNames()[key] = s_editName;
                    pp.CustomBase()[key]  = s_editBase;
                    SaveCustomPrices(d.dir, pp);
                }
            } catch (...) {}
            s_editMode = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(TrId("prices_cancel"))) s_editMode = false;
    }

    // ── Existing custom prices ─────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText(Tr("prices_section_custom"));
    (void)exRate;
    if (pp.Custom().empty()) {
        ImGui::TextDisabled("%s", Tr("prices_none"));
        return;
    }

    int tableRows = (int)pp.Custom().size() + 1;
    if (tableRows > 10) tableRows = 10;
    float tableH = ImGui::GetTextLineHeightWithSpacing() * (float)tableRows;
    if (ImGui::BeginTable("##CPT", 3,
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
            ImVec2(0, tableH))) {
        // Fixed columns at width 0 fit their translated header and cells (no Resizable flag).
        ImGui::TableSetupColumn(TrId("col_item"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(TrId("prices_col_exalted"), ImGuiTableColumnFlags_WidthFixed, 0.f);
        ImGui::TableSetupColumn("##ops", ImGuiTableColumnFlags_WidthFixed, 0.f);
        ImGui::TableHeadersRow();

        std::string toDelete;
        for (auto& [key, exalts] : pp.Custom()) {
            auto itN = pp.CustomNames().find(key);
            auto itB = pp.CustomBase().find(key);
            const std::string uname = (itN != pp.CustomNames().end()) ? itN->second : key;
            const std::string bname = (itB != pp.CustomBase().end())  ? itB->second : std::string();
            const std::string displayName = !bname.empty() ? uname + " - " + bname : uname;

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(displayName.c_str());
            ImGui::TableNextColumn(); Show(TrF("prices_value_ex", { { "value", Num("%.3f", exalts) } }));
            ImGui::TableNextColumn();
            ImGui::PushID(key.c_str());
            if (ImGui::SmallButton(TrId("prices_edit"))) {
                s_editName = uname;
                s_editBase = bname;
                snprintf(s_editBuf, sizeof(s_editBuf), "%.4g", exalts);
                s_editMode = true;
            }
            ImGui::SameLine();
            {
                DangerButtonColors danger;
                if (ImGui::SmallButton(TrId("prices_delete"))) toDelete = key;
            }
            ImGui::PopID();
        }
        if (!toDelete.empty()) {
            pp.Custom().erase(toDelete);
            pp.CustomNames().erase(toDelete);
            pp.CustomBase().erase(toDelete);
            SaveCustomPrices(d.dir, pp);
        }
        ImGui::EndTable();
    }
}

void DrawStatisticsTab(const SettingsDeps& d) {
    if (d.statisticsView) d.statisticsView->Draw(d);
}

void DrawZonesTab(const SettingsDeps& d) {
    if (d.zonesView) d.zonesView->Draw(d);
}

void DrawHivebloodTab(const SettingsDeps& d) {
    OverlaySettings& s = *d.settings;
    auto save = [&] { SaveSettings(d.dir, s); };

    ImGui::SeparatorText(Tr("hive_section"));
    if (ImGui::Checkbox(TrId("hive_show"), &s.hbShow))      save();
    ImGui::Spacing();
    const std::string warnLabel = TrF("hive_warn_near_cap", { { "cap", FormatThousands(100000) } }) + "###hive_warn_near_cap";
    if (ImGui::Checkbox(warnLabel.c_str(), &s.hbWarnNearCap)) save();
    if (ImGui::SliderInt(TrId("hive_warn_threshold"), &s.hbWarnThreshold, 50000, 100000)) save();
    if (ImGui::Checkbox(TrId("hive_show_map_gains"), &s.hbShowMapGains)) save();

    ImGui::SeparatorText(Tr("live_section"));
    const ResourceReaders::HbState hb = d.resources->Hiveblood();
    if (hb.ok && !hb.cached)
        ImGui::TextColored(FcTheme::kHive, "%s", TrF("hive_current", { { "value", FormatThousands(hb.total) } }).c_str());
    else if (hb.ok && hb.cached)
        Show(TrF("hive_current_cached", { { "value", FormatThousands(hb.total) } }));
    else
        ImGui::TextDisabled("%s", Tr("hive_not_read"));
}

void DrawBeaconsTab(const SettingsDeps& d) {
    OverlaySettings& s = *d.settings;
    auto save = [&] { SaveSettings(d.dir, s); };

    ImGui::SeparatorText(Tr("atziri_section"));
    ImGui::TextDisabled("%s", Tr("atziri_note"));
    if (ImGui::Checkbox(TrId("atziri_show"), &s.itShow))         save();
    if (ImGui::Checkbox(TrId("atziri_sound"), &s.itSound))       save();
    if (s.itSound) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.f);
        if (ImGui::SliderFloat(TrId("atziri_volume"), &s.itVolume, 0.f, 1.f, "%.2f")) save();
        ImGui::SameLine();
        if (ImGui::Button(TrId("atziri_test")) && d.onTestSound) d.onTestSound(s.itVolume);
    }
    if (ImGui::Checkbox(TrId("atziri_separate"), &s.itSeparate)) save();

    ImGui::SeparatorText(Tr("live_section"));
    const ResourceReaders::ItState it = d.resources->Incursion();
    if (it.ok)
        ImGui::TextColored(FcTheme::kIncur, "%s", TrF("atziri_current",
            { { "current", std::to_string(it.cur) }, { "max", std::to_string(it.max) } }).c_str());
    else
        ImGui::TextDisabled("%s", Tr("atziri_not_visible"));
}

void DrawKillsTab(const SettingsDeps& d) {
    OverlaySettings& s = *d.settings;
    auto save = [&] { SaveSettings(d.dir, s); };

    ImGui::SeparatorText(Tr("kills_section"));
    if (ImGui::Checkbox(TrId("kills_show"), &s.kcShow)) save();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Tr("kills_per_rarity"));
    if (ImGui::Checkbox(TrId("kills_normal"), &s.kcShowNormal)) save(); ImGui::SameLine();
    if (ImGui::Checkbox(TrId("kills_magic"),  &s.kcShowMagic))  save(); ImGui::SameLine();
    if (ImGui::Checkbox(TrId("kills_rare"),   &s.kcShowRare))   save(); ImGui::SameLine();
    if (ImGui::Checkbox(TrId("kills_unique"), &s.kcShowUnique)) save(); ImGui::SameLine();
    if (ImGui::Checkbox(TrId("kills_rogue"),  &s.kcShowRogue))  save();

    ImGui::SeparatorText(Tr("kills_section_counts"));
    Show(TrF("kills_count_normal", { { "count", std::to_string(d.kills->Normal()) } }));  Gap();
    Show(TrF("kills_count_magic",  { { "count", std::to_string(d.kills->Magic()) } }));   Gap();
    Show(TrF("kills_count_rare",   { { "count", std::to_string(d.kills->Rare()) } }));    Gap();
    Show(TrF("kills_count_unique", { { "count", std::to_string(d.kills->Unique()) } }));  Gap();
    Show(TrF("kills_count_rogue",  { { "count", std::to_string(d.kills->Rogue()) } }));   Gap();
    Show(TrF("kills_count_total",  { { "count", std::to_string(d.kills->Total()) } }));
    if (ImGui::SmallButton(TrId("kills_reset"))) d.kills->Reset();
    ImGui::TextDisabled("%s", Tr("kills_stats_note"));
}

} // namespace

void RenderSettings(const SettingsDeps& d) {
    if (!d.tracker || !d.prices || !d.settings || !d.zones || !d.kills || !d.resources)
        return;

    if (!ImGui::BeginTabBar("##FCTabs")) return;
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Trophy, Key("tab_settings")).c_str()))      { DrawSettingsTab(d);     ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Item, Key("tab_custom_prices")).c_str())) { DrawCustomPricesTab(d); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Profit, Key("tab_statistics")).c_str()))    { DrawStatisticsTab(d);   ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Map, Key("tab_zones")).c_str()))         { DrawZonesTab(d);        ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Session, Key("tab_hiveblood")).c_str()))     { DrawHivebloodTab(d);    ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Sword, Key("tab_atziri")).c_str()))        { DrawBeaconsTab(d);      ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Kills, Key("tab_kills")).c_str()))         { DrawKillsTab(d);        ImGui::EndTabItem(); }
    ImGui::EndTabBar();
}

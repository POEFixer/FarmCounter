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
#include "Settings.h"
#include "StatisticsView.h"
#include "ZonesView.h"
#include "Theme.h"
#include "Formatting.h"
#include "../../version.h"

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

std::string FormatThousands(long long n) {
    std::string s = std::to_string(n);
    const int stop = (n < 0) ? 1 : 0;   // don't split a leading minus
    for (int i = (int)s.size() - 3; i > stop; i -= 3) s.insert(i, ",");
    return s;
}

// Builds an icon-prefixed tab label ("<glyph>  Name"). The glyph bytes are pure
// ASCII escapes (see Theme.h) so this stays a single-allocation per frame.
std::string TabLabel(const char* glyph, const char* name) {
    return std::string(glyph) + "  " + name;
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

// ── Tabs ──────────────────────────────────────────────────────────────────────

void DrawSettingsTab(const SettingsDeps& d) {
    OverlaySettings& s = *d.settings;
    auto save = [&] { SaveSettings(d.dir, s); };

    ImGui::Text("FarmCounter %s", ZONETIMER_VERSION);
    ImGui::Spacing();

    ImGui::SeparatorText("Overlay");
    if (ImGui::Checkbox("Enable Overlay Mode", &s.wantsOverlay))      save();
    if (ImGui::Checkbox("Show Item List",      &s.showItems))         save();
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderInt("Visible item rows", &s.visibleItemRows, 1, 50, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        s.visibleItemRows = std::clamp(s.visibleItemRows, 1, 50);
        save();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show the highest total stack values first. Full loot totals and history are retained.");
    if (ImGui::Checkbox("Show Unpriced Items", &s.showUnpriced))      save();
    if (ImGui::Checkbox("Show Profit/Hour",    &s.showProfitPerHour)) save();
    if (ImGui::Checkbox("Show Gold (total + map gain)", &s.goldShow)) save();

    ImGui::SeparatorText("Experience");
    if (ImGui::Checkbox("Show XP/Hour in overlay", &s.showXpPerHour)) save();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hiding this card keeps the XP measurement running.");
    if (ImGui::SmallButton("Reset XP measurement")) d.tracker->ResetExperience();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Resets only the XP/hour measurement. Current-map XP, loot and farm statistics are kept.");
    ImGui::TextDisabled("Net XP includes death losses. Uses active farming time.");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The measurement pauses with farming (including hideout and the Esc menu).\nSwitching character starts a new XP measurement.");
    const auto& xp = d.tracker->Experience();
    if (xp.HasSample()) {
        ImGui::Text("Level %u  |  %s XP  |  %s XP/h  |  %s%s",
                    static_cast<unsigned>(xp.Level()),
                    FcFormat::Count(static_cast<double>(xp.GainedXp()), true).c_str(),
                    FcFormat::Count(xp.XpPerHour()).c_str(),
                    FcFormat::Duration(xp.ActiveSeconds()).c_str(), xp.IsRunning() ? "" : " (paused)");
    } else {
        ImGui::TextDisabled("Waiting for character XP.");
    }
    if (d.tracker->MapExperience().HasSample())
        ImGui::Text("Current map: %s XP", FcFormat::Count(static_cast<double>(d.tracker->MapExperience().GainedXp()), true).c_str());

    ImGui::SeparatorText("Display");
    ImGui::TextUnformatted("Overlay currency:");
    const char* currencies[] = { "Exalted", "Divine", "Chaos" };
    ImGui::SetNextItemWidth(130.f);
    if (ImGui::Combo("##OvCur", &s.overlayCurrency, currencies, 3)) save();
    if (ImGui::SliderFloat("Opacity", &s.windowAlpha, 0.1f, 1.0f, "%.1f")) save();

    // Read-only readout of the core price service (the core app owns the league
    // and refresh schedule — Configuration -> Settings in POEFixer).
    ImGui::SeparatorText("Prices");
    {
        const PluginSDK::PriceStatus st = d.prices->Status();
        if (st.loaded) {
            ImGui::Text("Price DB: %d items", st.totalItems);
            const float divInChaos = d.prices->DivineInChaos();
            const float exInChaos  = d.prices->ExaltedInChaos();
            if (divInChaos > 1.f && exInChaos > 1.f)
                ImGui::Text("1 div = %.1f c   1 ex = %.1f c", divInChaos, exInChaos);
            if (st.catsFailed > 0)
                ImGui::TextColored(ImVec4(1.f, .8f, .2f, 1.f),
                                   "%d price categories failed to load", st.catsFailed);
        } else if (st.catsPending > 0) {
            ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.f), "Fetching prices...");
        } else {
            ImGui::TextDisabled("No price data yet");
        }
        ImGui::TextDisabled("League is configured in POEFixer Settings.");
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
    ImGui::SeparatorText("Unpriced items in inventory");
    // Recomputed at most once per second: each miss is a whole-DB fuzzy scan in
    // the host, and the snapshot itself only refreshes at ~1 Hz — per-frame
    // lookups here made the tab stutter for nothing.
    static std::vector<std::string> s_unpriced;
    static std::chrono::steady_clock::time_point s_unpricedAt{};
    {
        auto now = std::chrono::steady_clock::now();
        if (now - s_unpricedAt >= std::chrono::seconds(1)) {
            s_unpricedAt = now;
            s_unpriced.clear();
            for (const auto& [key, sn] : snap)
                if (!pp.Lookup(sn.name).found) s_unpriced.push_back(sn.name);  // core OR custom miss
            std::sort(s_unpriced.begin(), s_unpriced.end());
        }
    }
    const std::vector<std::string>& unpriced = s_unpriced;

    if (unpriced.empty()) {
        ImGui::TextDisabled("All items have prices.");
    } else {
        float listH = ImGui::GetTextLineHeightWithSpacing()
            * (float)(unpriced.size() < 6 ? unpriced.size() + 1 : 6);
        if (ImGui::BeginTable("##UPT", 2,
                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, listH))) {
            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, 80.f);
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
                if (ImGui::SmallButton("Set price")) {
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
        ImGui::SeparatorText("Set custom price");
        if (!s_editBase.empty()) ImGui::Text("Item: %s - %s", s_editName.c_str(), s_editBase.c_str());
        else                     ImGui::Text("Item: %s", s_editName.c_str());
        ImGui::SetNextItemWidth(120.f);
        ImGui::InputText("ex value##ep", s_editBuf, sizeof(s_editBuf), ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (ImGui::Button("Save##ep")) {
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
        if (ImGui::Button("Cancel##ep")) s_editMode = false;
    }

    // ── Existing custom prices ─────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Custom prices");
    (void)exRate;
    if (pp.Custom().empty()) {
        ImGui::TextDisabled("No custom prices set.");
        return;
    }

    int tableRows = (int)pp.Custom().size() + 1;
    if (tableRows > 10) tableRows = 10;
    float tableH = ImGui::GetTextLineHeightWithSpacing() * (float)tableRows;
    if (ImGui::BeginTable("##CPT", 3,
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
            ImVec2(0, tableH))) {
        ImGui::TableSetupColumn("Item",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Exalt", ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("##ops", ImGuiTableColumnFlags_WidthFixed, 110.f);
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
            ImGui::TableNextColumn(); ImGui::Text("%.3f ex", exalts);
            ImGui::TableNextColumn();
            ImGui::PushID(key.c_str());
            if (ImGui::SmallButton("Edit")) {
                s_editName = uname;
                s_editBase = bname;
                snprintf(s_editBuf, sizeof(s_editBuf), "%.4g", exalts);
                s_editMode = true;
            }
            ImGui::SameLine();
            {
                DangerButtonColors danger;
                if (ImGui::SmallButton("Delete")) toDelete = key;
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

    ImGui::SeparatorText("Hiveblood");
    if (ImGui::Checkbox("Show Hiveblood in overlay", &s.hbShow))      save();
    ImGui::Spacing();
    if (ImGui::Checkbox("Warn near cap (100,000)", &s.hbWarnNearCap)) save();
    if (ImGui::SliderInt("Warn threshold", &s.hbWarnThreshold, 50000, 100000)) save();
    if (ImGui::Checkbox("Show map gains", &s.hbShowMapGains))         save();

    ImGui::SeparatorText("Live");
    const ResourceReaders::HbState hb = d.resources->Hiveblood();
    if (hb.ok && !hb.cached)
        ImGui::TextColored(FcTheme::kHive, "Current: %s", FormatThousands(hb.total).c_str());
    else if (hb.ok && hb.cached)
        ImGui::Text("Current: %s (last seen)", FormatThousands(hb.total).c_str());
    else
        ImGui::TextDisabled("Current: not read yet (visit a map or the tree)");
}

void DrawBeaconsTab(const SettingsDeps& d) {
    OverlaySettings& s = *d.settings;
    auto save = [&] { SaveSettings(d.dir, s); };

    ImGui::SeparatorText("Atziri Beacons");
    ImGui::TextDisabled("Temple entry counter (N / 60), read from the game UI.");
    if (ImGui::Checkbox("Show Atziri Beacons overlay", &s.itShow))      save();
    if (ImGui::Checkbox("Sound on beacon gain", &s.itSound))            save();
    if (s.itSound) {
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::SliderFloat("Volume##itVol", &s.itVolume, 0.f, 1.f, "%.2f")) save();
        ImGui::SameLine();
        if (ImGui::Button("Test##itSnd") && d.onTestSound) d.onTestSound(s.itVolume);
    }
    if (ImGui::Checkbox("Show as separate window", &s.itSeparate))      save();

    ImGui::SeparatorText("Live");
    const ResourceReaders::ItState it = d.resources->Incursion();
    if (it.ok)
        ImGui::TextColored(FcTheme::kIncur, "Current: %d / %d", it.cur, it.max);
    else
        ImGui::TextDisabled("Beacon counter not visible yet (it is searched for periodically)");
}

void DrawKillsTab(const SettingsDeps& d) {
    OverlaySettings& s = *d.settings;
    auto save = [&] { SaveSettings(d.dir, s); };

    ImGui::SeparatorText("Kill Counter");
    if (ImGui::Checkbox("Show kill counter in overlay", &s.kcShow)) save();
    ImGui::Spacing();
    ImGui::TextDisabled("Show per rarity:");
    if (ImGui::Checkbox("Normal##kc", &s.kcShowNormal)) save(); ImGui::SameLine();
    if (ImGui::Checkbox("Magic##kc",  &s.kcShowMagic))  save(); ImGui::SameLine();
    if (ImGui::Checkbox("Rare##kc",   &s.kcShowRare))   save(); ImGui::SameLine();
    if (ImGui::Checkbox("Unique##kc", &s.kcShowUnique)) save(); ImGui::SameLine();
    if (ImGui::Checkbox("Rogue Exiles##kc", &s.kcShowRogue)) save();

    ImGui::SeparatorText("Counts (current area)");
    ImGui::Text("Normal: %d  Magic: %d  Rare: %d  Unique: %d  Rogue: %d  Total: %d",
                d.kills->Normal(), d.kills->Magic(), d.kills->Rare(),
                d.kills->Unique(), d.kills->Rogue(), d.kills->Total());
    if (ImGui::SmallButton("Reset##kc")) d.kills->Reset();
    ImGui::TextDisabled("Per-run and per-session kill totals live in the Statistics tab.");
}

} // namespace

void RenderSettings(const SettingsDeps& d) {
    if (!d.tracker || !d.prices || !d.settings || !d.zones || !d.kills || !d.resources)
        return;

    if (!ImGui::BeginTabBar("##FCTabs")) return;
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Trophy,  "Settings").c_str()))      { DrawSettingsTab(d);     ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Item,    "Custom Prices").c_str())) { DrawCustomPricesTab(d); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Profit,  "Statistics").c_str()))    { DrawStatisticsTab(d);   ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Map,     "Zones").c_str()))         { DrawZonesTab(d);        ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Session, "Hiveblood").c_str()))     { DrawHivebloodTab(d);    ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Sword,   "Atziri").c_str()))        { DrawBeaconsTab(d);      ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem(TabLabel(FcGlyph::Kills,   "Kills").c_str()))         { DrawKillsTab(d);        ImGui::EndTabItem(); }
    ImGui::EndTabBar();
}

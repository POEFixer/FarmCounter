// Settings.cpp — FarmCounter modernized settings tab bar.
//
// Behavior-preserving port of the old monolith's settings tabs
// (FarmCounter.cpp:132-637, plus DrawMapRunHeader :1112-1171 and
// TopDropsForSession :353-374), re-skinned with SeparatorText card sections and
// FcGlyph tab icons. Deliberate changes vs the old tabs:
//   - Settings tab: League combo + Refresh(min) slider removed; the price block
//     is a read-only PriceProvider::Status() readout (core owns league/refresh).
//   - Custom Prices: the three custom maps come from PriceProvider and are keyed
//     by PriceProvider::ToLower(name); live inventory names from LastSnapshot().
//   - Statistics: session totals from FarmTracker::Runs(); the profit/h
//     denominator is FarmTracker::SessionActiveSec() (already accumulator +
//     running interval); New Session delegates to FarmTracker::NewSession()
//     (which persists internally). Per-run loot prices render as text + currency
//     label (no IconTextures dependency in SettingsDeps).
//   - Kills/Hiveblood/Incursion: toggles persist via SaveSettings on change;
//     live resource reads come from ResourceReaders.
//
// Contracts honored: ZoneNames::Display() results are copied into a std::string
// immediately (DrawMapRunHeader), never held across a Map() mutation; custom
// prices are written to all three maps under the ToLower key before saving.
#include "Settings.h"
#include "Theme.h"
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

// ── Currency display helpers (port of old ChaosToDisplay :1055-1059 /
//    DisplayCurrencyLabel :1061-1065): 0=exalted, 1=divine, 2=chaos. ───────────
inline float ChaosToDisplay(int oc, float chaos, float exRate, float divRate) {
    if (oc == 1) return chaos / divRate;  // divine
    if (oc == 2) return chaos;            // chaos
    return chaos / exRate;                // exalted (default)
}
inline const char* CurrencyLabel(int oc) {
    if (oc == 1) return "div";
    if (oc == 2) return "c";
    return "ex";
}

// Port of old HbFormatThousands (FarmCounter.cpp:45-49).
std::string FormatThousands(int32_t n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

// Builds an icon-prefixed tab label ("<glyph>  Name"). The glyph bytes are pure
// ASCII escapes (see Theme.h) so this stays a single-allocation per frame.
std::string TabLabel(const char* glyph, const char* name) {
    return std::string(glyph) + "  " + name;
}

// Red SmallButton styling for destructive actions (port of old delete buttons).
struct DangerButtonColors {
    DangerButtonColors() {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(.6f, .15f, .15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.8f, .25f, .25f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(.45f, .1f, .1f, 1.f));
    }
    ~DangerButtonColors() { ImGui::PopStyleColor(3); }
};

// ── Statistics helpers ────────────────────────────────────────────────────────

// Top-3 loot entries by total value across a session, deduped by name
// (verbatim port of old TopDropsForSession :353-374).
std::vector<LootEntry> TopDropsForSession(const std::vector<MapRun>& runs,
                                          bool archived, int sessionId) {
    std::unordered_map<std::string, LootEntry> merged;
    for (const auto& r : runs) {
        if (r.archived != archived) continue;
        if (archived && r.sessionId != sessionId) continue;
        for (const auto& e : r.loot) {
            if (e.stackCount <= 0 || e.chaosEach <= 0.f) continue;
            auto& slot = merged[e.name];
            slot.name        = e.name;
            slot.stackCount += e.stackCount;
            slot.chaosEach   = e.chaosEach;
            slot.rarity      = e.rarity;
        }
    }
    std::vector<LootEntry> all;
    all.reserve(merged.size());
    for (const auto& kv : merged) all.push_back(kv.second);
    std::sort(all.begin(), all.end(), [](const LootEntry& a, const LootEntry& b) {
        return (a.chaosEach * (float)a.stackCount) > (b.chaosEach * (float)b.stackCount);
    });
    if (all.size() > 3) all.resize(3);
    return all;
}

// One collapsible map-run row (port of old DrawMapRunHeader :1112-1171). The
// per-loot price renders as "value label" text instead of a currency icon (no
// IconTextures in SettingsDeps). ZoneNames::Display() is copied into a
// std::string immediately so no reference is held across a later Map() mutation.
void DrawMapRunHeader(const SettingsDeps& d, const MapRun& r, int idx,
                      float exRate, float divRate, int cur) {
    const char* lbl  = CurrencyLabel(cur);
    const float useEx = r.exaltedRate > 0.f ? r.exaltedRate : exRate;
    const float runDisplay = ChaosToDisplay(cur, r.totalChaos, useEx, divRate);
    const int mm = r.durationSec / 60, ss = r.durationSec % 60;
    const std::string mapName = d.zones ? d.zones->Display(r.mapName) : r.mapName;

    char header[224];
    snprintf(header, sizeof(header), "%s  -  %.2f %s  -  %02d:%02d###mrun%d",
             mapName.c_str(), runDisplay, lbl, mm, ss, idx);
    if (!ImGui::CollapsingHeader(header)) return;

    if (r.hivebloodGain > 0)
        ImGui::TextColored(FcTheme::kHive, "  Hiveblood: +%s",
                           FormatThousands(r.hivebloodGain).c_str());

    if (r.loot.empty()) {
        ImGui::TextDisabled("  No priced loot recorded.");
        return;
    }

    std::vector<const LootEntry*> entries;
    entries.reserve(r.loot.size());
    for (const auto& e : r.loot) entries.push_back(&e);
    std::sort(entries.begin(), entries.end(), [](const LootEntry* a, const LootEntry* b) {
        return (a->chaosEach * (float)a->stackCount) > (b->chaosEach * (float)b->stackCount);
    });

    ImGuiTableFlags tfl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody;
    if (ImGui::BeginTable("##sl", 2, tfl)) {
        ImGui::TableSetupColumn("##n", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##p", ImGuiTableColumnFlags_WidthFixed, 90.f);
        for (const auto* e : entries) {
            const bool lost = e->stackCount < 0;
            const int  absStack = std::abs(e->stackCount);
            float val = ChaosToDisplay(cur, e->chaosEach * (float)absStack, useEx, divRate);
            if (lost) val = -val;
            const ImVec4 col = lost ? FcTheme::kLoss : FcTheme::kGain;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (absStack > 1) ImGui::Text("  %s %s  x%d", lost ? "-" : "+", e->name.c_str(), absStack);
            else              ImGui::Text("  %s %s",      lost ? "-" : "+", e->name.c_str());
            ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            char pb[48];
            snprintf(pb, sizeof(pb), "%.2f %s", val, lbl);
            const float pw    = ImGui::CalcTextSize(pb).x;
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > pw + 3.f)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - pw - 3.f);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(pb);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
}

// ── Tabs ──────────────────────────────────────────────────────────────────────

void DrawSettingsTab(const SettingsDeps& d) {
    OverlaySettings& s = *d.settings;
    auto save = [&] { SaveSettings(d.dir, s); };

    ImGui::Text("FarmCounter %s", ZONETIMER_VERSION);
    ImGui::Spacing();

    ImGui::SeparatorText("Overlay");
    if (ImGui::Checkbox("Enable Overlay Mode", &s.wantsOverlay))      save();
    if (ImGui::Checkbox("Show Item List",      &s.showItems))         save();
    if (ImGui::Checkbox("Show Unpriced Items", &s.showUnpriced))      save();
    if (ImGui::Checkbox("Show Profit/Hour",    &s.showProfitPerHour)) save();

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
        for (auto& [key, chaos] : pp.Custom()) {
            auto itN = pp.CustomNames().find(key);
            auto itB = pp.CustomBase().find(key);
            const std::string uname = (itN != pp.CustomNames().end()) ? itN->second : key;
            const std::string bname = (itB != pp.CustomBase().end())  ? itB->second : std::string();
            const std::string displayName = !bname.empty() ? uname + " - " + bname : uname;

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(displayName.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f ex", chaos);
            ImGui::TableNextColumn();
            ImGui::PushID(key.c_str());
            if (ImGui::SmallButton("Edit")) {
                s_editName = uname;
                s_editBase = bname;
                snprintf(s_editBuf, sizeof(s_editBuf), "%.4g", chaos);
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
    const int   cur     = d.settings->overlayCurrency;
    const float exRate  = d.prices->ExaltedInChaos();
    const float divRate = d.prices->DivineInChaos();
    const char* lbl     = CurrencyLabel(cur);

    // Current session summary (totals from Runs(); denominator = SessionActiveSec).
    int   sessionMaps  = 0;
    float sessionChaos = 0.f;
    for (const auto& r : d.tracker->Runs())
        if (!r.archived) { sessionChaos += r.totalChaos; sessionMaps++; }
    const float sessionDisplay = ChaosToDisplay(cur, sessionChaos, exRate, divRate);

    ImGui::SeparatorText("Current session");
    ImGui::Text("Session: %d maps   Total: %.2f %s", sessionMaps, sessionDisplay, lbl);
    const int activeSec = d.tracker->SessionActiveSec();
    if (activeSec > 0)
        ImGui::Text("Profit/h: %.2f %s", sessionDisplay / ((float)activeSec / 3600.f), lbl);
    for (const auto& e : TopDropsForSession(d.tracker->Runs(), false, 0)) {
        float val = ChaosToDisplay(cur, e.chaosEach * (float)e.stackCount, exRate, divRate);
        ImGui::TextDisabled("  %s  +%.2f %s", e.name.c_str(), val, lbl);
    }

    ImGui::Spacing();
    if (ImGui::Button("New Session")) d.tracker->NewSession();  // persists internally
    ImGui::SameLine();
    ImGui::TextDisabled("(archives current session; old runs stay)");
    ImGui::Separator();

    ImGui::BeginChild("##StatsScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    auto& runs = d.tracker->Runs();

    // Current session runs, newest-first.
    for (int i = (int)runs.size() - 1; i >= 0; i--) {
        if (runs[i].archived) continue;
        DrawMapRunHeader(d, runs[i], i, exRate, divRate, cur);
    }

    // Archived sessions, grouped by sessionId, newest session first.
    std::vector<int> sessionIds;
    for (const auto& r : runs) {
        if (!r.archived) continue;
        if (std::find(sessionIds.begin(), sessionIds.end(), r.sessionId) == sessionIds.end())
            sessionIds.push_back(r.sessionId);
    }
    std::sort(sessionIds.begin(), sessionIds.end(), [](int a, int b) { return a > b; });

    for (int sid : sessionIds) {
        float sChaos = 0.f; int sSec = 0; int sMaps = 0;
        for (const auto& r : runs) {
            if (!r.archived || r.sessionId != sid) continue;
            sChaos += r.totalChaos; sSec += r.durationSec; sMaps++;
        }
        const float sDisplay = ChaosToDisplay(cur, sChaos, exRate, divRate);
        const float pph = (sSec > 0) ? (sDisplay / ((float)sSec / 3600.f)) : 0.f;
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "Session %d  -  %d maps  -  %.2f %s total  -  %.1f %s/h###arcsess%d",
                 sid, sMaps, sDisplay, lbl, pph, lbl, sid);
        ImGui::Spacing();
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(hdr)) {
            for (const auto& e : TopDropsForSession(runs, true, sid)) {
                float val = ChaosToDisplay(cur, e.chaosEach * (float)e.stackCount, exRate, divRate);
                ImGui::TextDisabled("  %s  +%.2f %s", e.name.c_str(), val, lbl);
            }
            ImGui::Indent(12.f);
            for (int i = (int)runs.size() - 1; i >= 0; i--) {
                if (!runs[i].archived || runs[i].sessionId != sid) continue;
                DrawMapRunHeader(d, runs[i], i, exRate, divRate, cur);
            }
            ImGui::Unindent(12.f);
        }
    }

    ImGui::EndChild();
}

void DrawZonesTab(const SettingsDeps& d) {
    auto& names = d.zones->Map();

    static char        s_addRaw[128] = "";
    static std::string s_editKey;
    static char        s_editBuf[128] = "";

    ImGui::TextDisabled("Zones are added automatically when visited. Set a display name to rename them.");
    ImGui::Spacing();

    ImGui::SeparatorText("Add zone");
    ImGui::SetNextItemWidth(200.f);
    ImGui::InputText("Raw zone name##znraw", s_addRaw, sizeof(s_addRaw));
    ImGui::SameLine();
    if (ImGui::Button("Add##znadd")) {
        std::string raw = s_addRaw;
        if (!raw.empty()) {
            if (names.find(raw) == names.end()) names[raw] = "";
            s_addRaw[0] = '\0';
            d.zones->Save(d.dir);
        }
    }

    ImGui::SeparatorText("Known zones");
    std::vector<std::string> zones;
    zones.reserve(names.size());
    for (const auto& [k, v] : names) zones.push_back(k);
    std::sort(zones.begin(), zones.end());

    if (zones.empty()) {
        ImGui::TextDisabled("No zones yet. Visit a map to populate this list.");
        return;
    }

    float listH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
    if (listH < 80.f) listH = 80.f;
    if (ImGui::BeginTable("##ZNTable", 3,
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, listH))) {
        ImGui::TableSetupColumn("Raw Name",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Display Name", ImGuiTableColumnFlags_WidthFixed, 180.f);
        ImGui::TableSetupColumn("##ops",        ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableHeadersRow();

        std::string toDelete;
        for (const auto& raw : zones) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(raw.c_str());

            ImGui::TableNextColumn();
            ImGui::PushID(raw.c_str());
            const bool isEditing = (s_editKey == raw);
            if (isEditing) {
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::InputText("##znval", s_editBuf, sizeof(s_editBuf),
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
                    names[raw] = s_editBuf;
                    s_editKey.clear();
                    d.zones->Save(d.dir);
                }
                // Commit on click-away (matches old inline-edit behavior).
                if (!ImGui::IsItemActive() && !ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
                    names[raw] = s_editBuf;
                    s_editKey.clear();
                    d.zones->Save(d.dir);
                }
            } else {
                auto itv = names.find(raw);
                const std::string disp = (itv != names.end()) ? itv->second : std::string();
                ImGui::TextUnformatted(disp.empty() ? "(raw)" : disp.c_str());
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    s_editKey = raw;
                    snprintf(s_editBuf, sizeof(s_editBuf), "%s", disp.c_str());
                    ImGui::SetKeyboardFocusHere(-1);
                }
            }

            ImGui::TableNextColumn();
            {
                DangerButtonColors danger;
                if (ImGui::SmallButton("Del")) toDelete = raw;
            }
            ImGui::PopID();
        }
        if (!toDelete.empty()) {
            names.erase(toDelete);
            if (s_editKey == toDelete) s_editKey.clear();
            d.zones->Save(d.dir);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Double-click display name to edit, Enter to confirm.");
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
    if (ImGui::Checkbox("Unique##kc", &s.kcShowUnique)) save();

    ImGui::SeparatorText("Counts");
    ImGui::Text("Normal: %d  Magic: %d  Rare: %d  Unique: %d  Total: %d",
                d.kills->Normal(), d.kills->Magic(), d.kills->Rare(),
                d.kills->Unique(), d.kills->Total());
    if (ImGui::SmallButton("Reset##kc")) d.kills->Reset();
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

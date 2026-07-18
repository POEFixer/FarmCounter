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

// ── Currency display helpers: 0=exalted, 1=divine, 2=chaos. ──────────────────
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

std::string FormatThousands(long long n) {
    std::string s = std::to_string(n);
    const int stop = (n < 0) ? 1 : 0;   // don't split a leading minus
    for (int i = (int)s.size() - 3; i > stop; i -= 3) s.insert(i, ",");
    return s;
}

// "H:MM:SS" above an hour, else "MM:SS".
std::string FormatDuration(int sec) {
    if (sec < 0) sec = 0;
    char buf[32];
    if (sec >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
    else             snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);
    return buf;
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

// Inline two-step destructive button (no modal — modals freeze in overlay
// mode). First click arms it as a red "Sure?" for 3 seconds; the second click
// confirms. `strId` keys the ImGui ID (call under a unique PushID scope).
bool ConfirmSmallButton(const char* text, const char* strId) {
    static ImGuiID s_armed   = 0;
    static double  s_armedAt = 0.0;
    const ImGuiID id  = ImGui::GetID(strId);
    const double  now = ImGui::GetTime();
    if (s_armed == id && (now - s_armedAt) > 3.0) s_armed = 0;

    char label[96];
    const bool armed = (s_armed == id);
    snprintf(label, sizeof(label), "%s###%s", armed ? "Sure?" : text, strId);
    DangerButtonColors danger;
    if (!ImGui::SmallButton(label)) return false;
    if (armed) { s_armed = 0; return true; }
    s_armed = id; s_armedAt = now;
    return false;
}

// Small colored square ("rarity dot") drawn inline at the cursor — fallback
// when the radar atlas isn't available.
void RarityDot(ImVec4 col) {
    float sz  = ImGui::GetTextLineHeight() * 0.70f;
    float pad = (ImGui::GetTextLineHeight() - sz) * 0.5f;
    ImVec2 p  = ImGui::GetCursorScreenPos();
    p.y += pad;
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz),
        ImGui::ColorConvertFloat4ToU32(col), 2.f);
    ImGui::Dummy(ImVec2(sz + 3.f, ImGui::GetTextLineHeight()));
}

// One rarity marker: the radar-atlas monster icon (same sprites the Radar draws
// for mobs), colored-dot fallback when the atlas is missing.
void RarityMark(IconTextures* icons, int rarity, ImVec4 dotCol) {
    if (icons) {
        const AtlasIcon& ic = icons->Monster(rarity);
        if (ic.valid) {
            const float lh = ImGui::GetTextLineHeight();
            ImGui::Image(ic.tex, ImVec2(lh, lh), ic.uv0, ic.uv1);
            return;
        }
    }
    RarityDot(dotCol);
}

// Rogue Exile inline marker: the radar atlas Rogue icon, dot fallback.
void RogueMark(IconTextures* icons) {
    if (icons) {
        const AtlasIcon& ic = icons->RogueExile();
        if (ic.valid) {
            const float lh = ImGui::GetTextLineHeight();
            ImGui::Image(ic.tex, ImVec2(lh, lh), ic.uv0, ic.uv1);
            return;
        }
    }
    RarityDot(ImVec4(0.85f, 0.35f, 0.15f, 1.f));
}

// Per-rarity kill counts inline: monster icon + count per nonzero rarity, then
// the Rogue Exile count (own icon) and "(total)".
void KillsInline(IconTextures* icons, int n, int m, int r, int u, int rogue) {
    struct { int v; int rarity; ImVec4 c; } parts[4] = {
        { n, 0, ImVec4(0.85f, 0.85f, 0.85f, 1.f) },   // Normal — white
        { m, 1, ImVec4(0.33f, 0.53f, 1.f,   1.f) },   // Magic — blue
        { r, 2, ImVec4(1.f,   0.87f, 0.2f,  1.f) },   // Rare — yellow
        { u, 3, ImVec4(1.f,   0.5f,  0.1f,  1.f) },   // Unique — orange
    };
    bool any = false;
    for (const auto& p : parts) {
        if (p.v <= 0) continue;
        any = true;
        RarityMark(icons, p.rarity, p.c);
        ImGui::SameLine(0.f, 2.f); ImGui::Text("%d", p.v); ImGui::SameLine(0.f, 8.f);
    }
    if (rogue > 0) {
        any = true;
        RogueMark(icons);
        ImGui::SameLine(0.f, 2.f); ImGui::Text("%d", rogue); ImGui::SameLine(0.f, 8.f);
    }
    if (any) ImGui::Text("(%d)", n + m + r + u + rogue);
    else     ImGui::TextUnformatted("0");
}

// One loot line's name cell: item icon (when cached) + signed colored name.
void LootNameCell(IconTextures* icons, const LootEntry& e, ImVec4 col, float lineH) {
    const bool lost   = e.stackCount < 0;
    const int  absStk = std::abs(e.stackCount);
    if (icons) {
        IconTex tex = icons->Item(e.iconPath);
        if (tex.valid && tex.h > 0) {
            ImGui::Image(tex.srv, ImVec2(lineH * (float)tex.w / (float)tex.h, lineH));
            ImGui::SameLine(0.f, 4.f);
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    if (absStk > 1) ImGui::Text("%s %s  x%d", lost ? "-" : "+", e.name.c_str(), absStk);
    else            ImGui::Text("%s %s",      lost ? "-" : "+", e.name.c_str());
    ImGui::PopStyleColor();
}

// Value-sorted loot table with item icons and a right-aligned value + currency
// icon column (the same shape the overlay uses).
void LootTable(const SettingsDeps& d, const std::vector<LootEntry>& loot,
               float exRate, float divRate, int cur, const char* tableId) {
    if (loot.empty()) { ImGui::TextDisabled("  No loot recorded."); return; }

    std::vector<const LootEntry*> entries;
    entries.reserve(loot.size());
    for (const auto& e : loot) entries.push_back(&e);
    std::sort(entries.begin(), entries.end(), [](const LootEntry* a, const LootEntry* b) {
        return (a->chaosEach * (float)std::abs(a->stackCount)) >
               (b->chaosEach * (float)std::abs(b->stackCount));
    });

    const float lineH = ImGui::GetTextLineHeight();
    const IconTex ci  = d.icons ? d.icons->Currency(cur) : IconTex{};
    const float ciW   = (ci.valid && ci.h > 0) ? lineH * (float)ci.w / (float)ci.h : 0.f;

    ImGuiTableFlags tfl = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody;
    if (!ImGui::BeginTable(tableId, 2, tfl)) return;
    ImGui::TableSetupColumn("##n", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##p", ImGuiTableColumnFlags_WidthFixed, 96.f);
    for (const auto* e : entries) {
        const bool lost = e->stackCount < 0;
        float val = ChaosToDisplay(cur, e->chaosEach * (float)std::abs(e->stackCount), exRate, divRate);
        if (lost) val = -val;
        const ImVec4 col = lost ? FcTheme::kLoss : FcTheme::kGain;

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        LootNameCell(d.icons, *e, col, lineH);

        ImGui::TableNextColumn();
        if (e->chaosEach > 0.f) {
            char pb[32]; snprintf(pb, sizeof(pb), "%.2f", val);
            const float gap   = ciW > 0.f ? 3.f : 0.f;
            const float pw    = ImGui::CalcTextSize(pb).x + gap + ciW;
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > pw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - pw);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(pb);
            ImGui::PopStyleColor();
            if (ciW > 0.f) { ImGui::SameLine(0.f, gap); ImGui::Image(ci.srv, ImVec2(ciW, lineH)); }
        } else {
            const char* np = "--";
            const float pw = ImGui::CalcTextSize(np).x;
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > pw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - pw);
            ImGui::TextDisabled("%s", np);
        }
    }
    ImGui::EndTable();
}

// ── Statistics aggregation ────────────────────────────────────────────────────

struct RunAgg {
    int   maps = 0, seconds = 0, hiveblood = 0, beacons = 0;
    int   killsN = 0, killsM = 0, killsR = 0, killsU = 0, killsG = 0;
    long long gold = 0;
    float chaos = 0.f;
    void Add(const MapRun& r) {
        maps++; seconds += r.durationSec; chaos += r.totalChaos;
        hiveblood += r.hivebloodGain; beacons += r.beaconGain;
        gold += r.goldGain;
        killsN += r.killsNormal; killsM += r.killsMagic;
        killsR += r.killsRare;   killsU += r.killsUnique;
        killsG += r.killsRogue;
    }
    int KillsTotal() const { return killsN + killsM + killsR + killsU + killsG; }
};

// Top loot entries by total value across a set of runs, deduped by name
// (keeps the first non-empty icon so top drops render with images).
std::vector<LootEntry> TopDrops(const std::vector<MapRun>& runs,
                                bool archived, int sessionId, size_t topN) {
    std::unordered_map<std::string, LootEntry> merged;
    for (const auto& r : runs) {
        if (r.archived != archived) continue;
        if (archived && sessionId >= 0 && r.sessionId != sessionId) continue;
        for (const auto& e : r.loot) {
            if (e.stackCount <= 0 || e.chaosEach <= 0.f) continue;
            auto& slot = merged[e.name];
            slot.name        = e.name;
            slot.stackCount += e.stackCount;
            slot.chaosEach   = e.chaosEach;
            slot.rarity      = e.rarity;
            if (slot.iconPath.empty()) slot.iconPath = e.iconPath;
        }
    }
    std::vector<LootEntry> all;
    all.reserve(merged.size());
    for (const auto& kv : merged) all.push_back(kv.second);
    std::sort(all.begin(), all.end(), [](const LootEntry& a, const LootEntry& b) {
        return (a.chaosEach * (float)a.stackCount) > (b.chaosEach * (float)b.stackCount);
    });
    if (all.size() > topN) all.resize(topN);
    return all;
}

void TopDropsBlock(const SettingsDeps& d, const std::vector<LootEntry>& drops,
                   float exRate, float divRate, int cur) {
    if (drops.empty()) return;
    const float lineH = ImGui::GetTextLineHeight();
    ImGui::TextDisabled("Top drops:");
    for (const auto& e : drops) {
        ImGui::Bullet(); ImGui::SameLine(0.f, 2.f);
        LootNameCell(d.icons, e, FcTheme::kGain, lineH);
        ImGui::SameLine(0.f, 8.f);
        float val = ChaosToDisplay(cur, e.chaosEach * (float)e.stackCount, exRate, divRate);
        ImGui::TextColored(FcTheme::kDim, "%.2f %s", val, CurrencyLabel(cur));
    }
}

// One collapsible run row. Returns true when the user confirmed deleting it.
bool DrawRunRow(const SettingsDeps& d, const MapRun& r, int idx, bool deletable,
                float exRate, float divRate, int cur) {
    const char* lbl   = CurrencyLabel(cur);
    const float useEx = r.exaltedRate > 0.f ? r.exaltedRate : exRate;
    const float val   = ChaosToDisplay(cur, r.totalChaos, useEx, divRate);
    // Date-stamped header; ### keys the ID to the stable dbId (idx appended so
    // rows stay unique even when the DB failed to open and every dbId is 0).
    char header[256];
    snprintf(header, sizeof(header), "%s  -  %.2f %s  -  %s%s  -  %s###mrun%lld_%d",
             r.mapName.c_str(), val, lbl, FormatDuration(r.durationSec).c_str(),
             deletable ? "" : "  [live]",
             r.startedText.empty() ? "?" : r.startedText.c_str(),
             (long long)r.dbId, idx);
    if (!ImGui::CollapsingHeader(header)) return false;

    bool wantDelete = false;
    ImGui::PushID(idx);
    ImGui::Indent(10.f);

    // Meta line: start time + per-run counters + delete.
    ImGui::TextDisabled("Started %s", r.startedText.empty() ? "?" : r.startedText.c_str());
    if (deletable) {
        ImGui::SameLine(0.f, 14.f);
        if (ConfirmSmallButton("Delete run", "delrun")) wantDelete = true;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(FcTheme::kDim, "Kills:");
    ImGui::SameLine(0.f, 6.f);
    KillsInline(d.icons, r.killsNormal, r.killsMagic, r.killsRare, r.killsUnique, r.killsRogue);
    if (r.goldGain > 0) {
        ImGui::SameLine(0.f, 14.f);
        ImGui::TextColored(FcTheme::kGold, "Gold +%s", FormatThousands(r.goldGain).c_str());
    }
    if (r.hivebloodGain > 0) {
        ImGui::SameLine(0.f, 14.f);
        ImGui::TextColored(FcTheme::kHive, "Hiveblood +%s", FormatThousands(r.hivebloodGain).c_str());
    }
    if (r.beaconGain > 0) {
        ImGui::SameLine(0.f, 14.f);
        ImGui::TextColored(FcTheme::kIncur, "Beacons +%d", r.beaconGain);
    }
    if (r.durationSec > 0 && r.totalChaos != 0.f) {
        const float pph = ChaosToDisplay(cur, r.totalChaos, useEx, divRate) / ((float)r.durationSec / 3600.f);
        ImGui::TextColored(FcTheme::kDim, "Rate: %.1f %s/h", pph, lbl);
    }

    // Map modifiers (collapsed by default — the panel can be long).
    if (!r.mapMods.empty()) {
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        char mh[48]; snprintf(mh, sizeof(mh), "Map modifiers (%d)###mm", (int)r.mapMods.size());
        if (ImGui::CollapsingHeader(mh)) {
            ImGui::Indent(8.f);
            for (const auto& line : r.mapMods)
                ImGui::TextColored(FcTheme::kMod, "%s", line.c_str());
            ImGui::Unindent(8.f);
        }
    }

    char tid[32]; snprintf(tid, sizeof(tid), "##rl%lld", (long long)r.dbId);
    LootTable(d, r.loot, useEx, divRate, cur, tid);

    ImGui::Unindent(10.f);
    ImGui::PopID();
    return wantDelete;
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
    if (ImGui::Checkbox("Show Gold (total + map gain)", &s.goldShow)) save();

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
    const int   cur     = d.settings->overlayCurrency;
    const float exRate  = d.prices->ExaltedInChaos();
    const float divRate = d.prices->DivineInChaos();
    const char* lbl     = CurrencyLabel(cur);
    auto& runs          = d.tracker->Runs();
    const int activeIdx = d.tracker->ActiveRunIndex();

    // Deferred mutations (applied after all rendering — never mutate the runs
    // vector while iterating it).
    int  deleteRunIdx     = -1;
    int  deleteSessionId  = INT_MIN;
    bool deleteAllArchive = false;

    if (!d.tracker->DbOpen())
        ImGui::TextColored(ImVec4(1.f, .6f, .2f, 1.f),
                           "Database failed to open — statistics won't persist this session.");

    // ── Current session ────────────────────────────────────────────────────────
    ImGui::SeparatorText("Current session");
    {
        RunAgg agg;
        for (const auto& r : runs) if (!r.archived) agg.Add(r);
        const int   activeSec = d.tracker->SessionActiveSec();
        const float total     = ChaosToDisplay(cur, agg.chaos, exRate, divRate);
        const float pph       = activeSec > 0 ? total / ((float)activeSec / 3600.f) : 0.f;

        ImGui::Text("Session %d", d.tracker->CurrentSessionId() + 1);
        ImGui::SameLine(0.f, 10.f);
        ImGui::TextColored(FcTheme::kDim, "%s", FcGlyph::Session);
        ImGui::SameLine(0.f, 4.f);
        ImGui::TextUnformatted(FormatDuration(activeSec).c_str());
        if (d.tracker->IsPaused()) {
            ImGui::SameLine(0.f, 8.f);
            ImGui::TextColored(ImVec4(1.f, .8f, .2f, 1.f), "PAUSED");
        } else if (!d.tracker->SessionRunning()) {
            ImGui::SameLine(0.f, 8.f);
            ImGui::TextColored(FcTheme::kDim, "(idle)");
        }
        ImGui::SameLine(0.f, 12.f);
        ImGui::TextColored(FcTheme::kDim, "%s", FcGlyph::Map);
        ImGui::SameLine(0.f, 4.f);
        ImGui::Text("%d", agg.maps);
        ImGui::SameLine(0.f, 12.f);
        ImGui::TextColored(total >= 0.f ? FcTheme::kGain : FcTheme::kLoss,
                           "%.2f %s  (%.1f %s/h)", total, lbl, pph, lbl);

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(FcTheme::kDim, "Kills:");
        ImGui::SameLine(0.f, 6.f);
        KillsInline(d.icons, agg.killsN, agg.killsM, agg.killsR, agg.killsU, agg.killsG);
        if (agg.gold > 0) {
            ImGui::SameLine(0.f, 14.f);
            ImGui::TextColored(FcTheme::kGold, "Gold +%s", FormatThousands(agg.gold).c_str());
        }
        if (agg.hiveblood > 0) {
            ImGui::SameLine(0.f, 14.f);
            ImGui::TextColored(FcTheme::kHive, "Hiveblood +%s", FormatThousands(agg.hiveblood).c_str());
        }
        if (agg.beacons > 0) {
            ImGui::SameLine(0.f, 14.f);
            ImGui::TextColored(FcTheme::kIncur, "Beacons +%d", agg.beacons);
        }

        TopDropsBlock(d, TopDrops(runs, false, -1, 3), exRate, divRate, cur);

        ImGui::Spacing();
        if (ImGui::Button("New Session")) d.tracker->NewSession();  // persists internally
        ImGui::SameLine();
        ImGui::TextDisabled("(archives current runs; history stays below)");
    }

    // ── All time ───────────────────────────────────────────────────────────────
    ImGui::SeparatorText("All time");
    {
        RunAgg agg;
        const MapRun* best = nullptr;
        for (const auto& r : runs) {
            agg.Add(r);
            if (!best || r.totalChaos > best->totalChaos) best = &r;
        }
        if (agg.maps == 0) {
            ImGui::TextDisabled("No runs recorded yet.");
        } else {
            const float total = ChaosToDisplay(cur, agg.chaos, exRate, divRate);
            ImGui::Text("%d maps  -  %s farmed  -  %.2f %s total  -  %s kills",
                        agg.maps, FormatDuration(agg.seconds).c_str(), total, lbl,
                        FormatThousands(agg.KillsTotal()).c_str());
            if (agg.seconds > 0)
                ImGui::TextColored(FcTheme::kDim, "Average: %.2f %s/map   %.1f %s/h",
                                   agg.maps > 0 ? total / (float)agg.maps : 0.f, lbl,
                                   total / ((float)agg.seconds / 3600.f), lbl);
            if (agg.gold > 0 || agg.hiveblood > 0 || agg.beacons > 0) {
                bool first = true;
                if (agg.gold > 0) {
                    ImGui::TextColored(FcTheme::kGold, "Gold +%s", FormatThousands(agg.gold).c_str());
                    first = false;
                }
                if (agg.hiveblood > 0) {
                    if (!first) ImGui::SameLine(0.f, 14.f);
                    ImGui::TextColored(FcTheme::kHive, "Hiveblood +%s", FormatThousands(agg.hiveblood).c_str());
                    first = false;
                }
                if (agg.beacons > 0) {
                    if (!first) ImGui::SameLine(0.f, 14.f);
                    ImGui::TextColored(FcTheme::kIncur, "Beacons +%d", agg.beacons);
                }
            }
            if (best && best->totalChaos > 0.f) {
                const float bv = ChaosToDisplay(cur, best->totalChaos,
                                                best->exaltedRate > 0.f ? best->exaltedRate : exRate, divRate);
                ImGui::TextColored(FcTheme::kAccent, "%s", FcGlyph::Trophy);
                ImGui::SameLine(0.f, 5.f);
                ImGui::Text("Best run: %s  -  %.2f %s  (%s)",
                            best->mapName.c_str(), bv, lbl,
                            best->startedText.empty() ? "?" : best->startedText.c_str());
            }
        }
    }

    ImGui::SeparatorText("History");
    ImGui::BeginChild("##StatsScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    // Current session runs, newest-first.
    for (int i = (int)runs.size() - 1; i >= 0; i--) {
        if (runs[i].archived) continue;
        if (DrawRunRow(d, runs[i], i, i != activeIdx, exRate, divRate, cur))
            deleteRunIdx = i;
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
        RunAgg agg;
        for (const auto& r : runs)
            if (r.archived && r.sessionId == sid) agg.Add(r);
        const float sTotal = ChaosToDisplay(cur, agg.chaos, exRate, divRate);
        const float pph    = (agg.seconds > 0) ? (sTotal / ((float)agg.seconds / 3600.f)) : 0.f;
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "Session %d  -  %d maps  -  %.2f %s  -  %.1f %s/h  -  %s kills###arcsess%d",
                 sid, agg.maps, sTotal, lbl, pph, lbl,
                 FormatThousands(agg.KillsTotal()).c_str(), sid);
        ImGui::Spacing();
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader(hdr)) {
            ImGui::PushID(sid);
            ImGui::Indent(10.f);
            if (ConfirmSmallButton("Delete session", "delsess"))
                deleteSessionId = sid;
            TopDropsBlock(d, TopDrops(runs, true, sid, 3), exRate, divRate, cur);
            ImGui::Spacing();
            for (int i = (int)runs.size() - 1; i >= 0; i--) {
                if (!runs[i].archived || runs[i].sessionId != sid) continue;
                if (DrawRunRow(d, runs[i], i, true, exRate, divRate, cur))
                    deleteRunIdx = i;
            }
            ImGui::Unindent(10.f);
            ImGui::PopID();
        }
    }

    if (!sessionIds.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        if (ConfirmSmallButton("Delete ALL archived history", "delallarch"))
            deleteAllArchive = true;
        ImGui::SameLine();
        ImGui::TextDisabled("(current session is kept)");
    }

    ImGui::EndChild();

    // Apply deferred mutations.
    if (deleteAllArchive)             d.tracker->DeleteAllArchived();
    else if (deleteSessionId != INT_MIN) d.tracker->DeleteArchivedSession(deleteSessionId);
    else if (deleteRunIdx >= 0)       d.tracker->DeleteRunAt(deleteRunIdx);
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

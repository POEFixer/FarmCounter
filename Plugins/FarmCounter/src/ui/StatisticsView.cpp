#include "StatisticsView.h"
#include "Settings.h"
#include "Theme.h"
#include "Formatting.h"
#include "../StatisticsModel.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <set>
#include <unordered_map>

namespace Stats = FarmStatistics;
namespace {
constexpr float kViewInset = 12.f;
constexpr float kViewTopInset = 2.f;
const char* Unit(int currency) { return currency == 1 ? "div" : currency == 2 ? "chaos" : "ex"; }
std::string Money(double value, int currency) {
    if (!std::isfinite(value)) return "--";
    if (std::fabs(value) >= 10000000.0) return FcFormat::Count(value, true) + " " + Unit(currency);
    char text[96];
    std::snprintf(text, sizeof(text), "%+.2f %s", value == 0 ? 0 : value, Unit(currency));
    return text;
}
ImVec4 ValueColor(double value) { return value < 0 ? FcTheme::kLoss : FcTheme::kGain; }
std::string Fold(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
void Tip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.f);
    ImGui::TextUnformatted(text); ImGui::PopTextWrapPos(); ImGui::EndTooltip();
}
const std::string& MapName(const SettingsDeps& d, const MapRun& run) {
    return d.zones ? d.zones->Display(run.mapName) : run.mapName;
}
std::string Date(const MapRun& run) {
    if (!run.startedText.empty()) return run.startedText;
    if (run.startedAt > 0) {
        const auto timestamp = static_cast<time_t>(run.startedAt); std::tm local{};
        if (localtime_s(&local, &timestamp) == 0) {
            char text[32]; std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local); return text;
        }
    }
    return run.dbId > 0 ? "Unknown date" : "Not yet saved";
}
std::string EndDate(const MapRun& run) {
    if (run.startedAt > 0) {
        const auto timestamp = static_cast<time_t>(run.startedAt + (std::max)(0, run.durationSec));
        std::tm local{};
        if (localtime_s(&local, &timestamp) == 0) {
            char text[32]; std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local); return text;
        }
    }
    return Date(run);
}
std::string KillBreakdown(const std::array<int64_t, 4>& kills) {
    return "Normal " + FcFormat::Count(static_cast<double>(kills[0])) +
        " | Magic " + FcFormat::Count(static_cast<double>(kills[1])) +
        " | Rare " + FcFormat::Count(static_cast<double>(kills[2])) +
        " | Unique " + FcFormat::Count(static_cast<double>(kills[3]));
}
std::string KillCell(const MapRun& run) {
    const auto kills = run.KillsByRarity();
    return FcFormat::Count(static_cast<double>(kills[0])) + "/" +
        FcFormat::Count(static_cast<double>(kills[1])) + "/" +
        FcFormat::Count(static_cast<double>(kills[2])) + "/" +
        FcFormat::Count(static_cast<double>(kills[3]));
}
void DrawKillIcons(const SettingsDeps& d, const std::array<int64_t, 5>& counts) {
    const float size = (std::max)(12.f, ImGui::GetTextLineHeight() * .9f);
    const char* labels[] = {"Normal monsters", "Magic monsters", "Rare monsters",
        "Unique monsters", "Rogue Exiles"};
    const ImVec4 colors[] = {{1.f, 1.f, 1.f, 1.f}, {0.4f, 0.7f, 1.f, 1.f},
        {1.f, 1.f, 0.4f, 1.f}, {1.f, 0.5f, 0.f, 1.f}, {0.85f, 0.45f, 1.f, 1.f}};
    bool first = true;
    for (int i = 0; i < 5; ++i) {
        if (!first) ImGui::SameLine(0, 6);
        AtlasIcon empty;
        const AtlasIcon& icon = i == 4
            ? (d.icons ? d.icons->RogueExile() : empty)
            : (d.icons ? d.icons->Monster(i) : empty);
        if (icon.valid) ImGui::Image(icon.tex, ImVec2(size, size), icon.uv0, icon.uv1);
        else ImGui::TextColored(colors[i], "%s", "●");
        ImGui::SameLine(0, 2);
        ImGui::TextUnformatted(FcFormat::Count(static_cast<double>(counts[i])).c_str());
        Tip(labels[i]);
        first = false;
    }
}
void DrawKillIcons(const SettingsDeps& d, const MapRun& run) {
    DrawKillIcons(d, std::array<int64_t, 5>{run.killsNormal, run.killsMagic, run.killsRare,
        run.killsUnique, run.killsRogue});
}
const char* StatusGlyph(const char* status) {
    if (std::strcmp(status, "Live") == 0) return FcGlyph::Live;
    if (std::strcmp(status, "Paused") == 0) return FcGlyph::Paused;
    return FcGlyph::Completed;
}
ImVec4 StatusColor(const char* status) {
    if (std::strcmp(status, "Live") == 0) return FcTheme::kXp;
    if (std::strcmp(status, "Paused") == 0) return FcTheme::kAccent;
    return FcTheme::kGain;
}
bool ResizeDown(const char* id, float& height, float minimum) {
    const float handleHeight = (std::max)(6.f, ImGui::GetFrameHeight() * .35f);
    const float width = (std::max)(1.f, ImGui::GetContentRegionAvail().x);
    const float previous = height;
    ImGui::InvisibleButton(id, ImVec2(width, handleHeight));
    // The handle sits inside a scrolling child, so the remaining content
    // height can be zero even though the child can scroll further. Bound the
    // user-controlled pane by the viewport instead of the current remainder.
    const float maximum = (std::max)(minimum, (std::max)(1600.f, ImGui::GetWindowSize().y * 2.f));
    if (ImGui::IsItemActive()) {
        height = std::clamp(height + ImGui::GetIO().MouseDelta.y, minimum, maximum);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    const ImU32 color = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_SeparatorActive :
        ImGui::IsItemHovered() ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator);
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(min.x, min.y + handleHeight * .4f),
        ImVec2(max.x, max.y - handleHeight * .4f), color, 1.f);
    return height != previous;
}
const char* Status(const SettingsDeps& d, size_t index) {
    if (d.tracker->ActiveRunIndex() >= 0 && index == static_cast<size_t>(d.tracker->ActiveRunIndex()))
        return d.tracker->IsPaused() || !d.tracker->InMap() ? "Paused" : "Live";
    return d.tracker->Runs()[index].archived ? "Archived" : "Completed";
}
void Card(const char* label, const std::string& value, const std::string& footer,
          const ImVec4& accent, float width, float height, const char* tooltip) {
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(11, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 7.f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    if (ImGui::BeginChild("##card", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar)) {
        const auto position = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(position.x + 9, position.y + 1),
            ImVec2(position.x + width - 9, position.y + 1), ImGui::ColorConvertFloat4ToU32(accent), 2.f);
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 6);
        ImGui::TextColored(accent, "%s", value.c_str()); ImGui::SameLine(0, 6);
        ImGui::TextDisabled("%s", footer.c_str());
    }
    ImGui::EndChild(); Tip(tooltip);
    ImGui::PopStyleColor(); ImGui::PopStyleVar(2); ImGui::PopID();
}
void CurrencyTip(int kind) {
    Tip(kind == 0 ? "Exalted values use each run's saved exchange rate. A missing saved rate uses the current rate."
        : kind == 1 ? "Divine values use the current divine exchange rate. Recorded item prices are unchanged."
                    : "Chaos values are the prices recorded with each run. Historical items are not repriced.");
}
float CategoryHeight() { return (std::max)(40.f, ImGui::GetTextLineHeight() * 1.85f + 6); }
float ComboWidth(float minimum, std::initializer_list<const char*> labels) {
    for (const auto* label : labels) minimum = (std::max)(minimum,
        std::ceil(ImGui::CalcTextSize(label).x + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x) + 1);
    return minimum;
}
bool NextControl(float width) {
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - kViewInset;
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width > right) return false;
    ImGui::SameLine(); return true;
}
std::string WrapControlText(const char* text, float width) {
    std::string result, line;
    const std::string source(text);
    for (size_t begin = 0; begin < source.size();) {
        const size_t space = source.find(' ', begin);
        const std::string word = source.substr(begin, space == std::string::npos ? space : space - begin);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && ImGui::CalcTextSize(candidate.c_str()).x > width) {
            result += line + '\n'; line = word;
        } else line = candidate;
        if (space == std::string::npos) break;
        begin = space + 1;
    }
    return result + line;
}
bool BeginMetricLine(const char* id, float textWidth, bool frameHeight = false) {
    const ImVec2 padding(8, 6);
    const float available = std::floor(ImGui::GetContentRegionAvail().x);
    const float required = std::ceil(textWidth);
    const bool overflow = required + 2 * padding.x > available;
    const float lineHeight = frameHeight ? (std::max)(ImGui::GetTextLineHeight(), ImGui::GetFrameHeight()) : ImGui::GetTextLineHeight();
    const float height = lineHeight + 2 * padding.y + 1 +
        (overflow ? ImGui::GetStyle().ScrollbarSize : 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::SetNextWindowContentSize(ImVec2((std::max)(required, available - 2 * padding.x), 0));
    const bool visible = ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_HorizontalScrollbar);
    if (visible) ImGui::PushTextWrapPos(-1);
    return visible;
}
void EndMetricLine(bool visible) {
    if (visible) ImGui::PopTextWrapPos();
    ImGui::EndChild(); ImGui::PopStyleVar();
}
float TableInnerWidth(float minimum) {
    // Auto width follows the real viewport, including its vertical scrollbar.
    // Passing the outer available width instead forces an empty gutter of
    // horizontal scrolling. Retain an explicit minimum when space is tight.
    return minimum + ImGui::GetStyle().ScrollbarSize <= ImGui::GetContentRegionAvail().x ? 0.f : minimum;
}
} // namespace

struct StatisticsView::State {
    enum class Action { None, NewSession, DeleteRun, DeleteSession, DeleteArchived };
    struct Selection { int64_t id = 0; size_t index = Stats::NoRun; uint64_t structure = 0; };
    struct Request { Action action = Action::None; Selection run; int session = -1; std::string description; };

    uint64_t structure = (std::numeric_limits<uint64_t>::max)();
    uint64_t history = (std::numeric_limits<uint64_t>::max)();
    uint64_t zonesRevision = (std::numeric_limits<uint64_t>::max)();
    uint64_t generation = 0, lootGeneration = 0, detailGeneration = 0, trendGeneration = 0;
    int64_t minute = -1;
    int sessionMode = 0, archivedSession = -1, period = 0, currencyKind = 0, categoryOrder = 0;
    int activeIndex = (std::numeric_limits<int>::min)();
    bool dirty = true, categoryDirty = true, showTrend = false, hasArchived = false;
    float runTableHeight = 205.f, lootTotalsHeight = 220.f;
    float detailLootHeight = 145.f, detailModsHeight = 145.f;
    Stats::Currency currency;
    Stats::Sort sort = Stats::Sort::Date;
    bool ascending = false;
    std::string category, error, lastSearch, widestSessionLabel;
    std::array<char, 128> search{};
    Stats::Snapshot snapshot;
    std::vector<int> sessions;
    std::unordered_map<int64_t, size_t> byId;
    std::vector<size_t> categoryRows, recent;
    size_t allCount = 0;
    Selection selected;
    Request pending, apply;
    std::vector<Stats::LootTotal> lootTotals, detailLoot;

    void SyncStructure(const SettingsDeps& d) {
        const auto revision = d.tracker->HistoryStructureRevision();
        if (structure == revision) return;
        structure = revision; dirty = true; byId.clear();
        std::set<int, std::greater<int>> archived;
        const auto& runs = d.tracker->Runs();
        for (size_t i = 0; i < runs.size(); ++i) {
            if (runs[i].dbId > 0) byId.emplace(runs[i].dbId, i);
            if (runs[i].archived) archived.insert(runs[i].sessionId);
        }
        sessions.assign(archived.begin(), archived.end()); hasArchived = !sessions.empty();
        widestSessionLabel = sessions.empty() ? "" : "Archived #" + std::to_string(sessions.front());
    }
    void Select(size_t index, const SettingsDeps& d) {
        selected = {d.tracker->Runs()[index].dbId, index, structure}; detailGeneration = 0;
    }
    void SortRows(const SettingsDeps& d) {
        Stats::SortRows(snapshot.rows, d.tracker->Runs(), currency, sort, ascending);
        if (sort == Stats::Sort::Map) {
            const auto& runs = d.tracker->Runs();
            std::stable_sort(snapshot.rows.begin(), snapshot.rows.end(), [&](size_t a, size_t b) {
                return ascending ? MapName(d, runs[a]) < MapName(d, runs[b]) : MapName(d, runs[a]) > MapName(d, runs[b]);
            });
        }
    }
    void Refresh(const SettingsDeps& d) {
        const uint64_t h = d.tracker->HistoryRevision();
        const uint64_t z = d.zones ? d.zones->Revision() : 0;
        const int active = d.tracker->ActiveRunIndex();
        const int64_t nowMinute = period ? static_cast<int64_t>(std::time(nullptr)) / 60 : 0;
        Stats::Currency next{currencyKind, d.prices ? d.prices->ExaltedInChaos() : 1.0,
            d.prices ? d.prices->DivineInChaos() : 1.0};
        // A failed placeholder discard can clear the active index while the
        // history revision stays unchanged. Completed averages depend on both.
        if (!dirty && history == h && zonesRevision == z && minute == nowMinute && activeIndex == active &&
            currency.kind == next.kind && currency.exalted == next.exalted && currency.divine == next.divine) return;
        history = h; zonesRevision = z; minute = nowMinute; currency = next; activeIndex = active; dirty = false;
        Stats::Filter filter;
        filter.currentSession = sessionMode == 1;
        filter.archivedSession = sessionMode == 2 ? archivedSession : -1;
        filter.since = period ? nowMinute * 60 - (period == 1 ? 7LL : 30LL) * 86400 : 0;
        filter.category = category;
        snapshot = Stats::Build(d.tracker->Runs(), active, filter, currency,
            [&](const std::string& raw) { return d.zones ? d.zones->CategoryKey(raw) : raw; });
        SortRows(d); ++generation; categoryDirty = true; allCount = 0;
        for (const auto& item : snapshot.categories) allCount += item.summary.runs;
        if (selected.id > 0) {
            const auto found = byId.find(selected.id);
            selected.index = found == byId.end() ? Stats::NoRun : found->second;
        } else if (selected.structure != structure) selected.index = Stats::NoRun;
        if (selected.index >= d.tracker->Runs().size() ||
            std::find(snapshot.rows.begin(), snapshot.rows.end(), selected.index) == snapshot.rows.end()) {
            selected = {};
            if (!snapshot.rows.empty()) Select(snapshot.rows.front(), d);
        }
    }
    void Controls(const SettingsDeps& d, bool& saveCurrency) {
        currencyKind = std::clamp(d.settings ? d.settings->overlayCurrency : currencyKind, 0, 2);
        const std::string preview = sessionMode == 0 ? "All sessions" : sessionMode == 1 ? "Current session" : "Archived #" + std::to_string(archivedSession);
        const float sessionWidth = ComboWidth(155, {"All sessions", "Current session", preview.c_str(), widestSessionLabel.c_str()});
        const float periodWidth = ComboWidth(108, {"All time", "Last 7 days", "Last 30 days"});
        const float currencyWidth = ComboWidth(104, {"Exalted", "Divine", "Chaos"});
        const float newWidth = (std::max)(120.f, std::ceil(ImGui::CalcTextSize("New session").x + 2 * ImGui::GetStyle().FramePadding.x) + 1);
        // EndGroup/SameLine carries the last framed widget's text baseline.
        // Give every label the same frame-height band, including the first one.
        ImGui::BeginGroup(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Session"); ImGui::SetNextItemWidth(sessionWidth);
        if (ImGui::BeginCombo("##StatsSession", preview.c_str())) {
            if (ImGui::Selectable("All sessions", sessionMode == 0)) { sessionMode = 0; dirty = true; }
            if (ImGui::Selectable("Current session", sessionMode == 1)) { sessionMode = 1; dirty = true; }
            for (int id : sessions) {
                const auto label = "Archived #" + std::to_string(id);
                if (ImGui::Selectable(label.c_str(), sessionMode == 2 && archivedSession == id)) {
                    sessionMode = 2; archivedSession = id; dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndGroup(); NextControl(periodWidth);
        ImGui::BeginGroup(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Period"); ImGui::SetNextItemWidth(periodWidth);
        if (ImGui::Combo("##StatsPeriod", &period, "All time\0Last 7 days\0Last 30 days\0")) dirty = true;
        ImGui::EndGroup(); NextControl(currencyWidth);
        ImGui::BeginGroup(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Currency"); ImGui::SetNextItemWidth(currencyWidth);
        if (ImGui::Combo("##StatsCurrency", &currencyKind, "Exalted\0Divine\0Chaos\0")) { dirty = true; saveCurrency = true; }
        CurrencyTip(currencyKind); ImGui::EndGroup(); const bool beside = NextControl(newWidth);
        ImGui::BeginGroup(); if (beside) ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
        if (ImGui::Button("New session##StatsNewSession", ImVec2(newWidth, 0))) apply.action = Action::NewSession;
        Tip("Archive the current session. If a map is open, a fresh run starts immediately in that map.");
        ImGui::EndGroup();
        if (!d.tracker->LastPersistenceError().empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, FcTheme::kLoss);
            ImGui::TextWrapped("Not saved: %s", d.tracker->LastPersistenceError().c_str()); ImGui::PopStyleColor();
        } else if (!d.tracker->DbOpen()) ImGui::TextColored(FcTheme::kLoss, "History storage is unavailable.");
        if (!error.empty()) { ImGui::TextWrapped("%s", error.c_str()); if (ImGui::SmallButton("Dismiss##StatsDismissError")) error.clear(); }
        if (pending.action != Action::None) {
            ImGui::TextColored(FcTheme::kLoss, "%s", pending.description.c_str());
            if (ImGui::Button("Confirm##StatsConfirm")) { apply = pending; pending = {}; }
            ImGui::SameLine(); if (ImGui::Button("Cancel##StatsCancel")) pending = {};
        }
        ImGui::Separator();
    }
    std::string CategoryName(const SettingsDeps& d, const Stats::Category& item) const {
        return d.zones ? d.zones->CategoryDisplay(item.key) : item.key;
    }
    void CategoryRow(const std::string& label, const std::string& footer, bool isSelected) {
        ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_None, ImVec2(0, CategoryHeight()));
        const auto position = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize() * .85f,
            ImVec2(position.x, position.y + ImGui::GetTextLineHeight() + 3),
            ImGui::ColorConvertFloat4ToU32(FcTheme::kDim), footer.c_str());
    }
    void Sidebar(const SettingsDeps& d) {
        ImGui::TextColored(FcTheme::kAccent, "MAP TYPES");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##StatsSearch", "Search map types", search.data(), search.size())) categoryDirty = true;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##StatsCategoryOrder", &categoryOrder, "Name\0Avg/run\0Profit/h\0Runs\0")) categoryDirty = true;
        Tip("Sort map types by name, average profit per completed run, profit per hour, or run count.");
        const float actionWidth = ImGui::GetContentRegionAvail().x;
        const float textWidth = (std::max)(1.f, actionWidth - 2 * ImGui::GetStyle().FramePadding.x);
        const std::string deleteAll = WrapControlText("Delete all archived", textWidth);
        const std::string deleteSession = WrapControlText("Delete session", textWidth);
        float footerHeight = ImGui::CalcTextSize("Manage history", nullptr, false, actionWidth).y +
            ImGui::CalcTextSize(deleteAll.c_str()).y + 2 * ImGui::GetStyle().FramePadding.y +
            5 * ImGui::GetStyle().ItemSpacing.y + 8;
        if (sessionMode == 2) footerHeight += ImGui::CalcTextSize(deleteSession.c_str()).y +
            2 * ImGui::GetStyle().FramePadding.y + ImGui::GetStyle().ItemSpacing.y;
        const float listHeight = (std::max)(80.f, ImGui::GetContentRegionAvail().y - footerHeight);
        if (ImGui::BeginChild("##StatsCategories", ImVec2(0, listHeight), ImGuiChildFlags_AlwaysUseWindowPadding)) {
            CategoryRow("All map types##StatsCategory:all", std::to_string(allCount) + " recorded runs", category.empty());
            if (ImGui::IsItemClicked()) { category.clear(); dirty = true; Refresh(d); }
            if (categoryDirty || lastSearch != search.data()) {
                categoryDirty = false; lastSearch = search.data(); categoryRows.clear(); const auto query = Fold(lastSearch);
                for (size_t i = 0; i < snapshot.categories.size(); ++i) {
                    const auto& item = snapshot.categories[i];
                    if (query.empty() || Fold(CategoryName(d, item) + " " + item.key).find(query) != std::string::npos) categoryRows.push_back(i);
                }
                std::sort(categoryRows.begin(), categoryRows.end(), [&](size_t a, size_t b) {
                    const auto& left = snapshot.categories[a]; const auto& right = snapshot.categories[b];
                    if (categoryOrder) {
                        const auto score = [&](const Stats::Category& c) { return categoryOrder == 1 ? c.summary.AverageValue() :
                            categoryOrder == 2 ? c.summary.PerHour() : static_cast<double>(c.summary.runs); };
                        if (score(left) != score(right)) return score(left) > score(right);
                    }
                    const auto ln = CategoryName(d, left), rn = CategoryName(d, right);
                    return ln != rn ? ln < rn : left.key < right.key;
                });
            }
            std::string clicked;
            ImGuiListClipper clipper; clipper.Begin(static_cast<int>(categoryRows.size()), CategoryHeight() + ImGui::GetStyle().ItemSpacing.y);
            while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto& item = snapshot.categories[categoryRows[static_cast<size_t>(row)]];
                const std::string footer = std::to_string(item.summary.runs) + " runs  |  " + Money(item.summary.AverageValue(), currency.kind) + " / run";
                CategoryRow(CategoryName(d, item) + "##StatsCategory:" + item.key, footer, category == item.key);
                if (ImGui::IsItemClicked()) clicked = item.key;
                Tip("Compare average profit per completed run. Select a map type to inspect its full history.");
            }
            if (categoryRows.empty()) ImGui::TextDisabled("No matching map types.");
            if (!clicked.empty()) { category = std::move(clicked); dirty = true; Refresh(d); }
        }
        ImGui::EndChild();
        ImGui::Separator(); ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Manage history"); ImGui::PopStyleColor();
        if (sessionMode == 2) {
            if (ImGui::Button((deleteSession + "###StatsDeleteSession").c_str(), ImVec2(actionWidth, 0)))
                pending = {Action::DeleteSession, {}, archivedSession, "Delete archived session #" + std::to_string(archivedSession) + " and its runs?"};
            Tip("Delete the selected archived session and its recorded runs.");
        }
        ImGui::BeginDisabled(!hasArchived);
        if (ImGui::Button((deleteAll + "###StatsDeleteArchived").c_str(), ImVec2(actionWidth, 0)))
            pending = {Action::DeleteArchived, {}, -1, "Delete all archived runs and their loot?"};
        ImGui::EndDisabled();
    }
    void Overview(const SettingsDeps& d) {
        const auto title = category.empty() ? std::string("All map types") : d.zones ? d.zones->CategoryDisplay(category) : category;
        ImGui::TextWrapped("%s", title.c_str());
        const auto& sum = snapshot.summary;
        const char* labels[] = {"Runs", "Profit", "Avg / run", "Profit / h"};
        const std::array<std::string, 4> values = {std::to_string(sum.runs), Money(sum.value, currency.kind),
            sum.completedRuns ? Money(sum.AverageValue(), currency.kind) : "--", Money(sum.PerHour(), currency.kind)};
        const std::array<std::string, 4> footers = {std::to_string(sum.completedRuns) + "/" +
            std::to_string(sum.runs - sum.completedRuns) + " live", "selected", "done", "map"};
        const ImVec4 colors[] = {FcTheme::kAccent, ValueColor(sum.value), ValueColor(sum.AverageValue()), ValueColor(sum.PerHour())};
        const char* tips[] = {
            "Every recorded run in this selection, including the current unfinished run.",
            "Total recorded profit in the selected sessions, period and map type.",
            "Average over completed runs. The current unfinished run is excluded.",
            "Total recorded value divided by total recorded map time, including the current run."};
        const float available = ImGui::GetContentRegionAvail().x, spacing = ImGui::GetStyle().ItemSpacing.x;
        float minimumWidth = 170;
        for (size_t i = 0; i < values.size(); ++i)
            minimumWidth = (std::max)(minimumWidth, ImGui::CalcTextSize(labels[i]).x +
                ImGui::CalcTextSize(values[i].c_str()).x + 36);
        int columns = available >= 700 ? 4 : 2;
        while (columns > 1 && (available - (columns - 1) * spacing) / columns < minimumWidth) columns /= 2;
        const float width = (available - (columns - 1) * spacing) / columns;
        float height = (std::max)(34.f, ImGui::GetTextLineHeight() + 2 * ImGui::GetStyle().WindowPadding.y + 2);
        for (size_t i = 0; i < values.size(); ++i) {
            if (i % static_cast<size_t>(columns)) ImGui::SameLine();
            Card(labels[i], values[i], footers[i], colors[i], width, height, tips[i]);
        }
        const std::string timeText = "Time " + FcFormat::Duration(static_cast<double>(sum.seconds));
        const std::string goldText = "Gold " + FcFormat::Count(static_cast<double>(sum.gold));
        const std::string hiveText = "Hiveblood " + FcFormat::Count(static_cast<double>(sum.hiveblood));
        const std::string beaconText = "Beacons " + FcFormat::Count(static_cast<double>(sum.beacons));
        const std::string bestText = "Best " + (sum.completedRuns ? Money(sum.bestValue, currency.kind) : "--");
        const std::string worstText = "Worst " + (sum.completedRuns ? Money(sum.worstValue, currency.kind) : "--");
        const float iconSize = (std::max)(12.f, ImGui::GetTextLineHeight() * .9f);
        const float killWidth = 5.f * (iconSize + ImGui::CalcTextSize("999K").x + 8.f);
        const float trendWidth = ImGui::GetFrameHeight() + ImGui::CalcTextSize("Recent runs").x + ImGui::GetStyle().ItemSpacing.x;
        const float metricsWidth = ImGui::CalcTextSize(timeText.c_str()).x + ImGui::CalcTextSize(goldText.c_str()).x +
            ImGui::CalcTextSize(hiveText.c_str()).x + ImGui::CalcTextSize(beaconText.c_str()).x +
            ImGui::CalcTextSize(bestText.c_str()).x + ImGui::CalcTextSize(worstText.c_str()).x + killWidth +
            ImGui::GetStyle().ItemSpacing.x * 14 + ImGui::CalcTextSize("Kills").x + trendWidth;
        const bool metricsVisible = BeginMetricLine("##StatsOverviewMetrics", metricsWidth, true);
        if (metricsVisible) {
            auto separator = [] {
                ImGui::SameLine(0, 6); ImGui::TextDisabled("%s", "|"); ImGui::SameLine(0, 6);
            };
            ImGui::TextUnformatted(timeText.c_str()); separator();
            ImGui::TextDisabled("Kills"); ImGui::SameLine(0, 4); DrawKillIcons(d, sum.kills); separator();
            ImGui::TextUnformatted(goldText.c_str()); separator(); ImGui::TextUnformatted(hiveText.c_str()); separator();
            ImGui::TextUnformatted(beaconText.c_str()); separator(); ImGui::TextUnformatted(bestText.c_str());
            separator(); ImGui::TextUnformatted(worstText.c_str()); ImGui::SameLine(0, 12);
            ImGui::Checkbox("Recent runs##StatsTrend", &showTrend);
        }
        EndMetricLine(metricsVisible); Tip("Best and worst are completed runs only.");
        if (sum.invalidValues) {
            ImGui::PushStyleColor(ImGuiCol_Text, FcTheme::kLoss);
            ImGui::TextWrapped("%zu invalid historical profit values were omitted.", sum.invalidValues); ImGui::PopStyleColor();
        }
    }
    void Trend(const SettingsDeps& d) {
        if (!showTrend) return;
        const auto& runs = d.tracker->Runs();
        if (trendGeneration != generation) {
            trendGeneration = generation; recent = snapshot.rows;
            const auto active = d.tracker->ActiveRunIndex();
            recent.erase(std::remove_if(recent.begin(), recent.end(), [&](size_t index) { return active >= 0 && index == static_cast<size_t>(active); }), recent.end());
            Stats::SortRows(recent, runs, currency, Stats::Sort::Date, true);
            if (recent.size() > 24) recent.erase(recent.begin(), recent.end() - 24);
        }
        if (recent.empty()) { ImGui::TextDisabled("No completed runs to chart."); return; }
        ImGui::TextDisabled("Latest %zu completed runs - oldest to newest", recent.size());
        ImGui::TextDisabled("%s - %s", Date(runs[recent.front()]).substr(0, 10).c_str(), Date(runs[recent.back()]).substr(0, 10).c_str());
        ImGui::InvisibleButton("##StatsTrendBars", ImVec2(ImGui::GetContentRegionAvail().x, 48));
        const auto origin = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
        double low = 0, high = 0;
        for (size_t index : recent) { const double v = Stats::RunValue(runs[index], currency); low = (std::min)(low, v); high = (std::max)(high, v); }
        const double range = high > low ? high - low : 1;
        const auto y = [&](double v) { return end.y - 3 - static_cast<float>((v - low) / range) * (end.y - origin.y - 6); };
        const float barWidth = (end.x - origin.x) / static_cast<float>(recent.size());
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddLine(ImVec2(origin.x, y(0)), ImVec2(end.x, y(0)), ImGui::GetColorU32(ImGuiCol_Border));
        for (size_t i = 0; i < recent.size(); ++i) {
            const double v = Stats::RunValue(runs[recent[i]], currency); const float top = (std::min)(y(0), y(v));
            draw->AddRectFilled(ImVec2(origin.x + static_cast<float>(i) * barWidth + 1, top),
                ImVec2(origin.x + static_cast<float>(i + 1) * barWidth - 1, (std::max)(top + 1, (std::max)(y(0), y(v)))), ImGui::ColorConvertFloat4ToU32(ValueColor(v)), 2);
        }
        if (ImGui::IsItemHovered()) {
            const size_t offset = (std::min)(recent.size() - 1, static_cast<size_t>((ImGui::GetIO().MousePos.x - origin.x) / barWidth));
            const auto& run = runs[recent[offset]];
            ImGui::BeginTooltip(); ImGui::Text("%s - %s", Date(run).c_str(), MapName(d, run).c_str());
            ImGui::TextUnformatted(Money(Stats::RunValue(run, currency), currency.kind).c_str()); ImGui::EndTooltip();
            if (ImGui::IsMouseClicked(0)) Select(recent[offset], d);
        }
    }
    void RunTable(const SettingsDeps& d) {
        if (snapshot.rows.empty()) { ImGui::TextDisabled("No runs match these filters."); return; }
        const auto& runs = d.tracker->Runs();
        const float scale = ImGui::GetFontSize() / 13.f;
        const float dateWidth = (std::max)(132.f * scale, ImGui::CalcTextSize("0000-00-00 00:00").x);
        const float moneyWidth = (std::max)(90.f * scale, ImGui::CalcTextSize(Money(-9999999.99, currency.kind).c_str()).x);
        const float timeWidth = (std::max)(60.f * scale, ImGui::CalcTextSize(FcFormat::Duration((std::numeric_limits<int>::max)(), true).c_str()).x);
        const float killsWidth = (std::max)(72.f * scale,
            ImGui::CalcTextSize("999K/999K/999K/999K").x + 2 * ImGui::GetStyle().CellPadding.x);
        const float goldWidth = 62.f * scale;
        const float innerWidth = TableInnerWidth(dateWidth + moneyWidth * 2 + timeWidth +
            killsWidth + goldWidth + 90.f * scale + 14 * ImGui::GetStyle().CellPadding.x);
        const auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_Hideable | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##StatsRuns", 7, flags, ImVec2(0, runTableHeight), innerWidth)) {
            ImGui::TableSetupColumn("Started", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, dateWidth, static_cast<ImGuiID>(Stats::Sort::Date));
            ImGui::TableSetupColumn("Map variant", ImGuiTableColumnFlags_WidthStretch, 1, static_cast<ImGuiID>(Stats::Sort::Map));
            ImGui::TableSetupColumn("Profit", ImGuiTableColumnFlags_WidthFixed, moneyWidth, static_cast<ImGuiID>(Stats::Sort::Profit));
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, timeWidth, static_cast<ImGuiID>(Stats::Sort::Duration));
            ImGui::TableSetupColumn("Profit / h", ImGuiTableColumnFlags_WidthFixed, moneyWidth, static_cast<ImGuiID>(Stats::Sort::Rate));
            ImGui::TableSetupColumn("Kills N/M/R/U", ImGuiTableColumnFlags_WidthFixed, killsWidth, static_cast<ImGuiID>(Stats::Sort::Kills));
            ImGui::TableSetupColumn("Gold", ImGuiTableColumnFlags_WidthFixed, goldWidth, static_cast<ImGuiID>(Stats::Sort::Gold));
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            if (auto* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty && specs->SpecsCount) {
                sort = static_cast<Stats::Sort>(specs->Specs[0].ColumnUserID); ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                SortRows(d); specs->SpecsDirty = false;
            }
            ImGuiListClipper clipper; clipper.Begin(static_cast<int>(snapshot.rows.size()));
            while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const size_t index = snapshot.rows[static_cast<size_t>(row)]; if (index >= runs.size()) continue;
                const auto& run = runs[index]; ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                const auto label = Date(run) + "##StatsRun:" + std::to_string(run.dbId);
                if (ImGui::Selectable(label.c_str(), selected.index == index, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) Select(index, d);
                Tip(Date(run).c_str());
                ImGui::TableNextColumn();
                const bool live = d.tracker->ActiveRunIndex() >= 0 && index == static_cast<size_t>(d.tracker->ActiveRunIndex());
                if (live) { ImGui::TextColored(FcTheme::kXp, "%s", Status(d, index)); ImGui::SameLine(); }
                ImGui::TextUnformatted(MapName(d, run).c_str()); Tip(run.mapName.c_str());
                ImGui::TableNextColumn(); ImGui::TextColored(ValueColor(Stats::RunValue(run, currency)), "%s", Money(Stats::RunValue(run, currency), currency.kind).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FcFormat::Duration(run.durationSec, true).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Money(Stats::RunRate(run, currency), currency.kind).c_str());
                ImGui::TableNextColumn();
                const auto killCell = KillCell(run);
                ImGui::TextUnformatted(killCell.c_str()); Tip(KillBreakdown(run.KillsByRarity()).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FcFormat::Count(run.goldGain).c_str()); ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ResizeDown("##StatsRunsResize", runTableHeight, 120.f);
        ImGui::TextDisabled("%zu runs - click a row for details", snapshot.rows.size());
    }
    void LootTable(const SettingsDeps& d, const char* id, const std::vector<Stats::LootTotal>& items, float& height) {
        if (items.empty()) { ImGui::TextDisabled("No recorded loot in this selection."); return; }
        const float scale = ImGui::GetFontSize() / 13.f;
        const float quantityWidth = (std::max)(72.f * scale, ImGui::CalcTextSize("-9223372036854775808").x);
        const float valueWidth = (std::max)(112.f * scale, ImGui::CalcTextSize(Money(-9999999.99, currency.kind).c_str()).x);
        const float innerWidth = TableInnerWidth(180.f * scale + quantityWidth + valueWidth + 6 * ImGui::GetStyle().CellPadding.x);
        if (ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollX | ImGuiTableFlags_PadOuterX, ImVec2(0, height), innerWidth)) {
            ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Quantity", ImGuiTableColumnFlags_WidthFixed, quantityWidth);
            ImGui::TableSetupColumn("Recorded value", ImGuiTableColumnFlags_WidthFixed, valueWidth);
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            const float rowHeight = (std::max)(16.f, ImGui::GetTextLineHeight()) + 2 * ImGui::GetStyle().CellPadding.y;
            ImGuiListClipper clipper; clipper.Begin(static_cast<int>(items.size()), rowHeight);
            while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& item = items[static_cast<size_t>(i)]; ImGui::TableNextRow(0, rowHeight); ImGui::TableNextColumn();
                if (d.icons && !item.iconPath.empty()) {
                    const auto image = d.icons->Item(item.iconPath);
                    if (image.valid) { ImGui::Image(image.srv, ImVec2(16, 16)); ImGui::SameLine(); }
                }
                ImGui::TextUnformatted(item.name.c_str()); Tip(item.name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%lld", static_cast<long long>(item.stack));
                ImGui::TableNextColumn(); ImGui::TextColored(ValueColor(item.value), "%s", Money(item.value, currency.kind).c_str());
            }
            ImGui::EndTable();
        }
        const std::string resizeId = std::string(id) + "Resize";
        ResizeDown(resizeId.c_str(), height, 100.f);
    }
    void Details(const SettingsDeps& d) {
        const auto& runs = d.tracker->Runs(); if (selected.index >= runs.size()) return;
        const auto& run = runs[selected.index];
        ImGui::SeparatorText("Run details");
        const auto& name = MapName(d, run);
        const std::string profit = Money(Stats::RunValue(run, currency), currency.kind);
        const std::string duration = "in " + FcFormat::Duration(run.durationSec, true);
        const std::string endDate = EndDate(run) + (run.archived ? "  #" + std::to_string(run.sessionId) : "");
        const char* status = Status(d, selected.index);
        const bool active = d.tracker->ActiveRunIndex() >= 0 && selected.index == static_cast<size_t>(d.tracker->ActiveRunIndex());
        const float iconSize = (std::max)(12.f, ImGui::GetTextLineHeight() * .9f);
        const float killWidth = 5.f * (iconSize + ImGui::CalcTextSize("999K").x + 8.f);
        const float deleteWidth = ImGui::CalcTextSize("Delete run").x + 2 * ImGui::GetStyle().FramePadding.x;
        const float summaryWidth = ImGui::CalcTextSize(name.c_str()).x + ImGui::CalcTextSize(profit.c_str()).x +
            ImGui::CalcTextSize(duration.c_str()).x + killWidth + ImGui::CalcTextSize("Gold 9999999 | Hiveblood 999999 | Beacons 999").x +
            ImGui::CalcTextSize(endDate.c_str()).x + deleteWidth + ImGui::GetStyle().ItemSpacing.x * 14 + 80.f;
        const bool summaryVisible = BeginMetricLine("##StatsRunSummary", summaryWidth);
        if (summaryVisible) {
            auto separator = [] {
                ImGui::SameLine(0, 6); ImGui::TextDisabled("%s", "|"); ImGui::SameLine(0, 6);
            };
            ImGui::TextUnformatted(name.c_str()); separator();
            ImGui::TextColored(ValueColor(Stats::RunValue(run, currency)), "%s", profit.c_str()); separator();
            ImGui::TextDisabled("%s", duration.c_str()); separator();
            DrawKillIcons(d, run); separator();
            ImGui::TextDisabled("Gold %s | Hiveblood %d | Beacons %d",
                FcFormat::Count(run.goldGain).c_str(), run.hivebloodGain, run.beaconGain); separator();
            ImGui::TextColored(StatusColor(status), "%s", StatusGlyph(status)); Tip(status);
            ImGui::SameLine(0, 4); ImGui::TextDisabled("%s", endDate.c_str()); separator();
            ImGui::BeginDisabled(active);
            if (ImGui::SmallButton("Delete run##StatsDeleteRun"))
                pending = {Action::DeleteRun, selected, -1, "Delete the selected run and its recorded loot?"};
            ImGui::EndDisabled();
        }
        EndMetricLine(summaryVisible);
        if (active) Tip("The current unfinished run cannot be deleted.");
        if (ImGui::BeginTabBar("##StatsDetails")) {
            if (ImGui::BeginTabItem("Loot##StatsDetailLoot")) {
                if (detailGeneration != generation) { detailLoot = Stats::AggregateLoot(runs, {selected.index}, currency); detailGeneration = generation; }
                LootTable(d, "##StatsDetailLootTable", detailLoot, detailLootHeight); ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Map modifiers##StatsDetailMods")) {
                if (ImGui::BeginChild("##StatsModifiers", ImVec2(0, detailModsHeight), ImGuiChildFlags_AlwaysUseWindowPadding)) {
                    if (run.mapMods.empty()) ImGui::TextDisabled("No modifiers were recorded for this run.");
                    for (const auto& modifier : run.mapMods) {
                        ImGui::PushStyleColor(ImGuiCol_Text, FcTheme::kMod); ImGui::TextWrapped("%s", modifier.c_str()); ImGui::PopStyleColor();
                    }
                }
                ImGui::EndChild(); ResizeDown("##StatsModifiersResize", detailModsHeight, 80.f); ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    void Content(const SettingsDeps& d) {
        Overview(d);
        Tip("Optional chart of the latest 24 completed runs in this selection. The table retains the full history.");
        Trend(d);
        if (ImGui::BeginTabBar("##StatsViews")) {
            if (ImGui::BeginTabItem("Map runs##StatsRunsTab")) { RunTable(d); Details(d); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Loot totals##StatsLootTab")) {
                if (lootGeneration != generation) { lootTotals = Stats::AggregateLoot(d.tracker->Runs(), snapshot.rows, currency); lootGeneration = generation; }
                ImGui::TextWrapped("Recorded loot across all %zu selected runs. Historical item prices are preserved.", snapshot.rows.size());
                LootTable(d, "##StatsLootTotals", lootTotals, lootTotalsHeight);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    void Apply(const SettingsDeps& d) {
        if (apply.action == Action::None) return;
        bool success = false;
        switch (apply.action) {
        case Action::NewSession:
            success = d.tracker->NewSession();
            if (success) { sessionMode = 1; category.clear(); selected = {}; }
            break;
        case Action::DeleteRun: {
            size_t index = Stats::NoRun; const auto& runs = d.tracker->Runs();
            if (apply.run.id > 0) {
                for (size_t i = 0; i < runs.size(); ++i) if (runs[i].dbId == apply.run.id) { index = i; break; }
            } else if (apply.run.structure == d.tracker->HistoryStructureRevision()) index = apply.run.index;
            if (index < runs.size()) success = d.tracker->DeleteRunAt(static_cast<int>(index));
            else error = "History changed. Select the run again before deleting it.";
            break;
        }
        case Action::DeleteSession:
            success = d.tracker->DeleteArchivedSession(apply.session);
            if (success && sessionMode == 2 && archivedSession == apply.session) sessionMode = 0;
            break;
        case Action::DeleteArchived:
            success = d.tracker->DeleteAllArchived();
            if (success && sessionMode == 2) sessionMode = 0;
            break;
        default: break;
        }
        if (success) { dirty = true; error.clear(); pending = {}; }
        else if (error.empty() && d.tracker->LastPersistenceError().empty()) error = "The history change could not be completed.";
        apply = {};
    }
};
StatisticsView::StatisticsView() : m_state(std::make_unique<State>()) {}
StatisticsView::~StatisticsView() = default;
void StatisticsView::Draw(const SettingsDeps& d) {
    if (!d.tracker) { ImGui::TextDisabled("Statistics are unavailable."); return; }
    auto& state = *m_state; bool saveCurrency = false;
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(origin.x + kViewInset, origin.y + kViewTopInset));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8, 5));
    ImGui::PushID(this); state.SyncStructure(d); state.Controls(d, saveCurrency); state.Refresh(d);
    const float available = ImGui::GetContentRegionAvail().x - kViewInset;
    const float sidebar = std::clamp(available * .23f, 170.f, 240.f);
    const float bodyHeight = (std::max)(230.f, ImGui::GetContentRegionAvail().y - kViewTopInset);
    if (ImGui::BeginChild("##StatsSidebar", ImVec2(sidebar, bodyHeight), ImGuiChildFlags_Borders)) state.Sidebar(d);
    ImGui::EndChild(); ImGui::SameLine();
    if (ImGui::BeginChild("##StatsContent", ImVec2((std::max)(1.f, available - sidebar - ImGui::GetStyle().ItemSpacing.x), bodyHeight),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_AlwaysVerticalScrollbar)) state.Content(d);
    ImGui::EndChild(); ImGui::PopID();
    ImGui::PopStyleVar(2); ImGui::EndGroup();
    // No history pointers survive drawing. Confirmation resolves the stable ID
    // against the latest tracker vector before any erase/archive/reset occurs.
    if (saveCurrency && d.settings) { d.settings->overlayCurrency = state.currencyKind; SaveSettings(d.dir, *d.settings); }
    state.Apply(d);
}

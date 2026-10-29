#include "ZonesView.h"
#include "Settings.h"
#include "Text.h"
#include "../ZoneCatalog.h"
#include "../ZoneClassify.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace {
using FcText::I18n::Key;
using FcText::I18n::Tr;
using FcText::I18n::TrF;
using FcText::I18n::TrId;

int ResizeString(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto& value = *static_cast<std::string*>(data->UserData);
        value.resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = value.data();
    }
    return 0;
}

bool TextInput(const char* label, const char* hint, std::string& value,
               ImGuiInputTextFlags flags = 0) {
    return ImGui::InputTextWithHint(label, hint, value.data(), value.capacity() + 1,
        flags | ImGuiInputTextFlags_CallbackResize, ResizeString, &value);
}

std::string FoldAscii(std::string value) {
    for (char& ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 128) ch = static_cast<char>(std::tolower(byte));
    }
    return value;
}

std::string TrimId(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}
} // namespace

void ZonesView::Select(const std::string& raw, const ZoneNames& names) {
    m_selected = raw;
    m_editText = names.Override(raw);
    m_focusEditor = true;
    m_status.clear();
    m_error = false;
}

void ZonesView::Rebuild(const ZoneNames& names) {
    m_rows.clear();
    const auto search = FoldAscii(m_search);
    std::unordered_set<std::string> added;
    auto add = [&](const std::string& raw, const ZoneCatalog::Entry* entry) {
        if (!added.insert(raw).second) return;
        const bool registered = names.Map().contains(raw);
        if (entry && entry->isUnused && !m_includeUnused && !registered) return;
        if (!m_allAreas && !registered && !(entry && entry->isMapArea) && !ClassifyZone(raw).isMap) return;
        // An area the catalog does not know has no game name (drawn as "Unknown area").
        Row row{raw, entry ? entry->name : "", names.Override(raw), entry && entry->isUnused};
        if (!search.empty() && FoldAscii(raw).find(search) == std::string::npos &&
            FoldAscii(row.official).find(search) == std::string::npos &&
            FoldAscii(row.custom).find(search) == std::string::npos) return;
        m_rows.push_back(std::move(row));
    };
    for (const auto& entry : ZoneCatalog::Entries()) add(entry.id, &entry);
    for (const auto& [raw, display] : names.Map()) {
        (void)display;
        add(raw, ZoneCatalog::Lookup(raw));
    }
    std::sort(m_rows.begin(), m_rows.end(), [](const Row& a, const Row& b) {
        if (a.official.empty() != b.official.empty()) return !a.official.empty();   // unknown areas last
        if (a.official != b.official) return a.official < b.official;
        return a.id < b.id;
    });
    m_revision = names.Revision();
    m_rebuild = false;
}

void ZonesView::Draw(const SettingsDeps& deps) {
    if (!deps.zones) return;
    auto& names = *deps.zones;
    if (m_source != &names || m_directory != deps.dir) {
        m_source = &names;
        m_directory = deps.dir;
        m_selected.clear();
        m_editText.clear();
        m_status.clear();
        m_rebuild = true;
    }
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", Tr("zones_intro"));
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(-1.f);
    if (TextInput("##ZoneSearch", Tr("zones_search_hint"), m_search)) m_rebuild = true;
    if (ImGui::Checkbox(TrId("zones_all_areas"), &m_allAreas)) m_rebuild = true;
    ImGui::SameLine();
    if (ImGui::Checkbox(TrId("zones_include_unused"), &m_includeUnused)) m_rebuild = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", Tr("zones_default_note"));

    ImGui::SeparatorText(Tr("zones_section_add"));
    // The field leaves room for its button, whatever the button's translated text.
    const float addButtonWidth = ImGui::CalcTextSize(Tr("zones_add")).x + 2 * ImGui::GetStyle().FramePadding.x +
        ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth((std::max)(ImGui::GetFontSize() * 12.f, ImGui::GetContentRegionAvail().x - addButtonWidth));
    const bool addEntered = TextInput("##ZoneAddId", Tr("zones_add_hint"), m_addRaw,
                                      ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool addClicked = ImGui::Button(TrId("zones_add"));
    if (addEntered || addClicked) {
        const std::string raw = TrimId(m_addRaw);
        if (raw.empty()) {
            m_status = Key("zones_status_enter_id");
            m_error = true;
        } else if (names.Map().contains(raw) || names.SetOverride(deps.dir, raw, "")) {
            Select(raw, names);
            m_addRaw.clear();
        } else {
            m_status = Key("zones_status_add_failed");
            m_error = true;
        }
    }

    ImGui::SeparatorText(Tr("zones_section_display"));
    if (m_selected.empty()) {
        ImGui::TextDisabled("%s", Tr("zones_choose_edit"));
    } else {
        const auto* entry = ZoneCatalog::Lookup(m_selected);
        ImGui::TextWrapped("%s", entry ? entry->name : Tr("zones_unknown_area"));
        ImGui::TextDisabled("%s", m_selected.c_str());
        if (entry && entry->isUnused) ImGui::TextDisabled("%s", Tr("zones_marked_unused"));
        if (m_focusEditor) { ImGui::SetKeyboardFocusHere(); m_focusEditor = false; }
        ImGui::SetNextItemWidth(-1.f);
        const bool entered = TextInput("##ZoneDisplayName", names.OfficialName(m_selected).c_str(),
            m_editText, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool saveClicked = ImGui::Button(TrId("zones_save"));
        ImGui::SameLine();
        const bool resetClicked = ImGui::Button(TrId("zones_reset"));
        ImGui::SameLine();
        if (ImGui::Button(TrId("zones_cancel"))) {
            m_selected.clear();
            m_editText.clear();
            m_status.clear();
        } else if (entered || saveClicked || resetClicked) {
            const bool saved = resetClicked ? names.ResetOverride(deps.dir, m_selected)
                                           : names.SetOverride(deps.dir, m_selected, m_editText);
            m_error = !saved;
            m_status = saved ? Key("zones_status_saved") : Key("zones_status_save_failed");
            if (saved && resetClicked) m_editText.clear();
        }
    }
    if (!m_status.empty()) {
        if (m_error) ImGui::TextColored(ImVec4(1.f, .45f, .35f, 1.f), "%s", Tr(m_status.c_str()));
        else ImGui::TextDisabled("%s", Tr(m_status.c_str()));
    }

    if (m_rebuild || m_revision != names.Revision()) Rebuild(names);
    ImGui::SeparatorText(Tr("zones_section_areas"));
    ImGui::TextDisabled("%s", TrF("zones_count", { { "count", std::to_string(m_rows.size()) } }).c_str());
    const float height = (std::max)(100.f, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginTable("##ZoneCatalogTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0.f, height))) {
        ImGui::TableSetupColumn(TrId("zones_col_game_name"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn(TrId("zones_col_area_id"), ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn(TrId("zones_col_custom"), ImGuiTableColumnFlags_WidthStretch, .85f);
        // The table is resizable, so this measured width applies when it is first made.
        ImGui::TableSetupColumn("##ZoneActions", ImGuiTableColumnFlags_WidthFixed,
            ImGui::CalcTextSize(Tr("zones_edit")).x + 2 * ImGui::GetStyle().FramePadding.x);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()));
        while (clipper.Step()) {
            for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
                const auto& row = m_rows[static_cast<size_t>(index)];
                ImGui::PushID(row.id.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const char* official = row.official.empty() ? Tr("zones_unknown_area") : row.official.c_str();
                ImGui::TextUnformatted(official);
                if (ImGui::IsItemHovered()) {
                    if (row.unused) ImGui::SetTooltip("%s\n%s", official, Tr("zones_row_unused"));
                    else ImGui::SetTooltip("%s", official);
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.id.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.id.c_str());
                ImGui::TableNextColumn();
                if (row.custom.empty()) ImGui::TextDisabled("%s", Tr("zones_game_default"));
                else ImGui::TextUnformatted(row.custom.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(TrId("zones_edit"))) Select(row.id, names);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

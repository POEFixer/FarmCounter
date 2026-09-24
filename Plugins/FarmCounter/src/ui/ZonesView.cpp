#include "ZonesView.h"
#include "Settings.h"
#include "../ZoneCatalog.h"
#include "../ZoneClassify.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace {
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
        Row row{raw, entry ? entry->name : "Unknown area", names.Override(raw), entry && entry->isUnused};
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
    ImGui::TextDisabled("Game names are built in. Custom display names change labels, keeping history together.");
    ImGui::SetNextItemWidth(-1.f);
    if (TextInput("##ZoneSearch", "Search game name, area ID, or custom name", m_search)) m_rebuild = true;
    if (ImGui::Checkbox("All areas##ZoneAllAreas", &m_allAreas)) m_rebuild = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Include unused##ZoneUnused", &m_includeUnused)) m_rebuild = true;
    ImGui::SameLine();
    ImGui::TextDisabled("Default: maps and visited areas");

    ImGui::SeparatorText("Add an area ID");
    ImGui::SetNextItemWidth((std::max)(160.f, ImGui::GetContentRegionAvail().x - 115.f));
    const bool addEntered = TextInput("##ZoneAddId", "Unknown or future area ID", m_addRaw,
                                      ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool addClicked = ImGui::Button("Add / edit##ZoneAdd");
    if (addEntered || addClicked) {
        const std::string raw = TrimId(m_addRaw);
        if (raw.empty()) {
            m_status = "Enter an area ID.";
            m_error = true;
        } else if (names.Map().contains(raw) || names.SetOverride(deps.dir, raw, "")) {
            Select(raw, names);
            m_addRaw.clear();
        } else {
            m_status = "Could not save this area. Check the plugin folder is writable.";
            m_error = true;
        }
    }

    ImGui::SeparatorText("Display name");
    if (m_selected.empty()) {
        ImGui::TextDisabled("Choose Edit beside an area below.");
    } else {
        const auto* entry = ZoneCatalog::Lookup(m_selected);
        ImGui::TextWrapped("%s", entry ? entry->name : "Unknown area");
        ImGui::TextDisabled("%s", m_selected.c_str());
        if (entry && entry->isUnused) ImGui::TextDisabled("Marked unused in the game data.");
        if (m_focusEditor) { ImGui::SetKeyboardFocusHere(); m_focusEditor = false; }
        ImGui::SetNextItemWidth(-1.f);
        const bool entered = TextInput("##ZoneDisplayName", names.OfficialName(m_selected).c_str(),
            m_editText, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool saveClicked = ImGui::Button("Save##ZoneSave");
        ImGui::SameLine();
        const bool resetClicked = ImGui::Button("Reset to game name##ZoneReset");
        ImGui::SameLine();
        if (ImGui::Button("Cancel##ZoneCancel")) {
            m_selected.clear();
            m_editText.clear();
            m_status.clear();
        } else if (entered || saveClicked || resetClicked) {
            const bool saved = resetClicked ? names.ResetOverride(deps.dir, m_selected)
                                           : names.SetOverride(deps.dir, m_selected, m_editText);
            m_error = !saved;
            m_status = saved ? "Saved." : "Could not save the display name. Your previous name is unchanged.";
            if (saved && resetClicked) m_editText.clear();
        }
    }
    if (!m_status.empty()) {
        if (m_error) ImGui::TextColored(ImVec4(1.f, .45f, .35f, 1.f), "%s", m_status.c_str());
        else ImGui::TextDisabled("%s", m_status.c_str());
    }

    if (m_rebuild || m_revision != names.Revision()) Rebuild(names);
    ImGui::SeparatorText("Areas");
    ImGui::TextDisabled("%zu areas", m_rows.size());
    const float height = (std::max)(100.f, ImGui::GetContentRegionAvail().y);
    if (ImGui::BeginTable("##ZoneCatalogTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0.f, height))) {
        ImGui::TableSetupColumn("Game name", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("Area ID", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("Custom display name", ImGuiTableColumnFlags_WidthStretch, .85f);
        ImGui::TableSetupColumn("##ZoneActions", ImGuiTableColumnFlags_WidthFixed, 44.f);
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
                ImGui::TextUnformatted(row.official.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s%s", row.official.c_str(), row.unused ? "\nMarked unused in game data." : "");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.id.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", row.id.c_str());
                ImGui::TableNextColumn();
                if (row.custom.empty()) ImGui::TextDisabled("Game default");
                else ImGui::TextUnformatted(row.custom.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Edit")) Select(row.id, names);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SettingsDeps;
class ZoneNames;

// Owned by the plugin shell. Draft text survives redraws, registry refreshes,
// and other tabs; separate plugin/editor instances never share input state.
class ZonesView {
public:
    void Draw(const SettingsDeps& deps);

private:
    struct Row {
        std::string id, official, custom;
        bool unused = false;
    };
    void Select(const std::string& raw, const ZoneNames& names);
    void Rebuild(const ZoneNames& names);

    ZoneNames* m_source = nullptr;
    std::filesystem::path m_directory;
    uint64_t m_revision = UINT64_MAX;
    bool m_rebuild = true;
    bool m_allAreas = false;
    bool m_includeUnused = false;
    bool m_focusEditor = false;
    bool m_error = false;
    std::string m_search, m_addRaw, m_selected, m_editText, m_status;
    std::vector<Row> m_rows;
};

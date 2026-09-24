#include "ZoneNames.h"
#include "ZoneCatalog.h"
#include "FileIO.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
const std::unordered_map<std::string, std::string>& OfficialNames() {
    // Immutable storage keeps Display's existing reference-return contract safe
    // across Register/Load and unordered-map rehashes in the user's overrides.
    static const auto names = [] {
        std::unordered_map<std::string, std::string> result;
        result.reserve(ZoneCatalog::Entries().size());
        for (const auto& row : ZoneCatalog::Entries()) result.emplace(row.id, row.name);
        return result;
    }();
    return names;
}

bool SaveNames(const fs::path& dir, const std::unordered_map<std::string, std::string>& names) {
    if (dir.empty()) return false;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return false;
    json document = json::object();
    for (const auto& [raw, display] : names) document[raw] = display;
    const auto payload = document.dump(4, ' ', false, json::error_handler_t::replace) + "\n";
    return FileIO::AtomicWriteBackup(dir / "zone_names.json", payload);
}
} // namespace

void ZoneNames::Load(const fs::path& dir) {
    m_names.clear();
    ++m_revision;
    if (dir.empty()) return;
    fs::path p = dir / "zone_names.json";
    std::ifstream f(p);
    if (!f.is_open()) return;
    json j = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_string() && !it.key().empty())
            m_names[it.key()] = it.value().get<std::string>();
}

bool ZoneNames::Save(const fs::path& dir) const { return SaveNames(dir, m_names); }

void ZoneNames::Register(const fs::path& dir, const std::string& raw) {
    if (raw.empty()) return;
    if (m_names.emplace(raw, "").second) { ++m_revision; Save(dir); }
}

const std::string& ZoneNames::Display(const std::string& raw) const {
    auto it = m_names.find(raw);
    if (it != m_names.end() && !it->second.empty()) return it->second;
    return OfficialName(raw);
}

const std::string& ZoneNames::OfficialName(const std::string& raw) const {
    const auto& names = OfficialNames();
    const auto found = names.find(raw);
    return found == names.end() ? raw : found->second;
}

const std::string& ZoneNames::Override(const std::string& raw) const {
    static const std::string empty;
    const auto found = m_names.find(raw);
    return found == m_names.end() ? empty : found->second;
}

std::string ZoneNames::CategoryKey(const std::string& raw) const {
    const auto* entry = ZoneCatalog::Lookup(raw);
    return entry ? entry->category : raw;
}

const std::string& ZoneNames::CategoryDisplay(const std::string& key) const {
    static const std::string simulacrum = "Simulacrum";
    return key == simulacrum ? simulacrum : Display(key);
}

bool ZoneNames::SetOverride(const fs::path& dir, const std::string& raw, const std::string& display) {
    if (raw.empty()) return false;
    auto updated = m_names;
    updated[raw] = display;
    if (!SaveNames(dir, updated)) return false;
    m_names.swap(updated);
    ++m_revision;
    return true;
}

bool ZoneNames::ResetOverride(const fs::path& dir, const std::string& raw) {
    // Keep the row registered; a reset restores its generated default, and a
    // future catalog regeneration still cannot overwrite the user's other rows.
    return SetOverride(dir, raw, "");
}

#include "ZoneNames.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

void ZoneNames::Load(const fs::path& dir) {
    fs::path p = dir / "zone_names.json";
    if (!fs::exists(p)) return;
    std::ifstream f(p);
    if (!f.is_open()) return;
    json j = json::parse(f, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return;
    m_names.clear();
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_string() && !it.key().empty())
            m_names[it.key()] = it.value().get<std::string>();
}

void ZoneNames::Save(const fs::path& dir) const {
    // Write-to-tmp + rename so a crash mid-write can't truncate the file.
    const fs::path target = dir / "zone_names.json";
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f.is_open()) return;
        // nlohmann keeps object keys sorted — stable, diff-friendly output.
        // error_handler replace: raw zone names come from game memory; a stray
        // invalid-UTF-8 byte must never throw inside the host's ImGui frame.
        json j = json::object();
        for (const auto& kv : m_names) j[kv.first] = kv.second;
        const std::string out = j.dump(4, ' ', false, json::error_handler_t::replace) + "\n";
        f.write(out.data(), (std::streamsize)out.size());
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) fs::remove(tmp, ec);
}

void ZoneNames::Register(const fs::path& dir, const std::string& raw) {
    if (raw.empty()) return;
    if (m_names.find(raw) == m_names.end()) { m_names[raw] = ""; Save(dir); }
}

const std::string& ZoneNames::Display(const std::string& raw) const {
    auto it = m_names.find(raw);
    if (it != m_names.end() && !it->second.empty()) return it->second;
    return raw;
}

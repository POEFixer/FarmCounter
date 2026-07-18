#include "ZoneNames.h"
#include <fstream>
#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Minimal JSON string escaping for the hand-rolled zone_names.json format:
// display names come from a free-text InputText, so quotes/backslashes must not
// corrupt the file.
std::string JsonEscape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '\\')      r += "\\\\";
        else if (c == '"')  r += "\\\"";
        else if ((unsigned char)c < 0x20) r += ' ';  // control chars -> space
        else r += c;
    }
    return r;
}

// Parses a double-quoted string starting at `from` (which must point at the
// opening '"'), honoring \" and \\ escapes. Returns false on malformed input;
// on success `out` holds the unescaped value and `next` the index PAST the
// closing quote.
bool ParseQuoted(const std::string& line, size_t from, std::string& out, size_t& next) {
    if (from >= line.size() || line[from] != '"') return false;
    out.clear();
    for (size_t i = from + 1; i < line.size(); i++) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size()) { out += line[++i]; continue; }
        if (c == '"') { next = i + 1; return true; }
        out += c;
    }
    return false;
}

} // namespace

void ZoneNames::Load(const fs::path& dir) {
    fs::path p = dir / "zone_names.json";
    if (!fs::exists(p)) return;
    std::ifstream f(p);
    if (!f.is_open()) return;
    m_names.clear();
    std::string line;
    while (std::getline(f, line)) {
        // Expected shape per line:  "key": "value"[,]
        size_t q = line.find('"');
        if (q == std::string::npos) continue;
        std::string key, val;
        size_t after = 0;
        if (!ParseQuoted(line, q, key, after) || key.empty()) continue;
        size_t colon = line.find(':', after);
        if (colon == std::string::npos) continue;
        size_t vq = line.find('"', colon);
        if (vq != std::string::npos) ParseQuoted(line, vq, val, after);
        m_names[key] = val;
    }
}

void ZoneNames::Save(const fs::path& dir) const {
    // Write-to-tmp + rename so a crash mid-write can't truncate the file.
    const fs::path target = dir / "zone_names.json";
    fs::path tmp = target;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f.is_open()) return;
        std::vector<std::pair<std::string, std::string>> sorted(m_names.begin(), m_names.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
        f << "{\n";
        bool first = true;
        for (const auto& kv : sorted) {
            if (!first) f << ",\n";
            first = false;
            f << "    \"" << JsonEscape(kv.first) << "\": \"" << JsonEscape(kv.second) << "\"";
        }
        f << "\n}\n";
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

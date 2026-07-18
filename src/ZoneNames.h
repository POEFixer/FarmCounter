#pragma once
#include <string>
#include <unordered_map>
#include <filesystem>

class ZoneNames {
public:
    void Load(const std::filesystem::path& dir);
    void Save(const std::filesystem::path& dir) const;
    void Register(const std::filesystem::path& dir, const std::string& raw);
    const std::string& Display(const std::string& raw) const;
    std::unordered_map<std::string, std::string>& Map() { return m_names; }
    const std::unordered_map<std::string, std::string>& Map() const { return m_names; }

private:
    std::unordered_map<std::string, std::string> m_names; // raw -> display ("" = use raw)
};

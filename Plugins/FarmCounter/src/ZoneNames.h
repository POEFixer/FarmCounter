#pragma once
#include <string>
#include <unordered_map>
#include <filesystem>
#include <cstdint>

class ZoneNames {
public:
    void Load(const std::filesystem::path& dir);
    bool Save(const std::filesystem::path& dir) const;
    void Register(const std::filesystem::path& dir, const std::string& raw);
    const std::string& Display(const std::string& raw) const;
    const std::string& OfficialName(const std::string& raw) const;
    const std::string& Override(const std::string& raw) const;
    std::string CategoryKey(const std::string& raw) const;
    const std::string& CategoryDisplay(const std::string& key) const;
    bool SetOverride(const std::filesystem::path& dir, const std::string& raw, const std::string& display);
    bool ResetOverride(const std::filesystem::path& dir, const std::string& raw);
    uint64_t Revision() const { return m_revision; }
    // Legacy mutable access retained for existing callers. New editors must use
    // setters so persistence and the revision used by list caches stay together.
    std::unordered_map<std::string, std::string>& Map() { return m_names; }
    const std::unordered_map<std::string, std::string>& Map() const { return m_names; }

private:
    std::unordered_map<std::string, std::string> m_names; // visited/custom raw -> override ("" = game default)
    uint64_t m_revision = 0;
};

#pragma once
#include "FarmTypes.h"
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace FarmStatistics {
inline constexpr size_t NoRun = (std::numeric_limits<size_t>::max)();
using CategoryResolver = std::function<std::string(const std::string&)>;

struct Currency {
    int kind = 0; // 0: saved exalted rate; 1: current divine rate; 2: chaos.
    double exalted = 1.0;
    double divine = 1.0;
};
struct Filter {
    bool currentSession = false;
    int archivedSession = -1;
    int64_t since = 0;
    std::string category;
};
struct Summary {
    size_t runs = 0, completedRuns = 0, invalidValues = 0;
    int64_t seconds = 0, completedSeconds = 0, gold = 0, hiveblood = 0, beacons = 0;
    std::array<int64_t, 5> kills{};
    double chaos = 0, value = 0, completedValue = 0;
    double bestValue = 0, worstValue = 0;
    size_t bestRun = NoRun, worstRun = NoRun;
    double PerHour() const;
    double AverageValue() const;
    int64_t Kills() const;
    // Display buckets are Normal, Magic, Rare and Unique. Rogue Exiles are
    // tracked independently for compatibility, but are shown in Unique.
    std::array<int64_t, 4> KillsByRarity() const;
};
struct Category {
    std::string key;
    Summary summary;
};
struct Snapshot {
    Summary summary;
    std::vector<Category> categories;
    std::vector<size_t> rows;
};
struct LootTotal {
    std::string name, iconPath;
    int64_t stack = 0;
    double chaos = 0, value = 0;
    int rarity = 0;
};
enum class Sort { Date, Map, Profit, Duration, Rate, Kills, Gold };

double RunValue(const MapRun& run, const Currency& currency);
double RunRate(const MapRun& run, const Currency& currency);
Snapshot Build(const std::vector<MapRun>& runs, int activeIndex, const Filter& filter,
               const Currency& currency, const CategoryResolver& category);
std::vector<LootTotal> AggregateLoot(const std::vector<MapRun>& runs,
                                   const std::vector<size_t>& rows, const Currency& currency);
void SortRows(std::vector<size_t>& rows, const std::vector<MapRun>& runs,
              const Currency& currency, Sort sort, bool ascending);
std::unordered_map<std::string, size_t> CountCategories(const std::vector<MapRun>& runs,
                                                      const CategoryResolver& category);
} // namespace FarmStatistics

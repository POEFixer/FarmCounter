#include "StatisticsModel.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace FarmStatistics {
namespace {
double Rate(double value, double fallback = 1.0) {
    return std::isfinite(value) && value > .0001 ? value : fallback;
}
double Convert(double chaos, const MapRun& run, const Currency& currency) {
    if (!std::isfinite(chaos)) return 0;
    if (currency.kind == 0) return chaos / Rate(run.exaltedRate, Rate(currency.exalted));
    if (currency.kind == 1) return chaos / Rate(currency.divine);
    return chaos;
}
std::string Key(const MapRun& run, const CategoryResolver& resolver) {
    std::string key = resolver ? resolver(run.mapName) : run.mapName;
    return key.empty() ? "Unknown area" : key;
}
bool Matches(const MapRun& run, const Filter& filter) {
    if (filter.currentSession && run.archived) return false;
    if (filter.archivedSession >= 0 && (!run.archived || run.sessionId != filter.archivedSession)) return false;
    return filter.since <= 0 || run.startedAt >= filter.since;
}
void Add(Summary& sum, const MapRun& run, size_t index, bool active, const Currency& currency) {
    ++sum.runs;
    const int64_t seconds = (std::max)(0, run.durationSec);
    const double value = RunValue(run, currency);
    sum.seconds += seconds;
    sum.chaos += std::isfinite(run.totalChaos) ? run.totalChaos : 0;
    sum.value += value;
    if (!std::isfinite(run.totalChaos)) ++sum.invalidValues;
    sum.gold += run.goldGain;
    sum.hiveblood += run.hivebloodGain;
    sum.beacons += run.beaconGain;
    sum.kills[0] += run.killsNormal; sum.kills[1] += run.killsMagic;
    sum.kills[2] += run.killsRare; sum.kills[3] += run.killsUnique; sum.kills[4] += run.killsRogue;
    if (active) return;
    ++sum.completedRuns;
    sum.completedSeconds += seconds;
    sum.completedValue += value;
    if (sum.bestRun == NoRun || value > sum.bestValue) { sum.bestRun = index; sum.bestValue = value; }
    if (sum.worstRun == NoRun || value < sum.worstValue) { sum.worstRun = index; sum.worstValue = value; }
}
int64_t Kills(const MapRun& run) {
    return static_cast<int64_t>(run.killsNormal) + run.killsMagic + run.killsRare + run.killsUnique + run.killsRogue;
}
}

double Summary::PerHour() const { return seconds > 0 ? value * 3600.0 / static_cast<double>(seconds) : 0; }
double Summary::AverageValue() const { return completedRuns ? completedValue / static_cast<double>(completedRuns) : 0; }
int64_t Summary::Kills() const { return std::accumulate(kills.begin(), kills.end(), int64_t{0}); }
std::array<int64_t, 4> Summary::KillsByRarity() const {
    return {kills[0], kills[1], kills[2], kills[3] + kills[4]};
}
double RunValue(const MapRun& run, const Currency& currency) { return Convert(run.totalChaos, run, currency); }
double RunRate(const MapRun& run, const Currency& currency) {
    return run.durationSec > 0 ? RunValue(run, currency) * 3600.0 / run.durationSec : 0;
}

Snapshot Build(const std::vector<MapRun>& runs, int activeIndex, const Filter& filter,
               const Currency& currency, const CategoryResolver& category) {
    Snapshot result;
    std::unordered_map<std::string, size_t> groupIndices;
    result.rows.reserve(runs.size());
    for (size_t i = 0; i < runs.size(); ++i) {
        const auto& run = runs[i];
        if (!Matches(run, filter)) continue;
        const std::string key = Key(run, category);
        auto [found, inserted] = groupIndices.try_emplace(key, result.categories.size());
        if (inserted) result.categories.push_back(Category{key, {}});
        const bool active = activeIndex >= 0 && i == static_cast<size_t>(activeIndex);
        Add(result.categories[found->second].summary, run, i, active, currency);
        if (!filter.category.empty() && key != filter.category) continue;
        result.rows.push_back(i);
        Add(result.summary, run, i, active, currency);
    }
    std::sort(result.categories.begin(), result.categories.end(), [](const Category& a, const Category& b) {
        return a.key < b.key;
    });
    SortRows(result.rows, runs, currency, Sort::Date, false);
    return result;
}

std::vector<LootTotal> AggregateLoot(const std::vector<MapRun>& runs,
                                   const std::vector<size_t>& rows, const Currency& currency) {
    std::unordered_map<std::string, LootTotal> merged;
    for (size_t i : rows) {
        if (i >= runs.size()) continue;
        const auto& run = runs[i];
        for (const auto& entry : run.loot) {
            auto& item = merged[entry.name];
            item.name = entry.name;
            item.stack += entry.stackCount;
            const double value = static_cast<double>(entry.chaosEach) * entry.stackCount;
            if (std::isfinite(value)) { item.chaos += value; item.value += Convert(value, run, currency); }
            if (item.iconPath.empty()) item.iconPath = entry.iconPath;
            item.rarity = entry.rarity;
        }
    }
    std::vector<LootTotal> result;
    result.reserve(merged.size());
    for (auto& [name, item] : merged) result.push_back(std::move(item));
    std::sort(result.begin(), result.end(), [](const LootTotal& a, const LootTotal& b) {
        if (a.value != b.value) return a.value > b.value;
        return a.name < b.name;
    });
    return result;
}

void SortRows(std::vector<size_t>& rows, const std::vector<MapRun>& runs,
              const Currency& currency, Sort sort, bool ascending) {
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](size_t i) { return i >= runs.size(); }), rows.end());
    const auto scalar = [&](const MapRun& r) -> double {
        switch (sort) {
        case Sort::Date: return static_cast<double>(r.startedAt);
        case Sort::Profit: return RunValue(r, currency);
        case Sort::Duration: return r.durationSec;
        case Sort::Rate: return RunRate(r, currency);
        case Sort::Kills: return static_cast<double>(Kills(r));
        case Sort::Gold: return r.goldGain;
        default: return 0;
        }
    };
    std::sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
        const auto& left = runs[a]; const auto& right = runs[b];
        if (sort == Sort::Map && left.mapName != right.mapName)
            return ascending ? left.mapName < right.mapName : left.mapName > right.mapName;
        const double av = scalar(left), bv = scalar(right);
        if (av != bv) return ascending ? av < bv : av > bv;
        if (left.startedAt != right.startedAt) return left.startedAt > right.startedAt;
        if (left.dbId != right.dbId) return left.dbId > right.dbId;
        return a > b;
    });
}

std::unordered_map<std::string, size_t> CountCategories(const std::vector<MapRun>& runs,
                                                      const CategoryResolver& category) {
    std::unordered_map<std::string, size_t> result;
    for (const auto& run : runs) ++result[Key(run, category)];
    return result;
}
} // namespace FarmStatistics

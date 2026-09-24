#pragma once
#include <memory>

struct SettingsDeps;

// Per-plugin view state. Cached rows are indices/IDs, never retained MapRun
// pointers; the tracker owns history and applies mutations after drawing.
class StatisticsView {
public:
    StatisticsView();
    ~StatisticsView();
    StatisticsView(const StatisticsView&) = delete;
    StatisticsView& operator=(const StatisticsView&) = delete;
    void Draw(const SettingsDeps& dependencies);
private:
    struct State;
    std::unique_ptr<State> m_state;
};

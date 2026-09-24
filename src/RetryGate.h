#pragma once

#include <chrono>

namespace Core {

// Small single-threaded gate for retrying a read that may become available
// later. It prevents a failed probe from becoming a per-frame hot path while
// retaining a bounded window for late publication.
class RetryGate {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr auto DefaultInterval = std::chrono::milliseconds(250);
    static constexpr auto DefaultWindow = std::chrono::seconds(120);

    void Reset(TimePoint now, Clock::duration window = DefaultWindow) noexcept {
        m_deadline = now + window;
        m_nextAttempt = now;
        m_ready = false;
    }

    bool ShouldAttempt(TimePoint now) const noexcept {
        return !m_ready && now < m_deadline && now >= m_nextAttempt;
    }

    void RecordAttempt(TimePoint now, bool ready) noexcept {
        if (ready) {
            m_ready = true;
        } else {
            m_nextAttempt = now + DefaultInterval;
        }
    }

    // Move the schedule with a paused caller's clock. Consumers that pause
    // their elapsed-time anchors must shift this gate by the same duration.
    void Shift(Clock::duration delta) noexcept {
        m_deadline += delta;
        m_nextAttempt += delta;
    }

    bool Ready() const noexcept { return m_ready; }

private:
    TimePoint m_deadline{};
    TimePoint m_nextAttempt{};
    bool m_ready = false;
};

} // namespace Core

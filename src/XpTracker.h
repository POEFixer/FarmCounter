#pragma once
// A character-local XP measurement, independent of the farm-history session.
// XP is cumulative across levels. Signed subtraction preserves death penalties;
// the caller supplies the same running/paused state as FarmCounter's active timer.
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

class XpTracker {
public:
    using Clock = std::chrono::steady_clock;

    void Clear() { *this = XpTracker{}; }

    void Reset() {
        m_gainedXp = 0;
        m_activeSeconds = 0;
        m_running = false;
        // The last cached XP may predate the click. Anchor to the next fresh
        // player read so old gains cannot enter the newly reset measurement.
        m_needsBaseline = true;
    }

    void Tick(bool running, Clock::time_point now) {
        if (m_running && now > m_lastTick)
            m_activeSeconds += std::chrono::duration<double>(now - m_lastTick).count();
        m_lastTick = now;
        m_running = running && m_hasSample && !m_needsBaseline;
    }

    void Sample(std::string_view character, uint32_t xp, uint8_t level,
                bool running, Clock::time_point now) {
        // A failed memory read can be reported as zeroed fields. Do not turn it
        // into a character switch or billions of XP lost. Level 1 can have 0 XP.
        if (character.empty() || level == 0 || level > 100 || (level > 1 && xp == 0)) {
            Tick(running, now);
            return;
        }
        if (!m_hasSample || m_needsBaseline || character != m_characterName || level < m_level) {
            m_characterName.assign(character);
            m_baselineXp = xp;
            m_gainedXp = 0;
            m_activeSeconds = 0;
            m_hasSample = true;
            m_needsBaseline = false;
            m_lastTick = now;
            m_running = running;
        } else {
            Tick(running, now);
            m_gainedXp = static_cast<int64_t>(xp) - static_cast<int64_t>(m_baselineXp);
        }
        m_level = level;
    }

    bool HasSample() const { return m_hasSample; }
    bool IsRunning() const { return m_running; }
    uint8_t Level() const { return m_level; }
    const std::string& CharacterName() const { return m_characterName; }
    int64_t GainedXp() const { return m_gainedXp; }
    double ActiveSeconds() const { return m_activeSeconds; }
    double XpPerHour() const {
        return m_activeSeconds >= 1.0 ? static_cast<double>(m_gainedXp) * 3600.0 / m_activeSeconds : 0.0;
    }

private:
    std::string m_characterName;
    uint32_t m_baselineXp = 0;
    uint8_t m_level = 0;
    int64_t m_gainedXp = 0;
    double m_activeSeconds = 0;
    Clock::time_point m_lastTick{};
    bool m_hasSample = false;
    bool m_needsBaseline = false;
    bool m_running = false;
};

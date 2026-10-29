#pragma once
// AtlasReload.h — when FarmCounter (re)loads the radar icon atlas (Resources/radar/icons.png).
// The host installs that file as game data (group PSDK_GAMEDATA_GROUP_RADAR_ICONS, desktop
// 371+): a fresh install has no file until the first download finishes, and a later download
// replaces it under a new group revision. STL-only: tools/farm_counter_resources_test runs it.
#include <cstdint>

namespace FcAtlas {

// Retry period while the current revision's atlas is not loaded (the file is absent on a first
// run until the host installs it, or could not be read for a moment).
inline constexpr uint64_t kRetryMs = 5000;

// A texture view replaced by a reload stays alive for this many more frames: a draw list built
// earlier in the same frame (the settings tab in another ImGui context) may still name it.
inline constexpr int kRetireFrames = 3;

class ReloadPolicy {
public:
    // True when a load should be tried now: the first look, a new group revision (new data
    // installed; 0 = none yet or an older host), or a retry while the current revision's atlas
    // is not loaded.
    bool Due(uint64_t revision, uint64_t nowMs) const {
        if (!m_attempted || revision != m_revision) return true;
        return !m_loaded && nowMs - m_lastAttemptMs >= kRetryMs;
    }
    // Records a load attempt for `revision` at nowMs: ok = that revision's atlas is loaded (a
    // failed reload keeps drawing the atlas already held, and is tried again).
    void Record(uint64_t revision, uint64_t nowMs, bool ok) {
        m_attempted = true;
        m_revision = revision;
        m_lastAttemptMs = nowMs;
        m_loaded = ok;
    }
    void Reset() { *this = ReloadPolicy{}; }

private:
    bool     m_attempted = false;
    bool     m_loaded = false;
    uint64_t m_revision = 0;
    uint64_t m_lastAttemptMs = 0;
};

}  // namespace FcAtlas

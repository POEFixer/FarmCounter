#pragma once
// LootScanner.h — targeted inventory Scan(1) request/read + snapshot build.
//
// THE FIX: we request ctx()->Inventory.Scan(1) (targeted, player main inventory
// id 1) rather than Scan(-1). Post host "update 251" a full -1 scan is skipped by
// the host when no inventory panel is visible (visibility gate), so loot stopped
// accruing with the backpack closed. A targeted id-1 scan ("id >= 0 always read")
// bypasses the gate and is also cheaper (one inventory vs. ~15).
#include "sdk/PluginSDK.h"
#include "FarmTypes.h"
#include "PriceProvider.h"
#include <unordered_map>
#include <string>
#include <chrono>

class LootScanner {
public:
    void SetContext(const PluginSDK::Context* ctx, PriceProvider* prices) { m_ctx = ctx; m_prices = prices; }
    void SetAreaCounter(uint64_t counter) { m_expectedAreaCounter = counter; m_hasExpectedArea = true; }
    // Drop any in-flight read phase (call on zone change / new session). The
    // throttle timestamp is intentionally NOT reset: a stale m_lastScan only ever
    // lets the next request fire sooner, and the baseline-settle gate (1500ms) and
    // resume path both live in FarmTracker, so timing is unaffected.
    void ResetTiming(uint64_t baselineStamp = 0) {
        m_pending = false;
        if (baselineStamp != 0) m_lastConsumedStamp = baselineStamp;
    }
    // Read the latest published, usable backpack without requesting a refresh.
    // This is the observable inventory boundary for an in-map session reset.
    // A peek does not consume the token: a failed session transaction must not
    // hide that scan from the old session. Adopt it through ResetTiming on commit.
    bool ReadCurrent(std::unordered_map<std::string, InvSnapshot>& out, uint64_t* scanStamp = nullptr);
    // A supporting host must provide a distinct completed-scan token. Retained
    // data from a suppressed/coalesced scan request cannot count as a second read.
    bool RequestAndRead(std::unordered_map<std::string, InvSnapshot>& out);

private:
    std::unordered_map<std::string, InvSnapshot> BuildSnapshot(const PluginSDK::Inventory& inv);
    bool ReadPublished(std::unordered_map<std::string, InvSnapshot>& out, bool requireNew, uint64_t* scanStamp);
    const PluginSDK::Context* m_ctx = nullptr;
    PriceProvider* m_prices = nullptr;
    std::chrono::steady_clock::time_point m_lastScan{};
    bool m_pending = false;
    uint64_t m_lastConsumedStamp = 0;
    uint64_t m_expectedAreaCounter = 0;
    bool m_hasExpectedArea = false;
};

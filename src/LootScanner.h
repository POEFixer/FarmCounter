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
    // Drop any in-flight read phase (call on zone change / new session). The
    // throttle timestamp is intentionally NOT reset: a stale m_lastScan only ever
    // lets the next request fire sooner, and the baseline-settle gate (1500ms) and
    // resume path both live in FarmTracker, so timing is unaffected.
    void ResetTiming() { m_pending = false; }
    // Returns true and fills `out` when a fresh snapshot was read this call.
    bool RequestAndRead(std::unordered_map<std::string, InvSnapshot>& out);

private:
    std::unordered_map<std::string, InvSnapshot> BuildSnapshot(const PluginSDK::Inventory& inv);
    const PluginSDK::Context* m_ctx = nullptr;
    PriceProvider* m_prices = nullptr;
    std::chrono::steady_clock::time_point m_lastScan{};
    bool m_pending = false;
};

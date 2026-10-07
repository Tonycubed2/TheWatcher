#pragma once
#include <cstdint>
namespace IndependentDecision {
inline bool Stalled(std::uint64_t now, std::uint64_t heartbeat, std::uint64_t threshold) {
    return heartbeat != 0 && now >= heartbeat && now - heartbeat >= threshold;
}
struct KeyHold {
    bool down = false;
    bool fired = false;
    std::uint64_t since = 0;
    bool Poll(bool pressed, bool modifiers, std::uint64_t now, std::uint64_t hold) {
        if (!pressed) { down = false; fired = false; return false; }
        if (!down) { down = true; fired = false; since = now; }
        if (fired || !modifiers || now - since < hold) return false;
        fired = true;
        return true;
    }
};
}

namespace IndependentDecision {
// One automatic capture per episode. Recovery must be sustained AFTER capture;
// progress made while dumping never rearms detection.
struct Episode {
    bool captured = false;
    std::uint64_t healthySince = 0;
    std::uint64_t lastEnd = 0;
    bool wasLoading = false;
    bool ObserveState(std::uint64_t now, std::uint64_t heartbeat, std::uint64_t threshold,
                      bool busy, std::uint64_t captureEnd, bool loading, bool allowLoading) {
        if (loading != wasLoading) {
            // Loading and gameplay are separate episodes. LoadEnd refreshes the frame heartbeat.
            captured = false;
            healthySince = 0;
            wasLoading = loading;
        }
        const bool due = Observe(now, heartbeat, threshold, busy, captureEnd);
        return due && (!loading || allowLoading);
    }
    bool Observe(std::uint64_t now, std::uint64_t heartbeat, std::uint64_t threshold,
                 bool busy, std::uint64_t captureEnd) {
        if (captureEnd != lastEnd) { lastEnd = captureEnd; healthySince = 0; }
        if (busy || (lastEnd && now - lastEnd < 5000)) { healthySince = 0; return false; }
        const bool healthy = heartbeat && now >= heartbeat && now - heartbeat <= 250;
        if (healthy) {
            if (!healthySince) healthySince = now;
            if (now - healthySince >= 2000) captured = false;
        } else healthySince = 0;
        return !captured && Stalled(now, heartbeat, threshold);
    }
    void Captured() { captured = true; healthySince = 0; }
};
}

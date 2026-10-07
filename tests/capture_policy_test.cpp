#include "../src/IndependentDecision.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <iostream>
using namespace IndependentDecision;
int main() {
    Episode e;
    assert(!e.Observe(8999, 1000, 8000, false, 0));
    assert(e.Observe(9000, 1000, 8000, false, 0));
    e.Captured();
    // Simulate the reported 59-second dump, with brief progress during it.
    assert(!e.Observe(30000, 29990, 8000, true, 0));
    assert(!e.Observe(68000, 1000, 8000, true, 0));
    assert(!e.Observe(68020, 1000, 8000, false, 68000));
    // A finished capture cannot immediately generate another one.
    assert(!e.Observe(73020, 1000, 8000, false, 68000));
    assert(!e.Observe(100000, 1000, 8000, false, 68000));
    // One brief resumed frame is insufficient to rearm.
    assert(!e.Observe(100100, 100100, 8000, false, 68000));
    assert(!e.Observe(108100, 100100, 8000, false, 68000));
    // Two full seconds of fresh progress after cooldown rearms the next real stall.
    for (unsigned long long t = 110000; t <= 112000; t += 20)
        assert(!e.Observe(t, t, 8000, false, 68000));
    assert(e.Observe(120000, 112000, 8000, false, 68000));
    e.Captured();
    assert(!e.Observe(150000, 112000, 8000, false, 68000));
    Episode loading;
    assert(!loading.Observe(20999, 1000, 20000, false, 0));
    assert(loading.Observe(21000, 1000, 20000, false, 0));
    loading.Captured();
    assert(!loading.Observe(60000, 1000, 20000, false, 0));
    Episode normalLoad;
    // Even a six-minute load with no events cannot trigger a default automatic capture.
    for (std::uint64_t t = 1000; t <= 362000; t += 20)
        assert(!normalLoad.ObserveState(t, 1000, 300000, false, 0, true, false));
    // Explicit opt-in permits the longer loading threshold.
    assert(normalLoad.ObserveState(362020, 1000, 300000, false, 0, true, true));
    normalLoad.Captured();
    assert(!normalLoad.ObserveState(400000, 1000, 300000, false, 0, true, true));
    // Gameplay starts a separate episode with a refreshed frame heartbeat.
    assert(!normalLoad.ObserveState(400020, 400020, 8000, false, 0, false, false));
    assert(!normalLoad.ObserveState(408019, 400020, 8000, false, 0, false, false));
    assert(normalLoad.ObserveState(408020, 400020, 8000, false, 0, false, false));
    // CAS acquisition used by the Windows interlocked gate must admit one owner.
    std::atomic<int> gate{0};
    std::atomic<int> winners{0};
    auto attempt = [&](int id) { int expected=0; if(gate.compare_exchange_strong(expected,id)) ++winners; };
    std::thread plugin(attempt,1), helper(attempt,2);
    plugin.join(); helper.join();
    assert(winners==1);
    int wrongOwner=gate==1?2:1;
    assert(!gate.compare_exchange_strong(wrongOwner,0));
    int owner=gate.load(); assert(gate.compare_exchange_strong(owner,0));
    assert(gate==0);
    std::cout << "Capture policy regression tests passed\n";
}

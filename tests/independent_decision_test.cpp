#include "../src/IndependentDecision.h"
#include <cassert>
#include <iostream>
using namespace IndependentDecision;
int main() {
    // Startup without a heartbeat is not a freeze. A real frame gap triggers
    // at three seconds, before the reported ten-second crash.
    assert(!Stalled(10000, 0, 3000));
    assert(!Stalled(3999, 1000, 3000));
    assert(Stalled(4000, 1000, 3000));
    assert(Stalled(10999, 1000, 3000));
    assert(!Stalled(11000, 10980, 3000));
    assert(!Stalled(4000, 5000, 3000));
    // Loading uses its own progress clock and longer threshold.
    assert(!Stalled(4000, 1000, 45000));
    assert(Stalled(46000, 1000, 45000));
    assert(!Stalled(46000, 45900, 45000));
    KeyHold tap;
    assert(tap.Poll(true, true, 100, 0));
    assert(!tap.Poll(true, true, 120, 0));
    assert(!tap.Poll(false, true, 140, 0));
    assert(tap.Poll(true, true, 160, 0));
    KeyHold hold;
    assert(!hold.Poll(true, true, 100, 3000));
    assert(!hold.Poll(true, true, 3099, 3000));
    assert(hold.Poll(true, true, 3100, 3000));
    assert(!hold.Poll(true, true, 3120, 3000));
    hold.Poll(false, true, 3140, 3000);
    assert(!hold.Poll(true, true, 3200, 3000));
    // Adding a required modifier during the same hold remains valid.
    KeyHold mods;
    assert(!mods.Poll(true, false, 100, 0));
    assert(mods.Poll(true, true, 120, 0));
    assert(!mods.Poll(true, true, 140, 0));
    std::cout << "Independent detection and hotkey tests passed\n";
}

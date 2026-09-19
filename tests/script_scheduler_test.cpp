#include "script/script_scheduler.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    ScriptScheduler scheduler;
    std::vector<int> fired;
    const int first = scheduler.schedule(10.0, 1.0, "", "first", {});
    const int sameTime = scheduler.schedule(10.0, 1.0, "", "same", {});
    const int owned = scheduler.schedule(10.0, 0.5, "Transient", "owned", {});

    assert(scheduler.pending(first));
    assert(scheduler.cancelForObject("Transient") == 1);
    assert(!scheduler.pending(owned));
    assert(scheduler.advance(10.99, [&](const auto& event) { fired.push_back(event.id); }) == 0);
    assert(scheduler.advance(11.0, [&](const auto& event) { fired.push_back(event.id); }) == 2);
    assert((fired == std::vector<int>{first, sameTime}));
    assert(!scheduler.pending(first));
    assert(!scheduler.cancel(first));
    scheduler.clear();
    assert(scheduler.size() == 0);

    const int canceller = scheduler.schedule(20.0, 0.0, "Mission", "canceller", {});
    const int cancelled = scheduler.schedule(20.0, 0.0, "Mission", "cancelled", {});
    std::vector<int> reentrant;
    assert(scheduler.advance(20.0, [&](const auto& event) {
        reentrant.push_back(event.id);
        if (event.id == canceller) assert(scheduler.cancel(cancelled));
    }) == 1);
    assert(reentrant == std::vector<int>{canceller});
    assert(!scheduler.pending(cancelled) && !scheduler.pending(canceller));
    return 0;
}

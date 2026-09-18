#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Script schedules are simulation callbacks, not GUI work.
class ScriptScheduler {
public:
    struct Event {
        int id = 0;
        double due = 0.0;
        std::string object;
        std::string command;
        std::vector<std::string> args;
    };

    int schedule(double now, double delay, std::string object,
                 std::string command, std::vector<std::string> args) {
        Event event{nextId++, now + std::max(0.0, delay), std::move(object),
                    std::move(command), std::move(args)};
        events.push_back(std::move(event));
        return events.back().id;
    }
    bool cancel(int id) {
        auto it = std::find_if(events.begin(), events.end(),
            [id](const Event& event) { return event.id == id; });
        if (it == events.end()) return false;
        events.erase(it);
        return true;
    }
    size_t cancelForObject(const std::string& object) {
        const size_t oldSize = events.size();
        events.erase(std::remove_if(events.begin(), events.end(),
            [&object](const Event& event) { return event.object == object; }), events.end());
        return oldSize - events.size();
    }
    bool pending(int id) const {
        return std::any_of(events.begin(), events.end(),
            [id](const Event& event) { return event.id == id; });
    }
    size_t advance(double now, const std::function<void(const Event&)>& execute) {
        size_t count = 0;
        for (;;) {
            auto it = std::min_element(events.begin(), events.end(),
                [](const Event& a, const Event& b) {
                    return a.due < b.due || (a.due == b.due && a.id < b.id);
                });
            if (it == events.end() || it->due > now) break;
            Event event = std::move(*it);
            events.erase(it);
            execute(event);
            ++count;
        }
        return count;
    }
    void clear() { events.clear(); }
    size_t size() const { return events.size(); }

private:
    int nextId = 1;
    std::vector<Event> events;
};

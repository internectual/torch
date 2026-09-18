#pragma once

#include <algorithm>
#include <cstdint>

namespace MatchRuntime {

constexpr int MatchDurationMs = 20 * 60 * 1000;

inline int balancedTeam(int redPlayers, int bluePlayers) {
    return redPlayers <= bluePlayers ? 1 : 2;
}

struct Clock {
    bool started = false;
    bool ended = false;
    int remainingMs = MatchDurationMs;

    void reset() {
        started = false;
        ended = false;
        remainingMs = MatchDurationMs;
    }

    void start() {
        if (!started && !ended) started = true;
    }

    bool tick(int elapsedMs) {
        if (!started || ended || elapsedMs <= 0) return false;
        const int previous = remainingMs;
        remainingMs = std::max(0, remainingMs - elapsedMs);
        if (remainingMs == 0) ended = true;
        return previous != remainingMs;
    }

    void finish() {
        if (started) ended = true;
    }
};

inline bool scoreKill(Clock& clock, int& playerScore, int& teamScore,
                     bool teamMode, int scoreLimit) {
    if (clock.ended || scoreLimit < 1) return false;
    ++playerScore;
    if (teamMode) ++teamScore;
    if ((teamMode ? teamScore : playerScore) >= scoreLimit) {
        clock.finish();
        return true;
    }
    return false;
}

} // namespace MatchRuntime

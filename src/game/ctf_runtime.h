#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <string>

namespace CtfRuntime {
constexpr double DroppedFlagReturnSeconds = 30.0;
enum class FlagState { Home, Held, Dropped };
enum class StatusToken { Home, Field, Held };

// Server message text is not guaranteed to preserve the capitalization,
// whitespace, or angle brackets used by the stock scripts.
inline StatusToken classifyStatusToken(const std::string& status) {
    std::string normalized;
    normalized.reserve(status.size());
    for (char c : status) {
        if (c == '<' || c == '>') continue;
        if (!std::isspace(static_cast<unsigned char>(c)))
            normalized.push_back((char)std::tolower(static_cast<unsigned char>(c)));
    }
    // Servers and mission scripts use both the stock phrase and its shorter
    // status-token forms. They all describe a flag that is back at its stand.
    // The stock HUD sometimes receives a localized/script phrase rather than
    // the short token, for example "Flag at Base" or "Your flag returned".
    // Match the stable status words after whitespace/tag normalization so
    // those messages do not appear as a held flag.
    if (normalized.empty() || normalized == "atbase" || normalized == "base" ||
        normalized == "home" || normalized == "returned" || normalized == "return" ||
        normalized.find("atbase") != std::string::npos ||
        normalized.find("returned") != std::string::npos)
        return StatusToken::Home;
    if (normalized == "inthefield" || normalized == "field" || normalized == "dropped" ||
        normalized.find("inthefield") != std::string::npos ||
        normalized.find("dropped") != std::string::npos)
        return StatusToken::Field;
    return StatusToken::Held;
}
struct Flag {
    int team = 0;
    FlagState state = FlagState::Home;
    int carrier = -1;
    double returnTimer = 0.0;
};

struct Match {
    int scoreLimit = 5;
    int score[3]{};
    bool started = false;
    bool ended = false;
    int64_t clockMs = 0;
    void reset(int64_t durationMs) {
        score[0] = score[1] = score[2] = 0; started = false; ended = false;
        clockMs = std::max<int64_t>(0, durationMs);
    }
    void start() {
        started = true;
        ended = clockMs <= 0;
    }
    bool tick(int64_t elapsedMs) {
        if (!started || ended) return false;
        clockMs = std::max<int64_t>(0, clockMs - std::max<int64_t>(0, elapsedMs));
        if (clockMs == 0) ended = true;
        return ended;
    }
    bool capture(int team) {
        if (!started || ended || scoreLimit < 1 || team < 1 || team > 2) return false;
        if (++score[team] >= scoreLimit) ended = true;
        return true;
    }
};

// Mission object names are resolved case-insensitively by Torque. Keep the
// fallback used for local flag labels consistent with that lookup behavior.
inline int missionFlagTeam(const std::string& objectName) {
    std::string normalized = objectName;
    for (char& c : normalized)
        c = (char)std::tolower(static_cast<unsigned char>(c));
    return normalized.find("team2") != std::string::npos ? 2 : 1;
}

inline bool take(Flag& flag, int playerTeam, int player) {
    // Unassigned/observer clients must not interact with team flags.  Without
    // this check a team-0 player could steal either flag in a live match.
    if (playerTeam < 1 || playerTeam > 2 || flag.team < 1 || flag.team > 2 ||
        flag.state == FlagState::Held || flag.team == playerTeam || player < 0)
        return false;
    flag.state = FlagState::Held;
    flag.carrier = player;
    // The return countdown only exists while the flag is unattended. Keeping
    // it after pickup lets stale local state return a carried flag early.
    flag.returnTimer = 0.0;
    return true;
}
inline bool drop(Flag& flag, int player) {
    if (flag.state != FlagState::Held || flag.carrier != player) return false;
    flag.state = FlagState::Dropped;
    flag.carrier = -1;
    flag.returnTimer = DroppedFlagReturnSeconds;
    return true;
}
inline bool returnHome(Flag& flag, int playerTeam) {
    // Team zero is the observer/unassigned team, not a playable CTF team.
    // Reject it explicitly so an unassigned flag can never be returned by an
    // observer or by malformed mission state.
    if (playerTeam < 1 || playerTeam > 2 || flag.team < 1 || flag.team > 2 ||
        flag.state != FlagState::Dropped || flag.team != playerTeam) return false;
    // A dropped flag normally has no carrier, but clear stale replicated
    // state as well so the HUD cannot resurrect the previous carrier when the
    // flag returns home.
    flag.state = FlagState::Home;
    flag.carrier = -1;
    flag.returnTimer = 0.0;
    return true;
}

// Tribes 2 returns an unattended dropped flag after thirty seconds. Keep this
// local state separate from network fields so the wire representation is unchanged.
inline bool tickFlag(Flag& flag, double elapsedSeconds) {
    // A malformed frame delta must not turn the local return deadline into
    // NaN.  That would leave a dropped flag in limbo because every later
    // comparison with the timer is false.
    if (flag.state != FlagState::Dropped || !std::isfinite(elapsedSeconds) ||
        elapsedSeconds <= 0.0) return false;
    // A dropped flag reconstructed from a ghost has no local timer because the
    // return deadline is not a wire field.  Treat the missing value as a fresh
    // drop instead of returning it on the first simulation tick.
    // A malformed local timer must not strand the flag.  The deadline is not
    // replicated, so reconstruct the native dropped-state timeout just as we
    // do when a fresh ghost has no local deadline.
    if (!std::isfinite(flag.returnTimer) || flag.returnTimer <= 0.0)
        flag.returnTimer = DroppedFlagReturnSeconds;
    flag.returnTimer = std::max(0.0, flag.returnTimer - elapsedSeconds);
    if (flag.returnTimer > 1.0e-9) return false;
    flag.state = FlagState::Home;
    flag.carrier = -1;
    return true;
}
inline bool canCapture(const Flag& carrierFlag, const Flag& homeFlag,
                       int carrierTeam, int carrier) {
    // A held flag with the sentinel carrier id is incomplete replicated state,
    // not a capturable flag. Accepting it lets an unresolved ghost score a
    // capture before the carrier update arrives.
    return carrier >= 0 && carrierFlag.state == FlagState::Held &&
           carrierFlag.carrier == carrier &&
           carrierTeam >= 1 && carrierTeam <= 2 && carrierFlag.team != carrierTeam &&
           homeFlag.team == carrierTeam &&
           homeFlag.state == FlagState::Home;
}
}

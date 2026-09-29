#pragma once
#include <cstdint>
// Engine-side server state the retail server scripts toggle.

namespace SimState {

struct Server {
    bool allowConnections = false;      // allowConnections(bool)
    bool cyclingConnectionsDisabled = false; // disableCyclingConnections(bool)
    bool heartbeat = false;             // startHeartbeat / stopHeartbeat
    float gravity = -20.0f;             // Player::mGravity / Item::mGravity
    bool aiSystemEnabled = false;       // gAISystemEnabled
    uint64_t timeMs = 0;                // server sim time, 32 ms per tick
};

// gServerProcessList: GameBase objects advance in fixed 32 ms ticks.
constexpr double TickSeconds = 0.032;
void advanceServer(double now);

// Sim::getCurrentTime (seconds): advanced once a frame by the elapsed real
// time, capped at 1024 ms (TribesGame::processTimeEvent). Scheduled events
// and the server's ticks run on it.
double simTime();
void advanceSimTime(double realElapsed);

inline Server& server() {
    static Server state;
    return state;
}

} // namespace SimState

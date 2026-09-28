#pragma once
// Engine-side server state the retail server scripts toggle.

namespace SimState {

struct Server {
    bool allowConnections = false;      // allowConnections(bool)
    bool cyclingConnectionsDisabled = false; // disableCyclingConnections(bool)
    bool heartbeat = false;             // startHeartbeat / stopHeartbeat
    float gravity = -20.0f;             // Player::mGravity / Item::mGravity
};

inline Server& server() {
    static Server state;
    return state;
}

} // namespace SimState

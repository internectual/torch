#pragma once
// The engine's UDP network interface (game/netDispatch.cc, sim/netConnection.cc
// timeouts, game/serverQuery.cc server answers): Net::openPort through
// setNetPort, the connect handshake on both sides (challenge, connect
// request, accept/reject, disconnect), per-connection packet routing by
// source address, and the ping/info replies a hosting server sends.

class TorqueScript;

void registerNetInterfaceNatives(TorqueScript& ts);
// TribesGame::processPacketReceiveEvent for every waiting datagram, then
// dispatchCheckTimeouts.
void netInterfaceProcess(double now);

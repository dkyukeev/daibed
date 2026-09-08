#pragma once

#include "NetTypes.h"
#include "NetworkSnapshot.h"
#include "PlayerCommand.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Real socket/provider transport for multiplayer — see
// docs/P2P_IMPLEMENTATION.md. This layer moves
// bytes off the heap and over UDP, but it is deliberately minimal: connect /
// disconnect / send command / receive snapshot / timeout. No reliability, no
// ordering, no production hardening yet.
//
// Transport choice: native UDP via the OS socket API (Winsock2 / BSD sockets).
// ENet was considered but NOT used so the first transport adds no fetched
// external dependency — only the system socket library (ws2_32 on Windows),
// gated by the CMake option DAIBED_ENABLE_NETWORK. When that option is OFF the
// build defines DAIBED_HAVE_NETWORK=0 and these classes compile to safe stubs
// that report "transport unavailable" instead of opening sockets.
//
// Serialization reuses NetworkProtocol (the wire packets); this layer only
// frames connect/disconnect and ships the encoded command/snapshot bytes.

// True when the build included the real socket transport.
bool NetworkTransportAvailable();

// Common transport lifecycle. ServerTransport and ClientTransport both satisfy
// it; a future ENet/Steam transport can implement the same interface.
class INetworkTransport
{
public:
    virtual ~INetworkTransport() = default;
    virtual bool IsOpen() const = 0;
    virtual void Close() = 0;
    virtual const std::string& LastError() const = 0;
};

// A command received by the server, tagged with the client it came from.
struct ReceivedCommand
{
    int clientId = -1;
    PlayerCommand command;
};

// A client that has left (graceful disconnect or timeout). playerId is the
// in-match player it had been assigned, so the server can update slot policy.
struct DisconnectedClient
{
    int clientId = -1;
    int playerId = -1;
};

// A client that returned to a preserved in-match slot.
struct ReconnectedClient
{
    int clientId = -1;
    int playerId = -1;
};

// Authoritative-side endpoint: binds a UDP port (no window) and serves any
// number of clients. Each datagram is a NetworkProtocol packet.
class ServerTransport : public INetworkTransport
{
public:
    ServerTransport();
    ~ServerTransport() override;

    ServerTransport(const ServerTransport&) = delete;
    ServerTransport& operator=(const ServerTransport&) = delete;

    // Bind the UDP socket (non-blocking) to config.listenAddress:config.port.
    bool Start(const ServerConfig& config);

    bool IsOpen() const override;
    void Close() override;
    const std::string& LastError() const override;

    // Pump the socket once: validate Connect tokens against the configured
    // password (deny on mismatch), register accepted clients as pending,
    // queue PlayerCommands from assigned clients, refresh keepalives, drop
    // timed-out clients. Non-blocking — call it each server tick.
    void Poll();

    // --- Game-driven slot assignment ------------------------------------
    // Legacy assignment hook kept for local smokes and the Game server. In the
    // lobby flow clients are acknowledged immediately, then AssignPlayer binds
    // them to a match player only once the lobby starts.
    std::vector<int> TakePendingClients();
    void AssignPlayer(int clientId, int playerId);
    // Clients that left since the last call.
    std::vector<DisconnectedClient> TakeDisconnectedClients();
    // Clients that rejoined a preserved slot since the last call.
    std::vector<ReconnectedClient> TakeReconnectedClients();

    // Match join policy: once locked, brand-new late joins are denied. A client
    // that disconnected from an assigned slot can reclaim it only by presenting
    // the server-issued session credentials (the display name is not identity).
    void SetMatchJoinLocked(bool locked);

    // --- Pre-match lobby -------------------------------------------------
    std::vector<LobbyPlayerState> LobbyPlayers() const;
    LobbyValidation ValidateLobbyStart() const;
    bool LobbyStartRequested() const;
    LobbySnapshot BuildLobbySnapshot(bool matchStarting = false, bool matchStarted = false,
                                     const std::string& statusMessage = "") const;
    void BroadcastLobbySnapshot(const LobbySnapshot& snapshot);
    void SendLobbySnapshotToClient(int clientId, const LobbySnapshot& snapshot);

    // Encode + send a snapshot to every connected client.
    void BroadcastSnapshot(const MatchSnapshot& snapshot);
    // Encode + send a snapshot to a single client (for per-client views).
    void SendSnapshotToClient(int clientId, const MatchSnapshot& snapshot);
    // True when SendSnapshotToClient would consume a freshly-built snapshot now.
    bool NeedsSnapshotForClient(int clientId) const;

    // Commands received since the last drain (each tagged with its clientId).
    std::vector<ReceivedCommand> DrainCommands();

    std::size_t ClientCount() const;
    std::uint16_t BoundPort() const;
    std::uint32_t PacketsReceived() const;
    std::uint32_t PacketsSent() const;
    std::uint64_t BytesReceived() const;
    std::uint64_t BytesSent() const;
    std::uint32_t StaleCommandsDropped() const;
    std::uint32_t FullSnapshotsSent() const;
    std::uint32_t DeltaSnapshotsSent() const;
    std::uint32_t ResyncRequestsReceived() const;
    std::size_t LastFullSnapshotBytes() const;
    std::size_t LastDeltaSnapshotBytes() const;
    std::uint32_t LastProcessedCommandTick(int clientId) const;
    void AdvanceProcessedCommandTick(int clientId, std::uint32_t tick);

    // Player a client was assigned (-1 if unknown / not yet assigned).
    int PlayerForClient(int clientId) const;
    // clientIds of all assigned (acked) clients, for per-client snapshot sends.
    std::vector<int> ConnectedClients() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Client-side endpoint: opens a UDP socket toward a server and exchanges
// commands for snapshots.
class ClientTransport : public INetworkTransport
{
public:
    explicit ClientTransport(NetworkBackend backend = NetworkBackend::SystemUdp);
    ~ClientTransport() override;

    ClientTransport(const ClientTransport&) = delete;
    ClientTransport& operator=(const ClientTransport&) = delete;

    // Create the socket and remember the server endpoint, then send a Connect
    // carrying the join token (server password; empty if none). Returns false
    // only on a setup failure (bad host, socket error) — it does NOT block
    // waiting for the server, so a dead server is not an error here; connection
    // completes asynchronously via Poll() -> IsConnected() (or WasDenied()).
    bool Open(const std::string& host, std::uint16_t port, const std::string& token = "",
              float timeoutSeconds = 5.0f,
              const SessionCredentials& resumeCredentials = {});

    // Convenience for standalone clients (e.g. --connect): Open(), then pump for
    // up to timeoutSeconds. Returns whether the handshake completed. A dead /
    // unreachable server returns false; a wrong password returns false with
    // WasDenied()==true. It never throws or crashes.
    bool Connect(const std::string& host, std::uint16_t port, const std::string& token = "",
                 float timeoutSeconds = 5.0f,
                 const SessionCredentials& resumeCredentials = {});

    bool IsOpen() const override;
    void Close() override;
    const std::string& LastError() const override;

    // Pump the socket once: complete the handshake, store the latest snapshot,
    // refresh the server keepalive, detect timeout. Non-blocking.
    void Poll();

    // Encode + send a command to the server.
    void SendCommand(const PlayerCommand& command);
    // Encode + send pre-match lobby state to the server.
    void SendLobbyUpdate(const LobbyUpdate& update);

    // Politely tell the server we are leaving, then close.
    void Disconnect();

    bool IsConnected() const;
    bool TimedOut() const;
    bool WasDenied() const;
    const std::string& DenyReason() const;
    int LobbyClientId() const;
    const SessionCredentials& Credentials() const;
    int AssignedPlayerId() const;
    bool InMatch() const;
    bool HasLobbySnapshot() const;
    const LobbySnapshot& LatestLobbySnapshot() const;
    bool HasSnapshot() const;
    const MatchSnapshot& LatestSnapshot() const;
    std::uint32_t PacketsReceived() const;
    std::uint32_t PacketsSent() const;
    std::uint64_t BytesReceived() const;
    std::uint64_t BytesSent() const;
    float BytesPerSecond() const;
    float PacketsPerSecond() const;
    std::uint32_t DroppedSnapshots() const;
    std::uint32_t IgnoredSnapshots() const;
    std::uint32_t FullSnapshotsReceived() const;
    std::uint32_t DeltaSnapshotsReceived() const;
    std::uint32_t ResyncRequestsSent() const;
    std::size_t LastFullSnapshotBytes() const;
    std::size_t LastDeltaSnapshotBytes() const;
    std::uint32_t LastAckedCommandTick() const;
    std::size_t PendingCommandCount() const;
    float SnapshotAgeSeconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Headless self-test (CLI: --localhost-net-smoke). Starts a server and a client
// in one process over real loopback UDP, has the client connect, the server
// serve advancing snapshots, and verifies the client actually receives one.
// Also checks that a connect to a dead port fails gracefully. Returns a process
// exit code (0 == success).
int RunLocalhostNetSmoke(const ServerConfig& config);
// Runs the complete handshake/snapshot protocol over opaque in-process peer
// handles. This is the provider-contract test before an external P2P SDK is
// selected and linked.
int RunDatagramBackendSmoke();

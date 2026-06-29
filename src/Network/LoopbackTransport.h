#pragma once

#include "NetTypes.h"
#include "NetworkSnapshot.h"
#include "PlayerCommand.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// In-process "server + N clients" mock, with no sockets. Where LocalServerSession
// models a single command-in / snapshot-out channel, LoopbackTransport models
// several clients sharing one authoritative server: each client is bound to the
// player it controls, the server drains everyone's commands, and it publishes a
// per-client snapshot. A real transport replaces this class while keeping the
// same shape (connect -> submit -> drain -> publish -> read). See
// docs/NETWORK_PREP_PLAN.md.
//
// clientId <-> playerId mapping is authoritative here: a client may only command
// the player it owns. SubmitCommand stamps the command's controlledPlayerId from
// the mapping, so a client can never move (or even address) another player — two
// clients cannot control the same player, and the same player cannot be bound to
// two clients (Connect rejects it).
class LoopbackTransport
{
public:
    void Configure(const ServerConfig& config);
    const ServerConfig& Config() const;

    void Start();
    void Stop();
    bool IsRunning() const;

    // --- Client lifecycle / mapping -------------------------------------
    // Bind a client to the player it controls. Rejected (false) if the clientId
    // is already connected or the player is already owned by another client.
    bool Connect(int clientId, int playerId);
    void Disconnect(int clientId);
    bool IsConnected(int clientId) const;
    int PlayerForClient(int clientId) const; // -1 if the client is unknown
    int ClientForPlayer(int playerId) const; // -1 if the player is unowned
    std::size_t ClientCount() const;

    // --- Client -> server -----------------------------------------------
    // Queue a command from a client. The transport stamps controlledPlayerId to
    // the client's mapped player (authoritative), so the command always targets
    // that player regardless of what the client put in the field. Returns false
    // if the client is not connected (or the transport is stopped).
    bool SubmitCommand(int clientId, const PlayerCommand& command);

    // --- Server ---------------------------------------------------------
    std::vector<PlayerCommand> DrainCommands();
    std::size_t PendingCommandCount() const;

    // --- Server -> per-client snapshots ---------------------------------
    void PublishSnapshot(int clientId, const MatchSnapshot& snapshot);
    // Latest snapshot delivered to a client; an empty snapshot if none yet / the
    // client is unknown.
    const MatchSnapshot& LatestSnapshot(int clientId) const;
    bool HasSnapshot(int clientId) const;
    std::uint32_t SnapshotsPublished() const; // total across all clients

private:
    struct ClientChannel
    {
        int clientId = -1;
        int playerId = -1;
        MatchSnapshot latestSnapshot;
        bool hasSnapshot = false;
    };

    ClientChannel* FindClient(int clientId);
    const ClientChannel* FindClient(int clientId) const;

    ServerConfig config_;
    bool running_ = false;
    std::uint32_t snapshotsPublished_ = 0;
    std::vector<ClientChannel> clients_;
    std::vector<PlayerCommand> commandQueue_;
    MatchSnapshot emptySnapshot_; // returned for unknown / not-yet-published clients
};

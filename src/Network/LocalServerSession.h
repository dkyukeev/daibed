#pragma once

#include "NetTypes.h"
#include "NetworkSnapshot.h"
#include "PlayerCommand.h"

#include <cstddef>
#include <vector>

// In-process transport/session mock. It models the direction the real
// transport will flow without any sockets:
//
//   client -> SubmitCommand(PlayerCommand)   // queue for the server
//   server -> DrainCommands()                // forward into MatchSimulation
//   server -> PublishSnapshot(MatchSnapshot)
//   client -> LatestSnapshot()
//
// The authoritative simulation tick lives in MatchSimulation (single source of
// truth), not here — this class is only the transport/snapshot channel. A real
// ENet/Steam/UDP transport replaces it while keeping the same command-in /
// snapshot-out shape, so Game/simulation code does not change when networking
// arrives. See docs/NETWORK_PREP_PLAN.md.
class LocalServerSession
{
public:
    void Configure(const ServerConfig& config);
    const ServerConfig& Config() const;

    void Start();
    void Stop();
    bool IsRunning() const;

    // Client side: queue a command for the authoritative server.
    void SubmitCommand(const PlayerCommand& command);

    // Server side: take ownership of all queued commands for this tick.
    std::vector<PlayerCommand> DrainCommands();
    std::size_t PendingCommandCount() const;

    // Server -> client snapshot exchange.
    void PublishSnapshot(const MatchSnapshot& snapshot);
    const MatchSnapshot& LatestSnapshot() const;
    std::uint32_t SnapshotsPublished() const;

private:
    ServerConfig config_;
    bool running_ = false;
    std::uint32_t snapshotsPublished_ = 0;
    std::vector<PlayerCommand> commandQueue_;
    MatchSnapshot latestSnapshot_;
};

#include "Network/LoopbackTransport.h"

#include <utility>

void LoopbackTransport::Configure(const ServerConfig& config)
{
    config_ = config;
}

const ServerConfig& LoopbackTransport::Config() const
{
    return config_;
}

void LoopbackTransport::Start()
{
    running_ = true;
    snapshotsPublished_ = 0;
    clients_.clear();
    commandQueue_.clear();
}

void LoopbackTransport::Stop()
{
    running_ = false;
    clients_.clear();
    commandQueue_.clear();
}

bool LoopbackTransport::IsRunning() const
{
    return running_;
}

LoopbackTransport::ClientChannel* LoopbackTransport::FindClient(int clientId)
{
    for (ClientChannel& client : clients_)
    {
        if (client.clientId == clientId)
        {
            return &client;
        }
    }
    return nullptr;
}

const LoopbackTransport::ClientChannel* LoopbackTransport::FindClient(int clientId) const
{
    for (const ClientChannel& client : clients_)
    {
        if (client.clientId == clientId)
        {
            return &client;
        }
    }
    return nullptr;
}

bool LoopbackTransport::Connect(int clientId, int playerId)
{
    if (!running_)
    {
        return false;
    }
    // No duplicate clients, and no two clients on the same player.
    if (FindClient(clientId) != nullptr || ClientForPlayer(playerId) != -1)
    {
        return false;
    }
    ClientChannel channel;
    channel.clientId = clientId;
    channel.playerId = playerId;
    clients_.push_back(std::move(channel));
    return true;
}

void LoopbackTransport::Disconnect(int clientId)
{
    for (std::size_t i = 0; i < clients_.size(); ++i)
    {
        if (clients_[i].clientId == clientId)
        {
            clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

bool LoopbackTransport::IsConnected(int clientId) const
{
    return FindClient(clientId) != nullptr;
}

int LoopbackTransport::PlayerForClient(int clientId) const
{
    const ClientChannel* client = FindClient(clientId);
    return client != nullptr ? client->playerId : -1;
}

int LoopbackTransport::ClientForPlayer(int playerId) const
{
    for (const ClientChannel& client : clients_)
    {
        if (client.playerId == playerId)
        {
            return client.clientId;
        }
    }
    return -1;
}

std::size_t LoopbackTransport::ClientCount() const
{
    return clients_.size();
}

bool LoopbackTransport::SubmitCommand(int clientId, const PlayerCommand& command)
{
    if (!running_)
    {
        return false;
    }
    const ClientChannel* client = FindClient(clientId);
    if (client == nullptr)
    {
        return false;
    }
    // Authoritative mapping: the command always targets the client's own player,
    // never one it doesn't own. This is what stops two clients from driving the
    // same player even if a client lies about controlledPlayerId.
    PlayerCommand stamped = command;
    stamped.controlledPlayerId = static_cast<std::uint32_t>(client->playerId);
    commandQueue_.push_back(stamped);
    return true;
}

std::vector<PlayerCommand> LoopbackTransport::DrainCommands()
{
    std::vector<PlayerCommand> commands;
    commands.swap(commandQueue_);
    return commands;
}

std::size_t LoopbackTransport::PendingCommandCount() const
{
    return commandQueue_.size();
}

void LoopbackTransport::PublishSnapshot(int clientId, const MatchSnapshot& snapshot)
{
    if (!running_)
    {
        return;
    }
    ClientChannel* client = FindClient(clientId);
    if (client == nullptr)
    {
        return;
    }
    client->latestSnapshot = snapshot;
    client->hasSnapshot = true;
    ++snapshotsPublished_;
}

const MatchSnapshot& LoopbackTransport::LatestSnapshot(int clientId) const
{
    const ClientChannel* client = FindClient(clientId);
    return client != nullptr ? client->latestSnapshot : emptySnapshot_;
}

bool LoopbackTransport::HasSnapshot(int clientId) const
{
    const ClientChannel* client = FindClient(clientId);
    return client != nullptr && client->hasSnapshot;
}

std::uint32_t LoopbackTransport::SnapshotsPublished() const
{
    return snapshotsPublished_;
}

#include "Network/LocalServerSession.h"

#include <utility>

void LocalServerSession::Configure(const ServerConfig& config)
{
    config_ = config;
}

const ServerConfig& LocalServerSession::Config() const
{
    return config_;
}

void LocalServerSession::Start()
{
    running_ = true;
    snapshotsPublished_ = 0;
    commandQueue_.clear();
    latestSnapshot_ = MatchSnapshot {};
}

void LocalServerSession::Stop()
{
    running_ = false;
    commandQueue_.clear();
}

bool LocalServerSession::IsRunning() const
{
    return running_;
}

void LocalServerSession::SubmitCommand(const PlayerCommand& command)
{
    if (!running_)
    {
        return;
    }

    commandQueue_.push_back(command);
}

std::vector<PlayerCommand> LocalServerSession::DrainCommands()
{
    std::vector<PlayerCommand> commands;
    commands.swap(commandQueue_);
    return commands;
}

std::size_t LocalServerSession::PendingCommandCount() const
{
    return commandQueue_.size();
}

void LocalServerSession::PublishSnapshot(const MatchSnapshot& snapshot)
{
    if (!running_)
    {
        return;
    }

    latestSnapshot_ = snapshot;
    ++snapshotsPublished_;
}

const MatchSnapshot& LocalServerSession::LatestSnapshot() const
{
    return latestSnapshot_;
}

std::uint32_t LocalServerSession::SnapshotsPublished() const
{
    return snapshotsPublished_;
}

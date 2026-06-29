#include "Simulation/MatchSimulation.h"

#include "Player.h" // complete type for the players access methods (.cpp only)

#include <algorithm>

namespace
{
constexpr std::size_t kMaxBufferedBlockDeltas = 4096;

int SanitizeTickRate(int tickRate)
{
    return tickRate > 0 ? tickRate : 60;
}
}

MatchSimulation::MatchSimulation(int tickRate)
    : tickRate_(SanitizeTickRate(tickRate)),
      fixedDt_(1.0f / static_cast<float>(SanitizeTickRate(tickRate)))
{
}

void MatchSimulation::Reset()
{
    tick_ = 0;
    matchTime_ = 0.0f;
    phase_ = MatchPhase::Lobby;
    winner_.reset();
    commandQueue_.clear();
    blockDeltas_.clear();
}

MatchPhase MatchSimulation::Phase() const
{
    return phase_;
}

void MatchSimulation::SetPhase(MatchPhase phase)
{
    phase_ = phase;
}

bool MatchSimulation::HasWinner() const
{
    return winner_.has_value();
}

std::optional<int> MatchSimulation::Winner() const
{
    return winner_;
}

int MatchSimulation::WinnerTeamId() const
{
    return winner_.value_or(-1);
}

void MatchSimulation::SetWinner(std::optional<int> winner)
{
    winner_ = winner;
    if (winner.has_value())
    {
        phase_ = MatchPhase::Finished;
    }
}

void MatchSimulation::AdvanceTick()
{
    ++tick_;
}

std::uint32_t MatchSimulation::CurrentTick() const
{
    return tick_;
}

void MatchSimulation::AdvanceClock(float dt)
{
    matchTime_ += dt;
}

float MatchSimulation::MatchTimeSeconds() const
{
    return matchTime_;
}

void MatchSimulation::SetMatchTimeSeconds(float seconds)
{
    matchTime_ = seconds;
}

int MatchSimulation::TickRate() const
{
    return tickRate_;
}

float MatchSimulation::FixedDeltaSeconds() const
{
    return fixedDt_;
}

void MatchSimulation::SubmitCommand(const PlayerCommand& command)
{
    commandQueue_.push_back(command);
}

std::vector<PlayerCommand> MatchSimulation::DrainCommands()
{
    std::vector<PlayerCommand> drained;
    drained.swap(commandQueue_);
    return drained;
}

std::size_t MatchSimulation::PendingCommandCount() const
{
    return commandQueue_.size();
}

void MatchSimulation::ResetGenerators()
{
    generators_.clear();
}

std::vector<Generator>& MatchSimulation::Generators()
{
    return generators_;
}

const std::vector<Generator>& MatchSimulation::Generators() const
{
    return generators_;
}

void MatchSimulation::ResetPickups()
{
    pickups_.clear();
}

std::vector<ResourcePickup>& MatchSimulation::Pickups()
{
    return pickups_;
}

const std::vector<ResourcePickup>& MatchSimulation::Pickups() const
{
    return pickups_;
}

void MatchSimulation::ResetDroppedItems()
{
    droppedItems_.clear();
}

std::vector<DroppedItem>& MatchSimulation::DroppedItems()
{
    return droppedItems_;
}

const std::vector<DroppedItem>& MatchSimulation::DroppedItems() const
{
    return droppedItems_;
}

void MatchSimulation::ResetCores()
{
    cores_.clear();
}

std::vector<EnergyCore>& MatchSimulation::Cores()
{
    return cores_;
}

const std::vector<EnergyCore>& MatchSimulation::Cores() const
{
    return cores_;
}

void MatchSimulation::RecordBlockDelta(const BlockDelta& delta)
{
    blockDeltas_.push_back(delta);
    if (blockDeltas_.size() > kMaxBufferedBlockDeltas)
    {
        const std::size_t overflow = blockDeltas_.size() - kMaxBufferedBlockDeltas;
        blockDeltas_.erase(blockDeltas_.begin(), blockDeltas_.begin() + static_cast<std::ptrdiff_t>(overflow));
    }
}

const std::vector<BlockDelta>& MatchSimulation::BlockDeltas() const
{
    return blockDeltas_;
}

void MatchSimulation::ClearBlockDeltasThrough(std::uint32_t tick)
{
    blockDeltas_.erase(
        std::remove_if(
            blockDeltas_.begin(),
            blockDeltas_.end(),
            [tick](const BlockDelta& delta)
            {
                return delta.tick <= tick;
            }),
        blockDeltas_.end());
}

void MatchSimulation::ResetBlockDeltas()
{
    blockDeltas_.clear();
}

void MatchSimulation::SetPlayers(std::vector<Player>* players)
{
    players_ = players;
}

std::vector<Player>& MatchSimulation::Players()
{
    return *players_;
}

const std::vector<Player>& MatchSimulation::Players() const
{
    return *players_;
}

Player* MatchSimulation::GetPlayer(int id)
{
    if (players_ == nullptr)
    {
        return nullptr;
    }
    for (Player& player : *players_)
    {
        if (player.GetId() == id)
        {
            return &player;
        }
    }
    return nullptr;
}

const Player* MatchSimulation::GetPlayer(int id) const
{
    if (players_ == nullptr)
    {
        return nullptr;
    }
    for (const Player& player : *players_)
    {
        if (player.GetId() == id)
        {
            return &player;
        }
    }
    return nullptr;
}

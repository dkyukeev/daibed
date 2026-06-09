#include "Core.h"

#include <algorithm>

EnergyCore::EnergyCore(int teamId, GridPos blockPosition, int health)
    : teamId_(teamId),
      blockPosition_(blockPosition),
      maxHealth_(health),
      health_(health),
      alive_(true)
{
}

int EnergyCore::GetTeamId() const
{
    return teamId_;
}

GridPos EnergyCore::GetBlockPosition() const
{
    return blockPosition_;
}

int EnergyCore::GetHealth() const
{
    return health_;
}

int EnergyCore::GetMaxHealth() const
{
    return maxHealth_;
}

bool EnergyCore::IsAlive() const
{
    return alive_;
}

bool EnergyCore::Damage(int amount)
{
    if (!alive_)
    {
        return false;
    }

    health_ = std::max(0, health_ - std::max(0, amount));
    alive_ = health_ > 0;
    return !alive_;
}

void EnergyCore::Repair(int amount)
{
    if (!alive_)
    {
        return;
    }

    health_ = std::min(maxHealth_, health_ + std::max(0, amount));
}

void EnergyCore::SetHealth(int health)
{
    health_ = std::clamp(health, 0, maxHealth_);
    alive_ = health_ > 0;
}

void EnergyCore::Restore()
{
    health_ = maxHealth_;
    alive_ = true;
}

#pragma once

#include "Block.h"

class EnergyCore
{
public:
    EnergyCore() = default;
    EnergyCore(int teamId, GridPos blockPosition, int health);

    int GetTeamId() const;
    GridPos GetBlockPosition() const;
    int GetHealth() const;
    int GetMaxHealth() const;
    bool IsAlive() const;

    bool Damage(int amount);
    void Repair(int amount);
    void Restore();

private:
    int teamId_ = -1;
    GridPos blockPosition_ {};
    int maxHealth_ = 80;
    int health_ = 80;
    bool alive_ = true;
};

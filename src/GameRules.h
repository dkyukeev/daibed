#pragma once

#include "Player.h"
#include "Team.h"

#include <optional>
#include <vector>

class GameRules
{
public:
    std::optional<int> CheckWinCondition(const std::vector<Team>& teams, const std::vector<Player>& players) const;
};


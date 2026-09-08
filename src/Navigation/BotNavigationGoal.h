#pragma once

#include "BotAI.h"
#include "Navigation/NavigationGoal.h"

// A travel segment takes precedence over interaction with the final objective.
// Only after the corridor is exhausted may the executor pursue the actor,
// defend the Core, or enter shop range.
struct BotNavigationDestination
{
    BotIntent intent = BotIntent::Recover;
    GridPos support {};
    bool intermediate = false;
    bool bridgeAssist = false;
    int targetPlayerId = -1;
    std::optional<GridPos> targetCore;
    std::optional<GridPos> ownCore;
};

NavigationGoalPtr BuildBotNavigationGoal(const BotNavigationDestination& destination);

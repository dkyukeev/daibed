#include "Navigation/BotNavigationGoal.h"

NavigationGoalPtr BuildBotNavigationGoal(const BotNavigationDestination& destination)
{
    const GridPos support = destination.support;
    if (destination.intermediate || destination.bridgeAssist)
    {
        return std::make_shared<GoalWithinRadius>(support, 0.8f);
    }
    switch (destination.intent)
    {
    case BotIntent::PressureCore:
    case BotIntent::BreakCoreDefense:
        if (destination.targetCore)
            return std::make_shared<GoalCoreAttackPosition>(*destination.targetCore, 4.2f, true);
        return std::make_shared<GoalWithinRadius>(support, 2.2f);
    case BotIntent::FightEnemy:
    case BotIntent::ChaseWeakEnemy:
        if (destination.targetPlayerId >= 0)
            return std::make_shared<GoalAttackRangeOfPlayer>(destination.targetPlayerId, 1.4f, 2.8f, true);
        break;
    case BotIntent::DefendCore:
        return std::make_shared<GoalDefensivePosition>(destination.ownCore.value_or(support), 1.0f, 5.5f, 20.0f);
    case BotIntent::GearUp:
        return std::make_shared<GoalShopPosition>(support, 2.6f);
    case BotIntent::SecureResources:
        return std::make_shared<GoalResourceGenerator>(support, 1.4f);
    case BotIntent::RetreatHome:
        return std::make_shared<GoalWithinRadius>(support, 1.8f);
    case BotIntent::RepairCoreDefense:
        // The existing defense builder owns the final interaction, but travel
        // to its base is handled by the same corridor as every other task.
        return nullptr;
    case BotIntent::Recover:
        break;
    }
    return std::make_shared<GoalWithinRadius>(support, 0.8f);
}

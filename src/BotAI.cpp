#include "BotAI.h"

const char* ToString(BotState state)
{
    switch (state)
    {
    case BotState::Collect:
        return "Collect";
    case BotState::Shop:
        return "Shop";
    case BotState::Bridge:
        return "Bridge";
    case BotState::BreakDefense:
        return "BreakDefense";
    case BotState::AttackCore:
        return "AttackCore";
    case BotState::Fight:
        return "Fight";
    case BotState::Retreat:
        return "Retreat";
    }

    return "Unknown";
}

const char* ToString(BotRole role)
{
    switch (role)
    {
    case BotRole::Defender:
        return "Defender";
    case BotRole::Rusher:
        return "Rusher";
    case BotRole::Collector:
        return "Collector";
    case BotRole::Fighter:
        return "Fighter";
    }

    return "Unknown";
}

const char* ToString(BotIntent intent)
{
    switch (intent)
    {
    case BotIntent::DefendCore:
        return "DefendCore";
    case BotIntent::RepairCoreDefense:
        return "RepairDefense";
    case BotIntent::GearUp:
        return "GearUp";
    case BotIntent::SecureResources:
        return "Resources";
    case BotIntent::PressureCore:
        return "PressureCore";
    case BotIntent::BreakCoreDefense:
        return "BreakDefense";
    case BotIntent::FightEnemy:
        return "Fight";
    case BotIntent::ChaseWeakEnemy:
        return "Chase";
    case BotIntent::RetreatHome:
        return "Retreat";
    case BotIntent::Recover:
        return "Recover";
    }

    return "Unknown";
}

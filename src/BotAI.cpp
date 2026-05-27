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

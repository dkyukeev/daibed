#pragma once

#include "BotAI.h"
#include <functional>

class Player;
using BotCombatVisibility = std::function<bool(const Player&, const Player&)>;

struct BotCombatPowerWeights
{
    float health = 1.05f;
    float sword = 17.0f;
    float armor = 14.0f;
    float shield = 13.0f;
    float tool = 8.0f;
    float fireball = 8.0f;
    float dash = 7.0f;
    float molotov = 6.0f;
    float speedBoost = 10.0f;
};

inline constexpr BotCombatPowerWeights kDecisionCombatPowerWeights {};
inline constexpr BotCombatPowerWeights kPathCombatPowerWeights {
    1.1f,
    18.0f,
    14.0f,
    13.0f,
    8.0f,
    7.0f,
    6.0f,
    5.0f,
    9.0f
};

struct FightAssessment
{
    float selfPower = 0.0f;
    float targetPower = 0.0f;
    float allyPower = 0.0f;
    float enemyPower = 0.0f;
    float powerMargin = 0.0f;
    float distance = 999.0f;
    int nearbyAllies = 0;
    int nearbyEnemies = 0;
    bool canWin = false;
    bool shouldFight = false;
    bool shouldRetreat = false;
};

BotCombatPowerWeights ApplyCombatTuning(BotCombatPowerWeights weights, const BotTuningGenome& tuning);
float BotCombatPowerScore(const Player& player, const BotCombatPowerWeights& weights);
float BotCombatPowerScore(const Player& player, const BotTuningGenome& tuning);

FightAssessment AssessFight(
    const Player& bot,
    const Player& target,
    const BotCombatVisibility& visible,
    const std::vector<Player*>* aliveAllies,
    const std::vector<Player*>* aliveEnemies,
    BotRole role,
    BotDifficulty difficulty,
    const BotTuningGenome& tuning,
    bool defendingCore,
    bool finalLifeTarget,
    bool objectiveBlocker);

#include "BotCombatAssessment.h"
#include "Player.h"
#include <algorithm>
#include <cmath>

namespace
{
float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float FightRequiredMarginForDifficulty(const BotTuningGenome& tuning, BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return tuning.fightRequiredMarginEasy;
    case BotDifficulty::Hard:
        return tuning.fightRequiredMarginHard;
    case BotDifficulty::Normal:
        break;
    }
    return tuning.fightRequiredMarginNormal;
}

float RetreatPowerMarginForDifficulty(const BotTuningGenome& tuning, BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return tuning.retreatPowerMarginEasy;
    case BotDifficulty::Hard:
        return tuning.retreatPowerMarginHard;
    case BotDifficulty::Normal:
        break;
    }
    return tuning.retreatPowerMarginNormal;
}

}

BotCombatPowerWeights ApplyCombatTuning(BotCombatPowerWeights weights, const BotTuningGenome& tuning)
{
    weights.shield = tuning.shieldPower;
    weights.speedBoost = tuning.speedBoostPower;
    return weights;
}

float BotCombatPowerScore(const Player& player, const BotCombatPowerWeights& weights)
{
    const Inventory& inventory = player.GetInventory();
    float score = static_cast<float>(player.GetHealth()) * weights.health;
    score += static_cast<float>(inventory.GetSwordLevel()) * weights.sword;
    score += static_cast<float>(inventory.GetArmorLevel()) * weights.armor;
    score += player.HasShield() ? weights.shield : 0.0f;
    score += static_cast<float>(inventory.GetToolLevel()) * weights.tool;
    score += static_cast<float>(inventory.GetUtility(UtilityType::Fireball)) * weights.fireball;
    score += static_cast<float>(inventory.GetUtility(UtilityType::Dash)) * weights.dash;
    score += static_cast<float>(inventory.GetUtility(UtilityType::Molotov)) * weights.molotov;
    score += player.GetSpeedBoostTimer() > 0.0f ? weights.speedBoost : 0.0f;
    return score;
}

float BotCombatPowerScore(const Player& player, const BotTuningGenome& tuning)
{
    return BotCombatPowerScore(player, ApplyCombatTuning(kDecisionCombatPowerWeights, tuning));
}

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
    bool objectiveBlocker)
{
    FightAssessment result {};
    result.selfPower = BotCombatPowerScore(bot, tuning);
    result.targetPower = BotCombatPowerScore(target, tuning);
    result.distance = std::sqrt(DistanceSquared(bot.GetPosition(), target.GetPosition()));

    // Hunting a final-life target is a pack activity: allies still closing in
    // should already count, otherwise hunters trickle in one by one and lose.
    const float allyAssistRangeSq = finalLifeTarget ? 16.0f * 16.0f : 9.5f * 9.5f;
    constexpr float enemyAssistRangeSq = 8.5f * 8.5f;
    if (aliveAllies != nullptr)
    {
        for (const Player* ally : *aliveAllies)
        {
            if (ally == nullptr
                || ally->GetId() == bot.GetId()
                || ally->GetTeamId() != bot.GetTeamId()
                || !ally->IsAlive()
                || ally->IsEliminated())
            {
                continue;
            }

            const bool closeToFight = DistanceSquared(ally->GetPosition(), target.GetPosition()) < allyAssistRangeSq
                && std::fabs(ally->GetPosition().y - target.GetPosition().y) <= 3.0f
                && visible(*ally, target);
            if (!closeToFight)
            {
                continue;
            }

            ++result.nearbyAllies;
            result.allyPower += BotCombatPowerScore(*ally, tuning) * tuning.allyAssistWeight;
        }
    }
    if (aliveEnemies != nullptr)
    {
        for (const Player* enemy : *aliveEnemies)
        {
            // Both callers may supply authoritative actors. Knowledge must be
            // checked here too, so path selection never reads hidden support.
            if (enemy == nullptr || enemy->GetId() == target.GetId()
                || enemy->GetTeamId() == bot.GetTeamId()
                || !enemy->IsAlive() || enemy->IsEliminated()
                || !visible(bot, *enemy)) continue;
            const bool closeToFight =
                DistanceSquared(enemy->GetPosition(), bot.GetPosition()) < enemyAssistRangeSq
                || DistanceSquared(enemy->GetPosition(), target.GetPosition()) < enemyAssistRangeSq;
            if (!closeToFight) continue;
            ++result.nearbyEnemies;
            result.enemyPower += BotCombatPowerScore(*enemy, tuning) * tuning.enemyAssistWeight;
        }
    }

    result.powerMargin = result.selfPower + result.allyPower - result.targetPower - result.enemyPower;

    float requiredMargin = FightRequiredMarginForDifficulty(tuning, difficulty);
    if (role == BotRole::Fighter)
    {
        requiredMargin -= 12.0f;
    }
    else if (role == BotRole::Collector)
    {
        requiredMargin += 14.0f;
    }
    else if (role == BotRole::Defender && defendingCore)
    {
        requiredMargin -= 18.0f;
    }
    if (finalLifeTarget)
    {
        requiredMargin -= 22.0f;
    }
    if (objectiveBlocker)
    {
        requiredMargin -= 10.0f;
    }
    if (bot.GetHealth() < 48)
    {
        requiredMargin += 18.0f;
    }
    const int outnumberedBy = std::max(0, result.nearbyEnemies - result.nearbyAllies);
    requiredMargin += static_cast<float>(outnumberedBy) * 16.0f;

    const bool targetWeak = target.GetHealth() <= bot.GetHealth() - (difficulty == BotDifficulty::Easy ? 34 : 18)
        && outnumberedBy < 2;
    result.canWin = result.powerMargin >= requiredMargin;
    result.shouldFight = defendingCore
        || finalLifeTarget
        || objectiveBlocker
        || targetWeak
        || result.canWin;
    result.shouldRetreat = !defendingCore
        && !finalLifeTarget
        && bot.GetHealth() < (role == BotRole::Collector ? 72 : 58)
        && result.powerMargin < RetreatPowerMarginForDifficulty(tuning, difficulty);
    if (!defendingCore && !finalLifeTarget && outnumberedBy >= 2 && bot.GetHealth() < 70)
    {
        result.shouldRetreat = true;
    }

    return result;
}


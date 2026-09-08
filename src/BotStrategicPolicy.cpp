#include "BotStrategicPolicy.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
float Distance(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}

bool BotOpeningPressureEligible(float matchTime, int openingRank, bool isDefender,
    float homeDistanceSquared, bool continuingPressure)
{
    // Starting a scout mission must not steal the base defender or reverse
    // an attacker as soon as it crosses the departure radius.
    return matchTime < 60.0f && openingRank >= 1 && openingRank <= 2
        && !isDefender && (homeDistanceSquared < 196.0f || continuingPressure);
}

int SelectBotBridgeHelper(const std::vector<BotBridgeHelperOption>& options)
{
    int best = -1;
    float bestCost = std::numeric_limits<float>::infinity();
    bool bestComplete = false;
    for (const auto& option : options)
    {
        if (option.playerId < 0 || option.blocks < 4 || option.blocks < option.requiredBlocks
            || (!option.completeRoute && !option.executableApproach)) continue;
        const float cost = option.travelSeconds
            + 2.0f * static_cast<float>(std::max(0, 4 - (option.blocks - option.requiredBlocks)));
        if (best < 0 || (option.completeRoute && !bestComplete)
            || (option.completeRoute == bestComplete
                && (cost < bestCost || (cost == bestCost && option.playerId < best))))
        {
            best = option.playerId;
            bestCost = cost;
            bestComplete = option.completeRoute;
        }
    }
    return best;
}

BotHelpOutcome AssessBotHelpOutcome(const BotBridgeHelpFollowup& followup, Vector3 position,
    Vector3 currentObjective, bool alive, bool grounded, bool recovering, float now)
{
    if (!alive) return BotHelpOutcome::Failed;
    if (Distance(followup.objective, currentObjective) > 10.0f) return BotHelpOutcome::ChangedObjective;
    if (now - followup.arrivedAt >= 2.0f && grounded && !recovering
        && Distance(position, followup.objective) + 4.0f <= followup.initialDistance)
        return BotHelpOutcome::Progress;
    return now - followup.arrivedAt >= 30.0f ? BotHelpOutcome::TimedOut : BotHelpOutcome::Pending;
}

float BotAttackCost(const BotAttackOption& option)
{
    if (!option.reachable) return std::numeric_limits<float>::infinity();
    return std::max(0.0f, option.travelSeconds)
        + std::max(0.0f, option.preparationSeconds)
        + 25.0f * std::clamp(option.remainingHealthFraction, 0.0f, 1.0f)
        + 12.0f * std::max(0, option.observedDefenders)
        + 18.0f * std::max(0, option.recentRouteFailures)
        - 4.0f * std::clamp(option.supportingAttackers, 0, 3);
}

int SelectBotAttackOption(const std::vector<BotAttackOption>& options,
    BotAttackCommitment& commitment, float now)
{
    const BotAttackOption* best = nullptr;
    const BotAttackOption* current = nullptr;
    float bestCost = std::numeric_limits<float>::infinity();
    for (const BotAttackOption& option : options)
    {
        const float cost = BotAttackCost(option);
        if (!std::isfinite(cost)) continue;
        if (option.teamId == commitment.teamId) current = &option;
        if (cost < bestCost || (cost == bestCost && best != nullptr && option.teamId < best->teamId))
        {
            best = &option;
            bestCost = cost;
        }
    }
    if (current != nullptr && best != current)
    {
        const float currentCost = BotAttackCost(*current);
        const bool minimumCommit = now - commitment.selectedAt < 12.0f;
        const bool materiallyBetter = bestCost + std::max(8.0f, currentCost * 0.25f) < currentCost;
        if ((minimumCommit && current->recentRouteFailures < 2) || !materiallyBetter) best = current;
    }
    const int selected = best != nullptr ? best->teamId : -1;
    if (selected != commitment.teamId)
    {
        commitment.teamId = selected;
        commitment.selectedAt = now;
    }
    commitment.recheckAt = now + 2.0f;
    return selected;
}

Vector3 SelectBotSearchSite(const std::vector<BotSearchSite>& sites,
    BotSearchMemory& memory, Vector3 hunterPosition, int targetTeamId,
    float observationTime, float now)
{
    if (memory.targetTeamId != targetTeamId)
    {
        memory = {};
        memory.targetTeamId = targetTeamId;
    }
    if (observationTime > memory.observationTime)
    {
        memory.observationTime = observationTime;
        memory.siteId = -1;
        // New evidence reopens the last-seen site without forgetting the
        // rest of the territory already checked by this team.
        for (BotSearchVisit& visit : memory.visits)
            if (visit.siteId == 0) visit.retryAt = -1000.0f;
    }
    if (memory.siteId >= 0)
    {
        const float distance = Distance(hunterPosition, memory.position);
        if (distance + 0.75f < memory.bestDistance)
        {
            memory.bestDistance = distance;
            memory.lastProgressAt = now;
        }
        const bool inspected = distance <= 3.0f && now - memory.selectedAt >= 1.5f;
        const bool stalled = now - memory.lastProgressAt > 18.0f;
        if (!inspected && !stalled) return memory.position;
        auto visit = std::find_if(memory.visits.begin(), memory.visits.end(),
            [&memory](const BotSearchVisit& entry) { return entry.siteId == memory.siteId; });
        const float retryAt = now + (inspected ? 60.0f : 35.0f);
        if (visit == memory.visits.end()) memory.visits.push_back({ memory.siteId, retryAt });
        else visit->retryAt = retryAt;
        memory.siteId = -1;
    }

    const BotSearchSite* best = nullptr;
    float bestCost = std::numeric_limits<float>::infinity();
    for (const BotSearchSite& site : sites)
    {
        float wait = 0.0f;
        for (const BotSearchVisit& visit : memory.visits)
            if (visit.siteId == site.id) wait = std::max(0.0f, visit.retryAt - now);
        const float cost = Distance(hunterPosition, site.position) / 4.0f
            - site.priority + wait * 20.0f;
        if (cost < bestCost || (cost == bestCost && best != nullptr && site.id < best->id))
        {
            bestCost = cost;
            best = &site;
        }
    }
    if (best == nullptr) return hunterPosition;
    memory.siteId = best->id;
    memory.position = best->position;
    memory.selectedAt = memory.lastProgressAt = now;
    memory.bestDistance = Distance(hunterPosition, memory.position);
    return memory.position;
}

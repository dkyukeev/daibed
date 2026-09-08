#pragma once

#include "raylib.h"
#include <vector>

bool BotOpeningPressureEligible(float matchTime, int openingRank, bool isDefender,
    float homeDistanceSquared, bool continuingPressure);

struct BotBridgeHelperOption
{
    int playerId = -1;
    bool completeRoute = false;
    bool executableApproach = false;
    float travelSeconds = 0.0f;
    int blocks = 0;
    int requiredBlocks = 0;
};
int SelectBotBridgeHelper(const std::vector<BotBridgeHelperOption>& options);

struct BotBridgeHelpFollowup
{
    int requesterId = -1;
    int helperId = -1;
    Vector3 objective {};
    float initialDistance = 0.0f;
    float arrivedAt = 0.0f;
};
enum class BotHelpOutcome { Pending, Progress, Failed, ChangedObjective, TimedOut };
BotHelpOutcome AssessBotHelpOutcome(const BotBridgeHelpFollowup& followup, Vector3 position,
    Vector3 currentObjective, bool alive, bool grounded, bool recovering, float now);

struct BotAttackOption
{
    int teamId = -1;
    bool reachable = true;
    float travelSeconds = 0.0f;
    float preparationSeconds = 0.0f;
    float remainingHealthFraction = 1.0f;
    int observedDefenders = 0;
    int supportingAttackers = 0;
    int recentRouteFailures = 0;
};

struct BotAttackCommitment
{
    int teamId = -1;
    float selectedAt = -1000.0f;
    float recheckAt = -1000.0f;
};

// Costs are expressed in estimated seconds. An active assault is replaced
// only by material new evidence, not by its own reinforcement broadcasts.
float BotAttackCost(const BotAttackOption& option);
int SelectBotAttackOption(const std::vector<BotAttackOption>& options,
    BotAttackCommitment& commitment, float now);

struct BotSearchSite
{
    int id = -1;
    Vector3 position {};
    float priority = 0.0f;
};

struct BotSearchVisit
{
    int siteId = -1;
    float retryAt = -1000.0f;
};

struct BotSearchMemory
{
    int targetTeamId = -1;
    int siteId = -1;
    float selectedAt = -1000.0f;
    float lastProgressAt = -1000.0f;
    float bestDistance = 0.0f;
    float observationTime = -1000.0f;
    Vector3 position {};
    std::vector<BotSearchVisit> visits;
};

// Sites contain public map locations and a genuine last sighting only.
// Reaching or failing a site advances the search; no hidden actor positions
// enter this policy. Memory may be retained when another teammate takes over.
Vector3 SelectBotSearchSite(const std::vector<BotSearchSite>& sites,
    BotSearchMemory& memory, Vector3 hunterPosition, int targetTeamId,
    float observationTime, float now);

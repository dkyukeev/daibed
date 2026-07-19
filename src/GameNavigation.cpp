#include "Game.h"

#include "Navigation/NavigationGoal.h"
#include "Navigation/NavigationProfile.h"
#include "Navigation/NavigationWorldView.h"
#include "Navigation/ThreatMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace
{
std::uint64_t Mix(std::uint64_t seed, std::uint64_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

std::uint64_t GoalSignature(BotIntent intent, GridPos target, int targetActor, int targetCore)
{
    // Strategic targets often carry sub-block motion (pickups, combat context,
    // coarse authored bearings). A goal only changes after crossing a material
    // navigation cell, otherwise the executor would be cancelled every tick.
    target.x /= 4;
    target.y /= 2;
    target.z /= 4;
    std::uint64_t signature = static_cast<std::uint64_t>(intent) + 1U;
    signature = Mix(signature, static_cast<std::uint32_t>(target.x));
    signature = Mix(signature, static_cast<std::uint32_t>(target.y));
    signature = Mix(signature, static_cast<std::uint32_t>(target.z));
    signature = Mix(signature, static_cast<std::uint32_t>(targetActor + 1));
    return Mix(signature, static_cast<std::uint32_t>(targetCore + 1));
}

void AimAt(PlayerCommand& command, Vector3 eye, Vector3 target)
{
    const float dx = target.x - eye.x;
    const float dy = target.y - eye.y;
    const float dz = target.z - eye.z;
    command.aimYaw = std::atan2(dx, -dz);
    command.aimPitch = std::atan2(dy, std::max(0.0001f, std::sqrt(dx * dx + dz * dz)));
}

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

int PickaxeSlot(const Player& player)
{
    const auto& hotbar = player.GetInventory().GetHotbarSlots();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        if (!hotbar[slot].IsEmpty() && ItemIsPickaxe(hotbar[slot].type)) return slot;
    }
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        if (!hotbar[slot].IsEmpty() && ItemIsWeapon(hotbar[slot].type)) return slot;
    }
    return 0;
}
}

bool Game::UpdateActionNavigation(
    Player& bot,
    BotMemory& memory,
    Vector3 target,
    Player* targetPlayer,
    EnergyCore* targetCore,
    float dt,
    PlayerCommand& command,
    bool& goalSatisfied)
{
    goalSatisfied = false;
    std::vector<NavigationActor> actors;
    actors.reserve(players_.size());
    for (const Player& player : players_)
    {
        const bool enemy = player.GetTeamId() != bot.GetTeamId();
        const bool visibleEnemy = enemy && BotHasLineOfSight(bot, player);
        const bool rememberedEnemy = enemy
            && memory.rememberedEnemyId == player.GetId()
            && memory.enemyMemoryConfidence > 0.15f;
        if (enemy && !visibleEnemy && !rememberedEnemy)
        {
            continue;
        }
        NavigationActor actor;
        actor.playerId = player.GetId();
        actor.teamId = player.GetTeamId();
        actor.position = rememberedEnemy && !visibleEnemy
            ? memory.lastSeenEnemyPosition
            : player.GetPosition();
        actor.velocity = rememberedEnemy && !visibleEnemy
            ? memory.rememberedEnemyVelocity
            : player.GetVelocity();
        actor.alive = player.IsAlive() && !player.IsEliminated();
        const int weaponLevel = visibleEnemy || !enemy ? player.GetInventory().GetSwordLevel() : 1;
        actor.threatRadius = visibleEnemy && player.GetInventory().HasItem(ItemType::SniperRifle)
            ? 18.0f
            : (visibleEnemy && player.GetInventory().HasItem(ItemType::Bow) ? 13.0f : 8.0f);
        actor.threatCost = 6.0f + static_cast<float>(weaponLevel) * 2.5f
            + static_cast<float>(player.GetHealth()) * 0.035f;
        actors.push_back(actor);
    }

    ThreatMap threatMap;
    threatMap.AddActorThreats(actors, bot.GetTeamId());
    for (const HazardZone& zone : hazardZones_)
    {
        if (zone.ownerTeamId != bot.GetTeamId())
        {
            threatMap.AddSource(ThreatSource {
                zone.position, zone.radius + 1.2f, 22.0f, zone.ownerPlayerId, zone.ownerTeamId });
        }
    }
    for (const TimedExplosion& explosive : timedExplosions_)
    {
        if (explosive.ownerTeamId != bot.GetTeamId())
        {
            threatMap.AddSource(ThreatSource {
                explosive.position, explosive.radius + 1.5f,
                explosive.timer < 0.8f ? 30.0f : 14.0f,
                explosive.ownerPlayerId, explosive.ownerTeamId });
        }
    }
    NavigationWorldView view(world_, bot.GetTeamId(), actors, &threatMap);

    NavigationProfileSettings profileSettings;
    const BotTuningGenome& tuning = BotTuningForTeam(bot.GetTeamId());
    profileSettings.gravityMultiplier = BiomeGravityMultiplier();
    profileSettings.jumpMultiplier = BiomeJumpMultiplier();
    profileSettings.terrainSpeedMultiplier = TerrainSpeedMultiplier(bot);
    profileSettings.maxStepHeightBlocks = 0;
    const bool cleanupNavigation = memory.currentPlan.goal == StrategicGoal::HuntPlayers;
    profileSettings.maxSafeDropBlocks = cleanupNavigation
        ? (memory.routeFailureCooldown <= 0.0f ? 3 : 2)
        : (memory.role == BotRole::Rusher && memory.routeFailureCooldown <= 0.0f ? 2 : 1);
    const bool lowBridgeStock = bot.GetInventory().GetBlocks() < 4;
    const bool coreGapOpportunity = lowBridgeStock
        && targetCore != nullptr
        && (memory.intent == BotIntent::PressureCore
            || memory.intent == BotIntent::BreakCoreDefense)
        && DistanceSquared(bot.GetPosition(), target) <= 324.0f;
    const bool crystalGapOpportunity = lowBridgeStock
        && memory.intent == BotIntent::SecureResources
        && memory.cachedResourceType == static_cast<int>(ResourceType::Crystal)
        && DistanceSquared(bot.GetPosition(), target) <= 324.0f;
    // Gap actions build their own short, verified sprint runway; their
    // physics envelope decides whether 1-, 2- or 3-cell jumps are legal.
    profileSettings.maxGapJumpBlocks = 3;
    profileSettings.allowStairBuilding = true;
    profileSettings.gapRunupDistanceBlocks = tuning.navigationRunupDistanceBlocks;
    profileSettings.gapTakeoffDelaySeconds = tuning.navigationTakeoffDelaySeconds;
    profileSettings.gapTakeoffEdgeOffsetBlocks = tuning.navigationTakeoffEdgeOffsetBlocks;
    profileSettings.gapTakeoffGapScale = tuning.navigationTakeoffGapScale;
    profileSettings.gapAirControlScale = tuning.navigationAirControlScale;
    profileSettings.gapLandingCorrectionGain = tuning.navigationLandingCorrectionGain;
    profileSettings.fallRiskPenalty = tuning.navigationFallRiskPenalty;
    profileSettings.maxSprintRunBlocks = botDifficulty_ == BotDifficulty::Hard
        ? 4 : (botDifficulty_ == BotDifficulty::Normal ? 3 : 2);
    profileSettings.maxConsecutiveBridgeBlocks = botDifficulty_ == BotDifficulty::Hard
        ? 8 : (botDifficulty_ == BotDifficulty::Normal ? 6 : 4);
    if (memory.routeCorridorBridgeSegment)
    {
        // Long construction is legal only on a map-authored bridge segment
        // with exact supported endpoints. Ordinary local navigation retains
        // the conservative difficulty limit above.
        profileSettings.maxConsecutiveBridgeBlocks = std::clamp(
            memory.routeCorridorExpectedBridgeBlocks + 2, 8, 64);
    }
    switch (memory.role)
    {
    case BotRole::Defender:
        profileSettings.riskTolerance = 0.28f;
        profileSettings.resourceConservation = 0.90f;
        profileSettings.reserveBridgeBlocks = 4;
        break;
    case BotRole::Collector:
        profileSettings.riskTolerance = 0.20f;
        profileSettings.resourceConservation = 1.10f;
        profileSettings.reserveBridgeBlocks = 5;
        break;
    case BotRole::Rusher:
        profileSettings.riskTolerance = 0.52f + memory.aggressionTrait * 0.28f;
        profileSettings.resourceConservation = 0.24f;
        profileSettings.reserveBridgeBlocks = 0;
        break;
    case BotRole::Fighter:
        profileSettings.riskTolerance = 0.64f;
        profileSettings.resourceConservation = 0.45f;
        profileSettings.reserveBridgeBlocks = 1;
        break;
    }
    if (memory.routeCorridorBridgeSegment)
    {
        profileSettings.reserveBridgeBlocks = 0;
        profileSettings.resourceConservation = 0.10f;
    }
    const Team* ownTeam = FindTeam(bot.GetTeamId());
    if (ownTeam != nullptr && !ownTeam->coreAlive)
    {
        profileSettings.riskTolerance *= 0.72f;
    }
    if (memory.routeFailureCooldown > 0.0f)
    {
        profileSettings.riskTolerance *= 0.52f;
        profileSettings.resourceConservation *= 1.35f;
        profileSettings.reserveBridgeBlocks = std::max(profileSettings.reserveBridgeBlocks, 3);
    }
    if (memory.assignedBridgeAssist)
    {
        // The selected ally owns the short repair, so let its normal action
        // planner use the available stack instead of stopping at the usual
        // local bridge-chain cap or holding blocks in reserve.
        profileSettings.maxConsecutiveBridgeBlocks = std::max(
            profileSettings.maxConsecutiveBridgeBlocks, 10);
        profileSettings.reserveBridgeBlocks = 0;
        profileSettings.resourceConservation = std::min(
            profileSettings.resourceConservation, 0.15f);
    }
    const NavigationProfile profile = BuildNavigationProfile(bot, profileSettings);

    GridPos targetSupport = view.FindSupport(target, 12, 2, profile.bodyCenterAboveSupport)
        .value_or(view.WorldToGrid(Vector3 { target.x, target.y - profile.bodyCenterAboveSupport, target.z }));
    NavigationGoalPtr goal;
    switch (memory.intent)
    {
    case BotIntent::PressureCore:
    case BotIntent::BreakCoreDefense:
        if (targetCore != nullptr && !memory.usingAuthoredRoute && !memory.usingRouteCorridor)
        {
            goal = std::make_shared<GoalCoreAttackPosition>(targetCore->GetBlockPosition(), 4.2f, true);
            targetSupport = targetCore->GetBlockPosition();
        }
        else if (targetCore == nullptr)
        {
            // A pressure bot temporarily sent to restock still carries its
            // strategic intent. Shops/generators may expose a marker one block
            // above or below the nearest legal support, so requiring the exact
            // support cell creates an infinite NoPath loop while already in
            // interaction range.
            goal = std::make_shared<GoalWithinRadius>(targetSupport, 2.2f);
        }
        break;
    case BotIntent::FightEnemy:
    case BotIntent::ChaseWeakEnemy:
        if (targetPlayer != nullptr)
        {
            goal = std::make_shared<GoalAttackRangeOfPlayer>(targetPlayer->GetId(), 1.4f, 2.8f, true);
        }
        break;
    case BotIntent::DefendCore:
        if (ownTeam != nullptr)
        {
            targetSupport = ownTeam->coreBlock;
        }
        goal = std::make_shared<GoalDefensivePosition>(targetSupport, 1.0f, 5.5f, 20.0f);
        break;
    case BotIntent::GearUp:
        goal = std::make_shared<GoalShopPosition>(targetSupport, 2.6f);
        break;
    case BotIntent::SecureResources:
        goal = std::make_shared<GoalResourceGenerator>(targetSupport, 1.4f);
        break;
    case BotIntent::RetreatHome:
        goal = std::make_shared<GoalWithinRadius>(targetSupport, 1.8f);
        break;
    case BotIntent::Recover:
        goal = std::make_shared<GoalWithinRadius>(targetSupport, 0.8f);
        break;
    case BotIntent::RepairCoreDefense:
        // The adaptive defense builder remains an explicit migration fallback.
        return false;
    }
    if (goal == nullptr) goal = std::make_shared<GoalWithinRadius>(targetSupport, 0.8f);

    auto inserted = botNavigationControllers_.try_emplace(bot.GetId());
    NavigationController& controller = inserted.first->second;
    const bool actorGoal = memory.intent == BotIntent::FightEnemy
        || memory.intent == BotIntent::ChaseWeakEnemy;
    const bool coreGoal = (memory.intent == BotIntent::PressureCore
        || memory.intent == BotIntent::BreakCoreDefense)
        && !memory.usingAuthoredRoute
        && !memory.usingRouteCorridor;
    const std::uint64_t signature = GoalSignature(
        memory.intent,
        targetSupport,
        actorGoal && targetPlayer != nullptr ? targetPlayer->GetId() : -1,
        coreGoal && targetCore != nullptr ? targetCore->GetTeamId() : -1);
    const auto previousIntent = botNavigationIntents_.find(bot.GetId());
    const bool intentChanged = previousIntent == botNavigationIntents_.end()
        || previousIntent->second != memory.intent;
    // A moving semantic target may refresh between perception ticks. Keep the
    // current short segment (at most 16 actions) unless the intent itself
    // changed; this prevents target jitter from cancelling A* every tick.
    if (intentChanged || !controller.HasGoal() || !controller.Executor().IsActive()
        || memory.usingAuthoredRoute || memory.usingRouteCorridor)
    {
        controller.SetGoal(goal, signature);
        botNavigationIntents_[bot.GetId()] = memory.intent;
    }
    NavigationControllerUpdate update = controller.Update(
        bot, view, profile, dt, matchSimulation_.CurrentTick());
    const NavigationMetrics navigationDelta = controller.ConsumeMetricsDelta();
    navigationMetrics_.Add(navigationDelta);
    if (navigationDelta.failedPathRequests > 0
        && lowBridgeStock
        && (coreGapOpportunity || crystalGapOpportunity)
        && memory.bridgeHelpRequestCooldown <= 0.0f)
    {
        const int busIndex = bot.GetTeamId() >= 0
            && bot.GetTeamId() < static_cast<int>(teamCoordBuses_.size())
            ? bot.GetTeamId() : -1;
        if (busIndex >= 0)
        {
            TeamCoordinationBus& bus = teamCoordBuses_[static_cast<std::size_t>(busIndex)];
            const float now = matchSimulation_.MatchTimeSeconds();
            if (bus.bridgeRequestorId < 0
                || now - bus.bridgeRequestTimestamp > 7.0f
                || bus.bridgeRequestorId == bot.GetId())
            {
                bus.bridgeRequestorId = bot.GetId();
                bus.bridgeRequestBuilderId = -1;
                bus.bridgeRequestIntent = memory.intent;
                bus.bridgeRequestTarget = target;
                bus.bridgeRequestTargetTeamId = targetCore != nullptr
                    ? targetCore->GetTeamId() : -1;
                bus.bridgeRequestTimestamp = now;
                bus.bridgeRequestReservationUntil = -1000.0f;
                bus.Broadcast(bot.GetId(), CoordinationSignal::CallingForHelp,
                    target, now, bus.bridgeRequestTargetTeamId);
                memory.waitingForBridgeHelp = true;
            }
        }
        memory.bridgeHelpRequestCooldown = 4.0f;
    }
    if (cleanupNavigation && navigationDelta.failedPathRequests > 0)
    {
        // A cleanup scout can be stranded on enemy high ground after the Core
        // fight. Do not hammer the same impossible segment every 350 ms: mark
        // the assignment as physically failed so the team can hand it to the
        // next rusher on the following strategic frame.
        memory.repeatedRouteFailures = std::max(2, memory.repeatedRouteFailures + 1);
        memory.routeFailureCooldown = std::max(memory.routeFailureCooldown, 18.0f);
        memory.lastRouteFailurePosition = bot.GetPosition();
        memory.hasLastRouteFailure = true;
        memory.routeCorridorNodes.clear();
        memory.routeCorridorTraversal.clear();
        memory.routeCorridorBridgeBlocks.clear();
        memory.hasRouteCorridorObjective = false;
        memory.routeCorridorReplanCooldown = 2.0f;
    }
    if (automatch_.active && navigationDelta.failedPathRequests > 0)
    {
        const int recordedPathFailures = static_cast<int>(std::count_if(
            automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
            [](const AutomatchTimelineEvent& event) { return event.type == "pathFailure"; }));
        if (recordedPathFailures < 320)
        {
            NavigationSearchStatus failedStatus = NavigationSearchStatus::NoPath;
            for (std::size_t status = static_cast<std::size_t>(NavigationSearchStatus::NoPath);
                status < kNavigationSearchStatusCount; ++status)
            {
                if (navigationDelta.pathResultsByStatus[status] > 0)
                {
                    failedStatus = static_cast<NavigationSearchStatus>(status);
                    break;
                }
            }
            const std::optional<GridPos> actorSupport = view.FindSupport(
                bot.GetPosition(), profile.maxSafeDropBlocks + 2, 2,
                profile.bodyCenterAboveSupport);
            const GridPos start = actorSupport.value_or(view.WorldToGrid(Vector3 {
                bot.GetPosition().x,
                bot.GetPosition().y - profile.bodyCenterAboveSupport,
                bot.GetPosition().z
            }));
            automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                matchSimulation_.MatchTimeSeconds(), "pathFailure", bot.GetTeamId(),
                bot.GetTeamId(), bot.GetId(), targetCore != nullptr ? targetCore->GetTeamId() : -1,
                static_cast<int>(failedStatus),
                std::string(ToString(failedStatus))
                    + " intent=" + ToString(memory.intent)
                    + " start=" + std::to_string(start.x) + ","
                    + std::to_string(start.y) + "," + std::to_string(start.z)
                    + " goal=" + std::to_string(targetSupport.x) + ","
                    + std::to_string(targetSupport.y) + "," + std::to_string(targetSupport.z)
                    + " route=" + std::to_string(memory.usingAuthoredRoute ? 1 : 0)
                    + " corridor=" + std::to_string(memory.usingRouteCorridor ? 1 : 0)
                    + " routeIndex=" + std::to_string(memory.authoredRouteIndex)
                    + " corridorIndex=" + std::to_string(memory.routeCorridorIndex)
                    + " blocks=" + std::to_string(profile.availableBridgeBlocks)
            });
        }
    }
    if (update.blockedStartRelocated)
    {
        const Vector3 relocation = view.SupportCenter(
            update.blockedStartRelocationSupport, profile.bodyCenterAboveSupport);
        bot.SetPosition(Vec3 { relocation.x, relocation.y, relocation.z });
        bot.SetVelocity(Vec3 {});
        controller.ForceRepath(RepathReason::ActorDisplaced);
    }
    command = update.command;
    goalSatisfied = update.goalSatisfied;
    if (automatch_.active && update.supportLost)
    {
        const int recordedSupportLosses = static_cast<int>(std::count_if(
            automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
            [](const AutomatchTimelineEvent& event) { return event.type == "supportLost"; }));
        if (recordedSupportLosses < 160)
        {
            const Vector3 actorPosition = bot.GetPosition();
            automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                matchSimulation_.MatchTimeSeconds(), "supportLost", bot.GetTeamId(),
                bot.GetTeamId(), bot.GetId(), bot.GetId(),
                static_cast<int>(update.supportLostMovementType),
                std::string(ToString(update.supportLostMovementType))
                    + " actor=" + std::to_string(actorPosition.x) + ","
                    + std::to_string(actorPosition.y) + "," + std::to_string(actorPosition.z)
                    + " action=" + std::to_string(update.supportLostMovementFrom.x) + ","
                    + std::to_string(update.supportLostMovementFrom.y) + ","
                    + std::to_string(update.supportLostMovementFrom.z) + "->"
                    + std::to_string(update.supportLostMovementTo.x) + ","
                    + std::to_string(update.supportLostMovementTo.y) + ","
                    + std::to_string(update.supportLostMovementTo.z)
            });
        }
    }
    if (automatch_.active && update.blockedStart)
    {
        const float now = matchSimulation_.MatchTimeSeconds();
        const int recordedBlockedStarts = static_cast<int>(std::count_if(
            automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
            [](const AutomatchTimelineEvent& event) { return event.type == "blockedStart"; }));
        const auto previous = std::find_if(
            automatch_.currentTimeline.rbegin(), automatch_.currentTimeline.rend(),
            [&bot](const AutomatchTimelineEvent& event)
            {
                return event.type == "blockedStart" && event.actorId == bot.GetId();
            });
        const bool sampleDue = previous == automatch_.currentTimeline.rend()
            || now - previous->time >= 2.0f;
        if (recordedBlockedStarts < 240 && sampleDue)
        {
            const Vector3 actorPosition = bot.GetPosition();
            automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                now, "blockedStart", bot.GetTeamId(), bot.GetTeamId(), bot.GetId(), bot.GetId(),
                static_cast<int>(update.blockedStartBlockType),
                std::string(ToString(update.blockedStartBlockType))
                    + (update.blockedStartRecovered ? " recovered" : " deferred")
                    + " actor=" + std::to_string(actorPosition.x) + ","
                    + std::to_string(actorPosition.y) + "," + std::to_string(actorPosition.z)
                    + " support=" + std::to_string(update.blockedStartSupport.x) + ","
                    + std::to_string(update.blockedStartSupport.y) + ","
                    + std::to_string(update.blockedStartSupport.z)
                    + " blocker=" + std::to_string(update.blockedStartBlock.x) + ","
                    + std::to_string(update.blockedStartBlock.y) + ","
                    + std::to_string(update.blockedStartBlock.z)
            });
        }
    }
    if (update.routeAbandoned)
    {
        const Vector3 failedPosition = bot.GetPosition();
        if (automatch_.active)
        {
            const int recordedRouteFailures = static_cast<int>(std::count_if(
                automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
                [](const AutomatchTimelineEvent& event) { return event.type == "routeFailure"; }));
            if (recordedRouteFailures < 40)
            {
                automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                    matchSimulation_.MatchTimeSeconds(), "routeFailure", bot.GetTeamId(),
                    bot.GetTeamId(), bot.GetId(), bot.GetId(),
                    static_cast<int>(update.failedMovementType),
                    std::string(ToString(update.failedMovementType))
                        + " actor=" + std::to_string(failedPosition.x) + ","
                        + std::to_string(failedPosition.y) + "," + std::to_string(failedPosition.z)
                        + " action=" + std::to_string(update.failedMovementFrom.x) + ","
                        + std::to_string(update.failedMovementFrom.y) + ","
                        + std::to_string(update.failedMovementFrom.z) + "->"
                        + std::to_string(update.failedMovementTo.x) + ","
                        + std::to_string(update.failedMovementTo.y) + ","
                        + std::to_string(update.failedMovementTo.z)
                });
            }
        }
        const float failureDx = memory.lastRouteFailurePosition.x - failedPosition.x;
        const float failureDy = memory.lastRouteFailurePosition.y - failedPosition.y;
        const float failureDz = memory.lastRouteFailurePosition.z - failedPosition.z;
        const bool repeatedHere = memory.hasLastRouteFailure
            && failureDx * failureDx + failureDy * failureDy + failureDz * failureDz <= 16.0f;
        memory.lastRouteFailurePosition = failedPosition;
        memory.hasLastRouteFailure = true;
        memory.repeatedRouteFailures = repeatedHere
            ? std::min(memory.repeatedRouteFailures + 1, 6)
            : 1;
        memory.routeFailureCooldown = 6.0f
            + 2.0f * static_cast<float>(memory.repeatedRouteFailures);
        memory.stuckTimer = std::max(memory.stuckTimer, 2.30f);
        memory.routeCorridorNodes.clear();
        memory.routeCorridorTraversal.clear();
        memory.routeCorridorBridgeBlocks.clear();
        memory.hasRouteCorridorObjective = false;
        memory.routeCorridorReplanCooldown = 0.0f;
        memory.routeCorridorBridgeSegment = false;
        memory.routeCorridorExpectedBridgeBlocks = 0;
        memory.routeCorridorBridgeStarted = false;
        memory.routeCorridorActiveBridgeSignature = 0;
        memory.routeCorridorWaitingForBuilder = false;
        botNavigationIntents_.erase(bot.GetId());
    }
    if (update.repathReason == RepathReason::EmergencySave && command.placePressed)
    {
        memory.pendingVoidEscapeTimer = 3.0f;
        memory.pendingVoidEscapePosition = bot.GetPosition();
        if (memory.carriedResourceValue >= 20 || bot.GetHealth() <= 40 || memory.repeatedRouteFailures > 0)
        {
            RecordMemorableMoment(
                "EmergencyBridge", bot.GetTeamId(), -1, { bot.GetId() },
                bot.GetName() + " attempted an action-navigation void save",
                0.40f, true, bot.GetHealth(), -1, memory.carriedResourceValue,
                { "navigation lost support", "emergency arbiter won", "issued place command" });
        }
    }

    // Core interaction is still command-driven: the authoritative raycast
    // breaks the visible shell first and damages the Core only after access.
    if (goalSatisfied && targetCore != nullptr
        && (memory.intent == BotIntent::PressureCore
            || memory.intent == BotIntent::BreakCoreDefense))
    {
        const int toolSlot = PickaxeSlot(bot);
        command.selectedSlot = toolSlot;
        command.attackHeld = true;
        command.attackPressed = false;
        const Vector3 position = bot.GetPosition();
        AimAt(command,
            Vector3 { position.x, position.y + 0.78f, position.z },
            world_.GridToWorld(targetCore->GetBlockPosition()));
        return true;
    }
    // An exhausted/cancelled executor keeps its path for diagnostics, so
    // HasPath() alone does not mean it produced movement. Returning true here
    // with a zero command suppressed the legacy fallback and froze bots at a
    // corridor portal indefinitely. Only an active executor owns movement.
    return update.hasCommand || update.planned || controller.Executor().IsActive();
}

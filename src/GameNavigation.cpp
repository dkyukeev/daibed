#include "Game.h"

#include "Navigation/NavigationGoal.h"
#include "Navigation/BotNavigationGoal.h"
#include "Navigation/NavigationProfile.h"
#include "Navigation/NavigationWorldView.h"
#include "Navigation/ThreatMap.h"
#include "Navigation/VoxelPathfinder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <limits>
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

Vector3 NormalizeCorridorDirection(Vector3 direction)
{
    const float length = std::sqrt(direction.x * direction.x + direction.z * direction.z);
    if (length <= 0.0001f) return Vector3 {};
    return Vector3 { direction.x / length, 0.0f, direction.z / length };
}

GridPos FindCorridorSupport(const World& world, Vector3 position, int maxDropBlocks)
{
    const GridPos underCenter = world.WorldToGrid(Vector3 { position.x, position.y - 1.40f, position.z });
    for (int drop = 0; drop <= maxDropBlocks; ++drop)
    {
        const GridPos candidate { underCenter.x, underCenter.y - drop, underCenter.z };
        if (!world.IsAir(candidate)) return candidate;
    }
    return underCenter;
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

void Game::UpdateBotBridgeCoordination()
{
    const float now = matchSimulation_.MatchTimeSeconds();
    const auto playerFor = [&](int id) -> const Player* {
        for (const auto& player : players_) if (player.GetId() == id) return &player;
        return nullptr;
    };
    const auto memoryFor = [&](int id) -> const BotMemory* {
        for (const auto& memory : botMemories_) if (memory.playerId == id) return &memory;
        return nullptr;
    };
    const auto record = [&](const char* type, int team, int helper, int requester, int value) {
        if (automatch_.active && std::count_if(automatch_.currentTimeline.begin(),
            automatch_.currentTimeline.end(), [&](const AutomatchTimelineEvent& e) { return e.type == type; }) < 128)
            automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                now, type, team, team, helper, requester, value, "bridge assistance observation" });
    };
    for (std::size_t team = 0; team < teamCoordBuses_.size(); ++team)
    {
        auto& bus = teamCoordBuses_[team];
        for (auto it = bus.bridgeHelpFollowups.begin(); it != bus.bridgeHelpFollowups.end();)
        {
            const Player* requester = playerFor(it->requesterId);
            const BotMemory* memory = memoryFor(it->requesterId);
            const bool alive = requester && requester->IsAlive() && !requester->IsEliminated();
            const BotHelpOutcome outcome = AssessBotHelpOutcome(*it,
                requester ? requester->GetPosition() : Vector3 {},
                memory && memory->routeEntryPending ? memory->routeEntryObjective
                    : memory && memory->hasObjectiveTarget ? memory->objectiveTarget : it->objective,
                alive, requester && requester->IsOnGround(), !memory || memory->routeEntryPending, now);
            if (outcome == BotHelpOutcome::Pending) { ++it; continue; }
            const char* type = outcome == BotHelpOutcome::Progress ? "bridgeHelpProgress"
                : outcome == BotHelpOutcome::Failed ? "bridgeHelpFailed"
                : outcome == BotHelpOutcome::ChangedObjective ? "bridgeHelpChangedObjective" : "bridgeHelpUnresolved";
            record(type, static_cast<int>(team), it->helperId, it->requesterId, 0);
            it = bus.bridgeHelpFollowups.erase(it);
        }
        if (bus.bridgeRequestorId < 0 || now - bus.bridgeRequestTimestamp > 7.0f) continue;
        const Player* requester = playerFor(bus.bridgeRequestorId);
        if (!requester || !requester->IsAlive() || requester->IsEliminated()) continue;
        const Player* owner = playerFor(bus.bridgeRequestBuilderId);
        if (owner && owner->IsAlive() && !owner->IsEliminated()
            && bus.bridgeRequestReservationUntil > now) continue;
        if (now < bus.bridgeHelperRecheckAt) continue;
        bus.bridgeHelperRecheckAt = now + 2.0f;
        std::vector<BotBridgeHelperOption> options;
        NavigationWorldView view(world_, static_cast<int>(team));
        for (const Player& candidate : players_)
        {
            const BotMemory* memory = memoryFor(candidate.GetId());
            if (candidate.GetTeamId() != static_cast<int>(team) || candidate.GetId() == bus.bridgeRequestorId
                || !candidate.IsAlive() || candidate.IsEliminated() || !IsBotControlled(ControlKindForPlayer(candidate))
                || !memory || memory->role == BotRole::Defender || memory->intent == BotIntent::RetreatHome
                || memory->intent == BotIntent::Recover || candidate.GetInventory().GetBlocks() < 4
                || (candidate.GetId() == bus.bridgeRequestBuilderId && now - bus.bridgeRequestProgressAt > 12.0f)) continue;
            const bool fighting = std::any_of(players_.begin(), players_.end(), [&](const Player& enemy) {
                return enemy.GetTeamId() != candidate.GetTeamId() && enemy.IsAlive() && !enemy.IsEliminated()
                    && DistanceSquared(candidate.GetPosition(), enemy.GetPosition()) < 64.0f
                    && BotHasLineOfSight(candidate, enemy);
            });
            if (fighting) continue;
            NavigationProfileSettings settings;
            settings.gravityMultiplier = BiomeGravityMultiplier();
            settings.jumpMultiplier = BiomeJumpMultiplier();
            settings.terrainSpeedMultiplier = TerrainSpeedMultiplier(candidate);
            settings.maxSafeDropBlocks = 8;
            settings.allowStairBuilding = true;
            settings.reserveBridgeBlocks = 0;
            settings.resourceConservation = 0.15f;
            const auto& tuning = BotTuningForTeam(static_cast<int>(team));
            settings.gapRunupDistanceBlocks = tuning.navigationRunupDistanceBlocks;
            settings.gapTakeoffDelaySeconds = tuning.navigationTakeoffDelaySeconds;
            settings.gapTakeoffEdgeOffsetBlocks = tuning.navigationTakeoffEdgeOffsetBlocks;
            settings.gapTakeoffGapScale = tuning.navigationTakeoffGapScale;
            settings.gapAirControlScale = tuning.navigationAirControlScale;
            settings.gapLandingCorrectionGain = tuning.navigationLandingCorrectionGain;
            const NavigationProfile profile = BuildNavigationProfile(candidate, settings);
            const auto start = view.FindSupport(candidate.GetPosition(), 8, 2, profile.bodyCenterAboveSupport);
            const auto goal = view.FindSupport(bus.bridgeRequestTarget, 8, 2, profile.bodyCenterAboveSupport);
            if (!start || !goal) continue;
            NavigationSearchLimits limits;
            limits.maxActions = 128;
            limits.maxSearchRadius = 96;
            const auto result = VoxelPathfinder {}.FindPath(NavigationState { *start, 0, 0 },
                GoalWithinRadius(*goal, 0.8f), view, profile, limits);
            const bool complete = result.status == NavigationSearchStatus::Success
                || result.status == NavigationSearchStatus::AlreadySatisfied;
            const Vector3 endpoint = view.SupportCenter(result.path.resolvedGoal, profile.bodyCenterAboveSupport);
            const bool approach = result.status == NavigationSearchStatus::Partial && result.HasPath()
                && DistanceSquared(candidate.GetPosition(), endpoint) > 1.0f;
            const int required = static_cast<int>(std::count_if(result.path.movements.begin(), result.path.movements.end(),
                [](const PlannedMovement& move) { return move.type == MovementType::PlaceBlock || move.type == MovementType::SneakBridge; }));
            const float travel = result.path.timeCost + (complete ? 0.0f
                : std::sqrt(DistanceSquared(endpoint, bus.bridgeRequestTarget)) / std::max(1.0f, profile.moveSpeed));
            options.push_back({ candidate.GetId(), complete, approach, travel, profile.availableBridgeBlocks, required });
        }
        bus.preferredBridgeHelperId = SelectBotBridgeHelper(options);
        for (const auto& option : options) if (option.playerId == bus.preferredBridgeHelperId)
            record(option.completeRoute ? "bridgeHelpRouteVerified" : "bridgeHelpRoutePartial",
                static_cast<int>(team), option.playerId, bus.bridgeRequestorId, option.requiredBlocks);
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
    // Shared movement capabilities let every role return along the same
    // stairs/bridge. Risk preference is expressed in costs, not by trapping
    // a defender on a two-block ledge after a recovery action.
    // Castle roofs are up to eight blocks above the next landing. Player
    // physics has no height-based damage; a verified landing is preferable
    // to trapping an otherwise healthy bot on its own base. Failure cooldown
    // changes risk costs below, not the physical ability to descend.
    profileSettings.maxSafeDropBlocks = 8;
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
    if (memory.routeEntryPending)
    {
        profileSettings.allowBridging = memory.routeEntryRepair;
        if (memory.routeEntryRepair)
        {
            profileSettings.maxConsecutiveBridgeBlocks = 8;
            profileSettings.reserveBridgeBlocks = 2;
        }
        profileSettings.allowStairBuilding = false;
        profileSettings.allowBlockBreaking = false;
    }
    const NavigationProfile profile = BuildNavigationProfile(bot, profileSettings);

    GridPos targetSupport = view.FindSupport(target, 12, 2, profile.bodyCenterAboveSupport)
        .value_or(view.WorldToGrid(Vector3 { target.x, target.y - profile.bodyCenterAboveSupport, target.z }));
    const bool intermediate = memory.usingAuthoredRoute || memory.usingRouteCorridor;
    BotNavigationDestination destination;
    destination.intent = memory.intent;
    destination.support = targetSupport;
    destination.intermediate = intermediate;
    destination.bridgeAssist = memory.assignedBridgeAssist;
    destination.targetPlayerId = targetPlayer != nullptr ? targetPlayer->GetId() : -1;
    if (targetCore != nullptr) destination.targetCore = targetCore->GetBlockPosition();
    if (ownTeam != nullptr) destination.ownCore = ownTeam->coreBlock;
    NavigationGoalPtr goal = BuildBotNavigationGoal(destination);
    if (goal == nullptr) return false;
    targetSupport = goal->RepresentativePosition().value_or(targetSupport);

    auto inserted = botNavigationControllers_.try_emplace(bot.GetId());
    NavigationController& controller = inserted.first->second;
    const bool actorGoal = !intermediate && (memory.intent == BotIntent::FightEnemy
        || memory.intent == BotIntent::ChaseWeakEnemy);
    const bool coreGoal = (memory.intent == BotIntent::PressureCore
        || memory.intent == BotIntent::BreakCoreDefense)
        && !memory.usingAuthoredRoute
        && !memory.usingRouteCorridor;
    std::uint64_t signature = Mix(GoalSignature(
        memory.intent,
        targetSupport,
        actorGoal && targetPlayer != nullptr ? targetPlayer->GetId() : -1,
        coreGoal && targetCore != nullptr ? targetCore->GetTeamId() : -1), intermediate ? 1U : 0U);
    signature = Mix(signature, memory.assignedBridgeAssist ? 1U : 0U);
    if (intermediate || memory.assignedBridgeAssist)
    {
        // Adjacent portal/recovery cells are distinct destinations even when
        // they occupy the same coarse bucket used for moving semantic goals.
        signature = Mix(signature, static_cast<std::uint32_t>(targetSupport.x));
        signature = Mix(signature, static_cast<std::uint32_t>(targetSupport.y));
        signature = Mix(signature, static_cast<std::uint32_t>(targetSupport.z));
        signature = Mix(signature, memory.routeEntryPending ? 1U : 0U);
    }
    const auto previousIntent = botNavigationIntents_.find(bot.GetId());
    const bool intentChanged = previousIntent == botNavigationIntents_.end()
        || previousIntent->second != memory.intent;
    // A moving semantic target may refresh between perception ticks. Keep the
    // current short segment (at most 16 actions) unless the intent itself
    // changed; this prevents target jitter from cancelling A* every tick.
    if (intentChanged || !controller.HasGoal() || !controller.Executor().IsActive()
        || memory.usingAuthoredRoute || memory.usingRouteCorridor || memory.assignedBridgeAssist)
    {
        controller.SetGoal(goal, signature);
        botNavigationIntents_[bot.GetId()] = memory.intent;
    }
    NavigationControllerUpdate update = controller.Update(
        bot, view, profile, dt, matchSimulation_.CurrentTick());
    const NavigationMetrics navigationDelta = controller.ConsumeMetricsDelta();
    navigationMetrics_.Add(navigationDelta);
    if (navigationDelta.failedPathRequests > 0
        && lowBridgeStock && !memory.assignedBridgeAssist
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
            const Vector3 helpPosition = bot.GetPosition();
            // A Core cell is an interaction target, not a place a helper can
            // stand. Both gap and route-entry requests rendezvous with the
            // stranded bot before resuming the shared strategic objective.
            if (bus.PublishBridgeRequest(bot.GetId(), BotIntent::SecureResources, helpPosition, -1, now))
            {
                bus.Broadcast(bot.GetId(), CoordinationSignal::CallingForHelp,
                    helpPosition, now, -1);
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
                        + " velocity=" + std::to_string(bot.GetVelocity().x) + ","
                        + std::to_string(bot.GetVelocity().y) + "," + std::to_string(bot.GetVelocity().z)
                        + " grounded=" + (bot.IsOnGround() ? "1" : "0")
                        + " destinationClear=" + (view.IsBodyClear(update.failedMovementTo, profile) ? "1" : "0")
                        + " intent=" + std::to_string(static_cast<int>(memory.intent))
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
    if (goalSatisfied && !intermediate && memory.assignedBridgeAssist)
    {
        if (bot.GetTeamId() >= 0 && bot.GetTeamId() < static_cast<int>(teamCoordBuses_.size()))
        {
            auto& bus = teamCoordBuses_[static_cast<std::size_t>(bot.GetTeamId())];
            const int requestor = bus.bridgeRequestorId;
            const bool completed = bus.CompleteBridgeRequest(bot.GetId());
            if (completed)
            {
                // A rendezvous on the same island does not repair the onward
                // route. Share real inventory through the ordinary drop action
                // so the requester can build; keep an emergency stack ourselves.
                for (const auto& requester : players_)
                {
                    if (requester.GetId() != requestor || !requester.IsAlive() || requester.IsEliminated()
                        || requester.GetTeamId() != bot.GetTeamId() || !bot.IsOnGround() || !requester.IsOnGround()
                        || requester.GetInventory().GetBlocks() >= 4
                        || DistanceSquared(bot.GetPosition(), requester.GetPosition()) > 4.0f
                        || !BotHasLineOfSight(bot, requester)) continue;
                    const auto supply = BuildNavigationProfile(bot);
                    const int amount = std::min(16, supply.availableBridgeBlocks - 4);
                    if (amount < 4) continue;
                    for (int slot = 0; slot < kInventorySlotCount; ++slot)
                    {
                        const ItemStack stack = bot.GetInventory().GetSlot(slot);
                        if (stack.type != ItemFromBlock(supply.preferredBridgeBlock) || stack.count < 4) continue;
                        command.actionSeq = std::max(command.tick * 32u + 31u, economyActionSeq_[bot.GetId()] + 1u);
                        command.actionType = static_cast<int>(PlayerActionType::DropItem);
                        command.actionParamA = slot;
                        command.actionParamB = std::min(amount, stack.count);
                        AimAt(command, bot.GetPosition(), requester.GetPosition());
                        command.moveForward = command.moveStrafe = 0.0f;
                        command.sneak = true;
                        break;
                    }
                }
            }
            if (completed && bus.bridgeHelpFollowups.size() < 16)
            {
                for (const auto& requester : players_) if (requester.GetId() == requestor)
                    for (const auto& requesterMemory : botMemories_)
                        if (requesterMemory.playerId == requestor)
                        {
                            const Vector3 objective = requesterMemory.routeEntryPending
                                ? requesterMemory.routeEntryObjective : requesterMemory.objectiveTarget;
                            bus.bridgeHelpFollowups.push_back({ requestor, bot.GetId(), objective,
                                std::sqrt(DistanceSquared(requester.GetPosition(), objective)),
                                matchSimulation_.MatchTimeSeconds() });
                        }
            }
            if (completed && automatch_.active
                && std::count_if(automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
                    [](const AutomatchTimelineEvent& event) { return event.type == "bridgeHelpArrived"; }) < 128)
                automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                    matchSimulation_.MatchTimeSeconds(), "bridgeHelpArrived", bot.GetTeamId(), bot.GetTeamId(),
                    bot.GetId(), requestor, bot.GetInventory().GetBlocks(), "helper reached request destination" });
        }
        return true;
    }
    if (goalSatisfied && !intermediate && targetCore != nullptr
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
    if (update.hasCommand || update.planned || controller.Executor().IsActive()) return true;
    if (memory.routeEntryPending || (controller.HasGoal() && !goalSatisfied
        && !update.blockedStart && !update.routeAbandoned))
    {
        // A search cooldown is not permission for legacy movement to push
        // toward the raw objective. Brake until navigation has an executable
        // action or an explicit strategic recovery replaces this objective.
        command.moveForward = 0.0f;
        command.moveStrafe = 0.0f;
        command.jump = false;
        command.sprint = false;
        command.sneak = true;
        return true;
    }
    return false;
}

Vector3 Game::ChooseBotRouteCorridorWaypoint(Player& bot, Vector3 finalTarget)
{
    BotMemory& memory = GetBotMemory(bot);
    const bool wasUsingCorridor = memory.usingRouteCorridor;
    memory.usingRouteCorridor = false;
    memory.routeCorridorBridgeSegment = false;
    memory.routeCorridorExpectedBridgeBlocks = 0;
    if (routeGraph_.Empty())
    {
        memory.routeEntryPending = false;
        memory.routeEntryRepair = false;
        memory.routeCorridorNodes.clear();
        memory.routeCorridorTraversal.clear();
        memory.routeCorridorBridgeBlocks.clear();
        memory.hasRouteCorridorObjective = false;
        return finalTarget;
    }

    const bool objectiveChanged = !memory.hasRouteCorridorObjective
        || DistanceSquared(memory.routeCorridorObjective, finalTarget) > 100.0f;
    if (memory.routeEntryPending && DistanceSquared(memory.routeEntryObjective, finalTarget) <= 100.0f)
    {
        const float distance = DistanceSquared(bot.GetPosition(), memory.routeEntryTarget);
        const float now = matchSimulation_.MatchTimeSeconds();
        if (distance + 1.0f < memory.routeCorridorProgressDistanceSq)
        {
            memory.routeCorridorProgressDistanceSq = distance;
            memory.routeCorridorLastProgressTimestamp = now;
        }
        if (distance > 1.0f && now - memory.routeCorridorLastProgressTimestamp < 12.0f)
        {
            memory.usingRouteCorridor = true;
            return memory.routeEntryTarget;
        }
        if (distance > 1.0f)
        {
            memory.repeatedRouteFailures = std::min(6, memory.repeatedRouteFailures + 1);
            memory.routeFailureCooldown = std::max(memory.routeFailureCooldown, 8.0f);
            memory.lastRouteFailurePosition = bot.GetPosition();
            memory.hasLastRouteFailure = true;
            memory.strategicUpdateTimer = 0.0f;
        }
    }
    if (objectiveChanged && memory.routeCorridorReplanCooldown > 0.0f)
    {
        if (!memory.routeEntryPending || DistanceSquared(memory.routeEntryObjective, finalTarget) > 100.0f)
        {
            memory.routeEntryTarget = bot.GetPosition();
            memory.routeEntryRepair = false;
        }
        memory.routeEntryPending = true;
        memory.routeEntryObjective = finalTarget;
        memory.usingRouteCorridor = true;
        return memory.routeEntryTarget;
    }

    if (objectiveChanged || memory.routeCorridorNodes.empty())
    {
        memory.routeEntryRepair = false;
        const GridPos failedPosition = world_.WorldToGrid(memory.lastRouteFailurePosition);
        const GridPos* avoidFailure = memory.hasLastRouteFailure
            && memory.routeFailureCooldown > 0.0f ? &failedPosition : nullptr;
        NavigationProfileSettings entrySettings;
        entrySettings.gravityMultiplier = BiomeGravityMultiplier();
        entrySettings.jumpMultiplier = BiomeJumpMultiplier();
        entrySettings.terrainSpeedMultiplier = TerrainSpeedMultiplier(bot);
        const auto& entryTuning = BotTuningForTeam(bot.GetTeamId());
        entrySettings.gapRunupDistanceBlocks = entryTuning.navigationRunupDistanceBlocks;
        entrySettings.gapTakeoffDelaySeconds = entryTuning.navigationTakeoffDelaySeconds;
        entrySettings.gapTakeoffEdgeOffsetBlocks = entryTuning.navigationTakeoffEdgeOffsetBlocks;
        entrySettings.gapTakeoffGapScale = entryTuning.navigationTakeoffGapScale;
        entrySettings.gapAirControlScale = entryTuning.navigationAirControlScale;
        entrySettings.gapLandingCorrectionGain = entryTuning.navigationLandingCorrectionGain;
        entrySettings.maxSafeDropBlocks = 8;
        // Join the authored network over existing terrain. A geometrically
        // close portal across the void is not an entrance to the current island.
        entrySettings.allowBlockPlacement = false;
        entrySettings.allowBlockBreaking = false;
        entrySettings.allowBridging = false;
        const NavigationProfile entryProfile = BuildNavigationProfile(bot, entrySettings);
        NavigationWorldView entryView(world_, bot.GetTeamId());
        const auto entrySupport = entryView.FindSupport(bot.GetPosition(), 5, 2,
            entryProfile.bodyCenterAboveSupport);
        NavigationSearchLimits entryLimits;
        entryLimits.maxExpansions = 3200;
        entryLimits.maxActions = 64;
        entryLimits.maxSearchRadius = 48;
        entryLimits.allowPartial = true;
        int entryAttempts = 0;
        std::string entryOutcomes;
        std::optional<Vector3> partialApproach;
        const auto canReachEntry = [&](const CreativeRouteNode& node) {
            if (!entrySupport || entryAttempts >= 4) return false;
            ++entryAttempts;
            const auto support = entryView.FindSupport(world_.GridToWorld(node.pos), 12, 2,
                entryProfile.bodyCenterAboveSupport);
            if (!support) return false;
            const GoalWithinRadius entryGoal(*support, 0.8f);
            const NavigationSearchResult result = VoxelPathfinder {}.FindPath(
                NavigationState { *entrySupport, 0, 0 }, entryGoal, entryView, entryProfile, entryLimits);
            if (automatch_.active) entryOutcomes += node.id + ":" + ToString(result.status) + " ";
            if (!partialApproach && result.status == NavigationSearchStatus::Partial && result.HasPath())
            {
                const Vector3 approach = entryView.SupportCenter(result.path.resolvedGoal,
                    entryProfile.bodyCenterAboveSupport);
                if (DistanceSquared(bot.GetPosition(), approach) > 1.0f) partialApproach = approach;
            }
            return result.status == NavigationSearchStatus::Success
                || result.status == NavigationSearchStatus::AlreadySatisfied;
        };
        const RouteCorridor corridor = routeGraph_.FindCorridor(
            world_.WorldToGrid(bot.GetPosition()),
            world_.WorldToGrid(finalTarget),
            bot.GetTeamId(),
            avoidFailure,
            55.0f * static_cast<float>(std::max(1, memory.repeatedRouteFailures)), canReachEntry);
        memory.routeCorridorReplanCooldown = 1.25f;
        if (!corridor.valid || corridor.nodeIndices.empty())
        {
            // Prefer existing terrain. Only after that search fails, admit a
            // fully planned short repair to a real portal, never a partial
            // construction path that may consume the stack over the void.
            if (entrySupport && bot.GetInventory().GetBlocks() >= 4)
            {
                entrySettings.allowBlockPlacement = true;
                entrySettings.allowBridging = true;
                entrySettings.maxConsecutiveBridgeBlocks = 8;
                entrySettings.reserveBridgeBlocks = 2;
                entrySettings.terrainSpeedMultiplier = TerrainSpeedMultiplier(bot);
                const auto repairProfile = BuildNavigationProfile(bot, entrySettings);
                auto repairLimits = entryLimits;
                repairLimits.allowPartial = false;
                int repairAttempts = 0;
                std::optional<Vector3> repairedEntry;
                const auto repairable = [&](const CreativeRouteNode& node) {
                    if (++repairAttempts > 4) return false;
                    const auto support = entryView.FindSupport(world_.GridToWorld(node.pos), 12, 2,
                        repairProfile.bodyCenterAboveSupport);
                    if (!support) return false;
                    const auto result = VoxelPathfinder {}.FindPath(NavigationState { *entrySupport, 0, 0 },
                        GoalWithinRadius(*support, 0.8f), entryView, repairProfile, repairLimits);
                    if (result.status != NavigationSearchStatus::Success) return false;
                    repairedEntry = entryView.SupportCenter(*support, repairProfile.bodyCenterAboveSupport);
                    return true;
                };
                const auto repaired = routeGraph_.FindCorridor(world_.WorldToGrid(bot.GetPosition()),
                    world_.WorldToGrid(finalTarget), bot.GetTeamId(), avoidFailure,
                    55.0f * static_cast<float>(std::max(1, memory.repeatedRouteFailures)), repairable);
                if (repaired.valid && repairedEntry)
                {
                    memory.routeEntryRepair = true;
                    partialApproach = repairedEntry;
                    if (automatch_.active && std::count_if(automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
                        [](const AutomatchTimelineEvent& event) { return event.type == "routeEntryRepair"; }) < 128)
                        automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                            matchSimulation_.MatchTimeSeconds(), "routeEntryRepair", bot.GetTeamId(), bot.GetTeamId(),
                            bot.GetId(), -1, repairProfile.availableBridgeBlocks, "complete stocked path to route entrance" });
                }
            }
            if (automatch_.active && entrySupport
                && std::count_if(automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
                    [](const AutomatchTimelineEvent& event) { return event.type == "routeEntryFailure"; }) < 40)
                automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                    matchSimulation_.MatchTimeSeconds(), "routeEntryFailure", bot.GetTeamId(), bot.GetTeamId(),
                    bot.GetId(), -1, entryAttempts,
                    "start=" + std::to_string(entrySupport->x) + "," + std::to_string(entrySupport->y)
                        + "," + std::to_string(entrySupport->z) + " " + entryOutcomes });
            memory.routeCorridorNodes.clear();
            memory.routeCorridorTraversal.clear();
            memory.routeCorridorBridgeBlocks.clear();
            memory.hasRouteCorridorObjective = false;
            memory.routeEntryPending = true;
            memory.routeEntryObjective = finalTarget;
            memory.routeEntryTarget = partialApproach.value_or(bot.GetPosition());
            memory.routeCorridorProgressDistanceSq = DistanceSquared(bot.GetPosition(), memory.routeEntryTarget);
            memory.routeCorridorLastProgressTimestamp = matchSimulation_.MatchTimeSeconds();
            memory.usingRouteCorridor = true;
            if (!memory.assignedBridgeAssist && bot.GetInventory().GetBlocks() < 4
                && bot.GetTeamId() >= 0 && bot.GetTeamId() < static_cast<int>(teamCoordBuses_.size()))
            {
                auto& bus = teamCoordBuses_[static_cast<std::size_t>(bot.GetTeamId())];
                // The helper must reach the stranded teammate, not inherit
                // its far-away attack or shopping objective.
                memory.waitingForBridgeHelp = bus.PublishBridgeRequest(bot.GetId(),
                    BotIntent::SecureResources, bot.GetPosition(), -1, matchSimulation_.MatchTimeSeconds());
            }
            ++navigationMetrics_.corridorFailures;
            return memory.routeEntryTarget;
        }

        memory.routeEntryPending = false;
        memory.routeCorridorNodes = corridor.nodeIndices;
        memory.routeCorridorTraversal.clear();
        memory.routeCorridorBridgeBlocks.clear();
        memory.routeCorridorTraversal.reserve(corridor.segments.size());
        memory.routeCorridorBridgeBlocks.reserve(corridor.segments.size());
        for (const RouteCorridorSegment& segment : corridor.segments)
        {
            memory.routeCorridorTraversal.push_back(segment.IsBridge() ? 1 : 0);
            memory.routeCorridorBridgeBlocks.push_back(segment.expectedBridgeBlocks);
        }
        memory.routeCorridorIndex = 0;
        memory.routeCorridorObjective = finalTarget;
        memory.hasRouteCorridorObjective = true;
        memory.routeCorridorSignature = corridor.signature;
        memory.hasRouteCorridorLastAdvancePosition = false;
        memory.routeCorridorWaitingForBuilder = false;
        memory.routeCorridorLastProgressTimestamp = matchSimulation_.MatchTimeSeconds();
        memory.routeCorridorProgressPosition = bot.GetPosition();
        memory.routeCorridorProgressIndex = 0;
        memory.routeCorridorProgressDistanceSq = std::numeric_limits<float>::max();
        memory.hasNavWaypoint = false;
        ++navigationMetrics_.corridorPlans;
    }
    else if (!wasUsingCorridor
        && memory.routeCorridorIndex < static_cast<int>(memory.routeCorridorNodes.size()))
    {
        // Tactical combat/defense temporarily interrupted, but did not erase,
        // the strategic route. Resuming it is cheaper and more human-readable
        // than planning an unrelated lane from scratch.
        ++navigationMetrics_.corridorReuses;
    }

    const int count = static_cast<int>(memory.routeCorridorNodes.size());
    memory.routeCorridorIndex = std::clamp(memory.routeCorridorIndex, 0, count);
    TeamCoordinationBus* coordBus = bot.GetTeamId() >= 0
        && bot.GetTeamId() < static_cast<int>(teamCoordBuses_.size())
        ? &teamCoordBuses_[static_cast<std::size_t>(bot.GetTeamId())]
        : nullptr;
    const auto bridgeSegmentForTarget = [&memory](int targetIndex)
    {
        const int segmentIndex = targetIndex - 1;
        return segmentIndex >= 0
            && segmentIndex < static_cast<int>(memory.routeCorridorTraversal.size())
            && memory.routeCorridorTraversal[static_cast<std::size_t>(segmentIndex)] != 0;
    };
    const auto bridgeSignatureForTarget = [&memory](int targetIndex) -> std::uint64_t
    {
        const int segmentIndex = targetIndex - 1;
        if (segmentIndex < 0
            || targetIndex >= static_cast<int>(memory.routeCorridorNodes.size()))
        {
            return 0;
        }
        const std::uint32_t from = static_cast<std::uint32_t>(
            memory.routeCorridorNodes[static_cast<std::size_t>(segmentIndex)] + 1);
        const std::uint32_t to = static_cast<std::uint32_t>(
            memory.routeCorridorNodes[static_cast<std::size_t>(targetIndex)] + 1);
        const std::uint32_t low = std::min(from, to);
        const std::uint32_t high = std::max(from, to);
        return (static_cast<std::uint64_t>(low) << 32u) | high;
    };
    while (memory.routeCorridorIndex < count)
    {
        const CreativeRouteNode* node = routeGraph_.Node(
            memory.routeCorridorNodes[static_cast<std::size_t>(memory.routeCorridorIndex)]);
        if (node == nullptr)
        {
            memory.routeCorridorNodes.clear();
            memory.routeCorridorTraversal.clear();
            memory.routeCorridorBridgeBlocks.clear();
            memory.hasRouteCorridorObjective = false;
            ++navigationMetrics_.corridorFailures;
            return finalTarget;
        }

        const Vector3 nodePosition = world_.GridToWorld(node->pos);
        const float nodeDistanceSq = DistanceSquared(bot.GetPosition(), nodePosition);
        const bool firstPortal = memory.routeCorridorIndex == 0;
        // The six-block portal radius used to overlap several castle ingress
        // nodes.  Combined with a separate three-block movement requirement,
        // nodes spaced 2.8-3 blocks apart could never advance.  Only the first
        // coarse portal keeps the generous radius; subsequent nodes require a
        // real visit to their support cell.  Advance at most one per frame.
        const bool reached = nodeDistanceSq <= (firstPortal ? 36.0f : 4.0f);
        if (!reached) break;

        const bool completedBridge = bridgeSegmentForTarget(memory.routeCorridorIndex);
        const std::uint64_t completedBridgeSignature = completedBridge
            ? bridgeSignatureForTarget(memory.routeCorridorIndex) : 0;
        if (completedBridge && memory.routeCorridorBridgeStarted
            && completedBridgeSignature == memory.routeCorridorActiveBridgeSignature)
        {
            ++navigationMetrics_.routeBridgeSegmentsCompleted;
            memory.routeCorridorBridgeStarted = false;
            memory.routeCorridorActiveBridgeSignature = 0;
            memory.routeCorridorWaitingForBuilder = false;
            if (coordBus != nullptr)
            {
                coordBus->MarkRouteOpened(
                    completedBridgeSignature, matchSimulation_.MatchTimeSeconds());
                if (coordBus->bridgeRouteSignature == completedBridgeSignature)
                {
                    coordBus->bridgeBuilderId = -1;
                    coordBus->bridgeReservationUntil = -1000.0f;
                    coordBus->bridgeLastProgressTimestamp = -1000.0f;
                    coordBus->bridgeLastProgressBlocks = -1;
                }
            }
        }

        ++memory.routeCorridorIndex;
        ++memory.routeCorridorAdvances;
        ++navigationMetrics_.corridorAdvances;
        memory.routeCorridorLastAdvancePosition = bot.GetPosition();
        memory.hasRouteCorridorLastAdvancePosition = true;
        memory.routeCorridorLastProgressTimestamp = matchSimulation_.MatchTimeSeconds();
        memory.routeCorridorProgressPosition = bot.GetPosition();
        memory.routeCorridorProgressIndex = memory.routeCorridorIndex;
        memory.routeCorridorProgressDistanceSq = std::numeric_limits<float>::max();
        memory.hasNavWaypoint = false;
        break;
    }

    if (memory.routeCorridorIndex >= count)
    {
        return finalTarget;
    }
    const CreativeRouteNode* next = routeGraph_.Node(
        memory.routeCorridorNodes[static_cast<std::size_t>(memory.routeCorridorIndex)]);
    if (next == nullptr) return finalTarget;
    const float now = matchSimulation_.MatchTimeSeconds();
    const float nextDistanceSq = DistanceSquared(bot.GetPosition(), world_.GridToWorld(next->pos));
    if (memory.routeCorridorProgressIndex != memory.routeCorridorIndex
        || nextDistanceSq + 1.0f < memory.routeCorridorProgressDistanceSq)
    {
        memory.routeCorridorProgressIndex = memory.routeCorridorIndex;
        memory.routeCorridorProgressDistanceSq = nextDistanceSq;
        memory.routeCorridorLastProgressTimestamp = now;
        memory.routeCorridorProgressPosition = bot.GetPosition();
    }
    if (now - memory.routeCorridorLastProgressTimestamp > 16.0f
        && !memory.routeCorridorWaitingForBuilder)
    {
        if (coordBus != nullptr && coordBus->bridgeBuilderId == bot.GetId())
        {
            coordBus->bridgeBuilderId = -1;
            coordBus->bridgeReservationUntil = -1000.0f;
            coordBus->bridgeLastProgressTimestamp = -1000.0f;
            coordBus->bridgeLastProgressBlocks = -1;
        }
        memory.lastRouteFailurePosition = bot.GetPosition();
        memory.hasLastRouteFailure = true;
        memory.repeatedRouteFailures = std::max(1, memory.repeatedRouteFailures + 1);
        memory.routeFailureCooldown = std::max(memory.routeFailureCooldown, 8.0f);
        memory.routeCorridorNodes.clear();
        memory.routeCorridorTraversal.clear();
        memory.routeCorridorBridgeBlocks.clear();
        memory.hasRouteCorridorObjective = false;
        memory.routeCorridorReplanCooldown = 0.0f;
        memory.routeCorridorBridgeSegment = false;
        memory.routeCorridorWaitingForBuilder = false;
        ++navigationMetrics_.corridorFailures;
        return finalTarget;
    }
    memory.usingRouteCorridor = true;
    const int segmentIndex = memory.routeCorridorIndex - 1;
    const bool bridgeSegment = bridgeSegmentForTarget(memory.routeCorridorIndex);
    if (bridgeSegment)
    {
        const std::uint64_t activeBridgeSignature =
            bridgeSignatureForTarget(memory.routeCorridorIndex);
        const bool routeAlreadyOpened = coordBus != nullptr
            && coordBus->IsRouteOpened(
                activeBridgeSignature, matchSimulation_.MatchTimeSeconds());
        if (coordBus != nullptr
            && coordBus->bridgeBuilderId >= 0
            && coordBus->bridgeRouteSignature == activeBridgeSignature
            && matchSimulation_.MatchTimeSeconds() - coordBus->bridgeLastProgressTimestamp > 8.0f)
        {
            // A lease is backed by physical progress, not by repeatedly asking
            // for the same waypoint.  Let a waiting rusher/fighter take over.
            coordBus->bridgeBuilderId = -1;
            coordBus->bridgeReservationUntil = -1000.0f;
            coordBus->bridgeLastProgressTimestamp = -1000.0f;
            coordBus->bridgeLastProgressBlocks = -1;
        }
        if (!routeAlreadyOpened && coordBus != nullptr && memory.routeFailureCooldown <= 0.0f)
            coordBus->TryReserveBridge(bot.GetId(), activeBridgeSignature,
                matchSimulation_.MatchTimeSeconds(), bot.GetPosition(), bot.GetInventory().GetBlocks());
        const bool isBuilder = routeAlreadyOpened || coordBus == nullptr
            || (coordBus->bridgeBuilderId == bot.GetId() && coordBus->bridgeRouteSignature == activeBridgeSignature);
        if (!isBuilder)
        {
            if (!memory.routeCorridorWaitingForBuilder)
            {
                ++navigationMetrics_.routeBridgeFollowersHeld;
                memory.routeCorridorWaitingForBuilder = true;
            }
            const RouteCorridorSegment waitingSegment {
                memory.routeCorridorNodes[static_cast<std::size_t>(segmentIndex)],
                memory.routeCorridorNodes[static_cast<std::size_t>(memory.routeCorridorIndex)],
                "bridge", 0 };
            const CreativeRouteNode* waitAt = routeGraph_.Node(waitingSegment.fromNodeIndex);
            memory.routeCorridorBridgeSegment = false;
            memory.routeCorridorLastProgressTimestamp = matchSimulation_.MatchTimeSeconds();
            if (waitAt != nullptr)
            {
                const Vector3 portal = world_.GridToWorld(waitAt->pos);
                const Vector3 bridgeDirection = NormalizeCorridorDirection(Vector3 {
                    static_cast<float>(next->pos.x - waitAt->pos.x), 0.0f,
                    static_cast<float>(next->pos.z - waitAt->pos.z) });
                // Keep the construction takeoff clear. Select an actual
                // supported waiting point behind it, with room for teammates.
                for (int attempt = 0; attempt < 4; ++attempt)
                {
                    const float side = (bot.GetId() % 2 == 0 ? 1.0f : -1.0f) * (attempt < 2 ? 1.5f : 0.0f);
                    const float back = 2.5f + static_cast<float>(attempt % 2) * 1.5f;
                    const Vector3 candidate { portal.x - bridgeDirection.x * back - bridgeDirection.z * side,
                        portal.y + 0.4f, portal.z - bridgeDirection.z * back + bridgeDirection.x * side };
                    const GridPos support = FindCorridorSupport(world_, candidate, 2);
                    const Vector3 standing { static_cast<float>(support.x), static_cast<float>(support.y) + 1.4f,
                        static_cast<float>(support.z) };
                    if (world_.IsSolid(support) && !world_.CollidesWithAABB(standing, Vector3 { 0.32f, 0.89f, 0.32f }))
                        return standing;
                }
                return portal;
            }
            return world_.GridToWorld(next->pos);
        }

        memory.routeCorridorWaitingForBuilder = false;
        // An opened span can be damaged later. Every returning or advancing
        // role retains the authored construction budget to repair its holes.
        memory.routeCorridorBridgeSegment = true;
        memory.routeCorridorExpectedBridgeBlocks = segmentIndex >= 0
            && segmentIndex < static_cast<int>(memory.routeCorridorBridgeBlocks.size())
            ? memory.routeCorridorBridgeBlocks[static_cast<std::size_t>(segmentIndex)]
            : 0;
        if (!routeAlreadyOpened
            && memory.routeCorridorActiveBridgeSignature != activeBridgeSignature)
        {
            memory.routeCorridorActiveBridgeSignature = activeBridgeSignature;
            memory.routeCorridorBridgeStarted = true;
            ++navigationMetrics_.routeBridgeSegmentsStarted;
        }
        if (!routeAlreadyOpened && coordBus != nullptr)
        {
            const int blocksNow = bot.GetInventory().GetBlocks();
            const bool placedBlock = coordBus->bridgeLastProgressBlocks >= 0
                && blocksNow < coordBus->bridgeLastProgressBlocks;
            const bool movedForward = DistanceSquared(
                bot.GetPosition(), coordBus->bridgeLastProgressPosition) >= 4.0f;
            if (placedBlock || movedForward)
            {
                coordBus->bridgeLastProgressTimestamp = matchSimulation_.MatchTimeSeconds();
                coordBus->bridgeLastProgressPosition = bot.GetPosition();
                coordBus->bridgeLastProgressBlocks = blocksNow;
                coordBus->bridgeReservationUntil = matchSimulation_.MatchTimeSeconds() + 8.0f;
            }
            coordBus->Broadcast(bot.GetId(), CoordinationSignal::BuildingBridge,
                world_.GridToWorld(next->pos), matchSimulation_.MatchTimeSeconds(),
                memory.currentPlan.targetTeamId);
        }
    }
    else
    {
        memory.routeCorridorWaitingForBuilder = false;
    }
    return world_.GridToWorld(next->pos);
}

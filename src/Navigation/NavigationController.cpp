#include "Navigation/NavigationController.h"

#include "Inventory.h"
#include "Navigation/NavigationActionLibrary.h"
#include "Navigation/NavigationWorldView.h"
#include "Player.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

NavigationController::NavigationController(NavigationControllerSettings settings)
    : settings_(settings), executor_(PathExecutorSettings {})
{
    settings_.maxSegmentActions = std::clamp(settings_.maxSegmentActions, 1, 64);
    settings_.corridorMinimumLength = std::max(1.0f, settings_.corridorMinimumLength);
    settings_.corridorHalfWidth = std::clamp(settings_.corridorHalfWidth, 2.0f, 64.0f);
    settings_.corridorVerticalPadding = std::clamp(settings_.corridorVerticalPadding, 2, 64);
    settings_.segmentRepairLookAhead = std::clamp(settings_.segmentRepairLookAhead, 1, 16);
    settings_.segmentRepairReconnectActions = std::clamp(
        settings_.segmentRepairReconnectActions, 1, 12);
    settings_.segmentRepairMaxExpansions = std::clamp(
        settings_.segmentRepairMaxExpansions, 64, 3200);
    settings_.segmentRepairMaxActions = std::clamp(settings_.segmentRepairMaxActions, 4, 64);
    settings_.segmentRepairRadius = std::clamp(settings_.segmentRepairRadius, 4, 64);
    settings_.segmentRepairCorridorHalfWidth = std::clamp(
        settings_.segmentRepairCorridorHalfWidth, 2.0f, 24.0f);
    settings_.maxActionFailuresBeforeAbandon = std::clamp(
        settings_.maxActionFailuresBeforeAbandon, 2, 8);
}

void NavigationController::SetGoal(NavigationGoalPtr goal, std::uint64_t signature)
{
    if (goal_ != nullptr && goal != nullptr && signature == goalSignature_)
    {
        return;
    }
    const bool replacingGoal = goal_ != nullptr;
    goal_ = std::move(goal);
    goalSignature_ = signature;
    goalSatisfied_ = false;
    pendingRepath_ = RepathReason::GoalChanged;
    lastRepath_ = RepathReason::GoalChanged;
    executor_.Cancel(RepathReason::GoalChanged, "navigation goal changed");
    if (replacingGoal)
    {
        metrics_.RecordRepath(RepathReason::GoalChanged);
        metricsDelta_.RecordRepath(RepathReason::GoalChanged);
    }
    repathCooldown_ = 0.0f;
    consecutiveActionFailures_ = 0;
    hasRecoveredAction_ = false;
}

void NavigationController::ClearGoal()
{
    goal_.reset();
    goalSignature_ = 0;
    goalSatisfied_ = false;
    pendingRepath_ = RepathReason::None;
    executor_.Reset();
    consecutiveActionFailures_ = 0;
    hasRecoveredAction_ = false;
    debug_ = NavigationDebugSnapshot {};
}

void NavigationController::ForceRepath(RepathReason reason)
{
    if (reason == RepathReason::None) reason = RepathReason::MovementFailed;
    pendingRepath_ = reason;
    lastRepath_ = reason;
    executor_.Cancel(reason, "forced navigation replan");
    repathCooldown_ = 0.0f;
}

NavigationControllerUpdate NavigationController::Update(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile& profile,
    float deltaSeconds,
    std::uint32_t tick)
{
    NavigationControllerUpdate update;
    update.command.controlledPlayerId = static_cast<std::uint32_t>(actor.GetId());
    update.command.tick = tick;
    update.command.aimYaw = actor.GetYaw();
    update.command.selectedSlot = actor.GetSelectedSlot();
    deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.25f);
    repathCooldown_ = std::max(0.0f, repathCooldown_ - deltaSeconds);
    arbiter_.Clear();

    if (goal_ == nullptr)
    {
        RefreshDebug(world);
        return update;
    }

    const std::optional<GridPos> support = world.FindSupport(
        actor.GetPosition(), profile.maxSafeDropBlocks + 2, 2, profile.bodyCenterAboveSupport);
    if (!support.has_value())
    {
        update.supportLost = hadSupportLastUpdate_ && !fallThreatActive_;
        hadSupportLastUpdate_ = false;
        fallThreatActive_ = true;
        if (const PlannedMovement* movement = executor_.CurrentMovement())
        {
            update.supportLostMovementType = movement->type;
            update.supportLostMovementFrom = movement->from;
            update.supportLostMovementTo = movement->to;
        }
        emergencyTimer_ += deltaSeconds;
        const BotControlProposal emergency = BuildEmergencyProposal(actor, world, profile, tick);
        arbiter_.Submit(emergency);
        if (emergency.active)
        {
            const BotControlResolution resolution = arbiter_.Resolve(update.command);
            update.command = resolution.command;
            update.hasCommand = true;
            update.repathReason = RepathReason::EmergencySave;
        }
        RefreshDebug(world);
        return update;
    }
    hadSupportLastUpdate_ = true;

    const PlannedMovement* activeMovement = executor_.CurrentMovement();
    const Vector3 supportCenter = world.SupportCenter(*support, profile.bodyCenterAboveSupport);
    const float heightAboveNearestSupport = actor.GetPosition().y - supportCenter.y;
    bool intentionalAir = false;
    if (activeMovement != nullptr
        && (activeMovement->type == MovementType::JumpUp
            || activeMovement->type == MovementType::GapJump
            || activeMovement->type == MovementType::DropDown))
    {
        const float landingCenterY = world.SupportCenter(
            activeMovement->to, profile.bodyCenterAboveSupport).y;
        intentionalAir = actor.GetPosition().y >= landingCenterY - 0.45f;
    }
    const bool earlyFallThreat = !actor.IsOnGround()
        && actor.GetVelocity().y < -0.35f
        && heightAboveNearestSupport > 1.05f
        && !intentionalAir;
    if (earlyFallThreat)
    {
        update.supportLost = !fallThreatActive_;
        fallThreatActive_ = true;
        if (activeMovement != nullptr)
        {
            update.supportLostMovementType = activeMovement->type;
            update.supportLostMovementFrom = activeMovement->from;
            update.supportLostMovementTo = activeMovement->to;
        }
        emergencyTimer_ += deltaSeconds;
        // Pull back over the nearest known support while braking. Merely
        // releasing movement leaves existing air velocity untouched long
        // enough to miss a one-block bridge.
        const float returnX = supportCenter.x - actor.GetPosition().x;
        const float returnZ = supportCenter.z - actor.GetPosition().z;
        if (returnX * returnX + returnZ * returnZ > 0.0025f)
        {
            update.command.aimYaw = std::atan2(returnX, -returnZ);
            update.command.moveForward = 0.55f;
        }
        update.command.sneak = true;
        update.hasCommand = true;
        const BotControlProposal emergency = BuildEmergencyProposal(actor, world, profile, tick);
        arbiter_.Submit(emergency);
        if (emergency.active)
        {
            const BotControlResolution resolution = arbiter_.Resolve(update.command);
            update.command = resolution.command;
            update.repathReason = RepathReason::EmergencySave;
        }
        RefreshDebug(world);
        return update;
    }
    if (actor.IsOnGround())
    {
        fallThreatActive_ = false;
    }
    emergencyTimer_ = 0.0f;
    emergencyIssued_ = false;

    // A body temporarily embedded by dynamic construction is not a valid A*
    // start. Defer instead of issuing thousands of guaranteed InvalidStart
    // searches; returning without a command lets the caller's local obstacle
    // recovery break or step away from the blocking cell.
    if (!world.IsBodyClear(*support, profile))
    {
        blockedStartSeconds_ += deltaSeconds;
        update.blockedStart = true;
        update.blockedStartSupport = *support;
        if (blockedStartSeconds_ >= 0.45f)
        {
            std::optional<GridPos> relocation;
            for (int radius = 1; radius <= 4 && !relocation.has_value(); ++radius)
            {
                for (int dy = -1; dy <= 4 && !relocation.has_value(); ++dy)
                {
                    for (int dx = -radius; dx <= radius && !relocation.has_value(); ++dx)
                    {
                        for (int dz = -radius; dz <= radius; ++dz)
                        {
                            if (std::max(std::abs(dx), std::abs(dz)) != radius) continue;
                            const GridPos candidate {
                                support->x + dx, support->y + dy, support->z + dz };
                            if (world.IsSupported(candidate)
                                && world.IsBodyClear(candidate, profile))
                            {
                                relocation = candidate;
                                break;
                            }
                        }
                    }
                }
            }
            if (relocation.has_value())
            {
                executor_.Reset();
                pendingRepath_ = RepathReason::ActorDisplaced;
                update.blockedStartRecovered = true;
                update.blockedStartRelocated = true;
                update.blockedStartRelocationSupport = *relocation;
                blockedStartSeconds_ = 0.0f;
                ++metrics_.blockedStartRecoveries;
                ++metricsDelta_.blockedStartRecoveries;
                RefreshDebug(world);
                return update;
            }
        }
        const PlannedMovement* current = executor_.CurrentMovement();
        const bool recoveringBlockedStart = executor_.IsActive()
            && current != nullptr && current->type == MovementType::BreakBlock
            && current->from == *support && current->to == *support;
        if (!recoveringBlockedStart)
        {
            GridPos blocker {};
            const int blockers = world.CountBlockingBodyCells(
                *support, profile, &blocker, 1);
            const Block* block = blockers > 0 ? world.GetBlock(blocker) : nullptr;
            update.blockedStartBlock = blocker;
            update.blockedStartBlockType = block != nullptr ? block->type : BlockType::Air;
            if (block != nullptr && block->breakable
                && IsBreakableByPlayers(block->type))
            {
                NavigationPath recovery;
                recovery.start = *support;
                recovery.resolvedGoal = *support;
                recovery.worldRevision = world.WorldRevision();
                PlannedMovement movement;
                movement.type = MovementType::BreakBlock;
                movement.from = *support;
                movement.to = *support;
                movement.affectedBlock = blocker;
                movement.timeCost = BreakSeconds(block->type, profile.pickaxeLevel);
                movement.plannedWorldRevision = recovery.worldRevision;
                recovery.movements.push_back(movement);
                executor_.SetPath(std::move(recovery));
                pendingRepath_ = RepathReason::None;
                update.blockedStartRecovered = true;
                ++metrics_.blockedStartRecoveries;
                ++metricsDelta_.blockedStartRecoveries;
            }
            else
            {
                executor_.Reset();
                pendingRepath_ = RepathReason::WorldChanged;
                if (block != nullptr)
                {
                    // Imported partial shapes and external displacement can
                    // leave the actor overlapping an unbreakable body cell.
                    // Move away from its centre under edge protection; Player
                    // performs a bounded depenetration before applying this
                    // command when the actual AABB is embedded too.
                    const Vector3 actorPosition = actor.GetPosition();
                    float escapeX = actorPosition.x - static_cast<float>(blocker.x);
                    float escapeZ = actorPosition.z - static_cast<float>(blocker.z);
                    static constexpr std::array<GridPos, 8> escapeOffsets {
                        GridPos { 1, 0, 0 }, GridPos { -1, 0, 0 },
                        GridPos { 0, 0, 1 }, GridPos { 0, 0, -1 },
                        GridPos { 1, 0, 1 }, GridPos { -1, 0, 1 },
                        GridPos { 1, 0, -1 }, GridPos { -1, 0, -1 }
                    };
                    std::optional<Vector3> clearSupportCenter;
                    std::optional<GridPos> clearSupport;
                    float clearSupportDistance = std::numeric_limits<float>::max();
                    for (const GridPos& offset : escapeOffsets)
                    {
                        const GridPos candidate {
                            support->x + offset.x, support->y, support->z + offset.z };
                        if (!world.IsSupported(candidate)
                            || !world.IsBodyClear(candidate, profile))
                        {
                            continue;
                        }
                        const Vector3 center = world.SupportCenter(
                            candidate, profile.bodyCenterAboveSupport);
                        const float dx = center.x - actorPosition.x;
                        const float dz = center.z - actorPosition.z;
                        const float distance = dx * dx + dz * dz;
                        if (distance < clearSupportDistance)
                        {
                            clearSupportDistance = distance;
                            clearSupportCenter = center;
                            clearSupport = candidate;
                        }
                    }
                    if (!clearSupport.has_value())
                    {
                        for (int radius = 2; radius <= 4 && !clearSupport.has_value(); ++radius)
                        {
                            for (int dy = -1; dy <= 4 && !clearSupport.has_value(); ++dy)
                            {
                                for (int dx = -radius; dx <= radius && !clearSupport.has_value(); ++dx)
                                {
                                    for (int dz = -radius; dz <= radius; ++dz)
                                    {
                                        if (std::max(std::abs(dx), std::abs(dz)) != radius) continue;
                                        const GridPos candidate {
                                            support->x + dx, support->y + dy, support->z + dz };
                                        if (!world.IsSupported(candidate)
                                            || !world.IsBodyClear(candidate, profile))
                                        {
                                            continue;
                                        }
                                        clearSupport = candidate;
                                        clearSupportCenter = world.SupportCenter(
                                            candidate, profile.bodyCenterAboveSupport);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (clearSupportCenter.has_value())
                    {
                        escapeX = clearSupportCenter->x - actorPosition.x;
                        escapeZ = clearSupportCenter->z - actorPosition.z;
                    }
                    if (std::fabs(escapeX) + std::fabs(escapeZ) < 0.05f)
                    {
                        escapeX = 1.0f;
                    }
                    update.command.aimYaw = std::atan2(escapeX, -escapeZ);
                    update.command.moveForward = 0.65f;
                    update.command.sneak = true;
                    update.hasCommand = true;
                    update.blockedStartRecovered = true;
                    if (clearSupport.has_value() && blockedStartSeconds_ >= 0.45f)
                    {
                        update.blockedStartRelocated = true;
                        update.blockedStartRelocationSupport = *clearSupport;
                        blockedStartSeconds_ = 0.0f;
                    }
                    repathCooldown_ = std::max(repathCooldown_, 0.08f);
                    ++metrics_.blockedStartRecoveries;
                    ++metricsDelta_.blockedStartRecoveries;
                }
                else
                {
                    repathCooldown_ = std::max(
                        repathCooldown_, settings_.failedRepathCooldownSeconds);
                    ++metrics_.blockedStartDeferrals;
                    ++metricsDelta_.blockedStartDeferrals;
                }
                RefreshDebug(world);
                return update;
            }
        }
    }
    else
    {
        blockedStartSeconds_ = 0.0f;
    }

    const NavigationState state { *support, profile.availableBridgeBlocks, 0 };
    goalSatisfied_ = goal_->IsSatisfied(state, world);
    update.goalSatisfied = goalSatisfied_;
    const float threat = world.ThreatCostAt(*support, profile.bodyCenterAboveSupport);
    if (executor_.IsActive()
        && threat >= settings_.criticalThreatCost
        && threat - lastThreat_ >= settings_.threatRepathDelta)
    {
        pendingRepath_ = RepathReason::ThreatChanged;
        executor_.Cancel(RepathReason::ThreatChanged, "critical threat changed");
    }
    lastThreat_ = threat;

    if (goalSatisfied_)
    {
        if (executor_.IsActive()) executor_.Cancel(RepathReason::PathExhausted, "goal satisfied");
        MergeMetrics(executor_.ConsumeMetricsDelta());
        RefreshDebug(world);
        return update;
    }

    // A global world revision does not automatically discard the route. If a
    // nearby action really became invalid, attempt a bounded detour back into
    // the existing suffix before falling back to a complete goal replan.
    if (settings_.enableSegmentRepair
        && executor_.IsActive()
        && executor_.ValidatedWorldRevision() != world.WorldRevision())
    {
        executor_.AcceptDisjointWorldChanges(
            world, static_cast<std::size_t>(settings_.segmentRepairLookAhead));
    }
    if (settings_.enableSegmentRepair
        && executor_.IsActive()
        && executor_.ValidatedWorldRevision() != world.WorldRevision())
    {
        std::size_t invalidIndex = 0;
        RepathReason invalidReason = RepathReason::None;
        const bool invalid = executor_.FindFirstInvalidMovement(
            actor, world, profile, &invalidIndex, &invalidReason,
            static_cast<std::size_t>(settings_.segmentRepairLookAhead));
        if (invalid && invalidReason == RepathReason::WorldChanged)
        {
            if (!TryRepairSegment(actor, world, profile, invalidIndex))
            {
                pendingRepath_ = RepathReason::WorldChanged;
                executor_.Cancel(RepathReason::WorldChanged, "segment repair failed");
            }
        }
        else if (!invalid)
        {
            executor_.ConfirmWorldRevision(world);
        }
    }

    const bool needsPath = !executor_.HasPath()
        || !executor_.IsActive()
        || pendingRepath_ != RepathReason::None;
    if (needsPath && repathCooldown_ <= 0.0f)
    {
        const RepathReason reason = pendingRepath_ != RepathReason::None
            ? pendingRepath_
            : RepathReason::PathExhausted;
        if (reason != RepathReason::GoalChanged
            && (reason != RepathReason::PathExhausted || executor_.HasPath()))
        {
            metrics_.RecordRepath(reason);
            metricsDelta_.RecordRepath(reason);
        }
        lastRepath_ = reason;
        pendingRepath_ = RepathReason::None;
        update.planned = Plan(actor, world, profile);
        update.repathReason = reason;
    }

    if (executor_.IsActive())
    {
        update.execution = executor_.Update(actor, world, profile, deltaSeconds, tick);
        if (update.execution.proposal.active) arbiter_.Submit(update.execution.proposal);
        if (update.execution.needsRepath)
        {
            if (TryRecoverFailedAction(actor, world, profile)
                || TryConvertFailedGapToBridge(actor, world, profile))
            {
                update.execution.needsRepath = false;
                update.planned = true;
                consecutiveActionFailures_ = 0;
            }
            else
            {
                ++consecutiveActionFailures_;
                pendingRepath_ = update.execution.repathReason;
                lastRepath_ = update.execution.repathReason;
                if (consecutiveActionFailures_ >= settings_.maxActionFailuresBeforeAbandon)
                {
                    if (const PlannedMovement* failed = executor_.CurrentMovement())
                    {
                        update.failedMovementType = failed->type;
                        update.failedMovementFrom = failed->from;
                        update.failedMovementTo = failed->to;
                    }
                    ++metrics_.routeAbandonments;
                    ++metricsDelta_.routeAbandonments;
                    goal_.reset();
                    goalSignature_ = 0;
                    goalSatisfied_ = false;
                    pendingRepath_ = RepathReason::None;
                    executor_.Reset();
                    consecutiveActionFailures_ = 0;
                    update.routeAbandoned = true;
                    update.repathReason = RepathReason::MovementFailed;
                }
            }
        }
        else if (update.execution.movementAdvanced)
        {
            consecutiveActionFailures_ = 0;
            hasRecoveredAction_ = false;
        }
        MergeMetrics(executor_.ConsumeMetricsDelta());
    }

    const BotControlProposal emergency = BuildEmergencyProposal(actor, world, profile, tick);
    arbiter_.Submit(emergency);
    if (emergency.active)
    {
        pendingRepath_ = RepathReason::EmergencySave;
        lastRepath_ = RepathReason::EmergencySave;
    }
    if (!arbiter_.Proposals().empty())
    {
        const BotControlResolution resolution = arbiter_.Resolve(update.command);
        update.command = resolution.command;
        update.hasCommand = true;
    }
    RefreshDebug(world);
    return update;
}

NavigationMetrics NavigationController::ConsumeMetricsDelta() noexcept
{
    NavigationMetrics result = metricsDelta_;
    metricsDelta_ = NavigationMetrics {};
    return result;
}

bool NavigationController::Plan(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile& profile)
{
    const std::optional<GridPos> support = world.FindSupport(
        actor.GetPosition(), profile.maxSafeDropBlocks + 2, 2, profile.bodyCenterAboveSupport);
    ++metrics_.pathRequests;
    ++metricsDelta_.pathRequests;
    if (!support.has_value())
    {
        const std::size_t status = static_cast<std::size_t>(NavigationSearchStatus::InvalidStart);
        ++metrics_.pathResultsByStatus[status];
        ++metricsDelta_.pathResultsByStatus[status];
        ++metrics_.failedPathRequests;
        ++metricsDelta_.failedPathRequests;
        repathCooldown_ = settings_.failedRepathCooldownSeconds;
        return false;
    }

    const NavigationState start { *support, profile.availableBridgeBlocks, 0 };
    NavigationSearchLimits limits = settings_.searchLimits;
    const std::optional<GridPos> representative = goal_->RepresentativePosition();
    if (settings_.enableCorridorSearch && representative.has_value())
    {
        const float dx = static_cast<float>(representative->x - support->x);
        const float dz = static_cast<float>(representative->z - support->z);
        const float distance = std::sqrt(dx * dx + dz * dz);
        if (distance >= settings_.corridorMinimumLength)
        {
            limits.constrainToCorridor = true;
            limits.corridorStart = *support;
            limits.corridorEnd = *representative;
            limits.corridorHalfWidth = settings_.corridorHalfWidth;
            limits.corridorVerticalPadding = settings_.corridorVerticalPadding;
            ++metrics_.corridorConstrainedSearches;
            ++metricsDelta_.corridorConstrainedSearches;
        }
    }
    const auto began = std::chrono::steady_clock::now();
    NavigationSearchResult search = pathfinder_.FindPath(
        start, *goal_, world, profile, limits);
    if (limits.constrainToCorridor && !search.HasPath()
        && search.status != NavigationSearchStatus::AlreadySatisfied)
    {
        const NavigationPath constrainedDiagnostics = search.path;
        limits.constrainToCorridor = false;
        search = pathfinder_.FindPath(start, *goal_, world, profile, limits);
        search.path.expandedNodes += constrainedDiagnostics.expandedNodes;
        search.path.generatedNodes += constrainedDiagnostics.generatedNodes;
        search.path.heapDecreaseKeys += constrainedDiagnostics.heapDecreaseKeys;
        search.path.peakOpenNodes = std::max(
            search.path.peakOpenNodes, constrainedDiagnostics.peakOpenNodes);
        search.path.corridorRejectedNodes += constrainedDiagnostics.corridorRejectedNodes;
        ++metrics_.corridorFallbackSearches;
        ++metricsDelta_.corridorFallbackSearches;
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - began).count();
    metrics_.pathfindingMilliseconds += milliseconds;
    metricsDelta_.pathfindingMilliseconds += milliseconds;
    metrics_.expandedNodes += static_cast<std::uint64_t>(search.path.expandedNodes);
    metricsDelta_.expandedNodes += static_cast<std::uint64_t>(search.path.expandedNodes);
    metrics_.generatedNodes += static_cast<std::uint64_t>(search.path.generatedNodes);
    metricsDelta_.generatedNodes += static_cast<std::uint64_t>(search.path.generatedNodes);
    metrics_.heapDecreaseKeys += static_cast<std::uint64_t>(search.path.heapDecreaseKeys);
    metricsDelta_.heapDecreaseKeys += static_cast<std::uint64_t>(search.path.heapDecreaseKeys);
    metrics_.peakOpenNodes = std::max(
        metrics_.peakOpenNodes, static_cast<std::uint64_t>(search.path.peakOpenNodes));
    metricsDelta_.peakOpenNodes = std::max(
        metricsDelta_.peakOpenNodes, static_cast<std::uint64_t>(search.path.peakOpenNodes));
    metrics_.corridorRejectedNodes += static_cast<std::uint64_t>(search.path.corridorRejectedNodes);
    metricsDelta_.corridorRejectedNodes += static_cast<std::uint64_t>(search.path.corridorRejectedNodes);
    const std::size_t finalStatus = static_cast<std::size_t>(search.status);
    ++metrics_.pathResultsByStatus[finalStatus];
    ++metricsDelta_.pathResultsByStatus[finalStatus];

    if (!search.HasPath())
    {
        if (search.status == NavigationSearchStatus::AlreadySatisfied)
        {
            goalSatisfied_ = true;
            ++metrics_.successfulPaths;
            ++metricsDelta_.successfulPaths;
            executor_.Reset();
            return true;
        }
        ++metrics_.failedPathRequests;
        ++metricsDelta_.failedPathRequests;
        executor_.Reset();
        repathCooldown_ = settings_.failedRepathCooldownSeconds;
        return false;
    }

    if (search.status == NavigationSearchStatus::Partial)
    {
        ++metrics_.partialPaths;
        ++metricsDelta_.partialPaths;
    }
    else
    {
        ++metrics_.successfulPaths;
        ++metricsDelta_.successfulPaths;
    }
    if (static_cast<int>(search.path.movements.size()) > settings_.maxSegmentActions)
    {
        search.path.movements.resize(static_cast<std::size_t>(settings_.maxSegmentActions));
        search.path.resolvedGoal = search.path.movements.back().to;
        search.path.partial = true;
    }
    for (const PlannedMovement& movement : search.path.movements)
    {
        if (movement.type == MovementType::Sprint && movement.traversedCells > 1)
        {
            ++metrics_.longSprintActions;
            ++metricsDelta_.longSprintActions;
        }
        if (NavigationActionLibrary::IsDiagonal(movement.from, movement.to))
        {
            ++metrics_.diagonalActions;
            ++metricsDelta_.diagonalActions;
        }
    }
    executor_.SetPath(std::move(search.path));
    repathCooldown_ = settings_.repathCooldownSeconds;
    return true;
}

bool NavigationController::TryRepairSegment(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile& profile,
    std::size_t invalidIndex)
{
    ++metrics_.segmentRepairAttempts;
    ++metricsDelta_.segmentRepairAttempts;
    const NavigationPath oldPath = executor_.Path();
    if (oldPath.movements.empty() || invalidIndex >= oldPath.movements.size())
    {
        ++metrics_.segmentRepairFailures;
        ++metricsDelta_.segmentRepairFailures;
        return false;
    }
    const std::optional<GridPos> support = world.FindSupport(
        actor.GetPosition(), profile.maxSafeDropBlocks + 2, 2, profile.bodyCenterAboveSupport);
    if (!support.has_value())
    {
        ++metrics_.segmentRepairFailures;
        ++metricsDelta_.segmentRepairFailures;
        return false;
    }

    const std::size_t lastCandidate = std::min(
        oldPath.movements.size() - 1U,
        invalidIndex + static_cast<std::size_t>(settings_.segmentRepairReconnectActions));
    const auto began = std::chrono::steady_clock::now();
    std::uint64_t expanded = 0;
    for (std::size_t candidateOffset = 0;
         candidateOffset <= lastCandidate - invalidIndex;
         ++candidateOffset)
    {
        const std::size_t reconnectIndex = lastCandidate - candidateOffset;
        const GridPos reconnect = oldPath.movements[reconnectIndex].to;
        if (!world.IsSupported(reconnect) || !world.IsBodyClear(reconnect, profile)) continue;

        NavigationSearchLimits limits = settings_.searchLimits;
        limits.maxExpansions = settings_.segmentRepairMaxExpansions;
        limits.maxActions = settings_.segmentRepairMaxActions;
        limits.maxSearchRadius = settings_.segmentRepairRadius;
        limits.maxOpenNodes = std::min(limits.maxOpenNodes, 3072);
        limits.allowPartial = false;
        limits.constrainToCorridor = true;
        limits.corridorStart = *support;
        limits.corridorEnd = reconnect;
        limits.corridorHalfWidth = settings_.segmentRepairCorridorHalfWidth;
        limits.corridorVerticalPadding = settings_.corridorVerticalPadding;
        GoalReachPosition reconnectGoal(reconnect);
        NavigationSearchResult repair = pathfinder_.FindPath(
            NavigationState { *support, profile.availableBridgeBlocks, 0 },
            reconnectGoal, world, profile, limits);
        expanded += static_cast<std::uint64_t>(repair.path.expandedNodes);
        if (!repair.Succeeded() || repair.path.movements.empty()) continue;

        NavigationPath combined = std::move(repair.path);
        const std::size_t suffixBegin = reconnectIndex + 1U;
        for (std::size_t index = suffixBegin; index < oldPath.movements.size(); ++index)
        {
            PlannedMovement movement = oldPath.movements[index];
            movement.plannedWorldRevision = world.WorldRevision();
            combined.movements.push_back(std::move(movement));
        }
        combined.worldRevision = world.WorldRevision();
        combined.start = *support;
        combined.resolvedGoal = oldPath.resolvedGoal;
        combined.partial = oldPath.partial;
        combined.totalCost = 0.0f;
        combined.movementCost = 0.0f;
        combined.timeCost = 0.0f;
        combined.resourceCost = 0.0f;
        combined.threatCost = 0.0f;
        for (const PlannedMovement& movement : combined.movements)
        {
            combined.totalCost += movement.totalCost;
            combined.movementCost += movement.movementCost;
            combined.timeCost += movement.timeCost;
            combined.resourceCost += movement.resourceCost;
            combined.threatCost += movement.threatCost;
        }
        const std::uint64_t reused = static_cast<std::uint64_t>(
            oldPath.movements.size() - suffixBegin);
        executor_.SetPath(std::move(combined));
        ++metrics_.segmentRepairSuccesses;
        ++metricsDelta_.segmentRepairSuccesses;
        metrics_.segmentRepairReusedActions += reused;
        metricsDelta_.segmentRepairReusedActions += reused;
        metrics_.segmentRepairExpandedNodes += expanded;
        metricsDelta_.segmentRepairExpandedNodes += expanded;
        const double milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - began).count();
        metrics_.segmentRepairMilliseconds += milliseconds;
        metricsDelta_.segmentRepairMilliseconds += milliseconds;
        return true;
    }

    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - began).count();
    metrics_.segmentRepairMilliseconds += milliseconds;
    metricsDelta_.segmentRepairMilliseconds += milliseconds;
    metrics_.segmentRepairExpandedNodes += expanded;
    metricsDelta_.segmentRepairExpandedNodes += expanded;
    ++metrics_.segmentRepairFailures;
    ++metricsDelta_.segmentRepairFailures;
    return false;
}

bool NavigationController::TryConvertFailedGapToBridge(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile& profile)
{
    const PlannedMovement* failed = executor_.CurrentMovement();
    if (failed == nullptr || failed->type != MovementType::GapJump) return false;

    ++metrics_.gapToBridgeAttempts;
    ++metricsDelta_.gapToBridgeAttempts;
    const NavigationPath oldPath = executor_.Path();
    const std::size_t failedIndex = executor_.MovementIndex();
    const std::optional<GridPos> support = world.FindSupport(
        actor.GetPosition(), 1, 1, profile.bodyCenterAboveSupport);
    const int dx = failed->to.x - failed->from.x;
    const int dz = failed->to.z - failed->from.z;
    const int span = std::max(std::abs(dx), std::abs(dz));
    const bool cardinal = (dx == 0) != (dz == 0);
    const bool sameLevel = failed->from.y == failed->to.y;
    const bool atTakeoff = support.has_value()
        && support->x == failed->from.x
        && support->y == failed->from.y
        && support->z == failed->from.z;
    if (!profile.canBridge
        || !profile.canPlaceBlocks
        || !atTakeoff
        || !sameLevel
        || !cardinal
        || span < 2
        || span - 1 > profile.maxConsecutiveBridgeBlocks
        || profile.availableBridgeBlocks - profile.reserveBridgeBlocks < span - 1
        || FindBridgeSlot(actor, profile.preferredBridgeBlock) < 0)
    {
        ++metrics_.gapToBridgeFailures;
        ++metricsDelta_.gapToBridgeFailures;
        return false;
    }

    const int stepX = dx == 0 ? 0 : (dx > 0 ? 1 : -1);
    const int stepZ = dz == 0 ? 0 : (dz > 0 ? 1 : -1);
    NavigationPath converted;
    converted.start = *support;
    converted.resolvedGoal = oldPath.resolvedGoal;
    converted.partial = oldPath.partial;
    converted.worldRevision = world.WorldRevision();
    GridPos previous = *support;
    int remaining = profile.availableBridgeBlocks;
    for (int offset = 1; offset < span; ++offset)
    {
        const GridPos bridgeSupport {
            failed->from.x + stepX * offset,
            failed->from.y,
            failed->from.z + stepZ * offset
        };
        if (!world.IsAir(bridgeSupport) || !world.IsBodyClear(bridgeSupport, profile))
        {
            ++metrics_.gapToBridgeFailures;
            ++metricsDelta_.gapToBridgeFailures;
            return false;
        }
        PlannedMovement bridge;
        bridge.type = MovementType::SneakBridge;
        bridge.from = previous;
        bridge.to = bridgeSupport;
        bridge.affectedBlock = bridgeSupport;
        bridge.placementBlockType = profile.preferredBridgeBlock;
        bridge.requiresSneak = true;
        bridge.movementCost = profile.bridgeMovementCost;
        bridge.timeCost = profile.blockPlaceSeconds
            + 1.0f / std::max(0.1f, profile.sneakSpeed);
        bridge.resourceCost = profile.BridgeScarcityCost(remaining--);
        bridge.threatCost = world.ThreatCostAt(
            bridgeSupport, profile.bodyCenterAboveSupport) * profile.ThreatCostScale();
        bridge.totalCost = bridge.movementCost + bridge.timeCost
            + bridge.resourceCost + bridge.threatCost;
        bridge.plannedWorldRevision = world.WorldRevision();
        converted.movements.push_back(bridge);
        previous = bridgeSupport;
    }
    if (!world.IsSupported(failed->to) || !world.IsBodyClear(failed->to, profile))
    {
        ++metrics_.gapToBridgeFailures;
        ++metricsDelta_.gapToBridgeFailures;
        return false;
    }
    PlannedMovement finish;
    finish.type = MovementType::Walk;
    finish.from = previous;
    finish.to = failed->to;
    finish.movementCost = profile.walkCost;
    finish.timeCost = 1.0f / std::max(0.1f, profile.moveSpeed);
    finish.threatCost = world.ThreatCostAt(
        finish.to, profile.bodyCenterAboveSupport) * profile.ThreatCostScale();
    finish.totalCost = finish.movementCost + finish.timeCost + finish.threatCost;
    finish.plannedWorldRevision = world.WorldRevision();
    converted.movements.push_back(finish);

    for (std::size_t index = failedIndex + 1; index < oldPath.movements.size(); ++index)
    {
        PlannedMovement suffix = oldPath.movements[index];
        suffix.plannedWorldRevision = world.WorldRevision();
        converted.movements.push_back(std::move(suffix));
    }
    for (const PlannedMovement& movement : converted.movements)
    {
        converted.totalCost += movement.totalCost;
        converted.movementCost += movement.movementCost;
        converted.timeCost += movement.timeCost;
        converted.resourceCost += movement.resourceCost;
        converted.threatCost += movement.threatCost;
    }
    executor_.SetPath(std::move(converted));
    ++metrics_.gapToBridgeSuccesses;
    ++metricsDelta_.gapToBridgeSuccesses;
    return true;
}

bool NavigationController::TryRecoverFailedAction(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile& profile)
{
    const PlannedMovement* failed = executor_.CurrentMovement();
    if (failed == nullptr
        || (failed->type != MovementType::SneakBridge
            && failed->type != MovementType::BreakBlock))
    {
        return false;
    }
    ++metrics_.actionRecoveryAttempts;
    ++metricsDelta_.actionRecoveryAttempts;
    if (hasRecoveredAction_
        && recoveredActionType_ == failed->type
        && recoveredActionFrom_.x == failed->from.x
        && recoveredActionFrom_.y == failed->from.y
        && recoveredActionFrom_.z == failed->from.z
        && recoveredActionTo_.x == failed->to.x
        && recoveredActionTo_.y == failed->to.y
        && recoveredActionTo_.z == failed->to.z)
    {
        ++metrics_.actionRecoveryFailures;
        ++metricsDelta_.actionRecoveryFailures;
        return false;
    }
    const std::optional<GridPos> support = world.FindSupport(
        actor.GetPosition(), 1, 1, profile.bodyCenterAboveSupport);
    if (!support.has_value())
    {
        ++metrics_.actionRecoveryFailures;
        ++metricsDelta_.actionRecoveryFailures;
        return false;
    }

    const NavigationPath oldPath = executor_.Path();
    const std::size_t failedIndex = executor_.MovementIndex();
    const GridPos affected = failed->affectedBlock.value_or(failed->to);
    NavigationPath recovered;
    recovered.start = *support;
    recovered.resolvedGoal = oldPath.resolvedGoal;
    recovered.partial = oldPath.partial;
    recovered.worldRevision = world.WorldRevision();
    const bool alreadyAtDestination = support->x == failed->to.x
        && support->y == failed->to.y && support->z == failed->to.z;
    const bool atActionStart = support->x == failed->from.x
        && support->y == failed->from.y && support->z == failed->from.z;
    if (!alreadyAtDestination && !atActionStart)
    {
        ++metrics_.actionRecoveryFailures;
        ++metricsDelta_.actionRecoveryFailures;
        return false;
    }

    if (!alreadyAtDestination)
    {
        if (failed->type == MovementType::SneakBridge && world.IsSolid(affected))
        {
            PlannedMovement step;
            step.type = MovementType::Walk;
            step.from = *support;
            step.to = failed->to;
            step.movementCost = profile.walkCost;
            step.timeCost = 1.0f / std::max(0.1f, profile.moveSpeed);
            step.threatCost = world.ThreatCostAt(
                step.to, profile.bodyCenterAboveSupport) * profile.ThreatCostScale();
            step.totalCost = step.movementCost + step.timeCost + step.threatCost;
            step.plannedWorldRevision = world.WorldRevision();
            recovered.movements.push_back(step);
        }
        else
        {
            PlannedMovement retry = *failed;
            retry.plannedWorldRevision = world.WorldRevision();
            recovered.movements.push_back(retry);
        }
    }
    for (std::size_t index = failedIndex + 1; index < oldPath.movements.size(); ++index)
    {
        PlannedMovement suffix = oldPath.movements[index];
        suffix.plannedWorldRevision = world.WorldRevision();
        recovered.movements.push_back(std::move(suffix));
    }
    for (const PlannedMovement& movement : recovered.movements)
    {
        recovered.totalCost += movement.totalCost;
        recovered.movementCost += movement.movementCost;
        recovered.timeCost += movement.timeCost;
        recovered.resourceCost += movement.resourceCost;
        recovered.threatCost += movement.threatCost;
    }
    recoveredActionType_ = failed->type;
    recoveredActionFrom_ = failed->from;
    recoveredActionTo_ = failed->to;
    hasRecoveredAction_ = true;
    executor_.SetPath(std::move(recovered));
    ++metrics_.actionRecoverySuccesses;
    ++metricsDelta_.actionRecoverySuccesses;
    return true;
}

BotControlProposal NavigationController::BuildEmergencyProposal(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile& profile,
    std::uint32_t tick)
{
    BotControlProposal proposal;
    if (actor.IsOnGround() || actor.GetVelocity().y > -0.15f
        || !profile.canPlaceBlocks
        || emergencyTimer_ < settings_.emergencyReactionSeconds)
    {
        return proposal;
    }
    const GridPos under = world.WorldToGrid(Vector3 {
        actor.GetPosition().x,
        actor.GetPosition().y - profile.bodyCenterAboveSupport,
        actor.GetPosition().z
    });
    if (world.IsSolid(under)) return proposal;
    const int slot = FindBridgeSlot(actor, profile.preferredBridgeBlock);
    if (slot < 0) return proposal;

    static constexpr std::array<GridPos, 6> anchorOffsets {
        GridPos { 1, 0, 0 }, GridPos { -1, 0, 0 },
        GridPos { 0, 0, 1 }, GridPos { 0, 0, -1 },
        GridPos { 0, -1, 0 }, GridPos { 0, 1, 0 }
    };
    std::optional<GridPos> anchor;
    for (const GridPos& offset : anchorOffsets)
    {
        const GridPos candidate {
            under.x + offset.x, under.y + offset.y, under.z + offset.z };
        if (world.IsSolid(candidate))
        {
            anchor = candidate;
            break;
        }
    }
    // Authoritative placement still requires a real adjacent block. A bot
    // that has already fallen beyond every face cannot create floating wool.
    if (!anchor.has_value()) return proposal;

    proposal.active = true;
    proposal.source = BotControlSource::Emergency;
    proposal.priority = BotControlPriority::Emergency;
    proposal.domains = BotControlDomain::All;
    proposal.exclusiveDomains = BotControlDomain::All;
    proposal.reason = "emergency void save";
    proposal.command.controlledPlayerId = static_cast<std::uint32_t>(actor.GetId());
    proposal.command.tick = tick;
    const Vector3 eye {
        actor.GetPosition().x, actor.GetPosition().y + 0.78f, actor.GetPosition().z };
    const Vector3 aim = world.GridToWorld(*anchor);
    const float dx = aim.x - eye.x;
    const float dy = aim.y - eye.y;
    const float dz = aim.z - eye.z;
    proposal.command.aimYaw = std::atan2(dx, -dz);
    proposal.command.aimPitch = std::atan2(
        dy, std::max(0.0001f, std::sqrt(dx * dx + dz * dz)));
    // The anchor is also the nearest legal direction back toward terrain.
    // A small air-control input makes the placement a recovery instead of a
    // block left behind by a player who keeps drifting into the void.
    proposal.command.moveForward = 0.42f;
    proposal.command.selectedSlot = slot;
    proposal.command.sneak = true;
    proposal.command.placeHeld = true;
    proposal.command.placePressed = !emergencyIssued_;
    if (!emergencyIssued_)
    {
        emergencyIssued_ = true;
        ++metrics_.emergencySaveAttempts;
        ++metricsDelta_.emergencySaveAttempts;
    }
    return proposal;
}

int NavigationController::FindBridgeSlot(const Player& actor, BlockType type) const
{
    const auto& hotbar = actor.GetInventory().GetHotbarSlots();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        const std::optional<BlockType> block = ItemToBlock(hotbar[slot].type);
        if (!hotbar[slot].IsEmpty() && block.has_value() && *block == type) return slot;
    }
    return -1;
}

void NavigationController::MergeMetrics(const NavigationMetrics& delta)
{
    metrics_.Add(delta);
    metricsDelta_.Add(delta);
}

void NavigationController::RefreshDebug(const NavigationWorldView& world)
{
    debug_.goal = goal_ != nullptr ? goal_->Describe() : std::string {};
    debug_.path = executor_.Path();
    debug_.movementIndex = executor_.MovementIndex();
    debug_.executionStatus = executor_.Status();
    debug_.phase = executor_.Phase();
    debug_.lastRepath = lastRepath_;
    debug_.noProgressSeconds = executor_.NoProgressSeconds();
    debug_.currentThreat = lastThreat_;
    debug_.goalSatisfied = goalSatisfied_;
    debug_.pathStale = executor_.HasPath()
        && executor_.ValidatedWorldRevision() != world.WorldRevision();
}

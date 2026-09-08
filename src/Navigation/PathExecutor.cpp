#include "Navigation/PathExecutor.h"

#include "Inventory.h"
#include "Navigation/NavigationActionLibrary.h"
#include "Navigation/NavigationWorldView.h"
#include "Player.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
constexpr float kPi = 3.14159265358979323846f;

float Distance(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float HorizontalDistance(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float WrapAngle(float angle)
{
    while (angle > kPi) angle -= 2.0f * kPi;
    while (angle < -kPi) angle += 2.0f * kPi;
    return angle;
}

float GridDistance(GridPos a, GridPos b)
{
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y);
    const float dz = static_cast<float>(a.z - b.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

int OpenSupportSides(const NavigationWorldView& world, GridPos support)
{
    static constexpr std::array<GridPos, 4> offsets {
        GridPos { 1, 0, 0 }, GridPos { -1, 0, 0 },
        GridPos { 0, 0, 1 }, GridPos { 0, 0, -1 }
    };
    int open = 0;
    for (const GridPos& offset : offsets)
    {
        if (world.IsAir(GridPos {
                support.x + offset.x,
                support.y + offset.y,
                support.z + offset.z }))
        {
            ++open;
        }
    }
    return open;
}

bool SameHorizontalDirection(const PlannedMovement& a, const PlannedMovement& b)
{
    const int ax = std::clamp(a.to.x - a.from.x, -1, 1);
    const int az = std::clamp(a.to.z - a.from.z, -1, 1);
    const int bx = std::clamp(b.to.x - b.from.x, -1, 1);
    const int bz = std::clamp(b.to.z - b.from.z, -1, 1);
    return ax == bx && az == bz;
}
}

void NavigationMetrics::Add(const NavigationMetrics& other) noexcept
{
    pathRequests += other.pathRequests;
    successfulPaths += other.successfulPaths;
    partialPaths += other.partialPaths;
    failedPathRequests += other.failedPathRequests;
    for (std::size_t i = 0; i < pathResultsByStatus.size(); ++i)
    {
        pathResultsByStatus[i] += other.pathResultsByStatus[i];
    }
    blockedStartDeferrals += other.blockedStartDeferrals;
    blockedStartRecoveries += other.blockedStartRecoveries;
    expandedNodes += other.expandedNodes;
    generatedNodes += other.generatedNodes;
    heapDecreaseKeys += other.heapDecreaseKeys;
    peakOpenNodes = std::max(peakOpenNodes, other.peakOpenNodes);
    pathfindingMilliseconds += other.pathfindingMilliseconds;
    repaths += other.repaths;
    for (std::size_t i = 0; i < repathReasons.size(); ++i) repathReasons[i] += other.repathReasons[i];
    movementFailures += other.movementFailures;
    for (std::size_t i = 0; i < movementFailuresByMovement.size(); ++i)
    {
        movementFailuresByMovement[i] += other.movementFailuresByMovement[i];
    }
    stuckEvents += other.stuckEvents;
    for (std::size_t i = 0; i < stuckEventsByMovement.size(); ++i)
    {
        stuckEventsByMovement[i] += other.stuckEventsByMovement[i];
    }
    recenterRecoveryAttempts += other.recenterRecoveryAttempts;
    recenterRecoveryRetries += other.recenterRecoveryRetries;
    actionRecoveryAttempts += other.actionRecoveryAttempts;
    actionRecoverySuccesses += other.actionRecoverySuccesses;
    actionRecoveryFailures += other.actionRecoveryFailures;
    gapToBridgeAttempts += other.gapToBridgeAttempts;
    gapToBridgeSuccesses += other.gapToBridgeSuccesses;
    gapToBridgeFailures += other.gapToBridgeFailures;
    routeAbandonments += other.routeAbandonments;
    bridgeBlocksUsed += other.bridgeBlocksUsed;
    blocksBrokenForNavigation += other.blocksBrokenForNavigation;
    failedJumps += other.failedJumps;
    successfulGapJumps += other.successfulGapJumps;
    recoveredJumpLandings += other.recoveredJumpLandings;
    emergencySaveAttempts += other.emergencySaveAttempts;
    emergencySaves += other.emergencySaves;
    voidDeaths += other.voidDeaths;
    for (std::size_t i = 0; i < voidDeathsByMovement.size(); ++i)
    {
        voidDeathsByMovement[i] += other.voidDeathsByMovement[i];
    }
    unforcedVoidDeaths += other.unforcedVoidDeaths;
    for (std::size_t i = 0; i < unforcedVoidDeathsByMovement.size(); ++i)
    {
        unforcedVoidDeathsByMovement[i] += other.unforcedVoidDeathsByMovement[i];
    }
    combatAttributedVoidDeaths += other.combatAttributedVoidDeaths;
    voidDeathsOutsideNavigation += other.voidDeathsOutsideNavigation;
    momentumPreservedTransitions += other.momentumPreservedTransitions;
    edgeBrakeActions += other.edgeBrakeActions;
    edgeBrakeCompletions += other.edgeBrakeCompletions;
    cancelledActions += other.cancelledActions;
    corridorPlans += other.corridorPlans;
    corridorReuses += other.corridorReuses;
    corridorAdvances += other.corridorAdvances;
    corridorFailures += other.corridorFailures;
    routeBridgeSegmentsStarted += other.routeBridgeSegmentsStarted;
    routeBridgeSegmentsCompleted += other.routeBridgeSegmentsCompleted;
    routeBridgeFollowersHeld += other.routeBridgeFollowersHeld;
    longSprintActions += other.longSprintActions;
    diagonalActions += other.diagonalActions;
    corridorConstrainedSearches += other.corridorConstrainedSearches;
    corridorFallbackSearches += other.corridorFallbackSearches;
    corridorRejectedNodes += other.corridorRejectedNodes;
    segmentRepairAttempts += other.segmentRepairAttempts;
    segmentRepairSuccesses += other.segmentRepairSuccesses;
    segmentRepairFailures += other.segmentRepairFailures;
    segmentRepairReusedActions += other.segmentRepairReusedActions;
    segmentRepairMilliseconds += other.segmentRepairMilliseconds;
    segmentRepairExpandedNodes += other.segmentRepairExpandedNodes;
    dirtyRegionFastAccepts += other.dirtyRegionFastAccepts;
    dirtyRegionIntersectValidations += other.dirtyRegionIntersectValidations;
    dirtyRegionHistoryMisses += other.dirtyRegionHistoryMisses;
    routeEfficiencyTotal += other.routeEfficiencyTotal;
    routeEfficiencySamples += other.routeEfficiencySamples;
    noProgressSeconds += other.noProgressSeconds;
}

void NavigationMetrics::RecordRepath(RepathReason reason) noexcept
{
    if (reason == RepathReason::None || reason == RepathReason::Count) return;
    ++repaths;
    ++repathReasons[static_cast<std::size_t>(reason)];
}

double NavigationMetrics::AverageExpandedNodes() const noexcept
{
    return pathRequests > 0 ? static_cast<double>(expandedNodes) / static_cast<double>(pathRequests) : 0.0;
}

double NavigationMetrics::AverageGeneratedNodes() const noexcept
{
    return pathRequests > 0 ? static_cast<double>(generatedNodes) / static_cast<double>(pathRequests) : 0.0;
}

double NavigationMetrics::AveragePathfindingMilliseconds() const noexcept
{
    return pathRequests > 0 ? pathfindingMilliseconds / static_cast<double>(pathRequests) : 0.0;
}

double NavigationMetrics::AverageRouteEfficiency() const noexcept
{
    return routeEfficiencySamples > 0 ? routeEfficiencyTotal / static_cast<double>(routeEfficiencySamples) : 0.0;
}

PathExecutor::PathExecutor(PathExecutorSettings settings) : settings_(settings) {}

void PathExecutor::SetPath(NavigationPath path)
{
    Reset();
    path_ = std::move(path);
    validatedWorldRevision_ = path_.worldRevision;
    status_ = path_.movements.empty()
        ? MovementExecutionStatus::Succeeded
        : MovementExecutionStatus::NotStarted;
    if (!path_.movements.empty())
    {
        const GridPos start = path_.start;
        const GridPos end = path_.resolvedGoal;
        routeStraightDistance_ = GridDistance(start, end);
    }
}

void PathExecutor::Reset()
{
    path_ = NavigationPath {};
    movementIndex_ = 0;
    status_ = MovementExecutionStatus::NotStarted;
    phase_ = PathExecutorPhase::Idle;
    lastRepathReason_ = RepathReason::None;
    lastMessage_.clear();
    actionElapsedSeconds_ = 0.0f;
    phaseElapsedSeconds_ = 0.0f;
    noProgressSeconds_ = 0.0f;
    bestTargetDistance_ = 0.0f;
    hasBestTargetDistance_ = false;
    stuckEventRecorded_ = false;
    recenterRecoveryActive_ = false;
    recenterRecoveryAttempted_ = false;
    recenterRecoveryElapsedSeconds_ = 0.0f;
    recenterRecoveryTarget_ = Vector3 {};
    edgeBrakeIssuedThisMovement_ = false;
    momentumTransitionRecorded_ = false;
    edgeBrakeEligibleThisMovement_ = false;
    preserveMomentumThisMovement_ = false;
    actionIssued_ = false;
    selectedActionSlot_ = -1;
    selectedBlockCountBefore_ = 0;
    expectedAffectedBlock_.reset();
    validatedWorldRevision_ = 0;
    hasLastActorPosition_ = false;
    routeDistanceTravelled_ = 0.0f;
    routeStraightDistance_ = 0.0f;
}

void PathExecutor::Cancel(RepathReason reason, std::string message)
{
    if (IsActive()) ++metricsDelta_.cancelledActions;
    lastRepathReason_ = reason;
    lastMessage_ = std::move(message);
    status_ = MovementExecutionStatus::Invalidated;
    phase_ = PathExecutorPhase::Failed;
}

PathExecutionUpdate PathExecutor::Update(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile& profile,
    float deltaSeconds,
    std::uint32_t tick)
{
    PathExecutionUpdate update;
    deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.25f);
    if (!HasPath() || movementIndex_ >= path_.movements.size())
    {
        status_ = MovementExecutionStatus::Succeeded;
        phase_ = PathExecutorPhase::Complete;
        update.status = status_;
        update.phase = phase_;
        update.pathFinished = true;
        return update;
    }

    if (world.WorldRevision() != validatedWorldRevision_)
    {
        if (!AcceptDisjointWorldChanges(world))
        {
            RepathReason reason = RepathReason::None;
            if (!ValidateRemainingPath(actor, world, profile, &reason))
            {
                FailCurrent(MovementExecutionStatus::Invalidated, reason, "world changed under path", update);
                return update;
            }
            validatedWorldRevision_ = world.WorldRevision();
        }
    }

    if (status_ == MovementExecutionStatus::NotStarted || phase_ == PathExecutorPhase::Idle)
    {
        BeginCurrentMovement(actor, world, profile);
    }

    actionElapsedSeconds_ += deltaSeconds;
    phaseElapsedSeconds_ += deltaSeconds;
    const PlannedMovement& movement = path_.movements[movementIndex_];
    const Vector3 actorPosition = actor.GetPosition();
    const Vector3 target = MovementTarget(movement, world);
    Vector3 progressTarget = target;
    bool tracksLocomotionProgress = true;
    if (movement.type == MovementType::SneakBridge
        || movement.type == MovementType::PlaceBlock)
    {
        if (phase_ == PathExecutorPhase::BridgeApproach)
        {
            progressTarget = world.SupportCenter(
                movement.from, profile.bodyCenterAboveSupport);
        }
        else if (phase_ != PathExecutorPhase::BridgeStep)
        {
            tracksLocomotionProgress = false;
        }
    }
    else if (movement.type == MovementType::BreakBlock)
    {
        if (phase_ == PathExecutorPhase::BreakApproach)
        {
            progressTarget = world.SupportCenter(
                movement.from, profile.bodyCenterAboveSupport);
        }
        else if (phase_ != PathExecutorPhase::Move)
        {
            tracksLocomotionProgress = false;
        }
    }
    else if (movement.type == MovementType::Wait)
    {
        tracksLocomotionProgress = false;
    }
    else if (movement.type == MovementType::GapJump
        && phase_ == PathExecutorPhase::JumpRunup)
    {
        const int dx = movement.to.x == movement.from.x ? 0
            : (movement.to.x > movement.from.x ? 1 : -1);
        const int dz = movement.to.z == movement.from.z ? 0
            : (movement.to.z > movement.from.z ? 1 : -1);
        progressTarget = world.SupportCenter(GridPos {
            movement.from.x - dx, movement.from.y, movement.from.z - dz },
            profile.bodyCenterAboveSupport);
        progressTarget.x = world.SupportCenter(
            movement.from, profile.bodyCenterAboveSupport).x
            - static_cast<float>(dx) * profile.gapRunupDistanceBlocks;
        progressTarget.z = world.SupportCenter(
            movement.from, profile.bodyCenterAboveSupport).z
            - static_cast<float>(dz) * profile.gapRunupDistanceBlocks;
    }
    // noProgressSeconds is a duration: accumulate the tick delta only while the
    // actor is failing to close distance (the previous noProgressSeconds_ * dt
    // integrated seconds-squared, so the reported total was meaningless). This
    // is telemetry-only — it never feeds a control decision.
    if (tracksLocomotionProgress)
    {
        const bool madeProgress = TrackProgress(actor, progressTarget, deltaSeconds);
        if (!madeProgress) metricsDelta_.noProgressSeconds += deltaSeconds;
    }
    else
    {
        // Alignment, inventory selection, placement confirmation and mining
        // are action phases, not failed locomotion. Their bounded action
        // timeout remains active, but they must not emit false stuck events.
        noProgressSeconds_ = 0.0f;
        hasBestTargetDistance_ = false;
    }

    // A human player usually releases the key and recentres on the support
    // block before repeating a jump/drop or pushing into a tight corner. Do
    // that once, before the action is classified as stuck. The retry remains
    // bounded and still goes through PlayerCommand.
    if (!recenterRecoveryActive_
        && !recenterRecoveryAttempted_
        && actor.IsOnGround()
        && noProgressSeconds_ >= settings_.recenterRecoveryTriggerSeconds
        && (movement.type != MovementType::SneakBridge
            || phase_ == PathExecutorPhase::BridgeStep)
        && movement.type != MovementType::PlaceBlock
        && movement.type != MovementType::BreakBlock
        && movement.type != MovementType::Wait)
    {
        const std::optional<GridPos> support = world.FindSupport(
            actorPosition, 1, 1, profile.bodyCenterAboveSupport);
        if (support.has_value())
        {
            const Vector3 center = world.SupportCenter(*support, profile.bodyCenterAboveSupport);
            if (HorizontalDistance(actorPosition, center) > 0.14f)
            {
                recenterRecoveryActive_ = true;
                recenterRecoveryAttempted_ = true;
                recenterRecoveryElapsedSeconds_ = 0.0f;
                recenterRecoveryTarget_ = center;
                ++metricsDelta_.recenterRecoveryAttempts;
            }
        }
    }

    if (recenterRecoveryActive_)
    {
        recenterRecoveryElapsedSeconds_ += deltaSeconds;
        PlayerCommand recoveryCommand;
        recoveryCommand.controlledPlayerId = static_cast<std::uint32_t>(actor.GetId());
        recoveryCommand.tick = tick;
        recoveryCommand.aimYaw = actor.GetYaw();
        recoveryCommand.selectedSlot = actor.GetSelectedSlot();
        MoveToward(recoveryCommand, actorPosition, recenterRecoveryTarget_, false, true);
        update.proposal = BaseProposal(
            actor, tick, BotControlSource::Navigation, BotControlPriority::Navigation,
            BotControlDomain::Movement | BotControlDomain::Look);
        update.proposal.command = recoveryCommand;
        const bool centered = HorizontalDistance(actorPosition, recenterRecoveryTarget_) <= 0.14f;
        if (centered
            || recenterRecoveryElapsedSeconds_ >= settings_.recenterRecoveryDurationSeconds)
        {
            recenterRecoveryActive_ = false;
            noProgressSeconds_ = 0.0f;
            hasBestTargetDistance_ = false;
            actionElapsedSeconds_ = 0.0f;
            phaseElapsedSeconds_ = 0.0f;
            if (movement.type == MovementType::JumpUp || movement.type == MovementType::GapJump)
            {
                phase_ = PathExecutorPhase::JumpTakeoff;
            }
            ++metricsDelta_.recenterRecoveryRetries;
        }
        update.status = status_;
        update.phase = phase_;
        return update;
    }

    if (noProgressSeconds_ >= settings_.stuckSeconds && !stuckEventRecorded_)
    {
        stuckEventRecorded_ = true;
        ++metricsDelta_.stuckEvents;
        ++metricsDelta_.stuckEventsByMovement[static_cast<std::size_t>(movement.type)];
    }
    if (noProgressSeconds_ >= settings_.stuckSeconds * 1.8f
        || actionElapsedSeconds_ > CurrentTimeoutSeconds())
    {
        if (movement.type == MovementType::JumpUp || movement.type == MovementType::GapJump)
        {
            ++metricsDelta_.failedJumps;
        }
        FailCurrent(
            MovementExecutionStatus::Failed,
            noProgressSeconds_ >= settings_.stuckSeconds * 1.8f
                ? RepathReason::MovementFailed
                : RepathReason::ActionTimeout,
            "movement made no progress",
            update);
        return update;
    }

    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(actor.GetId());
    command.tick = tick;
    command.aimYaw = actor.GetYaw();
    command.selectedSlot = actor.GetSelectedSlot();
    const Vector3 eye { actorPosition.x, actorPosition.y + settings_.eyeHeight, actorPosition.z };

    switch (movement.type)
    {
    case MovementType::Walk:
    case MovementType::Sprint:
    case MovementType::StepUp:
    case MovementType::DropDown:
    {
        const float remaining = HorizontalDistance(actorPosition, target);
        const float brakeDistance = movement.type == MovementType::Sprint ? 0.58f : 0.42f;
        const bool edgeBrake = edgeBrakeEligibleThisMovement_
            && remaining <= brakeDistance;
        // Walk is the precision mode used for turns and bridge approaches.
        // Protect actions touching an exposed support cell, while leaving
        // enclosed-floor movement at normal speed. Include the actor's actual
        // support: after a replan or collision it may no longer match the
        // movement's authored `from` cell.
        const std::optional<GridPos> currentSupport = world.FindSupport(
            actorPosition, 2, 1, profile.bodyCenterAboveSupport);
        const bool precisionWalk = movement.type == MovementType::Walk
            && (OpenSupportSides(world, movement.from) >= 1
                || OpenSupportSides(world, movement.to) >= 1
                || (currentSupport.has_value()
                    && OpenSupportSides(world, *currentSupport) >= 1));
        const bool exposedSprint = movement.type == MovementType::Sprint
            && !preserveMomentumThisMovement_
            && (OpenSupportSides(world, movement.from) >= 2
                || OpenSupportSides(world, movement.to) >= 2
                || (currentSupport.has_value()
                    && OpenSupportSides(world, *currentSupport) >= 2));
        if (preserveMomentumThisMovement_
            && remaining <= brakeDistance
            && !momentumTransitionRecorded_)
        {
            momentumTransitionRecorded_ = true;
            ++metricsDelta_.momentumPreservedTransitions;
        }
        if (edgeBrake && !edgeBrakeIssuedThisMovement_)
        {
            edgeBrakeIssuedThisMovement_ = true;
            ++metricsDelta_.edgeBrakeActions;
        }
        // Sneak deliberately refuses to step off the current collision
        // surface. On a lower slab that also refuses a safe half-block drop,
        // although both voxels are supported. Inspect the next footprint and
        // cross that small, verified height change at controlled walking speed.
        bool descendingSurface = false;
        if ((movement.type == MovementType::Walk || movement.type == MovementType::Sprint)
            && remaining > 0.05f)
        {
            const float probeDistance = std::min(0.65f, remaining);
            const Vector3 probe {
                actorPosition.x + (target.x - actorPosition.x) * probeDistance / remaining,
                actorPosition.y,
                actorPosition.z + (target.z - actorPosition.z) * probeDistance / remaining };
            const std::optional<GridPos> nextSupport = world.FindSupport(probe, 1, 0, profile.bodyCenterAboveSupport);
            if (nextSupport.has_value() && world.IsBodyClear(*nextSupport, profile))
            {
                const float drop = actorPosition.y - world.SupportCenter(*nextSupport, profile.bodyCenterAboveSupport).y;
                descendingSurface = drop > 0.20f && drop <= 0.75f;
            }
        }
        MoveToward(command, actorPosition, target,
            movement.type == MovementType::Sprint && !edgeBrake && !exposedSprint && !descendingSurface,
            !descendingSurface && (edgeBrake || precisionWalk || exposedSprint));
        if (descendingSurface) command.moveForward *= 0.45f;
        // A safe diagonal sprint from the planner carries requiresJump. Keep
        // the hop grounded and cancel it whenever edge braking takes over, so
        // exposed routes still use the conservative walk/sneak behaviour.
        if (movement.type == MovementType::Sprint
            && movement.requiresJump
            && actor.IsOnGround()
            && !edgeBrake
            && !exposedSprint)
        {
            command.jump = true;
        }
        update.proposal = BaseProposal(
            actor, tick, BotControlSource::Navigation, BotControlPriority::Navigation,
            BotControlDomain::Movement | BotControlDomain::Look);
        update.proposal.command = command;
        if (ReachedMovementTarget(actor, movement, world)) AdvanceMovement(actor, update);
        break;
    }

    case MovementType::JumpUp:
    case MovementType::GapJump:
    {
        const int dx = movement.to.x == movement.from.x ? 0
            : (movement.to.x > movement.from.x ? 1 : -1);
        const int dz = movement.to.z == movement.from.z ? 0
            : (movement.to.z > movement.from.z ? 1 : -1);
        const Vector3 takeoffCenter = world.SupportCenter(
            movement.from, profile.bodyCenterAboveSupport);
        const Vector3 runway {
            takeoffCenter.x - static_cast<float>(dx) * profile.gapRunupDistanceBlocks,
            takeoffCenter.y,
            takeoffCenter.z - static_cast<float>(dz) * profile.gapRunupDistanceBlocks
        };
        if (movement.type == MovementType::GapJump
            && phase_ == PathExecutorPhase::JumpRunup)
        {
            MoveToward(command, actorPosition, runway, false, false);
            update.proposal = BaseProposal(
                actor, tick, BotControlSource::Navigation, BotControlPriority::Navigation,
                BotControlDomain::Movement | BotControlDomain::Look);
            update.proposal.command = command;
            if (HorizontalDistance(actorPosition, runway)
                <= settings_.horizontalArrivalTolerance)
            {
                phase_ = PathExecutorPhase::JumpTakeoff;
                phaseElapsedSeconds_ = 0.0f;
                noProgressSeconds_ = 0.0f;
                hasBestTargetDistance_ = false;
            }
            break;
        }
        MoveToward(command, actorPosition, target, movement.requiresSprint, false);
        if (movement.type == MovementType::JumpUp)
        {
            // A one-block climb needs a controlled hop, not the full-speed
            // sprint impulse used to clear a gap. Normal walking acceleration
            // is still required to cross the raised block's collision edge.
            command.sprint = false;
        }
        if (phase_ == PathExecutorPhase::JumpTakeoff)
        {
            // Spend a short, fixed run-up interval at sprint speed before
            // takeoff. This makes the three supported gap sizes reproducible
            // instead of depending on whatever velocity a prior path action
            // happened to leave behind.
            const int gapBlocks = std::max(1,
                std::max(std::abs(movement.to.x - movement.from.x),
                    std::abs(movement.to.z - movement.from.z)) - 1);
            const float takeoffOffset = profile.gapTakeoffEdgeOffsetBlocks
                + static_cast<float>(std::max(0, 3 - gapBlocks))
                    * profile.gapTakeoffGapScale;
            const bool chargingRunup = movement.type == MovementType::GapJump
                && movement.requiresSprint
                && (phaseElapsedSeconds_ < profile.gapTakeoffDelaySeconds
                    || static_cast<float>(dx) * (actorPosition.x - takeoffCenter.x)
                        + static_cast<float>(dz) * (actorPosition.z - takeoffCenter.z)
                        < -takeoffOffset);
            command.jump = movement.requiresJump && !chargingRunup;
            if (!actor.IsOnGround())
            {
                phase_ = PathExecutorPhase::Airborne;
                phaseElapsedSeconds_ = 0.0f;
            }
        }
        if (movement.type == MovementType::GapJump
            && phase_ == PathExecutorPhase::Airborne
            && !actor.IsOnGround())
        {
            const float desiredYaw = command.aimYaw;
            command.aimYaw = actor.GetYaw()
                + WrapAngle(desiredYaw - actor.GetYaw())
                    * profile.gapLandingCorrectionGain;
            command.moveForward *= profile.gapAirControlScale;
        }
        if (phase_ == PathExecutorPhase::Airborne && actor.IsOnGround())
        {
            const Vector3 landingTarget = MovementTarget(movement, world);
            const Vector3 landingPosition = actor.GetPosition();
            const bool exactLanding = ReachedMovementTarget(actor, movement, world);
            const bool supportedRecoveryLanding = !exactLanding
                && HorizontalDistance(landingPosition, landingTarget)
                    <= settings_.jumpLandingTolerance
                && std::fabs(landingPosition.y - landingTarget.y)
                    <= settings_.verticalArrivalTolerance
                && world.FindSupport(
                    landingPosition, 1, 1, profile.bodyCenterAboveSupport).has_value();
            if (exactLanding || supportedRecoveryLanding)
            {
                if (supportedRecoveryLanding) ++metricsDelta_.recoveredJumpLandings;
                if (movement.type == MovementType::GapJump) ++metricsDelta_.successfulGapJumps;
                AdvanceMovement(actor, update);
            }
            else if (phaseElapsedSeconds_ > 0.18f)
            {
                ++metricsDelta_.failedJumps;
                FailCurrent(MovementExecutionStatus::Failed, RepathReason::MovementFailed,
                    "jump landed away from target", update);
                return update;
            }
        }
        update.proposal = BaseProposal(
            actor, tick, BotControlSource::Navigation, BotControlPriority::Navigation,
            BotControlDomain::Movement | BotControlDomain::Look);
        update.proposal.command = command;
        break;
    }

    case MovementType::SneakBridge:
    case MovementType::PlaceBlock:
    {
        const GridPos affected = movement.affectedBlock.value_or(movement.to);
        Vector3 bridgeDirection {
            static_cast<float>(movement.to.x - movement.from.x),
            0.0f,
            static_cast<float>(movement.to.z - movement.from.z)
        };
        const float bridgeDirectionLength = std::sqrt(
            bridgeDirection.x * bridgeDirection.x + bridgeDirection.z * bridgeDirection.z);
        if (bridgeDirectionLength > 0.001f)
        {
            bridgeDirection.x /= bridgeDirectionLength;
            bridgeDirection.z /= bridgeDirectionLength;
        }
        const Vector3 supportCenter = world.SupportCenter(
            movement.from, profile.bodyCenterAboveSupport);
        const Vector3 edgeStand {
            supportCenter.x + bridgeDirection.x * 0.44f,
            supportCenter.y,
            supportCenter.z + bridgeDirection.z * 0.44f
        };
        Vector3 exposedSupportFace = world.GridToWorld(movement.from);
        exposedSupportFace.x += bridgeDirection.x * 0.50f;
        exposedSupportFace.z += bridgeDirection.z * 0.50f;
        exposedSupportFace.y -= 0.25f;
        if (world.IsSolid(affected)
            && phase_ != PathExecutorPhase::BridgeStep
            && phase_ != PathExecutorPhase::Complete)
        {
            ++metricsDelta_.bridgeBlocksUsed;
            phase_ = movement.type == MovementType::PlaceBlock
                ? PathExecutorPhase::Complete
                : PathExecutorPhase::BridgeStep;
            actionIssued_ = true;
        }

        if (phase_ == PathExecutorPhase::BridgeApproach)
        {
            const Vector3 approach = world.SupportCenter(movement.from, profile.bodyCenterAboveSupport);
            MoveToward(command, actorPosition, approach, false, true);
            if (HorizontalDistance(actorPosition, approach) <= settings_.horizontalArrivalTolerance)
            {
                phase_ = PathExecutorPhase::BridgeAlign;
                phaseElapsedSeconds_ = 0.0f;
            }
        }
        else if (phase_ == PathExecutorPhase::BridgeAlign)
        {
            AimAt(command, eye, world.SupportCenter(movement.to, profile.bodyCenterAboveSupport));
            command.sneak = true;
            if (phaseElapsedSeconds_ >= settings_.orientToleranceRadians)
            {
                phase_ = PathExecutorPhase::BridgeSelectBlock;
                phaseElapsedSeconds_ = 0.0f;
            }
        }
        else if (phase_ == PathExecutorPhase::BridgeSelectBlock)
        {
            selectedActionSlot_ = FindBlockHotbarSlot(
                actor, movement.placementBlockType.value_or(profile.preferredBridgeBlock));
            if (selectedActionSlot_ < 0)
            {
                FailCurrent(MovementExecutionStatus::Failed, RepathReason::InventoryChanged,
                    "bridge material is not in the hotbar", update);
                return update;
            }
            selectedBlockCountBefore_ = actor.GetInventory().GetSlot(selectedActionSlot_).count;
            phase_ = PathExecutorPhase::BridgeSneak;
            phaseElapsedSeconds_ = 0.0f;
        }
        else if (phase_ == PathExecutorPhase::BridgeSneak)
        {
            command.sneak = true;
            command.selectedSlot = selectedActionSlot_;
            AimAt(command, eye, world.SupportCenter(movement.to, profile.bodyCenterAboveSupport - 0.7f));
            MoveToward(command, actorPosition, edgeStand, false, true);
            const float edgeProgress = (actorPosition.x - supportCenter.x) * bridgeDirection.x
                + (actorPosition.z - supportCenter.z) * bridgeDirection.z;
            if (edgeProgress >= 0.40f)
            {
                phase_ = PathExecutorPhase::BridgePlace;
                phaseElapsedSeconds_ = 0.0f;
            }
        }
        else if (phase_ == PathExecutorPhase::BridgePlace)
        {
            command.sneak = true;
            command.jump = movement.to.y > movement.from.y;
            command.selectedSlot = selectedActionSlot_;
            command.placeHeld = true;
            command.placePressed = !actionIssued_;
            actionIssued_ = true;
            // Use the regular placement rule: aim at the exposed face of the
            // existing support block so the raycast selects the adjacent cell.
            Vector3 bridgeAim = world.GridToWorld(affected);
            if (movement.to.y > movement.from.y)
            {
                static constexpr std::array<GridPos, 6> anchorOffsets {
                    GridPos { 1, 0, 0 }, GridPos { -1, 0, 0 },
                    GridPos { 0, 1, 0 }, GridPos { 0, -1, 0 },
                    GridPos { 0, 0, 1 }, GridPos { 0, 0, -1 }
                };
                for (const GridPos& offset : anchorOffsets)
                {
                    const GridPos anchor {
                        affected.x + offset.x,
                        affected.y + offset.y,
                        affected.z + offset.z
                    };
                    if (!world.IsSolid(anchor)) continue;
                    bridgeAim = world.GridToWorld(anchor);
                    bridgeAim.x += static_cast<float>(affected.x - anchor.x) * 0.49f;
                    bridgeAim.y += static_cast<float>(affected.y - anchor.y) * 0.49f;
                    bridgeAim.z += static_cast<float>(affected.z - anchor.z) * 0.49f;
                    break;
                }
            }
            else
            {
                bridgeAim.y = exposedSupportFace.y;
            }
            AimAt(command, eye, bridgeAim);
            phase_ = PathExecutorPhase::BridgeConfirm;
            phaseElapsedSeconds_ = 0.0f;
        }
        else if (phase_ == PathExecutorPhase::BridgeConfirm)
        {
            command.sneak = true;
            command.jump = movement.to.y > movement.from.y;
            command.selectedSlot = selectedActionSlot_;
            AimAt(command, eye, world.GridToWorld(affected));
            if (world.IsSolid(affected))
            {
                ++metricsDelta_.bridgeBlocksUsed;
                phase_ = movement.type == MovementType::PlaceBlock
                    ? PathExecutorPhase::Complete
                    : PathExecutorPhase::BridgeStep;
                phaseElapsedSeconds_ = 0.0f;
            }
            else if (phaseElapsedSeconds_ >= 0.05f)
            {
                phase_ = PathExecutorPhase::BridgePlace;
            }
        }

        if (phase_ == PathExecutorPhase::BridgeStep)
        {
            const bool stairStep = movement.to.y > movement.from.y;
            MoveToward(command, actorPosition, target, false, !stairStep);
            command.jump = stairStep && actor.IsOnGround();
            if (ReachedMovementTarget(actor, movement, world)) AdvanceMovement(actor, update);
        }
        else if (phase_ == PathExecutorPhase::Complete)
        {
            AdvanceMovement(actor, update);
        }

        update.proposal = BaseProposal(
            actor, tick, BotControlSource::BridgeAction, BotControlPriority::BridgeAction,
            BotControlDomain::Movement | BotControlDomain::Look
                | BotControlDomain::Hotbar | BotControlDomain::Action,
            BotControlDomain::Look | BotControlDomain::Hotbar | BotControlDomain::Action);
        update.proposal.command = command;
        break;
    }

    case MovementType::BreakBlock:
    {
        const GridPos affected = movement.affectedBlock.value_or(movement.to);
        const Block* block = world.GetBlock(affected);
        if (block == nullptr || !world.IsSolid(affected))
        {
            if (phase_ != PathExecutorPhase::Move)
            {
                ++metricsDelta_.blocksBrokenForNavigation;
                phase_ = PathExecutorPhase::Move;
                phaseElapsedSeconds_ = 0.0f;
            }
        }
        if (phase_ == PathExecutorPhase::BreakApproach)
        {
            const Vector3 approach = world.SupportCenter(movement.from, profile.bodyCenterAboveSupport);
            MoveToward(command, actorPosition, approach, false, false);
            if (HorizontalDistance(actorPosition, approach) <= settings_.horizontalArrivalTolerance)
            {
                phase_ = PathExecutorPhase::BreakAlign;
                phaseElapsedSeconds_ = 0.0f;
            }
        }
        else if (phase_ == PathExecutorPhase::BreakAlign)
        {
            AimAt(command, eye, world.GridToWorld(affected));
            phase_ = PathExecutorPhase::BreakSelectTool;
        }
        else if (phase_ == PathExecutorPhase::BreakSelectTool)
        {
            selectedActionSlot_ = FindToolHotbarSlot(actor, block != nullptr ? block->type : BlockType::Air);
            if (selectedActionSlot_ < 0)
            {
                FailCurrent(MovementExecutionStatus::Failed, RepathReason::InventoryChanged,
                    "required breaking tool is not in the hotbar", update);
                return update;
            }
            expectedAffectedBlock_ = block != nullptr ? std::optional<Block>(*block) : std::nullopt;
            phase_ = PathExecutorPhase::BreakHold;
            phaseElapsedSeconds_ = 0.0f;
        }
        else if (phase_ == PathExecutorPhase::BreakHold)
        {
            command.selectedSlot = selectedActionSlot_;
            command.attackHeld = true;
            AimAt(command, eye, world.GridToWorld(affected));
        }
        else if (phase_ == PathExecutorPhase::Move)
        {
            MoveToward(command, actorPosition, target, false, false);
            if (ReachedMovementTarget(actor, movement, world)) AdvanceMovement(actor, update);
        }
        update.proposal = BaseProposal(
            actor, tick, BotControlSource::BreakAction, BotControlPriority::BreakAction,
            BotControlDomain::Movement | BotControlDomain::Look
                | BotControlDomain::Hotbar | BotControlDomain::Action,
            BotControlDomain::Look | BotControlDomain::Hotbar | BotControlDomain::Action);
        update.proposal.command = command;
        break;
    }

    case MovementType::Wait:
        if (actionElapsedSeconds_ >= std::max(0.05f, movement.timeCost)) AdvanceMovement(actor, update);
        break;
    }

    update.status = status_;
    update.phase = phase_;
    return update;
}

bool PathExecutor::ValidateRemainingPath(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile& profile,
    RepathReason* reason) const
{
    return !FindFirstInvalidMovement(actor, world, profile, nullptr, reason, 6);
}

bool PathExecutor::FindFirstInvalidMovement(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile& profile,
    std::size_t* invalidIndex,
    RepathReason* reason,
    std::size_t lookAhead) const
{
    if (!ValidateInventory(actor, profile, reason))
    {
        if (invalidIndex != nullptr) *invalidIndex = movementIndex_;
        return true;
    }
    const std::size_t end = std::min(path_.movements.size(), movementIndex_ + lookAhead);
    for (std::size_t index = movementIndex_; index < end; ++index)
    {
        if (!ValidateMovement(path_.movements[index], index == movementIndex_, actor, world, profile, reason))
        {
            if (invalidIndex != nullptr) *invalidIndex = index;
            return true;
        }
    }
    if (reason != nullptr) *reason = RepathReason::None;
    return false;
}

bool PathExecutor::AcceptDisjointWorldChanges(
    const NavigationWorldView& world,
    std::size_t lookAhead)
{
    if (validatedWorldRevision_ == world.WorldRevision()) return true;
    if (!HasPath() || movementIndex_ >= path_.movements.size())
    {
        validatedWorldRevision_ = world.WorldRevision();
        return true;
    }
    const std::size_t end = std::min(path_.movements.size(), movementIndex_ + lookAhead);
    GridPos minimum = path_.movements[movementIndex_].from;
    GridPos maximum = minimum;
    const auto include = [&minimum, &maximum](GridPos pos)
    {
        minimum.x = std::min(minimum.x, pos.x);
        minimum.y = std::min(minimum.y, pos.y);
        minimum.z = std::min(minimum.z, pos.z);
        maximum.x = std::max(maximum.x, pos.x);
        maximum.y = std::max(maximum.y, pos.y);
        maximum.z = std::max(maximum.z, pos.z);
    };
    for (std::size_t index = movementIndex_; index < end; ++index)
    {
        const PlannedMovement& movement = path_.movements[index];
        include(movement.from);
        include(movement.to);
        if (movement.affectedBlock.has_value()) include(*movement.affectedBlock);
    }
    // One horizontal cell covers body width/corner motion; two vertical cells
    // cover support plus the standing body volume.
    --minimum.x;
    minimum.y -= 1;
    --minimum.z;
    ++maximum.x;
    maximum.y += 2;
    ++maximum.z;
    const NavigationChangeQuery query = world.QueryNavigationChanges(
        validatedWorldRevision_, minimum, maximum);
    if (query == NavigationChangeQuery::NoChanges
        || query == NavigationChangeQuery::Disjoint)
    {
        validatedWorldRevision_ = world.WorldRevision();
        ++metricsDelta_.dirtyRegionFastAccepts;
        return true;
    }
    if (query == NavigationChangeQuery::HistoryUnavailable)
    {
        ++metricsDelta_.dirtyRegionHistoryMisses;
    }
    else
    {
        ++metricsDelta_.dirtyRegionIntersectValidations;
    }
    return false;
}

void PathExecutor::ConfirmWorldRevision(const NavigationWorldView& world) noexcept
{
    validatedWorldRevision_ = world.WorldRevision();
}

bool PathExecutor::HasPath() const noexcept { return !path_.movements.empty(); }
bool PathExecutor::IsActive() const noexcept
{
    return HasPath() && movementIndex_ < path_.movements.size()
        && status_ != MovementExecutionStatus::Failed
        && status_ != MovementExecutionStatus::Invalidated;
}
const NavigationPath& PathExecutor::Path() const noexcept { return path_; }
const PlannedMovement* PathExecutor::CurrentMovement() const noexcept
{
    return movementIndex_ < path_.movements.size() ? &path_.movements[movementIndex_] : nullptr;
}
std::size_t PathExecutor::MovementIndex() const noexcept { return movementIndex_; }
MovementExecutionStatus PathExecutor::Status() const noexcept { return status_; }
PathExecutorPhase PathExecutor::Phase() const noexcept { return phase_; }
RepathReason PathExecutor::LastRepathReason() const noexcept { return lastRepathReason_; }
const std::string& PathExecutor::LastMessage() const noexcept { return lastMessage_; }
float PathExecutor::NoProgressSeconds() const noexcept { return noProgressSeconds_; }
float PathExecutor::ActionElapsedSeconds() const noexcept { return actionElapsedSeconds_; }
std::uint64_t PathExecutor::ValidatedWorldRevision() const noexcept { return validatedWorldRevision_; }

NavigationMetrics PathExecutor::ConsumeMetricsDelta() noexcept
{
    NavigationMetrics result = metricsDelta_;
    metricsDelta_ = NavigationMetrics {};
    return result;
}

void PathExecutor::BeginCurrentMovement(
    const Player& actor,
    const NavigationWorldView& world,
    const NavigationProfile&)
{
    status_ = MovementExecutionStatus::Running;
    actionElapsedSeconds_ = 0.0f;
    phaseElapsedSeconds_ = 0.0f;
    noProgressSeconds_ = 0.0f;
    hasBestTargetDistance_ = false;
    stuckEventRecorded_ = false;
    recenterRecoveryActive_ = false;
    recenterRecoveryAttempted_ = false;
    recenterRecoveryElapsedSeconds_ = 0.0f;
    edgeBrakeIssuedThisMovement_ = false;
    momentumTransitionRecorded_ = false;
    edgeBrakeEligibleThisMovement_ = false;
    preserveMomentumThisMovement_ = false;
    actionIssued_ = false;
    selectedActionSlot_ = -1;
    expectedAffectedBlock_.reset();
    hasLastActorPosition_ = true;
    lastActorPosition_ = actor.GetPosition();
    const PlannedMovement& movement = path_.movements[movementIndex_];
    if (movement.type == MovementType::Sprint || movement.type == MovementType::Walk)
    {
        const PlannedMovement* next = movementIndex_ + 1 < path_.movements.size()
            ? &path_.movements[movementIndex_ + 1]
            : nullptr;
        preserveMomentumThisMovement_ = movement.type == MovementType::Sprint
            && next != nullptr
            && (next->type == MovementType::JumpUp
                || next->type == MovementType::GapJump);
        const bool terminalAction = next == nullptr && !path_.partial;
        const bool changesActionMode = next != nullptr
            && (next->type == MovementType::SneakBridge
                || next->type == MovementType::PlaceBlock
                || next->type == MovementType::BreakBlock
                || next->type == MovementType::DropDown
                || next->type == MovementType::Wait
                || !SameHorizontalDirection(movement, *next));
        edgeBrakeEligibleThisMovement_ = !preserveMomentumThisMovement_
            && OpenSupportSides(world, movement.to) >= 1
            && (terminalAction || changesActionMode);
    }
    switch (movement.type)
    {
    case MovementType::JumpUp: phase_ = PathExecutorPhase::JumpTakeoff; break;
    case MovementType::GapJump:
        phase_ = movement.requiresSprint ? PathExecutorPhase::JumpRunup : PathExecutorPhase::JumpTakeoff;
        break;
    case MovementType::SneakBridge:
    case MovementType::PlaceBlock: phase_ = PathExecutorPhase::BridgeApproach; break;
    case MovementType::BreakBlock: phase_ = PathExecutorPhase::BreakApproach; break;
    case MovementType::DropDown: phase_ = PathExecutorPhase::Drop; break;
    case MovementType::Wait: phase_ = PathExecutorPhase::Wait; break;
    default: phase_ = PathExecutorPhase::Move; break;
    }
    validatedWorldRevision_ = world.WorldRevision();
}

void PathExecutor::AdvanceMovement(const Player& actor, PathExecutionUpdate& update)
{
    if (edgeBrakeIssuedThisMovement_) ++metricsDelta_.edgeBrakeCompletions;
    ++movementIndex_;
    update.movementAdvanced = true;
    if (movementIndex_ >= path_.movements.size())
    {
        status_ = MovementExecutionStatus::Succeeded;
        phase_ = PathExecutorPhase::Complete;
        update.pathFinished = true;
        RecordRouteSample();
    }
    else
    {
        status_ = MovementExecutionStatus::NotStarted;
        phase_ = PathExecutorPhase::Idle;
    }
    lastActorPosition_ = actor.GetPosition();
}

void PathExecutor::FailCurrent(
    MovementExecutionStatus status,
    RepathReason reason,
    std::string message,
    PathExecutionUpdate& update)
{
    status_ = status;
    phase_ = PathExecutorPhase::Failed;
    lastRepathReason_ = reason;
    lastMessage_ = std::move(message);
    ++metricsDelta_.movementFailures;
    if (const PlannedMovement* movement = CurrentMovement())
    {
        ++metricsDelta_.movementFailuresByMovement[static_cast<std::size_t>(movement->type)];
    }
    metricsDelta_.RecordRepath(reason);
    update.status = status_;
    update.phase = phase_;
    update.repathReason = reason;
    update.needsRepath = true;
    update.message = lastMessage_;
}

BotControlProposal PathExecutor::BaseProposal(
    const Player& actor,
    std::uint32_t tick,
    BotControlSource source,
    int priority,
    BotControlDomain domains,
    BotControlDomain exclusiveDomains) const
{
    BotControlProposal proposal;
    proposal.active = true;
    proposal.source = source;
    proposal.priority = priority;
    proposal.domains = domains;
    proposal.exclusiveDomains = exclusiveDomains;
    proposal.command.controlledPlayerId = static_cast<std::uint32_t>(actor.GetId());
    proposal.command.tick = tick;
    proposal.command.aimYaw = actor.GetYaw();
    proposal.command.selectedSlot = actor.GetSelectedSlot();
    proposal.reason = ToString(phase_);
    return proposal;
}

bool PathExecutor::ValidateMovement(
    const PlannedMovement& movement,
    bool current,
    const Player&,
    const NavigationWorldView& world,
    const NavigationProfile& profile,
    RepathReason* reason) const
{
    const auto fail = [&](RepathReason value)
    {
        if (reason != nullptr) *reason = value;
        return false;
    };
    if (movement.type == MovementType::SneakBridge || movement.type == MovementType::PlaceBlock)
    {
        const GridPos target = movement.affectedBlock.value_or(movement.to);
        const Block* block = world.GetBlock(target);
        if (block != nullptr && world.IsSolid(target))
        {
            return movement.placementBlockType.has_value()
                ? block->type == *movement.placementBlockType
                : true;
        }
        return profile.canPlaceBlocks && world.IsAir(target);
    }
    if (movement.type == MovementType::BreakBlock)
    {
        const GridPos target = movement.affectedBlock.value_or(movement.to);
        return !world.IsSolid(target) || world.IsBreakableObstacle(target, profile)
            ? true : fail(RepathReason::WorldChanged);
    }
    if (movement.type == MovementType::Sprint && movement.traversedCells > 1)
    {
        return NavigationActionLibrary::CanSprintLine(
            movement.from, movement.to, world, profile)
            ? true : fail(RepathReason::WorldChanged);
    }
    if (NavigationActionLibrary::IsDiagonal(movement.from, movement.to))
    {
        return NavigationActionLibrary::CanWalkDiagonal(
            movement.from, movement.to, world, profile)
            ? true : fail(RepathReason::WorldChanged);
    }
    if (!world.IsSupported(movement.to)) return fail(RepathReason::WorldChanged);
    if (!world.IsBodyClear(movement.to, profile))
    {
        (void)current;
        return fail(RepathReason::WorldChanged);
    }
    return true;
}

bool PathExecutor::ValidateInventory(
    const Player& actor,
    const NavigationProfile& profile,
    RepathReason* reason) const
{
    int requiredBlocks = 0;
    for (std::size_t index = movementIndex_; index < path_.movements.size(); ++index)
    {
        const MovementType type = path_.movements[index].type;
        if (type == MovementType::SneakBridge || type == MovementType::PlaceBlock)
        {
            if (!(index == movementIndex_ && actionIssued_)) ++requiredBlocks;
        }
    }
    if (requiredBlocks > actor.GetInventory().GetBlockCount(profile.preferredBridgeBlock))
    {
        if (reason != nullptr) *reason = RepathReason::InventoryChanged;
        return false;
    }
    return true;
}

int PathExecutor::FindBlockHotbarSlot(const Player& actor, BlockType requested) const
{
    const auto& hotbar = actor.GetInventory().GetHotbarSlots();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        const std::optional<BlockType> type = ItemToBlock(hotbar[slot].type);
        if (!hotbar[slot].IsEmpty() && type.has_value() && *type == requested) return slot;
    }
    return -1;
}

int PathExecutor::FindToolHotbarSlot(const Player& actor, BlockType) const
{
    const auto& hotbar = actor.GetInventory().GetHotbarSlots();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        if (!hotbar[slot].IsEmpty() && ItemIsPickaxe(hotbar[slot].type)) return slot;
    }
    return -1;
}

GridPos PathExecutor::EstimateActorSupport(const Player& actor, const NavigationWorldView& world) const
{
    return world.FindSupport(actor.GetPosition(), 4, 1).value_or(
        world.WorldToGrid(Vector3 { actor.GetPosition().x, actor.GetPosition().y - 1.40f, actor.GetPosition().z }));
}

Vector3 PathExecutor::MovementTarget(
    const PlannedMovement& movement,
    const NavigationWorldView& world) const
{
    return world.SupportCenter(movement.to);
}

Vector3 PathExecutor::PlacementAimPoint(
    const GridPos& target,
    Vector3,
    const NavigationWorldView& world) const
{
    return world.GridToWorld(target);
}

void PathExecutor::AimAt(PlayerCommand& command, Vector3 eye, Vector3 target) const
{
    const float dx = target.x - eye.x;
    const float dy = target.y - eye.y;
    const float dz = target.z - eye.z;
    const float horizontal = std::sqrt(dx * dx + dz * dz);
    command.aimYaw = std::atan2(dx, -dz);
    command.aimPitch = std::atan2(dy, std::max(0.0001f, horizontal));
}

void PathExecutor::MoveToward(
    PlayerCommand& command,
    Vector3 eye,
    Vector3 target,
    bool sprint,
    bool sneak) const
{
    AimAt(command, eye, Vector3 { target.x, eye.y, target.z });
    command.moveForward = 1.0f;
    command.moveStrafe = 0.0f;
    command.sprint = sprint && !sneak;
    command.sneak = sneak;
}

bool PathExecutor::ReachedMovementTarget(
    const Player& actor,
    const PlannedMovement& movement,
    const NavigationWorldView& world) const
{
    const Vector3 target = MovementTarget(movement, world);
    const Vector3 position = actor.GetPosition();
    return HorizontalDistance(position, target) <= settings_.horizontalArrivalTolerance
        && std::fabs(position.y - target.y) <= settings_.verticalArrivalTolerance;
}

bool PathExecutor::TrackProgress(const Player& actor, Vector3 target, float deltaSeconds)
{
    const Vector3 position = actor.GetPosition();
    const float distance = Distance(position, target);
    bool madeProgress = false;
    if (!hasBestTargetDistance_ || distance < bestTargetDistance_ - settings_.progressEpsilon)
    {
        bestTargetDistance_ = distance;
        hasBestTargetDistance_ = true;
        noProgressSeconds_ = 0.0f;
        madeProgress = true;
    }
    else
    {
        noProgressSeconds_ += deltaSeconds;
    }
    if (hasLastActorPosition_)
    {
        routeDistanceTravelled_ += Distance(position, lastActorPosition_);
    }
    lastActorPosition_ = position;
    hasLastActorPosition_ = true;
    return madeProgress;
}

float PathExecutor::CurrentTimeoutSeconds() const
{
    const PlannedMovement* movement = CurrentMovement();
    const float expected = movement != nullptr ? movement->timeCost : 0.5f;
    return std::max(settings_.minimumMovementTimeoutSeconds,
        expected * settings_.movementTimeoutScale + settings_.actionConfirmationSeconds);
}

void PathExecutor::RecordRouteSample()
{
    if (routeStraightDistance_ > 0.01f && routeDistanceTravelled_ > 0.01f)
    {
        metricsDelta_.routeEfficiencyTotal += std::clamp(
            static_cast<double>(routeStraightDistance_ / routeDistanceTravelled_), 0.0, 1.0);
        ++metricsDelta_.routeEfficiencySamples;
    }
}

const char* ToString(MovementExecutionStatus status) noexcept
{
    switch (status)
    {
    case MovementExecutionStatus::NotStarted: return "NotStarted";
    case MovementExecutionStatus::Running: return "Running";
    case MovementExecutionStatus::Succeeded: return "Succeeded";
    case MovementExecutionStatus::TemporarilyBlocked: return "TemporarilyBlocked";
    case MovementExecutionStatus::Invalidated: return "Invalidated";
    case MovementExecutionStatus::Failed: return "Failed";
    }
    return "Unknown";
}

const char* ToString(RepathReason reason) noexcept
{
    switch (reason)
    {
    case RepathReason::None: return "None";
    case RepathReason::GoalChanged: return "GoalChanged";
    case RepathReason::WorldChanged: return "WorldChanged";
    case RepathReason::MovementFailed: return "MovementFailed";
    case RepathReason::ActorDisplaced: return "ActorDisplaced";
    case RepathReason::InventoryChanged: return "InventoryChanged";
    case RepathReason::ThreatChanged: return "ThreatChanged";
    case RepathReason::PathExhausted: return "PathExhausted";
    case RepathReason::ActionInvalid: return "ActionInvalid";
    case RepathReason::ActionTimeout: return "ActionTimeout";
    case RepathReason::EmergencySave: return "EmergencySave";
    case RepathReason::Count: break;
    }
    return "Unknown";
}

const char* ToString(PathExecutorPhase phase) noexcept
{
    switch (phase)
    {
    case PathExecutorPhase::Idle: return "Idle";
    case PathExecutorPhase::Orient: return "Orient";
    case PathExecutorPhase::Move: return "Move";
    case PathExecutorPhase::JumpRunup: return "JumpRunup";
    case PathExecutorPhase::JumpTakeoff: return "JumpTakeoff";
    case PathExecutorPhase::Airborne: return "Airborne";
    case PathExecutorPhase::Drop: return "Drop";
    case PathExecutorPhase::Wait: return "Wait";
    case PathExecutorPhase::BridgeApproach: return "BridgeApproach";
    case PathExecutorPhase::BridgeAlign: return "BridgeAlign";
    case PathExecutorPhase::BridgeSelectBlock: return "BridgeSelectBlock";
    case PathExecutorPhase::BridgeSneak: return "BridgeSneak";
    case PathExecutorPhase::BridgePlace: return "BridgePlace";
    case PathExecutorPhase::BridgeConfirm: return "BridgeConfirm";
    case PathExecutorPhase::BridgeStep: return "BridgeStep";
    case PathExecutorPhase::BreakApproach: return "BreakApproach";
    case PathExecutorPhase::BreakAlign: return "BreakAlign";
    case PathExecutorPhase::BreakSelectTool: return "BreakSelectTool";
    case PathExecutorPhase::BreakHold: return "BreakHold";
    case PathExecutorPhase::BreakConfirm: return "BreakConfirm";
    case PathExecutorPhase::Complete: return "Complete";
    case PathExecutorPhase::Failed: return "Failed";
    }
    return "Unknown";
}

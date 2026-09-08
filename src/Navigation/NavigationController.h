#pragma once

#include "Navigation/BotControlArbiter.h"
#include "Navigation/NavigationGoal.h"
#include "Navigation/PathExecutor.h"
#include "Navigation/VoxelPathfinder.h"

#include <cstdint>
#include <string>

class NavigationWorldView;
class Player;

struct NavigationControllerSettings
{
    NavigationSearchLimits searchLimits {};
    int maxSegmentActions = 16;
    float repathCooldownSeconds = 0.18f;
    float failedRepathCooldownSeconds = 0.35f;
    float criticalThreatCost = 18.0f;
    float threatRepathDelta = 8.0f;
    float emergencyReactionSeconds = 0.06f;
    int maxActionFailuresBeforeAbandon = 3;
    bool enableCorridorSearch = true;
    float corridorMinimumLength = 10.0f;
    float corridorHalfWidth = 12.0f;
    int corridorVerticalPadding = 12;
    bool enableSegmentRepair = true;
    int segmentRepairLookAhead = 6;
    int segmentRepairReconnectActions = 4;
    int segmentRepairMaxExpansions = 900;
    int segmentRepairMaxActions = 32;
    int segmentRepairRadius = 18;
    float segmentRepairCorridorHalfWidth = 7.0f;
};

struct NavigationDebugSnapshot
{
    std::string goal;
    NavigationPath path;
    std::size_t movementIndex = 0;
    MovementExecutionStatus executionStatus = MovementExecutionStatus::NotStarted;
    PathExecutorPhase phase = PathExecutorPhase::Idle;
    RepathReason lastRepath = RepathReason::None;
    float noProgressSeconds = 0.0f;
    float currentThreat = 0.0f;
    bool goalSatisfied = false;
    bool pathStale = false;
};

struct NavigationControllerUpdate
{
    PlayerCommand command {};
    PathExecutionUpdate execution {};
    bool planned = false;
    bool hasCommand = false;
    bool goalSatisfied = false;
    bool routeAbandoned = false;
    RepathReason repathReason = RepathReason::None;
    MovementType failedMovementType = MovementType::Count;
    GridPos failedMovementFrom {};
    GridPos failedMovementTo {};
    bool blockedStart = false;
    bool blockedStartRecovered = false;
    GridPos blockedStartSupport {};
    GridPos blockedStartBlock {};
    BlockType blockedStartBlockType = BlockType::Air;
    bool blockedStartRelocated = false;
    GridPos blockedStartRelocationSupport {};
    bool supportLost = false;
    MovementType supportLostMovementType = MovementType::Count;
    GridPos supportLostMovementFrom {};
    GridPos supportLostMovementTo {};
};

class NavigationController
{
public:
    explicit NavigationController(NavigationControllerSettings settings = {});

    void SetGoal(NavigationGoalPtr goal, std::uint64_t signature);
    void ClearGoal();
    void ForceRepath(RepathReason reason);

    NavigationControllerUpdate Update(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile,
        float deltaSeconds,
        std::uint32_t tick);

    bool HasGoal() const noexcept { return goal_ != nullptr; }
    bool GoalSatisfied() const noexcept { return goalSatisfied_; }
    std::uint64_t GoalSignature() const noexcept { return goalSignature_; }
    const NavigationDebugSnapshot& DebugSnapshot() const noexcept { return debug_; }
    const NavigationMetrics& Metrics() const noexcept { return metrics_; }
    NavigationMetrics ConsumeMetricsDelta() noexcept;
    PathExecutor& Executor() noexcept { return executor_; }
    const PathExecutor& Executor() const noexcept { return executor_; }

private:
    bool Plan(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile);
    bool TryRepairSegment(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile,
        std::size_t invalidIndex);
    bool TryConvertFailedGapToBridge(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile);
    bool TryRecoverFailedAction(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile);
    BotControlProposal BuildEmergencyProposal(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile,
        std::uint32_t tick);
    int FindBridgeSlot(const Player& actor, BlockType type) const;
    void MergeMetrics(const NavigationMetrics& delta);
    void RefreshDebug(const NavigationWorldView& world);

    NavigationControllerSettings settings_;
    VoxelPathfinder pathfinder_;
    PathExecutor executor_;
    BotControlArbiter arbiter_;
    NavigationGoalPtr goal_;
    std::uint64_t goalSignature_ = 0;
    float repathCooldown_ = 0.0f;
    float emergencyTimer_ = 0.0f;
    float lastThreat_ = 0.0f;
    RepathReason pendingRepath_ = RepathReason::None;
    RepathReason lastRepath_ = RepathReason::None;
    bool goalSatisfied_ = false;
    bool emergencyIssued_ = false;
    bool hadSupportLastUpdate_ = false;
    bool fallThreatActive_ = false;
    float blockedStartSeconds_ = 0.0f;
    int consecutiveActionFailures_ = 0;
    bool hasRecoveredAction_ = false;
    MovementType recoveredActionType_ = MovementType::Count;
    GridPos recoveredActionFrom_ {};
    GridPos recoveredActionTo_ {};
    struct FailedTransitionMemory
    {
        NavigationFailedTransition transition;
        float remainingSeconds = 0.0f;
    };
    std::vector<FailedTransitionMemory> failedTransitions_;
    NavigationMetrics metrics_;
    NavigationMetrics metricsDelta_;
    NavigationDebugSnapshot debug_;
};

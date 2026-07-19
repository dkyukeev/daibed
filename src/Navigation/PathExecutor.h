#pragma once

#include "Navigation/BotControlArbiter.h"
#include "Navigation/NavigationProfile.h"
#include "Navigation/NavigationTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

class NavigationWorldView;
class Player;

enum class MovementExecutionStatus : std::uint8_t
{
    NotStarted,
    Running,
    Succeeded,
    TemporarilyBlocked,
    Invalidated,
    Failed
};

enum class RepathReason : std::uint8_t
{
    None,
    GoalChanged,
    WorldChanged,
    MovementFailed,
    ActorDisplaced,
    InventoryChanged,
    ThreatChanged,
    PathExhausted,
    ActionInvalid,
    ActionTimeout,
    EmergencySave,
    Count
};

enum class PathExecutorPhase : std::uint8_t
{
    Idle,
    Orient,
    Move,
    JumpRunup,
    JumpTakeoff,
    Airborne,
    Drop,
    Wait,
    BridgeApproach,
    BridgeAlign,
    BridgeSelectBlock,
    BridgeSneak,
    BridgePlace,
    BridgeConfirm,
    BridgeStep,
    BreakApproach,
    BreakAlign,
    BreakSelectTool,
    BreakHold,
    BreakConfirm,
    Complete,
    Failed
};

constexpr std::size_t kRepathReasonCount = static_cast<std::size_t>(RepathReason::Count);

struct NavigationMetrics
{
    std::uint64_t pathRequests = 0;
    std::uint64_t successfulPaths = 0;
    std::uint64_t partialPaths = 0;
    std::uint64_t failedPathRequests = 0;
    std::array<std::uint64_t, kNavigationSearchStatusCount> pathResultsByStatus {};
    std::uint64_t blockedStartDeferrals = 0;
    std::uint64_t blockedStartRecoveries = 0;
    std::uint64_t expandedNodes = 0;
    std::uint64_t generatedNodes = 0;
    std::uint64_t heapDecreaseKeys = 0;
    std::uint64_t peakOpenNodes = 0;
    double pathfindingMilliseconds = 0.0;

    std::uint64_t repaths = 0;
    std::array<std::uint64_t, kRepathReasonCount> repathReasons {};
    std::uint64_t movementFailures = 0;
    std::array<std::uint64_t, kMovementTypeCount> movementFailuresByMovement {};
    std::uint64_t stuckEvents = 0;
    std::array<std::uint64_t, kMovementTypeCount> stuckEventsByMovement {};
    std::uint64_t recenterRecoveryAttempts = 0;
    std::uint64_t recenterRecoveryRetries = 0;
    std::uint64_t actionRecoveryAttempts = 0;
    std::uint64_t actionRecoverySuccesses = 0;
    std::uint64_t actionRecoveryFailures = 0;
    std::uint64_t gapToBridgeAttempts = 0;
    std::uint64_t gapToBridgeSuccesses = 0;
    std::uint64_t gapToBridgeFailures = 0;
    std::uint64_t routeAbandonments = 0;
    std::uint64_t bridgeBlocksUsed = 0;
    std::uint64_t blocksBrokenForNavigation = 0;
    std::uint64_t failedJumps = 0;
    std::uint64_t successfulGapJumps = 0;
    std::uint64_t recoveredJumpLandings = 0;
    std::uint64_t emergencySaveAttempts = 0;
    std::uint64_t emergencySaves = 0;
    std::uint64_t voidDeaths = 0;
    std::array<std::uint64_t, kMovementTypeCount> voidDeathsByMovement {};
    std::uint64_t unforcedVoidDeaths = 0;
    std::array<std::uint64_t, kMovementTypeCount> unforcedVoidDeathsByMovement {};
    std::uint64_t combatAttributedVoidDeaths = 0;
    std::uint64_t voidDeathsOutsideNavigation = 0;
    std::uint64_t momentumPreservedTransitions = 0;
    std::uint64_t edgeBrakeActions = 0;
    std::uint64_t edgeBrakeCompletions = 0;
    std::uint64_t cancelledActions = 0;
    std::uint64_t corridorPlans = 0;
    std::uint64_t corridorReuses = 0;
    std::uint64_t corridorAdvances = 0;
    std::uint64_t corridorFailures = 0;
    std::uint64_t routeBridgeSegmentsStarted = 0;
    std::uint64_t routeBridgeSegmentsCompleted = 0;
    std::uint64_t routeBridgeFollowersHeld = 0;
    std::uint64_t longSprintActions = 0;
    std::uint64_t diagonalActions = 0;
    std::uint64_t corridorConstrainedSearches = 0;
    std::uint64_t corridorFallbackSearches = 0;
    std::uint64_t corridorRejectedNodes = 0;
    std::uint64_t segmentRepairAttempts = 0;
    std::uint64_t segmentRepairSuccesses = 0;
    std::uint64_t segmentRepairFailures = 0;
    std::uint64_t segmentRepairReusedActions = 0;
    double segmentRepairMilliseconds = 0.0;
    std::uint64_t segmentRepairExpandedNodes = 0;
    std::uint64_t dirtyRegionFastAccepts = 0;
    std::uint64_t dirtyRegionIntersectValidations = 0;
    std::uint64_t dirtyRegionHistoryMisses = 0;
    double routeEfficiencyTotal = 0.0;
    std::uint64_t routeEfficiencySamples = 0;
    double noProgressSeconds = 0.0;

    void Add(const NavigationMetrics& other) noexcept;
    void RecordRepath(RepathReason reason) noexcept;
    double AverageExpandedNodes() const noexcept;
    double AverageGeneratedNodes() const noexcept;
    double AveragePathfindingMilliseconds() const noexcept;
    double AverageRouteEfficiency() const noexcept;
};

struct PathExecutorSettings
{
    float horizontalArrivalTolerance = 0.30f;
    float verticalArrivalTolerance = 0.62f;
    float jumpLandingTolerance = 0.78f;
    float orientToleranceRadians = 0.10f;
    float progressEpsilon = 0.025f;
    float recenterRecoveryTriggerSeconds = 0.52f;
    float recenterRecoveryDurationSeconds = 0.34f;
    float stuckSeconds = 0.85f;
    float minimumMovementTimeoutSeconds = 1.50f;
    float movementTimeoutScale = 3.0f;
    float actionConfirmationSeconds = 1.15f;
    float bridgeSneakSettleSeconds = 0.10f;
    float maximumInteractionReach = 4.55f;
    float actorDisplacementBlocks = 3.25f;
    float eyeHeight = 0.78f;
};

struct PathExecutionUpdate
{
    BotControlProposal proposal {};
    MovementExecutionStatus status = MovementExecutionStatus::NotStarted;
    PathExecutorPhase phase = PathExecutorPhase::Idle;
    RepathReason repathReason = RepathReason::None;
    bool movementAdvanced = false;
    bool pathFinished = false;
    bool needsRepath = false;
    std::string message;
};

// Executes a NavigationPath solely by proposing PlayerCommand fields. The
// authoritative caller remains responsible for applying that command and for
// mutating Player, World and Inventory.
class PathExecutor
{
public:
    explicit PathExecutor(PathExecutorSettings settings = {});

    void SetPath(NavigationPath path);
    void Reset();
    void Cancel(RepathReason reason, std::string message = {});

    PathExecutionUpdate Update(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile,
        float deltaSeconds,
        std::uint32_t tick);

    bool ValidateRemainingPath(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile,
        RepathReason* reason = nullptr) const;
    bool FindFirstInvalidMovement(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile,
        std::size_t* invalidIndex,
        RepathReason* reason = nullptr,
        std::size_t lookAhead = 6) const;
    bool AcceptDisjointWorldChanges(
        const NavigationWorldView& world,
        std::size_t lookAhead = 6);
    void ConfirmWorldRevision(const NavigationWorldView& world) noexcept;

    bool HasPath() const noexcept;
    bool IsActive() const noexcept;
    const NavigationPath& Path() const noexcept;
    const PlannedMovement* CurrentMovement() const noexcept;
    std::size_t MovementIndex() const noexcept;
    MovementExecutionStatus Status() const noexcept;
    PathExecutorPhase Phase() const noexcept;
    RepathReason LastRepathReason() const noexcept;
    const std::string& LastMessage() const noexcept;
    float NoProgressSeconds() const noexcept;
    float ActionElapsedSeconds() const noexcept;
    std::uint64_t ValidatedWorldRevision() const noexcept;

    NavigationMetrics ConsumeMetricsDelta() noexcept;

private:
    void BeginCurrentMovement(
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile);
    void AdvanceMovement(const Player& actor, PathExecutionUpdate& update);
    void FailCurrent(
        MovementExecutionStatus status,
        RepathReason reason,
        std::string message,
        PathExecutionUpdate& update);

    BotControlProposal BaseProposal(
        const Player& actor,
        std::uint32_t tick,
        BotControlSource source,
        int priority,
        BotControlDomain domains,
        BotControlDomain exclusiveDomains = BotControlDomain::None) const;

    bool ValidateMovement(
        const PlannedMovement& movement,
        bool current,
        const Player& actor,
        const NavigationWorldView& world,
        const NavigationProfile& profile,
        RepathReason* reason) const;
    bool ValidateInventory(
        const Player& actor,
        const NavigationProfile& profile,
        RepathReason* reason) const;

    int FindBlockHotbarSlot(const Player& actor, BlockType requested) const;
    int FindToolHotbarSlot(const Player& actor, BlockType block) const;
    GridPos EstimateActorSupport(const Player& actor, const NavigationWorldView& world) const;
    Vector3 MovementTarget(const PlannedMovement& movement, const NavigationWorldView& world) const;
    Vector3 PlacementAimPoint(
        const GridPos& target,
        Vector3 eye,
        const NavigationWorldView& world) const;
    void AimAt(PlayerCommand& command, Vector3 eye, Vector3 target) const;
    void MoveToward(
        PlayerCommand& command,
        Vector3 eye,
        Vector3 target,
        bool sprint,
        bool sneak) const;
    bool ReachedMovementTarget(
        const Player& actor,
        const PlannedMovement& movement,
        const NavigationWorldView& world) const;
    // Returns true when the actor closed distance to the target this tick.
    bool TrackProgress(const Player& actor, Vector3 target, float deltaSeconds);
    float CurrentTimeoutSeconds() const;
    void RecordRouteSample();

    PathExecutorSettings settings_;
    NavigationPath path_;
    std::size_t movementIndex_ = 0;
    MovementExecutionStatus status_ = MovementExecutionStatus::NotStarted;
    PathExecutorPhase phase_ = PathExecutorPhase::Idle;
    RepathReason lastRepathReason_ = RepathReason::None;
    std::string lastMessage_;

    float actionElapsedSeconds_ = 0.0f;
    float phaseElapsedSeconds_ = 0.0f;
    float noProgressSeconds_ = 0.0f;
    float bestTargetDistance_ = 0.0f;
    bool hasBestTargetDistance_ = false;
    bool stuckEventRecorded_ = false;
    bool recenterRecoveryActive_ = false;
    bool recenterRecoveryAttempted_ = false;
    float recenterRecoveryElapsedSeconds_ = 0.0f;
    Vector3 recenterRecoveryTarget_ {};
    bool edgeBrakeIssuedThisMovement_ = false;
    bool momentumTransitionRecorded_ = false;
    bool edgeBrakeEligibleThisMovement_ = false;
    bool preserveMomentumThisMovement_ = false;
    bool actionIssued_ = false;

    int selectedActionSlot_ = -1;
    int selectedBlockCountBefore_ = 0;
    std::optional<Block> expectedAffectedBlock_;
    std::uint64_t validatedWorldRevision_ = 0;

    bool hasLastActorPosition_ = false;
    Vector3 lastActorPosition_ {};
    float routeDistanceTravelled_ = 0.0f;
    float routeStraightDistance_ = 0.0f;

    NavigationMetrics metricsDelta_;
};

const char* ToString(MovementExecutionStatus status) noexcept;
const char* ToString(RepathReason reason) noexcept;
const char* ToString(PathExecutorPhase phase) noexcept;

#pragma once

#include "Block.h"
#include "BotStrategicPolicy.h"
#include "raylib.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

enum class BotDifficulty
{
    Easy,
    Normal,
    Hard
};

enum class BotState
{
    Collect,
    Shop,
    Bridge,
    BreakDefense,
    AttackCore,
    Fight,
    Retreat
};

enum class BotRole
{
    Defender,
    Rusher,
    Collector,
    Fighter
};

enum class BotArchetype : std::uint8_t
{
    CautiousDefender,
    AggressiveRusher,
    FrugalBuilder,
    IsolationHunter,
    TeamHelper,
    ImpulsiveDuelist,
    Engineer,
    Opportunist
};

enum class BotIntent
{
    DefendCore,
    RepairCoreDefense,
    GearUp,
    SecureResources,
    PressureCore,
    BreakCoreDefense,
    FightEnemy,
    ChaseWeakEnemy,
    RetreatHome,
    Recover
};

enum class CoordinationSignal : std::uint8_t
{
    None,
    AttackingCore,
    DefendingCore,
    BuildingBridge,
    Retreating,
    CallingForHelp,
    HoldingMid
};

enum class StrategicGoal : std::uint8_t
{
    Idle,
    EconomicPhase,
    BridgePush,
    CoreAssault,
    BaseDefense,
    HuntPlayers,
    MidControl
};

struct StrategicPlan
{
    StrategicGoal goal = StrategicGoal::Idle;
    float plannedDuration = 0.0f;
    float elapsedTime = 0.0f;
    int targetTeamId = -1;
    bool committed = false;
    std::string reason;
    // A plan is a small task chain, not a single tick-level choice.  Stages are
    // intentionally generic so economy/defense/assault plans share the same
    // bounded state machine: prepare -> travel -> execute -> disengage.
    int stage = 0;
    int stageCount = 1;
    float expectedValue = 0.0f;
    float allowedRisk = 0.5f;
    float minimumCommitSeconds = 0.0f;
    int sequence = 0;
};

struct BotRoleTuning
{
    float desiredBlocks = 24.0f;
    float retreatHealthCoreAlive = 34.0f;
    float retreatHealthFinalLife = 20.0f;
    float fightHealth = 28.0f;
    float lootReturnValue = 30.0f;
    float engageRange = 5.0f;
    float roleLockSeconds = 5.0f;
    float pressureBiasScale = 1.0f;
    float defenseBiasScale = 1.0f;
    float resourceBiasScale = 1.0f;
    float combatBiasScale = 1.0f;
    float aggression = 1.0f;
};

struct BotTuningGenome
{
    int schemaVersion = 2;
    std::string id = "default";
    int generation = 0;
    float fitness = 0.0f;
    std::array<BotRoleTuning, 4> roles {};
    float intentLockScale = 1.0f;
    float roleLockScale = 1.0f;
    float fightRequiredMarginEasy = 24.0f;
    float fightRequiredMarginNormal = 8.0f;
    float fightRequiredMarginHard = -6.0f;
    float retreatPowerMarginEasy = -30.0f;
    float retreatPowerMarginNormal = -30.0f;
    float retreatPowerMarginHard = -42.0f;
    float allyAssistWeight = 0.52f;
    float enemyAssistWeight = 0.48f;
    float shieldPower = 13.0f;
    float speedBoostPower = 10.0f;
    float strategicDefenseUrgencyScale = 0.30f;
    float repairUrgencyScale = 0.34f;
    float strategicAttackUrgencyScale = 0.30f;
    float lateAttackUrgencyScale = 0.20f;
    float attackSlotBonus = 82.0f;
    float attackSlotPenalty = -96.0f;
    float breakCoordinationPenalty = 70.0f;
    float pressureCoordinationPenalty = 85.0f;
    float lateCoordinationPenalty = 80.0f;
    float strategicEconomyBonus = 95.0f;
    float personalEconomyBonus = 130.0f;
    float strategicPressureEconomyPenalty = 55.0f;
    float easyPlanCadence = 3.6f;
    float normalPlanCadence = 2.4f;
    float hardPlanCadence = 1.6f;
    float earlyEconomySeconds = 40.0f;
    float pressurePhaseSeconds = 42.0f;
    float latePressureSeconds = 120.0f;
    float allInSeconds = 185.0f;

    // Navigation genes are consumed by the same authoritative navigation
    // profile in normal matches, automatch and the headless genetic trainer.
    float navigationRunupDistanceBlocks = 0.55f;
    float navigationTakeoffDelaySeconds = 0.052f;
    float navigationTakeoffEdgeOffsetBlocks = 0.30f;
    float navigationTakeoffGapScale = 0.116f;
    float navigationAirControlScale = 0.76f;
    float navigationLandingCorrectionGain = 0.84f;
    float navigationFallRiskPenalty = 0.50f;
};

BotTuningGenome DefaultBotTuningGenome();
void ClampBotTuningGenome(BotTuningGenome& genome);
unsigned int BotTuningGenomeHash(const BotTuningGenome& genome);
bool LoadBotTuningGenomeSetFromJsonFile(
    const std::string& path,
    std::array<BotTuningGenome, 4>& genomes,
    std::string* errorMessage = nullptr);
bool WriteBotTuningGenomeJsonFile(
    const std::string& path,
    const BotTuningGenome& genome,
    std::string* errorMessage = nullptr);

struct TeamCoordinationEntry
{
    int playerId = -1;
    CoordinationSignal signal = CoordinationSignal::None;
    Vector3 targetPosition {};
    float timestamp = 0.0f;
    int targetTeamId = -1;
};

struct TeamCoordinationBus
{
    static constexpr int MaxEntries = 8;
    static constexpr int MaxOpenedRoutes = 16;

    std::array<TeamCoordinationEntry, MaxEntries> entries {};
    int count = 0;
    BotAttackCommitment attackCommitment;
    BotSearchMemory searchMemory;
    int pressureTeamId = -1;
    int reserveDefenderId = -1;
    int assistActorId = -1;
    Vector3 sharedAttackRoute {};
    Vector3 weakDefensePoint {};
    float pressureTimestamp = -1000.0f;
    float defenseRequestTimestamp = -1000.0f;
    int emergencyPrimaryDefenderId = -1;
    int emergencySecondaryDefenderId = -1;
    float emergencyDefenderAssignmentUntil = -1000.0f;
    int bridgeBuilderId = -1;
    std::uint64_t bridgeRouteSignature = 0;
    float bridgeReservationUntil = -1000.0f;
    float bridgeLastProgressTimestamp = -1000.0f;
    Vector3 bridgeLastProgressPosition {};
    int bridgeLastProgressBlocks = -1;
    // A local path can end at a short void gap even when the authored route
    // graph has no bridge segment there.  Keep one concrete request per team
    // so a stranded bot can hand the build to an ally with blocks.
    int bridgeRequestorId = -1;
    int bridgeRequestBuilderId = -1;
    BotIntent bridgeRequestIntent = BotIntent::SecureResources;
    Vector3 bridgeRequestTarget {};
    int bridgeRequestTargetTeamId = -1;
    float bridgeRequestTimestamp = -1000.0f;
    float bridgeRequestReservationUntil = -1000.0f;
    float bridgeRequestProgressAt = -1000.0f;
    float bridgeRequestBestDistanceSq = 0.0f;
    int bridgeRequestLastBlocks = 0;
    int preferredBridgeHelperId = -1;
    float bridgeHelperRecheckAt = 0.0f;
    std::vector<BotBridgeHelpFollowup> bridgeHelpFollowups;
    // One stable cleanup assignment lets the team finish a Core-less opponent
    // without pulling every attacker away from the remaining live Cores.
    int cleanupHunterId = -1;
    int cleanupTargetTeamId = -1;
    Vector3 cleanupLastKnownPosition {};
    float cleanupLastSeenTimestamp = -1000.0f;
    std::array<std::uint64_t, MaxOpenedRoutes> openedRouteSignatures {};
    std::array<float, MaxOpenedRoutes> routeOpenedTimestamps {};
    int openedRouteWriteIndex = 0;

    void Clear()
    {
        entries = {};
        count = 0;
        attackCommitment = {};
        searchMemory = {};
        pressureTeamId = -1;
        reserveDefenderId = -1;
        assistActorId = -1;
        sharedAttackRoute = {};
        weakDefensePoint = {};
        pressureTimestamp = -1000.0f;
        defenseRequestTimestamp = -1000.0f;
        emergencyPrimaryDefenderId = -1;
        emergencySecondaryDefenderId = -1;
        emergencyDefenderAssignmentUntil = -1000.0f;
        bridgeBuilderId = -1;
        bridgeRouteSignature = 0;
        bridgeReservationUntil = -1000.0f;
        bridgeLastProgressTimestamp = -1000.0f;
        bridgeLastProgressPosition = {};
        bridgeLastProgressBlocks = -1;
        bridgeRequestorId = -1;
        bridgeRequestBuilderId = -1;
        bridgeRequestIntent = BotIntent::SecureResources;
        bridgeRequestTarget = {};
        bridgeRequestTargetTeamId = -1;
        bridgeRequestTimestamp = -1000.0f;
        bridgeRequestReservationUntil = -1000.0f;
        bridgeRequestProgressAt = -1000.0f;
        bridgeRequestBestDistanceSq = 0.0f;
        bridgeRequestLastBlocks = 0;
        preferredBridgeHelperId = -1;
        bridgeHelperRecheckAt = 0.0f;
        bridgeHelpFollowups.clear();
        cleanupHunterId = -1;
        cleanupTargetTeamId = -1;
        cleanupLastKnownPosition = {};
        cleanupLastSeenTimestamp = -1000.0f;
        openedRouteSignatures = {};
        routeOpenedTimestamps.fill(-1000.0f);
        openedRouteWriteIndex = 0;
    }

    bool PublishBridgeRequest(int requestor, BotIntent intent, Vector3 target, int targetTeam, float now)
    {
        // Give a completed rendezvous time to produce observable progress;
        // otherwise the same stranded bot repeatedly summons a nearby ally.
        for (const auto& followup : bridgeHelpFollowups)
            if (followup.requesterId == requestor && now - followup.arrivedAt < 30.0f) return false;
        const bool live = bridgeRequestorId >= 0 && now - bridgeRequestTimestamp <= 7.0f;
        if (live && bridgeRequestorId != requestor) return false;
        const float dx = target.x - bridgeRequestTarget.x;
        const float dy = target.y - bridgeRequestTarget.y;
        const float dz = target.z - bridgeRequestTarget.z;
        const bool same = live && bridgeRequestIntent == intent && bridgeRequestTargetTeamId == targetTeam
            && dx * dx + dy * dy + dz * dz <= 4.0f;
        if (!same)
        {
            preferredBridgeHelperId = -1;
            bridgeHelperRecheckAt = 0.0f;
            bridgeRequestBuilderId = -1;
            bridgeRequestReservationUntil = -1000.0f;
            bridgeRequestProgressAt = -1000.0f;
            bridgeRequestTarget = target;
        }
        bridgeRequestorId = requestor;
        bridgeRequestIntent = intent;
        bridgeRequestTargetTeamId = targetTeam;
        bridgeRequestTimestamp = now;
        return true;
    }

    bool TryClaimBridgeRequest(int builder, Vector3 position, int blocks, float now, bool ownerAlive)
    {
        if (bridgeRequestorId < 0 || bridgeRequestorId == builder || now - bridgeRequestTimestamp > 7.0f)
            return false;
        const float dx = position.x - bridgeRequestTarget.x;
        const float dy = position.y - bridgeRequestTarget.y;
        const float dz = position.z - bridgeRequestTarget.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        if (bridgeRequestBuilderId == builder)
        {
            if (blocks <= 0) return false;
            if (distance + 1.0f < bridgeRequestBestDistanceSq || blocks < bridgeRequestLastBlocks)
            {
                bridgeRequestProgressAt = now;
                bridgeRequestBestDistanceSq = distance;
            }
            if (now - bridgeRequestProgressAt > 12.0f) return false;
        }
        else
        {
            if (blocks < 4 || (ownerAlive && bridgeRequestReservationUntil > now)) return false;
            bridgeRequestBuilderId = builder;
            bridgeRequestProgressAt = now;
            bridgeRequestBestDistanceSq = distance;
        }
        bridgeRequestLastBlocks = blocks;
        bridgeRequestReservationUntil = now + 2.5f;
        bridgeRequestTimestamp = now;
        return true;
    }

    bool CompleteBridgeRequest(int builder)
    {
        if (bridgeRequestBuilderId != builder) return false;
        bridgeRequestorId = -1;
        bridgeRequestBuilderId = -1;
        bridgeRequestTimestamp = -1000.0f;
        bridgeRequestReservationUntil = -1000.0f;
        return true;
    }

    bool TryReserveBridge(int builderId, std::uint64_t signature, float now, Vector3 position, int blocks)
    {
        if (signature == 0 || blocks <= 0) return false;
        if (bridgeBuilderId >= 0 && bridgeReservationUntil > now
            && now - bridgeLastProgressTimestamp <= 8.0f)
            return bridgeBuilderId == builderId && bridgeRouteSignature == signature;
        bridgeBuilderId = builderId;
        bridgeRouteSignature = signature;
        bridgeReservationUntil = now + 8.0f;
        bridgeLastProgressTimestamp = now;
        bridgeLastProgressPosition = position;
        bridgeLastProgressBlocks = blocks;
        return true;
    }

    bool IsRouteOpened(std::uint64_t signature, float matchTime, float maxAge = 600.0f) const
    {
        if (signature == 0) return false;
        for (int i = 0; i < MaxOpenedRoutes; ++i)
        {
            if (openedRouteSignatures[static_cast<std::size_t>(i)] == signature
                && matchTime - routeOpenedTimestamps[static_cast<std::size_t>(i)] < maxAge)
            {
                return true;
            }
        }
        return false;
    }

    void MarkRouteOpened(std::uint64_t signature, float matchTime)
    {
        if (signature == 0) return;
        for (int i = 0; i < MaxOpenedRoutes; ++i)
        {
            if (openedRouteSignatures[static_cast<std::size_t>(i)] == signature)
            {
                routeOpenedTimestamps[static_cast<std::size_t>(i)] = matchTime;
                return;
            }
        }
        const std::size_t slot = static_cast<std::size_t>(openedRouteWriteIndex);
        openedRouteSignatures[slot] = signature;
        routeOpenedTimestamps[slot] = matchTime;
        openedRouteWriteIndex = (openedRouteWriteIndex + 1) % MaxOpenedRoutes;
    }

    void Broadcast(int playerId, CoordinationSignal signal, Vector3 target, float matchTime, int targetTeamId = -1)
    {
        if (signal == CoordinationSignal::AttackingCore || signal == CoordinationSignal::BuildingBridge)
        {
            pressureTeamId = targetTeamId;
            assistActorId = playerId;
            sharedAttackRoute = target;
            pressureTimestamp = matchTime;
        }
        else if (signal == CoordinationSignal::CallingForHelp)
        {
            defenseRequestTimestamp = matchTime;
        }
        else if (signal == CoordinationSignal::DefendingCore
            && (reserveDefenderId < 0 || playerId < reserveDefenderId))
        {
            reserveDefenderId = playerId;
        }
        for (int i = 0; i < count; ++i)
        {
            if (entries[i].playerId == playerId)
            {
                entries[i] = TeamCoordinationEntry { playerId, signal, target, matchTime, targetTeamId };
                return;
            }
        }

        if (count < MaxEntries)
        {
            entries[count++] = TeamCoordinationEntry { playerId, signal, target, matchTime, targetTeamId };
        }
    }

    void Prune(float matchTime, float maxAge)
    {
        int write = 0;
        reserveDefenderId = -1;
        for (int read = 0; read < count; ++read)
        {
            if (entries[read].signal == CoordinationSignal::None
                || matchTime - entries[read].timestamp > maxAge)
            {
                continue;
            }
            entries[write++] = entries[read];
            if (entries[read].signal == CoordinationSignal::DefendingCore
                && (reserveDefenderId < 0 || entries[read].playerId < reserveDefenderId))
            {
                reserveDefenderId = entries[read].playerId;
            }
        }
        for (int i = write; i < count; ++i)
        {
            entries[i] = TeamCoordinationEntry {};
        }
        count = write;
        if (matchTime - pressureTimestamp > maxAge * 2.0f)
        {
            pressureTeamId = -1;
            assistActorId = -1;
        }
        if (matchTime - defenseRequestTimestamp > maxAge * 2.0f)
        {
            defenseRequestTimestamp = -1000.0f;
        }
        if (matchTime - bridgeRequestTimestamp > 7.0f)
        {
            bridgeRequestorId = -1;
            bridgeRequestBuilderId = -1;
            bridgeRequestIntent = BotIntent::SecureResources;
            bridgeRequestTarget = {};
            bridgeRequestTargetTeamId = -1;
            bridgeRequestTimestamp = -1000.0f;
            bridgeRequestReservationUntil = -1000.0f;
        }
    }

    int CountSignal(
        CoordinationSignal signal,
        float matchTime,
        float maxAge,
        int targetTeamId = -999,
        int excludePlayerId = -1) const
    {
        int result = 0;
        for (int i = 0; i < count; ++i)
        {
            const TeamCoordinationEntry& entry = entries[i];
            if (entry.playerId == excludePlayerId
                || entry.signal != signal
                || matchTime - entry.timestamp > maxAge)
            {
                continue;
            }
            if (targetTeamId != -999 && entry.targetTeamId != targetTeamId)
            {
                continue;
            }
            ++result;
        }
        return result;
    }
};

struct BotMemory
{
    int playerId = -1;
    BotState state = BotState::Collect;
    BotRole role = BotRole::Rusher;
    BotIntent intent = BotIntent::SecureResources;
    float stateTimer = 0.0f;
    float intentTimer = 0.0f;
    float intentLockTimer = 0.0f;
    float roleTimer = 0.0f;
    float roleLockTimer = 0.0f;
    float intentScore = 0.0f;
    std::string intentReason;
    std::string roleReason;
    BotArchetype archetype = BotArchetype::TeamHelper;
    std::uint32_t personalitySeed = 0;
    float aggressionTrait = 0.5f;
    float cautionTrait = 0.5f;
    float economyTrait = 0.5f;
    float teamworkTrait = 0.5f;
    float creativityTrait = 0.5f;
    float planPatienceTrait = 0.5f;
    GridPos breakTarget {};
    bool hasBreakTarget = false;
    float breakProgress = 0.0f;
    Vector3 lastSeenEnemyPosition {};
    bool hasLastSeenEnemy = false;
    // Freshness of lastSeenEnemyPosition (seconds since last actually seen via
    // LOS); decays each tick, clears hasLastSeenEnemy at 0. Gates the firing
    // perch so a bot only towers up to peek at an enemy it genuinely saw
    // recently duck behind cover — no wallhack.
    float lastSeenEnemyTimer = 0.0f;
    // Firing-perch (creative building): blocks stacked so far in the current
    // tower-up, and a per-placement rate limiter.
    int perchBlocksPlaced = 0;
    float perchCooldown = 0.0f;
    Vector3 bridgeTarget {};
    bool hasBridgeTarget = false;
    Vector3 navTarget {};
    Vector3 navWaypoint {};
    bool hasNavWaypoint = false;
    float navTimer = 0.0f;
    // Semantic destination selected by the decision layer, before A* turns it
    // into short local waypoints. Automatch uses this to distinguish genuine
    // progress toward an objective from movement around the same obstacle.
    Vector3 objectiveTarget {};
    bool hasObjectiveTarget = false;
    Vector3 authoredRouteObjective {};
    bool hasAuthoredRouteObjective = false;
    bool usingAuthoredRoute = false;
    int authoredRouteIndex = 0;
    int authoredRouteMarkerCount = 0;
    int authoredRouteMarkerKind = -1;
    int authoredRouteAdvances = 0;
    Vector3 authoredRouteLastAdvancePosition {};
    bool hasAuthoredRouteLastAdvancePosition = false;
    // Stable coarse portal sequence selected from the map RouteGraph. Local
    // action paths may be exhausted/rebuilt without discarding this corridor.
    std::vector<int> routeCorridorNodes;
    std::vector<int> routeCorridorTraversal;
    std::vector<int> routeCorridorBridgeBlocks;
    int routeCorridorIndex = 0;
    Vector3 routeCorridorObjective {};
    bool hasRouteCorridorObjective = false;
    bool usingRouteCorridor = false;
    // A bounded entry search may find a safe approach without reaching the
    // strategic graph. Navigation retains movement ownership during recovery.
    bool routeEntryPending = false;
    bool routeEntryRepair = false;
    Vector3 routeEntryTarget {};
    Vector3 routeEntryObjective {};
    std::uint64_t routeCorridorSignature = 0;
    float routeCorridorReplanCooldown = 0.0f;
    int routeCorridorAdvances = 0;
    Vector3 routeCorridorLastAdvancePosition {};
    bool hasRouteCorridorLastAdvancePosition = false;
    bool routeCorridorBridgeSegment = false;
    int routeCorridorExpectedBridgeBlocks = 0;
    bool routeCorridorBridgeStarted = false;
    std::uint64_t routeCorridorActiveBridgeSignature = 0;
    bool routeCorridorWaitingForBuilder = false;
    float routeCorridorLastProgressTimestamp = -1000.0f;
    Vector3 routeCorridorProgressPosition {};
    int routeCorridorProgressIndex = -1;
    float routeCorridorProgressDistanceSq = 0.0f;
    // Hard floor between full path searches. The waypoint cache alone is not
    // enough: a bot standing NEAR its waypoint (arrived / fighting / mining)
    // bypassed it and re-ran the full A* every tick — 60 searches per second
    // per bot, the main interactive frame-rate killer.
    float pathReplanCooldown = 0.0f;
    int lastAttackedCoreTeamId = -1;
    int carriedResourceValue = 0;
    float retreatTimer = 0.0f;
    float attackTimer = 0.0f;
    float stuckTimer = 0.0f;
    float strafeTimer = 0.0f;
    float bridgePlaceCooldown = 0.0f;
    float utilityTimer = 0.0f;
    float jumpTimer = 0.0f;
    int strafeSign = 1;
    int bridgeBlocksPlaced = 0;
    float defenseCheckTimer = 0.0f;
    float strategicUpdateTimer = 0.0f;
    StrategicPlan currentPlan {};
    float abandonedPlanCooldown = 0.0f;
    int abandonedPlanTargetTeamId = -1;
    int planStarts = 0;
    int planStageAdvances = 0;
    int planCompletions = 0;
    int planCancellations = 0;
    int planExpiryCancellations = 0;
    int planEvidenceCancellations = 0;
    int planRouteFailureCancellations = 0;
    int planVoidCancellations = 0;
    float totalPlanHoldSeconds = 0.0f;
    std::string lastPlanOutcome;
    int assaultProgressTargetTeamId = -1;
    float assaultNoProgressTimer = 0.0f;
    float assaultLastDistance = 0.0f;
    int assaultLastCorridorAdvances = 0;
    int assaultLastCoreHealth = -1;
    int assaultLastBlocks = -1;
    int assaultLastResourceValue = -1;
    bool hasAssaultProgressSample = false;
    bool cleanupBridgeKitReady = false;
    bool coreDefenseCritical = false;
    int missingDefenseBlocks = 0;
    Vector3 lastPosition {};
    int chaseTargetId = -1;
    int fightTargetHealth = -1;
    float fightStallTimer = 0.0f;
    float chaseStuckTimer = 0.0f;
    float chaseBanTimer = 0.0f;
    float chaseLastDistance = -1.0f;
    float heroAbilityTimer = 0.0f;
    float repairPlaceCooldown = 0.0f;
    float reactionDelayTimer = 0.0f;
    // Perception is a belief, not a live pointer.  A sighting becomes less
    // trustworthy over time and never follows an unseen target through walls.
    int rememberedEnemyId = -1;
    Vector3 rememberedEnemyVelocity {};
    float enemyMemoryConfidence = 0.0f;
    float perceptionAcquireTimer = 0.0f;
    int pendingPerceptionEnemyId = -1;
    float resourcePlanTimer = 0.0f;
    Vector3 cachedResourceTarget {};
    int cachedResourceType = 0;
    bool hasCachedResourceTarget = false;
    float tacticalCheckTimer = 0.0f;
    int tacticalEnemyCoreTeamId = -1;
    bool cachedCanBreakDefense = false;
    bool cachedCoreCanUpgrade = false;
    // Stable route failure memory.  Repeated deaths near the same point make
    // the bot conserve blocks and avoid another immediate greedy push.
    Vector3 lastRouteFailurePosition {};
    bool hasLastRouteFailure = false;
    int repeatedRouteFailures = 0;
    float routeFailureCooldown = 0.0f;
    float bridgeHelpRequestCooldown = 0.0f;
    bool waitingForBridgeHelp = false;
    bool assignedBridgeAssist = false;
    float pendingVoidEscapeTimer = 0.0f;
    Vector3 pendingVoidEscapePosition {};
    int lastDeathCarriedValue = 0;
    int lastKillerId = -1;
    int recentCoreAttackerId = -1;
    Vector3 recentCoreAttackOrigin {};
    float recentCoreAttackTimer = 0.0f;
};

const char* ToString(BotState state);
const char* ToString(BotRole role);
const char* ToString(BotArchetype archetype);
const char* ToString(BotIntent intent);
const char* ToString(CoordinationSignal signal);
const char* ToString(StrategicGoal goal);

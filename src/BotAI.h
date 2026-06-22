#pragma once

#include "Block.h"
#include "raylib.h"

#include <array>
#include <cstdint>
#include <string>

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

    std::array<TeamCoordinationEntry, MaxEntries> entries {};
    int count = 0;

    void Clear()
    {
        entries = {};
        count = 0;
    }

    void Broadcast(int playerId, CoordinationSignal signal, Vector3 target, float matchTime, int targetTeamId = -1)
    {
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
        for (int read = 0; read < count; ++read)
        {
            if (entries[read].signal == CoordinationSignal::None
                || matchTime - entries[read].timestamp > maxAge)
            {
                continue;
            }
            entries[write++] = entries[read];
        }
        for (int i = write; i < count; ++i)
        {
            entries[i] = TeamCoordinationEntry {};
        }
        count = write;
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
    GridPos breakTarget {};
    bool hasBreakTarget = false;
    float breakProgress = 0.0f;
    Vector3 lastSeenEnemyPosition {};
    bool hasLastSeenEnemy = false;
    Vector3 bridgeTarget {};
    bool hasBridgeTarget = false;
    Vector3 navTarget {};
    Vector3 navWaypoint {};
    bool hasNavWaypoint = false;
    float navTimer = 0.0f;
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
    float resourcePlanTimer = 0.0f;
    Vector3 cachedResourceTarget {};
    int cachedResourceType = 0;
    bool hasCachedResourceTarget = false;
    float tacticalCheckTimer = 0.0f;
    int tacticalEnemyCoreTeamId = -1;
    bool cachedCanBreakDefense = false;
    bool cachedCoreCanUpgrade = false;
};

const char* ToString(BotState state);
const char* ToString(BotRole role);
const char* ToString(BotIntent intent);
const char* ToString(CoordinationSignal signal);
const char* ToString(StrategicGoal goal);

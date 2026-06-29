#include "Game.h"
#include "VecConvert.h"

#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
constexpr int kBotPathMinY = -2;
constexpr int kBotPathMaxY = 64;
constexpr float kBotPi = 3.1415926535f;
constexpr float kCoordinationSignalTtl = 2.25f;

class ScopedProfileTimer
{
public:
    ScopedProfileTimer(bool enabled, double& elapsedMs, unsigned long long& calls)
        : enabled_(enabled), elapsedMs_(elapsedMs), calls_(calls)
    {
        if (enabled_)
        {
            started_ = std::chrono::steady_clock::now();
        }
    }

    ~ScopedProfileTimer()
    {
        Stop();
    }

    void Stop()
    {
        if (enabled_)
        {
            elapsedMs_ += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started_).count();
            ++calls_;
            enabled_ = false;
        }
    }

private:
    bool enabled_ = false;
    double& elapsedMs_;
    unsigned long long& calls_;
    std::chrono::steady_clock::time_point started_ {};
};

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float Length2D(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.z * value.z);
}

Vector3 Normalize2D(Vector3 value)
{
    const float length = Length2D(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, 0.0f, value.z / length };
}

bool HasSupportBelow(const World& world, Vector3 position, int maxDropBlocks)
{
    const GridPos underCenter = world.WorldToGrid(Vector3 { position.x, position.y - 1.08f, position.z });
    for (int drop = 0; drop <= maxDropBlocks; ++drop)
    {
        if (!world.IsAir(GridPos { underCenter.x, underCenter.y - drop, underCenter.z }))
        {
            return true;
        }
    }
    return false;
}

GridPos FindSupportBelow(const World& world, Vector3 position, int maxDropBlocks)
{
    const GridPos underCenter = world.WorldToGrid(Vector3 { position.x, position.y - 1.08f, position.z });
    for (int drop = 0; drop <= maxDropBlocks; ++drop)
    {
        const GridPos candidate { underCenter.x, underCenter.y - drop, underCenter.z };
        if (!world.IsAir(candidate))
        {
            return candidate;
        }
    }
    return underCenter;
}

float Length(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vector3 StepTargetToward(Vector3 from, Vector3 target, float maxStep)
{
    const Vector3 delta { target.x - from.x, 0.0f, target.z - from.z };
    const float distance = Length2D(delta);
    if (distance <= maxStep || distance <= 0.0001f)
    {
        return target;
    }

    const Vector3 direction = Normalize2D(delta);
    return Vector3 {
        from.x + direction.x * maxStep,
        target.y,
        from.z + direction.z * maxStep
    };
}

float YawFromDirection(Vector3 direction)
{
    return std::atan2(direction.x, -direction.z);
}

int DefenseBlockRank(BlockType type)
{
    switch (type)
    {
    case BlockType::ObsidianBlock:
        return 5;
    case BlockType::StoneBlock:
        return 4;
    case BlockType::EnergyGlassBlock:
        return 3;
    case BlockType::WoodBlock:
        return 2;
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        return 1;
    default:
        break;
    }
    return 0;
}

std::optional<BlockType> BestDefenseBlockAvailable(const Inventory& inventory)
{
    const BlockType priority[] {
        BlockType::ObsidianBlock,
        BlockType::StoneBlock,
        BlockType::EnergyGlassBlock,
        BlockType::WoodBlock,
        BlockType::WoolBlock
    };
    for (BlockType type : priority)
    {
        if (inventory.GetBlockCount(type) > 0)
        {
            return type;
        }
    }
    return std::nullopt;
}

long long PathKey(int x, int y, int z)
{
    return (static_cast<long long>(x + 2048) << 32)
        ^ (static_cast<long long>(z + 2048) << 8)
        ^ static_cast<unsigned int>(y + 32);
}

struct BotPathNode
{
    GridPos pos {};
    float g = 0.0f;
    float f = 0.0f;
};

std::vector<GridPos> CoreDefensePositions(GridPos corePos)
{
    std::vector<GridPos> positions {
        GridPos { corePos.x + 1, corePos.y, corePos.z },
        GridPos { corePos.x - 1, corePos.y, corePos.z },
        GridPos { corePos.x, corePos.y, corePos.z + 1 },
        GridPos { corePos.x, corePos.y, corePos.z - 1 },
        GridPos { corePos.x, corePos.y + 1, corePos.z },
        GridPos { corePos.x + 1, corePos.y + 1, corePos.z },
        GridPos { corePos.x - 1, corePos.y + 1, corePos.z },
        GridPos { corePos.x, corePos.y + 1, corePos.z + 1 },
        GridPos { corePos.x, corePos.y + 1, corePos.z - 1 },
        GridPos { corePos.x + 2, corePos.y, corePos.z },
        GridPos { corePos.x - 2, corePos.y, corePos.z },
        GridPos { corePos.x, corePos.y, corePos.z + 2 },
        GridPos { corePos.x, corePos.y, corePos.z - 2 },
        GridPos { corePos.x + 1, corePos.y, corePos.z + 1 },
        GridPos { corePos.x + 1, corePos.y, corePos.z - 1 },
        GridPos { corePos.x - 1, corePos.y, corePos.z + 1 },
        GridPos { corePos.x - 1, corePos.y, corePos.z - 1 },
        // A paced outer firing wall and compact roof turn repairs into a
        // readable fort without sealing the shop or primary approaches.
        GridPos { corePos.x + 2, corePos.y + 1, corePos.z },
        GridPos { corePos.x - 2, corePos.y + 1, corePos.z },
        GridPos { corePos.x, corePos.y + 1, corePos.z + 2 },
        GridPos { corePos.x, corePos.y + 1, corePos.z - 2 },
        GridPos { corePos.x + 1, corePos.y + 2, corePos.z },
        GridPos { corePos.x - 1, corePos.y + 2, corePos.z },
        GridPos { corePos.x, corePos.y + 2, corePos.z + 1 },
        GridPos { corePos.x, corePos.y + 2, corePos.z - 1 },
        GridPos { corePos.x, corePos.y + 2, corePos.z }
    };
    return positions;
}

struct CoreDefenseStatus
{
    int missingBlocks = 0;
    int weakBlocks = 0;
    bool critical = false;
};

CoreDefenseStatus InspectCoreDefense(const World& world, const Team& team)
{
    CoreDefenseStatus status {};
    for (const GridPos& candidate : CoreDefensePositions(team.coreBlock))
    {
        const Block* block = world.GetBlock(candidate);
        if (block == nullptr || world.IsAir(candidate) || block->teamId != team.id)
        {
            ++status.missingBlocks;
            continue;
        }

        if (DefenseBlockRank(block->type) <= 1)
        {
            ++status.weakBlocks;
        }
    }

    status.critical = status.missingBlocks >= 3
        || (status.missingBlocks >= 1 && status.weakBlocks >= 4)
        || status.weakBlocks >= 8;
    return status;
}

int CarriedResourceValue(const Inventory& inventory)
{
    return inventory.GetResource(ResourceType::Iron)
        + inventory.GetResource(ResourceType::Gold) * 4
        + inventory.GetResource(ResourceType::Crystal) * 9;
}

float BotAttackDelay(BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return 0.48f;
    case BotDifficulty::Hard:
        return 0.20f;
    case BotDifficulty::Normal:
        break;
    }
    return 0.32f;
}

float BotFightReactionDelay(BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return 0.48f;
    case BotDifficulty::Hard:
        return 0.18f;
    case BotDifficulty::Normal:
        break;
    }
    return 0.32f;
}

float BotBridgeCooldown(BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return 0.34f;
    case BotDifficulty::Hard:
        return 0.16f;
    case BotDifficulty::Normal:
        break;
    }
    return 0.22f;
}

// Human-like reaction time after taking a hit: until it expires the bot cannot
// place a save block, so knockback displaces bots instead of being negated by a
// same-frame block under the feet.
float BotHitReactionSeconds(BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return 0.60f;
    case BotDifficulty::Hard:
        return 0.22f;
    case BotDifficulty::Normal:
        break;
    }
    return 0.38f;
}

BotRole RoleForBotId(int id)
{
    return id % 4 == 0
        ? BotRole::Defender
        : (id % 4 == 1 ? BotRole::Fighter : (id % 4 == 2 ? BotRole::Rusher : BotRole::Collector));
}

int PreferredNeighborTeam(int teamId, bool clockwise)
{
    if (clockwise)
    {
        switch (teamId)
        {
        case 0: return 2;
        case 2: return 1;
        case 1: return 3;
        case 3: return 0;
        default: return -1;
        }
    }
    switch (teamId)
    {
    case 0: return 3;
    case 3: return 1;
    case 1: return 2;
    case 2: return 0;
    default: return -1;
    }
}

const BotRoleTuning& RoleTuning(const BotTuningGenome& tuning, BotRole role)
{
    return tuning.roles[std::clamp(static_cast<int>(role), 0, 3)];
}

float DifficultyCautionOffset(BotDifficulty difficulty, float easyOffset, float hardOffset)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return easyOffset;
    case BotDifficulty::Hard:
        return hardOffset;
    case BotDifficulty::Normal:
        break;
    }
    return 0.0f;
}

float FightRequiredMarginForDifficulty(const BotTuningGenome& tuning, BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return tuning.fightRequiredMarginEasy;
    case BotDifficulty::Hard:
        return tuning.fightRequiredMarginHard;
    case BotDifficulty::Normal:
        break;
    }
    return tuning.fightRequiredMarginNormal;
}

float RetreatPowerMarginForDifficulty(const BotTuningGenome& tuning, BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return tuning.retreatPowerMarginEasy;
    case BotDifficulty::Hard:
        return tuning.retreatPowerMarginHard;
    case BotDifficulty::Normal:
        break;
    }
    return tuning.retreatPowerMarginNormal;
}

int BotRetreatHealth(BotRole role, BotDifficulty difficulty, bool coreAlive, const BotTuningGenome& tuning)
{
    const BotRoleTuning& roleTuning = RoleTuning(tuning, role);
    if (!coreAlive)
    {
        return static_cast<int>(std::round(roleTuning.retreatHealthFinalLife + DifficultyCautionOffset(difficulty, 10.0f, -8.0f)));
    }

    return static_cast<int>(std::round(roleTuning.retreatHealthCoreAlive + DifficultyCautionOffset(difficulty, 14.0f, -12.0f)));
}

int BotFightHealth(BotRole role, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    const BotRoleTuning& roleTuning = RoleTuning(tuning, role);
    return static_cast<int>(std::round(roleTuning.fightHealth + DifficultyCautionOffset(difficulty, 18.0f, -10.0f)));
}

float BotEngageRange(BotRole role, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    const float difficultyBonus = difficulty == BotDifficulty::Hard ? 1.15f : (difficulty == BotDifficulty::Easy ? -0.85f : 0.0f);
    const float roleScale = role == BotRole::Rusher ? 0.6f : (role == BotRole::Collector ? 0.45f : 1.0f);
    return RoleTuning(tuning, role).engageRange + difficultyBonus * roleScale;
}

int BotDesiredBlocks(BotRole role, const BotTuningGenome& tuning)
{
    return static_cast<int>(std::round(RoleTuning(tuning, role).desiredBlocks));
}

int BotLootReturnValue(BotRole role, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    int value = static_cast<int>(std::round(RoleTuning(tuning, role).lootReturnValue));
    if (difficulty == BotDifficulty::Easy)
    {
        value -= 4;
    }
    else if (difficulty == BotDifficulty::Hard)
    {
        value += 8;
    }
    return value;
}

float IntentLockSeconds(BotIntent intent, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    const float difficultyScale = difficulty == BotDifficulty::Hard ? 0.78f : (difficulty == BotDifficulty::Easy ? 1.22f : 1.0f);
    const float tuningScale = tuning.intentLockScale;
    switch (intent)
    {
    case BotIntent::DefendCore:
        return 0.35f * tuningScale;
    case BotIntent::FightEnemy:
    case BotIntent::ChaseWeakEnemy:
        return 0.55f * difficultyScale * tuningScale;
    case BotIntent::RetreatHome:
        return 1.10f * difficultyScale * tuningScale;
    case BotIntent::GearUp:
        return 1.45f * difficultyScale * tuningScale;
    case BotIntent::PressureCore:
    case BotIntent::BreakCoreDefense:
        return 1.05f * difficultyScale * tuningScale;
    case BotIntent::SecureResources:
        return 1.20f * difficultyScale * tuningScale;
    case BotIntent::RepairCoreDefense:
        return 0.90f * difficultyScale * tuningScale;
    case BotIntent::Recover:
        return 0.32f;
    }
    return 0.8f * tuningScale;
}

BotState StateForIntent(BotIntent intent)
{
    switch (intent)
    {
    case BotIntent::DefendCore:
    case BotIntent::FightEnemy:
    case BotIntent::ChaseWeakEnemy:
        return BotState::Fight;
    case BotIntent::GearUp:
        return BotState::Shop;
    case BotIntent::SecureResources:
        return BotState::Collect;
    case BotIntent::PressureCore:
        return BotState::AttackCore;
    case BotIntent::BreakCoreDefense:
        return BotState::BreakDefense;
    case BotIntent::RetreatHome:
    case BotIntent::RepairCoreDefense:
        return BotState::Retreat;
    case BotIntent::Recover:
        return BotState::Bridge;
    }
    return BotState::Bridge;
}

struct BotDecision
{
    BotIntent intent = BotIntent::SecureResources;
    BotState state = BotState::Collect;
    Vector3 target {};
    Player* fightTarget = nullptr;
    float score = -100000.0f;
    const char* reason = "";
};

struct BotTeamSnapshot
{
    int aliveAllies = 0;
    int alliesNearCore = 0;
    int defendersNearCore = 0;
    int activePressure = 0;
    int activeShop = 0;
    int activeRepair = 0;
    int activeResource = 0;
};

struct BotRoleDistribution
{
    int defenders = 0;
    int rushers = 0;
    int collectors = 0;
    int fighters = 0;
    int total = 0;
};

struct BotResourcePlan
{
    Vector3 target {};
    ResourceType type = ResourceType::Iron;
    bool hasTarget = false;
    float score = std::numeric_limits<float>::max();
};

enum class BotStrategicFocus
{
    Economy,
    Defense,
    Pressure,
    Cleanup
};

struct BotStrategicPlan
{
    BotStrategicFocus focus = BotStrategicFocus::Economy;
    int attackCoreTeamId = -1;
    int desiredAttackers = 1;
    int desiredDefenders = 1;
    float attackUrgency = 0.0f;
    float defenseUrgency = 0.0f;
    bool allIn = false;
};

const BotMemory* FindBotMemoryByPlayerId(
    const std::vector<BotMemory>& memories,
    const std::unordered_map<int, std::size_t>& memoryIndex,
    int playerId)
{
    const auto foundIndex = memoryIndex.find(playerId);
    if (foundIndex != memoryIndex.end())
    {
        const std::size_t index = foundIndex->second;
        if (index < memories.size() && memories[index].playerId == playerId)
        {
            return &memories[index];
        }
    }

    for (const BotMemory& memory : memories)
    {
        if (memory.playerId == playerId)
        {
            return &memory;
        }
    }
    return nullptr;
}

BotRole ResolveRoleForPlayerId(
    int playerId,
    const std::vector<BotMemory>& memories,
    const std::unordered_map<int, std::size_t>& memoryIndex)
{
    if (const BotMemory* memory = FindBotMemoryByPlayerId(memories, memoryIndex, playerId))
    {
        return memory->role;
    }
    return RoleForBotId(playerId);
}

BotRoleDistribution BuildRoleDistributionForTeam(
    int teamId,
    const std::vector<Player>& players,
    const std::vector<BotMemory>& memories,
    const std::unordered_map<int, std::size_t>& memoryIndex,
    int ignorePlayerId = -1)
{
    BotRoleDistribution distribution {};
    for (const Player& player : players)
    {
        if (player.GetId() == ignorePlayerId
            || player.GetTeamId() != teamId
            || !IsBotControlled(player.GetControlKind())
            || !player.IsAlive()
            || player.IsEliminated())
        {
            continue;
        }

        ++distribution.total;
        switch (ResolveRoleForPlayerId(player.GetId(), memories, memoryIndex))
        {
        case BotRole::Defender:
            ++distribution.defenders;
            break;
        case BotRole::Rusher:
            ++distribution.rushers;
            break;
        case BotRole::Collector:
            ++distribution.collectors;
            break;
        case BotRole::Fighter:
            ++distribution.fighters;
            break;
        }
    }
    return distribution;
}

void RemoveRoleFromDistribution(BotRoleDistribution& distribution, BotRole role)
{
    distribution.total = std::max(0, distribution.total - 1);
    switch (role)
    {
    case BotRole::Defender:
        distribution.defenders = std::max(0, distribution.defenders - 1);
        break;
    case BotRole::Rusher:
        distribution.rushers = std::max(0, distribution.rushers - 1);
        break;
    case BotRole::Collector:
        distribution.collectors = std::max(0, distribution.collectors - 1);
        break;
    case BotRole::Fighter:
        distribution.fighters = std::max(0, distribution.fighters - 1);
        break;
    }
}

void RemoveIntentFromSnapshot(BotTeamSnapshot& snapshot, BotIntent intent)
{
    switch (intent)
    {
    case BotIntent::PressureCore:
    case BotIntent::BreakCoreDefense:
    case BotIntent::ChaseWeakEnemy:
        snapshot.activePressure = std::max(0, snapshot.activePressure - 1);
        break;
    case BotIntent::GearUp:
        snapshot.activeShop = std::max(0, snapshot.activeShop - 1);
        break;
    case BotIntent::RepairCoreDefense:
        snapshot.activeRepair = std::max(0, snapshot.activeRepair - 1);
        break;
    case BotIntent::SecureResources:
        snapshot.activeResource = std::max(0, snapshot.activeResource - 1);
        break;
    case BotIntent::DefendCore:
    case BotIntent::FightEnemy:
    case BotIntent::RetreatHome:
    case BotIntent::Recover:
        break;
    }
}

float RoleLockSeconds(BotRole role, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    const float scale = difficulty == BotDifficulty::Hard ? 0.82f : (difficulty == BotDifficulty::Easy ? 1.22f : 1.0f);
    return RoleTuning(tuning, role).roleLockSeconds * scale * tuning.roleLockScale;
}

float RoleIntentBias(BotRole role, BotIntent intent, const BotTuningGenome& tuning)
{
    float bias = 0.0f;
    switch (role)
    {
    case BotRole::Defender:
        switch (intent)
        {
        case BotIntent::DefendCore:
            bias = 210.0f;
            break;
        case BotIntent::RepairCoreDefense:
            bias = 260.0f;
            break;
        case BotIntent::FightEnemy:
            bias = 70.0f;
            break;
        case BotIntent::PressureCore:
        case BotIntent::BreakCoreDefense:
            bias = -180.0f;
            break;
        case BotIntent::SecureResources:
            bias = -35.0f;
            break;
        case BotIntent::GearUp:
        case BotIntent::ChaseWeakEnemy:
        case BotIntent::RetreatHome:
        case BotIntent::Recover:
            break;
        }
        break;
    case BotRole::Rusher:
        switch (intent)
        {
        case BotIntent::PressureCore:
            bias = 230.0f;
            break;
        case BotIntent::BreakCoreDefense:
            bias = 210.0f;
            break;
        case BotIntent::FightEnemy:
            bias = 80.0f;
            break;
        case BotIntent::DefendCore:
            bias = -95.0f;
            break;
        case BotIntent::RepairCoreDefense:
            bias = -170.0f;
            break;
        case BotIntent::SecureResources:
            bias = -135.0f;
            break;
        case BotIntent::GearUp:
        case BotIntent::ChaseWeakEnemy:
        case BotIntent::RetreatHome:
        case BotIntent::Recover:
            break;
        }
        break;
    case BotRole::Collector:
        switch (intent)
        {
        case BotIntent::SecureResources:
            bias = 260.0f;
            break;
        case BotIntent::GearUp:
            bias = 150.0f;
            break;
        case BotIntent::RetreatHome:
            bias = 90.0f;
            break;
        case BotIntent::PressureCore:
        case BotIntent::BreakCoreDefense:
            bias = -210.0f;
            break;
        case BotIntent::ChaseWeakEnemy:
            bias = -70.0f;
            break;
        case BotIntent::DefendCore:
            bias = -60.0f;
            break;
        case BotIntent::RepairCoreDefense:
            bias = -45.0f;
            break;
        case BotIntent::FightEnemy:
        case BotIntent::Recover:
            break;
        }
        break;
    case BotRole::Fighter:
        switch (intent)
        {
        case BotIntent::FightEnemy:
            bias = 230.0f;
            break;
        case BotIntent::ChaseWeakEnemy:
            bias = 210.0f;
            break;
        case BotIntent::PressureCore:
            bias = 95.0f;
            break;
        case BotIntent::BreakCoreDefense:
            bias = 75.0f;
            break;
        case BotIntent::SecureResources:
            bias = -110.0f;
            break;
        case BotIntent::DefendCore:
            bias = -35.0f;
            break;
        case BotIntent::RepairCoreDefense:
            bias = -130.0f;
            break;
        case BotIntent::GearUp:
        case BotIntent::RetreatHome:
        case BotIntent::Recover:
            break;
        }
        break;
    }

    const BotRoleTuning& roleTuning = RoleTuning(tuning, role);
    switch (intent)
    {
    case BotIntent::PressureCore:
    case BotIntent::BreakCoreDefense:
        return bias * roleTuning.pressureBiasScale;
    case BotIntent::DefendCore:
    case BotIntent::RepairCoreDefense:
        return bias * roleTuning.defenseBiasScale;
    case BotIntent::SecureResources:
    case BotIntent::GearUp:
    case BotIntent::RetreatHome:
        return bias * roleTuning.resourceBiasScale;
    case BotIntent::FightEnemy:
    case BotIntent::ChaseWeakEnemy:
        return bias * roleTuning.combatBiasScale;
    case BotIntent::Recover:
        break;
    }
    return bias;
}

struct BotRoleDecision
{
    BotRole role = BotRole::Rusher;
    bool force = false;
    const char* reason = "";
};

BotRoleDecision EvaluateDynamicRoleDecision(
    const Player& bot,
    const Team& team,
    const BotMemory& memory,
    const BotRoleDistribution& distribution,
    bool carryingLoot,
    bool wantsShop,
    Player* enemyAtCore,
    EnergyCore* enemyCore,
    float matchTime,
    const Inventory& inventory)
{
    BotRoleDecision decision {};
    decision.role = memory.role;
    decision.reason = "keep role";

    const bool emergencyDefense = team.coreAlive && enemyAtCore != nullptr;
    const bool lowHealth = bot.GetHealth() < 44;
    const bool lowBlocks = inventory.GetBlocks() < 8;
    const bool lowCombat = inventory.GetSwordLevel() <= 0 && inventory.GetToolLevel() <= 0;
    const bool nearEnemyCore = enemyCore != nullptr
        && enemyCore->IsAlive()
        && DistanceSquared(bot.GetPosition(), Vector3 {
            static_cast<float>(enemyCore->GetBlockPosition().x),
            1.5f,
            static_cast<float>(enemyCore->GetBlockPosition().z) }) < 170.0f;
    const int teamSize = std::max(1, distribution.total + 1);
    const int desiredDefenders = team.coreAlive ? std::min(teamSize, emergencyDefense ? 2 : 1) : 0;
    int desiredCollectors = !team.coreAlive
        ? std::max(1, teamSize / 3)
        : (matchTime < 100.0f ? std::max(1, teamSize / 3) : std::max(1, teamSize / 4));
    int desiredRushers = !team.coreAlive
        ? std::max(1, teamSize / 2)
        : (matchTime < 85.0f ? std::max(1, teamSize / 4) : std::max(1, teamSize / 3));
    desiredCollectors = std::min(desiredCollectors, std::max(0, teamSize - desiredDefenders));
    desiredRushers = std::min(desiredRushers, std::max(0, teamSize - desiredDefenders - desiredCollectors));
    const int desiredFighters = std::max(0, teamSize - desiredDefenders - desiredCollectors - desiredRushers);
    const auto roleCountWithSelf = [&](BotRole role)
    {
        int count = 0;
        switch (role)
        {
        case BotRole::Defender:
            count = distribution.defenders;
            break;
        case BotRole::Rusher:
            count = distribution.rushers;
            break;
        case BotRole::Collector:
            count = distribution.collectors;
            break;
        case BotRole::Fighter:
            count = distribution.fighters;
            break;
        }
        if (memory.role == role)
        {
            ++count;
        }
        return count;
    };
    const auto roleDeficit = [&](BotRole role)
    {
        int desired = 0;
        switch (role)
        {
        case BotRole::Defender:
            desired = desiredDefenders;
            break;
        case BotRole::Rusher:
            desired = desiredRushers;
            break;
        case BotRole::Collector:
            desired = desiredCollectors;
            break;
        case BotRole::Fighter:
            desired = desiredFighters;
            break;
        }
        return desired - roleCountWithSelf(role);
    };
    // Voluntary role hops are only allowed when the current role is overstaffed,
    // otherwise bots ping-pong between roles chasing each other's deficits.
    const bool ownRoleSurplus = roleDeficit(memory.role) < 0;

    if (!team.coreAlive)
    {
        if (memory.role == BotRole::Defender)
        {
            decision.role = BotRole::Fighter;
            decision.force = true;
            decision.reason = "core destroyed";
            return decision;
        }
        if ((carryingLoot || lowHealth) && distribution.collectors < std::max(1, distribution.total / 3))
        {
            decision.role = BotRole::Collector;
            decision.reason = "recover economy";
            return decision;
        }
        if (nearEnemyCore && distribution.rushers < std::max(1, distribution.total / 2))
        {
            decision.role = BotRole::Rusher;
            decision.reason = "final push";
            return decision;
        }

        decision.role = BotRole::Fighter;
        decision.reason = "final combat";
        return decision;
    }

    if (emergencyDefense
        && roleDeficit(BotRole::Defender) > 0)
    {
        decision.role = BotRole::Defender;
        decision.force = enemyAtCore != nullptr;
        decision.reason = enemyAtCore != nullptr ? "base under attack" : "repair core";
        return decision;
    }

    if (ownRoleSurplus
        && roleDeficit(BotRole::Collector) > 0
        && (carryingLoot || wantsShop || lowBlocks || lowHealth))
    {
        decision.role = BotRole::Collector;
        decision.reason = "gather and gear";
        return decision;
    }

    if (ownRoleSurplus
        && roleDeficit(BotRole::Rusher) > 0
        && (nearEnemyCore || !lowCombat || matchTime > 75.0f))
    {
        decision.role = BotRole::Rusher;
        decision.reason = nearEnemyCore ? "pressure core" : "open lane";
        return decision;
    }

    if (ownRoleSurplus && roleDeficit(BotRole::Fighter) > 0)
    {
        decision.role = BotRole::Fighter;
        decision.reason = "mid control";
        return decision;
    }

    if (ownRoleSurplus && roleDeficit(BotRole::Defender) > 0)
    {
        decision.role = BotRole::Defender;
        decision.reason = "maintain defense";
        return decision;
    }

    return decision;
}

bool TryApplyDynamicRoleDecision(
    BotMemory& memory,
    const BotRoleDecision& decision,
    BotDifficulty difficulty,
    const BotTuningGenome& tuning)
{
    if (decision.role == memory.role)
    {
        return false;
    }
    if (!decision.force && memory.roleLockTimer > 0.0f)
    {
        return false;
    }
    const float minimumRoleTime = difficulty == BotDifficulty::Hard
        ? 6.0f
        : (difficulty == BotDifficulty::Easy ? 9.0f : 7.5f);
    if (!decision.force && memory.roleTimer < minimumRoleTime)
    {
        return false;
    }

    memory.role = decision.role;
    memory.roleTimer = 0.0f;
    memory.roleLockTimer = RoleLockSeconds(decision.role, difficulty, tuning);
    memory.roleReason = decision.reason;
    memory.intentLockTimer = 0.0f;
    memory.hasNavWaypoint = false;
    memory.hasBreakTarget = false;
    memory.breakProgress = 0.0f;
    memory.stuckTimer = 0.0f;
    return true;
}

void TickBotMemory(BotMemory& memory, float dt)
{
    memory.stateTimer += dt;
    memory.intentTimer += dt;
    memory.intentLockTimer = std::max(0.0f, memory.intentLockTimer - dt);
    memory.attackTimer = std::max(0.0f, memory.attackTimer - dt);
    memory.retreatTimer = std::max(0.0f, memory.retreatTimer - dt);
    memory.strafeTimer = std::max(0.0f, memory.strafeTimer - dt);
    memory.bridgePlaceCooldown = std::max(0.0f, memory.bridgePlaceCooldown - dt);
    memory.utilityTimer = std::max(0.0f, memory.utilityTimer - dt);
    memory.jumpTimer = std::max(0.0f, memory.jumpTimer - dt);
    memory.defenseCheckTimer = std::max(0.0f, memory.defenseCheckTimer - dt);
    memory.strategicUpdateTimer = std::max(0.0f, memory.strategicUpdateTimer - dt);
    memory.currentPlan.elapsedTime += dt;
    memory.roleTimer += dt;
    memory.roleLockTimer = std::max(0.0f, memory.roleLockTimer - dt);
    memory.chaseBanTimer = std::max(0.0f, memory.chaseBanTimer - dt);
    memory.heroAbilityTimer = std::max(0.0f, memory.heroAbilityTimer - dt);
    memory.repairPlaceCooldown = std::max(0.0f, memory.repairPlaceCooldown - dt);
    memory.reactionDelayTimer = std::max(0.0f, memory.reactionDelayTimer - dt);
    memory.resourcePlanTimer = std::max(0.0f, memory.resourcePlanTimer - dt);
    memory.tacticalCheckTimer = std::max(0.0f, memory.tacticalCheckTimer - dt);
}

BotTeamSnapshot BuildBotTeamSnapshot(
    const Player& bot,
    Vector3 coreHome,
    const std::vector<Player>& players,
    const std::vector<BotMemory>& memories,
    const std::unordered_map<int, std::size_t>& memoryIndex)
{
    BotTeamSnapshot snapshot {};
    for (const Player& ally : players)
    {
        if (ally.GetId() == bot.GetId()
            || ally.GetTeamId() != bot.GetTeamId()
            || !ally.IsAlive()
            || ally.IsEliminated())
        {
            continue;
        }

        ++snapshot.aliveAllies;
        const bool nearCore = DistanceSquared(ally.GetPosition(), coreHome) < 72.0f;
        if (nearCore)
        {
            ++snapshot.alliesNearCore;
        }

        const BotMemory* allyMemory = FindBotMemoryByPlayerId(memories, memoryIndex, ally.GetId());
        if (allyMemory == nullptr)
        {
            continue;
        }

        if (nearCore && allyMemory->role == BotRole::Defender)
        {
            ++snapshot.defendersNearCore;
        }

        if (allyMemory->intent == BotIntent::PressureCore
            || allyMemory->intent == BotIntent::BreakCoreDefense
            || allyMemory->intent == BotIntent::ChaseWeakEnemy)
        {
            ++snapshot.activePressure;
        }
        else if (allyMemory->intent == BotIntent::GearUp)
        {
            ++snapshot.activeShop;
        }
        else if (allyMemory->intent == BotIntent::RepairCoreDefense)
        {
            ++snapshot.activeRepair;
        }
        else if (allyMemory->intent == BotIntent::SecureResources)
        {
            ++snapshot.activeResource;
        }
    }

    return snapshot;
}

Player* FindEnemyNearCore(Player& bot, std::vector<Player>& players, Vector3 coreHome, float& enemyDistanceSq)
{
    Player* enemyAtCore = nullptr;
    enemyDistanceSq = std::numeric_limits<float>::max();
    for (Player& other : players)
    {
        if (other.GetTeamId() == bot.GetTeamId()
            || other.GetId() == bot.GetId()
            || !other.IsAlive()
            || other.IsEliminated())
        {
            continue;
        }

        const float coreDistance = DistanceSquared(other.GetPosition(), coreHome);
        if (coreDistance < 42.0f && coreDistance < enemyDistanceSq)
        {
            enemyAtCore = &other;
            enemyDistanceSq = coreDistance;
        }
    }

    return enemyAtCore;
}

bool ShouldBotShop(
    const Player& bot,
    const Inventory& inventory,
    const Team& team,
    const BotMemory& memory,
    BotDifficulty difficulty,
    const BotTuningGenome& tuning)
{
    const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, difficulty, tuning);
    const int desiredBlocks = BotDesiredBlocks(memory.role, tuning);
    const bool canBuyBlocks = inventory.GetResource(ResourceType::Iron) >= 8;
    const bool hasObsidianMoney = inventory.GetResource(ResourceType::Gold) >= 8
        && inventory.GetResource(ResourceType::Crystal) >= 3;

    return (inventory.GetBlocks() < desiredBlocks && canBuyBlocks)
        || (memory.role != BotRole::Defender
            && inventory.GetToolLevel() < 2
            && inventory.GetResource(ResourceType::Iron) >= 8
            && inventory.GetResource(ResourceType::Crystal) >= 1)
        || (memory.role == BotRole::Fighter
            && inventory.GetSwordLevel() < 2
            && inventory.GetResource(ResourceType::Gold) >= 6)
        || (memory.role == BotRole::Defender
            && inventory.GetBlockCount(BlockType::StoneBlock) < 16
            && inventory.GetResource(ResourceType::Iron) >= 18)
        || (memory.role == BotRole::Defender
            && inventory.GetBlockCount(BlockType::ObsidianBlock) < 8
            && hasObsidianMoney)
        || (memory.role == BotRole::Collector
            && team.forgeLevel < 3
            && inventory.GetResource(ResourceType::Crystal) >= 4
            && inventory.GetResource(ResourceType::Gold) >= 2)
        || (memory.role == BotRole::Collector
            && (inventory.GetResource(ResourceType::Crystal) >= 3
                || inventory.GetResource(ResourceType::Gold) >= 8
                || memory.carriedResourceValue >= BotLootReturnValue(memory.role, difficulty, tuning)))
        || (bot.GetHealth() < 70 && inventory.GetResource(ResourceType::Crystal) >= 3)
        || carryingLoot;
}

BotResourcePlan BuildBotResourcePlan(
    const Player& bot,
    const ResourcePickup* bestPickup,
    const std::vector<Generator>& generators,
    BotRole role,
    bool ruinsBiome)
{
    BotResourcePlan plan {};
    if (bestPickup != nullptr)
    {
        plan.target = ToVector3(bestPickup->position);
        plan.type = bestPickup->type;
        plan.hasTarget = true;
        plan.score = DistanceSquared(bot.GetPosition(), plan.target);
    }

    for (const Generator& generator : generators)
    {
        if (generator.GetTeamId() != -1)
        {
            continue;
        }

        const ResourceType type = generator.GetType();
        const Vector3 generatorPos = ToVector3(generator.GetPosition());
        float score = DistanceSquared(bot.GetPosition(), generatorPos);
        if (type == ResourceType::Crystal)
        {
            score -= role == BotRole::Collector ? 420.0f : 180.0f;
        }
        else if (type == ResourceType::Gold)
        {
            score -= role == BotRole::Fighter ? 120.0f : 70.0f;
        }
        if (role == BotRole::Collector)
        {
            score -= DistanceSquared(generatorPos, Vector3 { 0.0f, generatorPos.y, 0.0f }) * 0.08f;
        }
        if (ruinsBiome
            && role == BotRole::Collector
            && std::fabs(generatorPos.x) >= 18.0f
            && std::fabs(generatorPos.z) >= 18.0f)
        {
            score -= type == ResourceType::Crystal ? 520.0f : 320.0f;
        }

        if (!plan.hasTarget || score < plan.score)
        {
            plan.target = generatorPos;
            plan.type = type;
            plan.hasTarget = true;
            plan.score = score;
        }
    }

    return plan;
}

const ResourcePickup* FindBestPickupForBot(
    const Player& bot,
    BotRole role,
    const std::vector<ResourcePickup>& pickups,
    const std::vector<Player*>& enemies,
    Vector3 coreHome,
    bool ruinsBiome,
    bool allowHomePickup)
{
    const ResourcePickup* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();
    const Inventory& inventory = bot.GetInventory();

    for (const ResourcePickup& pickup : pickups)
    {
        if (pickup.collected)
        {
            continue;
        }
        const Vector3 pickupPos = ToVector3(pickup.position);
        if (!allowHomePickup && DistanceSquared(pickupPos, coreHome) <= 144.0f)
        {
            continue;
        }

        float score = DistanceSquared(bot.GetPosition(), pickupPos);
        if (pickup.type == ResourceType::Crystal)
        {
            score -= role == BotRole::Collector ? 150.0f : 90.0f;
        }
        else if (pickup.type == ResourceType::Gold)
        {
            score -= role == BotRole::Fighter ? 65.0f : 35.0f;
        }
        else if (inventory.GetBlocks() < 24)
        {
            score -= 30.0f;
        }

        for (const Player* enemy : enemies)
        {
            if (enemy == nullptr)
            {
                continue;
            }

            const float enemyDistance = DistanceSquared(enemy->GetPosition(), pickupPos);
            if (enemyDistance < 18.0f)
            {
                score += role == BotRole::Collector ? 120.0f : 42.0f;
            }
            else if (enemyDistance < 42.0f && role == BotRole::Collector && bot.GetHealth() < 74)
            {
                score += 48.0f;
            }
        }

        if (role == BotRole::Rusher && pickup.type == ResourceType::Iron && inventory.GetBlocks() >= 24)
        {
            score += 80.0f;
        }
        if (role == BotRole::Defender)
        {
            score += DistanceSquared(pickupPos, coreHome) * 0.35f;
        }
        if (ruinsBiome
            && role == BotRole::Collector
            && std::fabs(pickupPos.x) >= 18.0f
            && std::fabs(pickupPos.z) >= 18.0f)
        {
            score -= pickup.type == ResourceType::Crystal ? 360.0f : 220.0f;
        }

        if (score < bestScore)
        {
            bestScore = score;
            best = &pickup;
        }
    }

    return best;
}

struct BotCombatPowerWeights
{
    float health = 1.05f;
    float sword = 17.0f;
    float armor = 14.0f;
    float shield = 13.0f;
    float tool = 8.0f;
    float fireball = 8.0f;
    float dash = 7.0f;
    float molotov = 6.0f;
    float speedBoost = 10.0f;
};

constexpr BotCombatPowerWeights kDecisionCombatPowerWeights {};
constexpr BotCombatPowerWeights kPathCombatPowerWeights {
    1.1f,
    18.0f,
    14.0f,
    13.0f,
    8.0f,
    7.0f,
    6.0f,
    5.0f,
    9.0f
};

BotCombatPowerWeights ApplyCombatTuning(BotCombatPowerWeights weights, const BotTuningGenome& tuning)
{
    weights.shield = tuning.shieldPower;
    weights.speedBoost = tuning.speedBoostPower;
    return weights;
}

float BotCombatPowerScore(const Player& player, const BotCombatPowerWeights& weights)
{
    const Inventory& inventory = player.GetInventory();
    float score = static_cast<float>(player.GetHealth()) * weights.health;
    score += static_cast<float>(inventory.GetSwordLevel()) * weights.sword;
    score += static_cast<float>(inventory.GetArmorLevel()) * weights.armor;
    score += player.HasShield() ? weights.shield : 0.0f;
    score += static_cast<float>(inventory.GetToolLevel()) * weights.tool;
    score += static_cast<float>(inventory.GetUtility(UtilityType::Fireball)) * weights.fireball;
    score += static_cast<float>(inventory.GetUtility(UtilityType::Dash)) * weights.dash;
    score += static_cast<float>(inventory.GetUtility(UtilityType::Molotov)) * weights.molotov;
    score += player.GetSpeedBoostTimer() > 0.0f ? weights.speedBoost : 0.0f;
    return score;
}

float BotCombatPowerScore(const Player& player, const BotTuningGenome& tuning)
{
    return BotCombatPowerScore(player, ApplyCombatTuning(kDecisionCombatPowerWeights, tuning));
}

struct FightAssessment
{
    float selfPower = 0.0f;
    float targetPower = 0.0f;
    float allyPower = 0.0f;
    float enemyPower = 0.0f;
    float powerMargin = 0.0f;
    float distance = 999.0f;
    int nearbyAllies = 0;
    int nearbyEnemies = 0;
    bool canWin = false;
    bool shouldFight = false;
    bool shouldRetreat = false;
};

FightAssessment AssessFight(
    const Player& bot,
    const Player& target,
    const std::vector<Player*>* aliveAllies,
    const std::vector<Player*>* aliveEnemies,
    BotRole role,
    BotDifficulty difficulty,
    const BotTuningGenome& tuning,
    bool defendingCore,
    bool finalLifeTarget,
    bool objectiveBlocker)
{
    FightAssessment result {};
    result.selfPower = BotCombatPowerScore(bot, tuning);
    result.targetPower = BotCombatPowerScore(target, tuning);
    result.distance = std::sqrt(DistanceSquared(bot.GetPosition(), target.GetPosition()));

    // Hunting a final-life target is a pack activity: allies still closing in
    // should already count, otherwise hunters trickle in one by one and lose.
    const float allyAssistRangeSq = finalLifeTarget ? 16.0f * 16.0f : 9.5f * 9.5f;
    constexpr float enemyAssistRangeSq = 8.5f * 8.5f;
    if (aliveAllies != nullptr)
    {
        for (const Player* ally : *aliveAllies)
        {
            if (ally == nullptr
                || ally->GetId() == bot.GetId()
                || ally->GetTeamId() != bot.GetTeamId()
                || !ally->IsAlive()
                || ally->IsEliminated())
            {
                continue;
            }

            const bool closeToFight = DistanceSquared(ally->GetPosition(), target.GetPosition()) < allyAssistRangeSq
                || DistanceSquared(ally->GetPosition(), bot.GetPosition()) < allyAssistRangeSq;
            if (!closeToFight)
            {
                continue;
            }

            ++result.nearbyAllies;
            result.allyPower += BotCombatPowerScore(*ally, tuning) * tuning.allyAssistWeight;
        }
    }
    if (aliveEnemies != nullptr)
    {
        for (const Player* enemy : *aliveEnemies)
        {
            if (enemy == nullptr
                || enemy->GetId() == target.GetId()
                || enemy->GetTeamId() == bot.GetTeamId()
                || !enemy->IsAlive()
                || enemy->IsEliminated())
            {
                continue;
            }

            const bool closeToFight = DistanceSquared(enemy->GetPosition(), target.GetPosition()) < enemyAssistRangeSq
                || DistanceSquared(enemy->GetPosition(), bot.GetPosition()) < enemyAssistRangeSq;
            if (!closeToFight)
            {
                continue;
            }

            ++result.nearbyEnemies;
            result.enemyPower += BotCombatPowerScore(*enemy, tuning) * tuning.enemyAssistWeight;
        }
    }

    result.powerMargin = result.selfPower + result.allyPower - result.targetPower - result.enemyPower;

    float requiredMargin = FightRequiredMarginForDifficulty(tuning, difficulty);
    if (role == BotRole::Fighter)
    {
        requiredMargin -= 12.0f;
    }
    else if (role == BotRole::Collector)
    {
        requiredMargin += 14.0f;
    }
    else if (role == BotRole::Defender && defendingCore)
    {
        requiredMargin -= 18.0f;
    }
    if (finalLifeTarget)
    {
        requiredMargin -= 22.0f;
    }
    if (objectiveBlocker)
    {
        requiredMargin -= 10.0f;
    }
    if (bot.GetHealth() < 48)
    {
        requiredMargin += 18.0f;
    }
    const int outnumberedBy = std::max(0, result.nearbyEnemies - result.nearbyAllies);
    requiredMargin += static_cast<float>(outnumberedBy) * 16.0f;

    const bool targetWeak = target.GetHealth() <= bot.GetHealth() - (difficulty == BotDifficulty::Easy ? 34 : 18)
        && outnumberedBy < 2;
    result.canWin = result.powerMargin >= requiredMargin;
    result.shouldFight = defendingCore
        || finalLifeTarget
        || objectiveBlocker
        || targetWeak
        || result.canWin;
    result.shouldRetreat = !defendingCore
        && !finalLifeTarget
        && bot.GetHealth() < (role == BotRole::Collector ? 72 : 58)
        && result.powerMargin < RetreatPowerMarginForDifficulty(tuning, difficulty);
    if (!defendingCore && !finalLifeTarget && outnumberedBy >= 2 && bot.GetHealth() < 70)
    {
        result.shouldRetreat = true;
    }

    return result;
}

// A chase that has not closed the gap for a while gets banned for a few
// seconds: endless pursuits are what stalls the late game. Targets already
// in melee reach are always worth finishing regardless of the ban.
bool ChaseBlockedByFutility(const BotMemory& memory, const Player* target, Vector3 botPos)
{
    if (memory.chaseBanTimer <= 0.0f || target == nullptr)
    {
        return false;
    }
    return DistanceSquared(botPos, target->GetPosition()) > 49.0f;
}

struct BotMacroDirective
{
    bool active = false;
    BotIntent intent = BotIntent::SecureResources;
    Vector3 target {};
    Player* fightTarget = nullptr;
    const char* reason = "";
};

struct BotDecisionContext
{
    const Player& bot;
    const Team& team;
    const Inventory& inventory;
    const BotMemory& memory;
    const BotTuningGenome& tuning;
    BotDifficulty difficulty = BotDifficulty::Normal;
    float matchTime = 0.0f;
    Player* nearbyEnemy = nullptr;
    Player* weakEnemy = nullptr;
    Player* huntEnemy = nullptr;
    Player* enemyAtCore = nullptr;
    EnergyCore* enemyCore = nullptr;
    BotTeamSnapshot teamPlan {};
    Vector3 botPos {};
    Vector3 coreHome {};
    Vector3 resourceTarget {};
    float enemyAtCoreDistance = std::numeric_limits<float>::max();
    float distanceFromHome = 0.0f;
    float nearbyEnemyDistance = 999.0f;
    FightAssessment nearbyFight {};
    int retreatHealth = 0;
    int fightHealth = 0;
    int desiredBlocks = 0;
    ResourceType resourceTargetType = ResourceType::Iron;
    bool retreating = false;
    bool defenderAwayFromBase = false;
    bool carryingLoot = false;
    bool wantsShop = false;
    bool canBreakDefense = false;
    bool readyToRush = false;
    bool coreNeedsRepair = false;
    bool coreDefenseCritical = false;
    bool coreCanUpgrade = false;
    BotStrategicPlan strategicPlan {};
    bool finalDuelPhase = false;
    bool huntEnemyOnFinalLife = false;
    bool finalLifeTargetClose = false;
    bool shouldFightNearby = false;
    bool shouldPressureCore = false;
    bool hasResourceTarget = false;
    int coordinatedAttackersOnTarget = 0;
    int coordinatedDefenders = 0;
    int coordinatedHelpCalls = 0;
    int missingDefenseBlocks = 0;
};

float StrategicPlanUpdateCadence(BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return tuning.easyPlanCadence;
    case BotDifficulty::Hard:
        return tuning.hardPlanCadence;
    case BotDifficulty::Normal:
        break;
    }
    return tuning.normalPlanCadence;
}

bool StrategicPlanExpired(const StrategicPlan& plan)
{
    return plan.plannedDuration > 0.0f && plan.elapsedTime >= plan.plannedDuration;
}

bool ShouldInterruptStrategicPlan(const StrategicPlan& plan, const BotDecisionContext& ctx)
{
    if (plan.goal == StrategicGoal::Idle || StrategicPlanExpired(plan))
    {
        return true;
    }

    const bool emergencyDefense = ctx.team.coreAlive
        && (ctx.enemyAtCore != nullptr || (ctx.coreDefenseCritical && ctx.matchTime > 55.0f));
    if (emergencyDefense && plan.goal != StrategicGoal::BaseDefense)
    {
        return true;
    }
    if (ctx.huntEnemyOnFinalLife && ctx.huntEnemy != nullptr && ctx.matchTime > 135.0f && plan.goal != StrategicGoal::HuntPlayers)
    {
        return true;
    }

    if ((plan.goal == StrategicGoal::BridgePush || plan.goal == StrategicGoal::CoreAssault)
        && (ctx.enemyCore == nullptr
            || !ctx.enemyCore->IsAlive()
            || (plan.targetTeamId >= 0 && ctx.enemyCore->GetTeamId() != plan.targetTeamId)))
    {
        return true;
    }
    if (plan.committed)
    {
        return false;
    }
    if (plan.goal == StrategicGoal::EconomicPhase && ctx.readyToRush && ctx.strategicPlan.focus == BotStrategicFocus::Pressure)
    {
        return true;
    }

    return false;
}

StrategicPlan EvaluateStrategicPlan(const BotDecisionContext& ctx)
{
    StrategicPlan plan {};
    const bool emergencyDefense = ctx.team.coreAlive && (ctx.enemyAtCore != nullptr || ctx.coreDefenseCritical);
    if (emergencyDefense)
    {
        plan.goal = StrategicGoal::BaseDefense;
        plan.plannedDuration = ctx.coreDefenseCritical ? 16.0f : 11.0f;
        plan.committed = true;
        plan.reason = ctx.enemyAtCore != nullptr ? "core under attack" : "core defense critical";
        return plan;
    }

    if (ctx.huntEnemyOnFinalLife && ctx.huntEnemy != nullptr && ctx.matchTime > 135.0f)
    {
        plan.goal = StrategicGoal::HuntPlayers;
        plan.plannedDuration = 24.0f;
        plan.targetTeamId = ctx.huntEnemy->GetTeamId();
        plan.committed = ctx.matchTime > 155.0f || ctx.finalLifeTargetClose;
        plan.reason = "enemy final life";
        return plan;
    }

    const bool earlyEconomy = ctx.matchTime < ctx.tuning.earlyEconomySeconds
        && !ctx.carryingLoot
        && !ctx.readyToRush
        && ctx.hasResourceTarget;
    if (earlyEconomy)
    {
        plan.goal = StrategicGoal::EconomicPhase;
        plan.plannedDuration = 12.0f;
        plan.reason = "early economy";
        return plan;
    }

    if (ctx.strategicPlan.focus == BotStrategicFocus::Defense
        || (ctx.coreDefenseCritical && ctx.matchTime > 55.0f))
    {
        plan.goal = StrategicGoal::BaseDefense;
        plan.plannedDuration = 12.0f;
        plan.reason = ctx.coreDefenseCritical ? "repair base" : "hold defense";
        return plan;
    }

    if (ctx.enemyCore != nullptr && ctx.enemyCore->IsAlive() && (ctx.readyToRush || ctx.strategicPlan.allIn))
    {
        const GridPos enemyCoreBlock = ctx.enemyCore->GetBlockPosition();
        const Vector3 enemyCoreTarget {
            static_cast<float>(enemyCoreBlock.x),
            1.5f,
            static_cast<float>(enemyCoreBlock.z)
        };
        const bool assaultNow = ctx.canBreakDefense
            || ctx.strategicPlan.allIn
            || DistanceSquared(ctx.botPos, enemyCoreTarget) < 90.0f;
        plan.goal = assaultNow ? StrategicGoal::CoreAssault : StrategicGoal::BridgePush;
        plan.plannedDuration = assaultNow ? 18.0f : 24.0f;
        plan.targetTeamId = ctx.enemyCore->GetTeamId();
        plan.committed = ctx.strategicPlan.allIn || ctx.matchTime > 150.0f;
        plan.reason = assaultNow ? "assault core" : "bridge and pressure";
        return plan;
    }

    if (ctx.hasResourceTarget && ctx.resourceTargetType == ResourceType::Crystal && ctx.matchTime > 32.0f)
    {
        plan.goal = StrategicGoal::MidControl;
        plan.plannedDuration = 14.0f;
        plan.reason = "mid control";
        return plan;
    }

    plan.goal = StrategicGoal::EconomicPhase;
    plan.plannedDuration = ctx.carryingLoot ? 6.0f : 10.0f;
    plan.reason = ctx.carryingLoot ? "bank resources" : "default economy";
    return plan;
}

BotMacroDirective EvaluateAutonomousMacroDirective(const BotDecisionContext& ctx)
{
    BotMacroDirective directive {};
    const Vector3 homeTarget { ctx.coreHome.x, 1.5f, ctx.coreHome.z };
    const Vector3 shopTarget { ctx.team.shopPosition.x, 1.5f, ctx.team.shopPosition.z };
    const Vector3 enemyCoreTarget = ctx.enemyCore != nullptr
        ? Vector3 {
            static_cast<float>(ctx.enemyCore->GetBlockPosition().x),
            1.5f,
            static_cast<float>(ctx.enemyCore->GetBlockPosition().z) }
        : Vector3 { 0.0f, 1.5f, 0.0f };
    const bool isAssaultRole = ctx.memory.role == BotRole::Rusher || ctx.memory.role == BotRole::Fighter;
    const bool isDefender = ctx.memory.role == BotRole::Defender;
    const bool hasBuyMoney = ctx.inventory.GetResource(ResourceType::Iron) >= 8
        || ctx.inventory.GetResource(ResourceType::Gold) >= 4
        || ctx.inventory.GetResource(ResourceType::Crystal) >= 2;
    const bool idleNearBase = ctx.distanceFromHome < 90.0f
        && ctx.memory.intentTimer > 8.5f
        && ctx.enemyAtCore == nullptr
        && ctx.nearbyEnemy == nullptr;
    const bool teamWantsPressure = ctx.strategicPlan.focus == BotStrategicFocus::Pressure;
    const bool attackSlotOpen = ctx.coordinatedAttackersOnTarget < std::max(1, ctx.strategicPlan.desiredAttackers);

    if (ctx.enemyAtCore != nullptr)
    {
        directive.active = true;
        directive.intent = BotIntent::DefendCore;
        directive.target = ctx.enemyAtCore->GetPosition();
        directive.fightTarget = ctx.enemyAtCore;
        directive.reason = "macro emergency defend";
        return directive;
    }

    if (ctx.huntEnemyOnFinalLife
        && ctx.huntEnemy != nullptr
        && ctx.matchTime > 145.0f
        && ctx.finalLifeTargetClose
        && !ChaseBlockedByFutility(ctx.memory, ctx.huntEnemy, ctx.botPos)
        && (ctx.bot.GetHealth() > ctx.fightHealth || ctx.matchTime > 150.0f)
        && (ctx.memory.role != BotRole::Defender || ctx.matchTime > 150.0f || !ctx.team.coreAlive))
    {
        directive.active = true;
        directive.intent = BotIntent::ChaseWeakEnemy;
        directive.target = ctx.huntEnemy->GetPosition();
        directive.fightTarget = ctx.huntEnemy;
        directive.reason = "macro final-life cleanup";
        return directive;
    }

    if (ctx.nearbyEnemy != nullptr && !isDefender && ctx.memory.chaseBanTimer <= 0.0f)
    {
        const bool canWinFight = ctx.nearbyFight.canWin || ctx.nearbyFight.powerMargin >= -4.0f;
        if (canWinFight && (isAssaultRole || ctx.matchTime > 55.0f))
        {
            directive.active = true;
            directive.intent = BotIntent::FightEnemy;
            directive.target = ctx.nearbyEnemy->GetPosition();
            directive.fightTarget = ctx.nearbyEnemy;
            directive.reason = "macro favorable duel";
            return directive;
        }
    }

    const bool routineRepairWindow = (isDefender && ctx.inventory.GetBlocks() > 0)
        || (ctx.coreDefenseCritical && ctx.matchTime > 55.0f)
        || (ctx.missingDefenseBlocks >= 7 && ctx.matchTime > 80.0f);

    if (isDefender && ctx.team.coreAlive && ctx.coreNeedsRepair && routineRepairWindow)
    {
        directive.active = true;
        if (ctx.inventory.GetBlocks() > 0)
        {
            directive.intent = BotIntent::RepairCoreDefense;
            directive.target = homeTarget;
            directive.reason = "macro repair now";
        }
        else if (ctx.hasResourceTarget)
        {
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "macro gather for repair";
        }
        else
        {
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "macro buy blocks";
        }
        return directive;
    }

    if (ctx.memory.currentPlan.goal == StrategicGoal::BaseDefense
        && ctx.team.coreAlive
        && ctx.coreNeedsRepair
        && routineRepairWindow
        && (isDefender || ctx.coordinatedDefenders < std::max(1, ctx.strategicPlan.desiredDefenders)))
    {
        directive.active = true;
        if (ctx.inventory.GetBlocks() > 0)
        {
            directive.intent = BotIntent::RepairCoreDefense;
            directive.target = homeTarget;
            directive.reason = "plan repair base";
        }
        else if (ctx.hasResourceTarget)
        {
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "plan gather blocks";
        }
        else
        {
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "plan buy blocks";
        }
        return directive;
    }

    if ((ctx.memory.currentPlan.goal == StrategicGoal::BridgePush || ctx.memory.currentPlan.goal == StrategicGoal::CoreAssault)
        && ctx.enemyCore != nullptr
        && ctx.enemyCore->IsAlive()
        && ctx.memory.currentPlan.targetTeamId == ctx.enemyCore->GetTeamId()
        && (attackSlotOpen || ctx.strategicPlan.allIn)
        && (ctx.readyToRush || ctx.memory.currentPlan.goal == StrategicGoal::BridgePush || ctx.matchTime > 90.0f))
    {
        directive.active = true;
        directive.intent = ctx.canBreakDefense ? BotIntent::BreakCoreDefense : BotIntent::PressureCore;
        directive.target = enemyCoreTarget;
        directive.reason = ctx.memory.currentPlan.goal == StrategicGoal::CoreAssault ? "plan core assault" : "plan bridge push";
        return directive;
    }

    if ((ctx.memory.currentPlan.goal == StrategicGoal::EconomicPhase || ctx.memory.currentPlan.goal == StrategicGoal::MidControl)
        && ctx.hasResourceTarget
        && !ctx.carryingLoot
        && (!ctx.wantsShop || !hasBuyMoney)
        && ctx.enemyAtCore == nullptr)
    {
        directive.active = true;
        directive.intent = BotIntent::SecureResources;
        directive.target = ctx.resourceTarget;
        directive.reason = ctx.memory.currentPlan.goal == StrategicGoal::MidControl ? "plan mid control" : "plan economy";
        return directive;
    }

    const bool openingPhase = ctx.matchTime < ctx.tuning.pressurePhaseSeconds;
    const bool midPhase = ctx.matchTime >= ctx.tuning.pressurePhaseSeconds && ctx.matchTime < ctx.tuning.latePressureSeconds;

    if (openingPhase)
    {
        if (ctx.hasResourceTarget && (!ctx.carryingLoot || ctx.inventory.GetBlocks() < 12))
        {
            directive.active = true;
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "macro opening gather";
            return directive;
        }
        if (hasBuyMoney && ctx.wantsShop)
        {
            directive.active = true;
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "macro opening shop";
            return directive;
        }
    }

    if (midPhase)
    {
        if (isAssaultRole
            && ctx.enemyCore != nullptr
            && (attackSlotOpen || ctx.strategicPlan.allIn)
            && (ctx.readyToRush || teamWantsPressure || ctx.teamPlan.activePressure <= 1 || idleNearBase))
        {
            directive.active = true;
            directive.intent = ctx.canBreakDefense ? BotIntent::BreakCoreDefense : BotIntent::PressureCore;
            directive.target = enemyCoreTarget;
            directive.reason = ctx.canBreakDefense ? "macro mid breach" : "macro mid pressure";
            return directive;
        }
        if (hasBuyMoney && ctx.wantsShop && !ctx.readyToRush)
        {
            directive.active = true;
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "macro mid gear";
            return directive;
        }
        if (ctx.hasResourceTarget && (idleNearBase || !ctx.carryingLoot))
        {
            directive.active = true;
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "macro mid rotate";
            return directive;
        }
    }

    if (ctx.matchTime >= ctx.tuning.latePressureSeconds)
    {
        if (ctx.huntEnemyOnFinalLife
            && ctx.huntEnemy != nullptr
            && (ctx.finalLifeTargetClose || ctx.matchTime > 210.0f)
            && !ChaseBlockedByFutility(ctx.memory, ctx.huntEnemy, ctx.botPos))
        {
            directive.active = true;
            directive.intent = BotIntent::ChaseWeakEnemy;
            directive.target = ctx.huntEnemy->GetPosition();
            directive.fightTarget = ctx.huntEnemy;
            directive.reason = "macro late cleanup";
            return directive;
        }
        if (ctx.enemyCore != nullptr && ctx.enemyCore->IsAlive())
        {
            if ((ctx.canBreakDefense || isAssaultRole || ctx.readyToRush || teamWantsPressure || idleNearBase)
                && (attackSlotOpen || ctx.strategicPlan.allIn))
            {
                directive.active = true;
                directive.intent = ctx.canBreakDefense ? BotIntent::BreakCoreDefense : BotIntent::PressureCore;
                directive.target = enemyCoreTarget;
                directive.reason = ctx.canBreakDefense ? "macro late breach" : "macro late all-in";
                return directive;
            }
        }
        else if (ctx.huntEnemy != nullptr && !ChaseBlockedByFutility(ctx.memory, ctx.huntEnemy, ctx.botPos))
        {
            directive.active = true;
            directive.intent = BotIntent::ChaseWeakEnemy;
            directive.target = ctx.huntEnemy->GetPosition();
            directive.fightTarget = ctx.huntEnemy;
            directive.reason = "macro cleanup";
            return directive;
        }
    }

    if (idleNearBase && ctx.hasResourceTarget)
    {
        directive.active = true;
        directive.intent = BotIntent::SecureResources;
        directive.target = ctx.resourceTarget;
        directive.reason = "macro anti-camp";
        return directive;
    }

    return directive;
}

struct BotDecisionDerivedContext
{
    Vector3 homeTarget {};
    Vector3 shopTarget {};
    bool canAffordAnyBuy = false;
    bool canRepairNow = false;
    bool canRepairSoon = false;
    bool idleNearBase = false;
};

BotDecisionDerivedContext BuildBotDecisionDerivedContext(const BotDecisionContext& ctx)
{
    const int iron = ctx.inventory.GetResource(ResourceType::Iron);
    const int gold = ctx.inventory.GetResource(ResourceType::Gold);
    const int crystal = ctx.inventory.GetResource(ResourceType::Crystal);
    return BotDecisionDerivedContext {
        Vector3 { ctx.coreHome.x, 1.5f, ctx.coreHome.z },
        Vector3 { ctx.team.shopPosition.x, 1.5f, ctx.team.shopPosition.z },
        iron >= 8 || gold >= 4 || crystal >= 2,
        ctx.inventory.GetBlocks() > 0,
        ctx.inventory.GetBlocks() > 0 || iron >= 8,
        ctx.distanceFromHome < 84.0f
            && ctx.memory.intentTimer > 9.0f
            && ctx.enemyAtCore == nullptr
            && ctx.nearbyEnemy == nullptr
    };
}

void ConsiderBotDecision(
    BotDecision& decision,
    const BotDecisionContext& ctx,
    BotIntent intent,
    float score,
    Vector3 target,
    Player* fightTarget,
    const char* reason)
{
    score += RoleIntentBias(ctx.memory.role, intent, ctx.tuning);
    if (ctx.memory.intent == intent)
    {
        score += 105.0f + std::min(45.0f, ctx.memory.intentTimer * 12.0f);
    }
    else if (ctx.memory.intentLockTimer > 0.0f
        && intent != BotIntent::DefendCore
        && intent != BotIntent::Recover
        && intent != BotIntent::RetreatHome)
    {
        score -= 140.0f;
    }

    if (score > decision.score)
    {
        decision.intent = intent;
        decision.state = StateForIntent(intent);
        decision.target = target;
        decision.fightTarget = fightTarget;
        decision.score = score;
        decision.reason = reason;
    }
}

void ConsiderRecoverIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if (ctx.memory.stuckTimer <= 1.05f)
    {
        return;
    }

    Vector3 recoverDirection = Normalize2D(Vector3 { ctx.botPos.x, 0.0f, ctx.botPos.z });
    if (Length2D(recoverDirection) <= 0.0001f)
    {
        recoverDirection = Normalize2D(Vector3 {
            derived.homeTarget.x - ctx.botPos.x,
            0.0f,
            derived.homeTarget.z - ctx.botPos.z
        });
    }
    if (Length2D(recoverDirection) <= 0.0001f)
    {
        recoverDirection = Vector3 { 1.0f, 0.0f, 0.0f };
    }

    const float rotateStep = std::floor(std::max(0.0f, ctx.memory.stuckTimer - 1.05f) / 0.75f);
    if (rotateStep > 0.0f)
    {
        const float directionSign = (static_cast<int>(rotateStep) % 2 == 0) ? -1.0f : 1.0f;
        const float angle = directionSign * rotateStep * 0.25f * kBotPi;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        recoverDirection = Normalize2D(Vector3 {
            recoverDirection.x * cosine - recoverDirection.z * sine,
            0.0f,
            recoverDirection.x * sine + recoverDirection.z * cosine
        });
    }

    Vector3 recoverTarget {
        ctx.botPos.x + recoverDirection.x * 3.0f,
        1.5f,
        ctx.botPos.z + recoverDirection.z * 3.0f
    };
    if (ctx.memory.stuckTimer < 2.25f
        && DistanceSquared(recoverTarget, derived.homeTarget) > DistanceSquared(ctx.botPos, derived.homeTarget) + 36.0f)
    {
        recoverTarget = derived.homeTarget;
    }

    ConsiderBotDecision(decision, ctx, BotIntent::Recover, 980.0f + ctx.memory.stuckTimer * 120.0f, recoverTarget, nullptr, "unstick");
}

void ConsiderBaseDefenseIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    if (ctx.enemyAtCore == nullptr)
    {
        return;
    }

    const float defenderBonus = ctx.memory.role == BotRole::Defender ? 220.0f : 80.0f;
    const float lonelyBaseBonus = ctx.teamPlan.alliesNearCore == 0 ? 130.0f : 0.0f;
    const float coordinationBonus = ctx.coordinatedHelpCalls > 0 ? 110.0f : (ctx.coordinatedDefenders == 0 ? 70.0f : 0.0f);
    const float defenseCriticalBonus = ctx.coreDefenseCritical ? 120.0f : 0.0f;
    const float personalPlanBonus = ctx.memory.currentPlan.goal == StrategicGoal::BaseDefense ? 115.0f : 0.0f;
    const float strategicDefenseBonus = ctx.strategicPlan.defenseUrgency * ctx.tuning.strategicDefenseUrgencyScale;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::DefendCore,
        1180.0f + defenderBonus + lonelyBaseBonus + coordinationBonus + defenseCriticalBonus + personalPlanBonus + strategicDefenseBonus - std::sqrt(ctx.enemyAtCoreDistance) * 10.0f,
        ctx.enemyAtCore->GetPosition(),
        ctx.enemyAtCore,
        "enemy at core");
}

void ConsiderRetreatIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if ((!ctx.retreating && !ctx.defenderAwayFromBase)
        || (ctx.memory.role == BotRole::Defender && ctx.enemyAtCore != nullptr))
    {
        return;
    }

    const float lootBonus = ctx.carryingLoot ? static_cast<float>(ctx.memory.carriedResourceValue) * 4.0f : 0.0f;
    const float healthBonus = static_cast<float>(std::max(0, ctx.retreatHealth + 18 - ctx.bot.GetHealth())) * 9.0f;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::RetreatHome,
        760.0f + lootBonus + healthBonus + (ctx.defenderAwayFromBase ? 180.0f : 0.0f),
        ctx.memory.role == BotRole::Defender ? derived.homeTarget : derived.shopTarget,
        nullptr,
        ctx.defenderAwayFromBase ? "return to base" : "heal and bank");
}

void ConsiderRepairIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if ((!ctx.coreNeedsRepair && !ctx.coreCanUpgrade) || !ctx.team.coreAlive)
    {
        return;
    }

    const bool routineRepair = ctx.coreNeedsRepair && !ctx.coreDefenseCritical;
    if (routineRepair && ctx.memory.role != BotRole::Defender)
    {
        return;
    }
    if (!derived.canRepairNow && !ctx.coreCanUpgrade)
    {
        return;
    }

    const bool assignedBuilder = ctx.memory.role == BotRole::Defender
        || ctx.coreDefenseCritical
        || ((ctx.teamPlan.defendersNearCore == 0 || ctx.coordinatedDefenders == 0)
            && ctx.teamPlan.activeRepair == 0
            && ctx.distanceFromHome < 120.0f);
    if (!assignedBuilder || !derived.canRepairSoon)
    {
        return;
    }

    const float defenderBonus = ctx.memory.role == BotRole::Defender ? 280.0f : 0.0f;
    const float repairCrowdPenalty = static_cast<float>(ctx.teamPlan.activeRepair) * 115.0f;
    const float defenseUrgency = (ctx.coreDefenseCritical ? 210.0f : 0.0f)
        + static_cast<float>(ctx.missingDefenseBlocks) * 34.0f
        + ctx.strategicPlan.defenseUrgency * ctx.tuning.repairUrgencyScale
        + (ctx.memory.currentPlan.goal == StrategicGoal::BaseDefense ? 125.0f : 0.0f);
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::RepairCoreDefense,
        500.0f
            + defenderBonus
            + defenseUrgency
            + (ctx.coreCanUpgrade ? 90.0f : 0.0f)
            + (derived.canRepairNow ? 70.0f : -90.0f)
            - repairCrowdPenalty
            - std::sqrt(ctx.distanceFromHome) * 5.0f,
        derived.homeTarget,
        nullptr,
        ctx.coreCanUpgrade ? "upgrade defense" : "repair defense");
}

void ConsiderGearIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if ((!ctx.wantsShop && ctx.inventory.GetBlocks() >= 3)
        || (ctx.huntEnemyOnFinalLife && ctx.finalLifeTargetClose && ctx.matchTime > 155.0f))
    {
        return;
    }

    const float blockPressure = static_cast<float>(std::max(0, ctx.desiredBlocks - ctx.inventory.GetBlocks())) * 7.0f;
    const float lootPressure = ctx.carryingLoot ? static_cast<float>(ctx.memory.carriedResourceValue) * 3.4f : 0.0f;
    const float shopCrowdPenalty = static_cast<float>(ctx.teamPlan.activeShop) * 72.0f;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::GearUp,
        470.0f
            + blockPressure
            + lootPressure
            + (ctx.bot.GetHealth() < 70 ? 120.0f : 0.0f)
            + (derived.canAffordAnyBuy ? 80.0f : -190.0f)
            - shopCrowdPenalty,
        derived.shopTarget,
        nullptr,
        "buy gear");
}

void ConsiderCombatIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    if (ctx.shouldFightNearby && ctx.nearbyEnemy != nullptr)
    {
        const float roleBonus = ctx.memory.role == BotRole::Fighter ? 150.0f : (ctx.memory.role == BotRole::Defender ? 85.0f : 0.0f);
        const float healthEdge = static_cast<float>(ctx.bot.GetHealth() - ctx.nearbyEnemy->GetHealth()) * 2.8f;
        const float powerEdge = std::clamp(ctx.nearbyFight.powerMargin, -70.0f, 70.0f) * 1.15f;
        ConsiderBotDecision(
            decision,
            ctx,
            BotIntent::FightEnemy,
            520.0f + roleBonus + healthEdge + powerEdge - ctx.nearbyEnemyDistance * 18.0f,
            ctx.nearbyEnemy->GetPosition(),
            ctx.nearbyEnemy,
            "take fight");
    }

    if (ctx.weakEnemy != nullptr
        && ctx.bot.GetHealth() > ctx.fightHealth
        && !ChaseBlockedByFutility(ctx.memory, ctx.weakEnemy, ctx.botPos))
    {
        const float weakDistance = std::sqrt(DistanceSquared(ctx.bot.GetPosition(), ctx.weakEnemy->GetPosition()));
        ConsiderBotDecision(
            decision,
            ctx,
            BotIntent::ChaseWeakEnemy,
            500.0f + static_cast<float>(ctx.bot.GetHealth() - ctx.weakEnemy->GetHealth()) * 3.8f - weakDistance * 8.0f,
            ctx.weakEnemy->GetPosition(),
            ctx.weakEnemy,
            "finish weak enemy");
    }
}

Vector3 CoreTargetPosition(const EnergyCore& core)
{
    return Vector3 {
        static_cast<float>(core.GetBlockPosition().x),
        1.5f,
        static_cast<float>(core.GetBlockPosition().z)
    };
}

void ConsiderCorePressureIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    const bool strategicTarget = ctx.enemyCore != nullptr
        && ctx.strategicPlan.attackCoreTeamId == ctx.enemyCore->GetTeamId();
    const bool personalPlanTarget = ctx.enemyCore != nullptr
        && (ctx.memory.currentPlan.goal == StrategicGoal::BridgePush
            || ctx.memory.currentPlan.goal == StrategicGoal::CoreAssault)
        && ctx.memory.currentPlan.targetTeamId == ctx.enemyCore->GetTeamId();
    const bool attackSlotOpen = ctx.coordinatedAttackersOnTarget < std::max(1, ctx.strategicPlan.desiredAttackers);
    const float strategicAttackBonus = strategicTarget ? ctx.strategicPlan.attackUrgency * ctx.tuning.strategicAttackUrgencyScale : 0.0f;
    const float personalPlanAttackBonus = personalPlanTarget
        ? (ctx.memory.currentPlan.goal == StrategicGoal::CoreAssault ? 155.0f : 105.0f)
        : 0.0f;
    const float attackSlotBonus = attackSlotOpen || ctx.strategicPlan.allIn ? ctx.tuning.attackSlotBonus : ctx.tuning.attackSlotPenalty;
    // Destroying a surviving core ends the match much faster than hunting
    // runners, so the assault options keep gaining weight in the late game.
    const float lateAssaultBonus = std::min(300.0f, std::max(0.0f, ctx.matchTime - ctx.tuning.latePressureSeconds) * 2.4f);

    if (ctx.canBreakDefense && ctx.enemyCore != nullptr)
    {
        const float coordinationPenalty = static_cast<float>(ctx.coordinatedAttackersOnTarget) * ctx.tuning.breakCoordinationPenalty;
        ConsiderBotDecision(
            decision,
            ctx,
            BotIntent::BreakCoreDefense,
            650.0f + (ctx.memory.role == BotRole::Rusher ? 120.0f : 0.0f) + strategicAttackBonus + personalPlanAttackBonus + attackSlotBonus + lateAssaultBonus - coordinationPenalty,
            CoreTargetPosition(*ctx.enemyCore),
            nullptr,
            "crack defense");
    }

    if (ctx.shouldPressureCore && ctx.enemyCore != nullptr)
    {
        const float teamPushBonus = ctx.teamPlan.activePressure > 0 ? 70.0f : 0.0f;
        const float baseCoveredBonus = ctx.teamPlan.alliesNearCore > 0 || !ctx.team.coreAlive ? 60.0f : -85.0f;
        const float timePressureBonus = std::min(280.0f, std::max(0.0f, ctx.matchTime - 80.0f) * 1.6f);
        const float coordinationPenalty = static_cast<float>(ctx.coordinatedAttackersOnTarget) * ctx.tuning.pressureCoordinationPenalty;
        ConsiderBotDecision(
            decision,
            ctx,
            BotIntent::PressureCore,
            570.0f + teamPushBonus + baseCoveredBonus + timePressureBonus + strategicAttackBonus + personalPlanAttackBonus + attackSlotBonus + lateAssaultBonus + (ctx.memory.role == BotRole::Rusher ? 150.0f : 60.0f) - coordinationPenalty,
            CoreTargetPosition(*ctx.enemyCore),
            nullptr,
            "rush core");
    }
}

void ConsiderResourceIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if (!ctx.hasResourceTarget
        || ctx.carryingLoot
        || (ctx.huntEnemyOnFinalLife && ctx.finalLifeTargetClose && ctx.matchTime > 155.0f))
    {
        return;
    }

    const float resourceBonus = ctx.resourceTargetType == ResourceType::Crystal
        ? (ctx.memory.role == BotRole::Collector ? 190.0f : 120.0f)
        : (ctx.resourceTargetType == ResourceType::Gold ? 105.0f : 45.0f);
    const float collectorBonus = ctx.memory.role == BotRole::Collector ? 260.0f : 0.0f;
    const float resourceCrowdPenalty = static_cast<float>(ctx.teamPlan.activeResource) * (ctx.memory.role == BotRole::Collector ? 12.0f : 58.0f);
    const float blockShortageBoost = ctx.inventory.GetBlocks() < 6 ? 220.0f : 0.0f;
    const float idleExpeditionBoost = derived.idleNearBase ? 240.0f : 0.0f;
    const float strategicEconomyBonus = ctx.strategicPlan.focus == BotStrategicFocus::Economy ? ctx.tuning.strategicEconomyBonus : 0.0f;
    const float personalPlanEconomyBonus = ctx.memory.currentPlan.goal == StrategicGoal::EconomicPhase
        || ctx.memory.currentPlan.goal == StrategicGoal::MidControl
        ? ctx.tuning.personalEconomyBonus
        : 0.0f;
    const float strategicPressurePenalty = ctx.strategicPlan.focus == BotStrategicFocus::Pressure
        && ctx.memory.role != BotRole::Collector
        ? ctx.tuning.strategicPressureEconomyPenalty
        : 0.0f;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::SecureResources,
        390.0f + resourceBonus + collectorBonus + blockShortageBoost + idleExpeditionBoost + strategicEconomyBonus + personalPlanEconomyBonus - strategicPressurePenalty - resourceCrowdPenalty,
        ctx.resourceTarget,
        nullptr,
        ctx.resourceTargetType == ResourceType::Crystal ? "center crystals" : "secure resources");
}

void ConsiderFinalDuelIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    if (!ctx.finalDuelPhase
        || ctx.huntEnemy == nullptr
        || ChaseBlockedByFutility(ctx.memory, ctx.huntEnemy, ctx.botPos)
        || (ctx.bot.GetHealth() <= ctx.fightHealth && !(ctx.huntEnemyOnFinalLife && ctx.matchTime > 145.0f)))
    {
        return;
    }

    const float finalLifeBonus = ctx.huntEnemyOnFinalLife ? (ctx.finalLifeTargetClose && ctx.matchTime > 145.0f ? 420.0f : 120.0f) : 0.0f;
    // Capped: an unbounded urgency made every bot chase runners forever
    // instead of finishing the remaining cores.
    const float cleanupUrgency = ctx.huntEnemyOnFinalLife
        ? std::min(140.0f, std::max(0.0f, ctx.matchTime - 145.0f) * 5.0f)
        : 0.0f;
    const float personalHuntBonus = ctx.memory.currentPlan.goal == StrategicGoal::HuntPlayers ? 170.0f : 0.0f;
    const float distance = std::sqrt(DistanceSquared(ctx.bot.GetPosition(), ctx.huntEnemy->GetPosition()));
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::ChaseWeakEnemy,
        520.0f
            + finalLifeBonus
            + cleanupUrgency
            + personalHuntBonus
            + (ctx.team.coreAlive ? 0.0f : 120.0f)
            + static_cast<float>(ctx.bot.GetHealth() - ctx.huntEnemy->GetHealth()) * 1.8f
            - distance * 4.5f,
        ctx.huntEnemy->GetPosition(),
        ctx.huntEnemy,
        ctx.huntEnemyOnFinalLife ? "final-life cleanup" : "final duel");
}

void ConsiderLateMapPressureIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    if (ctx.enemyCore == nullptr
        || !ctx.enemyCore->IsAlive()
        || !ctx.readyToRush
        || (ctx.huntEnemyOnFinalLife && ctx.finalLifeTargetClose && ctx.matchTime > 155.0f))
    {
        return;
    }

    const float pressureCrowdPenalty = static_cast<float>(std::max(0, ctx.teamPlan.activePressure - 2)) * 46.0f;
    const float coordinationCrowdPenalty = static_cast<float>(ctx.coordinatedAttackersOnTarget) * ctx.tuning.lateCoordinationPenalty;
    const float lateMatchBoost = std::max(0.0f, ctx.matchTime - 120.0f) * 2.2f;
    const float strategicAttackBonus = ctx.strategicPlan.attackCoreTeamId == ctx.enemyCore->GetTeamId()
        ? ctx.strategicPlan.attackUrgency * ctx.tuning.lateAttackUrgencyScale
        : 0.0f;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::PressureCore,
        360.0f + lateMatchBoost + strategicAttackBonus + (ctx.memory.role == BotRole::Rusher ? 170.0f : 0.0f) - pressureCrowdPenalty - coordinationCrowdPenalty,
        CoreTargetPosition(*ctx.enemyCore),
        nullptr,
        "map pressure");
}

void ConsiderIdleResourceIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if (derived.idleNearBase
        && ctx.hasResourceTarget
        && !ctx.carryingLoot
        && !ctx.coreNeedsRepair
        && ctx.enemyAtCore == nullptr)
    {
        ConsiderBotDecision(decision, ctx, BotIntent::SecureResources, 860.0f, ctx.resourceTarget, nullptr, "leave base");
    }
}

BotDecision EvaluateBotDecision(const BotDecisionContext& ctx)
{
    BotDecision decision {};
    decision.target = Vector3 { 0.0f, 1.5f, 0.0f };
    const BotDecisionDerivedContext derived = BuildBotDecisionDerivedContext(ctx);

    ConsiderRecoverIntent(decision, ctx, derived);
    ConsiderBaseDefenseIntent(decision, ctx);
    ConsiderRetreatIntent(decision, ctx, derived);
    ConsiderRepairIntent(decision, ctx, derived);
    ConsiderGearIntent(decision, ctx, derived);
    ConsiderCombatIntent(decision, ctx);
    ConsiderCorePressureIntent(decision, ctx);
    ConsiderResourceIntent(decision, ctx, derived);
    ConsiderFinalDuelIntent(decision, ctx);
    ConsiderLateMapPressureIntent(decision, ctx);
    ConsiderIdleResourceIntent(decision, ctx, derived);

    if (decision.score < -9990.0f)
    {
        ConsiderBotDecision(decision, ctx, BotIntent::SecureResources, 0.0f, Vector3 { 0.0f, 1.5f, 0.0f }, nullptr, "default mid");
    }

    return decision;
}

void ApplyBotDecision(
    BotMemory& memory,
    const BotDecision& decision,
    BotDifficulty difficulty,
    const BotTuningGenome& tuning)
{
    if (decision.intent != memory.intent)
    {
        memory.intent = decision.intent;
        memory.intentTimer = 0.0f;
        memory.intentLockTimer = IntentLockSeconds(decision.intent, difficulty, tuning);
        memory.hasNavWaypoint = false;
        if (decision.intent == BotIntent::Recover)
        {
            memory.hasBreakTarget = false;
            memory.breakProgress = 0.0f;
        }
    }
    memory.intentReason = decision.reason;
    memory.intentScore = decision.score;

    if (decision.state != memory.state)
    {
        memory.state = decision.state;
        memory.stateTimer = 0.0f;
        if (decision.state == BotState::Fight)
        {
            memory.attackTimer = std::max(memory.attackTimer, BotFightReactionDelay(difficulty));
        }
    }
}

void UpdateBotStuckAfterMove(BotMemory& memory, Vector3 wish, float movedDistance, float dt, Vector3 currentPosition)
{
    if (Length2D(wish) > 0.1f && movedDistance < 0.0008f)
    {
        memory.stuckTimer += dt;
    }
    else
    {
        memory.stuckTimer = std::max(0.0f, memory.stuckTimer - dt * 1.8f);
    }
    memory.lastPosition = currentPosition;
}

struct BotMovementPlan
{
    Vector3 wish {};
    Vector3 aimDirection {};
    float fightDistance = 999.0f;
    bool edgePressure = false;
    Player* fightTarget = nullptr;
};

struct BotTraversalPlan
{
    bool jump = false;
    bool consumedByMining = false;
};

PlayerCommand BuildBotMovementCommand(
    const Player& bot,
    std::uint32_t tick,
    Vector3 wish,
    Vector3 aimDirection,
    bool jump,
    bool sprint)
{
    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
    command.tick = tick;
    command.jump = jump;
    command.sprint = sprint;
    command.selectedSlot = 0;

    Vector3 commandAim = Normalize2D(aimDirection);
    if (Length2D(commandAim) <= 0.0001f)
    {
        commandAim = Normalize2D(wish);
    }
    command.aimYaw = Length2D(commandAim) > 0.0001f
        ? YawFromDirection(commandAim)
        : bot.GetYaw();

    const float sinYaw = std::sin(command.aimYaw);
    const float cosYaw = std::cos(command.aimYaw);
    const Vector3 forward { sinYaw, 0.0f, -cosYaw };
    const Vector3 right { cosYaw, 0.0f, sinYaw };
    command.moveForward = wish.x * forward.x + wish.z * forward.z;
    command.moveStrafe = wish.x * right.x + wish.z * right.z;
    return command;
}

template <typename IsVoidThreatFn, typename TryBridgeFn>
BotMovementPlan BuildBotMovementPlan(
    Player& bot,
    BotMemory& memory,
    BotDifficulty difficulty,
    float matchTime,
    Vector3 coreHome,
    Vector3 target,
    BotState state,
    BotIntent intent,
    Player* decisionFightTarget,
    IsVoidThreatFn&& isVoidThreat,
    TryBridgeFn&& tryBridge)
{
    const Vector3 botPos = bot.GetPosition();
    BotMovementPlan plan {};
    plan.fightTarget = state == BotState::Fight ? decisionFightTarget : nullptr;
    plan.wish = Normalize2D(Vector3 { target.x - botPos.x, 0.0f, target.z - botPos.z });
    plan.aimDirection = plan.wish;
    plan.fightDistance = plan.fightTarget != nullptr
        ? std::sqrt(DistanceSquared(botPos, plan.fightTarget->GetPosition()))
        : 999.0f;

    if (plan.fightTarget != nullptr)
    {
        const Vector3 toEnemy = Normalize2D(Vector3 {
            plan.fightTarget->GetPosition().x - botPos.x,
            0.0f,
            plan.fightTarget->GetPosition().z - botPos.z
        });
        const Vector3 side { -toEnemy.z, 0.0f, toEnemy.x };
        if (memory.strafeTimer <= 0.0f)
        {
            memory.strafeSign = -memory.strafeSign;
            memory.strafeTimer = difficulty == BotDifficulty::Hard ? 0.42f : (difficulty == BotDifficulty::Easy ? 0.78f : 0.56f);
        }

        const float desiredRange = difficulty == BotDifficulty::Hard ? 2.22f : (difficulty == BotDifficulty::Easy ? 1.82f : 2.05f);
        const bool shouldKite = bot.GetHealth() < plan.fightTarget->GetHealth() - 18
            && intent != BotIntent::DefendCore
            && intent != BotIntent::ChaseWeakEnemy;
        const float forwardAmount = shouldKite
            ? (plan.fightDistance < 4.2f ? -0.55f : 0.10f)
            : (plan.fightDistance > desiredRange + 0.35f
            ? 1.0f
            : (plan.fightDistance < desiredRange - 0.35f ? -0.55f : 0.16f));
        const bool highGroundRisk = isVoidThreat(Vector3 { botPos.x + side.x * memory.strafeSign * 0.90f, botPos.y, botPos.z + side.z * memory.strafeSign * 0.90f });
        const float strafeScale = highGroundRisk ? 0.18f : 1.0f;
        const float strafeAmount = static_cast<float>(memory.strafeSign)
            * (difficulty == BotDifficulty::Hard ? 0.84f : (difficulty == BotDifficulty::Easy ? 0.32f : 0.58f))
            * strafeScale;
        plan.wish = Normalize2D(Vector3 {
            toEnemy.x * forwardAmount + side.x * strafeAmount,
            0.0f,
            toEnemy.z * forwardAmount + side.z * strafeAmount
        });

        float aimError = difficulty == BotDifficulty::Easy ? 0.58f : (difficulty == BotDifficulty::Hard ? 0.24f : 0.38f);
        if (plan.fightDistance < 2.55f || plan.fightTarget->GetHealth() <= bot.GetHealth() - 20)
        {
            aimError *= difficulty == BotDifficulty::Easy ? 0.82f : 0.62f;
        }
        const float aimWave = std::sin(matchTime * (difficulty == BotDifficulty::Hard ? 5.1f : 3.7f) + static_cast<float>(bot.GetId()) * 1.73f);
        plan.aimDirection = Normalize2D(Vector3 {
            toEnemy.x + side.x * aimError * (static_cast<float>(memory.strafeSign) * 0.55f + aimWave * 0.45f),
            0.0f,
            toEnemy.z + side.z * aimError * (static_cast<float>(memory.strafeSign) * 0.55f + aimWave * 0.45f)
        });
    }

    const Vector3 nextStep {
        botPos.x + plan.wish.x * 0.95f,
        botPos.y,
        botPos.z + plan.wish.z * 0.95f
    };
    const Vector3 wishSide { -plan.wish.z, 0.0f, plan.wish.x };
    const bool voidAhead = isVoidThreat(nextStep);
    const bool voidLeft = Length2D(plan.wish) > 0.0001f
        && isVoidThreat(Vector3 { botPos.x + plan.wish.x * 0.35f + wishSide.x * 0.78f, botPos.y, botPos.z + plan.wish.z * 0.35f + wishSide.z * 0.78f });
    const bool voidRight = Length2D(plan.wish) > 0.0001f
        && isVoidThreat(Vector3 { botPos.x + plan.wish.x * 0.35f - wishSide.x * 0.78f, botPos.y, botPos.z + plan.wish.z * 0.35f - wishSide.z * 0.78f });
    const bool currentVoid = isVoidThreat(botPos);
    plan.edgePressure = voidAhead || currentVoid;

    bool placedVoidBlock = false;
    if (voidAhead && bot.GetInventory().GetBlocks() > 0)
    {
        placedVoidBlock = tryBridge(target);
    }
    Vector3 safetyWish = Normalize2D(Vector3 { coreHome.x - botPos.x, 0.0f, coreHome.z - botPos.z });
    if (Length2D(safetyWish) <= 0.0001f)
    {
        safetyWish = Normalize2D(Vector3 { -botPos.x, 0.0f, -botPos.z });
    }
    if (voidAhead && plan.fightTarget != nullptr && !placedVoidBlock)
    {
        if (bot.GetInventory().GetBlocks() > 0)
        {
            plan.wish = Vector3 {};
            tryBridge(Vector3 { botPos.x + safetyWish.x * 2.0f, botPos.y, botPos.z + safetyWish.z * 2.0f });
        }
        else
        {
            plan.wish = safetyWish;
        }
    }
    else if (plan.fightTarget != nullptr && !voidAhead)
    {
        if (voidLeft && !voidRight)
        {
            plan.wish = Normalize2D(Vector3 { plan.wish.x - wishSide.x * 0.45f, 0.0f, plan.wish.z - wishSide.z * 0.45f });
        }
        else if (voidRight && !voidLeft)
        {
            plan.wish = Normalize2D(Vector3 { plan.wish.x + wishSide.x * 0.45f, 0.0f, plan.wish.z + wishSide.z * 0.45f });
        }
    }
    else if (voidAhead && bot.GetInventory().GetBlocks() <= 0)
    {
        plan.wish = safetyWish;
    }
    else if (voidAhead && !placedVoidBlock)
    {
        plan.wish = bot.GetInventory().GetBlocks() > 0
            ? Vector3 {}
            : Vector3 { safetyWish.x * 0.45f, 0.0f, safetyWish.z * 0.45f };
    }

    const bool shouldBridge = state == BotState::Bridge
        || state == BotState::AttackCore
        || state == BotState::BreakDefense
        || state == BotState::Collect
        || state == BotState::Retreat
        || intent == BotIntent::GearUp
        || intent == BotIntent::Recover
        || memory.stuckTimer > 0.35f
        || plan.edgePressure;
    if (shouldBridge)
    {
        tryBridge(target);
    }

    return plan;
}

template <typename TryBreakBlockingFn>
BotTraversalPlan BuildBotTraversalPlan(
    Player& bot,
    BotMemory& memory,
    BotDifficulty difficulty,
    Player* fightTarget,
    float fightDistance,
    Vector3 nextStep,
    Vector3 wish,
    float dt,
    const World& world,
    TryBreakBlockingFn&& tryBreakBlocking)
{
    BotTraversalPlan plan {};
    const GridPos stepBlock = world.WorldToGrid(Vector3 { nextStep.x, bot.GetPosition().y + 0.04f, nextStep.z });
    const GridPos stepHead { stepBlock.x, stepBlock.y + 1, stepBlock.z };
    const GridPos stepSupport = world.WorldToGrid(Vector3 { nextStep.x, bot.GetPosition().y - 1.08f, nextStep.z });
    const bool oneBlockObstacle = world.IsSolid(stepBlock)
        && world.IsAir(stepHead)
        && stepBlock.y > stepSupport.y;
    const bool shouldMineObstacle = memory.state == BotState::AttackCore
        || memory.state == BotState::BreakDefense
        || memory.state == BotState::Bridge
        || memory.intent == BotIntent::FightEnemy
        || memory.intent == BotIntent::ChaseWeakEnemy
        || memory.intent == BotIntent::Recover
        || memory.stuckTimer > 0.20f;
    if (shouldMineObstacle && tryBreakBlocking(wish, dt))
    {
        plan.consumedByMining = true;
        return plan;
    }

    if (memory.jumpTimer <= 0.0f
        && (oneBlockObstacle
            || memory.stuckTimer > 0.28f
            || (fightTarget != nullptr && fightDistance > 1.8f && fightDistance < 2.8f && difficulty == BotDifficulty::Hard && bot.IsOnGround())))
    {
        plan.jump = true;
        memory.jumpTimer = oneBlockObstacle
            ? (difficulty == BotDifficulty::Hard ? 0.38f : 0.52f)
            : (difficulty == BotDifficulty::Hard ? 0.82f : 1.2f);
    }

    return plan;
}

template <typename RegisterCombatEventFn>
void TryPerformBotMeleeAttack(
    Player& bot,
    BotMemory& memory,
    BotDifficulty difficulty,
    float matchTime,
    bool sprint,
    Vector3 aimDirection,
    Player* fightTarget,
    CombatSystem& combat,
    World& world,
    std::vector<Player>& players,
    RegisterCombatEventFn&& registerCombatEvent)
{
    if (fightTarget == nullptr
        || memory.attackTimer > 0.0f
        || memory.stateTimer < BotFightReactionDelay(difficulty))
    {
        return;
    }

    std::string combatMessage;
    CombatEvent combatEvent;
    WeaponType botWeapon = WeaponType::Sword;
    const auto isVoidThreat = [&world](Vector3 position)
    {
        const GridPos underCenter = world.WorldToGrid(Vector3 { position.x, position.y - 1.08f, position.z });
        if (!world.IsAir(underCenter))
        {
            return false;
        }
        const GridPos lowerCenter = world.WorldToGrid(Vector3 { position.x, position.y - 1.86f, position.z });
        return world.IsAir(lowerCenter);
    };
    const float fightDistance = std::sqrt(DistanceSquared(bot.GetPosition(), fightTarget->GetPosition()));
    const bool enemyNearVoid = isVoidThreat(fightTarget->GetPosition())
        || isVoidThreat(Vector3 { fightTarget->GetPosition().x + aimDirection.x * 0.6f, fightTarget->GetPosition().y, fightTarget->GetPosition().z + aimDirection.z * 0.6f });
    const int nearbyEnemyCount = static_cast<int>(std::count_if(
        players.begin(),
        players.end(),
        [&bot, fightTarget](const Player& other)
        {
            return other.GetId() != fightTarget->GetId()
                && other.GetTeamId() != bot.GetTeamId()
                && other.IsAlive()
                && !other.IsEliminated()
                && DistanceSquared(other.GetPosition(), fightTarget->GetPosition()) < 4.0f;
        }));
    const bool axeMoment = bot.GetInventory().HasItem(ItemType::Axe)
        && (memory.intent == BotIntent::DefendCore
            || memory.intent == BotIntent::BreakCoreDefense
            || nearbyEnemyCount > 0
            || fightTarget->HasShield()
            || fightTarget->GetInventory().GetArmorLevel() >= 2);
    const bool spearMoment = bot.GetInventory().HasItem(ItemType::Spear)
        && !axeMoment
        && (enemyNearVoid
            || fightDistance > 2.55f
            || bot.GetHealth() < fightTarget->GetHealth() - 10
            || memory.role == BotRole::Collector);
    if (axeMoment)
    {
        botWeapon = WeaponType::Axe;
    }
    else if (spearMoment)
    {
        botWeapon = WeaponType::Spear;
    }

    AttackOptions attackOptions {};
    attackOptions.sprinting = sprint;
    attackOptions.sprintReset = sprint
        && difficulty != BotDifficulty::Easy
        && std::fmod(matchTime + static_cast<float>(bot.GetId()) * 0.37f, difficulty == BotDifficulty::Hard ? 1.20f : 1.80f) < 0.22f;
    attackOptions.attackerAirborne = !bot.IsOnGround();
    attackOptions.attackerVelocity = bot.GetVelocity();
    float botMeleeRayLimit = CombatSystem::AttackRange(botWeapon, bot.GetInventory().GetSwordLevel());
    const Vector3 botEye {
        bot.GetPosition().x,
        bot.GetPosition().y + 0.78f,
        bot.GetPosition().z
    };
    const std::optional<RaycastHit> botTerrainHit = world.Raycast(botEye, aimDirection, botMeleeRayLimit);
    if (botTerrainHit.has_value())
    {
        botMeleeRayLimit = std::max(0.0f, botTerrainHit->distance - 0.06f);
    }

    if (combat.Attack(bot, players, aimDirection, combatMessage, &combatEvent, botWeapon, 1.0f, &attackOptions, botMeleeRayLimit))
    {
        registerCombatEvent(combatEvent, combatMessage);
        memory.attackTimer = BotAttackDelay(difficulty);
    }
    else
    {
        memory.attackTimer = BotAttackDelay(difficulty) * 0.85f;
    }
}

template <typename TryBreakDefenseFn, typename HasCoreAccessFn, typename OnCoreDestroyedFn, typename RegisterCombatEventFn>
bool TryPerformBotCoreAssault(
    Player& bot,
    BotMemory& memory,
    EnergyCore* enemyCore,
    float dt,
    World& world,
    CombatSystem& combat,
    TryBreakDefenseFn&& tryBreakDefense,
    HasCoreAccessFn&& hasCoreAccess,
    OnCoreDestroyedFn&& onCoreDestroyed,
    RegisterCombatEventFn&& registerCombatEvent)
{
    if (enemyCore == nullptr || !enemyCore->IsAlive())
    {
        return false;
    }

    const Vector3 corePos = world.GridToWorld(enemyCore->GetBlockPosition());
    const Vector3 toCore {
        corePos.x - bot.GetPosition().x,
        0.0f,
        corePos.z - bot.GetPosition().z
    };
    if (toCore.x * toCore.x + toCore.z * toCore.z >= 18.0f)
    {
        return false;
    }

    if (tryBreakDefense(bot, *enemyCore, dt))
    {
        if (memory.breakProgress >= 0.16f)
        {
            std::string coreMessage;
            CombatEvent coreEvent;
            if (combat.DamageCore(bot, *enemyCore, coreMessage, &coreEvent))
            {
                memory.lastAttackedCoreTeamId = enemyCore->GetTeamId();
                if (!enemyCore->IsAlive())
                {
                    onCoreDestroyed(*enemyCore);
                }
                registerCombatEvent(coreEvent, coreMessage);
            }
        }
        return true;
    }
    if (!hasCoreAccess(bot, *enemyCore))
    {
        return true;
    }

    std::string coreMessage;
    CombatEvent coreEvent;
    if (combat.DamageCore(bot, *enemyCore, coreMessage, &coreEvent))
    {
        memory.lastAttackedCoreTeamId = enemyCore->GetTeamId();
        if (!enemyCore->IsAlive())
        {
            onCoreDestroyed(*enemyCore);
        }
        registerCombatEvent(coreEvent, coreMessage);
    }
    return false;
}
}

struct Game::BotTeamFrameContext
{
    int teamId = -1;
    Team* team = nullptr;
    Vector3 coreHome {};
    std::vector<Player*> aliveAllies;
    std::vector<Player*> aliveEnemies;
    BotTeamSnapshot teamPlan {};
    BotRoleDistribution roleDistribution {};
    BotStrategicPlan strategicPlan {};
    Player* enemyAtCore = nullptr;
    float enemyAtCoreDistance = std::numeric_limits<float>::max();
    bool coreNeedsRepair = false;
    bool coreDefenseCritical = false;
    int missingDefenseBlocks = 0;
    int weakDefenseBlocks = 0;
};

struct Game::BotFrameContext
{
    std::vector<Player*> alivePlayers;
    std::unordered_map<int, BotTeamFrameContext> teamContexts;
};

bool Game::BotUseUtility(Player& bot, Team& team, Player* enemy, EnergyCore* enemyCore, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
    const BotTuningGenome& botTuning = BotTuningForTeam(team.id);
    if (memory.utilityTimer > 0.0f)
    {
        return false;
    }
    // Easy bots still defend themselves (heal, retreat tools) but skip the
    // offensive utility play to keep the difficulty gap.
    const bool offensiveUtilities = botDifficulty_ != BotDifficulty::Easy;
    const float cooldownScale = botDifficulty_ == BotDifficulty::Hard
        ? 0.7f
        : (botDifficulty_ == BotDifficulty::Easy ? 1.6f : 1.0f);
    const auto armUtilityCooldown = [&memory, cooldownScale](float seconds)
    {
        memory.utilityTimer = seconds * cooldownScale;
    };

    if (bot.GetHealth() <= 45 && SpendUtilityItem(bot, UtilityType::Heal))
    {
        bot.Heal(45);
        AddWorldEffect(bot.GetPosition(), Color { 128, 238, 166, 255 }, 0.30f, 0.30f);
        armUtilityCooldown(1.3f);
        return true;
    }
    // Mid-combat heals are deliberately slow: spamming them made bot fights
    // unkillable healing wars that stalled entire matches.
    if (enemy != nullptr && bot.GetHealth() <= 55 && SpendUtilityItem(bot, UtilityType::Heal))
    {
        bot.Heal(45);
        AddWorldEffect(bot.GetPosition(), Color { 128, 238, 166, 255 }, 0.24f, 0.24f);
        armUtilityCooldown(3.0f);
        return true;
    }

    if (team.coreAlive
        && bot.GetInventory().GetUtility(UtilityType::HomeTeleport) > 0
        && memory.intent == BotIntent::RetreatHome
        && (bot.GetHealth() < 42 || memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_, botTuning) + 10)
        && DistanceSquared(bot.GetPosition(), team.spawnPoint) > 260.0f
        && SpendUtilityItem(bot, UtilityType::HomeTeleport))
    {
        bot.RespawnAtHome();
        memory.hasNavWaypoint = false;
        memory.stuckTimer = 0.0f;
        AddWorldEffect(bot.GetHomeSpawnPoint(), GetTeamColor(team.color), 0.38f, 0.42f);
        AddFloatingText("home", bot.GetHomeSpawnPoint(), GetTeamColor(team.color));
        armUtilityCooldown(2.6f);
        return true;
    }

    if (memory.role == BotRole::Defender
        && team.coreAlive
        && bot.GetInventory().GetUtility(UtilityType::AlarmTrap) > 0
        && alarmTraps_.end() == std::find_if(
            alarmTraps_.begin(),
            alarmTraps_.end(),
            [&team](const AlarmTrap& trap)
            {
                return trap.ownerTeamId == team.id && !trap.triggered;
            })
        && SpendUtilityItem(bot, UtilityType::AlarmTrap))
    {
        alarmTraps_.push_back(AlarmTrap { team.spawnPoint, bot.GetTeamId(), 5.2f, false });
        AddEventMessage(bot.GetName() + " поставил тревогу на базе", GetTeamColor(team.color), 1.5f);
        armUtilityCooldown(1.5f);
        return true;
    }

    if (enemy != nullptr && offensiveUtilities)
    {
        const Vector3 toEnemy {
            enemy->GetPosition().x - bot.GetPosition().x,
            enemy->GetPosition().y + 0.35f - bot.GetPosition().y,
            enemy->GetPosition().z - bot.GetPosition().z
        };
        const float distance = std::sqrt(DistanceSquared(bot.GetPosition(), enemy->GetPosition()));

        if (distance > 5.0f
            && distance < 19.0f
            && (bot.GetInventory().HasItem(ItemType::Blaster)
                || bot.GetInventory().HasItem(ItemType::SniperRifle))
            && (memory.role == BotRole::Fighter
                || memory.role == BotRole::Defender
                || memory.intent == BotIntent::DefendCore))
        {
            const bool aimed = distance > 8.0f;
            const float loadTime = BlasterChargeSeconds(bot.GetInventory().GetBlasterRapidFireLevel());
            if (bot.GetBlasterState() == CrossbowState::Unloaded)
            {
                bot.StartBlasterLoading();
                AddFloatingText("бластер: зарядка", bot.GetPosition(), Color { 255, 96, 72, 255 });
                return true;
            }
            if (bot.GetBlasterState() == CrossbowState::Loading)
            {
                if (bot.AdvanceBlasterLoading(dt, loadTime))
                {
                    AddFloatingText("бластер: готов", bot.GetPosition(), Color { 104, 255, 128, 255 });
                }
                return true;
            }
            if (LaunchBlasterShot(bot, toEnemy, aimed, false))
            {
                AddFloatingText(aimed ? "бластер: прицел" : "бластер", bot.GetPosition(), Color { 98, 245, 255, 255 });
                return true;
            }
        }

        if (distance > 4.2f
            && distance < 11.0f
            && bot.GetInventory().HasItem(ItemType::Bow)
            && bot.GetInventory().GetUtility(UtilityType::Arrows) > 0
            && (memory.role == BotRole::Fighter || memory.role == BotRole::Rusher || botDifficulty_ == BotDifficulty::Hard))
        {
            const float drawPower = distance > 7.0f ? 1.0f : 0.72f;
            if (LaunchBowShot(bot, toEnemy, drawPower, false))
            {
                AddFloatingText(drawPower >= 1.0f ? "лук: полный" : "лук", bot.GetPosition(), Color { 112, 232, 255, 255 });
                armUtilityCooldown(kBowTuning.fullDrawTime * drawPower + 0.25f);
                return true;
            }
        }

        const bool enemyNearVoid = IsVoidThreatAt(enemy->GetPosition())
            || IsVoidThreatAt(Vector3 { enemy->GetPosition().x + toEnemy.x * 0.18f, enemy->GetPosition().y, enemy->GetPosition().z + toEnemy.z * 0.18f });
        const bool fireballDuel = distance > 7.0f
            || enemyNearVoid
            || (memory.intent == BotIntent::DefendCore && DistanceSquared(enemy->GetPosition(), team.spawnPoint) < 70.0f);
        if (fireballDuel
            && distance > 4.5f
            && distance < 14.0f
            && bot.CanAttack()
            && bot.GetInventory().GetUtility(UtilityType::Fireball) > 0)
        {
            if (SpendUtilityItem(bot, UtilityType::Fireball))
            {
                LaunchProjectileDirected(bot, UtilityType::Fireball, toEnemy, false);
                AddFloatingText("fireball", bot.GetPosition(), Color { 255, 178, 96, 255 });
                armUtilityCooldown(1.9f);
                return true;
            }
        }

        const bool molotovFight = bot.GetInventory().GetUtility(UtilityType::Molotov) > 0
            && distance > 5.0f
            && distance < 12.0f
            && (memory.intent == BotIntent::DefendCore
                || memory.intent == BotIntent::FightEnemy
                || memory.intent == BotIntent::BreakCoreDefense);
        if (molotovFight && SpendUtilityItem(bot, UtilityType::Molotov))
        {
            LaunchProjectileDirected(bot, UtilityType::Molotov, toEnemy, false);
            AddFloatingText("molotov", bot.GetPosition(), Color { 255, 128, 72, 255 });
            armUtilityCooldown(2.2f);
            return true;
        }

        const bool dashCommit = memory.role == BotRole::Fighter
            || memory.intent == BotIntent::ChaseWeakEnemy
            || (memory.role == BotRole::Rusher && enemy->GetHealth() <= bot.GetHealth() - 16);
        if (distance > 3.8f && distance < 9.0f && dashCommit && bot.GetInventory().GetUtility(UtilityType::Dash) > 0)
        {
            if (SpendUtilityItem(bot, UtilityType::Dash))
            {
                const Vector3 dash = Normalize2D(toEnemy);
                bot.ApplyKnockback(Vector3 { dash.x * 7.2f, 1.1f, dash.z * 7.2f });
                AddWorldEffect(bot.GetPosition(), Color { 112, 232, 255, 255 }, 0.28f, 0.28f);
                armUtilityCooldown(2.6f);
                return true;
            }
        }
    }

    if (enemyCore != nullptr
        && offensiveUtilities
        && enemyCore->IsAlive()
        && memory.role == BotRole::Rusher
        && bot.CanAttack()
        && bot.GetInventory().GetUtility(UtilityType::Fireball) > 0)
    {
        const Vector3 corePos = world_.GridToWorld(enemyCore->GetBlockPosition());
        const float coreDistance = std::sqrt(DistanceSquared(bot.GetPosition(), corePos));
        if (coreDistance > 5.0f && coreDistance < 16.0f)
        {
            const std::optional<GridPos> defense = FindCoreDefenseBlock(*enemyCore, bot);
            if (defense.has_value()
                && bot.GetInventory().GetUtility(UtilityType::Molotov) > 0
                && coreDistance < 12.0f
                && SpendUtilityItem(bot, UtilityType::Molotov))
            {
                const Vector3 defensePos = world_.GridToWorld(*defense);
                LaunchProjectileDirected(
                    bot,
                    UtilityType::Molotov,
                    Vector3 {
                        defensePos.x - bot.GetPosition().x,
                        defensePos.y + 0.35f - bot.GetPosition().y,
                        defensePos.z - bot.GetPosition().z },
                    false);
                AddFloatingText("molotov", bot.GetPosition(), Color { 255, 128, 72, 255 });
                armUtilityCooldown(2.2f);
                return true;
            }
            if (defense.has_value() && SpendUtilityItem(bot, UtilityType::Fireball))
            {
                const Vector3 defensePos = world_.GridToWorld(*defense);
                LaunchProjectileDirected(
                    bot,
                    UtilityType::Fireball,
                    Vector3 {
                        defensePos.x - bot.GetPosition().x,
                        defensePos.y + 0.35f - bot.GetPosition().y,
                        defensePos.z - bot.GetPosition().z },
                    false);
                AddFloatingText("fireball", bot.GetPosition(), Color { 255, 178, 96, 255 });
                armUtilityCooldown(1.9f);
                return true;
            }
        }
    }

    return false;
}

bool Game::BotCastHeroAbility(Player& bot, HeroAbilitySlot slot)
{
    // Keep local HUD messages quiet, but preserve world VFX and cast SFX so
    // opponents can read and react to a bot's ability.
    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
    command.tick = matchSimulation_.CurrentTick();
    command.aimYaw = bot.GetYaw();
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        command.useAbility1 = true;
        break;
    case HeroAbilitySlot::Active2:
        command.useAbility2 = true;
        break;
    case HeroAbilitySlot::Ultimate:
        command.useUltimate = true;
        break;
    }

    const bool previousSuppress = suppressLocalFeedback_;
    suppressLocalFeedback_ = true;
    const bool used = ApplyPlayerActionCommand(bot, command);
    suppressLocalFeedback_ = previousSuppress;
    return used;
}

bool Game::BotUseHeroAbility(Player& bot, Team& team, Player* enemy, EnergyCore* enemyCore)
{
    BotMemory& memory = GetBotMemory(bot);
    if (memory.heroAbilityTimer > 0.0f)
    {
        return false;
    }

    const float retryDelay = 0.6f;
    const float castDelay = botDifficulty_ == BotDifficulty::Hard
        ? 1.1f
        : (botDifficulty_ == BotDifficulty::Easy ? 3.2f : 1.8f);
    const auto cast = [&](HeroAbilitySlot slot, Vector3 faceTarget, bool face)
    {
        if (face)
        {
            const Vector3 faceDirection {
                faceTarget.x - bot.GetPosition().x,
                0.0f,
                faceTarget.z - bot.GetPosition().z
            };
            const PlayerCommand aimCommand = BuildBotMovementCommand(
                bot,
                matchSimulation_.CurrentTick(),
                Vector3 {},
                faceDirection,
                false,
                false);
            ApplyPlayerCommand(bot, aimCommand, 0.0f);
        }
        const bool used = BotCastHeroAbility(bot, slot);
        memory.heroAbilityTimer = used ? castDelay : retryDelay;
        return used;
    };

    const HeroRuntimeState& heroState = bot.GetHeroState();
    const Inventory& inventory = bot.GetInventory();
    const float enemyDistance = enemy != nullptr
        ? std::sqrt(DistanceSquared(bot.GetPosition(), enemy->GetPosition()))
        : 999.0f;
    const bool fightingIntent = memory.intent == BotIntent::FightEnemy
        || memory.intent == BotIntent::DefendCore
        || memory.intent == BotIntent::ChaseWeakEnemy;
    const bool defendingBase = memory.intent == BotIntent::DefendCore
        || memory.intent == BotIntent::RepairCoreDefense;
    const bool assaultIntent = memory.intent == BotIntent::PressureCore
        || memory.intent == BotIntent::BreakCoreDefense;
    const float distanceFromHomeSq = DistanceSquared(bot.GetPosition(), team.spawnPoint);

    switch (bot.GetHeroId())
    {
    case HeroId::Radon:
    {
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate))
        {
            EnergyCore* ownCore = FindCoreByTeam(team.id);
            const bool coreAlive = ownCore != nullptr && ownCore->IsAlive();
            // Prime the sacrifice only when the base is in real danger; the
            // primed state is a toggle, so never re-cast while primed.
            const bool coreInDanger = coreAlive
                && (memory.coreDefenseCritical
                    || ownCore->GetHealth() <= ownCore->GetMaxHealth() / 3
                    || (enemy != nullptr
                        && DistanceSquared(enemy->GetPosition(), world_.GridToWorld(ownCore->GetBlockPosition())) < 60.0f));
            if (coreAlive && coreInDanger && !heroState.ultimatePrimed)
            {
                return cast(HeroAbilitySlot::Ultimate, Vector3 {}, false);
            }
            if (!coreAlive && enemy != nullptr && enemyDistance < 4.5f)
            {
                return cast(HeroAbilitySlot::Ultimate, enemy->GetPosition(), true);
            }
        }
        // Force pulse: shove a close attacker away, extra value near the void.
        if (enemy != nullptr
            && enemyDistance < 4.0f
            && bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && (defendingBase
                || bot.GetHealth() <= enemy->GetHealth() + 12
                || IsVoidThreatAt(enemy->GetPosition())))
        {
            return cast(HeroAbilitySlot::Active1, enemy->GetPosition(), true);
        }
        // Molotov zones a mid-range target while fighting or sieging.
        if (enemy != nullptr
            && enemyDistance > 4.5f
            && enemyDistance < 12.0f
            && (fightingIntent || assaultIntent)
            && bot.IsHeroAbilityReady(HeroAbilitySlot::Active2))
        {
            return cast(HeroAbilitySlot::Active2, enemy->GetPosition(), true);
        }
        break;
    }
    case HeroId::Orbita:
    {
        // Escape teleport: hurt and cornered. The teleport costs health, so
        // skip it when nearly dead.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && enemy != nullptr
            && enemyDistance < 6.0f
            && bot.GetHealth() < 40
            && bot.GetHealth() > 18)
        {
            return cast(
                HeroAbilitySlot::Ultimate,
                Vector3 { team.spawnPoint.x, bot.GetPosition().y, team.spawnPoint.z },
                true);
        }
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1) && bot.IsOnGround())
        {
            // Gap-closing dash on runners; this also primes the momentum strike.
            if (enemy != nullptr
                && enemyDistance > 4.5f
                && enemyDistance < 11.0f
                && (memory.intent == BotIntent::ChaseWeakEnemy
                    || (fightingIntent && bot.GetHealth() >= enemy->GetHealth() - 8)))
            {
                return cast(HeroAbilitySlot::Active1, enemy->GetPosition(), true);
            }
            // Disengage dash when retreating with an enemy on top of us.
            if (enemy != nullptr && enemyDistance < 4.0f && memory.intent == BotIntent::RetreatHome)
            {
                const Vector3 away {
                    bot.GetPosition().x * 2.0f - enemy->GetPosition().x,
                    bot.GetPosition().y,
                    bot.GetPosition().z * 2.0f - enemy->GetPosition().z
                };
                return cast(HeroAbilitySlot::Active1, away, true);
            }
        }
        // Phantom bridge: cross a gap toward the objective without real blocks.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2)
            && assaultIntent
            && inventory.GetBlocks() <= 4)
        {
            const Vector3 ahead {
                bot.GetPosition().x + bot.Forward().x * 1.6f,
                bot.GetPosition().y,
                bot.GetPosition().z + bot.Forward().z * 1.6f
            };
            if (IsVoidThreatAt(ahead))
            {
                return cast(HeroAbilitySlot::Active2, Vector3 {}, false);
            }
        }
        break;
    }
    case HeroId::Brom:
    {
        const bool nearHome = distanceFromHomeSq < 900.0f;
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && nearHome
            && (defendingBase || memory.coreDefenseCritical))
        {
            return cast(HeroAbilitySlot::Ultimate, Vector3 {}, false);
        }
        // Turret holds ground while defending or brawling near base. Keep a
        // small gold reserve when not defending.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2)
            && inventory.GetResource(ResourceType::Gold) >= 12 + (defendingBase ? 0 : 8)
            && (defendingBase || (enemy != nullptr && enemyDistance < 9.0f)))
        {
            return cast(HeroAbilitySlot::Active2, Vector3 {}, false);
        }
        // Vacuum bot only with a clear iron surplus and nobody attacking.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && enemy == nullptr
            && inventory.GetResource(ResourceType::Iron) >= 60
            && nearHome)
        {
            return cast(HeroAbilitySlot::Active1, Vector3 {}, false);
        }
        break;
    }
    case HeroId::Konvoy:
    {
        // Containment dome on a committed kill or base defense.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && enemy != nullptr
            && enemyDistance < 5.0f
            && (memory.intent == BotIntent::ChaseWeakEnemy
                || defendingBase
                || enemy->GetHealth() < bot.GetHealth()))
        {
            return cast(HeroAbilitySlot::Ultimate, enemy->GetPosition(), true);
        }
        // Handcuffs tether a kiting enemy.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2)
            && enemy != nullptr
            && enemyDistance < 5.5f
            && fightingIntent)
        {
            return cast(HeroAbilitySlot::Active2, enemy->GetPosition(), true);
        }
        // Defenders seed traps around the base while it is quiet.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && memory.role == BotRole::Defender
            && enemy == nullptr
            && distanceFromHomeSq < 110.0f)
        {
            return cast(HeroAbilitySlot::Active1, Vector3 {}, false);
        }
        break;
    }
    case HeroId::Likho:
    {
        // Silent steps into the backline while sieging or stalking.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && enemy != nullptr
            && enemyDistance < 14.0f
            && (assaultIntent || memory.intent == BotIntent::ChaseWeakEnemy))
        {
            return cast(HeroAbilitySlot::Active1, enemy->GetPosition(), true);
        }
        // Bleed once we are trading hits.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2)
            && enemy != nullptr
            && enemyDistance < 3.5f)
        {
            return cast(HeroAbilitySlot::Active2, enemy->GetPosition(), true);
        }
        // Disguise to slip toward the enemy core.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && assaultIntent
            && enemyCore != nullptr
            && DistanceSquared(bot.GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 900.0f)
        {
            return cast(HeroAbilitySlot::Ultimate, Vector3 {}, false);
        }
        break;
    }
    case HeroId::Svidetel:
    {
        // Echo support when entering a fight.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && enemy != nullptr
            && enemyDistance < 8.0f
            && fightingIntent)
        {
            return cast(HeroAbilitySlot::Active1, enemy->GetPosition(), true);
        }
        // Phase a blocked path open.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2) && memory.stuckTimer > 1.4f)
        {
            return cast(HeroAbilitySlot::Active2, Vector3 {}, false);
        }
        // Void contours to find the last runners.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && (memory.intent == BotIntent::ChaseWeakEnemy || matchSimulation_.MatchTimeSeconds() > 150.0f))
        {
            return cast(HeroAbilitySlot::Ultimate, Vector3 {}, false);
        }
        break;
    }
    }

    return false;
}

void Game::UpdateBots(float dt)
{
    for (TeamCoordinationBus& bus : teamCoordBuses_)
    {
        bus.Prune(matchSimulation_.MatchTimeSeconds(), kCoordinationSignalTtl);
    }
    for (CoreDefenseMonitor& monitor : coreDefenseMonitors_)
    {
        monitor.checkTimer = std::max(0.0f, monitor.checkTimer - dt);
    }

    for (Player& bot : players_)
    {
        if (IsBotControlled(ControlKindForPlayer(bot)) && bot.IsAlive() && !bot.IsEliminated())
        {
            GetBotMemory(bot);
        }
    }

    BotFrameContext frameContext {};
    frameContext.alivePlayers.reserve(players_.size());
    for (Player& player : players_)
    {
        if (player.IsAlive() && !player.IsEliminated())
        {
            frameContext.alivePlayers.push_back(&player);
        }
    }

    frameContext.teamContexts.reserve(teams_.size());
    for (Team& team : teams_)
    {
        BotTeamFrameContext teamContext {};
        teamContext.teamId = team.id;
        teamContext.team = &team;
        teamContext.coreHome = world_.GridToWorld(team.coreBlock);
        if (team.id >= 0 && team.id < static_cast<int>(coreDefenseMonitors_.size()))
        {
            CoreDefenseMonitor& defenseMonitor = coreDefenseMonitors_[team.id];
            if (!team.coreAlive)
            {
                defenseMonitor = CoreDefenseMonitor {};
            }
            else if (defenseMonitor.checkTimer <= 0.0f)
            {
                const CoreDefenseStatus status = InspectCoreDefense(world_, team);
                defenseMonitor.missingBlocks = status.missingBlocks;
                defenseMonitor.weakBlocks = status.weakBlocks;
                defenseMonitor.critical = status.critical;
                defenseMonitor.checkTimer = botDifficulty_ == BotDifficulty::Hard
                    ? 0.85f
                    : (botDifficulty_ == BotDifficulty::Easy ? 1.75f : 1.20f);
            }
            teamContext.coreNeedsRepair = team.coreAlive && defenseMonitor.missingBlocks > 0;
            teamContext.coreDefenseCritical = team.coreAlive && defenseMonitor.critical;
            teamContext.missingDefenseBlocks = defenseMonitor.missingBlocks;
            teamContext.weakDefenseBlocks = defenseMonitor.weakBlocks;
        }
        else
        {
            teamContext.coreNeedsRepair = team.coreAlive && FindMissingCoreDefenseBlock(team).has_value();
        }
        teamContext.aliveAllies.reserve(frameContext.alivePlayers.size());
        teamContext.aliveEnemies.reserve(frameContext.alivePlayers.size());

        for (Player* player : frameContext.alivePlayers)
        {
            if (player == nullptr)
            {
                continue;
            }

            if (player->GetTeamId() == team.id)
            {
                teamContext.aliveAllies.push_back(player);
                ++teamContext.teamPlan.aliveAllies;
                const bool nearCore = DistanceSquared(player->GetPosition(), teamContext.coreHome) < 72.0f;
                if (nearCore)
                {
                    ++teamContext.teamPlan.alliesNearCore;
                }

                if (IsBotControlled(ControlKindForPlayer(*player)))
                {
                    const BotMemory* memory = FindBotMemoryByPlayerId(botMemories_, botMemoryIndexByPlayerId_, player->GetId());
                    const BotRole role = memory != nullptr ? memory->role : RoleForBotId(player->GetId());
                    ++teamContext.roleDistribution.total;
                    switch (role)
                    {
                    case BotRole::Defender:
                        ++teamContext.roleDistribution.defenders;
                        if (nearCore)
                        {
                            ++teamContext.teamPlan.defendersNearCore;
                        }
                        break;
                    case BotRole::Rusher:
                        ++teamContext.roleDistribution.rushers;
                        break;
                    case BotRole::Collector:
                        ++teamContext.roleDistribution.collectors;
                        break;
                    case BotRole::Fighter:
                        ++teamContext.roleDistribution.fighters;
                        break;
                    }

                    if (memory != nullptr)
                    {
                        if (memory->intent == BotIntent::PressureCore
                            || memory->intent == BotIntent::BreakCoreDefense
                            || memory->intent == BotIntent::ChaseWeakEnemy)
                        {
                            ++teamContext.teamPlan.activePressure;
                        }
                        else if (memory->intent == BotIntent::GearUp)
                        {
                            ++teamContext.teamPlan.activeShop;
                        }
                        else if (memory->intent == BotIntent::RepairCoreDefense)
                        {
                            ++teamContext.teamPlan.activeRepair;
                        }
                        else if (memory->intent == BotIntent::SecureResources)
                        {
                            ++teamContext.teamPlan.activeResource;
                        }
                    }
                }
                continue;
            }

            teamContext.aliveEnemies.push_back(player);
            const float coreDistance = DistanceSquared(player->GetPosition(), teamContext.coreHome);
            if (coreDistance < 42.0f && coreDistance < teamContext.enemyAtCoreDistance)
            {
                teamContext.enemyAtCore = player;
                teamContext.enemyAtCoreDistance = coreDistance;
            }
        }

        frameContext.teamContexts.emplace(team.id, std::move(teamContext));
    }

    for (auto& entry : frameContext.teamContexts)
    {
        BotTeamFrameContext& teamContext = entry.second;
        if (teamContext.team == nullptr)
        {
            continue;
        }

        BotStrategicPlan plan {};
        const Team& team = *teamContext.team;
        const BotTuningGenome& teamTuning = BotTuningForTeam(team.id);
        plan.desiredDefenders = teamContext.coreDefenseCritical ? 2 : 1;
        if (team.coreAlive)
        {
            if (teamContext.enemyAtCore != nullptr)
            {
                plan.defenseUrgency += 520.0f;
                plan.desiredDefenders = 2;
            }
            const bool defenseCriticalNow = teamContext.coreDefenseCritical && matchSimulation_.MatchTimeSeconds() > 55.0f;
            if (defenseCriticalNow)
            {
                plan.defenseUrgency += 360.0f;
            }
            if (matchSimulation_.MatchTimeSeconds() > 40.0f || teamContext.enemyAtCore != nullptr)
            {
                plan.defenseUrgency += static_cast<float>(teamContext.missingDefenseBlocks) * 24.0f;
            }
            if (teamContext.teamPlan.defendersNearCore == 0 && (teamContext.coreNeedsRepair || teamContext.enemyAtCore != nullptr))
            {
                plan.defenseUrgency += matchSimulation_.MatchTimeSeconds() > 55.0f || teamContext.enemyAtCore != nullptr ? 140.0f : 0.0f;
            }
        }

        EnergyCore* bestCore = nullptr;
        float bestCoreScore = std::numeric_limits<float>::max();
        int aliveEnemyCores = 0;
        const TeamCoordinationBus* coordBus = team.id >= 0 && team.id < static_cast<int>(teamCoordBuses_.size())
            ? &teamCoordBuses_[team.id]
            : nullptr;
        for (EnergyCore& core : matchSimulation_.Cores())
        {
            if (core.GetTeamId() == team.id || !core.IsAlive())
            {
                continue;
            }

            ++aliveEnemyCores;
            const Vector3 corePos = world_.GridToWorld(core.GetBlockPosition());
            const float distanceFromBase = DistanceSquared(teamContext.coreHome, corePos);
            const float weaknessBonus = static_cast<float>(core.GetMaxHealth() - core.GetHealth()) * 2.4f;
            float defenderPenalty = 0.0f;
            const auto foundTargetContext = frameContext.teamContexts.find(core.GetTeamId());
            if (foundTargetContext != frameContext.teamContexts.end())
            {
                const BotTeamFrameContext& targetContext = foundTargetContext->second;
                defenderPenalty += static_cast<float>(targetContext.teamPlan.defendersNearCore) * 95.0f;
                defenderPenalty += targetContext.coreDefenseCritical ? 120.0f : 0.0f;
            }

            const int coordinatedAttackers = coordBus != nullptr
                ? coordBus->CountSignal(
                    CoordinationSignal::AttackingCore,
                    matchSimulation_.MatchTimeSeconds(),
                    kCoordinationSignalTtl,
                    core.GetTeamId())
                : 0;
            const bool clockwisePressure = !automatch_.active || automatch_.completedRuns % 2 == 0;
            const float neighborBonus = core.GetTeamId() == PreferredNeighborTeam(team.id, clockwisePressure) ? 48.0f : 0.0f;
            const float score = distanceFromBase * 0.38f
                - weaknessBonus
                + defenderPenalty
                + static_cast<float>(coordinatedAttackers) * 88.0f
                - neighborBonus;
            if (score < bestCoreScore)
            {
                bestCoreScore = score;
                bestCore = &core;
            }
        }

        if (bestCore != nullptr)
        {
            plan.attackCoreTeamId = bestCore->GetTeamId();
            plan.allIn = !team.coreAlive || matchSimulation_.MatchTimeSeconds() > teamTuning.allInSeconds || aliveEnemyCores <= 1;
            plan.desiredAttackers = plan.allIn
                ? 4
                : (matchSimulation_.MatchTimeSeconds() > teamTuning.latePressureSeconds ? 3 : (matchSimulation_.MatchTimeSeconds() > teamTuning.pressurePhaseSeconds ? 2 : 1));
            plan.attackUrgency = 220.0f
                + std::max(0.0f, matchSimulation_.MatchTimeSeconds() - 70.0f) * 1.8f
                + static_cast<float>(bestCore->GetMaxHealth() - bestCore->GetHealth()) * 2.0f
                + (plan.allIn ? 220.0f : 0.0f);
        }

        if (!team.coreAlive && bestCore == nullptr)
        {
            plan.focus = BotStrategicFocus::Cleanup;
            plan.desiredAttackers = 4;
            plan.attackUrgency = 520.0f;
            plan.allIn = true;
        }
        else if (teamContext.enemyAtCore != nullptr || plan.defenseUrgency >= 420.0f)
        {
            plan.focus = BotStrategicFocus::Defense;
        }
        else if (bestCore != nullptr && (matchSimulation_.MatchTimeSeconds() > teamTuning.pressurePhaseSeconds || plan.allIn || teamContext.teamPlan.activeResource >= 2))
        {
            plan.focus = BotStrategicFocus::Pressure;
        }
        else
        {
            plan.focus = BotStrategicFocus::Economy;
        }

        teamContext.strategicPlan = plan;
    }

    for (std::size_t botOffset = 0; botOffset < players_.size(); ++botOffset)
    {
        Player& bot = players_[(simulationOrderOffset_ + botOffset) % players_.size()];
        if (!IsBotControlled(ControlKindForPlayer(bot)) || !bot.IsAlive() || bot.IsEliminated())
        {
            continue;
        }
        // A player driven by a remote network client is not AI-controlled — its
        // movement comes from the client's PlayerCommand (Phase 0.1S).
        const auto foundTeamContext = frameContext.teamContexts.find(bot.GetTeamId());
        if (foundTeamContext == frameContext.teamContexts.end() || foundTeamContext->second.team == nullptr)
        {
            continue;
        }

        UpdateSingleBot(bot, *foundTeamContext->second.team, dt, frameContext);
    }
}

void Game::UpdateSingleBot(Player& bot, Team& team, float dt, const BotFrameContext& frameContext)
{
    ScopedProfileTimer decisionProfile(profilingEnabled_, profileDecisionMs_, profileDecisionCalls_);
    BotMemory& memory = GetBotMemory(bot);
    TickBotMemory(memory, dt);
    memory.carriedResourceValue = CarriedResourceValue(bot.GetInventory());
    const BotTuningGenome& botTuning = BotTuningForTeam(team.id);

    const auto foundTeamContext = frameContext.teamContexts.find(team.id);
    const BotTeamFrameContext* teamContext = foundTeamContext != frameContext.teamContexts.end()
        ? &foundTeamContext->second
        : nullptr;
    const std::vector<Player*>& aliveEnemies = teamContext != nullptr
        ? teamContext->aliveEnemies
        : frameContext.alivePlayers;
    const int coordinationBusIndex = team.id >= 0 && team.id < static_cast<int>(teamCoordBuses_.size())
        ? team.id
        : -1;
    TeamCoordinationBus* coordBus = coordinationBusIndex >= 0 ? &teamCoordBuses_[coordinationBusIndex] : nullptr;
    EnergyCore* enemyCore = SelectBestAttackTarget(bot, frameContext, coordBus);
    const BotStrategicPlan strategicPlan = teamContext != nullptr ? teamContext->strategicPlan : BotStrategicPlan {};
    if (strategicPlan.attackCoreTeamId >= 0)
    {
        EnergyCore* plannedCore = FindCoreByTeam(strategicPlan.attackCoreTeamId);
        if (plannedCore != nullptr
            && plannedCore->IsAlive()
            && (enemyCore == nullptr
                || strategicPlan.focus == BotStrategicFocus::Pressure
                || strategicPlan.allIn))
        {
            enemyCore = plannedCore;
        }
    }
    if ((memory.currentPlan.goal == StrategicGoal::BridgePush || memory.currentPlan.goal == StrategicGoal::CoreAssault)
        && memory.currentPlan.targetTeamId >= 0
        && !StrategicPlanExpired(memory.currentPlan))
    {
        EnergyCore* plannedCore = FindCoreByTeam(memory.currentPlan.targetTeamId);
        if (plannedCore != nullptr && plannedCore->IsAlive())
        {
            enemyCore = plannedCore;
        }
    }
    const Vector3 coreHome = teamContext != nullptr ? teamContext->coreHome : world_.GridToWorld(team.coreBlock);
    const Vector3 botPos = bot.GetPosition();
    const float distanceFromHome = DistanceSquared(bot.GetPosition(), coreHome);
    int openingRank = 0;
    for (const Player& candidate : players_)
    {
        if (IsBotControlled(ControlKindForPlayer(candidate))
            && candidate.GetTeamId() == bot.GetTeamId()
            && candidate.GetId() < bot.GetId())
        {
            ++openingRank;
        }
    }
    BotTeamSnapshot teamPlan = teamContext != nullptr
        ? teamContext->teamPlan
        : BuildBotTeamSnapshot(bot, coreHome, players_, botMemories_, botMemoryIndexByPlayerId_);
    if (teamContext != nullptr)
    {
        teamPlan.aliveAllies = std::max(0, teamPlan.aliveAllies - 1);
        const bool selfNearCore = DistanceSquared(bot.GetPosition(), coreHome) < 72.0f;
        if (selfNearCore)
        {
            teamPlan.alliesNearCore = std::max(0, teamPlan.alliesNearCore - 1);
            if (memory.role == BotRole::Defender)
            {
                teamPlan.defendersNearCore = std::max(0, teamPlan.defendersNearCore - 1);
            }
        }
        RemoveIntentFromSnapshot(teamPlan, memory.intent);
    }
    float enemyAtCoreDistance = teamContext != nullptr ? teamContext->enemyAtCoreDistance : std::numeric_limits<float>::max();
    Player* enemyAtCore = teamContext != nullptr ? teamContext->enemyAtCore : FindEnemyNearCore(bot, players_, coreHome, enemyAtCoreDistance);
    const Inventory& inventory = bot.GetInventory();
    const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_, botTuning);
    const bool wantsShop = ShouldBotShop(bot, inventory, team, memory, botDifficulty_, botTuning);
    if (teamContext != nullptr)
    {
        memory.coreDefenseCritical = teamContext->coreDefenseCritical;
        memory.missingDefenseBlocks = teamContext->missingDefenseBlocks;
    }
    else if (!team.coreAlive)
    {
        memory.coreDefenseCritical = false;
        memory.missingDefenseBlocks = 0;
        memory.defenseCheckTimer = 0.0f;
    }
    else if (memory.defenseCheckTimer <= 0.0f)
    {
        const CoreDefenseStatus status = InspectCoreDefense(world_, team);
        memory.coreDefenseCritical = status.critical;
        memory.missingDefenseBlocks = status.missingBlocks;
        memory.defenseCheckTimer = botDifficulty_ == BotDifficulty::Hard
            ? 0.85f
            : (botDifficulty_ == BotDifficulty::Easy ? 1.75f : 1.20f);
    }
    const bool coreDefenseCritical = team.coreAlive && memory.coreDefenseCritical;
    const int missingDefenseBlocks = team.coreAlive ? memory.missingDefenseBlocks : 0;
    const bool coreNeedsRepair = team.coreAlive
        && ((teamContext != nullptr ? teamContext->coreNeedsRepair : missingDefenseBlocks > 0)
            || coreDefenseCritical);
    BotRoleDistribution roleDistribution = teamContext != nullptr
        ? teamContext->roleDistribution
        : BuildRoleDistributionForTeam(team.id, players_, botMemories_, botMemoryIndexByPlayerId_, bot.GetId());
    if (teamContext != nullptr)
    {
        RemoveRoleFromDistribution(roleDistribution, memory.role);
    }
    const BotRoleDecision roleDecision = EvaluateDynamicRoleDecision(
        bot,
        team,
        memory,
        roleDistribution,
        carryingLoot,
        wantsShop,
        enemyAtCore,
        enemyCore,
        matchSimulation_.MatchTimeSeconds(),
        inventory);
    if (TryApplyDynamicRoleDecision(memory, roleDecision, botDifficulty_, botTuning))
    {
        AddEventMessage(bot.GetName() + ": роль -> " + ToString(memory.role), GetTeamColor(team.color), 1.2f);
    }

    ScopedProfileTimer perceptionProfile(profilingEnabled_, profilePerceptionMs_, profilePerceptionCalls_);
    const auto findNearbyEnemyFromSnapshot = [&](float maxDistance) -> Player*
    {
        Player* best = nullptr;
        float bestScore = std::numeric_limits<float>::max();
        const float maxDistanceSq = maxDistance * maxDistance;

        for (Player* enemy : aliveEnemies)
        {
            if (enemy == nullptr
                || enemy->GetId() == bot.GetId()
                || enemy->GetTeamId() == bot.GetTeamId()
                || !enemy->IsAlive()
                || enemy->IsEliminated())
            {
                continue;
            }

            const float distance = DistanceSquared(bot.GetPosition(), enemy->GetPosition());
            if (distance > maxDistanceSq)
            {
                continue;
            }

            float score = distance;
            score -= static_cast<float>(std::max(0, bot.GetHealth() - enemy->GetHealth())) * 0.95f;
            const Team* enemyTeam = FindTeam(enemy->GetTeamId());
            if (enemyTeam != nullptr && !enemyTeam->coreAlive)
            {
                score -= 420.0f;
                if (enemy->GetHealth() <= bot.GetHealth() + 12)
                {
                    score -= 180.0f;
                }
            }
            if (memory.role == BotRole::Defender)
            {
                score += DistanceSquared(enemy->GetPosition(), coreHome) * 0.24f;
            }
            if (memory.role == BotRole::Collector && enemy->GetHealth() > bot.GetHealth() + 12)
            {
                score += 24.0f;
            }
            if (score < bestScore)
            {
                bestScore = score;
                best = enemy;
            }
        }

        return best;
    };

    Player* nearbyEnemy = findNearbyEnemyFromSnapshot(BotEngageRange(memory.role, botDifficulty_, botTuning));
    // Once no cores remain anywhere the whole map is the hunting ground:
    // corner bases sit farther apart than the normal hunt radius, and a bot
    // that cannot "see" anyone will idle at home until sudden death kills it.
    const bool allCoresGone = !team.coreAlive && (enemyCore == nullptr || !enemyCore->IsAlive());
    const float huntRange = allCoresGone
        ? 100000.0f
        : (matchSimulation_.MatchTimeSeconds() > 120.0f
            ? (botDifficulty_ == BotDifficulty::Hard ? 168.0f : 142.0f)
            : (botDifficulty_ == BotDifficulty::Hard ? 128.0f : 108.0f));
    Player* huntEnemy = findNearbyEnemyFromSnapshot(huntRange);
    Player* finalLifeEnemy = nullptr;
    float finalLifeEnemyDistanceSq = std::numeric_limits<float>::max();
    for (Player* candidate : aliveEnemies)
    {
        if (candidate == nullptr
            || candidate->GetId() == bot.GetId()
            || candidate->GetTeamId() == bot.GetTeamId()
            || !candidate->IsAlive()
            || candidate->IsEliminated())
        {
            continue;
        }

        const Team* candidateTeam = FindTeam(candidate->GetTeamId());
        if (candidateTeam == nullptr || candidateTeam->coreAlive)
        {
            continue;
        }

        const float distance = DistanceSquared(bot.GetPosition(), candidate->GetPosition());
        if (matchSimulation_.MatchTimeSeconds() < 135.0f)
        {
            continue;
        }

        const float score = distance
            + static_cast<float>(candidate->GetHealth()) * 18.0f
            - static_cast<float>(std::max(0, bot.GetHealth() - candidate->GetHealth())) * 22.0f;
        if (score < finalLifeEnemyDistanceSq)
        {
            finalLifeEnemyDistanceSq = score;
            finalLifeEnemy = candidate;
        }
    }
    if (finalLifeEnemy != nullptr)
    {
        huntEnemy = finalLifeEnemy;
        const float finalDistance = std::sqrt(DistanceSquared(bot.GetPosition(), finalLifeEnemy->GetPosition()));
        if (nearbyEnemy == nullptr || finalDistance < BotEngageRange(memory.role, botDifficulty_, botTuning) * 1.8f)
        {
            nearbyEnemy = finalLifeEnemy;
        }
    }
    const int retreatHealth = BotRetreatHealth(memory.role, botDifficulty_, team.coreAlive, botTuning);
    const int fightHealth = BotFightHealth(memory.role, botDifficulty_, botTuning);
    const float rushTime = botDifficulty_ == BotDifficulty::Hard ? 38.0f : (botDifficulty_ == BotDifficulty::Easy ? 115.0f : 70.0f);

    Player* weakEnemy = nullptr;
    if (huntEnemy != nullptr
        && huntEnemy->GetHealth() <= bot.GetHealth() - (botDifficulty_ == BotDifficulty::Easy ? 34 : 18))
    {
        weakEnemy = huntEnemy;
    }

    if (nearbyEnemy != nullptr)
    {
        memory.lastSeenEnemyPosition = nearbyEnemy->GetPosition();
        memory.hasLastSeenEnemy = true;
    }
    else if (enemyAtCore != nullptr)
    {
        memory.lastSeenEnemyPosition = enemyAtCore->GetPosition();
        memory.hasLastSeenEnemy = true;
        nearbyEnemy = enemyAtCore;
    }

    if (bot.GetHealth() < retreatHealth && team.coreAlive)
    {
        memory.retreatTimer = std::max(memory.retreatTimer, botDifficulty_ == BotDifficulty::Hard ? 1.25f : 2.1f);
    }
    const bool defenderAwayFromBase = memory.role == BotRole::Defender
        && team.coreAlive
        && distanceFromHome > 40.0f;
    const int desiredBlocks = BotDesiredBlocks(memory.role, botTuning);
    BotResourcePlan resourcePlan {};
    if (memory.resourcePlanTimer <= 0.0f)
    {
        int openingHomeCollectorId = std::numeric_limits<int>::max();
        for (const Player& candidate : players_)
        {
            if (IsBotControlled(ControlKindForPlayer(candidate))
                && candidate.IsAlive()
                && !candidate.IsEliminated()
                && candidate.GetTeamId() == bot.GetTeamId())
            {
                openingHomeCollectorId = std::min(openingHomeCollectorId, candidate.GetId());
            }
        }
        const bool allowHomePickup = matchSimulation_.MatchTimeSeconds() >= 60.0f || bot.GetId() == openingHomeCollectorId;
        const ResourcePickup* bestPickup = ::FindBestPickupForBot(
            bot,
            memory.role,
            matchSimulation_.Pickups(),
            aliveEnemies,
            coreHome,
            arenaBiome_ == ArenaBiome::Ruins,
            allowHomePickup);
        resourcePlan = BuildBotResourcePlan(bot, bestPickup, matchSimulation_.Generators(), memory.role, arenaBiome_ == ArenaBiome::Ruins);
        memory.cachedResourceTarget = resourcePlan.target;
        memory.cachedResourceType = static_cast<int>(resourcePlan.type);
        memory.hasCachedResourceTarget = resourcePlan.hasTarget;
        memory.resourcePlanTimer = botDifficulty_ == BotDifficulty::Hard ? 0.12f : 0.24f;
    }
    else
    {
        resourcePlan.target = memory.cachedResourceTarget;
        resourcePlan.type = static_cast<ResourceType>(memory.cachedResourceType);
        resourcePlan.hasTarget = memory.hasCachedResourceTarget;
    }
    const Vector3 resourceTarget = resourcePlan.target;
    const bool hasResourceTarget = resourcePlan.hasTarget;
    const ResourceType resourceTargetType = resourcePlan.type;
    const float nearbyEnemyDistance = nearbyEnemy != nullptr ? std::sqrt(DistanceSquared(bot.GetPosition(), nearbyEnemy->GetPosition())) : 999.0f;
    const int tacticalEnemyCoreTeamId = enemyCore != nullptr ? enemyCore->GetTeamId() : -1;
    if (memory.tacticalCheckTimer <= 0.0f || memory.tacticalEnemyCoreTeamId != tacticalEnemyCoreTeamId)
    {
        memory.tacticalEnemyCoreTeamId = tacticalEnemyCoreTeamId;
        memory.cachedCanBreakDefense = enemyCore != nullptr
            && enemyCore->IsAlive()
            && DistanceSquared(bot.GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 28.0f
            && FindCoreDefenseBlock(*enemyCore, bot).has_value();
        memory.cachedCoreCanUpgrade = team.coreAlive
            && memory.role == BotRole::Defender
            && FindUpgradeableCoreDefenseBlock(team, bot).has_value();
        memory.tacticalCheckTimer = botDifficulty_ == BotDifficulty::Hard ? 0.10f : 0.18f;
    }
    const bool canBreakDefense = memory.cachedCanBreakDefense
        && enemyCore != nullptr
        && enemyCore->IsAlive()
        && enemyCore->GetTeamId() == memory.tacticalEnemyCoreTeamId
        && DistanceSquared(bot.GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 32.0f;
    const bool closeToEnemyCore = enemyCore != nullptr
        && DistanceSquared(botPos, world_.GridToWorld(enemyCore->GetBlockPosition())) < 150.0f;
    const bool bridgeKitReady = inventory.GetBlocks() >= (memory.role == BotRole::Rusher ? 12 : 18)
        || (closeToEnemyCore && inventory.GetBlocks() >= 3);
    const bool combatKitReady = inventory.GetToolLevel() > 0
        || inventory.GetSwordLevel() > 0
        || inventory.GetUtility(UtilityType::Fireball) > 0
        || (memory.role == BotRole::Rusher && matchSimulation_.MatchTimeSeconds() > 12.0f)
        || (memory.role == BotRole::Fighter && matchSimulation_.MatchTimeSeconds() > 24.0f)
        || matchSimulation_.MatchTimeSeconds() > rushTime;
    const bool readyToRush = bridgeKitReady && combatKitReady;
    const bool coreCanUpgrade = team.coreAlive
        && memory.role == BotRole::Defender
        && memory.cachedCoreCanUpgrade;
    const Team* huntEnemyTeam = huntEnemy != nullptr ? FindTeam(huntEnemy->GetTeamId()) : nullptr;
    const bool huntEnemyOnFinalLife = huntEnemyTeam != nullptr && !huntEnemyTeam->coreAlive;
    int finalLifeTeamAlive = 0;
    if (huntEnemyOnFinalLife)
    {
        const auto foundHuntTeamContext = frameContext.teamContexts.find(huntEnemy->GetTeamId());
        if (foundHuntTeamContext != frameContext.teamContexts.end())
        {
            finalLifeTeamAlive = static_cast<int>(foundHuntTeamContext->second.aliveAllies.size());
        }
        else
        {
            for (const Player* candidate : frameContext.alivePlayers)
            {
                if (candidate != nullptr
                    && candidate->GetTeamId() == huntEnemy->GetTeamId()
                    && candidate->IsAlive()
                    && !candidate->IsEliminated())
                {
                    ++finalLifeTeamAlive;
                }
            }
        }
    }
    const float huntEnemyDistance = huntEnemy != nullptr ? std::sqrt(DistanceSquared(bot.GetPosition(), huntEnemy->GetPosition())) : 999.0f;
    const bool lastFinalLifeEnemy = huntEnemyOnFinalLife && finalLifeTeamAlive <= 1;
    const bool finalLifeTargetClose = huntEnemyOnFinalLife
        && (huntEnemyDistance < 92.0f
            || (matchSimulation_.MatchTimeSeconds() > 165.0f && huntEnemyDistance < 150.0f)
            || (lastFinalLifeEnemy && matchSimulation_.MatchTimeSeconds() > 145.0f)
            || matchSimulation_.MatchTimeSeconds() > 165.0f);
    const bool finalDuelPhase = huntEnemy != nullptr && (!team.coreAlive || enemyCore == nullptr || huntEnemyOnFinalLife);
    const bool defenderThreat = memory.role == BotRole::Defender
        && (enemyAtCore != nullptr || (nearbyEnemy != nullptr && distanceFromHome < 72.0f));
    const bool vulnerableEnemy = nearbyEnemy != nullptr
        && nearbyEnemy->GetHealth() <= bot.GetHealth() - (botDifficulty_ == BotDifficulty::Easy ? 34 : 18);
    const bool enemyBlockingObjective = nearbyEnemy != nullptr
        && enemyCore != nullptr
        && DistanceSquared(nearbyEnemy->GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 36.0f;
    const FightAssessment nearbyFightAssessment = nearbyEnemy != nullptr
        ? AssessFight(
            bot,
            *nearbyEnemy,
            teamContext != nullptr ? &teamContext->aliveAllies : nullptr,
            &aliveEnemies,
            memory.role,
            botDifficulty_,
            botTuning,
            defenderThreat,
            huntEnemyOnFinalLife && nearbyEnemy == huntEnemy,
            enemyBlockingObjective)
        : FightAssessment {};
    if (nearbyFightAssessment.shouldRetreat && team.coreAlive)
    {
        memory.retreatTimer = std::max(memory.retreatTimer, botDifficulty_ == BotDifficulty::Hard ? 0.95f : 1.55f);
    }
    const bool retreating = memory.retreatTimer > 0.0f && team.coreAlive;
    // A stalled-fight ban means: stop seeking this brawl, play the objective.
    // Defending the base or finishing a final-life enemy always overrides it.
    const bool fightSeekAllowed = memory.chaseBanTimer <= 0.0f
        || defenderThreat
        || enemyBlockingObjective
        || (huntEnemyOnFinalLife && nearbyEnemy == huntEnemy);
    const bool shouldFightNearby = nearbyEnemy != nullptr
        && fightSeekAllowed
        && bot.GetHealth() > fightHealth
        && !nearbyFightAssessment.shouldRetreat
        && (nearbyFightAssessment.shouldFight
            || memory.role == BotRole::Fighter
            || defenderThreat
            || vulnerableEnemy
            || (huntEnemyOnFinalLife && nearbyEnemy == huntEnemy)
            || enemyBlockingObjective
            || (memory.role == BotRole::Rusher && nearbyEnemyDistance < 4.6f)
            || (memory.role == BotRole::Collector && nearbyEnemyDistance < 3.0f && bot.GetHealth() > 68));
    const bool cleanupOverEconomy = huntEnemyOnFinalLife && finalLifeTargetClose && matchSimulation_.MatchTimeSeconds() > 155.0f;
    const bool shouldPressureCore = enemyCore != nullptr
        && enemyCore->IsAlive()
        && (memory.role == BotRole::Rusher || memory.role == BotRole::Fighter)
        && (readyToRush || (matchSimulation_.MatchTimeSeconds() > 95.0f && inventory.GetBlocks() >= 6))
        && !retreating
        && !cleanupOverEconomy;
    const int coordinatedAttackersOnTarget = coordBus != nullptr && enemyCore != nullptr
        ? coordBus->CountSignal(
            CoordinationSignal::AttackingCore,
            matchSimulation_.MatchTimeSeconds(),
            kCoordinationSignalTtl,
            enemyCore->GetTeamId(),
            bot.GetId())
        : 0;
    const int coordinatedDefenders = coordBus != nullptr
        ? coordBus->CountSignal(CoordinationSignal::DefendingCore, matchSimulation_.MatchTimeSeconds(), kCoordinationSignalTtl, -999, bot.GetId())
        : 0;
    const int coordinatedHelpCalls = coordBus != nullptr
        ? coordBus->CountSignal(CoordinationSignal::CallingForHelp, matchSimulation_.MatchTimeSeconds(), kCoordinationSignalTtl, -999, bot.GetId())
        : 0;

    const Vector3 shopTarget { team.shopPosition.x, 1.5f, team.shopPosition.z };
    const Vector3 spawnTarget { team.spawnPoint.x, 1.5f, team.spawnPoint.z };
    perceptionProfile.Stop();
    ScopedProfileTimer planningProfile(profilingEnabled_, profilePlanningMs_, profilePlanningCalls_);
    const BotDecisionContext decisionContext {
        bot,
        team,
        inventory,
        memory,
        botTuning,
        botDifficulty_,
        matchSimulation_.MatchTimeSeconds(),
        nearbyEnemy,
        weakEnemy,
        huntEnemy,
        enemyAtCore,
        enemyCore,
        teamPlan,
        botPos,
        coreHome,
        resourceTarget,
        enemyAtCoreDistance,
        distanceFromHome,
        nearbyEnemyDistance,
        nearbyFightAssessment,
        retreatHealth,
        fightHealth,
        desiredBlocks,
        resourceTargetType,
        retreating,
        defenderAwayFromBase,
        carryingLoot,
        wantsShop,
        canBreakDefense,
        readyToRush,
        coreNeedsRepair,
        coreDefenseCritical,
        coreCanUpgrade,
        strategicPlan,
        finalDuelPhase,
        huntEnemyOnFinalLife,
        finalLifeTargetClose,
        shouldFightNearby,
        shouldPressureCore,
        hasResourceTarget,
        coordinatedAttackersOnTarget,
        coordinatedDefenders,
        coordinatedHelpCalls,
        missingDefenseBlocks
    };
    if (memory.strategicUpdateTimer <= 0.0f || ShouldInterruptStrategicPlan(memory.currentPlan, decisionContext))
    {
        memory.currentPlan = EvaluateStrategicPlan(decisionContext);
        memory.strategicUpdateTimer = StrategicPlanUpdateCadence(botDifficulty_, botTuning);
    }

    BotDecision decision = EvaluateBotDecision(decisionContext);
    const BotMacroDirective macroDirective = EvaluateAutonomousMacroDirective(decisionContext);
    if (macroDirective.active)
    {
        decision.intent = macroDirective.intent;
        decision.state = StateForIntent(macroDirective.intent);
        decision.target = macroDirective.target;
        decision.fightTarget = macroDirective.fightTarget;
        decision.reason = macroDirective.reason;
        decision.score += 520.0f;
    }
    const bool openingScout = matchSimulation_.MatchTimeSeconds() < 60.0f
        && openingRank >= 1
        && openingRank <= 2
        && distanceFromHome < 196.0f
        && enemyAtCore == nullptr
        && !coreDefenseCritical;
    if (openingScout)
    {
        decision.intent = BotIntent::PressureCore;
        decision.state = BotState::AttackCore;
        decision.target = enemyCore != nullptr
            ? world_.GridToWorld(enemyCore->GetBlockPosition())
            : Vector3 { 0.0f, 1.5f, 0.0f };
        decision.fightTarget = nullptr;
        decision.reason = "opening bridge pressure";
        decision.score = 2000.0f;
    }
    ApplyBotDecision(memory, decision, botDifficulty_, botTuning);
    planningProfile.Stop();
    if (coordBus != nullptr)
    {
        CoordinationSignal signal = CoordinationSignal::None;
        int coordinationTargetTeamId = -1;
        switch (decision.intent)
        {
        case BotIntent::PressureCore:
        case BotIntent::BreakCoreDefense:
            signal = CoordinationSignal::AttackingCore;
            coordinationTargetTeamId = enemyCore != nullptr ? enemyCore->GetTeamId() : -1;
            break;
        case BotIntent::DefendCore:
        case BotIntent::RepairCoreDefense:
            signal = enemyAtCore != nullptr || coreDefenseCritical
                ? CoordinationSignal::CallingForHelp
                : CoordinationSignal::DefendingCore;
            coordinationTargetTeamId = team.id;
            break;
        case BotIntent::RetreatHome:
            signal = CoordinationSignal::Retreating;
            coordinationTargetTeamId = team.id;
            break;
        case BotIntent::SecureResources:
            signal = resourceTargetType == ResourceType::Crystal ? CoordinationSignal::HoldingMid : CoordinationSignal::None;
            break;
        case BotIntent::GearUp:
        case BotIntent::FightEnemy:
        case BotIntent::ChaseWeakEnemy:
        case BotIntent::Recover:
            break;
        }

        coordBus->Broadcast(bot.GetId(), signal, decision.target, matchSimulation_.MatchTimeSeconds(), coordinationTargetTeamId);
    }

    if (memory.intent == BotIntent::RepairCoreDefense && TryBotRepairCoreDefense(bot, team, dt))
    {
        return;
    }

    decisionProfile.Stop();
    ScopedProfileTimer movementProfile(profilingEnabled_, profileMovementMs_, profileMovementCalls_);
    Vector3 target = decision.target;
    if (memory.intent == BotIntent::RetreatHome && memory.role != BotRole::Defender && bot.GetHealth() < 55)
    {
        target = spawnTarget;
    }
    const bool objectivePush = memory.intent == BotIntent::PressureCore
        || memory.intent == BotIntent::BreakCoreDefense
        || memory.state == BotState::AttackCore
        || memory.state == BotState::BreakDefense;
    if (objectivePush && enemyCore != nullptr && enemyCore->IsAlive())
    {
        target = Vector3 {
            static_cast<float>(enemyCore->GetBlockPosition().x),
            1.5f,
            static_cast<float>(enemyCore->GetBlockPosition().z)
        };
        const Vector3 center { 0.0f, 1.5f, 0.0f };
        if (DistanceSquared(botPos, center) > 130.0f
            && DistanceSquared(botPos, target) > 360.0f)
        {
            target = center;
        }
        if (inventory.GetBlocks() <= 0
            && DistanceSquared(botPos, world_.GridToWorld(enemyCore->GetBlockPosition())) > 42.0f)
        {
            target = shopTarget;
        }
    }

    if (memory.state != BotState::Fight && DistanceSquared(botPos, target) > 180.0f)
    {
        target = ChooseBotWaypoint(bot, target);
    }
    if (memory.state != BotState::Fight)
    {
        target = ChooseBotPathWaypoint(bot, target, dt, frameContext);
    }

    BotMovementPlan movementPlan = BuildBotMovementPlan(
        bot,
        memory,
        botDifficulty_,
        matchSimulation_.MatchTimeSeconds(),
        coreHome,
        target,
        memory.state,
        memory.intent,
        decision.fightTarget,
        [this, objectivePush](Vector3 position)
        {
            if (objectivePush && HasSupportBelow(world_, position, 3))
            {
                return false;
            }
            return IsVoidThreatAt(position);
        },
        [&bot, this](Vector3 bridgeTarget)
        {
            return TryBotBridgeBlock(bot, bridgeTarget);
        });
    Vector3 wish = movementPlan.wish;
    Player* fightTarget = movementPlan.fightTarget;
    Vector3 aimDirection = movementPlan.aimDirection;
    const float fightDistance = movementPlan.fightDistance;
    const bool edgePressure = movementPlan.edgePressure;
    const Vector3 nextStep {
        botPos.x + wish.x * 0.95f,
        botPos.y,
        botPos.z + wish.z * 0.95f
    };

    const BotTraversalPlan traversalPlan = BuildBotTraversalPlan(
        bot,
        memory,
        botDifficulty_,
        fightTarget,
        fightDistance,
        nextStep,
        wish,
        dt,
        world_,
        [&bot, target, this](Vector3 breakWish, float delta)
        {
            return TryBotBreakBlockingBlock(bot, breakWish, target, delta);
        });
    if (traversalPlan.consumedByMining)
    {
        return;
    }
    const bool jump = traversalPlan.jump;

    const bool sprint = botDifficulty_ != BotDifficulty::Easy
        && Length2D(wish) > 0.2f
        && !edgePressure
        && (memory.state == BotState::AttackCore
            || memory.state == BotState::Fight
            || memory.state == BotState::Collect
            || memory.intent == BotIntent::GearUp
            || memory.intent == BotIntent::ChaseWeakEnemy);
    const Vector3 beforeMove = bot.GetPosition();
    const PlayerCommand movementCommand = BuildBotMovementCommand(
        bot,
        matchSimulation_.CurrentTick(),
        wish,
        aimDirection,
        jump,
        sprint);
    ApplyPlayerCommand(bot, movementCommand, dt);
    ApplyStandingBlockEffects(bot, false);
    movementProfile.Stop();
    ScopedProfileTimer combatProfile(profilingEnabled_, profileCombatMs_, profileCombatCalls_);

    const float movedDistance = DistanceSquared(beforeMove, bot.GetPosition());
    UpdateBotStuckAfterMove(memory, wish, movedDistance, dt, bot.GetPosition());

    // Chase futility bookkeeping: pursuits that stop closing the distance get
    // banned for a while so the bot pivots to core assault or economy instead
    // of orbiting a runner (and falling into the void) for minutes.
    const bool suddenDeathBrawl = !team.coreAlive
        && (enemyCore == nullptr || !enemyCore->IsAlive());
    if (suddenDeathBrawl)
    {
        memory.chaseBanTimer = 0.0f;
        memory.fightStallTimer = 0.0f;
        memory.chaseStuckTimer = 0.0f;
    }
    else if (memory.intent == BotIntent::FightEnemy && fightTarget != nullptr)
    {
        // A fight that is not actually hurting anyone for a long stretch is a
        // stall (heal wars, knockback ping-pong). Walk away and play the map.
        if (memory.chaseTargetId != fightTarget->GetId())
        {
            memory.chaseTargetId = fightTarget->GetId();
            memory.fightTargetHealth = fightTarget->GetHealth();
            memory.fightStallTimer = 0.0f;
        }
        else if (fightTarget->GetHealth() < memory.fightTargetHealth - 4 || bot.GetHealth() < 30)
        {
            memory.fightTargetHealth = fightTarget->GetHealth();
            memory.fightStallTimer = std::max(0.0f, memory.fightStallTimer - 2.5f);
        }
        else
        {
            memory.fightStallTimer += dt;
        }
        if (memory.fightStallTimer > 11.0f)
        {
            memory.chaseBanTimer = std::max(memory.chaseBanTimer, 22.0f);
            memory.fightStallTimer = 0.0f;
            memory.chaseTargetId = -1;
        }
    }
    else if (memory.intent == BotIntent::ChaseWeakEnemy && fightTarget != nullptr)
    {
        const float chaseDistance = std::sqrt(DistanceSquared(bot.GetPosition(), fightTarget->GetPosition()));
        if (memory.chaseTargetId != fightTarget->GetId())
        {
            memory.chaseTargetId = fightTarget->GetId();
            memory.chaseStuckTimer = 0.0f;
            memory.chaseLastDistance = chaseDistance;
        }
        const float closingSpeed = dt > 0.0f ? (memory.chaseLastDistance - chaseDistance) / dt : 0.0f;
        memory.chaseLastDistance = chaseDistance;
        if (chaseDistance < 3.4f)
        {
            memory.chaseStuckTimer = std::max(0.0f, memory.chaseStuckTimer - dt * 2.0f);
        }
        else if (closingSpeed < 0.35f)
        {
            memory.chaseStuckTimer += dt;
            if (edgePressure && bot.GetInventory().GetBlocks() <= 0)
            {
                memory.chaseStuckTimer += dt * 2.0f;
            }
        }
        else
        {
            memory.chaseStuckTimer = std::max(0.0f, memory.chaseStuckTimer - dt * 0.6f);
        }

        if (memory.chaseStuckTimer > 8.0f)
        {
            memory.chaseBanTimer = enemyCore != nullptr && enemyCore->IsAlive() ? 26.0f : 11.0f;
            memory.chaseStuckTimer = 0.0f;
            memory.chaseTargetId = -1;
        }
    }
    else if (memory.chaseTargetId != -1)
    {
        memory.chaseTargetId = -1;
        memory.chaseStuckTimer = 0.0f;
        memory.fightStallTimer = 0.0f;
    }

    if ((memory.intent == BotIntent::GearUp || memory.state == BotState::Shop || wantsShop)
        && nearbyEnemy == nullptr)
    {
        BotTryShop(bot, team);
    }

    BotUseHeroAbility(bot, team, fightTarget, enemyCore);
    BotUseUtility(bot, team, fightTarget, enemyCore, dt);

    TryPerformBotMeleeAttack(
        bot,
        memory,
        botDifficulty_,
        matchSimulation_.MatchTimeSeconds(),
        sprint,
        aimDirection,
        fightTarget,
        combat_,
        world_,
        players_,
        [this](const CombatEvent& event, const std::string& message)
        {
            RegisterCombatEvent(event, message);
        });

    if (TryPerformBotCoreAssault(
            bot,
            memory,
            enemyCore,
            dt,
            world_,
            combat_,
            [this](Player& player, EnergyCore& core, float delta)
            {
                return TryBotBreakCoreDefense(player, core, delta);
            },
            [this](const Player& player, const EnergyCore& core)
            {
                return HasBotCoreAccess(player, core);
            },
            [this, &bot](const EnergyCore& core)
            {
                RemoveWorldBlock(core.GetBlockPosition(), BlockDeltaReason::CoreDestroyed, bot.GetId());
                Team* destroyedTeam = FindTeam(core.GetTeamId());
                if (destroyedTeam != nullptr)
                {
                    destroyedTeam->coreAlive = false;
                }
            },
            [this](const CombatEvent& event, const std::string& message)
            {
                RegisterCombatEvent(event, message);
            }))
    {
        return;
    }
}

Vector3 Game::ChooseBotPathWaypoint(Player& bot, Vector3 finalTarget, float dt, const BotFrameContext& frameContext)
{
    ScopedProfileTimer profileTimer(profilingEnabled_, profilePathMs_, profilePathCalls_);
    BotMemory& memory = GetBotMemory(bot);
    const auto foundTeamContext = frameContext.teamContexts.find(bot.GetTeamId());
    const BotTeamFrameContext* teamContext = foundTeamContext != frameContext.teamContexts.end()
        ? &foundTeamContext->second
        : nullptr;
    const BotTuningGenome& botTuning = BotTuningForTeam(bot.GetTeamId());
    const Inventory& inventory = bot.GetInventory();
    const bool defendingCore = memory.role == BotRole::Defender
        && (memory.intent == BotIntent::DefendCore || memory.intent == BotIntent::RepairCoreDefense);
    const bool pushingObjective = memory.intent == BotIntent::PressureCore
        || memory.intent == BotIntent::BreakCoreDefense
        || memory.state == BotState::AttackCore
        || memory.state == BotState::BreakDefense;
    const bool retreating = memory.intent == BotIntent::RetreatHome;
    const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_, botTuning);
    const bool conservativeRoute = retreating || (memory.role == BotRole::Collector && carryingLoot);
    const float refreshCadence = defendingCore
        ? 0.14f
        : (pushingObjective ? 0.20f : (conservativeRoute ? 0.24f : 0.30f));

    memory.navTimer = std::max(0.0f, memory.navTimer - dt);
    if (memory.hasNavWaypoint
        && memory.navTimer > 0.0f
        && DistanceSquared(memory.navTarget, finalTarget) < 9.0f
        && DistanceSquared(bot.GetPosition(), memory.navWaypoint) > 1.2f)
    {
        return memory.navWaypoint;
    }

    memory.hasNavWaypoint = false;
    memory.navTarget = finalTarget;
    memory.navTimer = refreshCadence;

    const auto findDuelEnemy = [&](float maxDistance) -> const Player*
    {
        const float maxDistanceSq = maxDistance * maxDistance;
        const Player* best = nullptr;
        float bestScore = std::numeric_limits<float>::max();
        const auto considerEnemy = [&](const Player& enemy)
        {
            if (enemy.GetTeamId() == bot.GetTeamId() || !enemy.IsAlive() || enemy.IsEliminated())
            {
                return;
            }

            const float distance = DistanceSquared(bot.GetPosition(), enemy.GetPosition());
            if (distance <= maxDistanceSq && distance < bestScore)
            {
                bestScore = distance;
                best = &enemy;
            }
        };

        if (teamContext != nullptr)
        {
            for (const Player* enemy : teamContext->aliveEnemies)
            {
                if (enemy != nullptr)
                {
                    considerEnemy(*enemy);
                }
            }
        }
        else
        {
            for (const Player& enemy : players_)
            {
                considerEnemy(enemy);
            }
        }

        return best;
    };

    const Player* duelEnemy = findDuelEnemy(defendingCore ? 30.0f : 16.0f);
    const FightAssessment pathFightAssessment = duelEnemy != nullptr
        ? AssessFight(
            bot,
            *duelEnemy,
            teamContext != nullptr ? &teamContext->aliveAllies : nullptr,
            teamContext != nullptr ? &teamContext->aliveEnemies : nullptr,
            memory.role,
            botDifficulty_,
            botTuning,
            defendingCore,
            false,
            pushingObjective)
        : FightAssessment {};
    const bool canTakeFight = defendingCore
        || duelEnemy == nullptr
        || pathFightAssessment.canWin
        || (pushingObjective && pathFightAssessment.powerMargin > -18.0f);
    const float botPower = BotCombatPowerScore(bot, ApplyCombatTuning(kPathCombatPowerWeights, botTuning));
    const int blockCount = inventory.GetBlocks();
    const int toolLevel = inventory.GetToolLevel();
    const bool canBridge = blockCount >= (memory.role == BotRole::Rusher ? 3 : (conservativeRoute ? 6 : 4));
    struct ThreatSample
    {
        Vector3 position {};
        float power = 0.0f;
    };
    std::vector<ThreatSample> threatSamples;
    threatSamples.reserve(teamContext != nullptr ? teamContext->aliveEnemies.size() : players_.size());
    const auto addThreatSample = [&](const Player& other)
    {
        if (other.GetTeamId() == bot.GetTeamId() || !other.IsAlive() || other.IsEliminated())
        {
            return;
        }
        threatSamples.push_back(ThreatSample { other.GetPosition(), BotCombatPowerScore(other, ApplyCombatTuning(kPathCombatPowerWeights, botTuning)) });
    };
    if (teamContext != nullptr)
    {
        for (const Player* other : teamContext->aliveEnemies)
        {
            if (other != nullptr)
            {
                addThreatSample(*other);
            }
        }
    }
    else
    {
        for (const Player& other : players_)
        {
            addThreatSample(other);
        }
    }

    const GridPos start = FindSupportBelow(world_, bot.GetPosition(), pushingObjective ? 4 : 2);
    const GridPos rawGoal = FindSupportBelow(world_, finalTarget, pushingObjective ? 3 : 1);
    const int pathRadius = defendingCore ? 56 : (pushingObjective ? 52 : (conservativeRoute ? 44 : 48));
    const int maxExpansions = defendingCore ? 3600 : (pushingObjective ? 3200 : 2600);
    const float threatRange = defendingCore ? 11.0f : 8.2f;
    const float threatScaleBase = defendingCore
        ? 0.12f
        : (canTakeFight ? 0.55f : (memory.role == BotRole::Collector ? 1.45f : 1.0f));
    const float threatScale = conservativeRoute ? threatScaleBase * 1.35f : threatScaleBase;
    const float threatRangeSq = threatRange * threatRange;
    const int minX = start.x - pathRadius;
    const int maxX = start.x + pathRadius;
    const int minZ = start.z - pathRadius;
    const int maxZ = start.z + pathRadius;

    const auto inBounds = [&](const GridPos& pos)
    {
        return pos.x >= minX && pos.x <= maxX && pos.z >= minZ && pos.z <= maxZ && pos.y >= kBotPathMinY && pos.y <= kBotPathMaxY;
    };
    const auto hasAnyAnchor = [&](const GridPos& support)
    {
        const GridPos around[] {
            GridPos { support.x + 1, support.y, support.z },
            GridPos { support.x - 1, support.y, support.z },
            GridPos { support.x, support.y, support.z + 1 },
            GridPos { support.x, support.y, support.z - 1 },
            GridPos { support.x, support.y - 1, support.z }
        };
        for (const GridPos& probe : around)
        {
            if (world_.IsSolid(probe))
            {
                return true;
            }
        }
        return false;
    };
    const auto headClearanceCost = [&](const GridPos& support, float& digCost)
    {
        digCost = 0.0f;
        const GridPos headBlocks[] {
            GridPos { support.x, support.y + 1, support.z },
            GridPos { support.x, support.y + 2, support.z }
        };

        for (const GridPos& head : headBlocks)
        {
            if (world_.IsAir(head))
            {
                continue;
            }

            const Block* block = world_.GetBlock(head);
            if (block == nullptr
                || !block->breakable
                || !IsBreakableByPlayers(block->type)
                || (block->teamId == bot.GetTeamId() && block->teamId != -1)
                || toolLevel <= 0)
            {
                return false;
            }
            digCost += 2.0f - std::min(1.2f, static_cast<float>(toolLevel) * 0.35f);
        }
        return true;
    };
    const auto nodeTravelCost = [&](const GridPos& support) -> std::optional<float>
    {
        if (!inBounds(support))
        {
            return std::nullopt;
        }
        float digCost = 0.0f;
        if (!headClearanceCost(support, digCost))
        {
            return std::nullopt;
        }

        bool usesBridgeNode = false;
        float cost = 1.0f;
        if (world_.IsSolid(support))
        {
            const Block* supportBlock = world_.GetBlock(support);
            if (supportBlock != nullptr)
            {
                if (supportBlock->type == BlockType::LavaBlock)
                {
                    cost += 20.0f;
                }
                else if (supportBlock->type == BlockType::SpikeBlock)
                {
                    cost += 12.0f;
                }
                else if (supportBlock->type == BlockType::IceBlock && conservativeRoute)
                {
                    cost += 1.0f;
                }
            }
        }
        else if (world_.IsAir(support) && canBridge && hasAnyAnchor(support))
        {
            usesBridgeNode = true;
            cost += 2.6f + (memory.role == BotRole::Rusher ? 0.2f : 0.9f);
            if (conservativeRoute)
            {
                cost += 1.8f;
            }
        }
        else
        {
            return std::nullopt;
        }

        int exposure = 0;
        const GridPos around[] {
            GridPos { support.x + 1, support.y, support.z },
            GridPos { support.x - 1, support.y, support.z },
            GridPos { support.x, support.y, support.z + 1 },
            GridPos { support.x, support.y, support.z - 1 }
        };
        for (const GridPos& neighbor : around)
        {
            if (world_.IsAir(neighbor))
            {
                ++exposure;
            }
        }
        cost += static_cast<float>(exposure) * (usesBridgeNode ? 0.55f : 0.18f);

        const Vector3 nodePos = world_.GridToWorld(support);
        for (const ThreatSample& threat : threatSamples)
        {
            const float enemyDistanceSq = DistanceSquared(threat.position, nodePos);
            if (enemyDistanceSq > threatRangeSq)
            {
                continue;
            }

            const float enemyDistance = std::sqrt(std::max(0.0001f, enemyDistanceSq));
            const float influence = 1.0f - enemyDistance / threatRange;
            const float enemyDiff = std::max(0.0f, threat.power - botPower);
            cost += (7.0f + enemyDiff * 0.085f) * influence * threatScale;
            if (canTakeFight && !conservativeRoute)
            {
                cost -= 1.4f * influence;
            }
            if (defendingCore)
            {
                cost -= 1.0f * influence;
            }
        }

        const float centerDistance = std::sqrt(static_cast<float>(support.x * support.x + support.z * support.z));
        if (conservativeRoute && centerDistance < 32.0f)
        {
            cost += (32.0f - centerDistance) * 0.12f;
        }
        if (pushingObjective && memory.role == BotRole::Rusher && centerDistance < 30.0f)
        {
            cost -= (30.0f - centerDistance) * 0.06f;
        }

        return std::max(0.25f, cost + digCost);
    };

    GridPos goal = rawGoal;
    if (!nodeTravelCost(goal).has_value())
    {
        int bestScore = std::numeric_limits<int>::max();
        bool found = false;
        for (int radius = 1; radius <= 3; ++radius)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                for (int dz = -radius; dz <= radius; ++dz)
                {
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        const GridPos candidate { rawGoal.x + dx, rawGoal.y + dy, rawGoal.z + dz };
                        if (!nodeTravelCost(candidate).has_value())
                        {
                            continue;
                        }
                        const int score = std::abs(candidate.x - rawGoal.x)
                            + std::abs(candidate.z - rawGoal.z)
                            + std::abs(candidate.y - rawGoal.y) * 2;
                        if (score < bestScore)
                        {
                            bestScore = score;
                            goal = candidate;
                            found = true;
                        }
                    }
                }
            }
            if (found)
            {
                break;
            }
        }
    }

    const auto heuristic = [&](const GridPos& support)
    {
        const float horizontal = static_cast<float>(std::abs(support.x - goal.x) + std::abs(support.z - goal.z));
        const float vertical = static_cast<float>(std::abs(support.y - goal.y));
        return horizontal * (pushingObjective ? 0.98f : 1.05f) + vertical * 1.95f;
    };
    const auto goalScore = [&](const GridPos& support)
    {
        return std::abs(support.x - goal.x) + std::abs(support.z - goal.z) + std::abs(support.y - goal.y) * 2;
    };

    struct OpenNode
    {
        GridPos pos {};
        float g = 0.0f;
        float f = 0.0f;
    };
    const auto cmp = [](const OpenNode& a, const OpenNode& b)
    {
        return a.f > b.f;
    };
    static thread_local std::vector<OpenNode> open;
    static thread_local std::unordered_map<long long, float> bestCost;
    static thread_local std::unordered_map<long long, GridPos> parent;
    static thread_local std::vector<GridPos> path;
    static thread_local std::unordered_map<long long, bool> seenPath;
    open.clear();
    bestCost.clear();
    parent.clear();
    path.clear();
    seenPath.clear();
    const std::size_t pathReserve = static_cast<std::size_t>(maxExpansions);
    const std::size_t mapReserve = pathReserve * 2U;
    open.reserve(pathReserve);
    path.reserve(pathReserve);
    if (bestCost.bucket_count() < mapReserve)
    {
        bestCost.reserve(mapReserve);
    }
    if (parent.bucket_count() < mapReserve)
    {
        parent.reserve(mapReserve);
    }
    if (seenPath.bucket_count() < pathReserve)
    {
        seenPath.reserve(pathReserve);
    }
    if (!nodeTravelCost(start).has_value())
    {
        return finalTarget;
    }
    open.push_back(OpenNode { start, 0.0f, heuristic(start) });
    std::push_heap(open.begin(), open.end(), cmp);
    bestCost[PathKey(start.x, start.y, start.z)] = 0.0f;

    std::optional<GridPos> bestGoal;
    int bestGoalScore = goalScore(start);
    int expansions = 0;
    while (!open.empty() && expansions++ < maxExpansions)
    {
        std::pop_heap(open.begin(), open.end(), cmp);
        const OpenNode current = open.back();
        open.pop_back();
        const long long currentKey = PathKey(current.pos.x, current.pos.y, current.pos.z);
        const auto foundCurrent = bestCost.find(currentKey);
        if (foundCurrent == bestCost.end() || current.g > foundCurrent->second + 0.0001f)
        {
            continue;
        }

        const int currentGoalScore = goalScore(current.pos);
        if (!bestGoal.has_value() || currentGoalScore < bestGoalScore)
        {
            bestGoal = current.pos;
            bestGoalScore = currentGoalScore;
        }
        if (currentGoalScore <= 1)
        {
            bestGoal = current.pos;
            break;
        }

        const bool preferZFirst = bot.GetId() % 2 == 0;
        const GridPos offsets[] {
            preferZFirst ? GridPos { 0, 0, 1 } : GridPos { 1, 0, 0 },
            preferZFirst ? GridPos { 0, 0, -1 } : GridPos { -1, 0, 0 },
            preferZFirst ? GridPos { 1, 0, 0 } : GridPos { 0, 0, 1 },
            preferZFirst ? GridPos { -1, 0, 0 } : GridPos { 0, 0, -1 }
        };
        for (const GridPos& offset : offsets)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                const GridPos next { current.pos.x + offset.x, current.pos.y + dy, current.pos.z + offset.z };
                const std::optional<float> travelCost = nodeTravelCost(next);
                if (!travelCost.has_value())
                {
                    continue;
                }

                const float verticalCost = dy > 0 ? 0.90f : (dy < 0 ? 0.38f : 0.0f);
                const float nextG = current.g + *travelCost + verticalCost;
                const long long key = PathKey(next.x, next.y, next.z);
                const auto found = bestCost.find(key);
                if (found != bestCost.end() && found->second <= nextG)
                {
                    continue;
                }

                bestCost[key] = nextG;
                parent[key] = current.pos;
                open.push_back(OpenNode { next, nextG, nextG + heuristic(next) });
                std::push_heap(open.begin(), open.end(), cmp);
            }
        }
    }

    if (!bestGoal.has_value() || bestGoalScore > 5)
    {
        return finalTarget;
    }

    GridPos step = *bestGoal;
    path.push_back(step);
    seenPath[PathKey(step.x, step.y, step.z)] = true;
    int reconstructGuard = 0;
    while (step != start)
    {
        if (++reconstructGuard > maxExpansions)
        {
            return finalTarget;
        }
        const auto found = parent.find(PathKey(step.x, step.y, step.z));
        if (found == parent.end())
        {
            return finalTarget;
        }
        step = found->second;
        const long long stepKey = PathKey(step.x, step.y, step.z);
        if (seenPath.find(stepKey) != seenPath.end() && step != start)
        {
            return finalTarget;
        }
        seenPath[stepKey] = true;
        path.push_back(step);
    }
    std::reverse(path.begin(), path.end());

    if (path.size() < 2)
    {
        return finalTarget;
    }

    const std::size_t maxLookAhead = defendingCore
        ? 4
        : (pushingObjective ? 4 : (conservativeRoute ? 2 : 3));
    const std::size_t lookAhead = std::min<std::size_t>(path.size() - 1, maxLookAhead);
    const GridPos nextStep = path[lookAhead];
    const Vector3 waypoint {
        static_cast<float>(nextStep.x),
        static_cast<float>(nextStep.y) + 1.08f,
        static_cast<float>(nextStep.z)
    };
    memory.navWaypoint = waypoint;
    memory.hasNavWaypoint = true;
    return waypoint;
}

Vector3 Game::ChooseBotWaypoint(const Player& bot, Vector3 finalTarget) const
{
    const Vector3 botPos = bot.GetPosition();
    const float finalDistance = DistanceSquared(botPos, finalTarget);
    Vector3 best = StepTargetToward(botPos, finalTarget, botDifficulty_ == BotDifficulty::Hard ? 14.0f : 11.0f);
    float bestScore = finalDistance;

    const auto consider = [&](Vector3 waypoint, float bias)
    {
        waypoint.y = 1.5f;
        const float fromBot = DistanceSquared(botPos, waypoint);
        const float toFinal = DistanceSquared(waypoint, finalTarget);
        if (fromBot < 18.0f || fromBot > finalDistance + 0.01f || toFinal > finalDistance + 28.0f)
        {
            return;
        }

        const float score = fromBot * 0.72f + toFinal * 0.48f + bias;
        if (score < bestScore)
        {
            bestScore = score;
            best = waypoint;
        }
    };

    consider(Vector3 { 0.0f, 1.5f, 0.0f }, botDifficulty_ == BotDifficulty::Hard ? -70.0f : -40.0f);
    for (const Generator& generator : matchSimulation_.Generators())
    {
        if (generator.GetTeamId() != -1)
        {
            continue;
        }

        const ResourceType type = generator.GetType();
        const float bias = type == ResourceType::Crystal ? -95.0f : -42.0f;
        consider(ToVector3(generator.GetPosition()), bias);
    }

    return best;
}

bool Game::TryBotBridgeBlock(Player& bot, Vector3 target)
{
    if (bot.GetInventory().GetBlocks() <= 0)
    {
        return false;
    }

    const Vector3 botPos = bot.GetPosition();
    const Vector3 direction = Normalize2D(Vector3 { target.x - botPos.x, 0.0f, target.z - botPos.z });
    if (Length2D(direction) <= 0.0001f)
    {
        return false;
    }

    BotMemory& memory = GetBotMemory(bot);
    if (memory.reactionDelayTimer > 0.0f)
    {
        return false;
    }
    const GridPos underFeet = world_.WorldToGrid(Vector3 { botPos.x, botPos.y - 1.08f, botPos.z });
    const GridPos targetSupport = world_.WorldToGrid(Vector3 { target.x, target.y - 1.08f, target.z });
    const int bridgeY = std::min(underFeet.y, targetSupport.y);
    const bool inVoid = IsVoidThreatAt(botPos);
    if (inVoid
        && underFeet.y <= bridgeY
        && bot.GetVelocity().y <= 0.2f
        && world_.IsAir(underFeet)
        && TryPlaceBlockForPlayer(bot, underFeet, false))
    {
        memory.bridgePlaceCooldown = BotBridgeCooldown(botDifficulty_);
        ++memory.bridgeBlocksPlaced;
        return true;
    }
    if (!bot.IsOnGround() && !inVoid)
    {
        return false;
    }
    if (memory.bridgePlaceCooldown > 0.0f)
    {
        return false;
    }

    const GridPos ahead = world_.WorldToGrid(Vector3 {
        botPos.x + direction.x * 1.05f,
        static_cast<float>(bridgeY),
        botPos.z + direction.z * 1.05f
    });
    const int stepX = direction.x > 0.18f ? 1 : (direction.x < -0.18f ? -1 : 0);
    const int stepZ = direction.z > 0.18f ? 1 : (direction.z < -0.18f ? -1 : 0);
    const float axisDifference = std::fabs(direction.x) - std::fabs(direction.z);
    const bool xDominant = axisDifference > 0.001f
        || (std::fabs(axisDifference) <= 0.001f && bot.GetId() % 2 != 0);
    const GridPos primary {
        underFeet.x + (xDominant ? stepX : 0),
        bridgeY,
        underFeet.z + (xDominant ? 0 : stepZ)
    };
    const GridPos secondary {
        underFeet.x + (xDominant ? 0 : stepX),
        bridgeY,
        underFeet.z + (xDominant ? stepZ : 0)
    };
    const GridPos sideStep {
        xDominant ? 0 : memory.strafeSign,
        0,
        xDominant ? memory.strafeSign : 0
    };
    const GridPos candidates[] {
        primary,
        secondary,
        ahead,
        GridPos { ahead.x + sideStep.x, ahead.y, ahead.z + sideStep.z },
        GridPos { ahead.x - sideStep.x, ahead.y, ahead.z - sideStep.z }
    };

    bool placedAhead = false;
    GridPos placedPos = ahead;
    for (const GridPos& candidate : candidates)
    {
        if (world_.IsAir(candidate) && TryPlaceBlockForPlayer(bot, candidate, false))
        {
            placedAhead = true;
            placedPos = candidate;
            break;
        }
    }
    if (placedAhead)
    {
        memory.bridgePlaceCooldown = BotBridgeCooldown(botDifficulty_);
        ++memory.bridgeBlocksPlaced;
        const bool placeSafetyBlock = (botDifficulty_ == BotDifficulty::Hard
                && (memory.role == BotRole::Fighter || memory.role == BotRole::Defender || memory.bridgeBlocksPlaced % 5 == 0))
            || (botDifficulty_ == BotDifficulty::Normal && memory.bridgeBlocksPlaced % 7 == 0);
        if (placeSafetyBlock)
        {
            const GridPos side {
                placedPos.x + sideStep.x,
                placedPos.y,
                placedPos.z + sideStep.z
            };
            if (world_.IsAir(side) && TryPlaceBlockForPlayer(bot, side, false))
            {
                ++memory.bridgeBlocksPlaced;
            }
        }
    }
    return placedAhead;
}

bool Game::TryBotBreakCoreDefense(Player& bot, EnergyCore& core, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
    const std::optional<GridPos> defenseBlock = FindCoreDefenseBlock(core, bot);
    if (!defenseBlock.has_value())
    {
        memory.hasBreakTarget = false;
        memory.breakProgress = 0.0f;
        return false;
    }

    const GridPos target = *defenseBlock;
    const Vector3 targetPos = world_.GridToWorld(target);
    if (DistanceSquared(bot.GetPosition(), targetPos) > 10.0f)
    {
        return true;
    }

    const Block* block = world_.GetBlock(target);
    if (block == nullptr || !block->breakable || !IsBreakableByPlayers(block->type))
    {
        memory.hasBreakTarget = false;
        memory.breakProgress = 0.0f;
        return false;
    }

    if (!memory.hasBreakTarget || memory.breakTarget != target)
    {
        memory.breakTarget = target;
        memory.hasBreakTarget = true;
        memory.breakProgress = 0.0f;
    }

    memory.breakProgress += dt / BreakSeconds(block->type, bot.GetInventory().GetToolLevel());
    if (memory.breakProgress < 1.0f)
    {
        return true;
    }

    memory.breakProgress = 0.22f;
    return true;
}

std::optional<GridPos> Game::FindBotBlockingBlock(const Player& bot, Vector3 wish, Vector3 target) const
{
    const Vector3 direction = Normalize2D(wish);
    const Vector3 botPos = bot.GetPosition();
    const Vector3 eye { botPos.x, botPos.y + 0.68f, botPos.z };
    const Vector3 targetEye { target.x, target.y + 0.62f, target.z };
    const Vector3 toTarget {
        targetEye.x - eye.x,
        targetEye.y - eye.y,
        targetEye.z - eye.z
    };
    const float targetDistance = Length(toTarget);
    if (targetDistance > 1.2f)
    {
        const std::optional<RaycastHit> hit = world_.Raycast(eye, toTarget, std::min(5.6f, targetDistance));
        if (hit.has_value())
        {
            const Block* block = world_.GetBlock(hit->block);
            if (block != nullptr
                && block->breakable
                && block->teamId != bot.GetTeamId()
                && IsBreakableByPlayers(block->type))
            {
                return hit->block;
            }
        }
    }

    if (Length2D(direction) <= 0.0001f)
    {
        return std::nullopt;
    }

    const float distances[] { 0.72f, 1.05f, 1.34f, 1.72f, 2.05f };
    const float heights[] { -0.42f, 0.12f, 0.62f, 1.02f };
    std::optional<GridPos> best;
    float bestScore = std::numeric_limits<float>::max();

    for (float distance : distances)
    {
        for (float height : heights)
        {
            const GridPos pos = world_.WorldToGrid(Vector3 {
                botPos.x + direction.x * distance,
                botPos.y + height,
                botPos.z + direction.z * distance
            });
            const Block* block = world_.GetBlock(pos);
            if (block == nullptr || !block->breakable || block->teamId == bot.GetTeamId() || !IsBreakableByPlayers(block->type))
            {
                continue;
            }

            const float score = DistanceSquared(botPos, world_.GridToWorld(pos))
                + BreakSeconds(block->type, bot.GetInventory().GetToolLevel()) * 0.35f;
            if (score < bestScore)
            {
                bestScore = score;
                best = pos;
            }
        }
    }

    return best;
}

bool Game::TryBotBreakBlockingBlock(Player& bot, Vector3 wish, Vector3 targetPosition, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
    const std::optional<GridPos> blockingBlock = FindBotBlockingBlock(bot, wish, targetPosition);
    if (!blockingBlock.has_value())
    {
        if (memory.hasBreakTarget)
        {
            const Block* block = world_.GetBlock(memory.breakTarget);
            if (block == nullptr || !block->breakable || !IsBreakableByPlayers(block->type))
            {
                memory.hasBreakTarget = false;
                memory.breakProgress = 0.0f;
            }
        }
        return false;
    }

    const GridPos target = *blockingBlock;
    const Block* block = world_.GetBlock(target);
    if (block == nullptr || !block->breakable || block->teamId == bot.GetTeamId() || !IsBreakableByPlayers(block->type))
    {
        return false;
    }

    if (!memory.hasBreakTarget || memory.breakTarget != target)
    {
        memory.breakTarget = target;
        memory.hasBreakTarget = true;
        memory.breakProgress = 0.0f;
    }

    Vector3 breakAim = Normalize2D(wish);
    if (Length2D(breakAim) <= 0.0001f)
    {
        breakAim = Normalize2D(Vector3 {
            targetPosition.x - bot.GetPosition().x,
            0.0f,
            targetPosition.z - bot.GetPosition().z
        });
    }
    if (Length2D(breakAim) > 0.0001f)
    {
        const PlayerCommand aimCommand = BuildBotMovementCommand(
            bot,
            matchSimulation_.CurrentTick(),
            Vector3 {},
            breakAim,
            false,
            false);
        ApplyPlayerCommand(bot, aimCommand, 0.0f);
    }
    memory.breakProgress += dt / BreakSeconds(block->type, bot.GetInventory().GetToolLevel());
    if (memory.breakProgress < 1.0f)
    {
        return true;
    }

    const Vector3 targetPos = world_.GridToWorld(target);
    if (BreakWorldBlock(target, bot.GetTeamId(), BlockDeltaReason::PlayerBreak, bot.GetId()))
    {
        AddWorldEffect(targetPos, Color { 210, 220, 235, 255 }, 0.24f, 0.22f);
        AddFloatingText("break", targetPos, Color { 210, 220, 235, 255 });
        audio_.PlayBreakBlockAt(targetPos);
        bot.GetInventory().DamageTool(1);
    }

    memory.hasBreakTarget = false;
    memory.breakProgress = 0.0f;
    memory.stuckTimer = 0.0f;
    return true;
}

void Game::BotTryShop(Player& bot, Team& team)
{
    if (!shop_.IsPlayerInShop(bot, team))
    {
        return;
    }

    BotMemory& memory = GetBotMemory(bot);
    const Inventory& inventory = bot.GetInventory();
    std::vector<int> priorities;

    if (bot.GetHealth() < 55)
    {
        priorities.push_back(203);
        priorities.push_back(202);
    }
    if (inventory.GetBlocks() < 8)
    {
        priorities.push_back(2);
    }

    switch (memory.role)
    {
    case BotRole::Defender:
        if (EnergyCore* core = FindCoreByTeam(team.id); core != nullptr && core->IsAlive() && core->GetHealth() < core->GetMaxHealth() - 24)
        {
            priorities.push_back(303);
        }
        if (inventory.GetBlocks() < 20)
        {
            priorities.push_back(2);
        }
        if (inventory.GetBlockCount(BlockType::StoneBlock) < 20)
        {
            priorities.push_back(3);
        }
        if (inventory.GetBlockCount(BlockType::ObsidianBlock) < 4)
        {
            priorities.push_back(4);
        }
        if (inventory.GetArmorLevel() < 2)
        {
            priorities.push_back(103);
        }
        if (!inventory.HasItem(ItemType::Axe))
        {
            priorities.push_back(106);
        }
        if (inventory.GetUtility(UtilityType::AlarmTrap) < 1)
        {
            priorities.push_back(207);
        }
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            priorities.push_back(401);
        }
        else if (inventory.GetBlasterDamageLevel() == 0 && inventory.GetBlasterRapidFireLevel() < 2)
        {
            priorities.push_back(402);
        }
        if (!inventory.HasItem(ItemType::Bow))
        {
            priorities.push_back(108);
        }
        else if (inventory.GetBowUpgradeLevel() < 2)
        {
            priorities.push_back(109);
        }
        break;

    case BotRole::Rusher:
        if (inventory.GetBlocks() < 40)
        {
            priorities.push_back(2);
        }
        if (inventory.GetToolLevel() < 2)
        {
            priorities.push_back(102);
        }
        if (!inventory.HasItem(ItemType::Axe) && inventory.GetToolLevel() >= 1)
        {
            priorities.push_back(106);
        }
        if (inventory.GetSwordLevel() < 2)
        {
            priorities.push_back(101);
        }
        if (inventory.GetUtility(UtilityType::Fireball) < 1)
        {
            priorities.push_back(105);
        }
        if (inventory.GetUtility(UtilityType::Molotov) < 1)
        {
            priorities.push_back(206);
        }
        if (inventory.GetUtility(UtilityType::Arrows) < 6)
        {
            priorities.push_back(104);
        }
        if (!inventory.HasItem(ItemType::Bow))
        {
            priorities.push_back(108);
        }
        else if (inventory.GetBowUpgradeLevel() < 1)
        {
            priorities.push_back(109);
        }
        if (inventory.GetUtility(UtilityType::Dash) < 1)
        {
            priorities.push_back(205);
        }
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            priorities.push_back(401);
        }
        break;

    case BotRole::Collector:
        if (team.forgeLevel < 3)
        {
            priorities.push_back(301);
        }
        if (team.healAuraLevel < 2)
        {
            priorities.push_back(302);
        }
        if (!team.enemyTrackerUnlocked)
        {
            priorities.push_back(304);
        }
        if (inventory.GetBlocks() < 24)
        {
            priorities.push_back(2);
        }
        if (inventory.GetArmorLevel() < 2)
        {
            priorities.push_back(103);
        }
        if (!inventory.HasItem(ItemType::Spear))
        {
            priorities.push_back(107);
        }
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            priorities.push_back(401);
        }
        if (!inventory.HasItem(ItemType::Bow))
        {
            priorities.push_back(108);
        }
        break;

    case BotRole::Fighter:
        if (inventory.GetSwordLevel() < 2)
        {
            priorities.push_back(101);
        }
        if (!inventory.HasItem(ItemType::Spear))
        {
            priorities.push_back(107);
        }
        if (!inventory.HasItem(ItemType::Axe) && inventory.GetArmorLevel() >= 1)
        {
            priorities.push_back(106);
        }
        if (inventory.GetArmorLevel() < 2)
        {
            priorities.push_back(103);
        }
        if (inventory.GetBlocks() < 24)
        {
            priorities.push_back(2);
        }
        if (inventory.GetUtility(UtilityType::Fireball) < 1)
        {
            priorities.push_back(105);
        }
        if (inventory.GetUtility(UtilityType::Molotov) < 1)
        {
            priorities.push_back(206);
        }
        if (inventory.GetUtility(UtilityType::Dash) < 1)
        {
            priorities.push_back(205);
        }
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            priorities.push_back(401);
        }
        else if (inventory.GetBlasterRapidFireLevel() == 0 && inventory.GetBlasterDamageLevel() < 3)
        {
            priorities.push_back(403);
        }
        if (!inventory.HasItem(ItemType::Bow))
        {
            priorities.push_back(108);
        }
        else if (inventory.GetBowUpgradeLevel() < 3)
        {
            priorities.push_back(109);
        }
        break;
    }

    if ((memory.role == BotRole::Collector || memory.role == BotRole::Defender)
        && inventory.GetUtility(UtilityType::HomeTeleport) < 1)
    {
        priorities.push_back(204);
    }
    if (bot.GetHealth() < 70)
    {
        priorities.push_back(203);
        priorities.push_back(202);
    }
    if (bot.GetSpeedBoostTimer() <= 0.0f)
    {
        priorities.push_back(201);
    }

    for (int choice : priorities)
    {
        std::string message;
        if (TryShopPurchase(bot, team, choice, 1, message))
        {
            AddEventMessage(bot.GetName() + ": " + message, GetTeamColor(team.color), 1.6f);
            return;
        }
    }
}

EnergyCore* Game::SelectBestAttackTarget(
    const Player& player,
    const BotFrameContext& frameContext,
    const TeamCoordinationBus* coordBus)
{
    EnergyCore* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();
    const BotTuningGenome& tuning = BotTuningForTeam(player.GetTeamId());

    for (EnergyCore& core : matchSimulation_.Cores())
    {
        if (core.GetTeamId() == player.GetTeamId() || !core.IsAlive())
        {
            continue;
        }

        const Vector3 corePos = world_.GridToWorld(core.GetBlockPosition());
        const float distance = DistanceSquared(player.GetPosition(), corePos);
        const float weaknessBonus = static_cast<float>(core.GetMaxHealth() - core.GetHealth()) * 1.8f;
        float defenderPenalty = 0.0f;
        const auto foundTargetTeamContext = frameContext.teamContexts.find(core.GetTeamId());
        if (foundTargetTeamContext != frameContext.teamContexts.end())
        {
            const BotTeamFrameContext& targetTeam = foundTargetTeamContext->second;
            defenderPenalty += static_cast<float>(targetTeam.teamPlan.defendersNearCore) * 72.0f;
            for (const Player* defender : targetTeam.aliveAllies)
            {
                if (defender == nullptr
                    || defender->GetId() == player.GetId()
                    || !defender->IsAlive()
                    || defender->IsEliminated())
                {
                    continue;
                }
                if (DistanceSquared(defender->GetPosition(), corePos) < 90.0f)
                {
                    defenderPenalty += 34.0f;
                }
            }
        }

        const int coordinatedAttackers = coordBus != nullptr
            ? coordBus->CountSignal(
                CoordinationSignal::AttackingCore,
                matchSimulation_.MatchTimeSeconds(),
                kCoordinationSignalTtl,
                core.GetTeamId(),
                player.GetId())
            : 0;
        const float coordinationPenalty = static_cast<float>(coordinatedAttackers) * tuning.pressureCoordinationPenalty;
        const float accessBonus = HasBotCoreAccess(player, core) ? 95.0f : 0.0f;
        const bool clockwisePressure = !automatch_.active || automatch_.completedRuns % 2 == 0;
        const float neighborBonus = core.GetTeamId() == PreferredNeighborTeam(player.GetTeamId(), clockwisePressure) ? 120.0f : 0.0f;
        const float score = distance - weaknessBonus + defenderPenalty + coordinationPenalty - accessBonus - neighborBonus;
        if (score < bestScore)
        {
            bestScore = score;
            best = &core;
        }
    }

    return best;
}

Player* Game::FindNearbyEnemyPlayer(const Player& player, float maxDistance)
{
    Player* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();
    const BotRole role = ResolveRoleForPlayerId(player.GetId(), botMemories_, botMemoryIndexByPlayerId_);
    const Team* ownTeam = FindTeam(player.GetTeamId());
    const Vector3 ownCore = ownTeam != nullptr ? world_.GridToWorld(ownTeam->coreBlock) : player.GetPosition();

    for (Player& other : players_)
    {
        if (other.GetId() == player.GetId()
            || other.GetTeamId() == player.GetTeamId()
            || !other.IsAlive()
            || other.IsEliminated())
        {
            continue;
        }

        const float distance = DistanceSquared(player.GetPosition(), other.GetPosition());
        if (distance > maxDistance * maxDistance)
        {
            continue;
        }

        float score = distance;
        score -= static_cast<float>(std::max(0, player.GetHealth() - other.GetHealth())) * 0.95f;
        const Team* enemyTeam = FindTeam(other.GetTeamId());
        if (enemyTeam != nullptr && !enemyTeam->coreAlive)
        {
            score -= 420.0f;
            if (other.GetHealth() <= player.GetHealth() + 12)
            {
                score -= 180.0f;
            }
        }
        if (role == BotRole::Defender)
        {
            score += DistanceSquared(other.GetPosition(), ownCore) * 0.24f;
        }
        if (role == BotRole::Collector && other.GetHealth() > player.GetHealth() + 12)
        {
            score += 24.0f;
        }
        if (score < bestScore)
        {
            bestScore = score;
            best = &other;
        }
    }

    return best;
}

BotMemory& Game::GetBotMemory(Player& bot)
{
    const int playerId = bot.GetId();
    const auto foundIndex = botMemoryIndexByPlayerId_.find(playerId);
    if (foundIndex != botMemoryIndexByPlayerId_.end())
    {
        const std::size_t index = foundIndex->second;
        if (index < botMemories_.size() && botMemories_[index].playerId == playerId)
        {
            return botMemories_[index];
        }
        botMemoryIndexByPlayerId_.erase(foundIndex);
    }

    for (std::size_t index = 0; index < botMemories_.size(); ++index)
    {
        BotMemory& memory = botMemories_[index];
        if (memory.playerId == bot.GetId())
        {
            botMemoryIndexByPlayerId_[playerId] = index;
            return memory;
        }
    }

    const BotRole role = RoleForBotId(bot.GetId());
    BotMemory memory {};
    memory.playerId = bot.GetId();
    memory.state = BotState::Collect;
    memory.role = role;
    memory.intent = BotIntent::SecureResources;
    memory.intentReason = "spawn plan";
    memory.roleReason = "spawn role";
    const std::size_t index = botMemories_.size();
    botMemories_.push_back(memory);
    botMemoryIndexByPlayerId_[playerId] = index;
    return botMemories_[index];
}

void Game::ApplyBotHitReaction(int playerId)
{
    // Only existing bot memories are touched: the human player has none and
    // must not get one created here.
    for (BotMemory& memory : botMemories_)
    {
        if (memory.playerId == playerId)
        {
            memory.reactionDelayTimer = std::max(
                memory.reactionDelayTimer,
                BotHitReactionSeconds(botDifficulty_));
            return;
        }
    }
}

std::optional<RaycastHit> Game::RaycastFromAim(const Player& player, float maxDistance) const
{
    const Vector3 aimDirection = cameraController_.GetAimDirection();
    const std::optional<RaycastHit> cameraHit = world_.Raycast(cameraController_.GetAimOrigin(), aimDirection, maxDistance);
    if (cameraHit.has_value())
    {
        const Vector3 blockCenter = world_.GridToWorld(cameraHit->block);
        if (DistanceSquared(player.GetPosition(), blockCenter) > 2.0f)
        {
            return cameraHit;
        }
    }

    const Vector3 eye {
        player.GetPosition().x,
        player.GetPosition().y + 0.78f,
        player.GetPosition().z
    };
    return world_.Raycast(eye, aimDirection, maxDistance);
}

std::optional<RaycastHit> Game::RaycastFromPlayerEye(const Player& player, Vector3 aimDirection, float maxDistance) const
{
    const Vector3 eye {
        player.GetPosition().x,
        player.GetPosition().y + 0.78f,
        player.GetPosition().z
    };
    return world_.Raycast(eye, aimDirection, maxDistance);
}

std::optional<GridPos> Game::FindCoreDefenseBlock(const EnergyCore& core, const Player& bot) const
{
    const GridPos corePos = core.GetBlockPosition();
    const std::vector<GridPos> candidates = CoreDefensePositions(corePos);
    const Vector3 eye {
        bot.GetPosition().x,
        bot.GetPosition().y + 0.78f,
        bot.GetPosition().z
    };
    const Vector3 coreCenter {
        static_cast<float>(corePos.x),
        static_cast<float>(corePos.y) + 0.58f,
        static_cast<float>(corePos.z)
    };
    const Vector3 toCore {
        coreCenter.x - eye.x,
        coreCenter.y - eye.y,
        coreCenter.z - eye.z
    };
    const float rayDistance = std::sqrt(toCore.x * toCore.x + toCore.y * toCore.y + toCore.z * toCore.z) + 0.25f;
    const std::optional<RaycastHit> directBlock = world_.Raycast(eye, toCore, rayDistance);
    if (directBlock.has_value() && directBlock->block != corePos)
    {
        const Block* block = world_.GetBlock(directBlock->block);
        if (block != nullptr && block->breakable && IsBreakableByPlayers(block->type))
        {
            return directBlock->block;
        }
    }

    std::optional<GridPos> best;
    float bestDistance = std::numeric_limits<float>::max();
    for (const GridPos& candidate : candidates)
    {
        const Block* block = world_.GetBlock(candidate);
        if (block == nullptr || !block->breakable || !IsBreakableByPlayers(block->type))
        {
            continue;
        }

        const float coreShellDistance = static_cast<float>(
            std::abs(candidate.x - corePos.x)
            + std::abs(candidate.y - corePos.y)
            + std::abs(candidate.z - corePos.z));
        const float score = DistanceSquared(bot.GetPosition(), world_.GridToWorld(candidate))
            + coreShellDistance * 3.5f
            + BreakSeconds(block->type, bot.GetInventory().GetToolLevel()) * 1.6f;
        if (score < bestDistance)
        {
            bestDistance = score;
            best = candidate;
        }
    }

    return best;
}

std::optional<GridPos> Game::FindMissingCoreDefenseBlock(const Team& team) const
{
    const std::vector<GridPos> candidates = CoreDefensePositions(team.coreBlock);

    for (const GridPos& candidate : candidates)
    {
        if (world_.IsAir(candidate))
        {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<GridPos> Game::FindUpgradeableCoreDefenseBlock(const Team& team, const Player& bot) const
{
    const std::optional<BlockType> replacement = BestDefenseBlockAvailable(bot.GetInventory());
    if (!replacement.has_value())
    {
        return std::nullopt;
    }

    const int replacementRank = DefenseBlockRank(*replacement);
    const std::vector<GridPos> candidates = CoreDefensePositions(team.coreBlock);
    std::optional<GridPos> best;
    float bestScore = std::numeric_limits<float>::max();

    for (const GridPos& candidate : candidates)
    {
        const Block* block = world_.GetBlock(candidate);
        if (block == nullptr
            || !block->breakable
            || block->teamId != team.id
            || !IsBreakableByPlayers(block->type))
        {
            continue;
        }

        const int currentRank = DefenseBlockRank(block->type);
        if (currentRank <= 0 || replacementRank <= currentRank)
        {
            continue;
        }

        const float score = static_cast<float>(currentRank) * 18.0f
            + DistanceSquared(bot.GetPosition(), world_.GridToWorld(candidate)) * 0.12f;
        if (score < bestScore)
        {
            bestScore = score;
            best = candidate;
        }
    }

    return best;
}

bool Game::TryBotUpgradeCoreDefense(Player& bot, Team& team, float dt)
{
    const std::optional<GridPos> upgradeTarget = FindUpgradeableCoreDefenseBlock(team, bot);
    if (!upgradeTarget.has_value())
    {
        return false;
    }

    const GridPos target = *upgradeTarget;
    const Vector3 targetPos = world_.GridToWorld(target);
    if (DistanceSquared(bot.GetPosition(), targetPos) > 12.0f)
    {
        Vector3 wish = Normalize2D(Vector3 { targetPos.x - bot.GetPosition().x, 0.0f, targetPos.z - bot.GetPosition().z });
        if (IsVoidThreatAt(Vector3 { bot.GetPosition().x + wish.x * 0.85f, bot.GetPosition().y, bot.GetPosition().z + wish.z * 0.85f }))
        {
            TryBotBridgeBlock(bot, targetPos);
        }
        const PlayerCommand movementCommand = BuildBotMovementCommand(
            bot,
            matchSimulation_.CurrentTick(),
            wish,
            wish,
            false,
            false);
        ApplyPlayerCommand(bot, movementCommand, dt);
        ApplyStandingBlockEffects(bot, false);
        return true;
    }

    const Block* block = world_.GetBlock(target);
    if (block == nullptr || !block->breakable || block->teamId != team.id || !IsBreakableByPlayers(block->type))
    {
        return false;
    }

    BotMemory& memory = GetBotMemory(bot);
    if (!memory.hasBreakTarget || memory.breakTarget != target)
    {
        memory.breakTarget = target;
        memory.hasBreakTarget = true;
        memory.breakProgress = 0.0f;
    }

    memory.breakProgress += dt / BreakSeconds(block->type, bot.GetInventory().GetToolLevel());
    if (memory.breakProgress < 1.0f)
    {
        return true;
    }

    if (BreakWorldBlock(target, bot.GetTeamId(), BlockDeltaReason::PlayerBreak, bot.GetId()))
    {
        AddWorldEffect(targetPos, GetTeamColor(team.color), 0.26f, 0.24f);
        AddFloatingText("upgrade", targetPos, GetTeamColor(team.color));
        AddEventMessage(bot.GetName() + " улучшил защиту Кора", GetTeamColor(team.color), 1.5f);
        audio_.PlayBreakBlockAt(targetPos);
        bot.GetInventory().DamageTool(1);
    }
    memory.hasBreakTarget = false;
    memory.breakProgress = 0.0f;
    return true;
}

bool Game::TryBotRepairCoreDefense(Player& bot, Team& team, float dt)
{
    if (!team.coreAlive || bot.GetInventory().GetBlocks() <= 0)
    {
        return false;
    }

    std::optional<GridPos> missing;
    Vector3 threatDirection {};
    float nearestThreatSq = std::numeric_limits<float>::max();
    for (const Player& candidate : players_)
    {
        if (!candidate.IsAlive() || candidate.GetTeamId() == team.id)
        {
            continue;
        }
        const float distanceSq = DistanceSquared(candidate.GetPosition(), world_.GridToWorld(team.coreBlock));
        if (distanceSq < nearestThreatSq)
        {
            nearestThreatSq = distanceSq;
            threatDirection = Normalize2D(Vector3 {
                candidate.GetPosition().x - static_cast<float>(team.coreBlock.x),
                0.0f,
                candidate.GetPosition().z - static_cast<float>(team.coreBlock.z) });
        }
    }
    float bestMissingScore = std::numeric_limits<float>::max();
    for (const GridPos& candidate : CoreDefensePositions(team.coreBlock))
    {
        if (!world_.IsAir(candidate))
        {
            continue;
        }

        std::string reason;
        if (CanPlaceBlockAt(candidate, bot, &reason))
        {
            const Vector3 offset = Normalize2D(Vector3 {
                static_cast<float>(candidate.x - team.coreBlock.x),
                0.0f,
                static_cast<float>(candidate.z - team.coreBlock.z) });
            const float facesThreat = offset.x * threatDirection.x + offset.z * threatDirection.z;
            const float score = DistanceSquared(bot.GetPosition(), world_.GridToWorld(candidate)) * 0.08f
                - facesThreat * (nearestThreatSq < 625.0f ? 12.0f : 2.0f)
                + static_cast<float>(candidate.y - team.coreBlock.y) * 1.5f;
            if (score < bestMissingScore)
            {
                bestMissingScore = score;
                missing = candidate;
            }
        }
    }
    if (!missing.has_value())
    {
        return TryBotUpgradeCoreDefense(bot, team, dt);
    }

    const Vector3 target = world_.GridToWorld(*missing);
    if (DistanceSquared(bot.GetPosition(), target) > 12.0f)
    {
        Vector3 wish = Normalize2D(Vector3 { target.x - bot.GetPosition().x, 0.0f, target.z - bot.GetPosition().z });
        if (IsVoidThreatAt(Vector3 { bot.GetPosition().x + wish.x * 0.85f, bot.GetPosition().y, bot.GetPosition().z + wish.z * 0.85f }))
        {
            TryBotBridgeBlock(bot, target);
        }
        const PlayerCommand movementCommand = BuildBotMovementCommand(
            bot,
            matchSimulation_.CurrentTick(),
            wish,
            wish,
            false,
            false);
        ApplyPlayerCommand(bot, movementCommand, dt);
        ApplyStandingBlockEffects(bot, false);
        return true;
    }

    BotMemory& memory = GetBotMemory(bot);
    if (memory.repairPlaceCooldown > 0.0f)
    {
        return true;
    }
    if (TryPlaceBlockForPlayer(bot, *missing, false))
    {
        // Rate-limit wall rebuilding: an instant infinite wall made every
        // siege unwinnable; a paced defender can be out-damaged by two
        // coordinated attackers but still holds off a single one.
        memory.repairPlaceCooldown = botDifficulty_ == BotDifficulty::Hard
            ? 0.75f
            : (botDifficulty_ == BotDifficulty::Easy ? 1.3f : 0.95f);
        AddEventMessage(bot.GetName() + " починил защиту Кора", GetTeamColor(team.color), 1.5f);
        return true;
    }
    return false;
}


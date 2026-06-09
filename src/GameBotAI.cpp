#include "Game.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr int kBotPathMinY = -2;
constexpr int kBotPathMaxY = 64;

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
        GridPos { corePos.x - 1, corePos.y, corePos.z - 1 }
    };
    return positions;
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

float BotUtilityCooldown(BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return 2.8f;
    case BotDifficulty::Hard:
        return 1.05f;
    case BotDifficulty::Normal:
        break;
    }
    return 1.7f;
}

BotRole RoleForBotId(int id)
{
    return id % 4 == 0
        ? BotRole::Defender
        : (id % 4 == 1 ? BotRole::Fighter : (id % 4 == 2 ? BotRole::Rusher : BotRole::Collector));
}

int BotRetreatHealth(BotRole role, BotDifficulty difficulty, bool coreAlive)
{
    if (!coreAlive)
    {
        return difficulty == BotDifficulty::Hard ? 12 : (difficulty == BotDifficulty::Easy ? 30 : 20);
    }

    switch (role)
    {
    case BotRole::Collector:
        return difficulty == BotDifficulty::Hard ? 32 : (difficulty == BotDifficulty::Easy ? 58 : 44);
    case BotRole::Defender:
        return difficulty == BotDifficulty::Hard ? 22 : (difficulty == BotDifficulty::Easy ? 48 : 34);
    case BotRole::Fighter:
        return difficulty == BotDifficulty::Hard ? 20 : (difficulty == BotDifficulty::Easy ? 46 : 30);
    case BotRole::Rusher:
        return difficulty == BotDifficulty::Hard ? 18 : (difficulty == BotDifficulty::Easy ? 42 : 28);
    }
    return 35;
}

int BotFightHealth(BotRole role, BotDifficulty difficulty)
{
    switch (role)
    {
    case BotRole::Collector:
        return difficulty == BotDifficulty::Hard ? 38 : (difficulty == BotDifficulty::Easy ? 72 : 54);
    case BotRole::Defender:
        return difficulty == BotDifficulty::Hard ? 18 : (difficulty == BotDifficulty::Easy ? 40 : 26);
    case BotRole::Fighter:
        return difficulty == BotDifficulty::Hard ? 14 : (difficulty == BotDifficulty::Easy ? 38 : 22);
    case BotRole::Rusher:
        return difficulty == BotDifficulty::Hard ? 20 : (difficulty == BotDifficulty::Easy ? 44 : 28);
    }
    return 28;
}

float BotEngageRange(BotRole role, BotDifficulty difficulty)
{
    const float difficultyBonus = difficulty == BotDifficulty::Hard ? 1.15f : (difficulty == BotDifficulty::Easy ? -0.85f : 0.0f);
    switch (role)
    {
    case BotRole::Defender:
        return 7.4f + difficultyBonus;
    case BotRole::Fighter:
        return 8.6f + difficultyBonus;
    case BotRole::Rusher:
        return 4.6f + difficultyBonus * 0.6f;
    case BotRole::Collector:
        return 3.1f + difficultyBonus * 0.45f;
    }
    return 5.0f;
}

int BotDesiredBlocks(BotRole role)
{
    switch (role)
    {
    case BotRole::Rusher:
        return 36;
    case BotRole::Defender:
        return 24;
    case BotRole::Collector:
        return 20;
    case BotRole::Fighter:
        return 28;
    }
    return 24;
}

int BotLootReturnValue(BotRole role, BotDifficulty difficulty)
{
    int value = role == BotRole::Collector ? 14 : 30;
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

float IntentLockSeconds(BotIntent intent, BotDifficulty difficulty)
{
    const float difficultyScale = difficulty == BotDifficulty::Hard ? 0.78f : (difficulty == BotDifficulty::Easy ? 1.22f : 1.0f);
    switch (intent)
    {
    case BotIntent::DefendCore:
        return 0.35f;
    case BotIntent::FightEnemy:
    case BotIntent::ChaseWeakEnemy:
        return 0.55f * difficultyScale;
    case BotIntent::RetreatHome:
        return 1.10f * difficultyScale;
    case BotIntent::GearUp:
        return 1.45f * difficultyScale;
    case BotIntent::PressureCore:
    case BotIntent::BreakCoreDefense:
        return 1.05f * difficultyScale;
    case BotIntent::SecureResources:
        return 1.20f * difficultyScale;
    case BotIntent::RepairCoreDefense:
        return 0.90f * difficultyScale;
    case BotIntent::Recover:
        return 0.32f;
    }
    return 0.8f;
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
    std::string reason;
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

const BotMemory* FindBotMemoryByPlayerId(const std::vector<BotMemory>& memories, int playerId)
{
    for (const BotMemory& memory : memories)
    {
        if (memory.playerId == playerId)
        {
            return &memory;
        }
    }
    return nullptr;
}

BotRole ResolveRoleForPlayerId(int playerId, const std::vector<BotMemory>& memories)
{
    if (const BotMemory* memory = FindBotMemoryByPlayerId(memories, playerId))
    {
        return memory->role;
    }
    return RoleForBotId(playerId);
}

BotRoleDistribution BuildRoleDistributionForTeam(
    int teamId,
    const std::vector<Player>& players,
    const std::vector<BotMemory>& memories,
    int ignorePlayerId = -1)
{
    BotRoleDistribution distribution {};
    for (const Player& player : players)
    {
        if (player.GetId() == ignorePlayerId
            || player.GetTeamId() != teamId
            || player.IsLocal()
            || !player.IsAlive()
            || player.IsEliminated())
        {
            continue;
        }

        ++distribution.total;
        switch (ResolveRoleForPlayerId(player.GetId(), memories))
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

float RoleLockSeconds(BotRole role, BotDifficulty difficulty)
{
    const float scale = difficulty == BotDifficulty::Hard ? 0.82f : (difficulty == BotDifficulty::Easy ? 1.22f : 1.0f);
    switch (role)
    {
    case BotRole::Defender:
        return 7.0f * scale;
    case BotRole::Collector:
        return 6.0f * scale;
    case BotRole::Rusher:
        return 5.0f * scale;
    case BotRole::Fighter:
        return 4.6f * scale;
    }
    return 5.0f * scale;
}

float RoleIntentBias(BotRole role, BotIntent intent)
{
    switch (role)
    {
    case BotRole::Defender:
        switch (intent)
        {
        case BotIntent::DefendCore:
            return 210.0f;
        case BotIntent::RepairCoreDefense:
            return 260.0f;
        case BotIntent::FightEnemy:
            return 70.0f;
        case BotIntent::PressureCore:
        case BotIntent::BreakCoreDefense:
            return -180.0f;
        case BotIntent::SecureResources:
            return -35.0f;
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
            return 230.0f;
        case BotIntent::BreakCoreDefense:
            return 210.0f;
        case BotIntent::FightEnemy:
            return 80.0f;
        case BotIntent::DefendCore:
            return -95.0f;
        case BotIntent::RepairCoreDefense:
            return -170.0f;
        case BotIntent::SecureResources:
            return -135.0f;
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
            return 260.0f;
        case BotIntent::GearUp:
            return 150.0f;
        case BotIntent::RetreatHome:
            return 90.0f;
        case BotIntent::PressureCore:
        case BotIntent::BreakCoreDefense:
            return -210.0f;
        case BotIntent::ChaseWeakEnemy:
            return -70.0f;
        case BotIntent::DefendCore:
            return -60.0f;
        case BotIntent::RepairCoreDefense:
            return -45.0f;
        case BotIntent::FightEnemy:
        case BotIntent::Recover:
            break;
        }
        break;
    case BotRole::Fighter:
        switch (intent)
        {
        case BotIntent::FightEnemy:
            return 230.0f;
        case BotIntent::ChaseWeakEnemy:
            return 210.0f;
        case BotIntent::PressureCore:
            return 95.0f;
        case BotIntent::BreakCoreDefense:
            return 75.0f;
        case BotIntent::SecureResources:
            return -110.0f;
        case BotIntent::DefendCore:
            return -35.0f;
        case BotIntent::RepairCoreDefense:
            return -130.0f;
        case BotIntent::GearUp:
        case BotIntent::RetreatHome:
        case BotIntent::Recover:
            break;
        }
        break;
    }
    return 0.0f;
}

struct BotRoleDecision
{
    BotRole role = BotRole::Rusher;
    bool force = false;
    std::string reason;
};

BotRoleDecision EvaluateDynamicRoleDecision(
    const Player& bot,
    const Team& team,
    const BotMemory& memory,
    const BotRoleDistribution& distribution,
    bool coreNeedsRepair,
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

    const bool emergencyDefense = team.coreAlive && (enemyAtCore != nullptr || coreNeedsRepair);
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

    if (roleDeficit(BotRole::Collector) > 0
        && (carryingLoot || wantsShop || lowBlocks || lowHealth))
    {
        decision.role = BotRole::Collector;
        decision.reason = "gather and gear";
        return decision;
    }

    if (roleDeficit(BotRole::Rusher) > 0
        && (nearEnemyCore || !lowCombat || matchTime > 75.0f))
    {
        decision.role = BotRole::Rusher;
        decision.reason = nearEnemyCore ? "pressure core" : "open lane";
        return decision;
    }

    if (roleDeficit(BotRole::Fighter) > 0)
    {
        decision.role = BotRole::Fighter;
        decision.reason = "mid control";
        return decision;
    }

    if (roleDeficit(BotRole::Defender) > 0)
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
    BotDifficulty difficulty)
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
        ? 3.0f
        : (difficulty == BotDifficulty::Easy ? 5.5f : 4.2f);
    if (!decision.force && memory.roleTimer < minimumRoleTime)
    {
        return false;
    }

    memory.role = decision.role;
    memory.roleTimer = 0.0f;
    memory.roleLockTimer = RoleLockSeconds(decision.role, difficulty);
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
    memory.roleTimer += dt;
    memory.roleLockTimer = std::max(0.0f, memory.roleLockTimer - dt);
}

BotTeamSnapshot BuildBotTeamSnapshot(
    const Player& bot,
    Vector3 coreHome,
    const std::vector<Player>& players,
    const std::vector<BotMemory>& memories)
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

        const BotMemory* allyMemory = FindBotMemoryByPlayerId(memories, ally.GetId());
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
    BotDifficulty difficulty)
{
    const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, difficulty);
    const int desiredBlocks = BotDesiredBlocks(memory.role);
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
                || memory.carriedResourceValue >= BotLootReturnValue(memory.role, difficulty)))
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
        plan.target = bestPickup->position;
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
        float score = DistanceSquared(bot.GetPosition(), generator.GetPosition());
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
            score -= DistanceSquared(generator.GetPosition(), Vector3 { 0.0f, generator.GetPosition().y, 0.0f }) * 0.08f;
        }
        if (ruinsBiome
            && role == BotRole::Collector
            && std::fabs(generator.GetPosition().x) >= 18.0f
            && std::fabs(generator.GetPosition().z) >= 18.0f)
        {
            score -= type == ResourceType::Crystal ? 520.0f : 320.0f;
        }

        if (!plan.hasTarget || score < plan.score)
        {
            plan.target = generator.GetPosition();
            plan.type = type;
            plan.hasTarget = true;
            plan.score = score;
        }
    }

    return plan;
}

float BotCombatPowerScore(const Player& player)
{
    const Inventory& inventory = player.GetInventory();
    float score = static_cast<float>(player.GetHealth()) * 1.05f;
    score += static_cast<float>(inventory.GetSwordLevel()) * 17.0f;
    score += static_cast<float>(inventory.GetArmorLevel()) * 14.0f;
    score += static_cast<float>(inventory.GetToolLevel()) * 8.0f;
    score += static_cast<float>(inventory.GetUtility(UtilityType::Fireball)) * 8.0f;
    score += static_cast<float>(inventory.GetUtility(UtilityType::Dash)) * 7.0f;
    score += static_cast<float>(inventory.GetUtility(UtilityType::Molotov)) * 6.0f;
    score += player.GetSpeedBoostTimer() > 0.0f ? 10.0f : 0.0f;
    return score;
}

struct BotMacroDirective
{
    bool active = false;
    BotIntent intent = BotIntent::SecureResources;
    Vector3 target {};
    Player* fightTarget = nullptr;
    std::string reason;
};

struct BotDecisionContext
{
    const Player& bot;
    const Team& team;
    const Inventory& inventory;
    const BotMemory& memory;
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
    bool coreCanUpgrade = false;
    bool finalDuelPhase = false;
    bool huntEnemyOnFinalLife = false;
    bool finalLifeTargetClose = false;
    bool shouldFightNearby = false;
    bool shouldPressureCore = false;
    bool hasResourceTarget = false;
};

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

    if (ctx.nearbyEnemy != nullptr && !isDefender)
    {
        const float selfPower = BotCombatPowerScore(ctx.bot);
        const float enemyPower = BotCombatPowerScore(*ctx.nearbyEnemy);
        const bool canWinFight = selfPower + 8.0f >= enemyPower;
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

    if (isDefender && ctx.team.coreAlive && ctx.coreNeedsRepair)
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

    const bool openingPhase = ctx.matchTime < 42.0f;
    const bool midPhase = ctx.matchTime >= 42.0f && ctx.matchTime < 120.0f;

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
            && (ctx.readyToRush || ctx.teamPlan.activePressure <= 1 || idleNearBase))
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

    if (ctx.matchTime >= 120.0f)
    {
        if (ctx.huntEnemyOnFinalLife && ctx.huntEnemy != nullptr && (ctx.finalLifeTargetClose || ctx.matchTime > 210.0f))
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
            if (ctx.canBreakDefense || isAssaultRole || ctx.readyToRush || idleNearBase)
            {
                directive.active = true;
                directive.intent = ctx.canBreakDefense ? BotIntent::BreakCoreDefense : BotIntent::PressureCore;
                directive.target = enemyCoreTarget;
                directive.reason = ctx.canBreakDefense ? "macro late breach" : "macro late all-in";
                return directive;
            }
        }
        else if (ctx.huntEnemy != nullptr)
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

BotDecision EvaluateBotDecision(const BotDecisionContext& ctx)
{
    BotDecision decision {};
    decision.target = Vector3 { 0.0f, 1.5f, 0.0f };
    const auto consider = [&](BotIntent intent, float score, Vector3 target, Player* fightTarget, const std::string& reason)
    {
        score += RoleIntentBias(ctx.memory.role, intent);
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
    };

    const Vector3 homeTarget { ctx.coreHome.x, 1.5f, ctx.coreHome.z };
    const Vector3 shopTarget { ctx.team.shopPosition.x, 1.5f, ctx.team.shopPosition.z };
    const int iron = ctx.inventory.GetResource(ResourceType::Iron);
    const int gold = ctx.inventory.GetResource(ResourceType::Gold);
    const int crystal = ctx.inventory.GetResource(ResourceType::Crystal);
    const bool canAffordAnyBuy = iron >= 8 || gold >= 4 || crystal >= 2;
    const bool canRepairNow = ctx.inventory.GetBlocks() > 0;
    const bool canRepairSoon = canRepairNow || iron >= 8;
    const bool idleNearBase = ctx.distanceFromHome < 84.0f
        && ctx.memory.intentTimer > 9.0f
        && ctx.enemyAtCore == nullptr
        && ctx.nearbyEnemy == nullptr;

    if (ctx.memory.stuckTimer > 1.05f)
    {
        const Vector3 awayFromCenter = Normalize2D(Vector3 { ctx.botPos.x, 0.0f, ctx.botPos.z });
        Vector3 recoverTarget = Length2D(awayFromCenter) > 0.0001f
            ? Vector3 { ctx.botPos.x + awayFromCenter.x * 3.0f, 1.5f, ctx.botPos.z + awayFromCenter.z * 3.0f }
            : homeTarget;
        if (DistanceSquared(recoverTarget, homeTarget) > DistanceSquared(ctx.botPos, homeTarget) + 36.0f)
        {
            recoverTarget = homeTarget;
        }
        consider(BotIntent::Recover, 980.0f + ctx.memory.stuckTimer * 120.0f, recoverTarget, nullptr, "unstick");
    }

    if (ctx.enemyAtCore != nullptr)
    {
        const float defenderBonus = ctx.memory.role == BotRole::Defender ? 220.0f : 80.0f;
        const float lonelyBaseBonus = ctx.teamPlan.alliesNearCore == 0 ? 130.0f : 0.0f;
        consider(
            BotIntent::DefendCore,
            1180.0f + defenderBonus + lonelyBaseBonus - std::sqrt(ctx.enemyAtCoreDistance) * 10.0f,
            ctx.enemyAtCore->GetPosition(),
            ctx.enemyAtCore,
            "enemy at core");
    }

    if ((ctx.retreating || ctx.defenderAwayFromBase)
        && !(ctx.memory.role == BotRole::Defender && ctx.enemyAtCore != nullptr))
    {
        const float lootBonus = ctx.carryingLoot ? static_cast<float>(ctx.memory.carriedResourceValue) * 4.0f : 0.0f;
        const float healthBonus = static_cast<float>(std::max(0, ctx.retreatHealth + 18 - ctx.bot.GetHealth())) * 9.0f;
        consider(
            BotIntent::RetreatHome,
            760.0f + lootBonus + healthBonus + (ctx.defenderAwayFromBase ? 180.0f : 0.0f),
            ctx.memory.role == BotRole::Defender ? homeTarget : shopTarget,
            nullptr,
            ctx.defenderAwayFromBase ? "return to base" : "heal and bank");
    }

    if ((ctx.coreNeedsRepair || ctx.coreCanUpgrade) && ctx.team.coreAlive)
    {
        const bool assignedBuilder = ctx.memory.role == BotRole::Defender
            || (ctx.teamPlan.defendersNearCore == 0 && ctx.teamPlan.activeRepair == 0 && ctx.distanceFromHome < 120.0f);
        const float defenderBonus = ctx.memory.role == BotRole::Defender ? 280.0f : 0.0f;
        const float repairCrowdPenalty = static_cast<float>(ctx.teamPlan.activeRepair) * 115.0f;
        if (assignedBuilder && canRepairSoon)
        {
            consider(
                BotIntent::RepairCoreDefense,
                500.0f
                    + defenderBonus
                    + (ctx.coreCanUpgrade ? 90.0f : 0.0f)
                    + (canRepairNow ? 70.0f : -90.0f)
                    - repairCrowdPenalty
                    - std::sqrt(ctx.distanceFromHome) * 5.0f,
                homeTarget,
                nullptr,
                ctx.coreCanUpgrade ? "upgrade defense" : "repair defense");
        }
    }

    if ((ctx.wantsShop || ctx.inventory.GetBlocks() < 3) && !(ctx.huntEnemyOnFinalLife && ctx.finalLifeTargetClose && ctx.matchTime > 155.0f))
    {
        const float blockPressure = static_cast<float>(std::max(0, ctx.desiredBlocks - ctx.inventory.GetBlocks())) * 7.0f;
        const float lootPressure = ctx.carryingLoot ? static_cast<float>(ctx.memory.carriedResourceValue) * 3.4f : 0.0f;
        const float shopCrowdPenalty = static_cast<float>(ctx.teamPlan.activeShop) * 72.0f;
        consider(
            BotIntent::GearUp,
            470.0f
                + blockPressure
                + lootPressure
                + (ctx.bot.GetHealth() < 70 ? 120.0f : 0.0f)
                + (canAffordAnyBuy ? 80.0f : -190.0f)
                - shopCrowdPenalty,
            shopTarget,
            nullptr,
            "buy gear");
    }

    if (ctx.shouldFightNearby && ctx.nearbyEnemy != nullptr)
    {
        const float roleBonus = ctx.memory.role == BotRole::Fighter ? 150.0f : (ctx.memory.role == BotRole::Defender ? 85.0f : 0.0f);
        const float healthEdge = static_cast<float>(ctx.bot.GetHealth() - ctx.nearbyEnemy->GetHealth()) * 2.8f;
        consider(
            BotIntent::FightEnemy,
            520.0f + roleBonus + healthEdge - ctx.nearbyEnemyDistance * 18.0f,
            ctx.nearbyEnemy->GetPosition(),
            ctx.nearbyEnemy,
            "take fight");
    }

    if (ctx.weakEnemy != nullptr && ctx.bot.GetHealth() > ctx.fightHealth)
    {
        const float weakDistance = std::sqrt(DistanceSquared(ctx.bot.GetPosition(), ctx.weakEnemy->GetPosition()));
        consider(
            BotIntent::ChaseWeakEnemy,
            500.0f + static_cast<float>(ctx.bot.GetHealth() - ctx.weakEnemy->GetHealth()) * 3.8f - weakDistance * 8.0f,
            ctx.weakEnemy->GetPosition(),
            ctx.weakEnemy,
            "finish weak enemy");
    }

    if (ctx.canBreakDefense && ctx.enemyCore != nullptr)
    {
        const Vector3 corePos = Vector3 {
            static_cast<float>(ctx.enemyCore->GetBlockPosition().x),
            1.5f,
            static_cast<float>(ctx.enemyCore->GetBlockPosition().z)
        };
        consider(
            BotIntent::BreakCoreDefense,
            650.0f + (ctx.memory.role == BotRole::Rusher ? 120.0f : 0.0f),
            corePos,
            nullptr,
            "crack defense");
    }

    if (ctx.shouldPressureCore && ctx.enemyCore != nullptr)
    {
        const Vector3 corePos = Vector3 {
            static_cast<float>(ctx.enemyCore->GetBlockPosition().x),
            1.5f,
            static_cast<float>(ctx.enemyCore->GetBlockPosition().z)
        };
        const float teamPushBonus = ctx.teamPlan.activePressure > 0 ? 70.0f : 0.0f;
        const float baseCoveredBonus = ctx.teamPlan.alliesNearCore > 0 || !ctx.team.coreAlive ? 60.0f : -85.0f;
        const float timePressureBonus = std::max(0.0f, ctx.matchTime - 80.0f) * 1.6f;
        consider(
            BotIntent::PressureCore,
            570.0f + teamPushBonus + baseCoveredBonus + timePressureBonus + (ctx.memory.role == BotRole::Rusher ? 150.0f : 60.0f),
            corePos,
            nullptr,
            "rush core");
    }

    if (ctx.hasResourceTarget && !ctx.carryingLoot && !(ctx.huntEnemyOnFinalLife && ctx.finalLifeTargetClose && ctx.matchTime > 155.0f))
    {
        const float resourceBonus = ctx.resourceTargetType == ResourceType::Crystal
            ? (ctx.memory.role == BotRole::Collector ? 190.0f : 120.0f)
            : (ctx.resourceTargetType == ResourceType::Gold ? 105.0f : 45.0f);
        const float collectorBonus = ctx.memory.role == BotRole::Collector ? 260.0f : 0.0f;
        const float resourceCrowdPenalty = static_cast<float>(ctx.teamPlan.activeResource) * (ctx.memory.role == BotRole::Collector ? 12.0f : 58.0f);
        const float blockShortageBoost = ctx.inventory.GetBlocks() < 6 ? 220.0f : 0.0f;
        const float idleExpeditionBoost = idleNearBase ? 240.0f : 0.0f;
        consider(
            BotIntent::SecureResources,
            390.0f + resourceBonus + collectorBonus + blockShortageBoost + idleExpeditionBoost - resourceCrowdPenalty,
            ctx.resourceTarget,
            nullptr,
            ctx.resourceTargetType == ResourceType::Crystal ? "center crystals" : "secure resources");
    }

    if (ctx.finalDuelPhase
        && ctx.huntEnemy != nullptr
        && (ctx.bot.GetHealth() > ctx.fightHealth || (ctx.huntEnemyOnFinalLife && ctx.matchTime > 145.0f)))
    {
        const float finalLifeBonus = ctx.huntEnemyOnFinalLife ? (ctx.finalLifeTargetClose && ctx.matchTime > 145.0f ? 420.0f : 120.0f) : 0.0f;
        const float cleanupUrgency = ctx.huntEnemyOnFinalLife ? std::max(0.0f, ctx.matchTime - 145.0f) * 5.0f : 0.0f;
        const float distance = std::sqrt(DistanceSquared(ctx.bot.GetPosition(), ctx.huntEnemy->GetPosition()));
        consider(
            BotIntent::ChaseWeakEnemy,
            520.0f
                + finalLifeBonus
                + cleanupUrgency
                + (ctx.team.coreAlive ? 0.0f : 120.0f)
                + static_cast<float>(ctx.bot.GetHealth() - ctx.huntEnemy->GetHealth()) * 1.8f
                - distance * 4.5f,
            ctx.huntEnemy->GetPosition(),
            ctx.huntEnemy,
            ctx.huntEnemyOnFinalLife ? "final-life cleanup" : "final duel");
    }

    if (ctx.enemyCore != nullptr
        && ctx.enemyCore->IsAlive()
        && ctx.readyToRush
        && !(ctx.huntEnemyOnFinalLife && ctx.finalLifeTargetClose && ctx.matchTime > 155.0f))
    {
        const Vector3 corePos = Vector3 {
            static_cast<float>(ctx.enemyCore->GetBlockPosition().x),
            1.5f,
            static_cast<float>(ctx.enemyCore->GetBlockPosition().z)
        };
        const float pressureCrowdPenalty = static_cast<float>(std::max(0, ctx.teamPlan.activePressure - 2)) * 46.0f;
        const float lateMatchBoost = std::max(0.0f, ctx.matchTime - 120.0f) * 2.2f;
        consider(
            BotIntent::PressureCore,
            360.0f + lateMatchBoost + (ctx.memory.role == BotRole::Rusher ? 170.0f : 0.0f) - pressureCrowdPenalty,
            corePos,
            nullptr,
            "map pressure");
    }

    if (idleNearBase
        && ctx.hasResourceTarget
        && !ctx.carryingLoot
        && !ctx.coreNeedsRepair
        && ctx.enemyAtCore == nullptr)
    {
        consider(BotIntent::SecureResources, 860.0f, ctx.resourceTarget, nullptr, "leave base");
    }

    if (decision.score < -9990.0f)
    {
        consider(BotIntent::SecureResources, 0.0f, Vector3 { 0.0f, 1.5f, 0.0f }, nullptr, "default mid");
    }

    return decision;
}

void ApplyBotDecision(BotMemory& memory, const BotDecision& decision, BotDifficulty difficulty)
{
    if (decision.intent != memory.intent)
    {
        memory.intent = decision.intent;
        memory.intentTimer = 0.0f;
        memory.intentLockTimer = IntentLockSeconds(decision.intent, difficulty);
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

    if (Length2D(plan.aimDirection) > 0.0001f)
    {
        bot.SetYaw(YawFromDirection(plan.aimDirection));
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
bool Game::BotUseUtility(Player& bot, Team& team, Player* enemy, EnergyCore* enemyCore)
{
    BotMemory& memory = GetBotMemory(bot);
    if (memory.utilityTimer > 0.0f || botDifficulty_ == BotDifficulty::Easy)
    {
        return false;
    }

    if (bot.GetHealth() <= 45 && SpendUtilityItem(bot, UtilityType::Heal))
    {
        bot.Heal(45);
        AddWorldEffect(bot.GetPosition(), Color { 128, 238, 166, 255 }, 0.30f, 0.30f);
        return true;
    }
    if (enemy != nullptr && bot.GetHealth() <= 68 && SpendUtilityItem(bot, UtilityType::Heal))
    {
        bot.Heal(45);
        AddWorldEffect(bot.GetPosition(), Color { 128, 238, 166, 255 }, 0.24f, 0.24f);
        return true;
    }

    if (team.coreAlive
        && bot.GetInventory().GetUtility(UtilityType::HomeTeleport) > 0
        && memory.intent == BotIntent::RetreatHome
        && (bot.GetHealth() < 42 || memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_) + 10)
        && DistanceSquared(bot.GetPosition(), team.spawnPoint) > 260.0f
        && SpendUtilityItem(bot, UtilityType::HomeTeleport))
    {
        bot.Respawn(team.spawnPoint);
        memory.hasNavWaypoint = false;
        memory.stuckTimer = 0.0f;
        AddWorldEffect(team.spawnPoint, GetTeamColor(team.color), 0.38f, 0.42f);
        AddFloatingText("home", team.spawnPoint, GetTeamColor(team.color));
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
        AddEventMessage(bot.GetName() + " armed base alarm", GetTeamColor(team.color), 1.5f);
        return true;
    }

    if (enemy != nullptr)
    {
        const Vector3 toEnemy {
            enemy->GetPosition().x - bot.GetPosition().x,
            enemy->GetPosition().y + 0.35f - bot.GetPosition().y,
            enemy->GetPosition().z - bot.GetPosition().z
        };
        const float distance = std::sqrt(DistanceSquared(bot.GetPosition(), enemy->GetPosition()));

        if (distance > 4.2f
            && distance < 11.0f
            && bot.GetInventory().GetUtility(UtilityType::Arrows) > 0
            && (memory.role == BotRole::Fighter || memory.role == BotRole::Rusher || botDifficulty_ == BotDifficulty::Hard))
        {
            if (SpendUtilityItem(bot, UtilityType::Arrows))
            {
                LaunchProjectileDirected(bot, UtilityType::Arrows, toEnemy, false);
                AddFloatingText("shot", bot.GetPosition(), Color { 112, 232, 255, 255 });
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
            && bot.GetInventory().GetUtility(UtilityType::Fireball) > 0)
        {
            if (SpendUtilityItem(bot, UtilityType::Fireball))
            {
                LaunchProjectileDirected(bot, UtilityType::Fireball, toEnemy, false);
                AddFloatingText("fireball", bot.GetPosition(), Color { 255, 178, 96, 255 });
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
                return true;
            }
        }
    }

    if (enemyCore != nullptr
        && enemyCore->IsAlive()
        && memory.role == BotRole::Rusher
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
                return true;
            }
        }
    }

    return false;
}

void Game::UpdateBots(float dt)
{
    for (Player& bot : players_)
    {
        if (bot.IsLocal() || !bot.IsAlive() || bot.IsEliminated())
        {
            continue;
        }

        Team* team = FindTeam(bot.GetTeamId());
        if (team == nullptr)
        {
            continue;
        }

        UpdateSingleBot(bot, *team, dt);
    }
}

void Game::UpdateSingleBot(Player& bot, Team& team, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
    TickBotMemory(memory, dt);
    memory.carriedResourceValue = CarriedResourceValue(bot.GetInventory());

    EnergyCore* enemyCore = FindNearestEnemyCore(bot);
    const Vector3 coreHome = world_.GridToWorld(team.coreBlock);
    const Vector3 botPos = bot.GetPosition();
    const float distanceFromHome = DistanceSquared(bot.GetPosition(), coreHome);
    const BotTeamSnapshot teamPlan = BuildBotTeamSnapshot(bot, coreHome, players_, botMemories_);
    float enemyAtCoreDistance = std::numeric_limits<float>::max();
    Player* enemyAtCore = FindEnemyNearCore(bot, players_, coreHome, enemyAtCoreDistance);
    const Inventory& inventory = bot.GetInventory();
    const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_);
    const bool wantsShop = ShouldBotShop(bot, inventory, team, memory, botDifficulty_);
    const bool coreNeedsRepair = team.coreAlive && FindMissingCoreDefenseBlock(team).has_value();
    const BotRoleDistribution roleDistribution = BuildRoleDistributionForTeam(team.id, players_, botMemories_, bot.GetId());
    const BotRoleDecision roleDecision = EvaluateDynamicRoleDecision(
        bot,
        team,
        memory,
        roleDistribution,
        coreNeedsRepair,
        carryingLoot,
        wantsShop,
        enemyAtCore,
        enemyCore,
        matchTime_,
        inventory);
    if (TryApplyDynamicRoleDecision(memory, roleDecision, botDifficulty_))
    {
        AddEventMessage(bot.GetName() + " role -> " + ToString(memory.role), GetTeamColor(team.color), 1.2f);
    }

    Player* nearbyEnemy = FindNearbyEnemyPlayer(bot, BotEngageRange(memory.role, botDifficulty_));
    const float huntRange = matchTime_ > 120.0f
        ? (botDifficulty_ == BotDifficulty::Hard ? 168.0f : 142.0f)
        : (botDifficulty_ == BotDifficulty::Hard ? 128.0f : 108.0f);
    Player* huntEnemy = FindNearbyEnemyPlayer(bot, huntRange);
    Player* finalLifeEnemy = nullptr;
    float finalLifeEnemyDistanceSq = std::numeric_limits<float>::max();
    for (Player& candidate : players_)
    {
        if (candidate.GetId() == bot.GetId()
            || candidate.GetTeamId() == bot.GetTeamId()
            || !candidate.IsAlive()
            || candidate.IsEliminated())
        {
            continue;
        }

        const Team* candidateTeam = FindTeam(candidate.GetTeamId());
        if (candidateTeam == nullptr || candidateTeam->coreAlive)
        {
            continue;
        }

        const float distance = DistanceSquared(bot.GetPosition(), candidate.GetPosition());
        if (matchTime_ < 135.0f)
        {
            continue;
        }

        const float score = distance
            + static_cast<float>(candidate.GetHealth()) * 18.0f
            - static_cast<float>(std::max(0, bot.GetHealth() - candidate.GetHealth())) * 22.0f;
        if (score < finalLifeEnemyDistanceSq)
        {
            finalLifeEnemyDistanceSq = score;
            finalLifeEnemy = &candidate;
        }
    }
    if (finalLifeEnemy != nullptr)
    {
        huntEnemy = finalLifeEnemy;
        const float finalDistance = std::sqrt(DistanceSquared(bot.GetPosition(), finalLifeEnemy->GetPosition()));
        if (nearbyEnemy == nullptr || finalDistance < BotEngageRange(memory.role, botDifficulty_) * 1.8f)
        {
            nearbyEnemy = finalLifeEnemy;
        }
    }
    const int retreatHealth = BotRetreatHealth(memory.role, botDifficulty_, team.coreAlive);
    const int fightHealth = BotFightHealth(memory.role, botDifficulty_);
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
    const bool retreating = memory.retreatTimer > 0.0f && team.coreAlive;
    const bool defenderAwayFromBase = memory.role == BotRole::Defender
        && team.coreAlive
        && distanceFromHome > 40.0f;
    const int desiredBlocks = BotDesiredBlocks(memory.role);
    const ResourcePickup* bestPickup = FindBestPickupForBot(bot);
    const BotResourcePlan resourcePlan = BuildBotResourcePlan(bot, bestPickup, generators_, memory.role, arenaBiome_ == ArenaBiome::Ruins);
    const Vector3 resourceTarget = resourcePlan.target;
    const bool hasResourceTarget = resourcePlan.hasTarget;
    const ResourceType resourceTargetType = resourcePlan.type;
    const float nearbyEnemyDistance = nearbyEnemy != nullptr ? std::sqrt(DistanceSquared(bot.GetPosition(), nearbyEnemy->GetPosition())) : 999.0f;
    const bool canBreakDefense = enemyCore != nullptr
        && enemyCore->IsAlive()
        && DistanceSquared(bot.GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 28.0f
        && FindCoreDefenseBlock(*enemyCore, bot).has_value();
    const bool closeToEnemyCore = enemyCore != nullptr
        && DistanceSquared(botPos, world_.GridToWorld(enemyCore->GetBlockPosition())) < 150.0f;
    const bool bridgeKitReady = inventory.GetBlocks() >= (memory.role == BotRole::Rusher ? 12 : 18)
        || (closeToEnemyCore && inventory.GetBlocks() >= 3);
    const bool combatKitReady = inventory.GetToolLevel() > 0
        || inventory.GetSwordLevel() > 0
        || inventory.GetUtility(UtilityType::Fireball) > 0
        || (memory.role == BotRole::Rusher && matchTime_ > 12.0f)
        || (memory.role == BotRole::Fighter && matchTime_ > 24.0f)
        || matchTime_ > rushTime;
    const bool readyToRush = bridgeKitReady && combatKitReady;
    const bool coreCanUpgrade = team.coreAlive
        && memory.role == BotRole::Defender
        && FindUpgradeableCoreDefenseBlock(team, bot).has_value();
    const Team* huntEnemyTeam = huntEnemy != nullptr ? FindTeam(huntEnemy->GetTeamId()) : nullptr;
    const bool huntEnemyOnFinalLife = huntEnemyTeam != nullptr && !huntEnemyTeam->coreAlive;
    int finalLifeTeamAlive = 0;
    if (huntEnemyOnFinalLife)
    {
        for (const Player& candidate : players_)
        {
            if (candidate.GetTeamId() == huntEnemy->GetTeamId()
                && candidate.IsAlive()
                && !candidate.IsEliminated())
            {
                ++finalLifeTeamAlive;
            }
        }
    }
    const float huntEnemyDistance = huntEnemy != nullptr ? std::sqrt(DistanceSquared(bot.GetPosition(), huntEnemy->GetPosition())) : 999.0f;
    const bool lastFinalLifeEnemy = huntEnemyOnFinalLife && finalLifeTeamAlive <= 1;
    const bool finalLifeTargetClose = huntEnemyOnFinalLife
        && (huntEnemyDistance < 92.0f
            || (matchTime_ > 165.0f && huntEnemyDistance < 150.0f)
            || (lastFinalLifeEnemy && matchTime_ > 145.0f)
            || matchTime_ > 165.0f);
    const bool finalDuelPhase = huntEnemy != nullptr && (!team.coreAlive || enemyCore == nullptr || huntEnemyOnFinalLife);
    const bool defenderThreat = memory.role == BotRole::Defender
        && (enemyAtCore != nullptr || (nearbyEnemy != nullptr && distanceFromHome < 72.0f));
    const bool vulnerableEnemy = nearbyEnemy != nullptr
        && nearbyEnemy->GetHealth() <= bot.GetHealth() - (botDifficulty_ == BotDifficulty::Easy ? 34 : 18);
    const bool enemyBlockingObjective = nearbyEnemy != nullptr
        && enemyCore != nullptr
        && DistanceSquared(nearbyEnemy->GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 36.0f;
    const bool shouldFightNearby = nearbyEnemy != nullptr
        && bot.GetHealth() > fightHealth
        && (memory.role == BotRole::Fighter
            || defenderThreat
            || vulnerableEnemy
            || (huntEnemyOnFinalLife && nearbyEnemy == huntEnemy)
            || enemyBlockingObjective
            || (memory.role == BotRole::Rusher && nearbyEnemyDistance < 4.6f)
            || (memory.role == BotRole::Collector && nearbyEnemyDistance < 3.0f && bot.GetHealth() > 68));
    const bool cleanupOverEconomy = huntEnemyOnFinalLife && finalLifeTargetClose && matchTime_ > 155.0f;
    const bool shouldPressureCore = enemyCore != nullptr
        && enemyCore->IsAlive()
        && (memory.role == BotRole::Rusher || memory.role == BotRole::Fighter)
        && (readyToRush || (matchTime_ > 95.0f && inventory.GetBlocks() >= 6))
        && !retreating
        && !cleanupOverEconomy;

    const Vector3 homeTarget { coreHome.x, 1.5f, coreHome.z };
    const Vector3 shopTarget { team.shopPosition.x, 1.5f, team.shopPosition.z };
    const Vector3 spawnTarget { team.spawnPoint.x, 1.5f, team.spawnPoint.z };
    const BotDecisionContext decisionContext {
        bot,
        team,
        inventory,
        memory,
        botDifficulty_,
        matchTime_,
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
        coreCanUpgrade,
        finalDuelPhase,
        huntEnemyOnFinalLife,
        finalLifeTargetClose,
        shouldFightNearby,
        shouldPressureCore,
        hasResourceTarget
    };
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
    ApplyBotDecision(memory, decision, botDifficulty_);

    if (memory.intent == BotIntent::RepairCoreDefense && TryBotRepairCoreDefense(bot, team, dt))
    {
        return;
    }

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
    if (memory.state != BotState::Fight && !objectivePush)
    {
        target = ChooseBotPathWaypoint(bot, target, dt);
    }

    BotMovementPlan movementPlan = BuildBotMovementPlan(
        bot,
        memory,
        botDifficulty_,
        matchTime_,
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
    bot.Move(
        wish,
        jump,
        dt,
        world_,
        sprint,
        false,
        TerrainSpeedMultiplier(bot),
        true,
        BiomeGravityMultiplier(),
        BiomeJumpMultiplier(),
        BiomeGroundControlMultiplier(bot),
        BiomeAirControlMultiplier());
    ApplyStandingBlockEffects(bot, false);

    const float movedDistance = DistanceSquared(beforeMove, bot.GetPosition());
    UpdateBotStuckAfterMove(memory, wish, movedDistance, dt, bot.GetPosition());

    if ((memory.intent == BotIntent::GearUp || memory.state == BotState::Shop || wantsShop)
        && nearbyEnemy == nullptr)
    {
        BotTryShop(bot, team);
    }

    if (BotUseUtility(bot, team, fightTarget, enemyCore))
    {
        memory.utilityTimer = BotUtilityCooldown(botDifficulty_);
    }

    TryPerformBotMeleeAttack(
        bot,
        memory,
        botDifficulty_,
        matchTime_,
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
            [this](const EnergyCore& core)
            {
                world_.RemoveBlock(core.GetBlockPosition());
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

Vector3 Game::ChooseBotPathWaypoint(Player& bot, Vector3 finalTarget, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
    const Inventory& inventory = bot.GetInventory();
    const bool defendingCore = memory.role == BotRole::Defender
        && (memory.intent == BotIntent::DefendCore || memory.intent == BotIntent::RepairCoreDefense);
    const bool pushingObjective = memory.intent == BotIntent::PressureCore
        || memory.intent == BotIntent::BreakCoreDefense
        || memory.state == BotState::AttackCore
        || memory.state == BotState::BreakDefense;
    const bool retreating = memory.intent == BotIntent::RetreatHome;
    const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_);
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

    const auto combatPower = [](const Player& player)
    {
        const Inventory& inv = player.GetInventory();
        float score = static_cast<float>(player.GetHealth()) * 1.1f;
        score += static_cast<float>(inv.GetSwordLevel()) * 18.0f;
        score += static_cast<float>(inv.GetArmorLevel()) * 14.0f;
        score += static_cast<float>(inv.GetToolLevel()) * 8.0f;
        score += static_cast<float>(inv.GetUtility(UtilityType::Fireball)) * 7.0f;
        score += static_cast<float>(inv.GetUtility(UtilityType::Dash)) * 6.0f;
        score += static_cast<float>(inv.GetUtility(UtilityType::Molotov)) * 5.0f;
        score += player.GetSpeedBoostTimer() > 0.0f ? 9.0f : 0.0f;
        return score;
    };

    const float botPower = combatPower(bot);
    const Player* duelEnemy = FindNearbyEnemyPlayer(bot, defendingCore ? 30.0f : 16.0f);
    const float enemyPower = duelEnemy != nullptr ? combatPower(*duelEnemy) : 0.0f;
    const bool canTakeFight = defendingCore || duelEnemy == nullptr || botPower + 6.0f >= enemyPower;
    const int blockCount = inventory.GetBlocks();
    const int toolLevel = inventory.GetToolLevel();
    const bool canBridge = blockCount >= (memory.role == BotRole::Rusher ? 3 : (conservativeRoute ? 6 : 4));
    struct ThreatSample
    {
        Vector3 position {};
        float power = 0.0f;
    };
    std::vector<ThreatSample> threatSamples;
    threatSamples.reserve(players_.size());
    for (const Player& other : players_)
    {
        if (other.GetTeamId() == bot.GetTeamId() || !other.IsAlive() || other.IsEliminated())
        {
            continue;
        }
        threatSamples.push_back(ThreatSample { other.GetPosition(), combatPower(other) });
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
    std::priority_queue<OpenNode, std::vector<OpenNode>, decltype(cmp)> open(cmp);
    std::unordered_map<long long, float> bestCost;
    std::unordered_map<long long, GridPos> parent;
    if (!nodeTravelCost(start).has_value())
    {
        return finalTarget;
    }
    open.push(OpenNode { start, 0.0f, heuristic(start) });
    bestCost[PathKey(start.x, start.y, start.z)] = 0.0f;

    std::optional<GridPos> bestGoal;
    int bestGoalScore = goalScore(start);
    int expansions = 0;
    while (!open.empty() && expansions++ < maxExpansions)
    {
        const OpenNode current = open.top();
        open.pop();
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

        const GridPos offsets[] {
            GridPos { 1, 0, 0 },
            GridPos { -1, 0, 0 },
            GridPos { 0, 0, 1 },
            GridPos { 0, 0, -1 }
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
                open.push(OpenNode { next, nextG, nextG + heuristic(next) });
            }
        }
    }

    if (!bestGoal.has_value() || bestGoalScore > 5)
    {
        return finalTarget;
    }

    std::vector<GridPos> path;
    GridPos step = *bestGoal;
    path.push_back(step);
    std::unordered_map<long long, bool> seenPath;
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
    for (const Generator& generator : generators_)
    {
        if (generator.GetTeamId() != -1)
        {
            continue;
        }

        const ResourceType type = generator.GetType();
        const float bias = type == ResourceType::Crystal ? -95.0f : -42.0f;
        consider(generator.GetPosition(), bias);
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
    const bool xDominant = std::fabs(direction.x) >= std::fabs(direction.z);
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
        std::fabs(direction.x) > std::fabs(direction.z) ? 0 : memory.strafeSign,
        0,
        std::fabs(direction.x) > std::fabs(direction.z) ? memory.strafeSign : 0
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
        bot.SetYaw(YawFromDirection(breakAim));
    }
    memory.breakProgress += dt / BreakSeconds(block->type, bot.GetInventory().GetToolLevel());
    if (memory.breakProgress < 1.0f)
    {
        return true;
    }

    const Vector3 targetPos = world_.GridToWorld(target);
    if (world_.BreakBlock(target, bot.GetTeamId()))
    {
        AddWorldEffect(targetPos, Color { 210, 220, 235, 255 }, 0.24f, 0.22f);
        AddFloatingText("break", targetPos, Color { 210, 220, 235, 255 });
        audio_.PlayBreakBlock();
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
        if (inventory.GetUtility(UtilityType::Arrows) < 8)
        {
            priorities.push_back(104);
        }
        if (inventory.GetUtility(UtilityType::Dash) < 1)
        {
            priorities.push_back(205);
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

EnergyCore* Game::FindNearestEnemyCore(const Player& player)
{
    EnergyCore* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();

    for (EnergyCore& core : cores_)
    {
        if (core.GetTeamId() == player.GetTeamId() || !core.IsAlive())
        {
            continue;
        }

        const Vector3 corePos = world_.GridToWorld(core.GetBlockPosition());
        const float distance = DistanceSquared(player.GetPosition(), corePos);
        const float weaknessBonus = static_cast<float>(core.GetMaxHealth() - core.GetHealth()) * 1.8f;
        const float score = distance - weaknessBonus;
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
    const BotRole role = ResolveRoleForPlayerId(player.GetId(), botMemories_);
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
    for (BotMemory& memory : botMemories_)
    {
        if (memory.playerId == bot.GetId())
        {
            return memory;
        }
    }

    const BotRole role = RoleForBotId(bot.GetId());
    BotMemory memory {};
    memory.playerId = bot.GetId();
    memory.state = BotState::Collect;
    memory.role = role;
    memory.intent = role == BotRole::Defender ? BotIntent::RepairCoreDefense : BotIntent::SecureResources;
    memory.intentReason = "spawn plan";
    memory.roleReason = "spawn role";
    botMemories_.push_back(memory);
    return botMemories_.back();
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
        bot.SetYaw(YawFromDirection(wish));
        if (IsVoidThreatAt(Vector3 { bot.GetPosition().x + wish.x * 0.85f, bot.GetPosition().y, bot.GetPosition().z + wish.z * 0.85f }))
        {
            TryBotBridgeBlock(bot, targetPos);
        }
        bot.Move(
            wish,
            false,
            dt,
            world_,
            false,
            false,
            TerrainSpeedMultiplier(bot),
            true,
            BiomeGravityMultiplier(),
            BiomeJumpMultiplier(),
            BiomeGroundControlMultiplier(bot),
            BiomeAirControlMultiplier());
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

    if (world_.BreakBlock(target, bot.GetTeamId()))
    {
        AddWorldEffect(targetPos, GetTeamColor(team.color), 0.26f, 0.24f);
        AddFloatingText("upgrade", targetPos, GetTeamColor(team.color));
        AddEventMessage(bot.GetName() + " upgraded Core defense", GetTeamColor(team.color), 1.5f);
        audio_.PlayBreakBlock();
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
    for (const GridPos& candidate : CoreDefensePositions(team.coreBlock))
    {
        if (!world_.IsAir(candidate))
        {
            continue;
        }

        std::string reason;
        if (CanPlaceBlockAt(candidate, bot, &reason))
        {
            missing = candidate;
            break;
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
        bot.SetYaw(YawFromDirection(wish));
        if (IsVoidThreatAt(Vector3 { bot.GetPosition().x + wish.x * 0.85f, bot.GetPosition().y, bot.GetPosition().z + wish.z * 0.85f }))
        {
            TryBotBridgeBlock(bot, target);
        }
        bot.Move(
            wish,
            false,
            dt,
            world_,
            false,
            false,
            TerrainSpeedMultiplier(bot),
            true,
            BiomeGravityMultiplier(),
            BiomeJumpMultiplier(),
            BiomeGroundControlMultiplier(bot),
            BiomeAirControlMultiplier());
        ApplyStandingBlockEffects(bot, false);
        return true;
    }

    if (TryPlaceBlockForPlayer(bot, *missing, false))
    {
        AddEventMessage(bot.GetName() + " repaired Core defense", GetTeamColor(team.color), 1.5f);
        return true;
    }
    return false;
}

const ResourcePickup* Game::FindBestPickupForBot(const Player& bot) const
{
    const ResourcePickup* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();
    const Inventory& inventory = bot.GetInventory();
    const BotRole role = ResolveRoleForPlayerId(bot.GetId(), botMemories_);

    for (const ResourcePickup& pickup : pickups_)
    {
        if (pickup.collected)
        {
            continue;
        }

        float score = DistanceSquared(bot.GetPosition(), pickup.position);
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
        for (const Player& enemy : players_)
        {
            if (enemy.GetTeamId() == bot.GetTeamId() || !enemy.IsAlive() || enemy.IsEliminated())
            {
                continue;
            }

            const float enemyDistance = DistanceSquared(enemy.GetPosition(), pickup.position);
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
            const Team* team = FindTeam(bot.GetTeamId());
            if (team != nullptr)
            {
                score += DistanceSquared(pickup.position, world_.GridToWorld(team->coreBlock)) * 0.35f;
            }
        }
        if (arenaBiome_ == ArenaBiome::Ruins
            && role == BotRole::Collector
            && std::fabs(pickup.position.x) >= 18.0f
            && std::fabs(pickup.position.z) >= 18.0f)
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


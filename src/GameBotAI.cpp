#include "Game.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
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
    int value = role == BotRole::Collector ? 18 : 30;
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

        BotMemory& memory = GetBotMemory(bot);
        memory.stateTimer += dt;
        memory.intentTimer += dt;
        memory.intentLockTimer = std::max(0.0f, memory.intentLockTimer - dt);
        memory.attackTimer = std::max(0.0f, memory.attackTimer - dt);
        memory.retreatTimer = std::max(0.0f, memory.retreatTimer - dt);
        memory.strafeTimer = std::max(0.0f, memory.strafeTimer - dt);
        memory.bridgePlaceCooldown = std::max(0.0f, memory.bridgePlaceCooldown - dt);
        memory.utilityTimer = std::max(0.0f, memory.utilityTimer - dt);
        memory.jumpTimer = std::max(0.0f, memory.jumpTimer - dt);
        memory.carriedResourceValue = CarriedResourceValue(bot.GetInventory());

        EnergyCore* enemyCore = FindNearestEnemyCore(bot);
        Player* nearbyEnemy = FindNearbyEnemyPlayer(bot, BotEngageRange(memory.role, botDifficulty_));
        Player* huntEnemy = FindNearbyEnemyPlayer(bot, botDifficulty_ == BotDifficulty::Hard ? 118.0f : 96.0f);
        const int retreatHealth = BotRetreatHealth(memory.role, botDifficulty_, team->coreAlive);
        const int fightHealth = BotFightHealth(memory.role, botDifficulty_);
        const float rushTime = botDifficulty_ == BotDifficulty::Hard ? 38.0f : (botDifficulty_ == BotDifficulty::Easy ? 115.0f : 70.0f);
        const Vector3 coreHome = world_.GridToWorld(team->coreBlock);
        const Vector3 botPos = bot.GetPosition();
        const float distanceFromHome = DistanceSquared(bot.GetPosition(), coreHome);
        BotTeamSnapshot teamPlan {};
        for (const Player& ally : players_)
        {
            if (ally.GetId() == bot.GetId()
                || ally.GetTeamId() != bot.GetTeamId()
                || !ally.IsAlive()
                || ally.IsEliminated())
            {
                continue;
            }

            ++teamPlan.aliveAllies;
            const bool nearCore = DistanceSquared(ally.GetPosition(), coreHome) < 72.0f;
            if (nearCore)
            {
                ++teamPlan.alliesNearCore;
            }

            const BotMemory* allyMemory = nullptr;
            for (const BotMemory& candidate : botMemories_)
            {
                if (candidate.playerId == ally.GetId())
                {
                    allyMemory = &candidate;
                    break;
                }
            }

            if (allyMemory == nullptr)
            {
                continue;
            }

            if (nearCore && allyMemory->role == BotRole::Defender)
            {
                ++teamPlan.defendersNearCore;
            }
            if (allyMemory->intent == BotIntent::PressureCore
                || allyMemory->intent == BotIntent::BreakCoreDefense
                || allyMemory->intent == BotIntent::ChaseWeakEnemy)
            {
                ++teamPlan.activePressure;
            }
            else if (allyMemory->intent == BotIntent::GearUp)
            {
                ++teamPlan.activeShop;
            }
            else if (allyMemory->intent == BotIntent::RepairCoreDefense)
            {
                ++teamPlan.activeRepair;
            }
            else if (allyMemory->intent == BotIntent::SecureResources)
            {
                ++teamPlan.activeResource;
            }
        }

        Player* enemyAtCore = nullptr;
        float enemyAtCoreDistance = std::numeric_limits<float>::max();
        for (Player& other : players_)
        {
            if (other.GetTeamId() == bot.GetTeamId()
                || other.GetId() == bot.GetId()
                || !other.IsAlive()
                || other.IsEliminated())
            {
                continue;
            }

            const float coreDistance = DistanceSquared(other.GetPosition(), coreHome);
            if (coreDistance < 42.0f && coreDistance < enemyAtCoreDistance)
            {
                enemyAtCore = &other;
                enemyAtCoreDistance = coreDistance;
            }
        }

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

        if (bot.GetHealth() < retreatHealth && team->coreAlive)
        {
            memory.retreatTimer = std::max(memory.retreatTimer, botDifficulty_ == BotDifficulty::Hard ? 1.25f : 2.1f);
        }
        const bool retreating = memory.retreatTimer > 0.0f && team->coreAlive;
        const bool defenderAwayFromBase = memory.role == BotRole::Defender
            && team->coreAlive
            && distanceFromHome > 40.0f;
        const Inventory& inventory = bot.GetInventory();
        const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_);
        const int desiredBlocks = BotDesiredBlocks(memory.role);
        const bool canBuyBlocks = inventory.GetResource(ResourceType::Iron) >= 8;
        const bool hasObsidianMoney = inventory.GetResource(ResourceType::Gold) >= 8 && inventory.GetResource(ResourceType::Crystal) >= 3;
        const bool wantsShop = (inventory.GetBlocks() < desiredBlocks && canBuyBlocks)
            || (memory.role != BotRole::Defender && inventory.GetToolLevel() < 2 && inventory.GetResource(ResourceType::Iron) >= 8 && inventory.GetResource(ResourceType::Crystal) >= 1)
            || (memory.role == BotRole::Fighter && inventory.GetSwordLevel() < 2 && inventory.GetResource(ResourceType::Gold) >= 6)
            || (memory.role == BotRole::Defender && inventory.GetBlockCount(BlockType::StoneBlock) < 16 && inventory.GetResource(ResourceType::Iron) >= 18)
            || (memory.role == BotRole::Defender && inventory.GetBlockCount(BlockType::ObsidianBlock) < 8 && hasObsidianMoney)
            || (memory.role == BotRole::Collector && team->forgeLevel < 3 && inventory.GetResource(ResourceType::Crystal) >= 4 && inventory.GetResource(ResourceType::Gold) >= 2)
            || (bot.GetHealth() < 70 && inventory.GetResource(ResourceType::Crystal) >= 3)
            || carryingLoot;
        const ResourcePickup* bestPickup = FindBestPickupForBot(bot);
        Vector3 resourceTarget {};
        bool hasResourceTarget = false;
        ResourceType resourceTargetType = ResourceType::Iron;
        float bestResourceScore = std::numeric_limits<float>::max();
        if (bestPickup != nullptr)
        {
            resourceTarget = bestPickup->position;
            resourceTargetType = bestPickup->type;
            hasResourceTarget = true;
            bestResourceScore = DistanceSquared(botPos, resourceTarget);
        }
        for (const Generator& generator : generators_)
        {
            if (generator.GetTeamId() != -1)
            {
                continue;
            }

            const ResourceType type = generator.GetType();
            float score = DistanceSquared(botPos, generator.GetPosition());
            if (type == ResourceType::Crystal)
            {
                score -= memory.role == BotRole::Collector ? 420.0f : 180.0f;
            }
            else if (type == ResourceType::Gold)
            {
                score -= memory.role == BotRole::Fighter ? 120.0f : 70.0f;
            }
            if (memory.role == BotRole::Collector)
            {
                score -= DistanceSquared(generator.GetPosition(), Vector3 { 0.0f, generator.GetPosition().y, 0.0f }) * 0.08f;
            }
            if (!hasResourceTarget || score < bestResourceScore)
            {
                resourceTarget = generator.GetPosition();
                resourceTargetType = type;
                hasResourceTarget = true;
                bestResourceScore = score;
            }
        }
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
        const bool coreNeedsRepair = team->coreAlive && FindMissingCoreDefenseBlock(*team).has_value();
        const bool coreCanUpgrade = team->coreAlive
            && memory.role == BotRole::Defender
            && FindUpgradeableCoreDefenseBlock(*team, bot).has_value();
        const bool finalDuelPhase = huntEnemy != nullptr && (!team->coreAlive || enemyCore == nullptr);
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
                || enemyBlockingObjective
                || (memory.role == BotRole::Rusher && nearbyEnemyDistance < 4.6f)
                || (memory.role == BotRole::Collector && nearbyEnemyDistance < 3.0f && bot.GetHealth() > 68));
        const bool shouldPressureCore = enemyCore != nullptr
            && enemyCore->IsAlive()
            && (memory.role == BotRole::Rusher || memory.role == BotRole::Fighter)
            && readyToRush
            && !retreating;

        BotDecision decision {};
        decision.target = Vector3 { 0.0f, 1.5f, 0.0f };
        const auto consider = [&](BotIntent intent, float score, Vector3 target, Player* fightTarget, const std::string& reason)
        {
            if (memory.intent == intent)
            {
                score += 105.0f + std::min(45.0f, memory.intentTimer * 12.0f);
            }
            else if (memory.intentLockTimer > 0.0f
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

        const Vector3 homeTarget { coreHome.x, 1.5f, coreHome.z };
        const Vector3 shopTarget { team->shopPosition.x, 1.5f, team->shopPosition.z };
        const Vector3 spawnTarget { team->spawnPoint.x, 1.5f, team->spawnPoint.z };

        if (memory.stuckTimer > 1.05f)
        {
            const Vector3 awayFromCenter = Normalize2D(Vector3 { botPos.x, 0.0f, botPos.z });
            Vector3 recoverTarget = Length2D(awayFromCenter) > 0.0001f
                ? Vector3 { botPos.x + awayFromCenter.x * 3.0f, 1.5f, botPos.z + awayFromCenter.z * 3.0f }
                : homeTarget;
            if (DistanceSquared(recoverTarget, homeTarget) > DistanceSquared(botPos, homeTarget) + 36.0f)
            {
                recoverTarget = homeTarget;
            }
            consider(BotIntent::Recover, 980.0f + memory.stuckTimer * 120.0f, recoverTarget, nullptr, "unstick");
        }

        if (enemyAtCore != nullptr)
        {
            const float defenderBonus = memory.role == BotRole::Defender ? 220.0f : 80.0f;
            const float lonelyBaseBonus = teamPlan.alliesNearCore == 0 ? 130.0f : 0.0f;
            consider(
                BotIntent::DefendCore,
                1180.0f + defenderBonus + lonelyBaseBonus - std::sqrt(enemyAtCoreDistance) * 10.0f,
                enemyAtCore->GetPosition(),
                enemyAtCore,
                "enemy at core");
        }

        if (retreating || defenderAwayFromBase)
        {
            const float lootBonus = carryingLoot ? static_cast<float>(memory.carriedResourceValue) * 4.0f : 0.0f;
            const float healthBonus = static_cast<float>(std::max(0, retreatHealth + 18 - bot.GetHealth())) * 9.0f;
            consider(
                BotIntent::RetreatHome,
                760.0f + lootBonus + healthBonus + (defenderAwayFromBase ? 180.0f : 0.0f),
                memory.role == BotRole::Defender ? homeTarget : shopTarget,
                nullptr,
                defenderAwayFromBase ? "return to base" : "heal and bank");
        }

        if ((coreNeedsRepair || coreCanUpgrade) && team->coreAlive)
        {
            const bool assignedBuilder = memory.role == BotRole::Defender
                || (teamPlan.defendersNearCore == 0 && teamPlan.activeRepair == 0 && distanceFromHome < 120.0f);
            const float defenderBonus = memory.role == BotRole::Defender ? 280.0f : 0.0f;
            const float repairCrowdPenalty = static_cast<float>(teamPlan.activeRepair) * 115.0f;
            if (assignedBuilder)
            {
                consider(
                    BotIntent::RepairCoreDefense,
                    500.0f + defenderBonus + (coreCanUpgrade ? 90.0f : 0.0f) - repairCrowdPenalty - std::sqrt(distanceFromHome) * 5.0f,
                    homeTarget,
                    nullptr,
                    coreCanUpgrade ? "upgrade defense" : "repair defense");
            }
        }

        if (wantsShop || inventory.GetBlocks() < 3)
        {
            const float blockPressure = static_cast<float>(std::max(0, desiredBlocks - inventory.GetBlocks())) * 7.0f;
            const float lootPressure = carryingLoot ? static_cast<float>(memory.carriedResourceValue) * 3.4f : 0.0f;
            const float shopCrowdPenalty = static_cast<float>(teamPlan.activeShop) * 72.0f;
            consider(
                BotIntent::GearUp,
                470.0f + blockPressure + lootPressure + (bot.GetHealth() < 70 ? 120.0f : 0.0f) - shopCrowdPenalty,
                shopTarget,
                nullptr,
                "buy gear");
        }

        if (shouldFightNearby && nearbyEnemy != nullptr)
        {
            const float roleBonus = memory.role == BotRole::Fighter ? 150.0f : (memory.role == BotRole::Defender ? 85.0f : 0.0f);
            const float healthEdge = static_cast<float>(bot.GetHealth() - nearbyEnemy->GetHealth()) * 2.8f;
            consider(
                BotIntent::FightEnemy,
                520.0f + roleBonus + healthEdge - nearbyEnemyDistance * 18.0f,
                nearbyEnemy->GetPosition(),
                nearbyEnemy,
                "take fight");
        }

        if (weakEnemy != nullptr && bot.GetHealth() > fightHealth)
        {
            const float weakDistance = std::sqrt(DistanceSquared(bot.GetPosition(), weakEnemy->GetPosition()));
            consider(
                BotIntent::ChaseWeakEnemy,
                500.0f + static_cast<float>(bot.GetHealth() - weakEnemy->GetHealth()) * 3.8f - weakDistance * 8.0f,
                weakEnemy->GetPosition(),
                weakEnemy,
                "finish weak enemy");
        }

        if (canBreakDefense && enemyCore != nullptr)
        {
            const std::optional<GridPos> defense = FindCoreDefenseBlock(*enemyCore, bot);
            if (defense.has_value())
            {
                const Vector3 defensePos = world_.GridToWorld(*defense);
                consider(
                    BotIntent::BreakCoreDefense,
                    650.0f + (memory.role == BotRole::Rusher ? 120.0f : 0.0f),
                    Vector3 { defensePos.x, 1.5f, defensePos.z },
                    nullptr,
                    "crack defense");
            }
        }

        if (shouldPressureCore && enemyCore != nullptr)
        {
            const Vector3 corePos = world_.GridToWorld(enemyCore->GetBlockPosition());
            const float exposedBonus = HasBotCoreAccess(bot, *enemyCore) ? 180.0f : 0.0f;
            const float teamPushBonus = teamPlan.activePressure > 0 ? 70.0f : 0.0f;
            const float baseCoveredBonus = teamPlan.alliesNearCore > 0 || !team->coreAlive ? 60.0f : -85.0f;
            consider(
                BotIntent::PressureCore,
                570.0f + exposedBonus + teamPushBonus + baseCoveredBonus + (memory.role == BotRole::Rusher ? 150.0f : 60.0f),
                Vector3 { corePos.x, 1.5f, corePos.z },
                nullptr,
                "rush core");
        }

        if (hasResourceTarget && !carryingLoot)
        {
            const float resourceBonus = resourceTargetType == ResourceType::Crystal
                ? (memory.role == BotRole::Collector ? 190.0f : 120.0f)
                : (resourceTargetType == ResourceType::Gold ? 105.0f : 45.0f);
            const float collectorBonus = memory.role == BotRole::Collector ? 260.0f : 0.0f;
            const float resourceCrowdPenalty = static_cast<float>(teamPlan.activeResource) * (memory.role == BotRole::Collector ? 12.0f : 58.0f);
            const float blockShortagePenalty = inventory.GetBlocks() < 6 ? 360.0f : 0.0f;
            consider(
                BotIntent::SecureResources,
                390.0f + resourceBonus + collectorBonus - resourceCrowdPenalty - blockShortagePenalty,
                resourceTarget,
                nullptr,
                resourceTargetType == ResourceType::Crystal ? "center crystals" : "secure resources");
        }

        if (finalDuelPhase && huntEnemy != nullptr && bot.GetHealth() > fightHealth)
        {
            consider(
                BotIntent::ChaseWeakEnemy,
                430.0f + (team->coreAlive ? 0.0f : 120.0f),
                huntEnemy->GetPosition(),
                huntEnemy,
                "final duel");
        }

        if (enemyCore != nullptr && enemyCore->IsAlive() && readyToRush)
        {
            const Vector3 corePos = world_.GridToWorld(enemyCore->GetBlockPosition());
            const float pressureCrowdPenalty = static_cast<float>(std::max(0, teamPlan.activePressure - 1)) * 46.0f;
            consider(
                BotIntent::PressureCore,
                360.0f + (memory.role == BotRole::Rusher ? 170.0f : 0.0f) - pressureCrowdPenalty,
                Vector3 { corePos.x, 1.5f, corePos.z },
                nullptr,
                "map pressure");
        }

        if (decision.score < -9990.0f)
        {
            consider(BotIntent::SecureResources, 0.0f, Vector3 { 0.0f, 1.5f, 0.0f }, nullptr, "default mid");
        }

        if (decision.intent != memory.intent)
        {
            memory.intent = decision.intent;
            memory.intentTimer = 0.0f;
            memory.intentLockTimer = IntentLockSeconds(decision.intent, botDifficulty_);
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
                memory.attackTimer = std::max(memory.attackTimer, BotFightReactionDelay(botDifficulty_));
            }
        }

        if (memory.intent == BotIntent::RepairCoreDefense && TryBotRepairCoreDefense(bot, *team, dt))
        {
            continue;
        }

        Vector3 target = decision.target;
        if (memory.intent == BotIntent::RetreatHome && memory.role != BotRole::Defender && bot.GetHealth() < 55)
        {
            target = spawnTarget;
        }

        if (memory.state != BotState::Fight && DistanceSquared(botPos, target) > 180.0f)
        {
            target = ChooseBotWaypoint(bot, target);
        }
        if (memory.state != BotState::Fight)
        {
            target = ChooseBotPathWaypoint(bot, target, dt);
        }

        Vector3 wish = Normalize2D(Vector3 { target.x - botPos.x, 0.0f, target.z - botPos.z });
        Player* fightTarget = memory.state == BotState::Fight ? decision.fightTarget : nullptr;
        Vector3 aimDirection = wish;
        float fightDistance = fightTarget != nullptr ? std::sqrt(DistanceSquared(bot.GetPosition(), fightTarget->GetPosition())) : 999.0f;
        if (fightTarget != nullptr)
        {
            const Vector3 toEnemy = Normalize2D(Vector3 {
                fightTarget->GetPosition().x - botPos.x,
                0.0f,
                fightTarget->GetPosition().z - botPos.z
            });
            const Vector3 side { -toEnemy.z, 0.0f, toEnemy.x };
            if (memory.strafeTimer <= 0.0f)
            {
                memory.strafeSign = -memory.strafeSign;
                memory.strafeTimer = botDifficulty_ == BotDifficulty::Hard ? 0.42f : (botDifficulty_ == BotDifficulty::Easy ? 0.78f : 0.56f);
            }

            const float desiredRange = botDifficulty_ == BotDifficulty::Hard ? 2.22f : (botDifficulty_ == BotDifficulty::Easy ? 1.82f : 2.05f);
            const bool shouldKite = bot.GetHealth() < fightTarget->GetHealth() - 18
                && memory.intent != BotIntent::DefendCore
                && memory.intent != BotIntent::ChaseWeakEnemy;
            const float forwardAmount = shouldKite
                ? (fightDistance < 4.2f ? -0.55f : 0.10f)
                : (fightDistance > desiredRange + 0.35f
                ? 1.0f
                : (fightDistance < desiredRange - 0.35f ? -0.55f : 0.16f));
            const bool highGroundRisk = IsVoidThreatAt(Vector3 { botPos.x + side.x * memory.strafeSign * 0.90f, botPos.y, botPos.z + side.z * memory.strafeSign * 0.90f });
            const float strafeScale = highGroundRisk ? 0.18f : 1.0f;
            const float strafeAmount = static_cast<float>(memory.strafeSign)
                * (botDifficulty_ == BotDifficulty::Hard ? 0.84f : (botDifficulty_ == BotDifficulty::Easy ? 0.32f : 0.58f))
                * strafeScale;
            wish = Normalize2D(Vector3 {
                toEnemy.x * forwardAmount + side.x * strafeAmount,
                0.0f,
                toEnemy.z * forwardAmount + side.z * strafeAmount
            });

            float aimError = botDifficulty_ == BotDifficulty::Easy ? 0.58f : (botDifficulty_ == BotDifficulty::Hard ? 0.24f : 0.38f);
            if (fightDistance < 2.55f || fightTarget->GetHealth() <= bot.GetHealth() - 20)
            {
                aimError *= botDifficulty_ == BotDifficulty::Easy ? 0.82f : 0.62f;
            }
            const float aimWave = std::sin(matchTime_ * (botDifficulty_ == BotDifficulty::Hard ? 5.1f : 3.7f) + static_cast<float>(bot.GetId()) * 1.73f);
            aimDirection = Normalize2D(Vector3 {
                toEnemy.x + side.x * aimError * (static_cast<float>(memory.strafeSign) * 0.55f + aimWave * 0.45f),
                0.0f,
                toEnemy.z + side.z * aimError * (static_cast<float>(memory.strafeSign) * 0.55f + aimWave * 0.45f)
            });
        }

        const Vector3 nextStep {
            botPos.x + wish.x * 0.95f,
            botPos.y,
            botPos.z + wish.z * 0.95f
        };
        const Vector3 wishSide { -wish.z, 0.0f, wish.x };
        const bool voidAhead = IsVoidThreatAt(nextStep);
        const bool voidLeft = Length2D(wish) > 0.0001f
            && IsVoidThreatAt(Vector3 { botPos.x + wish.x * 0.35f + wishSide.x * 0.78f, botPos.y, botPos.z + wish.z * 0.35f + wishSide.z * 0.78f });
        const bool voidRight = Length2D(wish) > 0.0001f
            && IsVoidThreatAt(Vector3 { botPos.x + wish.x * 0.35f - wishSide.x * 0.78f, botPos.y, botPos.z + wish.z * 0.35f - wishSide.z * 0.78f });
        const bool currentVoid = IsVoidThreatAt(botPos);
        const bool edgePressure = voidAhead || currentVoid;
        bool placedVoidBlock = false;
        if (voidAhead && bot.GetInventory().GetBlocks() > 0)
        {
            placedVoidBlock = TryBotBridgeBlock(bot, target);
        }
        Vector3 safetyWish = Normalize2D(Vector3 { coreHome.x - botPos.x, 0.0f, coreHome.z - botPos.z });
        if (Length2D(safetyWish) <= 0.0001f)
        {
            safetyWish = Normalize2D(Vector3 { -botPos.x, 0.0f, -botPos.z });
        }
        if (voidAhead && fightTarget != nullptr && !placedVoidBlock)
        {
            wish = safetyWish;
            if (bot.GetInventory().GetBlocks() > 0)
            {
                TryBotBridgeBlock(bot, Vector3 { botPos.x + safetyWish.x * 2.0f, botPos.y, botPos.z + safetyWish.z * 2.0f });
            }
        }
        else if (fightTarget != nullptr && !voidAhead)
        {
            if (voidLeft && !voidRight)
            {
                wish = Normalize2D(Vector3 { wish.x - wishSide.x * 0.45f, 0.0f, wish.z - wishSide.z * 0.45f });
            }
            else if (voidRight && !voidLeft)
            {
                wish = Normalize2D(Vector3 { wish.x + wishSide.x * 0.45f, 0.0f, wish.z + wishSide.z * 0.45f });
            }
        }
        else if (voidAhead && bot.GetInventory().GetBlocks() <= 0)
        {
            wish = safetyWish;
        }
        else if (voidAhead && !placedVoidBlock)
        {
            wish = Vector3 { safetyWish.x * 0.45f, 0.0f, safetyWish.z * 0.45f };
        }

        if (Length2D(aimDirection) > 0.0001f)
        {
            bot.SetYaw(YawFromDirection(aimDirection));
        }

        const bool shouldBridge = memory.state == BotState::Bridge
            || memory.state == BotState::AttackCore
            || memory.state == BotState::BreakDefense
            || memory.state == BotState::Collect
            || memory.state == BotState::Retreat
            || memory.intent == BotIntent::GearUp
            || memory.intent == BotIntent::Recover
            || memory.stuckTimer > 0.35f
            || edgePressure;
        if (shouldBridge)
        {
            TryBotBridgeBlock(bot, target);
        }

        bool jump = false;
        const GridPos stepBlock = world_.WorldToGrid(Vector3 { nextStep.x, botPos.y + 0.04f, nextStep.z });
        const GridPos stepHead { stepBlock.x, stepBlock.y + 1, stepBlock.z };
        const GridPos stepSupport = world_.WorldToGrid(Vector3 { nextStep.x, botPos.y - 1.08f, nextStep.z });
        const bool oneBlockObstacle = world_.IsSolid(stepBlock)
            && world_.IsAir(stepHead)
            && stepBlock.y > stepSupport.y;
        const bool shouldMineObstacle = memory.state == BotState::AttackCore
            || memory.state == BotState::BreakDefense
            || memory.state == BotState::Bridge
            || memory.intent == BotIntent::Recover
            || memory.stuckTimer > 0.20f;
        if (shouldMineObstacle && TryBotBreakBlockingBlock(bot, wish, dt))
        {
            continue;
        }
        if (memory.jumpTimer <= 0.0f
            && (oneBlockObstacle
                || memory.stuckTimer > 0.28f
                || (fightTarget != nullptr && fightDistance > 1.8f && fightDistance < 2.8f && botDifficulty_ == BotDifficulty::Hard && bot.IsOnGround())))
        {
            jump = true;
            memory.jumpTimer = oneBlockObstacle
                ? (botDifficulty_ == BotDifficulty::Hard ? 0.38f : 0.52f)
                : (botDifficulty_ == BotDifficulty::Hard ? 0.82f : 1.2f);
        }

        const bool sprint = botDifficulty_ != BotDifficulty::Easy
            && Length2D(wish) > 0.2f
            && !edgePressure
            && (memory.state == BotState::AttackCore || memory.state == BotState::Fight || memory.state == BotState::Collect || memory.intent == BotIntent::GearUp);
        const Vector3 beforeMove = bot.GetPosition();
        bot.Move(wish, jump, dt, world_, sprint, false, TerrainSpeedMultiplier(bot), true);
        ApplyStandingBlockEffects(bot, false);

        const float movedDistance = DistanceSquared(beforeMove, bot.GetPosition());
        if (Length2D(wish) > 0.1f && movedDistance < 0.0008f)
        {
            memory.stuckTimer += dt;
        }
        else
        {
            memory.stuckTimer = std::max(0.0f, memory.stuckTimer - dt * 1.8f);
        }
        memory.lastPosition = bot.GetPosition();

        if ((memory.intent == BotIntent::GearUp || memory.state == BotState::Shop || wantsShop)
            && nearbyEnemy == nullptr)
        {
            BotTryShop(bot, *team);
        }

        if (BotUseUtility(bot, *team, fightTarget, enemyCore))
        {
            memory.utilityTimer = BotUtilityCooldown(botDifficulty_);
        }

        if (fightTarget != nullptr && memory.attackTimer <= 0.0f && memory.stateTimer >= BotFightReactionDelay(botDifficulty_))
        {
            std::string combatMessage;
            CombatEvent combatEvent;
            WeaponType botWeapon = WeaponType::Sword;
            if (memory.role == BotRole::Defender && bot.GetInventory().HasItem(ItemType::Axe))
            {
                botWeapon = WeaponType::Axe;
            }
            else if (memory.role == BotRole::Collector && bot.GetInventory().HasItem(ItemType::Spear))
            {
                botWeapon = WeaponType::Spear;
            }

            AttackOptions attackOptions {};
            attackOptions.sprinting = sprint;
            attackOptions.sprintReset = sprint
                && botDifficulty_ != BotDifficulty::Easy
                && std::fmod(matchTime_ + static_cast<float>(bot.GetId()) * 0.37f, botDifficulty_ == BotDifficulty::Hard ? 1.20f : 1.80f) < 0.22f;
            attackOptions.attackerAirborne = !bot.IsOnGround();
            attackOptions.attackerVelocity = bot.GetVelocity();
            float botMeleeRayLimit = CombatSystem::AttackRange(botWeapon, bot.GetInventory().GetSwordLevel());
            const Vector3 botEye {
                bot.GetPosition().x,
                bot.GetPosition().y + 0.78f,
                bot.GetPosition().z
            };
            const std::optional<RaycastHit> botTerrainHit = world_.Raycast(botEye, aimDirection, botMeleeRayLimit);
            if (botTerrainHit.has_value())
            {
                botMeleeRayLimit = std::max(0.0f, botTerrainHit->distance - 0.06f);
            }
            if (combat_.Attack(bot, players_, aimDirection, combatMessage, &combatEvent, botWeapon, 1.0f, &attackOptions, botMeleeRayLimit))
            {
                RegisterCombatEvent(combatEvent, combatMessage);
                memory.attackTimer = BotAttackDelay(botDifficulty_);
            }
            else
            {
                memory.attackTimer = BotAttackDelay(botDifficulty_) * 0.85f;
            }
        }

        if (enemyCore != nullptr && enemyCore->IsAlive())
        {
            const Vector3 corePos = world_.GridToWorld(enemyCore->GetBlockPosition());
            if (DistanceSquared(bot.GetPosition(), corePos) < 5.0f)
            {
                if (TryBotBreakCoreDefense(bot, *enemyCore, dt))
                {
                    continue;
                }
                if (!HasBotCoreAccess(bot, *enemyCore))
                {
                    continue;
                }

                std::string coreMessage;
                CombatEvent coreEvent;
                if (combat_.DamageCore(bot, *enemyCore, coreMessage, &coreEvent))
                {
                    memory.lastAttackedCoreTeamId = enemyCore->GetTeamId();
                    if (!enemyCore->IsAlive())
                    {
                        world_.RemoveBlock(enemyCore->GetBlockPosition());
                        Team* destroyedTeam = FindTeam(enemyCore->GetTeamId());
                        if (destroyedTeam != nullptr)
                        {
                            destroyedTeam->coreAlive = false;
                        }
                    }
                    RegisterCombatEvent(coreEvent, coreMessage);
                }
            }
        }
    }
}

Vector3 Game::ChooseBotPathWaypoint(Player& bot, Vector3 finalTarget, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
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
    memory.navTimer = 0.32f;

    const GridPos start = world_.WorldToGrid(Vector3 { bot.GetPosition().x, bot.GetPosition().y - 1.08f, bot.GetPosition().z });
    const GridPos rawGoal = world_.WorldToGrid(Vector3 { finalTarget.x, finalTarget.y - 1.08f, finalTarget.z });
    constexpr int kPathRadius = 42;
    constexpr int kMaxExpansions = 2200;
    const int minX = start.x - kPathRadius;
    const int maxX = start.x + kPathRadius;
    const int minZ = start.z - kPathRadius;
    const int maxZ = start.z + kPathRadius;

    const auto inBounds = [&](const GridPos& pos)
    {
        return pos.x >= minX && pos.x <= maxX && pos.z >= minZ && pos.z <= maxZ && pos.y >= kBotPathMinY && pos.y <= kBotPathMaxY;
    };
    const auto hasHeadRoom = [&](const GridPos& support)
    {
        return world_.IsAir(GridPos { support.x, support.y + 1, support.z })
            && world_.IsAir(GridPos { support.x, support.y + 2, support.z });
    };
    const auto isWalkSupport = [&](const GridPos& support)
    {
        return world_.IsSolid(support) && hasHeadRoom(support);
    };
    const auto nodeCost = [&](const GridPos& support)
    {
        if (isWalkSupport(support))
        {
            float edgePenalty = 0.0f;
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
                    edgePenalty += 0.28f;
                }
            }
            return 1.0f + edgePenalty;
        }
        return std::numeric_limits<float>::infinity();
    };
    const auto heuristic = [&](const GridPos& support)
    {
        return static_cast<float>(std::abs(support.x - rawGoal.x) + std::abs(support.z - rawGoal.z)) * 1.05f
            + static_cast<float>(std::abs(support.y - rawGoal.y)) * 1.8f;
    };
    const auto goalScore = [&](const GridPos& support)
    {
        return std::abs(support.x - rawGoal.x) + std::abs(support.z - rawGoal.z) + std::abs(support.y - rawGoal.y) * 2;
    };

    std::vector<BotPathNode> open;
    std::unordered_map<long long, float> bestCost;
    std::unordered_map<long long, GridPos> parent;
    open.push_back(BotPathNode { start, 0.0f, heuristic(start) });
    bestCost[PathKey(start.x, start.y, start.z)] = 0.0f;

    std::optional<GridPos> bestGoal;
    int bestGoalScore = goalScore(start);
    int expansions = 0;
    while (!open.empty() && expansions++ < kMaxExpansions)
    {
        const auto currentIt = std::min_element(
            open.begin(),
            open.end(),
            [](const BotPathNode& a, const BotPathNode& b)
            {
                return a.f < b.f;
            });
        const BotPathNode current = *currentIt;
        open.erase(currentIt);

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
                if (!inBounds(next) || !isWalkSupport(next))
                {
                    continue;
                }

                const float cost = nodeCost(next);
                if (!std::isfinite(cost))
                {
                    continue;
                }
                const float verticalCost = dy > 0 ? 0.90f : (dy < 0 ? 0.38f : 0.0f);
                const float nextG = current.g + cost + verticalCost;
                const long long key = PathKey(next.x, next.y, next.z);
                const auto found = bestCost.find(key);
                if (found != bestCost.end() && found->second <= nextG)
                {
                    continue;
                }

                bestCost[key] = nextG;
                parent[key] = current.pos;
                open.push_back(BotPathNode { next, nextG, nextG + heuristic(next) });
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
    while (step != start)
    {
        const auto found = parent.find(PathKey(step.x, step.y, step.z));
        if (found == parent.end())
        {
            return finalTarget;
        }
        step = found->second;
        path.push_back(step);
    }
    std::reverse(path.begin(), path.end());

    if (path.size() < 2)
    {
        return finalTarget;
    }

    const std::size_t lookAhead = std::min<std::size_t>(path.size() - 1, path.size() > 5 ? 3 : 2);
    const GridPos previous = path[lookAhead];
    const Vector3 waypoint {
        static_cast<float>(previous.x),
        static_cast<float>(previous.y) + 1.08f,
        static_cast<float>(previous.z)
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
    const GridPos sideStep {
        std::fabs(direction.x) > std::fabs(direction.z) ? 0 : memory.strafeSign,
        0,
        std::fabs(direction.x) > std::fabs(direction.z) ? memory.strafeSign : 0
    };
    const GridPos candidates[] {
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
    if (DistanceSquared(bot.GetPosition(), targetPos) > 7.0f)
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

    if (world_.BreakBlock(target, bot.GetTeamId()))
    {
        AddWorldEffect(targetPos, Color { 255, 224, 122, 255 }, 0.28f, 0.25f);
        AddFloatingText("breach", targetPos, Color { 255, 224, 122, 255 });
        AddEventMessage(bot.GetName() + " breached Core defense", Color { 255, 224, 122, 255 }, 1.6f);
        audio_.PlayBreakBlock();
        bot.GetInventory().DamageTool(1);
    }
    memory.hasBreakTarget = false;
    memory.breakProgress = 0.0f;
    return true;
}

std::optional<GridPos> Game::FindBotBlockingBlock(const Player& bot, Vector3 wish) const
{
    const Vector3 direction = Normalize2D(wish);
    if (Length2D(direction) <= 0.0001f)
    {
        return std::nullopt;
    }

    const Vector3 botPos = bot.GetPosition();
    const float distances[] { 0.72f, 1.05f, 1.34f };
    const float heights[] { -0.42f, 0.12f, 0.62f };
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

bool Game::TryBotBreakBlockingBlock(Player& bot, Vector3 wish, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
    const std::optional<GridPos> blockingBlock = FindBotBlockingBlock(bot, wish);
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

    bot.SetYaw(YawFromDirection(Normalize2D(wish)));
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
    const BotRole role = RoleForBotId(player.GetId());
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
        bot.Move(wish, false, dt, world_, false, false, TerrainSpeedMultiplier(bot), true);
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
        bot.Move(wish, false, dt, world_, false, false, TerrainSpeedMultiplier(bot), true);
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
    const BotRole role = RoleForBotId(bot.GetId());

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

        if (score < bestScore)
        {
            bestScore = score;
            best = &pickup;
        }
    }

    return best;
}


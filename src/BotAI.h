#pragma once

#include "Block.h"
#include "raylib.h"

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

struct BotMemory
{
    int playerId = -1;
    BotState state = BotState::Collect;
    BotRole role = BotRole::Rusher;
    BotIntent intent = BotIntent::SecureResources;
    float stateTimer = 0.0f;
    float intentTimer = 0.0f;
    float intentLockTimer = 0.0f;
    float intentScore = 0.0f;
    std::string intentReason;
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
    Vector3 lastPosition {};
};

const char* ToString(BotState state);
const char* ToString(BotRole role);
const char* ToString(BotIntent intent);

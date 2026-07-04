#pragma once

#include "Block.h"
#include "Inventory.h"
#include "RangedCombat.h"
#include "raylib.h"

#include <string>

struct PlacementPreview
{
    GridPos position {};
    GridPos targetBlock {};
    GridPos faceNormal {};
    BlockType selectedType = BlockType::WoolBlock;
    bool visible = false;
    bool valid = false;
    bool hasTarget = false;
    std::string reason;
};

struct BreakProgress
{
    GridPos target {};
    BlockType targetType = BlockType::Air;
    bool visible = false;
    bool isCore = false;
    float fraction = 0.0f;
    std::string label;
};

struct CombatPreview
{
    bool visible = false;
    bool targetInRange = false;
    bool ready = false;
    bool predictedSprintReset = false;
    bool predictedCombo = false;
    bool blockedByTerrain = false;
    float cooldownFraction = 1.0f;
    float range = 0.0f;
    float targetDistance = 0.0f;
    float terrainBlockDistance = 0.0f;
    int damage = 0;
    int targetHealth = 0;
    int targetMaxHealth = 0;
    Vector3 predictedKnockback {};
    std::string targetName;
    std::string hitZoneName;
    std::string label;
};

struct OrbitaTeleportPreview
{
    bool visible = false;
    bool valid = false;
    Vector3 start {};
    Vector3 destination {};
    Vector3 direction { 0.0f, 0.0f, 1.0f };
    float travelDistance = 0.0f;
    int healthCost = 0;
    std::string reason;
};

enum class HeroDeviceVisualKind
{
    BromVacuumBot,
    BromTurretDrone,
    KonvoyTrap,
    KonvoyTether,
    KonvoyDome,
    SvidetelEcho
};

struct HeroDeviceVisual
{
    HeroDeviceVisualKind kind = HeroDeviceVisualKind::BromVacuumBot;
    Vector3 position {};
    Vector3 target {};
    Vector3 direction { 0.0f, 0.0f, 1.0f };
    int teamId = -1;
    int cargoUnits = 0;
    int cargoCapacity = 0;
    float radius = 0.0f;
    bool temporary = false;
    bool returning = false;
    bool active = false;
    float lifetimeFraction = 0.0f;
};

enum class WorldEffectKind
{
    Burst,
    Ring,
    Cone,
    Pull,
    Trail,
    FireZone,
    CorePulse,
    Sacrifice
};

struct WorldEffect
{
    Vector3 position {};
    Vector3 direction { 0.0f, 0.0f, 1.0f };
    Color color = WHITE;
    float radius = 0.25f;
    float lifetime = 0.35f;
    float age = 0.0f;
    WorldEffectKind kind = WorldEffectKind::Burst;
};

struct TimedExplosion
{
    GridPos block {};
    int ownerTeamId = -1;
    int ownerPlayerId = -1;
    float timer = 2.6f;
    float radius = 2.6f;
    // Stable per-spawn id (Game::NextExplosiveId()), NOT a vector index — see
    // EnergyProjectile::id above for why. Trailing field so existing positional
    // aggregate-init call sites that don't mention it keep defaulting to -1.
    int id = -1;
};

struct EnergyProjectile
{
    // Stable per-spawn id (assigned once by Game::NextProjectileId() when the
    // projectile is created), NOT a vector index — an earlier projectile
    // expiring must not shift a still-flying projectile's identity. See
    // docs/MULTIPLAYER_QUALITY_TARGET.md "Что считается провалом": dynamic
    // entity id == vector index used for interpolation is an explicit failure.
    int id = -1;
    Vector3 position {};
    Vector3 previousPosition {};
    Vector3 startPosition {};
    Vector3 velocity {};
    int ownerId = -1;
    int ownerTeamId = -1;
    int damage = 20;
    float baseDamage = 0.0f;
    float radius = 0.18f;
    float explosionRadius = 0.0f;
    float lifetime = 2.8f;
    float gravity = 2.35f;
    float airDragPerTick = 1.0f;
    float maxRange = 0.0f;
    float distanceTraveled = 0.0f;
    int punchLevel = 0;
    ProjectileKind kind = ProjectileKind::Arrow;
    bool critical = false;
    bool speedBasedDamage = false;
    bool affectedByDrag = false;
    bool fireZone = false;
    bool blueFire = false;
};

struct HazardZone
{
    Vector3 position {};
    int ownerTeamId = -1;
    int ownerPlayerId = -1;
    float radius = 2.4f;
    float lifetime = 5.0f;
    float tickTimer = 0.0f;
    int damagePerTick = 8;
    bool blueFire = false;
    // Stable per-spawn id (Game::NextHazardZoneId()), NOT a vector index — see
    // EnergyProjectile::id above for why. Trailing field so existing positional
    // aggregate-init call sites that don't mention it keep defaulting to -1.
    int id = -1;
};

struct AlarmTrap
{
    Vector3 position {};
    int ownerTeamId = -1;
    float radius = 4.4f;
    bool triggered = false;
};

struct FloatingText
{
    std::string text;
    Vector3 position {};
    Color color = WHITE;
    float lifetime = 0.75f;
    float age = 0.0f;
};

struct EventMessage
{
    std::string text;
    Color color = WHITE;
    float lifetime = 2.4f;
    float age = 0.0f;
};

struct KillFeedEntry
{
    std::string text;
    Color color = WHITE;
    float lifetime = 5.0f;
    float age = 0.0f;
};

// DroppedItem moved to Inventory.h (raylib-free, owned by MatchSimulation).

struct MatchStats
{
    int hitsDealt = 0;
    int damageDealt = 0;
    int coreDamageDealt = 0;
    int kills = 0;
    int deaths = 0;
    int coresDestroyed = 0;
    int blocksPlaced = 0;
    int blocksBroken = 0;
    int resourcesPicked = 0;
};

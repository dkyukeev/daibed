#pragma once

#include "Block.h"
#include "Inventory.h"
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

struct WorldEffect
{
    Vector3 position {};
    Color color = WHITE;
    float radius = 0.25f;
    float lifetime = 0.35f;
    float age = 0.0f;
};

struct TimedExplosion
{
    GridPos block {};
    int ownerTeamId = -1;
    int ownerPlayerId = -1;
    float timer = 2.6f;
    float radius = 2.6f;
};

struct EnergyProjectile
{
    Vector3 position {};
    Vector3 velocity {};
    int ownerId = -1;
    int ownerTeamId = -1;
    int damage = 20;
    float radius = 0.18f;
    float explosionRadius = 0.0f;
    float lifetime = 2.8f;
    bool fireZone = false;
};

struct HazardZone
{
    Vector3 position {};
    int ownerTeamId = -1;
    int ownerPlayerId = -1;
    float radius = 2.4f;
    float lifetime = 5.0f;
    float tickTimer = 0.0f;
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

struct DroppedItem
{
    ItemStack stack;
    Vector3 position {};
    Vector3 velocity {};
    int ownerPlayerId = -1;
    float ownerPickupDelay = 0.0f;
    float lifetime = 45.0f;
    float age = 0.0f;
    bool collected = false;
};

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

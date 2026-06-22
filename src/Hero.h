#pragma once

#include "raylib.h"

#include <string>

enum class HeroId
{
    Radon,
    Orbita,
    Brom,
    Konvoy,
    Likho,
    Svidetel
};

enum class HeroAbilitySlot
{
    Active1,
    Active2,
    Ultimate
};

enum class HeroAnimationState
{
    Idle,
    Walk,
    Run,
    Jump,
    Fall,
    Attack,
    Hurt,
    Death,
    Ability1,
    Ability2,
    Ultimate,
    WindUp,
    Cast,
    Recovery,
    Overloaded,
    UltPrimed,
    DeathSacrifice
};

struct HeroHitboxProfile
{
    float bodyRadius = 0.36f;
    float bodyHeight = 2.26f;
    float headStart = 1.03f;
    float headEnd = 1.42f;
    Vector3 visualOffset { 0.0f, -0.86f, 0.0f };
    float visualScale = 1.0f;
};

struct HeroAbilityDefinition
{
    std::string name;
    std::string description;
    float cooldownSeconds = 0.0f;
    float durationSeconds = 0.0f;
};

struct HeroDefinition
{
    HeroId id = HeroId::Radon;
    std::string name;
    std::string role;
    std::string passiveName;
    std::string passiveDescription;
    HeroAbilityDefinition active1;
    HeroAbilityDefinition active2;
    HeroAbilityDefinition ultimate;
    std::string ultimateChargeDescription;
};

struct HeroAbilityState
{
    float cooldownRemaining = 0.0f;
    float activeTimer = 0.0f;
    bool active = false;
};

struct HeroRuntimeState
{
    HeroAbilityState active1;
    HeroAbilityState active2;
    HeroAbilityState ultimate;
    float ultimateCharge = 0.0f;
    float animationTimer = 0.0f;
    float animationDuration = 0.0f;
    HeroAnimationState animationState = HeroAnimationState::Idle;
    bool ultimateReady = false;
    bool ultimatePrimed = false;
    bool radonProtected = false;
    bool radonOverloaded = false;
    bool orbitaMomentumStrike = false;
    bool orbitaAirDashLocked = false;
    float orbitaPulseTimer = 0.0f;
    float orbitaDashRemaining = 0.0f;
    Vector3 orbitaDashDirection { 0.0f, 0.0f, 1.0f };
    float orbitaDashLiftRemaining = 0.0f;
    bool orbitaTeleportPrimed = false;
    float orbitaTeleportPreviewTimer = 0.0f;
    Vector3 orbitaTeleportDestination {};
    Vector3 orbitaTeleportDirection { 0.0f, 0.0f, 1.0f };
    float orbitaTeleportDistance = 0.0f;
    int orbitaTeleportHealthCost = 0;
    int likhoDisguiseTeamId = -1;
    int likhoDisguisePlayerId = -1;
    HeroId likhoDisguiseHeroId = HeroId::Likho;
    bool likhoInsideEnemyBase = false;
};

const char* HeroIdKey(HeroId id);
const char* HeroAbilitySlotName(HeroAbilitySlot slot);
const char* HeroAnimationStateName(HeroAnimationState state);

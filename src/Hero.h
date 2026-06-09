#pragma once

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
    WindUp,
    Cast,
    Recovery,
    Overloaded,
    UltPrimed,
    DeathSacrifice
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
};

const char* HeroIdKey(HeroId id);
const char* HeroAbilitySlotName(HeroAbilitySlot slot);

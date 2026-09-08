#pragma once

#include <algorithm>
#include <cmath>

inline constexpr float kTicksPerSecond = 20.0f;
inline constexpr float kBlocksPerUnit = 1.0f;

enum class CrossbowState
{
    Unloaded,
    Loading,
    Loaded
};

enum class ProjectileKind
{
    Arrow,
    Fireball,
    Molotov,
    Blaster
};

enum class ArrowVariant
{
    Standard = 0,
    Breacher = 1,
    Impulse = 2,
    Count = 3
};

inline constexpr int kArrowVariantCount = static_cast<int>(ArrowVariant::Count);
inline constexpr float kQuiverReloadSeconds = 3.4f;

inline constexpr int ArrowQuiverCapacity(ArrowVariant variant)
{
    switch (variant)
    {
    case ArrowVariant::Standard: return 16;
    case ArrowVariant::Breacher: return 12;
    case ArrowVariant::Impulse: return 10;
    case ArrowVariant::Count: break;
    }
    return 0;
}

inline constexpr const char* ArrowVariantName(ArrowVariant variant)
{
    switch (variant)
    {
    case ArrowVariant::Standard: return "Обычные стрелы";
    case ArrowVariant::Breacher: return "Осадные стрелы";
    case ArrowVariant::Impulse: return "Импульсные стрелы";
    case ArrowVariant::Count: break;
    }
    return "Стрелы";
}

struct ProjectileTuning
{
    float speed;
    float gravity;
    float lifetime;
    float radius;
    int damage;
    float explosionRadius;
    float cooldown;
};

struct BlasterTuning
{
    float loadTime = 1.25f;
    float minimumLoadTime = 0.50f;
    float quickChargeReduction = 0.25f;
    float aimedSpread = 0.006f;
    float hipSpread = 0.025f;
    int baseDamage = 40;
    float baseSpeed = 63.0f;
    float gravity = 1.0f;
    float cooldown = 0.48f;
};

struct BowTuning
{
    float fullDrawTime = 1.0f;
    float minimumDrawPower = 0.10f;
    float maximumArrowSpeed = 60.0f;
    float baseArrowDamage = 10.0f;
    float powerDamageBonusPerLevel = 0.25f;
    float criticalMultiplier = 1.5f;
    float baseKnockback = 0.85f;
    float punchKnockbackPerLevel = 1.15f;
    float verticalKnockback = 0.35f;
};

struct ProjectilePhysicsTuning
{
    float arrowAirDragPerTick = 0.99f;
    float arrowWaterDragPerTick = 0.60f;
    float arrowGravityPerSecond = 1.0f;
    float maxLifetime = 6.0f;
    float maximumRange = 80.0f;
};

inline constexpr ProjectileTuning kArrowTuning { 60.0f, 1.0f, 6.0f, 0.12f, 6, 0.0f, 0.20f };
inline constexpr ProjectileTuning kFireballTuning { 17.0f, 1.65f, 3.0f, 0.31f, 28, 2.55f, 1.25f };
inline constexpr ProjectileTuning kMolotovTuning { 14.0f, 3.8f, 2.8f, 0.27f, 10, 1.6f, 1.15f };
inline constexpr BlasterTuning kBlasterTuning {};
inline constexpr BowTuning kBowTuning {};
inline constexpr ProjectilePhysicsTuning kProjectilePhysicsTuning {};

inline constexpr float BlasterChargeSeconds(int rapidFireLevel)
{
    return std::max(
        kBlasterTuning.minimumLoadTime,
        kBlasterTuning.loadTime - kBlasterTuning.quickChargeReduction * std::clamp(rapidFireLevel, 0, 3));
}

inline constexpr float BowDrawPower(float holdSeconds)
{
    const float rawPower = std::max(0.0f, holdSeconds) / kBowTuning.fullDrawTime;
    return std::clamp((rawPower * rawPower + rawPower * 2.0f) / 3.0f, 0.0f, 1.0f);
}

inline constexpr int BowPowerLevelForUpgrade(int bowUpgradeLevel)
{
    return bowUpgradeLevel >= 3 ? 2 : (bowUpgradeLevel >= 1 ? 1 : 0);
}

inline constexpr int BowPunchLevelForUpgrade(int bowUpgradeLevel)
{
    return bowUpgradeLevel >= 3 ? 2 : (bowUpgradeLevel >= 2 ? 1 : 0);
}

static_assert(BowDrawPower(0.0f) == 0.0f, "An untouched bow must not fire");
static_assert(BowDrawPower(1.0f) == 1.0f, "A one-second draw must be full power");
static_assert(BlasterChargeSeconds(0) == 1.25f && BlasterChargeSeconds(3) == 0.50f,
    "Blaster quick-charge progression changed unexpectedly");
static_assert(BowPowerLevelForUpgrade(1) == 1 && BowPunchLevelForUpgrade(1) == 0,
    "Bow level I must be Power I");
static_assert(BowPowerLevelForUpgrade(2) == 1 && BowPunchLevelForUpgrade(2) == 1,
    "Bow level II must be Power I / Punch I");
static_assert(BowPowerLevelForUpgrade(3) == 2 && BowPunchLevelForUpgrade(3) == 2,
    "Bow level III must be Power II / Punch II");

inline float BlasterDamageMultiplier(int damageLevel)
{
    static constexpr float multipliers[] { 1.0f, 1.16f, 1.34f, 1.58f };
    return multipliers[std::clamp(damageLevel, 0, 3)];
}

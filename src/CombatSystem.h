#pragma once

#include "Core.h"
#include "Player.h"

#include <optional>
#include <string>
#include <vector>

enum class WeaponType
{
    Sword,
    Axe,
    Spear
};

enum class HitZone
{
    Body,
    Head
};

struct CombatTargetInfo
{
    int targetId = -1;
    int targetTeamId = -1;
    std::string targetName;
    Vector3 position {};
    float distance = 0.0f;
    int damage = 0;
    int targetHealth = 0;
    int targetMaxHealth = 0;
    bool shielded = false;
    bool lethal = false;
    HitZone hitZone = HitZone::Body;
    float chargeMultiplier = 1.0f;
    Vector3 predictedKnockback {};
    bool predictedSprintReset = false;
    bool predictedCombo = false;
};

struct AttackOptions
{
    bool sprinting = false;
    bool sprintReset = false;
    bool attackerAirborne = false;
    Vector3 attackerVelocity {};
    float aimAssist = 0.0f;
};

struct CombatEvent
{
    int attackerId = -1;
    int targetId = -1;
    int targetTeamId = -1;
    Vector3 position {};
    int damage = 0;
    bool killed = false;
    bool coreHit = false;
    bool coreDestroyed = false;
    HitZone hitZone = HitZone::Body;
    WeaponType weapon = WeaponType::Sword;
    bool charged = false;
    bool combo = false;
    bool sprintReset = false;
    bool airborneTarget = false;
    bool voidHit = false;
    Vector3 knockback {};
};

class CombatSystem
{
public:
    static const char* WeaponName(WeaponType weapon);
    static const char* HitZoneName(HitZone hitZone);
    static float AttackRange(WeaponType weapon, int swordLevel);
    static float AttackCooldown(WeaponType weapon, int swordLevel);
    static float AttackConeDot(WeaponType weapon);
    static int BaseDamage(WeaponType weapon, int swordLevel);
    static int DamageBeforeShield(const Player& attacker, const Player& target, WeaponType weapon, HitZone hitZone, float chargeMultiplier);
    static int FinalDamageAgainstTarget(const Player& attacker, const Player& target, WeaponType weapon, HitZone hitZone, float chargeMultiplier);
    static float AttackRangeForSword(int swordLevel);
    static float AttackCooldownForSword(int swordLevel);
    static float AttackConeDot();
    static int BaseDamageForSword(int swordLevel);
    static int DamageBeforeShield(const Player& attacker, const Player& target);
    static int FinalDamageAgainstTarget(const Player& attacker, const Player& target);

    std::optional<CombatTargetInfo> FindMeleeTarget(const Player& attacker, const std::vector<Player>& players, Vector3 aimDirection, WeaponType weapon = WeaponType::Sword, float chargeMultiplier = 1.0f, float maxRayDistance = 0.0f) const;
    bool Attack(Player& attacker, std::vector<Player>& players, Vector3 aimDirection, std::string& message, CombatEvent* event = nullptr, WeaponType weapon = WeaponType::Sword, float chargeMultiplier = 1.0f, const AttackOptions* options = nullptr, float maxRayDistance = 0.0f) const;
    bool DamageCore(Player& attacker, EnergyCore& core, std::string& message, CombatEvent* event = nullptr, int toolLevelOverride = -1) const;
};

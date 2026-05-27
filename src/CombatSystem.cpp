#include "CombatSystem.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace
{
constexpr float kAttackConeDot = 0.38f;
constexpr float kMaxAttackHeightDelta = 1.9f;
constexpr float kHitInvulnerabilitySeconds = 0.18f;
constexpr float kEyeHeight = 0.78f;
constexpr float kTargetCapsuleBottom = -0.84f;
constexpr float kTargetCapsuleTop = 1.42f;
constexpr float kTargetCapsuleRadius = 0.36f;
constexpr float kMeleeRayRadius = 0.16f;

struct MeleeRayHit
{
    float rayDistance = 0.0f;
    Vector3 point {};
    HitZone hitZone = HitZone::Body;
};

struct KnockbackResult
{
    Vector3 impulse {};
    bool sprintReset = false;
    bool combo = false;
    bool targetAirborne = false;
};

float KnockbackForSword(int swordLevel)
{
    return 5.15f + std::min(4, swordLevel) * 0.34f;
}

float KnockbackForWeapon(WeaponType weapon, int swordLevel)
{
    const float base = KnockbackForSword(swordLevel);
    switch (weapon)
    {
    case WeaponType::Axe:
        return base * 1.02f;
    case WeaponType::Spear:
        return base * 0.72f;
    case WeaponType::Sword:
        break;
    }
    return base;
}

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float Dot(Vector3 a, Vector3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Dot2D(Vector3 a, Vector3 b)
{
    return a.x * b.x + a.z * b.z;
}

float Length(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vector3 NormalizeOrFallback(Vector3 value, Vector3 fallback)
{
    const float length = Length(value);
    if (length <= 0.0001f)
    {
        return fallback;
    }

    return Vector3 { value.x / length, value.y / length, value.z / length };
}

Vector3 Direction(Vector3 from, Vector3 to)
{
    const Vector3 delta { to.x - from.x, 0.0f, to.z - from.z };
    const float length = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, -1.0f };
    }

    return Vector3 { delta.x / length, 0.0f, delta.z / length };
}

Vector3 FlattenAim(Vector3 aimDirection, Vector3 fallback)
{
    const Vector3 flat { aimDirection.x, 0.0f, aimDirection.z };
    const float length = std::sqrt(flat.x * flat.x + flat.z * flat.z);
    if (length <= 0.0001f)
    {
        return fallback;
    }

    return Vector3 { flat.x / length, 0.0f, flat.z / length };
}

bool ConsiderRayDistance(float candidate, float maxDistance, float& bestDistance)
{
    if (candidate < 0.0f || candidate > maxDistance || candidate >= bestDistance)
    {
        return false;
    }

    bestDistance = candidate;
    return true;
}

void ConsiderRaySphere(Vector3 origin, Vector3 direction, Vector3 center, float radius, float maxDistance, float& bestDistance)
{
    const Vector3 offset { origin.x - center.x, origin.y - center.y, origin.z - center.z };
    const float b = Dot(offset, direction);
    const float c = Dot(offset, offset) - radius * radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f)
    {
        return;
    }

    const float root = std::sqrt(discriminant);
    ConsiderRayDistance(-b - root, maxDistance, bestDistance);
    ConsiderRayDistance(-b + root, maxDistance, bestDistance);
}

std::optional<MeleeRayHit> IntersectMeleeRayWithTarget(const Player& attacker, const Player& target, Vector3 aimDirection, WeaponType weapon, float maxRayDistance)
{
    const float weaponRange = CombatSystem::AttackRange(weapon, attacker.GetInventory().GetSwordLevel());
    const float maxDistance = maxRayDistance > 0.0f
        ? std::min(weaponRange, maxRayDistance)
        : weaponRange;
    if (maxDistance <= 0.05f)
    {
        return std::nullopt;
    }

    const Vector3 origin {
        attacker.GetPosition().x,
        attacker.GetPosition().y + kEyeHeight,
        attacker.GetPosition().z
    };
    const Vector3 direction = NormalizeOrFallback(aimDirection, attacker.Forward());
    const Vector3 targetPosition = target.GetPosition();
    const float bottomY = targetPosition.y + kTargetCapsuleBottom;
    const float topY = targetPosition.y + kTargetCapsuleTop;
    const float radius = kTargetCapsuleRadius + kMeleeRayRadius;
    float bestDistance = maxDistance + 0.001f;

    const float dx = origin.x - targetPosition.x;
    const float dz = origin.z - targetPosition.z;
    const float a = direction.x * direction.x + direction.z * direction.z;
    const float b = 2.0f * (dx * direction.x + dz * direction.z);
    const float c = dx * dx + dz * dz - radius * radius;
    if (a > 0.0001f)
    {
        const float discriminant = b * b - 4.0f * a * c;
        if (discriminant >= 0.0f)
        {
            const float root = std::sqrt(discriminant);
            const float inv = 1.0f / (2.0f * a);
            const float candidates[] {
                (-b - root) * inv,
                (-b + root) * inv
            };
            for (float candidate : candidates)
            {
                const float y = origin.y + direction.y * candidate;
                if (y >= bottomY && y <= topY)
                {
                    ConsiderRayDistance(candidate, maxDistance, bestDistance);
                }
            }
        }
    }
    else if (c <= 0.0f)
    {
        const float candidate = std::clamp(targetPosition.y + kEyeHeight, bottomY, topY) - origin.y;
        if (std::fabs(direction.y) > 0.0001f)
        {
            ConsiderRayDistance(candidate / direction.y, maxDistance, bestDistance);
        }
    }

    ConsiderRaySphere(origin, direction, Vector3 { targetPosition.x, bottomY, targetPosition.z }, radius, maxDistance, bestDistance);
    ConsiderRaySphere(origin, direction, Vector3 { targetPosition.x, topY, targetPosition.z }, radius, maxDistance, bestDistance);
    if (bestDistance > maxDistance)
    {
        return std::nullopt;
    }

    const Vector3 point {
        origin.x + direction.x * bestDistance,
        origin.y + direction.y * bestDistance,
        origin.z + direction.z * bestDistance
    };
    return MeleeRayHit {
        bestDistance,
        point,
        point.y >= targetPosition.y + 1.03f ? HitZone::Head : HitZone::Body
    };
}

bool IsValidMeleeTarget(const Player& attacker, const Player& target)
{
    return target.GetId() != attacker.GetId()
        && target.GetTeamId() != attacker.GetTeamId()
        && target.IsAlive()
        && !target.IsEliminated()
        && !target.IsInvulnerable()
        && std::fabs(attacker.GetPosition().y - target.GetPosition().y) <= kMaxAttackHeightDelta;
}

KnockbackResult CalculateKnockback(const Player& attacker, const Player& target, Vector3 aimDirection, WeaponType weapon, float chargeMultiplier, const AttackOptions* options, bool sprintReset)
{
    AttackOptions context {};
    if (options != nullptr)
    {
        context = *options;
    }
    else
    {
        context.sprinting = attacker.IsSprinting();
        context.attackerAirborne = !attacker.IsOnGround();
        context.attackerVelocity = attacker.GetVelocity();
    }

    const bool targetAirborne = !target.IsOnGround();
    const bool combo = targetAirborne || target.GetVelocity().y > 0.12f;
    const Vector3 hitDirection = FlattenAim(aimDirection, Direction(attacker.GetPosition(), target.GetPosition()));
    const float attackerAlong = Dot2D(context.attackerVelocity, hitDirection);
    const float targetAlong = Dot2D(target.GetVelocity(), hitDirection);

    const float baseHorizontal = KnockbackForWeapon(weapon, attacker.GetInventory().GetSwordLevel());
    const float sprintBonus = sprintReset ? 1.28f : (context.sprinting ? 0.58f : 0.0f);
    const float chargeBonus = (std::clamp(chargeMultiplier, 1.0f, 1.65f) - 1.0f) * 1.65f;
    const float comboBonus = combo ? 0.38f : 0.0f;
    const float movementBonus = std::clamp(attackerAlong * 0.075f, -0.18f, 0.58f);
    const float targetResistance = std::clamp(
        1.0f
            - target.GetInventory().GetArmorLevel() * 0.035f
            - (target.HasShield() ? 0.12f : 0.0f)
            - (targetAlong > 1.2f ? 0.08f : 0.0f)
            + (targetAlong < -0.8f ? 0.06f : 0.0f),
        0.72f,
        1.08f);
    const float horizontalKnockback = std::max(0.0f, baseHorizontal + sprintBonus + chargeBonus + comboBonus + movementBonus) * targetResistance;
    const float weaponVertical = weapon == WeaponType::Axe ? 0.10f : (weapon == WeaponType::Spear ? -0.08f : 0.0f);
    const float verticalKnockback = std::clamp(
        (targetAirborne ? 0.58f : 0.78f)
            + (sprintReset ? 0.13f : 0.0f)
            + (chargeMultiplier > 1.05f ? 0.09f : 0.0f)
            + weaponVertical
            + (context.attackerAirborne ? -0.09f : 0.0f),
        0.42f,
        1.16f);

    return KnockbackResult {
        Vector3 {
            hitDirection.x * horizontalKnockback,
            verticalKnockback,
            hitDirection.z * horizontalKnockback
        },
        sprintReset,
        combo,
        targetAirborne
    };
}
}

const char* CombatSystem::WeaponName(WeaponType weapon)
{
    switch (weapon)
    {
    case WeaponType::Sword:
        return "Sword";
    case WeaponType::Axe:
        return "Axe";
    case WeaponType::Spear:
        return "Spear";
    }
    return "Weapon";
}

const char* CombatSystem::HitZoneName(HitZone hitZone)
{
    return hitZone == HitZone::Head ? "head" : "body";
}

float CombatSystem::AttackRange(WeaponType weapon, int swordLevel)
{
    const float base = 2.45f + std::min(3, swordLevel) * 0.10f;
    switch (weapon)
    {
    case WeaponType::Axe:
        return base - 0.18f;
    case WeaponType::Spear:
        return base + 0.48f;
    case WeaponType::Sword:
        break;
    }
    return base;
}

float CombatSystem::AttackCooldown(WeaponType weapon, int swordLevel)
{
    const float sword = std::max(0.12f, 0.18f - std::min(4, swordLevel) * 0.008f);
    switch (weapon)
    {
    case WeaponType::Axe:
        return sword + 0.08f;
    case WeaponType::Spear:
        return sword + 0.03f;
    case WeaponType::Sword:
        break;
    }
    return sword;
}

float CombatSystem::AttackConeDot(WeaponType weapon)
{
    switch (weapon)
    {
    case WeaponType::Axe:
        return 0.24f;
    case WeaponType::Spear:
        return 0.55f;
    case WeaponType::Sword:
        break;
    }
    return kAttackConeDot;
}

int CombatSystem::BaseDamage(WeaponType weapon, int swordLevel)
{
    const int sword = 18 + swordLevel * 7;
    switch (weapon)
    {
    case WeaponType::Axe:
        return sword + 11;
    case WeaponType::Spear:
        return std::max(8, sword - 4);
    case WeaponType::Sword:
        break;
    }
    return sword;
}

int CombatSystem::DamageBeforeShield(const Player& attacker, const Player& target, WeaponType weapon, HitZone hitZone, float chargeMultiplier)
{
    const int armorReduction = target.GetInventory().GetArmorLevel() * (weapon == WeaponType::Axe ? 3 : 5);
    const float zoneMultiplier = hitZone == HitZone::Head ? 1.35f : 1.0f;
    const int rawDamage = static_cast<int>(static_cast<float>(BaseDamage(weapon, attacker.GetInventory().GetSwordLevel())) * zoneMultiplier * chargeMultiplier + 0.5f);
    return std::max(6, rawDamage - armorReduction);
}

int CombatSystem::FinalDamageAgainstTarget(const Player& attacker, const Player& target, WeaponType weapon, HitZone hitZone, float chargeMultiplier)
{
    const int damageBeforeShield = DamageBeforeShield(attacker, target, weapon, hitZone, chargeMultiplier);
    return target.HasShield() ? std::max(1, damageBeforeShield / 2) : damageBeforeShield;
}

float CombatSystem::AttackRangeForSword(int swordLevel)
{
    return AttackRange(WeaponType::Sword, swordLevel);
}

float CombatSystem::AttackCooldownForSword(int swordLevel)
{
    return AttackCooldown(WeaponType::Sword, swordLevel);
}

float CombatSystem::AttackConeDot()
{
    return AttackConeDot(WeaponType::Sword);
}

int CombatSystem::BaseDamageForSword(int swordLevel)
{
    return BaseDamage(WeaponType::Sword, swordLevel);
}

int CombatSystem::DamageBeforeShield(const Player& attacker, const Player& target)
{
    const int armorReduction = target.GetInventory().GetArmorLevel() * 5;
    return std::max(6, BaseDamageForSword(attacker.GetInventory().GetSwordLevel()) - armorReduction);
}

int CombatSystem::FinalDamageAgainstTarget(const Player& attacker, const Player& target)
{
    const int damageBeforeShield = DamageBeforeShield(attacker, target);
    return target.HasShield() ? std::max(1, damageBeforeShield / 2) : damageBeforeShield;
}

std::optional<CombatTargetInfo> CombatSystem::FindMeleeTarget(const Player& attacker, const std::vector<Player>& players, Vector3 aimDirection, WeaponType weapon, float chargeMultiplier, float maxRayDistance) const
{
    const float attackRange = AttackRange(weapon, attacker.GetInventory().GetSwordLevel());
    float bestDistance = (maxRayDistance > 0.0f ? std::min(attackRange, maxRayDistance) : attackRange) + 0.001f;
    std::optional<CombatTargetInfo> bestTarget;

    for (const Player& target : players)
    {
        if (!IsValidMeleeTarget(attacker, target))
        {
            continue;
        }

        const std::optional<MeleeRayHit> hit = IntersectMeleeRayWithTarget(attacker, target, aimDirection, weapon, maxRayDistance);
        if (!hit.has_value() || hit->rayDistance >= bestDistance)
        {
            continue;
        }

        const int damage = FinalDamageAgainstTarget(attacker, target, weapon, hit->hitZone, chargeMultiplier);
        const bool predictedSprintReset = attacker.HasSprintReset();
        const KnockbackResult knockback = CalculateKnockback(attacker, target, aimDirection, weapon, chargeMultiplier, nullptr, predictedSprintReset);
        bestTarget = CombatTargetInfo {
            target.GetId(),
            target.GetTeamId(),
            target.GetName(),
            hit->point,
            hit->rayDistance,
            damage,
            target.GetHealth(),
            target.GetMaxHealth(),
            target.HasShield(),
            target.GetHealth() <= damage,
            hit->hitZone,
            chargeMultiplier,
            knockback.impulse,
            knockback.sprintReset,
            knockback.combo
        };
        bestDistance = hit->rayDistance;
    }

    return bestTarget;
}

bool CombatSystem::Attack(Player& attacker, std::vector<Player>& players, Vector3 aimDirection, std::string& message, CombatEvent* event, WeaponType weapon, float chargeMultiplier, const AttackOptions* options, float maxRayDistance) const
{
    if (!attacker.CanAttack())
    {
        return false;
    }

    const float attackRange = CombatSystem::AttackRange(weapon, attacker.GetInventory().GetSwordLevel());
    float bestDistance = (maxRayDistance > 0.0f ? std::min(attackRange, maxRayDistance) : attackRange) + 0.001f;
    Player* bestTarget = nullptr;
    MeleeRayHit bestHit {};

    for (Player& target : players)
    {
        if (!IsValidMeleeTarget(attacker, target))
        {
            continue;
        }

        const std::optional<MeleeRayHit> hit = IntersectMeleeRayWithTarget(attacker, target, aimDirection, weapon, maxRayDistance);
        if (!hit.has_value() || hit->rayDistance >= bestDistance)
        {
            continue;
        }

        bestTarget = &target;
        bestHit = *hit;
        bestDistance = hit->rayDistance;
    }

    if (bestTarget == nullptr)
    {
        return false;
    }

    const HitZone hitZone = bestHit.hitZone;
    const int damageBeforeShield = CombatSystem::DamageBeforeShield(attacker, *bestTarget, weapon, hitZone, chargeMultiplier);
    const int finalDamage = CombatSystem::FinalDamageAgainstTarget(attacker, *bestTarget, weapon, hitZone, chargeMultiplier);
    AttackOptions context {};
    if (options != nullptr)
    {
        context = *options;
    }
    else
    {
        context.sprinting = attacker.IsSprinting();
        context.attackerAirborne = !attacker.IsOnGround();
        context.attackerVelocity = attacker.GetVelocity();
    }

    const bool sprintReset = context.sprintReset || attacker.ConsumeSprintReset();
    context.sprintReset = sprintReset;
    const KnockbackResult knockback = CalculateKnockback(attacker, *bestTarget, aimDirection, weapon, chargeMultiplier, &context, sprintReset);
    bestTarget->Damage(damageBeforeShield);
    bestTarget->ActivateHitInvulnerability(kHitInvulnerabilitySeconds);
    bestTarget->ApplyKnockback(knockback.impulse);
    attacker.ResetAttackCooldown(CombatSystem::AttackCooldown(weapon, attacker.GetInventory().GetSwordLevel()) + (chargeMultiplier > 1.05f ? 0.14f : 0.0f));

    std::ostringstream stream;
    stream << attacker.GetName() << " " << (chargeMultiplier > 1.05f ? "charged " : "")
           << CombatSystem::WeaponName(weapon) << " hit " << bestTarget->GetName()
           << " (" << CombatSystem::HitZoneName(hitZone) << ") for " << finalDamage << ".";
    if (knockback.combo)
    {
        stream << " Combo.";
    }
    else if (knockback.sprintReset)
    {
        stream << " Sprint reset.";
    }
    message = stream.str();
    if (event != nullptr)
    {
        *event = CombatEvent {
            attacker.GetId(),
            bestTarget->GetId(),
            bestTarget->GetTeamId(),
            bestHit.point,
            finalDamage,
            bestTarget->GetHealth() <= 0,
            false,
            false,
            hitZone,
            weapon,
            chargeMultiplier > 1.05f,
            knockback.combo,
            knockback.sprintReset,
            knockback.targetAirborne,
            false,
            knockback.impulse
        };
    }
    return true;
}

bool CombatSystem::DamageCore(Player& attacker, EnergyCore& core, std::string& message, CombatEvent* event, int toolLevelOverride) const
{
    if (!attacker.CanAttack() || attacker.GetTeamId() == core.GetTeamId() || !core.IsAlive())
    {
        return false;
    }

    const int toolLevel = toolLevelOverride >= 0 ? toolLevelOverride : attacker.GetInventory().GetToolLevel();
    const int damage = 14 + toolLevel * 12;
    const bool destroyed = core.Damage(damage);
    attacker.ResetAttackCooldown(0.45f);

    std::ostringstream stream;
    stream << attacker.GetName() << " damaged EnergyCore for " << damage << ".";
    if (destroyed)
    {
        stream << " Core destroyed!";
    }
    message = stream.str();
    if (event != nullptr)
    {
        *event = CombatEvent {
            attacker.GetId(),
            -1,
            core.GetTeamId(),
            Vector3 {
                static_cast<float>(core.GetBlockPosition().x),
                static_cast<float>(core.GetBlockPosition().y) + 0.95f,
                static_cast<float>(core.GetBlockPosition().z) },
            damage,
            false,
            true,
            destroyed
        };
    }
    return true;
}

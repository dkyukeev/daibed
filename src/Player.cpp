#include "Player.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
constexpr float kMoveSpeed = 5.85f;
constexpr float kGroundAcceleration = 52.0f;
constexpr float kGroundDeceleration = 46.0f;
constexpr float kAirAcceleration = 15.5f;
constexpr float kJumpSpeed = 7.35f;
constexpr float kGravity = 18.0f;
constexpr float kFallGravity = 23.5f;
constexpr float kJumpBufferSeconds = 0.12f;
constexpr float kCoyoteSeconds = 0.09f;
constexpr float kSprintResetSeconds = 0.46f;
constexpr float kKnockbackControlSeconds = 0.18f;
constexpr float kSneakSpeedMultiplier = 0.34f;
constexpr float kStepHeight = 1.02f;
constexpr Vector3 kHalfExtents { 0.32f, 0.9f, 0.32f };
constexpr Vector3 kGroundProbeHalfExtents { 0.28f, 0.9f, 0.28f };

float Length(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vector3 NormalizeOrZero(Vector3 value)
{
    const float length = Length(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, value.y / length, value.z / length };
}

float Approach(float current, float target, float maxDelta)
{
    if (current < target)
    {
        return std::min(current + maxDelta, target);
    }
    if (current > target)
    {
        return std::max(current - maxDelta, target);
    }

    return target;
}
}

Player::Player(int id, std::string name, int teamId, Vector3 spawnPoint, bool local)
    : id_(id),
      name_(std::move(name)),
      teamId_(teamId),
      position_(spawnPoint),
      local_(local)
{
}

int Player::GetId() const
{
    return id_;
}

const std::string& Player::GetName() const
{
    return name_;
}

int Player::GetTeamId() const
{
    return teamId_;
}

Vector3 Player::GetPosition() const
{
    return position_;
}

Vector3 Player::GetVelocity() const
{
    return velocity_;
}

float Player::GetYaw() const
{
    return yaw_;
}

int Player::GetHealth() const
{
    return health_;
}

int Player::GetMaxHealth() const
{
    return maxHealth_;
}

bool Player::IsAlive() const
{
    return alive_;
}

bool Player::IsEliminated() const
{
    return eliminated_;
}

bool Player::IsLocal() const
{
    return local_;
}

bool Player::IsOnGround() const
{
    return onGround_;
}

bool Player::IsSprinting() const
{
    return sprinting_;
}

bool Player::IsSneaking() const
{
    return sneaking_;
}

bool Player::HasSprintReset() const
{
    return sprintResetTimer_ > 0.0f;
}

float Player::GetRespawnTimer() const
{
    return respawnTimer_;
}

float Player::GetAttackCooldownRemaining() const
{
    return attackCooldown_;
}

float Player::GetAttackCooldownDuration() const
{
    return attackCooldownDuration_;
}

float Player::GetSpeedBoostTimer() const
{
    return speedBoostTimer_;
}

float Player::GetJumpBoostTimer() const
{
    return jumpBoostTimer_;
}

float Player::GetShieldTimer() const
{
    return shieldTimer_;
}

float Player::GetInvulnerabilityTimer() const
{
    return invulnerabilityTimer_;
}

bool Player::HasShield() const
{
    return shieldTimer_ > 0.0f;
}

bool Player::IsInvulnerable() const
{
    return invulnerabilityTimer_ > 0.0f;
}

float Player::GetHeroOutgoingDamageMultiplier() const
{
    return heroOutgoingDamageMultiplier_;
}

Inventory& Player::GetInventory()
{
    return inventory_;
}

const Inventory& Player::GetInventory() const
{
    return inventory_;
}

Vector3 Player::Forward() const
{
    return Vector3 { std::sin(yaw_), 0.0f, -std::cos(yaw_) };
}

Vector3 Player::Right() const
{
    return Vector3 { std::cos(yaw_), 0.0f, std::sin(yaw_) };
}

void Player::AddYaw(float delta)
{
    yaw_ += delta;
}

void Player::SetYaw(float yaw)
{
    yaw_ = yaw;
}

void Player::Move(
    Vector3 wishDirection,
    bool jump,
    float dt,
    const World& world,
    bool sprint,
    bool sneak,
    float terrainSpeedMultiplier,
    bool allowAutoStep,
    float gravityMultiplier,
    float jumpMultiplier,
    float groundControlMultiplier,
    float airControlMultiplier)
{
    if (!alive_ || eliminated_)
    {
        velocity_ = Vector3 { 0.0f, 0.0f, 0.0f };
        sprinting_ = false;
        sneaking_ = false;
        return;
    }

    const Vector3 normalizedWish = NormalizeOrZero(wishDirection);
    const bool wantsMovement = std::fabs(normalizedWish.x) > 0.0001f || std::fabs(normalizedWish.z) > 0.0001f;
    sneaking_ = sneak && onGround_;
    const bool nextSprinting = sprint && wantsMovement && !sneaking_;
    if (nextSprinting && !sprinting_)
    {
        RefreshSprintReset();
    }
    sprinting_ = nextSprinting;

    const float speedMultiplier = ((speedBoostTimer_ > 0.0f ? 1.22f : 1.0f) + (sprinting_ ? 0.28f : 0.0f))
        * (sneaking_ ? kSneakSpeedMultiplier : 1.0f)
        * std::clamp(terrainSpeedMultiplier, 0.35f, 1.45f);
    const Vector3 targetVelocity {
        normalizedWish.x * kMoveSpeed * speedMultiplier,
        0.0f,
        normalizedWish.z * kMoveSpeed * speedMultiplier
    };

    if (jump)
    {
        jumpBufferTimer_ = kJumpBufferSeconds;
    }
    else
    {
        jumpBufferTimer_ = std::max(0.0f, jumpBufferTimer_ - dt);
    }

    if (onGround_)
    {
        coyoteTimer_ = kCoyoteSeconds;
    }
    else
    {
        coyoteTimer_ = std::max(0.0f, coyoteTimer_ - dt);
    }

    const float acceleration = onGround_
        ? (wantsMovement ? kGroundAcceleration : kGroundDeceleration) * std::clamp(groundControlMultiplier, 0.25f, 1.35f)
        : kAirAcceleration * std::clamp(airControlMultiplier, 0.35f, 1.45f);
    const float knockbackControl = knockbackControlTimer_ > 0.0f
        ? (onGround_ ? 0.34f : 0.48f)
        : 1.0f;
    velocity_.x = Approach(velocity_.x, targetVelocity.x, acceleration * knockbackControl * dt);
    velocity_.z = Approach(velocity_.z, targetVelocity.z, acceleration * knockbackControl * dt);

    if (jumpBufferTimer_ > 0.0f && coyoteTimer_ > 0.0f)
    {
        velocity_.y = (kJumpSpeed + (jumpBoostTimer_ > 0.0f ? 1.45f : 0.0f)) * std::clamp(jumpMultiplier, 0.65f, 1.45f);
        onGround_ = false;
        coyoteTimer_ = 0.0f;
        jumpBufferTimer_ = 0.0f;
    }

    velocity_.y -= (velocity_.y < 0.0f ? kFallGravity : kGravity) * std::clamp(gravityMultiplier, 0.45f, 1.35f) * dt;

    TryMoveAxis(Vector3 { velocity_.x * dt, 0.0f, 0.0f }, world, sneaking_, allowAutoStep);
    TryMoveAxis(Vector3 { 0.0f, 0.0f, velocity_.z * dt }, world, sneaking_, allowAutoStep);

    const float oldYVelocity = velocity_.y;
    onGround_ = false;
    TryMoveAxis(Vector3 { 0.0f, velocity_.y * dt, 0.0f }, world, false, false);
    if (velocity_.y == 0.0f && oldYVelocity < 0.0f)
    {
        onGround_ = true;
    }
}

void Player::UpdateTimers(float dt)
{
    attackCooldown_ = std::max(0.0f, attackCooldown_ - dt);
    sprintResetTimer_ = std::max(0.0f, sprintResetTimer_ - dt);
    knockbackControlTimer_ = std::max(0.0f, knockbackControlTimer_ - dt);
    speedBoostTimer_ = std::max(0.0f, speedBoostTimer_ - dt);
    jumpBoostTimer_ = std::max(0.0f, jumpBoostTimer_ - dt);
    shieldTimer_ = std::max(0.0f, shieldTimer_ - dt);
    invulnerabilityTimer_ = std::max(0.0f, invulnerabilityTimer_ - dt);

    auto updateHeroAbility = [dt](HeroAbilityState& ability)
    {
        ability.cooldownRemaining = std::max(0.0f, ability.cooldownRemaining - dt);
        ability.activeTimer = std::max(0.0f, ability.activeTimer - dt);
        ability.active = ability.activeTimer > 0.0f;
    };
    updateHeroAbility(heroState_.active1);
    updateHeroAbility(heroState_.active2);
    updateHeroAbility(heroState_.ultimate);
    heroState_.animationTimer = std::max(0.0f, heroState_.animationTimer - dt);
    heroState_.orbitaPulseTimer = std::max(0.0f, heroState_.orbitaPulseTimer - dt);
    if (heroState_.animationTimer <= 0.0f
        && heroState_.animationState != HeroAnimationState::UltPrimed
        && heroState_.animationState != HeroAnimationState::Overloaded)
    {
        heroState_.animationState = HeroAnimationState::Idle;
        heroState_.animationDuration = 0.0f;
    }
    heroState_.ultimateCharge = std::clamp(heroState_.ultimateCharge, 0.0f, 100.0f);
    heroState_.ultimateReady = heroState_.ultimateCharge >= 100.0f;

    if (!alive_ && !eliminated_ && respawnTimer_ > 0.0f)
    {
        respawnTimer_ = std::max(0.0f, respawnTimer_ - dt);
    }
}

void Player::Damage(int amount)
{
    if (!alive_ || eliminated_ || invulnerabilityTimer_ > 0.0f)
    {
        return;
    }

    const int adjusted = static_cast<int>(static_cast<float>(std::max(0, amount)) * heroIncomingDamageMultiplier_ + 0.5f);
    const int mitigated = shieldTimer_ > 0.0f ? std::max(1, adjusted / 2) : adjusted;
    health_ = std::max(0, health_ - std::max(0, mitigated));
    if (heroId_ == HeroId::Radon && mitigated > 0)
    {
        AddHeroUltimateCharge(static_cast<float>(mitigated));
    }
    if (inventory_.GetArmorLevel() > 0)
    {
        inventory_.DamageArmor(std::max(1, adjusted / 5));
    }
}

void Player::Heal(int amount)
{
    if (!alive_ || eliminated_)
    {
        return;
    }

    health_ = std::min(maxHealth_, health_ + std::max(0, amount));
}

void Player::ApplyKnockback(Vector3 impulse)
{
    if (!alive_ || eliminated_)
    {
        return;
    }

    velocity_.x += impulse.x;
    velocity_.y = std::max(velocity_.y, impulse.y);
    velocity_.z += impulse.z;
    knockbackControlTimer_ = std::max(knockbackControlTimer_, kKnockbackControlSeconds);
}

void Player::ActivateHitInvulnerability(float seconds)
{
    if (!alive_ || eliminated_)
    {
        return;
    }

    invulnerabilityTimer_ = std::max(invulnerabilityTimer_, seconds);
}

void Player::ActivateSpeedBoost(float seconds)
{
    speedBoostTimer_ = std::max(speedBoostTimer_, seconds);
}

void Player::ActivateJumpBoost(float seconds)
{
    jumpBoostTimer_ = std::max(jumpBoostTimer_, seconds);
}

void Player::ActivateShield(float seconds)
{
    shieldTimer_ = std::max(shieldTimer_, seconds);
}

void Player::Teleport(Vector3 position, bool clearVelocity)
{
    if (!alive_ || eliminated_)
    {
        return;
    }

    position_ = position;
    if (clearVelocity)
    {
        velocity_ = Vector3 { 0.0f, 0.0f, 0.0f };
    }
    onGround_ = false;
}

HeroId Player::GetHeroId() const
{
    return heroId_;
}

const HeroRuntimeState& Player::GetHeroState() const
{
    return heroState_;
}

HeroRuntimeState& Player::MutableHeroState()
{
    return heroState_;
}

void Player::SetHeroId(HeroId heroId)
{
    heroId_ = heroId;
    heroState_ = HeroRuntimeState {};
    heroIncomingDamageMultiplier_ = 1.0f;
    heroOutgoingDamageMultiplier_ = 1.0f;
}

void Player::SetHeroDamageMultipliers(float incomingMultiplier, float outgoingMultiplier)
{
    heroIncomingDamageMultiplier_ = std::clamp(incomingMultiplier, 0.2f, 3.0f);
    heroOutgoingDamageMultiplier_ = std::clamp(outgoingMultiplier, 0.2f, 3.0f);
}

void Player::AddHeroUltimateCharge(float amount)
{
    SetHeroUltimateCharge(heroState_.ultimateCharge + amount);
}

void Player::SetHeroUltimateCharge(float amount)
{
    heroState_.ultimateCharge = std::clamp(amount, 0.0f, 100.0f);
    heroState_.ultimateReady = heroState_.ultimateCharge >= 100.0f;
}

bool Player::IsHeroAbilityReady(HeroAbilitySlot slot) const
{
    const HeroAbilityState* ability = nullptr;
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        ability = &heroState_.active1;
        break;
    case HeroAbilitySlot::Active2:
        ability = &heroState_.active2;
        break;
    case HeroAbilitySlot::Ultimate:
        ability = &heroState_.ultimate;
        break;
    }

    if (ability == nullptr || ability->cooldownRemaining > 0.0f)
    {
        return false;
    }
    return slot != HeroAbilitySlot::Ultimate || heroState_.ultimateReady;
}

void Player::StartHeroAbilityCooldown(HeroAbilitySlot slot, float cooldownSeconds, float durationSeconds)
{
    HeroAbilityState* ability = nullptr;
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        ability = &heroState_.active1;
        break;
    case HeroAbilitySlot::Active2:
        ability = &heroState_.active2;
        break;
    case HeroAbilitySlot::Ultimate:
        ability = &heroState_.ultimate;
        heroState_.ultimateCharge = 0.0f;
        heroState_.ultimateReady = false;
        heroState_.ultimatePrimed = false;
        break;
    }

    if (ability == nullptr)
    {
        return;
    }

    ability->cooldownRemaining = std::max(0.0f, cooldownSeconds);
    ability->activeTimer = std::max(0.0f, durationSeconds);
    ability->active = ability->activeTimer > 0.0f;
}

void Player::ClearHeroActiveEffects()
{
    heroState_.active1.active = false;
    heroState_.active1.activeTimer = 0.0f;
    heroState_.active2.active = false;
    heroState_.active2.activeTimer = 0.0f;
    heroState_.ultimate.active = false;
    heroState_.ultimate.activeTimer = 0.0f;
}

void Player::Respawn(Vector3 spawnPoint)
{
    position_ = spawnPoint;
    velocity_ = Vector3 { 0.0f, 0.0f, 0.0f };
    health_ = maxHealth_;
    alive_ = true;
    eliminated_ = false;
    respawnTimer_ = 0.0f;
    onGround_ = false;
    attackCooldown_ = 0.0f;
    attackCooldownDuration_ = 0.0f;
    jumpBufferTimer_ = 0.0f;
    coyoteTimer_ = 0.0f;
    sprinting_ = false;
    sneaking_ = false;
    sprintResetTimer_ = 0.0f;
    knockbackControlTimer_ = 0.0f;
    speedBoostTimer_ = 0.0f;
    jumpBoostTimer_ = 0.0f;
    shieldTimer_ = 0.0f;
    invulnerabilityTimer_ = 1.65f;
    ClearHeroActiveEffects();
}

void Player::Kill(bool finalDeath)
{
    alive_ = false;
    eliminated_ = finalDeath;
    respawnTimer_ = finalDeath ? 0.0f : 3.0f;
    velocity_ = Vector3 { 0.0f, 0.0f, 0.0f };
    jumpBufferTimer_ = 0.0f;
    coyoteTimer_ = 0.0f;
    sprinting_ = false;
    sneaking_ = false;
    sprintResetTimer_ = 0.0f;
    knockbackControlTimer_ = 0.0f;
    invulnerabilityTimer_ = 0.0f;
    ClearHeroActiveEffects();
}

void Player::KillWithRespawn(float seconds)
{
    Kill(false);
    respawnTimer_ = std::max(0.0f, seconds);
}

bool Player::CanAttack() const
{
    return alive_ && !eliminated_ && attackCooldown_ <= 0.0f;
}

void Player::ResetAttackCooldown(float seconds)
{
    attackCooldown_ = std::max(0.0f, seconds);
    attackCooldownDuration_ = attackCooldown_;
}

void Player::RefreshSprintReset()
{
    sprintResetTimer_ = std::max(sprintResetTimer_, kSprintResetSeconds);
}

bool Player::ConsumeSprintReset()
{
    if (sprintResetTimer_ <= 0.0f)
    {
        return false;
    }

    sprintResetTimer_ = 0.0f;
    return true;
}

bool Player::HasGroundSupportAt(Vector3 position, const World& world) const
{
    Vector3 probe = position;
    probe.y -= 0.08f;
    return world.CollidesWithAABB(probe, kGroundProbeHalfExtents);
}

void Player::TryMoveAxis(Vector3 delta, const World& world, bool preventEdgeFall, bool allowAutoStep)
{
    Vector3 next = position_;
    next.x += delta.x;
    next.y += delta.y;
    next.z += delta.z;

    const bool horizontalMove = std::fabs(delta.y) <= 0.0001f
        && (std::fabs(delta.x) > 0.0001f || std::fabs(delta.z) > 0.0001f);
    if (preventEdgeFall && horizontalMove && onGround_ && !HasGroundSupportAt(next, world))
    {
        if (delta.x != 0.0f)
        {
            velocity_.x = 0.0f;
        }
        if (delta.z != 0.0f)
        {
            velocity_.z = 0.0f;
        }
        return;
    }

    if (!world.CollidesWithAABB(next, kHalfExtents))
    {
        position_ = next;
        return;
    }

    if (allowAutoStep && horizontalMove && onGround_)
    {
        Vector3 lifted = position_;
        lifted.y += kStepHeight;
        Vector3 stepped = lifted;
        stepped.x += delta.x;
        stepped.z += delta.z;
        if (!world.CollidesWithAABB(lifted, kHalfExtents)
            && !world.CollidesWithAABB(stepped, kHalfExtents))
        {
            position_ = stepped;
            return;
        }
    }

    if (delta.x != 0.0f)
    {
        velocity_.x = 0.0f;
    }
    if (delta.y != 0.0f)
    {
        velocity_.y = 0.0f;
    }
    if (delta.z != 0.0f)
    {
        velocity_.z = 0.0f;
    }
}

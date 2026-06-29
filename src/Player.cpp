#include "Player.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
constexpr float kMoveSpeed = 4.32f;
constexpr float kGroundAcceleration = 52.0f;
constexpr float kGroundDeceleration = 46.0f;
constexpr float kAirAcceleration = 15.5f;
constexpr float kJumpSpeed = 6.72f;
constexpr float kGravity = 18.0f;
constexpr float kFallGravity = 23.5f;
constexpr float kJumpBufferSeconds = 0.12f;
constexpr float kCoyoteSeconds = 0.09f;
constexpr float kSprintResetSeconds = 0.46f;
constexpr float kKnockbackControlSeconds = 0.18f;
constexpr float kSneakSpeedMultiplier = 0.30f;
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
      homeSpawnPoint_ { spawnPoint.x, spawnPoint.y, spawnPoint.z },
      position_ { spawnPoint.x, spawnPoint.y, spawnPoint.z },
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

Vector3 Player::GetHomeSpawnPoint() const
{
    return Vector3 { homeSpawnPoint_.x, homeSpawnPoint_.y, homeSpawnPoint_.z };
}

Vector3 Player::GetPosition() const
{
    return Vector3 { position_.x, position_.y, position_.z };
}

Vector3 Player::GetVelocity() const
{
    return Vector3 { velocity_.x, velocity_.y, velocity_.z };
}

Vec3 Player::GetPositionVec3() const
{
    return position_;
}

Vec3 Player::GetVelocityVec3() const
{
    return velocity_;
}

void Player::SetPosition(Vec3 position)
{
    position_ = position;
}

void Player::SetVelocity(Vec3 velocity)
{
    velocity_ = velocity;
}

void Player::ApplyReplicatedState(int health, int maxHealth, bool alive, bool eliminated, float respawnTimer)
{
    // Client-only: reflect an authoritative snapshot's public state without
    // running combat/death logic. See docs/NETWORK_PREP_PLAN.md (Phase 0.1T).
    maxHealth_ = maxHealth > 0 ? maxHealth : maxHealth_;
    health_ = std::clamp(health, 0, maxHealth_);
    alive_ = alive;
    eliminated_ = eliminated;
    respawnTimer_ = std::max(0.0f, respawnTimer);
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

float Player::GetControlDebuffTimer() const
{
    return controlDebuffTimer_;
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

int Player::GetSelectedSlot() const
{
    return selectedSlot_;
}

void Player::SetSelectedSlot(int slot)
{
    selectedSlot_ = slot;
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
        velocity_ = Vec3 { 0.0f, 0.0f, 0.0f };
        sprinting_ = false;
        sneaking_ = false;
        return;
    }
    if (heroId_ == HeroId::Orbita && heroState_.orbitaDashRemaining > 0.0f)
    {
        velocity_ = Vec3 { 0.0f, 0.0f, 0.0f };
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
        * std::clamp(terrainSpeedMultiplier, 0.35f, 1.45f)
        * (controlDebuffTimer_ > 0.0f ? controlMoveMultiplier_ : 1.0f);
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
        velocity_.y = (kJumpSpeed + (jumpBoostTimer_ > 0.0f ? 1.45f : 0.0f))
            * std::clamp(jumpMultiplier, 0.65f, 1.45f)
            * (controlDebuffTimer_ > 0.0f ? controlJumpMultiplier_ : 1.0f);
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
    attackCooldown_ = std::max(0.0f, attackCooldown_ - dt
        * (controlDebuffTimer_ > 0.0f ? controlAttackRecoveryMultiplier_ : 1.0f));
    sprintResetTimer_ = std::max(0.0f, sprintResetTimer_ - dt);
    knockbackControlTimer_ = std::max(0.0f, knockbackControlTimer_ - dt);
    speedBoostTimer_ = std::max(0.0f, speedBoostTimer_ - dt);
    jumpBoostTimer_ = std::max(0.0f, jumpBoostTimer_ - dt);
    shieldTimer_ = std::max(0.0f, shieldTimer_ - dt);
    invulnerabilityTimer_ = std::max(0.0f, invulnerabilityTimer_ - dt);
    controlDebuffTimer_ = std::max(0.0f, controlDebuffTimer_ - dt);
    if (controlDebuffTimer_ <= 0.0f)
    {
        controlMoveMultiplier_ = 1.0f;
        controlJumpMultiplier_ = 1.0f;
        controlAttackRecoveryMultiplier_ = 1.0f;
    }

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
    heroState_.orbitaTeleportPreviewTimer = std::max(0.0f, heroState_.orbitaTeleportPreviewTimer - dt);
    if (heroState_.orbitaTeleportPreviewTimer <= 0.0f)
    {
        heroState_.orbitaTeleportPrimed = false;
    }
    UpdateLocomotionAnimation();
    heroState_.ultimateCharge = std::clamp(heroState_.ultimateCharge, 0.0f, 100.0f);
    heroState_.ultimateReady = heroState_.ultimateCharge >= 100.0f;

    if (!alive_ && !eliminated_ && respawnTimer_ > 0.0f)
    {
        respawnTimer_ = std::max(0.0f, respawnTimer_ - dt);
    }
}

void Player::UpdateLocomotionAnimation()
{
    if (heroState_.animationTimer <= 0.0f
        && heroState_.animationState != HeroAnimationState::UltPrimed
        && heroState_.animationState != HeroAnimationState::Overloaded)
    {
        const float horizontalSpeed = std::sqrt(velocity_.x * velocity_.x + velocity_.z * velocity_.z);
        if (!alive_)
        {
            heroState_.animationState = HeroAnimationState::Death;
        }
        else if (!onGround_)
        {
            heroState_.animationState = velocity_.y >= 0.0f ? HeroAnimationState::Jump : HeroAnimationState::Fall;
        }
        else if (horizontalSpeed > kMoveSpeed * 1.12f)
        {
            heroState_.animationState = HeroAnimationState::Run;
        }
        else if (horizontalSpeed > 0.12f)
        {
            heroState_.animationState = HeroAnimationState::Walk;
        }
        else
        {
            heroState_.animationState = HeroAnimationState::Idle;
        }
        heroState_.animationDuration = 0.0f;
    }
}

void Player::AdvanceAnimation(float dt)
{
    // Tick down an active event pose (attack/cast/...) then derive locomotion from
    // the current velocity. Animation only — no movement/cooldown side effects.
    heroState_.animationTimer = std::max(0.0f, heroState_.animationTimer - dt);
    UpdateLocomotionAnimation();
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
    if (mitigated > 0 && health_ > 0)
    {
        heroState_.animationState = HeroAnimationState::Hurt;
        heroState_.animationTimer = 0.22f;
        heroState_.animationDuration = 0.22f;
    }
    if (heroId_ == HeroId::Radon && mitigated > 0)
    {
        AddHeroUltimateCharge(static_cast<float>(mitigated));
    }
    if (heroId_ == HeroId::Likho && mitigated > 0)
    {
        heroState_.ultimate.active = false;
        heroState_.ultimate.activeTimer = 0.0f;
        heroState_.likhoDisguiseTeamId = -1;
        heroState_.likhoDisguisePlayerId = -1;
        heroState_.likhoDisguiseHeroId = HeroId::Likho;
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

void Player::ApplyKnockback(Vector3 impulse, float controlLossSeconds)
{
    if (!alive_ || eliminated_)
    {
        return;
    }

    velocity_.x += impulse.x;
    velocity_.y = std::max(velocity_.y, impulse.y);
    velocity_.z += impulse.z;
    knockbackControlTimer_ = std::max(
        knockbackControlTimer_,
        controlLossSeconds > 0.0f ? controlLossSeconds : kKnockbackControlSeconds);
}

void Player::AdoptReplicatedKnockbackVelocity(Vec3 velocity, float controlLossSeconds)
{
    if (!alive_ || eliminated_)
    {
        return;
    }

    velocity_ = velocity;
    knockbackControlTimer_ = std::max(
        knockbackControlTimer_,
        controlLossSeconds > 0.0f ? controlLossSeconds : kKnockbackControlSeconds);
}

CrossbowState Player::GetBlasterState() const
{
    return blasterState_;
}

float Player::GetBlasterLoadTimer() const
{
    return blasterLoadTimer_;
}

void Player::StartBlasterLoading()
{
    if (blasterState_ == CrossbowState::Unloaded)
    {
        blasterState_ = CrossbowState::Loading;
        blasterLoadTimer_ = 0.0f;
    }
}

bool Player::AdvanceBlasterLoading(float dt, float requiredSeconds)
{
    if (blasterState_ != CrossbowState::Loading)
    {
        return false;
    }
    blasterLoadTimer_ = std::min(std::max(0.05f, requiredSeconds), blasterLoadTimer_ + std::max(0.0f, dt));
    if (blasterLoadTimer_ + 0.0001f >= requiredSeconds)
    {
        blasterLoadTimer_ = requiredSeconds;
        blasterState_ = CrossbowState::Loaded;
        return true;
    }
    return false;
}

void Player::CancelBlasterLoading()
{
    if (blasterState_ == CrossbowState::Loading)
    {
        blasterState_ = CrossbowState::Unloaded;
        blasterLoadTimer_ = 0.0f;
    }
}

bool Player::ConsumeLoadedBlaster()
{
    if (blasterState_ != CrossbowState::Loaded)
    {
        return false;
    }
    blasterState_ = CrossbowState::Unloaded;
    blasterLoadTimer_ = 0.0f;
    return true;
}

float Player::GetBowDrawTimer() const
{
    return bowDrawTimer_;
}

void Player::AdvanceBowDraw(float dt)
{
    bowDrawTimer_ = std::min(kBowTuning.fullDrawTime, bowDrawTimer_ + std::max(0.0f, dt));
}

void Player::ResetBowDraw()
{
    bowDrawTimer_ = 0.0f;
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

    position_ = Vec3 { position.x, position.y, position.z };
    if (clearVelocity)
    {
        velocity_ = Vec3 { 0.0f, 0.0f, 0.0f };
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
    heroState_.ultimatePrimed = false;
    heroState_.orbitaTeleportPrimed = false;
    heroState_.orbitaTeleportPreviewTimer = 0.0f;
    heroState_.orbitaDashRemaining = 0.0f;
    heroState_.orbitaDashLiftRemaining = 0.0f;
    heroState_.likhoDisguiseTeamId = -1;
    heroState_.likhoDisguisePlayerId = -1;
    heroState_.likhoDisguiseHeroId = HeroId::Likho;
    heroState_.likhoInsideEnemyBase = false;
    controlDebuffTimer_ = 0.0f;
    controlMoveMultiplier_ = 1.0f;
    controlJumpMultiplier_ = 1.0f;
    controlAttackRecoveryMultiplier_ = 1.0f;
}

void Player::RespawnAtHome()
{
    position_ = homeSpawnPoint_;
    velocity_ = Vec3 { 0.0f, 0.0f, 0.0f };
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
    blasterState_ = CrossbowState::Unloaded;
    blasterLoadTimer_ = 0.0f;
    bowDrawTimer_ = 0.0f;
    ClearHeroActiveEffects();
    heroState_.animationState = HeroAnimationState::Idle;
    heroState_.animationTimer = 0.0f;
    heroState_.animationDuration = 0.0f;
}

void Player::Kill(bool finalDeath)
{
    alive_ = false;
    eliminated_ = finalDeath;
    respawnTimer_ = finalDeath ? 0.0f : 3.0f;
    velocity_ = Vec3 { 0.0f, 0.0f, 0.0f };
    jumpBufferTimer_ = 0.0f;
    coyoteTimer_ = 0.0f;
    sprinting_ = false;
    sneaking_ = false;
    sprintResetTimer_ = 0.0f;
    knockbackControlTimer_ = 0.0f;
    invulnerabilityTimer_ = 0.0f;
    blasterState_ = CrossbowState::Unloaded;
    blasterLoadTimer_ = 0.0f;
    bowDrawTimer_ = 0.0f;
    ClearHeroActiveEffects();
    if (heroState_.animationState != HeroAnimationState::DeathSacrifice)
    {
        heroState_.animationState = HeroAnimationState::Death;
    }
    heroState_.animationTimer = 0.65f;
    heroState_.animationDuration = 0.65f;
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
    heroState_.animationState = HeroAnimationState::Attack;
    heroState_.animationTimer = std::min(0.32f, attackCooldown_);
    heroState_.animationDuration = heroState_.animationTimer;
}

void Player::ApplyControlDebuff(float seconds, float moveMultiplier, float jumpMultiplier, float attackRecoveryMultiplier)
{
    if (!alive_ || eliminated_)
    {
        return;
    }
    controlDebuffTimer_ = std::max(controlDebuffTimer_, std::max(0.0f, seconds));
    controlMoveMultiplier_ = std::min(controlMoveMultiplier_, std::clamp(moveMultiplier, 0.2f, 1.0f));
    controlJumpMultiplier_ = std::min(controlJumpMultiplier_, std::clamp(jumpMultiplier, 0.2f, 1.0f));
    controlAttackRecoveryMultiplier_ = std::min(controlAttackRecoveryMultiplier_, std::clamp(attackRecoveryMultiplier, 0.2f, 1.0f));
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
    Vector3 next { position_.x, position_.y, position_.z };
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
        position_ = Vec3 { next.x, next.y, next.z };
        return;
    }

    if (allowAutoStep && horizontalMove && onGround_)
    {
        Vector3 lifted { position_.x, position_.y, position_.z };
        lifted.y += kStepHeight;
        Vector3 stepped = lifted;
        stepped.x += delta.x;
        stepped.z += delta.z;
        if (!world.CollidesWithAABB(lifted, kHalfExtents)
            && !world.CollidesWithAABB(stepped, kHalfExtents))
        {
            position_ = Vec3 { stepped.x, stepped.y, stepped.z };
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

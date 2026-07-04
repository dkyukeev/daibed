#pragma once

#include "Hero.h"
#include "Inventory.h"
#include "RangedCombat.h"
#include "Simulation/SimMath.h"
#include "World.h"
#include "raylib.h"

#include <string>

enum class PlayerControlKind
{
    LocalHumanPredicted,
    RemoteHumanAuthoritative,
    BotAuthoritative,
    Replica,
    Spectator
};

bool IsHumanControlled(PlayerControlKind kind);
bool IsBotControlled(PlayerControlKind kind);
bool IsLocallyPredicted(PlayerControlKind kind);
bool HasLocalCamera(PlayerControlKind kind);

class Player
{
public:
    Player() = default;
    Player(int id, std::string name, int teamId, Vector3 spawnPoint, bool local);

    int GetId() const;
    const std::string& GetName() const;
    int GetTeamId() const;
    Vector3 GetHomeSpawnPoint() const;
    Vector3 GetPosition() const; // raylib adapter over the Vec3 storage
    Vector3 GetVelocity() const; // raylib adapter over the Vec3 storage
    // Raylib-free boundary helpers (preferred for sim/snapshot; the spatial
    // storage is Vec3 internally). See docs/NETWORK_PREP_PLAN.md.
    Vec3 GetPositionVec3() const;
    Vec3 GetVelocityVec3() const;
    void SetPosition(Vec3 position);
    void SetVelocity(Vec3 velocity);
    // Client-only: reflect an authoritative snapshot's public health/alive state
    // (no combat/death logic runs). See docs/NETWORK_PREP_PLAN.md (Phase 0.1T).
    void ApplyReplicatedState(int health, int maxHealth, bool alive, bool eliminated, float respawnTimer);
    float GetYaw() const;
    int GetHealth() const;
    int GetMaxHealth() const;
    bool IsAlive() const;
    bool IsEliminated() const;
    PlayerControlKind GetControlKind() const;
    void SetControlKind(PlayerControlKind kind);
    bool IsOnGround() const;
    bool IsSprinting() const;
    bool IsSneaking() const;
    bool HasSprintReset() const;
    float GetRespawnTimer() const;
    float GetAttackCooldownRemaining() const;
    float GetAttackCooldownDuration() const;
    float GetSpeedBoostTimer() const;
    float GetJumpBoostTimer() const;
    float GetShieldTimer() const;
    float GetInvulnerabilityTimer() const;
    float GetControlDebuffTimer() const;
    bool HasShield() const;
    bool IsInvulnerable() const;
    float GetHeroOutgoingDamageMultiplier() const;
    HeroId GetHeroId() const;
    const HeroRuntimeState& GetHeroState() const;
    HeroRuntimeState& MutableHeroState();

    Inventory& GetInventory();
    const Inventory& GetInventory() const;

    // Per-player active hotbar slot. The local player still mirrors the slot in
    // Game::selectedHotbarSlot_ for UI; network-controlled players (bots aside)
    // need their own slot so the authoritative server can replicate it (Phase
    // 0.1W). See docs/NETWORK_PREP_PLAN.md.
    int GetSelectedSlot() const;
    void SetSelectedSlot(int slot);

    Vector3 Forward() const;
    Vector3 Right() const;

    void AddYaw(float delta);
    void SetYaw(float yaw);
    void Move(
        Vector3 wishDirection,
        bool jump,
        float dt,
        const World& world,
        bool sprint = false,
        bool sneak = false,
        float terrainSpeedMultiplier = 1.0f,
        bool allowAutoStep = false,
        float gravityMultiplier = 1.0f,
        float jumpMultiplier = 1.0f,
        float groundControlMultiplier = 1.0f,
        float airControlMultiplier = 1.0f);
    void UpdateTimers(float dt);
    // Client-side own-player hook: advance ONLY the animation (tick the event-pose
    // timer, then derive locomotion from the current velocity) without touching
    // movement/cooldown timers. Lets a predicted player animate at zero lag while
    // event poses (attack/cast/death) still come from the authoritative snapshot.
    void AdvanceAnimation(float dt);

    void Damage(int amount);
    void Heal(int amount);
    void ApplyKnockback(Vector3 impulse, float controlLossSeconds = -1.0f);
    void AdoptReplicatedKnockbackVelocity(Vec3 velocity, float controlLossSeconds = -1.0f);
    void ActivateHitInvulnerability(float seconds);
    void ActivateSpeedBoost(float seconds);
    void ActivateJumpBoost(float seconds);
    void ActivateShield(float seconds);
    void ApplyControlDebuff(float seconds, float moveMultiplier, float jumpMultiplier, float attackRecoveryMultiplier);
    void Teleport(Vector3 position, bool clearVelocity = true);
    void SetHeroId(HeroId heroId);
    void SetHeroDamageMultipliers(float incomingMultiplier, float outgoingMultiplier);
    void AddHeroUltimateCharge(float amount);
    void SetHeroUltimateCharge(float amount);
    bool IsHeroAbilityReady(HeroAbilitySlot slot) const;
    void StartHeroAbilityCooldown(HeroAbilitySlot slot, float cooldownSeconds, float durationSeconds = 0.0f);
    void ClearHeroActiveEffects();
    void RespawnAtHome();
    void Kill(bool finalDeath);
    void KillWithRespawn(float seconds);

    bool CanAttack() const;
    void ResetAttackCooldown(float seconds);
    void RefreshSprintReset();
    bool ConsumeSprintReset();
    CrossbowState GetBlasterState() const;
    float GetBlasterLoadTimer() const;
    void StartBlasterLoading();
    bool AdvanceBlasterLoading(float dt, float requiredSeconds);
    void CancelBlasterLoading();
    bool ConsumeLoadedBlaster();
    float GetBowDrawTimer() const;
    void AdvanceBowDraw(float dt);
    void ResetBowDraw();
    // Client-side visual sync only: a network client's own weapon-charge
    // fields are never advanced locally (charging only ever runs
    // authoritatively via ApplyNetworkPlayerActions on the server), so
    // without these setters the client's combat-charge HUD reads stuck
    // defaults regardless of the server's true state. Not used server-side.
    void SetBowDrawTimerReplicated(float value);
    void SetBlasterStateReplicated(CrossbowState state, float loadTimer);

private:
    bool HasGroundSupportAt(Vector3 position, const World& world) const;
    void TryMoveAxis(Vector3 delta, const World& world, bool preventEdgeFall, bool allowAutoStep);
    // Derive the locomotion pose (Idle/Walk/Run/Jump/Fall/Death) from the current
    // velocity/ground/alive state, but only when no event pose is playing
    // (animationTimer <= 0 and not UltPrimed/Overloaded). Shared by UpdateTimers
    // (server/local sim) and AdvanceAnimation (client own-player).
    void UpdateLocomotionAnimation();

    int id_ = -1;
    std::string name_;
    int teamId_ = -1;
    Vec3 homeSpawnPoint_ {};
    Vec3 position_ {};
    Vec3 velocity_ {};
    float yaw_ = 0.0f;
    int maxHealth_ = 100;
    int health_ = 100;
    bool alive_ = true;
    bool eliminated_ = false;
    // The single source of truth for "who drives this player" — there is no
    // separate local_ flag. The ctor's `local` argument maps to
    // LocalHumanPredicted/BotAuthoritative; SetControlKind refines it (remote
    // humans, replicas). Query via IsLocallyPredicted/IsHumanControlled/
    // IsBotControlled/HasLocalCamera, never a bespoke bool.
    PlayerControlKind controlKind_ = PlayerControlKind::Replica;
    bool onGround_ = false;
    bool sprinting_ = false;
    bool sneaking_ = false;
    float attackCooldown_ = 0.0f;
    float attackCooldownDuration_ = 0.0f;
    float respawnTimer_ = 0.0f;
    float jumpBufferTimer_ = 0.0f;
    float coyoteTimer_ = 0.0f;
    float sprintResetTimer_ = 0.0f;
    float knockbackControlTimer_ = 0.0f;
    float speedBoostTimer_ = 0.0f;
    float jumpBoostTimer_ = 0.0f;
    float shieldTimer_ = 0.0f;
    float invulnerabilityTimer_ = 0.0f;
    float controlDebuffTimer_ = 0.0f;
    float controlMoveMultiplier_ = 1.0f;
    float controlJumpMultiplier_ = 1.0f;
    float controlAttackRecoveryMultiplier_ = 1.0f;
    CrossbowState blasterState_ = CrossbowState::Unloaded;
    float blasterLoadTimer_ = 0.0f;
    float bowDrawTimer_ = 0.0f;
    int selectedSlot_ = 0;
    HeroId heroId_ = HeroId::Radon;
    HeroRuntimeState heroState_ {};
    float heroIncomingDamageMultiplier_ = 1.0f;
    float heroOutgoingDamageMultiplier_ = 1.0f;
    Inventory inventory_;
};

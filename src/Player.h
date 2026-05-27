#pragma once

#include "Inventory.h"
#include "World.h"
#include "raylib.h"

#include <string>

class Player
{
public:
    Player() = default;
    Player(int id, std::string name, int teamId, Vector3 spawnPoint, bool local);

    int GetId() const;
    const std::string& GetName() const;
    int GetTeamId() const;
    Vector3 GetPosition() const;
    Vector3 GetVelocity() const;
    float GetYaw() const;
    int GetHealth() const;
    int GetMaxHealth() const;
    bool IsAlive() const;
    bool IsEliminated() const;
    bool IsLocal() const;
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
    bool HasShield() const;
    bool IsInvulnerable() const;

    Inventory& GetInventory();
    const Inventory& GetInventory() const;

    Vector3 Forward() const;
    Vector3 Right() const;

    void AddYaw(float delta);
    void SetYaw(float yaw);
    void Move(Vector3 wishDirection, bool jump, float dt, const World& world, bool sprint = false, bool sneak = false, float terrainSpeedMultiplier = 1.0f, bool allowAutoStep = false);
    void UpdateTimers(float dt);

    void Damage(int amount);
    void Heal(int amount);
    void ApplyKnockback(Vector3 impulse);
    void ActivateHitInvulnerability(float seconds);
    void ActivateSpeedBoost(float seconds);
    void ActivateJumpBoost(float seconds);
    void ActivateShield(float seconds);
    void Respawn(Vector3 spawnPoint);
    void Kill(bool finalDeath);

    bool CanAttack() const;
    void ResetAttackCooldown(float seconds);
    void RefreshSprintReset();
    bool ConsumeSprintReset();

private:
    bool HasGroundSupportAt(Vector3 position, const World& world) const;
    void TryMoveAxis(Vector3 delta, const World& world, bool preventEdgeFall, bool allowAutoStep);

    int id_ = -1;
    std::string name_;
    int teamId_ = -1;
    Vector3 position_ {};
    Vector3 velocity_ {};
    float yaw_ = 0.0f;
    int maxHealth_ = 100;
    int health_ = 100;
    bool alive_ = true;
    bool eliminated_ = false;
    bool local_ = false;
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
    Inventory inventory_;
};

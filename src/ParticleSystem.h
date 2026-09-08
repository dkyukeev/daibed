#pragma once

#include "Block.h"

#include "raylib.h"

#include <array>
#include <cstdint>
#include <vector>

struct ParticleGlowLight
{
    Vector3 position {};
    Color color = WHITE;
    float radius = 3.0f;
    float intensity = 0.4f;
};

enum class ParticleKind
{
    Debris,
    Dust,
    Spark,
    Smoke,
    Trail,
    PickupStreak,
    PickupMote,
    GroundDust,
    Fiber,
    Splinter,
    Shard,
    ImpactStreak,
    BuildTrace,
};

// Presentation-only material families. They deliberately collapse the large
// BlockType vocabulary into a few readable motion/shape recipes.
enum class ParticleMaterial : std::uint8_t
{
    Stone,
    Earth,
    Fabric,
    Wood,
    Metal,
    Glass,
    Energy,
    Character,
};

ParticleMaterial ParticleMaterialFromBlock(BlockType type);

enum class ParticlePriority : std::uint8_t
{
    Ambient = 0,
    Gameplay = 1,
    Critical = 2,
};

enum class AbilityParticleStyle : std::uint8_t
{
    Burst,
    Ring,
    Cone,
    Pull,
    Trail,
    CorePulse,
    Sacrifice,
};

class ParticleSystem
{
public:
    void Clear();
    void SetQuality(int quality);
    void Update(float dt);
    void Emit(
        ParticleKind kind,
        Vector3 position,
        Vector3 direction,
        Color color,
        int count,
        float speed = 2.0f);
    void EmitPickup(
        Vector3 source,
        Vector3 target,
        Color color,
        int amount = 1);
    void EmitLanding(
        Vector3 groundPosition,
        Vector3 planarVelocity,
        Color surfaceColor,
        float intensity);
    void EmitImpact(
        Vector3 position,
        Vector3 direction,
        Color color,
        ParticleMaterial material,
        float intensity = 1.0f);
    void EmitBlockBreak(
        Vector3 position,
        Vector3 direction,
        Color color,
        ParticleMaterial material);
    void EmitBlockPlace(
        Vector3 position,
        Vector3 direction,
        Color color,
        ParticleMaterial material);
    void EmitAbility(
        Vector3 position,
        Vector3 direction,
        Color color,
        float radius,
        AbilityParticleStyle style);
    void EmitHeal(Vector3 position, Color color, float intensity = 1.0f);
    void EmitTrap(Vector3 position, Color color, float radius, bool triggered);
    void EmitDevice(Vector3 position, Color color, float intensity, bool assembling);
    void EmitCoreDestruction(Vector3 position, Color color, float intensity = 1.0f);
    void EmitRespawn(Vector3 position, Color color);
    void EmitProjectileCue(Vector3 position, Vector3 direction, Color color, float intensity = 1.0f);
    void EmitHazard(Vector3 position, Color color, float radius);
    // Draw in two material classes so debris/dust inherit scene lighting and
    // energy/gameplay streaks can be explicitly emissive.
    void Draw(bool emissiveOnly) const;
    void AppendGlowLights(std::vector<ParticleGlowLight>& lights) const;
    int ActiveCount() const;
    int ActiveCount(ParticleKind kind) const;

private:
    struct Particle
    {
        Vector3 position {};
        Vector3 previousPosition {};
        Vector3 velocity {};
        Vector3 target {};
        Color color = WHITE;
        float age = 0.0f;
        float lifetime = 0.5f;
        float size = 0.08f;
        float endSize = 0.02f;
        float drag = 3.6f;
        float gravity = 5.6f;
        ParticleKind kind = ParticleKind::Spark;
        ParticlePriority priority = ParticlePriority::Ambient;
        bool seeksTarget = false;
        bool active = false;
    };

    static constexpr int kCapacity = 512;
    int QualityLimit() const;
    int QualityCount(int requested) const;
    Particle* Allocate(ParticlePriority priority);
    void Spawn(
        ParticleKind kind,
        ParticlePriority priority,
        Vector3 position,
        Vector3 velocity,
        Color color,
        float lifetime,
        float startSize,
        float endSize,
        float drag,
        float gravity,
        Vector3 target = Vector3 {},
        bool seeksTarget = false);

    std::array<Particle, kCapacity> particles_ {};
    int nextSlot_ = 0;
    int quality_ = 2;
    std::uint32_t emissionSerial_ = 1;
};

// Pure, window-free diagnostic used by --particle-system-smoke.
int RunParticleSystemSmoke();

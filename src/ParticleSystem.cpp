#include "ParticleSystem.h"

#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
constexpr float kTau = 6.28318530718f;

float Hash01(std::uint32_t value)
{
    std::uint32_t state = value * 747796405u + 2891336453u;
    state = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    state = (state >> 22u) ^ state;
    return static_cast<float>(state & 0xffffu) / 65535.0f;
}

Color ScaleColor(Color color, float scale)
{
    return Color {
        static_cast<unsigned char>(std::clamp(static_cast<int>(color.r * scale), 0, 255)),
        static_cast<unsigned char>(std::clamp(static_cast<int>(color.g * scale), 0, 255)),
        static_cast<unsigned char>(std::clamp(static_cast<int>(color.b * scale), 0, 255)),
        color.a,
    };
}

Vector3 SafeNormalize(Vector3 value, Vector3 fallback)
{
    return Vector3LengthSqr(value) > 0.0001f ? Vector3Normalize(value) : fallback;
}

Vector3 Perpendicular(Vector3 direction)
{
    Vector3 side = Vector3CrossProduct(direction, Vector3 { 0.0f, 1.0f, 0.0f });
    if (Vector3LengthSqr(side) < 0.0001f)
    {
        side = Vector3CrossProduct(direction, Vector3 { 1.0f, 0.0f, 0.0f });
    }
    return SafeNormalize(side, Vector3 { 1.0f, 0.0f, 0.0f });
}

ParticleKind FragmentKind(ParticleMaterial material)
{
    switch (material)
    {
    case ParticleMaterial::Earth: return ParticleKind::GroundDust;
    case ParticleMaterial::Fabric: return ParticleKind::Fiber;
    case ParticleMaterial::Wood: return ParticleKind::Splinter;
    case ParticleMaterial::Metal: return ParticleKind::Spark;
    case ParticleMaterial::Glass: return ParticleKind::Shard;
    case ParticleMaterial::Energy: return ParticleKind::PickupMote;
    case ParticleMaterial::Character: return ParticleKind::ImpactStreak;
    case ParticleMaterial::Stone:
    default: return ParticleKind::Debris;
    }
}
}

ParticleMaterial ParticleMaterialFromBlock(BlockType type)
{
    switch (type)
    {
    case BlockType::GrassBlock:
    case BlockType::DirtBlock:
    case BlockType::LeafBlock:
        return ParticleMaterial::Earth;
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        return ParticleMaterial::Fabric;
    case BlockType::WoodBlock:
    case BlockType::PlankBlock:
    case BlockType::BirchPlankBlock:
    case BlockType::BirchSlabBlock:
    case BlockType::BirchStairsBlock:
    case BlockType::LadderBlock:
        return ParticleMaterial::Wood;
    case BlockType::MetalBlock:
    case BlockType::IronBarsBlock:
    case BlockType::GoldBlock:
        return ParticleMaterial::Metal;
    case BlockType::EnergyGlassBlock:
    case BlockType::ColoredGlassBlock:
    case BlockType::IceBlock:
        return ParticleMaterial::Glass;
    case BlockType::ResourceGenerator:
    case BlockType::EnergyCoreBlock:
    case BlockType::SpringBlock:
    case BlockType::StickyBlock:
    case BlockType::ExplosiveBlock:
    case BlockType::SpikeBlock:
    case BlockType::LavaBlock:
    case BlockType::GlowBlock:
    case BlockType::DiamondBlock:
    case BlockType::EmeraldBlock:
        return ParticleMaterial::Energy;
    case BlockType::Air:
        return ParticleMaterial::Energy;
    case BlockType::Solid:
    case BlockType::StoneBlock:
    case BlockType::ObsidianBlock:
    case BlockType::SmoothStoneBlock:
    case BlockType::DarkBrickBlock:
    case BlockType::LightBrickBlock:
    case BlockType::DecorativeTileBlock:
    case BlockType::TrimBlock:
    case BlockType::CobblestoneBlock:
    case BlockType::AndesiteBlock:
    case BlockType::PolishedAndesiteBlock:
    case BlockType::StoneBrickBlock:
    case BlockType::ChiseledStoneBrickBlock:
    case BlockType::StoneSlabBlock:
    case BlockType::StoneBrickSlabBlock:
    case BlockType::StoneBrickStairsBlock:
    case BlockType::ColoredClayBlock:
    case BlockType::BarrierBlock:
    case BlockType::Count:
    default:
        return ParticleMaterial::Stone;
    }
}

void ParticleSystem::Clear()
{
    for (Particle& particle : particles_)
    {
        particle.active = false;
    }
    nextSlot_ = 0;
    emissionSerial_ = 1;
}

void ParticleSystem::SetQuality(int quality)
{
    quality_ = std::clamp(quality, 0, 2);
}

int ParticleSystem::QualityLimit() const
{
    return quality_ == 0 ? 128 : (quality_ == 1 ? 256 : kCapacity);
}

int ParticleSystem::QualityCount(int requested) const
{
    if (requested <= 0)
    {
        return 0;
    }
    return quality_ == 0 ? std::max(1, requested / 3)
        : (quality_ == 1 ? std::max(1, requested * 2 / 3) : requested);
}

ParticleSystem::Particle* ParticleSystem::Allocate(ParticlePriority priority)
{
    const int limit = QualityLimit();
    for (int offset = 0; offset < limit; ++offset)
    {
        const int slot = (nextSlot_ + offset) % limit;
        if (!particles_[slot].active)
        {
            nextSlot_ = (slot + 1) % limit;
            return &particles_[slot];
        }
    }

    // Preserve gameplay feedback under load: a new event may evict an older
    // particle of equal/lower priority, never a more important one.
    int candidate = -1;
    int candidatePriority = 99;
    float candidateProgress = -1.0f;
    for (int slot = 0; slot < limit; ++slot)
    {
        const Particle& particle = particles_[slot];
        const int existingPriority = static_cast<int>(particle.priority);
        if (existingPriority > static_cast<int>(priority))
        {
            continue;
        }
        const float progress = particle.age / std::max(0.001f, particle.lifetime);
        if (existingPriority < candidatePriority
            || (existingPriority == candidatePriority && progress > candidateProgress))
        {
            candidate = slot;
            candidatePriority = existingPriority;
            candidateProgress = progress;
        }
    }
    if (candidate < 0)
    {
        return nullptr;
    }
    nextSlot_ = (candidate + 1) % limit;
    return &particles_[candidate];
}

void ParticleSystem::Spawn(
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
    Vector3 target,
    bool seeksTarget)
{
    Particle* particle = Allocate(priority);
    if (particle == nullptr)
    {
        return;
    }
    particle->position = position;
    particle->previousPosition = position;
    particle->velocity = velocity;
    particle->target = target;
    particle->color = color;
    particle->age = 0.0f;
    particle->lifetime = std::max(0.01f, lifetime);
    particle->size = std::max(0.001f, startSize);
    particle->endSize = std::max(0.001f, endSize);
    particle->drag = std::max(0.0f, drag);
    particle->gravity = gravity;
    particle->kind = kind;
    particle->priority = priority;
    particle->seeksTarget = seeksTarget;
    particle->active = true;
}

void ParticleSystem::Update(float dt)
{
    if (dt <= 0.0f)
    {
        return;
    }
    dt = std::min(dt, 0.05f);
    for (Particle& particle : particles_)
    {
        if (!particle.active)
        {
            continue;
        }
        particle.age += dt;
        if (particle.age >= particle.lifetime)
        {
            particle.active = false;
            continue;
        }

        particle.previousPosition = particle.position;
        if (particle.seeksTarget)
        {
            const Vector3 toTarget = Vector3Subtract(particle.target, particle.position);
            const float distance = Vector3Length(toTarget);
            if (distance < 0.055f)
            {
                particle.active = false;
                continue;
            }
            const float progress = particle.age / particle.lifetime;
            const float attraction = 16.0f + progress * 28.0f;
            particle.velocity = Vector3Add(
                particle.velocity,
                Vector3Scale(SafeNormalize(toTarget, Vector3 { 0.0f, 1.0f, 0.0f }), attraction * dt));
        }

        particle.velocity = Vector3Scale(particle.velocity, std::exp(-particle.drag * dt));
        particle.velocity.y -= particle.gravity * dt;
        particle.position = Vector3Add(particle.position, Vector3Scale(particle.velocity, dt));
    }
}

void ParticleSystem::Emit(
    ParticleKind kind,
    Vector3 position,
    Vector3 direction,
    Color color,
    int count,
    float speed)
{
    const int emittedCount = QualityCount(count);
    const Vector3 baseDirection = SafeNormalize(direction, Vector3 { 0.0f, 1.0f, 0.0f });
    const ParticlePriority priority = kind == ParticleKind::Dust || kind == ParticleKind::Smoke
        ? ParticlePriority::Ambient
        : ParticlePriority::Gameplay;
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < emittedCount; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const Vector3 randomDirection {
            Hash01(seed + 1) * 2.0f - 1.0f,
            Hash01(seed + 2) * 1.3f,
            Hash01(seed + 3) * 2.0f - 1.0f,
        };
        const Vector3 velocityDirection = SafeNormalize(
            Vector3Add(Vector3Scale(baseDirection, 0.55f), randomDirection), baseDirection);
        const float particleSpeed = speed * (0.55f + Hash01(seed + 4) * 0.9f);
        const bool smoke = kind == ParticleKind::Smoke;
        const bool trail = kind == ParticleKind::Trail;
        const bool debris = kind == ParticleKind::Debris;
        Spawn(
            kind,
            priority,
            position,
            Vector3Scale(velocityDirection, particleSpeed),
            color,
            smoke ? 1.15f + Hash01(seed + 5) * 0.55f
                : (trail ? 0.20f : 0.35f + Hash01(seed + 5) * 0.35f),
            smoke ? 0.14f : (debris ? 0.085f : 0.055f),
            smoke ? 0.26f : 0.018f,
            smoke ? 1.4f : 3.6f,
            smoke ? -0.42f : (trail ? 0.0f : 5.6f));
    }
}

void ParticleSystem::EmitPickup(
    Vector3 source,
    Vector3 target,
    Color color,
    int amount)
{
    const int emittedCount = QualityCount(7 + std::min(std::max(amount, 1), 7));
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < emittedCount; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * (static_cast<float>(i) / std::max(1, emittedCount) + Hash01(seed + 1) * 0.16f);
        const float radius = 0.14f + Hash01(seed + 2) * 0.46f;
        const Vector3 radial { std::cos(angle), 0.0f, std::sin(angle) };
        const Vector3 start {
            source.x + radial.x * radius,
            source.y + 0.04f + Hash01(seed + 3) * 0.38f,
            source.z + radial.z * radius,
        };
        const Vector3 toTarget = SafeNormalize(Vector3Subtract(target, start), Vector3 { 0.0f, 1.0f, 0.0f });
        const Vector3 tangent { -radial.z, 0.0f, radial.x };
        const Vector3 velocity = Vector3Add(
            Vector3Scale(toTarget, 2.0f + Hash01(seed + 4) * 1.5f),
            Vector3Scale(tangent, (Hash01(seed + 5) - 0.5f) * 1.3f));
        const bool mote = (i % 4) == 0;
        Spawn(
            mote ? ParticleKind::PickupMote : ParticleKind::PickupStreak,
            ParticlePriority::Gameplay,
            start,
            velocity,
            mote ? ScaleColor(color, 1.22f) : color,
            0.22f + Hash01(seed + 6) * 0.16f,
            mote ? 0.050f : 0.032f,
            0.010f,
            0.8f,
            0.0f,
            target,
            true);
    }
}

void ParticleSystem::EmitLanding(
    Vector3 groundPosition,
    Vector3 planarVelocity,
    Color surfaceColor,
    float intensity)
{
    intensity = std::clamp(intensity, 0.0f, 1.0f);
    const int emittedCount = QualityCount(7 + static_cast<int>(intensity * 9.0f));
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    Vector3 travelDirection { planarVelocity.x, 0.0f, planarVelocity.z };
    travelDirection = SafeNormalize(travelDirection, Vector3 {});
    for (int i = 0; i < emittedCount; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * (static_cast<float>(i) / std::max(1, emittedCount) + Hash01(seed + 1) * 0.22f);
        Vector3 outward { std::cos(angle), 0.0f, std::sin(angle) };
        outward = SafeNormalize(Vector3Add(outward, Vector3Scale(travelDirection, 0.28f)), outward);
        const float radialOffset = 0.06f + Hash01(seed + 2) * 0.22f;
        const Vector3 start {
            groundPosition.x + outward.x * radialOffset,
            groundPosition.y + 0.015f + Hash01(seed + 3) * 0.035f,
            groundPosition.z + outward.z * radialOffset,
        };
        const bool chip = i % 4 == 0;
        if (chip)
        {
            const float speed = 0.75f + intensity * 1.35f + Hash01(seed + 4) * 0.75f;
            Spawn(
                ParticleKind::Debris,
                ParticlePriority::Gameplay,
                start,
                Vector3 { outward.x * speed, 0.55f + intensity * 1.05f, outward.z * speed },
                ScaleColor(surfaceColor, 0.78f + Hash01(seed + 5) * 0.32f),
                0.30f + Hash01(seed + 6) * 0.20f,
                0.040f + intensity * 0.030f,
                0.022f,
                2.8f,
                6.4f);
        }
        else
        {
            const float speed = 0.55f + intensity * 1.10f + Hash01(seed + 4) * 0.55f;
            Spawn(
                ParticleKind::GroundDust,
                ParticlePriority::Gameplay,
                start,
                Vector3 { outward.x * speed, 0.08f + Hash01(seed + 5) * 0.20f, outward.z * speed },
                ScaleColor(surfaceColor, 0.88f + Hash01(seed + 6) * 0.24f),
                0.28f + Hash01(seed + 7) * 0.22f,
                0.045f + intensity * 0.025f,
                0.11f + intensity * 0.09f,
                3.4f,
                0.35f);
        }
    }
}

void ParticleSystem::EmitImpact(
    Vector3 position,
    Vector3 direction,
    Color color,
    ParticleMaterial material,
    float intensity)
{
    intensity = std::clamp(intensity, 0.35f, 1.5f);
    const Vector3 forward = SafeNormalize(direction, Vector3 { 0.0f, 0.15f, 1.0f });
    const Vector3 side = Perpendicular(forward);
    const Vector3 up = SafeNormalize(Vector3CrossProduct(side, forward), Vector3 { 0.0f, 1.0f, 0.0f });
    const ParticleKind fragmentKind = FragmentKind(material);
    const int fragmentCount = QualityCount(
        (material == ParticleMaterial::Metal || material == ParticleMaterial::Glass ? 8 : 6)
        + static_cast<int>(intensity * 2.0f));
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;

    for (int i = 0; i < fragmentCount; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        Vector3 fan = Vector3Add(
            forward,
            Vector3Add(
                Vector3Scale(side, (Hash01(seed + 1) - 0.5f) * 1.15f),
                Vector3Scale(up, (Hash01(seed + 2) - 0.32f) * 0.85f)));
        fan = SafeNormalize(fan, forward);
        float speed = (1.8f + Hash01(seed + 3) * 2.2f) * intensity;
        float lifetime = 0.16f + Hash01(seed + 4) * 0.16f;
        float size = 0.040f + Hash01(seed + 5) * 0.035f;
        float gravity = 3.8f;
        float drag = 4.2f;
        if (material == ParticleMaterial::Fabric)
        {
            speed *= 0.62f; lifetime += 0.16f; gravity = 0.9f; drag = 2.8f; size *= 0.72f;
        }
        else if (material == ParticleMaterial::Wood)
        {
            gravity = 6.2f; drag = 2.6f; size *= 0.85f;
        }
        else if (material == ParticleMaterial::Metal)
        {
            speed *= 1.45f; gravity = 4.5f; drag = 2.0f; size *= 0.52f;
        }
        else if (material == ParticleMaterial::Glass)
        {
            speed *= 1.18f; gravity = 5.4f; drag = 1.8f; size *= 0.76f;
        }
        else if (material == ParticleMaterial::Energy)
        {
            speed *= 1.25f; gravity = 0.0f; drag = 5.0f; size *= 0.72f;
        }
        else if (material == ParticleMaterial::Character)
        {
            speed *= 1.12f; lifetime *= 0.72f; gravity = 0.4f; drag = 6.5f; size *= 0.68f;
        }
        Spawn(
            fragmentKind,
            ParticlePriority::Critical,
            position,
            Vector3Scale(fan, speed),
            ScaleColor(color, 0.86f + Hash01(seed + 6) * 0.34f),
            lifetime,
            size,
            std::max(0.007f, size * 0.28f),
            drag,
            gravity);
    }

    // Hard surfaces shed a compact secondary layer. Soft/character impacts
    // stay clean and directional instead of becoming a generic puff.
    int secondaryCount = 0;
    ParticleKind secondaryKind = ParticleKind::Dust;
    if (material == ParticleMaterial::Stone || material == ParticleMaterial::Earth
        || material == ParticleMaterial::Wood)
    {
        secondaryCount = QualityCount(material == ParticleMaterial::Earth ? 5 : 3);
        secondaryKind = material == ParticleMaterial::Earth ? ParticleKind::GroundDust : ParticleKind::Dust;
    }
    else if (material == ParticleMaterial::Metal || material == ParticleMaterial::Energy)
    {
        secondaryCount = QualityCount(3);
        secondaryKind = ParticleKind::Spark;
    }
    for (int i = 0; i < secondaryCount; ++i)
    {
        const std::uint32_t seed = emissionSeed + 0x9e3779b9u + static_cast<std::uint32_t>(i) * 668265263u;
        const float angle = kTau * Hash01(seed + 1);
        const Vector3 radial { std::cos(angle), 0.08f + Hash01(seed + 2) * 0.22f, std::sin(angle) };
        Spawn(
            secondaryKind,
            ParticlePriority::Gameplay,
            position,
            Vector3Scale(radial, 0.65f + Hash01(seed + 3) * 0.85f),
            ScaleColor(color, secondaryKind == ParticleKind::Spark ? 1.25f : 0.72f),
            0.18f + Hash01(seed + 4) * 0.18f,
            secondaryKind == ParticleKind::Spark ? 0.022f : 0.050f,
            secondaryKind == ParticleKind::Spark ? 0.006f : 0.11f,
            3.8f,
            secondaryKind == ParticleKind::Spark ? 2.2f : 0.35f);
    }
}

void ParticleSystem::EmitBlockBreak(
    Vector3 position,
    Vector3 direction,
    Color color,
    ParticleMaterial material)
{
    const ParticleKind fragmentKind = FragmentKind(material);
    const Vector3 bias = SafeNormalize(direction, Vector3 { 0.0f, 0.25f, 1.0f });
    const int fragmentCount = QualityCount(material == ParticleMaterial::Fabric ? 16
        : (material == ParticleMaterial::Metal || material == ParticleMaterial::Glass ? 14 : 12));
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < fragmentCount; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * (static_cast<float>(i) / std::max(1, fragmentCount) + Hash01(seed + 1) * 0.12f);
        Vector3 outward {
            std::cos(angle) + bias.x * 0.28f,
            0.28f + Hash01(seed + 2) * 0.90f + bias.y * 0.18f,
            std::sin(angle) + bias.z * 0.28f,
        };
        outward = SafeNormalize(outward, Vector3 { 0.0f, 1.0f, 0.0f });
        float speed = 1.25f + Hash01(seed + 3) * 2.15f;
        float lifetime = 0.30f + Hash01(seed + 4) * 0.28f;
        float size = 0.048f + Hash01(seed + 5) * 0.055f;
        float drag = 2.8f;
        float gravity = 6.2f;
        if (material == ParticleMaterial::Earth)
        {
            speed *= 0.68f; gravity = 0.55f; drag = 3.7f; size *= 1.28f;
        }
        else if (material == ParticleMaterial::Fabric)
        {
            speed *= 0.72f; lifetime += 0.24f; gravity = 0.85f; drag = 2.4f; size *= 0.62f;
        }
        else if (material == ParticleMaterial::Wood)
        {
            gravity = 7.2f; drag = 2.1f; size *= 0.82f;
        }
        else if (material == ParticleMaterial::Metal)
        {
            speed *= 1.42f; gravity = 4.8f; drag = 1.8f; size *= 0.48f;
        }
        else if (material == ParticleMaterial::Glass)
        {
            speed *= 1.25f; gravity = 5.6f; drag = 1.6f; size *= 0.72f;
        }
        else if (material == ParticleMaterial::Energy)
        {
            speed *= 1.18f; gravity = -0.15f; drag = 4.2f; size *= 0.65f;
        }
        Spawn(
            fragmentKind,
            ParticlePriority::Critical,
            Vector3 {
                position.x + (Hash01(seed + 6) - 0.5f) * 0.34f,
                position.y + (Hash01(seed + 7) - 0.5f) * 0.34f,
                position.z + (Hash01(seed + 8) - 0.5f) * 0.34f,
            },
            Vector3Scale(outward, speed),
            ScaleColor(color, 0.72f + Hash01(seed + 9) * 0.42f),
            lifetime,
            size,
            std::max(0.008f, size * 0.28f),
            drag,
            gravity);
    }

    if (material == ParticleMaterial::Stone || material == ParticleMaterial::Wood
        || material == ParticleMaterial::Earth)
    {
        const int dustCount = QualityCount(material == ParticleMaterial::Earth ? 9 : 6);
        for (int i = 0; i < dustCount; ++i)
        {
            const std::uint32_t seed = emissionSeed + 0x85ebca6bu + static_cast<std::uint32_t>(i) * 668265263u;
            const float angle = kTau * Hash01(seed + 1);
            const Vector3 radial { std::cos(angle), 0.10f + Hash01(seed + 2) * 0.25f, std::sin(angle) };
            Spawn(
                material == ParticleMaterial::Earth ? ParticleKind::GroundDust : ParticleKind::Dust,
                ParticlePriority::Gameplay,
                position,
                Vector3Scale(radial, 0.65f + Hash01(seed + 3) * 0.75f),
                ScaleColor(color, 0.68f),
                0.34f + Hash01(seed + 4) * 0.24f,
                0.060f,
                0.15f,
                3.5f,
                0.3f);
        }
    }
}

void ParticleSystem::EmitBlockPlace(
    Vector3 position,
    Vector3 direction,
    Color color,
    ParticleMaterial material)
{
    const Vector3 facing = SafeNormalize(Vector3 { direction.x, 0.0f, direction.z }, Vector3 { 0.0f, 0.0f, 1.0f });
    const Vector3 side = Perpendicular(facing);
    const int traceCount = QualityCount(material == ParticleMaterial::Fabric ? 12 : 10);
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < traceCount; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * (static_cast<float>(i) / std::max(1, traceCount));
        const float height = -0.38f + Hash01(seed + 1) * 0.76f;
        const float radius = 0.48f + Hash01(seed + 2) * 0.14f;
        const Vector3 start = Vector3Add(
            position,
            Vector3Add(
                Vector3Scale(side, std::cos(angle) * radius),
                Vector3 { facing.x * std::sin(angle) * radius, height, facing.z * std::sin(angle) * radius }));
        const Vector3 target {
            position.x + (Hash01(seed + 3) - 0.5f) * 0.10f,
            position.y + (Hash01(seed + 4) - 0.5f) * 0.16f,
            position.z + (Hash01(seed + 5) - 0.5f) * 0.10f,
        };
        const Vector3 inward = SafeNormalize(Vector3Subtract(target, start), facing);
        Spawn(
            ParticleKind::BuildTrace,
            ParticlePriority::Critical,
            start,
            Vector3Scale(inward, 1.35f + Hash01(seed + 6) * 0.65f),
            ScaleColor(color, 0.90f + Hash01(seed + 7) * 0.34f),
            0.20f + Hash01(seed + 8) * 0.10f,
            material == ParticleMaterial::Metal || material == ParticleMaterial::Glass ? 0.026f : 0.036f,
            0.008f,
            1.0f,
            0.0f,
            target,
            true);
    }

    // A few restrained material marks make placement readable without looking
    // like destruction played backwards.
    const ParticleKind signatureKind = FragmentKind(material);
    const int signatureCount = QualityCount(material == ParticleMaterial::Energy ? 5 : 3);
    for (int i = 0; i < signatureCount; ++i)
    {
        const std::uint32_t seed = emissionSeed + 0xc2b2ae35u + static_cast<std::uint32_t>(i) * 668265263u;
        const float angle = kTau * Hash01(seed + 1);
        const Vector3 velocity {
            std::cos(angle) * (0.22f + Hash01(seed + 2) * 0.25f),
            0.18f + Hash01(seed + 3) * 0.34f,
            std::sin(angle) * (0.22f + Hash01(seed + 4) * 0.25f),
        };
        Spawn(
            signatureKind,
            ParticlePriority::Gameplay,
            Vector3 { position.x, position.y - 0.35f, position.z },
            velocity,
            ScaleColor(color, 0.82f + Hash01(seed + 5) * 0.32f),
            material == ParticleMaterial::Fabric ? 0.42f : 0.26f,
            material == ParticleMaterial::Earth ? 0.060f : 0.035f,
            0.010f,
            3.2f,
            material == ParticleMaterial::Energy ? 0.0f : 2.6f);
    }
}

void ParticleSystem::EmitAbility(
    Vector3 position,
    Vector3 direction,
    Color color,
    float radius,
    AbilityParticleStyle style)
{
    const Vector3 forward = SafeNormalize(Vector3 { direction.x, 0.0f, direction.z }, Vector3 { 0.0f, 0.0f, 1.0f });
    const Vector3 side = Perpendicular(forward);
    const float visualRadius = std::clamp(radius, 0.28f, 6.0f);
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;

    if (style == AbilityParticleStyle::Pull)
    {
        const int count = QualityCount(18);
        for (int i = 0; i < count; ++i)
        {
            const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
            const float angle = (Hash01(seed + 1) - 0.5f) * 1.45f;
            const float distance = visualRadius * (0.42f + Hash01(seed + 2) * 0.58f);
            const Vector3 ray = SafeNormalize(Vector3Add(
                Vector3Scale(forward, std::cos(angle)),
                Vector3Scale(side, std::sin(angle))), forward);
            const Vector3 start {
                position.x + ray.x * distance,
                position.y + 0.08f + Hash01(seed + 3) * 0.75f,
                position.z + ray.z * distance,
            };
            Spawn(ParticleKind::PickupStreak, ParticlePriority::Critical, start,
                Vector3Scale(ray, -1.2f), ScaleColor(color, 0.90f + Hash01(seed + 4) * 0.32f),
                0.30f + Hash01(seed + 5) * 0.16f, 0.040f, 0.008f, 0.65f, 0.0f,
                Vector3 { position.x, position.y + 0.45f, position.z }, true);
        }
        return;
    }

    if (style == AbilityParticleStyle::Cone || style == AbilityParticleStyle::Trail)
    {
        const int count = QualityCount(style == AbilityParticleStyle::Cone ? 18 : 13);
        for (int i = 0; i < count; ++i)
        {
            const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
            const float spread = style == AbilityParticleStyle::Cone ? 0.95f : 0.34f;
            Vector3 ray = Vector3Add(forward,
                Vector3Add(Vector3Scale(side, (Hash01(seed + 1) - 0.5f) * spread),
                    Vector3 { 0.0f, (Hash01(seed + 2) - 0.35f) * spread * 0.45f, 0.0f }));
            ray = SafeNormalize(ray, forward);
            const Vector3 start = style == AbilityParticleStyle::Trail
                ? Vector3Subtract(position, Vector3Scale(forward, Hash01(seed + 3) * std::min(visualRadius, 1.4f)))
                : position;
            Spawn(ParticleKind::Trail, ParticlePriority::Critical, start,
                Vector3Scale(ray, 2.8f + Hash01(seed + 4) * (style == AbilityParticleStyle::Cone ? 4.2f : 2.2f)),
                ScaleColor(color, 0.85f + Hash01(seed + 5) * 0.38f),
                0.18f + Hash01(seed + 6) * 0.18f, 0.045f, 0.008f, 2.2f, 0.0f);
        }
        return;
    }

    const bool ring = style == AbilityParticleStyle::Ring || style == AbilityParticleStyle::CorePulse;
    if (ring)
    {
        const int count = QualityCount(style == AbilityParticleStyle::CorePulse ? 24 : 18);
        for (int i = 0; i < count; ++i)
        {
            const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
            const float angle = kTau * (static_cast<float>(i) / std::max(1, count));
            const Vector3 radial { std::cos(angle), 0.0f, std::sin(angle) };
            const Vector3 tangent { -radial.z, 0.0f, radial.x };
            const Vector3 start {
                position.x + radial.x * visualRadius,
                position.y + 0.05f + (style == AbilityParticleStyle::CorePulse ? Hash01(seed + 1) * 0.42f : 0.0f),
                position.z + radial.z * visualRadius,
            };
            Spawn(i % 3 == 0 ? ParticleKind::PickupMote : ParticleKind::Trail,
                ParticlePriority::Critical, start,
                Vector3Add(Vector3Scale(tangent, 0.85f + Hash01(seed + 2) * 0.55f),
                    Vector3 { radial.x * 0.42f, 0.18f + Hash01(seed + 3) * 0.36f, radial.z * 0.42f }),
                ScaleColor(color, 0.88f + Hash01(seed + 4) * 0.34f),
                0.32f + Hash01(seed + 5) * 0.20f, 0.040f, 0.009f, 2.8f, 0.25f);
        }
        return;
    }

    const int count = QualityCount(style == AbilityParticleStyle::Sacrifice ? 28 : 16);
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * Hash01(seed + 1);
        const Vector3 radial { std::cos(angle), 0.0f, std::sin(angle) };
        const float upward = style == AbilityParticleStyle::Sacrifice
            ? 2.8f + Hash01(seed + 2) * 4.6f
            : 0.55f + Hash01(seed + 2) * 1.8f;
        const float outward = (0.9f + Hash01(seed + 3) * 2.7f)
            * (style == AbilityParticleStyle::Sacrifice ? 0.72f : 1.0f);
        Spawn(i % 4 == 0 ? ParticleKind::PickupMote : ParticleKind::ImpactStreak,
            ParticlePriority::Critical,
            Vector3 { position.x + radial.x * Hash01(seed + 4) * 0.22f, position.y, position.z + radial.z * Hash01(seed + 5) * 0.22f },
            Vector3 { radial.x * outward, upward, radial.z * outward },
            ScaleColor(color, 0.82f + Hash01(seed + 6) * 0.44f),
            0.30f + Hash01(seed + 7) * (style == AbilityParticleStyle::Sacrifice ? 0.48f : 0.22f),
            style == AbilityParticleStyle::Sacrifice ? 0.065f : 0.045f, 0.009f,
            2.4f, style == AbilityParticleStyle::Sacrifice ? 1.2f : 4.0f);
    }
}

void ParticleSystem::EmitHeal(Vector3 position, Color color, float intensity)
{
    intensity = std::clamp(intensity, 0.35f, 1.6f);
    const int count = QualityCount(12 + static_cast<int>(intensity * 6.0f));
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * Hash01(seed + 1);
        const float radius = 0.18f + Hash01(seed + 2) * 0.48f;
        const Vector3 start { position.x + std::cos(angle) * radius, position.y + Hash01(seed + 3) * 0.45f, position.z + std::sin(angle) * radius };
        Spawn(i % 3 == 0 ? ParticleKind::PickupMote : ParticleKind::PickupStreak,
            ParticlePriority::Critical, start,
            Vector3 { -std::sin(angle) * 0.34f, (1.15f + Hash01(seed + 4) * 1.35f) * intensity, std::cos(angle) * 0.34f },
            ScaleColor(color, 0.92f + Hash01(seed + 5) * 0.32f),
            0.38f + Hash01(seed + 6) * 0.30f, 0.044f, 0.012f, 1.4f, -0.18f);
    }
}

void ParticleSystem::EmitTrap(Vector3 position, Color color, float radius, bool triggered)
{
    const int count = QualityCount(triggered ? 24 : 12);
    const float visualRadius = std::clamp(radius, 0.35f, 5.5f);
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * (static_cast<float>(i) / std::max(1, count));
        const Vector3 radial { std::cos(angle), 0.0f, std::sin(angle) };
        const Vector3 start = triggered
            ? position
            : Vector3 { position.x + radial.x * visualRadius, position.y + 0.035f, position.z + radial.z * visualRadius };
        const Vector3 velocity = triggered
            ? Vector3 { radial.x * (2.0f + Hash01(seed + 1) * 2.8f), 0.12f + Hash01(seed + 2) * 0.35f, radial.z * (2.0f + Hash01(seed + 1) * 2.8f) }
            : Vector3 { -radial.x * 1.4f, 0.05f, -radial.z * 1.4f };
        Spawn(triggered ? ParticleKind::ImpactStreak : ParticleKind::BuildTrace,
            ParticlePriority::Critical, start, velocity, ScaleColor(color, 0.88f + Hash01(seed + 3) * 0.35f),
            triggered ? 0.26f : 0.34f, triggered ? 0.050f : 0.035f, 0.008f,
            triggered ? 3.8f : 1.0f, triggered ? 2.0f : 0.0f,
            position, !triggered);
    }
}

void ParticleSystem::EmitDevice(Vector3 position, Color color, float intensity, bool assembling)
{
    intensity = std::clamp(intensity, 0.2f, 1.5f);
    const int count = QualityCount(assembling ? 14 : std::max(2, static_cast<int>(5.0f * intensity)));
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * Hash01(seed + 1);
        const Vector3 radial { std::cos(angle), 0.0f, std::sin(angle) };
        const Vector3 start = assembling
            ? Vector3 { position.x + radial.x * (0.45f + Hash01(seed + 2) * 0.42f), position.y + (Hash01(seed + 3) - 0.5f) * 0.70f, position.z + radial.z * (0.45f + Hash01(seed + 2) * 0.42f) }
            : position;
        Spawn(assembling ? ParticleKind::BuildTrace : ParticleKind::Spark,
            ParticlePriority::Gameplay, start,
            assembling ? Vector3Scale(radial, -1.5f) : Vector3 { radial.x * 0.45f, 0.35f + Hash01(seed + 4) * 0.75f, radial.z * 0.45f },
            ScaleColor(color, 0.88f + Hash01(seed + 5) * 0.40f),
            assembling ? 0.30f : 0.22f, assembling ? 0.035f : 0.024f, 0.007f,
            assembling ? 1.0f : 3.8f, assembling ? 0.0f : 2.8f,
            position, assembling);
    }
}

void ParticleSystem::EmitCoreDestruction(Vector3 position, Color color, float intensity)
{
    intensity = std::clamp(intensity, 0.5f, 1.8f);
    const int count = QualityCount(30 + static_cast<int>(intensity * 12.0f));
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * Hash01(seed + 1);
        const Vector3 radial { std::cos(angle), 0.0f, std::sin(angle) };
        const bool shard = (i % 4) != 0;
        Spawn(shard ? ParticleKind::Shard : ParticleKind::Smoke, ParticlePriority::Critical,
            Vector3 { position.x + (Hash01(seed + 2) - 0.5f) * 0.55f, position.y + (Hash01(seed + 3) - 0.5f) * 0.55f, position.z + (Hash01(seed + 4) - 0.5f) * 0.55f },
            Vector3 { radial.x * (1.8f + Hash01(seed + 5) * 4.2f) * intensity,
                (0.8f + Hash01(seed + 6) * 4.8f) * intensity,
                radial.z * (1.8f + Hash01(seed + 7) * 4.2f) * intensity },
            shard ? ScaleColor(color, 0.82f + Hash01(seed + 8) * 0.45f) : ScaleColor(color, 0.34f),
            shard ? 0.42f + Hash01(seed + 9) * 0.34f : 0.72f + Hash01(seed + 9) * 0.45f,
            shard ? 0.070f : 0.15f, shard ? 0.012f : 0.28f,
            shard ? 1.8f : 1.3f, shard ? 6.2f : -0.35f);
    }
}

void ParticleSystem::EmitRespawn(Vector3 position, Color color)
{
    const int count = QualityCount(20);
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * (static_cast<float>(i) / std::max(1, count));
        const float radius = 0.52f + Hash01(seed + 1) * 0.24f;
        Spawn(i % 4 == 0 ? ParticleKind::PickupMote : ParticleKind::PickupStreak,
            ParticlePriority::Critical,
            Vector3 { position.x + std::cos(angle) * radius, position.y + Hash01(seed + 2) * 0.20f, position.z + std::sin(angle) * radius },
            Vector3 { -std::sin(angle) * 0.65f, 1.8f + Hash01(seed + 3) * 2.2f, std::cos(angle) * 0.65f },
            ScaleColor(color, 0.88f + Hash01(seed + 4) * 0.38f),
            0.42f + Hash01(seed + 5) * 0.28f, 0.045f, 0.010f, 1.1f, -0.15f);
    }
}

void ParticleSystem::EmitProjectileCue(Vector3 position, Vector3 direction, Color color, float intensity)
{
    intensity = std::clamp(intensity, 0.35f, 1.5f);
    const Vector3 forward = SafeNormalize(direction, Vector3 { 0.0f, 0.0f, 1.0f });
    const int count = QualityCount(4);
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        Spawn(ParticleKind::Trail, ParticlePriority::Gameplay,
            Vector3 { position.x + (Hash01(seed + 1) - 0.5f) * 0.10f,
                position.y + (Hash01(seed + 2) - 0.5f) * 0.10f,
                position.z + (Hash01(seed + 3) - 0.5f) * 0.10f },
            Vector3Scale(forward, -(0.35f + Hash01(seed + 4) * 0.75f) * intensity),
            ScaleColor(color, 0.90f + Hash01(seed + 5) * 0.30f),
            0.12f + Hash01(seed + 6) * 0.09f, 0.030f * intensity, 0.006f,
            4.8f, 0.0f);
    }
}

void ParticleSystem::EmitHazard(Vector3 position, Color color, float radius)
{
    const int count = QualityCount(10);
    const float visualRadius = std::clamp(radius * 0.72f, 0.35f, 3.2f);
    const std::uint32_t emissionSeed = emissionSerial_++ * 2246822519u;
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t seed = emissionSeed + static_cast<std::uint32_t>(i) * 3266489917u;
        const float angle = kTau * (static_cast<float>(i) / std::max(1, count) + Hash01(seed + 1) * 0.08f);
        Spawn(i % 3 == 0 ? ParticleKind::Smoke : ParticleKind::Spark, ParticlePriority::Gameplay,
            Vector3 { position.x + std::cos(angle) * visualRadius, position.y + 0.05f, position.z + std::sin(angle) * visualRadius },
            Vector3 { (Hash01(seed + 2) - 0.5f) * 0.35f, 0.45f + Hash01(seed + 3) * 1.15f, (Hash01(seed + 4) - 0.5f) * 0.35f },
            ScaleColor(color, 0.82f + Hash01(seed + 5) * 0.38f),
            0.42f + Hash01(seed + 6) * 0.38f, i % 3 == 0 ? 0.11f : 0.025f,
            i % 3 == 0 ? 0.22f : 0.006f, 1.8f, -0.18f);
    }
}

void ParticleSystem::Draw(bool emissiveOnly) const
{
    for (const Particle& particle : particles_)
    {
        if (!particle.active)
        {
            continue;
        }
        const bool emissive = particle.kind == ParticleKind::Spark
            || particle.kind == ParticleKind::Trail
            || particle.kind == ParticleKind::PickupStreak
            || particle.kind == ParticleKind::PickupMote
            || particle.kind == ParticleKind::ImpactStreak
            || particle.kind == ParticleKind::BuildTrace;
        if (emissive != emissiveOnly)
        {
            continue;
        }
        const float progress = std::clamp(
            particle.age / std::max(0.001f, particle.lifetime), 0.0f, 1.0f);
        const float remaining = 1.0f - progress;
        const float size = particle.size + (particle.endSize - particle.size) * progress;
        const float alpha = particle.kind == ParticleKind::Smoke
            ? remaining * 0.42f
            : (particle.kind == ParticleKind::GroundDust
                ? remaining * 0.38f
                : (particle.kind == ParticleKind::PickupMote
                ? std::sin(progress * 3.14159265f) * 0.95f
                : (particle.kind == ParticleKind::Fiber
                ? remaining * 0.62f
                : remaining * 0.88f)));
        const Color tint = Fade(particle.color, alpha);

        switch (particle.kind)
        {
        case ParticleKind::Debris:
            DrawCube(particle.position, size, size, size, tint);
            break;
        case ParticleKind::Dust:
        case ParticleKind::GroundDust:
            DrawCylinder(
                particle.position,
                size * 0.92f,
                size * 0.58f,
                std::max(0.012f, size * 0.16f),
                6,
                tint);
            break;
        case ParticleKind::Smoke:
            DrawCube(particle.position, size * 1.55f, size * 1.18f, size * 1.55f, tint);
            break;
        case ParticleKind::PickupMote:
        {
            const Vector3 x { size, 0.0f, 0.0f };
            const Vector3 y { 0.0f, size, 0.0f };
            const Vector3 z { 0.0f, 0.0f, size };
            DrawLine3D(Vector3Subtract(particle.position, x), Vector3Add(particle.position, x), tint);
            DrawLine3D(Vector3Subtract(particle.position, y), Vector3Add(particle.position, y), tint);
            DrawLine3D(Vector3Subtract(particle.position, z), Vector3Add(particle.position, z), tint);
            break;
        }
        case ParticleKind::Fiber:
        {
            const Vector3 axis = SafeNormalize(particle.velocity, Vector3 { 0.0f, 1.0f, 0.0f });
            const Vector3 side = Vector3Scale(Perpendicular(axis), size * 0.24f);
            const Vector3 tail = Vector3Subtract(particle.position, Vector3Scale(axis, size * 1.8f));
            DrawLine3D(Vector3Subtract(tail, side), Vector3Add(particle.position, side), tint);
            DrawLine3D(Vector3Add(tail, side), Vector3Subtract(particle.position, side), Fade(tint, 0.56f));
            break;
        }
        case ParticleKind::Splinter:
        {
            const Vector3 axis = SafeNormalize(particle.velocity, Vector3 { 0.0f, 1.0f, 0.0f });
            const Vector3 tail = Vector3Subtract(particle.position, Vector3Scale(axis, size * 2.4f));
            DrawCylinderEx(tail, particle.position, size * 0.18f, size * 0.34f, 4, tint);
            break;
        }
        case ParticleKind::Shard:
        {
            const Vector3 axis = SafeNormalize(particle.velocity, Vector3 { 0.0f, 1.0f, 0.0f });
            const Vector3 side = Vector3Scale(Perpendicular(axis), size * 0.62f);
            const Vector3 tip = Vector3Add(particle.position, Vector3Scale(axis, size * 1.45f));
            const Vector3 base = Vector3Subtract(particle.position, Vector3Scale(axis, size * 0.90f));
            DrawTriangle3D(tip, Vector3Add(base, side), Vector3Subtract(base, side), tint);
            DrawTriangle3D(tip, Vector3Subtract(base, side), Vector3Add(base, side), tint);
            break;
        }
        case ParticleKind::Spark:
        case ParticleKind::Trail:
        case ParticleKind::PickupStreak:
        case ParticleKind::ImpactStreak:
        case ParticleKind::BuildTrace:
        {
            const float trailScale = particle.kind == ParticleKind::PickupStreak ? 0.065f
                : (particle.kind == ParticleKind::ImpactStreak ? 0.085f : 0.045f);
            Vector3 tail = Vector3Subtract(particle.position, Vector3Scale(particle.velocity, trailScale));
            if (particle.kind == ParticleKind::BuildTrace
                && Vector3DistanceSqr(particle.previousPosition, particle.position) > 0.00001f)
            {
                tail = particle.previousPosition;
            }
            if (particle.kind == ParticleKind::PickupStreak
                || particle.kind == ParticleKind::ImpactStreak
                || particle.kind == ParticleKind::BuildTrace)
            {
                DrawCylinderEx(tail, particle.position,
                    std::max(0.004f, size * (particle.kind == ParticleKind::ImpactStreak ? 0.34f : 0.24f)),
                    std::max(0.002f, size * 0.08f),
                    5,
                    tint);
            }
            else
            {
                DrawLine3D(tail, particle.position, tint);
            }
            if (particle.kind == ParticleKind::Spark)
            {
                DrawCube(particle.position, size, size, size, tint);
            }
            break;
        }
        }
    }
}

void ParticleSystem::AppendGlowLights(std::vector<ParticleGlowLight>& lights) const
{
    struct Accumulator
    {
        Vector3 positionSum {};
        int red = 0;
        int green = 0;
        int blue = 0;
        int count = 0;
        int family = -1;
    };
    std::array<Accumulator, 6> groups {};
    for (const Particle& particle : particles_)
    {
        if (!particle.active)
        {
            continue;
        }
        const bool emissive = particle.kind == ParticleKind::Spark
            || particle.kind == ParticleKind::Trail
            || particle.kind == ParticleKind::PickupStreak
            || particle.kind == ParticleKind::PickupMote
            || particle.kind == ParticleKind::ImpactStreak
            || particle.kind == ParticleKind::BuildTrace;
        if (!emissive)
        {
            continue;
        }
        // Preserve separate warm fire, green healing/device and cool energy
        // pools so unlike effects do not average into a muddy white light.
        const int family = particle.color.r > particle.color.g * 1.18f
            ? 0
            : (particle.color.g > particle.color.r * 1.10f
                && particle.color.g > particle.color.b * 1.02f ? 1 : 2);
        Accumulator* selected = nullptr;
        for (Accumulator& group : groups)
        {
            if (group.count <= 0 || group.family != family)
            {
                continue;
            }
            const Vector3 center = Vector3Scale(group.positionSum, 1.0f / static_cast<float>(group.count));
            if (Vector3DistanceSqr(center, particle.position) <= 36.0f)
            {
                selected = &group;
                break;
            }
        }
        if (selected == nullptr)
        {
            for (Accumulator& group : groups)
            {
                if (group.count == 0)
                {
                    selected = &group;
                    selected->family = family;
                    break;
                }
            }
        }
        if (selected == nullptr)
        {
            continue;
        }
        Accumulator& accumulator = *selected;
        accumulator.positionSum = Vector3Add(accumulator.positionSum, particle.position);
        accumulator.red += particle.color.r;
        accumulator.green += particle.color.g;
        accumulator.blue += particle.color.b;
        ++accumulator.count;
    }
    for (const Accumulator& accumulator : groups)
    {
        if (accumulator.count == 0)
        {
            continue;
        }
        const float inverseCount = 1.0f / static_cast<float>(accumulator.count);
        lights.push_back(ParticleGlowLight {
            Vector3Scale(accumulator.positionSum, inverseCount),
            Color {
                static_cast<unsigned char>(accumulator.red / accumulator.count),
                static_cast<unsigned char>(accumulator.green / accumulator.count),
                static_cast<unsigned char>(accumulator.blue / accumulator.count),
                255 },
            std::clamp(2.8f + static_cast<float>(accumulator.count) * 0.055f, 2.8f, 6.5f),
            std::clamp(0.28f + static_cast<float>(accumulator.count) * 0.018f, 0.28f, 0.92f)
        });
    }
}

int ParticleSystem::ActiveCount() const
{
    int count = 0;
    for (const Particle& particle : particles_)
    {
        count += particle.active ? 1 : 0;
    }
    return count;
}

int ParticleSystem::ActiveCount(ParticleKind kind) const
{
    int count = 0;
    for (const Particle& particle : particles_)
    {
        count += particle.active && particle.kind == kind ? 1 : 0;
    }
    return count;
}

int RunParticleSystemSmoke()
{
#if DAIBED_DIAGNOSTICS
    ParticleSystem particles;
    particles.SetQuality(2);
    particles.EmitPickup(
        Vector3 { 0.0f, 0.0f, 0.0f },
        Vector3 { 0.0f, 0.8f, 0.0f },
        Color { 246, 196, 74, 255 },
        4);
    const int pickupCount = particles.ActiveCount(ParticleKind::PickupStreak)
        + particles.ActiveCount(ParticleKind::PickupMote);
    if (pickupCount < 8 || particles.ActiveCount(ParticleKind::GroundDust) != 0)
    {
        std::cerr << "particle-system smoke failed: pickup recipe=" << pickupCount << "\n";
        return 2;
    }

    particles.Clear();
    particles.EmitLanding(
        Vector3 { 0.0f, 0.0f, 0.0f },
        Vector3 { 4.0f, 0.0f, 1.0f },
        Color { 132, 138, 148, 255 },
        1.0f);
    const int landingDust = particles.ActiveCount(ParticleKind::GroundDust);
    const int landingChips = particles.ActiveCount(ParticleKind::Debris);
    if (landingDust < 8 || landingChips < 3)
    {
        std::cerr << "particle-system smoke failed: landing dust=" << landingDust
                  << " chips=" << landingChips << "\n";
        return 3;
    }

    particles.Clear();
    particles.EmitImpact(
        Vector3 {}, Vector3 { 0.0f, 0.1f, 1.0f }, Color { 255, 224, 122, 255 },
        ParticleMaterial::Character, 1.0f);
    const int characterImpact = particles.ActiveCount(ParticleKind::ImpactStreak);
    if (characterImpact < 6 || particles.ActiveCount(ParticleKind::Debris) != 0)
    {
        std::cerr << "particle-system smoke failed: character impact=" << characterImpact << "\n";
        return 4;
    }

    particles.Clear();
    particles.EmitBlockBreak(
        Vector3 {}, Vector3 { 0.0f, 0.2f, 1.0f }, Color { 190, 80, 80, 255 },
        ParticleMaterialFromBlock(BlockType::WoolBlock));
    const int woolFibers = particles.ActiveCount(ParticleKind::Fiber);
    particles.EmitBlockBreak(
        Vector3 { 1.0f, 0.0f, 0.0f }, Vector3 { 0.0f, 0.2f, 1.0f },
        Color { 150, 160, 172, 255 }, ParticleMaterialFromBlock(BlockType::MetalBlock));
    const int metalSparks = particles.ActiveCount(ParticleKind::Spark);
    particles.EmitBlockBreak(
        Vector3 { 2.0f, 0.0f, 0.0f }, Vector3 { 0.0f, 0.2f, 1.0f },
        Color { 112, 232, 255, 255 }, ParticleMaterialFromBlock(BlockType::EnergyGlassBlock));
    const int glassShards = particles.ActiveCount(ParticleKind::Shard);
    if (woolFibers < 12 || metalSparks < 10 || glassShards < 10)
    {
        std::cerr << "particle-system smoke failed: material break recipes="
                  << woolFibers << "/" << metalSparks << "/" << glassShards << "\n";
        return 5;
    }

    particles.Clear();
    particles.EmitBlockPlace(
        Vector3 {}, Vector3 { 0.0f, 0.0f, 1.0f }, Color { 148, 152, 160, 255 },
        ParticleMaterialFromBlock(BlockType::StoneBlock));
    const int placementTraces = particles.ActiveCount(ParticleKind::BuildTrace);
    if (placementTraces < 8 || particles.ActiveCount(ParticleKind::ImpactStreak) != 0)
    {
        std::cerr << "particle-system smoke failed: placement traces=" << placementTraces << "\n";
        return 6;
    }

    particles.Clear();
    particles.EmitAbility(Vector3 {}, Vector3 { 0.0f, 0.0f, 1.0f },
        Color { 112, 232, 255, 255 }, 2.0f, AbilityParticleStyle::Ring);
    const int abilitySignature = particles.ActiveCount(ParticleKind::Trail)
        + particles.ActiveCount(ParticleKind::PickupMote);
    particles.Clear();
    particles.EmitHeal(Vector3 {}, Color { 128, 238, 166, 255 }, 1.0f);
    const int healSignature = particles.ActiveCount(ParticleKind::PickupStreak)
        + particles.ActiveCount(ParticleKind::PickupMote);
    std::vector<ParticleGlowLight> healLights;
    particles.AppendGlowLights(healLights);
    const bool healLightOk = !healLights.empty()
        && healLights.front().color.g > healLights.front().color.r;
    particles.Clear();
    particles.EmitTrap(Vector3 {}, Color { 255, 235, 142, 255 }, 1.2f, true);
    const int trapSignature = particles.ActiveCount(ParticleKind::ImpactStreak);
    particles.Clear();
    particles.EmitDevice(Vector3 {}, Color { 128, 238, 166, 255 }, 1.0f, true);
    const int deviceSignature = particles.ActiveCount(ParticleKind::BuildTrace);
    particles.Clear();
    particles.EmitCoreDestruction(Vector3 {}, Color { 255, 118, 118, 255 }, 1.0f);
    const int coreSignature = particles.ActiveCount(ParticleKind::Shard)
        + particles.ActiveCount(ParticleKind::Smoke);
    particles.Clear();
    particles.EmitRespawn(Vector3 {}, Color { 112, 232, 255, 255 });
    const int respawnSignature = particles.ActiveCount(ParticleKind::PickupStreak)
        + particles.ActiveCount(ParticleKind::PickupMote);
    particles.Clear();
    particles.EmitProjectileCue(Vector3 {}, Vector3 { 0.0f, 0.0f, 1.0f }, WHITE, 1.0f);
    const int projectileSignature = particles.ActiveCount(ParticleKind::Trail);
    particles.Clear();
    particles.EmitHazard(Vector3 {}, Color { 255, 118, 70, 255 }, 2.0f);
    const int hazardSignature = particles.ActiveCount(ParticleKind::Smoke)
        + particles.ActiveCount(ParticleKind::Spark);
    if (abilitySignature < 12 || healSignature < 12 || !healLightOk || trapSignature < 16
        || deviceSignature < 10 || coreSignature < 30 || respawnSignature < 16
        || projectileSignature < 3 || hazardSignature < 8)
    {
        std::cerr << "particle-system smoke failed: semantic recipes="
                  << abilitySignature << "/" << healSignature << "/" << trapSignature << "/"
                  << deviceSignature << "/" << coreSignature << "/" << respawnSignature << "/"
                  << projectileSignature << "/" << hazardSignature << "\n";
        return 9;
    }

    // Low quality hard-caps the pool. Gameplay pickup feedback must still be
    // able to replace ambient dust when that budget is saturated.
    particles.Clear();
    particles.SetQuality(0);
    for (int i = 0; i < 500; ++i)
    {
        particles.Emit(
            ParticleKind::Dust,
            Vector3 { static_cast<float>(i), 0.0f, 0.0f },
            Vector3 { 0.0f, 1.0f, 0.0f },
            WHITE,
            3,
            1.0f);
    }
    const int saturated = particles.ActiveCount();
    particles.EmitPickup(Vector3 {}, Vector3 { 0.0f, 1.0f, 0.0f }, WHITE, 2);
    const int protectedPickup = particles.ActiveCount(ParticleKind::PickupStreak)
        + particles.ActiveCount(ParticleKind::PickupMote);
    if (saturated != 128 || particles.ActiveCount() != 128 || protectedPickup <= 0)
    {
        std::cerr << "particle-system smoke failed: budget=" << saturated
                  << " pickup=" << protectedPickup << "\n";
        return 7;
    }

    for (int i = 0; i < 300; ++i)
    {
        particles.Update(1.0f / 120.0f);
    }
    if (particles.ActiveCount() != 0)
    {
        std::cerr << "particle-system smoke failed: particles did not expire\n";
        return 8;
    }

    std::cout << "particle-system smoke passed: pickup=" << pickupCount
              << " landing=" << landingDust << "+" << landingChips
              << " impact=" << characterImpact
              << " materials=" << woolFibers << "/" << metalSparks << "/" << glassShards
              << " place=" << placementTraces
              << " semantic=" << abilitySignature << "/" << healSignature << "/"
              << trapSignature << "/" << deviceSignature << "/" << coreSignature << "/"
              << respawnSignature << "/" << projectileSignature << "/" << hazardSignature
              << " low-budget=" << saturated
              << " protected-pickup=" << protectedPickup << "\n";
    return 0;
#else
    std::cerr << "diagnostic smokes are disabled in this build\n";
    return 2;
#endif
}

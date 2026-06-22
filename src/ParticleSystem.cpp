#include "ParticleSystem.h"

#include "raymath.h"

#include <algorithm>
#include <cmath>

namespace
{
float Hash01(int value)
{
    unsigned int state = static_cast<unsigned int>(value) * 747796405u + 2891336453u;
    state = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    state = (state >> 22u) ^ state;
    return static_cast<float>(state & 0xffffu) / 65535.0f;
}
}

void ParticleSystem::Clear()
{
    for (Particle& particle : particles_)
    {
        particle.active = false;
    }
    nextSlot_ = 0;
}

void ParticleSystem::SetQuality(int quality)
{
    quality_ = std::clamp(quality, 0, 2);
}

void ParticleSystem::Update(float dt)
{
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
        const float drag = particle.kind == ParticleKind::Smoke ? 1.4f : 3.6f;
        particle.velocity = Vector3Scale(particle.velocity, std::max(0.0f, 1.0f - drag * dt));
        if (particle.kind == ParticleKind::Smoke)
        {
            particle.velocity.y += 0.42f * dt;
            particle.size += 0.11f * dt;
        }
        else if (particle.kind != ParticleKind::Trail)
        {
            particle.velocity.y -= 5.6f * dt;
        }
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
    const int qualityLimit = quality_ == 0 ? 128 : (quality_ == 1 ? 256 : kCapacity);
    const int qualityCount = quality_ == 0 ? std::max(1, count / 3) : (quality_ == 1 ? std::max(1, count * 2 / 3) : count);
    const Vector3 baseDirection = Vector3LengthSqr(direction) > 0.0001f
        ? Vector3Normalize(direction)
        : Vector3 { 0.0f, 1.0f, 0.0f };
    for (int i = 0; i < qualityCount; ++i)
    {
        const int slot = nextSlot_++ % qualityLimit;
        const int seed = slot * 31 + i * 131 + static_cast<int>(GetTime() * 1000.0);
        const Vector3 randomDirection {
            Hash01(seed + 1) * 2.0f - 1.0f,
            Hash01(seed + 2) * 1.3f,
            Hash01(seed + 3) * 2.0f - 1.0f
        };
        const Vector3 velocityDirection = Vector3Normalize(Vector3Add(Vector3Scale(baseDirection, 0.55f), randomDirection));
        Particle& particle = particles_[slot];
        particle.position = position;
        particle.velocity = Vector3Scale(velocityDirection, speed * (0.55f + Hash01(seed + 4) * 0.9f));
        particle.color = color;
        particle.age = 0.0f;
        particle.kind = kind;
        particle.lifetime = kind == ParticleKind::Smoke ? 1.15f + Hash01(seed + 5) * 0.55f
            : (kind == ParticleKind::Trail ? 0.20f : 0.35f + Hash01(seed + 5) * 0.35f);
        particle.size = kind == ParticleKind::Smoke ? 0.14f
            : (kind == ParticleKind::Debris ? 0.085f : 0.055f);
        particle.active = true;
    }
}

void ParticleSystem::Draw() const
{
    for (const Particle& particle : particles_)
    {
        if (!particle.active)
        {
            continue;
        }
        const float remaining = 1.0f - std::clamp(particle.age / std::max(0.001f, particle.lifetime), 0.0f, 1.0f);
        const Color tint = Fade(particle.color, remaining * (particle.kind == ParticleKind::Smoke ? 0.42f : 0.88f));
        if (particle.kind == ParticleKind::Debris)
        {
            DrawCube(particle.position, particle.size, particle.size, particle.size, tint);
        }
        else
        {
            DrawSphere(particle.position, particle.size * (particle.kind == ParticleKind::Smoke ? 1.0f + particle.age * 0.4f : 1.0f), tint);
        }
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

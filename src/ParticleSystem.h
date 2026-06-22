#pragma once

#include "raylib.h"

#include <array>

enum class ParticleKind
{
    Debris,
    Dust,
    Spark,
    Smoke,
    Trail
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
    void Draw() const;
    int ActiveCount() const;

private:
    struct Particle
    {
        Vector3 position {};
        Vector3 velocity {};
        Color color = WHITE;
        float age = 0.0f;
        float lifetime = 0.5f;
        float size = 0.08f;
        ParticleKind kind = ParticleKind::Spark;
        bool active = false;
    };

    static constexpr int kCapacity = 512;
    std::array<Particle, kCapacity> particles_ {};
    int nextSlot_ = 0;
    int quality_ = 2;
};

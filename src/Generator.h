#pragma once

#include "Resource.h"

#include <vector>

// Raylib-free: spatial state is Vec3 (see Resource.h / SimMath.h). Callers that
// need a raylib Vector3 convert at the Game/render boundary via VecConvert.h.
class Generator
{
public:
    Generator() = default;
    Generator(ResourceType type, Vec3 position, float interval, int amount, int teamId = -1);

    void Update(float dt, std::vector<ResourcePickup>& pickups, int bonusAmount);

    ResourceType GetType() const;
    Vec3 GetPosition() const;
    int GetTeamId() const;

private:
    ResourceType type_ = ResourceType::Iron;
    Vec3 position_ {};
    float interval_ = 1.0f;
    float timer_ = 0.0f;
    int amount_ = 1;
    int teamId_ = -1;
};


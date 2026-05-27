#pragma once

#include "Resource.h"
#include "raylib.h"

#include <vector>

class Generator
{
public:
    Generator() = default;
    Generator(ResourceType type, Vector3 position, float interval, int amount, int teamId = -1);

    void Update(float dt, std::vector<ResourcePickup>& pickups, int bonusAmount);

    ResourceType GetType() const;
    Vector3 GetPosition() const;
    int GetTeamId() const;

private:
    ResourceType type_ = ResourceType::Iron;
    Vector3 position_ {};
    float interval_ = 1.0f;
    float timer_ = 0.0f;
    int amount_ = 1;
    int teamId_ = -1;
};


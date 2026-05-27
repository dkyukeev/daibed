#pragma once

#include "raylib.h"

enum class ResourceType
{
    Iron = 0,
    Gold = 1,
    Crystal = 2
};

struct ResourcePickup
{
    ResourceType type = ResourceType::Iron;
    int amount = 1;
    Vector3 position {};
    float radius = 0.55f;
    float lifetime = 30.0f;
    float age = 0.0f;
    bool collected = false;
};

const char* ToString(ResourceType type);

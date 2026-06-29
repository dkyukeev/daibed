#pragma once

#include "Simulation/SimMath.h"

enum class ResourceType
{
    Iron = 0,
    Gold = 1,
    Crystal = 2
};

// Spatial state uses the raylib-free Vec3 so generators/pickups can later move
// into MatchSimulation. Conversion to raylib Vector3 happens at the
// Game/render boundary (VecConvert.h). See docs/NETWORK_PREP_PLAN.md.
struct ResourcePickup
{
    ResourceType type = ResourceType::Iron;
    int amount = 1;
    Vec3 position {};
    float radius = 0.55f;
    float lifetime = 30.0f;
    float age = 0.0f;
    bool collected = false;
};

const char* ToString(ResourceType type);

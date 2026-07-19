#include "Generator.h"

#include <algorithm>

namespace
{
float DistanceSquared(Vec3 a, Vec3 b)
{
    return (a - b).LengthSquared();
}
}

Generator::Generator(ResourceType type, Vec3 position, float interval, int amount, int teamId)
    : type_(type),
      position_(position),
      interval_(interval),
      amount_(amount),
      teamId_(teamId)
{
}

void Generator::Update(float dt, std::vector<ResourcePickup>& pickups, int forgeLevel, int bonusAmount)
{
    timer_ += dt;
    const int clampedForgeLevel = std::clamp(forgeLevel, 0, 4);
    const float upgradedInterval = interval_ * (1.0f - 0.08f * static_cast<float>(clampedForgeLevel));
    if (timer_ < upgradedInterval)
    {
        return;
    }

    timer_ = 0.0f;
    const Vec3 spawnPosition { position_.x, position_.y + 1.0f, position_.z };
    int nearbyCount = 0;
    for (const ResourcePickup& pickup : pickups)
    {
        if (!pickup.collected
            && pickup.type == type_
            && DistanceSquared(pickup.position, spawnPosition) < 5.0f)
        {
            ++nearbyCount;
        }
    }

    const int maxNearby = type_ == ResourceType::Iron ? 10 : (type_ == ResourceType::Gold ? 7 : 5);
    if (nearbyCount >= maxNearby)
    {
        return;
    }

    pickups.push_back(ResourcePickup {
        type_,
        amount_ + bonusAmount,
        spawnPosition,
        0.55f,
        30.0f,
        0.0f,
        false });

    ++successfulSpawns_;
    // Forge IV occasionally produces a crystal in addition to the normal
    // resource. A deterministic cadence keeps authoritative simulations and
    // automatches reproducible while remaining rare during normal play.
    if (clampedForgeLevel >= 4 && successfulSpawns_ % 64u == 0u)
    {
        int nearbyCrystals = 0;
        for (const ResourcePickup& pickup : pickups)
        {
            if (!pickup.collected
                && pickup.type == ResourceType::Crystal
                && DistanceSquared(pickup.position, spawnPosition) < 5.0f)
            {
                ++nearbyCrystals;
            }
        }
        if (nearbyCrystals < 5)
        {
            pickups.push_back(ResourcePickup {
                ResourceType::Crystal,
                1,
                spawnPosition,
                0.55f,
                30.0f,
                0.0f,
                false });
        }
    }
}

ResourceType Generator::GetType() const
{
    return type_;
}

Vec3 Generator::GetPosition() const
{
    return position_;
}

int Generator::GetTeamId() const
{
    return teamId_;
}

#include "Game.h"
#include "HeroSystem.h"
#include "VecConvert.h"

#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr float kItemPickupRadiusSq = 1.35f;
constexpr float kItemMagnetRadius = 2.35f;
constexpr float kItemMagnetRadiusSq = kItemMagnetRadius * kItemMagnetRadius;
constexpr float kItemMergeRadiusSq = 0.85f * 0.85f;
constexpr float kItemMergeInterval = 0.5f;
constexpr float kResourceMagnetSpeed = 5.8f;
constexpr float kDroppedItemMagnetAccel = 28.0f;
constexpr float kDroppedItemMagnetMaxSpeed = 7.0f;
constexpr int kBuildMinY = -2;
constexpr int kBuildMaxY = 64;
constexpr float kRadonBaseRadiusSq = 105.0f;
constexpr float kOrbitaMomentumSpeed = 7.2f;
constexpr int kBromVacuumCapacity = 24;
constexpr float kBromVacuumStepHeight = 1.08f;
constexpr float kBromVacuumDropHeight = 1.15f;
constexpr float kBromTurretAttackRange = 16.0f;
constexpr float kKonvoyHandcuffRadius = 6.0f;
constexpr float kKonvoyDomeVisualRadius = 6.0f;
constexpr Vector3 kPlayerCollisionHalfExtents { 0.36f, 0.95f, 0.36f };

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float Length2D(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.z * value.z);
}

float Length(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vector3 Normalize(Vector3 value)
{
    const float length = Length(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, value.y / length, value.z / length };
}

// Nominative label for the owner-private projectile-impact CombatEvent message
// (mirrors the instrumental-case strings NoteDamageCredit already uses just
// below for the kill feed's "cause" text).
const char* ProjectileImpactLabel(ProjectileKind kind, bool fireZone)
{
    switch (kind)
    {
    case ProjectileKind::Arrow:
        return "стрела";
    case ProjectileKind::Blaster:
        return "болт бластера";
    case ProjectileKind::Fireball:
        return "фаербол";
    case ProjectileKind::Molotov:
        return fireZone ? "коктейль Молотова" : "снаряд";
    }
    return "снаряд";
}

Color HeroAccentColor(HeroId id)
{
    switch (id)
    {
    case HeroId::Radon:
        return Color { 92, 164, 255, 255 };
    case HeroId::Orbita:
        return Color { 255, 96, 82, 255 };
    case HeroId::Brom:
        return Color { 96, 202, 118, 255 };
    case HeroId::Konvoy:
        return Color { 92, 210, 255, 255 };
    case HeroId::Likho:
        return Color { 104, 238, 92, 255 };
    case HeroId::Svidetel:
        return Color { 180, 104, 255, 255 };
    }
    return WHITE;
}

float PointSegmentDistanceSquared(Vector3 point, Vector3 start, Vector3 end)
{
    const Vector3 segment { end.x - start.x, end.y - start.y, end.z - start.z };
    const Vector3 offset { point.x - start.x, point.y - start.y, point.z - start.z };
    const float lengthSq = segment.x * segment.x + segment.y * segment.y + segment.z * segment.z;
    const float t = lengthSq > 0.000001f
        ? std::clamp((offset.x * segment.x + offset.y * segment.y + offset.z * segment.z) / lengthSq, 0.0f, 1.0f)
        : 0.0f;
    return DistanceSquared(point, Vector3 { start.x + segment.x * t, start.y + segment.y * t, start.z + segment.z * t });
}

long long DevicePathKey(const GridPos& pos)
{
    return (static_cast<long long>(pos.x + 2048) << 32)
        ^ (static_cast<long long>(pos.z + 2048) << 8)
        ^ static_cast<unsigned int>(pos.y + 32);
}

std::optional<GridPos> FindDeviceSupport(const World& world, Vector3 position, int maxDrop = 7)
{
    const GridPos column = world.WorldToGrid(Vector3 { position.x, position.y - 0.70f, position.z });
    for (int drop = 0; drop <= maxDrop; ++drop)
    {
        const GridPos support { column.x, column.y - drop, column.z };
        if (world.IsSolid(support)
            && world.IsAir(GridPos { support.x, support.y + 1, support.z }))
        {
            return support;
        }
    }
    return std::nullopt;
}

std::optional<Vector3> FindDeviceGroundWaypoint(const World& world, Vector3 from, Vector3 target)
{
    const std::optional<GridPos> startSupport = FindDeviceSupport(world, from, 4);
    const std::optional<GridPos> goalSupport = FindDeviceSupport(world, target, 10);
    if (!startSupport.has_value() || !goalSupport.has_value())
    {
        return std::nullopt;
    }

    const GridPos start = *startSupport;
    GridPos goal = *goalSupport;
    constexpr int pathRadius = 42;
    constexpr int maxExpansions = 1800;
    const auto canStand = [&world, &start](const GridPos& support)
    {
        if (std::abs(support.x - start.x) > pathRadius
            || std::abs(support.z - start.z) > pathRadius
            || support.y < kBuildMinY || support.y > kBuildMaxY)
        {
            return false;
        }
        return world.IsSolid(support)
            && world.IsAir(GridPos { support.x, support.y + 1, support.z });
    };
    if (!canStand(goal))
    {
        bool found = false;
        for (int radius = 1; radius <= 4 && !found; ++radius)
        {
            for (int dx = -radius; dx <= radius && !found; ++dx)
            {
                for (int dz = -radius; dz <= radius && !found; ++dz)
                {
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        const GridPos candidate { goal.x + dx, goal.y + dy, goal.z + dz };
                        if (canStand(candidate))
                        {
                            goal = candidate;
                            found = true;
                            break;
                        }
                    }
                }
            }
        }
        if (!found)
        {
            return std::nullopt;
        }
    }

    struct Node
    {
        GridPos pos {};
        float g = 0.0f;
        float f = 0.0f;
    };
    const auto compare = [](const Node& a, const Node& b) { return a.f > b.f; };
    const auto heuristic = [&goal](const GridPos& pos)
    {
        return static_cast<float>(std::abs(pos.x - goal.x) + std::abs(pos.z - goal.z))
            + static_cast<float>(std::abs(pos.y - goal.y)) * 1.6f;
    };
    std::priority_queue<Node, std::vector<Node>, decltype(compare)> open(compare);
    std::unordered_map<long long, float> cost;
    std::unordered_map<long long, GridPos> parent;
    open.push(Node { start, 0.0f, heuristic(start) });
    cost[DevicePathKey(start)] = 0.0f;
    std::optional<GridPos> reached;
    int expansions = 0;
    while (!open.empty() && expansions++ < maxExpansions)
    {
        const Node current = open.top();
        open.pop();
        if (std::abs(current.pos.x - goal.x) + std::abs(current.pos.z - goal.z) <= 1
            && std::abs(current.pos.y - goal.y) <= 1)
        {
            reached = current.pos;
            break;
        }
        const GridPos flatOffsets[] {
            GridPos { 1, 0, 0 }, GridPos { -1, 0, 0 },
            GridPos { 0, 0, 1 }, GridPos { 0, 0, -1 }
        };
        for (const GridPos& offset : flatOffsets)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                const GridPos next { current.pos.x + offset.x, current.pos.y + dy, current.pos.z + offset.z };
                if (!canStand(next))
                {
                    continue;
                }
                const float nextCost = current.g + 1.0f + static_cast<float>(std::abs(dy)) * 0.65f;
                const long long key = DevicePathKey(next);
                const auto old = cost.find(key);
                if (old != cost.end() && old->second <= nextCost)
                {
                    continue;
                }
                cost[key] = nextCost;
                parent[key] = current.pos;
                open.push(Node { next, nextCost, nextCost + heuristic(next) });
            }
        }
    }
    if (!reached.has_value())
    {
        return std::nullopt;
    }

    std::vector<GridPos> path;
    GridPos cursor = *reached;
    path.push_back(cursor);
    while (cursor != start && path.size() < static_cast<std::size_t>(maxExpansions))
    {
        const auto previous = parent.find(DevicePathKey(cursor));
        if (previous == parent.end())
        {
            return std::nullopt;
        }
        cursor = previous->second;
        path.push_back(cursor);
    }
    std::reverse(path.begin(), path.end());
    if (path.size() < 2)
    {
        return target;
    }
    const GridPos waypoint = path[std::min<std::size_t>(3, path.size() - 1)];
    const Vector3 center = world.GridToWorld(waypoint);
    return Vector3 { center.x, center.y + 0.68f, center.z };
}

Vector3 PickupTargetFor(const Player& player)
{
    const Vector3 pos = player.GetPosition();
    return Vector3 { pos.x, pos.y + 0.35f, pos.z };
}

int ResourceIndex(ResourceType type)
{
    return static_cast<int>(type);
}

int BromCargoWeight(ResourceType type)
{
    switch (type)
    {
    case ResourceType::Iron:
        return 1;
    case ResourceType::Gold:
        return 2;
    case ResourceType::Crystal:
        return 3;
    }
    return 1;
}

int BromCargoUnits(const std::array<int, 3>& cargo)
{
    return cargo[ResourceIndex(ResourceType::Iron)] * BromCargoWeight(ResourceType::Iron)
        + cargo[ResourceIndex(ResourceType::Gold)] * BromCargoWeight(ResourceType::Gold)
        + cargo[ResourceIndex(ResourceType::Crystal)] * BromCargoWeight(ResourceType::Crystal);
}

float BromUltimateChargeForResource(ResourceType type, int amount)
{
    const float value = type == ResourceType::Iron ? 0.9f : (type == ResourceType::Gold ? 2.2f : 4.0f);
    return value * static_cast<float>(std::max(0, amount));
}

Player* FindMagnetTarget(std::vector<Player>& players, Vector3 itemPosition, int ownerPlayerId, float ownerPickupDelay, float itemAge)
{
    Player* target = nullptr;
    float bestDistance = kItemMagnetRadiusSq;
    for (Player& player : players)
    {
        if (!player.IsAlive() || player.IsEliminated())
        {
            continue;
        }
        if (player.GetId() == ownerPlayerId && itemAge < ownerPickupDelay)
        {
            continue;
        }

        const float distance = DistanceSquared(PickupTargetFor(player), itemPosition);
        if (distance <= bestDistance)
        {
            bestDistance = distance;
            target = &player;
        }
    }
    return target;
}

void MergeNearbyResourcePickups(std::vector<ResourcePickup>& pickups)
{
    for (std::size_t i = 0; i < pickups.size(); ++i)
    {
        ResourcePickup& target = pickups[i];
        if (target.collected)
        {
            continue;
        }

        for (std::size_t j = i + 1; j < pickups.size(); ++j)
        {
            ResourcePickup& other = pickups[j];
            if (other.collected || other.type != target.type)
            {
                continue;
            }
            if (DistanceSquared(ToVector3(target.position), ToVector3(other.position)) > kItemMergeRadiusSq)
            {
                continue;
            }

            const int total = target.amount + other.amount;
            target.position = Vec3 {
                (target.position.x * static_cast<float>(target.amount) + other.position.x * static_cast<float>(other.amount)) / static_cast<float>(total),
                (target.position.y * static_cast<float>(target.amount) + other.position.y * static_cast<float>(other.amount)) / static_cast<float>(total),
                (target.position.z * static_cast<float>(target.amount) + other.position.z * static_cast<float>(other.amount)) / static_cast<float>(total)
            };
            target.amount = total;
            target.radius = std::max(target.radius, other.radius);
            target.lifetime = std::max(target.lifetime, other.lifetime);
            other.collected = true;
        }
    }
}

void MergeNearbyDroppedItems(std::vector<DroppedItem>& droppedItems)
{
    for (std::size_t i = 0; i < droppedItems.size(); ++i)
    {
        DroppedItem& target = droppedItems[i];
        if (target.collected || target.stack.IsEmpty() || target.age < target.ownerPickupDelay)
        {
            continue;
        }

        for (std::size_t j = i + 1; j < droppedItems.size(); ++j)
        {
            DroppedItem& other = droppedItems[j];
            if (other.collected || other.stack.IsEmpty() || other.stack.type != target.stack.type || other.age < other.ownerPickupDelay)
            {
                continue;
            }
            if (DistanceSquared(ToVector3(target.position), ToVector3(other.position)) > kItemMergeRadiusSq)
            {
                continue;
            }

            const int total = target.stack.count + other.stack.count;
            target.position = Vec3 {
                (target.position.x * static_cast<float>(target.stack.count) + other.position.x * static_cast<float>(other.stack.count)) / static_cast<float>(total),
                (target.position.y * static_cast<float>(target.stack.count) + other.position.y * static_cast<float>(other.stack.count)) / static_cast<float>(total),
                (target.position.z * static_cast<float>(target.stack.count) + other.position.z * static_cast<float>(other.stack.count)) / static_cast<float>(total)
            };
            target.velocity = Vec3 {
                (target.velocity.x + other.velocity.x) * 0.35f,
                std::max(target.velocity.y, other.velocity.y) * 0.25f,
                (target.velocity.z + other.velocity.z) * 0.35f
            };
            target.stack.count = total;
            target.lifetime = std::max(target.lifetime, other.lifetime);
            target.ownerPlayerId = -1;
            target.ownerPickupDelay = 0.0f;
            other.collected = true;
        }
    }
}

}

void Game::UpdateGenerators(float dt)
{
    // Generators are owned by matchSimulation_. The forge bonus depends on
    // Game's Team data, so it is supplied as a callback at the boundary.
    matchSimulation_.UpdateGenerators(dt, matchSimulation_.Pickups(), [this](int teamId) { return GetForgeBonusForTeam(teamId); });
}

void Game::UpdatePickups(float dt)
{
    std::vector<ResourcePickup>& pickups = matchSimulation_.Pickups();
    for (ResourcePickup& pickup : pickups)
    {
        if (pickup.collected)
        {
            continue;
        }

        pickup.age += dt;
        pickup.lifetime -= dt;
        if (pickup.lifetime <= 0.0f)
        {
            pickup.collected = true;
            continue;
        }

        if (Player* magnetTarget = FindMagnetTarget(players_, ToVector3(pickup.position), -1, 0.0f, pickup.age))
        {
            const Vector3 target = PickupTargetFor(*magnetTarget);
            const Vector3 toTarget {
                target.x - pickup.position.x,
                target.y - pickup.position.y,
                target.z - pickup.position.z
            };
            const float distance = Length(toTarget);
            if (distance > 0.0001f)
            {
                const Vector3 direction = Normalize(toTarget);
                const float pull = kResourceMagnetSpeed * dt * (1.0f + (1.0f - std::min(distance / kItemMagnetRadius, 1.0f)) * 1.35f);
                const float step = std::min(distance, pull);
                pickup.position.x += direction.x * step;
                pickup.position.y += direction.y * step;
                pickup.position.z += direction.z * step;
            }
        }

        for (std::size_t playerOffset = 0; playerOffset < players_.size(); ++playerOffset)
        {
            Player& player = players_[(simulationOrderOffset_ + playerOffset) % players_.size()];
            if (!player.IsAlive())
            {
                continue;
            }

            if (DistanceSquared(PickupTargetFor(player), ToVector3(pickup.position)) <= kItemPickupRadiusSq)
            {
                player.GetInventory().AddResource(pickup.type, pickup.amount);
                if (player.GetHeroId() == HeroId::Brom)
                {
                    player.AddHeroUltimateCharge(BromUltimateChargeForResource(pickup.type, pickup.amount));
                }
                pickup.collected = true;
                const bool localCamera = HasLocalCamera(ControlKindForPlayer(player));
                AddWorldEffect(ToVector3(pickup.position), localCamera ? Color { 255, 245, 170, 255 } : Color { 180, 210, 255, 255 }, 0.22f, 0.28f);
                AddFloatingText("+" + std::to_string(pickup.amount) + " " + ToString(pickup.type), ToVector3(pickup.position), localCamera ? Color { 255, 236, 135, 255 } : Fade(WHITE, 0.85f));
                if (IsLocallyPredicted(ControlKindForPlayer(player)))
                {
                    ++stats_.resourcesPicked;
                    SetMessage("Подобрано: " + std::to_string(pickup.amount) + " " + ToString(pickup.type) + ".");
                    audio_.PlayPickup();
                }
                PushWorldEventSnapshot(
                    WorldEventKind::ResourcePickup,
                    player.GetId(),
                    -1,
                    player.GetTeamId(),
                    ToVector3(pickup.position),
                    static_cast<int>(pickup.type),
                    pickup.amount);
                break;
            }
        }
    }

    pickupMergeTimer_ += dt;
    if (pickupMergeTimer_ >= kItemMergeInterval)
    {
        pickupMergeTimer_ = 0.0f;
        MergeNearbyResourcePickups(pickups);
    }

    pickups.erase(
        std::remove_if(
            pickups.begin(),
            pickups.end(),
            [](const ResourcePickup& pickup)
            {
                return pickup.collected;
            }),
        pickups.end());
}

void Game::UpdateDroppedItems(float dt)
{
    std::vector<DroppedItem>& droppedItems = matchSimulation_.DroppedItems();
    for (DroppedItem& dropped : droppedItems)
    {
        if (dropped.collected)
        {
            continue;
        }

        dropped.age += dt;
        dropped.lifetime -= dt;
        dropped.velocity.y -= 9.0f * dt;
        dropped.position.x += dropped.velocity.x * dt;
        dropped.position.y += dropped.velocity.y * dt;
        dropped.position.z += dropped.velocity.z * dt;
        const GridPos under = world_.WorldToGrid(Vector3 { dropped.position.x, dropped.position.y - 0.22f, dropped.position.z });
        if (!world_.IsAir(under) && dropped.velocity.y < 0.0f)
        {
            dropped.position.y = world_.GridToWorld(under).y + 0.72f;
            dropped.velocity = Vec3 { dropped.velocity.x * 0.72f, 0.0f, dropped.velocity.z * 0.72f };
        }

        if (Player* magnetTarget = FindMagnetTarget(players_, ToVector3(dropped.position), dropped.ownerPlayerId, dropped.ownerPickupDelay, dropped.age))
        {
            const Vector3 target = PickupTargetFor(*magnetTarget);
            const Vector3 toTarget {
                target.x - dropped.position.x,
                target.y - dropped.position.y,
                target.z - dropped.position.z
            };
            const float distance = Length(toTarget);
            if (distance > 0.0001f)
            {
                const Vector3 direction = Normalize(toTarget);
                const float closeness = 1.0f - std::min(distance / kItemMagnetRadius, 1.0f);
                const float accel = kDroppedItemMagnetAccel * (0.55f + closeness * 1.15f);
                dropped.velocity.x += direction.x * accel * dt;
                dropped.velocity.y += direction.y * accel * dt;
                dropped.velocity.z += direction.z * accel * dt;

                const float speed = Length(ToVector3(dropped.velocity));
                if (speed > kDroppedItemMagnetMaxSpeed)
                {
                    const Vector3 capped = Normalize(ToVector3(dropped.velocity));
                    dropped.velocity = Vec3 {
                        capped.x * kDroppedItemMagnetMaxSpeed,
                        capped.y * kDroppedItemMagnetMaxSpeed,
                        capped.z * kDroppedItemMagnetMaxSpeed
                    };
                }
            }
        }

        for (Player& player : players_)
        {
            if (!player.IsAlive() || player.IsEliminated())
            {
                continue;
            }
            if (player.GetId() == dropped.ownerPlayerId && dropped.age < dropped.ownerPickupDelay)
            {
                continue;
            }

            if (DistanceSquared(PickupTargetFor(player), ToVector3(dropped.position)) <= kItemPickupRadiusSq)
            {
                const std::optional<ResourceType> resource = ItemToResource(dropped.stack.type);
                const bool added = resource.has_value()
                    ? (player.GetInventory().AddResource(*resource, dropped.stack.count), true)
                    : player.GetInventory().AddItem(dropped.stack.type, dropped.stack.count);
                if (added)
                {
                    if (resource.has_value() && player.GetHeroId() == HeroId::Brom)
                    {
                        player.AddHeroUltimateCharge(BromUltimateChargeForResource(*resource, dropped.stack.count));
                    }
                    dropped.collected = true;
                    AddWorldEffect(ToVector3(dropped.position), Color { 255, 245, 170, 255 }, 0.18f, 0.22f);
                    if (HasLocalCamera(ControlKindForPlayer(player)))
                    {
                        SetMessage(std::string("Подобрано: ") + ItemDisplayName(dropped.stack.type) + ".");
                        audio_.PlayPickup();
                    }
                    PushWorldEventSnapshot(
                        WorldEventKind::ItemPickup,
                        player.GetId(),
                        -1,
                        player.GetTeamId(),
                        ToVector3(dropped.position),
                        static_cast<int>(dropped.stack.type),
                        dropped.stack.count);
                    break;
                }
            }
        }
    }

    droppedItemMergeTimer_ += dt;
    if (droppedItemMergeTimer_ >= kItemMergeInterval)
    {
        droppedItemMergeTimer_ = 0.0f;
        MergeNearbyDroppedItems(droppedItems);
    }

    droppedItems.erase(
        std::remove_if(
            droppedItems.begin(),
            droppedItems.end(),
            [](const DroppedItem& dropped)
            {
                return dropped.collected || dropped.lifetime <= 0.0f;
            }),
        droppedItems.end());
}

void Game::UpdateBlockHazards(float dt)
{
    blockHazardTimer_ += dt;
    if (blockHazardTimer_ < 0.45f)
    {
        return;
    }
    blockHazardTimer_ = 0.0f;

    for (Player& player : players_)
    {
        if (!player.IsAlive())
        {
            continue;
        }

        const Vector3 pos = player.GetPosition();
        const GridPos underFeet = world_.WorldToGrid(Vector3 { pos.x, pos.y - 1.05f, pos.z });
        const Block* block = world_.GetBlock(underFeet);
        if (block == nullptr)
        {
            continue;
        }

        if (block->type == BlockType::SpikeBlock || block->type == BlockType::LavaBlock)
        {
            const int damage = block->type == BlockType::LavaBlock ? 12 : 7;
            player.Damage(damage);
            AddWorldEffect(player.GetPosition(), block->type == BlockType::LavaBlock ? Color { 255, 88, 42, 255 } : Color { 255, 118, 118, 255 }, 0.20f, 0.20f);
            AddFloatingText(block->type == BlockType::LavaBlock ? "burn" : "spike", player.GetPosition(), block->type == BlockType::LavaBlock ? Color { 255, 128, 72, 255 } : Color { 255, 118, 118, 255 });
            if (HasLocalCamera(ControlKindForPlayer(player)))
            {
                damageFlashTimer_ = std::max(damageFlashTimer_, 0.35f);
            }
        }
        else if (arenaBiome_ == ArenaBiome::Lava && pos.y < 0.25f)
        {
            const Team* team = FindTeam(player.GetTeamId());
            const bool inBaseSafeZone = team != nullptr && DistanceSquared(pos, team->spawnPoint) < 105.0f;
            if (!inBaseSafeZone)
            {
                player.Damage(5);
                AddWorldEffect(player.GetPosition(), Color { 255, 88, 42, 255 }, 0.18f, 0.18f);
                AddFloatingText("heat", player.GetPosition(), Color { 255, 128, 72, 255 });
                if (HasLocalCamera(ControlKindForPlayer(player)))
                {
                    damageFlashTimer_ = std::max(damageFlashTimer_, 0.32f);
                    SetMessage("Жар лавового биома: поднимитесь на безопасную высоту.", 1.2f);
                }
            }
        }
    }
}

void Game::UpdateExplosives(float dt)
{
    for (TimedExplosion& explosive : timedExplosions_)
    {
        explosive.timer -= dt;
        const Vector3 pos = world_.GridToWorld(explosive.block);
        if (explosive.timer > 0.0f)
        {
            AddWorldEffect(pos, explosive.timer < 0.8f ? Color { 255, 118, 70, 255 } : Color { 255, 224, 122, 255 }, 0.10f, 0.12f);
            continue;
        }

        DetonateAt(pos, explosive.ownerTeamId, explosive.ownerPlayerId, explosive.radius, 48, false);
    }

    timedExplosions_.erase(
        std::remove_if(
            timedExplosions_.begin(),
            timedExplosions_.end(),
            [](const TimedExplosion& explosive)
            {
                return explosive.timer <= 0.0f;
            }),
        timedExplosions_.end());
}

void Game::UpdateProjectiles(float dt)
{
    for (EnergyProjectile& projectile : projectiles_)
    {
        projectile.lifetime -= dt;
        const Vector3 previousPosition = projectile.position;
        projectile.previousPosition = previousPosition;
        projectile.position.x += projectile.velocity.x * dt;
        projectile.position.y += projectile.velocity.y * dt;
        projectile.position.z += projectile.velocity.z * dt;
        if (projectile.affectedByDrag)
        {
            const float drag = std::pow(projectile.airDragPerTick, dt * kTicksPerSecond);
            projectile.velocity.x *= drag;
            projectile.velocity.y *= drag;
            projectile.velocity.z *= drag;
        }
        projectile.velocity.y -= projectile.gravity * dt;

        const float impactSpeed = std::sqrt(
            projectile.velocity.x * projectile.velocity.x
            + projectile.velocity.y * projectile.velocity.y
            + projectile.velocity.z * projectile.velocity.z);
        if (projectile.speedBasedDamage)
        {
            const float rawDamage = std::ceil((impactSpeed / kTicksPerSecond) * projectile.baseDamage);
            projectile.damage = std::max(1, static_cast<int>(std::ceil(
                rawDamage * (projectile.critical ? kBowTuning.criticalMultiplier : 1.0f))));
        }

        bool consumed = projectile.lifetime <= 0.0f || projectile.position.y < -8.0f;
        const float travel = std::sqrt(DistanceSquared(previousPosition, projectile.position));
        projectile.distanceTraveled += travel;
        consumed = consumed || (projectile.maxRange > 0.0f && projectile.distanceTraveled >= projectile.maxRange);
        const int collisionSteps = std::max(1, static_cast<int>(std::ceil(travel / 0.28f)));
        for (int step = 1; !consumed && step <= collisionSteps; ++step)
        {
            const float t = static_cast<float>(step) / static_cast<float>(collisionSteps);
            const Vector3 sample {
                previousPosition.x + (projectile.position.x - previousPosition.x) * t,
                previousPosition.y + (projectile.position.y - previousPosition.y) * t,
                previousPosition.z + (projectile.position.z - previousPosition.z) * t };
            const GridPos sampleBlock = world_.WorldToGrid(sample);
            if (!world_.IsAir(sampleBlock))
            {
                projectile.position = sample;
                if (EnergyCore* core = FindCoreAt(sampleBlock);
                    core != nullptr && core->IsAlive() && core->GetTeamId() != projectile.ownerTeamId)
                {
                    const int coreDamage = std::max(1, projectile.damage / 2);
                    const bool coreDestroyed = core->Damage(coreDamage);
                    AddWorldEffect(sample, Color { 178, 245, 255, 255 }, 0.34f, 0.28f);
                    // Owner-private replicated feedback, independent of the direct
                    // AddWorldEffect above: pushes to the replicated event stream only
                    // (no PresentCombatEvent call here), so a real network shooter
                    // learns their shot hit the core without touching host presentation.
                    if (const Player* shooter = matchSimulation_.GetPlayer(projectile.ownerId))
                    {
                        PlayerMatchScore& shooterScore = GetPlayerScore(projectile.ownerId);
                        shooterScore.coreDamage += coreDamage;
                        if (coreDestroyed)
                        {
                            ++shooterScore.coresDestroyed;
                        }
                        CombatPresentationEvent presentation {};
                        presentation.valid = true;
                        presentation.event.attackerId = projectile.ownerId;
                        presentation.event.targetTeamId = core->GetTeamId();
                        presentation.event.position = sample;
                        presentation.event.damage = coreDamage;
                        presentation.event.coreHit = true;
                        presentation.event.coreDestroyed = coreDestroyed;
                        presentation.message = shooter->GetName() + ": "
                            + ProjectileImpactLabel(projectile.kind, projectile.fireZone)
                            + " нанес Кору " + std::to_string(coreDamage) + " урона."
                            + (coreDestroyed ? " Кор уничтожен!" : "");
                        PushCombatEventSnapshots(presentation);
                    }
                }
                if (projectile.explosionRadius > 0.0f)
                {
                    DetonateAt(sample, projectile.ownerTeamId, projectile.ownerId, projectile.explosionRadius, projectile.damage, projectile.fireZone, projectile.blueFire);
                }
                consumed = true;
            }
        }

        if (!consumed)
        {
            if (DamageHeroDeviceAlongSegment(
                    projectile.ownerTeamId,
                    previousPosition,
                    projectile.position,
                    projectile.damage,
                    false))
            {
                consumed = true;
            }
        }

        if (!consumed)
        {
            for (Player& player : players_)
            {
                if (player.GetId() == projectile.ownerId
                    || player.GetTeamId() == projectile.ownerTeamId
                    || !player.IsAlive())
                {
                    continue;
                }
                const Vector3 targetCenter { player.GetPosition().x, player.GetPosition().y + 0.35f, player.GetPosition().z };
                const float hitRadius = 0.54f + projectile.radius;
                if (PointSegmentDistanceSquared(targetCenter, previousPosition, projectile.position) <= hitRadius * hitRadius)
                {
                    NoteDamageCredit(player.GetId(), projectile.ownerId,
                        projectile.kind == ProjectileKind::Arrow ? "стрелой"
                        : (projectile.kind == ProjectileKind::Blaster ? "болтом бластера"
                            : (projectile.fireZone ? "коктейлем Молотова" : "снарядом")));
                    player.Damage(projectile.damage);
                    const bool projectileKilledTarget = player.GetHealth() <= 0;
                    for (Player& owner : players_)
                    {
                        if (owner.GetId() == projectile.ownerId
                            && owner.GetHeroId() == HeroId::Likho
                            && owner.GetHeroState().ultimate.active)
                        {
                            HeroRuntimeState& state = owner.MutableHeroState();
                            state.ultimate.active = false;
                            state.ultimate.activeTimer = 0.0f;
                            state.likhoDisguiseTeamId = -1;
                            state.likhoDisguisePlayerId = -1;
                            state.likhoDisguiseHeroId = HeroId::Likho;
                            break;
                        }
                    }
                    const float knockback = BiomeKnockbackMultiplier();
                    if (projectile.kind == ProjectileKind::Arrow)
                    {
                        const float horizontalSpeed = std::sqrt(
                            projectile.velocity.x * projectile.velocity.x + projectile.velocity.z * projectile.velocity.z);
                        const float strength = (kBowTuning.baseKnockback
                            + kBowTuning.punchKnockbackPerLevel * static_cast<float>(projectile.punchLevel)) * knockback;
                        player.ApplyKnockback(Vector3 {
                            horizontalSpeed > 0.001f ? projectile.velocity.x / horizontalSpeed * strength : 0.0f,
                            kBowTuning.verticalKnockback * knockback,
                            horizontalSpeed > 0.001f ? projectile.velocity.z / horizontalSpeed * strength : 0.0f });
                    }
                    else
                    {
                        player.ApplyKnockback(Vector3 { projectile.velocity.x * 0.055f * knockback, 0.85f * knockback, projectile.velocity.z * 0.055f * knockback });
                    }
                    if (projectile.explosionRadius > 0.0f)
                    {
                        DetonateAt(projectile.position, projectile.ownerTeamId, projectile.ownerId, projectile.explosionRadius, projectile.damage, projectile.fireZone, projectile.blueFire);
                    }
                    else
                    {
                        AddWorldEffect(projectile.position, projectile.kind == ProjectileKind::Blaster
                            ? Color { 98, 245, 255, 255 }
                            : Color { 112, 232, 255, 255 }, 0.26f, 0.25f);
                        audio_.PlayHit();
                    }
                    // Owner-private replicated hit feedback for BOTH shooter and
                    // victim (PushCombatEventSnapshots already fans out to both
                    // recipients — see the melee RegisterCombatEvent path this
                    // mirrors). Push-only: does not call PresentCombatEvent, so it
                    // never touches the host's own local message/effects/audio,
                    // which are the direct calls just above.
                    if (const Player* shooter = matchSimulation_.GetPlayer(projectile.ownerId))
                    {
                        CombatPresentationEvent presentation {};
                        presentation.valid = true;
                        presentation.event.attackerId = projectile.ownerId;
                        presentation.event.targetId = player.GetId();
                        presentation.event.targetTeamId = player.GetTeamId();
                        presentation.event.position = projectile.position;
                        presentation.event.damage = projectile.damage;
                        presentation.event.killed = projectileKilledTarget;
                        presentation.message = shooter->GetName() + ": "
                            + ProjectileImpactLabel(projectile.kind, projectile.fireZone)
                            + " попадает по " + player.GetName() + ", урон "
                            + std::to_string(projectile.damage) + ".";
                        PushCombatEventSnapshots(presentation);
                    }
                    consumed = true;
                    break;
                }
            }
        }

        if (consumed)
        {
            projectile.lifetime = -1.0f;
        }
        else
        {
            AddWorldEffect(
                projectile.position,
                projectile.velocity,
                projectile.fireZone
                    ? (projectile.blueFire ? Color { 92, 164, 255, 255 } : Color { 255, 118, 70, 255 })
                    : (projectile.kind == ProjectileKind::Blaster ? Color { 98, 245, 255, 255 } : Color { 112, 232, 255, 255 }),
                projectile.radius,
                0.12f,
                projectile.fireZone ? WorldEffectKind::Trail : WorldEffectKind::Burst);
        }
    }

    projectiles_.erase(
        std::remove_if(
            projectiles_.begin(),
            projectiles_.end(),
            [](const EnergyProjectile& projectile)
            {
                return projectile.lifetime <= 0.0f;
            }),
        projectiles_.end());
}

void Game::UpdateHazardZones(float dt)
{
    for (RadonBurn& burn : radonBurns_)
    {
        burn.lifetime -= dt;
        burn.tickTimer -= dt;
        if (burn.tickTimer > 0.0f)
        {
            continue;
        }
        burn.tickTimer = 1.0f;
        for (Player& target : players_)
        {
            if (target.GetId() != burn.targetPlayerId)
            {
                continue;
            }
            if (!target.IsAlive() || target.IsEliminated())
            {
                burn.lifetime = 0.0f;
                break;
            }
            const int damage = burn.blueFire ? 4 : 2;
            NoteDamageCredit(target.GetId(), burn.ownerPlayerId, burn.blueFire ? "blue fire" : "burning");
            target.Damage(damage);
            AddFloatingText(burn.blueFire ? "BLUE BURN -4" : "BURN -2", target.GetPosition(),
                burn.blueFire ? Color { 92, 164, 255, 255 } : Color { 255, 118, 70, 255 });
            break;
        }
    }
    radonBurns_.erase(
        std::remove_if(radonBurns_.begin(), radonBurns_.end(),
            [](const RadonBurn& burn) { return burn.lifetime <= 0.0f; }),
        radonBurns_.end());

    for (MolotovBlockBurn& burn : molotovBlockBurns_)
    {
        burn.timer -= dt;
        const Block* block = world_.GetBlock(burn.position);
        if (block == nullptr || block->type != burn.blockType)
        {
            burn.timer = -1.0f;
            continue;
        }
        if (burn.timer <= 0.0f)
        {
            BreakWorldBlock(burn.position, burn.ownerTeamId, BlockDeltaReason::FireBurn);
            AddWorldEffect(world_.GridToWorld(burn.position), Color { 255, 118, 70, 255 }, 0.24f, 0.30f);
        }
        else
        {
            AddWorldEffect(world_.GridToWorld(burn.position), Color { 255, 118, 70, 255 }, 0.10f, 0.12f);
        }
    }
    molotovBlockBurns_.erase(
        std::remove_if(molotovBlockBurns_.begin(), molotovBlockBurns_.end(),
            [](const MolotovBlockBurn& burn) { return burn.timer <= 0.0f; }),
        molotovBlockBurns_.end());

    for (HazardZone& zone : hazardZones_)
    {
        zone.lifetime -= dt;
        zone.tickTimer -= dt;
        const Color fireColor = zone.blueFire ? Color { 92, 164, 255, 255 } : Color { 255, 88, 42, 255 };
        AddWorldEffect(zone.position, Vector3 { 0.0f, 0.0f, 1.0f }, fireColor, zone.radius, 0.18f, WorldEffectKind::FireZone);
        if (zone.tickTimer > 0.0f)
        {
            continue;
        }
        zone.tickTimer = 0.55f;

        for (EnergyCore& core : matchSimulation_.Cores())
        {
            if (core.IsAlive()
                && DistanceSquared(world_.GridToWorld(core.GetBlockPosition()), zone.position)
                    <= (zone.radius + 0.8f) * (zone.radius + 0.8f))
            {
                core.Damage(zone.blueFire ? 4 : 2);
            }
        }

        for (Player& player : players_)
        {
            if (!player.IsAlive() || player.GetTeamId() == zone.ownerTeamId)
            {
                continue;
            }
            if (DistanceSquared(player.GetPosition(), zone.position) <= zone.radius * zone.radius)
            {
                NoteDamageCredit(player.GetId(), zone.ownerPlayerId, zone.blueFire ? "синим огнем" : "огнем");
                player.Damage(zone.damagePerTick);
                auto burn = std::find_if(radonBurns_.begin(), radonBurns_.end(),
                    [&player, &zone](const RadonBurn& existing)
                    {
                        return existing.targetPlayerId == player.GetId()
                            && existing.ownerPlayerId == zone.ownerPlayerId;
                    });
                if (burn == radonBurns_.end())
                {
                    radonBurns_.push_back(RadonBurn {
                        player.GetId(), zone.ownerPlayerId, zone.ownerTeamId, 2.2f, 0.7f, zone.blueFire });
                }
                else
                {
                    burn->lifetime = 2.2f;
                    burn->blueFire = zone.blueFire;
                }
                AddWorldEffect(player.GetPosition(), zone.blueFire ? Color { 92, 164, 255, 255 } : Color { 255, 118, 70, 255 }, 0.22f, 0.18f);
            }
        }
    }

    hazardZones_.erase(
        std::remove_if(
            hazardZones_.begin(),
            hazardZones_.end(),
            [](const HazardZone& zone)
            {
                return zone.lifetime <= 0.0f;
            }),
        hazardZones_.end());
}

void Game::UpdateHeroPassives(float dt)
{
    for (Player& player : players_)
    {
        float incomingMultiplier = 1.0f;
        float outgoingMultiplier = 1.0f;
        bool radonProtected = false;
        bool radonOverloaded = false;
        HeroRuntimeState& heroState = player.MutableHeroState();
        if (player.GetHeroId() == HeroId::Radon)
        {
            EnergyCore* core = FindCoreByTeam(player.GetTeamId());
            if (core != nullptr && core->IsAlive())
            {
                const Vector3 corePosition = world_.GridToWorld(core->GetBlockPosition());
                if (DistanceSquared(player.GetPosition(), corePosition) <= kRadonBaseRadiusSq)
                {
                    incomingMultiplier = 0.8f;
                    radonProtected = true;
                }
            }
            else
            {
                incomingMultiplier = 1.2f;
                outgoingMultiplier = 1.2f;
                radonOverloaded = true;
            }
        }
        else if (player.GetHeroId() == HeroId::Orbita)
        {
            StepOrbitaDash(player, dt);
            if (player.IsOnGround())
            {
                heroState.orbitaAirDashLocked = false;
            }

            const Vector3 velocity = player.GetVelocity();
            const float horizontalSpeed = Length2D(velocity);
            const bool riskyAirMove = !player.IsOnGround()
                && horizontalSpeed >= kOrbitaMomentumSpeed * 0.70f
                && std::fabs(velocity.y) > 1.05f;
            if (horizontalSpeed >= kOrbitaMomentumSpeed || riskyAirMove)
            {
                const bool wasCharged = heroState.orbitaMomentumStrike;
                heroState.orbitaMomentumStrike = true;
                heroState.orbitaPulseTimer = std::max(heroState.orbitaPulseTimer, 0.85f);
                if (!wasCharged && HasLocalCamera(ControlKindForPlayer(player)))
                {
                    AddFloatingText("разгон", Vector3 { player.GetPosition().x, player.GetPosition().y + 1.25f, player.GetPosition().z }, HeroAccentColor(HeroId::Orbita));
                }
            }

            if (heroState.ultimate.cooldownRemaining <= 0.0f && heroState.ultimateCharge < 100.0f)
            {
                float chargeGain = horizontalSpeed * dt * 0.18f;
                if (riskyAirMove)
                {
                    chargeGain += dt * 1.25f;
                }
                if (chargeGain > 0.0f)
                {
                    player.AddHeroUltimateCharge(chargeGain);
                }
            }
        }
        else if (player.GetHeroId() == HeroId::Likho && player.IsAlive())
        {
            bool insideEnemyBase = false;
            for (const EnergyCore& core : matchSimulation_.Cores())
            {
                if (!core.IsAlive() || core.GetTeamId() == player.GetTeamId())
                {
                    continue;
                }
                const Vector3 corePosition = world_.GridToWorld(core.GetBlockPosition());
                if (DistanceSquared(player.GetPosition(), corePosition) <= 144.0f)
                {
                    insideEnemyBase = true;
                    if (heroState.ultimate.active && DistanceSquared(player.GetPosition(), corePosition) <= 49.0f)
                    {
                        heroState.ultimate.active = false;
                        heroState.ultimate.activeTimer = 0.0f;
                        heroState.likhoDisguiseTeamId = -1;
                        if (HasLocalCamera(ControlKindForPlayer(player)))
                        {
                            AddEventMessage("Кор раскрыл маскировку Лихо", HeroAccentColor(HeroId::Likho), 2.0f);
                        }
                    }
                    break;
                }
            }
            if (heroState.likhoInsideEnemyBase && !insideEnemyBase)
            {
                player.ActivateSpeedBoost(4.0f);
                player.AddHeroUltimateCharge(8.0f);
                if (HasLocalCamera(ControlKindForPlayer(player)))
                {
                    AddFloatingText("побег", player.GetPosition(), HeroAccentColor(HeroId::Likho));
                }
            }
            heroState.likhoInsideEnemyBase = insideEnemyBase;
        }
        heroState.radonProtected = radonProtected;
        heroState.radonOverloaded = radonOverloaded;
        if (player.GetHeroId() == HeroId::Radon
            && heroState.animationTimer <= 0.0f
            && !heroState.ultimatePrimed)
        {
            if (radonOverloaded)
            {
                heroState.animationState = HeroAnimationState::Overloaded;
            }
            else
            {
                heroState.animationState = HeroAnimationState::Idle;
            }
        }
        player.SetHeroDamageMultipliers(incomingMultiplier, outgoingMultiplier);
    }
}

bool Game::StepOrbitaDash(Player& player, float dt)
{
    if (player.GetHeroId() != HeroId::Orbita || dt <= 0.0f)
    {
        return false;
    }

    HeroRuntimeState& heroState = player.MutableHeroState();
    if (heroState.orbitaDashRemaining <= 0.0f)
    {
        return false;
    }

    constexpr float dashSpeed = 42.0f;
    const float step = std::min(heroState.orbitaDashRemaining, dashSpeed * dt);
    const float lift = heroState.orbitaDashRemaining > 0.001f
        ? heroState.orbitaDashLiftRemaining * (step / heroState.orbitaDashRemaining)
        : 0.0f;
    const Vector3 position = player.GetPosition();
    const Vector3 candidate {
        position.x + heroState.orbitaDashDirection.x * step,
        position.y + lift,
        position.z + heroState.orbitaDashDirection.z * step
    };
    if (!world_.CollidesWithAABB(candidate, kPlayerCollisionHalfExtents))
    {
        player.Teleport(candidate, false);
        heroState.orbitaDashRemaining -= step;
        heroState.orbitaDashLiftRemaining = std::max(0.0f, heroState.orbitaDashLiftRemaining - lift);
        return true;
    }

    heroState.orbitaDashRemaining = 0.0f;
    heroState.orbitaDashLiftRemaining = 0.0f;
    return false;
}

void Game::UpdateHeroTemporaryBlocks(float dt)
{
    for (HeroTemporaryBlock& temporary : heroTemporaryBlocks_)
    {
        temporary.timer -= dt;
        const Block* block = world_.GetBlock(temporary.position);
        const bool stillOwnedPhantom = block != nullptr
            && block->type == BlockType::EnergyGlassBlock
            && block->teamId == temporary.ownerTeamId
            && block->breakable;
        if (!stillOwnedPhantom)
        {
            temporary.timer = -1.0f;
            continue;
        }

        if (temporary.timer <= 0.0f)
        {
            const Vector3 center = world_.GridToWorld(temporary.position);
            RemoveWorldBlock(temporary.position, BlockDeltaReason::TemporaryExpire);
            AddWorldEffect(center, HeroAccentColor(HeroId::Orbita), 0.18f, 0.22f);
        }
    }

    heroTemporaryBlocks_.erase(
        std::remove_if(
            heroTemporaryBlocks_.begin(),
            heroTemporaryBlocks_.end(),
            [](const HeroTemporaryBlock& temporary)
            {
                return temporary.timer <= 0.0f;
            }),
        heroTemporaryBlocks_.end());
}

bool Game::DamageHeroDeviceAlongSegment(
    int attackerTeamId,
    Vector3 start,
    Vector3 end,
    int damage,
    bool toolAttack)
{
    const auto distanceToSegmentSq = [start, end](Vector3 point)
    {
        const Vector3 segment { end.x - start.x, end.y - start.y, end.z - start.z };
        const Vector3 offset { point.x - start.x, point.y - start.y, point.z - start.z };
        const float lengthSq = segment.x * segment.x + segment.y * segment.y + segment.z * segment.z;
        const float t = lengthSq > 0.0001f
            ? std::clamp((offset.x * segment.x + offset.y * segment.y + offset.z * segment.z) / lengthSq, 0.0f, 1.0f)
            : 0.0f;
        const Vector3 nearest { start.x + segment.x * t, start.y + segment.y * t, start.z + segment.z * t };
        return DistanceSquared(point, nearest);
    };
    const auto hitFeedback = [this](Vector3 position, HeroId hero)
    {
        AddWorldEffect(position, HeroAccentColor(hero), 0.28f, 0.24f);
        audio_.PlayHit();
    };

    for (BromVacuumBot& bot : bromVacuumBots_)
    {
        if (bot.ownerTeamId != attackerTeamId && distanceToSegmentSq(bot.position) <= 0.58f * 0.58f)
        {
            if (bot.invulnerabilityTimer <= 0.0f)
            {
                bot.health -= std::max(1, damage);
                hitFeedback(bot.position, HeroId::Brom);
            }
            return true;
        }
    }
    for (BromTurretDrone& drone : bromTurretDrones_)
    {
        if (drone.ownerTeamId != attackerTeamId && distanceToSegmentSq(drone.position) <= 0.62f * 0.62f)
        {
            if (drone.invulnerabilityTimer <= 0.0f)
            {
                drone.health -= std::max(1, damage);
                hitFeedback(drone.position, HeroId::Brom);
            }
            return true;
        }
    }
    for (KonvoyTrap& trap : konvoyTraps_)
    {
        if (trap.ownerTeamId != attackerTeamId && distanceToSegmentSq(trap.position) <= 0.58f * 0.58f)
        {
            if (toolAttack)
            {
                trap.health -= std::max(1, damage);
                hitFeedback(trap.position, HeroId::Konvoy);
            }
            return true;
        }
    }
    for (KonvoyDome& dome : konvoyDomes_)
    {
        const float startDistance = std::sqrt(DistanceSquared(Vector3 { start.x, dome.position.y, start.z }, dome.position));
        const float endDistance = std::sqrt(DistanceSquared(Vector3 { end.x, dome.position.y, end.z }, dome.position));
        const bool crossesShell = (startDistance < kKonvoyDomeVisualRadius) != (endDistance < kKonvoyDomeVisualRadius)
            || std::fabs(endDistance - kKonvoyDomeVisualRadius) < 0.65f;
        if (dome.ownerTeamId != attackerTeamId && crossesShell)
        {
            dome.health -= std::max(1, damage);
            hitFeedback(end, HeroId::Konvoy);
            return true;
        }
    }
    for (SvidetelEcho& echo : svidetelEchoes_)
    {
        if (echo.ownerTeamId != attackerTeamId && distanceToSegmentSq(echo.position) <= 0.62f * 0.62f)
        {
            echo.health -= std::max(1, damage);
            hitFeedback(echo.position, HeroId::Svidetel);
            return true;
        }
    }
    return false;
}

void Game::UpdateBromDevices(float dt)
{
    const Color bromColor = HeroAccentColor(HeroId::Brom);
    const auto horizontalDistanceSq = [](Vector3 a, Vector3 b)
    {
        const float dx = a.x - b.x;
        const float dz = a.z - b.z;
        return dx * dx + dz * dz;
    };
    const auto surfaceFor = [this](Vector3 desired, float currentY, float maxClimb) -> std::optional<Vector3>
    {
        const GridPos column = world_.WorldToGrid(desired);
        const int startY = std::clamp(world_.WorldToGrid(Vector3 { desired.x, currentY, desired.z }).y + 2, kBuildMinY, kBuildMaxY);
        for (int y = startY; y >= kBuildMinY - 4; --y)
        {
            const GridPos ground { column.x, y, column.z };
            if (!world_.IsSolid(ground))
            {
                continue;
            }

            Vector3 snapped {
                desired.x,
                world_.GridToWorld(ground).y + 0.68f,
                desired.z
            };
            const float heightDelta = snapped.y - currentY;
            if (heightDelta > maxClimb || heightDelta < -kBromVacuumDropHeight)
            {
                continue;
            }
            if (!world_.CollidesWithAABB(Vector3 { snapped.x, snapped.y + 0.24f, snapped.z }, Vector3 { 0.30f, 0.22f, 0.30f }))
            {
                return snapped;
            }
        }

        return std::nullopt;
    };
    const auto moveGroundedTowards = [dt, &surfaceFor](Vector3& position, Vector3 target, float speed)
    {
        const Vector3 flatDelta {
            target.x - position.x,
            0.0f,
            target.z - position.z
        };
        const float distance = Length2D(flatDelta);
        if (distance <= 0.001f)
        {
            if (const std::optional<Vector3> snapped = surfaceFor(position, position.y, kBromVacuumStepHeight))
            {
                position = *snapped;
            }
            return distance;
        }

        if (const std::optional<Vector3> snapped = surfaceFor(position, position.y, kBromVacuumStepHeight))
        {
            position = *snapped;
        }

        const Vector3 direction { flatDelta.x / distance, 0.0f, flatDelta.z / distance };
        const float step = std::min(distance, speed * dt);
        const auto tryMove = [&position, &surfaceFor](Vector3 candidate, float maxClimb)
        {
            const std::optional<Vector3> snapped = surfaceFor(candidate, position.y, maxClimb);
            if (!snapped.has_value())
            {
                return false;
            }

            position = *snapped;
            return true;
        };

        const Vector3 directCandidate { position.x + direction.x * step, position.y, position.z + direction.z * step };
        if (tryMove(directCandidate, 0.28f) || tryMove(directCandidate, kBromVacuumStepHeight))
        {
            return distance;
        }

        const bool tryXFirst = std::fabs(direction.x) >= std::fabs(direction.z);
        const Vector3 xCandidate { position.x + direction.x * step, position.y, position.z };
        const Vector3 zCandidate { position.x, position.y, position.z + direction.z * step };
        if (tryXFirst)
        {
            if (tryMove(xCandidate, 0.28f) || tryMove(xCandidate, kBromVacuumStepHeight)
                || tryMove(zCandidate, 0.28f) || tryMove(zCandidate, kBromVacuumStepHeight))
            {
                return distance;
            }
        }
        else if (tryMove(zCandidate, 0.28f) || tryMove(zCandidate, kBromVacuumStepHeight)
            || tryMove(xCandidate, 0.28f) || tryMove(xCandidate, kBromVacuumStepHeight))
        {
            return distance;
        }

        return distance;
    };
    const auto navigateVacuum = [this, dt, &moveGroundedTowards, &horizontalDistanceSq](BromVacuumBot& bot, Vector3 target, float speed)
    {
        bot.repathTimer = std::max(0.0f, bot.repathTimer - dt);
        const bool targetChanged = DistanceSquared(bot.navTarget, target) > 3.0f * 3.0f;
        if (!bot.hasNavWaypoint || bot.repathTimer <= 0.0f || targetChanged || bot.stuckTimer > 0.65f)
        {
            bot.navTarget = target;
            const std::optional<Vector3> waypoint = FindDeviceGroundWaypoint(world_, bot.position, target);
            bot.hasNavWaypoint = waypoint.has_value();
            bot.navWaypoint = waypoint.value_or(target);
            bot.repathTimer = bot.stuckTimer > 0.65f ? 0.10f : 0.38f;
        }

        const Vector3 before = bot.position;
        const Vector3 destination = bot.hasNavWaypoint ? bot.navWaypoint : target;
        moveGroundedTowards(bot.position, destination, speed);
        const float movedSq = horizontalDistanceSq(before, bot.position);
        const bool stillFar = horizontalDistanceSq(bot.position, target) > 1.2f * 1.2f;
        bot.stuckTimer = movedSq < 0.003f * 0.003f && stillFar
            ? bot.stuckTimer + dt
            : std::max(0.0f, bot.stuckTimer - dt * 2.0f);
        if (bot.hasNavWaypoint && horizontalDistanceSq(bot.position, bot.navWaypoint) < 0.65f * 0.65f)
        {
            bot.repathTimer = 0.0f;
        }
        if (bot.stuckTimer > 1.2f)
        {
            const Vector3 delta { target.x - bot.position.x, 0.0f, target.z - bot.position.z };
            const Vector3 side = Normalize(Vector3 { -delta.z, 0.0f, delta.x });
            moveGroundedTowards(bot.position,
                Vector3 { bot.position.x + side.x * 1.8f, bot.position.y, bot.position.z + side.z * 1.8f },
                speed);
            bot.repathTimer = 0.0f;
            bot.stuckTimer = 0.45f;
        }
        bot.lastPosition = bot.position;
    };
    const auto moveFlyingTowards = [this, dt](BromTurretDrone& drone, Vector3 target, float speed)
    {
        const Vector3 delta {
            target.x - drone.position.x,
            target.y - drone.position.y,
            target.z - drone.position.z
        };
        const float distance = Length(delta);
        if (distance <= 0.05f)
        {
            return;
        }
        const Vector3 direction { delta.x / distance, delta.y / distance, delta.z / distance };
        const float step = std::min(distance, speed * dt);
        const Vector3 direct {
            drone.position.x + direction.x * step,
            drone.position.y + direction.y * step,
            drone.position.z + direction.z * step
        };
        const Vector3 halfExtents { 0.32f, 0.28f, 0.32f };
        if (!world_.CollidesWithAABB(direct, halfExtents))
        {
            drone.position = direct;
            return;
        }
        const Vector3 side = Normalize(Vector3 { -direction.z, 0.0f, direction.x });
        const Vector3 alternatives[] {
            Vector3 { direct.x, direct.y + 0.65f, direct.z },
            Vector3 { drone.position.x + side.x * step, drone.position.y, drone.position.z + side.z * step },
            Vector3 { drone.position.x - side.x * step, drone.position.y, drone.position.z - side.z * step },
            Vector3 { direct.x, direct.y - 0.45f, direct.z }
        };
        for (const Vector3& candidate : alternatives)
        {
            if (!world_.CollidesWithAABB(candidate, halfExtents))
            {
                drone.position = candidate;
                return;
            }
        }
    };

    for (BromVacuumBot& bot : bromVacuumBots_)
    {
        bot.invulnerabilityTimer = std::max(0.0f, bot.invulnerabilityTimer - dt);
        bot.targetLockTimer = std::max(0.0f, bot.targetLockTimer - dt);
        if (bot.temporary)
        {
            bot.lifetime -= dt;
            if (bot.lifetime <= 0.0f)
            {
                AddWorldEffect(bot.position, bromColor, 0.24f, 0.24f);
                continue;
            }
        }

        bot.pulseTimer -= dt;
        if (bot.pulseTimer <= 0.0f)
        {
            bot.pulseTimer = 0.45f;
            AddWorldEffect(bot.position, bromColor, 0.12f, 0.16f);
        }

        const int cargoUnits = BromCargoUnits(bot.cargo);
        if (cargoUnits >= kBromVacuumCapacity)
        {
            bot.returning = true;
        }

        Team* team = FindTeam(bot.ownerTeamId);
        const Vector3 basePosition = team != nullptr
            ? TeamChestDepositPosition(*team)
            : bot.position;

        if (bot.returning)
        {
            bot.lockedPickupIndex = -1;
            navigateVacuum(bot, basePosition, 3.55f);
            if (DistanceSquared(bot.position, basePosition) <= 1.25f * 1.25f)
            {
                int delivered = 0;
                if (bot.ownerTeamId >= 0 && bot.ownerTeamId < static_cast<int>(teamChests_.size()))
                {
                    for (ResourceType type : { ResourceType::Iron, ResourceType::Gold, ResourceType::Crystal })
                    {
                        const int amount = bot.cargo[ResourceIndex(type)];
                        if (amount <= 0)
                        {
                            continue;
                        }
                        teamChests_[bot.ownerTeamId].AddResource(type, amount);
                        delivered += amount;
                        bot.cargo[ResourceIndex(type)] = 0;
                    }
                }

                if (delivered > 0)
                {
                    for (Player& player : players_)
                    {
                        if (player.GetId() == bot.ownerPlayerId && player.GetHeroId() == HeroId::Brom)
                        {
                            player.AddHeroUltimateCharge(static_cast<float>(delivered) * 2.0f);
                            break;
                        }
                    }
                    AddFloatingText("командный сундук +" + std::to_string(delivered), basePosition, bromColor);
                    AddWorldEffect(basePosition, Vector3 { 0.0f, 0.0f, 1.0f }, bromColor, 0.82f, 0.42f, WorldEffectKind::CorePulse);
                    AddEventMessage("Пылесос Брома сложил +" + std::to_string(delivered) + " в командный сундук на базе.", bromColor, 2.6f);
                    audio_.PlayPickup();
                }
                bot.returning = false;
            }
            continue;
        }

        ResourcePickup* bestPickup = nullptr;
        int bestPickupIndex = -1;
        float bestScore = std::numeric_limits<float>::max();
        constexpr float searchRadiusSq = 56.0f * 56.0f;
        if (bot.targetLockTimer > 0.0f
            && bot.lockedPickupIndex >= 0
            && bot.lockedPickupIndex < static_cast<int>(matchSimulation_.Pickups().size()))
        {
            ResourcePickup& locked = matchSimulation_.Pickups()[bot.lockedPickupIndex];
            const int remainingCapacity = kBromVacuumCapacity - BromCargoUnits(bot.cargo);
            if (!locked.collected
                && remainingCapacity >= BromCargoWeight(locked.type) * locked.amount
                && DistanceSquared(bot.position, ToVector3(locked.position)) <= searchRadiusSq)
            {
                bestPickup = &locked;
                bestPickupIndex = bot.lockedPickupIndex;
            }
        }
        for (int pickupIndex = 0; bestPickup == nullptr && pickupIndex < static_cast<int>(matchSimulation_.Pickups().size()); ++pickupIndex)
        {
            ResourcePickup& pickup = matchSimulation_.Pickups()[pickupIndex];
            if (pickup.collected)
            {
                continue;
            }
            const int remainingCapacity = kBromVacuumCapacity - BromCargoUnits(bot.cargo);
            if (remainingCapacity < BromCargoWeight(pickup.type) * pickup.amount)
            {
                continue;
            }

            const float distance = DistanceSquared(bot.position, ToVector3(pickup.position));
            if (distance > searchRadiusSq)
            {
                continue;
            }

            const float centerDistanceSq = pickup.position.x * pickup.position.x + pickup.position.z * pickup.position.z;
            const bool nearOwnBase = DistanceSquared(ToVector3(pickup.position), basePosition) < 13.0f * 13.0f;
            float score = distance + centerDistanceSq * 0.18f;
            if (nearOwnBase)
            {
                score += 1800.0f;
            }
            switch (pickup.type)
            {
            case ResourceType::Crystal:
                score -= 320.0f;
                break;
            case ResourceType::Gold:
                score -= 180.0f;
                break;
            case ResourceType::Iron:
                score -= 30.0f;
                break;
            }
            score -= static_cast<float>(pickup.amount) * 8.0f;

            if (score < bestScore)
            {
                bestScore = score;
                bestPickup = &pickup;
                bestPickupIndex = pickupIndex;
            }
        }

        if (bestPickup == nullptr)
        {
            if (cargoUnits > 0)
            {
                bot.returning = true;
            }
            else
            {
                const Vector3 centerPatrol { 0.0f, basePosition.y, 0.0f };
                navigateVacuum(bot, centerPatrol, 2.95f);
            }
            continue;
        }

        const Vector3 target { bestPickup->position.x, bestPickup->position.y + 0.12f, bestPickup->position.z };
        bot.lockedPickupIndex = bestPickupIndex;
        bot.targetLockTimer = std::max(bot.targetLockTimer, 1.5f);
        navigateVacuum(bot, target, 3.75f);
        if (horizontalDistanceSq(bot.position, target) <= 0.75f * 0.75f
            && std::fabs(bot.position.y - target.y) <= 1.35f)
        {
            bot.cargo[ResourceIndex(bestPickup->type)] += bestPickup->amount;
            AddFloatingText("пылесос +" + std::to_string(bestPickup->amount), ToVector3(bestPickup->position), bromColor);
            AddWorldEffect(ToVector3(bestPickup->position), bromColor, 0.18f, 0.18f);
            bestPickup->collected = true;
            bot.lockedPickupIndex = -1;
            bot.targetLockTimer = 0.0f;
            bot.repathTimer = 0.0f;
            if (BromCargoUnits(bot.cargo) >= kBromVacuumCapacity)
            {
                bot.returning = true;
            }
        }
    }

    bromVacuumBots_.erase(
        std::remove_if(
            bromVacuumBots_.begin(),
            bromVacuumBots_.end(),
            [](const BromVacuumBot& bot)
            {
                return bot.health <= 0 || (bot.temporary && bot.lifetime <= 0.0f);
            }),
        bromVacuumBots_.end());

    for (BromTurretDrone& drone : bromTurretDrones_)
    {
        drone.invulnerabilityTimer = std::max(0.0f, drone.invulnerabilityTimer - dt);
        if (drone.temporary)
        {
            drone.lifetime -= dt;
            if (drone.lifetime <= 0.0f)
            {
                AddWorldEffect(drone.position, bromColor, 0.28f, 0.24f);
                continue;
            }
        }

        drone.fireCooldown = std::max(0.0f, drone.fireCooldown - dt);
        drone.repathTimer = std::max(0.0f, drone.repathTimer - dt);
        drone.targetLockTimer = std::max(0.0f, drone.targetLockTimer - dt);
        drone.shotFlashTimer = std::max(0.0f, drone.shotFlashTimer - dt);
        drone.pulseTimer -= dt;
        if (drone.pulseTimer <= 0.0f)
        {
            drone.pulseTimer = 0.38f;
            AddWorldEffect(drone.position, bromColor, 0.16f, 0.14f);
        }
        Player* target = nullptr;
        float bestScore = std::numeric_limits<float>::max();
        constexpr float awarenessRange = 34.0f;
        const Team* ownerTeam = FindTeam(drone.ownerTeamId);
        const Vector3 defensePoint = ownerTeam != nullptr
            ? world_.GridToWorld(ownerTeam->coreBlock)
            : drone.position;
        for (Player& player : players_)
        {
            if (!player.IsAlive() || player.IsEliminated() || player.GetHealth() <= 0 || player.GetTeamId() == drone.ownerTeamId)
            {
                continue;
            }

            const Vector3 aimPoint { player.GetPosition().x, player.GetPosition().y + 0.65f, player.GetPosition().z };
            const float distanceSq = DistanceSquared(drone.position, aimPoint);
            if (distanceSq > awarenessRange * awarenessRange)
            {
                continue;
            }

            const Vector3 ray {
                aimPoint.x - drone.position.x,
                aimPoint.y - drone.position.y,
                aimPoint.z - drone.position.z
            };
            const float distance = Length(ray);
            const std::optional<RaycastHit> wall = world_.Raycast(drone.position, ray, distance);
            const bool visible = !wall.has_value() || wall->distance >= distance - 0.35f;
            float score = distance
                + static_cast<float>(player.GetHealth()) * 0.055f
                + std::sqrt(DistanceSquared(player.GetPosition(), defensePoint)) * 0.08f;
            if (visible)
            {
                score -= 5.0f;
            }
            if (player.GetId() == drone.lockedTargetPlayerId && drone.targetLockTimer > 0.0f)
            {
                score -= 7.0f;
            }
            if (score < bestScore)
            {
                target = &player;
                bestScore = score;
            }
        }

        if (target == nullptr)
        {
            drone.lockedTargetPlayerId = -1;
            Player* owner = nullptr;
            for (Player& player : players_)
            {
                if (player.GetId() == drone.ownerPlayerId && player.IsAlive() && !player.IsEliminated())
                {
                    owner = &player;
                    break;
                }
            }
            const Vector3 anchor = owner != nullptr ? owner->GetPosition() : defensePoint;
            const float orbit = static_cast<float>(drone.ownerPlayerId) * 1.7f + matchSimulation_.MatchTimeSeconds() * 0.45f;
            const Vector3 patrol {
                anchor.x + std::cos(orbit) * 3.4f,
                anchor.y + 2.2f,
                anchor.z + std::sin(orbit) * 3.4f
            };
            moveFlyingTowards(drone, patrol, 4.8f);
            continue;
        }

        drone.lockedTargetPlayerId = target->GetId();
        drone.targetLockTimer = std::max(drone.targetLockTimer, 1.4f);

        const Vector3 targetPoint { target->GetPosition().x, target->GetPosition().y + 0.65f, target->GetPosition().z };
        const Vector3 toTarget {
            targetPoint.x - drone.position.x,
            targetPoint.y - drone.position.y,
            targetPoint.z - drone.position.z
        };
        const float targetDistance = Length(toTarget);
        const std::optional<RaycastHit> currentWall = world_.Raycast(drone.position, toTarget, targetDistance);
        const bool hasLineOfSight = !currentWall.has_value() || currentWall->distance >= targetDistance - 0.35f;

        Vector3 tacticalPosition = drone.position;
        bool shouldMove = false;
        if (!hasLineOfSight || targetDistance > 13.5f)
        {
            float bestPositionScore = std::numeric_limits<float>::max();
            for (int index = 0; index < 12; ++index)
            {
                const float angle = static_cast<float>(index) * 0.5235987756f
                    + static_cast<float>(drone.ownerPlayerId) * 0.31f;
                const Vector3 candidate {
                    targetPoint.x + std::cos(angle) * 9.5f,
                    targetPoint.y + 1.8f + static_cast<float>(index % 3) * 0.35f,
                    targetPoint.z + std::sin(angle) * 9.5f
                };
                if (world_.CollidesWithAABB(candidate, Vector3 { 0.32f, 0.28f, 0.32f }))
                {
                    continue;
                }
                const Vector3 candidateRay {
                    targetPoint.x - candidate.x, targetPoint.y - candidate.y, targetPoint.z - candidate.z };
                const float candidateDistance = Length(candidateRay);
                const std::optional<RaycastHit> candidateWall = world_.Raycast(candidate, candidateRay, candidateDistance);
                if (candidateWall.has_value() && candidateWall->distance < candidateDistance - 0.35f)
                {
                    continue;
                }
                const float score = DistanceSquared(drone.position, candidate)
                    + DistanceSquared(candidate, defensePoint) * 0.04f;
                if (score < bestPositionScore)
                {
                    bestPositionScore = score;
                    tacticalPosition = candidate;
                    shouldMove = true;
                }
            }
        }
        else if (targetDistance < 5.5f)
        {
            const Vector3 away = Normalize(Vector3 {
                drone.position.x - targetPoint.x, 0.25f, drone.position.z - targetPoint.z });
            tacticalPosition = Vector3 {
                drone.position.x + away.x * 4.0f,
                drone.position.y + 0.7f,
                drone.position.z + away.z * 4.0f
            };
            shouldMove = true;
        }
        else
        {
            const Vector3 orbitSide = Normalize(Vector3 { -toTarget.z, 0.0f, toTarget.x });
            tacticalPosition = Vector3 {
                drone.position.x + orbitSide.x * 1.8f,
                targetPoint.y + 1.7f,
                drone.position.z + orbitSide.z * 1.8f
            };
            shouldMove = true;
        }
        if (shouldMove)
        {
            moveFlyingTowards(drone, tacticalPosition, 6.4f);
        }

        const Vector3 updatedRay {
            targetPoint.x - drone.position.x,
            targetPoint.y - drone.position.y,
            targetPoint.z - drone.position.z
        };
        const float updatedDistance = Length(updatedRay);
        const std::optional<RaycastHit> updatedWall = world_.Raycast(drone.position, updatedRay, updatedDistance);
        if (drone.fireCooldown > 0.0f
            || updatedDistance > kBromTurretAttackRange
            || (updatedWall.has_value() && updatedWall->distance < updatedDistance - 0.35f))
        {
            continue;
        }

        const float projectileTravelTime = updatedDistance / kBlasterTuning.baseSpeed;
        const Vector3 targetVelocity = target->GetVelocity();
        const Vector3 predictedTarget {
            targetPoint.x + targetVelocity.x * projectileTravelTime,
            targetPoint.y + targetVelocity.y * projectileTravelTime,
            targetPoint.z + targetVelocity.z * projectileTravelTime
        };
        const Vector3 direction = Normalize(Vector3 {
            predictedTarget.x - drone.position.x,
            predictedTarget.y - drone.position.y,
            predictedTarget.z - drone.position.z
        });
        const int targetHealthBefore = target->GetHealth();
        NoteDamageCredit(target->GetId(), drone.ownerPlayerId, "турелью Брома");
        target->Damage(6);
        if (targetHealthBefore > 0 && target->GetHealth() <= 0 && drone.ownerPlayerId >= 0)
        {
            ++GetPlayerScore(drone.ownerPlayerId).kills;
            if (drone.ownerPlayerId == localPlayerId_)
            {
                ++stats_.kills;
            }

            std::string ownerName = "Бром";
            for (const Player& player : players_)
            {
                if (player.GetId() == drone.ownerPlayerId)
                {
                    ownerName = player.GetName();
                    break;
                }
            }
            AddKillFeed(ownerName + " добил дроном " + target->GetName(), bromColor, 5.2f);
        }
        target->ApplyKnockback(Vector3 { direction.x * 3.2f, 0.58f, direction.z * 3.2f });
        AddWorldEffect(drone.position, direction, bromColor, 0.28f, 0.24f, WorldEffectKind::Trail);
        AddWorldEffect(target->GetPosition(), bromColor, 0.18f, 0.18f);
        AddFloatingText("дрон -6", target->GetPosition(), bromColor);
        drone.lastShotTarget = predictedTarget;
        drone.shotFlashTimer = 0.28f;
        drone.fireCooldown = 1.15f;
        audio_.PlayHit();
    }

    bromTurretDrones_.erase(
        std::remove_if(
            bromTurretDrones_.begin(),
            bromTurretDrones_.end(),
            [](const BromTurretDrone& drone)
            {
                return drone.health <= 0 || (drone.temporary && drone.lifetime <= 0.0f);
            }),
        bromTurretDrones_.end());
}

void Game::UpdateKonvoyDevices(float dt)
{
    const Color konvoyColor = HeroAccentColor(HeroId::Konvoy);
    const auto playerById = [this](int playerId) -> Player*
    {
        for (Player& player : players_)
        {
            if (player.GetId() == playerId)
            {
                return &player;
            }
        }
        return nullptr;
    };

    for (Player& owner : players_)
    {
        if (!owner.IsAlive() || owner.IsEliminated() || owner.GetHeroId() != HeroId::Konvoy)
        {
            continue;
        }
        const EnergyCore* ownCore = FindCoreByTeam(owner.GetTeamId());
        for (Player& target : players_)
        {
            if (!target.IsAlive() || target.IsEliminated() || target.GetTeamId() == owner.GetTeamId())
            {
                continue;
            }
            bool dangerZone = false;
            if (ownCore != nullptr && ownCore->IsAlive()
                && DistanceSquared(target.GetPosition(), world_.GridToWorld(ownCore->GetBlockPosition())) <= 12.0f * 12.0f)
            {
                dangerZone = true;
            }
            for (const EnergyCore& core : matchSimulation_.Cores())
            {
                if (core.IsAlive() && core.GetTeamId() != target.GetTeamId()
                    && DistanceSquared(target.GetPosition(), world_.GridToWorld(core.GetBlockPosition())) <= 10.0f * 10.0f)
                {
                    dangerZone = true;
                    break;
                }
            }
            const int carriedResources = target.GetInventory().GetResource(ResourceType::Iron)
                + target.GetInventory().GetResource(ResourceType::Gold) * 2
                + target.GetInventory().GetResource(ResourceType::Crystal) * 3;
            const float centerDistanceSq = target.GetPosition().x * target.GetPosition().x
                + target.GetPosition().z * target.GetPosition().z;
            dangerZone = dangerZone || centerDistanceSq <= 10.0f * 10.0f || carriedResources >= 12;

            auto mark = std::find_if(konvoyIntruderMarks_.begin(), konvoyIntruderMarks_.end(),
                [&owner, &target](const KonvoyIntruderMark& existing)
                {
                    return existing.ownerPlayerId == owner.GetId() && existing.targetPlayerId == target.GetId();
                });
            if (mark == konvoyIntruderMarks_.end())
            {
                konvoyIntruderMarks_.push_back(KonvoyIntruderMark { owner.GetId(), target.GetId() });
                mark = std::prev(konvoyIntruderMarks_.end());
            }
            mark->markedTimer = std::max(0.0f, mark->markedTimer - dt);
            mark->pulseTimer = std::max(0.0f, mark->pulseTimer - dt);
            if (dangerZone)
            {
                mark->exposure = std::min(3.0f, mark->exposure + dt);
                if (mark->exposure >= 3.0f && mark->markedTimer <= 0.0f)
                {
                    mark->markedTimer = 8.0f;
                    owner.AddHeroUltimateCharge(8.0f);
                    AddFloatingText("INTRUDER", target.GetPosition(), konvoyColor);
                }
            }
            else
            {
                mark->exposure = std::max(0.0f, mark->exposure - dt * 0.5f);
            }
            if (mark->markedTimer > 0.0f && mark->pulseTimer <= 0.0f)
            {
                mark->pulseTimer = 1.0f;
                AddWorldEffect(target.GetPosition(), konvoyColor, 0.18f, 0.18f);
            }
        }
    }

    for (KonvoyTrap& trap : konvoyTraps_)
    {
        trap.lifetime -= dt;
        trap.flashTimer = std::max(0.0f, trap.flashTimer - dt);
        if (trap.lifetime <= 0.0f)
        {
            AddWorldEffect(trap.position, konvoyColor, 0.20f, 0.22f);
            continue;
        }

        for (Player& target : players_)
        {
            if (!target.IsAlive()
                || target.IsEliminated()
                || target.GetHealth() <= 0
                || target.GetTeamId() == trap.ownerTeamId)
            {
                continue;
            }

            const float horizontalSq = (target.GetPosition().x - trap.position.x) * (target.GetPosition().x - trap.position.x)
                + (target.GetPosition().z - trap.position.z) * (target.GetPosition().z - trap.position.z);
            if (horizontalSq > 0.72f * 0.72f || std::fabs(target.GetPosition().y - trap.position.y) > 1.35f)
            {
                continue;
            }

            trap.flashTimer = 0.60f;
            trap.lifetime = 0.0f;
            const bool markedIntruder = std::any_of(
                konvoyIntruderMarks_.begin(), konvoyIntruderMarks_.end(),
                [&trap, &target](const KonvoyIntruderMark& mark)
                {
                    return mark.ownerPlayerId == trap.ownerPlayerId
                        && mark.targetPlayerId == target.GetId()
                        && mark.markedTimer > 0.0f;
                });
            target.ApplyControlDebuff(3.5f,
                markedIntruder ? 0.46f : 0.55f,
                markedIntruder ? 0.42f : 0.50f,
                markedIntruder ? 0.52f : 0.62f);
            AddWorldEffect(trap.position, Vector3 { 0.0f, 0.0f, 1.0f }, konvoyColor, 1.15f, 0.48f, WorldEffectKind::Ring);
            AddFloatingText("капкан", target.GetPosition(), konvoyColor);
            AddEventMessage("Капкан Конвоя сработал на цели " + target.GetName() + ".", konvoyColor, 2.4f);
            if (Player* owner = playerById(trap.ownerPlayerId))
            {
                SetHeroAnimation(*owner, HeroAnimationState::Cast, 0.20f);
                owner->AddHeroUltimateCharge(12.0f);
            }
            audio_.PlayHit();
            break;
        }
    }

    konvoyTraps_.erase(
        std::remove_if(
            konvoyTraps_.begin(),
            konvoyTraps_.end(),
            [](const KonvoyTrap& trap)
            {
                return trap.lifetime <= 0.0f || trap.health <= 0;
            }),
        konvoyTraps_.end());

    for (KonvoyTether& tether : konvoyTethers_)
    {
        tether.lifetime -= dt;
        tether.flashTimer = std::max(0.0f, tether.flashTimer - dt);
        Player* owner = playerById(tether.ownerPlayerId);
        Player* target = playerById(tether.targetPlayerId);
        if (owner == nullptr
            || target == nullptr
            || !owner->IsAlive()
            || !target->IsAlive()
            || owner->GetHealth() <= 0
            || target->GetHealth() <= 0
            || owner->IsEliminated()
            || target->IsEliminated())
        {
            tether.lifetime = 0.0f;
            continue;
        }

        const int ownerDamage = std::max(0, tether.ownerLastHealth - owner->GetHealth());
        tether.accumulatedOwnerDamage += ownerDamage;
        tether.ownerLastHealth = owner->GetHealth();
        if (tether.accumulatedOwnerDamage > 30)
        {
            tether.lifetime = 0.0f;
            AddWorldEffect(target->GetPosition(), konvoyColor, 0.35f, 0.25f);
            AddFloatingText("CHAIN BROKEN", target->GetPosition(), konvoyColor);
            continue;
        }

        const Vector3 ownerPosition = owner->GetPosition();
        const Vector3 targetPosition = target->GetPosition();
        Vector3 towardOwner {
            ownerPosition.x - targetPosition.x, 0.0f, ownerPosition.z - targetPosition.z };
        const float towardOwnerLength = Length2D(towardOwner);
        if (towardOwnerLength > 0.0001f)
        {
            towardOwner.x /= towardOwnerLength;
            towardOwner.z /= towardOwnerLength;
        }
        const float tetherDistanceSq = DistanceSquared(ownerPosition, targetPosition);
        if (tetherDistanceSq > 5.35f * 5.35f)
        {
            tether.flashTimer = std::max(tether.flashTimer, 0.18f);
            target->ApplyKnockback(Vector3 { towardOwner.x * 1.15f, 0.18f, towardOwner.z * 1.15f }, 0.08f);
            if (towardOwnerLength > kKonvoyHandcuffRadius)
            {
                Vector3 corrected = targetPosition;
                corrected.x = ownerPosition.x - towardOwner.x * (kKonvoyHandcuffRadius - 0.15f);
                corrected.z = ownerPosition.z - towardOwner.z * (kKonvoyHandcuffRadius - 0.15f);
                target->Teleport(corrected, false);
            }
            AddWorldEffect(target->GetPosition(), konvoyColor, 0.14f, 0.14f);
        }
    }

    konvoyTethers_.erase(
        std::remove_if(
            konvoyTethers_.begin(),
            konvoyTethers_.end(),
            [](const KonvoyTether& tether)
            {
                return tether.lifetime <= 0.0f;
            }),
        konvoyTethers_.end());

    for (KonvoyDome& dome : konvoyDomes_)
    {
        dome.lifetime -= dt;
        dome.flashTimer = std::max(0.0f, dome.flashTimer - dt);
        if (dome.lifetime > 0.0f && dome.flashTimer <= 0.0f)
        {
            dome.flashTimer = 1.0f;
            AddWorldEffect(dome.position, Vector3 { 0.0f, 0.0f, 1.0f }, konvoyColor, kKonvoyDomeVisualRadius, 0.16f, WorldEffectKind::Ring);
        }
        if (dome.lifetime <= 0.0f)
        {
            continue;
        }
        dome.chargeTimer += dt;
        if (dome.chargeTimer >= 1.0f)
        {
            dome.chargeTimer -= 1.0f;
            if (Player* owner = playerById(dome.ownerPlayerId))
            {
                const int detained = static_cast<int>(std::count_if(
                    players_.begin(), players_.end(), [&dome](const Player& target)
                    {
                        return target.IsAlive() && !target.IsEliminated()
                            && target.GetTeamId() != dome.ownerTeamId
                            && DistanceSquared(target.GetPosition(), dome.position)
                                < kKonvoyDomeVisualRadius * kKonvoyDomeVisualRadius;
                    }));
                owner->AddHeroUltimateCharge(std::min(2.0f, static_cast<float>(detained) * 0.5f));
            }
        }
        for (Player& target : players_)
        {
            if (!target.IsAlive() || target.IsEliminated() || target.GetTeamId() == dome.ownerTeamId)
            {
                continue;
            }
            Vector3 radial {
                target.GetPosition().x - dome.position.x,
                0.0f,
                target.GetPosition().z - dome.position.z
            };
            const float distance = Length2D(radial);
            if (distance <= 0.001f)
            {
                radial = Vector3 { 1.0f, 0.0f, 0.0f };
            }
            else
            {
                radial.x /= distance;
                radial.z /= distance;
            }
            const bool startedInside = std::find(
                dome.initiallyInsideEnemyIds.begin(), dome.initiallyInsideEnemyIds.end(), target.GetId())
                != dome.initiallyInsideEnemyIds.end();
            if (startedInside)
            {
                target.ApplyControlDebuff(0.18f, 0.72f, 0.78f, 0.70f);
                if (distance > kKonvoyDomeVisualRadius - 0.42f)
                {
                    Vector3 corrected = target.GetPosition();
                    corrected.x = dome.position.x + radial.x * (kKonvoyDomeVisualRadius - 0.45f);
                    corrected.z = dome.position.z + radial.z * (kKonvoyDomeVisualRadius - 0.45f);
                    target.Teleport(corrected, false);
                }
            }
            else if (distance < kKonvoyDomeVisualRadius + 0.42f)
            {
                Vector3 corrected = target.GetPosition();
                corrected.x = dome.position.x + radial.x * (kKonvoyDomeVisualRadius + 0.45f);
                corrected.z = dome.position.z + radial.z * (kKonvoyDomeVisualRadius + 0.45f);
                target.Teleport(corrected, false);
            }
        }
    }

    konvoyDomes_.erase(
        std::remove_if(
            konvoyDomes_.begin(),
            konvoyDomes_.end(),
            [](const KonvoyDome& dome)
            {
                return dome.lifetime <= 0.0f || dome.health <= 0;
            }),
        konvoyDomes_.end());

    konvoyIntruderMarks_.erase(
        std::remove_if(konvoyIntruderMarks_.begin(), konvoyIntruderMarks_.end(),
            [playerById](const KonvoyIntruderMark& mark)
            {
                const Player* owner = playerById(mark.ownerPlayerId);
                const Player* target = playerById(mark.targetPlayerId);
                return owner == nullptr || target == nullptr || owner->IsEliminated() || target->IsEliminated();
            }),
        konvoyIntruderMarks_.end());
}

void Game::UpdateLikhoBleeds(float dt)
{
    for (LikhoBlockCut& cut : likhoBlockCuts_)
    {
        cut.lifetime -= dt;
        const Block* block = world_.GetBlock(cut.position);
        if (block == nullptr || block->type != cut.blockType)
        {
            cut.lifetime = 0.0f;
        }
    }
    likhoBlockCuts_.erase(
        std::remove_if(likhoBlockCuts_.begin(), likhoBlockCuts_.end(),
            [](const LikhoBlockCut& cut) { return cut.lifetime <= 0.0f; }),
        likhoBlockCuts_.end());

    for (LikhoBleed& bleed : likhoBleeds_)
    {
        bleed.lifetime -= dt;
        bleed.tickTimer -= dt;
        if (bleed.tickTimer > 0.0f)
        {
            continue;
        }
        bleed.tickTimer = 1.0f;
        for (Player& target : players_)
        {
            if (target.GetId() != bleed.targetPlayerId)
            {
                continue;
            }
            if (!target.IsAlive() || target.IsEliminated())
            {
                bleed.lifetime = 0.0f;
                break;
            }
            const int damage = 2 + std::min(3, bleed.stacks);
            NoteDamageCredit(target.GetId(), bleed.ownerPlayerId, "кровотечением Лихо");
            target.Damage(damage);
            AddFloatingText("кровотечение -" + std::to_string(damage), target.GetPosition(), HeroAccentColor(HeroId::Likho));
            break;
        }
    }
    likhoBleeds_.erase(
        std::remove_if(likhoBleeds_.begin(), likhoBleeds_.end(), [](const LikhoBleed& bleed) { return bleed.lifetime <= 0.0f; }),
        likhoBleeds_.end());
}

void Game::UpdateSvidetelEffects(float dt)
{
    for (SvidetelPhaseBlock& phased : svidetelPhaseBlocks_)
    {
        phased.timer -= dt;
        const Vector3 center = world_.GridToWorld(phased.position);
        for (Player& player : players_)
        {
            if (player.IsAlive() && !player.IsEliminated()
                && std::fabs(player.GetPosition().x - center.x) < 0.72f
                && std::fabs(player.GetPosition().y - center.y) < 1.45f
                && std::fabs(player.GetPosition().z - center.z) < 0.72f)
            {
                player.ApplyControlDebuff(0.18f, 0.58f, 0.68f, 0.72f);
            }
        }
        if (phased.timer > 0.0f)
        {
            continue;
        }
        if (!world_.IsAir(phased.position))
        {
            // A newly placed block wins; never overwrite it with the old one.
            phased.timer = -1.0f;
            continue;
        }

        bool occupied = false;
        for (Player& player : players_)
        {
            if (!player.IsAlive() || player.IsEliminated()
                || std::fabs(player.GetPosition().x - center.x) >= 0.72f
                || std::fabs(player.GetPosition().y - center.y) >= 1.45f
                || std::fabs(player.GetPosition().z - center.z) >= 0.72f)
            {
                continue;
            }
            occupied = true;
            bool ejected = false;
            for (const Vector3 offset : { Vector3 { 1.2f, 0.0f, 0.0f }, Vector3 { -1.2f, 0.0f, 0.0f },
                    Vector3 { 0.0f, 0.0f, 1.2f }, Vector3 { 0.0f, 0.0f, -1.2f }, Vector3 { 0.0f, 1.2f, 0.0f } })
            {
                const Vector3 candidate {
                    player.GetPosition().x + offset.x,
                    player.GetPosition().y + offset.y,
                    player.GetPosition().z + offset.z
                };
                if (!world_.CollidesWithAABB(candidate, kPlayerCollisionHalfExtents))
                {
                    player.Teleport(candidate);
                    ejected = true;
                    break;
                }
            }
            if (!ejected)
            {
                phased.suffocationTimer -= dt;
                if (phased.suffocationTimer <= 0.0f)
                {
                    phased.suffocationTimer = 0.5f;
                    player.Damage(6);
                    AddFloatingText("SUFFOCATION -6", player.GetPosition(), HeroAccentColor(HeroId::Svidetel));
                }
                break;
            }
            occupied = false;
        }
        if (!occupied)
        {
            PlaceWorldBlock(phased.position, phased.block, true, BlockDeltaReason::PhaseRestore);
            AddWorldEffect(center, HeroAccentColor(HeroId::Svidetel), 0.28f, 0.35f);
            phased.timer = -1.0f;
        }
    }
    svidetelPhaseBlocks_.erase(
        std::remove_if(svidetelPhaseBlocks_.begin(), svidetelPhaseBlocks_.end(), [](const SvidetelPhaseBlock& phased) { return phased.timer < 0.0f; }),
        svidetelPhaseBlocks_.end());

    for (SvidetelEcho& echo : svidetelEchoes_)
    {
        echo.lifetime -= dt;
        echo.fireCooldown -= dt;
        echo.flashTimer = std::max(0.0f, echo.flashTimer - dt);
        if (echo.fireCooldown > 0.0f)
        {
            continue;
        }
        Player* target = nullptr;
        float bestDistance = echo.armed ? 100.0f : 64.0f;
        for (Player& candidate : players_)
        {
            if (!candidate.IsAlive() || candidate.IsEliminated() || candidate.GetTeamId() == echo.ownerTeamId)
            {
                continue;
            }
            const float distance = DistanceSquared(candidate.GetPosition(), echo.position);
            if (distance < bestDistance)
            {
                const Vector3 aimPoint { candidate.GetPosition().x, candidate.GetPosition().y + 0.65f, candidate.GetPosition().z };
                const Vector3 ray { aimPoint.x - echo.position.x, aimPoint.y - echo.position.y, aimPoint.z - echo.position.z };
                const float rayLength = Length(ray);
                const std::optional<RaycastHit> wall = world_.Raycast(echo.position, ray, rayLength);
                if (wall.has_value() && wall->distance < rayLength - 0.35f)
                {
                    continue;
                }
                bestDistance = distance;
                target = &candidate;
            }
        }
        echo.fireCooldown = echo.armed ? 1.35f : 2.0f;
        if (target == nullptr)
        {
            continue;
        }
        echo.lastTarget = target->GetPosition();
        echo.flashTimer = 0.28f;
        for (Player& owner : players_)
        {
            if (owner.GetId() == echo.ownerPlayerId)
            {
                owner.AddHeroUltimateCharge(echo.armed ? 3.0f : 5.0f);
                break;
            }
        }
        if (echo.armed)
        {
            const Vector3 origin { echo.position.x, echo.position.y + 0.72f, echo.position.z };
            const Vector3 targetPoint { target->GetPosition().x, target->GetPosition().y + 0.65f, target->GetPosition().z };
            const Vector3 direction = Normalize(Vector3 {
                targetPoint.x - origin.x, targetPoint.y - origin.y, targetPoint.z - origin.z });
            EnergyProjectile projectile {};
            projectile.id = NextProjectileId();
            projectile.position = origin;
            projectile.previousPosition = origin;
            projectile.startPosition = origin;
            projectile.ownerId = echo.ownerPlayerId;
            projectile.ownerTeamId = echo.ownerTeamId;
            projectile.velocity = Vector3 {
                direction.x * kBlasterTuning.baseSpeed,
                direction.y * kBlasterTuning.baseSpeed,
                direction.z * kBlasterTuning.baseSpeed };
            projectile.damage = kBlasterTuning.baseDamage;
            projectile.radius = 0.22f;
            projectile.gravity = kBlasterTuning.gravity;
            projectile.lifetime = kProjectilePhysicsTuning.maxLifetime;
            projectile.maxRange = kProjectilePhysicsTuning.maximumRange;
            projectile.kind = ProjectileKind::Blaster;
            projectile.airDragPerTick = kProjectilePhysicsTuning.arrowAirDragPerTick;
            projectile.affectedByDrag = true;
            projectiles_.push_back(projectile);
            AddWorldEffect(origin, direction, HeroAccentColor(HeroId::Svidetel), 0.38f, 0.22f, WorldEffectKind::Trail);
        }
    }
    svidetelEchoes_.erase(
        std::remove_if(svidetelEchoes_.begin(), svidetelEchoes_.end(), [](const SvidetelEcho& echo) { return echo.lifetime <= 0.0f || echo.health <= 0; }),
        svidetelEchoes_.end());
}

void Game::UpdatePassiveRegeneration(float dt)
{
    // Sudden death: the collapsed arena drains everyone instead of healing,
    // so the final brawl always resolves into a winner.
    if (coreCollapseTriggered_)
    {
        suddenDeathDecayTimer_ += dt;
        if (suddenDeathDecayTimer_ >= 2.5f)
        {
            suddenDeathDecayTimer_ = 0.0f;
            int bestScore = std::numeric_limits<int>::min();
            std::vector<int> tiedTeams;
            for (const Team& team : teams_)
            {
                if (!IsTeamActiveForMode(team.id))
                {
                    continue;
                }
                int score = 0;
                for (const Player& player : players_)
                {
                    if (player.GetTeamId() != team.id)
                    {
                        continue;
                    }
                    if (player.IsAlive() && !player.IsEliminated())
                    {
                        score += player.GetHealth() * 1000;
                    }
                }
                if (score > bestScore)
                {
                    bestScore = score;
                    tiedTeams.assign(1, team.id);
                }
                else if (score == bestScore)
                {
                    tiedTeams.push_back(team.id);
                }
            }
            if (!tiedTeams.empty())
            {
                suddenDeathTiebreakTeamId_ = tiedTeams[GetRandomValue(0, static_cast<int>(tiedTeams.size()) - 1)];
            }
            for (Player& player : players_)
            {
                if (!player.IsAlive() || player.IsEliminated())
                {
                    continue;
                }
                player.Damage(3);
                if (player.GetId() == localPlayerId_)
                {
                    damageFlashTimer_ = std::max(damageFlashTimer_, 0.35f);
                }
                if (!player.IsAlive())
                {
                    AddKillFeed(player.GetName() + " поглощен распадом арены", Color { 255, 118, 118, 255 }, 5.0f);
                }
            }
        }
        return;
    }

    passiveRegenTimer_ += dt;
    if (passiveRegenTimer_ < 2.0f)
    {
        return;
    }

    passiveRegenTimer_ = 0.0f;
    for (Player& player : players_)
    {
        if (!player.IsAlive() || player.IsEliminated() || player.GetHealth() >= player.GetMaxHealth())
        {
            continue;
        }

        player.Heal(5);
    }
}

void Game::UpdateBaseHealing(float dt)
{
    if (coreCollapseTriggered_)
    {
        return;
    }
    baseHealTimer_ += dt;
    if (baseHealTimer_ < 0.25f)
    {
        return;
    }

    baseHealTimer_ = 0.0f;
    for (Player& player : players_)
    {
        Team* team = FindTeam(player.GetTeamId());
        if (team == nullptr || !shop_.IsPlayerInShop(player, *team) || player.GetHealth() >= player.GetMaxHealth())
        {
            continue;
        }

        player.Heal(3 + team->healAuraLevel * 2);
        if (HasLocalCamera(ControlKindForPlayer(player)))
        {
            AddWorldEffect(player.GetPosition(), Color { 128, 238, 166, 255 }, 0.18f, 0.18f);
        }
    }
}

void Game::UpdateFeedback(float dt)
{
    hitMarkerTimer_ = std::max(0.0f, hitMarkerTimer_ - dt);
    damageFlashTimer_ = std::max(0.0f, damageFlashTimer_ - dt);
    localDeathOverlayTimer_ = std::max(0.0f, localDeathOverlayTimer_ - dt);
    particles_.SetQuality(effectsQuality_);
    particles_.Update(dt);

    for (WorldEffect& effect : worldEffects_)
    {
        effect.age += dt;
    }
    worldEffects_.erase(
        std::remove_if(
            worldEffects_.begin(),
            worldEffects_.end(),
            [](const WorldEffect& effect)
            {
                return effect.age >= effect.lifetime;
            }),
        worldEffects_.end());

    for (FloatingText& text : floatingTexts_)
    {
        text.age += dt;
    }
    floatingTexts_.erase(
        std::remove_if(
            floatingTexts_.begin(),
            floatingTexts_.end(),
            [](const FloatingText& text)
            {
                return text.age >= text.lifetime;
            }),
        floatingTexts_.end());

    for (EventMessage& event : eventMessages_)
    {
        event.age += dt;
    }
    eventMessages_.erase(
        std::remove_if(
            eventMessages_.begin(),
            eventMessages_.end(),
            [](const EventMessage& event)
            {
                return event.age >= event.lifetime;
            }),
        eventMessages_.end());

    for (KillFeedEntry& entry : killFeed_)
    {
        entry.age += dt;
    }
    killFeed_.erase(
        std::remove_if(
            killFeed_.begin(),
            killFeed_.end(),
            [](const KillFeedEntry& entry)
            {
                return entry.age >= entry.lifetime;
            }),
        killFeed_.end());
}

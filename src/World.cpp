#include "World.h"

#include <algorithm>
#include <cmath>

namespace
{
float Length(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vector3 Normalize(Vector3 value)
{
    const float length = Length(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, -1.0f };
    }

    return Vector3 { value.x / length, value.y / length, value.z / length };
}

bool Overlaps(float minA, float maxA, float minB, float maxB)
{
    return minA <= maxB && maxA >= minB;
}
}

void World::Clear()
{
    blocks_.clear();
}

bool World::PlaceBlock(const GridPos& pos, const Block& block, bool allowReplace)
{
    if (block.type == BlockType::Air)
    {
        return RemoveBlock(pos);
    }

    if (!allowReplace && !IsAir(pos))
    {
        return false;
    }

    blocks_[pos] = block;
    return true;
}

bool World::BreakBlock(const GridPos& pos, int attackerTeam)
{
    (void)attackerTeam;

    auto it = blocks_.find(pos);
    if (it == blocks_.end() || !it->second.breakable)
    {
        return false;
    }

    blocks_.erase(it);
    return true;
}

bool World::RemoveBlock(const GridPos& pos)
{
    return blocks_.erase(pos) > 0;
}

const Block* World::GetBlock(const GridPos& pos) const
{
    const auto it = blocks_.find(pos);
    if (it == blocks_.end())
    {
        return nullptr;
    }

    return &it->second;
}

bool World::IsAir(const GridPos& pos) const
{
    return GetBlock(pos) == nullptr;
}

bool World::IsSolid(const GridPos& pos) const
{
    const Block* block = GetBlock(pos);
    return block != nullptr && IsCollisionBlock(block->type);
}

GridPos World::WorldToGrid(Vector3 position) const
{
    return GridPos {
        static_cast<int>(std::floor(position.x + 0.5f)),
        static_cast<int>(std::floor(position.y + 0.5f)),
        static_cast<int>(std::floor(position.z + 0.5f))
    };
}

Vector3 World::GridToWorld(const GridPos& pos) const
{
    return Vector3 {
        static_cast<float>(pos.x),
        static_cast<float>(pos.y),
        static_cast<float>(pos.z)
    };
}

bool World::CollidesWithAABB(Vector3 center, Vector3 halfExtents) const
{
    const int minX = static_cast<int>(std::floor(center.x - halfExtents.x - 0.5f));
    const int maxX = static_cast<int>(std::ceil(center.x + halfExtents.x + 0.5f));
    const int minY = static_cast<int>(std::floor(center.y - halfExtents.y - 0.5f));
    const int maxY = static_cast<int>(std::ceil(center.y + halfExtents.y + 0.5f));
    const int minZ = static_cast<int>(std::floor(center.z - halfExtents.z - 0.5f));
    const int maxZ = static_cast<int>(std::ceil(center.z + halfExtents.z + 0.5f));

    for (int x = minX; x <= maxX; ++x)
    {
        for (int y = minY; y <= maxY; ++y)
        {
            for (int z = minZ; z <= maxZ; ++z)
            {
                const GridPos pos { x, y, z };
                if (!IsSolid(pos))
                {
                    continue;
                }

                const float blockMinX = static_cast<float>(x) - 0.5f;
                const float blockMaxX = static_cast<float>(x) + 0.5f;
                const float blockMinY = static_cast<float>(y) - 0.5f;
                const float blockMaxY = static_cast<float>(y) + 0.5f;
                const float blockMinZ = static_cast<float>(z) - 0.5f;
                const float blockMaxZ = static_cast<float>(z) + 0.5f;

                if (Overlaps(center.x - halfExtents.x, center.x + halfExtents.x, blockMinX, blockMaxX)
                    && Overlaps(center.y - halfExtents.y, center.y + halfExtents.y, blockMinY, blockMaxY)
                    && Overlaps(center.z - halfExtents.z, center.z + halfExtents.z, blockMinZ, blockMaxZ))
                {
                    return true;
                }
            }
        }
    }

    return false;
}

std::optional<RaycastHit> World::Raycast(Vector3 origin, Vector3 direction, float maxDistance) const
{
    const Vector3 dir = Normalize(direction);
    GridPos previous = WorldToGrid(origin);

    for (float distance = 0.0f; distance <= maxDistance; distance += 0.08f)
    {
        const Vector3 point {
            origin.x + dir.x * distance,
            origin.y + dir.y * distance,
            origin.z + dir.z * distance
        };

        const GridPos current = WorldToGrid(point);
        const Block* block = GetBlock(current);
        if (block != nullptr && block->type != BlockType::Air)
        {
            GridPos normal {
                previous.x - current.x,
                previous.y - current.y,
                previous.z - current.z
            };
            const int normalAxes = std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
            if (normalAxes != 1)
            {
                normal = GridPos {};
                if (std::fabs(dir.x) >= std::fabs(dir.y) && std::fabs(dir.x) >= std::fabs(dir.z))
                {
                    normal.x = dir.x > 0.0f ? -1 : 1;
                }
                else if (std::fabs(dir.y) >= std::fabs(dir.z))
                {
                    normal.y = dir.y > 0.0f ? -1 : 1;
                }
                else
                {
                    normal.z = dir.z > 0.0f ? -1 : 1;
                }
            }
            return RaycastHit { current, previous, normal, *block, distance };
        }

        previous = current;
    }

    return std::nullopt;
}

void World::AddIsland(Vector3 center, int halfSize, int y)
{
    for (int x = -halfSize; x <= halfSize; ++x)
    {
        for (int z = -halfSize; z <= halfSize; ++z)
        {
            const float distance = std::sqrt(static_cast<float>(x * x + z * z));
            if (distance <= static_cast<float>(halfSize) + 0.25f)
            {
                PlaceBlock(
                    GridPos {
                        static_cast<int>(std::round(center.x)) + x,
                        y,
                        static_cast<int>(std::round(center.z)) + z },
                    Block { BlockType::Solid, -1, false },
                    true);
            }
        }
    }
}

void World::AddBridge(const GridPos& from, const GridPos& to, int y)
{
    const int dx = (to.x > from.x) ? 1 : (to.x < from.x ? -1 : 0);
    const int dz = (to.z > from.z) ? 1 : (to.z < from.z ? -1 : 0);
    GridPos pos { from.x, y, from.z };

    while (pos.x != to.x || pos.z != to.z)
    {
        PlaceBlock(pos, Block { BlockType::Solid, -1, false }, true);
        if (pos.x != to.x)
        {
            pos.x += dx;
        }
        if (pos.z != to.z)
        {
            pos.z += dz;
        }
    }

    PlaceBlock(GridPos { to.x, y, to.z }, Block { BlockType::Solid, -1, false }, true);
}

const World::BlockMap& World::GetBlocks() const
{
    return blocks_;
}

bool World::IsCollisionBlock(BlockType type)
{
    return type == BlockType::Solid
        || type == BlockType::GrassBlock
        || type == BlockType::DirtBlock
        || type == BlockType::LeafBlock
        || type == BlockType::TeamBlock
        || type == BlockType::WoodBlock
        || type == BlockType::WoolBlock
        || type == BlockType::StoneBlock
        || type == BlockType::ObsidianBlock
        || type == BlockType::EnergyGlassBlock
        || type == BlockType::SpringBlock
        || type == BlockType::StickyBlock
        || type == BlockType::ExplosiveBlock
        || type == BlockType::SpikeBlock
        || type == BlockType::LavaBlock
        || type == BlockType::IceBlock
        || type == BlockType::ResourceGenerator
        || type == BlockType::EnergyCoreBlock;
}

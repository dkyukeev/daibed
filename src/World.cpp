#include "World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
int FloorDiv(int value, int divisor)
{
    const int quotient = value / divisor;
    const int remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
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
        return Vector3 { 0.0f, 0.0f, -1.0f };
    }

    return Vector3 { value.x / length, value.y / length, value.z / length };
}

bool Overlaps(float minA, float maxA, float minB, float maxB)
{
    return minA <= maxB && maxA >= minB;
}

struct LocalCollisionBox
{
    Vector3 min {};
    Vector3 max {};
};

int CollisionBoxesFor(const Block& block, std::array<LocalCollisionBox, 2>& boxes)
{
    constexpr LocalCollisionBox kFull { Vector3 { -0.5f, -0.5f, -0.5f }, Vector3 { 0.5f, 0.5f, 0.5f } };
    switch (block.type)
    {
    case BlockType::StoneSlabBlock:
    case BlockType::StoneBrickSlabBlock:
    case BlockType::BirchSlabBlock:
    {
        const bool upper = (block.variant & 0x8) != 0;
        boxes[0] = upper
            ? LocalCollisionBox { Vector3 { -0.5f, 0.0f, -0.5f }, Vector3 { 0.5f, 0.5f, 0.5f } }
            : LocalCollisionBox { Vector3 { -0.5f, -0.5f, -0.5f }, Vector3 { 0.5f, 0.0f, 0.5f } };
        return 1;
    }
    case BlockType::StoneBrickStairsBlock:
    case BlockType::BirchStairsBlock:
    {
        const bool upsideDown = (block.variant & 0x4) != 0;
        // Legacy Minecraft metadata 0..3 maps to the four horizontal backs.
        // Two boxes give a genuine half-height step collider without forcing a
        // full cube into the imported castle's stairs.
        boxes[0] = upsideDown
            ? LocalCollisionBox { Vector3 { -0.5f, 0.0f, -0.5f }, Vector3 { 0.5f, 0.5f, 0.5f } }
            : LocalCollisionBox { Vector3 { -0.5f, -0.5f, -0.5f }, Vector3 { 0.5f, 0.0f, 0.5f } };
        const float low = upsideDown ? -0.5f : 0.0f;
        const float high = upsideDown ? 0.0f : 0.5f;
        switch (block.variant & 0x3)
        {
        case 0: boxes[1] = LocalCollisionBox { Vector3 { -0.5f, low, -0.5f }, Vector3 { 0.0f, high, 0.5f } }; break;
        case 1: boxes[1] = LocalCollisionBox { Vector3 { 0.0f, low, -0.5f }, Vector3 { 0.5f, high, 0.5f } }; break;
        case 2: boxes[1] = LocalCollisionBox { Vector3 { -0.5f, low, -0.5f }, Vector3 { 0.5f, high, 0.0f } }; break;
        default: boxes[1] = LocalCollisionBox { Vector3 { -0.5f, low, 0.0f }, Vector3 { 0.5f, high, 0.5f } }; break;
        }
        return 2;
    }
    case BlockType::IronBarsBlock:
        // Crossed thin bars retain a small, tangible obstacle while allowing
        // paths through the gaps instead of behaving like a stone wall.
        boxes[0] = LocalCollisionBox { Vector3 { -0.08f, -0.5f, -0.5f }, Vector3 { 0.08f, 0.5f, 0.5f } };
        boxes[1] = LocalCollisionBox { Vector3 { -0.5f, -0.5f, -0.08f }, Vector3 { 0.5f, 0.5f, 0.08f } };
        return 2;
    default:
        boxes[0] = kFull;
        return 1;
    }
}
}

void World::Clear()
{
    for (const auto& entry : blocks_)
    {
        dirtyRenderChunks_.insert(RenderChunkForBlock(entry.first));
    }
    blocks_.clear();
    ++renderRevision_;
    navigationDirtyHistory_.clear();
    navigationHistoryFloorRevision_ = renderRevision_;
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

    const auto existing = blocks_.find(pos);
    if (existing != blocks_.end()
        && existing->second.type == block.type
        && existing->second.teamId == block.teamId
        && existing->second.breakable == block.breakable
        && existing->second.variant == block.variant)
    {
        return true;
    }

    blocks_[pos] = block;
    MarkRenderDirty(pos);
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
    MarkRenderDirty(pos);
    return true;
}

bool World::RemoveBlock(const GridPos& pos)
{
    if (blocks_.erase(pos) == 0)
    {
        return false;
    }
    MarkRenderDirty(pos);
    return true;
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

                std::array<LocalCollisionBox, 2> boxes {};
                const int boxCount = CollisionBoxesFor(*GetBlock(pos), boxes);
                for (int boxIndex = 0; boxIndex < boxCount; ++boxIndex)
                {
                    const LocalCollisionBox& box = boxes[boxIndex];
                    const float blockMinX = static_cast<float>(x) + box.min.x;
                    const float blockMaxX = static_cast<float>(x) + box.max.x;
                    const float blockMinY = static_cast<float>(y) + box.min.y;
                    const float blockMaxY = static_cast<float>(y) + box.max.y;
                    const float blockMinZ = static_cast<float>(z) + box.min.z;
                    const float blockMaxZ = static_cast<float>(z) + box.max.z;

                    if (Overlaps(center.x - halfExtents.x, center.x + halfExtents.x, blockMinX, blockMaxX)
                        && Overlaps(center.y - halfExtents.y, center.y + halfExtents.y, blockMinY, blockMaxY)
                        && Overlaps(center.z - halfExtents.z, center.z + halfExtents.z, blockMinZ, blockMaxZ))
                    {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

std::optional<RaycastHit> World::Raycast(Vector3 origin, Vector3 direction, float maxDistance) const
{
    if (maxDistance < 0.0f)
    {
        return std::nullopt;
    }

    const Vector3 dir = Normalize(direction);
    GridPos cell = WorldToGrid(origin);
    GridPos previous = cell;
    GridPos normal {};
    float distance = 0.0f;

    const auto fallbackNormal = [&dir]()
    {
        GridPos fallback {};
        if (std::fabs(dir.x) >= std::fabs(dir.y) && std::fabs(dir.x) >= std::fabs(dir.z))
        {
            fallback.x = dir.x > 0.0f ? -1 : 1;
        }
        else if (std::fabs(dir.y) >= std::fabs(dir.z))
        {
            fallback.y = dir.y > 0.0f ? -1 : 1;
        }
        else
        {
            fallback.z = dir.z > 0.0f ? -1 : 1;
        }
        return fallback;
    };

    const auto checkCell = [&]() -> std::optional<RaycastHit>
    {
        const Block* block = GetBlock(cell);
        if (block == nullptr || block->type == BlockType::Air)
        {
            return std::nullopt;
        }

        GridPos hitNormal = normal;
        const int normalAxes = std::abs(hitNormal.x) + std::abs(hitNormal.y) + std::abs(hitNormal.z);
        if (normalAxes != 1)
        {
            hitNormal = fallbackNormal();
        }
        return RaycastHit { cell, previous, hitNormal, *block, distance };
    };

    if (const std::optional<RaycastHit> hit = checkCell())
    {
        return hit;
    }

    const float infinity = std::numeric_limits<float>::infinity();
    const int stepX = dir.x > 0.0f ? 1 : (dir.x < 0.0f ? -1 : 0);
    const int stepY = dir.y > 0.0f ? 1 : (dir.y < 0.0f ? -1 : 0);
    const int stepZ = dir.z > 0.0f ? 1 : (dir.z < 0.0f ? -1 : 0);

    const auto initialTMax = [infinity](float originCoord, int cellCoord, float dirCoord, int step)
    {
        if (step == 0)
        {
            return infinity;
        }
        const float boundary = static_cast<float>(cellCoord) + (step > 0 ? 0.5f : -0.5f);
        return std::max(0.0f, (boundary - originCoord) / dirCoord);
    };

    const auto tDelta = [infinity](float dirCoord, int step)
    {
        return step == 0 ? infinity : 1.0f / std::fabs(dirCoord);
    };

    float tMaxX = initialTMax(origin.x, cell.x, dir.x, stepX);
    float tMaxY = initialTMax(origin.y, cell.y, dir.y, stepY);
    float tMaxZ = initialTMax(origin.z, cell.z, dir.z, stepZ);
    const float tDeltaX = tDelta(dir.x, stepX);
    const float tDeltaY = tDelta(dir.y, stepY);
    const float tDeltaZ = tDelta(dir.z, stepZ);

    while (distance <= maxDistance)
    {
        previous = cell;
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ)
        {
            cell.x += stepX;
            distance = tMaxX;
            tMaxX += tDeltaX;
            normal = GridPos { -stepX, 0, 0 };
        }
        else if (tMaxY <= tMaxZ)
        {
            cell.y += stepY;
            distance = tMaxY;
            tMaxY += tDeltaY;
            normal = GridPos { 0, -stepY, 0 };
        }
        else
        {
            cell.z += stepZ;
            distance = tMaxZ;
            tMaxZ += tDeltaZ;
            normal = GridPos { 0, 0, -stepZ };
        }

        if (distance > maxDistance)
        {
            break;
        }
        if (const std::optional<RaycastHit> hit = checkCell())
        {
            return hit;
        }
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

std::uint64_t World::GetRenderRevision() const
{
    return renderRevision_;
}

NavigationChangeQuery World::QueryNavigationChanges(
    std::uint64_t sinceRevision,
    GridPos minimum,
    GridPos maximum,
    int padding) const
{
    if (sinceRevision >= renderRevision_) return NavigationChangeQuery::NoChanges;
    if (sinceRevision < navigationHistoryFloorRevision_)
    {
        return NavigationChangeQuery::HistoryUnavailable;
    }
    padding = std::max(0, padding);
    minimum.x -= padding;
    minimum.y -= padding;
    minimum.z -= padding;
    maximum.x += padding;
    maximum.y += padding;
    maximum.z += padding;
    for (auto it = navigationDirtyHistory_.rbegin(); it != navigationDirtyHistory_.rend(); ++it)
    {
        if (it->revision <= sinceRevision) break;
        const GridPos pos = it->pos;
        if (pos.x >= minimum.x && pos.x <= maximum.x
            && pos.y >= minimum.y && pos.y <= maximum.y
            && pos.z >= minimum.z && pos.z <= maximum.z)
        {
            return NavigationChangeQuery::Intersects;
        }
    }
    return NavigationChangeQuery::Disjoint;
}

std::vector<GridPos> World::TakeDirtyRenderChunks() const
{
    std::vector<GridPos> result;
    result.reserve(dirtyRenderChunks_.size());
    for (const GridPos& chunk : dirtyRenderChunks_)
    {
        result.push_back(chunk);
    }
    dirtyRenderChunks_.clear();
    return result;
}

GridPos World::RenderChunkForBlock(const GridPos& pos)
{
    return GridPos {
        FloorDiv(pos.x, kRenderChunkSize),
        FloorDiv(pos.y, kRenderChunkSize),
        FloorDiv(pos.z, kRenderChunkSize)
    };
}

void World::MarkRenderDirty(const GridPos& pos)
{
    ++renderRevision_;
    navigationDirtyHistory_.push_back(NavigationDirtyCell { pos, renderRevision_ });
    if (navigationDirtyHistory_.size() > kNavigationDirtyHistoryCapacity)
    {
        navigationHistoryFloorRevision_ = std::max(
            navigationHistoryFloorRevision_, navigationDirtyHistory_.front().revision);
        navigationDirtyHistory_.pop_front();
    }
    // Baked ambient occlusion samples the full 3x3x3 neighborhood around a
    // block, so a change on a chunk boundary can darken faces in diagonally
    // adjacent chunks, not just the face-adjacent ones.
    for (int dx = -1; dx <= 1; ++dx)
    {
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dz = -1; dz <= 1; ++dz)
            {
                dirtyRenderChunks_.insert(RenderChunkForBlock(GridPos {
                    pos.x + dx,
                    pos.y + dy,
                    pos.z + dz
                }));
            }
        }
    }
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
        || type == BlockType::SmoothStoneBlock
        || type == BlockType::DarkBrickBlock
        || type == BlockType::LightBrickBlock
        || type == BlockType::MetalBlock
        || type == BlockType::GlowBlock
        || type == BlockType::PlankBlock
        || type == BlockType::DecorativeTileBlock
        || type == BlockType::TrimBlock
        || type == BlockType::CobblestoneBlock
        || type == BlockType::AndesiteBlock
        || type == BlockType::PolishedAndesiteBlock
        || type == BlockType::StoneBrickBlock
        || type == BlockType::ChiseledStoneBrickBlock
        || type == BlockType::StoneSlabBlock
        || type == BlockType::StoneBrickSlabBlock
        || type == BlockType::StoneBrickStairsBlock
        || type == BlockType::BirchPlankBlock
        || type == BlockType::BirchSlabBlock
        || type == BlockType::BirchStairsBlock
        || type == BlockType::ColoredGlassBlock
        || type == BlockType::ColoredClayBlock
        || type == BlockType::LapisBlock
        || type == BlockType::DiamondBlock
        || type == BlockType::EmeraldBlock
        || type == BlockType::GoldBlock
        || type == BlockType::IronBarsBlock
        || type == BlockType::BarrierBlock
        || type == BlockType::ResourceGenerator
        || type == BlockType::TeamChestBlock
        || type == BlockType::EnergyCoreBlock;
}

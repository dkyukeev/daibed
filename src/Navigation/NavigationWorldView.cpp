#include "Navigation/NavigationWorldView.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
struct CachedBlockLookup
{
    GridPos pos {};
    const Block* block = nullptr;
    std::uint64_t revision = 0;
    std::uint32_t generation = 0;
};

struct ThreadBlockLookupStorage
{
    std::array<CachedBlockLookup, 4096> entries {};
    std::uint32_t generation = 0;
};

thread_local ThreadBlockLookupStorage gBlockLookupStorage;

std::uint32_t AcquireBlockLookupGeneration()
{
    if (++gBlockLookupStorage.generation == 0)
    {
        for (CachedBlockLookup& entry : gBlockLookupStorage.entries) entry.generation = 0;
        gBlockLookupStorage.generation = 1;
    }
    return gBlockLookupStorage.generation;
}

float Distance(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}

NavigationWorldView::NavigationWorldView(
    const World& world,
    int agentTeamId,
    std::vector<NavigationActor> actors,
    const ThreatMap* threatMap)
    : world_(world),
      agentTeamId_(agentTeamId),
      actors_(std::move(actors)),
      threatMap_(threatMap),
      blockLookupGeneration_(AcquireBlockLookupGeneration())
{
    std::sort(actors_.begin(), actors_.end(), [](const NavigationActor& lhs, const NavigationActor& rhs)
    {
        if (lhs.playerId != rhs.playerId)
        {
            return lhs.playerId < rhs.playerId;
        }
        if (lhs.teamId != rhs.teamId)
        {
            return lhs.teamId < rhs.teamId;
        }
        if (lhs.position.x != rhs.position.x)
        {
            return lhs.position.x < rhs.position.x;
        }
        return lhs.position.z < rhs.position.z;
    });
}

std::uint64_t NavigationWorldView::WorldRevision() const noexcept
{
    return world_.GetRenderRevision();
}

NavigationChangeQuery NavigationWorldView::QueryNavigationChanges(
    std::uint64_t sinceRevision,
    GridPos minimum,
    GridPos maximum,
    int padding) const
{
    return world_.QueryNavigationChanges(sinceRevision, minimum, maximum, padding);
}

const Block* NavigationWorldView::GetBlock(const GridPos& pos) const
{
    std::size_t hash = GridPosHash {}(pos);
    hash ^= hash >> 16U;
    CachedBlockLookup& entry = gBlockLookupStorage.entries[
        hash & (gBlockLookupStorage.entries.size() - 1U)];
    const std::uint64_t revision = world_.GetRenderRevision();
    if (entry.generation == blockLookupGeneration_
        && entry.revision == revision && entry.pos == pos)
    {
        return entry.block;
    }
    entry.pos = pos;
    entry.block = world_.GetBlock(pos);
    entry.revision = revision;
    entry.generation = blockLookupGeneration_;
    return entry.block;
}

NavVoxel NavigationWorldView::VoxelAt(const GridPos& pos) const
{
    const Block* block = GetBlock(pos);
    if (block == nullptr || block->type == BlockType::Air)
    {
        return NavVoxel::Air;
    }
    if (block->type == BlockType::LavaBlock || block->type == BlockType::SpikeBlock)
    {
        return NavVoxel::Hazard;
    }
    if (block->type == BlockType::EnergyCoreBlock)
    {
        return block->teamId == agentTeamId_ ? NavVoxel::FriendlyCore : NavVoxel::EnemyCore;
    }
    if (block->type == BlockType::EnergyGlassBlock && block->breakable)
    {
        return NavVoxel::Temporary;
    }
    if (!block->breakable || !IsBreakableByPlayers(block->type))
    {
        return NavVoxel::SolidUnbreakable;
    }
    const float seconds = BreakSeconds(block->type, 0);
    if (seconds <= 1.5f) return NavVoxel::BreakableCheap;
    if (seconds <= 4.0f) return NavVoxel::BreakableMedium;
    return NavVoxel::BreakableExpensive;
}

bool NavigationWorldView::IsAir(const GridPos& pos) const
{
    return GetBlock(pos) == nullptr;
}

bool NavigationWorldView::IsSolid(const GridPos& pos) const
{
    const Block* block = GetBlock(pos);
    return block != nullptr && World::IsCollisionBlock(block->type);
}

bool NavigationWorldView::IsHazard(const GridPos& support) const
{
    const Block* block = GetBlock(support);
    return block != nullptr
        && (block->type == BlockType::LavaBlock || block->type == BlockType::SpikeBlock);
}

bool NavigationWorldView::IsBreakableObstacle(
    const GridPos& pos,
    const NavigationProfile& profile) const
{
    if (!profile.canBreakBlocks)
    {
        return false;
    }
    const Block* block = GetBlock(pos);
    if (block == nullptr
        || !IsSolid(pos)
        || !block->breakable
        || !IsBreakableByPlayers(block->type))
    {
        return false;
    }
    return profile.allowBreakOwnTeamBlocks
        || block->teamId < 0
        || block->teamId != agentTeamId_;
}

bool NavigationWorldView::IsSupported(const GridPos& support) const
{
    return IsSolid(support);
}

bool NavigationWorldView::IsSupported(const NavigationState& state) const
{
    return IsSolid(state.support) || state.consecutiveBridgeBlocks > 0;
}

int NavigationWorldView::CountBlockingBodyCells(
    const GridPos& support,
    const NavigationProfile& profile,
    GridPos* firstBlocker,
    int stopAfter) const
{
    const int horizontalRadius = std::max(
        0,
        static_cast<int>(std::ceil(std::max(0.0f, profile.bodyHalfWidth - 0.49f))));
    const int clearanceBlocks = std::max(
        1,
        static_cast<int>(std::ceil(profile.bodyHalfHeight * 2.0f)));

    int count = 0;
    stopAfter = std::max(1, stopAfter);
    for (int dy = 1; dy <= clearanceBlocks; ++dy)
    {
        for (int dx = -horizontalRadius; dx <= horizontalRadius; ++dx)
        {
            for (int dz = -horizontalRadius; dz <= horizontalRadius; ++dz)
            {
                const GridPos cell { support.x + dx, support.y + dy, support.z + dz };
                if (IsSolid(cell))
                {
                    if (count == 0 && firstBlocker != nullptr) *firstBlocker = cell;
                    if (++count >= stopAfter) return count;
                }
            }
        }
    }
    return count;
}

bool NavigationWorldView::IsBodyClear(
    const GridPos& support,
    const NavigationProfile& profile) const
{
    return CountBlockingBodyCells(support, profile, nullptr, 1) == 0;
}

GridPos NavigationWorldView::WorldToGrid(Vector3 position) const
{
    return world_.WorldToGrid(position);
}

Vector3 NavigationWorldView::GridToWorld(const GridPos& pos) const
{
    return world_.GridToWorld(pos);
}

Vector3 NavigationWorldView::SupportCenter(
    const GridPos& support,
    float bodyCenterAboveSupport) const
{
    const Vector3 center = world_.GridToWorld(support);
    return Vector3 { center.x, center.y + bodyCenterAboveSupport, center.z };
}

std::optional<GridPos> NavigationWorldView::FindSupport(
    Vector3 bodyCenter,
    int maxDropBlocks,
    int maxRiseBlocks,
    float bodyCenterAboveSupport) const
{
    const GridPos expected = world_.WorldToGrid(Vector3 {
        bodyCenter.x,
        bodyCenter.y - bodyCenterAboveSupport,
        bodyCenter.z
    });
    if (IsSolid(expected))
    {
        return expected;
    }

    const int down = std::max(0, maxDropBlocks);
    const int up = std::max(0, maxRiseBlocks);
    for (int distance = 1; distance <= std::max(down, up); ++distance)
    {
        if (distance <= down)
        {
            const GridPos candidate { expected.x, expected.y - distance, expected.z };
            if (IsSolid(candidate))
            {
                return candidate;
            }
        }
        if (distance <= up)
        {
            const GridPos candidate { expected.x, expected.y + distance, expected.z };
            if (IsSolid(candidate))
            {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

bool NavigationWorldView::HasLineOfSight(
    Vector3 from,
    Vector3 to,
    std::optional<GridPos> allowedHitBlock) const
{
    const Vector3 direction { to.x - from.x, to.y - from.y, to.z - from.z };
    const float distance = Distance(from, to);
    if (distance <= 0.001f)
    {
        return true;
    }
    const std::optional<RaycastHit> hit = world_.Raycast(from, direction, distance);
    if (!hit.has_value())
    {
        return true;
    }
    return allowedHitBlock.has_value() && hit->block == *allowedHitBlock;
}

bool NavigationWorldView::HasLineOfSightFromSupport(
    const GridPos& support,
    Vector3 to,
    std::optional<GridPos> allowedHitBlock,
    float eyeHeightAboveSupport) const
{
    const Vector3 base = world_.GridToWorld(support);
    return HasLineOfSight(
        Vector3 { base.x, base.y + eyeHeightAboveSupport, base.z },
        to,
        allowedHitBlock);
}

float NavigationWorldView::ThreatCostAt(
    const GridPos& support,
    float bodyCenterAboveSupport) const
{
    const Vector3 center = SupportCenter(support, bodyCenterAboveSupport);
    float result = 0.0f;
    if (threatMap_ != nullptr)
    {
        result += threatMap_->CostAt(center);
    }
    else
    {
        for (const NavigationActor& actor : actors_)
        {
            if (!actor.alive
                || actor.teamId == agentTeamId_
                || actor.threatRadius <= 0.0f
                || actor.threatCost <= 0.0f)
            {
                continue;
            }
            const float distance = Distance(center, actor.position);
            if (distance < actor.threatRadius)
            {
                const float influence = 1.0f - distance / actor.threatRadius;
                result += actor.threatCost * influence * influence;
            }
        }
    }

    const Block* supportBlock = GetBlock(support);
    if (supportBlock != nullptr)
    {
        if (supportBlock->type == BlockType::LavaBlock)
        {
            result += 24.0f;
        }
        else if (supportBlock->type == BlockType::SpikeBlock)
        {
            result += 14.0f;
        }
        else if (supportBlock->type == BlockType::IceBlock)
        {
            result += 0.8f;
        }
    }
    return result;
}

const NavigationActor* NavigationWorldView::FindActor(int playerId) const
{
    const auto found = std::lower_bound(
        actors_.begin(),
        actors_.end(),
        playerId,
        [](const NavigationActor& actor, int id) { return actor.playerId < id; });
    return found != actors_.end() && found->playerId == playerId ? &*found : nullptr;
}

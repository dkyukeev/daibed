#pragma once

#include "Navigation/NavigationProfile.h"
#include "Navigation/ThreatMap.h"
#include "World.h"

#include <cstdint>
#include <optional>
#include <vector>

class NavigationWorldView
{
public:
    explicit NavigationWorldView(
        const World& world,
        int agentTeamId = -1,
        std::vector<NavigationActor> actors = {},
        const ThreatMap* threatMap = nullptr);

    const World& GetWorld() const noexcept { return world_; }
    int AgentTeamId() const noexcept { return agentTeamId_; }
    std::uint64_t WorldRevision() const noexcept;
    NavigationChangeQuery QueryNavigationChanges(
        std::uint64_t sinceRevision,
        GridPos minimum,
        GridPos maximum,
        int padding = 0) const;

    const Block* GetBlock(const GridPos& pos) const;
    NavVoxel VoxelAt(const GridPos& pos) const;
    bool IsAir(const GridPos& pos) const;
    bool IsSolid(const GridPos& pos) const;
    bool IsHazard(const GridPos& support) const;
    bool IsBreakableObstacle(const GridPos& pos, const NavigationProfile& profile) const;

    bool IsSupported(const GridPos& support) const;
    bool IsSupported(const NavigationState& state) const;
    bool IsBodyClear(const GridPos& support, const NavigationProfile& profile) const;
    // Counts only up to stopAfter and optionally returns the first blocker.
    // This is the hot path used by A*: unlike the old vector-returning helper,
    // it performs no allocation for every successor candidate.
    int CountBlockingBodyCells(
        const GridPos& support,
        const NavigationProfile& profile,
        GridPos* firstBlocker = nullptr,
        int stopAfter = 2) const;

    GridPos WorldToGrid(Vector3 position) const;
    Vector3 GridToWorld(const GridPos& pos) const;
    Vector3 SupportCenter(const GridPos& support, float bodyCenterAboveSupport = 1.40f) const;
    std::optional<GridPos> FindSupport(
        Vector3 bodyCenter,
        int maxDropBlocks = 4,
        int maxRiseBlocks = 1,
        float bodyCenterAboveSupport = 1.40f) const;

    bool HasLineOfSight(
        Vector3 from,
        Vector3 to,
        std::optional<GridPos> allowedHitBlock = std::nullopt) const;
    bool HasLineOfSightFromSupport(
        const GridPos& support,
        Vector3 to,
        std::optional<GridPos> allowedHitBlock = std::nullopt,
        float eyeHeightAboveSupport = 1.78f) const;

    float ThreatCostAt(const GridPos& support, float bodyCenterAboveSupport = 1.40f) const;
    const NavigationActor* FindActor(int playerId) const;
    const std::vector<NavigationActor>& Actors() const noexcept { return actors_; }

private:
    const World& world_;
    int agentTeamId_ = -1;
    std::vector<NavigationActor> actors_;
    const ThreatMap* threatMap_ = nullptr;
    // The large direct-map storage is thread-local rather than embedded here:
    // NavigationWorldView is intentionally cheap to create every bot update
    // and must not consume ~100 KiB of stack per view.
    std::uint32_t blockLookupGeneration_ = 0;
};

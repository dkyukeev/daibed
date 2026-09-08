#pragma once

#include "Block.h"
#include "raylib.h"

#include <optional>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct RaycastHit
{
    GridPos block {};
    GridPos adjacent {};
    GridPos normal {};
    Block blockData {};
    float distance = 0.0f;
};

enum class NavigationChangeQuery : std::uint8_t
{
    NoChanges,
    Disjoint,
    Intersects,
    HistoryUnavailable
};

class World
{
public:
    using BlockMap = std::unordered_map<GridPos, Block, GridPosHash>;

    void Clear();

    bool PlaceBlock(const GridPos& pos, const Block& block, bool allowReplace = false);
    bool BreakBlock(const GridPos& pos, int attackerTeam);
    bool RemoveBlock(const GridPos& pos);

    const Block* GetBlock(const GridPos& pos) const;
    bool IsAir(const GridPos& pos) const;
    bool IsSolid(const GridPos& pos) const;
    static bool IsCollisionBlock(BlockType type);

    GridPos WorldToGrid(Vector3 position) const;
    Vector3 GridToWorld(const GridPos& pos) const;

    bool CollidesWithAABB(Vector3 center, Vector3 halfExtents) const;
    // Highest collision surface of a support voxel (lower slabs are half a
    // block below a full cube). Shares the player collision shape definition.
    std::optional<float> BlockCollisionTop(const GridPos& pos) const;
    std::optional<RaycastHit> Raycast(Vector3 origin, Vector3 direction, float maxDistance) const;

    void AddIsland(Vector3 center, int halfSize, int y);
    void AddBridge(const GridPos& from, const GridPos& to, int y);

    const BlockMap& GetBlocks() const;
    std::uint64_t GetRenderRevision() const;
    std::vector<GridPos> TakeDirtyRenderChunks() const;
    NavigationChangeQuery QueryNavigationChanges(
        std::uint64_t sinceRevision,
        GridPos minimum,
        GridPos maximum,
        int padding = 0) const;

    static constexpr int kRenderChunkSize = 16;
    static GridPos RenderChunkForBlock(const GridPos& pos);

private:
    void MarkRenderDirty(const GridPos& pos);

    BlockMap blocks_;
    std::uint64_t renderRevision_ = 1;
    mutable std::unordered_set<GridPos, GridPosHash> dirtyRenderChunks_;
    struct NavigationDirtyCell
    {
        GridPos pos {};
        std::uint64_t revision = 0;
    };
    static constexpr std::size_t kNavigationDirtyHistoryCapacity = 4096;
    std::deque<NavigationDirtyCell> navigationDirtyHistory_;
    std::uint64_t navigationHistoryFloorRevision_ = 1;
};

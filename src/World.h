#pragma once

#include "Block.h"
#include "raylib.h"

#include <optional>
#include <unordered_map>

struct RaycastHit
{
    GridPos block {};
    GridPos adjacent {};
    GridPos normal {};
    Block blockData {};
    float distance = 0.0f;
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

    GridPos WorldToGrid(Vector3 position) const;
    Vector3 GridToWorld(const GridPos& pos) const;

    bool CollidesWithAABB(Vector3 center, Vector3 halfExtents) const;
    std::optional<RaycastHit> Raycast(Vector3 origin, Vector3 direction, float maxDistance) const;

    void AddIsland(Vector3 center, int halfSize, int y);
    void AddBridge(const GridPos& from, const GridPos& to, int y);

    const BlockMap& GetBlocks() const;

private:
    static bool IsCollisionBlock(BlockType type);

    BlockMap blocks_;
};

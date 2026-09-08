#pragma once

#include "World.h"

namespace VoxelLightShape
{
// Bit x + 2*z + 4*y describes an occupied half-block octant. 128 is reserved
// for full emissive cells by the atlas; slabs and stairs never produce it.
inline bool IsStair(BlockType type)
{
    return type == BlockType::StoneBrickStairsBlock || type == BlockType::BirchStairsBlock;
}

inline GridPos RaisedDirection(int variant)
{
    switch (variant & 3)
    {
    case 0: return GridPos { -1, 0, 0 };
    case 1: return GridPos { 1, 0, 0 };
    case 2: return GridPos { 0, 0, -1 };
    default: return GridPos { 0, 0, 1 };
    }
}

inline unsigned char Occupancy(const World& world, GridPos pos, const Block& block)
{
    switch (block.type)
    {
    case BlockType::StoneSlabBlock:
    case BlockType::StoneBrickSlabBlock:
    case BlockType::BirchSlabBlock:
        return (block.variant & 8) != 0 ? 0xf0 : 0x0f;
    default: break;
    }
    if (!IsStair(block.type)) return 255;

    const bool upper = (block.variant & 4) != 0;
    const GridPos raised = RaisedDirection(block.variant);
    const auto perpendicular = [&](GridPos at, GridPos& direction) {
        const Block* other = world.GetBlock(at);
        if (other == nullptr || !IsStair(other->type) || ((other->variant & 4) != 0) != upper) return false;
        direction = RaisedDirection(other->variant);
        return direction.x * raised.x + direction.z * raised.z == 0;
    };
    GridPos behind {}, front {};
    const bool outer = perpendicular(GridPos { pos.x + raised.x, pos.y, pos.z + raised.z }, behind);
    const bool inner = perpendicular(GridPos { pos.x - raised.x, pos.y, pos.z - raised.z }, front);
    const auto towards = [](int x, int z, GridPos d) {
        return (2 * x - 1) * d.x + (2 * z - 1) * d.z > 0;
    };
    unsigned char mask = upper ? 0xf0 : 0x0f;
    for (int z = 0; z < 2; ++z)
    for (int x = 0; x < 2; ++x)
    {
        const bool inRaisedHalf = towards(x, z, raised);
        // Same outer/inner corner rules as ChunkRenderer::AppendStairs.
        const bool occupied = (inRaisedHalf && (!outer || towards(x, z, behind)))
            || (inner && !inRaisedHalf && towards(x, z, front));
        if (occupied) mask |= static_cast<unsigned char>(1 << (x + 2 * z + (upper ? 0 : 4)));
    }
    return mask;
}
}

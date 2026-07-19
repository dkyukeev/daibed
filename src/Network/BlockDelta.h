#pragma once

#include "Block.h"

#include <cstdint>

enum class BlockDeltaReason
{
    Unknown,
    PlayerPlace,
    PlayerBreak,
    Explosion,
    FireBurn,
    TemporaryPlace,
    TemporaryExpire,
    PhaseRemove,
    PhaseRestore,
    CoreDestroyed,
    CoreCollapse,
    MapSetup,
    ReplicationTest
};

struct BlockDelta
{
    std::uint32_t tick = 0;
    GridPos position {};
    BlockType oldType = BlockType::Air;
    BlockType newType = BlockType::Air;
    int oldTeamId = -1;
    int newTeamId = -1;
    // Shape/orientation payload (stairs, slabs, torches, ladders, dyes);
    // without it a remote client renders placed blocks in default pose.
    int oldVariant = 0;
    int newVariant = 0;
    int ownerPlayerId = -1;
    BlockDeltaReason reason = BlockDeltaReason::Unknown;
};

#pragma once

#include <cstddef>

enum class BlockType
{
    Air,
    Solid,
    GrassBlock,
    DirtBlock,
    LeafBlock,
    TeamBlock,
    WoodBlock,
    WoolBlock,
    StoneBlock,
    ObsidianBlock,
    EnergyGlassBlock,
    SpringBlock,
    StickyBlock,
    ExplosiveBlock,
    SpikeBlock,
    LavaBlock,
    IceBlock,
    ResourceGenerator,
    EnergyCoreBlock
};

struct GridPos
{
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const GridPos& other) const;
    bool operator!=(const GridPos& other) const;
};

struct GridPosHash
{
    std::size_t operator()(const GridPos& pos) const noexcept;
};

struct Block
{
    BlockType type = BlockType::Air;
    int teamId = -1;
    bool breakable = false;
};

const char* ToString(BlockType type);
const char* DisplayName(BlockType type);
bool IsBuildableBlock(BlockType type);
bool IsBreakableByPlayers(BlockType type);
float BreakSeconds(BlockType type, int toolLevel);

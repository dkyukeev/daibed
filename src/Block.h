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
    TeamChestBlock,
    EnergyCoreBlock,
    SmoothStoneBlock,
    DarkBrickBlock,
    LightBrickBlock,
    MetalBlock,
    GlowBlock,
    PlankBlock,
    DecorativeTileBlock,
    TrimBlock,
    // Imported-map materials are deliberately appended so numeric .dbmap
    // files made by older builds keep their existing meanings.
    CobblestoneBlock,
    AndesiteBlock,
    PolishedAndesiteBlock,
    StoneBrickBlock,
    ChiseledStoneBrickBlock,
    StoneSlabBlock,
    StoneBrickSlabBlock,
    StoneBrickStairsBlock,
    BirchPlankBlock,
    BirchSlabBlock,
    BirchStairsBlock,
    // teamId is a Minecraft dye value (0..15) for the two coloured materials.
    ColoredGlassBlock,
    ColoredClayBlock,
    LapisBlock,
    DiamondBlock,
    EmeraldBlock,
    GoldBlock,
    IronBarsBlock,
    LadderBlock,
    TorchBlock,
    // Invisible, colliding map-editor helper; never sold by the shop.
    BarrierBlock,
    Count
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
    // Legacy Minecraft block-data / shape state.  It is deliberately kept
    // separate from teamId so imported coloured blocks and stairs do not
    // acquire gameplay-team semantics.
    int variant = 0;
};

const char* ToString(BlockType type);
const char* DisplayName(BlockType type);
bool BlockTypeFromString(const char* token, BlockType& out);
bool IsBuildableBlock(BlockType type);
bool IsCreativeOnlyBlock(BlockType type);
bool IsBreakableByPlayers(BlockType type);
float BreakSeconds(BlockType type, int toolLevel, bool usingAxe = false);

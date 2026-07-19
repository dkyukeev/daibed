#include "Block.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

bool GridPos::operator==(const GridPos& other) const
{
    return x == other.x && y == other.y && z == other.z;
}

bool GridPos::operator!=(const GridPos& other) const
{
    return !(*this == other);
}

std::size_t GridPosHash::operator()(const GridPos& pos) const noexcept
{
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](int value)
    {
        hash ^= static_cast<std::uint32_t>(value) ^ 0x80000000u;
        hash *= 1099511628211ull;
    };
    mix(pos.x);
    mix(pos.y);
    mix(pos.z);
    return static_cast<std::size_t>(hash);
}

const char* ToString(BlockType type)
{
    switch (type)
    {
    case BlockType::Air:
        return "Air";
    case BlockType::Solid:
        return "Solid";
    case BlockType::GrassBlock:
        return "GrassBlock";
    case BlockType::DirtBlock:
        return "DirtBlock";
    case BlockType::LeafBlock:
        return "LeafBlock";
    case BlockType::TeamBlock:
        return "TeamBlock";
    case BlockType::WoodBlock:
        return "WoodBlock";
    case BlockType::WoolBlock:
        return "WoolBlock";
    case BlockType::StoneBlock:
        return "StoneBlock";
    case BlockType::ObsidianBlock:
        return "ObsidianBlock";
    case BlockType::EnergyGlassBlock:
        return "EnergyGlassBlock";
    case BlockType::SpringBlock:
        return "SpringBlock";
    case BlockType::StickyBlock:
        return "StickyBlock";
    case BlockType::ExplosiveBlock:
        return "ExplosiveBlock";
    case BlockType::SpikeBlock:
        return "SpikeBlock";
    case BlockType::LavaBlock:
        return "LavaBlock";
    case BlockType::IceBlock:
        return "IceBlock";
    case BlockType::ResourceGenerator:
        return "ResourceGenerator";
    case BlockType::TeamChestBlock:
        return "TeamChestBlock";
    case BlockType::EnergyCoreBlock:
        return "EnergyCoreBlock";
    case BlockType::SmoothStoneBlock:
        return "SmoothStoneBlock";
    case BlockType::DarkBrickBlock:
        return "DarkBrickBlock";
    case BlockType::LightBrickBlock:
        return "LightBrickBlock";
    case BlockType::MetalBlock:
        return "MetalBlock";
    case BlockType::GlowBlock:
        return "GlowBlock";
    case BlockType::PlankBlock:
        return "PlankBlock";
    case BlockType::DecorativeTileBlock:
        return "DecorativeTileBlock";
    case BlockType::TrimBlock:
        return "TrimBlock";
    case BlockType::CobblestoneBlock:
        return "CobblestoneBlock";
    case BlockType::AndesiteBlock:
        return "AndesiteBlock";
    case BlockType::PolishedAndesiteBlock:
        return "PolishedAndesiteBlock";
    case BlockType::StoneBrickBlock:
        return "StoneBrickBlock";
    case BlockType::ChiseledStoneBrickBlock:
        return "ChiseledStoneBrickBlock";
    case BlockType::StoneSlabBlock:
        return "StoneSlabBlock";
    case BlockType::StoneBrickSlabBlock:
        return "StoneBrickSlabBlock";
    case BlockType::StoneBrickStairsBlock:
        return "StoneBrickStairsBlock";
    case BlockType::BirchPlankBlock:
        return "BirchPlankBlock";
    case BlockType::BirchSlabBlock:
        return "BirchSlabBlock";
    case BlockType::BirchStairsBlock:
        return "BirchStairsBlock";
    case BlockType::ColoredGlassBlock:
        return "ColoredGlassBlock";
    case BlockType::ColoredClayBlock:
        return "ColoredClayBlock";
    case BlockType::LapisBlock:
        return "LapisBlock";
    case BlockType::DiamondBlock:
        return "DiamondBlock";
    case BlockType::EmeraldBlock:
        return "EmeraldBlock";
    case BlockType::GoldBlock:
        return "GoldBlock";
    case BlockType::IronBarsBlock:
        return "IronBarsBlock";
    case BlockType::LadderBlock:
        return "LadderBlock";
    case BlockType::TorchBlock:
        return "TorchBlock";
    case BlockType::BarrierBlock:
        return "BarrierBlock";
    case BlockType::Count:
        break;
    }

    return "Неизвестно";
}

const char* DisplayName(BlockType type)
{
    switch (type)
    {
    case BlockType::WoodBlock:
        return "Деревянный блок";
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        return "Легкий блок";
    case BlockType::StoneBlock:
        return "Каменный блок";
    case BlockType::ObsidianBlock:
        return "Обсидиан";
    case BlockType::EnergyGlassBlock:
        return "Энергостекло";
    case BlockType::SpringBlock:
        return "Пружинный блок";
    case BlockType::StickyBlock:
        return "Липкий блок";
    case BlockType::ExplosiveBlock:
        return "TNT";
    case BlockType::SpikeBlock:
        return "Шипы";
    case BlockType::LavaBlock:
        return "Лава";
    case BlockType::IceBlock:
        return "Лед";
    case BlockType::Solid:
        return "Камень арены";
    case BlockType::GrassBlock:
        return "Трава";
    case BlockType::DirtBlock:
        return "Земля";
    case BlockType::LeafBlock:
        return "Листва";
    case BlockType::ResourceGenerator:
        return "Генератор";
    case BlockType::EnergyCoreBlock:
        return "Кор";
    case BlockType::SmoothStoneBlock:
        return "Гладкий камень";
    case BlockType::DarkBrickBlock:
        return "Темный кирпич";
    case BlockType::LightBrickBlock:
        return "Светлый кирпич";
    case BlockType::MetalBlock:
        return "Металлический блок";
    case BlockType::GlowBlock:
        return "Светящийся блок";
    case BlockType::PlankBlock:
        return "Резные доски";
    case BlockType::DecorativeTileBlock:
        return "Декоративная плитка";
    case BlockType::TrimBlock:
        return "Акцентный блок";
    case BlockType::CobblestoneBlock:
        return "Cobblestone";
    case BlockType::AndesiteBlock:
        return "Andesite";
    case BlockType::PolishedAndesiteBlock:
        return "Polished Andesite";
    case BlockType::StoneBrickBlock:
        return "Stone Brick";
    case BlockType::ChiseledStoneBrickBlock:
        return "Chiseled Stone";
    case BlockType::StoneSlabBlock:
        return "Stone Slab";
    case BlockType::StoneBrickSlabBlock:
        return "Stone Brick Slab";
    case BlockType::StoneBrickStairsBlock:
        return "Stone Brick Stairs";
    case BlockType::BirchPlankBlock:
        return "Birch Planks";
    case BlockType::BirchSlabBlock:
        return "Birch Slab";
    case BlockType::BirchStairsBlock:
        return "Birch Stairs";
    case BlockType::ColoredGlassBlock:
        return "Colored Glass";
    case BlockType::ColoredClayBlock:
        return "Colored Clay";
    case BlockType::LapisBlock:
        return "Lapis Block";
    case BlockType::DiamondBlock:
        return "Diamond Block";
    case BlockType::EmeraldBlock:
        return "Emerald Block";
    case BlockType::GoldBlock:
        return "Gold Block";
    case BlockType::IronBarsBlock:
        return "Iron Bars";
    case BlockType::LadderBlock:
        return "Ladder";
    case BlockType::TorchBlock:
        return "Torch";
    case BlockType::BarrierBlock:
        return "Barrier";
    case BlockType::Air:
        return "Воздух";
    case BlockType::Count:
        break;
    }

    return "Неизвестно";
}

bool BlockTypeFromString(const char* token, BlockType& out)
{
    if (token == nullptr)
    {
        return false;
    }
    for (int value = 0; value < static_cast<int>(BlockType::Count); ++value)
    {
        const BlockType candidate = static_cast<BlockType>(value);
        if (std::string_view(token) == ToString(candidate))
        {
            out = candidate;
            return true;
        }
    }
    return false;
}

bool IsBuildableBlock(BlockType type)
{
    return type == BlockType::WoodBlock
        || type == BlockType::WoolBlock
        || type == BlockType::StoneBlock
        || type == BlockType::ObsidianBlock
        || type == BlockType::EnergyGlassBlock
        || type == BlockType::SpringBlock
        || type == BlockType::StickyBlock
        || type == BlockType::ExplosiveBlock
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
        || type == BlockType::LadderBlock
        || type == BlockType::TorchBlock
        || type == BlockType::BarrierBlock
        || type == BlockType::TeamBlock;
}

bool IsCreativeOnlyBlock(BlockType type)
{
    return type == BlockType::BarrierBlock;
}

bool IsBreakableByPlayers(BlockType type)
{
    return type == BlockType::WoodBlock
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
        || type == BlockType::LadderBlock
        || type == BlockType::TorchBlock
        || type == BlockType::BarrierBlock
        || type == BlockType::TeamBlock;
}

float BreakSeconds(BlockType type, int toolLevel, bool usingAxe)
{
    float baseSeconds = 999.0f;
    switch (type)
    {
    case BlockType::WoodBlock:
        baseSeconds = 1.25f;
        break;
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        baseSeconds = 0.38f;
        break;
    case BlockType::StoneBlock:
    case BlockType::SmoothStoneBlock:
    case BlockType::DarkBrickBlock:
    case BlockType::LightBrickBlock:
    case BlockType::DecorativeTileBlock:
    case BlockType::TrimBlock:
    case BlockType::CobblestoneBlock:
    case BlockType::AndesiteBlock:
    case BlockType::StoneBrickBlock:
    case BlockType::ChiseledStoneBrickBlock:
        baseSeconds = 1.15f;
        break;
    case BlockType::PolishedAndesiteBlock:
    case BlockType::StoneSlabBlock:
    case BlockType::StoneBrickSlabBlock:
    case BlockType::StoneBrickStairsBlock:
        baseSeconds = 1.25f;
        break;
    case BlockType::ObsidianBlock:
        baseSeconds = 3.4f;
        break;
    case BlockType::MetalBlock:
        baseSeconds = 1.8f;
        break;
    case BlockType::EnergyGlassBlock:
    case BlockType::GlowBlock:
        baseSeconds = 1.65f;
        break;
    case BlockType::SpringBlock:
    case BlockType::StickyBlock:
        baseSeconds = 0.9f;
        break;
    case BlockType::PlankBlock:
    case BlockType::BirchPlankBlock:
    case BlockType::BirchSlabBlock:
    case BlockType::BirchStairsBlock:
        baseSeconds = 0.52f;
        break;
    case BlockType::ColoredClayBlock:
        baseSeconds = 1.05f;
        break;
    case BlockType::ColoredGlassBlock:
        baseSeconds = 0.38f;
        break;
    case BlockType::LapisBlock:
    case BlockType::DiamondBlock:
    case BlockType::EmeraldBlock:
    case BlockType::GoldBlock:
        baseSeconds = 1.75f;
        break;
    case BlockType::IronBarsBlock:
        baseSeconds = 0.65f;
        break;
    case BlockType::LadderBlock:
        baseSeconds = 0.35f;
        break;
    case BlockType::TorchBlock:
        baseSeconds = 0.20f;
        break;
    case BlockType::ExplosiveBlock:
        baseSeconds = 0.75f;
        break;
    case BlockType::SpikeBlock:
    case BlockType::LavaBlock:
    case BlockType::IceBlock:
        baseSeconds = 1.2f;
        break;
    case BlockType::EnergyCoreBlock:
        baseSeconds = 1.25f;
        break;
    case BlockType::Air:
    case BlockType::Solid:
    case BlockType::GrassBlock:
    case BlockType::DirtBlock:
    case BlockType::LeafBlock:
    case BlockType::ResourceGenerator:
    case BlockType::TeamChestBlock:
    case BlockType::BarrierBlock:
    case BlockType::Count:
        break;
    }

    const float toolMultiplier = 1.0f + std::clamp(toolLevel, 0, 4) * 0.28f;
    const float axeMultiplier = type == BlockType::WoodBlock && usingAxe ? 0.44f : 1.0f;
    return std::max(0.18f, baseSeconds * axeMultiplier / toolMultiplier);
}

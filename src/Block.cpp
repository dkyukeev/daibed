#include "Block.h"

#include <algorithm>
#include <functional>

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
    const std::size_t hx = std::hash<int>{}(pos.x);
    const std::size_t hy = std::hash<int>{}(pos.y);
    const std::size_t hz = std::hash<int>{}(pos.z);
    return hx ^ (hy << 1U) ^ (hz << 2U);
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
    case BlockType::EnergyCoreBlock:
        return "EnergyCoreBlock";
    }

    return "Unknown";
}

const char* DisplayName(BlockType type)
{
    switch (type)
    {
    case BlockType::WoodBlock:
        return "Wood block";
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        return "Light block";
    case BlockType::StoneBlock:
        return "Stone block";
    case BlockType::ObsidianBlock:
        return "Obsidian";
    case BlockType::EnergyGlassBlock:
        return "Energy glass";
    case BlockType::SpringBlock:
        return "Spring block";
    case BlockType::StickyBlock:
        return "Sticky block";
    case BlockType::ExplosiveBlock:
        return "TNT";
    case BlockType::SpikeBlock:
        return "Spikes";
    case BlockType::LavaBlock:
        return "Lava";
    case BlockType::IceBlock:
        return "Ice";
    case BlockType::Solid:
        return "Arena stone";
    case BlockType::GrassBlock:
        return "Grass";
    case BlockType::DirtBlock:
        return "Dirt";
    case BlockType::LeafBlock:
        return "Leaves";
    case BlockType::ResourceGenerator:
        return "Generator";
    case BlockType::EnergyCoreBlock:
        return "EnergyCore";
    case BlockType::Air:
        return "Air";
    }

    return "Unknown";
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
        || type == BlockType::TeamBlock;
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
        || type == BlockType::TeamBlock;
}

float BreakSeconds(BlockType type, int toolLevel)
{
    float baseSeconds = 999.0f;
    switch (type)
    {
    case BlockType::WoodBlock:
        baseSeconds = 0.52f;
        break;
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        baseSeconds = 0.38f;
        break;
    case BlockType::StoneBlock:
        baseSeconds = 1.15f;
        break;
    case BlockType::ObsidianBlock:
        baseSeconds = 3.4f;
        break;
    case BlockType::EnergyGlassBlock:
        baseSeconds = 1.65f;
        break;
    case BlockType::SpringBlock:
    case BlockType::StickyBlock:
        baseSeconds = 0.9f;
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
        break;
    }

    const float toolMultiplier = 1.0f + std::clamp(toolLevel, 0, 4) * 0.28f;
    return std::max(0.18f, baseSeconds / toolMultiplier);
}

#include "Block.h"

#include <algorithm>
#include <cstdint>

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
    case BlockType::Air:
        return "Воздух";
    }

    return "Неизвестно";
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
    case BlockType::TeamChestBlock:
        break;
    }

    const float toolMultiplier = 1.0f + std::clamp(toolLevel, 0, 4) * 0.28f;
    return std::max(0.18f, baseSeconds / toolMultiplier);
}

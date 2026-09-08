#pragma once

#include "Block.h"
#include "Hero.h"
#include "Resource.h"
#include "Team.h"

#include "raylib.h"

// Small, semantic vocabulary shared by presentation systems. Gameplay values
// do not belong here: these tokens may change without affecting simulation,
// networking, collision, damage, or ability timing.
namespace VisualTheme
{
namespace Palette
{
inline constexpr Color Danger { 255, 118, 118, 255 };
inline constexpr Color Healing { 128, 238, 166, 255 };
inline constexpr Color Shield { 98, 245, 255, 255 };
inline constexpr Color Energy { 112, 232, 255, 255 };
inline constexpr Color Objective { 255, 235, 142, 255 };

inline constexpr Color ResourceIron { 188, 198, 210, 255 };
inline constexpr Color ResourceGold { 246, 196, 74, 255 };
inline constexpr Color ResourceCrystal { 112, 232, 255, 255 };

inline constexpr Color Panel { 8, 10, 14, 255 };
inline constexpr Color TextBright { 236, 240, 245, 255 };
inline constexpr Color TextDim { 170, 180, 195, 255 };
}

namespace Timing
{
inline constexpr float Flash = 0.12f;
inline constexpr float Trail = 0.22f;
inline constexpr float Impact = 0.32f;
inline constexpr float Dissolve = 0.65f;
}

namespace Emissive
{
inline constexpr float Accent = 0.18f;
inline constexpr float Ability = 0.42f;
inline constexpr float Threat = 0.72f;
inline constexpr float Objective = 1.0f;
}

namespace Screen
{
inline constexpr float Vignette = 0.28f;
inline constexpr float BloomLow = 0.30f;
inline constexpr float BloomMedium = 0.40f;
inline constexpr float BloomHigh = 0.52f;
inline constexpr float DamageFlashFull = 0.72f;
inline constexpr float DamageFlashReduced = 0.22f;
}

inline constexpr Color TeamIdentity(TeamColor color)
{
    switch (color)
    {
    case TeamColor::Red: return Color { 230, 74, 74, 255 };
    case TeamColor::Blue: return Color { 74, 135, 230, 255 };
    case TeamColor::Green: return Color { 64, 190, 110, 255 };
    case TeamColor::Yellow: return Color { 236, 202, 72, 255 };
    }
    return WHITE;
}

inline constexpr Color HeroAccent(HeroId id)
{
    switch (id)
    {
    case HeroId::Radon: return Color { 92, 164, 255, 255 };
    case HeroId::Orbita: return Color { 255, 96, 82, 255 };
    case HeroId::Brom: return Color { 96, 202, 118, 255 };
    case HeroId::Konvoy: return Color { 92, 210, 255, 255 };
    case HeroId::Likho: return Color { 104, 238, 92, 255 };
    case HeroId::Svidetel: return Color { 180, 104, 255, 255 };
    }
    return WHITE;
}

inline constexpr Color ResourcePickup(ResourceType type)
{
    switch (type)
    {
    case ResourceType::Iron: return Palette::ResourceIron;
    case ResourceType::Gold: return Palette::ResourceGold;
    case ResourceType::Crystal: return Palette::ResourceCrystal;
    }
    return Palette::Objective;
}

inline constexpr Color SurfaceDust(BlockType type)
{
    switch (type)
    {
    case BlockType::GrassBlock:
    case BlockType::LeafBlock:
        return Color { 94, 142, 76, 255 };
    case BlockType::DirtBlock:
        return Color { 126, 88, 58, 255 };
    case BlockType::WoodBlock:
    case BlockType::PlankBlock:
    case BlockType::LadderBlock:
        return Color { 166, 112, 68, 255 };
    case BlockType::BirchPlankBlock:
    case BlockType::BirchSlabBlock:
    case BlockType::BirchStairsBlock:
        return Color { 218, 198, 148, 255 };
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        return Color { 170, 170, 174, 255 };
    case BlockType::ObsidianBlock:
        return Color { 66, 48, 82, 255 };
    case BlockType::EnergyGlassBlock:
    case BlockType::ColoredGlassBlock:
    case BlockType::IceBlock:
        return Color { 154, 222, 242, 255 };
    case BlockType::LavaBlock:
        return Color { 238, 104, 48, 255 };
    case BlockType::ExplosiveBlock:
        return Color { 222, 102, 62, 255 };
    case BlockType::SpringBlock:
        return Color { 112, 214, 132, 255 };
    case BlockType::StickyBlock:
        return Color { 112, 190, 96, 255 };
    case BlockType::EnergyCoreBlock:
    case BlockType::ResourceGenerator:
    case BlockType::GlowBlock:
        return Palette::Energy;
    case BlockType::MetalBlock:
    case BlockType::IronBarsBlock:
        return Color { 138, 150, 162, 255 };
    case BlockType::GoldBlock:
        return Palette::ResourceGold;
    case BlockType::DiamondBlock:
        return Color { 92, 224, 224, 255 };
    case BlockType::EmeraldBlock:
        return Color { 64, 204, 112, 255 };
    default:
        return Color { 148, 152, 160, 255 };
    }
}
}

#include "Navigation/NavigationActionLibrary.h"

#include "Navigation/NavigationWorldView.h"

#include <algorithm>
#include <cmath>

const std::array<GridPos, 4>& NavigationActionLibrary::CardinalDirections() noexcept
{
    static constexpr std::array<GridPos, 4> directions {
        GridPos { 1, 0, 0 }, GridPos { 0, 0, 1 },
        GridPos { -1, 0, 0 }, GridPos { 0, 0, -1 }
    };
    return directions;
}

const std::array<GridPos, 4>& NavigationActionLibrary::DiagonalDirections() noexcept
{
    static constexpr std::array<GridPos, 4> directions {
        GridPos { 1, 0, 1 }, GridPos { -1, 0, 1 },
        GridPos { -1, 0, -1 }, GridPos { 1, 0, -1 }
    };
    return directions;
}

bool NavigationActionLibrary::IsDiagonal(GridPos from, GridPos to) noexcept
{
    return from.x != to.x && from.z != to.z;
}

float NavigationActionLibrary::HorizontalDistance(GridPos from, GridPos to) noexcept
{
    const float dx = static_cast<float>(to.x - from.x);
    const float dz = static_cast<float>(to.z - from.z);
    return std::sqrt(dx * dx + dz * dz);
}

bool NavigationActionLibrary::CanWalkDiagonal(
    GridPos from,
    GridPos to,
    const NavigationWorldView& world,
    const NavigationProfile& profile)
{
    if (from.y != to.y || !IsDiagonal(from, to)) return false;
    const int dx = std::clamp(to.x - from.x, -1, 1);
    const int dz = std::clamp(to.z - from.z, -1, 1);
    if (std::abs(to.x - from.x) != 1 || std::abs(to.z - from.z) != 1) return false;

    const GridPos shoulderX { from.x + dx, from.y, from.z };
    const GridPos shoulderZ { from.x, from.y, from.z + dz };
    return world.IsSupported(to)
        && world.IsBodyClear(to, profile)
        && world.IsSupported(shoulderX)
        && world.IsBodyClear(shoulderX, profile)
        && world.IsSupported(shoulderZ)
        && world.IsBodyClear(shoulderZ, profile);
}

bool NavigationActionLibrary::CanSprintLine(
    GridPos from,
    GridPos to,
    const NavigationWorldView& world,
    const NavigationProfile& profile)
{
    if (from.y != to.y) return false;
    const int dx = to.x - from.x;
    const int dz = to.z - from.z;
    if ((dx == 0) == (dz == 0)) return false;
    const int span = std::max(std::abs(dx), std::abs(dz));
    if (span < 2 || span > profile.maxSprintRunBlocks) return false;
    const int stepX = dx == 0 ? 0 : (dx > 0 ? 1 : -1);
    const int stepZ = dz == 0 ? 0 : (dz > 0 ? 1 : -1);
    for (int offset = 1; offset <= span; ++offset)
    {
        const GridPos cell {
            from.x + stepX * offset,
            from.y,
            from.z + stepZ * offset
        };
        if (!world.IsSupported(cell)
            || !world.IsBodyClear(cell, profile)
            || world.IsHazard(cell))
        {
            return false;
        }
    }
    return true;
}

#pragma once

#include "Navigation/NavigationProfile.h"

#include <array>

class NavigationWorldView;

// Shared physical contract for local navigation primitives. The pathfinder
// chooses among these actions; PathExecutor still turns every chosen action
// into ordinary PlayerCommand input.
class NavigationActionLibrary
{
public:
    static const std::array<GridPos, 4>& CardinalDirections() noexcept;
    static const std::array<GridPos, 4>& DiagonalDirections() noexcept;

    static bool IsDiagonal(GridPos from, GridPos to) noexcept;
    static float HorizontalDistance(GridPos from, GridPos to) noexcept;

    // Conservative corner rule: a diagonal floor move is accepted only when
    // both orthogonal shoulders are supported and have body clearance. This
    // prevents the search from cutting through solid corners or balancing on
    // a single voxel corner that the command executor cannot reproduce.
    static bool CanWalkDiagonal(
        GridPos from,
        GridPos to,
        const NavigationWorldView& world,
        const NavigationProfile& profile);

    // Checks every cell crossed by a long sprint. This is used both while
    // planning and when a world revision forces PathExecutor validation.
    static bool CanSprintLine(
        GridPos from,
        GridPos to,
        const NavigationWorldView& world,
        const NavigationProfile& profile);
};

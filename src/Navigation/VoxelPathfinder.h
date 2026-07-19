#pragma once

#include "Navigation/NavigationGoal.h"
#include "Navigation/NavigationProfile.h"
#include "Navigation/NavigationTypes.h"

class NavigationWorldView;

// Deterministic, bounded action search over support blocks. No Game, renderer,
// camera or mutable World dependency is permitted here.
class VoxelPathfinder
{
public:
    NavigationSearchResult FindPath(
        const NavigationState& start,
        const NavigationGoal& goal,
        const NavigationWorldView& world,
        const NavigationProfile& profile,
        const NavigationSearchLimits& limits = {}) const;
};

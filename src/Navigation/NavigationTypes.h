#pragma once

#include "Block.h"

#include "raylib.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class MovementType : std::uint8_t
{
    Walk,
    Sprint,
    StepUp,
    JumpUp,
    GapJump,
    DropDown,
    SneakBridge,
    PlaceBlock,
    BreakBlock,
    Wait,
    Count
};

constexpr std::size_t kMovementTypeCount = static_cast<std::size_t>(MovementType::Count);

enum class NavVoxel : std::uint8_t
{
    Air,
    SolidUnbreakable,
    BreakableCheap,
    BreakableMedium,
    BreakableExpensive,
    Hazard,
    Temporary,
    FriendlyCore,
    EnemyCore
};

inline const char* ToString(MovementType type) noexcept
{
    switch (type)
    {
    case MovementType::Walk: return "Walk";
    case MovementType::Sprint: return "Sprint";
    case MovementType::StepUp: return "StepUp";
    case MovementType::JumpUp: return "JumpUp";
    case MovementType::GapJump: return "GapJump";
    case MovementType::DropDown: return "DropDown";
    case MovementType::SneakBridge: return "SneakBridge";
    case MovementType::PlaceBlock: return "PlaceBlock";
    case MovementType::BreakBlock: return "BreakBlock";
    case MovementType::Wait: return "Wait";
    case MovementType::Count: break;
    }
    return "Unknown";
}

// A node is the block supporting the agent, not the player's body cell.
// The corresponding player center is normally support.y + bodyCenterAboveSupport.
struct NavigationState
{
    GridPos support {};
    int remainingBridgeBlocks = 0;
    int consecutiveBridgeBlocks = 0;
};

struct NavigationActor
{
    int playerId = -1;
    int teamId = -1;
    Vector3 position {};
    Vector3 velocity {};
    bool alive = true;
    float collisionRadius = 0.36f;
    float threatRadius = 8.0f;
    float threatCost = 8.0f;
};

struct DefenseBlueprintCell
{
    GridPos relativePosition {};
    int minimumDefenseRank = 0;
    int priority = 0;
    bool mustRemainPassable = false;
};

struct PlannedMovement
{
    MovementType type = MovementType::Walk;
    GridPos from {};
    GridPos to {};

    std::optional<GridPos> affectedBlock;
    std::optional<BlockType> placementBlockType;

    float movementCost = 0.0f;
    float timeCost = 0.0f;
    float resourceCost = 0.0f;
    float threatCost = 0.0f;
    float totalCost = 0.0f;

    bool requiresJump = false;
    bool requiresSneak = false;
    bool requiresSprint = false;

    // Number of support cells crossed by this action. Normally one; long
    // sprint primitives retain their full swept-cell validation contract.
    int traversedCells = 1;

    std::uint64_t plannedWorldRevision = 0;

    bool MutatesWorld() const noexcept
    {
        return type == MovementType::PlaceBlock
            || type == MovementType::BreakBlock
            || type == MovementType::SneakBridge;
    }
};

struct NavigationPath
{
    std::vector<PlannedMovement> movements;

    float totalCost = 0.0f;
    float movementCost = 0.0f;
    float timeCost = 0.0f;
    float resourceCost = 0.0f;
    float threatCost = 0.0f;

    std::uint64_t worldRevision = 0;
    GridPos start {};
    GridPos resolvedGoal {};
    bool partial = false;
    int expandedNodes = 0;
    int generatedNodes = 0;
    int peakOpenNodes = 0;
    int heapDecreaseKeys = 0;
    int corridorRejectedNodes = 0;

    bool Empty() const noexcept { return movements.empty(); }
    std::size_t ActionCount() const noexcept { return movements.size(); }
};

struct NavigationSearchLimits
{
    int maxExpansions = 3200;
    int maxActions = 256;
    int maxSearchRadius = 64;
    int maxVerticalRange = 24;
    int maxOpenNodes = 8192;
    bool allowPartial = true;
    float heuristicWeight = 1.08f;

    // Optional horizontal tube around the current coarse route segment. It is
    // a search-space hint, never an authoritative movement shortcut.
    bool constrainToCorridor = false;
    GridPos corridorStart {};
    GridPos corridorEnd {};
    float corridorHalfWidth = 10.0f;
    int corridorVerticalPadding = 10;
};

enum class NavigationSearchStatus : std::uint8_t
{
    Success,
    AlreadySatisfied,
    Partial,
    NoPath,
    InvalidStart,
    ExpansionLimit,
    OpenSetLimit,
    ActionLimit
};

constexpr std::size_t kNavigationSearchStatusCount =
    static_cast<std::size_t>(NavigationSearchStatus::ActionLimit) + 1;

inline const char* ToString(NavigationSearchStatus status) noexcept
{
    switch (status)
    {
    case NavigationSearchStatus::Success: return "Success";
    case NavigationSearchStatus::AlreadySatisfied: return "AlreadySatisfied";
    case NavigationSearchStatus::Partial: return "Partial";
    case NavigationSearchStatus::NoPath: return "NoPath";
    case NavigationSearchStatus::InvalidStart: return "InvalidStart";
    case NavigationSearchStatus::ExpansionLimit: return "ExpansionLimit";
    case NavigationSearchStatus::OpenSetLimit: return "OpenSetLimit";
    case NavigationSearchStatus::ActionLimit: return "ActionLimit";
    }
    return "Unknown";
}

struct NavigationSearchResult
{
    NavigationSearchStatus status = NavigationSearchStatus::NoPath;
    NavigationPath path;
    std::string description;

    bool Succeeded() const noexcept
    {
        return status == NavigationSearchStatus::Success
            || status == NavigationSearchStatus::AlreadySatisfied;
    }

    bool HasPath() const noexcept
    {
        return !path.movements.empty()
            && (Succeeded() || status == NavigationSearchStatus::Partial);
    }
};

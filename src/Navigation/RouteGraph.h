#pragma once

#include "CreativeMap.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class World;

struct RouteCorridorSegment
{
    int fromNodeIndex = -1;
    int toNodeIndex = -1;
    std::string tag = "walk";
    int expectedBridgeBlocks = 0;

    bool IsBridge() const noexcept { return tag == "bridge"; }
};

struct RouteCorridor
{
    std::vector<int> nodeIndices;
    std::vector<RouteCorridorSegment> segments;
    float cost = 0.0f;
    std::uint64_t signature = 0;
    bool valid = false;
};

// A small, authored strategic graph above action navigation. It selects a
// stable sequence of coarse portals; VoxelPathfinder remains authoritative for
// the physical route between each pair of portals.
class RouteGraph
{
public:
    bool Build(
        const std::vector<CreativeRouteNode>& nodes,
        const std::vector<CreativeRouteEdge>& edges,
        std::vector<std::string>* errors = nullptr);
    void Clear();

    bool Empty() const noexcept { return nodes_.empty(); }
    std::size_t NodeCount() const noexcept { return nodes_.size(); }
    std::size_t EdgeCount() const noexcept { return edgeCount_; }
    const CreativeRouteNode* Node(int index) const noexcept;
    bool ValidatePhysical(const World& world, std::vector<std::string>* errors = nullptr) const;

    RouteCorridor FindCorridor(
        GridPos start,
        GridPos goal,
        int teamId,
        const GridPos* recentlyFailedPosition = nullptr,
        float failurePenalty = 0.0f,
        const std::function<bool(const CreativeRouteNode&)>& canReachEntry = {}) const;

private:
    struct Link
    {
        int to = -1;
        float cost = 1.0f;
        std::string tag = "walk";
        int expectedBridgeBlocks = 0;
    };

    bool IsAccessible(int nodeIndex, int teamId) const noexcept;
    int FindNearestNode(GridPos pos, int teamId,
        const std::function<bool(const CreativeRouteNode&)>& canReach = {}) const;

    std::vector<CreativeRouteNode> nodes_;
    std::vector<std::vector<Link>> adjacency_;
    std::size_t edgeCount_ = 0;
};

#include "Navigation/RouteGraph.h"

#include "World.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>

namespace
{
float RouteDistance(GridPos a, GridPos b) noexcept
{
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y) * 1.35f;
    const float dz = static_cast<float>(a.z - b.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float HorizontalDistanceToSegment(GridPos point, GridPos from, GridPos to) noexcept
{
    const float vx = static_cast<float>(to.x - from.x);
    const float vz = static_cast<float>(to.z - from.z);
    const float wx = static_cast<float>(point.x - from.x);
    const float wz = static_cast<float>(point.z - from.z);
    const float lengthSquared = vx * vx + vz * vz;
    const float t = lengthSquared > 0.001f
        ? std::clamp((wx * vx + wz * vz) / lengthSquared, 0.0f, 1.0f)
        : 0.0f;
    const float dx = wx - vx * t;
    const float dz = wz - vz * t;
    return std::sqrt(dx * dx + dz * dz);
}

void HashByte(std::uint64_t& value, unsigned char byte) noexcept
{
    value ^= byte;
    value *= 1099511628211ull;
}

bool HasExactBodySupport(const World& world, GridPos body)
{
    const GridPos support { body.x, body.y - 1, body.z };
    const GridPos head { body.x, body.y + 1, body.z };
    return world.IsSolid(support) && world.IsAir(body) && world.IsAir(head);
}

bool HasBodySupportNear(const World& world, GridPos body, int radius)
{
    for (int distance = 0; distance <= radius; ++distance)
    {
        for (int dx = -distance; dx <= distance; ++dx)
        {
            for (int dz = -distance; dz <= distance; ++dz)
            {
                if (std::max(std::abs(dx), std::abs(dz)) != distance) continue;
                for (int dy = -2; dy <= 2; ++dy)
                {
                    if (HasExactBodySupport(world, GridPos { body.x + dx, body.y + dy, body.z + dz }))
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}
}

bool RouteGraph::Build(
    const std::vector<CreativeRouteNode>& nodes,
    const std::vector<CreativeRouteEdge>& edges,
    std::vector<std::string>* errors)
{
    Clear();
    std::vector<std::string> localErrors;
    std::unordered_map<std::string, int> byId;
    for (const CreativeRouteNode& node : nodes)
    {
        if (node.id.empty())
        {
            localErrors.emplace_back("route node has an empty id");
            continue;
        }
        if (byId.find(node.id) != byId.end())
        {
            localErrors.emplace_back("duplicate route node: " + node.id);
            continue;
        }
        const int index = static_cast<int>(nodes_.size());
        byId.emplace(node.id, index);
        nodes_.push_back(node);
    }
    adjacency_.resize(nodes_.size());

    for (const CreativeRouteEdge& edge : edges)
    {
        const auto from = byId.find(edge.fromId);
        const auto to = byId.find(edge.toId);
        if (from == byId.end() || to == byId.end())
        {
            localErrors.emplace_back("route edge references a missing node: "
                + edge.fromId + " -> " + edge.toId);
            continue;
        }
        if (from->second == to->second)
        {
            localErrors.emplace_back("route edge loops to itself: " + edge.fromId);
            continue;
        }
        if (!std::isfinite(edge.cost) || edge.cost <= 0.0f)
        {
            localErrors.emplace_back("route edge has invalid cost: "
                + edge.fromId + " -> " + edge.toId);
            continue;
        }

        const float distance = std::max(1.0f,
            RouteDistance(nodes_[from->second].pos, nodes_[to->second].pos));
        const float cost = distance * edge.cost;
        const bool bridge = edge.tag == "bridge";
        const GridPos fromPos = nodes_[from->second].pos;
        const GridPos toPos = nodes_[to->second].pos;
        const int horizontalSpan = std::abs(fromPos.x - toPos.x) + std::abs(fromPos.z - toPos.z);
        const int expectedBridgeBlocks = bridge ? std::max(0, horizontalSpan - 1) : 0;
        if (bridge)
        {
            const std::string& fromKind = nodes_[from->second].kind;
            const std::string& toKind = nodes_[to->second].kind;
            const bool endpointKinds = (fromKind == "bridge_start" && toKind == "bridge_land")
                || (fromKind == "bridge_land" && toKind == "bridge_start");
            if (!endpointKinds)
            {
                localErrors.emplace_back("bridge edge requires bridge_start/bridge_land nodes: "
                    + edge.fromId + " -> " + edge.toId);
                continue;
            }
            if (std::abs(fromPos.y - toPos.y) > 1 || expectedBridgeBlocks < 1 || expectedBridgeBlocks > 64)
            {
                localErrors.emplace_back("bridge edge has unsupported span: "
                    + edge.fromId + " -> " + edge.toId);
                continue;
            }
        }
        adjacency_[from->second].push_back(Link { to->second, cost, edge.tag, expectedBridgeBlocks });
        if (edge.bidirectional)
        {
            adjacency_[to->second].push_back(Link { from->second, cost, edge.tag, expectedBridgeBlocks });
        }
        ++edgeCount_;
    }

    for (std::vector<Link>& links : adjacency_)
    {
        std::sort(links.begin(), links.end(), [this](const Link& a, const Link& b)
        {
            return nodes_[a.to].id < nodes_[b.to].id;
        });
    }

    if (errors != nullptr)
    {
        *errors = localErrors;
    }
    if (!localErrors.empty())
    {
        // A partially valid graph is intentionally rejected: silent omission
        // would make authored routes difficult to diagnose.
        Clear();
        return false;
    }
    return !nodes_.empty();
}

bool RouteGraph::ValidatePhysical(const World& world, std::vector<std::string>* errors) const
{
    std::vector<std::string> localErrors;
    for (const CreativeRouteNode& node : nodes_)
    {
        const bool exact = node.kind == "bridge_start" || node.kind == "bridge_land";
        if ((exact && !HasExactBodySupport(world, node.pos))
            || (!exact && !HasBodySupportNear(world, node.pos, 6)))
        {
            localErrors.emplace_back("route node has no usable body support: " + node.id);
        }
    }
    for (int from = 0; from < static_cast<int>(adjacency_.size()); ++from)
    {
        for (const Link& link : adjacency_[static_cast<std::size_t>(from)])
        {
            if (link.tag != "bridge" || from > link.to) continue;
            const GridPos a = nodes_[static_cast<std::size_t>(from)].pos;
            const GridPos b = nodes_[static_cast<std::size_t>(link.to)].pos;
            if (!HasExactBodySupport(world, a) || !HasExactBodySupport(world, b))
            {
                localErrors.emplace_back("bridge edge endpoint is not physically usable: "
                    + nodes_[static_cast<std::size_t>(from)].id + " -> "
                    + nodes_[static_cast<std::size_t>(link.to)].id);
            }
        }
    }
    if (errors != nullptr) *errors = localErrors;
    return localErrors.empty();
}

void RouteGraph::Clear()
{
    nodes_.clear();
    adjacency_.clear();
    edgeCount_ = 0;
}

const CreativeRouteNode* RouteGraph::Node(int index) const noexcept
{
    if (index < 0 || index >= static_cast<int>(nodes_.size())) return nullptr;
    return &nodes_[static_cast<std::size_t>(index)];
}

bool RouteGraph::IsAccessible(int nodeIndex, int teamId) const noexcept
{
    const int owner = nodes_[static_cast<std::size_t>(nodeIndex)].teamId;
    return owner < 0 || owner == teamId;
}

int RouteGraph::FindNearestNode(GridPos pos, int teamId,
    const std::function<bool(const CreativeRouteNode&)>& canReach) const
{
    if (canReach)
    {
        std::vector<int> candidates;
        for (int i = 0; i < static_cast<int>(nodes_.size()); ++i)
            if (IsAccessible(i, teamId)) candidates.push_back(i);
        std::sort(candidates.begin(), candidates.end(), [&](int a, int b) {
            const float da = RouteDistance(pos, nodes_[a].pos);
            const float db = RouteDistance(pos, nodes_[b].pos);
            return da != db ? da < db : nodes_[a].id < nodes_[b].id;
        });
        for (int candidate : candidates)
            if (canReach(nodes_[candidate])) return candidate;
        return -1;
    }
    int best = -1;
    float bestDistance = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(nodes_.size()); ++i)
    {
        if (!IsAccessible(i, teamId)) continue;
        const float distance = RouteDistance(pos, nodes_[static_cast<std::size_t>(i)].pos);
        if (distance < bestDistance - 0.001f
            || (std::fabs(distance - bestDistance) <= 0.001f
                && (best < 0 || nodes_[static_cast<std::size_t>(i)].id
                    < nodes_[static_cast<std::size_t>(best)].id)))
        {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

RouteCorridor RouteGraph::FindCorridor(
    GridPos start,
    GridPos goal,
    int teamId,
    const GridPos* recentlyFailedPosition,
    float failurePenalty,
    const std::function<bool(const CreativeRouteNode&)>& canReachEntry) const
{
    RouteCorridor result;
    const int startNode = FindNearestNode(start, teamId, canReachEntry);
    const int goalNode = FindNearestNode(goal, teamId);
    if (startNode < 0 || goalNode < 0) return result;

    const std::size_t count = nodes_.size();
    std::vector<float> distance(count, std::numeric_limits<float>::max());
    std::vector<int> parent(count, -1);
    using QueueEntry = std::pair<float, int>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> open;
    distance[static_cast<std::size_t>(startNode)] = 0.0f;
    open.emplace(0.0f, startNode);

    while (!open.empty())
    {
        const auto [currentCost, current] = open.top();
        open.pop();
        if (currentCost > distance[static_cast<std::size_t>(current)] + 0.001f) continue;
        if (current == goalNode) break;

        for (const Link& link : adjacency_[static_cast<std::size_t>(current)])
        {
            if (!IsAccessible(link.to, teamId)) continue;
            float linkCost = link.cost;
            if (recentlyFailedPosition != nullptr && failurePenalty > 0.0f
                && HorizontalDistanceToSegment(
                    *recentlyFailedPosition,
                    nodes_[static_cast<std::size_t>(current)].pos,
                    nodes_[static_cast<std::size_t>(link.to)].pos) <= 12.0f)
            {
                linkCost += failurePenalty;
            }
            const float candidate = currentCost + linkCost;
            const std::size_t next = static_cast<std::size_t>(link.to);
            const bool cheaper = candidate < distance[next] - 0.001f;
            const bool deterministicTie = std::fabs(candidate - distance[next]) <= 0.001f
                && (parent[next] < 0 || nodes_[static_cast<std::size_t>(current)].id
                    < nodes_[static_cast<std::size_t>(parent[next])].id);
            if (!cheaper && !deterministicTie) continue;
            distance[next] = candidate;
            parent[next] = current;
            open.emplace(candidate, link.to);
        }
    }

    if (distance[static_cast<std::size_t>(goalNode)] == std::numeric_limits<float>::max())
    {
        return result;
    }
    for (int node = goalNode; node >= 0; node = parent[static_cast<std::size_t>(node)])
    {
        result.nodeIndices.push_back(node);
        if (node == startNode) break;
    }
    if (result.nodeIndices.empty() || result.nodeIndices.back() != startNode)
    {
        result.nodeIndices.clear();
        return result;
    }
    std::reverse(result.nodeIndices.begin(), result.nodeIndices.end());
    for (std::size_t index = 1; index < result.nodeIndices.size(); ++index)
    {
        const int from = result.nodeIndices[index - 1];
        const int to = result.nodeIndices[index];
        const auto found = std::find_if(
            adjacency_[static_cast<std::size_t>(from)].begin(),
            adjacency_[static_cast<std::size_t>(from)].end(),
            [to](const Link& link) { return link.to == to; });
        if (found == adjacency_[static_cast<std::size_t>(from)].end())
        {
            result.nodeIndices.clear();
            result.segments.clear();
            return result;
        }
        result.segments.push_back(RouteCorridorSegment {
            from, to, found->tag, found->expectedBridgeBlocks });
    }
    result.cost = distance[static_cast<std::size_t>(goalNode)]
        + RouteDistance(start, nodes_[static_cast<std::size_t>(startNode)].pos)
        + RouteDistance(nodes_[static_cast<std::size_t>(goalNode)].pos, goal);
    result.signature = 1469598103934665603ull;
    for (int index : result.nodeIndices)
    {
        for (unsigned char byte : nodes_[static_cast<std::size_t>(index)].id) HashByte(result.signature, byte);
        HashByte(result.signature, 0xff);
    }
    for (const RouteCorridorSegment& segment : result.segments)
    {
        for (unsigned char byte : segment.tag) HashByte(result.signature, byte);
        HashByte(result.signature, 0xfe);
    }
    result.valid = true;
    return result;
}

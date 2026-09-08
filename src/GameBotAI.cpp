#include "Game.h"
#include "BotCombatAssessment.h"
#include "VecConvert.h"

#include "raylib.h"
#include "raymath.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Define for one-off navigation diagnostics (rate-limited stdout traces in
// ChooseBotPathWaypoint).
// #define DAIBED_NAV_TRACE 1

namespace
{
constexpr int kBotPathMinY = -2;
// Stock arenas stay below this, while imported Creative maps such as Castle
// Bedwars reach y=90 after normalization.
constexpr int kBotPathMaxY = 112;
constexpr float kBotPi = 3.1415926535f;
constexpr float kCoordinationSignalTtl = 2.25f;

int NavigationMarkerKindIndex(CreativeSpecialKind kind)
{
    switch (kind)
    {
    case CreativeSpecialKind::Rally: return 0;
    case CreativeSpecialKind::Lane: return 1;
    case CreativeSpecialKind::Chokepoint: return 2;
    case CreativeSpecialKind::Highground: return 3;
    default: return -1;
    }
}

class ScopedProfileTimer
{
public:
    ScopedProfileTimer(bool enabled, double& elapsedMs, unsigned long long& calls)
        : enabled_(enabled), elapsedMs_(elapsedMs), calls_(calls)
    {
        if (enabled_)
        {
            started_ = std::chrono::steady_clock::now();
        }
    }

    ~ScopedProfileTimer()
    {
        Stop();
    }

    void Stop()
    {
        if (enabled_)
        {
            elapsedMs_ += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started_).count();
            ++calls_;
            enabled_ = false;
        }
    }

private:
    bool enabled_ = false;
    double& elapsedMs_;
    unsigned long long& calls_;
    std::chrono::steady_clock::time_point started_ {};
};

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float Length2D(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.z * value.z);
}

Vector3 Normalize2D(Vector3 value)
{
    const float length = Length2D(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, 0.0f, value.z / length };
}

Vector3 Normalize3D(Vector3 value)
{
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }
    return Vector3 { value.x / length, value.y / length, value.z / length };
}

Vector3 ApplyStableAimBias(
    Vector3 direction,
    std::uint32_t personalitySeed,
    int targetId,
    BotDifficulty difficulty,
    float caution)
{
    direction = Normalize3D(direction);
    std::uint32_t hash = personalitySeed ^ (static_cast<std::uint32_t>(targetId + 17) * 0x85ebca6bu);
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    const float signedUnit = static_cast<float>(hash & 0xFFFFu) / 32767.5f - 1.0f;
    const float degrees = difficulty == BotDifficulty::Hard ? 1.35f
        : (difficulty == BotDifficulty::Easy ? 5.5f : 2.8f);
    // The miss is stable for one opponent: readable personal bias rather than
    // per-tick random jitter. Cautious bots take slightly steadier shots.
    const float radians = signedUnit * degrees * (1.12f - caution * 0.24f) * kBotPi / 180.0f;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    return Normalize3D(Vector3 {
        direction.x * cosine - direction.z * sine,
        direction.y,
        direction.x * sine + direction.z * cosine });
}

int BotPickaxeSlot(const Player& player)
{
    const auto& hotbar = player.GetInventory().GetHotbarSlots();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        if (!hotbar[slot].IsEmpty() && ItemIsPickaxe(hotbar[slot].type)) return slot;
    }
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        if (!hotbar[slot].IsEmpty() && ItemIsWeapon(hotbar[slot].type)) return slot;
    }
    return 0;
}

int PickaxeSlot(const Player& player)
{
    return BotPickaxeSlot(player);
}

bool HasSupportBelow(const World& world, Vector3 position, int maxDropBlocks)
{
    const GridPos underCenter = world.WorldToGrid(Vector3 { position.x, position.y - 1.40f, position.z });
    for (int drop = 0; drop <= maxDropBlocks; ++drop)
    {
        if (!world.IsAir(GridPos { underCenter.x, underCenter.y - drop, underCenter.z }))
        {
            return true;
        }
    }
    return false;
}

GridPos FindSupportBelow(const World& world, Vector3 position, int maxDropBlocks)
{
    const GridPos underCenter = world.WorldToGrid(Vector3 { position.x, position.y - 1.40f, position.z });
    for (int drop = 0; drop <= maxDropBlocks; ++drop)
    {
        const GridPos candidate { underCenter.x, underCenter.y - drop, underCenter.z };
        if (!world.IsAir(candidate))
        {
            return candidate;
        }
    }
    return underCenter;
}

float Length(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vector3 StepTargetToward(Vector3 from, Vector3 target, float maxStep)
{
    const Vector3 delta { target.x - from.x, 0.0f, target.z - from.z };
    const float distance = Length2D(delta);
    if (distance <= maxStep || distance <= 0.0001f)
    {
        return target;
    }

    const Vector3 direction = Normalize2D(delta);
    return Vector3 {
        from.x + direction.x * maxStep,
        target.y,
        from.z + direction.z * maxStep
    };
}

float YawFromDirection(Vector3 direction)
{
    return std::atan2(direction.x, -direction.z);
}

int DefenseBlockRank(BlockType type)
{
    switch (type)
    {
    case BlockType::ObsidianBlock:
        return 5;
    case BlockType::StoneBlock:
    // Imported-map masonry (the castle is built from these) protects a core
    // at least as well as shop stone; without a rank the defense monitor
    // reads a walled-in core as "weak" and keeps bots repairing forever.
    case BlockType::SmoothStoneBlock:
    case BlockType::DarkBrickBlock:
    case BlockType::LightBrickBlock:
    case BlockType::MetalBlock:
    case BlockType::CobblestoneBlock:
    case BlockType::AndesiteBlock:
    case BlockType::PolishedAndesiteBlock:
    case BlockType::StoneBrickBlock:
    case BlockType::ChiseledStoneBrickBlock:
    case BlockType::ColoredClayBlock:
    case BlockType::LapisBlock:
    case BlockType::DiamondBlock:
    case BlockType::EmeraldBlock:
    case BlockType::GoldBlock:
    case BlockType::DecorativeTileBlock:
    case BlockType::TrimBlock:
        return 4;
    case BlockType::EnergyGlassBlock:
    case BlockType::ColoredGlassBlock:
        return 3;
    case BlockType::WoodBlock:
    case BlockType::PlankBlock:
    case BlockType::BirchPlankBlock:
        return 2;
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        return 1;
    default:
        break;
    }
    return 0;
}

std::optional<BlockType> BestDefenseBlockAvailable(const Inventory& inventory)
{
    const BlockType priority[] {
        BlockType::ObsidianBlock,
        BlockType::StoneBlock,
        BlockType::EnergyGlassBlock,
        BlockType::WoodBlock,
        BlockType::WoolBlock
    };
    for (BlockType type : priority)
    {
        if (inventory.GetBlockCount(type) > 0)
        {
            return type;
        }
    }
    return std::nullopt;
}

long long PathKey(int x, int y, int z)
{
    return (static_cast<long long>(x + 2048) << 32)
        ^ (static_cast<long long>(z + 2048) << 8)
        ^ static_cast<unsigned int>(y + 32);
}

struct BotPathNode
{
    GridPos pos {};
    float g = 0.0f;
    float f = 0.0f;
};

// --- Bot build blueprints (creative building framework) ---------------------
//
// A blueprint is a STRUCTURE expressed as data: a list of cells relative to an
// anchor world cell, each with an optional block-type preference and a build
// tier. This replaces per-structure hardcoded placement procedures — a new
// structure (sniper perch, forward staging platform, ...) becomes a new
// blueprint (data) + an anchor, not a new TryBotX() function. Anchoring is pure
// integer arithmetic so a blueprint's world cells are exact and deterministic.
//
// NOTE ON SCOPE: only STRUCTURES are blueprints. A bridge is a procedural PATH
// (it grows as the bot walks toward a target), not a fixed structure, so it
// stays its own routine (TryBotBridgeBlock) — forcing it into a blueprint would
// be a rewrite for its own sake.
struct BotBlueprintCell
{
    GridPos offset {};
    // Air = "no preference": the bot places its normal pick (the core shell
    // does this — its block choice comes from SelectPlacementBlockForPlayer /
    // the upgrade logic). A concrete type is a preference for later blueprints.
    BlockType preferredType = BlockType::Air;
    // Lower tier builds first when the generic selector fills a blueprint. The
    // core shell's consumers iterate in list ORDER (not tier), so tiers are
    // inert for it; they matter for blueprints that use the generic selector.
    int tier = 0;
};
using BotBlueprint = std::vector<BotBlueprintCell>;

std::vector<GridPos> AnchorBlueprint(const BotBlueprint& blueprint, GridPos anchor)
{
    std::vector<GridPos> cells;
    cells.reserve(blueprint.size());
    for (const BotBlueprintCell& cell : blueprint)
    {
        cells.push_back(GridPos {
            anchor.x + cell.offset.x,
            anchor.y + cell.offset.y,
            anchor.z + cell.offset.z });
    }
    return cells;
}

// Generic blueprint executor primitive: the first still-empty (air) cell of an
// anchored blueprint, in blueprint order (deterministic). Shared by the core
// shell's repair scan and any later structure (e.g. the sniper perch). Returns
// nullopt when the structure is already complete.
std::optional<GridPos> NextMissingBlueprintCell(
    const World& world, const BotBlueprint& blueprint, GridPos anchor)
{
    for (const GridPos& cell : AnchorBlueprint(blueprint, anchor))
    {
        if (world.IsAir(cell))
        {
            return cell;
        }
    }
    return std::nullopt;
}

// The core-defense fort as a blueprint. Offsets + ORDER are exactly the former
// hardcoded CoreDefensePositions list, so anchoring at the core reproduces the
// same 26 cells in the same order — every shell consumer is byte-for-byte
// unchanged (the automatch baseline proves it).
const BotBlueprint& CoreShellBlueprint()
{
    static const BotBlueprint blueprint {
        // Inner ring (y = 0) then the y = +1 ring.
        { GridPos { 1, 0, 0 }, BlockType::Air, 0 },
        { GridPos { -1, 0, 0 }, BlockType::Air, 0 },
        { GridPos { 0, 0, 1 }, BlockType::Air, 0 },
        { GridPos { 0, 0, -1 }, BlockType::Air, 0 },
        { GridPos { 0, 1, 0 }, BlockType::Air, 1 },
        { GridPos { 1, 1, 0 }, BlockType::Air, 1 },
        { GridPos { -1, 1, 0 }, BlockType::Air, 1 },
        { GridPos { 0, 1, 1 }, BlockType::Air, 1 },
        { GridPos { 0, 1, -1 }, BlockType::Air, 1 },
        // Outer ring (y = 0) + diagonals.
        { GridPos { 2, 0, 0 }, BlockType::Air, 0 },
        { GridPos { -2, 0, 0 }, BlockType::Air, 0 },
        { GridPos { 0, 0, 2 }, BlockType::Air, 0 },
        { GridPos { 0, 0, -2 }, BlockType::Air, 0 },
        { GridPos { 1, 0, 1 }, BlockType::Air, 0 },
        { GridPos { 1, 0, -1 }, BlockType::Air, 0 },
        { GridPos { -1, 0, 1 }, BlockType::Air, 0 },
        { GridPos { -1, 0, -1 }, BlockType::Air, 0 },
        // A paced outer firing wall and compact roof turn repairs into a
        // readable fort without sealing the shop or primary approaches.
        { GridPos { 2, 1, 0 }, BlockType::Air, 1 },
        { GridPos { -2, 1, 0 }, BlockType::Air, 1 },
        { GridPos { 0, 1, 2 }, BlockType::Air, 1 },
        { GridPos { 0, 1, -2 }, BlockType::Air, 1 },
        { GridPos { 1, 2, 0 }, BlockType::Air, 2 },
        { GridPos { -1, 2, 0 }, BlockType::Air, 2 },
        { GridPos { 0, 2, 1 }, BlockType::Air, 2 },
        { GridPos { 0, 2, -1 }, BlockType::Air, 2 },
        { GridPos { 0, 2, 0 }, BlockType::Air, 2 }
    };
    return blueprint;
}

std::vector<GridPos> CoreDefensePositions(GridPos corePos)
{
    return AnchorBlueprint(CoreShellBlueprint(), corePos);
}

// ---------------------------------------------------------------------------
// Adaptive defense planner.  The legacy dome blueprint assumed the core
// stands in an open field; on imported maps cores live in niches, gardens
// and towers, where most of the dome is wasted or unbuildable.  The plan is
// instead derived from the actual geometry: the minimum set of buildable air
// cells that disconnects the core from the outside of a small region around
// it (a vertex min-cut over the air graph), plus a second layer computed the
// same way with the first one virtually built.  A garden niche produces a
// thick plug across its mouth; open ground degenerates into the classic
// concentric shells.  Already-built own walls participate with a cheaper
// capacity than fresh air, so recomputed plans gravitate to the walls the
// team already owns instead of flip-flopping between equivalent cuts.
namespace defense_planner
{
constexpr int kRadiusXZ = 5;
constexpr int kBelow = 1;
constexpr int kAbove = 4;
constexpr int kSizeXZ = kRadiusXZ * 2 + 1;
constexpr int kSizeY = kBelow + kAbove + 1;
constexpr int kCellCount = kSizeXZ * kSizeXZ * kSizeY;
constexpr int kLayerOneCellCap = 14;
constexpr int kTotalCellCap = 30;
constexpr int kFlowAbort = 40;
constexpr int kInfiniteCapacity = 1 << 20;

struct FlowEdge
{
    int to = 0;
    int reverseIndex = 0;
    int capacity = 0;
};

struct FlowGraph
{
    std::vector<std::vector<FlowEdge>> adjacency;

    void Reset(int nodes)
    {
        adjacency.assign(static_cast<std::size_t>(nodes), {});
    }

    void AddEdge(int from, int to, int capacity)
    {
        adjacency[from].push_back(FlowEdge { to, static_cast<int>(adjacency[to].size()), capacity });
        adjacency[to].push_back(FlowEdge { from, static_cast<int>(adjacency[from].size()) - 1, 0 });
    }
};

// Edmonds-Karp is plenty: the region holds ~700 cells and useful cut costs
// are in the low tens, so the augmenting loop runs a few dozen BFS passes.
int MaxFlow(FlowGraph& graph, int source, int sink, int abortAbove)
{
    int flow = 0;
    std::vector<int> parentNode(graph.adjacency.size());
    std::vector<int> parentEdge(graph.adjacency.size());
    while (flow <= abortAbove)
    {
        std::fill(parentNode.begin(), parentNode.end(), -1);
        parentNode[source] = source;
        std::queue<int> frontier;
        frontier.push(source);
        while (!frontier.empty() && parentNode[sink] < 0)
        {
            const int node = frontier.front();
            frontier.pop();
            for (int edgeIndex = 0; edgeIndex < static_cast<int>(graph.adjacency[node].size()); ++edgeIndex)
            {
                const FlowEdge& edge = graph.adjacency[node][edgeIndex];
                if (edge.capacity > 0 && parentNode[edge.to] < 0)
                {
                    parentNode[edge.to] = node;
                    parentEdge[edge.to] = edgeIndex;
                    frontier.push(edge.to);
                }
            }
        }
        if (parentNode[sink] < 0)
        {
            return flow;
        }
        int bottleneck = kInfiniteCapacity;
        for (int node = sink; node != source; node = parentNode[node])
        {
            bottleneck = std::min(bottleneck, graph.adjacency[parentNode[node]][parentEdge[node]].capacity);
        }
        for (int node = sink; node != source; node = parentNode[node])
        {
            FlowEdge& edge = graph.adjacency[parentNode[node]][parentEdge[node]];
            edge.capacity -= bottleneck;
            graph.adjacency[edge.to][edge.reverseIndex].capacity += bottleneck;
        }
        flow += bottleneck;
    }
    return flow;
}

enum class RegionCell : std::uint8_t
{
    Blocked,   // unbreakable/neutral/enemy solid, or the core itself
    OwnWall,   // own-team breakable block: part of the plannable wall
    Air
};

struct RegionModel
{
    GridPos core {};
    std::array<RegionCell, kCellCount> cells {};
};

int CellIndexFor(int dx, int dy, int dz)
{
    return ((dy + kBelow) * kSizeXZ + (dz + kRadiusXZ)) * kSizeXZ + (dx + kRadiusXZ);
}

GridPos CellPosition(const RegionModel& model, int index)
{
    const int dx = index % kSizeXZ - kRadiusXZ;
    const int dz = (index / kSizeXZ) % kSizeXZ - kRadiusXZ;
    const int dy = index / (kSizeXZ * kSizeXZ) - kBelow;
    return GridPos { model.core.x + dx, model.core.y + dy, model.core.z + dz };
}

bool IsBoundaryIndex(int index)
{
    const int dx = index % kSizeXZ - kRadiusXZ;
    const int dz = (index / kSizeXZ) % kSizeXZ - kRadiusXZ;
    const int dy = index / (kSizeXZ * kSizeXZ) - kBelow;
    return std::abs(dx) == kRadiusXZ || std::abs(dz) == kRadiusXZ || dy == -kBelow || dy == kAbove;
}

RegionModel BuildRegionModel(const World& world, const Team& team)
{
    RegionModel model;
    model.core = team.coreBlock;
    for (int dy = -kBelow; dy <= kAbove; ++dy)
    {
        for (int dz = -kRadiusXZ; dz <= kRadiusXZ; ++dz)
        {
            for (int dx = -kRadiusXZ; dx <= kRadiusXZ; ++dx)
            {
                const int index = CellIndexFor(dx, dy, dz);
                const GridPos pos { model.core.x + dx, model.core.y + dy, model.core.z + dz };
                if (dx == 0 && dy == 0 && dz == 0)
                {
                    model.cells[index] = RegionCell::Blocked; // the core itself
                    continue;
                }
                const Block* block = world.GetBlock(pos);
                if (block == nullptr || block->type == BlockType::Air)
                {
                    model.cells[index] = RegionCell::Air;
                }
                else if (block->teamId == team.id && block->breakable)
                {
                    model.cells[index] = RegionCell::OwnWall;
                }
                else
                {
                    model.cells[index] = RegionCell::Blocked;
                }
            }
        }
    }
    return model;
}

// One min-cut layer over the region.  Passable cells (air / own walls) are
// split into in->out nodes; air costs 2, an existing own wall costs 1, so the
// cut re-uses built geometry when it can.  Returns nullopt when the opening
// is too wide to seal within budget.
std::optional<std::vector<GridPos>> ComputeCutLayer(const RegionModel& model)
{
    constexpr int kNodeCount = kCellCount * 2 + 2;
    const int source = kCellCount * 2;
    const int sink = kCellCount * 2 + 1;
    const auto inNode = [](int index) { return index; };
    const auto outNode = [](int index) { return kCellCount + index; };

    static thread_local FlowGraph graph;
    graph.Reset(kNodeCount);

    const int coreIndex = CellIndexFor(0, 0, 0);
    for (int index = 0; index < kCellCount; ++index)
    {
        const RegionCell cell = model.cells[index];
        if (index == coreIndex || cell == RegionCell::Blocked)
        {
            continue;
        }
        graph.AddEdge(inNode(index), outNode(index), cell == RegionCell::OwnWall ? 1 : 2);
        if (IsBoundaryIndex(index))
        {
            graph.AddEdge(outNode(index), sink, kInfiniteCapacity);
        }
    }

    const auto forEachNeighbor = [](int index, const auto& visit)
    {
        const int dx = index % kSizeXZ - kRadiusXZ;
        const int dz = (index / kSizeXZ) % kSizeXZ - kRadiusXZ;
        const int dy = index / (kSizeXZ * kSizeXZ) - kBelow;
        const int offsets[6][3] { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (const auto& offset : offsets)
        {
            const int nx = dx + offset[0];
            const int ny = dy + offset[1];
            const int nz = dz + offset[2];
            if (std::abs(nx) > kRadiusXZ || std::abs(nz) > kRadiusXZ || ny < -kBelow || ny > kAbove)
            {
                continue;
            }
            visit(CellIndexFor(nx, ny, nz));
        }
    };

    for (int index = 0; index < kCellCount; ++index)
    {
        if (model.cells[index] == RegionCell::Blocked && index != coreIndex)
        {
            continue;
        }
        forEachNeighbor(index, [&](int neighbor)
        {
            if (model.cells[neighbor] == RegionCell::Blocked && neighbor != coreIndex)
            {
                return;
            }
            if (index == coreIndex)
            {
                if (neighbor != coreIndex)
                {
                    graph.AddEdge(source, inNode(neighbor), kInfiniteCapacity);
                }
                return;
            }
            if (neighbor == coreIndex)
            {
                return;
            }
            graph.AddEdge(outNode(index), inNode(neighbor), kInfiniteCapacity);
        });
    }

    const int flow = MaxFlow(graph, source, sink, kFlowAbort);
    if (flow > kFlowAbort)
    {
        return std::nullopt;
    }
    if (flow == 0)
    {
        return std::vector<GridPos> {};
    }

    // Min-cut extraction: cells whose in-node is reachable in the residual
    // graph while the out-node is not.
    std::vector<bool> reachable(kNodeCount, false);
    std::queue<int> frontier;
    reachable[source] = true;
    frontier.push(source);
    while (!frontier.empty())
    {
        const int node = frontier.front();
        frontier.pop();
        for (const FlowEdge& edge : graph.adjacency[node])
        {
            if (edge.capacity > 0 && !reachable[edge.to])
            {
                reachable[edge.to] = true;
                frontier.push(edge.to);
            }
        }
    }

    std::vector<GridPos> cut;
    for (int index = 0; index < kCellCount; ++index)
    {
        if (index == coreIndex || model.cells[index] == RegionCell::Blocked)
        {
            continue;
        }
        if (reachable[inNode(index)] && !reachable[outNode(index)])
        {
            cut.push_back(CellPosition(model, index));
        }
    }
    return cut;
}

std::optional<std::vector<GridPos>> ComputePlan(const World& world, const Team& team)
{
    RegionModel model = BuildRegionModel(world, team);
    const std::optional<std::vector<GridPos>> layerOne = ComputeCutLayer(model);
    if (!layerOne.has_value() || static_cast<int>(layerOne->size()) > kLayerOneCellCap)
    {
        return std::nullopt;
    }

    std::vector<GridPos> plan = *layerOne;
    const auto closerToCore = [&team](const GridPos& a, const GridPos& b)
    {
        const auto rank = [&team](const GridPos& pos)
        {
            return std::abs(pos.x - team.coreBlock.x)
                + std::abs(pos.y - team.coreBlock.y)
                + std::abs(pos.z - team.coreBlock.z);
        };
        return rank(a) < rank(b);
    };
    std::sort(plan.begin(), plan.end(), closerToCore);

    // Only reinforce with a second layer when the first one actually needed
    // building: a core whose inner shell already stands (stock arenas and
    // authored maps ship pre-built defenses) keeps the author's footprint —
    // bots maintain and upgrade it, they do not double every base by default.
    bool layerOneHadHoles = false;
    for (const GridPos& cell : plan)
    {
        const int dx = cell.x - model.core.x;
        const int dy = cell.y - model.core.y;
        const int dz = cell.z - model.core.z;
        if (model.cells[CellIndexFor(dx, dy, dz)] == RegionCell::Air)
        {
            layerOneHadHoles = true;
            break;
        }
    }

    if (!plan.empty() && layerOneHadHoles)
    {
        // Second layer: recompute with layer one blocked outright (a passable
        // "own wall" would just be cut through again), so the next cut has to
        // sit outside the first.
        for (const GridPos& cell : plan)
        {
            const int dx = cell.x - model.core.x;
            const int dy = cell.y - model.core.y;
            const int dz = cell.z - model.core.z;
            model.cells[CellIndexFor(dx, dy, dz)] = RegionCell::Blocked;
        }
        const std::optional<std::vector<GridPos>> layerTwo = ComputeCutLayer(model);
        if (layerTwo.has_value()
            && !layerTwo->empty()
            && static_cast<int>(plan.size() + layerTwo->size()) <= kTotalCellCap)
        {
            std::vector<GridPos> outer = *layerTwo;
            std::sort(outer.begin(), outer.end(), closerToCore);
            plan.insert(plan.end(), outer.begin(), outer.end());
        }
    }
    return plan;
}

std::uint64_t HashRegion(const World& world, const Team& team)
{
    // Own-team breakable blocks hash like air on purpose: the team building
    // its own plan must not invalidate the plan.  Neutral/enemy/unbreakable
    // geometry changes do.
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(static_cast<std::uint64_t>(team.coreBlock.x + 4096));
    mix(static_cast<std::uint64_t>(team.coreBlock.y + 4096));
    mix(static_cast<std::uint64_t>(team.coreBlock.z + 4096));
    for (int dy = -kBelow; dy <= kAbove; ++dy)
    {
        for (int dz = -kRadiusXZ; dz <= kRadiusXZ; ++dz)
        {
            for (int dx = -kRadiusXZ; dx <= kRadiusXZ; ++dx)
            {
                const GridPos pos { team.coreBlock.x + dx, team.coreBlock.y + dy, team.coreBlock.z + dz };
                const Block* block = world.GetBlock(pos);
                const bool blocked = block != nullptr
                    && block->type != BlockType::Air
                    && !(block->teamId == team.id && block->breakable);
                mix(blocked ? 2u + static_cast<std::uint64_t>(block->type) : 1u);
            }
        }
    }
    return hash;
}
} // namespace defense_planner

// The firing perch as a blueprint: a short vertical column the bot towers up to
// clear a wall and regain a shooting angle. Its size() is the height cap; the
// realization is procedural (tower under own feet, like the bridge is a
// procedural path) — a blueprint DESCRIBES the structure, it need not force
// every mechanic through the generic executor.
const BotBlueprint& FiringPerchBlueprint()
{
    static const BotBlueprint blueprint {
        { GridPos { 0, 1, 0 }, BlockType::Air, 0 },
        { GridPos { 0, 2, 0 }, BlockType::Air, 1 },
        { GridPos { 0, 3, 0 }, BlockType::Air, 2 }
    };
    return blueprint;
}

struct CoreDefenseStatus
{
    int missingBlocks = 0;
    int weakBlocks = 0;
    bool critical = false;
};

CoreDefenseStatus InspectCoreDefense(const World& world, const Team& team, const std::vector<GridPos>& defenseCells)
{
    CoreDefenseStatus status {};
    for (const GridPos& candidate : defenseCells)
    {
        const Block* block = world.GetBlock(candidate);
        // Neutral map geometry (imported castles carry teamId -1) shelters
        // the core just as well as an own-team block.  Counting it as a hole
        // used to trap every bot in an unfinishable repair loop, because the
        // occupied cell can never be built over.  Only air and enemy-owned
        // blocks are real gaps.
        if (block == nullptr || world.IsAir(candidate)
            || (block->teamId >= 0 && block->teamId != team.id))
        {
            ++status.missingBlocks;
            continue;
        }

        // Only own-team blocks count toward "weak": neutral map geometry
        // (imported hedges, masonry) is not upgradeable by bots, so scoring
        // it as weak would keep the defense permanently "critical".
        if (block->teamId == team.id && DefenseBlockRank(block->type) <= 1)
        {
            ++status.weakBlocks;
        }
    }

    status.critical = status.missingBlocks >= 3
        || (status.missingBlocks >= 1 && status.weakBlocks >= 4)
        || status.weakBlocks >= 8;
    return status;
}

int CarriedResourceValue(const Inventory& inventory)
{
    return inventory.GetResource(ResourceType::Iron)
        + inventory.GetResource(ResourceType::Gold) * 4
        + inventory.GetResource(ResourceType::Crystal) * 9;
}

float BotAttackDelay(BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return 0.48f;
    case BotDifficulty::Hard:
        return 0.20f;
    case BotDifficulty::Normal:
        break;
    }
    return 0.32f;
}

float BotFightReactionDelay(BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return 0.48f;
    case BotDifficulty::Hard:
        return 0.18f;
    case BotDifficulty::Normal:
        break;
    }
    return 0.32f;
}

float BotBridgeCooldown(BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return 0.34f;
    case BotDifficulty::Hard:
        return 0.16f;
    case BotDifficulty::Normal:
        break;
    }
    return 0.22f;
}

// Human-like reaction time after taking a hit: until it expires the bot cannot
// place a save block, so knockback displaces bots instead of being negated by a
// same-frame block under the feet.
float BotHitReactionSeconds(BotDifficulty difficulty)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return 0.60f;
    case BotDifficulty::Hard:
        return 0.22f;
    case BotDifficulty::Normal:
        break;
    }
    return 0.38f;
}

BotRole RoleForBotId(int id)
{
    return id % 4 == 0
        ? BotRole::Defender
        : (id % 4 == 1 ? BotRole::Fighter : (id % 4 == 2 ? BotRole::Rusher : BotRole::Collector));
}

int PreferredNeighborTeam(int teamId, bool clockwise)
{
    if (clockwise)
    {
        switch (teamId)
        {
        case 0: return 2;
        case 2: return 1;
        case 1: return 3;
        case 3: return 0;
        default: return -1;
        }
    }
    switch (teamId)
    {
    case 0: return 3;
    case 3: return 1;
    case 1: return 2;
    case 2: return 0;
    default: return -1;
    }
}

const BotRoleTuning& RoleTuning(const BotTuningGenome& tuning, BotRole role)
{
    return tuning.roles[std::clamp(static_cast<int>(role), 0, 3)];
}

float DifficultyCautionOffset(BotDifficulty difficulty, float easyOffset, float hardOffset)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return easyOffset;
    case BotDifficulty::Hard:
        return hardOffset;
    case BotDifficulty::Normal:
        break;
    }
    return 0.0f;
}

int BotRetreatHealth(BotRole role, BotDifficulty difficulty, bool coreAlive, const BotTuningGenome& tuning)
{
    const BotRoleTuning& roleTuning = RoleTuning(tuning, role);
    if (!coreAlive)
    {
        return static_cast<int>(std::round(roleTuning.retreatHealthFinalLife + DifficultyCautionOffset(difficulty, 10.0f, -8.0f)));
    }

    return static_cast<int>(std::round(roleTuning.retreatHealthCoreAlive + DifficultyCautionOffset(difficulty, 14.0f, -12.0f)));
}

int BotFightHealth(BotRole role, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    const BotRoleTuning& roleTuning = RoleTuning(tuning, role);
    return static_cast<int>(std::round(roleTuning.fightHealth + DifficultyCautionOffset(difficulty, 18.0f, -10.0f)));
}

// ---------------------------------------------------------------------------
// Bot personality: stable per-bot biases derived from the bot id (pure hash —
// no RNG stream, deterministic for a given roster). Teammates share the same
// inputs (spawn point, team context, tuning), so without personal biases they
// make IDENTICAL decisions and move in lockstep — the "clone squad" effect
// players notice. Personality decorrelates the tie-breaks: who fights vs who
// farms, engage distances, flank sides, spacing and plan patience.
// ---------------------------------------------------------------------------
struct BotPersonality
{
    float aggression = 1.0f;   // scales combat/pressure intent appetite
    float economy = 1.0f;      // scales gathering/gearing intent appetite
    float patience = 1.0f;     // scales strategic plan durations
    float engageOffset = 0.0f; // blocks added to the preferred engage range
    int flankSign = 1;         // preferred strafe/flank side
    float spacing = 2.0f;      // preferred distance to the nearest teammate
    float caution = 0.5f;
    float teamwork = 0.5f;
    float creativity = 0.5f;
    BotArchetype archetype = BotArchetype::TeamHelper;
};

BotPersonality PersonalityForBot(int botId, std::uint32_t personalitySeed = 0)
{
    unsigned int h = static_cast<unsigned int>(botId) * 2654435761u;
    h ^= personalitySeed + 0x9e3779b9u + (h << 6u) + (h >> 2u);
    const auto unit = [h](int shift)
    {
        return static_cast<float>((h >> shift) & 0xFFu) / 255.0f;
    };
    BotPersonality personality;
    personality.aggression = 0.86f + unit(0) * 0.30f;
    personality.economy = 0.86f + unit(4) * 0.30f;
    personality.patience = 0.85f + unit(8) * 0.35f;
    personality.engageOffset = (unit(12) - 0.5f) * 1.8f;
    personality.flankSign = ((h >> 16) & 1u) != 0 ? 1 : -1;
    personality.spacing = 1.7f + unit(20) * 1.5f;
    personality.caution = 0.25f + unit(5) * 0.65f;
    personality.teamwork = 0.25f + unit(11) * 0.70f;
    personality.creativity = 0.20f + unit(17) * 0.75f;
    personality.archetype = static_cast<BotArchetype>((h >> 24u) & 7u);
    switch (personality.archetype)
    {
    case BotArchetype::CautiousDefender:
        personality.aggression *= 0.72f; personality.patience *= 1.22f; personality.caution = 0.94f; break;
    case BotArchetype::AggressiveRusher:
        personality.aggression *= 1.24f; personality.patience *= 0.88f; personality.caution *= 0.68f; break;
    case BotArchetype::FrugalBuilder:
        personality.economy *= 1.18f; personality.patience *= 1.12f; personality.creativity = std::max(personality.creativity, 0.72f); break;
    case BotArchetype::IsolationHunter:
        personality.aggression *= 1.10f; personality.teamwork *= 0.70f; break;
    case BotArchetype::TeamHelper:
        personality.teamwork = 0.96f; personality.aggression *= 0.92f; break;
    case BotArchetype::ImpulsiveDuelist:
        personality.aggression *= 1.18f; personality.patience *= 0.70f; personality.caution *= 0.62f; break;
    case BotArchetype::Engineer:
        personality.creativity = 0.98f; personality.economy *= 1.08f; break;
    case BotArchetype::Opportunist:
        personality.aggression *= 1.06f; personality.patience *= 0.82f; break;
    }
    return personality;
}

float BotEngageRange(BotRole role, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    const float difficultyBonus = difficulty == BotDifficulty::Hard ? 1.15f : (difficulty == BotDifficulty::Easy ? -0.85f : 0.0f);
    const float roleScale = role == BotRole::Rusher ? 0.6f : (role == BotRole::Collector ? 0.45f : 1.0f);
    return RoleTuning(tuning, role).engageRange + difficultyBonus * roleScale;
}

int BotDesiredBlocks(BotRole role, const BotTuningGenome& tuning)
{
    return static_cast<int>(std::round(RoleTuning(tuning, role).desiredBlocks));
}

int BotLootReturnValue(BotRole role, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    int value = static_cast<int>(std::round(RoleTuning(tuning, role).lootReturnValue));
    if (difficulty == BotDifficulty::Easy)
    {
        value -= 4;
    }
    else if (difficulty == BotDifficulty::Hard)
    {
        value += 8;
    }
    return value;
}

float IntentLockSeconds(BotIntent intent, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    const float difficultyScale = difficulty == BotDifficulty::Hard ? 0.78f : (difficulty == BotDifficulty::Easy ? 1.22f : 1.0f);
    const float tuningScale = tuning.intentLockScale;
    switch (intent)
    {
    case BotIntent::DefendCore:
        return 0.35f * tuningScale;
    case BotIntent::FightEnemy:
    case BotIntent::ChaseWeakEnemy:
        return 0.55f * difficultyScale * tuningScale;
    case BotIntent::RetreatHome:
        return 1.10f * difficultyScale * tuningScale;
    case BotIntent::GearUp:
        return 1.45f * difficultyScale * tuningScale;
    case BotIntent::PressureCore:
    case BotIntent::BreakCoreDefense:
        return 1.05f * difficultyScale * tuningScale;
    case BotIntent::SecureResources:
        return 1.20f * difficultyScale * tuningScale;
    case BotIntent::RepairCoreDefense:
        return 0.90f * difficultyScale * tuningScale;
    case BotIntent::Recover:
        return 0.32f;
    }
    return 0.8f * tuningScale;
}

BotState StateForIntent(BotIntent intent)
{
    switch (intent)
    {
    case BotIntent::DefendCore:
    case BotIntent::FightEnemy:
    case BotIntent::ChaseWeakEnemy:
        return BotState::Fight;
    case BotIntent::GearUp:
        return BotState::Shop;
    case BotIntent::SecureResources:
        return BotState::Collect;
    case BotIntent::PressureCore:
        return BotState::AttackCore;
    case BotIntent::BreakCoreDefense:
        return BotState::BreakDefense;
    case BotIntent::RetreatHome:
    case BotIntent::RepairCoreDefense:
        return BotState::Retreat;
    case BotIntent::Recover:
        return BotState::Bridge;
    }
    return BotState::Bridge;
}

struct BotDecision
{
    BotIntent intent = BotIntent::SecureResources;
    BotState state = BotState::Collect;
    Vector3 target {};
    Player* fightTarget = nullptr;
    float score = -100000.0f;
    const char* reason = "";
};

struct BotTeamSnapshot
{
    int aliveAllies = 0;
    int alliesNearCore = 0;
    int defendersNearCore = 0;
    int activePressure = 0;
    int activeShop = 0;
    int activeRepair = 0;
    int activeResource = 0;
};

struct BotRoleDistribution
{
    int defenders = 0;
    int rushers = 0;
    int collectors = 0;
    int fighters = 0;
    int total = 0;
};

struct BotResourcePlan
{
    Vector3 target {};
    ResourceType type = ResourceType::Iron;
    bool hasTarget = false;
    float score = std::numeric_limits<float>::max();
};

enum class BotStrategicFocus
{
    Economy,
    Defense,
    Pressure,
    Cleanup
};

struct BotStrategicPlan
{
    BotStrategicFocus focus = BotStrategicFocus::Economy;
    int attackCoreTeamId = -1;
    int cleanupHunterId = -1;
    int cleanupTargetTeamId = -1;
    int cleanupTargetAlivePlayers = 0;
    Vector3 cleanupTargetPosition {};
    bool cleanupTargetRecentlySeen = false;
    int desiredAttackers = 1;
    int desiredDefenders = 1;
    int primaryDefenderId = -1;
    int secondaryDefenderId = -1;
    float attackUrgency = 0.0f;
    float defenseUrgency = 0.0f;
    bool allIn = false;
};

const BotMemory* FindBotMemoryByPlayerId(
    const std::vector<BotMemory>& memories,
    const std::unordered_map<int, std::size_t>& memoryIndex,
    int playerId)
{
    const auto foundIndex = memoryIndex.find(playerId);
    if (foundIndex != memoryIndex.end())
    {
        const std::size_t index = foundIndex->second;
        if (index < memories.size() && memories[index].playerId == playerId)
        {
            return &memories[index];
        }
    }

    for (const BotMemory& memory : memories)
    {
        if (memory.playerId == playerId)
        {
            return &memory;
        }
    }
    return nullptr;
}

BotRole ResolveRoleForPlayerId(
    int playerId,
    const std::vector<BotMemory>& memories,
    const std::unordered_map<int, std::size_t>& memoryIndex)
{
    if (const BotMemory* memory = FindBotMemoryByPlayerId(memories, memoryIndex, playerId))
    {
        return memory->role;
    }
    return RoleForBotId(playerId);
}

BotRoleDistribution BuildRoleDistributionForTeam(
    int teamId,
    const std::vector<Player>& players,
    const std::vector<BotMemory>& memories,
    const std::unordered_map<int, std::size_t>& memoryIndex,
    int ignorePlayerId = -1)
{
    BotRoleDistribution distribution {};
    for (const Player& player : players)
    {
        if (player.GetId() == ignorePlayerId
            || player.GetTeamId() != teamId
            || !IsBotControlled(player.GetControlKind())
            || !player.IsAlive()
            || player.IsEliminated())
        {
            continue;
        }

        ++distribution.total;
        switch (ResolveRoleForPlayerId(player.GetId(), memories, memoryIndex))
        {
        case BotRole::Defender:
            ++distribution.defenders;
            break;
        case BotRole::Rusher:
            ++distribution.rushers;
            break;
        case BotRole::Collector:
            ++distribution.collectors;
            break;
        case BotRole::Fighter:
            ++distribution.fighters;
            break;
        }
    }
    return distribution;
}

void RemoveRoleFromDistribution(BotRoleDistribution& distribution, BotRole role)
{
    distribution.total = std::max(0, distribution.total - 1);
    switch (role)
    {
    case BotRole::Defender:
        distribution.defenders = std::max(0, distribution.defenders - 1);
        break;
    case BotRole::Rusher:
        distribution.rushers = std::max(0, distribution.rushers - 1);
        break;
    case BotRole::Collector:
        distribution.collectors = std::max(0, distribution.collectors - 1);
        break;
    case BotRole::Fighter:
        distribution.fighters = std::max(0, distribution.fighters - 1);
        break;
    }
}

void RemoveIntentFromSnapshot(BotTeamSnapshot& snapshot, BotIntent intent)
{
    switch (intent)
    {
    case BotIntent::PressureCore:
    case BotIntent::BreakCoreDefense:
    case BotIntent::ChaseWeakEnemy:
        snapshot.activePressure = std::max(0, snapshot.activePressure - 1);
        break;
    case BotIntent::GearUp:
        snapshot.activeShop = std::max(0, snapshot.activeShop - 1);
        break;
    case BotIntent::RepairCoreDefense:
        snapshot.activeRepair = std::max(0, snapshot.activeRepair - 1);
        break;
    case BotIntent::SecureResources:
        snapshot.activeResource = std::max(0, snapshot.activeResource - 1);
        break;
    case BotIntent::DefendCore:
    case BotIntent::FightEnemy:
    case BotIntent::RetreatHome:
    case BotIntent::Recover:
        break;
    }
}

float RoleLockSeconds(BotRole role, BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    const float scale = difficulty == BotDifficulty::Hard ? 0.82f : (difficulty == BotDifficulty::Easy ? 1.22f : 1.0f);
    return RoleTuning(tuning, role).roleLockSeconds * scale * tuning.roleLockScale;
}

float RoleIntentBias(BotRole role, BotIntent intent, const BotTuningGenome& tuning)
{
    float bias = 0.0f;
    switch (role)
    {
    case BotRole::Defender:
        switch (intent)
        {
        case BotIntent::DefendCore:
            bias = 210.0f;
            break;
        case BotIntent::RepairCoreDefense:
            bias = 260.0f;
            break;
        case BotIntent::FightEnemy:
            bias = 70.0f;
            break;
        case BotIntent::PressureCore:
        case BotIntent::BreakCoreDefense:
            bias = -180.0f;
            break;
        case BotIntent::SecureResources:
            bias = -35.0f;
            break;
        case BotIntent::GearUp:
        case BotIntent::ChaseWeakEnemy:
        case BotIntent::RetreatHome:
        case BotIntent::Recover:
            break;
        }
        break;
    case BotRole::Rusher:
        switch (intent)
        {
        case BotIntent::PressureCore:
            bias = 230.0f;
            break;
        case BotIntent::BreakCoreDefense:
            bias = 210.0f;
            break;
        case BotIntent::FightEnemy:
            bias = 80.0f;
            break;
        case BotIntent::DefendCore:
            bias = -95.0f;
            break;
        case BotIntent::RepairCoreDefense:
            bias = -170.0f;
            break;
        case BotIntent::SecureResources:
            bias = -135.0f;
            break;
        case BotIntent::GearUp:
        case BotIntent::ChaseWeakEnemy:
        case BotIntent::RetreatHome:
        case BotIntent::Recover:
            break;
        }
        break;
    case BotRole::Collector:
        switch (intent)
        {
        case BotIntent::SecureResources:
            bias = 260.0f;
            break;
        case BotIntent::GearUp:
            bias = 150.0f;
            break;
        case BotIntent::RetreatHome:
            bias = 90.0f;
            break;
        case BotIntent::PressureCore:
        case BotIntent::BreakCoreDefense:
            bias = -210.0f;
            break;
        case BotIntent::ChaseWeakEnemy:
            bias = -70.0f;
            break;
        case BotIntent::DefendCore:
            bias = -60.0f;
            break;
        case BotIntent::RepairCoreDefense:
            bias = -45.0f;
            break;
        case BotIntent::FightEnemy:
        case BotIntent::Recover:
            break;
        }
        break;
    case BotRole::Fighter:
        switch (intent)
        {
        case BotIntent::FightEnemy:
            bias = 230.0f;
            break;
        case BotIntent::ChaseWeakEnemy:
            bias = 210.0f;
            break;
        case BotIntent::PressureCore:
            bias = 95.0f;
            break;
        case BotIntent::BreakCoreDefense:
            bias = 75.0f;
            break;
        case BotIntent::SecureResources:
            bias = -110.0f;
            break;
        case BotIntent::DefendCore:
            bias = -35.0f;
            break;
        case BotIntent::RepairCoreDefense:
            bias = -130.0f;
            break;
        case BotIntent::GearUp:
        case BotIntent::RetreatHome:
        case BotIntent::Recover:
            break;
        }
        break;
    }

    const BotRoleTuning& roleTuning = RoleTuning(tuning, role);
    switch (intent)
    {
    case BotIntent::PressureCore:
    case BotIntent::BreakCoreDefense:
        return bias * roleTuning.pressureBiasScale;
    case BotIntent::DefendCore:
    case BotIntent::RepairCoreDefense:
        return bias * roleTuning.defenseBiasScale;
    case BotIntent::SecureResources:
    case BotIntent::GearUp:
    case BotIntent::RetreatHome:
        return bias * roleTuning.resourceBiasScale;
    case BotIntent::FightEnemy:
    case BotIntent::ChaseWeakEnemy:
        return bias * roleTuning.combatBiasScale;
    case BotIntent::Recover:
        break;
    }
    return bias;
}

struct BotRoleDecision
{
    BotRole role = BotRole::Rusher;
    bool force = false;
    const char* reason = "";
};

BotRoleDecision EvaluateDynamicRoleDecision(
    const Player& bot,
    const Team& team,
    const BotMemory& memory,
    const BotRoleDistribution& distribution,
    bool carryingLoot,
    bool wantsShop,
    Player* enemyAtCore,
    EnergyCore* enemyCore,
    float matchTime,
    const Inventory& inventory)
{
    BotRoleDecision decision {};
    decision.role = memory.role;
    decision.reason = "keep role";

    const bool emergencyDefense = team.coreAlive && enemyAtCore != nullptr;
    const bool lowHealth = bot.GetHealth() < 44;
    const bool lowBlocks = inventory.GetBlocks() < 8;
    const bool lowCombat = inventory.GetSwordLevel() <= 0 && inventory.GetToolLevel() <= 0;
    const bool nearEnemyCore = enemyCore != nullptr
        && enemyCore->IsAlive()
        && DistanceSquared(bot.GetPosition(), Vector3 {
            static_cast<float>(enemyCore->GetBlockPosition().x),
            static_cast<float>(enemyCore->GetBlockPosition().y),
            static_cast<float>(enemyCore->GetBlockPosition().z) }) < 170.0f;
    const int teamSize = std::max(1, distribution.total + 1);
    const int desiredDefenders = team.coreAlive ? std::min(teamSize, emergencyDefense ? 2 : 1) : 0;
    int desiredCollectors = !team.coreAlive
        ? std::max(1, teamSize / 3)
        : (matchTime < 100.0f ? std::max(1, teamSize / 3) : std::max(1, teamSize / 4));
    int desiredRushers = !team.coreAlive
        ? std::max(1, teamSize / 2)
        : (matchTime < 85.0f ? std::max(1, teamSize / 4) : std::max(1, teamSize / 3));
    desiredCollectors = std::min(desiredCollectors, std::max(0, teamSize - desiredDefenders));
    desiredRushers = std::min(desiredRushers, std::max(0, teamSize - desiredDefenders - desiredCollectors));
    const int desiredFighters = std::max(0, teamSize - desiredDefenders - desiredCollectors - desiredRushers);
    const auto roleCountWithSelf = [&](BotRole role)
    {
        int count = 0;
        switch (role)
        {
        case BotRole::Defender:
            count = distribution.defenders;
            break;
        case BotRole::Rusher:
            count = distribution.rushers;
            break;
        case BotRole::Collector:
            count = distribution.collectors;
            break;
        case BotRole::Fighter:
            count = distribution.fighters;
            break;
        }
        if (memory.role == role)
        {
            ++count;
        }
        return count;
    };
    const auto roleDeficit = [&](BotRole role)
    {
        int desired = 0;
        switch (role)
        {
        case BotRole::Defender:
            desired = desiredDefenders;
            break;
        case BotRole::Rusher:
            desired = desiredRushers;
            break;
        case BotRole::Collector:
            desired = desiredCollectors;
            break;
        case BotRole::Fighter:
            desired = desiredFighters;
            break;
        }
        return desired - roleCountWithSelf(role);
    };
    // Voluntary role hops are only allowed when the current role is overstaffed,
    // otherwise bots ping-pong between roles chasing each other's deficits.
    const bool ownRoleSurplus = roleDeficit(memory.role) < 0;

    if (!team.coreAlive)
    {
        if (memory.role == BotRole::Defender)
        {
            decision.role = BotRole::Fighter;
            decision.force = true;
            decision.reason = "core destroyed";
            return decision;
        }
        if ((carryingLoot || lowHealth) && distribution.collectors < std::max(1, distribution.total / 3))
        {
            decision.role = BotRole::Collector;
            decision.reason = "recover economy";
            return decision;
        }
        if (nearEnemyCore && distribution.rushers < std::max(1, distribution.total / 2))
        {
            decision.role = BotRole::Rusher;
            decision.reason = "final push";
            return decision;
        }

        decision.role = BotRole::Fighter;
        decision.reason = "final combat";
        return decision;
    }

    if (emergencyDefense
        && roleDeficit(BotRole::Defender) > 0)
    {
        decision.role = BotRole::Defender;
        decision.force = enemyAtCore != nullptr;
        decision.reason = enemyAtCore != nullptr ? "base under attack" : "repair core";
        return decision;
    }

    if (ownRoleSurplus
        && roleDeficit(BotRole::Collector) > 0
        && (carryingLoot || wantsShop || lowBlocks || lowHealth))
    {
        decision.role = BotRole::Collector;
        decision.reason = "gather and gear";
        return decision;
    }

    if (ownRoleSurplus
        && roleDeficit(BotRole::Rusher) > 0
        && (nearEnemyCore || !lowCombat || matchTime > 75.0f))
    {
        decision.role = BotRole::Rusher;
        decision.reason = nearEnemyCore ? "pressure core" : "open lane";
        return decision;
    }

    if (ownRoleSurplus && roleDeficit(BotRole::Fighter) > 0)
    {
        decision.role = BotRole::Fighter;
        decision.reason = "mid control";
        return decision;
    }

    if (ownRoleSurplus && roleDeficit(BotRole::Defender) > 0)
    {
        decision.role = BotRole::Defender;
        decision.reason = "maintain defense";
        return decision;
    }

    return decision;
}

bool TryApplyDynamicRoleDecision(
    BotMemory& memory,
    const BotRoleDecision& decision,
    BotDifficulty difficulty,
    const BotTuningGenome& tuning)
{
    if (decision.role == memory.role)
    {
        return false;
    }
    if (!decision.force && memory.roleLockTimer > 0.0f)
    {
        return false;
    }
    const float minimumRoleTime = difficulty == BotDifficulty::Hard
        ? 6.0f
        : (difficulty == BotDifficulty::Easy ? 9.0f : 7.5f);
    if (!decision.force && memory.roleTimer < minimumRoleTime)
    {
        return false;
    }

    memory.role = decision.role;
    memory.roleTimer = 0.0f;
    memory.roleLockTimer = RoleLockSeconds(decision.role, difficulty, tuning);
    memory.roleReason = decision.reason;
    memory.intentLockTimer = 0.0f;
    memory.hasNavWaypoint = false;
    memory.hasBreakTarget = false;
    memory.breakProgress = 0.0f;
    memory.stuckTimer = 0.0f;
    return true;
}

void TickBotMemory(BotMemory& memory, float dt)
{
    memory.stateTimer += dt;
    memory.intentTimer += dt;
    memory.intentLockTimer = std::max(0.0f, memory.intentLockTimer - dt);
    memory.attackTimer = std::max(0.0f, memory.attackTimer - dt);
    memory.retreatTimer = std::max(0.0f, memory.retreatTimer - dt);
    memory.strafeTimer = std::max(0.0f, memory.strafeTimer - dt);
    memory.bridgePlaceCooldown = std::max(0.0f, memory.bridgePlaceCooldown - dt);
    memory.utilityTimer = std::max(0.0f, memory.utilityTimer - dt);
    memory.jumpTimer = std::max(0.0f, memory.jumpTimer - dt);
    memory.defenseCheckTimer = std::max(0.0f, memory.defenseCheckTimer - dt);
    memory.strategicUpdateTimer = std::max(0.0f, memory.strategicUpdateTimer - dt);
    memory.currentPlan.elapsedTime += dt;
    if (memory.currentPlan.goal != StrategicGoal::Idle)
    {
        memory.totalPlanHoldSeconds += dt;
    }
    memory.roleTimer += dt;
    memory.roleLockTimer = std::max(0.0f, memory.roleLockTimer - dt);
    memory.chaseBanTimer = std::max(0.0f, memory.chaseBanTimer - dt);
    memory.heroAbilityTimer = std::max(0.0f, memory.heroAbilityTimer - dt);
    memory.repairPlaceCooldown = std::max(0.0f, memory.repairPlaceCooldown - dt);
    memory.reactionDelayTimer = std::max(0.0f, memory.reactionDelayTimer - dt);
    memory.perceptionAcquireTimer = std::max(0.0f, memory.perceptionAcquireTimer - dt);
    memory.enemyMemoryConfidence = std::max(0.0f, memory.enemyMemoryConfidence - dt * 0.16f);
    if (memory.enemyMemoryConfidence <= 0.0f)
    {
        memory.rememberedEnemyId = -1;
    }
    memory.abandonedPlanCooldown = std::max(0.0f, memory.abandonedPlanCooldown - dt);
    memory.routeFailureCooldown = std::max(0.0f, memory.routeFailureCooldown - dt);
    memory.bridgeHelpRequestCooldown = std::max(0.0f, memory.bridgeHelpRequestCooldown - dt);
    memory.routeCorridorReplanCooldown = std::max(0.0f, memory.routeCorridorReplanCooldown - dt);
    memory.recentCoreAttackTimer = std::max(0.0f, memory.recentCoreAttackTimer - dt);
    if (memory.recentCoreAttackTimer <= 0.0f)
    {
        memory.recentCoreAttackerId = -1;
    }
    memory.resourcePlanTimer = std::max(0.0f, memory.resourcePlanTimer - dt);
    memory.tacticalCheckTimer = std::max(0.0f, memory.tacticalCheckTimer - dt);
}

BotTeamSnapshot BuildBotTeamSnapshot(
    const Player& bot,
    Vector3 coreHome,
    const std::vector<Player>& players,
    const std::vector<BotMemory>& memories,
    const std::unordered_map<int, std::size_t>& memoryIndex)
{
    BotTeamSnapshot snapshot {};
    for (const Player& ally : players)
    {
        if (ally.GetId() == bot.GetId()
            || ally.GetTeamId() != bot.GetTeamId()
            || !ally.IsAlive()
            || ally.IsEliminated())
        {
            continue;
        }

        ++snapshot.aliveAllies;
        const bool nearCore = DistanceSquared(ally.GetPosition(), coreHome) < 72.0f;
        if (nearCore)
        {
            ++snapshot.alliesNearCore;
        }

        const BotMemory* allyMemory = FindBotMemoryByPlayerId(memories, memoryIndex, ally.GetId());
        if (allyMemory == nullptr)
        {
            continue;
        }

        if (nearCore && allyMemory->role == BotRole::Defender)
        {
            ++snapshot.defendersNearCore;
        }

        if (allyMemory->intent == BotIntent::PressureCore
            || allyMemory->intent == BotIntent::BreakCoreDefense
            || allyMemory->intent == BotIntent::ChaseWeakEnemy)
        {
            ++snapshot.activePressure;
        }
        else if (allyMemory->intent == BotIntent::GearUp)
        {
            ++snapshot.activeShop;
        }
        else if (allyMemory->intent == BotIntent::RepairCoreDefense)
        {
            ++snapshot.activeRepair;
        }
        else if (allyMemory->intent == BotIntent::SecureResources)
        {
            ++snapshot.activeResource;
        }
    }

    return snapshot;
}

Player* FindEnemyNearCore(Player& bot, std::vector<Player>& players, Vector3 coreHome, float& enemyDistanceSq)
{
    Player* enemyAtCore = nullptr;
    enemyDistanceSq = std::numeric_limits<float>::max();
    for (Player& other : players)
    {
        if (other.GetTeamId() == bot.GetTeamId()
            || other.GetId() == bot.GetId()
            || !other.IsAlive()
            || other.IsEliminated())
        {
            continue;
        }

        const float coreDistance = DistanceSquared(other.GetPosition(), coreHome);
        if (coreDistance < 42.0f && coreDistance < enemyDistanceSq)
        {
            enemyAtCore = &other;
            enemyDistanceSq = coreDistance;
        }
    }

    return enemyAtCore;
}

bool ShouldBotShop(
    const Player& bot,
    const Inventory& inventory,
    const Team& team,
    const BotMemory& memory,
    BotDifficulty difficulty,
    const BotTuningGenome& tuning)
{
    const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, difficulty, tuning);
    const int desiredBlocks = BotDesiredBlocks(memory.role, tuning);
    const bool canBuyBlocks = inventory.GetResource(ResourceType::Iron) >= 8;
    const bool hasObsidianMoney = inventory.GetResource(ResourceType::Gold) >= 8
        && inventory.GetResource(ResourceType::Crystal) >= 3;

    return (inventory.GetBlocks() < desiredBlocks && canBuyBlocks)
        || (memory.role != BotRole::Defender
            && inventory.GetToolLevel() < 2
            && inventory.GetResource(ResourceType::Iron) >= 8
            && inventory.GetResource(ResourceType::Crystal) >= 1)
        || (memory.role == BotRole::Fighter
            && inventory.GetSwordLevel() < 2
            && inventory.GetResource(ResourceType::Gold) >= 6)
        || (memory.role == BotRole::Defender
            && inventory.GetBlockCount(BlockType::StoneBlock) < 16
            && inventory.GetResource(ResourceType::Iron) >= 18)
        || (memory.role == BotRole::Defender
            && inventory.GetBlockCount(BlockType::ObsidianBlock) < 8
            && hasObsidianMoney)
        || (memory.role == BotRole::Collector
            && team.forgeLevel < 4
            && inventory.GetResource(ResourceType::Crystal) >= 4
            && inventory.GetResource(ResourceType::Gold) >= 2)
        || (memory.role == BotRole::Collector
            && (inventory.GetResource(ResourceType::Crystal) >= 3
                || inventory.GetResource(ResourceType::Gold) >= 8
                || memory.carriedResourceValue >= BotLootReturnValue(memory.role, difficulty, tuning)))
        || (bot.GetHealth() < 70 && inventory.GetResource(ResourceType::Crystal) >= 3)
        || carryingLoot;
}

BotResourcePlan BuildBotResourcePlan(
    const Player& bot,
    const ResourcePickup* bestPickup,
    const std::vector<Generator>& generators,
    BotRole role,
    bool ruinsBiome,
    const std::vector<Vector3>* teammateClaims = nullptr)
{
    BotResourcePlan plan {};
    if (bestPickup != nullptr)
    {
        plan.target = ToVector3(bestPickup->position);
        plan.type = bestPickup->type;
        plan.hasTarget = true;
        plan.score = DistanceSquared(bot.GetPosition(), plan.target);
    }

    const auto claimedPenalty = [teammateClaims](Vector3 pos) -> float
    {
        if (teammateClaims == nullptr)
        {
            return 0.0f;
        }
        for (const Vector3& claim : *teammateClaims)
        {
            if (DistanceSquared(claim, pos) < 6.25f)
            {
                // A teammate already camps this generator: a mild nudge to
                // spread out. Kept SOFT on purpose — a hard penalty (260) drove
                // bots off the few efficient neutral generators and collapsed
                // the whole team economy (arena coreDamage halved).
                return 70.0f;
            }
        }
        return 0.0f;
    };

    for (const Generator& generator : generators)
    {
        if (generator.GetTeamId() != -1)
        {
            continue;
        }

        const ResourceType type = generator.GetType();
        const Vector3 generatorPos = ToVector3(generator.GetPosition());
        float score = DistanceSquared(bot.GetPosition(), generatorPos);
        if (type == ResourceType::Crystal)
        {
            score -= role == BotRole::Collector ? 420.0f : 180.0f;
        }
        else if (type == ResourceType::Gold)
        {
            score -= role == BotRole::Fighter ? 120.0f : 70.0f;
        }
        if (role == BotRole::Collector)
        {
            score -= DistanceSquared(generatorPos, Vector3 { 0.0f, generatorPos.y, 0.0f }) * 0.08f;
        }
        if (ruinsBiome
            && role == BotRole::Collector
            && std::fabs(generatorPos.x) >= 18.0f
            && std::fabs(generatorPos.z) >= 18.0f)
        {
            score -= type == ResourceType::Crystal ? 520.0f : 320.0f;
        }
        score += claimedPenalty(generatorPos);

        if (!plan.hasTarget || score < plan.score)
        {
            plan.target = generatorPos;
            plan.type = type;
            plan.hasTarget = true;
            plan.score = score;
        }
    }

    return plan;
}

const ResourcePickup* FindBestPickupForBot(
    const Player& bot,
    BotRole role,
    const std::vector<ResourcePickup>& pickups,
    const std::vector<Player*>& enemies,
    Vector3 coreHome,
    bool ruinsBiome,
    bool allowHomePickup,
    const std::vector<Vector3>* teammateClaims = nullptr)
{
    const ResourcePickup* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();
    const Inventory& inventory = bot.GetInventory();

    for (const ResourcePickup& pickup : pickups)
    {
        if (pickup.collected)
        {
            continue;
        }
        const Vector3 pickupPos = ToVector3(pickup.position);
        if (!allowHomePickup && DistanceSquared(pickupPos, coreHome) <= 144.0f)
        {
            continue;
        }

        float score = DistanceSquared(bot.GetPosition(), pickupPos);
        // A teammate already farming this spot: prefer a different one (soft
        // penalty — a lone crystal is still worth sharing).
        if (teammateClaims != nullptr)
        {
            for (const Vector3& claim : *teammateClaims)
            {
                if (DistanceSquared(claim, pickupPos) < 6.25f)
                {
                    score += 45.0f;
                    break;
                }
            }
        }
        if (pickup.type == ResourceType::Crystal)
        {
            score -= role == BotRole::Collector ? 150.0f : 90.0f;
        }
        else if (pickup.type == ResourceType::Gold)
        {
            score -= role == BotRole::Fighter ? 65.0f : 35.0f;
        }
        else if (inventory.GetBlocks() < 24)
        {
            score -= 30.0f;
        }

        for (const Player* enemy : enemies)
        {
            if (enemy == nullptr)
            {
                continue;
            }

            const float enemyDistance = DistanceSquared(enemy->GetPosition(), pickupPos);
            if (enemyDistance < 18.0f)
            {
                score += role == BotRole::Collector ? 120.0f : 42.0f;
            }
            else if (enemyDistance < 42.0f && role == BotRole::Collector && bot.GetHealth() < 74)
            {
                score += 48.0f;
            }
        }

        if (role == BotRole::Rusher && pickup.type == ResourceType::Iron && inventory.GetBlocks() >= 24)
        {
            score += 80.0f;
        }
        if (role == BotRole::Defender)
        {
            score += DistanceSquared(pickupPos, coreHome) * 0.35f;
        }
        if (ruinsBiome
            && role == BotRole::Collector
            && std::fabs(pickupPos.x) >= 18.0f
            && std::fabs(pickupPos.z) >= 18.0f)
        {
            score -= pickup.type == ResourceType::Crystal ? 360.0f : 220.0f;
        }

        if (score < bestScore)
        {
            bestScore = score;
            best = &pickup;
        }
    }

    return best;
}

// A chase that has not closed the gap for a while gets banned for a few
// seconds: endless pursuits are what stalls the late game. Targets already
// in melee reach are always worth finishing regardless of the ban.
bool ChaseBlockedByFutility(const BotMemory& memory, const Player* target, Vector3 botPos)
{
    if (memory.chaseBanTimer <= 0.0f || target == nullptr)
    {
        return false;
    }
    return DistanceSquared(botPos, target->GetPosition()) > 49.0f;
}

struct BotMacroDirective
{
    bool active = false;
    BotIntent intent = BotIntent::SecureResources;
    Vector3 target {};
    Player* fightTarget = nullptr;
    const char* reason = "";
};

struct BotDecisionContext
{
    const Player& bot;
    const Team& team;
    const Inventory& inventory;
    const BotMemory& memory;
    const BotTuningGenome& tuning;
    BotDifficulty difficulty = BotDifficulty::Normal;
    float matchTime = 0.0f;
    Player* nearbyEnemy = nullptr;
    Player* weakEnemy = nullptr;
    Player* huntEnemy = nullptr;
    Player* enemyAtCore = nullptr;
    EnergyCore* enemyCore = nullptr;
    BotTeamSnapshot teamPlan {};
    Vector3 botPos {};
    Vector3 coreHome {};
    Vector3 resourceTarget {};
    float enemyAtCoreDistance = std::numeric_limits<float>::max();
    float distanceFromHome = 0.0f;
    float nearbyEnemyDistance = 999.0f;
    FightAssessment nearbyFight {};
    int retreatHealth = 0;
    int fightHealth = 0;
    int desiredBlocks = 0;
    ResourceType resourceTargetType = ResourceType::Iron;
    bool retreating = false;
    bool defenderAwayFromBase = false;
    bool carryingLoot = false;
    bool wantsShop = false;
    bool canBreakDefense = false;
    bool readyToRush = false;
    bool coreNeedsRepair = false;
    bool coreDefenseCritical = false;
    bool coreCanUpgrade = false;
    int aliveEnemyCores = 0;
    BotStrategicPlan strategicPlan {};
    bool finalDuelPhase = false;
    bool huntEnemyOnFinalLife = false;
    bool finalLifeTargetClose = false;
    bool shouldFightNearby = false;
    bool shouldPressureCore = false;
    bool hasResourceTarget = false;
    int coordinatedAttackersOnTarget = 0;
    int coordinatedDefenders = 0;
    int coordinatedHelpCalls = 0;
    int missingDefenseBlocks = 0;
    int currentPlanTargetAlivePlayers = 0;
};

bool IsAssignedEmergencyDefender(const BotDecisionContext& ctx)
{
    return ctx.bot.GetId() == ctx.strategicPlan.primaryDefenderId
        || ctx.bot.GetId() == ctx.strategicPlan.secondaryDefenderId;
}

float StrategicPlanUpdateCadence(BotDifficulty difficulty, const BotTuningGenome& tuning)
{
    switch (difficulty)
    {
    case BotDifficulty::Easy:
        return tuning.easyPlanCadence;
    case BotDifficulty::Hard:
        return tuning.hardPlanCadence;
    case BotDifficulty::Normal:
        break;
    }
    return tuning.normalPlanCadence;
}

bool StrategicPlanExpired(const StrategicPlan& plan)
{
    return plan.plannedDuration > 0.0f && plan.elapsedTime >= plan.plannedDuration;
}

Vector3 CoreTargetPosition(const EnergyCore& core);

bool AdvanceStrategicPlan(StrategicPlan& plan, const BotDecisionContext& ctx)
{
    if (plan.goal == StrategicGoal::Idle || plan.stageCount <= 1)
    {
        return false;
    }
    const int previousStage = plan.stage;
    switch (plan.goal)
    {
    case StrategicGoal::EconomicPhase:
        if (plan.stage == 0 && (ctx.carryingLoot || ctx.inventory.GetResource(ResourceType::Iron) >= 8)) plan.stage = 1;
        else if (plan.stage == 1 && (ctx.readyToRush || !ctx.wantsShop)) plan.stage = 2;
        break;
    case StrategicGoal::BridgePush:
        if (plan.stage == 0 && ctx.inventory.GetBlocks() >= 8) plan.stage = 1;
        else if (plan.stage == 1 && ctx.readyToRush) plan.stage = 2;
        else if (plan.stage == 2 && ctx.enemyCore != nullptr
            && DistanceSquared(ctx.botPos, CoreTargetPosition(*ctx.enemyCore)) < 150.0f) plan.stage = 3;
        break;
    case StrategicGoal::CoreAssault:
        if (plan.stage == 0 && ctx.enemyCore != nullptr
            && DistanceSquared(ctx.botPos, CoreTargetPosition(*ctx.enemyCore)) < 170.0f) plan.stage = 1;
        else if (plan.stage == 1 && ctx.enemyCore != nullptr
            && DistanceSquared(ctx.botPos, CoreTargetPosition(*ctx.enemyCore)) < 50.0f
            && !ctx.canBreakDefense) plan.stage = 2;
        break;
    case StrategicGoal::BaseDefense:
        if (plan.stage == 0 && ctx.distanceFromHome < 50.0f) plan.stage = 1;
        else if (plan.stage == 1 && !ctx.coreNeedsRepair && ctx.enemyAtCore == nullptr) plan.stage = 2;
        break;
    case StrategicGoal::HuntPlayers:
        if (plan.stage == 0 && ctx.huntEnemy != nullptr
            && DistanceSquared(ctx.botPos, ctx.huntEnemy->GetPosition()) < 90.0f) plan.stage = 1;
        else if (plan.stage == 0
            && plan.targetTeamId == ctx.strategicPlan.cleanupTargetTeamId
            && DistanceSquared(ctx.botPos, ctx.strategicPlan.cleanupTargetPosition) < 90.0f) plan.stage = 1;
        else if (plan.stage == 1 && ctx.currentPlanTargetAlivePlayers <= 0) plan.stage = 2;
        break;
    case StrategicGoal::MidControl:
        if (plan.stage == 0 && ctx.distanceFromHome > 180.0f) plan.stage = 1;
        break;
    case StrategicGoal::Idle:
        break;
    }
    return plan.stage != previousStage;
}

enum class StrategicInterruptReason : std::uint8_t
{
    None,
    Idle,
    Expired,
    EmergencyDefense,
    FinalLifeCleanup,
    AssaultTargetInvalid,
    RouteFailures,
    EconomyReady,
    CleanupAssignmentChanged
};

const char* ToString(StrategicInterruptReason reason)
{
    switch (reason)
    {
    case StrategicInterruptReason::None: return "none";
    case StrategicInterruptReason::Idle: return "idle";
    case StrategicInterruptReason::Expired: return "expired";
    case StrategicInterruptReason::EmergencyDefense: return "emergency-defense";
    case StrategicInterruptReason::FinalLifeCleanup: return "final-life-cleanup";
    case StrategicInterruptReason::AssaultTargetInvalid: return "assault-target-invalid";
    case StrategicInterruptReason::RouteFailures: return "route-failures";
    case StrategicInterruptReason::EconomyReady: return "economy-ready";
    case StrategicInterruptReason::CleanupAssignmentChanged: return "cleanup-assignment-changed";
    }
    return "unknown";
}

StrategicInterruptReason StrategicPlanInterruptReason(
    const StrategicPlan& plan,
    const BotDecisionContext& ctx)
{
    if (plan.goal == StrategicGoal::Idle)
    {
        return StrategicInterruptReason::Idle;
    }
    // Commitment means "finish this objective", not "restart the same plan
    // when its estimate expires". A committed push is still interruptible by
    // an emergency, a dead target, or repeated physical route failures below.
    if (StrategicPlanExpired(plan) && !plan.committed)
    {
        return StrategicInterruptReason::Expired;
    }

    const bool assignedEmergencyDefender = IsAssignedEmergencyDefender(ctx);
    const bool emergencyDefense = ctx.team.coreAlive
        && assignedEmergencyDefender
        && (ctx.enemyAtCore != nullptr
            || ctx.memory.recentCoreAttackTimer > 0.0f
            || (ctx.coreDefenseCritical && ctx.matchTime > 55.0f));
    if (emergencyDefense && plan.goal != StrategicGoal::BaseDefense)
    {
        return StrategicInterruptReason::EmergencyDefense;
    }
    if (plan.elapsedTime < plan.minimumCommitSeconds)
    {
        return StrategicInterruptReason::None;
    }
    if (ctx.strategicPlan.cleanupHunterId == ctx.bot.GetId()
        && ctx.strategicPlan.cleanupTargetTeamId >= 0
        && ctx.strategicPlan.cleanupTargetAlivePlayers > 0
        && plan.goal != StrategicGoal::HuntPlayers)
    {
        return StrategicInterruptReason::FinalLifeCleanup;
    }
    if (ctx.huntEnemyOnFinalLife
        && ctx.huntEnemy != nullptr
        && ctx.matchTime > 135.0f
        && ctx.finalLifeTargetClose
        && ctx.aliveEnemyCores == 0
        && plan.goal != StrategicGoal::HuntPlayers)
    {
        return StrategicInterruptReason::FinalLifeCleanup;
    }

    if (plan.goal == StrategicGoal::HuntPlayers
        && ctx.aliveEnemyCores > 0
        && (ctx.strategicPlan.cleanupHunterId != ctx.bot.GetId()
            || ctx.strategicPlan.cleanupTargetTeamId != plan.targetTeamId))
    {
        return StrategicInterruptReason::CleanupAssignmentChanged;
    }

    if ((plan.goal == StrategicGoal::BridgePush || plan.goal == StrategicGoal::CoreAssault)
        && (ctx.enemyCore == nullptr
            || !ctx.enemyCore->IsAlive()
            || (plan.targetTeamId >= 0 && ctx.enemyCore->GetTeamId() != plan.targetTeamId)))
    {
        return StrategicInterruptReason::AssaultTargetInvalid;
    }
    if ((plan.goal == StrategicGoal::BridgePush || plan.goal == StrategicGoal::CoreAssault)
        && ctx.memory.routeFailureCooldown > 0.0f
        && ctx.memory.repeatedRouteFailures >= (plan.committed ? 3 : 2)
        && (plan.committed || plan.allowedRisk < 0.75f))
    {
        return StrategicInterruptReason::RouteFailures;
    }
    if (plan.committed)
    {
        return StrategicInterruptReason::None;
    }
    if (plan.goal == StrategicGoal::EconomicPhase && ctx.readyToRush && ctx.strategicPlan.focus == BotStrategicFocus::Pressure)
    {
        return StrategicInterruptReason::EconomyReady;
    }

    return StrategicInterruptReason::None;
}

Vector3 CoreTargetPosition(const EnergyCore& core);

StrategicPlan EvaluateStrategicPlan(const BotDecisionContext& ctx)
{
    StrategicPlan plan {};
    const bool assignedEmergencyDefender = IsAssignedEmergencyDefender(ctx);
    const bool emergencyDefense = ctx.team.coreAlive
        && assignedEmergencyDefender
        && (ctx.enemyAtCore != nullptr || ctx.memory.recentCoreAttackTimer > 0.0f || ctx.coreDefenseCritical);
    if (emergencyDefense)
    {
        plan.goal = StrategicGoal::BaseDefense;
        plan.plannedDuration = ctx.coreDefenseCritical ? 16.0f : 11.0f;
        plan.committed = true;
        plan.reason = ctx.enemyAtCore != nullptr ? "core under attack"
            : (ctx.memory.recentCoreAttackTimer > 0.0f ? "recent Core breach" : "core defense critical");
        plan.stageCount = 3;
        plan.expectedValue = 950.0f;
        plan.allowedRisk = 0.72f + ctx.memory.teamworkTrait * 0.20f;
        plan.minimumCommitSeconds = 4.0f;
        return plan;
    }

    const bool assignedCleanupHunter = ctx.strategicPlan.cleanupHunterId == ctx.bot.GetId()
        && ctx.strategicPlan.cleanupTargetTeamId >= 0
        && ctx.strategicPlan.cleanupTargetAlivePlayers > 0;
    if (assignedCleanupHunter)
    {
        plan.goal = StrategicGoal::HuntPlayers;
        plan.plannedDuration = 45.0f;
        plan.targetTeamId = ctx.strategicPlan.cleanupTargetTeamId;
        plan.committed = true;
        plan.reason = "assigned final-life cleanup";
        plan.stageCount = 3;
        plan.expectedValue = 760.0f;
        plan.allowedRisk = 0.52f + ctx.memory.aggressionTrait * 0.36f;
        plan.minimumCommitSeconds = 4.0f;
        return plan;
    }

    if (ctx.huntEnemyOnFinalLife
        && ctx.huntEnemy != nullptr
        && ctx.matchTime > 135.0f
        && ctx.aliveEnemyCores == 0)
    {
        plan.goal = StrategicGoal::HuntPlayers;
        plan.plannedDuration = 24.0f;
        plan.targetTeamId = ctx.huntEnemy->GetTeamId();
        plan.committed = ctx.matchTime > 155.0f || ctx.finalLifeTargetClose;
        plan.reason = "enemy final life";
        plan.stageCount = 3;
        plan.expectedValue = 620.0f;
        plan.allowedRisk = 0.45f + ctx.memory.aggressionTrait * 0.45f;
        plan.minimumCommitSeconds = 5.0f;
        return plan;
    }

    const bool earlyEconomy = ctx.matchTime < ctx.tuning.earlyEconomySeconds
        && !ctx.carryingLoot
        && !ctx.readyToRush
        && ctx.hasResourceTarget;
    if (earlyEconomy)
    {
        plan.goal = StrategicGoal::EconomicPhase;
        plan.plannedDuration = 12.0f;
        plan.reason = "early economy";
        plan.stageCount = 3;
        plan.expectedValue = 360.0f;
        plan.allowedRisk = 0.12f + ctx.memory.cautionTrait * 0.18f;
        plan.minimumCommitSeconds = 4.5f;
        return plan;
    }

    if (ctx.strategicPlan.focus == BotStrategicFocus::Defense
        || (ctx.coreDefenseCritical && ctx.matchTime > 55.0f))
    {
        plan.goal = StrategicGoal::BaseDefense;
        plan.plannedDuration = 12.0f;
        plan.reason = ctx.coreDefenseCritical ? "repair base" : "hold defense";
        plan.stageCount = 3;
        plan.expectedValue = 700.0f;
        plan.allowedRisk = 0.25f + ctx.memory.teamworkTrait * 0.30f;
        plan.minimumCommitSeconds = 4.0f;
        return plan;
    }

    const bool resumeAuthoredRoute = ctx.memory.role == BotRole::Rusher
        && ctx.memory.hasAuthoredRouteObjective
        && ctx.memory.authoredRouteIndex > 0
        && ctx.memory.authoredRouteIndex < ctx.memory.authoredRouteMarkerCount
        && ctx.strategicPlan.focus == BotStrategicFocus::Pressure;
    if (ctx.enemyCore != nullptr
        && ctx.enemyCore->IsAlive()
        && (ctx.readyToRush || ctx.strategicPlan.allIn || resumeAuthoredRoute))
    {
        const GridPos enemyCoreBlock = ctx.enemyCore->GetBlockPosition();
        const Vector3 enemyCoreTarget {
            static_cast<float>(enemyCoreBlock.x),
            static_cast<float>(enemyCoreBlock.y),
            static_cast<float>(enemyCoreBlock.z)
        };
        const bool assaultNow = ctx.canBreakDefense
            || ctx.strategicPlan.allIn
            || DistanceSquared(ctx.botPos, enemyCoreTarget) < 90.0f;
        plan.goal = assaultNow ? StrategicGoal::CoreAssault : StrategicGoal::BridgePush;
        // Commitment scales with the trip: a fixed 24s covers ~100 blocks of
        // walking — enough for the stock arena, but on big custom maps the plan
        // expired mid-journey and the re-pick turned the bot around forever.
        // Give the push the time the distance actually needs (emergency defense
        // still interrupts a committed plan immediately).
        const float travelSeconds = std::sqrt(DistanceSquared(ctx.botPos, enemyCoreTarget)) / 4.0f;
        plan.plannedDuration = assaultNow
            ? std::clamp(12.0f + travelSeconds * 1.2f, 18.0f, 60.0f)
            : std::clamp(10.0f + travelSeconds * 1.8f, 24.0f, 90.0f);
        plan.targetTeamId = ctx.enemyCore->GetTeamId();
        plan.committed = ctx.strategicPlan.allIn || ctx.matchTime > 150.0f;
        plan.reason = assaultNow ? "assault core" : "bridge and pressure";
        // CoreAssault remains active through approach -> breach -> attack and
        // completes only when the selected Core is actually destroyed.
        plan.stageCount = 4;
        plan.stage = assaultNow ? 0 : (ctx.inventory.GetBlocks() >= 8 ? 1 : 0);
        plan.expectedValue = assaultNow ? 820.0f : 650.0f;
        plan.allowedRisk = std::clamp(0.28f + ctx.memory.aggressionTrait * 0.55f
            - ctx.memory.cautionTrait * 0.16f, 0.12f, 0.88f);
        plan.minimumCommitSeconds = 6.0f;
        return plan;
    }

    if (ctx.hasResourceTarget && ctx.resourceTargetType == ResourceType::Crystal && ctx.matchTime > 32.0f)
    {
        plan.goal = StrategicGoal::MidControl;
        plan.plannedDuration = 14.0f;
        plan.reason = "mid control";
        plan.stageCount = 2;
        plan.expectedValue = 440.0f;
        plan.allowedRisk = 0.30f + ctx.memory.aggressionTrait * 0.24f;
        plan.minimumCommitSeconds = 5.0f;
        return plan;
    }

    plan.goal = StrategicGoal::EconomicPhase;
    plan.plannedDuration = ctx.carryingLoot ? 6.0f : 10.0f;
    plan.reason = ctx.carryingLoot ? "bank resources" : "default economy";
    plan.stageCount = 3;
    plan.expectedValue = ctx.carryingLoot ? 520.0f : 300.0f;
    plan.allowedRisk = 0.10f + (1.0f - ctx.memory.cautionTrait) * 0.18f;
    plan.minimumCommitSeconds = 3.5f;
    return plan;
}

BotMacroDirective EvaluateAutonomousMacroDirective(const BotDecisionContext& ctx)
{
    BotMacroDirective directive {};
    const Vector3 homeTarget = ctx.coreHome;
    const Vector3 shopTarget = ctx.team.shopPosition;
    const Vector3 enemyCoreTarget = ctx.enemyCore != nullptr
        ? CoreTargetPosition(*ctx.enemyCore)
        : Vector3 { 0.0f, ctx.botPos.y, 0.0f };
    const bool isAssaultRole = ctx.memory.role == BotRole::Rusher || ctx.memory.role == BotRole::Fighter;
    const bool isDefender = ctx.memory.role == BotRole::Defender;
    const bool hasBuyMoney = ctx.inventory.GetResource(ResourceType::Iron) >= 8
        || ctx.inventory.GetResource(ResourceType::Gold) >= 4
        || ctx.inventory.GetResource(ResourceType::Crystal) >= 2;
    const bool idleNearBase = ctx.distanceFromHome < 90.0f
        && ctx.memory.intentTimer > 8.5f
        && ctx.enemyAtCore == nullptr
        && ctx.nearbyEnemy == nullptr;
    const bool teamWantsPressure = ctx.strategicPlan.focus == BotStrategicFocus::Pressure;
    const bool attackSlotOpen = ctx.coordinatedAttackersOnTarget < std::max(1, ctx.strategicPlan.desiredAttackers);

    if (ctx.enemyAtCore != nullptr)
    {
        directive.active = true;
        directive.intent = BotIntent::DefendCore;
        directive.target = ctx.enemyAtCore->GetPosition();
        directive.fightTarget = ctx.enemyAtCore;
        directive.reason = "macro emergency defend";
        return directive;
    }

    const bool assignedCleanupHunter = ctx.strategicPlan.cleanupHunterId == ctx.bot.GetId()
        && ctx.strategicPlan.cleanupTargetTeamId >= 0
        && ctx.strategicPlan.cleanupTargetAlivePlayers > 0;
    if (assignedCleanupHunter)
    {
        const bool hasAssignedTarget = ctx.huntEnemy != nullptr
            && ctx.huntEnemy->GetTeamId() == ctx.strategicPlan.cleanupTargetTeamId
            && !ChaseBlockedByFutility(ctx.memory, ctx.huntEnemy, ctx.botPos);
        directive.active = true;
        if (!hasAssignedTarget && ctx.aliveEnemyCores == 0
            && !ctx.memory.cleanupBridgeKitReady)
        {
            const bool canBuyBlocks = ctx.inventory.GetResource(ResourceType::Iron) >= 8
                || ctx.inventory.GetResource(ResourceType::Gold) >= 4
                || ctx.inventory.GetResource(ResourceType::Crystal) >= 2;
            directive.intent = canBuyBlocks ? BotIntent::GearUp : BotIntent::SecureResources;
            directive.target = canBuyBlocks ? shopTarget
                : (ctx.hasResourceTarget ? ctx.resourceTarget : homeTarget);
            directive.fightTarget = nullptr;
            directive.reason = canBuyBlocks
                ? "rearm final-life hunter" : "fund final-life hunter";
        }
        else
        {
            directive.intent = hasAssignedTarget ? BotIntent::ChaseWeakEnemy : BotIntent::SecureResources;
            directive.target = hasAssignedTarget
                ? ctx.huntEnemy->GetPosition() : ctx.strategicPlan.cleanupTargetPosition;
            directive.fightTarget = hasAssignedTarget ? ctx.huntEnemy : nullptr;
            directive.reason = hasAssignedTarget ? "assigned final-life chase" : "assigned final-life search";
        }
        return directive;
    }

    if (ctx.huntEnemyOnFinalLife
        && ctx.huntEnemy != nullptr
        && ctx.matchTime > 145.0f
        && ctx.finalLifeTargetClose
        && ctx.aliveEnemyCores == 0
        && !ChaseBlockedByFutility(ctx.memory, ctx.huntEnemy, ctx.botPos)
        && (ctx.bot.GetHealth() > ctx.fightHealth || ctx.matchTime > 150.0f)
        && (ctx.memory.role != BotRole::Defender || ctx.matchTime > 150.0f || !ctx.team.coreAlive))
    {
        directive.active = true;
        directive.intent = BotIntent::ChaseWeakEnemy;
        directive.target = ctx.huntEnemy->GetPosition();
        directive.fightTarget = ctx.huntEnemy;
        directive.reason = "macro final-life cleanup";
        return directive;
    }

    if (ctx.nearbyEnemy != nullptr && !isDefender && ctx.memory.chaseBanTimer <= 0.0f)
    {
        const bool canWinFight = ctx.nearbyFight.canWin || ctx.nearbyFight.powerMargin >= -4.0f;
        if (canWinFight && (isAssaultRole || ctx.matchTime > 55.0f))
        {
            directive.active = true;
            directive.intent = BotIntent::FightEnemy;
            directive.target = ctx.nearbyEnemy->GetPosition();
            directive.fightTarget = ctx.nearbyEnemy;
            directive.reason = "macro favorable duel";
            return directive;
        }
    }

    const bool routineRepairWindow = (isDefender && ctx.inventory.GetBlocks() > 0)
        || (ctx.coreDefenseCritical && ctx.matchTime > 55.0f)
        || (ctx.missingDefenseBlocks >= 7 && ctx.matchTime > 80.0f);

    if (isDefender && ctx.team.coreAlive && ctx.coreNeedsRepair && routineRepairWindow)
    {
        directive.active = true;
        if (ctx.inventory.GetBlocks() > 0)
        {
            directive.intent = BotIntent::RepairCoreDefense;
            directive.target = homeTarget;
            directive.reason = "macro repair now";
        }
        else if (ctx.hasResourceTarget)
        {
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "macro gather for repair";
        }
        else
        {
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "macro buy blocks";
        }
        return directive;
    }

    if (ctx.memory.currentPlan.goal == StrategicGoal::BaseDefense
        && ctx.team.coreAlive
        && ctx.coreNeedsRepair
        && routineRepairWindow
        && (isDefender || ctx.coordinatedDefenders < std::max(1, ctx.strategicPlan.desiredDefenders)))
    {
        directive.active = true;
        if (ctx.inventory.GetBlocks() > 0)
        {
            directive.intent = BotIntent::RepairCoreDefense;
            directive.target = homeTarget;
            directive.reason = "plan repair base";
        }
        else if (ctx.hasResourceTarget)
        {
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "plan gather blocks";
        }
        else
        {
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "plan buy blocks";
        }
        return directive;
    }

    if (ctx.memory.currentPlan.goal == StrategicGoal::BridgePush
        && ctx.memory.currentPlan.stage <= 1
        && !ctx.readyToRush)
    {
        directive.active = true;
        if (hasBuyMoney && ctx.wantsShop)
        {
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "plan stage buy bridge kit";
        }
        else if (ctx.hasResourceTarget)
        {
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "plan stage fund bridge kit";
        }
        else
        {
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "plan stage prepare bridge kit";
        }
        return directive;
    }

    const bool rearmingAuthoredPush = ctx.memory.currentPlan.goal == StrategicGoal::BridgePush
        && ctx.memory.role == BotRole::Rusher
        && ctx.memory.hasAuthoredRouteObjective
        && ctx.memory.authoredRouteIndex > 0
        && ctx.memory.authoredRouteIndex < ctx.memory.authoredRouteMarkerCount
        && ctx.inventory.GetBlocks() < 4;
    if (rearmingAuthoredPush)
    {
        directive.active = true;
        if (hasBuyMoney && ctx.wantsShop)
        {
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "plan rearm route";
        }
        else if (ctx.hasResourceTarget)
        {
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "plan fund route";
        }
        else
        {
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "plan seek route blocks";
        }
        return directive;
    }

    if ((ctx.memory.currentPlan.goal == StrategicGoal::BridgePush || ctx.memory.currentPlan.goal == StrategicGoal::CoreAssault)
        && ctx.enemyCore != nullptr
        && ctx.enemyCore->IsAlive()
        && ctx.memory.currentPlan.targetTeamId == ctx.enemyCore->GetTeamId()
        && (attackSlotOpen || ctx.strategicPlan.allIn)
        && (ctx.readyToRush || ctx.memory.currentPlan.stage >= 2 || ctx.matchTime > 90.0f))
    {
        directive.active = true;
        directive.intent = ctx.canBreakDefense ? BotIntent::BreakCoreDefense : BotIntent::PressureCore;
        directive.target = enemyCoreTarget;
        directive.reason = ctx.memory.currentPlan.goal == StrategicGoal::CoreAssault ? "plan core assault" : "plan bridge push";
        return directive;
    }

    if ((ctx.memory.currentPlan.goal == StrategicGoal::EconomicPhase || ctx.memory.currentPlan.goal == StrategicGoal::MidControl)
        && ctx.hasResourceTarget
        && !ctx.carryingLoot
        && (!ctx.wantsShop || !hasBuyMoney)
        && ctx.enemyAtCore == nullptr)
    {
        directive.active = true;
        directive.intent = BotIntent::SecureResources;
        directive.target = ctx.resourceTarget;
        directive.reason = ctx.memory.currentPlan.goal == StrategicGoal::MidControl ? "plan mid control" : "plan economy";
        return directive;
    }

    const bool openingPhase = ctx.matchTime < ctx.tuning.pressurePhaseSeconds;
    const bool midPhase = ctx.matchTime >= ctx.tuning.pressurePhaseSeconds && ctx.matchTime < ctx.tuning.latePressureSeconds;

    if (openingPhase)
    {
        if (ctx.hasResourceTarget && (!ctx.carryingLoot || ctx.inventory.GetBlocks() < 12))
        {
            directive.active = true;
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "macro opening gather";
            return directive;
        }
        if (hasBuyMoney && ctx.wantsShop)
        {
            directive.active = true;
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "macro opening shop";
            return directive;
        }
    }

    if (midPhase)
    {
        if (isAssaultRole
            && ctx.enemyCore != nullptr
            && (attackSlotOpen || ctx.strategicPlan.allIn)
            && (ctx.readyToRush || teamWantsPressure || ctx.teamPlan.activePressure <= 1 || idleNearBase))
        {
            directive.active = true;
            directive.intent = ctx.canBreakDefense ? BotIntent::BreakCoreDefense : BotIntent::PressureCore;
            directive.target = enemyCoreTarget;
            directive.reason = ctx.canBreakDefense ? "macro mid breach" : "macro mid pressure";
            return directive;
        }
        if (hasBuyMoney && ctx.wantsShop && !ctx.readyToRush)
        {
            directive.active = true;
            directive.intent = BotIntent::GearUp;
            directive.target = shopTarget;
            directive.reason = "macro mid gear";
            return directive;
        }
        if (ctx.hasResourceTarget && (idleNearBase || !ctx.carryingLoot))
        {
            directive.active = true;
            directive.intent = BotIntent::SecureResources;
            directive.target = ctx.resourceTarget;
            directive.reason = "macro mid rotate";
            return directive;
        }
    }

    if (ctx.matchTime >= ctx.tuning.latePressureSeconds)
    {
        if (ctx.huntEnemyOnFinalLife
            && ctx.huntEnemy != nullptr
            && ctx.aliveEnemyCores == 0
            && (ctx.finalLifeTargetClose || ctx.matchTime > 210.0f)
            && !ChaseBlockedByFutility(ctx.memory, ctx.huntEnemy, ctx.botPos))
        {
            directive.active = true;
            directive.intent = BotIntent::ChaseWeakEnemy;
            directive.target = ctx.huntEnemy->GetPosition();
            directive.fightTarget = ctx.huntEnemy;
            directive.reason = "macro late cleanup";
            return directive;
        }
        if (ctx.enemyCore != nullptr && ctx.enemyCore->IsAlive())
        {
            if ((ctx.canBreakDefense || isAssaultRole || ctx.readyToRush || teamWantsPressure || idleNearBase)
                && (attackSlotOpen || ctx.strategicPlan.allIn))
            {
                directive.active = true;
                directive.intent = ctx.canBreakDefense ? BotIntent::BreakCoreDefense : BotIntent::PressureCore;
                directive.target = enemyCoreTarget;
                directive.reason = ctx.canBreakDefense ? "macro late breach" : "macro late all-in";
                return directive;
            }
        }
        else if (ctx.huntEnemy != nullptr && !ChaseBlockedByFutility(ctx.memory, ctx.huntEnemy, ctx.botPos))
        {
            directive.active = true;
            directive.intent = BotIntent::ChaseWeakEnemy;
            directive.target = ctx.huntEnemy->GetPosition();
            directive.fightTarget = ctx.huntEnemy;
            directive.reason = "macro cleanup";
            return directive;
        }
    }

    if (idleNearBase && ctx.hasResourceTarget)
    {
        directive.active = true;
        directive.intent = BotIntent::SecureResources;
        directive.target = ctx.resourceTarget;
        directive.reason = "macro anti-camp";
        return directive;
    }

    return directive;
}

struct BotDecisionDerivedContext
{
    Vector3 homeTarget {};
    Vector3 shopTarget {};
    bool canAffordAnyBuy = false;
    bool canRepairNow = false;
    bool canRepairSoon = false;
    bool idleNearBase = false;
};

BotDecisionDerivedContext BuildBotDecisionDerivedContext(const BotDecisionContext& ctx)
{
    const int iron = ctx.inventory.GetResource(ResourceType::Iron);
    const int gold = ctx.inventory.GetResource(ResourceType::Gold);
    const int crystal = ctx.inventory.GetResource(ResourceType::Crystal);
    return BotDecisionDerivedContext {
        ctx.coreHome,
        ctx.team.shopPosition,
        iron >= 8 || gold >= 4 || crystal >= 2,
        ctx.inventory.GetBlocks() > 0,
        ctx.inventory.GetBlocks() > 0 || iron >= 8,
        ctx.distanceFromHome < 84.0f
            && ctx.memory.intentTimer > 9.0f
            && ctx.enemyAtCore == nullptr
            && ctx.nearbyEnemy == nullptr
    };
}

void ConsiderBotDecision(
    BotDecision& decision,
    const BotDecisionContext& ctx,
    BotIntent intent,
    float score,
    Vector3 target,
    Player* fightTarget,
    const char* reason)
{
    // Personality tie-break: teammates evaluating the same situation get
    // slightly different appetites, so two bots at the same spot pick
    // different intents instead of mirroring each other.
    if (score > 0.0f)
    {
        const BotPersonality personality = PersonalityForBot(ctx.bot.GetId(), ctx.memory.personalitySeed);
        const bool combatIntent = intent == BotIntent::FightEnemy
            || intent == BotIntent::ChaseWeakEnemy
            || intent == BotIntent::PressureCore
            || intent == BotIntent::BreakCoreDefense;
        const bool economyIntent = intent == BotIntent::SecureResources
            || intent == BotIntent::GearUp;
        if (combatIntent)
        {
            score *= personality.aggression;
        }
        else if (economyIntent)
        {
            score *= personality.economy;
        }
    }
    if ((intent == BotIntent::PressureCore || intent == BotIntent::BreakCoreDefense)
        && ctx.memory.abandonedPlanCooldown > 0.0f
        && ctx.enemyCore != nullptr
        && ctx.enemyCore->GetTeamId() == ctx.memory.abandonedPlanTargetTeamId)
    {
        score -= 260.0f + ctx.memory.cautionTrait * 180.0f;
    }
    score += RoleIntentBias(ctx.memory.role, intent, ctx.tuning);
    if (ctx.memory.intent == intent)
    {
        score += 105.0f + std::min(45.0f, ctx.memory.intentTimer * 12.0f);
    }
    else if (ctx.memory.intentLockTimer > 0.0f
        && intent != BotIntent::DefendCore
        && intent != BotIntent::Recover
        && intent != BotIntent::RetreatHome)
    {
        score -= 140.0f;
    }

    if (score > decision.score)
    {
        decision.intent = intent;
        decision.state = StateForIntent(intent);
        decision.target = target;
        decision.fightTarget = fightTarget;
        decision.score = score;
        decision.reason = reason;
    }
}

void ConsiderRecoverIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if (ctx.memory.stuckTimer <= 1.05f)
    {
        return;
    }

    Vector3 recoverDirection = Normalize2D(Vector3 { ctx.botPos.x, 0.0f, ctx.botPos.z });
    if (Length2D(recoverDirection) <= 0.0001f)
    {
        recoverDirection = Normalize2D(Vector3 {
            derived.homeTarget.x - ctx.botPos.x,
            0.0f,
            derived.homeTarget.z - ctx.botPos.z
        });
    }
    if (Length2D(recoverDirection) <= 0.0001f)
    {
        recoverDirection = Vector3 { 1.0f, 0.0f, 0.0f };
    }

    const float rotateStep = std::floor(std::max(0.0f, ctx.memory.stuckTimer - 1.05f) / 0.75f);
    if (rotateStep > 0.0f)
    {
        const float directionSign = (static_cast<int>(rotateStep) % 2 == 0) ? -1.0f : 1.0f;
        const float angle = directionSign * rotateStep * 0.25f * kBotPi;
        const float cosine = std::cos(angle);
        const float sine = std::sin(angle);
        recoverDirection = Normalize2D(Vector3 {
            recoverDirection.x * cosine - recoverDirection.z * sine,
            0.0f,
            recoverDirection.x * sine + recoverDirection.z * cosine
        });
    }

    Vector3 recoverTarget {
        ctx.botPos.x + recoverDirection.x * 3.0f,
        ctx.botPos.y,
        ctx.botPos.z + recoverDirection.z * 3.0f
    };
    if (ctx.memory.stuckTimer < 2.25f
        && DistanceSquared(recoverTarget, derived.homeTarget) > DistanceSquared(ctx.botPos, derived.homeTarget) + 36.0f)
    {
        recoverTarget = derived.homeTarget;
    }

    ConsiderBotDecision(decision, ctx, BotIntent::Recover, 980.0f + ctx.memory.stuckTimer * 120.0f, recoverTarget, nullptr, "unstick");
}

void ConsiderBaseDefenseIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    if (ctx.enemyAtCore == nullptr)
    {
        return;
    }

    const float defenderBonus = ctx.memory.role == BotRole::Defender ? 220.0f : 80.0f;
    const float lonelyBaseBonus = ctx.teamPlan.alliesNearCore == 0 ? 130.0f : 0.0f;
    const float coordinationBonus = ctx.coordinatedHelpCalls > 0 ? 110.0f : (ctx.coordinatedDefenders == 0 ? 70.0f : 0.0f);
    const float defenseCriticalBonus = ctx.coreDefenseCritical ? 120.0f : 0.0f;
    const float personalPlanBonus = ctx.memory.currentPlan.goal == StrategicGoal::BaseDefense ? 115.0f : 0.0f;
    const float strategicDefenseBonus = ctx.strategicPlan.defenseUrgency * ctx.tuning.strategicDefenseUrgencyScale;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::DefendCore,
        1180.0f + defenderBonus + lonelyBaseBonus + coordinationBonus + defenseCriticalBonus + personalPlanBonus + strategicDefenseBonus - std::sqrt(ctx.enemyAtCoreDistance) * 10.0f,
        ctx.enemyAtCore->GetPosition(),
        ctx.enemyAtCore,
        "enemy at core");
}

void ConsiderRetreatIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if ((!ctx.retreating && !ctx.defenderAwayFromBase)
        || (ctx.memory.role == BotRole::Defender && ctx.enemyAtCore != nullptr))
    {
        return;
    }

    const float lootBonus = ctx.carryingLoot ? static_cast<float>(ctx.memory.carriedResourceValue) * 4.0f : 0.0f;
    const float healthBonus = static_cast<float>(std::max(0, ctx.retreatHealth + 18 - ctx.bot.GetHealth())) * 9.0f;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::RetreatHome,
        760.0f + lootBonus + healthBonus + (ctx.defenderAwayFromBase ? 180.0f : 0.0f),
        ctx.memory.role == BotRole::Defender ? derived.homeTarget : derived.shopTarget,
        nullptr,
        ctx.defenderAwayFromBase ? "return to base" : "heal and bank");
}

void ConsiderRepairIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if ((!ctx.coreNeedsRepair && !ctx.coreCanUpgrade) || !ctx.team.coreAlive)
    {
        return;
    }

    const bool routineRepair = ctx.coreNeedsRepair && !ctx.coreDefenseCritical;
    if (routineRepair && ctx.memory.role != BotRole::Defender)
    {
        return;
    }
    if (!derived.canRepairNow && !ctx.coreCanUpgrade)
    {
        return;
    }

    const bool assignedBuilder = ctx.memory.role == BotRole::Defender
        || ctx.coreDefenseCritical
        || ((ctx.teamPlan.defendersNearCore == 0 || ctx.coordinatedDefenders == 0)
            && ctx.teamPlan.activeRepair == 0
            && ctx.distanceFromHome < 120.0f);
    if (!assignedBuilder || !derived.canRepairSoon)
    {
        return;
    }

    const float defenderBonus = ctx.memory.role == BotRole::Defender ? 280.0f : 0.0f;
    const float repairCrowdPenalty = static_cast<float>(ctx.teamPlan.activeRepair) * 115.0f;
    const float defenseUrgency = (ctx.coreDefenseCritical ? 210.0f : 0.0f)
        + static_cast<float>(ctx.missingDefenseBlocks) * 34.0f
        + ctx.strategicPlan.defenseUrgency * ctx.tuning.repairUrgencyScale
        + (ctx.memory.currentPlan.goal == StrategicGoal::BaseDefense ? 125.0f : 0.0f);
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::RepairCoreDefense,
        500.0f
            + defenderBonus
            + defenseUrgency
            + (ctx.coreCanUpgrade ? 90.0f : 0.0f)
            + (derived.canRepairNow ? 70.0f : -90.0f)
            - repairCrowdPenalty
            - std::sqrt(ctx.distanceFromHome) * 5.0f,
        derived.homeTarget,
        nullptr,
        ctx.coreCanUpgrade ? "upgrade defense" : "repair defense");
}

void ConsiderGearIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if ((!ctx.wantsShop && ctx.inventory.GetBlocks() >= 3)
        || (ctx.huntEnemyOnFinalLife && ctx.finalLifeTargetClose && ctx.matchTime > 155.0f))
    {
        return;
    }

    const float blockPressure = static_cast<float>(std::max(0, ctx.desiredBlocks - ctx.inventory.GetBlocks())) * 7.0f;
    const float lootPressure = ctx.carryingLoot ? static_cast<float>(ctx.memory.carriedResourceValue) * 3.4f : 0.0f;
    const float shopCrowdPenalty = static_cast<float>(ctx.teamPlan.activeShop) * 72.0f;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::GearUp,
        470.0f
            + blockPressure
            + lootPressure
            + (ctx.bot.GetHealth() < 70 ? 120.0f : 0.0f)
            + (derived.canAffordAnyBuy ? 80.0f : -190.0f)
            - shopCrowdPenalty,
        derived.shopTarget,
        nullptr,
        "buy gear");
}

void ConsiderCombatIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    if (ctx.shouldFightNearby && ctx.nearbyEnemy != nullptr)
    {
        const float roleBonus = ctx.memory.role == BotRole::Fighter ? 150.0f : (ctx.memory.role == BotRole::Defender ? 85.0f : 0.0f);
        const float healthEdge = static_cast<float>(ctx.bot.GetHealth() - ctx.nearbyEnemy->GetHealth()) * 2.8f;
        const float powerEdge = std::clamp(ctx.nearbyFight.powerMargin, -70.0f, 70.0f) * 1.15f;
        ConsiderBotDecision(
            decision,
            ctx,
            BotIntent::FightEnemy,
            520.0f + roleBonus + healthEdge + powerEdge - ctx.nearbyEnemyDistance * 18.0f,
            ctx.nearbyEnemy->GetPosition(),
            ctx.nearbyEnemy,
            "take fight");
    }

    if (ctx.weakEnemy != nullptr
        && ctx.bot.GetHealth() > ctx.fightHealth
        && !ChaseBlockedByFutility(ctx.memory, ctx.weakEnemy, ctx.botPos))
    {
        const float weakDistance = std::sqrt(DistanceSquared(ctx.bot.GetPosition(), ctx.weakEnemy->GetPosition()));
        ConsiderBotDecision(
            decision,
            ctx,
            BotIntent::ChaseWeakEnemy,
            500.0f + static_cast<float>(ctx.bot.GetHealth() - ctx.weakEnemy->GetHealth()) * 3.8f - weakDistance * 8.0f,
            ctx.weakEnemy->GetPosition(),
            ctx.weakEnemy,
            "finish weak enemy");
    }
}

Vector3 CoreTargetPosition(const EnergyCore& core)
{
    return Vector3 {
        static_cast<float>(core.GetBlockPosition().x),
        static_cast<float>(core.GetBlockPosition().y),
        static_cast<float>(core.GetBlockPosition().z)
    };
}

void ConsiderCorePressureIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    const bool strategicTarget = ctx.enemyCore != nullptr
        && ctx.strategicPlan.attackCoreTeamId == ctx.enemyCore->GetTeamId();
    const bool personalPlanTarget = ctx.enemyCore != nullptr
        && (ctx.memory.currentPlan.goal == StrategicGoal::BridgePush
            || ctx.memory.currentPlan.goal == StrategicGoal::CoreAssault)
        && ctx.memory.currentPlan.targetTeamId == ctx.enemyCore->GetTeamId();
    const bool attackSlotOpen = ctx.coordinatedAttackersOnTarget < std::max(1, ctx.strategicPlan.desiredAttackers);
    const float strategicAttackBonus = strategicTarget ? ctx.strategicPlan.attackUrgency * ctx.tuning.strategicAttackUrgencyScale : 0.0f;
    const float personalPlanAttackBonus = personalPlanTarget
        ? (ctx.memory.currentPlan.goal == StrategicGoal::CoreAssault ? 155.0f : 105.0f)
        : 0.0f;
    const float attackSlotBonus = attackSlotOpen || ctx.strategicPlan.allIn ? ctx.tuning.attackSlotBonus : ctx.tuning.attackSlotPenalty;
    // Destroying a surviving core ends the match much faster than hunting
    // runners, so the assault options keep gaining weight in the late game.
    const float lateAssaultBonus = std::min(300.0f, std::max(0.0f, ctx.matchTime - ctx.tuning.latePressureSeconds) * 2.4f);

    if (ctx.canBreakDefense && ctx.enemyCore != nullptr)
    {
        const float coordinationPenalty = static_cast<float>(ctx.coordinatedAttackersOnTarget) * ctx.tuning.breakCoordinationPenalty;
        ConsiderBotDecision(
            decision,
            ctx,
            BotIntent::BreakCoreDefense,
            650.0f + (ctx.memory.role == BotRole::Rusher ? 120.0f : 0.0f) + strategicAttackBonus + personalPlanAttackBonus + attackSlotBonus + lateAssaultBonus - coordinationPenalty,
            CoreTargetPosition(*ctx.enemyCore),
            nullptr,
            "crack defense");
    }

    if (ctx.shouldPressureCore && ctx.enemyCore != nullptr)
    {
        const float teamPushBonus = ctx.teamPlan.activePressure > 0 ? 70.0f : 0.0f;
        const float baseCoveredBonus = ctx.teamPlan.alliesNearCore > 0 || !ctx.team.coreAlive ? 60.0f : -85.0f;
        const float timePressureBonus = std::min(280.0f, std::max(0.0f, ctx.matchTime - 80.0f) * 1.6f);
        const float coordinationPenalty = static_cast<float>(ctx.coordinatedAttackersOnTarget) * ctx.tuning.pressureCoordinationPenalty;
        ConsiderBotDecision(
            decision,
            ctx,
            BotIntent::PressureCore,
            570.0f + teamPushBonus + baseCoveredBonus + timePressureBonus + strategicAttackBonus + personalPlanAttackBonus + attackSlotBonus + lateAssaultBonus + (ctx.memory.role == BotRole::Rusher ? 150.0f : 60.0f) - coordinationPenalty,
            CoreTargetPosition(*ctx.enemyCore),
            nullptr,
            "rush core");
    }
}

void ConsiderResourceIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if (!ctx.hasResourceTarget
        || ctx.carryingLoot
        || (ctx.huntEnemyOnFinalLife && ctx.finalLifeTargetClose && ctx.matchTime > 155.0f))
    {
        return;
    }

    const float resourceBonus = ctx.resourceTargetType == ResourceType::Crystal
        ? (ctx.memory.role == BotRole::Collector ? 190.0f : 120.0f)
        : (ctx.resourceTargetType == ResourceType::Gold ? 105.0f : 45.0f);
    const float collectorBonus = ctx.memory.role == BotRole::Collector ? 260.0f : 0.0f;
    const float resourceCrowdPenalty = static_cast<float>(ctx.teamPlan.activeResource) * (ctx.memory.role == BotRole::Collector ? 12.0f : 58.0f);
    const float blockShortageBoost = ctx.inventory.GetBlocks() < 6 ? 220.0f : 0.0f;
    const float idleExpeditionBoost = derived.idleNearBase ? 240.0f : 0.0f;
    const float strategicEconomyBonus = ctx.strategicPlan.focus == BotStrategicFocus::Economy ? ctx.tuning.strategicEconomyBonus : 0.0f;
    const float personalPlanEconomyBonus = ctx.memory.currentPlan.goal == StrategicGoal::EconomicPhase
        || ctx.memory.currentPlan.goal == StrategicGoal::MidControl
        ? ctx.tuning.personalEconomyBonus
        : 0.0f;
    const float strategicPressurePenalty = ctx.strategicPlan.focus == BotStrategicFocus::Pressure
        && ctx.memory.role != BotRole::Collector
        ? ctx.tuning.strategicPressureEconomyPenalty
        : 0.0f;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::SecureResources,
        390.0f + resourceBonus + collectorBonus + blockShortageBoost + idleExpeditionBoost + strategicEconomyBonus + personalPlanEconomyBonus - strategicPressurePenalty - resourceCrowdPenalty,
        ctx.resourceTarget,
        nullptr,
        ctx.resourceTargetType == ResourceType::Crystal ? "center crystals" : "secure resources");
}

void ConsiderFinalDuelIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    if (!ctx.finalDuelPhase
        || ctx.huntEnemy == nullptr
        || ChaseBlockedByFutility(ctx.memory, ctx.huntEnemy, ctx.botPos)
        || (ctx.bot.GetHealth() <= ctx.fightHealth && !(ctx.huntEnemyOnFinalLife && ctx.matchTime > 145.0f)))
    {
        return;
    }

    const float finalLifeBonus = ctx.huntEnemyOnFinalLife ? (ctx.finalLifeTargetClose && ctx.matchTime > 145.0f ? 420.0f : 120.0f) : 0.0f;
    // Capped: an unbounded urgency made every bot chase runners forever
    // instead of finishing the remaining cores.
    const float cleanupUrgency = ctx.huntEnemyOnFinalLife
        ? std::min(140.0f, std::max(0.0f, ctx.matchTime - 145.0f) * 5.0f)
        : 0.0f;
    const float personalHuntBonus = ctx.memory.currentPlan.goal == StrategicGoal::HuntPlayers ? 170.0f : 0.0f;
    const float distance = std::sqrt(DistanceSquared(ctx.bot.GetPosition(), ctx.huntEnemy->GetPosition()));
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::ChaseWeakEnemy,
        520.0f
            + finalLifeBonus
            + cleanupUrgency
            + personalHuntBonus
            + (ctx.team.coreAlive ? 0.0f : 120.0f)
            + static_cast<float>(ctx.bot.GetHealth() - ctx.huntEnemy->GetHealth()) * 1.8f
            - distance * 4.5f,
        ctx.huntEnemy->GetPosition(),
        ctx.huntEnemy,
        ctx.huntEnemyOnFinalLife ? "final-life cleanup" : "final duel");
}

void ConsiderLateMapPressureIntent(BotDecision& decision, const BotDecisionContext& ctx)
{
    if (ctx.enemyCore == nullptr
        || !ctx.enemyCore->IsAlive()
        || !ctx.readyToRush
        || (ctx.huntEnemyOnFinalLife && ctx.finalLifeTargetClose && ctx.matchTime > 155.0f))
    {
        return;
    }

    const float pressureCrowdPenalty = static_cast<float>(std::max(0, ctx.teamPlan.activePressure - 2)) * 46.0f;
    const float coordinationCrowdPenalty = static_cast<float>(ctx.coordinatedAttackersOnTarget) * ctx.tuning.lateCoordinationPenalty;
    const float lateMatchBoost = std::max(0.0f, ctx.matchTime - 120.0f) * 2.2f;
    const float strategicAttackBonus = ctx.strategicPlan.attackCoreTeamId == ctx.enemyCore->GetTeamId()
        ? ctx.strategicPlan.attackUrgency * ctx.tuning.lateAttackUrgencyScale
        : 0.0f;
    ConsiderBotDecision(
        decision,
        ctx,
        BotIntent::PressureCore,
        360.0f + lateMatchBoost + strategicAttackBonus + (ctx.memory.role == BotRole::Rusher ? 170.0f : 0.0f) - pressureCrowdPenalty - coordinationCrowdPenalty,
        CoreTargetPosition(*ctx.enemyCore),
        nullptr,
        "map pressure");
}

void ConsiderIdleResourceIntent(BotDecision& decision, const BotDecisionContext& ctx, const BotDecisionDerivedContext& derived)
{
    if (derived.idleNearBase
        && ctx.hasResourceTarget
        && !ctx.carryingLoot
        && !ctx.coreNeedsRepair
        && ctx.enemyAtCore == nullptr)
    {
        ConsiderBotDecision(decision, ctx, BotIntent::SecureResources, 860.0f, ctx.resourceTarget, nullptr, "leave base");
    }
}

BotDecision EvaluateBotDecision(const BotDecisionContext& ctx)
{
    BotDecision decision {};
    decision.target = Vector3 { 0.0f, ctx.botPos.y, 0.0f };
    const BotDecisionDerivedContext derived = BuildBotDecisionDerivedContext(ctx);

    ConsiderRecoverIntent(decision, ctx, derived);
    ConsiderBaseDefenseIntent(decision, ctx);
    ConsiderRetreatIntent(decision, ctx, derived);
    ConsiderRepairIntent(decision, ctx, derived);
    ConsiderGearIntent(decision, ctx, derived);
    ConsiderCombatIntent(decision, ctx);
    ConsiderCorePressureIntent(decision, ctx);
    ConsiderResourceIntent(decision, ctx, derived);
    ConsiderFinalDuelIntent(decision, ctx);
    ConsiderLateMapPressureIntent(decision, ctx);
    ConsiderIdleResourceIntent(decision, ctx, derived);

    if (decision.score < -9990.0f)
    {
        ConsiderBotDecision(decision, ctx, BotIntent::SecureResources, 0.0f, Vector3 { 0.0f, ctx.botPos.y, 0.0f }, nullptr, "default mid");
    }

    return decision;
}

void ApplyBotDecision(
    BotMemory& memory,
    const BotDecision& decision,
    BotDifficulty difficulty,
    const BotTuningGenome& tuning)
{
    if (decision.intent != memory.intent)
    {
        memory.intent = decision.intent;
        memory.intentTimer = 0.0f;
        memory.intentLockTimer = IntentLockSeconds(decision.intent, difficulty, tuning);
        memory.hasNavWaypoint = false;
        if (decision.intent == BotIntent::Recover)
        {
            memory.hasBreakTarget = false;
            memory.breakProgress = 0.0f;
        }
    }
    memory.intentReason = decision.reason;
    memory.intentScore = decision.score;

    if (decision.state != memory.state)
    {
        memory.state = decision.state;
        memory.stateTimer = 0.0f;
        if (decision.state == BotState::Fight)
        {
            memory.attackTimer = std::max(memory.attackTimer, BotFightReactionDelay(difficulty));
        }
    }
}

void UpdateBotStuckAfterMove(BotMemory& memory, Vector3 wish, float movedDistance, float dt, Vector3 currentPosition)
{
    if (Length2D(wish) > 0.1f && movedDistance < 0.0008f)
    {
        memory.stuckTimer += dt;
    }
    else
    {
        memory.stuckTimer = std::max(0.0f, memory.stuckTimer - dt * 1.8f);
    }
    memory.lastPosition = currentPosition;
}

struct BotMovementPlan
{
    Vector3 wish {};
    Vector3 aimDirection {};
    float fightDistance = 999.0f;
    bool edgePressure = false;
    bool rangedKite = false; // holding range with a ranged weapon (zoner mode)
    Player* fightTarget = nullptr;
};

struct BotTraversalPlan
{
    bool jump = false;
    bool consumedByMining = false;
};

// The hotbar slot the bot should be "holding" this tick: its active ranged
// weapon while zoning/kiting (sniper > blaster > bow), otherwise its best melee
// (sword > axe > spear); slot 0 when not fighting. Cosmetic/believability +
// carries the weapon-switch decision on the command — bot combat/build do NOT
// depend on the slot, so this never changes what actually fires or gets built.
int BotHeldSlotForCombat(const Player& bot, bool preferRanged, bool fighting)
{
    if (!fighting)
    {
        return 0;
    }
    const auto& hotbar = bot.GetInventory().GetHotbarSlots();
    const auto firstSlotOf = [&hotbar](std::initializer_list<ItemType> prefs) -> int
    {
        for (ItemType want : prefs)
        {
            for (int i = 0; i < kHotbarSlotCount; ++i)
            {
                if (!hotbar[i].IsEmpty() && hotbar[i].type == want)
                {
                    return i;
                }
            }
        }
        return -1;
    };
    if (preferRanged)
    {
        const int ranged = firstSlotOf({ ItemType::SniperRifle, ItemType::Blaster, ItemType::Bow });
        if (ranged >= 0)
        {
            return ranged;
        }
    }
    const int melee = firstSlotOf({ ItemType::Sword, ItemType::Axe, ItemType::Spear });
    return melee >= 0 ? melee : 0;
}

PlayerCommand BuildBotMovementCommand(
    const Player& bot,
    std::uint32_t tick,
    Vector3 wish,
    Vector3 aimDirection,
    bool jump,
    bool sprint)
{
    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
    command.tick = tick;
    command.jump = jump;
    command.sprint = sprint;
    command.selectedSlot = 0;

    Vector3 commandAim = Normalize2D(aimDirection);
    if (Length2D(commandAim) <= 0.0001f)
    {
        commandAim = Normalize2D(wish);
    }
    command.aimYaw = Length2D(commandAim) > 0.0001f
        ? YawFromDirection(commandAim)
        : bot.GetYaw();

    const float sinYaw = std::sin(command.aimYaw);
    const float cosYaw = std::cos(command.aimYaw);
    const Vector3 forward { sinYaw, 0.0f, -cosYaw };
    const Vector3 right { cosYaw, 0.0f, sinYaw };
    command.moveForward = wish.x * forward.x + wish.z * forward.z;
    command.moveStrafe = wish.x * right.x + wish.z * right.z;
    return command;
}

template <typename IsVoidThreatFn, typename TryBridgeFn>
BotMovementPlan BuildBotMovementPlan(
    Player& bot,
    BotMemory& memory,
    BotDifficulty difficulty,
    float matchTime,
    Vector3 coreHome,
    Vector3 target,
    BotState state,
    BotIntent intent,
    Player* decisionFightTarget,
    IsVoidThreatFn&& isVoidThreat,
    TryBridgeFn&& tryBridge)
{
    const Vector3 botPos = bot.GetPosition();
    BotMovementPlan plan {};
    plan.fightTarget = state == BotState::Fight ? decisionFightTarget : nullptr;
    plan.wish = Normalize2D(Vector3 { target.x - botPos.x, 0.0f, target.z - botPos.z });
    plan.aimDirection = plan.wish;
    plan.fightDistance = plan.fightTarget != nullptr
        ? std::sqrt(DistanceSquared(botPos, plan.fightTarget->GetPosition()))
        : 999.0f;

    if (plan.fightTarget != nullptr)
    {
        const Vector3 toEnemy = Normalize2D(Vector3 {
            plan.fightTarget->GetPosition().x - botPos.x,
            0.0f,
            plan.fightTarget->GetPosition().z - botPos.z
        });
        const Vector3 side { -toEnemy.z, 0.0f, toEnemy.x };
        if (memory.strafeTimer <= 0.0f)
        {
            memory.strafeSign = -memory.strafeSign;
            memory.strafeTimer = difficulty == BotDifficulty::Hard ? 0.42f : (difficulty == BotDifficulty::Easy ? 0.78f : 0.56f);
        }

        const float desiredRange = difficulty == BotDifficulty::Hard ? 2.22f : (difficulty == BotDifficulty::Easy ? 1.82f : 2.05f);
        const bool shouldKite = bot.GetHealth() < plan.fightTarget->GetHealth() - 18
            && intent != BotIntent::DefendCore
            && intent != BotIntent::ChaseWeakEnemy;

        // --- Ranged-advantage kiting (positional intelligence) --------------
        // A bot with a ready ranged weapon treats RANGE itself as the advantage:
        // it holds a firing standoff and backpedals when the enemy closes,
        // instead of only kiting when low on health. Svidetel is a dedicated
        // zoner (his whole kit is ranged control), so he keeps an even larger
        // standoff and kites in nearly every fight. Chasing a fleeing weak enemy
        // still closes (you finish the kill); defenders hold their shell.
        const Inventory& kiteInventory = bot.GetInventory();
        const bool hasReadyRanged =
            kiteInventory.HasItem(ItemType::Bow)
            || kiteInventory.HasItem(ItemType::Blaster)
            || kiteInventory.HasItem(ItemType::SniperRifle);
        const bool witnessZoner = bot.GetHeroId() == HeroId::Svidetel;
        const bool rangedKite = hasReadyRanged
            && difficulty != BotDifficulty::Easy
            && intent != BotIntent::DefendCore
            && intent != BotIntent::ChaseWeakEnemy
            && (witnessZoner
                || intent == BotIntent::FightEnemy
                || intent == BotIntent::PressureCore
                || intent == BotIntent::BreakCoreDefense);
        plan.rangedKite = rangedKite;
        const float rangedRange = witnessZoner ? 10.5f : 8.0f;

        const float forwardAmount = rangedKite
            ? (plan.fightDistance < rangedRange - 1.5f ? -0.85f
                : (plan.fightDistance > rangedRange + 2.5f ? 0.65f : -0.05f))
            : (shouldKite
                ? (plan.fightDistance < 4.2f ? -0.55f : 0.10f)
                : (plan.fightDistance > desiredRange + 0.35f
                    ? 1.0f
                    : (plan.fightDistance < desiredRange - 0.35f ? -0.55f : 0.16f)));
        const bool highGroundRisk = isVoidThreat(Vector3 { botPos.x + side.x * memory.strafeSign * 0.90f, botPos.y, botPos.z + side.z * memory.strafeSign * 0.90f });
        const float strafeScale = highGroundRisk ? 0.18f : 1.0f;
        const float strafeAmount = static_cast<float>(memory.strafeSign)
            * (difficulty == BotDifficulty::Hard ? 0.84f : (difficulty == BotDifficulty::Easy ? 0.32f : 0.58f))
            * strafeScale;
        plan.wish = Normalize2D(Vector3 {
            toEnemy.x * forwardAmount + side.x * strafeAmount,
            0.0f,
            toEnemy.z * forwardAmount + side.z * strafeAmount
        });

        // --- Void execute (positional intelligence) -------------------------
        // If the enemy stands near a void edge, flank to the side OPPOSITE the
        // gap and close in, so the next melee/blaster knockback (always applied
        // AWAY from the attacker) sends them off the map instead of trading
        // blows. The most dramatic BedWars positioning play, entirely emergent —
        // the bot just lines its knockback vector up with the void; the existing
        // attack code delivers it. Easy bots skip it. The downstream void-safety
        // still guards the bot's OWN footing, so it commits without suiciding.
        // A ranged zoner does NOT dive for this — it holds range and lets its
        // shot's own knockback push an edge-standing enemy off from afar.
        const bool aggressiveIntent = intent == BotIntent::FightEnemy
            || intent == BotIntent::ChaseWeakEnemy
            || intent == BotIntent::PressureCore
            || intent == BotIntent::BreakCoreDefense;
        const bool hasKnockback = kiteInventory.HasItem(ItemType::Sword)
            || kiteInventory.HasItem(ItemType::Axe)
            || kiteInventory.HasItem(ItemType::Spear)
            || kiteInventory.HasItem(ItemType::Blaster)
            || kiteInventory.HasItem(ItemType::SniperRifle);
        if (difficulty != BotDifficulty::Easy && aggressiveIntent && hasKnockback && !rangedKite
            && plan.fightDistance < 5.5f && !isVoidThreat(botPos))
        {
            const Vector3 enemyPos = plan.fightTarget->GetPosition();
            // Fixed-order compass probe → deterministic. First void direction
            // found is the way to push the enemy.
            constexpr float kProbe = 1.7f;
            constexpr float kDiag = 0.7071f;
            const Vector3 probes[] {
                Vector3 { 1.0f, 0.0f, 0.0f }, Vector3 { -1.0f, 0.0f, 0.0f },
                Vector3 { 0.0f, 0.0f, 1.0f }, Vector3 { 0.0f, 0.0f, -1.0f },
                Vector3 { kDiag, 0.0f, kDiag }, Vector3 { -kDiag, 0.0f, kDiag },
                Vector3 { kDiag, 0.0f, -kDiag }, Vector3 { -kDiag, 0.0f, -kDiag }
            };
            Vector3 voidDir {};
            bool enemyNearVoid = false;
            for (const Vector3& dir : probes)
            {
                if (isVoidThreat(Vector3 { enemyPos.x + dir.x * kProbe, enemyPos.y, enemyPos.z + dir.z * kProbe }))
                {
                    voidDir = dir;
                    enemyNearVoid = true;
                    break;
                }
            }
            if (enemyNearVoid)
            {
                // The kill stance is on the enemy's far side from the void, so
                // bot -> enemy aligns with enemy -> void. Move there; once lined
                // up (close), drive straight in to deliver the knockback.
                const Vector3 killSpot {
                    enemyPos.x - voidDir.x * 1.7f, botPos.y, enemyPos.z - voidDir.z * 1.7f };
                const float misalign = std::sqrt(DistanceSquared(botPos, killSpot));
                plan.wish = misalign > 0.9f
                    ? Normalize2D(Vector3 { killSpot.x - botPos.x, 0.0f, killSpot.z - botPos.z })
                    : Normalize2D(Vector3 { enemyPos.x - botPos.x, 0.0f, enemyPos.z - botPos.z });
            }
        }

        float aimError = difficulty == BotDifficulty::Easy ? 0.58f : (difficulty == BotDifficulty::Hard ? 0.24f : 0.38f);
        if (plan.fightDistance < 2.55f || plan.fightTarget->GetHealth() <= bot.GetHealth() - 20)
        {
            aimError *= difficulty == BotDifficulty::Easy ? 0.82f : 0.62f;
        }
        const float aimWave = std::sin(matchTime * (difficulty == BotDifficulty::Hard ? 5.1f : 3.7f) + static_cast<float>(bot.GetId()) * 1.73f);
        plan.aimDirection = Normalize2D(Vector3 {
            toEnemy.x + side.x * aimError * (static_cast<float>(memory.strafeSign) * 0.55f + aimWave * 0.45f),
            0.0f,
            toEnemy.z + side.z * aimError * (static_cast<float>(memory.strafeSign) * 0.55f + aimWave * 0.45f)
        });
    }

    const Vector3 nextStep {
        botPos.x + plan.wish.x * 0.95f,
        botPos.y,
        botPos.z + plan.wish.z * 0.95f
    };
    const Vector3 wishSide { -plan.wish.z, 0.0f, plan.wish.x };
    const bool voidAhead = isVoidThreat(nextStep);
    const bool voidLeft = Length2D(plan.wish) > 0.0001f
        && isVoidThreat(Vector3 { botPos.x + plan.wish.x * 0.35f + wishSide.x * 0.78f, botPos.y, botPos.z + plan.wish.z * 0.35f + wishSide.z * 0.78f });
    const bool voidRight = Length2D(plan.wish) > 0.0001f
        && isVoidThreat(Vector3 { botPos.x + plan.wish.x * 0.35f - wishSide.x * 0.78f, botPos.y, botPos.z + plan.wish.z * 0.35f - wishSide.z * 0.78f });
    const bool currentVoid = isVoidThreat(botPos);
    plan.edgePressure = voidAhead || currentVoid;

    bool placedVoidBlock = false;
    if (voidAhead && bot.GetInventory().GetBlocks() > 0)
    {
        placedVoidBlock = tryBridge(target);
        // Placement and locomotion are separate human-speed actions.  Waiting
        // one tick for authoritative confirmation prevents the classic bot
        // failure where it walks into the cell before the bridge block exists.
        if (placedVoidBlock)
        {
            plan.wish = Vector3 {};
        }
    }
    Vector3 safetyWish = Normalize2D(Vector3 { coreHome.x - botPos.x, 0.0f, coreHome.z - botPos.z });
    if (Length2D(safetyWish) <= 0.0001f)
    {
        safetyWish = Normalize2D(Vector3 { -botPos.x, 0.0f, -botPos.z });
    }
    if (voidAhead && plan.fightTarget != nullptr && !placedVoidBlock)
    {
        if (bot.GetInventory().GetBlocks() > 0)
        {
            plan.wish = Vector3 {};
            tryBridge(Vector3 { botPos.x + safetyWish.x * 2.0f, botPos.y, botPos.z + safetyWish.z * 2.0f });
        }
        else
        {
            plan.wish = safetyWish;
        }
    }
    else if (plan.fightTarget != nullptr && !voidAhead)
    {
        if (voidLeft && !voidRight)
        {
            plan.wish = Normalize2D(Vector3 { plan.wish.x - wishSide.x * 0.45f, 0.0f, plan.wish.z - wishSide.z * 0.45f });
        }
        else if (voidRight && !voidLeft)
        {
            plan.wish = Normalize2D(Vector3 { plan.wish.x + wishSide.x * 0.45f, 0.0f, plan.wish.z + wishSide.z * 0.45f });
        }
    }
    else if (voidAhead && bot.GetInventory().GetBlocks() <= 0)
    {
        plan.wish = safetyWish;
    }
    else if (voidAhead && !placedVoidBlock)
    {
        plan.wish = bot.GetInventory().GetBlocks() > 0
            ? Vector3 {}
            : Vector3 { safetyWish.x * 0.45f, 0.0f, safetyWish.z * 0.45f };
    }

    const bool shouldBridge = state == BotState::Bridge
        || state == BotState::AttackCore
        || state == BotState::BreakDefense
        || state == BotState::Collect
        || state == BotState::Retreat
        || intent == BotIntent::GearUp
        || intent == BotIntent::Recover
        || memory.stuckTimer > 0.35f
        || plan.edgePressure;
    if (shouldBridge)
    {
        tryBridge(target);
    }

    return plan;
}

template <typename TryBreakBlockingFn>
BotTraversalPlan BuildBotTraversalPlan(
    Player& bot,
    BotMemory& memory,
    BotDifficulty difficulty,
    Player* fightTarget,
    float fightDistance,
    Vector3 nextStep,
    Vector3 wish,
    float dt,
    const World& world,
    TryBreakBlockingFn&& tryBreakBlocking)
{
    BotTraversalPlan plan {};
    const GridPos stepBlock = world.WorldToGrid(Vector3 { nextStep.x, bot.GetPosition().y + 0.04f, nextStep.z });
    const GridPos stepHead { stepBlock.x, stepBlock.y + 1, stepBlock.z };
    const GridPos stepSupport = world.WorldToGrid(Vector3 { nextStep.x, bot.GetPosition().y - 1.40f, nextStep.z });
    const bool oneBlockObstacle = world.IsSolid(stepBlock)
        && world.IsAir(stepHead)
        && stepBlock.y > stepSupport.y;
    const bool shouldMineObstacle = memory.state == BotState::AttackCore
        || memory.state == BotState::BreakDefense
        || memory.state == BotState::Bridge
        || memory.intent == BotIntent::FightEnemy
        || memory.intent == BotIntent::ChaseWeakEnemy
        || memory.intent == BotIntent::Recover
        || memory.stuckTimer > 0.20f;
    if (shouldMineObstacle && tryBreakBlocking(wish, dt))
    {
        plan.consumedByMining = true;
        return plan;
    }

    if (memory.jumpTimer <= 0.0f
        && (oneBlockObstacle
            || memory.stuckTimer > 0.28f
            || (fightTarget != nullptr && fightDistance > 1.8f && fightDistance < 2.8f && difficulty == BotDifficulty::Hard && bot.IsOnGround())))
    {
        plan.jump = true;
        memory.jumpTimer = oneBlockObstacle
            ? (difficulty == BotDifficulty::Hard ? 0.38f : 0.52f)
            : (difficulty == BotDifficulty::Hard ? 0.82f : 1.2f);
    }

    return plan;
}

template <typename RegisterCombatEventFn>
void TryPerformBotMeleeAttack(
    Player& bot,
    BotMemory& memory,
    BotDifficulty difficulty,
    float matchTime,
    bool sprint,
    Vector3 aimDirection,
    Player* fightTarget,
    CombatSystem& combat,
    World& world,
    std::vector<Player>& players,
    RegisterCombatEventFn&& registerCombatEvent)
{
    if (fightTarget == nullptr
        || memory.attackTimer > 0.0f
        || memory.stateTimer < BotFightReactionDelay(difficulty))
    {
        return;
    }

    std::string combatMessage;
    CombatEvent combatEvent;
    WeaponType botWeapon = WeaponType::Sword;
    const auto isVoidThreat = [&world](Vector3 position)
    {
        const GridPos underCenter = world.WorldToGrid(Vector3 { position.x, position.y - 1.40f, position.z });
        if (!world.IsAir(underCenter))
        {
            return false;
        }
        const GridPos lowerCenter = world.WorldToGrid(Vector3 { position.x, position.y - 2.18f, position.z });
        return world.IsAir(lowerCenter);
    };
    const float fightDistance = std::sqrt(DistanceSquared(bot.GetPosition(), fightTarget->GetPosition()));
    const bool enemyNearVoid = isVoidThreat(fightTarget->GetPosition())
        || isVoidThreat(Vector3 { fightTarget->GetPosition().x + aimDirection.x * 0.6f, fightTarget->GetPosition().y, fightTarget->GetPosition().z + aimDirection.z * 0.6f });
    const int nearbyEnemyCount = static_cast<int>(std::count_if(
        players.begin(),
        players.end(),
        [&bot, fightTarget](const Player& other)
        {
            return other.GetId() != fightTarget->GetId()
                && other.GetTeamId() != bot.GetTeamId()
                && other.IsAlive()
                && !other.IsEliminated()
                && DistanceSquared(other.GetPosition(), fightTarget->GetPosition()) < 4.0f;
        }));
    const bool axeMoment = bot.GetInventory().HasItem(ItemType::Axe)
        && (memory.intent == BotIntent::DefendCore
            || memory.intent == BotIntent::BreakCoreDefense
            || nearbyEnemyCount > 0
            || fightTarget->HasShield()
            || fightTarget->GetInventory().GetArmorLevel() >= 2);
    const bool spearMoment = bot.GetInventory().HasItem(ItemType::Spear)
        && !axeMoment
        && (enemyNearVoid
            || fightDistance > 2.55f
            || bot.GetHealth() < fightTarget->GetHealth() - 10
            || memory.role == BotRole::Collector);
    if (axeMoment)
    {
        botWeapon = WeaponType::Axe;
    }
    else if (spearMoment)
    {
        botWeapon = WeaponType::Spear;
    }

    AttackOptions attackOptions {};
    attackOptions.sprinting = sprint;
    attackOptions.sprintReset = sprint
        && difficulty != BotDifficulty::Easy
        && std::fmod(matchTime + static_cast<float>(bot.GetId()) * 0.37f, difficulty == BotDifficulty::Hard ? 1.20f : 1.80f) < 0.22f;
    attackOptions.attackerAirborne = !bot.IsOnGround();
    attackOptions.attackerVelocity = bot.GetVelocity();
    float botMeleeRayLimit = CombatSystem::AttackRange(botWeapon, bot.GetInventory().GetSwordLevel());
    const Vector3 botEye {
        bot.GetPosition().x,
        bot.GetPosition().y + 0.78f,
        bot.GetPosition().z
    };
    const std::optional<RaycastHit> botTerrainHit = world.Raycast(botEye, aimDirection, botMeleeRayLimit);
    if (botTerrainHit.has_value())
    {
        botMeleeRayLimit = std::max(0.0f, botTerrainHit->distance - 0.06f);
    }

    // Legacy template retained only until its callers were migrated. It must
    // never execute combat outside PlayerCommand.
    if (false)
    {
        registerCombatEvent(combatEvent, combatMessage);
        memory.attackTimer = BotAttackDelay(difficulty);
    }
    else
    {
        memory.attackTimer = BotAttackDelay(difficulty) * 0.85f;
    }
}

template <typename TryBreakDefenseFn, typename HasCoreAccessFn, typename OnCoreDestroyedFn, typename RegisterCombatEventFn>
bool TryPerformBotCoreAssault(
    Player& bot,
    BotMemory& memory,
    EnergyCore* enemyCore,
    float dt,
    World& world,
    CombatSystem& combat,
    TryBreakDefenseFn&& tryBreakDefense,
    HasCoreAccessFn&& hasCoreAccess,
    OnCoreDestroyedFn&& onCoreDestroyed,
    RegisterCombatEventFn&& registerCombatEvent)
{
    if (enemyCore == nullptr || !enemyCore->IsAlive())
    {
        return false;
    }

    const Vector3 corePos = world.GridToWorld(enemyCore->GetBlockPosition());
    const Vector3 toCore {
        corePos.x - bot.GetPosition().x,
        0.0f,
        corePos.z - bot.GetPosition().z
    };
    if (toCore.x * toCore.x + toCore.z * toCore.z >= 18.0f)
    {
        return false;
    }

    if (tryBreakDefense(bot, *enemyCore, dt))
    {
        if (memory.breakProgress >= 0.16f)
        {
            std::string coreMessage;
            CombatEvent coreEvent;
            if (false)
            {
                memory.lastAttackedCoreTeamId = enemyCore->GetTeamId();
                if (!enemyCore->IsAlive())
                {
                    onCoreDestroyed(*enemyCore);
                }
                registerCombatEvent(coreEvent, coreMessage);
            }
        }
        return true;
    }
    if (!hasCoreAccess(bot, *enemyCore))
    {
        return true;
    }

    std::string coreMessage;
    CombatEvent coreEvent;
    if (false)
    {
        memory.lastAttackedCoreTeamId = enemyCore->GetTeamId();
        if (!enemyCore->IsAlive())
        {
            onCoreDestroyed(*enemyCore);
        }
        registerCombatEvent(coreEvent, coreMessage);
    }
    return false;
}
}

struct Game::BotTeamFrameContext
{
    int teamId = -1;
    Team* team = nullptr;
    Vector3 coreHome {};
    std::vector<Player*> aliveAllies;
    std::vector<Player*> aliveEnemies;
    BotTeamSnapshot teamPlan {};
    BotRoleDistribution roleDistribution {};
    BotStrategicPlan strategicPlan {};
    Player* enemyAtCore = nullptr;
    float enemyAtCoreDistance = std::numeric_limits<float>::max();
    bool coreNeedsRepair = false;
    bool coreDefenseCritical = false;
    int missingDefenseBlocks = 0;
    int weakDefenseBlocks = 0;
};

struct Game::BotFrameContext
{
    std::vector<Player*> alivePlayers;
    std::unordered_map<int, BotTeamFrameContext> teamContexts;
};

bool Game::BotUseUtility(Player& bot, Team& team, Player* enemy, EnergyCore* enemyCore, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
    const BotTuningGenome& botTuning = BotTuningForTeam(team.id);
    if (memory.utilityTimer > 0.0f)
    {
        return false;
    }
    // Easy bots still defend themselves (heal, retreat tools) but skip the
    // offensive utility play to keep the difficulty gap.
    const bool offensiveUtilities = botDifficulty_ != BotDifficulty::Easy;
    const float cooldownScale = botDifficulty_ == BotDifficulty::Hard
        ? 0.7f
        : (botDifficulty_ == BotDifficulty::Easy ? 1.6f : 1.0f);
    const auto armUtilityCooldown = [&memory, cooldownScale](float seconds)
    {
        memory.utilityTimer = seconds * cooldownScale;
    };
    const auto issueUtilityCommand = [this, &bot, &memory, enemy](UtilityType type, Vector3 direction)
    {
        PlayerCommand command;
        command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
        command.tick = matchSimulation_.CurrentTick();
        if ((type == UtilityType::Fireball || type == UtilityType::Molotov) && enemy != nullptr)
        {
            direction = ApplyStableAimBias(
                direction, memory.personalitySeed, enemy->GetId(), botDifficulty_, memory.cautionTrait);
        }
        direction = Length(direction) > 0.01f ? Vector3Normalize(direction) : bot.Forward();
        command.aimYaw = std::atan2(direction.x, -direction.z);
        command.aimPitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
        switch (type)
        {
        case UtilityType::Heal: command.useHeal = true; break;
        case UtilityType::HomeTeleport: command.useTeleport = true; break;
        case UtilityType::Dash: command.useDash = true; break;
        case UtilityType::Fireball: command.useFireball = true; break;
        case UtilityType::Molotov: command.useMolotov = true; break;
        case UtilityType::AlarmTrap: command.useAlarm = true; break;
        default: return false;
        }
        const int before = bot.GetInventory().GetUtility(type);
        ApplyPlayerCommand(bot, command, 0.0f);
        UseUtilityInputs(bot, command);
        return bot.GetInventory().GetUtility(type) < before;
    };
    const auto issueRangedCommand = [this, &bot, &memory, enemy, dt](ItemType type, Vector3 direction,
                                                     bool hold, bool press, bool release, bool aimed)
    {
        const auto& hotbar = bot.GetInventory().GetHotbarSlots();
        int slot = -1;
        for (int index = 0; index < kHotbarSlotCount; ++index)
        {
            if (!hotbar[index].IsEmpty() && hotbar[index].type == type)
            {
                slot = index;
                break;
            }
        }
        if (slot < 0) return false;
        direction = ApplyStableAimBias(
            direction, memory.personalitySeed, enemy != nullptr ? enemy->GetId() : -1,
            botDifficulty_, memory.cautionTrait);
        PlayerCommand command;
        command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
        command.tick = matchSimulation_.CurrentTick();
        command.selectedSlot = slot;
        command.aimYaw = std::atan2(direction.x, -direction.z);
        command.aimPitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
        command.attackHeld = hold;
        command.attackPressed = press;
        command.attackReleased = release;
        command.placeHeld = aimed && type == ItemType::Blaster;
        command.scopeHeld = aimed && type == ItemType::SniperRifle;
        ApplyPlayerCommand(bot, command, 0.0f);
        ApplyNetworkPlayerActions(bot, command, dt);
        return true;
    };

    if (bot.GetHealth() <= 45 && issueUtilityCommand(UtilityType::Heal, bot.Forward()))
    {
        armUtilityCooldown(1.3f);
        return true;
    }
    // Mid-combat heals are deliberately slow: spamming them made bot fights
    // unkillable healing wars that stalled entire matches.
    if (enemy != nullptr && bot.GetHealth() <= 55 && issueUtilityCommand(UtilityType::Heal, bot.Forward()))
    {
        armUtilityCooldown(3.0f);
        return true;
    }

    if (team.coreAlive
        && bot.GetInventory().GetUtility(UtilityType::HomeTeleport) > 0
        && memory.intent == BotIntent::RetreatHome
        && (bot.GetHealth() < 42 || memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_, botTuning) + 10)
        && DistanceSquared(bot.GetPosition(), team.spawnPoint) > 260.0f
        && issueUtilityCommand(UtilityType::HomeTeleport,
            Vector3 { team.spawnPoint.x - bot.GetPosition().x, 0.0f, team.spawnPoint.z - bot.GetPosition().z }))
    {
        memory.hasNavWaypoint = false;
        memory.stuckTimer = 0.0f;
        armUtilityCooldown(2.6f);
        return true;
    }

    if (memory.role == BotRole::Defender
        && team.coreAlive
        && bot.GetInventory().GetUtility(UtilityType::AlarmTrap) > 0
        && alarmTraps_.end() == std::find_if(
            alarmTraps_.begin(),
            alarmTraps_.end(),
            [&team](const AlarmTrap& trap)
            {
                return trap.ownerTeamId == team.id && !trap.triggered;
            })
        && issueUtilityCommand(UtilityType::AlarmTrap, bot.Forward()))
    {
        AddEventMessage(bot.GetName() + " поставил тревогу на базе", GetTeamColor(team.color), 1.5f);
        armUtilityCooldown(1.5f);
        return true;
    }

    if (enemy != nullptr && offensiveUtilities)
    {
        const Vector3 toEnemy {
            enemy->GetPosition().x - bot.GetPosition().x,
            enemy->GetPosition().y + 0.35f - bot.GetPosition().y,
            enemy->GetPosition().z - bot.GetPosition().z
        };
        const float distance = std::sqrt(DistanceSquared(bot.GetPosition(), enemy->GetPosition()));

        if (distance > 5.0f
            && distance < 19.0f
            && (bot.GetInventory().HasItem(ItemType::Blaster)
                || bot.GetInventory().HasItem(ItemType::SniperRifle))
            && (memory.role == BotRole::Fighter
                || memory.role == BotRole::Defender
                || memory.intent == BotIntent::DefendCore))
        {
            const bool aimed = distance > 8.0f;
            const ItemType rangedType = bot.GetInventory().HasItem(ItemType::SniperRifle)
                ? ItemType::SniperRifle : ItemType::Blaster;
            if (bot.GetBlasterState() == CrossbowState::Unloaded)
            {
                issueRangedCommand(rangedType, toEnemy, true, false, false, aimed);
                AddFloatingText("бластер: зарядка", bot.GetPosition(), Color { 255, 96, 72, 255 });
                return true;
            }
            if (bot.GetBlasterState() == CrossbowState::Loading)
            {
                if (issueRangedCommand(rangedType, toEnemy, true, false, false, aimed)
                    && bot.GetBlasterState() == CrossbowState::Loaded)
                {
                    AddFloatingText("бластер: готов", bot.GetPosition(), Color { 104, 255, 128, 255 });
                }
                return true;
            }
            if (issueRangedCommand(rangedType, toEnemy, false, true, false, aimed))
            {
                AddFloatingText(aimed ? "бластер: прицел" : "бластер", bot.GetPosition(), Color { 98, 245, 255, 255 });
                return true;
            }
        }

        if (distance > 4.2f
            && distance < 11.0f
            && bot.GetInventory().HasItem(ItemType::Bow)
            && bot.GetArrowReloadTimer(bot.GetArrowVariant()) <= 0.0f
            && (memory.role == BotRole::Fighter || memory.role == BotRole::Rusher || botDifficulty_ == BotDifficulty::Hard))
        {
            const float drawPower = distance > 7.0f ? 1.0f : 0.72f;
            const bool releaseBow = bot.GetBowDrawTimer() >= kBowTuning.fullDrawTime * drawPower;
            if (issueRangedCommand(ItemType::Bow, toEnemy, !releaseBow, false, releaseBow, false))
            {
                if (!releaseBow)
                {
                    return true;
                }
                AddFloatingText(drawPower >= 1.0f ? "лук: полный" : "лук", bot.GetPosition(), Color { 112, 232, 255, 255 });
                armUtilityCooldown(kBowTuning.fullDrawTime * drawPower + 0.25f);
                return true;
            }
        }

        // Tactical opportunity, rather than a distance-only duel rule.  Aim at
        // where the enemy will be when the projectile arrives; a target over a
        // void/bridge gets the largest utility because breaking its support can
        // immediately remove a player from the next fight.
        const float flightSeconds = distance / std::max(1.0f, kFireballTuning.speed);
        const Vector3 enemyVelocity = enemy->GetVelocity();
        const Vector3 predictedEnemy {
            enemy->GetPosition().x + enemyVelocity.x * std::min(flightSeconds, 0.65f),
            enemy->GetPosition().y + enemyVelocity.y * std::min(flightSeconds, 0.35f),
            enemy->GetPosition().z + enemyVelocity.z * std::min(flightSeconds, 0.65f)
        };
        const bool bridgeOpportunity = IsVoidThreatAt(predictedEnemy)
            || IsVoidThreatAt(Vector3 { predictedEnemy.x + toEnemy.x * 0.22f, predictedEnemy.y, predictedEnemy.z + toEnemy.z * 0.22f });
        const bool baseDefenseOpportunity = memory.intent == BotIntent::DefendCore
            && DistanceSquared(predictedEnemy, team.spawnPoint) < 70.0f;
        float fireballOpportunityScore = 0.0f;
        if (bridgeOpportunity) fireballOpportunityScore += 100.0f;
        if (baseDefenseOpportunity) fireballOpportunityScore += 58.0f;
        if (enemy->GetHealth() <= 34) fireballOpportunityScore += 18.0f;
        if (memory.intent == BotIntent::PressureCore || memory.intent == BotIntent::BreakCoreDefense) fireballOpportunityScore += 14.0f;
        if (distance > 7.0f) fireballOpportunityScore += 8.0f;
        if (distance > 4.5f
            && distance < 14.0f
            && fireballOpportunityScore >= 58.0f
            && bot.CanAttack()
            && bot.GetInventory().GetUtility(UtilityType::Fireball) > 0)
        {
            const Vector3 predictedAim {
                predictedEnemy.x - bot.GetPosition().x,
                predictedEnemy.y + 0.35f - bot.GetPosition().y,
                predictedEnemy.z - bot.GetPosition().z
            };
            if (issueUtilityCommand(UtilityType::Fireball, predictedAim))
            {
                if (AutomatchBotStats* stats = FindAutomatchBotStats(bot))
                {
                    ++stats->fireballTacticalUses;
                    if (bridgeOpportunity) ++stats->fireballBridgeOpportunities;
                }
                AddFloatingText(bridgeOpportunity ? "fireball: bridge" : "fireball: pressure", bot.GetPosition(), Color { 255, 178, 96, 255 });
                armUtilityCooldown(1.9f);
                return true;
            }
        }

        const bool molotovFight = bot.GetInventory().GetUtility(UtilityType::Molotov) > 0
            && distance > 5.0f
            && distance < 12.0f
            && (memory.intent == BotIntent::DefendCore
                || memory.intent == BotIntent::FightEnemy
                || memory.intent == BotIntent::BreakCoreDefense);
        if (molotovFight && issueUtilityCommand(UtilityType::Molotov, toEnemy))
        {
            AddFloatingText("molotov", bot.GetPosition(), Color { 255, 128, 72, 255 });
            armUtilityCooldown(2.2f);
            return true;
        }

        const bool dashCommit = memory.role == BotRole::Fighter
            || memory.intent == BotIntent::ChaseWeakEnemy
            || (memory.role == BotRole::Rusher && enemy->GetHealth() <= bot.GetHealth() - 16);
        if (distance > 3.8f && distance < 9.0f && dashCommit && bot.GetInventory().GetUtility(UtilityType::Dash) > 0)
        {
            if (issueUtilityCommand(UtilityType::Dash, toEnemy))
            {
                armUtilityCooldown(2.6f);
                return true;
            }
        }
    }

    if (enemyCore != nullptr
        && offensiveUtilities
        && enemyCore->IsAlive()
        && memory.role == BotRole::Rusher
        && bot.CanAttack()
        && bot.GetInventory().GetUtility(UtilityType::Fireball) > 0)
    {
        const Vector3 corePos = world_.GridToWorld(enemyCore->GetBlockPosition());
        const float coreDistance = std::sqrt(DistanceSquared(bot.GetPosition(), corePos));
        if (coreDistance > 5.0f && coreDistance < 16.0f)
        {
            const std::optional<GridPos> defense = FindCoreDefenseBlock(*enemyCore, bot);
            if (defense.has_value()
                && bot.GetInventory().GetUtility(UtilityType::Molotov) > 0
                && coreDistance < 12.0f
                && issueUtilityCommand(UtilityType::Molotov, Vector3 {
                    world_.GridToWorld(*defense).x - bot.GetPosition().x,
                    world_.GridToWorld(*defense).y + 0.35f - bot.GetPosition().y,
                    world_.GridToWorld(*defense).z - bot.GetPosition().z }))
            {
                const Vector3 defensePos = world_.GridToWorld(*defense);
                AddFloatingText("molotov", bot.GetPosition(), Color { 255, 128, 72, 255 });
                armUtilityCooldown(2.2f);
                return true;
            }
            if (defense.has_value())
            {
                const Vector3 defensePos = world_.GridToWorld(*defense);
                if (!issueUtilityCommand(UtilityType::Fireball, Vector3 {
                    defensePos.x - bot.GetPosition().x,
                    defensePos.y + 0.35f - bot.GetPosition().y,
                    defensePos.z - bot.GetPosition().z }))
                {
                    return false;
                }
                if (AutomatchBotStats* stats = FindAutomatchBotStats(bot))
                {
                    ++stats->fireballTacticalUses;
                    ++stats->fireballDefenseOpportunities;
                }
                AddFloatingText("fireball: breach", bot.GetPosition(), Color { 255, 178, 96, 255 });
                armUtilityCooldown(1.9f);
                return true;
            }
        }
    }

    return false;
}

bool Game::BotCastHeroAbility(Player& bot, HeroAbilitySlot slot)
{
    // Keep local HUD messages quiet, but preserve world VFX and cast SFX so
    // opponents can read and react to a bot's ability.
    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
    command.tick = matchSimulation_.CurrentTick();
    command.aimYaw = bot.GetYaw();
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        command.useAbility1 = true;
        break;
    case HeroAbilitySlot::Active2:
        command.useAbility2 = true;
        break;
    case HeroAbilitySlot::Ultimate:
        command.useUltimate = true;
        break;
    }

    const bool previousSuppress = suppressLocalFeedback_;
    suppressLocalFeedback_ = true;
    const bool used = ApplyPlayerActionCommand(bot, command);
    suppressLocalFeedback_ = previousSuppress;
    if (used && automatch_.active && bot.GetTeamId() >= 0 && bot.GetTeamId() < 4)
    {
        const int teamId = bot.GetTeamId();
        const std::uint32_t now = matchSimulation_.CurrentTick();
        const std::uint32_t previous = automatch_.lastAbilityTickByTeam[teamId];
        const int previousActor = automatch_.lastAbilityActorByTeam[teamId];
        if (previousActor >= 0 && previousActor != bot.GetId()
            && now >= previous && now - previous <= 3u * 60u)
        {
            RecordMemorableMoment(
                "AbilityCombo", teamId, -1, { previousActor, bot.GetId() },
                bot.GetName() + " chained a hero ability with a teammate",
                0.64f, true, bot.GetHealth(), -1, 0,
                { "teammate committed an ability", "second bot read the same fight", "abilities overlapped" });
        }
        automatch_.lastAbilityTickByTeam[teamId] = now;
        automatch_.lastAbilityActorByTeam[teamId] = bot.GetId();
    }
    return used;
}

bool Game::BotUseHeroAbility(Player& bot, Team& team, Player* enemy, EnergyCore* enemyCore)
{
    BotMemory& memory = GetBotMemory(bot);
    if (memory.heroAbilityTimer > 0.0f)
    {
        return false;
    }

    const float retryDelay = 0.6f;
    const float castDelay = botDifficulty_ == BotDifficulty::Hard
        ? 1.1f
        : (botDifficulty_ == BotDifficulty::Easy ? 3.2f : 1.8f);
    const auto cast = [&](HeroAbilitySlot slot, Vector3 faceTarget, bool face)
    {
        if (face)
        {
            const Vector3 faceDirection {
                faceTarget.x - bot.GetPosition().x,
                0.0f,
                faceTarget.z - bot.GetPosition().z
            };
            const PlayerCommand aimCommand = BuildBotMovementCommand(
                bot,
                matchSimulation_.CurrentTick(),
                Vector3 {},
                faceDirection,
                false,
                false);
            ApplyPlayerCommand(bot, aimCommand, 0.0f);
        }
        const bool used = BotCastHeroAbility(bot, slot);
        memory.heroAbilityTimer = used ? castDelay : retryDelay;
        return used;
    };

    const HeroRuntimeState& heroState = bot.GetHeroState();
    const Inventory& inventory = bot.GetInventory();
    const float enemyDistance = enemy != nullptr
        ? std::sqrt(DistanceSquared(bot.GetPosition(), enemy->GetPosition()))
        : 999.0f;
    const bool fightingIntent = memory.intent == BotIntent::FightEnemy
        || memory.intent == BotIntent::DefendCore
        || memory.intent == BotIntent::ChaseWeakEnemy;
    const bool defendingBase = memory.intent == BotIntent::DefendCore
        || memory.intent == BotIntent::RepairCoreDefense;
    const bool assaultIntent = memory.intent == BotIntent::PressureCore
        || memory.intent == BotIntent::BreakCoreDefense;
    const float distanceFromHomeSq = DistanceSquared(bot.GetPosition(), team.spawnPoint);

    switch (bot.GetHeroId())
    {
    case HeroId::Radon:
    {
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate))
        {
            EnergyCore* ownCore = FindCoreByTeam(team.id);
            const bool coreAlive = ownCore != nullptr && ownCore->IsAlive();
            // Prime the sacrifice only when the base is in real danger; the
            // primed state is a toggle, so never re-cast while primed.
            const bool coreInDanger = coreAlive
                && (memory.coreDefenseCritical
                    || ownCore->GetHealth() <= ownCore->GetMaxHealth() / 3
                    || (enemy != nullptr
                        && DistanceSquared(enemy->GetPosition(), world_.GridToWorld(ownCore->GetBlockPosition())) < 60.0f));
            if (coreAlive && coreInDanger && !heroState.ultimatePrimed)
            {
                return cast(HeroAbilitySlot::Ultimate, Vector3 {}, false);
            }
            if (!coreAlive && enemy != nullptr && enemyDistance < 4.5f)
            {
                return cast(HeroAbilitySlot::Ultimate, enemy->GetPosition(), true);
            }
        }
        // Force pulse: shove a close attacker away, extra value near the void.
        if (enemy != nullptr
            && enemyDistance < 4.0f
            && bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && (defendingBase
                || bot.GetHealth() <= enemy->GetHealth() + 12
                || IsVoidThreatAt(enemy->GetPosition())))
        {
            return cast(HeroAbilitySlot::Active1, enemy->GetPosition(), true);
        }
        // Molotov zones a mid-range target while fighting or sieging.
        if (enemy != nullptr
            && enemyDistance > 4.5f
            && enemyDistance < 12.0f
            && (fightingIntent || assaultIntent)
            && bot.IsHeroAbilityReady(HeroAbilitySlot::Active2))
        {
            return cast(HeroAbilitySlot::Active2, enemy->GetPosition(), true);
        }
        break;
    }
    case HeroId::Orbita:
    {
        // Escape teleport: hurt and cornered. The teleport costs health, so
        // skip it when nearly dead.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && enemy != nullptr
            && enemyDistance < 6.0f
            && bot.GetHealth() < 40
            && bot.GetHealth() > 18)
        {
            return cast(
                HeroAbilitySlot::Ultimate,
                Vector3 { team.spawnPoint.x, bot.GetPosition().y, team.spawnPoint.z },
                true);
        }
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1) && bot.IsOnGround())
        {
            // Gap-closing dash on runners; this also primes the momentum strike.
            if (enemy != nullptr
                && enemyDistance > 4.5f
                && enemyDistance < 11.0f
                && (memory.intent == BotIntent::ChaseWeakEnemy
                    || (fightingIntent && bot.GetHealth() >= enemy->GetHealth() - 8)))
            {
                return cast(HeroAbilitySlot::Active1, enemy->GetPosition(), true);
            }
            // Disengage dash when retreating with an enemy on top of us.
            if (enemy != nullptr && enemyDistance < 4.0f && memory.intent == BotIntent::RetreatHome)
            {
                const Vector3 away {
                    bot.GetPosition().x * 2.0f - enemy->GetPosition().x,
                    bot.GetPosition().y,
                    bot.GetPosition().z * 2.0f - enemy->GetPosition().z
                };
                return cast(HeroAbilitySlot::Active1, away, true);
            }
        }
        // Phantom bridge: cross a gap toward the objective without real blocks.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2)
            && assaultIntent
            && inventory.GetBlocks() <= 4)
        {
            const Vector3 ahead {
                bot.GetPosition().x + bot.Forward().x * 1.6f,
                bot.GetPosition().y,
                bot.GetPosition().z + bot.Forward().z * 1.6f
            };
            if (IsVoidThreatAt(ahead))
            {
                return cast(HeroAbilitySlot::Active2, Vector3 {}, false);
            }
        }
        break;
    }
    case HeroId::Brom:
    {
        const bool nearHome = distanceFromHomeSq < 900.0f;
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && nearHome
            && (defendingBase || memory.coreDefenseCritical))
        {
            return cast(HeroAbilitySlot::Ultimate, Vector3 {}, false);
        }
        // Kamikaze drone intercepts attackers near the base. Keep a small gold
        // reserve when the bot is not actively defending.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2)
            && inventory.GetResource(ResourceType::Gold) >= 12 + (defendingBase ? 0 : 8)
            && (defendingBase || (enemy != nullptr && enemyDistance < 9.0f)))
        {
            return cast(HeroAbilitySlot::Active2, Vector3 {}, false);
        }
        // Vacuum bot only with a clear iron surplus and nobody attacking.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && enemy == nullptr
            && inventory.GetResource(ResourceType::Iron) >= 60
            && nearHome)
        {
            return cast(HeroAbilitySlot::Active1, Vector3 {}, false);
        }
        break;
    }
    case HeroId::Konvoy:
    {
        // Containment dome on a committed kill or base defense.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && enemy != nullptr
            && enemyDistance < 5.0f
            && (memory.intent == BotIntent::ChaseWeakEnemy
                || defendingBase
                || enemy->GetHealth() < bot.GetHealth()))
        {
            return cast(HeroAbilitySlot::Ultimate, enemy->GetPosition(), true);
        }
        // Handcuffs tether a kiting enemy.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2)
            && enemy != nullptr
            && enemyDistance < 5.5f
            && fightingIntent)
        {
            return cast(HeroAbilitySlot::Active2, enemy->GetPosition(), true);
        }
        // Defenders seed traps around the base while it is quiet.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && memory.role == BotRole::Defender
            && enemy == nullptr
            && distanceFromHomeSq < 110.0f)
        {
            return cast(HeroAbilitySlot::Active1, Vector3 {}, false);
        }
        break;
    }
    case HeroId::Likho:
    {
        // Silent steps into the backline while sieging or stalking.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && enemy != nullptr
            && enemyDistance < 14.0f
            && (assaultIntent || memory.intent == BotIntent::ChaseWeakEnemy))
        {
            return cast(HeroAbilitySlot::Active1, enemy->GetPosition(), true);
        }
        // Bleed once we are trading hits.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2)
            && enemy != nullptr
            && enemyDistance < 3.5f)
        {
            return cast(HeroAbilitySlot::Active2, enemy->GetPosition(), true);
        }
        // Disguise to slip toward the enemy core.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && assaultIntent
            && enemyCore != nullptr
            && DistanceSquared(bot.GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 900.0f)
        {
            return cast(HeroAbilitySlot::Ultimate, Vector3 {}, false);
        }
        break;
    }
    case HeroId::Svidetel:
    {
        // Echo support when entering a fight.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active1)
            && enemy != nullptr
            && enemyDistance < 8.0f
            && fightingIntent)
        {
            return cast(HeroAbilitySlot::Active1, enemy->GetPosition(), true);
        }
        // Phase a blocked path open.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Active2) && memory.stuckTimer > 1.4f)
        {
            return cast(HeroAbilitySlot::Active2, Vector3 {}, false);
        }
        // Void contours to find the last runners.
        if (bot.IsHeroAbilityReady(HeroAbilitySlot::Ultimate)
            && (memory.intent == BotIntent::ChaseWeakEnemy || matchSimulation_.MatchTimeSeconds() > 150.0f))
        {
            return cast(HeroAbilitySlot::Ultimate, Vector3 {}, false);
        }
        break;
    }
    }

    return false;
}

void Game::UpdateBots(float dt)
{
    // Full path searches allowed this tick (see ChooseBotPathWaypoint): caps
    // worst-case burst cost when many bots need to replan at once.
    pathSearchBudgetThisTick_ = 4;
    for (TeamCoordinationBus& bus : teamCoordBuses_)
    {
        bus.Prune(matchSimulation_.MatchTimeSeconds(), kCoordinationSignalTtl);
    }
    for (CoreDefenseMonitor& monitor : coreDefenseMonitors_)
    {
        monitor.checkTimer = std::max(0.0f, monitor.checkTimer - dt);
    }

    for (Player& bot : players_)
    {
        if (IsBotControlled(ControlKindForPlayer(bot)) && bot.IsAlive() && !bot.IsEliminated())
        {
            GetBotMemory(bot);
        }
    }

    BotFrameContext frameContext {};
    frameContext.alivePlayers.reserve(players_.size());
    for (Player& player : players_)
    {
        if (player.IsAlive() && !player.IsEliminated())
        {
            frameContext.alivePlayers.push_back(&player);
        }
    }

    frameContext.teamContexts.reserve(teams_.size());
    for (Team& team : teams_)
    {
        BotTeamFrameContext teamContext {};
        teamContext.teamId = team.id;
        teamContext.team = &team;
        teamContext.coreHome = world_.GridToWorld(team.coreBlock);
        if (team.id >= 0 && team.id < static_cast<int>(coreDefenseMonitors_.size()))
        {
            CoreDefenseMonitor& defenseMonitor = coreDefenseMonitors_[team.id];
            if (!team.coreAlive)
            {
                defenseMonitor = CoreDefenseMonitor {};
            }
            else if (defenseMonitor.checkTimer <= 0.0f)
            {
                const CoreDefenseStatus status = InspectCoreDefense(world_, team, TeamDefenseCells(team));
                defenseMonitor.missingBlocks = status.missingBlocks;
                defenseMonitor.weakBlocks = status.weakBlocks;
                defenseMonitor.critical = status.critical;
                defenseMonitor.checkTimer = botDifficulty_ == BotDifficulty::Hard
                    ? 0.85f
                    : (botDifficulty_ == BotDifficulty::Easy ? 1.75f : 1.20f);
            }
            // Tolerate a couple of open cells: on open-core maps the last
            // shell holes are routinely occupied by teammates' bodies, and a
            // hard "0 missing" requirement kept whole teams camping repair
            // instead of playing.  Three or more holes is a real breach.
            teamContext.coreNeedsRepair = team.coreAlive && defenseMonitor.missingBlocks > 2;
            teamContext.coreDefenseCritical = team.coreAlive && defenseMonitor.critical;
            teamContext.missingDefenseBlocks = defenseMonitor.missingBlocks;
            teamContext.weakDefenseBlocks = defenseMonitor.weakBlocks;
        }
        else
        {
            teamContext.coreNeedsRepair = team.coreAlive && FindMissingCoreDefenseBlock(team).has_value();
        }
        teamContext.aliveAllies.reserve(frameContext.alivePlayers.size());
        teamContext.aliveEnemies.reserve(frameContext.alivePlayers.size());

        for (Player* player : frameContext.alivePlayers)
        {
            if (player == nullptr)
            {
                continue;
            }

            if (player->GetTeamId() == team.id)
            {
                teamContext.aliveAllies.push_back(player);
                ++teamContext.teamPlan.aliveAllies;
                const bool nearCore = DistanceSquared(player->GetPosition(), teamContext.coreHome) < 72.0f;
                if (nearCore)
                {
                    ++teamContext.teamPlan.alliesNearCore;
                }

                if (IsBotControlled(ControlKindForPlayer(*player)))
                {
                    const BotMemory* memory = FindBotMemoryByPlayerId(botMemories_, botMemoryIndexByPlayerId_, player->GetId());
                    const BotRole role = memory != nullptr ? memory->role : RoleForBotId(player->GetId());
                    ++teamContext.roleDistribution.total;
                    switch (role)
                    {
                    case BotRole::Defender:
                        ++teamContext.roleDistribution.defenders;
                        if (nearCore)
                        {
                            ++teamContext.teamPlan.defendersNearCore;
                        }
                        break;
                    case BotRole::Rusher:
                        ++teamContext.roleDistribution.rushers;
                        break;
                    case BotRole::Collector:
                        ++teamContext.roleDistribution.collectors;
                        break;
                    case BotRole::Fighter:
                        ++teamContext.roleDistribution.fighters;
                        break;
                    }

                    if (memory != nullptr)
                    {
                        if (memory->intent == BotIntent::PressureCore
                            || memory->intent == BotIntent::BreakCoreDefense
                            || memory->intent == BotIntent::ChaseWeakEnemy)
                        {
                            ++teamContext.teamPlan.activePressure;
                        }
                        else if (memory->intent == BotIntent::GearUp)
                        {
                            ++teamContext.teamPlan.activeShop;
                        }
                        else if (memory->intent == BotIntent::RepairCoreDefense)
                        {
                            ++teamContext.teamPlan.activeRepair;
                        }
                        else if (memory->intent == BotIntent::SecureResources)
                        {
                            ++teamContext.teamPlan.activeResource;
                        }
                    }
                }
                continue;
            }

            teamContext.aliveEnemies.push_back(player);
            const float coreDistance = DistanceSquared(player->GetPosition(), teamContext.coreHome);
            // Team knowledge is earned: an intruder near the Core is shared
            // only when at least one living ally has a real line of sight.
            // Merely existing in the authoritative player array is not a
            // sensor and must not wake every defender through walls.
            const bool witnessedByTeam = std::any_of(
                teamContext.aliveAllies.begin(), teamContext.aliveAllies.end(),
                [this, player](const Player* ally)
                {
                    return ally != nullptr && BotHasLineOfSight(*ally, *player);
                });
            if (witnessedByTeam && coreDistance < 42.0f && coreDistance < teamContext.enemyAtCoreDistance)
            {
                teamContext.enemyAtCore = player;
                teamContext.enemyAtCoreDistance = coreDistance;
            }
        }

        frameContext.teamContexts.emplace(team.id, std::move(teamContext));
    }

    for (auto& entry : frameContext.teamContexts)
    {
        BotTeamFrameContext& teamContext = entry.second;
        if (teamContext.team == nullptr)
        {
            continue;
        }

        BotStrategicPlan plan {};
        const Team& team = *teamContext.team;
        const BotTuningGenome teamTuning = ScaledBotTuningForTeam(team.id);
        const bool hypixelRush = botStrategyProfile_ == BotStrategyProfile::HypixelRush;
        // Hypixel-style play treats an unfinished shell as routine work for one
        // defender. Only a witnessed attacker turns it into a team emergency.
        const bool effectiveDefenseCritical = teamContext.coreDefenseCritical
            && (!hypixelRush || teamContext.enemyAtCore != nullptr);
        plan.desiredDefenders = effectiveDefenseCritical ? 2 : 1;
        if (team.coreAlive)
        {
            if (teamContext.enemyAtCore != nullptr)
            {
                plan.defenseUrgency += 520.0f;
                plan.desiredDefenders = 2;
            }
            const bool defenseCriticalNow = effectiveDefenseCritical && matchSimulation_.MatchTimeSeconds() > 55.0f;
            if (defenseCriticalNow)
            {
                plan.defenseUrgency += 360.0f;
            }
            if (matchSimulation_.MatchTimeSeconds() > 40.0f || teamContext.enemyAtCore != nullptr)
            {
                plan.defenseUrgency += static_cast<float>(teamContext.missingDefenseBlocks) * 24.0f;
            }
            if (teamContext.teamPlan.defendersNearCore == 0 && (teamContext.coreNeedsRepair || teamContext.enemyAtCore != nullptr))
            {
                plan.defenseUrgency += matchSimulation_.MatchTimeSeconds() > 55.0f || teamContext.enemyAtCore != nullptr ? 140.0f : 0.0f;
            }
        }

        EnergyCore* bestCore = nullptr;
        int aliveEnemyCores = 0;
        TeamCoordinationBus* coordBus = team.id >= 0 && team.id < static_cast<int>(teamCoordBuses_.size())
            ? &teamCoordBuses_[team.id]
            : nullptr;
        // Elect emergency defenders once for the whole strategic frame.  The
        // previous per-bot fallback ("nobody has broadcast DefendingCore")
        // made every bot answer the same help call before a stable signal could
        // exist.  Prefer the current Defender, then proximity and stable id.
        std::array<std::pair<float, int>, 2> defenderCandidates {{
            { std::numeric_limits<float>::max(), -1 },
            { std::numeric_limits<float>::max(), -1 }
        }};
        for (const Player* candidate : teamContext.aliveAllies)
        {
            if (candidate == nullptr || !candidate->IsAlive() || candidate->IsEliminated()
                || !IsBotControlled(ControlKindForPlayer(*candidate)))
            {
                continue;
            }
            const BotMemory* candidateMemory = FindBotMemoryByPlayerId(
                botMemories_, botMemoryIndexByPlayerId_, candidate->GetId());
            const bool defenderRole = candidateMemory != nullptr
                && candidateMemory->role == BotRole::Defender;
            const float score = (defenderRole ? 0.0f : 10000.0f)
                + DistanceSquared(candidate->GetPosition(), teamContext.coreHome)
                + static_cast<float>(candidate->GetId()) * 0.001f;
            const std::pair<float, int> ranked { score, candidate->GetId() };
            if (ranked.first < defenderCandidates[0].first)
            {
                defenderCandidates[1] = defenderCandidates[0];
                defenderCandidates[0] = ranked;
            }
            else if (ranked.first < defenderCandidates[1].first)
            {
                defenderCandidates[1] = ranked;
            }
        }
        const float now = matchSimulation_.MatchTimeSeconds();
        const auto validDefenderId = [&teamContext](int playerId)
        {
            return std::any_of(teamContext.aliveAllies.begin(), teamContext.aliveAllies.end(),
                [playerId](const Player* player)
                {
                    return player != nullptr && player->GetId() == playerId
                        && player->IsAlive() && !player->IsEliminated();
                });
        };
        const bool emergencyAssignmentActive = plan.desiredDefenders > 1
            && coordBus != nullptr
            && coordBus->emergencyDefenderAssignmentUntil > now
            && validDefenderId(coordBus->emergencyPrimaryDefenderId)
            && validDefenderId(coordBus->emergencySecondaryDefenderId);
        if (emergencyAssignmentActive)
        {
            plan.primaryDefenderId = coordBus->emergencyPrimaryDefenderId;
            plan.secondaryDefenderId = coordBus->emergencySecondaryDefenderId;
        }
        else
        {
            plan.primaryDefenderId = defenderCandidates[0].second;
            plan.secondaryDefenderId = plan.desiredDefenders > 1
                ? defenderCandidates[1].second : -1;
            if (coordBus != nullptr && plan.desiredDefenders > 1)
            {
                coordBus->emergencyPrimaryDefenderId = plan.primaryDefenderId;
                coordBus->emergencySecondaryDefenderId = plan.secondaryDefenderId;
                coordBus->emergencyDefenderAssignmentUntil = now + 8.0f;
            }
        }
        if (coordBus != nullptr)
        {
            coordBus->reserveDefenderId = plan.primaryDefenderId;
        }
        BotAttackCommitment localCommitment;
        BotAttackCommitment& attackCommitment = coordBus != nullptr
            ? coordBus->attackCommitment : localCommitment;
        EnergyCore* committedCore = FindCoreByTeam(attackCommitment.teamId);
        const bool recheckAttack = now >= attackCommitment.recheckAt
            || committedCore == nullptr || !committedCore->IsAlive();
        std::vector<BotAttackOption> attackOptions;
        int availableBridgeBlocks = 0;
        for (const Player* ally : teamContext.aliveAllies)
            if (ally != nullptr) availableBridgeBlocks = std::max(availableBridgeBlocks, ally->GetInventory().GetBlocks());
        for (EnergyCore& core : matchSimulation_.Cores())
        {
            if (core.GetTeamId() == team.id || !core.IsAlive()) continue;
            ++aliveEnemyCores;
            if (!recheckAttack) continue;
            BotAttackOption option;
            option.teamId = core.GetTeamId();
            const Vector3 corePosition = world_.GridToWorld(core.GetBlockPosition());
            option.travelSeconds = std::sqrt(DistanceSquared(teamContext.coreHome, corePosition)) / 4.0f;
            if (!routeGraph_.Empty())
            {
                const RouteCorridor corridor = routeGraph_.FindCorridor(
                    world_.WorldToGrid(teamContext.coreHome), core.GetBlockPosition(), team.id);
                option.reachable = corridor.valid;
                if (corridor.valid)
                {
                    option.travelSeconds = corridor.cost / 4.0f;
                    int constructionBlocks = 0;
                    for (const RouteCorridorSegment& segment : corridor.segments)
                    {
                        if (!segment.IsBridge()) continue;
                        const auto low = static_cast<std::uint32_t>(std::min(segment.fromNodeIndex, segment.toNodeIndex) + 1);
                        const auto high = static_cast<std::uint32_t>(std::max(segment.fromNodeIndex, segment.toNodeIndex) + 1);
                        const std::uint64_t signature = (static_cast<std::uint64_t>(low) << 32u) | high;
                        if (coordBus == nullptr || !coordBus->IsRouteOpened(signature, now))
                            constructionBlocks += segment.expectedBridgeBlocks;
                    }
                    option.preparationSeconds = constructionBlocks * 0.25f
                        + std::max(0, constructionBlocks - availableBridgeBlocks) * 0.5f;
                }
            }
            option.remainingHealthFraction = static_cast<float>(core.GetHealth()) / std::max(1, core.GetMaxHealth());
            option.supportingAttackers = coordBus != nullptr ? coordBus->CountSignal(
                CoordinationSignal::AttackingCore, now, kCoordinationSignalTtl, core.GetTeamId()) : 0;
            for (const Player* enemy : teamContext.aliveEnemies)
            {
                if (enemy == nullptr || DistanceSquared(enemy->GetPosition(), corePosition) > 144.0f) continue;
                const bool observed = std::any_of(teamContext.aliveAllies.begin(), teamContext.aliveAllies.end(),
                    [this, enemy](const Player* ally) { return ally != nullptr && BotHasLineOfSight(*ally, *enemy); });
                if (observed) ++option.observedDefenders;
            }
            for (const Player* ally : teamContext.aliveAllies)
            {
                if (ally == nullptr) continue;
                const BotMemory* memory = FindBotMemoryByPlayerId(botMemories_, botMemoryIndexByPlayerId_, ally->GetId());
                if (memory != nullptr && memory->currentPlan.targetTeamId == core.GetTeamId()
                    && memory->routeFailureCooldown > 0.0f)
                    option.recentRouteFailures = std::max(option.recentRouteFailures, memory->repeatedRouteFailures);
            }
            attackOptions.push_back(option);
        }
        const int selectedCoreTeam = recheckAttack
            ? SelectBotAttackOption(attackOptions, attackCommitment, now) : attackCommitment.teamId;
        bestCore = FindCoreByTeam(selectedCoreTeam);

        if (bestCore != nullptr)
        {
            plan.attackCoreTeamId = bestCore->GetTeamId();
            plan.allIn = !team.coreAlive || matchSimulation_.MatchTimeSeconds() > teamTuning.allInSeconds || aliveEnemyCores <= 1;
            plan.desiredAttackers = plan.allIn ? 4
                : (matchSimulation_.MatchTimeSeconds() > teamTuning.latePressureSeconds ? 3
                    : (matchSimulation_.MatchTimeSeconds() > teamTuning.pressurePhaseSeconds ? (hypixelRush ? 3 : 2)
                        : (hypixelRush ? 2 : 1)));
            plan.attackUrgency = 220.0f
                + std::max(0.0f, matchSimulation_.MatchTimeSeconds() - 70.0f) * 1.8f
                + static_cast<float>(bestCore->GetMaxHealth() - bestCore->GetHealth()) * 2.0f
                + (plan.allIn ? 220.0f : 0.0f)
                + (hypixelRush && matchSimulation_.MatchTimeSeconds() > teamTuning.pressurePhaseSeconds ? 165.0f : 0.0f);
        }

        // If one enemy team has already lost its Core while other Cores remain,
        // peel off exactly one combat bot to finish it.  The assignment lives on
        // the coordination bus, so dynamic roles and simulation order cannot
        // make all four bots swap into cleanup on the same frame.
        BotTeamFrameContext* cleanupTargetContext = nullptr;
        if (coordBus != nullptr)
        {
            const auto eligibleCleanupTarget = [&frameContext, &team](int targetTeamId) -> BotTeamFrameContext*
            {
                const auto found = frameContext.teamContexts.find(targetTeamId);
                if (found == frameContext.teamContexts.end()
                    || found->second.team == nullptr
                    || found->second.team->id == team.id
                    || found->second.team->coreAlive
                    || found->second.aliveAllies.empty())
                {
                    return nullptr;
                }
                return &found->second;
            };

            cleanupTargetContext = eligibleCleanupTarget(coordBus->cleanupTargetTeamId);
            if (cleanupTargetContext == nullptr)
            {
                float bestCleanupScore = std::numeric_limits<float>::max();
                for (Team& candidateTeam : teams_)
                {
                    BotTeamFrameContext* candidate = eligibleCleanupTarget(candidateTeam.id);
                    if (candidate == nullptr)
                    {
                        continue;
                    }
                    const float score = DistanceSquared(teamContext.coreHome, candidate->coreHome)
                        + static_cast<float>(candidateTeam.id) * 0.01f;
                    if (score < bestCleanupScore)
                    {
                        bestCleanupScore = score;
                        cleanupTargetContext = candidate;
                    }
                }

                coordBus->cleanupTargetTeamId = cleanupTargetContext != nullptr
                    ? cleanupTargetContext->teamId : -1;
                coordBus->cleanupHunterId = -1;
                coordBus->cleanupLastKnownPosition = {};
                coordBus->cleanupLastSeenTimestamp = -1000.0f;
            }

            if (cleanupTargetContext != nullptr)
            {
                const bool finalCleanupPhase = bestCore == nullptr;
                const auto validCleanupHunter = [this, &team](int playerId)
                {
                    return std::any_of(players_.begin(), players_.end(), [this, &team, playerId](const Player& player)
                    {
                        const BotMemory* memory = FindBotMemoryByPlayerId(
                            botMemories_, botMemoryIndexByPlayerId_, player.GetId());
                        return player.GetId() == playerId
                            && player.GetTeamId() == team.id
                            && !player.IsEliminated()
                            && (memory == nullptr
                                || memory->routeFailureCooldown <= 0.0f
                                || memory->repeatedRouteFailures < 2)
                            && IsBotControlled(ControlKindForPlayer(player));
                    });
                };
                const bool currentHunterHasFinalKit = std::any_of(
                    players_.begin(), players_.end(), [this, coordBus](const Player& player)
                    {
                        if (player.GetId() != coordBus->cleanupHunterId) return false;
                        const BotMemory* hunterMemory = FindBotMemoryByPlayerId(
                            botMemories_, botMemoryIndexByPlayerId_, player.GetId());
                        return player.GetInventory().GetBlocks() >= 32
                            || (hunterMemory != nullptr && hunterMemory->cleanupBridgeKitReady);
                    });
                if (!validCleanupHunter(coordBus->cleanupHunterId)
                    || (finalCleanupPhase && !currentHunterHasFinalKit))
                {
                    float bestHunterScore = std::numeric_limits<float>::max();
                    int bestHunterId = -1;
                    for (const Player& candidate : players_)
                    {
                        if (candidate.GetTeamId() != team.id
                            || candidate.IsEliminated()
                            || !IsBotControlled(ControlKindForPlayer(candidate)))
                        {
                            continue;
                        }
                        const BotMemory* candidateMemory = FindBotMemoryByPlayerId(
                            botMemories_, botMemoryIndexByPlayerId_, candidate.GetId());
                        if (candidateMemory != nullptr
                            && candidateMemory->routeFailureCooldown > 0.0f
                            && candidateMemory->repeatedRouteFailures >= 2)
                        {
                            continue;
                        }
                        const BotRole role = candidateMemory != nullptr
                            ? candidateMemory->role : RoleForBotId(candidate.GetId());
                        // Rushers already know the authored lanes and usually
                        // carry the bridge stack needed to reach an exposed base.
                        const float rolePenalty = role == BotRole::Rusher ? 0.0f
                            : (role == BotRole::Fighter ? 180.0f
                                : (role == BotRole::Collector ? 2000.0f : 3000.0f));
                        const float alivePenalty = candidate.IsAlive() ? 0.0f : 500.0f;
                        const float bridgeKitPenalty = finalCleanupPhase
                            ? static_cast<float>(std::max(0,
                                36 - candidate.GetInventory().GetBlocks())) * 240.0f
                            : 0.0f;
                        const float score = rolePenalty + alivePenalty + bridgeKitPenalty
                            + std::sqrt(DistanceSquared(candidate.GetPosition(), cleanupTargetContext->coreHome))
                            + static_cast<float>(candidate.GetId()) * 0.001f;
                        if (score < bestHunterScore)
                        {
                            bestHunterScore = score;
                            bestHunterId = candidate.GetId();
                        }
                    }
                    coordBus->cleanupHunterId = bestHunterId;
                }

                for (const Player* target : cleanupTargetContext->aliveAllies)
                {
                    if (target == nullptr)
                    {
                        continue;
                    }
                    const bool witnessed = std::any_of(
                        teamContext.aliveAllies.begin(), teamContext.aliveAllies.end(),
                        [this, target](const Player* ally)
                        {
                            return ally != nullptr && BotHasLineOfSight(*ally, *target);
                        });
                    if (witnessed)
                    {
                        coordBus->cleanupLastKnownPosition = target->GetPosition();
                        coordBus->cleanupLastSeenTimestamp = matchSimulation_.MatchTimeSeconds();
                        break;
                    }
                }

                plan.cleanupHunterId = coordBus->cleanupHunterId;
                plan.cleanupTargetTeamId = cleanupTargetContext->teamId;
                plan.cleanupTargetAlivePlayers = static_cast<int>(cleanupTargetContext->aliveAllies.size());
                plan.cleanupTargetRecentlySeen = matchSimulation_.MatchTimeSeconds()
                    - coordBus->cleanupLastSeenTimestamp <= 12.0f;
                plan.cleanupTargetPosition = plan.cleanupTargetRecentlySeen
                    ? coordBus->cleanupLastKnownPosition : cleanupTargetContext->coreHome;
            }
        }
        else if (coordBus != nullptr)
        {
            coordBus->cleanupHunterId = -1;
            coordBus->cleanupTargetTeamId = -1;
            coordBus->cleanupLastKnownPosition = {};
            coordBus->cleanupLastSeenTimestamp = -1000.0f;
        }

        if (!team.coreAlive && bestCore == nullptr)
        {
            plan.focus = BotStrategicFocus::Cleanup;
            plan.desiredAttackers = 4;
            plan.attackUrgency = 520.0f;
            plan.allIn = true;
        }
        else if (teamContext.enemyAtCore != nullptr || plan.defenseUrgency >= 420.0f)
        {
            plan.focus = BotStrategicFocus::Defense;
        }
        else if (bestCore != nullptr && (matchSimulation_.MatchTimeSeconds() > teamTuning.pressurePhaseSeconds || plan.allIn || teamContext.teamPlan.activeResource >= 2))
        {
            plan.focus = BotStrategicFocus::Pressure;
        }
        else
        {
            plan.focus = BotStrategicFocus::Economy;
        }

        teamContext.strategicPlan = plan;
    }

    UpdateBotBridgeCoordination();
    for (std::size_t botOffset = 0; botOffset < players_.size(); ++botOffset)
    {
        Player& bot = players_[(simulationOrderOffset_ + botOffset) % players_.size()];
        if (!IsBotControlled(ControlKindForPlayer(bot)) || !bot.IsAlive() || bot.IsEliminated())
        {
            continue;
        }
        // A player driven by a remote network client is not AI-controlled — its
        // movement comes from the client's PlayerCommand (Phase 0.1S).
        const auto foundTeamContext = frameContext.teamContexts.find(bot.GetTeamId());
        if (foundTeamContext == frameContext.teamContexts.end() || foundTeamContext->second.team == nullptr)
        {
            continue;
        }

        UpdateSingleBot(bot, *foundTeamContext->second.team, dt, frameContext);
    }
}

void Game::UpdateSingleBot(Player& bot, Team& team, float dt, const BotFrameContext& frameContext)
{
    ScopedProfileTimer decisionProfile(profilingEnabled_, profileDecisionMs_, profileDecisionCalls_);
    BotMemory& memory = GetBotMemory(bot);
    TickBotMemory(memory, dt);
    // This is a frame-local assignment from the team bridge-request bus.
    // Re-evaluate it each decision frame instead of pinning a player forever.
    memory.assignedBridgeAssist = false;
    memory.carriedResourceValue = CarriedResourceValue(bot.GetInventory());
    if (memory.pendingVoidEscapeTimer > 0.0f)
    {
        memory.pendingVoidEscapeTimer = std::max(0.0f, memory.pendingVoidEscapeTimer - dt);
        if (bot.IsOnGround()
            && !IsVoidThreatAt(bot.GetPosition())
            && DistanceSquared(bot.GetPosition(), memory.pendingVoidEscapePosition) > 0.35f)
        {
            ++navigationMetrics_.emergencySaves;
            if (memory.carriedResourceValue >= 20 || bot.GetHealth() <= 40 || memory.repeatedRouteFailures > 0)
            {
                RecordMemorableMoment(
                    "VoidEscape", bot.GetTeamId(), -1, { bot.GetId() },
                    bot.GetName() + " recovered after an emergency block placement",
                    0.72f, true, bot.GetHealth(), -1, memory.carriedResourceValue,
                    { "detected falling", "placed through PlayerCommand", "regained support" });
            }
            memory.pendingVoidEscapeTimer = 0.0f;
        }
    }
    memory.hasObjectiveTarget = false;
    const BotTuningGenome botTuning = ScaledBotTuningForTeam(team.id);

    const auto foundTeamContext = frameContext.teamContexts.find(team.id);
    const BotTeamFrameContext* teamContext = foundTeamContext != frameContext.teamContexts.end()
        ? &foundTeamContext->second
        : nullptr;
    const std::vector<Player*>& rawEnemies = teamContext != nullptr
        ? teamContext->aliveEnemies
        : frameContext.alivePlayers;
    std::vector<Player*> perceivedEnemies;
    perceivedEnemies.reserve(rawEnemies.size());
    for (Player* enemy : rawEnemies)
    {
        if (enemy != nullptr && enemy->GetTeamId() != bot.GetTeamId()
            && (team.enemyTrackerUnlocked || BotHasLineOfSight(bot, *enemy)))
        {
            perceivedEnemies.push_back(enemy);
        }
    }
    const std::vector<Player*>& aliveEnemies = perceivedEnemies;
    const int coordinationBusIndex = team.id >= 0 && team.id < static_cast<int>(teamCoordBuses_.size())
        ? team.id
        : -1;
    TeamCoordinationBus* coordBus = coordinationBusIndex >= 0 ? &teamCoordBuses_[coordinationBusIndex] : nullptr;
    EnergyCore* enemyCore = SelectBestAttackTarget(bot, frameContext, coordBus);
    BotStrategicPlan strategicPlan = teamContext != nullptr ? teamContext->strategicPlan : BotStrategicPlan {};
    const bool assignedCleanupHunter = strategicPlan.cleanupHunterId == bot.GetId()
        && strategicPlan.cleanupTargetTeamId >= 0
        && strategicPlan.cleanupTargetAlivePlayers > 0;
    if (assignedCleanupHunter && !strategicPlan.cleanupTargetRecentlySeen && coordBus != nullptr)
    {
        const Team* cleanupTeam = FindTeam(strategicPlan.cleanupTargetTeamId);
        if (cleanupTeam != nullptr)
        {
            std::vector<BotSearchSite> sites;
            if (coordBus->cleanupLastSeenTimestamp > -999.0f
                && matchSimulation_.MatchTimeSeconds() - coordBus->cleanupLastSeenTimestamp < 45.0f)
                sites.push_back({ 0, coordBus->cleanupLastKnownPosition, 80.0f });
            sites.push_back({ 1, world_.GridToWorld(cleanupTeam->coreBlock), 40.0f });
            sites.push_back({ 2, cleanupTeam->spawnPoint, 35.0f });
            sites.push_back({ 3, cleanupTeam->shopPosition, 25.0f });
            for (int index = 0; index < static_cast<int>(routeGraph_.NodeCount()); ++index)
            {
                const CreativeRouteNode* node = routeGraph_.Node(index);
                if (node == nullptr || (node->teamId >= 0 && node->teamId != team.id)) continue;
                // These are authored public places, never coordinates read
                // from an unseen enemy. Include the different Castle floors.
                sites.push_back({ 10 + index, world_.GridToWorld(node->pos), 0.0f });
            }
            strategicPlan.cleanupTargetPosition = SelectBotSearchSite(sites,
                coordBus->searchMemory, bot.GetPosition(), cleanupTeam->id,
                coordBus->cleanupLastSeenTimestamp, matchSimulation_.MatchTimeSeconds());
        }
    }
    if (strategicPlan.attackCoreTeamId >= 0)
    {
        EnergyCore* plannedCore = FindCoreByTeam(strategicPlan.attackCoreTeamId);
        if (plannedCore != nullptr
            && plannedCore->IsAlive()
            && (enemyCore == nullptr
                || strategicPlan.focus == BotStrategicFocus::Pressure
                || strategicPlan.allIn))
        {
            enemyCore = plannedCore;
        }
    }
    if ((memory.currentPlan.goal == StrategicGoal::BridgePush || memory.currentPlan.goal == StrategicGoal::CoreAssault)
        && memory.currentPlan.targetTeamId >= 0)
    {
        EnergyCore* plannedCore = FindCoreByTeam(memory.currentPlan.targetTeamId);
        if (plannedCore != nullptr && plannedCore->IsAlive())
        {
            enemyCore = plannedCore;
        }
    }
    const Vector3 coreHome = teamContext != nullptr ? teamContext->coreHome : world_.GridToWorld(team.coreBlock);
    const Vector3 botPos = bot.GetPosition();
    const float distanceFromHome = DistanceSquared(bot.GetPosition(), coreHome);
    int openingRank = 0;
    for (const Player& candidate : players_)
    {
        if (IsBotControlled(ControlKindForPlayer(candidate))
            && candidate.GetTeamId() == bot.GetTeamId()
            && candidate.GetId() < bot.GetId())
        {
            ++openingRank;
        }
    }
    BotTeamSnapshot teamPlan = teamContext != nullptr
        ? teamContext->teamPlan
        : BuildBotTeamSnapshot(bot, coreHome, players_, botMemories_, botMemoryIndexByPlayerId_);
    if (teamContext != nullptr)
    {
        teamPlan.aliveAllies = std::max(0, teamPlan.aliveAllies - 1);
        const bool selfNearCore = DistanceSquared(bot.GetPosition(), coreHome) < 72.0f;
        if (selfNearCore)
        {
            teamPlan.alliesNearCore = std::max(0, teamPlan.alliesNearCore - 1);
            if (memory.role == BotRole::Defender)
            {
                teamPlan.defendersNearCore = std::max(0, teamPlan.defendersNearCore - 1);
            }
        }
        RemoveIntentFromSnapshot(teamPlan, memory.intent);
    }
    float enemyAtCoreDistance = teamContext != nullptr ? teamContext->enemyAtCoreDistance : std::numeric_limits<float>::max();
    Player* enemyAtCore = teamContext != nullptr ? teamContext->enemyAtCore : FindEnemyNearCore(bot, players_, coreHome, enemyAtCoreDistance);
    const Inventory& inventory = bot.GetInventory();
    const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_, botTuning);
    const bool wantsShop = ShouldBotShop(bot, inventory, team, memory, botDifficulty_, botTuning);
    if (teamContext != nullptr)
    {
        memory.coreDefenseCritical = teamContext->coreDefenseCritical;
        memory.missingDefenseBlocks = teamContext->missingDefenseBlocks;
    }
    else if (!team.coreAlive)
    {
        memory.coreDefenseCritical = false;
        memory.missingDefenseBlocks = 0;
        memory.defenseCheckTimer = 0.0f;
    }
    else if (memory.defenseCheckTimer <= 0.0f)
    {
        const CoreDefenseStatus status = InspectCoreDefense(world_, team, TeamDefenseCells(team));
        memory.coreDefenseCritical = status.critical;
        memory.missingDefenseBlocks = status.missingBlocks;
        memory.defenseCheckTimer = botDifficulty_ == BotDifficulty::Hard
            ? 0.85f
            : (botDifficulty_ == BotDifficulty::Easy ? 1.75f : 1.20f);
    }
    const bool hypixelRush = botStrategyProfile_ == BotStrategyProfile::HypixelRush;
    const bool coreDefenseCritical = team.coreAlive
        && memory.coreDefenseCritical
        && (!hypixelRush || enemyAtCore != nullptr || memory.recentCoreAttackTimer > 0.0f);
    const int missingDefenseBlocks = team.coreAlive ? memory.missingDefenseBlocks : 0;
    const bool coreNeedsRepair = team.coreAlive
        && ((teamContext != nullptr ? teamContext->coreNeedsRepair : missingDefenseBlocks > 0)
            || coreDefenseCritical);
    BotRoleDistribution roleDistribution = teamContext != nullptr
        ? teamContext->roleDistribution
        : BuildRoleDistributionForTeam(team.id, players_, botMemories_, botMemoryIndexByPlayerId_, bot.GetId());
    if (teamContext != nullptr)
    {
        RemoveRoleFromDistribution(roleDistribution, memory.role);
    }
    const BotRoleDecision roleDecision = EvaluateDynamicRoleDecision(
        bot,
        team,
        memory,
        roleDistribution,
        carryingLoot,
        wantsShop,
        enemyAtCore,
        enemyCore,
        matchSimulation_.MatchTimeSeconds(),
        inventory);
    if (TryApplyDynamicRoleDecision(memory, roleDecision, botDifficulty_, botTuning))
    {
        AddEventMessage(bot.GetName() + ": роль -> " + ToString(memory.role), GetTeamColor(team.color), 1.2f);
    }

    ScopedProfileTimer perceptionProfile(profilingEnabled_, profilePerceptionMs_, profilePerceptionCalls_);
    // requireLineOfSight gates ENGAGEMENT perception (the close-range target the
    // bot will react to): an enemy hidden behind a wall is not "seen", so the
    // bot stops shooting/meleeing through cover (audit #8). The long-range hunt
    // (objective navigation to break a stalemate) passes false — a bot still
    // walks toward a base it knows is there without a direct sightline.
    const auto findNearbyEnemyFromSnapshot = [&](float maxDistance, bool requireLineOfSight) -> Player*
    {
        Player* best = nullptr;
        float bestScore = std::numeric_limits<float>::max();
        const float maxDistanceSq = maxDistance * maxDistance;

        for (Player* enemy : aliveEnemies)
        {
            if (enemy == nullptr
                || enemy->GetId() == bot.GetId()
                || enemy->GetTeamId() == bot.GetTeamId()
                || !enemy->IsAlive()
                || enemy->IsEliminated())
            {
                continue;
            }

            const float distance = DistanceSquared(bot.GetPosition(), enemy->GetPosition());
            if (distance > maxDistanceSq)
            {
                continue;
            }
            if (requireLineOfSight && !BotHasLineOfSight(bot, *enemy))
            {
                continue;
            }

            float score = distance;
            score -= static_cast<float>(std::max(0, bot.GetHealth() - enemy->GetHealth())) * 0.95f;
            const Team* enemyTeam = FindTeam(enemy->GetTeamId());
            if (enemyTeam != nullptr && !enemyTeam->coreAlive)
            {
                score -= 420.0f;
                if (enemy->GetHealth() <= bot.GetHealth() + 12)
                {
                    score -= 180.0f;
                }
            }
            if (memory.role == BotRole::Defender)
            {
                score += DistanceSquared(enemy->GetPosition(), coreHome) * 0.24f;
            }
            if (memory.role == BotRole::Collector && enemy->GetHealth() > bot.GetHealth() + 12)
            {
                score += 24.0f;
            }
            if (score < bestScore)
            {
                bestScore = score;
                best = enemy;
            }
        }

        return best;
    };

    // Engagement and pursuit both need a sighting.  The purchased team tracker
    // is the one explicit rules-based exception; without it bots do not follow
    // authoritative positions through walls.
    Player* nearbyEnemy = findNearbyEnemyFromSnapshot(
        BotEngageRange(memory.role, botDifficulty_, botTuning)
            + PersonalityForBot(bot.GetId(), memory.personalitySeed).engageOffset, true);
    // Once no cores remain anywhere the whole map is the hunting ground:
    // corner bases sit farther apart than the normal hunt radius, and a bot
    // that cannot "see" anyone will idle at home until sudden death kills it.
    const bool allCoresGone = !team.coreAlive && (enemyCore == nullptr || !enemyCore->IsAlive());
    const float huntRange = allCoresGone
        ? 100000.0f
        : (matchSimulation_.MatchTimeSeconds() > 120.0f
            ? (botDifficulty_ == BotDifficulty::Hard ? 168.0f : 142.0f)
            : (botDifficulty_ == BotDifficulty::Hard ? 128.0f : 108.0f));
    const bool trackerKnowledge = team.enemyTrackerUnlocked;
    Player* huntEnemy = findNearbyEnemyFromSnapshot(huntRange, !trackerKnowledge);
    Player* finalLifeEnemy = nullptr;
    float finalLifeEnemyDistanceSq = std::numeric_limits<float>::max();
    for (Player* candidate : aliveEnemies)
    {
        if (candidate == nullptr
            || candidate->GetId() == bot.GetId()
            || candidate->GetTeamId() == bot.GetTeamId()
            || !candidate->IsAlive()
            || candidate->IsEliminated())
        {
            continue;
        }

        const Team* candidateTeam = FindTeam(candidate->GetTeamId());
        if (candidateTeam == nullptr || candidateTeam->coreAlive)
        {
            continue;
        }

        if (!trackerKnowledge && !BotHasLineOfSight(bot, *candidate))
        {
            continue;
        }
        const float distance = DistanceSquared(bot.GetPosition(), candidate->GetPosition());
        if (assignedCleanupHunter && candidate->GetTeamId() != strategicPlan.cleanupTargetTeamId)
        {
            continue;
        }
        if (matchSimulation_.MatchTimeSeconds() < 135.0f && !assignedCleanupHunter)
        {
            continue;
        }

        const float score = distance
            + static_cast<float>(candidate->GetHealth()) * 18.0f
            - static_cast<float>(std::max(0, bot.GetHealth() - candidate->GetHealth())) * 22.0f;
        if (score < finalLifeEnemyDistanceSq)
        {
            finalLifeEnemyDistanceSq = score;
            finalLifeEnemy = candidate;
        }
    }
    if (finalLifeEnemy != nullptr)
    {
        huntEnemy = finalLifeEnemy;
        const float finalDistance = std::sqrt(DistanceSquared(bot.GetPosition(), finalLifeEnemy->GetPosition()));
        if (nearbyEnemy == nullptr
            || finalDistance < (BotEngageRange(memory.role, botDifficulty_, botTuning)
                                + PersonalityForBot(bot.GetId(), memory.personalitySeed).engageOffset) * 1.8f)
        {
            nearbyEnemy = finalLifeEnemy;
        }
    }
    else if (assignedCleanupHunter
        && huntEnemy != nullptr
        && huntEnemy->GetTeamId() != strategicPlan.cleanupTargetTeamId)
    {
        // The cleaner may still defend itself through nearbyEnemy, but its
        // long-range pursuit remains locked to the exposed team it was assigned.
        huntEnemy = nullptr;
    }
    const int retreatHealth = BotRetreatHealth(memory.role, botDifficulty_, team.coreAlive, botTuning);
    const int fightHealth = BotFightHealth(memory.role, botDifficulty_, botTuning);
    const float rushTime = botDifficulty_ == BotDifficulty::Hard ? 38.0f : (botDifficulty_ == BotDifficulty::Easy ? 115.0f : 70.0f);

    Player* weakEnemy = nullptr;
    if (huntEnemy != nullptr
        && huntEnemy->GetHealth() <= bot.GetHealth() - (botDifficulty_ == BotDifficulty::Easy ? 34 : 18))
    {
        weakEnemy = huntEnemy;
    }

    // Stable reaction latency: noticing a new target starts one timer.  It is
    // not randomised every tick, so a delayed response is readable rather than
    // jitter.  Hard bots are quicker, never instantaneous.
    if (nearbyEnemy != nullptr && nearbyEnemy->GetId() != memory.rememberedEnemyId)
    {
        if (memory.pendingPerceptionEnemyId != nearbyEnemy->GetId())
        {
            memory.pendingPerceptionEnemyId = nearbyEnemy->GetId();
            const float baseDelay = botDifficulty_ == BotDifficulty::Hard ? 0.12f
                : (botDifficulty_ == BotDifficulty::Easy ? 0.48f : 0.25f);
            memory.perceptionAcquireTimer = baseDelay * (0.82f + memory.cautionTrait * 0.42f);
        }
        if (memory.perceptionAcquireTimer > 0.0f)
        {
            nearbyEnemy = nullptr;
        }
    }

    if (nearbyEnemy != nullptr)
    {
        const Vector3 previous = memory.lastSeenEnemyPosition;
        memory.lastSeenEnemyPosition = nearbyEnemy->GetPosition();
        memory.hasLastSeenEnemy = true;
        memory.lastSeenEnemyTimer = 4.0f; // fresh sighting via clear LOS
        memory.rememberedEnemyVelocity = Vector3 {
            memory.lastSeenEnemyPosition.x - previous.x,
            memory.lastSeenEnemyPosition.y - previous.y,
            memory.lastSeenEnemyPosition.z - previous.z };
        memory.rememberedEnemyId = nearbyEnemy->GetId();
        memory.pendingPerceptionEnemyId = -1;
        memory.enemyMemoryConfidence = 1.0f;
    }
    else if (enemyAtCore != nullptr)
    {
        memory.lastSeenEnemyPosition = enemyAtCore->GetPosition();
        memory.hasLastSeenEnemy = true;
        memory.lastSeenEnemyTimer = 4.0f;
        memory.rememberedEnemyId = enemyAtCore->GetId();
        memory.enemyMemoryConfidence = 1.0f;
        nearbyEnemy = enemyAtCore;
    }
    else if (memory.lastSeenEnemyTimer > 0.0f)
    {
        // No current sightline: the memory of where the enemy was decays and
        // then clears, so the firing perch only ever peeks at a recent, fair
        // sighting rather than an ancient one.
        memory.lastSeenEnemyTimer = std::max(0.0f, memory.lastSeenEnemyTimer - dt);
        if (memory.lastSeenEnemyTimer <= 0.0f)
        {
            memory.hasLastSeenEnemy = false;
        }
    }

    if (bot.GetHealth() < retreatHealth && team.coreAlive)
    {
        memory.retreatTimer = std::max(memory.retreatTimer, botDifficulty_ == BotDifficulty::Hard ? 1.25f : 2.1f);
    }
    const bool defenderAwayFromBase = memory.role == BotRole::Defender
        && team.coreAlive
        && distanceFromHome > 40.0f;
    const int desiredBlocks = BotDesiredBlocks(memory.role, botTuning);
    BotResourcePlan resourcePlan {};
    if (memory.resourcePlanTimer <= 0.0f)
    {
        int openingHomeCollectorId = std::numeric_limits<int>::max();
        for (const Player& candidate : players_)
        {
            if (IsBotControlled(ControlKindForPlayer(candidate))
                && candidate.IsAlive()
                && !candidate.IsEliminated()
                && candidate.GetTeamId() == bot.GetTeamId())
            {
                openingHomeCollectorId = std::min(openingHomeCollectorId, candidate.GetId());
            }
        }
        const bool allowHomePickup = matchSimulation_.MatchTimeSeconds() >= 60.0f
            || bot.GetId() == openingHomeCollectorId;
        // Farming spots already claimed by teammates: prefer to spread out
        // instead of stacking the whole team on one generator.
        std::vector<Vector3> teammateClaims;
        for (const Player& mate : players_)
        {
            if (mate.GetId() == bot.GetId()
                || mate.GetTeamId() != bot.GetTeamId()
                || !mate.IsAlive()
                || mate.IsEliminated()
                || !IsBotControlled(ControlKindForPlayer(mate)))
            {
                continue;
            }
            const BotMemory* mateMemory = FindBotMemoryByPlayerId(
                botMemories_, botMemoryIndexByPlayerId_, mate.GetId());
            if (mateMemory != nullptr && mateMemory->hasCachedResourceTarget)
            {
                teammateClaims.push_back(mateMemory->cachedResourceTarget);
            }
        }
        const ResourcePickup* bestPickup = ::FindBestPickupForBot(
            bot,
            memory.role,
            matchSimulation_.Pickups(),
            aliveEnemies,
            coreHome,
            arenaBiome_ == ArenaBiome::Ruins,
            allowHomePickup,
            &teammateClaims);
        resourcePlan = BuildBotResourcePlan(bot, bestPickup, matchSimulation_.Generators(), memory.role,
                                            arenaBiome_ == ArenaBiome::Ruins, &teammateClaims);
        // The generic plan deliberately favours neutral economy. That is wrong
        // for a rusher stranded part-way through an authored bridge: it needs
        // a deterministic local rearm point before it can safely contest mid.
        const bool rearmingAuthoredRoute = memory.role == BotRole::Rusher
            && memory.hasAuthoredRouteObjective
            && memory.authoredRouteIndex > 0
            && memory.authoredRouteIndex < memory.authoredRouteMarkerCount
            && inventory.GetBlocks() < 4;
        if (rearmingAuthoredRoute)
        {
            for (const Generator& generator : matchSimulation_.Generators())
            {
                if (generator.GetTeamId() == team.id && generator.GetType() == ResourceType::Iron)
                {
                    resourcePlan.target = ToVector3(generator.GetPosition());
                    resourcePlan.type = ResourceType::Iron;
                    resourcePlan.hasTarget = true;
                    resourcePlan.score = DistanceSquared(bot.GetPosition(), resourcePlan.target);
                    break;
                }
            }
        }
        memory.cachedResourceTarget = resourcePlan.target;
        memory.cachedResourceType = static_cast<int>(resourcePlan.type);
        memory.hasCachedResourceTarget = resourcePlan.hasTarget;
        memory.resourcePlanTimer = botDifficulty_ == BotDifficulty::Hard ? 0.12f : 0.24f;
    }
    else
    {
        resourcePlan.target = memory.cachedResourceTarget;
        resourcePlan.type = static_cast<ResourceType>(memory.cachedResourceType);
        resourcePlan.hasTarget = memory.hasCachedResourceTarget;
    }
    const Vector3 resourceTarget = resourcePlan.target;
    const bool hasResourceTarget = resourcePlan.hasTarget;
    const ResourceType resourceTargetType = resourcePlan.type;
    const float nearbyEnemyDistance = nearbyEnemy != nullptr ? std::sqrt(DistanceSquared(bot.GetPosition(), nearbyEnemy->GetPosition())) : 999.0f;
    const int tacticalEnemyCoreTeamId = enemyCore != nullptr ? enemyCore->GetTeamId() : -1;
    if (memory.tacticalCheckTimer <= 0.0f || memory.tacticalEnemyCoreTeamId != tacticalEnemyCoreTeamId)
    {
        memory.tacticalEnemyCoreTeamId = tacticalEnemyCoreTeamId;
        // Siege trigger radius: on the stock arena the core is touchable, but
        // on castle-style maps it hides in a niche behind walls — a 5-meter
        // gate meant bots reached the enemy base hundreds of times without
        // ever switching to BreakCoreDefense (0% "Взлом" in 30-minute runs).
        // ~13 m covers standing at the base perimeter; the executor walks to
        // the concrete wall cell and mines from melee range as before.
        memory.cachedCanBreakDefense = enemyCore != nullptr
            && enemyCore->IsAlive()
            && DistanceSquared(bot.GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 170.0f
            && FindCoreDefenseBlock(*enemyCore, bot).has_value();
        memory.cachedCoreCanUpgrade = team.coreAlive
            && memory.role == BotRole::Defender
            && FindUpgradeableCoreDefenseBlock(team, bot).has_value();
        memory.tacticalCheckTimer = botDifficulty_ == BotDifficulty::Hard ? 0.10f : 0.18f;
    }
    const bool canBreakDefense = memory.cachedCanBreakDefense
        && enemyCore != nullptr
        && enemyCore->IsAlive()
        && enemyCore->GetTeamId() == memory.tacticalEnemyCoreTeamId
        && DistanceSquared(bot.GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 190.0f;
    const bool closeToEnemyCore = enemyCore != nullptr
        && DistanceSquared(botPos, world_.GridToWorld(enemyCore->GetBlockPosition())) < 150.0f;
    // Once a rusher has opened part of an authored lane, do not demand the
    // original full bridge stack before it may resume. Long Castle crossings
    // naturally consume that stack in stages; requiring 12 again made the bot
    // abandon a half-built bridge and farm for the rest of the match.
    const bool authoredRouteInProgress = memory.role == BotRole::Rusher
        && memory.hasAuthoredRouteObjective
        && memory.authoredRouteIndex > 0
        && memory.authoredRouteIndex < memory.authoredRouteMarkerCount;
    const int bridgeBlockRequirement = memory.role == BotRole::Rusher
        ? (authoredRouteInProgress ? 4 : 12)
        : 18;
    const bool bridgeKitReady = inventory.GetBlocks() >= bridgeBlockRequirement
        || (closeToEnemyCore && inventory.GetBlocks() >= 3);
    const bool combatKitReady = inventory.GetToolLevel() > 0
        || inventory.GetSwordLevel() > 0
        || inventory.GetUtility(UtilityType::Fireball) > 0
        || (memory.role == BotRole::Rusher && matchSimulation_.MatchTimeSeconds() > 12.0f)
        || (memory.role == BotRole::Fighter && matchSimulation_.MatchTimeSeconds() > 24.0f)
        || matchSimulation_.MatchTimeSeconds() > rushTime;
    const bool readyToRush = bridgeKitReady && combatKitReady;
    const bool coreCanUpgrade = team.coreAlive
        && memory.role == BotRole::Defender
        && memory.cachedCoreCanUpgrade;
    const int aliveEnemyCores = static_cast<int>(std::count_if(
        matchSimulation_.Cores().begin(), matchSimulation_.Cores().end(),
        [&team](const EnergyCore& core)
        {
            return core.GetTeamId() != team.id && core.IsAlive();
        }));
    const Team* huntEnemyTeam = huntEnemy != nullptr ? FindTeam(huntEnemy->GetTeamId()) : nullptr;
    const bool huntEnemyOnFinalLife = huntEnemyTeam != nullptr && !huntEnemyTeam->coreAlive;
    int finalLifeTeamAlive = 0;
    if (huntEnemyOnFinalLife)
    {
        const auto foundHuntTeamContext = frameContext.teamContexts.find(huntEnemy->GetTeamId());
        if (foundHuntTeamContext != frameContext.teamContexts.end())
        {
            finalLifeTeamAlive = static_cast<int>(foundHuntTeamContext->second.aliveAllies.size());
        }
        else
        {
            for (const Player* candidate : frameContext.alivePlayers)
            {
                if (candidate != nullptr
                    && candidate->GetTeamId() == huntEnemy->GetTeamId()
                    && candidate->IsAlive()
                    && !candidate->IsEliminated())
                {
                    ++finalLifeTeamAlive;
                }
            }
        }
    }
    const float huntEnemyDistance = huntEnemy != nullptr ? std::sqrt(DistanceSquared(bot.GetPosition(), huntEnemy->GetPosition())) : 999.0f;
    const bool lastFinalLifeEnemy = huntEnemyOnFinalLife && finalLifeTeamAlive <= 1;
    const bool finalLifeTargetClose = huntEnemyOnFinalLife
        && (huntEnemyDistance < 92.0f
            || (matchSimulation_.MatchTimeSeconds() > 165.0f && huntEnemyDistance < 150.0f)
            || (lastFinalLifeEnemy && matchSimulation_.MatchTimeSeconds() > 145.0f)
            || matchSimulation_.MatchTimeSeconds() > 165.0f);
    const bool finalDuelPhase = huntEnemy != nullptr
        && (!team.coreAlive || aliveEnemyCores == 0 || assignedCleanupHunter);
    const bool defenderThreat = memory.role == BotRole::Defender
        && (enemyAtCore != nullptr || (nearbyEnemy != nullptr && distanceFromHome < 72.0f));
    const bool vulnerableEnemy = nearbyEnemy != nullptr
        && nearbyEnemy->GetHealth() <= bot.GetHealth() - (botDifficulty_ == BotDifficulty::Easy ? 34 : 18);
    const bool enemyBlockingObjective = nearbyEnemy != nullptr
        && enemyCore != nullptr
        && DistanceSquared(nearbyEnemy->GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 36.0f;
    const FightAssessment nearbyFightAssessment = nearbyEnemy != nullptr
        ? AssessFight(
            bot,
            *nearbyEnemy,
            [this](const Player& observer, const Player& actor) { return BotHasLineOfSight(observer, actor); },
            teamContext != nullptr ? &teamContext->aliveAllies : nullptr,
            &aliveEnemies,
            memory.role,
            botDifficulty_,
            botTuning,
            defenderThreat,
            huntEnemyOnFinalLife && nearbyEnemy == huntEnemy,
            enemyBlockingObjective)
        : FightAssessment {};
    if (nearbyFightAssessment.shouldRetreat && team.coreAlive)
    {
        memory.retreatTimer = std::max(memory.retreatTimer, botDifficulty_ == BotDifficulty::Hard ? 0.95f : 1.55f);
    }
    const bool retreating = memory.retreatTimer > 0.0f && team.coreAlive;
    // A stalled-fight ban means: stop seeking this brawl, play the objective.
    // Defending the base or finishing a final-life enemy always overrides it.
    const bool fightSeekAllowed = memory.chaseBanTimer <= 0.0f
        || defenderThreat
        || enemyBlockingObjective
        || (huntEnemyOnFinalLife && nearbyEnemy == huntEnemy);
    const bool shouldFightNearby = nearbyEnemy != nullptr
        && fightSeekAllowed
        && bot.GetHealth() > fightHealth
        && !nearbyFightAssessment.shouldRetreat
        && (nearbyFightAssessment.shouldFight
            || memory.role == BotRole::Fighter
            || defenderThreat
            || vulnerableEnemy
            || (huntEnemyOnFinalLife && nearbyEnemy == huntEnemy)
            || enemyBlockingObjective
            || (memory.role == BotRole::Rusher && nearbyEnemyDistance < 4.6f)
            || (memory.role == BotRole::Collector && nearbyEnemyDistance < 3.0f && bot.GetHealth() > 68));
    const bool cleanupOverEconomy = assignedCleanupHunter
        || (huntEnemyOnFinalLife
            && aliveEnemyCores == 0
            && finalLifeTargetClose
            && matchSimulation_.MatchTimeSeconds() > 155.0f);
    const bool shouldPressureCore = enemyCore != nullptr
        && enemyCore->IsAlive()
        && (memory.role == BotRole::Rusher || memory.role == BotRole::Fighter)
        && (readyToRush || (matchSimulation_.MatchTimeSeconds() > 95.0f && inventory.GetBlocks() >= 6))
        && !retreating
        && !cleanupOverEconomy;
    const int coordinatedAttackersOnTarget = coordBus != nullptr && enemyCore != nullptr
        ? coordBus->CountSignal(
            CoordinationSignal::AttackingCore,
            matchSimulation_.MatchTimeSeconds(),
            kCoordinationSignalTtl,
            enemyCore->GetTeamId(),
            bot.GetId())
        : 0;
    const int coordinatedDefenders = coordBus != nullptr
        ? coordBus->CountSignal(CoordinationSignal::DefendingCore, matchSimulation_.MatchTimeSeconds(), kCoordinationSignalTtl, -999, bot.GetId())
        : 0;
    const int coordinatedHelpCalls = coordBus != nullptr
        ? coordBus->CountSignal(CoordinationSignal::CallingForHelp, matchSimulation_.MatchTimeSeconds(), kCoordinationSignalTtl, -999, bot.GetId())
        : 0;
    const int currentPlanTargetAlivePlayers = memory.currentPlan.targetTeamId >= 0
        ? static_cast<int>(std::count_if(
            frameContext.alivePlayers.begin(), frameContext.alivePlayers.end(),
            [&memory](const Player* player)
            {
                return player != nullptr
                    && player->GetTeamId() == memory.currentPlan.targetTeamId
                    && player->IsAlive()
                    && !player->IsEliminated();
            }))
        : 0;

    const Vector3 shopTarget = team.shopPosition;
    const Vector3 spawnTarget = team.spawnPoint;

    if (!assignedCleanupHunter)
    {
        memory.cleanupBridgeKitReady = false;
    }
    else if (inventory.GetBlocks() >= 36)
    {
        memory.cleanupBridgeKitReady = true;
    }
    else if (inventory.GetBlocks() < 8)
    {
        memory.cleanupBridgeKitReady = false;
    }

    const bool assaultPlanActive = memory.currentPlan.goal == StrategicGoal::BridgePush
        || memory.currentPlan.goal == StrategicGoal::CoreAssault;
    const bool cleanupProgressActive = assignedCleanupHunter
        && memory.currentPlan.goal == StrategicGoal::HuntPlayers
        && memory.currentPlan.targetTeamId == strategicPlan.cleanupTargetTeamId;
    const bool trackedAssault = assaultPlanActive
        && enemyCore != nullptr && enemyCore->IsAlive();
    if (!trackedAssault && !cleanupProgressActive)
    {
        memory.assaultProgressTargetTeamId = -1;
        memory.assaultNoProgressTimer = 0.0f;
        memory.hasAssaultProgressSample = false;
    }
    else
    {
        const Vector3 progressTarget = trackedAssault
            ? world_.GridToWorld(enemyCore->GetBlockPosition())
            : strategicPlan.cleanupTargetPosition;
        const int progressTargetTeamId = trackedAssault
            ? enemyCore->GetTeamId() : strategicPlan.cleanupTargetTeamId;
        const float coreDistance = std::sqrt(DistanceSquared(
            bot.GetPosition(), progressTarget));
        const int blocksNow = inventory.GetBlocks();
        const int resourcesNow = inventory.GetResource(ResourceType::Iron)
            + inventory.GetResource(ResourceType::Gold) * 3
            + inventory.GetResource(ResourceType::Crystal) * 6;
        const bool newTarget = !memory.hasAssaultProgressSample
            || memory.assaultProgressTargetTeamId != progressTargetTeamId;
        const bool corridorAdvanced = memory.routeCorridorAdvances
            > memory.assaultLastCorridorAdvances;
        const bool coreDamaged = trackedAssault && memory.assaultLastCoreHealth >= 0
            && enemyCore->GetHealth() < memory.assaultLastCoreHealth;
        const bool inventoryChanged = memory.assaultLastBlocks >= 0
            && blocksNow != memory.assaultLastBlocks;
        const bool economyProgressed = memory.assaultLastResourceValue >= 0
            && resourcesNow != memory.assaultLastResourceValue;
        const bool rearmingCleanup = cleanupProgressActive
            && aliveEnemyCores == 0 && !memory.cleanupBridgeKitReady;
        const bool approached = !newTarget
            && coreDistance + 1.0f < memory.assaultLastDistance;
        if (newTarget || corridorAdvanced || coreDamaged || inventoryChanged
            || ((trackedAssault || rearmingCleanup) && economyProgressed) || approached)
        {
            memory.assaultNoProgressTimer = 0.0f;
            memory.assaultLastDistance = coreDistance;
        }
        else
        {
            memory.assaultNoProgressTimer += dt;
        }
        memory.assaultProgressTargetTeamId = progressTargetTeamId;
        memory.assaultLastCorridorAdvances = memory.routeCorridorAdvances;
        memory.assaultLastCoreHealth = trackedAssault ? enemyCore->GetHealth() : -1;
        memory.assaultLastBlocks = blocksNow;
        memory.assaultLastResourceValue = resourcesNow;
        memory.hasAssaultProgressSample = true;

        const float assaultStallLimit = matchSimulation_.MatchTimeSeconds() >= 180.0f
            ? 20.0f : 32.0f;
        if (memory.assaultNoProgressTimer > assaultStallLimit
            && !memory.routeCorridorWaitingForBuilder)
        {
            // A committed plan is allowed to outlive its estimate, but not to
            // repeat an unchanged route forever.  Feed a real lack of progress
            // into the existing route-failure interrupt and corridor avoidance.
            memory.lastRouteFailurePosition = bot.GetPosition();
            memory.hasLastRouteFailure = true;
            memory.repeatedRouteFailures = std::max(
                memory.repeatedRouteFailures, cleanupProgressActive ? 1 : 3);
            memory.routeFailureCooldown = std::max(memory.routeFailureCooldown, 10.0f);
            memory.routeCorridorNodes.clear();
            memory.routeCorridorTraversal.clear();
            memory.routeCorridorBridgeBlocks.clear();
            memory.hasRouteCorridorObjective = false;
            memory.usingRouteCorridor = false;
            memory.routeCorridorReplanCooldown = 0.0f;
            memory.routeCorridorWaitingForBuilder = false;
            if (coordBus != nullptr && coordBus->bridgeBuilderId == bot.GetId())
            {
                coordBus->bridgeBuilderId = -1;
                coordBus->bridgeReservationUntil = -1000.0f;
                coordBus->bridgeLastProgressTimestamp = -1000.0f;
                coordBus->bridgeLastProgressBlocks = -1;
            }
            if (automatch_.active)
            {
                const int recordedStalls = static_cast<int>(std::count_if(
                    automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
                    [cleanupProgressActive](const AutomatchTimelineEvent& event)
                    {
                        return event.type == (cleanupProgressActive ? "cleanupStall" : "assaultStall");
                    }));
                if (recordedStalls < 160)
                {
                    automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                        matchSimulation_.MatchTimeSeconds(),
                        cleanupProgressActive ? "cleanupStall" : "assaultStall",
                        bot.GetTeamId(), bot.GetTeamId(), bot.GetId(), progressTargetTeamId,
                        memory.routeCorridorIndex,
                        "no objective progress; corridor released pos="
                            + std::to_string(bot.GetPosition().x) + ","
                            + std::to_string(bot.GetPosition().y) + ","
                            + std::to_string(bot.GetPosition().z)
                            + " coreDistance=" + std::to_string(coreDistance)
                            + " corridorIndex=" + std::to_string(memory.routeCorridorIndex)
                    });
                }
            }
            memory.assaultNoProgressTimer = 0.0f;
            memory.hasAssaultProgressSample = false;
        }
    }
    perceptionProfile.Stop();
    ScopedProfileTimer planningProfile(profilingEnabled_, profilePlanningMs_, profilePlanningCalls_);
    const BotDecisionContext decisionContext {
        bot,
        team,
        inventory,
        memory,
        botTuning,
        botDifficulty_,
        matchSimulation_.MatchTimeSeconds(),
        nearbyEnemy,
        weakEnemy,
        huntEnemy,
        enemyAtCore,
        enemyCore,
        teamPlan,
        botPos,
        coreHome,
        resourceTarget,
        enemyAtCoreDistance,
        distanceFromHome,
        nearbyEnemyDistance,
        nearbyFightAssessment,
        retreatHealth,
        fightHealth,
        desiredBlocks,
        resourceTargetType,
        retreating,
        defenderAwayFromBase,
        carryingLoot,
        wantsShop,
        canBreakDefense,
        readyToRush,
        coreNeedsRepair,
        coreDefenseCritical,
        coreCanUpgrade,
        aliveEnemyCores,
        strategicPlan,
        finalDuelPhase,
        huntEnemyOnFinalLife,
        finalLifeTargetClose,
        shouldFightNearby,
        shouldPressureCore,
        hasResourceTarget,
        coordinatedAttackersOnTarget,
        coordinatedDefenders,
        coordinatedHelpCalls,
        missingDefenseBlocks,
        currentPlanTargetAlivePlayers
    };
    if (memory.strategicUpdateTimer <= 0.0f
        && AdvanceStrategicPlan(memory.currentPlan, decisionContext))
    {
        ++memory.planStageAdvances;
        memory.strategicUpdateTimer = StrategicPlanUpdateCadence(botDifficulty_, botTuning);
    }
    const bool idlePlanDue = memory.currentPlan.goal == StrategicGoal::Idle
        && memory.strategicUpdateTimer <= 0.0f;
    const EnergyCore* plannedAssaultCore = (memory.currentPlan.goal == StrategicGoal::CoreAssault
            || memory.currentPlan.goal == StrategicGoal::BridgePush)
        && memory.currentPlan.targetTeamId >= 0
        ? FindCoreByTeam(memory.currentPlan.targetTeamId)
        : nullptr;
    const bool assaultTargetDestroyed = (memory.currentPlan.goal == StrategicGoal::CoreAssault
            || memory.currentPlan.goal == StrategicGoal::BridgePush)
        && memory.currentPlan.targetTeamId >= 0
        && (plannedAssaultCore == nullptr || !plannedAssaultCore->IsAlive());
    const bool timeboxedPlanCompleted = StrategicPlanExpired(memory.currentPlan)
        && (memory.currentPlan.goal == StrategicGoal::EconomicPhase
            || memory.currentPlan.goal == StrategicGoal::BaseDefense
            || memory.currentPlan.goal == StrategicGoal::MidControl);
    const bool economyReadinessCompleted = memory.currentPlan.goal == StrategicGoal::EconomicPhase
        && readyToRush
        && strategicPlan.focus == BotStrategicFocus::Pressure
        && memory.currentPlan.elapsedTime >= memory.currentPlan.minimumCommitSeconds;
    const bool completedPlan = memory.currentPlan.goal != StrategicGoal::Idle
        && memory.currentPlan.stageCount > 1
        && (timeboxedPlanCompleted
            || economyReadinessCompleted
            || memory.currentPlan.stage >= memory.currentPlan.stageCount - 1
            || assaultTargetDestroyed)
        && (timeboxedPlanCompleted
            || economyReadinessCompleted
            || assaultTargetDestroyed
            || memory.currentPlan.elapsedTime >= memory.currentPlan.minimumCommitSeconds);
    const StrategicInterruptReason interruptReason = memory.currentPlan.goal != StrategicGoal::Idle
        ? StrategicPlanInterruptReason(memory.currentPlan, decisionContext)
        : StrategicInterruptReason::None;
    const bool interruptedPlan = interruptReason != StrategicInterruptReason::None;
    // Cadence controls when a plan may be reconsidered; it no longer destroys
    // and recreates the same global plan every 1-3 seconds.  Replanning needs a
    // real completion/cancel condition.
    if (idlePlanDue || completedPlan || interruptedPlan)
    {
        const StrategicPlan previousPlan = memory.currentPlan;
        if (previousPlan.goal != StrategicGoal::Idle)
        {
            if (completedPlan)
            {
                ++memory.planCompletions;
                memory.lastPlanOutcome = "completed";
            }
            else
            {
                ++memory.planCancellations;
                if (interruptReason == StrategicInterruptReason::Expired)
                {
                    ++memory.planExpiryCancellations;
                    memory.lastPlanOutcome = "expired";
                }
                else if ((previousPlan.goal == StrategicGoal::BridgePush
                        || previousPlan.goal == StrategicGoal::CoreAssault)
                    && memory.routeFailureCooldown > 0.0f
                    && memory.repeatedRouteFailures >= (previousPlan.committed ? 3 : 2))
                {
                    ++memory.planRouteFailureCancellations;
                    memory.lastPlanOutcome = "cancelled after repeated route failures";
                    // The cancellation consumes this evidence. Keep the failed
                    // position/cooldown for corridor avoidance, but require new
                    // failures before cancelling the replacement plan too.
                    memory.repeatedRouteFailures = 0;
                }
                else
                {
                    ++memory.planEvidenceCancellations;
                    memory.lastPlanOutcome = "cancelled by new evidence";
                    if (automatch_.active)
                    {
                        const int recordedEvidenceCancellations = static_cast<int>(std::count_if(
                            automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
                            [](const AutomatchTimelineEvent& event)
                            {
                                return event.type == "planEvidenceCancel";
                            }));
                        if (recordedEvidenceCancellations < 320)
                        {
                            automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                                matchSimulation_.MatchTimeSeconds(), "planEvidenceCancel",
                                bot.GetTeamId(), bot.GetTeamId(), bot.GetId(),
                                previousPlan.targetTeamId,
                                static_cast<int>(interruptReason),
                                std::string(ToString(interruptReason))
                                    + " goal=" + ToString(previousPlan.goal)
                                    + " elapsed=" + std::to_string(previousPlan.elapsedTime)
                                    + " min=" + std::to_string(previousPlan.minimumCommitSeconds)
                                    + " committed=" + std::to_string(previousPlan.committed ? 1 : 0)
                            });
                        }
                    }
                }
                if (previousPlan.goal == StrategicGoal::BridgePush || previousPlan.goal == StrategicGoal::CoreAssault)
                {
                    memory.abandonedPlanTargetTeamId = previousPlan.targetTeamId;
                    memory.abandonedPlanCooldown = 7.0f + memory.cautionTrait * 7.0f;
                }
            }
        }
        memory.currentPlan = EvaluateStrategicPlan(decisionContext);
        // Patient bots commit longer, impulsive ones re-evaluate sooner —
        // breaks team-wide synchronized plan flips.
        memory.currentPlan.plannedDuration *= PersonalityForBot(bot.GetId(), memory.personalitySeed).patience;
        memory.currentPlan.sequence = previousPlan.sequence + 1;
        if (memory.currentPlan.goal != StrategicGoal::Idle)
        {
            ++memory.planStarts;
        }
        memory.strategicUpdateTimer = StrategicPlanUpdateCadence(botDifficulty_, botTuning);
    }

    BotDecision decision = EvaluateBotDecision(decisionContext);
    const BotMacroDirective macroDirective = EvaluateAutonomousMacroDirective(decisionContext);
    if (macroDirective.active)
    {
        decision.intent = macroDirective.intent;
        decision.state = StateForIntent(macroDirective.intent);
        decision.target = macroDirective.target;
        decision.fightTarget = macroDirective.fightTarget;
        decision.reason = macroDirective.reason;
        decision.score += 520.0f;
    }
    const bool openingScout = BotOpeningPressureEligible(
        matchSimulation_.MatchTimeSeconds(), openingRank, memory.role == BotRole::Defender,
        distanceFromHome, memory.intent == BotIntent::PressureCore
            || memory.intent == BotIntent::BreakCoreDefense)
        && enemyAtCore == nullptr
        && !coreDefenseCritical
        && (!hypixelRush || inventory.GetBlocks() >= 12);
    if (openingScout)
    {
        decision.intent = BotIntent::PressureCore;
        decision.state = BotState::AttackCore;
        decision.target = enemyCore != nullptr
            ? world_.GridToWorld(enemyCore->GetBlockPosition())
            : Vector3 { 0.0f, botPos.y, 0.0f };
        decision.fightTarget = nullptr;
        decision.reason = "opening bridge pressure";
        decision.score = 2000.0f;
    }
    const float coordinationNow = matchSimulation_.MatchTimeSeconds();
    const bool bridgeRequestActive = coordBus != nullptr
        && coordBus->bridgeRequestorId >= 0
        && coordinationNow - coordBus->bridgeRequestTimestamp <= 7.0f
        && std::any_of(players_.begin(), players_.end(), [coordBus](const Player& player) {
            return player.GetId() == coordBus->bridgeRequestorId && player.IsAlive() && !player.IsEliminated();
        });
    if (bridgeRequestActive && coordBus->bridgeRequestorId != bot.GetId())
    {
        const bool assignedBuilderAlive = std::any_of(
            players_.begin(), players_.end(), [coordBus](const Player& candidate)
            {
                return candidate.GetId() == coordBus->bridgeRequestBuilderId
                    && candidate.IsAlive() && !candidate.IsEliminated();
            });
        const bool canTakeBridgeRequest = memory.role != BotRole::Defender
            && (coordBus->bridgeRequestBuilderId == bot.GetId() || coordBus->preferredBridgeHelperId == bot.GetId())
            && !coreDefenseCritical
            && decision.intent != BotIntent::RetreatHome
            && decision.intent != BotIntent::Recover
            && (nearbyEnemy == nullptr || nearbyEnemyDistance > 7.5f);
        const int previousHelper = coordBus->bridgeRequestBuilderId;
        if (canTakeBridgeRequest && coordBus->TryClaimBridgeRequest(bot.GetId(), botPos,
            inventory.GetBlocks(), coordinationNow, assignedBuilderAlive))
        {
            memory.assignedBridgeAssist = true;
            if (automatch_.active && previousHelper != bot.GetId()
                && std::count_if(automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
                    [](const AutomatchTimelineEvent& event) { return event.type == "bridgeHelpAssigned"; }) < 128)
                automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                    coordinationNow, "bridgeHelpAssigned", team.id, team.id, bot.GetId(),
                    coordBus->bridgeRequestorId, inventory.GetBlocks(), "helper assigned" });
            decision.intent = coordBus->bridgeRequestIntent;
            decision.state = BotState::Bridge;
            decision.target = coordBus->bridgeRequestTarget;
            decision.fightTarget = nullptr;
            decision.reason = "ally bridge request";
            decision.score += 900.0f;
            if ((decision.intent == BotIntent::PressureCore
                    || decision.intent == BotIntent::BreakCoreDefense)
                && coordBus->bridgeRequestTargetTeamId >= 0)
            {
                EnergyCore* requestedCore = FindCoreByTeam(coordBus->bridgeRequestTargetTeamId);
                if (requestedCore != nullptr && requestedCore->IsAlive())
                {
                    enemyCore = requestedCore;
                }
            }
        }
    }
    ApplyBotDecision(memory, decision, botDifficulty_, botTuning);
    if (memory.intent != BotIntent::FightEnemy
        && memory.intent != BotIntent::ChaseWeakEnemy
        && memory.state != BotState::Fight)
    {
        memory.objectiveTarget = decision.target;
        memory.hasObjectiveTarget = true;
    }
    planningProfile.Stop();
    if (coordBus != nullptr)
    {
        CoordinationSignal signal = CoordinationSignal::None;
        int coordinationTargetTeamId = -1;
        switch (decision.intent)
        {
        case BotIntent::PressureCore:
        case BotIntent::BreakCoreDefense:
            signal = CoordinationSignal::AttackingCore;
            coordinationTargetTeamId = enemyCore != nullptr ? enemyCore->GetTeamId() : -1;
            break;
        case BotIntent::DefendCore:
        case BotIntent::RepairCoreDefense:
            signal = enemyAtCore != nullptr || coreDefenseCritical
                ? CoordinationSignal::CallingForHelp
                : CoordinationSignal::DefendingCore;
            coordinationTargetTeamId = team.id;
            break;
        case BotIntent::RetreatHome:
            signal = CoordinationSignal::Retreating;
            coordinationTargetTeamId = team.id;
            break;
        case BotIntent::SecureResources:
            signal = resourceTargetType == ResourceType::Crystal ? CoordinationSignal::HoldingMid : CoordinationSignal::None;
            break;
        case BotIntent::GearUp:
        case BotIntent::FightEnemy:
        case BotIntent::ChaseWeakEnemy:
        case BotIntent::Recover:
            break;
        }

        coordBus->Broadcast(bot.GetId(), signal, decision.target, matchSimulation_.MatchTimeSeconds(), coordinationTargetTeamId);
    }

    if (memory.intent == BotIntent::RepairCoreDefense
        && DistanceSquared(botPos, coreHome) <= 100.0f
        && TryBotRepairCoreDefense(bot, team, dt))
    {
        return;
    }

    decisionProfile.Stop();
    ScopedProfileTimer movementProfile(profilingEnabled_, profileMovementMs_, profileMovementCalls_);
    Vector3 target = decision.target;
    if (!memory.assignedBridgeAssist && memory.intent == BotIntent::RetreatHome
        && memory.role != BotRole::Defender && bot.GetHealth() < 55)
    {
        target = spawnTarget;
    }
    const bool cleanupSearch = assignedCleanupHunter
        && (decision.fightTarget == nullptr
            || decision.fightTarget->GetTeamId() != strategicPlan.cleanupTargetTeamId);
    const bool rangedApproach = memory.intent == BotIntent::FightEnemy
        && (inventory.HasItem(ItemType::Bow) || inventory.HasItem(ItemType::Blaster)
            || inventory.HasItem(ItemType::SniperRifle));
    const bool pursuitNavigation = (memory.intent == BotIntent::ChaseWeakEnemy
            || memory.intent == BotIntent::FightEnemy)
        && decision.fightTarget != nullptr
        && DistanceSquared(botPos, decision.fightTarget->GetPosition()) > (rangedApproach ? 144.0f : 22.0f);
    const bool navigationEligible = memory.state != BotState::Fight || pursuitNavigation;
    const bool cleanupRoute = assignedCleanupHunter && (cleanupSearch || pursuitNavigation);
    const bool objectivePush = memory.intent == BotIntent::PressureCore
        || memory.intent == BotIntent::BreakCoreDefense
        || memory.state == BotState::AttackCore
        || memory.state == BotState::BreakDefense;
    if (!memory.assignedBridgeAssist && objectivePush && enemyCore != nullptr && enemyCore->IsAlive())
    {
        target = world_.GridToWorld(enemyCore->GetBlockPosition());
        Vector3 center { 0.0f, botPos.y, 0.0f };
        for (const Generator& generator : matchSimulation_.Generators())
        {
            if (generator.GetTeamId() == -1 && generator.GetType() == ResourceType::Crystal)
            {
                center = ToVector3(generator.GetPosition());
                break;
            }
        }
        // Route via mid only while mid is actually PROGRESS toward the target
        // core. Without the progress check a bot circling the mid-radius rim on
        // a large map flip-flops between "go to center" and "go to core" every
        // step across the boundary and orbits forever.
        if (routeGraph_.Empty() && DistanceSquared(botPos, center) > 130.0f
            && DistanceSquared(botPos, target) > 360.0f
            && DistanceSquared(center, target) + 380.0f < DistanceSquared(botPos, target))
        {
            target = center;
        }
        const float coreDistanceSq = DistanceSquared(
            botPos, world_.GridToWorld(enemyCore->GetBlockPosition()));
        const bool nearCoreGapOpportunity = inventory.GetBlocks() < 4
            && coreDistanceSq > 42.0f && coreDistanceSq <= 324.0f;
        if (inventory.GetBlocks() < 4 && coreDistanceSq > 42.0f
            && !nearCoreGapOpportunity)
        {
            // Restock — but only when the bot can actually afford something.
            // Parking a broke bot at the shop deadlocks it (it stands on the
            // shop marker, farms nothing, and never earns the money to leave).
            const bool canAffordRestock = inventory.GetResource(ResourceType::Iron) >= 8
                || inventory.GetResource(ResourceType::Gold) >= 4
                || inventory.GetResource(ResourceType::Crystal) >= 2;
            if (canAffordRestock)
            {
                target = shopTarget;
            }
            else
            {
                // Farm the nearest own generator until the wallet recovers.
                Vector3 farmTarget = coreHome;
                float bestFarmDistance = std::numeric_limits<float>::max();
                for (const Generator& generator : matchSimulation_.Generators())
                {
                    if (generator.GetTeamId() != bot.GetTeamId())
                    {
                        continue;
                    }
                    const Vector3 generatorPos = ToVector3(generator.GetPosition());
                    const float distance = DistanceSquared(botPos, generatorPos);
                    if (distance < bestFarmDistance)
                    {
                        bestFarmDistance = distance;
                        farmTarget = generatorPos;
                    }
                }
                target = farmTarget;
            }
        }
    }

    const bool outboundResourceRoute = memory.intent == BotIntent::SecureResources
        && !cleanupSearch
        && resourceTargetType == ResourceType::Crystal
        && DistanceSquared(coreHome, target) > 900.0f;
    // The graph describes map connectivity, not a role assignment. All long
    // trips use it, including defense, resupply, retreat and pursuit. Legacy
    // outward-only markers remain restricted to outbound tasks.
    const bool longTrip = DistanceSquared(botPos, target) > 1024.0f;
    const bool continuingTrip = memory.usingRouteCorridor
        && memory.hasRouteCorridorObjective
        && DistanceSquared(memory.routeCorridorObjective, target) <= 100.0f;
    memory.usingAuthoredRoute = false;
    memory.authoredRouteMarkerKind = -1;
    if (navigationEligible && (longTrip || continuingTrip))
    {
        target = ChooseBotRouteCorridorWaypoint(bot, target);
        if (routeGraph_.Empty() && (objectivePush || outboundResourceRoute || cleanupRoute))
            target = ChooseBotAuthoredWaypoint(bot, target);
    }
    else
    {
        memory.usingRouteCorridor = false;
        memory.routeEntryPending = false;
        memory.routeEntryRepair = false;
        memory.routeCorridorBridgeSegment = false;
        memory.routeCorridorWaitingForBuilder = false;
    }

    if (navigationEligible
        && !memory.usingAuthoredRoute
        && !memory.usingRouteCorridor
        && DistanceSquared(botPos, target) > 180.0f)
    {
        target = ChooseBotWaypoint(bot, target);
    }
    PlayerCommand actionNavigationCommand {};
    bool navigationGoalSatisfied = false;
    bool actionNavigationActive = false;
    if (navigationEligible)
    {
        // Preserve the semantic objective for ordinary action navigation, but
        // do not discard the two transformations that are authoritative: a
        // RouteGraph waypoint and an explicit restock when an outbound pusher
        // has no blocks. The old unconditional objectiveTarget caused a bot on
        // a disconnected island to ask A* for the enemy Core hundreds of times
        // instead of walking back to its generator.
        const float coreDistanceSq = enemyCore != nullptr
            ? DistanceSquared(botPos, world_.GridToWorld(enemyCore->GetBlockPosition()))
            : 0.0f;
        const bool nearCoreGapOpportunity = objectivePush
            && enemyCore != nullptr
            && inventory.GetBlocks() < 4
            && coreDistanceSq > 42.0f && coreDistanceSq <= 324.0f;
        const bool restockingObjectivePush = objectivePush
            && enemyCore != nullptr
            && inventory.GetBlocks() < 4
            && coreDistanceSq > 42.0f
            && !nearCoreGapOpportunity;
        const Vector3 navigationTarget = (memory.usingAuthoredRoute
                || memory.usingRouteCorridor
                || restockingObjectivePush || memory.assignedBridgeAssist)
            ? target
            : (pursuitNavigation ? decision.target
                : (memory.hasObjectiveTarget ? memory.objectiveTarget : target));
        actionNavigationActive = UpdateActionNavigation(
            bot,
            memory,
            navigationTarget,
            decision.fightTarget,
            (restockingObjectivePush || memory.assignedBridgeAssist) ? nullptr : enemyCore,
            dt,
            actionNavigationCommand,
            navigationGoalSatisfied);
        if (navigationGoalSatisfied)
        {
            memory.stuckTimer = 0.0f;
        }
    }
    if (navigationEligible && !actionNavigationActive)
    {
        target = ChooseBotPathWaypoint(bot, target, dt, frameContext);
    }

    BotMovementPlan movementPlan {};
    if (actionNavigationActive)
    {
        const float sinYaw = std::sin(actionNavigationCommand.aimYaw);
        const float cosYaw = std::cos(actionNavigationCommand.aimYaw);
        movementPlan.wish = Vector3 {
            sinYaw * actionNavigationCommand.moveForward + cosYaw * actionNavigationCommand.moveStrafe,
            0.0f,
            -cosYaw * actionNavigationCommand.moveForward + sinYaw * actionNavigationCommand.moveStrafe
        };
        movementPlan.aimDirection = Vector3 { sinYaw, 0.0f, -cosYaw };
        movementPlan.edgePressure = actionNavigationCommand.sneak;
    }
    else
    {
        movementPlan = BuildBotMovementPlan(
            bot,
            memory,
            botDifficulty_,
            matchSimulation_.MatchTimeSeconds(),
            coreHome,
            target,
            memory.state,
            memory.intent,
            decision.fightTarget,
            [this, objectivePush](Vector3 position)
            {
                if (objectivePush && HasSupportBelow(world_, position, 3))
                {
                    return false;
                }
                return IsVoidThreatAt(position);
            },
            [&bot, this](Vector3 bridgeTarget)
            {
                return TryBotBridgeBlock(bot, bridgeTarget);
            });
    }
    Vector3 wish = movementPlan.wish;
    Player* fightTarget = movementPlan.fightTarget;
    Vector3 aimDirection = movementPlan.aimDirection;
    const float fightDistance = movementPlan.fightDistance;
    const bool edgePressure = movementPlan.edgePressure;
    const Vector3 nextStep {
        botPos.x + wish.x * 0.95f,
        botPos.y,
        botPos.z + wish.z * 0.95f
    };

    BotTraversalPlan traversalPlan {};
    if (!actionNavigationActive)
    {
        traversalPlan = BuildBotTraversalPlan(
            bot,
            memory,
            botDifficulty_,
            fightTarget,
            fightDistance,
            nextStep,
            wish,
            dt,
            world_,
            [&bot, target, this](Vector3 breakWish, float delta)
            {
                return TryBotBreakBlockingBlock(bot, breakWish, target, delta);
            });
    }
    if (traversalPlan.consumedByMining)
    {
        return;
    }
    bool jump = traversalPlan.jump;

    // Firing perch (creative building): assaulting a base with a ranged weapon
    // but a defender just ducked behind the wall (recent fair sighting, no
    // current LOS) — tower up to peek over. Holds position and jumps while the
    // perch routine stacks blocks under the feet; once elevation restores the
    // sightline it stops and the normal ranged fire resumes. Easy bots skip it
    // to keep the difficulty gap.
    constexpr float kPerchSenseRangeSq = 14.0f * 14.0f;
    const Inventory& perchInventory = bot.GetInventory();
    const bool botHasRanged =
        perchInventory.HasItem(ItemType::Bow)
        || perchInventory.HasItem(ItemType::Blaster)
        || perchInventory.HasItem(ItemType::SniperRifle);
    const bool perchTriggerActive = !actionNavigationActive
        && botDifficulty_ != BotDifficulty::Easy
        && nearbyEnemy == nullptr
        && botHasRanged
        && memory.hasLastSeenEnemy
        && (memory.intent == BotIntent::PressureCore
            || memory.intent == BotIntent::BreakCoreDefense
            || memory.state == BotState::AttackCore)
        && DistanceSquared(bot.GetPosition(), memory.lastSeenEnemyPosition) < kPerchSenseRangeSq;
    bool perching = false;
    if (perchTriggerActive)
    {
        perching = TryBotBuildFiringPerch(bot, memory, memory.lastSeenEnemyPosition, dt);
    }
    else
    {
        memory.perchBlocksPlaced = 0; // reset only when the trigger is inactive
    }
    if (perching)
    {
        wish = Vector3 { 0.0f, 0.0f, 0.0f }; // hold position on the tower
        jump = true;
    }

    const bool sprint = actionNavigationActive
        ? actionNavigationCommand.sprint
        : (botDifficulty_ != BotDifficulty::Easy
        && Length2D(wish) > 0.2f
        && !edgePressure
        && (memory.state == BotState::AttackCore
            || memory.state == BotState::Fight
            || memory.state == BotState::Collect
            || memory.intent == BotIntent::GearUp
            || memory.intent == BotIntent::ChaseWeakEnemy));
    // Teammate separation: bots sharing a target (shop, generator, defense
    // spot) used to stand INSIDE each other in a perfectly mirrored stack.
    // When nearly idle and a teammate is closer than the bot's personal
    // spacing, drift apart — only onto supported ground, never off an edge.
    // NEVER while working (breaking/assaulting): standing still there is the
    // job, and the drift kept dragging siege groups off the blocks they mine.
    if (!actionNavigationActive
        && fightTarget == nullptr
        && !memory.hasBreakTarget
        && memory.state != BotState::AttackCore
        && memory.state != BotState::BreakDefense
        && Length2D(wish) < 0.35f)
    {
        const BotPersonality personality = PersonalityForBot(bot.GetId(), memory.personalitySeed);
        const Player* crowding = nullptr;
        float crowdingSq = personality.spacing * personality.spacing;
        for (const Player& mate : players_)
        {
            if (mate.GetId() == bot.GetId()
                || mate.GetTeamId() != bot.GetTeamId()
                || !mate.IsAlive()
                || mate.IsEliminated())
            {
                continue;
            }
            const float distSq = DistanceSquared(bot.GetPosition(), mate.GetPosition());
            if (distSq < crowdingSq)
            {
                crowdingSq = distSq;
                crowding = &mate;
            }
        }
        if (crowding != nullptr)
        {
            Vector3 away {
                bot.GetPosition().x - crowding->GetPosition().x, 0.0f,
                bot.GetPosition().z - crowding->GetPosition().z };
            if (Length2D(away) < 0.05f)
            {
                // Perfectly stacked: split along the personal flank side.
                away = Vector3 { aimDirection.z * static_cast<float>(personality.flankSign), 0.0f,
                                 -aimDirection.x * static_cast<float>(personality.flankSign) };
            }
            const Vector3 driftDir = Normalize2D(away);
            const Vector3 driftedPos {
                bot.GetPosition().x + driftDir.x * 0.9f, bot.GetPosition().y,
                bot.GetPosition().z + driftDir.z * 0.9f };
            const GridPos below = world_.WorldToGrid(Vector3 {
                driftedPos.x, bot.GetPosition().y - 1.2f, driftedPos.z });
            if (world_.IsSolid(below))
            {
                wish = Vector3 { wish.x + driftDir.x * 0.6f, wish.y, wish.z + driftDir.z * 0.6f };
            }
        }
    }

    const Vector3 beforeMove = bot.GetPosition();
    const bool groundedBeforeMove = bot.IsOnGround();
    PlayerCommand movementCommand = actionNavigationActive
        ? actionNavigationCommand
        : BuildBotMovementCommand(
            bot,
            matchSimulation_.CurrentTick(),
            wish,
            aimDirection,
            jump,
            sprint);
    // Weapon switching: the held slot reflects the weapon the bot is fighting
    // with this tick (ranged when zoning/kiting, else its best melee), so a
    // human opponent sees it wield the bow it is shooting — and the decision
    // rides the command like any real input. Bot combat/build don't key off the
    // slot (melee picks its own weapon, building auto-picks a block), so this is
    // safe; ApplyPlayerCommand copies it onto the player.
    if (!actionNavigationActive)
    {
        movementCommand.selectedSlot = BotHeldSlotForCombat(bot, movementPlan.rangedKite, fightTarget != nullptr);
        movementCommand.sneak = edgePressure;
    }
    ApplyPlayerCommand(bot, movementCommand, dt);
    if (actionNavigationActive && memory.assignedBridgeAssist
        && movementCommand.actionType == static_cast<int>(PlayerActionType::DropItem))
    {
        const PlayerActionResult result = ApplyPlayerEconomyCommand(bot, movementCommand);
        if (result.success && automatch_.active
            && std::count_if(automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
                [](const AutomatchTimelineEvent& event) { return event.type == "bridgeHelpSupplied"; }) < 128)
            automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                matchSimulation_.MatchTimeSeconds(), "bridgeHelpSupplied", bot.GetTeamId(), bot.GetTeamId(),
                bot.GetId(), -1, movementCommand.actionParamB, "helper dropped building stock; pickup not yet confirmed" });
    }
    if (automatch_.active
        && groundedBeforeMove
        && !bot.IsOnGround()
        && bot.GetVelocity().y < -0.35f
        && IsVoidThreatAt(bot.GetPosition()))
    {
        const int recordedDepartures = static_cast<int>(std::count_if(
            automatch_.currentTimeline.begin(), automatch_.currentTimeline.end(),
            [](const AutomatchTimelineEvent& event) { return event.type == "groundDeparture"; }));
        if (recordedDepartures < 240)
        {
            MovementType movementType = MovementType::Count;
            GridPos movementFrom {};
            GridPos movementTo {};
            const auto controller = botNavigationControllers_.find(bot.GetId());
            const PlannedMovement* activeMovement = controller != botNavigationControllers_.end()
                ? controller->second.Executor().CurrentMovement()
                : nullptr;
            if (activeMovement != nullptr)
            {
                movementType = activeMovement->type;
                movementFrom = activeMovement->from;
                movementTo = activeMovement->to;
            }
            const Vector3 position = bot.GetPosition();
            const Vector3 velocity = bot.GetVelocity();
            automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                matchSimulation_.MatchTimeSeconds(), "groundDeparture", bot.GetTeamId(),
                bot.GetTeamId(), bot.GetId(), bot.GetId(), static_cast<int>(movementType),
                std::string(ToString(movementType))
                    + " actor=" + std::to_string(position.x) + ","
                    + std::to_string(position.y) + "," + std::to_string(position.z)
                    + " velocity=" + std::to_string(velocity.x) + ","
                    + std::to_string(velocity.y) + "," + std::to_string(velocity.z)
                    + " action=" + std::to_string(movementFrom.x) + ","
                    + std::to_string(movementFrom.y) + "," + std::to_string(movementFrom.z)
                    + "->" + std::to_string(movementTo.x) + ","
                    + std::to_string(movementTo.y) + "," + std::to_string(movementTo.z)
                    + " nav=" + (actionNavigationActive ? "1" : "0")
                    + " sneak=" + (movementCommand.sneak ? "1" : "0")
                    + " jump=" + (movementCommand.jump ? "1" : "0")
                    + " state=" + std::to_string(static_cast<int>(memory.state))
                    + " intent=" + std::to_string(static_cast<int>(memory.intent))
            });
        }
    }
    ApplyStandingBlockEffects(bot, false);
    if (actionNavigationActive)
    {
        ApplyNetworkPlayerActions(bot, movementCommand, dt);
    }
    movementProfile.Stop();
    ScopedProfileTimer combatProfile(profilingEnabled_, profileCombatMs_, profileCombatCalls_);

    const float movedDistance = DistanceSquared(beforeMove, bot.GetPosition());
    UpdateBotStuckAfterMove(memory, wish, movedDistance, dt, bot.GetPosition());

    // Chase futility bookkeeping: pursuits that stop closing the distance get
    // banned for a while so the bot pivots to core assault or economy instead
    // of orbiting a runner (and falling into the void) for minutes.
    const bool suddenDeathBrawl = !team.coreAlive
        && (enemyCore == nullptr || !enemyCore->IsAlive());
    if (suddenDeathBrawl)
    {
        memory.chaseBanTimer = 0.0f;
        memory.fightStallTimer = 0.0f;
        memory.chaseStuckTimer = 0.0f;
    }
    else if (memory.intent == BotIntent::FightEnemy && fightTarget != nullptr)
    {
        // A fight that is not actually hurting anyone for a long stretch is a
        // stall (heal wars, knockback ping-pong). Walk away and play the map.
        if (memory.chaseTargetId != fightTarget->GetId())
        {
            memory.chaseTargetId = fightTarget->GetId();
            memory.fightTargetHealth = fightTarget->GetHealth();
            memory.fightStallTimer = 0.0f;
        }
        else if (fightTarget->GetHealth() < memory.fightTargetHealth - 4 || bot.GetHealth() < 30)
        {
            memory.fightTargetHealth = fightTarget->GetHealth();
            memory.fightStallTimer = std::max(0.0f, memory.fightStallTimer - 2.5f);
        }
        else
        {
            memory.fightStallTimer += dt;
        }
        if (memory.fightStallTimer > 11.0f)
        {
            memory.chaseBanTimer = std::max(memory.chaseBanTimer, 22.0f);
            memory.fightStallTimer = 0.0f;
            memory.chaseTargetId = -1;
        }
    }
    else if (memory.intent == BotIntent::ChaseWeakEnemy && fightTarget != nullptr)
    {
        const float chaseDistance = std::sqrt(DistanceSquared(bot.GetPosition(), fightTarget->GetPosition()));
        if (memory.chaseTargetId != fightTarget->GetId())
        {
            memory.chaseTargetId = fightTarget->GetId();
            memory.chaseStuckTimer = 0.0f;
            memory.chaseLastDistance = chaseDistance;
        }
        const float closingSpeed = dt > 0.0f ? (memory.chaseLastDistance - chaseDistance) / dt : 0.0f;
        memory.chaseLastDistance = chaseDistance;
        if (chaseDistance < 3.4f)
        {
            memory.chaseStuckTimer = std::max(0.0f, memory.chaseStuckTimer - dt * 2.0f);
        }
        else if (closingSpeed < 0.35f)
        {
            memory.chaseStuckTimer += dt;
            if (edgePressure && bot.GetInventory().GetBlocks() <= 0)
            {
                memory.chaseStuckTimer += dt * 2.0f;
            }
        }
        else
        {
            memory.chaseStuckTimer = std::max(0.0f, memory.chaseStuckTimer - dt * 0.6f);
        }

        if (memory.chaseStuckTimer > 8.0f)
        {
            memory.chaseBanTimer = enemyCore != nullptr && enemyCore->IsAlive() ? 26.0f : 11.0f;
            memory.chaseStuckTimer = 0.0f;
            memory.chaseTargetId = -1;
        }
    }
    else if (memory.chaseTargetId != -1)
    {
        memory.chaseTargetId = -1;
        memory.chaseStuckTimer = 0.0f;
        memory.fightStallTimer = 0.0f;
    }

    if ((memory.intent == BotIntent::GearUp || memory.state == BotState::Shop || wantsShop)
        && nearbyEnemy == nullptr)
    {
        BotTryShop(bot, team);
    }

    BotUseHeroAbility(bot, team, fightTarget, enemyCore);
    BotUseUtility(bot, team, fightTarget, enemyCore, dt);

    if (fightTarget != nullptr
        && memory.attackTimer <= 0.0f
        && memory.stateTimer >= BotFightReactionDelay(botDifficulty_)
        && BotHasLineOfSight(bot, *fightTarget))
    {
        const auto& hotbar = bot.GetInventory().GetHotbarSlots();
        int meleeSlot = -1;
        const bool preferAxe = memory.intent == BotIntent::DefendCore
            || fightTarget->HasShield()
            || fightTarget->GetInventory().GetArmorLevel() >= 2;
        const bool preferSpear = !preferAxe
            && (memory.cautionTrait > 0.62f
                || DistanceSquared(bot.GetPosition(), fightTarget->GetPosition()) > 6.0f);
        const ItemType preferences[] {
            preferAxe ? ItemType::Axe : (preferSpear ? ItemType::Spear : ItemType::Sword),
            ItemType::Sword, ItemType::Spear, ItemType::Axe };
        for (ItemType preferred : preferences)
        {
            for (int slot = 0; slot < kHotbarSlotCount; ++slot)
            {
                if (!hotbar[slot].IsEmpty() && hotbar[slot].type == preferred)
                {
                    meleeSlot = slot;
                    break;
                }
            }
            if (meleeSlot >= 0) break;
        }
        if (meleeSlot >= 0)
        {
            const Vector3 humanAimDirection = ApplyStableAimBias(
                aimDirection, memory.personalitySeed, fightTarget->GetId(),
                botDifficulty_, memory.cautionTrait);
            PlayerCommand attackCommand = BuildBotMovementCommand(
                bot, matchSimulation_.CurrentTick(), Vector3 {}, humanAimDirection, false, false);
            attackCommand.selectedSlot = meleeSlot;
            attackCommand.attackPressed = true;
            attackCommand.attackHeld = true;
            attackCommand.sprint = sprint;
            ApplyPlayerCommand(bot, attackCommand, 0.0f);
            const int beforeHealth = fightTarget->GetHealth();
            ApplyNetworkPlayerActions(bot, attackCommand, dt);
            memory.attackTimer = BotAttackDelay(botDifficulty_)
                * (fightTarget->GetHealth() < beforeHealth ? 1.0f : 0.85f);
        }
    }

    if (!actionNavigationActive && enemyCore != nullptr && enemyCore->IsAlive()
        && (memory.intent == BotIntent::PressureCore || memory.intent == BotIntent::BreakCoreDefense)
        && DistanceSquared(bot.GetPosition(), world_.GridToWorld(enemyCore->GetBlockPosition())) < 18.0f)
    {
        if (TryBotBreakCoreDefense(bot, *enemyCore, dt))
        {
            return;
        }
        if (HasBotCoreAccess(bot, *enemyCore))
        {
            const Vector3 corePos = world_.GridToWorld(enemyCore->GetBlockPosition());
            const Vector3 eye { bot.GetPosition().x, bot.GetPosition().y + 0.78f, bot.GetPosition().z };
            Vector3 direction { corePos.x - eye.x, corePos.y - eye.y, corePos.z - eye.z };
            const float length = Length(direction);
            if (length > 0.05f)
            {
                direction = Vector3Scale(direction, 1.0f / length);
                PlayerCommand command;
                command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
                command.tick = matchSimulation_.CurrentTick();
                command.selectedSlot = PickaxeSlot(bot);
                command.aimYaw = std::atan2(direction.x, -direction.z);
                command.aimPitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
                command.attackHeld = true;
                ApplyPlayerCommand(bot, command, 0.0f);
                ApplyNetworkPlayerActions(bot, command, dt);
                return;
            }
        }
    }
}

Vector3 Game::ChooseBotPathWaypoint(Player& bot, Vector3 finalTarget, float dt, const BotFrameContext& frameContext)
{
    ScopedProfileTimer profileTimer(profilingEnabled_, profilePathMs_, profilePathCalls_);
    BotMemory& memory = GetBotMemory(bot);
    const auto foundTeamContext = frameContext.teamContexts.find(bot.GetTeamId());
    const BotTeamFrameContext* teamContext = foundTeamContext != frameContext.teamContexts.end()
        ? &foundTeamContext->second
        : nullptr;
    const BotTuningGenome& botTuning = BotTuningForTeam(bot.GetTeamId());
    const Inventory& inventory = bot.GetInventory();
    const bool defendingCore = memory.role == BotRole::Defender
        && (memory.intent == BotIntent::DefendCore || memory.intent == BotIntent::RepairCoreDefense);
    const bool pushingObjective = memory.intent == BotIntent::PressureCore
        || memory.intent == BotIntent::BreakCoreDefense
        || memory.state == BotState::AttackCore
        || memory.state == BotState::BreakDefense;
    const bool retreating = memory.intent == BotIntent::RetreatHome;
    const bool carryingLoot = memory.carriedResourceValue >= BotLootReturnValue(memory.role, botDifficulty_, botTuning);
    const bool conservativeRoute = retreating || (memory.role == BotRole::Collector && carryingLoot);
    const float refreshCadence = defendingCore
        ? 0.14f
        : (pushingObjective ? 0.20f : (conservativeRoute ? 0.24f : 0.30f));

    memory.navTimer = std::max(0.0f, memory.navTimer - dt);
    memory.pathReplanCooldown = std::max(0.0f, memory.pathReplanCooldown - dt);
    if (memory.hasNavWaypoint
        && memory.navTimer > 0.0f
        && DistanceSquared(memory.navTarget, finalTarget) < 9.0f
        && DistanceSquared(bot.GetPosition(), memory.navWaypoint) > 1.2f)
    {
        return memory.navWaypoint;
    }
    // Near/at the waypoint the cache above is bypassed on purpose (the bot
    // needs the NEXT step), but never re-search more often than the cooldown:
    // an arrived/fighting/mining bot used to re-run the full A* EVERY tick.
    if (memory.hasNavWaypoint
        && memory.pathReplanCooldown > 0.0f
        && DistanceSquared(memory.navTarget, finalTarget) < 9.0f)
    {
        return memory.navWaypoint;
    }

    if (profilingEnabled_)
    {
        ++profilePathFull_;
        if (!memory.hasNavWaypoint)
        {
            ++profilePathMissNoCache_;
        }
        else if (DistanceSquared(memory.navTarget, finalTarget) >= 9.0f)
        {
            ++profilePathMissMoved_;
        }
        else if (memory.navTimer <= 0.0f)
        {
            ++profilePathMissTimer_;
        }
        else
        {
            ++profilePathMissNear_;
        }
    }

    // Global per-tick search budget: even with caching, bursts happen (match
    // start, mass respawn — every cache expires together). Over-budget bots
    // keep their raw target for a short beat and retry next ticks.
    if (pathSearchBudgetThisTick_ <= 0)
    {
        memory.navTarget = finalTarget;
        memory.navWaypoint = finalTarget;
        memory.hasNavWaypoint = true;
        memory.navTimer = 0.0f;
        memory.pathReplanCooldown = 0.05f;
        return finalTarget;
    }
    --pathSearchBudgetThisTick_;

    memory.hasNavWaypoint = false;
    memory.navTarget = finalTarget;
    // Per-bot phase jitter: all caches are born on the same match-start tick,
    // so a fixed cadence makes every bot replan on the SAME tick — one
    // synchronized 15-search burst every 0.3 s (a visible periodic hitch).
    memory.navTimer = refreshCadence + static_cast<float>(bot.GetId() % 8) * 0.023f;
    memory.pathReplanCooldown = 0.12f;
    // Negative cache: a failed/degenerate search must ALSO be remembered.
    // These exits used to leave hasNavWaypoint == false, so a bot whose target
    // was unreachable (enemy across the void, goal outside the search box)
    // repeated the FULL search every tick — 60 A* runs/sec per bot, the main
    // interactive frame-rate killer. Falling back to the raw target for one
    // cache window produces the same movement the caller got anyway.
    const auto finishWithoutPath = [&]() -> Vector3
    {
        memory.navWaypoint = finalTarget;
        memory.hasNavWaypoint = true;
        return finalTarget;
    };

    const auto findDuelEnemy = [&](float maxDistance) -> const Player*
    {
        const float maxDistanceSq = maxDistance * maxDistance;
        const Player* best = nullptr;
        float bestScore = std::numeric_limits<float>::max();
        const auto considerEnemy = [&](const Player& enemy)
        {
            if (enemy.GetTeamId() == bot.GetTeamId() || !enemy.IsAlive() || enemy.IsEliminated())
            {
                return;
            }
            if (!BotHasLineOfSight(bot, enemy))
            {
                return;
            }

            const float distance = DistanceSquared(bot.GetPosition(), enemy.GetPosition());
            if (distance <= maxDistanceSq && distance < bestScore)
            {
                bestScore = distance;
                best = &enemy;
            }
        };

        if (teamContext != nullptr)
        {
            for (const Player* enemy : teamContext->aliveEnemies)
            {
                if (enemy != nullptr)
                {
                    considerEnemy(*enemy);
                }
            }
        }
        else
        {
            for (const Player& enemy : players_)
            {
                considerEnemy(enemy);
            }
        }

        return best;
    };

    const Player* duelEnemy = findDuelEnemy(defendingCore ? 30.0f : 16.0f);
    const FightAssessment pathFightAssessment = duelEnemy != nullptr
        ? AssessFight(
            bot,
            *duelEnemy,
            [this](const Player& observer, const Player& actor) { return BotHasLineOfSight(observer, actor); },
            teamContext != nullptr ? &teamContext->aliveAllies : nullptr,
            teamContext != nullptr ? &teamContext->aliveEnemies : nullptr,
            memory.role,
            botDifficulty_,
            botTuning,
            defendingCore,
            false,
            pushingObjective)
        : FightAssessment {};
    const bool canTakeFight = defendingCore
        || duelEnemy == nullptr
        || pathFightAssessment.canWin
        || (pushingObjective && pathFightAssessment.powerMargin > -18.0f);
    const float botPower = BotCombatPowerScore(bot, ApplyCombatTuning(kPathCombatPowerWeights, botTuning));
    const int blockCount = inventory.GetBlocks();
    const int toolLevel = inventory.GetToolLevel();
    const bool canBridge = blockCount >= (memory.role == BotRole::Rusher ? 3 : (conservativeRoute ? 6 : 4));
    struct ThreatSample
    {
        Vector3 position {};
        float power = 0.0f;
    };
    std::vector<ThreatSample> threatSamples;
    threatSamples.reserve(teamContext != nullptr ? teamContext->aliveEnemies.size() : players_.size());
    const auto addThreatSample = [&](const Player& other)
    {
        if (other.GetTeamId() == bot.GetTeamId() || !other.IsAlive() || other.IsEliminated())
        {
            return;
        }
        if (!BotHasLineOfSight(bot, other))
        {
            return;
        }
        threatSamples.push_back(ThreatSample { other.GetPosition(), BotCombatPowerScore(other, ApplyCombatTuning(kPathCombatPowerWeights, botTuning)) });
    };
    if (teamContext != nullptr)
    {
        for (const Player* other : teamContext->aliveEnemies)
        {
            if (other != nullptr)
            {
                addThreatSample(*other);
            }
        }
    }
    else
    {
        for (const Player& other : players_)
        {
            addThreatSample(other);
        }
    }

    const GridPos start = FindSupportBelow(world_, bot.GetPosition(), pushingObjective ? 4 : 2);
    GridPos rawGoal = FindSupportBelow(world_, finalTarget, pushingObjective ? 3 : 1);
    const int pathRadius = defendingCore ? 56 : (pushingObjective ? 52 : (conservativeRoute ? 44 : 48));
    // Long-range navigation: on castle-sized maps the objective sits far
    // outside the search window, and a goal the window cannot even contain
    // used to fail every search — bots marched straight into their own base
    // wall forever.  Clamp the goal to the window edge along the bearing and
    // let each replan carry the route another window forward.
    bool longRangeGoal = false;
    {
        const int spanX = rawGoal.x - start.x;
        const int spanZ = rawGoal.z - start.z;
        const int span = std::max(std::abs(spanX), std::abs(spanZ));
        const int clampSpan = pathRadius - 6;
        if (span > clampSpan)
        {
            longRangeGoal = true;
            const float scale = static_cast<float>(clampSpan) / static_cast<float>(span);
            const Vector3 edgePoint {
                static_cast<float>(start.x) + static_cast<float>(spanX) * scale,
                bot.GetPosition().y + 6.0f,
                static_cast<float>(start.z) + static_cast<float>(spanZ) * scale
            };
            rawGoal = FindSupportBelow(world_, edgePoint, 26);
        }
    }
    const int maxExpansions = defendingCore ? 3600 : (pushingObjective ? 3200 : 2600);
    const float threatRange = defendingCore ? 11.0f : 8.2f;
    const float threatScaleBase = defendingCore
        ? 0.12f
        : (canTakeFight ? 0.55f : (memory.role == BotRole::Collector ? 1.45f : 1.0f));
    const float threatScale = conservativeRoute ? threatScaleBase * 1.35f : threatScaleBase;
    const float threatRangeSq = threatRange * threatRange;
    const int minX = start.x - pathRadius;
    const int maxX = start.x + pathRadius;
    const int minZ = start.z - pathRadius;
    const int maxZ = start.z + pathRadius;

    const auto inBounds = [&](const GridPos& pos)
    {
        return pos.x >= minX && pos.x <= maxX && pos.z >= minZ && pos.z <= maxZ && pos.y >= kBotPathMinY && pos.y <= kBotPathMaxY;
    };
    const auto hasAnyAnchor = [&](const GridPos& support)
    {
        const GridPos around[] {
            GridPos { support.x + 1, support.y, support.z },
            GridPos { support.x - 1, support.y, support.z },
            GridPos { support.x, support.y, support.z + 1 },
            GridPos { support.x, support.y, support.z - 1 },
            GridPos { support.x, support.y - 1, support.z }
        };
        for (const GridPos& probe : around)
        {
            if (world_.IsSolid(probe))
            {
                return true;
            }
        }
        return false;
    };
    const auto headClearanceCost = [&](const GridPos& support, float& digCost)
    {
        digCost = 0.0f;
        const GridPos headBlocks[] {
            GridPos { support.x, support.y + 1, support.z },
            GridPos { support.x, support.y + 2, support.z }
        };

        for (const GridPos& head : headBlocks)
        {
            if (world_.IsAir(head))
            {
                continue;
            }

            const Block* block = world_.GetBlock(head);
            if (block == nullptr
                || !block->breakable
                || !IsBreakableByPlayers(block->type)
                || (block->teamId == bot.GetTeamId() && block->teamId != -1)
                || toolLevel <= 0)
            {
                return false;
            }
            digCost += 2.0f - std::min(1.2f, static_cast<float>(toolLevel) * 0.35f);
        }
        return true;
    };
    // Planned bridge chains: consecutive air nodes are traversable as long as
    // the CHAIN starts at an anchored cell — the runtime bridge builder places
    // the blocks as the bot advances. Without chains the planner dead-ended at
    // every gap wider than one cell (custom maps use such gaps as chokepoints),
    // and bots parked at the rim forever. Depth-priced so real floor wins when
    // it exists; chains only run flat (bridging does not climb).
    constexpr int kMaxPlannedBridgeChain = 10;
    const auto nodeTravelCost = [&](const GridPos& support, int parentBridgeChain, int& outBridgeChain) -> std::optional<float>
    {
        outBridgeChain = 0;
        if (!inBounds(support))
        {
            return std::nullopt;
        }
        float digCost = 0.0f;
        if (!headClearanceCost(support, digCost))
        {
            return std::nullopt;
        }

        bool usesBridgeNode = false;
        float cost = 1.0f;
        if (world_.IsSolid(support))
        {
            const Block* supportBlock = world_.GetBlock(support);
            if (supportBlock != nullptr)
            {
                if (supportBlock->type == BlockType::LavaBlock)
                {
                    cost += 20.0f;
                }
                else if (supportBlock->type == BlockType::SpikeBlock)
                {
                    cost += 12.0f;
                }
                else if (supportBlock->type == BlockType::IceBlock && conservativeRoute)
                {
                    cost += 1.0f;
                }
            }
        }
        else if (world_.IsAir(support) && canBridge
                 && (hasAnyAnchor(support)
                     || (parentBridgeChain >= 1 && parentBridgeChain < kMaxPlannedBridgeChain)))
        {
            usesBridgeNode = true;
            outBridgeChain = hasAnyAnchor(support) ? 1 : parentBridgeChain + 1;
            cost += 2.6f + (memory.role == BotRole::Rusher ? 0.2f : 0.9f);
            cost += static_cast<float>(std::max(0, outBridgeChain - 1)) * 0.7f;
            if (conservativeRoute)
            {
                cost += 1.8f;
            }
        }
        else
        {
            return std::nullopt;
        }

        int exposure = 0;
        const GridPos around[] {
            GridPos { support.x + 1, support.y, support.z },
            GridPos { support.x - 1, support.y, support.z },
            GridPos { support.x, support.y, support.z + 1 },
            GridPos { support.x, support.y, support.z - 1 }
        };
        for (const GridPos& neighbor : around)
        {
            if (world_.IsAir(neighbor))
            {
                ++exposure;
            }
        }
        cost += static_cast<float>(exposure) * (usesBridgeNode ? 0.55f : 0.18f);

        const Vector3 nodePos = world_.GridToWorld(support);
        for (const ThreatSample& threat : threatSamples)
        {
            const float enemyDistanceSq = DistanceSquared(threat.position, nodePos);
            if (enemyDistanceSq > threatRangeSq)
            {
                continue;
            }

            const float enemyDistance = std::sqrt(std::max(0.0001f, enemyDistanceSq));
            const float influence = 1.0f - enemyDistance / threatRange;
            const float enemyDiff = std::max(0.0f, threat.power - botPower);
            cost += (7.0f + enemyDiff * 0.085f) * influence * threatScale;
            if (canTakeFight && !conservativeRoute)
            {
                cost -= 1.4f * influence;
            }
            if (defendingCore)
            {
                cost -= 1.0f * influence;
            }
        }

        const float centerDistance = std::sqrt(static_cast<float>(support.x * support.x + support.z * support.z));
        if (conservativeRoute && centerDistance < 32.0f)
        {
            cost += (32.0f - centerDistance) * 0.12f;
        }
        if (pushingObjective && memory.role == BotRole::Rusher && centerDistance < 30.0f)
        {
            cost -= (30.0f - centerDistance) * 0.06f;
        }

        return std::max(0.25f, cost + digCost);
    };

    int scratchChain = 0;
    GridPos goal = rawGoal;
    if (!nodeTravelCost(goal, 0, scratchChain).has_value())
    {
        int bestScore = std::numeric_limits<int>::max();
        bool found = false;
        for (int radius = 1; radius <= 3; ++radius)
        {
            for (int dx = -radius; dx <= radius; ++dx)
            {
                for (int dz = -radius; dz <= radius; ++dz)
                {
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        const GridPos candidate { rawGoal.x + dx, rawGoal.y + dy, rawGoal.z + dz };
                        if (!nodeTravelCost(candidate, 0, scratchChain).has_value())
                        {
                            continue;
                        }
                        const int score = std::abs(candidate.x - rawGoal.x)
                            + std::abs(candidate.z - rawGoal.z)
                            + std::abs(candidate.y - rawGoal.y) * 2;
                        if (score < bestScore)
                        {
                            bestScore = score;
                            goal = candidate;
                            found = true;
                        }
                    }
                }
            }
            if (found)
            {
                break;
            }
        }
    }

    const auto heuristic = [&](const GridPos& support)
    {
        const float horizontal = static_cast<float>(std::abs(support.x - goal.x) + std::abs(support.z - goal.z));
        const float vertical = static_cast<float>(std::abs(support.y - goal.y));
        return horizontal * (pushingObjective ? 0.98f : 1.05f) + vertical * 1.95f;
    };
    const auto goalScore = [&](const GridPos& support)
    {
        return std::abs(support.x - goal.x) + std::abs(support.z - goal.z) + std::abs(support.y - goal.y) * 2;
    };

    struct OpenNode
    {
        GridPos pos {};
        float g = 0.0f;
        float f = 0.0f;
    };
    const auto cmp = [](const OpenNode& a, const OpenNode& b)
    {
        return a.f > b.f;
    };
    static thread_local std::vector<OpenNode> open;
    static thread_local std::unordered_map<long long, float> bestCost;
    static thread_local std::unordered_map<long long, GridPos> parent;
    static thread_local std::vector<GridPos> path;
    static thread_local std::unordered_map<long long, bool> seenPath;
    static thread_local std::unordered_map<long long, int> bridgeChain;
    open.clear();
    bestCost.clear();
    parent.clear();
    path.clear();
    seenPath.clear();
    bridgeChain.clear();
    const std::size_t pathReserve = static_cast<std::size_t>(maxExpansions);
    const std::size_t mapReserve = pathReserve * 2U;
    open.reserve(pathReserve);
    path.reserve(pathReserve);
    if (bestCost.bucket_count() < mapReserve)
    {
        bestCost.reserve(mapReserve);
    }
    if (parent.bucket_count() < mapReserve)
    {
        parent.reserve(mapReserve);
    }
    if (seenPath.bucket_count() < pathReserve)
    {
        seenPath.reserve(pathReserve);
    }
    int startChain = 0;
    if (!nodeTravelCost(start, 0, startChain).has_value())
    {
        return finishWithoutPath();
    }
    // Weighted A*: inflating the heuristic trades path optimality (bots do not
    // need perfect routes) for a several-fold cut in node expansions — the
    // search runs 60x/sec across the roster and dominated the sim tick.
    constexpr float kHeuristicWeight = 1.35f;
    open.push_back(OpenNode { start, 0.0f, heuristic(start) * kHeuristicWeight });
    std::push_heap(open.begin(), open.end(), cmp);
    bestCost[PathKey(start.x, start.y, start.z)] = 0.0f;
    bridgeChain[PathKey(start.x, start.y, start.z)] = startChain;

    std::optional<GridPos> bestGoal;
    int bestGoalScore = goalScore(start);
    int expansions = 0;
    while (!open.empty() && expansions++ < maxExpansions)
    {
        std::pop_heap(open.begin(), open.end(), cmp);
        const OpenNode current = open.back();
        open.pop_back();
        const long long currentKey = PathKey(current.pos.x, current.pos.y, current.pos.z);
        const auto foundCurrent = bestCost.find(currentKey);
        if (foundCurrent == bestCost.end() || current.g > foundCurrent->second + 0.0001f)
        {
            continue;
        }

        const int currentGoalScore = goalScore(current.pos);
        if (!bestGoal.has_value() || currentGoalScore < bestGoalScore)
        {
            bestGoal = current.pos;
            bestGoalScore = currentGoalScore;
        }
        if (currentGoalScore <= 1)
        {
            bestGoal = current.pos;
            break;
        }

        const bool preferZFirst = bot.GetId() % 2 == 0;
        const GridPos offsets[] {
            preferZFirst ? GridPos { 0, 0, 1 } : GridPos { 1, 0, 0 },
            preferZFirst ? GridPos { 0, 0, -1 } : GridPos { -1, 0, 0 },
            preferZFirst ? GridPos { 1, 0, 0 } : GridPos { 0, 0, 1 },
            preferZFirst ? GridPos { -1, 0, 0 } : GridPos { 0, 0, -1 }
        };
        const int currentChain = bridgeChain[currentKey];
        for (const GridPos& offset : offsets)
        {
            // Drops down to three blocks are safe walk-offs the movement code
            // already performs; without them the planner could not leave any
            // raised structure (castle walls, the bots' own core dome) and
            // parked on top forever.
            for (int dy = -3; dy <= 1; ++dy)
            {
                const GridPos next { current.pos.x + offset.x, current.pos.y + dy, current.pos.z + offset.z };
                if (dy == -3 && !world_.IsAir(GridPos { next.x, next.y + 3, next.z }))
                {
                    // The fall column has to be clear back up to the ledge.
                    continue;
                }
                // Bridge chains only continue on the level: an unanchored air
                // node cannot be entered while climbing or dropping.
                int nextChain = 0;
                const std::optional<float> travelCost = nodeTravelCost(next, dy == 0 ? currentChain : 0, nextChain);
                if (!travelCost.has_value())
                {
                    continue;
                }

                const float verticalCost = dy > 0
                    ? 0.90f
                    : (dy == 0 ? 0.0f : 0.38f + 0.42f * static_cast<float>(-dy - 1));
                const float nextG = current.g + *travelCost + verticalCost;
                const long long key = PathKey(next.x, next.y, next.z);
                const auto found = bestCost.find(key);
                if (found != bestCost.end() && found->second <= nextG)
                {
                    continue;
                }

                bestCost[key] = nextG;
                parent[key] = current.pos;
                bridgeChain[key] = nextChain;
                open.push_back(OpenNode { next, nextG, nextG + heuristic(next) * kHeuristicWeight });
                std::push_heap(open.begin(), open.end(), cmp);
            }
        }
    }

    // A clamped long-range goal only marks a bearing; any node that made real
    // progress toward it is a usable waypoint (the next replan continues from
    // there).  Short-range goals deliberately keep the strict <=5 gate: an
    // experiment that accepted partial progress for them too (control seeds
    // 41001-41003) zeroed ALL core damage and raised strategic stall ~30% —
    // bots zigzag toward unreachable targets forever instead of failing fast
    // so the decision layer can pick a different plan.
    const int startGoalScore = goalScore(start);
    const bool acceptedProgress = bestGoal.has_value()
        && (longRangeGoal
            ? startGoalScore - bestGoalScore >= 8
            : bestGoalScore <= 5);
    if (!acceptedProgress)
    {
#ifdef DAIBED_NAV_TRACE
        if (pushingObjective)
        {
            static float lastNavTrace = -10.0f;
            const float now = matchSimulation_.MatchTimeSeconds();
            if (now - lastNavTrace > 2.0f)
            {
                lastNavTrace = now;
                std::printf("[nav-trace] t=%.1f bot=%d NOPATH long=%d start=(%d,%d,%d) goal=(%d,%d,%d) bestScore=%d expansions=%d\n",
                    now, bot.GetId(), longRangeGoal ? 1 : 0,
                    start.x, start.y, start.z, goal.x, goal.y, goal.z,
                    bestGoalScore, expansions);
            }
        }
#endif
        return finishWithoutPath();
    }

    GridPos step = *bestGoal;
    path.push_back(step);
    seenPath[PathKey(step.x, step.y, step.z)] = true;
    int reconstructGuard = 0;
    while (step != start)
    {
        if (++reconstructGuard > maxExpansions)
        {
            return finishWithoutPath();
        }
        const auto found = parent.find(PathKey(step.x, step.y, step.z));
        if (found == parent.end())
        {
            return finishWithoutPath();
        }
        step = found->second;
        const long long stepKey = PathKey(step.x, step.y, step.z);
        if (seenPath.find(stepKey) != seenPath.end() && step != start)
        {
            return finishWithoutPath();
        }
        seenPath[stepKey] = true;
        path.push_back(step);
    }
    std::reverse(path.begin(), path.end());

    if (path.size() < 2)
    {
        return finishWithoutPath();
    }

    const std::size_t maxLookAhead = defendingCore
        ? 4
        : (pushingObjective ? 4 : (conservativeRoute ? 2 : 3));
    const std::size_t lookAhead = std::min<std::size_t>(path.size() - 1, maxLookAhead);
    const GridPos nextStep = path[lookAhead];
    const Vector3 waypoint {
        static_cast<float>(nextStep.x),
        static_cast<float>(nextStep.y) + 1.40f,
        static_cast<float>(nextStep.z)
    };
    memory.navWaypoint = waypoint;
    memory.hasNavWaypoint = true;
#ifdef DAIBED_NAV_TRACE
    if (pushingObjective)
    {
        static float lastNavOkTrace = -10.0f;
        const float now = matchSimulation_.MatchTimeSeconds();
        if (now - lastNavOkTrace > 2.0f)
        {
            lastNavOkTrace = now;
            std::printf("[nav-trace] t=%.1f bot=%d PATH long=%d start=(%d,%d,%d) goal=(%d,%d,%d) len=%d wp=(%.0f,%.0f,%.0f)\n",
                now, bot.GetId(), longRangeGoal ? 1 : 0,
                start.x, start.y, start.z, goal.x, goal.y, goal.z,
                static_cast<int>(path.size()), waypoint.x, waypoint.y, waypoint.z);
        }
    }
#endif
    return waypoint;
}

Vector3 Game::ChooseBotWaypoint(const Player& bot, Vector3 finalTarget) const
{
    const Vector3 botPos = bot.GetPosition();
    const float finalDistance = DistanceSquared(botPos, finalTarget);
    Vector3 best = StepTargetToward(botPos, finalTarget, botDifficulty_ == BotDifficulty::Hard ? 14.0f : 11.0f);
    float bestScore = finalDistance;

    const auto consider = [&](Vector3 waypoint, float bias)
    {
        // Stock centre waypoints were historically hard-coded at y=1.5.
        // Imported maps can place the entire playable floor far above that
        // (Castle Bedwars uses y~53), so probe from the bot/goal elevation
        // instead of making FindSupportBelow search downward from the void.
        if (waypoint.y < 10.0f)
        {
            waypoint.y = std::max(botPos.y, finalTarget.y);
        }
        const float fromBot = DistanceSquared(botPos, waypoint);
        const float toFinal = DistanceSquared(waypoint, finalTarget);
        if (fromBot < 18.0f || fromBot > finalDistance + 0.01f || toFinal > finalDistance + 28.0f)
        {
            return;
        }

        const float score = fromBot * 0.72f + toFinal * 0.48f + bias;
        if (score < bestScore)
        {
            bestScore = score;
            best = waypoint;
        }
    };

    consider(Vector3 { 0.0f, botPos.y, 0.0f }, botDifficulty_ == BotDifficulty::Hard ? -70.0f : -40.0f);
    for (const Generator& generator : matchSimulation_.Generators())
    {
        if (generator.GetTeamId() != -1)
        {
            continue;
        }

        const ResourceType type = generator.GetType();
        const float bias = type == ResourceType::Crystal ? -95.0f : -42.0f;
        consider(ToVector3(generator.GetPosition()), bias);
    }

    return best;
}

Vector3 Game::ChooseBotAuthoredWaypoint(Player& bot, Vector3 finalTarget)
{
    BotMemory& memory = GetBotMemory(bot);
    memory.usingAuthoredRoute = false;
    memory.authoredRouteMarkerKind = -1;

    std::vector<const CreativeSpecial*> markers;
    for (const CreativeSpecial& special : creativeSpecials_)
    {
        if (special.teamId == bot.GetTeamId() && NavigationMarkerKindIndex(special.kind) >= 0)
        {
            markers.push_back(&special);
        }
    }
    if (markers.empty())
    {
        memory.authoredRouteMarkerCount = 0;
        return finalTarget;
    }

    const EnergyCore* ownCore = FindCoreByTeam(bot.GetTeamId());
    const Vector3 home = ownCore != nullptr
        ? world_.GridToWorld(ownCore->GetBlockPosition())
        : bot.GetHomeSpawnPoint();
    std::sort(markers.begin(), markers.end(), [&home, this](const CreativeSpecial* a, const CreativeSpecial* b)
    {
        const float da = DistanceSquared(home, world_.GridToWorld(a->pos));
        const float db = DistanceSquared(home, world_.GridToWorld(b->pos));
        if (std::fabs(da - db) > 0.01f)
        {
            return da < db;
        }
        return NavigationMarkerKindIndex(a->kind) < NavigationMarkerKindIndex(b->kind);
    });

    if (!memory.hasAuthoredRouteObjective)
    {
        memory.authoredRouteObjective = memory.hasObjectiveTarget ? memory.objectiveTarget : finalTarget;
        memory.hasAuthoredRouteObjective = true;
        memory.authoredRouteIndex = 0;
        memory.hasAuthoredRouteLastAdvancePosition = false;
        memory.hasNavWaypoint = false;
    }

    memory.authoredRouteMarkerCount = static_cast<int>(markers.size());
    memory.authoredRouteIndex = std::clamp(memory.authoredRouteIndex, 0, memory.authoredRouteMarkerCount);
    while (memory.authoredRouteIndex < memory.authoredRouteMarkerCount)
    {
        const CreativeSpecial& marker = *markers[static_cast<std::size_t>(memory.authoredRouteIndex)];
        const Vector3 markerPos = world_.GridToWorld(marker.pos);
        // A six-block reach radius lets a bridge bot hand off at a safe near
        // edge. Require a real movement between hand-offs so clustered markers
        // cannot all be consumed from that same edge.
        const bool reached = DistanceSquared(bot.GetPosition(), markerPos) <= 36.0f;
        const bool movedSinceLastAdvance = !memory.hasAuthoredRouteLastAdvancePosition
            || DistanceSquared(bot.GetPosition(), memory.authoredRouteLastAdvancePosition) >= 9.0f;
        if (!reached || !movedSinceLastAdvance)
        {
            break;
        }
        ++memory.authoredRouteIndex;
        ++memory.authoredRouteAdvances;
        memory.authoredRouteLastAdvancePosition = bot.GetPosition();
        memory.hasAuthoredRouteLastAdvancePosition = true;
        memory.hasNavWaypoint = false;
    }

    if (memory.authoredRouteIndex >= memory.authoredRouteMarkerCount)
    {
        return finalTarget;
    }

    const CreativeSpecial& marker = *markers[static_cast<std::size_t>(memory.authoredRouteIndex)];
    memory.usingAuthoredRoute = true;
    memory.authoredRouteMarkerKind = NavigationMarkerKindIndex(marker.kind);
    return world_.GridToWorld(marker.pos);
}

bool Game::TryBotPlaceCommand(Player& bot, const GridPos& pos, float dt, bool preferCheapBlock)
{
    const auto& hotbar = bot.GetInventory().GetHotbarSlots();
    int selectedSlot = -1;
    int selectedScore = std::numeric_limits<int>::max();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        if (hotbar[slot].IsEmpty()) continue;
        const std::optional<BlockType> type = ItemToBlock(hotbar[slot].type);
        if (!type.has_value() || !IsBuildableBlock(*type)) continue;
        int score = 20;
        if (preferCheapBlock)
        {
            if (*type == BlockType::WoolBlock) score = 0;
            else if (*type == BlockType::WoodBlock || *type == BlockType::PlankBlock) score = 3;
            else if (*type == BlockType::StoneBlock) score = 8;
            else if (*type == BlockType::ObsidianBlock) score = 50;
        }
        else
        {
            if (*type == BlockType::ObsidianBlock) score = 0;
            else if (*type == BlockType::StoneBlock) score = 3;
            else if (*type == BlockType::EnergyGlassBlock) score = 6;
            else if (*type == BlockType::WoodBlock || *type == BlockType::PlankBlock) score = 9;
            else if (*type == BlockType::WoolBlock) score = 12;
        }
        if (score < selectedScore)
        {
            selectedScore = score;
            selectedSlot = slot;
        }
    }
    if (selectedSlot < 0) return false;

    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
    command.tick = matchSimulation_.CurrentTick();
    command.selectedSlot = selectedSlot;
    command.placeHeld = true;
    command.placePressed = true;
    command.sneak = true;

    const Vector3 eye { bot.GetPosition().x, bot.GetPosition().y + 0.78f, bot.GetPosition().z };
    const GridPos anchors[] {
        GridPos { pos.x + 1, pos.y, pos.z }, GridPos { pos.x - 1, pos.y, pos.z },
        GridPos { pos.x, pos.y + 1, pos.z }, GridPos { pos.x, pos.y - 1, pos.z },
        GridPos { pos.x, pos.y, pos.z + 1 }, GridPos { pos.x, pos.y, pos.z - 1 }
    };
    bool aimed = false;
    for (const GridPos& anchor : anchors)
    {
        if (!world_.IsSolid(anchor)) continue;
        const Vector3 center = world_.GridToWorld(anchor);
        Vector3 direction { center.x - eye.x, center.y - eye.y, center.z - eye.z };
        const float distance = Length(direction);
        if (distance <= 0.05f || distance > 4.45f) continue;
        direction = Vector3Scale(direction, 1.0f / distance);
        const std::optional<RaycastHit> hit = RaycastFromPlayerEye(bot, direction, 4.5f);
        if (!hit.has_value() || hit->adjacent != pos) continue;
        command.aimYaw = std::atan2(direction.x, -direction.z);
        command.aimPitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
        aimed = true;
        break;
    }

    if (!aimed)
    {
        const Vector3 toCell {
            static_cast<float>(pos.x) - bot.GetPosition().x,
            0.0f,
            static_cast<float>(pos.z) - bot.GetPosition().z };
        if (Length2D(toCell) <= 0.01f) return false;
        command.aimYaw = YawFromDirection(toCell);
        command.aimPitch = -0.72f;
        return false;
    }

    ApplyPlayerCommand(bot, command, 0.0f);
    bool placed = false;
    ApplyNetworkBlockPlace(bot, command, dt, &placed);
    return placed;
}

bool Game::TryBotBridgeBlock(Player& bot, Vector3 target)
{
    if (bot.GetInventory().GetBlocks() <= 0)
    {
        return false;
    }

    const Vector3 botPos = bot.GetPosition();
    const Vector3 direction = Normalize2D(Vector3 { target.x - botPos.x, 0.0f, target.z - botPos.z });
    if (Length2D(direction) <= 0.0001f)
    {
        return false;
    }

    BotMemory& memory = GetBotMemory(bot);
    if (memory.reactionDelayTimer > 0.0f)
    {
        return false;
    }
    const GridPos underFeet = world_.WorldToGrid(Vector3 { botPos.x, botPos.y - 1.40f, botPos.z });
    const GridPos targetSupport = world_.WorldToGrid(Vector3 { target.x, target.y - 1.40f, target.z });
    const int bridgeY = std::min(underFeet.y, targetSupport.y);
    const bool inVoid = IsVoidThreatAt(botPos);
    if (inVoid
        && underFeet.y <= bridgeY
        && bot.GetVelocity().y <= 0.2f
        && world_.IsAir(underFeet)
        && TryBotPlaceCommand(bot, underFeet, 1.0f / 60.0f, true))
    {
        memory.bridgePlaceCooldown = BotBridgeCooldown(botDifficulty_);
        ++memory.bridgeBlocksPlaced;
        memory.pendingVoidEscapeTimer = 3.0f;
        memory.pendingVoidEscapePosition = bot.GetPosition();
        if (memory.carriedResourceValue >= 20 || bot.GetHealth() <= 40 || memory.repeatedRouteFailures > 0)
        {
            RecordMemorableMoment(
                "EmergencyBridge", bot.GetTeamId(), -1, { bot.GetId() },
                bot.GetName() + " placed a last-chance block while falling",
                0.62f, true, bot.GetHealth(), -1, memory.carriedResourceValue,
                { "lost support", "selected cheap bridge block", "issued place command" });
        }
        return true;
    }
    if (!bot.IsOnGround() && !inVoid)
    {
        return false;
    }
    if (memory.bridgePlaceCooldown > 0.0f)
    {
        return false;
    }

    const GridPos ahead = world_.WorldToGrid(Vector3 {
        botPos.x + direction.x * 1.05f,
        static_cast<float>(bridgeY),
        botPos.z + direction.z * 1.05f
    });
    const int stepX = direction.x > 0.18f ? 1 : (direction.x < -0.18f ? -1 : 0);
    const int stepZ = direction.z > 0.18f ? 1 : (direction.z < -0.18f ? -1 : 0);
    const float axisDifference = std::fabs(direction.x) - std::fabs(direction.z);
    const bool xDominant = axisDifference > 0.001f
        || (std::fabs(axisDifference) <= 0.001f && bot.GetId() % 2 != 0);
    const GridPos primary {
        underFeet.x + (xDominant ? stepX : 0),
        bridgeY,
        underFeet.z + (xDominant ? 0 : stepZ)
    };
    const GridPos secondary {
        underFeet.x + (xDominant ? 0 : stepX),
        bridgeY,
        underFeet.z + (xDominant ? stepZ : 0)
    };
    const GridPos sideStep {
        xDominant ? 0 : memory.strafeSign,
        0,
        xDominant ? memory.strafeSign : 0
    };
    const GridPos candidates[] {
        primary,
        secondary,
        ahead,
        GridPos { ahead.x + sideStep.x, ahead.y, ahead.z + sideStep.z },
        GridPos { ahead.x - sideStep.x, ahead.y, ahead.z - sideStep.z }
    };

    bool placedAhead = false;
    GridPos placedPos = ahead;
    for (const GridPos& candidate : candidates)
    {
        if (world_.IsAir(candidate) && TryBotPlaceCommand(bot, candidate, 1.0f / 60.0f, true))
        {
            placedAhead = true;
            placedPos = candidate;
            break;
        }
    }
    if (placedAhead)
    {
        memory.bridgePlaceCooldown = BotBridgeCooldown(botDifficulty_);
        ++memory.bridgeBlocksPlaced;
        const bool placeSafetyBlock = (botDifficulty_ == BotDifficulty::Hard
                && (memory.role == BotRole::Fighter || memory.role == BotRole::Defender || memory.bridgeBlocksPlaced % 5 == 0))
            || (botDifficulty_ == BotDifficulty::Normal && memory.bridgeBlocksPlaced % 7 == 0);
        if (placeSafetyBlock)
        {
            const GridPos side {
                placedPos.x + sideStep.x,
                placedPos.y,
                placedPos.z + sideStep.z
            };
            if (world_.IsAir(side) && TryBotPlaceCommand(bot, side, 1.0f / 60.0f, true))
            {
                ++memory.bridgeBlocksPlaced;
            }
        }
    }
    return placedAhead;
}

bool Game::TryBotBuildFiringPerch(Player& bot, BotMemory& memory, Vector3 sensedEnemyPos, float dt)
{
    if (memory.perchCooldown > 0.0f)
    {
        memory.perchCooldown = std::max(0.0f, memory.perchCooldown - dt);
    }
    if (bot.GetInventory().GetBlocks() <= 0)
    {
        memory.perchBlocksPlaced = 0;
        return false;
    }

    const Vector3 botPos = bot.GetPosition();
    // Already have a firing angle to the remembered spot from the current
    // (possibly already-raised) eye? Then the perch did its job — stop so the
    // normal LOS-gated perception + BotUseUtility can pick up the shot.
    const Vector3 eye { botPos.x, botPos.y + 0.78f, botPos.z };
    const Vector3 targetPoint { sensedEnemyPos.x, sensedEnemyPos.y + 0.9f, sensedEnemyPos.z };
    const Vector3 toTarget { targetPoint.x - eye.x, targetPoint.y - eye.y, targetPoint.z - eye.z };
    const float targetDist = Length(toTarget);
    if (targetDist > 0.5f
        && !world_.Raycast(eye, toTarget, targetDist - 0.3f).has_value())
    {
        memory.perchBlocksPlaced = 0;
        return false;
    }

    // Height cap comes from the blueprint spec (structure = data).
    if (memory.perchBlocksPlaced >= static_cast<int>(FiringPerchBlueprint().size()))
    {
        return false; // gave it a fair try; resume normal play
    }
    if (memory.reactionDelayTimer > 0.0f)
    {
        return true; // committed to perching, just reacting slowly
    }

    // Tower up: while airborne with air directly under the feet and the rise
    // has slowed, place a block under the feet — the same proven pattern the
    // bridge void-save uses. The caller drives the jump and holds position.
    if (memory.perchCooldown <= 0.0f)
    {
        const GridPos underFeet = world_.WorldToGrid(Vector3 { botPos.x, botPos.y - 1.40f, botPos.z });
        if (!bot.IsOnGround()
            && bot.GetVelocity().y <= 0.4f
            && world_.IsAir(underFeet)
            && TryBotPlaceCommand(bot, underFeet, dt, true))
        {
            ++memory.perchBlocksPlaced;
            memory.perchCooldown = 0.55f;
            if (memory.perchBlocksPlaced == 1)
            {
                AddEventMessage(bot.GetName() + " строит стрелковую вышку",
                                Color { 112, 232, 255, 255 }, 1.4f);
            }
        }
    }
    return true; // actively perching this tick (hold + jump)
}

bool Game::TryBotBreakCoreDefense(Player& bot, EnergyCore& core, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
    const std::optional<GridPos> defenseBlock = FindCoreDefenseBlock(core, bot);
    if (!defenseBlock.has_value())
    {
        memory.hasBreakTarget = false;
        memory.breakProgress = 0.0f;
        return false;
    }

    const GridPos target = *defenseBlock;
    const Vector3 targetPos = world_.GridToWorld(target);
    if (DistanceSquared(bot.GetPosition(), targetPos) > 10.0f)
    {
        return true;
    }

    const Block* block = world_.GetBlock(target);
    if (block == nullptr || !block->breakable || !IsBreakableByPlayers(block->type))
    {
        memory.hasBreakTarget = false;
        memory.breakProgress = 0.0f;
        return false;
    }

    memory.breakTarget = target;
    memory.hasBreakTarget = true;
    const Vector3 eye { bot.GetPosition().x, bot.GetPosition().y + 0.78f, bot.GetPosition().z };
    Vector3 direction { targetPos.x - eye.x, targetPos.y - eye.y, targetPos.z - eye.z };
    const float length = Length(direction);
    if (length <= 0.05f) return true;
    direction = Vector3Scale(direction, 1.0f / length);
    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
    command.tick = matchSimulation_.CurrentTick();
    command.selectedSlot = PickaxeSlot(bot);
    command.aimYaw = std::atan2(direction.x, -direction.z);
    command.aimPitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
    command.attackHeld = true;
    ApplyPlayerCommand(bot, command, 0.0f);
    ApplyNetworkPlayerActions(bot, command, dt);
    if (world_.IsAir(target))
    {
        memory.hasBreakTarget = false;
        memory.breakProgress = 0.0f;
    }
    return true;
}

std::optional<GridPos> Game::FindBotBlockingBlock(const Player& bot, Vector3 wish, Vector3 target) const
{
    const Vector3 direction = Normalize2D(wish);
    const Vector3 botPos = bot.GetPosition();
    const Vector3 eye { botPos.x, botPos.y + 0.68f, botPos.z };
    const Vector3 targetEye { target.x, target.y + 0.62f, target.z };
    const Vector3 toTarget {
        targetEye.x - eye.x,
        targetEye.y - eye.y,
        targetEye.z - eye.z
    };
    const float targetDistance = Length(toTarget);
    if (targetDistance > 1.2f)
    {
        const std::optional<RaycastHit> hit = world_.Raycast(eye, toTarget, std::min(5.6f, targetDistance));
        if (hit.has_value())
        {
            const Block* block = world_.GetBlock(hit->block);
            if (block != nullptr
                && block->breakable
                && block->teamId != bot.GetTeamId()
                && IsBreakableByPlayers(block->type))
            {
                return hit->block;
            }
        }
    }

    if (Length2D(direction) <= 0.0001f)
    {
        return std::nullopt;
    }

    const float distances[] { 0.72f, 1.05f, 1.34f, 1.72f, 2.05f };
    const float heights[] { -0.42f, 0.12f, 0.62f, 1.02f };
    std::optional<GridPos> best;
    float bestScore = std::numeric_limits<float>::max();

    for (float distance : distances)
    {
        for (float height : heights)
        {
            const GridPos pos = world_.WorldToGrid(Vector3 {
                botPos.x + direction.x * distance,
                botPos.y + height,
                botPos.z + direction.z * distance
            });
            const Block* block = world_.GetBlock(pos);
            if (block == nullptr || !block->breakable || block->teamId == bot.GetTeamId() || !IsBreakableByPlayers(block->type))
            {
                continue;
            }

            const float score = DistanceSquared(botPos, world_.GridToWorld(pos))
                + BreakSeconds(block->type, bot.GetInventory().GetToolLevel()) * 0.35f;
            if (score < bestScore)
            {
                bestScore = score;
                best = pos;
            }
        }
    }

    return best;
}

bool Game::TryBotBreakBlockingBlock(Player& bot, Vector3 wish, Vector3 targetPosition, float dt)
{
    BotMemory& memory = GetBotMemory(bot);
    const std::optional<GridPos> blockingBlock = FindBotBlockingBlock(bot, wish, targetPosition);
    if (!blockingBlock.has_value())
    {
        if (memory.hasBreakTarget)
        {
            const Block* block = world_.GetBlock(memory.breakTarget);
            if (block == nullptr || !block->breakable || !IsBreakableByPlayers(block->type))
            {
                memory.hasBreakTarget = false;
                memory.breakProgress = 0.0f;
            }
        }
        return false;
    }

    const GridPos target = *blockingBlock;
    const Block* block = world_.GetBlock(target);
    if (block == nullptr || !block->breakable || block->teamId == bot.GetTeamId() || !IsBreakableByPlayers(block->type))
    {
        return false;
    }

    memory.breakTarget = target;
    memory.hasBreakTarget = true;
    const Vector3 targetPos = world_.GridToWorld(target);
    const Vector3 eye { bot.GetPosition().x, bot.GetPosition().y + 0.78f, bot.GetPosition().z };
    Vector3 direction { targetPos.x - eye.x, targetPos.y - eye.y, targetPos.z - eye.z };
    const float length = Length(direction);
    if (length <= 0.05f) return true;
    direction = Vector3Scale(direction, 1.0f / length);
    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
    command.tick = matchSimulation_.CurrentTick();
    command.selectedSlot = PickaxeSlot(bot);
    command.aimYaw = std::atan2(direction.x, -direction.z);
    command.aimPitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
    command.attackHeld = true;
    ApplyPlayerCommand(bot, command, 0.0f);
    ApplyNetworkPlayerActions(bot, command, dt);
    if (world_.IsAir(target))
    {
        memory.hasBreakTarget = false;
        memory.breakProgress = 0.0f;
        memory.stuckTimer = 0.0f;
    }
    return true;
}

void Game::BotTryShop(Player& bot, Team& team)
{
    if (!shop_.IsPlayerInShop(bot, team))
    {
        return;
    }

    BotMemory& memory = GetBotMemory(bot);
    const Inventory& inventory = bot.GetInventory();
    struct ScoredShopItem
    {
        const ShopItem* item = nullptr;
        float score = 0.0f;
    };
    const int desiredBridgeBlocks = memory.role == BotRole::Rusher ? 48 : 28;
    const bool lowBlocks = inventory.GetBlocks() < desiredBridgeBlocks;
    const bool defenseStockLow = inventory.GetBlocks() < 32;
    const EnergyCore* ownCore = FindCoreByTeam(team.id);
    const bool coreDamaged = ownCore != nullptr && ownCore->IsAlive()
        && ownCore->GetHealth() < ownCore->GetMaxHealth() - 24;
    std::vector<ScoredShopItem> candidates;
    for (const ShopItem& item : shop_.GetItems())
    {
        if (!shop_.CanAfford(inventory, item) || item.aiTags == 0)
        {
            continue;
        }
        const auto hasTag = [&item](ShopItemTags tag) { return (item.aiTags & tag) != 0; };
        float value = 0.0f;
        if (hasTag(ShopTagBridge)) value += lowBlocks ? 120.0f : 0.0f;
        if (hasTag(ShopTagDefense)) value += (coreDamaged || (memory.role == BotRole::Defender && defenseStockLow))
            ? (memory.role == BotRole::Defender ? 80.0f : 42.0f) : 0.0f;
        if (hasTag(ShopTagBreach)) value += (memory.role == BotRole::Rusher || memory.role == BotRole::Fighter) ? 58.0f : 8.0f;
        if (hasTag(ShopTagMelee)) value += memory.role == BotRole::Fighter ? 42.0f : 18.0f;
        if (hasTag(ShopTagRanged)) value += memory.role == BotRole::Fighter ? 34.0f : 14.0f;
        if (hasTag(ShopTagMobility)) value += memory.role == BotRole::Rusher ? 30.0f : 12.0f;
        if (hasTag(ShopTagSustain)) value += bot.GetHealth() < 55 ? 92.0f : 8.0f;
        if (hasTag(ShopTagTeam)) value += memory.role == BotRole::Collector ? 64.0f : 6.0f;
        if (hasTag(ShopTagUpgrade)) value += 9.0f;
        if (item.usageMode == ShopUsageMode::TimedEffect
            && ((hasTag(ShopTagMobility) && bot.GetSpeedBoostTimer() > 0.0f)
                || (hasTag(ShopTagSustain) && bot.HasShield())))
        {
            value = 0.0f;
        }
        if (value <= 0.0f)
        {
            continue;
        }
        const auto resourceCost = [](ResourceType type, int amount)
        {
            const float weight = type == ResourceType::Iron ? 1.0f
                : (type == ResourceType::Gold ? 2.0f : 4.0f);
            return static_cast<float>(amount) * weight;
        };
        const float cost = resourceCost(item.primaryType, item.primaryCost)
            + (item.hasSecondaryCost ? resourceCost(item.secondaryType, item.secondaryCost) : 0.0f);
        candidates.push_back(ScoredShopItem { &item, value / std::max(1.0f, cost) });
    }
    std::sort(candidates.begin(), candidates.end(), [](const ScoredShopItem& a, const ScoredShopItem& b)
    {
        return a.score != b.score ? a.score > b.score : a.item->choice < b.item->choice;
    });
    int attempt = 0;
    for (const ScoredShopItem& candidate : candidates)
    {
        PlayerCommand command;
        command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
        command.tick = matchSimulation_.CurrentTick();
        command.actionSeq = command.tick * 32u + static_cast<std::uint32_t>(++attempt);
        command.actionType = static_cast<int>(PlayerActionType::BuyItem);
        command.actionParamA = candidate.item->choice;
        command.actionParamB = 1;
        const PlayerActionResult result = ApplyPlayerEconomyCommand(bot, command);
        if (!result.success)
        {
            continue;
        }
        if (automatch_.active)
        {
            const int category = candidate.item->choice < 100 ? 0
                : (candidate.item->choice < 200 ? 1 : (candidate.item->choice < 300 ? 2 : (candidate.item->choice < 400 ? 3 : 4)));
            ++automatch_.purchasesByCategory[category];
        }
        PushPlayerActionResultSnapshot(result);
        AddEventMessage(bot.GetName() + ": " + result.message, GetTeamColor(team.color), 1.6f);
        return;
    }

    // Fallback below retains the old authored priority list only when every
    // tagged candidate was rejected by a contextual shop rule (for example a
    // maxed team upgrade). New items participate through metadata above.
    std::vector<int> priorities;
    if (bot.GetHealth() < 55)
    {
        priorities.push_back(203);
        priorities.push_back(202);
    }
    if (inventory.GetBlocks() < 8)
    {
        priorities.push_back(2);
    }

    switch (memory.role)
    {
    case BotRole::Defender:
        if (EnergyCore* core = FindCoreByTeam(team.id); core != nullptr && core->IsAlive() && core->GetHealth() < core->GetMaxHealth() - 24)
        {
            priorities.push_back(303);
        }
        if (inventory.GetBlocks() < 20)
        {
            priorities.push_back(2);
        }
        if (inventory.GetBlockCount(BlockType::StoneBlock) < 20)
        {
            priorities.push_back(3);
        }
        if (inventory.GetBlockCount(BlockType::ObsidianBlock) < 4)
        {
            priorities.push_back(4);
        }
        if (inventory.GetArmorLevel() < 2)
        {
            priorities.push_back(103);
        }
        if (!inventory.HasItem(ItemType::Axe))
        {
            priorities.push_back(106);
        }
        if (inventory.GetUtility(UtilityType::AlarmTrap) < 1)
        {
            priorities.push_back(207);
        }
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            priorities.push_back(401);
        }
        else if (inventory.GetBlasterDamageLevel() == 0 && inventory.GetBlasterRapidFireLevel() < 2)
        {
            priorities.push_back(402);
        }
        if (!inventory.HasItem(ItemType::Bow))
        {
            priorities.push_back(108);
        }
        else if (inventory.GetBowUpgradeLevel() < 2)
        {
            priorities.push_back(109);
        }
        break;

    case BotRole::Rusher:
        if (inventory.GetBlocks() < 40)
        {
            priorities.push_back(2);
        }
        if (inventory.GetToolLevel() < 2)
        {
            priorities.push_back(102);
        }
        if (!inventory.HasItem(ItemType::Axe) && inventory.GetToolLevel() >= 1)
        {
            priorities.push_back(106);
        }
        if (inventory.GetSwordLevel() < 2)
        {
            priorities.push_back(101);
        }
        if (inventory.GetUtility(UtilityType::Fireball) < 1)
        {
            priorities.push_back(105);
        }
        if (inventory.GetUtility(UtilityType::Molotov) < 1)
        {
            priorities.push_back(206);
        }
        if (!inventory.HasItem(ItemType::Bow))
        {
            priorities.push_back(108);
        }
        else if (inventory.GetBowUpgradeLevel() < 1)
        {
            priorities.push_back(109);
        }
        if (inventory.GetUtility(UtilityType::Dash) < 1)
        {
            priorities.push_back(205);
        }
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            priorities.push_back(401);
        }
        break;

    case BotRole::Collector:
        if (team.forgeLevel < 4)
        {
            priorities.push_back(301);
        }
        if (team.healAuraLevel < 2)
        {
            priorities.push_back(302);
        }
        if (!team.enemyTrackerUnlocked)
        {
            priorities.push_back(304);
        }
        if (inventory.GetBlocks() < 24)
        {
            priorities.push_back(2);
        }
        if (inventory.GetArmorLevel() < 2)
        {
            priorities.push_back(103);
        }
        if (!inventory.HasItem(ItemType::Spear))
        {
            priorities.push_back(107);
        }
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            priorities.push_back(401);
        }
        if (!inventory.HasItem(ItemType::Bow))
        {
            priorities.push_back(108);
        }
        break;

    case BotRole::Fighter:
        if (inventory.GetSwordLevel() < 2)
        {
            priorities.push_back(101);
        }
        if (!inventory.HasItem(ItemType::Spear))
        {
            priorities.push_back(107);
        }
        if (!inventory.HasItem(ItemType::Axe) && inventory.GetArmorLevel() >= 1)
        {
            priorities.push_back(106);
        }
        if (inventory.GetArmorLevel() < 2)
        {
            priorities.push_back(103);
        }
        if (inventory.GetBlocks() < 24)
        {
            priorities.push_back(2);
        }
        if (inventory.GetUtility(UtilityType::Fireball) < 1)
        {
            priorities.push_back(105);
        }
        if (inventory.GetUtility(UtilityType::Molotov) < 1)
        {
            priorities.push_back(206);
        }
        if (inventory.GetUtility(UtilityType::Dash) < 1)
        {
            priorities.push_back(205);
        }
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            priorities.push_back(401);
        }
        else if (inventory.GetBlasterRapidFireLevel() == 0 && inventory.GetBlasterDamageLevel() < 3)
        {
            priorities.push_back(403);
        }
        if (!inventory.HasItem(ItemType::Bow))
        {
            priorities.push_back(108);
        }
        else if (inventory.GetBowUpgradeLevel() < 3)
        {
            priorities.push_back(109);
        }
        break;
    }

    if ((memory.role == BotRole::Collector || memory.role == BotRole::Defender)
        && inventory.GetUtility(UtilityType::HomeTeleport) < 1)
    {
        priorities.push_back(204);
    }
    if (bot.GetHealth() < 70)
    {
        priorities.push_back(203);
        priorities.push_back(202);
    }
    if (bot.GetSpeedBoostTimer() <= 0.0f)
    {
        priorities.push_back(201);
    }

    int legacyAttempt = 0;
    for (int choice : priorities)
    {
        PlayerCommand command;
        command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
        command.tick = matchSimulation_.CurrentTick();
        command.actionSeq = command.tick * 32u + static_cast<std::uint32_t>(++legacyAttempt);
        command.actionType = static_cast<int>(PlayerActionType::BuyItem);
        command.actionParamA = choice;
        command.actionParamB = 1;
        const PlayerActionResult result = ApplyPlayerEconomyCommand(bot, command);
        if (result.success)
        {
            if (automatch_.active)
            {
                const int category = choice < 100 ? 0
                    : (choice < 200 ? 1 : (choice < 300 ? 2 : (choice < 400 ? 3 : 4)));
                ++automatch_.purchasesByCategory[category];
            }
            PushPlayerActionResultSnapshot(result);
            AddEventMessage(bot.GetName() + ": " + result.message, GetTeamColor(team.color), 1.6f);
            return;
        }
    }
}

EnergyCore* Game::SelectBestAttackTarget(
    const Player& player,
    const BotFrameContext& frameContext,
    const TeamCoordinationBus* coordBus)
{
    const auto context = frameContext.teamContexts.find(player.GetTeamId());
    if (context != frameContext.teamContexts.end())
    {
        EnergyCore* selected = FindCoreByTeam(context->second.strategicPlan.attackCoreTeamId);
        return selected != nullptr && selected->IsAlive() ? selected : nullptr;
    }
    EnergyCore* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();
    const BotTuningGenome& tuning = BotTuningForTeam(player.GetTeamId());

    for (EnergyCore& core : matchSimulation_.Cores())
    {
        if (core.GetTeamId() == player.GetTeamId() || !core.IsAlive())
        {
            continue;
        }

        const Vector3 corePos = world_.GridToWorld(core.GetBlockPosition());
        const float distance = DistanceSquared(player.GetPosition(), corePos);
        const float weaknessBonus = static_cast<float>(core.GetMaxHealth() - core.GetHealth()) * 1.8f;
        // Opponent roles and exact off-screen positions are private. Visible
        // defenders influence tactics; strategic scoring uses public Core
        // state and observations shared by this bot's own team.
        const float defenderPenalty = 0.0f;

        const int coordinatedAttackers = coordBus != nullptr
            ? coordBus->CountSignal(
                CoordinationSignal::AttackingCore,
                matchSimulation_.MatchTimeSeconds(),
                kCoordinationSignalTtl,
                core.GetTeamId(),
                player.GetId())
            : 0;
        const float coordinationPenalty = static_cast<float>(coordinatedAttackers) * tuning.pressureCoordinationPenalty;
        const float accessBonus = HasBotCoreAccess(player, core) ? 95.0f : 0.0f;
        const bool clockwisePressure = !automatch_.active || automatch_.completedRuns % 2 == 0;
        const float neighborBonus = core.GetTeamId() == PreferredNeighborTeam(player.GetTeamId(), clockwisePressure) ? 120.0f : 0.0f;
        const float score = distance - weaknessBonus + defenderPenalty + coordinationPenalty - accessBonus - neighborBonus;
        if (score < bestScore)
        {
            bestScore = score;
            best = &core;
        }
    }

    return best;
}

bool Game::BotHasLineOfSight(const Player& observer, const Player& target) const
{
    const Vector3 eye {
        observer.GetPosition().x,
        observer.GetPosition().y + 0.78f,
        observer.GetPosition().z
    };
    // Two sample points on the target (torso + upper chest) so a wall that only
    // partially covers still leaves a "peeking" enemy visible — mirrors how a
    // human reacts to a sliver of an opponent. LOS is clear if EITHER reaches.
    const Vector3 samples[] {
        Vector3 { target.GetPosition().x, target.GetPosition().y + 0.55f, target.GetPosition().z },
        Vector3 { target.GetPosition().x, target.GetPosition().y + 1.10f, target.GetPosition().z }
    };
    for (const Vector3& sample : samples)
    {
        const Vector3 delta { sample.x - eye.x, sample.y - eye.y, sample.z - eye.z };
        const float distance = Length(delta);
        if (distance <= 0.35f)
        {
            return true; // effectively point-blank — always "seen"
        }
        const Vector3 direction { delta.x / distance, delta.y / distance, delta.z / distance };
        // Stop just short of the target so its own occupied cell (if any) does
        // not count as an obstruction. A solid block before that = LOS blocked.
        const std::optional<RaycastHit> hit = world_.Raycast(eye, direction, distance - 0.3f);
        if (!hit.has_value())
        {
            return true;
        }
    }
    return false;
}

Player* Game::FindNearbyEnemyPlayer(const Player& player, float maxDistance)
{
    Player* best = nullptr;
    float bestScore = std::numeric_limits<float>::max();
    const BotRole role = ResolveRoleForPlayerId(player.GetId(), botMemories_, botMemoryIndexByPlayerId_);
    const Team* ownTeam = FindTeam(player.GetTeamId());
    const Vector3 ownCore = ownTeam != nullptr ? world_.GridToWorld(ownTeam->coreBlock) : player.GetPosition();

    for (Player& other : players_)
    {
        if (other.GetId() == player.GetId()
            || other.GetTeamId() == player.GetTeamId()
            || !other.IsAlive()
            || other.IsEliminated())
        {
            continue;
        }

        const float distance = DistanceSquared(player.GetPosition(), other.GetPosition());
        if (distance > maxDistance * maxDistance)
        {
            continue;
        }
        // Can't target through a wall (e.g. Konvoy handcuffs) — same LOS rule
        // the primary combat perception uses.
        if (!BotHasLineOfSight(player, other))
        {
            continue;
        }

        float score = distance;
        score -= static_cast<float>(std::max(0, player.GetHealth() - other.GetHealth())) * 0.95f;
        const Team* enemyTeam = FindTeam(other.GetTeamId());
        if (enemyTeam != nullptr && !enemyTeam->coreAlive)
        {
            score -= 420.0f;
            if (other.GetHealth() <= player.GetHealth() + 12)
            {
                score -= 180.0f;
            }
        }
        if (role == BotRole::Defender)
        {
            score += DistanceSquared(other.GetPosition(), ownCore) * 0.24f;
        }
        if (role == BotRole::Collector && other.GetHealth() > player.GetHealth() + 12)
        {
            score += 24.0f;
        }
        if (score < bestScore)
        {
            bestScore = score;
            best = &other;
        }
    }

    return best;
}

BotMemory& Game::GetBotMemory(Player& bot)
{
    const int playerId = bot.GetId();
    const auto foundIndex = botMemoryIndexByPlayerId_.find(playerId);
    if (foundIndex != botMemoryIndexByPlayerId_.end())
    {
        const std::size_t index = foundIndex->second;
        if (index < botMemories_.size() && botMemories_[index].playerId == playerId)
        {
            return botMemories_[index];
        }
        botMemoryIndexByPlayerId_.erase(foundIndex);
    }

    for (std::size_t index = 0; index < botMemories_.size(); ++index)
    {
        BotMemory& memory = botMemories_[index];
        if (memory.playerId == bot.GetId())
        {
            botMemoryIndexByPlayerId_[playerId] = index;
            return memory;
        }
    }

    const BotRole role = RoleForBotId(bot.GetId());
    BotMemory memory {};
    memory.playerId = bot.GetId();
    memory.state = BotState::Collect;
    memory.role = role;
    memory.intent = BotIntent::SecureResources;
    memory.intentReason = "spawn plan";
    memory.roleReason = "spawn role";
    // Personal flank side: without it every bot strafes the same way and
    // mirrored duels look robotic.
    memory.personalitySeed = static_cast<std::uint32_t>(automatchSeed_)
        ^ (static_cast<std::uint32_t>(automatch_.completedRuns + 1) * 0x9e3779b9u)
        ^ (static_cast<std::uint32_t>(bot.GetId()) * 0x85ebca6bu);
    const BotPersonality personality = PersonalityForBot(bot.GetId(), memory.personalitySeed);
    memory.archetype = personality.archetype;
    memory.aggressionTrait = std::clamp((personality.aggression - 0.6f) / 0.8f, 0.0f, 1.0f);
    memory.cautionTrait = personality.caution;
    memory.economyTrait = std::clamp((personality.economy - 0.6f) / 0.8f, 0.0f, 1.0f);
    memory.teamworkTrait = personality.teamwork;
    memory.creativityTrait = personality.creativity;
    memory.planPatienceTrait = std::clamp((personality.patience - 0.55f) / 0.9f, 0.0f, 1.0f);
    memory.strafeSign = personality.flankSign;
    const std::size_t index = botMemories_.size();
    botMemories_.push_back(memory);
    botMemoryIndexByPlayerId_[playerId] = index;
    return botMemories_[index];
}

void Game::ApplyBotHitReaction(int playerId)
{
    // Only existing bot memories are touched: the human player has none and
    // must not get one created here.
    for (BotMemory& memory : botMemories_)
    {
        if (memory.playerId == playerId)
        {
            memory.reactionDelayTimer = std::max(
                memory.reactionDelayTimer,
                BotHitReactionSeconds(botDifficulty_));
            return;
        }
    }
}

std::optional<RaycastHit> Game::RaycastFromAim(const Player& player, float maxDistance) const
{
    const Vector3 aimDirection = cameraController_.GetAimDirection();
    const std::optional<RaycastHit> cameraHit = world_.Raycast(cameraController_.GetAimOrigin(), aimDirection, maxDistance);
    if (cameraHit.has_value())
    {
        const Vector3 blockCenter = world_.GridToWorld(cameraHit->block);
        if (DistanceSquared(player.GetPosition(), blockCenter) > 2.0f)
        {
            return cameraHit;
        }
    }

    const Vector3 eye {
        player.GetPosition().x,
        player.GetPosition().y + 0.78f,
        player.GetPosition().z
    };
    return world_.Raycast(eye, aimDirection, maxDistance);
}

std::optional<RaycastHit> Game::RaycastFromPlayerEye(const Player& player, Vector3 aimDirection, float maxDistance) const
{
    const Vector3 eye {
        player.GetPosition().x,
        player.GetPosition().y + 0.78f,
        player.GetPosition().z
    };
    return world_.Raycast(eye, aimDirection, maxDistance);
}

std::optional<GridPos> Game::FindCoreDefenseBlock(const EnergyCore& core, const Player& bot) const
{
    const GridPos corePos = core.GetBlockPosition();
    // Attack the walls the defenders actually planned/built: the adaptive
    // plan of the core's own team is exactly the set worth breaching.
    const Team* coreTeam = FindTeam(core.GetTeamId());
    const std::vector<GridPos> candidates = coreTeam != nullptr
        ? TeamDefenseCells(*coreTeam)
        : CoreDefensePositions(corePos);
    const Vector3 eye {
        bot.GetPosition().x,
        bot.GetPosition().y + 0.78f,
        bot.GetPosition().z
    };
    const Vector3 coreCenter {
        static_cast<float>(corePos.x),
        static_cast<float>(corePos.y) + 0.58f,
        static_cast<float>(corePos.z)
    };
    const Vector3 toCore {
        coreCenter.x - eye.x,
        coreCenter.y - eye.y,
        coreCenter.z - eye.z
    };
    const float rayDistance = std::sqrt(toCore.x * toCore.x + toCore.y * toCore.y + toCore.z * toCore.z) + 0.25f;
    const std::optional<RaycastHit> directBlock = world_.Raycast(eye, toCore, rayDistance);
    if (directBlock.has_value() && directBlock->block != corePos)
    {
        const Block* block = world_.GetBlock(directBlock->block);
        if (block != nullptr && block->breakable && IsBreakableByPlayers(block->type))
        {
            return directBlock->block;
        }
    }

    std::optional<GridPos> best;
    float bestDistance = std::numeric_limits<float>::max();
    for (const GridPos& candidate : candidates)
    {
        const Block* block = world_.GetBlock(candidate);
        if (block == nullptr || !block->breakable || !IsBreakableByPlayers(block->type))
        {
            continue;
        }

        const float coreShellDistance = static_cast<float>(
            std::abs(candidate.x - corePos.x)
            + std::abs(candidate.y - corePos.y)
            + std::abs(candidate.z - corePos.z));
        const float score = DistanceSquared(bot.GetPosition(), world_.GridToWorld(candidate))
            + coreShellDistance * 3.5f
            + BreakSeconds(block->type, bot.GetInventory().GetToolLevel()) * 1.6f;
        if (score < bestDistance)
        {
            bestDistance = score;
            best = candidate;
        }
    }

    return best;
}

const std::vector<GridPos>& Game::TeamDefenseCells(const Team& team) const
{
    static const std::vector<GridPos> kNoCells;
    if (team.id < 0 || team.id >= static_cast<int>(teamDefensePlans_.size()))
    {
        return kNoCells;
    }
    TeamDefensePlan& plan = teamDefensePlans_[static_cast<std::size_t>(team.id)];
    const float now = matchSimulation_.MatchTimeSeconds();
    if (plan.valid && plan.core == team.coreBlock && now < plan.nextRecomputeTime)
    {
        return plan.cells;
    }

    // The hash ignores the team's own breakable blocks, so bots building the
    // plan never invalidate it; castle/enemy geometry changes near the core do.
    const std::uint64_t regionHash = defense_planner::HashRegion(world_, team);
    plan.nextRecomputeTime = now + 2.0f;
    if (plan.valid && plan.core == team.coreBlock && regionHash == plan.regionHash)
    {
        return plan.cells;
    }

    plan.core = team.coreBlock;
    plan.regionHash = regionHash;
    plan.valid = true;
    const std::optional<std::vector<GridPos>> computed = defense_planner::ComputePlan(world_, team);
    // An opening too wide to seal within budget falls back to the legacy
    // dome: partial cover beats standing idle.
    plan.cells = computed.has_value() ? *computed : CoreDefensePositions(team.coreBlock);
    return plan.cells;
}

std::optional<GridPos> Game::FindMissingCoreDefenseBlock(const Team& team) const
{
    for (const GridPos& cell : TeamDefenseCells(team))
    {
        if (world_.IsAir(cell))
        {
            return cell;
        }
    }
    return std::nullopt;
}

std::optional<GridPos> Game::FindUpgradeableCoreDefenseBlock(const Team& team, const Player& bot) const
{
    const std::optional<BlockType> replacement = BestDefenseBlockAvailable(bot.GetInventory());
    if (!replacement.has_value())
    {
        return std::nullopt;
    }

    const int replacementRank = DefenseBlockRank(*replacement);
    const std::vector<GridPos>& candidates = TeamDefenseCells(team);
    std::optional<GridPos> best;
    float bestScore = std::numeric_limits<float>::max();

    for (const GridPos& candidate : candidates)
    {
        const Block* block = world_.GetBlock(candidate);
        if (block == nullptr
            || !block->breakable
            || block->teamId != team.id
            || !IsBreakableByPlayers(block->type))
        {
            continue;
        }

        const int currentRank = DefenseBlockRank(block->type);
        if (currentRank <= 0 || replacementRank <= currentRank)
        {
            continue;
        }

        const float score = static_cast<float>(currentRank) * 18.0f
            + DistanceSquared(bot.GetPosition(), world_.GridToWorld(candidate)) * 0.12f;
        if (score < bestScore)
        {
            bestScore = score;
            best = candidate;
        }
    }

    return best;
}

bool Game::TryBotUpgradeCoreDefense(Player& bot, Team& team, float dt)
{
    const std::optional<GridPos> upgradeTarget = FindUpgradeableCoreDefenseBlock(team, bot);
    if (!upgradeTarget.has_value())
    {
        return false;
    }

    const GridPos target = *upgradeTarget;
    const Vector3 targetPos = world_.GridToWorld(target);
    if (DistanceSquared(bot.GetPosition(), targetPos) > 12.0f)
    {
        Vector3 wish = Normalize2D(Vector3 { targetPos.x - bot.GetPosition().x, 0.0f, targetPos.z - bot.GetPosition().z });
        if (IsVoidThreatAt(Vector3 { bot.GetPosition().x + wish.x * 0.85f, bot.GetPosition().y, bot.GetPosition().z + wish.z * 0.85f }))
        {
            TryBotBridgeBlock(bot, targetPos);
        }
        const PlayerCommand movementCommand = BuildBotMovementCommand(
            bot,
            matchSimulation_.CurrentTick(),
            wish,
            wish,
            false,
            false);
        ApplyPlayerCommand(bot, movementCommand, dt);
        ApplyStandingBlockEffects(bot, false);
        return true;
    }

    const Block* block = world_.GetBlock(target);
    if (block == nullptr || !block->breakable || block->teamId != team.id || !IsBreakableByPlayers(block->type))
    {
        return false;
    }

    BotMemory& memory = GetBotMemory(bot);
    if (!memory.hasBreakTarget || memory.breakTarget != target)
    {
        memory.breakTarget = target;
        memory.hasBreakTarget = true;
        memory.breakProgress = 0.0f;
    }

    const Vector3 eye { bot.GetPosition().x, bot.GetPosition().y + 0.78f, bot.GetPosition().z };
    Vector3 direction { targetPos.x - eye.x, targetPos.y - eye.y, targetPos.z - eye.z };
    const float length = Length(direction);
    if (length <= 0.05f) return true;
    direction = Vector3Scale(direction, 1.0f / length);
    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(bot.GetId());
    command.tick = matchSimulation_.CurrentTick();
    command.selectedSlot = PickaxeSlot(bot);
    command.aimYaw = std::atan2(direction.x, -direction.z);
    command.aimPitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
    command.attackHeld = true;
    ApplyPlayerCommand(bot, command, 0.0f);
    ApplyNetworkPlayerActions(bot, command, dt);
    if (world_.IsAir(target))
    {
        memory.hasBreakTarget = false;
        memory.breakProgress = 0.0f;
    }
    return true;

    return true;
}

bool Game::TryBotRepairCoreDefense(Player& bot, Team& team, float dt)
{
    if (!team.coreAlive || bot.GetInventory().GetBlocks() <= 0)
    {
        return false;
    }

    std::optional<GridPos> missing;
    Vector3 threatDirection {};
    float nearestThreatSq = std::numeric_limits<float>::max();
    for (const Player& candidate : players_)
    {
        if (!candidate.IsAlive() || candidate.GetTeamId() == team.id
            || !BotHasLineOfSight(bot, candidate))
        {
            continue;
        }
        const float distanceSq = DistanceSquared(candidate.GetPosition(), world_.GridToWorld(team.coreBlock));
        if (distanceSq < nearestThreatSq)
        {
            nearestThreatSq = distanceSq;
            threatDirection = Normalize2D(Vector3 {
                candidate.GetPosition().x - static_cast<float>(team.coreBlock.x),
                0.0f,
                candidate.GetPosition().z - static_cast<float>(team.coreBlock.z) });
        }
    }
    float bestMissingScore = std::numeric_limits<float>::max();
    for (const GridPos& candidate : TeamDefenseCells(team))
    {
        if (!world_.IsAir(candidate))
        {
            continue;
        }

        std::string reason;
        if (CanPlaceBlockAt(candidate, bot, &reason))
        {
            const Vector3 offset = Normalize2D(Vector3 {
                static_cast<float>(candidate.x - team.coreBlock.x),
                0.0f,
                static_cast<float>(candidate.z - team.coreBlock.z) });
            const float facesThreat = offset.x * threatDirection.x + offset.z * threatDirection.z;
            const float score = DistanceSquared(bot.GetPosition(), world_.GridToWorld(candidate)) * 0.08f
                - facesThreat * (nearestThreatSq < 625.0f ? 12.0f : 2.0f)
                + static_cast<float>(candidate.y - team.coreBlock.y) * 1.5f;
            if (score < bestMissingScore)
            {
                bestMissingScore = score;
                missing = candidate;
            }
        }
    }
    if (!missing.has_value())
    {
        // Classic open-core deadlock: the last shell holes fail placement
        // only because the builder's own body fills them.  Step aside so the
        // next tick can close the shell; otherwise the team camps "repair"
        // forever and never switches to offense.
        const auto overlapsBot = [&bot](const GridPos& cell)
        {
            const Vector3 position = bot.GetPosition();
            return std::fabs(position.x - static_cast<float>(cell.x)) < 0.85f
                && std::fabs(position.z - static_cast<float>(cell.z)) < 0.85f
                && position.y + 0.95f > static_cast<float>(cell.y) - 0.5f
                && position.y - 0.95f < static_cast<float>(cell.y) + 0.5f;
        };
        std::optional<GridPos> blockedBySelf;
        for (const GridPos& candidate : TeamDefenseCells(team))
        {
            if (world_.IsAir(candidate) && overlapsBot(candidate))
            {
                blockedBySelf = candidate;
                break;
            }
        }
        if (blockedBySelf.has_value())
        {
            const Vector3 cellCenter = world_.GridToWorld(*blockedBySelf);
            Vector3 away = Normalize2D(Vector3 {
                bot.GetPosition().x - cellCenter.x,
                0.0f,
                bot.GetPosition().z - cellCenter.z });
            if (std::fabs(away.x) < 0.001f && std::fabs(away.z) < 0.001f)
            {
                away = Vector3 { 1.0f, 0.0f, 0.0f };
            }
            const PlayerCommand stepAside = BuildBotMovementCommand(
                bot,
                matchSimulation_.CurrentTick(),
                away,
                away,
                false,
                false);
            ApplyPlayerCommand(bot, stepAside, dt);
            ApplyStandingBlockEffects(bot, false);
            return true;
        }
        return TryBotUpgradeCoreDefense(bot, team, dt);
    }

    const Vector3 target = world_.GridToWorld(*missing);
    if (DistanceSquared(bot.GetPosition(), target) > 12.0f)
    {
        Vector3 wish = Normalize2D(Vector3 { target.x - bot.GetPosition().x, 0.0f, target.z - bot.GetPosition().z });
        if (IsVoidThreatAt(Vector3 { bot.GetPosition().x + wish.x * 0.85f, bot.GetPosition().y, bot.GetPosition().z + wish.z * 0.85f }))
        {
            TryBotBridgeBlock(bot, target);
        }
        const PlayerCommand movementCommand = BuildBotMovementCommand(
            bot,
            matchSimulation_.CurrentTick(),
            wish,
            wish,
            false,
            false);
        ApplyPlayerCommand(bot, movementCommand, dt);
        ApplyStandingBlockEffects(bot, false);
        return true;
    }

    BotMemory& memory = GetBotMemory(bot);
    if (memory.repairPlaceCooldown > 0.0f)
    {
        return true;
    }
    if (TryBotPlaceCommand(bot, *missing, dt, false))
    {
        if (automatch_.active)
        {
            ++automatch_.coreFortifications;
        }
        // Rate-limit wall rebuilding: an instant infinite wall made every
        // siege unwinnable; a paced defender can be out-damaged by two
        // coordinated attackers but still holds off a single one.
        memory.repairPlaceCooldown = botDifficulty_ == BotDifficulty::Hard
            ? 0.75f
            : (botDifficulty_ == BotDifficulty::Easy ? 1.3f : 0.95f);
        AddEventMessage(bot.GetName() + " починил защиту Кора", GetTeamColor(team.color), 1.5f);
        return true;
    }
    return false;
}

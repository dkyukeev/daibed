#include "Navigation/VoxelPathfinder.h"

#include "Navigation/NavigationActionLibrary.h"
#include "Navigation/NavigationWorldView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace
{
using PackedSearchKey = std::uint64_t;

// Search coordinates are local to the current segment. Packing the complete
// state into one integer makes hashing/comparison cheap while retaining exact
// bridge inventory and chain state. The ranges are deliberately much larger
// than the current bounded search (64 horizontal / 24 vertical).
bool TryPackSearchKey(
    GridPos origin,
    GridPos support,
    int remainingBlocks,
    int bridgeChain,
    PackedSearchKey& result) noexcept
{
    const int dx = support.x - origin.x;
    const int dy = support.y - origin.y;
    const int dz = support.z - origin.z;
    if (dx < -2048 || dx > 2047
        || dz < -2048 || dz > 2047
        || dy < -512 || dy > 511
        || remainingBlocks < 0 || remainingBlocks > 65535
        || bridgeChain < 0 || bridgeChain > 255)
    {
        return false;
    }
    result = static_cast<PackedSearchKey>(dx + 2048)
        | (static_cast<PackedSearchKey>(dz + 2048) << 12U)
        | (static_cast<PackedSearchKey>(dy + 512) << 24U)
        | (static_cast<PackedSearchKey>(remainingBlocks) << 34U)
        | (static_cast<PackedSearchKey>(bridgeChain) << 50U);
    return true;
}

std::size_t HashPackedKey(PackedSearchKey key) noexcept
{
    key += 0x9e3779b97f4a7c15ULL;
    key = (key ^ (key >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    key = (key ^ (key >> 27U)) * 0x94d049bb133111ebULL;
    return static_cast<std::size_t>(key ^ (key >> 31U));
}

std::size_t NextPowerOfTwo(std::size_t value) noexcept
{
    std::size_t result = 16;
    while (result < value) result <<= 1U;
    return result;
}

struct SearchNode
{
    PackedSearchKey key = 0;
    GridPos support {};
    int remainingBlocks = 0;
    int bridgeChain = 0;
    float cost = std::numeric_limits<float>::infinity();
    float heuristic = 0.0f;
    float score = std::numeric_limits<float>::infinity();
    int depth = 0;
    int parentIndex = -1;
    int heapPosition = -1;
    std::uint64_t order = 0;
    PlannedMovement movement {};
};

struct SearchBucket
{
    int nodeIndex = -1;
    std::uint32_t generation = 0;
};

struct ThreadSearchStorage
{
    std::vector<SearchBucket> buckets;
    std::vector<SearchNode> nodes;
    std::vector<int> heap;
    std::uint32_t generation = 0;
};

thread_local ThreadSearchStorage gSearchStorage;

class SearchNodeArena
{
public:
    SearchNodeArena(ThreadSearchStorage& storage, std::size_t expectedNodes)
        : buckets_(storage.buckets), nodes_(storage.nodes)
    {
        const std::size_t requiredBuckets = NextPowerOfTwo(
            std::max<std::size_t>(32, expectedNodes * 2U));
        if (buckets_.size() < requiredBuckets) buckets_.resize(requiredBuckets);
        if (++storage.generation == 0)
        {
            for (SearchBucket& bucket : buckets_) bucket.generation = 0;
            storage.generation = 1;
        }
        generation_ = storage.generation;
        nodes_.clear();
        if (nodes_.capacity() < expectedNodes) nodes_.reserve(expectedNodes);
    }

    int Find(PackedSearchKey key) const noexcept
    {
        const std::size_t mask = buckets_.size() - 1U;
        std::size_t slot = HashPackedKey(key) & mask;
        for (std::size_t probe = 0; probe < buckets_.size(); ++probe)
        {
            const SearchBucket& bucket = buckets_[slot];
            if (bucket.generation != generation_) return -1;
            const int index = bucket.nodeIndex;
            if (nodes_[static_cast<std::size_t>(index)].key == key) return index;
            slot = (slot + 1U) & mask;
        }
        return -1;
    }

    int Create(
        PackedSearchKey key,
        GridPos support,
        int remainingBlocks,
        int bridgeChain)
    {
        const int index = static_cast<int>(nodes_.size());
        nodes_.push_back(SearchNode {});
        SearchNode& node = nodes_.back();
        node.key = key;
        node.support = support;
        node.remainingBlocks = remainingBlocks;
        node.bridgeChain = bridgeChain;

        const std::size_t mask = buckets_.size() - 1U;
        std::size_t slot = HashPackedKey(key) & mask;
        while (buckets_[slot].generation == generation_) slot = (slot + 1U) & mask;
        buckets_[slot].nodeIndex = index;
        buckets_[slot].generation = generation_;
        return index;
    }

    SearchNode& operator[](int index) noexcept { return nodes_[static_cast<std::size_t>(index)]; }
    const SearchNode& operator[](int index) const noexcept { return nodes_[static_cast<std::size_t>(index)]; }
    std::size_t Size() const noexcept { return nodes_.size(); }

private:
    std::vector<SearchBucket>& buckets_;
    std::vector<SearchNode>& nodes_;
    std::uint32_t generation_ = 0;
};

class SearchOpenHeap
{
public:
    SearchOpenHeap(
        SearchNodeArena& arena,
        std::vector<int>& storage,
        std::size_t reserve)
        : arena_(arena), heap_(storage)
    {
        heap_.clear();
        if (heap_.capacity() < reserve) heap_.reserve(reserve);
    }

    bool Empty() const noexcept { return heap_.empty(); }
    std::size_t Size() const noexcept { return heap_.size(); }
    std::size_t PeakSize() const noexcept { return peakSize_; }
    std::uint64_t UpdateCount() const noexcept { return updateCount_; }
    int PeekLowest() const noexcept { return heap_.empty() ? -1 : heap_.front(); }

    void Insert(int nodeIndex)
    {
        SearchNode& node = arena_[nodeIndex];
        node.heapPosition = static_cast<int>(heap_.size());
        heap_.push_back(nodeIndex);
        BubbleUp(node.heapPosition);
        peakSize_ = std::max(peakSize_, heap_.size());
    }

    void DecreaseKey(int nodeIndex)
    {
        ++updateCount_;
        BubbleUp(arena_[nodeIndex].heapPosition);
    }

    int PopLowest()
    {
        const int result = heap_.front();
        const int tail = heap_.back();
        heap_.pop_back();
        arena_[result].heapPosition = -1;
        if (!heap_.empty())
        {
            heap_.front() = tail;
            arena_[tail].heapPosition = 0;
            BubbleDown(0);
        }
        return result;
    }

private:
    bool Better(int lhsIndex, int rhsIndex) const noexcept
    {
        const SearchNode& lhs = arena_[lhsIndex];
        const SearchNode& rhs = arena_[rhsIndex];
        if (lhs.score != rhs.score) return lhs.score < rhs.score;
        if (lhs.heuristic != rhs.heuristic) return lhs.heuristic < rhs.heuristic;
        if (lhs.cost != rhs.cost) return lhs.cost < rhs.cost;
        if (lhs.support.x != rhs.support.x) return lhs.support.x < rhs.support.x;
        if (lhs.support.z != rhs.support.z) return lhs.support.z < rhs.support.z;
        if (lhs.support.y != rhs.support.y) return lhs.support.y < rhs.support.y;
        return lhs.order < rhs.order;
    }

    void Swap(std::size_t lhs, std::size_t rhs)
    {
        std::swap(heap_[lhs], heap_[rhs]);
        arena_[heap_[lhs]].heapPosition = static_cast<int>(lhs);
        arena_[heap_[rhs]].heapPosition = static_cast<int>(rhs);
    }

    void BubbleUp(int position)
    {
        std::size_t child = static_cast<std::size_t>(position);
        while (child > 0)
        {
            const std::size_t parent = (child - 1U) >> 1U;
            if (!Better(heap_[child], heap_[parent])) break;
            Swap(child, parent);
            child = parent;
        }
    }

    void BubbleDown(std::size_t position)
    {
        while (true)
        {
            const std::size_t left = position * 2U + 1U;
            if (left >= heap_.size()) return;
            const std::size_t right = left + 1U;
            std::size_t best = left;
            if (right < heap_.size() && Better(heap_[right], heap_[left])) best = right;
            if (!Better(heap_[best], heap_[position])) return;
            Swap(best, position);
            position = best;
        }
    }

    SearchNodeArena& arena_;
    std::vector<int>& heap_;
    std::size_t peakSize_ = 0;
    std::uint64_t updateCount_ = 0;
};

float HorizontalDistance(GridPos a, GridPos b)
{
    const float dx = static_cast<float>(a.x - b.x);
    const float dz = static_cast<float>(a.z - b.z);
    return std::sqrt(dx * dx + dz * dz);
}

bool InBounds(GridPos start, GridPos candidate, const NavigationSearchLimits& limits)
{
    return std::abs(candidate.x - start.x) <= limits.maxSearchRadius
        && std::abs(candidate.z - start.z) <= limits.maxSearchRadius
        && std::abs(candidate.y - start.y) <= limits.maxVerticalRange;
}

bool InSearchCorridor(GridPos candidate, const NavigationSearchLimits& limits)
{
    if (!limits.constrainToCorridor) return true;
    const int minY = std::min(limits.corridorStart.y, limits.corridorEnd.y)
        - limits.corridorVerticalPadding;
    const int maxY = std::max(limits.corridorStart.y, limits.corridorEnd.y)
        + limits.corridorVerticalPadding;
    if (candidate.y < minY || candidate.y > maxY) return false;

    const float vx = static_cast<float>(limits.corridorEnd.x - limits.corridorStart.x);
    const float vz = static_cast<float>(limits.corridorEnd.z - limits.corridorStart.z);
    const float wx = static_cast<float>(candidate.x - limits.corridorStart.x);
    const float wz = static_cast<float>(candidate.z - limits.corridorStart.z);
    const float lengthSquared = vx * vx + vz * vz;
    const float t = lengthSquared > 0.001f
        ? std::clamp((wx * vx + wz * vz) / lengthSquared, 0.0f, 1.0f)
        : 0.0f;
    const float dx = wx - vx * t;
    const float dz = wz - vz * t;
    return dx * dx + dz * dz
        <= limits.corridorHalfWidth * limits.corridorHalfWidth;
}

struct SearchQueryEntry
{
    GridPos support {};
    GridPos firstBlocker {};
    float exposure = 0.0f;
    float threat = 0.0f;
    int blockerCount = 0;
    std::uint32_t generation = 0;
    bool clearanceValid = false;
    bool costValid = false;
};

struct ThreadQueryStorage
{
    std::array<SearchQueryEntry, 4096> entries {};
    std::uint32_t generation = 0;
};

thread_local ThreadQueryStorage gQueryStorage;

class SearchQueryCache
{
public:
    SearchQueryCache(const NavigationWorldView& world, const NavigationProfile& profile)
        : world_(world), profile_(profile)
    {
        if (++gQueryStorage.generation == 0)
        {
            for (SearchQueryEntry& entry : gQueryStorage.entries) entry.generation = 0;
            gQueryStorage.generation = 1;
        }
        generation_ = gQueryStorage.generation;
    }

    int BlockingBodyCells(GridPos support, GridPos* firstBlocker)
    {
        SearchQueryEntry& entry = Get(support);
        if (!entry.clearanceValid)
        {
            entry.blockerCount = world_.CountBlockingBodyCells(
                support, profile_, &entry.firstBlocker, 2);
            entry.clearanceValid = true;
        }
        if (firstBlocker != nullptr && entry.blockerCount > 0) *firstBlocker = entry.firstBlocker;
        return entry.blockerCount;
    }

    bool IsBodyClear(GridPos support) { return BlockingBodyCells(support, nullptr) == 0; }

    float Exposure(GridPos support)
    {
        SearchQueryEntry& entry = Get(support);
        if (!entry.costValid) PopulateCost(entry);
        return entry.exposure;
    }

    float Threat(GridPos support)
    {
        SearchQueryEntry& entry = Get(support);
        if (!entry.costValid) PopulateCost(entry);
        return entry.threat;
    }

    void Finalize(PlannedMovement& movement, float executionRisk)
    {
        SearchQueryEntry& entry = Get(movement.to);
        if (!entry.costValid) PopulateCost(entry);
        movement.threatCost = entry.threat * profile_.ThreatCostScale();
        const float edgeRisk = entry.exposure
            * (0.08f + (1.0f - profile_.riskTolerance) * 0.16f);
        movement.totalCost = movement.movementCost
            + movement.timeCost
            + movement.resourceCost
            + movement.threatCost
            + edgeRisk
            + executionRisk;
    }

    bool IsSafeSprintCell(GridPos support)
    {
        return Threat(support) < 2.0f && Exposure(support) <= 1.0f;
    }

    void FinalizeSprint(
        PlannedMovement& movement,
        GridPos direction,
        int span,
        float executionRisk)
    {
        movement.threatCost = 0.0f;
        float edgeRisk = 0.0f;
        for (int offset = 1; offset <= span; ++offset)
        {
            const GridPos support {
                movement.from.x + direction.x * offset,
                movement.from.y,
                movement.from.z + direction.z * offset
            };
            SearchQueryEntry& entry = Get(support);
            if (!entry.costValid) PopulateCost(entry);
            movement.threatCost += entry.threat * profile_.ThreatCostScale();
            edgeRisk += entry.exposure
                * (0.08f + (1.0f - profile_.riskTolerance) * 0.16f);
        }
        movement.totalCost = movement.movementCost
            + movement.timeCost
            + movement.resourceCost
            + movement.threatCost
            + edgeRisk
            + executionRisk;
    }

private:
    SearchQueryEntry& Get(GridPos support)
    {
        std::size_t hash = GridPosHash {}(support);
        hash ^= hash >> 16U;
        SearchQueryEntry& entry = gQueryStorage.entries[
            hash & (gQueryStorage.entries.size() - 1U)];
        if (entry.generation != generation_ || !(entry.support == support))
        {
            entry = SearchQueryEntry {};
            entry.support = support;
            entry.generation = generation_;
        }
        return entry;
    }

    void PopulateCost(SearchQueryEntry& entry)
    {
        constexpr std::array<GridPos, 4> offsets {
            GridPos { 1, 0, 0 }, GridPos { -1, 0, 0 },
            GridPos { 0, 0, 1 }, GridPos { 0, 0, -1 }
        };
        int openSides = 0;
        for (const GridPos& offset : offsets)
        {
            if (world_.IsAir(GridPos {
                entry.support.x + offset.x,
                entry.support.y + offset.y,
                entry.support.z + offset.z }))
            {
                ++openSides;
            }
        }
        entry.exposure = static_cast<float>(openSides);
        entry.threat = world_.ThreatCostAt(entry.support, profile_.bodyCenterAboveSupport);
        entry.costValid = true;
    }

    const NavigationWorldView& world_;
    const NavigationProfile& profile_;
    std::uint32_t generation_ = 0;
};

bool DirectionSame(const PlannedMovement& a, const PlannedMovement& b)
{
    return b.to.x - b.from.x == a.to.x - a.from.x
        && b.to.z - b.from.z == a.to.z - a.from.z
        && a.to.y == a.from.y && b.to.y == b.from.y;
}

void MarkSprintRuns(
    std::vector<PlannedMovement>& movements,
    SearchQueryCache& query,
    const NavigationProfile& profile)
{
    if (!profile.canSprint)
    {
        return;
    }
    std::size_t begin = 0;
    while (begin < movements.size())
    {
        if (movements[begin].type != MovementType::Walk)
        {
            ++begin;
            continue;
        }
        std::size_t end = begin + 1;
        while (end < movements.size()
            && movements[end].type == MovementType::Walk
            && DirectionSame(movements[begin], movements[end])
            && query.Threat(movements[end].to) < 2.0f
            && query.Exposure(movements[end].to) <= 1.0f)
        {
            ++end;
        }
        if (end - begin >= 3)
        {
            for (std::size_t index = begin; index < end; ++index)
            {
                PlannedMovement& movement = movements[index];
                movement.type = MovementType::Sprint;
                movement.requiresSprint = true;
                const float oldTime = movement.timeCost;
                movement.timeCost = HorizontalDistance(movement.from, movement.to)
                    / std::max(0.1f, profile.sprintSpeed);
                movement.totalCost += movement.timeCost - oldTime;
            }
        }
        begin = end;
    }
}
}

NavigationSearchResult VoxelPathfinder::FindPath(
    const NavigationState& requestedStart,
    const NavigationGoal& goal,
    const NavigationWorldView& world,
    const NavigationProfile& profile,
    const NavigationSearchLimits& requestedLimits) const
{
    NavigationSearchResult result;
    NavigationSearchLimits limits = requestedLimits;
    limits.maxExpansions = std::max(1, limits.maxExpansions);
    limits.maxActions = std::max(1, limits.maxActions);
    limits.maxSearchRadius = std::clamp(limits.maxSearchRadius, 1, 2047);
    limits.maxVerticalRange = std::clamp(limits.maxVerticalRange, 1, 511);
    limits.maxOpenNodes = std::max(16, limits.maxOpenNodes);
    limits.heuristicWeight = std::clamp(limits.heuristicWeight, 1.0f, 2.5f);
    limits.corridorHalfWidth = std::clamp(limits.corridorHalfWidth, 1.0f, 64.0f);
    limits.corridorVerticalPadding = std::clamp(limits.corridorVerticalPadding, 1, 64);

    NavigationState start = requestedStart;
    start.remainingBridgeBlocks = std::clamp(
        start.remainingBridgeBlocks > 0 ? start.remainingBridgeBlocks : profile.availableBridgeBlocks,
        0,
        std::min(profile.availableBridgeBlocks, 65535));
    start.consecutiveBridgeBlocks = 0;
    result.path.start = start.support;
    result.path.resolvedGoal = start.support;
    result.path.worldRevision = world.WorldRevision();
    SearchQueryCache query(world, profile);
    const float maximumJumpRise = profile.MaximumJumpRise();
    int maxExecutableGapJump = 0;
    for (int gap = 1; gap <= profile.maxGapJumpBlocks; ++gap)
    {
        if (!profile.CanExecuteGapJump(gap)) break;
        maxExecutableGapJump = gap;
    }

    if (!world.IsSupported(start.support) || !query.IsBodyClear(start.support))
    {
        result.status = NavigationSearchStatus::InvalidStart;
        result.description = "start has no safe support or body clearance";
        return result;
    }
    if (goal.IsSatisfied(start, world))
    {
        result.status = NavigationSearchStatus::AlreadySatisfied;
        result.description = "goal already satisfied";
        return result;
    }

    PackedSearchKey startKey = 0;
    if (!TryPackSearchKey(start.support, start.support, start.remainingBridgeBlocks, 0, startKey))
    {
        result.status = NavigationSearchStatus::InvalidStart;
        result.description = "start state exceeds packed navigation limits";
        return result;
    }
    const std::size_t expectedNodes = static_cast<std::size_t>(limits.maxOpenNodes)
        + static_cast<std::size_t>(limits.maxExpansions) + 1U;
    SearchNodeArena nodes(gSearchStorage, expectedNodes);
    SearchOpenHeap open(
        nodes, gSearchStorage.heap, static_cast<std::size_t>(limits.maxOpenNodes));
    const int startIndex = nodes.Create(startKey, start.support, start.remainingBridgeBlocks, 0);
    const float startHeuristic = goal.EstimateRemainingCost(start, world);
    std::uint64_t order = 0;
    SearchNode& startNode = nodes[startIndex];
    startNode.cost = 0.0f;
    startNode.heuristic = startHeuristic;
    startNode.score = startHeuristic * limits.heuristicWeight;
    startNode.order = order++;
    open.Insert(startIndex);

    int bestIndex = startIndex;
    float bestHeuristic = startHeuristic;
    int reachedIndex = -1;
    bool reached = false;
    bool openLimit = false;
    bool actionLimit = false;
    int corridorRejected = 0;

    int expanded = 0;
    while (!open.Empty() && expanded < limits.maxExpansions)
    {
        const int currentIndex = open.PopLowest();
        const SearchNode& currentNode = nodes[currentIndex];
        const float currentCost = currentNode.cost;
        const int currentDepth = currentNode.depth;
        ++expanded;

        NavigationState currentState {
            currentNode.support,
            currentNode.remainingBlocks,
            currentNode.bridgeChain
        };
        if (goal.IsSatisfied(currentState, world))
        {
            reachedIndex = currentIndex;
            reached = true;
            break;
        }
        if (currentDepth >= limits.maxActions)
        {
            actionLimit = true;
            continue;
        }

        const auto consider = [&] (
            GridPos nextSupport,
            int nextRemainingBlocks,
            int nextBridgeChain,
            PlannedMovement movement)
        {
            for (const NavigationFailedTransition& failure : limits.failedTransitions)
                if (failure.from == movement.from && failure.to == movement.to && failure.type == movement.type)
                    return;

            if (!InBounds(start.support, nextSupport, limits))
            {
                return;
            }
            if (!InSearchCorridor(nextSupport, limits))
            {
                ++corridorRejected;
                return;
            }
            PackedSearchKey nextKey = 0;
            if (!TryPackSearchKey(
                start.support, nextSupport, nextRemainingBlocks, nextBridgeChain, nextKey))
            {
                return;
            }
            const float nextCost = currentCost + movement.totalCost;
            int nextIndex = nodes.Find(nextKey);
            if (nextIndex >= 0 && nodes[nextIndex].cost <= nextCost + 0.0001f) return;
            const bool needsHeapSlot = nextIndex < 0 || nodes[nextIndex].heapPosition < 0;
            if (needsHeapSlot && static_cast<int>(open.Size()) >= limits.maxOpenNodes)
            {
                openLimit = true;
                return;
            }

            const NavigationState nextState {
                nextSupport,
                nextRemainingBlocks,
                nextBridgeChain
            };
            const float heuristic = goal.EstimateRemainingCost(nextState, world);
            if (nextIndex < 0)
            {
                nextIndex = nodes.Create(
                    nextKey, nextSupport, nextRemainingBlocks, nextBridgeChain);
            }
            SearchNode& nextNode = nodes[nextIndex];
            nextNode.cost = nextCost;
            nextNode.heuristic = heuristic;
            nextNode.score = nextCost + heuristic * limits.heuristicWeight;
            nextNode.depth = currentDepth + 1;
            nextNode.parentIndex = currentIndex;
            nextNode.movement = movement;
            nextNode.order = order++;
            if (heuristic < bestHeuristic - 0.0001f
                || (std::fabs(heuristic - bestHeuristic) <= 0.0001f
                    && nextCost < nodes[bestIndex].cost))
            {
                bestHeuristic = heuristic;
                bestIndex = nextIndex;
            }
            if (nextNode.heapPosition >= 0)
            {
                open.DecreaseKey(nextIndex);
            }
            else open.Insert(nextIndex);
        };

        for (const GridPos& direction : NavigationActionLibrary::CardinalDirections())
        {
            // Existing support: walk, climb, or drop to the first supported cell.
            for (int dy = 1; dy >= -profile.maxSafeDropBlocks; --dy)
            {
                const GridPos next {
                    currentState.support.x + direction.x,
                    currentState.support.y + dy,
                    currentState.support.z + direction.z
                };
                if (!world.IsSupported(next))
                {
                    continue;
                }
                // Voxel dy=1 can mean a 1.5-block climb from a lower slab.
                // Validate actual collision surfaces against player physics,
                // rather than repeatedly executing an impossible "one block" hop.
                const float surfaceRise = world.SupportCenter(next, profile.bodyCenterAboveSupport).y
                    - world.SupportCenter(currentState.support, profile.bodyCenterAboveSupport).y;
                if (surfaceRise > static_cast<float>(profile.maxStepHeightBlocks) + 0.001f
                    && (!profile.canJump || surfaceRise > maximumJumpRise + 0.001f))
                    continue;
                if (dy > 0 && !profile.canJump && profile.maxStepHeightBlocks < dy)
                {
                    continue;
                }
                if (dy > std::max(1, profile.maxStepHeightBlocks))
                {
                    continue;
                }
                if (dy > 0)
                {
                    // Standing clearance is not enough for a jump: the body
                    // rises through one extra cell before reaching the higher
                    // support. Reject low-ceiling hops at planning time instead
                    // of feeding the executor an impossible action forever.
                    const GridPos raisedCurrent {
                        currentState.support.x,
                        currentState.support.y + dy,
                        currentState.support.z };
                    if (!query.IsBodyClear(raisedCurrent))
                    {
                        continue;
                    }
                }
                if (dy < 0)
                {
                    bool clearFall = true;
                    for (int y = currentState.support.y; y > next.y; --y)
                    {
                        const GridPos column { next.x, y, next.z };
                        if (!query.IsBodyClear(column))
                        {
                            clearFall = false;
                            break;
                        }
                    }
                    if (!clearFall) continue;
                }

                GridPos blocker {};
                const int blockerCount = query.BlockingBodyCells(next, &blocker);
                std::optional<GridPos> breakBlock;
                if (blockerCount > 0)
                {
                    if (blockerCount != 1
                        || !world.IsBreakableObstacle(blocker, profile))
                    {
                        continue;
                    }
                    breakBlock = blocker;
                }

                PlannedMovement movement;
                movement.from = currentState.support;
                movement.to = next;
                movement.plannedWorldRevision = world.WorldRevision();
                float executionRisk = 0.0f;
                if (breakBlock.has_value())
                {
                    const Block* block = world.GetBlock(*breakBlock);
                    if (block == nullptr) continue;
                    movement.type = MovementType::BreakBlock;
                    movement.affectedBlock = breakBlock;
                    const float breakSeconds = BreakSeconds(block->type, profile.pickaxeLevel);
                    movement.movementCost = profile.walkCost;
                    movement.timeCost = breakSeconds;
                    movement.resourceCost = breakSeconds * profile.breakCostPerSecond;
                    executionRisk = 0.25f;
                }
                else if (dy > 0)
                {
                    movement.type = profile.maxStepHeightBlocks >= dy
                        ? MovementType::StepUp
                        : MovementType::JumpUp;
                    movement.requiresJump = movement.type == MovementType::JumpUp;
                    movement.movementCost = movement.requiresJump ? profile.jumpCost : profile.stepCost;
                    movement.timeCost = 0.45f;
                    executionRisk = movement.requiresJump ? 0.22f : 0.05f;
                }
                else if (dy < 0)
                {
                    movement.type = MovementType::DropDown;
                    movement.movementCost = profile.dropCost + static_cast<float>(-dy - 1) * 0.22f;
                    movement.timeCost = std::sqrt(2.0f * static_cast<float>(-dy) / std::max(0.1f, profile.fallGravity));
                    executionRisk = static_cast<float>(-dy) * (1.0f - profile.riskTolerance) * 0.25f;
                }
                else
                {
                    movement.type = MovementType::Walk;
                    movement.movementCost = profile.walkCost;
                    movement.timeCost = 1.0f / std::max(0.1f, profile.moveSpeed);
                }
                query.Finalize(movement, executionRisk);
                consider(next, currentState.remainingBridgeBlocks, 0, movement);
                // First supported landing is the only sensible vertical result.
                break;
            }

            // Planned bridge segment on the same level.
            const GridPos bridgeSupport {
                currentState.support.x + direction.x,
                currentState.support.y,
                currentState.support.z + direction.z
            };
            if (profile.canBridge
                && currentState.remainingBridgeBlocks > profile.reserveBridgeBlocks
                && currentState.consecutiveBridgeBlocks < profile.maxConsecutiveBridgeBlocks
                && world.IsAir(bridgeSupport)
                && query.IsBodyClear(bridgeSupport))
            {
                PlannedMovement movement;
                movement.type = MovementType::SneakBridge;
                movement.from = currentState.support;
                movement.to = bridgeSupport;
                movement.affectedBlock = bridgeSupport;
                movement.placementBlockType = profile.preferredBridgeBlock;
                movement.requiresSneak = true;
                movement.movementCost = profile.bridgeMovementCost;
                movement.timeCost = profile.blockPlaceSeconds + 1.0f / std::max(0.1f, profile.sneakSpeed);
                movement.resourceCost = profile.BridgeScarcityCost(currentState.remainingBridgeBlocks);
                movement.plannedWorldRevision = world.WorldRevision();
                query.Finalize(movement, 0.45f + query.Exposure(bridgeSupport) * 0.18f);
                consider(
                    bridgeSupport,
                    currentState.remainingBridgeBlocks - 1,
                    currentState.consecutiveBridgeBlocks + 1,
                    movement);
            }

            // Authored training ramps and damaged vertical routes may expose a
            // side anchor one block above the actor. Place a wool step against
            // that anchor, then perform a real jump onto it. This is the
            // staircase counterpart of the same-level SneakBridge action.
            const GridPos stairSupport {
                currentState.support.x + direction.x,
                currentState.support.y + 1,
                currentState.support.z + direction.z
            };
            bool stairHasAnchor = false;
            static constexpr std::array<GridPos, 6> anchorOffsets {
                GridPos { 1, 0, 0 }, GridPos { -1, 0, 0 },
                GridPos { 0, 1, 0 }, GridPos { 0, -1, 0 },
                GridPos { 0, 0, 1 }, GridPos { 0, 0, -1 }
            };
            for (const GridPos& offset : anchorOffsets)
            {
                stairHasAnchor = stairHasAnchor || world.IsSolid(GridPos {
                    stairSupport.x + offset.x,
                    stairSupport.y + offset.y,
                    stairSupport.z + offset.z });
            }
            if (profile.canBuildStairs
                && currentState.remainingBridgeBlocks > profile.reserveBridgeBlocks
                && currentState.consecutiveBridgeBlocks < profile.maxConsecutiveBridgeBlocks
                && world.IsAir(stairSupport)
                && query.IsBodyClear(stairSupport)
                && query.IsBodyClear(GridPos {
                    currentState.support.x,
                    currentState.support.y + 1,
                    currentState.support.z })
                && stairHasAnchor)
            {
                PlannedMovement movement;
                movement.type = MovementType::SneakBridge;
                movement.from = currentState.support;
                movement.to = stairSupport;
                movement.affectedBlock = stairSupport;
                movement.placementBlockType = profile.preferredBridgeBlock;
                movement.requiresSneak = true;
                movement.requiresJump = true;
                movement.movementCost = profile.bridgeMovementCost + profile.jumpCost;
                movement.timeCost = profile.blockPlaceSeconds + 0.55f;
                movement.resourceCost = profile.BridgeScarcityCost(currentState.remainingBridgeBlocks);
                movement.plannedWorldRevision = world.WorldRevision();
                query.Finalize(movement, 0.58f + query.Exposure(stairSupport) * 0.18f);
                consider(
                    stairSupport,
                    currentState.remainingBridgeBlocks - 1,
                    currentState.consecutiveBridgeBlocks + 1,
                    movement);
            }

            // Short deterministic sprint-jump. The executor first steps back
            // onto the checked runway, then accelerates before takeoff.
            if (profile.canJump && maxExecutableGapJump > 0)
            {
                for (int gap = 1; gap <= maxExecutableGapJump; ++gap)
                {
                    bool takeoffRunwayClear = true;
                    const int runwayCells = std::max(
                        1, static_cast<int>(std::ceil(profile.gapRunupDistanceBlocks)));
                    for (int runway = 1; runway <= runwayCells; ++runway)
                    {
                        const GridPos takeoffRunway {
                            currentState.support.x - direction.x * runway,
                            currentState.support.y,
                            currentState.support.z - direction.z * runway
                        };
                        if (!world.IsSupported(takeoffRunway)
                            || !query.IsBodyClear(takeoffRunway))
                        {
                            takeoffRunwayClear = false;
                            break;
                        }
                    }
                    if (!takeoffRunwayClear
                        || query.Exposure(currentState.support) > 1.0f
                        || query.Threat(currentState.support) >= 2.0f)
                    {
                        break;
                    }
                    bool airGap = true;
                    for (int offset = 1; offset <= gap; ++offset)
                    {
                        const GridPos gapSupport {
                            currentState.support.x + direction.x * offset,
                            currentState.support.y,
                            currentState.support.z + direction.z * offset
                        };
                        if (!world.IsAir(gapSupport))
                        {
                            airGap = false;
                            break;
                        }
                    }
                    const GridPos landing {
                        currentState.support.x + direction.x * (gap + 1),
                        currentState.support.y,
                        currentState.support.z + direction.z * (gap + 1)
                    };
                    const GridPos landingRunway {
                        landing.x + direction.x,
                        landing.y,
                        landing.z + direction.z
                    };
                    if (!airGap || !world.IsSupported(landing) || !query.IsBodyClear(landing))
                    {
                        continue;
                    }
                    if (!world.IsSupported(landingRunway)
                        || !query.IsBodyClear(landingRunway)
                        || query.Exposure(landing) > 1.0f
                        || query.Threat(landing) >= 2.0f)
                    {
                        continue;
                    }
                    PlannedMovement movement;
                    movement.type = MovementType::GapJump;
                    movement.from = currentState.support;
                    movement.to = landing;
                    movement.requiresJump = true;
                    movement.requiresSprint = true;
                    movement.movementCost = profile.gapJumpCost
                        + static_cast<float>(gap) * 0.45f * profile.fallRiskPenalty;
                    movement.timeCost = static_cast<float>(gap + 1) / std::max(0.1f, profile.sprintSpeed);
                    movement.plannedWorldRevision = world.WorldRevision();
                    query.Finalize(
                        movement,
                        (0.8f + static_cast<float>(gap) * 0.7f)
                            * (1.2f - profile.riskTolerance)
                            * profile.fallRiskPenalty);
                    consider(landing, currentState.remainingBridgeBlocks, 0, movement);
                    break;
                }
            }

            // A straight, low-threat floor run is one executable action. It
            // extends the action horizon without skipping geometry: every
            // crossed support is checked now and again after a world change.
            if (profile.canSprint && profile.maxSprintRunBlocks >= 2)
            {
                for (int span = 1; span <= profile.maxSprintRunBlocks; ++span)
                {
                    const GridPos landing {
                        currentState.support.x + direction.x * span,
                        currentState.support.y,
                        currentState.support.z + direction.z * span
                    };
                    if (!world.IsSupported(landing)
                        || !query.IsBodyClear(landing)
                        || world.IsHazard(landing)
                        || !query.IsSafeSprintCell(landing))
                    {
                        break;
                    }
                    if (span < 2) continue;

                    PlannedMovement movement;
                    movement.type = MovementType::Sprint;
                    movement.from = currentState.support;
                    movement.to = landing;
                    movement.requiresSprint = true;
                    movement.traversedCells = span;
                    movement.movementCost = profile.walkCost * static_cast<float>(span);
                    movement.timeCost = static_cast<float>(span)
                        / std::max(0.1f, profile.sprintSpeed);
                    movement.plannedWorldRevision = world.WorldRevision();
                    query.FinalizeSprint(movement, direction, span, 0.03f * static_cast<float>(span - 1));
                    consider(landing, currentState.remainingBridgeBlocks, 0, movement);
                }
            }
        }

        // Safe same-level diagonals remove the Manhattan bias on broad Castle
        // floors.  When the complete corner footprint is low-threat and has
        // enough support, make the move a sprint-hop: it is materially faster
        // than treating every diagonal as a cautious, sneaking walk. Vertical,
        // bridge and parkour diagonals remain unavailable until their command
        // execution has its own calibrated primitive.
        if (profile.canMoveDiagonally)
        {
            for (const GridPos& direction : NavigationActionLibrary::DiagonalDirections())
            {
                const GridPos next {
                    currentState.support.x + direction.x,
                    currentState.support.y,
                    currentState.support.z + direction.z
                };
                const GridPos shoulderX {
                    currentState.support.x + direction.x,
                    currentState.support.y,
                    currentState.support.z
                };
                const GridPos shoulderZ {
                    currentState.support.x,
                    currentState.support.y,
                    currentState.support.z + direction.z
                };
                if (!world.IsSupported(next) || !query.IsBodyClear(next)
                    || !world.IsSupported(shoulderX) || !query.IsBodyClear(shoulderX)
                    || !world.IsSupported(shoulderZ) || !query.IsBodyClear(shoulderZ))
                {
                    continue;
                }
                PlannedMovement movement;
                const bool sprintHop = profile.canSprint
                    && query.IsSafeSprintCell(currentState.support)
                    && query.IsSafeSprintCell(next)
                    && query.IsSafeSprintCell(shoulderX)
                    && query.IsSafeSprintCell(shoulderZ);
                movement.type = sprintHop ? MovementType::Sprint : MovementType::Walk;
                movement.from = currentState.support;
                movement.to = next;
                movement.movementCost = profile.walkCost * 1.41421356f;
                movement.timeCost = 1.41421356f / std::max(
                    0.1f, sprintHop ? profile.sprintSpeed : profile.moveSpeed);
                movement.requiresSprint = sprintHop;
                // The executor emits this only while grounded.  It preserves
                // sprint momentum without turning an exposed edge traverse
                // into a blind bunny-hop.
                movement.requiresJump = sprintHop;
                movement.plannedWorldRevision = world.WorldRevision();
                if (sprintHop)
                {
                    movement.traversedCells = 1;
                    query.FinalizeSprint(movement, direction, 1, 0.06f);
                }
                else
                {
                    query.Finalize(movement, 0.04f);
                }
                consider(next, currentState.remainingBridgeBlocks, 0, movement);
            }
        }
    }

    result.path.expandedNodes = expanded;
    result.path.generatedNodes = static_cast<int>(nodes.Size());
    result.path.peakOpenNodes = static_cast<int>(open.PeakSize());
    result.path.heapDecreaseKeys = static_cast<int>(open.UpdateCount());
    result.path.corridorRejectedNodes = corridorRejected;
    int finalIndex = -1;
    if (reached)
    {
        finalIndex = reachedIndex;
        result.status = NavigationSearchStatus::Success;
        result.description = "goal reached";
    }
    else if (limits.allowPartial
        && (bestIndex != startIndex || !open.Empty()))
    {
        // A bounded search may need to move away from the Euclidean goal
        // before it can round a wall. In that case bestIndex remains the
        // start even though A* has a valid, promising frontier. Continue to
        // the lowest-score open node instead of reporting a false NoPath.
        finalIndex = bestIndex != startIndex ? bestIndex : open.PeekLowest();
        result.status = NavigationSearchStatus::Partial;
        result.path.partial = true;
        result.description = bestIndex != startIndex
            ? "bounded partial path"
            : "bounded detour partial path";
    }
    else
    {
        result.status = openLimit
            ? NavigationSearchStatus::OpenSetLimit
            : (actionLimit ? NavigationSearchStatus::ActionLimit
                           : (expanded >= limits.maxExpansions
                                ? NavigationSearchStatus::ExpansionLimit
                                : NavigationSearchStatus::NoPath));
        result.description = "no executable route";
        return result;
    }

    std::vector<PlannedMovement> reverse;
    int cursor = finalIndex;
    int guard = 0;
    while (cursor != startIndex && guard++ <= limits.maxActions)
    {
        if (cursor < 0 || nodes[cursor].parentIndex < 0)
        {
            result.status = NavigationSearchStatus::NoPath;
            result.path.movements.clear();
            result.description = "path reconstruction failed";
            return result;
        }
        reverse.push_back(nodes[cursor].movement);
        cursor = nodes[cursor].parentIndex;
    }
    std::reverse(reverse.begin(), reverse.end());
    MarkSprintRuns(reverse, query, profile);
    result.path.movements = std::move(reverse);
    result.path.resolvedGoal = nodes[finalIndex].support;
    for (const PlannedMovement& movement : result.path.movements)
    {
        result.path.totalCost += movement.totalCost;
        result.path.movementCost += movement.movementCost;
        result.path.timeCost += movement.timeCost;
        result.path.resourceCost += movement.resourceCost;
        result.path.threatCost += movement.threatCost;
    }
    return result;
}

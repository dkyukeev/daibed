#include "Network/SnapshotDelta.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace
{
bool VecEqual(const Vec3& a, const Vec3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

float DistanceSq(const Vec3& a, const Vec3& b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool InventoryEqual(const InventorySnapshot& a, const InventorySnapshot& b)
{
    if (a.present != b.present
        || a.resources != b.resources
        || a.hotbar.size() != b.hotbar.size()
        || a.main.size() != b.main.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.hotbar.size(); ++i)
    {
        if (a.hotbar[i].itemType != b.hotbar[i].itemType || a.hotbar[i].count != b.hotbar[i].count)
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.main.size(); ++i)
    {
        if (a.main[i].itemType != b.main[i].itemType || a.main[i].count != b.main[i].count)
        {
            return false;
        }
    }
    return true;
}

bool ItemSlotsEqual(const std::vector<ItemStackSnapshot>& a, const std::vector<ItemStackSnapshot>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].itemType != b[i].itemType || a[i].count != b[i].count)
        {
            return false;
        }
    }
    return true;
}

bool TeamChestEqual(const TeamChestSnapshot& a, const TeamChestSnapshot& b)
{
    return a.teamId == b.teamId
        && a.resources == b.resources
        && ItemSlotsEqual(a.slots, b.slots);
}

bool PlayerScoreEqual(const PlayerScoreSnapshot& a, const PlayerScoreSnapshot& b)
{
    return a.playerId == b.playerId
        && a.kills == b.kills
        && a.deaths == b.deaths
        && a.finalDeaths == b.finalDeaths
        && a.coreDamage == b.coreDamage
        && a.coresDestroyed == b.coresDestroyed;
}

bool PlayerEqual(const PlayerSnapshot& a, const PlayerSnapshot& b)
{
    return a.playerId == b.playerId && a.playerName == b.playerName
        && a.teamId == b.teamId && a.heroId == b.heroId
        && VecEqual(a.position, b.position) && VecEqual(a.velocity, b.velocity)
        && a.yaw == b.yaw && a.health == b.health && a.maxHealth == b.maxHealth
        && a.alive == b.alive && a.eliminated == b.eliminated
        && a.respawnTimer == b.respawnTimer && a.selectedSlot == b.selectedSlot
        && InventoryEqual(a.inventory, b.inventory)
        && a.disguiseTeamId == b.disguiseTeamId && a.disguiseHeroId == b.disguiseHeroId;
}

bool CoreEqual(const CoreSnapshot& a, const CoreSnapshot& b)
{
    return a.teamId == b.teamId && a.health == b.health && a.maxHealth == b.maxHealth
        && a.alive == b.alive;
}

bool GeneratorEqual(const GeneratorSnapshot& a, const GeneratorSnapshot& b)
{
    return a.resourceType == b.resourceType && a.teamId == b.teamId
        && VecEqual(a.position, b.position);
}

bool PickupEqual(const PickupSnapshot& a, const PickupSnapshot& b)
{
    return a.resourceType == b.resourceType && a.amount == b.amount
        && VecEqual(a.position, b.position);
}

bool DroppedItemEqual(const DroppedItemSnapshot& a, const DroppedItemSnapshot& b)
{
    return a.id == b.id && a.itemType == b.itemType && a.count == b.count
        && VecEqual(a.position, b.position) && VecEqual(a.velocity, b.velocity)
        && a.ownerPlayerId == b.ownerPlayerId
        && a.ownerPickupDelay == b.ownerPickupDelay
        && a.lifetime == b.lifetime
        && a.age == b.age;
}

bool BlockDeltaEqual(const BlockDelta& a, const BlockDelta& b)
{
    return a.tick == b.tick && a.position == b.position && a.oldType == b.oldType
        && a.newType == b.newType && a.oldTeamId == b.oldTeamId && a.newTeamId == b.newTeamId
        && a.ownerPlayerId == b.ownerPlayerId && a.reason == b.reason;
}

bool ProjectileEqual(const ProjectileSnapshot& a, const ProjectileSnapshot& b)
{
    return a.id == b.id && a.kind == b.kind && VecEqual(a.position, b.position)
        && VecEqual(a.velocity, b.velocity) && a.ownerPlayerId == b.ownerPlayerId
        && a.ownerTeamId == b.ownerTeamId && a.remainingLifetime == b.remainingLifetime
        && a.fireZone == b.fireZone && a.visibility == b.visibility;
}

bool ExplosiveEqual(const ExplosiveSnapshot& a, const ExplosiveSnapshot& b)
{
    return a.id == b.id && VecEqual(a.position, b.position)
        && a.ownerPlayerId == b.ownerPlayerId && a.ownerTeamId == b.ownerTeamId
        && a.remainingTimer == b.remainingTimer && a.radius == b.radius
        && a.visibility == b.visibility;
}

bool HazardZoneEqual(const HazardZoneSnapshot& a, const HazardZoneSnapshot& b)
{
    return a.id == b.id && VecEqual(a.position, b.position)
        && a.ownerPlayerId == b.ownerPlayerId && a.ownerTeamId == b.ownerTeamId
        && a.remainingLifetime == b.remainingLifetime && a.radius == b.radius
        && a.blueFire == b.blueFire && a.visibility == b.visibility;
}

bool HeroDeviceEqual(const HeroDeviceSnapshot& a, const HeroDeviceSnapshot& b)
{
    return a.id == b.id && a.type == b.type && VecEqual(a.position, b.position)
        && a.ownerPlayerId == b.ownerPlayerId && a.ownerTeamId == b.ownerTeamId
        && a.targetPlayerId == b.targetPlayerId && a.remainingLifetime == b.remainingLifetime
        && a.health == b.health && a.visibility == b.visibility;
}

bool StatusEffectEqual(const StatusEffectSnapshot& a, const StatusEffectSnapshot& b)
{
    return a.id == b.id && a.type == b.type && VecEqual(a.position, b.position)
        && a.targetPlayerId == b.targetPlayerId && a.ownerPlayerId == b.ownerPlayerId
        && a.ownerTeamId == b.ownerTeamId && a.remaining == b.remaining
        && a.amount == b.amount && a.visibility == b.visibility;
}

const PlayerSnapshot* FindPlayer(const std::vector<PlayerSnapshot>& players, int playerId)
{
    for (const PlayerSnapshot& player : players)
    {
        if (player.playerId == playerId)
        {
            return &player;
        }
    }
    return nullptr;
}

const CoreSnapshot* FindCore(const std::vector<CoreSnapshot>& cores, int teamId)
{
    for (const CoreSnapshot& core : cores)
    {
        if (core.teamId == teamId)
        {
            return &core;
        }
    }
    return nullptr;
}

const TeamChestSnapshot* FindTeamChest(const std::vector<TeamChestSnapshot>& chests, int teamId)
{
    for (const TeamChestSnapshot& chest : chests)
    {
        if (chest.teamId == teamId)
        {
            return &chest;
        }
    }
    return nullptr;
}

const PlayerScoreSnapshot* FindPlayerScore(const std::vector<PlayerScoreSnapshot>& scores, int playerId)
{
    for (const PlayerScoreSnapshot& score : scores)
    {
        if (score.playerId == playerId)
        {
            return &score;
        }
    }
    return nullptr;
}

template <typename T>
const T* FindById(const std::vector<T>& values, int id)
{
    for (const T& value : values)
    {
        if (value.id == id)
        {
            return &value;
        }
    }
    return nullptr;
}

template <typename T, typename EqualFn>
void BuildIndexedDelta(
    const std::vector<T>& baseline,
    const std::vector<T>& current,
    std::vector<std::uint32_t>& removed,
    std::vector<std::pair<std::uint32_t, T>>& changed,
    EqualFn equal)
{
    if (baseline.size() > current.size())
    {
        for (std::size_t i = current.size(); i < baseline.size(); ++i)
        {
            removed.push_back(static_cast<std::uint32_t>(i));
        }
    }
    const std::size_t maxCommon = std::min(baseline.size(), current.size());
    for (std::size_t i = 0; i < maxCommon; ++i)
    {
        if (!equal(baseline[i], current[i]))
        {
            changed.push_back({ static_cast<std::uint32_t>(i), current[i] });
        }
    }
    for (std::size_t i = maxCommon; i < current.size(); ++i)
    {
        changed.push_back({ static_cast<std::uint32_t>(i), current[i] });
    }
}

template <typename T>
bool ApplyIndexedChanges(
    std::vector<T>& target,
    const std::vector<std::uint32_t>& removed,
    const std::vector<std::pair<std::uint32_t, T>>& changed)
{
    std::vector<std::uint32_t> sortedRemoved = removed;
    std::sort(sortedRemoved.begin(), sortedRemoved.end(), std::greater<std::uint32_t>());
    sortedRemoved.erase(std::unique(sortedRemoved.begin(), sortedRemoved.end()), sortedRemoved.end());
    for (std::uint32_t index : sortedRemoved)
    {
        if (index >= target.size())
        {
            return false;
        }
        target.erase(target.begin() + static_cast<std::ptrdiff_t>(index));
    }

    for (const auto& entry : changed)
    {
        const std::uint32_t index = entry.first;
        if (index > target.size())
        {
            return false;
        }
        if (index == target.size())
        {
            target.push_back(entry.second);
        }
        else
        {
            target[index] = entry.second;
        }
    }
    return true;
}

template <typename T>
void RemoveIds(std::vector<T>& target, const std::vector<int>& ids)
{
    target.erase(
        std::remove_if(target.begin(), target.end(),
            [&ids](const T& value)
            {
                return std::find(ids.begin(), ids.end(), value.id) != ids.end();
            }),
        target.end());
}

void RemovePlayers(std::vector<PlayerSnapshot>& target, const std::vector<int>& ids)
{
    target.erase(
        std::remove_if(target.begin(), target.end(),
            [&ids](const PlayerSnapshot& value)
            {
                return std::find(ids.begin(), ids.end(), value.playerId) != ids.end();
            }),
        target.end());
}

void RemoveCores(std::vector<CoreSnapshot>& target, const std::vector<int>& teamIds)
{
    target.erase(
        std::remove_if(target.begin(), target.end(),
            [&teamIds](const CoreSnapshot& value)
            {
                return std::find(teamIds.begin(), teamIds.end(), value.teamId) != teamIds.end();
            }),
        target.end());
}

void RemoveTeamChests(std::vector<TeamChestSnapshot>& target, const std::vector<int>& teamIds)
{
    target.erase(
        std::remove_if(target.begin(), target.end(),
            [&teamIds](const TeamChestSnapshot& value)
            {
                return std::find(teamIds.begin(), teamIds.end(), value.teamId) != teamIds.end();
            }),
        target.end());
}

void RemovePlayerScores(std::vector<PlayerScoreSnapshot>& target, const std::vector<int>& playerIds)
{
    target.erase(
        std::remove_if(target.begin(), target.end(),
            [&playerIds](const PlayerScoreSnapshot& value)
            {
                return std::find(playerIds.begin(), playerIds.end(), value.playerId) != playerIds.end();
            }),
        target.end());
}

template <typename T>
void UpsertById(std::vector<T>& target, const T& value)
{
    for (T& existing : target)
    {
        if (existing.id == value.id)
        {
            existing = value;
            return;
        }
    }
    target.push_back(value);
}

void UpsertPlayer(std::vector<PlayerSnapshot>& target, const PlayerSnapshot& value)
{
    for (PlayerSnapshot& existing : target)
    {
        if (existing.playerId == value.playerId)
        {
            existing = value;
            return;
        }
    }
    target.push_back(value);
}

void UpsertCore(std::vector<CoreSnapshot>& target, const CoreSnapshot& value)
{
    for (CoreSnapshot& existing : target)
    {
        if (existing.teamId == value.teamId)
        {
            existing = value;
            return;
        }
    }
    target.push_back(value);
}

void UpsertTeamChest(std::vector<TeamChestSnapshot>& target, const TeamChestSnapshot& value)
{
    for (TeamChestSnapshot& existing : target)
    {
        if (existing.teamId == value.teamId)
        {
            existing = value;
            return;
        }
    }
    target.push_back(value);
}

void UpsertPlayerScore(std::vector<PlayerScoreSnapshot>& target, const PlayerScoreSnapshot& value)
{
    for (PlayerScoreSnapshot& existing : target)
    {
        if (existing.playerId == value.playerId)
        {
            existing = value;
            return;
        }
    }
    target.push_back(value);
}

template <typename T>
void SortByPriorityPosition(std::vector<T>& values, Vec3 priorityPosition)
{
    std::stable_sort(
        values.begin(),
        values.end(),
        [priorityPosition](const T& a, const T& b)
        {
            return DistanceSq(a.position, priorityPosition) < DistanceSq(b.position, priorityPosition);
        });
}
} // namespace

const char* ToString(SnapshotDeltaApplyStatus status)
{
    switch (status)
    {
    case SnapshotDeltaApplyStatus::Applied:
        return "Applied";
    case SnapshotDeltaApplyStatus::OldSnapshot:
        return "OldSnapshot";
    case SnapshotDeltaApplyStatus::MissingBaseline:
        return "MissingBaseline";
    case SnapshotDeltaApplyStatus::BaselineMismatch:
        return "BaselineMismatch";
    case SnapshotDeltaApplyStatus::BadDelta:
        return "BadDelta";
    }
    return "Unknown";
}

bool SnapshotDeltaStatusNeedsFullResync(SnapshotDeltaApplyStatus status)
{
    return status == SnapshotDeltaApplyStatus::MissingBaseline
        || status == SnapshotDeltaApplyStatus::BaselineMismatch
        || status == SnapshotDeltaApplyStatus::BadDelta;
}

MatchSnapshotDelta BuildSnapshotDelta(
    const MatchSnapshot& baseline,
    const MatchSnapshot& current,
    std::uint32_t baselineSequence,
    int priorityPlayerId)
{
    MatchSnapshotDelta delta;
    delta.tick = current.tick;
    delta.baselineTick = baseline.tick;
    delta.baselineSequence = baselineSequence;
    delta.lastProcessedCommandTick = current.lastProcessedCommandTick;
    delta.matchTime = current.matchTime;
    delta.phase = current.phase;
    delta.winnerTeamId = current.winnerTeamId;

    Vec3 priorityPosition {};
    bool hasPriorityPosition = false;
    if (const PlayerSnapshot* priority = FindPlayer(current.players, priorityPlayerId))
    {
        priorityPosition = priority->position;
        hasPriorityPosition = true;
    }

    for (const PlayerSnapshot& player : current.players)
    {
        const PlayerSnapshot* old = FindPlayer(baseline.players, player.playerId);
        if (old == nullptr || !PlayerEqual(*old, player))
        {
            delta.players.push_back(player);
        }
    }
    for (const PlayerSnapshot& player : baseline.players)
    {
        if (FindPlayer(current.players, player.playerId) == nullptr)
        {
            delta.removedPlayerIds.push_back(player.playerId);
        }
    }
    if (priorityPlayerId >= 0)
    {
        std::stable_sort(
            delta.players.begin(),
            delta.players.end(),
            [priorityPlayerId, priorityPosition, hasPriorityPosition](const PlayerSnapshot& a, const PlayerSnapshot& b)
            {
                if (a.playerId == priorityPlayerId || b.playerId == priorityPlayerId)
                {
                    return a.playerId == priorityPlayerId;
                }
                if (hasPriorityPosition)
                {
                    return DistanceSq(a.position, priorityPosition) < DistanceSq(b.position, priorityPosition);
                }
                return a.playerId < b.playerId;
            });
    }

    for (const PlayerScoreSnapshot& score : current.matchScores)
    {
        const PlayerScoreSnapshot* old = FindPlayerScore(baseline.matchScores, score.playerId);
        if (old == nullptr || !PlayerScoreEqual(*old, score))
        {
            delta.matchScores.push_back(score);
        }
    }
    for (const PlayerScoreSnapshot& score : baseline.matchScores)
    {
        if (FindPlayerScore(current.matchScores, score.playerId) == nullptr)
        {
            delta.removedScorePlayerIds.push_back(score.playerId);
        }
    }

    for (const CoreSnapshot& core : current.cores)
    {
        const CoreSnapshot* old = FindCore(baseline.cores, core.teamId);
        if (old == nullptr || !CoreEqual(*old, core))
        {
            delta.cores.push_back(core);
        }
    }
    for (const CoreSnapshot& core : baseline.cores)
    {
        if (FindCore(current.cores, core.teamId) == nullptr)
        {
            delta.removedCoreTeamIds.push_back(core.teamId);
        }
    }

    for (const TeamChestSnapshot& chest : current.teamChests)
    {
        const TeamChestSnapshot* old = FindTeamChest(baseline.teamChests, chest.teamId);
        if (old == nullptr || !TeamChestEqual(*old, chest))
        {
            delta.teamChests.push_back(chest);
        }
    }
    for (const TeamChestSnapshot& chest : baseline.teamChests)
    {
        if (FindTeamChest(current.teamChests, chest.teamId) == nullptr)
        {
            delta.removedTeamChestTeamIds.push_back(chest.teamId);
        }
    }

    std::vector<std::pair<std::uint32_t, GeneratorSnapshot>> changedGenerators;
    BuildIndexedDelta(baseline.generators, current.generators, delta.removedGeneratorIndices,
                      changedGenerators, GeneratorEqual);
    for (const auto& entry : changedGenerators)
    {
        delta.generators.push_back(IndexedGeneratorSnapshot { entry.first, entry.second });
    }

    std::vector<std::pair<std::uint32_t, PickupSnapshot>> changedPickups;
    BuildIndexedDelta(baseline.pickups, current.pickups, delta.removedPickupIndices,
                      changedPickups, PickupEqual);
    for (const auto& entry : changedPickups)
    {
        delta.pickups.push_back(IndexedPickupSnapshot { entry.first, entry.second });
    }

    std::vector<std::pair<std::uint32_t, DroppedItemSnapshot>> changedDropped;
    BuildIndexedDelta(baseline.droppedItems, current.droppedItems, delta.removedDroppedItemIndices,
                      changedDropped, DroppedItemEqual);
    for (const auto& entry : changedDropped)
    {
        delta.droppedItems.push_back(IndexedDroppedItemSnapshot { entry.first, entry.second });
    }

    for (const BlockDelta& block : current.blockDeltas)
    {
        const auto found = std::find_if(
            baseline.blockDeltas.begin(),
            baseline.blockDeltas.end(),
            [&block](const BlockDelta& old) { return BlockDeltaEqual(old, block); });
        if (found == baseline.blockDeltas.end())
        {
            delta.blockDeltas.push_back(block);
        }
    }

    for (const ProjectileSnapshot& value : current.projectiles)
    {
        const ProjectileSnapshot* old = FindById(baseline.projectiles, value.id);
        if (old == nullptr || !ProjectileEqual(*old, value))
        {
            delta.projectiles.push_back(value);
        }
    }
    for (const ProjectileSnapshot& value : baseline.projectiles)
    {
        if (FindById(current.projectiles, value.id) == nullptr)
        {
            delta.removedProjectileIds.push_back(value.id);
        }
    }
    if (hasPriorityPosition)
    {
        SortByPriorityPosition(delta.projectiles, priorityPosition);
    }

    for (const ExplosiveSnapshot& value : current.explosives)
    {
        const ExplosiveSnapshot* old = FindById(baseline.explosives, value.id);
        if (old == nullptr || !ExplosiveEqual(*old, value))
        {
            delta.explosives.push_back(value);
        }
    }
    for (const ExplosiveSnapshot& value : baseline.explosives)
    {
        if (FindById(current.explosives, value.id) == nullptr)
        {
            delta.removedExplosiveIds.push_back(value.id);
        }
    }
    if (hasPriorityPosition)
    {
        SortByPriorityPosition(delta.explosives, priorityPosition);
    }

    for (const HazardZoneSnapshot& value : current.hazardZones)
    {
        const HazardZoneSnapshot* old = FindById(baseline.hazardZones, value.id);
        if (old == nullptr || !HazardZoneEqual(*old, value))
        {
            delta.hazardZones.push_back(value);
        }
    }
    for (const HazardZoneSnapshot& value : baseline.hazardZones)
    {
        if (FindById(current.hazardZones, value.id) == nullptr)
        {
            delta.removedHazardZoneIds.push_back(value.id);
        }
    }
    if (hasPriorityPosition)
    {
        SortByPriorityPosition(delta.hazardZones, priorityPosition);
    }

    for (const HeroDeviceSnapshot& value : current.heroDevices)
    {
        const HeroDeviceSnapshot* old = FindById(baseline.heroDevices, value.id);
        if (old == nullptr || !HeroDeviceEqual(*old, value))
        {
            delta.heroDevices.push_back(value);
        }
    }
    for (const HeroDeviceSnapshot& value : baseline.heroDevices)
    {
        if (FindById(current.heroDevices, value.id) == nullptr)
        {
            delta.removedHeroDeviceIds.push_back(value.id);
        }
    }
    if (hasPriorityPosition)
    {
        SortByPriorityPosition(delta.heroDevices, priorityPosition);
    }

    for (const StatusEffectSnapshot& value : current.statusEffects)
    {
        const StatusEffectSnapshot* old = FindById(baseline.statusEffects, value.id);
        if (old == nullptr || !StatusEffectEqual(*old, value))
        {
            delta.statusEffects.push_back(value);
        }
    }
    for (const StatusEffectSnapshot& value : baseline.statusEffects)
    {
        if (FindById(current.statusEffects, value.id) == nullptr)
        {
            delta.removedStatusEffectIds.push_back(value.id);
        }
    }
    if (hasPriorityPosition)
    {
        SortByPriorityPosition(delta.statusEffects, priorityPosition);
    }

    delta.actionResults = current.actionResults;
    delta.worldEvents = current.worldEvents;
    return delta;
}

bool HasSnapshotDeltaChanges(const MatchSnapshotDelta& delta)
{
    return delta.tick != delta.baselineTick
        || !delta.players.empty() || !delta.removedPlayerIds.empty()
        || !delta.matchScores.empty() || !delta.removedScorePlayerIds.empty()
        || !delta.cores.empty() || !delta.removedCoreTeamIds.empty()
        || !delta.teamChests.empty() || !delta.removedTeamChestTeamIds.empty()
        || !delta.generators.empty() || !delta.removedGeneratorIndices.empty()
        || !delta.pickups.empty() || !delta.removedPickupIndices.empty()
        || !delta.droppedItems.empty() || !delta.removedDroppedItemIndices.empty()
        || !delta.blockDeltas.empty()
        || !delta.projectiles.empty() || !delta.removedProjectileIds.empty()
        || !delta.explosives.empty() || !delta.removedExplosiveIds.empty()
        || !delta.hazardZones.empty() || !delta.removedHazardZoneIds.empty()
        || !delta.heroDevices.empty() || !delta.removedHeroDeviceIds.empty()
        || !delta.statusEffects.empty() || !delta.removedStatusEffectIds.empty()
        || !delta.actionResults.empty()
        || !delta.worldEvents.empty();
}

bool ApplySnapshotDelta(MatchSnapshot& baseline, const MatchSnapshotDelta& delta)
{
    if (baseline.tick != delta.baselineTick)
    {
        return false;
    }

    RemovePlayers(baseline.players, delta.removedPlayerIds);
    for (const PlayerSnapshot& value : delta.players)
    {
        UpsertPlayer(baseline.players, value);
    }

    RemovePlayerScores(baseline.matchScores, delta.removedScorePlayerIds);
    for (const PlayerScoreSnapshot& value : delta.matchScores)
    {
        UpsertPlayerScore(baseline.matchScores, value);
    }

    RemoveCores(baseline.cores, delta.removedCoreTeamIds);
    for (const CoreSnapshot& value : delta.cores)
    {
        UpsertCore(baseline.cores, value);
    }

    RemoveTeamChests(baseline.teamChests, delta.removedTeamChestTeamIds);
    for (const TeamChestSnapshot& value : delta.teamChests)
    {
        UpsertTeamChest(baseline.teamChests, value);
    }

    std::vector<std::pair<std::uint32_t, GeneratorSnapshot>> generators;
    for (const IndexedGeneratorSnapshot& entry : delta.generators)
    {
        generators.push_back({ entry.index, entry.value });
    }
    if (!ApplyIndexedChanges(baseline.generators, delta.removedGeneratorIndices, generators))
    {
        return false;
    }

    std::vector<std::pair<std::uint32_t, PickupSnapshot>> pickups;
    for (const IndexedPickupSnapshot& entry : delta.pickups)
    {
        pickups.push_back({ entry.index, entry.value });
    }
    if (!ApplyIndexedChanges(baseline.pickups, delta.removedPickupIndices, pickups))
    {
        return false;
    }

    std::vector<std::pair<std::uint32_t, DroppedItemSnapshot>> dropped;
    for (const IndexedDroppedItemSnapshot& entry : delta.droppedItems)
    {
        dropped.push_back({ entry.index, entry.value });
    }
    if (!ApplyIndexedChanges(baseline.droppedItems, delta.removedDroppedItemIndices, dropped))
    {
        return false;
    }

    baseline.blockDeltas.insert(
        baseline.blockDeltas.end(),
        delta.blockDeltas.begin(),
        delta.blockDeltas.end());

    RemoveIds(baseline.projectiles, delta.removedProjectileIds);
    for (const ProjectileSnapshot& value : delta.projectiles)
    {
        UpsertById(baseline.projectiles, value);
    }
    RemoveIds(baseline.explosives, delta.removedExplosiveIds);
    for (const ExplosiveSnapshot& value : delta.explosives)
    {
        UpsertById(baseline.explosives, value);
    }
    RemoveIds(baseline.hazardZones, delta.removedHazardZoneIds);
    for (const HazardZoneSnapshot& value : delta.hazardZones)
    {
        UpsertById(baseline.hazardZones, value);
    }
    RemoveIds(baseline.heroDevices, delta.removedHeroDeviceIds);
    for (const HeroDeviceSnapshot& value : delta.heroDevices)
    {
        UpsertById(baseline.heroDevices, value);
    }
    RemoveIds(baseline.statusEffects, delta.removedStatusEffectIds);
    for (const StatusEffectSnapshot& value : delta.statusEffects)
    {
        UpsertById(baseline.statusEffects, value);
    }
    baseline.actionResults = delta.actionResults;
    baseline.worldEvents = delta.worldEvents;

    baseline.tick = delta.tick;
    baseline.lastProcessedCommandTick = delta.lastProcessedCommandTick;
    baseline.matchTime = delta.matchTime;
    baseline.phase = delta.phase;
    baseline.winnerTeamId = delta.winnerTeamId;
    return true;
}

SnapshotDeltaApplyStatus ApplySnapshotDeltaIfCompatible(
    bool hasBaseline,
    MatchSnapshot& baseline,
    std::uint32_t& baselineSequence,
    std::uint32_t packetSequence,
    const MatchSnapshotDelta& delta)
{
    if (hasBaseline && packetSequence <= baselineSequence)
    {
        return SnapshotDeltaApplyStatus::OldSnapshot;
    }
    if (!hasBaseline)
    {
        return SnapshotDeltaApplyStatus::MissingBaseline;
    }
    if (delta.baselineTick != baseline.tick || delta.baselineSequence != baselineSequence)
    {
        return SnapshotDeltaApplyStatus::BaselineMismatch;
    }
    if (!ApplySnapshotDelta(baseline, delta))
    {
        return SnapshotDeltaApplyStatus::BadDelta;
    }
    baselineSequence = packetSequence;
    return SnapshotDeltaApplyStatus::Applied;
}

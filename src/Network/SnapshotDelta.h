#pragma once

#include "Network/NetworkSnapshot.h"

#include <cstdint>
#include <vector>

struct IndexedGeneratorSnapshot
{
    std::uint32_t index = 0;
    GeneratorSnapshot value;
};

struct IndexedPickupSnapshot
{
    std::uint32_t index = 0;
    PickupSnapshot value;
};

struct IndexedDroppedItemSnapshot
{
    std::uint32_t index = 0;
    DroppedItemSnapshot value;
};

struct MatchSnapshotDelta
{
    std::uint32_t tick = 0;
    std::uint32_t baselineTick = 0;
    std::uint32_t baselineSequence = 0;
    std::uint32_t lastProcessedCommandTick = 0;
    float matchTime = 0.0f;
    MatchPhase phase = MatchPhase::Lobby;
    int winnerTeamId = -1;

    std::vector<PlayerSnapshot> players;
    std::vector<int> removedPlayerIds;
    std::vector<CoreSnapshot> cores;
    std::vector<int> removedCoreTeamIds;

    std::vector<IndexedGeneratorSnapshot> generators;
    std::vector<std::uint32_t> removedGeneratorIndices;
    std::vector<IndexedPickupSnapshot> pickups;
    std::vector<std::uint32_t> removedPickupIndices;
    std::vector<IndexedDroppedItemSnapshot> droppedItems;
    std::vector<std::uint32_t> removedDroppedItemIndices;

    std::vector<BlockDelta> blockDeltas;

    std::vector<ProjectileSnapshot> projectiles;
    std::vector<int> removedProjectileIds;
    std::vector<ExplosiveSnapshot> explosives;
    std::vector<int> removedExplosiveIds;
    std::vector<HazardZoneSnapshot> hazardZones;
    std::vector<int> removedHazardZoneIds;
    std::vector<HeroDeviceSnapshot> heroDevices;
    std::vector<int> removedHeroDeviceIds;
    std::vector<StatusEffectSnapshot> statusEffects;
    std::vector<int> removedStatusEffectIds;
};

enum class SnapshotDeltaApplyStatus
{
    Applied,
    OldSnapshot,
    MissingBaseline,
    BaselineMismatch,
    BadDelta
};

const char* ToString(SnapshotDeltaApplyStatus status);
bool SnapshotDeltaStatusNeedsFullResync(SnapshotDeltaApplyStatus status);

MatchSnapshotDelta BuildSnapshotDelta(
    const MatchSnapshot& baseline,
    const MatchSnapshot& current,
    std::uint32_t baselineSequence,
    int priorityPlayerId = -1);

bool HasSnapshotDeltaChanges(const MatchSnapshotDelta& delta);
bool ApplySnapshotDelta(MatchSnapshot& baseline, const MatchSnapshotDelta& delta);

SnapshotDeltaApplyStatus ApplySnapshotDeltaIfCompatible(
    bool hasBaseline,
    MatchSnapshot& baseline,
    std::uint32_t& baselineSequence,
    std::uint32_t packetSequence,
    const MatchSnapshotDelta& delta);

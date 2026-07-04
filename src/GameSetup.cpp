#include "Game.h"
#include "VecConvert.h"

#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace
{
constexpr float kPi = 3.1415926535f;

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float YawForTeam(int teamId)
{
    switch (teamId)
    {
    case 0:
        return kPi * 0.5f;
    case 1:
        return -kPi * 0.5f;
    case 2:
        return kPi;
    case 3:
        return 0.0f;
    default:
        return 0.0f;
    }
}

void PlaceMapBlock(World& world, GridPos pos, BlockType type, int teamId = -1, bool breakable = false)
{
    world.PlaceBlock(pos, Block { type, teamId, breakable }, true);
}

void AddOvalLayer(World& world, Vector3 center, int radiusX, int radiusZ, int y, BlockType type, bool replace = true)
{
    const int cx = static_cast<int>(std::round(center.x));
    const int cz = static_cast<int>(std::round(center.z));
    const float rx = static_cast<float>(std::max(1, radiusX));
    const float rz = static_cast<float>(std::max(1, radiusZ));

    for (int x = -radiusX; x <= radiusX; ++x)
    {
        for (int z = -radiusZ; z <= radiusZ; ++z)
        {
            const float nx = static_cast<float>(x) / rx;
            const float nz = static_cast<float>(z) / rz;
            const GridPos pos { cx + x, y, cz + z };
            if (nx * nx + nz * nz <= 1.0f && (replace || world.IsAir(pos)))
            {
                PlaceMapBlock(world, pos, type);
            }
        }
    }
}

void AddClassicIsland(World& world, Vector3 center, int radiusX, int radiusZ, int y = 0)
{
    AddOvalLayer(world, center, radiusX, radiusZ, y, BlockType::GrassBlock);
    AddOvalLayer(world, center, std::max(1, radiusX - 1), std::max(1, radiusZ - 1), y - 1, BlockType::DirtBlock);
    AddOvalLayer(world, center, std::max(1, radiusX - 3), std::max(1, radiusZ - 3), y - 2, BlockType::DirtBlock);
}

void AddTree(World& world, GridPos base)
{
    for (int y = base.y; y <= base.y + 2; ++y)
    {
        PlaceMapBlock(world, GridPos { base.x, y, base.z }, BlockType::WoodBlock);
    }

    for (int x = -1; x <= 1; ++x)
    {
        for (int z = -1; z <= 1; ++z)
        {
            PlaceMapBlock(world, GridPos { base.x + x, base.y + 3, base.z + z }, BlockType::LeafBlock);
            if (std::abs(x) + std::abs(z) <= 1)
            {
                PlaceMapBlock(world, GridPos { base.x + x, base.y + 4, base.z + z }, BlockType::LeafBlock);
            }
        }
    }
}

void AddWoodDock(World& world, GridPos start, int length, int stepX, int stepZ)
{
    for (int i = 0; i < length; ++i)
    {
        PlaceMapBlock(
            world,
            GridPos { start.x + stepX * i, start.y, start.z + stepZ * i },
            BlockType::WoodBlock);
    }
}

void AddRectLayer(World& world, GridPos center, int halfX, int halfZ, BlockType type, bool breakable = false)
{
    for (int x = -halfX; x <= halfX; ++x)
    {
        for (int z = -halfZ; z <= halfZ; ++z)
        {
            PlaceMapBlock(world, GridPos { center.x + x, center.y, center.z + z }, type, -1, breakable);
        }
    }
}

void AddLineBridge(World& world, GridPos from, GridPos to, int width, BlockType type, bool breakable = false)
{
    const int dx = (to.x > from.x) ? 1 : (to.x < from.x ? -1 : 0);
    const int dz = (to.z > from.z) ? 1 : (to.z < from.z ? -1 : 0);
    const int steps = std::max(std::abs(to.x - from.x), std::abs(to.z - from.z));
    const bool xMajor = std::abs(to.x - from.x) >= std::abs(to.z - from.z);

    for (int i = 0; i <= steps; ++i)
    {
        const int x = from.x + dx * std::min(i, std::abs(to.x - from.x));
        const int z = from.z + dz * std::min(i, std::abs(to.z - from.z));
        const int sideMin = -width / 2;
        const int sideMax = width - 1 + sideMin;
        for (int side = sideMin; side <= sideMax; ++side)
        {
            const GridPos pos {
                x + (xMajor ? 0 : side),
                from.y,
                z + (xMajor ? side : 0)
            };
            PlaceMapBlock(world, pos, type, -1, breakable);
        }
    }
}

void AddSteppedBridge(World& world, GridPos from, GridPos to, int width, BlockType type, bool breakable = false)
{
    const int dx = (to.x > from.x) ? 1 : (to.x < from.x ? -1 : 0);
    const int dz = (to.z > from.z) ? 1 : (to.z < from.z ? -1 : 0);
    const int steps = std::max(std::abs(to.x - from.x), std::abs(to.z - from.z));
    const bool xMajor = std::abs(to.x - from.x) >= std::abs(to.z - from.z);

    for (int i = 0; i <= steps; ++i)
    {
        const float t = steps > 0 ? static_cast<float>(i) / static_cast<float>(steps) : 0.0f;
        const int y = static_cast<int>(std::round(static_cast<float>(from.y) + static_cast<float>(to.y - from.y) * t));
        const int x = from.x + dx * std::min(i, std::abs(to.x - from.x));
        const int z = from.z + dz * std::min(i, std::abs(to.z - from.z));
        const int sideMin = -width / 2;
        const int sideMax = width - 1 + sideMin;
        for (int side = sideMin; side <= sideMax; ++side)
        {
            const GridPos pos {
                x + (xMajor ? 0 : side),
                y,
                z + (xMajor ? side : 0)
            };
            PlaceMapBlock(world, pos, type, -1, breakable);
        }
    }
}

void AddSquareRing(World& world, GridPos center, int innerRadius, int outerRadius, BlockType type, bool breakable = false)
{
    for (int x = -outerRadius; x <= outerRadius; ++x)
    {
        for (int z = -outerRadius; z <= outerRadius; ++z)
        {
            const int radius = std::max(std::abs(x), std::abs(z));
            if (radius >= innerRadius && radius <= outerRadius)
            {
                PlaceMapBlock(world, GridPos { center.x + x, center.y, center.z + z }, type, -1, breakable);
            }
        }
    }
}

void AddCenterMonument(World& world)
{
    AddOvalLayer(world, Vector3 { 0.0f, 0.0f, 0.0f }, 4, 4, 1, BlockType::GrassBlock);
    AddOvalLayer(world, Vector3 { 0.0f, 0.0f, 0.0f }, 2, 2, 2, BlockType::StoneBlock);

    const GridPos columns[] {
        GridPos { -2, 1, -2 },
        GridPos { 2, 1, -2 },
        GridPos { -2, 1, 2 },
        GridPos { 2, 1, 2 }
    };
    for (const GridPos& column : columns)
    {
        for (int y = 1; y <= 4; ++y)
        {
            PlaceMapBlock(world, GridPos { column.x, y, column.z }, BlockType::StoneBlock);
        }
    }

    PlaceMapBlock(world, GridPos { 0, 3, 0 }, BlockType::EnergyGlassBlock);
}

float PointSegmentDistanceSquared(Vector3 point, Vector3 start, Vector3 end)
{
    const Vector3 segment { end.x - start.x, end.y - start.y, end.z - start.z };
    const Vector3 offset { point.x - start.x, point.y - start.y, point.z - start.z };
    const float lengthSq = segment.x * segment.x + segment.y * segment.y + segment.z * segment.z;
    const float t = lengthSq > 0.000001f
        ? std::clamp((offset.x * segment.x + offset.y * segment.y + offset.z * segment.z) / lengthSq, 0.0f, 1.0f)
        : 0.0f;
    return DistanceSquared(point, Vector3 { start.x + segment.x * t, start.y + segment.y * t, start.z + segment.z * t });
}

void AddColumn(World& world, GridPos base, int height, BlockType type, int teamId = -1)
{
    for (int y = 0; y < height; ++y)
    {
        PlaceMapBlock(world, GridPos { base.x, base.y + y, base.z }, type, teamId);
    }
}

void AddClassicTeamStripe(World& world, GridPos center, int dirX, int dirZ, int teamId)
{
    const bool xAxis = dirX != 0;
    for (int forward = 0; forward <= 4; ++forward)
    {
        const int halfWidth = forward < 2 ? 2 : 1;
        for (int side = -halfWidth; side <= halfWidth; ++side)
        {
            const GridPos pos {
                center.x + dirX * forward + (xAxis ? 0 : side),
                center.y,
                center.z + dirZ * forward + (xAxis ? side : 0)
            };
            PlaceMapBlock(world, pos, BlockType::WoolBlock, teamId);
        }
    }
}

void AddHypixelStyleBase(World& world, GridPos center, bool xAxis, int teamId)
{
    AddClassicIsland(
        world,
        Vector3 {
            static_cast<float>(center.x),
            static_cast<float>(center.y),
            static_cast<float>(center.z)
        },
        xAxis ? 11 : 8,
        xAxis ? 8 : 11);

    AddRectLayer(world, center, xAxis ? 5 : 4, xAxis ? 4 : 5, BlockType::WoodBlock);
    AddRectLayer(world, GridPos { center.x, center.y, center.z }, xAxis ? 2 : 1, xAxis ? 1 : 2, BlockType::StoneBlock);

    const int dirX = center.x < 0 ? 1 : (center.x > 0 ? -1 : 0);
    const int dirZ = center.z < 0 ? 1 : (center.z > 0 ? -1 : 0);
    AddClassicTeamStripe(world, GridPos { center.x + dirX * 4, center.y, center.z + dirZ * 4 }, dirX, dirZ, teamId);

    const int sideA = xAxis ? center.z - 6 : center.x - 6;
    const int sideB = xAxis ? center.z + 6 : center.x + 6;
    const int back = xAxis ? center.x - dirX * 6 : center.z - dirZ * 6;
    const GridPos pillarA {
        xAxis ? back : sideA,
        center.y + 1,
        xAxis ? sideA : back
    };
    const GridPos pillarB {
        xAxis ? back : sideB,
        center.y + 1,
        xAxis ? sideB : back
    };
    AddColumn(world, pillarA, 3, BlockType::StoneBlock, teamId);
    AddColumn(world, pillarB, 3, BlockType::StoneBlock, teamId);
    PlaceMapBlock(world, GridPos { pillarA.x, pillarA.y + 3, pillarA.z }, BlockType::EnergyGlassBlock, teamId);
    PlaceMapBlock(world, GridPos { pillarB.x, pillarB.y + 3, pillarB.z }, BlockType::EnergyGlassBlock, teamId);
}

void AddHypixelStyleResourceIsland(World& world, GridPos center, bool xAxis)
{
    AddClassicIsland(
        world,
        Vector3 {
            static_cast<float>(center.x),
            static_cast<float>(center.y),
            static_cast<float>(center.z)
        },
        xAxis ? 4 : 5,
        xAxis ? 5 : 4);
    AddRectLayer(world, center, xAxis ? 2 : 3, xAxis ? 3 : 2, BlockType::StoneBlock);
    AddColumn(world, GridPos { center.x, center.y + 1, center.z }, 2, BlockType::EnergyGlassBlock);

    if (xAxis)
    {
        AddColumn(world, GridPos { center.x, center.y + 1, center.z - 4 }, 2, BlockType::WoodBlock);
        AddColumn(world, GridPos { center.x, center.y + 1, center.z + 4 }, 2, BlockType::WoodBlock);
    }
    else
    {
        AddColumn(world, GridPos { center.x - 4, center.y + 1, center.z }, 2, BlockType::WoodBlock);
        AddColumn(world, GridPos { center.x + 4, center.y + 1, center.z }, 2, BlockType::WoodBlock);
    }
}

void AddHypixelStyleCenter(World& world)
{
    AddClassicIsland(world, Vector3 { 0.0f, 0.0f, 0.0f }, 12, 12);
    AddSquareRing(world, GridPos { 0, 0, 0 }, 9, 13, BlockType::StoneBlock);
    AddSquareRing(world, GridPos { 0, -1, 0 }, 10, 12, BlockType::DirtBlock);

    AddClassicIsland(world, Vector3 { 0.0f, 1.0f, 0.0f }, 6, 6, 1);
    AddRectLayer(world, GridPos { 0, 1, 0 }, 4, 4, BlockType::StoneBlock);
    AddCenterMonument(world);

    const GridPos glassSpikes[] {
        GridPos { -9, 1, -9 },
        GridPos { 9, 1, -9 },
        GridPos { -9, 1, 9 },
        GridPos { 9, 1, 9 }
    };
    for (const GridPos& spike : glassSpikes)
    {
        AddColumn(world, spike, 3, BlockType::EnergyGlassBlock);
    }
}
}

void Game::SetupMatch()
{
    world_.Clear();
    teams_.clear();
    matchSimulation_.ResetCores();
    matchSimulation_.Players().clear(); // players accessed via MatchSimulation (6A)
    matchSimulation_.ResetGenerators();
    matchSimulation_.ResetPickups();
    worldEffects_.clear();
    particles_.Clear();
    timedExplosions_.clear();
    projectiles_.clear();
    hazardZones_.clear();
    alarmTraps_.clear();
    heroTemporaryBlocks_.clear();
    molotovBlockBurns_.clear();
    radonBurns_.clear();
    likhoBlockCuts_.clear();
    konvoyIntruderMarks_.clear();
    bromVacuumBots_.clear();
    bromTurretDrones_.clear();
    konvoyTraps_.clear();
    konvoyTethers_.clear();
    konvoyDomes_.clear();
    likhoBleeds_.clear();
    svidetelEchoes_.clear();
    replicatedProjectiles_.clear();
    replicatedExplosives_.clear();
    replicatedHazardZones_.clear();
    replicatedHeroDevices_.clear();
    replicatedStatusEffects_.clear();
    svidetelPhaseBlocks_.clear();
    matchSimulation_.ResetDroppedItems();
    floatingTexts_.clear();
    eventMessages_.clear();
    killFeed_.clear();
    playerScores_.clear();
    damageCredits_.clear();
    botMemories_.clear();
    botMemoryIndexByPlayerId_.clear();
    for (TeamCoordinationBus& bus : teamCoordBuses_)
    {
        bus.Clear();
    }
    coreDefenseMonitors_ = {};
    teamChests_ = {};
    personalChest_ = Inventory {};
    placementPreview_ = PlacementPreview {};
    breakProgress_ = BreakProgress {};
    combatPreview_ = CombatPreview {};
    orbitaTeleportPreview_ = OrbitaTeleportPreview {};
    stats_ = MatchStats {};
    hitMarkerTimer_ = 0.0f;
    damageFlashTimer_ = 0.0f;
    orbitaTeleportPreviewTimer_ = 0.0f;
    // matchTime_ is now owned by matchSimulation_ and cleared in its Reset().
    pickupMergeTimer_ = 0.0f;
    droppedItemMergeTimer_ = 0.0f;
    passiveRegenTimer_ = 0.0f;
    baseHealTimer_ = 0.0f;
    fastPlaceTimer_ = 0.0f;
    localFallVelocity_ = 0.0f;
    localAirPeakY_ = 0.0f;
    localWasOnGround_ = false;
    simulationOrderOffset_ = 0;
    predictionHistory_.clear();
    remoteSnapshotBuffer_.clear();
    estimatedPingMs_ = 0.0f;
    networkSnapshotAgeMs_ = 0.0f;
    networkPacketLossEstimate_ = 0.0f;
    networkInterpolationDelayMs_ = 0.0f;
    networkInterpolationDelaySeconds_ = 0.10f;
    networkBytesPerSecond_ = 0.0f;
    networkPacketsPerSecond_ = 0.0f;
    networkLastFullSnapshotBytes_ = 0;
    networkLastDeltaSnapshotBytes_ = 0;
    networkFullSnapshots_ = 0;
    networkDeltaSnapshots_ = 0;
    networkDroppedSnapshots_ = 0;
    networkIgnoredSnapshots_ = 0;
    networkResyncRequests_ = 0;
    predictionError_ = 0.0f;
    predictionCorrectionFlashTimer_ = 0.0f;
    predictionCorrectionStatsTimer_ = 0.0f;
    predictionCorrectionsThisSecond_ = 0;
    predictionCorrectionsPerSecond_ = 0.0f;
    unackedCommandCount_ = 0;
    lastAuthoritativeTick_ = 0;
    lagCompHistory_.clear();
    matchSimulation_.Reset(); // clears tick/clock/queue + winner + phase (Lobby)
    suddenDeathTiebreakTeamId_.reset();
    coreCollapseTriggered_ = false;
    coreCollapseWarned_ = false;
    suddenDeathDecayTimer_ = 0.0f;
    generatorBoostTriggered_ = false;
    shopOpen_ = false;
    economyActionSeq_.clear();
    recentActionResults_.clear();
    presentedActionResultSeq_.clear();
    nextActionResultSeq_ = 0;
    recentWorldEvents_.clear();
    nextWorldEventSeq_ = 0;
    presentedWorldEventSeq_ = 0;
    nextProjectileId_ = 0;
    nextExplosiveId_ = 0;
    nextHazardZoneId_ = 0;
    nextHeroDeviceId_ = 0;
    nextDroppedItemId_ = 0;
    clientEconomyActionSeq_ = 0;
    pendingEconomyActionType_ = PlayerActionType::None;
    pendingEconomyActionParamA_ = 0;
    pendingEconomyActionParamB_ = 0;
    selectedHotbarSlot_ = 0;
    inventoryOpen_ = false;
    CloseChest();
    inventoryCursorSlot_ = 0;
    heldInventoryStack_ = ItemStack {};
    heldInventoryOrigin_ = HeldInventoryOrigin::None;
    heldInventoryOriginSlot_ = -1;
    attackChargeActive_ = false;
    attackChargeTimer_ = 0.0f;
    sniperScopeBlend_ = 0.0f;
    shopCategoryIndex_ = 0;
    selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
    selectedTeamSize_ = std::clamp(selectedTeamSize_, 1, 4);
    selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
    scoreboardHeld_ = false;
    spectatorMode_ = false;
    spectatorFreeCamera_ = false;
    spectatorTargetIndex_ = 0;
    spectatorPosition_ = Vector3 {};
    localDeathKiller_.clear();
    localDeathCause_.clear();
    localDeathOverlayTimer_ = 0.0f;

    Team red {
        0,
        TeamColor::Red,
        "Red",
        Vector3 { -38.0f, 1.5f, 0.0f },
        GridPos { -30, 1, 0 },
        Vector3 { -40.0f, 0.58f, 3.0f },
        true,
        0,
        0
    };

    Team blue {
        1,
        TeamColor::Blue,
        "Blue",
        Vector3 { 38.0f, 1.5f, 0.0f },
        GridPos { 30, 1, 0 },
        Vector3 { 40.0f, 0.58f, -3.0f },
        true,
        0,
        0
    };

    Team green {
        2,
        TeamColor::Green,
        "Green",
        Vector3 { 0.0f, 1.5f, -38.0f },
        GridPos { 0, 1, -30 },
        Vector3 { 3.0f, 0.58f, -40.0f },
        true,
        0,
        0
    };

    Team yellow {
        3,
        TeamColor::Yellow,
        "Yellow",
        Vector3 { 0.0f, 1.5f, 38.0f },
        GridPos { 0, 1, 30 },
        Vector3 { -3.0f, 0.58f, 40.0f },
        true,
        0,
        0
    };

    teams_.push_back(red);
    teams_.push_back(blue);
    teams_.push_back(green);
    teams_.push_back(yellow);

    teams_[0].teamChestBlock = GridPos { -40, 1, -3 };
    teams_[1].teamChestBlock = GridPos { 40, 1, 3 };
    teams_[2].teamChestBlock = GridPos { -3, 1, -40 };
    teams_[3].teamChestBlock = GridPos { 3, 1, 40 };

    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        AddFrozenRingLayout();
        break;
    case ArenaBiome::Lava:
        AddMoltenLayersLayout();
        break;
    case ArenaBiome::Space:
        AddOrbitalShardsLayout();
        break;
    case ArenaBiome::Ruins:
        AddBrokenCitadelLayout();
        break;
    case ArenaBiome::Arena:
        AddClassicArenaLayout();
        if (arenaLayout_ == ArenaLayout::Vertical)
        {
            AddVerticalArenaFeatures();
        }
        break;
    }

    for (const Team& team : teams_)
    {
        if (!IsTeamActiveForMode(team.id))
        {
            continue;
        }
        world_.PlaceBlock(team.coreBlock, Block { BlockType::EnergyCoreBlock, team.id, false }, true);
        matchSimulation_.Cores().emplace_back(team.id, team.coreBlock, 120);
        world_.PlaceBlock(GridPos { team.coreBlock.x + 1, team.coreBlock.y, team.coreBlock.z }, Block { BlockType::StoneBlock, team.id, true }, true);
        world_.PlaceBlock(GridPos { team.coreBlock.x - 1, team.coreBlock.y, team.coreBlock.z }, Block { BlockType::WoolBlock, team.id, true }, true);
        world_.PlaceBlock(GridPos { team.coreBlock.x, team.coreBlock.y, team.coreBlock.z + 1 }, Block { BlockType::WoolBlock, team.id, true }, true);
        world_.PlaceBlock(GridPos { team.coreBlock.x, team.coreBlock.y, team.coreBlock.z - 1 }, Block { BlockType::WoolBlock, team.id, true }, true);
        world_.PlaceBlock(team.teamChestBlock, Block { BlockType::TeamChestBlock, team.id, false }, true);
    }

    SetupGenerators();

    const Team* localTeam = FindTeam(selectedTeamId_);
    if (localTeam == nullptr)
    {
        localTeam = &teams_.front();
        selectedTeamId_ = localTeam->id;
    }

    const auto giveHumanLoadout = [](Player& player, HeroId heroId)
    {
        player.GetInventory().AddItem(heroId == HeroId::Svidetel ? ItemType::SniperRifle : ItemType::Sword, 1);
        player.GetInventory().AddBlock(BlockType::WoodBlock, 24);
        player.GetInventory().AddBlock(BlockType::WoolBlock, 32);
        player.GetInventory().AddBlock(BlockType::StoneBlock, 8);
    };

    if (!pendingNetworkRoster_.empty())
    {
        int nextPlayerId = 1;
        for (const LobbyPlayerState& lobbyPlayer : pendingNetworkRoster_)
        {
            const int teamId = std::clamp(lobbyPlayer.selectedTeam, 0, TeamCountForMode() - 1);
            const Team* team = FindTeam(teamId);
            if (team == nullptr)
            {
                continue;
            }
            const int heroIndex = std::clamp(lobbyPlayer.selectedHero, 0, HeroSystem::kHeroCount - 1);
            const HeroId heroId = HeroSystem::IdFromIndex(heroIndex);
            const std::string name = lobbyPlayer.playerName.empty()
                ? ("Игрок " + std::to_string(lobbyPlayer.clientId))
                : lobbyPlayer.playerName;
            Player player(nextPlayerId++, name, teamId, team->spawnPoint, false);
            player.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
            player.SetHeroId(heroId);
            player.SetYaw(YawForTeam(teamId));
            giveHumanLoadout(player, heroId);
            matchSimulation_.Players().push_back(player);
        }

        // Fill the remaining team slots with bots so a small network roster still
        // gets a full, lively arena to play/spectate (Phase 0.1T). Network matches
        // only — the single-player branch below is untouched, so automatch/seed
        // determinism is unaffected. The roster players above keep ids 1..N and are
        // the ones the server binds to clients; these bots get the later ids and
        // are driven by the AI (they are not in networkControlledPlayerIds_).
        const int teamSize = std::clamp(selectedTeamSize_, 1, 4);
        std::array<int, 4> teamCounts {};
        for (const Player& existing : matchSimulation_.Players())
        {
            const int t = existing.GetTeamId();
            if (t >= 0 && t < 4)
            {
                ++teamCounts[t];
            }
        }
        int nextBotId = nextPlayerId;
        for (const Team& team : teams_)
        {
            if (!IsTeamActiveForMode(team.id))
            {
                continue;
            }
            while (teamCounts[team.id] < teamSize)
            {
                Player bot(
                    nextBotId++,
                    std::string(TeamName(team.id)) + " бот " + std::to_string(teamCounts[team.id] + 1),
                    team.id,
                    team.spawnPoint,
                    false);
                std::array<HeroId, HeroSystem::kHeroCount> availableHeroes {};
                int availableHeroCount = 0;
                for (int heroIndex = 0; heroIndex < HeroSystem::kHeroCount; ++heroIndex)
                {
                    const HeroId candidate = HeroSystem::IdFromIndex(heroIndex);
                    const bool alreadyUsed = std::any_of(players_.begin(), players_.end(),
                        [&team, candidate](const Player& other)
                        {
                            return other.GetTeamId() == team.id && other.GetHeroId() == candidate;
                        });
                    if (!alreadyUsed)
                    {
                        availableHeroes[availableHeroCount++] = candidate;
                    }
                }
                const int heroChoice = availableHeroCount > 1 ? GetRandomValue(0, availableHeroCount - 1) : 0;
                bot.SetHeroId(availableHeroCount > 0 ? availableHeroes[heroChoice] : HeroId::Radon);
                bot.SetYaw(YawForTeam(team.id));
                ApplyBotLoadout(bot);
                matchSimulation_.Players().push_back(bot);
                ++teamCounts[team.id];
            }
        }
    }
    else
    {
        Player local(localPlayerId_, "Игрок", selectedTeamId_, localTeam->spawnPoint, true);
        local.SetHeroId(selectedHeroId_);
        local.SetYaw(YawForTeam(selectedTeamId_));
        giveHumanLoadout(local, selectedHeroId_);

        matchSimulation_.Players().push_back(local);

        int nextBotId = 2;
        std::array<int, 4> teamRoster {};
        teamRoster[selectedTeamId_] = 1;
        int botsRemaining = selectedBotCount_;
        const auto addBot = [this, &nextBotId, &teamRoster, &botsRemaining](int teamId)
        {
            const Team* team = FindTeam(teamId);
            if (team == nullptr || botsRemaining <= 0 || teamRoster[teamId] >= selectedTeamSize_)
            {
                return;
            }

            const bool ally = teamId == selectedTeamId_;
            const std::string name = std::string(ally ? "Союзник" : TeamName(teamId))
                + " бот " + std::to_string(teamRoster[teamId] + 1);
            Player bot(nextBotId++, name, teamId, team->spawnPoint, false);
            std::array<HeroId, HeroSystem::kHeroCount> availableHeroes {};
            int availableHeroCount = 0;
            for (int heroIndex = 0; heroIndex < HeroSystem::kHeroCount; ++heroIndex)
            {
                const HeroId candidate = HeroSystem::IdFromIndex(heroIndex);
                const bool alreadyUsed = std::any_of(players_.begin(), players_.end(), [teamId, candidate](const Player& player)
                {
                    return player.GetTeamId() == teamId && player.GetHeroId() == candidate;
                });
                if (!alreadyUsed)
                {
                    availableHeroes[availableHeroCount++] = candidate;
                }
            }
            const int heroChoice = availableHeroCount > 1 ? GetRandomValue(0, availableHeroCount - 1) : 0;
            bot.SetHeroId(availableHeroCount > 0 ? availableHeroes[heroChoice] : HeroId::Radon);
            bot.SetYaw(YawForTeam(teamId));
            ApplyBotLoadout(bot);
            matchSimulation_.Players().push_back(bot);
            ++teamRoster[teamId];
            --botsRemaining;
        };

        std::vector<int> enemyTeams;
        for (const Team& team : teams_)
        {
            if (team.id != selectedTeamId_ && IsTeamActiveForMode(team.id))
            {
                enemyTeams.push_back(team.id);
            }
        }
        if (TeamCountForMode() == 2 && !enemyTeams.empty())
        {
            const int enemyTeamId = enemyTeams.front();
            while (botsRemaining > 0 && teamRoster[enemyTeamId] < selectedTeamSize_)
            {
                addBot(enemyTeamId);
            }
            while (botsRemaining > 0 && teamRoster[selectedTeamId_] < selectedTeamSize_)
            {
                addBot(selectedTeamId_);
            }
        }
        else
        {
            bool added = true;
            while (botsRemaining > 0 && added)
            {
                added = false;
                for (int teamId : enemyTeams)
                {
                    if (botsRemaining <= 0)
                    {
                        break;
                    }
                    if (teamRoster[teamId] < selectedTeamSize_)
                    {
                        addBot(teamId);
                        added = true;
                    }
                }
            }
            while (botsRemaining > 0 && teamRoster[selectedTeamId_] < selectedTeamSize_)
            {
                addBot(selectedTeamId_);
            }
        }
    }

    for (Player& player : players_)
    {
        GetPlayerScore(player.GetId());
    }

    if (Player* localPlayer = GetLocalPlayer())
    {
        cameraController_.Reset(localPlayer->GetYaw(), -0.14f, localPlayer->GetPosition());
        localWasOnGround_ = localPlayer->IsOnGround();
        localAirPeakY_ = localPlayer->GetPosition().y;
    }

    AddEventMessage("Собирайте ресурсы у генератора на базе", Color { 188, 198, 210, 255 }, 5.0f);
    AddEventMessage("Покупайте блоки, стройте мост к центру и ломайте защиту врага", Color { 255, 235, 142, 255 }, 5.5f);
    AddEventMessage("Удерживайте ЛКМ, чтобы ломать блоки или бить Кор", Color { 112, 232, 255, 255 }, 6.0f);
    AddEventMessage("Золотой прицел = враг в радиусе ближнего боя", Color { 255, 235, 142, 255 }, 6.5f);
    if (arenaBiome_ != ArenaBiome::Arena)
    {
        AddEventMessage(std::string("Правило биома: ") + ArenaBiomeName(), BiomeFogColor(), 6.8f);
    }

    // The match is set up and the simulation is live (snapshot phase). Reset()
    // above set it to Lobby; the win condition will move it to Finished.
    matchSimulation_.ResetBlockDeltas();
    matchSimulation_.SetPhase(MatchPhase::Playing);

    // Phase 6: plain singleplayer runs an in-process integrated server for the
    // local human. Automatch stays direct — there is no human client to route.
    if (networkMode_ == NetworkMode::LocalSinglePlayer && !automatch_.active)
    {
        StartIntegratedServer();
    }
    else
    {
        StopIntegratedServer();
    }
}

void Game::AddClassicArenaLayout()
{
    AddHypixelStyleBase(world_, GridPos { -38, 0, 0 }, true, 0);
    AddHypixelStyleBase(world_, GridPos { 38, 0, 0 }, true, 1);
    AddHypixelStyleBase(world_, GridPos { 0, 0, -38 }, false, 2);
    AddHypixelStyleBase(world_, GridPos { 0, 0, 38 }, false, 3);

    AddHypixelStyleCenter(world_);

    AddHypixelStyleResourceIsland(world_, GridPos { -22, 0, -22 }, true);
    AddHypixelStyleResourceIsland(world_, GridPos { 22, 0, -22 }, false);
    AddHypixelStyleResourceIsland(world_, GridPos { -22, 0, 22 }, false);
    AddHypixelStyleResourceIsland(world_, GridPos { 22, 0, 22 }, true);

    AddWoodDock(world_, GridPos { -29, 0, 0 }, 2, 1, 0);
    AddWoodDock(world_, GridPos { 29, 0, 0 }, 2, -1, 0);
    AddWoodDock(world_, GridPos { 0, 0, -29 }, 2, 0, 1);
    AddWoodDock(world_, GridPos { 0, 0, 29 }, 2, 0, -1);

    AddTree(world_, GridPos { -44, 1, -5 });
    AddTree(world_, GridPos { -44, 1, 5 });
    AddTree(world_, GridPos { 44, 1, 5 });
    AddTree(world_, GridPos { 44, 1, -5 });
    AddTree(world_, GridPos { -5, 1, -44 });
    AddTree(world_, GridPos { 5, 1, -44 });
    AddTree(world_, GridPos { 5, 1, 44 });
    AddTree(world_, GridPos { -5, 1, 44 });
}

void Game::PrepareStartupSmoke()
{
    if (!headless_)
    {
        StartSelectedMatch();
    }
}

void Game::ExerciseStartupSmokeMutation(bool place)
{
    const GridPos probe { 0, 20, 0 };
    if (place)
    {
        world_.PlaceBlock(probe, Block { BlockType::StoneBlock, -1, true }, true);
    }
    else
    {
        world_.RemoveBlock(probe);
    }
}

void Game::AddFrozenRingLayout()
{
    AddClassicIsland(world_, Vector3 { -36.0f, 0.0f, 0.0f }, 11, 7);
    AddClassicIsland(world_, Vector3 { 36.0f, 0.0f, 0.0f }, 11, 7);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -36.0f }, 7, 11);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 36.0f }, 7, 11);

    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 0.0f }, 8, 8);
    AddCenterMonument(world_);
    AddSquareRing(world_, GridPos { 0, 0, 0 }, 13, 16, BlockType::IceBlock);

    const GridPos sideShrines[] {
        GridPos { -24, 0, 0 },
        GridPos { 24, 0, 0 },
        GridPos { 0, 0, -24 },
        GridPos { 0, 0, 24 }
    };
    for (const GridPos& shrine : sideShrines)
    {
        AddRectLayer(world_, shrine, 4, 4, BlockType::StoneBlock);
        AddRectLayer(world_, GridPos { shrine.x, shrine.y + 1, shrine.z }, 2, 2, BlockType::IceBlock);
    }

    const GridPos corners[] {
        GridPos { -24, 0, -24 },
        GridPos { 24, 0, -24 },
        GridPos { -24, 0, 24 },
        GridPos { 24, 0, 24 }
    };
    for (const GridPos& corner : corners)
    {
        AddRectLayer(world_, corner, 4, 4, BlockType::StoneBlock);
    }

    AddLineBridge(world_, GridPos { -28, 0, -3 }, GridPos { -24, 0, -24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -28, 0, 3 }, GridPos { -24, 0, 24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 28, 0, -3 }, GridPos { 24, 0, -24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 28, 0, 3 }, GridPos { 24, 0, 24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -3, 0, -28 }, GridPos { -24, 0, -24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 3, 0, -28 }, GridPos { 24, 0, -24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -3, 0, 28 }, GridPos { -24, 0, 24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 3, 0, 28 }, GridPos { 24, 0, 24 }, 3, BlockType::StoneBlock);

    AddLineBridge(world_, GridPos { -24, 0, -24 }, GridPos { -16, 0, -16 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 24, 0, -24 }, GridPos { 16, 0, -16 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -24, 0, 24 }, GridPos { -16, 0, 16 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 24, 0, 24 }, GridPos { 16, 0, 16 }, 3, BlockType::StoneBlock);

    AddLineBridge(world_, GridPos { -28, 0, 0 }, GridPos { -9, 0, 0 }, 2, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 28, 0, 0 }, GridPos { 9, 0, 0 }, 2, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 0, 0, -28 }, GridPos { 0, 0, -9 }, 2, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 0, 0, 28 }, GridPos { 0, 0, 9 }, 2, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { -16, 0, 0 }, GridPos { -9, 0, 0 }, 3, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 16, 0, 0 }, GridPos { 9, 0, 0 }, 3, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 0, 0, -16 }, GridPos { 0, 0, -9 }, 3, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 0, 0, 16 }, GridPos { 0, 0, 9 }, 3, BlockType::IceBlock);
}

void Game::AddMoltenLayersLayout()
{
    AddClassicIsland(world_, Vector3 { -36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -36.0f }, 7, 10);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 36.0f }, 7, 10);

    AddRectLayer(world_, GridPos { 0, -1, 0 }, 8, 8, BlockType::StoneBlock);
    AddRectLayer(world_, GridPos { 0, -1, 0 }, 4, 4, BlockType::LavaBlock);
    AddRectLayer(world_, GridPos { 0, 2, 0 }, 7, 7, BlockType::StoneBlock);
    AddRectLayer(world_, GridPos { 0, 3, 0 }, 3, 3, BlockType::EnergyGlassBlock);

    const GridPos highPads[] {
        GridPos { -21, 2, 0 },
        GridPos { 21, 2, 0 },
        GridPos { 0, 2, -21 },
        GridPos { 0, 2, 21 }
    };
    for (const GridPos& pad : highPads)
    {
        AddRectLayer(world_, pad, 5, 4, BlockType::StoneBlock);
    }

    const GridPos lowForges[] {
        GridPos { -18, -1, -18 },
        GridPos { 18, -1, -18 },
        GridPos { -18, -1, 18 },
        GridPos { 18, -1, 18 }
    };
    for (const GridPos& forge : lowForges)
    {
        AddRectLayer(world_, forge, 4, 4, BlockType::StoneBlock);
        AddRectLayer(world_, GridPos { forge.x, forge.y, forge.z }, 2, 2, BlockType::LavaBlock);
    }

    AddSteppedBridge(world_, GridPos { -28, 0, -3 }, GridPos { -21, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { -28, 0, 3 }, GridPos { -21, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 28, 0, 3 }, GridPos { 21, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 28, 0, -3 }, GridPos { 21, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 3, 0, -28 }, GridPos { 0, 2, -21 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { -3, 0, -28 }, GridPos { 0, 2, -21 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { -3, 0, 28 }, GridPos { 0, 2, 21 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 3, 0, 28 }, GridPos { 0, 2, 21 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -21, 2, 0 }, GridPos { -7, 2, 0 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 21, 2, 0 }, GridPos { 7, 2, 0 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 0, 2, -21 }, GridPos { 0, 2, -7 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 0, 2, 21 }, GridPos { 0, 2, 7 }, 3, BlockType::StoneBlock);

    AddLineBridge(world_, GridPos { -28, -1, 0 }, GridPos { -8, -1, 0 }, 2, BlockType::LavaBlock);
    AddLineBridge(world_, GridPos { 28, -1, 0 }, GridPos { 8, -1, 0 }, 2, BlockType::LavaBlock);
    AddLineBridge(world_, GridPos { 0, -1, -28 }, GridPos { 0, -1, -8 }, 2, BlockType::LavaBlock);
    AddLineBridge(world_, GridPos { 0, -1, 28 }, GridPos { 0, -1, 8 }, 2, BlockType::LavaBlock);
    AddSteppedBridge(world_, GridPos { -8, -1, 0 }, GridPos { -3, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 8, -1, 0 }, GridPos { 3, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 0, -1, -8 }, GridPos { 0, 2, -3 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 0, -1, 8 }, GridPos { 0, 2, 3 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -18, -1, -18 }, GridPos { -8, -1, -8 }, 2, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 18, -1, -18 }, GridPos { 8, -1, -8 }, 2, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -18, -1, 18 }, GridPos { -8, -1, 8 }, 2, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 18, -1, 18 }, GridPos { 8, -1, 8 }, 2, BlockType::StoneBlock);
}

void Game::AddOrbitalShardsLayout()
{
    AddClassicIsland(world_, Vector3 { -36.0f, 0.0f, 0.0f }, 9, 7);
    AddClassicIsland(world_, Vector3 { 36.0f, 0.0f, 0.0f }, 9, 7);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -36.0f }, 7, 9);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 36.0f }, 7, 9);

    AddRectLayer(world_, GridPos { 0, 3, 0 }, 7, 7, BlockType::EnergyGlassBlock);
    AddRectLayer(world_, GridPos { 0, 4, 0 }, 3, 3, BlockType::StoneBlock);

    const GridPos launchPads[] {
        GridPos { -24, 1, 0 },
        GridPos { 24, 1, 0 },
        GridPos { 0, 1, -24 },
        GridPos { 0, 1, 24 }
    };
    for (const GridPos& pad : launchPads)
    {
        AddRectLayer(world_, pad, 4, 3, BlockType::StoneBlock);
        PlaceMapBlock(world_, GridPos { pad.x, pad.y + 1, pad.z }, BlockType::SpringBlock, -1, true);
    }

    const GridPos shards[] {
        GridPos { -16, 2, -16 },
        GridPos { 16, 2, -16 },
        GridPos { -16, 2, 16 },
        GridPos { 16, 2, 16 },
        GridPos { -10, 3, 0 },
        GridPos { 10, 3, 0 },
        GridPos { 0, 3, -10 },
        GridPos { 0, 3, 10 }
    };
    for (const GridPos& shard : shards)
    {
        AddRectLayer(world_, shard, 3, 3, BlockType::StoneBlock);
    }

    const GridPos anchors[] {
        GridPos { -29, 0, 0 },
        GridPos { -27, 1, 0 },
        GridPos { -21, 1, 0 },
        GridPos { 29, 0, 0 },
        GridPos { 27, 1, 0 },
        GridPos { 21, 1, 0 },
        GridPos { 0, 0, -29 },
        GridPos { 0, 1, -27 },
        GridPos { 0, 1, -21 },
        GridPos { 0, 0, 29 },
        GridPos { 0, 1, 27 },
        GridPos { 0, 1, 21 }
    };
    for (const GridPos& anchor : anchors)
    {
        AddRectLayer(world_, anchor, 1, 1, BlockType::StoneBlock);
    }

    AddLineBridge(world_, GridPos { -24, 1, 0 }, GridPos { -16, 2, -16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { -24, 1, 0 }, GridPos { -16, 2, 16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 24, 1, 0 }, GridPos { 16, 2, -16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 24, 1, 0 }, GridPos { 16, 2, 16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 1, -24 }, GridPos { -16, 2, -16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 1, -24 }, GridPos { 16, 2, -16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 1, 24 }, GridPos { -16, 2, 16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 1, 24 }, GridPos { 16, 2, 16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { -10, 3, 0 }, GridPos { -7, 3, 0 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 10, 3, 0 }, GridPos { 7, 3, 0 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 3, -10 }, GridPos { 0, 3, -7 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 3, 10 }, GridPos { 0, 3, 7 }, 2, BlockType::EnergyGlassBlock);
}

void Game::AddBrokenCitadelLayout()
{
    AddClassicIsland(world_, Vector3 { -36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -36.0f }, 7, 10);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 36.0f }, 7, 10);

    AddRectLayer(world_, GridPos { 0, 0, 0 }, 9, 9, BlockType::StoneBlock, true);
    AddRectLayer(world_, GridPos { 0, 1, 0 }, 5, 5, BlockType::StoneBlock, true);
    AddRectLayer(world_, GridPos { 0, 2, 0 }, 2, 2, BlockType::EnergyGlassBlock, true);
    const GridPos missingCitadel[] {
        GridPos { -7, 0, -7 },
        GridPos { 7, 0, -7 },
        GridPos { -7, 0, 7 },
        GridPos { 7, 0, 7 },
        GridPos { -3, 1, 4 },
        GridPos { 4, 1, -3 }
    };
    for (const GridPos& pos : missingCitadel)
    {
        world_.RemoveBlock(pos);
    }

    const GridPos rooms[] {
        GridPos { -21, 1, -21 },
        GridPos { 21, 1, -21 },
        GridPos { -21, 1, 21 },
        GridPos { 21, 1, 21 }
    };
    for (const GridPos& room : rooms)
    {
        AddRectLayer(world_, room, 5, 5, BlockType::StoneBlock, true);
        AddRectLayer(world_, GridPos { room.x, room.y + 1, room.z }, 3, 3, BlockType::EnergyGlassBlock, true);
        PlaceMapBlock(world_, GridPos { room.x, room.y + 2, room.z }, BlockType::EnergyGlassBlock, -1, true);
        world_.RemoveBlock(GridPos { room.x + 5, room.y + 1, room.z });
        world_.RemoveBlock(GridPos { room.x - 5, room.y + 1, room.z });
        world_.RemoveBlock(GridPos { room.x, room.y + 1, room.z + 5 });
        world_.RemoveBlock(GridPos { room.x, room.y + 1, room.z - 5 });
    }

    AddLineBridge(world_, GridPos { -28, 0, 0 }, GridPos { -9, 0, 0 }, 3, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 28, 0, 0 }, GridPos { 9, 0, 0 }, 3, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 0, 0, -28 }, GridPos { 0, 0, -9 }, 3, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 0, 0, 28 }, GridPos { 0, 0, 9 }, 3, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { -28, 0, -3 }, GridPos { -21, 1, -21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { -28, 0, 3 }, GridPos { -21, 1, 21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { 28, 0, -3 }, GridPos { 21, 1, -21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { 28, 0, 3 }, GridPos { 21, 1, 21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { -3, 0, -28 }, GridPos { -21, 1, -21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { 3, 0, -28 }, GridPos { 21, 1, -21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { -3, 0, 28 }, GridPos { -21, 1, 21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { 3, 0, 28 }, GridPos { 21, 1, 21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { -21, 1, -21 }, GridPos { -8, 1, -8 }, 2, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 21, 1, -21 }, GridPos { 8, 1, -8 }, 2, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { -21, 1, 21 }, GridPos { -8, 1, 8 }, 2, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 21, 1, 21 }, GridPos { 8, 1, 8 }, 2, BlockType::WoodBlock, true);

    const GridPos crackedEdges[] {
        GridPos { -18, 1, -18 },
        GridPos { 18, 1, -18 },
        GridPos { -18, 1, 18 },
        GridPos { 18, 1, 18 },
        GridPos { -13, 0, 1 },
        GridPos { 13, 0, -1 },
        GridPos { 1, 0, -13 },
        GridPos { -1, 0, 13 }
    };
    for (const GridPos& pos : crackedEdges)
    {
        world_.RemoveBlock(pos);
    }
}

void Game::SetupGenerators()
{
    const auto addGenerator = [this](ResourceType type, Vector3 position, float interval, int amount, int teamId)
    {
        world_.PlaceBlock(world_.WorldToGrid(position), Block { BlockType::ResourceGenerator, teamId, false }, true);
        matchSimulation_.Generators().emplace_back(type, ToVec3(position), interval, amount, teamId);
    };

    if (IsTeamActiveForMode(0))
    {
        addGenerator(ResourceType::Iron, Vector3 { -39.0f, 1.0f, -1.0f }, 0.78f, 1, 0);
        addGenerator(ResourceType::Gold, Vector3 { -41.0f, 1.0f, 1.0f }, 3.4f, 1, 0);
    }
    if (IsTeamActiveForMode(1))
    {
        addGenerator(ResourceType::Iron, Vector3 { 39.0f, 1.0f, 1.0f }, 0.78f, 1, 1);
        addGenerator(ResourceType::Gold, Vector3 { 41.0f, 1.0f, -1.0f }, 3.4f, 1, 1);
    }
    if (IsTeamActiveForMode(2))
    {
        addGenerator(ResourceType::Iron, Vector3 { 1.0f, 1.0f, -39.0f }, 0.78f, 1, 2);
        addGenerator(ResourceType::Gold, Vector3 { -1.0f, 1.0f, -41.0f }, 3.4f, 1, 2);
    }
    if (IsTeamActiveForMode(3))
    {
        addGenerator(ResourceType::Iron, Vector3 { -1.0f, 1.0f, 39.0f }, 0.78f, 1, 3);
        addGenerator(ResourceType::Gold, Vector3 { 1.0f, 1.0f, 41.0f }, 3.4f, 1, 3);
    }

    if (arenaBiome_ == ArenaBiome::Ice)
    {
        addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 2.0f, 0.0f }, 4.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -24.0f, 2.0f, 0.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 24.0f, 2.0f, 0.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 2.0f, -24.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 2.0f, 24.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { -24.0f, 1.0f, -24.0f }, 6.0f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { 24.0f, 1.0f, 24.0f }, 6.0f, 1, -1);
        return;
    }
    if (arenaBiome_ == ArenaBiome::Lava)
    {
        addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 4.0f, 0.0f }, 5.4f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -21.0f, 3.0f, 0.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 21.0f, 3.0f, 0.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 3.0f, -21.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 3.0f, 21.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { -18.0f, 0.0f, -18.0f }, 4.8f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { 18.0f, 0.0f, 18.0f }, 4.8f, 1, -1);
        return;
    }
    if (arenaBiome_ == ArenaBiome::Space)
    {
        addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 5.0f, 0.0f }, 4.4f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -10.0f, 4.0f, 0.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 10.0f, 4.0f, 0.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 4.0f, -10.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 4.0f, 10.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { -16.0f, 3.0f, -16.0f }, 6.0f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { 16.0f, 3.0f, 16.0f }, 6.0f, 1, -1);
        return;
    }
    if (arenaBiome_ == ArenaBiome::Ruins)
    {
        addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 3.0f, 0.0f }, 5.2f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { -21.0f, 3.0f, -21.0f }, 4.8f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 21.0f, 3.0f, -21.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -21.0f, 3.0f, 21.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { 21.0f, 3.0f, 21.0f }, 4.8f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -6.0f, 1.0f, 0.0f }, 3.4f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 6.0f, 1.0f, 0.0f }, 3.4f, 1, -1);
        return;
    }

    addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 2.0f, 0.0f }, 4.8f, 1, -1);
    addGenerator(ResourceType::Gold, Vector3 { -4.0f, 2.0f, 0.0f }, 3.0f, 1, -1);
    addGenerator(ResourceType::Gold, Vector3 { 4.0f, 2.0f, 0.0f }, 3.0f, 1, -1);
    addGenerator(ResourceType::Crystal, Vector3 { -22.0f, 1.0f, -22.0f }, 6.2f, 1, -1);
    addGenerator(ResourceType::Crystal, Vector3 { 22.0f, 1.0f, -22.0f }, 6.2f, 1, -1);
    addGenerator(ResourceType::Crystal, Vector3 { -22.0f, 1.0f, 22.0f }, 6.2f, 1, -1);
    addGenerator(ResourceType::Crystal, Vector3 { 22.0f, 1.0f, 22.0f }, 6.2f, 1, -1);
}

void Game::AddVerticalArenaFeatures()
{
    AddOvalLayer(world_, Vector3 { 0.0f, 0.0f, 0.0f }, 7, 7, 2, BlockType::GrassBlock, false);
    AddOvalLayer(world_, Vector3 { 0.0f, 0.0f, 0.0f }, 9, 9, -1, BlockType::DirtBlock, false);
    AddWoodDock(world_, GridPos { -12, -1, 0 }, 6, 1, 0);
    AddWoodDock(world_, GridPos { 12, -1, 0 }, 6, -1, 0);
    AddWoodDock(world_, GridPos { 0, -1, -12 }, 6, 0, 1);
    AddWoodDock(world_, GridPos { 0, -1, 12 }, 6, 0, -1);

    const GridPos towerBases[] {
        GridPos { -33, 1, 5 },
        GridPos { 33, 1, -5 },
        GridPos { 5, 1, -33 },
        GridPos { -5, 1, 33 }
    };
    for (const GridPos& base : towerBases)
    {
        for (int y = 1; y <= 4; ++y)
        {
            PlaceMapBlock(world_, GridPos { base.x, y, base.z }, BlockType::WoodBlock);
        }
        PlaceMapBlock(world_, GridPos { base.x + 1, 4, base.z }, BlockType::LeafBlock);
        PlaceMapBlock(world_, GridPos { base.x - 1, 4, base.z }, BlockType::LeafBlock);
        PlaceMapBlock(world_, GridPos { base.x, 4, base.z + 1 }, BlockType::LeafBlock);
        PlaceMapBlock(world_, GridPos { base.x, 4, base.z - 1 }, BlockType::LeafBlock);
        PlaceMapBlock(world_, GridPos { base.x, 5, base.z }, BlockType::LeafBlock);
    }

    const BlockType hazard = arenaBiome_ == ArenaBiome::Ice
        ? BlockType::IceBlock
        : (arenaBiome_ == ArenaBiome::Lava ? BlockType::LavaBlock : BlockType::SpikeBlock);
    const GridPos hazardPads[] {
        GridPos { -4, 3, 0 },
        GridPos { 4, 3, 0 },
        GridPos { 0, 3, -4 },
        GridPos { 0, 3, 4 }
    };
    for (const GridPos& pad : hazardPads)
    {
        PlaceMapBlock(world_, pad, hazard, -1, true);
    }
    PlaceMapBlock(world_, GridPos { 0, 3, 0 }, BlockType::SpringBlock, -1, true);
}

void Game::AddRuinsBiomeFeatures()
{
    const GridPos relicPlatforms[] {
        GridPos { -20, 1, -20 },
        GridPos { 20, 1, -20 },
        GridPos { -20, 1, 20 },
        GridPos { 20, 1, 20 }
    };
    for (const GridPos& center : relicPlatforms)
    {
        for (int x = -2; x <= 2; ++x)
        {
            for (int z = -2; z <= 2; ++z)
            {
                if (std::abs(x) + std::abs(z) <= 3)
                {
                    PlaceMapBlock(world_, GridPos { center.x + x, center.y, center.z + z }, BlockType::StoneBlock, -1, true);
                }
            }
        }
        PlaceMapBlock(world_, GridPos { center.x, center.y + 1, center.z }, BlockType::EnergyGlassBlock, -1, true);
    }

    const GridPos crackedBridges[] {
        GridPos { -13, 0, 0 },
        GridPos { -11, 0, 0 },
        GridPos { 11, 0, 0 },
        GridPos { 13, 0, 0 },
        GridPos { 0, 0, -13 },
        GridPos { 0, 0, -11 },
        GridPos { 0, 0, 11 },
        GridPos { 0, 0, 13 },
        GridPos { -5, 2, 0 },
        GridPos { 5, 2, 0 },
        GridPos { 0, 2, -5 },
        GridPos { 0, 2, 5 }
    };
    for (const GridPos& pos : crackedBridges)
    {
        if (!world_.IsAir(pos))
        {
            PlaceMapBlock(world_, pos, BlockType::WoodBlock, -1, true);
        }
    }
}

void Game::StartSelectedMatch()
{
    automatch_.active = false;
    tutorialMode_ = false;
    selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
    selectedTeamSize_ = std::clamp(selectedTeamSize_, 1, 4);
    selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
    SaveSettings();
    SetupMatch();
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);
    UpdateCamera(0.016f);
    screen_ = GameScreen::Playing;
    DisableCursor();
    SetMessage(std::string("Режим: ") + MatchModeName() + ". Защищайте Кор.", 4.0f);
}

void Game::StartTutorialMatch()
{
    automatch_.active = false;
    tutorialMode_ = true;
    selectedMode_ = MatchMode::TwoVsTwo;
    selectedTeamId_ = 0;
    selectedTeamSize_ = 1;
    selectedBotCount_ = 1;
    botDifficulty_ = BotDifficulty::Easy;
    arenaLayout_ = ArenaLayout::Classic;
    arenaBiome_ = ArenaBiome::Arena;
    SetupMatch();
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);
    UpdateCamera(0.016f);
    screen_ = GameScreen::Playing;
    DisableCursor();
    SetMessage("Обучение: защищайте свой Кор и уничтожьте вражеский.", 6.0f);
}

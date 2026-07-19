#include "Game.h"
#include "CrashLogger.h"

#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
constexpr float kPi = 3.1415926535f;

float Distance3D(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
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

std::string JsonEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value)
    {
        switch (ch)
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += ch;
            break;
        }
    }
    return escaped;
}

void WriteJsonVector3(std::ofstream& file, Vector3 value)
{
    file << "{\"x\":" << value.x << ",\"y\":" << value.y << ",\"z\":" << value.z << "}";
}

const char* ShopUsageModeName(ShopUsageMode mode)
{
    switch (mode)
    {
    case ShopUsageMode::Placeable: return "placeable";
    case ShopUsageMode::Consumable: return "consumable";
    case ShopUsageMode::TimedEffect: return "timedEffect";
    case ShopUsageMode::Equipment: return "equipment";
    case ShopUsageMode::Upgrade: return "upgrade";
    case ShopUsageMode::TeamEffect: return "teamEffect";
    }
    return "unknown";
}
}

void Game::RecordMemorableMoment(
    std::string category,
    int primaryTeamId,
    int secondaryTeamId,
    std::vector<int> participants,
    std::string description,
    float significance,
    bool botDecisionDriven,
    int actorHealth,
    int targetHealth,
    int carriedResourceValue,
    std::vector<std::string> precedingActions)
{
    if (!automatch_.active || significance < 0.55f)
    {
        return;
    }
    const std::uint32_t tick = matchSimulation_.CurrentTick();
    const int existingCategoryHighlights = static_cast<int>(std::count_if(
        automatch_.currentMemorableMoments.begin(), automatch_.currentMemorableMoments.end(),
        [&category](const MemorableMoment& moment) { return moment.category == category; }));
    if (existingCategoryHighlights >= 2)
    {
        return;
    }
    // Moment categories are highlights, not an action log. Collapse repeated
    // variants from the same team/fight into one event over a meaningful
    // tactical window.
    const std::uint32_t duplicateWindow = 15u * 60u;
    for (auto it = automatch_.currentMemorableMoments.rbegin();
         it != automatch_.currentMemorableMoments.rend(); ++it)
    {
        if (tick > it->tick + duplicateWindow) break;
        if (it->category == category && it->primaryTeamId == primaryTeamId)
        {
            it->significance = std::max(it->significance, significance);
            return;
        }
    }

    MemorableMoment moment;
    moment.tick = tick;
    moment.seed = automatchSeed_ + static_cast<unsigned int>(automatch_.completedRuns) * 0x9e3779b9U;
    moment.map = automatchMapLoaded_ ? automatchMapPath_ : "generated-arena";
    moment.category = std::move(category);
    moment.primaryTeamId = primaryTeamId;
    moment.secondaryTeamId = secondaryTeamId;
    moment.participants = std::move(participants);
    moment.description = std::move(description);
    moment.significance = std::clamp(significance, 0.0f, 1.0f);
    moment.botDecisionDriven = botDecisionDriven;
    moment.actorHealth = actorHealth;
    moment.targetHealth = targetHealth;
    moment.carriedResourceValue = carriedResourceValue;
    moment.precedingActions = std::move(precedingActions);
    for (const EnergyCore& core : matchSimulation_.Cores())
    {
        if (core.GetTeamId() >= 0 && core.GetTeamId() < 4)
        {
            moment.coreHealth[core.GetTeamId()] = core.IsAlive() ? core.GetHealth() : 0;
        }
    }
    ++automatch_.memorableMomentCounts[moment.category];
    automatch_.currentMemorableMoments.push_back(std::move(moment));
}

void Game::StartAutomatch()
{
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    selectedTeamSize_ = 4;
    selectedBotCount_ = MaxBotCountForSelection();
    automatch_ = AutomatchState {};
    automatch_.active = true;
    automatch_.targetRuns = std::clamp(automatchRunTarget_, 1, 500);
    automatch_.maxMatchSeconds = static_cast<float>(std::clamp(automatchMaxMinutes_, 3, 30) * 60);
    if (!headless_)
    {
        SaveSettings();
    }
    SetupMatch();
    ConfigureAutomatchMatch();
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);
    if (!headless_)
    {
        UpdateCamera(0.016f);
    }
    screen_ = GameScreen::Playing;
    if (!headless_)
    {
        DisableCursor();
    }
    showBotDebug_ = true;
    SetMessage("Автоматч запущен: симуляция только с ботами.", 4.0f);
}

bool Game::RunAutomatchBatch(int runs, int ticksPerFrame, int maxMinutes, unsigned int seed)
{
    navigationMetrics_ = NavigationMetrics {};
    profileSimulationMs_ = 0.0;
    profileBotsMs_ = 0.0;
    profilePathMs_ = 0.0;
    profileDecisionMs_ = 0.0;
    profileMovementMs_ = 0.0;
    profileCombatMs_ = 0.0;
    profilePerceptionMs_ = 0.0;
    profilePlanningMs_ = 0.0;
    profileSimulationTicks_ = 0;
    profilePathCalls_ = 0;
    profileDecisionCalls_ = 0;
    profileMovementCalls_ = 0;
    profileCombatCalls_ = 0;
    profilePerceptionCalls_ = 0;
    profilePlanningCalls_ = 0;
    automatchRunTarget_ = std::clamp(runs, 1, 500);
    automatchTicksPerFrame_ = std::clamp(ticksPerFrame, 1, headless_ ? 1024 : 32);
    automatchMaxMinutes_ = std::clamp(maxMinutes, 3, 30);
    automatchSeed_ = seed;
    // Custom map (--map): every SetupMatch in the batch (one per run) builds
    // the world/entities from the creative document instead of the arena. The
    // document's biome/layout drive the sky and biome rules too.
    if (automatchMapLoaded_)
    {
        pendingCreativeDoc_ = &automatchMapDoc_;
        UpdateCustomMapBuildBounds(automatchMapDoc_);
        arenaBiome_ = static_cast<ArenaBiome>(automatchMapDoc_.biome);
        arenaLayout_ = static_cast<ArenaLayout>(automatchMapDoc_.layout);
    }
    else
    {
        hasCustomMapBuildBounds_ = false;
    }
    StartAutomatch();

    const int simulationTicks = automatchRunTarget_ * automatchMaxMinutes_ * 60 * 60;
    const int guardFrames = simulationTicks / automatchTicksPerFrame_ + automatchRunTarget_ * 8 + 64;
    int frames = 0;
    while (automatch_.active && frames++ < guardFrames && !ShouldClose())
    {
        CrashLogger::Heartbeat("automatch-update");
        Update(1.0f / 60.0f);
    }
    if (automatch_.active)
    {
        automatch_.active = false;
        WriteAutomatchStatsJson();
        pendingCreativeDoc_ = nullptr;
        return false;
    }
    pendingCreativeDoc_ = nullptr;
    if (profilingEnabled_)
    {
        const double botsPercent = profileSimulationMs_ > 0.0 ? profileBotsMs_ * 100.0 / profileSimulationMs_ : 0.0;
        const double pathPercent = profileBotsMs_ > 0.0 ? profilePathMs_ * 100.0 / profileBotsMs_ : 0.0;
        std::cout << "PROFILE simulationMs=" << profileSimulationMs_
                  << " ticks=" << profileSimulationTicks_
                  << " botsMs=" << profileBotsMs_
                  << " botsPercent=" << botsPercent
                  << " pathMs=" << profilePathMs_
                  << " pathCalls=" << profilePathCalls_
                  << " pathPercentOfBots=" << pathPercent
                  << " decisionMs=" << profileDecisionMs_
                  << " perceptionMs=" << profilePerceptionMs_
                  << " planningMs=" << profilePlanningMs_
                  << " movementMs=" << profileMovementMs_
                  << " combatMs=" << profileCombatMs_ << '\n';
    }
    return automatch_.completedRuns >= automatch_.targetRuns;
}

void Game::ConfigureAutomatchMatch()
{
    SetRandomSeed(automatchSeed_ + static_cast<unsigned int>(automatch_.completedRuns) * 0x9e3779b9U);
    automatch_.currentFirstCoreDamageTime = -1.0f;
    automatch_.currentTeamStats[0] = AutomatchTeamStats {};
    automatch_.currentTeamStats[1] = AutomatchTeamStats {};
    automatch_.currentTeamStats[2] = AutomatchTeamStats {};
    automatch_.currentTeamStats[3] = AutomatchTeamStats {};
    automatch_.currentTimeline.clear();
    automatch_.currentMemorableMoments.clear();
    automatch_.recentKillTimes.clear();
    automatch_.recentKillCounts.clear();
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        automatch_.lastAbilityTickByTeam[teamId] = 0;
        automatch_.lastAbilityActorByTeam[teamId] = -1;
    }
    automatch_.lastLeaderTeamId = -1;
    automatch_.lastCoreDestroyedTime = -1000.0f;
    automatch_.lastCoreDestroyedTeamId = -1;
    for (AutomatchBotStats& stats : automatch_.botStats)
    {
        // Per-run comparison state must not compare the last target/position
        // of one seed with the freshly spawned bot in the next seed. Aggregate
        // counters intentionally remain cumulative across the batch.
        stats.hasObjectiveSample = false;
        stats.currentNoProgressSamples = 0;
        stats.lastObjectiveIntent = -1;
        stats.lastAuthoredRouteAdvances = 0;
        stats.lastPlanStarts = 0;
        stats.lastPlanStageAdvances = 0;
        stats.lastPlanCompletions = 0;
        stats.lastPlanCancellations = 0;
        stats.lastPlanExpiryCancellations = 0;
        stats.lastPlanEvidenceCancellations = 0;
        stats.lastPlanRouteFailureCancellations = 0;
        stats.lastPlanVoidCancellations = 0;
        stats.lastIntentForRetreat = -1;
    }

    players_.erase(
        std::remove_if(players_.begin(), players_.end(), [](const Player& player) { return !IsLocallyPredicted(player.GetControlKind()); }),
        players_.end());
    int nextId = localPlayerId_ + 1;
    for (Team& team : teams_)
    {
        // Custom maps (--map) define playable teams by their cores.
        if (!TeamPlayableForSetup(team.id))
        {
            continue;
        }
        for (int slot = 0; slot < selectedTeamSize_; ++slot)
        {
            Player bot(nextId++, std::string(TeamName(team.id)) + " бот " + std::to_string(slot + 1), team.id, team.spawnPoint, false);
            bot.SetHeroId(HeroSystem::IdFromIndex(slot % HeroSystem::kHeroCount));
            bot.SetYaw(YawForTeam(team.id));
            ApplyBotLoadout(bot);
            players_.push_back(bot);
        }
    }

    std::array<std::array<int, HeroSystem::kHeroCount>, 4> heroOrdersByTeam {};
    for (std::array<int, HeroSystem::kHeroCount>& heroOrder : heroOrdersByTeam)
    {
        for (int index = 0; index < HeroSystem::kHeroCount; ++index)
        {
            heroOrder[index] = index;
        }
        for (int index = HeroSystem::kHeroCount - 1; index > 0; --index)
        {
            const int other = GetRandomValue(0, index);
            std::swap(heroOrder[index], heroOrder[other]);
        }
    }
    std::array<int, 4> heroSlotByTeam {};
    for (Player& player : players_)
    {
        const int teamId = player.GetTeamId();
        if (IsLocallyPredicted(player.GetControlKind()) || teamId < 0 || teamId >= static_cast<int>(heroSlotByTeam.size()))
        {
            continue;
        }
        const int slot = heroSlotByTeam[teamId]++ % HeroSystem::kHeroCount;
        player.SetHeroId(HeroSystem::IdFromIndex(heroOrdersByTeam[teamId][slot]));
    }

    if (Player* localPlayer = GetLocalPlayer())
    {
        localPlayer->Kill(true);
    }
    for (Player& player : players_)
    {
        GetPlayerScore(player.GetId());
    }
    spectatorMode_ = true;
    spectatorFreeCamera_ = false;
    spectatorTargetIndex_ = 0;
    for (int i = 0; i < static_cast<int>(players_.size()); ++i)
    {
        if (!IsLocallyPredicted(players_[i].GetControlKind()) && players_[i].IsAlive() && !players_[i].IsEliminated())
        {
            spectatorTargetIndex_ = i;
            spectatorPosition_ = players_[i].GetPosition();
            break;
        }
    }
    cameraController_.SetMode(ViewMode::ThirdPerson);
    automatch_.sampleTimer = 0.0f;
}

void Game::UpdateAutomatch(float dt)
{
    if (!automatch_.active)
    {
        return;
    }

    automatch_.sampleTimer += dt;
    if (automatch_.sampleTimer >= 1.0f)
    {
        automatch_.sampleTimer = 0.0f;
        SampleAutomatchBots();
    }

    const bool timeout = !matchSimulation_.HasWinner() && matchSimulation_.MatchTimeSeconds() >= automatch_.maxMatchSeconds;
    if (!matchSimulation_.HasWinner() && !timeout)
    {
        return;
    }

    FinishAutomatchRun(timeout);
    if (automatch_.completedRuns >= automatch_.targetRuns)
    {
        automatch_.active = false;
        WriteAutomatchStatsJson();
        SetMessage("Автоматч завершен. Смотрите оверлей статистики.", 8.0f);
        return;
    }

    SetupMatch();
    ConfigureAutomatchMatch();
}

void Game::SampleAutomatchBots()
{
    const auto findBotStats = [this](const Player& player) -> AutomatchBotStats&
    {
        for (AutomatchBotStats& stats : automatch_.botStats)
        {
            if (stats.teamId == player.GetTeamId() && stats.name == player.GetName())
            {
                return stats;
            }
        }
        automatch_.botStats.push_back(AutomatchBotStats { player.GetName(), player.GetTeamId() });
        return automatch_.botStats.back();
    };

    for (const Player& player : players_)
    {
        if (IsLocallyPredicted(player.GetControlKind()))
        {
            continue;
        }
        const BotMemory* memory = nullptr;
        for (const BotMemory& candidate : botMemories_)
        {
            if (candidate.playerId == player.GetId())
            {
                memory = &candidate;
                break;
            }
        }
        if (memory == nullptr)
        {
            continue;
        }

        AutomatchBotStats& stats = findBotStats(player);
        const bool activeSample = player.IsAlive() && !player.IsEliminated();
        if (activeSample)
        {
            ++stats.samples;
        }
        stats.archetype = ToString(memory->archetype);
        const int planStartsDelta = std::max(0, memory->planStarts - stats.lastPlanStarts);
        const int planStageDelta = std::max(0, memory->planStageAdvances - stats.lastPlanStageAdvances);
        const int planCompleteDelta = std::max(0, memory->planCompletions - stats.lastPlanCompletions);
        const int planCancelDelta = std::max(0, memory->planCancellations - stats.lastPlanCancellations);
        const int planExpiryDelta = std::max(0, memory->planExpiryCancellations - stats.lastPlanExpiryCancellations);
        const int planEvidenceDelta = std::max(0, memory->planEvidenceCancellations - stats.lastPlanEvidenceCancellations);
        const int planRouteFailureDelta = std::max(0, memory->planRouteFailureCancellations - stats.lastPlanRouteFailureCancellations);
        const int planVoidDelta = std::max(0, memory->planVoidCancellations - stats.lastPlanVoidCancellations);
        stats.planStarts += planStartsDelta;
        stats.planStageAdvances += planStageDelta;
        stats.planCompletions += planCompleteDelta;
        stats.planCancellations += planCancelDelta;
        stats.planExpiryCancellations += planExpiryDelta;
        stats.planEvidenceCancellations += planEvidenceDelta;
        stats.planRouteFailureCancellations += planRouteFailureDelta;
        stats.planVoidCancellations += planVoidDelta;
        stats.lastPlanStarts = memory->planStarts;
        stats.lastPlanStageAdvances = memory->planStageAdvances;
        stats.lastPlanCompletions = memory->planCompletions;
        stats.lastPlanCancellations = memory->planCancellations;
        stats.lastPlanExpiryCancellations = memory->planExpiryCancellations;
        stats.lastPlanEvidenceCancellations = memory->planEvidenceCancellations;
        stats.lastPlanRouteFailureCancellations = memory->planRouteFailureCancellations;
        stats.lastPlanVoidCancellations = memory->planVoidCancellations;
        stats.planHoldSeconds += activeSample && memory->currentPlan.goal != StrategicGoal::Idle ? 1.0f : 0.0f;
        if (stats.lastIntentForRetreat >= 0
            && stats.lastIntentForRetreat != static_cast<int>(BotIntent::RetreatHome)
            && memory->intent == BotIntent::RetreatHome)
        {
            ++stats.retreats;
            ++automatch_.retreats;
        }
        stats.lastIntentForRetreat = static_cast<int>(memory->intent);
        stats.repeatedRouteDeaths = std::max(stats.repeatedRouteDeaths, memory->repeatedRouteFailures);
        const Inventory& inventory = player.GetInventory();
        const int blocksHeld = inventory.GetBlocks();
        if (activeSample) stats.blocksHeld += blocksHeld;
        stats.maxBlocksHeld = std::max(stats.maxBlocksHeld, blocksHeld);
        stats.finalBlocksHeld = blocksHeld;
        for (int resourceIndex = 0; resourceIndex < 3; ++resourceIndex)
        {
            const ResourceType resource = static_cast<ResourceType>(resourceIndex);
            const int amount = inventory.GetResource(resource);
            if (activeSample) stats.resourcesHeld[resourceIndex] += amount;
            stats.finalResourcesHeld[resourceIndex] = amount;
        }
        const int roleIndex = std::clamp(static_cast<int>(memory->role), 0, 3);
        const int intentIndex = std::clamp(static_cast<int>(memory->intent), 0, 9);
        if (activeSample)
        {
            ++stats.roleSamples[roleIndex];
            ++stats.intentSamples[intentIndex];
            if (stats.lastRole >= 0 && stats.lastRole != roleIndex)
            {
                ++stats.roleChanges;
            }
            if (stats.lastIntent >= 0 && stats.lastIntent != intentIndex)
            {
                ++stats.intentChanges;
            }
            stats.lastRole = roleIndex;
            stats.lastIntent = intentIndex;
            if (memory->stuckTimer > 1.0f)
            {
                ++stats.stuckSamples;
            }
        }

        const int strategicGoalIndex = std::clamp(static_cast<int>(memory->currentPlan.goal), 0, 6);
        if (activeSample) ++stats.strategicGoalSamples[strategicGoalIndex];

        const Vector3 position = player.GetPosition();
        bool objectiveSampled = false;
        bool objectiveProgressed = false;
        bool objectiveNoProgress = false;
        bool objectiveRegressed = false;
        bool objectiveReached = false;
        bool strategicStalled = false;
        if (player.IsAlive() && !player.IsEliminated() && memory->hasObjectiveTarget)
        {
            objectiveSampled = true;
            ++stats.objectiveSamples;
            const float objectiveDistance = Distance3D(position, memory->objectiveTarget);
            const bool targetChanged = !stats.hasObjectiveSample
                || stats.lastObjectiveIntent != intentIndex
                || Distance3D(stats.lastObjectiveTarget, memory->objectiveTarget) > 5.0f;
            if (targetChanged)
            {
                if (stats.hasObjectiveSample)
                {
                    ++stats.objectiveTargetChanges;
                }
                stats.currentNoProgressSamples = 0;
                stats.closestObjectiveDistance = objectiveDistance;
            }
            else
            {
                const float progress = stats.lastObjectiveDistance - objectiveDistance;
                stats.netObjectiveDistanceChange += progress;
                stats.closestObjectiveDistance = std::min(stats.closestObjectiveDistance, objectiveDistance);
                if (objectiveDistance <= 4.0f)
                {
                    objectiveReached = true;
                    ++stats.objectiveReachedSamples;
                    stats.currentNoProgressSamples = 0;
                }
                else if (progress > 0.75f)
                {
                    objectiveProgressed = true;
                    ++stats.objectiveProgressSamples;
                    stats.currentNoProgressSamples = 0;
                }
                else
                {
                    ++stats.currentNoProgressSamples;
                    if (progress < -0.75f)
                    {
                        objectiveRegressed = true;
                        ++stats.objectiveRegressionSamples;
                    }
                    else
                    {
                        objectiveNoProgress = true;
                        ++stats.objectiveNoProgressSamples;
                    }
                    stats.maxNoProgressSamples = std::max(stats.maxNoProgressSamples, stats.currentNoProgressSamples);
                    if (stats.currentNoProgressSamples >= 10)
                    {
                        strategicStalled = true;
                        ++stats.strategicStallSamples;
                    }
                }
            }
            stats.hasObjectiveSample = true;
            stats.lastObjectiveTarget = memory->objectiveTarget;
            stats.lastObjectiveDistance = objectiveDistance;
            stats.lastObjectiveIntent = intentIndex;
        }

        bool midProximity = false;
        bool midReach = false;
        for (const Generator& generator : matchSimulation_.Generators())
        {
            if (generator.GetTeamId() != -1 || generator.GetType() != ResourceType::Crystal)
            {
                continue;
            }
            const float distance = Distance3D(position, Vector3 {
                static_cast<float>(generator.GetPosition().x),
                static_cast<float>(generator.GetPosition().y),
                static_cast<float>(generator.GetPosition().z)
            });
            midProximity = midProximity || distance <= 16.0f;
            midReach = midReach || distance <= 5.0f;
        }
        bool enemyBase = false;
        bool enemyCoreReach = false;
        for (const EnergyCore& enemyCore : matchSimulation_.Cores())
        {
            if (enemyCore.GetTeamId() == player.GetTeamId())
            {
                continue;
            }
            const float distance = Distance3D(position, world_.GridToWorld(enemyCore.GetBlockPosition()));
            enemyBase = enemyBase || distance <= 18.0f;
            enemyCoreReach = enemyCoreReach || distance <= 6.0f;
        }
        if (player.IsAlive() && !player.IsEliminated())
        {
            if (midProximity)
            {
                ++stats.midProximitySamples;
            }
            if (midReach)
            {
                ++stats.midReachSamples;
                if (stats.firstMidReachTime < 0.0f)
                {
                    stats.firstMidReachTime = matchSimulation_.MatchTimeSeconds();
                }
            }
            if (enemyBase)
            {
                ++stats.enemyBaseSamples;
                if (stats.firstEnemyBaseTime < 0.0f)
                {
                    stats.firstEnemyBaseTime = matchSimulation_.MatchTimeSeconds();
                }
            }
            if (enemyCoreReach)
            {
                ++stats.enemyCoreReachSamples;
            }
        }

        int authoredRouteAdvanceDelta = 0;
        if (memory->authoredRouteAdvances < stats.lastAuthoredRouteAdvances)
        {
            stats.lastAuthoredRouteAdvances = 0;
        }
        authoredRouteAdvanceDelta = memory->authoredRouteAdvances - stats.lastAuthoredRouteAdvances;
        stats.lastAuthoredRouteAdvances = memory->authoredRouteAdvances;
        stats.authoredRouteAdvances += authoredRouteAdvanceDelta;
        stats.authoredRouteMarkerCount = std::max(stats.authoredRouteMarkerCount, memory->authoredRouteMarkerCount);
        stats.maxAuthoredRouteIndex = std::max(stats.maxAuthoredRouteIndex, memory->authoredRouteIndex);
        if (memory->usingAuthoredRoute)
        {
            ++stats.authoredRouteSamples;
            if (memory->authoredRouteMarkerKind >= 0 && memory->authoredRouteMarkerKind < 4)
            {
                ++stats.authoredRouteMarkerSamples[memory->authoredRouteMarkerKind];
            }
        }

        if (activeSample && player.GetTeamId() >= 0 && player.GetTeamId() < 4)
        {
            AutomatchTeamStats& teamStats = automatch_.currentTeamStats[player.GetTeamId()];
            ++teamStats.samples;
            ++teamStats.roleSamples[roleIndex];
            ++teamStats.intentSamples[intentIndex];
            ++teamStats.strategicGoalSamples[strategicGoalIndex];
            teamStats.resourcesHeld[0] += player.GetInventory().GetResource(ResourceType::Iron);
            teamStats.resourcesHeld[1] += player.GetInventory().GetResource(ResourceType::Gold);
            teamStats.resourcesHeld[2] += player.GetInventory().GetResource(ResourceType::Crystal);
            if (objectiveSampled) ++teamStats.objectiveSamples;
            if (objectiveProgressed) ++teamStats.objectiveProgressSamples;
            if (objectiveNoProgress) ++teamStats.objectiveNoProgressSamples;
            if (objectiveRegressed) ++teamStats.objectiveRegressionSamples;
            if (objectiveReached) ++teamStats.objectiveReachedSamples;
            if (strategicStalled) ++teamStats.strategicStallSamples;
            if (player.IsAlive() && !player.IsEliminated() && midProximity) ++teamStats.midProximitySamples;
            if (player.IsAlive() && !player.IsEliminated() && midReach)
            {
                ++teamStats.midReachSamples;
                if (teamStats.firstMidReachTime < 0.0f)
                {
                    teamStats.firstMidReachTime = matchSimulation_.MatchTimeSeconds();
                }
            }
            if (player.IsAlive() && !player.IsEliminated() && enemyBase)
            {
                ++teamStats.enemyBaseSamples;
                if (teamStats.firstEnemyBaseTime < 0.0f)
                {
                    teamStats.firstEnemyBaseTime = matchSimulation_.MatchTimeSeconds();
                }
            }
            if (player.IsAlive() && !player.IsEliminated() && enemyCoreReach) ++teamStats.enemyCoreReachSamples;
            teamStats.authoredRouteAdvances += authoredRouteAdvanceDelta;
            teamStats.authoredRouteMarkerCount = std::max(teamStats.authoredRouteMarkerCount, memory->authoredRouteMarkerCount);
            teamStats.maxAuthoredRouteIndex = std::max(teamStats.maxAuthoredRouteIndex, memory->authoredRouteIndex);
            if (memory->usingAuthoredRoute)
            {
                ++teamStats.authoredRouteSamples;
                if (memory->authoredRouteMarkerKind >= 0 && memory->authoredRouteMarkerKind < 4)
                {
                    ++teamStats.authoredRouteMarkerSamples[memory->authoredRouteMarkerKind];
                }
            }
        }

        const EnergyCore* core = FindCoreByTeam(player.GetTeamId());
        const Vector3 base = core != nullptr ? world_.GridToWorld(core->GetBlockPosition()) : Vector3 {};
        const float fromBase = Distance3D(position, base);
        const float fromCenter = Distance3D(position, Vector3 {});
        if (!stats.hasMovementSample)
        {
            stats.hasMovementSample = true;
            stats.lastPosition = position;
            stats.minPosition = position;
            stats.maxPosition = position;
        }
        else
        {
            stats.totalDistance += Distance3D(stats.lastPosition, position);
            stats.lastPosition = position;
            stats.minPosition = Vector3 {
                std::min(stats.minPosition.x, position.x),
                std::min(stats.minPosition.y, position.y),
                std::min(stats.minPosition.z, position.z)
            };
            stats.maxPosition = Vector3 {
                std::max(stats.maxPosition.x, position.x),
                std::max(stats.maxPosition.y, position.y),
                std::max(stats.maxPosition.z, position.z)
            };
        }
        stats.maxDistanceFromBase = std::max(stats.maxDistanceFromBase, fromBase);
        if (matchSimulation_.MatchTimeSeconds() <= 60.0f)
        {
            stats.earlyMaxDistanceFromBase = std::max(stats.earlyMaxDistanceFromBase, fromBase);
        }
        stats.maxDistanceFromCenter = std::max(stats.maxDistanceFromCenter, fromCenter);
        const float samples = static_cast<float>(std::max(1, stats.samples));
        stats.averageDistanceFromBase += (fromBase - stats.averageDistanceFromBase) / samples;
        stats.averageDistanceFromCenter += (fromCenter - stats.averageDistanceFromCenter) / samples;
    }

    int leader = -1;
    float leaderScore = -1.0f;
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        const EnergyCore* core = FindCoreByTeam(teamId);
        int living = 0;
        for (const Player& player : players_)
        {
            if (player.GetTeamId() == teamId && player.IsAlive() && !player.IsEliminated()) ++living;
        }
        const float score = static_cast<float>(living * 100)
            + (core != nullptr && core->IsAlive() ? 500.0f + static_cast<float>(core->GetHealth()) : 0.0f)
            + static_cast<float>(automatch_.currentTeamStats[teamId].coreDamage) * 0.35f;
        if (score > leaderScore)
        {
            leaderScore = score;
            leader = teamId;
        }
    }
    if (automatch_.lastLeaderTeamId >= 0 && leader >= 0 && leader != automatch_.lastLeaderTeamId)
    {
        ++automatch_.leaderChanges;
    }
    automatch_.lastLeaderTeamId = leader;
}

Game::AutomatchBotStats* Game::FindAutomatchBotStats(const Player& player)
{
    if (!automatch_.active || IsLocallyPredicted(player.GetControlKind()))
    {
        return nullptr;
    }
    for (AutomatchBotStats& stats : automatch_.botStats)
    {
        if (stats.teamId == player.GetTeamId() && stats.name == player.GetName())
        {
            return &stats;
        }
    }
    automatch_.botStats.push_back(AutomatchBotStats { player.GetName(), player.GetTeamId() });
    return &automatch_.botStats.back();
}

void Game::RecordAutomatchExplosivePurchase(const Player& player, int shopChoice)
{
    RecordAutomatchShopPurchase(player, shopChoice);
}

void Game::RecordAutomatchShopPurchase(const Player& player, int shopChoice)
{
    AutomatchBotStats* stats = FindAutomatchBotStats(player);
    if (stats == nullptr)
    {
        return;
    }
    int itemCount = 1;
    for (const ShopItem& item : shop_.GetItems())
    {
        if (item.choice == shopChoice)
        {
            itemCount = item.grantCount;
            break;
        }
    }
    stats->shopPurchases[shopChoice] += itemCount;
    if (shopChoice == 105) ++stats->fireballsPurchased;
    if (shopChoice == 8) ++stats->tntPurchased;
}

void Game::RecordAutomatchShopUse(const Player& player, int shopChoice)
{
    AutomatchBotStats* stats = FindAutomatchBotStats(player);
    if (stats == nullptr || stats->shopUses[shopChoice] >= stats->shopPurchases[shopChoice])
    {
        return;
    }
    ++stats->shopUses[shopChoice];
}

void Game::RecordAutomatchExplosiveUse(const Player& player, bool fireball)
{
    RecordAutomatchShopUse(player, fireball ? 105 : 8);
    AutomatchBotStats* stats = FindAutomatchBotStats(player);
    if (stats == nullptr) return;
    if (fireball) stats->fireballsUsed = stats->shopUses[105];
    else stats->tntActivated = stats->shopUses[8];
}

void Game::RecordAutomatchDefenseBlockDestroyed(int ownerPlayerId, ExplosionBlockPolicy blockPolicy)
{
    if (!automatch_.active)
    {
        return;
    }
    const Player* player = matchSimulation_.GetPlayer(ownerPlayerId);
    if (player == nullptr)
    {
        return;
    }
    AutomatchBotStats* stats = FindAutomatchBotStats(*player);
    if (stats == nullptr)
    {
        return;
    }
    if (blockPolicy == ExplosionBlockPolicy::PreserveFortified)
    {
        ++stats->fireballDefenseBlocksDestroyed;
    }
    else if (blockPolicy == ExplosionBlockPolicy::PreserveReinforced)
    {
        ++stats->tntDefenseBlocksDestroyed;
    }
}

void Game::FinishAutomatchRun(bool timeout)
{
    SampleAutomatchBots();
    AutomatchRunStats run {};
    run.winnerTeamId = matchSimulation_.WinnerTeamId();
    run.duration = matchSimulation_.MatchTimeSeconds();
    run.timeout = timeout;
    run.firstCoreDamageTime = automatch_.currentFirstCoreDamageTime;
    run.timeline = automatch_.currentTimeline;
    run.memorableMoments = automatch_.currentMemorableMoments;
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        run.teamStats[teamId] = automatch_.currentTeamStats[teamId];
    }
    for (const Player& player : players_)
    {
        if (IsLocallyPredicted(player.GetControlKind()))
        {
            continue;
        }
        const int heroIndex = std::clamp(static_cast<int>(player.GetHeroId()), 0, HeroSystem::kHeroCount - 1);
        AutomatchHeroStats& heroStats = automatch_.heroStats[heroIndex];
        ++heroStats.appearances;
        if (run.winnerTeamId == player.GetTeamId())
        {
            ++heroStats.wins;
        }
        if (const PlayerMatchScore* score = FindPlayerScore(player.GetId()))
        {
            heroStats.kills += score->kills;
            heroStats.deaths += score->deaths;
            heroStats.coreDamage += score->coreDamage;
        }
    }

    const auto findBotStats = [this](const Player& player) -> AutomatchBotStats&
    {
        for (AutomatchBotStats& stats : automatch_.botStats)
        {
            if (stats.teamId == player.GetTeamId() && stats.name == player.GetName())
            {
                return stats;
            }
        }
        automatch_.botStats.push_back(AutomatchBotStats { player.GetName(), player.GetTeamId() });
        return automatch_.botStats.back();
    };

    for (const Player& player : players_)
    {
        if (IsLocallyPredicted(player.GetControlKind()))
        {
            continue;
        }
        const PlayerMatchScore* score = FindPlayerScore(player.GetId());
        if (score == nullptr)
        {
            continue;
        }

        AutomatchBotStats& stats = findBotStats(player);
        stats.kills += score->kills;
        stats.deaths += score->deaths;
        stats.finalDeaths += score->finalDeaths;
        stats.coreDamage += score->coreDamage;
        run.kills += score->kills;
        run.coreDamage += score->coreDamage;
        if (player.GetTeamId() >= 0 && player.GetTeamId() < 4)
        {
            AutomatchTeamStats& teamStats = run.teamStats[player.GetTeamId()];
            teamStats.kills += score->kills;
            teamStats.deaths += score->deaths;
            teamStats.finalDeaths += score->finalDeaths;
            teamStats.coreDamage += score->coreDamage;
        }
    }

    for (const Player& player : players_)
    {
        if (IsLocallyPredicted(player.GetControlKind()) || player.GetTeamId() < 0 || player.GetTeamId() >= 4)
        {
            continue;
        }

        AutomatchTeamStats& teamStats = run.teamStats[player.GetTeamId()];
        if (player.IsEliminated())
        {
            ++teamStats.eliminatedPlayers;
        }
        else
        {
            ++teamStats.alivePlayers;
        }
    }

    for (const EnergyCore& core : matchSimulation_.Cores())
    {
        const int teamId = core.GetTeamId();
        if (teamId < 0 || teamId >= 4)
        {
            continue;
        }
        AutomatchTeamStats& teamStats = run.teamStats[teamId];
        teamStats.coreAlive = core.IsAlive();
        teamStats.coreHealth = core.GetHealth();
        teamStats.coreMaxHealth = core.GetMaxHealth();
        if (!core.IsAlive())
        {
            ++run.coreDestroyedCount;
        }
    }
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        run.finalDeathCount += run.teamStats[teamId].finalDeaths;
    }
    if (timeout)
    {
        int aliveCores = 0;
        int teamsWithLives = 0;
        for (int teamId = 0; teamId < 4; ++teamId)
        {
            if (run.teamStats[teamId].coreAlive)
            {
                ++aliveCores;
            }
            if (run.teamStats[teamId].alivePlayers > 0)
            {
                ++teamsWithLives;
            }
        }
        run.finishReason = "тайм-аут: живых ядер " + std::to_string(aliveCores) + ", команд с жизнями "
            + std::to_string(teamsWithLives);
    }
    else
    {
        run.finishReason = run.winnerTeamId >= 0
            ? std::string(TeamName(run.winnerTeamId)) + " осталась последней командой"
            : "матч завершен без победителя";
    }

    if (!timeout && run.winnerTeamId >= 0 && !run.teamStats[run.winnerTeamId].coreAlive)
    {
        RecordMemorableMoment(
            "UnderdogVictory", run.winnerTeamId, -1, {},
            std::string(TeamName(run.winnerTeamId)) + " won after losing its Core",
            1.0f, true);
        ++automatch_.comebackSuccesses;
        run.memorableMoments = automatch_.currentMemorableMoments;
    }
    if (run.memorableMoments.empty())
    {
        ++automatch_.matchesWithoutMemorableMoment;
    }

    ++automatch_.completedRuns;
    automatch_.totalDuration += run.duration;
    automatch_.totalKills += run.kills;
    automatch_.totalCoreDamage += run.coreDamage;
    automatch_.totalFinalDeaths += run.finalDeathCount;
    automatch_.totalCoreDestroyed += run.coreDestroyedCount;
    if (timeout)
    {
        ++automatch_.timeouts;
    }
    else if (run.winnerTeamId >= 0 && run.winnerTeamId < 4)
    {
        ++automatch_.teamWins[run.winnerTeamId];
    }
    automatch_.runs.push_back(run);
    std::cout << "run " << automatch_.completedRuns << "/" << automatch_.targetRuns
              << ": winner=" << (run.winnerTeamId >= 0 ? TeamName(run.winnerTeamId) : "none")
              << " timeout=" << (run.timeout ? "yes" : "no")
              << " duration=" << run.duration
              << " kills=" << run.kills
              << " coreDamage=" << run.coreDamage << '\n';
}

void Game::WriteAutomatchStatsJson() const
{
    std::ofstream file(automatchStatsPath_, std::ios::trunc);
    if (!file)
    {
        return;
    }

    const float avgDuration = automatch_.completedRuns > 0
        ? automatch_.totalDuration / static_cast<float>(automatch_.completedRuns)
        : 0.0f;
    int totalVoidFalls = 0;
    int totalStuckSamples = 0;
    int totalStrategicStallSamples = 0;
    int totalMidReachSamples = 0;
    int totalEnemyBaseSamples = 0;
    int totalAuthoredRouteSamples = 0;
    int totalAuthoredRouteAdvances = 0;
    int totalIntentChanges = 0;
    int totalPlanStarts = 0;
    int totalPlanStageAdvances = 0;
    int totalPlanCompletions = 0;
    int totalPlanCancellations = 0;
    int totalPlanExpiryCancellations = 0;
    int totalPlanEvidenceCancellations = 0;
    int totalPlanRouteFailureCancellations = 0;
    int totalPlanVoidCancellations = 0;
    float totalPlanHoldSeconds = 0.0f;
    int totalRepeatedRouteDeaths = 0;
    int totalFireballsPurchased = 0;
    int totalFireballsUsed = 0;
    int totalFireballDefenseBlocksDestroyed = 0;
    int totalFireballBridgeOpportunities = 0;
    int totalFireballDefenseOpportunities = 0;
    int totalFireballTacticalUses = 0;
    int totalTntPurchased = 0;
    int totalTntActivated = 0;
    int totalTntDefenseBlocksDestroyed = 0;
    for (const AutomatchBotStats& stats : automatch_.botStats)
    {
        totalVoidFalls += stats.voidFalls;
        totalStuckSamples += stats.stuckSamples;
        totalStrategicStallSamples += stats.strategicStallSamples;
        totalMidReachSamples += stats.midReachSamples;
        totalEnemyBaseSamples += stats.enemyBaseSamples;
        totalAuthoredRouteSamples += stats.authoredRouteSamples;
        totalAuthoredRouteAdvances += stats.authoredRouteAdvances;
        totalIntentChanges += stats.intentChanges;
        totalPlanStarts += stats.planStarts;
        totalPlanStageAdvances += stats.planStageAdvances;
        totalPlanCompletions += stats.planCompletions;
        totalPlanCancellations += stats.planCancellations;
        totalPlanExpiryCancellations += stats.planExpiryCancellations;
        totalPlanEvidenceCancellations += stats.planEvidenceCancellations;
        totalPlanRouteFailureCancellations += stats.planRouteFailureCancellations;
        totalPlanVoidCancellations += stats.planVoidCancellations;
        totalPlanHoldSeconds += stats.planHoldSeconds;
        totalRepeatedRouteDeaths += stats.repeatedRouteDeaths;
        totalFireballsPurchased += stats.fireballsPurchased;
        totalFireballsUsed += stats.fireballsUsed;
        totalFireballDefenseBlocksDestroyed += stats.fireballDefenseBlocksDestroyed;
        totalFireballBridgeOpportunities += stats.fireballBridgeOpportunities;
        totalFireballDefenseOpportunities += stats.fireballDefenseOpportunities;
        totalFireballTacticalUses += stats.fireballTacticalUses;
        totalTntPurchased += stats.tntPurchased;
        totalTntActivated += stats.tntActivated;
        totalTntDefenseBlocksDestroyed += stats.tntDefenseBlocksDestroyed;
    }
    const float runs = static_cast<float>(std::max(1, automatch_.completedRuns));
    const float fitness =
        static_cast<float>(automatch_.totalCoreDestroyed) * 520.0f
        + static_cast<float>(automatch_.totalCoreDamage) * 1.35f
        + static_cast<float>(automatch_.totalKills) * 30.0f
        + static_cast<float>(automatch_.totalFinalDeaths) * 70.0f
        - static_cast<float>(automatch_.timeouts) * 420.0f
        - static_cast<float>(totalVoidFalls) * 55.0f
        - static_cast<float>(totalStuckSamples) * 2.2f
        - static_cast<float>(totalIntentChanges) * 0.55f
        - avgDuration * 0.35f * runs;
    file << "{\n";
    file << "  \"summary\": {\n";
    file << "    \"completedRuns\": " << automatch_.completedRuns << ",\n";
    file << "    \"targetRuns\": " << automatch_.targetRuns << ",\n";
    file << "    \"biome\": \"" << JsonEscape(ArenaBiomeName()) << "\",\n";
    file << "    \"botStrategyProfile\": \"" << JsonEscape(BotStrategyProfileName()) << "\",\n";
    file << "    \"botTuningSource\": \"" << JsonEscape(botTuningSource_) << "\",\n";
    file << "    \"fitness\": " << fitness << ",\n";
    file << "    \"fitnessPerRun\": " << fitness / runs << ",\n";
    file << "    \"timeouts\": " << automatch_.timeouts << ",\n";
    file << "    \"averageDurationSeconds\": " << avgDuration << ",\n";
    file << "    \"totalKills\": " << automatch_.totalKills << ",\n";
    file << "    \"totalCoreDamage\": " << automatch_.totalCoreDamage << ",\n";
    file << "    \"totalFinalDeaths\": " << automatch_.totalFinalDeaths << ",\n";
    file << "    \"totalCoreDestroyed\": " << automatch_.totalCoreDestroyed << ",\n";
    file << "    \"totalVoidFalls\": " << totalVoidFalls << ",\n";
    file << "    \"totalStuckSamples\": " << totalStuckSamples << ",\n";
    file << "    \"totalStrategicStallSamples\": " << totalStrategicStallSamples << ",\n";
    file << "    \"totalMidReachSamples\": " << totalMidReachSamples << ",\n";
    file << "    \"totalEnemyBaseSamples\": " << totalEnemyBaseSamples << ",\n";
    file << "    \"totalAuthoredRouteSamples\": " << totalAuthoredRouteSamples << ",\n";
    file << "    \"totalAuthoredRouteAdvances\": " << totalAuthoredRouteAdvances << ",\n";
    file << "    \"totalIntentChanges\": " << totalIntentChanges << ",\n";
    file << "    \"strategy\": {";
    file << "\"planStarts\": " << totalPlanStarts << ", ";
    file << "\"stageAdvances\": " << totalPlanStageAdvances << ", ";
    file << "\"completions\": " << totalPlanCompletions << ", ";
    file << "\"cancellations\": " << totalPlanCancellations << ", ";
    file << "\"cancellationsByReason\": {\"expired\": " << totalPlanExpiryCancellations
         << ", \"evidence\": " << totalPlanEvidenceCancellations
         << ", \"routeFailure\": " << totalPlanRouteFailureCancellations
         << ", \"void\": " << totalPlanVoidCancellations << "}, ";
    file << "\"averageHoldSeconds\": " << (totalPlanStarts > 0 ? totalPlanHoldSeconds / static_cast<float>(totalPlanStarts) : 0.0f) << ", ";
    file << "\"retreats\": " << automatch_.retreats << ", ";
    file << "\"repeatedRouteDeaths\": " << totalRepeatedRouteDeaths << ", ";
    file << "\"leaderChanges\": " << automatch_.leaderChanges << ", ";
    file << "\"comebackAttempts\": " << automatch_.comebackAttempts << ", ";
    file << "\"comebackSuccesses\": " << automatch_.comebackSuccesses << "},\n";
    file << "    \"teamplay\": {";
    file << "\"jointAttacks\": " << automatch_.jointAttacks << ", ";
    file << "\"coreDefenseResponses\": " << automatch_.coreDefenseResponses << ", ";
    file << "\"coreFortifications\": " << automatch_.coreFortifications << "},\n";
    file << "    \"economy\": {";
    file << "\"resourcesLost\": " << automatch_.resourcesLost << ", ";
    file << "\"purchases\": {\"blocks\": " << automatch_.purchasesByCategory[0]
         << ", \"combat\": " << automatch_.purchasesByCategory[1]
         << ", \"utility\": " << automatch_.purchasesByCategory[2]
         << ", \"team\": " << automatch_.purchasesByCategory[3]
         << ", \"ranged\": " << automatch_.purchasesByCategory[4] << "}},\n";
    file << "    \"shopExplosives\": {";
    file << "\"fireball\": {\"purchased\": " << totalFireballsPurchased
         << ", \"used\": " << totalFireballsUsed
         << ", \"enemyDefenseBlocksDestroyed\": " << totalFireballDefenseBlocksDestroyed
         << ", \"bridgeOpportunities\": " << totalFireballBridgeOpportunities
         << ", \"defenseOpportunities\": " << totalFireballDefenseOpportunities
         << ", \"tacticalUses\": " << totalFireballTacticalUses << "}, ";
    file << "\"tnt\": {\"purchased\": " << totalTntPurchased
         << ", \"activated\": " << totalTntActivated
         << ", \"enemyDefenseBlocksDestroyed\": " << totalTntDefenseBlocksDestroyed << "}},\n";
    file << "    \"memorableMoments\": {\n";
    file << "      \"matchesWithoutSignificantMoment\": " << automatch_.matchesWithoutMemorableMoment << ",\n";
    file << "      \"byCategory\": {";
    static constexpr const char* kMomentCategories[] {
        "CoreClutchDefense", "LastSecondCoreSave", "SuccessfulFlank", "BridgeFight",
        "VoidEscape", "MultiKill", "AbilityCombo", "SacrificeForCore", "Comeback",
        "BaseTrade", "ResourceHeist", "LongRangeFinish", "DefenseBreakthrough",
        "FailedGreedyPush", "EmergencyBridge", "Revenge", "UnderdogVictory"
    };
    for (std::size_t categoryIndex = 0; categoryIndex < std::size(kMomentCategories); ++categoryIndex)
    {
        const auto found = automatch_.memorableMomentCounts.find(kMomentCategories[categoryIndex]);
        file << (categoryIndex == 0 ? "" : ", ") << "\"" << kMomentCategories[categoryIndex] << "\": "
             << (found != automatch_.memorableMomentCounts.end() ? found->second : 0);
    }
    file << "}\n";
    file << "    },\n";
    file << "    \"coreCollapseSeconds\": " << coreCollapseSeconds_ << ",\n";
    file << "    \"navigation\": {\n";
    file << "      \"routeGraphNodes\": " << routeGraph_.NodeCount() << ",\n";
    file << "      \"routeGraphEdges\": " << routeGraph_.EdgeCount() << ",\n";
    file << "      \"pathRequests\": " << navigationMetrics_.pathRequests << ",\n";
    file << "      \"successfulPaths\": " << navigationMetrics_.successfulPaths << ",\n";
    file << "      \"partialPaths\": " << navigationMetrics_.partialPaths << ",\n";
    file << "      \"failedPathRequests\": " << navigationMetrics_.failedPathRequests << ",\n";
    file << "      \"pathResultsByStatus\": {";
    for (std::size_t status = 0; status < kNavigationSearchStatusCount; ++status)
    {
        if (status > 0) file << ", ";
        file << "\"" << ToString(static_cast<NavigationSearchStatus>(status)) << "\": "
             << navigationMetrics_.pathResultsByStatus[status];
    }
    file << "},\n";
    file << "      \"blockedStartDeferrals\": " << navigationMetrics_.blockedStartDeferrals << ",\n";
    file << "      \"blockedStartRecoveries\": " << navigationMetrics_.blockedStartRecoveries << ",\n";
    file << "      \"averageExpandedNodes\": " << navigationMetrics_.AverageExpandedNodes() << ",\n";
    file << "      \"averageGeneratedNodes\": " << navigationMetrics_.AverageGeneratedNodes() << ",\n";
    file << "      \"heapDecreaseKeys\": " << navigationMetrics_.heapDecreaseKeys << ",\n";
    file << "      \"peakOpenNodes\": " << navigationMetrics_.peakOpenNodes << ",\n";
    file << "      \"longSprintActions\": " << navigationMetrics_.longSprintActions << ",\n";
    file << "      \"diagonalActions\": " << navigationMetrics_.diagonalActions << ",\n";
    file << "      \"corridorConstrainedSearches\": " << navigationMetrics_.corridorConstrainedSearches << ",\n";
    file << "      \"corridorFallbackSearches\": " << navigationMetrics_.corridorFallbackSearches << ",\n";
    file << "      \"corridorRejectedNodes\": " << navigationMetrics_.corridorRejectedNodes << ",\n";
    file << "      \"segmentRepairAttempts\": " << navigationMetrics_.segmentRepairAttempts << ",\n";
    file << "      \"segmentRepairSuccesses\": " << navigationMetrics_.segmentRepairSuccesses << ",\n";
    file << "      \"segmentRepairFailures\": " << navigationMetrics_.segmentRepairFailures << ",\n";
    file << "      \"segmentRepairReusedActions\": " << navigationMetrics_.segmentRepairReusedActions << ",\n";
    file << "      \"segmentRepairExpandedNodes\": " << navigationMetrics_.segmentRepairExpandedNodes << ",\n";
    file << "      \"segmentRepairMilliseconds\": " << navigationMetrics_.segmentRepairMilliseconds << ",\n";
    file << "      \"dirtyRegionFastAccepts\": " << navigationMetrics_.dirtyRegionFastAccepts << ",\n";
    file << "      \"dirtyRegionIntersectValidations\": " << navigationMetrics_.dirtyRegionIntersectValidations << ",\n";
    file << "      \"dirtyRegionHistoryMisses\": " << navigationMetrics_.dirtyRegionHistoryMisses << ",\n";
    file << "      \"averagePathfindingMilliseconds\": " << navigationMetrics_.AveragePathfindingMilliseconds() << ",\n";
    file << "      \"repaths\": " << navigationMetrics_.repaths << ",\n";
    file << "      \"movementFailures\": " << navigationMetrics_.movementFailures << ",\n";
    file << "      \"movementFailuresByMovement\": {";
    for (std::size_t movement = 0; movement < kMovementTypeCount; ++movement)
    {
        if (movement > 0) file << ", ";
        file << "\"" << ToString(static_cast<MovementType>(movement)) << "\": "
             << navigationMetrics_.movementFailuresByMovement[movement];
    }
    file << "},\n";
    file << "      \"stuckEvents\": " << navigationMetrics_.stuckEvents << ",\n";
    file << "      \"stuckEventsByMovement\": {";
    for (std::size_t movement = 0; movement < kMovementTypeCount; ++movement)
    {
        if (movement > 0) file << ", ";
        file << "\"" << ToString(static_cast<MovementType>(movement)) << "\": "
             << navigationMetrics_.stuckEventsByMovement[movement];
    }
    file << "},\n";
    file << "      \"recenterRecoveryAttempts\": " << navigationMetrics_.recenterRecoveryAttempts << ",\n";
    file << "      \"recenterRecoveryRetries\": " << navigationMetrics_.recenterRecoveryRetries << ",\n";
    file << "      \"actionRecoveryAttempts\": " << navigationMetrics_.actionRecoveryAttempts << ",\n";
    file << "      \"actionRecoverySuccesses\": " << navigationMetrics_.actionRecoverySuccesses << ",\n";
    file << "      \"actionRecoveryFailures\": " << navigationMetrics_.actionRecoveryFailures << ",\n";
    file << "      \"gapToBridgeAttempts\": " << navigationMetrics_.gapToBridgeAttempts << ",\n";
    file << "      \"gapToBridgeSuccesses\": " << navigationMetrics_.gapToBridgeSuccesses << ",\n";
    file << "      \"gapToBridgeFailures\": " << navigationMetrics_.gapToBridgeFailures << ",\n";
    file << "      \"routeAbandonments\": " << navigationMetrics_.routeAbandonments << ",\n";
    file << "      \"bridgeBlocksUsed\": " << navigationMetrics_.bridgeBlocksUsed << ",\n";
    file << "      \"blocksBrokenForNavigation\": " << navigationMetrics_.blocksBrokenForNavigation << ",\n";
    file << "      \"failedJumps\": " << navigationMetrics_.failedJumps << ",\n";
    file << "      \"successfulGapJumps\": " << navigationMetrics_.successfulGapJumps << ",\n";
    file << "      \"recoveredJumpLandings\": " << navigationMetrics_.recoveredJumpLandings << ",\n";
    file << "      \"emergencySaveAttempts\": " << navigationMetrics_.emergencySaveAttempts << ",\n";
    file << "      \"emergencySaves\": " << navigationMetrics_.emergencySaves << ",\n";
    file << "      \"voidDeaths\": " << totalVoidFalls << ",\n";
    file << "      \"unforcedVoidDeaths\": " << navigationMetrics_.unforcedVoidDeaths << ",\n";
    file << "      \"combatAttributedVoidDeaths\": " << navigationMetrics_.combatAttributedVoidDeaths << ",\n";
    file << "      \"voidDeathsOutsideNavigation\": " << navigationMetrics_.voidDeathsOutsideNavigation << ",\n";
    file << "      \"voidDeathsByMovement\": {";
    for (std::size_t movement = 0; movement < kMovementTypeCount; ++movement)
    {
        if (movement > 0) file << ", ";
        file << "\"" << ToString(static_cast<MovementType>(movement)) << "\": "
             << navigationMetrics_.voidDeathsByMovement[movement];
    }
    file << "},\n";
    file << "      \"unforcedVoidDeathsByMovement\": {";
    for (std::size_t movement = 0; movement < kMovementTypeCount; ++movement)
    {
        if (movement > 0) file << ", ";
        file << "\"" << ToString(static_cast<MovementType>(movement)) << "\": "
             << navigationMetrics_.unforcedVoidDeathsByMovement[movement];
    }
    file << "},\n";
    file << "      \"momentumPreservedTransitions\": " << navigationMetrics_.momentumPreservedTransitions << ",\n";
    file << "      \"edgeBrakeActions\": " << navigationMetrics_.edgeBrakeActions << ",\n";
    file << "      \"edgeBrakeCompletions\": " << navigationMetrics_.edgeBrakeCompletions << ",\n";
    file << "      \"cancelledActions\": " << navigationMetrics_.cancelledActions << ",\n";
    file << "      \"corridorPlans\": " << navigationMetrics_.corridorPlans << ",\n";
    file << "      \"corridorReuses\": " << navigationMetrics_.corridorReuses << ",\n";
    file << "      \"corridorAdvances\": " << navigationMetrics_.corridorAdvances << ",\n";
    file << "      \"corridorFailures\": " << navigationMetrics_.corridorFailures << ",\n";
    file << "      \"routeBridgeSegmentsStarted\": " << navigationMetrics_.routeBridgeSegmentsStarted << ",\n";
    file << "      \"routeBridgeSegmentsCompleted\": " << navigationMetrics_.routeBridgeSegmentsCompleted << ",\n";
    file << "      \"routeBridgeFollowersHeld\": " << navigationMetrics_.routeBridgeFollowersHeld << ",\n";
    file << "      \"averageRouteEfficiency\": " << navigationMetrics_.AverageRouteEfficiency() << ",\n";
    file << "      \"repathReasons\": {\n";
    for (std::size_t reason = 1; reason < kRepathReasonCount; ++reason)
    {
        file << "        \"" << ToString(static_cast<RepathReason>(reason)) << "\": "
             << navigationMetrics_.repathReasons[reason]
             << (reason + 1 < kRepathReasonCount ? "," : "") << "\n";
    }
    file << "      }\n";
    file << "    },\n";
    const BotTuningGenome effectiveTiming = ScaledBotTuningForTeam(0);
    file << "    \"effectiveStrategicTimings\": {";
    file << "\"earlyEconomy\": " << effectiveTiming.earlyEconomySeconds << ", ";
    file << "\"pressure\": " << effectiveTiming.pressurePhaseSeconds << ", ";
    file << "\"latePressure\": " << effectiveTiming.latePressureSeconds << ", ";
    file << "\"allIn\": " << effectiveTiming.allInSeconds << "},\n";
    file << "    \"botTuningByTeam\": [\n";
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        const BotTuningGenome& genome = botTuningByTeam_[teamId];
        file << "      {";
        file << "\"teamId\": " << teamId << ", ";
        file << "\"id\": \"" << JsonEscape(genome.id) << "\", ";
        file << "\"schemaVersion\": " << genome.schemaVersion << ", ";
        file << "\"generation\": " << genome.generation << ", ";
        file << "\"hash\": " << BotTuningGenomeHash(genome) << ", ";
        file << "\"navigation\": {";
        file << "\"runupDistanceBlocks\": " << genome.navigationRunupDistanceBlocks << ", ";
        file << "\"takeoffDelaySeconds\": " << genome.navigationTakeoffDelaySeconds << ", ";
        file << "\"takeoffEdgeOffsetBlocks\": " << genome.navigationTakeoffEdgeOffsetBlocks << ", ";
        file << "\"takeoffGapScale\": " << genome.navigationTakeoffGapScale << ", ";
        file << "\"airControlScale\": " << genome.navigationAirControlScale << ", ";
        file << "\"landingCorrectionGain\": " << genome.navigationLandingCorrectionGain << ", ";
        file << "\"fallRiskPenalty\": " << genome.navigationFallRiskPenalty << "}";
        file << "}" << (teamId < 3 ? "," : "") << "\n";
    }
    file << "    ],\n";
    file << "    \"winsByTeam\": {\n";
    file << "      \"Red\": " << automatch_.teamWins[0] << ",\n";
    file << "      \"Blue\": " << automatch_.teamWins[1] << ",\n";
    file << "      \"Green\": " << automatch_.teamWins[2] << ",\n";
    file << "      \"Yellow\": " << automatch_.teamWins[3] << "\n";
    file << "    }\n";
    file << "  },\n";

    file << "  \"runs\": [\n";
    for (std::size_t i = 0; i < automatch_.runs.size(); ++i)
    {
        const AutomatchRunStats& run = automatch_.runs[i];
        file << "    {\n";
        file << "      \"index\": " << (i + 1) << ",\n";
        file << "      \"winnerTeamId\": " << run.winnerTeamId << ",\n";
        file << "      \"winnerTeam\": \"" << JsonEscape(run.winnerTeamId >= 0 ? TeamName(run.winnerTeamId) : "Нет") << "\",\n";
        file << "      \"durationSeconds\": " << run.duration << ",\n";
        file << "      \"timeout\": " << (run.timeout ? "true" : "false") << ",\n";
        file << "      \"finishReason\": \"" << JsonEscape(run.finishReason) << "\",\n";
        file << "      \"firstCoreDamageTime\": " << run.firstCoreDamageTime << ",\n";
        file << "      \"kills\": " << run.kills << ",\n";
        file << "      \"finalDeaths\": " << run.finalDeathCount << ",\n";
        file << "      \"coreDamage\": " << run.coreDamage << ",\n";
        file << "      \"coresDestroyed\": " << run.coreDestroyedCount << ",\n";
        file << "      \"teams\": [\n";
        for (int teamId = 0; teamId < 4; ++teamId)
        {
            const AutomatchTeamStats& teamStats = run.teamStats[teamId];
            const float resourceSamples = static_cast<float>(std::max(1, teamStats.samples));
            file << "        {\n";
            file << "          \"teamId\": " << teamId << ",\n";
            file << "          \"team\": \"" << JsonEscape(TeamName(teamId)) << "\",\n";
            file << "          \"botTuningId\": \"" << JsonEscape(botTuningByTeam_[teamId].id) << "\",\n";
            file << "          \"botTuningHash\": " << BotTuningGenomeHash(botTuningByTeam_[teamId]) << ",\n";
            file << "          \"kills\": " << teamStats.kills << ",\n";
            file << "          \"deaths\": " << teamStats.deaths << ",\n";
            file << "          \"finalDeaths\": " << teamStats.finalDeaths << ",\n";
            file << "          \"coreDamage\": " << teamStats.coreDamage << ",\n";
            file << "          \"alivePlayers\": " << teamStats.alivePlayers << ",\n";
            file << "          \"eliminatedPlayers\": " << teamStats.eliminatedPlayers << ",\n";
            file << "          \"coreAlive\": " << (teamStats.coreAlive ? "true" : "false") << ",\n";
            file << "          \"coreHealth\": " << teamStats.coreHealth << ",\n";
            file << "          \"coreMaxHealth\": " << teamStats.coreMaxHealth << ",\n";
            file << "          \"averageResourcesHeld\": {";
            file << "\"Iron\": " << static_cast<float>(teamStats.resourcesHeld[0]) / resourceSamples << ", ";
            file << "\"Gold\": " << static_cast<float>(teamStats.resourcesHeld[1]) / resourceSamples << ", ";
            file << "\"Crystal\": " << static_cast<float>(teamStats.resourcesHeld[2]) / resourceSamples << "},\n";
            file << "          \"roleSamples\": {";
            file << "\"Defender\": " << teamStats.roleSamples[0] << ", ";
            file << "\"Rusher\": " << teamStats.roleSamples[1] << ", ";
            file << "\"Collector\": " << teamStats.roleSamples[2] << ", ";
            file << "\"Fighter\": " << teamStats.roleSamples[3] << "},\n";
            file << "          \"intentSamples\": {\n";
            for (int intent = 0; intent < 10; ++intent)
            {
                file << "            \"" << ToString(static_cast<BotIntent>(intent)) << "\": " << teamStats.intentSamples[intent]
                    << (intent < 9 ? "," : "") << "\n";
            }
            file << "          },\n";
            file << "          \"strategicGoalSamples\": {\n";
            for (int goal = 0; goal < 7; ++goal)
            {
                file << "            \"" << ToString(static_cast<StrategicGoal>(goal)) << "\": " << teamStats.strategicGoalSamples[goal]
                    << (goal < 6 ? "," : "") << "\n";
            }
            file << "          },\n";
            file << "          \"objectiveProgress\": {";
            file << "\"samples\": " << teamStats.objectiveSamples << ", ";
            file << "\"progress\": " << teamStats.objectiveProgressSamples << ", ";
            file << "\"noProgress\": " << teamStats.objectiveNoProgressSamples << ", ";
            file << "\"regression\": " << teamStats.objectiveRegressionSamples << ", ";
            file << "\"reached\": " << teamStats.objectiveReachedSamples << ", ";
            file << "\"strategicStall\": " << teamStats.strategicStallSamples << "},\n";
            file << "          \"mapControl\": {";
            file << "\"midProximitySamples\": " << teamStats.midProximitySamples << ", ";
            file << "\"midReachSamples\": " << teamStats.midReachSamples << ", ";
            file << "\"enemyBaseSamples\": " << teamStats.enemyBaseSamples << ", ";
            file << "\"enemyCoreReachSamples\": " << teamStats.enemyCoreReachSamples << ", ";
            file << "\"firstMidReachTime\": " << teamStats.firstMidReachTime << ", ";
            file << "\"firstEnemyBaseTime\": " << teamStats.firstEnemyBaseTime << "},\n";
            file << "          \"authoredRoute\": {";
            file << "\"samples\": " << teamStats.authoredRouteSamples << ", ";
            file << "\"advances\": " << teamStats.authoredRouteAdvances << ", ";
            file << "\"maxIndex\": " << teamStats.maxAuthoredRouteIndex << ", ";
            file << "\"markerCount\": " << teamStats.authoredRouteMarkerCount << ", ";
            file << "\"markerSamples\": {";
            file << "\"Rally\": " << teamStats.authoredRouteMarkerSamples[0] << ", ";
            file << "\"Lane\": " << teamStats.authoredRouteMarkerSamples[1] << ", ";
            file << "\"Chokepoint\": " << teamStats.authoredRouteMarkerSamples[2] << ", ";
            file << "\"Highground\": " << teamStats.authoredRouteMarkerSamples[3] << "}}\n";
            file << "        }" << (teamId < 3 ? "," : "") << "\n";
        }
        file << "      ],\n";
        file << "      \"timeline\": [\n";
        for (std::size_t eventIndex = 0; eventIndex < run.timeline.size(); ++eventIndex)
        {
            const AutomatchTimelineEvent& event = run.timeline[eventIndex];
            file << "        {";
            file << "\"time\": " << event.time << ", ";
            file << "\"type\": \"" << JsonEscape(event.type) << "\", ";
            file << "\"teamId\": " << event.teamId << ", ";
            file << "\"actorTeamId\": " << event.actorTeamId << ", ";
            file << "\"actorId\": " << event.actorId << ", ";
            file << "\"targetId\": " << event.targetId << ", ";
            file << "\"value\": " << event.value << ", ";
            file << "\"text\": \"" << JsonEscape(event.text) << "\"";
            file << "}" << (eventIndex + 1 < run.timeline.size() ? "," : "") << "\n";
        }
        file << "      ],\n";
        file << "      \"memorableMoments\": [\n";
        for (std::size_t momentIndex = 0; momentIndex < run.memorableMoments.size(); ++momentIndex)
        {
            const MemorableMoment& moment = run.memorableMoments[momentIndex];
            file << "        {";
            file << "\"tick\": " << moment.tick << ", ";
            file << "\"seed\": " << moment.seed << ", ";
            file << "\"map\": \"" << JsonEscape(moment.map) << "\", ";
            file << "\"category\": \"" << JsonEscape(moment.category) << "\", ";
            file << "\"primaryTeamId\": " << moment.primaryTeamId << ", ";
            file << "\"secondaryTeamId\": " << moment.secondaryTeamId << ", ";
            file << "\"participants\": [";
            for (std::size_t p = 0; p < moment.participants.size(); ++p)
            {
                file << (p == 0 ? "" : ", ") << moment.participants[p];
            }
            file << "], \"coreHealth\": [";
            for (int teamId = 0; teamId < 4; ++teamId)
            {
                file << (teamId == 0 ? "" : ", ") << moment.coreHealth[teamId];
            }
            file << "], \"actorHealth\": " << moment.actorHealth;
            file << ", \"targetHealth\": " << moment.targetHealth;
            file << ", \"carriedResourceValue\": " << moment.carriedResourceValue;
            file << ", \"precedingActions\": [";
            for (std::size_t action = 0; action < moment.precedingActions.size(); ++action)
            {
                file << (action == 0 ? "" : ", ") << "\"" << JsonEscape(moment.precedingActions[action]) << "\"";
            }
            file << "], \"description\": \"" << JsonEscape(moment.description) << "\"";
            file << ", \"significance\": " << moment.significance;
            file << ", \"botDecisionDriven\": " << (moment.botDecisionDriven ? "true" : "false");
            file << "}" << (momentIndex + 1 < run.memorableMoments.size() ? "," : "") << "\n";
        }
        file << "      ]\n";
        file << "    }" << (i + 1 < automatch_.runs.size() ? "," : "") << "\n";
    }
    file << "  ],\n";

    file << "  \"heroes\": [\n";
    for (int heroIndex = 0; heroIndex < HeroSystem::kHeroCount; ++heroIndex)
    {
        const HeroId heroId = HeroSystem::IdFromIndex(heroIndex);
        const AutomatchHeroStats& stats = automatch_.heroStats[heroIndex];
        file << "    {\n";
        file << "      \"hero\": \"" << JsonEscape(HeroSystem::GetDefinition(heroId).name) << "\",\n";
        file << "      \"appearances\": " << stats.appearances << ",\n";
        file << "      \"wins\": " << stats.wins << ",\n";
        file << "      \"winrate\": " << (stats.appearances > 0 ? static_cast<float>(stats.wins) / static_cast<float>(stats.appearances) : 0.0f) << ",\n";
        file << "      \"kills\": " << stats.kills << ",\n";
        file << "      \"deaths\": " << stats.deaths << ",\n";
        file << "      \"coreDamage\": " << stats.coreDamage << "\n";
        file << "    }" << (heroIndex + 1 < HeroSystem::kHeroCount ? "," : "") << "\n";
    }
    file << "  ],\n";

    file << "  \"bots\": [\n";
    for (std::size_t i = 0; i < automatch_.botStats.size(); ++i)
    {
        const AutomatchBotStats& stats = automatch_.botStats[i];
        file << "    {\n";
        file << "      \"name\": \"" << JsonEscape(stats.name) << "\",\n";
        file << "      \"teamId\": " << stats.teamId << ",\n";
            file << "      \"team\": \"" << JsonEscape(stats.teamId >= 0 ? TeamName(stats.teamId) : "Неизвестно") << "\",\n";
        file << "      \"samples\": " << stats.samples << ",\n";
        file << "      \"roleChanges\": " << stats.roleChanges << ",\n";
        file << "      \"intentChanges\": " << stats.intentChanges << ",\n";
        file << "      \"stuckSamples\": " << stats.stuckSamples << ",\n";
        file << "      \"voidFalls\": " << stats.voidFalls << ",\n";
        file << "      \"kills\": " << stats.kills << ",\n";
        file << "      \"deaths\": " << stats.deaths << ",\n";
        file << "      \"finalDeaths\": " << stats.finalDeaths << ",\n";
        file << "      \"coreDamage\": " << stats.coreDamage << ",\n";
        file << "      \"archetype\": \"" << JsonEscape(stats.archetype) << "\",\n";
        file << "      \"shopItems\": [\n";
        const std::vector<ShopItem> shopItems = shop_.GetItems();
        for (std::size_t itemIndex = 0; itemIndex < shopItems.size(); ++itemIndex)
        {
            const ShopItem& item = shopItems[itemIndex];
            const auto purchases = stats.shopPurchases.find(item.choice);
            const auto uses = stats.shopUses.find(item.choice);
            file << "        {\"choice\": " << item.choice
                 << ", \"category\": \"" << JsonEscape(item.category)
                 << "\", \"name\": \"" << JsonEscape(item.name)
                 << "\", \"usageMode\": \"" << ShopUsageModeName(item.usageMode)
                 << "\", \"grantCount\": " << item.grantCount
                 << ", \"purchased\": " << (purchases != stats.shopPurchases.end() ? purchases->second : 0)
                 << ", \"used\": " << (uses != stats.shopUses.end() ? uses->second : 0) << "}"
                 << (itemIndex + 1 < shopItems.size() ? "," : "") << "\n";
        }
        file << "      ],\n";
        file << "      \"shopExplosives\": {";
        file << "\"fireball\": {\"purchased\": " << stats.fireballsPurchased
             << ", \"used\": " << stats.fireballsUsed
             << ", \"enemyDefenseBlocksDestroyed\": " << stats.fireballDefenseBlocksDestroyed
             << ", \"bridgeOpportunities\": " << stats.fireballBridgeOpportunities
             << ", \"defenseOpportunities\": " << stats.fireballDefenseOpportunities
             << ", \"tacticalUses\": " << stats.fireballTacticalUses << "}, ";
        file << "\"tnt\": {\"purchased\": " << stats.tntPurchased
             << ", \"activated\": " << stats.tntActivated
             << ", \"enemyDefenseBlocksDestroyed\": " << stats.tntDefenseBlocksDestroyed << "}},\n";
        file << "      \"strategy\": {";
        file << "\"planStarts\": " << stats.planStarts << ", ";
        file << "\"stageAdvances\": " << stats.planStageAdvances << ", ";
        file << "\"completions\": " << stats.planCompletions << ", ";
        file << "\"cancellations\": " << stats.planCancellations << ", ";
        file << "\"cancellationsByReason\": {\"expired\": " << stats.planExpiryCancellations
             << ", \"evidence\": " << stats.planEvidenceCancellations
             << ", \"routeFailure\": " << stats.planRouteFailureCancellations
             << ", \"void\": " << stats.planVoidCancellations << "}, ";
        file << "\"averageHoldSeconds\": " << (stats.planStarts > 0 ? stats.planHoldSeconds / static_cast<float>(stats.planStarts) : 0.0f) << ", ";
        file << "\"retreats\": " << stats.retreats << ", ";
        file << "\"repeatedRouteDeaths\": " << stats.repeatedRouteDeaths << "},\n";
        const float inventorySamples = static_cast<float>(std::max(1, stats.samples));
        file << "      \"inventory\": {\n";
        file << "        \"averageBlocks\": " << static_cast<float>(stats.blocksHeld) / inventorySamples << ",\n";
        file << "        \"maxBlocks\": " << stats.maxBlocksHeld << ",\n";
        file << "        \"finalBlocks\": " << stats.finalBlocksHeld << ",\n";
        file << "        \"averageResources\": {\"Iron\": " << static_cast<float>(stats.resourcesHeld[0]) / inventorySamples
            << ", \"Gold\": " << static_cast<float>(stats.resourcesHeld[1]) / inventorySamples
            << ", \"Crystal\": " << static_cast<float>(stats.resourcesHeld[2]) / inventorySamples << "},\n";
        file << "        \"finalResources\": {\"Iron\": " << stats.finalResourcesHeld[0]
            << ", \"Gold\": " << stats.finalResourcesHeld[1]
            << ", \"Crystal\": " << stats.finalResourcesHeld[2] << "}\n";
        file << "      },\n";
        file << "      \"roleSamples\": {\n";
        file << "        \"Defender\": " << stats.roleSamples[0] << ",\n";
        file << "        \"Rusher\": " << stats.roleSamples[1] << ",\n";
        file << "        \"Collector\": " << stats.roleSamples[2] << ",\n";
        file << "        \"Fighter\": " << stats.roleSamples[3] << "\n";
        file << "      },\n";
        file << "      \"intentSamples\": {\n";
        for (int intent = 0; intent < 10; ++intent)
        {
            file << "        \"" << ToString(static_cast<BotIntent>(intent)) << "\": " << stats.intentSamples[intent]
                << (intent < 9 ? "," : "") << "\n";
        }
        file << "      },\n";
        file << "      \"strategicGoalSamples\": {\n";
        for (int goal = 0; goal < 7; ++goal)
        {
            file << "        \"" << ToString(static_cast<StrategicGoal>(goal)) << "\": " << stats.strategicGoalSamples[goal]
                << (goal < 6 ? "," : "") << "\n";
        }
        file << "      },\n";
        file << "      \"objectiveProgress\": {\n";
        file << "        \"samples\": " << stats.objectiveSamples << ",\n";
        file << "        \"progress\": " << stats.objectiveProgressSamples << ",\n";
        file << "        \"noProgress\": " << stats.objectiveNoProgressSamples << ",\n";
        file << "        \"regression\": " << stats.objectiveRegressionSamples << ",\n";
        file << "        \"reached\": " << stats.objectiveReachedSamples << ",\n";
        file << "        \"strategicStall\": " << stats.strategicStallSamples << ",\n";
        file << "        \"targetChanges\": " << stats.objectiveTargetChanges << ",\n";
        file << "        \"maxNoProgressSeconds\": " << stats.maxNoProgressSamples << ",\n";
        file << "        \"netDistanceChange\": " << stats.netObjectiveDistanceChange << ",\n";
        file << "        \"closestDistance\": " << stats.closestObjectiveDistance << "\n";
        file << "      },\n";
        file << "      \"mapControl\": {\n";
        file << "        \"midProximitySamples\": " << stats.midProximitySamples << ",\n";
        file << "        \"midReachSamples\": " << stats.midReachSamples << ",\n";
        file << "        \"enemyBaseSamples\": " << stats.enemyBaseSamples << ",\n";
        file << "        \"enemyCoreReachSamples\": " << stats.enemyCoreReachSamples << ",\n";
        file << "        \"firstMidReachTime\": " << stats.firstMidReachTime << ",\n";
        file << "        \"firstEnemyBaseTime\": " << stats.firstEnemyBaseTime << "\n";
        file << "      },\n";
        file << "      \"authoredRoute\": {\n";
        file << "        \"samples\": " << stats.authoredRouteSamples << ",\n";
        file << "        \"advances\": " << stats.authoredRouteAdvances << ",\n";
        file << "        \"maxIndex\": " << stats.maxAuthoredRouteIndex << ",\n";
        file << "        \"markerCount\": " << stats.authoredRouteMarkerCount << ",\n";
        file << "        \"markerSamples\": {";
        file << "\"Rally\": " << stats.authoredRouteMarkerSamples[0] << ", ";
        file << "\"Lane\": " << stats.authoredRouteMarkerSamples[1] << ", ";
        file << "\"Chokepoint\": " << stats.authoredRouteMarkerSamples[2] << ", ";
        file << "\"Highground\": " << stats.authoredRouteMarkerSamples[3] << "}\n";
        file << "      },\n";
        file << "      \"movement\": {\n";
        file << "        \"totalDistance\": " << stats.totalDistance << ",\n";
        file << "        \"maxDistanceFromBase\": " << stats.maxDistanceFromBase << ",\n";
        file << "        \"earlyMaxDistanceFromBase\": " << stats.earlyMaxDistanceFromBase << ",\n";
        file << "        \"maxDistanceFromCenter\": " << stats.maxDistanceFromCenter << ",\n";
        file << "        \"averageDistanceFromBase\": " << stats.averageDistanceFromBase << ",\n";
        file << "        \"averageDistanceFromCenter\": " << stats.averageDistanceFromCenter << ",\n";
        file << "        \"lastPosition\": ";
        WriteJsonVector3(file, stats.lastPosition);
        file << ",\n";
        file << "        \"minPosition\": ";
        WriteJsonVector3(file, stats.minPosition);
        file << ",\n";
        file << "        \"maxPosition\": ";
        WriteJsonVector3(file, stats.maxPosition);
        file << "\n";
        file << "      }\n";
        file << "    }" << (i + 1 < automatch_.botStats.size() ? "," : "") << "\n";
    }
    file << "  ]\n";
    file << "}\n";
}

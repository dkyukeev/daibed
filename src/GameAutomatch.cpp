#include "Game.h"
#include "CrashLogger.h"

#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
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
    SetMessage("Automatch running: bot-only simulation.", 4.0f);
}

bool Game::RunAutomatchBatch(int runs, int ticksPerFrame, int maxMinutes, unsigned int seed)
{
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
        return false;
    }
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

    players_.erase(
        std::remove_if(players_.begin(), players_.end(), [](const Player& player) { return !player.IsLocal(); }),
        players_.end());
    int nextId = localPlayerId_ + 1;
    for (Team& team : teams_)
    {
        if (!IsTeamActiveForMode(team.id))
        {
            continue;
        }
        for (int slot = 0; slot < selectedTeamSize_; ++slot)
        {
            Player bot(nextId++, std::string(TeamName(team.id)) + " Bot " + std::to_string(slot + 1), team.id, team.spawnPoint, false);
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
        if (player.IsLocal() || teamId < 0 || teamId >= static_cast<int>(heroSlotByTeam.size()))
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
        if (!players_[i].IsLocal() && players_[i].IsAlive() && !players_[i].IsEliminated())
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

    const bool timeout = !winnerTeamId_.has_value() && matchTime_ >= automatch_.maxMatchSeconds;
    if (!winnerTeamId_.has_value() && !timeout)
    {
        return;
    }

    FinishAutomatchRun(timeout);
    if (automatch_.completedRuns >= automatch_.targetRuns)
    {
        automatch_.active = false;
        WriteAutomatchStatsJson();
        SetMessage("Automatch complete. Review stats overlay.", 8.0f);
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
        if (player.IsLocal())
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
        ++stats.samples;
        const int roleIndex = std::clamp(static_cast<int>(memory->role), 0, 3);
        const int intentIndex = std::clamp(static_cast<int>(memory->intent), 0, 9);
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

        if (player.GetTeamId() >= 0 && player.GetTeamId() < 4)
        {
            AutomatchTeamStats& teamStats = automatch_.currentTeamStats[player.GetTeamId()];
            ++teamStats.samples;
            ++teamStats.roleSamples[roleIndex];
            ++teamStats.intentSamples[intentIndex];
            teamStats.resourcesHeld[0] += player.GetInventory().GetResource(ResourceType::Iron);
            teamStats.resourcesHeld[1] += player.GetInventory().GetResource(ResourceType::Gold);
            teamStats.resourcesHeld[2] += player.GetInventory().GetResource(ResourceType::Crystal);
        }

        const Vector3 position = player.GetPosition();
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
        if (matchTime_ <= 60.0f)
        {
            stats.earlyMaxDistanceFromBase = std::max(stats.earlyMaxDistanceFromBase, fromBase);
        }
        stats.maxDistanceFromCenter = std::max(stats.maxDistanceFromCenter, fromCenter);
        const float samples = static_cast<float>(std::max(1, stats.samples));
        stats.averageDistanceFromBase += (fromBase - stats.averageDistanceFromBase) / samples;
        stats.averageDistanceFromCenter += (fromCenter - stats.averageDistanceFromCenter) / samples;
    }
}

void Game::FinishAutomatchRun(bool timeout)
{
    SampleAutomatchBots();
    AutomatchRunStats run {};
    run.winnerTeamId = winnerTeamId_.value_or(-1);
    run.duration = matchTime_;
    run.timeout = timeout;
    run.firstCoreDamageTime = automatch_.currentFirstCoreDamageTime;
    run.timeline = automatch_.currentTimeline;
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        run.teamStats[teamId] = automatch_.currentTeamStats[teamId];
    }
    for (const Player& player : players_)
    {
        if (player.IsLocal())
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
        if (player.IsLocal())
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
        if (player.IsLocal() || player.GetTeamId() < 0 || player.GetTeamId() >= 4)
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

    for (const EnergyCore& core : cores_)
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
        run.finishReason = "timeout: " + std::to_string(aliveCores) + " cores alive, "
            + std::to_string(teamsWithLives) + " teams with lives";
    }
    else
    {
        run.finishReason = run.winnerTeamId >= 0
            ? std::string(TeamName(run.winnerTeamId)) + " was last team standing"
            : "match ended without winner";
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
    int totalIntentChanges = 0;
    for (const AutomatchBotStats& stats : automatch_.botStats)
    {
        totalVoidFalls += stats.voidFalls;
        totalStuckSamples += stats.stuckSamples;
        totalIntentChanges += stats.intentChanges;
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
    file << "    \"totalIntentChanges\": " << totalIntentChanges << ",\n";
    file << "    \"botTuningByTeam\": [\n";
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        const BotTuningGenome& genome = botTuningByTeam_[teamId];
        file << "      {";
        file << "\"teamId\": " << teamId << ", ";
        file << "\"id\": \"" << JsonEscape(genome.id) << "\", ";
        file << "\"generation\": " << genome.generation << ", ";
        file << "\"hash\": " << BotTuningGenomeHash(genome);
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
        file << "      \"winnerTeam\": \"" << JsonEscape(run.winnerTeamId >= 0 ? TeamName(run.winnerTeamId) : "None") << "\",\n";
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
            file << "          }\n";
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
        file << "      \"team\": \"" << JsonEscape(stats.teamId >= 0 ? TeamName(stats.teamId) : "Unknown") << "\",\n";
        file << "      \"samples\": " << stats.samples << ",\n";
        file << "      \"roleChanges\": " << stats.roleChanges << ",\n";
        file << "      \"intentChanges\": " << stats.intentChanges << ",\n";
        file << "      \"stuckSamples\": " << stats.stuckSamples << ",\n";
        file << "      \"voidFalls\": " << stats.voidFalls << ",\n";
        file << "      \"kills\": " << stats.kills << ",\n";
        file << "      \"deaths\": " << stats.deaths << ",\n";
        file << "      \"finalDeaths\": " << stats.finalDeaths << ",\n";
        file << "      \"coreDamage\": " << stats.coreDamage << ",\n";
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

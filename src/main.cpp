#include "Game.h"
#include "CrashLogger.h"

#include "raylib.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::optional<ArenaBiome> ParseBiome(const std::string& value)
{
    if (value == "arena")
    {
        return ArenaBiome::Arena;
    }
    if (value == "ice")
    {
        return ArenaBiome::Ice;
    }
    if (value == "lava")
    {
        return ArenaBiome::Lava;
    }
    if (value == "space")
    {
        return ArenaBiome::Space;
    }
    if (value == "ruins")
    {
        return ArenaBiome::Ruins;
    }
    return std::nullopt;
}

std::optional<BotDifficulty> ParseDifficulty(const std::string& value)
{
    if (value == "easy")
    {
        return BotDifficulty::Easy;
    }
    if (value == "normal")
    {
        return BotDifficulty::Normal;
    }
    if (value == "hard")
    {
        return BotDifficulty::Hard;
    }
    return std::nullopt;
}

std::vector<std::string> SplitWorkerCommand(const std::string& line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size())
    {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
        {
            break;
        }
        start = tab + 1;
    }
    return fields;
}
}

int main(int argc, char** argv)
{
    CrashLogger::Initialize(argc, argv);
    try
    {
        bool cliAutomatch = false;
        bool cliAutomatchWorker = false;
        bool cliProfile = false;
        bool cliCrashTest = false;
        bool cliStartupSmoke = false;
        int cliRuns = 3;
        int cliSpeed = 16;
        int cliMinutes = 10;
        unsigned int cliSeed = 0;
        std::optional<ArenaBiome> cliBiome;
        std::optional<BotDifficulty> cliDifficulty;
        std::string cliBotTuningPath;
        std::string cliStatsPath;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--automatch")
            {
                cliAutomatch = true;
            }
            else if (arg == "--automatch-worker")
            {
                cliAutomatchWorker = true;
            }
            else if (arg == "--profile")
            {
                cliProfile = true;
            }
            else if (arg == "--crash-test")
            {
                cliCrashTest = true;
            }
            else if (arg == "--startup-smoke")
            {
                cliStartupSmoke = true;
            }
            else if (arg == "--runs" && i + 1 < argc)
            {
                cliRuns = std::atoi(argv[++i]);
            }
            else if (arg == "--speed" && i + 1 < argc)
            {
                cliSpeed = std::atoi(argv[++i]);
            }
            else if (arg == "--minutes" && i + 1 < argc)
            {
                cliMinutes = std::atoi(argv[++i]);
            }
            else if (arg == "--seed" && i + 1 < argc)
            {
                cliSeed = static_cast<unsigned int>(std::strtoul(argv[++i], nullptr, 10));
            }
            else if (arg == "--biome" && i + 1 < argc)
            {
                cliBiome = ParseBiome(argv[++i]);
            }
            else if (arg == "--difficulty" && i + 1 < argc)
            {
                cliDifficulty = ParseDifficulty(argv[++i]);
            }
            else if (arg == "--bot-tuning" && i + 1 < argc)
            {
                cliBotTuningPath = argv[++i];
            }
            else if (arg == "--stats" && i + 1 < argc)
            {
                cliStatsPath = argv[++i];
            }
        }

        if (cliCrashTest)
        {
            CrashLogger::LogEvent("artificial crash test requested");
            throw std::runtime_error("artificial crash test");
        }

        CrashLogger::Heartbeat("construct-game");
        Game game;
        CrashLogger::LogEvent("game constructed");
        CrashLogger::Heartbeat("initialize");
        if (!game.Initialize(cliAutomatch || cliAutomatchWorker))
        {
            CrashLogger::LogEvent("game initialize failed");
            CrashLogger::Shutdown();
            return 1;
        }
        if (cliBiome.has_value())
        {
            game.SetSelectedBiome(*cliBiome);
        }
        if (cliDifficulty.has_value())
        {
            game.SetBotDifficulty(*cliDifficulty);
        }
        if (!cliBotTuningPath.empty())
        {
            game.SetBotTuningPath(cliBotTuningPath);
        }
        if (!cliStatsPath.empty())
        {
            game.SetAutomatchStatsPath(cliStatsPath);
        }
        game.SetProfilingEnabled(cliProfile);

        if (cliAutomatchWorker)
        {
            std::cout << "WORKER_READY" << std::endl;
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (line == "QUIT")
                {
                    break;
                }

                const std::vector<std::string> fields = SplitWorkerCommand(line);
                if (fields.size() != 7)
                {
                    std::cout << "WORKER_ERROR\t?\tinvalid command" << std::endl;
                    continue;
                }

                const std::string& jobId = fields[0];
                try
                {
                    const int runs = std::stoi(fields[1]);
                    const int speed = std::stoi(fields[2]);
                    const int minutes = std::stoi(fields[3]);
                    const unsigned int seed = static_cast<unsigned int>(std::stoul(fields[4]));
                    game.SetBotTuningPath(fields[5]);
                    game.SetAutomatchStatsPath(fields[6]);
                    const bool success = game.RunAutomatchBatch(runs, speed, minutes, seed);
                    std::cout << "WORKER_DONE\t" << jobId << '\t' << (success ? 1 : 0) << std::endl;
                }
                catch (const std::exception& ex)
                {
                    std::cout << "WORKER_ERROR\t" << jobId << '\t' << ex.what() << std::endl;
                }
            }
            game.Shutdown();
            CrashLogger::Shutdown();
            return 0;
        }

        if (cliAutomatch)
        {
            if (cliSeed == 0)
            {
                cliSeed = static_cast<unsigned int>(
                    std::chrono::high_resolution_clock::now().time_since_epoch().count());
            }
            std::cout << "headless automatch: runs=" << cliRuns
                      << " speed=" << cliSpeed
                      << " maxMinutes=" << cliMinutes
                      << " seed=" << cliSeed << '\n';
            CrashLogger::LogEvent("automatch started");
            CrashLogger::Heartbeat("automatch");
            const bool success = game.RunAutomatchBatch(cliRuns, cliSpeed, cliMinutes, cliSeed);
            CrashLogger::Heartbeat("shutdown");
            game.Shutdown();
            CrashLogger::Shutdown();
            return success ? 0 : 2;
        }

        if (cliStartupSmoke)
        {
            game.PrepareStartupSmoke();
            for (int frame = 0; frame < 8; ++frame)
            {
                if (frame == 3) game.ExerciseStartupSmokeMutation(true);
                if (frame == 5) game.ExerciseStartupSmokeMutation(false);
                CrashLogger::Heartbeat("startup-smoke");
                game.HandleInput();
                game.Update(1.0f / 60.0f);
                game.Render();
            }
            game.Shutdown();
            CrashLogger::Shutdown();
            return 0;
        }

        while (!WindowShouldClose() && !game.ShouldClose())
        {
            CrashLogger::Heartbeat("input");
            game.HandleInput();
            CrashLogger::Heartbeat("update");
            game.Update(GetFrameTime());
            CrashLogger::Heartbeat("render");
            game.Render();
        }

        CrashLogger::Heartbeat("shutdown");
        game.Shutdown();
        CrashLogger::Shutdown();
        return 0;
    }
    catch (const std::exception& ex)
    {
        CrashLogger::WriteManualLog(std::string("unhandled C++ exception: ") + ex.what());
        CrashLogger::Shutdown();
        return 3;
    }
    catch (...)
    {
        CrashLogger::WriteManualLog("unhandled unknown C++ exception");
        CrashLogger::Shutdown();
        return 3;
    }
}

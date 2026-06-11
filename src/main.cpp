#include "Game.h"
#include "CrashLogger.h"

#include "raylib.h"

#include <cstdlib>
#include <exception>
#include <optional>
#include <string>

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
}

int main(int argc, char** argv)
{
    CrashLogger::Initialize(argc, argv);
    try
    {
        bool cliAutomatch = false;
        int cliRuns = 3;
        int cliSpeed = 16;
        int cliMinutes = 10;
        std::optional<ArenaBiome> cliBiome;
        std::string cliBotTuningPath;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--automatch")
            {
                cliAutomatch = true;
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
            else if (arg == "--biome" && i + 1 < argc)
            {
                cliBiome = ParseBiome(argv[++i]);
            }
            else if (arg == "--bot-tuning" && i + 1 < argc)
            {
                cliBotTuningPath = argv[++i];
            }
        }

        CrashLogger::Heartbeat("construct-game");
        Game game;
        CrashLogger::LogEvent("game constructed");
        CrashLogger::Heartbeat("initialize");
        if (!game.Initialize())
        {
            CrashLogger::LogEvent("game initialize failed");
            CrashLogger::Shutdown();
            return 1;
        }
        if (cliBiome.has_value())
        {
            game.SetSelectedBiome(*cliBiome);
        }
        if (!cliBotTuningPath.empty())
        {
            game.SetBotTuningPath(cliBotTuningPath);
        }

        if (cliAutomatch)
        {
            CrashLogger::LogEvent("automatch started");
            CrashLogger::Heartbeat("automatch");
            const bool success = game.RunAutomatchBatch(cliRuns, cliSpeed, cliMinutes);
            CrashLogger::Heartbeat("shutdown");
            game.Shutdown();
            CrashLogger::Shutdown();
            return success ? 0 : 2;
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

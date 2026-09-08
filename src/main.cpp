#include "Game.h"
#include "CrashLogger.h"
#include "FirstPersonMotion.h"
#include "Network/NetworkProtocol.h"
#include "Network/NetworkTransport.h"
#include "Platform/SteamLobbyService.h"
#include "Platform/SteamRuntime.h"

#include "raylib.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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

std::optional<MatchMode> ParseMode(const std::string& value)
{
    if (value == "solo" || value == "solovsbots")
    {
        return MatchMode::SoloVsBots;
    }
    if (value == "2v2" || value == "two" || value == "2teams")
    {
        return MatchMode::TwoVsTwo;
    }
    if (value == "ffa" || value == "4teams" || value == "four" || value == "fourteams")
    {
        return MatchMode::FourTeams;
    }
    if (value == "duel")
    {
        return MatchMode::Duel;
    }
    return std::nullopt;
}

std::optional<ArenaLayout> ParseLayout(const std::string& value)
{
    if (value == "classic")
    {
        return ArenaLayout::Classic;
    }
    if (value == "vertical")
    {
        return ArenaLayout::Vertical;
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

std::optional<BotStrategyProfile> ParseBotStrategy(const std::string& value)
{
    if (value == "standard" || value == "default")
    {
        return BotStrategyProfile::Standard;
    }
    if (value == "hypixel" || value == "hypixel-rush" || value == "rush")
    {
        return BotStrategyProfile::HypixelRush;
    }
    return std::nullopt;
}

std::string LowerAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::optional<int> ParseTeamIndex(const std::string& value)
{
    const std::string v = LowerAscii(value);
    if (v == "red") return 0;
    if (v == "blue") return 1;
    if (v == "green") return 2;
    if (v == "yellow") return 3;
    char* parseEnd = nullptr;
    const long parsed = std::strtol(value.c_str(), &parseEnd, 10);
    if (parseEnd != value.c_str() && *parseEnd == '\0' && parsed >= 0 && parsed <= 3)
    {
        return static_cast<int>(parsed);
    }
    return std::nullopt;
}

std::optional<int> ParseHeroIndex(const std::string& value)
{
    const std::string v = LowerAscii(value);
    if (v == "radon") return 0;
    if (v == "orbita") return 1;
    if (v == "brom") return 2;
    if (v == "konvoy") return 3;
    if (v == "likho") return 4;
    if (v == "svidetel" || v == "witness") return 5;
    char* parseEnd = nullptr;
    const long parsed = std::strtol(value.c_str(), &parseEnd, 10);
    if (parseEnd != value.c_str() && *parseEnd == '\0' && parsed >= 0 && parsed < HeroSystem::kHeroCount)
    {
        return static_cast<int>(parsed);
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
        bool cliBotTrainer = false;
        bool cliProfile = false;
        bool cliDevKeyboard = false;
        bool cliCrashTest = false;
        bool cliStartupSmoke = false;
        bool cliNetworkSmoke = false;
        bool cliPurchaseSmoke = false;
        bool cliProtocolSmoke = false;
        bool cliLoopbackSmoke = false;
        bool cliLocalhostNetSmoke = false;
        bool cliDatagramBackendSmoke = false;
        bool cliSteamLobbySmoke = false;
        bool cliMpLoopbackSmoke = false;
        bool cliClientGuiSmoke = false;
        bool cliClientInputSmoke = false;
        bool cliNetworkClientUiSmoke = false;
        bool cliNetworkActionsSmoke = false;
        bool cliLagCompSmoke = false;
        bool cliCreativeSmoke = false;
        bool cliUiScreenshot = false;
        bool cliParticleReview = false;
        bool cliMapReview = false;
        bool cliFrameProfile = false;
        bool cliNetworkRangedSmoke = false;
        bool cliMovementParitySmoke = false;
        bool cliFirstPersonMotionSmoke = false;
        bool cliParticleSystemSmoke = false;
        bool cliClientDynamicApplySmoke = false;
        bool cliIntegratedServerSmoke = false;
        bool cliNavigationSmoke = false;
        bool cliBotAISmoke = false;
        double cliServerSeconds = 0.0; // 0 == run until interrupted
        bool cliServer = false;
        bool cliHost = false;
        bool cliConnectRequested = false;
        bool cliPortExplicit = false;
        bool cliNetworkBackendExplicit = false;
        std::string cliConnectAddress;
        std::string cliPlayerName;
        int cliLobbyTeam = -1;
        int cliLobbyHero = -1;
        bool cliLobbyReady = false;
        bool cliLobbyStart = false;
        ServerConfig cliServerConfig;
        int cliRuns = 3;
        int cliSpeed = 16;
        int cliMinutes = 10;
        int cliTrainerGenerations = 20;
        int cliTrainerPopulation = 24;
        std::string cliTrainerOutput = "bot_tuning.trained.json";
        unsigned int cliSeed = 0;
        std::optional<ArenaBiome> cliBiome;
        std::optional<MatchMode> cliMode;
        std::optional<int> cliTeamSize;
        std::optional<ArenaLayout> cliLayout;
        bool cliPublicRequested = false;
        std::optional<BotDifficulty> cliDifficulty;
        std::optional<BotStrategyProfile> cliBotStrategy;
        std::string cliBotTuningPath;
        std::string cliStatsPath;
        std::string cliMapPath;
        std::string cliCreativeMapPath;
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
            else if (arg == "--train-bots")
            {
                cliBotTrainer = true;
            }
            else if (arg == "--generations" && i + 1 < argc)
            {
                cliTrainerGenerations = std::stoi(argv[++i]);
            }
            else if (arg == "--population" && i + 1 < argc)
            {
                cliTrainerPopulation = std::stoi(argv[++i]);
            }
            else if (arg == "--trainer-output" && i + 1 < argc)
            {
                cliTrainerOutput = argv[++i];
            }
            else if (arg == "--map" && i + 1 < argc)
            {
                // Custom map for --automatch: bots play a creative-made
                // maps/*.dbmap document instead of the stock arena.
                cliMapPath = argv[++i];
            }
            else if (arg == "--creative-map" && i + 1 < argc)
            {
                // Open any CreativeMapDocument straight in the editor.  The
                // session saves back to this path, which makes built-in maps
                // such as Castle Bedwars directly editable.
                cliCreativeMapPath = argv[++i];
            }
            else if (arg == "--profile")
            {
                cliProfile = true;
            }
            else if (arg == "--dev-keyboard")
            {
                cliDevKeyboard = true;
            }
            else if (arg == "--crash-test")
            {
                cliCrashTest = true;
            }
            else if (arg == "--startup-smoke")
            {
                cliStartupSmoke = true;
            }
            else if (arg == "--network-smoke")
            {
                cliNetworkSmoke = true;
            }
            else if (arg == "--network-purchase-smoke")
            {
                cliPurchaseSmoke = true;
            }
            else if (arg == "--protocol-smoke")
            {
                cliProtocolSmoke = true;
            }
            else if (arg == "--loopback-two-client-smoke")
            {
                cliLoopbackSmoke = true;
            }
            else if (arg == "--localhost-net-smoke")
            {
                cliLocalhostNetSmoke = true;
            }
            else if (arg == "--datagram-backend-smoke")
            {
                cliDatagramBackendSmoke = true;
            }
            else if (arg == "--steam-lobby-smoke")
            {
                cliSteamLobbySmoke = true;
            }
            else if (arg == "--mp-loopback-smoke")
            {
                cliMpLoopbackSmoke = true;
            }
            else if (arg == "--client-gui-smoke")
            {
                cliClientGuiSmoke = true;
            }
            else if (arg == "--client-input-smoke")
            {
                cliClientInputSmoke = true;
            }
            else if (arg == "--network-client-ui-smoke")
            {
                cliNetworkClientUiSmoke = true;
            }
            else if (arg == "--network-actions-smoke")
            {
                cliNetworkActionsSmoke = true;
            }
            else if (arg == "--lag-comp-smoke")
            {
                cliLagCompSmoke = true;
            }
            else if (arg == "--creative-smoke")
            {
                cliCreativeSmoke = true;
            }
            else if (arg == "--ui-screenshot")
            {
                cliUiScreenshot = true;
            }
            else if (arg == "--particle-review")
            {
                cliParticleReview = true;
            }
            else if (arg == "--map-review")
            {
                cliMapReview = true;
            }
            else if (arg == "--frame-profile")
            {
                cliFrameProfile = true;
            }
            else if (arg == "--network-ranged-smoke")
            {
                cliNetworkRangedSmoke = true;
            }
            else if (arg == "--movement-parity-smoke")
            {
                cliMovementParitySmoke = true;
            }
            else if (arg == "--first-person-motion-smoke")
            {
                cliFirstPersonMotionSmoke = true;
            }
            else if (arg == "--particle-system-smoke")
            {
                cliParticleSystemSmoke = true;
            }
            else if (arg == "--client-dynamic-apply-smoke")
            {
                cliClientDynamicApplySmoke = true;
            }
            else if (arg == "--integrated-server-smoke")
            {
                cliIntegratedServerSmoke = true;
            }
            else if (arg == "--navigation-smoke")
            {
                cliNavigationSmoke = true;
            }
            else if (arg == "--bot-ai-smoke")
            {
                cliBotAISmoke = true;
            }
            else if (arg == "--server-seconds" && i + 1 < argc)
            {
                cliServerSeconds = std::strtod(argv[++i], nullptr);
            }
            else if (arg == "--server")
            {
                cliServer = true;
            }
            else if (arg == "--host")
            {
                cliHost = true;
            }
            else if (arg == "--connect" && i + 1 < argc)
            {
                cliConnectRequested = true;
                cliConnectAddress = argv[++i];
            }
            else if (arg == "--port" && i + 1 < argc)
            {
                cliPortExplicit = true;
                const char* portText = argv[++i];
                char* parseEnd = nullptr;
                const long parsedPort = std::strtol(portText, &parseEnd, 10);
                if (parseEnd == portText || *parseEnd != '\0' || parsedPort < 1 || parsedPort > 65535)
                {
                    std::cerr << "error: --port must be an integer in range 1..65535 (got \""
                              << portText << "\")\n";
                    CrashLogger::Shutdown();
                    return 5;
                }
                cliServerConfig.port = static_cast<std::uint16_t>(parsedPort);
            }
            else if ((arg == "--listen" || arg == "--listen-address") && i + 1 < argc)
            {
                cliServerConfig.listenAddress = argv[++i];
            }
            else if (arg == "--network-backend" && i + 1 < argc)
            {
                cliNetworkBackendExplicit = true;
                const std::string backend = LowerAscii(argv[++i]);
                if (backend == "udp" || backend == "systemudp")
                {
                    cliServerConfig.networkBackend = NetworkBackend::SystemUdp;
                }
                else if (backend == "steam" || backend == "steamp2p")
                {
                    cliServerConfig.networkBackend = NetworkBackend::SteamP2P;
                }
                else
                {
                    std::cerr << "error: --network-backend must be udp or steam (got \""
                              << backend << "\")\n";
                    CrashLogger::Shutdown();
                    return 5;
                }
            }
            else if (arg == "--server-name" && i + 1 < argc)
            {
                cliServerConfig.serverName = argv[++i];
            }
            else if ((arg == "--password" || arg == "--token") && i + 1 < argc)
            {
                cliServerConfig.password = argv[++i];
            }
            else if (arg == "--private")
            {
                cliServerConfig.privateServer = true;
            }
            else if ((arg == "--player-name" || arg == "--name") && i + 1 < argc)
            {
                cliPlayerName = argv[++i];
            }
            else if (arg == "--team" && i + 1 < argc)
            {
                const std::optional<int> team = ParseTeamIndex(argv[++i]);
                if (team.has_value())
                {
                    cliLobbyTeam = *team;
                }
            }
            else if (arg == "--hero" && i + 1 < argc)
            {
                const std::optional<int> hero = ParseHeroIndex(argv[++i]);
                if (hero.has_value())
                {
                    cliLobbyHero = *hero;
                }
            }
            else if (arg == "--ready")
            {
                cliLobbyReady = true;
            }
            else if (arg == "--not-ready")
            {
                cliLobbyReady = false;
                cliLobbyStart = false;
            }
            else if (arg == "--start")
            {
                cliLobbyStart = true;
                cliLobbyReady = true;
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
            else if (arg == "--mode" && i + 1 < argc)
            {
                cliMode = ParseMode(LowerAscii(argv[++i]));
            }
            else if (arg == "--team-size" && i + 1 < argc)
            {
                const int value = std::atoi(argv[++i]);
                if (value >= 1 && value <= 4)
                {
                    cliTeamSize = value;
                    cliServerConfig.maxTeamSize = value;
                }
            }
            else if (arg == "--layout" && i + 1 < argc)
            {
                cliLayout = ParseLayout(LowerAscii(argv[++i]));
            }
            else if (arg == "--max-players" && i + 1 < argc)
            {
                cliServerConfig.maxPlayers = std::clamp(std::atoi(argv[++i]), 1, 64);
            }
            else if (arg == "--min-players" && i + 1 < argc)
            {
                cliServerConfig.minPlayersToStart = std::clamp(std::atoi(argv[++i]), 1, 64);
            }
            else if (arg == "--require-ready")
            {
                cliServerConfig.requireAllReady = true;
            }
            else if (arg == "--no-require-ready")
            {
                cliServerConfig.requireAllReady = false;
            }
            else if (arg == "--unique-heroes")
            {
                cliServerConfig.enforceUniqueHeroesPerTeam = true;
            }
            else if (arg == "--no-unique-heroes")
            {
                cliServerConfig.enforceUniqueHeroesPerTeam = false;
            }
            else if (arg == "--public")
            {
                cliServerConfig.privateServer = false;
                cliPublicRequested = true;
            }
            else if (arg == "--difficulty" && i + 1 < argc)
            {
                cliDifficulty = ParseDifficulty(argv[++i]);
            }
            else if (arg == "--bot-strategy" && i + 1 < argc)
            {
                cliBotStrategy = ParseBotStrategy(LowerAscii(argv[++i]));
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

#if defined(DAIBED_HAVE_STEAMWORKS) && DAIBED_HAVE_STEAMWORKS
        // The universal release contains both transports. Prefer Steam when
        // its client and authenticated user are ready, otherwise keep the same
        // executable fully usable over UDP. Explicit CLI selection remains
        // authoritative for diagnostics and dedicated hosts.
        if (!cliNetworkBackendExplicit
            && !cliServer && !cliHost && !cliConnectRequested
            && !cliLocalhostNetSmoke && !cliDatagramBackendSmoke
            && !cliMpLoopbackSmoke && !cliSteamLobbySmoke)
        {
            SteamRuntimeLease steamProbe;
            if (steamProbe.Acquire())
            {
                cliServerConfig.networkBackend = NetworkBackend::SteamP2P;
                CrashLogger::LogEvent("network auto-select: Steam P2P available");
            }
            else
            {
                cliServerConfig.networkBackend = NetworkBackend::SystemUdp;
                CrashLogger::LogEvent(
                    "network auto-select: Steam unavailable, using UDP ("
                    + steamProbe.LastError() + ")");
            }
        }
#endif
        if (cliServerConfig.networkBackend == NetworkBackend::SteamP2P
            && !cliPortExplicit)
        {
            cliServerConfig.port = 777;
        }

        if (cliCrashTest)
        {
            CrashLogger::LogEvent("artificial crash test requested");
            throw std::runtime_error("artificial crash test");
        }

        if (cliProtocolSmoke)
        {
            // Pure serialization self-test: no window, no game state needed.
            CrashLogger::LogEvent("protocol smoke started");
            const int rc = RunProtocolSmoke();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliLocalhostNetSmoke)
        {
            // Real loopback UDP server+client in one process: no window, no Game.
            CrashLogger::LogEvent("localhost net smoke started");
            const int rc = RunLocalhostNetSmoke(cliServerConfig);
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliFirstPersonMotionSmoke)
        {
            CrashLogger::LogEvent("first-person motion smoke started");
            const int rc = RunFirstPersonMotionSmoke();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliParticleSystemSmoke)
        {
            CrashLogger::LogEvent("particle system smoke started");
            const int rc = RunParticleSystemSmoke();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliDatagramBackendSmoke)
        {
            // Same wire/session protocol over opaque provider-style peer handles.
            CrashLogger::LogEvent("datagram backend smoke started");
            const int rc = RunDatagramBackendSmoke();
            CrashLogger::Shutdown();
            return rc;
        }
        if (cliSteamLobbySmoke)
        {
            CrashLogger::LogEvent("Steam lobby smoke started");
            const int rc = RunSteamLobbySmoke();
            CrashLogger::Shutdown();
            return rc;
        }

        CrashLogger::Heartbeat("construct-game");
        Game game;
        CrashLogger::LogEvent("game constructed");
        CrashLogger::Heartbeat("initialize");
        // The connect path and --client-gui-smoke render with raylib, so they need
        // a real window (NOT headless). The headless server/automatch/smoke modes
        // run without one.
        const bool headlessRun = cliAutomatch || cliAutomatchWorker || cliBotTrainer
            || cliNetworkSmoke || cliPurchaseSmoke || cliLoopbackSmoke || cliMpLoopbackSmoke
            || cliClientInputSmoke || cliNetworkClientUiSmoke
            || cliNetworkActionsSmoke || cliLagCompSmoke || cliNetworkRangedSmoke
            || cliMovementParitySmoke
            || cliClientDynamicApplySmoke
            || cliIntegratedServerSmoke
            || cliNavigationSmoke
            || cliBotAISmoke
            || cliServer || cliHost;
        if (!game.Initialize(headlessRun))
        {
            CrashLogger::LogEvent("game initialize failed");
            CrashLogger::Shutdown();
            return 1;
        }
        if (cliBiome.has_value())
        {
            game.SetSelectedBiome(*cliBiome);
        }
        if (cliMode.has_value())
        {
            game.SetSelectedMode(*cliMode);
        }
        if (cliTeamSize.has_value())
        {
            game.SetSelectedTeamSize(*cliTeamSize);
        }
        if (cliLayout.has_value())
        {
            game.SetArenaLayout(*cliLayout);
        }
        if (cliDifficulty.has_value())
        {
            game.SetBotDifficulty(*cliDifficulty);
        }
        if (cliBotStrategy.has_value())
        {
            game.SetBotStrategyProfile(*cliBotStrategy);
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
        game.SetDevKeyboard(cliDevKeyboard);
        game.SetServerConfig(cliServerConfig);

        if (cliBotTrainer)
        {
            if (cliSeed == 0) cliSeed = 0xD41BEDu;
            CrashLogger::LogEvent("bot tuning trainer started");
            const bool success = game.RunBotTuningTrainer(
                cliTrainerGenerations, cliTrainerPopulation, cliSeed, cliTrainerOutput);
            game.Shutdown();
            CrashLogger::Shutdown();
            return success ? 0 : 2;
        }

        if (cliNetworkSmoke)
        {
            CrashLogger::LogEvent("network smoke started");
            const int rc = game.RunNetworkSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliPurchaseSmoke)
        {
            CrashLogger::LogEvent("network purchase smoke started");
            const int rc = game.RunNetworkPurchaseSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliLoopbackSmoke)
        {
            CrashLogger::LogEvent("loopback two-client smoke started");
            const int rc = game.RunLoopbackTwoClientSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliMpLoopbackSmoke)
        {
            CrashLogger::LogEvent("multiplayer loopback smoke started");
            const int rc = game.RunMultiplayerLoopbackSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliClientGuiSmoke)
        {
            // Windowed client + headless server in one process: renders the
            // replicated match and verifies the client world is in sync.
            CrashLogger::LogEvent("client gui smoke started");
            const int rc = game.RunClientGuiSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliCreativeSmoke)
        {
            // Windowed (creative uses the camera/cursor): enter the creative
            // sandbox and verify the flag + palette + free placement + no win.
            CrashLogger::LogEvent("creative smoke started");
            const int rc = game.RunCreativeSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliUiScreenshot)
        {
            // Windowed: renders the menu screens and saves ui_*.png for review.
            CrashLogger::LogEvent("ui screenshot diag started");
            const int rc = game.RunUiScreenshotDiag();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliMapReview)
        {
            // Windowed: creative test-play of a map + castle-utility physics
            // assertions + map_review_*.png shots for visual review.
            CrashLogger::LogEvent("map review diag started");
            const int rc = game.RunMapReviewDiag(
                cliMapPath.empty() ? std::string("maps/castle_bedwars.dbmap") : cliMapPath,
                cliBiome);
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliFrameProfile)
        {
            // Windowed: real loop, prints slow frames (hitch attribution).
            CrashLogger::LogEvent("frame profile diag started");
            const int rc = game.RunFrameProfileDiag();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliClientInputSmoke)
        {
            // Headless client + headless server in one process: drives real input
            // commands and verifies the server accepts them and the assigned
            // player's replicated position/yaw move.
            CrashLogger::LogEvent("client input smoke started");
            const int rc = game.RunClientInputSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliNetworkClientUiSmoke)
        {
            CrashLogger::LogEvent("network client ui smoke started");
            const int rc = game.RunNetworkClientUiSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliNetworkActionsSmoke)
        {
            // B1: verifies a network-controlled player's attack/break/place mutate
            // authoritative state (enemy HP, world block count).
            CrashLogger::LogEvent("network actions smoke started");
            const int rc = game.RunNetworkActionsSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliLagCompSmoke)
        {
            // #4: verifies server-side hitbox rewind — a melee that lands only
            // against the target's PAST position (where the client saw it) hits
            // with a rewindTick command and misses without one.
            CrashLogger::LogEvent("lag comp smoke started");
            const int rc = game.RunLagCompSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliNetworkRangedSmoke)
        {
            CrashLogger::LogEvent("network ranged smoke started");
            const int rc = game.RunNetworkRangedSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliMovementParitySmoke)
        {
            CrashLogger::LogEvent("movement parity smoke started");
            const int rc = game.RunMovementParitySmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliClientDynamicApplySmoke)
        {
            CrashLogger::LogEvent("client dynamic apply smoke started");
            const int rc = game.RunClientDynamicApplySmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliIntegratedServerSmoke)
        {
            // Phase 6: singleplayer integrated-server skeleton + the economy
            // command crossing the same loopback pipeline multiplayer uses.
            CrashLogger::LogEvent("integrated server smoke started");
            const int rc = game.RunIntegratedServerSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliParticleReview)
        {
            CrashLogger::LogEvent("particle review diag started");
            const int rc = game.RunParticleReviewDiag();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliNavigationSmoke)
        {
            CrashLogger::LogEvent("navigation smoke started");
            const int rc = game.RunNavigationSmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliBotAISmoke)
        {
            CrashLogger::LogEvent("bot AI smoke started");
            const int rc = game.RunBotAISmoke();
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliHost)
        {
            // A host defaults to private unless the operator explicitly opted in
            // to a public listing with --public.
            if (!cliPublicRequested)
            {
                cliServerConfig.privateServer = true;
            }
            game.SetServerConfig(cliServerConfig);
            CrashLogger::LogEvent("private host lobby started");
            const int rc = game.RunNetworkServer(cliServerConfig, cliServerSeconds);
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliServer)
        {
            CrashLogger::LogEvent("dedicated server started");
            // Real authoritative UDP server (headless, no window). Without
            // --server-seconds it runs until interrupted.
            const int rc = game.RunNetworkServer(cliServerConfig, cliServerSeconds);
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliConnectRequested)
        {
            // Parse host:port (default to the configured port if ":port" is absent).
            std::string connectHost = cliConnectAddress;
            std::uint16_t connectPort = cliServerConfig.port;
            const std::size_t colon = cliConnectAddress.rfind(':');
            if (colon != std::string::npos)
            {
                connectHost = cliConnectAddress.substr(0, colon);
                const std::string portText = cliConnectAddress.substr(colon + 1);
                const long parsed = std::strtol(portText.c_str(), nullptr, 10);
                if (parsed >= 1 && parsed <= 65535)
                {
                    connectPort = static_cast<std::uint16_t>(parsed);
                }
            }

            // GUI client: open a window and render the live match (Phase 0.1T).
            // Lobby preferences come from the CLI flags. A dead/unreachable server
            // or wrong password is handled gracefully inside (exit 0). Without
            // --server-seconds it runs until the window is closed.
            LobbyUpdate lobbyPrefs;
            lobbyPrefs.playerName = cliPlayerName;
            lobbyPrefs.selectedTeam = cliLobbyTeam;
            lobbyPrefs.selectedHero = cliLobbyHero;
            lobbyPrefs.ready = cliLobbyReady;
            lobbyPrefs.startRequested = cliLobbyStart && cliLobbyReady;
            const int rc = game.RunNetworkClient(
                connectHost, connectPort, cliServerConfig.password, cliServerSeconds, lobbyPrefs);
            game.Shutdown();
            CrashLogger::Shutdown();
            return rc;
        }

        if (cliAutomatchWorker)
        {
            if (!cliMapPath.empty())
            {
                std::string mapError;
                if (!game.LoadAutomatchMapDocument(cliMapPath, &mapError))
                {
                    std::cout << "WORKER_ERROR\t?\tmap load failed: " << mapError << std::endl;
                    game.Shutdown();
                    CrashLogger::Shutdown();
                    return 3;
                }
            }
            std::cout << "WORKER_READY" << std::endl;
            std::string line;
            while (std::getline(std::cin, line))
            {
                if (line == "QUIT")
                {
                    break;
                }

                const std::vector<std::string> fields = SplitWorkerCommand(line);
                if (fields.size() != 7 && fields.size() != 8)
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
                    if (fields.size() == 8 && !fields[7].empty())
                    {
                        std::string mapError;
                        if (!game.LoadAutomatchMapDocument(fields[7], &mapError))
                        {
                            std::cout << "WORKER_ERROR\t" << jobId
                                      << "\tmap load failed: " << mapError << std::endl;
                            continue;
                        }
                    }
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
            if (!cliMapPath.empty())
            {
                std::string mapError;
                if (!game.LoadAutomatchMapDocument(cliMapPath, &mapError))
                {
                    std::cout << "automatch map load failed: " << mapError << std::endl;
                    game.Shutdown();
                    CrashLogger::Shutdown();
                    return 3;
                }
                std::cout << "automatch map: " << cliMapPath << '\n';
            }
            // Print the full gameplay-relevant config: mode/team size/bot count/
            // difficulty/strategy/layout/biome come from the mutable DaiBed.settings file
            // unless pinned by CLI flags, so two runs with identical flags can
            // still simulate different matches. A baseline is only comparable
            // when this whole line matches (pin at least --difficulty).
            std::cout << "headless automatch: runs=" << cliRuns
                      << " speed=" << cliSpeed
                      << " maxMinutes=" << cliMinutes
                      << " seed=" << cliSeed
                      << " mode=" << game.MatchModeName()
                      << " difficulty=" << game.BotDifficultyName()
                      << " botStrategy=" << game.BotStrategyProfileName()
                      << " layout=" << game.ArenaLayoutName()
                      << " biome=" << game.ArenaBiomeName() << '\n';
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

        if (!cliCreativeMapPath.empty())
        {
            std::string mapError;
            if (!game.LoadCreativeMapDocumentForEditor(cliCreativeMapPath, &mapError))
            {
                std::cout << "creative map load failed: " << mapError << std::endl;
                game.Shutdown();
                CrashLogger::Shutdown();
                return 3;
            }
            std::cout << "creative map: " << cliCreativeMapPath << '\n';
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

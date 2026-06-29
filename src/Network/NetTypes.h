#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Pure simulation/networking types. Intentionally free of raylib so the
// authoritative server and future transport can build/link without the
// renderer or windowing layer. See docs/NETWORK_PREP_PLAN.md.

enum class NetworkMode
{
    LocalSinglePlayer, // No session layer; the local match is authoritative (default).
    LocalHost,         // Local authoritative host that also renders for one player.
    LocalClient,       // Client connected to a remote/host server (transport stub for now).
    DedicatedServer    // Headless authoritative server (transport stub for now).
};

inline const char* ToString(NetworkMode mode)
{
    switch (mode)
    {
    case NetworkMode::LocalSinglePlayer:
        return "LocalSinglePlayer";
    case NetworkMode::LocalHost:
        return "LocalHost";
    case NetworkMode::LocalClient:
        return "LocalClient";
    case NetworkMode::DedicatedServer:
        return "DedicatedServer";
    }
    return "Unknown";
}

// Configuration for a (future) listening server. Stored and validated now;
// no real socket/transport or cryptography is wired in this pass.
struct ServerConfig
{
    std::string listenAddress = "127.0.0.1";
    std::uint16_t port = 7777;
    int maxPlayers = 16;
    int teamCount = 4;
    int maxTeamSize = 4;
    int heroCount = 6;
    int minPlayersToStart = 2;
    bool enforceUniqueHeroesPerTeam = true;
    bool requireAllReady = true;
    bool privateServer = false;
    std::string serverName = "DaiBed Server";
    std::string password; // Plain storage only; no hashing/crypto yet.

    // World/map configuration the server is running, advertised to clients so a
    // GUI client can rebuild an identical arena locally (the map is deterministic
    // from these — see docs/NETWORK_PREP_PLAN.md Phase 0.1T). Stored as plain ints
    // (mirroring Game's ArenaBiome/ArenaLayout/MatchMode enums) to keep this header
    // raylib-free. Defaults match Game's defaults (Arena/Classic/FourTeams).
    int worldBiome = 0;  // ArenaBiome: Arena=0, Ice=1, Lava=2, Space=3, Ruins=4.
    int worldLayout = 0; // ArenaLayout: Classic=0, Vertical=1.
    int matchMode = 2;   // MatchMode: SoloVsBots=0, TwoVsTwo=1, FourTeams=2, Duel=3.

    bool HasPassword() const { return !password.empty(); }

    // Plain comparison placeholder. Real auth (hashed token, challenge) lands
    // with the transport layer; this keeps the call site stable.
    bool ValidatePassword(const std::string& candidate) const
    {
        return !HasPassword() || candidate == password;
    }
};

struct LobbyUpdate
{
    std::string playerName;
    // -1 means "keep/auto"; otherwise these are protocol indices, not HeroId
    // enum values. The Game layer translates hero indices through HeroSystem.
    int selectedTeam = -1;
    int selectedHero = -1;
    bool ready = false;
    bool startRequested = false;
};

struct LobbyPlayerState
{
    int clientId = -1;
    int assignedPlayerId = -1;
    std::string playerName;
    int selectedTeam = 0;
    int selectedHero = 0;
    bool ready = false;
    bool connected = true;
    bool startRequested = false;
};

struct LobbySnapshot
{
    std::uint32_t revision = 0;
    int hostClientId = -1;
    std::string serverName;
    bool privateServer = false;
    int maxPlayers = 16;
    int teamCount = 4;
    int maxTeamSize = 4;
    int heroCount = 6;
    bool enforceUniqueHeroesPerTeam = true;
    bool requireAllReady = true;
    bool canStart = false;
    bool matchStarting = false;
    bool matchStarted = false;
    // World/map config the server advertises (see ServerConfig above) so a client
    // can rebuild the same arena. Defaults match Game's defaults.
    int worldBiome = 0;
    int worldLayout = 0;
    int matchMode = 2;
    std::string statusMessage;
    std::vector<LobbyPlayerState> players;
};

struct LobbyValidation
{
    bool canStart = false;
    std::string reason;
};

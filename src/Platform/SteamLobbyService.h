#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct SteamLobbyHostSettings
{
    std::string serverName;
    std::uint64_t hostSteamId = 0;
    std::uint16_t virtualPort = 0;
    int maxPlayers = 16;
    int mode = 0;
    int biome = 0;
    bool privateLobby = false;
    bool passwordProtected = false;
};

struct SteamLobbyJoinTarget
{
    std::uint64_t lobbyId = 0;
    std::uint64_t hostSteamId = 0;
    std::uint16_t virtualPort = 0;
    std::string serverName;
    bool passwordProtected = false;

    bool IsValid() const { return lobbyId != 0 && hostSteamId != 0; }
};

struct SteamLobbyFriend
{
    std::uint64_t steamId = 0;
    std::string displayName;
    bool online = false;
    bool playingDaiBed = false;
};

// Steam matchmaking/lobby bridge used by the interactive game. It owns no
// gameplay state: a lobby only publishes how to reach the host's existing
// Steam Networking Sockets listen-server.
class SteamLobbyService
{
public:
    SteamLobbyService();
    ~SteamLobbyService();

    SteamLobbyService(const SteamLobbyService&) = delete;
    SteamLobbyService& operator=(const SteamLobbyService&) = delete;

    bool Start();
    void Stop();
    void PumpCallbacks();

    bool CreateLobby(const SteamLobbyHostSettings& settings);
    bool JoinLobby(std::uint64_t lobbyId);
    void LeaveLobby();
    bool OpenInviteDialog();
    bool OverlayAvailable() const;
    std::vector<SteamLobbyFriend> Friends() const;
    bool InviteFriend(std::uint64_t steamId);
    void SetHostJoinable(bool joinable);

    bool IsActive() const;
    bool IsCreatingLobby() const;
    bool HasLobby() const;
    bool OwnsLobby() const;
    std::uint64_t CurrentLobbyId() const;
    const std::string& Status() const;
    const std::string& LastError() const;
    std::optional<SteamLobbyJoinTarget> TakeJoinTarget();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One-account runtime diagnostic: initializes Steam (App ID from
// steam_appid.txt), creates a private lobby, publishes its endpoint metadata,
// then leaves it. No second client is required.
int RunSteamLobbySmoke();

#include "Platform/SteamLobbyService.h"

#include "Network/NetworkProtocol.h"
#include "Network/NetworkTransport.h"
#include "Platform/SteamRuntime.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>

#if defined(DAIBED_HAVE_STEAMWORKS) && DAIBED_HAVE_STEAMWORKS

#include <steam/steam_api.h>

namespace
{
constexpr const char* kLobbyProductKey = "daibed_product";
constexpr const char* kLobbyProtocolKey = "daibed_protocol";
constexpr const char* kLobbyHostKey = "daibed_host";
constexpr const char* kLobbyPortKey = "daibed_port";
constexpr const char* kLobbyNameKey = "daibed_name";
constexpr const char* kLobbyPasswordKey = "daibed_password";
constexpr const char* kLobbyModeKey = "daibed_mode";
constexpr const char* kLobbyBiomeKey = "daibed_biome";
constexpr const char* kLobbyBuildKey = "daibed_build";

bool ParseUint64(std::string_view text, std::uint64_t& value)
{
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    return result.ec == std::errc() && result.ptr == end && value != 0;
}

bool ParseVirtualPort(std::string_view text, std::uint16_t& value)
{
    unsigned int parsed = 0;
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed >= 1000)
    {
        return false;
    }
    value = static_cast<std::uint16_t>(parsed);
    return true;
}

std::uint64_t StartupLobbyId()
{
    if (SteamApps() == nullptr) return 0;
    char commandLine[4096] {};
    const int length = SteamApps()->GetLaunchCommandLine(
        commandLine, static_cast<int>(sizeof(commandLine)));
    if (length <= 0) return 0;

    const std::string_view line(commandLine, static_cast<std::size_t>(length));
    constexpr std::string_view marker = "+connect_lobby";
    const std::size_t markerAt = line.find(marker);
    if (markerAt == std::string_view::npos) return 0;
    std::size_t begin = markerAt + marker.size();
    while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
    std::size_t end = begin;
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
    std::uint64_t lobbyId = 0;
    return ParseUint64(line.substr(begin, end - begin), lobbyId) ? lobbyId : 0;
}
} // namespace

struct SteamLobbyService::Impl
{
    SteamRuntimeLease runtime;
    SteamLobbyHostSettings pendingHost;
    std::optional<SteamLobbyJoinTarget> joinTarget;
    std::uint64_t currentLobbyId = 0;
    std::uint64_t pendingJoinLobbyId = 0;
    bool ownsLobby = false;
    bool creatingLobby = false;
    bool hostJoinable = true;
    std::string status;
    std::string lastError;

    CCallResult<Impl, LobbyCreated_t> createResult;
    CCallResult<Impl, LobbyEnter_t> joinResult;
    STEAM_CALLBACK(Impl, OnGameLobbyJoinRequested, GameLobbyJoinRequested_t);

    bool ReadyInterfaces()
    {
        return SteamMatchmaking() != nullptr && SteamFriends() != nullptr;
    }

    void Fail(std::string error)
    {
        lastError = std::move(error);
        status = lastError;
    }

    void PublishHostMetadata()
    {
        ISteamMatchmaking* matchmaking = SteamMatchmaking();
        if (matchmaking == nullptr || currentLobbyId == 0) return;
        const CSteamID lobby(currentLobbyId);
        const std::string protocol = std::to_string(kProtocolVersion);
        const std::string host = std::to_string(pendingHost.hostSteamId);
        const std::string port = std::to_string(pendingHost.virtualPort);
        const std::string mode = std::to_string(pendingHost.mode);
        const std::string biome = std::to_string(pendingHost.biome);
        matchmaking->SetLobbyData(lobby, kLobbyProductKey, "daibed");
        matchmaking->SetLobbyData(lobby, kLobbyProtocolKey, protocol.c_str());
        matchmaking->SetLobbyData(lobby, kLobbyHostKey, host.c_str());
        matchmaking->SetLobbyData(lobby, kLobbyPortKey, port.c_str());
        matchmaking->SetLobbyData(lobby, kLobbyNameKey, pendingHost.serverName.c_str());
        matchmaking->SetLobbyData(
            lobby, kLobbyPasswordKey, pendingHost.passwordProtected ? "1" : "0");
        matchmaking->SetLobbyData(lobby, kLobbyModeKey, mode.c_str());
        matchmaking->SetLobbyData(lobby, kLobbyBiomeKey, biome.c_str());
        matchmaking->SetLobbyData(lobby, kLobbyBuildKey, DAIBED_VERSION);
        matchmaking->SetLobbyJoinable(lobby, hostJoinable);
    }

    void OnLobbyCreated(LobbyCreated_t* event, bool ioFailure)
    {
        creatingLobby = false;
        if (ioFailure || event == nullptr || event->m_eResult != k_EResultOK
            || event->m_ulSteamIDLobby == 0)
        {
            Fail("Steam не смог создать лобби");
            return;
        }
        currentLobbyId = event->m_ulSteamIDLobby;
        ownsLobby = true;
        PublishHostMetadata();
        status = "Steam-лобби создано. Можно приглашать друзей.";
        lastError.clear();
    }

    void BeginJoin(std::uint64_t lobbyId)
    {
        if (!runtime.IsActive() || !ReadyInterfaces() || lobbyId == 0)
        {
            Fail("Steam-лобби недоступно");
            return;
        }
        if (currentLobbyId != 0 && currentLobbyId != lobbyId)
        {
            SteamMatchmaking()->LeaveLobby(CSteamID(currentLobbyId));
            currentLobbyId = 0;
            ownsLobby = false;
        }
        pendingJoinLobbyId = lobbyId;
        joinTarget.reset();
        const SteamAPICall_t call = SteamMatchmaking()->JoinLobby(CSteamID(lobbyId));
        if (call == k_uAPICallInvalid)
        {
            pendingJoinLobbyId = 0;
            Fail("Не удалось отправить запрос входа в Steam-лобби");
            return;
        }
        joinResult.Set(call, this, &Impl::OnLobbyEntered);
        status = "Вход в Steam-лобби...";
        lastError.clear();
    }

    void OnLobbyEntered(LobbyEnter_t* event, bool ioFailure)
    {
        const bool success = !ioFailure && event != nullptr
            && event->m_ulSteamIDLobby != 0
            && event->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess;
        if (!success)
        {
            pendingJoinLobbyId = 0;
            Fail("Steam отклонил вход в лобби");
            return;
        }

        ISteamMatchmaking* matchmaking = SteamMatchmaking();
        const CSteamID lobby(event->m_ulSteamIDLobby);
        if (matchmaking == nullptr
            || std::string_view(matchmaking->GetLobbyData(lobby, kLobbyProductKey)) != "daibed"
            || std::string_view(matchmaking->GetLobbyData(lobby, kLobbyProtocolKey))
                != std::to_string(kProtocolVersion))
        {
            if (matchmaking) matchmaking->LeaveLobby(lobby);
            pendingJoinLobbyId = 0;
            Fail("Лобби использует несовместимую версию DaiBed");
            return;
        }

        SteamLobbyJoinTarget target;
        target.lobbyId = event->m_ulSteamIDLobby;
        const char* hostText = matchmaking->GetLobbyData(lobby, kLobbyHostKey);
        const char* portText = matchmaking->GetLobbyData(lobby, kLobbyPortKey);
        if (!ParseUint64(hostText != nullptr ? hostText : "", target.hostSteamId)
            || !ParseVirtualPort(portText != nullptr ? portText : "", target.virtualPort))
        {
            matchmaking->LeaveLobby(lobby);
            pendingJoinLobbyId = 0;
            Fail("В Steam-лобби отсутствует адрес хоста");
            return;
        }
        const char* name = matchmaking->GetLobbyData(lobby, kLobbyNameKey);
        target.serverName = name != nullptr ? name : "DaiBed";
        const char* password = matchmaking->GetLobbyData(lobby, kLobbyPasswordKey);
        target.passwordProtected = password != nullptr && std::string_view(password) == "1";

        currentLobbyId = target.lobbyId;
        pendingJoinLobbyId = 0;
        ownsLobby = false;
        joinTarget = std::move(target);
        status = "Steam-лобби найдено. Подключение к хосту...";
        lastError.clear();
    }

};

void SteamLobbyService::Impl::OnGameLobbyJoinRequested(GameLobbyJoinRequested_t* event)
{
    if (event != nullptr && event->m_steamIDLobby.IsValid())
    {
        BeginJoin(event->m_steamIDLobby.ConvertToUint64());
    }
}

SteamLobbyService::SteamLobbyService() : impl_(std::make_unique<Impl>()) {}
SteamLobbyService::~SteamLobbyService() { Stop(); }

bool SteamLobbyService::Start()
{
    if (impl_->runtime.IsActive()) return true;
    if (!impl_->runtime.Acquire())
    {
        impl_->Fail(impl_->runtime.LastError());
        return false;
    }
    if (!impl_->ReadyInterfaces())
    {
        impl_->Fail("Steam Matchmaking или Friends API недоступен");
        impl_->runtime.Release();
        return false;
    }
    impl_->status = "Steam подключён";
    impl_->lastError.clear();
    const std::uint64_t startupLobby = StartupLobbyId();
    if (startupLobby != 0)
    {
        impl_->BeginJoin(startupLobby);
    }
    return true;
}

void SteamLobbyService::Stop()
{
    LeaveLobby();
    impl_->createResult.Cancel();
    impl_->joinResult.Cancel();
    impl_->runtime.Release();
}

void SteamLobbyService::PumpCallbacks()
{
    impl_->runtime.PumpCallbacks();
}

bool SteamLobbyService::CreateLobby(const SteamLobbyHostSettings& settings)
{
    if (!Start()) return false;
    LeaveLobby();
    impl_->pendingHost = settings;
    impl_->hostJoinable = true;
    impl_->creatingLobby = true;
    const ELobbyType type = settings.privateLobby ? k_ELobbyTypePrivate : k_ELobbyTypePublic;
    const SteamAPICall_t call = SteamMatchmaking()->CreateLobby(
        type, std::clamp(settings.maxPlayers, 2, 250));
    if (call == k_uAPICallInvalid)
    {
        impl_->creatingLobby = false;
        impl_->Fail("Не удалось отправить запрос создания Steam-лобби");
        return false;
    }
    impl_->createResult.Set(call, impl_.get(), &Impl::OnLobbyCreated);
    impl_->status = "Создание Steam-лобби...";
    impl_->lastError.clear();
    return true;
}

bool SteamLobbyService::JoinLobby(std::uint64_t lobbyId)
{
    if (!Start()) return false;
    impl_->BeginJoin(lobbyId);
    return impl_->lastError.empty();
}

void SteamLobbyService::LeaveLobby()
{
    if (impl_->runtime.IsActive() && SteamMatchmaking() != nullptr
        && impl_->currentLobbyId != 0)
    {
        SteamMatchmaking()->LeaveLobby(CSteamID(impl_->currentLobbyId));
    }
    impl_->createResult.Cancel();
    impl_->joinResult.Cancel();
    impl_->currentLobbyId = 0;
    impl_->pendingJoinLobbyId = 0;
    impl_->ownsLobby = false;
    impl_->creatingLobby = false;
    impl_->joinTarget.reset();
}

bool SteamLobbyService::OpenInviteDialog()
{
    if (!impl_->runtime.IsActive() || SteamFriends() == nullptr || !HasLobby())
    {
        impl_->Fail("Сначала дождитесь создания Steam-лобби");
        return false;
    }
    SteamFriends()->ActivateGameOverlayInviteDialog(CSteamID(impl_->currentLobbyId));
    impl_->status = "Открыт список друзей Steam";
    impl_->lastError.clear();
    return true;
}

bool SteamLobbyService::OverlayAvailable() const
{
    return impl_->runtime.IsActive() && SteamUtils() != nullptr
        && SteamUtils()->IsOverlayEnabled();
}

std::vector<SteamLobbyFriend> SteamLobbyService::Friends() const
{
    std::vector<SteamLobbyFriend> result;
    if (!impl_->runtime.IsActive() || SteamFriends() == nullptr)
    {
        return result;
    }
    const int count = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
    result.reserve(static_cast<std::size_t>(std::max(count, 0)));
    const AppId_t appId = SteamUtils() != nullptr ? SteamUtils()->GetAppID() : 0;
    for (int i = 0; i < count; ++i)
    {
        const CSteamID friendId = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
        if (!friendId.IsValid()) continue;
        SteamLobbyFriend entry;
        entry.steamId = friendId.ConvertToUint64();
        const char* name = SteamFriends()->GetFriendPersonaName(friendId);
        entry.displayName = name != nullptr ? name : "Steam friend";
        entry.online = SteamFriends()->GetFriendPersonaState(friendId) != k_EPersonaStateOffline;
        FriendGameInfo_t gameInfo {};
        entry.playingDaiBed = SteamFriends()->GetFriendGamePlayed(friendId, &gameInfo)
            && gameInfo.m_gameID.AppID() == appId;
        result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(), [](const SteamLobbyFriend& a, const SteamLobbyFriend& b)
    {
        if (a.playingDaiBed != b.playingDaiBed) return a.playingDaiBed > b.playingDaiBed;
        if (a.online != b.online) return a.online > b.online;
        return a.displayName < b.displayName;
    });
    return result;
}

bool SteamLobbyService::InviteFriend(std::uint64_t steamId)
{
    if (!impl_->runtime.IsActive() || SteamMatchmaking() == nullptr
        || !HasLobby() || steamId == 0)
    {
        impl_->Fail("Steam-приглашение недоступно");
        return false;
    }
    if (!SteamMatchmaking()->InviteUserToLobby(
            CSteamID(impl_->currentLobbyId), CSteamID(steamId)))
    {
        impl_->Fail("Steam не смог отправить приглашение");
        return false;
    }
    impl_->status = "Приглашение отправлено через Steam";
    impl_->lastError.clear();
    return true;
}

void SteamLobbyService::SetHostJoinable(bool joinable)
{
    if (!impl_->ownsLobby || impl_->currentLobbyId == 0
        || impl_->hostJoinable == joinable || SteamMatchmaking() == nullptr)
    {
        return;
    }
    impl_->hostJoinable = joinable;
    SteamMatchmaking()->SetLobbyJoinable(CSteamID(impl_->currentLobbyId), joinable);
}

bool SteamLobbyService::IsActive() const { return impl_->runtime.IsActive(); }
bool SteamLobbyService::IsCreatingLobby() const { return impl_->creatingLobby; }
bool SteamLobbyService::HasLobby() const { return impl_->currentLobbyId != 0; }
bool SteamLobbyService::OwnsLobby() const { return impl_->ownsLobby; }
std::uint64_t SteamLobbyService::CurrentLobbyId() const { return impl_->currentLobbyId; }
const std::string& SteamLobbyService::Status() const { return impl_->status; }
const std::string& SteamLobbyService::LastError() const { return impl_->lastError; }

std::optional<SteamLobbyJoinTarget> SteamLobbyService::TakeJoinTarget()
{
    std::optional<SteamLobbyJoinTarget> result = std::move(impl_->joinTarget);
    impl_->joinTarget.reset();
    return result;
}

int RunSteamLobbySmoke()
{
    SteamRuntimeLease identity;
    SteamLobbyService lobby;
    if (!identity.Acquire() || !lobby.Start())
    {
        const std::string error = !identity.IsActive()
            ? identity.LastError()
            : lobby.LastError();
        std::cout << "steam-lobby-smoke: init failed: " << error << '\n';
        std::cout << "STEAM_LOBBY_SMOKE_FAIL" << std::endl;
        return 8;
    }

    SteamLobbyHostSettings settings;
    settings.serverName = "DaiBed lobby smoke";
    settings.hostSteamId = identity.LocalSteamId();
    settings.virtualPort = 777;
    settings.maxPlayers = 4;
    settings.privateLobby = true;
    if (!lobby.CreateLobby(settings))
    {
        std::cout << "steam-lobby-smoke: create failed: " << lobby.LastError() << '\n';
        std::cout << "STEAM_LOBBY_SMOKE_FAIL" << std::endl;
        return 8;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!lobby.HasLobby() && lobby.LastError().empty()
           && std::chrono::steady_clock::now() < deadline)
    {
        lobby.PumpCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool created = lobby.HasLobby() && lobby.OwnsLobby();
    const std::uint64_t lobbyId = lobby.CurrentLobbyId();

    // Also validate the exact local listen-server shape used by the GUI host.
    // This must take Steam's CreateSocketPair path, not route to our own ID.
    ServerConfig serverConfig;
    serverConfig.networkBackend = NetworkBackend::SteamP2P;
    serverConfig.port = 778;
    serverConfig.minPlayersToStart = 1;
    ServerTransport server;
    ClientTransport localClient(NetworkBackend::SteamP2P);
    bool localP2P = created && server.Start(serverConfig)
        && localClient.Open(std::to_string(identity.LocalSteamId()), serverConfig.port, "", 5.0f);
    const auto p2pDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (localP2P && !localClient.IsConnected()
           && !localClient.WasDenied() && !localClient.TimedOut()
           && std::chrono::steady_clock::now() < p2pDeadline)
    {
        server.Poll();
        localClient.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    localP2P = localP2P && localClient.IsConnected()
        && server.ClientCount() == 1;
    std::cout << "steam-lobby-smoke: steamId=" << identity.LocalSteamId()
              << " created=" << (created ? "yes" : "no")
              << " localP2P=" << (localP2P ? "yes" : "no")
              << " lobbyId=" << lobbyId
              << " status=\"" << lobby.Status() << "\"\n";
    localClient.Disconnect();
    server.Close();
    lobby.LeaveLobby();
    lobby.Stop();
    identity.Release();
    const bool ok = created && localP2P;
    std::cout << (ok ? "STEAM_LOBBY_SMOKE_OK" : "STEAM_LOBBY_SMOKE_FAIL")
              << std::endl;
    return ok ? 0 : 8;
}

#else

struct SteamLobbyService::Impl
{
    std::string status;
    std::string lastError =
        "Steam Lobby отключён; соберите с DAIBED_ENABLE_STEAMWORKS=ON";
};

SteamLobbyService::SteamLobbyService() : impl_(std::make_unique<Impl>()) {}
SteamLobbyService::~SteamLobbyService() = default;
bool SteamLobbyService::Start() { impl_->status = impl_->lastError; return false; }
void SteamLobbyService::Stop() {}
void SteamLobbyService::PumpCallbacks() {}
bool SteamLobbyService::CreateLobby(const SteamLobbyHostSettings&) { return false; }
bool SteamLobbyService::JoinLobby(std::uint64_t) { return false; }
void SteamLobbyService::LeaveLobby() {}
bool SteamLobbyService::OpenInviteDialog() { return false; }
bool SteamLobbyService::OverlayAvailable() const { return false; }
std::vector<SteamLobbyFriend> SteamLobbyService::Friends() const { return {}; }
bool SteamLobbyService::InviteFriend(std::uint64_t) { return false; }
void SteamLobbyService::SetHostJoinable(bool) {}
bool SteamLobbyService::IsActive() const { return false; }
bool SteamLobbyService::IsCreatingLobby() const { return false; }
bool SteamLobbyService::HasLobby() const { return false; }
bool SteamLobbyService::OwnsLobby() const { return false; }
std::uint64_t SteamLobbyService::CurrentLobbyId() const { return 0; }
const std::string& SteamLobbyService::Status() const { return impl_->status; }
const std::string& SteamLobbyService::LastError() const { return impl_->lastError; }
std::optional<SteamLobbyJoinTarget> SteamLobbyService::TakeJoinTarget() { return std::nullopt; }
int RunSteamLobbySmoke()
{
    std::cout << "steam-lobby-smoke: Steamworks SDK disabled - skipped\n";
    std::cout << "STEAM_LOBBY_SMOKE_SKIPPED" << std::endl;
    return 0;
}

#endif

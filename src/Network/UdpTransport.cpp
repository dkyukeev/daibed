#include "Network/NetworkTransport.h"
#include "Network/NetworkProtocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

// Real UDP transport. The whole socket implementation is gated behind
// DAIBED_HAVE_NETWORK (CMake option DAIBED_ENABLE_NETWORK). When the option is
// off, every entry point compiles to a safe stub so the rest of the game builds
// and links unchanged. See docs/NETWORK_PREP_PLAN.md.

#if defined(DAIBED_HAVE_NETWORK) && DAIBED_HAVE_NETWORK

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// Some SDKs declare this control code in <mstcpip.h> rather than <winsock2.h>.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

#include <cstring>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr float kDefaultTimeoutSeconds = 5.0f;
constexpr float kCommandReplayIntervalSeconds = 0.10f;
constexpr std::size_t kMaxPendingReliableCommands = 32;
constexpr float kReliableRetryIntervalSeconds = 0.12f;
constexpr int kReliableMaxAttempts = 12;
constexpr float kSnapshotSendIntervalSeconds = 1.0f / 20.0f;
constexpr float kFullResyncRetryIntervalSeconds = 0.20f;
constexpr std::size_t kMaxSnapshotPacketBytes = 4096;

double SecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Process-wide socket init refcount (WSAStartup on Windows; no-op elsewhere).
int g_netInitCount = 0;

bool NetInit(std::string& err)
{
#if defined(_WIN32)
    if (g_netInitCount == 0)
    {
        WSADATA data;
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0)
        {
            err = "WSAStartup failed (" + std::to_string(rc) + ")";
            return false;
        }
    }
#else
    (void)err;
#endif
    ++g_netInitCount;
    return true;
}

void NetShutdown()
{
    if (g_netInitCount > 0)
    {
        --g_netInitCount;
#if defined(_WIN32)
        if (g_netInitCount == 0)
        {
            WSACleanup();
        }
#endif
    }
}

int LastSocketError()
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool WouldBlock(int error)
{
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK;
#else
    return error == EWOULDBLOCK || error == EAGAIN;
#endif
}

void CloseSocket(socket_t sock)
{
    if (sock != kInvalidSocket)
    {
#if defined(_WIN32)
        closesocket(sock);
#else
        ::close(sock);
#endif
    }
}

bool SetNonBlocking(socket_t sock)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(sock, F_GETFL, 0);
    return flags != -1 && fcntl(sock, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

// On Windows a UDP send to a closed port makes the NEXT recvfrom fail with
// WSAECONNRESET (an ICMP port-unreachable echo). Disable that so a dead peer
// never trips the receive loop.
void SuppressConnReset(socket_t sock)
{
#if defined(_WIN32)
    BOOL behavior = FALSE;
    DWORD bytes = 0;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &behavior, sizeof(behavior), nullptr, 0, &bytes, nullptr, nullptr);
#else
    (void)sock;
#endif
}

// Resolve a numeric IPv4 (or "localhost") without touching DNS; fall back to
// getaddrinfo only for real hostnames.
bool ResolveIPv4(const std::string& host, std::uint16_t port, sockaddr_in& out, std::string& err)
{
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);

    const std::string node = host == "localhost" ? std::string("127.0.0.1") : host;
    if (inet_pton(AF_INET, node.c_str(), &out.sin_addr) == 1)
    {
        return true;
    }

    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr)
    {
        err = "could not resolve host \"" + host + "\"";
        if (result != nullptr)
        {
            freeaddrinfo(result);
        }
        return false;
    }
    out.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return true;
}

bool SameEndpoint(const sockaddr_in& a, const sockaddr_in& b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

int PositiveOrDefault(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

std::string SanitizeLobbyName(std::string value, int clientId)
{
    if (value.empty())
    {
        value = "Player " + std::to_string(clientId);
    }
    if (value.size() > 32)
    {
        value.resize(32);
    }
    return value;
}

LobbyUpdate DefaultLobbyUpdateForClient(int clientId, const ServerConfig& config)
{
    const int teamCount = PositiveOrDefault(config.teamCount, 4);
    const int heroCount = PositiveOrDefault(config.heroCount, 6);
    LobbyUpdate update;
    update.playerName = SanitizeLobbyName("", clientId);
    update.selectedTeam = (clientId - 1) % teamCount;
    update.selectedHero = (clientId - 1) % heroCount;
    update.ready = false;
    update.startRequested = false;
    return update;
}

void ApplyLobbyUpdate(LobbyUpdate& current, const LobbyUpdate& incoming,
                      const ServerConfig& config, int clientId)
{
    current.playerName = SanitizeLobbyName(incoming.playerName, clientId);
    if (incoming.selectedTeam >= 0)
    {
        current.selectedTeam = incoming.selectedTeam;
    }
    if (incoming.selectedHero >= 0)
    {
        current.selectedHero = incoming.selectedHero;
    }
    current.ready = incoming.ready;
    current.startRequested = incoming.startRequested;

    const int teamCount = PositiveOrDefault(config.teamCount, 4);
    const int heroCount = PositiveOrDefault(config.heroCount, 6);
    current.selectedTeam = std::clamp(current.selectedTeam, 0, teamCount - 1);
    current.selectedHero = std::clamp(current.selectedHero, 0, heroCount - 1);
}

LobbyValidation ValidateLobbyPlayers(const std::vector<LobbyPlayerState>& players,
                                     const ServerConfig& config)
{
    const int maxPlayers = PositiveOrDefault(config.maxPlayers, 16);
    const int teamCount = PositiveOrDefault(config.teamCount, 4);
    const int maxTeamSize = PositiveOrDefault(config.maxTeamSize, 4);
    const int heroCount = PositiveOrDefault(config.heroCount, 6);
    const int minPlayersToStart = PositiveOrDefault(config.minPlayersToStart, 1);

    if (players.empty())
    {
        return LobbyValidation { false, "waiting for players" };
    }
    if (static_cast<int>(players.size()) < minPlayersToStart)
    {
        return LobbyValidation {
            false,
            "waiting for " + std::to_string(minPlayersToStart) + " players"
        };
    }
    if (static_cast<int>(players.size()) > maxPlayers)
    {
        return LobbyValidation { false, "lobby full" };
    }

    std::vector<int> teamSizes(static_cast<std::size_t>(teamCount), 0);
    std::vector<std::vector<bool>> heroesByTeam(
        static_cast<std::size_t>(teamCount),
        std::vector<bool>(static_cast<std::size_t>(heroCount), false));

    for (const LobbyPlayerState& player : players)
    {
        if (player.selectedTeam < 0 || player.selectedTeam >= teamCount)
        {
            return LobbyValidation { false, "invalid team selection" };
        }
        if (player.selectedHero < 0 || player.selectedHero >= heroCount)
        {
            return LobbyValidation { false, "invalid hero selection" };
        }
        if (config.requireAllReady && !player.ready)
        {
            return LobbyValidation { false, "waiting for ready players" };
        }
        int& teamSize = teamSizes[static_cast<std::size_t>(player.selectedTeam)];
        ++teamSize;
        if (teamSize > maxTeamSize)
        {
            return LobbyValidation { false, "team full" };
        }
        if (config.enforceUniqueHeroesPerTeam)
        {
            const std::size_t teamIndex = static_cast<std::size_t>(player.selectedTeam);
            const std::size_t heroIndex = static_cast<std::size_t>(player.selectedHero);
            if (heroesByTeam[teamIndex][heroIndex])
            {
                return LobbyValidation { false, "duplicate hero in team" };
            }
            heroesByTeam[teamIndex][heroIndex] = true;
        }
    }

    return LobbyValidation { true, "can start" };
}
} // namespace

// ============================ ServerTransport ==============================
struct ServerTransport::Impl
{
    struct ClientChannel
    {
        int clientId = -1;
        int playerId = -1;     // -1 until the game assigns a match player
        bool ackSent = false;  // ConnectAck sent (i.e. lobby entry confirmed)
        std::uint32_t lastProcessedCommandTick = 0;
        LobbyUpdate lobby;
        sockaddr_in addr {};
        Clock::time_point lastSeen;
        Clock::time_point lastSnapshotSent;
        MatchSnapshot snapshotBaseline;
        bool hasSnapshotBaseline = false;
        bool needsFullSnapshot = true;
        bool fullSnapshotAcked = false;
        std::uint32_t snapshotBaselineSequence = 0;
        std::uint32_t pendingFullSnapshotSequence = 0;
        struct ReliablePacket
        {
            std::uint32_t sequence = 0;
            MessageType type = MessageType::Invalid;
            std::vector<std::uint8_t> bytes;
            Clock::time_point lastSent;
            int attempts = 0;
        };
        std::vector<ReliablePacket> reliable;
    };

    struct SlotReservation
    {
        int playerId = -1;
        std::string playerName;
        LobbyUpdate lobby;
    };

    socket_t sock = kInvalidSocket;
    bool inited = false;
    std::string lastError;
    ServerConfig config;
    std::uint16_t boundPort = 0;
    int nextClientId = 1;
    std::uint32_t sequence = 0;
    std::uint32_t packetsReceived = 0;
    std::uint32_t packetsSent = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t bytesSent = 0;
    std::uint32_t staleCommandsDropped = 0;
    std::uint32_t fullSnapshotsSent = 0;
    std::uint32_t deltaSnapshotsSent = 0;
    std::uint32_t resyncRequestsReceived = 0;
    std::size_t lastFullSnapshotBytes = 0;
    std::size_t lastDeltaSnapshotBytes = 0;
    std::uint32_t lobbyRevision = 0;
    int hostClientId = -1;
    std::string lobbyStatusOverride;
    bool matchJoinLocked = false;
    float timeoutSeconds = kDefaultTimeoutSeconds;
    std::vector<ClientChannel> clients;
    std::vector<SlotReservation> reservations;
    std::vector<ReceivedCommand> commands;
    std::vector<DisconnectedClient> disconnected;
    std::vector<ReconnectedClient> reconnected;

    void SendTo(const sockaddr_in& addr, const std::vector<std::uint8_t>& bytes)
    {
        sendto(sock, reinterpret_cast<const char*>(bytes.data()),
               static_cast<int>(bytes.size()), 0,
               reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        ++packetsSent;
        bytesSent += bytes.size();
    }

    void QueueReliable(ClientChannel& client, MessageType type, std::uint32_t seq,
                       const std::vector<std::uint8_t>& bytes, bool replaceSameType)
    {
        if (replaceSameType)
        {
            client.reliable.erase(
                std::remove_if(client.reliable.begin(), client.reliable.end(),
                    [type](const ClientChannel::ReliablePacket& packet)
                    {
                        return packet.type == type;
                    }),
                client.reliable.end());
        }
        SendTo(client.addr, bytes);
        ClientChannel::ReliablePacket packet;
        packet.sequence = seq;
        packet.type = type;
        packet.bytes = bytes;
        packet.lastSent = Clock::now();
        packet.attempts = 1;
        client.reliable.push_back(std::move(packet));
    }

    void AckReliable(ClientChannel& client, std::uint32_t ackedSequence, MessageType ackedType)
    {
        client.reliable.erase(
            std::remove_if(client.reliable.begin(), client.reliable.end(),
                [ackedSequence, ackedType](const ClientChannel::ReliablePacket& packet)
                {
                    return packet.sequence == ackedSequence && packet.type == ackedType;
                }),
            client.reliable.end());
        if (ackedType == MessageType::MatchSnapshot
            && ackedSequence == client.pendingFullSnapshotSequence)
        {
            client.fullSnapshotAcked = true;
        }
    }

    void RetryReliable()
    {
        const Clock::time_point now = Clock::now();
        for (ClientChannel& client : clients)
        {
            for (std::size_t i = 0; i < client.reliable.size();)
            {
                ClientChannel::ReliablePacket& packet = client.reliable[i];
                if (SecondsSince(packet.lastSent) < kReliableRetryIntervalSeconds)
                {
                    ++i;
                    continue;
                }
                if (packet.attempts >= kReliableMaxAttempts)
                {
                    if (packet.type == MessageType::MatchSnapshot
                        && packet.sequence == client.pendingFullSnapshotSequence)
                    {
                        client.needsFullSnapshot = true;
                        client.fullSnapshotAcked = false;
                    }
                    client.reliable.erase(client.reliable.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                SendTo(client.addr, packet.bytes);
                packet.lastSent = now;
                ++packet.attempts;
                ++i;
            }
        }
    }

    ClientChannel* FindByEndpoint(const sockaddr_in& addr)
    {
        for (ClientChannel& c : clients)
        {
            if (SameEndpoint(c.addr, addr))
            {
                return &c;
            }
        }
        return nullptr;
    }
    ClientChannel* FindById(int clientId)
    {
        for (ClientChannel& c : clients)
        {
            if (c.clientId == clientId)
            {
                return &c;
            }
        }
        return nullptr;
    }

    bool IsHost(int clientId) const
    {
        return clientId >= 0 && clientId == hostClientId;
    }

    void ReserveSlotForReconnect(const ClientChannel& client)
    {
        if (!matchJoinLocked || client.playerId < 0 || client.lobby.playerName.empty())
        {
            return;
        }
        reservations.erase(
            std::remove_if(reservations.begin(), reservations.end(),
                [&client](const SlotReservation& reservation)
                {
                    return reservation.playerId == client.playerId
                        || reservation.playerName == client.lobby.playerName;
                }),
            reservations.end());

        SlotReservation reservation;
        reservation.playerId = client.playerId;
        reservation.playerName = client.lobby.playerName;
        reservation.lobby = client.lobby;
        reservation.lobby.ready = true;
        reservation.lobby.startRequested = false;
        reservations.push_back(reservation);
    }

    void NoteDisconnected(const ClientChannel& client)
    {
        disconnected.push_back(DisconnectedClient { client.clientId, client.playerId });
        ReserveSlotForReconnect(client);
    }

    SlotReservation* FindReservationByName(const std::string& playerName)
    {
        for (SlotReservation& reservation : reservations)
        {
            if (reservation.playerName == playerName)
            {
                return &reservation;
            }
        }
        return nullptr;
    }

    void PromoteHostIfNeeded()
    {
        if (clients.empty())
        {
            hostClientId = -1;
            return;
        }
        const auto it = std::find_if(
            clients.begin(), clients.end(),
            [this](const ClientChannel& c) { return c.clientId == hostClientId; });
        if (it == clients.end())
        {
            hostClientId = clients.front().clientId;
        }
    }
};

ServerTransport::ServerTransport() : impl_(std::make_unique<Impl>()) {}
ServerTransport::~ServerTransport() { Close(); }

bool ServerTransport::Start(const ServerConfig& config)
{
    Close();
    impl_->config = config;
    if (!NetInit(impl_->lastError))
    {
        return false;
    }
    impl_->inited = true;

    impl_->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->sock == kInvalidSocket)
    {
        impl_->lastError = "socket() failed (" + std::to_string(LastSocketError()) + ")";
        Close();
        return false;
    }
    SuppressConnReset(impl_->sock);
    if (!SetNonBlocking(impl_->sock))
    {
        impl_->lastError = "could not set non-blocking";
        Close();
        return false;
    }

    sockaddr_in addr;
    if (!ResolveIPv4(config.listenAddress, config.port, addr, impl_->lastError))
    {
        Close();
        return false;
    }
    if (bind(impl_->sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        impl_->lastError = "bind(" + config.listenAddress + ":" + std::to_string(config.port)
            + ") failed (" + std::to_string(LastSocketError()) + ")";
        Close();
        return false;
    }

    // Read back the actually-bound port (handles config.port == 0 => ephemeral).
    sockaddr_in bound;
    socklen_t boundLen = sizeof(bound);
    if (getsockname(impl_->sock, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
    {
        impl_->boundPort = ntohs(bound.sin_port);
    }
    else
    {
        impl_->boundPort = config.port;
    }
    impl_->lastError.clear();
    impl_->lobbyRevision = 0;
    impl_->staleCommandsDropped = 0;
    impl_->fullSnapshotsSent = 0;
    impl_->deltaSnapshotsSent = 0;
    impl_->resyncRequestsReceived = 0;
    impl_->lastFullSnapshotBytes = 0;
    impl_->lastDeltaSnapshotBytes = 0;
    impl_->matchJoinLocked = false;
    impl_->reservations.clear();
    return true;
}

bool ServerTransport::IsOpen() const { return impl_->sock != kInvalidSocket; }

void ServerTransport::Close()
{
    if (impl_->sock != kInvalidSocket)
    {
        CloseSocket(impl_->sock);
        impl_->sock = kInvalidSocket;
    }
    impl_->clients.clear();
    impl_->reservations.clear();
    impl_->commands.clear();
    impl_->disconnected.clear();
    impl_->reconnected.clear();
    impl_->boundPort = 0;
    impl_->lobbyRevision = 0;
    impl_->staleCommandsDropped = 0;
    impl_->fullSnapshotsSent = 0;
    impl_->deltaSnapshotsSent = 0;
    impl_->resyncRequestsReceived = 0;
    impl_->lastFullSnapshotBytes = 0;
    impl_->lastDeltaSnapshotBytes = 0;
    impl_->hostClientId = -1;
    impl_->lobbyStatusOverride.clear();
    impl_->matchJoinLocked = false;
    if (impl_->inited)
    {
        NetShutdown();
        impl_->inited = false;
    }
}

const std::string& ServerTransport::LastError() const { return impl_->lastError; }

void ServerTransport::Poll()
{
    if (impl_->sock == kInvalidSocket)
    {
        return;
    }
    std::vector<std::uint8_t> buffer(65536);
    for (;;)
    {
        sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        const int n = recvfrom(impl_->sock, reinterpret_cast<char*>(buffer.data()),
                               static_cast<int>(buffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0)
        {
            if (n < 0 && !WouldBlock(LastSocketError()))
            {
                // Non-fatal: a stray ICMP/closed-peer error; stop draining now.
            }
            break;
        }
        ++impl_->packetsReceived;
        impl_->bytesReceived += static_cast<std::uint64_t>(n);

        PacketHeader header;
        if (DecodeHeader(buffer.data(), static_cast<std::size_t>(n), header) != DecodeStatus::Ok)
        {
            continue; // bad magic / version / truncated — ignore, never crash
        }

        Impl::ClientChannel* client = impl_->FindByEndpoint(from);
        switch (header.type)
        {
        case MessageType::Connect:
        {
            PacketHeader cHeader;
            std::string token;
            if (DecodeConnect(buffer.data(), static_cast<std::size_t>(n), cHeader, token) != DecodeStatus::Ok)
            {
                break; // malformed connect — ignore
            }
            // Password / token check (task 2). Empty server password accepts any.
            if (!impl_->config.ValidatePassword(token))
            {
                impl_->SendTo(from, EncodeConnectDenied(impl_->sequence++, "bad password"));
                break; // never register a denied client
            }
            if (client == nullptr)
            {
                if (impl_->matchJoinLocked && impl_->reservations.empty())
                {
                    impl_->SendTo(from, EncodeConnectDenied(impl_->sequence++, "match already started"));
                    break;
                }
                if (!impl_->matchJoinLocked
                    && static_cast<int>(impl_->clients.size()) >= PositiveOrDefault(impl_->config.maxPlayers, 16))
                {
                    impl_->SendTo(from, EncodeConnectDenied(impl_->sequence++, "lobby full"));
                    break;
                }
                // Accepted, but not yet assigned a match player — that is the
                // game's job (TakePendingClients -> AssignPlayer).
                Impl::ClientChannel channel;
                channel.clientId = impl_->nextClientId++;
                channel.playerId = -1;
                channel.ackSent = true;
                channel.lobby = DefaultLobbyUpdateForClient(channel.clientId, impl_->config);
                channel.addr = from;
                channel.lastSeen = Clock::now();
                channel.lastSnapshotSent = Clock::now() - std::chrono::seconds(1);
                impl_->clients.push_back(channel);
                Impl::ClientChannel* accepted = &impl_->clients.back();
                if (impl_->hostClientId < 0)
                {
                    impl_->hostClientId = accepted->clientId;
                }
                const std::uint32_t seq = impl_->sequence++;
                impl_->QueueReliable(
                    *accepted,
                    MessageType::ConnectAck,
                    seq,
                    EncodeConnectAck(seq, accepted->clientId),
                    true);
                ++impl_->lobbyRevision;
            }
            else
            {
                client->lastSeen = Clock::now();
                // Re-send the ack if we already assigned this client (the first
                // ack may have been lost; the client keeps retrying Connect).
                const std::uint32_t seq = impl_->sequence++;
                impl_->QueueReliable(
                    *client,
                    MessageType::ConnectAck,
                    seq,
                    EncodeConnectAck(seq, client->clientId),
                    true);
            }
            break;
        }
        case MessageType::Disconnect:
        {
            if (client != nullptr)
            {
                impl_->NoteDisconnected(*client);
                impl_->clients.erase(impl_->clients.begin() + (client - impl_->clients.data()));
                impl_->PromoteHostIfNeeded();
                ++impl_->lobbyRevision;
            }
            break;
        }
        case MessageType::Heartbeat:
        {
            if (client != nullptr)
            {
                client->lastSeen = Clock::now();
            }
            break;
        }
        case MessageType::ReliableAck:
        {
            if (client != nullptr)
            {
                PacketHeader ackHeader;
                std::uint32_t ackedSequence = 0;
                MessageType ackedType = MessageType::Invalid;
                if (DecodeReliableAck(buffer.data(), static_cast<std::size_t>(n),
                                      ackHeader, ackedSequence, ackedType) == DecodeStatus::Ok)
                {
                    impl_->AckReliable(*client, ackedSequence, ackedType);
                    client->lastSeen = Clock::now();
                }
            }
            break;
        }
        case MessageType::FullResyncRequest:
        {
            if (client != nullptr)
            {
                PacketHeader requestHeader;
                std::uint32_t lastKnownSequence = 0;
                std::uint32_t lastKnownTick = 0;
                if (DecodeFullResyncRequest(buffer.data(), static_cast<std::size_t>(n),
                                            requestHeader, lastKnownSequence, lastKnownTick)
                    == DecodeStatus::Ok)
                {
                    (void)lastKnownSequence;
                    (void)lastKnownTick;
                    client->needsFullSnapshot = true;
                    client->fullSnapshotAcked = false;
                    ++impl_->resyncRequestsReceived;
                    client->lastSeen = Clock::now();
                }
            }
            break;
        }
        case MessageType::LobbyUpdate:
        {
            if (client != nullptr)
            {
                PacketHeader lobbyHeader;
                LobbyUpdate update;
                if (DecodeLobbyUpdate(buffer.data(), static_cast<std::size_t>(n), lobbyHeader, update)
                    == DecodeStatus::Ok)
                {
                    if (impl_->matchJoinLocked && client->playerId < 0)
                    {
                        ApplyLobbyUpdate(client->lobby, update, impl_->config, client->clientId);
                        Impl::SlotReservation* reservation =
                            impl_->FindReservationByName(client->lobby.playerName);
                        if (reservation == nullptr)
                        {
                            impl_->SendTo(client->addr,
                                          EncodeConnectDenied(impl_->sequence++, "match already started"));
                            impl_->clients.erase(impl_->clients.begin() + (client - impl_->clients.data()));
                            impl_->PromoteHostIfNeeded();
                            ++impl_->lobbyRevision;
                            break;
                        }

                        const int reservedPlayerId = reservation->playerId;
                        LobbyUpdate reservedLobby = reservation->lobby;
                        impl_->reservations.erase(
                            std::remove_if(impl_->reservations.begin(), impl_->reservations.end(),
                                [reservedPlayerId](const Impl::SlotReservation& entry)
                                {
                                    return entry.playerId == reservedPlayerId;
                                }),
                            impl_->reservations.end());
                        client = impl_->FindByEndpoint(from);
                        if (client == nullptr)
                        {
                            break;
                        }
                        client->playerId = reservedPlayerId;
                        client->lobby = reservedLobby;
                        client->lobby.ready = true;
                        client->lobby.startRequested = false;
                        client->lastProcessedCommandTick = 0;
                        client->lastSeen = Clock::now();
                        impl_->reconnected.push_back(
                            ReconnectedClient { client->clientId, client->playerId });
                        client->hasSnapshotBaseline = false;
                        client->needsFullSnapshot = true;
                        client->fullSnapshotAcked = false;
                        ++impl_->lobbyRevision;
                        const std::uint32_t seq = impl_->sequence++;
                        impl_->QueueReliable(
                            *client,
                            MessageType::LobbySnapshot,
                            seq,
                            EncodeLobbySnapshot(
                                seq,
                                BuildLobbySnapshot(false, true, "reconnected")),
                            true);
                        break;
                    }
                    if (update.startRequested && !impl_->IsHost(client->clientId))
                    {
                        update.startRequested = false;
                        impl_->lobbyStatusOverride = "start denied for non-host";
                    }
                    else if (impl_->IsHost(client->clientId))
                    {
                        impl_->lobbyStatusOverride.clear();
                    }
                    ApplyLobbyUpdate(client->lobby, update, impl_->config, client->clientId);
                    if (!impl_->IsHost(client->clientId))
                    {
                        client->lobby.startRequested = false;
                    }
                    client->lastSeen = Clock::now();
                    ++impl_->lobbyRevision;
                }
            }
            break;
        }
        case MessageType::PlayerCommand:
        {
            // Only assigned clients may drive a player.
            if (client != nullptr && client->ackSent && client->playerId >= 0)
            {
                PacketHeader cmdHeader;
                PlayerCommand command;
                if (DecodePlayerCommand(buffer.data(), static_cast<std::size_t>(n), cmdHeader, command)
                    == DecodeStatus::Ok)
                {
                    if (command.tick <= client->lastProcessedCommandTick)
                    {
                        ++impl_->staleCommandsDropped;
                        client->lastSeen = Clock::now();
                        break;
                    }
                    // Authoritative mapping: stamp to the client's own player.
                    command.controlledPlayerId = static_cast<std::uint32_t>(client->playerId);
                    client->lastProcessedCommandTick = command.tick;
                    impl_->commands.push_back(ReceivedCommand { client->clientId, command });
                    client->lastSeen = Clock::now();
                }
            }
            break;
        }
        default:
            break;
        }
    }

    // Drop clients that have gone silent past the timeout (report so the game
    // frees their player slot).
    for (std::size_t i = 0; i < impl_->clients.size();)
    {
        if (SecondsSince(impl_->clients[i].lastSeen) > impl_->timeoutSeconds)
        {
            impl_->NoteDisconnected(impl_->clients[i]);
            impl_->clients.erase(impl_->clients.begin() + static_cast<std::ptrdiff_t>(i));
            impl_->PromoteHostIfNeeded();
            ++impl_->lobbyRevision;
        }
        else
        {
            ++i;
        }
    }

    impl_->RetryReliable();
}

std::vector<int> ServerTransport::TakePendingClients()
{
    std::vector<int> pending;
    for (const Impl::ClientChannel& c : impl_->clients)
    {
        if (c.playerId < 0)
        {
            pending.push_back(c.clientId);
        }
    }
    return pending;
}

void ServerTransport::AssignPlayer(int clientId, int playerId)
{
    Impl::ClientChannel* client = impl_->FindById(clientId);
    if (client == nullptr)
    {
        return;
    }
    client->playerId = playerId;
    client->ackSent = true;
    client->lastSeen = Clock::now();
    client->lastSnapshotSent = Clock::now() - std::chrono::seconds(1);
    client->hasSnapshotBaseline = false;
    client->needsFullSnapshot = true;
    client->fullSnapshotAcked = false;
    client->snapshotBaselineSequence = 0;
    client->pendingFullSnapshotSequence = 0;
    ++impl_->lobbyRevision;
}

std::vector<DisconnectedClient> ServerTransport::TakeDisconnectedClients()
{
    std::vector<DisconnectedClient> out;
    out.swap(impl_->disconnected);
    return out;
}

std::vector<ReconnectedClient> ServerTransport::TakeReconnectedClients()
{
    std::vector<ReconnectedClient> out;
    out.swap(impl_->reconnected);
    return out;
}

void ServerTransport::SetMatchJoinLocked(bool locked)
{
    impl_->matchJoinLocked = locked;
    if (!locked)
    {
        impl_->reservations.clear();
    }
}

std::vector<LobbyPlayerState> ServerTransport::LobbyPlayers() const
{
    std::vector<LobbyPlayerState> players;
    players.reserve(impl_->clients.size());
    for (const Impl::ClientChannel& client : impl_->clients)
    {
        LobbyPlayerState state;
        state.clientId = client.clientId;
        state.assignedPlayerId = client.playerId;
        state.playerName = client.lobby.playerName;
        state.selectedTeam = client.lobby.selectedTeam;
        state.selectedHero = client.lobby.selectedHero;
        state.ready = client.lobby.ready;
        state.connected = true;
        state.startRequested = client.lobby.startRequested;
        players.push_back(state);
    }
    return players;
}

LobbyValidation ServerTransport::ValidateLobbyStart() const
{
    return ValidateLobbyPlayers(LobbyPlayers(), impl_->config);
}

bool ServerTransport::LobbyStartRequested() const
{
    for (const Impl::ClientChannel& client : impl_->clients)
    {
        if (impl_->IsHost(client.clientId) && client.lobby.startRequested)
        {
            return true;
        }
    }
    return false;
}

LobbySnapshot ServerTransport::BuildLobbySnapshot(
    bool matchStarting, bool matchStarted, const std::string& statusMessage) const
{
    LobbySnapshot snapshot;
    snapshot.revision = impl_->lobbyRevision;
    snapshot.hostClientId = impl_->hostClientId;
    snapshot.serverName = impl_->config.serverName;
    snapshot.privateServer = impl_->config.privateServer;
    snapshot.maxPlayers = impl_->config.maxPlayers;
    snapshot.teamCount = impl_->config.teamCount;
    snapshot.maxTeamSize = impl_->config.maxTeamSize;
    snapshot.heroCount = impl_->config.heroCount;
    snapshot.enforceUniqueHeroesPerTeam = impl_->config.enforceUniqueHeroesPerTeam;
    snapshot.requireAllReady = impl_->config.requireAllReady;
    snapshot.worldBiome = impl_->config.worldBiome;
    snapshot.worldLayout = impl_->config.worldLayout;
    snapshot.matchMode = impl_->config.matchMode;
    snapshot.players = LobbyPlayers();
    const LobbyValidation validation = ValidateLobbyPlayers(snapshot.players, impl_->config);
    snapshot.canStart = validation.canStart;
    snapshot.matchStarting = matchStarting;
    snapshot.matchStarted = matchStarted;
    if (!statusMessage.empty())
    {
        snapshot.statusMessage = statusMessage;
    }
    else if (!validation.canStart)
    {
        snapshot.statusMessage = validation.reason;
    }
    else if (!impl_->lobbyStatusOverride.empty())
    {
        snapshot.statusMessage = impl_->lobbyStatusOverride;
    }
    else
    {
        snapshot.statusMessage = validation.reason;
    }
    return snapshot;
}

void ServerTransport::BroadcastLobbySnapshot(const LobbySnapshot& snapshot)
{
    if (impl_->sock == kInvalidSocket)
    {
        return;
    }
    for (Impl::ClientChannel& client : impl_->clients)
    {
        const std::uint32_t seq = impl_->sequence++;
        impl_->QueueReliable(
            client,
            MessageType::LobbySnapshot,
            seq,
            EncodeLobbySnapshot(seq, snapshot),
            true);
    }
}

void ServerTransport::SendLobbySnapshotToClient(int clientId, const LobbySnapshot& snapshot)
{
    if (impl_->sock == kInvalidSocket)
    {
        return;
    }
    Impl::ClientChannel* client = impl_->FindById(clientId);
    if (client != nullptr)
    {
        const std::uint32_t seq = impl_->sequence++;
        impl_->QueueReliable(
            *client,
            MessageType::LobbySnapshot,
            seq,
            EncodeLobbySnapshot(seq, snapshot),
            true);
    }
}

void ServerTransport::SendSnapshotToClient(int clientId, const MatchSnapshot& snapshot)
{
    if (impl_->sock == kInvalidSocket)
    {
        return;
    }
    Impl::ClientChannel* client = impl_->FindById(clientId);
    if (client != nullptr && client->playerId >= 0)
    {
        MatchSnapshot perClientSnapshot = snapshot;
        perClientSnapshot.lastProcessedCommandTick = client->lastProcessedCommandTick;
        const Clock::time_point now = Clock::now();
        const bool needsFull = !client->hasSnapshotBaseline || client->needsFullSnapshot;
        if (!needsFull && !client->fullSnapshotAcked)
        {
            return;
        }
        if (!needsFull && SecondsSince(client->lastSnapshotSent) < kSnapshotSendIntervalSeconds)
        {
            return;
        }

        if (needsFull)
        {
            const std::uint32_t seq = impl_->sequence++;
            const std::vector<std::uint8_t> bytes = EncodeMatchSnapshot(seq, perClientSnapshot);
            impl_->QueueReliable(*client, MessageType::MatchSnapshot, seq, bytes, true);
            client->snapshotBaseline = perClientSnapshot;
            client->snapshotBaselineSequence = seq;
            client->pendingFullSnapshotSequence = seq;
            client->hasSnapshotBaseline = true;
            client->needsFullSnapshot = false;
            client->fullSnapshotAcked = false;
            client->lastSnapshotSent = now;
            ++impl_->fullSnapshotsSent;
            impl_->lastFullSnapshotBytes = bytes.size();
            return;
        }

        MatchSnapshotDelta delta = BuildSnapshotDelta(
            client->snapshotBaseline,
            perClientSnapshot,
            client->snapshotBaselineSequence,
            client->playerId);
        const std::uint32_t seq = impl_->sequence++;
        std::vector<std::uint8_t> bytes = EncodeSnapshotDelta(seq, delta);
        if (bytes.size() > kMaxSnapshotPacketBytes)
        {
            client->needsFullSnapshot = true;
            client->fullSnapshotAcked = false;
            const std::uint32_t fullSeq = impl_->sequence++;
            const std::vector<std::uint8_t> fullBytes = EncodeMatchSnapshot(fullSeq, perClientSnapshot);
            impl_->QueueReliable(*client, MessageType::MatchSnapshot, fullSeq, fullBytes, true);
            client->snapshotBaseline = perClientSnapshot;
            client->snapshotBaselineSequence = fullSeq;
            client->pendingFullSnapshotSequence = fullSeq;
            client->hasSnapshotBaseline = true;
            client->needsFullSnapshot = false;
            client->lastSnapshotSent = now;
            ++impl_->fullSnapshotsSent;
            impl_->lastFullSnapshotBytes = fullBytes.size();
            return;
        }
        impl_->SendTo(client->addr, bytes);
        client->snapshotBaseline = perClientSnapshot;
        client->snapshotBaselineSequence = seq;
        client->lastSnapshotSent = now;
        ++impl_->deltaSnapshotsSent;
        impl_->lastDeltaSnapshotBytes = bytes.size();
    }
}

void ServerTransport::BroadcastSnapshot(const MatchSnapshot& snapshot)
{
    if (impl_->sock == kInvalidSocket)
    {
        return;
    }
    for (Impl::ClientChannel& client : impl_->clients)
    {
        if (client.playerId >= 0)
        {
            SendSnapshotToClient(client.clientId, snapshot);
        }
    }
}

std::vector<ReceivedCommand> ServerTransport::DrainCommands()
{
    std::vector<ReceivedCommand> drained;
    drained.swap(impl_->commands);
    return drained;
}

std::size_t ServerTransport::ClientCount() const { return impl_->clients.size(); }
std::uint16_t ServerTransport::BoundPort() const { return impl_->boundPort; }
std::uint32_t ServerTransport::PacketsReceived() const { return impl_->packetsReceived; }
std::uint32_t ServerTransport::PacketsSent() const { return impl_->packetsSent; }
std::uint64_t ServerTransport::BytesReceived() const { return impl_->bytesReceived; }
std::uint64_t ServerTransport::BytesSent() const { return impl_->bytesSent; }
std::uint32_t ServerTransport::StaleCommandsDropped() const { return impl_->staleCommandsDropped; }
std::uint32_t ServerTransport::FullSnapshotsSent() const { return impl_->fullSnapshotsSent; }
std::uint32_t ServerTransport::DeltaSnapshotsSent() const { return impl_->deltaSnapshotsSent; }
std::uint32_t ServerTransport::ResyncRequestsReceived() const { return impl_->resyncRequestsReceived; }
std::size_t ServerTransport::LastFullSnapshotBytes() const { return impl_->lastFullSnapshotBytes; }
std::size_t ServerTransport::LastDeltaSnapshotBytes() const { return impl_->lastDeltaSnapshotBytes; }

std::uint32_t ServerTransport::LastProcessedCommandTick(int clientId) const
{
    const Impl::ClientChannel* client = impl_->FindById(clientId);
    return client != nullptr ? client->lastProcessedCommandTick : 0;
}

int ServerTransport::PlayerForClient(int clientId) const
{
    const Impl::ClientChannel* client = impl_->FindById(clientId);
    return client != nullptr ? client->playerId : -1;
}

std::vector<int> ServerTransport::ConnectedClients() const
{
    std::vector<int> ids;
    for (const Impl::ClientChannel& c : impl_->clients)
    {
        if (c.playerId >= 0)
        {
            ids.push_back(c.clientId);
        }
    }
    return ids;
}

// ============================ ClientTransport ==============================
struct ClientTransport::Impl
{
    socket_t sock = kInvalidSocket;
    bool inited = false;
    std::string lastError;
    sockaddr_in serverAddr {};
    std::string token;
    bool connected = false;
    bool inMatch = false;
    bool timedOut = false;
    bool denied = false;
    std::string denyReason;
    int lobbyClientId = -1;
    int assignedPlayerId = -1;
    std::uint32_t sequence = 0;
    std::uint32_t packetsReceived = 0;
    std::uint32_t packetsSent = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t rateBytes = 0;
    std::uint32_t ratePackets = 0;
    float bytesPerSecond = 0.0f;
    float packetsPerSecond = 0.0f;
    float timeoutSeconds = kDefaultTimeoutSeconds;
    Clock::time_point openTime;
    Clock::time_point lastServerSeen;
    Clock::time_point lastHello;
    Clock::time_point latestSnapshotReceivedAt;
    Clock::time_point lastCommandReplay;
    Clock::time_point lastFullResyncRequest;
    Clock::time_point rateWindowStart;
    bool hasSnapshot = false;
    bool hasLobbySnapshot = false;
    bool fullResyncRequested = false;
    std::uint32_t droppedSnapshots = 0;
    std::uint32_t ignoredSnapshots = 0;
    std::uint32_t acceptedSnapshots = 0;
    std::uint32_t lastSnapshotSequence = 0;
    std::uint32_t lastLobbySequence = 0;
    std::uint32_t fullSnapshotsReceived = 0;
    std::uint32_t deltaSnapshotsReceived = 0;
    std::uint32_t resyncRequestsSent = 0;
    std::size_t lastFullSnapshotBytes = 0;
    std::size_t lastDeltaSnapshotBytes = 0;
    std::uint32_t lastAckedCommandTick = 0;
    std::vector<PlayerCommand> pendingCommands;
    MatchSnapshot latestSnapshot;
    LobbySnapshot latestLobbySnapshot;

    void SendBytes(const std::vector<std::uint8_t>& bytes)
    {
        if (sock == kInvalidSocket)
        {
            return;
        }
        sendto(sock, reinterpret_cast<const char*>(bytes.data()),
               static_cast<int>(bytes.size()), 0,
               reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
        ++packetsSent;
        bytesSent += bytes.size();
    }

    void NoteReceived(int byteCount)
    {
        ++packetsReceived;
        bytesReceived += static_cast<std::uint64_t>(byteCount);
        rateBytes += static_cast<std::uint64_t>(byteCount);
        ++ratePackets;
        const double elapsed = SecondsSince(rateWindowStart);
        if (elapsed >= 1.0)
        {
            bytesPerSecond = static_cast<float>(static_cast<double>(rateBytes) / elapsed);
            packetsPerSecond = static_cast<float>(static_cast<double>(ratePackets) / elapsed);
            rateBytes = 0;
            ratePackets = 0;
            rateWindowStart = Clock::now();
        }
    }

    void SendAck(const PacketHeader& acked)
    {
        SendBytes(EncodeReliableAck(sequence++, acked.sequence, acked.type));
    }

    void RequestFullResync(bool immediate)
    {
        fullResyncRequested = true;
        if (!immediate && SecondsSince(lastFullResyncRequest) < kFullResyncRetryIntervalSeconds)
        {
            return;
        }
        SendBytes(EncodeFullResyncRequest(
            sequence++,
            lastSnapshotSequence,
            hasSnapshot ? latestSnapshot.tick : 0));
        lastFullResyncRequest = Clock::now();
        ++resyncRequestsSent;
    }
};

ClientTransport::ClientTransport() : impl_(std::make_unique<Impl>()) {}
ClientTransport::~ClientTransport() { Close(); }

bool ClientTransport::Open(const std::string& host, std::uint16_t port, const std::string& token,
                           float timeoutSeconds)
{
    Close();
    impl_->token = token;
    if (!NetInit(impl_->lastError))
    {
        return false;
    }
    impl_->inited = true;

    if (!ResolveIPv4(host, port, impl_->serverAddr, impl_->lastError))
    {
        Close();
        return false;
    }

    impl_->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->sock == kInvalidSocket)
    {
        impl_->lastError = "socket() failed (" + std::to_string(LastSocketError()) + ")";
        Close();
        return false;
    }
    SuppressConnReset(impl_->sock);
    if (!SetNonBlocking(impl_->sock))
    {
        impl_->lastError = "could not set non-blocking";
        Close();
        return false;
    }

    // Bind an ephemeral local port so we can receive replies.
    sockaddr_in local;
    std::memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (bind(impl_->sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
    {
        impl_->lastError = "client bind failed (" + std::to_string(LastSocketError()) + ")";
        Close();
        return false;
    }

    impl_->timeoutSeconds = timeoutSeconds > 0.0f ? timeoutSeconds : kDefaultTimeoutSeconds;
    impl_->openTime = Clock::now();
    impl_->lastServerSeen = impl_->openTime;
    impl_->lastHello = impl_->openTime;
    impl_->latestSnapshotReceivedAt = impl_->openTime;
    impl_->lastCommandReplay = impl_->openTime;
    impl_->lastFullResyncRequest = impl_->openTime - std::chrono::seconds(1);
    impl_->rateWindowStart = impl_->openTime;
    impl_->connected = false;
    impl_->inMatch = false;
    impl_->timedOut = false;
    impl_->denied = false;
    impl_->denyReason.clear();
    impl_->lobbyClientId = -1;
    impl_->assignedPlayerId = -1;
    impl_->packetsReceived = 0;
    impl_->packetsSent = 0;
    impl_->bytesReceived = 0;
    impl_->bytesSent = 0;
    impl_->hasSnapshot = false;
    impl_->hasLobbySnapshot = false;
    impl_->fullResyncRequested = false;
    impl_->droppedSnapshots = 0;
    impl_->ignoredSnapshots = 0;
    impl_->acceptedSnapshots = 0;
    impl_->lastSnapshotSequence = 0;
    impl_->lastLobbySequence = 0;
    impl_->fullSnapshotsReceived = 0;
    impl_->deltaSnapshotsReceived = 0;
    impl_->resyncRequestsSent = 0;
    impl_->lastFullSnapshotBytes = 0;
    impl_->lastDeltaSnapshotBytes = 0;
    impl_->rateBytes = 0;
    impl_->ratePackets = 0;
    impl_->bytesPerSecond = 0.0f;
    impl_->packetsPerSecond = 0.0f;
    impl_->lastAckedCommandTick = 0;
    impl_->pendingCommands.clear();

    // Say hello with the join token. Delivery is best-effort; Poll() re-sends it
    // until we get a ConnectAck or ConnectDenied. A dead server just never
    // replies and IsConnected() stays false.
    const std::vector<std::uint8_t> hello = EncodeConnect(impl_->sequence++, impl_->token);
    impl_->SendBytes(hello);
    impl_->lastError.clear();
    return true;
}

bool ClientTransport::Connect(const std::string& host, std::uint16_t port, const std::string& token,
                              float timeoutSeconds)
{
    if (!Open(host, port, token, timeoutSeconds))
    {
        return false;
    }
    const Clock::time_point deadline = Clock::now()
        + std::chrono::milliseconds(static_cast<long long>(impl_->timeoutSeconds * 1000.0f));
    while (Clock::now() < deadline && !impl_->connected && !impl_->denied)
    {
        Poll(); // re-sends the hello as needed
        if (impl_->connected || impl_->denied)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!impl_->connected && !impl_->denied)
    {
        impl_->timedOut = true;
    }
    return impl_->connected;
}

bool ClientTransport::IsOpen() const { return impl_->sock != kInvalidSocket; }

void ClientTransport::Close()
{
    if (impl_->sock != kInvalidSocket)
    {
        CloseSocket(impl_->sock);
        impl_->sock = kInvalidSocket;
    }
    impl_->connected = false;
    impl_->inMatch = false;
    impl_->hasSnapshot = false;
    impl_->hasLobbySnapshot = false;
    impl_->fullResyncRequested = false;
    impl_->droppedSnapshots = 0;
    impl_->ignoredSnapshots = 0;
    impl_->acceptedSnapshots = 0;
    impl_->lastSnapshotSequence = 0;
    impl_->lastLobbySequence = 0;
    impl_->fullSnapshotsReceived = 0;
    impl_->deltaSnapshotsReceived = 0;
    impl_->resyncRequestsSent = 0;
    impl_->lastFullSnapshotBytes = 0;
    impl_->lastDeltaSnapshotBytes = 0;
    impl_->lastAckedCommandTick = 0;
    impl_->pendingCommands.clear();
    impl_->assignedPlayerId = -1;
    impl_->lobbyClientId = -1;
    if (impl_->inited)
    {
        NetShutdown();
        impl_->inited = false;
    }
}

const std::string& ClientTransport::LastError() const { return impl_->lastError; }

void ClientTransport::Poll()
{
    if (impl_->sock == kInvalidSocket)
    {
        return;
    }
    std::vector<std::uint8_t> buffer(65536);
    for (;;)
    {
        sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        const int n = recvfrom(impl_->sock, reinterpret_cast<char*>(buffer.data()),
                               static_cast<int>(buffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0)
        {
            break;
        }
        if (!SameEndpoint(from, impl_->serverAddr))
        {
            continue; // only trust packets from our server
        }
        impl_->NoteReceived(n);

        PacketHeader header;
        if (DecodeHeader(buffer.data(), static_cast<std::size_t>(n), header) != DecodeStatus::Ok)
        {
            continue;
        }
        impl_->lastServerSeen = Clock::now();
        switch (header.type)
        {
        case MessageType::ConnectAck:
        {
            PacketHeader ackHeader;
            int clientId = -1;
            if (DecodeConnectAck(buffer.data(), static_cast<std::size_t>(n), ackHeader, clientId)
                == DecodeStatus::Ok)
            {
                impl_->SendAck(ackHeader);
                impl_->connected = true;
                impl_->timedOut = false;
                impl_->lobbyClientId = clientId;
            }
            break;
        }
        case MessageType::LobbySnapshot:
        {
            PacketHeader lobbyHeader;
            LobbySnapshot snapshot;
            if (DecodeLobbySnapshot(buffer.data(), static_cast<std::size_t>(n), lobbyHeader, snapshot)
                == DecodeStatus::Ok)
            {
                impl_->SendAck(lobbyHeader);
                if (lobbyHeader.sequence <= impl_->lastLobbySequence
                    && impl_->hasLobbySnapshot
                    && snapshot.revision <= impl_->latestLobbySnapshot.revision)
                {
                    break;
                }
                impl_->lastLobbySequence = lobbyHeader.sequence;
                impl_->latestLobbySnapshot = std::move(snapshot);
                impl_->hasLobbySnapshot = true;
                impl_->connected = true;
                impl_->timedOut = false;
                if (impl_->latestLobbySnapshot.matchStarted)
                {
                    for (const LobbyPlayerState& player : impl_->latestLobbySnapshot.players)
                    {
                        if (player.clientId == impl_->lobbyClientId && player.assignedPlayerId >= 0)
                        {
                            impl_->assignedPlayerId = player.assignedPlayerId;
                            impl_->inMatch = true;
                            break;
                        }
                    }
                }
            }
            break;
        }
        case MessageType::MatchSnapshot:
        {
            PacketHeader snapHeader;
            MatchSnapshot snapshot;
            if (DecodeMatchSnapshot(buffer.data(), static_cast<std::size_t>(n), snapHeader, snapshot)
                == DecodeStatus::Ok)
            {
                impl_->SendAck(snapHeader);
                impl_->lastFullSnapshotBytes = static_cast<std::size_t>(n);
                if (impl_->hasSnapshot && snapHeader.sequence <= impl_->lastSnapshotSequence)
                {
                    ++impl_->ignoredSnapshots;
                    break;
                }
                impl_->latestSnapshot = std::move(snapshot);
                impl_->hasSnapshot = true;
                impl_->lastSnapshotSequence = snapHeader.sequence;
                impl_->fullResyncRequested = false;
                ++impl_->fullSnapshotsReceived;
                ++impl_->acceptedSnapshots;
                impl_->latestSnapshotReceivedAt = Clock::now();
                if (impl_->latestSnapshot.lastProcessedCommandTick > impl_->lastAckedCommandTick)
                {
                    impl_->lastAckedCommandTick = impl_->latestSnapshot.lastProcessedCommandTick;
                }
                impl_->pendingCommands.erase(
                    std::remove_if(impl_->pendingCommands.begin(), impl_->pendingCommands.end(),
                        [this](const PlayerCommand& command)
                        {
                            return command.tick <= impl_->lastAckedCommandTick;
                        }),
                    impl_->pendingCommands.end());
                impl_->connected = true; // a snapshot implies the server accepted us
                impl_->timedOut = false;
                impl_->inMatch = impl_->assignedPlayerId >= 0;
            }
            break;
        }
        case MessageType::SnapshotDelta:
        {
            PacketHeader snapHeader;
            MatchSnapshotDelta delta;
            if (DecodeSnapshotDelta(buffer.data(), static_cast<std::size_t>(n), snapHeader, delta)
                == DecodeStatus::Ok)
            {
                impl_->lastDeltaSnapshotBytes = static_cast<std::size_t>(n);
                const SnapshotDeltaApplyStatus applyStatus = ApplySnapshotDeltaIfCompatible(
                    impl_->hasSnapshot,
                    impl_->latestSnapshot,
                    impl_->lastSnapshotSequence,
                    snapHeader.sequence,
                    delta);
                if (applyStatus == SnapshotDeltaApplyStatus::OldSnapshot)
                {
                    ++impl_->ignoredSnapshots;
                    break;
                }
                if (SnapshotDeltaStatusNeedsFullResync(applyStatus))
                {
                    ++impl_->droppedSnapshots;
                    impl_->RequestFullResync(true);
                    break;
                }
                impl_->hasSnapshot = true;
                ++impl_->deltaSnapshotsReceived;
                ++impl_->acceptedSnapshots;
                impl_->latestSnapshotReceivedAt = Clock::now();
                if (impl_->latestSnapshot.lastProcessedCommandTick > impl_->lastAckedCommandTick)
                {
                    impl_->lastAckedCommandTick = impl_->latestSnapshot.lastProcessedCommandTick;
                }
                impl_->pendingCommands.erase(
                    std::remove_if(impl_->pendingCommands.begin(), impl_->pendingCommands.end(),
                        [this](const PlayerCommand& command)
                        {
                            return command.tick <= impl_->lastAckedCommandTick;
                        }),
                    impl_->pendingCommands.end());
                impl_->connected = true;
                impl_->timedOut = false;
                impl_->inMatch = impl_->assignedPlayerId >= 0;
            }
            break;
        }
        case MessageType::ConnectDenied:
        {
            PacketHeader denyHeader;
            std::string reason;
            if (DecodeConnectDenied(buffer.data(), static_cast<std::size_t>(n), denyHeader, reason)
                == DecodeStatus::Ok)
            {
                impl_->SendAck(denyHeader);
                impl_->denied = true;
                impl_->denyReason = reason;
                impl_->connected = false;
                impl_->inMatch = false;
                impl_->pendingCommands.clear();
            }
            break;
        }
        case MessageType::Disconnect:
            impl_->connected = false;
            impl_->inMatch = false;
            break;
        default:
            break;
        }
    }

    // Keep retrying the handshake until accepted or denied (covers a lost
    // Connect/ConnectAck on best-effort UDP).
    if (!impl_->connected && !impl_->denied && SecondsSince(impl_->lastHello) > 0.15)
    {
        const std::vector<std::uint8_t> hello = EncodeConnect(impl_->sequence++, impl_->token);
        impl_->SendBytes(hello);
        impl_->lastHello = Clock::now();
    }
    else if (impl_->connected && SecondsSince(impl_->lastHello) > 0.5)
    {
        const std::vector<std::uint8_t> heartbeat =
            EncodeControl(MessageType::Heartbeat, impl_->sequence++, 0);
        impl_->SendBytes(heartbeat);
        impl_->lastHello = Clock::now();
    }

    if (impl_->connected && impl_->fullResyncRequested)
    {
        impl_->RequestFullResync(false);
    }

    if (impl_->connected && impl_->inMatch && !impl_->pendingCommands.empty()
        && SecondsSince(impl_->lastCommandReplay) > kCommandReplayIntervalSeconds)
    {
        for (const PlayerCommand& command : impl_->pendingCommands)
        {
            const std::vector<std::uint8_t> bytes = EncodePlayerCommand(impl_->sequence++, command);
            impl_->SendBytes(bytes);
        }
        impl_->lastCommandReplay = Clock::now();
    }

    if (impl_->connected && SecondsSince(impl_->lastServerSeen) > impl_->timeoutSeconds)
    {
        impl_->connected = false;
        impl_->inMatch = false;
        impl_->timedOut = true;
    }
}

void ClientTransport::SendCommand(const PlayerCommand& command)
{
    if (impl_->sock == kInvalidSocket)
    {
        return;
    }
    const auto existing = std::find_if(impl_->pendingCommands.begin(), impl_->pendingCommands.end(),
        [&command](const PlayerCommand& pending)
        {
            return pending.tick == command.tick;
        });
    if (existing != impl_->pendingCommands.end())
    {
        *existing = command;
    }
    else
    {
        impl_->pendingCommands.push_back(command);
        while (impl_->pendingCommands.size() > kMaxPendingReliableCommands)
        {
            impl_->pendingCommands.erase(impl_->pendingCommands.begin());
        }
    }
    const std::vector<std::uint8_t> bytes = EncodePlayerCommand(impl_->sequence++, command);
    impl_->SendBytes(bytes);
    impl_->lastCommandReplay = Clock::now();
}

void ClientTransport::SendLobbyUpdate(const LobbyUpdate& update)
{
    if (impl_->sock == kInvalidSocket || !impl_->connected)
    {
        return;
    }
    const std::vector<std::uint8_t> bytes = EncodeLobbyUpdate(impl_->sequence++, update);
    impl_->SendBytes(bytes);
}

void ClientTransport::Disconnect()
{
    if (impl_->sock != kInvalidSocket)
    {
        const std::vector<std::uint8_t> bye = EncodeControl(MessageType::Disconnect, impl_->sequence++, 0);
        impl_->SendBytes(bye);
    }
    Close();
}

bool ClientTransport::IsConnected() const { return impl_->connected; }
bool ClientTransport::TimedOut() const { return impl_->timedOut; }
bool ClientTransport::WasDenied() const { return impl_->denied; }
const std::string& ClientTransport::DenyReason() const { return impl_->denyReason; }
int ClientTransport::LobbyClientId() const { return impl_->lobbyClientId; }
int ClientTransport::AssignedPlayerId() const { return impl_->assignedPlayerId; }
bool ClientTransport::InMatch() const { return impl_->inMatch; }
bool ClientTransport::HasLobbySnapshot() const { return impl_->hasLobbySnapshot; }
const LobbySnapshot& ClientTransport::LatestLobbySnapshot() const { return impl_->latestLobbySnapshot; }
bool ClientTransport::HasSnapshot() const { return impl_->hasSnapshot; }
const MatchSnapshot& ClientTransport::LatestSnapshot() const { return impl_->latestSnapshot; }
std::uint32_t ClientTransport::PacketsReceived() const { return impl_->packetsReceived; }
std::uint32_t ClientTransport::PacketsSent() const { return impl_->packetsSent; }
std::uint64_t ClientTransport::BytesReceived() const { return impl_->bytesReceived; }
std::uint64_t ClientTransport::BytesSent() const { return impl_->bytesSent; }
float ClientTransport::BytesPerSecond() const { return impl_->bytesPerSecond; }
float ClientTransport::PacketsPerSecond() const { return impl_->packetsPerSecond; }
std::uint32_t ClientTransport::DroppedSnapshots() const { return impl_->droppedSnapshots; }
std::uint32_t ClientTransport::IgnoredSnapshots() const { return impl_->ignoredSnapshots; }
std::uint32_t ClientTransport::FullSnapshotsReceived() const { return impl_->fullSnapshotsReceived; }
std::uint32_t ClientTransport::DeltaSnapshotsReceived() const { return impl_->deltaSnapshotsReceived; }
std::uint32_t ClientTransport::ResyncRequestsSent() const { return impl_->resyncRequestsSent; }
std::size_t ClientTransport::LastFullSnapshotBytes() const { return impl_->lastFullSnapshotBytes; }
std::size_t ClientTransport::LastDeltaSnapshotBytes() const { return impl_->lastDeltaSnapshotBytes; }
std::uint32_t ClientTransport::LastAckedCommandTick() const { return impl_->lastAckedCommandTick; }
std::size_t ClientTransport::PendingCommandCount() const { return impl_->pendingCommands.size(); }
float ClientTransport::SnapshotAgeSeconds() const
{
    return impl_->hasSnapshot ? static_cast<float>(SecondsSince(impl_->latestSnapshotReceivedAt)) : 0.0f;
}

bool NetworkTransportAvailable() { return true; }

// ============================== Smoke =======================================
namespace
{
MatchSnapshot MakeServerSnapshot(std::uint32_t tick)
{
    MatchSnapshot s;
    s.tick = tick;
    s.matchTime = static_cast<float>(tick) / 60.0f;
    s.phase = MatchPhase::Playing;
    s.winnerTeamId = -1;

    PlayerSnapshot p0;
    p0.playerId = 1;
    p0.teamId = 0;
    p0.heroId = 0;
    // Advance the X position with the tick so the client can see live state.
    p0.position = Vec3 { -38.0f + static_cast<float>(tick) * 0.1f, 1.5f, 0.0f };
    p0.yaw = 0.2f;
    p0.health = 100;
    p0.maxHealth = 100;
    p0.alive = true;
    s.players.push_back(p0);

    PlayerSnapshot p1;
    p1.playerId = 2;
    p1.teamId = 1;
    p1.heroId = 1;
    p1.position = Vec3 { 38.0f, 1.5f, 0.0f };
    p1.yaw = -2.8f;
    p1.health = 100;
    p1.maxHealth = 100;
    p1.alive = true;
    s.players.push_back(p1);

    s.cores.push_back(CoreSnapshot { 0, 500, 500, true });
    s.cores.push_back(CoreSnapshot { 1, 500, 500, true });
    return s;
}
} // namespace

int RunLocalhostNetSmoke(const ServerConfig& config)
{
    ServerTransport server;
    // Bind to loopback on an ephemeral port so the smoke never collides with a
    // port already in use (the client connects to the actually-bound port).
    ServerConfig serverCfg = config;
    serverCfg.listenAddress = "127.0.0.1";
    serverCfg.port = 0;
    serverCfg.minPlayersToStart = 1;
    if (!server.Start(serverCfg))
    {
        std::cout << "localhost-net-smoke: server start failed: " << server.LastError() << '\n';
        std::cout << "LOCALHOST_NET_SMOKE_FAIL" << std::endl;
        return 8;
    }
    const std::uint16_t port = server.BoundPort();
    std::cout << "localhost-net-smoke: server listening on 127.0.0.1:" << port
              << " (headless, no window)\n";

    ClientTransport client;
    if (!client.Open("127.0.0.1", port, "", 3.0f))
    {
        std::cout << "localhost-net-smoke: client open failed: " << client.LastError() << '\n';
        std::cout << "LOCALHOST_NET_SMOKE_FAIL" << std::endl;
        server.Close();
        return 8;
    }

    // Service both sides in one thread until the client has the snapshot and the
    // server has seen a command, or we hit the wall-clock budget.
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(3);
    std::uint32_t tick = 0;
    int nextPlayer = 1;
    bool clientSentLobbyUpdate = false;
    bool matchAssigned = false;
    bool serverSawCommand = false;
    bool clientSentCommand = false;
    while (Clock::now() < deadline)
    {
        server.Poll();
        if (!matchAssigned)
        {
            const LobbyValidation validation = server.ValidateLobbyStart();
            if (validation.canStart && server.LobbyStartRequested())
            {
                for (int clientId : server.TakePendingClients())
                {
                    server.AssignPlayer(clientId, nextPlayer++);
                }
                matchAssigned = true;
                server.BroadcastLobbySnapshot(server.BuildLobbySnapshot(true, true, "match started"));
            }
            else if (server.ClientCount() > 0)
            {
                server.BroadcastLobbySnapshot(server.BuildLobbySnapshot(false, false, validation.reason));
            }
        }
        if (matchAssigned && server.ClientCount() > 0)
        {
            server.BroadcastSnapshot(MakeServerSnapshot(++tick));
        }
        for (const ReceivedCommand& rc : server.DrainCommands())
        {
            if (rc.command.moveForward > 0.5f)
            {
                serverSawCommand = true;
            }
        }

        client.Poll();
        if (client.IsConnected() && !clientSentLobbyUpdate)
        {
            LobbyUpdate update;
            update.playerName = "Smoke Client";
            update.selectedTeam = 0;
            update.selectedHero = 0;
            update.ready = true;
            update.startRequested = true;
            client.SendLobbyUpdate(update);
            clientSentLobbyUpdate = true;
        }
        if (client.InMatch() && !clientSentCommand)
        {
            PlayerCommand cmd;
            cmd.controlledPlayerId = static_cast<std::uint32_t>(client.AssignedPlayerId());
            cmd.tick = client.HasSnapshot() ? client.LatestSnapshot().tick + 1 : tick + 1;
            cmd.moveForward = 1.0f;
            client.SendCommand(cmd);
            clientSentCommand = true;
        }

        if (client.HasSnapshot() && serverSawCommand && client.IsConnected())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const bool connected = client.IsConnected();
    const bool gotSnapshot = client.HasSnapshot();
    const MatchSnapshot& snap = client.LatestSnapshot();
    const bool snapshotValid = gotSnapshot && snap.tick > 0 && !snap.players.empty();
    const int assignedPlayer = client.AssignedPlayerId();

    std::cout << "localhost-net-smoke: connected=" << (connected ? "yes" : "no")
              << " assignedPlayer=" << assignedPlayer
              << " gotSnapshot=" << (gotSnapshot ? "yes" : "no")
              << " snapTick=" << snap.tick << " snapPlayers=" << snap.players.size()
              << " serverSawCommand=" << (serverSawCommand ? "yes" : "no")
              << " full/delta(rx)=" << client.FullSnapshotsReceived()
              << '/' << client.DeltaSnapshotsReceived()
              << " size[full/delta]=" << client.LastFullSnapshotBytes()
              << '/' << client.LastDeltaSnapshotBytes()
              << " ignored=" << client.IgnoredSnapshots()
              << " resync=" << client.ResyncRequestsSent()
              << " (clientRx=" << client.PacketsReceived() << ",clientTx=" << client.PacketsSent()
              << ",serverRx=" << server.PacketsReceived() << ",serverTx=" << server.PacketsSent() << ")\n";

    // Bad connect: a port with no server must fail gracefully (no crash). Use a
    // short timeout so the smoke stays fast.
    ClientTransport deadClient;
    const std::uint16_t deadPort = 1; // nothing serves UDP on loopback:1
    const bool deadConnected = deadClient.Connect("127.0.0.1", deadPort, "", 0.4f);
    const bool badConnectHandled = !deadConnected && deadClient.TimedOut();
    deadClient.Close();
    std::cout << "localhost-net-smoke: badConnect(port=" << deadPort << ") connected="
              << (deadConnected ? "yes" : "no")
              << " timedOut=" << (deadClient.TimedOut() ? "yes" : "no")
              << " handled=" << (badConnectHandled ? "ok" : "FAIL") << '\n';

    client.Disconnect();
    server.Close();

    const bool ok = connected && snapshotValid && serverSawCommand && badConnectHandled;
    std::cout << (ok ? "LOCALHOST_NET_SMOKE_OK" : "LOCALHOST_NET_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 8;
}

#else // DAIBED_HAVE_NETWORK

// ----- Stubs when the build excluded the socket transport -------------------
#include <string>

namespace
{
const std::string kDisabled = "network transport disabled at build (DAIBED_ENABLE_NETWORK=OFF)";
}

bool NetworkTransportAvailable() { return false; }

struct ServerTransport::Impl { std::string lastError = kDisabled; };
ServerTransport::ServerTransport() : impl_(std::make_unique<Impl>()) {}
ServerTransport::~ServerTransport() = default;
bool ServerTransport::Start(const ServerConfig&) { return false; }
bool ServerTransport::IsOpen() const { return false; }
void ServerTransport::Close() {}
const std::string& ServerTransport::LastError() const { return impl_->lastError; }
void ServerTransport::Poll() {}
std::vector<int> ServerTransport::TakePendingClients() { return {}; }
void ServerTransport::AssignPlayer(int, int) {}
std::vector<DisconnectedClient> ServerTransport::TakeDisconnectedClients() { return {}; }
std::vector<ReconnectedClient> ServerTransport::TakeReconnectedClients() { return {}; }
void ServerTransport::SetMatchJoinLocked(bool) {}
std::vector<LobbyPlayerState> ServerTransport::LobbyPlayers() const { return {}; }
LobbyValidation ServerTransport::ValidateLobbyStart() const { return LobbyValidation { false, kDisabled }; }
bool ServerTransport::LobbyStartRequested() const { return false; }
LobbySnapshot ServerTransport::BuildLobbySnapshot(bool, bool, const std::string& statusMessage) const
{
    LobbySnapshot snapshot;
    snapshot.statusMessage = statusMessage.empty() ? kDisabled : statusMessage;
    return snapshot;
}
void ServerTransport::BroadcastLobbySnapshot(const LobbySnapshot&) {}
void ServerTransport::SendLobbySnapshotToClient(int, const LobbySnapshot&) {}
void ServerTransport::BroadcastSnapshot(const MatchSnapshot&) {}
void ServerTransport::SendSnapshotToClient(int, const MatchSnapshot&) {}
std::vector<ReceivedCommand> ServerTransport::DrainCommands() { return {}; }
std::size_t ServerTransport::ClientCount() const { return 0; }
std::uint16_t ServerTransport::BoundPort() const { return 0; }
std::uint32_t ServerTransport::PacketsReceived() const { return 0; }
std::uint32_t ServerTransport::PacketsSent() const { return 0; }
std::uint64_t ServerTransport::BytesReceived() const { return 0; }
std::uint64_t ServerTransport::BytesSent() const { return 0; }
std::uint32_t ServerTransport::StaleCommandsDropped() const { return 0; }
std::uint32_t ServerTransport::FullSnapshotsSent() const { return 0; }
std::uint32_t ServerTransport::DeltaSnapshotsSent() const { return 0; }
std::uint32_t ServerTransport::ResyncRequestsReceived() const { return 0; }
std::size_t ServerTransport::LastFullSnapshotBytes() const { return 0; }
std::size_t ServerTransport::LastDeltaSnapshotBytes() const { return 0; }
std::uint32_t ServerTransport::LastProcessedCommandTick(int) const { return 0; }
int ServerTransport::PlayerForClient(int) const { return -1; }
std::vector<int> ServerTransport::ConnectedClients() const { return {}; }

struct ClientTransport::Impl
{
    std::string lastError = kDisabled;
    std::string denyReason;
    MatchSnapshot empty;
    LobbySnapshot emptyLobby;
};
ClientTransport::ClientTransport() : impl_(std::make_unique<Impl>()) {}
ClientTransport::~ClientTransport() = default;
bool ClientTransport::Open(const std::string&, std::uint16_t, const std::string&, float) { return false; }
bool ClientTransport::Connect(const std::string&, std::uint16_t, const std::string&, float) { return false; }
bool ClientTransport::IsOpen() const { return false; }
void ClientTransport::Close() {}
const std::string& ClientTransport::LastError() const { return impl_->lastError; }
void ClientTransport::Poll() {}
void ClientTransport::SendCommand(const PlayerCommand&) {}
void ClientTransport::SendLobbyUpdate(const LobbyUpdate&) {}
void ClientTransport::Disconnect() {}
bool ClientTransport::IsConnected() const { return false; }
bool ClientTransport::TimedOut() const { return false; }
bool ClientTransport::WasDenied() const { return false; }
const std::string& ClientTransport::DenyReason() const { return impl_->denyReason; }
int ClientTransport::LobbyClientId() const { return -1; }
int ClientTransport::AssignedPlayerId() const { return -1; }
bool ClientTransport::InMatch() const { return false; }
bool ClientTransport::HasLobbySnapshot() const { return false; }
const LobbySnapshot& ClientTransport::LatestLobbySnapshot() const { return impl_->emptyLobby; }
bool ClientTransport::HasSnapshot() const { return false; }
const MatchSnapshot& ClientTransport::LatestSnapshot() const { return impl_->empty; }
std::uint32_t ClientTransport::PacketsReceived() const { return 0; }
std::uint32_t ClientTransport::PacketsSent() const { return 0; }
std::uint64_t ClientTransport::BytesReceived() const { return 0; }
std::uint64_t ClientTransport::BytesSent() const { return 0; }
float ClientTransport::BytesPerSecond() const { return 0.0f; }
float ClientTransport::PacketsPerSecond() const { return 0.0f; }
std::uint32_t ClientTransport::DroppedSnapshots() const { return 0; }
std::uint32_t ClientTransport::IgnoredSnapshots() const { return 0; }
std::uint32_t ClientTransport::FullSnapshotsReceived() const { return 0; }
std::uint32_t ClientTransport::DeltaSnapshotsReceived() const { return 0; }
std::uint32_t ClientTransport::ResyncRequestsSent() const { return 0; }
std::size_t ClientTransport::LastFullSnapshotBytes() const { return 0; }
std::size_t ClientTransport::LastDeltaSnapshotBytes() const { return 0; }
std::uint32_t ClientTransport::LastAckedCommandTick() const { return 0; }
std::size_t ClientTransport::PendingCommandCount() const { return 0; }
float ClientTransport::SnapshotAgeSeconds() const { return 0.0f; }

int RunLocalhostNetSmoke(const ServerConfig&)
{
    std::cout << "localhost-net-smoke: " << kDisabled << " — skipped\n";
    std::cout << "LOCALHOST_NET_SMOKE_SKIPPED" << std::endl;
    return 0;
}

#endif // DAIBED_HAVE_NETWORK

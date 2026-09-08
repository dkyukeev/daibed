#include "Network/NetworkTransport.h"
#include "Network/NetworkProtocol.h"
#include "Network/DatagramBackend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <thread>
#include <utility>

// Real UDP transport. The whole socket implementation is gated behind
// DAIBED_HAVE_NETWORK (CMake option DAIBED_ENABLE_NETWORK). When the option is
// off, every entry point compiles to a safe stub so the rest of the game builds
// and links unchanged. See docs/P2P_IMPLEMENTATION.md.

#if defined(DAIBED_HAVE_NETWORK) && DAIBED_HAVE_NETWORK
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
// --- Transport-level packet fragmentation (audit finding: >MTU datagrams) ---
// Any encoded packet larger than kMaxDatagramBytes (full snapshot baselines,
// resyncs, and large deltas) is split into PacketFragment datagrams of
// kFragmentChunkBytes each instead of one huge UDP datagram: a single >1500 B
// datagram IP-fragments (loss of ANY fragment loses the whole packet, and some
// middleboxes drop IP fragments outright), and >64 KB would not send at all.
// 1200 B is the QUIC-style safe-MTU value; the fixed chunk layout keeps
// reassembly idempotent across reliable retries (fragmentId = the original
// packet's sequence, stable per retry).
constexpr std::size_t kMaxDatagramBytes = 1200;
constexpr std::size_t kFragmentChunkBytes = 1024;
constexpr std::size_t kMaxAssembledPacketBytes = 512 * 1024;
constexpr std::size_t kMaxSnapshotPacketBytes = kMaxAssembledPacketBytes;
constexpr std::size_t kMaxFragmentAssemblies = 8;
constexpr double kFragmentAssemblyTimeoutSeconds = 3.0;
constexpr int kReconnectReservationSeconds = 120;

// --- Test-only datagram loss injection (network debugging) ------------------
// DAIBED_NET_DROP_PCT=<0..95> makes every endpoint drop that percentage of
// OUTGOING datagrams before the socket (counters still tick, emulating wire
// loss). Deterministic seed so a failing run can be replayed. Zero overhead
// when the variable is unset.
int NetDropPercent()
{
    static const int value = []
    {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: fine for a test-only debug knob
#endif
        const char* env = std::getenv("DAIBED_NET_DROP_PCT");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        if (env == nullptr)
        {
            return 0;
        }
        const long parsed = std::strtol(env, nullptr, 10);
        return static_cast<int>(std::clamp(parsed, 0L, 95L));
    }();
    return value;
}

bool DropDatagramForTesting()
{
    const int pct = NetDropPercent();
    if (pct <= 0)
    {
        return false;
    }
    static std::mt19937 rng { 0xDA1BEDu };
    return static_cast<int>(rng() % 100u) < pct;
}

std::mt19937_64 MakeCredentialRng()
{
    std::random_device source;
    std::seed_seq seed {
        source(), source(), source(), source(), source(), source(), source(), source()
    };
    return std::mt19937_64(seed);
}

template <typename Identity>
Identity GenerateIdentity(std::mt19937_64& rng)
{
    Identity identity;
    do
    {
        identity.high = rng();
        identity.low = rng();
    }
    while (!identity.IsValid());
    return identity;
}

bool MatchesReservation(const SessionCredentials& presented,
                        const SessionCredentials& reserved)
{
    return presented.CanReconnect()
        && presented.sessionId == reserved.sessionId
        && presented.playerSessionId == reserved.playerSessionId
        && presented.reconnectToken == reserved.reconnectToken;
}

bool AuthenticatedIdentityMatches(AuthenticatedPeerIdentity expected,
                                  AuthenticatedPeerIdentity presented)
{
    // UDP has no provider identity and continues to rely on the rotating
    // reconnect bearer. Once a session has an authenticated provider identity,
    // it may never downgrade to an unauthenticated or different peer.
    return !expected.IsValid() || expected == presented;
}

// Reassembles PacketFragment datagrams back into the original oversized packet.
// Keyed by (endpoint, fragmentId); duplicate chunks (reliable retries) merge
// idempotently. Stale or conflicting assemblies are dropped defensively.
struct FragmentAssembler
{
    struct Entry
    {
        std::uint64_t endpointKey = 0;
        std::uint32_t fragmentId = 0;
        std::uint32_t totalSize = 0;
        std::uint16_t count = 0;
        std::size_t receivedChunks = 0;
        std::vector<std::uint8_t> data;
        std::vector<bool> have;
        Clock::time_point started;
    };
    std::vector<Entry> entries;

    // Returns true and fills `out` when this chunk completed a packet.
    bool Accept(std::uint64_t endpointKey, std::uint32_t fragmentId,
                std::uint16_t index, std::uint16_t count, std::uint32_t totalSize,
                const std::vector<std::uint8_t>& chunk, std::vector<std::uint8_t>& out)
    {
        if (count == 0 || index >= count || totalSize == 0
            || totalSize > kMaxAssembledPacketBytes
            || static_cast<std::size_t>(count) * kFragmentChunkBytes < totalSize)
        {
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(index) * kFragmentChunkBytes;
        const std::size_t expected = (index + 1u == count)
            ? static_cast<std::size_t>(totalSize) - offset
            : kFragmentChunkBytes;
        if (offset >= totalSize || chunk.size() != expected)
        {
            return false;
        }

        const Clock::time_point now = Clock::now();
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                [now](const Entry& entry)
                {
                    return std::chrono::duration<double>(now - entry.started).count()
                        > kFragmentAssemblyTimeoutSeconds;
                }),
            entries.end());

        auto found = std::find_if(entries.begin(), entries.end(),
            [endpointKey, fragmentId](const Entry& entry)
            {
                return entry.endpointKey == endpointKey && entry.fragmentId == fragmentId;
            });
        if (found == entries.end())
        {
            if (entries.size() >= kMaxFragmentAssemblies)
            {
                entries.erase(entries.begin());
            }
            Entry entry;
            entry.endpointKey = endpointKey;
            entry.fragmentId = fragmentId;
            entry.totalSize = totalSize;
            entry.count = count;
            entry.data.resize(totalSize);
            entry.have.assign(count, false);
            entry.started = now;
            entries.push_back(std::move(entry));
            found = entries.end() - 1;
        }
        else if (found->count != count || found->totalSize != totalSize)
        {
            return false; // conflicting metadata for the same id — ignore
        }

        if (!found->have[index])
        {
            std::copy(chunk.begin(), chunk.end(),
                      found->data.begin() + static_cast<std::ptrdiff_t>(offset));
            found->have[index] = true;
            ++found->receivedChunks;
        }
        if (found->receivedChunks == found->count)
        {
            out = std::move(found->data);
            entries.erase(found);
            return true;
        }
        return false;
    }
};

constexpr std::size_t kCommandBackupCount = 3;
constexpr std::size_t kMaxSentSnapshotHistory = 32;
constexpr std::size_t kMaxClientSnapshotHistory = 32;

double SecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

int PositiveOrDefault(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

std::string SanitizeLobbyName(std::string value, int clientId)
{
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch)
    {
        return ch < 0x20 || ch == 0x7F;
    }), value.end());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.pop_back();
    if (value.empty())
    {
        value = "Игрок";
    }
    // Display names are labels only. The stable session/reconnect credentials
    // carry identity, while this suffix makes duplicate chosen names harmless.
    const std::string suffix = " #" + std::to_string(clientId);
    const std::size_t maxBaseBytes = 64u > suffix.size() ? 64u - suffix.size() : 1u;
    if (value.size() > maxBaseBytes)
    {
        value.resize(maxBaseBytes);
        std::size_t lead = value.size() - 1;
        while (lead > 0
            && (static_cast<unsigned char>(value[lead]) & 0xC0u) == 0x80u)
        {
            --lead;
        }
        const unsigned char leadByte = static_cast<unsigned char>(value[lead]);
        const std::size_t sequenceBytes = (leadByte & 0x80u) == 0 ? 1u
            : ((leadByte & 0xE0u) == 0xC0u ? 2u
            : ((leadByte & 0xF0u) == 0xE0u ? 3u
            : ((leadByte & 0xF8u) == 0xF0u ? 4u : 1u)));
        if (lead + sequenceBytes > value.size())
        {
            value.resize(lead);
        }
    }
    return value + suffix;
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
        SessionCredentials credentials;
        AuthenticatedPeerIdentity authenticatedIdentity;
        std::uint32_t lastProcessedCommandTick = 0;
        LobbyUpdate lobby;
        DatagramPeer peer;
        Clock::time_point lastSeen;
        Clock::time_point lastSnapshotSent;
        MatchSnapshot snapshotBaseline;
        bool hasSnapshotBaseline = false;
        bool needsFullSnapshot = true;
        bool fullSnapshotAcked = false;
        std::uint32_t snapshotBaselineSequence = 0;
        std::uint32_t pendingFullSnapshotSequence = 0;
        struct SentSnapshot
        {
            std::uint32_t sequence = 0;
            MatchSnapshot snapshot;
        };
        std::vector<SentSnapshot> sentSnapshotHistory;
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
        SessionCredentials credentials;
        AuthenticatedPeerIdentity authenticatedIdentity;
        Clock::time_point expiresAt;
    };

    NetworkBackend backendKind = NetworkBackend::SystemUdp;
    std::unique_ptr<IDatagramBackend> backend =
        CreateDatagramBackend(NetworkBackend::SystemUdp);
    std::string lastError;
    ServerConfig config;
    SessionId sessionId;
    std::mt19937_64 credentialRng = MakeCredentialRng();
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
    FragmentAssembler fragments;

    void SendTo(DatagramPeer peer, const std::vector<std::uint8_t>& bytes)
    {
        if (bytes.size() > kMaxDatagramBytes)
        {
            SendFragmented(peer, bytes);
            return;
        }
        if (!DropDatagramForTesting())
        {
            backend->Send(peer, bytes.data(), bytes.size());
        }
        ++packetsSent;
        bytesSent += bytes.size();
    }

    void SendFragmented(DatagramPeer peer, const std::vector<std::uint8_t>& bytes)
    {
        PacketHeader original;
        if (DecodeHeader(bytes.data(), bytes.size(), original) != DecodeStatus::Ok)
        {
            return; // never happens for our own encoders
        }
        const std::size_t total = bytes.size();
        const std::uint16_t count = static_cast<std::uint16_t>(
            (total + kFragmentChunkBytes - 1) / kFragmentChunkBytes);
        for (std::uint16_t i = 0; i < count; ++i)
        {
            const std::size_t offset = static_cast<std::size_t>(i) * kFragmentChunkBytes;
            const std::size_t len = total - offset < kFragmentChunkBytes
                ? total - offset
                : kFragmentChunkBytes;
            SendTo(peer, EncodePacketFragment(
                sequence++, original.sequence, i, count,
                static_cast<std::uint32_t>(total), bytes.data() + offset, len));
        }
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
        SendTo(client.peer, bytes);
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
        if (ackedType == MessageType::SnapshotDelta)
        {
            const auto found = std::find_if(
                client.sentSnapshotHistory.begin(),
                client.sentSnapshotHistory.end(),
                [ackedSequence](const ClientChannel::SentSnapshot& sent)
                {
                    return sent.sequence == ackedSequence;
                });
            if (found != client.sentSnapshotHistory.end()
                && ackedSequence > client.snapshotBaselineSequence)
            {
                client.snapshotBaseline = found->snapshot;
                client.snapshotBaselineSequence = ackedSequence;
                client.hasSnapshotBaseline = true;
                client.fullSnapshotAcked = true;
                client.sentSnapshotHistory.erase(
                    std::remove_if(
                        client.sentSnapshotHistory.begin(),
                        client.sentSnapshotHistory.end(),
                        [ackedSequence](const ClientChannel::SentSnapshot& sent)
                        {
                            return sent.sequence <= ackedSequence;
                        }),
                    client.sentSnapshotHistory.end());
            }
        }
    }

    void QueueDecodedCommand(ClientChannel& client, PlayerCommand command)
    {
        if (command.tick <= client.lastProcessedCommandTick)
        {
            ++staleCommandsDropped;
            client.lastSeen = Clock::now();
            return;
        }
        // Authoritative mapping: stamp to the client's own player.
        command.controlledPlayerId = static_cast<std::uint32_t>(client.playerId);
        client.lastProcessedCommandTick = command.tick;
        commands.push_back(ReceivedCommand { client.clientId, command });
        client.lastSeen = Clock::now();
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
                SendTo(client.peer, packet.bytes);
                packet.lastSent = now;
                ++packet.attempts;
                ++i;
            }
        }
    }

    ClientChannel* FindByEndpoint(DatagramPeer peer)
    {
        for (ClientChannel& c : clients)
        {
            if (c.peer == peer)
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

    ClientChannel* FindByCredentials(const SessionCredentials& credentials,
                                     AuthenticatedPeerIdentity identity)
    {
        if (!credentials.CanReconnect())
        {
            return nullptr;
        }
        const auto found = std::find_if(
            clients.begin(), clients.end(),
            [&credentials, identity](const ClientChannel& channel)
            {
                return MatchesReservation(credentials, channel.credentials)
                    && AuthenticatedIdentityMatches(channel.authenticatedIdentity, identity);
            });
        return found != clients.end() ? &*found : nullptr;
    }

    bool IsHost(int clientId) const
    {
        return clientId >= 0 && clientId == hostClientId;
    }

    void ReserveSlotForReconnect(const ClientChannel& client)
    {
        if (!matchJoinLocked || client.playerId < 0 || !client.credentials.CanReconnect())
        {
            return;
        }
        reservations.erase(
            std::remove_if(reservations.begin(), reservations.end(),
                [&client](const SlotReservation& reservation)
                {
                    return reservation.playerId == client.playerId
                        || reservation.credentials.playerSessionId
                            == client.credentials.playerSessionId;
                }),
            reservations.end());

        SlotReservation reservation;
        reservation.playerId = client.playerId;
        reservation.playerName = client.lobby.playerName;
        reservation.lobby = client.lobby;
        reservation.credentials = client.credentials;
        reservation.authenticatedIdentity = client.authenticatedIdentity;
        reservation.expiresAt = Clock::now()
            + std::chrono::seconds(kReconnectReservationSeconds);
        reservation.lobby.ready = true;
        reservation.lobby.startRequested = false;
        reservations.push_back(reservation);
    }

    void NoteDisconnected(const ClientChannel& client)
    {
        disconnected.push_back(DisconnectedClient { client.clientId, client.playerId });
        ReserveSlotForReconnect(client);
    }

    void RemoveExpiredReservations()
    {
        const Clock::time_point now = Clock::now();
        reservations.erase(
            std::remove_if(reservations.begin(), reservations.end(),
                [now](const SlotReservation& reservation)
                {
                    return reservation.expiresAt <= now;
                }),
            reservations.end());
    }

    SlotReservation* FindReservation(const SessionCredentials& credentials,
                                     AuthenticatedPeerIdentity identity)
    {
        const auto found = std::find_if(
            reservations.begin(), reservations.end(),
            [&credentials, identity](const SlotReservation& reservation)
            {
                return MatchesReservation(credentials, reservation.credentials)
                    && AuthenticatedIdentityMatches(reservation.authenticatedIdentity, identity);
            });
        return found != reservations.end() ? &*found : nullptr;
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
    if (impl_->backendKind != config.networkBackend)
    {
        impl_->backend = CreateDatagramBackend(config.networkBackend);
        impl_->backendKind = config.networkBackend;
    }
    if (!impl_->backend->Listen(config.listenAddress, config.port))
    {
        impl_->lastError = impl_->backend->LastError();
        return false;
    }
    impl_->boundPort = impl_->backend->BoundPort();
    impl_->sessionId = GenerateIdentity<SessionId>(impl_->credentialRng);
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

bool ServerTransport::IsOpen() const { return impl_->backend->IsOpen(); }

void ServerTransport::Close()
{
    impl_->backend->Close();
    impl_->clients.clear();
    impl_->reservations.clear();
    impl_->commands.clear();
    impl_->disconnected.clear();
    impl_->reconnected.clear();
    impl_->boundPort = 0;
    impl_->sessionId = {};
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
}

const std::string& ServerTransport::LastError() const { return impl_->lastError; }

void ServerTransport::Poll()
{
    if (!impl_->backend->IsOpen())
    {
        return;
    }
    std::vector<std::uint8_t> buffer(65536);
    for (;;)
    {
        DatagramPeer from;
        std::size_t n = 0;
        const DatagramReceiveResult receive =
            impl_->backend->Receive(from, buffer.data(), buffer.size(), n);
        if (receive != DatagramReceiveResult::Received)
        {
            if (receive == DatagramReceiveResult::Error)
            {
                impl_->lastError = impl_->backend->LastError();
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

        // Oversized-packet reassembly: a completed fragment set replaces the
        // datagram in `buffer` and re-enters the normal dispatch below as if
        // the original packet had arrived whole.
        if (header.type == MessageType::PacketFragment)
        {
            PacketHeader fragHeader;
            std::uint32_t fragmentId = 0;
            std::uint16_t fragmentIndex = 0;
            std::uint16_t fragmentCount = 0;
            std::uint32_t totalSize = 0;
            std::vector<std::uint8_t> chunk;
            if (DecodePacketFragment(buffer.data(), static_cast<std::size_t>(n), fragHeader,
                                     fragmentId, fragmentIndex, fragmentCount, totalSize, chunk)
                != DecodeStatus::Ok)
            {
                continue;
            }
            std::vector<std::uint8_t> assembled;
            if (!impl_->fragments.Accept(from.value, fragmentId, fragmentIndex,
                                         fragmentCount, totalSize, chunk, assembled))
            {
                continue; // incomplete (or rejected) — wait for more chunks
            }
            if (assembled.size() > buffer.size())
            {
                buffer.resize(assembled.size());
            }
            std::copy(assembled.begin(), assembled.end(), buffer.begin());
            n = assembled.size();
            if (DecodeHeader(buffer.data(), static_cast<std::size_t>(n), header) != DecodeStatus::Ok)
            {
                continue;
            }
        }

        const AuthenticatedPeerIdentity incomingIdentity =
            impl_->backend->AuthenticatedIdentity(from);
        Impl::ClientChannel* client = impl_->FindByEndpoint(from);
        const bool endpointIdentityMismatch = client != nullptr
            && !AuthenticatedIdentityMatches(client->authenticatedIdentity, incomingIdentity);
        if (endpointIdentityMismatch)
        {
            client = nullptr;
        }
        switch (header.type)
        {
        case MessageType::Connect:
        {
            PacketHeader cHeader;
            ConnectRequest request;
            if (DecodeConnect(buffer.data(), static_cast<std::size_t>(n), cHeader, request)
                != DecodeStatus::Ok)
            {
                break; // malformed connect — ignore
            }
            // Password / token check (task 2). Empty server password accepts any.
            if (!impl_->config.ValidatePassword(request.password))
            {
                impl_->SendTo(from, EncodeConnectDenied(impl_->sequence++, "bad password"));
                break; // never register a denied client
            }
            if (endpointIdentityMismatch)
            {
                impl_->SendTo(from, EncodeConnectDenied(
                    impl_->sequence++, "authenticated peer identity changed"));
                break;
            }
            if (client == nullptr)
            {
                // A NAT rebinding or provider reconnect can change the opaque
                // endpoint before the old channel times out. Possession of the
                // current bearer credentials authorizes moving that live
                // channel; rotate the token so the presented value is one-use.
                client = impl_->FindByCredentials(request.resume, incomingIdentity);
                if (client != nullptr)
                {
                    client->peer = from;
                    client->lastSeen = Clock::now();
                    client->credentials.reconnectToken =
                        GenerateIdentity<ReconnectToken>(impl_->credentialRng);
                }
            }
            if (client == nullptr)
            {
                impl_->RemoveExpiredReservations();
                Impl::SlotReservation* reservation = impl_->matchJoinLocked
                    ? impl_->FindReservation(request.resume, incomingIdentity)
                    : nullptr;
                if (impl_->matchJoinLocked && reservation == nullptr)
                {
                    const char* reason = request.resume.CanReconnect()
                        ? "invalid reconnect credentials"
                        : "match already started";
                    impl_->SendTo(from, EncodeConnectDenied(impl_->sequence++, reason));
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
                channel.ackSent = true;
                channel.peer = from;
                channel.authenticatedIdentity = incomingIdentity;
                channel.lastSeen = Clock::now();
                channel.lastSnapshotSent = Clock::now() - std::chrono::seconds(1);
                channel.credentials.sessionId = impl_->sessionId;
                channel.credentials.reconnectToken =
                    GenerateIdentity<ReconnectToken>(impl_->credentialRng);

                const bool reconnecting = reservation != nullptr;
                if (reconnecting)
                {
                    const Impl::SlotReservation restored = *reservation;
                    channel.playerId = restored.playerId;
                    channel.lobby = restored.lobby;
                    channel.lobby.ready = true;
                    channel.lobby.startRequested = false;
                    channel.credentials.playerSessionId =
                        restored.credentials.playerSessionId;
                    impl_->reservations.erase(
                        std::remove_if(
                            impl_->reservations.begin(), impl_->reservations.end(),
                            [&restored](const Impl::SlotReservation& entry)
                            {
                                return entry.credentials.playerSessionId
                                    == restored.credentials.playerSessionId;
                            }),
                        impl_->reservations.end());
                }
                else
                {
                    channel.playerId = -1;
                    channel.lobby = DefaultLobbyUpdateForClient(
                        channel.clientId, impl_->config);
                    channel.credentials.playerSessionId =
                        GenerateIdentity<PlayerSessionId>(impl_->credentialRng);
                }
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
                    EncodeConnectAck(
                        seq,
                        ConnectAccept { accepted->clientId, accepted->credentials }),
                    true);
                ++impl_->lobbyRevision;
                if (reconnecting)
                {
                    impl_->reconnected.push_back(
                        ReconnectedClient { accepted->clientId, accepted->playerId });
                    const std::uint32_t lobbySeq = impl_->sequence++;
                    impl_->QueueReliable(
                        *accepted,
                        MessageType::LobbySnapshot,
                        lobbySeq,
                        EncodeLobbySnapshot(
                            lobbySeq,
                            BuildLobbySnapshot(false, true, "reconnected")),
                        true);
                }
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
                    EncodeConnectAck(
                        seq,
                        ConnectAccept { client->clientId, client->credentials }),
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
                    client->sentSnapshotHistory.clear();
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
                    if (update.startRequested && !impl_->IsHost(client->clientId))
                    {
                        update.startRequested = false;
                        impl_->lobbyStatusOverride = "start denied for non-host";
                    }
                    else if (impl_->IsHost(client->clientId))
                    {
                        impl_->lobbyStatusOverride.clear();
                    }
                    const LobbyUpdate before = client->lobby;
                    ApplyLobbyUpdate(client->lobby, update, impl_->config, client->clientId);
                    if (!impl_->IsHost(client->clientId))
                    {
                        client->lobby.startRequested = false;
                    }
                    client->lastSeen = Clock::now();
                    // Clients re-send their lobby intent periodically for loss
                    // resilience; only a real change bumps the revision.
                    const bool changed = before.playerName != client->lobby.playerName
                        || before.selectedTeam != client->lobby.selectedTeam
                        || before.selectedHero != client->lobby.selectedHero
                        || before.ready != client->lobby.ready
                        || before.startRequested != client->lobby.startRequested;
                    if (changed)
                    {
                        ++impl_->lobbyRevision;
                    }
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
                    impl_->QueueDecodedCommand(*client, command);
                }
            }
            break;
        }
        case MessageType::PlayerCommandBatch:
        {
            // Only assigned clients may drive a player.
            if (client != nullptr && client->ackSent && client->playerId >= 0)
            {
                PacketHeader batchHeader;
                std::vector<PlayerCommand> commands;
                if (DecodePlayerCommandBatch(buffer.data(), static_cast<std::size_t>(n),
                                             batchHeader, commands) == DecodeStatus::Ok)
                {
                    for (const PlayerCommand& command : commands)
                    {
                        impl_->QueueDecodedCommand(*client, command);
                    }
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
    client->sentSnapshotHistory.clear();
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
    if (!impl_->backend->IsOpen())
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
    if (!impl_->backend->IsOpen())
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
    if (!impl_->backend->IsOpen())
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
            client->sentSnapshotHistory.clear();
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
            client->sentSnapshotHistory.clear();
            client->lastSnapshotSent = now;
            ++impl_->fullSnapshotsSent;
            impl_->lastFullSnapshotBytes = fullBytes.size();
            return;
        }
        impl_->SendTo(client->peer, bytes);
        client->sentSnapshotHistory.push_back(
            Impl::ClientChannel::SentSnapshot { seq, perClientSnapshot });
        while (client->sentSnapshotHistory.size() > kMaxSentSnapshotHistory)
        {
            client->sentSnapshotHistory.erase(client->sentSnapshotHistory.begin());
        }
        client->lastSnapshotSent = now;
        ++impl_->deltaSnapshotsSent;
        impl_->lastDeltaSnapshotBytes = bytes.size();
    }
}

void ServerTransport::BroadcastSnapshot(const MatchSnapshot& snapshot)
{
    if (!impl_->backend->IsOpen())
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

bool ServerTransport::NeedsSnapshotForClient(int clientId) const
{
    if (!impl_->backend->IsOpen())
    {
        return false;
    }
    const Impl::ClientChannel* client = impl_->FindById(clientId);
    if (client == nullptr || client->playerId < 0)
    {
        return false;
    }
    if (!client->hasSnapshotBaseline || client->needsFullSnapshot)
    {
        return true;
    }
    if (!client->fullSnapshotAcked)
    {
        return false;
    }
    return SecondsSince(client->lastSnapshotSent) >= kSnapshotSendIntervalSeconds;
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

void ServerTransport::AdvanceProcessedCommandTick(int clientId, std::uint32_t tick)
{
    Impl::ClientChannel* client = impl_->FindById(clientId);
    if (client != nullptr && tick > client->lastProcessedCommandTick)
    {
        client->lastProcessedCommandTick = tick;
    }
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
    explicit Impl(NetworkBackend selectedBackend)
        : backendKind(selectedBackend), backend(CreateDatagramBackend(selectedBackend))
    {
    }

    NetworkBackend backendKind = NetworkBackend::SystemUdp;
    std::unique_ptr<IDatagramBackend> backend;
    std::string lastError;
    DatagramPeer serverPeer;
    ConnectRequest connectRequest;
    SessionCredentials credentials;
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
    // Last lobby intent, re-sent periodically until the match starts (see
    // SendLobbyUpdate for why plain one-shot sends were not enough).
    LobbyUpdate lastLobbyUpdate {};
    bool hasLobbyUpdate = false;
    Clock::time_point lastLobbyUpdateSent;
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
    struct SnapshotHistoryEntry
    {
        std::uint32_t sequence = 0;
        MatchSnapshot snapshot;
    };
    std::vector<SnapshotHistoryEntry> snapshotHistory;
    FragmentAssembler fragments;
    std::vector<PlayerCommand> pendingCommands;
    MatchSnapshot latestSnapshot;
    LobbySnapshot latestLobbySnapshot;

    void RefreshMatchAssignment()
    {
        if (!hasLobbySnapshot || !latestLobbySnapshot.matchStarted || lobbyClientId < 0)
        {
            return;
        }
        for (const LobbyPlayerState& player : latestLobbySnapshot.players)
        {
            if (player.clientId == lobbyClientId && player.assignedPlayerId >= 0)
            {
                assignedPlayerId = player.assignedPlayerId;
                inMatch = true;
                return;
            }
        }
    }

    void SendBytes(const std::vector<std::uint8_t>& bytes)
    {
        if (!backend->IsOpen())
        {
            return;
        }
        if (bytes.size() > kMaxDatagramBytes)
        {
            PacketHeader original;
            if (DecodeHeader(bytes.data(), bytes.size(), original) != DecodeStatus::Ok)
            {
                return;
            }
            const std::size_t total = bytes.size();
            const std::uint16_t count = static_cast<std::uint16_t>(
                (total + kFragmentChunkBytes - 1) / kFragmentChunkBytes);
            for (std::uint16_t i = 0; i < count; ++i)
            {
                const std::size_t offset = static_cast<std::size_t>(i) * kFragmentChunkBytes;
                const std::size_t len = total - offset < kFragmentChunkBytes
                    ? total - offset
                    : kFragmentChunkBytes;
                SendBytes(EncodePacketFragment(
                    sequence++, original.sequence, i, count,
                    static_cast<std::uint32_t>(total), bytes.data() + offset, len));
            }
            return;
        }
        if (!DropDatagramForTesting())
        {
            backend->Send(serverPeer, bytes.data(), bytes.size());
        }
        ++packetsSent;
        bytesSent += bytes.size();
    }

    void NoteReceived(std::size_t byteCount)
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

    void RememberSnapshot(std::uint32_t snapshotSequence, const MatchSnapshot& snapshot)
    {
        const auto existing = std::find_if(
            snapshotHistory.begin(),
            snapshotHistory.end(),
            [snapshotSequence](const SnapshotHistoryEntry& entry)
            {
                return entry.sequence == snapshotSequence;
            });
        if (existing != snapshotHistory.end())
        {
            existing->snapshot = snapshot;
        }
        else
        {
            snapshotHistory.push_back(SnapshotHistoryEntry { snapshotSequence, snapshot });
        }
        while (snapshotHistory.size() > kMaxClientSnapshotHistory)
        {
            snapshotHistory.erase(snapshotHistory.begin());
        }
    }

    const MatchSnapshot* FindSnapshotHistory(std::uint32_t snapshotSequence, std::uint32_t tick) const
    {
        const auto found = std::find_if(
            snapshotHistory.begin(),
            snapshotHistory.end(),
            [snapshotSequence, tick](const SnapshotHistoryEntry& entry)
            {
                return entry.sequence == snapshotSequence && entry.snapshot.tick == tick;
            });
        return found != snapshotHistory.end() ? &found->snapshot : nullptr;
    }

    void SendCommandWindow(std::size_t begin, std::size_t end)
    {
        if (begin >= end || begin >= pendingCommands.size())
        {
            return;
        }
        end = end < pendingCommands.size() ? end : pendingCommands.size();
        std::vector<PlayerCommand> commands(
            pendingCommands.begin() + static_cast<std::ptrdiff_t>(begin),
            pendingCommands.begin() + static_cast<std::ptrdiff_t>(end));
        if (commands.empty())
        {
            return;
        }
        if (commands.size() == 1)
        {
            SendBytes(EncodePlayerCommand(sequence++, commands.front()));
        }
        else
        {
            SendBytes(EncodePlayerCommandBatch(sequence++, commands));
        }
    }
};

ClientTransport::ClientTransport(NetworkBackend backend)
    : impl_(std::make_unique<Impl>(backend))
{
}
ClientTransport::~ClientTransport() { Close(); }

bool ClientTransport::Open(const std::string& host, std::uint16_t port, const std::string& token,
                           float timeoutSeconds,
                           const SessionCredentials& resumeCredentials)
{
    const SessionCredentials resume = resumeCredentials;
    Close();
    impl_->connectRequest.password = token;
    impl_->connectRequest.resume = resume;
    impl_->credentials = resume;
    if (!impl_->backend->OpenClient(host, port, impl_->serverPeer))
    {
        impl_->lastError = impl_->backend->LastError();
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
    impl_->snapshotHistory.clear();
    impl_->pendingCommands.clear();
    impl_->hasLobbyUpdate = false;

    // Say hello with the join token. Delivery is best-effort; Poll() re-sends it
    // until we get a ConnectAck or ConnectDenied. A dead server just never
    // replies and IsConnected() stays false.
    const std::vector<std::uint8_t> hello =
        EncodeConnect(impl_->sequence++, impl_->connectRequest);
    impl_->SendBytes(hello);
    impl_->lastError.clear();
    return true;
}

bool ClientTransport::Connect(const std::string& host, std::uint16_t port, const std::string& token,
                              float timeoutSeconds,
                              const SessionCredentials& resumeCredentials)
{
    if (!Open(host, port, token, timeoutSeconds, resumeCredentials))
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

bool ClientTransport::IsOpen() const { return impl_->backend->IsOpen(); }

void ClientTransport::Close()
{
    impl_->backend->Close();
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
    impl_->snapshotHistory.clear();
    impl_->pendingCommands.clear();
    impl_->hasLobbyUpdate = false;
    impl_->assignedPlayerId = -1;
    impl_->lobbyClientId = -1;
}

const std::string& ClientTransport::LastError() const { return impl_->lastError; }

void ClientTransport::Poll()
{
    if (!impl_->backend->IsOpen())
    {
        if (!impl_->connected && !impl_->denied)
        {
            impl_->timedOut = true;
            if (!impl_->backend->LastError().empty())
            {
                impl_->lastError = impl_->backend->LastError();
            }
        }
        return;
    }
    std::vector<std::uint8_t> buffer(65536);
    for (;;)
    {
        DatagramPeer from;
        std::size_t n = 0;
        const DatagramReceiveResult receive =
            impl_->backend->Receive(from, buffer.data(), buffer.size(), n);
        if (receive != DatagramReceiveResult::Received)
        {
            if (receive == DatagramReceiveResult::Error)
            {
                impl_->lastError = impl_->backend->LastError();
            }
            break;
        }
        if (from != impl_->serverPeer)
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

        // Oversized-packet reassembly (full snapshot baselines / resyncs): a
        // completed fragment set replaces the datagram in `buffer` and falls
        // through into the normal dispatch below.
        if (header.type == MessageType::PacketFragment)
        {
            PacketHeader fragHeader;
            std::uint32_t fragmentId = 0;
            std::uint16_t fragmentIndex = 0;
            std::uint16_t fragmentCount = 0;
            std::uint32_t totalSize = 0;
            std::vector<std::uint8_t> chunk;
            if (DecodePacketFragment(buffer.data(), static_cast<std::size_t>(n), fragHeader,
                                     fragmentId, fragmentIndex, fragmentCount, totalSize, chunk)
                != DecodeStatus::Ok)
            {
                continue;
            }
            std::vector<std::uint8_t> assembled;
            if (!impl_->fragments.Accept(0, fragmentId, fragmentIndex,
                                         fragmentCount, totalSize, chunk, assembled))
            {
                continue; // incomplete — wait for the remaining chunks
            }
            if (assembled.size() > buffer.size())
            {
                buffer.resize(assembled.size());
            }
            std::copy(assembled.begin(), assembled.end(), buffer.begin());
            n = assembled.size();
            if (DecodeHeader(buffer.data(), static_cast<std::size_t>(n), header) != DecodeStatus::Ok)
            {
                continue;
            }
        }
        switch (header.type)
        {
        case MessageType::ConnectAck:
        {
            PacketHeader ackHeader;
            ConnectAccept accept;
            if (DecodeConnectAck(buffer.data(), static_cast<std::size_t>(n), ackHeader, accept)
                == DecodeStatus::Ok)
            {
                impl_->SendAck(ackHeader);
                impl_->connected = true;
                impl_->timedOut = false;
                impl_->lobbyClientId = accept.clientId;
                impl_->credentials = accept.credentials;
                impl_->connectRequest.resume = accept.credentials;
                impl_->RefreshMatchAssignment();
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
                impl_->RefreshMatchAssignment();
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
                impl_->RememberSnapshot(impl_->lastSnapshotSequence, impl_->latestSnapshot);
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
                SnapshotDeltaApplyStatus applyStatus = ApplySnapshotDeltaIfCompatible(
                    impl_->hasSnapshot,
                    impl_->latestSnapshot,
                    impl_->lastSnapshotSequence,
                    snapHeader.sequence,
                    delta);
                if (applyStatus == SnapshotDeltaApplyStatus::OldSnapshot)
                {
                    impl_->SendAck(snapHeader);
                    ++impl_->ignoredSnapshots;
                    break;
                }
                if (applyStatus == SnapshotDeltaApplyStatus::BaselineMismatch
                    && snapHeader.sequence > impl_->lastSnapshotSequence)
                {
                    if (const MatchSnapshot* historical =
                            impl_->FindSnapshotHistory(delta.baselineSequence, delta.baselineTick))
                    {
                        MatchSnapshot recovered = *historical;
                        std::uint32_t recoveredSequence = delta.baselineSequence;
                        applyStatus = ApplySnapshotDeltaIfCompatible(
                            true,
                            recovered,
                            recoveredSequence,
                            snapHeader.sequence,
                            delta);
                        if (applyStatus == SnapshotDeltaApplyStatus::Applied)
                        {
                            impl_->latestSnapshot = std::move(recovered);
                            impl_->lastSnapshotSequence = recoveredSequence;
                        }
                    }
                }
                if (SnapshotDeltaStatusNeedsFullResync(applyStatus))
                {
                    ++impl_->droppedSnapshots;
                    impl_->RequestFullResync(true);
                    break;
                }
                impl_->SendAck(snapHeader);
                impl_->hasSnapshot = true;
                impl_->RememberSnapshot(impl_->lastSnapshotSequence, impl_->latestSnapshot);
                impl_->fullResyncRequested = false;
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
                impl_->snapshotHistory.clear();
                impl_->pendingCommands.clear();
                impl_->hasLobbyUpdate = false;
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
    if (!impl_->connected && !impl_->denied && !impl_->timedOut
        && SecondsSince(impl_->lastHello) > 0.15)
    {
        const std::vector<std::uint8_t> hello =
            EncodeConnect(impl_->sequence++, impl_->connectRequest);
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

    // Pre-match lobby intent rides best-effort datagrams; re-send the latest
    // state periodically so a lost ready/start can never strand the client in
    // the lobby (idempotent server-side; the server only bumps its lobby
    // revision when the applied state actually changed).
    if (impl_->connected && !impl_->inMatch && impl_->hasLobbyUpdate
        && SecondsSince(impl_->lastLobbyUpdateSent) > 0.4)
    {
        impl_->SendBytes(EncodeLobbyUpdate(impl_->sequence++, impl_->lastLobbyUpdate));
        impl_->lastLobbyUpdateSent = Clock::now();
    }

    if (impl_->connected && impl_->inMatch && !impl_->pendingCommands.empty()
        && SecondsSince(impl_->lastCommandReplay) > kCommandReplayIntervalSeconds)
    {
        for (std::size_t i = 0; i < impl_->pendingCommands.size(); i += kCommandBackupCount)
        {
            impl_->SendCommandWindow(i, i + kCommandBackupCount);
        }
        impl_->lastCommandReplay = Clock::now();
    }

    if (impl_->connected && SecondsSince(impl_->lastServerSeen) > impl_->timeoutSeconds)
    {
        impl_->connected = false;
        impl_->inMatch = false;
        impl_->timedOut = true;
    }
    else if (!impl_->connected && !impl_->denied && !impl_->timedOut
             && SecondsSince(impl_->openTime) > impl_->timeoutSeconds)
    {
        impl_->timedOut = true;
        if (impl_->lastError.empty())
        {
            impl_->lastError = "connect timed out";
        }
    }
}

void ClientTransport::SendCommand(const PlayerCommand& command)
{
    if (!impl_->backend->IsOpen())
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
    const std::size_t pendingCount = impl_->pendingCommands.size();
    const std::size_t count = kCommandBackupCount < pendingCount ? kCommandBackupCount : pendingCount;
    impl_->SendCommandWindow(impl_->pendingCommands.size() - count, impl_->pendingCommands.size());
    impl_->lastCommandReplay = Clock::now();
}

void ClientTransport::SendLobbyUpdate(const LobbyUpdate& update)
{
    if (!impl_->backend->IsOpen() || !impl_->connected)
    {
        return;
    }
    // Lobby intent is idempotent STATE (name/team/hero/ready/start), so its
    // reliability is periodic re-send from Poll(), not an ack protocol: a
    // single unreliable send used to strand a lossy client in the lobby
    // forever when its one ready/start datagram was lost (found by the
    // DAIBED_NET_DROP_PCT loss-injection stress).
    impl_->lastLobbyUpdate = update;
    impl_->hasLobbyUpdate = true;
    impl_->lastLobbyUpdateSent = Clock::now();
    const std::vector<std::uint8_t> bytes = EncodeLobbyUpdate(impl_->sequence++, update);
    impl_->SendBytes(bytes);
}

void ClientTransport::Disconnect()
{
    if (impl_->backend->IsOpen())
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
const SessionCredentials& ClientTransport::Credentials() const { return impl_->credentials; }
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
// Lives with the transport internals it exercises, so it is gated in place
// (the Game-level smokes live in src/Diag/Smokes.cpp).
#if DAIBED_DIAGNOSTICS
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
    const bool inProcess = config.networkBackend == NetworkBackend::InProcessP2P;
    const char* label = inProcess ? "datagram-backend-smoke" : "localhost-net-smoke";
    const char* okMarker = inProcess ? "DATAGRAM_BACKEND_SMOKE_OK" : "LOCALHOST_NET_SMOKE_OK";
    const char* failMarker = inProcess ? "DATAGRAM_BACKEND_SMOKE_FAIL" : "LOCALHOST_NET_SMOKE_FAIL";
    ServerTransport server;
    // Bind to loopback on an ephemeral port so the smoke never collides with a
    // port already in use (the client connects to the actually-bound port).
    ServerConfig serverCfg = config;
    serverCfg.listenAddress = "127.0.0.1";
    serverCfg.port = 0;
    serverCfg.minPlayersToStart = 1;
    if (!server.Start(serverCfg))
    {
        std::cout << label << ": server start failed: " << server.LastError() << '\n';
        std::cout << failMarker << std::endl;
        return 8;
    }
    const std::uint16_t port = server.BoundPort();
    std::cout << label << ": backend=" << ToString(serverCfg.networkBackend)
              << " listening at 127.0.0.1:" << port << " (headless, no window)\n";

    ClientTransport client(serverCfg.networkBackend);
    if (!client.Open("127.0.0.1", port, "", 3.0f))
    {
        std::cout << label << ": client open failed: " << client.LastError() << '\n';
        std::cout << failMarker << std::endl;
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

    std::cout << label << ": connected=" << (connected ? "yes" : "no")
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
    ClientTransport deadClient(serverCfg.networkBackend);
    const std::uint16_t deadPort = 1; // nothing serves UDP on loopback:1
    const bool deadConnected = deadClient.Connect("127.0.0.1", deadPort, "", 0.4f);
    const bool badConnectHandled = !deadConnected
        && (inProcess ? !deadClient.IsOpen() : deadClient.TimedOut());
    deadClient.Close();
    std::cout << label << ": badConnect(port=" << deadPort << ") connected="
              << (deadConnected ? "yes" : "no")
              << " timedOut=" << (deadClient.TimedOut() ? "yes" : "no")
              << " handled=" << (badConnectHandled ? "ok" : "FAIL") << '\n';

    client.Disconnect();
    server.Close();

    const bool ok = connected && snapshotValid && serverSawCommand && badConnectHandled;
    std::cout << (ok ? okMarker : failMarker) << std::endl;
    return ok ? 0 : 8;
}

int RunDatagramBackendSmoke()
{
    std::unique_ptr<IDatagramBackend> listener =
        CreateDatagramBackend(NetworkBackend::InProcessP2P);
    std::unique_ptr<IDatagramBackend> duplicate =
        CreateDatagramBackend(NetworkBackend::InProcessP2P);
    std::unique_ptr<IDatagramBackend> client =
        CreateDatagramBackend(NetworkBackend::InProcessP2P);
    std::unique_ptr<IDatagramBackend> missing =
        CreateDatagramBackend(NetworkBackend::InProcessP2P);
    std::unique_ptr<IDatagramBackend> unsupported =
        CreateDatagramBackend(static_cast<NetworkBackend>(255));

    const bool listened = listener->Listen("p2p-contract", 0);
    const std::uint16_t contractPort = listener->BoundPort();
    const bool duplicateRejected = listened
        && !duplicate->Listen("p2p-contract", contractPort);
    DatagramPeer ignoredPeer;
    const bool missingRejected = !missing->OpenClient(
        "p2p-contract", static_cast<std::uint16_t>(contractPort + 1), ignoredPeer);
    const bool unsupportedRejected = !unsupported->Listen("p2p-contract", 0)
        && unsupported->LastError() == "unsupported network backend";
    DatagramPeer serverPeer;
    const bool clientOpened = listened
        && client->OpenClient("p2p-contract", contractPort, serverPeer);
    const std::array<std::uint8_t, 4> payload { 0xDA, 0x1B, 0xED, 0x26 };
    const bool sent = clientOpened
        && client->Send(serverPeer, payload.data(), payload.size());
    DatagramPeer clientPeer;
    std::array<std::uint8_t, 16> received {};
    std::size_t receivedBytes = 0;
    const bool delivered = sent
        && listener->Receive(
            clientPeer, received.data(), received.size(), receivedBytes)
            == DatagramReceiveResult::Received
        && receivedBytes == payload.size()
        && std::equal(payload.begin(), payload.end(), received.begin());
    const AuthenticatedPeerIdentity clientIdentity =
        listener->AuthenticatedIdentity(clientPeer);
    const AuthenticatedPeerIdentity serverIdentity =
        client->AuthenticatedIdentity(serverPeer);
    const bool identitiesAuthenticated = delivered
        && clientIdentity.IsValid()
        && serverIdentity.IsValid()
        && clientIdentity != serverIdentity;
    client->Close();
    const bool closedPeerRejected = delivered
        && !listener->Send(clientPeer, payload.data(), payload.size());
    listener->Close();

    const bool contractOk = listened && duplicateRejected && missingRejected
        && unsupportedRejected
        && clientOpened && delivered && identitiesAuthenticated && closedPeerRejected;
    std::cout << "datagram-backend-smoke: contract duplicate="
              << (duplicateRejected ? "rejected" : "FAIL")
              << " missing=" << (missingRejected ? "rejected" : "FAIL")
              << " unsupported=" << (unsupportedRejected ? "rejected" : "FAIL")
              << " delivery=" << (delivered ? "ok" : "FAIL")
              << " identity=" << (identitiesAuthenticated ? "authenticated" : "FAIL")
              << " closedPeer=" << (closedPeerRejected ? "rejected" : "FAIL") << '\n';
    if (!contractOk)
    {
        std::cout << "DATAGRAM_BACKEND_SMOKE_FAIL" << std::endl;
        return 8;
    }

    // Provider identity is an independent reconnect factor. A different P2P
    // user who somehow obtains all bearer credentials must not be able to
    // claim the reserved match slot; the original provider identity can.
    ServerConfig identityConfig;
    identityConfig.networkBackend = NetworkBackend::InProcessP2P;
    identityConfig.listenAddress = "identity-contract";
    identityConfig.port = 0;
    identityConfig.minPlayersToStart = 1;
    ServerTransport identityServer;
    ClientTransport legitimate(NetworkBackend::InProcessP2P);
    ClientTransport impostor(NetworkBackend::InProcessP2P);
    bool identityBindingOk = identityServer.Start(identityConfig)
        && legitimate.Open(
            "identity-contract", identityServer.BoundPort(), "", 1.0f);
    for (int attempt = 0; identityBindingOk && attempt < 200
         && !legitimate.IsConnected(); ++attempt)
    {
        identityServer.Poll();
        legitimate.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    identityBindingOk = identityBindingOk && legitimate.IsConnected();
    if (identityBindingOk)
    {
        const std::vector<int> pending = identityServer.TakePendingClients();
        identityBindingOk = pending.size() == 1;
        if (identityBindingOk)
        {
            identityServer.AssignPlayer(pending.front(), 7);
            identityServer.SetMatchJoinLocked(true);
        }
    }

    const SessionCredentials stolenCredentials = legitimate.Credentials();
    if (identityBindingOk)
    {
        legitimate.Disconnect();
        for (int attempt = 0; attempt < 50 && identityServer.ClientCount() != 0; ++attempt)
        {
            identityServer.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        identityBindingOk = identityServer.ClientCount() == 0;
    }
    if (identityBindingOk)
    {
        identityBindingOk = impostor.Open(
            "identity-contract", identityServer.BoundPort(), "", 1.0f,
            stolenCredentials);
    }
    for (int attempt = 0; identityBindingOk && attempt < 200
         && !impostor.WasDenied(); ++attempt)
    {
        identityServer.Poll();
        impostor.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool stolenIdentityDenied = identityBindingOk
        && impostor.WasDenied()
        && impostor.DenyReason() == "invalid reconnect credentials";
    impostor.Close();

    bool originalIdentityReconnected = false;
    bool originalConnected = false;
    std::string originalDenyReason;
    std::size_t reconnectEventCount = 0;
    if (stolenIdentityDenied)
    {
        identityBindingOk = legitimate.Open(
            "identity-contract", identityServer.BoundPort(), "", 1.0f,
            stolenCredentials);
        for (int attempt = 0; identityBindingOk && attempt < 200
             && !legitimate.IsConnected(); ++attempt)
        {
            identityServer.Poll();
            legitimate.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const std::vector<ReconnectedClient> reconnected =
            identityServer.TakeReconnectedClients();
        reconnectEventCount = reconnected.size();
        originalConnected = legitimate.IsConnected();
        originalDenyReason = legitimate.WasDenied() ? legitimate.DenyReason() : std::string();
        originalIdentityReconnected = identityBindingOk
            && legitimate.IsConnected()
            && reconnected.size() == 1
            && reconnected.front().playerId == 7;
    }
    legitimate.Close();
    identityServer.Close();
    identityBindingOk = stolenIdentityDenied && originalIdentityReconnected;
    std::cout << "datagram-backend-smoke: reconnectIdentity stolen="
              << (stolenIdentityDenied ? "denied" : "FAIL")
              << " original=" << (originalIdentityReconnected ? "accepted" : "FAIL")
              << " connected=" << (originalConnected ? "yes" : "no")
              << " events=" << reconnectEventCount
              << " denied=" << (originalDenyReason.empty() ? "no" : originalDenyReason)
              << '\n';
    if (!identityBindingOk)
    {
        std::cout << "DATAGRAM_BACKEND_SMOKE_FAIL" << std::endl;
        return 8;
    }

    ServerConfig config;
    config.networkBackend = NetworkBackend::InProcessP2P;
    return RunLocalhostNetSmoke(config);
}
#else // DAIBED_DIAGNOSTICS == 0
int RunLocalhostNetSmoke(const ServerConfig&)
{
    std::cout << "diagnostics are disabled in this build (DAIBED_DIAGNOSTICS=OFF)" << std::endl;
    return 100;
}
int RunDatagramBackendSmoke()
{
    std::cout << "diagnostics are disabled in this build (DAIBED_DIAGNOSTICS=OFF)" << std::endl;
    return 100;
}
#endif // DAIBED_DIAGNOSTICS

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
bool ServerTransport::NeedsSnapshotForClient(int) const { return false; }
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
void ServerTransport::AdvanceProcessedCommandTick(int, std::uint32_t) {}
int ServerTransport::PlayerForClient(int) const { return -1; }
std::vector<int> ServerTransport::ConnectedClients() const { return {}; }

struct ClientTransport::Impl
{
    std::string lastError = kDisabled;
    std::string denyReason;
    SessionCredentials credentials;
    MatchSnapshot empty;
    LobbySnapshot emptyLobby;
};
ClientTransport::ClientTransport(NetworkBackend) : impl_(std::make_unique<Impl>()) {}
ClientTransport::~ClientTransport() = default;
bool ClientTransport::Open(
    const std::string&, std::uint16_t, const std::string&, float,
    const SessionCredentials&) { return false; }
bool ClientTransport::Connect(
    const std::string&, std::uint16_t, const std::string&, float,
    const SessionCredentials&) { return false; }
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
const SessionCredentials& ClientTransport::Credentials() const { return impl_->credentials; }
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

int RunDatagramBackendSmoke()
{
    std::cout << "datagram-backend-smoke: " << kDisabled << " - skipped\n";
    std::cout << "DATAGRAM_BACKEND_SMOKE_SKIPPED" << std::endl;
    return 0;
}

#endif // DAIBED_HAVE_NETWORK

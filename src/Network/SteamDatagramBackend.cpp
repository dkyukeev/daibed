#include "Network/DatagramBackend.h"
#include "Platform/SteamRuntime.h"

#include <string>

#if defined(DAIBED_HAVE_STEAMWORKS) && DAIBED_HAVE_STEAMWORKS

#include <steam/steam_api.h>
#include <steam/isteamnetworkingsockets.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace
{
class SteamDatagramBackend;

std::mutex gBackendMutex;
std::vector<SteamDatagramBackend*> gBackends;

bool ParseSteamId(std::string_view text, std::uint64_t& value)
{
    constexpr std::string_view prefix = "steam:";
    if (text.substr(0, prefix.size()) == prefix)
    {
        text.remove_prefix(prefix.size());
    }
    if (text.empty())
    {
        return false;
    }
    const char* begin = text.data();
    const char* end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end && value != 0;
}

DatagramPeer PeerFromConnection(HSteamNetConnection connection)
{
    return DatagramPeer { static_cast<std::uint64_t>(connection) };
}

HSteamNetConnection ConnectionFromPeer(DatagramPeer peer)
{
    if (peer.value > static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()))
    {
        return k_HSteamNetConnection_Invalid;
    }
    return static_cast<HSteamNetConnection>(peer.value);
}

class SteamDatagramBackend final : public IDatagramBackend
{
public:
    ~SteamDatagramBackend() override { Close(); }

    bool Listen(const std::string&, std::uint16_t port) override
    {
        Close();
        if (port >= 1000)
        {
            lastError_ = "Steam P2P virtual port must be in range 0..999";
            return false;
        }
        if (!OpenSteam())
        {
            return false;
        }

        SteamNetworkingConfigValue_t callback;
        callback.SetPtr(
            k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
            reinterpret_cast<void*>(OnConnectionStatusChanged));
        listenSocket_ = sockets_->CreateListenSocketP2P(static_cast<int>(port), 1, &callback);
        if (listenSocket_ == k_HSteamListenSocket_Invalid)
        {
            return FailAndClose("Steam CreateListenSocketP2P failed");
        }
        pollGroup_ = sockets_->CreatePollGroup();
        if (pollGroup_ == k_HSteamNetPollGroup_Invalid)
        {
            return FailAndClose("Steam CreatePollGroup failed");
        }
        virtualPort_ = port;
        listening_ = true;
        lastError_.clear();
        return true;
    }

    bool OpenClient(const std::string& host, std::uint16_t port,
                    DatagramPeer& serverPeer) override
    {
        Close();
        if (port >= 1000)
        {
            lastError_ = "Steam P2P virtual port must be in range 0..999";
            return false;
        }
        std::uint64_t steamId = 0;
        if (!ParseSteamId(host, steamId))
        {
            lastError_ = "Steam P2P host must be a numeric Steam ID (optionally steam:<id>)";
            return false;
        }
        if (!OpenSteam())
        {
            return false;
        }

        pollGroup_ = sockets_->CreatePollGroup();
        if (pollGroup_ == k_HSteamNetPollGroup_Invalid)
        {
            return FailAndClose("Steam CreatePollGroup failed");
        }

        // A listen-server's visible client has the same Steam identity as its
        // authoritative endpoint. ConnectP2P is intended for remote identities;
        // Steam explicitly provides CreateSocketPair for an in-process client /
        // server. Besides being reliable, this avoids an unnecessary SDR route
        // negotiation every time the host clicks "create and join".
        SteamDatagramBackend* localListener = nullptr;
        if (steamId == runtime_.LocalSteamId())
        {
            const std::lock_guard<std::mutex> lock(gBackendMutex);
            const auto found = std::find_if(
                gBackends.begin(), gBackends.end(),
                [this, port](SteamDatagramBackend* backend)
                {
                    return backend != nullptr && backend != this
                        && backend->listening_ && backend->virtualPort_ == port
                        && backend->sockets_ == sockets_;
                });
            if (found != gBackends.end())
            {
                localListener = *found;
            }
        }
        if (localListener != nullptr)
        {
            SteamNetworkingIdentity localIdentity;
            localIdentity.Clear();
            localIdentity.SetSteamID64(steamId);
            HSteamNetConnection clientConnection = k_HSteamNetConnection_Invalid;
            HSteamNetConnection serverConnection = k_HSteamNetConnection_Invalid;
            if (!sockets_->CreateSocketPair(
                    &clientConnection, &serverConnection, false,
                    &localIdentity, &localIdentity)
                || !sockets_->SetConnectionPollGroup(clientConnection, pollGroup_)
                || !sockets_->SetConnectionPollGroup(
                    serverConnection, localListener->pollGroup_))
            {
                if (clientConnection != k_HSteamNetConnection_Invalid)
                {
                    sockets_->CloseConnection(clientConnection, 0, "loopback setup failed", false);
                }
                if (serverConnection != k_HSteamNetConnection_Invalid)
                {
                    sockets_->CloseConnection(serverConnection, 0, "loopback setup failed", false);
                }
                return FailAndClose("Steam CreateSocketPair failed for local listen-server");
            }

            const AuthenticatedPeerIdentity authenticated {
                DatagramIdentityProvider::Steam,
                steamId
            };
            connections_.push_back(clientConnection);
            connectionIdentities_[clientConnection] = authenticated;
            localListener->connections_.push_back(serverConnection);
            localListener->connectionIdentities_[serverConnection] = authenticated;
            localListener->lastError_.clear();
            serverPeer = PeerFromConnection(clientConnection);
            virtualPort_ = port;
            listening_ = false;
            lastError_.clear();
            return true;
        }

        SteamNetworkingIdentity identity;
        identity.Clear();
        identity.SetSteamID64(steamId);
        SteamNetworkingConfigValue_t callback;
        callback.SetPtr(
            k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
            reinterpret_cast<void*>(OnConnectionStatusChanged));
        const HSteamNetConnection connection = sockets_->ConnectP2P(
            identity, static_cast<int>(port), 1, &callback);
        if (connection == k_HSteamNetConnection_Invalid)
        {
            return FailAndClose("Steam ConnectP2P failed");
        }
        if (!sockets_->SetConnectionPollGroup(connection, pollGroup_))
        {
            sockets_->CloseConnection(connection, 0, "poll group setup failed", false);
            return FailAndClose("Steam SetConnectionPollGroup failed");
        }

        connections_.push_back(connection);
        connectionIdentities_[connection] = AuthenticatedPeerIdentity {
            DatagramIdentityProvider::Steam,
            steamId
        };
        serverPeer = PeerFromConnection(connection);
        virtualPort_ = port;
        listening_ = false;
        lastError_.clear();
        return true;
    }

    DatagramReceiveResult Receive(
        DatagramPeer& peer, std::uint8_t* bytes, std::size_t capacity,
        std::size_t& receivedBytes) override
    {
        receivedBytes = 0;
        if (!sockets_ || pollGroup_ == k_HSteamNetPollGroup_Invalid || !bytes || capacity == 0)
        {
            lastError_ = "Steam P2P endpoint is not open";
            return DatagramReceiveResult::Error;
        }

        runtime_.PumpCallbacks();
        SteamNetworkingMessage_t* message = nullptr;
        const int count = sockets_->ReceiveMessagesOnPollGroup(pollGroup_, &message, 1);
        if (count < 0)
        {
            lastError_ = "Steam ReceiveMessagesOnPollGroup failed";
            return DatagramReceiveResult::Error;
        }
        if (count == 0)
        {
            return DatagramReceiveResult::Empty;
        }

        if (message == nullptr || message->m_cbSize < 0
            || static_cast<std::size_t>(message->m_cbSize) > capacity)
        {
            if (message)
            {
                message->Release();
            }
            lastError_ = "Steam P2P receive buffer is too small";
            return DatagramReceiveResult::Error;
        }

        peer = PeerFromConnection(message->m_conn);
        const std::uint64_t remoteSteamId = message->m_identityPeer.GetSteamID64();
        if (remoteSteamId != 0)
        {
            connectionIdentities_[message->m_conn] = AuthenticatedPeerIdentity {
                DatagramIdentityProvider::Steam,
                remoteSteamId
            };
        }
        receivedBytes = static_cast<std::size_t>(message->m_cbSize);
        std::memcpy(bytes, message->m_pData, receivedBytes);
        message->Release();
        return DatagramReceiveResult::Received;
    }

    bool Send(DatagramPeer peer, const std::uint8_t* bytes, std::size_t size) override
    {
        if (!sockets_ || !peer.IsValid() || !bytes
            || size > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()))
        {
            lastError_ = "Steam P2P send endpoint is invalid";
            return false;
        }
        runtime_.PumpCallbacks();
        const HSteamNetConnection connection = ConnectionFromPeer(peer);
        if (connection == k_HSteamNetConnection_Invalid
            || std::find(connections_.begin(), connections_.end(), connection) == connections_.end())
        {
            lastError_ = "Steam P2P connection is unavailable";
            return false;
        }
        const EResult result = sockets_->SendMessageToConnection(
            connection, bytes, static_cast<std::uint32_t>(size),
            k_nSteamNetworkingSend_UnreliableNoNagle, nullptr);
        if (result != k_EResultOK)
        {
            lastError_ = "Steam SendMessageToConnection failed (EResult "
                + std::to_string(static_cast<int>(result)) + ")";
            return false;
        }
        return true;
    }

    bool IsOpen() const override
    {
        return sockets_ != nullptr
            && (listenSocket_ != k_HSteamListenSocket_Invalid || !connections_.empty());
    }

    void Close() override
    {
        if (sockets_)
        {
            for (HSteamNetConnection connection : connections_)
            {
                sockets_->CloseConnection(connection, 0, "endpoint closed", false);
            }
            connections_.clear();
            connectionIdentities_.clear();
            if (listenSocket_ != k_HSteamListenSocket_Invalid)
            {
                sockets_->CloseListenSocket(listenSocket_);
            }
            if (pollGroup_ != k_HSteamNetPollGroup_Invalid)
            {
                sockets_->DestroyPollGroup(pollGroup_);
            }
        }
        Unregister();
        listenSocket_ = k_HSteamListenSocket_Invalid;
        pollGroup_ = k_HSteamNetPollGroup_Invalid;
        sockets_ = nullptr;
        virtualPort_ = 0;
        listening_ = false;
        runtime_.Release();
    }

    std::uint16_t BoundPort() const override { return virtualPort_; }
    const std::string& LastError() const override { return lastError_; }

    AuthenticatedPeerIdentity AuthenticatedIdentity(DatagramPeer peer) const override
    {
        if (!sockets_)
        {
            return {};
        }
        const HSteamNetConnection connection = ConnectionFromPeer(peer);
        const auto cached = connectionIdentities_.find(connection);
        if (cached != connectionIdentities_.end())
        {
            return cached->second;
        }
        SteamNetConnectionInfo_t info {};
        if (connection == k_HSteamNetConnection_Invalid
            || !sockets_->GetConnectionInfo(connection, &info))
        {
            return {};
        }
        const std::uint64_t steamId = info.m_identityRemote.GetSteamID64();
        return steamId != 0
            ? AuthenticatedPeerIdentity { DatagramIdentityProvider::Steam, steamId }
            : AuthenticatedPeerIdentity {};
    }

private:
    static void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* event)
    {
        std::vector<SteamDatagramBackend*> backends;
        {
            const std::lock_guard<std::mutex> lock(gBackendMutex);
            backends = gBackends;
        }
        for (SteamDatagramBackend* backend : backends)
        {
            if (backend && backend->HandleConnectionStatus(*event))
            {
                break;
            }
        }
    }

    bool HandleConnectionStatus(const SteamNetConnectionStatusChangedCallback_t& event)
    {
        const HSteamNetConnection connection = event.m_hConn;
        const bool ours = event.m_info.m_hListenSocket == listenSocket_
            || std::find(connections_.begin(), connections_.end(), connection) != connections_.end();
        if (!ours)
        {
            return false;
        }
        const std::uint64_t remoteSteamId = event.m_info.m_identityRemote.GetSteamID64();
        if (remoteSteamId != 0)
        {
            connectionIdentities_[connection] = AuthenticatedPeerIdentity {
                DatagramIdentityProvider::Steam,
                remoteSteamId
            };
        }

        switch (event.m_info.m_eState)
        {
        case k_ESteamNetworkingConnectionState_Connecting:
            if (listening_ && event.m_info.m_hListenSocket == listenSocket_)
            {
                const EResult accepted = sockets_->AcceptConnection(connection);
                if (accepted != k_EResultOK
                    || !sockets_->SetConnectionPollGroup(connection, pollGroup_))
                {
                    lastError_ = "Steam rejected an incoming P2P connection";
                    sockets_->CloseConnection(connection, 0, "accept failed", false);
                }
                else
                {
                    connections_.push_back(connection);
                }
            }
            break;
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
            char details[4096] {};
            const int detailResult = sockets_->GetDetailedConnectionStatus(
                connection, details, static_cast<int>(sizeof(details)));
            lastError_ = event.m_info.m_szEndDebug;
            if (detailResult >= 0 && details[0] != '\0')
            {
                lastError_ += " | ";
                lastError_ += details;
            }
            sockets_->CloseConnection(connection, 0, nullptr, false);
            connections_.erase(
                std::remove(connections_.begin(), connections_.end(), connection),
                connections_.end());
            break;
        }
        case k_ESteamNetworkingConnectionState_Connected:
            lastError_.clear();
            break;
        default:
            break;
        }
        return true;
    }

    bool OpenSteam()
    {
        if (!runtime_.Acquire())
        {
            lastError_ = runtime_.LastError();
            return false;
        }
        sockets_ = SteamNetworkingSockets();
        if (!sockets_)
        {
            return FailAndClose("SteamNetworkingSockets is unavailable");
        }
        const std::lock_guard<std::mutex> lock(gBackendMutex);
        gBackends.push_back(this);
        registered_ = true;
        return true;
    }

    void Unregister()
    {
        if (!registered_)
        {
            return;
        }
        const std::lock_guard<std::mutex> lock(gBackendMutex);
        gBackends.erase(std::remove(gBackends.begin(), gBackends.end(), this), gBackends.end());
        registered_ = false;
    }

    bool FailAndClose(const std::string& error)
    {
        Close();
        lastError_ = error;
        return false;
    }

    ISteamNetworkingSockets* sockets_ = nullptr;
    HSteamListenSocket listenSocket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup pollGroup_ = k_HSteamNetPollGroup_Invalid;
    std::vector<HSteamNetConnection> connections_;
    mutable std::unordered_map<HSteamNetConnection, AuthenticatedPeerIdentity>
        connectionIdentities_;
    SteamRuntimeLease runtime_;
    std::uint16_t virtualPort_ = 0;
    bool registered_ = false;
    bool listening_ = false;
    std::string lastError_;
};
} // namespace

std::unique_ptr<IDatagramBackend> CreateSteamDatagramBackend()
{
    return std::make_unique<SteamDatagramBackend>();
}

#else

namespace
{
class DisabledSteamDatagramBackend final : public IDatagramBackend
{
public:
    bool Listen(const std::string&, std::uint16_t) override { return false; }
    bool OpenClient(const std::string&, std::uint16_t, DatagramPeer&) override { return false; }
    DatagramReceiveResult Receive(
        DatagramPeer&, std::uint8_t*, std::size_t, std::size_t& receivedBytes) override
    {
        receivedBytes = 0;
        return DatagramReceiveResult::Error;
    }
    bool Send(DatagramPeer, const std::uint8_t*, std::size_t) override { return false; }
    bool IsOpen() const override { return false; }
    void Close() override {}
    std::uint16_t BoundPort() const override { return 0; }
    const std::string& LastError() const override { return error_; }

private:
    std::string error_ =
        "Steam P2P backend disabled; configure with DAIBED_ENABLE_STEAMWORKS=ON "
        "and DAIBED_STEAMWORKS_SDK_ROOT=<sdk-root>";
};
} // namespace

std::unique_ptr<IDatagramBackend> CreateSteamDatagramBackend()
{
    return std::make_unique<DisabledSteamDatagramBackend>();
}

#endif

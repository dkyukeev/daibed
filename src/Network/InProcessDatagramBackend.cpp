#include "Network/DatagramBackend.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
struct QueuedDatagram
{
    DatagramPeer from;
    AuthenticatedPeerIdentity identity;
    std::vector<std::uint8_t> bytes;
};

struct InProcessEndpoint
{
    AuthenticatedPeerIdentity identity;
    std::deque<QueuedDatagram> incoming;
};

struct InProcessHub
{
    std::mutex mutex;
    std::uint64_t nextPeer = 1;
    std::uint64_t nextIdentity = 1;
    std::uint16_t nextEphemeralPort = 40000;
    std::unordered_map<std::uint64_t, InProcessEndpoint> endpoints;
    std::unordered_map<std::string, std::uint64_t> listeners;
};

InProcessHub& Hub()
{
    static InProcessHub hub;
    return hub;
}

std::string NormalizeAddress(const std::string& address)
{
    return address == "localhost" ? std::string("127.0.0.1") : address;
}

std::string ListenerKey(const std::string& address, std::uint16_t port)
{
    return NormalizeAddress(address) + ":" + std::to_string(port);
}

std::uint64_t AllocatePeer(InProcessHub& hub)
{
    while (hub.nextPeer == 0 || hub.endpoints.find(hub.nextPeer) != hub.endpoints.end())
    {
        ++hub.nextPeer;
    }
    return hub.nextPeer++;
}

class InProcessDatagramBackend final : public IDatagramBackend
{
public:
    ~InProcessDatagramBackend() override { Close(); }

    bool Listen(const std::string& address, std::uint16_t port) override
    {
        Close();
        InProcessHub& hub = Hub();
        const std::lock_guard<std::mutex> lock(hub.mutex);

        const std::string normalized = NormalizeAddress(address);
        if (normalized.empty())
        {
            lastError_ = "in-process listen address is empty";
            return false;
        }
        if (port == 0)
        {
            port = AllocatePort(hub, normalized);
            if (port == 0)
            {
                lastError_ = "no in-process ephemeral ports available";
                return false;
            }
        }

        const std::string key = ListenerKey(normalized, port);
        if (hub.listeners.find(key) != hub.listeners.end())
        {
            lastError_ = "in-process endpoint already listening at " + key;
            return false;
        }

        EnsureIdentity(hub);
        endpoint_.value = AllocatePeer(hub);
        hub.endpoints.emplace(endpoint_.value, InProcessEndpoint { identity_, {} });
        hub.listeners.emplace(key, endpoint_.value);
        listenerKey_ = key;
        boundPort_ = port;
        lastError_.clear();
        return true;
    }

    bool OpenClient(const std::string& host, std::uint16_t port,
                    DatagramPeer& serverPeer) override
    {
        Close();
        InProcessHub& hub = Hub();
        const std::lock_guard<std::mutex> lock(hub.mutex);

        auto found = hub.listeners.find(ListenerKey(host, port));
        if (found == hub.listeners.end())
        {
            found = hub.listeners.find(ListenerKey("0.0.0.0", port));
        }
        if (found == hub.listeners.end())
        {
            lastError_ = "in-process listener not found at "
                + ListenerKey(host, port);
            return false;
        }

        EnsureIdentity(hub);
        endpoint_.value = AllocatePeer(hub);
        hub.endpoints.emplace(endpoint_.value, InProcessEndpoint { identity_, {} });
        serverPeer.value = found->second;
        boundPort_ = 0;
        lastError_.clear();
        return true;
    }

    DatagramReceiveResult Receive(
        DatagramPeer& peer, std::uint8_t* bytes, std::size_t capacity,
        std::size_t& receivedBytes) override
    {
        receivedBytes = 0;
        if (!endpoint_.IsValid() || bytes == nullptr || capacity == 0)
        {
            lastError_ = "in-process endpoint is not open";
            return DatagramReceiveResult::Error;
        }

        InProcessHub& hub = Hub();
        const std::lock_guard<std::mutex> lock(hub.mutex);
        const auto endpoint = hub.endpoints.find(endpoint_.value);
        if (endpoint == hub.endpoints.end())
        {
            lastError_ = "in-process endpoint was closed";
            return DatagramReceiveResult::Error;
        }
        if (endpoint->second.incoming.empty())
        {
            return DatagramReceiveResult::Empty;
        }

        QueuedDatagram datagram = std::move(endpoint->second.incoming.front());
        endpoint->second.incoming.pop_front();
        if (datagram.bytes.size() > capacity)
        {
            lastError_ = "in-process receive buffer is too small";
            return DatagramReceiveResult::Error;
        }
        peer = datagram.from;
        observedIdentities_[peer.value] = datagram.identity;
        receivedBytes = datagram.bytes.size();
        std::memcpy(bytes, datagram.bytes.data(), receivedBytes);
        return DatagramReceiveResult::Received;
    }

    bool Send(DatagramPeer peer, const std::uint8_t* bytes, std::size_t size) override
    {
        if (!endpoint_.IsValid() || !peer.IsValid() || bytes == nullptr)
        {
            lastError_ = "in-process send endpoint is invalid";
            return false;
        }
        if (size > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)()))
        {
            lastError_ = "in-process datagram is too large";
            return false;
        }

        InProcessHub& hub = Hub();
        const std::lock_guard<std::mutex> lock(hub.mutex);
        if (hub.endpoints.find(endpoint_.value) == hub.endpoints.end())
        {
            lastError_ = "in-process sender was closed";
            return false;
        }
        const auto destination = hub.endpoints.find(peer.value);
        if (destination == hub.endpoints.end())
        {
            lastError_ = "in-process destination is unavailable";
            return false;
        }

        QueuedDatagram datagram;
        datagram.from = endpoint_;
        datagram.identity = identity_;
        datagram.bytes.assign(bytes, bytes + size);
        destination->second.incoming.push_back(std::move(datagram));
        return true;
    }

    bool IsOpen() const override { return endpoint_.IsValid(); }

    void Close() override
    {
        if (!endpoint_.IsValid())
        {
            boundPort_ = 0;
            listenerKey_.clear();
            observedIdentities_.clear();
            return;
        }

        InProcessHub& hub = Hub();
        const std::lock_guard<std::mutex> lock(hub.mutex);
        if (!listenerKey_.empty())
        {
            const auto listener = hub.listeners.find(listenerKey_);
            if (listener != hub.listeners.end() && listener->second == endpoint_.value)
            {
                hub.listeners.erase(listener);
            }
        }
        hub.endpoints.erase(endpoint_.value);
        observedIdentities_.clear();
        endpoint_ = {};
        boundPort_ = 0;
        listenerKey_.clear();
    }

    std::uint16_t BoundPort() const override { return boundPort_; }
    const std::string& LastError() const override { return lastError_; }

    AuthenticatedPeerIdentity AuthenticatedIdentity(DatagramPeer peer) const override
    {
        InProcessHub& hub = Hub();
        const std::lock_guard<std::mutex> lock(hub.mutex);
        const auto endpoint = hub.endpoints.find(peer.value);
        if (endpoint != hub.endpoints.end())
        {
            return endpoint->second.identity;
        }
        const auto observed = observedIdentities_.find(peer.value);
        return observed != observedIdentities_.end()
            ? observed->second
            : AuthenticatedPeerIdentity {};
    }

private:
    void EnsureIdentity(InProcessHub& hub)
    {
        if (identity_.IsValid())
        {
            return;
        }
        while (hub.nextIdentity == 0)
        {
            ++hub.nextIdentity;
        }
        identity_ = AuthenticatedPeerIdentity {
            DatagramIdentityProvider::InProcessTest,
            hub.nextIdentity++
        };
    }

    static std::uint16_t AllocatePort(InProcessHub& hub, const std::string& address)
    {
        constexpr std::uint32_t kAttempts = 65536u - 40000u;
        for (std::uint32_t attempt = 0; attempt < kAttempts; ++attempt)
        {
            const std::uint16_t candidate = hub.nextEphemeralPort++;
            if (hub.nextEphemeralPort < 40000)
            {
                hub.nextEphemeralPort = 40000;
            }
            if (hub.listeners.find(ListenerKey(address, candidate)) == hub.listeners.end())
            {
                return candidate;
            }
        }
        return 0;
    }

    DatagramPeer endpoint_;
    AuthenticatedPeerIdentity identity_;
    mutable std::unordered_map<std::uint64_t, AuthenticatedPeerIdentity> observedIdentities_;
    std::uint16_t boundPort_ = 0;
    std::string listenerKey_;
    std::string lastError_;
};

class UnsupportedDatagramBackend final : public IDatagramBackend
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
    std::string error_ = "unsupported network backend";
};
} // namespace

std::unique_ptr<IDatagramBackend> CreateInProcessDatagramBackend()
{
    return std::make_unique<InProcessDatagramBackend>();
}

std::unique_ptr<IDatagramBackend> CreateDatagramBackend(NetworkBackend backend)
{
    switch (backend)
    {
    case NetworkBackend::SystemUdp:
        return CreateUdpDatagramBackend();
    case NetworkBackend::InProcessP2P:
        return CreateInProcessDatagramBackend();
    case NetworkBackend::SteamP2P:
        return CreateSteamDatagramBackend();
    }
    return std::make_unique<UnsupportedDatagramBackend>();
}

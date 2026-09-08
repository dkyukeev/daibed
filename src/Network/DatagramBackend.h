#pragma once

#include "Network/NetTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Transport-neutral identity of a datagram peer. The native UDP backend packs
// an IPv4 address and port into this value; a future relay backend may use the
// provider's connection/peer handle instead. Protocol code must treat it as an
// opaque token and only compare or pass it back to the backend.
struct DatagramPeer
{
    std::uint64_t value = 0;

    bool IsValid() const { return value != 0; }
};

inline bool operator==(DatagramPeer a, DatagramPeer b) { return a.value == b.value; }
inline bool operator!=(DatagramPeer a, DatagramPeer b) { return !(a == b); }

// Provider-authenticated identity associated with an opaque transport peer.
// Native UDP cannot supply one. Steam obtains it from the authenticated remote
// SteamNetworkingIdentity, never from a display name or gameplay packet.
enum class DatagramIdentityProvider : std::uint8_t
{
    None,
    InProcessTest,
    Steam
};

struct AuthenticatedPeerIdentity
{
    DatagramIdentityProvider provider = DatagramIdentityProvider::None;
    std::uint64_t value = 0;

    bool IsValid() const
    {
        return provider != DatagramIdentityProvider::None && value != 0;
    }
};

inline bool operator==(AuthenticatedPeerIdentity a, AuthenticatedPeerIdentity b)
{
    return a.provider == b.provider && a.value == b.value;
}
inline bool operator!=(AuthenticatedPeerIdentity a, AuthenticatedPeerIdentity b)
{
    return !(a == b);
}

enum class DatagramReceiveResult
{
    Received,
    Empty,
    Error
};

// Lowest networking layer used by the DaiBed wire/session transport. It knows
// how to move one datagram to/from an opaque peer, but knows nothing about
// handshakes, reliability, snapshots, commands, lobbies, or gameplay.
class IDatagramBackend
{
public:
    virtual ~IDatagramBackend() = default;

    // Server mode: bind to address:port. Port zero requests an ephemeral port.
    virtual bool Listen(const std::string& address, std::uint16_t port) = 0;
    // Client mode: open an ephemeral local endpoint and resolve the server.
    virtual bool OpenClient(const std::string& host, std::uint16_t port,
                            DatagramPeer& serverPeer) = 0;

    virtual DatagramReceiveResult Receive(
        DatagramPeer& peer, std::uint8_t* bytes, std::size_t capacity,
        std::size_t& receivedBytes) = 0;
    virtual bool Send(DatagramPeer peer, const std::uint8_t* bytes, std::size_t size) = 0;

    virtual bool IsOpen() const = 0;
    virtual void Close() = 0;
    virtual std::uint16_t BoundPort() const = 0;
    virtual const std::string& LastError() const = 0;

    // Returns an identity cryptographically/platform-authenticated by the
    // provider. The default is deliberately invalid for plain UDP backends.
    virtual AuthenticatedPeerIdentity AuthenticatedIdentity(DatagramPeer) const
    {
        return {};
    }
};

// Native IPv4 UDP implementation (Winsock2 on Windows, BSD sockets elsewhere).
std::unique_ptr<IDatagramBackend> CreateUdpDatagramBackend();
// Dependency-free message hub used to validate P2P/provider semantics in one
// process. It uses opaque peer handles and never opens an OS socket.
std::unique_ptr<IDatagramBackend> CreateInProcessDatagramBackend();
// Steam Networking Sockets implementation. Without an SDK-enabled build this
// returns a safe stub whose LastError() explains how to enable it.
std::unique_ptr<IDatagramBackend> CreateSteamDatagramBackend();

// Single construction point used by the session transport. Provider-specific
// SDK backends are added here without leaking their types into gameplay code.
std::unique_ptr<IDatagramBackend> CreateDatagramBackend(NetworkBackend backend);

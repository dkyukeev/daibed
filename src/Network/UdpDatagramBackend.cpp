#include "Network/DatagramBackend.h"

#include <cstring>
#include <limits>
#include <mutex>
#include <string>

#if defined(DAIBED_HAVE_NETWORK) && DAIBED_HAVE_NETWORK

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
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

namespace
{
int g_udpInitCount = 0;
std::mutex g_udpInitMutex;

bool NetInit(std::string& error)
{
    const std::lock_guard<std::mutex> lock(g_udpInitMutex);
#if defined(_WIN32)
    if (g_udpInitCount == 0)
    {
        WSADATA data;
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0)
        {
            error = "WSAStartup failed (" + std::to_string(rc) + ")";
            return false;
        }
    }
#else
    (void)error;
#endif
    ++g_udpInitCount;
    return true;
}

void NetShutdown()
{
    const std::lock_guard<std::mutex> lock(g_udpInitMutex);
    if (g_udpInitCount <= 0)
    {
        return;
    }
    --g_udpInitCount;
#if defined(_WIN32)
    if (g_udpInitCount == 0)
    {
        WSACleanup();
    }
#endif
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

void CloseSocket(socket_t socket)
{
    if (socket == kInvalidSocket)
    {
        return;
    }
#if defined(_WIN32)
    closesocket(socket);
#else
    ::close(socket);
#endif
}

bool SetNonBlocking(socket_t socket)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags != -1 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

void SuppressConnReset(socket_t socket)
{
#if defined(_WIN32)
    BOOL behavior = FALSE;
    DWORD bytes = 0;
    WSAIoctl(socket, SIO_UDP_CONNRESET, &behavior, sizeof(behavior),
             nullptr, 0, &bytes, nullptr, nullptr);
#else
    (void)socket;
#endif
}

bool ResolveIPv4(const std::string& host, std::uint16_t port,
                 sockaddr_in& out, std::string& error)
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
        error = "could not resolve host \"" + host + "\"";
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

DatagramPeer PeerFromAddress(const sockaddr_in& address)
{
    DatagramPeer peer;
    peer.value = (static_cast<std::uint64_t>(address.sin_addr.s_addr) << 16)
        | static_cast<std::uint64_t>(address.sin_port);
    return peer;
}

sockaddr_in AddressFromPeer(DatagramPeer peer)
{
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = static_cast<decltype(address.sin_addr.s_addr)>(peer.value >> 16);
    address.sin_port = static_cast<decltype(address.sin_port)>(peer.value & 0xFFFFu);
    return address;
}

class UdpDatagramBackend final : public IDatagramBackend
{
public:
    ~UdpDatagramBackend() override { Close(); }

    bool Listen(const std::string& address, std::uint16_t port) override
    {
        Close();
        if (!InitializeSocket())
        {
            return false;
        }

        sockaddr_in local;
        if (!ResolveIPv4(address, port, local, lastError_))
        {
            Close();
            return false;
        }
        if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
        {
            lastError_ = "bind(" + address + ":" + std::to_string(port)
                + ") failed (" + std::to_string(LastSocketError()) + ")";
            Close();
            return false;
        }

        sockaddr_in bound;
        socklen_t boundLength = sizeof(bound);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0)
        {
            boundPort_ = ntohs(bound.sin_port);
        }
        else
        {
            boundPort_ = port;
        }
        lastError_.clear();
        return true;
    }

    bool OpenClient(const std::string& host, std::uint16_t port,
                    DatagramPeer& serverPeer) override
    {
        Close();
        sockaddr_in serverAddress;
        if (!NetInit(lastError_))
        {
            return false;
        }
        initialized_ = true;
        if (!ResolveIPv4(host, port, serverAddress, lastError_))
        {
            Close();
            return false;
        }
        if (!CreateSocket())
        {
            Close();
            return false;
        }

        sockaddr_in local;
        std::memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = 0;
        if (bind(socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
        {
            lastError_ = "client bind failed (" + std::to_string(LastSocketError()) + ")";
            Close();
            return false;
        }

        sockaddr_in bound;
        socklen_t boundLength = sizeof(bound);
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0)
        {
            boundPort_ = ntohs(bound.sin_port);
        }
        serverPeer = PeerFromAddress(serverAddress);
        lastError_.clear();
        return true;
    }

    DatagramReceiveResult Receive(
        DatagramPeer& peer, std::uint8_t* bytes, std::size_t capacity,
        std::size_t& receivedBytes) override
    {
        receivedBytes = 0;
        if (socket_ == kInvalidSocket || bytes == nullptr || capacity == 0)
        {
            return DatagramReceiveResult::Error;
        }
        if (capacity > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        {
            lastError_ = "receive buffer is too large";
            return DatagramReceiveResult::Error;
        }

        sockaddr_in from;
        socklen_t fromLength = sizeof(from);
        const int count = recvfrom(
            socket_, reinterpret_cast<char*>(bytes), static_cast<int>(capacity), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (count < 0)
        {
            const int error = LastSocketError();
            if (WouldBlock(error))
            {
                return DatagramReceiveResult::Empty;
            }
            lastError_ = "recvfrom failed (" + std::to_string(error) + ")";
            return DatagramReceiveResult::Error;
        }

        peer = PeerFromAddress(from);
        receivedBytes = static_cast<std::size_t>(count);
        return DatagramReceiveResult::Received;
    }

    bool Send(DatagramPeer peer, const std::uint8_t* bytes, std::size_t size) override
    {
        if (socket_ == kInvalidSocket || !peer.IsValid() || bytes == nullptr)
        {
            return false;
        }
        if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        {
            lastError_ = "datagram is too large";
            return false;
        }
        const sockaddr_in address = AddressFromPeer(peer);
        const int sent = sendto(
            socket_, reinterpret_cast<const char*>(bytes), static_cast<int>(size), 0,
            reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (sent != static_cast<int>(size))
        {
            lastError_ = "sendto failed (" + std::to_string(LastSocketError()) + ")";
            return false;
        }
        return true;
    }

    bool IsOpen() const override { return socket_ != kInvalidSocket; }

    void Close() override
    {
        CloseSocketOnly();
        boundPort_ = 0;
        if (initialized_)
        {
            NetShutdown();
            initialized_ = false;
        }
    }

    std::uint16_t BoundPort() const override { return boundPort_; }
    const std::string& LastError() const override { return lastError_; }

private:
    bool InitializeSocket()
    {
        if (!NetInit(lastError_))
        {
            return false;
        }
        initialized_ = true;
        if (!CreateSocket())
        {
            Close();
            return false;
        }
        return true;
    }

    bool CreateSocket()
    {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == kInvalidSocket)
        {
            lastError_ = "socket() failed (" + std::to_string(LastSocketError()) + ")";
            return false;
        }
        SuppressConnReset(socket_);
        if (!SetNonBlocking(socket_))
        {
            lastError_ = "could not set non-blocking";
            CloseSocketOnly();
            return false;
        }
        return true;
    }

    void CloseSocketOnly()
    {
        if (socket_ != kInvalidSocket)
        {
            CloseSocket(socket_);
            socket_ = kInvalidSocket;
        }
    }

    socket_t socket_ = kInvalidSocket;
    bool initialized_ = false;
    std::uint16_t boundPort_ = 0;
    std::string lastError_;
};
} // namespace

std::unique_ptr<IDatagramBackend> CreateUdpDatagramBackend()
{
    return std::make_unique<UdpDatagramBackend>();
}

#else

namespace
{
class DisabledUdpDatagramBackend final : public IDatagramBackend
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
    std::string error_ = "network transport disabled at build (DAIBED_ENABLE_NETWORK=OFF)";
};
} // namespace

std::unique_ptr<IDatagramBackend> CreateUdpDatagramBackend()
{
    return std::make_unique<DisabledUdpDatagramBackend>();
}

#endif

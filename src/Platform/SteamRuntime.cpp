#include "Platform/SteamRuntime.h"

#if defined(DAIBED_HAVE_STEAMWORKS) && DAIBED_HAVE_STEAMWORKS

#include <steam/steam_api.h>

#include <mutex>

namespace
{
std::mutex gRuntimeMutex;
int gRuntimeLeases = 0;
}

SteamRuntimeLease::~SteamRuntimeLease()
{
    Release();
}

bool SteamRuntimeLease::Acquire()
{
    if (active_)
    {
        return true;
    }

    const std::lock_guard<std::mutex> lock(gRuntimeMutex);
    if (gRuntimeLeases == 0 && !SteamAPI_Init())
    {
        lastError_ =
            "SteamAPI_Init failed; start Steam and launch the game through its Steam App ID";
        return false;
    }
    // BLoggedOn() is a live connection-state indicator, not proof that the
    // local Steam identity is invalid.  It may be false briefly while the
    // client reconnects immediately after SteamAPI_Init.  Treating that
    // transient state as fatal used to shut the API down and permanently
    // disable Steam hosting for the rest of the DaiBed process.
    if (SteamUser() == nullptr)
    {
        if (gRuntimeLeases == 0)
        {
            SteamAPI_Shutdown();
        }
        lastError_ = "Steam User API is unavailable; restart Steam and DaiBed";
        return false;
    }
    if (SteamUser()->GetSteamID().ConvertToUint64() == 0)
    {
        if (gRuntimeLeases == 0)
        {
            SteamAPI_Shutdown();
        }
        lastError_ = "Steam has not provided a local user identity yet; retry in a moment";
        return false;
    }
    // Warm SDR routing/certificates before the first ConnectP2P. Without this,
    // a cold relay route often spends the old three-second game timeout merely
    // initializing, which looked like a failed DaiBed handshake to every peer.
    if (SteamNetworkingUtils() != nullptr)
    {
        SteamNetworkingUtils()->InitRelayNetworkAccess();
    }

    ++gRuntimeLeases;
    active_ = true;
    lastError_.clear();
    return true;
}

void SteamRuntimeLease::Release()
{
    if (!active_)
    {
        return;
    }
    const std::lock_guard<std::mutex> lock(gRuntimeMutex);
    active_ = false;
    if (gRuntimeLeases > 0 && --gRuntimeLeases == 0)
    {
        SteamAPI_Shutdown();
    }
}

void SteamRuntimeLease::PumpCallbacks()
{
    if (active_)
    {
        SteamAPI_RunCallbacks();
    }
}

std::uint64_t SteamRuntimeLease::LocalSteamId() const
{
    if (!active_ || SteamUser() == nullptr)
    {
        return 0;
    }
    return SteamUser()->GetSteamID().ConvertToUint64();
}

#else

SteamRuntimeLease::~SteamRuntimeLease() = default;

bool SteamRuntimeLease::Acquire()
{
    lastError_ =
        "Steam runtime disabled; configure with DAIBED_ENABLE_STEAMWORKS=ON "
        "and DAIBED_STEAMWORKS_SDK_ROOT=<sdk-root>";
    return false;
}

void SteamRuntimeLease::Release()
{
    active_ = false;
}

void SteamRuntimeLease::PumpCallbacks() {}

std::uint64_t SteamRuntimeLease::LocalSteamId() const
{
    return 0;
}

#endif

#pragma once

#include <cstdint>
#include <string>

// Process-wide Steamworks lifetime lease. Networking, lobbies and invitations
// share this owner instead of independently calling SteamAPI_Init/Shutdown.
// DaiBed currently invokes Steam services from its serialized game/network
// polling thread; provider callbacks must hand work back to that thread.
class SteamRuntimeLease
{
public:
    SteamRuntimeLease() = default;
    ~SteamRuntimeLease();

    SteamRuntimeLease(const SteamRuntimeLease&) = delete;
    SteamRuntimeLease& operator=(const SteamRuntimeLease&) = delete;

    bool Acquire();
    void Release();
    void PumpCallbacks();

    bool IsActive() const { return active_; }
    std::uint64_t LocalSteamId() const;
    const std::string& LastError() const { return lastError_; }

private:
    bool active_ = false;
    std::string lastError_;
};

#pragma once

#include "Network/PlayerCommand.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

enum class BotControlSource : std::uint8_t
{
    Strategy,
    Navigation,
    Utility,
    Combat,
    BreakAction,
    BridgeAction,
    Emergency
};

enum class BotControlDomain : std::uint8_t
{
    None = 0,
    Movement = 1 << 0,
    Look = 1 << 1,
    Hotbar = 1 << 2,
    Action = 1 << 3,
    All = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3)
};

constexpr BotControlDomain operator|(BotControlDomain lhs, BotControlDomain rhs) noexcept
{
    return static_cast<BotControlDomain>(
        static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
}

constexpr BotControlDomain operator&(BotControlDomain lhs, BotControlDomain rhs) noexcept
{
    return static_cast<BotControlDomain>(
        static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
}

constexpr BotControlDomain& operator|=(BotControlDomain& lhs, BotControlDomain rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool HasControlDomain(BotControlDomain value, BotControlDomain domain) noexcept
{
    return (value & domain) != BotControlDomain::None;
}

namespace BotControlPriority
{
constexpr int Strategy = 20;
constexpr int Navigation = 40;
constexpr int Utility = 50;
constexpr int Combat = 60;
constexpr int BreakAction = 80;
constexpr int BridgeAction = 80;
constexpr int Emergency = 100;
}

struct BotControlProposal
{
    bool active = false;
    BotControlSource source = BotControlSource::Strategy;
    int priority = 0;
    BotControlDomain domains = BotControlDomain::None;
    BotControlDomain exclusiveDomains = BotControlDomain::None;
    PlayerCommand command {};
    std::string reason;
};

struct BotControlOwner
{
    bool claimed = false;
    bool exclusive = false;
    BotControlSource source = BotControlSource::Strategy;
    int priority = 0;
};

struct BotControlResolution
{
    PlayerCommand command {};
    BotControlOwner movementOwner {};
    BotControlOwner lookOwner {};
    BotControlOwner hotbarOwner {};
    BotControlOwner actionOwner {};
};

// Combines independently produced command channels without ever applying the
// result. Exactly one proposal owns each domain. Bridge/break controllers claim
// look + action exclusively; emergency control normally claims every domain.
class BotControlArbiter
{
public:
    BotControlArbiter();

    void Clear();
    void Submit(const BotControlProposal& proposal);
    void Submit(BotControlProposal&& proposal);

    BotControlResolution Resolve(const PlayerCommand& baseline) const;
    const std::vector<BotControlProposal>& Proposals() const noexcept;

private:
    const BotControlProposal* WinnerFor(BotControlDomain domain) const;

    std::vector<BotControlProposal> proposals_;
};

const char* ToString(BotControlSource source) noexcept;

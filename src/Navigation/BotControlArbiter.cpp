#include "Navigation/BotControlArbiter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
int SourceTieRank(BotControlSource source) noexcept
{
    switch (source)
    {
    case BotControlSource::Emergency: return 7;
    case BotControlSource::BridgeAction: return 6;
    case BotControlSource::BreakAction: return 5;
    case BotControlSource::Combat: return 4;
    case BotControlSource::Utility: return 3;
    case BotControlSource::Navigation: return 2;
    case BotControlSource::Strategy: return 1;
    }
    return 0;
}

bool BetterForDomain(
    const BotControlProposal& candidate,
    const BotControlProposal& incumbent,
    BotControlDomain domain) noexcept
{
    if (candidate.priority != incumbent.priority)
    {
        return candidate.priority > incumbent.priority;
    }

    const bool candidateExclusive = HasControlDomain(candidate.exclusiveDomains, domain);
    const bool incumbentExclusive = HasControlDomain(incumbent.exclusiveDomains, domain);
    if (candidateExclusive != incumbentExclusive)
    {
        return candidateExclusive;
    }

    return SourceTieRank(candidate.source) > SourceTieRank(incumbent.source);
}

void CopyMovement(PlayerCommand& output, const PlayerCommand& input)
{
    output.moveForward = input.moveForward;
    output.moveStrafe = input.moveStrafe;
    output.jump = input.jump;
    output.sprint = input.sprint;
    output.sprintTapped = input.sprintTapped;
    output.sneak = input.sneak;
}

void CopyLook(PlayerCommand& output, const PlayerCommand& input)
{
    output.aimYaw = input.aimYaw;
    output.aimPitch = input.aimPitch;
}

void CopyAction(PlayerCommand& output, const PlayerCommand& input)
{
    output.attackPressed = input.attackPressed;
    output.attackHeld = input.attackHeld;
    output.attackReleased = input.attackReleased;
    output.placePressed = input.placePressed;
    output.placeHeld = input.placeHeld;
    output.scopeHeld = input.scopeHeld;
    output.interact = input.interact;
    output.useAbility1 = input.useAbility1;
    output.useAbility2 = input.useAbility2;
    output.useUltimate = input.useUltimate;
    output.useHeal = input.useHeal;
    output.useTeleport = input.useTeleport;
    output.useDash = input.useDash;
    output.useShoot = input.useShoot;
    output.useFireball = input.useFireball;
    output.useMolotov = input.useMolotov;
    output.useAlarm = input.useAlarm;
    output.actionSeq = input.actionSeq;
    output.actionType = input.actionType;
    output.actionParamA = input.actionParamA;
    output.actionParamB = input.actionParamB;
    output.rewindTick = input.rewindTick;
}

BotControlOwner DescribeOwner(
    const BotControlProposal* proposal,
    BotControlDomain domain) noexcept
{
    BotControlOwner owner;
    if (proposal == nullptr)
    {
        return owner;
    }
    owner.claimed = true;
    owner.exclusive = HasControlDomain(proposal->exclusiveDomains, domain);
    owner.source = proposal->source;
    owner.priority = proposal->priority;
    return owner;
}

float WrapYaw(float yaw) noexcept
{
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kPi = 3.14159265358979323846f;
    yaw = std::fmod(yaw + kPi, kTwoPi);
    if (yaw < 0.0f)
    {
        yaw += kTwoPi;
    }
    return yaw - kPi;
}
}

BotControlArbiter::BotControlArbiter()
{
    proposals_.reserve(8);
}

void BotControlArbiter::Clear()
{
    proposals_.clear();
}

void BotControlArbiter::Submit(const BotControlProposal& proposal)
{
    if (proposal.active && proposal.domains != BotControlDomain::None)
    {
        proposals_.push_back(proposal);
    }
}

void BotControlArbiter::Submit(BotControlProposal&& proposal)
{
    if (proposal.active && proposal.domains != BotControlDomain::None)
    {
        proposals_.push_back(std::move(proposal));
    }
}

BotControlResolution BotControlArbiter::Resolve(const PlayerCommand& baseline) const
{
    BotControlResolution result;
    result.command = baseline;

    const BotControlProposal* movement = WinnerFor(BotControlDomain::Movement);
    const BotControlProposal* look = WinnerFor(BotControlDomain::Look);
    const BotControlProposal* hotbar = WinnerFor(BotControlDomain::Hotbar);
    const BotControlProposal* action = WinnerFor(BotControlDomain::Action);

    if (movement != nullptr)
    {
        CopyMovement(result.command, movement->command);
    }
    if (look != nullptr)
    {
        CopyLook(result.command, look->command);
    }
    if (hotbar != nullptr)
    {
        result.command.selectedSlot = hotbar->command.selectedSlot;
    }
    if (action != nullptr)
    {
        CopyAction(result.command, action->command);
    }

    result.command.moveForward = std::clamp(result.command.moveForward, -1.0f, 1.0f);
    result.command.moveStrafe = std::clamp(result.command.moveStrafe, -1.0f, 1.0f);
    result.command.aimYaw = WrapYaw(result.command.aimYaw);
    result.command.aimPitch = std::clamp(result.command.aimPitch, -1.55334306f, 1.55334306f);

    // Placement and attack share the same physical input window. A malformed
    // proposal cannot make the authoritative simulation perform both.
    if (result.command.placePressed || result.command.placeHeld)
    {
        result.command.attackPressed = false;
        result.command.attackHeld = false;
        result.command.attackReleased = false;
    }

    result.movementOwner = DescribeOwner(movement, BotControlDomain::Movement);
    result.lookOwner = DescribeOwner(look, BotControlDomain::Look);
    result.hotbarOwner = DescribeOwner(hotbar, BotControlDomain::Hotbar);
    result.actionOwner = DescribeOwner(action, BotControlDomain::Action);
    return result;
}

const std::vector<BotControlProposal>& BotControlArbiter::Proposals() const noexcept
{
    return proposals_;
}

const BotControlProposal* BotControlArbiter::WinnerFor(BotControlDomain domain) const
{
    const BotControlProposal* winner = nullptr;
    for (const BotControlProposal& proposal : proposals_)
    {
        if (!HasControlDomain(proposal.domains, domain))
        {
            continue;
        }
        if (winner == nullptr || BetterForDomain(proposal, *winner, domain))
        {
            winner = &proposal;
        }
    }
    return winner;
}

const char* ToString(BotControlSource source) noexcept
{
    switch (source)
    {
    case BotControlSource::Strategy: return "Strategy";
    case BotControlSource::Navigation: return "Navigation";
    case BotControlSource::Utility: return "Utility";
    case BotControlSource::Combat: return "Combat";
    case BotControlSource::BreakAction: return "BreakAction";
    case BotControlSource::BridgeAction: return "BridgeAction";
    case BotControlSource::Emergency: return "Emergency";
    }
    return "Unknown";
}

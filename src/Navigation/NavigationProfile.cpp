#include "Navigation/NavigationProfile.h"

#include "Inventory.h"
#include "Player.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
constexpr float kBaseMoveSpeed = 4.32f;
constexpr float kSprintMultiplier = 1.28f;
constexpr float kSneakMultiplier = 0.30f;
constexpr float kBaseJumpSpeed = 6.72f;
constexpr float kBaseGravity = 18.0f;
constexpr float kBaseFallGravity = 23.5f;

constexpr std::array<BlockType, 6> kBridgePreference {
    BlockType::WoolBlock,
    BlockType::WoodBlock,
    BlockType::TeamBlock,
    BlockType::PlankBlock,
    BlockType::StoneBlock,
    BlockType::SmoothStoneBlock
};

int FindHotbarSlot(const Inventory& inventory, BlockType blockType)
{
    const auto& hotbar = inventory.GetHotbarSlots();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        if (hotbar[slot].IsEmpty())
        {
            continue;
        }
        const std::optional<BlockType> block = ItemToBlock(hotbar[slot].type);
        if (block.has_value() && *block == blockType)
        {
            return slot;
        }
    }
    return -1;
}

float BridgeMaterialValue(BlockType type)
{
    switch (type)
    {
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        return 0.75f;
    case BlockType::WoodBlock:
    case BlockType::PlankBlock:
        return 1.0f;
    case BlockType::StoneBlock:
    case BlockType::SmoothStoneBlock:
        return 1.35f;
    case BlockType::EnergyGlassBlock:
        return 1.75f;
    case BlockType::ObsidianBlock:
        return 3.4f;
    default:
        return 1.6f;
    }
}
}

float NavigationProfile::BridgeScarcityCost(int remainingBeforePlacement) const noexcept
{
    if (remainingBeforePlacement <= 0)
    {
        return 1000000.0f;
    }

    const int total = std::max(1, availableBridgeBlocks);
    const int remainingAfter = std::max(0, remainingBeforePlacement - 1);
    const float remainingFraction = std::clamp(
        static_cast<float>(remainingAfter) / static_cast<float>(total),
        0.0f,
        1.0f);
    const float depletion = 1.0f - remainingFraction;
    const float reservePenalty = static_cast<float>(
        std::max(0, reserveBridgeBlocks - remainingAfter)) * 4.0f;
    const float materialValue = BridgeMaterialValue(preferredBridgeBlock);
    return resourceConservation
        * materialValue
        * (0.55f + depletion * depletion * 4.25f + reservePenalty);
}

float NavigationProfile::ThreatCostScale() const noexcept
{
    const float tolerance = std::clamp(riskTolerance, 0.0f, 1.0f);
    return threatWeight * (1.75f - tolerance * 1.45f);
}

float NavigationProfile::MaximumJumpRise() const noexcept
{
    if (!canJump || gravity <= 0.0f) return 0.0f;
    constexpr float dt = 1.0f / 60.0f;
    float velocity = jumpSpeed;
    float height = 0.0f;
    for (int tick = 0; tick < 180; ++tick)
    {
        velocity -= gravity * dt;
        if (velocity <= 0.0f) break;
        height += velocity * dt;
    }
    return height;
}

float NavigationProfile::EstimatedJumpHorizontalReach(
    int landingRiseBlocks,
    bool sprint) const noexcept
{
    constexpr float dt = 1.0f / 60.0f;
    const float targetSpeed = sprint ? sprintSpeed : moveSpeed;
    // Gap actions explicitly step back to a verified runway before takeoff.
    // Model that authored run-up as full sprint speed rather than the
    // first-tick standing acceleration used by ordinary hops.
    float horizontalSpeed = sprint ? targetSpeed
        : std::min(targetSpeed, groundAcceleration * dt);
    float verticalSpeed = jumpSpeed;
    float horizontal = 0.0f;
    float height = 0.0f;
    bool descending = false;
    for (int tick = 0; tick < 180; ++tick)
    {
        horizontalSpeed = std::min(targetSpeed, horizontalSpeed + airAcceleration * dt);
        verticalSpeed -= (verticalSpeed < 0.0f ? fallGravity : gravity) * dt;
        horizontal += horizontalSpeed * dt;
        height += verticalSpeed * dt;
        descending = descending || verticalSpeed < 0.0f;
        if (descending && height <= static_cast<float>(landingRiseBlocks)) break;
    }
    return horizontal * std::clamp(jumpLandingSafety, 0.5f, 1.0f);
}

bool NavigationProfile::CanExecuteGapJump(int gapBlocks) const noexcept
{
    if (!canJump || gapBlocks <= 0 || gapBlocks > maxGapJumpBlocks) return false;
    // Collision accepts a supported landing before the exact cell centre; use
    // the executor's robust landing window, not the old precision-only value.
    const float required = std::max(
        0.0f, static_cast<float>(gapBlocks + 1) - std::max(jumpLandingTolerance, 0.72f));
    return EstimatedJumpHorizontalReach(0, true) >= required;
}

NavigationProfile BuildNavigationProfile(
    const Player& player,
    const NavigationProfileSettings& settings)
{
    NavigationProfile profile;
    const Inventory& inventory = player.GetInventory();

    const float terrainMultiplier = std::clamp(settings.terrainSpeedMultiplier, 0.35f, 1.45f);
    const float speedBoost = player.GetSpeedBoostTimer() > 0.0f ? 1.22f : 1.0f;
    profile.moveSpeed = kBaseMoveSpeed * terrainMultiplier * speedBoost;
    profile.sprintSpeed = profile.moveSpeed * kSprintMultiplier;
    profile.sneakSpeed = profile.moveSpeed * kSneakMultiplier;
    profile.jumpSpeed = (kBaseJumpSpeed + (player.GetJumpBoostTimer() > 0.0f ? 1.45f : 0.0f))
        * std::clamp(settings.jumpMultiplier, 0.65f, 1.45f);
    profile.gravity = kBaseGravity * std::clamp(settings.gravityMultiplier, 0.45f, 1.35f);
    profile.fallGravity = kBaseFallGravity * std::clamp(settings.gravityMultiplier, 0.45f, 1.35f);

    profile.maxStepHeightBlocks = std::max(0, settings.maxStepHeightBlocks);
    profile.maxSafeDropBlocks = std::max(0, settings.maxSafeDropBlocks);
    profile.maxGapJumpBlocks = std::max(0, settings.maxGapJumpBlocks);
    profile.maxSprintRunBlocks = std::clamp(settings.maxSprintRunBlocks, 1, 8);
    profile.maxConsecutiveBridgeBlocks = std::max(0, settings.maxConsecutiveBridgeBlocks);

    profile.canSprint = settings.allowSprinting && profile.sprintSpeed > profile.moveSpeed + 0.01f;
    profile.canMoveDiagonally = settings.allowDiagonalMovement;
    profile.canJump = settings.allowJumping && profile.jumpSpeed > 0.01f;
    profile.canSneak = settings.allowSneaking;
    profile.canBuildStairs = settings.allowStairBuilding;
    profile.canBreakBlocks = settings.allowBlockBreaking
        && inventory.HasItem(ItemType::Pickaxe)
        && inventory.GetToolDurability() > 0;
    profile.allowBreakOwnTeamBlocks = settings.allowBreakOwnTeamBlocks;

    bool foundPreferred = false;
    // Prefer a cheap material that is already command-selectable in the hotbar.
    for (BlockType type : kBridgePreference)
    {
        const int slot = FindHotbarSlot(inventory, type);
        if (inventory.GetBlockCount(type) > 0 && slot >= 0)
        {
            profile.preferredBridgeBlock = type;
            profile.preferredBridgeHotbarSlot = slot;
            foundPreferred = true;
            break;
        }
    }
    if (!foundPreferred)
    {
        for (BlockType type : kBridgePreference)
        {
            if (inventory.GetBlockCount(type) > 0)
            {
                profile.preferredBridgeBlock = type;
                profile.preferredBridgeHotbarSlot = FindHotbarSlot(inventory, type);
                foundPreferred = true;
                break;
            }
        }
    }

    // The search spends one concrete material. Counting all build materials
    // while executing only preferredBridgeBlock would over-promise a bridge.
    profile.availableBridgeBlocks = foundPreferred
        ? std::max(0, inventory.GetBlockCount(profile.preferredBridgeBlock))
        : 0;
    profile.reserveBridgeBlocks = std::clamp(
        settings.reserveBridgeBlocks,
        0,
        profile.availableBridgeBlocks);

    profile.canPlaceBlocks = settings.allowBlockPlacement
        && foundPreferred
        && profile.availableBridgeBlocks > 0;
    profile.canBridge = settings.allowBridging
        && profile.canPlaceBlocks
        && profile.canSneak
        && profile.maxConsecutiveBridgeBlocks > 0;
    profile.canBuildStairs = profile.canBuildStairs
        && profile.canBridge
        && profile.canJump;

    profile.pickaxeLevel = std::max(0, inventory.GetToolLevel());
    profile.pickaxeDurability = std::max(0, inventory.GetToolDurability());
    profile.swordLevel = std::max(0, inventory.GetSwordLevel());
    profile.riskTolerance = std::clamp(settings.riskTolerance, 0.0f, 1.0f);
    profile.resourceConservation = std::clamp(settings.resourceConservation, 0.0f, 2.0f);
    profile.gapRunupDistanceBlocks = std::clamp(settings.gapRunupDistanceBlocks, 0.55f, 2.75f);
    profile.gapTakeoffDelaySeconds = std::clamp(settings.gapTakeoffDelaySeconds, 0.04f, 0.34f);
    profile.gapTakeoffEdgeOffsetBlocks = std::clamp(
        settings.gapTakeoffEdgeOffsetBlocks, 0.02f, 0.45f);
    profile.gapTakeoffGapScale = std::clamp(settings.gapTakeoffGapScale, 0.0f, 0.35f);
    profile.gapAirControlScale = std::clamp(settings.gapAirControlScale, 0.25f, 1.0f);
    profile.gapLandingCorrectionGain = std::clamp(settings.gapLandingCorrectionGain, 0.15f, 1.0f);
    profile.fallRiskPenalty = std::clamp(settings.fallRiskPenalty, 0.25f, 4.0f);

    return profile;
}

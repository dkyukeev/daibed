#pragma once

#include "Navigation/NavigationTypes.h"

class Player;

struct NavigationProfileSettings
{
    float terrainSpeedMultiplier = 1.0f;
    float gravityMultiplier = 1.0f;
    float jumpMultiplier = 1.0f;

    bool allowSprinting = true;
    bool allowDiagonalMovement = true;
    bool allowJumping = true;
    bool allowBlockPlacement = true;
    bool allowBlockBreaking = true;
    bool allowBridging = true;
    bool allowSneaking = true;
    bool allowStairBuilding = false;
    bool allowBreakOwnTeamBlocks = false;

    // Full-block auto-step is deliberately disabled by default. Normal DaiBed
    // players step slabs/stairs and use a real jump for a full block.
    int maxStepHeightBlocks = 0;
    int maxSafeDropBlocks = 3;
    int maxGapJumpBlocks = 3;
    int maxSprintRunBlocks = 4;
    int maxConsecutiveBridgeBlocks = 10;
    int reserveBridgeBlocks = 2;

    float riskTolerance = 0.50f;
    float resourceConservation = 0.65f;
    float gapRunupDistanceBlocks = 0.55f;
    float gapTakeoffDelaySeconds = 0.052f;
    float gapTakeoffEdgeOffsetBlocks = 0.30f;
    float gapTakeoffGapScale = 0.116f;
    float gapAirControlScale = 0.76f;
    float gapLandingCorrectionGain = 0.84f;
    float fallRiskPenalty = 0.50f;
};

struct NavigationProfile
{
    float bodyHalfWidth = 0.32f;
    float bodyHalfHeight = 0.90f;
    // Support coordinates address the centre of a full block: 0.50 block to
    // its top plus the player's 0.90 vertical half-extent.
    float bodyCenterAboveSupport = 1.40f;

    float moveSpeed = 4.32f;
    float sprintSpeed = 5.5296f;
    float sneakSpeed = 1.296f;
    float jumpSpeed = 6.72f;
    float gravity = 18.0f;
    float fallGravity = 23.5f;
    float groundAcceleration = 52.0f;
    float airAcceleration = 15.5f;
    float jumpLandingSafety = 0.86f;
    float jumpLandingTolerance = 0.30f;

    int maxStepHeightBlocks = 0;
    int maxSafeDropBlocks = 3;
    int maxGapJumpBlocks = 3;
    int maxSprintRunBlocks = 4;
    int maxConsecutiveBridgeBlocks = 10;

    bool canSprint = true;
    bool canMoveDiagonally = true;
    bool canJump = true;
    bool canPlaceBlocks = false;
    bool canBreakBlocks = true;
    bool canBridge = false;
    bool canSneak = true;
    bool canBuildStairs = false;
    bool allowBreakOwnTeamBlocks = false;

    int availableBridgeBlocks = 0;
    int reserveBridgeBlocks = 0;
    BlockType preferredBridgeBlock = BlockType::WoolBlock;
    int preferredBridgeHotbarSlot = -1;

    int pickaxeLevel = 0;
    int pickaxeDurability = 0;
    int swordLevel = 0;

    float riskTolerance = 0.50f;
    float resourceConservation = 0.65f;
    float gapRunupDistanceBlocks = 0.55f;
    float gapTakeoffDelaySeconds = 0.052f;
    float gapTakeoffEdgeOffsetBlocks = 0.30f;
    float gapTakeoffGapScale = 0.116f;
    float gapAirControlScale = 0.76f;
    float gapLandingCorrectionGain = 0.84f;
    float fallRiskPenalty = 0.50f;

    // Cost calibration. Costs are additive and intentionally expressed in
    // roughly walk-block equivalents.
    float walkCost = 1.0f;
    float stepCost = 1.20f;
    float jumpCost = 1.48f;
    float gapJumpCost = 1.80f;
    float dropCost = 1.08f;
    float bridgeMovementCost = 1.45f;
    float blockPlaceSeconds = 0.22f;
    float breakCostPerSecond = 1.35f;
    float threatWeight = 1.0f;

    float BridgeScarcityCost(int remainingBeforePlacement) const noexcept;
    float ThreatCostScale() const noexcept;
    float EstimatedJumpHorizontalReach(
        int landingRiseBlocks = 0,
        bool sprint = true) const noexcept;
    bool CanExecuteGapJump(int gapBlocks) const noexcept;
};

NavigationProfile BuildNavigationProfile(
    const Player& player,
    const NavigationProfileSettings& settings = NavigationProfileSettings {});

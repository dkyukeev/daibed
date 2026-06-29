#pragma once

#include <cstdint>

// Discrete, event-style player action carried alongside the per-tick movement
// intent (economy / inventory). Unlike the held/pressed combat flags these are
// rare one-shot requests, so they ride on a monotonic sequence number and the
// server applies each (player, actionSeq) exactly once — duplicates and resends
// are harmless. See docs/NETWORK_PREP_PLAN.md (Phase A).
enum class PlayerActionType : int
{
    None = 0,
    BuyItem = 1,       // paramA = shop choice id, paramB = repeat count
    DropItem = 2,      // paramA = inventory slot, paramB = count   (reserved)
    MoveInventory = 3, // paramA = from slot, paramB = to slot      (reserved)
    ChestTransfer = 4, // paramA = slot, paramB = direction         (reserved)
};

// One tick of player input intent, in a transport-friendly, raylib-free form.
// Humans and (later) bots produce these; the authoritative simulation consumes
// them. Movement is expressed as local axes and look as yaw/pitch so the same
// command is meaningful regardless of camera/renderer state on the receiving
// side. See docs/NETWORK_PREP_PLAN.md.
struct PlayerCommand
{
    std::uint32_t controlledPlayerId = 0;
    std::uint32_t tick = 0;

    // --- Movement / aim --------------------------------------------------
    // Movement axes in [-1, 1]. Forward is +moveForward, strafe right is
    // +moveStrafe. The server resolves these against the player's own facing.
    float moveForward = 0.0f;
    float moveStrafe = 0.0f;
    // Look/aim direction in radians.
    float aimYaw = 0.0f;
    float aimPitch = 0.0f;
    bool jump = false;
    bool sprint = false;
    bool sprintTapped = false; // sprint key tapped this tick (sprint reset)
    bool sneak = false;
    bool bridgeMode = false;   // build-assist modifier (place off the edge)

    int selectedSlot = 0; // active hotbar slot index

    // --- Attack / break (left mouse) ------------------------------------
    bool attackPressed = false;
    bool attackHeld = false;
    bool attackReleased = false;

    // --- Place / use selected item (right mouse) ------------------------
    bool placePressed = false;
    bool placeHeld = false;
    bool scopeHeld = false; // aim-down-sight (sniper / blaster)

    bool interact = false; // generic use/interact (open chest, shop, ...)

    // --- Hero abilities --------------------------------------------------
    bool useAbility1 = false;
    bool useAbility2 = false;
    bool useUltimate = false;

    // --- Utility quick-use (legacy direct-use keys) ---------------------
    bool useHeal = false;
    bool useTeleport = false;
    bool useDash = false;
    bool useShoot = false;
    bool useFireball = false;
    bool useMolotov = false;
    bool useAlarm = false;

    // --- Discrete economy / inventory action (event, not per-tick state) ----
    // actionSeq == 0 means "no request". A non-zero seq is a client-monotonic id;
    // the server applies each new seq once per player (dedupe), so the same action
    // may be (re)sent on several ticks without buying twice. See PlayerActionType.
    std::uint32_t actionSeq = 0;
    int actionType = static_cast<int>(PlayerActionType::None);
    int actionParamA = 0;
    int actionParamB = 0;
};

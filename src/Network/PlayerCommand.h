#pragma once

#include <cstdint>
#include <string>

// Discrete, event-style player action carried alongside the per-tick movement
// intent (economy / inventory). Unlike the held/pressed combat flags these are
// rare one-shot requests, so they ride on a monotonic sequence number and the
// server applies each (player, actionSeq) exactly once — duplicates and resends
// are harmless. See docs/MULTIPLAYER_TARGET_ARCHITECTURE.md.
enum class PlayerActionType : int
{
    None = 0,
    BuyItem = 1,       // paramA = shop choice id, paramB = repeat count
    DropItem = 2,      // paramA = inventory slot, paramB = count   (reserved)
    MoveInventory = 3, // legacy paramB=0 quick-move, packed paramB=exact slot move.
    ChestTransfer = 4, // legacy paramB: 0=deposit, 1=first withdraw, 2=slot withdraw.
    BlockPlace = 5,    // replicated result/event; not sent as a discrete request yet.
    BlockBreak = 6,    // replicated result/event; not sent as a discrete request yet.
    CombatEvent = 7,   // replicated combat/core feedback event.
    UtilityUse = 8,    // replicated utility item result (heal/teleport/dash/fireball/molotov/alarm).
    HeroAbility = 9,   // replicated hero ability cast/denied owner-private result.
    ProjectileLaunch = 10, // replicated bow/blaster spawn/owner feedback event.
};

enum class InventoryMoveOp : int
{
    QuickMove = 0,
    SlotToSlot = 1,
};

enum class ChestTransferOp : int
{
    DepositFirst = 0,
    WithdrawFirst = 1,
    WithdrawExact = 2,
    PlayerToChestSlot = 3,
    ChestToPlayerSlot = 4,
    ChestToChestSlot = 5,
};

constexpr int kPlayerActionPackedFlag = 0x40000000;
constexpr int kPlayerActionOpShift = 20;
constexpr int kPlayerActionSlotShift = 10;
constexpr int kPlayerActionOpMask = 0x3FF;
constexpr int kPlayerActionSlotMask = 0x3FF;
constexpr int kPlayerActionAmountMask = 0x3FF;

inline int PackPlayerActionParam(int op, int slot, int amount = 0)
{
    return kPlayerActionPackedFlag
        | ((op & kPlayerActionOpMask) << kPlayerActionOpShift)
        | ((slot & kPlayerActionSlotMask) << kPlayerActionSlotShift)
        | (amount & kPlayerActionAmountMask);
}

inline bool DecodePackedPlayerActionParam(int packed, int& op, int& slot, int& amount)
{
    if ((packed & kPlayerActionPackedFlag) == 0)
    {
        return false;
    }

    op = (packed >> kPlayerActionOpShift) & kPlayerActionOpMask;
    slot = (packed >> kPlayerActionSlotShift) & kPlayerActionSlotMask;
    amount = packed & kPlayerActionAmountMask;
    return true;
}

// One tick of player input intent, in a transport-friendly, raylib-free form.
// Humans and (later) bots produce these; the authoritative simulation consumes
// them. Movement is expressed as local axes and look as yaw/pitch so the same
// command is meaningful regardless of camera/renderer state on the receiving
// side. See docs/MULTIPLAYER_TARGET_ARCHITECTURE.md.
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

    int selectedSlot = 0; // active hotbar slot index
    int woolVariant = -1; // selected dye inside the combined wool hotbar stack.
    int arrowVariant = 0; // selected infinite-quiver arrow family.

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

    // Discrete player chat message. chatSeq is client-monotonic so command
    // batching/retries cannot duplicate a line on the authoritative server.
    std::uint32_t chatSeq = 0;
    std::string chatMessage;

    // --- Lag compensation (server-side hitbox rewind) -------------------
    // The authoritative server tick this client was actually SEEING enemies at
    // when it issued this command — i.e. the last acknowledged snapshot tick
    // minus the client's interpolation delay. For an instant-hit melee attack
    // the server rewinds every OTHER player to their position at this tick
    // before running the hit test, so a hit that looked good on the laggy
    // client also lands on the server. 0 means "no rewind" (bots, the SP
    // integrated server where client==server): the server uses live positions.
    std::uint32_t rewindTick = 0;
};

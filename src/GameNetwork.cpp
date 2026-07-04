#include "Game.h"

#include "CrashLogger.h"
#include "Network/LocalServerSession.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetworkTransport.h"
#include "Network/SnapshotVisibility.h"
#include "Platform/PreciseTimer.h"
#include "UiText.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

#define DrawText DrawTextUtf8
#define MeasureText MeasureTextUtf8

// Network preparation layer (see docs/NETWORK_PREP_PLAN.md). This translation
// unit owns the typed command/snapshot glue, the headless diagnostic modes, and
// (Phase 0.1T) the windowed GUI client. The headless server/smoke code stays
// free of raylib draw/audio/window calls; the GUI client at the bottom does use
// raylib (already available transitively via Game.h -> World.h) to render the
// replicated match.

namespace
{
constexpr std::size_t kMaxPredictionHistory = 128;
constexpr std::size_t kMaxRemoteSnapshotBuffer = 32;
constexpr float kPredictionCorrectionThreshold = 0.18f;
// Client-side error smoothing. A reconciliation correction no larger than this
// (world units) is eased out of the first-person view over the smoothing tau
// seconds instead of snapping; larger jumps (respawn / teleport / big knockback)
// still snap so the view never floats far from the true position. Purely
// cosmetic — gameplay always uses the true corrected position.
constexpr float kPredictionSmoothingMaxDistance = 1.5f;
// The smoothing time constant itself (kPredictionSmoothingTau) lives in
// Game.cpp with the merged UpdateCamera that consumes the offset.
constexpr float kReplicatedVelocityCorrectionThreshold = 1.15f;
constexpr float kReplicatedVelocityImpulseThreshold = 1.85f;
constexpr float kClientDroppedItemMagnetRadius = 2.35f;
constexpr float kClientDroppedItemMagnetAccel = 28.0f;
constexpr float kClientDroppedItemMagnetMaxSpeed = 7.0f;
// Beyond this gap the local prediction is too far off to smoothly reconcile
// (respawn relocation, teleport, knockback, or a missing prediction history
// entry) — snap straight to the authoritative position instead.
constexpr float kClientHardResyncDistance = 3.0f;
constexpr float kMatchStartingBarrierSeconds = 0.35f;
constexpr float kClientInterpolationDelaySeconds = 0.10f;
constexpr float kReconnectRespawnSeconds = 7.0f;
constexpr std::size_t kMaxRecentActionResults = 32;
constexpr std::size_t kMaxRecentWorldEvents = 16;
// The client predicts + sends input at the fixed sim tick rate (not the render
// frame rate), so the authoritative 60 Hz server applies ~one input per tick:
// full-speed movement, no stale-dropped command spam. Cap catch-up steps per
// frame (spiral-of-death guard) for very low frame rates.
constexpr int kMaxClientStepsPerFrame = 8;
constexpr int kCombatFlagRecipientAttacker = 1 << 0;
constexpr int kCombatFlagRecipientTarget = 1 << 1;
constexpr int kCombatFlagKilled = 1 << 2;
constexpr int kCombatFlagCoreHit = 1 << 3;
constexpr int kCombatFlagCoreDestroyed = 1 << 4;
constexpr int kCombatFlagHeadshot = 1 << 5;
constexpr int kCombatFlagCharged = 1 << 6;
constexpr int kCombatFlagCombo = 1 << 7;
constexpr int kCombatFlagSprintReset = 1 << 8;
constexpr int kCombatFlagAirborneTarget = 1 << 9;
constexpr int kCombatFlagVoidHit = 1 << 10;
constexpr int kCombatFlagVoidThreat = 1 << 11;

constexpr int kUtilityFlagWorldEffect = 1 << 0;
constexpr int kUtilityFlagPickupSound = 1 << 1;
constexpr int kUtilityFlagBreakBlockSound = 1 << 2;
constexpr int kUtilityFlagDeniedSound = 1 << 3;

constexpr int kHeroAbilityFlagPickupSound = 1 << 0;
constexpr int kHeroAbilityFlagBuildSound = 1 << 1;
constexpr int kHeroAbilityFlagPurchaseSound = 1 << 2;
constexpr int kHeroAbilityFlagBreakBlockSound = 1 << 3;
constexpr int kHeroAbilityFlagCoreDestroyedSound = 1 << 4;
constexpr int kHeroAbilityFlagDeniedSound = 1 << 5;
// Primary-effect visual feedback (position/color already ride the existing
// position/color fields; radius rides the new ActionResultSnapshot::radius).
// Only ONE effect round-trips per cast (the first of HeroAbilityActionResult's
// worldEffects, or the singular hasWorldEffect fields for the heroes that
// still use those) — matches how melee combat feedback also carries a single
// flash, not a list. Direction for a directed effect is NOT replicated (would
// need a new Vec3 field); the client derives it from the caster's own
// snapshot yaw instead, which is a fair approximation since directed effects
// are almost always forward-facing.
constexpr int kHeroAbilityFlagWorldEffect = 1 << 6;
constexpr int kHeroAbilityFlagDirectedEffect = 1 << 7;
constexpr int kHeroAbilityEffectKindShift = 8;
constexpr int kHeroAbilityEffectKindMask = 0x7; // 3 bits, WorldEffectKind has 8 values.
constexpr int kHeroAbilityFlagCameraShake = 1 << 11;

// Reused for both bow (critical shot) and blaster (sniper item) spawn feedback —
// the two never mix within a single ActionResultSnapshot (subjectType picks the
// projectile kind).
constexpr int kProjectileFlagPrimary = 1 << 0;

bool MoveInventoryStackToSlot(Inventory& from, int fromSlot, Inventory& to, int toSlot, int amount)
{
    if (!from.IsValidSlot(fromSlot) || !to.IsValidSlot(toSlot))
    {
        return false;
    }
    if (&from == &to && fromSlot == toSlot)
    {
        return true;
    }

    const ItemStack source = from.GetSlot(fromSlot);
    if (source.IsEmpty())
    {
        return false;
    }

    const int requested = amount > 0 ? std::min(amount, source.count) : source.count;
    if (requested <= 0)
    {
        return false;
    }

    const ItemStack destination = to.GetSlot(toSlot);
    ItemStack sourceAfter = source;
    ItemStack destinationAfter = destination;

    if (destination.IsEmpty())
    {
        destinationAfter = ItemStack { source.type, requested };
        sourceAfter.count -= requested;
        if (sourceAfter.count <= 0)
        {
            sourceAfter = ItemStack {};
        }
    }
    else if (destination.type == source.type)
    {
        const int moved = std::min(ItemMaxStack(destination.type) - destination.count, requested);
        if (moved <= 0)
        {
            return false;
        }
        destinationAfter.count += moved;
        sourceAfter.count -= moved;
        if (sourceAfter.count <= 0)
        {
            sourceAfter = ItemStack {};
        }
    }
    else if (requested == source.count)
    {
        destinationAfter = source;
        sourceAfter = destination;
    }
    else
    {
        return false;
    }

    from.SetSlot(fromSlot, sourceAfter);
    to.SetSlot(toSlot, destinationAfter);
    return true;
}

// Client-side one-shot input buffering for the fixed-step send loop: a render
// frame may produce zero, one or several fixed steps, so edge (press) inputs are
// OR-accumulated across frames and cleared once a step consumes them, while
// continuous fields take the latest value. Mirrors Game.cpp's MergePendingInput
// but scoped to the fields the network command actually carries.
void ClearClientInputEdges(PlayerInput& in)
{
    in.sprintTapped = false;
    in.attackPressed = false;
    in.attackReleased = false;
    in.placePressed = false;
    in.interactPressed = false;
    in.heroActive1Pressed = false;
    in.heroActive2Pressed = false;
    in.heroUltimatePressed = false;
    in.healPressed = false;
    in.teleportPressed = false;
    in.dashPressed = false;
    in.shootPressed = false;
    in.fireballPressed = false;
    in.molotovPressed = false;
    in.alarmPressed = false;
}

PlayerInput MergeClientInput(const PlayerInput& pending, const PlayerInput& frame)
{
    PlayerInput merged = frame; // continuous: move / held / aim / wheel / slot = latest
    merged.sprintTapped |= pending.sprintTapped;
    merged.attackPressed |= pending.attackPressed;
    merged.attackReleased |= pending.attackReleased;
    merged.placePressed |= pending.placePressed;
    merged.interactPressed |= pending.interactPressed;
    merged.heroActive1Pressed |= pending.heroActive1Pressed;
    merged.heroActive2Pressed |= pending.heroActive2Pressed;
    merged.heroUltimatePressed |= pending.heroUltimatePressed;
    merged.healPressed |= pending.healPressed;
    merged.teleportPressed |= pending.teleportPressed;
    merged.dashPressed |= pending.dashPressed;
    merged.shootPressed |= pending.shootPressed;
    merged.fireballPressed |= pending.fireballPressed;
    merged.molotovPressed |= pending.molotovPressed;
    merged.alarmPressed |= pending.alarmPressed;
    return merged;
}

const PlayerSnapshot* FindPlayerSnapshot(const MatchSnapshot& snapshot, int playerId)
{
    for (const PlayerSnapshot& entry : snapshot.players)
    {
        if (entry.playerId == playerId)
        {
            return &entry;
        }
    }
    return nullptr;
}

Vec3 LerpVec3(Vec3 a, Vec3 b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return Vec3 {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

float LengthVec3(Vec3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

float DistanceVec3(Vec3 a, Vec3 b)
{
    return LengthVec3(Vec3 { a.x - b.x, a.y - b.y, a.z - b.z });
}

Vec3 ToSnapshotVec3(Vector3 value)
{
    return Vec3 { value.x, value.y, value.z };
}

Vector3 ToRaylibVector3(Vec3 value)
{
    return Vector3 { value.x, value.y, value.z };
}

Vector3 ClientPickupTargetFor(const Player& player)
{
    const Vector3 pos = player.GetPosition();
    return Vector3 { pos.x, pos.y + 0.58f, pos.z };
}

// Status effects have no spawn moment to hang a NextXId()-style counter on —
// they're synthesized fresh every BuildNetworkSnapshot call straight from
// Player timers / RadonBurn / KonvoyIntruderMark (a status "exists" for a tick
// purely because its remaining/timer is > 0, then can vanish and reappear).
// So the stable id has to be a pure function of (type, target, owner) instead
// of an assigned-once value, the same way LikhoDisguise derives its id from
// ownerPlayerId: the SAME conceptual effect (e.g. "player 3's shield", or
// "player 5's Radon burn from player 2") then keeps the SAME id every tick
// it's present, fixing the same index-shift bug as the other entity types
// (see NetworkSnapshot.h). Player/owner ids are small non-negative
// match-lifetime ids (-1 = none); +1 offset packs -1 safely.
int StatusEffectStableId(StatusEffectType type, int targetPlayerId, int ownerPlayerId)
{
    constexpr int kSlotRange = 128; // generous headroom above any realistic player count.
    const int targetSlot = targetPlayerId + 1;
    const int ownerSlot = ownerPlayerId + 1;
    return ((static_cast<int>(type) * kSlotRange + targetSlot) * kSlotRange + ownerSlot) + 1;
}

Vec3 AdvanceVec3(Vec3 position, Vec3 velocity, float dt)
{
    return Vec3 {
        position.x + velocity.x * dt,
        position.y + velocity.y * dt,
        position.z + velocity.z * dt
    };
}

const CoreSnapshot* FindCoreSnapshot(const MatchSnapshot& snapshot, int teamId)
{
    for (const CoreSnapshot& entry : snapshot.cores)
    {
        if (entry.teamId == teamId)
        {
            return &entry;
        }
    }
    return nullptr;
}

bool HasMatchingProjectileSnapshot(const MatchSnapshot& previous, const ProjectileSnapshot& current, float dt)
{
    for (const ProjectileSnapshot& old : previous.projectiles)
    {
        if (old.kind != current.kind || old.ownerPlayerId != current.ownerPlayerId)
        {
            continue;
        }
        const Vec3 predicted = AdvanceVec3(old.position, old.velocity, dt);
        const float tolerance = std::max(1.25f, 0.45f + LengthVec3(old.velocity) * std::max(0.0f, dt) * 0.65f);
        if (DistanceVec3(predicted, current.position) <= tolerance)
        {
            return true;
        }
    }
    return false;
}

bool HasMatchingHazardSnapshot(const MatchSnapshot& previous, const HazardZoneSnapshot& current)
{
    for (const HazardZoneSnapshot& old : previous.hazardZones)
    {
        if (old.ownerPlayerId == current.ownerPlayerId
            && old.blueFire == current.blueFire
            && DistanceVec3(old.position, current.position) <= 0.85f)
        {
            return true;
        }
    }
    return false;
}

bool HasMatchingExplosiveSnapshot(const MatchSnapshot& previous, const ExplosiveSnapshot& current)
{
    for (const ExplosiveSnapshot& old : previous.explosives)
    {
        if (old.ownerPlayerId == current.ownerPlayerId
            && DistanceVec3(old.position, current.position) <= 0.85f)
        {
            return true;
        }
    }
    return false;
}

bool HasMatchingDeviceSnapshot(const MatchSnapshot& previous, const HeroDeviceSnapshot& current)
{
    for (const HeroDeviceSnapshot& old : previous.heroDevices)
    {
        if (old.type == current.type
            && old.ownerPlayerId == current.ownerPlayerId
            && old.targetPlayerId == current.targetPlayerId
            && DistanceVec3(old.position, current.position) <= 0.95f)
        {
            return true;
        }
    }
    return false;
}

float LerpAngle(float a, float b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    float delta = b - a;
    while (delta > PI) delta -= 2.0f * PI;
    while (delta < -PI) delta += 2.0f * PI;
    return a + delta * t;
}

// The aim direction a network player is looking along, from its command's yaw +
// pitch (matches CameraController::LookDirection — the server has no camera).
Vector3 AimDirectionFromCommand(const PlayerCommand& command)
{
    const float cosPitch = std::cos(command.aimPitch);
    return Vector3 {
        std::sin(command.aimYaw) * cosPitch,
        std::sin(command.aimPitch),
        -std::cos(command.aimYaw) * cosPitch
    };
}

Vector3 SnapshotVecToRay(Vec3 v)
{
    return Vector3 { v.x, v.y, v.z };
}

ProjectileKind ProjectileKindFromSnapshot(int kind)
{
    switch (kind)
    {
    case static_cast<int>(ProjectileKind::Fireball):
        return ProjectileKind::Fireball;
    case static_cast<int>(ProjectileKind::Molotov):
        return ProjectileKind::Molotov;
    case static_cast<int>(ProjectileKind::Blaster):
        return ProjectileKind::Blaster;
    case static_cast<int>(ProjectileKind::Arrow):
    default:
        return ProjectileKind::Arrow;
    }
}

void ApplyProjectileDefaults(EnergyProjectile& projectile)
{
    const ProjectileTuning* tuning = &kArrowTuning;
    if (projectile.kind == ProjectileKind::Fireball)
    {
        tuning = &kFireballTuning;
    }
    else if (projectile.kind == ProjectileKind::Molotov)
    {
        tuning = &kMolotovTuning;
    }

    projectile.damage = tuning->damage;
    projectile.radius = tuning->radius;
    projectile.explosionRadius = tuning->explosionRadius;
    projectile.gravity = tuning->gravity;
    if (projectile.kind == ProjectileKind::Blaster)
    {
        projectile.damage = kBlasterTuning.baseDamage;
        projectile.radius = 0.24f;
        projectile.gravity = kBlasterTuning.gravity;
        projectile.airDragPerTick = 0.996f;
        projectile.affectedByDrag = true;
        projectile.speedBasedDamage = true;
    }
}

const EnergyProjectile* FindMatchingVisualProjectile(
    const std::vector<EnergyProjectile>& previousProjectiles,
    const ProjectileSnapshot& snapshot)
{
    // Prefer the real stable spawn id (EnergyProjectile::id / ProjectileSnapshot::id):
    // exact identity, no ambiguity even when two same-kind same-owner projectiles
    // are close together (e.g. a fast bow volley), which the distance heuristic
    // below cannot tell apart. Falls back to the heuristic only for entries
    // without a valid id (id<=0 — legacy/synthetic sources).
    if (snapshot.id > 0)
    {
        for (const EnergyProjectile& old : previousProjectiles)
        {
            if (old.id == snapshot.id)
            {
                return &old;
            }
        }
        return nullptr;
    }

    const EnergyProjectile* best = nullptr;
    float bestDistance = 999999.0f;
    for (const EnergyProjectile& old : previousProjectiles)
    {
        if (static_cast<int>(old.kind) != snapshot.kind || old.ownerId != snapshot.ownerPlayerId)
        {
            continue;
        }
        const Vec3 oldPosition { old.position.x, old.position.y, old.position.z };
        const float distance = DistanceVec3(oldPosition, snapshot.position);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = &old;
        }
    }
    if (best == nullptr)
    {
        return nullptr;
    }
    const float speed = std::sqrt(
        best->velocity.x * best->velocity.x
        + best->velocity.y * best->velocity.y
        + best->velocity.z * best->velocity.z);
    const float tolerance = std::max(3.0f, 0.4f + speed * 0.12f);
    return bestDistance <= tolerance ? best : nullptr;
}

const char* HostTeamName(int teamId)
{
    switch (teamId)
    {
    case 0:
        return "Red";
    case 1:
        return "Blue";
    case 2:
        return "Green";
    case 3:
        return "Yellow";
    default:
        return "Auto";
    }
}

std::string HostHeroName(int heroIndex)
{
    if (heroIndex < 0 || heroIndex >= HeroSystem::kHeroCount)
    {
        return "Авто";
    }
    return HeroSystem::GetDefinitionByIndex(heroIndex).name;
}

std::string FormatLobbyRosterForHost(const LobbySnapshot& lobby)
{
    if (lobby.players.empty())
    {
        return "empty";
    }

    std::string roster;
    for (const LobbyPlayerState& player : lobby.players)
    {
        if (!roster.empty())
        {
            roster += "; ";
        }
        const std::string name = player.playerName.empty()
            ? "Игрок " + std::to_string(player.clientId)
            : player.playerName;
        roster += name;
        if (player.clientId == lobby.hostClientId)
        {
            roster += "(хост)";
        }
        roster += player.ready ? "[готов " : "[ждет ";
        roster += HostTeamName(player.selectedTeam);
        roster += '/';
        roster += HostHeroName(player.selectedHero);
        roster += ']';
    }
    return roster;
}
} // namespace

void Game::SetNetworkMode(NetworkMode mode)
{
    networkMode_ = mode;
}

NetworkMode Game::GetNetworkMode() const
{
    return networkMode_;
}

void Game::SetServerConfig(const ServerConfig& config)
{
    serverConfig_ = config;
}

const ServerConfig& Game::GetServerConfig() const
{
    return serverConfig_;
}

PlayerCommand Game::BuildLocalPlayerCommand() const
{
    PlayerCommand command;
    command.controlledPlayerId = static_cast<std::uint32_t>(localPlayerId_);
    command.tick = matchSimulation_.CurrentTick();
    command.moveForward = currentInput_.move.z;
    command.moveStrafe = currentInput_.move.x;
    command.aimYaw = cameraController_.GetYaw();
    command.aimPitch = cameraController_.GetPitch();
    command.jump = currentInput_.jumpHeld;
    command.sprint = currentInput_.sprint;
    command.sprintTapped = currentInput_.sprintTapped;
    command.sneak = currentInput_.sneak;
    command.bridgeMode = currentInput_.bridgeMode;
    command.selectedSlot = selectedHotbarSlot_;
    command.attackPressed = currentInput_.attackPressed;
    command.attackHeld = currentInput_.attackHeld;
    command.attackReleased = currentInput_.attackReleased;
    command.placePressed = currentInput_.placePressed;
    command.placeHeld = currentInput_.placeHeld;
    command.scopeHeld = currentInput_.scopeHeld;
    command.interact = currentInput_.interactPressed;
    command.useAbility1 = currentInput_.heroActive1Pressed;
    command.useAbility2 = currentInput_.heroActive2Pressed;
    command.useUltimate = currentInput_.heroUltimatePressed;
    command.useHeal = currentInput_.healPressed;
    command.useTeleport = currentInput_.teleportPressed;
    command.useDash = currentInput_.dashPressed;
    command.useShoot = currentInput_.shootPressed;
    command.useFireball = currentInput_.fireballPressed;
    command.useMolotov = currentInput_.molotovPressed;
    command.useAlarm = currentInput_.alarmPressed;
    // Discrete economy/inventory action queued by the client UI (if any). seq is
    // zero unless a pending action is waiting, so an idle command carries none.
    command.actionSeq = pendingEconomyActionType_ != PlayerActionType::None
        ? clientEconomyActionSeq_ : 0;
    command.actionType = static_cast<int>(pendingEconomyActionType_);
    command.actionParamA = pendingEconomyActionParamA_;
    command.actionParamB = pendingEconomyActionParamB_;

    // Client-side UI gates, applied where the command is BUILT so every
    // consumer (SP integrated server, MP client prediction + send, previews)
    // agrees: while the shop/inventory overlay owns the mouse, combat/place
    // input is neutral — the same rule the old direct-path shopOpen_/
    // inventoryOpen_ checks enforced before Phase 6 routed actions through
    // the command.
    if (shopOpen_ || inventoryOpen_)
    {
        command.attackPressed = false;
        command.attackHeld = false;
        command.attackReleased = false;
        command.placePressed = false;
        command.placeHeld = false;
        command.scopeHeld = false;
    }
    // The ultimate key doubles as the shop-open key inside the shop zone; the
    // frame input path skips ALL ability handling on such a press (see
    // keepUltimateKeyForShop in HandleInput) — mirror it on the wire.
    if (command.useUltimate && IsLocalPlayerInShopZone())
    {
        command.useAbility1 = false;
        command.useAbility2 = false;
        command.useUltimate = false;
    }

    // Lag-compensation target tick: the authoritative tick this client is
    // actually displaying enemies at — the freshest server tick it knows
    // (lastAuthoritativeTick_) minus its own interpolation delay in ticks. The
    // server rewinds other players here for the melee hit test. Only a real
    // remote client has this lag; the SP integrated server leaves it 0 (its
    // "client" IS the server, so live positions already match what it sees).
    if (networkMode_ == NetworkMode::LocalClient && lastAuthoritativeTick_ > 0)
    {
        const float fixedDt = matchSimulation_.FixedDeltaSeconds();
        const std::uint32_t interpTicks = fixedDt > 0.0f
            ? static_cast<std::uint32_t>(networkInterpolationDelaySeconds_ / fixedDt + 0.5f)
            : 0;
        command.rewindTick = lastAuthoritativeTick_ > interpTicks
            ? lastAuthoritativeTick_ - interpTicks
            : 0;
    }
    return command;
}

MatchSnapshot Game::BuildNetworkSnapshot() const
{
    MatchSnapshot snapshot;
    snapshot.tick = matchSimulation_.CurrentTick();
    snapshot.matchTime = matchSimulation_.MatchTimeSeconds();
    // Phase + winner are authoritative from MatchSimulation now.
    snapshot.phase = matchSimulation_.Phase();
    snapshot.winnerTeamId = matchSimulation_.WinnerTeamId();
    snapshot.actionResults = recentActionResults_;
    snapshot.worldEvents = recentWorldEvents_;

    // Players come from MatchSimulation (authoritative access point, Phase 6A).
    const std::vector<Player>& players = matchSimulation_.Players();
    snapshot.players.reserve(players.size());
    for (const Player& player : players)
    {
        PlayerSnapshot entry;
        entry.playerId = player.GetId();
        entry.playerName = player.GetName();
        entry.teamId = player.GetTeamId();
        entry.heroId = static_cast<int>(player.GetHeroId());
        // Player stores spatial state as Vec3; take it directly (no conversion).
        entry.position = player.GetPositionVec3();
        entry.velocity = player.GetVelocityVec3();
        entry.yaw = player.GetYaw();
        entry.health = player.GetHealth();
        entry.maxHealth = player.GetMaxHealth();
        entry.alive = player.IsAlive();
        entry.eliminated = player.IsEliminated();
        entry.respawnTimer = player.GetRespawnTimer();
        const PlayerControlKind controlKind = ControlKindForPlayer(player);
        entry.selectedSlot = IsLocallyPredicted(controlKind)
            ? selectedHotbarSlot_
            : (IsHumanControlled(controlKind) ? player.GetSelectedSlot() : 0);

        // Owner-private inventory: the FULL snapshot carries it for everyone; the
        // per-client filter strips it for non-recipients (see SnapshotVisibility).
        const Inventory& inventory = player.GetInventory();
        entry.inventory.present = true;
        entry.inventory.resources[0] = inventory.GetResource(ResourceType::Iron);
        entry.inventory.resources[1] = inventory.GetResource(ResourceType::Gold);
        entry.inventory.resources[2] = inventory.GetResource(ResourceType::Crystal);
        const std::array<ItemStack, kHotbarSlotCount>& hotbar = inventory.GetHotbarSlots();
        entry.inventory.hotbar.reserve(hotbar.size());
        for (const ItemStack& slot : hotbar)
        {
            entry.inventory.hotbar.push_back(
                ItemStackSnapshot { static_cast<int>(slot.type), slot.count });
        }
        const std::array<ItemStack, kMainInventorySlotCount>& mainSlots = inventory.GetMainSlots();
        entry.inventory.main.reserve(mainSlots.size());
        for (const ItemStack& slot : mainSlots)
        {
            entry.inventory.main.push_back(
                ItemStackSnapshot { static_cast<int>(slot.type), slot.count });
        }

        // Hidden-enemy identity: while a Likho's ultimate disguise is active, an
        // enemy recipient should see the impersonated team/hero, not the real one.
        // Record both here; the filter swaps for enemies and clears the hint.
        const HeroRuntimeState& heroState = player.GetHeroState();
        // Owner-private ability HUD state — same present/strip contract as
        // inventory above. Without this a network client's own ability HUD is
        // permanently stuck on "ready" (HeroRuntimeState is never predicted
        // locally for a server-driven player).
        entry.abilityHud.present = true;
        entry.abilityHud.active1Cooldown = heroState.active1.cooldownRemaining;
        entry.abilityHud.active1ActiveTimer = heroState.active1.activeTimer;
        entry.abilityHud.active2Cooldown = heroState.active2.cooldownRemaining;
        entry.abilityHud.active2ActiveTimer = heroState.active2.activeTimer;
        entry.abilityHud.ultimateCooldown = heroState.ultimate.cooldownRemaining;
        entry.abilityHud.ultimateActiveTimer = heroState.ultimate.activeTimer;
        entry.abilityHud.ultimateCharge = heroState.ultimateCharge;
        entry.abilityHud.ultimatePrimed = heroState.ultimatePrimed;
        entry.abilityHud.bowDrawTimer = player.GetBowDrawTimer();
        entry.abilityHud.blasterState = static_cast<int>(player.GetBlasterState());
        entry.abilityHud.blasterLoadTimer = player.GetBlasterLoadTimer();
        // Public animation pose, computed authoritatively each tick on the server
        // (UpdateTimers + attack/ability casts). The client adopts it so remote and
        // own players animate (walk/run/jump/attack/cast/death) instead of freezing.
        entry.animationState = static_cast<int>(heroState.animationState);
        entry.animationTimer = heroState.animationTimer;
        entry.animationDuration = heroState.animationDuration;
        if (heroState.ultimate.active && heroState.likhoDisguiseTeamId >= 0)
        {
            entry.disguiseTeamId = heroState.likhoDisguiseTeamId;
            entry.disguiseHeroId = static_cast<int>(heroState.likhoDisguiseHeroId);
        }

        snapshot.players.push_back(entry);
    }

    snapshot.matchScores.reserve(playerScores_.size());
    for (const PlayerMatchScore& score : playerScores_)
    {
        snapshot.matchScores.push_back(PlayerScoreSnapshot {
            score.playerId,
            score.kills,
            score.deaths,
            score.finalDeaths,
            score.coreDamage,
            score.coresDestroyed });
    }

    const std::vector<EnergyCore>& cores = matchSimulation_.Cores();
    snapshot.cores.reserve(cores.size());
    for (const EnergyCore& core : cores)
    {
        CoreSnapshot entry;
        entry.teamId = core.GetTeamId();
        entry.health = core.GetHealth();
        entry.maxHealth = core.GetMaxHealth();
        entry.alive = core.IsAlive();
        snapshot.cores.push_back(entry);
    }

    snapshot.teamChests.reserve(teams_.size());
    for (const Team& team : teams_)
    {
        if (team.id < 0 || team.id >= static_cast<int>(teamChests_.size()))
        {
            continue;
        }
        const Inventory& chest = teamChests_[team.id];
        TeamChestSnapshot entry;
        entry.teamId = team.id;
        entry.resources[0] = chest.GetResource(ResourceType::Iron);
        entry.resources[1] = chest.GetResource(ResourceType::Gold);
        entry.resources[2] = chest.GetResource(ResourceType::Crystal);
        entry.slots.reserve(kInventorySlotCount);
        for (int slot = 0; slot < kInventorySlotCount; ++slot)
        {
            const ItemStack stack = chest.GetSlot(slot);
            entry.slots.push_back(ItemStackSnapshot { static_cast<int>(stack.type), stack.count });
        }
        snapshot.teamChests.push_back(entry);
    }

    // Generators / pickups / dropped items are owned by MatchSimulation
    // (raylib-free Vec3 positions), so they replicate directly without
    // conversion. No visibility filtering yet — every entry is included.
    const std::vector<Generator>& generators = matchSimulation_.Generators();
    snapshot.generators.reserve(generators.size());
    for (const Generator& generator : generators)
    {
        GeneratorSnapshot entry;
        entry.resourceType = static_cast<int>(generator.GetType());
        entry.teamId = generator.GetTeamId();
        entry.position = generator.GetPosition();
        snapshot.generators.push_back(entry);
    }

    const std::vector<ResourcePickup>& pickups = matchSimulation_.Pickups();
    snapshot.pickups.reserve(pickups.size());
    for (const ResourcePickup& pickup : pickups)
    {
        if (pickup.collected)
        {
            continue;
        }
        PickupSnapshot entry;
        entry.resourceType = static_cast<int>(pickup.type);
        entry.amount = pickup.amount;
        entry.position = pickup.position;
        snapshot.pickups.push_back(entry);
    }

    const std::vector<DroppedItem>& droppedItems = matchSimulation_.DroppedItems();
    snapshot.droppedItems.reserve(droppedItems.size());
    for (const DroppedItem& dropped : droppedItems)
    {
        if (dropped.collected)
        {
            continue;
        }
        DroppedItemSnapshot entry;
        entry.id = dropped.id;
        entry.itemType = static_cast<int>(dropped.stack.type);
        entry.count = dropped.stack.count;
        entry.position = dropped.position;
        entry.velocity = dropped.velocity;
        entry.ownerPlayerId = dropped.ownerPlayerId;
        entry.ownerPickupDelay = dropped.ownerPickupDelay;
        entry.lifetime = dropped.lifetime;
        entry.age = dropped.age;
        snapshot.droppedItems.push_back(entry);
    }

    snapshot.blockDeltas = matchSimulation_.BlockDeltas();

    // --- Dynamic gameplay entities -------------------------------------------
    // These still live in Game and some use raylib Vector3, so convert spatial
    // fields to Vec3 at this boundary. Ids are the per-collection index for now
    // (no stable spawn-ids in the sim yet — see NetworkSnapshot.h). Only public
    // state is copied; AI/nav/internal timers stay out.
    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };
    const auto playerPos = [this](int playerId) -> Vec3
    {
        const Player* player = matchSimulation_.GetPlayer(playerId);
        return player != nullptr ? player->GetPositionVec3() : Vec3 {};
    };

    snapshot.projectiles.reserve(projectiles_.size());
    for (const EnergyProjectile& projectile : projectiles_)
    {
        ProjectileSnapshot entry;
        // Stable per-spawn id (EnergyProjectile::id), NOT the vector index — an
        // earlier projectile expiring must not reassign a surviving one's id.
        entry.id = projectile.id;
        entry.kind = static_cast<int>(projectile.kind);
        entry.position = toVec3(projectile.position);
        entry.velocity = toVec3(projectile.velocity);
        entry.ownerPlayerId = projectile.ownerId;
        entry.ownerTeamId = projectile.ownerTeamId;
        entry.remainingLifetime = projectile.lifetime;
        entry.fireZone = projectile.fireZone;
        snapshot.projectiles.push_back(entry);
    }

    snapshot.explosives.reserve(timedExplosions_.size());
    for (const TimedExplosion& explosive : timedExplosions_)
    {
        ExplosiveSnapshot entry;
        // Stable per-spawn id (TimedExplosion::id), NOT the vector index — same
        // fix as projectiles above (see NetworkSnapshot.h).
        entry.id = explosive.id;
        entry.position = Vec3 {
            static_cast<float>(explosive.block.x),
            static_cast<float>(explosive.block.y),
            static_cast<float>(explosive.block.z)
        };
        entry.ownerPlayerId = explosive.ownerPlayerId;
        entry.ownerTeamId = explosive.ownerTeamId;
        entry.remainingTimer = explosive.timer;
        entry.radius = explosive.radius;
        snapshot.explosives.push_back(entry);
    }

    snapshot.hazardZones.reserve(hazardZones_.size());
    for (const HazardZone& zone : hazardZones_)
    {
        HazardZoneSnapshot entry;
        // Stable per-spawn id (HazardZone::id), NOT the vector index — same fix
        // as projectiles above (see NetworkSnapshot.h).
        entry.id = zone.id;
        entry.position = toVec3(zone.position);
        entry.ownerPlayerId = zone.ownerPlayerId;
        entry.ownerTeamId = zone.ownerTeamId;
        entry.remainingLifetime = zone.lifetime;
        entry.radius = zone.radius;
        entry.blueFire = zone.blueFire;
        snapshot.hazardZones.push_back(entry);
    }

    // Hero devices funnel into one section with a device-type tag. Free-standing
    // devices carry their own position; tether/bleed/disguise markers ride on a
    // target/owner player, so resolve that position via the player lookup.
    // Stable per-spawn id (each source struct's own `id`, set once by
    // Game::NextHeroDeviceId() at creation — shared across all 7 device
    // structs' id-space), NOT a per-build counter — same fix as
    // projectiles/explosives/hazard zones above (see NetworkSnapshot.h).
    const auto addDevice = [&snapshot](HeroDeviceSnapshot entry)
    {
        snapshot.heroDevices.push_back(entry);
    };
    for (const BromVacuumBot& bot : bromVacuumBots_)
    {
        HeroDeviceSnapshot entry;
        entry.id = bot.id;
        entry.type = HeroDeviceType::BromVacuumBot;
        entry.position = toVec3(bot.position);
        entry.ownerPlayerId = bot.ownerPlayerId;
        entry.ownerTeamId = bot.ownerTeamId;
        entry.remainingLifetime = bot.lifetime;
        entry.health = bot.health;
        addDevice(entry);
    }
    for (const BromTurretDrone& drone : bromTurretDrones_)
    {
        HeroDeviceSnapshot entry;
        entry.id = drone.id;
        entry.type = HeroDeviceType::BromTurretDrone;
        entry.position = toVec3(drone.position);
        entry.ownerPlayerId = drone.ownerPlayerId;
        entry.ownerTeamId = drone.ownerTeamId;
        entry.remainingLifetime = drone.lifetime;
        entry.health = drone.health;
        addDevice(entry);
    }
    for (const KonvoyTrap& trap : konvoyTraps_)
    {
        HeroDeviceSnapshot entry;
        entry.id = trap.id;
        entry.type = HeroDeviceType::KonvoyTrap;
        entry.position = toVec3(trap.position);
        entry.ownerPlayerId = trap.ownerPlayerId;
        entry.ownerTeamId = trap.ownerTeamId;
        entry.remainingLifetime = trap.lifetime;
        entry.health = trap.health;
        // A buried trap is hidden from enemies until it triggers.
        entry.visibility = SnapshotVisibility::OwnerTeam;
        addDevice(entry);
    }
    for (const KonvoyTether& tether : konvoyTethers_)
    {
        HeroDeviceSnapshot entry;
        entry.id = tether.id;
        entry.type = HeroDeviceType::KonvoyTether;
        entry.position = playerPos(tether.ownerPlayerId);
        entry.ownerPlayerId = tether.ownerPlayerId;
        entry.ownerTeamId = tether.ownerTeamId;
        entry.targetPlayerId = tether.targetPlayerId;
        entry.remainingLifetime = tether.lifetime;
        entry.visibility = SnapshotVisibility::OwnerTeam;
        addDevice(entry);
    }
    for (const KonvoyDome& dome : konvoyDomes_)
    {
        HeroDeviceSnapshot entry;
        entry.id = dome.id;
        entry.type = HeroDeviceType::KonvoyDome;
        entry.position = toVec3(dome.position);
        entry.ownerPlayerId = dome.ownerPlayerId;
        entry.ownerTeamId = dome.ownerTeamId;
        entry.remainingLifetime = dome.lifetime;
        entry.health = dome.health;
        addDevice(entry);
    }
    for (const SvidetelEcho& echo : svidetelEchoes_)
    {
        HeroDeviceSnapshot entry;
        entry.id = echo.id;
        entry.type = HeroDeviceType::SvidetelEcho;
        entry.position = toVec3(echo.position);
        entry.ownerPlayerId = echo.ownerPlayerId;
        entry.ownerTeamId = echo.ownerTeamId;
        entry.remainingLifetime = echo.lifetime;
        entry.health = echo.health;
        addDevice(entry);
    }
    for (const LikhoBleed& bleed : likhoBleeds_)
    {
        HeroDeviceSnapshot entry;
        entry.id = bleed.id;
        entry.type = HeroDeviceType::LikhoBleed;
        entry.position = playerPos(bleed.targetPlayerId);
        entry.ownerPlayerId = bleed.ownerPlayerId;
        entry.ownerTeamId = bleed.ownerTeamId;
        entry.targetPlayerId = bleed.targetPlayerId;
        entry.remainingLifetime = bleed.lifetime;
        entry.health = bleed.stacks; // bleed stacks carried in the HP slot.
        // Bleed marker is shown to the owner team; enemies feel it without the cue.
        entry.visibility = SnapshotVisibility::OwnerTeam;
        addDevice(entry);
    }
    // Likho disguise marker: only while the ultimate disguise is actually active.
    for (const Player& player : matchSimulation_.Players())
    {
        const HeroRuntimeState& heroState = player.GetHeroState();
        if (heroState.ultimate.active && heroState.likhoDisguiseTeamId >= 0)
        {
            HeroDeviceSnapshot entry;
            // LikhoDisguise has no persistent spawned object to hang a
            // NextHeroDeviceId() call on — it's synthesized fresh from player
            // state every snapshot build. At most one is active per player, so
            // derive a stable id from ownerPlayerId directly, offset well clear
            // of the shared counter's practical range (never reaches anywhere
            // near a billion spawns in one match) so the two id spaces can't collide.
            entry.id = 1000000000 + player.GetId();
            entry.type = HeroDeviceType::LikhoDisguise;
            entry.position = player.GetPositionVec3();
            entry.ownerPlayerId = player.GetId();
            entry.ownerTeamId = player.GetTeamId();
            entry.targetPlayerId = heroState.likhoDisguisePlayerId;
            entry.remainingLifetime = heroState.ultimate.activeTimer;
            // Allies should see through the disguise; enemies see the impersonation.
            entry.visibility = SnapshotVisibility::OwnerTeam;
            addDevice(entry);
        }
    }

    // Per-player status effects: buffs/debuffs from the player's own timers plus
    // applied damage-over-time/markers (radon burn, konvoy intruder mark). id is
    // a pure function of (type, target, owner) — see StatusEffectStableId above.
    const auto addStatus = [&snapshot](StatusEffectSnapshot entry)
    {
        entry.id = StatusEffectStableId(entry.type, entry.targetPlayerId, entry.ownerPlayerId);
        snapshot.statusEffects.push_back(entry);
    };
    for (const Player& player : matchSimulation_.Players())
    {
        if (!player.IsAlive())
        {
            continue;
        }
        const auto addPlayerTimer = [&](StatusEffectType type, float remaining, int amount)
        {
            if (remaining <= 0.0f)
            {
                return;
            }
            StatusEffectSnapshot entry;
            entry.type = type;
            entry.position = player.GetPositionVec3();
            entry.targetPlayerId = player.GetId();
            entry.ownerTeamId = player.GetTeamId();
            entry.remaining = remaining;
            entry.amount = amount;
            addStatus(entry);
        };
        addPlayerTimer(StatusEffectType::SpeedBoost, player.GetSpeedBoostTimer(), 0);
        addPlayerTimer(StatusEffectType::JumpBoost, player.GetJumpBoostTimer(), 0);
        addPlayerTimer(StatusEffectType::Shield, player.GetShieldTimer(), 0);
        addPlayerTimer(StatusEffectType::Invulnerability, player.GetInvulnerabilityTimer(), 0);
        addPlayerTimer(StatusEffectType::ControlDebuff, player.GetControlDebuffTimer(), 0);

        const HeroRuntimeState& heroState = player.GetHeroState();
        if (heroState.radonProtected)
        {
            StatusEffectSnapshot entry;
            entry.type = StatusEffectType::RadonProtected;
            entry.position = player.GetPositionVec3();
            entry.targetPlayerId = player.GetId();
            entry.ownerTeamId = player.GetTeamId();
            addStatus(entry);
        }
        if (heroState.radonOverloaded)
        {
            StatusEffectSnapshot entry;
            entry.type = StatusEffectType::RadonOverloaded;
            entry.position = player.GetPositionVec3();
            entry.targetPlayerId = player.GetId();
            entry.ownerTeamId = player.GetTeamId();
            addStatus(entry);
        }
    }
    for (const RadonBurn& burn : radonBurns_)
    {
        StatusEffectSnapshot entry;
        entry.type = StatusEffectType::RadonBurn;
        entry.position = playerPos(burn.targetPlayerId);
        entry.targetPlayerId = burn.targetPlayerId;
        entry.ownerPlayerId = burn.ownerPlayerId;
        entry.ownerTeamId = burn.ownerTeamId;
        entry.remaining = burn.lifetime;
        entry.amount = burn.blueFire ? 1 : 0;
        // The applier's team can see they set a target alight.
        entry.visibility = SnapshotVisibility::OwnerTeam;
        addStatus(entry);
    }
    for (const KonvoyIntruderMark& mark : konvoyIntruderMarks_)
    {
        StatusEffectSnapshot entry;
        entry.type = StatusEffectType::KonvoyMark;
        entry.position = playerPos(mark.targetPlayerId);
        entry.targetPlayerId = mark.targetPlayerId;
        entry.ownerPlayerId = mark.ownerPlayerId;
        // The mark has no own team field; resolve it from the applier so the
        // OwnerTeam visibility filter can match the Konvoy's team.
        const Player* markOwner = matchSimulation_.GetPlayer(mark.ownerPlayerId);
        entry.ownerTeamId = markOwner != nullptr ? markOwner->GetTeamId() : -1;
        entry.remaining = mark.markedTimer;
        entry.visibility = SnapshotVisibility::OwnerTeam;
        addStatus(entry);
    }

    return snapshot;
}

MatchSnapshot Game::BuildNetworkSnapshotForClient(int clientPlayerId) const
{
    // The full snapshot is the server/debug view; the filter derives what this
    // specific client is allowed to see (see SnapshotVisibility.h).
    return FilterSnapshotForClient(BuildNetworkSnapshot(), clientPlayerId);
}

void Game::StorePredictedLocalCommand(const PlayerCommand& command, const Player& player)
{
    if (networkMode_ == NetworkMode::LocalSinglePlayer
        || (localPlayerServerDriven_ && networkMode_ != NetworkMode::LocalClient)
        || static_cast<int>(command.controlledPlayerId) != localPlayerId_)
    {
        return;
    }

    PredictedCommandState state;
    state.command = command;
    state.predictedPosition = player.GetPositionVec3();
    state.predictedVelocity = player.GetVelocityVec3();
    state.predictedYaw = player.GetYaw();

    auto existing = std::find_if(predictionHistory_.begin(), predictionHistory_.end(),
        [&command](const PredictedCommandState& entry)
        {
            return entry.command.tick == command.tick;
        });
    if (existing != predictionHistory_.end())
    {
        *existing = state;
    }
    else
    {
        predictionHistory_.push_back(state);
    }

    while (predictionHistory_.size() > kMaxPredictionHistory)
    {
        predictionHistory_.erase(predictionHistory_.begin());
    }
    unackedCommandCount_ = static_cast<int>(predictionHistory_.size());
}

void Game::PushRemoteSnapshot(const MatchSnapshot& snapshot)
{
    const bool hadSnapshots = !remoteSnapshotBuffer_.empty();
    if (!remoteSnapshotBuffer_.empty())
    {
        MatchSnapshot& latest = remoteSnapshotBuffer_.back();
        if (snapshot.tick < latest.tick)
        {
            return;
        }
        if (snapshot.tick == latest.tick)
        {
            latest = snapshot;
            return;
        }
    }

    remoteSnapshotBuffer_.push_back(snapshot);
    if (!hadSnapshots || !hasNetworkRemoteRenderTime_)
    {
        networkRemoteRenderTime_ = snapshot.matchTime;
        hasNetworkRemoteRenderTime_ = true;
    }
    else if (networkRemoteRenderTime_ < snapshot.matchTime)
    {
        const float maxCatchup = snapshot.matchTime + networkInterpolationDelaySeconds_;
        networkRemoteRenderTime_ = std::min(maxCatchup, std::max(networkRemoteRenderTime_, snapshot.matchTime));
    }
    while (remoteSnapshotBuffer_.size() > kMaxRemoteSnapshotBuffer)
    {
        remoteSnapshotBuffer_.erase(remoteSnapshotBuffer_.begin());
    }
}

void Game::ApplyAuthoritativeSnapshotForPrediction(const MatchSnapshot& snapshot, float fixedDt)
{
    const PlayerSnapshot* authoritative = FindPlayerSnapshot(snapshot, localPlayerId_);
    if (authoritative == nullptr)
    {
        return;
    }

    lastAuthoritativeTick_ = snapshot.tick;
    const std::uint32_t acknowledgedTick = snapshot.lastProcessedCommandTick > 0
        ? snapshot.lastProcessedCommandTick
        : snapshot.tick;

    const auto matching = std::find_if(predictionHistory_.begin(), predictionHistory_.end(),
        [acknowledgedTick](const PredictedCommandState& entry)
        {
            return entry.command.tick == acknowledgedTick;
        });
    const bool hasPredictedState = matching != predictionHistory_.end();
    predictionError_ = hasPredictedState
        ? (matching->predictedPosition - authoritative->position).Length()
        : 0.0f;

    predictionHistory_.erase(
        std::remove_if(predictionHistory_.begin(), predictionHistory_.end(),
            [acknowledgedTick](const PredictedCommandState& entry)
            {
                return entry.command.tick <= acknowledgedTick;
            }),
        predictionHistory_.end());

    const bool needsCorrection = hasPredictedState
        && predictionError_ > kPredictionCorrectionThreshold;
    if (needsCorrection)
    {
        if (Player* player = GetLocalPlayer())
        {
            const Vec3 beforeCorrection = player->GetPositionVec3();

            player->SetPosition(authoritative->position);
            player->SetVelocity(authoritative->velocity);
            player->SetYaw(authoritative->yaw);

            for (PredictedCommandState& pending : predictionHistory_)
            {
                ApplyPredictedPlayerCommand(*player, pending.command, fixedDt);
                pending.predictedPosition = player->GetPositionVec3();
                pending.predictedVelocity = player->GetVelocityVec3();
                pending.predictedYaw = player->GetYaw();
            }

            // Error smoothing: hold the view where prediction had it and let the
            // camera glide to the freshly corrected position, so the correction
            // reads as a smooth ease instead of a snap (the bunny-hop / parkour
            // hitch). Only for corrections small enough that easing looks like
            // smoothing rather than lag; bigger jumps still snap. The offset is
            // decayed toward zero every frame in UpdateCamera.
            const Vec3 afterCorrection = player->GetPositionVec3();
            const Vector3 shift {
                beforeCorrection.x - afterCorrection.x,
                beforeCorrection.y - afterCorrection.y,
                beforeCorrection.z - afterCorrection.z
            };
            const float shiftLen = std::sqrt(shift.x * shift.x + shift.y * shift.y + shift.z * shift.z);
            if (shiftLen <= kPredictionSmoothingMaxDistance)
            {
                predictionSmoothingOffset_.x += shift.x;
                predictionSmoothingOffset_.y += shift.y;
                predictionSmoothingOffset_.z += shift.z;
                // Bound the accumulated visual lag if corrections stack up.
                const float offsetLen = std::sqrt(
                    predictionSmoothingOffset_.x * predictionSmoothingOffset_.x
                    + predictionSmoothingOffset_.y * predictionSmoothingOffset_.y
                    + predictionSmoothingOffset_.z * predictionSmoothingOffset_.z);
                if (offsetLen > kPredictionSmoothingMaxDistance)
                {
                    const float scale = kPredictionSmoothingMaxDistance / offsetLen;
                    predictionSmoothingOffset_.x *= scale;
                    predictionSmoothingOffset_.y *= scale;
                    predictionSmoothingOffset_.z *= scale;
                }
            }
            else
            {
                predictionSmoothingOffset_ = Vector3 { 0.0f, 0.0f, 0.0f };
            }
        }
        ++predictionCorrectionsThisSecond_;
        predictionCorrectionFlashTimer_ = 0.35f;
    }

    unackedCommandCount_ = static_cast<int>(predictionHistory_.size());
    const std::uint32_t localTick = matchSimulation_.CurrentTick();
    const std::uint32_t delayedTicks = localTick > snapshot.tick ? localTick - snapshot.tick : 0;
    const std::uint32_t visibleLagTicks =
        std::max(delayedTicks, static_cast<std::uint32_t>(predictionHistory_.size()));
    estimatedPingMs_ = static_cast<float>(visibleLagTicks) * matchSimulation_.FixedDeltaSeconds() * 1000.0f;
}

void Game::ApplyPredictedPlayerCommand(Player& player, const PlayerCommand& command, float dt)
{
    ApplyPlayerCommand(player, command, dt);
    ApplyStandingBlockEffects(player, HasLocalCamera(ControlKindForPlayer(player)));
    PredictOrbitaDashAction(player, command);
    StepOrbitaDash(player, dt);
}

void Game::UpdatePredictedRangedCharge(Player& player, const PlayerCommand& command, float dt)
{
    if (!HasLocalCamera(ControlKindForPlayer(player)))
    {
        return;
    }

    const ItemType rangedItem = GetSelectedHotbarStack(player).type;
    if (!ItemIsBlasterWeapon(rangedItem))
    {
        if (player.GetBlasterState() == CrossbowState::Loading)
        {
            player.CancelBlasterLoading();
        }
        blasterCharging_ = false;
        return;
    }

    const float fullCharge = BlasterChargeSeconds(player.GetInventory().GetBlasterRapidFireLevel());
    if (player.GetBlasterState() == CrossbowState::Loaded && command.attackPressed)
    {
        player.ConsumeLoadedBlaster();
        blasterCharging_ = false;
        attackChargeActive_ = false;
        attackChargeTimer_ = player.GetBlasterLoadTimer();
        return;
    }
    if (player.GetBlasterState() == CrossbowState::Unloaded && command.attackHeld)
    {
        player.StartBlasterLoading();
    }
    if (player.GetBlasterState() == CrossbowState::Loading && command.attackHeld)
    {
        player.AdvanceBlasterLoading(dt, fullCharge);
        blasterCharging_ = true;
        attackChargeActive_ = true;
        attackChargeTimer_ = player.GetBlasterLoadTimer();
        return;
    }
    if (command.attackReleased && player.GetBlasterState() == CrossbowState::Loading)
    {
        player.CancelBlasterLoading();
    }
    blasterCharging_ = false;
    attackChargeActive_ = false;
    attackChargeTimer_ = player.GetBlasterLoadTimer();
}

void Game::UpdatePredictedBreakProgress(Player& player, const PlayerCommand& command, float dt)
{
    if (!HasLocalCamera(ControlKindForPlayer(player)) || !command.attackHeld || command.placeHeld)
    {
        ResetBreakProgress();
        return;
    }

    const Vector3 aimDirection = AimDirectionFromCommand(command);
    const std::optional<WeaponType> weapon = GetSelectedWeaponType(player);
    float meleeRayLimit = 0.0f;
    if (weapon.has_value())
    {
        const float weaponRange = CombatSystem::AttackRange(*weapon, player.GetInventory().GetSwordLevel());
        meleeRayLimit = weaponRange;
        if (const std::optional<RaycastHit> terrainHit = RaycastFromPlayerEye(player, aimDirection, weaponRange))
        {
            meleeRayLimit = std::max(0.0f, terrainHit->distance - 0.06f);
        }
        if (combat_.FindMeleeTarget(player, players_, aimDirection, *weapon, 1.0f, meleeRayLimit).has_value())
        {
            ResetBreakProgress();
            return;
        }
    }

    const std::optional<RaycastHit> hit = RaycastFromPlayerEye(player, aimDirection, 4.5f);
    if (!hit.has_value())
    {
        ResetBreakProgress();
        return;
    }

    bool isCore = false;
    std::string label = DisplayName(hit->blockData.type);
    if (hit->blockData.type == BlockType::EnergyCoreBlock)
    {
        EnergyCore* core = FindCoreAt(hit->block);
        if (core == nullptr || core->GetTeamId() == player.GetTeamId())
        {
            ResetBreakProgress();
            return;
        }
        isCore = true;
    }
    else if (!hit->blockData.breakable || !IsBreakableByPlayers(hit->blockData.type))
    {
        ResetBreakProgress();
        return;
    }

    // Same break-time rule the server applies (ComputeBreakRequiredSeconds),
    // including the Likho persistent-cut modifier the old copy here lacked —
    // the predicted HUD bar now fills at the authoritative rate. The client
    // maintains its own likhoBlockCuts_ from its own mining, mirroring what
    // the server tracks for this player.
    const float requiredSeconds = ComputeBreakRequiredSeconds(player, *hit, isCore);

    if (!breakProgress_.visible || breakProgress_.target != hit->block || breakProgress_.isCore != isCore)
    {
        breakProgress_ = BreakProgress { hit->block, hit->blockData.type, true, isCore, 0.0f, label };
    }
    breakProgress_.targetType = hit->blockData.type;
    breakProgress_.isCore = isCore;
    breakProgress_.label = label;
    breakProgress_.fraction = std::min(
        0.995f,
        breakProgress_.fraction + dt / std::max(0.001f, requiredSeconds));
}

void Game::UpdatePredictionStats(float dt)
{
    if (predictionCorrectionFlashTimer_ > 0.0f)
    {
        predictionCorrectionFlashTimer_ = std::max(0.0f, predictionCorrectionFlashTimer_ - dt);
    }

    predictionCorrectionStatsTimer_ += dt;
    if (predictionCorrectionStatsTimer_ >= 1.0f)
    {
        predictionCorrectionsPerSecond_ =
            static_cast<float>(predictionCorrectionsThisSecond_) / predictionCorrectionStatsTimer_;
        predictionCorrectionsThisSecond_ = 0;
        predictionCorrectionStatsTimer_ = 0.0f;
    }
}

bool Game::TryGetInterpolatedRemotePlayerPosition(
    int playerId,
    float interpolationDelaySeconds,
    Vec3& out) const
{
    if (playerId == localPlayerId_ || remoteSnapshotBuffer_.empty() || !hasNetworkRemoteRenderTime_)
    {
        return false;
    }

    const float targetTime = networkRemoteRenderTime_
        - std::max(0.0f, interpolationDelaySeconds);
    const MatchSnapshot* older = nullptr;
    const MatchSnapshot* newer = nullptr;
    for (const MatchSnapshot& snapshot : remoteSnapshotBuffer_)
    {
        if (snapshot.matchTime <= targetTime)
        {
            older = &snapshot;
        }
        if (snapshot.matchTime >= targetTime)
        {
            newer = &snapshot;
            break;
        }
    }
    if (older == nullptr)
    {
        older = &remoteSnapshotBuffer_.front();
    }
    if (newer == nullptr)
    {
        newer = &remoteSnapshotBuffer_.back();
    }

    const PlayerSnapshot* a = FindPlayerSnapshot(*older, playerId);
    const PlayerSnapshot* b = FindPlayerSnapshot(*newer, playerId);
    if (a == nullptr || b == nullptr)
    {
        return false;
    }

    const float span = newer->matchTime - older->matchTime;
    if (span <= 0.0001f)
    {
        out = b->position;
        return true;
    }
    out = LerpVec3(a->position, b->position, (targetTime - older->matchTime) / span);
    return true;
}

bool Game::TryGetInterpolatedRemotePlayerYaw(
    int playerId,
    float interpolationDelaySeconds,
    float& out) const
{
    if (playerId == localPlayerId_ || remoteSnapshotBuffer_.empty() || !hasNetworkRemoteRenderTime_)
    {
        return false;
    }

    const float targetTime = networkRemoteRenderTime_
        - std::max(0.0f, interpolationDelaySeconds);
    const MatchSnapshot* older = nullptr;
    const MatchSnapshot* newer = nullptr;
    for (const MatchSnapshot& snapshot : remoteSnapshotBuffer_)
    {
        if (snapshot.matchTime <= targetTime)
        {
            older = &snapshot;
        }
        if (snapshot.matchTime >= targetTime)
        {
            newer = &snapshot;
            break;
        }
    }
    if (older == nullptr)
    {
        older = &remoteSnapshotBuffer_.front();
    }
    if (newer == nullptr)
    {
        newer = &remoteSnapshotBuffer_.back();
    }

    const PlayerSnapshot* a = FindPlayerSnapshot(*older, playerId);
    const PlayerSnapshot* b = FindPlayerSnapshot(*newer, playerId);
    if (a == nullptr || b == nullptr)
    {
        return false;
    }

    const float span = newer->matchTime - older->matchTime;
    if (span <= 0.0001f)
    {
        out = b->yaw;
        return true;
    }
    out = LerpAngle(a->yaw, b->yaw, (targetTime - older->matchTime) / span);
    return true;
}

int Game::RunNetworkSmoke()
{
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }

    serverSession_.Configure(serverConfig_);
    serverSession_.Start();

    // Build a real, headless match so the snapshot carries genuine data.
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();
    screen_ = GameScreen::Playing;

    // The controlled player is driven solely by the commands the server drains,
    // so the in-sim local update must not also self-drive it (double move).
    localPlayerServerDriven_ = true;

    const auto findPlayerById = [this](std::uint32_t id) -> Player*
    {
        for (Player& player : players_)
        {
            if (static_cast<std::uint32_t>(player.GetId()) == id)
            {
                return &player;
            }
        }
        return nullptr;
    };

    Player* controlled = GetLocalPlayer();
    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };
    const Vec3 startPos = controlled != nullptr ? toVec3(controlled->GetPosition()) : Vec3 {};
    const float startYaw = controlled != nullptr ? controlled->GetYaw() : 0.0f;
    const int startSlot = selectedHotbarSlot_;
    const int controlledId = controlled != nullptr ? controlled->GetId() : -1;
    const int controlledTeamId = controlled != nullptr ? controlled->GetTeamId() : -1;

    // Give the controlled player a distinctive private resource so the inventory
    // (owner-private state) is observably non-empty through the visibility filter.
    if (controlled != nullptr)
    {
        controlled->GetInventory().AddResource(ResourceType::Iron, 13);
    }
    // An enemy recipient on a different team, used to prove hidden state does not
    // leak (its own-team trap stays hidden, the controlled inventory is stripped).
    int enemyPlayerId = -1;
    for (const Player& player : matchSimulation_.Players())
    {
        if (player.GetTeamId() != controlledTeamId)
        {
            enemyPlayerId = player.GetId();
            break;
        }
    }

    constexpr int kSmokeTicks = 90; // ~1.5s at the simulation tick rate.
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    const float injectedAimYaw = startYaw; // fixed facing so movement is along one axis
    // Exercise the action-command path: fire the hero's first ability on tick 0
    // and observe the controlled effect (a cooldown is started, or it is a clean
    // no-op if the ability is not castable).
    const bool abilityReadyBefore = controlled != nullptr
        && controlled->IsHeroAbilityReady(HeroAbilitySlot::Active1);
    float abilityCooldownAfterCast = -1.0f;
    int commandsProcessed = 0;
    int commandsApplied = 0;
    int actionCommandsApplied = 0;
    const GridPos syntheticDeltaPos { 7, 21, -7 };
    const Vec3 syntheticProjectilePos { 12.0f, 30.0f, -5.0f };
    for (int i = 0; i < kSmokeTicks; ++i)
    {
        // Client side: assemble a command with real, checkable intent and hand
        // it to the transport/session. command.tick already carries the
        // authoritative MatchSimulation tick (set in BuildLocalPlayerCommand).
        PlayerCommand command = BuildLocalPlayerCommand();
        command.aimYaw = injectedAimYaw;
        command.moveForward = 1.0f;
        command.selectedSlot = i % kHotbarSlotCount;
        command.useAbility1 = (i == 0); // one action command, on the first tick
        serverSession_.SubmitCommand(command);

        // Server side: drain from the transport and forward into the
        // simulation's intake queue (transport buffer -> sim input buffer).
        for (const PlayerCommand& received : serverSession_.DrainCommands())
        {
            matchSimulation_.SubmitCommand(received);
        }

        // Simulation side: drain its intake queue and APPLY each command to the
        // controlled player before advancing the authoritative tick. Movement
        // via ApplyPlayerCommand, discrete actions via ApplyPlayerActionCommand.
        for (const PlayerCommand& received : matchSimulation_.DrainCommands())
        {
            ++commandsProcessed;
            Player* target = findPlayerById(received.controlledPlayerId);
            if (target != nullptr && target->IsAlive())
            {
                ApplyPlayerCommand(*target, received, fixedDt);
                ApplyPlayerActionCommand(*target, received);
                ++commandsApplied;
                ++actionCommandsApplied;
            }
        }

        if (i == 0 && controlled != nullptr)
        {
            abilityCooldownAfterCast = controlled->GetHeroState().active1.cooldownRemaining;
        }

        // Advance the rest of the authoritative world (clock via MatchSimulation,
        // bots, generators) by one fixed step.
        UpdateMatchSimulation(fixedDt);

        if (i == kSmokeTicks - 1)
        {
            BlockDelta delta;
            delta.tick = matchSimulation_.CurrentTick();
            delta.position = syntheticDeltaPos;
            delta.oldType = BlockType::Air;
            delta.newType = BlockType::StoneBlock;
            delta.oldTeamId = -1;
            delta.newTeamId = controlled != nullptr ? controlled->GetTeamId() : -1;
            delta.ownerPlayerId = controlled != nullptr ? controlled->GetId() : -1;
            delta.reason = BlockDeltaReason::ReplicationTest;
            matchSimulation_.RecordBlockDelta(delta);

            // Inject a synthetic projectile so a dynamic-entity section is
            // guaranteed non-empty and provably assembles from real Game state
            // (the world update already ran this tick, so it survives to the
            // snapshot unchanged). Distinctive position/kind make it findable.
            EnergyProjectile syntheticProjectile;
            syntheticProjectile.position =
                Vector3 { syntheticProjectilePos.x, syntheticProjectilePos.y, syntheticProjectilePos.z };
            syntheticProjectile.velocity = Vector3 { 1.0f, 0.0f, 0.0f };
            syntheticProjectile.ownerId = controlled != nullptr ? controlled->GetId() : -1;
            syntheticProjectile.ownerTeamId = controlled != nullptr ? controlled->GetTeamId() : -1;
            syntheticProjectile.kind = ProjectileKind::Fireball;
            syntheticProjectile.lifetime = 2.5f;
            syntheticProjectile.fireZone = true;
            projectiles_.push_back(syntheticProjectile);

            // Inject a synthetic Konvoy trap owned by the controlled player's
            // team. As OwnerTeam state it must reach allies but stay hidden from
            // enemy recipients of the filtered snapshot.
            KonvoyTrap syntheticTrap;
            syntheticTrap.position = Vector3 { 9.0f, 1.0f, 9.0f };
            syntheticTrap.ownerPlayerId = controlledId;
            syntheticTrap.ownerTeamId = controlledTeamId;
            syntheticTrap.lifetime = 8.0f;
            syntheticTrap.health = 48;
            konvoyTraps_.push_back(syntheticTrap);

            if (controlledTeamId >= 0 && controlledTeamId < static_cast<int>(teamChests_.size()))
            {
                teamChests_[controlledTeamId].AddResource(ResourceType::Iron, 7);
                ItemStack syntheticChestStack;
                syntheticChestStack.type = ItemType::MedKit;
                syntheticChestStack.count = 2;
                teamChests_[controlledTeamId].SwapSlot(12, syntheticChestStack);
            }
        }

        // Server publishes the replicated snapshot; the client reads it back.
        const MatchSnapshot snapshot = BuildNetworkSnapshot();
        serverSession_.PublishSnapshot(snapshot);
        matchSimulation_.ClearBlockDeltasThrough(snapshot.tick);
    }

    const Vec3 endPos = controlled != nullptr ? toVec3(controlled->GetPosition()) : Vec3 {};
    const float endYaw = controlled != nullptr ? controlled->GetYaw() : 0.0f;
    const int endSlot = selectedHotbarSlot_;
    const float movedDistance = (endPos - startPos).Length();

    localPlayerServerDriven_ = false;

    const MatchSnapshot& snapshot = serverSession_.LatestSnapshot();
    const ServerConfig& config = serverSession_.Config();

    // The pickup/dropped-item snapshot sections must reflect the live (visible,
    // i.e. not-collected) world items — proving they assemble from real state,
    // not an empty stub. The published snapshot was built from the same state
    // that matchSimulation_ still holds (no updates ran after it).
    std::size_t visiblePickups = 0;
    for (const ResourcePickup& pickup : matchSimulation_.Pickups())
    {
        if (!pickup.collected)
        {
            ++visiblePickups;
        }
    }
    std::size_t visibleDropped = 0;
    for (const DroppedItem& dropped : matchSimulation_.DroppedItems())
    {
        if (!dropped.collected)
        {
            ++visibleDropped;
        }
    }
    const bool worldItemsReplicated = snapshot.pickups.size() == visiblePickups
        && snapshot.droppedItems.size() == visibleDropped;
    // The authoritative world legitimately emits its own block deltas on the
    // final tick (bots breaking/placing, fire), so don't require the synthetic
    // ReplicationTest delta to be the only one — just prove it round-trips into
    // the snapshot and that the live buffer was cleared after publish.
    const bool blockDeltaReplicated = std::any_of(
            snapshot.blockDeltas.begin(), snapshot.blockDeltas.end(),
            [&syntheticDeltaPos](const BlockDelta& delta)
            {
                return delta.position == syntheticDeltaPos
                    && delta.oldType == BlockType::Air
                    && delta.newType == BlockType::StoneBlock
                    && delta.reason == BlockDeltaReason::ReplicationTest;
            })
        && matchSimulation_.BlockDeltas().empty();

    // The dynamic-entity sections must carry real state, not a stub. Prove it via
    // the injected synthetic projectile round-tripping through the snapshot with
    // its public fields intact, and confirm the projectile section size matches
    // the live Game vector (every live projectile replicated, no filtering yet).
    bool syntheticProjectileReplicated = false;
    for (const ProjectileSnapshot& entry : snapshot.projectiles)
    {
        const bool samePos = entry.position.x == syntheticProjectilePos.x
            && entry.position.y == syntheticProjectilePos.y
            && entry.position.z == syntheticProjectilePos.z;
        if (samePos
            && entry.kind == static_cast<int>(ProjectileKind::Fireball)
            && entry.fireZone
            && entry.remainingLifetime > 0.0f
            && entry.visibility == SnapshotVisibility::Public)
        {
            syntheticProjectileReplicated = true;
            break;
        }
    }
    const bool dynamicEntitiesReplicated = syntheticProjectileReplicated
        && snapshot.projectiles.size() == projectiles_.size();

    // --- Per-client visibility filter ---------------------------------------
    // Derive the owner's and an enemy's filtered views from the FULL snapshot and
    // assert: team state (own-team trap) reaches the owner but NOT the enemy;
    // owner-private inventory reaches the owner but is stripped for the enemy;
    // public state (the synthetic projectile + full player roster) survives both.
    const MatchSnapshot ownerView = FilterSnapshotForClient(snapshot, controlledId);
    const MatchSnapshot enemyView = FilterSnapshotForClient(snapshot, enemyPlayerId);

    const int expectedIron = controlled != nullptr
        ? controlled->GetInventory().GetResource(ResourceType::Iron) : 0;
    const auto ownTeamTrapVisible = [controlledTeamId](const MatchSnapshot& v)
    {
        for (const HeroDeviceSnapshot& d : v.heroDevices)
        {
            if (d.type == HeroDeviceType::KonvoyTrap && d.ownerTeamId == controlledTeamId)
            {
                return true;
            }
        }
        return false;
    };
    const auto controlledInventory = [controlledId](const MatchSnapshot& v) -> const InventorySnapshot*
    {
        for (const PlayerSnapshot& p : v.players)
        {
            if (p.playerId == controlledId)
            {
                return &p.inventory;
            }
        }
        return nullptr;
    };
    const auto publicProjectileVisible = [&syntheticProjectilePos](const MatchSnapshot& v)
    {
        for (const ProjectileSnapshot& pr : v.projectiles)
        {
            if (pr.position.x == syntheticProjectilePos.x
                && pr.position.y == syntheticProjectilePos.y
                && pr.position.z == syntheticProjectilePos.z)
            {
                return true;
            }
        }
        return false;
    };
    const auto teamChestFor = [controlledTeamId](const MatchSnapshot& v) -> const TeamChestSnapshot*
    {
        for (const TeamChestSnapshot& chest : v.teamChests)
        {
            if (chest.teamId == controlledTeamId)
            {
                return &chest;
            }
        }
        return nullptr;
    };

    const InventorySnapshot* fullInv = controlledInventory(snapshot);
    const InventorySnapshot* ownerInv = controlledInventory(ownerView);
    const InventorySnapshot* enemyInv = controlledInventory(enemyView);
    const TeamChestSnapshot* fullChest = teamChestFor(snapshot);
    const TeamChestSnapshot* ownerChest = teamChestFor(ownerView);
    const TeamChestSnapshot* enemyChest = teamChestFor(enemyView);

    const bool teamStateHidden = ownTeamTrapVisible(snapshot)   // full has it
        && ownTeamTrapVisible(ownerView)                        // ally keeps it
        && !ownTeamTrapVisible(enemyView);                      // enemy never sees it
    const bool ownerPrivateStripped = fullInv != nullptr && fullInv->present
        && fullInv->resources[0] == expectedIron && expectedIron > 0
        && ownerInv != nullptr && ownerInv->present && ownerInv->resources[0] == expectedIron
        && enemyInv != nullptr && !enemyInv->present
        && enemyInv->resources[0] == 0 && enemyInv->hotbar.empty();
    const bool publicSurvivesFilter = publicProjectileVisible(snapshot)
        && publicProjectileVisible(ownerView)
        && publicProjectileVisible(enemyView)
        && enemyView.players.size() == snapshot.players.size(); // positions public (no fog)
    const bool teamChestVisibleToOwner = fullChest != nullptr
        && ownerChest != nullptr
        && fullChest->resources[0] == ownerChest->resources[0]
        && ownerChest->resources[0] >= 7
        && ownerChest->slots.size() > 12
        && ownerChest->slots[12].itemType == static_cast<int>(ItemType::MedKit)
        && ownerChest->slots[12].count == 2
        && enemyChest == nullptr
        && !enemyView.teamChests.empty();
    bool teamChestClientApplyOk = false;
    if (controlledTeamId >= 0 && controlledTeamId < static_cast<int>(teamChests_.size()) && ownerChest != nullptr)
    {
        ItemStack empty;
        teamChests_[controlledTeamId].SwapSlot(12, empty);
        ApplyClientSnapshot(ownerView);
        const Inventory& appliedChest = teamChests_[controlledTeamId];
        const ItemStack appliedStack = appliedChest.GetSlot(12);
        teamChestClientApplyOk =
            appliedChest.GetResource(ResourceType::Iron) == ownerChest->resources[0]
            && appliedStack.type == ItemType::MedKit
            && appliedStack.count == 2;
    }
    const bool visibilityFilterOk = enemyPlayerId >= 0
        && teamStateHidden && ownerPrivateStripped && publicSurvivesFilter
        && teamChestVisibleToOwner && teamChestClientApplyOk;

    // Selected slot + replicated Vec3 position of the controlled player as seen
    // in the snapshot.
    int snapshotControlledSlot = -1;
    Vec3 snapshotControlledPos {};
    if (controlled != nullptr)
    {
        for (const PlayerSnapshot& entry : snapshot.players)
        {
            if (entry.playerId == controlled->GetId())
            {
                snapshotControlledSlot = entry.selectedSlot;
                snapshotControlledPos = entry.position;
                break;
            }
        }
    }
    const int expectedSlot = (kSmokeTicks - 1) % kHotbarSlotCount;
    const float snapshotPosError = (snapshotControlledPos - endPos).Length();

    constexpr int kArtificialUnacked = 3;
    bool predictionSmokeOk = false;
    float predictionCorrectionLiveError = 1e9f;
    if (controlled != nullptr)
    {
        predictionHistory_.clear();
        remoteSnapshotBuffer_.clear();
        PushRemoteSnapshot(snapshot);

        PlayerCommand acknowledged = BuildLocalPlayerCommand();
        acknowledged.controlledPlayerId = static_cast<std::uint32_t>(controlled->GetId());
        acknowledged.tick = snapshot.tick;
        acknowledged.aimYaw = endYaw;
        acknowledged.moveForward = 0.0f;
        acknowledged.moveStrafe = 0.0f;
        acknowledged.jump = false;
        acknowledged.sprint = false;
        acknowledged.sprintTapped = false;
        acknowledged.selectedSlot = endSlot;

        PredictedCommandState wrongState;
        wrongState.command = acknowledged;
        wrongState.predictedPosition = Vec3 {
            snapshotControlledPos.x + 1.0f,
            snapshotControlledPos.y,
            snapshotControlledPos.z
        };
        wrongState.predictedVelocity = Vec3 {};
        wrongState.predictedYaw = endYaw;
        predictionHistory_.push_back(wrongState);

        for (int lag = 1; lag <= kArtificialUnacked; ++lag)
        {
            PlayerCommand pending = acknowledged;
            pending.tick = snapshot.tick + static_cast<std::uint32_t>(lag);
            PredictedCommandState futureState;
            futureState.command = pending;
            futureState.predictedPosition = snapshotControlledPos;
            futureState.predictedVelocity = Vec3 {};
            futureState.predictedYaw = endYaw;
            predictionHistory_.push_back(futureState);
        }

        const int correctionsBefore = predictionCorrectionsThisSecond_;
        ApplyAuthoritativeSnapshotForPrediction(snapshot, fixedDt);
        predictionCorrectionLiveError = (controlled->GetPositionVec3() - snapshotControlledPos).Length();
        predictionSmokeOk = predictionError_ > 0.9f
            && predictionCorrectionsThisSecond_ == correctionsBefore + 1
            && unackedCommandCount_ == kArtificialUnacked
            && predictionCorrectionLiveError < 0.25f
            && estimatedPingMs_ > 0.0f;
    }

    std::cout << "network-smoke: mode=" << ToString(networkMode_)
              << " server=\"" << config.serverName << "\""
              << " listen=" << config.listenAddress << ":" << config.port
              << " maxPlayers=" << config.maxPlayers
              << " private=" << (config.privateServer ? "yes" : "no")
              << " password=" << (config.HasPassword() ? "set" : "none") << '\n';
    std::cout << "network-smoke: simulationTick=" << matchSimulation_.CurrentTick()
              << " snapshot.tick=" << snapshot.tick
              << " tickRate=" << matchSimulation_.TickRate()
              << " fixedDt=" << fixedDt
              << " commandsProcessed=" << commandsProcessed
              << " commandsApplied=" << commandsApplied
              << " snapshotsPublished=" << serverSession_.SnapshotsPublished()
              << " phase=" << ToString(snapshot.phase)
              << " players=" << snapshot.players.size()
              << " cores=" << snapshot.cores.size()
              << " generators=" << snapshot.generators.size()
              << " pickups=" << snapshot.pickups.size()
              << " droppedItems=" << snapshot.droppedItems.size()
              << " blockDeltas=" << snapshot.blockDeltas.size()
              << " deltaBufferAfterPublish=" << matchSimulation_.BlockDeltas().size()
              << " matchTime=" << snapshot.matchTime << '\n';
    std::cout << "network-smoke: controlledId=" << (controlled != nullptr ? controlled->GetId() : -1)
              << " movedDistance=" << movedDistance
              << " startPos=(" << startPos.x << ',' << startPos.y << ',' << startPos.z << ')'
              << " endPos=(" << endPos.x << ',' << endPos.y << ',' << endPos.z << ')'
              << " yaw=" << startYaw << "->" << endYaw << " (cmd aimYaw=" << injectedAimYaw << ')'
              << " slot=" << startSlot << "->" << endSlot
              << " (snapshot=" << snapshotControlledSlot << ", expected=" << expectedSlot << ')'
              << " snapshotPos=(" << snapshotControlledPos.x << ',' << snapshotControlledPos.y
              << ',' << snapshotControlledPos.z << ") posError=" << snapshotPosError << '\n';
    std::cout << "network-smoke: actionCommandsApplied=" << actionCommandsApplied
              << " ability1ReadyBefore=" << (abilityReadyBefore ? "yes" : "no")
              << " ability1CooldownAfterCast=" << abilityCooldownAfterCast
              << (abilityReadyBefore && abilityCooldownAfterCast > 0.0f
                      ? " (controlled effect: cooldown started)"
                      : " (no-op)") << '\n';
    std::cout << "network-smoke: dynamicEntities projectiles=" << snapshot.projectiles.size()
              << " explosives=" << snapshot.explosives.size()
              << " hazardZones=" << snapshot.hazardZones.size()
              << " heroDevices=" << snapshot.heroDevices.size()
              << " statusEffects=" << snapshot.statusEffects.size()
              << " syntheticProjectile=" << (syntheticProjectileReplicated ? "found" : "MISSING")
              << '\n';
    std::cout << "network-smoke: visibility ownerId=" << controlledId
              << " enemyId=" << enemyPlayerId
              << " trap[full/owner/enemy]=" << ownTeamTrapVisible(snapshot)
              << '/' << ownTeamTrapVisible(ownerView) << '/' << ownTeamTrapVisible(enemyView)
              << " inv.present[full/owner/enemy]=" << (fullInv != nullptr && fullInv->present)
              << '/' << (ownerInv != nullptr && ownerInv->present)
              << '/' << (enemyInv != nullptr && enemyInv->present)
              << " inv.iron[full/owner/enemy]=" << (fullInv != nullptr ? fullInv->resources[0] : -1)
              << '/' << (ownerInv != nullptr ? ownerInv->resources[0] : -1)
              << '/' << (enemyInv != nullptr ? enemyInv->resources[0] : -1)
              << " publicProjectile[owner/enemy]=" << publicProjectileVisible(ownerView)
              << '/' << publicProjectileVisible(enemyView)
              << " chest[full/owner/enemy/apply]=" << (fullChest != nullptr)
              << '/' << (ownerChest != nullptr)
              << '/' << (enemyChest != nullptr)
              << '/' << teamChestClientApplyOk
              << " players[full/enemy]=" << snapshot.players.size() << '/' << enemyView.players.size()
              << '\n';
    std::cout << "network-smoke: prediction error=" << predictionError_
              << " correctionLiveError=" << predictionCorrectionLiveError
              << " unacked=" << unackedCommandCount_
              << " pingMs=" << estimatedPingMs_
              << " correction=" << (predictionSmokeOk ? "ok" : "FAIL") << '\n';

    const bool commandMoved = movedDistance > 0.5f;
    const bool slotApplied = endSlot == expectedSlot && snapshotControlledSlot == expectedSlot;
    const bool aimApplied = std::fabs(endYaw - injectedAimYaw) < 0.001f;
    // The snapshot's raylib-free Vec3 position must match the live position.
    const bool snapshotPosOk = snapshotPosError < 0.001f;
    // The action-command path ran for every applied command (ability no-op or
    // controlled effect) without disturbing movement.
    const bool actionApplied = actionCommandsApplied == kSmokeTicks;
    const bool ok = !snapshot.players.empty()
        && !snapshot.cores.empty()
        && !snapshot.generators.empty()
        && matchSimulation_.CurrentTick() == static_cast<std::uint32_t>(kSmokeTicks)
        && commandsProcessed == kSmokeTicks
        && commandsApplied == kSmokeTicks
        && snapshot.tick == static_cast<std::uint32_t>(kSmokeTicks)
        && commandMoved
        && slotApplied
        && aimApplied
        && snapshotPosOk
        && worldItemsReplicated
        && blockDeltaReplicated
        && dynamicEntitiesReplicated
        && visibilityFilterOk
        && actionApplied
        && predictionSmokeOk;

    if (!ok)
    {
        std::cout << "network-smoke: checks moved=" << (commandMoved ? "ok" : "FAIL")
                  << " slot=" << (slotApplied ? "ok" : "FAIL")
                  << " aim=" << (aimApplied ? "ok" : "FAIL")
                  << " snapshotPos=" << (snapshotPosOk ? "ok" : "FAIL")
                  << " worldItems=" << (worldItemsReplicated ? "ok" : "FAIL")
                  << " blockDelta=" << (blockDeltaReplicated ? "ok" : "FAIL")
                  << " dynamicEntities=" << (dynamicEntitiesReplicated ? "ok" : "FAIL")
                  << " visibility=" << (visibilityFilterOk ? "ok" : "FAIL")
                  << "(team=" << (teamStateHidden ? "ok" : "FAIL")
                  << ",ownerPriv=" << (ownerPrivateStripped ? "ok" : "FAIL")
                  << ",public=" << (publicSurvivesFilter ? "ok" : "FAIL")
                  << ",chestVisible=" << (teamChestVisibleToOwner ? "ok" : "FAIL")
                  << ",chestApply=" << (teamChestClientApplyOk ? "ok" : "FAIL") << ')'
                  << " action=" << (actionApplied ? "ok" : "FAIL")
                  << " prediction=" << (predictionSmokeOk ? "ok" : "FAIL") << '\n';
    }

    std::cout << (ok ? "NETWORK_SMOKE_OK" : "NETWORK_SMOKE_FAIL") << std::endl;
    serverSession_.Stop();
    return ok ? 0 : 4;
}

int Game::RunMovementParitySmoke()
{
    arenaBiome_ = ArenaBiome::Ice;
    world_.Clear();
    for (int x = -4; x <= 80; ++x)
    {
        for (int z = -3; z <= 3; ++z)
        {
            world_.PlaceBlock(GridPos { x, 0, z }, Block { BlockType::GrassBlock, -1, false }, true);
        }
    }
    for (int x = 28; x <= 42; ++x)
    {
        for (int z = -3; z <= 3; ++z)
        {
            world_.PlaceBlock(GridPos { x, 0, z }, Block { BlockType::IceBlock, -1, false }, true);
        }
    }

    Player localHuman(1, "local-human", 0, Vector3 { 0.0f, 1.5f, 0.0f }, true);
    localHuman.SetControlKind(PlayerControlKind::LocalHumanPredicted);
    Player remoteHuman(2, "remote-human", 1, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    remoteHuman.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);

    PlayerCommand command;
    command.aimYaw = 3.1415926535f * 0.5f;
    command.moveForward = 1.0f;
    command.sprint = true;
    command.selectedSlot = 0;

    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    constexpr int kTicks = 180;
    for (int i = 0; i < kTicks; ++i)
    {
        command.tick = static_cast<std::uint32_t>(i + 1);
        command.controlledPlayerId = static_cast<std::uint32_t>(localHuman.GetId());
        ApplyPlayerCommand(localHuman, command, fixedDt);
        command.controlledPlayerId = static_cast<std::uint32_t>(remoteHuman.GetId());
        ApplyPlayerCommand(remoteHuman, command, fixedDt);
    }

    const float positionError = (localHuman.GetPositionVec3() - remoteHuman.GetPositionVec3()).Length();
    const float velocityError = (localHuman.GetVelocityVec3() - remoteHuman.GetVelocityVec3()).Length();
    const bool sameRoleProfile =
        IsHumanControlled(ControlKindForPlayer(localHuman))
        && IsHumanControlled(ControlKindForPlayer(remoteHuman))
        && IsLocallyPredicted(ControlKindForPlayer(localHuman))
        && !IsLocallyPredicted(ControlKindForPlayer(remoteHuman));
    const bool movementParity = positionError < 0.001f && velocityError < 0.001f;
    const bool localCameraOnly = HasLocalCamera(ControlKindForPlayer(localHuman))
        && !HasLocalCamera(ControlKindForPlayer(remoteHuman));

    world_.Clear();
    world_.PlaceBlock(GridPos { 0, 0, 0 }, Block { BlockType::GrassBlock, -1, false }, true);
    Player predictedSpring(3, "predicted-spring", 0, Vector3 { 0.0f, 1.5f, 0.0f }, true);
    predictedSpring.SetControlKind(PlayerControlKind::LocalHumanPredicted);
    Player authoritativeSpring(4, "authoritative-spring", 1, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    authoritativeSpring.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
    PlayerCommand springCommand;
    springCommand.selectedSlot = 0;
    for (int i = 0; i < 30; ++i)
    {
        springCommand.tick = static_cast<std::uint32_t>(i + 1);
        springCommand.controlledPlayerId = static_cast<std::uint32_t>(predictedSpring.GetId());
        ApplyPlayerCommand(predictedSpring, springCommand, fixedDt);
        springCommand.controlledPlayerId = static_cast<std::uint32_t>(authoritativeSpring.GetId());
        ApplyPlayerCommand(authoritativeSpring, springCommand, fixedDt);
    }
    world_.PlaceBlock(GridPos { 0, 0, 0 }, Block { BlockType::SpringBlock, -1, false }, true);
    for (int i = 0; i < 4; ++i)
    {
        springCommand.tick = static_cast<std::uint32_t>(31 + i);
        springCommand.controlledPlayerId = static_cast<std::uint32_t>(predictedSpring.GetId());
        ApplyPlayerCommand(predictedSpring, springCommand, fixedDt);
        ApplyStandingBlockEffects(predictedSpring, true);
        springCommand.controlledPlayerId = static_cast<std::uint32_t>(authoritativeSpring.GetId());
        ApplyPlayerCommand(authoritativeSpring, springCommand, fixedDt);
        ApplyStandingBlockEffects(authoritativeSpring, false);
    }
    const float springPositionError =
        (predictedSpring.GetPositionVec3() - authoritativeSpring.GetPositionVec3()).Length();
    const float springVelocityError =
        (predictedSpring.GetVelocityVec3() - authoritativeSpring.GetVelocityVec3()).Length();
    const bool springApplied = predictedSpring.GetVelocity().y > 4.0f
        && authoritativeSpring.GetVelocity().y > 4.0f;
    const bool springParity = springApplied
        && springPositionError < 0.001f
        && springVelocityError < 0.001f;

    world_.Clear();
    for (int x = -4; x <= 12; ++x)
    {
        for (int z = -2; z <= 2; ++z)
        {
            world_.PlaceBlock(GridPos { x, 0, z }, Block { BlockType::GrassBlock, -1, false }, true);
        }
    }
    matchSimulation_.Players().clear();
    networkControlledPlayerIds_.clear();
    serverHeldPlayerCommands_.clear();
    serverEffectiveCommandTickByPlayer_.clear();
    Player batchedServerPlayer(10, "batched-server", 0, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    batchedServerPlayer.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
    matchSimulation_.Players().push_back(batchedServerPlayer);
    MarkNetworkControlledPlayer(10);

    Player expectedSingleStep(11, "single-step", 0, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    expectedSingleStep.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
    PlayerCommand batchCommand;
    batchCommand.controlledPlayerId = 10;
    batchCommand.aimYaw = 3.1415926535f * 0.5f;
    batchCommand.moveForward = 1.0f;
    batchCommand.selectedSlot = 0;
    std::vector<ReceivedCommand> receivedBatch;
    for (int i = 0; i < 3; ++i)
    {
        PlayerCommand batchedCommand = batchCommand;
        batchedCommand.tick = static_cast<std::uint32_t>(i + 1);
        receivedBatch.push_back(ReceivedCommand { 100, batchedCommand });
    }
    ApplyBatchedServerCommands(receivedBatch, fixedDt);
    batchCommand.controlledPlayerId = 11;
    batchCommand.tick = 3;
    ApplyPlayerCommand(expectedSingleStep, batchCommand, fixedDt);
    ApplyStandingBlockEffects(expectedSingleStep, false);

    const Player* batchedAfter = matchSimulation_.GetPlayer(10);
    const float batchPositionError = batchedAfter != nullptr
        ? (batchedAfter->GetPositionVec3() - expectedSingleStep.GetPositionVec3()).Length()
        : 999.0f;
    ApplyBatchedServerCommands({}, fixedDt);
    ++batchCommand.tick;
    ApplyPlayerCommand(expectedSingleStep, batchCommand, fixedDt);
    ApplyStandingBlockEffects(expectedSingleStep, false);
    const Player* heldAfter = matchSimulation_.GetPlayer(10);
    const float heldPositionError = heldAfter != nullptr
        ? (heldAfter->GetPositionVec3() - expectedSingleStep.GetPositionVec3()).Length()
        : 999.0f;
    const bool batchPacing = batchPositionError < 0.001f && heldPositionError < 0.001f;

    world_.Clear();
    for (int x = -4; x <= 24; ++x)
    {
        for (int z = -2; z <= 2; ++z)
        {
            world_.PlaceBlock(GridPos { x, 0, z }, Block { BlockType::GrassBlock, -1, false }, true);
        }
    }
    Player predictedOrbita(12, "predicted-orbita", 0, Vector3 { 0.0f, 1.5f, 0.0f }, true);
    predictedOrbita.SetControlKind(PlayerControlKind::LocalHumanPredicted);
    predictedOrbita.SetHeroId(HeroId::Orbita);
    Player authoritativeOrbita(13, "authoritative-orbita", 1, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    authoritativeOrbita.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
    authoritativeOrbita.SetHeroId(HeroId::Orbita);

    PlayerCommand orbitaCommand;
    orbitaCommand.aimYaw = 3.1415926535f * 0.5f;
    orbitaCommand.selectedSlot = 0;
    orbitaCommand.useAbility1 = true;
    constexpr int kOrbitaDashSmokeTicks = 8;
    for (int i = 0; i < kOrbitaDashSmokeTicks; ++i)
    {
        orbitaCommand.tick = static_cast<std::uint32_t>(i + 1);
        orbitaCommand.controlledPlayerId = static_cast<std::uint32_t>(predictedOrbita.GetId());
        ApplyPredictedPlayerCommand(predictedOrbita, orbitaCommand, fixedDt);

        orbitaCommand.controlledPlayerId = static_cast<std::uint32_t>(authoritativeOrbita.GetId());
        ApplyPlayerCommand(authoritativeOrbita, orbitaCommand, fixedDt);
        ApplyStandingBlockEffects(authoritativeOrbita, false);
        ApplyPlayerActionCommand(authoritativeOrbita, orbitaCommand);
        StepOrbitaDash(authoritativeOrbita, fixedDt);

        orbitaCommand.useAbility1 = false;
    }
    const float orbitaDashMoved =
        (predictedOrbita.GetPositionVec3() - Vec3 { 0.0f, 1.5f, 0.0f }).Length();
    const float orbitaDashPositionError =
        (predictedOrbita.GetPositionVec3() - authoritativeOrbita.GetPositionVec3()).Length();
    const bool orbitaDashPrediction = orbitaDashMoved > 1.0f && orbitaDashPositionError < 0.001f;

    const bool ok = sameRoleProfile && movementParity && localCameraOnly && springParity
        && batchPacing && orbitaDashPrediction;

    std::cout << "movement-parity-smoke: posError=" << positionError
              << " velError=" << velocityError
              << " springPosError=" << springPositionError
              << " springVelError=" << springVelocityError
              << " batchPosError=" << batchPositionError
              << " heldPosError=" << heldPositionError
              << " orbitaDashMoved=" << orbitaDashMoved
              << " orbitaDashPosError=" << orbitaDashPositionError
              << " localKind=" << static_cast<int>(ControlKindForPlayer(localHuman))
              << " remoteKind=" << static_cast<int>(ControlKindForPlayer(remoteHuman))
              << " parity=" << (movementParity ? "ok" : "FAIL")
              << " spring=" << (springParity ? "ok" : "FAIL")
              << " batching=" << (batchPacing ? "ok" : "FAIL")
              << " orbitaDash=" << (orbitaDashPrediction ? "ok" : "FAIL")
              << " roles=" << (sameRoleProfile ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "MOVEMENT_PARITY_SMOKE_OK" : "MOVEMENT_PARITY_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 4;
}

int Game::RunNetworkPurchaseSmoke()
{
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }

    // Real headless match so shop/team/inventory are genuine, not stubs.
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();
    screen_ = GameScreen::Playing;

    Player* controlled = GetLocalPlayer();
    Team* team = controlled != nullptr ? FindTeam(controlled->GetTeamId()) : nullptr;
    if (controlled == nullptr || team == nullptr)
    {
        std::cout << "purchase-smoke: no controlled player/team\n";
        std::cout << "PURCHASE_SMOKE_FAIL" << std::endl;
        return 4;
    }

    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };
    Inventory& inv = controlled->GetInventory();

    // Stand the player on the shop so the server-side proximity gate passes.
    controlled->SetPosition(toVec3(team->shopPosition));

    suppressLocalFeedback_ = false;
    const std::string messageBefore = message_;
    const std::size_t eventMessagesBefore = eventMessages_.size();
    const std::size_t worldEffectsBefore = worldEffects_.size();
    const std::size_t floatingTextsBefore = floatingTexts_.size();
    const bool audioMutedBefore = audio_.IsMuted();

    // --- Case 1: a valid purchase spends resources and grants the item. ------
    // Choice 1 = 32 wood blocks for 5 Iron (no secondary cost). Guarantee funds.
    inv.AddResource(ResourceType::Iron, 5);
    const int ironBefore = inv.GetResource(ResourceType::Iron);
    const int woodBefore = inv.GetBlockCount(BlockType::WoodBlock);

    QueueEconomyAction(PlayerActionType::BuyItem, /*choice*/ 1, /*repeat*/ 1);
    PlayerCommand buy = BuildLocalPlayerCommand();
    const PlayerActionResult buyResult = ApplyPlayerEconomyCommand(*controlled, buy);
    const int ironAfter = inv.GetResource(ResourceType::Iron);
    const int woodAfter = inv.GetBlockCount(BlockType::WoodBlock);
    const bool buyOk = buyResult.handled && buyResult.success
        && !buyResult.message.empty()
        && ironAfter == ironBefore - 5 && woodAfter == woodBefore + 32;

    // --- Case 2: re-sending the SAME command (same seq) must NOT buy again. ---
    const PlayerActionResult duplicateResult = ApplyPlayerEconomyCommand(*controlled, buy);
    const bool dedupeOk = !duplicateResult.handled && !duplicateResult.success
        && inv.GetResource(ResourceType::Iron) == ironAfter
        && inv.GetBlockCount(BlockType::WoodBlock) == woodAfter;

    // --- Case 3: denied for lack of resources (no state change). -------------
    inv.SpendResource(ResourceType::Gold, inv.GetResource(ResourceType::Gold));
    inv.SpendResource(ResourceType::Crystal, inv.GetResource(ResourceType::Crystal));
    const int ironBeforeBroke = inv.GetResource(ResourceType::Iron);
    QueueEconomyAction(PlayerActionType::BuyItem, /*choice 4 = obsidian, costs Gold+Crystal*/ 4, 1);
    PlayerCommand brokeBuy = BuildLocalPlayerCommand();
    const PlayerActionResult brokeResult = ApplyPlayerEconomyCommand(*controlled, brokeBuy);
    const bool deniedFundsOk = brokeResult.handled && !brokeResult.success
        && !brokeResult.message.empty()
        && inv.GetResource(ResourceType::Iron) == ironBeforeBroke
        && inv.GetResource(ResourceType::Gold) == 0
        && inv.GetResource(ResourceType::Crystal) == 0;

    // --- Case 4: denied when out of the shop zone, even with funds. ----------
    controlled->SetPosition(toVec3(Vector3 {
        team->shopPosition.x + 100.0f, team->shopPosition.y, team->shopPosition.z }));
    inv.AddResource(ResourceType::Iron, 50);
    const int ironBeforeFar = inv.GetResource(ResourceType::Iron);
    const int woodBeforeFar = inv.GetBlockCount(BlockType::WoodBlock);
    QueueEconomyAction(PlayerActionType::BuyItem, 1, 1);
    PlayerCommand farBuy = BuildLocalPlayerCommand();
    const PlayerActionResult farResult = ApplyPlayerEconomyCommand(*controlled, farBuy);
    const bool deniedRangeOk = farResult.handled && !farResult.success
        && !farResult.message.empty()
        && inv.GetResource(ResourceType::Iron) == ironBeforeFar
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBeforeFar;

    // --- Case 5: inventory drop rides the same deduped action channel. -------
    ItemStack dropStack;
    dropStack.type = ItemType::Fireball;
    dropStack.count = 3;
    inv.SwapSlot(3, dropStack);
    const std::size_t droppedBefore = matchSimulation_.DroppedItems().size();
    QueueEconomyAction(PlayerActionType::DropItem, /*slot*/ 3, /*count*/ 2);
    PlayerCommand dropCommand = BuildLocalPlayerCommand();
    const PlayerActionResult dropResult = ApplyPlayerEconomyCommand(*controlled, dropCommand);
    const ItemStack dropSlotAfter = inv.GetSlot(3);
    const bool dropOk = dropResult.handled && dropResult.success
        && matchSimulation_.DroppedItems().size() == droppedBefore + 1
        && matchSimulation_.DroppedItems().back().stack.type == ItemType::Fireball
        && matchSimulation_.DroppedItems().back().stack.count == 2
        && dropSlotAfter.type == ItemType::Fireball
        && dropSlotAfter.count == 1;
    const PlayerActionResult dropDuplicate = ApplyPlayerEconomyCommand(*controlled, dropCommand);
    const bool dropDedupeOk = !dropDuplicate.handled
        && matchSimulation_.DroppedItems().size() == droppedBefore + 1
        && inv.GetSlot(3).count == 1;

    // --- Case 6: quick-move transfers a stack across hotbar/main inventory. ---
    ItemStack moveStack;
    moveStack.type = ItemType::MedKit;
    moveStack.count = 2;
    inv.SwapSlot(4, moveStack);
    const ItemStack mainSlotBeforeMove = inv.GetSlot(kHotbarSlotCount);
    inv.SwapSlot(kHotbarSlotCount, ItemStack {});
    QueueEconomyAction(PlayerActionType::MoveInventory, /*slot*/ 4, 0);
    PlayerCommand moveCommand = BuildLocalPlayerCommand();
    const PlayerActionResult moveResult = ApplyPlayerEconomyCommand(*controlled, moveCommand);
    const bool moveOk = moveResult.handled && moveResult.success
        && inv.GetSlot(4).IsEmpty()
        && inv.GetSlot(kHotbarSlotCount).type == ItemType::MedKit
        && inv.GetSlot(kHotbarSlotCount).count == 2;
    if (!mainSlotBeforeMove.IsEmpty())
    {
        inv.SwapSlot(kHotbarSlotCount, mainSlotBeforeMove);
    }

    // --- Case 7: team chest transfer is server-owned and deduped. ------------
    controlled->SetPosition(toVec3(TeamChestDepositPosition(*team)));
    Inventory& teamChest = teamChests_[team->id];
    ItemStack chestStack;
    chestStack.type = ItemType::AlarmTrap;
    chestStack.count = 2;
    inv.SwapSlot(6, chestStack);
    const int invAlarmBeforeChest = inv.CountItem(ItemType::AlarmTrap);
    const int chestAlarmBefore = teamChest.CountItem(ItemType::AlarmTrap);
    QueueEconomyAction(PlayerActionType::ChestTransfer, /*slot*/ 6, /*deposit*/ 0);
    PlayerCommand chestDepositCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestDepositResult = ApplyPlayerEconomyCommand(*controlled, chestDepositCommand);
    const bool chestDepositOk = chestDepositResult.handled && chestDepositResult.success
        && inv.CountItem(ItemType::AlarmTrap) == invAlarmBeforeChest - 2
        && teamChest.CountItem(ItemType::AlarmTrap) == chestAlarmBefore + 2;
    const PlayerActionResult chestDepositDuplicate = ApplyPlayerEconomyCommand(*controlled, chestDepositCommand);
    const bool chestDedupeOk = !chestDepositDuplicate.handled
        && inv.CountItem(ItemType::AlarmTrap) == invAlarmBeforeChest - 2
        && teamChest.CountItem(ItemType::AlarmTrap) == chestAlarmBefore + 2;
    ItemStack exactChestStack;
    exactChestStack.type = ItemType::Molotov;
    exactChestStack.count = 1;
    teamChest.SwapSlot(8, exactChestStack);
    const int invMolotovBeforeExactChest = inv.CountItem(ItemType::Molotov);
    QueueEconomyAction(PlayerActionType::ChestTransfer, /*slot*/ 8, /*withdraw exact*/ 2);
    PlayerCommand chestExactWithdrawCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestExactWithdrawResult = ApplyPlayerEconomyCommand(*controlled, chestExactWithdrawCommand);
    const bool chestExactWithdrawOk = chestExactWithdrawResult.handled && chestExactWithdrawResult.success
        && inv.CountItem(ItemType::Molotov) == invMolotovBeforeExactChest + 1
        && teamChest.GetSlot(8).IsEmpty();
    QueueEconomyAction(PlayerActionType::ChestTransfer, /*unused*/ 0, /*withdraw*/ 1);
    PlayerCommand chestWithdrawCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestWithdrawResult = ApplyPlayerEconomyCommand(*controlled, chestWithdrawCommand);
    const bool chestWithdrawOk = chestWithdrawResult.handled && chestWithdrawResult.success
        && inv.CountItem(ItemType::AlarmTrap) == invAlarmBeforeChest
        && teamChest.CountItem(ItemType::AlarmTrap) == chestAlarmBefore;

    ItemStack exactDepositStack;
    exactDepositStack.type = ItemType::MedKit;
    exactDepositStack.count = 2;
    inv.SwapSlot(10, exactDepositStack);
    teamChest.SwapSlot(14, ItemStack {});
    QueueEconomyAction(
        PlayerActionType::ChestTransfer,
        10,
        PackPlayerActionParam(static_cast<int>(ChestTransferOp::PlayerToChestSlot), 14, 0));
    PlayerCommand chestExactDepositCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestExactDepositResult =
        ApplyPlayerEconomyCommand(*controlled, chestExactDepositCommand);
    const bool chestExactDepositOk = chestExactDepositResult.handled && chestExactDepositResult.success
        && inv.GetSlot(10).IsEmpty()
        && teamChest.GetSlot(14).type == ItemType::MedKit
        && teamChest.GetSlot(14).count == 2;
    inv.SwapSlot(11, ItemStack {});
    QueueEconomyAction(
        PlayerActionType::ChestTransfer,
        14,
        PackPlayerActionParam(static_cast<int>(ChestTransferOp::ChestToPlayerSlot), 11, 0));
    PlayerCommand chestExactTakeCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestExactTakeResult =
        ApplyPlayerEconomyCommand(*controlled, chestExactTakeCommand);
    const bool chestExactTakeOk = chestExactTakeResult.handled && chestExactTakeResult.success
        && teamChest.GetSlot(14).IsEmpty()
        && inv.GetSlot(11).type == ItemType::MedKit
        && inv.GetSlot(11).count == 2;
    ItemStack chestMoveStack;
    chestMoveStack.type = ItemType::DashPearl;
    chestMoveStack.count = 3;
    teamChest.SwapSlot(20, chestMoveStack);
    teamChest.SwapSlot(21, ItemStack {});
    QueueEconomyAction(
        PlayerActionType::ChestTransfer,
        20,
        PackPlayerActionParam(static_cast<int>(ChestTransferOp::ChestToChestSlot), 21, 0));
    PlayerCommand chestMoveCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestMoveResult = ApplyPlayerEconomyCommand(*controlled, chestMoveCommand);
    const bool chestMoveOk = chestMoveResult.handled && chestMoveResult.success
        && teamChest.GetSlot(20).IsEmpty()
        && teamChest.GetSlot(21).type == ItemType::DashPearl
        && teamChest.GetSlot(21).count == 3;
    ItemStack inventoryMoveExactStack;
    inventoryMoveExactStack.type = ItemType::Molotov;
    inventoryMoveExactStack.count = 2;
    inv.SwapSlot(12, inventoryMoveExactStack);
    inv.SwapSlot(13, ItemStack {});
    QueueEconomyAction(
        PlayerActionType::MoveInventory,
        12,
        PackPlayerActionParam(static_cast<int>(InventoryMoveOp::SlotToSlot), 13, 0));
    PlayerCommand moveExactCommand = BuildLocalPlayerCommand();
    const PlayerActionResult moveExactResult = ApplyPlayerEconomyCommand(*controlled, moveExactCommand);
    const bool moveExactOk = moveExactResult.handled && moveExactResult.success
        && inv.GetSlot(12).IsEmpty()
        && inv.GetSlot(13).type == ItemType::Molotov
        && inv.GetSlot(13).count == 2;
    recentActionResults_.clear();
    PushPlayerActionResultSnapshot(buyResult);
    int enemyPlayerId = -1;
    for (const Player& player : matchSimulation_.Players())
    {
        if (player.GetId() != controlled->GetId())
        {
            enemyPlayerId = player.GetId();
            break;
        }
    }
    const MatchSnapshot actionResultSnapshot = BuildNetworkSnapshot();
    const MatchSnapshot ownerActionResultView = FilterSnapshotForClient(actionResultSnapshot, controlled->GetId());
    const MatchSnapshot enemyActionResultView = enemyPlayerId >= 0
        ? FilterSnapshotForClient(actionResultSnapshot, enemyPlayerId)
        : MatchSnapshot {};
    const bool actionResultReplicated = ownerActionResultView.actionResults.size() == 1
        && ownerActionResultView.actionResults[0].playerId == controlled->GetId()
        && ownerActionResultView.actionResults[0].resultSeq != 0
        && ownerActionResultView.actionResults[0].actionSeq == buyResult.actionSeq
        && ownerActionResultView.actionResults[0].success == buyResult.success
        && ownerActionResultView.actionResults[0].message == buyResult.message
        && enemyPlayerId >= 0
        && enemyActionResultView.actionResults.empty();
    recentActionResults_.clear();

    const bool presentationClean =
        message_ == messageBefore
        && eventMessages_.size() == eventMessagesBefore
        && worldEffects_.size() == worldEffectsBefore
        && floatingTexts_.size() == floatingTextsBefore
        && audio_.IsMuted() == audioMutedBefore;

    const bool ok = buyOk && dedupeOk && deniedFundsOk && deniedRangeOk
        && dropOk && dropDedupeOk && moveOk
        && chestDepositOk && chestDedupeOk && chestExactWithdrawOk && chestWithdrawOk
        && chestExactDepositOk && chestExactTakeOk && chestMoveOk && moveExactOk
        && actionResultReplicated && presentationClean;
    std::cout << "purchase-smoke: buy=" << (buyOk ? "ok" : "FAIL")
              << " (iron " << ironBefore << "->" << ironAfter
              << ", wood " << woodBefore << "->" << woodAfter << ")"
              << " dedupe=" << (dedupeOk ? "ok" : "FAIL")
              << " deniedFunds=" << (deniedFundsOk ? "ok" : "FAIL")
              << " deniedRange=" << (deniedRangeOk ? "ok" : "FAIL")
              << " drop=" << (dropOk ? "ok" : "FAIL")
              << " dropDedupe=" << (dropDedupeOk ? "ok" : "FAIL")
              << " move=" << (moveOk ? "ok" : "FAIL")
              << " chestDeposit=" << (chestDepositOk ? "ok" : "FAIL")
              << " chestDedupe=" << (chestDedupeOk ? "ok" : "FAIL")
              << " chestExactWithdraw=" << (chestExactWithdrawOk ? "ok" : "FAIL")
              << " chestWithdraw=" << (chestWithdrawOk ? "ok" : "FAIL")
              << " chestExactDeposit=" << (chestExactDepositOk ? "ok" : "FAIL")
              << " chestExactTake=" << (chestExactTakeOk ? "ok" : "FAIL")
              << " chestMove=" << (chestMoveOk ? "ok" : "FAIL")
              << " moveExact=" << (moveExactOk ? "ok" : "FAIL")
              << " actionResult=" << (actionResultReplicated ? "ok" : "FAIL")
              << " presentation=" << (presentationClean ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "PURCHASE_SMOKE_OK" : "PURCHASE_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 4;
}

int Game::RunNetworkClientUiSmoke()
{
    networkMode_ = NetworkMode::LocalHost;
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();
    screen_ = GameScreen::Playing;

    Player* serverPlayer = GetLocalPlayer();
    Team* team = serverPlayer != nullptr ? FindTeam(serverPlayer->GetTeamId()) : nullptr;
    if (serverPlayer == nullptr || team == nullptr)
    {
        std::cout << "network-client-ui-smoke: missing server player/team\n";
        std::cout << "NETWORK_CLIENT_UI_SMOKE_FAIL" << std::endl;
        return 4;
    }

    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };
    const int playerId = serverPlayer->GetId();
    serverPlayer->SetPosition(toVec3(team->shopPosition));
    serverPlayer->GetInventory().AddResource(ResourceType::Iron, 32);

    Game client;
    client.Initialize(true);
    client.networkMode_ = NetworkMode::LocalClient;
    client.networkAssignedPlayerId_ = playerId;
    client.localPlayerId_ = playerId;
    LobbySnapshot lobby;
    lobby.worldBiome = static_cast<int>(arenaBiome_);
    lobby.worldLayout = static_cast<int>(arenaLayout_);
    lobby.matchMode = static_cast<int>(selectedMode_);
    client.BuildClientWorld(lobby);
    client.networkAssignedPlayerId_ = playerId;
    client.localPlayerId_ = playerId;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));

    Player* clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    const bool clientReady = clientPlayer != nullptr;

    client.currentInput_ = PlayerInput {};
    client.currentInput_.interactPressed = true;
    client.HandleNetworkClientUiInput();
    const bool shopOpenOk = client.shopOpen_ && !client.inventoryOpen_ && !client.personalChestOpen_;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.exitPressed = true;
    client.HandleNetworkClientUiInput();
    const bool shopEscCloseOk = !client.shopOpen_ && !client.inventoryOpen_ && !client.clientPaused_;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.interactPressed = true;
    client.HandleNetworkClientUiInput();

    const int clientWoodBeforeShop = clientPlayer != nullptr
        ? clientPlayer->GetInventory().GetBlockCount(BlockType::WoodBlock)
        : 0;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.shopChoice = 1;
    if (clientPlayer != nullptr)
    {
        client.HandleNetworkClientShopInput(*clientPlayer);
    }
    const PlayerCommand buyCommand = client.BuildLocalPlayerCommand();
    const bool buyQueued = buyCommand.actionSeq != 0
        && buyCommand.actionType == static_cast<int>(PlayerActionType::BuyItem)
        && clientPlayer != nullptr
        && clientPlayer->GetInventory().GetBlockCount(BlockType::WoodBlock) == clientWoodBeforeShop;
    const PlayerActionResult buyResult = ApplyPlayerEconomyCommand(*serverPlayer, buyCommand);
    client.pendingEconomyActionType_ = PlayerActionType::None;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    const bool buyReflected = buyResult.handled && buyResult.success
        && clientPlayer != nullptr
        && clientPlayer->GetInventory().GetBlockCount(BlockType::WoodBlock) >= clientWoodBeforeShop + 32;

    serverPlayer->SetPosition(toVec3(TeamChestDepositPosition(*team)));
    ItemStack chestSource;
    chestSource.type = ItemType::MedKit;
    chestSource.count = 2;
    serverPlayer->GetInventory().SwapSlot(10, chestSource);
    teamChests_[team->id].SwapSlot(14, ItemStack {});
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    const bool chestOpenOk = clientPlayer != nullptr
        && client.OpenTeamChestUi(*clientPlayer)
        && client.inventoryOpen_
        && client.teamChestOpen_
        && !client.personalChestOpen_;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.exitPressed = true;
    client.HandleNetworkClientUiInput();
    const bool chestEscCloseOk = !client.inventoryOpen_ && !client.teamChestOpen_ && !client.clientPaused_;

    const int clientChestMedBefore = client.teamChests_[team->id].CountItem(ItemType::MedKit);
    client.QueuePlayerAction(
        PlayerActionType::ChestTransfer,
        10,
        PackPlayerActionParam(static_cast<int>(ChestTransferOp::PlayerToChestSlot), 14, 0));
    const PlayerCommand chestCommand = client.BuildLocalPlayerCommand();
    const bool chestQueued = chestCommand.actionSeq != 0
        && chestCommand.actionType == static_cast<int>(PlayerActionType::ChestTransfer)
        && client.teamChests_[team->id].CountItem(ItemType::MedKit) == clientChestMedBefore;
    const PlayerActionResult chestResult = ApplyPlayerEconomyCommand(*serverPlayer, chestCommand);
    client.pendingEconomyActionType_ = PlayerActionType::None;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    const bool chestReflected = chestResult.handled && chestResult.success
        && client.teamChests_[team->id].GetSlot(14).type == ItemType::MedKit
        && client.teamChests_[team->id].GetSlot(14).count == 2;

    ItemStack moveSource;
    moveSource.type = ItemType::Molotov;
    moveSource.count = 1;
    serverPlayer->GetInventory().SwapSlot(12, moveSource);
    serverPlayer->GetInventory().SwapSlot(13, ItemStack {});
    ItemStack dropSource;
    dropSource.type = ItemType::Fireball;
    dropSource.count = 3;
    serverPlayer->GetInventory().SwapSlot(3, dropSource);
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    client.QueuePlayerAction(
        PlayerActionType::MoveInventory,
        12,
        PackPlayerActionParam(static_cast<int>(InventoryMoveOp::SlotToSlot), 13, 0));
    const PlayerCommand moveCommand = client.BuildLocalPlayerCommand();
    const PlayerActionResult moveResult = ApplyPlayerEconomyCommand(*serverPlayer, moveCommand);
    client.pendingEconomyActionType_ = PlayerActionType::None;
    client.QueuePlayerAction(PlayerActionType::DropItem, 3, 2);
    const PlayerCommand dropCommand = client.BuildLocalPlayerCommand();
    const std::size_t droppedBefore = matchSimulation_.DroppedItems().size();
    const PlayerActionResult dropResult = ApplyPlayerEconomyCommand(*serverPlayer, dropCommand);
    client.pendingEconomyActionType_ = PlayerActionType::None;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    const bool moveDropReflected = moveResult.handled && moveResult.success
        && dropResult.handled && dropResult.success
        && matchSimulation_.DroppedItems().size() == droppedBefore + 1
        && clientPlayer != nullptr
        && clientPlayer->GetInventory().GetSlot(13).type == ItemType::Molotov
        && clientPlayer->GetInventory().GetSlot(3).type == ItemType::Fireball
        && clientPlayer->GetInventory().GetSlot(3).count == 1;

    PlayerMatchScore& score = GetPlayerScore(playerId);
    score.kills = 3;
    score.deaths = 2;
    score.finalDeaths = 1;
    score.coreDamage = 77;
    score.coresDestroyed = 1;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    const PlayerMatchScore* clientScore = client.FindPlayerScore(playerId);
    const bool scoreReplicated = clientScore != nullptr
        && clientScore->kills == 3
        && clientScore->deaths == 2
        && clientScore->finalDeaths == 1
        && clientScore->coreDamage == 77
        && clientScore->coresDestroyed == 1;

    client.networkMode_ = NetworkMode::LocalClient;
    client.scoreboardHeld_ = false;
    client.scoreboardHeld_ = true;
    const bool tabScoreboardOk = client.scoreboardHeld_;

    serverPlayer->GetInventory().SwapSlot(0, ItemStack { ItemType::SniperRifle, 1 });
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    if (clientPlayer != nullptr)
    {
        clientPlayer->SetControlKind(PlayerControlKind::LocalHumanPredicted);
    }
    client.selectedHotbarSlot_ = 0;
    client.sniperMagnification_ = 1.5f;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.scopeHeld = true;
    client.currentInput_.mouseWheel = 1.0f;
    const int slotBeforeScopeWheel = client.selectedHotbarSlot_;
    if (clientPlayer != nullptr)
    {
        client.HandleNetworkClientLookAndHotbarInput(*clientPlayer);
    }
    const bool sniperZoomWheelOk = clientPlayer != nullptr
        && client.selectedHotbarSlot_ == slotBeforeScopeWheel
        && client.sniperMagnification_ > 1.5f;

    PlayerCommand chargeCommand;
    chargeCommand.controlledPlayerId = static_cast<std::uint32_t>(playerId);
    chargeCommand.selectedSlot = 0;
    chargeCommand.attackHeld = true;
    if (clientPlayer != nullptr)
    {
        clientPlayer->CancelBlasterLoading();
        client.UpdatePredictedRangedCharge(*clientPlayer, chargeCommand, 0.20f);
    }
    const bool sniperChargePredicted = clientPlayer != nullptr
        && clientPlayer->GetBlasterState() == CrossbowState::Loading
        && clientPlayer->GetBlasterLoadTimer() > 0.0f;

    client.currentInput_ = PlayerInput {};
    client.currentInput_.exitPressed = true;
    client.HandleNetworkClientUiInput();
    const bool escPauseOk = client.clientPaused_ && !client.inventoryOpen_ && !client.shopOpen_;

    const bool personalHidden = !client.personalChestOpen_;
    const bool ok = clientReady && shopOpenOk && buyQueued && buyReflected
        && chestOpenOk && chestQueued && chestReflected
        && moveDropReflected && scoreReplicated && tabScoreboardOk
        && shopEscCloseOk && chestEscCloseOk && escPauseOk
        && sniperZoomWheelOk && sniperChargePredicted
        && personalHidden;
    std::cout << "network-client-ui-smoke: clientReady=" << (clientReady ? "ok" : "FAIL")
              << " shopOpen=" << (shopOpenOk ? "ok" : "FAIL")
              << " shopEsc=" << (shopEscCloseOk ? "ok" : "FAIL")
              << " buyQueued=" << (buyQueued ? "ok" : "FAIL")
              << " buySnapshot=" << (buyReflected ? "ok" : "FAIL")
              << " chestOpen=" << (chestOpenOk ? "ok" : "FAIL")
              << " chestEsc=" << (chestEscCloseOk ? "ok" : "FAIL")
              << " chestQueued=" << (chestQueued ? "ok" : "FAIL")
              << " chestSnapshot=" << (chestReflected ? "ok" : "FAIL")
              << " moveDropSnapshot=" << (moveDropReflected ? "ok" : "FAIL")
              << " score=" << (scoreReplicated ? "ok" : "FAIL")
              << " tab=" << (tabScoreboardOk ? "ok" : "FAIL")
              << " escPause=" << (escPauseOk ? "ok" : "FAIL")
              << " sniperZoom=" << (sniperZoomWheelOk ? "ok" : "FAIL")
              << " sniperCharge=" << (sniperChargePredicted ? "ok" : "FAIL")
              << " personalHidden=" << (personalHidden ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "NETWORK_CLIENT_UI_SMOKE_OK" : "NETWORK_CLIENT_UI_SMOKE_FAIL") << std::endl;
    client.Shutdown();
    return ok ? 0 : 4;
}

int Game::RunIntegratedServerSmoke()
{
    // Phase 6 skeleton: plain singleplayer connects the local human as a loopback
    // client of the same authoritative pipeline multiplayer uses. Hybrid — the
    // direct SP path still drives gameplay; economy is the first migrated system.
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();
    screen_ = GameScreen::Playing;

    Player* controlled = GetLocalPlayer();
    Team* team = controlled != nullptr ? FindTeam(controlled->GetTeamId()) : nullptr;
    if (controlled == nullptr || team == nullptr)
    {
        std::cout << "integrated-server-smoke: no controlled player/team\n";
        std::cout << "INTEGRATED_SERVER_SMOKE_FAIL" << std::endl;
        return 4;
    }

    // SetupMatch auto-starts the integrated server for LocalSinglePlayer.
    const bool startedOk = integratedServerActive_ && integratedServer_.IsRunning()
        && integratedServer_.PlayerForClient(kIntegratedServerClientId) == controlled->GetId()
        && integratedServer_.ClientForPlayer(controlled->GetId()) == kIntegratedServerClientId;

    // --- Part 1: the per-tick channel runs alongside the unchanged SP sim. ----
    // Headless UpdateMatchSimulation skips the presentation block that ticks the
    // integrated server in-game, so the smoke drives the tick explicitly.
    constexpr int kTicks = 30;
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    for (int i = 0; i < kTicks; ++i)
    {
        UpdateMatchSimulation(fixedDt);
        IntegratedServerTick(fixedDt);
    }

    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };
    const MatchSnapshot& snap = integratedServer_.LatestSnapshot(kIntegratedServerClientId);
    const PlayerSnapshot* self = nullptr;
    for (const PlayerSnapshot& entry : snap.players)
    {
        if (entry.playerId == controlled->GetId())
        {
            self = &entry;
            break;
        }
    }
    const float posError = self != nullptr
        ? (self->position - toVec3(controlled->GetPosition())).Length()
        : 1e9f;
    const bool channelOk =
        integratedServerCommandsDrained_ == static_cast<std::uint32_t>(kTicks)
        && integratedServer_.SnapshotsPublished() == static_cast<std::uint32_t>(kTicks)
        && snap.tick == matchSimulation_.CurrentTick()
        && self != nullptr && posError < 0.001f
        && self->inventory.present;

    // --- Part 2: a shop purchase crosses the SAME transport + server method. --
    // Headless initialization suppresses local feedback; the smoke asserts the
    // result message is presented, so re-enable it like the purchase smoke does.
    suppressLocalFeedback_ = false;
    Inventory& inv = controlled->GetInventory();
    controlled->SetPosition(toVec3(team->shopPosition));
    inv.AddResource(ResourceType::Iron, 5);
    const int ironBefore = inv.GetResource(ResourceType::Iron);
    const int woodBefore = inv.GetBlockCount(BlockType::WoodBlock);
    const std::uint32_t drainedBeforeBuy = integratedServerCommandsDrained_;
    QueuePlayerAction(PlayerActionType::BuyItem, /*choice*/ 1, /*repeat*/ 1);
    ApplyPendingLocalPlayerAction(*controlled);
    const bool buyOk = integratedServerCommandsDrained_ == drainedBeforeBuy + 1
        && integratedServerEconomyApplied_ == 1
        && inv.GetResource(ResourceType::Iron) == ironBefore - 5
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBefore + 32
        && !message_.empty()
        && pendingEconomyActionType_ == PlayerActionType::None;

    // Resending the SAME wire command must not buy twice (server-side dedupe).
    PlayerCommand resent = BuildLocalPlayerCommand();
    resent.actionSeq = clientEconomyActionSeq_;
    resent.actionType = static_cast<int>(PlayerActionType::BuyItem);
    resent.actionParamA = 1;
    resent.actionParamB = 1;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, resent);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool dedupeOk = integratedServerEconomyApplied_ == 1
        && inv.GetResource(ResourceType::Iron) == ironBefore - 5
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBefore + 32;

    // A denied purchase (out of shop range) also crosses the transport and is
    // rejected by the same server validation multiplayer relies on.
    controlled->SetPosition(toVec3(Vector3 {
        team->shopPosition.x + 100.0f, team->shopPosition.y, team->shopPosition.z }));
    const int ironBeforeFar = inv.GetResource(ResourceType::Iron);
    QueuePlayerAction(PlayerActionType::BuyItem, 1, 1);
    ApplyPendingLocalPlayerAction(*controlled);
    const bool deniedOk = integratedServerEconomyApplied_ == 2
        && inv.GetResource(ResourceType::Iron) == ironBeforeFar
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBefore + 32;

    // --- Part 3 (block slice): a block place crosses the SAME transport and the
    // authoritative place path (ApplyNetworkBlockPlace) — and lands exactly once.
    controlled->SetPosition(Vec3 { 0.0f, 40.0f, 0.0f });
    ItemStack woodStack;
    woodStack.type = ItemFromBlock(BlockType::WoodBlock);
    woodStack.count = 8;
    inv.SwapSlot(0, woodStack);
    selectedHotbarSlot_ = 0; // the locally predicted player reads the UI slot

    PlayerCommand placeCmd = BuildLocalPlayerCommand();
    placeCmd.aimYaw = PI / 2.0f; // aim = (+1, 0, 0)
    placeCmd.aimPitch = 0.0f;
    placeCmd.placePressed = true;
    placeCmd.placeHeld = true;
    // A free-floating anchor in clear air ahead of the eye gives the command's
    // raycast a deterministic target; the block lands on the anchor's near face.
    const Vector3 placeForward = AimDirectionFromCommand(placeCmd);
    const Vector3 placeEye { 0.0f, 40.0f + 0.78f, 0.0f };
    const GridPos anchorCell = world_.WorldToGrid(Vector3 {
        placeEye.x + placeForward.x * 2.0f, placeEye.y, placeEye.z + placeForward.z * 2.0f });
    world_.PlaceBlock(anchorCell, Block { BlockType::StoneBlock, -1, true }, true);
    const GridPos expectedCell { anchorCell.x - 1, anchorCell.y, anchorCell.z };
    const std::size_t blocksBeforePlace = world_.GetBlocks().size();
    const int woodBeforePlace = inv.GetBlockCount(BlockType::WoodBlock);
    const int statsPlacedBefore = stats_.blocksPlaced;
    message_.clear();
    integratedServer_.SubmitCommand(kIntegratedServerClientId, placeCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const Block* placedBlock = world_.GetBlock(expectedCell);
    const bool placeOk = integratedServerBlocksPlaced_ == 1
        && world_.GetBlocks().size() == blocksBeforePlace + 1
        && placedBlock != nullptr && placedBlock->type == BlockType::WoodBlock
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBeforePlace - 1
        && stats_.blocksPlaced == statsPlacedBefore + 1
        && !message_.empty();

    // Held place input keeps riding the per-tick commands; the server-side rate
    // limiter must keep that to ONE placement per cooldown window (no double
    // apply now that HandlePlaceBlock's direct call is gated off).
    placeCmd.placePressed = false;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, placeCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool placeOnceOk = integratedServerBlocksPlaced_ == 1
        && world_.GetBlocks().size() == blocksBeforePlace + 1
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBeforePlace - 1;

    // --- Part 4 (hero-ability slice): a cast crosses the SAME transport and the
    // authoritative action entry (ApplyPlayerActionCommand) — and fires exactly
    // once: the cooldown starts, and the result is presented directly.
    controlled->SetHeroId(HeroId::Likho); // cooldown-gated Active1, no target needed
    const float castCooldownBefore = controlled->GetHeroState().active1.cooldownRemaining;
    message_.clear();
    PlayerCommand castCmd = BuildLocalPlayerCommand();
    castCmd.useAbility1 = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, castCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const float castCooldownAfter = controlled->GetHeroState().active1.cooldownRemaining;
    const bool castOk = integratedServerHeroCastsApplied_ == 1
        && castCooldownBefore <= 0.0f
        && castCooldownAfter > 0.0f
        && !message_.empty();

    // Resubmitting the same cast while on cooldown must be a server-side no-op:
    // no second successful cast, and the running cooldown is not restarted.
    integratedServer_.SubmitCommand(kIntegratedServerClientId, castCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool castOnceOk = integratedServerHeroCastsApplied_ == 1
        && controlled->GetHeroState().active1.cooldownRemaining == castCooldownAfter;

    // --- Part 5 (utility slice): a heal crosses the SAME transport and the
    // command-driven utility path (UseUtilityInputs) — the medkit is consumed
    // exactly once and the result is presented directly.
    controlled->Heal(controlled->GetMaxHealth());
    controlled->Damage(60);
    ItemStack medkits;
    medkits.type = ItemFromUtility(UtilityType::Heal);
    medkits.count = 2;
    inv.SwapSlot(1, medkits);
    const int healthBeforeHeal = controlled->GetHealth();
    message_.clear();
    PlayerCommand healCmd = BuildLocalPlayerCommand();
    healCmd.useHeal = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, healCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const ItemStack medkitSlotAfter = inv.GetHotbarSlots()[1];
    const bool healOk = controlled->GetHealth() == healthBeforeHeal + 45
        && medkitSlotAfter.count == 1
        && !message_.empty();

    // --- Part 5b (unified right-click use): using the SELECTED medkit rides
    // the command's placePressed through ApplyNetworkPlayerActions — the same
    // code a real network client's right click hits. Exactly one charge per
    // press; a held button without a new press must not spend another.
    selectedHotbarSlot_ = 1; // the remaining medkit (UI slot mirror)
    controlled->Damage(60);
    const int healthBeforeRightClick = controlled->GetHealth();
    message_.clear();
    PlayerCommand useSelectedCmd = BuildLocalPlayerCommand();
    useSelectedCmd.selectedSlot = 1;
    useSelectedCmd.placePressed = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, useSelectedCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    useSelectedCmd.placePressed = false;
    useSelectedCmd.placeHeld = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, useSelectedCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool rightClickUtilityOk = controlled->GetHealth() == healthBeforeRightClick + 45
        && inv.GetHotbarSlots()[1].IsEmpty()
        && !message_.empty();

    Player* enemy = nullptr;
    for (Player& candidate : matchSimulation_.Players())
    {
        if (candidate.GetId() != controlled->GetId()
            && candidate.GetTeamId() != controlled->GetTeamId()
            && candidate.IsAlive())
        {
            enemy = &candidate;
            break;
        }
    }
    controlled->SetPosition(Vec3 { 0.0f, 60.0f, 0.0f });
    controlled->SetVelocity(Vec3 {});
    if (enemy != nullptr)
    {
        enemy->SetPosition(Vec3 { 0.0f, 60.0f, -1.6f });
        enemy->SetVelocity(Vec3 {});
        enemy->UpdateTimers(2.0f);
    }
    ItemStack sword;
    sword.type = ItemType::Sword;
    sword.count = 1;
    inv.SwapSlot(0, sword);
    controlled->SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;
    const int enemyHpBeforeDirect = enemy != nullptr ? enemy->GetHealth() : -1;
    currentInput_.attackPressed = true;
    currentInput_.attackHeld = true;
    UpdateAttackOrBreak(0.05f);
    currentInput_ = PlayerInput {};
    const bool directCombatSuppressed = enemy != nullptr
        && enemy->GetHealth() == enemyHpBeforeDirect;

    PlayerCommand meleeCmd = BuildLocalPlayerCommand();
    meleeCmd.controlledPlayerId = static_cast<std::uint32_t>(controlled->GetId());
    meleeCmd.selectedSlot = 0;
    meleeCmd.aimYaw = 0.0f;
    meleeCmd.aimPitch = 0.0f;
    meleeCmd.attackPressed = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, meleeCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, 0.05f);
    }
    const bool meleeOk = enemy != nullptr
        && enemy->GetHealth() < enemyHpBeforeDirect;

    if (enemy != nullptr)
    {
        enemy->SetPosition(Vec3 { 100.0f, 60.0f, 100.0f });
    }
    ItemStack pickaxe;
    pickaxe.type = ItemType::Pickaxe;
    pickaxe.count = 1;
    inv.SwapSlot(0, pickaxe);
    controlled->SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;
    world_.Clear();
    PlayerCommand breakTemplate;
    breakTemplate.controlledPlayerId = static_cast<std::uint32_t>(controlled->GetId());
    breakTemplate.selectedSlot = 0;
    breakTemplate.aimYaw = 0.0f;
    breakTemplate.aimPitch = 0.0f;
    breakTemplate.attackHeld = true;
    const Vector3 breakForward = AimDirectionFromCommand(breakTemplate);
    const Vector3 breakEye { 0.0f, 60.0f + 0.78f, 0.0f };
    const GridPos breakCell = world_.WorldToGrid(Vector3 {
        breakEye.x + breakForward.x * 2.0f,
        breakEye.y + breakForward.y * 2.0f,
        breakEye.z + breakForward.z * 2.0f });
    world_.PlaceBlock(breakCell, Block { BlockType::WoodBlock, -1, true }, true);
    const std::size_t blocksBeforeBreak = world_.GetBlocks().size();
    bool breakOk = false;
    for (int i = 0; i < 160 && !breakOk; ++i)
    {
        PlayerCommand breakCmd = breakTemplate;
        breakCmd.tick = static_cast<std::uint32_t>(i + 1);
        integratedServer_.SubmitCommand(kIntegratedServerClientId, breakCmd);
        for (const PlayerCommand& received : integratedServer_.DrainCommands())
        {
            ++integratedServerCommandsDrained_;
            ApplyIntegratedServerCommand(received, 0.05f);
        }
        breakOk = world_.GetBlocks().size() < blocksBeforeBreak;
    }

    ItemStack bow;
    bow.type = ItemType::Bow;
    bow.count = 1;
    ItemStack arrows;
    arrows.type = ItemType::EnergyArrow;
    arrows.count = 3;
    inv.SwapSlot(0, bow);
    inv.SwapSlot(1, arrows);
    controlled->SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;
    controlled->ResetBowDraw();
    const std::size_t projectilesBeforeBow = projectiles_.size();
    PlayerCommand bowCmd;
    bowCmd.controlledPlayerId = static_cast<std::uint32_t>(controlled->GetId());
    bowCmd.selectedSlot = 0;
    bowCmd.aimYaw = 0.0f;
    bowCmd.aimPitch = 0.0f;
    bowCmd.attackHeld = true;
    for (int i = 0; i < 48; ++i)
    {
        bowCmd.tick = static_cast<std::uint32_t>(i + 1);
        integratedServer_.SubmitCommand(kIntegratedServerClientId, bowCmd);
        for (const PlayerCommand& received : integratedServer_.DrainCommands())
        {
            ++integratedServerCommandsDrained_;
            ApplyIntegratedServerCommand(received, fixedDt);
        }
    }
    PlayerCommand bowRelease = bowCmd;
    bowRelease.attackHeld = false;
    bowRelease.attackReleased = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, bowRelease);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool rangedOk = projectiles_.size() == projectilesBeforeBow + 1
        && projectiles_.back().kind == ProjectileKind::Arrow
        && controlled->GetBowDrawTimer() <= 0.0f;

    const bool ok = startedOk && channelOk && buyOk && dedupeOk && deniedOk
        && placeOk && placeOnceOk && castOk && castOnceOk && healOk
        && rightClickUtilityOk
        && directCombatSuppressed && meleeOk && breakOk && rangedOk;
    std::cout << "integrated-server-smoke: started=" << (startedOk ? "ok" : "FAIL")
              << " channel=" << (channelOk ? "ok" : "FAIL")
              << " (drained=" << integratedServerCommandsDrained_
              << " snapshots=" << integratedServer_.SnapshotsPublished()
              << " snapTick=" << snap.tick
              << " posError=" << posError << ")"
              << " buy=" << (buyOk ? "ok" : "FAIL")
              << " (iron " << ironBefore << "->" << inv.GetResource(ResourceType::Iron)
              << ", wood " << woodBefore << "->" << inv.GetBlockCount(BlockType::WoodBlock) << ")"
              << " dedupe=" << (dedupeOk ? "ok" : "FAIL")
              << " deniedRange=" << (deniedOk ? "ok" : "FAIL")
              << " place=" << (placeOk ? "ok" : "FAIL")
              << " (blocks " << blocksBeforePlace << "->" << world_.GetBlocks().size()
              << ", wood " << woodBeforePlace << "->" << inv.GetBlockCount(BlockType::WoodBlock)
              << ", placedViaServer=" << integratedServerBlocksPlaced_ << ")"
              << " placeOnce=" << (placeOnceOk ? "ok" : "FAIL")
              << " heroCast=" << (castOk ? "ok" : "FAIL")
              << " (cooldown " << castCooldownBefore << "->" << castCooldownAfter
              << ", castsViaServer=" << integratedServerHeroCastsApplied_ << ")"
              << " castOnce=" << (castOnceOk ? "ok" : "FAIL")
              << " heal=" << (healOk ? "ok" : "FAIL")
              << " (hp " << healthBeforeHeal << "->" << controlled->GetHealth()
              << ", medkits 2->" << medkitSlotAfter.count << ")"
              << " rightClickUtility=" << (rightClickUtilityOk ? "ok" : "FAIL")
              << " directCombat=" << (directCombatSuppressed ? "ok" : "FAIL")
              << " melee=" << (meleeOk ? "ok" : "FAIL")
              << " break=" << (breakOk ? "ok" : "FAIL")
              << " ranged=" << (rangedOk ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "INTEGRATED_SERVER_SMOKE_OK" : "INTEGRATED_SERVER_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 4;
}

int Game::RunLoopbackTwoClientSmoke()
{
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }

    // In-process "server + two clients" — no sockets. The server is this Game's
    // authoritative match; the transport carries commands in and per-client
    // snapshots out.
    LoopbackTransport transport;
    transport.Configure(serverConfig_);
    transport.Start();

    // Real headless match so the snapshots carry genuine state.
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();
    screen_ = GameScreen::Playing;

    const auto findPlayerById = [this](std::uint32_t id) -> Player*
    {
        for (Player& player : players_)
        {
            if (static_cast<std::uint32_t>(player.GetId()) == id)
            {
                return &player;
            }
        }
        return nullptr;
    };
    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };

    // Two DIFFERENT players: client A drives the local player, client B drives a
    // player on another team.
    Player* localPlayer = GetLocalPlayer();
    const int playerAId = localPlayer != nullptr ? localPlayer->GetId() : -1;
    const int teamA = localPlayer != nullptr ? localPlayer->GetTeamId() : -1;
    int playerBId = -1;
    for (const Player& player : matchSimulation_.Players())
    {
        if (player.GetId() != playerAId && player.GetTeamId() != teamA && player.IsAlive())
        {
            playerBId = player.GetId();
            break;
        }
    }

    constexpr int kClientA = 101;
    constexpr int kClientB = 202;

    // Mapping: bind each client to its player. Binding a second client to an
    // already-owned player, or reusing a client id, must be rejected — that is
    // the connection-time guarantee that two clients never share one player.
    const bool connectA = transport.Connect(kClientA, playerAId);
    const bool connectB = transport.Connect(kClientB, playerBId);
    const bool duplicatePlayerRejected = !transport.Connect(303, playerAId);
    const bool duplicateClientRejected = !transport.Connect(kClientA, playerBId);

    Player* pa = findPlayerById(static_cast<std::uint32_t>(playerAId));
    Player* pb = findPlayerById(static_cast<std::uint32_t>(playerBId));
    const Vec3 startA = pa != nullptr ? toVec3(pa->GetPosition()) : Vec3 {};
    const Vec3 startB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    const float yawA = pa != nullptr ? pa->GetYaw() : 0.0f;
    const float yawB = pb != nullptr ? pb->GetYaw() : 0.0f;

    constexpr int kTicks = 90;
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    int commandsAppliedA = 0;
    int commandsAppliedB = 0;

    for (int i = 0; i < kTicks; ++i)
    {
        // Client A: move forward along its own facing.
        PlayerCommand cmdA;
        cmdA.controlledPlayerId = static_cast<std::uint32_t>(playerAId);
        cmdA.tick = matchSimulation_.CurrentTick();
        cmdA.aimYaw = yawA;
        cmdA.moveForward = 1.0f;
        transport.SubmitCommand(kClientA, cmdA);

        // Client B: a distinct command (its own facing) for a distinct player.
        PlayerCommand cmdB;
        cmdB.controlledPlayerId = static_cast<std::uint32_t>(playerBId);
        cmdB.tick = matchSimulation_.CurrentTick();
        cmdB.aimYaw = yawB;
        cmdB.moveForward = 1.0f;
        transport.SubmitCommand(kClientB, cmdB);

        // Server: drain both clients' commands into the sim intake, then apply
        // each to the (mapping-stamped) player it targets.
        for (const PlayerCommand& received : transport.DrainCommands())
        {
            matchSimulation_.SubmitCommand(received);
        }
        for (const PlayerCommand& received : matchSimulation_.DrainCommands())
        {
            Player* target = findPlayerById(received.controlledPlayerId);
            if (target != nullptr && target->IsAlive())
            {
                ApplyPlayerCommand(*target, received, fixedDt);
                if (received.controlledPlayerId == static_cast<std::uint32_t>(playerAId))
                {
                    ++commandsAppliedA;
                }
                else if (received.controlledPlayerId == static_cast<std::uint32_t>(playerBId))
                {
                    ++commandsAppliedB;
                }
            }
        }

        // Server advances the single authoritative tick/clock for everyone.
        matchSimulation_.AdvanceTick();
        matchSimulation_.AdvanceClock(fixedDt);

        // Server publishes a per-client, visibility-filtered snapshot (the
        // visibility hook is already wired: each client sees its OWN inventory).
        const MatchSnapshot snapshotA = BuildNetworkSnapshotForClient(playerAId);
        const MatchSnapshot snapshotB = BuildNetworkSnapshotForClient(playerBId);
        transport.PublishSnapshot(kClientA, snapshotA);
        transport.PublishSnapshot(kClientB, snapshotB);
        PushRemoteSnapshot(snapshotA);
    }

    // --- Gather results (captured BEFORE the spoof tick below, so they match
    // the snapshots published at the end of the loop) ------------------------
    const Vec3 endA = pa != nullptr ? toVec3(pa->GetPosition()) : Vec3 {};
    const Vec3 endB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    const float movedA = (endA - startA).Length();
    const float movedB = (endB - startB).Length();

    const MatchSnapshot& snapA = transport.LatestSnapshot(kClientA);
    const MatchSnapshot& snapB = transport.LatestSnapshot(kClientB);

    const auto findEntry = [](const MatchSnapshot& snap, int id) -> const PlayerSnapshot*
    {
        for (const PlayerSnapshot& entry : snap.players)
        {
            if (entry.playerId == id)
            {
                return &entry;
            }
        }
        return nullptr;
    };
    const auto posError = [](const PlayerSnapshot* entry, const Vec3& live) -> float
    {
        return entry != nullptr ? (entry->position - live).Length() : 1e9f;
    };

    // Both clients see BOTH players at their authoritative positions (public).
    const PlayerSnapshot* aSeesA = findEntry(snapA, playerAId);
    const PlayerSnapshot* aSeesB = findEntry(snapA, playerBId);
    const PlayerSnapshot* bSeesA = findEntry(snapB, playerAId);
    const PlayerSnapshot* bSeesB = findEntry(snapB, playerBId);
    const bool authoritativePositions =
        posError(aSeesA, endA) < 0.001f && posError(aSeesB, endB) < 0.001f
        && posError(bSeesA, endA) < 0.001f && posError(bSeesB, endB) < 0.001f;

    // tick/snapshot consistency: one authoritative tick, identical in both views.
    const std::uint32_t serverTick = matchSimulation_.CurrentTick();
    const bool tickConsistent = snapA.tick == serverTick && snapB.tick == serverTick
        && snapA.tick == snapB.tick;

    // Visibility hook is live: each client sees its OWN inventory, not the other's.
    const bool visibilityApplied =
        aSeesA != nullptr && aSeesA->inventory.present
        && aSeesB != nullptr && !aSeesB->inventory.present
        && bSeesB != nullptr && bSeesB->inventory.present
        && bSeesA != nullptr && !bSeesA->inventory.present;

    Vec3 interpolatedB {};
    const bool remoteInterpolationAvailable =
        TryGetInterpolatedRemotePlayerPosition(playerBId, fixedDt * 2.0f, interpolatedB);
    const float interpolatedMovedB = (interpolatedB - startB).Length();
    const float interpolatedEndErrorB = (interpolatedB - endB).Length();
    const bool remoteInterpolationOk = remoteInterpolationAvailable
        && remoteSnapshotBuffer_.size() >= 3
        && interpolatedMovedB > 0.1f
        && interpolatedEndErrorB < std::max(1.0f, movedB);

    // Spoof: client A claims to control player B. The transport re-stamps the
    // command to player A, so player B must NOT move from A's command — the
    // runtime guarantee that two clients can't drive the same player. (Run last,
    // after the snapshot comparison above, since it moves player A.)
    const Vec3 spoofStartB = endB;
    PlayerCommand spoof;
    spoof.controlledPlayerId = static_cast<std::uint32_t>(playerBId); // the lie
    spoof.tick = matchSimulation_.CurrentTick();
    spoof.aimYaw = yawB;
    spoof.moveForward = 1.0f;
    transport.SubmitCommand(kClientA, spoof);
    const std::vector<PlayerCommand> spoofDrained = transport.DrainCommands();
    const bool spoofStamped = spoofDrained.size() == 1
        && spoofDrained.front().controlledPlayerId == static_cast<std::uint32_t>(playerAId);
    for (const PlayerCommand& received : spoofDrained)
    {
        Player* target = findPlayerById(received.controlledPlayerId);
        if (target != nullptr && target->IsAlive())
        {
            ApplyPlayerCommand(*target, received, fixedDt);
        }
    }
    const Vec3 spoofEndB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    const bool spoofPlayerBUnchanged = (spoofEndB - spoofStartB).Length() < 1e-4f;

    networkAssignedPlayerId_ = playerAId;
    const Vec3 renderStepStartB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    UpdateRemoteInterpolation(fixedDt * 0.5f);
    const Vec3 renderStepMidB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    UpdateRemoteInterpolation(fixedDt * 0.5f);
    const Vec3 renderStepEndB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    const bool remoteInterpolationAdvancesBetweenSnapshots =
        (renderStepMidB - renderStepStartB).Length() > 0.0001f
        && (renderStepEndB - renderStepMidB).Length() > 0.0001f;

    const bool differentPlayers = playerAId != playerBId && playerAId >= 0 && playerBId >= 0;
    const bool mappingOk = connectA && connectB && duplicatePlayerRejected && duplicateClientRejected
        && transport.PlayerForClient(kClientA) == playerAId
        && transport.PlayerForClient(kClientB) == playerBId
        && transport.ClientForPlayer(playerAId) == kClientA
        && transport.ClientForPlayer(playerBId) == kClientB
        && transport.ClientCount() == 2;
    const bool bothMoved = movedA > 0.5f && movedB > 0.5f;
    const bool commandsApplied = commandsAppliedA == kTicks && commandsAppliedB == kTicks;
    const bool noSharedControl = spoofStamped && spoofPlayerBUnchanged
        && transport.ClientForPlayer(playerBId) == kClientB;

    std::cout << "loopback-smoke: clients=" << transport.ClientCount()
              << " A(client=" << kClientA << ",player=" << playerAId << ",team=" << teamA << ')'
              << " B(client=" << kClientB << ",player=" << playerBId << ')'
              << " mapping=" << (mappingOk ? "ok" : "FAIL")
              << " (dupPlayer=" << (duplicatePlayerRejected ? "rejected" : "ALLOWED")
              << ",dupClient=" << (duplicateClientRejected ? "rejected" : "ALLOWED") << ")\n";
    std::cout << "loopback-smoke: serverTick=" << serverTick
              << " snapTickA=" << snapA.tick << " snapTickB=" << snapB.tick
              << " snapshotsPublished=" << transport.SnapshotsPublished()
              << " commandsApplied A/B=" << commandsAppliedA << '/' << commandsAppliedB
              << " movedA=" << movedA << " movedB=" << movedB << '\n';
    std::cout << "loopback-smoke: posErr A.A=" << posError(aSeesA, endA)
              << " A.B=" << posError(aSeesB, endB)
              << " B.A=" << posError(bSeesA, endA)
              << " B.B=" << posError(bSeesB, endB)
              << " visibility(ownInventory)=" << (visibilityApplied ? "ok" : "FAIL")
              << " spoof(stamped=" << (spoofStamped ? "yes" : "no")
              << ",B_unchanged=" << (spoofPlayerBUnchanged ? "yes" : "no") << ")\n";
    std::cout << "loopback-smoke: interpolation buffer=" << remoteSnapshotBuffer_.size()
              << " B.pos=(" << interpolatedB.x << ',' << interpolatedB.y << ',' << interpolatedB.z << ')'
              << " moved=" << interpolatedMovedB
              << " endError=" << interpolatedEndErrorB
              << " frameAdvance=" << (remoteInterpolationAdvancesBetweenSnapshots ? "yes" : "no")
              << " status=" << (remoteInterpolationOk ? "ok" : "FAIL") << '\n';

    const bool ok = differentPlayers && mappingOk && bothMoved && commandsApplied
        && authoritativePositions && tickConsistent && visibilityApplied && noSharedControl
        && remoteInterpolationOk && remoteInterpolationAdvancesBetweenSnapshots
        && !snapA.players.empty() && !snapB.players.empty();

    if (!ok)
    {
        std::cout << "loopback-smoke: checks differentPlayers=" << (differentPlayers ? "ok" : "FAIL")
                  << " mapping=" << (mappingOk ? "ok" : "FAIL")
                  << " bothMoved=" << (bothMoved ? "ok" : "FAIL")
                  << " commandsApplied=" << (commandsApplied ? "ok" : "FAIL")
                  << " authPositions=" << (authoritativePositions ? "ok" : "FAIL")
                  << " tickConsistent=" << (tickConsistent ? "ok" : "FAIL")
                  << " visibility=" << (visibilityApplied ? "ok" : "FAIL")
                  << " interpolation=" << (remoteInterpolationOk ? "ok" : "FAIL")
                  << " frameAdvance=" << (remoteInterpolationAdvancesBetweenSnapshots ? "ok" : "FAIL")
                  << " noSharedControl=" << (noSharedControl ? "ok" : "FAIL") << '\n';
    }

    std::cout << (ok ? "LOOPBACK_SMOKE_OK" : "LOOPBACK_SMOKE_FAIL") << std::endl;
    transport.Stop();
    return ok ? 0 : 7;
}

bool Game::IsNetworkControlledPlayer(int playerId) const
{
    return std::find(networkControlledPlayerIds_.begin(), networkControlledPlayerIds_.end(), playerId)
        != networkControlledPlayerIds_.end();
}

void Game::MarkNetworkControlledPlayer(int playerId)
{
    if (playerId < 0)
    {
        return;
    }
    if (!IsNetworkControlledPlayer(playerId))
    {
        networkControlledPlayerIds_.push_back(playerId);
    }
    if (Player* player = matchSimulation_.GetPlayer(playerId))
    {
        player->SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
    }
}

void Game::SetupNetworkMatchFromLobby(ServerTransport& transport, const std::vector<LobbyPlayerState>& roster)
{
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamSize_ = std::clamp(serverConfig_.maxTeamSize, 1, 4);
    selectedBotCount_ = 0;
    if (!roster.empty())
    {
        selectedTeamId_ = std::clamp(roster.front().selectedTeam, 0, TeamCountForMode() - 1);
        selectedHeroId_ = HeroSystem::IdFromIndex(
            std::clamp(roster.front().selectedHero, 0, HeroSystem::kHeroCount - 1));
    }

    pendingNetworkRoster_ = roster;
    SetupMatch();
    pendingNetworkRoster_.clear();
    screen_ = GameScreen::Playing;
    localPlayerServerDriven_ = true;
    networkControlledPlayerIds_.clear();
    serverHeldPlayerCommands_.clear();
    serverEffectiveCommandTickByPlayer_.clear();

    const std::size_t assignedCount = std::min(roster.size(), players_.size());
    for (std::size_t i = 0; i < assignedCount; ++i)
    {
        const int playerId = players_[i].GetId();
        transport.AssignPlayer(roster[i].clientId, playerId);
        if (transport.PlayerForClient(roster[i].clientId) == playerId)
        {
            MarkNetworkControlledPlayer(playerId);
        }
    }
    networkLobbyMatchStarted_ = true;
    networkLobbyMatchStartedBroadcastsRemaining_ = 30;
}

bool Game::NetworkServerSetup(ServerTransport& transport, const ServerConfig& config)
{
    networkMode_ = NetworkMode::DedicatedServer;
    serverConfig_ = config;
    networkControlledPlayerIds_.clear();
    serverHeldPlayerCommands_.clear();
    serverEffectiveCommandTickByPlayer_.clear();
    pendingNetworkRoster_.clear();
    networkLobbyMatchStarted_ = false;
    networkLobbyMatchStarting_ = false;
    networkLobbyMatchStartingTimer_ = 0.0f;
    networkLobbyMatchStartedBroadcastsRemaining_ = 0;
    networkLobbyStartingRoster_.clear();
    transport.SetMatchJoinLocked(false);
    matchSimulation_.Reset();
    localPlayerServerDriven_ = true;

    ServerConfig lobbyConfig = config;
    lobbyConfig.teamCount = 4;
    lobbyConfig.heroCount = HeroSystem::kHeroCount;
    // Advertise the map config so a GUI client can rebuild the same arena (the
    // map is deterministic from biome/layout/mode — see Phase 0.1T).
    lobbyConfig.worldBiome = static_cast<int>(arenaBiome_);
    lobbyConfig.worldLayout = static_cast<int>(arenaLayout_);
    lobbyConfig.matchMode = static_cast<int>(selectedMode_);
    serverConfig_ = lobbyConfig;
    return transport.Start(lobbyConfig);
}

bool Game::ApplyNetworkBlockPlace(Player& player, const PlayerCommand& command, float dt,
                                  bool* outPlaced)
{
    // Phase 6 block slice: the place portion of the authoritative action path.
    // Shared by real network clients (ApplyNetworkPlayerActions) and the SP
    // integrated server (ApplyIntegratedServerCommand) — one validated,
    // rate-limited place implementation, driven entirely by the command.
    NetworkActionState& state = networkActionState_[player.GetId()];
    const std::optional<BlockType> selectedBlock = GetSelectedBlockType(player);
    if (!command.placeHeld || !selectedBlock.has_value())
    {
        state.placeCooldown = 0.0f;
        return false;
    }

    state.breakProgress = BreakProgress {};
    state.placeCooldown -= dt;
    if (state.placeCooldown <= 0.0f)
    {
        const Vector3 aimDirection = AimDirectionFromCommand(command);
        GridPos placePos {};
        bool haveTarget = false;
        if (command.bridgeMode)
        {
            // Bridge: lay the block just below-ahead of the player (yaw-flat).
            const Vector3 flat = player.Forward();
            const float forwardDistance = aimDirection.y < -0.45f ? 0.55f : 0.92f;
            placePos = world_.WorldToGrid(Vector3 {
                player.GetPosition().x + flat.x * forwardDistance,
                player.GetPosition().y - 1.08f,
                player.GetPosition().z + flat.z * forwardDistance
            });
            haveTarget = true;
        }
        else if (const std::optional<RaycastHit> hit = RaycastFromPlayerEye(player, aimDirection, 4.5f))
        {
            placePos = hit->adjacent;
            haveTarget = true;
        }
        if (haveTarget)
        {
            const BlockActionResult result = ApplyPlaceBlockForPlayer(player, placePos);
            if (outPlaced != nullptr)
            {
                *outPlaced = result.success;
            }
            if (result.success || command.placePressed)
            {
                PushBlockActionResultSnapshot(player, result, command.tick);
                // The integrated SP client doesn't consume ActionResultSnapshots
                // for presentation yet (hybrid) — present its own result directly,
                // like the economy slice does. Remote players never take this
                // branch (they present client-side from the snapshot).
                if (IsLocallyPredicted(ControlKindForPlayer(player)))
                {
                    PresentBlockActionResult(player, result, true);
                }
            }
        }
        state.placeCooldown = command.bridgeMode ? 0.16f : 0.22f;
    }
    return true;
}

void Game::ApplyNetworkPlayerActions(Player& player, const PlayerCommand& command, float dt)
{
    // B1: server-side attack / break / place for a network-controlled player.
    // Driven entirely by the command (aim = yaw/pitch) and the player's own state
    // — no camera, no single-instance local-player fields. The local-player path
    // (UpdateAttackOrBreak/HandlePlaceBlock) is untouched.
    const bool localCamera = HasLocalCamera(ControlKindForPlayer(player));
    ScopedLocalFeedbackSuppression suppressRemoteFeedback(*this, !localCamera);
    if (!player.IsAlive() || matchSimulation_.HasWinner())
    {
        networkActionState_.erase(player.GetId());
        if (localCamera)
        {
            ResetBreakProgress();
        }
        return;
    }

    NetworkActionState& state = networkActionState_[player.GetId()];
    const auto resetBreakProgressForPlayer = [&]()
    {
        state.breakProgress = BreakProgress {};
        if (localCamera)
        {
            ResetBreakProgress();
        }
    };
    const Vector3 aimDirection = AimDirectionFromCommand(command);

    // --- Block placement (held), rate-limited per player. Placing precludes
    // attacking/breaking this tick, mirroring the local input split. ---
    if (ApplyNetworkBlockPlace(player, command, dt))
    {
        if (localCamera)
        {
            ResetBreakProgress();
        }
        return;
    }

    // --- Use of the SELECTED utility item (right click): one use per place
    // press, the rule the old direct SP path implemented in UseSelectedItem.
    // Riding the command keeps SP and MP identical — a real network client
    // gets right-click utility use from the same code. ---
    if (command.placePressed)
    {
        if (const std::optional<UtilityType> selectedUtility =
                ItemToUtility(GetSelectedHotbarStack(player).type))
        {
            resetBreakProgressForPlayer();
            const bool projectile = *selectedUtility == UtilityType::Fireball
                || *selectedUtility == UtilityType::Molotov;
            const UtilityActionResult result = projectile
                ? ApplyProjectileUtility(player, *selectedUtility, aimDirection)
                : ApplyUtility(player, *selectedUtility);
            PushUtilityActionResultSnapshot(player, result);
            PresentUtilityActionResult(result);
            return;
        }
    }

    // --- Ranged charge/release weapons. Network players have no server camera;
    // aim is entirely command yaw/pitch.
    const ItemType rangedItem = GetSelectedHotbarStack(player).type;
    const bool bowSelected = rangedItem == ItemType::Bow;
    const bool blasterSelected = ItemIsBlasterWeapon(rangedItem);
    if (!bowSelected && player.GetBowDrawTimer() > 0.0f)
    {
        player.ResetBowDraw();
    }
    if (!blasterSelected && player.GetBlasterState() == CrossbowState::Loading)
    {
        player.CancelBlasterLoading();
    }
    if (bowSelected)
    {
        resetBreakProgressForPlayer();
        if (command.attackHeld)
        {
            const float previousPower = BowDrawPower(player.GetBowDrawTimer());
            player.AdvanceBowDraw(dt);
            if (localCamera)
            {
                const float drawPower = BowDrawPower(player.GetBowDrawTimer());
                attackChargeActive_ = true;
                attackChargeTimer_ = player.GetBowDrawTimer();
                if (previousPower < 1.0f && drawPower >= 1.0f)
                {
                    audio_.PlayPickup();
                    AddWorldEffect(
                        player.GetPosition(),
                        aimDirection,
                        Color { 255, 226, 96, 255 },
                        0.24f,
                        0.18f,
                        WorldEffectKind::Ring);
                }
            }
            return;
        }
        if (command.attackReleased && player.GetBowDrawTimer() > 0.0f)
        {
            LaunchBowShot(player, aimDirection, BowDrawPower(player.GetBowDrawTimer()), localCamera);
        }
        player.ResetBowDraw();
        if (localCamera)
        {
            attackChargeActive_ = false;
            attackChargeTimer_ = 0.0f;
        }
        return;
    }
    if (blasterSelected)
    {
        resetBreakProgressForPlayer();
        const float fullCharge = BlasterChargeSeconds(player.GetInventory().GetBlasterRapidFireLevel());
        if (player.GetBlasterState() == CrossbowState::Loaded && command.attackPressed)
        {
            const bool aimed = rangedItem == ItemType::SniperRifle ? command.scopeHeld : command.placeHeld;
            LaunchBlasterShot(player, aimDirection, aimed, localCamera);
            if (localCamera)
            {
                blasterCharging_ = false;
                attackChargeActive_ = false;
                attackChargeTimer_ = player.GetBlasterLoadTimer();
            }
            return;
        }
        if (player.GetBlasterState() == CrossbowState::Unloaded && command.attackHeld)
        {
            player.StartBlasterLoading();
            if (localCamera)
            {
                audio_.PlayPickup();
            }
        }
        if (player.GetBlasterState() == CrossbowState::Loading && command.attackHeld)
        {
            const bool becameLoaded = player.AdvanceBlasterLoading(dt, fullCharge);
            if (localCamera)
            {
                blasterCharging_ = true;
                attackChargeActive_ = true;
                attackChargeTimer_ = player.GetBlasterLoadTimer();
                if (becameLoaded)
                {
                    audio_.PlayPickup();
                    AddWorldEffect(
                        player.GetPosition(),
                        aimDirection,
                        Color { 190, 255, 255, 255 },
                        0.30f,
                        0.22f,
                        WorldEffectKind::Ring);
                    SetMessage("Blaster charged. Next click fires.");
                }
            }
            return;
        }
        if (command.attackReleased && player.GetBlasterState() == CrossbowState::Loading)
        {
            player.CancelBlasterLoading();
            if (localCamera)
            {
                SetMessage("Blaster charge canceled.");
            }
        }
        if (localCamera)
        {
            blasterCharging_ = false;
            attackChargeActive_ = false;
            attackChargeTimer_ = player.GetBlasterLoadTimer();
        }
        return;
    }
    if (localCamera)
    {
        blasterCharging_ = false;
    }

    // --- Melee attack (pressed) with a real weapon. ---
    const std::optional<WeaponType> weapon = GetSelectedWeaponType(player);
    float meleeRayLimit = 0.0f;
    if (weapon.has_value())
    {
        const float weaponRange = CombatSystem::AttackRange(*weapon, player.GetInventory().GetSwordLevel());
        meleeRayLimit = weaponRange;
        if (const std::optional<RaycastHit> terrainHit = RaycastFromPlayerEye(player, aimDirection, weaponRange))
        {
            meleeRayLimit = std::max(0.0f, terrainHit->distance - 0.06f);
        }
    }

    if (weapon.has_value() && command.attackPressed)
    {
        std::string message;
        CombatEvent event;
        // Lag compensation: rewind enemy hitboxes to where this client saw them
        // (command.rewindTick) just for the instant-hit test. Damage/knockback
        // applied inside persist; the enemies' live positions are restored when
        // `rewind` leaves scope. meleeRayLimit above was clipped against live
        // terrain (blocks don't rewind), and the attacker is never rewound, so
        // both use authoritative positions. rewindTick==0 (bots / SP) is inert.
        bool attackLanded = false;
        {
            ScopedLagCompensation rewind(*this, player.GetId(), command.rewindTick);
            attackLanded = combat_.Attack(
                player, players_, aimDirection, message, &event, *weapon, 1.0f, nullptr, meleeRayLimit);
        }
        if (attackLanded)
        {
            RegisterCombatEvent(event, message);
            resetBreakProgressForPlayer();
            return;
        }
    }

    // --- Hero-device damage (turrets/traps/tethers) along the aim segment.
    // Server-authoritative eye origin (no camera) mirrors the local path. ---
    if (command.attackPressed)
    {
        const Vector3 origin {
            player.GetPosition().x,
            player.GetPosition().y + 0.78f,
            player.GetPosition().z
        };
        const float range = weapon.has_value() ? std::max(3.5f, meleeRayLimit) : 4.5f;
        const Vector3 end {
            origin.x + aimDirection.x * range,
            origin.y + aimDirection.y * range,
            origin.z + aimDirection.z * range
        };
        const bool toolAttack = EffectiveToolLevel(player) > 0;
        const int deviceDamage = toolAttack ? 18 + EffectiveToolLevel(player) * 9 : 18;
        if (DamageHeroDeviceAlongSegment(player.GetTeamId(), origin, end, deviceDamage, toolAttack))
        {
            player.ResetAttackCooldown(0.45f);
            resetBreakProgressForPlayer();
            return;
        }
    }

    // --- Block / enemy-core break (held). ---
    if (!command.attackHeld)
    {
        resetBreakProgressForPlayer();
        return;
    }

    // Don't mine while an enemy is lined up for melee (mirror the local path).
    if (weapon.has_value()
        && combat_.FindMeleeTarget(player, players_, aimDirection, *weapon, 1.0f, meleeRayLimit).has_value())
    {
        resetBreakProgressForPlayer();
        return;
    }

    const std::optional<RaycastHit> hit = RaycastFromPlayerEye(player, aimDirection, 4.5f);
    if (!hit.has_value())
    {
        resetBreakProgressForPlayer();
        return;
    }

    bool isCore = false;
    std::string label = DisplayName(hit->blockData.type);
    if (hit->blockData.type == BlockType::EnergyCoreBlock)
    {
        EnergyCore* core = FindCoreAt(hit->block);
        if (core == nullptr || core->GetTeamId() == player.GetTeamId())
        {
            resetBreakProgressForPlayer();
            return;
        }
        isCore = true;
        label = "Вражеский Кор";
    }
    else if (!hit->blockData.breakable || !IsBreakableByPlayers(hit->blockData.type))
    {
        resetBreakProgressForPlayer();
        return;
    }

    // The one break-time rule (tool level + Likho modifiers) shared with the
    // client prediction path — see ComputeBreakRequiredSeconds.
    const float requiredSeconds = ComputeBreakRequiredSeconds(player, *hit, isCore);

    BreakProgress& progress = state.breakProgress;
    if (!progress.visible || progress.target != hit->block || progress.isCore != isCore)
    {
        progress = BreakProgress { hit->block, hit->blockData.type, true, isCore, 0.0f, label };
    }
    progress.targetType = hit->blockData.type;
    progress.isCore = isCore;
    progress.label = label;
    progress.fraction += dt / std::max(0.001f, requiredSeconds);
    if (localCamera)
    {
        breakProgress_ = progress;
    }
    if (progress.fraction >= 1.0f)
    {
        const BlockActionResult result = ApplyCompletedBreakProgress(player, progress);
        if (result.handled && localCamera)
        {
            PresentBlockActionResult(player, result, true);
        }
        else if (result.handled && !result.message.empty())
        {
            PushBlockActionResultSnapshot(player, result, command.tick);
        }
        resetBreakProgressForPlayer();
    }
}

Game::PlayerActionResult Game::ApplyPlayerEconomyCommand(Player& player, const PlayerCommand& command)
{
    PlayerActionResult result {};
    result.playerId = player.GetId();
    result.actionSeq = command.actionSeq;
    // No discrete action requested on this command.
    if (command.actionSeq == 0
        || command.actionType == static_cast<int>(PlayerActionType::None))
    {
        return result;
    }
    result.handled = true;
    result.type = static_cast<PlayerActionType>(command.actionType);

    // Exactly-once: a resent/duplicate action (seq already seen) is ignored, so a
    // command that rides several ticks — or a UDP duplicate — never buys twice.
    // We consume the seq even when the request is ultimately denied: the request
    // was handled (with a "denied" result), and retrying the same seq can't help.
    std::uint32_t& lastSeq = economyActionSeq_[player.GetId()];
    if (command.actionSeq <= lastSeq)
    {
        result.handled = false;
        result.type = PlayerActionType::None;
        return result;
    }
    lastSeq = command.actionSeq;

    if (!player.IsAlive() || matchSimulation_.HasWinner())
    {
        result.message = "Действие отклонено.";
        result.color = Color { 255, 130, 130, 255 };
        return result;
    }

    switch (static_cast<PlayerActionType>(command.actionType))
    {
    case PlayerActionType::BuyItem:
    {
        // Server-authoritative gating: the local path can only buy while the shop
        // UI is open, which itself requires standing in the shop zone. Mirror that
        // proximity check here so a network client can't purchase from anywhere.
        Team* team = FindTeam(player.GetTeamId());
        if (team == nullptr || !shop_.IsPlayerInShop(player, *team))
        {
            result.message = "Слишком далеко от магазина.";
            result.color = Color { 255, 130, 130, 255 };
            return result;
        }
        const int repeat = std::clamp(command.actionParamB, 1, 4);
        std::string message;
        const bool bought = TryShopPurchase(player, *team, command.actionParamA, repeat, message);
        result.success = bought;
        result.message = message.empty() ? "Покупка отклонена." : message;
        result.color = bought ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 };
        return result;
    }
    case PlayerActionType::DropItem:
    {
        const int slot = command.actionParamA;
        const int amount = command.actionParamB;
        const ItemStack before = player.GetInventory().GetSlot(slot);
        {
            ScopedLocalFeedbackSuppression suppressServerFeedback(*this, true);
            result.success = TryDropInventoryStack(player, slot, amount);
        }
        result.message = result.success
            ? (std::string("Выброшено: ") + ItemDisplayName(before.type) + ".")
            : "Не удалось выбросить.";
        result.color = result.success ? Color { 255, 245, 170, 255 } : Color { 255, 130, 130, 255 };
        return result;
    }
    case PlayerActionType::MoveInventory:
    {
        const int slot = command.actionParamA;
        int packedOp = 0;
        int packedSlot = 0;
        int packedAmount = 0;
        {
            ScopedLocalFeedbackSuppression suppressServerFeedback(*this, true);
            if (DecodePackedPlayerActionParam(command.actionParamB, packedOp, packedSlot, packedAmount)
                && packedOp == static_cast<int>(InventoryMoveOp::SlotToSlot))
            {
                Inventory& inventory = player.GetInventory();
                result.success = MoveInventoryStackToSlot(inventory, slot, inventory, packedSlot, packedAmount);
            }
            else
            {
                result.success = TryQuickMoveInventorySlot(player, slot);
            }
        }
        result.message = result.success ? "Стак перемещен." : "Не удалось переместить.";
        result.color = result.success ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 };
        return result;
    }
    case PlayerActionType::ChestTransfer:
    {
        constexpr int kChestDepositSelectedSlot = 0;
        constexpr int kChestWithdrawFirstStack = 1;
        constexpr int kChestWithdrawExactSlot = 2;
        Team* team = FindTeam(player.GetTeamId());
        if (team == nullptr || player.GetTeamId() < 0 || player.GetTeamId() >= static_cast<int>(teamChests_.size()))
        {
            result.message = "Сундук недоступен.";
            result.color = Color { 255, 130, 130, 255 };
            return result;
        }

        const auto distanceSq = [](Vector3 a, Vector3 b)
        {
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        };
        const Vector3 chestPosition = world_.GridToWorld(team->teamChestBlock);
        const bool nearShop = distanceSq(player.GetPosition(), team->shopPosition) <= 12.0f;
        const bool nearChest = distanceSq(player.GetPosition(), chestPosition) <= 16.0f;
        if (!nearShop && !nearChest)
        {
            result.message = "Слишком далеко от командного сундука.";
            result.color = Color { 255, 130, 130, 255 };
            return result;
        }

        Inventory& playerInventory = player.GetInventory();
        Inventory& chest = teamChests_[player.GetTeamId()];
        const int direction = command.actionParamB;
        int packedOp = 0;
        int packedSlot = 0;
        int packedAmount = 0;
        if (DecodePackedPlayerActionParam(direction, packedOp, packedSlot, packedAmount))
        {
            const ChestTransferOp op = static_cast<ChestTransferOp>(packedOp);
            {
                ScopedLocalFeedbackSuppression suppressServerFeedback(*this, true);
                switch (op)
                {
                case ChestTransferOp::PlayerToChestSlot:
                    result.success = MoveInventoryStackToSlot(
                        playerInventory,
                        command.actionParamA,
                        chest,
                        packedSlot,
                        packedAmount);
                    result.message = result.success ? "Сложено в командный сундук." : "Не удалось сложить в сундук.";
                    break;
                case ChestTransferOp::ChestToPlayerSlot:
                    result.success = MoveInventoryStackToSlot(
                        chest,
                        command.actionParamA,
                        playerInventory,
                        packedSlot,
                        packedAmount);
                    result.message = result.success ? "Взято из командного сундука." : "Не удалось взять из сундука.";
                    break;
                case ChestTransferOp::ChestToChestSlot:
                    result.success = MoveInventoryStackToSlot(
                        chest,
                        command.actionParamA,
                        chest,
                        packedSlot,
                        packedAmount);
                    result.message = result.success ? "Стак в сундуке перемещен." : "Не удалось переместить в сундуке.";
                    break;
                default:
                    result.success = false;
                    result.message = "Неподдерживаемое действие с сундуком.";
                    break;
                }
            }
            result.color = result.success ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 };
            return result;
        }

        if (direction == kChestDepositSelectedSlot)
        {
            const int slot = command.actionParamA;
            const ItemStack selected = playerInventory.GetSlot(slot);
            if (selected.IsEmpty())
            {
                result.message = "Не удалось сложить в сундук.";
                result.color = Color { 255, 130, 130, 255 };
                return result;
            }

            if (const std::optional<ResourceType> resource = ItemToResource(selected.type))
            {
                result.success = playerInventory.SpendSlotItem(slot, selected.count);
                if (result.success)
                {
                    chest.AddResource(*resource, selected.count);
                }
            }
            else
            {
                ItemStack moving = playerInventory.TakeSlot(slot);
                const int before = moving.count;
                for (int i = 0; i < kInventorySlotCount && !moving.IsEmpty(); ++i)
                {
                    chest.PlaceStack(i, moving);
                }
                if (!moving.IsEmpty())
                {
                    playerInventory.PlaceStack(slot, moving);
                }
                result.success = moving.IsEmpty() && before > 0;
            }

            result.message = result.success ? "Сложено в командный сундук." : "Не удалось сложить в сундук.";
            result.color = result.success ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 };
            return result;
        }

        if (direction == kChestWithdrawFirstStack || direction == kChestWithdrawExactSlot)
        {
            int chestSlot = -1;
            ItemStack selected {};
            if (direction == kChestWithdrawExactSlot)
            {
                chestSlot = command.actionParamA;
                if (!chest.IsValidSlot(chestSlot))
                {
                    result.message = "Не удалось взять из сундука.";
                    result.color = Color { 255, 130, 130, 255 };
                    return result;
                }
                selected = chest.GetSlot(chestSlot);
            }
            else
            {
                for (int i = 0; i < kInventorySlotCount; ++i)
                {
                    selected = chest.GetSlot(i);
                    if (!selected.IsEmpty())
                    {
                        chestSlot = i;
                        break;
                    }
                }
            }

            if (chestSlot < 0)
            {
                result.message = "Командный сундук пуст.";
                result.color = Color { 255, 130, 130, 255 };
                return result;
            }

            if (const std::optional<ResourceType> resource = ItemToResource(selected.type))
            {
                result.success = chest.SpendSlotItem(chestSlot, selected.count);
                if (result.success)
                {
                    playerInventory.AddResource(*resource, selected.count);
                }
            }
            else
            {
                ItemStack moving = chest.TakeSlot(chestSlot);
                const int before = moving.count;
                for (int i = 0; i < kInventorySlotCount && !moving.IsEmpty(); ++i)
                {
                    playerInventory.PlaceStack(i, moving);
                }
                if (!moving.IsEmpty())
                {
                    chest.PlaceStack(chestSlot, moving);
                }
                result.success = moving.IsEmpty() && before > 0;
            }

            result.message = result.success ? "Взято из командного сундука." : "Не удалось взять из сундука.";
            result.color = result.success ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 };
            return result;
        }

        result.message = "Неподдерживаемое действие с сундуком.";
        result.color = Color { 255, 130, 130, 255 };
        return result;
    }
    case PlayerActionType::None:
    default:
        // Reserved for the next economy increment (drop / inventory move / chest
        // transfer) over this same deduped channel.
        result.message = "Неподдерживаемое действие.";
        result.color = Color { 255, 130, 130, 255 };
        return result;
    }
}

void Game::PresentPlayerActionResult(const PlayerActionResult& result)
{
    if (!result.handled || suppressLocalFeedback_)
    {
        return;
    }

    if (!result.message.empty())
    {
        SetMessage(result.message, result.seconds);
        AddEventMessage(result.message, result.color, result.seconds);
    }

    if (result.success)
    {
        if (result.type == PlayerActionType::BuyItem)
        {
            audio_.PlayPurchase();
        }
        else if (result.type == PlayerActionType::DropItem
            || result.type == PlayerActionType::MoveInventory
            || result.type == PlayerActionType::ChestTransfer)
        {
            audio_.PlayPickup();
        }
    }
    else
    {
        audio_.PlayDenied();
    }
}

void Game::PushActionResultSnapshot(ActionResultSnapshot snapshot)
{
    if (snapshot.playerId < 0)
    {
        return;
    }

    snapshot.resultSeq = ++nextActionResultSeq_;
    recentActionResults_.push_back(snapshot);
    if (recentActionResults_.size() > kMaxRecentActionResults)
    {
        recentActionResults_.erase(
            recentActionResults_.begin(),
            recentActionResults_.begin()
                + static_cast<int>(recentActionResults_.size() - kMaxRecentActionResults));
    }
}

void Game::PushWorldEventSnapshot(
    WorldEventKind kind, int actorPlayerId, int targetPlayerId, int targetTeamId,
    Vector3 position, int subjectType, int amount, int flags, const std::string& cause)
{
    WorldEventSnapshot snapshot;
    snapshot.eventSeq = ++nextWorldEventSeq_;
    snapshot.kind = static_cast<int>(kind);
    snapshot.actorPlayerId = actorPlayerId;
    snapshot.targetPlayerId = targetPlayerId;
    snapshot.targetTeamId = targetTeamId;
    snapshot.position = ToSnapshotVec3(position);
    snapshot.subjectType = subjectType;
    snapshot.amount = amount;
    snapshot.flags = flags;
    snapshot.cause = cause;
    recentWorldEvents_.push_back(snapshot);
    if (recentWorldEvents_.size() > kMaxRecentWorldEvents)
    {
        recentWorldEvents_.erase(
            recentWorldEvents_.begin(),
            recentWorldEvents_.begin()
                + static_cast<int>(recentWorldEvents_.size() - kMaxRecentWorldEvents));
    }
}

void Game::PushPlayerActionResultSnapshot(const PlayerActionResult& result)
{
    if (!result.handled || result.actionSeq == 0)
    {
        return;
    }

    PushActionResultSnapshot(ActionResultSnapshot {
        result.playerId,
        0,
        result.actionSeq,
        static_cast<int>(result.type),
        0,
        -1,
        -1,
        -1,
        0,
        0,
        result.success,
        Vec3 {},
        result.message,
        {
            static_cast<int>(result.color.r),
            static_cast<int>(result.color.g),
            static_cast<int>(result.color.b),
            static_cast<int>(result.color.a)
        },
        result.seconds });
}

void Game::PushBlockActionResultSnapshot(const Player& player, const BlockActionResult& result, std::uint32_t actionSeq)
{
    if (!result.handled)
    {
        return;
    }

    PlayerActionType type = PlayerActionType::None;
    if (result.kind == BlockActionKind::Place)
    {
        type = PlayerActionType::BlockPlace;
    }
    else if (result.kind == BlockActionKind::Break)
    {
        type = PlayerActionType::BlockBreak;
    }
    if (type == PlayerActionType::None)
    {
        return;
    }

    const Color color = result.success ? result.color : Color { 255, 130, 130, 255 };
    PushActionResultSnapshot(ActionResultSnapshot {
        player.GetId(),
        0,
        actionSeq,
        static_cast<int>(type),
        static_cast<int>(result.blockType),
        -1,
        -1,
        -1,
        0,
        0,
        result.success,
        ToSnapshotVec3(result.position),
        result.message,
        {
            static_cast<int>(color.r),
            static_cast<int>(color.g),
            static_cast<int>(color.b),
            static_cast<int>(color.a)
        },
        1.6f });
}

void Game::PushUtilityActionResultSnapshot(const Player& player, const UtilityActionResult& result)
{
    if (!result.handled)
    {
        return;
    }

    int flags = 0;
    if (result.hasWorldEffect)
    {
        flags |= kUtilityFlagWorldEffect;
    }
    if (result.playPickupSound)
    {
        flags |= kUtilityFlagPickupSound;
    }
    if (result.playBreakBlockSound)
    {
        flags |= kUtilityFlagBreakBlockSound;
    }
    if (result.playDeniedSound)
    {
        flags |= kUtilityFlagDeniedSound;
    }

    PushActionResultSnapshot(ActionResultSnapshot {
        player.GetId(),
        0,
        0,
        static_cast<int>(PlayerActionType::UtilityUse),
        static_cast<int>(result.type),
        -1,
        -1,
        -1,
        0,
        flags,
        result.success,
        ToSnapshotVec3(result.position),
        result.message,
        {
            static_cast<int>(result.color.r),
            static_cast<int>(result.color.g),
            static_cast<int>(result.color.b),
            static_cast<int>(result.color.a)
        },
        result.seconds });
}

void Game::PushHeroAbilityActionResultSnapshot(const Player& player, const HeroAbilityActionResult& result)
{
    if (!result.handled)
    {
        return;
    }

    int flags = 0;
    if (result.playPickupSound)
    {
        flags |= kHeroAbilityFlagPickupSound;
    }
    if (result.playBuildSound)
    {
        flags |= kHeroAbilityFlagBuildSound;
    }
    if (result.playPurchaseSound)
    {
        flags |= kHeroAbilityFlagPurchaseSound;
    }
    if (result.playBreakBlockSound)
    {
        flags |= kHeroAbilityFlagBreakBlockSound;
    }
    if (result.playCoreDestroyedSound)
    {
        flags |= kHeroAbilityFlagCoreDestroyedSound;
    }
    if (result.playDeniedSound)
    {
        flags |= kHeroAbilityFlagDeniedSound;
    }
    if (result.hasCameraShake)
    {
        flags |= kHeroAbilityFlagCameraShake;
    }

    // Primary world effect: prefer the first entry of the richer worldEffects
    // vector (Radon/Orbita/Brom/Konvoy/Svidetel casts, which can push several);
    // fall back to the singular hasWorldEffect fields (Likho). See the flag
    // constants above for why only one effect + no direction round-trips.
    Vector3 effectPosition = result.position;
    float effectRadius = 0.0f;
    WorldEffectKind effectKind = WorldEffectKind::Burst;
    bool directedEffect = false;
    bool hasEffect = false;
    if (!result.worldEffects.empty())
    {
        const HeroWorldEffectResult& primary = result.worldEffects.front();
        effectPosition = primary.position;
        effectRadius = primary.radius;
        effectKind = primary.kind;
        directedEffect = primary.directed;
        hasEffect = true;
    }
    else if (result.hasWorldEffect)
    {
        effectRadius = result.radius;
        effectKind = result.effectKind;
        directedEffect = result.directedWorldEffect;
        hasEffect = true;
    }
    if (hasEffect)
    {
        flags |= kHeroAbilityFlagWorldEffect;
        flags |= (static_cast<int>(effectKind) & kHeroAbilityEffectKindMask) << kHeroAbilityEffectKindShift;
        if (directedEffect)
        {
            flags |= kHeroAbilityFlagDirectedEffect;
        }
    }

    PushActionResultSnapshot(ActionResultSnapshot {
        player.GetId(),
        0,
        0,
        static_cast<int>(PlayerActionType::HeroAbility),
        static_cast<int>(result.hero),
        -1,
        -1,
        -1,
        static_cast<int>(result.slot),
        flags,
        result.success,
        ToSnapshotVec3(effectPosition),
        result.message,
        {
            static_cast<int>(result.color.r),
            static_cast<int>(result.color.g),
            static_cast<int>(result.color.b),
            static_cast<int>(result.color.a)
        },
        result.messageSeconds,
        effectRadius });
}

void Game::PushProjectileActionResultSnapshot(const Player& player, bool success, ProjectileKind kind, bool primaryFlag, Vector3 position, const std::string& message)
{
    PushActionResultSnapshot(ActionResultSnapshot {
        player.GetId(),
        0,
        0,
        static_cast<int>(PlayerActionType::ProjectileLaunch),
        static_cast<int>(kind),
        -1,
        -1,
        -1,
        0,
        primaryFlag ? kProjectileFlagPrimary : 0,
        success,
        ToSnapshotVec3(position),
        message,
        { 255, 255, 255, 255 },
        1.6f });
}

void Game::PushCombatEventSnapshots(const CombatPresentationEvent& presentation)
{
    if (!presentation.valid)
    {
        return;
    }

    const CombatEvent& event = presentation.event;
    int flags = 0;
    if (event.killed)
    {
        flags |= kCombatFlagKilled;
    }
    if (event.coreHit)
    {
        flags |= kCombatFlagCoreHit;
    }
    if (event.coreDestroyed)
    {
        flags |= kCombatFlagCoreDestroyed;
    }
    if (event.hitZone == HitZone::Head)
    {
        flags |= kCombatFlagHeadshot;
    }
    if (event.charged)
    {
        flags |= kCombatFlagCharged;
    }
    if (event.combo)
    {
        flags |= kCombatFlagCombo;
    }
    if (event.sprintReset)
    {
        flags |= kCombatFlagSprintReset;
    }
    if (event.airborneTarget)
    {
        flags |= kCombatFlagAirborneTarget;
    }
    if (event.voidHit)
    {
        flags |= kCombatFlagVoidHit;
    }
    if (presentation.voidThreat)
    {
        flags |= kCombatFlagVoidThreat;
    }

    const Color color = event.coreDestroyed
        ? Color { 255, 118, 118, 255 }
        : Color { 255, 235, 142, 255 };
    const float seconds = event.coreDestroyed ? 4.0f : 2.2f;
    const auto pushForRecipient = [&](int recipientPlayerId, int recipientFlag)
    {
        if (recipientPlayerId < 0)
        {
            return;
        }
        PushActionResultSnapshot(ActionResultSnapshot {
            recipientPlayerId,
            0,
            0,
            static_cast<int>(PlayerActionType::CombatEvent),
            static_cast<int>(event.weapon),
            event.attackerId,
            event.targetId,
            event.targetTeamId,
            event.damage,
            flags | recipientFlag,
            true,
            ToSnapshotVec3(event.position),
            presentation.message,
            {
                static_cast<int>(color.r),
                static_cast<int>(color.g),
                static_cast<int>(color.b),
                static_cast<int>(color.a)
            },
            seconds });
    };

    pushForRecipient(event.attackerId, kCombatFlagRecipientAttacker);
    if (!event.coreHit && event.targetId != event.attackerId)
    {
        pushForRecipient(event.targetId, kCombatFlagRecipientTarget);
    }
}

void Game::QueuePlayerAction(PlayerActionType type, int paramA, int paramB)
{
    pendingEconomyActionType_ = type;
    pendingEconomyActionParamA_ = paramA;
    pendingEconomyActionParamB_ = paramB;
    ++clientEconomyActionSeq_;
}

void Game::QueueEconomyAction(PlayerActionType type, int paramA, int paramB)
{
    QueuePlayerAction(type, paramA, paramB);
}

void Game::ApplyPendingLocalPlayerAction(Player& player)
{
    if (networkMode_ != NetworkMode::LocalSinglePlayer
        || pendingEconomyActionType_ == PlayerActionType::None)
    {
        return;
    }

    PlayerCommand command = BuildLocalPlayerCommand();
    // Out-of-band submit for the queued economy action only: strip the per-tick
    // action intents (ability casts, utility uses, combat/place edges) so they
    // apply exactly once, on the per-tick command IntegratedServerTick ships
    // for the same input.
    command.attackPressed = false;
    command.attackReleased = false;
    command.placePressed = false;
    command.useAbility1 = false;
    command.useAbility2 = false;
    command.useUltimate = false;
    command.useHeal = false;
    command.useTeleport = false;
    command.useDash = false;
    command.useShoot = false;
    command.useFireball = false;
    command.useMolotov = false;
    command.useAlarm = false;
    if (integratedServerActive_
        && integratedServer_.SubmitCommand(kIntegratedServerClientId, command))
    {
        // Phase 6: the same command -> server -> result pipeline multiplayer
        // uses, over the loopback wire. Drained synchronously so the purchase /
        // deny feedback lands on the same frame as the click (parity with the
        // old direct call). dt = 0: this out-of-band send must not advance
        // time-based state (the place rate limiter) — only the per-tick
        // IntegratedServerTick does.
        for (const PlayerCommand& received : integratedServer_.DrainCommands())
        {
            ++integratedServerCommandsDrained_;
            ApplyIntegratedServerCommand(received, 0.0f);
        }
    }
    else
    {
        const PlayerActionResult result = ApplyPlayerEconomyCommand(player, command);
        PresentPlayerActionResult(result);
    }
    if (command.actionSeq != 0)
    {
        pendingEconomyActionType_ = PlayerActionType::None;
    }
}

void Game::StartIntegratedServer()
{
    StopIntegratedServer();
    if (networkMode_ != NetworkMode::LocalSinglePlayer)
    {
        return;
    }
    const Player* local = GetLocalPlayer();
    if (local == nullptr)
    {
        return;
    }
    integratedServer_.Configure(serverConfig_);
    integratedServer_.Start();
    if (!integratedServer_.Connect(kIntegratedServerClientId, local->GetId()))
    {
        integratedServer_.Stop();
        return;
    }
    integratedServerActive_ = true;
}

void Game::StopIntegratedServer()
{
    if (integratedServer_.IsRunning())
    {
        integratedServer_.Disconnect(kIntegratedServerClientId);
        integratedServer_.Stop();
    }
    integratedServerActive_ = false;
    integratedServerCommandsDrained_ = 0;
    integratedServerEconomyApplied_ = 0;
    integratedServerBlocksPlaced_ = 0;
    integratedServerHeroCastsApplied_ = 0;
}

void Game::IntegratedServerTick(float dt)
{
    if (!integratedServerActive_)
    {
        return;
    }
    // Client side: ship this tick's command over the loopback wire. Movement is
    // still applied by the direct SP path (hybrid step) — the server side below
    // only consumes the migrated systems, so nothing double-applies.
    integratedServer_.SubmitCommand(kIntegratedServerClientId, BuildLocalPlayerCommand());
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, dt);
    }
    // Server -> client: the same per-recipient visibility-filtered snapshot a
    // remote client would receive. Presentation does not consume it yet (hybrid);
    // it keeps the channel honest and observable for parity tests.
    integratedServer_.PublishSnapshot(
        kIntegratedServerClientId, BuildNetworkSnapshotForClient(localPlayerId_));
}

void Game::ApplyIntegratedServerCommand(const PlayerCommand& command, float dt)
{
    Player* target = matchSimulation_.GetPlayer(static_cast<int>(command.controlledPlayerId));
    if (target == nullptr)
    {
        return;
    }
    // Migrated system (Phase 6 slice): economy/inventory actions run through the
    // same validated + deduped server method as multiplayer. The result is
    // presented directly instead of via an ActionResultSnapshot round-trip — the
    // integrated client shares the server's presentation until snapshots drive it.
    const PlayerActionResult result = ApplyPlayerEconomyCommand(*target, command);
    if (result.handled)
    {
        ++integratedServerEconomyApplied_;
        PresentPlayerActionResult(result);
    }

    // Migrated system (Phase 6 hero-ability slice): casts run through the same
    // authoritative action entry as network clients and bots. The direct cast in
    // UseHeroAbilityInputs is gated off while the integrated server is active,
    // so a press fires exactly once (cooldown/charge consumed once).
    if (target->IsAlive()
        && (command.useAbility1 || command.useAbility2 || command.useUltimate))
    {
        if (ApplyPlayerActionCommand(*target, command))
        {
            ++integratedServerHeroCastsApplied_;
        }
    }

    // Migrated system (Phase 6 utility slice): heal/teleport/dash/fireball/
    // molotov/alarm run through the same command-driven server path as
    // multiplayer (UseUtilityInputs pushes the owner-private result and presents
    // directly for the local-camera player). The direct call in UpdateLocalPlayer
    // is gated off while the integrated server is active, so a utility item is
    // consumed exactly once per press. Alive/winner gate mirrors the direct
    // path (UpdateLocalPlayer only runs for a living player before a winner).
    if (target->IsAlive() && !matchSimulation_.HasWinner())
    {
        UseUtilityInputs(*target, command);
    }

    // Combat/build slice: place, use-selected-utility, break, melee, and
    // ranged weapons run through the same authoritative command path as
    // multiplayer (ApplyNetworkPlayerActions). The old direct SP combat/place
    // code has been REMOVED (UpdateAttackOrBreak is a HUD-reset stub,
    // HandlePlaceBlock is denial-feedback only) — this is the only path.
    if (target->IsAlive() && !matchSimulation_.HasWinner())
    {
        const int blocksPlacedBefore = stats_.blocksPlaced;
        ApplyNetworkPlayerActions(*target, command, dt);
        if (stats_.blocksPlaced > blocksPlacedBefore)
        {
            integratedServerBlocksPlaced_ +=
                static_cast<std::uint32_t>(stats_.blocksPlaced - blocksPlacedBefore);
        }
    }
}

void Game::ApplyBatchedServerCommands(const std::vector<ReceivedCommand>& receivedCommands, float dt)
{
    // Apply at most one movement step per player per server tick. UDP can batch
    // several client commands between polls; using dt for each one makes remote
    // players jump ahead in grid-like bursts. Keep the freshest continuous
    // input, but preserve edge-style one-shots across the batch.
    struct BatchedPlayerCommand
    {
        int playerId = -1;
        int clientId = -1;
        PlayerCommand latest {};
        bool hasCommand = false;
        bool sprintTapped = false;
        bool attackPressed = false;
        bool attackReleased = false;
        bool placePressed = false;
        bool interact = false;
        bool useAbility1 = false;
        bool useAbility2 = false;
        bool useUltimate = false;
        bool useHeal = false;
        bool useTeleport = false;
        bool useDash = false;
        bool useShoot = false;
        bool useFireball = false;
        bool useMolotov = false;
        bool useAlarm = false;
        std::uint32_t actionSeq = 0;
        int actionType = static_cast<int>(PlayerActionType::None);
        int actionParamA = 0;
        int actionParamB = 0;
    };

    std::vector<BatchedPlayerCommand> commandBatches;
    for (const ReceivedCommand& received : receivedCommands)
    {
        const int playerId = static_cast<int>(received.command.controlledPlayerId);
        auto batchIt = std::find_if(commandBatches.begin(), commandBatches.end(),
            [playerId](const BatchedPlayerCommand& batch)
            {
                return batch.playerId == playerId;
            });
        if (batchIt == commandBatches.end())
        {
            BatchedPlayerCommand batch;
            batch.playerId = playerId;
            commandBatches.push_back(batch);
            batchIt = commandBatches.end();
            --batchIt;
        }

        BatchedPlayerCommand& batch = *batchIt;
        const PlayerCommand& command = received.command;
        if (!batch.hasCommand || command.tick >= batch.latest.tick)
        {
            batch.latest = command;
            batch.clientId = received.clientId;
            batch.hasCommand = true;
        }

        batch.sprintTapped = batch.sprintTapped || command.sprintTapped;
        batch.attackPressed = batch.attackPressed || command.attackPressed;
        batch.attackReleased = batch.attackReleased || command.attackReleased;
        batch.placePressed = batch.placePressed || command.placePressed;
        batch.interact = batch.interact || command.interact;
        batch.useAbility1 = batch.useAbility1 || command.useAbility1;
        batch.useAbility2 = batch.useAbility2 || command.useAbility2;
        batch.useUltimate = batch.useUltimate || command.useUltimate;
        batch.useHeal = batch.useHeal || command.useHeal;
        batch.useTeleport = batch.useTeleport || command.useTeleport;
        batch.useDash = batch.useDash || command.useDash;
        batch.useShoot = batch.useShoot || command.useShoot;
        batch.useFireball = batch.useFireball || command.useFireball;
        batch.useMolotov = batch.useMolotov || command.useMolotov;
        batch.useAlarm = batch.useAlarm || command.useAlarm;
        if (command.actionSeq != 0 && command.actionSeq >= batch.actionSeq)
        {
            batch.actionSeq = command.actionSeq;
            batch.actionType = command.actionType;
            batch.actionParamA = command.actionParamA;
            batch.actionParamB = command.actionParamB;
        }
    }

    const auto clearOneShotInputs = [](PlayerCommand& command)
    {
        command.sprintTapped = false;
        command.attackPressed = false;
        command.attackReleased = false;
        command.placePressed = false;
        command.interact = false;
        command.useAbility1 = false;
        command.useAbility2 = false;
        command.useUltimate = false;
        command.useHeal = false;
        command.useTeleport = false;
        command.useDash = false;
        command.useShoot = false;
        command.useFireball = false;
        command.useMolotov = false;
        command.useAlarm = false;
        command.actionSeq = 0;
        command.actionType = static_cast<int>(PlayerActionType::None);
        command.actionParamA = 0;
        command.actionParamB = 0;
    };

    std::vector<int> playerIdsToApply = networkControlledPlayerIds_;
    const auto addPlayerToApply = [&playerIdsToApply](int playerId)
    {
        if (playerId < 0)
        {
            return;
        }
        if (std::find(playerIdsToApply.begin(), playerIdsToApply.end(), playerId) == playerIdsToApply.end())
        {
            playerIdsToApply.push_back(playerId);
        }
    };

    for (const BatchedPlayerCommand& batch : commandBatches)
    {
        if (!batch.hasCommand)
        {
            continue;
        }
        PlayerCommand held = batch.latest;
        clearOneShotInputs(held);
        serverHeldPlayerCommands_[batch.playerId] = held;
        addPlayerToApply(batch.playerId);
    }

    for (int playerId : playerIdsToApply)
    {
        auto heldIt = serverHeldPlayerCommands_.find(playerId);
        if (heldIt == serverHeldPlayerCommands_.end())
        {
            continue;
        }

        const BatchedPlayerCommand* freshBatch = nullptr;
        for (const BatchedPlayerCommand& batch : commandBatches)
        {
            if (batch.playerId == playerId && batch.hasCommand)
            {
                freshBatch = &batch;
                break;
            }
        }

        const bool isFreshCommand = freshBatch != nullptr;
        PlayerCommand command = heldIt->second;
        if (isFreshCommand)
        {
            command = freshBatch->latest;
            command.sprintTapped = freshBatch->sprintTapped;
            command.attackPressed = freshBatch->attackPressed;
            command.attackReleased = freshBatch->attackReleased;
            command.placePressed = freshBatch->placePressed;
            command.interact = freshBatch->interact;
            command.useAbility1 = freshBatch->useAbility1;
            command.useAbility2 = freshBatch->useAbility2;
            command.useUltimate = freshBatch->useUltimate;
            command.useHeal = freshBatch->useHeal;
            command.useTeleport = freshBatch->useTeleport;
            command.useDash = freshBatch->useDash;
            command.useShoot = freshBatch->useShoot;
            command.useFireball = freshBatch->useFireball;
            command.useMolotov = freshBatch->useMolotov;
            command.useAlarm = freshBatch->useAlarm;
            command.actionSeq = freshBatch->actionSeq;
            command.actionType = freshBatch->actionType;
            command.actionParamA = freshBatch->actionParamA;
            command.actionParamB = freshBatch->actionParamB;
        }
        else
        {
            // No fresh command this tick: re-apply the last continuous input for
            // movement continuity, but do NOT advance the acknowledged command
            // tick. Bumping it past the client's real, applied commands made the
            // client over-trim its prediction history and under-replay on
            // reconciliation, so it never restored the full prediction lead — a
            // persistent ~half-block deficit that stayed hidden while moving
            // (ongoing forward prediction re-hid it) but snapped backward the
            // instant the player stopped. The held command keeps its tick; only a
            // real client command advances the ack below.
            clearOneShotInputs(command);
        }
        command.controlledPlayerId = static_cast<std::uint32_t>(playerId);

        Player* target = matchSimulation_.GetPlayer(static_cast<int>(command.controlledPlayerId));
        if (target != nullptr && target->IsAlive())
        {
            ScopedLocalFeedbackSuppression suppressServerFeedback(*this, true);
            ApplyPlayerCommand(*target, command, dt);
            ApplyStandingBlockEffects(*target, HasLocalCamera(ControlKindForPlayer(*target)));
            ApplyPlayerActionCommand(*target, command);
            UseUtilityInputs(*target, command);
            ApplyNetworkPlayerActions(*target, command, dt);
            const PlayerActionResult result = ApplyPlayerEconomyCommand(*target, command);
            if (result.handled && result.actionSeq != 0)
            {
                PushPlayerActionResultSnapshot(result);
            }
            // Only a genuine client command advances the tick the server
            // acknowledges to that client; a synthesized held repeat must not.
            if (isFreshCommand)
            {
                serverEffectiveCommandTickByPlayer_[playerId] =
                    std::max(serverEffectiveCommandTickByPlayer_[playerId], command.tick);
            }
        }
    }
}

void Game::NetworkServerTick(ServerTransport& transport, float dt)
{
    transport.Poll();

    if (!networkLobbyMatchStarted_)
    {
        // Drain lobby disconnect notifications; before the match starts there
        // are no assigned match slots to free yet.
        (void)transport.TakeDisconnectedClients();

        if (networkLobbyMatchStarting_)
        {
            networkLobbyMatchStartingTimer_ += dt;
            if (networkLobbyMatchStartingTimer_ >= kMatchStartingBarrierSeconds)
            {
                SetupNetworkMatchFromLobby(transport, networkLobbyStartingRoster_);
                networkLobbyStartingRoster_.clear();
                networkLobbyMatchStarting_ = false;
                transport.SetMatchJoinLocked(true);
                transport.BroadcastLobbySnapshot(
                    transport.BuildLobbySnapshot(true, true, "match started"));
            }
            else
            {
                transport.BroadcastLobbySnapshot(
                    transport.BuildLobbySnapshot(true, false, "match starting"));
            }
            return;
        }

        const LobbyValidation validation = transport.ValidateLobbyStart();
        if (validation.canStart && transport.LobbyStartRequested())
        {
            networkLobbyStartingRoster_ = transport.LobbyPlayers();
            networkLobbyMatchStarting_ = true;
            networkLobbyMatchStartingTimer_ = 0.0f;
            transport.BroadcastLobbySnapshot(
                transport.BuildLobbySnapshot(true, false, "match starting"));
        }
        else
        {
            transport.BroadcastLobbySnapshot(
                transport.BuildLobbySnapshot(false, false, validation.reason));
        }
        return;
    }

    // Preserve disconnected human slots. They stay network-controlled so bot AI
    // cannot play for them; reconnect applies a respawn penalty below.
    for (const DisconnectedClient& gone : transport.TakeDisconnectedClients())
    {
        serverHeldPlayerCommands_.erase(gone.playerId);
        serverEffectiveCommandTickByPlayer_.erase(gone.playerId);
        if (const Player* player = matchSimulation_.GetPlayer(gone.playerId))
        {
            std::cout << "server: " << player->GetName()
                      << " disconnected; playerId=" << gone.playerId
                      << " slot reserved for reconnect\n";
            AddEventMessage(player->GetName() + " отключился; ждем переподключения",
                            Color { 255, 200, 120, 255 }, 3.0f);
        }
        else
        {
            std::cout << "server: client " << gone.clientId << " disconnected before assignment\n";
        }
    }
    for (const ReconnectedClient& returned : transport.TakeReconnectedClients())
    {
        MarkNetworkControlledPlayer(returned.playerId);
        if (Player* player = matchSimulation_.GetPlayer(returned.playerId))
        {
            std::cout << "server: " << player->GetName()
                      << " reconnected; playerId=" << returned.playerId
                      << " full baseline queued\n";
            player->KillWithRespawn(kReconnectRespawnSeconds);
            AddEventMessage(player->GetName() + " переподключился; респаун через 7 с",
                            Color { 128, 238, 166, 255 }, 3.0f);
        }
    }

    if (networkLobbyMatchStartedBroadcastsRemaining_ > 0)
    {
        transport.BroadcastLobbySnapshot(
            transport.BuildLobbySnapshot(false, true, "match started"));
        --networkLobbyMatchStartedBroadcastsRemaining_;
    }

    ApplyBatchedServerCommands(transport.DrainCommands(), dt);

    // Advance the authoritative world (bots for unclaimed players, physics,
    // combat, deaths) by one fixed step. Server simulation may generate
    // presentation events (projectile trails, pickups, passives); in multiplayer
    // those belong to clients via snapshots/events, not to the host UI/audio.
    {
        ScopedLocalFeedbackSuppression suppressServerFeedback(*this, true);
        UpdateMatchSimulation(dt);
    }

    // Record this tick's post-simulation player positions for lag compensation.
    // Keyed by the tick just advanced to (== the snapshot tick clients will see
    // below), so a later command's rewindTick maps to exactly the positions the
    // client was displaying. Recorded only here (real server), never in SP.
    RecordLagCompFrame();

    // Send each client its own visibility-filtered snapshot.
    for (int clientId : transport.ConnectedClients())
    {
        const int playerId = transport.PlayerForClient(clientId);
        const auto effectiveTick = serverEffectiveCommandTickByPlayer_.find(playerId);
        if (effectiveTick != serverEffectiveCommandTickByPlayer_.end())
        {
            transport.AdvanceProcessedCommandTick(clientId, effectiveTick->second);
        }
        if (transport.NeedsSnapshotForClient(clientId))
        {
            transport.SendSnapshotToClient(clientId, BuildNetworkSnapshotForClient(playerId));
        }
    }
}

int Game::RunNetworkServer(const ServerConfig& config, double maxSeconds)
{
    if (!NetworkTransportAvailable())
    {
        std::cerr << "server: network transport disabled at build (DAIBED_ENABLE_NETWORK=OFF); "
                     "cannot listen.\n";
        return 8;
    }
    ServerTransport transport;
    if (!NetworkServerSetup(transport, config))
    {
        std::cerr << "server: failed to bind " << config.listenAddress << ':' << config.port
                  << " — " << transport.LastError() << '\n';
        return 8;
    }

    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    std::cout << "server: lobby listening on " << config.listenAddress << ':' << transport.BoundPort()
              << " (headless, no window) password=" << (config.HasPassword() ? "set" : "none")
              << " private=" << (config.privateServer ? "yes" : "no")
              << " maxTeamSize=" << config.maxTeamSize
              << " uniqueHeroes=" << (config.enforceUniqueHeroesPerTeam ? "yes" : "no")
              << " tickRate=" << matchSimulation_.TickRate() << '\n';
    const std::string joinHost = config.listenAddress == "0.0.0.0"
        ? std::string("<host-ip>")
        : config.listenAddress;
    std::cout << "server: join command: DaiBed.exe --connect " << joinHost
              << ':' << transport.BoundPort()
              << (config.HasPassword() ? " --password <password>" : "")
              << '\n';
    std::cout << "SERVER_READY" << std::endl;

    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    Clock::time_point lastStatus = start;
    std::uint32_t ticks = 0;
    while (!ShouldClose())
    {
        CrashLogger::Heartbeat(networkLobbyMatchStarted_ ? "network-server-match" : "network-server-lobby");
        const Clock::time_point tickStart = Clock::now();
        NetworkServerTick(transport, fixedDt);
        ++ticks;

        if (std::chrono::duration<double>(tickStart - lastStatus).count() >= 2.0)
        {
            lastStatus = tickStart;
            if (!networkLobbyMatchStarted_)
            {
                const LobbySnapshot lobby = transport.BuildLobbySnapshot(
                    networkLobbyMatchStarting_, false,
                    networkLobbyMatchStarting_ ? "match starting" : "");
                std::cout << "server: lobby clients=" << transport.ClientCount()
                          << '/' << serverConfig_.maxPlayers
                          << " canStart=" << (lobby.canStart ? "yes" : "no")
                          << " status=\"" << lobby.statusMessage << "\""
                          << " roster=" << FormatLobbyRosterForHost(lobby)
                          << '\n';
            }
            else
            {
                std::cout << "server: clients=" << transport.ClientCount()
                          << " tick=" << matchSimulation_.CurrentTick()
                          << " rx=" << transport.PacketsReceived() << " tx=" << transport.PacketsSent()
                          << " bytes[rx/tx]=" << transport.BytesReceived() << '/' << transport.BytesSent()
                          << " snapshots[full/delta]=" << transport.FullSnapshotsSent()
                          << '/' << transport.DeltaSnapshotsSent()
                          << " lastSize[full/delta]=" << transport.LastFullSnapshotBytes()
                          << '/' << transport.LastDeltaSnapshotBytes()
                          << " resync=" << transport.ResyncRequestsReceived()
                          << '\n';
            }
        }

        if (maxSeconds > 0.0 && std::chrono::duration<double>(Clock::now() - start).count() >= maxSeconds)
        {
            break;
        }

        // Pace the loop to the tick rate so the server runs in real time. Use a
        // high-resolution wait: a plain sleep_for is bounded by the OS timer
        // granularity (~15.6 ms on Windows), which overshoots a 16.67 ms tick to
        // ~30 ms and drops the server to ~33 Hz. A client predicts at a steady
        // 60 Hz, so against a half-speed server it constantly out-runs the
        // authoritative sim and rubber-bands (jerky movement, dash stutter).
        const double elapsed = std::chrono::duration<double>(Clock::now() - tickStart).count();
        const double remaining = static_cast<double>(fixedDt) - elapsed;
        if (remaining > 0.0)
        {
            Platform::PreciseSleepSeconds(remaining);
        }
    }

    transport.Close();
    std::cout << "server: stopped after " << ticks << " ticks.\n";
    std::cout << "SERVER_STOPPED" << std::endl;
    return 0;
}

int Game::RunMultiplayerLoopbackSmoke()
{
    using Clock = std::chrono::steady_clock;

    if (!NetworkTransportAvailable())
    {
        std::cout << "mp-smoke: network transport disabled at build (DAIBED_ENABLE_NETWORK=OFF) — skipped\n";
        std::cout << "MP_LOOPBACK_SMOKE_SKIPPED" << std::endl;
        return 0;
    }

    ServerTransport server;
    ServerConfig cfg;
    cfg.listenAddress = "127.0.0.1";
    cfg.port = 0; // ephemeral
    cfg.password = "secret";
    if (!NetworkServerSetup(server, cfg))
    {
        std::cout << "mp-smoke: server setup failed: " << server.LastError() << '\n';
        std::cout << "MP_LOOPBACK_SMOKE_FAIL" << std::endl;
        return 9;
    }
    const std::uint16_t port = server.BoundPort();
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    std::cout << "mp-smoke: server listening on 127.0.0.1:" << port
              << " (headless, no window) password=set\n";

    ClientTransport clientA;
    ClientTransport clientB;
    ClientTransport clientBad;
    clientA.Open("127.0.0.1", port, "secret", 3.0f);
    clientB.Open("127.0.0.1", port, "secret", 3.0f);
    clientBad.Open("127.0.0.1", port, "wrong", 3.0f); // wrong password -> denied

    const auto posInSnapshot = [](const MatchSnapshot& snap, int playerId, Vec3& out) -> bool
    {
        for (const PlayerSnapshot& entry : snap.players)
        {
            if (entry.playerId == playerId)
            {
                out = entry.position;
                return true;
            }
        }
        return false;
    };
    const auto sendMove = [this](ClientTransport& client)
    {
        if (!client.InMatch())
        {
            return;
        }
        const int pid = client.AssignedPlayerId();
        const Player* p = matchSimulation_.GetPlayer(pid);
        PlayerCommand cmd;
        cmd.controlledPlayerId = static_cast<std::uint32_t>(pid);
        cmd.tick = matchSimulation_.CurrentTick();
        cmd.aimYaw = p != nullptr ? p->GetYaw() : 0.0f; // move forward along own facing
        cmd.moveForward = 1.0f;
        client.SendCommand(cmd);
    };

    // Phase 1: connect both, exchange commands/snapshots, watch A move (as B sees it).
    Vec3 bSeesAFirst {};
    bool haveFirst = false;
    float aMovementSeenByB = 0.0f;
    const Clock::time_point deadline1 = Clock::now() + std::chrono::seconds(3);
    int ticksAfterFirst = 0;
    bool sentLobbyA = false;
    bool sentLobbyB = false;
    bool duplicateBlocked = false;
    bool duplicateFixed = false;
    while (Clock::now() < deadline1)
    {
        NetworkServerTick(server, fixedDt);
        clientA.Poll();
        clientB.Poll();
        clientBad.Poll();

        if (clientA.IsConnected() && !sentLobbyA)
        {
            LobbyUpdate update;
            update.playerName = "Alice";
            update.selectedTeam = 0;
            update.selectedHero = 0;
            update.ready = true;
            update.startRequested = true;
            clientA.SendLobbyUpdate(update);
            sentLobbyA = true;
        }
        if (clientB.IsConnected() && !sentLobbyB)
        {
            LobbyUpdate update;
            update.playerName = "Bob";
            update.selectedTeam = 0;
            update.selectedHero = 0; // duplicate with Alice: start must block first.
            update.ready = true;
            update.startRequested = true;
            clientB.SendLobbyUpdate(update);
            sentLobbyB = true;
        }
        if (!duplicateFixed && clientA.HasLobbySnapshot())
        {
            const LobbySnapshot& lobby = clientA.LatestLobbySnapshot();
            if (!lobby.canStart && lobby.statusMessage == "duplicate hero in team")
            {
                duplicateBlocked = true;
                LobbyUpdate update;
                update.playerName = "Bob";
                update.selectedTeam = 0;
                update.selectedHero = 1;
                update.ready = true;
                update.startRequested = true;
                clientB.SendLobbyUpdate(update);
                duplicateFixed = true;
            }
        }

        sendMove(clientA);
        sendMove(clientB);

        if (clientB.HasSnapshot() && clientA.InMatch())
        {
            Vec3 p;
            if (posInSnapshot(clientB.LatestSnapshot(), clientA.AssignedPlayerId(), p))
            {
                if (!haveFirst)
                {
                    bSeesAFirst = p;
                    haveFirst = true;
                }
                else
                {
                    aMovementSeenByB = std::max(aMovementSeenByB, (p - bSeesAFirst).Length());
                }
            }
        }
        if (haveFirst)
        {
            if (++ticksAfterFirst > 120)
            {
                break; // ~enough movement observed
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const int playerA = clientA.AssignedPlayerId();
    const int playerB = clientB.AssignedPlayerId();
    Vec3 tmp {};
    const bool aSeesA = posInSnapshot(clientA.LatestSnapshot(), playerA, tmp);
    const bool aSeesB = posInSnapshot(clientA.LatestSnapshot(), playerB, tmp);
    const bool bSeesA = posInSnapshot(clientB.LatestSnapshot(), playerA, tmp);
    const bool bSeesB = posInSnapshot(clientB.LatestSnapshot(), playerB, tmp);

    const bool bothConnected = clientA.IsConnected() && clientB.IsConnected()
        && clientA.InMatch() && clientB.InMatch();
    const bool differentPlayers = playerA >= 0 && playerB >= 0 && playerA != playerB;
    const bool badDenied = clientBad.WasDenied();
    const bool lobbySnapshots = clientA.HasLobbySnapshot() && clientB.HasLobbySnapshot();
    const bool lobbyStartedBoth = clientA.LatestLobbySnapshot().matchStarted
        && clientB.LatestLobbySnapshot().matchStarted;
    const bool bothSeeBoth = aSeesA && aSeesB && bSeesA && bSeesB;
    const bool movementVisible = aMovementSeenByB > 0.5f;

    // Phase 2: A disconnects. The server must keep running and keep serving B.
    const std::uint32_t bTickBefore = clientB.LatestSnapshot().tick;
    const std::uint32_t clientAFullBeforeDisconnect = clientA.FullSnapshotsReceived();
    const std::uint32_t clientADeltaBeforeDisconnect = clientA.DeltaSnapshotsReceived();
    clientA.Disconnect();
    bool serverSurvived = true;
    const Clock::time_point deadline2 = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline2)
    {
        NetworkServerTick(server, fixedDt); // must not crash after a disconnect
        clientB.Poll();
        sendMove(clientB);
        if (server.ClientCount() <= 1 && clientB.LatestSnapshot().tick > bTickBefore + 60)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool aClientGone = server.ClientCount() == 1; // only B remains connected
    const bool noAiTakeover = IsNetworkControlledPlayer(playerA);
    const bool bStillConnected = clientB.IsConnected();
    const bool bStillReceiving = clientB.LatestSnapshot().tick > bTickBefore;

    // Phase 3: Alice reconnects by name into the reserved slot; an unrelated
    // late join is denied. Then send an old command tick and ensure the server
    // drops it instead of replaying stale input.
    ClientTransport clientAReconnect;
    ClientTransport clientLate;
    clientAReconnect.Open("127.0.0.1", port, "secret", 3.0f);
    clientLate.Open("127.0.0.1", port, "secret", 3.0f);
    bool sentReconnectLobby = false;
    bool sentLateLobby = false;
    bool reconnectSlot = false;
    bool lateDenied = false;
    const Clock::time_point deadline3 = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline3)
    {
        NetworkServerTick(server, fixedDt);
        clientB.Poll();
        clientAReconnect.Poll();
        clientLate.Poll();

        if (clientAReconnect.IsConnected() && !sentReconnectLobby)
        {
            LobbyUpdate update;
            update.playerName = "Alice";
            update.selectedTeam = 0;
            update.selectedHero = 0;
            update.ready = true;
            clientAReconnect.SendLobbyUpdate(update);
            sentReconnectLobby = true;
        }
        if (clientLate.IsConnected() && !sentLateLobby)
        {
            LobbyUpdate update;
            update.playerName = "Eve";
            update.selectedTeam = 1;
            update.selectedHero = 2;
            update.ready = true;
            clientLate.SendLobbyUpdate(update);
            sentLateLobby = true;
        }

        sendMove(clientB);
        sendMove(clientAReconnect);

        reconnectSlot = clientAReconnect.InMatch()
            && clientAReconnect.AssignedPlayerId() == playerA;
        lateDenied = clientLate.WasDenied();
        if (reconnectSlot && lateDenied && clientB.HasSnapshot()
            && clientAReconnect.HasSnapshot())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const Player* reconnectedServerPlayer = matchSimulation_.GetPlayer(playerA);
    const bool reconnectRespawning = reconnectedServerPlayer != nullptr
        && !reconnectedServerPlayer->IsAlive()
        && !reconnectedServerPlayer->IsEliminated()
        && reconnectedServerPlayer->GetRespawnTimer() > 6.0f
        && reconnectedServerPlayer->GetRespawnTimer() <= kReconnectRespawnSeconds;

    const std::uint32_t staleBefore = server.StaleCommandsDropped();
    PlayerCommand stale;
    stale.controlledPlayerId = static_cast<std::uint32_t>(playerB);
    stale.tick = clientB.LastAckedCommandTick() > 0 ? clientB.LastAckedCommandTick() : 1;
    stale.moveForward = 1.0f;
    clientB.SendCommand(stale);
    const Clock::time_point staleDeadline = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < staleDeadline && server.StaleCommandsDropped() == staleBefore)
    {
        NetworkServerTick(server, fixedDt);
        clientB.Poll();
        clientAReconnect.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const Clock::time_point reconnectDeltaDeadline = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < reconnectDeltaDeadline && clientAReconnect.DeltaSnapshotsReceived() == 0)
    {
        NetworkServerTick(server, fixedDt);
        clientB.Poll();
        clientAReconnect.Poll();
        sendMove(clientB);
        sendMove(clientAReconnect);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool staleDropped = server.StaleCommandsDropped() > staleBefore;
    const std::uint32_t staleDropCount = server.StaleCommandsDropped();
    const std::string lateDenyReason = clientLate.DenyReason();
    const std::uint32_t ackB = clientB.LastAckedCommandTick();
    const bool deltaSnapshotsActive = clientAFullBeforeDisconnect > 0
        && clientB.FullSnapshotsReceived() > 0
        && clientADeltaBeforeDisconnect > 0
        && clientB.DeltaSnapshotsReceived() > 0;
    const bool deltaSmallerThanFull =
        clientB.LastFullSnapshotBytes() > 0
        && clientB.LastDeltaSnapshotBytes() > 0
        && clientB.LastDeltaSnapshotBytes() < clientB.LastFullSnapshotBytes();
    const bool reconnectBaselineThenDeltas =
        clientAReconnect.FullSnapshotsReceived() > 0
        && clientAReconnect.DeltaSnapshotsReceived() > 0;
    const std::uint32_t clientBDropped = clientB.DroppedSnapshots();
    const std::uint32_t clientBIgnored = clientB.IgnoredSnapshots();
    const std::uint32_t serverResyncs = server.ResyncRequestsReceived();
    const std::uint32_t clientAFull = clientAFullBeforeDisconnect;
    const std::uint32_t clientADelta = clientADeltaBeforeDisconnect;
    const std::uint32_t clientBFull = clientB.FullSnapshotsReceived();
    const std::uint32_t clientBDelta = clientB.DeltaSnapshotsReceived();
    const std::uint32_t reconnectFull = clientAReconnect.FullSnapshotsReceived();
    const std::uint32_t reconnectDelta = clientAReconnect.DeltaSnapshotsReceived();
    const std::size_t clientBFullBytes = clientB.LastFullSnapshotBytes();
    const std::size_t clientBDeltaBytes = clientB.LastDeltaSnapshotBytes();

    // Capture final teardown state BEFORE closing (diagnostics use captured
    // booleans above, not live post-teardown values).
    clientAReconnect.Disconnect();
    clientLate.Disconnect();
    clientB.Disconnect();
    server.Close();

    std::cout << "mp-smoke: lobbySnapshots=" << (lobbySnapshots ? "yes" : "no")
              << " duplicateHeroBlocked=" << (duplicateBlocked ? "yes" : "no")
              << " lobbyStartedBoth=" << (lobbyStartedBoth ? "yes" : "no")
              << '\n';
    std::cout << "mp-smoke: bothConnected=" << (bothConnected ? "yes" : "no")
              << " playerA=" << playerA << " playerB=" << playerB
              << " (different=" << (differentPlayers ? "yes" : "no") << ')'
              << " badPassword=" << (badDenied ? ("denied(" + clientBad.DenyReason() + ")") : std::string("ACCEPTED"))
              << '\n';
    std::cout << "mp-smoke: bothSeeBoth=" << (bothSeeBoth ? "yes" : "no")
              << " aMovedSeenByB=" << aMovementSeenByB
              << " afterDisconnect[aClientGone=" << (aClientGone ? "yes" : "no")
              << " noAiTakeover=" << (noAiTakeover ? "yes" : "no")
              << " bConnected=" << (bStillConnected ? "yes" : "no")
              << " bReceiving=" << (bStillReceiving ? "yes" : "no")
              << " serverAlive=" << (serverSurvived ? "yes" : "no") << "]\n";
    std::cout << "mp-smoke: reconnectSlot=" << (reconnectSlot ? "yes" : "no")
              << " reconnectRespawning=" << (reconnectRespawning ? "yes" : "no")
              << " lateJoin=" << (lateDenied ? ("denied(" + lateDenyReason + ")") : std::string("ACCEPTED"))
              << " staleDropped=" << (staleDropped ? "yes" : "no")
              << " staleDrops=" << staleDropCount
              << " ackB=" << ackB << '\n';
    std::cout << "mp-smoke: snapshots full/delta A="
              << clientAFull << '/' << clientADelta
              << " B=" << clientBFull << '/' << clientBDelta
              << " reconnect=" << reconnectFull << '/' << reconnectDelta
              << " sizeB[full/delta]=" << clientBFullBytes << '/' << clientBDeltaBytes
              << " deltaSmaller=" << (deltaSmallerThanFull ? "yes" : "no")
              << " drop/ignoreB=" << clientBDropped << '/' << clientBIgnored
              << " resyncServer=" << serverResyncs << '\n';

    const bool ok = lobbySnapshots && duplicateBlocked && lobbyStartedBoth
        && bothConnected && differentPlayers && badDenied && bothSeeBoth
        && movementVisible && serverSurvived && aClientGone && noAiTakeover
        && bStillConnected && bStillReceiving
        && reconnectSlot && reconnectRespawning && lateDenied && staleDropped
        && deltaSnapshotsActive && deltaSmallerThanFull && reconnectBaselineThenDeltas;

    if (!ok)
    {
        std::cout << "mp-smoke: checks lobbySnapshots=" << (lobbySnapshots ? "ok" : "FAIL")
                  << " duplicateBlocked=" << (duplicateBlocked ? "ok" : "FAIL")
                  << " lobbyStartedBoth=" << (lobbyStartedBoth ? "ok" : "FAIL")
                  << " bothConnected=" << (bothConnected ? "ok" : "FAIL")
                  << " differentPlayers=" << (differentPlayers ? "ok" : "FAIL")
                  << " badDenied=" << (badDenied ? "ok" : "FAIL")
                  << " bothSeeBoth=" << (bothSeeBoth ? "ok" : "FAIL")
                  << " movementVisible=" << (movementVisible ? "ok" : "FAIL")
                  << " aClientGone=" << (aClientGone ? "ok" : "FAIL")
                  << " noAiTakeover=" << (noAiTakeover ? "ok" : "FAIL")
                  << " bStillConnected=" << (bStillConnected ? "ok" : "FAIL")
                  << " bStillReceiving=" << (bStillReceiving ? "ok" : "FAIL")
                  << " reconnectSlot=" << (reconnectSlot ? "ok" : "FAIL")
                  << " reconnectRespawning=" << (reconnectRespawning ? "ok" : "FAIL")
                  << " lateDenied=" << (lateDenied ? "ok" : "FAIL")
                  << " staleDropped=" << (staleDropped ? "ok" : "FAIL")
                  << " deltaActive=" << (deltaSnapshotsActive ? "ok" : "FAIL")
                  << " deltaSmaller=" << (deltaSmallerThanFull ? "ok" : "FAIL")
                  << " reconnectBaselineDelta=" << (reconnectBaselineThenDeltas ? "ok" : "FAIL")
                  << '\n';
    }

    std::cout << (ok ? "MP_LOOPBACK_SMOKE_OK" : "MP_LOOPBACK_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

// ============================================================================
// Phase 0.1T — GUI network client
//
// A windowed spectator client: it connects over the real transport, rebuilds the
// arena locally from the server's advertised lobby config (the map is
// deterministic from biome/layout/mode), folds each authoritative snapshot into
// world_/players_/cores, and draws the live match with the normal renderer. No
// input/prediction/interpolation yet (Phase 0.1U); the assigned player is shown
// but not driven, so the surrounding AI players supply the visible motion.
// ============================================================================

void Game::BuildClientWorld(const LobbySnapshot& lobby)
{
    // Adopt the server's advertised map config so SetupMatch reproduces the same
    // deterministic arena. Clamp into the valid enum ranges defensively.
    arenaBiome_ = static_cast<ArenaBiome>(std::clamp(lobby.worldBiome, 0, 4));
    arenaLayout_ = static_cast<ArenaLayout>(std::clamp(lobby.worldLayout, 0, 1));
    selectedMode_ = static_cast<MatchMode>(std::clamp(lobby.matchMode, 0, 3));

    // The client renders an authoritative replica — it runs no local sim, bots or
    // input. Build the static world (terrain/cores/generators) via SetupMatch, then
    // discard the roster it created: the player set is driven entirely by snapshots.
    pendingNetworkRoster_.clear();
    SetupMatch();
    matchSimulation_.Players().clear();
    networkControlledPlayerIds_.clear();
    localPlayerServerDriven_ = true;

    // Reset client send/prediction pacing and snapshot-fold tracking for the match.
    clientInputAccumulator_ = 0.0f;
    networkCommandTick_ = 0;
    pendingClientInput_ = PlayerInput {};
    hasFoldedSnapshot_ = false;
    lastFoldedSnapshotTick_ = 0;
    clientFeelSnapshot_ = MatchSnapshot {};
    hasClientFeelSnapshot_ = false;
    remoteSnapshotBuffer_.clear();
    networkRemoteRenderTime_ = 0.0f;
    hasNetworkRemoteRenderTime_ = false;
    predictionSmoothingOffset_ = Vector3 { 0.0f, 0.0f, 0.0f };
    screen_ = GameScreen::Playing;
    spectatorMode_ = false;
    inventoryOpen_ = false;
    shopOpen_ = false;
    CloseChest();
    heldInventoryStack_ = ItemStack {};
    heldInventoryOrigin_ = HeldInventoryOrigin::None;
    heldInventoryOriginSlot_ = -1;

    // Enter the match in first person, like SetupMatch (Reset() selects
    // ViewMode::FirstPerson). The roster was just cleared, so there's no player
    // to anchor to yet; the exact yaw is re-synced to the assigned player on the
    // first snapshot (clientAimInitialized_ in SampleClientInput) and the
    // position is followed every frame by UpdateCamera. Without this the
    // camera would keep whatever mode the menu left it in (third person).
    cameraController_.Reset(cameraController_.GetYaw(), -0.14f, Vector3 { 0.0f, 0.0f, 0.0f });
}

// Locomotion poses are derivable from velocity/ground state alone; event poses
// (attack/cast/ability/hurt/death/...) are gameplay-driven and only the server
// knows them. The client derives the former locally for its predicted player and
// adopts the latter from the snapshot. Keep in sync with Player::UpdateLocomotionAnimation.
static bool IsLocomotionAnimation(HeroAnimationState state)
{
    switch (state)
    {
    case HeroAnimationState::Idle:
    case HeroAnimationState::Walk:
    case HeroAnimationState::Run:
    case HeroAnimationState::Jump:
    case HeroAnimationState::Fall:
        return true;
    default:
        return false;
    }
}

void Game::ApplyClientSnapshotFeedback(const MatchSnapshot& snapshot)
{
    if (headless_ || networkMode_ != NetworkMode::LocalClient)
    {
        return;
    }

    for (const ActionResultSnapshot& replicated : snapshot.actionResults)
    {
        if (replicated.playerId != networkAssignedPlayerId_ || replicated.resultSeq == 0)
        {
            continue;
        }

        std::uint32_t& lastPresented = presentedActionResultSeq_[replicated.playerId];
        if (replicated.resultSeq <= lastPresented)
        {
            continue;
        }
        lastPresented = replicated.resultSeq;

        const PlayerActionType replicatedType = static_cast<PlayerActionType>(replicated.actionType);
        if (replicatedType == PlayerActionType::CombatEvent)
        {
            CombatPresentationEvent presentation;
            presentation.valid = true;
            presentation.message = replicated.message;
            presentation.voidThreat = (replicated.flags & kCombatFlagVoidThreat) != 0;
            presentation.event.attackerId = replicated.actorPlayerId;
            presentation.event.targetId = replicated.targetPlayerId;
            presentation.event.targetTeamId = replicated.targetTeamId;
            presentation.event.position = ToRaylibVector3(replicated.position);
            presentation.event.damage = replicated.amount;
            presentation.event.killed = (replicated.flags & kCombatFlagKilled) != 0;
            presentation.event.coreHit = (replicated.flags & kCombatFlagCoreHit) != 0;
            presentation.event.coreDestroyed = (replicated.flags & kCombatFlagCoreDestroyed) != 0;
            presentation.event.hitZone = (replicated.flags & kCombatFlagHeadshot) != 0
                ? HitZone::Head
                : HitZone::Body;
            presentation.event.weapon = static_cast<WeaponType>(replicated.subjectType);
            presentation.event.charged = (replicated.flags & kCombatFlagCharged) != 0;
            presentation.event.combo = (replicated.flags & kCombatFlagCombo) != 0;
            presentation.event.sprintReset = (replicated.flags & kCombatFlagSprintReset) != 0;
            presentation.event.airborneTarget = (replicated.flags & kCombatFlagAirborneTarget) != 0;
            presentation.event.voidHit = (replicated.flags & kCombatFlagVoidHit) != 0;
            PresentCombatEvent(presentation);
            continue;
        }
        if (replicatedType == PlayerActionType::BlockPlace
            || replicatedType == PlayerActionType::BlockBreak)
        {
            Player* player = matchSimulation_.GetPlayer(replicated.playerId);
            if (player == nullptr)
            {
                continue;
            }

            BlockActionResult result;
            result.handled = true;
            result.success = replicated.success;
            result.kind = replicatedType == PlayerActionType::BlockPlace
                ? BlockActionKind::Place
                : BlockActionKind::Break;
            result.blockType = static_cast<BlockType>(replicated.subjectType);
            result.position = ToRaylibVector3(replicated.position);
            result.color = Color {
                static_cast<unsigned char>(std::clamp(replicated.color[0], 0, 255)),
                static_cast<unsigned char>(std::clamp(replicated.color[1], 0, 255)),
                static_cast<unsigned char>(std::clamp(replicated.color[2], 0, 255)),
                static_cast<unsigned char>(std::clamp(replicated.color[3], 0, 255))
            };
            result.message = replicated.message;
            result.hasWorldEffect = replicated.success;
            result.playPlaceSound = replicated.success && result.kind == BlockActionKind::Place;
            result.playBreakSound = replicated.success && result.kind == BlockActionKind::Break;
            result.playDeniedSound = !replicated.success;
            result.incrementLocalPlaced = replicated.success && result.kind == BlockActionKind::Place;
            result.incrementLocalBroken = replicated.success && result.kind == BlockActionKind::Break;
            result.tntActivated = replicated.success
                && result.kind == BlockActionKind::Place
                && result.blockType == BlockType::ExplosiveBlock;
            PresentBlockActionResult(*player, result, true);
            if (!result.success && result.kind == BlockActionKind::Place)
            {
                audio_.PlayDenied();
            }
            continue;
        }
        if (replicatedType == PlayerActionType::UtilityUse)
        {
            UtilityActionResult result;
            result.handled = true;
            result.success = replicated.success;
            result.type = static_cast<UtilityType>(replicated.subjectType);
            result.message = replicated.message;
            result.position = ToRaylibVector3(replicated.position);
            result.color = Color {
                static_cast<unsigned char>(std::clamp(replicated.color[0], 0, 255)),
                static_cast<unsigned char>(std::clamp(replicated.color[1], 0, 255)),
                static_cast<unsigned char>(std::clamp(replicated.color[2], 0, 255)),
                static_cast<unsigned char>(std::clamp(replicated.color[3], 0, 255))
            };
            result.seconds = replicated.seconds;
            // No radius field on the wire; a fixed cosmetic default matches how
            // PresentBlockActionResult already hardcodes its own effect radius
            // client-side instead of replicating one.
            result.radius = 0.32f;
            result.hasWorldEffect = (replicated.flags & kUtilityFlagWorldEffect) != 0;
            result.playPickupSound = (replicated.flags & kUtilityFlagPickupSound) != 0;
            result.playBreakBlockSound = (replicated.flags & kUtilityFlagBreakBlockSound) != 0;
            result.playDeniedSound = (replicated.flags & kUtilityFlagDeniedSound) != 0;
            PresentUtilityActionResult(result);
            continue;
        }
        if (replicatedType == PlayerActionType::HeroAbility)
        {
            HeroAbilityActionResult result;
            result.handled = true;
            result.success = replicated.success;
            result.hero = static_cast<HeroId>(replicated.subjectType);
            result.slot = static_cast<HeroAbilitySlot>(replicated.amount);
            result.message = replicated.message;
            result.messageSeconds = replicated.seconds;
            result.position = ToRaylibVector3(replicated.position);
            result.color = Color {
                static_cast<unsigned char>(std::clamp(replicated.color[0], 0, 255)),
                static_cast<unsigned char>(std::clamp(replicated.color[1], 0, 255)),
                static_cast<unsigned char>(std::clamp(replicated.color[2], 0, 255)),
                static_cast<unsigned char>(std::clamp(replicated.color[3], 0, 255))
            };
            result.playPickupSound = (replicated.flags & kHeroAbilityFlagPickupSound) != 0;
            result.playBuildSound = (replicated.flags & kHeroAbilityFlagBuildSound) != 0;
            result.playPurchaseSound = (replicated.flags & kHeroAbilityFlagPurchaseSound) != 0;
            result.playBreakBlockSound = (replicated.flags & kHeroAbilityFlagBreakBlockSound) != 0;
            result.playCoreDestroyedSound = (replicated.flags & kHeroAbilityFlagCoreDestroyedSound) != 0;
            result.playDeniedSound = (replicated.flags & kHeroAbilityFlagDeniedSound) != 0;
            // Primary world effect (position already set above from the
            // replicated snapshot's position field; radius is the one new wire
            // field this slice added). Direction/duration are NOT replicated —
            // direction is approximated from the caster's own current facing
            // (fair for the near-always-forward-facing directed effects),
            // duration from a fixed cosmetic constant (same trick
            // PresentBlockActionResult/PresentUtilityActionResult already use
            // for their own hardcoded radius/seconds). Floating texts and event
            // messages still are NOT replicated (would need new string fields).
            result.hasWorldEffect = (replicated.flags & kHeroAbilityFlagWorldEffect) != 0;
            result.radius = replicated.radius;
            result.seconds = 0.4f;
            result.effectKind = static_cast<WorldEffectKind>(
                (replicated.flags >> kHeroAbilityEffectKindShift) & kHeroAbilityEffectKindMask);
            result.directedWorldEffect = (replicated.flags & kHeroAbilityFlagDirectedEffect) != 0;
            if (result.directedWorldEffect)
            {
                if (const Player* actor = matchSimulation_.GetPlayer(replicated.playerId))
                {
                    result.direction = actor->Forward();
                }
            }
            result.hasCameraShake = (replicated.flags & kHeroAbilityFlagCameraShake) != 0;
            result.cameraShakeStrength = 0.18f;
            result.cameraShakeSeconds = 0.2f;
            PresentHeroAbilityResult(result);
            continue;
        }
        if (replicatedType == PlayerActionType::ProjectileLaunch)
        {
            if (!replicated.message.empty())
            {
                SetMessage(replicated.message);
            }
            if (replicated.success)
            {
                audio_.PlayBreakBlock();
            }
            else
            {
                audio_.PlayDenied();
            }
            continue;
        }

        PlayerActionResult result;
        result.handled = true;
        result.success = replicated.success;
        result.playerId = replicated.playerId;
        result.actionSeq = replicated.actionSeq;
        result.type = static_cast<PlayerActionType>(replicated.actionType);
        result.message = replicated.message;
        result.color = Color {
            static_cast<unsigned char>(std::clamp(replicated.color[0], 0, 255)),
            static_cast<unsigned char>(std::clamp(replicated.color[1], 0, 255)),
            static_cast<unsigned char>(std::clamp(replicated.color[2], 0, 255)),
            static_cast<unsigned char>(std::clamp(replicated.color[3], 0, 255))
        };
        result.seconds = replicated.seconds;
        PresentPlayerActionResult(result);
    }

    // Public broadcast events: death/respawn/generator-boost/alarm/pickup
    // feedback HandleDeathsAndRespawns and friends produce on the host but a
    // network client never sees at all (it never runs UpdateMatchSimulation).
    // Victory and per-player damage/kill-feed text are NOT handled here — they
    // are already synthesized below from the player/core snapshot diff (see
    // `previous`), so re-adding them here would just double the message.
    for (const WorldEventSnapshot& event : snapshot.worldEvents)
    {
        if (event.eventSeq == 0 || event.eventSeq <= presentedWorldEventSeq_)
        {
            continue;
        }
        presentedWorldEventSeq_ = event.eventSeq;

        const bool isOwnTarget = event.targetPlayerId == networkAssignedPlayerId_;
        const bool isOwnActor = event.actorPlayerId == networkAssignedPlayerId_;
        switch (static_cast<WorldEventKind>(event.kind))
        {
        case WorldEventKind::PlayerDied:
        {
            audio_.PlayDeath();
            if (isOwnTarget)
            {
                const Player* killer = matchSimulation_.GetPlayer(event.actorPlayerId);
                const bool finalDeath = (event.flags & 1) != 0;
                localDeathKiller_ = killer != nullptr ? killer->GetName() : "Окружение";
                localDeathCause_ = event.cause;
                localDeathOverlayTimer_ = finalDeath ? 7.0f : 4.0f;
                ++stats_.deaths;
                damageFlashTimer_ = 0.9f;
                AddCameraShake(0.28f, 0.25f);
                audio_.PlayDenied();
                if (finalDeath)
                {
                    EnterSpectatorMode();
                }
            }
            break;
        }
        case WorldEventKind::PlayerRespawnLost:
        {
            if (const Player* player = matchSimulation_.GetPlayer(event.targetPlayerId))
            {
                SetMessage(player->GetName() + " потерял защиту респауна. Финальная смерть.");
            }
            if (isOwnTarget)
            {
                EnterSpectatorMode();
            }
            break;
        }
        case WorldEventKind::PlayerRespawned:
        {
            const Player* player = matchSimulation_.GetPlayer(event.targetPlayerId);
            const Team* team = FindTeam(event.targetTeamId);
            const EnergyCore* core = FindCoreByTeam(event.targetTeamId);
            const bool coreAlive = core == nullptr || core->IsAlive();
            SetMessage((player != nullptr ? player->GetName() : "Игрок")
                + " возродился. " + (coreAlive ? "Кор работает." : "Кор уничтожен. Последняя жизнь!"));
            AddWorldEffect(ToRaylibVector3(event.position),
                team != nullptr ? GetTeamColor(team->color) : WHITE, 0.42f, 0.45f);
            break;
        }
        case WorldEventKind::GeneratorBoost:
            AddEventMessage("10:00 Скорость генераторов увеличена", Color { 255, 235, 142, 255 }, 5.0f);
            AddKillFeed("Генераторы ускорены", Color { 255, 235, 142, 255 }, 6.0f);
            audio_.PlayPurchase();
            break;
        case WorldEventKind::AlarmTriggered:
        {
            const Team* owner = FindTeam(event.targetTeamId);
            AddEventMessage((owner != nullptr ? owner->name : "База") + std::string(": сработала тревога!"), Color { 255, 235, 142, 255 }, 3.0f);
            const Vector3 position = ToRaylibVector3(event.position);
            AddFloatingText("ТРЕВОГА", position, Color { 255, 235, 142, 255 });
            AddWorldEffect(position, Color { 255, 235, 142, 255 }, 0.42f, 0.45f);
            audio_.PlayDenied();
            break;
        }
        case WorldEventKind::ResourcePickup:
        {
            // Own pickups are already handled by the inventory-diff feedback
            // below (ownPrevious/ownCurrent); only bystanders need this path.
            if (isOwnActor)
            {
                break;
            }
            const Vector3 position = ToRaylibVector3(event.position);
            AddWorldEffect(position, Color { 180, 210, 255, 255 }, 0.22f, 0.28f);
            AddFloatingText(
                "+" + std::to_string(event.amount) + " " + ToString(static_cast<ResourceType>(event.subjectType)),
                position, Fade(WHITE, 0.85f));
            break;
        }
        case WorldEventKind::ItemPickup:
        {
            const Vector3 position = ToRaylibVector3(event.position);
            AddWorldEffect(position, Color { 255, 245, 170, 255 }, 0.18f, 0.22f);
            if (isOwnActor)
            {
                SetMessage(std::string("Подобрано: ") + ItemDisplayName(static_cast<ItemType>(event.subjectType)) + ".");
                audio_.PlayPickup();
            }
            break;
        }
        }
    }

    if (!hasClientFeelSnapshot_)
    {
        return;
    }

    const MatchSnapshot& previous = clientFeelSnapshot_;
    const PlayerSnapshot* ownPrevious = FindPlayerSnapshot(previous, networkAssignedPlayerId_);
    const PlayerSnapshot* ownCurrent = FindPlayerSnapshot(snapshot, networkAssignedPlayerId_);

    for (const PlayerSnapshot& current : snapshot.players)
    {
        const PlayerSnapshot* old = FindPlayerSnapshot(previous, current.playerId);
        if (old == nullptr)
        {
            continue;
        }

        const int damage = old->health - current.health;
        if (damage > 0)
        {
            Vector3 position = SnapshotVecToRay(current.position);
            position.y += 1.15f;
            const bool ownDamage = current.playerId == networkAssignedPlayerId_;
            const bool enemyDamage = ownCurrent != nullptr && current.teamId != ownCurrent->teamId;
            const Color color = ownDamage
                ? Color { 255, 118, 118, 255 }
                : (enemyDamage ? Color { 255, 236, 135, 255 } : Color { 180, 210, 255, 255 });
            AddWorldEffect(position, color, ownDamage ? 0.42f : 0.30f, ownDamage ? 0.42f : 0.30f);
            AddFloatingText("-" + std::to_string(damage), position, color);
            if (ownDamage)
            {
                damageFlashTimer_ = std::max(damageFlashTimer_, 0.75f);
                AddCameraShake(0.20f + std::min(0.12f, static_cast<float>(damage) * 0.006f), 0.18f);
                audio_.PlayHit();
            }
            else
            {
                audio_.PlayHitAt(position);
                if (enemyDamage)
                {
                    hitMarkerTimer_ = std::max(hitMarkerTimer_, 0.12f);
                }
            }
        }

        if (old->alive && !current.alive)
        {
            const std::string name = current.playerName.empty()
                ? "Игрок " + std::to_string(current.playerId)
                : current.playerName;
            const bool finalDeath = current.eliminated;
            AddEventMessage(name + (finalDeath ? " выбыл" : " повержен"),
                finalDeath ? RED : ORANGE, 2.2f);
            AddKillFeed(name + (finalDeath ? " выбыл" : " повержен"),
                finalDeath ? Color { 255, 118, 118, 255 } : Color { 255, 200, 120, 255 },
                finalDeath ? 6.0f : 4.0f);
        }
    }

    for (const CoreSnapshot& current : snapshot.cores)
    {
        const CoreSnapshot* old = FindCoreSnapshot(previous, current.teamId);
        if (old == nullptr)
        {
            continue;
        }

        const int damage = old->health - current.health;
        if (damage <= 0)
        {
            continue;
        }

        Vector3 position {};
        if (EnergyCore* core = FindCoreByTeam(current.teamId))
        {
            position = world_.GridToWorld(core->GetBlockPosition());
        }
        else if (const Team* team = FindTeam(current.teamId))
        {
            position = team->spawnPoint;
        }
        position.y += 0.8f;

        const bool destroyed = old->alive && !current.alive;
        AddWorldEffect(position,
            destroyed ? Color { 255, 118, 118, 255 } : Color { 112, 232, 255, 255 },
            destroyed ? 0.75f : 0.48f,
            destroyed ? 0.80f : 0.42f);
        AddFloatingText("-" + std::to_string(damage), position, Color { 112, 232, 255, 255 });
        AddKillFeed(std::string(TeamName(current.teamId)) + " Кор -" + std::to_string(damage),
            destroyed ? Color { 255, 118, 118, 255 } : Color { 112, 232, 255, 255 },
            destroyed ? 7.0f : 3.8f);
        if (destroyed)
        {
            AddEventMessage(std::string(TeamName(current.teamId)) + " Кор уничтожен",
                Color { 255, 118, 118, 255 }, 5.0f);
            AddCameraShake(0.30f, 0.32f);
            audio_.PlayCoreDestroyed();
        }
        else
        {
            AddCameraShake(0.12f, 0.14f);
            audio_.PlayBreakBlockAt(position);
        }
    }

    if (ownPrevious != nullptr && ownCurrent != nullptr
        && ownPrevious->inventory.present && ownCurrent->inventory.present)
    {
        bool playedPickup = false;
        const ResourceType resourceOrder[3] = {
            ResourceType::Iron, ResourceType::Gold, ResourceType::Crystal };
        const bool worldItemRemoved = snapshot.pickups.size() < previous.pickups.size()
            || snapshot.droppedItems.size() < previous.droppedItems.size();
        if (worldItemRemoved)
        {
            Vector3 position = SnapshotVecToRay(ownCurrent->position);
            position.y += 1.0f;
            for (int i = 0; i < 3; ++i)
            {
                const int gained = ownCurrent->inventory.resources[i] - ownPrevious->inventory.resources[i];
                if (gained <= 0)
                {
                    continue;
                }
                AddFloatingText("+" + std::to_string(gained) + " " + ToString(resourceOrder[i]),
                    position, Color { 255, 236, 135, 255 });
                stats_.resourcesPicked += 1;
                playedPickup = true;
            }
            if (playedPickup)
            {
                SetMessage("Ресурсы подобраны.", 1.15f);
                AddWorldEffect(position, Color { 255, 245, 170, 255 }, 0.28f, 0.28f);
                audio_.PlayPickup();
            }
        }
    }

    const float snapshotDt = std::max(0.0f, snapshot.matchTime - previous.matchTime);
    int dynamicFxBudget = 5;
    for (const ProjectileSnapshot& projectile : snapshot.projectiles)
    {
        if (dynamicFxBudget <= 0 || HasMatchingProjectileSnapshot(previous, projectile, snapshotDt))
        {
            continue;
        }
        const Vector3 position = SnapshotVecToRay(projectile.position);
        const Vector3 direction = SnapshotVecToRay(projectile.velocity);
        const ProjectileKind kind = ProjectileKindFromSnapshot(projectile.kind);
        const Color color = kind == ProjectileKind::Fireball ? Color { 255, 178, 96, 255 }
            : (kind == ProjectileKind::Molotov ? Color { 255, 118, 70, 255 }
            : (kind == ProjectileKind::Blaster ? Color { 98, 245, 255, 255 }
            : Color { 210, 220, 235, 255 }));
        AddWorldEffect(position, direction, color, 0.34f, 0.26f, WorldEffectKind::Burst);
        audio_.PlayBreakBlockAt(position);
        --dynamicFxBudget;
    }

    for (const ExplosiveSnapshot& explosive : snapshot.explosives)
    {
        if (dynamicFxBudget <= 0 || HasMatchingExplosiveSnapshot(previous, explosive))
        {
            continue;
        }
        const Vector3 position = SnapshotVecToRay(explosive.position);
        AddWorldEffect(position, Color { 255, 210, 120, 255 }, 0.42f, 0.35f);
        audio_.PlayBuildAt(position);
        --dynamicFxBudget;
    }

    for (const HazardZoneSnapshot& hazard : snapshot.hazardZones)
    {
        if (dynamicFxBudget <= 0 || HasMatchingHazardSnapshot(previous, hazard))
        {
            continue;
        }
        const Vector3 position = SnapshotVecToRay(hazard.position);
        AddWorldEffect(position, Color { 255, 118, 70, 255 }, hazard.radius, 0.45f);
        audio_.PlayBreakBlockAt(position);
        --dynamicFxBudget;
    }

    for (const HeroDeviceSnapshot& device : snapshot.heroDevices)
    {
        if (dynamicFxBudget <= 0 || HasMatchingDeviceSnapshot(previous, device))
        {
            continue;
        }
        const Vector3 position = SnapshotVecToRay(device.position);
        AddWorldEffect(position, Color { 128, 238, 166, 255 }, 0.34f, 0.32f);
        audio_.PlayBuildAt(position);
        --dynamicFxBudget;
    }

    if (previous.winnerTeamId < 0 && snapshot.winnerTeamId >= 0)
    {
        const Team* winner = FindTeam(snapshot.winnerTeamId);
        const std::string winnerName = winner != nullptr ? winner->name : "Команда";
        SetMessage(winnerName + " побеждает!", 8.0f);
        AddEventMessage(winnerName + " побеждает!", Color { 255, 235, 142, 255 }, 7.0f);
        audio_.PlayVictory();
    }
}

void Game::ApplyClientSnapshot(const MatchSnapshot& snapshot)
{
    ApplyClientSnapshotFeedback(snapshot);

    playerScores_.clear();
    playerScores_.reserve(snapshot.matchScores.size());
    for (const PlayerScoreSnapshot& score : snapshot.matchScores)
    {
        PlayerMatchScore localScore;
        localScore.playerId = score.playerId;
        localScore.kills = score.kills;
        localScore.deaths = score.deaths;
        localScore.finalDeaths = score.finalDeaths;
        localScore.coreDamage = score.coreDamage;
        localScore.coresDestroyed = score.coresDestroyed;
        playerScores_.push_back(localScore);
    }

    std::vector<Player>& players = matchSimulation_.Players();

    // Drop any player no longer present in the snapshot (left / eliminated-removed).
    players.erase(
        std::remove_if(players.begin(), players.end(),
            [&snapshot](const Player& existing)
            {
                return std::none_of(snapshot.players.begin(), snapshot.players.end(),
                    [&existing](const PlayerSnapshot& s) { return s.playerId == existing.GetId(); });
            }),
        players.end());

    // Create/update a renderable player per snapshot entry. Identity (team/hero)
    // comes from the authoritative snapshot; spatial/health state is overwritten.
    for (const PlayerSnapshot& s : snapshot.players)
    {
        const HeroId heroId = HeroSystem::IdFromIndex(std::clamp(s.heroId, 0, HeroSystem::kHeroCount - 1));
        Player* player = matchSimulation_.GetPlayer(s.playerId);
        if (player == nullptr)
        {
            const Team* team = FindTeam(s.teamId);
            const Vector3 spawn = team != nullptr ? team->spawnPoint : Vector3 {};
            const std::string playerName = s.playerName.empty()
                ? "Игрок " + std::to_string(s.playerId)
                : s.playerName;
            Player created(s.playerId, playerName, s.teamId, spawn,
                           s.playerId == networkAssignedPlayerId_);
            created.SetHeroId(heroId);
            players.push_back(created);
            player = matchSimulation_.GetPlayer(s.playerId);
            if (player == nullptr)
            {
                continue;
            }
        }
        else if (player->GetHeroId() != heroId)
        {
            // The filter can surface a different identity (e.g. a disguised Likho
            // seen by an enemy); reflect it so the right model renders.
            player->SetHeroId(heroId);
        }

        if (s.playerId == networkAssignedPlayerId_)
        {
            // Our own player: spatial state is owned by client-side prediction
            // (StepClientPredictionAndSend applies our command at the fixed tick rate) and
            // corrected by reconciliation (ApplyAuthoritativeSnapshotForPrediction,
            // run just before this). Overwriting it from the laggy snapshot every
            // frame is exactly what froze movement between packets ("3 FPS").
            // Only adopt the authoritative position when prediction can't own it
            // (dead/respawning) or has drifted too far to reconcile smoothly.
            const float drift = (player->GetPositionVec3() - s.position).Length();
            if (!s.alive || drift > kClientHardResyncDistance)
            {
                player->SetPosition(s.position);
                player->SetVelocity(s.velocity);
                player->SetYaw(s.yaw);
                // A hard resync (respawn / teleport / big desync) is meant to
                // snap; drop any smoothing so the view doesn't trail a ghost.
                predictionSmoothingOffset_ = Vector3 { 0.0f, 0.0f, 0.0f };
            }
            else if (s.health < player->GetHealth()
                && DistanceVec3(player->GetVelocityVec3(), s.velocity) > kReplicatedVelocityCorrectionThreshold
                && LengthVec3(s.velocity) > kReplicatedVelocityImpulseThreshold)
            {
                player->AdoptReplicatedKnockbackVelocity(s.velocity);
            }
        }
        else
        {
            // Remote players: render from the interpolation buffer (delayed ~100ms)
            // so they glide between snapshots instead of teleporting per packet.
            Vec3 replicatedPosition = s.position;
            float replicatedYaw = s.yaw;
            Vec3 smoothedPosition {};
            if (TryGetInterpolatedRemotePlayerPosition(
                    s.playerId, networkInterpolationDelaySeconds_, smoothedPosition))
            {
                replicatedPosition = smoothedPosition;
            }
            float smoothedYaw = 0.0f;
            if (TryGetInterpolatedRemotePlayerYaw(
                    s.playerId, networkInterpolationDelaySeconds_, smoothedYaw))
            {
                replicatedYaw = smoothedYaw;
            }
            player->SetPosition(replicatedPosition);
            player->SetVelocity(s.velocity);
            player->SetYaw(replicatedYaw);
        }
        player->ApplyReplicatedState(s.health, s.maxHealth, s.alive, s.eliminated, s.respawnTimer);

        // Adopt the authoritative animation pose so this player animates on the
        // client — walk/run/jump/attack/cast/death — instead of freezing on Idle.
        // The OWN (predicted) player adopts only server-driven EVENT poses
        // (attack/cast/death/...); its locomotion is derived locally each tick from
        // the predicted velocity (AdvanceAnimation) so it tracks input at zero lag
        // instead of lagging the snapshot by ~RTT. Remote players adopt everything.
        const HeroAnimationState snapAnim = static_cast<HeroAnimationState>(s.animationState);
        const bool ownPlayer = s.playerId == networkAssignedPlayerId_;
        if (!ownPlayer || !IsLocomotionAnimation(snapAnim))
        {
            HeroRuntimeState& clientHeroState = player->MutableHeroState();
            clientHeroState.animationState = snapAnim;
            clientHeroState.animationTimer = s.animationTimer;
            clientHeroState.animationDuration = s.animationDuration;
        }

        // Owner-private inventory survives the visibility filter only for ourselves;
        // mirror it onto the followed player so the HUD shows real resources/hotbar.
        if (s.playerId == networkAssignedPlayerId_ && s.inventory.present)
        {
            Inventory& inventory = player->GetInventory();
            const ResourceType resourceOrder[3] = {
                ResourceType::Iron, ResourceType::Gold, ResourceType::Crystal };
            for (int i = 0; i < 3; ++i)
            {
                const int current = inventory.GetResource(resourceOrder[i]);
                const int target = s.inventory.resources[i];
                if (target > current)
                {
                    inventory.AddResource(resourceOrder[i], target - current);
                }
                else if (target < current)
                {
                    inventory.SpendResource(resourceOrder[i], current - target);
                }
            }
            for (int slot = 0; slot < kHotbarSlotCount; ++slot)
            {
                ItemStack incoming;
                if (slot < static_cast<int>(s.inventory.hotbar.size()))
                {
                    incoming.type = static_cast<ItemType>(s.inventory.hotbar[slot].itemType);
                    incoming.count = s.inventory.hotbar[slot].count;
                }
                inventory.SwapSlot(slot, incoming);
            }
            for (int slot = 0; slot < kMainInventorySlotCount; ++slot)
            {
                ItemStack incoming;
                if (slot < static_cast<int>(s.inventory.main.size()))
                {
                    incoming.type = static_cast<ItemType>(s.inventory.main[slot].itemType);
                    incoming.count = s.inventory.main[slot].count;
                }
                inventory.SwapSlot(kHotbarSlotCount + slot, incoming);
            }
            // Note: selectedHotbarSlot_ is owned/predicted locally (see
            // SampleClientInput) — don't overwrite it from the laggy
            // snapshot, or a just-changed slot would flicker back for one RTT.
        }

        // Owner-private ability HUD state survives the visibility filter only
        // for ourselves; mirror it onto HeroRuntimeState so the ability HUD
        // (Renderer's AbilityStateText) shows real cooldowns/charge instead of
        // being permanently stuck on "готово" (HeroRuntimeState is never
        // otherwise predicted/updated locally for a server-driven player).
        if (s.playerId == networkAssignedPlayerId_ && s.abilityHud.present)
        {
            HeroRuntimeState& hudHeroState = player->MutableHeroState();
            hudHeroState.active1.cooldownRemaining = s.abilityHud.active1Cooldown;
            hudHeroState.active1.activeTimer = s.abilityHud.active1ActiveTimer;
            hudHeroState.active2.cooldownRemaining = s.abilityHud.active2Cooldown;
            hudHeroState.active2.activeTimer = s.abilityHud.active2ActiveTimer;
            hudHeroState.ultimate.cooldownRemaining = s.abilityHud.ultimateCooldown;
            hudHeroState.ultimate.activeTimer = s.abilityHud.ultimateActiveTimer;
            hudHeroState.ultimateCharge = s.abilityHud.ultimateCharge;
            hudHeroState.ultimatePrimed = s.abilityHud.ultimatePrimed;
            player->SetBowDrawTimerReplicated(s.abilityHud.bowDrawTimer);
            player->SetBlasterStateReplicated(
                static_cast<CrossbowState>(s.abilityHud.blasterState), s.abilityHud.blasterLoadTimer);
        }
    }

    for (const TeamChestSnapshot& chestSnapshot : snapshot.teamChests)
    {
        if (chestSnapshot.teamId < 0 || chestSnapshot.teamId >= static_cast<int>(teamChests_.size()))
        {
            continue;
        }
        Inventory& chest = teamChests_[chestSnapshot.teamId];
        const ResourceType resourceOrder[3] = {
            ResourceType::Iron, ResourceType::Gold, ResourceType::Crystal };
        for (int i = 0; i < 3; ++i)
        {
            const int current = chest.GetResource(resourceOrder[i]);
            const int target = chestSnapshot.resources[i];
            if (target > current)
            {
                chest.AddResource(resourceOrder[i], target - current);
            }
            else if (target < current)
            {
                chest.SpendResource(resourceOrder[i], current - target);
            }
        }
        for (int slot = 0; slot < kInventorySlotCount; ++slot)
        {
            ItemStack incoming;
            if (slot < static_cast<int>(chestSnapshot.slots.size()))
            {
                incoming.type = static_cast<ItemType>(chestSnapshot.slots[slot].itemType);
                incoming.count = chestSnapshot.slots[slot].count;
            }
            chest.SwapSlot(slot, incoming);
        }
    }

    // Cores: reflect authoritative health (SetHealth derives alive_).
    for (EnergyCore& core : matchSimulation_.Cores())
    {
        for (const CoreSnapshot& cs : snapshot.cores)
        {
            if (cs.teamId == core.GetTeamId())
            {
                core.SetHealth(cs.health);
                break;
            }
        }
    }

    // World items: rebuild from the snapshot each tick (positions already Vec3).
    std::vector<ResourcePickup>& pickups = matchSimulation_.Pickups();
    pickups.clear();
    for (const PickupSnapshot& p : snapshot.pickups)
    {
        ResourcePickup pickup;
        pickup.type = static_cast<ResourceType>(p.resourceType);
        pickup.amount = p.amount;
        pickup.position = p.position;
        pickups.push_back(pickup);
    }
    std::vector<DroppedItem>& droppedItems = matchSimulation_.DroppedItems();
    std::vector<int> visibleDroppedIds;
    visibleDroppedIds.reserve(snapshot.droppedItems.size());
    for (const DroppedItemSnapshot& d : snapshot.droppedItems)
    {
        DroppedItem* existing = nullptr;
        if (d.id > 0)
        {
            visibleDroppedIds.push_back(d.id);
            const auto found = std::find_if(
                droppedItems.begin(), droppedItems.end(),
                [&d](const DroppedItem& item) { return item.id == d.id; });
            if (found != droppedItems.end())
            {
                existing = &*found;
            }
        }

        DroppedItem fresh;
        fresh.stack.type = static_cast<ItemType>(d.itemType);
        fresh.stack.count = d.count;
        fresh.position = d.position;
        fresh.velocity = d.velocity;
        fresh.ownerPlayerId = d.ownerPlayerId;
        fresh.ownerPickupDelay = d.ownerPickupDelay;
        fresh.lifetime = d.lifetime;
        fresh.age = d.age;
        fresh.collected = false;
        fresh.id = d.id;

        if (existing == nullptr)
        {
            droppedItems.push_back(fresh);
            continue;
        }

        existing->stack = fresh.stack;
        existing->velocity = fresh.velocity;
        existing->ownerPlayerId = fresh.ownerPlayerId;
        existing->ownerPickupDelay = fresh.ownerPickupDelay;
        existing->lifetime = fresh.lifetime;
        existing->age = fresh.age;
        existing->collected = false;
        const float dx = fresh.position.x - existing->position.x;
        const float dy = fresh.position.y - existing->position.y;
        const float dz = fresh.position.z - existing->position.z;
        if (dx * dx + dy * dy + dz * dz > 4.0f)
        {
            existing->position = fresh.position;
        }
        else
        {
            existing->position.x += dx * 0.45f;
            existing->position.y += dy * 0.45f;
            existing->position.z += dz * 0.45f;
        }
    }
    droppedItems.erase(
        std::remove_if(
            droppedItems.begin(),
            droppedItems.end(),
            [&visibleDroppedIds](const DroppedItem& item)
            {
                return item.id > 0
                    && std::find(visibleDroppedIds.begin(), visibleDroppedIds.end(), item.id)
                        == visibleDroppedIds.end();
            }),
        droppedItems.end());

    // Block edits: fold the rolling delta stream onto the deterministic base map.
    for (const BlockDelta& delta : snapshot.blockDeltas)
    {
        if (delta.newType == BlockType::Air)
        {
            world_.RemoveBlock(delta.position);
        }
        else
        {
            world_.PlaceBlock(delta.position,
                Block { delta.newType, delta.newTeamId, IsBreakableByPlayers(delta.newType) }, true);
        }
    }

    // Dynamic gameplay entities: rebuild the client-side runtime containers from
    // the authoritative snapshot, matching the pickups/dropped-items contract
    // above. The snapshot id is preserved in order/shape today; stable spawn ids
    // for interpolation are a later pass.
    const std::vector<EnergyProjectile> previousProjectiles = projectiles_;
    replicatedProjectiles_ = snapshot.projectiles;
    replicatedExplosives_ = snapshot.explosives;
    replicatedHazardZones_ = snapshot.hazardZones;
    replicatedHeroDevices_ = snapshot.heroDevices;

    projectiles_.clear();
    projectiles_.reserve(snapshot.projectiles.size());
    for (const ProjectileSnapshot& p : snapshot.projectiles)
    {
        EnergyProjectile projectile;
        projectile.id = p.id;
        projectile.kind = ProjectileKindFromSnapshot(p.kind);
        projectile.position = SnapshotVecToRay(p.position);
        if (const EnergyProjectile* previousProjectile = FindMatchingVisualProjectile(previousProjectiles, p))
        {
            projectile.previousPosition = previousProjectile->position;
        }
        else
        {
            projectile.previousPosition = projectile.position;
        }
        projectile.startPosition = projectile.position;
        projectile.velocity = SnapshotVecToRay(p.velocity);
        projectile.ownerId = p.ownerPlayerId;
        projectile.ownerTeamId = p.ownerTeamId;
        projectile.lifetime = p.remainingLifetime;
        projectile.fireZone = p.fireZone;
        ApplyProjectileDefaults(projectile);
        projectiles_.push_back(projectile);
    }

    timedExplosions_.clear();
    timedExplosions_.reserve(snapshot.explosives.size());
    for (const ExplosiveSnapshot& e : snapshot.explosives)
    {
        TimedExplosion explosive;
        explosive.block = GridPos {
            static_cast<int>(std::lround(e.position.x)),
            static_cast<int>(std::lround(e.position.y)),
            static_cast<int>(std::lround(e.position.z))
        };
        explosive.ownerPlayerId = e.ownerPlayerId;
        explosive.ownerTeamId = e.ownerTeamId;
        explosive.timer = e.remainingTimer;
        explosive.radius = e.radius;
        timedExplosions_.push_back(explosive);
    }

    hazardZones_.clear();
    hazardZones_.reserve(snapshot.hazardZones.size());
    for (const HazardZoneSnapshot& h : snapshot.hazardZones)
    {
        HazardZone zone;
        zone.position = SnapshotVecToRay(h.position);
        zone.ownerPlayerId = h.ownerPlayerId;
        zone.ownerTeamId = h.ownerTeamId;
        zone.lifetime = h.remainingLifetime;
        zone.radius = h.radius;
        zone.damagePerTick = h.blueFire ? 16 : 8;
        zone.blueFire = h.blueFire;
        hazardZones_.push_back(zone);
    }

    bromVacuumBots_.clear();
    bromTurretDrones_.clear();
    konvoyTraps_.clear();
    konvoyTethers_.clear();
    konvoyDomes_.clear();
    likhoBleeds_.clear();
    svidetelEchoes_.clear();
    for (const HeroDeviceSnapshot& d : snapshot.heroDevices)
    {
        switch (d.type)
        {
        case HeroDeviceType::BromVacuumBot:
        {
            BromVacuumBot bot;
            bot.position = SnapshotVecToRay(d.position);
            bot.ownerPlayerId = d.ownerPlayerId;
            bot.ownerTeamId = d.ownerTeamId;
            bot.lifetime = d.remainingLifetime;
            bot.health = d.health;
            bromVacuumBots_.push_back(bot);
            break;
        }
        case HeroDeviceType::BromTurretDrone:
        {
            BromTurretDrone drone;
            drone.position = SnapshotVecToRay(d.position);
            drone.ownerPlayerId = d.ownerPlayerId;
            drone.ownerTeamId = d.ownerTeamId;
            drone.lifetime = d.remainingLifetime;
            drone.health = d.health;
            drone.lastShotTarget = Vector3 { drone.position.x, drone.position.y, drone.position.z + 1.0f };
            bromTurretDrones_.push_back(drone);
            break;
        }
        case HeroDeviceType::KonvoyTrap:
        {
            KonvoyTrap trap;
            trap.position = SnapshotVecToRay(d.position);
            trap.ownerPlayerId = d.ownerPlayerId;
            trap.ownerTeamId = d.ownerTeamId;
            trap.lifetime = d.remainingLifetime;
            trap.health = d.health;
            konvoyTraps_.push_back(trap);
            break;
        }
        case HeroDeviceType::KonvoyTether:
        {
            KonvoyTether tether;
            tether.ownerPlayerId = d.ownerPlayerId;
            tether.targetPlayerId = d.targetPlayerId;
            tether.ownerTeamId = d.ownerTeamId;
            tether.lifetime = d.remainingLifetime;
            konvoyTethers_.push_back(tether);
            break;
        }
        case HeroDeviceType::KonvoyDome:
        {
            KonvoyDome dome;
            dome.position = SnapshotVecToRay(d.position);
            dome.ownerPlayerId = d.ownerPlayerId;
            dome.ownerTeamId = d.ownerTeamId;
            dome.lifetime = d.remainingLifetime;
            dome.health = d.health;
            konvoyDomes_.push_back(dome);
            break;
        }
        case HeroDeviceType::SvidetelEcho:
        {
            SvidetelEcho echo;
            echo.position = SnapshotVecToRay(d.position);
            echo.ownerPlayerId = d.ownerPlayerId;
            echo.ownerTeamId = d.ownerTeamId;
            echo.lifetime = d.remainingLifetime;
            echo.health = d.health;
            echo.lastTarget = Vector3 { echo.position.x, echo.position.y, echo.position.z + 1.0f };
            svidetelEchoes_.push_back(echo);
            break;
        }
        case HeroDeviceType::LikhoBleed:
        {
            LikhoBleed bleed;
            bleed.targetPlayerId = d.targetPlayerId;
            bleed.ownerPlayerId = d.ownerPlayerId;
            bleed.ownerTeamId = d.ownerTeamId;
            bleed.lifetime = d.remainingLifetime;
            bleed.stacks = std::max(1, d.health);
            likhoBleeds_.push_back(bleed);
            break;
        }
        case HeroDeviceType::LikhoDisguise:
            // The player identity swap is already applied through PlayerSnapshot
            // filtering; keep the marker in replicatedHeroDevices_ for future UI.
            break;
        }
    }

    replicatedStatusEffects_ = snapshot.statusEffects;
    radonBurns_.clear();
    konvoyIntruderMarks_.clear();
    for (const StatusEffectSnapshot& s : snapshot.statusEffects)
    {
        if (s.type == StatusEffectType::RadonBurn)
        {
            RadonBurn burn;
            burn.targetPlayerId = s.targetPlayerId;
            burn.ownerPlayerId = s.ownerPlayerId;
            burn.ownerTeamId = s.ownerTeamId;
            burn.lifetime = s.remaining;
            burn.tickTimer = 0.7f;
            burn.blueFire = s.amount != 0;
            radonBurns_.push_back(burn);
        }
        else if (s.type == StatusEffectType::KonvoyMark)
        {
            KonvoyIntruderMark mark;
            mark.targetPlayerId = s.targetPlayerId;
            mark.ownerPlayerId = s.ownerPlayerId;
            mark.markedTimer = s.remaining;
            konvoyIntruderMarks_.push_back(mark);
        }
    }

    // Match clock/phase/winner so the HUD timer + win/end overlays render right
    // (the client runs no clock of its own).
    matchSimulation_.SetMatchTimeSeconds(snapshot.matchTime);
    matchSimulation_.SetPhase(snapshot.phase);
    if (snapshot.winnerTeamId >= 0)
    {
        matchSimulation_.SetWinner(snapshot.winnerTeamId);
    }

    clientFeelSnapshot_ = snapshot;
    hasClientFeelSnapshot_ = true;
}

bool Game::ShouldApplyClientSnapshot(const MatchSnapshot& snapshot)
{
    // Fold each authoritative snapshot exactly once. The render loop runs far
    // faster than the ~20 Hz snapshot stream, so without this gate the heavy work
    // (roster reconcile, world block-delta replay, pickup/dropped rebuild,
    // reconciliation) ran every frame on an unchanged snapshot — a needless FPS
    // sink. Remote players still glide every frame via UpdateRemoteInterpolation.
    if (hasFoldedSnapshot_ && snapshot.tick <= lastFoldedSnapshotTick_)
    {
        return false;
    }
    hasFoldedSnapshot_ = true;
    lastFoldedSnapshotTick_ = snapshot.tick;
    return true;
}

void Game::UpdateRemoteInterpolation(float dt)
{
    // Every rendered frame: glide remote players from the delayed interpolation
    // buffer so they move smoothly between the sparse snapshots. Our own player is
    // owned by client-side prediction; skip it here.
    if (!remoteSnapshotBuffer_.empty() && hasNetworkRemoteRenderTime_)
    {
        const float latestAllowedTime =
            remoteSnapshotBuffer_.back().matchTime + std::max(0.0f, networkInterpolationDelaySeconds_);
        networkRemoteRenderTime_ = std::min(
            latestAllowedTime,
            networkRemoteRenderTime_ + std::max(0.0f, dt));
    }

    for (Player& player : matchSimulation_.Players())
    {
        if (player.GetId() == networkAssignedPlayerId_)
        {
            continue;
        }
        Vec3 interpolatedPosition {};
        if (TryGetInterpolatedRemotePlayerPosition(
                player.GetId(), networkInterpolationDelaySeconds_, interpolatedPosition))
        {
            player.SetPosition(interpolatedPosition);
        }
        float interpolatedYaw = 0.0f;
        if (TryGetInterpolatedRemotePlayerYaw(
                player.GetId(), networkInterpolationDelaySeconds_, interpolatedYaw))
        {
            player.SetYaw(interpolatedYaw);
        }
    }
}

void Game::UpdateClientReplicatedDynamics(float dt)
{
    if (networkMode_ != NetworkMode::LocalClient || dt <= 0.0f)
    {
        return;
    }

    const float fixedDt = std::max(0.0001f, matchSimulation_.FixedDeltaSeconds());
    const float dragTicks = std::max(0.0f, dt / fixedDt);
    for (EnergyProjectile& projectile : projectiles_)
    {
        projectile.previousPosition = projectile.position;
        projectile.velocity.y -= projectile.gravity * dt;
        if (projectile.affectedByDrag)
        {
            const float drag = std::pow(projectile.airDragPerTick, dragTicks);
            projectile.velocity.x *= drag;
            projectile.velocity.y *= drag;
            projectile.velocity.z *= drag;
        }
        projectile.position.x += projectile.velocity.x * dt;
        projectile.position.y += projectile.velocity.y * dt;
        projectile.position.z += projectile.velocity.z * dt;
        projectile.distanceTraveled += std::sqrt(
            projectile.velocity.x * projectile.velocity.x
            + projectile.velocity.y * projectile.velocity.y
            + projectile.velocity.z * projectile.velocity.z) * dt;
        projectile.lifetime = std::max(0.0f, projectile.lifetime - dt);
    }

    for (DroppedItem& dropped : matchSimulation_.DroppedItems())
    {
        dropped.age += dt;
        dropped.lifetime = std::max(0.0f, dropped.lifetime - dt);
        dropped.velocity.y -= 9.0f * dt;

        Player* magnetTarget = nullptr;
        float bestDistanceSq = kClientDroppedItemMagnetRadius * kClientDroppedItemMagnetRadius;
        const Vector3 itemPosition = ToRaylibVector3(dropped.position);
        for (Player& player : players_)
        {
            if (!player.IsAlive() || player.IsEliminated())
            {
                continue;
            }
            if (player.GetId() == dropped.ownerPlayerId && dropped.age < dropped.ownerPickupDelay)
            {
                continue;
            }

            const Vector3 target = ClientPickupTargetFor(player);
            const float dx = target.x - itemPosition.x;
            const float dy = target.y - itemPosition.y;
            const float dz = target.z - itemPosition.z;
            const float distanceSq = dx * dx + dy * dy + dz * dz;
            if (distanceSq < bestDistanceSq)
            {
                bestDistanceSq = distanceSq;
                magnetTarget = &player;
            }
        }

        if (magnetTarget != nullptr)
        {
            const Vector3 target = ClientPickupTargetFor(*magnetTarget);
            const Vec3 toTarget {
                target.x - dropped.position.x,
                target.y - dropped.position.y,
                target.z - dropped.position.z
            };
            const float distance = LengthVec3(toTarget);
            if (distance > 0.0001f)
            {
                const float closeness = 1.0f - std::min(distance / kClientDroppedItemMagnetRadius, 1.0f);
                const float accel = kClientDroppedItemMagnetAccel * (0.55f + closeness * 1.15f);
                dropped.velocity.x += (toTarget.x / distance) * accel * dt;
                dropped.velocity.y += (toTarget.y / distance) * accel * dt;
                dropped.velocity.z += (toTarget.z / distance) * accel * dt;
                const float speed = LengthVec3(dropped.velocity);
                if (speed > kClientDroppedItemMagnetMaxSpeed)
                {
                    dropped.velocity.x = dropped.velocity.x / speed * kClientDroppedItemMagnetMaxSpeed;
                    dropped.velocity.y = dropped.velocity.y / speed * kClientDroppedItemMagnetMaxSpeed;
                    dropped.velocity.z = dropped.velocity.z / speed * kClientDroppedItemMagnetMaxSpeed;
                }
            }
        }

        dropped.position.x += dropped.velocity.x * dt;
        dropped.position.y += dropped.velocity.y * dt;
        dropped.position.z += dropped.velocity.z * dt;
        const GridPos under = world_.WorldToGrid(Vector3 {
            dropped.position.x, dropped.position.y - 0.22f, dropped.position.z });
        if (!world_.IsAir(under) && dropped.velocity.y < 0.0f)
        {
            dropped.position.y = world_.GridToWorld(under).y + 0.72f;
            dropped.velocity = Vec3 { dropped.velocity.x * 0.72f, 0.0f, dropped.velocity.z * 0.72f };
        }
    }

    for (TimedExplosion& explosive : timedExplosions_)
    {
        explosive.timer = std::max(0.0f, explosive.timer - dt);
    }
    for (HazardZone& zone : hazardZones_)
    {
        zone.lifetime = std::max(0.0f, zone.lifetime - dt);
        zone.tickTimer += dt;
    }
    for (BromVacuumBot& bot : bromVacuumBots_)
    {
        bot.lifetime = std::max(0.0f, bot.lifetime - dt);
        bot.pulseTimer += dt;
    }
    for (BromTurretDrone& drone : bromTurretDrones_)
    {
        drone.lifetime = std::max(0.0f, drone.lifetime - dt);
        drone.pulseTimer += dt;
        drone.shotFlashTimer = std::max(0.0f, drone.shotFlashTimer - dt);
    }
    for (KonvoyTrap& trap : konvoyTraps_)
    {
        trap.lifetime = std::max(0.0f, trap.lifetime - dt);
        trap.flashTimer = std::max(0.0f, trap.flashTimer - dt);
    }
    for (KonvoyTether& tether : konvoyTethers_)
    {
        tether.lifetime = std::max(0.0f, tether.lifetime - dt);
        tether.flashTimer = std::max(0.0f, tether.flashTimer - dt);
    }
    for (KonvoyDome& dome : konvoyDomes_)
    {
        dome.lifetime = std::max(0.0f, dome.lifetime - dt);
        dome.flashTimer = std::max(0.0f, dome.flashTimer - dt);
        dome.chargeTimer += dt;
    }
    for (LikhoBleed& bleed : likhoBleeds_)
    {
        bleed.lifetime = std::max(0.0f, bleed.lifetime - dt);
        bleed.tickTimer += dt;
    }
    for (SvidetelEcho& echo : svidetelEchoes_)
    {
        echo.lifetime = std::max(0.0f, echo.lifetime - dt);
        echo.fireCooldown = std::max(0.0f, echo.fireCooldown - dt);
        echo.flashTimer = std::max(0.0f, echo.flashTimer - dt);
    }
    for (RadonBurn& burn : radonBurns_)
    {
        burn.lifetime = std::max(0.0f, burn.lifetime - dt);
        burn.tickTimer += dt;
    }
    for (KonvoyIntruderMark& mark : konvoyIntruderMarks_)
    {
        mark.markedTimer = std::max(0.0f, mark.markedTimer - dt);
        mark.pulseTimer += dt;
    }
}

bool Game::IsPlayerNearTeamChestAccess(const Player& player) const
{
    const Team* team = FindTeam(player.GetTeamId());
    if (team == nullptr)
    {
        return false;
    }
    const auto distanceSq = [](Vector3 a, Vector3 b)
    {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    };
    const Vector3 chestPosition = world_.GridToWorld(team->teamChestBlock);
    return distanceSq(player.GetPosition(), team->shopPosition) <= 12.0f
        || distanceSq(player.GetPosition(), chestPosition) <= 16.0f;
}

void Game::HandleNetworkClientShopInput(Player& player)
{
    if (shopOpen_ && !IsLocalPlayerInShopZone())
    {
        shopOpen_ = false;
        if (!headless_)
        {
            DisableCursor();
        }
        currentInput_ = PlayerInput {};
        return;
    }

    if (shopOpen_ && (currentInput_.exitPressed || currentInput_.interactPressed))
    {
        shopOpen_ = false;
        if (!headless_)
        {
            DisableCursor();
        }
        currentInput_ = PlayerInput {};
        return;
    }

    // Stage 4.2: the browsing/purchase logic itself is the ONE shared handler
    // (Game.cpp HandleShopBrowseInput) — this wrapper only owns the MP-client
    // close conditions above.
    HandleShopBrowseInput(player);
    currentInput_ = PlayerInput {};
}

void Game::HandleNetworkClientUiInput()
{
    if (networkMode_ != NetworkMode::LocalClient)
    {
        return;
    }

    Player* localPlayer = matchSimulation_.GetPlayer(networkAssignedPlayerId_);
    if (localPlayer == nullptr)
    {
        return;
    }

    if (inventoryOpen_ && teamChestOpen_ && !IsPlayerNearTeamChestAccess(*localPlayer))
    {
        CloseChest();
        heldInventoryStack_ = ItemStack {};
        heldInventoryOrigin_ = HeldInventoryOrigin::None;
        heldInventoryOriginSlot_ = -1;
    }

    if (inventoryOpen_)
    {
        if (currentInput_.exitPressed || currentInput_.inventoryPressed)
        {
            inventoryOpen_ = false;
            CloseChest();
            heldInventoryStack_ = ItemStack {};
            heldInventoryOrigin_ = HeldInventoryOrigin::None;
            heldInventoryOriginSlot_ = -1;
            if (!headless_)
            {
                DisableCursor();
            }
            currentInput_ = PlayerInput {};
            return;
        }
        HandleInventoryInput(*localPlayer);
        currentInput_ = PlayerInput {};
        return;
    }

    if (shopOpen_)
    {
        HandleNetworkClientShopInput(*localPlayer);
        return;
    }

    if (currentInput_.exitPressed)
    {
        clientPaused_ = true;
        screen_ = GameScreen::Paused;
        pauseIndex_ = 0;
        pendingClientInput_ = PlayerInput {};
        ResetBreakProgress();
        blasterCharging_ = false;
        attackChargeActive_ = false;
        attackChargeTimer_ = 0.0f;
        if (!headless_)
        {
            EnableCursor();
        }
        currentInput_ = PlayerInput {};
        return;
    }

    if (currentInput_.inventoryPressed)
    {
        inventoryOpen_ = true;
        shopOpen_ = false;
        CloseChest();
        inventoryCursorSlot_ = selectedHotbarSlot_;
        heldInventoryStack_ = ItemStack {};
        heldInventoryOrigin_ = HeldInventoryOrigin::None;
        heldInventoryOriginSlot_ = -1;
        if (!headless_)
        {
            EnableCursor();
        }
        currentInput_ = PlayerInput {};
        return;
    }

    if (currentInput_.dropPressed)
    {
        const ItemStack stack = localPlayer->GetInventory().GetSlot(selectedHotbarSlot_);
        if (!stack.IsEmpty())
        {
            const int amount = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? stack.count : 1;
            QueuePlayerAction(PlayerActionType::DropItem, selectedHotbarSlot_, amount);
            // Same call pair the SP frame path uses: off the integrated server
            // this is a no-op and the queued action ships with the next command.
            ApplyPendingLocalPlayerAction(*localPlayer);
        }
        currentInput_ = PlayerInput {};
        return;
    }

    // Stage 4.3: aimed team chest goes through the ONE shared entry (open own /
    // deny enemy) the SP frame path uses.
    if (currentInput_.interactPressed || currentInput_.placePressed)
    {
        if (TryOpenAimedTeamChest(*localPlayer))
        {
            currentInput_ = PlayerInput {};
            return;
        }
    }

    if (currentInput_.interactPressed)
    {
        if (IsLocalPlayerInShopZone())
        {
            shopOpen_ = true;
            inventoryOpen_ = false;
            CloseChest();
            heldInventoryStack_ = ItemStack {};
            heldInventoryOrigin_ = HeldInventoryOrigin::None;
            heldInventoryOriginSlot_ = -1;
            if (!headless_)
            {
                EnableCursor();
            }
        }
        currentInput_ = PlayerInput {};
        return;
    }
}

void Game::HandleNetworkClientLookAndHotbarInput(Player& player)
{
    if (shopOpen_ || inventoryOpen_)
    {
        return;
    }

    cameraController_.AddLook(currentInput_.yawDelta, currentInput_.pitchDelta);
    // First/third person toggle — mirrors the SP frame path (Stage 4.3 closed
    // this MP gap; the renderer already hides the local mesh only in first
    // person, and the merged UpdateCamera handles both modes).
    if (currentInput_.cameraTogglePressed)
    {
        cameraController_.ToggleMode();
        SetMessage(std::string("Камера: ") + cameraController_.GetModeName(), 1.6f);
    }
    // Hero-ability presentation (the Orbita teleport preview): the cast itself
    // rides the command; this is the same presentation-only hook the SP frame
    // path calls (Stage 4.3 closed this MP gap). Mirrors SP's
    // keepUltimateKeyForShop rule: an ultimate press inside the shop zone is
    // the shop-open key, not a cast.
    const bool heroAbilityPressed = currentInput_.heroActive1Pressed
        || currentInput_.heroActive2Pressed
        || currentInput_.heroUltimatePressed;
    const bool keepUltimateKeyForShop = currentInput_.heroUltimatePressed && IsLocalPlayerInShopZone();
    if (player.IsAlive() && heroAbilityPressed && !keepUltimateKeyForShop)
    {
        UseHeroAbilityInputs(player);
    }
    // Stage 4.2: hotbar selection / wheel / sniper magnification are the ONE
    // shared handler (Game.cpp HandleHotbarSelectionInput) — this also aligned
    // the MP magnification range with SP's 1.5–3.0 (was a divergent 1.2–4.0).
    HandleHotbarSelectionInput(player);
}

void Game::SampleClientInput()
{
    // One-time: align the camera yaw to the assigned player's spawn facing so the
    // first frame doesn't snap. Done once the player exists (after the first snapshot).
    if (!clientAimInitialized_)
    {
        const Player* assigned = matchSimulation_.GetPlayer(networkAssignedPlayerId_);
        if (assigned != nullptr)
        {
            float yawDelta = assigned->GetYaw() - cameraController_.GetYaw();
            while (yawDelta > PI) yawDelta -= 2.0f * PI;
            while (yawDelta < -PI) yawDelta += 2.0f * PI;
            cameraController_.AddLook(yawDelta, 0.0f);
            clientAimInitialized_ = true;
        }
    }

    // Only sample real input while actively playing. Paused (ESC) or an unfocused
    // window yields neutral input so the character does not keep moving — this
    // mirrors single-player zeroing currentInput_ in menus/pause.
    Player* localPlayer = matchSimulation_.GetPlayer(networkAssignedPlayerId_);
    const bool active = !clientPaused_ && IsWindowFocused();
    if (active)
    {
        currentInput_ = input_.Poll();
        scoreboardHeld_ = IsKeyDown(KEY_TAB);
        HandleNetworkClientUiInput();
        if (!shopOpen_ && !inventoryOpen_ && localPlayer != nullptr)
        {
            HandleNetworkClientLookAndHotbarInput(*localPlayer);
        }
    }
    else
    {
        currentInput_ = PlayerInput {};
        scoreboardHeld_ = false;
    }

    // Accumulate this frame's input so edge (press) events aren't lost on frames
    // that run zero fixed steps, nor double-fired on frames that run several.
    pendingClientInput_ = MergeClientInput(pendingClientInput_, currentInput_);
}

void Game::StepClientPredictionAndSend(ClientTransport& client, float fixedDt)
{
    // Consume the accumulated input for exactly one fixed simulation step.
    currentInput_ = pendingClientInput_;

    // Build the command for the assigned player. The tick is a monotonic per-step
    // counter (NOT derived from the received snapshot): the previous design read
    // command.tick from the ~20 Hz snapshot, so the 60 Hz server dropped most
    // commands as stale and the player crawled at ~1/3 speed. A monotonic tick at
    // the sim rate means the server applies ~one input per tick — full speed,
    // and prediction integrates the same way the server does (fixedDt steps).
    PlayerCommand command = BuildLocalPlayerCommand();
    command.controlledPlayerId = static_cast<std::uint32_t>(client.AssignedPlayerId());
    command.tick = ++networkCommandTick_;

    if (Player* localPlayer = matchSimulation_.GetPlayer(client.AssignedPlayerId()))
    {
        // Client-side prediction at the fixed timestep: apply our own command
        // locally so the player (and the following camera) advance smoothly while
        // the server stays authoritative. ApplyAuthoritativeSnapshotForPrediction
        // reconciles it; ApplyClientSnapshot doesn't overwrite the predicted pos.
        // Store after applying so history holds the post-move position.
        const bool canPredictControlledPlayer = localPlayer->IsAlive() && !spectatorMode_;
        if (canPredictControlledPlayer)
        {
            ApplyPredictedPlayerCommand(*localPlayer, command, fixedDt);
            UpdatePredictedRangedCharge(*localPlayer, command, fixedDt);
            UpdatePredictedBreakProgress(*localPlayer, command, fixedDt);
        }
        else
        {
            command.moveForward = 0.0f;
            command.moveStrafe = 0.0f;
            command.jump = false;
            command.sneak = false;
            command.sprint = false;
            command.attackPressed = false;
            command.attackHeld = false;
            command.attackReleased = false;
            command.placePressed = false;
            command.placeHeld = false;
            command.scopeHeld = false;
            command.interact = false;
            command.useAbility1 = false;
            command.useAbility2 = false;
            command.useUltimate = false;
            command.useHeal = false;
            command.useTeleport = false;
            command.useDash = false;
            command.useShoot = false;
            command.useFireball = false;
            command.useMolotov = false;
            command.useAlarm = false;
            command.actionSeq = 0;
            command.actionType = static_cast<int>(PlayerActionType::None);
            command.actionParamA = 0;
            command.actionParamB = 0;
            pendingEconomyActionType_ = PlayerActionType::None;
        }
        // Drive our own animation locally from the predicted velocity at the fixed
        // tick rate (zero lag): ticks any server-adopted event pose down, then
        // derives locomotion. ApplyClientSnapshot only adopts EVENT poses for us.
        localPlayer->AdvanceAnimation(fixedDt);
        StorePredictedLocalCommand(command, *localPlayer);
    }
    client.SendCommand(command);

    // A discrete economy action is one-shot: once shipped in a command, stop
    // re-sending it (the server's per-player seq dedupe guards re-application).
    // The seq counter keeps climbing so the next action is strictly newer.
    if (command.actionSeq != 0)
    {
        pendingEconomyActionType_ = PlayerActionType::None;
    }

    // The step consumed the edge inputs; clear them so the next step starts fresh.
    ClearClientInputEdges(pendingClientInput_);
}

namespace
{
struct LobbyUiLayout
{
    Rectangle teamButtons[4] {};
    Rectangle heroButtons[HeroSystem::kHeroCount] {};
    Rectangle readyButton {};
    Rectangle startButton {};
};

const LobbyPlayerState* FindLobbyPlayer(const LobbySnapshot& lobby, int clientId)
{
    for (const LobbyPlayerState& player : lobby.players)
    {
        if (player.clientId == clientId)
        {
            return &player;
        }
    }
    return nullptr;
}

std::string LobbyHeroName(int heroIndex)
{
    if (heroIndex < 0 || heroIndex >= HeroSystem::kHeroCount)
    {
        return "Auto";
    }
    return HeroSystem::GetDefinitionByIndex(heroIndex).name;
}

Color LobbyTeamColor(int teamId)
{
    switch (teamId)
    {
    case 0:
        return Color { 230, 74, 74, 255 };
    case 1:
        return Color { 74, 135, 230, 255 };
    case 2:
        return Color { 64, 190, 110, 255 };
    case 3:
        return Color { 236, 202, 72, 255 };
    default:
        return Fade(WHITE, 0.7f);
    }
}

const char* LobbyTeamName(int teamId)
{
    switch (teamId)
    {
    case 0:
        return "Красные";
    case 1:
        return "Синие";
    case 2:
        return "Зеленые";
    case 3:
        return "Желтые";
    default:
        return "Авто";
    }
}

LobbyUiLayout BuildLobbyUiLayout()
{
    const int screenW = GetScreenWidth();
    const int panelW = std::min(1120, std::max(620, screenW - 64));
    const int x = screenW / 2 - panelW / 2;
    const int controlsX = x + panelW - 372;

    LobbyUiLayout layout;
    for (int i = 0; i < 4; ++i)
    {
        layout.teamButtons[i] = Rectangle {
            static_cast<float>(controlsX + (i % 2) * 150),
            static_cast<float>(262 + (i / 2) * 42),
            138.0f,
            34.0f
        };
    }
    for (int i = 0; i < HeroSystem::kHeroCount; ++i)
    {
        layout.heroButtons[i] = Rectangle {
            static_cast<float>(controlsX + (i % 2) * 150),
            static_cast<float>(386 + (i / 2) * 42),
            138.0f,
            34.0f
        };
    }
    layout.readyButton = Rectangle { static_cast<float>(controlsX), 548.0f, 138.0f, 40.0f };
    layout.startButton = Rectangle { static_cast<float>(controlsX + 150), 548.0f, 138.0f, 40.0f };
    return layout;
}

bool LobbyButtonPressed(Rectangle bounds, bool enabled)
{
    return enabled
        && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)
        && CheckCollisionPointRec(GetMousePosition(), bounds);
}

void DrawLobbyButton(Rectangle bounds, const std::string& label, bool selected, bool enabled, Color accent)
{
    const bool hot = enabled && CheckCollisionPointRec(GetMousePosition(), bounds);
    const Color bg = selected
        ? Fade(accent, hot ? 0.72f : 0.58f)
        : (enabled ? Fade(Color { 30, 36, 46, 255 }, hot ? 0.96f : 0.78f)
                   : Fade(Color { 26, 28, 34, 255 }, 0.46f));
    const Color line = selected ? Fade(accent, 0.96f) : Fade(WHITE, enabled ? 0.22f : 0.10f);
    const Color text = enabled ? WHITE : Fade(WHITE, 0.36f);
    DrawRectangleRec(bounds, bg);
    DrawRectangleLinesEx(bounds, 1.0f, line);
    const int fontSize = 15;
    const int textW = MeasureText(label.c_str(), fontSize);
    DrawText(label.c_str(),
             static_cast<int>(bounds.x + bounds.width / 2.0f - textW / 2.0f),
             static_cast<int>(bounds.y + bounds.height / 2.0f - 8.0f),
             fontSize,
             text);
}

void HandleNetworkLobbyControls(ClientTransport& client, LobbyUpdate& prefs, const LobbySnapshot& lobby,
                               bool devKeyboard)
{
    if (lobby.matchStarting || lobby.matchStarted)
    {
        return;
    }

    const int localClientId = client.LobbyClientId();
    const LobbyPlayerState* local = FindLobbyPlayer(lobby, localClientId);
    const bool isHost = localClientId >= 0 && localClientId == lobby.hostClientId;
    const bool ready = local != nullptr ? local->ready : prefs.ready;
    LobbyUpdate next = prefs;
    if (local != nullptr)
    {
        next.playerName = local->playerName;
        next.selectedTeam = local->selectedTeam;
        next.selectedHero = local->selectedHero;
        next.ready = local->ready;
        next.startRequested = local->startRequested;
    }
    if (next.playerName.empty())
    {
        next.playerName = "Игрок " + std::to_string(localClientId);
    }

    bool changed = false;
    const LobbyUiLayout layout = BuildLobbyUiLayout();
    const int teamCount = std::clamp(lobby.teamCount, 1, 4);
    for (int team = 0; team < teamCount; ++team)
    {
        if (LobbyButtonPressed(layout.teamButtons[team], true))
        {
            next.selectedTeam = team;
            next.startRequested = false;
            changed = true;
        }
    }

    const int heroCount = std::clamp(lobby.heroCount, 1, HeroSystem::kHeroCount);
    for (int hero = 0; hero < heroCount; ++hero)
    {
        if (LobbyButtonPressed(layout.heroButtons[hero], true))
        {
            next.selectedHero = hero;
            next.startRequested = false;
            changed = true;
        }
    }

    if (devKeyboard)
    {
        // Developer kit: keyboard team/hero selection (mouse-free lobby).
        // Keys 1..N pick a team; Z/X cycle the hero.
        for (int team = 0; team < teamCount; ++team)
        {
            if (IsKeyPressed(KEY_ONE + team))
            {
                next.selectedTeam = team;
                next.startRequested = false;
                changed = true;
            }
        }
        if (IsKeyPressed(KEY_Z))
        {
            next.selectedHero = (next.selectedHero + heroCount - 1) % heroCount;
            next.startRequested = false;
            changed = true;
        }
        if (IsKeyPressed(KEY_X))
        {
            next.selectedHero = (next.selectedHero + 1) % heroCount;
            next.startRequested = false;
            changed = true;
        }
    }

    if (LobbyButtonPressed(layout.readyButton, true) || IsKeyPressed(KEY_R))
    {
        next.ready = !ready;
        next.startRequested = false;
        changed = true;
    }

    const bool canRequestStart = isHost && lobby.canStart && next.ready;
    if (LobbyButtonPressed(layout.startButton, canRequestStart)
        || (canRequestStart && IsKeyPressed(KEY_ENTER)))
    {
        next.startRequested = true;
        changed = true;
    }

    if (changed)
    {
        prefs = next;
        client.SendLobbyUpdate(prefs);
    }
}

// One centered two-line message frame (connect failures, disconnects, etc.).
std::string LocalizeNetworkReason(const std::string& reason)
{
    if (reason == "bad password")
    {
        return "неверный пароль";
    }
    if (reason == "match already started")
    {
        return "матч уже начался";
    }
    if (reason == "lobby full")
    {
        return "лобби заполнено";
    }
    if (reason == "connect timed out")
    {
        return "время ожидания подключения истекло";
    }
    return reason;
}

std::string LocalizeLobbyStatus(const std::string& status)
{
    if (status == "match started")
    {
        return "матч начался";
    }
    if (status == "match starting")
    {
        return "матч запускается";
    }
    if (status == "can start")
    {
        return "можно начинать";
    }
    if (status == "waiting for players")
    {
        return "ожидание игроков";
    }
    if (status.rfind("waiting for ", 0) == 0 && status.find(" players") != std::string::npos)
    {
        const std::size_t begin = std::string("waiting for ").size();
        const std::size_t end = status.find(" players", begin);
        return "ожидание игроков: нужно " + status.substr(begin, end - begin);
    }
    if (status == "waiting for ready players")
    {
        return "ожидание готовности игроков";
    }
    if (status == "team full")
    {
        return "команда заполнена";
    }
    if (status == "duplicate hero in team")
    {
        return "в команде уже есть такой герой";
    }
    if (status == "invalid team selection")
    {
        return "некорректный выбор команды";
    }
    if (status == "invalid hero selection")
    {
        return "некорректный выбор героя";
    }
    if (status == "lobby full")
    {
        return "лобби заполнено";
    }
    return LocalizeNetworkReason(status);
}

void DrawClientMessageFrame(const char* title, const std::string& detail, Color titleColor)
{
    BeginDrawing();
    ClearBackground(Color { 14, 17, 24, 255 });
    const int centerX = GetScreenWidth() / 2;
    const int centerY = GetScreenHeight() / 2;
    DrawText(title, centerX - MeasureText(title, 30) / 2, centerY - 40, 30, titleColor);
    if (!detail.empty())
    {
        DrawText(detail.c_str(), centerX - MeasureText(detail.c_str(), 18) / 2, centerY + 6, 18,
                 Color { 190, 200, 214, 255 });
    }
    EndDrawing();
}
} // namespace

void Game::RenderNetworkLobby(const LobbySnapshot& lobby, int localClientId, const LobbyUpdate& localPrefs) const
{
    BeginDrawing();
    ClearBackground(Color { 14, 17, 24, 255 });

    const int screenW = GetScreenWidth();
    const int panelW = std::min(1120, std::max(620, screenW - 64));
    const int x = screenW / 2 - panelW / 2;
    const int listW = panelW - 430;
    const LobbyPlayerState* local = FindLobbyPlayer(lobby, localClientId);
    const bool locked = lobby.matchStarting || lobby.matchStarted;
    const bool isHost = localClientId >= 0 && localClientId == lobby.hostClientId;
    const int localTeam = local != nullptr ? local->selectedTeam : localPrefs.selectedTeam;
    const int localHero = local != nullptr ? local->selectedHero : localPrefs.selectedHero;
    const bool localReady = local != nullptr ? local->ready : localPrefs.ready;

    DrawText("Сетевое лобби DaiBed", x, 54, 30, RAYWHITE);
    const std::string server = "Сервер: " + (lobby.serverName.empty() ? std::string("Сервер DaiBed") : lobby.serverName);
    DrawText(server.c_str(), x, 94, 17, Color { 170, 190, 210, 255 });
    const std::string serverMeta =
        std::string(lobby.privateServer ? "Приватный" : "Открытый")
        + " | команда до " + std::to_string(lobby.maxTeamSize)
        + " | " + (lobby.enforceUniqueHeroesPerTeam ? "герои уникальны" : "повторы героев разрешены");
    DrawText(serverMeta.c_str(), x, 114, 14, Fade(WHITE, 0.50f));

    std::string status = lobby.statusMessage;
    if (lobby.matchStarted)
    {
        status = "match started";
    }
    else if (lobby.matchStarting)
    {
        status = "match starting";
    }
    else if (status.empty())
    {
        status = lobby.canStart ? "can start" : "waiting for players";
    }
    Color statusColor = lobby.canStart ? Color { 140, 235, 150, 255 } : Color { 255, 225, 150, 255 };
    if (status.find("duplicate") != std::string::npos
        || status.find("full") != std::string::npos
        || status.find("denied") != std::string::npos
        || status.find("invalid") != std::string::npos)
    {
        statusColor = Color { 255, 150, 130, 255 };
    }
    if (lobby.matchStarting)
    {
        statusColor = Color { 112, 232, 255, 255 };
    }
    const std::string statusText = LocalizeLobbyStatus(status);
    DrawText(statusText.c_str(), x, 126, 20, statusColor);

    const int hostId = lobby.hostClientId;
    const LobbyPlayerState* host = FindLobbyPlayer(lobby, hostId);
    const std::string hostLine = "Хост: " + std::string(host != nullptr ? host->playerName : "ожидание");
    DrawText(hostLine.c_str(), x + listW + 54, 126, 16, Fade(WHITE, 0.66f));
    DrawText(isHost ? "Вы хост" : "Ожидание хоста",
             x + listW + 54, 148, 15,
             isHost ? Color { 255, 225, 150, 255 } : Fade(WHITE, 0.48f));

    const int listY = 184;
    DrawText(TextFormat("Игроки (%d/%d)", static_cast<int>(lobby.players.size()), lobby.maxPlayers),
             x, listY - 38, 22, RAYWHITE);
    DrawRectangle(x, listY, listW, 34, Fade(Color { 30, 36, 46, 255 }, 0.82f));
    DrawText("Имя", x + 14, listY + 9, 15, Fade(WHITE, 0.68f));
    DrawText("Команда", x + 230, listY + 9, 15, Fade(WHITE, 0.68f));
    DrawText("Герой", x + 350, listY + 9, 15, Fade(WHITE, 0.68f));
    DrawText("Готов", x + listW - 82, listY + 9, 15, Fade(WHITE, 0.68f));

    int rowY = listY + 42;
    for (const LobbyPlayerState& player : lobby.players)
    {
        const bool self = player.clientId == localClientId;
        const bool playerHost = player.clientId == lobby.hostClientId;
        DrawRectangle(x, rowY - 4, listW, 30,
                      self ? Fade(Color { 56, 64, 76, 255 }, 0.82f)
                           : Fade(Color { 14, 17, 24, 255 }, 0.48f));
        std::string name = player.playerName.empty()
            ? "Игрок " + std::to_string(player.clientId)
            : player.playerName;
        if (playerHost)
        {
            name += " (хост)";
        }
        DrawText(name.c_str(), x + 14, rowY + 2, 16, player.connected ? WHITE : Fade(WHITE, 0.42f));
        DrawText(LobbyTeamName(player.selectedTeam), x + 230, rowY + 2, 16, LobbyTeamColor(player.selectedTeam));
        const std::string heroName = LobbyHeroName(player.selectedHero);
        DrawText(heroName.c_str(), x + 350, rowY + 2, 15, Fade(WHITE, 0.78f));
        DrawText(player.ready ? "ГОТОВ" : "...", x + listW - 82, rowY + 2, 15,
                 player.ready ? Color { 140, 235, 150, 255 } : Fade(WHITE, 0.48f));
        rowY += 34;
    }

    const LobbyUiLayout layout = BuildLobbyUiLayout();
    const int controlsX = static_cast<int>(layout.teamButtons[0].x);
    DrawText("Команда", controlsX, 232, 20, RAYWHITE);
    for (int team = 0; team < 4; ++team)
    {
        const bool enabled = !locked && team < std::clamp(lobby.teamCount, 1, 4);
        DrawLobbyButton(layout.teamButtons[team], LobbyTeamName(team), localTeam == team, enabled, LobbyTeamColor(team));
    }

    DrawText("Герой", controlsX, 356, 20, RAYWHITE);
    for (int hero = 0; hero < HeroSystem::kHeroCount; ++hero)
    {
        const bool enabled = !locked && hero < std::clamp(lobby.heroCount, 1, HeroSystem::kHeroCount);
        DrawLobbyButton(layout.heroButtons[hero], LobbyHeroName(hero), localHero == hero, enabled,
                        Color { 112, 232, 255, 255 });
    }

    DrawLobbyButton(layout.readyButton, localReady ? "Готов" : "Не готов", localReady, !locked,
                    Color { 140, 235, 150, 255 });
    if (isHost)
    {
        const bool canStart = !locked && lobby.canStart && localReady;
        DrawLobbyButton(layout.startButton, "Старт", false, canStart, Color { 255, 225, 150, 255 });
    }
    else
    {
        DrawText("Запускает хост", static_cast<int>(layout.startButton.x), static_cast<int>(layout.startButton.y + 11),
                 16, Fade(WHITE, 0.44f));
    }

    EndDrawing();
}

// ---------------------------------------------------------------------------
// Stage 4 (one client loop): the MP client as SESSION STATE.
//
// RunNetworkClient used to own a nested while-loop with its own per-frame
// input/update/render — a second, diverging client game loop. The loop body
// now lives in UpdateNetworkClientSession / RenderNetworkClientSessionOverlay
// and is driven by the ONE standard HandleInput/Update/Render trio (see the
// dispatches at the top of Game::HandleInput/Update/Render). RunNetworkClient
// below is a thin CLI wrapper that starts a session and spins that same trio,
// so `--connect` and the smokes behave exactly as before.
// ---------------------------------------------------------------------------

Game::~Game() = default;

bool Game::NetworkClientSessionActive() const
{
    return clientSessionPhase_ != ClientSessionPhase::Inactive;
}

void Game::FailNetworkClientSession(const char* title, std::string detail,
                                    Color color, double seconds)
{
    if (netClient_ != nullptr)
    {
        netClient_->Disconnect();
    }
    EnableCursor();
    clientSessionFailTitle_ = title;
    clientSessionFailDetail_ = std::move(detail);
    clientSessionFailColor_ = color;
    clientSessionFailUntil_ = GetTime() + seconds;
    clientSessionPhase_ = ClientSessionPhase::FailureMessage;
    clientSessionDraw_ = ClientSessionDraw::Failure;
}

bool Game::StartNetworkClientSession(const std::string& host, std::uint16_t port,
                                     const std::string& password, double maxSeconds,
                                     const LobbyUpdate& lobbyPrefs)
{
    StopNetworkClientSession(); // safety for re-entry; no-op when inactive

    networkMode_ = NetworkMode::LocalClient;
    clientWorldBuilt_ = false;
    networkAssignedPlayerId_ = -1;
    clientPaused_ = false;
    networkClientReturnToMainMenu_ = false;
    clientSessionReconnecting_ = false;
    clientSessionTarget_ = host + ":" + std::to_string(port);
    clientSessionStartTime_ = GetTime();
    clientSessionMaxSeconds_ = maxSeconds;
    clientSessionDraw_ = ClientSessionDraw::None;

    if (!NetworkTransportAvailable())
    {
        std::cout << "connect mode: network transport disabled at build "
                     "(DAIBED_ENABLE_NETWORK=OFF); cannot join "
                  << host << ':' << port << ".\n";
        FailNetworkClientSession("Сеть отключена в этой сборке",
                                 "Пересоберите игру с DAIBED_ENABLE_NETWORK=ON.",
                                 Color { 255, 170, 120, 255 }, 2.0);
        return true;
    }

    // Bounded, blocking connect (handshake retried internally). Graceful on a
    // dead server or a wrong password — the failure phase renders the reason
    // for a moment, then the session ends.
    netClient_ = std::make_unique<ClientTransport>();
    if (!netClient_->Connect(host, port, password, 3.0f))
    {
        const std::string detail = netClient_->WasDenied()
            ? ("отклонено: " + LocalizeNetworkReason(netClient_->DenyReason()))
            : ("не удалось подключиться к " + host + ':' + std::to_string(port) + " - "
               + (netClient_->LastError().empty() ? LocalizeNetworkReason("connect timed out") : netClient_->LastError()));
        std::cout << "connect mode: " << detail << '\n';
        FailNetworkClientSession("Не удалось подключиться", detail, Color { 255, 150, 130, 255 }, 2.5);
        return true;
    }

    // Announce ourselves to the lobby with the caller's preferences (CLI flags
    // or the GUI join form), defaulting the display name when none was given.
    clientSessionLobbyPrefs_ = lobbyPrefs;
    if (clientSessionLobbyPrefs_.playerName.empty())
    {
        clientSessionLobbyPrefs_.playerName = "Игрок " + std::to_string(netClient_->LobbyClientId());
    }
    netClient_->SendLobbyUpdate(clientSessionLobbyPrefs_);
    std::cout << "connect mode: connected to " << host << ':' << port
              << " as lobbyClientId=" << netClient_->LobbyClientId() << ". Rendering...\n";
    clientSessionPhase_ = ClientSessionPhase::Lobby;
    return true;
}

void Game::StopNetworkClientSession()
{
    if (clientSessionPhase_ == ClientSessionPhase::Inactive && netClient_ == nullptr)
    {
        return;
    }
    if (netClient_ != nullptr)
    {
        netClient_->Disconnect();
        std::cout << "connect mode: left (rx=" << netClient_->PacketsReceived()
                  << " tx=" << netClient_->PacketsSent() << ").\n";
        netClient_.reset();
    }
    EnableCursor();
    clientSessionPhase_ = ClientSessionPhase::Inactive;
    clientSessionDraw_ = ClientSessionDraw::None;
    clientSessionReconnecting_ = false;

    // Restore a sane menu state (mirrors the old post-loop block in
    // StartGuiConnect; harmless for the CLI wrapper — the process exits right
    // after). Skipped when the whole window is closing.
    if (!WindowShouldClose() && !ShouldClose())
    {
        SetNetworkMode(NetworkMode::LocalSinglePlayer);
        localPlayerServerDriven_ = false;
        clientWorldBuilt_ = false;
        clientPaused_ = false;
        clientAimInitialized_ = false;
        networkAssignedPlayerId_ = -1;
        remoteSnapshotBuffer_.clear();
        screen_ = networkClientReturnToMainMenu_ ? GameScreen::MainMenu : GameScreen::Multiplayer;
        networkClientReturnToMainMenu_ = false;
        multiplayerStatus_ = "Отключено от " + clientSessionTarget_ + ".";
    }
}

void Game::UpdateNetworkClientSession(float dt)
{
    clientSessionDraw_ = ClientSessionDraw::None;

    if (clientSessionPhase_ == ClientSessionPhase::FailureMessage)
    {
        CrashLogger::Heartbeat("network-client-message");
        if (GetTime() >= clientSessionFailUntil_)
        {
            StopNetworkClientSession();
            return;
        }
        clientSessionDraw_ = ClientSessionDraw::Failure;
        return;
    }

    if (netClient_ == nullptr)
    {
        StopNetworkClientSession();
        return;
    }
    ClientTransport& client = *netClient_;

    CrashLogger::Heartbeat(client.InMatch() ? "network-client-match" : "network-client-lobby");
    if (clientSessionMaxSeconds_ > 0.0
        && GetTime() - clientSessionStartTime_ >= clientSessionMaxSeconds_)
    {
        StopNetworkClientSession();
        return;
    }

    client.Poll();
    networkSnapshotAgeMs_ = client.SnapshotAgeSeconds() * 1000.0f;
    networkPacketLossEstimate_ = client.PacketsReceived() > 0
        ? static_cast<float>(client.DroppedSnapshots()) / static_cast<float>(client.PacketsReceived())
        : 0.0f;
    const float agePressure = client.HasSnapshot()
        ? std::clamp((client.SnapshotAgeSeconds() - 0.18f) * 0.35f, 0.0f, 0.16f)
        : 0.0f;
    const float lossPressure = std::clamp(networkPacketLossEstimate_ * 0.25f, 0.0f, 0.18f);
    networkInterpolationDelaySeconds_ = std::clamp(
        kClientInterpolationDelaySeconds + agePressure + lossPressure,
        kClientInterpolationDelaySeconds,
        0.32f);
    networkInterpolationDelayMs_ = networkInterpolationDelaySeconds_ * 1000.0f;
    networkBytesPerSecond_ = client.BytesPerSecond();
    networkPacketsPerSecond_ = client.PacketsPerSecond();
    networkLastFullSnapshotBytes_ = client.LastFullSnapshotBytes();
    networkLastDeltaSnapshotBytes_ = client.LastDeltaSnapshotBytes();
    networkFullSnapshots_ = client.FullSnapshotsReceived();
    networkDeltaSnapshots_ = client.DeltaSnapshotsReceived();
    networkDroppedSnapshots_ = client.DroppedSnapshots();
    networkIgnoredSnapshots_ = client.IgnoredSnapshots();
    networkResyncRequests_ = client.ResyncRequestsSent();
    unackedCommandCount_ = static_cast<int>(client.PendingCommandCount());
    if (!client.IsConnected())
    {
        if (client.WasDenied())
        {
            FailNetworkClientSession(
                "Отключено от сервера",
                "отклонено: " + LocalizeNetworkReason(client.DenyReason()),
                Color { 255, 200, 120, 255 }, 2.5);
            return;
        }
        clientSessionReconnecting_ = true;
        if (IsKeyPressed(KEY_ESCAPE))
        {
            StopNetworkClientSession();
            return;
        }
        clientSessionReconnectDetail_ = client.TimedOut()
            ? "Ожидание сервера. Esc - выйти из матча."
            : "Восстановление сессии. Esc - выйти из матча.";
        clientSessionDraw_ = ClientSessionDraw::Reconnecting;
        return;
    }
    if (clientSessionReconnecting_)
    {
        LobbyUpdate restore = clientSessionLobbyPrefs_;
        restore.ready = true;
        restore.startRequested = false;
        client.SendLobbyUpdate(restore);
        clientSessionReconnecting_ = false;
        clientPaused_ = false;
        clientAimInitialized_ = false;
    }

    if (client.InMatch())
    {
        clientSessionPhase_ = ClientSessionPhase::Match;
        if (!clientWorldBuilt_)
        {
            networkAssignedPlayerId_ = client.AssignedPlayerId();
            localPlayerId_ = networkAssignedPlayerId_;
            BuildClientWorld(client.LatestLobbySnapshot());
            clientWorldBuilt_ = true;
            clientPaused_ = false;
            clientAimInitialized_ = false;
            screen_ = GameScreen::Playing;
            DisableCursor(); // capture the mouse for FPS-style look
            std::cout << "connect mode: match started as playerId=" << networkAssignedPlayerId_ << ".\n";
        }

        // Active-match Esc is handled by SampleClientInput/HandleNetworkClientUiInput
        // so it can close inventory/shop/chests before pausing, matching SP.
        // Once paused, the network pause menu owns input; the match keeps
        // simulating underneath.
        bool skipClientInputThisFrame = false;
        if (clientPaused_)
        {
            bool requestMainMenu = false;
            if (screen_ == GameScreen::Settings)
            {
                HandleSettingsInput();
            }
            else if (screen_ == GameScreen::Controls)
            {
                HandleControlsInput();
            }
            else
            {
                screen_ = GameScreen::Paused;
                skipClientInputThisFrame = HandleNetworkPauseInput(requestMainMenu);
            }

            if (requestMainMenu)
            {
                networkClientReturnToMainMenu_ = true;
                StopNetworkClientSession();
                return;
            }
        }

        // Fold each authoritative snapshot once (heavy work — gated to new
        // snapshots so high render FPS doesn't redo it every frame).
        if (client.HasSnapshot() && ShouldApplyClientSnapshot(client.LatestSnapshot()))
        {
            const MatchSnapshot& snapshot = client.LatestSnapshot();
            PushRemoteSnapshot(snapshot);
            ApplyAuthoritativeSnapshotForPrediction(snapshot, matchSimulation_.FixedDeltaSeconds());
            ApplyClientSnapshot(snapshot);
        }

        // Sample input + mouse-look every frame (responsive aim), then predict
        // and send at the fixed sim tick rate so the 60 Hz server applies ~one
        // input per tick (full-speed movement, no stale-dropped command spam).
        if (skipClientInputThisFrame)
        {
            currentInput_ = PlayerInput {};
            pendingClientInput_ = PlayerInput {};
        }
        else
        {
            SampleClientInput();
        }
        const float fixedDt = matchSimulation_.FixedDeltaSeconds();
        clientInputAccumulator_ += dt;
        int clientSteps = 0;
        while (clientInputAccumulator_ >= fixedDt && clientSteps < kMaxClientStepsPerFrame)
        {
            StepClientPredictionAndSend(client, fixedDt);
            clientInputAccumulator_ -= fixedDt;
            ++clientSteps;
        }
        const float maxAccumulated = fixedDt * static_cast<float>(kMaxClientStepsPerFrame);
        if (clientInputAccumulator_ > maxAccumulated)
        {
            clientInputAccumulator_ = maxAccumulated;
        }

        // Glide remote players and short-lived replicated visuals every frame.
        UpdateRemoteInterpolation(dt);
        UpdateClientReplicatedDynamics(dt);
        UpdatePredictionStats(dt);
        // Presentation upkeep shared with SP's UpdatePresentation: the ONE
        // camera (UpdateCamera handles the session's spectator transitions and
        // the prediction-smoothing offset), feedback aging, combat preview.
        UpdateCamera(dt);
        UpdateFeedback(dt);
        UpdateCombatPreview();

        const bool waitingForSnapshot = !client.HasSnapshot()
            || client.SnapshotAgeSeconds() > 1.0f;
        clientSessionDraw_ = waitingForSnapshot
            ? ClientSessionDraw::WaitingSync
            : ClientSessionDraw::None; // None = the normal Render body draws the match
    }
    else
    {
        clientSessionPhase_ = ClientSessionPhase::Lobby;
        // Pre-match lobby: ESC leaves before the match begins.
        if (IsKeyPressed(KEY_ESCAPE))
        {
            StopNetworkClientSession();
            return;
        }
        HandleNetworkLobbyControls(client, clientSessionLobbyPrefs_, client.LatestLobbySnapshot(),
                                   input_.IsDevKeyboard());
        clientSessionDraw_ = ClientSessionDraw::Lobby;
    }
}

bool Game::RenderNetworkClientSessionOverlay()
{
    switch (clientSessionDraw_)
    {
    case ClientSessionDraw::Failure:
        DrawClientMessageFrame(clientSessionFailTitle_.c_str(), clientSessionFailDetail_, clientSessionFailColor_);
        return true;
    case ClientSessionDraw::Reconnecting:
        DrawClientMessageFrame("Переподключение", clientSessionReconnectDetail_, Color { 255, 225, 150, 255 });
        return true;
    case ClientSessionDraw::Lobby:
        if (netClient_ != nullptr)
        {
            RenderNetworkLobby(netClient_->LatestLobbySnapshot(), netClient_->LobbyClientId(), clientSessionLobbyPrefs_);
        }
        return true;
    case ClientSessionDraw::WaitingSync:
        DrawClientMessageFrame("Синхронизация", "Получаем свежее базовое состояние от сервера.", Color { 112, 232, 255, 255 });
        return true;
    case ClientSessionDraw::None:
    default:
        return false;
    }
}

int Game::RunNetworkClient(const std::string& host, std::uint16_t port,
                           const std::string& password, double maxSeconds,
                           const LobbyUpdate& lobbyPrefs)
{
    // CLI wrapper: start a session, then spin the SAME standard loop the GUI
    // uses. All client behavior lives in the session methods above.
    StartNetworkClientSession(host, port, password, maxSeconds, lobbyPrefs);
    while (!WindowShouldClose() && !ShouldClose() && NetworkClientSessionActive())
    {
        HandleInput();
        Update(GetFrameTime());
        Render();
    }
    StopNetworkClientSession(); // window closed mid-session; no-op otherwise
    return 0;
}

int Game::RunClientGuiSmoke()
{
    if (!NetworkTransportAvailable())
    {
        std::cout << "client-gui-smoke: network transport disabled at build "
                     "(DAIBED_ENABLE_NETWORK=OFF) — skipped\n";
        std::cout << "CLIENT_GUI_SMOKE_SKIPPED" << std::endl;
        return 0;
    }

    // A second, headless Game drives the authoritative server in this same process
    // (no second window). A Game method may touch another Game's private members.
    Game server;
    server.Initialize(true);

    ServerConfig cfg;
    cfg.listenAddress = "127.0.0.1";
    cfg.port = 0;              // ephemeral
    cfg.minPlayersToStart = 1; // a single client may start the match
    cfg.password.clear();

    ServerTransport transport;
    if (!server.NetworkServerSetup(transport, cfg))
    {
        std::cout << "client-gui-smoke: server setup failed: " << transport.LastError() << '\n';
        std::cout << "CLIENT_GUI_SMOKE_FAIL" << std::endl;
        server.Shutdown();
        return 9;
    }
    const std::uint16_t port = transport.BoundPort();
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    std::cout << "client-gui-smoke: server on 127.0.0.1:" << port << " (headless)\n";

    networkMode_ = NetworkMode::LocalClient;
    clientWorldBuilt_ = false;
    networkAssignedPlayerId_ = -1;

    ClientTransport client;
    client.Open("127.0.0.1", port, "", 3.0f);

    bool sentLobby = false;
    int renderedFrames = 0;
    int snapshotsApplied = 0;
    bool bufferedBeforeWorld = false;
    bool appliedBeforeWorld = false;
    constexpr int kTargetFrames = 30;
    constexpr int kMaxIterations = 1200;
    for (int i = 0; i < kMaxIterations && renderedFrames < kTargetFrames; ++i)
    {
        // Authoritative server: poll, advance, broadcast per-client snapshots.
        server.NetworkServerTick(transport, fixedDt);
        client.Poll();

        if (client.IsConnected() && !sentLobby)
        {
            LobbyUpdate update;
            update.playerName = "GuiSmoke";
            update.selectedTeam = 0;
            update.selectedHero = 0;
            update.ready = true;
            update.startRequested = true;
            client.SendLobbyUpdate(update);
            sentLobby = true;
        }

        if (client.InMatch())
        {
            if (client.HasSnapshot() && !clientWorldBuilt_)
            {
                bufferedBeforeWorld = true;
            }
            if (!clientWorldBuilt_)
            {
                networkAssignedPlayerId_ = client.AssignedPlayerId();
                localPlayerId_ = networkAssignedPlayerId_;
                BuildClientWorld(client.LatestLobbySnapshot());
                clientWorldBuilt_ = true;
            }
            if (client.HasSnapshot())
            {
                if (!clientWorldBuilt_)
                {
                    appliedBeforeWorld = true;
                }
                PushRemoteSnapshot(client.LatestSnapshot());
                ApplyClientSnapshot(client.LatestSnapshot());
                ++snapshotsApplied;
            }
            UpdateCamera(fixedDt);
            Render();
            ++renderedFrames;
        }
        else
        {
            RenderNetworkLobby(client.LatestLobbySnapshot(), client.LobbyClientId(), LobbyUpdate {});
        }
    }

    // Capture results BEFORE teardown (post-Disconnect getters would read false).
    const bool connected = client.IsConnected() && client.InMatch();
    const bool gotSnapshot = client.HasSnapshot() && snapshotsApplied > 0;
    const std::size_t snapPlayers = client.HasSnapshot() ? client.LatestSnapshot().players.size() : 0;
    const std::size_t clientBlocks = world_.GetBlocks().size();
    const std::size_t serverBlocks = server.world_.GetBlocks().size();
    const std::size_t clientPlayers = matchSimulation_.Players().size();
    const std::size_t clientCores = matchSimulation_.Cores().size();
    const std::size_t serverCores = server.matchSimulation_.Cores().size();
    const std::size_t clientGenerators = matchSimulation_.Generators().size();
    const std::size_t serverGenerators = server.matchSimulation_.Generators().size();
    const int assigned = networkAssignedPlayerId_;

    const bool worldBuilt = clientWorldBuilt_ && clientBlocks > 0;
    const bool worldInSync = clientBlocks == serverBlocks && serverBlocks > 0
        && clientCores == serverCores && serverCores > 0
        && clientGenerators == serverGenerators && serverGenerators > 0;
    const bool playersReplicated = clientPlayers == snapPlayers && snapPlayers > 0;
    const bool rendered = renderedFrames >= kTargetFrames;

    client.Disconnect();
    transport.Close();
    server.Shutdown();

    std::cout << "client-gui-smoke: connected=" << (connected ? "yes" : "no")
              << " assignedPlayer=" << assigned
              << " gotSnapshot=" << (gotSnapshot ? "yes" : "no")
              << " snapshotsApplied=" << snapshotsApplied
              << " renderedFrames=" << renderedFrames
              << " bootstrapBuffered=" << (bufferedBeforeWorld ? "yes" : "no")
              << " appliedBeforeWorld=" << (appliedBeforeWorld ? "yes" : "no")
              << " players[client/snapshot]=" << clientPlayers << '/' << snapPlayers
              << " worldBlocks[client/server]=" << clientBlocks << '/' << serverBlocks
              << " cores[client/server]=" << clientCores << '/' << serverCores
              << " generators[client/server]=" << clientGenerators << '/' << serverGenerators << '\n';

    const bool ok = connected && gotSnapshot && worldBuilt && worldInSync
        && playersReplicated && rendered && !appliedBeforeWorld;
    if (!ok)
    {
        std::cout << "client-gui-smoke: checks connected=" << (connected ? "ok" : "FAIL")
                  << " gotSnapshot=" << (gotSnapshot ? "ok" : "FAIL")
                  << " worldBuilt=" << (worldBuilt ? "ok" : "FAIL")
                  << " worldInSync=" << (worldInSync ? "ok" : "FAIL")
                  << " playersReplicated=" << (playersReplicated ? "ok" : "FAIL")
                  << " rendered=" << (rendered ? "ok" : "FAIL") << '\n';
    }

    std::cout << (ok ? "CLIENT_GUI_SMOKE_OK" : "CLIENT_GUI_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunClientInputSmoke()
{
    if (!NetworkTransportAvailable())
    {
        std::cout << "client-input-smoke: network transport disabled at build "
                     "(DAIBED_ENABLE_NETWORK=OFF) — skipped\n";
        std::cout << "CLIENT_INPUT_SMOKE_SKIPPED" << std::endl;
        return 0;
    }

    // A second, headless Game drives the authoritative server in this same process
    // (no second window). A Game method may touch another Game's private members.
    Game server;
    server.Initialize(true);

    ServerConfig cfg;
    cfg.listenAddress = "127.0.0.1";
    cfg.port = 0;              // ephemeral
    cfg.minPlayersToStart = 1; // a single client may start the match
    cfg.password.clear();

    ServerTransport transport;
    if (!server.NetworkServerSetup(transport, cfg))
    {
        std::cout << "client-input-smoke: server setup failed: " << transport.LastError() << '\n';
        std::cout << "CLIENT_INPUT_SMOKE_FAIL" << std::endl;
        server.Shutdown();
        return 9;
    }
    const std::uint16_t port = transport.BoundPort();
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    std::cout << "client-input-smoke: server on 127.0.0.1:" << port << " (headless)\n";

    networkMode_ = NetworkMode::LocalClient;
    clientWorldBuilt_ = false;
    networkAssignedPlayerId_ = -1;

    ClientTransport client;
    client.Open("127.0.0.1", port, "", 3.0f);

    bool sentLobby = false;
    int assignedId = -1;
    int commandsSent = 0;
    int snapshotsApplied = 0;

    // Aim is decided once from the server-side spawn facing plus a fixed offset, so
    // moving forward clears the spawn area while the yaw is provably distinct from
    // the spawn value (proving the command's aimYaw propagated, not a no-op).
    constexpr float kAimOffset = 0.5f;
    float injectedAimYaw = 0.0f;
    bool haveAim = false;
    Vec3 serverStartPos {};
    bool haveServerStart = false;

    Vec3 clientFirstSnapPos {};
    bool haveClientFirst = false;
    Vec3 clientLatestSnapPos {};
    float clientLatestYaw = 0.0f;
    bool haveClientLatest = false;

    constexpr int kMoveTicks = 150;
    constexpr int kMaxIterations = 2000;
    int moveTicks = 0;
    for (int i = 0; i < kMaxIterations && moveTicks < kMoveTicks; ++i)
    {
        // Authoritative server: poll, apply queued client commands, advance, send.
        server.NetworkServerTick(transport, fixedDt);
        client.Poll();

        if (client.IsConnected() && !sentLobby)
        {
            LobbyUpdate update;
            update.playerName = "InputSmoke";
            update.selectedTeam = 0;
            update.selectedHero = 0;
            update.ready = true;
            update.startRequested = true;
            client.SendLobbyUpdate(update);
            sentLobby = true;
        }

        if (client.InMatch())
        {
            assignedId = client.AssignedPlayerId();
            if (!clientWorldBuilt_)
            {
                networkAssignedPlayerId_ = assignedId;
                localPlayerId_ = assignedId;
                BuildClientWorld(client.LatestLobbySnapshot());
                clientWorldBuilt_ = true;
            }
            if (client.HasSnapshot())
            {
                PushRemoteSnapshot(client.LatestSnapshot());
                ApplyClientSnapshot(client.LatestSnapshot());
                ++snapshotsApplied;
            }

            // Decide the aim and capture the server-side baseline BEFORE the first
            // movement command is applied.
            if (!haveAim || !haveServerStart)
            {
                const Player* sp = server.matchSimulation_.GetPlayer(assignedId);
                if (sp != nullptr)
                {
                    if (!haveAim)
                    {
                        injectedAimYaw = sp->GetYaw() + kAimOffset;
                        haveAim = true;
                    }
                    if (!haveServerStart)
                    {
                        serverStartPos = sp->GetPositionVec3();
                        haveServerStart = true;
                    }
                }
            }

            // Inject the real client send path: moveForward + fixed aimYaw, for the
            // assigned player, shipped over the transport (no keyboard in headless).
            // Stamp a MONOTONIC command tick (as the real client now does) so the
            // server applies every command instead of dropping snapshot-derived
            // duplicate ticks as stale — full-speed movement, not ~1/3 speed.
            PlayerCommand command;
            command.controlledPlayerId = static_cast<std::uint32_t>(assignedId);
            command.tick = ++networkCommandTick_;
            command.aimYaw = injectedAimYaw;
            command.moveForward = 1.0f;
            client.SendCommand(command);
            ++commandsSent;

            // Track the assigned player's replicated position/yaw in the accepted
            // snapshot (this is what a real GUI client would render).
            if (client.HasSnapshot())
            {
                for (const PlayerSnapshot& entry : client.LatestSnapshot().players)
                {
                    if (entry.playerId == assignedId)
                    {
                        if (!haveClientFirst)
                        {
                            clientFirstSnapPos = entry.position;
                            haveClientFirst = true;
                        }
                        clientLatestSnapPos = entry.position;
                        clientLatestYaw = entry.yaw;
                        haveClientLatest = true;
                        break;
                    }
                }
            }
            ++moveTicks;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Server-side authoritative outcome (no RTT): the assigned player moved and its
    // yaw equals the command's aimYaw — proof the server accepted+applied commands.
    Vec3 serverEndPos {};
    float serverEndYaw = 0.0f;
    const Player* serverPlayer = server.matchSimulation_.GetPlayer(assignedId);
    if (serverPlayer != nullptr)
    {
        serverEndPos = serverPlayer->GetPositionVec3();
        serverEndYaw = serverPlayer->GetYaw();
    }
    const float serverMovedDistance = haveServerStart ? (serverEndPos - serverStartPos).Length() : 0.0f;
    const float clientMovedDistance = (haveClientFirst && haveClientLatest)
        ? (clientLatestSnapPos - clientFirstSnapPos).Length() : 0.0f;

    // Bad-path: a --connect to a dead server (port 1) must fail gracefully (no crash).
    ClientTransport deadClient;
    const bool deadConnected = deadClient.Connect("127.0.0.1", 1, "", 0.4f);
    deadClient.Disconnect();

    // Capture results BEFORE teardown (post-Disconnect getters would read false).
    const bool inMatch = client.InMatch();
    const std::uint32_t packetsRx = transport.PacketsReceived();
    const std::uint32_t staleDropped = transport.StaleCommandsDropped();

    client.Disconnect();
    transport.Close();
    server.Shutdown();

    const bool joined = inMatch && assignedId >= 0 && commandsSent > 0;
    const bool serverAcceptedCommands = serverMovedDistance > 0.5f
        && std::fabs(serverEndYaw - injectedAimYaw) < 0.001f && packetsRx > 0;
    const bool clientSawMovement = clientMovedDistance > 0.5f;
    const bool clientYawMatches = haveClientLatest
        && std::fabs(clientLatestYaw - injectedAimYaw) < 0.01f;
    const bool badPathGraceful = !deadConnected; // returned false, no crash

    std::cout << "client-input-smoke: assignedPlayer=" << assignedId
              << " commandsSent=" << commandsSent
              << " snapshotsApplied=" << snapshotsApplied
              << " packetsRx=" << packetsRx
              << " injectedAimYaw=" << injectedAimYaw << '\n';
    std::cout << "client-input-smoke: serverMoved=" << serverMovedDistance
              << " serverYaw=" << serverEndYaw
              << " clientMoved=" << clientMovedDistance
              << " clientYaw=" << clientLatestYaw
              << " staleDropped=" << staleDropped
              << " deadServerConnected=" << (deadConnected ? "yes" : "no") << '\n';

    const bool ok = joined && serverAcceptedCommands && clientSawMovement
        && clientYawMatches && badPathGraceful;
    if (!ok)
    {
        std::cout << "client-input-smoke: checks joined=" << (joined ? "ok" : "FAIL")
                  << " serverAccepted=" << (serverAcceptedCommands ? "ok" : "FAIL")
                  << " clientMoved=" << (clientSawMovement ? "ok" : "FAIL")
                  << " clientYaw=" << (clientYawMatches ? "ok" : "FAIL")
                  << " badPath=" << (badPathGraceful ? "ok" : "FAIL") << '\n';
    }

    std::cout << (ok ? "CLIENT_INPUT_SMOKE_OK" : "CLIENT_INPUT_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunNetworkRangedSmoke()
{
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();

    if (matchSimulation_.Players().empty())
    {
        std::cout << "network-ranged-smoke: no players\n"
                     "NETWORK_RANGED_SMOKE_FAIL" << std::endl;
        return 9;
    }

    Player& controlled = matchSimulation_.Players().front();
    const int controlledId = controlled.GetId();
    MarkNetworkControlledPlayer(controlledId);
    controlled.SetPosition(Vec3 { 0.0f, 38.0f, 0.0f });

    const auto normalized = [](Vector3 value)
    {
        const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
        return length > 0.0001f
            ? Vector3 { value.x / length, value.y / length, value.z / length }
            : Vector3 {};
    };
    const auto dot = [](Vector3 a, Vector3 b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    const auto directionMatches = [&normalized, &dot](const EnergyProjectile& projectile, Vector3 expected, float threshold)
    {
        return dot(normalized(projectile.velocity), normalized(expected)) >= threshold;
    };

    const float fixedDt = matchSimulation_.FixedDeltaSeconds();

    // Fireball quick-use: previously this used the server camera through
    // LaunchProjectile(); it must now use PlayerCommand aim.
    ItemStack fireball;
    fireball.type = ItemType::Fireball;
    fireball.count = 1;
    controlled.GetInventory().SwapSlot(0, fireball);
    PlayerCommand fireballCommand;
    fireballCommand.controlledPlayerId = static_cast<std::uint32_t>(controlledId);
    fireballCommand.selectedSlot = 0;
    fireballCommand.aimYaw = PI / 2.0f; // +X
    fireballCommand.aimPitch = 0.0f;
    fireballCommand.useFireball = true;
    ApplyPlayerCommand(controlled, fireballCommand, 0.0f);
    const std::size_t projectilesBeforeFireball = projectiles_.size();
    suppressLocalFeedback_ = false;
    const std::string messageBeforeFireball = message_;
    const std::size_t eventMessagesBeforeFireball = eventMessages_.size();
    const std::size_t worldEffectsBeforeFireball = worldEffects_.size();
    const std::size_t floatingTextsBeforeFireball = floatingTexts_.size();
    const bool audioMutedBeforeFireball = audio_.IsMuted();
    UseUtilityInputs(controlled, fireballCommand);
    const bool fireballSpawned = projectiles_.size() == projectilesBeforeFireball + 1
        && projectiles_.back().kind == ProjectileKind::Fireball;
    const bool fireballAimOk = fireballSpawned
        && directionMatches(projectiles_.back(), AimDirectionFromCommand(fireballCommand), 0.999f);
    const bool fireballPresentationClean =
        message_ == messageBeforeFireball
        && eventMessages_.size() == eventMessagesBeforeFireball
        && worldEffects_.size() == worldEffectsBeforeFireball
        && floatingTexts_.size() == floatingTextsBeforeFireball
        && audio_.IsMuted() == audioMutedBeforeFireball;
    const MatchSnapshot fireballOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool fireballResultReplicated = std::any_of(
        fireballOwnerView.actionResults.begin(),
        fireballOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::UtilityUse)
                && result.subjectType == static_cast<int>(UtilityType::Fireball)
                && result.success;
        });

    projectiles_.clear();
    recentActionResults_.clear();
    controlled.ResetAttackCooldown(0.0f);

    // Bow draw/release: charge state lives on the player; release consumes an
    // arrow and spawns an arrow along command aim.
    ItemStack bow;
    bow.type = ItemType::Bow;
    bow.count = 1;
    ItemStack arrows;
    arrows.type = ItemType::EnergyArrow;
    arrows.count = 4;
    controlled.GetInventory().SwapSlot(0, bow);
    controlled.GetInventory().SwapSlot(1, arrows);
    controlled.SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;

    PlayerCommand bowCommand;
    bowCommand.controlledPlayerId = static_cast<std::uint32_t>(controlledId);
    bowCommand.selectedSlot = 0;
    bowCommand.aimYaw = -PI / 2.0f; // -X
    bowCommand.aimPitch = 0.08f;
    bowCommand.attackHeld = true;
    ApplyPlayerCommand(controlled, bowCommand, 0.0f);
    for (int i = 0; i < 72; ++i)
    {
        ApplyNetworkPlayerActions(controlled, bowCommand, fixedDt);
    }
    PlayerCommand bowRelease = bowCommand;
    bowRelease.attackHeld = false;
    bowRelease.attackReleased = true;
    ApplyNetworkPlayerActions(controlled, bowRelease, fixedDt);
    const bool bowSpawned = projectiles_.size() == 1
        && projectiles_.back().kind == ProjectileKind::Arrow;
    const bool bowAimOk = bowSpawned
        && directionMatches(projectiles_.back(), AimDirectionFromCommand(bowCommand), 0.999f);
    // Stable spawn id (not the vector index — see EnergyProjectile::id): the
    // real LaunchBowShot path must assign a real id, not the struct default -1.
    const int bowProjectileId = bowSpawned ? projectiles_.back().id : -1;
    const bool bowHasStableId = bowProjectileId > 0;
    const MatchSnapshot bowOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool bowResultReplicated = std::any_of(
        bowOwnerView.actionResults.begin(),
        bowOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::ProjectileLaunch)
                && result.subjectType == static_cast<int>(ProjectileKind::Arrow)
                && result.success;
        });

    projectiles_.clear();
    recentActionResults_.clear();

    // Blaster charge/fire: after loading, attackPressed fires along command aim.
    ItemStack blaster;
    blaster.type = ItemType::Blaster;
    blaster.count = 1;
    controlled.GetInventory().SwapSlot(0, blaster);
    controlled.SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;
    controlled.CancelBlasterLoading();

    PlayerCommand blasterCommand;
    blasterCommand.controlledPlayerId = static_cast<std::uint32_t>(controlledId);
    blasterCommand.selectedSlot = 0;
    blasterCommand.aimYaw = 0.0f; // -Z
    blasterCommand.aimPitch = -0.04f;
    blasterCommand.attackHeld = true;
    ApplyPlayerCommand(controlled, blasterCommand, 0.0f);
    for (int i = 0; i < 90; ++i)
    {
        ApplyNetworkPlayerActions(controlled, blasterCommand, fixedDt);
    }
    PlayerCommand blasterFire = blasterCommand;
    blasterFire.attackHeld = false;
    blasterFire.attackPressed = true;
    blasterFire.placeHeld = true;
    ApplyNetworkPlayerActions(controlled, blasterFire, fixedDt);
    const bool blasterSpawned = projectiles_.size() == 1
        && projectiles_.back().kind == ProjectileKind::Blaster;
    const bool blasterAimOk = blasterSpawned
        && directionMatches(projectiles_.back(), AimDirectionFromCommand(blasterCommand), 0.995f);
    // Stable spawn id, and distinct from the bow's (proves ids are a genuine
    // per-spawn counter, not e.g. always resetting to the same value).
    const bool blasterHasStableId = blasterSpawned
        && projectiles_.back().id > 0
        && projectiles_.back().id != bowProjectileId;
    const MatchSnapshot blasterOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool blasterResultReplicated = std::any_of(
        blasterOwnerView.actionResults.begin(),
        blasterOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::ProjectileLaunch)
                && result.subjectType == static_cast<int>(ProjectileKind::Blaster)
                && result.success;
        });

    networkControlledPlayerIds_.clear();
    networkActionState_.clear();
    projectiles_.clear();

    // Stable spawn ids across expiry: the bug this fix closes is an EARLIER
    // projectile expiring shifting a SURVIVING projectile's id (which happened
    // when id was the vector index — see NetworkSnapshot.h). Spawn two, expire
    // the first via UpdateProjectiles, and assert the survivor's id is
    // unchanged (not reassigned to what used to be the expired one's slot).
    EnergyProjectile expiringProjectile {};
    expiringProjectile.id = NextProjectileId();
    expiringProjectile.kind = ProjectileKind::Arrow;
    expiringProjectile.lifetime = 0.001f;
    projectiles_.push_back(expiringProjectile);
    EnergyProjectile survivorProjectile {};
    survivorProjectile.id = NextProjectileId();
    survivorProjectile.kind = ProjectileKind::Arrow;
    survivorProjectile.lifetime = 5.0f;
    survivorProjectile.position = Vector3 { 0.0f, 60.0f, 0.0f };
    survivorProjectile.previousPosition = survivorProjectile.position;
    survivorProjectile.velocity = Vector3 { 0.0f, 0.0f, 0.0f };
    projectiles_.push_back(survivorProjectile);
    const int survivorIdBeforeExpiry = survivorProjectile.id;
    UpdateProjectiles(matchSimulation_.FixedDeltaSeconds());
    const bool stableIdSurvivedExpiry = projectiles_.size() == 1
        && projectiles_.front().id == survivorIdBeforeExpiry;
    projectiles_.clear();

    // Same proof for explosives and hazard zones (the other two entity types
    // fixed in the same pass — see NetworkSnapshot.h). Positions far from the
    // arena and any player so DetonateAt's side effects (block break, nearby
    // damage) can't interfere with this or later checks.
    TimedExplosion expiringExplosive {};
    expiringExplosive.block = GridPos { 200, 60, 200 };
    expiringExplosive.timer = 0.001f;
    expiringExplosive.id = NextExplosiveId();
    timedExplosions_.push_back(expiringExplosive);
    TimedExplosion survivorExplosive {};
    survivorExplosive.block = GridPos { 205, 60, 200 };
    survivorExplosive.timer = 5.0f;
    survivorExplosive.id = NextExplosiveId();
    timedExplosions_.push_back(survivorExplosive);
    const int survivorExplosiveIdBeforeExpiry = survivorExplosive.id;
    UpdateExplosives(matchSimulation_.FixedDeltaSeconds());
    const bool explosiveIdSurvivedExpiry = timedExplosions_.size() == 1
        && timedExplosions_.front().id == survivorExplosiveIdBeforeExpiry;
    timedExplosions_.clear();

    HazardZone expiringHazard {};
    expiringHazard.position = Vector3 { 200.0f, 60.0f, 200.0f };
    expiringHazard.lifetime = 0.001f;
    expiringHazard.id = NextHazardZoneId();
    hazardZones_.push_back(expiringHazard);
    HazardZone survivorHazard {};
    survivorHazard.position = Vector3 { 205.0f, 60.0f, 200.0f };
    survivorHazard.lifetime = 5.0f;
    survivorHazard.id = NextHazardZoneId();
    hazardZones_.push_back(survivorHazard);
    const int survivorHazardIdBeforeExpiry = survivorHazard.id;
    UpdateHazardZones(matchSimulation_.FixedDeltaSeconds());
    const bool hazardIdSurvivedExpiry = hazardZones_.size() == 1
        && hazardZones_.front().id == survivorHazardIdBeforeExpiry;
    hazardZones_.clear();

    const bool ok = fireballSpawned && fireballAimOk
        && fireballPresentationClean && fireballResultReplicated
        && bowSpawned && bowAimOk && bowResultReplicated && bowHasStableId
        && blasterSpawned && blasterAimOk && blasterResultReplicated && blasterHasStableId
        && stableIdSurvivedExpiry && explosiveIdSurvivedExpiry && hazardIdSurvivedExpiry;
    std::cout << "network-ranged-smoke: fireball spawned="
              << (fireballSpawned ? "yes" : "no")
              << " aim=" << (fireballAimOk ? "ok" : "FAIL")
              << " presentation=" << (fireballPresentationClean ? "ok" : "FAIL")
              << " result=" << (fireballResultReplicated ? "ok" : "FAIL")
              << " bow spawned=" << (bowSpawned ? "yes" : "no")
              << " aim=" << (bowAimOk ? "ok" : "FAIL")
              << " result=" << (bowResultReplicated ? "ok" : "FAIL")
              << " stableId=" << (bowHasStableId ? "ok" : "FAIL")
              << " blaster spawned=" << (blasterSpawned ? "yes" : "no")
              << " aim=" << (blasterAimOk ? "ok" : "FAIL")
              << " result=" << (blasterResultReplicated ? "ok" : "FAIL")
              << " stableId=" << (blasterHasStableId ? "ok" : "FAIL")
              << " survivesExpiry=" << (stableIdSurvivedExpiry ? "ok" : "FAIL")
              << " explosiveSurvivesExpiry=" << (explosiveIdSurvivedExpiry ? "ok" : "FAIL")
              << " hazardSurvivesExpiry=" << (hazardIdSurvivedExpiry ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "NETWORK_RANGED_SMOKE_OK" : "NETWORK_RANGED_SMOKE_FAIL")
              << std::endl;
    return ok ? 0 : 9;
}

int Game::RunNetworkActionsSmoke()
{
    // B1 headless integration test: a network-controlled player's attack / break /
    // place must mutate authoritative server state via ApplyNetworkPlayerActions.
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();

    if (matchSimulation_.Players().size() < 2)
    {
        std::cout << "network-actions-smoke: not enough players\n"
                     "NETWORK_ACTIONS_SMOKE_FAIL" << std::endl;
        return 9;
    }

    // Use a NON-LOCAL player as the network-controlled subject, mirroring a real
    // dedicated server (a client claims a bot slot; the host's own local player is
    // retired). This matters for held-item lookup: GetSelectedHotbarStack reads
    // the global UI slot (selectedHotbarSlot_) for a LOCAL player but the
    // per-player slot for a network player — reusing the local player here would
    // read slot 0 regardless of SetSelectedSlot(), so melee would never see the
    // sword and would silently no-op.
    Player* controlledPtr = nullptr;
    for (Player& candidate : matchSimulation_.Players())
    {
        if (IsBotControlled(ControlKindForPlayer(candidate)))
        {
            controlledPtr = &candidate;
            break;
        }
    }
    if (controlledPtr == nullptr)
    {
        std::cout << "network-actions-smoke: no non-local player\n"
                     "NETWORK_ACTIONS_SMOKE_FAIL" << std::endl;
        return 9;
    }
    Player& controlled = *controlledPtr;
    const int controlledId = controlled.GetId();
    MarkNetworkControlledPlayer(controlledId);

    Player* enemy = nullptr;
    for (Player& candidate : matchSimulation_.Players())
    {
        if (candidate.GetTeamId() != controlled.GetTeamId())
        {
            enemy = &candidate;
            break;
        }
    }
    if (enemy == nullptr)
    {
        std::cout << "network-actions-smoke: no enemy player\n"
                     "NETWORK_ACTIONS_SMOKE_FAIL" << std::endl;
        return 9;
    }

    // A clear patch of air well above the arena so existing geometry can't
    // interfere with the raycasts.
    controlled.SetPosition(Vec3 { 0.0f, 40.0f, 0.0f });
    controlled.SetHeroId(HeroId::Likho);
    const float aimYaw = PI / 2.0f; // Forward() = (+1, 0, 0)
    controlled.SetYaw(aimYaw);

    PlayerCommand base;
    base.controlledPlayerId = static_cast<std::uint32_t>(controlledId);
    base.aimYaw = aimYaw;
    base.aimPitch = 0.0f;
    const Vector3 forward = AimDirectionFromCommand(base);
    const Vector3 eye { 0.0f, 40.0f + 0.78f, 0.0f };

    const Team* controlledTeamForChest = FindTeam(controlled.GetTeamId());
    const Block* controlledChestBlock = controlledTeamForChest != nullptr
        ? world_.GetBlock(controlledTeamForChest->teamChestBlock)
        : nullptr;
    const bool bromChestBlockWorked = controlledTeamForChest != nullptr
        && controlledChestBlock != nullptr
        && controlledChestBlock->type == BlockType::TeamChestBlock
        && controlledChestBlock->teamId == controlled.GetTeamId();
    bool bromChestDeliveryWorked = false;
    if (bromChestBlockWorked)
    {
        const int teamId = controlled.GetTeamId();
        const int ironBeforeChestDelivery = teamChests_[teamId].GetResource(ResourceType::Iron);
        BromVacuumBot deliveryBot {};
        deliveryBot.ownerPlayerId = controlledId;
        deliveryBot.ownerTeamId = teamId;
        deliveryBot.position = TeamChestDepositPosition(*controlledTeamForChest);
        deliveryBot.lastPosition = deliveryBot.position;
        deliveryBot.returning = true;
        deliveryBot.health = 40;
        deliveryBot.pulseTimer = 1.0f;
        deliveryBot.cargo[0] = 3;
        bromVacuumBots_.push_back(deliveryBot);
        {
            ScopedLocalFeedbackSuppression suppressDeliveryFeedback(*this, true);
            UpdateBromDevices(matchSimulation_.FixedDeltaSeconds());
        }
        bromChestDeliveryWorked =
            teamChests_[teamId].GetResource(ResourceType::Iron) == ironBeforeChestDelivery + 3;
        bromVacuumBots_.clear();
    }

    // Phase 2 bridge: authoritative remote-human actions must mutate gameplay
    // without writing host-local presentation buffers. Force feedback on in this
    // headless smoke so the scoped server suppression is what keeps these stable.
    suppressLocalFeedback_ = false;
    const std::string messageBefore = message_;
    const std::size_t eventMessagesBefore = eventMessages_.size();
    const std::size_t worldEffectsBefore = worldEffects_.size();
    const std::size_t floatingTextsBefore = floatingTexts_.size();
    const bool audioMutedBefore = audio_.IsMuted();
    const int localPlayerIdBefore = localPlayerId_;
    localPlayerId_ = controlledId;
    const MatchStats statsBefore = stats_;
    const float hitMarkerBefore = hitMarkerTimer_;
    const float damageFlashBefore = damageFlashTimer_;
    const float fovKickBefore = fovKick_;

    // --- Hero abilities: remote actions mutate authoritative state only. ------
    controlled.SetHeroId(HeroId::Radon);
    enemy->SetPosition(Vec3 { eye.x + forward.x * 3.0f, 40.0f, eye.z + forward.z * 3.0f });
    enemy->UpdateTimers(2.0f);
    const int radonPulseHpBefore = enemy->GetHealth();
    PlayerCommand radonPulseCmd = base;
    radonPulseCmd.useAbility1 = true;
    recentActionResults_.clear();
    const bool radonPulseApplied = ApplyPlayerActionCommand(controlled, radonPulseCmd);
    const bool radonPulseWorked = radonPulseApplied && enemy->GetHealth() < radonPulseHpBefore;
    const MatchSnapshot radonPulseOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool radonPulseResultReplicated = std::any_of(
        radonPulseOwnerView.actionResults.begin(),
        radonPulseOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            // Radon's Active1 always pushes a directed Pull/Cone world effect
            // (radius 5.2/5.6) into HeroAbilityActionResult::worldEffects — so a
            // successful cast must replicate a real radius + the world-effect
            // and directed flags (PushHeroAbilityActionResultSnapshot's primary-
            // effect extraction), not just message+sound.
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::HeroAbility)
                && result.subjectType == static_cast<int>(HeroId::Radon)
                && result.amount == static_cast<int>(HeroAbilitySlot::Active1)
                && result.success
                && result.radius > 0.0f
                && (result.flags & kHeroAbilityFlagWorldEffect) != 0
                && (result.flags & kHeroAbilityFlagDirectedEffect) != 0;
        });

    // Immediately re-cast on the same command while the ability is on cooldown:
    // the server denies it, and a real network owner must learn "denied" the
    // same replicated way it learns "success" — not by inference from silence.
    recentActionResults_.clear();
    const bool radonPulseDeniedApplied = ApplyPlayerActionCommand(controlled, radonPulseCmd);
    const MatchSnapshot radonPulseDeniedView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool radonPulseDeniedReplicated = std::any_of(
        radonPulseDeniedView.actionResults.begin(),
        radonPulseDeniedView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::HeroAbility)
                && result.subjectType == static_cast<int>(HeroId::Radon)
                && result.amount == static_cast<int>(HeroAbilitySlot::Active1)
                && !result.success
                && !result.message.empty();
        });
    const bool heroAbilityResultsReplicated = radonPulseResultReplicated
        && !radonPulseDeniedApplied && radonPulseDeniedReplicated;

    const std::size_t radonProjectilesBefore = projectiles_.size();
    PlayerCommand radonMolotovCmd = base;
    radonMolotovCmd.useAbility2 = true;
    const bool radonMolotovApplied = ApplyPlayerActionCommand(controlled, radonMolotovCmd);
    const bool radonMolotovWorked = radonMolotovApplied && projectiles_.size() == radonProjectilesBefore + 1;

    controlled.SetHeroUltimateCharge(100.0f);
    const bool radonPrimedBefore = controlled.GetHeroState().ultimatePrimed;
    PlayerCommand radonPrimeCmd = base;
    radonPrimeCmd.useUltimate = true;
    const bool radonPrimeApplied = ApplyPlayerActionCommand(controlled, radonPrimeCmd);
    const bool radonPrimeWorked = radonPrimeApplied
        && controlled.GetHeroState().ultimatePrimed != radonPrimedBefore;

    EnergyCore* controlledCore = FindCoreByTeam(controlled.GetTeamId());
    if (controlledCore != nullptr)
    {
        controlledCore->SetHealth(0);
    }
    if (Team* controlledTeam = FindTeam(controlled.GetTeamId()))
    {
        controlledTeam->coreAlive = false;
    }
    controlled.SetHeroUltimateCharge(100.0f);
    enemy->SetPosition(Vec3 { eye.x + forward.x * 2.8f, 40.0f, eye.z + forward.z * 2.8f });
    enemy->UpdateTimers(2.0f);
    const int radonWaveHpBefore = enemy->GetHealth();
    const float radonUltimateCooldownBefore = controlled.GetHeroState().ultimate.cooldownRemaining;
    PlayerCommand radonWaveCmd = base;
    radonWaveCmd.useUltimate = true;
    const bool radonWaveApplied = ApplyPlayerActionCommand(controlled, radonWaveCmd);
    const bool radonWaveWorked = radonWaveApplied
        && controlled.GetHeroState().ultimate.cooldownRemaining > radonUltimateCooldownBefore
        && enemy->GetHealth() < radonWaveHpBefore;

    controlled.SetHeroId(HeroId::Likho);
    const float ability1CooldownBefore = controlled.GetHeroState().active1.cooldownRemaining;
    PlayerCommand hero1Cmd = base;
    hero1Cmd.useAbility1 = true;
    const bool hero1Applied = ApplyPlayerActionCommand(controlled, hero1Cmd);
    const float ability1CooldownAfter = controlled.GetHeroState().active1.cooldownRemaining;
    const bool hero1Worked = hero1Applied
        && ability1CooldownBefore <= 0.0f
        && ability1CooldownAfter > ability1CooldownBefore;

    const bool ability2ActiveBefore = controlled.GetHeroState().active2.active;
    PlayerCommand hero2Cmd = base;
    hero2Cmd.useAbility2 = true;
    const bool hero2Applied = ApplyPlayerActionCommand(controlled, hero2Cmd);
    const bool ability2ActiveAfter = controlled.GetHeroState().active2.active;
    const bool hero2Worked = hero2Applied && !ability2ActiveBefore && ability2ActiveAfter;

    // Same pitch-loss bug as Svidetel's Active2 below, for Likho's ultimate
    // (disguise target selection): position the enemy above eye level so a
    // flat aim falls outside the targeting cone but the command's real 3D aim
    // (from aimPitch) doesn't — regression case for the fix.
    constexpr float kLikhoUltimateRise = 1.5f;
    constexpr float kLikhoUltimateRun = 3.0f;
    // HeroRuntimeState's ability slots (and ultimateReady/ultimateCharge) are
    // hero-agnostic (not keyed by hero id), so Radon's ultimate cast earlier
    // (radonWaveCmd, 40s cooldown, and it consumes ultimateReady) is still
    // sitting on the SAME slot after SetHeroId(Likho) above — reset both or
    // Likho's IsHeroAbilityReady(Ultimate) denies this cast outright.
    controlled.MutableHeroState().ultimate.cooldownRemaining = 0.0f;
    controlled.SetHeroUltimateCharge(100.0f);
    enemy->SetPosition(Vec3 {
        eye.x + forward.x * kLikhoUltimateRun,
        controlled.GetPosition().y + 0.72f + kLikhoUltimateRise,
        eye.z + forward.z * kLikhoUltimateRun });
    enemy->UpdateTimers(2.0f);
    PlayerCommand likhoUltimateCmd = base;
    likhoUltimateCmd.useUltimate = true;
    likhoUltimateCmd.aimPitch = std::atan2(kLikhoUltimateRise, kLikhoUltimateRun);
    const bool likhoUltimateApplied = ApplyPlayerActionCommand(controlled, likhoUltimateCmd);
    const bool likhoUltimateWorked = likhoUltimateApplied
        && controlled.GetHeroState().likhoDisguiseTeamId == enemy->GetTeamId();

    controlled.SetHeroId(HeroId::Svidetel);
    const std::size_t echoesBefore = svidetelEchoes_.size();
    PlayerCommand svidetelEchoCmd = base;
    svidetelEchoCmd.useAbility1 = true;
    const bool svidetelEchoApplied = ApplyPlayerActionCommand(controlled, svidetelEchoCmd);
    const bool svidetelEchoWorked = svidetelEchoApplied && svidetelEchoes_.size() == echoesBefore + 1;

    // Elevated on purpose (not flat, unlike `base.aimPitch = 0.0f`): this is the
    // regression case for a real bug — ApplyHeroAbilityAction used to fall back
    // to player.Forward() (yaw-only, always flat) for a network player's aim
    // here, so a remote Svidetel could never phase a block above eye level no
    // matter what aimPitch the command carried. A flat-aim test can't catch
    // that (player.Forward() and a pitch=0 command agree), so this one aims up.
    constexpr float kPhaseRise = 1.5f;
    constexpr float kPhaseRun = 3.0f;
    const float phaseOriginY = controlled.GetPosition().y + 0.72f;
    const GridPos phaseCell = world_.WorldToGrid(Vector3 {
        eye.x + forward.x * kPhaseRun, phaseOriginY + kPhaseRise, eye.z + forward.z * kPhaseRun });
    world_.PlaceBlock(phaseCell, Block { BlockType::StoneBlock, -1, true }, true);
    const std::size_t phaseBlocksBefore = svidetelPhaseBlocks_.size();
    PlayerCommand svidetelPhaseCmd = base;
    svidetelPhaseCmd.useAbility2 = true;
    svidetelPhaseCmd.aimPitch = std::atan2(kPhaseRise, kPhaseRun);
    const bool svidetelPhaseApplied = ApplyPlayerActionCommand(controlled, svidetelPhaseCmd);
    const bool svidetelPhaseWorked = svidetelPhaseApplied
        && svidetelPhaseBlocks_.size() > phaseBlocksBefore
        && world_.GetBlock(phaseCell) == nullptr;

    controlled.SetHeroUltimateCharge(100.0f);
    const float svidetelUltimateCooldownBefore = controlled.GetHeroState().ultimate.cooldownRemaining;
    PlayerCommand svidetelUltimateCmd = base;
    svidetelUltimateCmd.useUltimate = true;
    const bool svidetelUltimateApplied = ApplyPlayerActionCommand(controlled, svidetelUltimateCmd);
    const float svidetelUltimateCooldownAfter = controlled.GetHeroState().ultimate.cooldownRemaining;
    const bool svidetelUltimateWorked = svidetelUltimateApplied
        && svidetelUltimateCooldownBefore <= 0.0f
        && svidetelUltimateCooldownAfter > svidetelUltimateCooldownBefore;

    controlled.SetHeroId(HeroId::Orbita);
    PlayerCommand orbitaDashCmd = base;
    orbitaDashCmd.useAbility1 = true;
    const bool orbitaDashApplied = ApplyPlayerActionCommand(controlled, orbitaDashCmd);
    const HeroRuntimeState& orbitaDashState = controlled.GetHeroState();
    const bool orbitaDashWorked = orbitaDashApplied
        && orbitaDashState.orbitaMomentumStrike
        && orbitaDashState.orbitaDashRemaining > 0.0f
        && orbitaDashState.active1.cooldownRemaining > 0.0f;

    const std::size_t orbitaTempBlocksBefore = heroTemporaryBlocks_.size();
    PlayerCommand orbitaBlocksCmd = base;
    orbitaBlocksCmd.useAbility2 = true;
    const bool orbitaBlocksApplied = ApplyPlayerActionCommand(controlled, orbitaBlocksCmd);
    const bool orbitaBlocksWorked = orbitaBlocksApplied
        && heroTemporaryBlocks_.size() > orbitaTempBlocksBefore
        && controlled.GetHeroState().active2.cooldownRemaining > 0.0f;

    controlled.SetHeroUltimateCharge(100.0f);
    PlayerCommand orbitaPrimeCmd = base;
    orbitaPrimeCmd.useUltimate = true;
    const bool orbitaPrimeApplied = ApplyPlayerActionCommand(controlled, orbitaPrimeCmd);
    const bool orbitaPrimeWorked = orbitaPrimeApplied
        && controlled.GetHeroState().orbitaTeleportPrimed
        && controlled.GetHeroState().orbitaTeleportPreviewTimer > 0.0f;

    const Vec3 orbitaPositionBeforeTeleport = controlled.GetPositionVec3();
    PlayerCommand orbitaTeleportCmd = base;
    orbitaTeleportCmd.useUltimate = true;
    const bool orbitaTeleportApplied = ApplyPlayerActionCommand(controlled, orbitaTeleportCmd);
    const float orbitaTeleportMove = (controlled.GetPositionVec3() - orbitaPositionBeforeTeleport).Length();
    const bool orbitaTeleportPrimedAfter = controlled.GetHeroState().orbitaTeleportPrimed;
    const float orbitaTeleportCooldownAfter = controlled.GetHeroState().ultimate.cooldownRemaining;
    const bool orbitaTeleportUltimateReadyAfter = controlled.GetHeroState().ultimateReady;
    const bool orbitaTeleportWorked = orbitaTeleportApplied
        && !orbitaTeleportPrimedAfter
        && !orbitaTeleportUltimateReadyAfter
        && orbitaTeleportMove > 1.0f;
    controlled.SetPosition(Vec3 { eye.x, 40.0f, eye.z });
    controlled.SetYaw(aimYaw);

    controlled.SetHeroId(HeroId::Konvoy);
    const Vector3 trapDesired {
        controlled.GetPosition().x + forward.x * 1.15f,
        controlled.GetPosition().y,
        controlled.GetPosition().z + forward.z * 1.15f
    };
    const GridPos trapColumn = world_.WorldToGrid(trapDesired);
    const int playerGridY = world_.WorldToGrid(controlled.GetPosition()).y;
    world_.PlaceBlock(GridPos { trapColumn.x, playerGridY - 1, trapColumn.z }, Block { BlockType::StoneBlock, -1, true }, true);
    const std::size_t konvoyTrapsBefore = konvoyTraps_.size();
    PlayerCommand konvoyTrapCmd = base;
    konvoyTrapCmd.useAbility1 = true;
    const bool konvoyTrapApplied = ApplyPlayerActionCommand(controlled, konvoyTrapCmd);
    const bool konvoyTrapWorked = konvoyTrapApplied && konvoyTraps_.size() == konvoyTrapsBefore + 1;

    enemy->SetPosition(Vec3 { eye.x + forward.x * 3.0f, 40.0f, eye.z + forward.z * 3.0f });
    const std::size_t konvoyTethersBefore = konvoyTethers_.size();
    PlayerCommand konvoyTetherCmd = base;
    konvoyTetherCmd.useAbility2 = true;
    const bool konvoyTetherApplied = ApplyPlayerActionCommand(controlled, konvoyTetherCmd);
    const bool konvoyTetherWorked = konvoyTetherApplied && konvoyTethers_.size() == konvoyTethersBefore + 1;

    controlled.SetHeroUltimateCharge(100.0f);
    const std::size_t konvoyDomesBefore = konvoyDomes_.size();
    PlayerCommand konvoyDomeCmd = base;
    konvoyDomeCmd.useUltimate = true;
    const bool konvoyDomeApplied = ApplyPlayerActionCommand(controlled, konvoyDomeCmd);
    const bool konvoyDomeWorked = konvoyDomeApplied && konvoyDomes_.size() == konvoyDomesBefore + 1;

    controlled.SetHeroId(HeroId::Brom);
    controlled.GetInventory().AddResource(ResourceType::Iron, 48);
    controlled.GetInventory().AddResource(ResourceType::Gold, 12);
    const std::size_t bromVacuumBefore = bromVacuumBots_.size();
    PlayerCommand bromVacuumCmd = base;
    bromVacuumCmd.useAbility1 = true;
    const bool bromVacuumApplied = ApplyPlayerActionCommand(controlled, bromVacuumCmd);
    const bool bromVacuumWorked = bromVacuumApplied && bromVacuumBots_.size() == bromVacuumBefore + 1;

    const std::size_t bromTurretBefore = bromTurretDrones_.size();
    PlayerCommand bromTurretCmd = base;
    bromTurretCmd.useAbility2 = true;
    const bool bromTurretApplied = ApplyPlayerActionCommand(controlled, bromTurretCmd);
    const bool bromTurretWorked = bromTurretApplied && bromTurretDrones_.size() == bromTurretBefore + 1;

    controlled.GetInventory().AddResource(ResourceType::Gold, 60);
    controlled.SetHeroUltimateCharge(100.0f);
    const std::size_t bromUltimateTurretsBefore = bromTurretDrones_.size();
    PlayerCommand bromUltimateCmd = base;
    bromUltimateCmd.useUltimate = true;
    const bool bromUltimateApplied = ApplyPlayerActionCommand(controlled, bromUltimateCmd);
    const bool bromUltimateWorked = bromUltimateApplied
        && bromTurretDrones_.size() > bromUltimateTurretsBefore
        && bromTurretDrones_.back().temporary
        && controlled.GetHeroState().ultimate.cooldownRemaining > 0.0f;

    // Stable spawn ids for hero devices (the 7 structs sharing one
    // HeroDeviceSnapshot id-space — see NetworkSnapshot.h): a real ability cast
    // must assign a real, non-default id, and removing an EARLIER device from
    // its vector must not reassign a SURVIVING device's id (the bug the fix
    // closes — this used to be a per-BuildNetworkSnapshot-call local counter).
    const bool bromDevicesHaveStableIds = bromVacuumWorked && bromTurretWorked
        && bromVacuumBots_.back().id > 0
        && bromTurretDrones_.back().id > 0
        && bromVacuumBots_.back().id != bromTurretDrones_.back().id;
    const int survivorTurretIdBeforeErase = bromTurretWorked ? bromTurretDrones_.back().id : -1;
    if (!bromVacuumBots_.empty())
    {
        bromVacuumBots_.erase(bromVacuumBots_.begin());
    }
    const bool heroDeviceIdSurvivedErase = bromTurretWorked
        && !bromTurretDrones_.empty()
        && bromTurretDrones_.back().id == survivorTurretIdBeforeErase;

    const bool heroAbilityWorked = radonPulseWorked && radonMolotovWorked
        && radonPrimeWorked && radonWaveWorked
        && heroAbilityResultsReplicated
        && hero1Worked && hero2Worked && likhoUltimateWorked
        && svidetelEchoWorked && svidetelPhaseWorked && svidetelUltimateWorked
        && orbitaDashWorked && orbitaBlocksWorked && orbitaPrimeWorked && orbitaTeleportWorked
        && konvoyTrapWorked && konvoyTetherWorked && konvoyDomeWorked
        && bromVacuumWorked && bromTurretWorked && bromUltimateWorked
        && bromDevicesHaveStableIds && heroDeviceIdSurvivedErase;
    if (!heroAbilityWorked)
    {
        std::cout << "network-actions-smoke hero-detail:"
                  << " radonPulse=" << (radonPulseWorked ? "ok" : "FAIL")
                  << " radonPulseResults=" << (heroAbilityResultsReplicated ? "ok" : "FAIL")
                  << " radonMolotov=" << (radonMolotovWorked ? "ok" : "FAIL")
                  << " radonPrime=" << (radonPrimeWorked ? "ok" : "FAIL")
                  << " radonWave=" << (radonWaveWorked ? "ok" : "FAIL")
                  << " likho1=" << (hero1Worked ? "ok" : "FAIL")
                  << " likho2=" << (hero2Worked ? "ok" : "FAIL")
                  << " likhoUlt=" << (likhoUltimateWorked ? "ok" : "FAIL")
                  << " svidetel1=" << (svidetelEchoWorked ? "ok" : "FAIL")
                  << " svidetel2=" << (svidetelPhaseWorked ? "ok" : "FAIL")
                  << " svidetelUlt=" << (svidetelUltimateWorked ? "ok" : "FAIL")
                  << " orbita1=" << (orbitaDashWorked ? "ok" : "FAIL")
                  << " orbita2=" << (orbitaBlocksWorked ? "ok" : "FAIL")
                  << " orbitaPrime=" << (orbitaPrimeWorked ? "ok" : "FAIL")
                  << " orbitaTeleport=" << (orbitaTeleportWorked ? "ok" : "FAIL")
                  << "(applied=" << (orbitaTeleportApplied ? "yes" : "no")
                  << ",primed=" << (orbitaTeleportPrimedAfter ? "yes" : "no")
                  << ",cd=" << orbitaTeleportCooldownAfter
                  << ",ready=" << (orbitaTeleportUltimateReadyAfter ? "yes" : "no")
                  << ",move=" << orbitaTeleportMove << ")"
                  << " konvoy1=" << (konvoyTrapWorked ? "ok" : "FAIL")
                  << " konvoy2=" << (konvoyTetherWorked ? "ok" : "FAIL")
                  << " konvoyUlt=" << (konvoyDomeWorked ? "ok" : "FAIL")
                  << " brom1=" << (bromVacuumWorked ? "ok" : "FAIL")
                  << " brom2=" << (bromTurretWorked ? "ok" : "FAIL")
                  << " bromUlt=" << (bromUltimateWorked ? "ok" : "FAIL")
                  << " deviceIds=" << (bromDevicesHaveStableIds ? "ok" : "FAIL")
                  << " deviceIdSurvivesErase=" << (heroDeviceIdSurvivedErase ? "ok" : "FAIL") << '\n';
    }

    // --- Utility: a remote dash mutates authoritative movement only. ---
    recentActionResults_.clear();
    ItemStack dashPearl;
    dashPearl.type = ItemFromUtility(UtilityType::Dash);
    dashPearl.count = 1;
    controlled.GetInventory().SwapSlot(2, dashPearl);
    controlled.SetSelectedSlot(2);
    const Vec3 velocityBeforeDash = controlled.GetVelocityVec3();
    PlayerCommand dashCmd = base;
    dashCmd.useDash = true;
    UseUtilityInputs(controlled, dashCmd);
    const Vec3 velocityAfterDash = controlled.GetVelocityVec3();
    const bool dashWorked = (velocityAfterDash - velocityBeforeDash).Length() > 0.1f
        && controlled.GetInventory().GetHotbarSlots()[2].IsEmpty();
    const MatchSnapshot dashOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool dashResultReplicated = std::any_of(
        dashOwnerView.actionResults.begin(),
        dashOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::UtilityUse)
                && result.subjectType == static_cast<int>(UtilityType::Dash)
                && result.success;
        });

    // --- Melee: an enemy lined up in front loses health on attackPressed. ---
    enemy->SetPosition(Vec3 { eye.x + forward.x * 1.6f, 40.0f, eye.z + forward.z * 1.6f });
    // Players spawn with ~1.65s anti-spawn-kill invulnerability (RespawnAtHome),
    // which correctly makes an enemy an invalid melee target on tick 0. A real
    // match melees AFTER it expires, so advance the target's timers past it —
    // otherwise this tests spawn protection, not the network melee path.
    enemy->UpdateTimers(2.0f);
    ItemStack sword;
    sword.type = ItemType::Sword;
    sword.count = 1;
    controlled.GetInventory().SwapSlot(1, sword);
    controlled.SetSelectedSlot(1);
    recentActionResults_.clear();
    const int enemyHpBefore = enemy->GetHealth();
    PlayerCommand attackCmd = base;
    attackCmd.attackPressed = true;
    ApplyNetworkPlayerActions(controlled, attackCmd, 0.05f);
    const int enemyHpAfter = enemy->GetHealth();
    const bool meleeWorked = enemyHpAfter < enemyHpBefore;
    const MatchSnapshot combatAttackerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlled.GetId());
    const MatchSnapshot combatTargetView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), enemy->GetId());
    const bool combatAttackerResultReplicated = std::any_of(
        combatAttackerView.actionResults.begin(),
        combatAttackerView.actionResults.end(),
        [controlledId, enemy](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::CombatEvent)
                && result.actorPlayerId == controlledId
                && result.targetPlayerId == enemy->GetId()
                && result.amount > 0
                && (result.flags & kCombatFlagRecipientAttacker) != 0;
        });
    const bool combatTargetResultReplicated = std::any_of(
        combatTargetView.actionResults.begin(),
        combatTargetView.actionResults.end(),
        [controlledId, enemy](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::CombatEvent)
                && result.actorPlayerId == controlledId
                && result.targetPlayerId == enemy->GetId()
                && result.amount > 0
                && (result.flags & kCombatFlagRecipientTarget) != 0;
        });
    const bool combatResultsReplicated = combatAttackerResultReplicated && combatTargetResultReplicated;

    // Park the enemy far away so it can't interfere with the build tests.
    enemy->SetPosition(Vec3 { 100.0f, 40.0f, 100.0f });

    // --- Place: with a block selected and an anchor in front, a block appears. ---
    recentActionResults_.clear();
    ItemStack stone;
    stone.type = ItemFromBlock(BlockType::StoneBlock);
    stone.count = 32;
    controlled.GetInventory().SwapSlot(0, stone);
    controlled.SetSelectedSlot(0);
    const GridPos anchorCell = world_.WorldToGrid(Vector3 {
        eye.x + forward.x * 2.0f, eye.y, eye.z + forward.z * 2.0f });
    world_.PlaceBlock(anchorCell, Block { BlockType::StoneBlock, -1, true }, true);
    const std::size_t blocksBeforePlace = world_.GetBlocks().size();
    PlayerCommand placeCmd = base;
    placeCmd.placeHeld = true;
    ApplyNetworkPlayerActions(controlled, placeCmd, 0.5f);
    const std::size_t blocksAfterPlace = world_.GetBlocks().size();
    const bool placeWorked = blocksAfterPlace == blocksBeforePlace + 1;

    // --- Break: holding attack on a block in front removes it. ---
    PlayerCommand breakCmd = base;
    breakCmd.attackHeld = true;
    bool breakWorked = false;
    bool breakProgressVisible = false;
    const std::size_t droppedItemsBeforeBreak = matchSimulation_.DroppedItems().size();
    for (int i = 0; i < 600 && !breakWorked; ++i)
    {
        ApplyNetworkPlayerActions(controlled, breakCmd, 0.05f);
        breakProgressVisible = breakProgressVisible || networkActionState_[controlled.GetId()].breakProgress.visible;
        if (world_.GetBlocks().size() < blocksAfterPlace)
        {
            breakWorked = true;
        }
    }
    const bool blockDropWorked = matchSimulation_.DroppedItems().size() == droppedItemsBeforeBreak + 1
        && matchSimulation_.DroppedItems().back().stack.type == ItemFromBlock(BlockType::StoneBlock)
        && matchSimulation_.DroppedItems().back().stack.count == 1
        && matchSimulation_.DroppedItems().back().id > 0
        && LengthVec3(matchSimulation_.DroppedItems().back().velocity) > 0.01f;

    Player localBreakUi(9901, "local-break-ui", controlled.GetTeamId(), Vector3 { 0.0f, 40.0f, 2.0f }, true);
    localBreakUi.SetControlKind(PlayerControlKind::LocalHumanPredicted);
    localBreakUi.SetYaw(aimYaw);
    selectedHotbarSlot_ = 0;
    const Vector3 localEye { localBreakUi.GetPosition().x, localBreakUi.GetPosition().y + 0.72f, localBreakUi.GetPosition().z };
    const GridPos localBreakCell = world_.WorldToGrid(Vector3 {
        localEye.x + forward.x * 2.0f,
        localEye.y,
        localEye.z + forward.z * 2.0f });
    world_.PlaceBlock(localBreakCell, Block { BlockType::StoneBlock, -1, true }, true);
    PlayerCommand localBreakCmd = base;
    localBreakCmd.controlledPlayerId = static_cast<std::uint32_t>(localBreakUi.GetId());
    localBreakCmd.attackHeld = true;
    UpdatePredictedBreakProgress(localBreakUi, localBreakCmd, 0.05f);
    const bool predictedBreakUiProgress = breakProgress_.visible
        && breakProgress_.target == localBreakCell
        && breakProgress_.fraction > 0.0f;
    ResetBreakProgress();
    ApplyNetworkPlayerActions(localBreakUi, localBreakCmd, 0.05f);
    const bool localBreakUiProgress = breakProgress_.visible
        && breakProgress_.target == localBreakCell
        && breakProgress_.fraction > 0.0f;
    ResetBreakProgress();
    networkActionState_.erase(localBreakUi.GetId());
    world_.RemoveBlock(localBreakCell);

    const MatchSnapshot blockResultOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlled.GetId());
    const MatchSnapshot blockResultEnemyView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), enemy->GetId());
    const bool blockPlaceResultReplicated = std::any_of(
        blockResultOwnerView.actionResults.begin(),
        blockResultOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::BlockPlace)
                && result.success
                && result.subjectType == static_cast<int>(BlockType::StoneBlock);
        });
    const bool blockBreakResultReplicated = std::any_of(
        blockResultOwnerView.actionResults.begin(),
        blockResultOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::BlockBreak)
                && result.success;
        });
    const bool blockResultSeqMonotonic =
        blockResultOwnerView.actionResults.size() >= 2
        && blockResultOwnerView.actionResults[0].resultSeq < blockResultOwnerView.actionResults[1].resultSeq;
    const bool blockResultsReplicated = blockPlaceResultReplicated
        && blockBreakResultReplicated
        && blockResultSeqMonotonic
        && blockResultEnemyView.actionResults.empty();

    // --- Hero-device damage (MP parity with UpdateAttackOrBreak): an enemy
    // Brom turret on the aim segment must lose health on attackPressed. This
    // path had no server-side counterpart, so a network player could not
    // destroy enemy devices at all. ---
    bromTurretDrones_.clear();
    BromTurretDrone enemyTurret {};
    enemyTurret.ownerTeamId = enemy->GetTeamId();
    enemyTurret.ownerPlayerId = enemy->GetId();
    enemyTurret.position = Vector3 { eye.x + forward.x * 1.5f, eye.y, eye.z + forward.z * 1.5f };
    enemyTurret.health = 40;
    enemyTurret.invulnerabilityTimer = 0.0f;
    bromTurretDrones_.push_back(enemyTurret);
    const int turretHpBeforeAttack = bromTurretDrones_.back().health;
    PlayerCommand deviceAttackCmd = base;
    deviceAttackCmd.attackPressed = true;
    ApplyNetworkPlayerActions(controlled, deviceAttackCmd, 0.05f);
    const bool heroDeviceDamageWorked = !bromTurretDrones_.empty()
        && bromTurretDrones_.back().health < turretHpBeforeAttack;
    bromTurretDrones_.clear();

    // --- Likho mining modifiers (MP parity): with active2 up, mining a block
    // registers a persistent cut server-side (was local-path only, so a network
    // Likho got neither the cut nor its 0.72x break-speed bonus). ---
    controlled.SetHeroId(HeroId::Likho);
    controlled.MutableHeroState().active2.active = true;
    likhoBlockCuts_.clear();
    const GridPos likhoCell = world_.WorldToGrid(Vector3 {
        eye.x + forward.x * 2.0f, eye.y, eye.z });
    world_.PlaceBlock(likhoCell, Block { BlockType::StoneBlock, -1, true }, true);
    PlayerCommand likhoMineCmd = base;
    likhoMineCmd.attackHeld = true;
    ApplyNetworkPlayerActions(controlled, likhoMineCmd, 0.01f);
    const bool likhoCutRegistered = std::any_of(
        likhoBlockCuts_.begin(), likhoBlockCuts_.end(),
        [&controlled, &likhoCell](const LikhoBlockCut& cut)
        {
            return cut.ownerPlayerId == controlled.GetId() && cut.position == likhoCell;
        });
    likhoBlockCuts_.clear();

    // --- Server tick presentation guard: authoritative world updates may hit
    // players and spawn visual/audio cues, but a network server tick must not
    // write host-local presentation.
    enemy->SetPosition(Vec3 { 3.0f, 40.0f, 0.0f });
    enemy->UpdateTimers(2.0f);
    const int serverTickHpBefore = enemy->GetHealth();
    EnergyProjectile serverTickProjectile {};
    serverTickProjectile.kind = ProjectileKind::Blaster;
    serverTickProjectile.position = Vector3 { 2.6f, 40.35f, 0.0f };
    serverTickProjectile.previousPosition = Vector3 { 1.2f, 40.35f, 0.0f };
    serverTickProjectile.startPosition = serverTickProjectile.previousPosition;
    serverTickProjectile.velocity = Vector3 { 18.0f, 0.0f, 0.0f };
    serverTickProjectile.ownerId = controlled.GetId();
    serverTickProjectile.ownerTeamId = controlled.GetTeamId();
    serverTickProjectile.lifetime = 1.0f;
    ApplyProjectileDefaults(serverTickProjectile);
    projectiles_.push_back(serverTickProjectile);
    {
        ScopedLocalFeedbackSuppression suppressServerFeedback(*this, true);
        UpdateMatchSimulation(matchSimulation_.FixedDeltaSeconds());
    }
    const bool serverTickImpactWorked = enemy->GetHealth() < serverTickHpBefore;
    // Projectile impacts push their own owner-private CombatEvent (see
    // UpdateProjectiles' PushCombatEventSnapshots call) independent of the
    // melee combat push checked above — assert it reaches BOTH the shooter and
    // the victim, the same way a melee hit does.
    const MatchSnapshot serverTickShooterView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlled.GetId());
    const MatchSnapshot serverTickVictimView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), enemy->GetId());
    const bool serverTickShooterResultReplicated = std::any_of(
        serverTickShooterView.actionResults.begin(),
        serverTickShooterView.actionResults.end(),
        [&controlled, enemy](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::CombatEvent)
                && result.actorPlayerId == controlled.GetId()
                && result.targetPlayerId == enemy->GetId()
                && result.amount > 0
                && (result.flags & kCombatFlagRecipientAttacker) != 0;
        });
    const bool serverTickVictimResultReplicated = std::any_of(
        serverTickVictimView.actionResults.begin(),
        serverTickVictimView.actionResults.end(),
        [&controlled, enemy](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::CombatEvent)
                && result.actorPlayerId == controlled.GetId()
                && result.targetPlayerId == enemy->GetId()
                && result.amount > 0
                && (result.flags & kCombatFlagRecipientTarget) != 0;
        });
    const bool serverTickResultsReplicated = serverTickShooterResultReplicated && serverTickVictimResultReplicated;

    // Stable ids for status effects: unlike every other dynamic entity type
    // these have no spawn moment (see NetworkSnapshot.h) — the id is a pure
    // function of (type, target, owner), so the SAME conceptual effect (this
    // player's speed boost) must keep the SAME id across two snapshots even
    // as an unrelated status effect on ANOTHER player appears in between
    // (which would have shifted an index-based id).
    controlled.ActivateSpeedBoost(5.0f);
    const auto findControlledSpeedBoostId = [&controlled](const MatchSnapshot& snap) -> int
    {
        for (const StatusEffectSnapshot& status : snap.statusEffects)
        {
            if (status.type == StatusEffectType::SpeedBoost && status.targetPlayerId == controlled.GetId())
            {
                return status.id;
            }
        }
        return -1;
    };
    const int speedBoostIdBefore = findControlledSpeedBoostId(BuildNetworkSnapshot());
    enemy->ActivateSpeedBoost(5.0f);
    const int speedBoostIdAfter = findControlledSpeedBoostId(BuildNetworkSnapshot());
    const bool statusEffectIdStable = speedBoostIdBefore > 0 && speedBoostIdBefore == speedBoostIdAfter;

    const bool presentationSuppressed =
        message_ == messageBefore
        && eventMessages_.size() == eventMessagesBefore
        && worldEffects_.size() == worldEffectsBefore
        && floatingTexts_.size() == floatingTextsBefore;
    const bool localCombatFeedbackSuppressed =
        stats_.hitsDealt == statsBefore.hitsDealt
        && stats_.damageDealt == statsBefore.damageDealt
        && stats_.kills == statsBefore.kills
        && stats_.coreDamageDealt == statsBefore.coreDamageDealt
        && stats_.coresDestroyed == statsBefore.coresDestroyed
        && stats_.blocksPlaced == statsBefore.blocksPlaced
        && stats_.blocksBroken == statsBefore.blocksBroken
        && hitMarkerTimer_ == hitMarkerBefore
        && damageFlashTimer_ == damageFlashBefore
        && fovKick_ == fovKickBefore;
    const bool audioRestored = audio_.IsMuted() == audioMutedBefore;
    localPlayerId_ = localPlayerIdBefore;

    networkControlledPlayerIds_.clear();
    networkActionState_.clear();

    std::cout << "network-actions-smoke: meleeHp " << enemyHpBefore << "->" << enemyHpAfter
              << " melee=" << (meleeWorked ? "ok" : "FAIL")
              << " combatResults=" << (combatResultsReplicated ? "ok" : "FAIL")
              << " | blocks " << blocksBeforePlace << "->" << blocksAfterPlace
              << " heroAbility=" << (heroAbilityWorked ? "ok" : "FAIL")
              << " dash=" << (dashWorked ? "ok" : "FAIL")
              << " dashResults=" << (dashResultReplicated ? "ok" : "FAIL")
              << " place=" << (placeWorked ? "ok" : "FAIL")
              << " break=" << (breakWorked ? "ok" : "FAIL")
              << " breakProgress=" << (breakProgressVisible ? "ok" : "FAIL")
              << " predictedBreakUi=" << (predictedBreakUiProgress ? "ok" : "FAIL")
              << " localBreakUi=" << (localBreakUiProgress ? "ok" : "FAIL")
              << " blockDrop=" << (blockDropWorked ? "ok" : "FAIL")
              << " deviceDamage=" << (heroDeviceDamageWorked ? "ok" : "FAIL")
              << " likhoCut=" << (likhoCutRegistered ? "ok" : "FAIL")
              << " blockResults=" << (blockResultsReplicated ? "ok" : "FAIL")
              << " bromChest=" << ((bromChestBlockWorked && bromChestDeliveryWorked) ? "ok" : "FAIL")
              << " serverTick=" << (serverTickImpactWorked ? "ok" : "FAIL")
              << " serverTickResults=" << (serverTickResultsReplicated ? "ok" : "FAIL")
              << " statusEffectIdStable=" << (statusEffectIdStable ? "ok" : "FAIL")
              << " presentation=" << (presentationSuppressed ? "ok" : "FAIL")
              << " localCombat=" << (localCombatFeedbackSuppressed ? "ok" : "FAIL")
              << " audioRestore=" << (audioRestored ? "ok" : "FAIL") << '\n';

    const bool ok = meleeWorked && combatResultsReplicated
        && heroAbilityWorked && dashWorked && dashResultReplicated && placeWorked && breakWorked
        && breakProgressVisible && predictedBreakUiProgress && localBreakUiProgress && blockDropWorked
        && heroDeviceDamageWorked && likhoCutRegistered
        && blockResultsReplicated
        && bromChestBlockWorked && bromChestDeliveryWorked
        && serverTickImpactWorked && serverTickResultsReplicated && statusEffectIdStable
        && presentationSuppressed && localCombatFeedbackSuppressed && audioRestored;
    std::cout << (ok ? "NETWORK_ACTIONS_SMOKE_OK" : "NETWORK_ACTIONS_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunLagCompSmoke()
{
    // #4 headless test: a melee that only overlaps the target's PAST position
    // must land when the command carries a rewindTick that points at that past
    // frame, and must MISS the same geometry when it doesn't (live position).
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();

    // Attacker: a non-local (network-controlled) player so GetSelectedHotbarStack
    // reads its per-player slot; enemy: any player on another team.
    Player* attackerPtr = nullptr;
    for (Player& candidate : matchSimulation_.Players())
    {
        if (IsBotControlled(ControlKindForPlayer(candidate)))
        {
            attackerPtr = &candidate;
            break;
        }
    }
    Player* enemyPtr = nullptr;
    if (attackerPtr != nullptr)
    {
        for (Player& candidate : matchSimulation_.Players())
        {
            if (candidate.GetTeamId() != attackerPtr->GetTeamId())
            {
                enemyPtr = &candidate;
                break;
            }
        }
    }
    if (attackerPtr == nullptr || enemyPtr == nullptr)
    {
        std::cout << "lag-comp-smoke: missing attacker/enemy\n"
                     "LAG_COMP_SMOKE_FAIL" << std::endl;
        return 9;
    }
    Player& attacker = *attackerPtr;
    Player& enemy = *enemyPtr;
    const int attackerId = attacker.GetId();
    const int enemyId = enemy.GetId();
    MarkNetworkControlledPlayer(attackerId);
    suppressLocalFeedback_ = false;

    // Clear air; attacker faces +X, gives it a sword.
    attacker.SetPosition(Vec3 { 0.0f, 40.0f, 0.0f });
    const float aimYaw = PI / 2.0f; // Forward() = (+1, 0, 0)
    attacker.SetYaw(aimYaw);
    ItemStack sword;
    sword.type = ItemType::Sword;
    sword.count = 1;
    attacker.GetInventory().SwapSlot(0, sword);
    attacker.SetSelectedSlot(0);
    enemy.Heal(enemy.GetMaxHealth());
    enemy.UpdateTimers(2.0f); // clear hit-invulnerability

    PlayerCommand base;
    base.controlledPlayerId = static_cast<std::uint32_t>(attackerId);
    base.selectedSlot = 0;
    base.aimYaw = aimYaw;
    base.aimPitch = 0.0f;

    // Positions: PAST = right in front of the attacker (in melee range), NOW =
    // far away so a live-position hit test can't reach it.
    const Vec3 pastPos { 2.0f, 40.0f, 0.0f };
    const Vec3 nowPos { 40.0f, 40.0f, 0.0f };

    // Record a history frame with the enemy at its PAST position, at a known
    // tick. RecordLagCompFrame reads matchSimulation_.CurrentTick(); advance a
    // few ticks so rewindTick has room below the current tick.
    lagCompHistory_.clear();
    for (int i = 0; i < 5; ++i)
    {
        matchSimulation_.AdvanceTick();
    }
    enemy.SetPosition(pastPos);
    RecordLagCompFrame();
    const std::uint32_t recordedTick = matchSimulation_.CurrentTick();
    for (int i = 0; i < 5; ++i)
    {
        matchSimulation_.AdvanceTick();
    }

    // 1) WITHOUT rewind (rewindTick 0): enemy is at NOW (far) — melee misses.
    enemy.SetPosition(nowPos);
    enemy.Heal(enemy.GetMaxHealth());
    enemy.UpdateTimers(2.0f);
    const int hpBeforeNoRewind = enemy.GetHealth();
    PlayerCommand noRewind = base;
    noRewind.attackPressed = true;
    noRewind.rewindTick = 0;
    ApplyNetworkPlayerActions(attacker, noRewind, matchSimulation_.FixedDeltaSeconds());
    const int hpAfterNoRewind = enemy.GetHealth();
    const bool missedLive = hpAfterNoRewind == hpBeforeNoRewind;

    // 2) WITH rewind: enemy still at NOW (far), but the command rewinds hitboxes
    // to recordedTick where the enemy was at PAST (in range) — melee lands, and
    // the enemy's LIVE position is restored afterward (only HP/velocity persist).
    enemy.SetPosition(nowPos);
    enemy.Heal(enemy.GetMaxHealth());
    enemy.UpdateTimers(2.0f);
    attacker.ResetAttackCooldown(0.0f);
    attacker.UpdateTimers(1.0f);
    const int hpBeforeRewind = enemy.GetHealth();
    PlayerCommand withRewind = base;
    withRewind.attackPressed = true;
    withRewind.rewindTick = recordedTick;
    ApplyNetworkPlayerActions(attacker, withRewind, matchSimulation_.FixedDeltaSeconds());
    const int hpAfterRewind = enemy.GetHealth();
    const Vec3 enemyPosAfter = enemy.GetPositionVec3();
    const bool hitRewound = hpAfterRewind < hpBeforeRewind;
    const bool positionRestored =
        std::fabs(enemyPosAfter.x - nowPos.x) < 0.001f
        && std::fabs(enemyPosAfter.z - nowPos.z) < 0.001f;

    const bool ok = missedLive && hitRewound && positionRestored;
    std::cout << "lag-comp-smoke: attackerId=" << attackerId << " enemyId=" << enemyId
              << " recordedTick=" << recordedTick
              << " liveMiss=" << (missedLive ? "ok" : "FAIL")
              << " (hp " << hpBeforeNoRewind << "->" << hpAfterNoRewind << ")"
              << " rewoundHit=" << (hitRewound ? "ok" : "FAIL")
              << " (hp " << hpBeforeRewind << "->" << hpAfterRewind << ")"
              << " positionRestored=" << (positionRestored ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "LAG_COMP_SMOKE_OK" : "LAG_COMP_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunClientDynamicApplySmoke()
{
    Game server;
    if (!server.Initialize(true))
    {
        std::cout << "client-dynamic-apply-smoke: server initialize failed\n"
                     "CLIENT_DYNAMIC_APPLY_SMOKE_FAIL" << std::endl;
        return 9;
    }

    server.networkMode_ = NetworkMode::LocalHost;
    server.selectedMode_ = MatchMode::FourTeams;
    server.selectedTeamId_ = 0;
    server.SetupMatch();
    server.screen_ = GameScreen::Playing;

    if (server.matchSimulation_.Players().size() < 2)
    {
        std::cout << "client-dynamic-apply-smoke: not enough players\n"
                     "CLIENT_DYNAMIC_APPLY_SMOKE_FAIL" << std::endl;
        server.Shutdown();
        return 9;
    }

    Player& owner = server.matchSimulation_.Players().front();
    Player& target = server.matchSimulation_.Players()[1];
    owner.SetPosition(Vec3 { 3.0f, 35.0f, -2.0f });
    target.SetPosition(Vec3 { 5.5f, 35.0f, -2.0f });
    owner.ActivateSpeedBoost(4.0f);
    target.ApplyControlDebuff(3.5f, 0.72f, 0.78f, 0.70f);

    EnergyProjectile projectile;
    projectile.kind = ProjectileKind::Fireball;
    projectile.position = Vector3 { 4.0f, 36.0f, -2.0f };
    projectile.previousPosition = projectile.position;
    projectile.startPosition = projectile.position;
    projectile.velocity = Vector3 { 9.0f, 1.0f, 0.0f };
    projectile.ownerId = owner.GetId();
    projectile.ownerTeamId = owner.GetTeamId();
    projectile.lifetime = 2.25f;
    projectile.fireZone = true;
    ApplyProjectileDefaults(projectile);
    server.projectiles_.push_back(projectile);

    TimedExplosion explosive;
    explosive.block = GridPos { 8, 35, -2 };
    explosive.ownerPlayerId = owner.GetId();
    explosive.ownerTeamId = owner.GetTeamId();
    explosive.timer = 1.35f;
    explosive.radius = 2.75f;
    server.timedExplosions_.push_back(explosive);

    HazardZone hazard;
    hazard.position = Vector3 { 6.0f, 35.0f, -2.0f };
    hazard.ownerPlayerId = owner.GetId();
    hazard.ownerTeamId = owner.GetTeamId();
    hazard.radius = 3.25f;
    hazard.lifetime = 4.5f;
    hazard.damagePerTick = 16;
    hazard.blueFire = true;
    server.hazardZones_.push_back(hazard);

    BromVacuumBot bot;
    bot.position = Vector3 { 2.0f, 35.0f, -1.0f };
    bot.ownerPlayerId = owner.GetId();
    bot.ownerTeamId = owner.GetTeamId();
    bot.lifetime = 7.0f;
    bot.health = 44;
    server.bromVacuumBots_.push_back(bot);

    BromTurretDrone drone;
    drone.position = Vector3 { 2.5f, 35.0f, -1.5f };
    drone.ownerPlayerId = owner.GetId();
    drone.ownerTeamId = owner.GetTeamId();
    drone.lifetime = 8.0f;
    drone.health = 58;
    server.bromTurretDrones_.push_back(drone);

    KonvoyTrap trap;
    trap.position = Vector3 { 3.0f, 35.0f, -3.5f };
    trap.ownerPlayerId = owner.GetId();
    trap.ownerTeamId = owner.GetTeamId();
    trap.lifetime = 6.0f;
    trap.health = 47;
    server.konvoyTraps_.push_back(trap);

    KonvoyTether tether;
    tether.ownerPlayerId = owner.GetId();
    tether.targetPlayerId = target.GetId();
    tether.ownerTeamId = owner.GetTeamId();
    tether.lifetime = 5.0f;
    server.konvoyTethers_.push_back(tether);

    KonvoyDome dome;
    dome.position = Vector3 { 4.0f, 35.0f, -4.0f };
    dome.ownerPlayerId = owner.GetId();
    dome.ownerTeamId = owner.GetTeamId();
    dome.lifetime = 9.0f;
    dome.health = 160;
    server.konvoyDomes_.push_back(dome);

    LikhoBleed bleed;
    bleed.targetPlayerId = target.GetId();
    bleed.ownerPlayerId = owner.GetId();
    bleed.ownerTeamId = owner.GetTeamId();
    bleed.lifetime = 4.25f;
    bleed.stacks = 2;
    server.likhoBleeds_.push_back(bleed);

    SvidetelEcho echo;
    echo.position = Vector3 { 5.0f, 35.0f, -1.0f };
    echo.ownerPlayerId = owner.GetId();
    echo.ownerTeamId = owner.GetTeamId();
    echo.lifetime = 11.0f;
    echo.health = 41;
    server.svidetelEchoes_.push_back(echo);

    server.radonBurns_.push_back(
        RadonBurn { target.GetId(), owner.GetId(), owner.GetTeamId(), 2.2f, 0.7f, true });
    KonvoyIntruderMark mark;
    mark.ownerPlayerId = owner.GetId();
    mark.targetPlayerId = target.GetId();
    mark.exposure = 1.0f;
    mark.markedTimer = 3.3f;
    server.konvoyIntruderMarks_.push_back(mark);

    server.matchSimulation_.DroppedItems().push_back(DroppedItem {
        ItemStack { ItemType::IronResource, 3 },
        Vec3 { 3.2f, 35.8f, -2.4f },
        Vec3 { 1.25f, 1.2f, 0.35f },
        owner.GetId(),
        0.0f,
        12.0f,
        1.0f,
        false,
        server.NextDroppedItemId() });

    // Distinctive animation poses so we can prove the client adopts them. The
    // owner is the client's OWN player (event pose → adopted), the target is a
    // remote player (locomotion → adopted, since remotes take the full pose).
    owner.MutableHeroState().animationState = HeroAnimationState::Attack;  // event
    target.MutableHeroState().animationState = HeroAnimationState::Run;    // locomotion

    const MatchSnapshot snapshot = server.BuildNetworkSnapshot();

    Game client;
    if (!client.Initialize(true))
    {
        std::cout << "client-dynamic-apply-smoke: client initialize failed\n"
                     "CLIENT_DYNAMIC_APPLY_SMOKE_FAIL" << std::endl;
        server.Shutdown();
        return 9;
    }
    client.networkMode_ = NetworkMode::LocalClient;
    client.selectedMode_ = MatchMode::FourTeams;
    client.selectedTeamId_ = 0;
    client.SetupMatch();
    client.networkAssignedPlayerId_ = owner.GetId();
    client.localPlayerId_ = owner.GetId();

    client.projectiles_.clear();
    client.timedExplosions_.clear();
    client.hazardZones_.clear();
    client.bromVacuumBots_.clear();
    client.bromTurretDrones_.clear();
    client.konvoyTraps_.clear();
    client.konvoyTethers_.clear();
    client.konvoyDomes_.clear();
    client.likhoBleeds_.clear();
    client.svidetelEchoes_.clear();
    client.radonBurns_.clear();
    client.konvoyIntruderMarks_.clear();
    client.replicatedProjectiles_.clear();
    client.replicatedExplosives_.clear();
    client.replicatedHazardZones_.clear();
    client.replicatedHeroDevices_.clear();
    client.replicatedStatusEffects_.clear();

    client.ApplyClientSnapshot(snapshot);

    const auto near = [](float a, float b)
    {
        return std::fabs(a - b) < 0.001f;
    };
    const auto sameVec = [&near](Vec3 a, Vec3 b)
    {
        return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
    };
    const auto sameRayVec = [&near](Vector3 a, Vec3 b)
    {
        return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
    };

    const bool projectileApplied = !snapshot.projectiles.empty()
        && client.projectiles_.size() == snapshot.projectiles.size()
        && client.replicatedProjectiles_.size() == snapshot.projectiles.size()
        && client.replicatedProjectiles_[0].id == snapshot.projectiles[0].id
        && sameVec(client.replicatedProjectiles_[0].position, snapshot.projectiles[0].position)
        && sameRayVec(client.projectiles_[0].position, snapshot.projectiles[0].position)
        && client.projectiles_[0].kind == ProjectileKind::Fireball;
    const Vector3 projectileVisualBefore = !client.projectiles_.empty()
        ? client.projectiles_[0].position
        : Vector3 {};
    const float projectileLifetimeBefore = !client.projectiles_.empty()
        ? client.projectiles_[0].lifetime
        : 0.0f;
    const Vec3 droppedItemBefore = !client.matchSimulation_.DroppedItems().empty()
        ? client.matchSimulation_.DroppedItems()[0].position
        : Vec3 {};
    const float droppedItemLifetimeBefore = !client.matchSimulation_.DroppedItems().empty()
        ? client.matchSimulation_.DroppedItems()[0].lifetime
        : 0.0f;
    client.UpdateClientReplicatedDynamics(0.05f);
    const Vector3 projectileVisualDelta {
        client.projectiles_.empty() ? 0.0f : client.projectiles_[0].position.x - projectileVisualBefore.x,
        client.projectiles_.empty() ? 0.0f : client.projectiles_[0].position.y - projectileVisualBefore.y,
        client.projectiles_.empty() ? 0.0f : client.projectiles_[0].position.z - projectileVisualBefore.z
    };
    const bool projectileVisualAdvanced = !client.projectiles_.empty()
        && std::sqrt(projectileVisualDelta.x * projectileVisualDelta.x
            + projectileVisualDelta.y * projectileVisualDelta.y
            + projectileVisualDelta.z * projectileVisualDelta.z) > 0.01f
        && client.projectiles_[0].lifetime < projectileLifetimeBefore;
    const bool droppedItemApplied = !snapshot.droppedItems.empty()
        && !client.matchSimulation_.DroppedItems().empty()
        && client.matchSimulation_.DroppedItems()[0].id == snapshot.droppedItems[0].id
        && client.matchSimulation_.DroppedItems()[0].stack.type == static_cast<ItemType>(snapshot.droppedItems[0].itemType);
    const Vec3 droppedItemAfter = !client.matchSimulation_.DroppedItems().empty()
        ? client.matchSimulation_.DroppedItems()[0].position
        : Vec3 {};
    const Vec3 droppedItemDelta {
        droppedItemAfter.x - droppedItemBefore.x,
        droppedItemAfter.y - droppedItemBefore.y,
        droppedItemAfter.z - droppedItemBefore.z
    };
    const bool droppedItemVisualAdvanced = droppedItemApplied
        && LengthVec3(droppedItemDelta) > 0.01f
        && client.matchSimulation_.DroppedItems()[0].lifetime < droppedItemLifetimeBefore;

    const bool explosiveApplied = !snapshot.explosives.empty()
        && client.timedExplosions_.size() == snapshot.explosives.size()
        && client.replicatedExplosives_.size() == snapshot.explosives.size()
        && client.replicatedExplosives_[0].id == snapshot.explosives[0].id
        && client.timedExplosions_[0].block == explosive.block;

    const bool hazardApplied = !snapshot.hazardZones.empty()
        && client.hazardZones_.size() == snapshot.hazardZones.size()
        && client.replicatedHazardZones_.size() == snapshot.hazardZones.size()
        && client.replicatedHazardZones_[0].id == snapshot.hazardZones[0].id
        && sameRayVec(client.hazardZones_[0].position, snapshot.hazardZones[0].position)
        && client.hazardZones_[0].blueFire;

    const std::size_t clientDeviceCount = client.bromVacuumBots_.size()
        + client.bromTurretDrones_.size()
        + client.konvoyTraps_.size()
        + client.konvoyTethers_.size()
        + client.konvoyDomes_.size()
        + client.likhoBleeds_.size()
        + client.svidetelEchoes_.size();
    const bool devicesApplied = !snapshot.heroDevices.empty()
        && clientDeviceCount == snapshot.heroDevices.size()
        && client.replicatedHeroDevices_.size() == snapshot.heroDevices.size()
        && client.replicatedHeroDevices_[0].id == snapshot.heroDevices[0].id
        && sameRayVec(client.bromVacuumBots_[0].position, snapshot.heroDevices[0].position);

    const bool statusesApplied = !snapshot.statusEffects.empty()
        && client.replicatedStatusEffects_.size() == snapshot.statusEffects.size()
        && !client.radonBurns_.empty()
        && !client.konvoyIntruderMarks_.empty()
        && client.radonBurns_[0].blueFire
        && client.radonBurns_[0].targetPlayerId == target.GetId()
        && client.konvoyIntruderMarks_[0].targetPlayerId == target.GetId();

    const Player* clientOwner = client.matchSimulation_.GetPlayer(owner.GetId());
    const Player* clientTarget = client.matchSimulation_.GetPlayer(target.GetId());
    // Own player adopts the server EVENT pose; remote player adopts the full pose
    // (locomotion included).
    const bool animationApplied = clientOwner != nullptr && clientTarget != nullptr
        && clientOwner->GetHeroState().animationState == HeroAnimationState::Attack
        && clientTarget->GetHeroState().animationState == HeroAnimationState::Run;

    owner.Damage(18);
    owner.SetVelocity(Vec3 { 5.75f, 1.35f, 0.0f });
    const MatchSnapshot knockbackSnapshot = server.BuildNetworkSnapshot();
    client.ApplyClientSnapshot(knockbackSnapshot);
    const Player* clientOwnerAfterHit = client.matchSimulation_.GetPlayer(owner.GetId());
    const bool ownKnockbackVelocityApplied = clientOwnerAfterHit != nullptr
        && clientOwnerAfterHit->GetHealth() == owner.GetHealth()
        && DistanceVec3(clientOwnerAfterHit->GetVelocityVec3(), owner.GetVelocityVec3()) < 0.001f;

    // Own-player rule: a LOCOMOTION pose in the snapshot must NOT be adopted for our
    // own player (its locomotion is derived locally from prediction). Re-send with
    // the owner now "running" and confirm the client keeps the event pose instead.
    owner.MutableHeroState().animationState = HeroAnimationState::Run;
    client.ApplyClientSnapshot(server.BuildNetworkSnapshot());
    const Player* clientOwner2 = client.matchSimulation_.GetPlayer(owner.GetId());
    const bool ownLocomotionLocal = clientOwner2 != nullptr
        && clientOwner2->GetHeroState().animationState != HeroAnimationState::Run;
    const bool animationOk = animationApplied && ownLocomotionLocal;

    // Regression (2026-07-01, user bug report): "ability cooldown HUD always
    // says ready" — PlayerSnapshot never carried HeroRuntimeState at all, so a
    // network client's own ability HUD (Renderer's AbilityStateText, which
    // reads player.GetHeroState()) was permanently stuck on defaults. Set a
    // known non-zero cooldown/charge/primed state on the server's owner,
    // fold a snapshot, and confirm the CLIENT's copy of the same player
    // reflects the real values instead of staying at 0/false.
    owner.MutableHeroState().active1.cooldownRemaining = 4.5f;
    owner.MutableHeroState().active2.activeTimer = 1.1f;
    owner.MutableHeroState().ultimate.cooldownRemaining = 22.0f;
    owner.MutableHeroState().ultimateCharge = 57.0f;
    owner.MutableHeroState().ultimatePrimed = true;
    client.ApplyClientSnapshot(FilterSnapshotForClient(server.BuildNetworkSnapshot(), owner.GetId()));
    const Player* clientOwnerForHud = client.matchSimulation_.GetPlayer(owner.GetId());
    const bool abilityHudApplied = clientOwnerForHud != nullptr
        && near(clientOwnerForHud->GetHeroState().active1.cooldownRemaining, 4.5f)
        && near(clientOwnerForHud->GetHeroState().active2.activeTimer, 1.1f)
        && near(clientOwnerForHud->GetHeroState().ultimate.cooldownRemaining, 22.0f)
        && near(clientOwnerForHud->GetHeroState().ultimateCharge, 57.0f)
        && clientOwnerForHud->GetHeroState().ultimatePrimed;

    // Regression (2026-07-01, user bug report): "sniper doesn't charge" —
    // bow/blaster charge state has the exact same gap as ability cooldowns
    // (never in PlayerSnapshot, so a network client's own Player object never
    // learns it — UpdateCombatPreview's charge % HUD read stuck defaults).
    // Advance real charge state server-side and confirm it round-trips.
    owner.ResetBowDraw();
    owner.AdvanceBowDraw(0.35f);
    owner.CancelBlasterLoading();
    owner.StartBlasterLoading();
    client.ApplyClientSnapshot(FilterSnapshotForClient(server.BuildNetworkSnapshot(), owner.GetId()));
    const Player* clientOwnerForCharge = client.matchSimulation_.GetPlayer(owner.GetId());
    const bool weaponChargeApplied = clientOwnerForCharge != nullptr
        && near(clientOwnerForCharge->GetBowDrawTimer(), owner.GetBowDrawTimer())
        && owner.GetBowDrawTimer() > 0.0f
        && clientOwnerForCharge->GetBlasterState() == owner.GetBlasterState()
        && owner.GetBlasterState() == CrossbowState::Loading;

    // Diagnostic (2026-07-01, user bug report): does a real Orbita dash cast,
    // pushed through PushHeroAbilityActionResultSnapshot and folded through
    // ApplyClientSnapshotFeedback exactly like a real network client would,
    // actually reconstruct the correct world-effect KIND on the client, or
    // does it silently fall back to Burst (which would explain "leaves a
    // sphere identical to a pickup effect" — pickup uses the 4-arg
    // AddWorldEffect overload, which always hardcodes Burst).
    owner.SetHeroId(HeroId::Orbita);
    owner.SetYaw(0.0f);
    owner.MutableHeroState().active1.cooldownRemaining = 0.0f;
    // client.Initialize(true) (headless) defaults suppressLocalFeedback_ to
    // true, which would make PresentHeroAbilityResult silently no-op — force
    // it off, matching how RunNetworkActionsSmoke already does this to test
    // presentation while staying headless. A real GUI client is never headless
    // so this suppression never applies there.
    client.suppressLocalFeedback_ = false;
    const std::size_t clientEffectsBeforeDash = client.worldEffects_.size();
    PlayerCommand orbitaDashDiagCmd;
    orbitaDashDiagCmd.controlledPlayerId = static_cast<std::uint32_t>(owner.GetId());
    orbitaDashDiagCmd.aimYaw = 0.0f;
    orbitaDashDiagCmd.useAbility1 = true;
    const bool orbitaDashDiagApplied = server.ApplyPlayerActionCommand(owner, orbitaDashDiagCmd);
    const MatchSnapshot orbitaDashSnapshot =
        FilterSnapshotForClient(server.BuildNetworkSnapshot(), owner.GetId());
    // ApplyClientSnapshotFeedback itself early-returns on `headless_` (a real
    // client is never headless — there'd be nothing to present to — but this
    // whole smoke harness runs headless by construction). Flip it off only
    // around this one call so the diagnostic actually exercises the function
    // instead of silently no-op'ing.
    client.headless_ = false;
    client.ApplyClientSnapshotFeedback(orbitaDashSnapshot);
    client.headless_ = true;
    const WorldEffect* orbitaDashEffect = client.worldEffects_.size() > clientEffectsBeforeDash
        ? &client.worldEffects_.back()
        : nullptr;
    const bool orbitaDashEffectKindCorrect = orbitaDashDiagApplied
        && orbitaDashEffect != nullptr
        && orbitaDashEffect->kind == WorldEffectKind::Trail
        && orbitaDashEffect->radius > 1.0f && orbitaDashEffect->radius < 1.3f;
    std::cout << "client-dynamic-apply-smoke: orbitaDash applied="
              << (orbitaDashDiagApplied ? "yes" : "no")
              << " effectPushed=" << (orbitaDashEffect != nullptr ? "yes" : "no")
              << " kind=" << (orbitaDashEffect != nullptr ? static_cast<int>(orbitaDashEffect->kind) : -1)
              << " radius=" << (orbitaDashEffect != nullptr ? orbitaDashEffect->radius : -1.0f)
              << " expectedKind=" << static_cast<int>(WorldEffectKind::Trail) << '\n';

    // Diagnostic (2026-07-01, RunNetworkClient audit): HandleDeathsAndRespawns
    // is server-only (never runs on the client), so a network player's own
    // death previously produced NO death overlay and NEVER entered spectator
    // mode on final death — PushWorldEventSnapshot(PlayerDied) + the client's
    // new WorldEventKind::PlayerDied branch above are the fix. Push a real
    // final-death event for the client's own player (owner) and confirm the
    // overlay/spectator-mode/killer-name/cause all land.
    server.spectatorMode_ = false;
    server.PushWorldEventSnapshot(
        WorldEventKind::PlayerDied, target.GetId(), owner.GetId(), owner.GetTeamId(),
        owner.GetPosition(), 0, 0, /*finalDeath*/ 1, "топором Свидетеля");
    const MatchSnapshot ownDeathSnapshot =
        FilterSnapshotForClient(server.BuildNetworkSnapshot(), owner.GetId());
    client.spectatorMode_ = false;
    client.localDeathOverlayTimer_ = 0.0f;
    client.headless_ = false;
    client.ApplyClientSnapshotFeedback(ownDeathSnapshot);
    client.headless_ = true;
    const bool ownDeathOverlayApplied = client.localDeathOverlayTimer_ > 0.0f
        && client.spectatorMode_
        && client.localDeathKiller_ == target.GetName()
        && client.localDeathCause_ == "топором Свидетеля";
    std::cout << "client-dynamic-apply-smoke: ownDeath overlayTimer=" << client.localDeathOverlayTimer_
              << " spectator=" << (client.spectatorMode_ ? "yes" : "no")
              << " killer=" << client.localDeathKiller_
              << " result=" << (ownDeathOverlayApplied ? "ok" : "FAIL") << '\n';

    Player* clientOwnerForSpectator = client.matchSimulation_.GetPlayer(owner.GetId());
    Player* clientTargetForSpectator = client.matchSimulation_.GetPlayer(target.GetId());
    if (clientOwnerForSpectator != nullptr)
    {
        clientOwnerForSpectator->Kill(true);
    }
    if (clientTargetForSpectator != nullptr && !clientTargetForSpectator->IsAlive())
    {
        clientTargetForSpectator->RespawnAtHome();
    }
    client.networkAssignedPlayerId_ = owner.GetId();
    client.localPlayerId_ = owner.GetId();
    client.spectatorMode_ = false;
    client.UpdateCamera(1.0f / 60.0f);
    const Player* initialSpectatorTarget = client.GetSpectatorTarget();
    client.CycleSpectatorTarget(1);
    const Player* cycledSpectatorTarget = client.GetSpectatorTarget();
    client.spectatorFreeCamera_ = true;
    client.UpdateCamera(1.0f / 60.0f);
    const bool spectatorControlsOk = client.spectatorMode_
        && initialSpectatorTarget != nullptr
        && cycledSpectatorTarget != nullptr
        && client.spectatorFreeCamera_;
    if (clientOwnerForSpectator != nullptr)
    {
        clientOwnerForSpectator->RespawnAtHome();
    }
    client.UpdateCamera(1.0f / 60.0f);
    const bool spectatorRespawnOk = !client.spectatorMode_
        && !client.spectatorFreeCamera_
        && clientOwnerForSpectator != nullptr
        && clientOwnerForSpectator->IsAlive();
    std::cout << "client-dynamic-apply-smoke: spectator controls="
              << (spectatorControlsOk ? "ok" : "FAIL")
              << " respawn=" << (spectatorRespawnOk ? "ok" : "FAIL") << '\n';

    const bool ok = projectileApplied && projectileVisualAdvanced
        && droppedItemApplied && droppedItemVisualAdvanced
        && explosiveApplied && hazardApplied
        && devicesApplied && statusesApplied && animationOk && ownKnockbackVelocityApplied
        && orbitaDashEffectKindCorrect && abilityHudApplied && weaponChargeApplied
        && ownDeathOverlayApplied && spectatorControlsOk && spectatorRespawnOk;

    std::cout << "client-dynamic-apply-smoke: snapshot projectiles=" << snapshot.projectiles.size()
              << " explosives=" << snapshot.explosives.size()
              << " hazardZones=" << snapshot.hazardZones.size()
              << " heroDevices=" << snapshot.heroDevices.size()
              << " statusEffects=" << snapshot.statusEffects.size() << '\n';
    std::cout << "client-dynamic-apply-smoke: client projectiles=" << client.projectiles_.size()
              << " explosives=" << client.timedExplosions_.size()
              << " hazardZones=" << client.hazardZones_.size()
              << " heroDevices=" << clientDeviceCount
              << " statusEffects=" << client.replicatedStatusEffects_.size() << '\n';
    std::cout << "client-dynamic-apply-smoke: checks projectile="
              << (projectileApplied ? "ok" : "FAIL")
              << " projectileVisual=" << (projectileVisualAdvanced ? "ok" : "FAIL")
              << " droppedItem=" << (droppedItemApplied ? "ok" : "FAIL")
              << " droppedItemVisual=" << (droppedItemVisualAdvanced ? "ok" : "FAIL")
              << " explosive=" << (explosiveApplied ? "ok" : "FAIL")
              << " hazard=" << (hazardApplied ? "ok" : "FAIL")
              << " devices=" << (devicesApplied ? "ok" : "FAIL")
              << " statuses=" << (statusesApplied ? "ok" : "FAIL")
              << " animation=" << (animationOk ? "ok" : "FAIL")
              << " (apply=" << (animationApplied ? "ok" : "FAIL")
              << " ownLocal=" << (ownLocomotionLocal ? "ok" : "FAIL") << ")"
              << " ownKnockback=" << (ownKnockbackVelocityApplied ? "ok" : "FAIL")
              << " abilityHud=" << (abilityHudApplied ? "ok" : "FAIL")
              << " weaponCharge=" << (weaponChargeApplied ? "ok" : "FAIL")
              << " orbitaDashEffect=" << (orbitaDashEffectKindCorrect ? "ok" : "FAIL")
              << " ownDeath=" << (ownDeathOverlayApplied ? "ok" : "FAIL")
              << " spectatorControls=" << (spectatorControlsOk ? "ok" : "FAIL")
              << " spectatorRespawn=" << (spectatorRespawnOk ? "ok" : "FAIL")
              << '\n';
    std::cout << (ok ? "CLIENT_DYNAMIC_APPLY_SMOKE_OK" : "CLIENT_DYNAMIC_APPLY_SMOKE_FAIL")
              << std::endl;

    client.Shutdown();
    server.Shutdown();
    return ok ? 0 : 9;
}

int Game::RunDedicatedServerStub()
{
    networkMode_ = NetworkMode::DedicatedServer;
    std::cout << "dedicated-server: transport is a STUB (no UDP/ENet yet); config validated, "
                 "running local authoritative simulation.\n";
    return RunNetworkSmoke();
}

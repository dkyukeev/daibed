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
#include <cstdio>
#include <iostream>
#include <limits>
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
constexpr float kDefaultBlockInteractionReach = 4.5f;
constexpr float kCreativeBlockInteractionReach = 12.0f;
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
constexpr int kHeroAbilityFlagHeroVoice = 1 << 12;
constexpr int kHeroAbilityVoiceEventShift = 13;
constexpr int kHeroAbilityVoiceEventMask = 0xF;

// Reused for both bow (critical shot) and blaster (sniper item) spawn feedback —
// the two never mix within a single ActionResultSnapshot (subjectType picks the
// projectile kind).
constexpr int kProjectileFlagPrimary = 1 << 0;

HeroVoiceEvent DefaultHeroVoiceEvent(HeroAbilitySlot slot)
{
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        return HeroVoiceEvent::Active1;
    case HeroAbilitySlot::Active2:
        return HeroVoiceEvent::Active2;
    case HeroAbilitySlot::Ultimate:
        return HeroVoiceEvent::Ultimate;
    }
    return HeroVoiceEvent::Count;
}

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
    // The creative special-blocks palette owns the mouse the same way the
    // shop/inventory overlays do: editor clicks must not double as combat or
    // block placement.
    if (shopOpen_ || inventoryOpen_ || (creativeMode_ && creativeSpecialMode_))
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
            : player.GetSelectedSlot();

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
        entry.position = toVec3(explosive.position);
        entry.ownerPlayerId = explosive.ownerPlayerId;
        entry.ownerTeamId = explosive.ownerTeamId;
        entry.remainingTimer = explosive.timer;
        entry.radius = explosive.radius;
        entry.velocity = toVec3(explosive.velocity);
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

    const float breakReach = creativeMode_ ? kCreativeBlockInteractionReach : kDefaultBlockInteractionReach;
    const std::optional<RaycastHit> hit = RaycastFromPlayerEye(player, aimDirection, breakReach);
    if (!hit.has_value())
    {
        ResetBreakProgress();
        return;
    }

    bool isCore = false;
    std::string label = DisplayName(hit->blockData.type);
    const bool creativeSpecialBlock = creativeMode_
        && std::any_of(creativeSpecials_.begin(), creativeSpecials_.end(), [&hit](const CreativeSpecial& special)
        {
            return special.pos == hit->block
                && special.kind != CreativeSpecialKind::HeroSpawn
                && special.kind != CreativeSpecialKind::Shop;
        });
    if (hit->blockData.type == BlockType::EnergyCoreBlock && !creativeSpecialBlock)
    {
        EnergyCore* core = FindCoreAt(hit->block);
        if (core == nullptr || core->GetTeamId() == player.GetTeamId())
        {
            ResetBreakProgress();
            return;
        }
        isCore = true;
    }
    else if (!creativeSpecialBlock && (!hit->blockData.breakable || !IsBreakableByPlayers(hit->blockData.type)))
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
        std::optional<GridPos> supportBlock;
        bool haveTarget = false;
        if (const std::optional<RaycastHit> hit = RaycastFromPlayerEye(player, aimDirection, creativeMode_ ? kCreativeBlockInteractionReach : kDefaultBlockInteractionReach))
        {
            placePos = hit->adjacent;
            supportBlock = hit->block;
            haveTarget = true;
        }
        // A sneaking bot at the lip of a bridge may legitimately look past the
        // exposed side face instead of producing a stable ray hit. Recover the
        // intended adjacent support cell from the same authoritative command's
        // yaw. This remains server validated and is deliberately bot-only;
        // humans keep exact crosshair placement semantics.
        if ((!haveTarget || command.jump)
            && command.sneak
            && IsBotControlled(ControlKindForPlayer(player)))
        {
            const Vector3 position = player.GetPosition();
            const GridPos expectedSupport = world_.WorldToGrid(Vector3 {
                position.x, position.y - 1.08f, position.z });
            std::optional<GridPos> standingSupport;
            float bestSupportDistance = std::numeric_limits<float>::max();
            for (int dy = 1; dy >= -2; --dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    for (int dz = -1; dz <= 1; ++dz)
                    {
                        const GridPos candidate {
                            expectedSupport.x + dx, expectedSupport.y + dy, expectedSupport.z + dz };
                        if (!world_.IsSolid(candidate)) continue;
                        const Vector3 center = world_.GridToWorld(candidate);
                        const float distance = (center.x - position.x) * (center.x - position.x)
                            + (center.z - position.z) * (center.z - position.z)
                            + static_cast<float>(std::abs(dy)) * 0.25f;
                        if (distance < bestSupportDistance)
                        {
                            bestSupportDistance = distance;
                            standingSupport = candidate;
                        }
                    }
                }
            }
            if (standingSupport.has_value())
            {
                const int stepX = std::fabs(aimDirection.x) >= std::fabs(aimDirection.z)
                    ? (aimDirection.x >= 0.0f ? 1 : -1) : 0;
                const int stepZ = stepX == 0 ? (aimDirection.z >= 0.0f ? 1 : -1) : 0;
                const GridPos inferred {
                    standingSupport->x + stepX,
                    standingSupport->y + (command.jump ? 1 : 0),
                    standingSupport->z + stepZ };
                if (world_.IsAir(inferred))
                {
                    placePos = inferred;
                    supportBlock = *standingSupport;
                    haveTarget = true;
                }
            }
        }
        if (haveTarget)
        {
            const BlockActionResult result = ApplyPlaceBlockForPlayer(player, placePos, supportBlock);
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
        state.placeCooldown = 0.22f;
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

    // Creative break rhythm (Minecraft-style): holding destroy paces breaks
    // at one per 0.3 s, while fresh clicks bypass the pause entirely — spam
    // clicking breaks exactly as fast as the player clicks.
    if (creativeMode_)
    {
        if (command.attackPressed)
        {
            state.creativeBreakCooldown = 0.0f;
        }
        else
        {
            state.creativeBreakCooldown = std::max(0.0f, state.creativeBreakCooldown - dt);
        }
        if (state.creativeBreakCooldown > 0.0f)
        {
            resetBreakProgressForPlayer();
            return;
        }
    }

    // Don't mine while an enemy is lined up for melee (mirror the local path).
    if (weapon.has_value()
        && combat_.FindMeleeTarget(player, players_, aimDirection, *weapon, 1.0f, meleeRayLimit).has_value())
    {
        resetBreakProgressForPlayer();
        return;
    }

    const float breakReach = creativeMode_ ? kCreativeBlockInteractionReach : kDefaultBlockInteractionReach;
    const std::optional<RaycastHit> hit = RaycastFromPlayerEye(player, aimDirection, breakReach);
    if (!hit.has_value())
    {
        resetBreakProgressForPlayer();
        return;
    }

    bool isCore = false;
    std::string label = DisplayName(hit->blockData.type);
    const bool creativeSpecialBlock = creativeMode_
        && std::any_of(creativeSpecials_.begin(), creativeSpecials_.end(), [&hit](const CreativeSpecial& special)
        {
            return special.pos == hit->block
                && special.kind != CreativeSpecialKind::HeroSpawn
                && special.kind != CreativeSpecialKind::Shop;
        });
    if (hit->blockData.type == BlockType::EnergyCoreBlock && !creativeSpecialBlock)
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
    // The creative EDITOR edits the raw map: imported/authored blocks carry
    // breakable=0 as MATCH data, but the editor must still remove them (and
    // barriers).  Test-play and real matches keep the protection.
    else if (!creativeMode_
        && !creativeSpecialBlock
        && (!hit->blockData.breakable || !IsBreakableByPlayers(hit->blockData.type)))
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
        if (creativeMode_ && result.success)
        {
            // Arm the hold-to-break pause; a fresh click clears it above.
            state.creativeBreakCooldown = 0.3f;
        }
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
    if (result.success)
    {
        const HeroVoiceEvent voiceEvent = result.playHeroVoice
            ? result.heroVoiceEvent
            : DefaultHeroVoiceEvent(result.slot);
        const int voiceIndex = static_cast<int>(voiceEvent);
        if (voiceIndex >= 0 && voiceIndex < static_cast<int>(HeroVoiceEvent::Count))
        {
            flags |= kHeroAbilityFlagHeroVoice;
            flags |= (voiceIndex & kHeroAbilityVoiceEventMask) << kHeroAbilityVoiceEventShift;
        }
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
            result.playHeroVoice = (replicated.flags & kHeroAbilityFlagHeroVoice) != 0;
            if (result.playHeroVoice)
            {
                result.heroVoiceEvent = static_cast<HeroVoiceEvent>(
                    (replicated.flags >> kHeroAbilityVoiceEventShift) & kHeroAbilityVoiceEventMask);
            }
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
            const bool voidDeath = (event.flags & 2) != 0;
            if (voidDeath)
            {
                PlayHeroVoiceForPlayerId(event.targetPlayerId, HeroVoiceEvent::VoidFall);
            }
            if (event.actorPlayerId >= 0 && event.actorPlayerId != event.targetPlayerId)
            {
                PlayHeroVoiceForPlayerId(event.actorPlayerId, HeroVoiceEvent::Kill);
            }
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
                Block { delta.newType, delta.newTeamId, IsBreakableByPlayers(delta.newType), delta.newVariant }, true);
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
        explosive.id = e.id;
        explosive.position = SnapshotVecToRay(e.position);
        explosive.velocity = SnapshotVecToRay(e.velocity);
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

int Game::RunDedicatedServerStub()
{
    networkMode_ = NetworkMode::DedicatedServer;
    std::cout << "dedicated-server: transport is a STUB (no UDP/ENet yet); config validated, "
                 "running local authoritative simulation.\n";
    return RunNetworkSmoke();
}

#include "Game.h"

#include "CrashLogger.h"
#include "Network/LocalServerSession.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetworkTransport.h"
#include "Network/SnapshotVisibility.h"
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
constexpr float kReplicatedVelocityCorrectionThreshold = 1.15f;
constexpr float kReplicatedVelocityImpulseThreshold = 1.85f;
// Beyond this gap the local prediction is too far off to smoothly reconcile
// (respawn relocation, teleport, knockback, or a missing prediction history
// entry) — snap straight to the authoritative position instead.
constexpr float kClientHardResyncDistance = 3.0f;
constexpr float kMatchStartingBarrierSeconds = 0.35f;
constexpr float kClientInterpolationDelaySeconds = 0.10f;
constexpr float kReconnectRespawnSeconds = 7.0f;
// The client predicts + sends input at the fixed sim tick rate (not the render
// frame rate), so the authoritative 60 Hz server applies ~one input per tick:
// full-speed movement, no stale-dropped command spam. Cap catch-up steps per
// frame (spiral-of-death guard) for very low frame rates.
constexpr int kMaxClientStepsPerFrame = 8;

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

        // Hidden-enemy identity: while a Likho's ultimate disguise is active, an
        // enemy recipient should see the impersonated team/hero, not the real one.
        // Record both here; the filter swaps for enemies and clears the hint.
        const HeroRuntimeState& heroState = player.GetHeroState();
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
        entry.itemType = static_cast<int>(dropped.stack.type);
        entry.count = dropped.stack.count;
        entry.position = dropped.position;
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
    for (std::size_t i = 0; i < projectiles_.size(); ++i)
    {
        const EnergyProjectile& projectile = projectiles_[i];
        ProjectileSnapshot entry;
        entry.id = static_cast<int>(i);
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
    for (std::size_t i = 0; i < timedExplosions_.size(); ++i)
    {
        const TimedExplosion& explosive = timedExplosions_[i];
        ExplosiveSnapshot entry;
        entry.id = static_cast<int>(i);
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
    for (std::size_t i = 0; i < hazardZones_.size(); ++i)
    {
        const HazardZone& zone = hazardZones_[i];
        HazardZoneSnapshot entry;
        entry.id = static_cast<int>(i);
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
    int deviceId = 0;
    const auto addDevice = [&snapshot, &deviceId](HeroDeviceSnapshot entry)
    {
        entry.id = deviceId++;
        snapshot.heroDevices.push_back(entry);
    };
    for (const BromVacuumBot& bot : bromVacuumBots_)
    {
        HeroDeviceSnapshot entry;
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
    // applied damage-over-time/markers (radon burn, konvoy intruder mark).
    int statusId = 0;
    const auto addStatus = [&snapshot, &statusId](StatusEffectSnapshot entry)
    {
        entry.id = statusId++;
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
            player->SetPosition(authoritative->position);
            player->SetVelocity(authoritative->velocity);
            player->SetYaw(authoritative->yaw);

            for (PredictedCommandState& pending : predictionHistory_)
            {
                ApplyPlayerCommand(*player, pending.command, fixedDt);
                pending.predictedPosition = player->GetPositionVec3();
                pending.predictedVelocity = player->GetVelocityVec3();
                pending.predictedYaw = player->GetYaw();
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

    const InventorySnapshot* fullInv = controlledInventory(snapshot);
    const InventorySnapshot* ownerInv = controlledInventory(ownerView);
    const InventorySnapshot* enemyInv = controlledInventory(enemyView);

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
    const bool visibilityFilterOk = enemyPlayerId >= 0
        && teamStateHidden && ownerPrivateStripped && publicSurvivesFilter;

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
                  << ",public=" << (publicSurvivesFilter ? "ok" : "FAIL") << ')'
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
    const bool ok = sameRoleProfile && movementParity && localCameraOnly;

    std::cout << "movement-parity-smoke: posError=" << positionError
              << " velError=" << velocityError
              << " localKind=" << static_cast<int>(ControlKindForPlayer(localHuman))
              << " remoteKind=" << static_cast<int>(ControlKindForPlayer(remoteHuman))
              << " parity=" << (movementParity ? "ok" : "FAIL")
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

    const bool presentationClean =
        message_ == messageBefore
        && eventMessages_.size() == eventMessagesBefore
        && worldEffects_.size() == worldEffectsBefore
        && floatingTexts_.size() == floatingTextsBefore
        && audio_.IsMuted() == audioMutedBefore;

    const bool ok = buyOk && dedupeOk && deniedFundsOk && deniedRangeOk && presentationClean;
    std::cout << "purchase-smoke: buy=" << (buyOk ? "ok" : "FAIL")
              << " (iron " << ironBefore << "->" << ironAfter
              << ", wood " << woodBefore << "->" << woodAfter << ")"
              << " dedupe=" << (dedupeOk ? "ok" : "FAIL")
              << " deniedFunds=" << (deniedFundsOk ? "ok" : "FAIL")
              << " deniedRange=" << (deniedRangeOk ? "ok" : "FAIL")
              << " presentation=" << (presentationClean ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "PURCHASE_SMOKE_OK" : "PURCHASE_SMOKE_FAIL") << std::endl;
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

void Game::ApplyNetworkPlayerActions(Player& player, const PlayerCommand& command, float dt)
{
    // B1: server-side attack / break / place for a network-controlled player.
    // Driven entirely by the command (aim = yaw/pitch) and the player's own state
    // — no camera, no single-instance local-player fields. The local-player path
    // (UpdateAttackOrBreak/HandlePlaceBlock) is untouched.
    ScopedLocalFeedbackSuppression suppressRemoteFeedback(*this, !HasLocalCamera(ControlKindForPlayer(player)));
    if (!player.IsAlive() || matchSimulation_.HasWinner())
    {
        networkActionState_.erase(player.GetId());
        return;
    }

    NetworkActionState& state = networkActionState_[player.GetId()];
    const Vector3 aimDirection = AimDirectionFromCommand(command);

    // --- Block placement (held), rate-limited per player. Placing precludes
    // attacking/breaking this tick, mirroring the local input split. ---
    const std::optional<BlockType> selectedBlock = GetSelectedBlockType(player);
    if (command.placeHeld && selectedBlock.has_value())
    {
        state.breakProgress = BreakProgress {};
        state.placeCooldown -= dt;
        if (state.placeCooldown <= 0.0f)
        {
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
                ApplyPlaceBlockForPlayer(player, placePos);
            }
            state.placeCooldown = command.bridgeMode ? 0.16f : 0.22f;
        }
        return;
    }
    state.placeCooldown = 0.0f;

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
        state.breakProgress = BreakProgress {};
        if (command.attackHeld)
        {
            player.AdvanceBowDraw(dt);
            return;
        }
        if (command.attackReleased && player.GetBowDrawTimer() > 0.0f)
        {
            LaunchBowShot(player, aimDirection, BowDrawPower(player.GetBowDrawTimer()), false);
        }
        player.ResetBowDraw();
        return;
    }
    if (blasterSelected)
    {
        state.breakProgress = BreakProgress {};
        const float fullCharge = BlasterChargeSeconds(player.GetInventory().GetBlasterRapidFireLevel());
        if (player.GetBlasterState() == CrossbowState::Loaded && command.attackPressed)
        {
            const bool aimed = rangedItem == ItemType::SniperRifle ? command.scopeHeld : command.placeHeld;
            LaunchBlasterShot(player, aimDirection, aimed, false);
            return;
        }
        if (player.GetBlasterState() == CrossbowState::Unloaded && command.attackHeld)
        {
            player.StartBlasterLoading();
        }
        if (player.GetBlasterState() == CrossbowState::Loading && command.attackHeld)
        {
            player.AdvanceBlasterLoading(dt, fullCharge);
            return;
        }
        if (command.attackReleased && player.GetBlasterState() == CrossbowState::Loading)
        {
            player.CancelBlasterLoading();
        }
        return;
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
        if (combat_.Attack(player, players_, aimDirection, message, &event, *weapon, 1.0f, nullptr, meleeRayLimit))
        {
            RegisterCombatEvent(event, message);
            state.breakProgress = BreakProgress {};
            return;
        }
    }

    // --- Block / enemy-core break (held). ---
    if (!command.attackHeld)
    {
        state.breakProgress = BreakProgress {};
        return;
    }

    // Don't mine while an enemy is lined up for melee (mirror the local path).
    if (weapon.has_value()
        && combat_.FindMeleeTarget(player, players_, aimDirection, *weapon, 1.0f, meleeRayLimit).has_value())
    {
        state.breakProgress = BreakProgress {};
        return;
    }

    const std::optional<RaycastHit> hit = RaycastFromPlayerEye(player, aimDirection, 4.5f);
    if (!hit.has_value())
    {
        state.breakProgress = BreakProgress {};
        return;
    }

    bool isCore = false;
    const float requiredSeconds = BreakSeconds(hit->blockData.type, EffectiveToolLevel(player));
    std::string label = DisplayName(hit->blockData.type);
    if (hit->blockData.type == BlockType::EnergyCoreBlock)
    {
        EnergyCore* core = FindCoreAt(hit->block);
        if (core == nullptr || core->GetTeamId() == player.GetTeamId())
        {
            state.breakProgress = BreakProgress {};
            return;
        }
        isCore = true;
        label = "Вражеский Кор";
    }
    else if (!hit->blockData.breakable || !IsBreakableByPlayers(hit->blockData.type))
    {
        state.breakProgress = BreakProgress {};
        return;
    }

    BreakProgress& progress = state.breakProgress;
    if (!progress.visible || progress.target != hit->block || progress.isCore != isCore)
    {
        progress = BreakProgress { hit->block, hit->blockData.type, true, isCore, 0.0f, label };
    }
    progress.targetType = hit->blockData.type;
    progress.isCore = isCore;
    progress.label = label;
    progress.fraction += dt / std::max(0.001f, requiredSeconds);
    if (progress.fraction >= 1.0f)
    {
        ApplyCompletedBreakProgress(player, progress);
        state.breakProgress = BreakProgress {};
    }
}

Game::PlayerActionResult Game::ApplyPlayerEconomyCommand(Player& player, const PlayerCommand& command)
{
    PlayerActionResult result {};
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
        result.message = "Action denied.";
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
            result.message = "Too far from the shop.";
            result.color = Color { 255, 130, 130, 255 };
            return result;
        }
        const int repeat = std::clamp(command.actionParamB, 1, 4);
        std::string message;
        const bool bought = TryShopPurchase(player, *team, command.actionParamA, repeat, message);
        result.success = bought;
        result.message = message.empty() ? "Purchase denied." : message;
        result.color = bought ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 };
        return result;
    }
    case PlayerActionType::DropItem:
    case PlayerActionType::MoveInventory:
    case PlayerActionType::ChestTransfer:
    case PlayerActionType::None:
    default:
        // Reserved for the next economy increment (drop / inventory move / chest
        // transfer) over this same deduped channel.
        result.message = "Unsupported action.";
        result.color = Color { 255, 130, 130, 255 };
        return result;
    }
}

void Game::QueueEconomyAction(PlayerActionType type, int paramA, int paramB)
{
    pendingEconomyActionType_ = type;
    pendingEconomyActionParamA_ = paramA;
    pendingEconomyActionParamB_ = paramB;
    ++clientEconomyActionSeq_;
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

    // Apply each client's command authoritatively to its player (the transport
    // already stamped controlledPlayerId to the client's own player).
    for (const ReceivedCommand& received : transport.DrainCommands())
    {
        Player* target = matchSimulation_.GetPlayer(static_cast<int>(received.command.controlledPlayerId));
        if (target != nullptr && target->IsAlive())
        {
            ScopedLocalFeedbackSuppression suppressServerFeedback(*this, true);
            // Movement/aim/slot, then discrete actions: hero abilities, utility
            // items, and attack/break/place (B1). These only gate on shop/inventory
            // for the local player, so a network player is never silenced by the
            // host's UI state.
            ApplyPlayerCommand(*target, received.command, dt);
            ApplyPlayerActionCommand(*target, received.command);
            UseUtilityInputs(*target, received.command);
            ApplyNetworkPlayerActions(*target, received.command, dt);
            // Discrete economy/inventory request (shop purchase, ...), deduped
            // per player so a resent command never applies twice (Phase A).
            ApplyPlayerEconomyCommand(*target, received.command);
        }
    }

    // Advance the authoritative world (bots for unclaimed players, physics,
    // combat, deaths) by one fixed step.
    UpdateMatchSimulation(dt);

    // Send each client its own visibility-filtered snapshot.
    for (int clientId : transport.ConnectedClients())
    {
        const int playerId = transport.PlayerForClient(clientId);
        transport.SendSnapshotToClient(clientId, BuildNetworkSnapshotForClient(playerId));
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

        // Pace the loop to the tick rate so the server runs in real time.
        const double elapsed = std::chrono::duration<double>(Clock::now() - tickStart).count();
        const double remaining = static_cast<double>(fixedDt) - elapsed;
        if (remaining > 0.0)
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
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
    screen_ = GameScreen::Playing;
    spectatorMode_ = false;
    inventoryOpen_ = false;
    shopOpen_ = false;

    // Enter the match in first person, like SetupMatch (Reset() selects
    // ViewMode::FirstPerson). The roster was just cleared, so there's no player
    // to anchor to yet; the exact yaw is re-synced to the assigned player on the
    // first snapshot (clientAimInitialized_ in SampleClientInput) and the
    // position is followed every frame by UpdateClientCamera. Without this the
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
    if (!hasClientFeelSnapshot_ || headless_ || networkMode_ != NetworkMode::LocalClient)
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
            // Note: selectedHotbarSlot_ is owned/predicted locally (see
            // SampleClientInput) — don't overwrite it from the laggy
            // snapshot, or a just-changed slot would flicker back for one RTT.
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
    droppedItems.clear();
    for (const DroppedItemSnapshot& d : snapshot.droppedItems)
    {
        DroppedItem dropped;
        dropped.stack.type = static_cast<ItemType>(d.itemType);
        dropped.stack.count = d.count;
        dropped.position = d.position;
        droppedItems.push_back(dropped);
    }

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
    const bool active = !clientPaused_ && IsWindowFocused();
    if (active)
    {
        currentInput_ = input_.Poll();
        // Apply mouse look locally per-frame so aim stays responsive despite RTT
        // lag; the player's position stays predicted/authoritative, not snapshot.
        cameraController_.AddLook(currentInput_.yawDelta, currentInput_.pitchDelta);

        // Hotbar slot selection (number keys / wheel). The client owns its slot
        // locally (predicted): it ships in the command, the server stores it per
        // player and echoes it in the snapshot — so we must NOT let the laggy
        // snapshot clobber a just-changed local selection (see ApplyClientSnapshot).
        if (currentInput_.hotbarSlot > 0)
        {
            const int slot = currentInput_.hotbarSlot - 1;
            if (slot >= 0 && slot < kHotbarSlotCount)
            {
                selectedHotbarSlot_ = slot;
            }
        }
        else if (std::fabs(currentInput_.mouseWheel) > 0.01f)
        {
            const int direction = currentInput_.mouseWheel > 0.0f ? -1 : 1;
            selectedHotbarSlot_ =
                (selectedHotbarSlot_ + direction + kHotbarSlotCount) % kHotbarSlotCount;
        }
    }
    else
    {
        currentInput_ = PlayerInput {};
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
        if (localPlayer->IsAlive())
        {
            ApplyPlayerCommand(*localPlayer, command, fixedDt);
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

void Game::UpdateClientCamera(float dt)
{
    const Player* assigned = matchSimulation_.GetPlayer(networkAssignedPlayerId_);
    const bool followingControlledSelf = assigned != nullptr && assigned->IsAlive();

    const Player* follow = assigned;
    if (!followingControlledSelf)
    {
        // Spectator fallback: any alive player, else the first one.
        follow = nullptr;
        for (const Player& candidate : matchSimulation_.Players())
        {
            if (candidate.IsAlive())
            {
                follow = &candidate;
                break;
            }
        }
        if (follow == nullptr)
        {
            if (matchSimulation_.Players().empty())
            {
                return;
            }
            follow = &matchSimulation_.Players().front();
        }

        // Spectating someone else: track their replicated facing (shortest-arc).
        float yawDelta = follow->GetYaw() - cameraController_.GetYaw();
        while (yawDelta > PI) yawDelta -= 2.0f * PI;
        while (yawDelta < -PI) yawDelta += 2.0f * PI;
        cameraController_.AddLook(yawDelta, 0.0f);
    }

    // For the controlled player the camera yaw is driven by local mouse look
    // (applied in SampleClientInput) — don't fight it with the laggy snapshot
    // yaw. Just move the camera to the authoritative (replicated) position.
    cameraController_.Update(follow->GetPosition(), dt);
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

int Game::RunNetworkClient(const std::string& host, std::uint16_t port,
                           const std::string& password, double maxSeconds,
                           const LobbyUpdate& lobbyPrefs)
{
    networkMode_ = NetworkMode::LocalClient;
    clientWorldBuilt_ = false;
    networkAssignedPlayerId_ = -1;

    if (!NetworkTransportAvailable())
    {
        std::cout << "connect mode: network transport disabled at build "
                     "(DAIBED_ENABLE_NETWORK=OFF); cannot join "
                  << host << ':' << port << ".\n";
        const double until = GetTime() + 2.0;
        while (GetTime() < until && !WindowShouldClose() && !ShouldClose())
        {
            CrashLogger::Heartbeat("network-client-disabled");
            DrawClientMessageFrame("Сеть отключена в этой сборке",
                                   "Пересоберите игру с DAIBED_ENABLE_NETWORK=ON.", Color { 255, 170, 120, 255 });
        }
        return 0;
    }

    // Bounded, blocking connect (handshake retried internally). Graceful on a dead
    // server or a wrong password — render the reason for a moment, then exit 0.
    ClientTransport client;
    const bool connected = client.Connect(host, port, password, 3.0f);
    if (!connected)
    {
        const std::string detail = client.WasDenied()
            ? ("отклонено: " + LocalizeNetworkReason(client.DenyReason()))
            : ("не удалось подключиться к " + host + ':' + std::to_string(port) + " - "
               + (client.LastError().empty() ? LocalizeNetworkReason("connect timed out") : client.LastError()));
        std::cout << "connect mode: " << detail << '\n';
        const double until = GetTime() + 2.5;
        while (GetTime() < until && !WindowShouldClose() && !ShouldClose())
        {
            CrashLogger::Heartbeat("network-client-connect-failed");
            DrawClientMessageFrame("Не удалось подключиться", detail, Color { 255, 150, 130, 255 });
        }
        client.Disconnect();
        return 0;
    }

    // Announce ourselves to the lobby with the caller's preferences (CLI flags),
    // defaulting the display name when none was provided.
    LobbyUpdate update = lobbyPrefs;
    if (update.playerName.empty())
    {
        update.playerName = "Игрок " + std::to_string(client.LobbyClientId());
    }
    client.SendLobbyUpdate(update);
    std::cout << "connect mode: connected to " << host << ':' << port
              << " as lobbyClientId=" << client.LobbyClientId() << ". Rendering...\n";

    const double startTime = GetTime();
    bool disconnected = false;
    std::string disconnectDetail;
    clientPaused_ = false;
    bool reconnecting = false;
    while (!WindowShouldClose() && !ShouldClose())
    {
        CrashLogger::Heartbeat(client.InMatch() ? "network-client-match" : "network-client-lobby");
        if (maxSeconds > 0.0 && GetTime() - startTime >= maxSeconds)
        {
            break;
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
                disconnected = true;
                disconnectDetail = "отклонено: " + LocalizeNetworkReason(client.DenyReason());
                break;
            }
            reconnecting = true;
            if (IsKeyPressed(KEY_ESCAPE))
            {
                break;
            }
            DrawClientMessageFrame(
                "Переподключение",
                client.TimedOut() ? "Ожидание сервера. Esc - выйти из матча."
                                  : "Восстановление сессии. Esc - выйти из матча.",
                Color { 255, 225, 150, 255 });
            continue;
        }
        if (reconnecting)
        {
            LobbyUpdate restore = update;
            restore.ready = true;
            restore.startRequested = false;
            client.SendLobbyUpdate(restore);
            reconnecting = false;
            clientPaused_ = false;
            clientAimInitialized_ = false;
        }

        const float dt = GetFrameTime();
        if (client.InMatch())
        {
            if (!clientWorldBuilt_)
            {
                networkAssignedPlayerId_ = client.AssignedPlayerId();
                localPlayerId_ = networkAssignedPlayerId_;
                BuildClientWorld(client.LatestLobbySnapshot());
                clientWorldBuilt_ = true;
                clientPaused_ = false;
                clientAimInitialized_ = false;
                DisableCursor(); // capture the mouse for FPS-style look
                std::cout << "connect mode: match started as playerId=" << networkAssignedPlayerId_ << ".\n";
            }

            // ESC pauses (our input is zeroed so the character stops); a second ESC
            // while paused leaves the match, Enter resumes.
            if (IsKeyPressed(KEY_ESCAPE))
            {
                if (clientPaused_)
                {
                    break;
                }
                clientPaused_ = true;
                EnableCursor();
            }
            else if (clientPaused_ && IsKeyPressed(KEY_ENTER))
            {
                clientPaused_ = false;
                DisableCursor();
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
            SampleClientInput();
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
            UpdateClientCamera(dt);

            const bool waitingForSnapshot = !client.HasSnapshot()
                || client.SnapshotAgeSeconds() > 1.0f;
            if (waitingForSnapshot)
            {
                DrawClientMessageFrame(
                    "Синхронизация",
                    "Получаем свежее базовое состояние от сервера.",
                    Color { 112, 232, 255, 255 });
            }
            else if (clientPaused_)
            {
                DrawClientMessageFrame("Пауза", "Esc: выйти    Enter: продолжить",
                                       Color { 235, 225, 150, 255 });
            }
            else
            {
                Render();
            }
        }
        else
        {
            // Pre-match lobby: ESC leaves before the match begins.
            if (IsKeyPressed(KEY_ESCAPE))
            {
                break;
            }
            HandleNetworkLobbyControls(client, update, client.LatestLobbySnapshot(),
                                       input_.IsDevKeyboard());
            RenderNetworkLobby(client.LatestLobbySnapshot(), client.LobbyClientId(), update);
        }
    }

    client.Disconnect();
    EnableCursor();

    if (disconnected)
    {
        const double until = GetTime() + 2.5;
        while (GetTime() < until && !WindowShouldClose() && !ShouldClose())
        {
            CrashLogger::Heartbeat("network-client-disconnected");
            DrawClientMessageFrame("Отключено от сервера", disconnectDetail, Color { 255, 200, 120, 255 });
        }
    }

    std::cout << "connect mode: left (rx=" << client.PacketsReceived()
              << " tx=" << client.PacketsSent() << ").\n";
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
            UpdateClientCamera(fixedDt);
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
    UseUtilityInputs(controlled, fireballCommand);
    const bool fireballSpawned = projectiles_.size() == projectilesBeforeFireball + 1
        && projectiles_.back().kind == ProjectileKind::Fireball;
    const bool fireballAimOk = fireballSpawned
        && directionMatches(projectiles_.back(), AimDirectionFromCommand(fireballCommand), 0.999f);

    projectiles_.clear();
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

    projectiles_.clear();

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

    networkControlledPlayerIds_.clear();
    networkActionState_.clear();

    const bool ok = fireballSpawned && fireballAimOk
        && bowSpawned && bowAimOk
        && blasterSpawned && blasterAimOk;
    std::cout << "network-ranged-smoke: fireball spawned="
              << (fireballSpawned ? "yes" : "no")
              << " aim=" << (fireballAimOk ? "ok" : "FAIL")
              << " bow spawned=" << (bowSpawned ? "yes" : "no")
              << " aim=" << (bowAimOk ? "ok" : "FAIL")
              << " blaster spawned=" << (blasterSpawned ? "yes" : "no")
              << " aim=" << (blasterAimOk ? "ok" : "FAIL") << '\n';
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
    const bool radonPulseApplied = ApplyPlayerActionCommand(controlled, radonPulseCmd);
    const bool radonPulseWorked = radonPulseApplied && enemy->GetHealth() < radonPulseHpBefore;

    const std::size_t radonProjectilesBefore = projectiles_.size();
    PlayerCommand radonMolotovCmd = base;
    radonMolotovCmd.useAbility2 = true;
    const bool radonMolotovApplied = ApplyPlayerActionCommand(controlled, radonMolotovCmd);
    const bool radonMolotovWorked = radonMolotovApplied && projectiles_.size() == radonProjectilesBefore + 1;

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

    controlled.SetHeroId(HeroId::Svidetel);
    const std::size_t echoesBefore = svidetelEchoes_.size();
    PlayerCommand svidetelEchoCmd = base;
    svidetelEchoCmd.useAbility1 = true;
    const bool svidetelEchoApplied = ApplyPlayerActionCommand(controlled, svidetelEchoCmd);
    const bool svidetelEchoWorked = svidetelEchoApplied && svidetelEchoes_.size() == echoesBefore + 1;

    const GridPos phaseCell = world_.WorldToGrid(Vector3 {
        eye.x + forward.x * 3.0f, eye.y, eye.z + forward.z * 3.0f });
    world_.PlaceBlock(phaseCell, Block { BlockType::StoneBlock, -1, true }, true);
    const std::size_t phaseBlocksBefore = svidetelPhaseBlocks_.size();
    PlayerCommand svidetelPhaseCmd = base;
    svidetelPhaseCmd.useAbility2 = true;
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

    const bool heroAbilityWorked = radonPulseWorked && radonMolotovWorked
        && hero1Worked && hero2Worked
        && svidetelEchoWorked && svidetelPhaseWorked && svidetelUltimateWorked
        && konvoyTrapWorked && konvoyTetherWorked && konvoyDomeWorked
        && bromVacuumWorked && bromTurretWorked;

    // --- Utility: a remote dash mutates authoritative movement only. ---
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
    const int enemyHpBefore = enemy->GetHealth();
    PlayerCommand attackCmd = base;
    attackCmd.attackPressed = true;
    ApplyNetworkPlayerActions(controlled, attackCmd, 0.05f);
    const int enemyHpAfter = enemy->GetHealth();
    const bool meleeWorked = enemyHpAfter < enemyHpBefore;

    // Park the enemy far away so it can't interfere with the build tests.
    enemy->SetPosition(Vec3 { 100.0f, 40.0f, 100.0f });

    // --- Place: with a block selected and an anchor in front, a block appears. ---
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
    for (int i = 0; i < 600 && !breakWorked; ++i)
    {
        ApplyNetworkPlayerActions(controlled, breakCmd, 0.05f);
        if (world_.GetBlocks().size() < blocksAfterPlace)
        {
            breakWorked = true;
        }
    }

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
              << " | blocks " << blocksBeforePlace << "->" << blocksAfterPlace
              << " heroAbility=" << (heroAbilityWorked ? "ok" : "FAIL")
              << " dash=" << (dashWorked ? "ok" : "FAIL")
              << " place=" << (placeWorked ? "ok" : "FAIL")
              << " break=" << (breakWorked ? "ok" : "FAIL")
              << " presentation=" << (presentationSuppressed ? "ok" : "FAIL")
              << " localCombat=" << (localCombatFeedbackSuppressed ? "ok" : "FAIL")
              << " audioRestore=" << (audioRestored ? "ok" : "FAIL") << '\n';

    const bool ok = meleeWorked && heroAbilityWorked && dashWorked && placeWorked && breakWorked
        && presentationSuppressed && localCombatFeedbackSuppressed && audioRestored;
    std::cout << (ok ? "NETWORK_ACTIONS_SMOKE_OK" : "NETWORK_ACTIONS_SMOKE_FAIL") << std::endl;
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

    const bool ok = projectileApplied && projectileVisualAdvanced && explosiveApplied && hazardApplied
        && devicesApplied && statusesApplied && animationOk && ownKnockbackVelocityApplied;

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
              << " explosive=" << (explosiveApplied ? "ok" : "FAIL")
              << " hazard=" << (hazardApplied ? "ok" : "FAIL")
              << " devices=" << (devicesApplied ? "ok" : "FAIL")
              << " statuses=" << (statusesApplied ? "ok" : "FAIL")
              << " animation=" << (animationOk ? "ok" : "FAIL")
              << " (apply=" << (animationApplied ? "ok" : "FAIL")
              << " ownLocal=" << (ownLocomotionLocal ? "ok" : "FAIL") << ")"
              << " ownKnockback=" << (ownKnockbackVelocityApplied ? "ok" : "FAIL")
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

#pragma once

#include "Network/BlockDelta.h"
#include "Simulation/MatchPhase.h"
#include "Simulation/SimMath.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Public, replication-oriented view of the match. Raylib-free so the server can
// build it headless and a future transport can serialize it (spatial fields use
// the raylib-free Vec3). See docs/NETWORK_PREP_PLAN.md.
//
// Visibility model (see SnapshotVisibility.h for the per-client filter):
//   - public state      — every client may see it (player positions, cores,
//                          generators, world items, projectiles, explosions).
//   - team state        — only the owner's team (hidden traps, tethers, the
//                          Likho bleed/disguise markers); SnapshotVisibility::OwnerTeam.
//   - owner-private state— only the owning player (their inventory, exact buff
//                          timers); SnapshotVisibility::Private.
//   - hidden enemy state — what an enemy must NOT learn (the disguise marker; a
//                          disguised Likho's real identity). The filter drops or
//                          rewrites it for enemy recipients.
// BuildNetworkSnapshot() builds the FULL snapshot (all of the above) for the
// server/debug; FilterSnapshotForClient() derives a per-recipient view.

// A single hotbar item slot (itemType mirrors ItemType).
struct ItemStackSnapshot
{
    int itemType = 0;
    int count = 0;
};

// Owner-private inventory view. Carried for every player in the FULL snapshot;
// the per-client filter clears it (present=false, contents empty) for everyone
// except the recipient, so a client never sees another player's inventory.
struct InventorySnapshot
{
    bool present = false;          // false once stripped by the visibility filter.
    std::array<int, 3> resources {}; // iron / gold / crystal counts.
    std::vector<ItemStackSnapshot> hotbar;
};

// Per-player state. Position/health are public; inventory is owner-private;
// disguise* is hidden-enemy scaffolding (the filter rewrites identity for enemy
// recipients of an actively-disguised Likho).
struct PlayerSnapshot
{
    int playerId = -1;
    std::string playerName;
    int teamId = -1;
    int heroId = -1;
    Vec3 position {};
    Vec3 velocity {};
    float yaw = 0.0f;
    int health = 0;
    int maxHealth = 0;
    bool alive = false;
    bool eliminated = false;
    float respawnTimer = 0.0f;
    int selectedSlot = 0;
    // Public animation state (mirrors HeroAnimationState; stored as int to keep
    // this header free of Hero.h). The server computes it authoritatively each
    // tick (UpdateTimers + attack/ability casts); the client adopts it so remote
    // (and own) players animate — walk/run/jump/attack/cast/death — instead of
    // freezing on Idle. Public: everyone sees everyone's pose.
    int animationState = 0;
    float animationTimer = 0.0f;
    float animationDuration = 0.0f;
    InventorySnapshot inventory {};
    // Identity an enemy should see while this player is a disguised Likho
    // (ultimate active). -1 when not disguised. Allies keep the real identity;
    // the filter swaps teamId/heroId to these for enemy recipients, then clears
    // these fields so the swap itself does not leak.
    int disguiseTeamId = -1;
    int disguiseHeroId = -1;
};

// Per-core (the BedWars "bed") public state.
struct CoreSnapshot
{
    int teamId = -1;
    int health = 0;
    int maxHealth = 0;
    bool alive = false;
};

// Per-generator public state. Static today (generators don't move), but it is
// now owned by MatchSimulation so the server can replicate it directly.
// resourceType mirrors ResourceType (stored as int to keep this header free of
// the Resource.h dependency).
struct GeneratorSnapshot
{
    int resourceType = 0;
    int teamId = -1;
    Vec3 position {};
};

// A resource pickup lying in the world. resourceType mirrors ResourceType.
struct PickupSnapshot
{
    int resourceType = 0;
    int amount = 0;
    Vec3 position {};
};

// A dropped inventory stack lying in the world. itemType mirrors ItemType.
struct DroppedItemSnapshot
{
    int itemType = 0;
    int count = 0;
    Vec3 position {};
};

// ---- Dynamic entities --------------------------------------------------------
//
// Replicated state for the short-lived gameplay entities (projectiles, timed
// explosives, hazard zones, hero devices) and per-player status effects. These
// live in Game today (some still use raylib Vector3); BuildNetworkSnapshot
// converts spatial fields to Vec3 at the boundary. Only public state is carried
// — no internal AI/nav/timing scratch.
//
// On entity ids: these gameplay entities do not yet carry stable per-spawn ids
// in the simulation. Until they do, `id` is the entity's index within its source
// collection at snapshot time (stable within a tick, NOT across ticks). A later
// pass that assigns real spawn-ids (needed for interpolation/prediction) will
// replace this; the field exists now so the snapshot shape is already correct.
//
// On visibility: no per-client filtering happens yet (every recipient would get
// every entry). `visibility` is scaffolding for the future public/private split
// (e.g. a buried Konvoy trap or a Likho disguise marker should only reach the
// owner team). It is recorded but NOT yet enforced — see docs/NETWORK_PREP_PLAN.md.
enum class SnapshotVisibility
{
    Public,    // every client may see it (projectile in flight, explosion).
    OwnerTeam, // only the owner's team should receive it (hidden trap, marker).
    Private    // only the affected player should receive it (exact buff timers).
};

// Hero-spawned device/marker entities. One flat type tag covers every hero's
// devices so replication has a single section instead of one vector per device.
enum class HeroDeviceType
{
    BromVacuumBot,
    BromTurretDrone,
    KonvoyTrap,
    KonvoyTether,
    KonvoyDome,
    SvidetelEcho,
    LikhoBleed,     // bleed marker on a target (visible to the owner team).
    LikhoDisguise   // disguise marker on the Likho (allies see through it).
};

// Per-player status effects (buffs/debuffs/markers). type tags the effect; the
// owner fields identify who applied it (for markers) vs. who carries it.
enum class StatusEffectType
{
    SpeedBoost,
    JumpBoost,
    Shield,
    Invulnerability,
    ControlDebuff,
    RadonProtected,
    RadonOverloaded,
    RadonBurn,
    KonvoyMark
};

struct ProjectileSnapshot
{
    int id = -1;
    int kind = 0; // mirrors ProjectileKind (Arrow/Fireball/Molotov/Blaster).
    Vec3 position {};
    Vec3 velocity {};
    int ownerPlayerId = -1;
    int ownerTeamId = -1;
    float remainingLifetime = 0.0f;
    bool fireZone = false; // leaves a fire hazard on impact (public visual cue).
    SnapshotVisibility visibility = SnapshotVisibility::Public;
};

// Timed explosive (TNT-style block), ticking down to detonation.
struct ExplosiveSnapshot
{
    int id = -1;
    Vec3 position {}; // block center.
    int ownerPlayerId = -1;
    int ownerTeamId = -1;
    float remainingTimer = 0.0f;
    float radius = 0.0f;
    SnapshotVisibility visibility = SnapshotVisibility::Public;
};

// Ground hazard area (fire / molotov pool) dealing damage over time.
struct HazardZoneSnapshot
{
    int id = -1;
    Vec3 position {};
    int ownerPlayerId = -1;
    int ownerTeamId = -1;
    float remainingLifetime = 0.0f;
    float radius = 0.0f;
    bool blueFire = false;
    SnapshotVisibility visibility = SnapshotVisibility::Public;
};

struct HeroDeviceSnapshot
{
    int id = -1;
    HeroDeviceType type = HeroDeviceType::BromVacuumBot;
    Vec3 position {};
    int ownerPlayerId = -1;
    int ownerTeamId = -1;
    int targetPlayerId = -1; // tethers/bleeds/disguise; -1 for free-standing devices.
    float remainingLifetime = 0.0f;
    int health = 0; // device HP where it has one (0 if not applicable).
    SnapshotVisibility visibility = SnapshotVisibility::Public;
};

struct StatusEffectSnapshot
{
    int id = -1;
    StatusEffectType type = StatusEffectType::SpeedBoost;
    Vec3 position {}; // carrier's position (so a viewer can place the effect).
    int targetPlayerId = -1; // player carrying the effect.
    int ownerPlayerId = -1;  // player who applied it (markers); -1 if self.
    int ownerTeamId = -1;
    float remaining = 0.0f;
    int amount = 0; // public scalar: stacks / disguise team id / level.
    SnapshotVisibility visibility = SnapshotVisibility::Private;
};

struct MatchSnapshot
{
    std::uint32_t tick = 0;
    std::uint32_t lastProcessedCommandTick = 0;
    float matchTime = 0.0f;
    MatchPhase phase = MatchPhase::Lobby;
    int winnerTeamId = -1;

    std::vector<PlayerSnapshot> players;
    std::vector<CoreSnapshot> cores;
    std::vector<GeneratorSnapshot> generators;
    // World items the server replicates. No visibility filtering yet — every
    // client would see all of these; per-client filtering (cull by distance /
    // team / fog) lands later, gating which entries are included per recipient.
    std::vector<PickupSnapshot> pickups;
    std::vector<DroppedItemSnapshot> droppedItems;
    // Block-level world replication. This is a rolling delta/event stream, not
    // a full World dump; setup/static map data is still out of this snapshot.
    std::vector<BlockDelta> blockDeltas;

    // Dynamic gameplay entities. No visibility filtering yet — every recipient
    // would get every entry; each carries owner/team + a `visibility` tag for
    // the future per-client public/private split.
    std::vector<ProjectileSnapshot> projectiles;
    std::vector<ExplosiveSnapshot> explosives;
    std::vector<HazardZoneSnapshot> hazardZones;
    std::vector<HeroDeviceSnapshot> heroDevices;
    std::vector<StatusEffectSnapshot> statusEffects;

    // Extension points for later replication passes (kept as comments so the
    // shape and intent are documented without bloating this pass):
    //   - per-player private state (inventory) under visibility filtering
};

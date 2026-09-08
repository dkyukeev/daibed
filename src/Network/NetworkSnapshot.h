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
// the raylib-free Vec3). See docs/MULTIPLAYER_TARGET_ARCHITECTURE.md.
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
    std::vector<ItemStackSnapshot> main;
};

// Owner-private hero ability HUD state (cooldowns/active timers/ultimate
// charge) PLUS ranged-weapon charge state (bow draw / blaster load). Stripped
// the same way InventorySnapshot is (present=false for everyone except the
// recipient) — no one else needs to know your exact cooldowns. Without this,
// a network client's own Player object never learns its true ability/weapon
// state at all (neither HeroRuntimeState nor the bow/blaster charge fields
// are predicted locally for a server-driven player), so its ability HUD is
// permanently stuck on "ready" and its weapon-charge HUD never animates even
// though the server is correctly tracking the charge.
struct HeroAbilityHudSnapshot
{
    bool present = false;
    float active1Cooldown = 0.0f;
    float active1ActiveTimer = 0.0f;
    float active2Cooldown = 0.0f;
    float active2ActiveTimer = 0.0f;
    float ultimateCooldown = 0.0f;
    float ultimateActiveTimer = 0.0f;
    float ultimateCharge = 0.0f;
    // Covers both Radon's `ultimatePrimed` toggle and the generic "primed"
    // flag AbilityStateText reads for the ultimate slot (Renderer.cpp only
    // ever reads HeroRuntimeState::ultimatePrimed for this, regardless of
    // hero, so one bool is enough).
    bool ultimatePrimed = false;
    float bowDrawTimer = 0.0f;
    int arrowVariant = 0;
    std::array<int, 3> arrowAmmo { 16, 12, 10 };
    std::array<float, 3> arrowReloadTimers {};
    int blasterState = 0; // mirrors CrossbowState (Unloaded/Loading/Loaded).
    float blasterLoadTimer = 0.0f;
};

// Team-scoped chest inventory. The full server snapshot carries every team
// chest; per-client filtering keeps only the recipient team's chest.
struct TeamChestSnapshot
{
    int teamId = -1;
    std::array<int, 3> resources {}; // iron / gold / crystal counts.
    std::vector<ItemStackSnapshot> slots; // all inventory slots, hotbar first.
};

// Public per-player match score. This is authoritative server state used by the
// MP scoreboard; inventory/ability privacy rules do not apply to match stats.
struct PlayerScoreSnapshot
{
    int playerId = -1;
    int kills = 0;
    int deaths = 0;
    int finalDeaths = 0;
    int coreDamage = 0;
    int coresDestroyed = 0;
};

// Owner-private discrete action/event feedback. The server emits these after
// applying exactly-once PlayerAction commands or authoritative command-side
// events such as block place/break; the visibility filter keeps only the target
// player's results. Clients dedupe by (playerId, resultSeq). actionSeq is the
// original client action id when one exists.
struct ActionResultSnapshot
{
    int playerId = -1;
    std::uint32_t resultSeq = 0;
    std::uint32_t actionSeq = 0;
    int actionType = 0;
    int subjectType = 0;
    int actorPlayerId = -1;
    int targetPlayerId = -1;
    int targetTeamId = -1;
    int amount = 0;
    int flags = 0;
    bool success = false;
    Vec3 position {};
    std::string message;
    std::array<int, 4> color { 255, 255, 255, 255 };
    float seconds = 1.6f;
    // World-effect radius (world units). Trailing/optional: 0 for action types
    // that don't carry a visual effect (defaults keep every existing positional
    // ActionResultSnapshot{...} construction compiling unchanged). Currently
    // used by PlayerActionType::HeroAbility to replicate the primary cast
    // effect's size; see PushHeroAbilityActionResultSnapshot.
    float radius = 0.0f;
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
    HeroAbilityHudSnapshot abilityHud {};
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
    int id = -1;
    int itemType = 0;
    int count = 0;
    Vec3 position {};
    Vec3 velocity {};
    int ownerPlayerId = -1;
    float ownerPickupDelay = 0.0f;
    float lifetime = 45.0f;
    float age = 0.0f;
};

// ---- Dynamic entities --------------------------------------------------------
//
// Replicated state for the short-lived gameplay entities (projectiles, timed
// explosives, hazard zones, hero devices) and per-player status effects. These
// live in Game today (some still use raylib Vector3); BuildNetworkSnapshot
// converts spatial fields to Vec3 at the boundary. Only public state is carried
// — no internal AI/nav/timing scratch.
//
// On entity ids: PROJECTILES, EXPLOSIVES, HAZARD ZONES and HERO DEVICES now
// carry a real stable per-spawn id (EnergyProjectile::id/TimedExplosion::id/
// HazardZone::id, and each of the 7 device structs' own id field, assigned
// once by Game::NextProjectileId()/NextExplosiveId()/NextHazardZoneId()/
// NextHeroDeviceId() at creation — the 7 device structs share ONE id-space
// since they all funnel into HeroDeviceSnapshot) — SnapshotDelta already
// matched entries by `id` (FindById/UpsertById/RemoveIds), so this alone fixes
// a real bug: with the old index-based id, an earlier entity expiring shifted
// every later entity's id, so the delta saw a spurious remove+add instead of a
// smooth continuation for a still-live entity (exactly the failure mode
// docs/MULTIPLAYER_QUALITY_TARGET.md's "Что считается провалом" section calls
// out). LikhoDisguise is the one device type with no persistent spawned
// object (synthesized fresh each snapshot from player state) — it derives a
// stable id directly from ownerPlayerId instead, offset clear of the shared
// counter's range. STATUS EFFECTS now carry stable ids too, but via a THIRD
// strategy: unlike everything above, a status effect is never "spawned" at
// all — it's synthesized fresh every BuildNetworkSnapshot call straight from
// Player timers / RadonBurn / KonvoyIntruderMark (it "exists" for a tick
// purely because some remaining/timer is > 0). So its id is a pure function
// of (type, targetPlayerId, ownerPlayerId) — see StatusEffectStableId in
// GameNetwork.cpp — rather than an assigned-once counter value or a
// derived-from-owner special case; the same conceptual effect (e.g. "player
// 3's shield") then keeps the same id every tick it's present. All 6 dynamic
// entity sections now have stable, index-independent ids.
//
// On visibility: no per-client filtering happens yet (every recipient would get
// every entry). `visibility` is scaffolding for the future public/private split
// (e.g. a buried Konvoy trap or a Likho disguise marker should only reach the
// owner team). See docs/MULTIPLAYER_TARGET_ARCHITECTURE.md.
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
    int arrowVariant = 0; // mirrors ArrowVariant for bow projectiles.
    Vec3 position {};
    Vec3 velocity {};
    int ownerPlayerId = -1;
    int ownerTeamId = -1;
    float remainingLifetime = 0.0f;
    bool fireZone = false; // leaves a fire hazard on impact (public visual cue).
    SnapshotVisibility visibility = SnapshotVisibility::Public;
};

// Timed explosive (falling TNT), ticking down to detonation.
struct ExplosiveSnapshot
{
    int id = -1;
    Vec3 position {};
    int ownerPlayerId = -1;
    int ownerTeamId = -1;
    float remainingTimer = 0.0f;
    float radius = 0.0f;
    SnapshotVisibility visibility = SnapshotVisibility::Public;
    Vec3 velocity {};
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

// Public broadcast event: unlike ActionResultSnapshot (owner-private, filtered
// to one recipient), every connected client receives every WorldEventSnapshot
// unfiltered — kill feed, death/respawn/victory announcements, and other
// map-wide feedback singleplayer/host gets "for free" by running
// UpdateMatchSimulation locally, which a network client never calls at all.
// Deduped client-side by a single monotonic eventSeq high-water mark (no
// per-player keying needed: the stream itself is global, not per-recipient).
// Most message text is reconstructed client-side from these fields. Chat is
// the deliberate exception: its validated UTF-8 display line uses `cause`.
enum class WorldEventKind
{
    PlayerDied,
    PlayerRespawnLost, // lost respawn protection while core already destroyed.
    PlayerRespawned,
    GeneratorBoost,
    AlarmTriggered,
    ResourcePickup,
    ItemPickup, // dropped inventory stack picked up (subjectType mirrors ItemType).
    ChatMessage // validated display text is carried in cause.
};

struct WorldEventSnapshot
{
    std::uint32_t eventSeq = 0;
    int kind = 0; // WorldEventKind
    int actorPlayerId = -1;  // killer / picking player / -1 for none/environment.
    int targetPlayerId = -1; // victim / respawning / affected player.
    int targetTeamId = -1;   // affected/owner team (victory winner, alarm owner).
    Vec3 position {};
    int subjectType = 0; // ResourceType for ResourcePickup; unused otherwise.
    int amount = 0;       // pickup amount / damage-tick amount.
    int flags = 0;        // bit0 finalDeath, bit1 voidDeath (PlayerDied only).
    // Death cause text (e.g. "падение в воид", or a DamageCredit::cause like
    // "топором Свидетеля") — free text from combat state that isn't otherwise
    // replicated, needed for the affected player's own death-overlay text.
    std::string cause;
};

struct MatchSnapshot
{
    std::uint32_t tick = 0;
    std::uint32_t lastProcessedCommandTick = 0;
    float matchTime = 0.0f;
    MatchPhase phase = MatchPhase::Lobby;
    int winnerTeamId = -1;

    std::vector<PlayerSnapshot> players;
    std::vector<PlayerScoreSnapshot> matchScores;
    std::vector<CoreSnapshot> cores;
    std::vector<TeamChestSnapshot> teamChests;
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
    std::vector<ActionResultSnapshot> actionResults;
    std::vector<WorldEventSnapshot> worldEvents;

    // Extension points for later replication passes (kept as comments so the
    // shape and intent are documented without bloating this pass):
    //   - per-player private state (inventory) under visibility filtering
};

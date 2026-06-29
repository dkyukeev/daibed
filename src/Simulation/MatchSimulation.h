#pragma once

#include "Core.h"
#include "Generator.h"
#include "Inventory.h"
#include "Network/BlockDelta.h"
#include "Network/PlayerCommand.h"
#include "Resource.h"
#include "Simulation/MatchPhase.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Player is NOT raylib-free yet (raylib Vector3 in its signatures + World/Hero
// includes), so it is only forward-declared here: MatchSimulation references
// the players vector (owned by Game for now) without pulling raylib into this
// header. Promoting to true ownership is the next step once Player is
// raylib-free. See docs/NETWORK_PREP_PLAN.md.
class Player;

// Phase 0.1A of the server-authoritative split (see docs/NETWORK_PREP_PLAN.md).
//
// MatchSimulation is the first real simulation boundary: it owns the
// authoritative simulation clock, the fixed-step configuration and the
// server-side command intake queue. It is intentionally raylib-free (no
// raylib.h, Draw*, Camera, audio or UI) so a future dedicated server can own it
// without the renderer.
//
// Phase 0.1B: it also owns the authoritative match clock (elapsed match time in
// seconds). Phase 0.1D/this phase: it now also owns the resource generators
// (raylib-free; Generator stores a Vec3). It does NOT yet own the
// world/players/teams/cores/pickups — Game still owns those. Moving that
// ownership in is the next step; for now this is a stable "simulation clock +
// command + generators boundary" everything else synchronizes to.
class MatchSimulation
{
public:
    explicit MatchSimulation(int tickRate = 60);

    // --- Clock -----------------------------------------------------------
    // Reset clock/queue + match outcome (winner cleared, phase -> Lobby). The
    // owned entity containers have their own Reset*() below. Config is kept.
    void Reset();
    void AdvanceTick();
    std::uint32_t CurrentTick() const;

    // --- Match outcome / lifecycle (owned here) -------------------------
    MatchPhase Phase() const;
    void SetPhase(MatchPhase phase);
    bool HasWinner() const;
    std::optional<int> Winner() const;
    int WinnerTeamId() const; // value_or(-1)
    // Setting a concrete winner also moves the phase to Finished. Setting
    // nullopt clears the winner but leaves the phase unchanged.
    void SetWinner(std::optional<int> winner);

    // Authoritative match clock. AdvanceClock is stepped by the simulation only
    // while the match is live (Game gates it on "no winner yet").
    void AdvanceClock(float dt);
    float MatchTimeSeconds() const;
    // Client-only: set the match clock directly from a replicated snapshot (the
    // client does not run AdvanceClock). See docs/NETWORK_PREP_PLAN.md (0.1T).
    void SetMatchTimeSeconds(float seconds);

    // --- Fixed-step configuration ---------------------------------------
    int TickRate() const;
    float FixedDeltaSeconds() const;

    // --- Server-authoritative command intake ----------------------------
    // Commands the simulation will apply on the current tick. Fed from the
    // transport/session layer (LocalServerSession today, real transport later).
    void SubmitCommand(const PlayerCommand& command);
    std::vector<PlayerCommand> DrainCommands();
    std::size_t PendingCommandCount() const;

    // --- World items owned here (generators / pickups / dropped items) ---
    // Reset*() clear a set (called at match setup, before map builders / drop
    // logic repopulate via the mutable accessors). The tick/collection logic
    // stays in Game (it couples to players/inventory/effects) and operates on
    // these accessors; only the storage moved here. The per-team forge bonus
    // depends on Game's Team data, so UpdateGenerators takes it as a callable.
    void ResetGenerators();
    std::vector<Generator>& Generators();
    const std::vector<Generator>& Generators() const;

    void ResetPickups();
    std::vector<ResourcePickup>& Pickups();
    const std::vector<ResourcePickup>& Pickups() const;

    void ResetDroppedItems();
    std::vector<DroppedItem>& DroppedItems();
    const std::vector<DroppedItem>& DroppedItems() const;

    // EnergyCores (the BedWars "beds"). Raylib-free, so owned here. Mutation
    // (Damage/Repair) is still driven from Game via the mutable accessor.
    void ResetCores();
    std::vector<EnergyCore>& Cores();
    const std::vector<EnergyCore>& Cores() const;

    // --- World block replication deltas ---------------------------------
    // Rolling per-tick block changes. The buffer is copied into MatchSnapshot
    // and then cleared through the published snapshot tick by the publisher.
    void RecordBlockDelta(const BlockDelta& delta);
    const std::vector<BlockDelta>& BlockDeltas() const;
    void ClearBlockDeltasThrough(std::uint32_t tick);
    void ResetBlockDeltas();

    // --- Players (authoritative access; Phase 6A) -----------------------
    // Game injects its players vector via SetPlayers(); MatchSimulation is the
    // access point everything else goes through (Players()/GetPlayer()).
    // Storage stays in Game until Player is raylib-free (then 6B moves it here).
    void SetPlayers(std::vector<Player>* players);
    std::vector<Player>& Players();
    const std::vector<Player>& Players() const;
    Player* GetPlayer(int id);
    const Player* GetPlayer(int id) const;

    template <typename ForgeBonusFn>
    void UpdateGenerators(float dt, std::vector<ResourcePickup>& pickups, ForgeBonusFn forgeBonusForTeam)
    {
        for (Generator& generator : generators_)
        {
            generator.Update(dt, pickups, forgeBonusForTeam(generator.GetTeamId()));
        }
    }

private:
    int tickRate_;
    float fixedDt_;
    std::uint32_t tick_ = 0;
    float matchTime_ = 0.0f;
    MatchPhase phase_ = MatchPhase::Lobby;
    std::optional<int> winner_;
    std::vector<PlayerCommand> commandQueue_;
    std::vector<Generator> generators_;
    std::vector<ResourcePickup> pickups_;
    std::vector<DroppedItem> droppedItems_;
    std::vector<EnergyCore> cores_;
    std::vector<BlockDelta> blockDeltas_;
    std::vector<Player>* players_ = nullptr; // owned by Game (6A); see note above
};

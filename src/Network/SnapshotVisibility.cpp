#include "Network/SnapshotVisibility.h"

// See SnapshotVisibility.h / docs/MULTIPLAYER_TARGET_ARCHITECTURE.md for the policy. This
// stays raylib-free and depends only on the snapshot value type.

namespace
{
// Find the recipient's REAL team from the unfiltered player list. A client always
// knows its own team truthfully (disguise never applies to oneself), so this is
// read before any identity rewrite. Returns -1 for an unknown/spectator client,
// which then only receives public state.
int FindClientTeamId(const MatchSnapshot& full, int clientPlayerId)
{
    for (const PlayerSnapshot& player : full.players)
    {
        if (player.playerId == clientPlayerId)
        {
            return player.teamId;
        }
    }
    return -1;
}
} // namespace

MatchSnapshot FilterSnapshotForClient(const MatchSnapshot& full, int clientPlayerId)
{
    const int clientTeamId = FindClientTeamId(full, clientPlayerId);

    MatchSnapshot view;
    // Global / scalar match state is public.
    view.tick = full.tick;
    view.lastProcessedCommandTick = full.lastProcessedCommandTick;
    view.matchTime = full.matchTime;
    view.phase = full.phase;
    view.winnerTeamId = full.winnerTeamId;

    // Players: positions/health are public (no fog yet), so everyone is kept.
    // Owner-private inventory is stripped for non-recipients; a disguised Likho's
    // identity is rewritten for enemy recipients.
    view.players.reserve(full.players.size());
    for (const PlayerSnapshot& player : full.players)
    {
        PlayerSnapshot entry = player;
        const bool isSelf = player.playerId == clientPlayerId;
        if (!isSelf)
        {
            entry.inventory = InventorySnapshot {}; // present=false, empty.
            entry.abilityHud = HeroAbilityHudSnapshot {}; // present=false, empty.
        }

        const bool isEnemy = clientTeamId < 0 || player.teamId != clientTeamId;
        if (isEnemy && player.disguiseTeamId >= 0)
        {
            // Hidden enemy state: enemies see the impersonated identity, not the
            // real Likho. Allies (same team) keep the real identity below.
            entry.teamId = player.disguiseTeamId;
            entry.heroId = player.disguiseHeroId;
        }
        // The disguise hint itself is server-only; never expose it to clients.
        entry.disguiseTeamId = -1;
        entry.disguiseHeroId = -1;
        view.players.push_back(entry);
    }
    view.matchScores = full.matchScores;

    // Public world state.
    view.cores = full.cores;
    for (const TeamChestSnapshot& chest : full.teamChests)
    {
        if (chest.teamId == clientTeamId)
        {
            view.teamChests.push_back(chest);
        }
    }
    view.generators = full.generators;
    view.pickups = full.pickups;
    view.droppedItems = full.droppedItems;
    view.blockDeltas = full.blockDeltas;

    // Dynamic entities filtered by their visibility tag.
    for (const ProjectileSnapshot& e : full.projectiles)
    {
        if (IsVisibleToClient(e.visibility, e.ownerTeamId, e.ownerPlayerId, -1, clientPlayerId, clientTeamId))
        {
            view.projectiles.push_back(e);
        }
    }
    for (const ExplosiveSnapshot& e : full.explosives)
    {
        if (IsVisibleToClient(e.visibility, e.ownerTeamId, e.ownerPlayerId, -1, clientPlayerId, clientTeamId))
        {
            view.explosives.push_back(e);
        }
    }
    for (const HazardZoneSnapshot& e : full.hazardZones)
    {
        if (IsVisibleToClient(e.visibility, e.ownerTeamId, e.ownerPlayerId, -1, clientPlayerId, clientTeamId))
        {
            view.hazardZones.push_back(e);
        }
    }
    for (const HeroDeviceSnapshot& e : full.heroDevices)
    {
        if (IsVisibleToClient(e.visibility, e.ownerTeamId, e.ownerPlayerId, e.targetPlayerId, clientPlayerId, clientTeamId))
        {
            view.heroDevices.push_back(e);
        }
    }
    for (const StatusEffectSnapshot& e : full.statusEffects)
    {
        if (IsVisibleToClient(e.visibility, e.ownerTeamId, e.ownerPlayerId, e.targetPlayerId, clientPlayerId, clientTeamId))
        {
            view.statusEffects.push_back(e);
        }
    }
    for (const ActionResultSnapshot& result : full.actionResults)
    {
        if (result.playerId == clientPlayerId)
        {
            view.actionResults.push_back(result);
        }
    }
    // Public broadcast events (death/respawn/victory/kill feed/...): every
    // recipient gets every entry, unlike the owner-private actionResults above.
    view.worldEvents = full.worldEvents;

    return view;
}

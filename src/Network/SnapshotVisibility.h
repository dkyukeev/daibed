#pragma once

#include "Network/NetworkSnapshot.h"

// Per-client visibility filter (see docs/MULTIPLAYER_TARGET_ARCHITECTURE.md). Raylib-free and
// pure: it takes the FULL MatchSnapshot the server built and returns the subset
// a specific recipient is allowed to see. It needs nothing but the snapshot —
// the recipient's team is derived from the (unfiltered) player list — so it is
// trivially testable and reusable by a future real transport.
//
// Policy:
//   - Public      entries  -> kept for everyone.
//   - OwnerTeam   entries  -> kept only when the recipient shares the owner team
//                             (hidden traps/tethers/markers do not reach enemies).
//   - Private     entries  -> kept only for the owning/affected player.
//   - Owner-private player fields (inventory) -> kept only on the recipient's own
//                             PlayerSnapshot; stripped from everyone else.
//   - Hidden enemy identity: an actively-disguised Likho is rewritten to its
//                             disguise team/hero for enemy recipients.
// No fog/LOS culling of player positions yet (the arena renders all players to
// everyone); the structure is here so a future fog pass plugs in at the player
// loop. Svidetel "contours" are such a future team-side reveal.

// Pure visibility predicate for a tagged entity. ownerTeamId/ownerPlayerId/
// targetPlayerId are -1 when the entity type lacks that field.
inline bool IsVisibleToClient(
    SnapshotVisibility visibility,
    int ownerTeamId,
    int ownerPlayerId,
    int targetPlayerId,
    int clientPlayerId,
    int clientTeamId)
{
    switch (visibility)
    {
    case SnapshotVisibility::Public:
        return true;
    case SnapshotVisibility::OwnerTeam:
        return clientTeamId >= 0 && ownerTeamId == clientTeamId;
    case SnapshotVisibility::Private:
        return clientPlayerId >= 0
            && (clientPlayerId == ownerPlayerId || clientPlayerId == targetPlayerId);
    }
    return false;
}

// Build the recipient-specific view of `full` for the given client player id.
// `full` is expected to be the unfiltered snapshot from BuildNetworkSnapshot().
MatchSnapshot FilterSnapshotForClient(const MatchSnapshot& full, int clientPlayerId);

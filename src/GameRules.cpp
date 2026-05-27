#include "GameRules.h"

#include <set>

std::optional<int> GameRules::CheckWinCondition(const std::vector<Team>& teams, const std::vector<Player>& players) const
{
    std::set<int> teamIdsWithPlayers;
    for (const Player& player : players)
    {
        teamIdsWithPlayers.insert(player.GetTeamId());
    }

    int remainingTeams = 0;
    int winnerTeamId = -1;

    for (const Team& team : teams)
    {
        if (teamIdsWithPlayers.count(team.id) == 0)
        {
            continue;
        }

        bool hasPlayerWithLife = false;
        for (const Player& player : players)
        {
            if (player.GetTeamId() == team.id && !player.IsEliminated())
            {
                hasPlayerWithLife = true;
                break;
            }
        }

        const bool teamStillInGame = team.coreAlive || hasPlayerWithLife;
        if (teamStillInGame)
        {
            ++remainingTeams;
            winnerTeamId = team.id;
        }
    }

    if (remainingTeams == 1)
    {
        return winnerTeamId;
    }

    return std::nullopt;
}


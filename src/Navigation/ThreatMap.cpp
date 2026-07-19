#include "Navigation/ThreatMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

void ThreatMap::Clear()
{
    sources_.clear();
}

void ThreatMap::AddSource(const ThreatSource& source)
{
    if (source.radius <= 0.0f || source.cost <= 0.0f)
    {
        return;
    }
    sources_.push_back(source);
}

void ThreatMap::AddActorThreats(const std::vector<NavigationActor>& actors, int agentTeamId)
{
    std::vector<const NavigationActor*> ordered;
    ordered.reserve(actors.size());
    for (const NavigationActor& actor : actors)
    {
        if (!actor.alive || actor.teamId == agentTeamId || actor.threatRadius <= 0.0f || actor.threatCost <= 0.0f)
        {
            continue;
        }
        ordered.push_back(&actor);
    }
    std::sort(ordered.begin(), ordered.end(), [](const NavigationActor* lhs, const NavigationActor* rhs)
    {
        if (lhs->playerId != rhs->playerId)
        {
            return lhs->playerId < rhs->playerId;
        }
        if (lhs->teamId != rhs->teamId)
        {
            return lhs->teamId < rhs->teamId;
        }
        if (lhs->position.x != rhs->position.x)
        {
            return lhs->position.x < rhs->position.x;
        }
        return lhs->position.z < rhs->position.z;
    });

    for (const NavigationActor* actor : ordered)
    {
        AddSource(ThreatSource {
            actor->position,
            actor->threatRadius,
            actor->threatCost,
            actor->playerId,
            actor->teamId
        });
    }
}

float ThreatMap::CostAt(Vector3 position) const noexcept
{
    float result = 0.0f;
    for (const ThreatSource& source : sources_)
    {
        const float dx = position.x - source.position.x;
        const float dy = (position.y - source.position.y) * 0.35f;
        const float dz = position.z - source.position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance >= source.radius)
        {
            continue;
        }
        const float influence = 1.0f - distance / source.radius;
        // Smooth quadratic falloff keeps routes from oscillating at a hard ring.
        result += source.cost * influence * influence;
    }
    return result;
}

float ThreatMap::CostAt(const GridPos& support, float bodyCenterAboveSupport) const noexcept
{
    return CostAt(Vector3 {
        static_cast<float>(support.x),
        static_cast<float>(support.y) + bodyCenterAboveSupport,
        static_cast<float>(support.z)
    });
}

float ThreatMap::NearestThreatDistance(Vector3 position) const noexcept
{
    float best = std::numeric_limits<float>::infinity();
    for (const ThreatSource& source : sources_)
    {
        const float dx = position.x - source.position.x;
        const float dy = position.y - source.position.y;
        const float dz = position.z - source.position.z;
        best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return best;
}

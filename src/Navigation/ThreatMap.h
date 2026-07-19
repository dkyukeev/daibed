#pragma once

#include "Navigation/NavigationTypes.h"

#include <vector>

struct ThreatSource
{
    Vector3 position {};
    float radius = 8.0f;
    float cost = 8.0f;
    int ownerPlayerId = -1;
    int ownerTeamId = -1;
};

class ThreatMap
{
public:
    void Clear();
    void AddSource(const ThreatSource& source);
    void AddActorThreats(const std::vector<NavigationActor>& actors, int agentTeamId);

    float CostAt(Vector3 position) const noexcept;
    float CostAt(const GridPos& support, float bodyCenterAboveSupport = 1.40f) const noexcept;
    float NearestThreatDistance(Vector3 position) const noexcept;

    bool Empty() const noexcept { return sources_.empty(); }
    const std::vector<ThreatSource>& Sources() const noexcept { return sources_; }

private:
    std::vector<ThreatSource> sources_;
};

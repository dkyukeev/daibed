#include "Navigation/NavigationGoal.h"

#include "Navigation/NavigationWorldView.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace
{
float Distance(GridPos a, GridPos b)
{
    const float dx = static_cast<float>(a.x - b.x);
    const float dy = static_cast<float>(a.y - b.y);
    const float dz = static_cast<float>(a.z - b.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float HorizontalDistance(GridPos a, GridPos b)
{
    const float dx = static_cast<float>(a.x - b.x);
    const float dz = static_cast<float>(a.z - b.z);
    return std::sqrt(dx * dx + dz * dz);
}

float OctileDistance(GridPos a, GridPos b)
{
    const int dx = std::abs(a.x - b.x);
    const int dz = std::abs(a.z - b.z);
    const int diagonal = std::min(dx, dz);
    const int straight = std::max(dx, dz) - diagonal;
    return static_cast<float>(straight)
        + static_cast<float>(diagonal) * 1.41421356f;
}

std::string PositionText(const char* name, GridPos pos)
{
    std::ostringstream out;
    out << name << '(' << pos.x << ',' << pos.y << ',' << pos.z << ')';
    return out.str();
}
}

GoalReachPosition::GoalReachPosition(GridPos target) : target_(target) {}

bool GoalReachPosition::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    return state.support == target_;
}

float GoalReachPosition::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    return OctileDistance(state.support, target_)
        + static_cast<float>(std::abs(state.support.y - target_.y)) * 1.7f;
}

std::string GoalReachPosition::Describe() const
{
    return PositionText("reach", target_);
}

GoalWithinRadius::GoalWithinRadius(GridPos center, float radius)
    : center_(center), radius_(std::max(0.0f, radius)) {}

bool GoalWithinRadius::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    return Distance(state.support, center_) <= radius_;
}

float GoalWithinRadius::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    return std::max(0.0f, Distance(state.support, center_) - radius_);
}

std::string GoalWithinRadius::Describe() const
{
    std::ostringstream out;
    out << PositionText("radius", center_) << " r=" << radius_;
    return out.str();
}

GoalAdjacentToBlock::GoalAdjacentToBlock(GridPos block, int verticalTolerance)
    : block_(block), verticalTolerance_(std::max(0, verticalTolerance)) {}

bool GoalAdjacentToBlock::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    const int horizontal = std::abs(state.support.x - block_.x)
        + std::abs(state.support.z - block_.z);
    return horizontal == 1
        && std::abs((state.support.y + 1) - block_.y) <= verticalTolerance_;
}

float GoalAdjacentToBlock::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    const float horizontal = OctileDistance(state.support, block_);
    const int vertical = std::abs((state.support.y + 1) - block_.y);
    return std::max(0.0f, horizontal - 1.0f)
        + static_cast<float>(std::max(0, vertical - verticalTolerance_)) * 1.7f;
}

std::string GoalAdjacentToBlock::Describe() const
{
    return PositionText("adjacent", block_);
}

GoalAttackRangeOfPlayer::GoalAttackRangeOfPlayer(
    int playerId,
    float minimumRange,
    float maximumRange,
    bool requireLineOfSight)
    : playerId_(playerId),
      minimumRange_(std::max(0.0f, minimumRange)),
      maximumRange_(std::max(minimumRange_, maximumRange)),
      requireLineOfSight_(requireLineOfSight) {}

bool GoalAttackRangeOfPlayer::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    const NavigationActor* actor = world.FindActor(playerId_);
    if (actor == nullptr || !actor->alive)
    {
        return false;
    }
    const Vector3 from = world.SupportCenter(state.support);
    const float dx = from.x - actor->position.x;
    const float dy = from.y - actor->position.y;
    const float dz = from.z - actor->position.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return distance >= minimumRange_ && distance <= maximumRange_
        && (!requireLineOfSight_
            || world.HasLineOfSightFromSupport(state.support, actor->position));
}

float GoalAttackRangeOfPlayer::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    const NavigationActor* actor = world.FindActor(playerId_);
    if (actor == nullptr || !actor->alive)
    {
        return 100000.0f;
    }
    const GridPos target = world.WorldToGrid(actor->position);
    const float distance = Distance(state.support, target);
    if (distance < minimumRange_)
    {
        return minimumRange_ - distance;
    }
    return std::max(0.0f, distance - maximumRange_);
}

std::string GoalAttackRangeOfPlayer::Describe() const
{
    std::ostringstream out;
    out << "attack-player(" << playerId_ << ") " << minimumRange_ << ".." << maximumRange_;
    return out.str();
}

GoalLineOfSightToPlayer::GoalLineOfSightToPlayer(int playerId, float maximumRange)
    : playerId_(playerId), maximumRange_(std::max(0.0f, maximumRange)) {}

bool GoalLineOfSightToPlayer::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    const NavigationActor* actor = world.FindActor(playerId_);
    if (actor == nullptr || !actor->alive)
    {
        return false;
    }
    const GridPos actorGrid = world.WorldToGrid(actor->position);
    return Distance(state.support, actorGrid) <= maximumRange_
        && world.HasLineOfSightFromSupport(state.support, actor->position);
}

float GoalLineOfSightToPlayer::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    const NavigationActor* actor = world.FindActor(playerId_);
    if (actor == nullptr || !actor->alive)
    {
        return 100000.0f;
    }
    return std::max(0.0f,
        Distance(state.support, world.WorldToGrid(actor->position)) - maximumRange_);
}

std::string GoalLineOfSightToPlayer::Describe() const
{
    return "line-of-sight-player(" + std::to_string(playerId_) + ')';
}

GoalAnyOf::GoalAnyOf(std::vector<NavigationGoalPtr> goals) : goals_(std::move(goals)) {}
GoalAnyOf::GoalAnyOf(std::initializer_list<NavigationGoalPtr> goals) : goals_(goals) {}

bool GoalAnyOf::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    return std::any_of(goals_.begin(), goals_.end(), [&](const NavigationGoalPtr& goal)
    {
        return goal != nullptr && goal->IsSatisfied(state, world);
    });
}

float GoalAnyOf::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    float best = std::numeric_limits<float>::infinity();
    for (const NavigationGoalPtr& goal : goals_)
    {
        if (goal != nullptr)
        {
            best = std::min(best, goal->EstimateRemainingCost(state, world));
        }
    }
    return std::isfinite(best) ? best : 100000.0f;
}

std::optional<GridPos> GoalAnyOf::RepresentativePosition() const
{
    for (const NavigationGoalPtr& goal : goals_)
    {
        if (goal != nullptr && goal->RepresentativePosition().has_value())
        {
            return goal->RepresentativePosition();
        }
    }
    return std::nullopt;
}

std::string GoalAnyOf::Describe() const
{
    return "any-of(" + std::to_string(goals_.size()) + ')';
}

GoalEscapeThreat::GoalEscapeThreat(
    float safeThreatThreshold,
    std::optional<GridPos> preferredRetreatPosition)
    : safeThreatThreshold_(std::max(0.0f, safeThreatThreshold)),
      preferredRetreatPosition_(preferredRetreatPosition) {}

bool GoalEscapeThreat::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    return world.ThreatCostAt(state.support) <= safeThreatThreshold_;
}

float GoalEscapeThreat::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    const float unsafe = std::max(0.0f, world.ThreatCostAt(state.support) - safeThreatThreshold_);
    return unsafe + (preferredRetreatPosition_.has_value()
        ? HorizontalDistance(state.support, *preferredRetreatPosition_) * 0.12f
        : 0.0f);
}

std::string GoalEscapeThreat::Describe() const
{
    return "escape-threat(" + std::to_string(safeThreatThreshold_) + ')';
}

GoalDefensivePosition::GoalDefensivePosition(
    GridPos anchor,
    float minimumRadius,
    float maximumRadius,
    float maximumThreat)
    : anchor_(anchor),
      minimumRadius_(std::max(0.0f, minimumRadius)),
      maximumRadius_(std::max(minimumRadius_, maximumRadius)),
      maximumThreat_(maximumThreat) {}

bool GoalDefensivePosition::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    const float radius = HorizontalDistance(state.support, anchor_);
    return radius >= minimumRadius_ && radius <= maximumRadius_
        && world.ThreatCostAt(state.support) <= maximumThreat_;
}

float GoalDefensivePosition::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    const float radius = HorizontalDistance(state.support, anchor_);
    float result = radius < minimumRadius_
        ? minimumRadius_ - radius
        : std::max(0.0f, radius - maximumRadius_);
    result += std::max(0.0f, world.ThreatCostAt(state.support) - maximumThreat_);
    return result;
}

std::string GoalDefensivePosition::Describe() const
{
    return PositionText("defend", anchor_);
}

GoalCoreAttackPosition::GoalCoreAttackPosition(
    GridPos coreBlock,
    float interactionRange,
    bool requireLineOfSight)
    : coreBlock_(coreBlock),
      interactionRange_(std::max(1.0f, interactionRange)),
      requireLineOfSight_(requireLineOfSight) {}

bool GoalCoreAttackPosition::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView& world) const
{
    const Vector3 core = world.GridToWorld(coreBlock_);
    const Vector3 standing = world.SupportCenter(state.support);
    const float dx = core.x - standing.x;
    const float dy = core.y - standing.y;
    const float dz = core.z - standing.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return distance <= interactionRange_
        && world.IsSupported(state)
        && world.IsBodyClear(state.support, NavigationProfile {})
        && (!requireLineOfSight_
            || world.HasLineOfSightFromSupport(state.support, core, coreBlock_));
}

float GoalCoreAttackPosition::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    return std::max(0.0f, Distance(state.support, coreBlock_) - interactionRange_);
}

std::string GoalCoreAttackPosition::Describe() const
{
    return PositionText("core-attack", coreBlock_);
}

GoalResourceGenerator::GoalResourceGenerator(GridPos generatorPosition, float collectionRadius)
    : generatorPosition_(generatorPosition), collectionRadius_(std::max(0.0f, collectionRadius)) {}

bool GoalResourceGenerator::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    return Distance(state.support, generatorPosition_) <= collectionRadius_;
}

float GoalResourceGenerator::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    return std::max(0.0f, Distance(state.support, generatorPosition_) - collectionRadius_);
}

std::string GoalResourceGenerator::Describe() const
{
    return PositionText("generator", generatorPosition_);
}

GoalShopPosition::GoalShopPosition(GridPos shopPosition, float interactionRadius)
    : shopPosition_(shopPosition), interactionRadius_(std::max(0.0f, interactionRadius)) {}

bool GoalShopPosition::IsSatisfied(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    return Distance(state.support, shopPosition_) <= interactionRadius_;
}

float GoalShopPosition::EstimateRemainingCost(
    const NavigationState& state,
    const NavigationWorldView&) const
{
    return std::max(0.0f, Distance(state.support, shopPosition_) - interactionRadius_);
}

std::string GoalShopPosition::Describe() const
{
    return PositionText("shop", shopPosition_);
}

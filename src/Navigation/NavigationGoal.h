#pragma once

#include "Navigation/NavigationTypes.h"

#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class NavigationWorldView;

class NavigationGoal
{
public:
    virtual ~NavigationGoal() = default;

    virtual bool IsSatisfied(
        const NavigationState& state,
        const NavigationWorldView& world) const = 0;
    virtual float EstimateRemainingCost(
        const NavigationState& state,
        const NavigationWorldView& world) const = 0;
    virtual std::optional<GridPos> RepresentativePosition() const { return std::nullopt; }
    virtual std::string Describe() const = 0;
};

using NavigationGoalPtr = std::shared_ptr<const NavigationGoal>;

class GoalReachPosition final : public NavigationGoal
{
public:
    explicit GoalReachPosition(GridPos target);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::optional<GridPos> RepresentativePosition() const override { return target_; }
    std::string Describe() const override;
    GridPos Target() const noexcept { return target_; }

private:
    GridPos target_ {};
};

class GoalWithinRadius final : public NavigationGoal
{
public:
    GoalWithinRadius(GridPos center, float radius);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::optional<GridPos> RepresentativePosition() const override { return center_; }
    std::string Describe() const override;

private:
    GridPos center_ {};
    float radius_ = 0.0f;
};

class GoalAdjacentToBlock final : public NavigationGoal
{
public:
    explicit GoalAdjacentToBlock(GridPos block, int verticalTolerance = 1);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::optional<GridPos> RepresentativePosition() const override { return block_; }
    std::string Describe() const override;

private:
    GridPos block_ {};
    int verticalTolerance_ = 1;
};

class GoalAttackRangeOfPlayer final : public NavigationGoal
{
public:
    GoalAttackRangeOfPlayer(
        int playerId,
        float minimumRange,
        float maximumRange,
        bool requireLineOfSight = true);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::string Describe() const override;

private:
    int playerId_ = -1;
    float minimumRange_ = 0.0f;
    float maximumRange_ = 3.0f;
    bool requireLineOfSight_ = true;
};

class GoalLineOfSightToPlayer final : public NavigationGoal
{
public:
    explicit GoalLineOfSightToPlayer(int playerId, float maximumRange = 16.0f);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::string Describe() const override;

private:
    int playerId_ = -1;
    float maximumRange_ = 16.0f;
};

class GoalAnyOf final : public NavigationGoal
{
public:
    explicit GoalAnyOf(std::vector<NavigationGoalPtr> goals);
    GoalAnyOf(std::initializer_list<NavigationGoalPtr> goals);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::optional<GridPos> RepresentativePosition() const override;
    std::string Describe() const override;

private:
    std::vector<NavigationGoalPtr> goals_;
};

class GoalEscapeThreat final : public NavigationGoal
{
public:
    explicit GoalEscapeThreat(
        float safeThreatThreshold,
        std::optional<GridPos> preferredRetreatPosition = std::nullopt);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::optional<GridPos> RepresentativePosition() const override { return preferredRetreatPosition_; }
    std::string Describe() const override;

private:
    float safeThreatThreshold_ = 0.0f;
    std::optional<GridPos> preferredRetreatPosition_;
};

class GoalDefensivePosition final : public NavigationGoal
{
public:
    GoalDefensivePosition(
        GridPos anchor,
        float minimumRadius = 1.5f,
        float maximumRadius = 7.0f,
        float maximumThreat = std::numeric_limits<float>::infinity());
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::optional<GridPos> RepresentativePosition() const override { return anchor_; }
    std::string Describe() const override;

private:
    GridPos anchor_ {};
    float minimumRadius_ = 1.5f;
    float maximumRadius_ = 7.0f;
    float maximumThreat_ = std::numeric_limits<float>::infinity();
};

class GoalCoreAttackPosition final : public NavigationGoal
{
public:
    explicit GoalCoreAttackPosition(
        GridPos coreBlock,
        float interactionRange = 4.2f,
        bool requireLineOfSight = true);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::optional<GridPos> RepresentativePosition() const override { return coreBlock_; }
    std::string Describe() const override;

private:
    GridPos coreBlock_ {};
    float interactionRange_ = 4.2f;
    bool requireLineOfSight_ = true;
};

class GoalResourceGenerator final : public NavigationGoal
{
public:
    explicit GoalResourceGenerator(GridPos generatorPosition, float collectionRadius = 1.8f);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::optional<GridPos> RepresentativePosition() const override { return generatorPosition_; }
    std::string Describe() const override;

private:
    GridPos generatorPosition_ {};
    float collectionRadius_ = 1.8f;
};

class GoalShopPosition final : public NavigationGoal
{
public:
    explicit GoalShopPosition(GridPos shopPosition, float interactionRadius = 2.8f);
    bool IsSatisfied(const NavigationState& state, const NavigationWorldView& world) const override;
    float EstimateRemainingCost(const NavigationState& state, const NavigationWorldView& world) const override;
    std::optional<GridPos> RepresentativePosition() const override { return shopPosition_; }
    std::string Describe() const override;

private:
    GridPos shopPosition_ {};
    float interactionRadius_ = 2.8f;
};

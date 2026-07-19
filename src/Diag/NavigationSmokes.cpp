#include "Game.h"

#include "Navigation/NavigationController.h"
#include "Navigation/NavigationActionLibrary.h"
#include "Navigation/NavigationGoal.h"
#include "Navigation/NavigationWorldView.h"
#include "Navigation/PathExecutor.h"
#include "Navigation/RouteGraph.h"
#include "Navigation/ThreatMap.h"
#include "Navigation/VoxelPathfinder.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#if DAIBED_DIAGNOSTICS
namespace
{
constexpr float kTick = 1.0f / 60.0f;

Block GroundBlock()
{
    return Block { BlockType::Solid, -1, false };
}

void AddPlatform(World& world, int minX, int maxX, int minZ, int maxZ, int y = 0)
{
    for (int x = minX; x <= maxX; ++x)
    {
        for (int z = minZ; z <= maxZ; ++z)
        {
            world.PlaceBlock(GridPos { x, y, z }, GroundBlock());
        }
    }
}

NavigationProfile PlannerProfile()
{
    NavigationProfile profile;
    profile.canSprint = true;
    profile.canJump = true;
    profile.canSneak = true;
    profile.canPlaceBlocks = false;
    profile.canBridge = false;
    profile.canBreakBlocks = false;
    profile.maxStepHeightBlocks = 0;
    profile.maxSafeDropBlocks = 3;
    profile.maxGapJumpBlocks = 2;
    profile.availableBridgeBlocks = 0;
    profile.reserveBridgeBlocks = 0;
    return profile;
}

NavigationSearchResult Search(
    const World& world,
    GridPos start,
    const NavigationGoal& goal,
    NavigationProfile profile,
    const ThreatMap* threats = nullptr)
{
    NavigationWorldView view(world, 0, {}, threats);
    NavigationSearchLimits limits;
    limits.maxExpansions = 2400;
    limits.maxSearchRadius = 32;
    limits.maxVerticalRange = 12;
    limits.maxActions = 96;
    return VoxelPathfinder {}.FindPath(
        NavigationState { start, profile.availableBridgeBlocks, 0 },
        goal,
        view,
        profile,
        limits);
}

bool Contains(const NavigationPath& path, MovementType type)
{
    return std::any_of(path.movements.begin(), path.movements.end(), [type](const PlannedMovement& movement)
    {
        return movement.type == type;
    });
}

std::string PathFingerprint(const NavigationPath& path)
{
    std::string result;
    for (const PlannedMovement& movement : path.movements)
    {
        result += std::to_string(static_cast<int>(movement.type)) + ':'
            + std::to_string(movement.from.x) + ',' + std::to_string(movement.from.y) + ',' + std::to_string(movement.from.z)
            + '>' + std::to_string(movement.to.x) + ',' + std::to_string(movement.to.y) + ',' + std::to_string(movement.to.z) + ';';
    }
    return result;
}
}

int Game::RunNavigationSmoke()
{
    int failures = 0;
    const auto check = [&](const char* name, bool passed, const std::string& reason)
    {
        std::cout << "NAV " << name << ' ' << (passed ? "OK" : "FAIL")
                  << (reason.empty() ? "" : " - ") << reason << '\n';
        if (!passed) ++failures;
    };

    // 1. Flat route and sprint post-processing.
    {
        World world;
        AddPlatform(world, 0, 8, -1, 1);
        GoalReachPosition goal(GridPos { 8, 0, 0 });
        const NavigationSearchResult result = Search(world, GridPos { 0, 0, 0 }, goal, PlannerProfile());
        const bool passed = result.Succeeded()
            && !result.path.movements.empty()
            && std::all_of(result.path.movements.begin(), result.path.movements.end(), [](const PlannedMovement& movement)
                { return movement.type == MovementType::Walk || movement.type == MovementType::Sprint; })
            && Contains(result.path, MovementType::Sprint);
        check("flat", passed, passed ? "" : result.description);
    }

    // 2. Full block climb must be a real jump, not bot-only auto-step.
    {
        World world;
        world.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world.PlaceBlock(GridPos { 1, 1, 0 }, GroundBlock());
        world.PlaceBlock(GridPos { 2, 1, 0 }, GroundBlock());
        GoalReachPosition goal(GridPos { 2, 1, 0 });
        const NavigationSearchResult result = Search(world, GridPos { 0, 0, 0 }, goal, PlannerProfile());
        const bool passed = result.Succeeded() && Contains(result.path, MovementType::JumpUp);
        check("step-jump", passed, passed ? "" : result.description);
    }

    // Broad floors use diagonal actions instead of a Manhattan staircase.
    {
        World world;
        AddPlatform(world, 0, 7, 0, 7);
        GoalReachPosition goal(GridPos { 6, 0, 6 });
        const NavigationSearchResult result = Search(
            world, GridPos { 0, 0, 0 }, goal, PlannerProfile());
        const int diagonalActions = static_cast<int>(std::count_if(
            result.path.movements.begin(), result.path.movements.end(),
            [](const PlannedMovement& movement)
            {
                return NavigationActionLibrary::IsDiagonal(movement.from, movement.to);
            }));
        const int diagonalSprintHops = static_cast<int>(std::count_if(
            result.path.movements.begin(), result.path.movements.end(),
            [](const PlannedMovement& movement)
            {
                return movement.type == MovementType::Sprint
                    && movement.requiresJump
                    && NavigationActionLibrary::IsDiagonal(movement.from, movement.to);
            }));
        const bool passed = result.Succeeded() && diagonalActions >= 5
            && diagonalSprintHops >= 5
            && result.path.movements.size() <= 7;
        check("diagonal-floor", passed, passed ? "" : PathFingerprint(result.path));
    }

    // Diagonal movement may not cut a solid shoulder corner.
    {
        World world;
        AddPlatform(world, 0, 1, 0, 1);
        world.PlaceBlock(GridPos { 1, 1, 0 }, GroundBlock());
        world.PlaceBlock(GridPos { 1, 2, 0 }, GroundBlock());
        GoalReachPosition goal(GridPos { 1, 0, 1 });
        const NavigationSearchResult result = Search(
            world, GridPos { 0, 0, 0 }, goal, PlannerProfile());
        const bool firstCutsCorner = !result.path.movements.empty()
            && NavigationActionLibrary::IsDiagonal(
                result.path.movements.front().from, result.path.movements.front().to);
        const bool passed = result.Succeeded() && !firstCutsCorner;
        check("diagonal-corner-contract", passed,
            passed ? "" : PathFingerprint(result.path));
    }

    // Long sprint validates every intermediate support after world mutation.
    {
        World world;
        AddPlatform(world, 0, 8, -1, 1);
        GoalReachPosition goal(GridPos { 8, 0, 0 });
        NavigationProfile profile = PlannerProfile();
        const NavigationSearchResult result = Search(
            world, GridPos { 0, 0, 0 }, goal, profile);
        const auto longSprint = std::find_if(
            result.path.movements.begin(), result.path.movements.end(),
            [](const PlannedMovement& movement)
            {
                return movement.type == MovementType::Sprint && movement.traversedCells > 1;
            });
        bool invalidated = false;
        if (longSprint != result.path.movements.end())
        {
            const int dx = longSprint->to.x == longSprint->from.x ? 0
                : (longSprint->to.x > longSprint->from.x ? 1 : -1);
            const int dz = longSprint->to.z == longSprint->from.z ? 0
                : (longSprint->to.z > longSprint->from.z ? 1 : -1);
            world.RemoveBlock(GridPos {
                longSprint->from.x + dx,
                longSprint->from.y,
                longSprint->from.z + dz
            });
            NavigationWorldView changed(world);
            Player actor(914, "sprint-revision", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
            PathExecutor executor;
            executor.SetPath(result.path);
            RepathReason reason = RepathReason::None;
            invalidated = !executor.ValidateRemainingPath(actor, changed, profile, &reason)
                && reason == RepathReason::WorldChanged;
        }
        const bool passed = result.Succeeded()
            && longSprint != result.path.movements.end() && invalidated;
        check("sprint-swept-validation", passed,
            passed ? "" : PathFingerprint(result.path));
    }

    // Execute a mixed diagonal route through normal authoritative movement
    // commands. The executor never writes the actor position itself.
    {
        world_.Clear();
        networkActionState_.clear();
        players_.clear();
        AddPlatform(world_, -1, 9, -1, 8);
        Player bot(915, "action-library", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        bot.SetControlKind(PlayerControlKind::BotAuthoritative);
        players_.push_back(bot);
        GoalReachPosition goal(GridPos { 8, 0, 6 });
        NavigationProfile profile = PlannerProfile();
        const NavigationSearchResult result = Search(
            world_, GridPos { 0, 0, 0 }, goal, profile);
        PathExecutor executor;
        executor.SetPath(result.path);
        const bool plannedLongSprint = std::any_of(
            result.path.movements.begin(), result.path.movements.end(),
            [](const PlannedMovement& movement)
            {
                return movement.type == MovementType::Sprint && movement.traversedCells > 1;
            });
        const bool plannedDiagonal = std::any_of(
            result.path.movements.begin(), result.path.movements.end(),
            [](const PlannedMovement& movement)
            {
                return NavigationActionLibrary::IsDiagonal(movement.from, movement.to);
            });
        bool finished = false;
        for (std::uint32_t tick = 1; tick <= 900 && !finished; ++tick)
        {
            NavigationWorldView view(world_, 0);
            const PathExecutionUpdate update = executor.Update(
                players_.front(), view, profile, kTick, tick);
            PlayerCommand command;
            command.controlledPlayerId = 915;
            command.tick = tick;
            command.aimYaw = players_.front().GetYaw();
            command.selectedSlot = players_.front().GetSelectedSlot();
            if (update.proposal.active) command = update.proposal.command;
            ApplyPlayerCommand(players_.front(), command, kTick);
            players_.front().UpdateTimers(kTick);
            finished = update.pathFinished
                || executor.Status() == MovementExecutionStatus::Succeeded;
            if (update.needsRepath) break;
        }
        const Vector3 position = players_.front().GetPosition();
        const bool passed = result.Succeeded() && plannedLongSprint && plannedDiagonal && finished
            && position.x > 7.55f && position.z > 5.55f;
        check("action-library-authoritative", passed, passed ? "" :
            "status=" + std::string(ToString(executor.Status()))
            + " phase=" + ToString(executor.Phase())
            + " x=" + std::to_string(position.x)
            + " z=" + std::to_string(position.z)
            + " path=" + PathFingerprint(result.path));
    }

    // A one-block climb is a controlled adjacent hop and must execute through
    // the same authoritative command path without overshooting its landing.
    {
        world_.Clear();
        networkActionState_.clear();
        players_.clear();
        world_.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world_.PlaceBlock(GridPos { 1, 1, 0 }, GroundBlock());
        world_.PlaceBlock(GridPos { 2, 1, 0 }, GroundBlock());
        Player bot(917, "step-jump-execution", 0, Vector3 { 0.0f, 1.50f, 0.0f }, false);
        bot.SetControlKind(PlayerControlKind::BotAuthoritative);
        players_.push_back(bot);
        NavigationProfile profile = PlannerProfile();
        GoalReachPosition goal(GridPos { 2, 1, 0 });
        const NavigationSearchResult result = Search(
            world_, GridPos { 0, 0, 0 }, goal, profile);
        PathExecutor executor;
        executor.SetPath(result.path);
        bool finished = false;
        for (std::uint32_t tick = 1; tick <= 360 && !finished; ++tick)
        {
            NavigationWorldView view(world_, 0);
            const PathExecutionUpdate update = executor.Update(
                players_.front(), view, profile, kTick, tick);
            PlayerCommand command;
            command.controlledPlayerId = 917;
            command.tick = tick;
            command.aimYaw = players_.front().GetYaw();
            if (update.proposal.active) command = update.proposal.command;
            ApplyPlayerCommand(players_.front(), command, kTick);
            players_.front().UpdateTimers(kTick);
            finished = update.pathFinished
                || executor.Status() == MovementExecutionStatus::Succeeded;
            if (update.needsRepath) break;
        }
        const NavigationMetrics execution = executor.ConsumeMetricsDelta();
        const bool passed = result.Succeeded() && Contains(result.path, MovementType::JumpUp)
            && finished && execution.failedJumps == 0;
        check("step-jump-execution", passed, passed ? "" :
            "status=" + std::string(ToString(executor.Status()))
                + " phase=" + ToString(executor.Phase())
                + " x=" + std::to_string(players_.front().GetPosition().x)
                + " y=" + std::to_string(players_.front().GetPosition().y));
    }

    // A higher support under a low ceiling is not a legal JumpUp arc even
    // when both standing positions separately have normal body clearance.
    {
        World world;
        world.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world.PlaceBlock(GridPos { 1, 1, 0 }, GroundBlock());
        world.PlaceBlock(GridPos { 0, 3, 0 }, GroundBlock());
        GoalReachPosition goal(GridPos { 1, 1, 0 });
        const NavigationSearchResult result = Search(
            world, GridPos { 0, 0, 0 }, goal, PlannerProfile());
        const bool attemptsImpossibleJump = Contains(result.path, MovementType::JumpUp);
        check("step-jump-headroom", !attemptsImpossibleJump,
            attemptsImpossibleJump ? PathFingerprint(result.path) : "");
    }

    // Gap eligibility comes from the same 60 Hz acceleration/gravity envelope
    // as Player movement, then the accepted action is executed by commands.
    {
        world_.Clear();
        networkActionState_.clear();
        players_.clear();
        AddPlatform(world_, -1, 0, -1, 1);
        AddPlatform(world_, 2, 4, -1, 1);
        Player bot(918, "gap-envelope", 0, Vector3 { 0.0f, 1.50f, 0.0f }, false);
        bot.SetControlKind(PlayerControlKind::BotAuthoritative);
        players_.push_back(bot);
        NavigationProfile profile = PlannerProfile();
        profile.maxGapJumpBlocks = 3;
        GoalReachPosition goal(GridPos { 2, 0, 0 });
        const NavigationSearchResult result = Search(
            world_, GridPos { 0, 0, 0 }, goal, profile);
        PathExecutor executor;
        executor.SetPath(result.path);
        bool finished = false;
        for (std::uint32_t tick = 1; tick <= 360 && !finished; ++tick)
        {
            NavigationWorldView view(world_, 0);
            const PathExecutionUpdate update = executor.Update(
                players_.front(), view, profile, kTick, tick);
            PlayerCommand command;
            command.controlledPlayerId = 918;
            command.tick = tick;
            command.aimYaw = players_.front().GetYaw();
            if (update.proposal.active) command = update.proposal.command;
            ApplyPlayerCommand(players_.front(), command, kTick);
            players_.front().UpdateTimers(kTick);
            finished = update.pathFinished
                || executor.Status() == MovementExecutionStatus::Succeeded;
            if (update.needsRepath) break;
        }
        const NavigationMetrics execution = executor.ConsumeMetricsDelta();
        const bool passed = profile.CanExecuteGapJump(1)
            && result.Succeeded() && Contains(result.path, MovementType::GapJump)
            && finished && execution.successfulGapJumps == 1;
        check("gap-jump-envelope", passed, passed ? "" :
            "reach=" + std::to_string(profile.EstimatedJumpHorizontalReach())
            + " status=" + ToString(executor.Status())
            + " x=" + std::to_string(players_.front().GetPosition().x)
            + " path=" + PathFingerprint(result.path));
    }

    // A sprint feeding a jump keeps sprint input through the transition even
    // on an exposed support. Braking here would remove the jump runway.
    {
        World world;
        AddPlatform(world, 0, 2, 0, 0);
        world.PlaceBlock(GridPos { 3, 1, 0 }, GroundBlock());
        NavigationPath path;
        path.start = GridPos { 0, 0, 0 };
        path.resolvedGoal = GridPos { 3, 1, 0 };
        path.worldRevision = world.GetRenderRevision();
        PlannedMovement run;
        run.type = MovementType::Sprint;
        run.from = GridPos { 0, 0, 0 };
        run.to = GridPos { 2, 0, 0 };
        run.requiresSprint = true;
        run.traversedCells = 2;
        run.plannedWorldRevision = path.worldRevision;
        PlannedMovement jump;
        jump.type = MovementType::JumpUp;
        jump.from = run.to;
        jump.to = GridPos { 3, 1, 0 };
        jump.requiresJump = true;
        jump.plannedWorldRevision = path.worldRevision;
        path.movements = { run, jump };
        Player actor(924, "momentum-transition", 0, Vector3 { 1.52f, 1.08f, 0.0f }, false);
        PathExecutor executor;
        executor.SetPath(path);
        NavigationWorldView view(world);
        const PathExecutionUpdate update = executor.Update(
            actor, view, PlannerProfile(), kTick, 1);
        const NavigationMetrics metrics = executor.ConsumeMetricsDelta();
        const bool passed = update.proposal.active
            && update.proposal.command.sprint
            && !update.proposal.command.sneak
            && metrics.momentumPreservedTransitions == 1
            && metrics.edgeBrakeActions == 0;
        check("momentum-preserved-before-jump", passed, passed ? "" :
            "sprint=" + std::to_string(update.proposal.command.sprint ? 1 : 0)
            + " sneak=" + std::to_string(update.proposal.command.sneak ? 1 : 0)
            + " preserved=" + std::to_string(metrics.momentumPreservedTransitions));
    }

    // Every authored parkour span is a real sprint-runup jump. The planning
    // contract covers one, two and three missing support cells without
    // relying on a spring block or a bridge.
    {
        NavigationProfile profile = PlannerProfile();
        profile.maxGapJumpBlocks = 3;
        bool passed = profile.CanExecuteGapJump(1)
            && profile.CanExecuteGapJump(2)
            && profile.CanExecuteGapJump(3);
        std::string failure;
        for (int gap = 1; gap <= 3 && passed; ++gap)
        {
            world_.Clear();
            networkActionState_.clear();
            players_.clear();
            AddPlatform(world_, -1, 0, -1, 1);
            AddPlatform(world_, gap + 1, gap + 2, -1, 1);
            Player bot(940 + gap, "sprint-runup-gap", 0, Vector3 { 0.0f, 1.50f, 0.0f }, false);
            bot.SetControlKind(PlayerControlKind::BotAuthoritative);
            players_.push_back(bot);
            GoalReachPosition goal(GridPos { gap + 1, 0, 0 });
            const NavigationSearchResult result = Search(
                world_, GridPos { 0, 0, 0 }, goal, profile);
            const auto jump = std::find_if(
                result.path.movements.begin(), result.path.movements.end(),
                [gap](const PlannedMovement& movement)
                {
                    return movement.type == MovementType::GapJump
                        && movement.to == GridPos { gap + 1, 0, 0 };
                });
            PathExecutor executor;
            executor.SetPath(result.path);
            bool finished = false;
            for (std::uint32_t tick = 1; tick <= 720 && !finished; ++tick)
            {
                NavigationWorldView view(world_);
                const PathExecutionUpdate update = executor.Update(
                    players_.front(), view, profile, kTick, tick);
                PlayerCommand command;
                command.controlledPlayerId = static_cast<std::uint32_t>(players_.front().GetId());
                command.tick = tick;
                command.aimYaw = players_.front().GetYaw();
                command.selectedSlot = players_.front().GetSelectedSlot();
                if (update.proposal.active) command = update.proposal.command;
                ApplyPlayerCommand(players_.front(), command, kTick);
                players_.front().UpdateTimers(kTick);
                finished = update.pathFinished
                    || executor.Status() == MovementExecutionStatus::Succeeded;
                if (update.needsRepath) break;
            }
            const NavigationMetrics execution = executor.ConsumeMetricsDelta();
            passed = result.Succeeded() && jump != result.path.movements.end()
                && jump->requiresJump && jump->requiresSprint;
            passed = passed && finished && execution.successfulGapJumps == 1;
            if (!passed) failure = PathFingerprint(result.path);
        }
        check("sprint-runup-gap-series", passed, passed ? "" : failure);
    }

    // The same exposed sprint endpoint brakes when transitioning into a
    // bridge action. The executor changes only ordinary command flags.
    {
        World world;
        AddPlatform(world, 0, 2, 0, 0);
        NavigationPath path;
        path.start = GridPos { 0, 0, 0 };
        path.resolvedGoal = GridPos { 3, 0, 0 };
        path.worldRevision = world.GetRenderRevision();
        PlannedMovement run;
        run.type = MovementType::Sprint;
        run.from = GridPos { 0, 0, 0 };
        run.to = GridPos { 2, 0, 0 };
        run.requiresSprint = true;
        run.traversedCells = 2;
        run.plannedWorldRevision = path.worldRevision;
        PlannedMovement bridge;
        bridge.type = MovementType::SneakBridge;
        bridge.from = run.to;
        bridge.to = GridPos { 3, 0, 0 };
        bridge.affectedBlock = bridge.to;
        bridge.placementBlockType = BlockType::WoolBlock;
        bridge.requiresSneak = true;
        bridge.plannedWorldRevision = path.worldRevision;
        path.movements = { run, bridge };
        Player actor(925, "edge-brake", 0, Vector3 { 1.52f, 1.08f, 0.0f }, false);
        PathExecutor executor;
        executor.SetPath(path);
        NavigationWorldView view(world);
        const PathExecutionUpdate update = executor.Update(
            actor, view, PlannerProfile(), kTick, 1);
        const NavigationMetrics metrics = executor.ConsumeMetricsDelta();
        const bool passed = update.proposal.active
            && !update.proposal.command.sprint
            && update.proposal.command.sneak
            && metrics.edgeBrakeActions == 1
            && metrics.momentumPreservedTransitions == 0;
        check("edge-brake-before-bridge", passed, passed ? "" :
            "sprint=" + std::to_string(update.proposal.command.sprint ? 1 : 0)
            + " sneak=" + std::to_string(update.proposal.command.sneak ? 1 : 0)
            + " brakes=" + std::to_string(metrics.edgeBrakeActions));
    }

    // Block revisions retain a bounded spatial journal. A remote mutation is
    // accepted without path validation; an intersecting mutation is visible,
    // and Clear deliberately invalidates older history.
    {
        World world;
        AddPlatform(world, 0, 12, 0, 2);
        Player actor(919, "dirty-regions", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        NavigationController controller;
        controller.SetGoal(std::make_shared<GoalReachPosition>(GridPos { 12, 0, 0 }), 919);
        NavigationProfile profile = PlannerProfile();
        NavigationWorldView initialView(world);
        const NavigationControllerUpdate first = controller.Update(
            actor, initialView, profile, kTick, 1);
        const std::string original = PathFingerprint(controller.DebugSnapshot().path);
        const std::uint64_t plannedRevision = initialView.WorldRevision();
        world.PlaceBlock(GridPos { 100, 0, 100 }, GroundBlock());
        const NavigationChangeQuery remote = world.QueryNavigationChanges(
            plannedRevision, GridPos { 0, -1, -1 }, GridPos { 12, 3, 3 });
        NavigationWorldView remoteView(world);
        controller.Update(actor, remoteView, profile, kTick, 2);
        const bool pathPreserved = original == PathFingerprint(controller.DebugSnapshot().path);
        const std::uint64_t beforeIntersect = world.GetRenderRevision();
        world.RemoveBlock(GridPos { 1, 0, 0 });
        const NavigationChangeQuery intersect = world.QueryNavigationChanges(
            beforeIntersect, GridPos { 0, -1, -1 }, GridPos { 4, 3, 1 });
        const std::uint64_t beforeClear = world.GetRenderRevision();
        world.Clear();
        const NavigationChangeQuery unavailable = world.QueryNavigationChanges(
            beforeClear, GridPos { 0, 0, 0 }, GridPos { 1, 1, 1 });
        const bool passed = first.planned
            && remote == NavigationChangeQuery::Disjoint
            && intersect == NavigationChangeQuery::Intersects
            && unavailable == NavigationChangeQuery::HistoryUnavailable
            && pathPreserved
            && controller.Metrics().dirtyRegionFastAccepts == 1
            && controller.Metrics().segmentRepairAttempts == 0;
        check("dirty-navigation-regions", passed, passed ? "" :
            "remote=" + std::to_string(static_cast<int>(remote))
            + " intersect=" + std::to_string(static_cast<int>(intersect))
            + " history=" + std::to_string(static_cast<int>(unavailable))
            + " accepts=" + std::to_string(controller.Metrics().dirtyRegionFastAccepts));
    }

    // 3. Bridge is part of the route and spends one concrete cheap material per cell.

    // A constrained search stays inside the coarse segment tube and records
    // successors that would have expanded into irrelevant side space.
    {
        World world;
        AddPlatform(world, 0, 20, -8, 8);
        NavigationWorldView view(world);
        NavigationProfile profile = PlannerProfile();
        NavigationSearchLimits limits;
        limits.maxExpansions = 2400;
        limits.maxSearchRadius = 32;
        limits.maxActions = 96;
        limits.constrainToCorridor = true;
        limits.corridorStart = GridPos { 0, 0, 0 };
        limits.corridorEnd = GridPos { 20, 0, 0 };
        limits.corridorHalfWidth = 2.0f;
        limits.corridorVerticalPadding = 4;
        GoalReachPosition goal(GridPos { 20, 0, 0 });
        const NavigationSearchResult result = VoxelPathfinder {}.FindPath(
            NavigationState { GridPos { 0, 0, 0 }, 0, 0 },
            goal, view, profile, limits);
        const bool inside = std::all_of(
            result.path.movements.begin(), result.path.movements.end(),
            [](const PlannedMovement& movement) { return std::abs(movement.to.z) <= 2; });
        const bool passed = result.Succeeded() && inside
            && result.path.corridorRejectedNodes > 0;
        check("corridor-constrained", passed, passed ? "" :
            "rejected=" + std::to_string(result.path.corridorRejectedNodes)
            + " path=" + PathFingerprint(result.path));
    }

    // If map authoring or geometry requires a wider detour, the controller
    // retries without the corridor hint instead of declaring the map blocked.
    {
        World world;
        for (int x = 0; x <= 3; ++x) world.PlaceBlock(GridPos { x, 0, 0 }, GroundBlock());
        for (int z = 0; z <= 6; ++z) world.PlaceBlock(GridPos { 3, 0, z }, GroundBlock());
        for (int x = 3; x <= 9; ++x) world.PlaceBlock(GridPos { x, 0, 6 }, GroundBlock());
        for (int z = 0; z <= 6; ++z) world.PlaceBlock(GridPos { 9, 0, z }, GroundBlock());
        for (int x = 9; x <= 12; ++x) world.PlaceBlock(GridPos { x, 0, 0 }, GroundBlock());
        Player actor(916, "corridor-fallback", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        NavigationWorldView view(world);
        NavigationControllerSettings settings;
        settings.corridorMinimumLength = 2.0f;
        settings.corridorHalfWidth = 1.0f;
        settings.searchLimits.allowPartial = false;
        NavigationController controller(settings);
        controller.SetGoal(std::make_shared<GoalReachPosition>(GridPos { 12, 0, 0 }), 916);
        const NavigationControllerUpdate update = controller.Update(
            actor, view, PlannerProfile(), kTick, 1);
        const bool usesDetour = std::any_of(
            controller.DebugSnapshot().path.movements.begin(),
            controller.DebugSnapshot().path.movements.end(),
            [](const PlannedMovement& movement) { return movement.to.z >= 6; });
        const bool passed = update.planned && usesDetour
            && controller.Metrics().corridorConstrainedSearches == 1
            && controller.Metrics().corridorFallbackSearches == 1;
        check("corridor-fallback", passed, passed ? "" :
            "planned=" + std::to_string(update.planned ? 1 : 0)
            + " constrained=" + std::to_string(controller.Metrics().corridorConstrainedSearches)
            + " fallback=" + std::to_string(controller.Metrics().corridorFallbackSearches));
    }

    // Damage a support crossed by an active long sprint. The controller must
    // splice a local detour into the untouched suffix rather than discard the
    // complete route.
    {
        World world;
        AddPlatform(world, 0, 14, 0, 2);
        Player actor(917, "segment-repair", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        NavigationController controller;
        controller.SetGoal(std::make_shared<GoalReachPosition>(GridPos { 14, 0, 0 }), 917);
        NavigationProfile profile = PlannerProfile();
        NavigationWorldView initialView(world);
        const NavigationControllerUpdate first = controller.Update(
            actor, initialView, profile, kTick, 1);
        const NavigationPath original = controller.DebugSnapshot().path;
        bool damaged = false;
        if (!original.movements.empty())
        {
            const PlannedMovement& movement = original.movements.front();
            const int dx = movement.to.x == movement.from.x ? 0
                : (movement.to.x > movement.from.x ? 1 : -1);
            const int dz = movement.to.z == movement.from.z ? 0
                : (movement.to.z > movement.from.z ? 1 : -1);
            const GridPos crossed {
                movement.from.x + dx,
                movement.from.y,
                movement.from.z + dz
            };
            damaged = crossed != movement.from && world.RemoveBlock(crossed);
        }
        NavigationWorldView changedView(world);
        controller.Update(actor, changedView, profile, kTick, 2);
        const NavigationPath repaired = controller.DebugSnapshot().path;
        const bool usesDetour = std::any_of(
            repaired.movements.begin(), repaired.movements.end(),
            [](const PlannedMovement& movement) { return movement.to.z > 0; });
        const NavigationMetrics& metrics = controller.Metrics();
        const bool passed = first.planned && damaged && usesDetour
            && metrics.segmentRepairAttempts == 1
            && metrics.segmentRepairSuccesses == 1
            && metrics.segmentRepairReusedActions > 0;
        check("segment-repair", passed, passed ? "" :
            "damaged=" + std::to_string(damaged ? 1 : 0)
            + " attempts=" + std::to_string(metrics.segmentRepairAttempts)
            + " successes=" + std::to_string(metrics.segmentRepairSuccesses)
            + " reused=" + std::to_string(metrics.segmentRepairReusedActions)
            + " path=" + PathFingerprint(repaired));
    }

    NavigationPath bridgePath;

    // A failed executable gap action is repaired locally into a bridge. The
    // controller retains the suffix and does not pay for another global A*.
    {
        World world;
        AddPlatform(world, -1, 0, -1, 1);
        AddPlatform(world, 2, 3, -1, 1);
        Player actor(920, "gap-to-bridge", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        actor.GetInventory().AddBlock(BlockType::WoolBlock, 1);
        NavigationController controller;
        controller.SetGoal(std::make_shared<GoalReachPosition>(GridPos { 2, 0, 0 }), 920);
        NavigationProfile jumpProfile = PlannerProfile();
        jumpProfile.maxGapJumpBlocks = 1;
        NavigationWorldView view(world);
        const NavigationControllerUpdate planned = controller.Update(
            actor, view, jumpProfile, kTick, 1);
        NavigationProfile bridgeProfile = BuildNavigationProfile(actor);
        bridgeProfile.reserveBridgeBlocks = 0;
        bridgeProfile.maxGapJumpBlocks = 1;
        bool converted = false;
        for (std::uint32_t tick = 2; tick <= 240 && !converted; ++tick)
        {
            controller.Update(actor, view, bridgeProfile, kTick, tick);
            converted = controller.Metrics().gapToBridgeSuccesses == 1;
        }
        const NavigationPath convertedPath = controller.DebugSnapshot().path;
        const bool passed = planned.planned
            && converted
            && Contains(convertedPath, MovementType::SneakBridge)
            && controller.Metrics().gapToBridgeAttempts == 1
            && controller.Metrics().pathRequests == 1;
        check("gap-to-bridge-recovery", passed, passed ? "" :
            "planned=" + std::to_string(planned.planned ? 1 : 0)
            + " attempts=" + std::to_string(controller.Metrics().gapToBridgeAttempts)
            + " successes=" + std::to_string(controller.Metrics().gapToBridgeSuccesses)
            + " paths=" + std::to_string(controller.Metrics().pathRequests)
            + " path=" + PathFingerprint(convertedPath));
    }

    // Before declaring a locomotion action stuck, release forward pressure
    // and recenter once on the current support. This is a deterministic retry,
    // not per-tick steering noise.
    {
        World world;
        AddPlatform(world, 0, 3, -1, 1);
        Player actor(921, "recenter-retry", 0, Vector3 { 0.38f, 1.40f, 0.0f }, false);
        actor.Move(Vector3 {}, false, kTick, world);
        NavigationProfile profile = PlannerProfile();
        GoalReachPosition goal(GridPos { 3, 0, 0 });
        const NavigationSearchResult route = Search(
            world, GridPos { 0, 0, 0 }, goal, profile);
        PathExecutor executor;
        executor.SetPath(route.path);
        NavigationWorldView view(world);
        bool recoveryIssued = false;
        for (std::uint32_t tick = 1; tick <= 90 && !recoveryIssued; ++tick)
        {
            executor.Update(actor, view, profile, kTick, tick);
            recoveryIssued = executor.ConsumeMetricsDelta().recenterRecoveryAttempts > 0;
        }
        check("action-recenter-recovery", route.Succeeded() && recoveryIssued,
            recoveryIssued ? "" : "bounded recenter was not issued onGround="
                + std::to_string(actor.IsOnGround() ? 1 : 0)
                + " pos=" + std::to_string(actor.GetPosition().x) + ","
                + std::to_string(actor.GetPosition().y) + ","
                + std::to_string(actor.GetPosition().z)
                + " noProgress=" + std::to_string(executor.NoProgressSeconds()));
    }

    // A placement confirmation timeout retries the concrete bridge action
    // once without discarding the route or running A* again.
    {
        World world;
        world.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world.PlaceBlock(GridPos { 2, 0, 0 }, GroundBlock());
        Player actor(923, "bridge-action-retry", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        actor.GetInventory().AddBlock(BlockType::WoolBlock, 1);
        NavigationProfile profile = BuildNavigationProfile(actor);
        profile.reserveBridgeBlocks = 0;
        profile.maxGapJumpBlocks = 0;
        NavigationController controller;
        controller.SetGoal(std::make_shared<GoalReachPosition>(GridPos { 2, 0, 0 }), 923);
        NavigationWorldView view(world);
        bool recovered = false;
        for (std::uint32_t tick = 1; tick <= 360 && !recovered; ++tick)
        {
            controller.Update(actor, view, profile, kTick, tick);
            recovered = controller.Metrics().actionRecoverySuccesses == 1;
        }
        const bool passed = recovered
            && controller.Metrics().actionRecoveryAttempts == 1
            && controller.Metrics().pathRequests == 1
            && Contains(controller.DebugSnapshot().path, MovementType::SneakBridge);
        check("bridge-action-local-retry", passed, passed ? "" :
            "attempts=" + std::to_string(controller.Metrics().actionRecoveryAttempts)
            + " successes=" + std::to_string(controller.Metrics().actionRecoverySuccesses)
            + " paths=" + std::to_string(controller.Metrics().pathRequests));
    }

    // Repeating the same failed local action cannot trigger an infinite
    // replan loop. After the bounded budget the semantic route is abandoned so
    // BotMemory can select recovery/a different corridor on the next decision.
    {
        World world;
        AddPlatform(world, 0, 8, -1, 1);
        Player actor(922, "route-abandon", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        NavigationControllerSettings settings;
        settings.maxActionFailuresBeforeAbandon = 3;
        NavigationController controller(settings);
        controller.SetGoal(std::make_shared<GoalReachPosition>(GridPos { 8, 0, 0 }), 922);
        NavigationProfile profile = PlannerProfile();
        NavigationWorldView view(world);
        bool abandoned = false;
        for (std::uint32_t tick = 1; tick <= 420 && !abandoned; ++tick)
        {
            const NavigationControllerUpdate update = controller.Update(
                actor, view, profile, kTick, tick);
            abandoned = update.routeAbandoned;
        }
        const bool passed = abandoned
            && !controller.HasGoal()
            && controller.Metrics().routeAbandonments == 1
            && controller.Metrics().movementFailures >= 3;
        check("repeated-action-route-abandonment", passed, passed ? "" :
            "abandoned=" + std::to_string(abandoned ? 1 : 0)
            + " failures=" + std::to_string(controller.Metrics().movementFailures)
            + " paths=" + std::to_string(controller.Metrics().pathRequests));
    }

    {
        World world;
        world.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world.PlaceBlock(GridPos { 3, 0, 0 }, GroundBlock());
        NavigationProfile profile = PlannerProfile();
        profile.canPlaceBlocks = true;
        profile.canBridge = true;
        profile.availableBridgeBlocks = 2;
        profile.preferredBridgeBlock = BlockType::WoolBlock;
        profile.maxGapJumpBlocks = 0;
        GoalReachPosition goal(GridPos { 3, 0, 0 });
        const NavigationSearchResult result = Search(world, GridPos { 0, 0, 0 }, goal, profile);
        bridgePath = result.path;
        const int bridges = static_cast<int>(std::count_if(
            result.path.movements.begin(), result.path.movements.end(), [](const PlannedMovement& movement)
            { return movement.type == MovementType::SneakBridge; }));
        const bool passed = result.Succeeded() && bridges == 2;
        check("bridge-plan", passed, passed ? "" : (result.Succeeded() ? "expected two SneakBridge actions" : result.description));
    }

    // A typed Castle bridge is a long Manhattan staircase between diagonal
    // islands. The ordinary eight-block cap must not make that authored edge
    // physically impossible.
    {
        World world;
        world.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world.PlaceBlock(GridPos { 25, 1, 24 }, GroundBlock());
        NavigationProfile profile = PlannerProfile();
        profile.canPlaceBlocks = true;
        profile.canBridge = true;
        profile.availableBridgeBlocks = 64;
        profile.maxConsecutiveBridgeBlocks = 52;
        profile.maxGapJumpBlocks = 0;
        GoalReachPosition goal(GridPos { 25, 1, 24 });
        NavigationWorldView view(world, 0);
        NavigationSearchLimits limits;
        limits.maxExpansions = 3200;
        limits.maxSearchRadius = 64;
        limits.maxVerticalRange = 24;
        limits.maxActions = 256;
        const NavigationSearchResult result = VoxelPathfinder {}.FindPath(
            NavigationState { GridPos { 0, 0, 0 }, 64, 0 }, goal, view, profile, limits);
        const int bridgeActions = static_cast<int>(std::count_if(
            result.path.movements.begin(), result.path.movements.end(),
            [](const PlannedMovement& movement) { return movement.type == MovementType::SneakBridge; }));
        const bool passed = result.status == NavigationSearchStatus::Partial
            && bridgeActions == 16
            && result.path.resolvedGoal != GridPos { 0, 0, 0 };
        check("authored-long-bridge-segment", passed, passed ? "" :
            std::string(ToString(result.status)) + " actions="
                + std::to_string(result.path.movements.size())
                + " bridges=" + std::to_string(bridgeActions)
                + " expanded=" + std::to_string(result.path.expandedNodes));
    }

    // Execute that bridge only through PlayerCommand + the authoritative block path.
    {
        world_.Clear();
        networkActionState_.clear();
        players_.clear();
        world_.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world_.PlaceBlock(GridPos { 3, 0, 0 }, GroundBlock());
        Player bot(912, "bridge-command", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        bot.SetControlKind(PlayerControlKind::BotAuthoritative);
        bot.GetInventory().AddBlock(BlockType::WoolBlock, 2);
        players_.push_back(bot);
        PathExecutor executor;
        executor.SetPath(bridgePath);
        bool finished = false;
        for (std::uint32_t tick = 1; tick <= 900 && !finished; ++tick)
        {
            NavigationProfileSettings settings;
            settings.reserveBridgeBlocks = 0;
            settings.maxGapJumpBlocks = 0;
            NavigationProfile profile = BuildNavigationProfile(players_.front(), settings);
            NavigationWorldView view(world_, 0);
            const PathExecutionUpdate update = executor.Update(players_.front(), view, profile, kTick, tick);
            PlayerCommand command;
            command.controlledPlayerId = 912;
            command.tick = tick;
            command.aimYaw = players_.front().GetYaw();
            command.selectedSlot = players_.front().GetSelectedSlot();
            if (update.proposal.active) command = update.proposal.command;
            ApplyPlayerCommand(players_.front(), command, kTick);
            ApplyNetworkPlayerActions(players_.front(), command, kTick);
            players_.front().UpdateTimers(kTick);
            finished = update.pathFinished || executor.Status() == MovementExecutionStatus::Succeeded;
            if (update.needsRepath) break;
        }
        const bool passed = finished
            && world_.IsSolid(GridPos { 1, 0, 0 })
            && world_.IsSolid(GridPos { 2, 0, 0 })
            && players_.front().GetInventory().GetBlockCount(BlockType::WoolBlock) == 0
            && players_.front().GetPosition().x > 2.55f;
        check("bridge-authoritative", passed, passed ? "" :
            "finished=" + std::to_string(finished ? 1 : 0)
            + " b1=" + std::to_string(world_.IsSolid(GridPos { 1, 0, 0 }) ? 1 : 0)
            + " b2=" + std::to_string(world_.IsSolid(GridPos { 2, 0, 0 }) ? 1 : 0)
            + " blocks=" + std::to_string(players_.front().GetInventory().GetBlockCount(BlockType::WoolBlock))
            + " x=" + std::to_string(players_.front().GetPosition().x)
            + " status=" + ToString(executor.Status())
            + " phase=" + ToString(executor.Phase())
            + " index=" + std::to_string(executor.MovementIndex())
            + " elapsed=" + std::to_string(executor.ActionElapsedSeconds())
            + " noProgress=" + std::to_string(executor.NoProgressSeconds())
            + " msg=" + executor.LastMessage());
    }

    // A side-anchored rising route is completed by placing wool one level up
    // and jumping onto it. This is the building primitive used by the
    // symmetric parkour curriculum's four-step staircases.
    {
        world_.Clear();
        networkActionState_.clear();
        players_.clear();
        world_.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world_.PlaceBlock(GridPos { 1, 1, 1 }, GroundBlock());
        world_.PlaceBlock(GridPos { 2, 2, 1 }, GroundBlock());
        Player bot(915, "stair-command", 0, Vector3 { 0.0f, 1.50f, 0.0f }, false);
        bot.SetControlKind(PlayerControlKind::BotAuthoritative);
        bot.GetInventory().AddBlock(BlockType::WoolBlock, 2);
        players_.push_back(bot);

        NavigationProfileSettings settings;
        settings.allowStairBuilding = true;
        settings.reserveBridgeBlocks = 0;
        settings.maxGapJumpBlocks = 0;
        settings.maxConsecutiveBridgeBlocks = 4;
        NavigationProfile profile = BuildNavigationProfile(players_.front(), settings);
        GoalReachPosition goal(GridPos { 2, 2, 0 });
        const NavigationSearchResult result = Search(
            world_, GridPos { 0, 0, 0 }, goal, profile);
        const int stairActions = static_cast<int>(std::count_if(
            result.path.movements.begin(), result.path.movements.end(),
            [](const PlannedMovement& movement)
            {
                return movement.type == MovementType::SneakBridge
                    && movement.requiresJump
                    && movement.to.y > movement.from.y;
            }));
        PathExecutor executor;
        executor.SetPath(result.path);
        bool finished = false;
        for (std::uint32_t tick = 1; tick <= 1200 && !finished; ++tick)
        {
            NavigationWorldView view(world_, 0);
            const PathExecutionUpdate update = executor.Update(
                players_.front(), view, profile, kTick, tick);
            PlayerCommand command;
            command.controlledPlayerId = 915;
            command.tick = tick;
            command.aimYaw = players_.front().GetYaw();
            command.selectedSlot = players_.front().GetSelectedSlot();
            if (update.proposal.active) command = update.proposal.command;
            ApplyPlayerCommand(players_.front(), command, kTick);
            ApplyNetworkPlayerActions(players_.front(), command, kTick);
            players_.front().UpdateTimers(kTick);
            finished = update.pathFinished
                || executor.Status() == MovementExecutionStatus::Succeeded;
            if (update.needsRepath) break;
        }
        const bool passed = result.Succeeded()
            && stairActions == 2
            && finished
            && world_.IsSolid(GridPos { 1, 1, 0 })
            && world_.IsSolid(GridPos { 2, 2, 0 })
            && players_.front().GetPosition().y > 3.0f;
        check("wool-stair-authoritative", passed, passed ? "" :
            std::string(ToString(result.status))
            + " actions=" + std::to_string(stairActions)
            + " finished=" + std::to_string(finished ? 1 : 0)
            + " y=" + std::to_string(players_.front().GetPosition().y)
            + " x=" + std::to_string(players_.front().GetPosition().x)
            + " b1=" + std::to_string(world_.IsSolid(GridPos { 1, 1, 0 }) ? 1 : 0)
            + " b2=" + std::to_string(world_.IsSolid(GridPos { 2, 2, 0 }) ? 1 : 0)
            + " flat=" + std::to_string(world_.IsSolid(GridPos { 1, 0, 0 }) ? 1 : 0)
            + " index=" + std::to_string(executor.MovementIndex())
            + " status=" + ToString(executor.Status())
            + " phase=" + ToString(executor.Phase())
            + " msg=" + executor.LastMessage());
    }

    // Horizontal construction is not clipped to the former stock-arena
    // square. Normal reach, support, inventory, collision and height rules
    // still apply through the authoritative placement path.
    {
        world_.Clear();
        players_.clear();
        world_.PlaceBlock(GridPos { 80, 0, 0 }, GroundBlock());
        Player builder(913, "unbounded-builder", 0, Vector3 { 80.0f, 1.08f, 0.0f }, false);
        builder.GetInventory().AddBlock(BlockType::WoolBlock, 1);
        builder.SetSelectedSlot(0);
        players_.push_back(builder);
        const BlockActionResult result = ApplyPlaceBlockForPlayer(players_.front(), GridPos { 81, 0, 0 });
        const bool passed = result.success
            && world_.IsSolid(GridPos { 81, 0, 0 })
            && players_.front().GetInventory().GetBlockCount(BlockType::WoolBlock) == 0;
        check("unbounded-build-zone", passed, passed ? "" : result.message);
    }

    // Multiple bounded partial plans must compose into one long authored
    // bridge instead of restarting after every 16-action segment.
    {
        world_.Clear();
        networkActionState_.clear();
        players_.clear();
        world_.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world_.PlaceBlock(GridPos { 25, 1, 24 }, GroundBlock());
        Player bot(914, "long-bridge-command", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        bot.SetControlKind(PlayerControlKind::BotAuthoritative);
        bot.GetInventory().AddBlock(BlockType::WoolBlock, 64);
        players_.push_back(bot);
        NavigationController controller;
        controller.SetGoal(std::make_shared<GoalReachPosition>(GridPos { 25, 1, 24 }), 914u);
        bool finished = false;
        bool abandoned = false;
        for (std::uint32_t tick = 1; tick <= 12000 && !finished && !abandoned; ++tick)
        {
            NavigationProfileSettings settings;
            settings.reserveBridgeBlocks = 0;
            settings.maxGapJumpBlocks = 0;
            settings.maxConsecutiveBridgeBlocks = 52;
            NavigationProfile profile = BuildNavigationProfile(players_.front(), settings);
            NavigationWorldView view(world_, 0);
            const NavigationControllerUpdate update = controller.Update(
                players_.front(), view, profile, kTick, tick);
            ApplyPlayerCommand(players_.front(), update.command, kTick);
            ApplyNetworkPlayerActions(players_.front(), update.command, kTick);
            players_.front().UpdateTimers(kTick);
            finished = update.goalSatisfied;
            abandoned = update.routeAbandoned;
        }
        const NavigationMetrics metrics = controller.Metrics();
        const NavigationDebugSnapshot debug = controller.DebugSnapshot();
        const PlannedMovement* firstDebugMovement = debug.path.movements.empty()
            ? nullptr : &debug.path.movements.front();
        const bool passed = finished && !abandoned
            && players_.front().GetPosition().x > 24.0f
            && players_.front().GetPosition().z > 23.0f
            && metrics.bridgeBlocksUsed >= 48;
        check("authored-long-bridge-authoritative", passed, passed ? "" :
            "finished=" + std::to_string(finished ? 1 : 0)
                + " abandoned=" + std::to_string(abandoned ? 1 : 0)
                + " blocks=" + std::to_string(metrics.bridgeBlocksUsed)
                + " failures=" + std::to_string(metrics.movementFailures)
                + " paths=" + std::to_string(metrics.pathRequests)
                + " goalRepaths=" + std::to_string(
                    metrics.repathReasons[static_cast<std::size_t>(RepathReason::GoalChanged)])
                + " x=" + std::to_string(players_.front().GetPosition().x)
                + " z=" + std::to_string(players_.front().GetPosition().z)
                + " status=" + ToString(debug.executionStatus)
                + " phase=" + ToString(debug.phase)
                + " index=" + std::to_string(debug.movementIndex)
                + " path=" + std::to_string(debug.path.movements.size())
                + " first=" + (firstDebugMovement != nullptr
                    ? std::to_string(firstDebugMovement->from.x) + ","
                        + std::to_string(firstDebugMovement->from.z) + ">"
                        + std::to_string(firstDebugMovement->to.x) + ","
                        + std::to_string(firstDebugMovement->to.z)
                    : std::string("none")));
    }

    // 4. Insufficient blocks cannot become a full route.
    {
        World world;
        world.PlaceBlock(GridPos { 0, 0, 0 }, GroundBlock());
        world.PlaceBlock(GridPos { 4, 0, 0 }, GroundBlock());
        NavigationProfile profile = PlannerProfile();
        profile.canPlaceBlocks = true;
        profile.canBridge = true;
        profile.availableBridgeBlocks = 2;
        profile.maxGapJumpBlocks = 0;
        GoalReachPosition goal(GridPos { 4, 0, 0 });
        const NavigationSearchResult result = Search(world, GridPos { 0, 0, 0 }, goal, profile);
        check("insufficient-blocks", !result.Succeeded()
                && (result.status == NavigationSearchStatus::Partial || !result.HasPath()),
            result.description);
    }

    // 5. Cheap obstacle is represented as a BreakBlock action.
    NavigationPath breakPath;
    {
        World world;
        AddPlatform(world, 0, 4, 0, 0);
        world.PlaceBlock(GridPos { 1, 1, 0 }, Block { BlockType::WoolBlock, 1, true });
        world.PlaceBlock(GridPos { 1, 3, 0 }, GroundBlock());
        NavigationProfile profile = PlannerProfile();
        profile.canBreakBlocks = true;
        profile.pickaxeLevel = 2;
        profile.pickaxeDurability = 100;
        GoalReachPosition goal(GridPos { 4, 0, 0 });
        const NavigationSearchResult result = Search(world, GridPos { 0, 0, 0 }, goal, profile);
        breakPath = result.path;
        const bool passed = result.Succeeded() && Contains(result.path, MovementType::BreakBlock);
        check("break-block", passed, passed ? "" : (result.Succeeded() ? "break action missing" : result.description));
    }


    // Execute planned mining through held attack input and authoritative confirmation.
    {
        world_.Clear();
        networkActionState_.clear();
        players_.clear();
        AddPlatform(world_, 0, 4, 0, 0);
        world_.PlaceBlock(GridPos { 1, 1, 0 }, Block { BlockType::WoolBlock, 1, true });
        world_.PlaceBlock(GridPos { 1, 3, 0 }, GroundBlock());
        Player bot(913, "break-command", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        bot.SetControlKind(PlayerControlKind::BotAuthoritative);
        bot.GetInventory().AddItem(ItemType::Pickaxe, 1);
        bot.GetInventory().UpgradeTool();
        players_.push_back(bot);
        PathExecutor executor;
        executor.SetPath(breakPath);
        bool finished = false;
        for (std::uint32_t tick = 1; tick <= 900 && !finished; ++tick)
        {
            NavigationProfileSettings settings;
            settings.allowBridging = false;
            NavigationProfile profile = BuildNavigationProfile(players_.front(), settings);
            NavigationWorldView view(world_, 0);
            const PathExecutionUpdate update = executor.Update(players_.front(), view, profile, kTick, tick);
            PlayerCommand command;
            command.controlledPlayerId = 913;
            command.tick = tick;
            command.aimYaw = players_.front().GetYaw();
            command.selectedSlot = players_.front().GetSelectedSlot();
            if (update.proposal.active) command = update.proposal.command;
            ApplyPlayerCommand(players_.front(), command, kTick);
            ApplyNetworkPlayerActions(players_.front(), command, kTick);
            players_.front().UpdateTimers(kTick);
            finished = update.pathFinished || executor.Status() == MovementExecutionStatus::Succeeded;
            if (update.needsRepath) break;
        }
        const bool passed = finished
            && world_.IsAir(GridPos { 1, 1, 0 })
            && players_.front().GetPosition().x > 3.45f;
        check("break-authoritative", passed, passed ? "" : "command mining did not remove/cross obstacle");
    }

    // 6. Expensive obstacle loses to a short supported detour.
    {
        World world;
        AddPlatform(world, 0, 5, 0, 1);
        world.PlaceBlock(GridPos { 2, 1, 0 }, Block { BlockType::ObsidianBlock, 1, true });
        NavigationProfile profile = PlannerProfile();
        profile.canBreakBlocks = true;
        profile.pickaxeLevel = 1;
        profile.pickaxeDurability = 100;
        GoalReachPosition goal(GridPos { 5, 0, 0 });
        const NavigationSearchResult result = Search(world, GridPos { 0, 0, 0 }, goal, profile);
        const bool passed = result.Succeeded() && !Contains(result.path, MovementType::BreakBlock);
        check("prefer-detour", passed, passed ? "" : (result.Succeeded() ? "planner chose expensive break" : result.description));
    }

    // 7. Removing route support invalidates it, then a deterministic detour replans.
    {
        World world;
        AddPlatform(world, 0, 5, 0, 1);
        GoalReachPosition goal(GridPos { 5, 0, 0 });
        NavigationProfile profile = PlannerProfile();
        const NavigationSearchResult first = Search(world, GridPos { 0, 0, 0 }, goal, profile);
        world.RemoveBlock(GridPos { 2, 0, 0 });
        NavigationWorldView changed(world);
        Player actor(910, "repath", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        PathExecutor executor;
        executor.SetPath(first.path);
        RepathReason reason = RepathReason::None;
        const bool invalid = !executor.ValidateRemainingPath(actor, changed, profile, &reason);
        const NavigationSearchResult second = Search(world, GridPos { 0, 0, 0 }, goal, profile);
        const bool passed = first.Succeeded() && invalid && reason == RepathReason::WorldChanged
            && second.Succeeded();
        check("repath", passed, passed ? "" : (invalid ? second.description : "old route remained valid"));
    }

    // 8. Risk coefficient changes the selected corridor.
    {
        World world;
        AddPlatform(world, 0, 6, 0, 2);
        ThreatMap threats;
        threats.AddSource(ThreatSource { Vector3 { 3.0f, 1.08f, 0.0f }, 2.6f, 10.0f, -1, 1 });
        GoalReachPosition goal(GridPos { 6, 0, 0 });
        NavigationProfile cautious = PlannerProfile();
        // This check isolates threat-cost personality from the action-library
        // shortcut tests above.
        cautious.canMoveDiagonally = false;
        cautious.canSprint = false;
        cautious.riskTolerance = 0.05f;
        NavigationProfile aggressive = cautious;
        aggressive.riskTolerance = 0.95f;
        const NavigationSearchResult safe = Search(world, GridPos { 0, 0, 0 }, goal, cautious, &threats);
        const NavigationSearchResult fast = Search(world, GridPos { 0, 0, 0 }, goal, aggressive, &threats);
        const auto usesOuterLane = [](const NavigationPath& path)
        {
            return std::any_of(path.movements.begin(), path.movements.end(), [](const PlannedMovement& movement)
                { return movement.to.z >= 2; });
        };
        const bool passed = safe.Succeeded() && fast.Succeeded()
                && usesOuterLane(safe.path)
                && !usesOuterLane(fast.path);
        check("threat-avoidance", passed,
            passed ? "" : "safe=" + PathFingerprint(safe.path) + " fast=" + PathFingerprint(fast.path));
    }

    // 9. Executor proposes a transportable command and cannot mutate the actor itself.
    {
        World world;
        AddPlatform(world, 0, 2, 0, 0);
        GoalReachPosition goal(GridPos { 2, 0, 0 });
        NavigationProfile profile = PlannerProfile();
        const NavigationSearchResult result = Search(world, GridPos { 0, 0, 0 }, goal, profile);
        Player actor(911, "command", 0, Vector3 { 0.0f, 1.08f, 0.0f }, false);
        const Vector3 before = actor.GetPosition();
        PathExecutor executor;
        executor.SetPath(result.path);
        NavigationWorldView view(world);
        const PathExecutionUpdate update = executor.Update(actor, view, profile, kTick, 1);
        const Vector3 after = actor.GetPosition();
        const bool passed = update.proposal.active
            && update.proposal.command.controlledPlayerId == 911U
            && update.proposal.command.moveForward > 0.0f
            && before.x == after.x && before.y == after.y && before.z == after.z;
        check("multiplayer-command", passed, passed ? "" : "executor mutated actor or emitted no PlayerCommand");
    }

    // 10. Fixed ordering produces byte-equivalent action fingerprints.
    {
        World world;
        AddPlatform(world, 0, 6, -1, 1);
        world.RemoveBlock(GridPos { 3, 0, 0 });
        GoalReachPosition goal(GridPos { 6, 0, 0 });
        const NavigationSearchResult a = Search(world, GridPos { 0, 0, 0 }, goal, PlannerProfile());
        const NavigationSearchResult b = Search(world, GridPos { 0, 0, 0 }, goal, PlannerProfile());
        const bool passed = a.status == b.status && PathFingerprint(a.path) == PathFingerprint(b.path);
        check("determinism", passed, passed ? "" : "same state produced different actions");
    }

    // 11. The strategic graph selects a stable branch and rejects malformed
    // authoring instead of silently dropping the bad edge.
    {
        const std::vector<CreativeRouteNode> nodes {
            { "start", GridPos { 0, 0, 0 }, -1, "base" },
            { "main", GridPos { 30, 0, 0 }, -1, "lane" },
            { "flank", GridPos { 0, 0, 40 }, -1, "lane" },
            { "goal", GridPos { 60, 0, 0 }, -1, "base" }
        };
        const std::vector<CreativeRouteEdge> edges {
            { "start", "main", 1.0f, true, "main" },
            { "main", "goal", 1.0f, true, "main" },
            { "start", "flank", 2.0f, true, "flank" },
            { "flank", "goal", 2.0f, true, "flank" }
        };
        RouteGraph graph;
        std::vector<std::string> errors;
        const bool built = graph.Build(nodes, edges, &errors);
        const RouteCorridor first = graph.FindCorridor(GridPos { 0, 0, 0 }, GridPos { 60, 0, 0 }, 0);
        const RouteCorridor second = graph.FindCorridor(GridPos { 0, 0, 0 }, GridPos { 60, 0, 0 }, 0);
        const CreativeRouteNode* branch = first.nodeIndices.size() > 1 ? graph.Node(first.nodeIndices[1]) : nullptr;
        const GridPos failedMain { 30, 0, 0 };
        const RouteCorridor rerouted = graph.FindCorridor(
            GridPos { 0, 0, 0 }, GridPos { 60, 0, 0 }, 0, &failedMain, 120.0f);
        const CreativeRouteNode* reroutedBranch = rerouted.nodeIndices.size() > 1
            ? graph.Node(rerouted.nodeIndices[1]) : nullptr;

        RouteGraph invalid;
        const bool acceptedInvalid = invalid.Build(nodes,
            { CreativeRouteEdge { "start", "missing", 1.0f, true, "bad" } }, &errors);
        const bool passed = built && first.valid && second.valid
            && first.signature == second.signature
            && first.segments.size() + 1 == first.nodeIndices.size()
            && branch != nullptr && branch->id == "main"
            && reroutedBranch != nullptr && reroutedBranch->id == "flank"
            && !acceptedInvalid && invalid.Empty();
        check("route-graph", passed, passed ? "" : "branch, determinism, or validation failed");
    }

    // A bounded search must make useful progress even when the only valid
    // route initially moves away from the goal. This is the shape of the
    // Castle base ingress and guards against returning NoPath at the doorway.
    {
        World world;
        AddPlatform(world, -3, 6, -6, 6);
        for (int z = -4; z <= 4; ++z)
        {
            for (int y = 1; y <= 3; ++y)
            {
                world.PlaceBlock(GridPos { 1, y, z }, GroundBlock());
            }
        }
        GoalReachPosition goal(GridPos { 4, 0, 0 });
        NavigationSearchLimits bounded;
        bounded.maxExpansions = 2;
        bounded.maxSearchRadius = 16;
        bounded.maxVerticalRange = 8;
        bounded.maxActions = 64;
        bounded.allowPartial = true;
        NavigationWorldView view(world, 0);
        const NavigationSearchResult partial = VoxelPathfinder {}.FindPath(
            NavigationState { GridPos { 0, 0, 0 }, 0, 0 }, goal, view,
            PlannerProfile(), bounded);
        const NavigationSearchResult complete = Search(
            world, GridPos { 0, 0, 0 }, goal, PlannerProfile());
        const bool passed = partial.status == NavigationSearchStatus::Partial
            && partial.HasPath() && complete.Succeeded();
        check("bounded-detour-partial", passed, passed ? "" :
            std::string("partial=") + ToString(partial.status)
                + " actions=" + std::to_string(partial.path.movements.size())
                + " complete=" + ToString(complete.status));
    }

    // 12. Castle is the reference large-map contract: its graph survives the
    // map serialization pipeline and offers a multi-portal base-to-base route.
    {
        CreativeMapDocument castle;
        std::string error;
        RouteGraph graph;
        std::vector<std::string> graphErrors;
        const bool loaded = LoadCreativeMapDocument("maps/castle_bedwars.dbmap", castle, &error);
        const bool built = loaded && graph.Build(castle.routeNodes, castle.routeEdges, &graphErrors);
        World castleWorld;
        if (loaded)
        {
            for (const CreativeMapBlock& block : castle.blocks)
            {
                castleWorld.PlaceBlock(block.pos,
                    Block { block.type, block.teamId, block.breakable, block.variant }, true);
            }
        }
        std::vector<std::string> physicalErrors;
        const bool physicallyValid = built && graph.ValidatePhysical(castleWorld, &physicalErrors);
        const RouteCorridor corridor = built
            ? graph.FindCorridor(GridPos { -4, 53, -78 }, GridPos { 4, 53, 78 }, 0)
            : RouteCorridor {};
        const int bridgeSegments = static_cast<int>(std::count_if(
            corridor.segments.begin(), corridor.segments.end(),
            [](const RouteCorridorSegment& segment) { return segment.IsBridge(); }));
        const bool passed = loaded && built && physicallyValid
            && graph.NodeCount() >= 44 && graph.EdgeCount() >= 52
            && corridor.valid && corridor.nodeIndices.size() >= 16
            && bridgeSegments >= 2;
        check("castle-route-contract", passed, passed ? "" :
            (!loaded ? error : "nodes=" + std::to_string(graph.NodeCount())
                + " edges=" + std::to_string(graph.EdgeCount())
                + " corridor=" + std::to_string(corridor.nodeIndices.size())
                + " bridgeSegments=" + std::to_string(bridgeSegments)
                + " physicalErrors=" + std::to_string(physicalErrors.size())));
    }

    // Repeated bounded searches over deterministic noisy/maze-like geometry
    // exercise packed keys, arena reuse, clearance caching and decrease-key.
    // The generous floor catches accidental algorithmic regressions without
    // turning the smoke into a machine-specific performance contest.
    {
        World world;
        AddPlatform(world, -32, 32, -22, 22);
        int wallIndex = 0;
        for (int x = -24; x <= 24; x += 6, ++wallIndex)
        {
            const int gap = wallIndex % 2 == 0 ? 16 : -16;
            for (int z = -20; z <= 20; ++z)
            {
                if (std::abs(z - gap) <= 2) continue;
                world.PlaceBlock(GridPos { x, 1, z }, GroundBlock());
                world.PlaceBlock(GridPos { x, 2, z }, GroundBlock());
            }
        }
        NavigationWorldView view(world);
        NavigationProfile profile = PlannerProfile();
        NavigationSearchLimits limits;
        limits.maxExpansions = 3200;
        limits.maxSearchRadius = 64;
        limits.maxVerticalRange = 8;
        limits.maxActions = 256;
        GoalReachPosition goal(GridPos { 28, 0, 0 });
        VoxelPathfinder pathfinder;
        constexpr int iterations = 16;
        std::string fingerprint;
        std::uint64_t expanded = 0;
        bool deterministic = true;
        bool countersValid = true;
        const auto began = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < iterations; ++iteration)
        {
            const NavigationSearchResult result = pathfinder.FindPath(
                NavigationState { GridPos { -28, 0, 0 }, 0, 0 },
                goal, view, profile, limits);
            const std::string current = PathFingerprint(result.path);
            if (iteration == 0) fingerprint = current;
            else deterministic = deterministic && current == fingerprint;
            deterministic = deterministic && result.HasPath();
            countersValid = countersValid
                && result.path.generatedNodes >= result.path.expandedNodes
                && result.path.peakOpenNodes <= limits.maxOpenNodes;
            expanded += static_cast<std::uint64_t>(result.path.expandedNodes);
        }
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - began).count();
        const double nodesPerSecond = seconds > 0.0
            ? static_cast<double>(expanded) / seconds : 0.0;
        const bool passed = deterministic && countersValid
            && expanded > 1000 && nodesPerSecond > 10000.0;
        check("kernel-throughput", passed,
            "nodesPerSecond=" + std::to_string(static_cast<std::uint64_t>(nodesPerSecond))
            + " expanded=" + std::to_string(expanded));
    }

    // Short full BotAI integration run: catches per-tick goal churn without
    // spending minutes on a complete automatch.
    {
        selectedMode_ = MatchMode::FourTeams;
        selectedTeamSize_ = 4;
        selectedBotCount_ = MaxBotCountForSelection();
        navigationMetrics_ = NavigationMetrics {};
        SetupMatch();
        screen_ = GameScreen::Playing;
        std::vector<Vector3> starts;
        for (const Player& player : players_)
        {
            if (IsBotControlled(player.GetControlKind())) starts.push_back(player.GetPosition());
        }
        for (int tick = 0; tick < 600 && !matchSimulation_.HasWinner(); ++tick)
        {
            StepSimulationProfiled(kTick);
        }
        int movedBots = 0;
        std::size_t botIndex = 0;
        for (const Player& player : players_)
        {
            if (!IsBotControlled(player.GetControlKind()) || botIndex >= starts.size()) continue;
            const Vector3 start = starts[botIndex++];
            const Vector3 now = player.GetPosition();
            const float dx = now.x - start.x;
            const float dz = now.z - start.z;
            if (dx * dx + dz * dz > 1.0f) ++movedBots;
        }
        const bool passed = navigationMetrics_.pathRequests < 1800
            && navigationMetrics_.cancelledActions < 1200
            && movedBots >= 4;
        check("bot-integration-budget", passed, passed ? "" :
            "requests=" + std::to_string(navigationMetrics_.pathRequests)
            + " cancelled=" + std::to_string(navigationMetrics_.cancelledActions)
            + " moved=" + std::to_string(movedBots));
    }

    std::cout << (failures == 0 ? "NAVIGATION_SMOKE_OK" : "NAVIGATION_SMOKE_FAIL")
              << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
#else
int Game::RunNavigationSmoke()
{
    std::cout << "diagnostics are disabled in this build (DAIBED_DIAGNOSTICS=OFF)" << std::endl;
    return 100;
}
#endif

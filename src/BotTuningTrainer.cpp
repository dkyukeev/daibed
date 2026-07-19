#include "Game.h"

#include "Navigation/NavigationGoal.h"
#include "Navigation/NavigationWorldView.h"
#include "Navigation/PathExecutor.h"
#include "Navigation/VoxelPathfinder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace
{
constexpr float kTrainerTick = 1.0f / 60.0f;
constexpr int kTrialTicks = 480;

Block TrainerGroundBlock()
{
    return Block { BlockType::Solid, -1, false };
}

void AddTrainerPlatform(World& world, int minX, int maxX, int minZ, int maxZ)
{
    for (int x = minX; x <= maxX; ++x)
    {
        for (int z = minZ; z <= maxZ; ++z)
        {
            world.PlaceBlock(GridPos { x, 0, z }, TrainerGroundBlock());
        }
    }
}

float Mutated(float value, float sigma, std::mt19937& random)
{
    return value + std::normal_distribution<float>(0.0f, sigma)(random);
}

BotTuningGenome CrossAndMutate(
    const BotTuningGenome& first,
    const BotTuningGenome& second,
    int generation,
    int index,
    std::mt19937& random)
{
    BotTuningGenome child = first;
    std::bernoulli_distribution inheritSecond(0.5);
    const auto inherited = [&](float a, float b)
    {
        return inheritSecond(random) ? b : a;
    };
    child.navigationRunupDistanceBlocks = inherited(
        first.navigationRunupDistanceBlocks, second.navigationRunupDistanceBlocks);
    child.navigationTakeoffDelaySeconds = inherited(
        first.navigationTakeoffDelaySeconds, second.navigationTakeoffDelaySeconds);
    child.navigationTakeoffEdgeOffsetBlocks = inherited(
        first.navigationTakeoffEdgeOffsetBlocks, second.navigationTakeoffEdgeOffsetBlocks);
    child.navigationTakeoffGapScale = inherited(
        first.navigationTakeoffGapScale, second.navigationTakeoffGapScale);
    child.navigationAirControlScale = inherited(
        first.navigationAirControlScale, second.navigationAirControlScale);
    child.navigationLandingCorrectionGain = inherited(
        first.navigationLandingCorrectionGain, second.navigationLandingCorrectionGain);
    child.navigationFallRiskPenalty = inherited(
        first.navigationFallRiskPenalty, second.navigationFallRiskPenalty);

    child.navigationRunupDistanceBlocks = Mutated(
        child.navigationRunupDistanceBlocks, 0.18f, random);
    child.navigationTakeoffDelaySeconds = Mutated(
        child.navigationTakeoffDelaySeconds, 0.025f, random);
    child.navigationTakeoffEdgeOffsetBlocks = Mutated(
        child.navigationTakeoffEdgeOffsetBlocks, 0.035f, random);
    child.navigationTakeoffGapScale = Mutated(
        child.navigationTakeoffGapScale, 0.025f, random);
    child.navigationAirControlScale = Mutated(
        child.navigationAirControlScale, 0.08f, random);
    child.navigationLandingCorrectionGain = Mutated(
        child.navigationLandingCorrectionGain, 0.09f, random);
    child.navigationFallRiskPenalty = Mutated(
        child.navigationFallRiskPenalty, 0.20f, random);
    child.generation = generation;
    child.fitness = 0.0f;
    child.id = "parkour-g" + std::to_string(generation)
        + "-" + std::to_string(index);
    ClampBotTuningGenome(child);
    return child;
}

struct TrainerEvaluation
{
    float fitness = 0.0f;
    int successes = 0;
    int falls = 0;
    int totalTicks = 0;
};
}

bool Game::RunBotTuningTrainer(
    int generations,
    int populationSize,
    unsigned int seed,
    const std::string& outputPath)
{
    generations = std::clamp(generations, 1, 1000);
    populationSize = std::clamp(populationSize, 4, 128);
    if (seed == 0) seed = 0xD41BEDu;
    std::mt19937 random(seed);

    const auto evaluate = [this](const BotTuningGenome& genome)
    {
        TrainerEvaluation result;
        static constexpr float lateralOffsets[] { -0.22f, 0.0f, 0.22f };
        for (int gap = 1; gap <= 3; ++gap)
        {
            for (float lateralOffset : lateralOffsets)
            {
                world_.Clear();
                players_.clear();
                networkActionState_.clear();
                AddTrainerPlatform(world_, -4, 0, -2, 2);
                AddTrainerPlatform(world_, gap + 1, gap + 4, -2, 2);

                Player actor(
                    970 + gap, "genetic-parkour", 0,
                    Vector3 { 0.0f, 1.50f, lateralOffset }, false);
                actor.SetControlKind(PlayerControlKind::BotAuthoritative);
                players_.push_back(actor);

                NavigationProfile profile;
                profile.maxGapJumpBlocks = 3;
                profile.gapRunupDistanceBlocks = genome.navigationRunupDistanceBlocks;
                profile.gapTakeoffDelaySeconds = genome.navigationTakeoffDelaySeconds;
                profile.gapTakeoffEdgeOffsetBlocks = genome.navigationTakeoffEdgeOffsetBlocks;
                profile.gapTakeoffGapScale = genome.navigationTakeoffGapScale;
                profile.gapAirControlScale = genome.navigationAirControlScale;
                profile.gapLandingCorrectionGain = genome.navigationLandingCorrectionGain;
                profile.fallRiskPenalty = genome.navigationFallRiskPenalty;

                NavigationWorldView planningView(world_);
                NavigationSearchLimits limits;
                limits.maxExpansions = 1200;
                limits.maxSearchRadius = 16;
                limits.maxVerticalRange = 6;
                limits.maxActions = 24;
                GoalReachPosition goal(GridPos { gap + 1, 0, 0 });
                const NavigationSearchResult search = VoxelPathfinder {}.FindPath(
                    NavigationState { GridPos { 0, 0, 0 }, 0, 0 },
                    goal, planningView, profile, limits);

                PathExecutor executor;
                executor.SetPath(search.path);
                bool finished = false;
                bool fell = false;
                int elapsedTicks = kTrialTicks;
                if (!search.Succeeded()) elapsedTicks = 0;
                for (int tick = 1; search.Succeeded() && tick <= kTrialTicks; ++tick)
                {
                    NavigationWorldView view(world_);
                    const PathExecutionUpdate update = executor.Update(
                        players_.front(), view, profile, kTrainerTick,
                        static_cast<std::uint32_t>(tick));
                    PlayerCommand command;
                    command.controlledPlayerId = static_cast<std::uint32_t>(players_.front().GetId());
                    command.tick = static_cast<std::uint32_t>(tick);
                    command.aimYaw = players_.front().GetYaw();
                    command.selectedSlot = players_.front().GetSelectedSlot();
                    if (update.proposal.active) command = update.proposal.command;
                    ApplyPlayerCommand(players_.front(), command, kTrainerTick);
                    players_.front().UpdateTimers(kTrainerTick);
                    if (players_.front().GetPosition().y < -2.0f)
                    {
                        fell = true;
                        elapsedTicks = tick;
                        break;
                    }
                    if (update.pathFinished
                        || executor.Status() == MovementExecutionStatus::Succeeded)
                    {
                        finished = true;
                        elapsedTicks = tick;
                        break;
                    }
                    if (update.needsRepath)
                    {
                        elapsedTicks = tick;
                        break;
                    }
                }

                result.totalTicks += elapsedTicks;
                if (finished)
                {
                    ++result.successes;
                    result.fitness += 1000.0f
                        + static_cast<float>(kTrialTicks - elapsedTicks) * 0.8f;
                    const float landingError = std::fabs(players_.front().GetPosition().z);
                    result.fitness -= landingError * 180.0f;
                }
                else
                {
                    result.fitness -= fell ? 1500.0f : 900.0f;
                    if (fell) ++result.falls;
                }
            }
        }

        // Keep the path-planning risk gene calibrated to observed motor risk.
        // This cannot be gamed by reducing the actual fixed fall penalty above.
        const float desiredRiskPenalty = 0.5f
            + 3.5f * static_cast<float>(result.falls) / 9.0f;
        result.fitness -= std::fabs(
            genome.navigationFallRiskPenalty - desiredRiskPenalty) * 120.0f;
        return result;
    };

    BotTuningGenome base = BotTuningForTeam(0);
    ClampBotTuningGenome(base);
    std::vector<BotTuningGenome> population;
    population.reserve(static_cast<std::size_t>(populationSize));
    base.id = "parkour-seed";
    population.push_back(base);
    for (int index = 1; index < populationSize; ++index)
    {
        population.push_back(CrossAndMutate(base, base, 0, index, random));
    }

    BotTuningGenome bestEver = base;
    bestEver.fitness = -1.0e30f;
    for (int generation = 0; generation < generations; ++generation)
    {
        int bestSuccesses = 0;
        int bestFalls = 0;
        for (BotTuningGenome& genome : population)
        {
            const TrainerEvaluation evaluation = evaluate(genome);
            genome.fitness = evaluation.fitness;
            if (genome.fitness > bestEver.fitness)
            {
                bestEver = genome;
                bestSuccesses = evaluation.successes;
                bestFalls = evaluation.falls;
            }
        }
        std::sort(population.begin(), population.end(), [](const auto& lhs, const auto& rhs)
        {
            return lhs.fitness > rhs.fitness;
        });
        const TrainerEvaluation leader = evaluate(population.front());
        bestSuccesses = leader.successes;
        bestFalls = leader.falls;
        std::cout << "trainer generation " << (generation + 1) << '/' << generations
                  << " fitness=" << population.front().fitness
                  << " success=" << bestSuccesses << "/9"
                  << " falls=" << bestFalls
                  << " hash=" << BotTuningGenomeHash(population.front()) << '\n';

        if (generation + 1 == generations) break;
        const int parentCount = std::max(2, populationSize / 4);
        std::vector<BotTuningGenome> next;
        next.reserve(static_cast<std::size_t>(populationSize));
        next.push_back(population.front());
        next.front().generation = generation + 1;
        std::uniform_int_distribution<int> parent(0, parentCount - 1);
        while (static_cast<int>(next.size()) < populationSize)
        {
            next.push_back(CrossAndMutate(
                population[parent(random)], population[parent(random)],
                generation + 1, static_cast<int>(next.size()), random));
        }
        population = std::move(next);
    }

    bestEver.id = "parkour-trained";
    bestEver.generation = generations;
    ClampBotTuningGenome(bestEver);
    std::string error;
    if (!WriteBotTuningGenomeJsonFile(outputPath, bestEver, &error))
    {
        std::cerr << "trainer write failed: " << error << '\n';
        return false;
    }
    std::cout << "BOT_TRAINER_OK output=" << outputPath
              << " fitness=" << bestEver.fitness
              << " hash=" << BotTuningGenomeHash(bestEver) << '\n';
    return true;
}

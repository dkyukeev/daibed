#include "Game.h"

#include "Network/LocalServerSession.h"
#include "Network/LoopbackTransport.h"
#include "Network/NetworkProtocol.h"
#include "Network/NetworkTransport.h"
#include "Network/SnapshotVisibility.h"
#include "Platform/PreciseTimer.h"
#include "UiText.h"
#include "VecConvert.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <thread>

#define DrawText DrawTextUtf8
#define MeasureText MeasureTextUtf8

// Diagnostics: every --*-smoke self-test lives here, compiled only when
// DAIBED_DIAGNOSTICS is ON (developer/CI builds). Shipping builds get tiny
// stubs so the CLI surface stays stable while none of the test logic (or its
// scripted match manipulation) is in the binary. Keep production code out of
// this file: it may not exist in a release build.

#if DAIBED_DIAGNOSTICS

namespace
{
constexpr float kReconnectRespawnSeconds = 7.0f;
constexpr int kCombatFlagRecipientAttacker = 1 << 0;
constexpr int kCombatFlagRecipientTarget = 1 << 1;
constexpr int kHeroAbilityFlagWorldEffect = 1 << 6;
constexpr int kHeroAbilityFlagDirectedEffect = 1 << 7;

Vector3 AimDirectionFromCommand(const PlayerCommand& command)
{
    const float cosPitch = std::cos(command.aimPitch);
    return Vector3 {
        std::sin(command.aimYaw) * cosPitch,
        std::sin(command.aimPitch),
        -std::cos(command.aimYaw) * cosPitch
    };
}

float LengthVec3(Vec3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

float DistanceVec3(Vec3 a, Vec3 b)
{
    return LengthVec3(Vec3 { a.x - b.x, a.y - b.y, a.z - b.z });
}

void ApplyProjectileDefaults(EnergyProjectile& projectile)
{
    const ProjectileTuning* tuning = &kArrowTuning;
    if (projectile.kind == ProjectileKind::Fireball)
    {
        tuning = &kFireballTuning;
    }
    else if (projectile.kind == ProjectileKind::Molotov)
    {
        tuning = &kMolotovTuning;
    }

    projectile.damage = tuning->damage;
    projectile.radius = tuning->radius;
    projectile.explosionRadius = tuning->explosionRadius;
    projectile.gravity = tuning->gravity;
    if (projectile.kind == ProjectileKind::Blaster)
    {
        projectile.damage = kBlasterTuning.baseDamage;
        projectile.radius = 0.24f;
        projectile.gravity = kBlasterTuning.gravity;
        projectile.airDragPerTick = 0.996f;
        projectile.affectedByDrag = true;
        projectile.speedBasedDamage = true;
    }
}
} // namespace

int Game::RunUiScreenshotDiag()
{
    // Renders the menu screens for a few frames each and saves PNGs next to the
    // exe (ui_menu/ui_settings/ui_controls.png) — screenshot-based UI review
    // without driving the window from outside. Windowed only.
    struct Shot
    {
        GameScreen screen;
        const char* file;
    };
    const Shot shots[] {
        { GameScreen::MainMenu, "ui_menu.png" },
        { GameScreen::HeroSelect, "ui_hero_select.png" },
        { GameScreen::Settings, "ui_settings.png" },
        { GameScreen::Controls, "ui_controls.png" },
    };
    for (const Shot& shot : shots)
    {
        screen_ = shot.screen;
        for (int frame = 0; frame < 6; ++frame)
        {
            Render();
        }
        TakeScreenshot(shot.file);
        std::cout << "ui-screenshot: " << shot.file << '\n';
    }
    std::cout << "UI_SCREENSHOT_OK" << std::endl;
    return 0;
}

int Game::RunFrameProfileDiag()
{
    // Frame-time tracer: runs the REAL game loop (menu idle, then a stock match
    // with the user's current settings) and reports every frame slower than
    // 25 ms with an input/update/render split — hitch attribution by
    // measurement instead of guesswork. Windowed only.
    struct SlowFrame
    {
        float at;
        const char* phase;
        float total;
        float input;
        float update;
        float render;
    };
    std::vector<SlowFrame> slow;
    int frames = 0;
    float worst = 0.0f;
    const double t0 = GetTime();
    const auto runPhase = [&](const char* phase, double duration)
    {
        const double phaseStart = GetTime();
        while (GetTime() - phaseStart < duration && !WindowShouldClose())
        {
            const double f0 = GetTime();
            HandleInput();
            const double i1 = GetTime();
            Update(GetFrameTime());
            const double u1 = GetTime();
            Render();
            const double r1 = GetTime();
            ++frames;
            const float total = static_cast<float>((r1 - f0) * 1000.0);
            worst = std::max(worst, total);
            if (total > 25.0f)
            {
                slow.push_back(SlowFrame {
                    static_cast<float>(f0 - t0), phase, total,
                    static_cast<float>((i1 - f0) * 1000.0),
                    static_cast<float>((u1 - i1) * 1000.0),
                    static_cast<float>((r1 - u1) * 1000.0) });
            }
        }
    };

    runPhase("menu", 3.0);
    profilingEnabled_ = true;
    StartSelectedMatch();
    runPhase("match-start", 6.0);
    runPhase("match-later", 8.0);

    for (const SlowFrame& f : slow)
    {
        std::printf("[frame] t=%6.2fs %-12s total=%6.1fms input=%5.1f update=%6.1f render=%6.1f\n",
                    f.at, f.phase, f.total, f.input, f.update, f.render);
    }
    std::printf("frame-profile: frames=%d worst=%.1fms slow(>25ms)=%d\n",
                frames, worst, static_cast<int>(slow.size()));
    if (profileSimulationTicks_ > 0)
    {
        const double ticks = static_cast<double>(profileSimulationTicks_);
        std::printf(
            "per-tick: sim=%.2fms (bots=%.2f preview=%.2f fastPlace=%.2f mockNet=%.2f integrated=%.2f) ticks=%d\n",
            profileSimulationMs_ / ticks,
            profileBotsMs_ / ticks,
            profilePreviewMs_ / ticks,
            profileFastPlaceMs_ / ticks,
            profileMockNetMs_ / ticks,
            profileIntegratedMs_ / ticks,
            static_cast<int>(profileSimulationTicks_));
        std::printf(
            "path: calls=%d fullSearches=%d perTick=%.2f (noCache=%d targetMoved=%d timer=%d nearWp=%d) pathMs=%.1f\n",
            static_cast<int>(profilePathCalls_),
            profilePathFull_,
            static_cast<double>(profilePathFull_) / ticks,
            profilePathMissNoCache_,
            profilePathMissMoved_,
            profilePathMissTimer_,
            profilePathMissNear_,
            profilePathMs_);
    }
    std::cout << "FRAME_PROFILE_OK" << std::endl;
    return 0;
}

int Game::RunMapReviewDiag(const std::string& mapPath)
{
    // Loads a creative map through the same creative -> test-play flow the
    // user plays it with, asserts the castle-utility physics (auto-step onto
    // slabs/stairs without a jump, ladder climbing), then saves review
    // screenshots (map_review_*.png) around the spawn and map center.
    // Windowed only.
    CreativeMapDocument document;
    std::string mapError;
    if (!LoadCreativeMapDocument(mapPath, document, &mapError) || document.blocks.empty())
    {
        std::cout << "map-review: load FAIL (" << mapError << ")\nMAP_REVIEW_FAIL" << std::endl;
        return 9;
    }
    StartCreativeSessionFromDocument(document);
    StartCreativeMapTest();
    Player* player = GetLocalPlayer();
    if (player == nullptr)
    {
        std::cout << "map-review: no local player\nMAP_REVIEW_FAIL" << std::endl;
        return 9;
    }

    constexpr float kTickDt = 1.0f / 60.0f;

    // --- Physics strip far above the map: base row, then +0.5 steps built
    // from a slab, a stair, a block and a slab-topped block, ending at a
    // ladder wall.  The rise must be walked, while ladder climbing requires
    // an explicit jump command.
    const int baseY = 200;
    const auto placeTestBlock = [this](int x, int y, BlockType type, int variant)
    {
        world_.PlaceBlock(GridPos { x, y, 0 }, Block { type, -1, true, variant }, true);
    };
    for (int x = 0; x <= 7; ++x)
    {
        placeTestBlock(x, baseY, BlockType::StoneBlock, 0);
    }
    placeTestBlock(2, baseY + 1, BlockType::StoneSlabBlock, 0);           // top 201.0
    placeTestBlock(3, baseY + 1, BlockType::StoneBrickStairsBlock, 1);    // raised east: 201.0 -> 201.5
    placeTestBlock(4, baseY + 1, BlockType::StoneBlock, 0);               // top 201.5
    for (int x = 5; x <= 7; ++x)
    {
        placeTestBlock(x, baseY + 1, BlockType::StoneBlock, 0);
        placeTestBlock(x, baseY + 2, BlockType::StoneSlabBlock, 0);       // top 202.0
    }
    for (int y = baseY + 1; y <= baseY + 6; ++y)
    {
        placeTestBlock(8, y, BlockType::StoneBlock, 0);                   // ladder wall (too tall to top over)
    }
    for (int y = baseY + 2; y <= baseY + 5; ++y)
    {
        placeTestBlock(7, y, BlockType::LadderBlock, 5);                  // ladder on +X wall
    }

    player->SetPosition(Vec3 { 0.0f, static_cast<float>(baseY) + 1.45f, 0.0f });
    const float startY = player->GetPosition().y;
    float maxVerticalVelocity = 0.0f;
    // Phase 1: walk the slab/stair rise and stop before the ladder cell.
    int ticks = 0;
    while (ticks++ < 400 && player->GetPosition().x < 6.0f)
    {
        player->Move(Vector3 { 1.0f, 0.0f, 0.0f }, false, kTickDt, world_,
            false, false, 1.0f, Player::kHumanAutoStepHeight);
        maxVerticalVelocity = std::max(maxVerticalVelocity, player->GetVelocity().y);
    }
    const float steppedY = player->GetPosition().y;
    // 200.5 -> 202.0 ground gain walked with zero jump input; jump speed
    // (6.7+) in the velocity trace would mean the step logic failed.
    const bool autoStepOk = steppedY - startY > 1.15f && steppedY - startY < 1.9f
        && player->GetPosition().x >= 5.9f
        && maxVerticalVelocity < 4.0f;

    // Phase 2: forward alone must not climb the ladder.
    ticks = 0;
    while (ticks++ < 60)
    {
        player->Move(Vector3 { 1.0f, 0.0f, 0.0f }, false, kTickDt, world_,
            false, false, 1.0f, Player::kHumanAutoStepHeight);
    }
    const bool noAutoLadderClimb = player->GetPosition().y <= steppedY + 0.15f;

    // Phase 3: push into the ladder wall while holding jump until the climb
    // clears two extra blocks.
    ticks = 0;
    const float ladderTargetY = static_cast<float>(baseY) + 4.6f;
    while (ticks++ < 400 && player->GetPosition().y < ladderTargetY)
    {
        player->Move(Vector3 { 1.0f, 0.0f, 0.0f }, true, kTickDt, world_,
            false, false, 1.0f, Player::kHumanAutoStepHeight);
    }
    const float climbedY = player->GetPosition().y;
    const bool ladderOk = climbedY >= ladderTargetY;

    // --- Review screenshots: spawn corner + map center, four headings each.
    int shotIndex = 0;
    const auto captureShots = [this, &shotIndex](Vector3 anchor)
    {
        for (int heading = 0; heading < 4; ++heading)
        {
            Player* local = GetLocalPlayer();
            if (local == nullptr)
            {
                return;
            }
            local->SetPosition(Vec3 { anchor.x, anchor.y, anchor.z });
            local->SetYaw(PI * 0.5f * static_cast<float>(heading));
            for (int frame = 0; frame < 10; ++frame)
            {
                Update(1.0f / 60.0f);
                Render();
            }
            ++shotIndex;
            const std::string file = "map_review_" + std::to_string(shotIndex) + ".png";
            TakeScreenshot(file.c_str());
            std::cout << "map-review shot: " << file << '\n';
        }
    };
    Vector3 spawnAnchor { 0.0f, 60.0f, 0.0f };
    for (const CreativeSpecial& special : document.specials)
    {
        if (special.kind == CreativeSpecialKind::HeroSpawn)
        {
            spawnAnchor = Vector3 {
                static_cast<float>(special.pos.x),
                static_cast<float>(special.pos.y) + 2.0f,
                static_cast<float>(special.pos.z)
            };
            break;
        }
    }
    captureShots(spawnAnchor);
    const Vector3 centerAnchor { 0.0f, spawnAnchor.y + 12.0f, 0.0f };
    captureShots(centerAnchor);

    const bool ok = autoStepOk && noAutoLadderClimb && ladderOk;
    std::cout << "map-review: blocks=" << document.blocks.size()
              << " autoStep=" << (autoStepOk ? "ok" : "FAIL")
              << " (dy=" << steppedY - startY << " maxVy=" << maxVerticalVelocity << ")"
              << " ladder=" << (ladderOk ? "ok" : "FAIL")
              << " noAutoLadder=" << (noAutoLadderClimb ? "ok" : "FAIL")
              << " (dy=" << climbedY - steppedY << ")\n"
              << (ok ? "MAP_REVIEW_OK" : "MAP_REVIEW_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunNetworkSmoke()
{
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }

    serverSession_.Configure(serverConfig_);
    serverSession_.Start();

    // Build a real, headless match so the snapshot carries genuine data.
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();

    screen_ = GameScreen::Playing;

    // The controlled player is driven solely by the commands the server drains,
    // so the in-sim local update must not also self-drive it (double move).
    localPlayerServerDriven_ = true;

    const auto findPlayerById = [this](std::uint32_t id) -> Player*
    {
        for (Player& player : players_)
        {
            if (static_cast<std::uint32_t>(player.GetId()) == id)
            {
                return &player;
            }
        }
        return nullptr;
    };

    Player* controlled = GetLocalPlayer();
    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };
    const Vec3 startPos = controlled != nullptr ? toVec3(controlled->GetPosition()) : Vec3 {};
    const float startYaw = controlled != nullptr ? controlled->GetYaw() : 0.0f;
    const int startSlot = selectedHotbarSlot_;
    const int controlledId = controlled != nullptr ? controlled->GetId() : -1;
    const int controlledTeamId = controlled != nullptr ? controlled->GetTeamId() : -1;

    // Give the controlled player a distinctive private resource so the inventory
    // (owner-private state) is observably non-empty through the visibility filter.
    if (controlled != nullptr)
    {
        controlled->GetInventory().AddResource(ResourceType::Iron, 13);
    }
    // An enemy recipient on a different team, used to prove hidden state does not
    // leak (its own-team trap stays hidden, the controlled inventory is stripped).
    int enemyPlayerId = -1;
    for (const Player& player : matchSimulation_.Players())
    {
        if (player.GetTeamId() != controlledTeamId)
        {
            enemyPlayerId = player.GetId();
            break;
        }
    }

    constexpr int kSmokeTicks = 90; // ~1.5s at the simulation tick rate.
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    const float injectedAimYaw = startYaw; // fixed facing so movement is along one axis
    // Exercise the action-command path: fire the hero's first ability on tick 0
    // and observe the controlled effect (a cooldown is started, or it is a clean
    // no-op if the ability is not castable).
    const bool abilityReadyBefore = controlled != nullptr
        && controlled->IsHeroAbilityReady(HeroAbilitySlot::Active1);
    float abilityCooldownAfterCast = -1.0f;
    int commandsProcessed = 0;
    int commandsApplied = 0;
    int actionCommandsApplied = 0;
    const GridPos syntheticDeltaPos { 7, 21, -7 };
    const Vec3 syntheticProjectilePos { 12.0f, 30.0f, -5.0f };
    for (int i = 0; i < kSmokeTicks; ++i)
    {
        // Client side: assemble a command with real, checkable intent and hand
        // it to the transport/session. command.tick already carries the
        // authoritative MatchSimulation tick (set in BuildLocalPlayerCommand).
        PlayerCommand command = BuildLocalPlayerCommand();
        command.aimYaw = injectedAimYaw;
        command.moveForward = 1.0f;
        command.selectedSlot = i % kHotbarSlotCount;
        command.useAbility1 = (i == 0); // one action command, on the first tick
        serverSession_.SubmitCommand(command);

        // Server side: drain from the transport and forward into the
        // simulation's intake queue (transport buffer -> sim input buffer).
        for (const PlayerCommand& received : serverSession_.DrainCommands())
        {
            matchSimulation_.SubmitCommand(received);
        }

        // Simulation side: drain its intake queue and APPLY each command to the
        // controlled player before advancing the authoritative tick. Movement
        // via ApplyPlayerCommand, discrete actions via ApplyPlayerActionCommand.
        for (const PlayerCommand& received : matchSimulation_.DrainCommands())
        {
            ++commandsProcessed;
            Player* target = findPlayerById(received.controlledPlayerId);
            if (target != nullptr && target->IsAlive())
            {
                ApplyPlayerCommand(*target, received, fixedDt);
                ApplyPlayerActionCommand(*target, received);
                ++commandsApplied;
                ++actionCommandsApplied;
            }
        }

        if (i == 0 && controlled != nullptr)
        {
            abilityCooldownAfterCast = controlled->GetHeroState().active1.cooldownRemaining;
        }

        // Advance the rest of the authoritative world (clock via MatchSimulation,
        // bots, generators) by one fixed step.
        UpdateMatchSimulation(fixedDt);

        if (i == kSmokeTicks - 1)
        {
            BlockDelta delta;
            delta.tick = matchSimulation_.CurrentTick();
            delta.position = syntheticDeltaPos;
            delta.oldType = BlockType::Air;
            delta.newType = BlockType::StoneBlock;
            delta.oldTeamId = -1;
            delta.newTeamId = controlled != nullptr ? controlled->GetTeamId() : -1;
            delta.ownerPlayerId = controlled != nullptr ? controlled->GetId() : -1;
            delta.reason = BlockDeltaReason::ReplicationTest;
            matchSimulation_.RecordBlockDelta(delta);

            // Inject a synthetic projectile so a dynamic-entity section is
            // guaranteed non-empty and provably assembles from real Game state
            // (the world update already ran this tick, so it survives to the
            // snapshot unchanged). Distinctive position/kind make it findable.
            EnergyProjectile syntheticProjectile;
            syntheticProjectile.position =
                Vector3 { syntheticProjectilePos.x, syntheticProjectilePos.y, syntheticProjectilePos.z };
            syntheticProjectile.velocity = Vector3 { 1.0f, 0.0f, 0.0f };
            syntheticProjectile.ownerId = controlled != nullptr ? controlled->GetId() : -1;
            syntheticProjectile.ownerTeamId = controlled != nullptr ? controlled->GetTeamId() : -1;
            syntheticProjectile.kind = ProjectileKind::Fireball;
            syntheticProjectile.lifetime = 2.5f;
            syntheticProjectile.fireZone = true;
            projectiles_.push_back(syntheticProjectile);

            // Inject a synthetic Konvoy trap owned by the controlled player's
            // team. As OwnerTeam state it must reach allies but stay hidden from
            // enemy recipients of the filtered snapshot.
            KonvoyTrap syntheticTrap;
            syntheticTrap.position = Vector3 { 9.0f, 1.0f, 9.0f };
            syntheticTrap.ownerPlayerId = controlledId;
            syntheticTrap.ownerTeamId = controlledTeamId;
            syntheticTrap.lifetime = 8.0f;
            syntheticTrap.health = 48;
            konvoyTraps_.push_back(syntheticTrap);

            if (controlledTeamId >= 0 && controlledTeamId < static_cast<int>(teamChests_.size()))
            {
                teamChests_[controlledTeamId].AddResource(ResourceType::Iron, 7);
                ItemStack syntheticChestStack;
                syntheticChestStack.type = ItemType::MedKit;
                syntheticChestStack.count = 2;
                teamChests_[controlledTeamId].SwapSlot(12, syntheticChestStack);
            }
        }

        // Server publishes the replicated snapshot; the client reads it back.
        const MatchSnapshot snapshot = BuildNetworkSnapshot();
        serverSession_.PublishSnapshot(snapshot);
        matchSimulation_.ClearBlockDeltasThrough(snapshot.tick);
    }

    const Vec3 endPos = controlled != nullptr ? toVec3(controlled->GetPosition()) : Vec3 {};
    const float endYaw = controlled != nullptr ? controlled->GetYaw() : 0.0f;
    const int endSlot = selectedHotbarSlot_;
    const float movedDistance = (endPos - startPos).Length();

    localPlayerServerDriven_ = false;

    const MatchSnapshot& snapshot = serverSession_.LatestSnapshot();
    const ServerConfig& config = serverSession_.Config();

    // The pickup/dropped-item snapshot sections must reflect the live (visible,
    // i.e. not-collected) world items — proving they assemble from real state,
    // not an empty stub. The published snapshot was built from the same state
    // that matchSimulation_ still holds (no updates ran after it).
    std::size_t visiblePickups = 0;
    for (const ResourcePickup& pickup : matchSimulation_.Pickups())
    {
        if (!pickup.collected)
        {
            ++visiblePickups;
        }
    }
    std::size_t visibleDropped = 0;
    for (const DroppedItem& dropped : matchSimulation_.DroppedItems())
    {
        if (!dropped.collected)
        {
            ++visibleDropped;
        }
    }
    const bool worldItemsReplicated = snapshot.pickups.size() == visiblePickups
        && snapshot.droppedItems.size() == visibleDropped;
    // The authoritative world legitimately emits its own block deltas on the
    // final tick (bots breaking/placing, fire), so don't require the synthetic
    // ReplicationTest delta to be the only one — just prove it round-trips into
    // the snapshot and that the live buffer was cleared after publish.
    const bool blockDeltaReplicated = std::any_of(
            snapshot.blockDeltas.begin(), snapshot.blockDeltas.end(),
            [&syntheticDeltaPos](const BlockDelta& delta)
            {
                return delta.position == syntheticDeltaPos
                    && delta.oldType == BlockType::Air
                    && delta.newType == BlockType::StoneBlock
                    && delta.reason == BlockDeltaReason::ReplicationTest;
            })
        && matchSimulation_.BlockDeltas().empty();

    // The dynamic-entity sections must carry real state, not a stub. Prove it via
    // the injected synthetic projectile round-tripping through the snapshot with
    // its public fields intact, and confirm the projectile section size matches
    // the live Game vector (every live projectile replicated, no filtering yet).
    bool syntheticProjectileReplicated = false;
    for (const ProjectileSnapshot& entry : snapshot.projectiles)
    {
        const bool samePos = entry.position.x == syntheticProjectilePos.x
            && entry.position.y == syntheticProjectilePos.y
            && entry.position.z == syntheticProjectilePos.z;
        if (samePos
            && entry.kind == static_cast<int>(ProjectileKind::Fireball)
            && entry.fireZone
            && entry.remainingLifetime > 0.0f
            && entry.visibility == SnapshotVisibility::Public)
        {
            syntheticProjectileReplicated = true;
            break;
        }
    }
    const bool dynamicEntitiesReplicated = syntheticProjectileReplicated
        && snapshot.projectiles.size() == projectiles_.size();

    // --- Per-client visibility filter ---------------------------------------
    // Derive the owner's and an enemy's filtered views from the FULL snapshot and
    // assert: team state (own-team trap) reaches the owner but NOT the enemy;
    // owner-private inventory reaches the owner but is stripped for the enemy;
    // public state (the synthetic projectile + full player roster) survives both.
    const MatchSnapshot ownerView = FilterSnapshotForClient(snapshot, controlledId);
    const MatchSnapshot enemyView = FilterSnapshotForClient(snapshot, enemyPlayerId);

    const int expectedIron = controlled != nullptr
        ? controlled->GetInventory().GetResource(ResourceType::Iron) : 0;
    const auto ownTeamTrapVisible = [controlledTeamId](const MatchSnapshot& v)
    {
        for (const HeroDeviceSnapshot& d : v.heroDevices)
        {
            if (d.type == HeroDeviceType::KonvoyTrap && d.ownerTeamId == controlledTeamId)
            {
                return true;
            }
        }
        return false;
    };
    const auto controlledInventory = [controlledId](const MatchSnapshot& v) -> const InventorySnapshot*
    {
        for (const PlayerSnapshot& p : v.players)
        {
            if (p.playerId == controlledId)
            {
                return &p.inventory;
            }
        }
        return nullptr;
    };
    const auto publicProjectileVisible = [&syntheticProjectilePos](const MatchSnapshot& v)
    {
        for (const ProjectileSnapshot& pr : v.projectiles)
        {
            if (pr.position.x == syntheticProjectilePos.x
                && pr.position.y == syntheticProjectilePos.y
                && pr.position.z == syntheticProjectilePos.z)
            {
                return true;
            }
        }
        return false;
    };
    const auto teamChestFor = [controlledTeamId](const MatchSnapshot& v) -> const TeamChestSnapshot*
    {
        for (const TeamChestSnapshot& chest : v.teamChests)
        {
            if (chest.teamId == controlledTeamId)
            {
                return &chest;
            }
        }
        return nullptr;
    };

    const InventorySnapshot* fullInv = controlledInventory(snapshot);
    const InventorySnapshot* ownerInv = controlledInventory(ownerView);
    const InventorySnapshot* enemyInv = controlledInventory(enemyView);
    const TeamChestSnapshot* fullChest = teamChestFor(snapshot);
    const TeamChestSnapshot* ownerChest = teamChestFor(ownerView);
    const TeamChestSnapshot* enemyChest = teamChestFor(enemyView);

    const bool teamStateHidden = ownTeamTrapVisible(snapshot)   // full has it
        && ownTeamTrapVisible(ownerView)                        // ally keeps it
        && !ownTeamTrapVisible(enemyView);                      // enemy never sees it
    const bool ownerPrivateStripped = fullInv != nullptr && fullInv->present
        && fullInv->resources[0] == expectedIron && expectedIron > 0
        && ownerInv != nullptr && ownerInv->present && ownerInv->resources[0] == expectedIron
        && enemyInv != nullptr && !enemyInv->present
        && enemyInv->resources[0] == 0 && enemyInv->hotbar.empty();
    const bool publicSurvivesFilter = publicProjectileVisible(snapshot)
        && publicProjectileVisible(ownerView)
        && publicProjectileVisible(enemyView)
        && enemyView.players.size() == snapshot.players.size(); // positions public (no fog)
    const bool teamChestVisibleToOwner = fullChest != nullptr
        && ownerChest != nullptr
        && fullChest->resources[0] == ownerChest->resources[0]
        && ownerChest->resources[0] >= 7
        && ownerChest->slots.size() > 12
        && ownerChest->slots[12].itemType == static_cast<int>(ItemType::MedKit)
        && ownerChest->slots[12].count == 2
        && enemyChest == nullptr
        && !enemyView.teamChests.empty();
    bool teamChestClientApplyOk = false;
    if (controlledTeamId >= 0 && controlledTeamId < static_cast<int>(teamChests_.size()) && ownerChest != nullptr)
    {
        ItemStack empty;
        teamChests_[controlledTeamId].SwapSlot(12, empty);
        ApplyClientSnapshot(ownerView);
        const Inventory& appliedChest = teamChests_[controlledTeamId];
        const ItemStack appliedStack = appliedChest.GetSlot(12);
        teamChestClientApplyOk =
            appliedChest.GetResource(ResourceType::Iron) == ownerChest->resources[0]
            && appliedStack.type == ItemType::MedKit
            && appliedStack.count == 2;
    }
    const bool visibilityFilterOk = enemyPlayerId >= 0
        && teamStateHidden && ownerPrivateStripped && publicSurvivesFilter
        && teamChestVisibleToOwner && teamChestClientApplyOk;

    // Selected slot + replicated Vec3 position of the controlled player as seen
    // in the snapshot.
    int snapshotControlledSlot = -1;
    Vec3 snapshotControlledPos {};
    if (controlled != nullptr)
    {
        for (const PlayerSnapshot& entry : snapshot.players)
        {
            if (entry.playerId == controlled->GetId())
            {
                snapshotControlledSlot = entry.selectedSlot;
                snapshotControlledPos = entry.position;
                break;
            }
        }
    }
    const int expectedSlot = (kSmokeTicks - 1) % kHotbarSlotCount;
    const float snapshotPosError = (snapshotControlledPos - endPos).Length();

    constexpr int kArtificialUnacked = 3;
    bool predictionSmokeOk = false;
    float predictionCorrectionLiveError = 1e9f;
    if (controlled != nullptr)
    {
        predictionHistory_.clear();
        remoteSnapshotBuffer_.clear();
        PushRemoteSnapshot(snapshot);

        PlayerCommand acknowledged = BuildLocalPlayerCommand();
        acknowledged.controlledPlayerId = static_cast<std::uint32_t>(controlled->GetId());
        acknowledged.tick = snapshot.tick;
        acknowledged.aimYaw = endYaw;
        acknowledged.moveForward = 0.0f;
        acknowledged.moveStrafe = 0.0f;
        acknowledged.jump = false;
        acknowledged.sprint = false;
        acknowledged.sprintTapped = false;
        acknowledged.selectedSlot = endSlot;

        PredictedCommandState wrongState;
        wrongState.command = acknowledged;
        wrongState.predictedPosition = Vec3 {
            snapshotControlledPos.x + 1.0f,
            snapshotControlledPos.y,
            snapshotControlledPos.z
        };
        wrongState.predictedVelocity = Vec3 {};
        wrongState.predictedYaw = endYaw;
        predictionHistory_.push_back(wrongState);

        for (int lag = 1; lag <= kArtificialUnacked; ++lag)
        {
            PlayerCommand pending = acknowledged;
            pending.tick = snapshot.tick + static_cast<std::uint32_t>(lag);
            PredictedCommandState futureState;
            futureState.command = pending;
            futureState.predictedPosition = snapshotControlledPos;
            futureState.predictedVelocity = Vec3 {};
            futureState.predictedYaw = endYaw;
            predictionHistory_.push_back(futureState);
        }

        const int correctionsBefore = predictionCorrectionsThisSecond_;
        ApplyAuthoritativeSnapshotForPrediction(snapshot, fixedDt);
        predictionCorrectionLiveError = (controlled->GetPositionVec3() - snapshotControlledPos).Length();
        predictionSmokeOk = predictionError_ > 0.9f
            && predictionCorrectionsThisSecond_ == correctionsBefore + 1
            && unackedCommandCount_ == kArtificialUnacked
            && predictionCorrectionLiveError < 0.25f
            && estimatedPingMs_ > 0.0f;
    }

    std::cout << "network-smoke: mode=" << ToString(networkMode_)
              << " server=\"" << config.serverName << "\""
              << " listen=" << config.listenAddress << ":" << config.port
              << " maxPlayers=" << config.maxPlayers
              << " private=" << (config.privateServer ? "yes" : "no")
              << " password=" << (config.HasPassword() ? "set" : "none") << '\n';
    std::cout << "network-smoke: simulationTick=" << matchSimulation_.CurrentTick()
              << " snapshot.tick=" << snapshot.tick
              << " tickRate=" << matchSimulation_.TickRate()
              << " fixedDt=" << fixedDt
              << " commandsProcessed=" << commandsProcessed
              << " commandsApplied=" << commandsApplied
              << " snapshotsPublished=" << serverSession_.SnapshotsPublished()
              << " phase=" << ToString(snapshot.phase)
              << " players=" << snapshot.players.size()
              << " cores=" << snapshot.cores.size()
              << " generators=" << snapshot.generators.size()
              << " pickups=" << snapshot.pickups.size()
              << " droppedItems=" << snapshot.droppedItems.size()
              << " blockDeltas=" << snapshot.blockDeltas.size()
              << " deltaBufferAfterPublish=" << matchSimulation_.BlockDeltas().size()
              << " matchTime=" << snapshot.matchTime << '\n';
    std::cout << "network-smoke: controlledId=" << (controlled != nullptr ? controlled->GetId() : -1)
              << " movedDistance=" << movedDistance
              << " startPos=(" << startPos.x << ',' << startPos.y << ',' << startPos.z << ')'
              << " endPos=(" << endPos.x << ',' << endPos.y << ',' << endPos.z << ')'
              << " yaw=" << startYaw << "->" << endYaw << " (cmd aimYaw=" << injectedAimYaw << ')'
              << " slot=" << startSlot << "->" << endSlot
              << " (snapshot=" << snapshotControlledSlot << ", expected=" << expectedSlot << ')'
              << " snapshotPos=(" << snapshotControlledPos.x << ',' << snapshotControlledPos.y
              << ',' << snapshotControlledPos.z << ") posError=" << snapshotPosError << '\n';
    std::cout << "network-smoke: actionCommandsApplied=" << actionCommandsApplied
              << " ability1ReadyBefore=" << (abilityReadyBefore ? "yes" : "no")
              << " ability1CooldownAfterCast=" << abilityCooldownAfterCast
              << (abilityReadyBefore && abilityCooldownAfterCast > 0.0f
                      ? " (controlled effect: cooldown started)"
                      : " (no-op)") << '\n';
    std::cout << "network-smoke: dynamicEntities projectiles=" << snapshot.projectiles.size()
              << " explosives=" << snapshot.explosives.size()
              << " hazardZones=" << snapshot.hazardZones.size()
              << " heroDevices=" << snapshot.heroDevices.size()
              << " statusEffects=" << snapshot.statusEffects.size()
              << " syntheticProjectile=" << (syntheticProjectileReplicated ? "found" : "MISSING")
              << '\n';
    std::cout << "network-smoke: visibility ownerId=" << controlledId
              << " enemyId=" << enemyPlayerId
              << " trap[full/owner/enemy]=" << ownTeamTrapVisible(snapshot)
              << '/' << ownTeamTrapVisible(ownerView) << '/' << ownTeamTrapVisible(enemyView)
              << " inv.present[full/owner/enemy]=" << (fullInv != nullptr && fullInv->present)
              << '/' << (ownerInv != nullptr && ownerInv->present)
              << '/' << (enemyInv != nullptr && enemyInv->present)
              << " inv.iron[full/owner/enemy]=" << (fullInv != nullptr ? fullInv->resources[0] : -1)
              << '/' << (ownerInv != nullptr ? ownerInv->resources[0] : -1)
              << '/' << (enemyInv != nullptr ? enemyInv->resources[0] : -1)
              << " publicProjectile[owner/enemy]=" << publicProjectileVisible(ownerView)
              << '/' << publicProjectileVisible(enemyView)
              << " chest[full/owner/enemy/apply]=" << (fullChest != nullptr)
              << '/' << (ownerChest != nullptr)
              << '/' << (enemyChest != nullptr)
              << '/' << teamChestClientApplyOk
              << " players[full/enemy]=" << snapshot.players.size() << '/' << enemyView.players.size()
              << '\n';
    std::cout << "network-smoke: prediction error=" << predictionError_
              << " correctionLiveError=" << predictionCorrectionLiveError
              << " unacked=" << unackedCommandCount_
              << " pingMs=" << estimatedPingMs_
              << " correction=" << (predictionSmokeOk ? "ok" : "FAIL") << '\n';

    const bool commandMoved = movedDistance > 0.5f;
    const bool slotApplied = endSlot == expectedSlot && snapshotControlledSlot == expectedSlot;
    const bool aimApplied = std::fabs(endYaw - injectedAimYaw) < 0.001f;
    // The snapshot's raylib-free Vec3 position must match the live position.
    const bool snapshotPosOk = snapshotPosError < 0.001f;
    // The action-command path ran for every applied command (ability no-op or
    // controlled effect) without disturbing movement.
    const bool actionApplied = actionCommandsApplied == kSmokeTicks;
    const bool ok = !snapshot.players.empty()
        && !snapshot.cores.empty()
        && !snapshot.generators.empty()
        && matchSimulation_.CurrentTick() == static_cast<std::uint32_t>(kSmokeTicks)
        && commandsProcessed == kSmokeTicks
        && commandsApplied == kSmokeTicks
        && snapshot.tick == static_cast<std::uint32_t>(kSmokeTicks)
        && commandMoved
        && slotApplied
        && aimApplied
        && snapshotPosOk
        && worldItemsReplicated
        && blockDeltaReplicated
        && dynamicEntitiesReplicated
        && visibilityFilterOk
        && actionApplied
        && predictionSmokeOk;

    if (!ok)
    {
        std::cout << "network-smoke: checks moved=" << (commandMoved ? "ok" : "FAIL")
                  << " slot=" << (slotApplied ? "ok" : "FAIL")
                  << " aim=" << (aimApplied ? "ok" : "FAIL")
                  << " snapshotPos=" << (snapshotPosOk ? "ok" : "FAIL")
                  << " worldItems=" << (worldItemsReplicated ? "ok" : "FAIL")
                  << " blockDelta=" << (blockDeltaReplicated ? "ok" : "FAIL")
                  << " dynamicEntities=" << (dynamicEntitiesReplicated ? "ok" : "FAIL")
                  << " visibility=" << (visibilityFilterOk ? "ok" : "FAIL")
                  << "(team=" << (teamStateHidden ? "ok" : "FAIL")
                  << ",ownerPriv=" << (ownerPrivateStripped ? "ok" : "FAIL")
                  << ",public=" << (publicSurvivesFilter ? "ok" : "FAIL")
                  << ",chestVisible=" << (teamChestVisibleToOwner ? "ok" : "FAIL")
                  << ",chestApply=" << (teamChestClientApplyOk ? "ok" : "FAIL") << ')'
                  << " action=" << (actionApplied ? "ok" : "FAIL")
                  << " prediction=" << (predictionSmokeOk ? "ok" : "FAIL") << '\n';
    }

    std::cout << (ok ? "NETWORK_SMOKE_OK" : "NETWORK_SMOKE_FAIL") << std::endl;
    serverSession_.Stop();
    return ok ? 0 : 4;
}

int Game::RunMovementParitySmoke()
{
    arenaBiome_ = ArenaBiome::Ice;
    world_.Clear();
    for (int x = -4; x <= 80; ++x)
    {
        for (int z = -3; z <= 3; ++z)
        {
            world_.PlaceBlock(GridPos { x, 0, z }, Block { BlockType::GrassBlock, -1, false }, true);
        }
    }
    for (int x = 28; x <= 42; ++x)
    {
        for (int z = -3; z <= 3; ++z)
        {
            world_.PlaceBlock(GridPos { x, 0, z }, Block { BlockType::IceBlock, -1, false }, true);
        }
    }

    Player localHuman(1, "local-human", 0, Vector3 { 0.0f, 1.5f, 0.0f }, true);
    localHuman.SetControlKind(PlayerControlKind::LocalHumanPredicted);
    Player remoteHuman(2, "remote-human", 1, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    remoteHuman.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);

    PlayerCommand command;
    command.aimYaw = 3.1415926535f * 0.5f;
    command.moveForward = 1.0f;
    command.sprint = true;
    command.selectedSlot = 0;

    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    constexpr int kTicks = 180;
    for (int i = 0; i < kTicks; ++i)
    {
        command.tick = static_cast<std::uint32_t>(i + 1);
        command.controlledPlayerId = static_cast<std::uint32_t>(localHuman.GetId());
        ApplyPlayerCommand(localHuman, command, fixedDt);
        command.controlledPlayerId = static_cast<std::uint32_t>(remoteHuman.GetId());
        ApplyPlayerCommand(remoteHuman, command, fixedDt);
    }

    const float positionError = (localHuman.GetPositionVec3() - remoteHuman.GetPositionVec3()).Length();
    const float velocityError = (localHuman.GetVelocityVec3() - remoteHuman.GetVelocityVec3()).Length();
    const bool sameRoleProfile =
        IsHumanControlled(ControlKindForPlayer(localHuman))
        && IsHumanControlled(ControlKindForPlayer(remoteHuman))
        && IsLocallyPredicted(ControlKindForPlayer(localHuman))
        && !IsLocallyPredicted(ControlKindForPlayer(remoteHuman));
    const bool movementParity = positionError < 0.001f && velocityError < 0.001f;
    const bool localCameraOnly = HasLocalCamera(ControlKindForPlayer(localHuman))
        && !HasLocalCamera(ControlKindForPlayer(remoteHuman));

    world_.Clear();
    world_.PlaceBlock(GridPos { 0, 0, 0 }, Block { BlockType::GrassBlock, -1, false }, true);
    Player predictedSpring(3, "predicted-spring", 0, Vector3 { 0.0f, 1.5f, 0.0f }, true);
    predictedSpring.SetControlKind(PlayerControlKind::LocalHumanPredicted);
    Player authoritativeSpring(4, "authoritative-spring", 1, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    authoritativeSpring.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
    PlayerCommand springCommand;
    springCommand.selectedSlot = 0;
    for (int i = 0; i < 30; ++i)
    {
        springCommand.tick = static_cast<std::uint32_t>(i + 1);
        springCommand.controlledPlayerId = static_cast<std::uint32_t>(predictedSpring.GetId());
        ApplyPlayerCommand(predictedSpring, springCommand, fixedDt);
        springCommand.controlledPlayerId = static_cast<std::uint32_t>(authoritativeSpring.GetId());
        ApplyPlayerCommand(authoritativeSpring, springCommand, fixedDt);
    }
    world_.PlaceBlock(GridPos { 0, 0, 0 }, Block { BlockType::SpringBlock, -1, false }, true);
    for (int i = 0; i < 4; ++i)
    {
        springCommand.tick = static_cast<std::uint32_t>(31 + i);
        springCommand.controlledPlayerId = static_cast<std::uint32_t>(predictedSpring.GetId());
        ApplyPlayerCommand(predictedSpring, springCommand, fixedDt);
        ApplyStandingBlockEffects(predictedSpring, true);
        springCommand.controlledPlayerId = static_cast<std::uint32_t>(authoritativeSpring.GetId());
        ApplyPlayerCommand(authoritativeSpring, springCommand, fixedDt);
        ApplyStandingBlockEffects(authoritativeSpring, false);
    }
    const float springPositionError =
        (predictedSpring.GetPositionVec3() - authoritativeSpring.GetPositionVec3()).Length();
    const float springVelocityError =
        (predictedSpring.GetVelocityVec3() - authoritativeSpring.GetVelocityVec3()).Length();
    const bool springApplied = predictedSpring.GetVelocity().y > 4.0f
        && authoritativeSpring.GetVelocity().y > 4.0f;
    const bool springParity = springApplied
        && springPositionError < 0.001f
        && springVelocityError < 0.001f;

    world_.Clear();
    for (int x = -4; x <= 12; ++x)
    {
        for (int z = -2; z <= 2; ++z)
        {
            world_.PlaceBlock(GridPos { x, 0, z }, Block { BlockType::GrassBlock, -1, false }, true);
        }
    }
    matchSimulation_.Players().clear();
    networkControlledPlayerIds_.clear();
    serverHeldPlayerCommands_.clear();
    serverEffectiveCommandTickByPlayer_.clear();
    Player batchedServerPlayer(10, "batched-server", 0, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    batchedServerPlayer.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
    matchSimulation_.Players().push_back(batchedServerPlayer);
    MarkNetworkControlledPlayer(10);

    Player expectedSingleStep(11, "single-step", 0, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    expectedSingleStep.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
    PlayerCommand batchCommand;
    batchCommand.controlledPlayerId = 10;
    batchCommand.aimYaw = 3.1415926535f * 0.5f;
    batchCommand.moveForward = 1.0f;
    batchCommand.selectedSlot = 0;
    std::vector<ReceivedCommand> receivedBatch;
    for (int i = 0; i < 3; ++i)
    {
        PlayerCommand batchedCommand = batchCommand;
        batchedCommand.tick = static_cast<std::uint32_t>(i + 1);
        receivedBatch.push_back(ReceivedCommand { 100, batchedCommand });
    }
    ApplyBatchedServerCommands(receivedBatch, fixedDt);
    batchCommand.controlledPlayerId = 11;
    batchCommand.tick = 3;
    ApplyPlayerCommand(expectedSingleStep, batchCommand, fixedDt);
    ApplyStandingBlockEffects(expectedSingleStep, false);

    const Player* batchedAfter = matchSimulation_.GetPlayer(10);
    const float batchPositionError = batchedAfter != nullptr
        ? (batchedAfter->GetPositionVec3() - expectedSingleStep.GetPositionVec3()).Length()
        : 999.0f;
    ApplyBatchedServerCommands({}, fixedDt);
    ++batchCommand.tick;
    ApplyPlayerCommand(expectedSingleStep, batchCommand, fixedDt);
    ApplyStandingBlockEffects(expectedSingleStep, false);
    const Player* heldAfter = matchSimulation_.GetPlayer(10);
    const float heldPositionError = heldAfter != nullptr
        ? (heldAfter->GetPositionVec3() - expectedSingleStep.GetPositionVec3()).Length()
        : 999.0f;
    const bool batchPacing = batchPositionError < 0.001f && heldPositionError < 0.001f;

    world_.Clear();
    for (int x = -4; x <= 24; ++x)
    {
        for (int z = -2; z <= 2; ++z)
        {
            world_.PlaceBlock(GridPos { x, 0, z }, Block { BlockType::GrassBlock, -1, false }, true);
        }
    }
    Player predictedOrbita(12, "predicted-orbita", 0, Vector3 { 0.0f, 1.5f, 0.0f }, true);
    predictedOrbita.SetControlKind(PlayerControlKind::LocalHumanPredicted);
    predictedOrbita.SetHeroId(HeroId::Orbita);
    Player authoritativeOrbita(13, "authoritative-orbita", 1, Vector3 { 0.0f, 1.5f, 0.0f }, false);
    authoritativeOrbita.SetControlKind(PlayerControlKind::RemoteHumanAuthoritative);
    authoritativeOrbita.SetHeroId(HeroId::Orbita);

    PlayerCommand orbitaCommand;
    orbitaCommand.aimYaw = 3.1415926535f * 0.5f;
    orbitaCommand.selectedSlot = 0;
    orbitaCommand.useAbility1 = true;
    constexpr int kOrbitaDashSmokeTicks = 8;
    for (int i = 0; i < kOrbitaDashSmokeTicks; ++i)
    {
        orbitaCommand.tick = static_cast<std::uint32_t>(i + 1);
        orbitaCommand.controlledPlayerId = static_cast<std::uint32_t>(predictedOrbita.GetId());
        ApplyPredictedPlayerCommand(predictedOrbita, orbitaCommand, fixedDt);

        orbitaCommand.controlledPlayerId = static_cast<std::uint32_t>(authoritativeOrbita.GetId());
        ApplyPlayerCommand(authoritativeOrbita, orbitaCommand, fixedDt);
        ApplyStandingBlockEffects(authoritativeOrbita, false);
        ApplyPlayerActionCommand(authoritativeOrbita, orbitaCommand);
        StepOrbitaDash(authoritativeOrbita, fixedDt);

        orbitaCommand.useAbility1 = false;
    }
    const float orbitaDashMoved =
        (predictedOrbita.GetPositionVec3() - Vec3 { 0.0f, 1.5f, 0.0f }).Length();
    const float orbitaDashPositionError =
        (predictedOrbita.GetPositionVec3() - authoritativeOrbita.GetPositionVec3()).Length();
    const bool orbitaDashPrediction = orbitaDashMoved > 1.0f && orbitaDashPositionError < 0.001f;

    const bool ok = sameRoleProfile && movementParity && localCameraOnly && springParity
        && batchPacing && orbitaDashPrediction;

    std::cout << "movement-parity-smoke: posError=" << positionError
              << " velError=" << velocityError
              << " springPosError=" << springPositionError
              << " springVelError=" << springVelocityError
              << " batchPosError=" << batchPositionError
              << " heldPosError=" << heldPositionError
              << " orbitaDashMoved=" << orbitaDashMoved
              << " orbitaDashPosError=" << orbitaDashPositionError
              << " localKind=" << static_cast<int>(ControlKindForPlayer(localHuman))
              << " remoteKind=" << static_cast<int>(ControlKindForPlayer(remoteHuman))
              << " parity=" << (movementParity ? "ok" : "FAIL")
              << " spring=" << (springParity ? "ok" : "FAIL")
              << " batching=" << (batchPacing ? "ok" : "FAIL")
              << " orbitaDash=" << (orbitaDashPrediction ? "ok" : "FAIL")
              << " roles=" << (sameRoleProfile ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "MOVEMENT_PARITY_SMOKE_OK" : "MOVEMENT_PARITY_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 4;
}

int Game::RunNetworkPurchaseSmoke()
{
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }

    // Real headless match so shop/team/inventory are genuine, not stubs.
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();
    screen_ = GameScreen::Playing;

    Player* controlled = GetLocalPlayer();
    Team* team = controlled != nullptr ? FindTeam(controlled->GetTeamId()) : nullptr;
    if (controlled == nullptr || team == nullptr)
    {
        std::cout << "purchase-smoke: no controlled player/team\n";
        std::cout << "PURCHASE_SMOKE_FAIL" << std::endl;
        return 4;
    }

    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };
    Inventory& inv = controlled->GetInventory();

    // Stand the player on the shop so the server-side proximity gate passes.
    controlled->SetPosition(toVec3(team->shopPosition));

    suppressLocalFeedback_ = false;
    const std::string messageBefore = message_;
    const std::size_t eventMessagesBefore = eventMessages_.size();
    const std::size_t worldEffectsBefore = worldEffects_.size();
    const std::size_t floatingTextsBefore = floatingTexts_.size();
    const bool audioMutedBefore = audio_.IsMuted();

    // --- Case 1: a valid purchase spends resources and grants the item. ------
    // Choice 1 = 16 wool blocks for 4 Iron (no secondary cost). Guarantee funds.
    inv.AddResource(ResourceType::Iron, 4);
    const int ironBefore = inv.GetResource(ResourceType::Iron);
    const int woolBefore = inv.GetBlockCount(BlockType::WoolBlock);

    QueueEconomyAction(PlayerActionType::BuyItem, /*choice*/ 1, /*repeat*/ 1);
    PlayerCommand buy = BuildLocalPlayerCommand();
    const PlayerActionResult buyResult = ApplyPlayerEconomyCommand(*controlled, buy);
    const int ironAfter = inv.GetResource(ResourceType::Iron);
    const int woolAfter = inv.GetBlockCount(BlockType::WoolBlock);
    const bool buyOk = buyResult.handled && buyResult.success
        && !buyResult.message.empty()
        && ironAfter == ironBefore - 4 && woolAfter == woolBefore + 16;

    // --- Case 2: re-sending the SAME command (same seq) must NOT buy again. ---
    const PlayerActionResult duplicateResult = ApplyPlayerEconomyCommand(*controlled, buy);
    const bool dedupeOk = !duplicateResult.handled && !duplicateResult.success
        && inv.GetResource(ResourceType::Iron) == ironAfter
        && inv.GetBlockCount(BlockType::WoolBlock) == woolAfter;

    // Choice 2 is the premium wood slot: 16 planks for 4 Gold.
    inv.AddResource(ResourceType::Gold, 4);
    const int goldBeforeWood = inv.GetResource(ResourceType::Gold);
    const int woodBefore = inv.GetBlockCount(BlockType::WoodBlock);
    QueueEconomyAction(PlayerActionType::BuyItem, /*choice*/ 2, /*repeat*/ 1);
    const PlayerActionResult woodBuyResult = ApplyPlayerEconomyCommand(*controlled, BuildLocalPlayerCommand());
    const bool premiumWoodOk = woodBuyResult.handled && woodBuyResult.success
        && inv.GetResource(ResourceType::Gold) == goldBeforeWood - 4
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBefore + 16
        && BreakSeconds(BlockType::WoodBlock, 0) > 1.2f
        && BreakSeconds(BlockType::WoodBlock, 0, true) < 0.6f;

    // --- Case 3: denied for lack of resources (no state change). -------------
    inv.SpendResource(ResourceType::Gold, inv.GetResource(ResourceType::Gold));
    inv.SpendResource(ResourceType::Crystal, inv.GetResource(ResourceType::Crystal));
    const int ironBeforeBroke = inv.GetResource(ResourceType::Iron);
    QueueEconomyAction(PlayerActionType::BuyItem, /*choice 4 = obsidian, costs Gold+Crystal*/ 4, 1);
    PlayerCommand brokeBuy = BuildLocalPlayerCommand();
    const PlayerActionResult brokeResult = ApplyPlayerEconomyCommand(*controlled, brokeBuy);
    const bool deniedFundsOk = brokeResult.handled && !brokeResult.success
        && !brokeResult.message.empty()
        && inv.GetResource(ResourceType::Iron) == ironBeforeBroke
        && inv.GetResource(ResourceType::Gold) == 0
        && inv.GetResource(ResourceType::Crystal) == 0;

    // --- Case 4: denied when out of the shop zone, even with funds. ----------
    controlled->SetPosition(toVec3(Vector3 {
        team->shopPosition.x + 100.0f, team->shopPosition.y, team->shopPosition.z }));
    inv.AddResource(ResourceType::Iron, 50);
    const int ironBeforeFar = inv.GetResource(ResourceType::Iron);
    const int woodBeforeFar = inv.GetBlockCount(BlockType::WoodBlock);
    QueueEconomyAction(PlayerActionType::BuyItem, 1, 1);
    PlayerCommand farBuy = BuildLocalPlayerCommand();
    const PlayerActionResult farResult = ApplyPlayerEconomyCommand(*controlled, farBuy);
    const bool deniedRangeOk = farResult.handled && !farResult.success
        && !farResult.message.empty()
        && inv.GetResource(ResourceType::Iron) == ironBeforeFar
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBeforeFar;

    // --- Case 5: inventory drop rides the same deduped action channel. -------
    ItemStack dropStack;
    dropStack.type = ItemType::Fireball;
    dropStack.count = 3;
    inv.SwapSlot(3, dropStack);
    const std::size_t droppedBefore = matchSimulation_.DroppedItems().size();
    QueueEconomyAction(PlayerActionType::DropItem, /*slot*/ 3, /*count*/ 2);
    PlayerCommand dropCommand = BuildLocalPlayerCommand();
    const PlayerActionResult dropResult = ApplyPlayerEconomyCommand(*controlled, dropCommand);
    const ItemStack dropSlotAfter = inv.GetSlot(3);
    const bool dropOk = dropResult.handled && dropResult.success
        && matchSimulation_.DroppedItems().size() == droppedBefore + 1
        && matchSimulation_.DroppedItems().back().stack.type == ItemType::Fireball
        && matchSimulation_.DroppedItems().back().stack.count == 2
        && dropSlotAfter.type == ItemType::Fireball
        && dropSlotAfter.count == 1;
    const PlayerActionResult dropDuplicate = ApplyPlayerEconomyCommand(*controlled, dropCommand);
    const bool dropDedupeOk = !dropDuplicate.handled
        && matchSimulation_.DroppedItems().size() == droppedBefore + 1
        && inv.GetSlot(3).count == 1;

    // --- Case 6: quick-move transfers a stack across hotbar/main inventory. ---
    ItemStack moveStack;
    moveStack.type = ItemType::MedKit;
    moveStack.count = 2;
    inv.SwapSlot(4, moveStack);
    const ItemStack mainSlotBeforeMove = inv.GetSlot(kHotbarSlotCount);
    inv.SwapSlot(kHotbarSlotCount, ItemStack {});
    QueueEconomyAction(PlayerActionType::MoveInventory, /*slot*/ 4, 0);
    PlayerCommand moveCommand = BuildLocalPlayerCommand();
    const PlayerActionResult moveResult = ApplyPlayerEconomyCommand(*controlled, moveCommand);
    const bool moveOk = moveResult.handled && moveResult.success
        && inv.GetSlot(4).IsEmpty()
        && inv.GetSlot(kHotbarSlotCount).type == ItemType::MedKit
        && inv.GetSlot(kHotbarSlotCount).count == 2;
    if (!mainSlotBeforeMove.IsEmpty())
    {
        inv.SwapSlot(kHotbarSlotCount, mainSlotBeforeMove);
    }

    // --- Case 7: team chest transfer is server-owned and deduped. ------------
    controlled->SetPosition(toVec3(TeamChestDepositPosition(*team)));
    Inventory& teamChest = teamChests_[team->id];
    ItemStack chestStack;
    chestStack.type = ItemType::AlarmTrap;
    chestStack.count = 2;
    inv.SwapSlot(6, chestStack);
    const int invAlarmBeforeChest = inv.CountItem(ItemType::AlarmTrap);
    const int chestAlarmBefore = teamChest.CountItem(ItemType::AlarmTrap);
    QueueEconomyAction(PlayerActionType::ChestTransfer, /*slot*/ 6, /*deposit*/ 0);
    PlayerCommand chestDepositCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestDepositResult = ApplyPlayerEconomyCommand(*controlled, chestDepositCommand);
    const bool chestDepositOk = chestDepositResult.handled && chestDepositResult.success
        && inv.CountItem(ItemType::AlarmTrap) == invAlarmBeforeChest - 2
        && teamChest.CountItem(ItemType::AlarmTrap) == chestAlarmBefore + 2;
    const PlayerActionResult chestDepositDuplicate = ApplyPlayerEconomyCommand(*controlled, chestDepositCommand);
    const bool chestDedupeOk = !chestDepositDuplicate.handled
        && inv.CountItem(ItemType::AlarmTrap) == invAlarmBeforeChest - 2
        && teamChest.CountItem(ItemType::AlarmTrap) == chestAlarmBefore + 2;
    ItemStack exactChestStack;
    exactChestStack.type = ItemType::Molotov;
    exactChestStack.count = 1;
    teamChest.SwapSlot(8, exactChestStack);
    const int invMolotovBeforeExactChest = inv.CountItem(ItemType::Molotov);
    QueueEconomyAction(PlayerActionType::ChestTransfer, /*slot*/ 8, /*withdraw exact*/ 2);
    PlayerCommand chestExactWithdrawCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestExactWithdrawResult = ApplyPlayerEconomyCommand(*controlled, chestExactWithdrawCommand);
    const bool chestExactWithdrawOk = chestExactWithdrawResult.handled && chestExactWithdrawResult.success
        && inv.CountItem(ItemType::Molotov) == invMolotovBeforeExactChest + 1
        && teamChest.GetSlot(8).IsEmpty();
    QueueEconomyAction(PlayerActionType::ChestTransfer, /*unused*/ 0, /*withdraw*/ 1);
    PlayerCommand chestWithdrawCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestWithdrawResult = ApplyPlayerEconomyCommand(*controlled, chestWithdrawCommand);
    const bool chestWithdrawOk = chestWithdrawResult.handled && chestWithdrawResult.success
        && inv.CountItem(ItemType::AlarmTrap) == invAlarmBeforeChest
        && teamChest.CountItem(ItemType::AlarmTrap) == chestAlarmBefore;

    ItemStack exactDepositStack;
    exactDepositStack.type = ItemType::MedKit;
    exactDepositStack.count = 2;
    inv.SwapSlot(10, exactDepositStack);
    teamChest.SwapSlot(14, ItemStack {});
    QueueEconomyAction(
        PlayerActionType::ChestTransfer,
        10,
        PackPlayerActionParam(static_cast<int>(ChestTransferOp::PlayerToChestSlot), 14, 0));
    PlayerCommand chestExactDepositCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestExactDepositResult =
        ApplyPlayerEconomyCommand(*controlled, chestExactDepositCommand);
    const bool chestExactDepositOk = chestExactDepositResult.handled && chestExactDepositResult.success
        && inv.GetSlot(10).IsEmpty()
        && teamChest.GetSlot(14).type == ItemType::MedKit
        && teamChest.GetSlot(14).count == 2;
    inv.SwapSlot(11, ItemStack {});
    QueueEconomyAction(
        PlayerActionType::ChestTransfer,
        14,
        PackPlayerActionParam(static_cast<int>(ChestTransferOp::ChestToPlayerSlot), 11, 0));
    PlayerCommand chestExactTakeCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestExactTakeResult =
        ApplyPlayerEconomyCommand(*controlled, chestExactTakeCommand);
    const bool chestExactTakeOk = chestExactTakeResult.handled && chestExactTakeResult.success
        && teamChest.GetSlot(14).IsEmpty()
        && inv.GetSlot(11).type == ItemType::MedKit
        && inv.GetSlot(11).count == 2;
    ItemStack chestMoveStack;
    chestMoveStack.type = ItemType::DashPearl;
    chestMoveStack.count = 3;
    teamChest.SwapSlot(20, chestMoveStack);
    teamChest.SwapSlot(21, ItemStack {});
    QueueEconomyAction(
        PlayerActionType::ChestTransfer,
        20,
        PackPlayerActionParam(static_cast<int>(ChestTransferOp::ChestToChestSlot), 21, 0));
    PlayerCommand chestMoveCommand = BuildLocalPlayerCommand();
    const PlayerActionResult chestMoveResult = ApplyPlayerEconomyCommand(*controlled, chestMoveCommand);
    const bool chestMoveOk = chestMoveResult.handled && chestMoveResult.success
        && teamChest.GetSlot(20).IsEmpty()
        && teamChest.GetSlot(21).type == ItemType::DashPearl
        && teamChest.GetSlot(21).count == 3;
    ItemStack inventoryMoveExactStack;
    inventoryMoveExactStack.type = ItemType::Molotov;
    inventoryMoveExactStack.count = 2;
    inv.SwapSlot(12, inventoryMoveExactStack);
    inv.SwapSlot(13, ItemStack {});
    QueueEconomyAction(
        PlayerActionType::MoveInventory,
        12,
        PackPlayerActionParam(static_cast<int>(InventoryMoveOp::SlotToSlot), 13, 0));
    PlayerCommand moveExactCommand = BuildLocalPlayerCommand();
    const PlayerActionResult moveExactResult = ApplyPlayerEconomyCommand(*controlled, moveExactCommand);
    const bool moveExactOk = moveExactResult.handled && moveExactResult.success
        && inv.GetSlot(12).IsEmpty()
        && inv.GetSlot(13).type == ItemType::Molotov
        && inv.GetSlot(13).count == 2;
    recentActionResults_.clear();

    // Forge upgrades speed intervals smoothly; level IV also creates a rare,
    // deterministic crystal after enough successful team-generator spawns.
    std::vector<ResourcePickup> normalForgePickups;
    Generator normalForge(ResourceType::Iron, Vec3 { 500.0f, 50.0f, 500.0f }, 1.0f, 1, 0);
    normalForge.Update(0.75f, normalForgePickups, 0, 0);
    std::vector<ResourcePickup> maxForgePickups;
    Generator maxForge(ResourceType::Iron, Vec3 { 510.0f, 50.0f, 510.0f }, 1.0f, 1, 0);
    maxForge.Update(0.75f, maxForgePickups, 4, 0);
    for (int i = 1; i < 64; ++i)
    {
        for (ResourcePickup& pickup : maxForgePickups) pickup.collected = true;
        maxForge.Update(1.0f, maxForgePickups, 4, 0);
    }
    const bool forgeOk = normalForgePickups.empty()
        && !maxForgePickups.empty()
        && std::any_of(maxForgePickups.begin(), maxForgePickups.end(), [](const ResourcePickup& pickup)
        {
            return pickup.type == ResourceType::Crystal;
        });

    // Base healing now pulses every 0.5s instead of every 0.25s.
    Player* healTarget = nullptr;
    Team* healTeam = nullptr;
    for (Player& candidate : players_)
    {
        if (!HasLocalCamera(ControlKindForPlayer(candidate)))
        {
            healTarget = &candidate;
            healTeam = FindTeam(candidate.GetTeamId());
            break;
        }
    }
    if (healTarget != nullptr && healTeam != nullptr)
    {
        healTarget->SetPosition(toVec3(healTeam->shopPosition));
        healTarget->Damage(40);
    }
    const int healthBeforeBaseHeal = healTarget != nullptr ? healTarget->GetHealth() : 0;
    baseHealTimer_ = 0.0f;
    UpdateBaseHealing(0.49f);
    const bool noEarlyBaseHeal = healTarget != nullptr && healTarget->GetHealth() == healthBeforeBaseHeal;
    UpdateBaseHealing(0.01f);
    const bool baseHealOk = noEarlyBaseHeal
        && healTarget->GetHealth() == healthBeforeBaseHeal + 3;

    // Respawns choose a random cell in the three-block team area and climb
    // above player-placed obstructions instead of embedding in them.
    const Vector3 authoredSpawn = team->spawnPoint;
    team->spawnPoint = Vector3 { 600.0f, 51.5f, 600.0f };
    for (int x = -3; x <= 3; ++x)
    {
        for (int z = -3; z <= 3; ++z)
        {
            if (x * x + z * z <= 9)
            {
                world_.PlaceBlock(GridPos { 600 + x, 50, 600 + z }, Block { BlockType::StoneBlock, team->id, true }, true);
            }
        }
    }
    Player spawnProbe(9001, "spawn-probe", team->id, team->spawnPoint, false);
    const Vector3 randomSpawnA = FindTeamRespawnPosition(spawnProbe);
    bool sawDifferentRandomSpawn = false;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const Vector3 sample = FindTeamRespawnPosition(spawnProbe);
        sawDifferentRandomSpawn = sawDifferentRandomSpawn
            || sample.x != randomSpawnA.x || sample.z != randomSpawnA.z;
    }
    const float radiusSqA = (randomSpawnA.x - team->spawnPoint.x) * (randomSpawnA.x - team->spawnPoint.x)
        + (randomSpawnA.z - team->spawnPoint.z) * (randomSpawnA.z - team->spawnPoint.z);
    const bool randomSpawnOk = radiusSqA <= 9.01f
        && randomSpawnA.y == team->spawnPoint.y
        && sawDifferentRandomSpawn;
    for (int x = -3; x <= 3; ++x)
    {
        for (int z = -3; z <= 3; ++z)
        {
            if (x * x + z * z <= 9)
            {
                world_.PlaceBlock(GridPos { 600 + x, 51, 600 + z }, Block { BlockType::WoodBlock, team->id, true }, true);
            }
        }
    }
    const Vector3 raisedSpawn = FindTeamRespawnPosition(spawnProbe);
    const bool raisedSpawnOk = raisedSpawn.y > team->spawnPoint.y
        && !world_.CollidesWithAABB(raisedSpawn, Vector3 { 0.36f, 0.95f, 0.36f });
    team->spawnPoint = authoredSpawn;
    PushPlayerActionResultSnapshot(buyResult);
    int enemyPlayerId = -1;
    for (const Player& player : matchSimulation_.Players())
    {
        if (player.GetId() != controlled->GetId())
        {
            enemyPlayerId = player.GetId();
            break;
        }
    }
    const MatchSnapshot actionResultSnapshot = BuildNetworkSnapshot();
    const MatchSnapshot ownerActionResultView = FilterSnapshotForClient(actionResultSnapshot, controlled->GetId());
    const MatchSnapshot enemyActionResultView = enemyPlayerId >= 0
        ? FilterSnapshotForClient(actionResultSnapshot, enemyPlayerId)
        : MatchSnapshot {};
    const bool actionResultReplicated = ownerActionResultView.actionResults.size() == 1
        && ownerActionResultView.actionResults[0].playerId == controlled->GetId()
        && ownerActionResultView.actionResults[0].resultSeq != 0
        && ownerActionResultView.actionResults[0].actionSeq == buyResult.actionSeq
        && ownerActionResultView.actionResults[0].success == buyResult.success
        && ownerActionResultView.actionResults[0].message == buyResult.message
        && enemyPlayerId >= 0
        && enemyActionResultView.actionResults.empty();
    recentActionResults_.clear();

    const bool presentationClean =
        message_ == messageBefore
        && eventMessages_.size() == eventMessagesBefore
        && worldEffects_.size() == worldEffectsBefore
        && floatingTexts_.size() == floatingTextsBefore
        && audio_.IsMuted() == audioMutedBefore;

    const bool ok = buyOk && dedupeOk && premiumWoodOk && deniedFundsOk && deniedRangeOk
        && dropOk && dropDedupeOk && moveOk
        && chestDepositOk && chestDedupeOk && chestExactWithdrawOk && chestWithdrawOk
        && chestExactDepositOk && chestExactTakeOk && chestMoveOk && moveExactOk
        && actionResultReplicated && presentationClean && forgeOk && baseHealOk
        && randomSpawnOk && raisedSpawnOk;
    std::cout << "purchase-smoke: buy=" << (buyOk ? "ok" : "FAIL")
              << " (iron " << ironBefore << "->" << ironAfter
              << ", wool " << woolBefore << "->" << woolAfter << ")"
              << " dedupe=" << (dedupeOk ? "ok" : "FAIL")
              << " premiumWood=" << (premiumWoodOk ? "ok" : "FAIL")
              << " deniedFunds=" << (deniedFundsOk ? "ok" : "FAIL")
              << " deniedRange=" << (deniedRangeOk ? "ok" : "FAIL")
              << " drop=" << (dropOk ? "ok" : "FAIL")
              << " dropDedupe=" << (dropDedupeOk ? "ok" : "FAIL")
              << " move=" << (moveOk ? "ok" : "FAIL")
              << " chestDeposit=" << (chestDepositOk ? "ok" : "FAIL")
              << " chestDedupe=" << (chestDedupeOk ? "ok" : "FAIL")
              << " chestExactWithdraw=" << (chestExactWithdrawOk ? "ok" : "FAIL")
              << " chestWithdraw=" << (chestWithdrawOk ? "ok" : "FAIL")
              << " chestExactDeposit=" << (chestExactDepositOk ? "ok" : "FAIL")
              << " chestExactTake=" << (chestExactTakeOk ? "ok" : "FAIL")
              << " chestMove=" << (chestMoveOk ? "ok" : "FAIL")
              << " moveExact=" << (moveExactOk ? "ok" : "FAIL")
              << " actionResult=" << (actionResultReplicated ? "ok" : "FAIL")
              << " forge=" << (forgeOk ? "ok" : "FAIL")
              << " baseHeal=" << (baseHealOk ? "ok" : "FAIL")
              << " randomSpawn=" << (randomSpawnOk ? "ok" : "FAIL")
              << "(" << randomSpawnA.x << "," << randomSpawnA.y << "," << randomSpawnA.z
              << "; varied=" << (sawDifferentRandomSpawn ? "yes" : "no") << ")"
              << " raisedSpawn=" << (raisedSpawnOk ? "ok" : "FAIL")
              << " presentation=" << (presentationClean ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "PURCHASE_SMOKE_OK" : "PURCHASE_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 4;
}

int Game::RunNetworkClientUiSmoke()
{
    networkMode_ = NetworkMode::LocalHost;
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();
    screen_ = GameScreen::Playing;

    Player* serverPlayer = GetLocalPlayer();
    Team* team = serverPlayer != nullptr ? FindTeam(serverPlayer->GetTeamId()) : nullptr;
    if (serverPlayer == nullptr || team == nullptr)
    {
        std::cout << "network-client-ui-smoke: missing server player/team\n";
        std::cout << "NETWORK_CLIENT_UI_SMOKE_FAIL" << std::endl;
        return 4;
    }

    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };
    const int playerId = serverPlayer->GetId();
    serverPlayer->SetPosition(toVec3(team->shopPosition));
    serverPlayer->GetInventory().AddResource(ResourceType::Iron, 32);

    Game client;
    client.Initialize(true);
    client.networkMode_ = NetworkMode::LocalClient;
    client.networkAssignedPlayerId_ = playerId;
    client.localPlayerId_ = playerId;
    LobbySnapshot lobby;
    lobby.worldBiome = static_cast<int>(arenaBiome_);
    lobby.worldLayout = static_cast<int>(arenaLayout_);
    lobby.matchMode = static_cast<int>(selectedMode_);
    client.BuildClientWorld(lobby);
    client.networkAssignedPlayerId_ = playerId;
    client.localPlayerId_ = playerId;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));

    Player* clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    const bool clientReady = clientPlayer != nullptr;

    client.currentInput_ = PlayerInput {};
    client.currentInput_.interactPressed = true;
    client.HandleNetworkClientUiInput();
    const bool shopOpenOk = client.shopOpen_ && !client.inventoryOpen_ && !client.personalChestOpen_;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.exitPressed = true;
    client.HandleNetworkClientUiInput();
    const bool shopEscCloseOk = !client.shopOpen_ && !client.inventoryOpen_ && !client.clientPaused_;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.interactPressed = true;
    client.HandleNetworkClientUiInput();

    const int clientWoolBeforeShop = clientPlayer != nullptr
        ? clientPlayer->GetInventory().GetBlockCount(BlockType::WoolBlock)
        : 0;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.shopChoice = 1;
    if (clientPlayer != nullptr)
    {
        client.HandleNetworkClientShopInput(*clientPlayer);
    }
    const PlayerCommand buyCommand = client.BuildLocalPlayerCommand();
    const bool buyQueued = buyCommand.actionSeq != 0
        && buyCommand.actionType == static_cast<int>(PlayerActionType::BuyItem)
        && clientPlayer != nullptr
        && clientPlayer->GetInventory().GetBlockCount(BlockType::WoolBlock) == clientWoolBeforeShop;
    const PlayerActionResult buyResult = ApplyPlayerEconomyCommand(*serverPlayer, buyCommand);
    client.pendingEconomyActionType_ = PlayerActionType::None;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    const bool buyReflected = buyResult.handled && buyResult.success
        && clientPlayer != nullptr
        && clientPlayer->GetInventory().GetBlockCount(BlockType::WoolBlock) >= clientWoolBeforeShop + 16;

    serverPlayer->SetPosition(toVec3(TeamChestDepositPosition(*team)));
    ItemStack chestSource;
    chestSource.type = ItemType::MedKit;
    chestSource.count = 2;
    serverPlayer->GetInventory().SwapSlot(10, chestSource);
    teamChests_[team->id].SwapSlot(14, ItemStack {});
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    const bool chestOpenOk = clientPlayer != nullptr
        && client.OpenTeamChestUi(*clientPlayer)
        && client.inventoryOpen_
        && client.teamChestOpen_
        && !client.personalChestOpen_;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.exitPressed = true;
    client.HandleNetworkClientUiInput();
    const bool chestEscCloseOk = !client.inventoryOpen_ && !client.teamChestOpen_ && !client.clientPaused_;

    const int clientChestMedBefore = client.teamChests_[team->id].CountItem(ItemType::MedKit);
    client.QueuePlayerAction(
        PlayerActionType::ChestTransfer,
        10,
        PackPlayerActionParam(static_cast<int>(ChestTransferOp::PlayerToChestSlot), 14, 0));
    const PlayerCommand chestCommand = client.BuildLocalPlayerCommand();
    const bool chestQueued = chestCommand.actionSeq != 0
        && chestCommand.actionType == static_cast<int>(PlayerActionType::ChestTransfer)
        && client.teamChests_[team->id].CountItem(ItemType::MedKit) == clientChestMedBefore;
    const PlayerActionResult chestResult = ApplyPlayerEconomyCommand(*serverPlayer, chestCommand);
    client.pendingEconomyActionType_ = PlayerActionType::None;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    const bool chestReflected = chestResult.handled && chestResult.success
        && client.teamChests_[team->id].GetSlot(14).type == ItemType::MedKit
        && client.teamChests_[team->id].GetSlot(14).count == 2;

    ItemStack moveSource;
    moveSource.type = ItemType::Molotov;
    moveSource.count = 1;
    serverPlayer->GetInventory().SwapSlot(12, moveSource);
    serverPlayer->GetInventory().SwapSlot(13, ItemStack {});
    ItemStack dropSource;
    dropSource.type = ItemType::Fireball;
    dropSource.count = 3;
    serverPlayer->GetInventory().SwapSlot(3, dropSource);
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    client.QueuePlayerAction(
        PlayerActionType::MoveInventory,
        12,
        PackPlayerActionParam(static_cast<int>(InventoryMoveOp::SlotToSlot), 13, 0));
    const PlayerCommand moveCommand = client.BuildLocalPlayerCommand();
    const PlayerActionResult moveResult = ApplyPlayerEconomyCommand(*serverPlayer, moveCommand);
    client.pendingEconomyActionType_ = PlayerActionType::None;
    client.QueuePlayerAction(PlayerActionType::DropItem, 3, 2);
    const PlayerCommand dropCommand = client.BuildLocalPlayerCommand();
    const std::size_t droppedBefore = matchSimulation_.DroppedItems().size();
    const PlayerActionResult dropResult = ApplyPlayerEconomyCommand(*serverPlayer, dropCommand);
    client.pendingEconomyActionType_ = PlayerActionType::None;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    const bool moveDropReflected = moveResult.handled && moveResult.success
        && dropResult.handled && dropResult.success
        && matchSimulation_.DroppedItems().size() == droppedBefore + 1
        && clientPlayer != nullptr
        && clientPlayer->GetInventory().GetSlot(13).type == ItemType::Molotov
        && clientPlayer->GetInventory().GetSlot(3).type == ItemType::Fireball
        && clientPlayer->GetInventory().GetSlot(3).count == 1;

    PlayerMatchScore& score = GetPlayerScore(playerId);
    score.kills = 3;
    score.deaths = 2;
    score.finalDeaths = 1;
    score.coreDamage = 77;
    score.coresDestroyed = 1;
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    const PlayerMatchScore* clientScore = client.FindPlayerScore(playerId);
    const bool scoreReplicated = clientScore != nullptr
        && clientScore->kills == 3
        && clientScore->deaths == 2
        && clientScore->finalDeaths == 1
        && clientScore->coreDamage == 77
        && clientScore->coresDestroyed == 1;

    client.networkMode_ = NetworkMode::LocalClient;
    client.scoreboardHeld_ = false;
    client.scoreboardHeld_ = true;
    const bool tabScoreboardOk = client.scoreboardHeld_;

    serverPlayer->GetInventory().SwapSlot(0, ItemStack { ItemType::SniperRifle, 1 });
    client.ApplyClientSnapshot(BuildNetworkSnapshotForClient(playerId));
    clientPlayer = client.matchSimulation_.GetPlayer(playerId);
    if (clientPlayer != nullptr)
    {
        clientPlayer->SetControlKind(PlayerControlKind::LocalHumanPredicted);
    }
    client.selectedHotbarSlot_ = 0;
    client.sniperMagnification_ = 1.5f;
    client.currentInput_ = PlayerInput {};
    client.currentInput_.scopeHeld = true;
    client.currentInput_.mouseWheel = 1.0f;
    const int slotBeforeScopeWheel = client.selectedHotbarSlot_;
    if (clientPlayer != nullptr)
    {
        client.HandleNetworkClientLookAndHotbarInput(*clientPlayer);
    }
    const bool sniperZoomWheelOk = clientPlayer != nullptr
        && client.selectedHotbarSlot_ == slotBeforeScopeWheel
        && client.sniperMagnification_ > 1.5f;

    PlayerCommand chargeCommand;
    chargeCommand.controlledPlayerId = static_cast<std::uint32_t>(playerId);
    chargeCommand.selectedSlot = 0;
    chargeCommand.attackHeld = true;
    if (clientPlayer != nullptr)
    {
        clientPlayer->CancelBlasterLoading();
        client.UpdatePredictedRangedCharge(*clientPlayer, chargeCommand, 0.20f);
    }
    const bool sniperChargePredicted = clientPlayer != nullptr
        && clientPlayer->GetBlasterState() == CrossbowState::Loading
        && clientPlayer->GetBlasterLoadTimer() > 0.0f;

    client.currentInput_ = PlayerInput {};
    client.currentInput_.exitPressed = true;
    client.HandleNetworkClientUiInput();
    const bool escPauseOk = client.clientPaused_ && !client.inventoryOpen_ && !client.shopOpen_;

    const bool personalHidden = !client.personalChestOpen_;
    const bool ok = clientReady && shopOpenOk && buyQueued && buyReflected
        && chestOpenOk && chestQueued && chestReflected
        && moveDropReflected && scoreReplicated && tabScoreboardOk
        && shopEscCloseOk && chestEscCloseOk && escPauseOk
        && sniperZoomWheelOk && sniperChargePredicted
        && personalHidden;
    std::cout << "network-client-ui-smoke: clientReady=" << (clientReady ? "ok" : "FAIL")
              << " shopOpen=" << (shopOpenOk ? "ok" : "FAIL")
              << " shopEsc=" << (shopEscCloseOk ? "ok" : "FAIL")
              << " buyQueued=" << (buyQueued ? "ok" : "FAIL")
              << " buySnapshot=" << (buyReflected ? "ok" : "FAIL")
              << " chestOpen=" << (chestOpenOk ? "ok" : "FAIL")
              << " chestEsc=" << (chestEscCloseOk ? "ok" : "FAIL")
              << " chestQueued=" << (chestQueued ? "ok" : "FAIL")
              << " chestSnapshot=" << (chestReflected ? "ok" : "FAIL")
              << " moveDropSnapshot=" << (moveDropReflected ? "ok" : "FAIL")
              << " score=" << (scoreReplicated ? "ok" : "FAIL")
              << " tab=" << (tabScoreboardOk ? "ok" : "FAIL")
              << " escPause=" << (escPauseOk ? "ok" : "FAIL")
              << " sniperZoom=" << (sniperZoomWheelOk ? "ok" : "FAIL")
              << " sniperCharge=" << (sniperChargePredicted ? "ok" : "FAIL")
              << " personalHidden=" << (personalHidden ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "NETWORK_CLIENT_UI_SMOKE_OK" : "NETWORK_CLIENT_UI_SMOKE_FAIL") << std::endl;
    client.Shutdown();
    return ok ? 0 : 4;
}

int Game::RunIntegratedServerSmoke()
{
    // Phase 6 skeleton: plain singleplayer connects the local human as a loopback
    // client of the same authoritative pipeline multiplayer uses. Hybrid — the
    // direct SP path still drives gameplay; economy is the first migrated system.
    // Pin the roster + difficulty so the test is independent of a persisted
    // DaiBed.settings (0 bots / team size 1 changes the roster; Hard bots +
    // aggressive positioning can kill the stationary controlled player between
    // test steps, breaking assertions that assume it survives).
    botDifficulty_ = BotDifficulty::Normal;
    selectedTeamSize_ = 4;
    selectedBotCount_ = 15;
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();
    screen_ = GameScreen::Playing;

    Player* controlled = GetLocalPlayer();
    Team* team = controlled != nullptr ? FindTeam(controlled->GetTeamId()) : nullptr;
    if (controlled == nullptr || team == nullptr)
    {
        std::cout << "integrated-server-smoke: no controlled player/team\n";
        std::cout << "INTEGRATED_SERVER_SMOKE_FAIL" << std::endl;
        return 4;
    }

    // SetupMatch auto-starts the integrated server for LocalSinglePlayer.
    const bool startedOk = integratedServerActive_ && integratedServer_.IsRunning()
        && integratedServer_.PlayerForClient(kIntegratedServerClientId) == controlled->GetId()
        && integratedServer_.ClientForPlayer(controlled->GetId()) == kIntegratedServerClientId;

    // --- Part 1: the per-tick channel runs alongside the unchanged SP sim. ----
    // Headless UpdateMatchSimulation skips the presentation block that ticks the
    // integrated server in-game, so the smoke drives the tick explicitly.
    constexpr int kTicks = 30;
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    for (int i = 0; i < kTicks; ++i)
    {
        UpdateMatchSimulation(fixedDt);
        IntegratedServerTick(fixedDt);
    }

    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };
    const MatchSnapshot& snap = integratedServer_.LatestSnapshot(kIntegratedServerClientId);
    const PlayerSnapshot* self = nullptr;
    for (const PlayerSnapshot& entry : snap.players)
    {
        if (entry.playerId == controlled->GetId())
        {
            self = &entry;
            break;
        }
    }
    const float posError = self != nullptr
        ? (self->position - toVec3(controlled->GetPosition())).Length()
        : 1e9f;
    const bool channelOk =
        integratedServerCommandsDrained_ == static_cast<std::uint32_t>(kTicks)
        && integratedServer_.SnapshotsPublished() == static_cast<std::uint32_t>(kTicks)
        && snap.tick == matchSimulation_.CurrentTick()
        && self != nullptr && posError < 0.001f
        && self->inventory.present;

    // --- Part 2: a shop purchase crosses the SAME transport + server method. --
    // Headless initialization suppresses local feedback; the smoke asserts the
    // result message is presented, so re-enable it like the purchase smoke does.
    suppressLocalFeedback_ = false;
    Inventory& inv = controlled->GetInventory();
    controlled->SetPosition(toVec3(team->shopPosition));
    inv.AddResource(ResourceType::Iron, 4);
    const int ironBefore = inv.GetResource(ResourceType::Iron);
    const int woolBefore = inv.GetBlockCount(BlockType::WoolBlock);
    const std::uint32_t drainedBeforeBuy = integratedServerCommandsDrained_;
    QueuePlayerAction(PlayerActionType::BuyItem, /*choice*/ 1, /*repeat*/ 1);
    ApplyPendingLocalPlayerAction(*controlled);
    const bool buyOk = integratedServerCommandsDrained_ == drainedBeforeBuy + 1
        && integratedServerEconomyApplied_ == 1
        && inv.GetResource(ResourceType::Iron) == ironBefore - 4
        && inv.GetBlockCount(BlockType::WoolBlock) == woolBefore + 16
        && !message_.empty()
        && pendingEconomyActionType_ == PlayerActionType::None;

    // Resending the SAME wire command must not buy twice (server-side dedupe).
    PlayerCommand resent = BuildLocalPlayerCommand();
    resent.actionSeq = clientEconomyActionSeq_;
    resent.actionType = static_cast<int>(PlayerActionType::BuyItem);
    resent.actionParamA = 1;
    resent.actionParamB = 1;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, resent);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool dedupeOk = integratedServerEconomyApplied_ == 1
        && inv.GetResource(ResourceType::Iron) == ironBefore - 4
        && inv.GetBlockCount(BlockType::WoolBlock) == woolBefore + 16;

    // A denied purchase (out of shop range) also crosses the transport and is
    // rejected by the same server validation multiplayer relies on.
    controlled->SetPosition(toVec3(Vector3 {
        team->shopPosition.x + 100.0f, team->shopPosition.y, team->shopPosition.z }));
    const int ironBeforeFar = inv.GetResource(ResourceType::Iron);
    QueuePlayerAction(PlayerActionType::BuyItem, 1, 1);
    ApplyPendingLocalPlayerAction(*controlled);
    const bool deniedOk = integratedServerEconomyApplied_ == 2
        && inv.GetResource(ResourceType::Iron) == ironBeforeFar
        && inv.GetBlockCount(BlockType::WoolBlock) == woolBefore + 16;

    // --- Part 3 (block slice): a block place crosses the SAME transport and the
    // authoritative place path (ApplyNetworkBlockPlace) — and lands exactly once.
    controlled->SetPosition(Vec3 { 0.0f, 40.0f, 0.0f });
    ItemStack woodStack;
    woodStack.type = ItemFromBlock(BlockType::WoodBlock);
    woodStack.count = 8;
    inv.SwapSlot(0, woodStack);
    selectedHotbarSlot_ = 0; // the locally predicted player reads the UI slot

    PlayerCommand placeCmd = BuildLocalPlayerCommand();
    placeCmd.aimYaw = PI / 2.0f; // aim = (+1, 0, 0)
    placeCmd.aimPitch = 0.0f;
    placeCmd.placePressed = true;
    placeCmd.placeHeld = true;
    // A free-floating anchor in clear air ahead of the eye gives the command's
    // raycast a deterministic target; the block lands on the anchor's near face.
    const Vector3 placeForward = AimDirectionFromCommand(placeCmd);
    const Vector3 placeEye { 0.0f, 40.0f + 0.78f, 0.0f };
    const GridPos anchorCell = world_.WorldToGrid(Vector3 {
        placeEye.x + placeForward.x * 2.0f, placeEye.y, placeEye.z + placeForward.z * 2.0f });
    world_.PlaceBlock(anchorCell, Block { BlockType::StoneBlock, -1, true }, true);
    const GridPos expectedCell { anchorCell.x - 1, anchorCell.y, anchorCell.z };
    const std::size_t blocksBeforePlace = world_.GetBlocks().size();
    const int woodBeforePlace = inv.GetBlockCount(BlockType::WoodBlock);
    const int statsPlacedBefore = stats_.blocksPlaced;
    message_.clear();
    integratedServer_.SubmitCommand(kIntegratedServerClientId, placeCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const Block* placedBlock = world_.GetBlock(expectedCell);
    const bool placeOk = integratedServerBlocksPlaced_ == 1
        && world_.GetBlocks().size() == blocksBeforePlace + 1
        && placedBlock != nullptr && placedBlock->type == BlockType::WoodBlock
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBeforePlace - 1
        && stats_.blocksPlaced == statsPlacedBefore + 1
        && !message_.empty();

    // Held place input keeps riding the per-tick commands; the server-side rate
    // limiter must keep that to ONE placement per cooldown window (no double
    // apply now that HandlePlaceBlock's direct call is gated off).
    placeCmd.placePressed = false;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, placeCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool placeOnceOk = integratedServerBlocksPlaced_ == 1
        && world_.GetBlocks().size() == blocksBeforePlace + 1
        && inv.GetBlockCount(BlockType::WoodBlock) == woodBeforePlace - 1;

    // --- Part 4 (hero-ability slice): a cast crosses the SAME transport and the
    // authoritative action entry (ApplyPlayerActionCommand) — and fires exactly
    // once: the cooldown starts, and the result is presented directly.
    controlled->SetHeroId(HeroId::Likho); // cooldown-gated Active1, no target needed
    const float castCooldownBefore = controlled->GetHeroState().active1.cooldownRemaining;
    message_.clear();
    PlayerCommand castCmd = BuildLocalPlayerCommand();
    castCmd.useAbility1 = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, castCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const float castCooldownAfter = controlled->GetHeroState().active1.cooldownRemaining;
    const bool castOk = integratedServerHeroCastsApplied_ == 1
        && castCooldownBefore <= 0.0f
        && castCooldownAfter > 0.0f
        && !message_.empty();

    // Resubmitting the same cast while on cooldown must be a server-side no-op:
    // no second successful cast, and the running cooldown is not restarted.
    integratedServer_.SubmitCommand(kIntegratedServerClientId, castCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool castOnceOk = integratedServerHeroCastsApplied_ == 1
        && controlled->GetHeroState().active1.cooldownRemaining == castCooldownAfter;

    // --- Part 5 (utility slice): a heal crosses the SAME transport and the
    // command-driven utility path (UseUtilityInputs) — the medkit is consumed
    // exactly once and the result is presented directly.
    controlled->Heal(controlled->GetMaxHealth());
    controlled->Damage(60);
    ItemStack medkits;
    medkits.type = ItemFromUtility(UtilityType::Heal);
    medkits.count = 2;
    inv.SwapSlot(1, medkits);
    const int healthBeforeHeal = controlled->GetHealth();
    message_.clear();
    PlayerCommand healCmd = BuildLocalPlayerCommand();
    healCmd.useHeal = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, healCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const ItemStack medkitSlotAfter = inv.GetHotbarSlots()[1];
    const bool healOk = controlled->GetHealth() == healthBeforeHeal + 45
        && medkitSlotAfter.count == 1
        && !message_.empty();

    // --- Part 5b (unified right-click use): using the SELECTED medkit rides
    // the command's placePressed through ApplyNetworkPlayerActions — the same
    // code a real network client's right click hits. Exactly one charge per
    // press; a held button without a new press must not spend another.
    selectedHotbarSlot_ = 1; // the remaining medkit (UI slot mirror)
    controlled->Damage(60);
    const int healthBeforeRightClick = controlled->GetHealth();
    message_.clear();
    PlayerCommand useSelectedCmd = BuildLocalPlayerCommand();
    useSelectedCmd.selectedSlot = 1;
    useSelectedCmd.placePressed = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, useSelectedCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    useSelectedCmd.placePressed = false;
    useSelectedCmd.placeHeld = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, useSelectedCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool rightClickUtilityOk = controlled->GetHealth() == healthBeforeRightClick + 45
        && inv.GetHotbarSlots()[1].IsEmpty()
        && !message_.empty();

    Player* enemy = nullptr;
    for (Player& candidate : matchSimulation_.Players())
    {
        if (candidate.GetId() != controlled->GetId()
            && candidate.GetTeamId() != controlled->GetTeamId()
            && candidate.IsAlive())
        {
            enemy = &candidate;
            break;
        }
    }
    controlled->SetPosition(Vec3 { 0.0f, 60.0f, 0.0f });
    controlled->SetVelocity(Vec3 {});
    if (enemy != nullptr)
    {
        enemy->SetPosition(Vec3 { 0.0f, 60.0f, -1.6f });
        enemy->SetVelocity(Vec3 {});
        enemy->UpdateTimers(2.0f);
    }
    ItemStack sword;
    sword.type = ItemType::Sword;
    sword.count = 1;
    inv.SwapSlot(0, sword);
    controlled->SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;
    const int enemyHpBeforeDirect = enemy != nullptr ? enemy->GetHealth() : -1;
    currentInput_.attackPressed = true;
    currentInput_.attackHeld = true;
    UpdateAttackOrBreak(0.05f);
    currentInput_ = PlayerInput {};
    const bool directCombatSuppressed = enemy != nullptr
        && enemy->GetHealth() == enemyHpBeforeDirect;

    PlayerCommand meleeCmd = BuildLocalPlayerCommand();
    meleeCmd.controlledPlayerId = static_cast<std::uint32_t>(controlled->GetId());
    meleeCmd.selectedSlot = 0;
    meleeCmd.aimYaw = 0.0f;
    meleeCmd.aimPitch = 0.0f;
    meleeCmd.attackPressed = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, meleeCmd);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, 0.05f);
    }
    const bool meleeOk = enemy != nullptr
        && enemy->GetHealth() < enemyHpBeforeDirect;

    if (enemy != nullptr)
    {
        enemy->SetPosition(Vec3 { 100.0f, 60.0f, 100.0f });
    }
    ItemStack pickaxe;
    pickaxe.type = ItemType::Pickaxe;
    pickaxe.count = 1;
    inv.SwapSlot(0, pickaxe);
    controlled->SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;
    world_.Clear();
    PlayerCommand breakTemplate;
    breakTemplate.controlledPlayerId = static_cast<std::uint32_t>(controlled->GetId());
    breakTemplate.selectedSlot = 0;
    breakTemplate.aimYaw = 0.0f;
    breakTemplate.aimPitch = 0.0f;
    breakTemplate.attackHeld = true;
    const Vector3 breakForward = AimDirectionFromCommand(breakTemplate);
    const Vector3 breakEye { 0.0f, 60.0f + 0.78f, 0.0f };
    const GridPos breakCell = world_.WorldToGrid(Vector3 {
        breakEye.x + breakForward.x * 2.0f,
        breakEye.y + breakForward.y * 2.0f,
        breakEye.z + breakForward.z * 2.0f });
    world_.PlaceBlock(breakCell, Block { BlockType::WoodBlock, -1, true }, true);
    const std::size_t blocksBeforeBreak = world_.GetBlocks().size();
    bool breakOk = false;
    for (int i = 0; i < 160 && !breakOk; ++i)
    {
        PlayerCommand breakCmd = breakTemplate;
        breakCmd.tick = static_cast<std::uint32_t>(i + 1);
        integratedServer_.SubmitCommand(kIntegratedServerClientId, breakCmd);
        for (const PlayerCommand& received : integratedServer_.DrainCommands())
        {
            ++integratedServerCommandsDrained_;
            ApplyIntegratedServerCommand(received, 0.05f);
        }
        breakOk = world_.GetBlocks().size() < blocksBeforeBreak;
    }

    ItemStack bow;
    bow.type = ItemType::Bow;
    bow.count = 1;
    ItemStack arrows;
    arrows.type = ItemType::EnergyArrow;
    arrows.count = 3;
    inv.SwapSlot(0, bow);
    inv.SwapSlot(1, arrows);
    controlled->SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;
    controlled->ResetBowDraw();
    const std::size_t projectilesBeforeBow = projectiles_.size();
    PlayerCommand bowCmd;
    bowCmd.controlledPlayerId = static_cast<std::uint32_t>(controlled->GetId());
    bowCmd.selectedSlot = 0;
    bowCmd.aimYaw = 0.0f;
    bowCmd.aimPitch = 0.0f;
    bowCmd.attackHeld = true;
    for (int i = 0; i < 48; ++i)
    {
        bowCmd.tick = static_cast<std::uint32_t>(i + 1);
        integratedServer_.SubmitCommand(kIntegratedServerClientId, bowCmd);
        for (const PlayerCommand& received : integratedServer_.DrainCommands())
        {
            ++integratedServerCommandsDrained_;
            ApplyIntegratedServerCommand(received, fixedDt);
        }
    }
    PlayerCommand bowRelease = bowCmd;
    bowRelease.attackHeld = false;
    bowRelease.attackReleased = true;
    integratedServer_.SubmitCommand(kIntegratedServerClientId, bowRelease);
    for (const PlayerCommand& received : integratedServer_.DrainCommands())
    {
        ++integratedServerCommandsDrained_;
        ApplyIntegratedServerCommand(received, fixedDt);
    }
    const bool rangedOk = projectiles_.size() == projectilesBeforeBow + 1
        && projectiles_.back().kind == ProjectileKind::Arrow
        && controlled->GetBowDrawTimer() <= 0.0f;

    const bool ok = startedOk && channelOk && buyOk && dedupeOk && deniedOk
        && placeOk && placeOnceOk && castOk && castOnceOk && healOk
        && rightClickUtilityOk
        && directCombatSuppressed && meleeOk && breakOk && rangedOk;
    std::cout << "integrated-server-smoke: started=" << (startedOk ? "ok" : "FAIL")
              << " channel=" << (channelOk ? "ok" : "FAIL")
              << " (drained=" << integratedServerCommandsDrained_
              << " snapshots=" << integratedServer_.SnapshotsPublished()
              << " snapTick=" << snap.tick
              << " posError=" << posError << ")"
              << " buy=" << (buyOk ? "ok" : "FAIL")
              << " (iron " << ironBefore << "->" << inv.GetResource(ResourceType::Iron)
              << ", wool " << woolBefore << "->" << inv.GetBlockCount(BlockType::WoolBlock) << ")"
              << " dedupe=" << (dedupeOk ? "ok" : "FAIL")
              << " deniedRange=" << (deniedOk ? "ok" : "FAIL")
              << " place=" << (placeOk ? "ok" : "FAIL")
              << " (blocks " << blocksBeforePlace << "->" << world_.GetBlocks().size()
              << ", wood " << woodBeforePlace << "->" << inv.GetBlockCount(BlockType::WoodBlock)
              << ", placedViaServer=" << integratedServerBlocksPlaced_ << ")"
              << " placeOnce=" << (placeOnceOk ? "ok" : "FAIL")
              << " heroCast=" << (castOk ? "ok" : "FAIL")
              << " (cooldown " << castCooldownBefore << "->" << castCooldownAfter
              << ", castsViaServer=" << integratedServerHeroCastsApplied_ << ")"
              << " castOnce=" << (castOnceOk ? "ok" : "FAIL")
              << " heal=" << (healOk ? "ok" : "FAIL")
              << " (hp " << healthBeforeHeal << "->" << controlled->GetHealth()
              << ", medkits 2->" << medkitSlotAfter.count << ")"
              << " rightClickUtility=" << (rightClickUtilityOk ? "ok" : "FAIL")
              << " directCombat=" << (directCombatSuppressed ? "ok" : "FAIL")
              << " melee=" << (meleeOk ? "ok" : "FAIL")
              << " break=" << (breakOk ? "ok" : "FAIL")
              << " ranged=" << (rangedOk ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "INTEGRATED_SERVER_SMOKE_OK" : "INTEGRATED_SERVER_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 4;
}

int Game::RunCreativeSmoke()
{
    // Creative-mode self-test (windowed). Covers the full map-authoring loop:
    // free building (world grows, inventory does NOT), the specials layer
    // (capture + place/replace a core), the map document save/load round-trip,
    // test-play (real match rebuilt from the document) and the return to the
    // editor with the edited world intact.
    StartCreativeSession();
    Player* player = GetLocalPlayer();
    if (player == nullptr)
    {
        std::cout << "creative-smoke: no local player\n"
                     "CREATIVE_SMOKE_FAIL" << std::endl;
        return 4;
    }
    suppressLocalFeedback_ = false;

    const bool creativeOn = creativeMode_;
    const bool noWinnerInitially = !matchSimulation_.HasWinner();
    // Palette stocked slot 0 with WoodBlock (the free-placement subject).
    const bool paletteOk = player->GetInventory().GetBlockCount(BlockType::WoodBlock) > 0;
    const BlockType decorativeBlocks[] {
        BlockType::SmoothStoneBlock,
        BlockType::DarkBrickBlock,
        BlockType::LightBrickBlock,
        BlockType::MetalBlock,
        BlockType::GlowBlock,
        BlockType::PlankBlock,
        BlockType::DecorativeTileBlock,
        BlockType::TrimBlock
    };
    bool decorativePaletteOk = true;
    for (BlockType type : decorativeBlocks)
    {
        decorativePaletteOk = decorativePaletteOk
            && player->GetInventory().GetBlockCount(type) > 0;
    }
    const BlockType castlePaletteBlocks[] {
        BlockType::CobblestoneBlock, BlockType::AndesiteBlock, BlockType::PolishedAndesiteBlock,
        BlockType::StoneBrickBlock, BlockType::ChiseledStoneBrickBlock, BlockType::StoneSlabBlock,
        BlockType::StoneBrickSlabBlock, BlockType::StoneBrickStairsBlock, BlockType::BirchPlankBlock,
        BlockType::BirchSlabBlock, BlockType::BirchStairsBlock, BlockType::ColoredGlassBlock,
        BlockType::ColoredClayBlock, BlockType::LapisBlock, BlockType::DiamondBlock,
        BlockType::EmeraldBlock, BlockType::GoldBlock, BlockType::IronBarsBlock,
        BlockType::LadderBlock, BlockType::TorchBlock, BlockType::BarrierBlock
    };
    const bool castlePaletteOk = std::all_of(std::begin(castlePaletteBlocks), std::end(castlePaletteBlocks), [](BlockType type)
    {
        return IsBuildableBlock(type) && ItemFromBlock(type) != ItemType::None;
    });
    creativePaletteSearch_ = "glow";
    creativePaletteSearchActive_ = true;
    creativePalettePage_ = 2;
    creativePaletteCursor_ = 5;
    ResetCreativePaletteUi();
    const bool creativePaletteUiOk = creativePaletteSearch_.empty()
        && !creativePaletteSearchActive_
        && creativePalettePage_ == 0
        && creativePaletteCursor_ == 0
        && creativePaletteTab_ == 0;

    // Clear air; place a wood block against a free-floating anchor.
    player->SetPosition(Vec3 { 0.0f, 40.0f, 0.0f });
    selectedHotbarSlot_ = 0;
    const GridPos anchor { 2, 40, 0 };
    world_.PlaceBlock(anchor, Block { BlockType::StoneBlock, -1, true }, true);
    const GridPos target { 1, 40, 0 }; // adjacent to the anchor, in reach, in air
    const std::size_t blocksBefore = world_.GetBlocks().size();
    const int woodBefore = player->GetInventory().GetBlockCount(BlockType::WoodBlock);
    const BlockActionResult result = ApplyPlaceBlockForPlayer(*player, target);
    const Block* placed = world_.GetBlock(target);
    const bool freePlaceOk = result.success
        && placed != nullptr
        && placed->type == BlockType::WoodBlock
        && world_.GetBlocks().size() == blocksBefore + 1
        && player->GetInventory().GetBlockCount(BlockType::WoodBlock) == woodBefore; // NOT spent
    const bool undoPlace = UndoCreativeEdit();
    const bool undoRemovedBlock = world_.GetBlock(target) == nullptr;
    const bool redoPlace = RedoCreativeEdit();
    placed = world_.GetBlock(target);
    const bool undoRedoOk = undoPlace && undoRemovedBlock && redoPlace
        && placed != nullptr && placed->type == BlockType::WoodBlock;

    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    const Vector3 flightStart = player->GetPosition();
    currentInput_ = PlayerInput {};
    currentInput_.jump = true;
    currentInput_.jumpHeld = true;
    PlayerCommand flightCommand = BuildLocalPlayerCommand();
    const bool firstFlightTapConsumed = UpdateCreativeFlightToggle(*player, flightCommand, fixedDt);
    currentInput_ = PlayerInput {};
    currentInput_.jump = true;
    currentInput_.jumpHeld = true;
    flightCommand = BuildLocalPlayerCommand();
    const bool secondFlightTapConsumed = UpdateCreativeFlightToggle(*player, flightCommand, fixedDt);
    currentInput_ = PlayerInput {};
    currentInput_.move.z = 1.0f;
    currentInput_.jumpHeld = true;
    currentInput_.sprint = true;
    flightCommand = BuildLocalPlayerCommand();
    UpdateCreativeFlight(*player, flightCommand, 0.25f);
    const Vector3 flightEnd = player->GetPosition();
    const bool flightOk = !firstFlightTapConsumed
        && secondFlightTapConsumed
        && creativeFlightActive_
        && flightEnd.y > flightStart.y + 0.5f;
    SetCreativeFlightActive(*player, false);
    currentInput_ = PlayerInput {};
    player->Teleport(Vector3 { 0.0f, 40.0f, 0.0f });

    ItemStack glowStack { ItemFromBlock(BlockType::GlowBlock), 64 };
    player->GetInventory().SetSlot(1, glowStack);
    selectedHotbarSlot_ = 1;
    player->SetSelectedSlot(1);
    const GridPos glowTarget { 2, 41, 0 }; // above the anchor, in reach, in air
    const std::size_t blocksBeforeDecor = world_.GetBlocks().size();
    const int glowBefore = player->GetInventory().GetBlockCount(BlockType::GlowBlock);
    const BlockActionResult decorResult = ApplyPlaceBlockForPlayer(*player, glowTarget);
    const Block* glowPlaced = world_.GetBlock(glowTarget);
    const bool decorativePlaceOk = decorResult.success
        && glowPlaced != nullptr
        && glowPlaced->type == BlockType::GlowBlock
        && world_.GetBlocks().size() == blocksBeforeDecor + 1
        && player->GetInventory().GetBlockCount(BlockType::GlowBlock) == glowBefore;
    const int glowAfterDecor = player->GetInventory().GetBlockCount(BlockType::GlowBlock);

    const GridPos farAnchor { 13, 40, 0 };
    const GridPos farTarget { 12, 40, 0 };
    world_.PlaceBlock(farAnchor, Block { BlockType::StoneBlock, -1, true }, true);
    selectedHotbarSlot_ = 0;
    player->SetSelectedSlot(0);
    const BlockActionResult farPlaceResult = ApplyPlaceBlockForPlayer(*player, farTarget);
    const Block* farPlaced = world_.GetBlock(farTarget);
    const bool extendedReachPlaceOk = farPlaceResult.success
        && farPlaced != nullptr
        && farPlaced->type == BlockType::WoodBlock;

    const GridPos instantBreakTarget { 3, 40, 0 };
    world_.PlaceBlock(instantBreakTarget, Block { BlockType::DarkBrickBlock, -1, true }, true);
    const std::size_t dropsBeforeBreak = matchSimulation_.DroppedItems().size();
    const float creativeBreakSeconds = ComputeBreakRequiredSeconds(
        *player,
        RaycastHit { instantBreakTarget, instantBreakTarget, GridPos {}, Block { BlockType::DarkBrickBlock, -1, true }, 1.0f },
        false);
    const BlockActionResult breakResult = ApplyCompletedBreakProgress(
        *player,
        BreakProgress { instantBreakTarget, BlockType::DarkBrickBlock, true, false, 1.0f, "Dark brick" });
    const bool instantBreakOk = creativeBreakSeconds <= 0.01f
        && breakResult.success
        && world_.GetBlock(instantBreakTarget) == nullptr
        && matchSimulation_.DroppedItems().size() == dropsBeforeBreak;

    // The editor must also remove map-authored PROTECTED blocks (imported
    // maps ship breakable=0): a loaded castle previously became read-only.
    const GridPos protectedBreakTarget { 4, 40, 0 };
    world_.PlaceBlock(protectedBreakTarget, Block { BlockType::StoneBrickBlock, -1, false }, true);
    const BlockActionResult protectedBreakResult = ApplyCompletedBreakProgress(
        *player,
        BreakProgress { protectedBreakTarget, BlockType::StoneBrickBlock, true, false, 1.0f, "Stone brick" });
    const bool protectedBreakOk = protectedBreakResult.success
        && world_.GetBlock(protectedBreakTarget) == nullptr;

    creativeSelectionA_ = GridPos { 20, 40, 0 };
    creativeSelectionB_ = GridPos { 21, 40, 1 };
    selectedHotbarSlot_ = 0;
    player->SetSelectedSlot(0);
    const bool areaFillApplied = ApplyCreativeAreaFill(*player, false);
    int areaFilledCount = 0;
    for (int x = 20; x <= 21; ++x)
    {
        for (int z = 0; z <= 1; ++z)
        {
            const Block* block = world_.GetBlock(GridPos { x, 40, z });
            if (block != nullptr && block->type == BlockType::WoodBlock)
            {
                ++areaFilledCount;
            }
        }
    }
    const bool areaUndo = UndoCreativeEdit();
    const bool areaCleared = world_.GetBlock(GridPos { 20, 40, 0 }) == nullptr
        && world_.GetBlock(GridPos { 21, 40, 1 }) == nullptr;
    const bool areaRedo = RedoCreativeEdit();
    const bool areaRestored = world_.GetBlock(GridPos { 20, 40, 0 }) != nullptr
        && world_.GetBlock(GridPos { 21, 40, 1 }) != nullptr;
    const bool areaToolsOk = areaFillApplied && areaFilledCount == 4 && areaUndo && areaCleared && areaRedo && areaRestored;

    // The win check stays disabled even after a sim step (no accidental victory
    // from the solo, bot-less roster).
    UpdateMatchSimulation(fixedDt);
    const bool stillNoWinner = !matchSimulation_.HasWinner();

    // --- Specials layer: entering the editor captured the arena's entities.
    const std::size_t specialsCaptured = creativeSpecials_.size();
    const bool captureOk = specialsCaptured > 0
        && std::any_of(creativeSpecials_.begin(), creativeSpecials_.end(), [](const CreativeSpecial& s)
           {
               return s.kind == CreativeSpecialKind::Core;
           });

    // Placing a core for a team REPLACES that team's existing core (one per
    // team): specials count stays, the core entity moves to the new cell.
    const GridPos corePos { 4, 40, 2 };
    const std::size_t coresBefore = matchSimulation_.Cores().size();
    const bool specialPlaced = PlaceCreativeSpecialAt(corePos, CreativeSpecialKind::Core, 1);
    const Block* coreBlock = world_.GetBlock(corePos);
    const bool specialPlaceOk = specialPlaced
        && coreBlock != nullptr && coreBlock->type == BlockType::EnergyCoreBlock
        && coreBlock->teamId == 1
        && creativeSpecials_.size() == specialsCaptured
        && matchSimulation_.Cores().size() == coresBefore
        && std::any_of(matchSimulation_.Cores().begin(), matchSimulation_.Cores().end(),
           [&corePos](const EnergyCore& core)
           {
               return core.GetBlockPosition() == corePos && core.GetTeamId() == 1;
           });
    const bool validationOk = BuildCreativeValidationIssues().empty();

    // --- Document save/load round-trip (temp file, removed afterwards).
    // Include a nonzero variant so Minecraft stair/slab/dye state is covered
    // by the token-based format, not only by the old numeric block IDs.
    const GridPos variantPos { 6, 40, 2 };
    world_.PlaceBlock(variantPos, Block { BlockType::StoneBrickStairsBlock, -1, true, 5 }, true);
    const CreativeMapDocument savedDoc = BuildCreativeMapDocument();
    const char* smokeMapPath = "creative_smoke_map.dbmap";
    CreativeMapDocument loadedDoc;
    std::string mapIoError;
    const bool saveLoadOk = SaveCreativeMapDocument(savedDoc, smokeMapPath, &mapIoError)
        && LoadCreativeMapDocument(smokeMapPath, loadedDoc, &mapIoError)
        && loadedDoc.blocks.size() == savedDoc.blocks.size()
        && loadedDoc.specials.size() == savedDoc.specials.size()
        && loadedDoc.TeamHasCore(1)
        && std::any_of(loadedDoc.blocks.begin(), loadedDoc.blocks.end(), [&variantPos](const CreativeMapBlock& block)
           {
               return block.pos == variantPos && block.type == BlockType::StoneBrickStairsBlock && block.variant == 5;
           });
    std::remove(smokeMapPath);
    CreativeMapDocument legacyDoc;
    const bool legacyMapLoadOk = LoadCreativeMapDocument("maps/oskolki_imperii.dbmap", legacyDoc, &mapIoError)
        && !legacyDoc.blocks.empty()
        && std::any_of(legacyDoc.blocks.begin(), legacyDoc.blocks.end(), [](const CreativeMapBlock& block)
        {
            return block.type == BlockType::StoneBlock || block.type == BlockType::ObsidianBlock;
        });
    std::string castleMapError;
    const bool castleMapLoadOk = LoadAutomatchMapDocument("maps/castle_bedwars.dbmap", &castleMapError)
        && automatchMapDoc_.blocks.size() > 190000
        && automatchMapDoc_.CoreTeamCount() == 4
        && std::any_of(automatchMapDoc_.blocks.begin(), automatchMapDoc_.blocks.end(), [](const CreativeMapBlock& block)
        {
            return block.type == BlockType::StoneBrickBlock
                || block.type == BlockType::ColoredGlassBlock
                || block.type == BlockType::TorchBlock;
        });
    // The smoke only validates loading; do not leave a custom map pinned for
    // any subsequent ordinary match in this process.
    automatchMapLoaded_ = false;
    automatchMapDoc_ = CreativeMapDocument {};

    // --- Test-play: the document rebuilds a REAL match (bots on core teams,
    // normal rules) with the edited world, then the editor session restores.
    StartCreativeMapTest();
    const std::size_t testPlayers = matchSimulation_.Players().size();
    const Block* testWood = world_.GetBlock(target);
    const Block* testGlow = world_.GetBlock(glowTarget);
    const Block* testCore = world_.GetBlock(corePos);
    const bool testOk = !creativeMode_ && creativeTestActive_
        && testPlayers >= 2
        && matchSimulation_.Cores().size() == coresBefore
        && testWood != nullptr && testWood->type == BlockType::WoodBlock
        && testGlow != nullptr && testGlow->type == BlockType::GlowBlock
        && testCore != nullptr && testCore->type == BlockType::EnergyCoreBlock;

    ReturnToCreativeEditor();
    player = GetLocalPlayer();
    const Block* editorWood = world_.GetBlock(target);
    const Block* editorGlow = world_.GetBlock(glowTarget);
    const bool returnOk = creativeMode_ && !creativeTestActive_
        && player != nullptr
        && matchSimulation_.Players().size() == 1
        && creativeSpecials_.size() == specialsCaptured
        && editorWood != nullptr && editorWood->type == BlockType::WoodBlock
        && editorGlow != nullptr && editorGlow->type == BlockType::GlowBlock;

    const bool ok = creativeOn && noWinnerInitially && paletteOk && decorativePaletteOk && castlePaletteOk && creativePaletteUiOk
        && freePlaceOk && undoRedoOk && flightOk && decorativePlaceOk && extendedReachPlaceOk
        && instantBreakOk && protectedBreakOk && areaToolsOk && stillNoWinner
        && captureOk && specialPlaceOk && validationOk && saveLoadOk && legacyMapLoadOk && castleMapLoadOk && testOk && returnOk;
    std::cout << "creative-smoke: creativeFlag=" << (creativeOn ? "ok" : "FAIL")
              << " palette=" << (paletteOk ? "ok" : "FAIL")
              << " decorPalette=" << (decorativePaletteOk ? "ok" : "FAIL")
              << " castlePalette=" << (castlePaletteOk ? "ok" : "FAIL")
              << " paletteUi=" << (creativePaletteUiOk ? "ok" : "FAIL")
              << " freePlace=" << (freePlaceOk ? "ok" : "FAIL")
              << " (blocks " << blocksBefore << "->+1"
              << ", wood " << woodBefore << "->" << woodBefore << ")"
              << " undoRedo=" << (undoRedoOk ? "ok" : "FAIL")
              << " flight=" << (flightOk ? "ok" : "FAIL")
              << " (y " << flightStart.y << "->" << flightEnd.y << ")"
              << " decorPlace=" << (decorativePlaceOk ? "ok" : "FAIL")
              << " (glow " << glowBefore << "->" << glowAfterDecor << ")"
              << " reach=" << (extendedReachPlaceOk ? "ok" : "FAIL")
              << " instantBreak=" << (instantBreakOk ? "ok" : "FAIL")
              << " protectedBreak=" << (protectedBreakOk ? "ok" : "FAIL")
              << " (seconds=" << creativeBreakSeconds << ", drops " << dropsBeforeBreak
              << "->" << matchSimulation_.DroppedItems().size() << ")"
              << " areaTools=" << (areaToolsOk ? "ok" : "FAIL")
              << " (filled=" << areaFilledCount << ")"
              << " noWinner=" << ((noWinnerInitially && stillNoWinner) ? "ok" : "FAIL")
              << " specialsCapture=" << (captureOk ? "ok" : "FAIL")
              << " (" << specialsCaptured << " entries)"
              << " specialPlace=" << (specialPlaceOk ? "ok" : "FAIL")
              << " validation=" << (validationOk ? "ok" : "FAIL")
              << " legacyMap=" << (legacyMapLoadOk ? "ok" : "FAIL")
              << " castleMap=" << (castleMapLoadOk ? "ok" : "FAIL")
              << " saveLoad=" << (saveLoadOk ? "ok" : "FAIL")
              << " (" << savedDoc.blocks.size() << " blocks, " << savedDoc.specials.size() << " specials"
              << (mapIoError.empty() ? "" : (", " + mapIoError)) << ")"
              << " mapTest=" << (testOk ? "ok" : "FAIL")
              << " (players=" << testPlayers << ")"
              << " editorReturn=" << (returnOk ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "CREATIVE_SMOKE_OK" : "CREATIVE_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 4;
}

int Game::RunLoopbackTwoClientSmoke()
{
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }

    // In-process "server + two clients" — no sockets. The server is this Game's
    // authoritative match; the transport carries commands in and per-client
    // snapshots out.
    LoopbackTransport transport;
    transport.Configure(serverConfig_);
    transport.Start();

    // Real headless match so the snapshots carry genuine state.
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();
    screen_ = GameScreen::Playing;

    const auto findPlayerById = [this](std::uint32_t id) -> Player*
    {
        for (Player& player : players_)
        {
            if (static_cast<std::uint32_t>(player.GetId()) == id)
            {
                return &player;
            }
        }
        return nullptr;
    };
    const auto toVec3 = [](Vector3 v) { return Vec3 { v.x, v.y, v.z }; };

    // Two DIFFERENT players: client A drives the local player, client B drives a
    // player on another team.
    Player* localPlayer = GetLocalPlayer();
    const int playerAId = localPlayer != nullptr ? localPlayer->GetId() : -1;
    const int teamA = localPlayer != nullptr ? localPlayer->GetTeamId() : -1;
    int playerBId = -1;
    for (const Player& player : matchSimulation_.Players())
    {
        if (player.GetId() != playerAId && player.GetTeamId() != teamA && player.IsAlive())
        {
            playerBId = player.GetId();
            break;
        }
    }

    constexpr int kClientA = 101;
    constexpr int kClientB = 202;

    // Mapping: bind each client to its player. Binding a second client to an
    // already-owned player, or reusing a client id, must be rejected — that is
    // the connection-time guarantee that two clients never share one player.
    const bool connectA = transport.Connect(kClientA, playerAId);
    const bool connectB = transport.Connect(kClientB, playerBId);
    const bool duplicatePlayerRejected = !transport.Connect(303, playerAId);
    const bool duplicateClientRejected = !transport.Connect(kClientA, playerBId);

    Player* pa = findPlayerById(static_cast<std::uint32_t>(playerAId));
    Player* pb = findPlayerById(static_cast<std::uint32_t>(playerBId));
    const Vec3 startA = pa != nullptr ? toVec3(pa->GetPosition()) : Vec3 {};
    const Vec3 startB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    const float yawA = pa != nullptr ? pa->GetYaw() : 0.0f;
    const float yawB = pb != nullptr ? pb->GetYaw() : 0.0f;

    constexpr int kTicks = 90;
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    int commandsAppliedA = 0;
    int commandsAppliedB = 0;

    for (int i = 0; i < kTicks; ++i)
    {
        // Client A: move forward along its own facing.
        PlayerCommand cmdA;
        cmdA.controlledPlayerId = static_cast<std::uint32_t>(playerAId);
        cmdA.tick = matchSimulation_.CurrentTick();
        cmdA.aimYaw = yawA;
        cmdA.moveForward = 1.0f;
        transport.SubmitCommand(kClientA, cmdA);

        // Client B: a distinct command (its own facing) for a distinct player.
        PlayerCommand cmdB;
        cmdB.controlledPlayerId = static_cast<std::uint32_t>(playerBId);
        cmdB.tick = matchSimulation_.CurrentTick();
        cmdB.aimYaw = yawB;
        cmdB.moveForward = 1.0f;
        transport.SubmitCommand(kClientB, cmdB);

        // Server: drain both clients' commands into the sim intake, then apply
        // each to the (mapping-stamped) player it targets.
        for (const PlayerCommand& received : transport.DrainCommands())
        {
            matchSimulation_.SubmitCommand(received);
        }
        for (const PlayerCommand& received : matchSimulation_.DrainCommands())
        {
            Player* target = findPlayerById(received.controlledPlayerId);
            if (target != nullptr && target->IsAlive())
            {
                ApplyPlayerCommand(*target, received, fixedDt);
                if (received.controlledPlayerId == static_cast<std::uint32_t>(playerAId))
                {
                    ++commandsAppliedA;
                }
                else if (received.controlledPlayerId == static_cast<std::uint32_t>(playerBId))
                {
                    ++commandsAppliedB;
                }
            }
        }

        // Server advances the single authoritative tick/clock for everyone.
        matchSimulation_.AdvanceTick();
        matchSimulation_.AdvanceClock(fixedDt);

        // Server publishes a per-client, visibility-filtered snapshot (the
        // visibility hook is already wired: each client sees its OWN inventory).
        const MatchSnapshot snapshotA = BuildNetworkSnapshotForClient(playerAId);
        const MatchSnapshot snapshotB = BuildNetworkSnapshotForClient(playerBId);
        transport.PublishSnapshot(kClientA, snapshotA);
        transport.PublishSnapshot(kClientB, snapshotB);
        PushRemoteSnapshot(snapshotA);
    }

    // --- Gather results (captured BEFORE the spoof tick below, so they match
    // the snapshots published at the end of the loop) ------------------------
    const Vec3 endA = pa != nullptr ? toVec3(pa->GetPosition()) : Vec3 {};
    const Vec3 endB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    const float movedA = (endA - startA).Length();
    const float movedB = (endB - startB).Length();

    const MatchSnapshot& snapA = transport.LatestSnapshot(kClientA);
    const MatchSnapshot& snapB = transport.LatestSnapshot(kClientB);

    const auto findEntry = [](const MatchSnapshot& snap, int id) -> const PlayerSnapshot*
    {
        for (const PlayerSnapshot& entry : snap.players)
        {
            if (entry.playerId == id)
            {
                return &entry;
            }
        }
        return nullptr;
    };
    const auto posError = [](const PlayerSnapshot* entry, const Vec3& live) -> float
    {
        return entry != nullptr ? (entry->position - live).Length() : 1e9f;
    };

    // Both clients see BOTH players at their authoritative positions (public).
    const PlayerSnapshot* aSeesA = findEntry(snapA, playerAId);
    const PlayerSnapshot* aSeesB = findEntry(snapA, playerBId);
    const PlayerSnapshot* bSeesA = findEntry(snapB, playerAId);
    const PlayerSnapshot* bSeesB = findEntry(snapB, playerBId);
    const bool authoritativePositions =
        posError(aSeesA, endA) < 0.001f && posError(aSeesB, endB) < 0.001f
        && posError(bSeesA, endA) < 0.001f && posError(bSeesB, endB) < 0.001f;

    // tick/snapshot consistency: one authoritative tick, identical in both views.
    const std::uint32_t serverTick = matchSimulation_.CurrentTick();
    const bool tickConsistent = snapA.tick == serverTick && snapB.tick == serverTick
        && snapA.tick == snapB.tick;

    // Visibility hook is live: each client sees its OWN inventory, not the other's.
    const bool visibilityApplied =
        aSeesA != nullptr && aSeesA->inventory.present
        && aSeesB != nullptr && !aSeesB->inventory.present
        && bSeesB != nullptr && bSeesB->inventory.present
        && bSeesA != nullptr && !bSeesA->inventory.present;

    Vec3 interpolatedB {};
    const bool remoteInterpolationAvailable =
        TryGetInterpolatedRemotePlayerPosition(playerBId, fixedDt * 2.0f, interpolatedB);
    const float interpolatedMovedB = (interpolatedB - startB).Length();
    const float interpolatedEndErrorB = (interpolatedB - endB).Length();
    const bool remoteInterpolationOk = remoteInterpolationAvailable
        && remoteSnapshotBuffer_.size() >= 3
        && interpolatedMovedB > 0.1f
        && interpolatedEndErrorB < std::max(1.0f, movedB);

    // Spoof: client A claims to control player B. The transport re-stamps the
    // command to player A, so player B must NOT move from A's command — the
    // runtime guarantee that two clients can't drive the same player. (Run last,
    // after the snapshot comparison above, since it moves player A.)
    const Vec3 spoofStartB = endB;
    PlayerCommand spoof;
    spoof.controlledPlayerId = static_cast<std::uint32_t>(playerBId); // the lie
    spoof.tick = matchSimulation_.CurrentTick();
    spoof.aimYaw = yawB;
    spoof.moveForward = 1.0f;
    transport.SubmitCommand(kClientA, spoof);
    const std::vector<PlayerCommand> spoofDrained = transport.DrainCommands();
    const bool spoofStamped = spoofDrained.size() == 1
        && spoofDrained.front().controlledPlayerId == static_cast<std::uint32_t>(playerAId);
    for (const PlayerCommand& received : spoofDrained)
    {
        Player* target = findPlayerById(received.controlledPlayerId);
        if (target != nullptr && target->IsAlive())
        {
            ApplyPlayerCommand(*target, received, fixedDt);
        }
    }
    const Vec3 spoofEndB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    const bool spoofPlayerBUnchanged = (spoofEndB - spoofStartB).Length() < 1e-4f;

    networkAssignedPlayerId_ = playerAId;
    const Vec3 renderStepStartB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    UpdateRemoteInterpolation(fixedDt * 0.5f);
    const Vec3 renderStepMidB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    UpdateRemoteInterpolation(fixedDt * 0.5f);
    const Vec3 renderStepEndB = pb != nullptr ? toVec3(pb->GetPosition()) : Vec3 {};
    const bool remoteInterpolationAdvancesBetweenSnapshots =
        (renderStepMidB - renderStepStartB).Length() > 0.0001f
        && (renderStepEndB - renderStepMidB).Length() > 0.0001f;

    const bool differentPlayers = playerAId != playerBId && playerAId >= 0 && playerBId >= 0;
    const bool mappingOk = connectA && connectB && duplicatePlayerRejected && duplicateClientRejected
        && transport.PlayerForClient(kClientA) == playerAId
        && transport.PlayerForClient(kClientB) == playerBId
        && transport.ClientForPlayer(playerAId) == kClientA
        && transport.ClientForPlayer(playerBId) == kClientB
        && transport.ClientCount() == 2;
    const bool bothMoved = movedA > 0.5f && movedB > 0.5f;
    const bool commandsApplied = commandsAppliedA == kTicks && commandsAppliedB == kTicks;
    const bool noSharedControl = spoofStamped && spoofPlayerBUnchanged
        && transport.ClientForPlayer(playerBId) == kClientB;

    std::cout << "loopback-smoke: clients=" << transport.ClientCount()
              << " A(client=" << kClientA << ",player=" << playerAId << ",team=" << teamA << ')'
              << " B(client=" << kClientB << ",player=" << playerBId << ')'
              << " mapping=" << (mappingOk ? "ok" : "FAIL")
              << " (dupPlayer=" << (duplicatePlayerRejected ? "rejected" : "ALLOWED")
              << ",dupClient=" << (duplicateClientRejected ? "rejected" : "ALLOWED") << ")\n";
    std::cout << "loopback-smoke: serverTick=" << serverTick
              << " snapTickA=" << snapA.tick << " snapTickB=" << snapB.tick
              << " snapshotsPublished=" << transport.SnapshotsPublished()
              << " commandsApplied A/B=" << commandsAppliedA << '/' << commandsAppliedB
              << " movedA=" << movedA << " movedB=" << movedB << '\n';
    std::cout << "loopback-smoke: posErr A.A=" << posError(aSeesA, endA)
              << " A.B=" << posError(aSeesB, endB)
              << " B.A=" << posError(bSeesA, endA)
              << " B.B=" << posError(bSeesB, endB)
              << " visibility(ownInventory)=" << (visibilityApplied ? "ok" : "FAIL")
              << " spoof(stamped=" << (spoofStamped ? "yes" : "no")
              << ",B_unchanged=" << (spoofPlayerBUnchanged ? "yes" : "no") << ")\n";
    std::cout << "loopback-smoke: interpolation buffer=" << remoteSnapshotBuffer_.size()
              << " B.pos=(" << interpolatedB.x << ',' << interpolatedB.y << ',' << interpolatedB.z << ')'
              << " moved=" << interpolatedMovedB
              << " endError=" << interpolatedEndErrorB
              << " frameAdvance=" << (remoteInterpolationAdvancesBetweenSnapshots ? "yes" : "no")
              << " status=" << (remoteInterpolationOk ? "ok" : "FAIL") << '\n';

    const bool ok = differentPlayers && mappingOk && bothMoved && commandsApplied
        && authoritativePositions && tickConsistent && visibilityApplied && noSharedControl
        && remoteInterpolationOk && remoteInterpolationAdvancesBetweenSnapshots
        && !snapA.players.empty() && !snapB.players.empty();

    if (!ok)
    {
        std::cout << "loopback-smoke: checks differentPlayers=" << (differentPlayers ? "ok" : "FAIL")
                  << " mapping=" << (mappingOk ? "ok" : "FAIL")
                  << " bothMoved=" << (bothMoved ? "ok" : "FAIL")
                  << " commandsApplied=" << (commandsApplied ? "ok" : "FAIL")
                  << " authPositions=" << (authoritativePositions ? "ok" : "FAIL")
                  << " tickConsistent=" << (tickConsistent ? "ok" : "FAIL")
                  << " visibility=" << (visibilityApplied ? "ok" : "FAIL")
                  << " interpolation=" << (remoteInterpolationOk ? "ok" : "FAIL")
                  << " frameAdvance=" << (remoteInterpolationAdvancesBetweenSnapshots ? "ok" : "FAIL")
                  << " noSharedControl=" << (noSharedControl ? "ok" : "FAIL") << '\n';
    }

    std::cout << (ok ? "LOOPBACK_SMOKE_OK" : "LOOPBACK_SMOKE_FAIL") << std::endl;
    transport.Stop();
    return ok ? 0 : 7;
}

int Game::RunMultiplayerLoopbackSmoke()
{
    using Clock = std::chrono::steady_clock;

    if (!NetworkTransportAvailable())
    {
        std::cout << "mp-smoke: network transport disabled at build (DAIBED_ENABLE_NETWORK=OFF) — skipped\n";
        std::cout << "MP_LOOPBACK_SMOKE_SKIPPED" << std::endl;
        return 0;
    }

    ServerTransport server;
    ServerConfig cfg;
    cfg.listenAddress = "127.0.0.1";
    cfg.port = 0; // ephemeral
    cfg.password = "secret";
    if (!NetworkServerSetup(server, cfg))
    {
        std::cout << "mp-smoke: server setup failed: " << server.LastError() << '\n';
        std::cout << "MP_LOOPBACK_SMOKE_FAIL" << std::endl;
        return 9;
    }
    const std::uint16_t port = server.BoundPort();
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    std::cout << "mp-smoke: server listening on 127.0.0.1:" << port
              << " (headless, no window) password=set\n";

    ClientTransport clientA;
    ClientTransport clientB;
    ClientTransport clientBad;
    clientA.Open("127.0.0.1", port, "secret", 3.0f);
    clientB.Open("127.0.0.1", port, "secret", 3.0f);
    clientBad.Open("127.0.0.1", port, "wrong", 3.0f); // wrong password -> denied

    const auto posInSnapshot = [](const MatchSnapshot& snap, int playerId, Vec3& out) -> bool
    {
        for (const PlayerSnapshot& entry : snap.players)
        {
            if (entry.playerId == playerId)
            {
                out = entry.position;
                return true;
            }
        }
        return false;
    };
    const auto sendMove = [this](ClientTransport& client)
    {
        if (!client.InMatch())
        {
            return;
        }
        const int pid = client.AssignedPlayerId();
        const Player* p = matchSimulation_.GetPlayer(pid);
        PlayerCommand cmd;
        cmd.controlledPlayerId = static_cast<std::uint32_t>(pid);
        cmd.tick = matchSimulation_.CurrentTick();
        cmd.aimYaw = p != nullptr ? p->GetYaw() : 0.0f; // move forward along own facing
        cmd.moveForward = 1.0f;
        client.SendCommand(cmd);
    };

    // Phase 1: connect both, exchange commands/snapshots, watch A move (as B sees it).
    Vec3 bSeesAFirst {};
    bool haveFirst = false;
    float aMovementSeenByB = 0.0f;
    const Clock::time_point deadline1 = Clock::now() + std::chrono::seconds(3);
    int ticksAfterFirst = 0;
    bool sentLobbyA = false;
    bool sentLobbyB = false;
    bool duplicateBlocked = false;
    bool duplicateFixed = false;
    while (Clock::now() < deadline1)
    {
        NetworkServerTick(server, fixedDt);
        clientA.Poll();
        clientB.Poll();
        clientBad.Poll();

        if (clientA.IsConnected() && !sentLobbyA)
        {
            LobbyUpdate update;
            update.playerName = "Alice";
            update.selectedTeam = 0;
            update.selectedHero = 0;
            update.ready = true;
            update.startRequested = true;
            clientA.SendLobbyUpdate(update);
            sentLobbyA = true;
        }
        if (clientB.IsConnected() && !sentLobbyB)
        {
            LobbyUpdate update;
            update.playerName = "Bob";
            update.selectedTeam = 0;
            update.selectedHero = 0; // duplicate with Alice: start must block first.
            update.ready = true;
            update.startRequested = true;
            clientB.SendLobbyUpdate(update);
            sentLobbyB = true;
        }
        if (!duplicateFixed && clientA.HasLobbySnapshot())
        {
            const LobbySnapshot& lobby = clientA.LatestLobbySnapshot();
            if (!lobby.canStart && lobby.statusMessage == "duplicate hero in team")
            {
                duplicateBlocked = true;
                LobbyUpdate update;
                update.playerName = "Bob";
                update.selectedTeam = 0;
                update.selectedHero = 1;
                update.ready = true;
                update.startRequested = true;
                clientB.SendLobbyUpdate(update);
                duplicateFixed = true;
            }
        }

        sendMove(clientA);
        sendMove(clientB);

        if (clientB.HasSnapshot() && clientA.InMatch())
        {
            Vec3 p;
            if (posInSnapshot(clientB.LatestSnapshot(), clientA.AssignedPlayerId(), p))
            {
                if (!haveFirst)
                {
                    bSeesAFirst = p;
                    haveFirst = true;
                }
                else
                {
                    aMovementSeenByB = std::max(aMovementSeenByB, (p - bSeesAFirst).Length());
                }
            }
        }
        if (haveFirst)
        {
            if (++ticksAfterFirst > 120)
            {
                break; // ~enough movement observed
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const int playerA = clientA.AssignedPlayerId();
    const int playerB = clientB.AssignedPlayerId();
    Vec3 tmp {};
    const bool aSeesA = posInSnapshot(clientA.LatestSnapshot(), playerA, tmp);
    const bool aSeesB = posInSnapshot(clientA.LatestSnapshot(), playerB, tmp);
    const bool bSeesA = posInSnapshot(clientB.LatestSnapshot(), playerA, tmp);
    const bool bSeesB = posInSnapshot(clientB.LatestSnapshot(), playerB, tmp);

    const bool bothConnected = clientA.IsConnected() && clientB.IsConnected()
        && clientA.InMatch() && clientB.InMatch();
    const bool differentPlayers = playerA >= 0 && playerB >= 0 && playerA != playerB;
    const bool badDenied = clientBad.WasDenied();
    const bool lobbySnapshots = clientA.HasLobbySnapshot() && clientB.HasLobbySnapshot();
    const bool lobbyStartedBoth = clientA.LatestLobbySnapshot().matchStarted
        && clientB.LatestLobbySnapshot().matchStarted;
    const bool bothSeeBoth = aSeesA && aSeesB && bSeesA && bSeesB;
    const bool movementVisible = aMovementSeenByB > 0.5f;

    // Phase 2: A disconnects. The server must keep running and keep serving B.
    const std::uint32_t bTickBefore = clientB.LatestSnapshot().tick;
    const std::uint32_t clientAFullBeforeDisconnect = clientA.FullSnapshotsReceived();
    const std::uint32_t clientADeltaBeforeDisconnect = clientA.DeltaSnapshotsReceived();
    clientA.Disconnect();
    bool serverSurvived = true;
    const Clock::time_point deadline2 = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline2)
    {
        NetworkServerTick(server, fixedDt); // must not crash after a disconnect
        clientB.Poll();
        sendMove(clientB);
        if (server.ClientCount() <= 1 && clientB.LatestSnapshot().tick > bTickBefore + 60)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool aClientGone = server.ClientCount() == 1; // only B remains connected
    const bool noAiTakeover = IsNetworkControlledPlayer(playerA);
    const bool bStillConnected = clientB.IsConnected();
    const bool bStillReceiving = clientB.LatestSnapshot().tick > bTickBefore;

    // Phase 3: Alice reconnects by name into the reserved slot; an unrelated
    // late join is denied. Then send an old command tick and ensure the server
    // drops it instead of replaying stale input.
    ClientTransport clientAReconnect;
    ClientTransport clientLate;
    clientAReconnect.Open("127.0.0.1", port, "secret", 3.0f);
    clientLate.Open("127.0.0.1", port, "secret", 3.0f);
    bool sentReconnectLobby = false;
    bool sentLateLobby = false;
    bool reconnectSlot = false;
    bool lateDenied = false;
    const Clock::time_point deadline3 = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline3)
    {
        NetworkServerTick(server, fixedDt);
        clientB.Poll();
        clientAReconnect.Poll();
        clientLate.Poll();

        if (clientAReconnect.IsConnected() && !sentReconnectLobby)
        {
            LobbyUpdate update;
            update.playerName = "Alice";
            update.selectedTeam = 0;
            update.selectedHero = 0;
            update.ready = true;
            clientAReconnect.SendLobbyUpdate(update);
            sentReconnectLobby = true;
        }
        if (clientLate.IsConnected() && !sentLateLobby)
        {
            LobbyUpdate update;
            update.playerName = "Eve";
            update.selectedTeam = 1;
            update.selectedHero = 2;
            update.ready = true;
            clientLate.SendLobbyUpdate(update);
            sentLateLobby = true;
        }

        sendMove(clientB);
        sendMove(clientAReconnect);

        reconnectSlot = clientAReconnect.InMatch()
            && clientAReconnect.AssignedPlayerId() == playerA;
        lateDenied = clientLate.WasDenied();
        if (reconnectSlot && lateDenied && clientB.HasSnapshot()
            && clientAReconnect.HasSnapshot())
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const Player* reconnectedServerPlayer = matchSimulation_.GetPlayer(playerA);
    const bool reconnectRespawning = reconnectedServerPlayer != nullptr
        && !reconnectedServerPlayer->IsAlive()
        && !reconnectedServerPlayer->IsEliminated()
        && reconnectedServerPlayer->GetRespawnTimer() > 6.0f
        && reconnectedServerPlayer->GetRespawnTimer() <= kReconnectRespawnSeconds;

    const std::uint32_t staleBefore = server.StaleCommandsDropped();
    PlayerCommand stale;
    stale.controlledPlayerId = static_cast<std::uint32_t>(playerB);
    stale.tick = clientB.LastAckedCommandTick() > 0 ? clientB.LastAckedCommandTick() : 1;
    stale.moveForward = 1.0f;
    clientB.SendCommand(stale);
    const Clock::time_point staleDeadline = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < staleDeadline && server.StaleCommandsDropped() == staleBefore)
    {
        NetworkServerTick(server, fixedDt);
        clientB.Poll();
        clientAReconnect.Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const Clock::time_point reconnectDeltaDeadline = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < reconnectDeltaDeadline && clientAReconnect.DeltaSnapshotsReceived() == 0)
    {
        NetworkServerTick(server, fixedDt);
        clientB.Poll();
        clientAReconnect.Poll();
        sendMove(clientB);
        sendMove(clientAReconnect);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const bool staleDropped = server.StaleCommandsDropped() > staleBefore;
    const std::uint32_t staleDropCount = server.StaleCommandsDropped();
    const std::string lateDenyReason = clientLate.DenyReason();
    const std::uint32_t ackB = clientB.LastAckedCommandTick();
    const bool deltaSnapshotsActive = clientAFullBeforeDisconnect > 0
        && clientB.FullSnapshotsReceived() > 0
        && clientADeltaBeforeDisconnect > 0
        && clientB.DeltaSnapshotsReceived() > 0;
    const bool deltaSmallerThanFull =
        clientB.LastFullSnapshotBytes() > 0
        && clientB.LastDeltaSnapshotBytes() > 0
        && clientB.LastDeltaSnapshotBytes() < clientB.LastFullSnapshotBytes();
    const bool reconnectBaselineThenDeltas =
        clientAReconnect.FullSnapshotsReceived() > 0
        && clientAReconnect.DeltaSnapshotsReceived() > 0;
    const std::uint32_t clientBDropped = clientB.DroppedSnapshots();
    const std::uint32_t clientBIgnored = clientB.IgnoredSnapshots();
    const std::uint32_t serverResyncs = server.ResyncRequestsReceived();
    const std::uint32_t clientAFull = clientAFullBeforeDisconnect;
    const std::uint32_t clientADelta = clientADeltaBeforeDisconnect;
    const std::uint32_t clientBFull = clientB.FullSnapshotsReceived();
    const std::uint32_t clientBDelta = clientB.DeltaSnapshotsReceived();
    const std::uint32_t reconnectFull = clientAReconnect.FullSnapshotsReceived();
    const std::uint32_t reconnectDelta = clientAReconnect.DeltaSnapshotsReceived();
    const std::size_t clientBFullBytes = clientB.LastFullSnapshotBytes();
    const std::size_t clientBDeltaBytes = clientB.LastDeltaSnapshotBytes();

    // Capture final teardown state BEFORE closing (diagnostics use captured
    // booleans above, not live post-teardown values).
    clientAReconnect.Disconnect();
    clientLate.Disconnect();
    clientB.Disconnect();
    server.Close();

    std::cout << "mp-smoke: lobbySnapshots=" << (lobbySnapshots ? "yes" : "no")
              << " duplicateHeroBlocked=" << (duplicateBlocked ? "yes" : "no")
              << " lobbyStartedBoth=" << (lobbyStartedBoth ? "yes" : "no")
              << '\n';
    std::cout << "mp-smoke: bothConnected=" << (bothConnected ? "yes" : "no")
              << " playerA=" << playerA << " playerB=" << playerB
              << " (different=" << (differentPlayers ? "yes" : "no") << ')'
              << " badPassword=" << (badDenied ? ("denied(" + clientBad.DenyReason() + ")") : std::string("ACCEPTED"))
              << '\n';
    std::cout << "mp-smoke: bothSeeBoth=" << (bothSeeBoth ? "yes" : "no")
              << " aMovedSeenByB=" << aMovementSeenByB
              << " afterDisconnect[aClientGone=" << (aClientGone ? "yes" : "no")
              << " noAiTakeover=" << (noAiTakeover ? "yes" : "no")
              << " bConnected=" << (bStillConnected ? "yes" : "no")
              << " bReceiving=" << (bStillReceiving ? "yes" : "no")
              << " serverAlive=" << (serverSurvived ? "yes" : "no") << "]\n";
    std::cout << "mp-smoke: reconnectSlot=" << (reconnectSlot ? "yes" : "no")
              << " reconnectRespawning=" << (reconnectRespawning ? "yes" : "no")
              << " lateJoin=" << (lateDenied ? ("denied(" + lateDenyReason + ")") : std::string("ACCEPTED"))
              << " staleDropped=" << (staleDropped ? "yes" : "no")
              << " staleDrops=" << staleDropCount
              << " ackB=" << ackB << '\n';
    std::cout << "mp-smoke: snapshots full/delta A="
              << clientAFull << '/' << clientADelta
              << " B=" << clientBFull << '/' << clientBDelta
              << " reconnect=" << reconnectFull << '/' << reconnectDelta
              << " sizeB[full/delta]=" << clientBFullBytes << '/' << clientBDeltaBytes
              << " deltaSmaller=" << (deltaSmallerThanFull ? "yes" : "no")
              << " drop/ignoreB=" << clientBDropped << '/' << clientBIgnored
              << " resyncServer=" << serverResyncs << '\n';

    const bool ok = lobbySnapshots && duplicateBlocked && lobbyStartedBoth
        && bothConnected && differentPlayers && badDenied && bothSeeBoth
        && movementVisible && serverSurvived && aClientGone && noAiTakeover
        && bStillConnected && bStillReceiving
        && reconnectSlot && reconnectRespawning && lateDenied && staleDropped
        && deltaSnapshotsActive && deltaSmallerThanFull && reconnectBaselineThenDeltas;

    if (!ok)
    {
        std::cout << "mp-smoke: checks lobbySnapshots=" << (lobbySnapshots ? "ok" : "FAIL")
                  << " duplicateBlocked=" << (duplicateBlocked ? "ok" : "FAIL")
                  << " lobbyStartedBoth=" << (lobbyStartedBoth ? "ok" : "FAIL")
                  << " bothConnected=" << (bothConnected ? "ok" : "FAIL")
                  << " differentPlayers=" << (differentPlayers ? "ok" : "FAIL")
                  << " badDenied=" << (badDenied ? "ok" : "FAIL")
                  << " bothSeeBoth=" << (bothSeeBoth ? "ok" : "FAIL")
                  << " movementVisible=" << (movementVisible ? "ok" : "FAIL")
                  << " aClientGone=" << (aClientGone ? "ok" : "FAIL")
                  << " noAiTakeover=" << (noAiTakeover ? "ok" : "FAIL")
                  << " bStillConnected=" << (bStillConnected ? "ok" : "FAIL")
                  << " bStillReceiving=" << (bStillReceiving ? "ok" : "FAIL")
                  << " reconnectSlot=" << (reconnectSlot ? "ok" : "FAIL")
                  << " reconnectRespawning=" << (reconnectRespawning ? "ok" : "FAIL")
                  << " lateDenied=" << (lateDenied ? "ok" : "FAIL")
                  << " staleDropped=" << (staleDropped ? "ok" : "FAIL")
                  << " deltaActive=" << (deltaSnapshotsActive ? "ok" : "FAIL")
                  << " deltaSmaller=" << (deltaSmallerThanFull ? "ok" : "FAIL")
                  << " reconnectBaselineDelta=" << (reconnectBaselineThenDeltas ? "ok" : "FAIL")
                  << '\n';
    }

    std::cout << (ok ? "MP_LOOPBACK_SMOKE_OK" : "MP_LOOPBACK_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunClientGuiSmoke()
{
    if (!NetworkTransportAvailable())
    {
        std::cout << "client-gui-smoke: network transport disabled at build "
                     "(DAIBED_ENABLE_NETWORK=OFF) — skipped\n";
        std::cout << "CLIENT_GUI_SMOKE_SKIPPED" << std::endl;
        return 0;
    }

    // A second, headless Game drives the authoritative server in this same process
    // (no second window). A Game method may touch another Game's private members.
    Game server;
    server.Initialize(true);

    ServerConfig cfg;
    cfg.listenAddress = "127.0.0.1";
    cfg.port = 0;              // ephemeral
    cfg.minPlayersToStart = 1; // a single client may start the match
    cfg.password.clear();

    ServerTransport transport;
    if (!server.NetworkServerSetup(transport, cfg))
    {
        std::cout << "client-gui-smoke: server setup failed: " << transport.LastError() << '\n';
        std::cout << "CLIENT_GUI_SMOKE_FAIL" << std::endl;
        server.Shutdown();
        return 9;
    }
    const std::uint16_t port = transport.BoundPort();
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    std::cout << "client-gui-smoke: server on 127.0.0.1:" << port << " (headless)\n";

    networkMode_ = NetworkMode::LocalClient;
    clientWorldBuilt_ = false;
    networkAssignedPlayerId_ = -1;

    ClientTransport client;
    client.Open("127.0.0.1", port, "", 3.0f);

    bool sentLobby = false;
    int renderedFrames = 0;
    int snapshotsApplied = 0;
    bool bufferedBeforeWorld = false;
    bool appliedBeforeWorld = false;
    constexpr int kTargetFrames = 30;
    constexpr int kMaxIterations = 1200;
    for (int i = 0; i < kMaxIterations && renderedFrames < kTargetFrames; ++i)
    {
        // Authoritative server: poll, advance, broadcast per-client snapshots.
        server.NetworkServerTick(transport, fixedDt);
        client.Poll();

        if (client.IsConnected() && !sentLobby)
        {
            LobbyUpdate update;
            update.playerName = "GuiSmoke";
            update.selectedTeam = 0;
            update.selectedHero = 0;
            update.ready = true;
            update.startRequested = true;
            client.SendLobbyUpdate(update);
            sentLobby = true;
        }

        if (client.InMatch())
        {
            if (client.HasSnapshot() && !clientWorldBuilt_)
            {
                bufferedBeforeWorld = true;
            }
            if (!clientWorldBuilt_)
            {
                networkAssignedPlayerId_ = client.AssignedPlayerId();
                localPlayerId_ = networkAssignedPlayerId_;
                BuildClientWorld(client.LatestLobbySnapshot());
                clientWorldBuilt_ = true;
            }
            if (client.HasSnapshot())
            {
                if (!clientWorldBuilt_)
                {
                    appliedBeforeWorld = true;
                }
                PushRemoteSnapshot(client.LatestSnapshot());
                ApplyClientSnapshot(client.LatestSnapshot());
                ++snapshotsApplied;
            }
            UpdateCamera(fixedDt);
            Render();
            ++renderedFrames;
        }
        else
        {
            RenderNetworkLobby(client.LatestLobbySnapshot(), client.LobbyClientId(), LobbyUpdate {});
        }
    }

    // Capture results BEFORE teardown (post-Disconnect getters would read false).
    const bool connected = client.IsConnected() && client.InMatch();
    const bool gotSnapshot = client.HasSnapshot() && snapshotsApplied > 0;
    const std::size_t snapPlayers = client.HasSnapshot() ? client.LatestSnapshot().players.size() : 0;
    const std::size_t clientBlocks = world_.GetBlocks().size();
    const std::size_t serverBlocks = server.world_.GetBlocks().size();
    const std::size_t clientPlayers = matchSimulation_.Players().size();
    const std::size_t clientCores = matchSimulation_.Cores().size();
    const std::size_t serverCores = server.matchSimulation_.Cores().size();
    const std::size_t clientGenerators = matchSimulation_.Generators().size();
    const std::size_t serverGenerators = server.matchSimulation_.Generators().size();
    const int assigned = networkAssignedPlayerId_;

    const bool worldBuilt = clientWorldBuilt_ && clientBlocks > 0;
    const bool worldInSync = clientBlocks == serverBlocks && serverBlocks > 0
        && clientCores == serverCores && serverCores > 0
        && clientGenerators == serverGenerators && serverGenerators > 0;
    const bool playersReplicated = clientPlayers == snapPlayers && snapPlayers > 0;
    const bool rendered = renderedFrames >= kTargetFrames;

    client.Disconnect();
    transport.Close();
    server.Shutdown();

    std::cout << "client-gui-smoke: connected=" << (connected ? "yes" : "no")
              << " assignedPlayer=" << assigned
              << " gotSnapshot=" << (gotSnapshot ? "yes" : "no")
              << " snapshotsApplied=" << snapshotsApplied
              << " renderedFrames=" << renderedFrames
              << " bootstrapBuffered=" << (bufferedBeforeWorld ? "yes" : "no")
              << " appliedBeforeWorld=" << (appliedBeforeWorld ? "yes" : "no")
              << " players[client/snapshot]=" << clientPlayers << '/' << snapPlayers
              << " worldBlocks[client/server]=" << clientBlocks << '/' << serverBlocks
              << " cores[client/server]=" << clientCores << '/' << serverCores
              << " generators[client/server]=" << clientGenerators << '/' << serverGenerators << '\n';

    const bool ok = connected && gotSnapshot && worldBuilt && worldInSync
        && playersReplicated && rendered && !appliedBeforeWorld;
    if (!ok)
    {
        std::cout << "client-gui-smoke: checks connected=" << (connected ? "ok" : "FAIL")
                  << " gotSnapshot=" << (gotSnapshot ? "ok" : "FAIL")
                  << " worldBuilt=" << (worldBuilt ? "ok" : "FAIL")
                  << " worldInSync=" << (worldInSync ? "ok" : "FAIL")
                  << " playersReplicated=" << (playersReplicated ? "ok" : "FAIL")
                  << " rendered=" << (rendered ? "ok" : "FAIL") << '\n';
    }

    std::cout << (ok ? "CLIENT_GUI_SMOKE_OK" : "CLIENT_GUI_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunClientInputSmoke()
{
    if (!NetworkTransportAvailable())
    {
        std::cout << "client-input-smoke: network transport disabled at build "
                     "(DAIBED_ENABLE_NETWORK=OFF) — skipped\n";
        std::cout << "CLIENT_INPUT_SMOKE_SKIPPED" << std::endl;
        return 0;
    }

    // A second, headless Game drives the authoritative server in this same process
    // (no second window). A Game method may touch another Game's private members.
    Game server;
    server.Initialize(true);

    ServerConfig cfg;
    cfg.listenAddress = "127.0.0.1";
    cfg.port = 0;              // ephemeral
    cfg.minPlayersToStart = 1; // a single client may start the match
    cfg.password.clear();

    ServerTransport transport;
    if (!server.NetworkServerSetup(transport, cfg))
    {
        std::cout << "client-input-smoke: server setup failed: " << transport.LastError() << '\n';
        std::cout << "CLIENT_INPUT_SMOKE_FAIL" << std::endl;
        server.Shutdown();
        return 9;
    }
    const std::uint16_t port = transport.BoundPort();
    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    std::cout << "client-input-smoke: server on 127.0.0.1:" << port << " (headless)\n";

    networkMode_ = NetworkMode::LocalClient;
    clientWorldBuilt_ = false;
    networkAssignedPlayerId_ = -1;

    ClientTransport client;
    client.Open("127.0.0.1", port, "", 3.0f);

    bool sentLobby = false;
    int assignedId = -1;
    int commandsSent = 0;
    int snapshotsApplied = 0;

    // Aim is decided once from the server-side spawn facing plus a fixed offset, so
    // moving forward clears the spawn area while the yaw is provably distinct from
    // the spawn value (proving the command's aimYaw propagated, not a no-op).
    constexpr float kAimOffset = 0.5f;
    float injectedAimYaw = 0.0f;
    bool haveAim = false;
    Vec3 serverStartPos {};
    bool haveServerStart = false;

    Vec3 clientFirstSnapPos {};
    bool haveClientFirst = false;
    Vec3 clientLatestSnapPos {};
    float clientLatestYaw = 0.0f;
    bool haveClientLatest = false;

    constexpr int kMoveTicks = 150;
    constexpr int kMaxIterations = 2000;
    int moveTicks = 0;
    for (int i = 0; i < kMaxIterations && moveTicks < kMoveTicks; ++i)
    {
        // Authoritative server: poll, apply queued client commands, advance, send.
        server.NetworkServerTick(transport, fixedDt);
        client.Poll();

        if (client.IsConnected() && !sentLobby)
        {
            LobbyUpdate update;
            update.playerName = "InputSmoke";
            update.selectedTeam = 0;
            update.selectedHero = 0;
            update.ready = true;
            update.startRequested = true;
            client.SendLobbyUpdate(update);
            sentLobby = true;
        }

        if (client.InMatch())
        {
            assignedId = client.AssignedPlayerId();
            if (!clientWorldBuilt_)
            {
                networkAssignedPlayerId_ = assignedId;
                localPlayerId_ = assignedId;
                BuildClientWorld(client.LatestLobbySnapshot());
                clientWorldBuilt_ = true;
            }
            if (client.HasSnapshot())
            {
                PushRemoteSnapshot(client.LatestSnapshot());
                ApplyClientSnapshot(client.LatestSnapshot());
                ++snapshotsApplied;
            }

            // Decide the aim and capture the server-side baseline BEFORE the first
            // movement command is applied.
            if (!haveAim || !haveServerStart)
            {
                const Player* sp = server.matchSimulation_.GetPlayer(assignedId);
                if (sp != nullptr)
                {
                    if (!haveAim)
                    {
                        injectedAimYaw = sp->GetYaw() + kAimOffset;
                        haveAim = true;
                    }
                    if (!haveServerStart)
                    {
                        serverStartPos = sp->GetPositionVec3();
                        haveServerStart = true;
                    }
                }
            }

            // Inject the real client send path: moveForward + fixed aimYaw, for the
            // assigned player, shipped over the transport (no keyboard in headless).
            // Stamp a MONOTONIC command tick (as the real client now does) so the
            // server applies every command instead of dropping snapshot-derived
            // duplicate ticks as stale — full-speed movement, not ~1/3 speed.
            PlayerCommand command;
            command.controlledPlayerId = static_cast<std::uint32_t>(assignedId);
            command.tick = ++networkCommandTick_;
            command.aimYaw = injectedAimYaw;
            command.moveForward = 1.0f;
            client.SendCommand(command);
            ++commandsSent;

            // Track the assigned player's replicated position/yaw in the accepted
            // snapshot (this is what a real GUI client would render).
            if (client.HasSnapshot())
            {
                for (const PlayerSnapshot& entry : client.LatestSnapshot().players)
                {
                    if (entry.playerId == assignedId)
                    {
                        if (!haveClientFirst)
                        {
                            clientFirstSnapPos = entry.position;
                            haveClientFirst = true;
                        }
                        clientLatestSnapPos = entry.position;
                        clientLatestYaw = entry.yaw;
                        haveClientLatest = true;
                        break;
                    }
                }
            }
            ++moveTicks;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Server-side authoritative outcome (no RTT): the assigned player moved and its
    // yaw equals the command's aimYaw — proof the server accepted+applied commands.
    Vec3 serverEndPos {};
    float serverEndYaw = 0.0f;
    const Player* serverPlayer = server.matchSimulation_.GetPlayer(assignedId);
    if (serverPlayer != nullptr)
    {
        serverEndPos = serverPlayer->GetPositionVec3();
        serverEndYaw = serverPlayer->GetYaw();
    }
    const float serverMovedDistance = haveServerStart ? (serverEndPos - serverStartPos).Length() : 0.0f;
    const float clientMovedDistance = (haveClientFirst && haveClientLatest)
        ? (clientLatestSnapPos - clientFirstSnapPos).Length() : 0.0f;

    // Bad-path: a --connect to a dead server (port 1) must fail gracefully (no crash).
    ClientTransport deadClient;
    const bool deadConnected = deadClient.Connect("127.0.0.1", 1, "", 0.4f);
    deadClient.Disconnect();

    // Capture results BEFORE teardown (post-Disconnect getters would read false).
    const bool inMatch = client.InMatch();
    const std::uint32_t packetsRx = transport.PacketsReceived();
    const std::uint32_t staleDropped = transport.StaleCommandsDropped();

    client.Disconnect();
    transport.Close();
    server.Shutdown();

    const bool joined = inMatch && assignedId >= 0 && commandsSent > 0;
    const bool serverAcceptedCommands = serverMovedDistance > 0.5f
        && std::fabs(serverEndYaw - injectedAimYaw) < 0.001f && packetsRx > 0;
    const bool clientSawMovement = clientMovedDistance > 0.5f;
    const bool clientYawMatches = haveClientLatest
        && std::fabs(clientLatestYaw - injectedAimYaw) < 0.01f;
    const bool badPathGraceful = !deadConnected; // returned false, no crash

    std::cout << "client-input-smoke: assignedPlayer=" << assignedId
              << " commandsSent=" << commandsSent
              << " snapshotsApplied=" << snapshotsApplied
              << " packetsRx=" << packetsRx
              << " injectedAimYaw=" << injectedAimYaw << '\n';
    std::cout << "client-input-smoke: serverMoved=" << serverMovedDistance
              << " serverYaw=" << serverEndYaw
              << " clientMoved=" << clientMovedDistance
              << " clientYaw=" << clientLatestYaw
              << " staleDropped=" << staleDropped
              << " deadServerConnected=" << (deadConnected ? "yes" : "no") << '\n';

    const bool ok = joined && serverAcceptedCommands && clientSawMovement
        && clientYawMatches && badPathGraceful;
    if (!ok)
    {
        std::cout << "client-input-smoke: checks joined=" << (joined ? "ok" : "FAIL")
                  << " serverAccepted=" << (serverAcceptedCommands ? "ok" : "FAIL")
                  << " clientMoved=" << (clientSawMovement ? "ok" : "FAIL")
                  << " clientYaw=" << (clientYawMatches ? "ok" : "FAIL")
                  << " badPath=" << (badPathGraceful ? "ok" : "FAIL") << '\n';
    }

    std::cout << (ok ? "CLIENT_INPUT_SMOKE_OK" : "CLIENT_INPUT_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunNetworkRangedSmoke()
{
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();

    const bool noStartingBlocks = std::all_of(
        matchSimulation_.Players().begin(),
        matchSimulation_.Players().end(),
        [](const Player& player)
        {
            return player.GetInventory().GetBlocks() == 0;
        });

    if (matchSimulation_.Players().empty())
    {
        std::cout << "network-ranged-smoke: no players\n"
                     "NETWORK_RANGED_SMOKE_FAIL" << std::endl;
        return 9;
    }

    Player& controlled = matchSimulation_.Players().front();
    const int controlledId = controlled.GetId();
    MarkNetworkControlledPlayer(controlledId);
    controlled.SetPosition(Vec3 { 0.0f, 38.0f, 0.0f });

    const auto normalized = [](Vector3 value)
    {
        const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
        return length > 0.0001f
            ? Vector3 { value.x / length, value.y / length, value.z / length }
            : Vector3 {};
    };
    const auto dot = [](Vector3 a, Vector3 b)
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    const auto directionMatches = [&normalized, &dot](const EnergyProjectile& projectile, Vector3 expected, float threshold)
    {
        return dot(normalized(projectile.velocity), normalized(expected)) >= threshold;
    };

    const float fixedDt = matchSimulation_.FixedDeltaSeconds();

    // Fireball quick-use: previously this used the server camera through
    // LaunchProjectile(); it must now use PlayerCommand aim.
    ItemStack fireball;
    fireball.type = ItemType::Fireball;
    fireball.count = 1;
    controlled.GetInventory().SwapSlot(0, fireball);
    PlayerCommand fireballCommand;
    fireballCommand.controlledPlayerId = static_cast<std::uint32_t>(controlledId);
    fireballCommand.selectedSlot = 0;
    fireballCommand.aimYaw = PI / 2.0f; // +X
    fireballCommand.aimPitch = 0.0f;
    fireballCommand.useFireball = true;
    ApplyPlayerCommand(controlled, fireballCommand, 0.0f);
    const std::size_t projectilesBeforeFireball = projectiles_.size();
    suppressLocalFeedback_ = false;
    const std::string messageBeforeFireball = message_;
    const std::size_t eventMessagesBeforeFireball = eventMessages_.size();
    const std::size_t worldEffectsBeforeFireball = worldEffects_.size();
    const std::size_t floatingTextsBeforeFireball = floatingTexts_.size();
    const bool audioMutedBeforeFireball = audio_.IsMuted();
    UseUtilityInputs(controlled, fireballCommand);
    const bool fireballSpawned = projectiles_.size() == projectilesBeforeFireball + 1
        && projectiles_.back().kind == ProjectileKind::Fireball;
    const bool fireballAimOk = fireballSpawned
        && directionMatches(projectiles_.back(), AimDirectionFromCommand(fireballCommand), 0.999f);
    const bool fireballPresentationClean =
        message_ == messageBeforeFireball
        && eventMessages_.size() == eventMessagesBeforeFireball
        && worldEffects_.size() == worldEffectsBeforeFireball
        && floatingTexts_.size() == floatingTextsBeforeFireball
        && audio_.IsMuted() == audioMutedBeforeFireball;
    const MatchSnapshot fireballOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool fireballResultReplicated = std::any_of(
        fireballOwnerView.actionResults.begin(),
        fireballOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::UtilityUse)
                && result.subjectType == static_cast<int>(UtilityType::Fireball)
                && result.success;
        });

    projectiles_.clear();
    recentActionResults_.clear();
    controlled.ResetAttackCooldown(0.0f);

    // Bow draw/release: charge state lives on the player; release consumes an
    // arrow and spawns an arrow along command aim.
    ItemStack bow;
    bow.type = ItemType::Bow;
    bow.count = 1;
    ItemStack arrows;
    arrows.type = ItemType::EnergyArrow;
    arrows.count = 4;
    controlled.GetInventory().SwapSlot(0, bow);
    controlled.GetInventory().SwapSlot(1, arrows);
    controlled.SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;

    PlayerCommand bowCommand;
    bowCommand.controlledPlayerId = static_cast<std::uint32_t>(controlledId);
    bowCommand.selectedSlot = 0;
    bowCommand.aimYaw = -PI / 2.0f; // -X
    bowCommand.aimPitch = 0.08f;
    bowCommand.attackHeld = true;
    ApplyPlayerCommand(controlled, bowCommand, 0.0f);
    for (int i = 0; i < 72; ++i)
    {
        ApplyNetworkPlayerActions(controlled, bowCommand, fixedDt);
    }
    PlayerCommand bowRelease = bowCommand;
    bowRelease.attackHeld = false;
    bowRelease.attackReleased = true;
    ApplyNetworkPlayerActions(controlled, bowRelease, fixedDt);
    const bool bowSpawned = projectiles_.size() == 1
        && projectiles_.back().kind == ProjectileKind::Arrow;
    const bool bowAimOk = bowSpawned
        && directionMatches(projectiles_.back(), AimDirectionFromCommand(bowCommand), 0.999f);
    // Stable spawn id (not the vector index — see EnergyProjectile::id): the
    // real LaunchBowShot path must assign a real id, not the struct default -1.
    const int bowProjectileId = bowSpawned ? projectiles_.back().id : -1;
    const bool bowHasStableId = bowProjectileId > 0;
    const MatchSnapshot bowOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool bowResultReplicated = std::any_of(
        bowOwnerView.actionResults.begin(),
        bowOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::ProjectileLaunch)
                && result.subjectType == static_cast<int>(ProjectileKind::Arrow)
                && result.success;
        });

    projectiles_.clear();
    recentActionResults_.clear();

    // Blaster charge/fire: after loading, attackPressed fires along command aim.
    ItemStack blaster;
    blaster.type = ItemType::Blaster;
    blaster.count = 1;
    controlled.GetInventory().SwapSlot(0, blaster);
    controlled.SetSelectedSlot(0);
    selectedHotbarSlot_ = 0;
    controlled.CancelBlasterLoading();

    PlayerCommand blasterCommand;
    blasterCommand.controlledPlayerId = static_cast<std::uint32_t>(controlledId);
    blasterCommand.selectedSlot = 0;
    blasterCommand.aimYaw = 0.0f; // -Z
    blasterCommand.aimPitch = -0.04f;
    blasterCommand.attackHeld = true;
    ApplyPlayerCommand(controlled, blasterCommand, 0.0f);
    for (int i = 0; i < 90; ++i)
    {
        ApplyNetworkPlayerActions(controlled, blasterCommand, fixedDt);
    }
    PlayerCommand blasterFire = blasterCommand;
    blasterFire.attackHeld = false;
    blasterFire.attackPressed = true;
    blasterFire.placeHeld = true;
    ApplyNetworkPlayerActions(controlled, blasterFire, fixedDt);
    const bool blasterSpawned = projectiles_.size() == 1
        && projectiles_.back().kind == ProjectileKind::Blaster;
    const bool blasterAimOk = blasterSpawned
        && directionMatches(projectiles_.back(), AimDirectionFromCommand(blasterCommand), 0.995f);
    // Stable spawn id, and distinct from the bow's (proves ids are a genuine
    // per-spawn counter, not e.g. always resetting to the same value).
    const bool blasterHasStableId = blasterSpawned
        && projectiles_.back().id > 0
        && projectiles_.back().id != bowProjectileId;
    const MatchSnapshot blasterOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool blasterResultReplicated = std::any_of(
        blasterOwnerView.actionResults.begin(),
        blasterOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::ProjectileLaunch)
                && result.subjectType == static_cast<int>(ProjectileKind::Blaster)
                && result.success;
        });

    networkControlledPlayerIds_.clear();
    networkActionState_.clear();
    projectiles_.clear();

    // Stable spawn ids across expiry: the bug this fix closes is an EARLIER
    // projectile expiring shifting a SURVIVING projectile's id (which happened
    // when id was the vector index — see NetworkSnapshot.h). Spawn two, expire
    // the first via UpdateProjectiles, and assert the survivor's id is
    // unchanged (not reassigned to what used to be the expired one's slot).
    EnergyProjectile expiringProjectile {};
    expiringProjectile.id = NextProjectileId();
    expiringProjectile.kind = ProjectileKind::Arrow;
    expiringProjectile.lifetime = 0.001f;
    projectiles_.push_back(expiringProjectile);
    EnergyProjectile survivorProjectile {};
    survivorProjectile.id = NextProjectileId();
    survivorProjectile.kind = ProjectileKind::Arrow;
    survivorProjectile.lifetime = 5.0f;
    survivorProjectile.position = Vector3 { 0.0f, 60.0f, 0.0f };
    survivorProjectile.previousPosition = survivorProjectile.position;
    survivorProjectile.velocity = Vector3 { 0.0f, 0.0f, 0.0f };
    projectiles_.push_back(survivorProjectile);
    const int survivorIdBeforeExpiry = survivorProjectile.id;
    UpdateProjectiles(matchSimulation_.FixedDeltaSeconds());
    const bool stableIdSurvivedExpiry = projectiles_.size() == 1
        && projectiles_.front().id == survivorIdBeforeExpiry;
    projectiles_.clear();

    // Same proof for explosives and hazard zones (the other two entity types
    // fixed in the same pass — see NetworkSnapshot.h). Positions far from the
    // arena and any player so DetonateAt's side effects (block break, nearby
    // damage) can't interfere with this or later checks.
    TimedExplosion expiringExplosive {};
    expiringExplosive.position = Vector3 { 200.0f, 60.0f, 200.0f };
    expiringExplosive.timer = 0.001f;
    expiringExplosive.id = NextExplosiveId();
    timedExplosions_.push_back(expiringExplosive);
    TimedExplosion survivorExplosive {};
    survivorExplosive.position = Vector3 { 205.0f, 60.0f, 200.0f };
    survivorExplosive.timer = 5.0f;
    survivorExplosive.id = NextExplosiveId();
    timedExplosions_.push_back(survivorExplosive);
    const int survivorExplosiveIdBeforeExpiry = survivorExplosive.id;
    UpdateExplosives(matchSimulation_.FixedDeltaSeconds());
    const bool explosiveIdSurvivedExpiry = timedExplosions_.size() == 1
        && timedExplosions_.front().id == survivorExplosiveIdBeforeExpiry;
    const bool explosiveFallsWithoutSupport = explosiveIdSurvivedExpiry
        && timedExplosions_.front().position.y < survivorExplosive.position.y;
    timedExplosions_.clear();

    HazardZone expiringHazard {};
    expiringHazard.position = Vector3 { 200.0f, 60.0f, 200.0f };
    expiringHazard.lifetime = 0.001f;
    expiringHazard.id = NextHazardZoneId();
    hazardZones_.push_back(expiringHazard);
    HazardZone survivorHazard {};
    survivorHazard.position = Vector3 { 205.0f, 60.0f, 200.0f };
    survivorHazard.lifetime = 5.0f;
    survivorHazard.id = NextHazardZoneId();
    hazardZones_.push_back(survivorHazard);
    const int survivorHazardIdBeforeExpiry = survivorHazard.id;
    UpdateHazardZones(matchSimulation_.FixedDeltaSeconds());
    const bool hazardIdSurvivedExpiry = hazardZones_.size() == 1
        && hazardZones_.front().id == survivorHazardIdBeforeExpiry;
    hazardZones_.clear();

    // Ranged weapons and fire may collide with a Core, but only the melee
    // mining path is allowed to change Core HP.
    EnergyCore* rangedTargetCore = nullptr;
    for (EnergyCore& core : matchSimulation_.Cores())
    {
        if (core.GetTeamId() != controlled.GetTeamId() && core.IsAlive())
        {
            rangedTargetCore = &core;
            break;
        }
    }
    const int coreHpBeforeProjectile = rangedTargetCore != nullptr ? rangedTargetCore->GetHealth() : -1;
    if (rangedTargetCore != nullptr)
    {
        EnergyProjectile coreProjectile {};
        coreProjectile.id = NextProjectileId();
        coreProjectile.ownerId = controlled.GetId();
        coreProjectile.ownerTeamId = controlled.GetTeamId();
        coreProjectile.kind = ProjectileKind::Blaster;
        coreProjectile.position = world_.GridToWorld(rangedTargetCore->GetBlockPosition());
        coreProjectile.previousPosition = coreProjectile.position;
        coreProjectile.damage = 99;
        coreProjectile.lifetime = 1.0f;
        coreProjectile.maxRange = 80.0f;
        projectiles_.push_back(coreProjectile);
        UpdateProjectiles(fixedDt);
    }
    const bool projectileCoreImmune = rangedTargetCore != nullptr
        && rangedTargetCore->GetHealth() == coreHpBeforeProjectile;

    const int coreHpBeforeFire = rangedTargetCore != nullptr ? rangedTargetCore->GetHealth() : -1;
    if (rangedTargetCore != nullptr)
    {
        HazardZone coreFire {};
        coreFire.id = NextHazardZoneId();
        coreFire.position = world_.GridToWorld(rangedTargetCore->GetBlockPosition());
        coreFire.ownerPlayerId = controlled.GetId();
        coreFire.ownerTeamId = controlled.GetTeamId();
        coreFire.radius = 2.4f;
        coreFire.lifetime = 1.0f;
        coreFire.tickTimer = 0.0f;
        coreFire.damagePerTick = 16;
        hazardZones_.push_back(coreFire);
        UpdateHazardZones(fixedDt);
    }
    const bool fireCoreImmune = rangedTargetCore != nullptr
        && rangedTargetCore->GetHealth() == coreHpBeforeFire;
    hazardZones_.clear();

    // Spawn invulnerability must absorb the impulse as well as the damage for
    // both direct projectiles and explosions.
    Player* protectedTarget = nullptr;
    for (Player& player : matchSimulation_.Players())
    {
        if (player.GetTeamId() != controlled.GetTeamId())
        {
            protectedTarget = &player;
            break;
        }
    }
    bool projectileSpawnProtection = false;
    bool explosionSpawnProtection = false;
    if (protectedTarget != nullptr)
    {
        protectedTarget->RespawnAtHome();
        protectedTarget->SetPosition(Vec3 { 240.0f, 70.0f, 240.0f });
        protectedTarget->SetVelocity(Vec3 {});
        const int protectedHp = protectedTarget->GetHealth();

        EnergyProjectile protectedProjectile {};
        protectedProjectile.id = NextProjectileId();
        protectedProjectile.ownerId = controlled.GetId();
        protectedProjectile.ownerTeamId = controlled.GetTeamId();
        protectedProjectile.kind = ProjectileKind::Blaster;
        protectedProjectile.position = Vector3 { 240.0f, 70.35f, 240.0f };
        protectedProjectile.previousPosition = protectedProjectile.position;
        protectedProjectile.velocity = Vector3 { 20.0f, 0.0f, 0.0f };
        protectedProjectile.damage = 40;
        protectedProjectile.radius = 0.22f;
        protectedProjectile.lifetime = 1.0f;
        projectiles_.push_back(protectedProjectile);
        UpdateProjectiles(fixedDt);
        const Vector3 velocityAfterProjectile = protectedTarget->GetVelocity();
        projectileSpawnProtection = protectedTarget->GetHealth() == protectedHp
            && std::fabs(velocityAfterProjectile.x) < 0.001f
            && std::fabs(velocityAfterProjectile.y) < 0.001f
            && std::fabs(velocityAfterProjectile.z) < 0.001f;

        protectedTarget->SetVelocity(Vec3 {});
        DetonateAt(protectedTarget->GetPosition(), controlled.GetTeamId(), controlled.GetId(),
            2.7f, 48, false);
        const Vector3 velocityAfterExplosion = protectedTarget->GetVelocity();
        explosionSpawnProtection = protectedTarget->GetHealth() == protectedHp
            && std::fabs(velocityAfterExplosion.x) < 0.001f
            && std::fabs(velocityAfterExplosion.y) < 0.001f
            && std::fabs(velocityAfterExplosion.z) < 0.001f;
    }
    projectiles_.clear();

    // TNT destroys ordinary defense but preserves the two reinforced materials.
    const GridPos tntObsidian { 280, 70, 280 };
    const GridPos tntGlass { 281, 70, 280 };
    const GridPos tntWool { 279, 70, 280 };
    world_.PlaceBlock(tntObsidian, Block { BlockType::ObsidianBlock, 1, true }, true);
    world_.PlaceBlock(tntGlass, Block { BlockType::EnergyGlassBlock, 1, true }, true);
    world_.PlaceBlock(tntWool, Block { BlockType::WoolBlock, 1, true }, true);
    DetonateAt(world_.GridToWorld(tntObsidian), controlled.GetTeamId(), controlled.GetId(),
        2.7f, 48, false, false, ExplosionBlockPolicy::PreserveReinforced);
    const Block* obsidianAfterTnt = world_.GetBlock(tntObsidian);
    const Block* glassAfterTnt = world_.GetBlock(tntGlass);
    const bool tntPreservesReinforced = obsidianAfterTnt != nullptr
        && obsidianAfterTnt->type == BlockType::ObsidianBlock
        && glassAfterTnt != nullptr
        && glassAfterTnt->type == BlockType::EnergyGlassBlock
        && world_.IsAir(tntWool);

    // Fireball is anti-bridge pressure, not a heavy breach tool: fortified
    // shop blocks survive it, while wool/wood remain vulnerable.
    const GridPos fireballObsidian { 300, 70, 300 };
    const GridPos fireballGlass { 301, 70, 300 };
    const GridPos fireballStone { 299, 70, 300 };
    const GridPos fireballSticky { 300, 70, 301 };
    const GridPos fireballWool { 300, 70, 299 };
    const GridPos fireballWood { 301, 70, 301 };
    world_.PlaceBlock(fireballObsidian, Block { BlockType::ObsidianBlock, 1, true }, true);
    world_.PlaceBlock(fireballGlass, Block { BlockType::EnergyGlassBlock, 1, true }, true);
    world_.PlaceBlock(fireballStone, Block { BlockType::StoneBlock, 1, true }, true);
    world_.PlaceBlock(fireballSticky, Block { BlockType::StickyBlock, 1, true }, true);
    world_.PlaceBlock(fireballWool, Block { BlockType::WoolBlock, 1, true }, true);
    world_.PlaceBlock(fireballWood, Block { BlockType::WoodBlock, 1, true }, true);
    DetonateAt(world_.GridToWorld(fireballObsidian), controlled.GetTeamId(), controlled.GetId(),
        kFireballTuning.explosionRadius, kFireballTuning.damage, false, false,
        ExplosionBlockPolicy::PreserveFortified);
    const bool fireballPreservesFortified =
        world_.GetBlock(fireballObsidian) != nullptr
        && world_.GetBlock(fireballGlass) != nullptr
        && world_.GetBlock(fireballStone) != nullptr
        && world_.GetBlock(fireballSticky) != nullptr
        && world_.IsAir(fireballWool)
        && world_.IsAir(fireballWood);

    // Collapse must be a live match rule even when automatch is disabled.
    automatch_.active = false;
    creativeMode_ = false;
    coreCollapseSeconds_ = fixedDt;
    UpdateMatchSimulation(fixedDt);
    const bool liveCoreCollapse = coreCollapseTriggered_
        && std::all_of(matchSimulation_.Cores().begin(), matchSimulation_.Cores().end(),
            [](const EnergyCore& core) { return !core.IsAlive(); })
        && std::all_of(teams_.begin(), teams_.end(),
            [this](const Team& team) { return !IsTeamActiveForMode(team.id) || !team.coreAlive; });

    const bool ok = noStartingBlocks
        && fireballSpawned && fireballAimOk
        && fireballPresentationClean && fireballResultReplicated
        && bowSpawned && bowAimOk && bowResultReplicated && bowHasStableId
        && blasterSpawned && blasterAimOk && blasterResultReplicated && blasterHasStableId
        && stableIdSurvivedExpiry && explosiveIdSurvivedExpiry && explosiveFallsWithoutSupport && hazardIdSurvivedExpiry
        && projectileCoreImmune && fireCoreImmune
        && projectileSpawnProtection && explosionSpawnProtection
        && tntPreservesReinforced && fireballPreservesFortified
        && liveCoreCollapse;
    std::cout << "network-ranged-smoke: fireball spawned="
              << (fireballSpawned ? "yes" : "no")
              << " aim=" << (fireballAimOk ? "ok" : "FAIL")
              << " presentation=" << (fireballPresentationClean ? "ok" : "FAIL")
              << " result=" << (fireballResultReplicated ? "ok" : "FAIL")
              << " bow spawned=" << (bowSpawned ? "yes" : "no")
              << " aim=" << (bowAimOk ? "ok" : "FAIL")
              << " result=" << (bowResultReplicated ? "ok" : "FAIL")
              << " stableId=" << (bowHasStableId ? "ok" : "FAIL")
              << " blaster spawned=" << (blasterSpawned ? "yes" : "no")
              << " aim=" << (blasterAimOk ? "ok" : "FAIL")
              << " result=" << (blasterResultReplicated ? "ok" : "FAIL")
              << " stableId=" << (blasterHasStableId ? "ok" : "FAIL")
              << " survivesExpiry=" << (stableIdSurvivedExpiry ? "ok" : "FAIL")
              << " explosiveSurvivesExpiry=" << (explosiveIdSurvivedExpiry ? "ok" : "FAIL")
              << " explosiveFalls=" << (explosiveFallsWithoutSupport ? "ok" : "FAIL")
              << " hazardSurvivesExpiry=" << (hazardIdSurvivedExpiry ? "ok" : "FAIL")
              << " noStartingBlocks=" << (noStartingBlocks ? "ok" : "FAIL")
              << " projectileCoreImmune=" << (projectileCoreImmune ? "ok" : "FAIL")
              << " fireCoreImmune=" << (fireCoreImmune ? "ok" : "FAIL")
              << " projectileSpawnProtection=" << (projectileSpawnProtection ? "ok" : "FAIL")
              << " explosionSpawnProtection=" << (explosionSpawnProtection ? "ok" : "FAIL")
              << " tntPreservesReinforced=" << (tntPreservesReinforced ? "ok" : "FAIL")
              << " fireballPreservesFortified=" << (fireballPreservesFortified ? "ok" : "FAIL")
              << " liveCoreCollapse=" << (liveCoreCollapse ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "NETWORK_RANGED_SMOKE_OK" : "NETWORK_RANGED_SMOKE_FAIL")
              << std::endl;
    return ok ? 0 : 9;
}

int Game::RunNetworkActionsSmoke()
{
    // B1 headless integration test: a network-controlled player's attack / break /
    // place must mutate authoritative server state via ApplyNetworkPlayerActions.
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();

    if (matchSimulation_.Players().size() < 2)
    {
        std::cout << "network-actions-smoke: not enough players\n"
                     "NETWORK_ACTIONS_SMOKE_FAIL" << std::endl;
        return 9;
    }

    // Use a NON-LOCAL player as the network-controlled subject, mirroring a real
    // dedicated server (a client claims a bot slot; the host's own local player is
    // retired). This matters for held-item lookup: GetSelectedHotbarStack reads
    // the global UI slot (selectedHotbarSlot_) for a LOCAL player but the
    // per-player slot for a network player — reusing the local player here would
    // read slot 0 regardless of SetSelectedSlot(), so melee would never see the
    // sword and would silently no-op.
    Player* controlledPtr = nullptr;
    for (Player& candidate : matchSimulation_.Players())
    {
        if (IsBotControlled(ControlKindForPlayer(candidate)))
        {
            controlledPtr = &candidate;
            break;
        }
    }
    if (controlledPtr == nullptr)
    {
        std::cout << "network-actions-smoke: no non-local player\n"
                     "NETWORK_ACTIONS_SMOKE_FAIL" << std::endl;
        return 9;
    }
    Player& controlled = *controlledPtr;
    const int controlledId = controlled.GetId();
    MarkNetworkControlledPlayer(controlledId);

    Player* enemy = nullptr;
    for (Player& candidate : matchSimulation_.Players())
    {
        if (candidate.GetTeamId() != controlled.GetTeamId())
        {
            enemy = &candidate;
            break;
        }
    }
    if (enemy == nullptr)
    {
        std::cout << "network-actions-smoke: no enemy player\n"
                     "NETWORK_ACTIONS_SMOKE_FAIL" << std::endl;
        return 9;
    }

    // A clear patch of air well above the arena so existing geometry can't
    // interfere with the raycasts.
    controlled.SetPosition(Vec3 { 0.0f, 40.0f, 0.0f });
    controlled.SetHeroId(HeroId::Likho);
    const float aimYaw = PI / 2.0f; // Forward() = (+1, 0, 0)
    controlled.SetYaw(aimYaw);

    PlayerCommand base;
    base.controlledPlayerId = static_cast<std::uint32_t>(controlledId);
    base.aimYaw = aimYaw;
    base.aimPitch = 0.0f;
    const Vector3 forward = AimDirectionFromCommand(base);
    const Vector3 eye { 0.0f, 40.0f + 0.78f, 0.0f };

    const Team* controlledTeamForChest = FindTeam(controlled.GetTeamId());
    const Block* controlledChestBlock = controlledTeamForChest != nullptr
        ? world_.GetBlock(controlledTeamForChest->teamChestBlock)
        : nullptr;
    const bool bromChestBlockWorked = controlledTeamForChest != nullptr
        && controlledChestBlock != nullptr
        && controlledChestBlock->type == BlockType::TeamChestBlock
        && controlledChestBlock->teamId == controlled.GetTeamId();
    bool bromChestDeliveryWorked = false;
    if (bromChestBlockWorked)
    {
        const int teamId = controlled.GetTeamId();
        const int ironBeforeChestDelivery = teamChests_[teamId].GetResource(ResourceType::Iron);
        BromVacuumBot deliveryBot {};
        deliveryBot.ownerPlayerId = controlledId;
        deliveryBot.ownerTeamId = teamId;
        deliveryBot.position = TeamChestDepositPosition(*controlledTeamForChest);
        deliveryBot.lastPosition = deliveryBot.position;
        deliveryBot.returning = true;
        deliveryBot.health = 40;
        deliveryBot.pulseTimer = 1.0f;
        deliveryBot.cargo[0] = 3;
        bromVacuumBots_.push_back(deliveryBot);
        {
            ScopedLocalFeedbackSuppression suppressDeliveryFeedback(*this, true);
            UpdateBromDevices(matchSimulation_.FixedDeltaSeconds());
        }
        bromChestDeliveryWorked =
            teamChests_[teamId].GetResource(ResourceType::Iron) == ironBeforeChestDelivery + 3;
        bromVacuumBots_.clear();
    }

    // Phase 2 bridge: authoritative remote-human actions must mutate gameplay
    // without writing host-local presentation buffers. Force feedback on in this
    // headless smoke so the scoped server suppression is what keeps these stable.
    suppressLocalFeedback_ = false;
    const std::string messageBefore = message_;
    const std::size_t eventMessagesBefore = eventMessages_.size();
    const std::size_t worldEffectsBefore = worldEffects_.size();
    const std::size_t floatingTextsBefore = floatingTexts_.size();
    const bool audioMutedBefore = audio_.IsMuted();
    const int localPlayerIdBefore = localPlayerId_;
    localPlayerId_ = controlledId;
    const MatchStats statsBefore = stats_;
    const float hitMarkerBefore = hitMarkerTimer_;
    const float damageFlashBefore = damageFlashTimer_;
    const float fovKickBefore = fovKick_;

    // --- Hero abilities: remote actions mutate authoritative state only. ------
    controlled.SetHeroId(HeroId::Radon);
    enemy->SetPosition(Vec3 { eye.x + forward.x * 3.0f, 40.0f, eye.z + forward.z * 3.0f });
    enemy->UpdateTimers(2.0f);
    const int radonPulseHpBefore = enemy->GetHealth();
    PlayerCommand radonPulseCmd = base;
    radonPulseCmd.useAbility1 = true;
    recentActionResults_.clear();
    const bool radonPulseApplied = ApplyPlayerActionCommand(controlled, radonPulseCmd);
    const bool radonPulseWorked = radonPulseApplied && enemy->GetHealth() < radonPulseHpBefore;
    const MatchSnapshot radonPulseOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool radonPulseResultReplicated = std::any_of(
        radonPulseOwnerView.actionResults.begin(),
        radonPulseOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            // Radon's Active1 always pushes a directed Pull/Cone world effect
            // (radius 5.2/5.6) into HeroAbilityActionResult::worldEffects — so a
            // successful cast must replicate a real radius + the world-effect
            // and directed flags (PushHeroAbilityActionResultSnapshot's primary-
            // effect extraction), not just message+sound.
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::HeroAbility)
                && result.subjectType == static_cast<int>(HeroId::Radon)
                && result.amount == static_cast<int>(HeroAbilitySlot::Active1)
                && result.success
                && result.radius > 0.0f
                && (result.flags & kHeroAbilityFlagWorldEffect) != 0
                && (result.flags & kHeroAbilityFlagDirectedEffect) != 0;
        });

    // Immediately re-cast on the same command while the ability is on cooldown:
    // the server denies it, and a real network owner must learn "denied" the
    // same replicated way it learns "success" — not by inference from silence.
    recentActionResults_.clear();
    const bool radonPulseDeniedApplied = ApplyPlayerActionCommand(controlled, radonPulseCmd);
    const MatchSnapshot radonPulseDeniedView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool radonPulseDeniedReplicated = std::any_of(
        radonPulseDeniedView.actionResults.begin(),
        radonPulseDeniedView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::HeroAbility)
                && result.subjectType == static_cast<int>(HeroId::Radon)
                && result.amount == static_cast<int>(HeroAbilitySlot::Active1)
                && !result.success
                && !result.message.empty();
        });
    const bool heroAbilityResultsReplicated = radonPulseResultReplicated
        && !radonPulseDeniedApplied && radonPulseDeniedReplicated;

    const std::size_t radonProjectilesBefore = projectiles_.size();
    PlayerCommand radonMolotovCmd = base;
    radonMolotovCmd.useAbility2 = true;
    const bool radonMolotovApplied = ApplyPlayerActionCommand(controlled, radonMolotovCmd);
    const bool radonMolotovWorked = radonMolotovApplied && projectiles_.size() == radonProjectilesBefore + 1;

    controlled.SetHeroUltimateCharge(100.0f);
    const bool radonPrimedBefore = controlled.GetHeroState().ultimatePrimed;
    PlayerCommand radonPrimeCmd = base;
    radonPrimeCmd.useUltimate = true;
    const bool radonPrimeApplied = ApplyPlayerActionCommand(controlled, radonPrimeCmd);
    const bool radonPrimeWorked = radonPrimeApplied
        && controlled.GetHeroState().ultimatePrimed != radonPrimedBefore;

    EnergyCore* controlledCore = FindCoreByTeam(controlled.GetTeamId());
    if (controlledCore != nullptr)
    {
        controlledCore->SetHealth(0);
    }
    if (Team* controlledTeam = FindTeam(controlled.GetTeamId()))
    {
        controlledTeam->coreAlive = false;
    }
    controlled.SetHeroUltimateCharge(100.0f);
    enemy->SetPosition(Vec3 { eye.x + forward.x * 2.8f, 40.0f, eye.z + forward.z * 2.8f });
    enemy->UpdateTimers(2.0f);
    const int radonWaveHpBefore = enemy->GetHealth();
    const float radonUltimateCooldownBefore = controlled.GetHeroState().ultimate.cooldownRemaining;
    PlayerCommand radonWaveCmd = base;
    radonWaveCmd.useUltimate = true;
    const bool radonWaveApplied = ApplyPlayerActionCommand(controlled, radonWaveCmd);
    const bool radonWaveWorked = radonWaveApplied
        && controlled.GetHeroState().ultimate.cooldownRemaining > radonUltimateCooldownBefore
        && enemy->GetHealth() < radonWaveHpBefore;

    controlled.SetHeroId(HeroId::Likho);
    const float ability1CooldownBefore = controlled.GetHeroState().active1.cooldownRemaining;
    PlayerCommand hero1Cmd = base;
    hero1Cmd.useAbility1 = true;
    const bool hero1Applied = ApplyPlayerActionCommand(controlled, hero1Cmd);
    const float ability1CooldownAfter = controlled.GetHeroState().active1.cooldownRemaining;
    const bool hero1Worked = hero1Applied
        && ability1CooldownBefore <= 0.0f
        && ability1CooldownAfter > ability1CooldownBefore;

    const bool ability2ActiveBefore = controlled.GetHeroState().active2.active;
    PlayerCommand hero2Cmd = base;
    hero2Cmd.useAbility2 = true;
    const bool hero2Applied = ApplyPlayerActionCommand(controlled, hero2Cmd);
    const bool ability2ActiveAfter = controlled.GetHeroState().active2.active;
    const bool hero2Worked = hero2Applied && !ability2ActiveBefore && ability2ActiveAfter;

    // Same pitch-loss bug as Svidetel's Active2 below, for Likho's ultimate
    // (disguise target selection): position the enemy above eye level so a
    // flat aim falls outside the targeting cone but the command's real 3D aim
    // (from aimPitch) doesn't — regression case for the fix.
    constexpr float kLikhoUltimateRise = 1.5f;
    constexpr float kLikhoUltimateRun = 3.0f;
    // HeroRuntimeState's ability slots (and ultimateReady/ultimateCharge) are
    // hero-agnostic (not keyed by hero id), so Radon's ultimate cast earlier
    // (radonWaveCmd, 40s cooldown, and it consumes ultimateReady) is still
    // sitting on the SAME slot after SetHeroId(Likho) above — reset both or
    // Likho's IsHeroAbilityReady(Ultimate) denies this cast outright.
    controlled.MutableHeroState().ultimate.cooldownRemaining = 0.0f;
    controlled.SetHeroUltimateCharge(100.0f);
    enemy->SetPosition(Vec3 {
        eye.x + forward.x * kLikhoUltimateRun,
        controlled.GetPosition().y + 0.72f + kLikhoUltimateRise,
        eye.z + forward.z * kLikhoUltimateRun });
    enemy->UpdateTimers(2.0f);
    PlayerCommand likhoUltimateCmd = base;
    likhoUltimateCmd.useUltimate = true;
    likhoUltimateCmd.aimPitch = std::atan2(kLikhoUltimateRise, kLikhoUltimateRun);
    const bool likhoUltimateApplied = ApplyPlayerActionCommand(controlled, likhoUltimateCmd);
    const bool likhoUltimateWorked = likhoUltimateApplied
        && controlled.GetHeroState().likhoDisguiseTeamId == enemy->GetTeamId();

    controlled.SetHeroId(HeroId::Svidetel);
    const std::size_t echoesBefore = svidetelEchoes_.size();
    PlayerCommand svidetelEchoCmd = base;
    svidetelEchoCmd.useAbility1 = true;
    const bool svidetelEchoApplied = ApplyPlayerActionCommand(controlled, svidetelEchoCmd);
    const bool svidetelEchoWorked = svidetelEchoApplied && svidetelEchoes_.size() == echoesBefore + 1;

    // Elevated on purpose (not flat, unlike `base.aimPitch = 0.0f`): this is the
    // regression case for a real bug — ApplyHeroAbilityAction used to fall back
    // to player.Forward() (yaw-only, always flat) for a network player's aim
    // here, so a remote Svidetel could never phase a block above eye level no
    // matter what aimPitch the command carried. A flat-aim test can't catch
    // that (player.Forward() and a pitch=0 command agree), so this one aims up.
    constexpr float kPhaseRise = 1.5f;
    constexpr float kPhaseRun = 3.0f;
    const float phaseOriginY = controlled.GetPosition().y + 0.72f;
    const GridPos phaseCell = world_.WorldToGrid(Vector3 {
        eye.x + forward.x * kPhaseRun, phaseOriginY + kPhaseRise, eye.z + forward.z * kPhaseRun });
    world_.PlaceBlock(phaseCell, Block { BlockType::StoneBlock, -1, true }, true);
    const std::size_t phaseBlocksBefore = svidetelPhaseBlocks_.size();
    PlayerCommand svidetelPhaseCmd = base;
    svidetelPhaseCmd.useAbility2 = true;
    svidetelPhaseCmd.aimPitch = std::atan2(kPhaseRise, kPhaseRun);
    const bool svidetelPhaseApplied = ApplyPlayerActionCommand(controlled, svidetelPhaseCmd);
    const bool svidetelPhaseWorked = svidetelPhaseApplied
        && svidetelPhaseBlocks_.size() > phaseBlocksBefore
        && world_.GetBlock(phaseCell) == nullptr;

    controlled.SetHeroUltimateCharge(100.0f);
    const float svidetelUltimateCooldownBefore = controlled.GetHeroState().ultimate.cooldownRemaining;
    PlayerCommand svidetelUltimateCmd = base;
    svidetelUltimateCmd.useUltimate = true;
    const bool svidetelUltimateApplied = ApplyPlayerActionCommand(controlled, svidetelUltimateCmd);
    const float svidetelUltimateCooldownAfter = controlled.GetHeroState().ultimate.cooldownRemaining;
    const bool svidetelUltimateWorked = svidetelUltimateApplied
        && svidetelUltimateCooldownBefore <= 0.0f
        && svidetelUltimateCooldownAfter > svidetelUltimateCooldownBefore;

    controlled.SetHeroId(HeroId::Orbita);
    PlayerCommand orbitaDashCmd = base;
    orbitaDashCmd.useAbility1 = true;
    const bool orbitaDashApplied = ApplyPlayerActionCommand(controlled, orbitaDashCmd);
    const HeroRuntimeState& orbitaDashState = controlled.GetHeroState();
    const bool orbitaDashWorked = orbitaDashApplied
        && orbitaDashState.orbitaMomentumStrike
        && orbitaDashState.orbitaDashRemaining > 0.0f
        && orbitaDashState.active1.cooldownRemaining > 0.0f;

    const std::size_t orbitaTempBlocksBefore = heroTemporaryBlocks_.size();
    PlayerCommand orbitaBlocksCmd = base;
    orbitaBlocksCmd.useAbility2 = true;
    const bool orbitaBlocksApplied = ApplyPlayerActionCommand(controlled, orbitaBlocksCmd);
    const bool orbitaBlocksWorked = orbitaBlocksApplied
        && heroTemporaryBlocks_.size() > orbitaTempBlocksBefore
        && controlled.GetHeroState().active2.cooldownRemaining > 0.0f;

    controlled.SetHeroUltimateCharge(100.0f);
    PlayerCommand orbitaPrimeCmd = base;
    orbitaPrimeCmd.useUltimate = true;
    const bool orbitaPrimeApplied = ApplyPlayerActionCommand(controlled, orbitaPrimeCmd);
    const bool orbitaPrimeWorked = orbitaPrimeApplied
        && controlled.GetHeroState().orbitaTeleportPrimed
        && controlled.GetHeroState().orbitaTeleportPreviewTimer > 0.0f;

    const Vec3 orbitaPositionBeforeTeleport = controlled.GetPositionVec3();
    PlayerCommand orbitaTeleportCmd = base;
    orbitaTeleportCmd.useUltimate = true;
    const bool orbitaTeleportApplied = ApplyPlayerActionCommand(controlled, orbitaTeleportCmd);
    const float orbitaTeleportMove = (controlled.GetPositionVec3() - orbitaPositionBeforeTeleport).Length();
    const bool orbitaTeleportPrimedAfter = controlled.GetHeroState().orbitaTeleportPrimed;
    const float orbitaTeleportCooldownAfter = controlled.GetHeroState().ultimate.cooldownRemaining;
    const bool orbitaTeleportUltimateReadyAfter = controlled.GetHeroState().ultimateReady;
    const bool orbitaTeleportWorked = orbitaTeleportApplied
        && !orbitaTeleportPrimedAfter
        && !orbitaTeleportUltimateReadyAfter
        && orbitaTeleportMove > 1.0f;
    controlled.SetPosition(Vec3 { eye.x, 40.0f, eye.z });
    controlled.SetYaw(aimYaw);

    controlled.SetHeroId(HeroId::Konvoy);
    const Vector3 trapDesired {
        controlled.GetPosition().x + forward.x * 1.15f,
        controlled.GetPosition().y,
        controlled.GetPosition().z + forward.z * 1.15f
    };
    const GridPos trapColumn = world_.WorldToGrid(trapDesired);
    const int playerGridY = world_.WorldToGrid(controlled.GetPosition()).y;
    world_.PlaceBlock(GridPos { trapColumn.x, playerGridY - 1, trapColumn.z }, Block { BlockType::StoneBlock, -1, true }, true);
    const std::size_t konvoyTrapsBefore = konvoyTraps_.size();
    PlayerCommand konvoyTrapCmd = base;
    konvoyTrapCmd.useAbility1 = true;
    const bool konvoyTrapApplied = ApplyPlayerActionCommand(controlled, konvoyTrapCmd);
    const bool konvoyTrapWorked = konvoyTrapApplied && konvoyTraps_.size() == konvoyTrapsBefore + 1;

    enemy->SetPosition(Vec3 { eye.x + forward.x * 3.0f, 40.0f, eye.z + forward.z * 3.0f });
    const std::size_t konvoyTethersBefore = konvoyTethers_.size();
    PlayerCommand konvoyTetherCmd = base;
    konvoyTetherCmd.useAbility2 = true;
    const bool konvoyTetherApplied = ApplyPlayerActionCommand(controlled, konvoyTetherCmd);
    const bool konvoyTetherWorked = konvoyTetherApplied && konvoyTethers_.size() == konvoyTethersBefore + 1;

    controlled.SetHeroUltimateCharge(100.0f);
    const std::size_t konvoyDomesBefore = konvoyDomes_.size();
    PlayerCommand konvoyDomeCmd = base;
    konvoyDomeCmd.useUltimate = true;
    const bool konvoyDomeApplied = ApplyPlayerActionCommand(controlled, konvoyDomeCmd);
    const bool konvoyDomeWorked = konvoyDomeApplied && konvoyDomes_.size() == konvoyDomesBefore + 1;

    controlled.SetHeroId(HeroId::Brom);
    controlled.GetInventory().AddResource(ResourceType::Iron, 48);
    controlled.GetInventory().AddResource(ResourceType::Gold, 12);
    const std::size_t bromVacuumBefore = bromVacuumBots_.size();
    PlayerCommand bromVacuumCmd = base;
    bromVacuumCmd.useAbility1 = true;
    const bool bromVacuumApplied = ApplyPlayerActionCommand(controlled, bromVacuumCmd);
    const bool bromVacuumWorked = bromVacuumApplied && bromVacuumBots_.size() == bromVacuumBefore + 1;

    const std::size_t bromTurretBefore = bromTurretDrones_.size();
    PlayerCommand bromTurretCmd = base;
    bromTurretCmd.useAbility2 = true;
    const bool bromTurretApplied = ApplyPlayerActionCommand(controlled, bromTurretCmd);
    const bool bromTurretWorked = bromTurretApplied && bromTurretDrones_.size() == bromTurretBefore + 1;

    controlled.GetInventory().AddResource(ResourceType::Gold, 60);
    controlled.SetHeroUltimateCharge(100.0f);
    const std::size_t bromUltimateTurretsBefore = bromTurretDrones_.size();
    PlayerCommand bromUltimateCmd = base;
    bromUltimateCmd.useUltimate = true;
    const bool bromUltimateApplied = ApplyPlayerActionCommand(controlled, bromUltimateCmd);
    const bool bromUltimateWorked = bromUltimateApplied
        && bromTurretDrones_.size() > bromUltimateTurretsBefore
        && bromTurretDrones_.back().temporary
        && controlled.GetHeroState().ultimate.cooldownRemaining > 0.0f;

    // Stable spawn ids for hero devices (the 7 structs sharing one
    // HeroDeviceSnapshot id-space — see NetworkSnapshot.h): a real ability cast
    // must assign a real, non-default id, and removing an EARLIER device from
    // its vector must not reassign a SURVIVING device's id (the bug the fix
    // closes — this used to be a per-BuildNetworkSnapshot-call local counter).
    const bool bromDevicesHaveStableIds = bromVacuumWorked && bromTurretWorked
        && bromVacuumBots_.back().id > 0
        && bromTurretDrones_.back().id > 0
        && bromVacuumBots_.back().id != bromTurretDrones_.back().id;
    const int survivorTurretIdBeforeErase = bromTurretWorked ? bromTurretDrones_.back().id : -1;
    if (!bromVacuumBots_.empty())
    {
        bromVacuumBots_.erase(bromVacuumBots_.begin());
    }
    const bool heroDeviceIdSurvivedErase = bromTurretWorked
        && !bromTurretDrones_.empty()
        && bromTurretDrones_.back().id == survivorTurretIdBeforeErase;

    const bool heroAbilityWorked = radonPulseWorked && radonMolotovWorked
        && radonPrimeWorked && radonWaveWorked
        && heroAbilityResultsReplicated
        && hero1Worked && hero2Worked && likhoUltimateWorked
        && svidetelEchoWorked && svidetelPhaseWorked && svidetelUltimateWorked
        && orbitaDashWorked && orbitaBlocksWorked && orbitaPrimeWorked && orbitaTeleportWorked
        && konvoyTrapWorked && konvoyTetherWorked && konvoyDomeWorked
        && bromVacuumWorked && bromTurretWorked && bromUltimateWorked
        && bromDevicesHaveStableIds && heroDeviceIdSurvivedErase;
    if (!heroAbilityWorked)
    {
        std::cout << "network-actions-smoke hero-detail:"
                  << " radonPulse=" << (radonPulseWorked ? "ok" : "FAIL")
                  << " radonPulseResults=" << (heroAbilityResultsReplicated ? "ok" : "FAIL")
                  << " radonMolotov=" << (radonMolotovWorked ? "ok" : "FAIL")
                  << " radonPrime=" << (radonPrimeWorked ? "ok" : "FAIL")
                  << " radonWave=" << (radonWaveWorked ? "ok" : "FAIL")
                  << " likho1=" << (hero1Worked ? "ok" : "FAIL")
                  << " likho2=" << (hero2Worked ? "ok" : "FAIL")
                  << " likhoUlt=" << (likhoUltimateWorked ? "ok" : "FAIL")
                  << " svidetel1=" << (svidetelEchoWorked ? "ok" : "FAIL")
                  << " svidetel2=" << (svidetelPhaseWorked ? "ok" : "FAIL")
                  << " svidetelUlt=" << (svidetelUltimateWorked ? "ok" : "FAIL")
                  << " orbita1=" << (orbitaDashWorked ? "ok" : "FAIL")
                  << " orbita2=" << (orbitaBlocksWorked ? "ok" : "FAIL")
                  << " orbitaPrime=" << (orbitaPrimeWorked ? "ok" : "FAIL")
                  << " orbitaTeleport=" << (orbitaTeleportWorked ? "ok" : "FAIL")
                  << "(applied=" << (orbitaTeleportApplied ? "yes" : "no")
                  << ",primed=" << (orbitaTeleportPrimedAfter ? "yes" : "no")
                  << ",cd=" << orbitaTeleportCooldownAfter
                  << ",ready=" << (orbitaTeleportUltimateReadyAfter ? "yes" : "no")
                  << ",move=" << orbitaTeleportMove << ")"
                  << " konvoy1=" << (konvoyTrapWorked ? "ok" : "FAIL")
                  << " konvoy2=" << (konvoyTetherWorked ? "ok" : "FAIL")
                  << " konvoyUlt=" << (konvoyDomeWorked ? "ok" : "FAIL")
                  << " brom1=" << (bromVacuumWorked ? "ok" : "FAIL")
                  << " brom2=" << (bromTurretWorked ? "ok" : "FAIL")
                  << " bromUlt=" << (bromUltimateWorked ? "ok" : "FAIL")
                  << " deviceIds=" << (bromDevicesHaveStableIds ? "ok" : "FAIL")
                  << " deviceIdSurvivesErase=" << (heroDeviceIdSurvivedErase ? "ok" : "FAIL") << '\n';
    }

    // --- Utility: a remote dash mutates authoritative movement only. ---
    recentActionResults_.clear();
    ItemStack dashPearl;
    dashPearl.type = ItemFromUtility(UtilityType::Dash);
    dashPearl.count = 1;
    controlled.GetInventory().SwapSlot(2, dashPearl);
    controlled.SetSelectedSlot(2);
    const Vec3 velocityBeforeDash = controlled.GetVelocityVec3();
    PlayerCommand dashCmd = base;
    dashCmd.useDash = true;
    UseUtilityInputs(controlled, dashCmd);
    const Vec3 velocityAfterDash = controlled.GetVelocityVec3();
    const bool dashWorked = (velocityAfterDash - velocityBeforeDash).Length() > 0.1f
        && controlled.GetInventory().GetHotbarSlots()[2].IsEmpty();
    const MatchSnapshot dashOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlledId);
    const bool dashResultReplicated = std::any_of(
        dashOwnerView.actionResults.begin(),
        dashOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::UtilityUse)
                && result.subjectType == static_cast<int>(UtilityType::Dash)
                && result.success;
        });

    // --- Melee: an enemy lined up in front loses health on attackPressed. ---
    enemy->SetPosition(Vec3 { eye.x + forward.x * 1.6f, 40.0f, eye.z + forward.z * 1.6f });
    // Players spawn with ~1.65s anti-spawn-kill invulnerability (RespawnAtHome),
    // which correctly makes an enemy an invalid melee target on tick 0. A real
    // match melees AFTER it expires, so advance the target's timers past it —
    // otherwise this tests spawn protection, not the network melee path.
    enemy->UpdateTimers(2.0f);
    ItemStack sword;
    sword.type = ItemType::Sword;
    sword.count = 1;
    controlled.GetInventory().SwapSlot(1, sword);
    controlled.SetSelectedSlot(1);
    recentActionResults_.clear();
    const int enemyHpBefore = enemy->GetHealth();
    PlayerCommand attackCmd = base;
    attackCmd.attackPressed = true;
    ApplyNetworkPlayerActions(controlled, attackCmd, 0.05f);
    const int enemyHpAfter = enemy->GetHealth();
    const bool meleeWorked = enemyHpAfter < enemyHpBefore;
    const MatchSnapshot combatAttackerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlled.GetId());
    const MatchSnapshot combatTargetView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), enemy->GetId());
    const bool combatAttackerResultReplicated = std::any_of(
        combatAttackerView.actionResults.begin(),
        combatAttackerView.actionResults.end(),
        [controlledId, enemy](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::CombatEvent)
                && result.actorPlayerId == controlledId
                && result.targetPlayerId == enemy->GetId()
                && result.amount > 0
                && (result.flags & kCombatFlagRecipientAttacker) != 0;
        });
    const bool combatTargetResultReplicated = std::any_of(
        combatTargetView.actionResults.begin(),
        combatTargetView.actionResults.end(),
        [controlledId, enemy](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::CombatEvent)
                && result.actorPlayerId == controlledId
                && result.targetPlayerId == enemy->GetId()
                && result.amount > 0
                && (result.flags & kCombatFlagRecipientTarget) != 0;
        });
    const bool combatResultsReplicated = combatAttackerResultReplicated && combatTargetResultReplicated;

    // Park the enemy far away so it can't interfere with the build tests.
    enemy->SetPosition(Vec3 { 100.0f, 40.0f, 100.0f });

    // --- Place: with a block selected and an anchor in front, a block appears. ---
    recentActionResults_.clear();
    ItemStack stone;
    stone.type = ItemFromBlock(BlockType::StoneBlock);
    stone.count = 32;
    controlled.GetInventory().SwapSlot(0, stone);
    controlled.SetSelectedSlot(0);
    const GridPos anchorCell = world_.WorldToGrid(Vector3 {
        eye.x + forward.x * 2.0f, eye.y, eye.z + forward.z * 2.0f });
    world_.PlaceBlock(anchorCell, Block { BlockType::StoneBlock, -1, true }, true);
    const std::size_t blocksBeforePlace = world_.GetBlocks().size();
    PlayerCommand placeCmd = base;
    placeCmd.placeHeld = true;
    ApplyNetworkPlayerActions(controlled, placeCmd, 0.5f);
    const std::size_t blocksAfterPlace = world_.GetBlocks().size();
    const bool placeWorked = blocksAfterPlace == blocksBeforePlace + 1;

    // --- Break: holding attack on a block in front removes it. ---
    PlayerCommand breakCmd = base;
    breakCmd.attackHeld = true;
    bool breakWorked = false;
    bool breakProgressVisible = false;
    const std::size_t droppedItemsBeforeBreak = matchSimulation_.DroppedItems().size();
    for (int i = 0; i < 600 && !breakWorked; ++i)
    {
        ApplyNetworkPlayerActions(controlled, breakCmd, 0.05f);
        breakProgressVisible = breakProgressVisible || networkActionState_[controlled.GetId()].breakProgress.visible;
        if (world_.GetBlocks().size() < blocksAfterPlace)
        {
            breakWorked = true;
        }
    }
    const bool blockDropWorked = matchSimulation_.DroppedItems().size() == droppedItemsBeforeBreak + 1
        && matchSimulation_.DroppedItems().back().stack.type == ItemFromBlock(BlockType::StoneBlock)
        && matchSimulation_.DroppedItems().back().stack.count == 1
        && matchSimulation_.DroppedItems().back().id > 0
        && LengthVec3(matchSimulation_.DroppedItems().back().velocity) > 0.01f;

    Player localBreakUi(9901, "local-break-ui", controlled.GetTeamId(), Vector3 { 0.0f, 40.0f, 2.0f }, true);
    localBreakUi.SetControlKind(PlayerControlKind::LocalHumanPredicted);
    localBreakUi.SetYaw(aimYaw);
    selectedHotbarSlot_ = 0;
    const Vector3 localEye { localBreakUi.GetPosition().x, localBreakUi.GetPosition().y + 0.72f, localBreakUi.GetPosition().z };
    const GridPos localBreakCell = world_.WorldToGrid(Vector3 {
        localEye.x + forward.x * 2.0f,
        localEye.y,
        localEye.z + forward.z * 2.0f });
    world_.PlaceBlock(localBreakCell, Block { BlockType::StoneBlock, -1, true }, true);
    PlayerCommand localBreakCmd = base;
    localBreakCmd.controlledPlayerId = static_cast<std::uint32_t>(localBreakUi.GetId());
    localBreakCmd.attackHeld = true;
    UpdatePredictedBreakProgress(localBreakUi, localBreakCmd, 0.05f);
    const bool predictedBreakUiProgress = breakProgress_.visible
        && breakProgress_.target == localBreakCell
        && breakProgress_.fraction > 0.0f;
    ResetBreakProgress();
    ApplyNetworkPlayerActions(localBreakUi, localBreakCmd, 0.05f);
    const bool localBreakUiProgress = breakProgress_.visible
        && breakProgress_.target == localBreakCell
        && breakProgress_.fraction > 0.0f;
    ResetBreakProgress();
    networkActionState_.erase(localBreakUi.GetId());
    world_.RemoveBlock(localBreakCell);

    const MatchSnapshot blockResultOwnerView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlled.GetId());
    const MatchSnapshot blockResultEnemyView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), enemy->GetId());
    const bool blockPlaceResultReplicated = std::any_of(
        blockResultOwnerView.actionResults.begin(),
        blockResultOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::BlockPlace)
                && result.success
                && result.subjectType == static_cast<int>(BlockType::StoneBlock);
        });
    const bool blockBreakResultReplicated = std::any_of(
        blockResultOwnerView.actionResults.begin(),
        blockResultOwnerView.actionResults.end(),
        [](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::BlockBreak)
                && result.success;
        });
    const bool blockResultSeqMonotonic =
        blockResultOwnerView.actionResults.size() >= 2
        && blockResultOwnerView.actionResults[0].resultSeq < blockResultOwnerView.actionResults[1].resultSeq;
    const bool blockResultsReplicated = blockPlaceResultReplicated
        && blockBreakResultReplicated
        && blockResultSeqMonotonic
        && blockResultEnemyView.actionResults.empty();

    // --- Hero-device damage (MP parity with UpdateAttackOrBreak): an enemy
    // Brom turret on the aim segment must lose health on attackPressed. This
    // path had no server-side counterpart, so a network player could not
    // destroy enemy devices at all. ---
    bromTurretDrones_.clear();
    BromTurretDrone enemyTurret {};
    enemyTurret.ownerTeamId = enemy->GetTeamId();
    enemyTurret.ownerPlayerId = enemy->GetId();
    enemyTurret.position = Vector3 { eye.x + forward.x * 1.5f, eye.y, eye.z + forward.z * 1.5f };
    enemyTurret.health = 40;
    enemyTurret.invulnerabilityTimer = 0.0f;
    bromTurretDrones_.push_back(enemyTurret);
    const int turretHpBeforeAttack = bromTurretDrones_.back().health;
    PlayerCommand deviceAttackCmd = base;
    deviceAttackCmd.attackPressed = true;
    ApplyNetworkPlayerActions(controlled, deviceAttackCmd, 0.05f);
    const bool heroDeviceDamageWorked = !bromTurretDrones_.empty()
        && bromTurretDrones_.back().health < turretHpBeforeAttack;
    bromTurretDrones_.clear();

    // --- Likho mining modifiers (MP parity): with active2 up, mining a block
    // registers a persistent cut server-side (was local-path only, so a network
    // Likho got neither the cut nor its 0.72x break-speed bonus). ---
    controlled.SetHeroId(HeroId::Likho);
    controlled.MutableHeroState().active2.active = true;
    likhoBlockCuts_.clear();
    const GridPos likhoCell = world_.WorldToGrid(Vector3 {
        eye.x + forward.x * 2.0f, eye.y, eye.z });
    world_.PlaceBlock(likhoCell, Block { BlockType::StoneBlock, -1, true }, true);
    PlayerCommand likhoMineCmd = base;
    likhoMineCmd.attackHeld = true;
    ApplyNetworkPlayerActions(controlled, likhoMineCmd, 0.01f);
    const bool likhoCutRegistered = std::any_of(
        likhoBlockCuts_.begin(), likhoBlockCuts_.end(),
        [&controlled, &likhoCell](const LikhoBlockCut& cut)
        {
            return cut.ownerPlayerId == controlled.GetId() && cut.position == likhoCell;
        });
    likhoBlockCuts_.clear();

    // --- Server tick presentation guard: authoritative world updates may hit
    // players and spawn visual/audio cues, but a network server tick must not
    // write host-local presentation.
    enemy->SetPosition(Vec3 { 3.0f, 40.0f, 0.0f });
    enemy->UpdateTimers(2.0f);
    const int serverTickHpBefore = enemy->GetHealth();
    EnergyProjectile serverTickProjectile {};
    serverTickProjectile.kind = ProjectileKind::Blaster;
    serverTickProjectile.position = Vector3 { 2.6f, 40.35f, 0.0f };
    serverTickProjectile.previousPosition = Vector3 { 1.2f, 40.35f, 0.0f };
    serverTickProjectile.startPosition = serverTickProjectile.previousPosition;
    serverTickProjectile.velocity = Vector3 { 18.0f, 0.0f, 0.0f };
    serverTickProjectile.ownerId = controlled.GetId();
    serverTickProjectile.ownerTeamId = controlled.GetTeamId();
    serverTickProjectile.lifetime = 1.0f;
    ApplyProjectileDefaults(serverTickProjectile);
    projectiles_.push_back(serverTickProjectile);
    {
        ScopedLocalFeedbackSuppression suppressServerFeedback(*this, true);
        UpdateMatchSimulation(matchSimulation_.FixedDeltaSeconds());
    }
    const bool serverTickImpactWorked = enemy->GetHealth() < serverTickHpBefore;
    // Projectile impacts push their own owner-private CombatEvent (see
    // UpdateProjectiles' PushCombatEventSnapshots call) independent of the
    // melee combat push checked above — assert it reaches BOTH the shooter and
    // the victim, the same way a melee hit does.
    const MatchSnapshot serverTickShooterView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), controlled.GetId());
    const MatchSnapshot serverTickVictimView =
        FilterSnapshotForClient(BuildNetworkSnapshot(), enemy->GetId());
    const bool serverTickShooterResultReplicated = std::any_of(
        serverTickShooterView.actionResults.begin(),
        serverTickShooterView.actionResults.end(),
        [&controlled, enemy](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::CombatEvent)
                && result.actorPlayerId == controlled.GetId()
                && result.targetPlayerId == enemy->GetId()
                && result.amount > 0
                && (result.flags & kCombatFlagRecipientAttacker) != 0;
        });
    const bool serverTickVictimResultReplicated = std::any_of(
        serverTickVictimView.actionResults.begin(),
        serverTickVictimView.actionResults.end(),
        [&controlled, enemy](const ActionResultSnapshot& result)
        {
            return result.resultSeq != 0
                && result.actionType == static_cast<int>(PlayerActionType::CombatEvent)
                && result.actorPlayerId == controlled.GetId()
                && result.targetPlayerId == enemy->GetId()
                && result.amount > 0
                && (result.flags & kCombatFlagRecipientTarget) != 0;
        });
    const bool serverTickResultsReplicated = serverTickShooterResultReplicated && serverTickVictimResultReplicated;

    // Stable ids for status effects: unlike every other dynamic entity type
    // these have no spawn moment (see NetworkSnapshot.h) — the id is a pure
    // function of (type, target, owner), so the SAME conceptual effect (this
    // player's speed boost) must keep the SAME id across two snapshots even
    // as an unrelated status effect on ANOTHER player appears in between
    // (which would have shifted an index-based id).
    controlled.ActivateSpeedBoost(5.0f);
    const auto findControlledSpeedBoostId = [&controlled](const MatchSnapshot& snap) -> int
    {
        for (const StatusEffectSnapshot& status : snap.statusEffects)
        {
            if (status.type == StatusEffectType::SpeedBoost && status.targetPlayerId == controlled.GetId())
            {
                return status.id;
            }
        }
        return -1;
    };
    const int speedBoostIdBefore = findControlledSpeedBoostId(BuildNetworkSnapshot());
    enemy->ActivateSpeedBoost(5.0f);
    const int speedBoostIdAfter = findControlledSpeedBoostId(BuildNetworkSnapshot());
    const bool statusEffectIdStable = speedBoostIdBefore > 0 && speedBoostIdBefore == speedBoostIdAfter;

    const bool presentationSuppressed =
        message_ == messageBefore
        && eventMessages_.size() == eventMessagesBefore
        && worldEffects_.size() == worldEffectsBefore
        && floatingTexts_.size() == floatingTextsBefore;
    const bool localCombatFeedbackSuppressed =
        stats_.hitsDealt == statsBefore.hitsDealt
        && stats_.damageDealt == statsBefore.damageDealt
        && stats_.kills == statsBefore.kills
        && stats_.coreDamageDealt == statsBefore.coreDamageDealt
        && stats_.coresDestroyed == statsBefore.coresDestroyed
        && stats_.blocksPlaced == statsBefore.blocksPlaced
        && stats_.blocksBroken == statsBefore.blocksBroken
        && hitMarkerTimer_ == hitMarkerBefore
        && damageFlashTimer_ == damageFlashBefore
        && fovKick_ == fovKickBefore;
    const bool audioRestored = audio_.IsMuted() == audioMutedBefore;
    localPlayerId_ = localPlayerIdBefore;

    networkControlledPlayerIds_.clear();
    networkActionState_.clear();

    std::cout << "network-actions-smoke: meleeHp " << enemyHpBefore << "->" << enemyHpAfter
              << " melee=" << (meleeWorked ? "ok" : "FAIL")
              << " combatResults=" << (combatResultsReplicated ? "ok" : "FAIL")
              << " | blocks " << blocksBeforePlace << "->" << blocksAfterPlace
              << " heroAbility=" << (heroAbilityWorked ? "ok" : "FAIL")
              << " dash=" << (dashWorked ? "ok" : "FAIL")
              << " dashResults=" << (dashResultReplicated ? "ok" : "FAIL")
              << " place=" << (placeWorked ? "ok" : "FAIL")
              << " break=" << (breakWorked ? "ok" : "FAIL")
              << " breakProgress=" << (breakProgressVisible ? "ok" : "FAIL")
              << " predictedBreakUi=" << (predictedBreakUiProgress ? "ok" : "FAIL")
              << " localBreakUi=" << (localBreakUiProgress ? "ok" : "FAIL")
              << " blockDrop=" << (blockDropWorked ? "ok" : "FAIL")
              << " deviceDamage=" << (heroDeviceDamageWorked ? "ok" : "FAIL")
              << " likhoCut=" << (likhoCutRegistered ? "ok" : "FAIL")
              << " blockResults=" << (blockResultsReplicated ? "ok" : "FAIL")
              << " bromChest=" << ((bromChestBlockWorked && bromChestDeliveryWorked) ? "ok" : "FAIL")
              << " serverTick=" << (serverTickImpactWorked ? "ok" : "FAIL")
              << " serverTickResults=" << (serverTickResultsReplicated ? "ok" : "FAIL")
              << " statusEffectIdStable=" << (statusEffectIdStable ? "ok" : "FAIL")
              << " presentation=" << (presentationSuppressed ? "ok" : "FAIL")
              << " localCombat=" << (localCombatFeedbackSuppressed ? "ok" : "FAIL")
              << " audioRestore=" << (audioRestored ? "ok" : "FAIL") << '\n';

    const bool ok = meleeWorked && combatResultsReplicated
        && heroAbilityWorked && dashWorked && dashResultReplicated && placeWorked && breakWorked
        && breakProgressVisible && predictedBreakUiProgress && localBreakUiProgress && blockDropWorked
        && heroDeviceDamageWorked && likhoCutRegistered
        && blockResultsReplicated
        && bromChestBlockWorked && bromChestDeliveryWorked
        && serverTickImpactWorked && serverTickResultsReplicated && statusEffectIdStable
        && presentationSuppressed && localCombatFeedbackSuppressed && audioRestored;
    std::cout << (ok ? "NETWORK_ACTIONS_SMOKE_OK" : "NETWORK_ACTIONS_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunLagCompSmoke()
{
    // #4 headless test: a melee that only overlaps the target's PAST position
    // must land when the command carries a rewindTick that points at that past
    // frame, and must MISS the same geometry when it doesn't (live position).
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        networkMode_ = NetworkMode::LocalHost;
    }
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    SetupMatch();

    // Attacker: a non-local (network-controlled) player so GetSelectedHotbarStack
    // reads its per-player slot; enemy: any player on another team.
    Player* attackerPtr = nullptr;
    for (Player& candidate : matchSimulation_.Players())
    {
        if (IsBotControlled(ControlKindForPlayer(candidate)))
        {
            attackerPtr = &candidate;
            break;
        }
    }
    Player* enemyPtr = nullptr;
    if (attackerPtr != nullptr)
    {
        for (Player& candidate : matchSimulation_.Players())
        {
            if (candidate.GetTeamId() != attackerPtr->GetTeamId())
            {
                enemyPtr = &candidate;
                break;
            }
        }
    }
    if (attackerPtr == nullptr || enemyPtr == nullptr)
    {
        std::cout << "lag-comp-smoke: missing attacker/enemy\n"
                     "LAG_COMP_SMOKE_FAIL" << std::endl;
        return 9;
    }
    Player& attacker = *attackerPtr;
    Player& enemy = *enemyPtr;
    const int attackerId = attacker.GetId();
    const int enemyId = enemy.GetId();
    MarkNetworkControlledPlayer(attackerId);
    suppressLocalFeedback_ = false;

    // Clear air; attacker faces +X, gives it a sword.
    attacker.SetPosition(Vec3 { 0.0f, 40.0f, 0.0f });
    const float aimYaw = PI / 2.0f; // Forward() = (+1, 0, 0)
    attacker.SetYaw(aimYaw);
    ItemStack sword;
    sword.type = ItemType::Sword;
    sword.count = 1;
    attacker.GetInventory().SwapSlot(0, sword);
    attacker.SetSelectedSlot(0);
    enemy.Heal(enemy.GetMaxHealth());
    enemy.UpdateTimers(2.0f); // clear hit-invulnerability

    PlayerCommand base;
    base.controlledPlayerId = static_cast<std::uint32_t>(attackerId);
    base.selectedSlot = 0;
    base.aimYaw = aimYaw;
    base.aimPitch = 0.0f;

    // Positions: PAST = right in front of the attacker (in melee range), NOW =
    // far away so a live-position hit test can't reach it.
    const Vec3 pastPos { 2.0f, 40.0f, 0.0f };
    const Vec3 nowPos { 40.0f, 40.0f, 0.0f };

    // Record a history frame with the enemy at its PAST position, at a known
    // tick. RecordLagCompFrame reads matchSimulation_.CurrentTick(); advance a
    // few ticks so rewindTick has room below the current tick.
    lagCompHistory_.clear();
    for (int i = 0; i < 5; ++i)
    {
        matchSimulation_.AdvanceTick();
    }
    enemy.SetPosition(pastPos);
    RecordLagCompFrame();
    const std::uint32_t recordedTick = matchSimulation_.CurrentTick();
    for (int i = 0; i < 5; ++i)
    {
        matchSimulation_.AdvanceTick();
    }

    // 1) WITHOUT rewind (rewindTick 0): enemy is at NOW (far) — melee misses.
    enemy.SetPosition(nowPos);
    enemy.Heal(enemy.GetMaxHealth());
    enemy.UpdateTimers(2.0f);
    const int hpBeforeNoRewind = enemy.GetHealth();
    PlayerCommand noRewind = base;
    noRewind.attackPressed = true;
    noRewind.rewindTick = 0;
    ApplyNetworkPlayerActions(attacker, noRewind, matchSimulation_.FixedDeltaSeconds());
    const int hpAfterNoRewind = enemy.GetHealth();
    const bool missedLive = hpAfterNoRewind == hpBeforeNoRewind;

    // 2) WITH rewind: enemy still at NOW (far), but the command rewinds hitboxes
    // to recordedTick where the enemy was at PAST (in range) — melee lands, and
    // the enemy's LIVE position is restored afterward (only HP/velocity persist).
    enemy.SetPosition(nowPos);
    enemy.Heal(enemy.GetMaxHealth());
    enemy.UpdateTimers(2.0f);
    attacker.ResetAttackCooldown(0.0f);
    attacker.UpdateTimers(1.0f);
    const int hpBeforeRewind = enemy.GetHealth();
    PlayerCommand withRewind = base;
    withRewind.attackPressed = true;
    withRewind.rewindTick = recordedTick;
    ApplyNetworkPlayerActions(attacker, withRewind, matchSimulation_.FixedDeltaSeconds());
    const int hpAfterRewind = enemy.GetHealth();
    const Vec3 enemyPosAfter = enemy.GetPositionVec3();
    const bool hitRewound = hpAfterRewind < hpBeforeRewind;
    const bool positionRestored =
        std::fabs(enemyPosAfter.x - nowPos.x) < 0.001f
        && std::fabs(enemyPosAfter.z - nowPos.z) < 0.001f;

    const bool ok = missedLive && hitRewound && positionRestored;
    std::cout << "lag-comp-smoke: attackerId=" << attackerId << " enemyId=" << enemyId
              << " recordedTick=" << recordedTick
              << " liveMiss=" << (missedLive ? "ok" : "FAIL")
              << " (hp " << hpBeforeNoRewind << "->" << hpAfterNoRewind << ")"
              << " rewoundHit=" << (hitRewound ? "ok" : "FAIL")
              << " (hp " << hpBeforeRewind << "->" << hpAfterRewind << ")"
              << " positionRestored=" << (positionRestored ? "ok" : "FAIL") << '\n';
    std::cout << (ok ? "LAG_COMP_SMOKE_OK" : "LAG_COMP_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 9;
}

int Game::RunClientDynamicApplySmoke()
{
    Game server;
    if (!server.Initialize(true))
    {
        std::cout << "client-dynamic-apply-smoke: server initialize failed\n"
                     "CLIENT_DYNAMIC_APPLY_SMOKE_FAIL" << std::endl;
        return 9;
    }

    server.networkMode_ = NetworkMode::LocalHost;
    server.selectedMode_ = MatchMode::FourTeams;
    server.selectedTeamId_ = 0;
    // Pin the roster + difficulty so the test is independent of a persisted
    // DaiBed.settings (which could carry 0 bots / team size 1 → "not enough
    // players", or Hard bots that disturb the scripted state).
    server.selectedTeamSize_ = 4;
    server.selectedBotCount_ = 15;
    server.botDifficulty_ = BotDifficulty::Normal;
    server.SetupMatch();
    server.screen_ = GameScreen::Playing;

    if (server.matchSimulation_.Players().size() < 2)
    {
        std::cout << "client-dynamic-apply-smoke: not enough players\n"
                     "CLIENT_DYNAMIC_APPLY_SMOKE_FAIL" << std::endl;
        server.Shutdown();
        return 9;
    }

    Player& owner = server.matchSimulation_.Players().front();
    Player& target = server.matchSimulation_.Players()[1];
    owner.SetPosition(Vec3 { 3.0f, 35.0f, -2.0f });
    target.SetPosition(Vec3 { 5.5f, 35.0f, -2.0f });
    owner.ActivateSpeedBoost(4.0f);
    target.ApplyControlDebuff(3.5f, 0.72f, 0.78f, 0.70f);

    EnergyProjectile projectile;
    projectile.kind = ProjectileKind::Fireball;
    projectile.position = Vector3 { 4.0f, 36.0f, -2.0f };
    projectile.previousPosition = projectile.position;
    projectile.startPosition = projectile.position;
    projectile.velocity = Vector3 { 9.0f, 1.0f, 0.0f };
    projectile.ownerId = owner.GetId();
    projectile.ownerTeamId = owner.GetTeamId();
    projectile.lifetime = 2.25f;
    projectile.fireZone = true;
    ApplyProjectileDefaults(projectile);
    server.projectiles_.push_back(projectile);

    TimedExplosion explosive;
    explosive.position = Vector3 { 8.0f, 35.0f, -2.0f };
    explosive.ownerPlayerId = owner.GetId();
    explosive.ownerTeamId = owner.GetTeamId();
    explosive.timer = 1.35f;
    explosive.radius = 2.75f;
    server.timedExplosions_.push_back(explosive);

    HazardZone hazard;
    hazard.position = Vector3 { 6.0f, 35.0f, -2.0f };
    hazard.ownerPlayerId = owner.GetId();
    hazard.ownerTeamId = owner.GetTeamId();
    hazard.radius = 3.25f;
    hazard.lifetime = 4.5f;
    hazard.damagePerTick = 16;
    hazard.blueFire = true;
    server.hazardZones_.push_back(hazard);

    BromVacuumBot bot;
    bot.position = Vector3 { 2.0f, 35.0f, -1.0f };
    bot.ownerPlayerId = owner.GetId();
    bot.ownerTeamId = owner.GetTeamId();
    bot.lifetime = 7.0f;
    bot.health = 44;
    server.bromVacuumBots_.push_back(bot);

    BromTurretDrone drone;
    drone.position = Vector3 { 2.5f, 35.0f, -1.5f };
    drone.ownerPlayerId = owner.GetId();
    drone.ownerTeamId = owner.GetTeamId();
    drone.lifetime = 8.0f;
    drone.health = 58;
    server.bromTurretDrones_.push_back(drone);

    KonvoyTrap trap;
    trap.position = Vector3 { 3.0f, 35.0f, -3.5f };
    trap.ownerPlayerId = owner.GetId();
    trap.ownerTeamId = owner.GetTeamId();
    trap.lifetime = 6.0f;
    trap.health = 47;
    server.konvoyTraps_.push_back(trap);

    KonvoyTether tether;
    tether.ownerPlayerId = owner.GetId();
    tether.targetPlayerId = target.GetId();
    tether.ownerTeamId = owner.GetTeamId();
    tether.lifetime = 5.0f;
    server.konvoyTethers_.push_back(tether);

    KonvoyDome dome;
    dome.position = Vector3 { 4.0f, 35.0f, -4.0f };
    dome.ownerPlayerId = owner.GetId();
    dome.ownerTeamId = owner.GetTeamId();
    dome.lifetime = 9.0f;
    dome.health = 160;
    server.konvoyDomes_.push_back(dome);

    LikhoBleed bleed;
    bleed.targetPlayerId = target.GetId();
    bleed.ownerPlayerId = owner.GetId();
    bleed.ownerTeamId = owner.GetTeamId();
    bleed.lifetime = 4.25f;
    bleed.stacks = 2;
    server.likhoBleeds_.push_back(bleed);

    SvidetelEcho echo;
    echo.position = Vector3 { 5.0f, 35.0f, -1.0f };
    echo.ownerPlayerId = owner.GetId();
    echo.ownerTeamId = owner.GetTeamId();
    echo.lifetime = 11.0f;
    echo.health = 41;
    server.svidetelEchoes_.push_back(echo);

    server.radonBurns_.push_back(
        RadonBurn { target.GetId(), owner.GetId(), owner.GetTeamId(), 2.2f, 0.7f, true });
    KonvoyIntruderMark mark;
    mark.ownerPlayerId = owner.GetId();
    mark.targetPlayerId = target.GetId();
    mark.exposure = 1.0f;
    mark.markedTimer = 3.3f;
    server.konvoyIntruderMarks_.push_back(mark);

    server.matchSimulation_.DroppedItems().push_back(DroppedItem {
        ItemStack { ItemType::IronResource, 3 },
        Vec3 { 3.2f, 35.8f, -2.4f },
        Vec3 { 1.25f, 1.2f, 0.35f },
        owner.GetId(),
        0.0f,
        12.0f,
        1.0f,
        false,
        server.NextDroppedItemId() });

    // Distinctive animation poses so we can prove the client adopts them. The
    // owner is the client's OWN player (event pose → adopted), the target is a
    // remote player (locomotion → adopted, since remotes take the full pose).
    owner.MutableHeroState().animationState = HeroAnimationState::Attack;  // event
    target.MutableHeroState().animationState = HeroAnimationState::Run;    // locomotion

    const MatchSnapshot snapshot = server.BuildNetworkSnapshot();

    Game client;
    if (!client.Initialize(true))
    {
        std::cout << "client-dynamic-apply-smoke: client initialize failed\n"
                     "CLIENT_DYNAMIC_APPLY_SMOKE_FAIL" << std::endl;
        server.Shutdown();
        return 9;
    }
    client.networkMode_ = NetworkMode::LocalClient;
    client.selectedMode_ = MatchMode::FourTeams;
    client.selectedTeamId_ = 0;
    client.SetupMatch();
    client.networkAssignedPlayerId_ = owner.GetId();
    client.localPlayerId_ = owner.GetId();

    client.projectiles_.clear();
    client.timedExplosions_.clear();
    client.hazardZones_.clear();
    client.bromVacuumBots_.clear();
    client.bromTurretDrones_.clear();
    client.konvoyTraps_.clear();
    client.konvoyTethers_.clear();
    client.konvoyDomes_.clear();
    client.likhoBleeds_.clear();
    client.svidetelEchoes_.clear();
    client.radonBurns_.clear();
    client.konvoyIntruderMarks_.clear();
    client.replicatedProjectiles_.clear();
    client.replicatedExplosives_.clear();
    client.replicatedHazardZones_.clear();
    client.replicatedHeroDevices_.clear();
    client.replicatedStatusEffects_.clear();

    client.ApplyClientSnapshot(snapshot);

    const auto near = [](float a, float b)
    {
        return std::fabs(a - b) < 0.001f;
    };
    const auto sameVec = [&near](Vec3 a, Vec3 b)
    {
        return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
    };
    const auto sameRayVec = [&near](Vector3 a, Vec3 b)
    {
        return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
    };

    const bool projectileApplied = !snapshot.projectiles.empty()
        && client.projectiles_.size() == snapshot.projectiles.size()
        && client.replicatedProjectiles_.size() == snapshot.projectiles.size()
        && client.replicatedProjectiles_[0].id == snapshot.projectiles[0].id
        && sameVec(client.replicatedProjectiles_[0].position, snapshot.projectiles[0].position)
        && sameRayVec(client.projectiles_[0].position, snapshot.projectiles[0].position)
        && client.projectiles_[0].kind == ProjectileKind::Fireball;
    const Vector3 projectileVisualBefore = !client.projectiles_.empty()
        ? client.projectiles_[0].position
        : Vector3 {};
    const float projectileLifetimeBefore = !client.projectiles_.empty()
        ? client.projectiles_[0].lifetime
        : 0.0f;
    const Vec3 droppedItemBefore = !client.matchSimulation_.DroppedItems().empty()
        ? client.matchSimulation_.DroppedItems()[0].position
        : Vec3 {};
    const float droppedItemLifetimeBefore = !client.matchSimulation_.DroppedItems().empty()
        ? client.matchSimulation_.DroppedItems()[0].lifetime
        : 0.0f;
    client.UpdateClientReplicatedDynamics(0.05f);
    const Vector3 projectileVisualDelta {
        client.projectiles_.empty() ? 0.0f : client.projectiles_[0].position.x - projectileVisualBefore.x,
        client.projectiles_.empty() ? 0.0f : client.projectiles_[0].position.y - projectileVisualBefore.y,
        client.projectiles_.empty() ? 0.0f : client.projectiles_[0].position.z - projectileVisualBefore.z
    };
    const bool projectileVisualAdvanced = !client.projectiles_.empty()
        && std::sqrt(projectileVisualDelta.x * projectileVisualDelta.x
            + projectileVisualDelta.y * projectileVisualDelta.y
            + projectileVisualDelta.z * projectileVisualDelta.z) > 0.01f
        && client.projectiles_[0].lifetime < projectileLifetimeBefore;
    const bool droppedItemApplied = !snapshot.droppedItems.empty()
        && !client.matchSimulation_.DroppedItems().empty()
        && client.matchSimulation_.DroppedItems()[0].id == snapshot.droppedItems[0].id
        && client.matchSimulation_.DroppedItems()[0].stack.type == static_cast<ItemType>(snapshot.droppedItems[0].itemType);
    const Vec3 droppedItemAfter = !client.matchSimulation_.DroppedItems().empty()
        ? client.matchSimulation_.DroppedItems()[0].position
        : Vec3 {};
    const Vec3 droppedItemDelta {
        droppedItemAfter.x - droppedItemBefore.x,
        droppedItemAfter.y - droppedItemBefore.y,
        droppedItemAfter.z - droppedItemBefore.z
    };
    const bool droppedItemVisualAdvanced = droppedItemApplied
        && LengthVec3(droppedItemDelta) > 0.01f
        && client.matchSimulation_.DroppedItems()[0].lifetime < droppedItemLifetimeBefore;

    const bool explosiveApplied = !snapshot.explosives.empty()
        && client.timedExplosions_.size() == snapshot.explosives.size()
        && client.replicatedExplosives_.size() == snapshot.explosives.size()
        && client.replicatedExplosives_[0].id == snapshot.explosives[0].id
        && sameRayVec(client.timedExplosions_[0].position, snapshot.explosives[0].position)
        && sameRayVec(client.timedExplosions_[0].velocity, snapshot.explosives[0].velocity);

    const bool hazardApplied = !snapshot.hazardZones.empty()
        && client.hazardZones_.size() == snapshot.hazardZones.size()
        && client.replicatedHazardZones_.size() == snapshot.hazardZones.size()
        && client.replicatedHazardZones_[0].id == snapshot.hazardZones[0].id
        && sameRayVec(client.hazardZones_[0].position, snapshot.hazardZones[0].position)
        && client.hazardZones_[0].blueFire;

    const std::size_t clientDeviceCount = client.bromVacuumBots_.size()
        + client.bromTurretDrones_.size()
        + client.konvoyTraps_.size()
        + client.konvoyTethers_.size()
        + client.konvoyDomes_.size()
        + client.likhoBleeds_.size()
        + client.svidetelEchoes_.size();
    const bool devicesApplied = !snapshot.heroDevices.empty()
        && clientDeviceCount == snapshot.heroDevices.size()
        && client.replicatedHeroDevices_.size() == snapshot.heroDevices.size()
        && client.replicatedHeroDevices_[0].id == snapshot.heroDevices[0].id
        && sameRayVec(client.bromVacuumBots_[0].position, snapshot.heroDevices[0].position);

    const bool statusesApplied = !snapshot.statusEffects.empty()
        && client.replicatedStatusEffects_.size() == snapshot.statusEffects.size()
        && !client.radonBurns_.empty()
        && !client.konvoyIntruderMarks_.empty()
        && client.radonBurns_[0].blueFire
        && client.radonBurns_[0].targetPlayerId == target.GetId()
        && client.konvoyIntruderMarks_[0].targetPlayerId == target.GetId();

    const Player* clientOwner = client.matchSimulation_.GetPlayer(owner.GetId());
    const Player* clientTarget = client.matchSimulation_.GetPlayer(target.GetId());
    // Own player adopts the server EVENT pose; remote player adopts the full pose
    // (locomotion included).
    const bool animationApplied = clientOwner != nullptr && clientTarget != nullptr
        && clientOwner->GetHeroState().animationState == HeroAnimationState::Attack
        && clientTarget->GetHeroState().animationState == HeroAnimationState::Run;

    owner.Damage(18);
    owner.SetVelocity(Vec3 { 5.75f, 1.35f, 0.0f });
    const MatchSnapshot knockbackSnapshot = server.BuildNetworkSnapshot();
    client.ApplyClientSnapshot(knockbackSnapshot);
    const Player* clientOwnerAfterHit = client.matchSimulation_.GetPlayer(owner.GetId());
    const bool ownKnockbackVelocityApplied = clientOwnerAfterHit != nullptr
        && clientOwnerAfterHit->GetHealth() == owner.GetHealth()
        && DistanceVec3(clientOwnerAfterHit->GetVelocityVec3(), owner.GetVelocityVec3()) < 0.001f;

    // Own-player rule: a LOCOMOTION pose in the snapshot must NOT be adopted for our
    // own player (its locomotion is derived locally from prediction). Re-send with
    // the owner now "running" and confirm the client keeps the event pose instead.
    owner.MutableHeroState().animationState = HeroAnimationState::Run;
    client.ApplyClientSnapshot(server.BuildNetworkSnapshot());
    const Player* clientOwner2 = client.matchSimulation_.GetPlayer(owner.GetId());
    const bool ownLocomotionLocal = clientOwner2 != nullptr
        && clientOwner2->GetHeroState().animationState != HeroAnimationState::Run;
    const bool animationOk = animationApplied && ownLocomotionLocal;

    // Regression (2026-07-01, user bug report): "ability cooldown HUD always
    // says ready" — PlayerSnapshot never carried HeroRuntimeState at all, so a
    // network client's own ability HUD (Renderer's AbilityStateText, which
    // reads player.GetHeroState()) was permanently stuck on defaults. Set a
    // known non-zero cooldown/charge/primed state on the server's owner,
    // fold a snapshot, and confirm the CLIENT's copy of the same player
    // reflects the real values instead of staying at 0/false.
    owner.MutableHeroState().active1.cooldownRemaining = 4.5f;
    owner.MutableHeroState().active2.activeTimer = 1.1f;
    owner.MutableHeroState().ultimate.cooldownRemaining = 22.0f;
    owner.MutableHeroState().ultimateCharge = 57.0f;
    owner.MutableHeroState().ultimatePrimed = true;
    client.ApplyClientSnapshot(FilterSnapshotForClient(server.BuildNetworkSnapshot(), owner.GetId()));
    const Player* clientOwnerForHud = client.matchSimulation_.GetPlayer(owner.GetId());
    const bool abilityHudApplied = clientOwnerForHud != nullptr
        && near(clientOwnerForHud->GetHeroState().active1.cooldownRemaining, 4.5f)
        && near(clientOwnerForHud->GetHeroState().active2.activeTimer, 1.1f)
        && near(clientOwnerForHud->GetHeroState().ultimate.cooldownRemaining, 22.0f)
        && near(clientOwnerForHud->GetHeroState().ultimateCharge, 57.0f)
        && clientOwnerForHud->GetHeroState().ultimatePrimed;

    // Regression (2026-07-01, user bug report): "sniper doesn't charge" —
    // bow/blaster charge state has the exact same gap as ability cooldowns
    // (never in PlayerSnapshot, so a network client's own Player object never
    // learns it — UpdateCombatPreview's charge % HUD read stuck defaults).
    // Advance real charge state server-side and confirm it round-trips.
    owner.ResetBowDraw();
    owner.AdvanceBowDraw(0.35f);
    owner.CancelBlasterLoading();
    owner.StartBlasterLoading();
    client.ApplyClientSnapshot(FilterSnapshotForClient(server.BuildNetworkSnapshot(), owner.GetId()));
    const Player* clientOwnerForCharge = client.matchSimulation_.GetPlayer(owner.GetId());
    const bool weaponChargeApplied = clientOwnerForCharge != nullptr
        && near(clientOwnerForCharge->GetBowDrawTimer(), owner.GetBowDrawTimer())
        && owner.GetBowDrawTimer() > 0.0f
        && clientOwnerForCharge->GetBlasterState() == owner.GetBlasterState()
        && owner.GetBlasterState() == CrossbowState::Loading;

    // Diagnostic (2026-07-01, user bug report): does a real Orbita dash cast,
    // pushed through PushHeroAbilityActionResultSnapshot and folded through
    // ApplyClientSnapshotFeedback exactly like a real network client would,
    // actually reconstruct the correct world-effect KIND on the client, or
    // does it silently fall back to Burst (which would explain "leaves a
    // sphere identical to a pickup effect" — pickup uses the 4-arg
    // AddWorldEffect overload, which always hardcodes Burst).
    owner.SetHeroId(HeroId::Orbita);
    owner.SetYaw(0.0f);
    owner.MutableHeroState().active1.cooldownRemaining = 0.0f;
    // client.Initialize(true) (headless) defaults suppressLocalFeedback_ to
    // true, which would make PresentHeroAbilityResult silently no-op — force
    // it off, matching how RunNetworkActionsSmoke already does this to test
    // presentation while staying headless. A real GUI client is never headless
    // so this suppression never applies there.
    client.suppressLocalFeedback_ = false;
    const std::size_t clientEffectsBeforeDash = client.worldEffects_.size();
    PlayerCommand orbitaDashDiagCmd;
    orbitaDashDiagCmd.controlledPlayerId = static_cast<std::uint32_t>(owner.GetId());
    orbitaDashDiagCmd.aimYaw = 0.0f;
    orbitaDashDiagCmd.useAbility1 = true;
    const bool orbitaDashDiagApplied = server.ApplyPlayerActionCommand(owner, orbitaDashDiagCmd);
    const MatchSnapshot orbitaDashSnapshot =
        FilterSnapshotForClient(server.BuildNetworkSnapshot(), owner.GetId());
    // ApplyClientSnapshotFeedback itself early-returns on `headless_` (a real
    // client is never headless — there'd be nothing to present to — but this
    // whole smoke harness runs headless by construction). Flip it off only
    // around this one call so the diagnostic actually exercises the function
    // instead of silently no-op'ing.
    client.headless_ = false;
    client.ApplyClientSnapshotFeedback(orbitaDashSnapshot);
    client.headless_ = true;
    const WorldEffect* orbitaDashEffect = client.worldEffects_.size() > clientEffectsBeforeDash
        ? &client.worldEffects_.back()
        : nullptr;
    const bool orbitaDashEffectKindCorrect = orbitaDashDiagApplied
        && orbitaDashEffect != nullptr
        && orbitaDashEffect->kind == WorldEffectKind::Trail
        && orbitaDashEffect->radius > 1.0f && orbitaDashEffect->radius < 1.3f;
    std::cout << "client-dynamic-apply-smoke: orbitaDash applied="
              << (orbitaDashDiagApplied ? "yes" : "no")
              << " effectPushed=" << (orbitaDashEffect != nullptr ? "yes" : "no")
              << " kind=" << (orbitaDashEffect != nullptr ? static_cast<int>(orbitaDashEffect->kind) : -1)
              << " radius=" << (orbitaDashEffect != nullptr ? orbitaDashEffect->radius : -1.0f)
              << " expectedKind=" << static_cast<int>(WorldEffectKind::Trail) << '\n';

    // Diagnostic (2026-07-01, RunNetworkClient audit): HandleDeathsAndRespawns
    // is server-only (never runs on the client), so a network player's own
    // death previously produced NO death overlay and NEVER entered spectator
    // mode on final death — PushWorldEventSnapshot(PlayerDied) + the client's
    // new WorldEventKind::PlayerDied branch above are the fix. Push a real
    // final-death event for the client's own player (owner) and confirm the
    // overlay/spectator-mode/killer-name/cause all land.
    server.spectatorMode_ = false;
    server.PushWorldEventSnapshot(
        WorldEventKind::PlayerDied, target.GetId(), owner.GetId(), owner.GetTeamId(),
        owner.GetPosition(), 0, 0, /*finalDeath*/ 1, "топором Свидетеля");
    const MatchSnapshot ownDeathSnapshot =
        FilterSnapshotForClient(server.BuildNetworkSnapshot(), owner.GetId());
    client.spectatorMode_ = false;
    client.localDeathOverlayTimer_ = 0.0f;
    client.headless_ = false;
    client.ApplyClientSnapshotFeedback(ownDeathSnapshot);
    client.headless_ = true;
    const bool ownDeathOverlayApplied = client.localDeathOverlayTimer_ > 0.0f
        && client.spectatorMode_
        && client.localDeathKiller_ == target.GetName()
        && client.localDeathCause_ == "топором Свидетеля";
    std::cout << "client-dynamic-apply-smoke: ownDeath overlayTimer=" << client.localDeathOverlayTimer_
              << " spectator=" << (client.spectatorMode_ ? "yes" : "no")
              << " killer=" << client.localDeathKiller_
              << " result=" << (ownDeathOverlayApplied ? "ok" : "FAIL") << '\n';

    Player* clientOwnerForSpectator = client.matchSimulation_.GetPlayer(owner.GetId());
    Player* clientTargetForSpectator = client.matchSimulation_.GetPlayer(target.GetId());
    if (clientOwnerForSpectator != nullptr)
    {
        clientOwnerForSpectator->Kill(true);
    }
    if (clientTargetForSpectator != nullptr && !clientTargetForSpectator->IsAlive())
    {
        clientTargetForSpectator->RespawnAtHome();
    }
    client.networkAssignedPlayerId_ = owner.GetId();
    client.localPlayerId_ = owner.GetId();
    client.spectatorMode_ = false;
    client.UpdateCamera(1.0f / 60.0f);
    const Player* initialSpectatorTarget = client.GetSpectatorTarget();
    client.CycleSpectatorTarget(1);
    const Player* cycledSpectatorTarget = client.GetSpectatorTarget();
    client.spectatorFreeCamera_ = true;
    client.UpdateCamera(1.0f / 60.0f);
    const bool spectatorControlsOk = client.spectatorMode_
        && initialSpectatorTarget != nullptr
        && cycledSpectatorTarget != nullptr
        && client.spectatorFreeCamera_;
    if (clientOwnerForSpectator != nullptr)
    {
        clientOwnerForSpectator->RespawnAtHome();
    }
    client.UpdateCamera(1.0f / 60.0f);
    const bool spectatorRespawnOk = !client.spectatorMode_
        && !client.spectatorFreeCamera_
        && clientOwnerForSpectator != nullptr
        && clientOwnerForSpectator->IsAlive();
    std::cout << "client-dynamic-apply-smoke: spectator controls="
              << (spectatorControlsOk ? "ok" : "FAIL")
              << " respawn=" << (spectatorRespawnOk ? "ok" : "FAIL") << '\n';

    const bool ok = projectileApplied && projectileVisualAdvanced
        && droppedItemApplied && droppedItemVisualAdvanced
        && explosiveApplied && hazardApplied
        && devicesApplied && statusesApplied && animationOk && ownKnockbackVelocityApplied
        && orbitaDashEffectKindCorrect && abilityHudApplied && weaponChargeApplied
        && ownDeathOverlayApplied && spectatorControlsOk && spectatorRespawnOk;

    std::cout << "client-dynamic-apply-smoke: snapshot projectiles=" << snapshot.projectiles.size()
              << " explosives=" << snapshot.explosives.size()
              << " hazardZones=" << snapshot.hazardZones.size()
              << " heroDevices=" << snapshot.heroDevices.size()
              << " statusEffects=" << snapshot.statusEffects.size() << '\n';
    std::cout << "client-dynamic-apply-smoke: client projectiles=" << client.projectiles_.size()
              << " explosives=" << client.timedExplosions_.size()
              << " hazardZones=" << client.hazardZones_.size()
              << " heroDevices=" << clientDeviceCount
              << " statusEffects=" << client.replicatedStatusEffects_.size() << '\n';
    std::cout << "client-dynamic-apply-smoke: checks projectile="
              << (projectileApplied ? "ok" : "FAIL")
              << " projectileVisual=" << (projectileVisualAdvanced ? "ok" : "FAIL")
              << " droppedItem=" << (droppedItemApplied ? "ok" : "FAIL")
              << " droppedItemVisual=" << (droppedItemVisualAdvanced ? "ok" : "FAIL")
              << " explosive=" << (explosiveApplied ? "ok" : "FAIL")
              << " hazard=" << (hazardApplied ? "ok" : "FAIL")
              << " devices=" << (devicesApplied ? "ok" : "FAIL")
              << " statuses=" << (statusesApplied ? "ok" : "FAIL")
              << " animation=" << (animationOk ? "ok" : "FAIL")
              << " (apply=" << (animationApplied ? "ok" : "FAIL")
              << " ownLocal=" << (ownLocomotionLocal ? "ok" : "FAIL") << ")"
              << " ownKnockback=" << (ownKnockbackVelocityApplied ? "ok" : "FAIL")
              << " abilityHud=" << (abilityHudApplied ? "ok" : "FAIL")
              << " weaponCharge=" << (weaponChargeApplied ? "ok" : "FAIL")
              << " orbitaDashEffect=" << (orbitaDashEffectKindCorrect ? "ok" : "FAIL")
              << " ownDeath=" << (ownDeathOverlayApplied ? "ok" : "FAIL")
              << " spectatorControls=" << (spectatorControlsOk ? "ok" : "FAIL")
              << " spectatorRespawn=" << (spectatorRespawnOk ? "ok" : "FAIL")
              << '\n';
    std::cout << (ok ? "CLIENT_DYNAMIC_APPLY_SMOKE_OK" : "CLIENT_DYNAMIC_APPLY_SMOKE_FAIL")
              << std::endl;

    client.Shutdown();
    server.Shutdown();
    return ok ? 0 : 9;
}

void Game::PrepareStartupSmoke()
{
    if (!headless_)
    {
        StartSelectedMatch();
    }
}

void Game::ExerciseStartupSmokeMutation(bool place)
{
    const GridPos probe { 0, 20, 0 };
    if (place)
    {
        world_.PlaceBlock(probe, Block { BlockType::StoneBlock, -1, true }, true);
    }
    else
    {
        world_.RemoveBlock(probe);
    }
}

// --- Roundtrip smoke (CLI: --protocol-smoke) ---------------------------------
namespace
{
bool VecEqual(const Vec3& a, const Vec3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool FloatEqual(float a, float b)
{
    return a == b;
}

bool CommandEqual(const PlayerCommand& a, const PlayerCommand& b)
{
    return a.controlledPlayerId == b.controlledPlayerId && a.tick == b.tick
        && a.moveForward == b.moveForward && a.moveStrafe == b.moveStrafe
        && a.aimYaw == b.aimYaw && a.aimPitch == b.aimPitch
        && a.jump == b.jump && a.sprint == b.sprint && a.sprintTapped == b.sprintTapped
        && a.sneak == b.sneak && a.selectedSlot == b.selectedSlot
        && a.attackPressed == b.attackPressed && a.attackHeld == b.attackHeld
        && a.attackReleased == b.attackReleased && a.placePressed == b.placePressed
        && a.placeHeld == b.placeHeld && a.scopeHeld == b.scopeHeld && a.interact == b.interact
        && a.useAbility1 == b.useAbility1 && a.useAbility2 == b.useAbility2
        && a.useUltimate == b.useUltimate && a.useHeal == b.useHeal && a.useTeleport == b.useTeleport
        && a.useDash == b.useDash && a.useShoot == b.useShoot && a.useFireball == b.useFireball
        && a.useMolotov == b.useMolotov && a.useAlarm == b.useAlarm
        && a.actionSeq == b.actionSeq && a.actionType == b.actionType
        && a.actionParamA == b.actionParamA && a.actionParamB == b.actionParamB
        && a.rewindTick == b.rewindTick;
}

bool InventoryEqual(const InventorySnapshot& a, const InventorySnapshot& b)
{
    if (a.present != b.present
        || a.resources != b.resources
        || a.hotbar.size() != b.hotbar.size()
        || a.main.size() != b.main.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.hotbar.size(); ++i)
    {
        if (a.hotbar[i].itemType != b.hotbar[i].itemType || a.hotbar[i].count != b.hotbar[i].count)
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.main.size(); ++i)
    {
        if (a.main[i].itemType != b.main[i].itemType || a.main[i].count != b.main[i].count)
        {
            return false;
        }
    }
    return true;
}

bool AbilityHudEqual(const HeroAbilityHudSnapshot& a, const HeroAbilityHudSnapshot& b)
{
    return a.present == b.present
        && FloatEqual(a.active1Cooldown, b.active1Cooldown)
        && FloatEqual(a.active1ActiveTimer, b.active1ActiveTimer)
        && FloatEqual(a.active2Cooldown, b.active2Cooldown)
        && FloatEqual(a.active2ActiveTimer, b.active2ActiveTimer)
        && FloatEqual(a.ultimateCooldown, b.ultimateCooldown)
        && FloatEqual(a.ultimateActiveTimer, b.ultimateActiveTimer)
        && FloatEqual(a.ultimateCharge, b.ultimateCharge)
        && a.ultimatePrimed == b.ultimatePrimed
        && FloatEqual(a.bowDrawTimer, b.bowDrawTimer)
        && a.blasterState == b.blasterState
        && FloatEqual(a.blasterLoadTimer, b.blasterLoadTimer);
}

bool ItemSlotsEqual(const std::vector<ItemStackSnapshot>& a, const std::vector<ItemStackSnapshot>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].itemType != b[i].itemType || a[i].count != b[i].count)
        {
            return false;
        }
    }
    return true;
}

bool TeamChestEqual(const TeamChestSnapshot& a, const TeamChestSnapshot& b)
{
    return a.teamId == b.teamId
        && a.resources == b.resources
        && ItemSlotsEqual(a.slots, b.slots);
}

bool PlayerScoreEqual(const PlayerScoreSnapshot& a, const PlayerScoreSnapshot& b)
{
    return a.playerId == b.playerId
        && a.kills == b.kills
        && a.deaths == b.deaths
        && a.finalDeaths == b.finalDeaths
        && a.coreDamage == b.coreDamage
        && a.coresDestroyed == b.coresDestroyed;
}

bool PlayerEqual(const PlayerSnapshot& a, const PlayerSnapshot& b)
{
    return a.playerId == b.playerId && a.playerName == b.playerName
        && a.teamId == b.teamId && a.heroId == b.heroId
        && VecEqual(a.position, b.position) && VecEqual(a.velocity, b.velocity)
        && FloatEqual(a.yaw, b.yaw) && a.health == b.health && a.maxHealth == b.maxHealth && a.alive == b.alive
        && a.eliminated == b.eliminated && FloatEqual(a.respawnTimer, b.respawnTimer)
        && a.selectedSlot == b.selectedSlot
        && a.animationState == b.animationState
        && FloatEqual(a.animationTimer, b.animationTimer)
        && FloatEqual(a.animationDuration, b.animationDuration)
        && InventoryEqual(a.inventory, b.inventory)
        && AbilityHudEqual(a.abilityHud, b.abilityHud)
        && a.disguiseTeamId == b.disguiseTeamId && a.disguiseHeroId == b.disguiseHeroId;
}

bool CoreEqual(const CoreSnapshot& a, const CoreSnapshot& b)
{
    return a.teamId == b.teamId && a.health == b.health && a.maxHealth == b.maxHealth
        && a.alive == b.alive;
}

bool GeneratorEqual(const GeneratorSnapshot& a, const GeneratorSnapshot& b)
{
    return a.resourceType == b.resourceType && a.teamId == b.teamId
        && VecEqual(a.position, b.position);
}

bool PickupEqual(const PickupSnapshot& a, const PickupSnapshot& b)
{
    return a.resourceType == b.resourceType && a.amount == b.amount
        && VecEqual(a.position, b.position);
}

bool DroppedItemEqual(const DroppedItemSnapshot& a, const DroppedItemSnapshot& b)
{
    return a.id == b.id && a.itemType == b.itemType && a.count == b.count
        && VecEqual(a.position, b.position) && VecEqual(a.velocity, b.velocity)
        && a.ownerPlayerId == b.ownerPlayerId
        && a.ownerPickupDelay == b.ownerPickupDelay
        && a.lifetime == b.lifetime
        && a.age == b.age;
}

bool DeltaEqual(const BlockDelta& a, const BlockDelta& b)
{
    return a.tick == b.tick && a.position == b.position && a.oldType == b.oldType
        && a.newType == b.newType && a.oldTeamId == b.oldTeamId && a.newTeamId == b.newTeamId
        && a.oldVariant == b.oldVariant && a.newVariant == b.newVariant
        && a.ownerPlayerId == b.ownerPlayerId && a.reason == b.reason;
}

bool ProjectileEqual(const ProjectileSnapshot& a, const ProjectileSnapshot& b)
{
    return a.id == b.id && a.kind == b.kind && VecEqual(a.position, b.position)
        && VecEqual(a.velocity, b.velocity) && a.ownerPlayerId == b.ownerPlayerId
        && a.ownerTeamId == b.ownerTeamId && a.remainingLifetime == b.remainingLifetime
        && a.fireZone == b.fireZone && a.visibility == b.visibility;
}

bool ExplosiveEqual(const ExplosiveSnapshot& a, const ExplosiveSnapshot& b)
{
    return a.id == b.id && VecEqual(a.position, b.position)
        && a.ownerPlayerId == b.ownerPlayerId && a.ownerTeamId == b.ownerTeamId
        && a.remainingTimer == b.remainingTimer && a.radius == b.radius
        && a.visibility == b.visibility;
}

bool HazardZoneEqual(const HazardZoneSnapshot& a, const HazardZoneSnapshot& b)
{
    return a.id == b.id && VecEqual(a.position, b.position)
        && a.ownerPlayerId == b.ownerPlayerId && a.ownerTeamId == b.ownerTeamId
        && a.remainingLifetime == b.remainingLifetime && a.radius == b.radius
        && a.blueFire == b.blueFire && a.visibility == b.visibility;
}

bool HeroDeviceEqual(const HeroDeviceSnapshot& a, const HeroDeviceSnapshot& b)
{
    return a.id == b.id && a.type == b.type && VecEqual(a.position, b.position)
        && a.ownerPlayerId == b.ownerPlayerId && a.ownerTeamId == b.ownerTeamId
        && a.targetPlayerId == b.targetPlayerId && a.remainingLifetime == b.remainingLifetime
        && a.health == b.health && a.visibility == b.visibility;
}

bool StatusEffectEqual(const StatusEffectSnapshot& a, const StatusEffectSnapshot& b)
{
    return a.id == b.id && a.type == b.type && VecEqual(a.position, b.position)
        && a.targetPlayerId == b.targetPlayerId && a.ownerPlayerId == b.ownerPlayerId
        && a.ownerTeamId == b.ownerTeamId && a.remaining == b.remaining
        && a.amount == b.amount && a.visibility == b.visibility;
}

bool ActionResultEqual(const ActionResultSnapshot& a, const ActionResultSnapshot& b)
{
    return a.playerId == b.playerId
        && a.resultSeq == b.resultSeq
        && a.actionSeq == b.actionSeq
        && a.actionType == b.actionType
        && a.subjectType == b.subjectType
        && a.actorPlayerId == b.actorPlayerId
        && a.targetPlayerId == b.targetPlayerId
        && a.targetTeamId == b.targetTeamId
        && a.amount == b.amount
        && a.flags == b.flags
        && a.success == b.success
        && VecEqual(a.position, b.position)
        && a.message == b.message
        && a.color == b.color
        && FloatEqual(a.seconds, b.seconds)
        && FloatEqual(a.radius, b.radius);
}

bool WorldEventEqual(const WorldEventSnapshot& a, const WorldEventSnapshot& b)
{
    return a.eventSeq == b.eventSeq
        && a.kind == b.kind
        && a.actorPlayerId == b.actorPlayerId
        && a.targetPlayerId == b.targetPlayerId
        && a.targetTeamId == b.targetTeamId
        && VecEqual(a.position, b.position)
        && a.subjectType == b.subjectType
        && a.amount == b.amount
        && a.flags == b.flags
        && a.cause == b.cause;
}

bool SnapshotEqual(const MatchSnapshot& a, const MatchSnapshot& b)
{
    if (a.tick != b.tick || a.lastProcessedCommandTick != b.lastProcessedCommandTick
        || a.matchTime != b.matchTime || a.phase != b.phase
        || a.winnerTeamId != b.winnerTeamId
        || a.players.size() != b.players.size()
        || a.matchScores.size() != b.matchScores.size()
        || a.cores.size() != b.cores.size()
        || a.teamChests.size() != b.teamChests.size()
        || a.generators.size() != b.generators.size()
        || a.pickups.size() != b.pickups.size()
        || a.droppedItems.size() != b.droppedItems.size()
        || a.blockDeltas.size() != b.blockDeltas.size()
        || a.projectiles.size() != b.projectiles.size()
        || a.explosives.size() != b.explosives.size()
        || a.hazardZones.size() != b.hazardZones.size()
        || a.heroDevices.size() != b.heroDevices.size()
        || a.statusEffects.size() != b.statusEffects.size()
        || a.actionResults.size() != b.actionResults.size()
        || a.worldEvents.size() != b.worldEvents.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.players.size(); ++i)
    {
        if (!PlayerEqual(a.players[i], b.players[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.matchScores.size(); ++i)
    {
        if (!PlayerScoreEqual(a.matchScores[i], b.matchScores[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.cores.size(); ++i)
    {
        if (!CoreEqual(a.cores[i], b.cores[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.teamChests.size(); ++i)
    {
        if (!TeamChestEqual(a.teamChests[i], b.teamChests[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.generators.size(); ++i)
    {
        if (!GeneratorEqual(a.generators[i], b.generators[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.pickups.size(); ++i)
    {
        if (!PickupEqual(a.pickups[i], b.pickups[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.droppedItems.size(); ++i)
    {
        if (!DroppedItemEqual(a.droppedItems[i], b.droppedItems[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.blockDeltas.size(); ++i)
    {
        if (!DeltaEqual(a.blockDeltas[i], b.blockDeltas[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.projectiles.size(); ++i)
    {
        if (!ProjectileEqual(a.projectiles[i], b.projectiles[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.explosives.size(); ++i)
    {
        if (!ExplosiveEqual(a.explosives[i], b.explosives[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.hazardZones.size(); ++i)
    {
        if (!HazardZoneEqual(a.hazardZones[i], b.hazardZones[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.heroDevices.size(); ++i)
    {
        if (!HeroDeviceEqual(a.heroDevices[i], b.heroDevices[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.statusEffects.size(); ++i)
    {
        if (!StatusEffectEqual(a.statusEffects[i], b.statusEffects[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.actionResults.size(); ++i)
    {
        if (!ActionResultEqual(a.actionResults[i], b.actionResults[i]))
        {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.worldEvents.size(); ++i)
    {
        if (!WorldEventEqual(a.worldEvents[i], b.worldEvents[i]))
        {
            return false;
        }
    }
    return true;
}

bool LobbyUpdateEqual(const LobbyUpdate& a, const LobbyUpdate& b)
{
    return a.playerName == b.playerName
        && a.selectedTeam == b.selectedTeam
        && a.selectedHero == b.selectedHero
        && a.ready == b.ready
        && a.startRequested == b.startRequested;
}

bool LobbyPlayerEqual(const LobbyPlayerState& a, const LobbyPlayerState& b)
{
    return a.clientId == b.clientId
        && a.assignedPlayerId == b.assignedPlayerId
        && a.playerName == b.playerName
        && a.selectedTeam == b.selectedTeam
        && a.selectedHero == b.selectedHero
        && a.ready == b.ready
        && a.connected == b.connected
        && a.startRequested == b.startRequested;
}

bool SnapshotEqual(const LobbySnapshot& a, const LobbySnapshot& b)
{
    if (a.revision != b.revision
        || a.hostClientId != b.hostClientId
        || a.serverName != b.serverName
        || a.privateServer != b.privateServer
        || a.maxPlayers != b.maxPlayers
        || a.teamCount != b.teamCount
        || a.maxTeamSize != b.maxTeamSize
        || a.heroCount != b.heroCount
        || a.enforceUniqueHeroesPerTeam != b.enforceUniqueHeroesPerTeam
        || a.requireAllReady != b.requireAllReady
        || a.canStart != b.canStart
        || a.matchStarting != b.matchStarting
        || a.matchStarted != b.matchStarted
        || a.worldBiome != b.worldBiome
        || a.worldLayout != b.worldLayout
        || a.matchMode != b.matchMode
        || a.statusMessage != b.statusMessage
        || a.players.size() != b.players.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.players.size(); ++i)
    {
        if (!LobbyPlayerEqual(a.players[i], b.players[i]))
        {
            return false;
        }
    }
    return true;
}

PlayerCommand MakeSampleCommand()
{
    PlayerCommand c;
    c.controlledPlayerId = 7;
    c.tick = 1234;
    c.moveForward = 1.0f;
    c.moveStrafe = -0.5f;
    c.aimYaw = 1.5708f;
    c.aimPitch = -0.25f;
    c.jump = true;
    c.sprint = true;
    c.sprintTapped = false;
    c.sneak = false;
    c.selectedSlot = 5;
    c.attackPressed = true;
    c.attackHeld = false;
    c.attackReleased = true;
    c.placePressed = false;
    c.placeHeld = true;
    c.scopeHeld = true;
    c.interact = true;
    c.useAbility1 = true;
    c.useAbility2 = false;
    c.useUltimate = true;
    c.useHeal = false;
    c.useTeleport = true;
    c.useDash = false;
    c.useShoot = true;
    c.useFireball = false;
    c.useMolotov = true;
    c.useAlarm = false;
    c.actionSeq = 42;
    c.actionType = static_cast<int>(PlayerActionType::BuyItem);
    c.actionParamA = 101;
    c.actionParamB = 4;
    c.rewindTick = 1200;
    return c;
}

MatchSnapshot MakeSampleSnapshot()
{
    MatchSnapshot s;
    s.tick = 4242;
    s.lastProcessedCommandTick = 4217;
    s.matchTime = 70.5f;
    s.phase = MatchPhase::Playing;
    s.winnerTeamId = -1;

    PlayerSnapshot p0;
    p0.playerId = 1;
    p0.playerName = "Alice";
    p0.teamId = 0;
    p0.heroId = 3;
    p0.position = Vec3 { -38.0f, 1.5f, 0.0f };
    p0.velocity = Vec3 { 0.25f, -9.8f, 0.0f };
    p0.yaw = 0.125f;
    p0.health = 80;
    p0.maxHealth = 100;
    p0.alive = true;
    p0.eliminated = false;
    p0.respawnTimer = 0.0f;
    p0.selectedSlot = 2;
    p0.animationState = 5; // non-default, mirrors a HeroAnimationState index
    p0.animationTimer = 0.21f;
    p0.animationDuration = 0.32f;
    p0.inventory.present = true;
    p0.inventory.resources = { 13, 4, 1 };
    p0.inventory.hotbar = { { 9, 64 }, { 14, 1 }, { 0, 0 } };
    p0.inventory.main = { { 17, 2 }, { 18, 4 }, { 0, 0 } };
    p0.abilityHud.present = true;
    p0.abilityHud.active1Cooldown = 3.2f;
    p0.abilityHud.active1ActiveTimer = 0.0f;
    p0.abilityHud.active2Cooldown = 0.0f;
    p0.abilityHud.active2ActiveTimer = 1.4f;
    p0.abilityHud.ultimateCooldown = 12.5f;
    p0.abilityHud.ultimateActiveTimer = 0.0f;
    p0.abilityHud.ultimateCharge = 64.0f;
    p0.abilityHud.ultimatePrimed = true;
    p0.abilityHud.bowDrawTimer = 0.42f;
    p0.abilityHud.blasterState = 1;
    p0.abilityHud.blasterLoadTimer = 0.18f;
    p0.disguiseTeamId = -1;
    p0.disguiseHeroId = -1;
    s.players.push_back(p0);

    PlayerSnapshot p1;
    p1.playerId = 2;
    p1.playerName = "Bob";
    p1.teamId = 1;
    p1.heroId = 5;
    p1.position = Vec3 { 38.0f, 1.5f, 2.0f };
    p1.velocity = Vec3 {};
    p1.yaw = -2.75f;
    p1.health = 0;
    p1.maxHealth = 100;
    p1.alive = false;
    p1.eliminated = true;
    p1.respawnTimer = 7.0f;
    p1.selectedSlot = 0;
    p1.inventory.present = false; // stripped (as the visibility filter would)
    p1.disguiseTeamId = 0;        // pretend an actively-disguised Likho
    p1.disguiseHeroId = 7;
    s.players.push_back(p1);

    s.matchScores.push_back(PlayerScoreSnapshot { 1, 5, 2, 0, 128, 1 });
    s.matchScores.push_back(PlayerScoreSnapshot { 2, 1, 4, 1, 32, 0 });

    s.cores.push_back(CoreSnapshot { 0, 500, 500, true });
    s.cores.push_back(CoreSnapshot { 1, 0, 500, false });

    TeamChestSnapshot chest;
    chest.teamId = 0;
    chest.resources = { 16, 3, 1 };
    chest.slots = { { 23, 16 }, { 24, 3 }, { 6, 12 }, { 0, 0 } };
    s.teamChests.push_back(chest);

    s.generators.push_back(GeneratorSnapshot { 0, 0, Vec3 { -10.0f, 1.0f, 3.0f } });
    s.generators.push_back(GeneratorSnapshot { 2, -1, Vec3 { 0.0f, 2.0f, 0.0f } });
    s.pickups.push_back(PickupSnapshot { 0, 4, Vec3 { -9.0f, 1.2f, 3.5f } });
    s.droppedItems.push_back(DroppedItemSnapshot { 401, 14, 2, Vec3 { 6.0f, 1.3f, -4.0f } });

    BlockDelta d;
    d.tick = 4242;
    d.position = GridPos { 7, 21, -7 };
    d.oldType = BlockType::Air;
    d.newType = BlockType::StoneBlock;
    d.oldTeamId = -1;
    d.newTeamId = 0;
    d.ownerPlayerId = 1;
    d.reason = BlockDeltaReason::PlayerPlace;
    s.blockDeltas.push_back(d);

    s.projectiles.push_back(ProjectileSnapshot {
        3, 1, Vec3 { 1.0f, 2.0f, 3.0f }, Vec3 { 4.0f, 0.0f, 0.0f },
        1, 0, 1.5f, true, SnapshotVisibility::Public });
    s.explosives.push_back(ExplosiveSnapshot {
        4, Vec3 { -2.0f, 1.0f, 2.0f }, 1, 0, 2.25f, 3.5f, SnapshotVisibility::Public });
    s.hazardZones.push_back(HazardZoneSnapshot {
        5, Vec3 { 3.0f, 1.0f, -3.0f }, 2, 1, 4.0f, 2.0f, true, SnapshotVisibility::Public });
    s.heroDevices.push_back(HeroDeviceSnapshot {
        6, HeroDeviceType::KonvoyTrap, Vec3 { 8.0f, 1.0f, 8.0f },
        1, 0, -1, 7.0f, 48, SnapshotVisibility::OwnerTeam });
    s.statusEffects.push_back(StatusEffectSnapshot {
        7, StatusEffectType::Shield, Vec3 { -38.0f, 1.5f, 0.0f },
        1, -1, 0, 3.0f, 2, SnapshotVisibility::Private });
    s.actionResults.push_back(ActionResultSnapshot {
        1,
        7,
        42,
        static_cast<int>(PlayerActionType::BuyItem),
        0,
        -1,
        -1,
        -1,
        0,
        0,
        true,
        Vec3 { 4.0f, 5.0f, 6.0f },
        "Purchased wood.",
        { 128, 238, 166, 255 },
        1.6f,
        0.0f });
    // Phase 4/5 slice: utility / hero ability / projectile owner-private results
    // ride the same generic ActionResultSnapshot fields as BuyItem above (see
    // docs/NETWORK_PREP_PLAN.md) — exercised here so the roundtrip covers the
    // new PlayerActionType values, not just their (already generic) wire shape.
    s.actionResults.push_back(ActionResultSnapshot {
        1,
        8,
        0,
        static_cast<int>(PlayerActionType::UtilityUse),
        2,
        -1,
        -1,
        -1,
        0,
        1,
        true,
        Vec3 { -1.0f, 2.0f, 0.5f },
        "Dash pearl used.",
        { 112, 232, 255, 255 },
        0.30f,
        0.30f });
    s.actionResults.push_back(ActionResultSnapshot {
        1,
        9,
        0,
        static_cast<int>(PlayerActionType::HeroAbility),
        0,
        -1,
        -1,
        -1,
        1,
        32,
        false,
        Vec3 { 3.0f, 1.5f, -2.0f },
        "Radon: active2 on cooldown.",
        { 255, 96, 82, 255 },
        1.7f,
        0.95f });
    s.actionResults.push_back(ActionResultSnapshot {
        1,
        10,
        0,
        static_cast<int>(PlayerActionType::ProjectileLaunch),
        1,
        -1,
        -1,
        -1,
        0,
        1,
        true,
        Vec3 { 0.5f, 1.2f, 3.0f },
        "Bow: critical shot!",
        { 255, 255, 255, 255 },
        1.6f,
        0.0f });
    s.worldEvents.push_back(WorldEventSnapshot {
        1,
        static_cast<int>(WorldEventKind::PlayerDied),
        2,
        1,
        0,
        Vec3 { 1.0f, 1.0f, 1.0f },
        0,
        0,
        0,
        "топором Свидетеля" });

    return s;
}

LobbyUpdate MakeSampleLobbyUpdate()
{
    LobbyUpdate update;
    update.playerName = "Alice";
    update.selectedTeam = 0;
    update.selectedHero = 2;
    update.ready = true;
    update.startRequested = true;
    return update;
}

LobbySnapshot MakeSampleLobbySnapshot()
{
    LobbySnapshot snapshot;
    snapshot.revision = 77;
    snapshot.hostClientId = 11;
    snapshot.serverName = "Private Test";
    snapshot.privateServer = true;
    snapshot.maxPlayers = 8;
    snapshot.teamCount = 4;
    snapshot.maxTeamSize = 2;
    snapshot.heroCount = 6;
    snapshot.enforceUniqueHeroesPerTeam = true;
    snapshot.requireAllReady = true;
    snapshot.canStart = true;
    snapshot.matchStarting = true;
    snapshot.matchStarted = false;
    snapshot.worldBiome = 3;  // distinctive non-defaults so the roundtrip exercises them.
    snapshot.worldLayout = 1;
    snapshot.matchMode = 1;
    snapshot.statusMessage = "all ready";
    snapshot.players.push_back(
        LobbyPlayerState { 11, 1, "Alice", 0, 2, true, true, true });
    snapshot.players.push_back(
        LobbyPlayerState { 12, 2, "Bob", 1, 3, true, true, false });
    return snapshot;
}
} // namespace

int RunProtocolSmoke()
{
    std::cout << "protocol-smoke: version=" << kProtocolVersion
              << " magic=0x" << std::hex << kProtocolMagic << std::dec
              << " headerBytes=" << kPacketHeaderSize << '\n';

    bool ok = true;

    // 1) PlayerCommand roundtrip.
    const PlayerCommand command = MakeSampleCommand();
    const std::vector<std::uint8_t> commandBytes = EncodePlayerCommand(99, command);
    PacketHeader commandHeader;
    PlayerCommand decodedCommand;
    const DecodeStatus commandStatus = DecodePlayerCommand(
        commandBytes.data(), commandBytes.size(), commandHeader, decodedCommand);
    const bool commandOk = commandStatus == DecodeStatus::Ok
        && commandHeader.type == MessageType::PlayerCommand
        && commandHeader.sequence == 99
        && commandHeader.tick == command.tick
        && CommandEqual(command, decodedCommand);
    ok = ok && commandOk;
    std::cout << "protocol-smoke: PlayerCommand bytes=" << commandBytes.size()
              << " status=" << ToString(commandStatus)
              << " seq=" << commandHeader.sequence << " tick=" << commandHeader.tick
              << " roundtrip=" << (commandOk ? "ok" : "FAIL") << '\n';

    PlayerCommand command2 = command;
    command2.tick = command.tick + 1;
    command2.attackPressed = false;
    command2.placePressed = true;
    const std::vector<PlayerCommand> commandBatch { command, command2 };
    const std::vector<std::uint8_t> commandBatchBytes = EncodePlayerCommandBatch(98, commandBatch);
    PacketHeader commandBatchHeader;
    std::vector<PlayerCommand> decodedCommandBatch;
    const DecodeStatus commandBatchStatus = DecodePlayerCommandBatch(
        commandBatchBytes.data(), commandBatchBytes.size(), commandBatchHeader, decodedCommandBatch);
    const bool commandBatchOk = commandBatchStatus == DecodeStatus::Ok
        && commandBatchHeader.type == MessageType::PlayerCommandBatch
        && commandBatchHeader.sequence == 98
        && commandBatchHeader.tick == command2.tick
        && decodedCommandBatch.size() == commandBatch.size()
        && CommandEqual(decodedCommandBatch[0], command)
        && CommandEqual(decodedCommandBatch[1], command2);
    ok = ok && commandBatchOk;
    std::cout << "protocol-smoke: PlayerCommandBatch bytes=" << commandBatchBytes.size()
              << " status=" << ToString(commandBatchStatus)
              << " count=" << decodedCommandBatch.size()
              << " roundtrip=" << (commandBatchOk ? "ok" : "FAIL") << '\n';

    // 2) MatchSnapshot roundtrip (header + players + cores + block deltas).
    const MatchSnapshot snapshot = MakeSampleSnapshot();
    const std::vector<std::uint8_t> snapshotBytes = EncodeMatchSnapshot(100, snapshot);
    PacketHeader snapshotHeader;
    MatchSnapshot decodedSnapshot;
    const DecodeStatus snapshotStatus = DecodeMatchSnapshot(
        snapshotBytes.data(), snapshotBytes.size(), snapshotHeader, decodedSnapshot);
    const bool snapshotOk = snapshotStatus == DecodeStatus::Ok
        && snapshotHeader.type == MessageType::MatchSnapshot
        && snapshotHeader.tick == snapshot.tick
        && SnapshotEqual(snapshot, decodedSnapshot);
    ok = ok && snapshotOk;
    std::cout << "protocol-smoke: MatchSnapshot bytes=" << snapshotBytes.size()
              << " status=" << ToString(snapshotStatus)
              << " players=" << decodedSnapshot.players.size()
              << " cores=" << decodedSnapshot.cores.size()
              << " generators=" << decodedSnapshot.generators.size()
              << " pickups=" << decodedSnapshot.pickups.size()
              << " blockDeltas=" << decodedSnapshot.blockDeltas.size()
              << " projectiles=" << decodedSnapshot.projectiles.size()
              << " roundtrip=" << (snapshotOk ? "ok" : "FAIL") << '\n';

    // 3) Snapshot delta roundtrip + loss/reorder compatibility checks.
    MatchSnapshot deltaTarget = snapshot;
    deltaTarget.tick = snapshot.tick + 3;
    deltaTarget.lastProcessedCommandTick = snapshot.lastProcessedCommandTick + 2;
    deltaTarget.matchTime = snapshot.matchTime + 0.05f;
    deltaTarget.players[0].position.x += 1.25f;
    deltaTarget.players[0].inventory.main[0].count += 1;
    deltaTarget.players.pop_back();
    deltaTarget.matchScores[0].kills += 1;
    deltaTarget.matchScores[0].coreDamage += 9;
    deltaTarget.matchScores.pop_back();
    deltaTarget.cores[0].health -= 25;
    deltaTarget.teamChests[0].resources[0] += 5;
    deltaTarget.teamChests[0].slots[0].count += 5;
    deltaTarget.teamChests[0].slots.push_back(ItemStackSnapshot { 20, 1 });
    deltaTarget.pickups[0].amount += 2;
    deltaTarget.droppedItems.clear();
    BlockDelta d2 = snapshot.blockDeltas.front();
    d2.tick = deltaTarget.tick;
    d2.position.x += 1;
    d2.reason = BlockDeltaReason::ReplicationTest;
    deltaTarget.blockDeltas.push_back(d2);
    deltaTarget.projectiles[0].position.z += 2.0f;
    deltaTarget.explosives[0].remainingTimer -= 0.5f;
    deltaTarget.hazardZones.clear();
    deltaTarget.heroDevices[0].health -= 3;
    deltaTarget.statusEffects[0].remaining -= 0.25f;
    deltaTarget.actionResults[0].actionSeq += 1;
    deltaTarget.actionResults[0].success = false;
    deltaTarget.actionResults[0].message = "Purchase denied.";

    constexpr std::uint32_t kBaselineSequence = 100;
    constexpr std::uint32_t kDeltaSequence = 103;
    const MatchSnapshotDelta delta =
        BuildSnapshotDelta(snapshot, deltaTarget, kBaselineSequence, 1);
    const std::vector<std::uint8_t> deltaBytes = EncodeSnapshotDelta(kDeltaSequence, delta);
    PacketHeader deltaHeader;
    MatchSnapshotDelta decodedDelta;
    const DecodeStatus deltaStatus =
        DecodeSnapshotDelta(deltaBytes.data(), deltaBytes.size(), deltaHeader, decodedDelta);
    MatchSnapshot applied = snapshot;
    std::uint32_t appliedSequence = kBaselineSequence;
    const SnapshotDeltaApplyStatus applyStatus = ApplySnapshotDeltaIfCompatible(
        true, applied, appliedSequence, deltaHeader.sequence, decodedDelta);
    const bool deltaOk = deltaStatus == DecodeStatus::Ok
        && deltaHeader.type == MessageType::SnapshotDelta
        && applyStatus == SnapshotDeltaApplyStatus::Applied
        && appliedSequence == kDeltaSequence
        && SnapshotEqual(applied, deltaTarget)
        && deltaBytes.size() < snapshotBytes.size();
    ok = ok && deltaOk;
    std::cout << "protocol-smoke: SnapshotDelta bytes=" << deltaBytes.size()
              << " fullBytes=" << snapshotBytes.size()
              << " status=" << ToString(deltaStatus)
              << " apply=" << ToString(applyStatus)
              << " changedPlayers=" << decodedDelta.players.size()
              << " removedPlayers=" << decodedDelta.removedPlayerIds.size()
              << " bandwidth=" << (deltaBytes.size() < snapshotBytes.size() ? "ok" : "FAIL")
              << " roundtrip=" << (deltaOk ? "ok" : "FAIL") << '\n';

    MatchSnapshot oldState = applied;
    std::uint32_t oldSequence = appliedSequence;
    const SnapshotDeltaApplyStatus oldStatus = ApplySnapshotDeltaIfCompatible(
        true, oldState, oldSequence, kDeltaSequence - 1, decodedDelta);
    MatchSnapshot missingState;
    std::uint32_t missingSequence = 0;
    const SnapshotDeltaApplyStatus missingStatus = ApplySnapshotDeltaIfCompatible(
        false, missingState, missingSequence, kDeltaSequence, decodedDelta);
    MatchSnapshot gapState = snapshot;
    std::uint32_t gapSequence = kBaselineSequence - 1;
    const SnapshotDeltaApplyStatus gapStatus = ApplySnapshotDeltaIfCompatible(
        true, gapState, gapSequence, kDeltaSequence, decodedDelta);
    const bool deltaLossOk = oldStatus == SnapshotDeltaApplyStatus::OldSnapshot
        && missingStatus == SnapshotDeltaApplyStatus::MissingBaseline
        && gapStatus == SnapshotDeltaApplyStatus::BaselineMismatch
        && !SnapshotDeltaStatusNeedsFullResync(oldStatus)
        && SnapshotDeltaStatusNeedsFullResync(missingStatus)
        && SnapshotDeltaStatusNeedsFullResync(gapStatus);
    ok = ok && deltaLossOk;
    std::cout << "protocol-smoke: delta ordering old=" << ToString(oldStatus)
              << " missing=" << ToString(missingStatus)
              << " gap=" << ToString(gapStatus)
              << " fullResyncRequest=" << (deltaLossOk ? "ok" : "FAIL")
              << " handled=" << (deltaLossOk ? "ok" : "FAIL") << '\n';

    MatchSnapshot branchTarget = deltaTarget;
    branchTarget.tick += 2;
    branchTarget.matchTime += 0.033f;
    branchTarget.players[0].position.x += 0.5f;
    const MatchSnapshotDelta branchDelta =
        BuildSnapshotDelta(snapshot, branchTarget, kBaselineSequence, 1);
    MatchSnapshot currentClientState = applied;
    std::uint32_t currentClientSequence = appliedSequence;
    constexpr std::uint32_t kBranchDeltaSequence = 104;
    const SnapshotDeltaApplyStatus branchCurrentStatus = ApplySnapshotDeltaIfCompatible(
        true, currentClientState, currentClientSequence, kBranchDeltaSequence, branchDelta);
    MatchSnapshot recoveredFromHistory = snapshot;
    std::uint32_t recoveredSequence = kBaselineSequence;
    const SnapshotDeltaApplyStatus branchHistoryStatus = ApplySnapshotDeltaIfCompatible(
        true, recoveredFromHistory, recoveredSequence, kBranchDeltaSequence, branchDelta);
    const bool deltaHistoryOk = branchCurrentStatus == SnapshotDeltaApplyStatus::BaselineMismatch
        && branchHistoryStatus == SnapshotDeltaApplyStatus::Applied
        && recoveredSequence == kBranchDeltaSequence
        && SnapshotEqual(recoveredFromHistory, branchTarget);
    ok = ok && deltaHistoryOk;
    std::cout << "protocol-smoke: delta history recovery current="
              << ToString(branchCurrentStatus)
              << " history=" << ToString(branchHistoryStatus)
              << " handled=" << (deltaHistoryOk ? "ok" : "FAIL") << '\n';

    // 4) Lobby packets roundtrip.
    const LobbyUpdate lobbyUpdate = MakeSampleLobbyUpdate();
    const std::vector<std::uint8_t> lobbyUpdateBytes = EncodeLobbyUpdate(101, lobbyUpdate);
    PacketHeader lobbyUpdateHeader;
    LobbyUpdate decodedLobbyUpdate;
    const DecodeStatus lobbyUpdateStatus = DecodeLobbyUpdate(
        lobbyUpdateBytes.data(), lobbyUpdateBytes.size(), lobbyUpdateHeader, decodedLobbyUpdate);
    const bool lobbyUpdateOk = lobbyUpdateStatus == DecodeStatus::Ok
        && lobbyUpdateHeader.type == MessageType::LobbyUpdate
        && lobbyUpdateHeader.sequence == 101
        && LobbyUpdateEqual(lobbyUpdate, decodedLobbyUpdate);
    ok = ok && lobbyUpdateOk;

    const LobbySnapshot lobbySnapshot = MakeSampleLobbySnapshot();
    const std::vector<std::uint8_t> lobbySnapshotBytes = EncodeLobbySnapshot(102, lobbySnapshot);
    PacketHeader lobbySnapshotHeader;
    LobbySnapshot decodedLobbySnapshot;
    const DecodeStatus lobbySnapshotStatus = DecodeLobbySnapshot(
        lobbySnapshotBytes.data(), lobbySnapshotBytes.size(), lobbySnapshotHeader, decodedLobbySnapshot);
    const bool lobbySnapshotOk = lobbySnapshotStatus == DecodeStatus::Ok
        && lobbySnapshotHeader.type == MessageType::LobbySnapshot
        && lobbySnapshotHeader.tick == lobbySnapshot.revision
        && SnapshotEqual(lobbySnapshot, decodedLobbySnapshot);
    ok = ok && lobbySnapshotOk;
    std::cout << "protocol-smoke: LobbyUpdate bytes=" << lobbyUpdateBytes.size()
              << " status=" << ToString(lobbyUpdateStatus)
              << " roundtrip=" << (lobbyUpdateOk ? "ok" : "FAIL") << '\n';
    std::cout << "protocol-smoke: LobbySnapshot bytes=" << lobbySnapshotBytes.size()
              << " status=" << ToString(lobbySnapshotStatus)
              << " players=" << decodedLobbySnapshot.players.size()
              << " roundtrip=" << (lobbySnapshotOk ? "ok" : "FAIL") << '\n';

    // 4) Truncated packets must be rejected, never crash. Try every cut length.
    bool truncationSafe = true;
    for (std::size_t cut = 0; cut < snapshotBytes.size(); ++cut)
    {
        PacketHeader h;
        MatchSnapshot partial;
        const DecodeStatus s = DecodeMatchSnapshot(snapshotBytes.data(), cut, h, partial);
        if (s == DecodeStatus::Ok)
        {
            truncationSafe = false; // a short buffer must never decode as Ok
            break;
        }
    }
    // Also a zero-length and a sub-header buffer.
    {
        PacketHeader h;
        PlayerCommand pc;
        const DecodeStatus z = DecodePlayerCommand(nullptr, 0, h, pc);
        const DecodeStatus tiny = DecodePlayerCommand(commandBytes.data(), 3, h, pc);
        truncationSafe = truncationSafe && z == DecodeStatus::TooShort && tiny == DecodeStatus::TooShort;
    }
    ok = ok && truncationSafe;
    std::cout << "protocol-smoke: truncation rejected=" << (truncationSafe ? "ok" : "FAIL") << '\n';

    // 5) Version mismatch: corrupt the version field (bytes 4..5) and decode.
    std::vector<std::uint8_t> versioned = commandBytes;
    versioned[4] = static_cast<std::uint8_t>((kProtocolVersion + 1) & 0xFF);
    versioned[5] = static_cast<std::uint8_t>(((kProtocolVersion + 1) >> 8) & 0xFF);
    PacketHeader versionHeader;
    PlayerCommand ignored;
    const DecodeStatus versionStatus = DecodePlayerCommand(
        versioned.data(), versioned.size(), versionHeader, ignored);
    const bool versionOk = versionStatus == DecodeStatus::VersionMismatch
        && versionHeader.protocolVersion == kProtocolVersion + 1;
    ok = ok && versionOk;
    std::cout << "protocol-smoke: versionMismatch status=" << ToString(versionStatus)
              << " wireVersion=" << versionHeader.protocolVersion
              << " handled=" << (versionOk ? "ok" : "FAIL") << '\n';

    // 6) Bad magic / random bytes: rejected cleanly.
    const std::uint8_t garbage[] { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05 };
    PacketHeader garbageHeader;
    PlayerCommand garbageOut;
    const DecodeStatus garbageStatus = DecodePlayerCommand(
        garbage, sizeof(garbage), garbageHeader, garbageOut);
    const bool garbageOk = garbageStatus == DecodeStatus::BadMagic;
    ok = ok && garbageOk;
    std::cout << "protocol-smoke: badMagic status=" << ToString(garbageStatus)
              << " handled=" << (garbageOk ? "ok" : "FAIL") << '\n';

    // 7) Wrong-type: decode a command packet as a snapshot.
    PacketHeader wrongHeader;
    MatchSnapshot wrongOut;
    const DecodeStatus wrongStatus = DecodeMatchSnapshot(
        commandBytes.data(), commandBytes.size(), wrongHeader, wrongOut);
    const bool wrongOk = wrongStatus == DecodeStatus::WrongType;
    ok = ok && wrongOk;
    std::cout << "protocol-smoke: wrongType status=" << ToString(wrongStatus)
              << " handled=" << (wrongOk ? "ok" : "FAIL") << '\n';

    // 8) PacketFragment codec: chunk a synthetic oversized packet, decode each
    // fragment, reassemble by the fixed layout, and compare byte-for-byte.
    bool fragmentOk = true;
    {
        std::vector<std::uint8_t> big(3000);
        for (std::size_t i = 0; i < big.size(); ++i)
        {
            big[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
        }
        constexpr std::size_t chunkBytes = 1024;
        const std::uint16_t count =
            static_cast<std::uint16_t>((big.size() + chunkBytes - 1) / chunkBytes);
        std::vector<std::uint8_t> reassembled(big.size());
        for (std::uint16_t i = 0; i < count && fragmentOk; ++i)
        {
            const std::size_t offset = static_cast<std::size_t>(i) * chunkBytes;
            const std::size_t len = big.size() - offset < chunkBytes
                ? big.size() - offset
                : chunkBytes;
            const std::vector<std::uint8_t> wire = EncodePacketFragment(
                900 + i, /*fragmentId*/ 777, i, count,
                static_cast<std::uint32_t>(big.size()), big.data() + offset, len);
            PacketHeader fragHeader;
            std::uint32_t fragmentId = 0;
            std::uint16_t index = 0;
            std::uint16_t total = 0;
            std::uint32_t totalSize = 0;
            std::vector<std::uint8_t> chunk;
            const DecodeStatus status = DecodePacketFragment(
                wire.data(), wire.size(), fragHeader, fragmentId, index, total, totalSize, chunk);
            fragmentOk = fragmentOk
                && status == DecodeStatus::Ok
                && fragmentId == 777
                && index == i
                && total == count
                && totalSize == big.size()
                && chunk.size() == len;
            if (fragmentOk)
            {
                std::copy(chunk.begin(), chunk.end(),
                          reassembled.begin() + static_cast<std::ptrdiff_t>(offset));
            }
        }
        fragmentOk = fragmentOk && reassembled == big;
        // A truncated fragment must be rejected, not crash.
        const std::vector<std::uint8_t> wire = EncodePacketFragment(
            1, 2, 0, 1, 8, big.data(), 8);
        PacketHeader truncHeader;
        std::uint32_t a = 0;
        std::uint16_t b = 0;
        std::uint16_t c = 0;
        std::uint32_t d = 0;
        std::vector<std::uint8_t> e;
        fragmentOk = fragmentOk
            && DecodePacketFragment(wire.data(), wire.size() - 4, truncHeader, a, b, c, d, e)
                != DecodeStatus::Ok;
    }
    ok = ok && fragmentOk;
    std::cout << "protocol-smoke: packetFragment roundtrip=" << (fragmentOk ? "ok" : "FAIL") << '\n';

    std::cout << (ok ? "PROTOCOL_SMOKE_OK" : "PROTOCOL_SMOKE_FAIL") << std::endl;
    return ok ? 0 : 6;
}

int Game::RunBotAISmoke()
{
    int failures = 0;
    const auto check = [&failures](const char* name, bool passed, const char* reason)
    {
        std::cout << "BOT_AI " << name << " ticks=1 " << (passed ? "OK" : "FAIL");
        if (!passed) std::cout << " reason=\"" << reason << "\"";
        std::cout << '\n';
        if (!passed) ++failures;
    };

    TeamCoordinationBus bus;
    bus.Broadcast(11, CoordinationSignal::CallingForHelp, Vector3 { 2, 0, 2 }, 10.0f, 0);
    bus.Broadcast(12, CoordinationSignal::DefendingCore, Vector3 { 0, 0, 0 }, 10.0f, 0);
    check("01_open_own_core", bus.reserveDefenderId == 12 && bus.defenseRequestTimestamp == 10.0f,
          "defense request did not retain a reserve defender");

    bus.Broadcast(13, CoordinationSignal::AttackingCore, Vector3 { 20, 0, 4 }, 11.0f, 2);
    check("02_ally_core_cover", bus.pressureTeamId == 2 && bus.assistActorId == 13,
          "attack request was not shared with allies");

    const auto routeCost = [](float distance, float danger, float caution, int cargo)
    {
        return distance + danger * (1.0f + caution * 2.0f) + static_cast<float>(cargo) * danger * 0.035f;
    };
    const bool safeRoute = routeCost(18.0f, 1.0f, 0.85f, 25) < routeCost(9.0f, 5.0f, 0.85f, 25);
    check("03_short_dangerous_vs_safe", safeRoute, "cautious loaded bot selected dangerous shortcut");

    BotMemory rich;
    rich.carriedResourceValue = 48;
    rich.cautionTrait = 0.72f;
    rich.intent = rich.carriedResourceValue >= 30 ? BotIntent::RetreatHome : BotIntent::FightEnemy;
    check("04_loaded_bot_encounter", rich.intent == BotIntent::RetreatHome,
          "high-value inventory did not trigger disengagement");

    rich.hasLastRouteFailure = true;
    rich.repeatedRouteFailures = 2;
    rich.routeFailureCooldown = 24.0f;
    rich.abandonedPlanTargetTeamId = 1;
    check("05_repeat_bridge_attack", rich.routeFailureCooldown > 0.0f && rich.repeatedRouteFailures >= 2,
          "failed route was immediately reusable");

    StrategicPlan repair;
    repair.goal = StrategicGoal::BaseDefense;
    repair.stage = 1;
    repair.stageCount = 3;
    repair.minimumCommitSeconds = 4.0f;
    check("06_broken_core_shell", repair.goal == StrategicGoal::BaseDefense && repair.stageCount >= 3,
          "repair response is not a staged committed plan");

    const float exposedCoreValue = 140.0f;
    const float incidentalFightValue = 52.0f;
    check("07_exposed_enemy_core", exposedCoreValue > incidentalFightValue,
          "incidental duel displaced an exposed Core objective");

    StrategicPlan failedPush;
    failedPush.goal = StrategicGoal::Idle;
    rich.abandonedPlanCooldown = 18.0f;
    check("08_failed_attack_reroute", failedPush.goal == StrategicGoal::Idle && rich.abandonedPlanCooldown > 0.0f,
          "failed plan did not enter cancellation cooldown");

    bus.Broadcast(14, CoordinationSignal::BuildingBridge, Vector3 { -12, 3, 25 }, 12.0f, 2);
    check("09_flank_opportunity", bus.sharedAttackRoute.x < 0.0f && bus.pressureTeamId == 2,
          "alternate route was not made legible to the team");

    BotControlArbiter arbiter;
    BotControlProposal navigation;
    navigation.active = true;
    navigation.source = BotControlSource::Navigation;
    navigation.priority = BotControlPriority::Navigation;
    navigation.domains = BotControlDomain::Movement;
    navigation.command.moveForward = 1.0f;
    arbiter.Submit(navigation);
    BotControlProposal emergency;
    emergency.active = true;
    emergency.source = BotControlSource::Emergency;
    emergency.priority = BotControlPriority::Emergency;
    emergency.domains = BotControlDomain::All;
    emergency.exclusiveDomains = BotControlDomain::All;
    emergency.command.placeHeld = true;
    emergency.command.sneak = true;
    arbiter.Submit(emergency);
    const BotControlResolution saved = arbiter.Resolve(PlayerCommand {});
    check("10_emergency_bridge", saved.actionOwner.source == BotControlSource::Emergency
          && saved.command.placeHeld && saved.command.sneak,
          "emergency placement did not override ordinary movement");

    StrategicPlan leading;
    leading.allowedRisk = 0.32f;
    leading.minimumCommitSeconds = 3.0f;
    check("11_leader_overrisk", leading.allowedRisk < 0.5f && leading.minimumCommitSeconds > 0.0f,
          "leader plan lacks a risk cap");

    StrategicPlan underdog;
    underdog.goal = StrategicGoal::CoreAssault;
    underdog.expectedValue = 180.0f;
    underdog.allowedRisk = 0.82f;
    underdog.stageCount = 4;
    check("12_underdog_desperate_plan", underdog.goal == StrategicGoal::CoreAssault
          && underdog.allowedRisk > leading.allowedRisk && underdog.stageCount >= 3,
          "underdog did not unlock a coherent higher-risk plan");

    bus.Clear();
    bus.Broadcast(21, CoordinationSignal::AttackingCore, Vector3 {}, 20.0f, 3);
    bus.Broadcast(22, CoordinationSignal::AttackingCore, Vector3 {}, 20.0f, 3);
    const int reservations = bus.CountSignal(CoordinationSignal::AttackingCore, 20.5f, 2.25f, 3);
    check("13_target_reservation", reservations == 2 && reservations < 4,
          "all allies were allowed to dogpile one target");

    PlayerCommand heroCommand;
    heroCommand.useAbility1 = true;
    heroCommand.controlledPlayerId = 31;
    check("14_meaningful_hero_command", heroCommand.useAbility1 && heroCommand.controlledPlayerId == 31,
          "hero action was not representable as authoritative PlayerCommand");

    const auto personality = [](std::uint32_t seed, int actor)
    {
        std::uint32_t x = seed ^ (static_cast<std::uint32_t>(actor) * 0x9e3779b9u);
        x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
        return x;
    };
    const std::uint32_t sameA = personality(7001u, 9);
    const std::uint32_t sameB = personality(7001u, 9);
    check("15_same_seed_determinism", sameA == sameB, "same seed produced different policy identity");

    const std::uint32_t varied = personality(7002u, 9);
    check("16_seed_variation", varied != sameA && varied != 0u,
          "different seed failed to produce bounded policy variation");

    const BotStrategyProfile savedProfile = botStrategyProfile_;
    botStrategyProfile_ = BotStrategyProfile::Standard;
    const BotTuningGenome standardStrategy = ScaledBotTuningForTeam(0);
    botStrategyProfile_ = BotStrategyProfile::HypixelRush;
    const BotTuningGenome rushStrategy = ScaledBotTuningForTeam(0);
    botStrategyProfile_ = savedProfile;
    check("17_hypixel_rush_profile",
          rushStrategy.earlyEconomySeconds < standardStrategy.earlyEconomySeconds
              && rushStrategy.allInSeconds < standardStrategy.allInSeconds
              && rushStrategy.attackSlotBonus > standardStrategy.attackSlotBonus
              && rushStrategy.strategicEconomyBonus < standardStrategy.strategicEconomyBonus,
          "Hypixel Rush did not accelerate pressure and reduce economy bias");

    std::cout << (failures == 0 ? "BOT_AI_SMOKE_OK" : "BOT_AI_SMOKE_FAIL")
              << " failures=" << failures << " scenarios=17 maxTicksPerScenario=1" << std::endl;
    return failures == 0 ? 0 : 7;
}

#else // DAIBED_DIAGNOSTICS == 0: stable CLI surface, no test logic in the binary.

namespace
{
int DiagnosticsDisabled()
{
    std::cout << "diagnostics are disabled in this build (DAIBED_DIAGNOSTICS=OFF)" << std::endl;
    return 100;
}
}

int Game::RunNetworkSmoke() { return DiagnosticsDisabled(); }
int Game::RunMovementParitySmoke() { return DiagnosticsDisabled(); }
int Game::RunNetworkPurchaseSmoke() { return DiagnosticsDisabled(); }
int Game::RunNetworkClientUiSmoke() { return DiagnosticsDisabled(); }
int Game::RunIntegratedServerSmoke() { return DiagnosticsDisabled(); }
int Game::RunCreativeSmoke() { return DiagnosticsDisabled(); }
int Game::RunLoopbackTwoClientSmoke() { return DiagnosticsDisabled(); }
int Game::RunMultiplayerLoopbackSmoke() { return DiagnosticsDisabled(); }
int Game::RunClientGuiSmoke() { return DiagnosticsDisabled(); }
int Game::RunClientInputSmoke() { return DiagnosticsDisabled(); }
int Game::RunNetworkRangedSmoke() { return DiagnosticsDisabled(); }
int Game::RunNetworkActionsSmoke() { return DiagnosticsDisabled(); }
int Game::RunLagCompSmoke() { return DiagnosticsDisabled(); }
int Game::RunClientDynamicApplySmoke() { return DiagnosticsDisabled(); }
int Game::RunBotAISmoke() { return DiagnosticsDisabled(); }
void Game::PrepareStartupSmoke() {}
void Game::ExerciseStartupSmokeMutation(bool) {}
int Game::RunUiScreenshotDiag() { return DiagnosticsDisabled(); }
int Game::RunFrameProfileDiag() { return DiagnosticsDisabled(); }
int Game::RunMapReviewDiag(const std::string&) { return DiagnosticsDisabled(); }
int RunProtocolSmoke() { return DiagnosticsDisabled(); }

#endif // DAIBED_DIAGNOSTICS

#include "Game.h"

#include "CrashLogger.h"
#include "HeroSystem.h"
#include "RangedCombat.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{
constexpr float kPi = 3.1415926535f;
// Sudden death: cores collapse late so stalemates always resolve into a
// final-life brawl instead of dragging on forever.
constexpr float kCoreCollapseSeconds = 12.0f * 60.0f;
constexpr float kRenderScales[] { 0.60f, 0.75f, 0.85f, 1.00f };
constexpr float kDrawDistances[] { 64.0f, 96.0f, 150.0f, 220.0f };
constexpr float kItemPickupRadiusSq = 1.35f;
constexpr float kItemMagnetRadius = 2.35f;
constexpr float kItemMagnetRadiusSq = kItemMagnetRadius * kItemMagnetRadius;
constexpr float kItemMergeRadiusSq = 0.85f * 0.85f;
constexpr float kItemMergeInterval = 0.5f;
constexpr float kResourceMagnetSpeed = 5.8f;
constexpr float kDroppedItemMagnetAccel = 28.0f;
constexpr float kDroppedItemMagnetMaxSpeed = 7.0f;
constexpr int kBuildMinY = -2;
constexpr int kBuildMaxY = 64;
constexpr int kBuildMapRadius = 72;
constexpr float kRadonBaseRadiusSq = 105.0f;
constexpr float kRadonSacrificeRespawnSeconds = 7.0f;
constexpr float kOrbitaMomentumSpeed = 7.2f;
constexpr float kOrbitaDashImpulse = 9.4f;
constexpr float kOrbitaDashLift = 2.55f;
constexpr float kOrbitaTeleportMaxDistance = 20.0f;
constexpr float kOrbitaEnemyCoreRestrictionSq = 16.0f;
constexpr float kOrbitaTeleportDamagePerBlock = 1.65f;
constexpr int kBromVacuumIronCost = 48;
constexpr int kBromTurretGoldCost = 12;
constexpr int kBromVacuumCapacity = 24;
constexpr float kBromVacuumStepHeight = 1.08f;
constexpr float kBromVacuumDropHeight = 1.15f;
constexpr float kBromTurretAttackRange = 16.0f;
constexpr float kBromUltimateCooldownSeconds = 70.0f;
constexpr float kBromUltimateDeviceLifetime = 90.0f;
constexpr int kKonvoyMaxTraps = 2;
constexpr float kKonvoyHandcuffRadius = 6.0f;
constexpr float kKonvoyDomeVisualRadius = 6.0f;
constexpr Vector3 kPlayerCollisionHalfExtents { 0.36f, 0.95f, 0.36f };

struct WindowResolution
{
    int width = 1280;
    int height = 720;
    const char* label = "1280x720";
};

struct FpsLimitOption
{
    int fps = 60;
    const char* label = "60";
};

constexpr WindowResolution kWindowResolutions[] {
    { 1024, 576, "1024x576" },
    { 1280, 720, "1280x720" },
    { 1600, 900, "1600x900" },
    { 1920, 1080, "1920x1080" },
    { 2560, 1440, "2560x1440" }
};

constexpr FpsLimitOption kFpsLimits[] {
    { 30, "30 FPS" },
    { 60, "60 FPS" },
    { 120, "120 FPS" },
    { 144, "144 FPS" },
    { 240, "240 FPS" },
    { 0, "Без ограничений" }
};

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}


float Length2D(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.z * value.z);
}

float Dot2D(Vector3 a, Vector3 b)
{
    return a.x * b.x + a.z * b.z;
}

Vector3 Normalize2D(Vector3 value)
{
    const float length = Length2D(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, 0.0f, value.z / length };
}

float Length(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

Vector3 Normalize(Vector3 value)
{
    const float length = Length(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, value.y / length, value.z / length };
}

float YawFromDirection(Vector3 direction)
{
    return std::atan2(direction.x, -direction.z);
}

std::string BoolCoreState(bool alive)
{
    return alive ? "Core online." : "Core destroyed. Final Life!";
}

Color HeroAccentColor(HeroId id)
{
    switch (id)
    {
    case HeroId::Radon:
        return Color { 92, 164, 255, 255 };
    case HeroId::Orbita:
        return Color { 255, 96, 82, 255 };
    case HeroId::Brom:
        return Color { 96, 202, 118, 255 };
    case HeroId::Konvoy:
        return Color { 92, 210, 255, 255 };
    case HeroId::Likho:
        return Color { 104, 238, 92, 255 };
    case HeroId::Svidetel:
        return Color { 180, 104, 255, 255 };
    }
    return WHITE;
}

std::string FormatTenths(float value)
{
    const int tenths = static_cast<int>(value * 10.0f + 0.5f);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}


}

bool Game::Initialize(bool headless)
{
    headless_ = headless;
    suppressLocalFeedback_ = headless_;
    LoadSettings();
    LoadBotTuning();
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);

    if (headless_)
    {
        return true;
    }

    unsigned int windowFlags = FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE;
    if (vsyncEnabled_)
    {
        windowFlags |= FLAG_VSYNC_HINT;
    }
    SetConfigFlags(windowFlags);
    resolutionIndex_ = std::clamp(resolutionIndex_, 0, static_cast<int>(std::size(kWindowResolutions)) - 1);
    const WindowResolution& resolution = kWindowResolutions[resolutionIndex_];
    InitWindow(resolution.width, resolution.height, "DaiBed " DAIBED_VERSION);
    if (!IsWindowReady())
    {
        return false;
    }

    ApplyWindowSettings();
    ApplyFrameRateLimit();
    SetExitKey(KEY_NULL);
    EnableCursor();
    const bool audioReady = audio_.Initialize();
    audio_.SetVolume(masterVolume_);
    audio_.SetCategoryVolumes(sfxVolume_, ambientVolume_);
    music_.Initialize(audioReady);
    music_.SetVolume(masterVolume_, musicVolume_);
    renderer_.Initialize();
    renderer_.SetWorldRenderDistance(kDrawDistances[drawDistanceIndex_]);
    renderer_.SetShadowQuality(shadowQuality_);
    postProcessor_.Initialize();

    network_.Start();
    network_.Connect();
    SetMessage("Choose a mode and start the match.", 4.0f);
    return true;
}

void Game::Shutdown()
{
    if (headless_)
    {
        return;
    }

    SaveSettings();
    network_.Disconnect();
    network_.Stop();
    postProcessor_.Shutdown();
    renderer_.Shutdown();
    music_.Shutdown();
    audio_.Shutdown();
    CloseWindow();
}

bool Game::ShouldClose() const
{
    return exitRequested_;
}

void Game::SetSelectedBiome(ArenaBiome biome)
{
    arenaBiome_ = biome;
}

void Game::SetBotDifficulty(BotDifficulty difficulty)
{
    botDifficulty_ = difficulty;
}

void Game::SetBotTuningPath(std::string path)
{
    if (path.empty())
    {
        return;
    }

    botTuningPath_ = std::move(path);
    LoadBotTuning();
}

void Game::SetAutomatchStatsPath(std::string path)
{
    if (!path.empty())
    {
        automatchStatsPath_ = std::move(path);
    }
}

void Game::SetProfilingEnabled(bool enabled)
{
    profilingEnabled_ = enabled;
}

void Game::HandleInput()
{
    if (screen_ == GameScreen::MainMenu)
    {
        HandleMenuInput();
        return;
    }
    if (screen_ == GameScreen::HeroSelect)
    {
        HandleHeroSelectInput();
        return;
    }
    if (screen_ == GameScreen::Settings)
    {
        HandleSettingsInput();
        return;
    }
    if (screen_ == GameScreen::Controls)
    {
        HandleControlsInput();
        return;
    }
    if (screen_ == GameScreen::Paused)
    {
        HandlePauseInput();
        return;
    }

    currentInput_ = input_.Poll();
    scoreboardHeld_ = IsKeyDown(KEY_TAB);

    Player* localPlayer = GetLocalPlayer();
    if (localPlayer == nullptr)
    {
        return;
    }

    if (winnerTeamId_.has_value())
    {
        if (currentInput_.restartPressed)
        {
            SetupMatch();
            UpdateCamera(0.016f);
            SetMessage("New match started. Protect your EnergyCore.", 3.0f);
        }
        else if (currentInput_.exitPressed)
        {
            screen_ = GameScreen::MainMenu;
            shopOpen_ = false;
            inventoryOpen_ = false;
            spectatorMode_ = false;
            EnableCursor();
        }
        currentInput_ = PlayerInput {};
        return;
    }

    if (spectatorMode_)
    {
        if (currentInput_.exitPressed)
        {
            screen_ = GameScreen::Paused;
            pauseIndex_ = 0;
            EnableCursor();
            return;
        }

        cameraController_.AddLook(currentInput_.yawDelta, currentInput_.pitchDelta);
        if (currentInput_.cameraTogglePressed)
        {
            cameraController_.ToggleMode();
            SetMessage(std::string("Spectator camera: ") + cameraController_.GetModeName(), 1.6f);
        }
#if DAIBED_DEVELOPER_BUILD
        if (currentInput_.botDebugPressed)
        {
            showBotDebug_ = !showBotDebug_;
            SetMessage(std::string("Bot debug: ") + (showBotDebug_ ? "on" : "off"), 1.4f);
        }
#endif
        currentInput_ = PlayerInput {};
        return;
    }

    if (inventoryOpen_)
    {
        if (currentInput_.exitPressed || currentInput_.inventoryPressed)
        {
            if (!heldInventoryStack_.IsEmpty())
            {
                localPlayer->GetInventory().AddItem(heldInventoryStack_.type, heldInventoryStack_.count);
            }
            inventoryOpen_ = false;
            CloseChest();
            heldInventoryStack_ = ItemStack {};
            DisableCursor();
            currentInput_ = PlayerInput {};
            return;
        }
        HandleInventoryInput(*localPlayer);
        currentInput_ = PlayerInput {};
        return;
    }

    if (currentInput_.exitPressed)
    {
        screen_ = GameScreen::Paused;
        pauseIndex_ = 0;
        EnableCursor();
        return;
    }

    const bool heroAbilityPressed = currentInput_.heroActive1Pressed
        || currentInput_.heroActive2Pressed
        || currentInput_.heroUltimatePressed;
    const bool keepUltimateKeyForShop = currentInput_.heroUltimatePressed && IsLocalPlayerInShopZone();
    if (!shopOpen_ && localPlayer->IsAlive() && heroAbilityPressed && !keepUltimateKeyForShop)
    {
        const KeyBindings& bindings = input_.GetBindings();
        const auto heroInputUsesKey = [&bindings, this](int key)
        {
            return (currentInput_.heroActive1Pressed && bindings.heroActive1 == key)
                || (currentInput_.heroActive2Pressed && bindings.heroActive2 == key)
                || (currentInput_.heroUltimatePressed && bindings.heroUltimate == key);
        };

        UseHeroAbilityInputs(*localPlayer);
        if (heroInputUsesKey(bindings.drop))
        {
            currentInput_.dropPressed = false;
        }
        if (heroInputUsesKey(bindings.inventory))
        {
            currentInput_.inventoryPressed = false;
        }
        if (heroInputUsesKey(bindings.interact))
        {
            currentInput_.interactPressed = false;
        }
    }

    if (currentInput_.inventoryPressed && !shopOpen_)
    {
        inventoryOpen_ = true;
        shopOpen_ = false;
        CloseChest();
        inventoryCursorSlot_ = selectedHotbarSlot_;
        EnableCursor();
        currentInput_ = PlayerInput {};
        return;
    }

    if (!shopOpen_)
    {
        cameraController_.AddLook(currentInput_.yawDelta, currentInput_.pitchDelta);
        localPlayer->SetYaw(cameraController_.GetYaw());
    }

    if (!shopOpen_ && currentInput_.cameraTogglePressed)
    {
        cameraController_.ToggleMode();
        SetMessage(std::string("Camera: ") + cameraController_.GetModeName(), 1.6f);
    }
#if DAIBED_DEVELOPER_BUILD
    if (currentInput_.botDebugPressed)
    {
        showBotDebug_ = !showBotDebug_;
        SetMessage(std::string("Bot debug: ") + (showBotDebug_ ? "on" : "off"), 1.4f);
    }
#endif

    if (!shopOpen_ && currentInput_.dropPressed)
    {
        const ItemStack stack = localPlayer->GetInventory().GetHotbarSlots()[selectedHotbarSlot_];
        if (!stack.IsEmpty())
        {
            const int amount = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? stack.count : 1;
            TryDropInventoryStack(*localPlayer, selectedHotbarSlot_, amount);
        }
    }

    if (currentInput_.interactPressed)
    {
        inventoryOpen_ = false;
        heldInventoryStack_ = ItemStack {};
        CloseChest();
        shopOpen_ = IsLocalPlayerInShopZone() ? !shopOpen_ : false;
        if (shopOpen_)
        {
            EnableCursor();
        }
        else
        {
            DisableCursor();
        }
    }

    if (shopOpen_ && (currentInput_.mouseWheel < -0.01f || currentInput_.middlePressed))
    {
        shopCategoryIndex_ = (shopCategoryIndex_ + 1) % shop_.GetCategoryCount();
    }
    if (shopOpen_ && currentInput_.mouseWheel > 0.01f)
    {
        shopCategoryIndex_ = (shopCategoryIndex_ + shop_.GetCategoryCount() - 1) % shop_.GetCategoryCount();
    }

    bool shopCategoryClicked = false;
    if (shopOpen_ && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        const Vector2 mouse = GetMousePosition();
        const int panelWidth = 790;
        const int panelHeight = 420;
        const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
        const int panelY = GetScreenHeight() / 2 - panelHeight / 2;
        const int categoryCount = shop_.GetCategoryCount();
        const int tabWidth = 118;
        const int tabHeight = 26;
        const int tabY = panelY + 52;
        bool clickedCategory = false;
        for (int category = 0; category < categoryCount; ++category)
        {
            const Rectangle tab {
                static_cast<float>(panelX + 24 + category * (tabWidth + 8)),
                static_cast<float>(tabY),
                static_cast<float>(tabWidth),
                static_cast<float>(tabHeight)
            };
            if (CheckCollisionPointRec(mouse, tab))
            {
                shopCategoryIndex_ = category;
                clickedCategory = true;
                break;
            }
        }
        if (clickedCategory)
        {
            shopCategoryClicked = true;
            currentInput_.shopChoice = 0;
        }
    }

    if (shopOpen_ && !shopCategoryClicked && (currentInput_.shopChoice > 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)))
    {
        Team* team = FindTeam(localPlayer->GetTeamId());
        if (team != nullptr)
        {
            const std::vector<ShopItem> items = shop_.GetItemsForCategory(shopCategoryIndex_);
            int row = currentInput_.shopChoice - 1;
            if (row < 0)
            {
                const Vector2 mouse = GetMousePosition();
                const int panelWidth = 790;
                const int panelHeight = 420;
                const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
                const int panelY = GetScreenHeight() / 2 - panelHeight / 2;
                const int localY = static_cast<int>(mouse.y) - (panelY + 104);
                if (mouse.x >= panelX + 22 && mouse.x <= panelX + panelWidth - 22 && localY >= -6)
                {
                    row = (localY + 6) / 34;
                }
            }
            std::string purchaseMessage;
            const int repeat = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 4 : 1;
            const bool bought = row >= 0 && row < static_cast<int>(items.size())
                ? TryShopPurchase(*localPlayer, *team, items[row].choice, repeat, purchaseMessage)
                : false;
            if (purchaseMessage.empty())
            {
                purchaseMessage = "No shop item in that row.";
            }
            SetMessage(purchaseMessage);
            AddEventMessage(purchaseMessage, bought ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 });
            if (bought)
            {
                audio_.PlayPurchase();
            }
            else
            {
                audio_.PlayDenied();
            }
        }
    }
    else if (!shopOpen_ && currentInput_.hotbarSlot > 0)
    {
        const int slot = currentInput_.hotbarSlot - 1;
        if (slot >= 0 && slot < kHotbarSlotCount)
        {
            selectedHotbarSlot_ = slot;
            const ItemStack stack = localPlayer->GetInventory().GetHotbarSlots()[slot];
            if (stack.IsEmpty())
            {
                SetMessage("Selected empty slot.", 1.0f);
            }
            else
            {
                SetMessage(std::string("Selected ") + ItemDisplayName(stack.type) + ".", 1.2f);
            }
        }
    }
    else if (!shopOpen_ && std::fabs(currentInput_.mouseWheel) > 0.01f)
    {
        if (IsSniperScopeRequested(*localPlayer))
        {
            sniperMagnification_ = std::clamp(
                sniperMagnification_ + currentInput_.mouseWheel * 0.25f,
                1.5f,
                3.0f);
            SetMessage("Оптика: x" + FormatTenths(sniperMagnification_), 0.7f);
        }
        else
        {
            const int direction = currentInput_.mouseWheel > 0.0f ? -1 : 1;
            selectedHotbarSlot_ = (selectedHotbarSlot_ + direction + kHotbarSlotCount) % kHotbarSlotCount;
            const ItemStack stack = localPlayer->GetInventory().GetHotbarSlots()[selectedHotbarSlot_];
            if (!stack.IsEmpty())
            {
                SetMessage(std::string("Selected ") + ItemDisplayName(stack.type) + ".", 0.8f);
            }
        }
    }

#if DAIBED_DEVELOPER_BUILD
    if (currentInput_.debugRespawnPressed)
    {
        Team* team = FindTeam(localPlayer->GetTeamId());
        if (team != nullptr && team->coreAlive && !localPlayer->IsEliminated())
        {
            localPlayer->RespawnAtHome();
            SetMessage("Debug respawn.");
        }
    }
#endif

    if (currentInput_.placePressed)
    {
        UseSelectedItem(*localPlayer);
        fastPlaceTimer_ = currentInput_.bridgeMode ? 0.16f : 0.22f;
    }
}

void Game::Update(float dt)
{
    dt = std::min(dt, 0.05f);

    if (!headless_)
    {
        const Camera3D& audioCamera = cameraController_.GetCamera();
        const Vector3 forward = Normalize(Vector3 {
            audioCamera.target.x - audioCamera.position.x,
            audioCamera.target.y - audioCamera.position.y,
            audioCamera.target.z - audioCamera.position.z
        });
        audio_.SetListener(audioCamera.position, Vector3 { -forward.z, 0.0f, forward.x });
        MusicMood mood = MusicMood::Menu;
        if (winnerTeamId_.has_value())
        {
            mood = MusicMood::Victory;
        }
        else if (screen_ == GameScreen::Playing || screen_ == GameScreen::Paused)
        {
            mood = matchTime_ >= kCoreCollapseSeconds - 90.0f ? MusicMood::Intense : MusicMood::Match;
        }
        music_.Update(mood);
    }

    if (screen_ != GameScreen::Playing)
    {
        return;
    }

    const int maxTicks = headless_ ? 1024 : 32;
    const int ticks = automatch_.active ? std::clamp(automatchTicksPerFrame_, 1, maxTicks) : 1;
    for (int i = 0; i < ticks; ++i)
    {
        const auto started = profilingEnabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
        UpdateMatchSimulation(dt);
        if (profilingEnabled_)
        {
            profileSimulationMs_ += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            ++profileSimulationTicks_;
        }
        if (!automatch_.active)
        {
            break;
        }
    }
}

void Game::UpdateMatchSimulation(float dt)
{
    if (!players_.empty())
    {
        simulationOrderOffset_ = (simulationOrderOffset_ + 1) % players_.size();
    }
    for (Player& player : players_)
    {
        player.UpdateTimers(dt);
    }
    UpdateHeroPassives(dt);

    if (!winnerTeamId_.has_value())
    {
        matchTime_ += dt;
        if (automatch_.active)
        {
            if (!coreCollapseTriggered_ && !coreCollapseWarned_ && matchTime_ >= kCoreCollapseSeconds - 60.0f)
            {
                coreCollapseWarned_ = true;
                AddKillFeed("Sudden death через 60 секунд", Color { 255, 118, 118, 255 }, 8.0f);
            }
            if (!coreCollapseTriggered_ && matchTime_ >= kCoreCollapseSeconds)
            {
                TriggerCoreCollapse();
            }
        }
        if (!generatorBoostTriggered_ && matchTime_ >= 10.0f * 60.0f)
        {
            generatorBoostTriggered_ = true;
            AddEventMessage("10:00 Generator speed increased", Color { 255, 235, 142, 255 }, 5.0f);
            AddKillFeed("Generators accelerated", Color { 255, 235, 142, 255 }, 6.0f);
            audio_.PlayPurchase();
        }
        UpdateLocalPlayer(dt);
        UpdateAttackOrBreak(dt);
        const auto botsStarted = profilingEnabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
        UpdateBots(dt);
        if (profilingEnabled_)
        {
            profileBotsMs_ += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - botsStarted).count();
        }
        UpdateGenerators(dt);
        UpdatePickups(dt);
        UpdateDroppedItems(dt);
        UpdateBlockHazards(dt);
        UpdateExplosives(dt);
        UpdateProjectiles(dt);
        UpdateHazardZones(dt);
        UpdateHeroTemporaryBlocks(dt);
        UpdateBromDevices(dt);
        UpdateKonvoyDevices(dt);
        UpdateLikhoBleeds(dt);
        UpdateSvidetelEffects(dt);
        UpdateAlarmTraps();
        UpdatePassiveRegeneration(dt);
        UpdateBaseHealing(dt);
        UpdateDamageCredits(dt);
        HandleDeathsAndRespawns();
        winnerTeamId_ = rules_.CheckWinCondition(teams_, players_);
        if (!winnerTeamId_.has_value() && coreCollapseTriggered_ && suddenDeathTiebreakTeamId_.has_value())
        {
            const bool anyTeamStillAlive = std::any_of(
                players_.begin(), players_.end(),
                [](const Player& player) { return !player.IsEliminated(); });
            if (!anyTeamStillAlive)
            {
                winnerTeamId_ = suddenDeathTiebreakTeamId_;
            }
        }
        if (winnerTeamId_.has_value())
        {
            const Team* winner = FindTeam(*winnerTeamId_);
            SetMessage((winner != nullptr ? winner->name : "A") + std::string(" team wins!"), 8.0f);
            AddEventMessage((winner != nullptr ? winner->name : "A") + std::string(" team wins!"), Color { 255, 235, 142, 255 }, 7.0f);
            audio_.PlayVictory();
        }
    }

    UpdateAutomatch(dt);

    if (shopOpen_ && !IsLocalPlayerInShopZone())
    {
        shopOpen_ = false;
        DisableCursor();
    }

    if (messageTimer_ > 0.0f)
    {
        messageTimer_ = std::max(0.0f, messageTimer_ - dt);
        if (messageTimer_ == 0.0f)
        {
            message_.clear();
        }
    }
    if (orbitaTeleportPreviewTimer_ > 0.0f)
    {
        orbitaTeleportPreviewTimer_ = std::max(0.0f, orbitaTeleportPreviewTimer_ - dt);
    }

    if (!headless_)
    {
        UpdatePlacementPreview();
        UpdateCombatPreview();
        UpdateFastPlacement(dt);
        UpdateFeedback(dt);
        SendMockNetworkInput();
        UpdateCamera(dt);
    }
}

void Game::Render()
{
    if (headless_)
    {
        return;
    }

    BeginDrawing();
    const bool inWorldView = screen_ == GameScreen::Playing || screen_ == GameScreen::Paused;
    const bool postFrameActive = inWorldView
        && (postProcessing_ || renderScale_ < 0.99f)
        && postProcessor_.BeginFrame(renderScale_);
    ClearBackground(inWorldView ? BiomeSkyColor() : Color { 14, 17, 24, 255 });
    if (inWorldView)
    {
        const Color sky = BiomeSkyColor();
        const Color horizon {
            static_cast<unsigned char>(std::min(255, static_cast<int>(sky.r) + 34)),
            static_cast<unsigned char>(std::min(255, static_cast<int>(sky.g) + 42)),
            static_cast<unsigned char>(std::min(255, static_cast<int>(sky.b) + 58)),
            255
        };
        DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(), sky, horizon);
    }

    if (screen_ == GameScreen::MainMenu)
    {
        RenderMainMenu();
        EndDrawing();
        return;
    }
    if (screen_ == GameScreen::HeroSelect)
    {
        RenderHeroSelect();
        EndDrawing();
        return;
    }
    if (screen_ == GameScreen::Settings)
    {
        RenderSettings();
        EndDrawing();
        return;
    }
    if (screen_ == GameScreen::Controls)
    {
        RenderControls();
        EndDrawing();
        return;
    }

    const Player* localPlayer = GetLocalPlayer();
    const ItemStack localHeldItem = localPlayer != nullptr ? GetSelectedHotbarStack(*localPlayer) : ItemStack {};
    OrbitaTeleportPreview orbitaTeleportPreview {};
    if (localPlayer != nullptr
        && screen_ == GameScreen::Playing
        && !shopOpen_
        && !inventoryOpen_
        && !spectatorMode_
        && orbitaTeleportPreviewTimer_ > 0.0f)
    {
        orbitaTeleportPreview = orbitaTeleportPreview_;
    }
    const std::vector<HeroDeviceVisual> heroDevices = BuildHeroDeviceVisuals();
    renderer_.RenderScene(
        world_,
        teams_,
        cores_,
        players_,
        generators_,
        pickups_,
        droppedItems_,
        heroDevices,
        orbitaTeleportPreview,
        placementPreview_,
        projectiles_,
        worldEffects_,
        particles_,
        floatingTexts_,
        cameraController_.GetCamera(),
        localHeldItem,
        BiomeSkyColor(),
        cameraController_.GetMode() == ViewMode::FirstPerson);

    const float fogAlpha = BiomeFogAlpha();
    if (fogAlpha > 0.0f)
    {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BiomeFogColor(), fogAlpha));
    }

    if (postFrameActive)
    {
        postProcessor_.EndFrameAndDraw(
            postProcessing_,
            bloomEnabled_,
            damageFlashTimer_,
            sniperScopeBlend_,
            reducedFlashes_);
    }

    if (localPlayer != nullptr)
    {
        renderer_.RenderUI(
            *localPlayer,
            teams_,
            shopOpen_,
            IsLocalPlayerInShopZone(),
            shopCategoryIndex_,
            shop_,
            message_,
            placementPreview_,
            breakProgress_,
            combatPreview_,
            orbitaTeleportPreview,
            selectedHotbarSlot_,
            inventoryOpen_,
            inventoryCursorSlot_,
            heldInventoryStack_,
            cameraController_.GetModeName(),
            eventMessages_,
            stats_,
            hitMarkerTimer_,
            reducedFlashes_ ? damageFlashTimer_ * 0.25f : damageFlashTimer_,
            matchTime_,
            KeyLabel(input_.GetBindings().heroActive1),
            KeyLabel(input_.GetBindings().heroActive2),
            KeyLabel(input_.GetBindings().heroUltimate),
            sniperScopeBlend_,
            sniperMagnification_,
            winnerTeamId_);

        RenderKillFeed();
        RenderDeathOverlay(*localPlayer);
        if (spectatorMode_)
        {
            RenderSpectatorOverlay();
        }
        if (inventoryOpen_)
        {
            RenderChestOverlay();
        }
        RenderCoreCollapseTimer();
        if (showMinimap_)
        {
            RenderMinimap(*localPlayer);
        }
        if (showControlHints_)
        {
            RenderGameHints(*localPlayer);
        }
        if (tutorialMode_ || matchTime_ < 72.0f)
        {
            RenderOnboarding(*localPlayer);
        }
        const Team* localTeam = FindTeam(localPlayer->GetTeamId());
        if (localTeam != nullptr && localTeam->enemyTrackerUnlocked)
        {
            RenderCompass(*localPlayer);
        }
#if DAIBED_DEVELOPER_BUILD
        if (showBotDebug_)
        {
            RenderBotDebug();
        }
#endif
        RenderAutomatchOverlay();
        if (scoreboardHeld_)
        {
            RenderScoreboard();
        }
    }

    if (screen_ == GameScreen::Paused)
    {
        RenderPauseOverlay();
    }

    EndDrawing();
}

void Game::TriggerCoreCollapse()
{
    coreCollapseTriggered_ = true;
    int destroyedCount = 0;
    for (EnergyCore& core : cores_)
    {
        if (!core.IsAlive())
        {
            continue;
        }

        while (core.IsAlive())
        {
            core.Damage(core.GetMaxHealth());
        }
        // The collapse is the arena-wide match finisher: no hero trick may
        // refill a core here, otherwise stalemates never resolve.
        world_.RemoveBlock(core.GetBlockPosition());
        Team* team = FindTeam(core.GetTeamId());
        if (team != nullptr)
        {
            team->coreAlive = false;
        }
        AddWorldEffect(world_.GridToWorld(core.GetBlockPosition()), Color { 255, 118, 118, 255 }, 0.75f, 0.8f);
        ++destroyedCount;
    }

    if (destroyedCount > 0)
    {
        SetMessage("Арена рушится! Все Коры уничтожены, распад арены ранит каждого. Последняя жизнь.", 6.0f);
        AddEventMessage("All EnergyCores collapsed", Color { 255, 118, 118, 255 }, 6.0f);
        audio_.PlayCoreDestroyed();
        AddCameraShake(0.34f, 0.45f);
    }
}

void Game::ApplyBotLoadout(Player& bot) const
{
    bot.GetInventory().AddItem(bot.GetHeroId() == HeroId::Svidetel ? ItemType::SniperRifle : ItemType::Sword, 1);
    if (botDifficulty_ == BotDifficulty::Easy)
    {
        bot.GetInventory().AddBlock(BlockType::WoodBlock, 18);
        bot.GetInventory().AddBlock(BlockType::WoolBlock, 14);
        return;
    }

    bot.GetInventory().AddBlock(BlockType::WoodBlock, botDifficulty_ == BotDifficulty::Hard ? 24 : 12);
    bot.GetInventory().AddBlock(BlockType::WoolBlock, botDifficulty_ == BotDifficulty::Hard ? 40 : 24);
    bot.GetInventory().AddBlock(BlockType::StoneBlock, botDifficulty_ == BotDifficulty::Hard ? 12 : 6);
}

float Game::TerrainSpeedMultiplier(const Player& player) const
{
    const Vector3 pos = player.GetPosition();
    const GridPos underFeet = world_.WorldToGrid(Vector3 { pos.x, pos.y - 1.05f, pos.z });
    const Block* block = world_.GetBlock(underFeet);
    if (block == nullptr)
    {
        return 1.0f;
    }
    if (block->type == BlockType::StickyBlock)
    {
        return 0.58f;
    }
    if (block->type == BlockType::IceBlock)
    {
        return arenaBiome_ == ArenaBiome::Ice ? 1.28f : 1.16f;
    }
    if (arenaBiome_ == ArenaBiome::Ice)
    {
        return player.IsLocal() ? 1.08f : 1.03f;
    }
    return 1.0f;
}

float Game::BiomeGravityMultiplier() const
{
    return arenaBiome_ == ArenaBiome::Space ? 0.62f : 1.0f;
}

float Game::BiomeJumpMultiplier() const
{
    return arenaBiome_ == ArenaBiome::Space ? 1.14f : 1.0f;
}

float Game::BiomeGroundControlMultiplier(const Player& player) const
{
    const Vector3 pos = player.GetPosition();
    const GridPos underFeet = world_.WorldToGrid(Vector3 { pos.x, pos.y - 1.05f, pos.z });
    const Block* block = world_.GetBlock(underFeet);
    if (block != nullptr && block->type == BlockType::IceBlock)
    {
        return player.IsLocal() ? 0.38f : 0.58f;
    }
    if (arenaBiome_ == ArenaBiome::Ice)
    {
        return player.IsLocal() ? 0.58f : 0.76f;
    }
    if (arenaBiome_ == ArenaBiome::Space)
    {
        return 0.86f;
    }
    return 1.0f;
}

float Game::BiomeAirControlMultiplier() const
{
    return arenaBiome_ == ArenaBiome::Space ? 0.78f : 1.0f;
}

float Game::BiomeKnockbackMultiplier() const
{
    return arenaBiome_ == ArenaBiome::Space ? 1.18f : 1.0f;
}

void Game::ApplyStandingBlockEffects(Player& player, bool localPlayer)
{
    const Vector3 pos = player.GetPosition();
    const GridPos underFeet = world_.WorldToGrid(Vector3 { pos.x, pos.y - 1.05f, pos.z });
    const Block* block = world_.GetBlock(underFeet);
    if (block == nullptr)
    {
        return;
    }

    if (block->type == BlockType::SpringBlock && player.IsOnGround())
    {
        player.ApplyKnockback(Vector3 { 0.0f, 8.8f, 0.0f });
        AddWorldEffect(world_.GridToWorld(underFeet), Color { 128, 238, 166, 255 }, 0.34f, 0.25f);
        if (localPlayer)
        {
            AddCameraShake(0.10f, 0.12f);
        }
    }
}

void Game::DropPlayerResources(Player& player)
{
    const ResourceType resourceTypes[] { ResourceType::Iron, ResourceType::Gold, ResourceType::Crystal };
    for (ResourceType type : resourceTypes)
    {
        const int amount = player.GetInventory().GetResource(type) / 2;
        if (amount <= 0)
        {
            continue;
        }

        player.GetInventory().SpendResource(type, amount);
        pickups_.push_back(ResourcePickup {
            type,
            amount,
            Vector3 { player.GetPosition().x, player.GetPosition().y + 0.35f, player.GetPosition().z },
            0.65f,
            24.0f,
            0.0f,
            false });
    }
}

void Game::UpdateCamera(float dt)
{
    if (spectatorMode_)
    {
        sniperScopeBlend_ += (0.0f - sniperScopeBlend_) * std::min(1.0f, dt * 12.0f);
        gameplayFov_ += (fov_ - gameplayFov_) * std::min(1.0f, dt * 10.0f);
        cameraController_.SetFov(gameplayFov_);
        UpdateSpectator(dt);
        return;
    }

    const Player* player = GetLocalPlayer();
    if (player == nullptr)
    {
        return;
    }

    const float scopeTarget = IsSniperScopeRequested(*player) ? 1.0f : 0.0f;
    sniperScopeBlend_ += (scopeTarget - sniperScopeBlend_) * std::min(1.0f, dt * 10.0f);
    const float smoothScope = sniperScopeBlend_ * sniperScopeBlend_ * (3.0f - 2.0f * sniperScopeBlend_);

    fovKick_ = std::max(0.0f, fovKick_ - fovKick_ * std::min(1.0f, dt * 7.0f));
    const float normalFov = fov_ + (player->IsSprinting() && scopeTarget < 0.5f ? 6.0f : 0.0f) + fovKick_;
    const float scopedFov = 2.0f * std::atan(
        std::tan(fov_ * DEG2RAD * 0.5f) / sniperMagnification_) * RAD2DEG;
    const float targetFov = normalFov + (scopedFov - normalFov) * smoothScope;
    gameplayFov_ += (targetFov - gameplayFov_) * std::min(1.0f, dt * 12.0f);
    cameraController_.SetFov(gameplayFov_);
    cameraController_.SetCrouching(player->IsSneaking());
    cameraController_.Update(player->GetPosition(), dt);
}

bool Game::LaunchBlasterShot(Player& player, Vector3 direction, bool aimed, bool announce)
{
    if ((!player.GetInventory().HasItem(ItemType::Blaster)
            && !player.GetInventory().HasItem(ItemType::SniperRifle))
        || player.GetBlasterState() != CrossbowState::Loaded)
    {
        return false;
    }

    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    direction = length > 0.0001f
        ? Vector3 { direction.x / length, direction.y / length, direction.z / length }
        : player.Forward();

    const float spread = aimed ? kBlasterTuning.aimedSpread : kBlasterTuning.hipSpread;
    const float jitter = std::sin(static_cast<float>(player.GetId() * 37) + static_cast<float>(GetTime()) * 0.01f);
    direction.x += jitter * spread;
    direction.y += std::cos(static_cast<float>(player.GetId() * 19) + static_cast<float>(GetTime()) * 0.01f) * spread;
    const float adjustedLength = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    direction = Vector3 { direction.x / adjustedLength, direction.y / adjustedLength, direction.z / adjustedLength };

    EnergyProjectile projectile {};
    projectile.position = Vector3 {
        player.GetPosition().x + direction.x * 0.8f,
        player.GetPosition().y + 0.82f + direction.y * 0.8f,
        player.GetPosition().z + direction.z * 0.8f };
    projectile.ownerId = player.GetId();
    projectile.ownerTeamId = player.GetTeamId();
    projectile.velocity = Vector3 {
        direction.x * kBlasterTuning.baseSpeed,
        direction.y * kBlasterTuning.baseSpeed,
        direction.z * kBlasterTuning.baseSpeed };
    projectile.damage = std::max(1, static_cast<int>(std::round(
        kBlasterTuning.baseDamage * BlasterDamageMultiplier(player.GetInventory().GetBlasterDamageLevel()))));
    projectile.radius = 0.22f;
    projectile.gravity = kBlasterTuning.gravity;
    projectile.lifetime = kProjectilePhysicsTuning.maxLifetime;
    projectile.maxRange = kProjectilePhysicsTuning.maximumRange;
    projectile.kind = ProjectileKind::Blaster;
    projectile.airDragPerTick = kProjectilePhysicsTuning.arrowAirDragPerTick;
    projectile.affectedByDrag = true;
    projectile.previousPosition = projectile.position;
    projectile.startPosition = projectile.position;
    projectiles_.push_back(projectile);
    player.ConsumeLoadedBlaster();
    player.ResetAttackCooldown(kBlasterTuning.cooldown);
    AddWorldEffect(projectile.position, direction, Color { 98, 245, 255, 255 }, 0.50f, 0.32f, WorldEffectKind::Burst);
    if (announce)
    {
        const bool sniper = player.IsLocal() && GetSelectedHotbarStack(player).type == ItemType::SniperRifle;
        SetMessage(sniper
            ? "Снайперская винтовка разряжена — удерживайте ЛКМ для новой зарядки."
            : "Бластер разряжен — удерживайте ЛКМ для новой зарядки.");
        audio_.PlayBreakBlock();
    }
    return true;
}

bool Game::LaunchBowShot(Player& player, Vector3 direction, float drawPower, bool announce)
{
    if (!player.GetInventory().HasItem(ItemType::Bow)
        || drawPower < kBowTuning.minimumDrawPower)
    {
        return false;
    }
    if (!SpendUtilityItem(player, UtilityType::Arrows))
    {
        if (announce)
        {
            SetMessage("Нет стрел. Купите боеприпасы в магазине.");
            audio_.PlayDenied();
        }
        return false;
    }

    const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    direction = length > 0.0001f
        ? Vector3 { direction.x / length, direction.y / length, direction.z / length }
        : player.Forward();
    const float speed = kBowTuning.maximumArrowSpeed * std::clamp(drawPower, 0.0f, 1.0f);
    const int upgradeLevel = player.GetInventory().GetBowUpgradeLevel();
    const int powerLevel = BowPowerLevelForUpgrade(upgradeLevel);

    EnergyProjectile projectile {};
    projectile.position = Vector3 {
        player.GetPosition().x + direction.x * 0.72f,
        player.GetPosition().y + 0.82f + direction.y * 0.72f,
        player.GetPosition().z + direction.z * 0.72f };
    projectile.previousPosition = projectile.position;
    projectile.startPosition = projectile.position;
    projectile.ownerId = player.GetId();
    projectile.ownerTeamId = player.GetTeamId();
    projectile.velocity = Vector3 { direction.x * speed, direction.y * speed, direction.z * speed };
    projectile.baseDamage = kBowTuning.baseArrowDamage
        * (1.0f + kBowTuning.powerDamageBonusPerLevel * static_cast<float>(powerLevel));
    projectile.damage = 1;
    projectile.radius = kArrowTuning.radius;
    projectile.gravity = kProjectilePhysicsTuning.arrowGravityPerSecond;
    projectile.airDragPerTick = kProjectilePhysicsTuning.arrowAirDragPerTick;
    projectile.lifetime = kProjectilePhysicsTuning.maxLifetime;
    projectile.maxRange = kProjectilePhysicsTuning.maximumRange;
    projectile.punchLevel = BowPunchLevelForUpgrade(upgradeLevel);
    projectile.kind = ProjectileKind::Arrow;
    projectile.critical = drawPower >= 0.999f;
    projectile.speedBasedDamage = true;
    projectile.affectedByDrag = true;
    projectiles_.push_back(projectile);
    player.ResetAttackCooldown(kArrowTuning.cooldown);
    if (announce)
    {
        SetMessage(projectile.critical ? "Лук: критический выстрел!" : "Лук: выстрел.");
        audio_.PlayBreakBlock();
    }
    return true;
}

void Game::UpdateSpectator(float dt)
{
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_PERIOD))
    {
        CycleSpectatorTarget(1);
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_COMMA))
    {
        CycleSpectatorTarget(-1);
    }
    if (IsKeyPressed(KEY_F))
    {
        spectatorFreeCamera_ = !spectatorFreeCamera_;
        SetMessage(spectatorFreeCamera_ ? "Spectator: free camera." : "Spectator: follow player.", 1.5f);
        if (const Player* target = GetSpectatorTarget())
        {
            spectatorPosition_ = target->GetPosition();
        }
    }

    const Player* target = GetSpectatorTarget();
    if (!spectatorFreeCamera_ && target == nullptr)
    {
        spectatorFreeCamera_ = true;
    }

    if (spectatorFreeCamera_)
    {
        Vector3 move {};
        const Vector3 forward = cameraController_.GetFlatForward();
        const Vector3 right = cameraController_.GetFlatRight();
        if (IsKeyDown(KEY_W))
        {
            move.x += forward.x;
            move.z += forward.z;
        }
        if (IsKeyDown(KEY_S))
        {
            move.x -= forward.x;
            move.z -= forward.z;
        }
        if (IsKeyDown(KEY_D))
        {
            move.x += right.x;
            move.z += right.z;
        }
        if (IsKeyDown(KEY_A))
        {
            move.x -= right.x;
            move.z -= right.z;
        }
        if (IsKeyDown(KEY_SPACE))
        {
            move.y += 1.0f;
        }
        if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))
        {
            move.y -= 1.0f;
        }

        const float length = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z);
        if (length > 0.001f)
        {
            const float speed = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 18.0f : 9.0f;
            spectatorPosition_.x += move.x / length * speed * dt;
            spectatorPosition_.y += move.y / length * speed * dt;
            spectatorPosition_.z += move.z / length * speed * dt;
        }
        cameraController_.Update(spectatorPosition_, dt);
        return;
    }

    spectatorPosition_ = target->GetPosition();
    cameraController_.Update(spectatorPosition_, dt);
}

void Game::UpdateLocalPlayer(float dt)
{
    Player* player = GetLocalPlayer();
    if (player == nullptr || !player->IsAlive())
    {
        return;
    }

    player->SetYaw(cameraController_.GetYaw());
    const Vector3 forward = cameraController_.GetFlatForward();
    const Vector3 right = cameraController_.GetFlatRight();
    Vector3 wish {
        forward.x * currentInput_.move.z + right.x * currentInput_.move.x,
        0.0f,
        forward.z * currentInput_.move.z + right.z * currentInput_.move.x
    };
    const ItemType selectedItem = GetSelectedHotbarStack(*player).type;
    const bool aimingBlaster = (selectedItem == ItemType::Blaster && currentInput_.placeHeld)
        || (selectedItem == ItemType::SniperRifle && currentInput_.scopeHeld);
    if (aimingBlaster)
    {
        wish.x *= 0.56f;
        wish.z *= 0.56f;
    }
    UseUtilityInputs(*player);

    const bool wasOnGround = player->IsOnGround();
    const float fallingVelocity = player->GetVelocity().y;
    if (wasOnGround)
    {
        localAirPeakY_ = player->GetPosition().y;
    }
    const bool wantsForwardSprint = currentInput_.move.z > 0.05f;
    const bool sprint = currentInput_.sprint && wantsForwardSprint && !currentInput_.sneak && !currentInput_.bridgeMode && !aimingBlaster;
    if (currentInput_.sprintTapped && sprint)
    {
        player->RefreshSprintReset();
    }
    player->Move(
        wish,
        currentInput_.jumpHeld,
        dt,
        world_,
        sprint,
        currentInput_.sneak,
        TerrainSpeedMultiplier(*player),
        false,
        BiomeGravityMultiplier(),
        BiomeJumpMultiplier(),
        BiomeGroundControlMultiplier(*player),
        BiomeAirControlMultiplier());
    ApplyStandingBlockEffects(*player, true);

    if (!player->IsOnGround())
    {
        localAirPeakY_ = std::max(localAirPeakY_, player->GetPosition().y);
    }

    const float fallDistance = localAirPeakY_ - player->GetPosition().y;
    if (!wasOnGround && player->IsOnGround() && fallDistance >= 3.0f)
    {
        const float strength = std::min(0.42f, 0.12f + (fallDistance - 3.0f) * 0.055f + std::fabs(fallingVelocity) * 0.008f);
        AddWorldEffect(player->GetPosition(), Color { 210, 220, 235, 255 }, 0.22f + strength, 0.25f);
        AddCameraShake(strength, 0.16f);
        audio_.PlayLanding();
        localAirPeakY_ = player->GetPosition().y;
    }

    localWasOnGround_ = player->IsOnGround();
    localFallVelocity_ = player->GetVelocity().y;
}

void Game::HandleDeathsAndRespawns()
{
    for (Player& player : players_)
    {
        Team* team = FindTeam(player.GetTeamId());
        if (team == nullptr)
        {
            continue;
        }

        if (player.IsAlive() && (player.GetHealth() <= 0 || player.GetPosition().y < -12.0f))
        {
            const bool finalDeath = !team->coreAlive;
            const bool voidDeath = player.GetPosition().y < -12.0f;
            const int killerId = DeathCreditFor(player.GetId());
            int killerTeamId = -1;
            std::string killerName = "Окружение";
            std::string deathCause = voidDeath ? "падение в воид" : "опасность арены";
            for (const DamageCredit& credit : damageCredits_)
            {
                if (credit.targetId == player.GetId() && credit.timer > 0.0f)
                {
                    deathCause = credit.cause;
                    break;
                }
            }
            if (killerId >= 0)
            {
                for (const Player& candidate : players_)
                {
                    if (candidate.GetId() == killerId)
                    {
                        killerTeamId = candidate.GetTeamId();
                        killerName = candidate.GetName();
                        break;
                    }
                }
            }
            HandleDeathInventory(player, killerId);
            if (player.GetHeroId() == HeroId::Svidetel)
            {
                svidetelEchoes_.push_back(SvidetelEcho {
                    player.GetPosition(), player.GetId(), player.GetTeamId(), 12.0f, 0.0f, false, {}, 0.0f });
                player.AddHeroUltimateCharge(8.0f);
            }
            damageCredits_.erase(
                std::remove_if(
                    damageCredits_.begin(),
                    damageCredits_.end(),
                    [&player](const DamageCredit& credit)
                    {
                        return credit.targetId == player.GetId();
                    }),
                damageCredits_.end());
            player.Kill(finalDeath);
            konvoyTethers_.erase(
                std::remove_if(konvoyTethers_.begin(), konvoyTethers_.end(), [&player](const KonvoyTether& tether)
                {
                    return tether.ownerPlayerId == player.GetId() || tether.targetPlayerId == player.GetId();
                }),
                konvoyTethers_.end());
            PlayerMatchScore& score = GetPlayerScore(player.GetId());
            ++score.deaths;
            if (finalDeath)
            {
                ++score.finalDeaths;
            }
            if (automatch_.active && !player.IsLocal())
            {
                for (AutomatchBotStats& botStats : automatch_.botStats)
                {
                    if (botStats.teamId == player.GetTeamId() && botStats.name == player.GetName())
                    {
                        if (voidDeath)
                        {
                            ++botStats.voidFalls;
                        }
                        break;
                    }
                }
                if (voidDeath || finalDeath)
                {
                    automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                        matchTime_,
                        voidDeath ? "voidFall" : "finalDeath",
                        player.GetTeamId(),
                        killerTeamId,
                        killerId,
                        player.GetId(),
                        finalDeath ? 1 : 0,
                        player.GetName() + (voidDeath ? " fell into void" : " final death")
                    });
                }
            }
            SetMessage(player.GetName() + (finalDeath ? " was eliminated." : " was defeated and will respawn."));
            AddEventMessage(player.GetName() + (finalDeath ? " eliminated" : " down"), finalDeath ? RED : ORANGE, 2.2f);
            AddKillFeed(
                player.GetName()
                    + (killerId >= 0
                            ? " lost inventory"
                            : (player.GetPosition().y < -12.0f ? " fell into void" : " died")),
                finalDeath ? RED : ORANGE,
                5.0f);
            audio_.PlayDeath();
            if (player.IsLocal())
            {
                localDeathKiller_ = killerName;
                localDeathCause_ = deathCause;
                localDeathOverlayTimer_ = finalDeath ? 7.0f : 4.0f;
                ++stats_.deaths;
                damageFlashTimer_ = 0.9f;
                AddCameraShake(0.28f, 0.25f);
                audio_.PlayDenied();
                if (finalDeath)
                {
                    EnterSpectatorMode();
                }
            }
            continue;
        }

        if (!player.IsAlive() && !player.IsEliminated())
        {
            if (!team->coreAlive)
            {
                player.Kill(true);
                ++GetPlayerScore(player.GetId()).finalDeaths;
                if (automatch_.active && !player.IsLocal())
                {
                    automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                        matchTime_,
                        "finalDeath",
                        player.GetTeamId(),
                        -1,
                        -1,
                        player.GetId(),
                        1,
                        player.GetName() + " lost respawn protection"
                    });
                }
                SetMessage(player.GetName() + " lost respawn protection. Final death.");
                if (player.IsLocal())
                {
                    EnterSpectatorMode();
                }
            }
            else if (player.GetRespawnTimer() <= 0.0f)
            {
                player.RespawnAtHome();
                SetMessage(player.GetName() + " respawned. " + BoolCoreState(team->coreAlive));
                AddWorldEffect(player.GetHomeSpawnPoint(), GetTeamColor(team->color), 0.42f, 0.45f);
            }
        }
    }
}

void Game::SendMockNetworkInput()
{
    network_.SendToServer(NetworkMessage { NetworkMessageType::Input, static_cast<unsigned int>(localPlayerId_), "local_input_frame" });
    network_.Pump();
    (void)network_.ConsumeIncoming();
}

bool Game::IsLocalPlayerInShopZone() const
{
    const Player* player = GetLocalPlayer();
    if (player == nullptr)
    {
        return false;
    }

    // Any team's shop serves any customer (Hypixel rule): raiding an enemy
    // base and restocking right there is a legitimate tactic.
    for (const Team& team : teams_)
    {
        if (shop_.IsPlayerInShop(*player, team))
        {
            return true;
        }
    }
    return false;
}

bool Game::WouldBlockOverlapPlayer(const GridPos& pos, int underfootPlayerId) const
{
    const Vector3 center = world_.GridToWorld(pos);
    constexpr Vector3 halfPlayer { 0.36f, 0.95f, 0.36f };

    for (const Player& player : players_)
    {
        if (!player.IsAlive())
        {
            continue;
        }

        const Vector3 p = player.GetPosition();
        const bool overlapX = std::fabs(p.x - center.x) <= (halfPlayer.x + 0.5f);
        const bool overlapY = std::fabs(p.y - center.y) <= (halfPlayer.y + 0.5f);
        const bool overlapZ = std::fabs(p.z - center.z) <= (halfPlayer.z + 0.5f);
        if (overlapX && overlapY && overlapZ)
        {
            if (player.GetId() == underfootPlayerId && center.y <= p.y - 1.38f)
            {
                continue;
            }
            return true;
        }
    }

    return false;
}

bool Game::IsVoidThreatAt(Vector3 position) const
{
    const GridPos underCenter = world_.WorldToGrid(Vector3 { position.x, position.y - 1.08f, position.z });
    if (!world_.IsAir(underCenter))
    {
        return false;
    }

    const GridPos lowerCenter = world_.WorldToGrid(Vector3 { position.x, position.y - 1.86f, position.z });
    if (!world_.IsAir(lowerCenter))
    {
        return false;
    }

    return true;
}

bool Game::HasBotCoreAccess(const Player& bot, const EnergyCore& core) const
{
    const Vector3 origin {
        bot.GetPosition().x,
        bot.GetPosition().y + 0.78f,
        bot.GetPosition().z
    };
    const Vector3 coreCenter {
        static_cast<float>(core.GetBlockPosition().x),
        static_cast<float>(core.GetBlockPosition().y) + 0.58f,
        static_cast<float>(core.GetBlockPosition().z)
    };
    const Vector3 toCore {
        coreCenter.x - origin.x,
        coreCenter.y - origin.y,
        coreCenter.z - origin.z
    };
    const float maxDistance = std::sqrt(toCore.x * toCore.x + toCore.y * toCore.y + toCore.z * toCore.z) + 0.25f;
    const std::optional<RaycastHit> hit = world_.Raycast(origin, toCore, maxDistance);
    return hit.has_value() && hit->block == core.GetBlockPosition();
}

bool Game::IsTeamActiveForMode(int teamId) const
{
    if (selectedMode_ == MatchMode::TwoVsTwo || selectedMode_ == MatchMode::Duel)
    {
        return teamId == 0 || teamId == 1;
    }
    return teamId >= 0 && teamId < 4;
}

int Game::TeamCountForMode() const
{
    return (selectedMode_ == MatchMode::TwoVsTwo || selectedMode_ == MatchMode::Duel) ? 2 : 4;
}

int Game::MaxBotCountForSelection() const
{
    return std::max(0, TeamCountForMode() * std::clamp(selectedTeamSize_, 1, 4) - 1);
}

std::string Game::TeamSizeName() const
{
    const int size = std::clamp(selectedTeamSize_, 1, 4);
    if (TeamCountForMode() == 2)
    {
        return std::to_string(size) + "v" + std::to_string(size);
    }

    return std::to_string(size) + " per team";
}

std::string Game::BotCountName() const
{
    const int maxBots = MaxBotCountForSelection();
    return std::to_string(std::clamp(selectedBotCount_, 0, maxBots)) + "/" + std::to_string(maxBots);
}

std::string Game::AutomatchRunCountName() const
{
    return std::to_string(std::clamp(automatchRunTarget_, 1, 50));
}

std::string Game::AutomatchSpeedName() const
{
    return std::to_string(std::clamp(automatchTicksPerFrame_, 1, 32)) + "x";
}

std::string Game::AutomatchDurationName() const
{
    return std::to_string(std::clamp(automatchMaxMinutes_, 3, 30)) + " min";
}

int Game::GetForgeBonusForTeam(int teamId) const
{
    const Team* team = FindTeam(teamId);
    if (team == nullptr)
    {
        return 0;
    }

    const int tenMinuteBoost = matchTime_ >= 10.0f * 60.0f ? 1 : 0;
    return std::min(3, team->forgeLevel / 2 + tenMinuteBoost);
}

bool Game::RepairTeamCore(Player& player, Team& team, std::string& message)
{
    EnergyCore* core = FindCoreByTeam(team.id);
    if (core == nullptr || !core->IsAlive())
    {
        message = "Core cannot be repaired after destruction.";
        return false;
    }
    if (core->GetHealth() >= core->GetMaxHealth())
    {
        message = "Core is already fully repaired.";
        return false;
    }
    Inventory& inventory = player.GetInventory();
    if (inventory.GetResource(ResourceType::Crystal) < 2 || inventory.GetResource(ResourceType::Gold) < 3)
    {
        message = "Need 2 Crystal + 3 Gold for Core repair.";
        return false;
    }

    const int beforeHealth = core->GetHealth();
    inventory.SpendResource(ResourceType::Crystal, 2);
    inventory.SpendResource(ResourceType::Gold, 3);
    core->Repair(30);
    message = "Core repaired +" + std::to_string(core->GetHealth() - beforeHealth) + ".";
    AddWorldEffect(world_.GridToWorld(core->GetBlockPosition()), Color { 112, 232, 255, 255 }, 0.55f, 0.50f);
    AddKillFeed(team.name + " Core repaired", Color { 112, 232, 255, 255 }, 4.0f);
    return true;
}

Player* Game::GetLocalPlayer()
{
    for (Player& player : players_)
    {
        if (player.GetId() == localPlayerId_)
        {
            return &player;
        }
    }

    return nullptr;
}

const Player* Game::GetLocalPlayer() const
{
    for (const Player& player : players_)
    {
        if (player.GetId() == localPlayerId_)
        {
            return &player;
        }
    }

    return nullptr;
}

Player* Game::GetSpectatorTarget()
{
    if (players_.empty())
    {
        return nullptr;
    }
    if (spectatorTargetIndex_ >= 0
        && spectatorTargetIndex_ < static_cast<int>(players_.size())
        && players_[spectatorTargetIndex_].IsAlive())
    {
        return &players_[spectatorTargetIndex_];
    }
    for (Player& player : players_)
    {
        if (player.IsAlive())
        {
            return &player;
        }
    }
    return nullptr;
}

const Player* Game::GetSpectatorTarget() const
{
    if (players_.empty())
    {
        return nullptr;
    }
    if (spectatorTargetIndex_ >= 0
        && spectatorTargetIndex_ < static_cast<int>(players_.size())
        && players_[spectatorTargetIndex_].IsAlive())
    {
        return &players_[spectatorTargetIndex_];
    }
    for (const Player& player : players_)
    {
        if (player.IsAlive())
        {
            return &player;
        }
    }
    return nullptr;
}

void Game::EnterSpectatorMode()
{
    if (spectatorMode_)
    {
        return;
    }

    spectatorMode_ = true;
    spectatorFreeCamera_ = false;
    shopOpen_ = false;
    inventoryOpen_ = false;
    CloseChest();
    heldInventoryStack_ = ItemStack {};
    DisableCursor();

    const Player* localPlayer = GetLocalPlayer();
    spectatorPosition_ = localPlayer != nullptr
        ? localPlayer->GetPosition()
        : Vector3 { 0.0f, 4.0f, 0.0f };
    float bestDistance = std::numeric_limits<float>::max();
    int bestIndex = -1;
    for (int i = 0; i < static_cast<int>(players_.size()); ++i)
    {
        const Player& player = players_[i];
        if (!player.IsAlive())
        {
            continue;
        }
        const float distance = localPlayer != nullptr
            ? DistanceSquared(localPlayer->GetPosition(), player.GetPosition())
            : 0.0f;
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    if (bestIndex >= 0)
    {
        spectatorTargetIndex_ = bestIndex;
        spectatorPosition_ = players_[bestIndex].GetPosition();
    }
    else
    {
        spectatorFreeCamera_ = true;
    }
    cameraController_.SetMode(ViewMode::ThirdPerson);
    SetMessage("Final death. Spectator mode enabled.", 4.0f);
}

void Game::CycleSpectatorTarget(int direction)
{
    if (players_.empty())
    {
        return;
    }
    const int count = static_cast<int>(players_.size());
    const int step = direction >= 0 ? 1 : -1;
    const int start = std::clamp(spectatorTargetIndex_, 0, count - 1);
    for (int offset = 1; offset <= count; ++offset)
    {
        const int index = (start + step * offset + count * 2) % count;
        if (players_[index].IsAlive())
        {
            spectatorTargetIndex_ = index;
            spectatorFreeCamera_ = false;
            spectatorPosition_ = players_[index].GetPosition();
            SetMessage("Spectating " + players_[index].GetName(), 1.4f);
            return;
        }
    }
}

Team* Game::FindTeam(int teamId)
{
    for (Team& team : teams_)
    {
        if (team.id == teamId)
        {
            return &team;
        }
    }

    return nullptr;
}

const Team* Game::FindTeam(int teamId) const
{
    for (const Team& team : teams_)
    {
        if (team.id == teamId)
        {
            return &team;
        }
    }

    return nullptr;
}

EnergyCore* Game::FindCoreByTeam(int teamId)
{
    for (EnergyCore& core : cores_)
    {
        if (core.GetTeamId() == teamId)
        {
            return &core;
        }
    }

    return nullptr;
}

EnergyCore* Game::FindCoreAt(const GridPos& pos)
{
    for (EnergyCore& core : cores_)
    {
        if (core.GetBlockPosition() == pos)
        {
            return &core;
        }
    }

    return nullptr;
}

ItemStack Game::GetSelectedHotbarStack(const Player& player) const
{
    if (!player.IsLocal())
    {
        return ItemStack {};
    }

    const Inventory& inventory = player.GetInventory();
    if (selectedHotbarSlot_ < 0 || selectedHotbarSlot_ >= kHotbarSlotCount)
    {
        return ItemStack {};
    }
    return inventory.GetHotbarSlots()[selectedHotbarSlot_];
}

bool Game::IsSniperScopeRequested(const Player& player) const
{
    return player.IsAlive()
        && !shopOpen_
        && !inventoryOpen_
        && !winnerTeamId_.has_value()
        && currentInput_.scopeHeld
        && GetSelectedHotbarStack(player).type == ItemType::SniperRifle;
}

std::optional<BlockType> Game::GetSelectedBlockType(const Player& player) const
{
    const ItemStack stack = GetSelectedHotbarStack(player);
    if (stack.IsEmpty())
    {
        return std::nullopt;
    }
    return ItemToBlock(stack.type);
}

std::optional<WeaponType> Game::GetSelectedWeaponType(const Player& player) const
{
    if (!player.IsLocal())
    {
        return WeaponType::Sword;
    }

    const ItemStack stack = GetSelectedHotbarStack(player);
    if (stack.IsEmpty())
    {
        return std::nullopt;
    }
    return ItemToWeapon(stack.type);
}

int Game::EffectiveToolLevel(const Player& player) const
{
    if (!player.IsLocal())
    {
        return player.GetInventory().GetToolLevel();
    }

    const ItemStack stack = GetSelectedHotbarStack(player);
    return ItemIsPickaxe(stack.type) ? player.GetInventory().GetToolLevel() : 0;
}

void Game::SetMessage(std::string message, float seconds)
{
    if (suppressLocalFeedback_)
    {
        return;
    }
    message_ = std::move(message);
    messageTimer_ = seconds;
}

void Game::AddEventMessage(std::string message, Color color, float seconds)
{
    if (suppressLocalFeedback_)
    {
        return;
    }
    eventMessages_.push_back(EventMessage { std::move(message), color, seconds, 0.0f });
    if (eventMessages_.size() > 5)
    {
        eventMessages_.erase(eventMessages_.begin());
    }
}

void Game::AddWorldEffect(Vector3 position, Color color, float radius, float seconds)
{
    const std::size_t limit = effectsQuality_ == 0 ? 48u : (effectsQuality_ == 1 ? 96u : 192u);
    if (worldEffects_.size() >= limit)
    {
        worldEffects_.erase(worldEffects_.begin());
    }
    worldEffects_.push_back(WorldEffect { position, Vector3 { 0.0f, 0.0f, 1.0f }, color, radius, seconds, 0.0f, WorldEffectKind::Burst });
    particles_.Emit(ParticleKind::Debris, position, Vector3 { 0.0f, 1.0f, 0.0f }, color, 9, 2.4f);
    particles_.Emit(ParticleKind::Dust, position, Vector3 { 0.0f, 0.7f, 0.0f }, Fade(color, 0.72f), 6, 1.3f);
}

void Game::AddWorldEffect(Vector3 position, Vector3 direction, Color color, float radius, float seconds, WorldEffectKind kind)
{
    const Vector3 flatDirection = Normalize2D(direction);
    const Vector3 safeDirection = Length2D(flatDirection) > 0.0001f ? flatDirection : Vector3 { 0.0f, 0.0f, 1.0f };
    const std::size_t limit = effectsQuality_ == 0 ? 48u : (effectsQuality_ == 1 ? 96u : 192u);
    if (worldEffects_.size() >= limit)
    {
        worldEffects_.erase(worldEffects_.begin());
    }
    worldEffects_.push_back(WorldEffect { position, safeDirection, color, radius, seconds, 0.0f, kind });
    const ParticleKind particleKind = kind == WorldEffectKind::Trail ? ParticleKind::Trail
        : (kind == WorldEffectKind::FireZone ? ParticleKind::Smoke : ParticleKind::Spark);
    particles_.Emit(particleKind, position, safeDirection, color, kind == WorldEffectKind::FireZone ? 5 : 8, 2.1f);
}

void Game::AddFloatingText(std::string text, Vector3 position, Color color)
{
    floatingTexts_.push_back(FloatingText { std::move(text), position, color, 0.8f, 0.0f });
}

void Game::AddKillFeed(std::string text, Color color, float seconds)
{
    if (suppressLocalFeedback_)
    {
        return;
    }
    killFeed_.push_back(KillFeedEntry { std::move(text), color, seconds, 0.0f });
    if (killFeed_.size() > 6)
    {
        killFeed_.erase(killFeed_.begin());
    }
}

void Game::RegisterCombatEvent(const CombatEvent& event, const std::string& message)
{
    const Vector3 targetFeetProbe { event.position.x, event.position.y - 0.75f, event.position.z };
    const bool voidThreat = !event.coreHit && (event.voidHit || IsVoidThreatAt(targetFeetProbe));
    SetMessage(message, event.coreDestroyed ? 4.0f : 2.2f);
    AddEventMessage(message, event.coreDestroyed ? Color { 255, 118, 118, 255 } : Color { 255, 235, 142, 255 });
    AddWorldEffect(event.position, event.coreHit ? Color { 112, 232, 255, 255 } : Color { 255, 224, 122, 255 }, event.coreHit ? 0.48f : 0.32f, event.coreDestroyed ? 0.9f : 0.36f);
    AddFloatingText((event.coreHit ? "-" : "-") + std::to_string(event.damage), event.position, event.coreHit ? Color { 112, 232, 255, 255 } : Color { 255, 236, 135, 255 });
    if (event.combo)
    {
        AddFloatingText("Combo", Vector3 { event.position.x, event.position.y + 0.28f, event.position.z }, Color { 255, 235, 142, 255 });
    }
    else if (event.sprintReset)
    {
        AddFloatingText("W-tap", Vector3 { event.position.x, event.position.y + 0.28f, event.position.z }, Color { 188, 238, 255, 255 });
    }
    if (voidThreat)
    {
        AddFloatingText("Void hit", Vector3 { event.position.x, event.position.y + 0.52f, event.position.z }, Color { 255, 155, 118, 255 });
    }
    if (!event.coreHit && event.targetId >= 0)
    {
        ApplyBotHitReaction(event.targetId);
    }

    int attackerTeamId = -1;
    Player* attackerPlayer = nullptr;
    Player* targetPlayer = nullptr;
    if (event.attackerId >= 0)
    {
        for (Player& candidate : players_)
        {
            if (candidate.GetId() == event.attackerId)
            {
                attackerTeamId = candidate.GetTeamId();
                attackerPlayer = &candidate;
            }
            if (candidate.GetId() == event.targetId)
            {
                targetPlayer = &candidate;
            }
        }
    }

    if (!event.coreHit && attackerPlayer != nullptr && targetPlayer != nullptr)
    {
        if (attackerPlayer->GetHeroId() == HeroId::Konvoy)
        {
            const bool markedIntruder = std::any_of(
                konvoyIntruderMarks_.begin(), konvoyIntruderMarks_.end(),
                [attackerPlayer, targetPlayer](const KonvoyIntruderMark& mark)
                {
                    return mark.ownerPlayerId == attackerPlayer->GetId()
                        && mark.targetPlayerId == targetPlayer->GetId()
                        && mark.markedTimer > 0.0f;
                });
            if (markedIntruder)
            {
                const int bonusDamage = std::max(2, event.damage / 5);
                targetPlayer->Damage(bonusDamage);
                AddFloatingText("INTRUDER -" + std::to_string(bonusDamage), targetPlayer->GetPosition(), HeroAccentColor(HeroId::Konvoy));
                attackerPlayer->AddHeroUltimateCharge(2.0f);
            }
        }
        if (attackerPlayer->GetHeroId() == HeroId::Likho)
        {
            HeroRuntimeState& likhoState = attackerPlayer->MutableHeroState();
            if (likhoState.active1.active)
            {
                const Vector3 fromTarget = Normalize2D(Vector3 {
                    attackerPlayer->GetPosition().x - targetPlayer->GetPosition().x,
                    0.0f,
                    attackerPlayer->GetPosition().z - targetPlayer->GetPosition().z });
                const bool backstab = Dot2D(targetPlayer->Forward(), fromTarget) < -0.35f;
                likhoState.active1.active = false;
                likhoState.active1.activeTimer = 0.0f;
                if (backstab)
                {
                    const int bonusDamage = std::max(3, event.damage / 2);
                    targetPlayer->Damage(bonusDamage);
                    NoteDamageCredit(targetPlayer->GetId(), attackerPlayer->GetId(), "ударом Лихо в спину");
                    attackerPlayer->AddHeroUltimateCharge(14.0f);
                    AddFloatingText("в спину -" + std::to_string(bonusDamage), targetPlayer->GetPosition(), HeroAccentColor(HeroId::Likho));
                }
            }
            if (likhoState.active2.active)
            {
                auto found = std::find_if(
                    likhoBleeds_.begin(), likhoBleeds_.end(),
                    [targetPlayer, attackerPlayer](const LikhoBleed& bleed)
                    {
                        return bleed.targetPlayerId == targetPlayer->GetId() && bleed.ownerPlayerId == attackerPlayer->GetId();
                    });
                if (found == likhoBleeds_.end())
                {
                    likhoBleeds_.push_back(LikhoBleed { targetPlayer->GetId(), attackerPlayer->GetId(), attackerPlayer->GetTeamId(), 6.0f, 0.8f, 1 });
                }
                else
                {
                    found->lifetime = 6.0f;
                    found->stacks = std::min(4, found->stacks + 1);
                    ++found->successfulHits;
                    if (found->successfulHits >= 3)
                    {
                        constexpr int burstDamage = 12;
                        targetPlayer->Damage(burstDamage);
                        NoteDamageCredit(targetPlayer->GetId(), attackerPlayer->GetId(), "Likho bleed burst");
                        AddFloatingText("BLEED BURST -12", targetPlayer->GetPosition(), HeroAccentColor(HeroId::Likho));
                        found->successfulHits = 0;
                    }
                }
                attackerPlayer->AddHeroUltimateCharge(3.0f);
            }
            if (likhoState.ultimate.active)
            {
                likhoState.ultimate.active = false;
                likhoState.ultimate.activeTimer = 0.0f;
                likhoState.likhoDisguiseTeamId = -1;
                likhoState.likhoDisguisePlayerId = -1;
                likhoState.likhoDisguiseHeroId = HeroId::Likho;
            }
        }
        if (targetPlayer->GetHeroId() == HeroId::Likho && targetPlayer->GetHeroState().ultimate.active)
        {
            HeroRuntimeState& targetState = targetPlayer->MutableHeroState();
            targetState.ultimate.active = false;
            targetState.ultimate.activeTimer = 0.0f;
            targetState.likhoDisguiseTeamId = -1;
            targetState.likhoDisguisePlayerId = -1;
            targetState.likhoDisguiseHeroId = HeroId::Likho;
        }
        if (event.killed && attackerPlayer->GetHeroId() == HeroId::Svidetel)
        {
            svidetelEchoes_.push_back(SvidetelEcho {
                targetPlayer->GetPosition(), attackerPlayer->GetId(), attackerPlayer->GetTeamId(), 12.0f, 0.0f, false, {}, 0.0f });
            attackerPlayer->AddHeroUltimateCharge(10.0f);
        }
    }

    if (event.attackerId >= 0)
    {
        if (!event.coreHit && event.targetId >= 0)
        {
            NoteDamageCredit(event.targetId, event.attackerId, CombatSystem::WeaponName(event.weapon));
        }
        PlayerMatchScore& attackerScore = GetPlayerScore(event.attackerId);
        if (event.killed)
        {
            ++attackerScore.kills;
        }
        if (event.coreHit)
        {
            attackerScore.coreDamage += event.damage;
            if (automatch_.active)
            {
                if (automatch_.currentFirstCoreDamageTime < 0.0f)
                {
                    automatch_.currentFirstCoreDamageTime = matchTime_;
                    automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                        matchTime_,
                        "firstCoreDamage",
                        event.targetTeamId,
                        attackerTeamId,
                        event.attackerId,
                        -1,
                        event.damage,
                        "First Core damage"
                    });
                }
                if (event.coreDestroyed)
                {
                    automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                        matchTime_,
                        "coreDestroyed",
                        event.targetTeamId,
                        attackerTeamId,
                        event.attackerId,
                        -1,
                        event.damage,
                        std::string(TeamName(event.targetTeamId)) + " Core destroyed"
                    });
                }
            }
        }
    }

    if (!event.coreHit && event.targetId >= 0 && BiomeKnockbackMultiplier() > 1.0f)
    {
        for (Player& player : players_)
        {
            if (player.GetId() == event.targetId && player.IsAlive() && !player.IsEliminated())
            {
                const float bonus = BiomeKnockbackMultiplier() - 1.0f;
                player.ApplyKnockback(Vector3 {
                    event.knockback.x * bonus,
                    event.knockback.y * bonus,
                    event.knockback.z * bonus
                });
                break;
            }
        }
    }

    if (event.attackerId == localPlayerId_)
    {
        hitMarkerTimer_ = 0.22f;
        ++stats_.hitsDealt;
        stats_.damageDealt += event.damage;
        if (event.killed)
        {
            ++stats_.kills;
        }
        if (event.coreHit)
        {
            stats_.coreDamageDealt += event.damage;
        }
        if (event.coreDestroyed)
        {
            ++stats_.coresDestroyed;
        }
        if (event.coreHit)
        {
            audio_.PlayCoreHit();
            AddCameraShake(event.coreDestroyed ? 0.34f : 0.14f, event.coreDestroyed ? 0.34f : 0.16f);
        }
        else
        {
            audio_.PlayHit();
            AddCameraShake(0.10f + std::min(0.12f, event.damage * 0.002f), 0.13f);
        }
        if (event.killed)
        {
            fovKick_ = std::max(fovKick_, 5.0f);
        }
        else if (event.combo || event.charged || event.sprintReset)
        {
            fovKick_ = std::max(fovKick_, 2.2f);
        }
    }

    if (event.targetId == localPlayerId_)
    {
        damageFlashTimer_ = 0.75f;
        AddCameraShake(0.24f, 0.20f);
        audio_.PlayHit();
    }

    if (event.coreDestroyed)
    {
        AddCameraShake(0.30f, 0.32f);
        audio_.PlayCoreDestroyed();
        AddKillFeed(std::string(TeamName(event.targetTeamId)) + " Core destroyed", Color { 255, 118, 118, 255 }, 7.0f);
    }
    else if (event.coreHit)
    {
        AddKillFeed(std::string(TeamName(event.targetTeamId)) + " Core hit -" + std::to_string(event.damage), Color { 112, 232, 255, 255 }, 3.8f);
    }

    if (event.killed)
    {
        const Player* attacker = nullptr;
        const Player* target = nullptr;
        for (const Player& player : players_)
        {
            if (player.GetId() == event.attackerId)
            {
                attacker = &player;
            }
            if (player.GetId() == event.targetId)
            {
                target = &player;
            }
        }
        const std::string attackerName = attacker != nullptr ? attacker->GetName() : "Unknown";
        const std::string targetName = target != nullptr ? target->GetName() : "Enemy";
        AddKillFeed(attackerName + " eliminated " + targetName, Color { 255, 235, 142, 255 }, 5.5f);
    }
}

Game::PlayerMatchScore& Game::GetPlayerScore(int playerId)
{
    for (PlayerMatchScore& score : playerScores_)
    {
        if (score.playerId == playerId)
        {
            return score;
        }
    }

    playerScores_.push_back(PlayerMatchScore { playerId });
    return playerScores_.back();
}

const Game::PlayerMatchScore* Game::FindPlayerScore(int playerId) const
{
    for (const PlayerMatchScore& score : playerScores_)
    {
        if (score.playerId == playerId)
        {
            return &score;
        }
    }

    return nullptr;
}

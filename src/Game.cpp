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

// --- Fixed-step input buffering ---------------------------------------------
// With a fixed-step simulation, a render frame may run zero, one or several
// simulation ticks. To keep one-shot (edge) inputs from being lost on a frame
// that runs zero ticks, or fired twice on a frame that runs several, the local
// input is accumulated into a pending buffer: continuous fields take the latest
// value while one-shot edges are OR-accumulated and cleared once a tick reads
// them. At the default 60 FPS (one tick per frame) this is behaviour-identical
// to consuming the frame input directly.
void ClearOneShotEdges(PlayerInput& in)
{
    in.jump = false;
    in.attackPressed = false;
    in.attackReleased = false;
    in.middlePressed = false;
    in.placePressed = false;
    in.cameraTogglePressed = false;
    in.sprintTapped = false;
    in.interactPressed = false;
    in.inventoryPressed = false;
    in.debugRespawnPressed = false;
    in.restartPressed = false;
    in.exitPressed = false;
    in.shootPressed = false;
    in.fireballPressed = false;
    in.healPressed = false;
    in.teleportPressed = false;
    in.dashPressed = false;
    in.molotovPressed = false;
    in.alarmPressed = false;
    in.heroActive1Pressed = false;
    in.heroActive2Pressed = false;
    in.heroUltimatePressed = false;
    in.dropPressed = false;
    in.botDebugPressed = false;
}

PlayerInput MergePendingInput(const PlayerInput& pending, const PlayerInput& frame)
{
    PlayerInput merged = frame; // continuous: move / held / aim / wheel / slot = latest
    merged.jump |= pending.jump;
    merged.attackPressed |= pending.attackPressed;
    merged.attackReleased |= pending.attackReleased;
    merged.middlePressed |= pending.middlePressed;
    merged.placePressed |= pending.placePressed;
    merged.cameraTogglePressed |= pending.cameraTogglePressed;
    merged.sprintTapped |= pending.sprintTapped;
    merged.interactPressed |= pending.interactPressed;
    merged.inventoryPressed |= pending.inventoryPressed;
    merged.debugRespawnPressed |= pending.debugRespawnPressed;
    merged.restartPressed |= pending.restartPressed;
    merged.exitPressed |= pending.exitPressed;
    merged.shootPressed |= pending.shootPressed;
    merged.fireballPressed |= pending.fireballPressed;
    merged.healPressed |= pending.healPressed;
    merged.teleportPressed |= pending.teleportPressed;
    merged.dashPressed |= pending.dashPressed;
    merged.molotovPressed |= pending.molotovPressed;
    merged.alarmPressed |= pending.alarmPressed;
    merged.heroActive1Pressed |= pending.heroActive1Pressed;
    merged.heroActive2Pressed |= pending.heroActive2Pressed;
    merged.heroUltimatePressed |= pending.heroUltimatePressed;
    merged.dropPressed |= pending.dropPressed;
    merged.botDebugPressed |= pending.botDebugPressed;
    return merged;
}

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

bool SameBlock(const Block& a, const Block& b)
{
    return a.type == b.type
        && a.teamId == b.teamId
        && a.breakable == b.breakable;
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
    return alive ? "Кор работает." : "Кор уничтожен. Последняя жизнь!";
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
    // MatchSimulation is the authoritative access point for players (Phase 6A).
    // Storage stays in Game's players_ for now (Player isn't raylib-free yet);
    // the pointer is stable across push_back since it points at the vector.
    matchSimulation_.SetPlayers(&players_);
    CrashLogger::Heartbeat("initialize-settings");
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
    CrashLogger::Heartbeat("initialize-window");
    InitWindow(resolution.width, resolution.height, "DaiBed " DAIBED_VERSION);
    if (!IsWindowReady())
    {
        return false;
    }

    CrashLogger::Heartbeat("initialize-audio");
    ApplyWindowSettings();
    ApplyFrameRateLimit();
    SetExitKey(KEY_NULL);
    EnableCursor();
    const bool audioReady = audio_.Initialize();
    audio_.SetVolume(masterVolume_);
    audio_.SetCategoryVolumes(sfxVolume_, ambientVolume_);
    music_.Initialize(audioReady);
    music_.SetVolume(masterVolume_, musicVolume_);
    CrashLogger::Heartbeat("initialize-renderer");
    renderer_.Initialize();
    renderer_.SetWorldRenderDistance(kDrawDistances[drawDistanceIndex_]);
    renderer_.SetShadowQuality(shadowQuality_);
    CrashLogger::Heartbeat("initialize-post");
    postProcessor_.Initialize();

    CrashLogger::Heartbeat("initialize-network");
    network_.Start();
    network_.Connect();
    SetMessage("Выберите режим и начните матч.", 4.0f);
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

void Game::SetSelectedMode(MatchMode mode)
{
    selectedMode_ = mode;
    selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
    selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
}

void Game::SetSelectedTeamSize(int teamSize)
{
    selectedTeamSize_ = std::clamp(teamSize, 1, 4);
    selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
}

void Game::SetArenaLayout(ArenaLayout layout)
{
    arenaLayout_ = layout;
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

void Game::SetDevKeyboard(bool enabled)
{
    input_.SetDevKeyboard(enabled);
}

void Game::HandleInput()
{
    if (screen_ == GameScreen::MainMenu)
    {
        HandleMenuInput();
        return;
    }
    if (screen_ == GameScreen::Multiplayer)
    {
        HandleMultiplayerInput();
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

    if (matchSimulation_.HasWinner())
    {
        if (currentInput_.restartPressed)
        {
            SetupMatch();
            UpdateCamera(0.016f);
            SetMessage("Новый матч начался. Защищайте Кор.", 3.0f);
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
            SetMessage(std::string("Камера наблюдателя: ") + cameraController_.GetModeName(), 1.6f);
        }
#if DAIBED_DEVELOPER_BUILD
        if (currentInput_.botDebugPressed)
        {
            showBotDebug_ = !showBotDebug_;
            SetMessage(std::string("Отладка ботов: ") + (showBotDebug_ ? "вкл." : "выкл."), 1.4f);
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
        SetMessage(std::string("Камера: ") + cameraController_.GetModeName(), 1.6f);
    }
#if DAIBED_DEVELOPER_BUILD
    if (currentInput_.botDebugPressed)
    {
        showBotDebug_ = !showBotDebug_;
        SetMessage(std::string("Отладка ботов: ") + (showBotDebug_ ? "вкл." : "выкл."), 1.4f);
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
                purchaseMessage = "В этой строке магазина нет товара.";
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
                SetMessage("Выбран пустой слот.", 1.0f);
            }
            else
            {
                SetMessage(std::string("Выбрано: ") + ItemDisplayName(stack.type) + ".", 1.2f);
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
                SetMessage(std::string("Выбрано: ") + ItemDisplayName(stack.type) + ".", 0.8f);
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
            SetMessage("Отладочный респаун.");
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
        if (matchSimulation_.HasWinner())
        {
            mood = MusicMood::Victory;
        }
        else if (screen_ == GameScreen::Playing || screen_ == GameScreen::Paused)
        {
            mood = matchSimulation_.MatchTimeSeconds() >= kCoreCollapseSeconds - 90.0f ? MusicMood::Intense : MusicMood::Match;
        }
        music_.Update(mood);
    }

    if (screen_ != GameScreen::Playing)
    {
        return;
    }

    const float fixedDt = matchSimulation_.FixedDeltaSeconds();
    const int maxSteps = headless_ ? 1024 : 32;

    if (automatch_.active)
    {
        // Headless / automatch fast-forward: run a fixed number of fixed-size
        // ticks per frame, independent of the real frame rate. The simulation is
        // fixed-step, so stepping does NOT use the frame dt — determinism does
        // not depend on FPS. fixedDt == 1/60 == the value the old loop passed in
        // (Update is fed 1/60 by RunAutomatchBatch), so this is value-preserving.
        const int ticks = std::clamp(automatchTicksPerFrame_, 1, maxSteps);
        int stepped = 0;
        for (int i = 0; i < ticks; ++i)
        {
            StepSimulationProfiled(fixedDt);
            ++stepped;
            // Match the previous loop: stop the moment the batch finishes so we
            // don't keep ticking a completed match (keeps stats byte-identical).
            if (!automatch_.active)
            {
                break;
            }
        }
        lastSimStepCount_ = stepped;
        renderAlpha_ = 0.0f;
        // Age presentation by the simulated time during fast-forward (matches the
        // old per-tick tail); no-op when headless.
        UpdatePresentation(static_cast<float>(stepped) * fixedDt);
        return;
    }

    // Interactive: accumulate real frame time and advance the simulation at the
    // fixed rate. Render is decoupled — a frame may run 0, 1 or several steps.
    simulationAccumulator_ += dt;
    const float maxAccumulated = fixedDt * static_cast<float>(maxSteps);
    if (simulationAccumulator_ > maxAccumulated)
    {
        // Spiral-of-death guard: after a long stall (window drag, breakpoint),
        // drop the backlog instead of trying to simulate it all this frame.
        ++accumulatorClampCount_;
        simulationAccumulator_ = maxAccumulated;
    }

    // Fold this frame's local input into the pending sim input so one-shot edges
    // survive a zero-step frame and fire on exactly one tick.
    pendingLocalInput_ = MergePendingInput(pendingLocalInput_, currentInput_);
    const PlayerInput frameInput = currentInput_;

    int steps = 0;
    const auto stepsStarted = std::chrono::steady_clock::now();
    while (simulationAccumulator_ >= fixedDt)
    {
        currentInput_ = pendingLocalInput_;
        StepSimulationProfiled(fixedDt);
        ClearOneShotEdges(pendingLocalInput_);
        simulationAccumulator_ -= fixedDt;
        ++steps;
    }
    lastSimStepMs_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stepsStarted).count();
    lastSimStepCount_ = steps;
    currentInput_ = frameInput; // restore for per-frame presentation

    // Interpolation factor toward the next tick. Plumbed for future render
    // interpolation; not yet applied to entity rendering (deferred).
    renderAlpha_ = fixedDt > 0.0f ? simulationAccumulator_ / fixedDt : 0.0f;

    UpdatePresentation(dt);
}

void Game::StepSimulationProfiled(float dt)
{
    const auto started = profilingEnabled_
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point {};
    UpdateMatchSimulation(dt);
    if (profilingEnabled_)
    {
        profileSimulationMs_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        ++profileSimulationTicks_;
    }
}

void Game::UpdatePresentation(float dt)
{
    // Per-frame presentation, decoupled from the fixed-step simulation so it
    // stays smooth regardless of how many sim ticks ran this frame (0 at high
    // FPS, several after a stall). Placement preview + fast placement + the
    // network mock stay per-tick inside UpdateMatchSimulation (they are gameplay
    // / per-tick replication); only camera, feedback and the combat preview run
    // here per frame.
    if (headless_)
    {
        return;
    }
    UpdateCombatPreview();
    UpdateFeedback(dt);
    UpdateCamera(dt);
}

void Game::UpdateMatchSimulation(float dt)
{
    // MatchSimulation owns the authoritative simulation clock (single source of
    // truth for the tick). See docs/NETWORK_PREP_PLAN.md.
    matchSimulation_.AdvanceTick();
    if (!players_.empty())
    {
        simulationOrderOffset_ = (simulationOrderOffset_ + 1) % players_.size();
    }
    for (Player& player : players_)
    {
        player.UpdateTimers(dt);
    }
    UpdateHeroPassives(dt);

    if (!matchSimulation_.HasWinner())
    {
        matchSimulation_.AdvanceClock(dt);
        if (automatch_.active)
        {
            if (!coreCollapseTriggered_ && !coreCollapseWarned_ && matchSimulation_.MatchTimeSeconds() >= kCoreCollapseSeconds - 60.0f)
            {
                coreCollapseWarned_ = true;
            AddKillFeed("Внезапная смерть через 60 секунд", Color { 255, 118, 118, 255 }, 8.0f);
            }
            if (!coreCollapseTriggered_ && matchSimulation_.MatchTimeSeconds() >= kCoreCollapseSeconds)
            {
                TriggerCoreCollapse();
            }
        }
        if (!generatorBoostTriggered_ && matchSimulation_.MatchTimeSeconds() >= 10.0f * 60.0f)
        {
            generatorBoostTriggered_ = true;
            AddEventMessage("10:00 Скорость генераторов увеличена", Color { 255, 235, 142, 255 }, 5.0f);
            AddKillFeed("Генераторы ускорены", Color { 255, 235, 142, 255 }, 6.0f);
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
        matchSimulation_.SetWinner(rules_.CheckWinCondition(teams_, players_));
        if (!matchSimulation_.HasWinner() && coreCollapseTriggered_ && suddenDeathTiebreakTeamId_.has_value())
        {
            const bool anyTeamStillAlive = std::any_of(
                players_.begin(), players_.end(),
                [](const Player& player) { return !player.IsEliminated(); });
            if (!anyTeamStillAlive)
            {
                matchSimulation_.SetWinner(suddenDeathTiebreakTeamId_);
            }
        }
        if (matchSimulation_.HasWinner())
        {
            const Team* winner = FindTeam(matchSimulation_.WinnerTeamId());
            SetMessage((winner != nullptr ? winner->name : "Команда") + std::string(" побеждает!"), 8.0f);
            AddEventMessage((winner != nullptr ? winner->name : "Команда") + std::string(" побеждает!"), Color { 255, 235, 142, 255 }, 7.0f);
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
        // Per-tick local gameplay + per-tick network replication. The placement
        // preview is recomputed here because UpdateFastPlacement consumes it.
        // Camera / feedback / combat preview moved to UpdatePresentation (per
        // frame) so they stay smooth at render rates above the tick rate.
        UpdatePlacementPreview();
        UpdateFastPlacement(dt);
        SendMockNetworkInput();
    }
    UpdatePredictionStats(dt);
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
    if (screen_ == GameScreen::Multiplayer)
    {
        RenderMultiplayerMenu();
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
        matchSimulation_.Cores(),
        players_,
        matchSimulation_.Generators(),
        matchSimulation_.Pickups(),
        matchSimulation_.DroppedItems(),
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
            matchSimulation_.MatchTimeSeconds(),
            KeyLabel(input_.GetBindings().heroActive1),
            KeyLabel(input_.GetBindings().heroActive2),
            KeyLabel(input_.GetBindings().heroUltimate),
            sniperScopeBlend_,
            sniperMagnification_,
            matchSimulation_.Winner());

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
        if (tutorialMode_ || matchSimulation_.MatchTimeSeconds() < 72.0f)
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
            RenderNetworkDebugOverlay();
            // Fixed-step loop diagnostics (item 7): steps run this frame, the
            // interpolation alpha, time spent stepping, and how many times the
            // accumulator hit the spiral-of-death clamp.
            DrawText(
                TextFormat(
                    "sim-loop steps/frame=%d alpha=%.2f stepMs=%.2f clamps=%llu tick=%u acc=%.3fs",
                    lastSimStepCount_, renderAlpha_, lastSimStepMs_, accumulatorClampCount_,
                    matchSimulation_.CurrentTick(), simulationAccumulator_),
                22, GetScreenHeight() - 26, 14, Color { 128, 238, 166, 255 });
        }
#endif
        if (networkMode_ == NetworkMode::LocalClient && !showBotDebug_)
        {
            RenderNetworkDebugOverlay();
        }
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

void Game::RenderNetworkDebugOverlay() const
{
    const Color color = predictionCorrectionFlashTimer_ > 0.0f
        ? Color { 255, 201, 112, 255 }
        : Color { 112, 214, 255, 255 };
    DrawText(
        TextFormat(
            "net ping=%.0fms age=%.0fms loss=%.1f%% interp=%.0fms predErr=%.3fm corr/s=%.1f unacked=%d snapshots=%d authTick=%u",
            estimatedPingMs_, networkSnapshotAgeMs_, networkPacketLossEstimate_ * 100.0f,
            networkInterpolationDelayMs_,
            predictionError_, predictionCorrectionsPerSecond_, unackedCommandCount_,
            static_cast<int>(remoteSnapshotBuffer_.size()), lastAuthoritativeTick_),
        22, GetScreenHeight() - 44, 14, color);
    DrawText(
        TextFormat(
            "net rx=%.0fB/s %.1fpps full/delta=%u/%u size=%zu/%zu drop/ignore=%u/%u resync=%u",
            networkBytesPerSecond_, networkPacketsPerSecond_,
            networkFullSnapshots_, networkDeltaSnapshots_,
            networkLastFullSnapshotBytes_, networkLastDeltaSnapshotBytes_,
            networkDroppedSnapshots_, networkIgnoredSnapshots_, networkResyncRequests_),
        22, GetScreenHeight() - 62, 14, color);
}

void Game::TriggerCoreCollapse()
{
    coreCollapseTriggered_ = true;
    int destroyedCount = 0;
    for (EnergyCore& core : matchSimulation_.Cores())
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
        RemoveWorldBlock(core.GetBlockPosition(), BlockDeltaReason::CoreCollapse);
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
        AddEventMessage("Все Коры разрушены", Color { 255, 118, 118, 255 }, 6.0f);
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
        matchSimulation_.Pickups().push_back(ResourcePickup {
            type,
            amount,
            Vec3 { player.GetPosition().x, player.GetPosition().y + 0.35f, player.GetPosition().z },
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
            SetMessage(spectatorFreeCamera_ ? "Наблюдатель: свободная камера." : "Наблюдатель: слежение за игроком.", 1.5f);
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
    if (player == nullptr || !player->IsAlive() || localPlayerServerDriven_)
    {
        // localPlayerServerDriven_: an external authority (the server mock in
        // --network-smoke) applies the controlled player's PlayerCommand
        // directly, so the local update must not also self-drive it. The flag
        // defaults false, so normal play is unchanged.
        return;
    }

    // Movement, aim and selected slot are sourced from a typed PlayerCommand —
    // the same shape bots/network will produce — and applied via
    // ApplyPlayerCommand(). Utility inputs run first (they aim through the
    // camera, independent of the player's yaw). See NETWORK_PREP_PLAN.md for
    // which inputs already flow through the command and which still don't.
    const PlayerCommand command = BuildLocalPlayerCommand();
    UseUtilityInputs(*player, command);

    const bool wasOnGround = player->IsOnGround();
    const float fallingVelocity = player->GetVelocity().y;
    if (wasOnGround)
    {
        localAirPeakY_ = player->GetPosition().y;
    }

    ApplyPlayerCommand(*player, command, dt);
    ApplyStandingBlockEffects(*player, true);
    StorePredictedLocalCommand(command, *player);

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

void Game::ApplyPlayerCommand(Player& player, const PlayerCommand& command, float dt)
{
    // Aim comes from the command: yaw drives both facing and the movement
    // basis. Pitch is retained for the local player for future aim-dependent
    // actions (currently unused). For the local player command.aimYaw equals
    // the camera yaw, so this reproduces the previous behaviour exactly.
    player.SetYaw(command.aimYaw);
    if (command.selectedSlot >= 0 && command.selectedSlot < kHotbarSlotCount)
    {
        // The local player keeps its slot in selectedHotbarSlot_ (UI mirror);
        // network-controlled players carry their own slot so the authoritative
        // server replicates the right held item per player (Phase 0.1W).
        if (player.IsLocal())
        {
            selectedHotbarSlot_ = command.selectedSlot;
        }
        else
        {
            player.SetSelectedSlot(command.selectedSlot);
        }
    }
    if (player.IsLocal())
    {
        localAimPitch_ = command.aimPitch;
    }

    const bool hasMovementIntent = std::fabs(command.moveForward) > 0.0001f
        || std::fabs(command.moveStrafe) > 0.0001f
        || command.jump
        || command.sprint
        || command.sprintTapped
        || command.sneak;
    if (dt <= 0.0f && !hasMovementIntent)
    {
        return;
    }

    const Vector3 forward = player.Forward();
    const Vector3 right = player.Right();
    Vector3 wish {
        forward.x * command.moveForward + right.x * command.moveStrafe,
        0.0f,
        forward.z * command.moveForward + right.z * command.moveStrafe
    };
    // Aiming-down-sight slow + sprint modifiers now come from the command.
    const ItemType selectedItem = GetSelectedHotbarStack(player).type;
    const bool aimingBlaster = (selectedItem == ItemType::Blaster && command.placeHeld)
        || (selectedItem == ItemType::SniperRifle && command.scopeHeld);
    if (aimingBlaster)
    {
        wish.x *= 0.56f;
        wish.z *= 0.56f;
    }

    const bool wantsForwardSprint = command.moveForward > 0.05f;
    const bool sprint = command.sprint && wantsForwardSprint
        && !command.bridgeMode && !command.sneak && !aimingBlaster;
    if (command.sprintTapped && sprint)
    {
        player.RefreshSprintReset();
    }
    player.Move(
        wish,
        command.jump,
        dt,
        world_,
        sprint,
        command.sneak,
        TerrainSpeedMultiplier(player),
        !player.IsLocal(),
        BiomeGravityMultiplier(),
        BiomeJumpMultiplier(),
        BiomeGroundControlMultiplier(player),
        BiomeAirControlMultiplier());
}

void Game::RecordBlockDelta(const GridPos& pos, const Block& oldBlock, const Block& newBlock, BlockDeltaReason reason, int ownerPlayerId)
{
    if (SameBlock(oldBlock, newBlock))
    {
        return;
    }

    BlockDelta delta;
    delta.tick = matchSimulation_.CurrentTick();
    delta.position = pos;
    delta.oldType = oldBlock.type;
    delta.newType = newBlock.type;
    delta.oldTeamId = oldBlock.teamId;
    delta.newTeamId = newBlock.teamId;
    delta.ownerPlayerId = ownerPlayerId;
    delta.reason = reason;
    matchSimulation_.RecordBlockDelta(delta);
}

bool Game::PlaceWorldBlock(const GridPos& pos, const Block& block, bool allowReplace, BlockDeltaReason reason, int ownerPlayerId)
{
    const Block* oldBlockPtr = world_.GetBlock(pos);
    const Block oldBlock = oldBlockPtr != nullptr ? *oldBlockPtr : Block {};
    if (!world_.PlaceBlock(pos, block, allowReplace))
    {
        return false;
    }

    const Block* newBlockPtr = world_.GetBlock(pos);
    const Block newBlock = newBlockPtr != nullptr ? *newBlockPtr : Block {};
    RecordBlockDelta(pos, oldBlock, newBlock, reason, ownerPlayerId);
    return true;
}

bool Game::BreakWorldBlock(const GridPos& pos, int attackerTeam, BlockDeltaReason reason, int ownerPlayerId)
{
    const Block* oldBlockPtr = world_.GetBlock(pos);
    const Block oldBlock = oldBlockPtr != nullptr ? *oldBlockPtr : Block {};
    if (!world_.BreakBlock(pos, attackerTeam))
    {
        return false;
    }

    RecordBlockDelta(pos, oldBlock, Block {}, reason, ownerPlayerId);
    return true;
}

bool Game::RemoveWorldBlock(const GridPos& pos, BlockDeltaReason reason, int ownerPlayerId)
{
    const Block* oldBlockPtr = world_.GetBlock(pos);
    const Block oldBlock = oldBlockPtr != nullptr ? *oldBlockPtr : Block {};
    if (!world_.RemoveBlock(pos))
    {
        return false;
    }

    RecordBlockDelta(pos, oldBlock, Block {}, reason, ownerPlayerId);
    return true;
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
                        matchSimulation_.MatchTimeSeconds(),
                        voidDeath ? "voidFall" : "finalDeath",
                        player.GetTeamId(),
                        killerTeamId,
                        killerId,
                        player.GetId(),
                        finalDeath ? 1 : 0,
                        player.GetName() + (voidDeath ? " упал в воид" : " финальная смерть")
                    });
                }
            }
            SetMessage(player.GetName() + (finalDeath ? " выбывает." : " повержен и скоро возродится."));
            AddEventMessage(player.GetName() + (finalDeath ? " выбыл" : " повержен"), finalDeath ? RED : ORANGE, 2.2f);
            AddKillFeed(
                player.GetName()
                    + (killerId >= 0
                            ? " потерял инвентарь"
                            : (player.GetPosition().y < -12.0f ? " упал в воид" : " погиб")),
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
                        matchSimulation_.MatchTimeSeconds(),
                        "finalDeath",
                        player.GetTeamId(),
                        -1,
                        -1,
                        player.GetId(),
                        1,
                        player.GetName() + " потерял защиту респауна"
                    });
                }
                SetMessage(player.GetName() + " потерял защиту респауна. Финальная смерть.");
                if (player.IsLocal())
                {
                    EnterSpectatorMode();
                }
            }
            else if (player.GetRespawnTimer() <= 0.0f)
            {
                player.RespawnAtHome();
                SetMessage(player.GetName() + " возродился. " + BoolCoreState(team->coreAlive));
                AddWorldEffect(player.GetHomeSpawnPoint(), GetTeamColor(team->color), 0.42f, 0.45f);
            }
        }
    }
}

void Game::SendMockNetworkInput()
{
    // Legacy byte-level mock kept for connect/disconnect lifecycle parity.
    network_.SendToServer(NetworkMessage { NetworkMessageType::Input, static_cast<unsigned int>(localPlayerId_), "local_input_frame" });
    network_.Pump();
    (void)network_.ConsumeIncoming();

    // Forward-looking typed session: exercised live only when a network mode
    // beyond pure single-player is active (e.g. launched with --host). It has
    // no gameplay effect yet — the local simulation stays authoritative — but
    // proves the command -> authoritative tick -> snapshot path end to end with
    // the real match data. The transport layer will later replace the session.
    if (networkMode_ == NetworkMode::LocalSinglePlayer)
    {
        return;
    }
    if (!serverSession_.IsRunning())
    {
        serverSession_.Configure(serverConfig_);
        serverSession_.Start();
    }
    // Exercise the full Phase 0.1A path: transport (session) receives the
    // command, the server forwards it into MatchSimulation's intake queue, and
    // the simulation drains it. In host mode the local simulation already
    // applied the local player's command this tick (UpdateLocalPlayer), so the
    // drained command is NOT re-applied here — that would double-move the
    // player. The authoritative tick already advanced via
    // matchSimulation_.AdvanceTick() in UpdateMatchSimulation. Remote players
    // are what the server will actually apply from drained commands later.
    serverSession_.SubmitCommand(BuildLocalPlayerCommand());
    for (const PlayerCommand& received : serverSession_.DrainCommands())
    {
        matchSimulation_.SubmitCommand(received);
    }
    (void)matchSimulation_.DrainCommands();
    const MatchSnapshot snapshot = BuildNetworkSnapshot();
    serverSession_.PublishSnapshot(snapshot);
    PushRemoteSnapshot(snapshot);
    ApplyAuthoritativeSnapshotForPrediction(snapshot, matchSimulation_.FixedDeltaSeconds());
    matchSimulation_.ClearBlockDeltasThrough(snapshot.tick);
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

    return std::to_string(size) + " в команде";
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
    return std::to_string(std::clamp(automatchMaxMinutes_, 3, 30)) + " мин";
}

int Game::GetForgeBonusForTeam(int teamId) const
{
    const Team* team = FindTeam(teamId);
    if (team == nullptr)
    {
        return 0;
    }

    const int tenMinuteBoost = matchSimulation_.MatchTimeSeconds() >= 10.0f * 60.0f ? 1 : 0;
    return std::min(3, team->forgeLevel / 2 + tenMinuteBoost);
}

bool Game::RepairTeamCore(Player& player, Team& team, std::string& message)
{
    EnergyCore* core = FindCoreByTeam(team.id);
    if (core == nullptr || !core->IsAlive())
    {
        message = "Кор нельзя починить после уничтожения.";
        return false;
    }
    if (core->GetHealth() >= core->GetMaxHealth())
    {
        message = "Кор уже полностью починен.";
        return false;
    }
    Inventory& inventory = player.GetInventory();
    if (inventory.GetResource(ResourceType::Crystal) < 2 || inventory.GetResource(ResourceType::Gold) < 3)
    {
        message = "Нужно 2 кристалла + 3 золота для ремонта Кора.";
        return false;
    }

    const int beforeHealth = core->GetHealth();
    inventory.SpendResource(ResourceType::Crystal, 2);
    inventory.SpendResource(ResourceType::Gold, 3);
    core->Repair(30);
    message = "Кор починен +" + std::to_string(core->GetHealth() - beforeHealth) + ".";
    AddWorldEffect(world_.GridToWorld(core->GetBlockPosition()), Color { 112, 232, 255, 255 }, 0.55f, 0.50f);
    AddKillFeed(team.name + " Кор починен", Color { 112, 232, 255, 255 }, 4.0f);
    return true;
}

Player* Game::GetLocalPlayer()
{
    return matchSimulation_.GetPlayer(localPlayerId_);
}

const Player* Game::GetLocalPlayer() const
{
    return matchSimulation_.GetPlayer(localPlayerId_);
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
    SetMessage("Финальная смерть. Режим наблюдателя включен.", 4.0f);
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
            SetMessage("Наблюдение за " + players_[index].GetName(), 1.4f);
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
    for (EnergyCore& core : matchSimulation_.Cores())
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
    for (EnergyCore& core : matchSimulation_.Cores())
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
    // The local player reads the UI slot (selectedHotbarSlot_); a network-
    // controlled player reads its own replicated slot. Bots have no held-item
    // concept (their combat path doesn't use this) — keep returning empty so
    // their behaviour and automatch determinism are unchanged.
    int slot;
    if (player.IsLocal())
    {
        slot = selectedHotbarSlot_;
    }
    else if (IsNetworkControlledPlayer(player.GetId()))
    {
        slot = player.GetSelectedSlot();
    }
    else
    {
        return ItemStack {};
    }

    const Inventory& inventory = player.GetInventory();
    if (slot < 0 || slot >= kHotbarSlotCount)
    {
        return ItemStack {};
    }
    return inventory.GetHotbarSlots()[slot];
}

bool Game::IsSniperScopeRequested(const Player& player) const
{
    return player.IsAlive()
        && !shopOpen_
        && !inventoryOpen_
        && !matchSimulation_.HasWinner()
        && BuildLocalPlayerCommand().scopeHeld
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
    // Bots have no held-item concept and always melee with a sword. The local
    // player and network-controlled players use their actually selected item.
    if (!player.IsLocal() && !IsNetworkControlledPlayer(player.GetId()))
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
    if (!player.IsLocal() && !IsNetworkControlledPlayer(player.GetId()))
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
        AddFloatingText("Комбо", Vector3 { event.position.x, event.position.y + 0.28f, event.position.z }, Color { 255, 235, 142, 255 });
    }
    else if (event.sprintReset)
    {
        AddFloatingText("W-tap", Vector3 { event.position.x, event.position.y + 0.28f, event.position.z }, Color { 188, 238, 255, 255 });
    }
    if (voidThreat)
    {
        AddFloatingText("Удар в воид", Vector3 { event.position.x, event.position.y + 0.52f, event.position.z }, Color { 255, 155, 118, 255 });
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
                AddFloatingText("НАРУШИТЕЛЬ -" + std::to_string(bonusDamage), targetPlayer->GetPosition(), HeroAccentColor(HeroId::Konvoy));
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
                    NoteDamageCredit(targetPlayer->GetId(), attackerPlayer->GetId(), "кровотечением Лихо");
                    AddFloatingText("КРОВОТЕЧЕНИЕ -12", targetPlayer->GetPosition(), HeroAccentColor(HeroId::Likho));
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
                    automatch_.currentFirstCoreDamageTime = matchSimulation_.MatchTimeSeconds();
                    automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                        matchSimulation_.MatchTimeSeconds(),
                        "firstCoreDamage",
                        event.targetTeamId,
                        attackerTeamId,
                        event.attackerId,
                        -1,
                        event.damage,
                        "Первый урон Кору"
                    });
                }
                if (event.coreDestroyed)
                {
                    automatch_.currentTimeline.push_back(AutomatchTimelineEvent {
                        matchSimulation_.MatchTimeSeconds(),
                        "coreDestroyed",
                        event.targetTeamId,
                        attackerTeamId,
                        event.attackerId,
                        -1,
                        event.damage,
                        std::string(TeamName(event.targetTeamId)) + " Кор уничтожен"
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
        AddKillFeed(std::string(TeamName(event.targetTeamId)) + " Кор уничтожен", Color { 255, 118, 118, 255 }, 7.0f);
    }
    else if (event.coreHit)
    {
        AddKillFeed(std::string(TeamName(event.targetTeamId)) + " Кор -" + std::to_string(event.damage), Color { 112, 232, 255, 255 }, 3.8f);
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
        const std::string attackerName = attacker != nullptr ? attacker->GetName() : "Неизвестно";
        const std::string targetName = target != nullptr ? target->GetName() : "Враг";
        AddKillFeed(attackerName + " устранил " + targetName, Color { 255, 235, 142, 255 }, 5.5f);
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

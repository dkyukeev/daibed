#include "Game.h"

#include "CrashLogger.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
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
constexpr float kCoreCollapseSeconds = 30.0f * 60.0f;
constexpr float kItemPickupRadiusSq = 1.35f;
constexpr float kItemMagnetRadius = 2.35f;
constexpr float kItemMagnetRadiusSq = kItemMagnetRadius * kItemMagnetRadius;
constexpr float kItemMergeRadiusSq = 0.85f * 0.85f;
constexpr float kResourceMagnetSpeed = 5.8f;
constexpr float kDroppedItemMagnetAccel = 28.0f;
constexpr float kDroppedItemMagnetMaxSpeed = 7.0f;
constexpr int kBuildMinY = -2;
constexpr int kBuildMaxY = 64;
constexpr int kBuildMapRadius = 72;

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
    { 0, "Unlimited" }
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

float Distance3D(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::string JsonEscape(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value)
    {
        switch (ch)
        {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += ch;
            break;
        }
    }
    return escaped;
}

void WriteJsonVector3(std::ofstream& file, Vector3 value)
{
    file << "{\"x\":" << value.x << ",\"y\":" << value.y << ",\"z\":" << value.z << "}";
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

Vector3 PickupTargetFor(const Player& player)
{
    const Vector3 pos = player.GetPosition();
    return Vector3 { pos.x, pos.y + 0.35f, pos.z };
}

Player* FindMagnetTarget(std::vector<Player>& players, Vector3 itemPosition, int ownerPlayerId, float ownerPickupDelay, float itemAge)
{
    Player* target = nullptr;
    float bestDistance = kItemMagnetRadiusSq;
    for (Player& player : players)
    {
        if (!player.IsAlive() || player.IsEliminated())
        {
            continue;
        }
        if (player.GetId() == ownerPlayerId && itemAge < ownerPickupDelay)
        {
            continue;
        }

        const float distance = DistanceSquared(PickupTargetFor(player), itemPosition);
        if (distance <= bestDistance)
        {
            bestDistance = distance;
            target = &player;
        }
    }
    return target;
}

void MergeNearbyResourcePickups(std::vector<ResourcePickup>& pickups)
{
    for (std::size_t i = 0; i < pickups.size(); ++i)
    {
        ResourcePickup& target = pickups[i];
        if (target.collected)
        {
            continue;
        }

        for (std::size_t j = i + 1; j < pickups.size(); ++j)
        {
            ResourcePickup& other = pickups[j];
            if (other.collected || other.type != target.type)
            {
                continue;
            }
            if (DistanceSquared(target.position, other.position) > kItemMergeRadiusSq)
            {
                continue;
            }

            const int total = target.amount + other.amount;
            target.position = Vector3 {
                (target.position.x * static_cast<float>(target.amount) + other.position.x * static_cast<float>(other.amount)) / static_cast<float>(total),
                (target.position.y * static_cast<float>(target.amount) + other.position.y * static_cast<float>(other.amount)) / static_cast<float>(total),
                (target.position.z * static_cast<float>(target.amount) + other.position.z * static_cast<float>(other.amount)) / static_cast<float>(total)
            };
            target.amount = total;
            target.radius = std::max(target.radius, other.radius);
            target.lifetime = std::max(target.lifetime, other.lifetime);
            other.collected = true;
        }
    }
}

void MergeNearbyDroppedItems(std::vector<DroppedItem>& droppedItems)
{
    for (std::size_t i = 0; i < droppedItems.size(); ++i)
    {
        DroppedItem& target = droppedItems[i];
        if (target.collected || target.stack.IsEmpty() || target.age < target.ownerPickupDelay)
        {
            continue;
        }

        for (std::size_t j = i + 1; j < droppedItems.size(); ++j)
        {
            DroppedItem& other = droppedItems[j];
            if (other.collected || other.stack.IsEmpty() || other.stack.type != target.stack.type || other.age < other.ownerPickupDelay)
            {
                continue;
            }
            if (DistanceSquared(target.position, other.position) > kItemMergeRadiusSq)
            {
                continue;
            }

            const int total = target.stack.count + other.stack.count;
            target.position = Vector3 {
                (target.position.x * static_cast<float>(target.stack.count) + other.position.x * static_cast<float>(other.stack.count)) / static_cast<float>(total),
                (target.position.y * static_cast<float>(target.stack.count) + other.position.y * static_cast<float>(other.stack.count)) / static_cast<float>(total),
                (target.position.z * static_cast<float>(target.stack.count) + other.position.z * static_cast<float>(other.stack.count)) / static_cast<float>(total)
            };
            target.velocity = Vector3 {
                (target.velocity.x + other.velocity.x) * 0.35f,
                std::max(target.velocity.y, other.velocity.y) * 0.25f,
                (target.velocity.z + other.velocity.z) * 0.35f
            };
            target.stack.count = total;
            target.lifetime = std::max(target.lifetime, other.lifetime);
            target.ownerPlayerId = -1;
            target.ownerPickupDelay = 0.0f;
            other.collected = true;
        }
    }
}

float YawFromDirection(Vector3 direction)
{
    return std::atan2(direction.x, -direction.z);
}

std::string BoolCoreState(bool alive)
{
    return alive ? "Core online." : "Core destroyed. Final Life!";
}

std::string FormatTenths(float value)
{
    const int tenths = static_cast<int>(value * 10.0f + 0.5f);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

float YawForTeam(int teamId)
{
    switch (teamId)
    {
    case 0:
        return kPi * 0.5f;
    case 1:
        return -kPi * 0.5f;
    case 2:
        return kPi;
    case 3:
        return 0.0f;
    default:
        return 0.0f;
    }
}

int OppositeTeam(int teamId)
{
    return teamId == 0 ? 1 : 0;
}

void PlaceMapBlock(World& world, GridPos pos, BlockType type, int teamId = -1, bool breakable = false)
{
    world.PlaceBlock(pos, Block { type, teamId, breakable }, true);
}

void AddOvalLayer(World& world, Vector3 center, int radiusX, int radiusZ, int y, BlockType type, bool replace = true)
{
    const int cx = static_cast<int>(std::round(center.x));
    const int cz = static_cast<int>(std::round(center.z));
    const float rx = static_cast<float>(std::max(1, radiusX));
    const float rz = static_cast<float>(std::max(1, radiusZ));

    for (int x = -radiusX; x <= radiusX; ++x)
    {
        for (int z = -radiusZ; z <= radiusZ; ++z)
        {
            const float nx = static_cast<float>(x) / rx;
            const float nz = static_cast<float>(z) / rz;
            const GridPos pos { cx + x, y, cz + z };
            if (nx * nx + nz * nz <= 1.0f && (replace || world.IsAir(pos)))
            {
                PlaceMapBlock(world, pos, type);
            }
        }
    }
}

void AddClassicIsland(World& world, Vector3 center, int radiusX, int radiusZ, int y = 0)
{
    AddOvalLayer(world, center, radiusX, radiusZ, y, BlockType::GrassBlock);
    AddOvalLayer(world, center, std::max(1, radiusX - 1), std::max(1, radiusZ - 1), y - 1, BlockType::DirtBlock);
    AddOvalLayer(world, center, std::max(1, radiusX - 3), std::max(1, radiusZ - 3), y - 2, BlockType::DirtBlock);
}

void AddTree(World& world, GridPos base)
{
    for (int y = base.y; y <= base.y + 2; ++y)
    {
        PlaceMapBlock(world, GridPos { base.x, y, base.z }, BlockType::WoodBlock);
    }

    for (int x = -1; x <= 1; ++x)
    {
        for (int z = -1; z <= 1; ++z)
        {
            PlaceMapBlock(world, GridPos { base.x + x, base.y + 3, base.z + z }, BlockType::LeafBlock);
            if (std::abs(x) + std::abs(z) <= 1)
            {
                PlaceMapBlock(world, GridPos { base.x + x, base.y + 4, base.z + z }, BlockType::LeafBlock);
            }
        }
    }
}

void AddWoodDock(World& world, GridPos start, int length, int stepX, int stepZ)
{
    for (int i = 0; i < length; ++i)
    {
        PlaceMapBlock(
            world,
            GridPos { start.x + stepX * i, start.y, start.z + stepZ * i },
            BlockType::WoodBlock);
    }
}

void AddRectLayer(World& world, GridPos center, int halfX, int halfZ, BlockType type, bool breakable = false)
{
    for (int x = -halfX; x <= halfX; ++x)
    {
        for (int z = -halfZ; z <= halfZ; ++z)
        {
            PlaceMapBlock(world, GridPos { center.x + x, center.y, center.z + z }, type, -1, breakable);
        }
    }
}

void AddLineBridge(World& world, GridPos from, GridPos to, int width, BlockType type, bool breakable = false)
{
    const int dx = (to.x > from.x) ? 1 : (to.x < from.x ? -1 : 0);
    const int dz = (to.z > from.z) ? 1 : (to.z < from.z ? -1 : 0);
    const int steps = std::max(std::abs(to.x - from.x), std::abs(to.z - from.z));
    const bool xMajor = std::abs(to.x - from.x) >= std::abs(to.z - from.z);

    for (int i = 0; i <= steps; ++i)
    {
        const int x = from.x + dx * std::min(i, std::abs(to.x - from.x));
        const int z = from.z + dz * std::min(i, std::abs(to.z - from.z));
        const int sideMin = -width / 2;
        const int sideMax = width - 1 + sideMin;
        for (int side = sideMin; side <= sideMax; ++side)
        {
            const GridPos pos {
                x + (xMajor ? 0 : side),
                from.y,
                z + (xMajor ? side : 0)
            };
            PlaceMapBlock(world, pos, type, -1, breakable);
        }
    }
}

void AddSteppedBridge(World& world, GridPos from, GridPos to, int width, BlockType type, bool breakable = false)
{
    const int dx = (to.x > from.x) ? 1 : (to.x < from.x ? -1 : 0);
    const int dz = (to.z > from.z) ? 1 : (to.z < from.z ? -1 : 0);
    const int steps = std::max(std::abs(to.x - from.x), std::abs(to.z - from.z));
    const bool xMajor = std::abs(to.x - from.x) >= std::abs(to.z - from.z);

    for (int i = 0; i <= steps; ++i)
    {
        const float t = steps > 0 ? static_cast<float>(i) / static_cast<float>(steps) : 0.0f;
        const int y = static_cast<int>(std::round(static_cast<float>(from.y) + static_cast<float>(to.y - from.y) * t));
        const int x = from.x + dx * std::min(i, std::abs(to.x - from.x));
        const int z = from.z + dz * std::min(i, std::abs(to.z - from.z));
        const int sideMin = -width / 2;
        const int sideMax = width - 1 + sideMin;
        for (int side = sideMin; side <= sideMax; ++side)
        {
            const GridPos pos {
                x + (xMajor ? 0 : side),
                y,
                z + (xMajor ? side : 0)
            };
            PlaceMapBlock(world, pos, type, -1, breakable);
        }
    }
}

void AddSquareRing(World& world, GridPos center, int innerRadius, int outerRadius, BlockType type, bool breakable = false)
{
    for (int x = -outerRadius; x <= outerRadius; ++x)
    {
        for (int z = -outerRadius; z <= outerRadius; ++z)
        {
            const int radius = std::max(std::abs(x), std::abs(z));
            if (radius >= innerRadius && radius <= outerRadius)
            {
                PlaceMapBlock(world, GridPos { center.x + x, center.y, center.z + z }, type, -1, breakable);
            }
        }
    }
}

void AddCenterMonument(World& world)
{
    AddOvalLayer(world, Vector3 { 0.0f, 0.0f, 0.0f }, 4, 4, 1, BlockType::GrassBlock);
    AddOvalLayer(world, Vector3 { 0.0f, 0.0f, 0.0f }, 2, 2, 2, BlockType::StoneBlock);

    const GridPos columns[] {
        GridPos { -2, 1, -2 },
        GridPos { 2, 1, -2 },
        GridPos { -2, 1, 2 },
        GridPos { 2, 1, 2 }
    };
    for (const GridPos& column : columns)
    {
        for (int y = 1; y <= 4; ++y)
        {
            PlaceMapBlock(world, GridPos { column.x, y, column.z }, BlockType::StoneBlock);
        }
    }

    PlaceMapBlock(world, GridPos { 0, 3, 0 }, BlockType::EnergyGlassBlock);
}

}

bool Game::Initialize()
{
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    LoadSettings();
    resolutionIndex_ = std::clamp(resolutionIndex_, 0, static_cast<int>(std::size(kWindowResolutions)) - 1);
    const WindowResolution& resolution = kWindowResolutions[resolutionIndex_];
    InitWindow(resolution.width, resolution.height, "DaiBed");
    if (!IsWindowReady())
    {
        return false;
    }

    ApplyFrameRateLimit();
    SetExitKey(KEY_NULL);
    EnableCursor();
    input_.SetMouseSensitivity(1.0f);
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);
    audio_.Initialize();
    renderer_.Initialize();

    network_.Start();
    network_.Connect();
    SetMessage("Choose a mode and start the match.", 4.0f);
    return true;
}

void Game::Shutdown()
{
    SaveSettings();
    network_.Disconnect();
    network_.Stop();
    renderer_.Shutdown();
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

void Game::HandleInput()
{
    if (screen_ == GameScreen::MainMenu)
    {
        HandleMenuInput();
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
        if (currentInput_.botDebugPressed)
        {
            showBotDebug_ = !showBotDebug_;
            SetMessage(std::string("Bot debug: ") + (showBotDebug_ ? "on" : "off"), 1.4f);
        }
        if (winnerTeamId_.has_value() && currentInput_.restartPressed)
        {
            SetupMatch();
            UpdateCamera(0.016f);
            SetMessage("New match started. Protect your EnergyCore.", 3.0f);
        }
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
    if (currentInput_.botDebugPressed)
    {
        showBotDebug_ = !showBotDebug_;
        SetMessage(std::string("Bot debug: ") + (showBotDebug_ ? "on" : "off"), 1.4f);
    }

    if (winnerTeamId_.has_value())
    {
        if (currentInput_.restartPressed)
        {
            SetupMatch();
            UpdateCamera(0.016f);
            SetMessage("New match started. Protect your EnergyCore.", 3.0f);
        }
        return;
    }

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
        const int direction = currentInput_.mouseWheel > 0.0f ? -1 : 1;
        selectedHotbarSlot_ = (selectedHotbarSlot_ + direction + kHotbarSlotCount) % kHotbarSlotCount;
        const ItemStack stack = localPlayer->GetInventory().GetHotbarSlots()[selectedHotbarSlot_];
        if (!stack.IsEmpty())
        {
            SetMessage(std::string("Selected ") + ItemDisplayName(stack.type) + ".", 0.8f);
        }
    }

    if (currentInput_.debugRespawnPressed)
    {
        Team* team = FindTeam(localPlayer->GetTeamId());
        if (team != nullptr && team->coreAlive && !localPlayer->IsEliminated())
        {
            localPlayer->Respawn(team->spawnPoint);
            SetMessage("Debug respawn.");
        }
    }

    if (currentInput_.placePressed)
    {
        UseSelectedItem(*localPlayer);
        fastPlaceTimer_ = currentInput_.bridgeMode ? 0.16f : 0.22f;
    }
}

void Game::Update(float dt)
{
    dt = std::min(dt, 0.05f);

    if (screen_ != GameScreen::Playing)
    {
        return;
    }

    const int ticks = automatch_.active ? std::clamp(automatchTicksPerFrame_, 1, 32) : 1;
    for (int i = 0; i < ticks; ++i)
    {
        UpdateMatchSimulation(dt);
        if (!automatch_.active)
        {
            break;
        }
    }
}

void Game::UpdateMatchSimulation(float dt)
{
    for (Player& player : players_)
    {
        player.UpdateTimers(dt);
    }

    if (!winnerTeamId_.has_value())
    {
        matchTime_ += dt;
        if (!coreCollapseTriggered_ && matchTime_ >= kCoreCollapseSeconds)
        {
            TriggerCoreCollapse();
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
        UpdateBots(dt);
        UpdateGenerators(dt);
        UpdatePickups(dt);
        UpdateDroppedItems(dt);
        UpdateBlockHazards(dt);
        UpdateExplosives(dt);
        UpdateProjectiles(dt);
        UpdateHazardZones(dt);
        UpdateAlarmTraps();
        UpdatePassiveRegeneration(dt);
        UpdateBaseHealing(dt);
        UpdateDamageCredits(dt);
        HandleDeathsAndRespawns();
        winnerTeamId_ = rules_.CheckWinCondition(teams_, players_);
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

    UpdatePlacementPreview();
    UpdateCombatPreview();
    UpdateFastPlacement(dt);
    UpdateFeedback(dt);
    SendMockNetworkInput();
    UpdateCamera(dt);
}

void Game::Render()
{
    BeginDrawing();
    const bool inWorldView = screen_ == GameScreen::Playing || screen_ == GameScreen::Paused;
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
    renderer_.RenderScene(
        world_,
        teams_,
        cores_,
        players_,
        generators_,
        pickups_,
        droppedItems_,
        placementPreview_,
        worldEffects_,
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
            selectedHotbarSlot_,
            inventoryOpen_,
            inventoryCursorSlot_,
            heldInventoryStack_,
            cameraController_.GetModeName(),
            eventMessages_,
            stats_,
            hitMarkerTimer_,
            damageFlashTimer_,
            matchTime_,
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
        const Team* localTeam = FindTeam(localPlayer->GetTeamId());
        if (localTeam != nullptr && localTeam->enemyTrackerUnlocked)
        {
            RenderCompass(*localPlayer);
        }
        if (showBotDebug_)
        {
            RenderBotDebug();
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

void Game::SetupMatch()
{
    world_.Clear();
    teams_.clear();
    cores_.clear();
    players_.clear();
    generators_.clear();
    pickups_.clear();
    worldEffects_.clear();
    timedExplosions_.clear();
    projectiles_.clear();
    hazardZones_.clear();
    alarmTraps_.clear();
    droppedItems_.clear();
    floatingTexts_.clear();
    eventMessages_.clear();
    killFeed_.clear();
    playerScores_.clear();
    damageCredits_.clear();
    botMemories_.clear();
    teamChests_ = {};
    personalChest_ = Inventory {};
    placementPreview_ = PlacementPreview {};
    breakProgress_ = BreakProgress {};
    combatPreview_ = CombatPreview {};
    stats_ = MatchStats {};
    hitMarkerTimer_ = 0.0f;
    damageFlashTimer_ = 0.0f;
    matchTime_ = 0.0f;
    passiveRegenTimer_ = 0.0f;
    baseHealTimer_ = 0.0f;
    fastPlaceTimer_ = 0.0f;
    localFallVelocity_ = 0.0f;
    localAirPeakY_ = 0.0f;
    localWasOnGround_ = false;
    winnerTeamId_.reset();
    coreCollapseTriggered_ = false;
    generatorBoostTriggered_ = false;
    shopOpen_ = false;
    selectedHotbarSlot_ = 0;
    inventoryOpen_ = false;
    CloseChest();
    inventoryCursorSlot_ = 0;
    heldInventoryStack_ = ItemStack {};
    attackChargeActive_ = false;
    attackChargeTimer_ = 0.0f;
    shopCategoryIndex_ = 0;
    selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
    selectedTeamSize_ = std::clamp(selectedTeamSize_, 1, 4);
    selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
    scoreboardHeld_ = false;
    spectatorMode_ = false;
    spectatorFreeCamera_ = false;
    spectatorTargetIndex_ = 0;
    spectatorPosition_ = Vector3 {};

    Team red {
        0,
        TeamColor::Red,
        "Red",
        Vector3 { -38.0f, 1.5f, 0.0f },
        GridPos { -30, 1, 0 },
        Vector3 { -40.0f, 0.58f, 3.0f },
        true,
        0,
        0
    };

    Team blue {
        1,
        TeamColor::Blue,
        "Blue",
        Vector3 { 38.0f, 1.5f, 0.0f },
        GridPos { 30, 1, 0 },
        Vector3 { 40.0f, 0.58f, -3.0f },
        true,
        0,
        0
    };

    Team green {
        2,
        TeamColor::Green,
        "Green",
        Vector3 { 0.0f, 1.5f, -38.0f },
        GridPos { 0, 1, -30 },
        Vector3 { 3.0f, 0.58f, -40.0f },
        true,
        0,
        0
    };

    Team yellow {
        3,
        TeamColor::Yellow,
        "Yellow",
        Vector3 { 0.0f, 1.5f, 38.0f },
        GridPos { 0, 1, 30 },
        Vector3 { -3.0f, 0.58f, 40.0f },
        true,
        0,
        0
    };

    teams_.push_back(red);
    teams_.push_back(blue);
    teams_.push_back(green);
    teams_.push_back(yellow);

    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        AddFrozenRingLayout();
        break;
    case ArenaBiome::Lava:
        AddMoltenLayersLayout();
        break;
    case ArenaBiome::Space:
        AddOrbitalShardsLayout();
        break;
    case ArenaBiome::Ruins:
        AddBrokenCitadelLayout();
        break;
    case ArenaBiome::Arena:
        AddClassicArenaLayout();
        if (arenaLayout_ == ArenaLayout::Vertical)
        {
            AddVerticalArenaFeatures();
        }
        break;
    }

    for (const Team& team : teams_)
    {
        if (!IsTeamActiveForMode(team.id))
        {
            continue;
        }
        world_.PlaceBlock(team.coreBlock, Block { BlockType::EnergyCoreBlock, team.id, false }, true);
        cores_.emplace_back(team.id, team.coreBlock, 120);
        world_.PlaceBlock(GridPos { team.coreBlock.x + 1, team.coreBlock.y, team.coreBlock.z }, Block { BlockType::StoneBlock, team.id, true }, true);
        world_.PlaceBlock(GridPos { team.coreBlock.x - 1, team.coreBlock.y, team.coreBlock.z }, Block { BlockType::WoolBlock, team.id, true }, true);
        world_.PlaceBlock(GridPos { team.coreBlock.x, team.coreBlock.y, team.coreBlock.z + 1 }, Block { BlockType::WoolBlock, team.id, true }, true);
        world_.PlaceBlock(GridPos { team.coreBlock.x, team.coreBlock.y, team.coreBlock.z - 1 }, Block { BlockType::WoolBlock, team.id, true }, true);
    }

    SetupGenerators();

    const Team* localTeam = FindTeam(selectedTeamId_);
    if (localTeam == nullptr)
    {
        localTeam = &teams_.front();
        selectedTeamId_ = localTeam->id;
    }

    Player local(localPlayerId_, "Local Runner", selectedTeamId_, localTeam->spawnPoint, true);
    local.SetYaw(YawForTeam(selectedTeamId_));
    local.GetInventory().AddItem(ItemType::Sword, 1);
    local.GetInventory().AddBlock(BlockType::WoodBlock, 24);
    local.GetInventory().AddBlock(BlockType::WoolBlock, 32);
    local.GetInventory().AddBlock(BlockType::StoneBlock, 8);

    players_.push_back(local);

    int nextBotId = 2;
    std::array<int, 4> teamRoster {};
    teamRoster[selectedTeamId_] = 1;
    int botsRemaining = selectedBotCount_;
    const auto addBot = [this, &nextBotId, &teamRoster, &botsRemaining](int teamId)
    {
        const Team* team = FindTeam(teamId);
        if (team == nullptr || botsRemaining <= 0 || teamRoster[teamId] >= selectedTeamSize_)
        {
            return;
        }

        const bool ally = teamId == selectedTeamId_;
        const std::string name = std::string(ally ? "Ally" : TeamName(teamId))
            + " Bot " + std::to_string(teamRoster[teamId] + 1);
        Player bot(nextBotId++, name, teamId, team->spawnPoint, false);
        bot.SetYaw(YawForTeam(teamId));
        ApplyBotLoadout(bot);
        players_.push_back(bot);
        ++teamRoster[teamId];
        --botsRemaining;
    };

    std::vector<int> enemyTeams;
    for (const Team& team : teams_)
    {
        if (team.id != selectedTeamId_ && IsTeamActiveForMode(team.id))
        {
            enemyTeams.push_back(team.id);
        }
    }
    if (TeamCountForMode() == 2 && !enemyTeams.empty())
    {
        const int enemyTeamId = enemyTeams.front();
        while (botsRemaining > 0 && teamRoster[enemyTeamId] < selectedTeamSize_)
        {
            addBot(enemyTeamId);
        }
        while (botsRemaining > 0 && teamRoster[selectedTeamId_] < selectedTeamSize_)
        {
            addBot(selectedTeamId_);
        }
    }
    else
    {
        bool added = true;
        while (botsRemaining > 0 && added)
        {
            added = false;
            for (int teamId : enemyTeams)
            {
                if (botsRemaining <= 0)
                {
                    break;
                }
                if (teamRoster[teamId] < selectedTeamSize_)
                {
                    addBot(teamId);
                    added = true;
                }
            }
        }
        while (botsRemaining > 0 && teamRoster[selectedTeamId_] < selectedTeamSize_)
        {
            addBot(selectedTeamId_);
        }
    }

    for (Player& player : players_)
    {
        GetPlayerScore(player.GetId());
    }

    if (Player* localPlayer = GetLocalPlayer())
    {
        cameraController_.Reset(localPlayer->GetYaw(), -0.14f, localPlayer->GetPosition());
        localWasOnGround_ = localPlayer->IsOnGround();
        localAirPeakY_ = localPlayer->GetPosition().y;
    }

    AddEventMessage("Collect resources at your base generator", Color { 188, 198, 210, 255 }, 5.0f);
    AddEventMessage("Buy blocks, bridge to center, crack enemy defenses", Color { 255, 235, 142, 255 }, 5.5f);
    AddEventMessage("Hold LMB to break blocks or damage an EnergyCore", Color { 112, 232, 255, 255 }, 6.0f);
    AddEventMessage("Gold crosshair = enemy in melee range", Color { 255, 235, 142, 255 }, 6.5f);
    if (arenaBiome_ != ArenaBiome::Arena)
    {
        AddEventMessage(std::string("Biome rule: ") + ArenaBiomeName(), BiomeFogColor(), 6.8f);
    }
}

void Game::AddClassicArenaLayout()
{
    AddClassicIsland(world_, Vector3 { -36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -36.0f }, 7, 10);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 36.0f }, 7, 10);

    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 0.0f }, 12, 12);
    AddClassicIsland(world_, Vector3 { -18.0f, 0.0f, 0.0f }, 5, 4);
    AddClassicIsland(world_, Vector3 { 18.0f, 0.0f, 0.0f }, 5, 4);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -18.0f }, 4, 5);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 18.0f }, 4, 5);
    AddClassicIsland(world_, Vector3 { -20.0f, 0.0f, -20.0f }, 4, 4);
    AddClassicIsland(world_, Vector3 { 20.0f, 0.0f, -20.0f }, 4, 4);
    AddClassicIsland(world_, Vector3 { -20.0f, 0.0f, 20.0f }, 4, 4);
    AddClassicIsland(world_, Vector3 { 20.0f, 0.0f, 20.0f }, 4, 4);

    AddWoodDock(world_, GridPos { -28, 0, 0 }, 4, 1, 0);
    AddWoodDock(world_, GridPos { 28, 0, 0 }, 4, -1, 0);
    AddWoodDock(world_, GridPos { 0, 0, -28 }, 4, 0, 1);
    AddWoodDock(world_, GridPos { 0, 0, 28 }, 4, 0, -1);
    AddCenterMonument(world_);

    AddTree(world_, GridPos { -39, 1, -5 });
    AddTree(world_, GridPos { -38, 1, 5 });
    AddTree(world_, GridPos { 39, 1, 5 });
    AddTree(world_, GridPos { 38, 1, -5 });
    AddTree(world_, GridPos { -5, 1, -39 });
    AddTree(world_, GridPos { 5, 1, -38 });
    AddTree(world_, GridPos { 5, 1, 39 });
    AddTree(world_, GridPos { -5, 1, 38 });
    AddTree(world_, GridPos { -21, 1, -22 });
    AddTree(world_, GridPos { 21, 1, -22 });
    AddTree(world_, GridPos { -21, 1, 22 });
    AddTree(world_, GridPos { 21, 1, 22 });
}

void Game::AddFrozenRingLayout()
{
    AddClassicIsland(world_, Vector3 { -36.0f, 0.0f, 0.0f }, 11, 7);
    AddClassicIsland(world_, Vector3 { 36.0f, 0.0f, 0.0f }, 11, 7);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -36.0f }, 7, 11);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 36.0f }, 7, 11);

    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 0.0f }, 8, 8);
    AddCenterMonument(world_);
    AddSquareRing(world_, GridPos { 0, 0, 0 }, 13, 16, BlockType::IceBlock);

    const GridPos sideShrines[] {
        GridPos { -24, 0, 0 },
        GridPos { 24, 0, 0 },
        GridPos { 0, 0, -24 },
        GridPos { 0, 0, 24 }
    };
    for (const GridPos& shrine : sideShrines)
    {
        AddRectLayer(world_, shrine, 4, 4, BlockType::StoneBlock);
        AddRectLayer(world_, GridPos { shrine.x, shrine.y + 1, shrine.z }, 2, 2, BlockType::IceBlock);
    }

    const GridPos corners[] {
        GridPos { -24, 0, -24 },
        GridPos { 24, 0, -24 },
        GridPos { -24, 0, 24 },
        GridPos { 24, 0, 24 }
    };
    for (const GridPos& corner : corners)
    {
        AddRectLayer(world_, corner, 4, 4, BlockType::StoneBlock);
    }

    AddLineBridge(world_, GridPos { -28, 0, -3 }, GridPos { -24, 0, -24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -28, 0, 3 }, GridPos { -24, 0, 24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 28, 0, -3 }, GridPos { 24, 0, -24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 28, 0, 3 }, GridPos { 24, 0, 24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -3, 0, -28 }, GridPos { -24, 0, -24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 3, 0, -28 }, GridPos { 24, 0, -24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -3, 0, 28 }, GridPos { -24, 0, 24 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 3, 0, 28 }, GridPos { 24, 0, 24 }, 3, BlockType::StoneBlock);

    AddLineBridge(world_, GridPos { -24, 0, -24 }, GridPos { -16, 0, -16 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 24, 0, -24 }, GridPos { 16, 0, -16 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -24, 0, 24 }, GridPos { -16, 0, 16 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 24, 0, 24 }, GridPos { 16, 0, 16 }, 3, BlockType::StoneBlock);

    AddLineBridge(world_, GridPos { -28, 0, 0 }, GridPos { -9, 0, 0 }, 2, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 28, 0, 0 }, GridPos { 9, 0, 0 }, 2, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 0, 0, -28 }, GridPos { 0, 0, -9 }, 2, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 0, 0, 28 }, GridPos { 0, 0, 9 }, 2, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { -16, 0, 0 }, GridPos { -9, 0, 0 }, 3, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 16, 0, 0 }, GridPos { 9, 0, 0 }, 3, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 0, 0, -16 }, GridPos { 0, 0, -9 }, 3, BlockType::IceBlock);
    AddLineBridge(world_, GridPos { 0, 0, 16 }, GridPos { 0, 0, 9 }, 3, BlockType::IceBlock);
}

void Game::AddMoltenLayersLayout()
{
    AddClassicIsland(world_, Vector3 { -36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -36.0f }, 7, 10);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 36.0f }, 7, 10);

    AddRectLayer(world_, GridPos { 0, -1, 0 }, 8, 8, BlockType::StoneBlock);
    AddRectLayer(world_, GridPos { 0, -1, 0 }, 4, 4, BlockType::LavaBlock);
    AddRectLayer(world_, GridPos { 0, 2, 0 }, 7, 7, BlockType::StoneBlock);
    AddRectLayer(world_, GridPos { 0, 3, 0 }, 3, 3, BlockType::EnergyGlassBlock);

    const GridPos highPads[] {
        GridPos { -21, 2, 0 },
        GridPos { 21, 2, 0 },
        GridPos { 0, 2, -21 },
        GridPos { 0, 2, 21 }
    };
    for (const GridPos& pad : highPads)
    {
        AddRectLayer(world_, pad, 5, 4, BlockType::StoneBlock);
    }

    const GridPos lowForges[] {
        GridPos { -18, -1, -18 },
        GridPos { 18, -1, -18 },
        GridPos { -18, -1, 18 },
        GridPos { 18, -1, 18 }
    };
    for (const GridPos& forge : lowForges)
    {
        AddRectLayer(world_, forge, 4, 4, BlockType::StoneBlock);
        AddRectLayer(world_, GridPos { forge.x, forge.y, forge.z }, 2, 2, BlockType::LavaBlock);
    }

    AddSteppedBridge(world_, GridPos { -28, 0, -3 }, GridPos { -21, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { -28, 0, 3 }, GridPos { -21, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 28, 0, 3 }, GridPos { 21, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 28, 0, -3 }, GridPos { 21, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 3, 0, -28 }, GridPos { 0, 2, -21 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { -3, 0, -28 }, GridPos { 0, 2, -21 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { -3, 0, 28 }, GridPos { 0, 2, 21 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 3, 0, 28 }, GridPos { 0, 2, 21 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -21, 2, 0 }, GridPos { -7, 2, 0 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 21, 2, 0 }, GridPos { 7, 2, 0 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 0, 2, -21 }, GridPos { 0, 2, -7 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 0, 2, 21 }, GridPos { 0, 2, 7 }, 3, BlockType::StoneBlock);

    AddLineBridge(world_, GridPos { -28, -1, 0 }, GridPos { -8, -1, 0 }, 2, BlockType::LavaBlock);
    AddLineBridge(world_, GridPos { 28, -1, 0 }, GridPos { 8, -1, 0 }, 2, BlockType::LavaBlock);
    AddLineBridge(world_, GridPos { 0, -1, -28 }, GridPos { 0, -1, -8 }, 2, BlockType::LavaBlock);
    AddLineBridge(world_, GridPos { 0, -1, 28 }, GridPos { 0, -1, 8 }, 2, BlockType::LavaBlock);
    AddSteppedBridge(world_, GridPos { -8, -1, 0 }, GridPos { -3, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 8, -1, 0 }, GridPos { 3, 2, 0 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 0, -1, -8 }, GridPos { 0, 2, -3 }, 3, BlockType::StoneBlock);
    AddSteppedBridge(world_, GridPos { 0, -1, 8 }, GridPos { 0, 2, 3 }, 3, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -18, -1, -18 }, GridPos { -8, -1, -8 }, 2, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 18, -1, -18 }, GridPos { 8, -1, -8 }, 2, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { -18, -1, 18 }, GridPos { -8, -1, 8 }, 2, BlockType::StoneBlock);
    AddLineBridge(world_, GridPos { 18, -1, 18 }, GridPos { 8, -1, 8 }, 2, BlockType::StoneBlock);
}

void Game::AddOrbitalShardsLayout()
{
    AddClassicIsland(world_, Vector3 { -36.0f, 0.0f, 0.0f }, 9, 7);
    AddClassicIsland(world_, Vector3 { 36.0f, 0.0f, 0.0f }, 9, 7);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -36.0f }, 7, 9);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 36.0f }, 7, 9);

    AddRectLayer(world_, GridPos { 0, 3, 0 }, 7, 7, BlockType::EnergyGlassBlock);
    AddRectLayer(world_, GridPos { 0, 4, 0 }, 3, 3, BlockType::StoneBlock);

    const GridPos launchPads[] {
        GridPos { -24, 1, 0 },
        GridPos { 24, 1, 0 },
        GridPos { 0, 1, -24 },
        GridPos { 0, 1, 24 }
    };
    for (const GridPos& pad : launchPads)
    {
        AddRectLayer(world_, pad, 4, 3, BlockType::StoneBlock);
        PlaceMapBlock(world_, GridPos { pad.x, pad.y + 1, pad.z }, BlockType::SpringBlock, -1, true);
    }

    const GridPos shards[] {
        GridPos { -16, 2, -16 },
        GridPos { 16, 2, -16 },
        GridPos { -16, 2, 16 },
        GridPos { 16, 2, 16 },
        GridPos { -10, 3, 0 },
        GridPos { 10, 3, 0 },
        GridPos { 0, 3, -10 },
        GridPos { 0, 3, 10 }
    };
    for (const GridPos& shard : shards)
    {
        AddRectLayer(world_, shard, 3, 3, BlockType::StoneBlock);
    }

    const GridPos anchors[] {
        GridPos { -29, 0, 0 },
        GridPos { -27, 1, 0 },
        GridPos { -21, 1, 0 },
        GridPos { 29, 0, 0 },
        GridPos { 27, 1, 0 },
        GridPos { 21, 1, 0 },
        GridPos { 0, 0, -29 },
        GridPos { 0, 1, -27 },
        GridPos { 0, 1, -21 },
        GridPos { 0, 0, 29 },
        GridPos { 0, 1, 27 },
        GridPos { 0, 1, 21 }
    };
    for (const GridPos& anchor : anchors)
    {
        AddRectLayer(world_, anchor, 1, 1, BlockType::StoneBlock);
    }

    AddLineBridge(world_, GridPos { -24, 1, 0 }, GridPos { -16, 2, -16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { -24, 1, 0 }, GridPos { -16, 2, 16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 24, 1, 0 }, GridPos { 16, 2, -16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 24, 1, 0 }, GridPos { 16, 2, 16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 1, -24 }, GridPos { -16, 2, -16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 1, -24 }, GridPos { 16, 2, -16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 1, 24 }, GridPos { -16, 2, 16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 1, 24 }, GridPos { 16, 2, 16 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { -10, 3, 0 }, GridPos { -7, 3, 0 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 10, 3, 0 }, GridPos { 7, 3, 0 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 3, -10 }, GridPos { 0, 3, -7 }, 2, BlockType::EnergyGlassBlock);
    AddLineBridge(world_, GridPos { 0, 3, 10 }, GridPos { 0, 3, 7 }, 2, BlockType::EnergyGlassBlock);
}

void Game::AddBrokenCitadelLayout()
{
    AddClassicIsland(world_, Vector3 { -36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 36.0f, 0.0f, 0.0f }, 10, 7);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, -36.0f }, 7, 10);
    AddClassicIsland(world_, Vector3 { 0.0f, 0.0f, 36.0f }, 7, 10);

    AddRectLayer(world_, GridPos { 0, 0, 0 }, 9, 9, BlockType::StoneBlock, true);
    AddRectLayer(world_, GridPos { 0, 1, 0 }, 5, 5, BlockType::StoneBlock, true);
    AddRectLayer(world_, GridPos { 0, 2, 0 }, 2, 2, BlockType::EnergyGlassBlock, true);
    const GridPos missingCitadel[] {
        GridPos { -7, 0, -7 },
        GridPos { 7, 0, -7 },
        GridPos { -7, 0, 7 },
        GridPos { 7, 0, 7 },
        GridPos { -3, 1, 4 },
        GridPos { 4, 1, -3 }
    };
    for (const GridPos& pos : missingCitadel)
    {
        world_.RemoveBlock(pos);
    }

    const GridPos rooms[] {
        GridPos { -21, 1, -21 },
        GridPos { 21, 1, -21 },
        GridPos { -21, 1, 21 },
        GridPos { 21, 1, 21 }
    };
    for (const GridPos& room : rooms)
    {
        AddRectLayer(world_, room, 5, 5, BlockType::StoneBlock, true);
        AddRectLayer(world_, GridPos { room.x, room.y + 1, room.z }, 3, 3, BlockType::EnergyGlassBlock, true);
        PlaceMapBlock(world_, GridPos { room.x, room.y + 2, room.z }, BlockType::EnergyGlassBlock, -1, true);
        world_.RemoveBlock(GridPos { room.x + 5, room.y + 1, room.z });
        world_.RemoveBlock(GridPos { room.x - 5, room.y + 1, room.z });
        world_.RemoveBlock(GridPos { room.x, room.y + 1, room.z + 5 });
        world_.RemoveBlock(GridPos { room.x, room.y + 1, room.z - 5 });
    }

    AddLineBridge(world_, GridPos { -28, 0, 0 }, GridPos { -9, 0, 0 }, 3, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 28, 0, 0 }, GridPos { 9, 0, 0 }, 3, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 0, 0, -28 }, GridPos { 0, 0, -9 }, 3, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 0, 0, 28 }, GridPos { 0, 0, 9 }, 3, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { -28, 0, -3 }, GridPos { -21, 1, -21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { -28, 0, 3 }, GridPos { -21, 1, 21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { 28, 0, -3 }, GridPos { 21, 1, -21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { 28, 0, 3 }, GridPos { 21, 1, 21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { -3, 0, -28 }, GridPos { -21, 1, -21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { 3, 0, -28 }, GridPos { 21, 1, -21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { -3, 0, 28 }, GridPos { -21, 1, 21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { 3, 0, 28 }, GridPos { 21, 1, 21 }, 3, BlockType::StoneBlock, true);
    AddLineBridge(world_, GridPos { -21, 1, -21 }, GridPos { -8, 1, -8 }, 2, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 21, 1, -21 }, GridPos { 8, 1, -8 }, 2, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { -21, 1, 21 }, GridPos { -8, 1, 8 }, 2, BlockType::WoodBlock, true);
    AddLineBridge(world_, GridPos { 21, 1, 21 }, GridPos { 8, 1, 8 }, 2, BlockType::WoodBlock, true);

    const GridPos crackedEdges[] {
        GridPos { -18, 1, -18 },
        GridPos { 18, 1, -18 },
        GridPos { -18, 1, 18 },
        GridPos { 18, 1, 18 },
        GridPos { -13, 0, 1 },
        GridPos { 13, 0, -1 },
        GridPos { 1, 0, -13 },
        GridPos { -1, 0, 13 }
    };
    for (const GridPos& pos : crackedEdges)
    {
        world_.RemoveBlock(pos);
    }
}

void Game::SetupGenerators()
{
    const auto addGenerator = [this](ResourceType type, Vector3 position, float interval, int amount, int teamId)
    {
        world_.PlaceBlock(world_.WorldToGrid(position), Block { BlockType::ResourceGenerator, teamId, false }, true);
        generators_.emplace_back(type, position, interval, amount, teamId);
    };

    if (IsTeamActiveForMode(0))
    {
        addGenerator(ResourceType::Iron, Vector3 { -39.0f, 1.0f, -1.0f }, 0.78f, 1, 0);
        addGenerator(ResourceType::Gold, Vector3 { -41.0f, 1.0f, 1.0f }, 3.4f, 1, 0);
    }
    if (IsTeamActiveForMode(1))
    {
        addGenerator(ResourceType::Iron, Vector3 { 39.0f, 1.0f, 1.0f }, 0.78f, 1, 1);
        addGenerator(ResourceType::Gold, Vector3 { 41.0f, 1.0f, -1.0f }, 3.4f, 1, 1);
    }
    if (IsTeamActiveForMode(2))
    {
        addGenerator(ResourceType::Iron, Vector3 { 1.0f, 1.0f, -39.0f }, 0.78f, 1, 2);
        addGenerator(ResourceType::Gold, Vector3 { -1.0f, 1.0f, -41.0f }, 3.4f, 1, 2);
    }
    if (IsTeamActiveForMode(3))
    {
        addGenerator(ResourceType::Iron, Vector3 { -1.0f, 1.0f, 39.0f }, 0.78f, 1, 3);
        addGenerator(ResourceType::Gold, Vector3 { 1.0f, 1.0f, 41.0f }, 3.4f, 1, 3);
    }

    if (arenaBiome_ == ArenaBiome::Ice)
    {
        addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 2.0f, 0.0f }, 4.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -24.0f, 2.0f, 0.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 24.0f, 2.0f, 0.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 2.0f, -24.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 2.0f, 24.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { -24.0f, 1.0f, -24.0f }, 6.0f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { 24.0f, 1.0f, 24.0f }, 6.0f, 1, -1);
        return;
    }
    if (arenaBiome_ == ArenaBiome::Lava)
    {
        addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 4.0f, 0.0f }, 5.4f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -21.0f, 3.0f, 0.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 21.0f, 3.0f, 0.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 3.0f, -21.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 3.0f, 21.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { -18.0f, 0.0f, -18.0f }, 4.8f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { 18.0f, 0.0f, 18.0f }, 4.8f, 1, -1);
        return;
    }
    if (arenaBiome_ == ArenaBiome::Space)
    {
        addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 5.0f, 0.0f }, 4.4f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -10.0f, 4.0f, 0.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 10.0f, 4.0f, 0.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 4.0f, -10.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 0.0f, 4.0f, 10.0f }, 3.2f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { -16.0f, 3.0f, -16.0f }, 6.0f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { 16.0f, 3.0f, 16.0f }, 6.0f, 1, -1);
        return;
    }
    if (arenaBiome_ == ArenaBiome::Ruins)
    {
        addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 3.0f, 0.0f }, 5.2f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { -21.0f, 3.0f, -21.0f }, 4.8f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 21.0f, 3.0f, -21.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -21.0f, 3.0f, 21.0f }, 3.6f, 1, -1);
        addGenerator(ResourceType::Crystal, Vector3 { 21.0f, 3.0f, 21.0f }, 4.8f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { -6.0f, 1.0f, 0.0f }, 3.4f, 1, -1);
        addGenerator(ResourceType::Gold, Vector3 { 6.0f, 1.0f, 0.0f }, 3.4f, 1, -1);
        return;
    }

    addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 2.0f, 0.0f }, 4.8f, 1, -1);
    addGenerator(ResourceType::Gold, Vector3 { -4.0f, 2.0f, 0.0f }, 3.0f, 1, -1);
    addGenerator(ResourceType::Gold, Vector3 { 4.0f, 2.0f, 0.0f }, 3.0f, 1, -1);
    addGenerator(ResourceType::Crystal, Vector3 { -18.0f, 1.0f, 0.0f }, 6.2f, 1, -1);
    addGenerator(ResourceType::Crystal, Vector3 { 18.0f, 1.0f, 0.0f }, 6.2f, 1, -1);
    addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 1.0f, -18.0f }, 6.2f, 1, -1);
    addGenerator(ResourceType::Crystal, Vector3 { 0.0f, 1.0f, 18.0f }, 6.2f, 1, -1);
}

void Game::AddVerticalArenaFeatures()
{
    AddOvalLayer(world_, Vector3 { 0.0f, 0.0f, 0.0f }, 7, 7, 2, BlockType::GrassBlock, false);
    AddOvalLayer(world_, Vector3 { 0.0f, 0.0f, 0.0f }, 9, 9, -1, BlockType::DirtBlock, false);
    AddWoodDock(world_, GridPos { -12, -1, 0 }, 6, 1, 0);
    AddWoodDock(world_, GridPos { 12, -1, 0 }, 6, -1, 0);
    AddWoodDock(world_, GridPos { 0, -1, -12 }, 6, 0, 1);
    AddWoodDock(world_, GridPos { 0, -1, 12 }, 6, 0, -1);

    const GridPos towerBases[] {
        GridPos { -33, 1, 5 },
        GridPos { 33, 1, -5 },
        GridPos { 5, 1, -33 },
        GridPos { -5, 1, 33 }
    };
    for (const GridPos& base : towerBases)
    {
        for (int y = 1; y <= 4; ++y)
        {
            PlaceMapBlock(world_, GridPos { base.x, y, base.z }, BlockType::WoodBlock);
        }
        PlaceMapBlock(world_, GridPos { base.x + 1, 4, base.z }, BlockType::LeafBlock);
        PlaceMapBlock(world_, GridPos { base.x - 1, 4, base.z }, BlockType::LeafBlock);
        PlaceMapBlock(world_, GridPos { base.x, 4, base.z + 1 }, BlockType::LeafBlock);
        PlaceMapBlock(world_, GridPos { base.x, 4, base.z - 1 }, BlockType::LeafBlock);
        PlaceMapBlock(world_, GridPos { base.x, 5, base.z }, BlockType::LeafBlock);
    }

    const BlockType hazard = arenaBiome_ == ArenaBiome::Ice
        ? BlockType::IceBlock
        : (arenaBiome_ == ArenaBiome::Lava ? BlockType::LavaBlock : BlockType::SpikeBlock);
    const GridPos hazardPads[] {
        GridPos { -4, 3, 0 },
        GridPos { 4, 3, 0 },
        GridPos { 0, 3, -4 },
        GridPos { 0, 3, 4 }
    };
    for (const GridPos& pad : hazardPads)
    {
        PlaceMapBlock(world_, pad, hazard, -1, true);
    }
    PlaceMapBlock(world_, GridPos { 0, 3, 0 }, BlockType::SpringBlock, -1, true);
}

void Game::AddRuinsBiomeFeatures()
{
    const GridPos relicPlatforms[] {
        GridPos { -20, 1, -20 },
        GridPos { 20, 1, -20 },
        GridPos { -20, 1, 20 },
        GridPos { 20, 1, 20 }
    };
    for (const GridPos& center : relicPlatforms)
    {
        for (int x = -2; x <= 2; ++x)
        {
            for (int z = -2; z <= 2; ++z)
            {
                if (std::abs(x) + std::abs(z) <= 3)
                {
                    PlaceMapBlock(world_, GridPos { center.x + x, center.y, center.z + z }, BlockType::StoneBlock, -1, true);
                }
            }
        }
        PlaceMapBlock(world_, GridPos { center.x, center.y + 1, center.z }, BlockType::EnergyGlassBlock, -1, true);
    }

    const GridPos crackedBridges[] {
        GridPos { -13, 0, 0 },
        GridPos { -11, 0, 0 },
        GridPos { 11, 0, 0 },
        GridPos { 13, 0, 0 },
        GridPos { 0, 0, -13 },
        GridPos { 0, 0, -11 },
        GridPos { 0, 0, 11 },
        GridPos { 0, 0, 13 },
        GridPos { -5, 2, 0 },
        GridPos { 5, 2, 0 },
        GridPos { 0, 2, -5 },
        GridPos { 0, 2, 5 }
    };
    for (const GridPos& pos : crackedBridges)
    {
        if (!world_.IsAir(pos))
        {
            PlaceMapBlock(world_, pos, BlockType::WoodBlock, -1, true);
        }
    }
}

void Game::StartSelectedMatch()
{
    automatch_.active = false;
    selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
    selectedTeamSize_ = std::clamp(selectedTeamSize_, 1, 4);
    selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
    SaveSettings();
    SetupMatch();
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);
    UpdateCamera(0.016f);
    screen_ = GameScreen::Playing;
    DisableCursor();
    SetMessage(std::string("Mode: ") + MatchModeName() + ". Protect your EnergyCore.", 4.0f);
}

void Game::StartAutomatch()
{
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    selectedTeamSize_ = 4;
    selectedBotCount_ = MaxBotCountForSelection();
    automatch_ = AutomatchState {};
    automatch_.active = true;
    automatch_.targetRuns = std::clamp(automatchRunTarget_, 1, 50);
    automatch_.maxMatchSeconds = static_cast<float>(std::clamp(automatchMaxMinutes_, 3, 30) * 60);
    SaveSettings();
    SetupMatch();
    ConfigureAutomatchMatch();
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);
    UpdateCamera(0.016f);
    screen_ = GameScreen::Playing;
    DisableCursor();
    showBotDebug_ = true;
    SetMessage("Automatch running: bot-only simulation.", 4.0f);
}

bool Game::RunAutomatchBatch(int runs, int ticksPerFrame, int maxMinutes)
{
    automatchRunTarget_ = std::clamp(runs, 1, 50);
    automatchTicksPerFrame_ = std::clamp(ticksPerFrame, 1, 32);
    automatchMaxMinutes_ = std::clamp(maxMinutes, 3, 30);
    StartAutomatch();

    const int guardFrames = automatchRunTarget_ * automatchMaxMinutes_ * 60 * 90;
    int frames = 0;
    while (automatch_.active && frames++ < guardFrames && !ShouldClose())
    {
        CrashLogger::Heartbeat("automatch-update");
        Update(1.0f / 60.0f);
    }
    if (automatch_.active)
    {
        automatch_.active = false;
        WriteAutomatchStatsJson();
        return false;
    }
    return automatch_.completedRuns >= automatch_.targetRuns;
}

void Game::ConfigureAutomatchMatch()
{
    automatch_.currentFirstCoreDamageTime = -1.0f;
    automatch_.currentTeamStats[0] = AutomatchTeamStats {};
    automatch_.currentTeamStats[1] = AutomatchTeamStats {};
    automatch_.currentTeamStats[2] = AutomatchTeamStats {};
    automatch_.currentTeamStats[3] = AutomatchTeamStats {};
    automatch_.currentTimeline.clear();

    std::array<int, 4> activeBotsByTeam {};
    int nextId = localPlayerId_ + 1;
    for (const Player& player : players_)
    {
        nextId = std::max(nextId, player.GetId() + 1);
        if (!player.IsLocal() && IsTeamActiveForMode(player.GetTeamId()))
        {
            ++activeBotsByTeam[player.GetTeamId()];
        }
    }

    for (Team& team : teams_)
    {
        if (!IsTeamActiveForMode(team.id))
        {
            continue;
        }
        while (activeBotsByTeam[team.id] < selectedTeamSize_)
        {
            Player bot(nextId++, std::string(TeamName(team.id)) + " Auto Bot " + std::to_string(activeBotsByTeam[team.id] + 1), team.id, team.spawnPoint, false);
            bot.SetYaw(YawForTeam(team.id));
            ApplyBotLoadout(bot);
            players_.push_back(bot);
            ++activeBotsByTeam[team.id];
        }
    }

    if (Player* localPlayer = GetLocalPlayer())
    {
        localPlayer->Kill(true);
    }
    for (Player& player : players_)
    {
        GetPlayerScore(player.GetId());
    }
    spectatorMode_ = true;
    spectatorFreeCamera_ = false;
    spectatorTargetIndex_ = 0;
    for (int i = 0; i < static_cast<int>(players_.size()); ++i)
    {
        if (!players_[i].IsLocal() && players_[i].IsAlive() && !players_[i].IsEliminated())
        {
            spectatorTargetIndex_ = i;
            spectatorPosition_ = players_[i].GetPosition();
            break;
        }
    }
    cameraController_.SetMode(ViewMode::ThirdPerson);
    automatch_.sampleTimer = 0.0f;
}

void Game::UpdateAutomatch(float dt)
{
    if (!automatch_.active)
    {
        return;
    }

    automatch_.sampleTimer += dt;
    if (automatch_.sampleTimer >= 1.0f)
    {
        automatch_.sampleTimer = 0.0f;
        SampleAutomatchBots();
    }

    const bool timeout = !winnerTeamId_.has_value() && matchTime_ >= automatch_.maxMatchSeconds;
    if (!winnerTeamId_.has_value() && !timeout)
    {
        return;
    }

    FinishAutomatchRun(timeout);
    if (automatch_.completedRuns >= automatch_.targetRuns)
    {
        automatch_.active = false;
        WriteAutomatchStatsJson();
        SetMessage("Automatch complete. Review stats overlay.", 8.0f);
        return;
    }

    SetupMatch();
    ConfigureAutomatchMatch();
}

void Game::SampleAutomatchBots()
{
    const auto findBotStats = [this](const Player& player) -> AutomatchBotStats&
    {
        for (AutomatchBotStats& stats : automatch_.botStats)
        {
            if (stats.teamId == player.GetTeamId() && stats.name == player.GetName())
            {
                return stats;
            }
        }
        automatch_.botStats.push_back(AutomatchBotStats { player.GetName(), player.GetTeamId() });
        return automatch_.botStats.back();
    };

    for (const Player& player : players_)
    {
        if (player.IsLocal())
        {
            continue;
        }
        const BotMemory* memory = nullptr;
        for (const BotMemory& candidate : botMemories_)
        {
            if (candidate.playerId == player.GetId())
            {
                memory = &candidate;
                break;
            }
        }
        if (memory == nullptr)
        {
            continue;
        }

        AutomatchBotStats& stats = findBotStats(player);
        ++stats.samples;
        const int roleIndex = std::clamp(static_cast<int>(memory->role), 0, 3);
        const int intentIndex = std::clamp(static_cast<int>(memory->intent), 0, 9);
        ++stats.roleSamples[roleIndex];
        ++stats.intentSamples[intentIndex];
        if (stats.lastRole >= 0 && stats.lastRole != roleIndex)
        {
            ++stats.roleChanges;
        }
        if (stats.lastIntent >= 0 && stats.lastIntent != intentIndex)
        {
            ++stats.intentChanges;
        }
        stats.lastRole = roleIndex;
        stats.lastIntent = intentIndex;
        if (memory->stuckTimer > 1.0f)
        {
            ++stats.stuckSamples;
        }

        if (player.GetTeamId() >= 0 && player.GetTeamId() < 4)
        {
            AutomatchTeamStats& teamStats = automatch_.currentTeamStats[player.GetTeamId()];
            ++teamStats.samples;
            ++teamStats.roleSamples[roleIndex];
            ++teamStats.intentSamples[intentIndex];
            teamStats.resourcesHeld[0] += player.GetInventory().GetResource(ResourceType::Iron);
            teamStats.resourcesHeld[1] += player.GetInventory().GetResource(ResourceType::Gold);
            teamStats.resourcesHeld[2] += player.GetInventory().GetResource(ResourceType::Crystal);
        }

        const Vector3 position = player.GetPosition();
        const Team* team = FindTeam(player.GetTeamId());
        const Vector3 base = team != nullptr ? team->spawnPoint : Vector3 {};
        const float fromBase = Distance3D(position, base);
        const float fromCenter = Distance3D(position, Vector3 {});
        if (!stats.hasMovementSample)
        {
            stats.hasMovementSample = true;
            stats.lastPosition = position;
            stats.minPosition = position;
            stats.maxPosition = position;
        }
        else
        {
            stats.totalDistance += Distance3D(stats.lastPosition, position);
            stats.lastPosition = position;
            stats.minPosition = Vector3 {
                std::min(stats.minPosition.x, position.x),
                std::min(stats.minPosition.y, position.y),
                std::min(stats.minPosition.z, position.z)
            };
            stats.maxPosition = Vector3 {
                std::max(stats.maxPosition.x, position.x),
                std::max(stats.maxPosition.y, position.y),
                std::max(stats.maxPosition.z, position.z)
            };
        }
        stats.maxDistanceFromBase = std::max(stats.maxDistanceFromBase, fromBase);
        stats.maxDistanceFromCenter = std::max(stats.maxDistanceFromCenter, fromCenter);
        const float samples = static_cast<float>(std::max(1, stats.samples));
        stats.averageDistanceFromBase += (fromBase - stats.averageDistanceFromBase) / samples;
        stats.averageDistanceFromCenter += (fromCenter - stats.averageDistanceFromCenter) / samples;
    }
}

void Game::FinishAutomatchRun(bool timeout)
{
    SampleAutomatchBots();
    AutomatchRunStats run {};
    run.winnerTeamId = winnerTeamId_.value_or(-1);
    run.duration = matchTime_;
    run.timeout = timeout;
    run.firstCoreDamageTime = automatch_.currentFirstCoreDamageTime;
    run.timeline = automatch_.currentTimeline;
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        run.teamStats[teamId] = automatch_.currentTeamStats[teamId];
    }

    const auto findBotStats = [this](const Player& player) -> AutomatchBotStats&
    {
        for (AutomatchBotStats& stats : automatch_.botStats)
        {
            if (stats.teamId == player.GetTeamId() && stats.name == player.GetName())
            {
                return stats;
            }
        }
        automatch_.botStats.push_back(AutomatchBotStats { player.GetName(), player.GetTeamId() });
        return automatch_.botStats.back();
    };

    for (const Player& player : players_)
    {
        if (player.IsLocal())
        {
            continue;
        }
        const PlayerMatchScore* score = FindPlayerScore(player.GetId());
        if (score == nullptr)
        {
            continue;
        }

        AutomatchBotStats& stats = findBotStats(player);
        stats.kills += score->kills;
        stats.deaths += score->deaths;
        stats.finalDeaths += score->finalDeaths;
        stats.coreDamage += score->coreDamage;
        run.kills += score->kills;
        run.coreDamage += score->coreDamage;
        if (player.GetTeamId() >= 0 && player.GetTeamId() < 4)
        {
            AutomatchTeamStats& teamStats = run.teamStats[player.GetTeamId()];
            teamStats.kills += score->kills;
            teamStats.deaths += score->deaths;
            teamStats.finalDeaths += score->finalDeaths;
            teamStats.coreDamage += score->coreDamage;
        }
    }

    for (const Player& player : players_)
    {
        if (player.IsLocal() || player.GetTeamId() < 0 || player.GetTeamId() >= 4)
        {
            continue;
        }

        AutomatchTeamStats& teamStats = run.teamStats[player.GetTeamId()];
        if (player.IsEliminated())
        {
            ++teamStats.eliminatedPlayers;
        }
        else
        {
            ++teamStats.alivePlayers;
        }
    }

    for (const EnergyCore& core : cores_)
    {
        const int teamId = core.GetTeamId();
        if (teamId < 0 || teamId >= 4)
        {
            continue;
        }
        AutomatchTeamStats& teamStats = run.teamStats[teamId];
        teamStats.coreAlive = core.IsAlive();
        teamStats.coreHealth = core.GetHealth();
        teamStats.coreMaxHealth = core.GetMaxHealth();
        if (!core.IsAlive())
        {
            ++run.coreDestroyedCount;
        }
    }
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        run.finalDeathCount += run.teamStats[teamId].finalDeaths;
    }
    if (timeout)
    {
        int aliveCores = 0;
        int teamsWithLives = 0;
        for (int teamId = 0; teamId < 4; ++teamId)
        {
            if (run.teamStats[teamId].coreAlive)
            {
                ++aliveCores;
            }
            if (run.teamStats[teamId].alivePlayers > 0)
            {
                ++teamsWithLives;
            }
        }
        run.finishReason = "timeout: " + std::to_string(aliveCores) + " cores alive, "
            + std::to_string(teamsWithLives) + " teams with lives";
    }
    else
    {
        run.finishReason = run.winnerTeamId >= 0
            ? std::string(TeamName(run.winnerTeamId)) + " was last team standing"
            : "match ended without winner";
    }

    ++automatch_.completedRuns;
    automatch_.totalDuration += run.duration;
    automatch_.totalKills += run.kills;
    automatch_.totalCoreDamage += run.coreDamage;
    automatch_.totalFinalDeaths += run.finalDeathCount;
    automatch_.totalCoreDestroyed += run.coreDestroyedCount;
    if (timeout)
    {
        ++automatch_.timeouts;
    }
    else if (run.winnerTeamId >= 0 && run.winnerTeamId < 4)
    {
        ++automatch_.teamWins[run.winnerTeamId];
    }
    automatch_.runs.push_back(run);
}

void Game::WriteAutomatchStatsJson() const
{
    std::ofstream file("automatch_stats.json", std::ios::trunc);
    if (!file)
    {
        return;
    }

    const float avgDuration = automatch_.completedRuns > 0
        ? automatch_.totalDuration / static_cast<float>(automatch_.completedRuns)
        : 0.0f;
    file << "{\n";
    file << "  \"summary\": {\n";
    file << "    \"completedRuns\": " << automatch_.completedRuns << ",\n";
    file << "    \"targetRuns\": " << automatch_.targetRuns << ",\n";
    file << "    \"biome\": \"" << JsonEscape(ArenaBiomeName()) << "\",\n";
    file << "    \"timeouts\": " << automatch_.timeouts << ",\n";
    file << "    \"averageDurationSeconds\": " << avgDuration << ",\n";
    file << "    \"totalKills\": " << automatch_.totalKills << ",\n";
    file << "    \"totalCoreDamage\": " << automatch_.totalCoreDamage << ",\n";
    file << "    \"totalFinalDeaths\": " << automatch_.totalFinalDeaths << ",\n";
    file << "    \"totalCoreDestroyed\": " << automatch_.totalCoreDestroyed << ",\n";
    file << "    \"winsByTeam\": {\n";
    file << "      \"Red\": " << automatch_.teamWins[0] << ",\n";
    file << "      \"Blue\": " << automatch_.teamWins[1] << ",\n";
    file << "      \"Green\": " << automatch_.teamWins[2] << ",\n";
    file << "      \"Yellow\": " << automatch_.teamWins[3] << "\n";
    file << "    }\n";
    file << "  },\n";

    file << "  \"runs\": [\n";
    for (std::size_t i = 0; i < automatch_.runs.size(); ++i)
    {
        const AutomatchRunStats& run = automatch_.runs[i];
        file << "    {\n";
        file << "      \"index\": " << (i + 1) << ",\n";
        file << "      \"winnerTeamId\": " << run.winnerTeamId << ",\n";
        file << "      \"winnerTeam\": \"" << JsonEscape(run.winnerTeamId >= 0 ? TeamName(run.winnerTeamId) : "None") << "\",\n";
        file << "      \"durationSeconds\": " << run.duration << ",\n";
        file << "      \"timeout\": " << (run.timeout ? "true" : "false") << ",\n";
        file << "      \"finishReason\": \"" << JsonEscape(run.finishReason) << "\",\n";
        file << "      \"firstCoreDamageTime\": " << run.firstCoreDamageTime << ",\n";
        file << "      \"kills\": " << run.kills << ",\n";
        file << "      \"finalDeaths\": " << run.finalDeathCount << ",\n";
        file << "      \"coreDamage\": " << run.coreDamage << ",\n";
        file << "      \"coresDestroyed\": " << run.coreDestroyedCount << ",\n";
        file << "      \"teams\": [\n";
        for (int teamId = 0; teamId < 4; ++teamId)
        {
            const AutomatchTeamStats& teamStats = run.teamStats[teamId];
            const float resourceSamples = static_cast<float>(std::max(1, teamStats.samples));
            file << "        {\n";
            file << "          \"teamId\": " << teamId << ",\n";
            file << "          \"team\": \"" << JsonEscape(TeamName(teamId)) << "\",\n";
            file << "          \"kills\": " << teamStats.kills << ",\n";
            file << "          \"deaths\": " << teamStats.deaths << ",\n";
            file << "          \"finalDeaths\": " << teamStats.finalDeaths << ",\n";
            file << "          \"coreDamage\": " << teamStats.coreDamage << ",\n";
            file << "          \"alivePlayers\": " << teamStats.alivePlayers << ",\n";
            file << "          \"eliminatedPlayers\": " << teamStats.eliminatedPlayers << ",\n";
            file << "          \"coreAlive\": " << (teamStats.coreAlive ? "true" : "false") << ",\n";
            file << "          \"coreHealth\": " << teamStats.coreHealth << ",\n";
            file << "          \"coreMaxHealth\": " << teamStats.coreMaxHealth << ",\n";
            file << "          \"averageResourcesHeld\": {";
            file << "\"Iron\": " << static_cast<float>(teamStats.resourcesHeld[0]) / resourceSamples << ", ";
            file << "\"Gold\": " << static_cast<float>(teamStats.resourcesHeld[1]) / resourceSamples << ", ";
            file << "\"Crystal\": " << static_cast<float>(teamStats.resourcesHeld[2]) / resourceSamples << "},\n";
            file << "          \"roleSamples\": {";
            file << "\"Defender\": " << teamStats.roleSamples[0] << ", ";
            file << "\"Rusher\": " << teamStats.roleSamples[1] << ", ";
            file << "\"Collector\": " << teamStats.roleSamples[2] << ", ";
            file << "\"Fighter\": " << teamStats.roleSamples[3] << "},\n";
            file << "          \"intentSamples\": {\n";
            for (int intent = 0; intent < 10; ++intent)
            {
                file << "            \"" << ToString(static_cast<BotIntent>(intent)) << "\": " << teamStats.intentSamples[intent]
                    << (intent < 9 ? "," : "") << "\n";
            }
            file << "          }\n";
            file << "        }" << (teamId < 3 ? "," : "") << "\n";
        }
        file << "      ],\n";
        file << "      \"timeline\": [\n";
        for (std::size_t eventIndex = 0; eventIndex < run.timeline.size(); ++eventIndex)
        {
            const AutomatchTimelineEvent& event = run.timeline[eventIndex];
            file << "        {";
            file << "\"time\": " << event.time << ", ";
            file << "\"type\": \"" << JsonEscape(event.type) << "\", ";
            file << "\"teamId\": " << event.teamId << ", ";
            file << "\"actorId\": " << event.actorId << ", ";
            file << "\"targetId\": " << event.targetId << ", ";
            file << "\"value\": " << event.value << ", ";
            file << "\"text\": \"" << JsonEscape(event.text) << "\"";
            file << "}" << (eventIndex + 1 < run.timeline.size() ? "," : "") << "\n";
        }
        file << "      ]\n";
        file << "    }" << (i + 1 < automatch_.runs.size() ? "," : "") << "\n";
    }
    file << "  ],\n";

    file << "  \"bots\": [\n";
    for (std::size_t i = 0; i < automatch_.botStats.size(); ++i)
    {
        const AutomatchBotStats& stats = automatch_.botStats[i];
        file << "    {\n";
        file << "      \"name\": \"" << JsonEscape(stats.name) << "\",\n";
        file << "      \"teamId\": " << stats.teamId << ",\n";
        file << "      \"team\": \"" << JsonEscape(stats.teamId >= 0 ? TeamName(stats.teamId) : "Unknown") << "\",\n";
        file << "      \"samples\": " << stats.samples << ",\n";
        file << "      \"roleChanges\": " << stats.roleChanges << ",\n";
        file << "      \"intentChanges\": " << stats.intentChanges << ",\n";
        file << "      \"stuckSamples\": " << stats.stuckSamples << ",\n";
        file << "      \"voidFalls\": " << stats.voidFalls << ",\n";
        file << "      \"kills\": " << stats.kills << ",\n";
        file << "      \"deaths\": " << stats.deaths << ",\n";
        file << "      \"finalDeaths\": " << stats.finalDeaths << ",\n";
        file << "      \"coreDamage\": " << stats.coreDamage << ",\n";
        file << "      \"roleSamples\": {\n";
        file << "        \"Defender\": " << stats.roleSamples[0] << ",\n";
        file << "        \"Rusher\": " << stats.roleSamples[1] << ",\n";
        file << "        \"Collector\": " << stats.roleSamples[2] << ",\n";
        file << "        \"Fighter\": " << stats.roleSamples[3] << "\n";
        file << "      },\n";
        file << "      \"intentSamples\": {\n";
        for (int intent = 0; intent < 10; ++intent)
        {
            file << "        \"" << ToString(static_cast<BotIntent>(intent)) << "\": " << stats.intentSamples[intent]
                << (intent < 9 ? "," : "") << "\n";
        }
        file << "      },\n";
        file << "      \"movement\": {\n";
        file << "        \"totalDistance\": " << stats.totalDistance << ",\n";
        file << "        \"maxDistanceFromBase\": " << stats.maxDistanceFromBase << ",\n";
        file << "        \"maxDistanceFromCenter\": " << stats.maxDistanceFromCenter << ",\n";
        file << "        \"averageDistanceFromBase\": " << stats.averageDistanceFromBase << ",\n";
        file << "        \"averageDistanceFromCenter\": " << stats.averageDistanceFromCenter << ",\n";
        file << "        \"lastPosition\": ";
        WriteJsonVector3(file, stats.lastPosition);
        file << ",\n";
        file << "        \"minPosition\": ";
        WriteJsonVector3(file, stats.minPosition);
        file << ",\n";
        file << "        \"maxPosition\": ";
        WriteJsonVector3(file, stats.maxPosition);
        file << "\n";
        file << "      }\n";
        file << "    }" << (i + 1 < automatch_.botStats.size() ? "," : "") << "\n";
    }
    file << "  ]\n";
    file << "}\n";
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
        SetMessage("30:00 reached. All EnergyCores collapsed. Final lives only.", 6.0f);
        AddEventMessage("All EnergyCores collapsed", Color { 255, 118, 118, 255 }, 6.0f);
        audio_.PlayCoreDestroyed();
        cameraController_.AddShake(0.34f, 0.45f);
    }
}

void Game::ApplyBotLoadout(Player& bot) const
{
    bot.GetInventory().AddItem(ItemType::Sword, 1);
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
            cameraController_.AddShake(0.10f, 0.12f);
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

void Game::UseUtilityInputs(Player& player)
{
    if (shopOpen_ || inventoryOpen_)
    {
        return;
    }

    const auto useHotbarUtility = [this, &player](UtilityType type)
    {
        const ItemType itemType = ItemFromUtility(type);
        const auto& hotbar = player.GetInventory().GetHotbarSlots();
        for (int i = 0; i < kHotbarSlotCount; ++i)
        {
            if (!hotbar[i].IsEmpty() && hotbar[i].type == itemType)
            {
                selectedHotbarSlot_ = i;
                UseUtility(player, type);
                return;
            }
        }

        SetMessage(std::string("No ") + ItemDisplayName(itemType) + " in hotbar.");
        audio_.PlayDenied();
    };

    if (currentInput_.healPressed)
    {
        useHotbarUtility(UtilityType::Heal);
    }
    if (currentInput_.teleportPressed)
    {
        useHotbarUtility(UtilityType::HomeTeleport);
    }
    if (currentInput_.dashPressed)
    {
        useHotbarUtility(UtilityType::Dash);
    }
    if (currentInput_.shootPressed)
    {
        useHotbarUtility(UtilityType::Arrows);
    }
    if (currentInput_.fireballPressed)
    {
        useHotbarUtility(UtilityType::Fireball);
    }
    if (currentInput_.molotovPressed)
    {
        useHotbarUtility(UtilityType::Molotov);
    }
    if (currentInput_.alarmPressed)
    {
        useHotbarUtility(UtilityType::AlarmTrap);
    }
}

bool Game::UseUtility(Player& player, UtilityType type)
{
    if (type == UtilityType::Arrows || type == UtilityType::Fireball || type == UtilityType::Molotov)
    {
        LaunchProjectile(player, type);
        return true;
    }

    if (type == UtilityType::Heal)
    {
        if (SpendUtilityItem(player, UtilityType::Heal))
        {
            player.Heal(45);
            SetMessage("Med kit used.");
            AddWorldEffect(player.GetPosition(), Color { 128, 238, 166, 255 }, 0.36f, 0.35f);
            audio_.PlayPickup();
            return true;
        }
        SetMessage("No med kits. Buy one in Utility.");
        audio_.PlayDenied();
        return false;
    }

    if (type == UtilityType::HomeTeleport)
    {
        Team* team = FindTeam(player.GetTeamId());
        if (team != nullptr && SpendUtilityItem(player, UtilityType::HomeTeleport))
        {
            player.Respawn(team->spawnPoint);
            SetMessage("Teleported home.");
            AddWorldEffect(team->spawnPoint, GetTeamColor(team->color), 0.42f, 0.45f);
            audio_.PlayPickup();
            return true;
        }
        if (team != nullptr)
        {
            SetMessage("No home teleports. Buy one in Utility.");
            audio_.PlayDenied();
        }
        return false;
    }

    if (type == UtilityType::Dash)
    {
        if (SpendUtilityItem(player, UtilityType::Dash))
        {
            const Vector3 forward = cameraController_.GetFlatForward();
            player.ApplyKnockback(Vector3 { forward.x * 8.5f, 1.5f, forward.z * 8.5f });
            SetMessage("Dash pearl used.");
            AddWorldEffect(player.GetPosition(), Color { 112, 232, 255, 255 }, 0.30f, 0.30f);
            audio_.PlayPickup();
            return true;
        }
        SetMessage("No dash pearls. Buy one in Utility.");
        audio_.PlayDenied();
        return false;
    }

    if (type == UtilityType::AlarmTrap)
    {
        Team* team = FindTeam(player.GetTeamId());
        if (team != nullptr && SpendUtilityItem(player, UtilityType::AlarmTrap))
        {
            alarmTraps_.push_back(AlarmTrap { team->spawnPoint, player.GetTeamId(), 5.2f, false });
            SetMessage("Alarm trap armed at your base.");
            AddWorldEffect(team->spawnPoint, Color { 255, 235, 142, 255 }, 0.32f, 0.35f);
            audio_.PlayPickup();
            return true;
        }
        if (team != nullptr)
        {
            SetMessage("No alarm traps. Buy one in Utility.");
            audio_.PlayDenied();
        }
        return false;
    }

    return false;
}

bool Game::SpendUtilityItem(Player& player, UtilityType type)
{
    Inventory& inventory = player.GetInventory();
    if (player.IsLocal())
    {
        const ItemType selectedType = GetSelectedHotbarStack(player).type;
        if (ItemToUtility(selectedType) == type && inventory.SpendSlotItem(selectedHotbarSlot_))
        {
            return true;
        }
    }

    return inventory.SpendUtility(type);
}

void Game::UseSelectedItem(Player& player)
{
    const ItemStack stack = GetSelectedHotbarStack(player);
    if (stack.IsEmpty())
    {
        SetMessage("Selected slot is empty.");
        audio_.PlayDenied();
        return;
    }

    const std::optional<BlockType> blockType = ItemToBlock(stack.type);
    if (blockType.has_value())
    {
        HandlePlaceBlock();
        return;
    }

    const std::optional<UtilityType> utility = ItemToUtility(stack.type);
    if (utility.has_value())
    {
        UseUtility(player, *utility);
        return;
    }

    SetMessage(std::string(ItemDisplayName(stack.type)) + " selected.");
}

void Game::HandleInventoryInput(Player& player)
{
    const auto visualToSlot = [](int row, int col)
    {
        col = std::clamp(col, 0, 8);
        row = std::clamp(row, 0, 3);
        return row == 3 ? col : kHotbarSlotCount + row * 9 + col;
    };
    const auto slotToVisual = [](int slot, int& row, int& col)
    {
        if (slot < kHotbarSlotCount)
        {
            row = 3;
            col = slot;
            return;
        }

        const int mainSlot = slot - kHotbarSlotCount;
        row = mainSlot / 9;
        col = mainSlot % 9;
    };
    const auto slotAtMouse = []() -> int
    {
        const int slotSize = 50;
        const int gap = 8;
        const int hotbarWidth = slotSize * kHotbarSlotCount + gap * (kHotbarSlotCount - 1);
        const int panelWidth = hotbarWidth + 42;
        const int panelHeight = 322;
        const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
        const int panelY = GetScreenHeight() / 2 - panelHeight / 2;
        const int gridX = panelX + 21;
        const int gridY = panelY + 58;
        const int inventoryHotbarY = gridY + 3 * (slotSize + gap) + 14;
        const Vector2 mouse = GetMousePosition();

        const auto hitGrid = [mouse, slotSize, gap](int x, int y, int cols, int rows, int firstSlot) -> int
        {
            for (int row = 0; row < rows; ++row)
            {
                for (int col = 0; col < cols; ++col)
                {
                    const Rectangle bounds {
                        static_cast<float>(x + col * (slotSize + gap)),
                        static_cast<float>(y + row * (slotSize + gap)),
                        static_cast<float>(slotSize),
                        static_cast<float>(slotSize)
                    };
                    if (CheckCollisionPointRec(mouse, bounds))
                    {
                        return firstSlot + row * cols + col;
                    }
                }
            }
            return -1;
        };

        const int mainSlot = hitGrid(gridX, gridY, 9, 3, kHotbarSlotCount);
        if (mainSlot >= 0)
        {
            return mainSlot;
        }
        return hitGrid(gridX, inventoryHotbarY, 9, 1, 0);
    };

    int row = 0;
    int col = 0;
    slotToVisual(inventoryCursorSlot_, row, col);
    const int hoveredSlot = slotAtMouse();
    if (hoveredSlot >= 0)
    {
        inventoryCursorSlot_ = hoveredSlot;
        slotToVisual(inventoryCursorSlot_, row, col);
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A))
    {
        col = (col + 8) % 9;
    }
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D))
    {
        col = (col + 1) % 9;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        row = std::max(0, row - 1);
    }
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        row = std::min(3, row + 1);
    }
    inventoryCursorSlot_ = visualToSlot(row, col);

    if (currentInput_.hotbarSlot > 0)
    {
        inventoryCursorSlot_ = currentInput_.hotbarSlot - 1;
    }

    if (IsKeyPressed(KEY_C))
    {
        TryOpenBaseChest(player);
    }
    if ((teamChestOpen_ || personalChestOpen_) && IsKeyPressed(KEY_X))
    {
        Inventory* chest = nullptr;
        if (teamChestOpen_)
        {
            chest = &teamChests_[std::clamp(player.GetTeamId(), 0, static_cast<int>(teamChests_.size()) - 1)];
        }
        else if (personalChestOpen_)
        {
            chest = &personalChest_;
        }

        if (chest != nullptr)
        {
            if (!heldInventoryStack_.IsEmpty())
            {
                for (int i = 0; i < kInventorySlotCount && !heldInventoryStack_.IsEmpty(); ++i)
                {
                    chest->PlaceStack(i, heldInventoryStack_);
                }
                SetMessage("Stored items in chest.", 1.1f);
            }
            else
            {
                for (int i = 0; i < kInventorySlotCount; ++i)
                {
                    heldInventoryStack_ = chest->TakeSlot(i);
                    if (!heldInventoryStack_.IsEmpty())
                    {
                        SetMessage("Took stack from chest.", 1.1f);
                        break;
                    }
                }
            }
            audio_.PlayPickup();
        }
        return;
    }
    if (IsKeyPressed(KEY_Q) && heldInventoryStack_.IsEmpty() && inventoryCursorSlot_ >= 0)
    {
        const ItemStack stack = player.GetInventory().GetSlot(inventoryCursorSlot_);
        if (!stack.IsEmpty())
        {
            TryDropInventoryStack(player, inventoryCursorSlot_, stack.count);
        }
        return;
    }
    if ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
        && heldInventoryStack_.IsEmpty()
        && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)))
    {
        if (TryQuickMoveInventorySlot(player, inventoryCursorSlot_))
        {
            audio_.PlayPickup();
        }
        return;
    }

    if (hoveredSlot >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && heldInventoryStack_.IsEmpty())
    {
        Inventory& inventory = player.GetInventory();
        const ItemStack stack = inventory.GetSlot(hoveredSlot);
        if (!stack.IsEmpty() && stack.count > 1)
        {
            const int taken = stack.count / 2;
            heldInventoryStack_ = ItemStack { stack.type, taken };
            ItemStack original = inventory.TakeSlot(hoveredSlot);
            original.count -= taken;
            inventory.PlaceStack(hoveredSlot, original);
            SetMessage("Split stack.", 1.0f);
            audio_.PlayPickup();
        }
        return;
    }

    const bool actionPressed = IsKeyPressed(KEY_ENTER)
        || IsKeyPressed(KEY_SPACE)
        || (hoveredSlot >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
    if (!actionPressed)
    {
        return;
    }

    Inventory& inventory = player.GetInventory();
    if (heldInventoryStack_.IsEmpty())
    {
        heldInventoryStack_ = inventory.TakeSlot(inventoryCursorSlot_);
        if (!heldInventoryStack_.IsEmpty())
        {
            SetMessage(std::string("Picked up ") + ItemDisplayName(heldInventoryStack_.type) + ".", 1.2f);
            audio_.PlayPickup();
        }
        return;
    }

    if (inventory.PlaceStack(inventoryCursorSlot_, heldInventoryStack_))
    {
        SetMessage("Stack placed.", 1.0f);
        audio_.PlayPickup();
        return;
    }

    heldInventoryStack_ = inventory.SwapSlot(inventoryCursorSlot_, heldInventoryStack_);
    SetMessage(heldInventoryStack_.IsEmpty() ? "Stack placed." : "Items swapped.", 1.0f);
    audio_.PlayPickup();
}

bool Game::TryDropInventoryStack(Player& player, int slot, int amount)
{
    Inventory& inventory = player.GetInventory();
    if (!inventory.IsValidSlot(slot) || amount <= 0)
    {
        return false;
    }

    const ItemStack stack = inventory.GetSlot(slot);
    if (stack.IsEmpty())
    {
        return false;
    }

    const int droppedCount = std::min(amount, stack.count);
    if (!inventory.SpendSlotItem(slot, droppedCount))
    {
        return false;
    }

    const Vector3 forward = player.Forward();
    droppedItems_.push_back(DroppedItem {
        ItemStack { stack.type, droppedCount },
        Vector3 {
            player.GetPosition().x + forward.x * 0.85f,
            player.GetPosition().y + 0.35f,
            player.GetPosition().z + forward.z * 0.85f },
        Vector3 { forward.x * 2.2f, 2.0f, forward.z * 2.2f },
        player.GetId(),
        0.85f,
        45.0f,
        0.0f,
        false });
    SetMessage(std::string("Dropped ") + ItemDisplayName(stack.type) + ".", 1.2f);
    AddFloatingText("drop", player.GetPosition(), Fade(WHITE, 0.85f));
    return true;
}

bool Game::TryQuickMoveInventorySlot(Player& player, int slot)
{
    Inventory& inventory = player.GetInventory();
    if (!inventory.IsValidSlot(slot))
    {
        return false;
    }

    ItemStack stack = inventory.TakeSlot(slot);
    if (stack.IsEmpty())
    {
        return false;
    }

    const int start = slot < kHotbarSlotCount ? kHotbarSlotCount : 0;
    const int end = slot < kHotbarSlotCount ? kInventorySlotCount : kHotbarSlotCount;
    for (int i = start; i < end && !stack.IsEmpty(); ++i)
    {
        inventory.PlaceStack(i, stack);
    }
    if (!stack.IsEmpty())
    {
        inventory.PlaceStack(slot, stack);
    }
    SetMessage("Quick moved stack.", 1.0f);
    return true;
}

void Game::HandleDeathInventory(Player& player, int killerId)
{
    Player* killer = nullptr;
    if (killerId >= 0 && killerId != player.GetId())
    {
        for (Player& candidate : players_)
        {
            if (candidate.GetId() == killerId && candidate.IsAlive() && !candidate.IsEliminated())
            {
                killer = &candidate;
                break;
            }
        }
    }

    Inventory& inventory = player.GetInventory();
    inventory.DowngradeSword();
    inventory.DowngradeTool();

    for (int slot = 0; slot < kInventorySlotCount; ++slot)
    {
        const ItemStack stack = inventory.GetSlot(slot);
        if (stack.IsEmpty())
        {
            continue;
        }

        inventory.SpendSlotItem(slot, stack.count);
        const std::optional<ResourceType> resource = ItemToResource(stack.type);
        if (killer != nullptr && resource.has_value())
        {
            killer->GetInventory().AddResource(*resource, stack.count);
        }
    }

    inventory.AddItem(ItemType::Sword, 1);
    if (inventory.GetToolLevel() > 0)
    {
        inventory.AddItem(ItemType::Pickaxe, 1);
    }
    if (player.IsLocal())
    {
        selectedHotbarSlot_ = 0;
    }
}

void Game::NoteDamageCredit(int targetId, int attackerId)
{
    if (targetId < 0 || attackerId < 0 || targetId == attackerId)
    {
        return;
    }

    for (DamageCredit& credit : damageCredits_)
    {
        if (credit.targetId == targetId)
        {
            credit.attackerId = attackerId;
            credit.timer = 8.0f;
            return;
        }
    }

    damageCredits_.push_back(DamageCredit { targetId, attackerId, 8.0f });
}

int Game::DeathCreditFor(int targetId) const
{
    for (const DamageCredit& credit : damageCredits_)
    {
        if (credit.targetId == targetId && credit.timer > 0.0f)
        {
            return credit.attackerId;
        }
    }

    return -1;
}

void Game::UpdateDamageCredits(float dt)
{
    for (DamageCredit& credit : damageCredits_)
    {
        credit.timer -= dt;
    }

    damageCredits_.erase(
        std::remove_if(
            damageCredits_.begin(),
            damageCredits_.end(),
            [](const DamageCredit& credit)
            {
                return credit.timer <= 0.0f;
            }),
        damageCredits_.end());
}

void Game::TryOpenBaseChest(Player& player)
{
    const Team* team = FindTeam(player.GetTeamId());
    if (team == nullptr || DistanceSquared(player.GetPosition(), team->shopPosition) > 12.0f)
    {
        SetMessage("Move to your base to open chests.", 1.4f);
        audio_.PlayDenied();
        return;
    }

    if (!inventoryOpen_)
    {
        inventoryOpen_ = true;
        EnableCursor();
    }
    if (teamChestOpen_)
    {
        teamChestOpen_ = false;
        personalChestOpen_ = true;
        SetMessage("Personal chest opened.", 1.2f);
    }
    else if (personalChestOpen_)
    {
        CloseChest();
        SetMessage("Chest closed.", 1.0f);
    }
    else
    {
        teamChestOpen_ = true;
        personalChestOpen_ = false;
        SetMessage("Team chest opened.", 1.2f);
    }
    audio_.PlayPickup();
}

void Game::CloseChest()
{
    teamChestOpen_ = false;
    personalChestOpen_ = false;
}

bool Game::TryShopPurchase(Player& player, Team& team, int choice, int repeat, std::string& message)
{
    if (choice == 303)
    {
        return RepairTeamCore(player, team, message);
    }

    const int attempts = std::max(1, repeat);
    int bought = 0;
    std::string lastMessage;
    for (int i = 0; i < attempts; ++i)
    {
        std::string purchaseMessage;
        if (!shop_.Purchase(player, team, choice, purchaseMessage))
        {
            if (bought == 0)
            {
                message = purchaseMessage;
                return false;
            }
            break;
        }
        lastMessage = purchaseMessage;
        ++bought;
    }

    message = bought > 1 ? ("Bought x" + std::to_string(bought) + ". " + lastMessage) : lastMessage;
    return bought > 0;
}

void Game::LaunchProjectile(Player& player, UtilityType type)
{
    if (!SpendUtilityItem(player, type))
    {
        if (type == UtilityType::Arrows)
        {
            SetMessage("No energy arrows. Buy them in Combat.");
        }
        else if (type == UtilityType::Fireball)
        {
            SetMessage("No fireballs. Buy one in Combat.");
        }
        else
        {
            SetMessage("No molotovs. Buy one in Utility.");
        }
        audio_.PlayDenied();
        return;
    }

    const Vector3 direction = cameraController_.GetAimDirection();
    LaunchProjectileDirected(player, type, direction, true);
}

void Game::LaunchProjectileDirected(Player& player, UtilityType type, Vector3 direction, bool announce)
{
    const float directionLength = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (directionLength <= 0.0001f)
    {
        direction = player.Forward();
    }
    else
    {
        direction = Vector3 { direction.x / directionLength, direction.y / directionLength, direction.z / directionLength };
    }

    EnergyProjectile projectile {};
    projectile.position = Vector3 {
        player.GetPosition().x + direction.x * 0.75f,
        player.GetPosition().y + 0.82f + direction.y * 0.75f,
        player.GetPosition().z + direction.z * 0.75f
    };
    projectile.ownerId = player.GetId();
    projectile.ownerTeamId = player.GetTeamId();
    projectile.velocity = Vector3 { direction.x * 18.0f, direction.y * 18.0f, direction.z * 18.0f };
    projectile.damage = 24 + player.GetInventory().GetSwordLevel() * 4;
    projectile.radius = 0.18f;

    if (type == UtilityType::Fireball)
    {
        projectile.velocity = Vector3 { direction.x * 12.0f, direction.y * 12.0f, direction.z * 12.0f };
        projectile.damage = 34;
        projectile.radius = 0.32f;
        projectile.explosionRadius = 2.8f;
    }
    else if (type == UtilityType::Molotov)
    {
        projectile.velocity = Vector3 { direction.x * 10.0f, direction.y * 10.0f + 2.0f, direction.z * 10.0f };
        projectile.damage = 12;
        projectile.radius = 0.28f;
        projectile.explosionRadius = 1.6f;
        projectile.fireZone = true;
    }

    projectiles_.push_back(projectile);
    if (announce)
    {
        SetMessage(type == UtilityType::Arrows ? "Energy arrow fired." : (type == UtilityType::Fireball ? "Fireball launched." : "Molotov thrown."));
        audio_.PlayBreakBlock();
    }
}

void Game::DetonateAt(Vector3 position, int ownerTeamId, int ownerPlayerId, float radius, int damage, bool createFireZone)
{
    const int blockRadius = static_cast<int>(std::ceil(radius));
    const GridPos center = world_.WorldToGrid(position);
    for (int x = center.x - blockRadius; x <= center.x + blockRadius; ++x)
    {
        for (int y = center.y - blockRadius; y <= center.y + blockRadius; ++y)
        {
            for (int z = center.z - blockRadius; z <= center.z + blockRadius; ++z)
            {
                const GridPos pos { x, y, z };
                if (DistanceSquared(world_.GridToWorld(pos), position) > radius * radius)
                {
                    continue;
                }
                const Block* block = world_.GetBlock(pos);
                if (block != nullptr && block->breakable && IsBreakableByPlayers(block->type))
                {
                    world_.BreakBlock(pos, ownerTeamId);
                }
            }
        }
    }

    for (Player& player : players_)
    {
        if (!player.IsAlive() || player.GetTeamId() == ownerTeamId)
        {
            continue;
        }
        const float distance = std::sqrt(DistanceSquared(player.GetPosition(), position));
        if (distance > radius + 0.7f)
        {
            continue;
        }
        const int scaledDamage = std::max(6, static_cast<int>(static_cast<float>(damage) * (1.0f - std::min(distance / (radius + 0.7f), 0.82f))));
        NoteDamageCredit(player.GetId(), ownerPlayerId);
        player.Damage(scaledDamage);
        const Vector3 away = Normalize2D(Vector3 { player.GetPosition().x - position.x, 0.0f, player.GetPosition().z - position.z });
        const float knockback = BiomeKnockbackMultiplier();
        player.ApplyKnockback(Vector3 { away.x * 5.4f * knockback, 2.4f * knockback, away.z * 5.4f * knockback });
    }

    if (createFireZone)
    {
        hazardZones_.push_back(HazardZone { position, ownerTeamId, ownerPlayerId, 2.4f, 5.0f, 0.0f });
    }

    AddWorldEffect(position, createFireZone ? Color { 255, 118, 70, 255 } : Color { 255, 224, 122, 255 }, radius * 0.24f, 0.55f);
    cameraController_.AddShake(0.18f + radius * 0.04f, 0.24f);
    audio_.PlayCoreDestroyed();
}

void Game::UpdateAlarmTraps()
{
    for (AlarmTrap& trap : alarmTraps_)
    {
        if (trap.triggered)
        {
            continue;
        }

        for (Player& player : players_)
        {
            if (!player.IsAlive() || player.GetTeamId() == trap.ownerTeamId)
            {
                continue;
            }
            if (DistanceSquared(player.GetPosition(), trap.position) <= trap.radius * trap.radius)
            {
                trap.triggered = true;
                player.Damage(12);
                const Team* owner = FindTeam(trap.ownerTeamId);
                AddEventMessage((owner != nullptr ? owner->name : "Base") + std::string(" alarm triggered!"), Color { 255, 235, 142, 255 }, 3.0f);
                AddFloatingText("ALARM", player.GetPosition(), Color { 255, 235, 142, 255 });
                AddWorldEffect(player.GetPosition(), Color { 255, 235, 142, 255 }, 0.42f, 0.45f);
                audio_.PlayDenied();
                break;
            }
        }
    }

    alarmTraps_.erase(
        std::remove_if(
            alarmTraps_.begin(),
            alarmTraps_.end(),
            [](const AlarmTrap& trap)
            {
                return trap.triggered;
            }),
        alarmTraps_.end());
}

void Game::UpdateCamera(float dt)
{
    if (spectatorMode_)
    {
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

    const float targetFov = fov_ + (player->IsSprinting() ? 6.0f : 0.0f);
    gameplayFov_ += (targetFov - gameplayFov_) * std::min(1.0f, dt * 12.0f);
    cameraController_.SetFov(gameplayFov_);
    cameraController_.Update(player->GetPosition(), dt);
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
    UseUtilityInputs(*player);

    const bool wasOnGround = player->IsOnGround();
    const float fallingVelocity = player->GetVelocity().y;
    if (wasOnGround)
    {
        localAirPeakY_ = player->GetPosition().y;
    }
    const bool wantsForwardSprint = currentInput_.move.z > 0.05f;
    const bool sprint = currentInput_.sprint && wantsForwardSprint && !currentInput_.sneak && !currentInput_.bridgeMode;
    if (currentInput_.sprintTapped && sprint)
    {
        player->RefreshSprintReset();
    }
    player->Move(
        wish,
        currentInput_.jump,
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
        cameraController_.AddShake(strength, 0.16f);
        audio_.PlayLanding();
        localAirPeakY_ = player->GetPosition().y;
    }

    localWasOnGround_ = player->IsOnGround();
    localFallVelocity_ = player->GetVelocity().y;
}

void Game::UpdateGenerators(float dt)
{
    for (Generator& generator : generators_)
    {
        generator.Update(dt, pickups_, GetForgeBonusForTeam(generator.GetTeamId()));
    }
}

void Game::UpdatePickups(float dt)
{
    for (ResourcePickup& pickup : pickups_)
    {
        pickup.age += dt;
        pickup.lifetime -= dt;
        if (pickup.lifetime <= 0.0f)
        {
            pickup.collected = true;
            continue;
        }

        if (Player* magnetTarget = FindMagnetTarget(players_, pickup.position, -1, 0.0f, pickup.age))
        {
            const Vector3 target = PickupTargetFor(*magnetTarget);
            const Vector3 toTarget {
                target.x - pickup.position.x,
                target.y - pickup.position.y,
                target.z - pickup.position.z
            };
            const float distance = Length(toTarget);
            if (distance > 0.0001f)
            {
                const Vector3 direction = Normalize(toTarget);
                const float pull = kResourceMagnetSpeed * dt * (1.0f + (1.0f - std::min(distance / kItemMagnetRadius, 1.0f)) * 1.35f);
                const float step = std::min(distance, pull);
                pickup.position.x += direction.x * step;
                pickup.position.y += direction.y * step;
                pickup.position.z += direction.z * step;
            }
        }

        for (Player& player : players_)
        {
            if (!player.IsAlive())
            {
                continue;
            }

            if (DistanceSquared(PickupTargetFor(player), pickup.position) <= kItemPickupRadiusSq)
            {
                player.GetInventory().AddResource(pickup.type, pickup.amount);
                pickup.collected = true;
                AddWorldEffect(pickup.position, player.IsLocal() ? Color { 255, 245, 170, 255 } : Color { 180, 210, 255, 255 }, 0.22f, 0.28f);
                AddFloatingText("+" + std::to_string(pickup.amount) + " " + ToString(pickup.type), pickup.position, player.IsLocal() ? Color { 255, 236, 135, 255 } : Fade(WHITE, 0.85f));
                if (player.IsLocal())
                {
                    ++stats_.resourcesPicked;
                    SetMessage("Picked up " + std::to_string(pickup.amount) + " " + ToString(pickup.type) + ".");
                    audio_.PlayPickup();
                }
                break;
            }
        }
    }

    MergeNearbyResourcePickups(pickups_);

    pickups_.erase(
        std::remove_if(
            pickups_.begin(),
            pickups_.end(),
            [](const ResourcePickup& pickup)
            {
                return pickup.collected;
            }),
        pickups_.end());
}

void Game::UpdateDroppedItems(float dt)
{
    for (DroppedItem& dropped : droppedItems_)
    {
        if (dropped.collected)
        {
            continue;
        }

        dropped.age += dt;
        dropped.lifetime -= dt;
        dropped.velocity.y -= 9.0f * dt;
        dropped.position.x += dropped.velocity.x * dt;
        dropped.position.y += dropped.velocity.y * dt;
        dropped.position.z += dropped.velocity.z * dt;
        const GridPos under = world_.WorldToGrid(Vector3 { dropped.position.x, dropped.position.y - 0.22f, dropped.position.z });
        if (!world_.IsAir(under) && dropped.velocity.y < 0.0f)
        {
            dropped.position.y = world_.GridToWorld(under).y + 0.72f;
            dropped.velocity = Vector3 { dropped.velocity.x * 0.72f, 0.0f, dropped.velocity.z * 0.72f };
        }

        if (Player* magnetTarget = FindMagnetTarget(players_, dropped.position, dropped.ownerPlayerId, dropped.ownerPickupDelay, dropped.age))
        {
            const Vector3 target = PickupTargetFor(*magnetTarget);
            const Vector3 toTarget {
                target.x - dropped.position.x,
                target.y - dropped.position.y,
                target.z - dropped.position.z
            };
            const float distance = Length(toTarget);
            if (distance > 0.0001f)
            {
                const Vector3 direction = Normalize(toTarget);
                const float closeness = 1.0f - std::min(distance / kItemMagnetRadius, 1.0f);
                const float accel = kDroppedItemMagnetAccel * (0.55f + closeness * 1.15f);
                dropped.velocity.x += direction.x * accel * dt;
                dropped.velocity.y += direction.y * accel * dt;
                dropped.velocity.z += direction.z * accel * dt;

                const float speed = Length(dropped.velocity);
                if (speed > kDroppedItemMagnetMaxSpeed)
                {
                    const Vector3 capped = Normalize(dropped.velocity);
                    dropped.velocity = Vector3 {
                        capped.x * kDroppedItemMagnetMaxSpeed,
                        capped.y * kDroppedItemMagnetMaxSpeed,
                        capped.z * kDroppedItemMagnetMaxSpeed
                    };
                }
            }
        }

        for (Player& player : players_)
        {
            if (!player.IsAlive() || player.IsEliminated())
            {
                continue;
            }
            if (player.GetId() == dropped.ownerPlayerId && dropped.age < dropped.ownerPickupDelay)
            {
                continue;
            }

            if (DistanceSquared(PickupTargetFor(player), dropped.position) <= kItemPickupRadiusSq)
            {
                const std::optional<ResourceType> resource = ItemToResource(dropped.stack.type);
                const bool added = resource.has_value()
                    ? (player.GetInventory().AddResource(*resource, dropped.stack.count), true)
                    : player.GetInventory().AddItem(dropped.stack.type, dropped.stack.count);
                if (added)
                {
                    dropped.collected = true;
                    AddWorldEffect(dropped.position, Color { 255, 245, 170, 255 }, 0.18f, 0.22f);
                    if (player.IsLocal())
                    {
                        SetMessage(std::string("Picked up ") + ItemDisplayName(dropped.stack.type) + ".");
                        audio_.PlayPickup();
                    }
                    break;
                }
            }
        }
    }

    MergeNearbyDroppedItems(droppedItems_);

    droppedItems_.erase(
        std::remove_if(
            droppedItems_.begin(),
            droppedItems_.end(),
            [](const DroppedItem& dropped)
            {
                return dropped.collected || dropped.lifetime <= 0.0f;
            }),
        droppedItems_.end());
}

void Game::UpdateBlockHazards(float dt)
{
    blockHazardTimer_ += dt;
    if (blockHazardTimer_ < 0.45f)
    {
        return;
    }
    blockHazardTimer_ = 0.0f;

    for (Player& player : players_)
    {
        if (!player.IsAlive())
        {
            continue;
        }

        const Vector3 pos = player.GetPosition();
        const GridPos underFeet = world_.WorldToGrid(Vector3 { pos.x, pos.y - 1.05f, pos.z });
        const Block* block = world_.GetBlock(underFeet);
        if (block == nullptr)
        {
            continue;
        }

        if (block->type == BlockType::SpikeBlock || block->type == BlockType::LavaBlock)
        {
            const int damage = block->type == BlockType::LavaBlock ? 12 : 7;
            player.Damage(damage);
            AddWorldEffect(player.GetPosition(), block->type == BlockType::LavaBlock ? Color { 255, 88, 42, 255 } : Color { 255, 118, 118, 255 }, 0.20f, 0.20f);
            AddFloatingText(block->type == BlockType::LavaBlock ? "burn" : "spike", player.GetPosition(), block->type == BlockType::LavaBlock ? Color { 255, 128, 72, 255 } : Color { 255, 118, 118, 255 });
            if (player.IsLocal())
            {
                damageFlashTimer_ = std::max(damageFlashTimer_, 0.35f);
            }
        }
        else if (arenaBiome_ == ArenaBiome::Lava && pos.y < 0.25f)
        {
            const Team* team = FindTeam(player.GetTeamId());
            const bool inBaseSafeZone = team != nullptr && DistanceSquared(pos, team->spawnPoint) < 105.0f;
            if (!inBaseSafeZone)
            {
                player.Damage(5);
                AddWorldEffect(player.GetPosition(), Color { 255, 88, 42, 255 }, 0.18f, 0.18f);
                AddFloatingText("heat", player.GetPosition(), Color { 255, 128, 72, 255 });
                if (player.IsLocal())
                {
                    damageFlashTimer_ = std::max(damageFlashTimer_, 0.32f);
                    SetMessage("Lava biome heat: climb to safer ground.", 1.2f);
                }
            }
        }
    }
}

void Game::UpdateExplosives(float dt)
{
    for (TimedExplosion& explosive : timedExplosions_)
    {
        explosive.timer -= dt;
        const Vector3 pos = world_.GridToWorld(explosive.block);
        if (explosive.timer > 0.0f)
        {
            AddWorldEffect(pos, explosive.timer < 0.8f ? Color { 255, 118, 70, 255 } : Color { 255, 224, 122, 255 }, 0.10f, 0.12f);
            continue;
        }

        DetonateAt(pos, explosive.ownerTeamId, explosive.ownerPlayerId, explosive.radius, 48, false);
    }

    timedExplosions_.erase(
        std::remove_if(
            timedExplosions_.begin(),
            timedExplosions_.end(),
            [](const TimedExplosion& explosive)
            {
                return explosive.timer <= 0.0f;
            }),
        timedExplosions_.end());
}

void Game::UpdateProjectiles(float dt)
{
    for (EnergyProjectile& projectile : projectiles_)
    {
        projectile.lifetime -= dt;
        projectile.velocity.y -= 5.5f * dt;
        projectile.position.x += projectile.velocity.x * dt;
        projectile.position.y += projectile.velocity.y * dt;
        projectile.position.z += projectile.velocity.z * dt;

        bool consumed = projectile.lifetime <= 0.0f || projectile.position.y < -8.0f;
        const GridPos projectileBlock = world_.WorldToGrid(projectile.position);
        if (!consumed && !world_.IsAir(projectileBlock))
        {
            if (projectile.explosionRadius > 0.0f)
            {
                DetonateAt(projectile.position, projectile.ownerTeamId, projectile.ownerId, projectile.explosionRadius, projectile.damage, projectile.fireZone);
            }
            consumed = true;
        }

        if (!consumed)
        {
            for (Player& player : players_)
            {
                if (player.GetId() == projectile.ownerId
                    || player.GetTeamId() == projectile.ownerTeamId
                    || !player.IsAlive())
                {
                    continue;
                }
                if (DistanceSquared(player.GetPosition(), projectile.position) <= 1.05f)
                {
                    NoteDamageCredit(player.GetId(), projectile.ownerId);
                    player.Damage(projectile.damage);
                    const float knockback = BiomeKnockbackMultiplier();
                    player.ApplyKnockback(Vector3 { projectile.velocity.x * 0.12f * knockback, 1.4f * knockback, projectile.velocity.z * 0.12f * knockback });
                    if (projectile.explosionRadius > 0.0f)
                    {
                        DetonateAt(projectile.position, projectile.ownerTeamId, projectile.ownerId, projectile.explosionRadius, projectile.damage, projectile.fireZone);
                    }
                    else
                    {
                        AddWorldEffect(projectile.position, Color { 112, 232, 255, 255 }, 0.26f, 0.25f);
                        audio_.PlayHit();
                    }
                    consumed = true;
                    break;
                }
            }
        }

        if (consumed)
        {
            projectile.lifetime = -1.0f;
        }
        else
        {
            AddWorldEffect(projectile.position, projectile.fireZone ? Color { 255, 118, 70, 255 } : Color { 112, 232, 255, 255 }, projectile.radius, 0.08f);
        }
    }

    projectiles_.erase(
        std::remove_if(
            projectiles_.begin(),
            projectiles_.end(),
            [](const EnergyProjectile& projectile)
            {
                return projectile.lifetime <= 0.0f;
            }),
        projectiles_.end());
}

void Game::UpdateHazardZones(float dt)
{
    for (HazardZone& zone : hazardZones_)
    {
        zone.lifetime -= dt;
        zone.tickTimer -= dt;
        AddWorldEffect(zone.position, Color { 255, 88, 42, 255 }, 0.18f, 0.12f);
        if (zone.tickTimer > 0.0f)
        {
            continue;
        }
        zone.tickTimer = 0.55f;

        for (Player& player : players_)
        {
            if (!player.IsAlive() || player.GetTeamId() == zone.ownerTeamId)
            {
                continue;
            }
            if (DistanceSquared(player.GetPosition(), zone.position) <= zone.radius * zone.radius)
            {
                NoteDamageCredit(player.GetId(), zone.ownerPlayerId);
                player.Damage(8);
                AddWorldEffect(player.GetPosition(), Color { 255, 118, 70, 255 }, 0.22f, 0.18f);
            }
        }
    }

    hazardZones_.erase(
        std::remove_if(
            hazardZones_.begin(),
            hazardZones_.end(),
            [](const HazardZone& zone)
            {
                return zone.lifetime <= 0.0f;
            }),
        hazardZones_.end());
}

void Game::UpdatePassiveRegeneration(float dt)
{
    passiveRegenTimer_ += dt;
    if (passiveRegenTimer_ < 2.0f)
    {
        return;
    }

    passiveRegenTimer_ = 0.0f;
    for (Player& player : players_)
    {
        if (!player.IsAlive() || player.IsEliminated() || player.GetHealth() >= player.GetMaxHealth())
        {
            continue;
        }

        player.Heal(5);
    }
}

void Game::UpdateBaseHealing(float dt)
{
    baseHealTimer_ += dt;
    if (baseHealTimer_ < 0.25f)
    {
        return;
    }

    baseHealTimer_ = 0.0f;
    for (Player& player : players_)
    {
        Team* team = FindTeam(player.GetTeamId());
        if (team == nullptr || !shop_.IsPlayerInShop(player, *team) || player.GetHealth() >= player.GetMaxHealth())
        {
            continue;
        }

        player.Heal(3 + team->healAuraLevel * 2);
        if (player.IsLocal())
        {
            AddWorldEffect(player.GetPosition(), Color { 128, 238, 166, 255 }, 0.18f, 0.18f);
        }
    }
}

void Game::UpdateFeedback(float dt)
{
    hitMarkerTimer_ = std::max(0.0f, hitMarkerTimer_ - dt);
    damageFlashTimer_ = std::max(0.0f, damageFlashTimer_ - dt);

    for (WorldEffect& effect : worldEffects_)
    {
        effect.age += dt;
    }
    worldEffects_.erase(
        std::remove_if(
            worldEffects_.begin(),
            worldEffects_.end(),
            [](const WorldEffect& effect)
            {
                return effect.age >= effect.lifetime;
            }),
        worldEffects_.end());

    for (FloatingText& text : floatingTexts_)
    {
        text.age += dt;
    }
    floatingTexts_.erase(
        std::remove_if(
            floatingTexts_.begin(),
            floatingTexts_.end(),
            [](const FloatingText& text)
            {
                return text.age >= text.lifetime;
            }),
        floatingTexts_.end());

    for (EventMessage& event : eventMessages_)
    {
        event.age += dt;
    }
    eventMessages_.erase(
        std::remove_if(
            eventMessages_.begin(),
            eventMessages_.end(),
            [](const EventMessage& event)
            {
                return event.age >= event.lifetime;
            }),
        eventMessages_.end());

    for (KillFeedEntry& entry : killFeed_)
    {
        entry.age += dt;
    }
    killFeed_.erase(
        std::remove_if(
            killFeed_.begin(),
            killFeed_.end(),
            [](const KillFeedEntry& entry)
            {
                return entry.age >= entry.lifetime;
            }),
        killFeed_.end());
}

void Game::UpdatePlacementPreview()
{
    const Player* player = GetLocalPlayer();
    if (player == nullptr || !player->IsAlive() || winnerTeamId_.has_value() || shopOpen_ || inventoryOpen_)
    {
        placementPreview_ = PlacementPreview {};
        return;
    }

    placementPreview_ = BuildPlacementPreview(*player);
}

void Game::UpdateCombatPreview()
{
    combatPreview_ = CombatPreview {};

    const Player* player = GetLocalPlayer();
    if (player == nullptr || !player->IsAlive() || shopOpen_ || inventoryOpen_ || winnerTeamId_.has_value())
    {
        return;
    }

    const Inventory& inventory = player->GetInventory();
    const std::optional<WeaponType> selectedWeapon = GetSelectedWeaponType(*player);
    if (!selectedWeapon.has_value())
    {
        combatPreview_.visible = true;
        combatPreview_.ready = false;
        combatPreview_.label = "Select a weapon to fight";
        return;
    }

    const float chargeMultiplier = 1.0f;
    combatPreview_.visible = true;
    combatPreview_.ready = player->CanAttack();
    combatPreview_.range = CombatSystem::AttackRange(*selectedWeapon, inventory.GetSwordLevel());
    combatPreview_.damage = static_cast<int>(CombatSystem::BaseDamage(*selectedWeapon, inventory.GetSwordLevel()) * chargeMultiplier + 0.5f);
    float meleeRayLimit = combatPreview_.range;
    const std::optional<RaycastHit> terrainHit = RaycastFromAim(*player, combatPreview_.range);
    if (terrainHit.has_value())
    {
        meleeRayLimit = std::max(0.0f, terrainHit->distance - 0.06f);
        combatPreview_.blockedByTerrain = true;
        combatPreview_.terrainBlockDistance = terrainHit->distance;
    }

    const float cooldownDuration = player->GetAttackCooldownDuration();
    combatPreview_.cooldownFraction = cooldownDuration > 0.0f
        ? 1.0f - player->GetAttackCooldownRemaining() / cooldownDuration
        : 1.0f;

    const std::optional<CombatTargetInfo> target = combat_.FindMeleeTarget(*player, players_, cameraController_.GetAimDirection(), *selectedWeapon, chargeMultiplier, meleeRayLimit);
    if (target.has_value())
    {
        combatPreview_.targetInRange = true;
        combatPreview_.targetName = target->targetName;
        combatPreview_.targetHealth = target->targetHealth;
        combatPreview_.targetMaxHealth = target->targetMaxHealth;
        combatPreview_.damage = target->damage;
        combatPreview_.targetDistance = target->distance;
        combatPreview_.predictedKnockback = target->predictedKnockback;
        combatPreview_.predictedSprintReset = target->predictedSprintReset;
        combatPreview_.predictedCombo = target->predictedCombo;
        combatPreview_.hitZoneName = CombatSystem::HitZoneName(target->hitZone);

        combatPreview_.label = combatPreview_.ready
            ? target->targetName + " " + std::string(CombatSystem::HitZoneName(target->hitZone)) + ": -" + std::to_string(target->damage)
            : target->targetName + " lined up: cooldown";
        if (target->shielded)
        {
            combatPreview_.label += " shielded";
        }
        if (target->lethal && combatPreview_.ready)
        {
            combatPreview_.label += " lethal";
        }
        return;
    }

    if (combatPreview_.ready)
    {
        combatPreview_.label = combatPreview_.blockedByTerrain
            ? std::string(CombatSystem::WeaponName(*selectedWeapon)) + " ready: line blocked"
            : std::string(CombatSystem::WeaponName(*selectedWeapon)) + " ready: aim an enemy inside "
                + FormatTenths(combatPreview_.range) + "m";
    }
    else
    {
        combatPreview_.label = "Melee cooldown "
            + std::to_string(static_cast<int>(combatPreview_.cooldownFraction * 100.0f)) + "%";
    }
}

void Game::UpdateFastPlacement(float dt)
{
    if (!currentInput_.placeHeld || winnerTeamId_.has_value() || shopOpen_ || inventoryOpen_)
    {
        fastPlaceTimer_ = 0.0f;
        return;
    }

    fastPlaceTimer_ -= dt;
    if (fastPlaceTimer_ > 0.0f)
    {
        return;
    }

    const Player* player = GetLocalPlayer();
    if (player == nullptr || !player->IsAlive())
    {
        return;
    }

    if (placementPreview_.visible && placementPreview_.valid)
    {
        HandlePlaceBlock();
    }
    fastPlaceTimer_ = currentInput_.bridgeMode ? 0.16f : 0.22f;
}

void Game::UpdateAttackOrBreak(float dt)
{
    Player* player = GetLocalPlayer();
    if (player == nullptr || !player->IsAlive() || shopOpen_ || inventoryOpen_ || winnerTeamId_.has_value())
    {
        ResetBreakProgress();
        attackChargeActive_ = false;
        attackChargeTimer_ = 0.0f;
        return;
    }

    const float chargeMultiplier = 1.0f;
    const std::optional<WeaponType> selectedWeapon = GetSelectedWeaponType(*player);
    float meleeRayLimit = 0.0f;
    if (selectedWeapon.has_value())
    {
        const float weaponRange = CombatSystem::AttackRange(*selectedWeapon, player->GetInventory().GetSwordLevel());
        meleeRayLimit = weaponRange;
        const std::optional<RaycastHit> terrainHit = RaycastFromAim(*player, weaponRange);
        if (terrainHit.has_value())
        {
            meleeRayLimit = std::max(0.0f, terrainHit->distance - 0.06f);
        }
    }

    if (selectedWeapon.has_value() && currentInput_.attackPressed)
    {
        std::string combatMessage;
        CombatEvent combatEvent;
        if (combat_.Attack(*player, players_, cameraController_.GetAimDirection(), combatMessage, &combatEvent, *selectedWeapon, 1.0f, nullptr, meleeRayLimit))
        {
            RegisterCombatEvent(combatEvent, combatMessage);
            ResetBreakProgress();
            attackChargeActive_ = false;
            attackChargeTimer_ = 0.0f;
            return;
        }
    }

    if (!currentInput_.attackHeld)
    {
        ResetBreakProgress();
        attackChargeActive_ = false;
        attackChargeTimer_ = 0.0f;
        return;
    }

    const std::optional<CombatTargetInfo> meleeTarget = selectedWeapon.has_value()
        ? combat_.FindMeleeTarget(*player, players_, cameraController_.GetAimDirection(), *selectedWeapon, chargeMultiplier, meleeRayLimit)
        : std::optional<CombatTargetInfo> {};
    if (selectedWeapon.has_value() && meleeTarget.has_value())
    {
        ResetBreakProgress();
        return;
    }

    attackChargeActive_ = false;
    attackChargeTimer_ = 0.0f;

    const std::optional<RaycastHit> hit = RaycastFromAim(*player, 5.5f);
    if (!hit.has_value())
    {
        ResetBreakProgress();
        return;
    }

    bool isCore = false;
    float requiredSeconds = BreakSeconds(hit->blockData.type, EffectiveToolLevel(*player));
    std::string label = DisplayName(hit->blockData.type);

    if (hit->blockData.type == BlockType::EnergyCoreBlock)
    {
        EnergyCore* core = FindCoreAt(hit->block);
        if (core == nullptr || core->GetTeamId() == player->GetTeamId())
        {
            ResetBreakProgress();
            return;
        }

        isCore = true;
        label = "Enemy EnergyCore";
    }
    else if (!hit->blockData.breakable || !IsBreakableByPlayers(hit->blockData.type))
    {
        ResetBreakProgress();
        return;
    }

    if (!breakProgress_.visible || breakProgress_.target != hit->block || breakProgress_.isCore != isCore)
    {
        breakProgress_ = BreakProgress { hit->block, hit->blockData.type, true, isCore, 0.0f, label };
    }

    breakProgress_.targetType = hit->blockData.type;
    breakProgress_.isCore = isCore;
    breakProgress_.label = label;
    breakProgress_.fraction += dt / std::max(0.001f, requiredSeconds);

    if (breakProgress_.fraction >= 1.0f)
    {
        CompleteBreakProgress(*player);
    }
}

void Game::ResetBreakProgress()
{
    breakProgress_ = BreakProgress {};
}

void Game::CompleteBreakProgress(Player& player)
{
    if (!breakProgress_.visible)
    {
        return;
    }

    const GridPos target = breakProgress_.target;
    if (breakProgress_.isCore)
    {
        EnergyCore* core = FindCoreAt(target);
        if (core != nullptr)
        {
            std::string coreMessage;
            CombatEvent coreEvent;
            if (combat_.DamageCore(player, *core, coreMessage, &coreEvent, EffectiveToolLevel(player)))
            {
                if (!core->IsAlive())
                {
                    world_.RemoveBlock(core->GetBlockPosition());
                    Team* team = FindTeam(core->GetTeamId());
                    if (team != nullptr)
                    {
                        team->coreAlive = false;
                    }
                }
                RegisterCombatEvent(coreEvent, coreMessage);
                player.GetInventory().DamageTool(2);
            }
        }
        ResetBreakProgress();
        return;
    }

    if (world_.BreakBlock(target, player.GetTeamId()))
    {
        const Vector3 center = world_.GridToWorld(target);
        SetMessage(std::string(DisplayName(breakProgress_.targetType)) + " broken.");
        AddWorldEffect(center, Color { 210, 220, 235, 255 }, 0.28f, 0.25f);
        AddFloatingText("break", center, Color { 210, 220, 235, 255 });
        audio_.PlayPlaceBlock();
        if (player.IsLocal())
        {
            ++stats_.blocksBroken;
        }
        player.GetInventory().DamageTool(1);
    }
    else
    {
        SetMessage("This block is protected.");
        audio_.PlayDenied();
    }

    ResetBreakProgress();
}

void Game::HandlePlaceBlock()
{
    Player* player = GetLocalPlayer();
    if (player == nullptr || !player->IsAlive())
    {
        return;
    }

    const std::optional<BlockType> selectedBlock = GetSelectedBlockType(*player);
    if (!selectedBlock.has_value())
    {
        SetMessage("Select a block in the hotbar to place it.");
        audio_.PlayDenied();
        return;
    }

    if (player->GetInventory().GetHotbarSlots()[selectedHotbarSlot_].count <= 0)
    {
        SetMessage(std::string("No ") + DisplayName(*selectedBlock) + "s in selected slot.");
        audio_.PlayDenied();
        return;
    }

    const PlacementPreview preview = BuildPlacementPreview(*player);
    if (!preview.visible || !preview.valid)
    {
        SetMessage(preview.reason.empty() ? "Cannot place block there." : preview.reason);
        audio_.PlayDenied();
        return;
    }

    if (!TryPlaceBlockForPlayer(*player, preview.position, true))
    {
        audio_.PlayDenied();
    }
}

PlacementPreview Game::BuildPlacementPreview(const Player& player) const
{
    PlacementPreview preview {};
    if (!player.IsAlive())
    {
        return preview;
    }

    const std::optional<BlockType> selectedBlock = GetSelectedBlockType(player);
    if (!selectedBlock.has_value())
    {
        return preview;
    }

    const Vector3 aimDirection = cameraController_.GetAimDirection();
    const Vector3 flatForward = cameraController_.GetFlatForward();
    GridPos placePos {};
    preview.selectedType = *selectedBlock;

    if (currentInput_.bridgeMode)
    {
        const float forwardDistance = aimDirection.y < -0.45f ? 0.55f : 0.92f;
        placePos = world_.WorldToGrid(Vector3 {
            player.GetPosition().x + flatForward.x * forwardDistance,
            player.GetPosition().y - 1.08f,
            player.GetPosition().z + flatForward.z * forwardDistance
        });
    }
    else if (currentInput_.sneak)
    {
        placePos = world_.WorldToGrid(Vector3 {
            player.GetPosition().x,
            player.GetPosition().y - 1.08f,
            player.GetPosition().z
        });
    }
    else
    {
        const std::optional<RaycastHit> hit = RaycastFromAim(player, 5.7f);
        if (hit.has_value())
        {
            placePos = hit->adjacent;
            preview.targetBlock = hit->block;
            preview.faceNormal = hit->normal;
            preview.hasTarget = true;
        }
        else
        {
            preview.reason = "Aim at a block face";
            return preview;
        }
    }

    std::string reason;
    preview.position = placePos;
    preview.visible = true;
    preview.valid = CanPlaceBlockAt(placePos, player, &reason);
    if (preview.valid)
    {
        if (preview.reason.empty())
        {
            preview.reason = currentInput_.bridgeMode
                ? std::string("Bridge: ") + DisplayName(*selectedBlock)
                : std::string("Place: ") + DisplayName(*selectedBlock);
        }
    }
    else
    {
        preview.reason = reason;
    }
    return preview;
}

bool Game::CanPlaceBlockAt(const GridPos& pos, const Player& player, std::string* reason) const
{
    const auto fail = [reason](const std::string& text)
    {
        if (reason != nullptr)
        {
            *reason = text;
        }
        return false;
    };

    std::optional<BlockType> selectedBlock = SelectPlacementBlockForPlayer(player, pos);
    BlockType requestedType = selectedBlock.value_or(BlockType::Air);

    if (!IsBuildableBlock(requestedType))
    {
        return fail("Select a block first");
    }
    if (player.GetInventory().GetBlockCount(requestedType) <= 0)
    {
        return fail(std::string("No ") + DisplayName(requestedType) + "s available");
    }
    if (pos.y < kBuildMinY || pos.y > kBuildMaxY)
    {
        return fail("Build height blocked");
    }
    if (std::abs(pos.x) > kBuildMapRadius || std::abs(pos.z) > kBuildMapRadius)
    {
        return fail("Outside build map");
    }
    if (!world_.IsAir(pos))
    {
        return fail("Space occupied");
    }
    if (!HasAdjacentAnchorBlock(pos))
    {
        return fail("Needs adjacent block");
    }

    const Vector3 blockCenter = world_.GridToWorld(pos);
    if (DistanceSquared(player.GetPosition(), blockCenter) > 38.0f)
    {
        return fail("Too far away");
    }
    if (WouldBlockOverlapPlayer(pos, player.GetId()))
    {
        return fail("Blocked by player");
    }

    for (const EnergyCore& core : cores_)
    {
        if (core.IsAlive() && core.GetBlockPosition() == pos)
        {
            return fail("EnergyCore protected");
        }
    }

    return true;
}

bool Game::HasAdjacentAnchorBlock(const GridPos& pos) const
{
    const GridPos neighbors[] {
        GridPos { pos.x + 1, pos.y, pos.z },
        GridPos { pos.x - 1, pos.y, pos.z },
        GridPos { pos.x, pos.y + 1, pos.z },
        GridPos { pos.x, pos.y - 1, pos.z },
        GridPos { pos.x, pos.y, pos.z + 1 },
        GridPos { pos.x, pos.y, pos.z - 1 }
    };

    for (const GridPos& neighbor : neighbors)
    {
        if (!world_.IsAir(neighbor))
        {
            return true;
        }
    }
    return false;
}

std::optional<BlockType> Game::SelectPlacementBlockForPlayer(const Player& player, const GridPos& pos) const
{
    if (player.IsLocal())
    {
        return GetSelectedBlockType(player);
    }

    const Inventory& inventory = player.GetInventory();
    const Team* team = FindTeam(player.GetTeamId());
    const bool nearOwnCore = team != nullptr
        && DistanceSquared(world_.GridToWorld(pos), world_.GridToWorld(team->coreBlock)) < 14.0f;

    const BlockType defensePriority[] {
        BlockType::ObsidianBlock,
        BlockType::StoneBlock,
        BlockType::EnergyGlassBlock,
        BlockType::WoodBlock,
        BlockType::WoolBlock
    };
    const BlockType bridgePriority[] {
        BlockType::WoolBlock,
        BlockType::WoodBlock,
        BlockType::StoneBlock,
        BlockType::EnergyGlassBlock,
        BlockType::ObsidianBlock
    };

    const BlockType* priority = nearOwnCore ? defensePriority : bridgePriority;
    const int count = nearOwnCore
        ? static_cast<int>(std::size(defensePriority))
        : static_cast<int>(std::size(bridgePriority));
    for (int i = 0; i < count; ++i)
    {
        if (inventory.GetBlockCount(priority[i]) > 0)
        {
            return priority[i];
        }
    }
    return std::nullopt;
}

bool Game::TryPlaceBlockForPlayer(Player& player, const GridPos& pos, bool announce)
{
    std::string reason;
    if (!CanPlaceBlockAt(pos, player, &reason))
    {
        if (announce)
        {
            SetMessage(reason);
        }
        return false;
    }

    std::optional<BlockType> selectedBlock = SelectPlacementBlockForPlayer(player, pos);
    BlockType blockType = selectedBlock.value_or(BlockType::Air);

    if (!IsBuildableBlock(blockType))
    {
        if (announce)
        {
            SetMessage("Select a block first.");
        }
        return false;
    }
    if (player.GetInventory().GetBlockCount(blockType) <= 0)
    {
        if (announce)
        {
            SetMessage(std::string("No ") + DisplayName(blockType) + "s available.");
        }
        return false;
    }

    if (!world_.PlaceBlock(pos, Block { blockType, player.GetTeamId(), true }))
    {
        if (announce)
        {
            SetMessage("Space already occupied.");
        }
        return false;
    }

    if (player.IsLocal())
    {
        player.GetInventory().SpendSlotItem(selectedHotbarSlot_);
    }
    else
    {
        player.GetInventory().SpendBlock(blockType);
    }
    const Vector3 center = world_.GridToWorld(pos);
    const Team* team = FindTeam(player.GetTeamId());
    Color effectColor = team != nullptr ? GetTeamColor(team->color) : WHITE;
    if (blockType == BlockType::EnergyGlassBlock)
    {
        effectColor = Color { 112, 232, 255, 255 };
    }
    AddWorldEffect(center, effectColor, 0.24f, 0.22f);
    if (blockType == BlockType::ExplosiveBlock)
    {
        timedExplosions_.push_back(TimedExplosion { pos, player.GetTeamId(), player.GetId(), 2.6f, 2.7f });
        AddEventMessage("TNT armed: 2.6s", Color { 255, 224, 122, 255 }, 1.8f);
    }
    if (player.IsLocal())
    {
        ++stats_.blocksPlaced;
        SetMessage((currentInput_.bridgeMode ? "Bridge " : "") + std::string(DisplayName(blockType)) + " placed.");
        audio_.PlayBuild();
    }
    else if (announce)
    {
        SetMessage(player.GetName() + " placed a block.");
    }

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
            HandleDeathInventory(player, killerId);
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
                ++stats_.deaths;
                damageFlashTimer_ = 0.9f;
                cameraController_.AddShake(0.28f, 0.25f);
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
                player.Respawn(team->spawnPoint);
                SetMessage(player.GetName() + " respawned. " + BoolCoreState(team->coreAlive));
                AddWorldEffect(team->spawnPoint, GetTeamColor(team->color), 0.42f, 0.45f);
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

    const Team* team = FindTeam(player->GetTeamId());
    return team != nullptr && shop_.IsPlayerInShop(*player, *team);
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
    message_ = std::move(message);
    messageTimer_ = seconds;
}

void Game::AddEventMessage(std::string message, Color color, float seconds)
{
    eventMessages_.push_back(EventMessage { std::move(message), color, seconds, 0.0f });
    if (eventMessages_.size() > 5)
    {
        eventMessages_.erase(eventMessages_.begin());
    }
}

const char* Game::MatchModeName() const
{
    switch (selectedMode_)
    {
    case MatchMode::SoloVsBots:
        return "Solo vs bots";
    case MatchMode::TwoVsTwo:
        return "2 teams";
    case MatchMode::FourTeams:
        return "4 teams FFA";
    case MatchMode::Duel:
        return "Duel arena";
    }
    return "Unknown";
}

const char* Game::BotDifficultyName() const
{
    switch (botDifficulty_)
    {
    case BotDifficulty::Easy:
        return "Easy";
    case BotDifficulty::Normal:
        return "Normal";
    case BotDifficulty::Hard:
        return "Hard";
    }
    return "Unknown";
}

const char* Game::ArenaLayoutName() const
{
    switch (arenaLayout_)
    {
    case ArenaLayout::Classic:
        return "Classic";
    case ArenaLayout::Vertical:
        return "Vertical";
    }
    return "Unknown";
}

const char* Game::ArenaBiomeName() const
{
    switch (arenaBiome_)
    {
    case ArenaBiome::Arena:
        return "Arena";
    case ArenaBiome::Ice:
        return "Ice";
    case ArenaBiome::Lava:
        return "Lava";
    case ArenaBiome::Space:
        return "Space";
    case ArenaBiome::Ruins:
        return "Ruins";
    }
    return "Unknown";
}

const char* Game::ResolutionName() const
{
    const int index = std::clamp(resolutionIndex_, 0, static_cast<int>(std::size(kWindowResolutions)) - 1);
    return kWindowResolutions[index].label;
}

const char* Game::FpsLimitName() const
{
    const int index = std::clamp(fpsLimitIndex_, 0, static_cast<int>(std::size(kFpsLimits)) - 1);
    return kFpsLimits[index].label;
}

void Game::ApplyWindowSettings()
{
    const int index = std::clamp(resolutionIndex_, 0, static_cast<int>(std::size(kWindowResolutions)) - 1);
    const WindowResolution& resolution = kWindowResolutions[index];

    if (fullscreen_)
    {
        if (!IsWindowFullscreen())
        {
            const int monitor = GetCurrentMonitor();
            SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
            ToggleFullscreen();
        }
        return;
    }

    if (IsWindowFullscreen())
    {
        ToggleFullscreen();
    }
    SetWindowSize(resolution.width, resolution.height);
}

void Game::ApplyFrameRateLimit()
{
    const int index = std::clamp(fpsLimitIndex_, 0, static_cast<int>(std::size(kFpsLimits)) - 1);
    SetTargetFPS(kFpsLimits[index].fps);
}

void Game::LoadSettings()
{
    std::ifstream file("DaiBed.settings");
    if (!file)
    {
        return;
    }

    std::string key;
    while (file >> key)
    {
        if (key == "mouseSensitivity")
        {
            float value = input_.GetMouseSensitivity();
            file >> value;
            input_.SetMouseSensitivity(std::clamp(value, 0.3f, 2.5f));
        }
        else if (key == "fov")
        {
            file >> fov_;
            fov_ = std::clamp(fov_, 50.0f, 90.0f);
        }
        else if (key == "resolutionIndex")
        {
            file >> resolutionIndex_;
        }
        else if (key == "fpsLimitIndex")
        {
            file >> fpsLimitIndex_;
        }
        else if (key == "fullscreen")
        {
            file >> fullscreen_;
        }
        else if (key == "showHints")
        {
            file >> showControlHints_;
        }
        else if (key == "showMinimap")
        {
            file >> showMinimap_;
        }
        else if (key == "selectedMode")
        {
            int value = static_cast<int>(selectedMode_);
            file >> value;
            selectedMode_ = static_cast<MatchMode>(std::clamp(value, 0, 3));
        }
        else if (key == "selectedTeamId")
        {
            file >> selectedTeamId_;
        }
        else if (key == "selectedTeamSize")
        {
            file >> selectedTeamSize_;
        }
        else if (key == "selectedBotCount")
        {
            file >> selectedBotCount_;
        }
        else if (key == "botDifficulty")
        {
            int value = static_cast<int>(botDifficulty_);
            file >> value;
            botDifficulty_ = static_cast<BotDifficulty>(std::clamp(value, 0, 2));
        }
        else if (key == "arenaLayout")
        {
            int value = static_cast<int>(arenaLayout_);
            file >> value;
            arenaLayout_ = static_cast<ArenaLayout>(std::clamp(value, 0, 1));
        }
        else if (key == "arenaBiome")
        {
            int value = static_cast<int>(arenaBiome_);
            file >> value;
            arenaBiome_ = static_cast<ArenaBiome>(std::clamp(value, 0, 4));
        }
        else if (key == "automatchRunTarget")
        {
            file >> automatchRunTarget_;
        }
        else if (key == "automatchTicksPerFrame")
        {
            file >> automatchTicksPerFrame_;
        }
        else if (key == "automatchMaxMinutes")
        {
            file >> automatchMaxMinutes_;
        }
    }

    resolutionIndex_ = std::clamp(resolutionIndex_, 0, static_cast<int>(std::size(kWindowResolutions)) - 1);
    fpsLimitIndex_ = std::clamp(fpsLimitIndex_, 0, static_cast<int>(std::size(kFpsLimits)) - 1);
    selectedTeamSize_ = std::clamp(selectedTeamSize_, 1, 4);
    selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
    selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
    automatchRunTarget_ = std::clamp(automatchRunTarget_, 1, 50);
    automatchTicksPerFrame_ = std::clamp(automatchTicksPerFrame_, 1, 32);
    automatchMaxMinutes_ = std::clamp(automatchMaxMinutes_, 3, 30);
}

void Game::SaveSettings() const
{
    std::ofstream file("DaiBed.settings", std::ios::trunc);
    if (!file)
    {
        return;
    }

    file << "mouseSensitivity " << input_.GetMouseSensitivity() << "\n";
    file << "fov " << fov_ << "\n";
    file << "resolutionIndex " << resolutionIndex_ << "\n";
    file << "fpsLimitIndex " << fpsLimitIndex_ << "\n";
    file << "fullscreen " << fullscreen_ << "\n";
    file << "showHints " << showControlHints_ << "\n";
    file << "showMinimap " << showMinimap_ << "\n";
    file << "selectedMode " << static_cast<int>(selectedMode_) << "\n";
    file << "selectedTeamId " << selectedTeamId_ << "\n";
    file << "selectedTeamSize " << selectedTeamSize_ << "\n";
    file << "selectedBotCount " << selectedBotCount_ << "\n";
    file << "botDifficulty " << static_cast<int>(botDifficulty_) << "\n";
    file << "arenaLayout " << static_cast<int>(arenaLayout_) << "\n";
    file << "arenaBiome " << static_cast<int>(arenaBiome_) << "\n";
    file << "automatchRunTarget " << automatchRunTarget_ << "\n";
    file << "automatchTicksPerFrame " << automatchTicksPerFrame_ << "\n";
    file << "automatchMaxMinutes " << automatchMaxMinutes_ << "\n";
}

const char* Game::TeamName(int teamId) const
{
    switch (teamId)
    {
    case 0:
        return "Red";
    case 1:
        return "Blue";
    case 2:
        return "Green";
    case 3:
        return "Yellow";
    }
    return "Unknown";
}

Color Game::BiomeSkyColor() const
{
    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        return Color { 30, 42, 56, 255 };
    case ArenaBiome::Lava:
        return Color { 38, 19, 18, 255 };
    case ArenaBiome::Space:
        return Color { 5, 6, 18, 255 };
    case ArenaBiome::Ruins:
        return Color { 24, 27, 24, 255 };
    case ArenaBiome::Arena:
        break;
    }
    return Color { 112, 166, 238, 255 };
}

Color Game::BiomeFogColor() const
{
    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        return Color { 160, 218, 255, 255 };
    case ArenaBiome::Lava:
        return Color { 255, 88, 42, 255 };
    case ArenaBiome::Space:
        return Color { 38, 44, 100, 255 };
    case ArenaBiome::Ruins:
        return Color { 138, 148, 118, 255 };
    case ArenaBiome::Arena:
        break;
    }
    return Color { 80, 90, 108, 255 };
}

float Game::BiomeFogAlpha() const
{
    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        return 0.10f;
    case ArenaBiome::Lava:
        return 0.09f;
    case ArenaBiome::Space:
        return 0.05f;
    case ArenaBiome::Ruins:
        return 0.07f;
    case ArenaBiome::Arena:
        break;
    }
    return 0.0f;
}

const char* Game::KeyLabel(int key) const
{
    switch (key)
    {
    case KEY_SPACE:
        return "Space";
    case KEY_LEFT_SHIFT:
        return "LShift";
    case KEY_RIGHT_SHIFT:
        return "RShift";
    case KEY_LEFT_CONTROL:
        return "LCtrl";
    case KEY_RIGHT_CONTROL:
        return "RCtrl";
    case KEY_TAB:
        return "Tab";
    case KEY_ENTER:
        return "Enter";
    case KEY_ESCAPE:
        return "Esc";
    case KEY_F3:
        return "F3";
    case KEY_F5:
        return "F5";
    case KEY_NULL:
        return "-";
    case KEY_A:
        return "A";
    case KEY_B:
        return "B";
    case KEY_C:
        return "C";
    case KEY_D:
        return "D";
    case KEY_E:
        return "E";
    case KEY_F:
        return "F";
    case KEY_G:
        return "G";
    case KEY_H:
        return "H";
    case KEY_I:
        return "I";
    case KEY_J:
        return "J";
    case KEY_K:
        return "K";
    case KEY_L:
        return "L";
    case KEY_M:
        return "M";
    case KEY_N:
        return "N";
    case KEY_O:
        return "O";
    case KEY_P:
        return "P";
    case KEY_Q:
        return "Q";
    case KEY_R:
        return "R";
    case KEY_S:
        return "S";
    case KEY_T:
        return "T";
    case KEY_U:
        return "U";
    case KEY_V:
        return "V";
    case KEY_W:
        return "W";
    case KEY_X:
        return "X";
    case KEY_Y:
        return "Y";
    case KEY_Z:
        return "Z";
    case KEY_ZERO:
        return "0";
    case KEY_ONE:
        return "1";
    case KEY_TWO:
        return "2";
    case KEY_THREE:
        return "3";
    case KEY_FOUR:
        return "4";
    case KEY_FIVE:
        return "5";
    case KEY_SIX:
        return "6";
    case KEY_SEVEN:
        return "7";
    case KEY_EIGHT:
        return "8";
    case KEY_NINE:
        return "9";
    case KEY_UP:
        return "Up";
    case KEY_DOWN:
        return "Down";
    case KEY_LEFT:
        return "Left";
    case KEY_RIGHT:
        return "Right";
    default:
        break;
    }

    return "?";
}

void Game::AddWorldEffect(Vector3 position, Color color, float radius, float seconds)
{
    worldEffects_.push_back(WorldEffect { position, color, radius, seconds, 0.0f });
}

void Game::AddFloatingText(std::string text, Vector3 position, Color color)
{
    floatingTexts_.push_back(FloatingText { std::move(text), position, color, 0.8f, 0.0f });
}

void Game::AddKillFeed(std::string text, Color color, float seconds)
{
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

    if (event.attackerId >= 0)
    {
        if (!event.coreHit && event.targetId >= 0)
        {
            NoteDamageCredit(event.targetId, event.attackerId);
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
            cameraController_.AddShake(event.coreDestroyed ? 0.34f : 0.14f, event.coreDestroyed ? 0.34f : 0.16f);
        }
        else
        {
            audio_.PlayHit();
            cameraController_.AddShake(0.10f + std::min(0.12f, event.damage * 0.002f), 0.13f);
        }
    }

    if (event.targetId == localPlayerId_)
    {
        damageFlashTimer_ = 0.75f;
        cameraController_.AddShake(0.24f, 0.20f);
        audio_.PlayHit();
    }

    if (event.coreDestroyed)
    {
        cameraController_.AddShake(0.30f, 0.32f);
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

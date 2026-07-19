#include "Game.h"
#include "RangedCombat.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace
{
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

Vector3 Normalize2D(Vector3 value)
{
    const float length = Length2D(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, 0.0f, value.z / length };
}

Vector3 AimDirectionFromCommandInput(const PlayerCommand& command)
{
    const float cosPitch = std::cos(command.aimPitch);
    return Vector3 {
        std::sin(command.aimYaw) * cosPitch,
        std::sin(command.aimPitch),
        -std::cos(command.aimYaw) * cosPitch
    };
}
}

void Game::UseUtilityInputs(Player& player, const PlayerCommand& command)
{
    const PlayerControlKind controlKind = ControlKindForPlayer(player);
    ScopedLocalFeedbackSuppression suppressRemoteFeedback(*this, !HasLocalCamera(controlKind));
    // UI state only gates the player with the local camera; a network-controlled
    // player on the server is never blocked by the host's menus.
    if (HasLocalCamera(controlKind) && (shopOpen_ || inventoryOpen_))
    {
        return;
    }

    // Select a hotbar slot for whichever player owns this command: the locally
    // predicted human uses the UI mirror, remote humans carry their own slot.
    const auto selectSlot = [this, &player, controlKind](int slot)
    {
        if (IsLocallyPredicted(controlKind))
        {
            selectedHotbarSlot_ = slot;
        }
        else
        {
            player.SetSelectedSlot(slot);
        }
    };
    // Push always registers the owner-private replicated result (a no-op sink
    // for a local/singleplayer host with no client folding it back); Present
    // shows it immediately and is itself suppressed for a non-local-camera
    // player by the ScopedLocalFeedbackSuppression above. This is the same
    // push-always/present-if-unsuppressed split RegisterCombatEvent uses.
    const auto useHotbarUtility = [this, &player, &selectSlot, &command](UtilityType type)
    {
        const ItemType itemType = ItemFromUtility(type);
        const auto& hotbar = player.GetInventory().GetHotbarSlots();
        for (int i = 0; i < kHotbarSlotCount; ++i)
        {
            if (!hotbar[i].IsEmpty() && hotbar[i].type == itemType)
            {
                selectSlot(i);
                UtilityActionResult result {};
                if (type == UtilityType::Fireball || type == UtilityType::Molotov)
                {
                    result = ApplyProjectileUtility(player, type, AimDirectionFromCommandInput(command));
                }
                else
                {
                    result = ApplyUtility(player, type);
                }
                if (result.success)
                {
                    const int shopChoice = type == UtilityType::Heal ? 203
                        : (type == UtilityType::HomeTeleport ? 204
                        : (type == UtilityType::Dash ? 205
                        : (type == UtilityType::Fireball ? 105
                        : (type == UtilityType::Molotov ? 206 : 207))));
                    if (type == UtilityType::Fireball)
                    {
                        RecordAutomatchExplosiveUse(player, true);
                    }
                    else
                    {
                        RecordAutomatchShopUse(player, shopChoice);
                    }
                }
                PushUtilityActionResultSnapshot(player, result);
                PresentUtilityActionResult(result);
                return;
            }
        }

        UtilityActionResult deniedResult {};
        deniedResult.handled = true;
        deniedResult.type = type;
        deniedResult.message = std::string("Нет ") + ItemDisplayName(itemType) + " на панели.";
        deniedResult.playDeniedSound = true;
        PushUtilityActionResultSnapshot(player, deniedResult);
        PresentUtilityActionResult(deniedResult);
    };

    if (command.useHeal)
    {
        useHotbarUtility(UtilityType::Heal);
    }
    if (command.useTeleport)
    {
        useHotbarUtility(UtilityType::HomeTeleport);
    }
    if (command.useDash)
    {
        useHotbarUtility(UtilityType::Dash);
    }
    if (command.useShoot)
    {
        const auto& hotbar = player.GetInventory().GetHotbarSlots();
        const auto bow = std::find_if(hotbar.begin(), hotbar.end(), [](const ItemStack& stack)
        {
            return !stack.IsEmpty() && stack.type == ItemType::Bow;
        });
        if (bow != hotbar.end())
        {
            selectSlot(static_cast<int>(std::distance(hotbar.begin(), bow)));
            SetMessage("Лук выбран. Удерживайте ЛКМ для натяжения.");
        }
        else
        {
            SetMessage("Сначала купите лук.");
            audio_.PlayDenied();
        }
    }
    if (command.useFireball)
    {
        useHotbarUtility(UtilityType::Fireball);
    }
    if (command.useMolotov)
    {
        useHotbarUtility(UtilityType::Molotov);
    }
    if (command.useAlarm)
    {
        useHotbarUtility(UtilityType::AlarmTrap);
    }
}

Game::UtilityActionResult Game::ApplyUtility(Player& player, UtilityType type)
{
    UtilityActionResult result {};
    result.handled = true;
    result.type = type;
    if (type == UtilityType::Arrows)
    {
        result.message = "Стрелы используются луком: выберите лук и удерживайте ЛКМ.";
        return result;
    }
    if (type == UtilityType::Fireball || type == UtilityType::Molotov)
    {
        result.handled = false;
        return result;
    }

    if (type == UtilityType::Heal)
    {
        if (SpendUtilityItem(player, UtilityType::Heal))
        {
            player.Heal(45);
            result.success = true;
            result.message = "Аптечка использована.";
            result.position = player.GetPosition();
            result.color = Color { 128, 238, 166, 255 };
            result.radius = 0.36f;
            result.seconds = 0.35f;
            result.hasWorldEffect = true;
            result.playPickupSound = true;
            return result;
        }
        result.message = "Нет аптечек. Купите одну в утилитах.";
        result.playDeniedSound = true;
        return result;
    }

    if (type == UtilityType::HomeTeleport)
    {
        Team* team = FindTeam(player.GetTeamId());
        if (team != nullptr && SpendUtilityItem(player, UtilityType::HomeTeleport))
        {
            player.RespawnAtHome();
            result.success = true;
            result.message = "Телепорт домой выполнен.";
            result.position = player.GetHomeSpawnPoint();
            result.color = GetTeamColor(team->color);
            result.radius = 0.42f;
            result.seconds = 0.45f;
            result.hasWorldEffect = true;
            result.playPickupSound = true;
            return result;
        }
        if (team != nullptr)
        {
            result.message = "Нет телепортов домой. Купите один в утилитах.";
            result.playDeniedSound = true;
        }
        return result;
    }

    if (type == UtilityType::Dash)
    {
        if (SpendUtilityItem(player, UtilityType::Dash))
        {
            // Local player dashes along the camera's flat facing; a network
            // player dashes along its own (authoritative) yaw — the server has
            // no meaningful camera for it.
            const Vector3 forward = HasLocalCamera(ControlKindForPlayer(player))
                ? cameraController_.GetFlatForward()
                : player.Forward();
            player.ApplyKnockback(Vector3 { forward.x * 8.5f, 1.5f, forward.z * 8.5f });
            result.success = true;
            result.message = "Жемчуг рывка использован.";
            result.position = player.GetPosition();
            result.color = Color { 112, 232, 255, 255 };
            result.radius = 0.30f;
            result.seconds = 0.30f;
            result.hasWorldEffect = true;
            result.playPickupSound = true;
            return result;
        }
        result.message = "Нет жемчуга рывка. Купите один в утилитах.";
        result.playDeniedSound = true;
        return result;
    }

    if (type == UtilityType::AlarmTrap)
    {
        Team* team = FindTeam(player.GetTeamId());
        if (team != nullptr && SpendUtilityItem(player, UtilityType::AlarmTrap))
        {
            alarmTraps_.push_back(AlarmTrap { team->spawnPoint, player.GetTeamId(), 5.2f, false });
            result.success = true;
            result.message = "Сигнальная ловушка установлена на базе.";
            result.position = team->spawnPoint;
            result.color = Color { 255, 235, 142, 255 };
            result.radius = 0.32f;
            result.seconds = 0.35f;
            result.hasWorldEffect = true;
            result.playPickupSound = true;
            return result;
        }
        if (team != nullptr)
        {
            result.message = "Нет сигнальных ловушек. Купите одну в утилитах.";
            result.playDeniedSound = true;
        }
        return result;
    }

    result.handled = false;
    return result;
}

Game::UtilityActionResult Game::ApplyProjectileUtility(Player& player, UtilityType type, Vector3 direction)
{
    UtilityActionResult result {};
    result.handled = true;
    result.type = type;
    if (type != UtilityType::Fireball && type != UtilityType::Molotov)
    {
        result.handled = false;
        return result;
    }

    if (type == UtilityType::Fireball && !player.CanAttack())
    {
        result.message = "Оружие перезаряжается.";
        result.playDeniedSound = true;
        return result;
    }

    if (!SpendUtilityItem(player, type))
    {
        result.message = type == UtilityType::Fireball
            ? "Нет фаерболов. Купите один в бою."
            : "Нет коктейлей Молотова. Купите один в утилитах.";
        result.playDeniedSound = true;
        return result;
    }

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
    projectile.id = NextProjectileId();
    projectile.position = Vector3 {
        player.GetPosition().x + direction.x * 0.75f,
        player.GetPosition().y + 0.82f + direction.y * 0.75f,
        player.GetPosition().z + direction.z * 0.75f
    };
    projectile.ownerId = player.GetId();
    projectile.ownerTeamId = player.GetTeamId();
    const ProjectileTuning& tuning = type == UtilityType::Fireball ? kFireballTuning : kMolotovTuning;
    projectile.kind = type == UtilityType::Fireball ? ProjectileKind::Fireball : ProjectileKind::Molotov;
    projectile.fireZone = type == UtilityType::Molotov;
    projectile.velocity = Vector3 { direction.x * tuning.speed, direction.y * tuning.speed, direction.z * tuning.speed };
    if (type == UtilityType::Molotov)
    {
        projectile.velocity.y += 1.2f;
    }
    projectile.damage = tuning.damage;
    projectile.radius = tuning.radius;
    projectile.explosionRadius = tuning.explosionRadius;
    projectile.gravity = tuning.gravity;
    projectile.lifetime = tuning.lifetime;
    projectile.previousPosition = projectile.position;
    projectile.startPosition = projectile.position;
    if (type == UtilityType::Fireball)
    {
        player.ResetAttackCooldown(tuning.cooldown);
    }

    projectiles_.push_back(projectile);
    result.success = true;
    result.message = type == UtilityType::Fireball ? "Фаербол запущен." : "Коктейль Молотова брошен.";
    result.playBreakBlockSound = true;
    return result;
}

void Game::PresentUtilityActionResult(const UtilityActionResult& result)
{
    if (!result.handled || suppressLocalFeedback_)
    {
        return;
    }
    if (!result.message.empty())
    {
        SetMessage(result.message);
    }
    if (result.hasWorldEffect)
    {
        AddWorldEffect(result.position, result.color, result.radius, result.seconds);
    }
    if (result.playPickupSound)
    {
        audio_.PlayPickup();
    }
    if (result.playBreakBlockSound)
    {
        audio_.PlayBreakBlock();
    }
    if (result.playDeniedSound)
    {
        audio_.PlayDenied();
    }
}

bool Game::SpendUtilityItem(Player& player, UtilityType type)
{
    Inventory& inventory = player.GetInventory();
    // Prefer spending from a human owner's selected slot so the held stack
    // drains; fall back to spending the item by type anywhere.
    const PlayerControlKind controlKind = ControlKindForPlayer(player);
    if (IsHumanControlled(controlKind))
    {
        const int slot = IsLocallyPredicted(controlKind) ? selectedHotbarSlot_ : player.GetSelectedSlot();
        const ItemType selectedType = GetSelectedHotbarStack(player).type;
        if (ItemToUtility(selectedType) == type && inventory.SpendSlotItem(slot))
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
        SetMessage("Выбранный слот пуст.");
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
        // The use rides this tick's command (placePressed) into the
        // authoritative path (ApplyNetworkPlayerActions) — the same code a
        // real network client's right click goes through. Feedback comes back
        // via the utility action result presentation.
        return;
    }

    SetMessage(std::string(ItemDisplayName(stack.type)) + " выбрано.");
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
    const auto slotAtMouse = [this]() -> int
    {
        const Vector2 mouse = GetMousePosition();
        const int slotSize = 50;
        const int gap = 8;

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

        const int hotbarWidth = slotSize * kHotbarSlotCount + gap * (kHotbarSlotCount - 1);
        const int panelWidth = hotbarWidth + 42;
        const int panelHeight = 322;
        const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
        const int panelY = GetScreenHeight() / 2 - panelHeight / 2;
        const int gridX = panelX + 21;
        const int gridY = panelY + 58;
        const int inventoryHotbarY = gridY + 3 * (slotSize + gap) + 14;
        const int mainSlot = hitGrid(gridX, gridY, 9, 3, kHotbarSlotCount);
        if (mainSlot >= 0)
        {
            return mainSlot;
        }
        return hitGrid(gridX, inventoryHotbarY, 9, 1, 0);
    };
    const auto chestSlotAtMouse = []() -> int
    {
        const Vector2 mouse = GetMousePosition();
        const int slotSize = 34;
        const int gap = 5;
        const int cols = 6;
        const int rows = 6;
        const int chestPanelWidth = 250;
        const int chestPanelHeight = 322;
        const int inventorySlotSize = 50;
        const int inventoryGap = 8;
        const int inventoryHotbarWidth = inventorySlotSize * kHotbarSlotCount + inventoryGap * (kHotbarSlotCount - 1);
        const int inventoryPanelWidth = inventoryHotbarWidth + 42;
        const int inventoryPanelX = GetScreenWidth() / 2 - inventoryPanelWidth / 2;
        const int inventoryPanelY = GetScreenHeight() / 2 - chestPanelHeight / 2;
        const int preferredX = inventoryPanelX + inventoryPanelWidth + 18;
        const int panelX = std::min(preferredX, GetScreenWidth() - chestPanelWidth - 16);
        const int panelY = inventoryPanelY;
        const int gridX = panelX + 14;
        const int gridY = panelY + 52;

        for (int row = 0; row < rows; ++row)
        {
            for (int col = 0; col < cols; ++col)
            {
                const Rectangle bounds {
                    static_cast<float>(gridX + col * (slotSize + gap)),
                    static_cast<float>(gridY + row * (slotSize + gap)),
                    static_cast<float>(slotSize),
                    static_cast<float>(slotSize)
                };
                if (CheckCollisionPointRec(mouse, bounds))
                {
                    return row * cols + col;
                }
            }
        }
        return -1;
    };

    int row = 0;
    int col = 0;
    slotToVisual(inventoryCursorSlot_, row, col);
    const int hoveredSlot = slotAtMouse();
    const int hoveredChestSlot = (teamChestOpen_ || personalChestOpen_) ? chestSlotAtMouse() : -1;
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

    const auto clearHeldStack = [this]()
    {
        heldInventoryStack_ = ItemStack {};
        heldInventoryOrigin_ = HeldInventoryOrigin::None;
        heldInventoryOriginSlot_ = -1;
    };
    const auto holdServerStack = [this](HeldInventoryOrigin origin, int slot, ItemStack stack, int amount)
    {
        if (amount > 0 && amount < stack.count)
        {
            stack.count = amount;
        }
        heldInventoryStack_ = stack;
        heldInventoryOrigin_ = origin;
        heldInventoryOriginSlot_ = slot;
    };
    const auto queueHeldToPlayerSlot = [this, &player, &clearHeldStack](int targetSlot)
    {
        if (heldInventoryOrigin_ == HeldInventoryOrigin::PlayerInventory)
        {
            QueuePlayerAction(
                PlayerActionType::MoveInventory,
                heldInventoryOriginSlot_,
                PackPlayerActionParam(
                    static_cast<int>(InventoryMoveOp::SlotToSlot),
                    targetSlot,
                    heldInventoryStack_.count));
        }
        else if (heldInventoryOrigin_ == HeldInventoryOrigin::TeamChest)
        {
            QueuePlayerAction(
                PlayerActionType::ChestTransfer,
                heldInventoryOriginSlot_,
                PackPlayerActionParam(
                    static_cast<int>(ChestTransferOp::ChestToPlayerSlot),
                    targetSlot,
                    heldInventoryStack_.count));
        }
        else
        {
            return false;
        }

        ApplyPendingLocalPlayerAction(player);
        clearHeldStack();
        return true;
    };
    const auto queueHeldToTeamChestSlot = [this, &player, &clearHeldStack](int targetSlot)
    {
        if (heldInventoryOrigin_ == HeldInventoryOrigin::PlayerInventory)
        {
            QueuePlayerAction(
                PlayerActionType::ChestTransfer,
                heldInventoryOriginSlot_,
                PackPlayerActionParam(
                    static_cast<int>(ChestTransferOp::PlayerToChestSlot),
                    targetSlot,
                    heldInventoryStack_.count));
        }
        else if (heldInventoryOrigin_ == HeldInventoryOrigin::TeamChest)
        {
            QueuePlayerAction(
                PlayerActionType::ChestTransfer,
                heldInventoryOriginSlot_,
                PackPlayerActionParam(
                    static_cast<int>(ChestTransferOp::ChestToChestSlot),
                    targetSlot,
                    heldInventoryStack_.count));
        }
        else
        {
            return false;
        }

        ApplyPendingLocalPlayerAction(player);
        clearHeldStack();
        return true;
    };

    if (IsKeyPressed(KEY_C))
    {
        OpenTeamChestUi(player);
    }
    if ((teamChestOpen_ || personalChestOpen_) && IsKeyPressed(KEY_X))
    {
        if (teamChestOpen_)
        {
            if (!heldInventoryStack_.IsEmpty())
            {
                SetMessage("Сначала верните предмет в инвентарь.", 1.1f);
                audio_.PlayDenied();
                return;
            }

            const bool depositSelected = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            QueuePlayerAction(
                PlayerActionType::ChestTransfer,
                depositSelected ? inventoryCursorSlot_ : 0,
                depositSelected ? 0 : 1);
            ApplyPendingLocalPlayerAction(player);
            return;
        }

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
                SetMessage("Предметы сложены в сундук.", 1.1f);
            }
            else
            {
                for (int i = 0; i < kInventorySlotCount; ++i)
                {
                    heldInventoryStack_ = chest->TakeSlot(i);
                    heldInventoryOrigin_ = HeldInventoryOrigin::None;
                    heldInventoryOriginSlot_ = -1;
                    if (!heldInventoryStack_.IsEmpty())
                    {
                        SetMessage("Стак взят из сундука.", 1.1f);
                        break;
                    }
                }
            }
            audio_.PlayPickup();
        }
        return;
    }
    if ((teamChestOpen_ || personalChestOpen_) && hoveredChestSlot >= 0)
    {
        const bool leftClick = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        const bool rightClick = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
        const bool activate = leftClick || rightClick;
        if (activate && teamChestOpen_)
        {
            const int teamId = std::clamp(player.GetTeamId(), 0, static_cast<int>(teamChests_.size()) - 1);
            Inventory& chest = teamChests_[teamId];
            if (heldInventoryStack_.IsEmpty())
            {
                const ItemStack stack = chest.GetSlot(hoveredChestSlot);
                if (stack.IsEmpty())
                {
                    return;
                }
                if (rightClick && stack.count <= 1)
                {
                    return;
                }
                holdServerStack(
                    HeldInventoryOrigin::TeamChest,
                    hoveredChestSlot,
                    stack,
                    rightClick ? stack.count / 2 : stack.count);
                SetMessage("Взято из командного сундука.", 1.0f);
                audio_.PlayPickup();
                return;
            }

            if (heldInventoryOrigin_ == HeldInventoryOrigin::TeamChest
                && heldInventoryOriginSlot_ == hoveredChestSlot)
            {
                clearHeldStack();
                return;
            }

            if (!queueHeldToTeamChestSlot(hoveredChestSlot))
            {
                clearHeldStack();
            }
            return;
        }

        if (activate && personalChestOpen_)
        {
            Inventory& chest = personalChest_;
            if (rightClick && heldInventoryStack_.IsEmpty())
            {
                const ItemStack stack = chest.GetSlot(hoveredChestSlot);
                if (!stack.IsEmpty() && stack.count > 1)
                {
                    const int taken = stack.count / 2;
                    heldInventoryStack_ = ItemStack { stack.type, taken };
                    heldInventoryOrigin_ = HeldInventoryOrigin::None;
                    heldInventoryOriginSlot_ = -1;
                    ItemStack original = chest.TakeSlot(hoveredChestSlot);
                    original.count -= taken;
                    chest.PlaceStack(hoveredChestSlot, original);
                    SetMessage("Стак в сундуке разделен.", 1.0f);
                    audio_.PlayPickup();
                }
                return;
            }

            if (heldInventoryStack_.IsEmpty())
            {
                heldInventoryStack_ = chest.TakeSlot(hoveredChestSlot);
                heldInventoryOrigin_ = HeldInventoryOrigin::None;
                heldInventoryOriginSlot_ = -1;
                if (!heldInventoryStack_.IsEmpty())
                {
                    SetMessage("Взято из сундука.", 1.0f);
                    audio_.PlayPickup();
                }
                return;
            }

            if (chest.PlaceStack(hoveredChestSlot, heldInventoryStack_))
            {
                SetMessage("Сложено в сундук.", 1.0f);
                audio_.PlayPickup();
                return;
            }

            heldInventoryStack_ = chest.SwapSlot(hoveredChestSlot, heldInventoryStack_);
            heldInventoryOrigin_ = HeldInventoryOrigin::None;
            heldInventoryOriginSlot_ = -1;
            SetMessage(heldInventoryStack_.IsEmpty() ? "Сложено в сундук." : "Стаки поменяны местами.", 1.0f);
            audio_.PlayPickup();
            return;
        }
    }
    const bool shiftDownForInventory = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const bool playerSlotKeyActivate = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
    if (teamChestOpen_ && !shiftDownForInventory && (hoveredSlot >= 0 || playerSlotKeyActivate))
    {
        const bool leftClick = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        const bool rightClick = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
        const bool activate = leftClick || rightClick || playerSlotKeyActivate;
        if (activate)
        {
            const int targetSlot = hoveredSlot >= 0 ? hoveredSlot : inventoryCursorSlot_;
            Inventory& inventory = player.GetInventory();
            if (heldInventoryStack_.IsEmpty())
            {
                const ItemStack stack = inventory.GetSlot(targetSlot);
                if (stack.IsEmpty())
                {
                    return;
                }
                if (rightClick && stack.count <= 1)
                {
                    return;
                }
                holdServerStack(
                    HeldInventoryOrigin::PlayerInventory,
                    targetSlot,
                    stack,
                    rightClick ? stack.count / 2 : stack.count);
                SetMessage(std::string("Взято: ") + ItemDisplayName(heldInventoryStack_.type) + ".", 1.0f);
                audio_.PlayPickup();
                return;
            }

            if (heldInventoryOrigin_ == HeldInventoryOrigin::PlayerInventory
                && heldInventoryOriginSlot_ == targetSlot)
            {
                clearHeldStack();
                return;
            }

            if (!queueHeldToPlayerSlot(targetSlot))
            {
                clearHeldStack();
            }
            return;
        }
    }
    if (IsKeyPressed(input_.GetBindings().drop) && heldInventoryStack_.IsEmpty() && inventoryCursorSlot_ >= 0)
    {
        const ItemStack stack = player.GetInventory().GetSlot(inventoryCursorSlot_);
        if (!stack.IsEmpty())
        {
            QueuePlayerAction(PlayerActionType::DropItem, inventoryCursorSlot_, stack.count);
            ApplyPendingLocalPlayerAction(player);
        }
        return;
    }
    if ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))
        && heldInventoryStack_.IsEmpty()
        && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)))
    {
        if (teamChestOpen_)
        {
            QueuePlayerAction(PlayerActionType::ChestTransfer, inventoryCursorSlot_, 0);
        }
        else if (personalChestOpen_)
        {
            Inventory& inventory = player.GetInventory();
            ItemStack moving = inventory.TakeSlot(inventoryCursorSlot_);
            for (int i = 0; i < kInventorySlotCount && !moving.IsEmpty(); ++i)
            {
                personalChest_.PlaceStack(i, moving);
            }
            if (!moving.IsEmpty())
            {
                inventory.PlaceStack(inventoryCursorSlot_, moving);
            }
            if (moving.IsEmpty())
            {
                SetMessage("Сложено в сундук.", 1.0f);
                audio_.PlayPickup();
            }
            else
            {
                SetMessage("Сундук заполнен.", 1.0f);
                audio_.PlayDenied();
            }
            return;
        }
        else
        {
            QueuePlayerAction(PlayerActionType::MoveInventory, inventoryCursorSlot_, 0);
        }
        ApplyPendingLocalPlayerAction(player);
        return;
    }

    // Stage 4.4: player-inventory drags are server-style for EVERY local human
    // — the held stack is a REFERENCE to the origin slot (nothing mutates on
    // pick-up), and the placement queues a MoveInventory action through the
    // ONE validated pipeline (MoveInventoryStackToSlot handles move / merge /
    // full-stack swap / partial amounts). In SP the out-of-band apply lands
    // the same frame; in MP it ships with the next command.
    if (hoveredSlot >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && heldInventoryStack_.IsEmpty())
    {
        Inventory& inventory = player.GetInventory();
        const ItemStack stack = inventory.GetSlot(hoveredSlot);
        if (!stack.IsEmpty() && stack.count > 1)
        {
            holdServerStack(HeldInventoryOrigin::PlayerInventory, hoveredSlot, stack, stack.count / 2);
            SetMessage("Стак разделен.", 1.0f);
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
        const ItemStack stack = inventory.GetSlot(inventoryCursorSlot_);
        holdServerStack(HeldInventoryOrigin::PlayerInventory, inventoryCursorSlot_, stack, stack.count);
        if (!heldInventoryStack_.IsEmpty())
        {
            SetMessage(std::string("Взято: ") + ItemDisplayName(heldInventoryStack_.type) + ".", 1.2f);
            audio_.PlayPickup();
        }
        return;
    }

    if (!queueHeldToPlayerSlot(inventoryCursorSlot_))
    {
        clearHeldStack();
    }
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
    matchSimulation_.DroppedItems().push_back(DroppedItem {
        ItemStack { stack.type, droppedCount },
        Vec3 {
            player.GetPosition().x + forward.x * 0.85f,
            player.GetPosition().y + 0.35f,
            player.GetPosition().z + forward.z * 0.85f },
        Vec3 { forward.x * 2.2f, 2.0f, forward.z * 2.2f },
        player.GetId(),
        0.85f,
        45.0f,
        0.0f,
        false,
        NextDroppedItemId() });
    SetMessage(std::string("Выброшено: ") + ItemDisplayName(stack.type) + ".", 1.2f);
    AddFloatingText("выброс", player.GetPosition(), Fade(WHITE, 0.85f));
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
    SetMessage("Стак быстро перемещен.", 1.0f);
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

    inventory.AddItem(player.GetHeroId() == HeroId::Svidetel ? ItemType::SniperRifle : ItemType::Sword, 1);
    if (inventory.GetToolLevel() > 0)
    {
        inventory.AddItem(ItemType::Pickaxe, 1);
    }
    if (IsLocallyPredicted(ControlKindForPlayer(player)))
    {
        selectedHotbarSlot_ = 0;
    }
}

void Game::NoteDamageCredit(int targetId, int attackerId, std::string cause)
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
            credit.cause = std::move(cause);
            return;
        }
    }

    damageCredits_.push_back(DamageCredit { targetId, attackerId, 8.0f, std::move(cause) });
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

bool Game::OpenTeamChestUi(Player& player)
{
    // THE chest-UI opener for every local human (Stage 4.3 merged the old
    // TryOpenBaseChest/TryOpenNetworkTeamChest split). Same proximity rule the
    // server-side ChestTransfer validation applies (shop 12 / chest 16 sq).
    if (!IsPlayerNearTeamChestAccess(player))
    {
        SetMessage("Вернитесь на базу, чтобы открыть сундуки.", 1.4f);
        audio_.PlayDenied();
        return false;
    }

    if (networkMode_ == NetworkMode::LocalClient)
    {
        // The personal chest is a local-only stash (not replicated yet), so a
        // network client only ever opens the TEAM chest — no chest cycling.
        inventoryOpen_ = true;
        teamChestOpen_ = true;
        personalChestOpen_ = false;
        inventoryCursorSlot_ = selectedHotbarSlot_;
        heldInventoryStack_ = ItemStack {};
        heldInventoryOrigin_ = HeldInventoryOrigin::None;
        heldInventoryOriginSlot_ = -1;
        shopOpen_ = false;
        if (!headless_)
        {
            EnableCursor();
        }
        SetMessage("Командный сундук открыт.", 1.2f);
        audio_.PlayPickup();
        return true;
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
        SetMessage("Личный сундук открыт.", 1.2f);
    }
    else if (personalChestOpen_)
    {
        CloseChest();
        SetMessage("Сундук закрыт.", 1.0f);
    }
    else
    {
        teamChestOpen_ = true;
        personalChestOpen_ = false;
        SetMessage("Командный сундук открыт.", 1.2f);
    }
    audio_.PlayPickup();
    return true;
}

bool Game::TryOpenAimedTeamChest(Player& player)
{
    // Aimed-chest entry shared by the SP frame path and the MP client session:
    // a team chest under the crosshair either opens (own team) or denies
    // (enemy team). Returns true when a team chest was aimed at — the caller
    // consumes the input either way.
    const std::optional<RaycastHit> chestHit = RaycastFromAim(player, 4.5f);
    if (!chestHit.has_value() || chestHit->blockData.type != BlockType::TeamChestBlock)
    {
        return false;
    }
    if (chestHit->blockData.teamId == player.GetTeamId())
    {
        OpenTeamChestUi(player);
    }
    else
    {
        SetMessage("Это сундук другой команды.", 1.2f);
        audio_.PlayDenied();
    }
    return true;
}

void Game::CloseChest()
{
    teamChestOpen_ = false;
    personalChestOpen_ = false;
}

Vector3 Game::TeamChestDepositPosition(const Team& team) const
{
    const Vector3 chestCenter = world_.GridToWorld(team.teamChestBlock);
    return Vector3 { chestCenter.x, chestCenter.y + 0.68f, chestCenter.z };
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
        RecordAutomatchShopPurchase(player, choice);
    }

    message = bought > 1 ? ("Куплено x" + std::to_string(bought) + ". " + lastMessage) : lastMessage;
    if (bought > 0 && player.GetHeroId() == HeroId::Brom)
    {
    player.AddHeroUltimateCharge(static_cast<float>(bought) * 4.0f);
    }
    return bought > 0;
}

bool Game::LaunchProjectile(Player& player, UtilityType type, Vector3 direction, bool announce)
{
    if ((type == UtilityType::Arrows || type == UtilityType::Fireball) && !player.CanAttack())
    {
        if (announce)
        {
            SetMessage("Оружие перезаряжается.");
            audio_.PlayDenied();
        }
        return false;
    }
    if (!SpendUtilityItem(player, type))
    {
        if (announce)
        {
            if (type == UtilityType::Arrows)
            {
                SetMessage("Нет энергострел. Купите их в бою.");
            }
            else if (type == UtilityType::Fireball)
            {
                SetMessage("Нет фаерболов. Купите один в бою.");
            }
            else
            {
                SetMessage("Нет коктейлей Молотова. Купите один в утилитах.");
            }
            audio_.PlayDenied();
        }
        return false;
    }

    LaunchProjectileDirected(player, type, direction, announce);
    return true;
}

void Game::LaunchProjectile(Player& player, UtilityType type)
{
    if ((type == UtilityType::Arrows || type == UtilityType::Fireball) && !player.CanAttack())
    {
        SetMessage("Оружие перезаряжается.");
        audio_.PlayDenied();
        return;
    }
    if (!SpendUtilityItem(player, type))
    {
        if (type == UtilityType::Arrows)
        {
            SetMessage("Нет энергострел. Купите их в бою.");
        }
        else if (type == UtilityType::Fireball)
        {
            SetMessage("Нет фаерболов. Купите один в бою.");
        }
        else
        {
            SetMessage("Нет коктейлей Молотова. Купите один в утилитах.");
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
    projectile.id = NextProjectileId();
    projectile.position = Vector3 {
        player.GetPosition().x + direction.x * 0.75f,
        player.GetPosition().y + 0.82f + direction.y * 0.75f,
        player.GetPosition().z + direction.z * 0.75f
    };
    projectile.ownerId = player.GetId();
    projectile.ownerTeamId = player.GetTeamId();
    const ProjectileTuning* tuning = &kArrowTuning;

    if (type == UtilityType::Fireball)
    {
        tuning = &kFireballTuning;
        projectile.kind = ProjectileKind::Fireball;
    }
    else if (type == UtilityType::Molotov)
    {
        tuning = &kMolotovTuning;
        projectile.kind = ProjectileKind::Molotov;
        projectile.fireZone = true;
    }

    projectile.velocity = Vector3 { direction.x * tuning->speed, direction.y * tuning->speed, direction.z * tuning->speed };
    if (type == UtilityType::Molotov)
    {
        projectile.velocity.y += 1.2f;
    }
    projectile.damage = tuning->damage;
    projectile.radius = tuning->radius;
    projectile.explosionRadius = tuning->explosionRadius;
    projectile.gravity = tuning->gravity;
    projectile.lifetime = tuning->lifetime;
    projectile.previousPosition = projectile.position;
    projectile.startPosition = projectile.position;
    if (type == UtilityType::Arrows || type == UtilityType::Fireball)
    {
        player.ResetAttackCooldown(tuning->cooldown);
    }

    projectiles_.push_back(projectile);
    if (type == UtilityType::Arrows)
    {
        RecordAutomatchShopUse(player, 104);
    }
    else if (type == UtilityType::Fireball)
    {
        RecordAutomatchExplosiveUse(player, true);
    }
    if (announce)
    {
        SetMessage(type == UtilityType::Arrows ? "Энергострела выпущена." : (type == UtilityType::Fireball ? "Фаербол запущен." : "Коктейль Молотова брошен."));
        audio_.PlayBreakBlock();
    }
}

void Game::DetonateAt(Vector3 position, int ownerTeamId, int ownerPlayerId, float radius, int damage,
                      bool createFireZone, bool blueFire, ExplosionBlockPolicy blockPolicy)
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
                    const bool reinforced = block->type == BlockType::ObsidianBlock
                        || block->type == BlockType::EnergyGlassBlock;
                    const bool fortified = reinforced
                        || block->type == BlockType::StoneBlock
                        || block->type == BlockType::StickyBlock;
                    if ((blockPolicy == ExplosionBlockPolicy::PreserveReinforced && reinforced)
                        || (blockPolicy == ExplosionBlockPolicy::PreserveFortified && fortified))
                    {
                        continue;
                    }
                    if (createFireZone)
                    {
                        if (block->type == BlockType::ObsidianBlock
                            || block->type == BlockType::EnergyGlassBlock
                            || block->type == BlockType::EnergyCoreBlock)
                        {
                            continue;
                        }
                        const bool alreadyBurning = std::any_of(
                            molotovBlockBurns_.begin(), molotovBlockBurns_.end(),
                            [pos](const MolotovBlockBurn& burn) { return burn.position == pos; });
                        if (!alreadyBurning)
                        {
                            const float burnSeconds = std::clamp(BreakSeconds(block->type, 0) * 2.1f, 0.9f, 4.5f);
                            molotovBlockBurns_.push_back(MolotovBlockBurn { pos, block->type, ownerTeamId, burnSeconds });
                        }
                    }
                    else
                    {
                        const bool enemyDefense = block->teamId >= 0 && block->teamId != ownerTeamId;
                        if (BreakWorldBlock(pos, ownerTeamId, BlockDeltaReason::Explosion, ownerPlayerId)
                            && enemyDefense)
                        {
                            RecordAutomatchDefenseBlockDestroyed(ownerPlayerId, blockPolicy);
                        }
                    }
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
        // Spawn protection covers the whole explosion outcome: Damage already
        // ignores it, and the impulse must not throw an invulnerable player into
        // the void either.
        if (player.IsInvulnerable())
        {
            continue;
        }
        const int scaledDamage = std::max(6, static_cast<int>(static_cast<float>(damage) * (1.0f - std::min(distance / (radius + 0.7f), 0.82f))));
        NoteDamageCredit(player.GetId(), ownerPlayerId, "взрывом");
        player.Damage(scaledDamage);
        const bool explosionKilledTarget = player.GetHealth() <= 0;
        const Vector3 away = Normalize2D(Vector3 { player.GetPosition().x - position.x, 0.0f, player.GetPosition().z - position.z });
        const float knockback = BiomeKnockbackMultiplier();
        player.ApplyKnockback(Vector3 { away.x * 5.4f * knockback, 2.4f * knockback, away.z * 5.4f * knockback });
        // Owner-private replicated feedback per AoE victim (push-only, mirrors the
        // direct-hit push in UpdateProjectiles). ownerPlayerId can be -1 for a
        // TNT block with no recorded placer — GetPlayer(-1) is nullptr, so this
        // silently skips the push rather than crashing or inventing an owner.
        if (const Player* owner = matchSimulation_.GetPlayer(ownerPlayerId))
        {
            CombatPresentationEvent presentation {};
            presentation.valid = true;
            presentation.event.attackerId = ownerPlayerId;
            presentation.event.targetId = player.GetId();
            presentation.event.targetTeamId = player.GetTeamId();
            presentation.event.position = player.GetPosition();
            presentation.event.damage = scaledDamage;
            presentation.event.killed = explosionKilledTarget;
            presentation.message = owner->GetName() + ": взрыв задел " + player.GetName()
                + ", урон " + std::to_string(scaledDamage) + ".";
            PushCombatEventSnapshots(presentation);
        }
    }

    if (createFireZone)
    {
        hazardZones_.push_back(HazardZone { position, ownerTeamId, ownerPlayerId, 2.4f, blueFire ? 4.0f : 5.0f, 0.0f, blueFire ? 16 : 8, blueFire, NextHazardZoneId() });
    }

    AddWorldEffect(
        position,
        Vector3 { 0.0f, 0.0f, 1.0f },
        createFireZone ? (blueFire ? Color { 92, 164, 255, 255 } : Color { 255, 118, 70, 255 }) : Color { 255, 224, 122, 255 },
        createFireZone ? radius : radius * 0.24f,
        0.55f,
        createFireZone ? WorldEffectKind::FireZone : WorldEffectKind::Burst);
    AddCameraShake(0.18f + radius * 0.04f, 0.24f);
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
                AddEventMessage((owner != nullptr ? owner->name : "База") + std::string(": сработала тревога!"), Color { 255, 235, 142, 255 }, 3.0f);
                AddFloatingText("ТРЕВОГА", player.GetPosition(), Color { 255, 235, 142, 255 });
                AddWorldEffect(player.GetPosition(), Color { 255, 235, 142, 255 }, 0.42f, 0.45f);
                audio_.PlayDenied();
                PushWorldEventSnapshot(
                    WorldEventKind::AlarmTriggered,
                    -1,
                    player.GetId(),
                    trap.ownerTeamId,
                    player.GetPosition());
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

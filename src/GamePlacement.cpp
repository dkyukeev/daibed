#include "Game.h"

#include "VisualTheme.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>

namespace
{
constexpr int kBuildMinY = -2;
constexpr int kBuildMaxY = 64;
constexpr int kCreativeBuildMinY = -8;
constexpr int kCreativeBuildMaxY = 96;
constexpr float kBuildReach = 4.5f;
constexpr float kCreativeBuildReach = 12.0f;
constexpr float kBuildDistanceSq = 38.0f;
constexpr float kCreativeBuildDistanceSq = 14.5f * 14.5f;

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

std::string FormatTenths(float value)
{
    const int tenths = static_cast<int>(value * 10.0f + 0.5f);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

// Support-driven orientation for torches and ladders.  When placement came
// from a raycast, supportBlock is the block whose face was clicked: it is the
// only reliable source of the intended wall.  Player position remains a
// deterministic fallback for bridge/bot placement, which has no clicked face.
int OrientationVariantFor(BlockType type, const World& world, const GridPos& pos, Vector3 playerPosition,
                          std::optional<GridPos> supportBlock)
{
    if (type != BlockType::LadderBlock && type != BlockType::TorchBlock)
    {
        return -2;
    }
    if (type == BlockType::TorchBlock && world.IsSolid(GridPos { pos.x, pos.y - 1, pos.z }))
    {
        // Solid ground below: a standing torch (legacy Minecraft data 5).
        return 5;
    }

    struct WallOption
    {
        GridPos offset;
        int ladderVariant; // renderer plane side (2/3 = -Z/+Z, 4/5 = -X/+X)
        int torchVariant;  // legacy Minecraft wall-torch data (leans away from wall)
    };
    constexpr WallOption kWalls[] {
        { GridPos { 0, 0, -1 }, 2, 3 },
        { GridPos { 0, 0, 1 }, 3, 4 },
        { GridPos { -1, 0, 0 }, 4, 1 },
        { GridPos { 1, 0, 0 }, 5, 2 },
    };

    const auto variantForWall = [&pos, &kWalls](const GridPos& wall)
    {
        for (const WallOption& option : kWalls)
        {
            if (wall.x == pos.x + option.offset.x && wall.y == pos.y + option.offset.y
                && wall.z == pos.z + option.offset.z)
            {
                return option;
            }
        }
        return WallOption { GridPos {}, -1, -1 };
    };

    if (supportBlock.has_value())
    {
        const Block* support = world.GetBlock(*supportBlock);
        const WallOption clickedWall = variantForWall(*supportBlock);
        if (clickedWall.ladderVariant >= 0)
        {
            return support != nullptr && world.IsSolid(*supportBlock)
                ? (type == BlockType::LadderBlock ? clickedWall.ladderVariant : clickedWall.torchVariant)
                : -1;
        }

        // Stacking a ladder by clicking the top/bottom face of the previous
        // section must preserve that section's wall orientation.  This keeps a
        // vertical run attached to one wall even though its clicked face is not
        // itself horizontal.
        if (type == BlockType::LadderBlock && support != nullptr && support->type == BlockType::LadderBlock
            && supportBlock->x == pos.x && supportBlock->z == pos.z
            && std::abs(supportBlock->y - pos.y) == 1)
        {
            const int inheritedVariant = support->variant & 0x07;
            const GridPos inheritedWall = inheritedVariant == 2 ? GridPos { pos.x, pos.y, pos.z - 1 }
                : inheritedVariant == 3 ? GridPos { pos.x, pos.y, pos.z + 1 }
                : inheritedVariant == 4 ? GridPos { pos.x - 1, pos.y, pos.z }
                : GridPos { pos.x + 1, pos.y, pos.z };
            return world.IsSolid(inheritedWall) ? inheritedVariant : -1;
        }
    }

    const float pushX = static_cast<float>(pos.x) - playerPosition.x;
    const float pushZ = static_cast<float>(pos.z) - playerPosition.z;
    int bestVariant = -1;
    float bestAlignment = -1.0e9f;
    for (const WallOption& wall : kWalls)
    {
        if (!world.IsSolid(GridPos { pos.x + wall.offset.x, pos.y, pos.z + wall.offset.z }))
        {
            continue;
        }
        const float alignment = pushX * static_cast<float>(wall.offset.x) + pushZ * static_cast<float>(wall.offset.z);
        if (alignment > bestAlignment)
        {
            bestAlignment = alignment;
            bestVariant = type == BlockType::LadderBlock ? wall.ladderVariant : wall.torchVariant;
        }
    }
    return bestVariant;
}

const char* SupportDeniedMessage(BlockType type)
{
    return type == BlockType::LadderBlock
        ? "Лестнице нужна стена рядом."
        : "Факелу нужен пол или стена.";
}
}

void Game::UpdatePlacementPreview()
{
    const Player* player = GetLocalPlayer();
    if (player == nullptr || !player->IsAlive() || matchSimulation_.HasWinner() || shopOpen_ || inventoryOpen_)
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
    if (player == nullptr || !player->IsAlive() || shopOpen_ || inventoryOpen_ || matchSimulation_.HasWinner())
    {
        return;
    }

    const Inventory& inventory = player->GetInventory();
    if (GetSelectedHotbarStack(*player).type == ItemType::Bow)
    {
        const float drawPower = BowDrawPower(player->GetBowDrawTimer());
        combatPreview_.visible = true;
        combatPreview_.ready = drawPower >= kBowTuning.minimumDrawPower;
        combatPreview_.cooldownFraction = drawPower;
        combatPreview_.damage = static_cast<int>(std::ceil(
            drawPower * 3.0f * kBowTuning.baseArrowDamage
            * (1.0f + kBowTuning.powerDamageBonusPerLevel
                * BowPowerLevelForUpgrade(inventory.GetBowUpgradeLevel()))
            * (drawPower >= 0.999f ? kBowTuning.criticalMultiplier : 1.0f)));
        const ArrowVariant variant = player->GetArrowVariant();
        if (variant == ArrowVariant::Impulse)
        {
            combatPreview_.damage = static_cast<int>(std::ceil(
                static_cast<float>(combatPreview_.damage) * 0.75f));
        }
        const float reload = player->GetArrowReloadTimer(variant);
        combatPreview_.ready = combatPreview_.ready && reload <= 0.0f;
        combatPreview_.label = std::string(ArrowVariantName(variant)) + " · "
            + std::to_string(player->GetArrowAmmo(variant)) + "/"
            + std::to_string(ArrowQuiverCapacity(variant))
            + (reload > 0.0f
                ? " · перезарядка " + FormatTenths(reload) + " с"
                : " · натяжение " + std::to_string(static_cast<int>(std::round(drawPower * 100.0f)))
                    + "% · урон ≈ " + std::to_string(combatPreview_.damage));
        return;
    }
    const ItemType selectedRangedItem = GetSelectedHotbarStack(*player).type;
    if (ItemIsBlasterWeapon(selectedRangedItem))
    {
        const float fullCharge = BlasterChargeSeconds(inventory.GetBlasterRapidFireLevel());
        const float fraction = std::clamp(player->GetBlasterLoadTimer() / fullCharge, 0.0f, 1.0f);
        combatPreview_.visible = true;
        combatPreview_.ready = player->GetBlasterState() == CrossbowState::Loaded;
        combatPreview_.cooldownFraction = player->GetBlasterState() == CrossbowState::Loaded ? 1.0f : fraction;
        if (player->GetBlasterState() == CrossbowState::Loaded)
        {
            const bool sniper = selectedRangedItem == ItemType::SniperRifle;
            const std::string weaponName = sniper ? "Снайперская винтовка" : "Бластер";
            combatPreview_.label = sniper && BuildLocalPlayerCommand().scopeHeld
                ? weaponName + " · ЗАРЯЖЕН · ПКМ/колесо: x" + FormatTenths(sniperMagnification_) + " · ЛКМ: огонь"
                : weaponName + (sniper ? " · ЗАРЯЖЕН · ПКМ: оптика · ЛКМ: огонь" : " · ЗАРЯЖЕН · ЛКМ: огонь");
        }
        else if (player->GetBlasterState() == CrossbowState::Loading)
        {
            combatPreview_.label = std::string(selectedRangedItem == ItemType::SniperRifle ? "Снайперская винтовка" : "Бластер") + " · зарядка "
                + std::to_string(static_cast<int>(std::round(fraction * 100.0f))) + "%";
        }
        else
        {
            combatPreview_.label = selectedRangedItem == ItemType::SniperRifle
                ? "Снайперская винтовка разряжена · удерживайте ЛКМ"
                : "Бластер разряжен · удерживайте ЛКМ";
        }
        return;
    }
    const std::optional<WeaponType> selectedWeapon = GetSelectedWeaponType(*player);
    if (!selectedWeapon.has_value())
    {
        combatPreview_.visible = true;
        combatPreview_.ready = false;
        combatPreview_.label = "Выберите оружие для боя";
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
    const PlayerCommand command = BuildLocalPlayerCommand();
    if (!command.placeHeld || matchSimulation_.HasWinner() || shopOpen_ || inventoryOpen_)
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
    fastPlaceTimer_ = 0.22f;
}

void Game::UpdateAttackOrBreak(float dt)
{
    // Phase 6: melee/break/ranged for every human run through the ONE
    // authoritative command path — ApplyNetworkPlayerActions, fed by the
    // integrated loopback server in singleplayer and by the real transport in
    // multiplayer. This per-tick hook only clears the single-instance local
    // HUD mirrors; the command path repopulates them for the player with the
    // local camera later in the same tick (IntegratedServerTick runs after
    // this in UpdateMatchSimulation).
    (void)dt;
    ResetBreakProgress();
    blasterCharging_ = false;
    attackChargeActive_ = false;
    attackChargeTimer_ = 0.0f;
}

float Game::ComputeBreakRequiredSeconds(Player& player, const RaycastHit& hit, bool isCore)
{
    if (creativeMode_)
    {
        return isCore ? 0.02f : 0.001f;
    }

    // THE break-time rule: base block toughness vs. tool level, plus the Likho
    // mining modifiers (solo speed bonus + active2 persistent cuts). Shared by
    // the authoritative server path (ApplyNetworkPlayerActions) and the client
    // prediction path (UpdatePredictedBreakProgress) so the predicted HUD bar
    // fills at exactly the authoritative rate. Registering/refreshing a cut is
    // part of the rule: on the server it is the authoritative record; on a
    // network client it mirrors the same record for the client's own mining.
    const bool usingAxe = GetSelectedWeaponType(player) == WeaponType::Axe;
    float requiredSeconds = BreakSeconds(hit.blockData.type, EffectiveToolLevel(player), usingAxe);
    if (isCore || player.GetHeroId() != HeroId::Likho)
    {
        return requiredSeconds;
    }

    const bool allyNearby = std::any_of(
        players_.begin(), players_.end(),
        [&player](const Player& candidate)
        {
            return candidate.GetId() != player.GetId()
                && candidate.GetTeamId() == player.GetTeamId()
                && candidate.IsAlive()
                && DistanceSquared(candidate.GetPosition(), player.GetPosition()) <= 64.0f;
        });
    if (!allyNearby)
    {
        requiredSeconds *= 0.75f;
    }

    if (player.GetHeroState().active2.active)
    {
        auto cut = std::find_if(likhoBlockCuts_.begin(), likhoBlockCuts_.end(),
            [&player, &hit](const LikhoBlockCut& existing)
            {
                return existing.ownerPlayerId == player.GetId() && existing.position == hit.block;
            });
        if (cut == likhoBlockCuts_.end())
        {
            likhoBlockCuts_.push_back(LikhoBlockCut { hit.block, hit.blockData.type, player.GetId(), 10.0f });
        }
        else
        {
            cut->lifetime = 10.0f;
        }
    }

    const bool hasPersistentCut = std::any_of(likhoBlockCuts_.begin(), likhoBlockCuts_.end(),
        [&player, &hit](const LikhoBlockCut& cut)
        {
            return cut.ownerPlayerId == player.GetId()
                && cut.position == hit.block
                && cut.blockType == hit.blockData.type;
        });
    if (hasPersistentCut)
    {
        requiredSeconds *= 0.72f;
    }
    return requiredSeconds;
}

void Game::ResetBreakProgress()
{
    breakProgress_ = BreakProgress {};
}

Game::BlockActionResult Game::ApplyCompletedBreakProgress(Player& player, const BreakProgress& progress)
{
    BlockActionResult result {};
    if (!progress.visible)
    {
        return result;
    }
    result.handled = true;
    result.kind = BlockActionKind::Break;
    result.blockType = progress.targetType;

    const GridPos target = progress.target;
    const bool recordCreativeHistory = creativeMode_ && !creativeRestoringHistory_;
    const CreativeMapDocument creativeBefore = recordCreativeHistory ? BuildCreativeMapDocument() : CreativeMapDocument {};
    if (creativeMode_)
    {
        auto specialIt = std::find_if(
            creativeSpecials_.begin(),
            creativeSpecials_.end(),
            [&target](const CreativeSpecial& special)
            {
                return special.pos == target
                    && special.kind != CreativeSpecialKind::HeroSpawn
                    && special.kind != CreativeSpecialKind::Shop;
            });
        if (specialIt != creativeSpecials_.end())
        {
            const std::string name = DisplayName(specialIt->kind);
            RemoveCreativeSpecialByIndex(static_cast<std::size_t>(std::distance(creativeSpecials_.begin(), specialIt)));
            result.success = true;
            result.position = world_.GridToWorld(target);
            result.color = Color { 255, 235, 142, 255 };
            result.message = "Убрано: " + name + ".";
            result.hasWorldEffect = true;
            result.playBreakSound = true;
            result.incrementLocalBroken = IsLocallyPredicted(ControlKindForPlayer(player));
            if (recordCreativeHistory)
            {
                PushCreativeHistory("удаление спецблока " + name, creativeBefore);
            }
            return result;
        }
    }
    if (progress.isCore)
    {
        EnergyCore* core = FindCoreAt(target);
        if (core != nullptr)
        {
            std::string coreMessage;
            CombatEvent coreEvent;
            if (combat_.DamageCore(player, *core, coreMessage, &coreEvent, EffectiveToolLevel(player)))
            {
                bool radonSacrifice = false;
                if (!core->IsAlive())
                {
                    radonSacrifice = TryRadonCoreSacrifice(*core);
                    if (radonSacrifice)
                    {
                        coreEvent.coreDestroyed = false;
                        coreMessage = "Радон принял разрушение Кора на себя. Кор остался на 20 HP.";
                    }
                    else
                    {
                        RemoveWorldBlock(core->GetBlockPosition(), BlockDeltaReason::CoreDestroyed, player.GetId());
                        Team* team = FindTeam(core->GetTeamId());
                        if (team != nullptr)
                        {
                            team->coreAlive = false;
                        }
                    }
                }
                RegisterCombatEvent(coreEvent, coreMessage);
                player.GetInventory().DamageTool(2);
            }
        }
        return result;
    }

    const Block* blockBeforeBreak = world_.GetBlock(target);
    const std::optional<Block> brokenBlock = blockBeforeBreak != nullptr
        ? std::optional<Block>(*blockBeforeBreak)
        : std::nullopt;
    // The creative editor edits raw map data: imported/authored blocks carry
    // breakable=0 as match protection, which must not lock the map's own
    // author out of removing them.
    const bool removed = creativeMode_
        ? RemoveWorldBlock(target, BlockDeltaReason::PlayerBreak, player.GetId())
        : BreakWorldBlock(target, player.GetTeamId(), BlockDeltaReason::PlayerBreak, player.GetId());
    if (removed)
    {
        const Vector3 center = world_.GridToWorld(target);
        result.success = true;
        result.position = center;
        result.color = Color { 210, 220, 235, 255 };
        result.message = std::string(DisplayName(progress.targetType)) + " сломан.";
        result.hasWorldEffect = true;
        result.playBreakSound = true;
        result.incrementLocalBroken = IsLocallyPredicted(ControlKindForPlayer(player));
        if (!creativeMode_)
        {
            SpawnBrokenBlockDrop(progress.targetType, target, player.GetId());
            if (brokenBlock.has_value())
            {
                ApplyBromBlockBreakPassive(player, *brokenBlock, center);
            }
            player.GetInventory().DamageTool(1);
        }
        else if (recordCreativeHistory)
        {
            PushCreativeHistory(std::string("ломание ") + DisplayName(progress.targetType), creativeBefore);
        }
    }
    else
    {
        result.message = "Этот блок защищен.";
        result.playDeniedSound = true;
    }
    return result;
}

void Game::SpawnBrokenBlockDrop(BlockType type, const GridPos& pos, int ownerPlayerId)
{
    const ItemType item = ItemFromBlock(type);
    if (item == ItemType::None)
    {
        return;
    }

    const Vector3 center = world_.GridToWorld(pos);
    const int seed = pos.x * 73856093 ^ pos.y * 19349663 ^ pos.z * 83492791;
    const float angle = static_cast<float>(std::abs(seed % 628)) * 0.01f;
    const float horizontalSpeed = 0.55f + static_cast<float>(std::abs((seed / 17) % 35)) * 0.01f;
    matchSimulation_.DroppedItems().push_back(DroppedItem {
        ItemStack { item, 1 },
        Vec3 { center.x, center.y + 0.42f, center.z },
        Vec3 { std::cos(angle) * horizontalSpeed, 1.85f, std::sin(angle) * horizontalSpeed },
        ownerPlayerId,
        0.45f,
        45.0f,
        0.0f,
        false,
        NextDroppedItemId() });
}

void Game::CompleteBreakProgress(Player& player, const BreakProgress& progress)
{
    const BlockActionResult result = ApplyCompletedBreakProgress(player, progress);
    PresentBlockActionResult(player, result, true);
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
        SetMessage("Выберите блок на панели, чтобы поставить его.");
        audio_.PlayDenied();
        return;
    }

    if (player->GetInventory().GetHotbarSlots()[selectedHotbarSlot_].count <= 0)
    {
        SetMessage(std::string("В выбранном слоте нет блоков: ") + DisplayName(*selectedBlock) + ".");
        audio_.PlayDenied();
        return;
    }

    const PlacementPreview preview = BuildPlacementPreview(*player);
    if (!preview.visible || !preview.valid)
    {
        SetMessage(preview.reason.empty() ? "Здесь нельзя поставить блок." : preview.reason);
        audio_.PlayDenied();
        return;
    }

    // The world mutation itself is always authoritative: this tick's place
    // input reaches ApplyNetworkBlockPlace through the command path (the
    // integrated loopback server in SP, the real server in MP). This function
    // only supplies the immediate client-side denial feedback above.
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
    GridPos placePos {};
    preview.selectedType = *selectedBlock;

    const std::optional<RaycastHit> hit = RaycastFromAim(player, creativeMode_ ? kCreativeBuildReach : kBuildReach);
    if (hit.has_value())
    {
        placePos = hit->adjacent;
        preview.targetBlock = hit->block;
        preview.faceNormal = hit->normal;
        preview.hasTarget = true;
    }
    else
    {
        preview.reason = "Наведитесь на грань блока";
        return preview;
    }

    std::string reason;
    preview.position = placePos;
    preview.visible = true;
    preview.valid = CanPlaceBlockAt(placePos, player, &reason,
        preview.hasTarget ? std::optional<GridPos>(preview.targetBlock) : std::nullopt);
    if (preview.valid)
    {
        if (preview.reason.empty())
        {
            preview.reason = std::string("Поставить: ") + DisplayName(*selectedBlock);
        }
    }
    else
    {
        preview.reason = reason;
    }
    return preview;
}

bool Game::CanPlaceBlockAt(const GridPos& pos, const Player& player, std::string* reason,
                           std::optional<GridPos> supportBlock) const
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
        return fail("Сначала выберите блок");
    }
    if (!creativeMode_ && IsCreativeOnlyBlock(requestedType))
    {
        return fail("Этот блок доступен только в Creative");
    }
    if (!creativeMode_ && player.GetInventory().GetBlockCount(requestedType) <= 0)
    {
        return fail(std::string("Нет доступных блоков: ") + DisplayName(requestedType));
    }
    int minY = creativeMode_ ? kCreativeBuildMinY : kBuildMinY;
    int maxY = creativeMode_ ? kCreativeBuildMaxY : kBuildMaxY;
    // A custom document may be taller than the stock arena. Its vertical bounds
    // are cached because pendingCreativeDoc_ is cleared after world setup.
    if (hasCustomMapBuildBounds_)
    {
        minY = std::min(minY, customMapBuildMinY_);
        maxY = std::max(maxY, customMapBuildMaxY_);
    }
    if (pos.y < minY || pos.y > maxY)
    {
        return fail("Высота строительства заблокирована");
    }
    if (OrientationVariantFor(requestedType, world_, pos, player.GetPosition(), supportBlock) == -1)
    {
        return fail(SupportDeniedMessage(requestedType));
    }
    if (!world_.IsAir(pos))
    {
        return fail("Место занято");
    }
    if (!HasAdjacentAnchorBlock(pos))
    {
        return fail("Нужен соседний блок");
    }

    const Vector3 blockCenter = world_.GridToWorld(pos);
    if (DistanceSquared(player.GetPosition(), blockCenter) > (creativeMode_ ? kCreativeBuildDistanceSq : kBuildDistanceSq))
    {
        return fail("Слишком далеко");
    }
    if (WouldBlockOverlapPlayer(pos, player.GetId()))
    {
        return fail("Мешает игрок");
    }

    for (const EnergyCore& core : matchSimulation_.Cores())
    {
        if (core.IsAlive() && core.GetBlockPosition() == pos)
        {
            return fail("Кор защищен");
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
    // Command-driven actors, including bots, place the block in their selected
    // hotbar slot. The auto-pick remains only as a compatibility fallback for
    // legacy bot builders that have not yet been migrated to PlayerCommand.
    if (const std::optional<BlockType> selected = GetSelectedBlockType(player))
    {
        return selected;
    }
    if (!IsBotControlled(ControlKindForPlayer(player)))
    {
        return std::nullopt;
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

Game::BlockActionResult Game::ApplyPlaceBlockForPlayer(Player& player, const GridPos& pos,
                                                        std::optional<GridPos> supportBlock)
{
    BlockActionResult result {};
    result.handled = true;
    result.kind = BlockActionKind::Place;
    result.position = world_.GridToWorld(pos);
    std::string reason;
    if (!CanPlaceBlockAt(pos, player, &reason, supportBlock))
    {
        result.message = reason;
        result.playDeniedSound = true;
        return result;
    }

    std::optional<BlockType> selectedBlock = SelectPlacementBlockForPlayer(player, pos);
    BlockType blockType = selectedBlock.value_or(BlockType::Air);

    if (!IsBuildableBlock(blockType))
    {
        result.message = "Сначала выберите блок.";
        result.playDeniedSound = true;
        return result;
    }
    // Creative building is free and infinite: no stock requirement.
    if (!creativeMode_ && player.GetInventory().GetBlockCount(blockType) <= 0)
    {
        result.message = std::string("Нет доступных блоков: ") + DisplayName(blockType) + ".";
        result.playDeniedSound = true;
        return result;
    }

    const bool recordCreativeHistory = creativeMode_ && !creativeRestoringHistory_;
    const CreativeMapDocument creativeBefore = recordCreativeHistory ? BuildCreativeMapDocument() : CreativeMapDocument {};
    // Fresh Creative coloured materials start cyan (Minecraft dye 9), the
    // dominant castle accent. Imported maps retain their exact dye in variant.
    int variant = (blockType == BlockType::ColoredGlassBlock || blockType == BlockType::ColoredClayBlock) ? 9 : 0;
    if (blockType == BlockType::WoolBlock && player.GetSelectedWoolVariant() >= 0)
    {
        // 16..31 marks an explicitly selected dye while retaining team ownership.
        variant = 16 + player.GetSelectedWoolVariant();
    }
    const int orientation = OrientationVariantFor(blockType, world_, pos, player.GetPosition(), supportBlock);
    if (orientation == -1)
    {
        result.message = SupportDeniedMessage(blockType);
        result.playDeniedSound = true;
        return result;
    }
    if (orientation >= 0)
    {
        variant = orientation;
    }
    // TNT is an entity with gravity.  It still uses the normal placement
    // validation (air cell and supporting face), but never becomes a static
    // world block that could suspend it in mid-air.
    if (blockType != BlockType::ExplosiveBlock
        && !PlaceWorldBlock(pos, Block { blockType, player.GetTeamId(), true, variant }, false, BlockDeltaReason::PlayerPlace, player.GetId()))
    {
        result.message = "Место уже занято.";
        result.playDeniedSound = true;
        return result;
    }

    const PlayerControlKind controlKind = ControlKindForPlayer(player);
    const std::optional<BlockType> selectedSlotBlock = GetSelectedBlockType(player);
    if (creativeMode_)
    {
        // Free building: nothing is spent.
    }
    else if (selectedSlotBlock.has_value() && *selectedSlotBlock == blockType)
    {
        const int slot = IsLocallyPredicted(controlKind) ? selectedHotbarSlot_ : player.GetSelectedSlot();
        player.GetInventory().SpendSlotItem(slot);
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
    result.success = true;
    result.blockType = blockType;
    result.position = center;
    result.color = effectColor;
    result.message = std::string(DisplayName(blockType)) + " поставлен.";
    result.hasWorldEffect = true;
    result.playPlaceSound = true;
    const int shopChoice = blockType == BlockType::WoolBlock ? 1
        : (blockType == BlockType::WoodBlock ? 2
        : (blockType == BlockType::StoneBlock ? 3
        : (blockType == BlockType::ObsidianBlock ? 4
        : (blockType == BlockType::EnergyGlassBlock ? 5
        : (blockType == BlockType::SpringBlock ? 6
        : (blockType == BlockType::StickyBlock ? 7 : (blockType == BlockType::ExplosiveBlock ? 8 : 0)))))));
    if (shopChoice != 0)
    {
        RecordAutomatchShopUse(player, shopChoice);
    }
    if (blockType == BlockType::ExplosiveBlock)
    {
        TimedExplosion explosive;
        explosive.position = center;
        explosive.ownerTeamId = player.GetTeamId();
        explosive.ownerPlayerId = player.GetId();
        explosive.timer = 2.6f;
        explosive.radius = 2.7f;
        explosive.id = NextExplosiveId();
        timedExplosions_.push_back(explosive);
        result.tntActivated = true;
        RecordAutomatchExplosiveUse(player, false);
    }
    result.incrementLocalPlaced = IsLocallyPredicted(controlKind);
    if (recordCreativeHistory)
    {
        PushCreativeHistory(std::string("постановка ") + DisplayName(blockType), creativeBefore);
    }

    return result;
}

void Game::PresentBlockActionResult(const Player& player, const BlockActionResult& result, bool announce)
{
    if (!result.handled || suppressLocalFeedback_)
    {
        return;
    }

    if (result.hasWorldEffect)
    {
        Vector3 direction {
            result.position.x - player.GetPosition().x,
            0.18f,
            result.position.z - player.GetPosition().z,
        };
        Color materialColor = result.blockType == BlockType::Air
            ? result.color
            : VisualTheme::SurfaceDust(result.blockType);
        if (result.blockType == BlockType::WoolBlock || result.blockType == BlockType::TeamBlock)
        {
            materialColor = result.color;
        }
        if (result.kind == BlockActionKind::Break)
        {
            EmitBlockBreakParticles(result.position, direction, materialColor, result.blockType);
        }
        else if (result.kind == BlockActionKind::Place)
        {
            EmitBlockPlaceParticles(result.position, direction, materialColor, result.blockType);
        }
    }
    if (result.kind == BlockActionKind::Break && result.success)
    {
        AddFloatingText("break", result.position, result.color);
    }
    if (result.playPlaceSound)
    {
        audio_.PlayPlaceBlockAt(result.position);
    }
    if (result.playBreakSound)
    {
        audio_.PlayBreakBlockAt(result.position);
    }
    if (result.kind == BlockActionKind::Break && result.playDeniedSound)
    {
        audio_.PlayDenied();
    }
    if (result.tntActivated)
    {
        AddEventMessage("TNT активирован: 2.6 с", Color { 255, 224, 122, 255 }, 1.8f);
    }

    if (result.incrementLocalPlaced)
    {
        ++stats_.blocksPlaced;
        SetMessage(result.message);
    }
    else if (result.incrementLocalBroken)
    {
        ++stats_.blocksBroken;
        SetMessage(result.message);
    }
    else if (announce && !result.message.empty())
    {
        if (result.success && result.kind == BlockActionKind::Place)
        {
            SetMessage(player.GetName() + " поставил блок.");
        }
        else
        {
            SetMessage(result.message);
        }
    }
}

bool Game::TryPlaceBlockForPlayer(Player& player, const GridPos& pos, bool announce)
{
    const BlockActionResult result = ApplyPlaceBlockForPlayer(player, pos);
    PresentBlockActionResult(player, result, announce);
    return result.success;
}

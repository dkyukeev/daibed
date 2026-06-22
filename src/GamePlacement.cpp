#include "Game.h"

#include "raylib.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace
{
constexpr int kBuildMinY = -2;
constexpr int kBuildMaxY = 64;
constexpr int kBuildMapRadius = 72;

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
        combatPreview_.label = inventory.GetUtility(UtilityType::Arrows) <= 0
            ? "Лук · нет стрел"
            : "Лук · натяжение " + std::to_string(static_cast<int>(std::round(drawPower * 100.0f)))
                + "% · урон ≈ " + std::to_string(combatPreview_.damage);
        return;
    }
    const ItemType selectedRangedItem = GetSelectedHotbarStack(*player).type;
    if (ItemIsBlasterWeapon(selectedRangedItem))
    {
        const float fullCharge = BlasterChargeSeconds(inventory.GetBlasterRapidFireLevel());
        const float fraction = std::clamp(attackChargeTimer_ / fullCharge, 0.0f, 1.0f);
        combatPreview_.visible = true;
        combatPreview_.ready = player->GetBlasterState() == CrossbowState::Loaded;
        combatPreview_.cooldownFraction = player->GetBlasterState() == CrossbowState::Loaded ? 1.0f : fraction;
        if (player->GetBlasterState() == CrossbowState::Loaded)
        {
            const bool sniper = selectedRangedItem == ItemType::SniperRifle;
            const std::string weaponName = sniper ? "Снайперская винтовка" : "Бластер";
            combatPreview_.label = sniper && currentInput_.scopeHeld
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
        if (player != nullptr)
        {
            player->ResetBowDraw();
            player->CancelBlasterLoading();
        }
        ResetBreakProgress();
        attackChargeActive_ = false;
        attackChargeTimer_ = 0.0f;
        return;
    }

    const ItemType rangedItem = GetSelectedHotbarStack(*player).type;
    const bool bowSelected = rangedItem == ItemType::Bow;
    const bool blasterSelected = ItemIsBlasterWeapon(rangedItem);
    if (!bowSelected && player->GetBowDrawTimer() > 0.0f)
    {
        player->ResetBowDraw();
    }
    if (!blasterSelected && player->GetBlasterState() == CrossbowState::Loading)
    {
        player->CancelBlasterLoading();
    }
    if (bowSelected)
    {
        if (currentInput_.attackHeld)
        {
            const float previousPower = BowDrawPower(player->GetBowDrawTimer());
            player->AdvanceBowDraw(dt);
            const float drawPower = BowDrawPower(player->GetBowDrawTimer());
            attackChargeActive_ = true;
            attackChargeTimer_ = player->GetBowDrawTimer();
            if (previousPower < 1.0f && drawPower >= 1.0f)
            {
                audio_.PlayPickup();
                AddWorldEffect(player->GetPosition(), cameraController_.GetAimDirection(), Color { 255, 226, 96, 255 }, 0.24f, 0.18f, WorldEffectKind::Ring);
            }
            ResetBreakProgress();
            return;
        }
        if (currentInput_.attackReleased && player->GetBowDrawTimer() > 0.0f)
        {
            LaunchBowShot(*player, cameraController_.GetAimDirection(), BowDrawPower(player->GetBowDrawTimer()), true);
        }
        player->ResetBowDraw();
        attackChargeActive_ = false;
        attackChargeTimer_ = 0.0f;
        ResetBreakProgress();
        return;
    }
    if (blasterSelected)
    {
        const float fullCharge = BlasterChargeSeconds(player->GetInventory().GetBlasterRapidFireLevel());
        if (player->GetBlasterState() == CrossbowState::Loaded && currentInput_.attackPressed)
        {
            const bool aimed = rangedItem == ItemType::SniperRifle ? currentInput_.scopeHeld : currentInput_.placeHeld;
            LaunchBlasterShot(*player, cameraController_.GetAimDirection(), aimed, true);
            ResetBreakProgress();
            return;
        }
        if (player->GetBlasterState() == CrossbowState::Unloaded && currentInput_.attackHeld)
        {
            player->StartBlasterLoading();
            audio_.PlayPickup();
        }
        if (player->GetBlasterState() == CrossbowState::Loading && currentInput_.attackHeld)
        {
            blasterCharging_ = true;
            attackChargeActive_ = true;
            const bool becameLoaded = player->AdvanceBlasterLoading(dt, fullCharge);
            attackChargeTimer_ = player->GetBlasterLoadTimer();
            if (becameLoaded)
            {
                audio_.PlayPickup();
                AddWorldEffect(player->GetPosition(), cameraController_.GetAimDirection(), Color { 190, 255, 255, 255 }, 0.30f, 0.22f, WorldEffectKind::Ring);
                SetMessage("Бластер заряжен. Следующий клик — выстрел.");
            }
            ResetBreakProgress();
            return;
        }
        if (currentInput_.attackReleased && player->GetBlasterState() == CrossbowState::Loading)
        {
            player->CancelBlasterLoading();
            SetMessage("Зарядка бластера отменена.");
        }
        blasterCharging_ = false;
        attackChargeActive_ = false;
        attackChargeTimer_ = player->GetBlasterLoadTimer();
        ResetBreakProgress();
        return;
    }
    blasterCharging_ = false;

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

    if (currentInput_.attackPressed)
    {
        const Vector3 origin = cameraController_.GetAimOrigin();
        const Vector3 direction = cameraController_.GetAimDirection();
        const float range = selectedWeapon.has_value() ? std::max(3.5f, meleeRayLimit) : 4.5f;
        const Vector3 end {
            origin.x + direction.x * range,
            origin.y + direction.y * range,
            origin.z + direction.z * range
        };
        const bool toolAttack = EffectiveToolLevel(*player) > 0;
        const int deviceDamage = toolAttack ? 18 + EffectiveToolLevel(*player) * 9 : 18;
        if (DamageHeroDeviceAlongSegment(player->GetTeamId(), origin, end, deviceDamage, toolAttack))
        {
            player->ResetAttackCooldown(0.45f);
            ResetBreakProgress();
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

    const std::optional<RaycastHit> hit = RaycastFromAim(*player, 4.5f);
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

    if (!isCore && player->GetHeroId() == HeroId::Likho)
    {
        const bool allyNearby = std::any_of(
            players_.begin(), players_.end(),
            [player](const Player& candidate)
            {
                return candidate.GetId() != player->GetId()
                    && candidate.GetTeamId() == player->GetTeamId()
                    && candidate.IsAlive()
                    && DistanceSquared(candidate.GetPosition(), player->GetPosition()) <= 64.0f;
            });
        if (!allyNearby)
        {
            requiredSeconds *= 0.75f;
        }
        if (player->GetHeroState().active2.active)
        {
            auto cut = std::find_if(likhoBlockCuts_.begin(), likhoBlockCuts_.end(),
                [player, &hit](const LikhoBlockCut& existing)
                {
                    return existing.ownerPlayerId == player->GetId() && existing.position == hit->block;
                });
            if (cut == likhoBlockCuts_.end())
            {
                likhoBlockCuts_.push_back(LikhoBlockCut { hit->block, hit->blockData.type, player->GetId(), 10.0f });
            }
            else
            {
                cut->lifetime = 10.0f;
            }
        }
        const bool hasPersistentCut = std::any_of(likhoBlockCuts_.begin(), likhoBlockCuts_.end(),
            [player, &hit](const LikhoBlockCut& cut)
            {
                return cut.ownerPlayerId == player->GetId()
                    && cut.position == hit->block
                    && cut.blockType == hit->blockData.type;
            });
        if (hasPersistentCut)
        {
            requiredSeconds *= 0.72f;
        }
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
                bool radonSacrifice = false;
                if (!core->IsAlive())
                {
                    radonSacrifice = TryRadonCoreSacrifice(*core);
                    if (radonSacrifice)
                    {
                        coreEvent.coreDestroyed = false;
                        coreMessage = "Радон принял разрушение Кора на себя. Core остался на 20 HP.";
                    }
                    else
                    {
                        world_.RemoveBlock(core->GetBlockPosition());
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
        ResetBreakProgress();
        return;
    }

    const Block* blockBeforeBreak = world_.GetBlock(target);
    const std::optional<Block> brokenBlock = blockBeforeBreak != nullptr
        ? std::optional<Block>(*blockBeforeBreak)
        : std::nullopt;
    if (world_.BreakBlock(target, player.GetTeamId()))
    {
        const Vector3 center = world_.GridToWorld(target);
        SetMessage(std::string(DisplayName(breakProgress_.targetType)) + " broken.");
        AddWorldEffect(center, Color { 210, 220, 235, 255 }, 0.28f, 0.25f);
        AddFloatingText("break", center, Color { 210, 220, 235, 255 });
        audio_.PlayBreakBlockAt(center);
        if (player.IsLocal())
        {
            ++stats_.blocksBroken;
        }
        if (brokenBlock.has_value())
        {
            ApplyBromBlockBreakPassive(player, *brokenBlock, center);
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
    else
    {
        const std::optional<RaycastHit> hit = RaycastFromAim(player, 4.5f);
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
    audio_.PlayPlaceBlockAt(center);
    if (blockType == BlockType::ExplosiveBlock)
    {
        timedExplosions_.push_back(TimedExplosion { pos, player.GetTeamId(), player.GetId(), 2.6f, 2.7f });
        AddEventMessage("TNT armed: 2.6s", Color { 255, 224, 122, 255 }, 1.8f);
    }
    if (player.IsLocal())
    {
        ++stats_.blocksPlaced;
        SetMessage((currentInput_.bridgeMode ? "Bridge " : "") + std::string(DisplayName(blockType)) + " placed.");
    }
    else if (announce)
    {
        SetMessage(player.GetName() + " placed a block.");
    }

    return true;
}

#include "Shop.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace
{
float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

const std::vector<ShopItem>& Items()
{
    static const std::vector<ShopItem> items {
        ShopItem { 1, "Blocks", "Wood blocks", "32 cheap building blocks", ResourceType::Iron, 5, ResourceType::Iron, 0, false, 0 },
        ShopItem { 2, "Blocks", "Light blocks", "24 quick bridge blocks", ResourceType::Iron, 8, ResourceType::Iron, 0, false, 0 },
        ShopItem { 3, "Blocks", "Stone blocks", "12 slower-break defense blocks", ResourceType::Iron, 18, ResourceType::Gold, 1, true, 0 },
        ShopItem { 4, "Blocks", "Obsidian", "4 super durable Core blocks", ResourceType::Gold, 8, ResourceType::Crystal, 3, true, 0 },
        ShopItem { 5, "Blocks", "Energy glass", "8 bright high-value blocks", ResourceType::Gold, 4, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 6, "Blocks", "Spring blocks", "4 bounce pads", ResourceType::Gold, 3, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 7, "Blocks", "Sticky blocks", "8 slowing defense blocks", ResourceType::Iron, 14, ResourceType::Gold, 1, true, 0 },
        ShopItem { 8, "Blocks", "TNT block", "1 timed explosive block", ResourceType::Gold, 5, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 101, "Combat", "Blade tier", "damage, reach and knockback", ResourceType::Gold, 6, ResourceType::Iron, 0, false, 3 },
        ShopItem { 102, "Combat", "Pickaxe tier", "faster block and Core breaking", ResourceType::Iron, 8, ResourceType::Crystal, 1, true, 3 },
        ShopItem { 103, "Combat", "Armor tier", "reduces melee damage", ResourceType::Gold, 5, ResourceType::Crystal, 2, true, 3 },
        ShopItem { 104, "Combat", "Energy arrows", "8 ranged shots", ResourceType::Iron, 10, ResourceType::Gold, 1, true, 0 },
        ShopItem { 105, "Combat", "Fireball", "bridge-breaking projectile", ResourceType::Gold, 4, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 106, "Combat", "Battle axe", "wide heavy melee weapon", ResourceType::Gold, 5, ResourceType::Iron, 0, false, 0 },
        ShopItem { 107, "Combat", "Spear", "long reach melee weapon", ResourceType::Gold, 4, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 201, "Utility", "Mobility burst", "18s speed and jump boost", ResourceType::Gold, 3, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 202, "Utility", "Shield pulse", "12s incoming damage shield", ResourceType::Crystal, 3, ResourceType::Iron, 0, false, 0 },
        ShopItem { 203, "Utility", "Med kit", "instant heal charge", ResourceType::Gold, 2, ResourceType::Iron, 0, false, 0 },
        ShopItem { 204, "Utility", "Home teleport", "return to your spawn", ResourceType::Crystal, 2, ResourceType::Gold, 2, true, 0 },
        ShopItem { 205, "Utility", "Dash pearl", "quick forward burst", ResourceType::Crystal, 2, ResourceType::Gold, 1, true, 0 },
        ShopItem { 206, "Utility", "Molotov", "temporary fire zone", ResourceType::Gold, 5, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 207, "Utility", "Alarm trap", "base warning and zap charge", ResourceType::Iron, 12, ResourceType::Gold, 1, true, 0 },
        ShopItem { 301, "Team", "Team forge", "team generators drop more", ResourceType::Crystal, 4, ResourceType::Gold, 2, true, 4 },
        ShopItem { 302, "Team", "Heal aura", "stronger healing in shop zone", ResourceType::Crystal, 3, ResourceType::Gold, 3, true, 3 },
        ShopItem { 303, "Team", "Core repair", "restore 30 EnergyCore health", ResourceType::Crystal, 2, ResourceType::Gold, 3, true, 0 },
        ShopItem { 304, "Team", "Enemy tracker", "reveals nearest enemy compass", ResourceType::Crystal, 3, ResourceType::Gold, 3, true, 1 }
    };
    return items;
}

const char* CategoryName(int categoryIndex)
{
    switch (categoryIndex)
    {
    case 0:
        return "Blocks";
    case 1:
        return "Combat";
    case 2:
        return "Utility";
    case 3:
        return "Team";
    default:
        break;
    }
    return "Blocks";
}

const ShopItem* FindItem(int choice)
{
    const auto& items = Items();
    const auto it = std::find_if(
        items.begin(),
        items.end(),
        [choice](const ShopItem& item)
        {
            return item.choice == choice;
        });

    return it != items.end() ? &(*it) : nullptr;
}

std::string CostText(const ShopItem& item)
{
    std::ostringstream stream;
    stream << item.primaryCost << " " << ToString(item.primaryType);
    if (item.hasSecondaryCost)
    {
        stream << " + " << item.secondaryCost << " " << ToString(item.secondaryType);
    }
    return stream.str();
}
}

bool Shop::IsPlayerInShop(const Player& player, const Team& team) const
{
    return player.IsAlive() && DistanceSquared(player.GetPosition(), team.shopPosition) <= 9.0f;
}

bool Shop::Purchase(Player& player, Team& team, int choice, std::string& message) const
{
    Inventory& inventory = player.GetInventory();
    const ShopItem* item = FindItem(choice);
    if (item == nullptr)
    {
        return false;
    }

    const auto spendCost = [&inventory, item]() -> bool
    {
        if (inventory.GetResource(item->primaryType) < item->primaryCost)
        {
            return false;
        }
        if (item->hasSecondaryCost && inventory.GetResource(item->secondaryType) < item->secondaryCost)
        {
            return false;
        }

        inventory.SpendResource(item->primaryType, item->primaryCost);
        if (item->hasSecondaryCost)
        {
            inventory.SpendResource(item->secondaryType, item->secondaryCost);
        }
        return true;
    };

    switch (choice)
    {
    case 1:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for wood blocks.";
            return false;
        }
        inventory.AddBlock(BlockType::WoodBlock, 32);
        message = "Bought 32 wood blocks.";
        return true;

    case 2:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for light blocks.";
            return false;
        }
        inventory.AddBlock(BlockType::WoolBlock, 24);
        message = "Bought 24 light blocks.";
        return true;

    case 3:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for stone blocks.";
            return false;
        }
        inventory.AddBlock(BlockType::StoneBlock, 12);
        message = "Bought 12 stone blocks.";
        return true;

    case 4:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for obsidian.";
            return false;
        }
        inventory.AddBlock(BlockType::ObsidianBlock, 4);
        message = "Bought 4 obsidian blocks.";
        return true;

    case 5:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for energy glass.";
            return false;
        }
        inventory.AddBlock(BlockType::EnergyGlassBlock, 8);
        message = "Bought 8 energy glass blocks.";
        return true;

    case 6:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for spring blocks.";
            return false;
        }
        inventory.AddBlock(BlockType::SpringBlock, 4);
        message = "Bought 4 spring blocks.";
        return true;

    case 7:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for sticky blocks.";
            return false;
        }
        inventory.AddBlock(BlockType::StickyBlock, 8);
        message = "Bought 8 sticky blocks.";
        return true;

    case 8:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for TNT.";
            return false;
        }
        inventory.AddBlock(BlockType::ExplosiveBlock, 1);
        message = "Bought 1 TNT block.";
        return true;

    case 101:
        if (inventory.GetSwordLevel() >= item->maxLevel)
        {
            message = "Blade is already at max level.";
            return false;
        }
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for blade upgrade.";
            return false;
        }
        inventory.UpgradeSword();
        message = "Blade upgraded.";
        return true;

    case 102:
        if (inventory.GetToolLevel() >= item->maxLevel)
        {
            message = "Pickaxe is already at max level.";
            return false;
        }
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for pickaxe.";
            return false;
        }
        inventory.UpgradeTool();
        if (!inventory.HasItem(ItemType::Pickaxe))
        {
            inventory.AddItem(ItemType::Pickaxe, 1);
        }
        message = inventory.GetToolLevel() == 1 ? "Bought pickaxe." : "Pickaxe upgraded.";
        return true;

    case 103:
        if (inventory.GetArmorLevel() >= item->maxLevel)
        {
            message = "Armor is already at max level.";
            return false;
        }
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for armor.";
            return false;
        }
        inventory.UpgradeArmor();
        message = "Armor upgraded.";
        return true;

    case 104:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for arrows.";
            return false;
        }
        inventory.AddUtility(UtilityType::Arrows, 8);
        message = "Bought 8 energy arrows.";
        return true;

    case 105:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for a fireball.";
            return false;
        }
        inventory.AddUtility(UtilityType::Fireball, 1);
        message = "Bought 1 fireball.";
        return true;

    case 106:
        if (inventory.HasItem(ItemType::Axe))
        {
            message = "You already have a battle axe.";
            return false;
        }
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for a battle axe.";
            return false;
        }
        inventory.AddItem(ItemType::Axe, 1);
        message = "Bought a battle axe.";
        return true;

    case 107:
        if (inventory.HasItem(ItemType::Spear))
        {
            message = "You already have a spear.";
            return false;
        }
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for a spear.";
            return false;
        }
        inventory.AddItem(ItemType::Spear, 1);
        message = "Bought a spear.";
        return true;

    case 201:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for speed boost.";
            return false;
        }
        player.ActivateSpeedBoost(18.0f);
        player.ActivateJumpBoost(18.0f);
        message = "Mobility boost active.";
        return true;

    case 202:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for shield.";
            return false;
        }
        player.ActivateShield(12.0f);
        message = "Shield active.";
        return true;

    case 203:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for med kit.";
            return false;
        }
        inventory.AddUtility(UtilityType::Heal, 1);
        message = "Bought 1 med kit.";
        return true;

    case 204:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for teleport.";
            return false;
        }
        inventory.AddUtility(UtilityType::HomeTeleport, 1);
        message = "Bought 1 home teleport.";
        return true;

    case 205:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for dash pearl.";
            return false;
        }
        inventory.AddUtility(UtilityType::Dash, 1);
        message = "Bought 1 dash pearl.";
        return true;

    case 206:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for molotov.";
            return false;
        }
        inventory.AddUtility(UtilityType::Molotov, 1);
        message = "Bought 1 molotov.";
        return true;

    case 207:
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for alarm trap.";
            return false;
        }
        inventory.AddUtility(UtilityType::AlarmTrap, 1);
        message = "Bought 1 alarm trap.";
        return true;

    case 301:
        if (team.forgeLevel >= item->maxLevel)
        {
            message = "Team forge is already at max level.";
            return false;
        }
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for forge upgrade.";
            return false;
        }
        ++team.forgeLevel;
        inventory.UpgradeTeam();
        message = "Team forge improved.";
        return true;

    case 302:
        if (team.healAuraLevel >= item->maxLevel)
        {
            message = "Heal aura is already at max level.";
            return false;
        }
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for heal aura.";
            return false;
        }
        ++team.healAuraLevel;
        message = "Heal aura improved.";
        return true;

    case 304:
        if (team.enemyTrackerUnlocked)
        {
            message = "Enemy tracker is already unlocked.";
            return false;
        }
        if (!spendCost())
        {
            message = "Need " + CostText(*item) + " for enemy tracker.";
            return false;
        }
        team.enemyTrackerUnlocked = true;
        message = "Enemy tracker unlocked.";
        return true;

    default:
        break;
    }

    return false;
}

bool Shop::CanAfford(const Inventory& inventory, const ShopItem& item) const
{
    if (inventory.GetResource(item.primaryType) < item.primaryCost)
    {
        return false;
    }
    if (item.hasSecondaryCost && inventory.GetResource(item.secondaryType) < item.secondaryCost)
    {
        return false;
    }

    return true;
}

std::vector<ShopItem> Shop::GetItems() const
{
    return Items();
}

std::vector<ShopItem> Shop::GetItemsForCategory(int categoryIndex) const
{
    std::vector<ShopItem> filtered;
    const std::string category = CategoryName(categoryIndex);
    for (const ShopItem& item : Items())
    {
        if (item.category == category)
        {
            filtered.push_back(item);
        }
    }
    return filtered;
}

int Shop::GetCategoryCount() const
{
    return 4;
}

const char* Shop::GetCategoryName(int categoryIndex) const
{
    return CategoryName(categoryIndex);
}

std::string Shop::GetMenuText() const
{
    return "Wheel/Middle category | click or 1-9 buy | Shift buys x4 | R close";
}

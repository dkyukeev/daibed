#pragma once

#include "Player.h"
#include "Team.h"

#include <vector>
#include <string>
#include <cstdint>

using ShopItemTags = std::uint32_t;
inline constexpr ShopItemTags ShopTagBridge = 1u << 0;
inline constexpr ShopItemTags ShopTagDefense = 1u << 1;
inline constexpr ShopItemTags ShopTagBreach = 1u << 2;
inline constexpr ShopItemTags ShopTagMelee = 1u << 3;
inline constexpr ShopItemTags ShopTagRanged = 1u << 4;
inline constexpr ShopItemTags ShopTagMobility = 1u << 5;
inline constexpr ShopItemTags ShopTagSustain = 1u << 6;
inline constexpr ShopItemTags ShopTagTeam = 1u << 7;
inline constexpr ShopItemTags ShopTagUpgrade = 1u << 8;

enum class ShopUsageMode
{
    Placeable,
    Consumable,
    TimedEffect,
    Equipment,
    Upgrade,
    TeamEffect
};

struct ShopItem
{
    int choice = 0;
    std::string category;
    std::string name;
    std::string description;
    ResourceType primaryType = ResourceType::Iron;
    int primaryCost = 0;
    ResourceType secondaryType = ResourceType::Iron;
    int secondaryCost = 0;
    bool hasSecondaryCost = false;
    int maxLevel = 0;
    // Shared contract for AI, telemetry and future item additions.
    ShopItemTags aiTags = 0;
    int grantCount = 1;
    ShopUsageMode usageMode = ShopUsageMode::Consumable;
};

class Shop
{
public:
    bool IsPlayerInShop(const Player& player, const Team& team) const;
    bool Purchase(Player& player, Team& team, int choice, std::string& message) const;
    bool CanAfford(const Inventory& inventory, const ShopItem& item) const;
    ShopItem GetItemForInventory(const Inventory& inventory, const ShopItem& item) const;
    std::vector<ShopItem> GetItems() const;
    std::vector<ShopItem> GetItemsForCategory(int categoryIndex) const;
    std::vector<ShopItem> GetItemsForCategory(int categoryIndex, const Inventory& inventory) const;
    int GetCategoryCount() const;
    const char* GetCategoryName(int categoryIndex) const;
    std::string GetMenuText() const;
};

#pragma once

#include "Player.h"
#include "Team.h"

#include <vector>
#include <string>

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
};

class Shop
{
public:
    bool IsPlayerInShop(const Player& player, const Team& team) const;
    bool Purchase(Player& player, Team& team, int choice, std::string& message) const;
    bool CanAfford(const Inventory& inventory, const ShopItem& item) const;
    std::vector<ShopItem> GetItems() const;
    std::vector<ShopItem> GetItemsForCategory(int categoryIndex) const;
    int GetCategoryCount() const;
    const char* GetCategoryName(int categoryIndex) const;
    std::string GetMenuText() const;
};

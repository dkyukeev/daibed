#include "Inventory.h"

#include "CombatSystem.h"

#include <algorithm>

namespace
{
ItemStack& SlotRef(std::array<ItemStack, kHotbarSlotCount>& hotbar, std::array<ItemStack, kMainInventorySlotCount>& mainSlots, int slot)
{
    return slot < kHotbarSlotCount ? hotbar[slot] : mainSlots[slot - kHotbarSlotCount];
}

const ItemStack& SlotRef(const std::array<ItemStack, kHotbarSlotCount>& hotbar, const std::array<ItemStack, kMainInventorySlotCount>& mainSlots, int slot)
{
    return slot < kHotbarSlotCount ? hotbar[slot] : mainSlots[slot - kHotbarSlotCount];
}

bool AddToSlot(ItemStack& slot, ItemType type, int& remaining)
{
    if (remaining <= 0)
    {
        return true;
    }

    const int maxStack = ItemMaxStack(type);
    if (slot.IsEmpty())
    {
        const int moved = std::min(maxStack, remaining);
        slot = ItemStack { type, moved };
        remaining -= moved;
        return remaining == 0;
    }

    if (slot.type != type || slot.count >= maxStack)
    {
        return false;
    }

    const int moved = std::min(maxStack - slot.count, remaining);
    slot.count += moved;
    remaining -= moved;
    return remaining == 0;
}
}

bool ItemStack::IsEmpty() const
{
    return type == ItemType::None || count <= 0;
}

const char* ItemDisplayName(ItemType type)
{
    switch (type)
    {
    case ItemType::WoodBlock:
        return "Wood block";
    case ItemType::LightBlock:
        return "Light block";
    case ItemType::StoneBlock:
        return "Stone block";
    case ItemType::ObsidianBlock:
        return "Obsidian";
    case ItemType::EnergyGlassBlock:
        return "Energy glass";
    case ItemType::SpringBlock:
        return "Spring block";
    case ItemType::StickyBlock:
        return "Sticky block";
    case ItemType::ExplosiveBlock:
        return "TNT";
    case ItemType::Sword:
        return "Sword";
    case ItemType::Axe:
        return "Axe";
    case ItemType::Spear:
        return "Spear";
    case ItemType::Pickaxe:
        return "Pickaxe";
    case ItemType::EnergyArrow:
        return "Energy arrow";
    case ItemType::Fireball:
        return "Fireball";
    case ItemType::MedKit:
        return "Med kit";
    case ItemType::HomeTeleport:
        return "Home teleport";
    case ItemType::DashPearl:
        return "Dash pearl";
    case ItemType::Molotov:
        return "Molotov";
    case ItemType::AlarmTrap:
        return "Alarm trap";
    case ItemType::IronResource:
        return "Iron";
    case ItemType::GoldResource:
        return "Gold";
    case ItemType::CrystalResource:
        return "Crystal";
    case ItemType::None:
        break;
    }

    return "";
}

const char* ItemShortName(ItemType type)
{
    switch (type)
    {
    case ItemType::WoodBlock:
        return "Wood";
    case ItemType::LightBlock:
        return "Light";
    case ItemType::StoneBlock:
        return "Stone";
    case ItemType::ObsidianBlock:
        return "Obs";
    case ItemType::EnergyGlassBlock:
        return "Glass";
    case ItemType::SpringBlock:
        return "Spring";
    case ItemType::StickyBlock:
        return "Sticky";
    case ItemType::ExplosiveBlock:
        return "TNT";
    case ItemType::Sword:
        return "Sword";
    case ItemType::Axe:
        return "Axe";
    case ItemType::Spear:
        return "Spear";
    case ItemType::Pickaxe:
        return "Pick";
    case ItemType::EnergyArrow:
        return "Arrow";
    case ItemType::Fireball:
        return "Fire";
    case ItemType::MedKit:
        return "Med";
    case ItemType::HomeTeleport:
        return "Home";
    case ItemType::DashPearl:
        return "Dash";
    case ItemType::Molotov:
        return "Molo";
    case ItemType::AlarmTrap:
        return "Alarm";
    case ItemType::IronResource:
        return "Fe";
    case ItemType::GoldResource:
        return "Au";
    case ItemType::CrystalResource:
        return "Cr";
    case ItemType::None:
        break;
    }

    return "";
}

int ItemMaxStack(ItemType type)
{
    if (ItemIsWeapon(type) || ItemIsPickaxe(type))
    {
        return 1;
    }
    if (ItemIsResource(type) || type == ItemType::EnergyArrow)
    {
        return 64;
    }
    if (ItemIsUtility(type))
    {
        return 16;
    }
    if (ItemIsBlock(type))
    {
        return 64;
    }
    return 0;
}

bool ItemIsBlock(ItemType type)
{
    return ItemToBlock(type).has_value();
}

bool ItemIsUtility(ItemType type)
{
    return ItemToUtility(type).has_value();
}

bool ItemIsWeapon(ItemType type)
{
    return ItemToWeapon(type).has_value();
}

bool ItemIsPickaxe(ItemType type)
{
    return type == ItemType::Pickaxe;
}

bool ItemIsResource(ItemType type)
{
    return ItemToResource(type).has_value();
}

std::optional<BlockType> ItemToBlock(ItemType type)
{
    switch (type)
    {
    case ItemType::WoodBlock:
        return BlockType::WoodBlock;
    case ItemType::LightBlock:
        return BlockType::WoolBlock;
    case ItemType::StoneBlock:
        return BlockType::StoneBlock;
    case ItemType::ObsidianBlock:
        return BlockType::ObsidianBlock;
    case ItemType::EnergyGlassBlock:
        return BlockType::EnergyGlassBlock;
    case ItemType::SpringBlock:
        return BlockType::SpringBlock;
    case ItemType::StickyBlock:
        return BlockType::StickyBlock;
    case ItemType::ExplosiveBlock:
        return BlockType::ExplosiveBlock;
    default:
        break;
    }

    return std::nullopt;
}

std::optional<UtilityType> ItemToUtility(ItemType type)
{
    switch (type)
    {
    case ItemType::EnergyArrow:
        return UtilityType::Arrows;
    case ItemType::Fireball:
        return UtilityType::Fireball;
    case ItemType::MedKit:
        return UtilityType::Heal;
    case ItemType::HomeTeleport:
        return UtilityType::HomeTeleport;
    case ItemType::DashPearl:
        return UtilityType::Dash;
    case ItemType::Molotov:
        return UtilityType::Molotov;
    case ItemType::AlarmTrap:
        return UtilityType::AlarmTrap;
    default:
        break;
    }

    return std::nullopt;
}

std::optional<WeaponType> ItemToWeapon(ItemType type)
{
    switch (type)
    {
    case ItemType::Sword:
        return WeaponType::Sword;
    case ItemType::Axe:
        return WeaponType::Axe;
    case ItemType::Spear:
        return WeaponType::Spear;
    default:
        break;
    }

    return std::nullopt;
}

std::optional<ResourceType> ItemToResource(ItemType type)
{
    switch (type)
    {
    case ItemType::IronResource:
        return ResourceType::Iron;
    case ItemType::GoldResource:
        return ResourceType::Gold;
    case ItemType::CrystalResource:
        return ResourceType::Crystal;
    default:
        break;
    }

    return std::nullopt;
}

ItemType ItemFromBlock(BlockType type)
{
    switch (type)
    {
    case BlockType::WoodBlock:
        return ItemType::WoodBlock;
    case BlockType::WoolBlock:
    case BlockType::TeamBlock:
        return ItemType::LightBlock;
    case BlockType::StoneBlock:
        return ItemType::StoneBlock;
    case BlockType::ObsidianBlock:
        return ItemType::ObsidianBlock;
    case BlockType::EnergyGlassBlock:
        return ItemType::EnergyGlassBlock;
    case BlockType::SpringBlock:
        return ItemType::SpringBlock;
    case BlockType::StickyBlock:
        return ItemType::StickyBlock;
    case BlockType::ExplosiveBlock:
        return ItemType::ExplosiveBlock;
    default:
        break;
    }

    return ItemType::None;
}

ItemType ItemFromUtility(UtilityType type)
{
    switch (type)
    {
    case UtilityType::Arrows:
        return ItemType::EnergyArrow;
    case UtilityType::Fireball:
        return ItemType::Fireball;
    case UtilityType::Heal:
        return ItemType::MedKit;
    case UtilityType::HomeTeleport:
        return ItemType::HomeTeleport;
    case UtilityType::Dash:
        return ItemType::DashPearl;
    case UtilityType::Molotov:
        return ItemType::Molotov;
    case UtilityType::AlarmTrap:
        return ItemType::AlarmTrap;
    }

    return ItemType::None;
}

ItemType ItemFromWeapon(WeaponType weapon)
{
    switch (weapon)
    {
    case WeaponType::Sword:
        return ItemType::Sword;
    case WeaponType::Axe:
        return ItemType::Axe;
    case WeaponType::Spear:
        return ItemType::Spear;
    }

    return ItemType::None;
}

ItemType ItemFromResource(ResourceType type)
{
    switch (type)
    {
    case ResourceType::Iron:
        return ItemType::IronResource;
    case ResourceType::Gold:
        return ItemType::GoldResource;
    case ResourceType::Crystal:
        return ItemType::CrystalResource;
    }

    return ItemType::None;
}

void Inventory::AddResource(ResourceType type, int amount)
{
    const int gained = std::max(0, amount);
    resources_[Index(type)] += gained;
    AddItemRaw(ItemFromResource(type), gained);
}

bool Inventory::SpendResource(ResourceType type, int amount)
{
    const int index = Index(type);
    if (resources_[index] < amount)
    {
        return false;
    }

    resources_[index] -= amount;
    SpendItemRaw(ItemFromResource(type), amount);
    return true;
}

int Inventory::GetResource(ResourceType type) const
{
    return resources_[Index(type)];
}

void Inventory::AddBlocks(int amount)
{
    AddBlock(BlockType::WoolBlock, amount);
}

bool Inventory::SpendBlock()
{
    return SpendBlock(BlockType::WoolBlock);
}

int Inventory::GetBlocks() const
{
    return GetTotalBuildBlocks();
}

void Inventory::AddBlock(BlockType type, int amount)
{
    if (!IsBuildableBlock(type))
    {
        return;
    }

    AddItem(ItemFromBlock(type), amount);
}

bool Inventory::SpendBlock(BlockType type)
{
    if (!IsBuildableBlock(type))
    {
        return false;
    }

    return SpendItem(ItemFromBlock(type), 1);
}

int Inventory::GetBlockCount(BlockType type) const
{
    if (!IsBuildableBlock(type))
    {
        return 0;
    }

    return CountItem(ItemFromBlock(type));
}

int Inventory::GetTotalBuildBlocks() const
{
    int total = 0;
    for (const ItemStack& slot : hotbar_)
    {
        if (ItemIsBlock(slot.type))
        {
            total += slot.count;
        }
    }
    for (const ItemStack& slot : mainSlots_)
    {
        if (ItemIsBlock(slot.type))
        {
            total += slot.count;
        }
    }
    return total;
}

void Inventory::AddUtility(UtilityType type, int amount)
{
    AddItem(ItemFromUtility(type), amount);
}

bool Inventory::SpendUtility(UtilityType type)
{
    return SpendItem(ItemFromUtility(type), 1);
}

int Inventory::GetUtility(UtilityType type) const
{
    return CountItem(ItemFromUtility(type));
}

int Inventory::GetSwordLevel() const
{
    return swordLevel_;
}

int Inventory::GetArmorLevel() const
{
    return armorLevel_;
}

int Inventory::GetToolLevel() const
{
    return toolLevel_;
}

int Inventory::GetTeamUpgradeLevel() const
{
    return teamUpgradeLevel_;
}

int Inventory::GetArmorDurability() const
{
    return armorDurability_;
}

int Inventory::GetMaxArmorDurability() const
{
    return maxArmorDurability_;
}

int Inventory::GetToolDurability() const
{
    return toolDurability_;
}

int Inventory::GetMaxToolDurability() const
{
    return maxToolDurability_;
}

bool Inventory::AddItem(ItemType type, int amount)
{
    if (const std::optional<ResourceType> resource = ItemToResource(type))
    {
        const int gained = std::max(0, amount);
        resources_[Index(*resource)] += gained;
        return AddItemRaw(type, gained);
    }

    return AddItemRaw(type, amount);
}

bool Inventory::AddItemRaw(ItemType type, int amount)
{
    if (type == ItemType::None || amount <= 0)
    {
        return false;
    }

    int remaining = amount;
    const bool resourceItem = ItemIsResource(type);
    auto fillExisting = [type, &remaining](auto& slots)
    {
        for (ItemStack& slot : slots)
        {
            if (!slot.IsEmpty() && slot.type == type)
            {
                AddToSlot(slot, type, remaining);
            }
        }
    };
    auto fillEmpty = [type, &remaining](auto& slots)
    {
        for (ItemStack& slot : slots)
        {
            if (slot.IsEmpty())
            {
                AddToSlot(slot, type, remaining);
            }
        }
    };

    if (resourceItem)
    {
        fillExisting(hotbar_);
        fillEmpty(hotbar_);
        fillExisting(mainSlots_);
        fillEmpty(mainSlots_);
    }
    else
    {
        fillExisting(hotbar_);
        fillExisting(mainSlots_);
        fillEmpty(hotbar_);
        fillEmpty(mainSlots_);
    }

    return remaining == 0;
}

bool Inventory::SpendItem(ItemType type, int amount)
{
    if (const std::optional<ResourceType> resource = ItemToResource(type))
    {
        const int index = Index(*resource);
        if (resources_[index] < amount)
        {
            return false;
        }
        if (!SpendItemRaw(type, amount))
        {
            return false;
        }
        resources_[index] -= amount;
        return true;
    }

    return SpendItemRaw(type, amount);
}

bool Inventory::SpendItemRaw(ItemType type, int amount)
{
    if (type == ItemType::None || amount <= 0 || CountItem(type) < amount)
    {
        return false;
    }

    int remaining = amount;
    const auto spendFrom = [&remaining, type](ItemStack& slot)
    {
        if (remaining <= 0 || slot.type != type || slot.IsEmpty())
        {
            return;
        }

        const int spent = std::min(slot.count, remaining);
        slot.count -= spent;
        remaining -= spent;
        if (slot.count <= 0)
        {
            slot = ItemStack {};
        }
    };

    for (ItemStack& slot : hotbar_)
    {
        spendFrom(slot);
    }
    for (ItemStack& slot : mainSlots_)
    {
        spendFrom(slot);
    }

    return true;
}

bool Inventory::SpendSlotItem(int slot, int amount)
{
    if (!IsValidSlot(slot) || amount <= 0)
    {
        return false;
    }

    ItemStack& stack = SlotRef(hotbar_, mainSlots_, slot);
    if (stack.count < amount)
    {
        return false;
    }

    if (const std::optional<ResourceType> resource = ItemToResource(stack.type))
    {
        const int index = Index(*resource);
        resources_[index] = std::max(0, resources_[index] - amount);
    }
    stack.count -= amount;
    if (stack.count <= 0)
    {
        stack = ItemStack {};
    }
    return true;
}

int Inventory::CountItem(ItemType type) const
{
    int total = 0;
    for (const ItemStack& slot : hotbar_)
    {
        if (slot.type == type && !slot.IsEmpty())
        {
            total += slot.count;
        }
    }
    for (const ItemStack& slot : mainSlots_)
    {
        if (slot.type == type && !slot.IsEmpty())
        {
            total += slot.count;
        }
    }
    return total;
}

bool Inventory::HasItem(ItemType type) const
{
    return CountItem(type) > 0;
}

const std::array<ItemStack, kHotbarSlotCount>& Inventory::GetHotbarSlots() const
{
    return hotbar_;
}

const std::array<ItemStack, kMainInventorySlotCount>& Inventory::GetMainSlots() const
{
    return mainSlots_;
}

const ItemStack& Inventory::GetSlot(int slot) const
{
    static const ItemStack empty {};
    if (!IsValidSlot(slot))
    {
        return empty;
    }
    return SlotRef(hotbar_, mainSlots_, slot);
}

ItemStack Inventory::TakeSlot(int slot)
{
    if (!IsValidSlot(slot))
    {
        return ItemStack {};
    }

    ItemStack taken = SlotRef(hotbar_, mainSlots_, slot);
    SlotRef(hotbar_, mainSlots_, slot) = ItemStack {};
    return taken;
}

bool Inventory::PlaceStack(int slot, ItemStack& stack)
{
    if (!IsValidSlot(slot) || stack.IsEmpty())
    {
        return false;
    }

    ItemStack& target = SlotRef(hotbar_, mainSlots_, slot);
    if (target.IsEmpty())
    {
        target = stack;
        stack = ItemStack {};
        return true;
    }

    if (target.type != stack.type)
    {
        return false;
    }

    const int maxStack = ItemMaxStack(target.type);
    const int moved = std::min(maxStack - target.count, stack.count);
    if (moved <= 0)
    {
        return false;
    }

    target.count += moved;
    stack.count -= moved;
    if (stack.count <= 0)
    {
        stack = ItemStack {};
    }
    return true;
}

ItemStack Inventory::SwapSlot(int slot, ItemStack incoming)
{
    if (!IsValidSlot(slot))
    {
        return incoming;
    }

    ItemStack& target = SlotRef(hotbar_, mainSlots_, slot);
    ItemStack old = target;
    target = incoming;
    return old;
}

bool Inventory::IsValidSlot(int slot) const
{
    return slot >= 0 && slot < kInventorySlotCount;
}

void Inventory::UpgradeSword()
{
    ++swordLevel_;
}

void Inventory::UpgradeArmor()
{
    ++armorLevel_;
    maxArmorDurability_ = 90 + armorLevel_ * 45;
    armorDurability_ = maxArmorDurability_;
}

void Inventory::UpgradeTool()
{
    ++toolLevel_;
    maxToolDurability_ = 80 + toolLevel_ * 35;
    toolDurability_ = maxToolDurability_;
}

void Inventory::UpgradeTeam()
{
    ++teamUpgradeLevel_;
}

void Inventory::DowngradeSword()
{
    swordLevel_ = std::max(0, swordLevel_ - 1);
}

void Inventory::DowngradeTool()
{
    toolLevel_ = std::max(0, toolLevel_ - 1);
    if (toolLevel_ <= 0)
    {
        maxToolDurability_ = 0;
        toolDurability_ = 0;
        return;
    }

    maxToolDurability_ = 80 + toolLevel_ * 35;
    toolDurability_ = maxToolDurability_;
}

void Inventory::RepairArmor()
{
    armorDurability_ = maxArmorDurability_;
}

void Inventory::RepairTool()
{
    toolDurability_ = maxToolDurability_;
}

void Inventory::DamageArmor(int amount)
{
    if (maxArmorDurability_ <= 0)
    {
        return;
    }

    armorDurability_ = std::max(0, armorDurability_ - std::max(0, amount));
}

void Inventory::DamageTool(int amount)
{
    toolDurability_ = std::max(0, toolDurability_ - std::max(0, amount));
}

int Inventory::Index(ResourceType type)
{
    return static_cast<int>(type);
}

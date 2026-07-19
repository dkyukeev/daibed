#pragma once

#include "Block.h"
#include "Resource.h"
#include "Simulation/SimMath.h"

#include <array>
#include <optional>

enum class WeaponType;

enum class UtilityType
{
    Arrows = 0,
    Fireball = 1,
    Heal = 2,
    HomeTeleport = 3,
    Dash = 4,
    Molotov = 5,
    AlarmTrap = 6
};

enum class ItemType
{
    None,
    WoodBlock,
    LightBlock,
    StoneBlock,
    ObsidianBlock,
    EnergyGlassBlock,
    SpringBlock,
    StickyBlock,
    ExplosiveBlock,
    Sword,
    Axe,
    Spear,
    Pickaxe,
    Bow,
    Blaster,
    SniperRifle,
    EnergyArrow,
    Fireball,
    MedKit,
    HomeTeleport,
    DashPearl,
    Molotov,
    AlarmTrap,
    IronResource,
    GoldResource,
    CrystalResource,
    SmoothStoneBlock,
    DarkBrickBlock,
    LightBrickBlock,
    MetalBlock,
    GlowBlock,
    PlankBlock,
    DecorativeTileBlock,
    TrimBlock,
    // Creative/imported-map construction materials.  No shop/loadout grants
    // reference these values, so they remain unavailable in normal matches.
    CobblestoneBlock,
    AndesiteBlock,
    PolishedAndesiteBlock,
    StoneBrickBlock,
    ChiseledStoneBrickBlock,
    StoneSlabBlock,
    StoneBrickSlabBlock,
    StoneBrickStairsBlock,
    BirchPlankBlock,
    BirchSlabBlock,
    BirchStairsBlock,
    ColoredGlassBlock,
    ColoredClayBlock,
    LapisBlock,
    DiamondBlock,
    EmeraldBlock,
    GoldBlock,
    IronBarsBlock,
    LadderBlock,
    TorchBlock,
    BarrierBlock
};

struct ItemStack
{
    ItemType type = ItemType::None;
    int count = 0;

    bool IsEmpty() const;
};

constexpr int kHotbarSlotCount = 9;
constexpr int kMainInventorySlotCount = 27;
constexpr int kInventorySlotCount = kHotbarSlotCount + kMainInventorySlotCount;

// A dropped inventory stack in the world. Raylib-free (Vec3) so it can be owned
// by MatchSimulation; the renderer/physics convert at the Game boundary. (Moved
// here from Feedback.h.) See docs/NETWORK_PREP_PLAN.md.
struct DroppedItem
{
    ItemStack stack;
    Vec3 position {};
    Vec3 velocity {};
    int ownerPlayerId = -1;
    float ownerPickupDelay = 0.0f;
    float lifetime = 45.0f;
    float age = 0.0f;
    bool collected = false;
    int id = -1;
};

const char* ItemDisplayName(ItemType type);
const char* ItemShortName(ItemType type);
int ItemMaxStack(ItemType type);
bool ItemIsBlock(ItemType type);
bool ItemIsUtility(ItemType type);
bool ItemIsWeapon(ItemType type);
bool ItemIsBlasterWeapon(ItemType type);
bool ItemIsPickaxe(ItemType type);
bool ItemIsResource(ItemType type);
std::optional<BlockType> ItemToBlock(ItemType type);
std::optional<UtilityType> ItemToUtility(ItemType type);
std::optional<WeaponType> ItemToWeapon(ItemType type);
std::optional<ResourceType> ItemToResource(ItemType type);
ItemType ItemFromBlock(BlockType type);
ItemType ItemFromUtility(UtilityType type);
ItemType ItemFromWeapon(WeaponType weapon);
ItemType ItemFromResource(ResourceType type);

class Inventory
{
public:
    void AddResource(ResourceType type, int amount);
    bool SpendResource(ResourceType type, int amount);
    int GetResource(ResourceType type) const;

    void AddBlocks(int amount);
    bool SpendBlock();
    int GetBlocks() const;
    void AddBlock(BlockType type, int amount);
    bool SpendBlock(BlockType type);
    int GetBlockCount(BlockType type) const;
    int GetTotalBuildBlocks() const;

    void AddUtility(UtilityType type, int amount);
    bool SpendUtility(UtilityType type);
    int GetUtility(UtilityType type) const;

    int GetSwordLevel() const;
    int GetArmorLevel() const;
    int GetToolLevel() const;
    int GetTeamUpgradeLevel() const;
    int GetBlasterRapidFireLevel() const;
    int GetBlasterDamageLevel() const;
    bool UpgradeBlasterRapidFire();
    bool UpgradeBlasterDamage();
    int GetBowUpgradeLevel() const;
    bool UpgradeBow();
    int GetArmorDurability() const;
    int GetMaxArmorDurability() const;
    int GetToolDurability() const;
    int GetMaxToolDurability() const;

    bool AddItem(ItemType type, int amount = 1);
    bool SpendItem(ItemType type, int amount = 1);
    bool SpendSlotItem(int slot, int amount = 1);
    int CountItem(ItemType type) const;
    bool HasItem(ItemType type) const;

    const std::array<ItemStack, kHotbarSlotCount>& GetHotbarSlots() const;
    const std::array<ItemStack, kMainInventorySlotCount>& GetMainSlots() const;
    const ItemStack& GetSlot(int slot) const;
    ItemStack TakeSlot(int slot);
    bool PlaceStack(int slot, ItemStack& stack);
    ItemStack SwapSlot(int slot, ItemStack incoming);
    bool SetSlot(int slot, ItemStack stack);
    bool IsValidSlot(int slot) const;

    void UpgradeSword();
    void UpgradeArmor();
    void UpgradeTool();
    void UpgradeTeam();
    void DowngradeSword();
    void DowngradeTool();
    void RepairArmor();
    void RepairTool();
    void DamageArmor(int amount);
    void DamageTool(int amount);

private:
    bool AddItemRaw(ItemType type, int amount);
    bool SpendItemRaw(ItemType type, int amount);
    static int Index(ResourceType type);

    std::array<int, 3> resources_ { 0, 0, 0 };
    std::array<ItemStack, kHotbarSlotCount> hotbar_ {};
    std::array<ItemStack, kMainInventorySlotCount> mainSlots_ {};
    int swordLevel_ = 0;
    int armorLevel_ = 0;
    int toolLevel_ = 0;
    int teamUpgradeLevel_ = 0;
    int blasterRapidFireLevel_ = 0;
    int blasterDamageLevel_ = 0;
    int bowUpgradeLevel_ = 0;
    int armorDurability_ = 0;
    int maxArmorDurability_ = 0;
    int toolDurability_ = 0;
    int maxToolDurability_ = 0;
};

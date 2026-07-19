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
    static const std::vector<ShopItem> catalog = []
    {
        std::vector<ShopItem> items {
        ShopItem { 1, "Блоки", "Шерсть", "16 дешевых блоков для быстрых мостов", ResourceType::Iron, 4, ResourceType::Iron, 0, false, 0 },
        ShopItem { 2, "Блоки", "Доски", "16 прочных деревянных блока; топор ломает их быстрее", ResourceType::Gold, 4, ResourceType::Iron, 0, false, 0 },
        ShopItem { 3, "Блоки", "Каменные блоки", "16 защитных блоков, ломаются медленнее", ResourceType::Iron, 24, ResourceType::Iron, 0, false, 0 },
        ShopItem { 4, "Блоки", "Обсидиан", "4 сверхпрочных блока для Кора", ResourceType::Crystal, 4, ResourceType::Iron, 0, false, 0 },
        ShopItem { 5, "Блоки", "Энергостекло", "4 прозрачных защитных блока", ResourceType::Iron, 11, ResourceType::Iron, 0, false, 0 },
        ShopItem { 6, "Блоки", "Пружинные блоки", "4 прыжковые платформы", ResourceType::Gold, 3, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 7, "Блоки", "Липкие блоки", "8 защитных блоков с замедлением", ResourceType::Iron, 14, ResourceType::Gold, 1, true, 0 },
        ShopItem { 8, "Блоки", "Блок TNT", "1 взрывной блок с таймером", ResourceType::Gold, 8, ResourceType::Iron, 0, false, 0 },
        ShopItem { 101, "Бой", "Уровень клинка", "урон, дальность и отбрасывание", ResourceType::Iron, 10, ResourceType::Iron, 0, false, 3 },
        ShopItem { 102, "Бой", "Уровень кирки", "быстрее ломает блоки и Кор", ResourceType::Iron, 10, ResourceType::Iron, 0, false, 3 },
        ShopItem { 103, "Бой", "Уровень брони", "уменьшает урон в ближнем бою", ResourceType::Iron, 40, ResourceType::Iron, 0, false, 3 },
        ShopItem { 104, "Бой", "Энергострелы", "8 тактических выстрелов", ResourceType::Gold, 2, ResourceType::Iron, 0, false, 0 },
        ShopItem { 105, "Бой", "Фаербол", "снаряд для разрушения мостов", ResourceType::Iron, 40, ResourceType::Iron, 0, false, 0 },
        ShopItem { 106, "Бой", "Боевой топор", "широкий тяжелый удар в ближнем бою", ResourceType::Gold, 5, ResourceType::Iron, 0, false, 0 },
        ShopItem { 107, "Бой", "Копье", "длинная дистанция в ближнем бою", ResourceType::Gold, 4, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 108, "Бой", "Лук", "натяжение ЛКМ, расходует стрелы", ResourceType::Gold, 12, ResourceType::Iron, 0, false, 0 },
        ShopItem { 109, "Бой", "Улучшение лука", "I: Сила I · II: Сила I/Отдача I · III: Сила II/Отдача II", ResourceType::Gold, 5, ResourceType::Crystal, 1, true, 3 },
        ShopItem { 401, "Бластер", "Бластер", "заряжаемый дальнобойный бластер", ResourceType::Gold, 8, ResourceType::Crystal, 2, true, 0 },
        ShopItem { 402, "Бластер", "Ускоренная зарядка", "ветка скорострельности, уровни I-III", ResourceType::Gold, 5, ResourceType::Crystal, 1, true, 3 },
        ShopItem { 403, "Бластер", "Усиленный выстрел", "ветка урона, уровни I-III", ResourceType::Gold, 6, ResourceType::Crystal, 1, true, 3 },
        ShopItem { 201, "Утилиты", "Рывок мобильности", "18 с скорости и усиленного прыжка", ResourceType::Crystal, 1, ResourceType::Iron, 0, false, 0 },
        ShopItem { 202, "Утилиты", "Импульс щита", "12 с защиты от входящего урона", ResourceType::Crystal, 3, ResourceType::Iron, 0, false, 0 },
        ShopItem { 203, "Утилиты", "Аптечка", "мгновенный заряд лечения", ResourceType::Gold, 2, ResourceType::Iron, 0, false, 0 },
        ShopItem { 204, "Утилиты", "Телепорт домой", "возврат на вашу точку спавна", ResourceType::Crystal, 2, ResourceType::Gold, 2, true, 0 },
        ShopItem { 205, "Утилиты", "Жемчуг рывка", "быстрый рывок вперед", ResourceType::Crystal, 4, ResourceType::Iron, 0, false, 0 },
        ShopItem { 206, "Утилиты", "Коктейль Молотова", "временная зона огня", ResourceType::Gold, 5, ResourceType::Crystal, 1, true, 0 },
        ShopItem { 207, "Утилиты", "Сигнальная ловушка", "сигнал на базе и разряд по врагу", ResourceType::Iron, 12, ResourceType::Gold, 1, true, 0 },
        ShopItem { 301, "Команда", "Командная кузня", "I–IV: генерация +8/16/24/32%; IV иногда создаёт кристалл", ResourceType::Crystal, 4, ResourceType::Gold, 2, true, 4 },
        ShopItem { 302, "Команда", "Аура лечения", "сильнее лечит в зоне магазина", ResourceType::Crystal, 3, ResourceType::Gold, 3, true, 3 },
        ShopItem { 303, "Команда", "Ремонт Кора", "восстановить 30 здоровья Кора", ResourceType::Crystal, 2, ResourceType::Gold, 3, true, 0 },
        ShopItem { 304, "Команда", "Трекер врага", "показывает компас к ближайшему врагу", ResourceType::Crystal, 3, ResourceType::Gold, 3, true, 1 }
        };
        for (ShopItem& item : items)
        {
            switch (item.choice)
            {
            case 1: item.aiTags = ShopTagBridge; item.grantCount = 16; item.usageMode = ShopUsageMode::Placeable; break;
            case 2: item.aiTags = ShopTagBridge | ShopTagDefense; item.grantCount = 16; item.usageMode = ShopUsageMode::Placeable; break;
            case 3: case 4: case 5: case 7: item.aiTags = ShopTagDefense; item.grantCount = item.choice == 3 ? 16 : (item.choice == 4 || item.choice == 5 ? 4 : 8); item.usageMode = ShopUsageMode::Placeable; break;
            case 6: item.aiTags = ShopTagBridge | ShopTagMobility; item.grantCount = 4; item.usageMode = ShopUsageMode::Placeable; break;
            case 8: item.aiTags = ShopTagBreach; item.usageMode = ShopUsageMode::Placeable; break;
            case 101: item.aiTags = ShopTagMelee | ShopTagUpgrade; item.usageMode = ShopUsageMode::Upgrade; break;
            case 102: item.aiTags = ShopTagBreach | ShopTagUpgrade; item.usageMode = ShopUsageMode::Upgrade; break;
            case 103: item.aiTags = ShopTagSustain | ShopTagUpgrade; item.usageMode = ShopUsageMode::Upgrade; break;
            case 104: item.aiTags = ShopTagRanged; item.grantCount = 8; break;
            case 105: case 206: item.aiTags = ShopTagBreach | ShopTagRanged; break;
            case 106: case 107: item.aiTags = ShopTagMelee; item.usageMode = ShopUsageMode::Equipment; break;
            case 108: item.aiTags = ShopTagRanged; item.usageMode = ShopUsageMode::Equipment; break;
            case 109: case 402: case 403: item.aiTags = ShopTagRanged | ShopTagUpgrade; item.usageMode = ShopUsageMode::Upgrade; break;
            case 401: item.aiTags = ShopTagRanged; item.usageMode = ShopUsageMode::Equipment; break;
            case 201: case 202: item.aiTags = item.choice == 201 ? ShopTagMobility : ShopTagSustain; item.usageMode = ShopUsageMode::TimedEffect; break;
            case 203: item.aiTags = ShopTagSustain; break;
            case 204: case 205: item.aiTags = ShopTagMobility; break;
            case 207: item.aiTags = ShopTagDefense; break;
            case 301: case 302: case 304: item.aiTags = ShopTagTeam | ShopTagUpgrade; item.usageMode = ShopUsageMode::TeamEffect; break;
            case 303: item.aiTags = ShopTagTeam | ShopTagSustain; item.usageMode = ShopUsageMode::TeamEffect; break;
            default: break;
            }
        }
        return items;
    }();
    return catalog;
}

const char* CategoryName(int categoryIndex)
{
    switch (categoryIndex)
    {
    case 0:
        return "Блоки";
    case 1:
        return "Бой";
    case 2:
        return "Утилиты";
    case 3:
        return "Команда";
    case 4:
        return "Бластер";
    default:
        break;
    }
    return "Блоки";
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
    const ShopItem* catalogItem = FindItem(choice);
    if (catalogItem == nullptr)
    {
        return false;
    }
    const ShopItem item = GetItemForInventory(inventory, *catalogItem);

    const auto spendCost = [&inventory, &item]() -> bool
    {
        if (inventory.GetResource(item.primaryType) < item.primaryCost)
        {
            return false;
        }
        if (item.hasSecondaryCost && inventory.GetResource(item.secondaryType) < item.secondaryCost)
        {
            return false;
        }

        inventory.SpendResource(item.primaryType, item.primaryCost);
        if (item.hasSecondaryCost)
        {
            inventory.SpendResource(item.secondaryType, item.secondaryCost);
        }
        return true;
    };

    switch (choice)
    {
    case 1:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для шерсти.";
            return false;
        }
        inventory.AddBlock(BlockType::WoolBlock, 16);
        message = "Куплено 16 блоков шерсти.";
        return true;

    case 2:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для досок.";
            return false;
        }
        inventory.AddBlock(BlockType::WoodBlock, 16);
        message = "Куплено 16 блоков досок.";
        return true;

    case 3:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для каменных блоков.";
            return false;
        }
        inventory.AddBlock(BlockType::StoneBlock, 16);
        message = "Куплено 16 каменных блоков.";
        return true;

    case 4:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для обсидиана.";
            return false;
        }
        inventory.AddBlock(BlockType::ObsidianBlock, 4);
        message = "Куплено 4 блока обсидиана.";
        return true;

    case 5:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для энергостекла.";
            return false;
        }
        inventory.AddBlock(BlockType::EnergyGlassBlock, 4);
        message = "Куплено 4 блока энергостекла.";
        return true;

    case 6:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для пружинных блоков.";
            return false;
        }
        inventory.AddBlock(BlockType::SpringBlock, 4);
        message = "Куплено 4 пружинных блока.";
        return true;

    case 7:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для липких блоков.";
            return false;
        }
        inventory.AddBlock(BlockType::StickyBlock, 8);
        message = "Куплено 8 липких блоков.";
        return true;

    case 8:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для TNT.";
            return false;
        }
        inventory.AddBlock(BlockType::ExplosiveBlock, 1);
        message = "Куплен 1 блок TNT.";
        return true;

    case 101:
        if (inventory.GetSwordLevel() >= item.maxLevel)
        {
            message = "Клинок уже максимального уровня.";
            return false;
        }
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для улучшения клинка.";
            return false;
        }
        inventory.UpgradeSword();
        message = "Клинок улучшен.";
        return true;

    case 102:
        if (inventory.GetToolLevel() >= item.maxLevel)
        {
            message = "Кирка уже максимального уровня.";
            return false;
        }
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для кирки.";
            return false;
        }
        inventory.UpgradeTool();
        if (!inventory.HasItem(ItemType::Pickaxe))
        {
            inventory.AddItem(ItemType::Pickaxe, 1);
        }
        message = inventory.GetToolLevel() == 1 ? "Кирка куплена." : "Кирка улучшена.";
        return true;

    case 103:
        if (inventory.GetArmorLevel() >= item.maxLevel)
        {
            message = "Броня уже максимального уровня.";
            return false;
        }
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для брони.";
            return false;
        }
        inventory.UpgradeArmor();
        message = "Броня улучшена.";
        return true;

    case 104:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для стрел.";
            return false;
        }
        inventory.AddUtility(UtilityType::Arrows, 8);
        message = "Куплено 8 энергетических стрел.";
        return true;

    case 105:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для фаербола.";
            return false;
        }
        inventory.AddUtility(UtilityType::Fireball, 1);
        message = "Куплен 1 фаербол.";
        return true;

    case 106:
        if (inventory.HasItem(ItemType::Axe))
        {
            message = "Боевой топор уже куплен.";
            return false;
        }
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для боевого топора.";
            return false;
        }
        inventory.AddItem(ItemType::Axe, 1);
        message = "Боевой топор куплен.";
        return true;

    case 107:
        if (inventory.HasItem(ItemType::Spear))
        {
            message = "Копье уже куплено.";
            return false;
        }
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для копья.";
            return false;
        }
        inventory.AddItem(ItemType::Spear, 1);
        message = "Копье куплено.";
        return true;

    case 108:
        if (inventory.HasItem(ItemType::Bow))
        {
            message = "Лук уже куплен.";
            return false;
        }
        if (!spendCost())
        {
            message = "Недостаточно ресурсов для лука.";
            return false;
        }
        inventory.AddItem(ItemType::Bow, 1);
        message = "Куплен лук. Удерживайте ЛКМ для натяжения.";
        return true;

    case 109:
        if (!inventory.HasItem(ItemType::Bow))
        {
            message = "Сначала купите лук.";
            return false;
        }
        if (inventory.GetBowUpgradeLevel() >= item.maxLevel)
        {
            message = "Лук улучшен до максимума.";
            return false;
        }
        if (!spendCost())
        {
            message = "Недостаточно ресурсов для улучшения лука.";
            return false;
        }
        inventory.UpgradeBow();
        if (inventory.GetBowUpgradeLevel() == 1)
        {
            message = "Лук: Сила I.";
        }
        else if (inventory.GetBowUpgradeLevel() == 2)
        {
            message = "Лук: Сила I, Отдача I.";
        }
        else
        {
            message = "Лук: Сила II, Отдача II.";
        }
        return true;

    case 401:
        if (inventory.HasItem(ItemType::Blaster) || inventory.HasItem(ItemType::SniperRifle))
        {
            message = "Бластер уже куплен.";
            return false;
        }
        if (!spendCost())
        {
            message = "Недостаточно ресурсов для бластера.";
            return false;
        }
        inventory.AddItem(ItemType::Blaster, 1);
        message = "Куплен бластер. Удерживайте ЛКМ для заряда, ПКМ — точное прицеливание.";
        return true;

    case 402:
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            message = "Сначала купите бластер.";
            return false;
        }
        if (inventory.GetBlasterDamageLevel() > 0 || inventory.GetBlasterRapidFireLevel() >= 3)
        {
            message = inventory.GetBlasterDamageLevel() > 0 ? "Уже выбрана ветка урона." : "Скорострельность максимальна.";
            return false;
        }
        if (!spendCost())
        {
            message = "Недостаточно ресурсов для ускорения зарядки.";
            return false;
        }
        inventory.UpgradeBlasterRapidFire();
        message = inventory.GetBlasterRapidFireLevel() == 3 ? "Максимальная скорострельность." : "Ускоренная зарядка улучшена.";
        return true;

    case 403:
        if (!inventory.HasItem(ItemType::Blaster) && !inventory.HasItem(ItemType::SniperRifle))
        {
            message = "Сначала купите бластер.";
            return false;
        }
        if (inventory.GetBlasterRapidFireLevel() > 0 || inventory.GetBlasterDamageLevel() >= 3)
        {
            message = inventory.GetBlasterRapidFireLevel() > 0 ? "Уже выбрана ветка скорострельности." : "Урон максимален.";
            return false;
        }
        if (!spendCost())
        {
            message = "Недостаточно ресурсов для усиления выстрела.";
            return false;
        }
        inventory.UpgradeBlasterDamage();
        message = inventory.GetBlasterDamageLevel() == 3 ? "Максимальный урон." : "Усиленный выстрел улучшен.";
        return true;

    case 201:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для рывка мобильности.";
            return false;
        }
        player.ActivateSpeedBoost(18.0f);
        player.ActivateJumpBoost(18.0f);
        message = "Усиление мобильности активно.";
        return true;

    case 202:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для щита.";
            return false;
        }
        player.ActivateShield(12.0f);
        message = "Щит активен.";
        return true;

    case 203:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для аптечки.";
            return false;
        }
        inventory.AddUtility(UtilityType::Heal, 1);
        message = "Куплена 1 аптечка.";
        return true;

    case 204:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для телепорта.";
            return false;
        }
        inventory.AddUtility(UtilityType::HomeTeleport, 1);
        message = "Куплен 1 телепорт домой.";
        return true;

    case 205:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для жемчуга рывка.";
            return false;
        }
        inventory.AddUtility(UtilityType::Dash, 1);
        message = "Куплен 1 жемчуг рывка.";
        return true;

    case 206:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для коктейля Молотова.";
            return false;
        }
        inventory.AddUtility(UtilityType::Molotov, 1);
        message = "Куплен 1 коктейль Молотова.";
        return true;

    case 207:
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для сигнальной ловушки.";
            return false;
        }
        inventory.AddUtility(UtilityType::AlarmTrap, 1);
        message = "Куплена 1 сигнальная ловушка.";
        return true;

    case 301:
        if (team.forgeLevel >= item.maxLevel)
        {
            message = "Командная кузня уже максимального уровня.";
            return false;
        }
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для улучшения кузни.";
            return false;
        }
        ++team.forgeLevel;
        inventory.UpgradeTeam();
        message = team.forgeLevel >= 4
            ? "Кузня IV: генерация +32%, иногда появляется кристалл."
            : "Кузня улучшена: скорость генерации +" + std::to_string(team.forgeLevel * 8) + "%.";
        return true;

    case 302:
        if (team.healAuraLevel >= item.maxLevel)
        {
            message = "Аура лечения уже максимального уровня.";
            return false;
        }
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для ауры лечения.";
            return false;
        }
        ++team.healAuraLevel;
        message = "Аура лечения улучшена.";
        return true;

    case 304:
        if (team.enemyTrackerUnlocked)
        {
            message = "Трекер врага уже открыт.";
            return false;
        }
        if (!spendCost())
        {
            message = "Нужно " + CostText(item) + " для трекера врага.";
            return false;
        }
        team.enemyTrackerUnlocked = true;
        message = "Трекер врага открыт.";
        return true;

    default:
        break;
    }

    return false;
}

bool Shop::CanAfford(const Inventory& inventory, const ShopItem& item) const
{
    const ShopItem pricedItem = GetItemForInventory(inventory, item);
    if (inventory.GetResource(pricedItem.primaryType) < pricedItem.primaryCost)
    {
        return false;
    }
    if (pricedItem.hasSecondaryCost && inventory.GetResource(pricedItem.secondaryType) < pricedItem.secondaryCost)
    {
        return false;
    }

    return true;
}

ShopItem Shop::GetItemForInventory(const Inventory& inventory, const ShopItem& item) const
{
    ShopItem pricedItem = item;
    const auto setSingleCost = [&pricedItem](ResourceType type, int cost)
    {
        pricedItem.primaryType = type;
        pricedItem.primaryCost = cost;
        pricedItem.hasSecondaryCost = false;
        pricedItem.secondaryType = ResourceType::Iron;
        pricedItem.secondaryCost = 0;
    };

    switch (pricedItem.choice)
    {
    case 101:
        switch (inventory.GetSwordLevel())
        {
        case 0: pricedItem.name = "Каменный меч"; setSingleCost(ResourceType::Iron, 10); break;
        case 1: pricedItem.name = "Железный меч"; setSingleCost(ResourceType::Gold, 7); break;
        default: pricedItem.name = "Алмазный меч"; setSingleCost(ResourceType::Crystal, 3); break;
        }
        break;
    case 102:
        switch (inventory.GetToolLevel())
        {
        case 0: pricedItem.name = "Деревянная кирка"; setSingleCost(ResourceType::Iron, 10); break;
        case 1: pricedItem.name = "Железная кирка"; setSingleCost(ResourceType::Iron, 10); break;
        default: pricedItem.name = "Золотая кирка"; setSingleCost(ResourceType::Gold, 3); break;
        }
        break;
    case 103:
        switch (inventory.GetArmorLevel())
        {
        case 0: pricedItem.name = "Кольчужная броня"; setSingleCost(ResourceType::Iron, 40); break;
        case 1: pricedItem.name = "Железная броня"; setSingleCost(ResourceType::Gold, 12); break;
        default: pricedItem.name = "Алмазная броня"; setSingleCost(ResourceType::Crystal, 6); break;
        }
        break;
    default:
        break;
    }

    return pricedItem;
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

std::vector<ShopItem> Shop::GetItemsForCategory(int categoryIndex, const Inventory& inventory) const
{
    std::vector<ShopItem> filtered = GetItemsForCategory(categoryIndex);
    for (ShopItem& item : filtered)
    {
        item = GetItemForInventory(inventory, item);
    }
    return filtered;
}

int Shop::GetCategoryCount() const
{
    return 5;
}

const char* Shop::GetCategoryName(int categoryIndex) const
{
    return CategoryName(categoryIndex);
}

std::string Shop::GetMenuText() const
{
    return "1-9/клик: купить | Shift x4 | R: закрыть";
}

#include "Game.h"

#include "UiText.h"
#include "VecConvert.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#define DrawText DrawTextUtf8
#define MeasureText MeasureTextUtf8

// Creative mode: the custom-map building sandbox. The running world IS the map
// document — free infinite block building plus a specials layer (cores,
// generators, hero spawns, team chests) that SetupMatch instantiates when the
// map is test-played. Save/load round-trips through CreativeMapDocument
// (maps/*.dbmap), which is also the future modding surface.

namespace
{
constexpr const char* kCreativeMapPath = "maps/creative_map.dbmap";
constexpr float kCreativeReach = 12.0f;
// Loose editor bounds: wider than gameplay's build radius so map authors can
// out-build the default arena, but still finite so a stray click can't place
// a block kilometres away.
// Castle Bedwars spans roughly 110 cells from its normalized centre.
constexpr int kCreativeBuildRadius = 128;
constexpr int kCreativeBuildMinY = -8;
constexpr int kCreativeBuildMaxY = 96;

struct CreativePaletteEntry
{
    bool special = false;
    BlockType block = BlockType::Air;
    CreativeSpecialKind specialKind = CreativeSpecialKind::Core;
};

constexpr CreativePaletteEntry BlockEntry(BlockType type)
{
    return CreativePaletteEntry { false, type, CreativeSpecialKind::Core };
}

constexpr CreativePaletteEntry SpecialEntry(CreativeSpecialKind kind)
{
    return CreativePaletteEntry { true, BlockType::Air, kind };
}

constexpr CreativePaletteEntry kCreativePaletteBasic[] {
    BlockEntry(BlockType::WoodBlock),
    BlockEntry(BlockType::PlankBlock),
    BlockEntry(BlockType::WoolBlock),
    BlockEntry(BlockType::StoneBlock),
    BlockEntry(BlockType::SmoothStoneBlock),
    BlockEntry(BlockType::CobblestoneBlock),
    BlockEntry(BlockType::AndesiteBlock),
    BlockEntry(BlockType::PolishedAndesiteBlock),
    BlockEntry(BlockType::StoneBrickBlock),
    BlockEntry(BlockType::ChiseledStoneBrickBlock),
    BlockEntry(BlockType::StoneSlabBlock),
    BlockEntry(BlockType::StoneBrickSlabBlock),
    BlockEntry(BlockType::StoneBrickStairsBlock),
    BlockEntry(BlockType::BirchPlankBlock),
    BlockEntry(BlockType::BirchSlabBlock),
    BlockEntry(BlockType::BirchStairsBlock),
    BlockEntry(BlockType::ObsidianBlock),
    BlockEntry(BlockType::EnergyGlassBlock)
};

constexpr CreativePaletteEntry kCreativePaletteDecor[] {
    BlockEntry(BlockType::DarkBrickBlock),
    BlockEntry(BlockType::LightBrickBlock),
    BlockEntry(BlockType::MetalBlock),
    BlockEntry(BlockType::GlowBlock),
    BlockEntry(BlockType::DecorativeTileBlock),
    BlockEntry(BlockType::TrimBlock),
    BlockEntry(BlockType::ColoredGlassBlock),
    BlockEntry(BlockType::ColoredClayBlock),
    BlockEntry(BlockType::LapisBlock),
    BlockEntry(BlockType::DiamondBlock),
    BlockEntry(BlockType::EmeraldBlock),
    BlockEntry(BlockType::GoldBlock),
    BlockEntry(BlockType::IronBarsBlock),
    BlockEntry(BlockType::LadderBlock),
    BlockEntry(BlockType::TorchBlock),
    BlockEntry(BlockType::BarrierBlock)
};

constexpr CreativePaletteEntry kCreativePaletteMechanics[] {
    BlockEntry(BlockType::SpringBlock),
    BlockEntry(BlockType::StickyBlock),
    BlockEntry(BlockType::ExplosiveBlock)
};

constexpr CreativePaletteEntry kCreativePaletteSpecials[] {
    SpecialEntry(CreativeSpecialKind::Core),
    SpecialEntry(CreativeSpecialKind::IronGenerator),
    SpecialEntry(CreativeSpecialKind::GoldGenerator),
    SpecialEntry(CreativeSpecialKind::CrystalGenerator),
    SpecialEntry(CreativeSpecialKind::HeroSpawn),
    SpecialEntry(CreativeSpecialKind::TeamChest),
    SpecialEntry(CreativeSpecialKind::Shop),
    SpecialEntry(CreativeSpecialKind::Rally),
    SpecialEntry(CreativeSpecialKind::Lane),
    SpecialEntry(CreativeSpecialKind::Chokepoint),
    SpecialEntry(CreativeSpecialKind::Highground)
};

constexpr int kCreativePaletteCategoryCount = 4;
constexpr int kCreativePaletteCols = 9;
constexpr int kCreativePaletteRows = 3;
constexpr int kCreativePaletteVisibleSlots = kCreativePaletteCols * kCreativePaletteRows;
constexpr int kCreativeAreaMaxBlocks = 8192;
constexpr std::size_t kCreativePaletteSearchMaxBytes = 40;

Color TeamColorForId(int teamId);

const CreativePaletteEntry* CreativePaletteEntries(int tab, int& count)
{
    switch (tab)
    {
    case 0:
        count = static_cast<int>(std::size(kCreativePaletteBasic));
        return kCreativePaletteBasic;
    case 1:
        count = static_cast<int>(std::size(kCreativePaletteDecor));
        return kCreativePaletteDecor;
    case 2:
        count = static_cast<int>(std::size(kCreativePaletteMechanics));
        return kCreativePaletteMechanics;
    default:
        count = static_cast<int>(std::size(kCreativePaletteSpecials));
        return kCreativePaletteSpecials;
    }
}

const char* CreativePaletteCategoryName(int tab)
{
    switch (tab)
    {
    case 0: return "Стройка";
    case 1: return "Декор";
    case 2: return "Механика";
    default: return "Функц.";
    }
}

const char* CreativePaletteEntryName(const CreativePaletteEntry& entry)
{
    return entry.special ? DisplayName(entry.specialKind) : DisplayName(entry.block);
}

Color CreativePaletteBlockColor(BlockType type)
{
    switch (type)
    {
    case BlockType::WoodBlock:
        return Color { 146, 101, 62, 255 };
    case BlockType::PlankBlock:
        return Color { 176, 118, 66, 255 };
    case BlockType::WoolBlock:
        return Color { 235, 235, 242, 255 };
    case BlockType::StoneBlock:
        return Color { 150, 156, 168, 255 };
    case BlockType::SmoothStoneBlock:
        return Color { 184, 190, 198, 255 };
    case BlockType::ObsidianBlock:
        return Color { 46, 34, 72, 255 };
    case BlockType::EnergyGlassBlock:
        return Color { 112, 232, 255, 190 };
    case BlockType::DarkBrickBlock:
        return Color { 70, 62, 74, 255 };
    case BlockType::LightBrickBlock:
        return Color { 214, 194, 154, 255 };
    case BlockType::MetalBlock:
        return Color { 112, 126, 136, 255 };
    case BlockType::GlowBlock:
        return Color { 255, 232, 116, 255 };
    case BlockType::DecorativeTileBlock:
        return Color { 78, 170, 184, 255 };
    case BlockType::TrimBlock:
        return Color { 218, 164, 76, 255 };
    case BlockType::CobblestoneBlock:
        return Color { 104, 108, 112, 255 };
    case BlockType::AndesiteBlock:
        return Color { 118, 122, 126, 255 };
    case BlockType::PolishedAndesiteBlock:
        return Color { 154, 160, 166, 255 };
    case BlockType::StoneBrickBlock:
    case BlockType::StoneBrickSlabBlock:
    case BlockType::StoneBrickStairsBlock:
        return Color { 122, 128, 136, 255 };
    case BlockType::ChiseledStoneBrickBlock:
        return Color { 102, 112, 120, 255 };
    case BlockType::StoneSlabBlock:
        return Color { 168, 174, 180, 255 };
    case BlockType::BirchPlankBlock:
    case BlockType::BirchSlabBlock:
    case BlockType::BirchStairsBlock:
        return Color { 218, 190, 130, 255 };
    case BlockType::ColoredGlassBlock:
        return Color { 84, 184, 208, 170 };
    case BlockType::ColoredClayBlock:
        return Color { 54, 156, 166, 255 };
    case BlockType::LapisBlock:
        return Color { 44, 82, 184, 255 };
    case BlockType::DiamondBlock:
        return Color { 84, 218, 222, 255 };
    case BlockType::EmeraldBlock:
        return Color { 62, 184, 104, 255 };
    case BlockType::GoldBlock:
        return Color { 232, 184, 54, 255 };
    case BlockType::IronBarsBlock:
        return Color { 104, 116, 128, 255 };
    case BlockType::LadderBlock:
        return Color { 170, 118, 62, 255 };
    case BlockType::TorchBlock:
        return Color { 255, 196, 84, 255 };
    case BlockType::BarrierBlock:
        return Color { 255, 80, 172, 120 };
    case BlockType::SpringBlock:
        return Color { 128, 238, 166, 255 };
    case BlockType::StickyBlock:
        return Color { 118, 92, 168, 255 };
    case BlockType::ExplosiveBlock:
        return Color { 220, 56, 50, 255 };
    default:
        return Color { 170, 178, 190, 255 };
    }
}

Color CreativeSpecialColor(CreativeSpecialKind kind, int teamId)
{
    switch (kind)
    {
    case CreativeSpecialKind::IronGenerator:
        return Color { 214, 222, 230, 255 };
    case CreativeSpecialKind::GoldGenerator:
        return Color { 255, 211, 94, 255 };
    case CreativeSpecialKind::CrystalGenerator:
        return Color { 112, 232, 255, 255 };
    case CreativeSpecialKind::Shop:
        return Color { 180, 150, 255, 255 };
    case CreativeSpecialKind::Rally:
        return Color { 255, 236, 120, 255 };
    case CreativeSpecialKind::Lane:
        return Color { 120, 220, 255, 255 };
    case CreativeSpecialKind::Chokepoint:
        return Color { 255, 150, 96, 255 };
    case CreativeSpecialKind::Highground:
        return Color { 210, 150, 255, 255 };
    default:
        return teamId >= 0 ? TeamColorForId(teamId) : WHITE;
    }
}

Rectangle CreativePalettePanelRect()
{
    const int slotSize = 54;
    const int gap = 8;
    const int width = slotSize * kCreativePaletteCols + gap * (kCreativePaletteCols - 1) + 48;
    const int height = 456;
    return Rectangle {
        static_cast<float>(GetScreenWidth() / 2 - width / 2),
        static_cast<float>(GetScreenHeight() / 2 - height / 2),
        static_cast<float>(width),
        static_cast<float>(height)
    };
}

Rectangle CreativePaletteSlotRect(int index)
{
    const Rectangle panel = CreativePalettePanelRect();
    const int slotSize = 54;
    const int gap = 8;
    const int col = index % kCreativePaletteCols;
    const int row = index / kCreativePaletteCols;
    return Rectangle {
        panel.x + 24.0f + static_cast<float>(col * (slotSize + gap)),
        panel.y + 126.0f + static_cast<float>(row * (slotSize + gap)),
        static_cast<float>(slotSize),
        static_cast<float>(slotSize)
    };
}

Rectangle CreativePaletteHotbarRect(int slot)
{
    const Rectangle panel = CreativePalettePanelRect();
    const int slotSize = 54;
    const int gap = 8;
    return Rectangle {
        panel.x + 24.0f + static_cast<float>(slot * (slotSize + gap)),
        panel.y + panel.height - 78.0f,
        static_cast<float>(slotSize),
        static_cast<float>(slotSize)
    };
}

Rectangle CreativePaletteSearchRect()
{
    const Rectangle panel = CreativePalettePanelRect();
    return Rectangle {
        panel.x + 24.0f,
        panel.y + 86.0f,
        panel.width - 48.0f,
        28.0f
    };
}

Rectangle CreativePalettePrevPageRect()
{
    const Rectangle panel = CreativePalettePanelRect();
    return Rectangle {
        panel.x + panel.width - 104.0f,
        panel.y + 316.0f,
        30.0f,
        24.0f
    };
}

Rectangle CreativePaletteNextPageRect()
{
    const Rectangle panel = CreativePalettePanelRect();
    return Rectangle {
        panel.x + panel.width - 64.0f,
        panel.y + 316.0f,
        30.0f,
        24.0f
    };
}

Rectangle CreativePaletteTabRect(int tab)
{
    const Rectangle panel = CreativePalettePanelRect();
    return Rectangle {
        panel.x + 24.0f + static_cast<float>(tab * 118),
        panel.y + 50.0f,
        106.0f,
        28.0f
    };
}

Rectangle CreativePaletteTeamRect(int index)
{
    const Rectangle panel = CreativePalettePanelRect();
    return Rectangle {
        panel.x + panel.width - 216.0f + static_cast<float>(index * 40),
        panel.y + 17.0f,
        32.0f,
        22.0f
    };
}

Color TeamColorForId(int teamId)
{
    switch (teamId)
    {
    case 0: return GetTeamColor(TeamColor::Red);
    case 1: return GetTeamColor(TeamColor::Blue);
    case 2: return GetTeamColor(TeamColor::Green);
    case 3: return GetTeamColor(TeamColor::Yellow);
    default: return WHITE;
    }
}

const char* TeamDisplayNameForId(int teamId)
{
    switch (teamId)
    {
    case 0: return "Красные";
    case 1: return "Синие";
    case 2: return "Зелёные";
    case 3: return "Жёлтые";
    default: return "Нейтральный";
    }
}

void AppendUtf8Codepoint(std::string& text, int codepoint)
{
    if (codepoint < 0 || text.size() >= kCreativePaletteSearchMaxBytes)
    {
        return;
    }
    int byteCount = 1;
    if (codepoint > 0x7F)
    {
        byteCount = codepoint <= 0x7FF ? 2 : (codepoint <= 0xFFFF ? 3 : 4);
    }
    if (text.size() + static_cast<std::size_t>(byteCount) > kCreativePaletteSearchMaxBytes)
    {
        return;
    }
    const auto appendByte = [&text](unsigned char byte)
    {
        text.push_back(static_cast<char>(byte));
    };
    if (codepoint <= 0x7F)
    {
        appendByte(static_cast<unsigned char>(codepoint));
    }
    else if (codepoint <= 0x7FF)
    {
        appendByte(static_cast<unsigned char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        appendByte(static_cast<unsigned char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0xFFFF)
    {
        appendByte(static_cast<unsigned char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        appendByte(static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3F)));
        appendByte(static_cast<unsigned char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0x10FFFF)
    {
        appendByte(static_cast<unsigned char>(0xF0 | ((codepoint >> 18) & 0x07)));
        appendByte(static_cast<unsigned char>(0x80 | ((codepoint >> 12) & 0x3F)));
        appendByte(static_cast<unsigned char>(0x80 | ((codepoint >> 6) & 0x3F)));
        appendByte(static_cast<unsigned char>(0x80 | (codepoint & 0x3F)));
    }
}

void PopUtf8Codepoint(std::string& text)
{
    if (text.empty())
    {
        return;
    }
    std::size_t start = text.size() - 1;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
    {
        --start;
    }
    text.erase(start);
}

std::string LowerAscii(std::string text)
{
    for (char& ch : text)
    {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (value < 128)
        {
            ch = static_cast<char>(std::tolower(value));
        }
    }
    return text;
}

bool ContainsSearchText(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
    {
        return true;
    }
    return LowerAscii(haystack).find(LowerAscii(needle)) != std::string::npos;
}

const char* CreativePaletteEntryAliases(const CreativePaletteEntry& entry)
{
    if (entry.special)
    {
        switch (entry.specialKind)
        {
        case CreativeSpecialKind::Core:
            return "core bed base objective nexus kor";
        case CreativeSpecialKind::IronGenerator:
            return "iron generator gen resource forge";
        case CreativeSpecialKind::GoldGenerator:
            return "gold generator gen resource forge";
        case CreativeSpecialKind::CrystalGenerator:
            return "crystal diamond emerald generator gen resource";
        case CreativeSpecialKind::HeroSpawn:
            return "spawn player hero start respawn";
        case CreativeSpecialKind::TeamChest:
            return "chest team storage stash";
        case CreativeSpecialKind::Shop:
            return "shop merchant store buy zone";
        case CreativeSpecialKind::Rally:
            return "rally regroup staging bot navigation route";
        case CreativeSpecialKind::Lane:
            return "lane route waypoint bot navigation path";
        case CreativeSpecialKind::Chokepoint:
            return "chokepoint choke gate passage bot navigation";
        case CreativeSpecialKind::Highground:
            return "highground height tower overlook bot navigation";
        }
    }

    switch (entry.block)
    {
    case BlockType::WoodBlock:
        return "wood log timber";
    case BlockType::PlankBlock:
        return "plank wood board floor";
    case BlockType::WoolBlock:
        return "wool light team soft";
    case BlockType::StoneBlock:
        return "stone rock gray grey";
    case BlockType::SmoothStoneBlock:
        return "smooth polished stone concrete clean";
    case BlockType::ObsidianBlock:
        return "obsidian dark hard";
    case BlockType::EnergyGlassBlock:
        return "glass transparent energy window cyan";
    case BlockType::DarkBrickBlock:
        return "dark brick masonry wall";
    case BlockType::LightBrickBlock:
        return "light brick sandstone pale wall";
    case BlockType::MetalBlock:
        return "metal iron steel industrial";
    case BlockType::GlowBlock:
        return "glow lamp light lantern emissive";
    case BlockType::DecorativeTileBlock:
        return "tile decorative pattern floor cyan";
    case BlockType::TrimBlock:
        return "trim accent border gold";
    case BlockType::CobblestoneBlock:
        return "cobblestone cobble stone castle masonry";
    case BlockType::AndesiteBlock:
        return "andesite stone gray grey castle";
    case BlockType::PolishedAndesiteBlock:
        return "polished andesite stone smooth castle";
    case BlockType::StoneBrickBlock:
        return "stone brick masonry castle wall";
    case BlockType::ChiseledStoneBrickBlock:
        return "chiseled stone brick carved castle";
    case BlockType::StoneSlabBlock:
        return "stone slab half block step";
    case BlockType::StoneBrickSlabBlock:
        return "stone brick slab half step";
    case BlockType::StoneBrickStairsBlock:
        return "stone brick stairs stair castle steps";
    case BlockType::BirchPlankBlock:
        return "birch planks wood boards pale";
    case BlockType::BirchSlabBlock:
        return "birch wood slab half step";
    case BlockType::BirchStairsBlock:
        return "birch wood stairs stair";
    case BlockType::ColoredGlassBlock:
        return "colored stained glass transparent window red blue lime yellow";
    case BlockType::ColoredClayBlock:
        return "colored stained clay terracotta cyan red lime yellow";
    case BlockType::LapisBlock:
        return "lapis blue gemstone ore block";
    case BlockType::DiamondBlock:
        return "diamond cyan crystal gem block";
    case BlockType::EmeraldBlock:
        return "emerald green crystal gem block";
    case BlockType::GoldBlock:
        return "gold metal treasure block";
    case BlockType::IronBarsBlock:
        return "iron bars metal fence gate";
    case BlockType::LadderBlock:
        return "ladder climb wood rungs";
    case BlockType::TorchBlock:
        return "torch lamp light flame";
    case BlockType::BarrierBlock:
        return "barrier invisible collision creative only";
    case BlockType::SpringBlock:
        return "spring jump bounce pad";
    case BlockType::StickyBlock:
        return "sticky slime glue slow";
    case BlockType::ExplosiveBlock:
        return "tnt explosive bomb";
    default:
        return "";
    }
}

std::string CreativePaletteEntrySearchText(const CreativePaletteEntry& entry)
{
    std::string text = CreativePaletteEntryName(entry);
    text += " ";
    text += CreativePaletteEntryAliases(entry);
    if (!entry.special)
    {
        text += " ";
        text += ToString(entry.block);
    }
    return text;
}

std::vector<CreativePaletteEntry> CreativePaletteVisibleEntries(int tab, const std::string& search)
{
    std::vector<CreativePaletteEntry> visible;
    const bool searching = !search.empty();
    const int firstTab = searching ? 0 : std::clamp(tab, 0, kCreativePaletteCategoryCount - 1);
    const int lastTab = searching ? kCreativePaletteCategoryCount - 1 : firstTab;
    for (int t = firstTab; t <= lastTab; ++t)
    {
        int count = 0;
        const CreativePaletteEntry* entries = CreativePaletteEntries(t, count);
        for (int i = 0; i < count; ++i)
        {
            if (!searching || ContainsSearchText(CreativePaletteEntrySearchText(entries[i]), search))
            {
                visible.push_back(entries[i]);
            }
        }
    }
    return visible;
}

const char* CreativePaletteEntryHint(const CreativePaletteEntry& entry)
{
    if (entry.special)
    {
        switch (entry.specialKind)
        {
        case CreativeSpecialKind::Core:
            return "Цель команды. Для теста карты нужны Коры минимум двух команд.";
        case CreativeSpecialKind::IronGenerator:
        case CreativeSpecialKind::GoldGenerator:
        case CreativeSpecialKind::CrystalGenerator:
            return "Ресурсный генератор. Можно ставить нейтральным или командным.";
        case CreativeSpecialKind::HeroSpawn:
            return "Точка появления команды в тесте карты.";
        case CreativeSpecialKind::TeamChest:
            return "Командный сундук, привязанный к выбранной команде.";
        case CreativeSpecialKind::Shop:
            return "Маркер зоны магазина. Ставится без обычного блока.";
        case CreativeSpecialKind::Rally:
            return "Командная точка сбора перед выходом на маршрут.";
        case CreativeSpecialKind::Lane:
            return "Промежуточная точка авторского маршрута ботов.";
        case CreativeSpecialKind::Chokepoint:
            return "Узкий проход, который боты должны пересечь и могут удерживать.";
        case CreativeSpecialKind::Highground:
            return "Тактическая высота на командном маршруте.";
        }
    }

    switch (entry.block)
    {
    case BlockType::EnergyGlassBlock:
        return "Прозрачный декоративный блок для окон, витрин и мостов.";
    case BlockType::GlowBlock:
        return "Яркий декоративный светильник без динамического освещения.";
    case BlockType::SpringBlock:
        return "Функциональный блок для прыжковых маршрутов.";
    case BlockType::StickyBlock:
        return "Функциональный блок для липких или замедляющих зон.";
    case BlockType::ExplosiveBlock:
        return "Функциональный взрывной блок для тестовых ловушек.";
    default:
        return "Строительный блок. В креативе ставится бесплатно и ломается мгновенно.";
    }
}

const char* CreativePaletteEntryShortLabel(const CreativePaletteEntry& entry)
{
    if (entry.special)
    {
        switch (entry.specialKind)
        {
        case CreativeSpecialKind::Core: return "Core";
        case CreativeSpecialKind::IronGenerator: return "Fe";
        case CreativeSpecialKind::GoldGenerator: return "Au";
        case CreativeSpecialKind::CrystalGenerator: return "Cr";
        case CreativeSpecialKind::HeroSpawn: return "Spawn";
        case CreativeSpecialKind::TeamChest: return "Chest";
        case CreativeSpecialKind::Shop: return "Shop";
        case CreativeSpecialKind::Rally: return "Rally";
        case CreativeSpecialKind::Lane: return "Lane";
        case CreativeSpecialKind::Chokepoint: return "Choke";
        case CreativeSpecialKind::Highground: return "High";
        }
    }
    return ItemShortName(ItemFromBlock(entry.block));
}

std::string GridPosText(const GridPos& pos)
{
    return std::to_string(pos.x) + ", " + std::to_string(pos.y) + ", " + std::to_string(pos.z);
}

std::string FormatMeters(float value)
{
    const int tenths = static_cast<int>(std::max(0.0f, value) * 10.0f + 0.5f);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "m";
}

std::string ClipTextToPixelWidth(std::string text, int maxWidth, int fontSize)
{
    if (MeasureText(text.c_str(), fontSize) <= maxWidth)
    {
        return text;
    }
    while (!text.empty())
    {
        std::string candidate = text + "...";
        if (MeasureText(candidate.c_str(), fontSize) <= maxWidth)
        {
            return candidate;
        }
        PopUtf8Codepoint(text);
    }
    return "...";
}

ResourceType GeneratorResourceForKind(CreativeSpecialKind kind)
{
    switch (kind)
    {
    case CreativeSpecialKind::GoldGenerator: return ResourceType::Gold;
    case CreativeSpecialKind::CrystalGenerator: return ResourceType::Crystal;
    default: return ResourceType::Iron;
    }
}

float GeneratorIntervalForKind(CreativeSpecialKind kind)
{
    // The arena defaults: base iron/gold cadence, mid-style crystal cadence.
    switch (kind)
    {
    case CreativeSpecialKind::GoldGenerator: return 3.4f;
    case CreativeSpecialKind::CrystalGenerator: return 5.0f;
    default: return 0.78f;
    }
}

bool IsGeneratorKind(CreativeSpecialKind kind)
{
    return kind == CreativeSpecialKind::IronGenerator
        || kind == CreativeSpecialKind::GoldGenerator
        || kind == CreativeSpecialKind::CrystalGenerator;
}

bool IsNavigationKind(CreativeSpecialKind kind)
{
    return kind == CreativeSpecialKind::Rally
        || kind == CreativeSpecialKind::Lane
        || kind == CreativeSpecialKind::Chokepoint
        || kind == CreativeSpecialKind::Highground;
}

bool HasPlayableDocumentSpecials(const CreativeMapDocument& doc, std::string& reason)
{
    std::array<int, 4> cores {};
    std::array<int, 4> spawns {};
    std::array<int, 4> chests {};
    std::array<int, 4> shops {};
    int generators = 0;
    for (std::size_t i = 0; i < doc.specials.size(); ++i)
    {
        const CreativeSpecial& special = doc.specials[i];
        if (IsGeneratorKind(special.kind))
        {
            ++generators;
            if (special.teamId < -1 || special.teamId > 3)
            {
                reason = "generator has an invalid team id";
                return false;
            }
        }
        else if (special.teamId < 0 || special.teamId > 3)
        {
            reason = "team special has no valid team";
            return false;
        }
        for (std::size_t j = 0; j < i; ++j)
        {
            if (doc.specials[j].pos == special.pos)
            {
                reason = "multiple specials occupy one cell";
                return false;
            }
        }
        if (special.teamId >= 0 && special.teamId < 4)
        {
            switch (special.kind)
            {
            case CreativeSpecialKind::Core: ++cores[special.teamId]; break;
            case CreativeSpecialKind::HeroSpawn: ++spawns[special.teamId]; break;
            case CreativeSpecialKind::TeamChest: ++chests[special.teamId]; break;
            case CreativeSpecialKind::Shop: ++shops[special.teamId]; break;
            default: break;
            }
        }
    }
    int playableTeams = 0;
    for (int team = 0; team < 4; ++team)
    {
        if (cores[team] == 0)
        {
            continue;
        }
        ++playableTeams;
        if (cores[team] != 1 || spawns[team] < 1 || chests[team] < 1 || shops[team] < 1)
        {
            reason = "each core team needs exactly one core plus spawn, chest and shop";
            return false;
        }
    }
    if (playableTeams < 2)
    {
        reason = "at least two teams need cores";
        return false;
    }
    if (generators == 0)
    {
        reason = "map has no resource generators";
        return false;
    }
    return true;
}

bool CreativeSpecialHasWorldBlock(CreativeSpecialKind kind)
{
    return kind != CreativeSpecialKind::HeroSpawn
        && kind != CreativeSpecialKind::Shop
        && !IsNavigationKind(kind);
}

bool SameCreativeBlock(const Block& a, const Block& b)
{
    return a.type == b.type && a.teamId == b.teamId && a.breakable == b.breakable
        && a.variant == b.variant;
}

struct CreativeSelectionBounds
{
    GridPos min {};
    GridPos max {};
    int volume = 0;
    bool valid = false;
};

CreativeSelectionBounds MakeCreativeSelectionBounds(
    const std::optional<GridPos>& a,
    const std::optional<GridPos>& b)
{
    if (!a.has_value() || !b.has_value())
    {
        return CreativeSelectionBounds {};
    }
    CreativeSelectionBounds bounds;
    bounds.min = GridPos {
        std::min(a->x, b->x),
        std::min(a->y, b->y),
        std::min(a->z, b->z)
    };
    bounds.max = GridPos {
        std::max(a->x, b->x),
        std::max(a->y, b->y),
        std::max(a->z, b->z)
    };
    const int sx = bounds.max.x - bounds.min.x + 1;
    const int sy = bounds.max.y - bounds.min.y + 1;
    const int sz = bounds.max.z - bounds.min.z + 1;
    bounds.volume = sx * sy * sz;
    bounds.valid = sx > 0 && sy > 0 && sz > 0;
    return bounds;
}

constexpr BlockType kCreativeBlockPalette[] {
    BlockType::WoodBlock,
    BlockType::WoolBlock,
    BlockType::StoneBlock,
    BlockType::ObsidianBlock,
    BlockType::EnergyGlassBlock,
    BlockType::SpringBlock,
    BlockType::StickyBlock,
    BlockType::ExplosiveBlock,
    BlockType::SmoothStoneBlock,
    BlockType::DarkBrickBlock,
    BlockType::LightBrickBlock,
    BlockType::MetalBlock,
    BlockType::GlowBlock,
    BlockType::PlankBlock,
    BlockType::DecorativeTileBlock,
    BlockType::TrimBlock,
    BlockType::CobblestoneBlock,
    BlockType::AndesiteBlock,
    BlockType::PolishedAndesiteBlock,
    BlockType::StoneBrickBlock,
    BlockType::ChiseledStoneBrickBlock,
    BlockType::StoneSlabBlock,
    BlockType::StoneBrickSlabBlock,
    BlockType::StoneBrickStairsBlock,
    BlockType::BirchPlankBlock,
    BlockType::BirchSlabBlock,
    BlockType::BirchStairsBlock,
    BlockType::ColoredGlassBlock,
    BlockType::ColoredClayBlock,
    BlockType::LapisBlock,
    BlockType::DiamondBlock,
    BlockType::EmeraldBlock,
    BlockType::GoldBlock,
    BlockType::IronBarsBlock,
    BlockType::LadderBlock,
    BlockType::TorchBlock,
    BlockType::BarrierBlock
};
}

void Game::ResetCreativeEditorTools()
{
    creativeUndoStack_.clear();
    creativeRedoStack_.clear();
    creativeRestoringHistory_ = false;
    creativeSelectionA_.reset();
    creativeSelectionB_.reset();
    creativeValidationVisible_ = false;
}

void Game::ResetCreativePaletteUi()
{
    creativePaletteTab_ = 0;
    creativePaletteCursor_ = 0;
    creativePalettePage_ = 0;
    creativePaletteSearchActive_ = false;
    creativePaletteSearch_.clear();
}

void Game::RestoreCreativeMapDocument(const CreativeMapDocument& doc)
{
    const bool previousRestoring = creativeRestoringHistory_;
    creativeRestoringHistory_ = true;

    Player* localPlayer = GetLocalPlayer();
    const Vec3 savedPosition = localPlayer != nullptr ? localPlayer->GetPositionVec3() : Vec3 {};
    const float savedYaw = localPlayer != nullptr ? localPlayer->GetYaw() : 0.0f;

    world_.Clear();
    matchSimulation_.ResetCores();
    matchSimulation_.ResetGenerators();
    matchSimulation_.ResetPickups();
    matchSimulation_.ResetDroppedItems();
    matchSimulation_.ResetBlockDeltas();
    timedExplosions_.clear();
    projectiles_.clear();
    hazardZones_.clear();

    arenaBiome_ = static_cast<ArenaBiome>(doc.biome);
    arenaLayout_ = static_cast<ArenaLayout>(doc.layout);
    for (const CreativeMapBlock& block : doc.blocks)
    {
        world_.PlaceBlock(block.pos, Block { block.type, block.teamId, block.breakable, block.variant }, true);
    }

    for (Team& team : teams_)
    {
        team.coreAlive = false;
    }

    creativeSpecials_ = doc.specials;
    creativeRouteNodes_ = doc.routeNodes;
    creativeRouteEdges_ = doc.routeEdges;
    std::vector<std::string> routeErrors;
    if (!routeGraph_.Build(creativeRouteNodes_, creativeRouteEdges_, &routeErrors))
    {
        for (const std::string& error : routeErrors)
        {
            std::cerr << "route graph: " << error << '\n';
        }
    }
    else if (!routeGraph_.Empty() && !routeGraph_.ValidatePhysical(world_, &routeErrors))
    {
        for (const std::string& error : routeErrors)
        {
            std::cerr << "route graph physical validation: " << error << '\n';
        }
        routeGraph_.Clear();
    }
    std::array<bool, 4> spawnSet {};
    for (const CreativeSpecial& special : creativeSpecials_)
    {
        const bool teamValid = special.teamId >= 0 && special.teamId < static_cast<int>(teams_.size());
        switch (special.kind)
        {
        case CreativeSpecialKind::Core:
            if (teamValid)
            {
                teams_[special.teamId].coreBlock = special.pos;
                teams_[special.teamId].coreAlive = true;
                world_.PlaceBlock(special.pos, Block { BlockType::EnergyCoreBlock, special.teamId, false }, true);
                matchSimulation_.Cores().emplace_back(special.teamId, special.pos, 120);
            }
            break;
        case CreativeSpecialKind::IronGenerator:
        case CreativeSpecialKind::GoldGenerator:
        case CreativeSpecialKind::CrystalGenerator:
            world_.PlaceBlock(special.pos, Block { BlockType::ResourceGenerator, special.teamId, false }, true);
            matchSimulation_.Generators().emplace_back(
                GeneratorResourceForKind(special.kind),
                ToVec3(world_.GridToWorld(special.pos)),
                GeneratorIntervalForKind(special.kind),
                1,
                special.teamId);
            break;
        case CreativeSpecialKind::HeroSpawn:
            if (teamValid && !spawnSet[special.teamId])
            {
                const Vector3 cell = world_.GridToWorld(special.pos);
                teams_[special.teamId].spawnPoint = Vector3 { cell.x, cell.y + 0.5f, cell.z };
                spawnSet[special.teamId] = true;
            }
            break;
        case CreativeSpecialKind::TeamChest:
            if (teamValid)
            {
                teams_[special.teamId].teamChestBlock = special.pos;
                world_.PlaceBlock(special.pos, Block { BlockType::TeamChestBlock, special.teamId, false }, true);
            }
            break;
        case CreativeSpecialKind::Shop:
            if (teamValid)
            {
                const Vector3 cell = world_.GridToWorld(special.pos);
                teams_[special.teamId].shopPosition = Vector3 { cell.x, cell.y - 0.42f, cell.z };
            }
            break;
        case CreativeSpecialKind::Rally:
        case CreativeSpecialKind::Lane:
        case CreativeSpecialKind::Chokepoint:
        case CreativeSpecialKind::Highground:
            break; // metadata-only navigation marker
        }
    }

    RefreshCreativeTeamSpawns();
    localPlayer = GetLocalPlayer();
    if (localPlayer != nullptr)
    {
        localPlayer->SetPosition(savedPosition);
        localPlayer->SetYaw(savedYaw);
    }
    creativeRestoringHistory_ = previousRestoring;
}

void Game::PushCreativeHistory(std::string label, const CreativeMapDocument& before)
{
    if (!creativeMode_ || creativeRestoringHistory_)
    {
        return;
    }
    CreativeHistoryEntry entry;
    entry.label = std::move(label);
    entry.before = before;
    entry.after = BuildCreativeMapDocument();
    creativeUndoStack_.push_back(std::move(entry));
    if (creativeUndoStack_.size() > 64)
    {
        creativeUndoStack_.erase(creativeUndoStack_.begin());
    }
    creativeRedoStack_.clear();
}

bool Game::UndoCreativeEdit()
{
    if (!creativeMode_ || creativeUndoStack_.empty())
    {
        SetMessage("Нет действий для отката.", 1.4f);
        return false;
    }
    CreativeHistoryEntry entry = creativeUndoStack_.back();
    creativeUndoStack_.pop_back();
    RestoreCreativeMapDocument(entry.before);
    creativeRedoStack_.push_back(entry);
    SetMessage("Отменено: " + entry.label + ".", 1.6f);
    audio_.PlayPickup();
    return true;
}

bool Game::RedoCreativeEdit()
{
    if (!creativeMode_ || creativeRedoStack_.empty())
    {
        SetMessage("Нет действий для повтора.", 1.4f);
        return false;
    }
    CreativeHistoryEntry entry = creativeRedoStack_.back();
    creativeRedoStack_.pop_back();
    RestoreCreativeMapDocument(entry.after);
    creativeUndoStack_.push_back(entry);
    SetMessage("Повторено: " + entry.label + ".", 1.6f);
    audio_.PlayPickup();
    return true;
}

bool Game::SetCreativeSelectionCorner(const Player& player, int corner)
{
    const std::optional<RaycastHit> hit = RaycastFromAim(player, kCreativeReach);
    if (!hit.has_value())
    {
        SetMessage("Наведитесь на блок, чтобы поставить угол выделения.", 1.8f);
        audio_.PlayDenied();
        return false;
    }
    if (corner <= 0)
    {
        creativeSelectionA_ = hit->block;
        SetMessage("Угол A: " + std::to_string(hit->block.x) + ", "
            + std::to_string(hit->block.y) + ", " + std::to_string(hit->block.z) + ".", 1.6f);
    }
    else
    {
        creativeSelectionB_ = hit->block;
        SetMessage("Угол B: " + std::to_string(hit->block.x) + ", "
            + std::to_string(hit->block.y) + ", " + std::to_string(hit->block.z) + ".", 1.6f);
    }
    audio_.PlayPickup();
    return true;
}

bool Game::PickCreativeBlockAlongAim(Player& player)
{
    const std::optional<RaycastHit> hit = RaycastFromAim(player, kCreativeReach);
    if (!hit.has_value())
    {
        SetMessage("Наведитесь на блок, чтобы взять его в слот.", 1.5f);
        return false;
    }
    const ItemType item = ItemFromBlock(hit->blockData.type);
    if (item == ItemType::None)
    {
        SetMessage("Этот объект выбирается через вкладку функциональных блоков.", 1.8f);
        audio_.PlayDenied();
        return false;
    }
    player.GetInventory().SetSlot(selectedHotbarSlot_, ItemStack { item, 999 });
    player.SetSelectedSlot(selectedHotbarSlot_);
    SetMessage(std::string("Взят блок: ") + ItemDisplayName(item) + ".", 1.4f);
    audio_.PlayPickup();
    return true;
}

bool Game::ApplyCreativeAreaFill(Player& player, bool replaceExisting)
{
    const CreativeSelectionBounds bounds = MakeCreativeSelectionBounds(creativeSelectionA_, creativeSelectionB_);
    if (!bounds.valid)
    {
        SetMessage("Сначала задайте два угла области: X и C.", 2.0f);
        audio_.PlayDenied();
        return false;
    }
    if (bounds.volume > kCreativeAreaMaxBlocks)
    {
        SetMessage("Область слишком большая: максимум " + std::to_string(kCreativeAreaMaxBlocks) + " блоков.", 2.4f);
        audio_.PlayDenied();
        return false;
    }
    const std::optional<BlockType> selectedBlock = GetSelectedBlockType(player);
    if (!selectedBlock.has_value() || !IsBuildableBlock(*selectedBlock))
    {
        SetMessage("Выберите строительный блок для заливки.", 1.8f);
        audio_.PlayDenied();
        return false;
    }

    const CreativeMapDocument before = BuildCreativeMapDocument();
    const Block desired { *selectedBlock, player.GetTeamId(), true };
    int changed = 0;
    for (int x = bounds.min.x; x <= bounds.max.x; ++x)
    {
        for (int y = bounds.min.y; y <= bounds.max.y; ++y)
        {
            for (int z = bounds.min.z; z <= bounds.max.z; ++z)
            {
                const GridPos pos { x, y, z };
                const bool specialHere = std::any_of(creativeSpecials_.begin(), creativeSpecials_.end(),
                    [&pos](const CreativeSpecial& special)
                    {
                        return special.pos == pos;
                    });
                if (specialHere)
                {
                    continue;
                }
                const Block* existing = world_.GetBlock(pos);
                if (!replaceExisting && existing != nullptr)
                {
                    continue;
                }
                if (replaceExisting && existing == nullptr)
                {
                    continue;
                }
                if (existing != nullptr
                    && (existing->type == BlockType::EnergyCoreBlock
                        || existing->type == BlockType::ResourceGenerator
                        || existing->type == BlockType::TeamChestBlock))
                {
                    continue;
                }
                if (existing != nullptr && SameCreativeBlock(*existing, desired))
                {
                    continue;
                }
                world_.PlaceBlock(pos, desired, true);
                ++changed;
            }
        }
    }

    if (changed <= 0)
    {
        SetMessage(replaceExisting ? "В области нечего заменить." : "В области нет пустых клеток для заливки.", 1.8f);
        audio_.PlayDenied();
        return false;
    }
    PushCreativeHistory(replaceExisting ? "замена области" : "заливка области", before);
    SetMessage((replaceExisting ? "Заменено блоков: " : "Залито блоков: ") + std::to_string(changed) + ".", 2.0f);
    audio_.PlayPlaceBlock();
    return true;
}

bool Game::ApplyCreativeAreaClear()
{
    const CreativeSelectionBounds bounds = MakeCreativeSelectionBounds(creativeSelectionA_, creativeSelectionB_);
    if (!bounds.valid)
    {
        SetMessage("Сначала задайте два угла области: X и C.", 2.0f);
        audio_.PlayDenied();
        return false;
    }
    if (bounds.volume > kCreativeAreaMaxBlocks)
    {
        SetMessage("Область слишком большая: максимум " + std::to_string(kCreativeAreaMaxBlocks) + " блоков.", 2.4f);
        audio_.PlayDenied();
        return false;
    }

    const CreativeMapDocument before = BuildCreativeMapDocument();
    int changed = 0;
    for (int x = bounds.min.x; x <= bounds.max.x; ++x)
    {
        for (int y = bounds.min.y; y <= bounds.max.y; ++y)
        {
            for (int z = bounds.min.z; z <= bounds.max.z; ++z)
            {
                const GridPos pos { x, y, z };
                const bool specialHere = std::any_of(creativeSpecials_.begin(), creativeSpecials_.end(),
                    [&pos](const CreativeSpecial& special)
                    {
                        return special.pos == pos;
                    });
                if (specialHere)
                {
                    continue;
                }
                const Block* existing = world_.GetBlock(pos);
                if (existing == nullptr
                    || existing->type == BlockType::EnergyCoreBlock
                    || existing->type == BlockType::ResourceGenerator
                    || existing->type == BlockType::TeamChestBlock)
                {
                    continue;
                }
                if (world_.RemoveBlock(pos))
                {
                    ++changed;
                }
            }
        }
    }

    if (changed <= 0)
    {
        SetMessage("В области нечего очистить.", 1.8f);
        audio_.PlayDenied();
        return false;
    }
    PushCreativeHistory("очистка области", before);
    SetMessage("Очищено блоков: " + std::to_string(changed) + ".", 2.0f);
    audio_.PlayBreakBlock();
    return true;
}

std::vector<std::string> Game::BuildCreativeValidationIssues() const
{
    std::vector<std::string> issues;
    std::array<int, 4> cores {};
    std::array<int, 4> spawns {};
    std::array<int, 4> chests {};
    std::array<int, 4> shops {};
    int generators = 0;

    for (std::size_t i = 0; i < creativeSpecials_.size(); ++i)
    {
        const CreativeSpecial& special = creativeSpecials_[i];
        if (!IsGeneratorKind(special.kind) && (special.teamId < 0 || special.teamId > 3))
        {
            issues.push_back(std::string(DisplayName(special.kind)) + " без команды.");
        }
        for (std::size_t j = i + 1; j < creativeSpecials_.size(); ++j)
        {
            if (creativeSpecials_[j].pos == special.pos)
            {
                issues.push_back("Несколько спецобъектов в одной клетке.");
                break;
            }
        }

        if (special.teamId >= 0 && special.teamId < 4)
        {
            switch (special.kind)
            {
            case CreativeSpecialKind::Core: ++cores[special.teamId]; break;
            case CreativeSpecialKind::HeroSpawn: ++spawns[special.teamId]; break;
            case CreativeSpecialKind::TeamChest: ++chests[special.teamId]; break;
            case CreativeSpecialKind::Shop: ++shops[special.teamId]; break;
            default: break;
            }
        }
        if (IsGeneratorKind(special.kind))
        {
            ++generators;
        }
    }

    int playableTeams = 0;
    for (int team = 0; team < 4; ++team)
    {
        if (cores[team] > 0)
        {
            ++playableTeams;
            if (cores[team] > 1)
            {
                issues.push_back(std::string(TeamDisplayNameForId(team)) + ": больше одного Кора.");
            }
            if (spawns[team] <= 0)
            {
                issues.push_back(std::string(TeamDisplayNameForId(team)) + ": нет спавна героя.");
            }
            if (chests[team] <= 0)
            {
                issues.push_back(std::string(TeamDisplayNameForId(team)) + ": нет сундука команды.");
            }
            if (shops[team] <= 0)
            {
                issues.push_back(std::string(TeamDisplayNameForId(team)) + ": нет магазина.");
            }
        }
    }
    if (playableTeams < 2)
    {
        issues.push_back("Для теста нужны Коры минимум двух команд.");
    }
    if (generators <= 0)
    {
        issues.push_back("На карте нет генераторов ресурсов.");
    }
    return issues;
}

bool Game::HandleCreativeEditorTools(Player& player)
{
    bool consumed = false;
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    if (ctrl && IsKeyPressed(KEY_Z))
    {
        consumed = shift ? RedoCreativeEdit() : UndoCreativeEdit();
    }
    else if (ctrl && IsKeyPressed(KEY_Y))
    {
        consumed = RedoCreativeEdit();
    }
    else if (currentInput_.middlePressed)
    {
        consumed = PickCreativeBlockAlongAim(player);
    }
    else if (IsKeyPressed(KEY_X))
    {
        consumed = SetCreativeSelectionCorner(player, 0);
    }
    else if (IsKeyPressed(KEY_C))
    {
        consumed = SetCreativeSelectionCorner(player, 1);
    }
    else if (IsKeyPressed(KEY_F))
    {
        consumed = ApplyCreativeAreaFill(player, false);
        currentInput_.heroActive1Pressed = false;
    }
    else if (IsKeyPressed(KEY_R))
    {
        consumed = ApplyCreativeAreaFill(player, true);
        currentInput_.interactPressed = false;
    }
    else if (IsKeyPressed(KEY_DELETE))
    {
        consumed = ApplyCreativeAreaClear();
    }
    else if (IsKeyPressed(KEY_G))
    {
        creativeSelectionA_.reset();
        creativeSelectionB_.reset();
        SetMessage("Выделение очищено.", 1.3f);
        consumed = true;
    }
    else if (IsKeyPressed(KEY_V))
    {
        creativeValidationVisible_ = !creativeValidationVisible_;
        const std::vector<std::string> issues = BuildCreativeValidationIssues();
        SetMessage(creativeValidationVisible_
            ? (issues.empty() ? "Проверка карты: ошибок нет." : "Проверка карты: " + std::to_string(issues.size()) + " замеч.")
            : "Проверка карты скрыта.",
            2.0f);
        consumed = true;
    }

    if (consumed)
    {
        currentInput_.middlePressed = false;
        currentInput_.attackPressed = false;
        currentInput_.attackHeld = false;
        currentInput_.attackReleased = false;
        currentInput_.placePressed = false;
        currentInput_.placeHeld = false;
        currentInput_.hotbarSlot = 0;
        currentInput_.shopChoice = 0;
        currentInput_.interactPressed = false;
        currentInput_.heroActive1Pressed = false;
        currentInput_.heroActive2Pressed = false;
        currentInput_.heroUltimatePressed = false;
        currentInput_.dropPressed = false;
    }
    return consumed;
}

void Game::GrantCreativePalette(Player& player)
{
    // Palette: the first page lands on predictable hotbar slots; extra
    // decorative blocks live in the Creative inventory so survival economy/shop
    // grants stay untouched. Placement is free in creative, so counts only make
    // the slots read as stocked.
    Inventory& inventory = player.GetInventory();
    for (int slot = 0; slot < static_cast<int>(std::size(kCreativeBlockPalette)) && slot < kInventorySlotCount; ++slot)
    {
        const ItemType item = ItemFromBlock(kCreativeBlockPalette[slot]);
        if (item != ItemType::None)
        {
            inventory.SetSlot(slot, ItemStack { item, 999 });
        }
    }
}

void Game::HandleCreativePaletteInputPolished(Player& player)
{
    creativePaletteTab_ = std::clamp(creativePaletteTab_, 0, kCreativePaletteCategoryCount - 1);
    const Vector2 mouse = GetMousePosition();
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const Rectangle searchRect = CreativePaletteSearchRect();
    const bool searchShortcut = (ctrl && IsKeyPressed(KEY_F))
        || (!creativePaletteSearchActive_ && IsKeyPressed(KEY_SLASH));

    if (searchShortcut)
    {
        creativePaletteSearchActive_ = true;
        for (int key = GetCharPressed(); key > 0; key = GetCharPressed())
        {
        }
    }
    if (CheckCollisionPointRec(mouse, searchRect) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        creativePaletteSearchActive_ = true;
    }
    else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(mouse, searchRect))
    {
        creativePaletteSearchActive_ = false;
    }

    if (creativePaletteSearchActive_)
    {
        bool editedSearch = false;
        if (ctrl && IsKeyPressed(KEY_V))
        {
            if (const char* clipboard = GetClipboardText())
            {
                for (const char* c = clipboard;
                     *c != '\0' && creativePaletteSearch_.size() < kCreativePaletteSearchMaxBytes;
                     ++c)
                {
                    const unsigned char ch = static_cast<unsigned char>(*c);
                    if (ch >= 32)
                    {
                        creativePaletteSearch_.push_back(static_cast<char>(ch));
                        editedSearch = true;
                    }
                }
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !creativePaletteSearch_.empty())
        {
            PopUtf8Codepoint(creativePaletteSearch_);
            editedSearch = true;
        }
        if (IsKeyPressed(KEY_DELETE) && !creativePaletteSearch_.empty())
        {
            creativePaletteSearch_.clear();
            editedSearch = true;
        }
        if (!searchShortcut)
        {
            for (int key = GetCharPressed(); key > 0; key = GetCharPressed())
            {
                if (key >= 32)
                {
                    AppendUtf8Codepoint(creativePaletteSearch_, key);
                    editedSearch = true;
                }
            }
        }
        if (editedSearch)
        {
            creativePaletteCursor_ = 0;
            creativePalettePage_ = 0;
        }
    }

    std::vector<CreativePaletteEntry> visibleEntries =
        CreativePaletteVisibleEntries(creativePaletteTab_, creativePaletteSearch_);
    const auto clampPaletteView = [this, &visibleEntries]()
    {
        const int visibleCount = static_cast<int>(visibleEntries.size());
        const int pageCount = std::max(1, (visibleCount + kCreativePaletteVisibleSlots - 1) / kCreativePaletteVisibleSlots);
        creativePalettePage_ = std::clamp(creativePalettePage_, 0, pageCount - 1);
        creativePaletteCursor_ = std::clamp(creativePaletteCursor_, 0, std::max(0, visibleCount - 1));
        if (visibleCount > 0)
        {
            const int firstOnPage = creativePalettePage_ * kCreativePaletteVisibleSlots;
            const int lastOnPage = std::min(visibleCount - 1, firstOnPage + kCreativePaletteVisibleSlots - 1);
            if (creativePaletteCursor_ < firstOnPage || creativePaletteCursor_ > lastOnPage)
            {
                creativePaletteCursor_ = firstOnPage;
            }
        }
    };
    clampPaletteView();

    for (int tab = 0; tab < kCreativePaletteCategoryCount; ++tab)
    {
        if (CheckCollisionPointRec(mouse, CreativePaletteTabRect(tab)) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            creativePaletteTab_ = tab;
            creativePaletteCursor_ = 0;
            creativePalettePage_ = 0;
            creativePaletteSearchActive_ = false;
            creativePaletteSearch_.clear();
            return;
        }
    }

    for (int teamButton = 0; teamButton < 5; ++teamButton)
    {
        if (CheckCollisionPointRec(mouse, CreativePaletteTeamRect(teamButton)) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            creativeSpecialTeam_ = teamButton < 4 ? teamButton : -1;
            SetMessage(std::string("Команда спецблока: ") + TeamDisplayNameForId(creativeSpecialTeam_) + ".", 1.2f);
            return;
        }
    }

    const int visibleCount = static_cast<int>(visibleEntries.size());
    const int pageCount = std::max(1, (visibleCount + kCreativePaletteVisibleSlots - 1) / kCreativePaletteVisibleSlots);
    if (pageCount > 1)
    {
        if (CheckCollisionPointRec(mouse, CreativePalettePrevPageRect()) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            creativePalettePage_ = (creativePalettePage_ + pageCount - 1) % pageCount;
            creativePaletteCursor_ = creativePalettePage_ * kCreativePaletteVisibleSlots;
            return;
        }
        if (CheckCollisionPointRec(mouse, CreativePaletteNextPageRect()) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            creativePalettePage_ = (creativePalettePage_ + 1) % pageCount;
            creativePaletteCursor_ = creativePalettePage_ * kCreativePaletteVisibleSlots;
            return;
        }
    }

    int hoveredEntry = -1;
    const int firstVisible = creativePalettePage_ * kCreativePaletteVisibleSlots;
    const int pageEntryCount = std::max(0, std::min(kCreativePaletteVisibleSlots, visibleCount - firstVisible));
    for (int slot = 0; slot < pageEntryCount; ++slot)
    {
        if (CheckCollisionPointRec(mouse, CreativePaletteSlotRect(slot)))
        {
            hoveredEntry = firstVisible + slot;
            creativePaletteCursor_ = hoveredEntry;
            break;
        }
    }

    int hoveredHotbar = -1;
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        if (CheckCollisionPointRec(mouse, CreativePaletteHotbarRect(slot)))
        {
            hoveredHotbar = slot;
            break;
        }
    }
    if (hoveredHotbar >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        selectedHotbarSlot_ = hoveredHotbar;
        player.SetSelectedSlot(hoveredHotbar);
        inventoryCursorSlot_ = hoveredHotbar;
        return;
    }
    if (hoveredHotbar >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
        player.GetInventory().SetSlot(hoveredHotbar, ItemStack {});
        selectedHotbarSlot_ = hoveredHotbar;
        player.SetSelectedSlot(hoveredHotbar);
        inventoryCursorSlot_ = hoveredHotbar;
        SetMessage("Слот " + std::to_string(hoveredHotbar + 1) + " очищен.", 1.2f);
        audio_.PlayPickup();
        return;
    }

    const auto selectEntry = [this, &player, &visibleEntries](int hotbarSlot, bool closeAfterPick)
    {
        if (visibleEntries.empty())
        {
            SetMessage("Поиск ничего не нашел.", 1.4f);
            audio_.PlayDenied();
            return;
        }
        if (creativePaletteCursor_ < 0 || creativePaletteCursor_ >= static_cast<int>(visibleEntries.size()))
        {
            return;
        }
        const CreativePaletteEntry& entry = visibleEntries[static_cast<std::size_t>(creativePaletteCursor_)];
        if (entry.special)
        {
            creativeSpecialMode_ = true;
            creativeSpecialKindIndex_ = static_cast<int>(entry.specialKind);
            inventoryOpen_ = false;
            DisableCursor();
            SetMessage(std::string("Спецблок: ") + CreativePaletteEntryName(entry)
                + ". ПКМ поставить, ЛКМ убрать, Y команда.", 2.5f);
            audio_.PlayPickup();
            return;
        }

        const ItemType item = ItemFromBlock(entry.block);
        if (item == ItemType::None)
        {
            audio_.PlayDenied();
            return;
        }
        const int slot = std::clamp(hotbarSlot, 0, kHotbarSlotCount - 1);
        player.GetInventory().SetSlot(slot, ItemStack { item, 999 });
        selectedHotbarSlot_ = slot;
        player.SetSelectedSlot(slot);
        inventoryCursorSlot_ = slot;
        creativeSpecialMode_ = false;
        SetMessage(std::string("В слот ") + std::to_string(slot + 1) + ": " + ItemDisplayName(item) + ".", 1.4f);
        audio_.PlayPickup();
        if (closeAfterPick)
        {
            inventoryOpen_ = false;
            DisableCursor();
        }
    };

    if (!creativePaletteSearchActive_ && currentInput_.hotbarSlot > 0)
    {
        selectEntry(currentInput_.hotbarSlot - 1, false);
        currentInput_.hotbarSlot = 0;
        return;
    }

    if (std::fabs(currentInput_.mouseWheel) > 0.01f)
    {
        const int direction = currentInput_.mouseWheel > 0.0f ? -1 : 1;
        if (pageCount > 1)
        {
            creativePalettePage_ = (creativePalettePage_ + direction + pageCount) % pageCount;
            creativePaletteCursor_ = creativePalettePage_ * kCreativePaletteVisibleSlots;
        }
        else if (creativePaletteSearch_.empty())
        {
            creativePaletteTab_ = (creativePaletteTab_ + direction + kCreativePaletteCategoryCount) % kCreativePaletteCategoryCount;
            creativePaletteCursor_ = 0;
            creativePalettePage_ = 0;
        }
        currentInput_.mouseWheel = 0.0f;
        return;
    }

    if (IsKeyPressed(KEY_TAB))
    {
        if (pageCount > 1)
        {
            creativePalettePage_ = (creativePalettePage_ + 1) % pageCount;
            creativePaletteCursor_ = creativePalettePage_ * kCreativePaletteVisibleSlots;
        }
        else if (creativePaletteSearch_.empty())
        {
            creativePaletteTab_ = (creativePaletteTab_ + 1) % kCreativePaletteCategoryCount;
            creativePaletteCursor_ = 0;
            creativePalettePage_ = 0;
        }
        return;
    }
    if (IsKeyPressed(KEY_Y))
    {
        creativeSpecialTeam_ = creativeSpecialTeam_ >= 3 ? -1 : creativeSpecialTeam_ + 1;
        SetMessage(std::string("Команда спецблока: ") + TeamDisplayNameForId(creativeSpecialTeam_) + ".", 1.2f);
        return;
    }

    const auto setCursor = [this, visibleCount](int index)
    {
        if (visibleCount <= 0)
        {
            creativePaletteCursor_ = 0;
            creativePalettePage_ = 0;
            return;
        }
        creativePaletteCursor_ = std::clamp(index, 0, visibleCount - 1);
        creativePalettePage_ = creativePaletteCursor_ / kCreativePaletteVisibleSlots;
    };
    if (IsKeyPressed(KEY_PAGE_UP))
    {
        creativePalettePage_ = (creativePalettePage_ + pageCount - 1) % pageCount;
        setCursor(creativePalettePage_ * kCreativePaletteVisibleSlots);
        return;
    }
    if (IsKeyPressed(KEY_PAGE_DOWN))
    {
        creativePalettePage_ = (creativePalettePage_ + 1) % pageCount;
        setCursor(creativePalettePage_ * kCreativePaletteVisibleSlots);
        return;
    }
    if (IsKeyPressed(KEY_HOME))
    {
        setCursor(0);
        return;
    }
    if (IsKeyPressed(KEY_END))
    {
        setCursor(visibleCount - 1);
        return;
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A))
    {
        setCursor(creativePaletteCursor_ - 1);
    }
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D))
    {
        setCursor(creativePaletteCursor_ + 1);
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        setCursor(creativePaletteCursor_ - kCreativePaletteCols);
    }
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        setCursor(creativePaletteCursor_ + kCreativePaletteCols);
    }

    if (hoveredEntry >= 0 && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)))
    {
        selectEntry(selectedHotbarSlot_, IsMouseButtonPressed(MOUSE_BUTTON_RIGHT));
        return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE))
    {
        selectEntry(selectedHotbarSlot_, false);
    }
}

void Game::HandleCreativePaletteInput(Player& player)
{
    HandleCreativePaletteInputPolished(player);
    return;

    creativePaletteTab_ = std::clamp(creativePaletteTab_, 0, kCreativePaletteCategoryCount - 1);
    int entryCount = 0;
    const CreativePaletteEntry* entries = CreativePaletteEntries(creativePaletteTab_, entryCount);
    creativePaletteCursor_ = std::clamp(creativePaletteCursor_, 0, std::max(0, entryCount - 1));

    const Vector2 mouse = GetMousePosition();
    for (int tab = 0; tab < kCreativePaletteCategoryCount; ++tab)
    {
        if (CheckCollisionPointRec(mouse, CreativePaletteTabRect(tab)) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            creativePaletteTab_ = tab;
            creativePaletteCursor_ = 0;
            entries = CreativePaletteEntries(creativePaletteTab_, entryCount);
            break;
        }
    }

    for (int teamButton = 0; teamButton < 5; ++teamButton)
    {
        if (CheckCollisionPointRec(mouse, CreativePaletteTeamRect(teamButton)) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            creativeSpecialTeam_ = teamButton < 4 ? teamButton : -1;
            SetMessage(std::string("Команда спецблока: ") + TeamDisplayNameForId(creativeSpecialTeam_) + ".", 1.2f);
            return;
        }
    }

    int hoveredEntry = -1;
    for (int i = 0; i < entryCount; ++i)
    {
        if (CheckCollisionPointRec(mouse, CreativePaletteSlotRect(i)))
        {
            hoveredEntry = i;
            creativePaletteCursor_ = i;
            break;
        }
    }

    int hoveredHotbar = -1;
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        if (CheckCollisionPointRec(mouse, CreativePaletteHotbarRect(slot)))
        {
            hoveredHotbar = slot;
            break;
        }
    }
    if (hoveredHotbar >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        selectedHotbarSlot_ = hoveredHotbar;
        player.SetSelectedSlot(hoveredHotbar);
        inventoryCursorSlot_ = hoveredHotbar;
        return;
    }

    const auto selectEntry = [this, &player, entries, entryCount](int hotbarSlot, bool closeAfterPick)
    {
        if (creativePaletteCursor_ < 0 || creativePaletteCursor_ >= entryCount)
        {
            return;
        }
        const CreativePaletteEntry& entry = entries[creativePaletteCursor_];
        if (entry.special)
        {
            creativeSpecialMode_ = true;
            creativeSpecialKindIndex_ = static_cast<int>(entry.specialKind);
            inventoryOpen_ = false;
            DisableCursor();
            SetMessage(std::string("Спецблок: ") + CreativePaletteEntryName(entry)
                + ". ПКМ поставить, ЛКМ убрать, Y команда.", 2.5f);
            audio_.PlayPickup();
            return;
        }

        const ItemType item = ItemFromBlock(entry.block);
        if (item == ItemType::None)
        {
            return;
        }
        const int slot = std::clamp(hotbarSlot, 0, kHotbarSlotCount - 1);
        player.GetInventory().SetSlot(slot, ItemStack { item, 999 });
        selectedHotbarSlot_ = slot;
        player.SetSelectedSlot(slot);
        inventoryCursorSlot_ = slot;
        creativeSpecialMode_ = false;
        SetMessage(std::string("В слот ") + std::to_string(slot + 1) + ": " + ItemDisplayName(item) + ".", 1.4f);
        audio_.PlayPickup();
        if (closeAfterPick)
        {
            inventoryOpen_ = false;
            DisableCursor();
        }
    };

    if (currentInput_.hotbarSlot > 0)
    {
        selectEntry(currentInput_.hotbarSlot - 1, false);
        currentInput_.hotbarSlot = 0;
        return;
    }

    if (std::fabs(currentInput_.mouseWheel) > 0.01f)
    {
        const int direction = currentInput_.mouseWheel > 0.0f ? -1 : 1;
        creativePaletteTab_ = (creativePaletteTab_ + direction + kCreativePaletteCategoryCount) % kCreativePaletteCategoryCount;
        creativePaletteCursor_ = 0;
        currentInput_.mouseWheel = 0.0f;
        return;
    }

    if (IsKeyPressed(KEY_TAB))
    {
        creativePaletteTab_ = (creativePaletteTab_ + 1) % kCreativePaletteCategoryCount;
        creativePaletteCursor_ = 0;
        return;
    }
    if (IsKeyPressed(KEY_Y))
    {
        creativeSpecialTeam_ = creativeSpecialTeam_ >= 3 ? -1 : creativeSpecialTeam_ + 1;
        SetMessage(std::string("Команда спецблока: ") + TeamDisplayNameForId(creativeSpecialTeam_) + ".", 1.2f);
        return;
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A))
    {
        creativePaletteCursor_ = std::max(0, creativePaletteCursor_ - 1);
    }
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D))
    {
        creativePaletteCursor_ = std::min(entryCount - 1, creativePaletteCursor_ + 1);
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        creativePaletteCursor_ = std::max(0, creativePaletteCursor_ - kCreativePaletteCols);
    }
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        creativePaletteCursor_ = std::min(entryCount - 1, creativePaletteCursor_ + kCreativePaletteCols);
    }

    if (hoveredEntry >= 0 && (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)))
    {
        selectEntry(selectedHotbarSlot_, IsMouseButtonPressed(MOUSE_BUTTON_RIGHT));
        return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE))
    {
        selectEntry(selectedHotbarSlot_, false);
    }
}

void Game::StartCreativeSession()
{
    // Default canvas: the classic arena as an editable starting point. Its
    // cores/generators/chests/spawns become editable specials right away.
    automatch_.active = false;
    tutorialMode_ = false;
    creativeMode_ = true;
    creativeFlightActive_ = false;
    creativeFlightJumpTapTimer_ = 0.0f;
    creativeTestActive_ = false;
    creativeSpecialMode_ = false;
    creativeMapPath_ = kCreativeMapPath;
    hasCustomMapBuildBounds_ = false;
    creativeSpecialKindIndex_ = 0;
    creativeSpecialTeam_ = 0;
    ResetCreativeEditorTools();
    ResetCreativePaletteUi();
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamId_ = 0;
    selectedTeamSize_ = 1;
    selectedBotCount_ = 0;
    arenaLayout_ = ArenaLayout::Classic;
    arenaBiome_ = ArenaBiome::Arena;
    SetupMatch();
    CaptureCreativeSpecialsFromMatch();
    if (Player* player = GetLocalPlayer())
    {
        GrantCreativePalette(*player);
    }
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);
    UpdateCamera(0.016f);
    screen_ = GameScreen::Playing;
    DisableCursor();
    SetMessage("Креатив: блоки бесконечны. T — спецблоки, ESC — тест и сохранение карты.", 6.0f);
}

void Game::StartCreativeSessionFromDocument(const CreativeMapDocument& doc)
{
    automatch_.active = false;
    tutorialMode_ = false;
    creativeMode_ = true;
    creativeFlightActive_ = false;
    creativeFlightJumpTapTimer_ = 0.0f;
    creativeTestActive_ = false;
    creativeSpecialMode_ = false;
    UpdateCustomMapBuildBounds(doc);
    ResetCreativeEditorTools();
    ResetCreativePaletteUi();
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamSize_ = 1;
    selectedBotCount_ = 0;
    arenaBiome_ = static_cast<ArenaBiome>(doc.biome);
    arenaLayout_ = static_cast<ArenaLayout>(doc.layout);
    pendingCreativeDoc_ = &doc;
    SetupMatch();
    pendingCreativeDoc_ = nullptr;
    creativeSpecials_ = doc.specials;
    creativeRouteNodes_ = doc.routeNodes;
    creativeRouteEdges_ = doc.routeEdges;
    std::vector<std::string> routeErrors;
    if (!routeGraph_.Build(creativeRouteNodes_, creativeRouteEdges_, &routeErrors))
    {
        for (const std::string& error : routeErrors)
        {
            std::cerr << "route graph: " << error << '\n';
        }
    }
    else if (!routeGraph_.Empty() && !routeGraph_.ValidatePhysical(world_, &routeErrors))
    {
        for (const std::string& error : routeErrors)
        {
            std::cerr << "route graph physical validation: " << error << '\n';
        }
        routeGraph_.Clear();
    }
    RefreshCreativeTeamSpawns();
    if (Player* player = GetLocalPlayer())
    {
        GrantCreativePalette(*player);
    }
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);
    UpdateCamera(0.016f);
    screen_ = GameScreen::Playing;
    DisableCursor();
}

void Game::CaptureCreativeSpecialsFromMatch()
{
    // Convert the freshly built arena's gameplay entities into editable
    // specials so the default canvas is already a valid, fully editable map.
    creativeSpecials_.clear();
    creativeRouteNodes_.clear();
    creativeRouteEdges_.clear();
    routeGraph_.Clear();
    for (const EnergyCore& core : matchSimulation_.Cores())
    {
        creativeSpecials_.push_back(CreativeSpecial {
            CreativeSpecialKind::Core, core.GetBlockPosition(), core.GetTeamId() });
    }
    for (const Generator& generator : matchSimulation_.Generators())
    {
        CreativeSpecialKind kind = CreativeSpecialKind::IronGenerator;
        if (generator.GetType() == ResourceType::Gold)
        {
            kind = CreativeSpecialKind::GoldGenerator;
        }
        else if (generator.GetType() == ResourceType::Crystal)
        {
            kind = CreativeSpecialKind::CrystalGenerator;
        }
        creativeSpecials_.push_back(CreativeSpecial {
            kind, world_.WorldToGrid(ToVector3(generator.GetPosition())), generator.GetTeamId() });
    }
    for (const Team& team : teams_)
    {
        if (!IsTeamActiveForMode(team.id))
        {
            continue;
        }
        creativeSpecials_.push_back(CreativeSpecial {
            CreativeSpecialKind::TeamChest, team.teamChestBlock, team.id });
        creativeSpecials_.push_back(CreativeSpecial {
            CreativeSpecialKind::HeroSpawn, world_.WorldToGrid(team.spawnPoint), team.id });
        creativeSpecials_.push_back(CreativeSpecial {
            CreativeSpecialKind::Shop, world_.WorldToGrid(team.shopPosition), team.id });
    }
}

CreativeMapDocument Game::BuildCreativeMapDocument() const
{
    CreativeMapDocument doc;
    doc.biome = static_cast<int>(arenaBiome_);
    doc.layout = static_cast<int>(arenaLayout_);
    for (const auto& [pos, block] : world_.GetBlocks())
    {
        // Core/generator/chest blocks are the specials' visuals — they are
        // rebuilt from the specials list, not stored as plain geometry.
        if (block.type == BlockType::EnergyCoreBlock
            || block.type == BlockType::ResourceGenerator
            || block.type == BlockType::TeamChestBlock)
        {
            continue;
        }
        doc.blocks.push_back(CreativeMapBlock { pos, block.type, block.teamId, block.breakable, block.variant });
    }
    doc.specials = creativeSpecials_;
    doc.routeNodes = creativeRouteNodes_;
    doc.routeEdges = creativeRouteEdges_;
    return doc;
}

void Game::UpdateCustomMapBuildBounds(const CreativeMapDocument& doc)
{
    hasCustomMapBuildBounds_ = true;
    customMapBuildMinY_ = -2;
    customMapBuildMaxY_ = 64;
    for (const CreativeMapBlock& block : doc.blocks)
    {
        customMapBuildMinY_ = std::min(customMapBuildMinY_, block.pos.y - 8);
        customMapBuildMaxY_ = std::max(customMapBuildMaxY_, block.pos.y + 16);
    }
    for (const CreativeSpecial& special : doc.specials)
    {
        customMapBuildMinY_ = std::min(customMapBuildMinY_, special.pos.y - 8);
        customMapBuildMaxY_ = std::max(customMapBuildMaxY_, special.pos.y + 16);
    }
    for (const CreativeRouteNode& node : doc.routeNodes)
    {
        customMapBuildMinY_ = std::min(customMapBuildMinY_, node.pos.y - 8);
        customMapBuildMaxY_ = std::max(customMapBuildMaxY_, node.pos.y + 16);
    }
}

void Game::ApplyPendingCreativeDocToMatch()
{
    // Called from SetupMatch instead of the arena layout + default entity
    // placement when a custom map document is pending.
    const CreativeMapDocument& doc = *pendingCreativeDoc_;
    // Keep metadata-only navigation specials available after SetupMatch; the
    // pending document pointer is intentionally short-lived in automatch.
    creativeSpecials_ = doc.specials;
    creativeRouteNodes_ = doc.routeNodes;
    creativeRouteEdges_ = doc.routeEdges;
    std::vector<std::string> routeErrors;
    if (!routeGraph_.Build(creativeRouteNodes_, creativeRouteEdges_, &routeErrors))
    {
        for (const std::string& error : routeErrors)
        {
            std::cerr << "route graph: " << error << '\n';
        }
    }
    for (const CreativeMapBlock& block : doc.blocks)
    {
        world_.PlaceBlock(block.pos, Block { block.type, block.teamId, block.breakable, block.variant }, true);
    }
    std::array<bool, 4> spawnSet {};
    for (const CreativeSpecial& special : doc.specials)
    {
        const bool teamValid = special.teamId >= 0 && special.teamId < static_cast<int>(teams_.size());
        switch (special.kind)
        {
        case CreativeSpecialKind::Core:
            if (teamValid)
            {
                teams_[special.teamId].coreBlock = special.pos;
                teams_[special.teamId].coreAlive = true;
                world_.PlaceBlock(special.pos, Block { BlockType::EnergyCoreBlock, special.teamId, false }, true);
                matchSimulation_.Cores().emplace_back(special.teamId, special.pos, 120);
            }
            break;
        case CreativeSpecialKind::IronGenerator:
        case CreativeSpecialKind::GoldGenerator:
        case CreativeSpecialKind::CrystalGenerator:
            world_.PlaceBlock(special.pos, Block { BlockType::ResourceGenerator, special.teamId, false }, true);
            matchSimulation_.Generators().emplace_back(
                GeneratorResourceForKind(special.kind),
                ToVec3(world_.GridToWorld(special.pos)),
                GeneratorIntervalForKind(special.kind),
                1,
                special.teamId);
            break;
        case CreativeSpecialKind::HeroSpawn:
            if (teamValid && !spawnSet[special.teamId])
            {
                const Vector3 cell = world_.GridToWorld(special.pos);
                teams_[special.teamId].spawnPoint = Vector3 { cell.x, cell.y + 0.5f, cell.z };
                spawnSet[special.teamId] = true;
            }
            break;
        case CreativeSpecialKind::TeamChest:
            if (teamValid)
            {
                teams_[special.teamId].teamChestBlock = special.pos;
                world_.PlaceBlock(special.pos, Block { BlockType::TeamChestBlock, special.teamId, false }, true);
            }
            break;
        case CreativeSpecialKind::Shop:
            if (teamValid)
            {
                const Vector3 cell = world_.GridToWorld(special.pos);
                // Mirror the stock arenas: the zone anchor floats just above
                // the floor under the marker cell.
                teams_[special.teamId].shopPosition = Vector3 { cell.x, cell.y - 0.42f, cell.z };
            }
            break;
        case CreativeSpecialKind::Rally:
        case CreativeSpecialKind::Lane:
        case CreativeSpecialKind::Chokepoint:
        case CreativeSpecialKind::Highground:
            break; // consumed by bot routing, not instantiated in the world
        }
    }
    if (!routeGraph_.Empty())
    {
        routeErrors.clear();
        if (!routeGraph_.ValidatePhysical(world_, &routeErrors))
        {
            for (const std::string& error : routeErrors)
            {
                std::cerr << "route graph physical validation: " << error << '\n';
            }
            routeGraph_.Clear();
        }
    }
}

bool Game::TeamPlayableForSetup(int teamId) const
{
    // Custom maps define team activity by their cores; stock matches by mode.
    if (pendingCreativeDoc_ != nullptr)
    {
        return pendingCreativeDoc_->TeamHasCore(teamId);
    }
    return IsTeamActiveForMode(teamId);
}

void Game::RefreshCreativeTeamSpawns()
{
    // The first spawn special per team drives the live spawn point; the
    // document keeps every marker for future multi-spawn support.
    for (Team& team : teams_)
    {
        for (const CreativeSpecial& special : creativeSpecials_)
        {
            if (special.kind == CreativeSpecialKind::HeroSpawn && special.teamId == team.id)
            {
                const Vector3 cell = world_.GridToWorld(special.pos);
                team.spawnPoint = Vector3 { cell.x, cell.y + 0.5f, cell.z };
                break;
            }
        }
    }
}

bool Game::PlaceCreativeSpecialAt(const GridPos& pos, CreativeSpecialKind kind, int teamId)
{
    if (std::abs(pos.x) > kCreativeBuildRadius || std::abs(pos.z) > kCreativeBuildRadius
        || pos.y < kCreativeBuildMinY || pos.y > kCreativeBuildMaxY)
    {
        SetMessage("Слишком далеко от карты.", 1.8f);
        return false;
    }
    if (!IsGeneratorKind(kind) && (teamId < 0 || teamId > 3))
    {
        SetMessage(std::string(DisplayName(kind)) + ": выберите команду (Y).", 2.2f);
        return false;
    }
    const bool needsBlock = CreativeSpecialHasWorldBlock(kind);
    if (needsBlock && world_.GetBlock(pos) != nullptr)
    {
        SetMessage("Клетка занята.", 1.6f);
        return false;
    }
    for (const CreativeSpecial& existing : creativeSpecials_)
    {
        if (existing.pos == pos)
        {
            SetMessage("Здесь уже есть спецблок.", 1.6f);
            return false;
        }
    }

    const bool recordHistory = creativeMode_ && !creativeRestoringHistory_;
    const CreativeMapDocument before = recordHistory ? BuildCreativeMapDocument() : CreativeMapDocument {};

    // A team has exactly one core, chest and shop: placing a new one moves it.
    if (kind == CreativeSpecialKind::Core || kind == CreativeSpecialKind::TeamChest
        || kind == CreativeSpecialKind::Shop)
    {
        for (std::size_t i = 0; i < creativeSpecials_.size(); ++i)
        {
            if (creativeSpecials_[i].kind == kind && creativeSpecials_[i].teamId == teamId)
            {
                RemoveCreativeSpecialByIndex(i);
                break;
            }
        }
    }

    creativeSpecials_.push_back(CreativeSpecial { kind, pos, teamId });
    const bool teamValid = teamId >= 0 && teamId < static_cast<int>(teams_.size());
    switch (kind)
    {
    case CreativeSpecialKind::Core:
        world_.PlaceBlock(pos, Block { BlockType::EnergyCoreBlock, teamId, false }, true);
        matchSimulation_.Cores().emplace_back(teamId, pos, 120);
        if (teamValid)
        {
            teams_[teamId].coreBlock = pos;
            teams_[teamId].coreAlive = true;
        }
        break;
    case CreativeSpecialKind::IronGenerator:
    case CreativeSpecialKind::GoldGenerator:
    case CreativeSpecialKind::CrystalGenerator:
        world_.PlaceBlock(pos, Block { BlockType::ResourceGenerator, teamId, false }, true);
        matchSimulation_.Generators().emplace_back(
            GeneratorResourceForKind(kind),
            ToVec3(world_.GridToWorld(pos)),
            GeneratorIntervalForKind(kind),
            1,
            teamId);
        break;
    case CreativeSpecialKind::HeroSpawn:
        RefreshCreativeTeamSpawns();
        break;
    case CreativeSpecialKind::TeamChest:
        world_.PlaceBlock(pos, Block { BlockType::TeamChestBlock, teamId, false }, true);
        if (teamValid)
        {
            teams_[teamId].teamChestBlock = pos;
        }
        break;
    case CreativeSpecialKind::Shop:
        if (teamValid)
        {
            const Vector3 cell = world_.GridToWorld(pos);
            teams_[teamId].shopPosition = Vector3 { cell.x, cell.y - 0.42f, cell.z };
        }
        break;
    case CreativeSpecialKind::Rally:
    case CreativeSpecialKind::Lane:
    case CreativeSpecialKind::Chokepoint:
    case CreativeSpecialKind::Highground:
        break; // metadata-only navigation marker
    }
    SetMessage(std::string("Поставлено: ") + DisplayName(kind)
        + " (" + TeamDisplayNameForId(teamId) + ").", 2.0f);
    if (recordHistory)
    {
        PushCreativeHistory(std::string("спецблок ") + DisplayName(kind), before);
    }
    return true;
}

void Game::RemoveCreativeSpecialByIndex(std::size_t index)
{
    if (index >= creativeSpecials_.size())
    {
        return;
    }
    const CreativeSpecial special = creativeSpecials_[index];
    creativeSpecials_.erase(creativeSpecials_.begin() + static_cast<std::ptrdiff_t>(index));
    switch (special.kind)
    {
    case CreativeSpecialKind::Core:
    {
        world_.RemoveBlock(special.pos);
        auto& cores = matchSimulation_.Cores();
        cores.erase(std::remove_if(cores.begin(), cores.end(), [&special](const EnergyCore& core)
        {
            return core.GetBlockPosition() == special.pos;
        }), cores.end());
        break;
    }
    case CreativeSpecialKind::IronGenerator:
    case CreativeSpecialKind::GoldGenerator:
    case CreativeSpecialKind::CrystalGenerator:
    {
        world_.RemoveBlock(special.pos);
        auto& generators = matchSimulation_.Generators();
        generators.erase(std::remove_if(generators.begin(), generators.end(),
            [this, &special](const Generator& generator)
        {
            return world_.WorldToGrid(ToVector3(generator.GetPosition())) == special.pos;
        }), generators.end());
        break;
    }
    case CreativeSpecialKind::HeroSpawn:
        RefreshCreativeTeamSpawns();
        break;
    case CreativeSpecialKind::TeamChest:
        world_.RemoveBlock(special.pos);
        break;
    case CreativeSpecialKind::Shop:
        break; // zone marker only; the position simply stops being exported
    case CreativeSpecialKind::Rally:
    case CreativeSpecialKind::Lane:
    case CreativeSpecialKind::Chokepoint:
    case CreativeSpecialKind::Highground:
        break; // metadata-only navigation marker
    }
}

bool Game::RemoveCreativeSpecialAlongAim(const Player& player)
{
    (void)player;
    // Ray-pick the specials list directly (spawn markers have no world block,
    // so a world raycast alone cannot select them).
    const Vector3 origin = cameraController_.GetAimOrigin();
    const Vector3 dir = cameraController_.GetAimDirection();
    int bestIndex = -1;
    float bestT = kCreativeReach + 1.0f;
    for (std::size_t i = 0; i < creativeSpecials_.size(); ++i)
    {
        const Vector3 center = world_.GridToWorld(creativeSpecials_[i].pos);
        const float toX = center.x - origin.x;
        const float toY = center.y - origin.y;
        const float toZ = center.z - origin.z;
        const float t = toX * dir.x + toY * dir.y + toZ * dir.z;
        if (t < 0.4f || t > kCreativeReach)
        {
            continue;
        }
        const float closestX = origin.x + dir.x * t - center.x;
        const float closestY = origin.y + dir.y * t - center.y;
        const float closestZ = origin.z + dir.z * t - center.z;
        const float distSq = closestX * closestX + closestY * closestY + closestZ * closestZ;
        if (distSq < 0.55f * 0.55f && t < bestT)
        {
            bestT = t;
            bestIndex = static_cast<int>(i);
        }
    }
    if (bestIndex < 0)
    {
        SetMessage("Наведитесь на спецблок, чтобы убрать его.", 1.6f);
        return false;
    }
    const bool recordHistory = creativeMode_ && !creativeRestoringHistory_;
    const CreativeMapDocument before = recordHistory ? BuildCreativeMapDocument() : CreativeMapDocument {};
    const std::string name = DisplayName(creativeSpecials_[static_cast<std::size_t>(bestIndex)].kind);
    RemoveCreativeSpecialByIndex(static_cast<std::size_t>(bestIndex));
    SetMessage("Убрано: " + name + ".", 1.8f);
    if (recordHistory)
    {
        PushCreativeHistory("удаление спецблока " + name, before);
    }
    return true;
}

void Game::HandleCreativeModeInput(Player& player)
{
    const bool editorToolConsumed = HandleCreativeEditorTools(player);
    if (editorToolConsumed)
    {
        return;
    }

    if (IsKeyPressed(KEY_T))
    {
        creativeSpecialMode_ = !creativeSpecialMode_;
        SetMessage(creativeSpecialMode_
            ? "Спецблоки: 1-6 выбор, Y команда, ПКМ поставить, ЛКМ убрать."
            : "Режим блоков: стройте из палитры хотбара.", 3.0f);
    }
    if (!creativeSpecialMode_)
    {
        return;
    }

    if (currentInput_.hotbarSlot >= 1 && currentInput_.hotbarSlot <= kCreativeSpecialKindCount)
    {
        creativeSpecialKindIndex_ = currentInput_.hotbarSlot - 1;
        SetMessage(std::string("Спецблок: ")
            + DisplayName(static_cast<CreativeSpecialKind>(creativeSpecialKindIndex_)) + ".", 1.4f);
    }
    currentInput_.hotbarSlot = 0; // special mode owns the number keys
    if (std::fabs(currentInput_.mouseWheel) > 0.01f)
    {
        const int direction = currentInput_.mouseWheel > 0.0f ? -1 : 1;
        creativeSpecialKindIndex_ =
            (creativeSpecialKindIndex_ + direction + kCreativeSpecialKindCount) % kCreativeSpecialKindCount;
        currentInput_.mouseWheel = 0.0f;
        SetMessage(std::string("Спецблок: ")
            + DisplayName(static_cast<CreativeSpecialKind>(creativeSpecialKindIndex_)) + ".", 1.2f);
    }
    if (IsKeyPressed(KEY_Y))
    {
        // 0 → 1 → 2 → 3 → нейтральный (-1) → 0 ...
        creativeSpecialTeam_ = creativeSpecialTeam_ >= 3 ? -1 : creativeSpecialTeam_ + 1;
        SetMessage(std::string("Команда спецблока: ") + TeamDisplayNameForId(creativeSpecialTeam_) + ".", 1.6f);
    }

    if (currentInput_.placePressed)
    {
        const std::optional<RaycastHit> hit = RaycastFromAim(player, kCreativeReach);
        if (hit.has_value())
        {
            PlaceCreativeSpecialAt(
                hit->adjacent,
                static_cast<CreativeSpecialKind>(creativeSpecialKindIndex_),
                creativeSpecialTeam_);
        }
        else
        {
            SetMessage("Наведитесь на блок — спецблок ставится рядом с ним.", 1.8f);
        }
    }
    if (currentInput_.attackPressed)
    {
        RemoveCreativeSpecialAlongAim(player);
    }

    // The editor owns the mouse while the special palette is open: the normal
    // attack/break/place pipeline must not also fire (same UI-gate idea as the
    // shop overlay, applied at the input source).
    currentInput_.attackPressed = false;
    currentInput_.attackHeld = false;
    currentInput_.attackReleased = false;
    currentInput_.placePressed = false;
    currentInput_.placeHeld = false;
}

void Game::RenderCreativeMarkersScene() const
{
    // Wire markers over every special so the map's logic layer is visible while
    // editing. Spawn markers additionally get a pillar (they have no block).
    for (const CreativeSpecial& special : creativeSpecials_)
    {
        const Vector3 center = world_.GridToWorld(special.pos);
        const Color color = TeamColorForId(special.teamId);
        if (special.kind == CreativeSpecialKind::HeroSpawn || special.kind == CreativeSpecialKind::Shop)
        {
            DrawCubeWires(center, 0.92f, 0.92f, 0.92f, color);
            const float pillar = special.kind == CreativeSpecialKind::HeroSpawn ? 1.7f : 0.9f;
            DrawCube(Vector3 { center.x, center.y + pillar * 0.5f, center.z }, 0.16f, pillar, 0.16f, Fade(color, 0.4f));
        }
        else
        {
            DrawCubeWires(center, 1.08f, 1.08f, 1.08f, Fade(color, 0.9f));
        }
    }
    RenderCreativeSelectionScene();
}

void Game::RenderCreativeSelectionScene() const
{
    const Color aColor { 112, 232, 255, 255 };
    const Color bColor { 255, 235, 142, 255 };
    if (creativeSelectionA_.has_value())
    {
        DrawCubeWires(world_.GridToWorld(*creativeSelectionA_), 1.14f, 1.14f, 1.14f, aColor);
    }
    if (creativeSelectionB_.has_value())
    {
        DrawCubeWires(world_.GridToWorld(*creativeSelectionB_), 1.14f, 1.14f, 1.14f, bColor);
    }
    const CreativeSelectionBounds bounds = MakeCreativeSelectionBounds(creativeSelectionA_, creativeSelectionB_);
    if (!bounds.valid)
    {
        return;
    }
    const Vector3 center {
        (static_cast<float>(bounds.min.x) + static_cast<float>(bounds.max.x)) * 0.5f,
        (static_cast<float>(bounds.min.y) + static_cast<float>(bounds.max.y)) * 0.5f,
        (static_cast<float>(bounds.min.z) + static_cast<float>(bounds.max.z)) * 0.5f
    };
    DrawCubeWires(
        center,
        static_cast<float>(bounds.max.x - bounds.min.x + 1),
        static_cast<float>(bounds.max.y - bounds.min.y + 1),
        static_cast<float>(bounds.max.z - bounds.min.z + 1),
        Color { 128, 238, 166, 255 });
}

void Game::RenderCreativeValidationOverlay() const
{
    if (!creativeValidationVisible_)
    {
        return;
    }
    const std::vector<std::string> issues = BuildCreativeValidationIssues();
    const int width = 430;
    const int visibleRows = std::min(issues.empty() ? 1 : static_cast<int>(issues.size()), 7);
    const int height = 70 + visibleRows * 22;
    const int x = 14;
    const int y = 88;
    const Color gold { 255, 235, 142, 255 };
    const Color ok { 128, 238, 166, 255 };
    const Color warn { 255, 170, 112, 255 };
    DrawRectangle(x, y, width, height, Fade(BLACK, 0.58f));
    DrawRectangleLines(x, y, width, height, Fade(issues.empty() ? ok : warn, 0.72f));
    DrawText("Проверка карты", x + 12, y + 10, 18, gold);
    if (issues.empty())
    {
        DrawText("Ошибок нет: карту можно тестировать.", x + 12, y + 42, 15, ok);
        return;
    }
    for (int i = 0; i < visibleRows; ++i)
    {
        const std::string row = "• " + issues[static_cast<std::size_t>(i)];
        DrawText(row.c_str(), x + 12, y + 40 + i * 22, 14, Fade(WHITE, 0.88f));
    }
    if (static_cast<int>(issues.size()) > visibleRows)
    {
        const std::string rest = "+" + std::to_string(static_cast<int>(issues.size()) - visibleRows) + " ещё";
        DrawText(rest.c_str(), x + 12, y + 40 + visibleRows * 22, 14, Fade(WHITE, 0.66f));
    }
}

void Game::RenderCreativeOverlayPolished() const
{
    const Player* player = GetLocalPlayer();
    if (player == nullptr)
    {
        return;
    }

    const Color gold { 255, 235, 142, 255 };
    const Color cyan { 112, 232, 255, 255 };
    const Color ok { 128, 238, 166, 255 };
    const Color warn { 255, 170, 112, 255 };
    const Color textBright { 236, 240, 246, 255 };
    const int width = 420;
    const int height = creativeSpecialMode_ ? 338 : 286;
    const int x = GetScreenWidth() - width - 14;
    const int y = 88;
    DrawRectangle(x, y, width, height, Fade(BLACK, 0.52f));
    DrawRectangleLines(x, y, width, height, Fade(creativeSpecialMode_ ? cyan : gold, 0.58f));

    DrawText("КРЕАТИВ", x + 14, y + 10, 18, creativeSpecialMode_ ? cyan : gold);
    DrawText(creativeSpecialMode_ ? "режим спецблоков" : "редактор карты", x + 122, y + 12, 15, Fade(textBright, 0.72f));

    const ItemStack held = GetSelectedHotbarStack(*player);
    std::string selectedLabel = "Пустой слот";
    Color selectedColor = Fade(WHITE, 0.40f);
    if (creativeSpecialMode_)
    {
        const CreativeSpecialKind kind = static_cast<CreativeSpecialKind>(creativeSpecialKindIndex_);
        selectedLabel = DisplayName(kind);
        selectedColor = CreativeSpecialColor(kind, creativeSpecialTeam_);
    }
    else if (!held.IsEmpty())
    {
        selectedLabel = ItemDisplayName(held.type);
        if (const std::optional<BlockType> block = ItemToBlock(held.type))
        {
            selectedColor = CreativePaletteBlockColor(*block);
        }
    }
    DrawRectangle(x + 14, y + 39, 42, 42, Fade(Color { 24, 28, 34, 255 }, 0.92f));
    DrawRectangle(x + 22, y + 46, 26, 24, selectedColor);
    DrawRectangleLines(x + 14, y + 39, 42, 42, Fade(WHITE, 0.22f));
    const std::string selectedLabelClipped = ClipTextToPixelWidth(selectedLabel, width - 88, 16);
    DrawText(selectedLabelClipped.c_str(), x + 66, y + 43, 16, textBright);
    const std::string slotLine = creativeSpecialMode_
        ? std::string("Команда: ") + TeamDisplayNameForId(creativeSpecialTeam_)
        : "Слот " + std::to_string(selectedHotbarSlot_ + 1) + "  |  E: палитра";
    DrawText(slotLine.c_str(), x + 66, y + 64, 13,
             creativeSpecialMode_ && creativeSpecialTeam_ >= 0 ? TeamColorForId(creativeSpecialTeam_) : Fade(textBright, 0.66f));

    const Vector3 pos = player->GetPosition();
    const std::string posLine = "Позиция: "
        + std::to_string(static_cast<int>(std::round(pos.x))) + ", "
        + std::to_string(static_cast<int>(std::round(pos.y))) + ", "
        + std::to_string(static_cast<int>(std::round(pos.z)));
    DrawText(posLine.c_str(), x + 14, y + 94, 14, Fade(textBright, 0.76f));
    DrawText(creativeFlightActive_ ? "Полёт: включен" : "Полёт: выключен",
             x + 250, y + 94, 14, creativeFlightActive_ ? cyan : Fade(textBright, 0.58f));

    const std::optional<RaycastHit> hit = RaycastFromAim(*player, kCreativeReach);
    if (hit.has_value())
    {
        const std::string targetLine = std::string("Цель: ") + DisplayName(hit->blockData.type)
            + " @ " + GridPosText(hit->block)
            + " (" + FormatMeters(hit->distance) + ")";
        const std::string clipped = ClipTextToPixelWidth(targetLine, width - 28, 13);
        DrawText(clipped.c_str(), x + 14, y + 118, 13, Fade(textBright, 0.78f));
    }
    else
    {
        DrawText("Цель: нет блока в радиусе креатива", x + 14, y + 118, 13, Fade(textBright, 0.52f));
    }

    const CreativeSelectionBounds bounds = MakeCreativeSelectionBounds(creativeSelectionA_, creativeSelectionB_);
    if (bounds.valid)
    {
        const int sx = bounds.max.x - bounds.min.x + 1;
        const int sy = bounds.max.y - bounds.min.y + 1;
        const int sz = bounds.max.z - bounds.min.z + 1;
        const std::string selectionLine = "Область: " + std::to_string(sx) + "x"
            + std::to_string(sy) + "x" + std::to_string(sz)
            + " = " + std::to_string(bounds.volume);
        DrawText(selectionLine.c_str(), x + 14, y + 142, 14,
                 bounds.volume > kCreativeAreaMaxBlocks ? warn : ok);
    }
    else
    {
        std::string selectionLine = "Область: ";
        selectionLine += creativeSelectionA_.has_value() ? "A задан" : "A нет";
        selectionLine += " / ";
        selectionLine += creativeSelectionB_.has_value() ? "B задан" : "B нет";
        DrawText(selectionLine.c_str(), x + 14, y + 142, 14, Fade(textBright, 0.62f));
    }

    const std::vector<std::string> issues = BuildCreativeValidationIssues();
    const std::string validationLine = issues.empty()
        ? "Проверка карты: ok"
        : "Проверка карты: " + std::to_string(static_cast<int>(issues.size())) + " замеч.";
    DrawText(validationLine.c_str(), x + 250, y + 142, 14, issues.empty() ? ok : warn);

    const std::string undoLine = creativeUndoStack_.empty()
        ? "Undo: нет"
        : "Undo: " + creativeUndoStack_.back().label;
    const std::string redoLine = creativeRedoStack_.empty()
        ? "Redo: нет"
        : "Redo: " + creativeRedoStack_.back().label;
    const std::string undoClipped = ClipTextToPixelWidth(undoLine, width - 28, 13);
    const std::string redoClipped = ClipTextToPixelWidth(redoLine, width - 28, 13);
    DrawText(undoClipped.c_str(), x + 14, y + 168, 13, Fade(textBright, creativeUndoStack_.empty() ? 0.46f : 0.76f));
    DrawText(redoClipped.c_str(), x + 14, y + 188, 13, Fade(textBright, creativeRedoStack_.empty() ? 0.46f : 0.76f));

    if (!creativeSpecialMode_)
    {
        DrawText("ПКМ ставить, ЛКМ ломать, MMB взять блок", x + 14, y + 216, 14, Fade(textBright, 0.78f));
        DrawText("X/C углы, F заливка, R замена, Delete очистка, G сброс", x + 14, y + 238, 13, Fade(textBright, 0.78f));
        DrawText("Ctrl+Z/Y история, V проверка, T спецблоки, ESC тест/карта", x + 14, y + 260, 14, Fade(textBright, 0.78f));
        return;
    }

    for (int i = 0; i < kCreativeSpecialKindCount; ++i)
    {
        const bool selected = i == creativeSpecialKindIndex_;
        const std::string row = std::to_string(i + 1) + ". "
            + DisplayName(static_cast<CreativeSpecialKind>(i));
        DrawText(row.c_str(), x + 14, y + 214 + i * 18, 14,
                 selected ? gold : Fade(textBright, 0.66f));
    }
    DrawText("ПКМ поставить, ЛКМ убрать, Y команда, T вернуться к блокам",
             x + 14, y + 316, 13, Fade(textBright, 0.78f));
}

void Game::RenderCreativeOverlay() const
{
    RenderCreativeOverlayPolished();
    return;

    const Color gold { 255, 235, 142, 255 };
    const Color textBright { 236, 240, 246, 255 };
    const int width = 372;
    const int x = GetScreenWidth() - width - 14;
    const int height = creativeSpecialMode_ ? 286 : 162;
    const int y = 88;
    DrawRectangle(x, y, width, height, Fade(BLACK, 0.48f));
    DrawRectangleLines(x, y, width, height, Fade(gold, 0.55f));
    DrawText("КРЕАТИВ — редактор карт", x + 12, y + 10, 18, gold);
    if (!creativeSpecialMode_)
    {
        DrawText("ПКМ — ставить блоки, ЛКМ — ломать", x + 12, y + 40, 15, Fade(textBright, 0.85f));
        DrawText("Двойной Space — полет, E — палитра", x + 12, y + 62, 15, Fade(textBright, 0.85f));
        DrawText("X/C — углы, F/R/Delete — область", x + 12, y + 84, 15, Fade(textBright, 0.85f));
        DrawText("Ctrl+Z/Y — откат/повтор, V — проверка", x + 12, y + 106, 15, Fade(textBright, 0.85f));
        DrawText("T — спецблоки, ESC — тест/карта", x + 12, y + 128, 15, Fade(textBright, 0.85f));
        return;
    }
    for (int i = 0; i < kCreativeSpecialKindCount; ++i)
    {
        const bool selected = i == creativeSpecialKindIndex_;
        const std::string row = std::to_string(i + 1) + ". "
            + DisplayName(static_cast<CreativeSpecialKind>(i));
        DrawText(row.c_str(), x + 12, y + 38 + i * 24, 16,
                 selected ? gold : Fade(textBright, 0.72f));
    }
    const std::string teamRow = std::string("Y — команда: ") + TeamDisplayNameForId(creativeSpecialTeam_);
    const Color teamColor = creativeSpecialTeam_ >= 0
        ? TeamColorForId(creativeSpecialTeam_)
        : Fade(textBright, 0.85f);
    DrawText(teamRow.c_str(), x + 12, y + 210, 16, teamColor);
    DrawText("ПКМ — поставить, ЛКМ — убрать", x + 12, y + 234, 15, Fade(textBright, 0.85f));
    DrawText("T — вернуться к блокам", x + 12, y + 256, 15, Fade(textBright, 0.85f));
}

void Game::RenderCreativePaletteOverlayPolished() const
{
    const Player* player = GetLocalPlayer();
    if (player == nullptr)
    {
        return;
    }

    const int activeTab = std::clamp(creativePaletteTab_, 0, kCreativePaletteCategoryCount - 1);
    const std::vector<CreativePaletteEntry> visibleEntries =
        CreativePaletteVisibleEntries(activeTab, creativePaletteSearch_);
    const int visibleCount = static_cast<int>(visibleEntries.size());
    const int pageCount = std::max(1, (visibleCount + kCreativePaletteVisibleSlots - 1) / kCreativePaletteVisibleSlots);
    const int page = std::clamp(creativePalettePage_, 0, pageCount - 1);
    const int firstVisible = page * kCreativePaletteVisibleSlots;
    const Rectangle panel = CreativePalettePanelRect();
    const Rectangle searchRect = CreativePaletteSearchRect();
    const Vector2 mouse = GetMousePosition();
    const Color panelBg { 16, 18, 22, 255 };
    const Color slotBg { 48, 50, 56, 255 };
    const Color slotEmpty { 30, 32, 38, 255 };
    const Color selected { 255, 235, 142, 255 };
    const Color cyan { 112, 232, 255, 255 };
    const Color textDim = Fade(WHITE, 0.62f);

    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.42f));
    DrawRectangleRec(panel, Fade(panelBg, 0.97f));
    DrawRectangleLinesEx(panel, 2.0f, Fade(WHITE, 0.30f));
    DrawText("Креатив", static_cast<int>(panel.x + 24.0f), static_cast<int>(panel.y + 17.0f), 24, WHITE);

    const bool searching = !creativePaletteSearch_.empty();
    for (int tab = 0; tab < kCreativePaletteCategoryCount; ++tab)
    {
        const Rectangle tabRect = CreativePaletteTabRect(tab);
        const bool active = !searching && tab == activeTab;
        const bool hovered = CheckCollisionPointRec(mouse, tabRect);
        DrawRectangleRec(tabRect, Fade(active ? Color { 82, 86, 96, 255 } : Color { 42, 44, 50, 255 }, hovered ? 0.98f : 0.84f));
        DrawRectangleLinesEx(tabRect, active ? 2.0f : 1.0f, Fade(active ? selected : WHITE, active ? 0.90f : 0.22f));
        DrawText(CreativePaletteCategoryName(tab), static_cast<int>(tabRect.x + 10.0f), static_cast<int>(tabRect.y + 7.0f), 14,
                 active ? selected : Fade(WHITE, 0.78f));
    }

    const char* teamLabels[] { "R", "B", "G", "Y", "N" };
    for (int teamButton = 0; teamButton < 5; ++teamButton)
    {
        const int teamId = teamButton < 4 ? teamButton : -1;
        const bool active = teamId == creativeSpecialTeam_;
        const Rectangle rect = CreativePaletteTeamRect(teamButton);
        const Color teamColor = teamId >= 0 ? TeamColorForId(teamId) : Fade(WHITE, 0.78f);
        DrawRectangleRec(rect, Fade(active ? teamColor : Color { 38, 40, 46, 255 }, active ? 0.55f : 0.90f));
        DrawRectangleLinesEx(rect, active ? 2.0f : 1.0f, Fade(active ? selected : teamColor, active ? 0.95f : 0.42f));
        DrawText(teamLabels[teamButton], static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 4.0f), 14, WHITE);
    }
    DrawText("команда", static_cast<int>(panel.x + panel.width - 78.0f), static_cast<int>(panel.y + 20.0f), 12, textDim);

    DrawRectangleRec(searchRect, Fade(Color { 28, 30, 36, 255 }, 0.95f));
    DrawRectangleLinesEx(searchRect, creativePaletteSearchActive_ ? 2.0f : 1.0f,
                         creativePaletteSearchActive_ ? cyan : Fade(WHITE, 0.24f));
    std::string searchText = creativePaletteSearch_.empty()
        ? "/ или Ctrl+F: поиск по всем блокам"
        : std::string("Поиск: ") + creativePaletteSearch_;
    if (creativePaletteSearchActive_ && std::fmod(GetTime(), 1.0) < 0.55)
    {
        searchText += "|";
    }
    DrawText(searchText.c_str(), static_cast<int>(searchRect.x + 12.0f),
             static_cast<int>(searchRect.y + 7.0f), 14,
             creativePaletteSearch_.empty() ? textDim : WHITE);
    if (searching)
    {
        const std::string countText = std::to_string(visibleCount) + " найдено";
        DrawText(countText.c_str(),
                 static_cast<int>(searchRect.x + searchRect.width - MeasureText(countText.c_str(), 12) - 10.0f),
                 static_cast<int>(searchRect.y + 8.0f), 12, Fade(selected, 0.88f));
    }

    const auto drawEntryIcon = [&](const CreativePaletteEntry& entry, Rectangle rect)
    {
        const Color iconColor = entry.special
            ? CreativeSpecialColor(entry.specialKind, creativeSpecialTeam_)
            : CreativePaletteBlockColor(entry.block);
        const Rectangle icon {
            rect.x + 13.0f,
            rect.y + 8.0f,
            rect.width - 26.0f,
            rect.height - 24.0f
        };
        DrawRectangleRec(icon, iconColor);
        DrawRectangleLinesEx(icon, 1.0f, Fade(WHITE, 0.44f));
        if (entry.special)
        {
            DrawCircle(static_cast<int>(icon.x + icon.width * 0.5f),
                       static_cast<int>(icon.y + icon.height * 0.5f),
                       icon.width * 0.24f, Fade(WHITE, 0.28f));
        }
        const char* label = CreativePaletteEntryShortLabel(entry);
        const int labelSize = 9;
        DrawText(label,
                 static_cast<int>(rect.x + rect.width * 0.5f) - MeasureText(label, labelSize) / 2,
                 static_cast<int>(rect.y + rect.height - 13.0f),
                 labelSize,
                 Fade(WHITE, 0.88f));
    };

    for (int slot = 0; slot < kCreativePaletteVisibleSlots; ++slot)
    {
        const Rectangle rect = CreativePaletteSlotRect(slot);
        const int entryIndex = firstVisible + slot;
        const bool hasEntry = entryIndex < visibleCount;
        const bool cursor = hasEntry && entryIndex == creativePaletteCursor_;
        const bool hovered = CheckCollisionPointRec(mouse, rect);
        DrawRectangleRec(rect, Fade(hasEntry ? slotBg : slotEmpty, hovered ? 0.98f : 0.88f));
        DrawRectangle(static_cast<int>(rect.x + 4.0f), static_cast<int>(rect.y + 4.0f),
                      static_cast<int>(rect.width - 8.0f), static_cast<int>(rect.height - 8.0f),
                      Fade(WHITE, hasEntry ? 0.07f : 0.03f));
        DrawRectangleLinesEx(rect, cursor ? 3.0f : 1.0f, cursor ? selected : Fade(WHITE, hovered ? 0.42f : 0.16f));
        if (hasEntry)
        {
            drawEntryIcon(visibleEntries[static_cast<std::size_t>(entryIndex)], rect);
        }
    }

    const int infoY = static_cast<int>(panel.y + 314.0f);
    if (visibleCount > 0 && creativePaletteCursor_ >= 0 && creativePaletteCursor_ < visibleCount)
    {
        const CreativePaletteEntry& entry = visibleEntries[static_cast<std::size_t>(creativePaletteCursor_)];
        const std::string title = CreativePaletteEntryName(entry);
        DrawText(title.c_str(), static_cast<int>(panel.x + 24.0f), infoY, 16, selected);
        DrawText(CreativePaletteEntryHint(entry), static_cast<int>(panel.x + 24.0f), infoY + 22, 12, Fade(WHITE, 0.70f));
    }
    else
    {
        DrawText("Нет результатов. Очистите поиск или выберите другую вкладку.",
                 static_cast<int>(panel.x + 24.0f), infoY, 15, Fade(WHITE, 0.76f));
    }

    if (pageCount > 1)
    {
        const Rectangle prev = CreativePalettePrevPageRect();
        const Rectangle next = CreativePaletteNextPageRect();
        DrawRectangleRec(prev, Fade(Color { 40, 44, 52, 255 }, CheckCollisionPointRec(mouse, prev) ? 0.98f : 0.82f));
        DrawRectangleRec(next, Fade(Color { 40, 44, 52, 255 }, CheckCollisionPointRec(mouse, next) ? 0.98f : 0.82f));
        DrawRectangleLinesEx(prev, 1.0f, Fade(WHITE, 0.28f));
        DrawRectangleLinesEx(next, 1.0f, Fade(WHITE, 0.28f));
        DrawText("<", static_cast<int>(prev.x + 10.0f), static_cast<int>(prev.y + 3.0f), 16, WHITE);
        DrawText(">", static_cast<int>(next.x + 10.0f), static_cast<int>(next.y + 3.0f), 16, WHITE);
        const std::string pageText = std::to_string(page + 1) + "/" + std::to_string(pageCount);
        DrawText(pageText.c_str(), static_cast<int>(panel.x + panel.width - 158.0f), infoY + 5, 13, textDim);
    }

    DrawText("ЛКМ: в слот | ПКМ: выбрать и закрыть | 1-9: назначить | ПКМ по слоту: очистить",
             static_cast<int>(panel.x + 24.0f), static_cast<int>(panel.y + 354.0f), 12, textDim);

    const Inventory& inventory = player->GetInventory();
    const auto& hotbar = inventory.GetHotbarSlots();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        const Rectangle rect = CreativePaletteHotbarRect(slot);
        const bool active = slot == selectedHotbarSlot_;
        const bool hovered = CheckCollisionPointRec(mouse, rect);
        DrawRectangleRec(rect, Fade(Color { 26, 28, 34, 255 }, hovered ? 0.96f : 0.84f));
        DrawRectangleLinesEx(rect, active ? 3.0f : 1.0f, active ? cyan : Fade(WHITE, hovered ? 0.42f : 0.20f));
        const ItemStack& stack = hotbar[slot];
        if (!stack.IsEmpty())
        {
            Color iconColor = Color { 188, 198, 210, 255 };
            if (const std::optional<BlockType> block = ItemToBlock(stack.type))
            {
                iconColor = CreativePaletteBlockColor(*block);
            }
            const Rectangle icon { rect.x + 13.0f, rect.y + 8.0f, rect.width - 26.0f, rect.height - 24.0f };
            DrawRectangleRec(icon, iconColor);
            DrawRectangleLinesEx(icon, 1.0f, Fade(WHITE, 0.42f));
            if (stack.count > 0)
            {
                const std::string count = std::to_string(stack.count);
                DrawText(count.c_str(), static_cast<int>(rect.x + rect.width) - MeasureText(count.c_str(), 12) - 4,
                         static_cast<int>(rect.y + rect.height) - 16, 12, WHITE);
            }
        }
        else
        {
            DrawText("+", static_cast<int>(rect.x + rect.width * 0.5f) - MeasureText("+", 18) / 2,
                     static_cast<int>(rect.y + 17.0f), 18, Fade(WHITE, 0.32f));
        }
        DrawText(std::to_string(slot + 1).c_str(), static_cast<int>(rect.x + 4.0f), static_cast<int>(rect.y + 3.0f), 10, Fade(WHITE, 0.62f));
    }
}

void Game::RenderCreativePaletteOverlay() const
{
    RenderCreativePaletteOverlayPolished();
    return;

    const Player* player = GetLocalPlayer();
    if (player == nullptr)
    {
        return;
    }

    int entryCount = 0;
    const int activeTab = std::clamp(creativePaletteTab_, 0, kCreativePaletteCategoryCount - 1);
    const CreativePaletteEntry* entries = CreativePaletteEntries(activeTab, entryCount);
    const Rectangle panel = CreativePalettePanelRect();
    const Vector2 mouse = GetMousePosition();
    const Color panelBg { 16, 18, 22, 255 };
    const Color slotBg { 58, 60, 66, 255 };
    const Color slotInset { 104, 106, 112, 255 };
    const Color selected { 255, 235, 142, 255 };
    const Color cyan { 112, 232, 255, 255 };

    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.36f));
    DrawRectangleRec(panel, Fade(panelBg, 0.96f));
    DrawRectangleLinesEx(panel, 2.0f, Fade(WHITE, 0.26f));
    DrawText("Креатив", static_cast<int>(panel.x + 24.0f), static_cast<int>(panel.y + 17.0f), 24, WHITE);

    for (int tab = 0; tab < kCreativePaletteCategoryCount; ++tab)
    {
        const Rectangle tabRect = CreativePaletteTabRect(tab);
        const bool active = tab == activeTab;
        const bool hovered = CheckCollisionPointRec(mouse, tabRect);
        DrawRectangleRec(tabRect, Fade(active ? Color { 82, 86, 96, 255 } : Color { 42, 44, 50, 255 }, hovered ? 0.96f : 0.84f));
        DrawRectangleLinesEx(tabRect, active ? 2.0f : 1.0f, Fade(active ? selected : WHITE, active ? 0.90f : 0.22f));
        DrawText(CreativePaletteCategoryName(tab), static_cast<int>(tabRect.x + 10.0f), static_cast<int>(tabRect.y + 7.0f), 14,
                 active ? selected : Fade(WHITE, 0.78f));
    }

    const char* teamLabels[] { "R", "B", "G", "Y", "N" };
    for (int teamButton = 0; teamButton < 5; ++teamButton)
    {
        const int teamId = teamButton < 4 ? teamButton : -1;
        const bool active = teamId == creativeSpecialTeam_;
        const Rectangle rect = CreativePaletteTeamRect(teamButton);
        const Color teamColor = teamId >= 0 ? TeamColorForId(teamId) : Fade(WHITE, 0.78f);
        DrawRectangleRec(rect, Fade(active ? teamColor : Color { 38, 40, 46, 255 }, active ? 0.55f : 0.90f));
        DrawRectangleLinesEx(rect, active ? 2.0f : 1.0f, Fade(active ? selected : teamColor, active ? 0.95f : 0.42f));
        DrawText(teamLabels[teamButton], static_cast<int>(rect.x + 10.0f), static_cast<int>(rect.y + 4.0f), 14, WHITE);
    }
    DrawText("команда", static_cast<int>(panel.x + panel.width - 78.0f), static_cast<int>(panel.y + 20.0f), 12, Fade(WHITE, 0.62f));

    const auto drawPaletteSlot = [&](const CreativePaletteEntry& entry, int index)
    {
        const Rectangle rect = CreativePaletteSlotRect(index);
        const bool cursor = index == creativePaletteCursor_;
        const bool hovered = CheckCollisionPointRec(mouse, rect);
        DrawRectangleRec(rect, Fade(slotBg, hovered ? 0.98f : 0.88f));
        DrawRectangle(static_cast<int>(rect.x + 4.0f), static_cast<int>(rect.y + 4.0f),
                      static_cast<int>(rect.width - 8.0f), static_cast<int>(rect.height - 8.0f),
                      Fade(slotInset, 0.28f));
        DrawRectangleLinesEx(rect, cursor ? 3.0f : 1.0f, cursor ? selected : Fade(WHITE, hovered ? 0.42f : 0.18f));

        const Color iconColor = entry.special
            ? CreativeSpecialColor(entry.specialKind, creativeSpecialTeam_)
            : CreativePaletteBlockColor(entry.block);
        const Rectangle icon {
            rect.x + 13.0f,
            rect.y + 9.0f,
            rect.width - 26.0f,
            rect.height - 24.0f
        };
        DrawRectangleRec(icon, iconColor);
        DrawRectangleLinesEx(icon, 1.0f, Fade(WHITE, 0.44f));
        if (entry.special)
        {
            DrawCircle(static_cast<int>(icon.x + icon.width * 0.5f), static_cast<int>(icon.y + icon.height * 0.5f),
                       icon.width * 0.24f, Fade(WHITE, 0.26f));
        }

        const char* label = entry.special ? DisplayName(entry.specialKind) : ItemShortName(ItemFromBlock(entry.block));
        const int labelSize = 9;
        DrawText(label,
                 static_cast<int>(rect.x + rect.width * 0.5f) - MeasureText(label, labelSize) / 2,
                 static_cast<int>(rect.y + rect.height - 13.0f),
                 labelSize,
                 Fade(WHITE, 0.86f));
    };

    for (int i = 0; i < entryCount; ++i)
    {
        drawPaletteSlot(entries[i], i);
    }

    if (creativePaletteCursor_ >= 0 && creativePaletteCursor_ < entryCount)
    {
        const std::string title = CreativePaletteEntryName(entries[creativePaletteCursor_]);
        DrawText(title.c_str(), static_cast<int>(panel.x + 24.0f), static_cast<int>(panel.y + 284.0f), 16, selected);
        DrawText("ЛКМ: в текущий слот | ПКМ: выбрать и закрыть | 1-9: в слот | Tab/колесо: вкладка",
                 static_cast<int>(panel.x + 24.0f), static_cast<int>(panel.y + 306.0f), 12, Fade(WHITE, 0.64f));
    }

    const Inventory& inventory = player->GetInventory();
    const auto& hotbar = inventory.GetHotbarSlots();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        const Rectangle rect = CreativePaletteHotbarRect(slot);
        const bool active = slot == selectedHotbarSlot_;
        const bool hovered = CheckCollisionPointRec(mouse, rect);
        DrawRectangleRec(rect, Fade(Color { 26, 28, 34, 255 }, hovered ? 0.96f : 0.84f));
        DrawRectangleLinesEx(rect, active ? 3.0f : 1.0f, active ? cyan : Fade(WHITE, hovered ? 0.42f : 0.20f));
        const ItemStack& stack = hotbar[slot];
        if (!stack.IsEmpty())
        {
            Color iconColor = Color { 188, 198, 210, 255 };
            if (const std::optional<BlockType> block = ItemToBlock(stack.type))
            {
                iconColor = CreativePaletteBlockColor(*block);
            }
            const Rectangle icon { rect.x + 13.0f, rect.y + 8.0f, rect.width - 26.0f, rect.height - 24.0f };
            DrawRectangleRec(icon, iconColor);
            DrawRectangleLinesEx(icon, 1.0f, Fade(WHITE, 0.42f));
            const char* label = ItemShortName(stack.type);
            DrawText(label,
                     static_cast<int>(rect.x + rect.width * 0.5f) - MeasureText(label, 9) / 2,
                     static_cast<int>(rect.y + rect.height - 13.0f),
                     9,
                     Fade(WHITE, 0.86f));
        }
        DrawText(std::to_string(slot + 1).c_str(), static_cast<int>(rect.x + 4.0f), static_cast<int>(rect.y + 3.0f), 10, Fade(WHITE, 0.62f));
    }
}

void Game::StartCreativeMapTest()
{
    creativeReturnDoc_ = BuildCreativeMapDocument();
    RestartCreativeMapTest();
}

void Game::RestartCreativeMapTest()
{
    if (creativeReturnDoc_.CoreTeamCount() < 2)
    {
        SetMessage("Для теста нужно минимум два Кора разных команд (T — спецблоки).", 4.5f);
        return;
    }
    automatch_.active = false;
    tutorialMode_ = false;
    creativeMode_ = false;
    creativeFlightActive_ = false;
    creativeFlightJumpTapTimer_ = 0.0f;
    creativeSpecialMode_ = false;
    creativeTestActive_ = true;
    selectedMode_ = MatchMode::FourTeams;
    selectedTeamSize_ = 1;
    // One bot on every other core team: the map immediately plays like a match.
    selectedBotCount_ = creativeReturnDoc_.CoreTeamCount() - 1;
    arenaBiome_ = static_cast<ArenaBiome>(creativeReturnDoc_.biome);
    arenaLayout_ = static_cast<ArenaLayout>(creativeReturnDoc_.layout);
    UpdateCustomMapBuildBounds(creativeReturnDoc_);
    pendingCreativeDoc_ = &creativeReturnDoc_;
    SetupMatch();
    pendingCreativeDoc_ = nullptr;
    gameplayFov_ = fov_;
    cameraController_.SetFov(gameplayFov_);
    UpdateCamera(0.016f);
    screen_ = GameScreen::Playing;
    DisableCursor();
    SetMessage("Тест карты: обычные правила. ESC — вернуться в редактор.", 5.0f);
}

void Game::ReturnToCreativeEditor()
{
    StartCreativeSessionFromDocument(creativeReturnDoc_);
    SetMessage("Редактор: карта восстановлена. T — спецблоки, ESC — тест и сохранение.", 4.5f);
}

bool Game::SaveCreativeMapToFile()
{
    std::error_code ec;
    const std::filesystem::path mapPath(creativeMapPath_);
    if (mapPath.has_parent_path())
    {
        std::filesystem::create_directories(mapPath.parent_path(), ec);
    }
    std::string error;
    const CreativeMapDocument doc = BuildCreativeMapDocument();
    if (!SaveCreativeMapDocument(doc, creativeMapPath_, &error))
    {
        SetMessage("Сохранение не удалось: " + error, 4.5f);
        return false;
    }
    SetMessage(std::string("Карта сохранена: ") + creativeMapPath_, 3.5f);
    return true;
}

bool Game::LoadAutomatchMapDocument(const std::string& path, std::string* errorMessage)
{
    CreativeMapDocument doc;
    if (!LoadCreativeMapDocument(path, doc, errorMessage))
    {
        return false;
    }
    if (doc.CoreTeamCount() < 2)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "карте нужно минимум два Кора разных команд: " + path;
        }
        return false;
    }
    std::string validationReason;
    if (!HasPlayableDocumentSpecials(doc, validationReason))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "карта не проходит проверку gameplay-спецточек (" + validationReason + "): " + path;
        }
        return false;
    }
    // A document with four core teams must not inherit a caller's Duel/2v2
    // selection: sudden-death and bot setup consult the selected match mode.
    SetSelectedMode(MatchMode::FourTeams);
    automatchMapDoc_ = std::move(doc);
    automatchMapLoaded_ = true;
    automatchMapPath_ = path;
    return true;
}

bool Game::LoadCreativeMapFromFile()
{
    std::string error;
    if (!LoadCreativeMapDocumentForEditor(creativeMapPath_, &error))
    {
        SetMessage("Загрузка не удалась: " + error, 4.5f);
        return false;
    }
    SetMessage("Карта загружена. T — спецблоки, ESC — тест и сохранение.", 4.5f);
    return true;
}

void Game::OpenCreativeMapBrowser()
{
    creativeMapBrowserPaths_.clear();
    creativeMapBrowserIndex_ = 0;

    std::error_code ec;
    const std::filesystem::path mapsDirectory("maps");
    if (std::filesystem::is_directory(mapsDirectory, ec))
    {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(mapsDirectory, ec))
        {
            if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".dbmap")
            {
                continue;
            }
            creativeMapBrowserPaths_.push_back(entry.path().generic_string());
        }
    }
    std::sort(creativeMapBrowserPaths_.begin(), creativeMapBrowserPaths_.end());

    if (creativeMapBrowserPaths_.empty())
    {
        SetMessage("В папке maps нет .dbmap карт.", 4.0f);
        return;
    }

    const auto current = std::find(creativeMapBrowserPaths_.begin(), creativeMapBrowserPaths_.end(), creativeMapPath_);
    if (current != creativeMapBrowserPaths_.end())
    {
        creativeMapBrowserIndex_ = static_cast<int>(std::distance(creativeMapBrowserPaths_.begin(), current));
    }
    creativeMapBrowserOpen_ = true;
}

bool Game::LoadCreativeMapBrowserSelection()
{
    if (creativeMapBrowserPaths_.empty())
    {
        creativeMapBrowserOpen_ = false;
        return false;
    }

    creativeMapBrowserIndex_ = std::clamp(
        creativeMapBrowserIndex_, 0, static_cast<int>(creativeMapBrowserPaths_.size()) - 1);
    std::string error;
    if (!LoadCreativeMapDocumentForEditor(creativeMapBrowserPaths_[static_cast<std::size_t>(creativeMapBrowserIndex_)], &error))
    {
        SetMessage("Загрузка не удалась: " + error, 4.5f);
        return false;
    }
    creativeMapBrowserOpen_ = false;
    SetMessage("Карта загружена. T — спецблоки, ESC — тест и сохранение.", 4.5f);
    return true;
}

bool Game::LoadCreativeMapDocumentForEditor(const std::string& path, std::string* errorMessage)
{
    CreativeMapDocument doc;
    if (!LoadCreativeMapDocument(path, doc, errorMessage))
    {
        return false;
    }
    creativeMapPath_ = path;
    StartCreativeSessionFromDocument(doc);
    return true;
}

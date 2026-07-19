#include "CreativeMap.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>

int CreativeMapDocument::CoreTeamCount() const
{
    int count = 0;
    for (int teamId = 0; teamId < 4; ++teamId)
    {
        if (TeamHasCore(teamId))
        {
            ++count;
        }
    }
    return count;
}

bool CreativeMapDocument::TeamHasCore(int teamId) const
{
    return std::any_of(specials.begin(), specials.end(), [teamId](const CreativeSpecial& special)
    {
        return special.kind == CreativeSpecialKind::Core && special.teamId == teamId;
    });
}

const char* ToString(CreativeSpecialKind kind)
{
    switch (kind)
    {
    case CreativeSpecialKind::Core: return "core";
    case CreativeSpecialKind::IronGenerator: return "gen_iron";
    case CreativeSpecialKind::GoldGenerator: return "gen_gold";
    case CreativeSpecialKind::CrystalGenerator: return "gen_crystal";
    case CreativeSpecialKind::HeroSpawn: return "spawn";
    case CreativeSpecialKind::TeamChest: return "chest";
    case CreativeSpecialKind::Shop: return "shop";
    case CreativeSpecialKind::Rally: return "rally";
    case CreativeSpecialKind::Lane: return "lane";
    case CreativeSpecialKind::Chokepoint: return "chokepoint";
    case CreativeSpecialKind::Highground: return "highground";
    }
    return "core";
}

const char* DisplayName(CreativeSpecialKind kind)
{
    switch (kind)
    {
    case CreativeSpecialKind::Core: return "Кор";
    case CreativeSpecialKind::IronGenerator: return "Генератор железа";
    case CreativeSpecialKind::GoldGenerator: return "Генератор золота";
    case CreativeSpecialKind::CrystalGenerator: return "Генератор кристаллов";
    case CreativeSpecialKind::HeroSpawn: return "Спавн героев";
    case CreativeSpecialKind::TeamChest: return "Сундук команды";
    case CreativeSpecialKind::Shop: return "Магазин";
    case CreativeSpecialKind::Rally: return "Точка сбора";
    case CreativeSpecialKind::Lane: return "Маршрут";
    case CreativeSpecialKind::Chokepoint: return "Узкий проход";
    case CreativeSpecialKind::Highground: return "Высота";
    }
    return "Кор";
}

bool CreativeSpecialKindFromString(const std::string& token, CreativeSpecialKind& out)
{
    for (int i = 0; i < kCreativeSpecialKindCount; ++i)
    {
        const CreativeSpecialKind kind = static_cast<CreativeSpecialKind>(i);
        if (token == ToString(kind))
        {
            out = kind;
            return true;
        }
    }
    return false;
}

bool SaveCreativeMapDocument(
    const CreativeMapDocument& doc,
    const std::string& path,
    std::string* errorMessage)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "не удалось открыть файл для записи: " + path;
        }
        return false;
    }

    file << "daibedmap 1\n";
    file << "biome " << doc.biome << '\n';
    file << "layout " << doc.layout << '\n';
    if (doc.collapseMinutes > 0.0f)
    {
        file << "collapse " << doc.collapseMinutes << '\n';
    }
    for (const CreativeSpecial& special : doc.specials)
    {
        file << "special " << ToString(special.kind) << ' '
             << special.pos.x << ' ' << special.pos.y << ' ' << special.pos.z << ' '
             << special.teamId << '\n';
    }
    std::vector<CreativeRouteNode> sortedRouteNodes = doc.routeNodes;
    std::sort(sortedRouteNodes.begin(), sortedRouteNodes.end(), [](const auto& a, const auto& b)
    {
        return a.id < b.id;
    });
    for (const CreativeRouteNode& node : sortedRouteNodes)
    {
        file << "route_node " << node.id << ' '
             << node.pos.x << ' ' << node.pos.y << ' ' << node.pos.z << ' '
             << node.teamId << ' ' << node.kind << '\n';
    }
    std::vector<CreativeRouteEdge> sortedRouteEdges = doc.routeEdges;
    std::sort(sortedRouteEdges.begin(), sortedRouteEdges.end(), [](const auto& a, const auto& b)
    {
        if (a.fromId != b.fromId) return a.fromId < b.fromId;
        if (a.toId != b.toId) return a.toId < b.toId;
        return a.tag < b.tag;
    });
    for (const CreativeRouteEdge& edge : sortedRouteEdges)
    {
        file << "route_edge " << edge.fromId << ' ' << edge.toId << ' '
             << edge.cost << ' ' << (edge.bidirectional ? 1 : 0) << ' '
             << edge.tag << '\n';
    }
    // Stable order so identical maps produce identical files (BlockMap iteration
    // order is unspecified).
    std::vector<CreativeMapBlock> sorted = doc.blocks;
    std::sort(sorted.begin(), sorted.end(), [](const CreativeMapBlock& a, const CreativeMapBlock& b)
    {
        if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
        if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
        return a.pos.z < b.pos.z;
    });
    for (const CreativeMapBlock& block : sorted)
    {
        file << "block " << block.pos.x << ' ' << block.pos.y << ' ' << block.pos.z << ' '
             << ToString(block.type) << ' ' << block.teamId << ' '
             << (block.breakable ? 1 : 0) << ' ' << block.variant << '\n';
    }
    if (!file.good())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "ошибка записи в файл: " + path;
        }
        return false;
    }
    return true;
}

bool LoadCreativeMapDocument(
    const std::string& path,
    CreativeMapDocument& doc,
    std::string* errorMessage)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "файл карты не найден: " + path;
        }
        return false;
    }

    std::string header;
    int version = 0;
    file >> header >> version;
    if (header != "daibedmap" || version != 1)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "неизвестный формат карты: " + path;
        }
        return false;
    }

    CreativeMapDocument result;
    std::string line;
    std::getline(file, line); // consume the rest of the header line
    while (std::getline(file, line))
    {
        std::istringstream lineStream(line);
        std::string key;
        if (!(lineStream >> key))
        {
            continue;
        }
        if (key == "biome")
        {
            lineStream >> result.biome;
        }
        else if (key == "layout")
        {
            lineStream >> result.layout;
        }
        else if (key == "collapse")
        {
            lineStream >> result.collapseMinutes;
        }
        else if (key == "special")
        {
            std::string kindToken;
            CreativeSpecial special;
            lineStream >> kindToken >> special.pos.x >> special.pos.y >> special.pos.z >> special.teamId;
            if (!lineStream.fail() && CreativeSpecialKindFromString(kindToken, special.kind))
            {
                result.specials.push_back(special);
            }
        }
        else if (key == "route_node")
        {
            CreativeRouteNode node;
            lineStream >> node.id >> node.pos.x >> node.pos.y >> node.pos.z >> node.teamId >> node.kind;
            if (!lineStream.fail() && !node.id.empty())
            {
                result.routeNodes.push_back(std::move(node));
            }
        }
        else if (key == "route_edge")
        {
            CreativeRouteEdge edge;
            int bidirectional = 1;
            lineStream >> edge.fromId >> edge.toId >> edge.cost >> bidirectional >> edge.tag;
            if (!lineStream.fail() && !edge.fromId.empty() && !edge.toId.empty())
            {
                edge.bidirectional = bidirectional != 0;
                result.routeEdges.push_back(std::move(edge));
            }
        }
        else if (key == "block")
        {
            CreativeMapBlock block;
            std::string typeToken;
            int breakable = 1;
            lineStream >> block.pos.x >> block.pos.y >> block.pos.z >> typeToken >> block.teamId >> breakable;
            if (lineStream.fail())
            {
                continue;
            }
            // The optional state was introduced with the Castle importer.
            // Old six-field files retain a zero variant.
            lineStream >> block.variant;
            if (lineStream.fail())
            {
                block.variant = 0;
            }

            BlockType type = BlockType::Air;
            bool parsed = BlockTypeFromString(typeToken.c_str(), type);
            if (!parsed)
            {
                int numericType = 0;
                const char* begin = typeToken.data();
                const char* end = begin + typeToken.size();
                const std::from_chars_result conversion = std::from_chars(begin, end, numericType);
                if (conversion.ec == std::errc() && conversion.ptr == end
                    && numericType >= 0 && numericType < static_cast<int>(BlockType::Count))
                {
                    type = static_cast<BlockType>(numericType);
                    parsed = true;
                }
            }
            if (parsed)
            {
                block.type = type;
                block.breakable = breakable != 0;
                result.blocks.push_back(block);
            }
        }
    }

    doc = std::move(result);
    return true;
}

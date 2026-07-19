#pragma once

#include "Block.h"

#include <string>
#include <vector>

// Creative map document: the serializable description of a custom map built in
// the creative editor. Specials define the gameplay entities (cores, generators,
// hero spawns, team chests) that SetupMatch instantiates when the map is played;
// blocks are the plain world geometry. This is the exchange format between the
// editor, the map files on disk (maps/*.dbmap) and the test-play rebuild — and,
// later, the surface mods plug into.
enum class CreativeSpecialKind
{
    Core,
    IronGenerator,
    GoldGenerator,
    CrystalGenerator,
    HeroSpawn,
    TeamChest,
    // Shop zone marker (no block): where the team buys gear. A map without
    // shops starves its bots/players of blocks — every playable team needs one.
    Shop,
    // Author-authored bot navigation. These are metadata-only markers: they
    // never create blocks/entities and old maps remain valid without them.
    Rally,
    Lane,
    Chokepoint,
    Highground
};

constexpr int kCreativeSpecialKindCount = 11;

struct CreativeSpecial
{
    CreativeSpecialKind kind = CreativeSpecialKind::Core;
    GridPos pos {};
    int teamId = 0; // -1 = neutral (generators only); navigation is team-owned
};

struct CreativeMapBlock
{
    GridPos pos {};
    BlockType type = BlockType::Air;
    int teamId = -1;
    bool breakable = true;
    int variant = 0;
};

// Optional coarse navigation metadata. Route nodes describe stable tactical
// regions/portals; edges describe authored alternatives between them. The
// voxel pathfinder still owns every physical step and may build or break along
// an edge -- this graph never bypasses PlayerCommand or placement validation.
struct CreativeRouteNode
{
    std::string id;
    GridPos pos {};
    int teamId = -1; // -1 = usable by every team
    std::string kind = "lane";
};

struct CreativeRouteEdge
{
    std::string fromId;
    std::string toId;
    float cost = 1.0f;
    bool bidirectional = true;
    std::string tag = "main";
};

struct CreativeMapDocument
{
    int biome = 0;  // ArenaBiome as int (sky/fog/biome rule)
    int layout = 0; // ArenaLayout as int
    // Sudden-death (core collapse) time in minutes; 0 = auto from map size.
    float collapseMinutes = 0.0f;
    std::vector<CreativeMapBlock> blocks;
    std::vector<CreativeSpecial> specials;
    std::vector<CreativeRouteNode> routeNodes;
    std::vector<CreativeRouteEdge> routeEdges;

    bool IsEmpty() const { return blocks.empty() && specials.empty() && routeNodes.empty(); }
    int CoreTeamCount() const;
    bool TeamHasCore(int teamId) const;
};

// Stable lowercase file token (core/gen_iron/...) and the Russian editor name.
const char* ToString(CreativeSpecialKind kind);
const char* DisplayName(CreativeSpecialKind kind);
bool CreativeSpecialKindFromString(const std::string& token, CreativeSpecialKind& out);

// Line-based UTF-8 text format (same family as DaiBed.settings):
//   daibedmap 1
//   biome <int>
//   layout <int>
//   special <kind> <x> <y> <z> <teamId>
//   route_node <id> <x> <y> <z> <teamId> <kind>
//   route_edge <fromId> <toId> <cost> <bidirectional> <tag>
//   block <x> <y> <z> <blockType> <teamId> <breakable> [variant]
// blockType accepts a stable token (preferred) or a legacy numeric enum value.
bool SaveCreativeMapDocument(
    const CreativeMapDocument& doc,
    const std::string& path,
    std::string* errorMessage = nullptr);
bool LoadCreativeMapDocument(
    const std::string& path,
    CreativeMapDocument& doc,
    std::string* errorMessage = nullptr);

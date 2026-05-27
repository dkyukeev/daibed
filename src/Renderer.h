#pragma once

#include "Core.h"
#include "Feedback.h"
#include "Generator.h"
#include "Player.h"
#include "Shop.h"
#include "Team.h"
#include "World.h"

#include <optional>
#include <string>
#include <vector>

class Renderer
{
public:
    bool Initialize();
    void Shutdown();

    void RenderScene(
        const World& world,
        const std::vector<Team>& teams,
        const std::vector<EnergyCore>& cores,
        const std::vector<Player>& players,
        const std::vector<Generator>& generators,
        const std::vector<ResourcePickup>& pickups,
        const std::vector<DroppedItem>& droppedItems,
        const PlacementPreview& placementPreview,
        const std::vector<WorldEffect>& worldEffects,
        const std::vector<FloatingText>& floatingTexts,
        const Camera3D& camera,
        const ItemStack& localHeldItem,
        Color skyColor,
        bool hideLocalPlayer) const;

    void RenderUI(
        const Player& localPlayer,
        const std::vector<Team>& teams,
        bool shopOpen,
        bool inShopZone,
        int shopCategoryIndex,
        const Shop& shop,
        const std::string& message,
        const PlacementPreview& placementPreview,
        const BreakProgress& breakProgress,
        const CombatPreview& combatPreview,
        int selectedHotbarSlot,
        bool inventoryOpen,
        int inventoryCursorSlot,
        const ItemStack& heldInventoryStack,
        const char* cameraModeText,
        const std::vector<EventMessage>& eventMessages,
        const MatchStats& stats,
        float hitMarkerTimer,
        float damageFlashTimer,
        float matchTime,
        std::optional<int> winnerTeamId) const;

private:
    Color GetBlockColor(const Block& block, const std::vector<Team>& teams) const;
    const Texture2D* GetBlockTexture(BlockType type) const;
    const Texture2D* GetItemTexture(ItemType type) const;
    Color GetResourceColor(ResourceType type) const;
    const Team* FindTeam(const std::vector<Team>& teams, int teamId) const;
    const EnergyCore* FindCore(const std::vector<EnergyCore>& cores, int teamId) const;

    bool texturesReady_ = false;
    Texture2D grassTexture_ {};
    Texture2D dirtTexture_ {};
    Texture2D leafTexture_ {};
    Texture2D woodTexture_ {};
    Texture2D woolTexture_ {};
    Texture2D stoneTexture_ {};
    Texture2D obsidianTexture_ {};
    Texture2D glassTexture_ {};
    Texture2D springTexture_ {};
    Texture2D stickyTexture_ {};
    Texture2D tntTexture_ {};
    Texture2D spikeTexture_ {};
    Texture2D lavaTexture_ {};
    Texture2D iceTexture_ {};
    Texture2D swordIcon_ {};
    Texture2D axeIcon_ {};
    Texture2D spearIcon_ {};
    Texture2D pickaxeIcon_ {};
    Texture2D arrowIcon_ {};
    Texture2D fireballIcon_ {};
    Texture2D medKitIcon_ {};
    Texture2D homeIcon_ {};
    Texture2D dashIcon_ {};
    Texture2D molotovIcon_ {};
    Texture2D alarmIcon_ {};
    Texture2D ironIcon_ {};
    Texture2D goldIcon_ {};
    Texture2D crystalIcon_ {};
};

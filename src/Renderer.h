#pragma once

#include "Core.h"
#include "ChunkRenderer.h"
#include "Feedback.h"
#include "Generator.h"
#include "HeroVisuals.h"
#include "Player.h"
#include "ParticleSystem.h"
#include "SceneShader.h"
#include "Shop.h"
#include "Team.h"
#include "World.h"

#include <array>
#include <cstdint>
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
        const std::vector<HeroDeviceVisual>& heroDevices,
        const OrbitaTeleportPreview& orbitaTeleportPreview,
        const PlacementPreview& placementPreview,
        const std::vector<EnergyProjectile>& projectiles,
        const std::vector<TimedExplosion>& explosives,
        const std::vector<WorldEffect>& worldEffects,
        const ParticleSystem& particles,
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
        const OrbitaTeleportPreview& orbitaTeleportPreview,
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
        const char* heroActive1KeyText,
        const char* heroActive2KeyText,
        const char* heroUltimateKeyText,
        float sniperScopeBlend,
        float sniperMagnification,
        std::optional<int> winnerTeamId) const;

    void RenderHeroPreview(HeroId heroId, Rectangle destination, float yawDegrees, bool portrait = false) const;
    void SetWorldRenderDistance(float distance);
    void SetShadowQuality(int quality);
    // Depth pass from the sun for shadowQuality >= 1.  Must run before the
    // post-processor opens its render target: raylib cannot nest
    // BeginTextureMode, so the shadow framebuffer has to come first.
    void PrepareSunShadows(const World& world, const std::vector<Team>& teams, const Camera3D& camera);
    const ChunkRenderStats& GetChunkRenderStats() const;

private:
    Color GetBlockColor(const Block& block, const std::vector<Team>& teams) const;
    const Texture2D* GetBlockTexture(BlockType type) const;
    const Texture2D* GetBlockNormalTexture(BlockType type) const;
    const Texture2D* GetItemTexture(ItemType type) const;
    void SyncChunks(const World& world, const std::vector<Team>& teams) const;
    void UpdateMapPointLights(const World& world) const;
    void SetNearestMapPointLights(const Camera3D& camera) const;
    bool EnsureShadowTarget(int resolution);
    void UnloadShadowTarget();
    Color GetResourceColor(ResourceType type) const;
    const Team* FindTeam(const std::vector<Team>& teams, int teamId) const;
    const EnergyCore* FindCore(const std::vector<EnergyCore>& cores, int teamId) const;

    bool texturesReady_ = false;
    Texture2D grassTexture_ {};
    Texture2D dirtTexture_ {};
    Texture2D leafTexture_ {};
    Texture2D woodTexture_ {};
    Texture2D teamChestTexture_ {};
    Texture2D woolTexture_ {};
    Texture2D stoneTexture_ {};
    Texture2D smoothStoneTexture_ {};
    Texture2D darkBrickTexture_ {};
    Texture2D lightBrickTexture_ {};
    Texture2D metalBlockTexture_ {};
    Texture2D glowBlockTexture_ {};
    Texture2D plankVariantTexture_ {};
    Texture2D decorativeTileTexture_ {};
    Texture2D trimBlockTexture_ {};
    Texture2D cobblestoneTexture_ {};
    Texture2D andesiteTexture_ {};
    Texture2D polishedAndesiteTexture_ {};
    Texture2D stoneBrickTexture_ {};
    Texture2D chiseledStoneBrickTexture_ {};
    Texture2D birchPlankTexture_ {};
    Texture2D coloredGlassTexture_ {};
    Texture2D coloredClayTexture_ {};
    Texture2D lapisTexture_ {};
    Texture2D diamondTexture_ {};
    Texture2D emeraldTexture_ {};
    Texture2D goldTexture_ {};
    Texture2D ironBarsTexture_ {};
    Texture2D ladderTexture_ {};
    Texture2D torchTexture_ {};
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
    mutable ChunkRenderer chunkRenderer_;
    mutable std::vector<ScenePointLight> mapPointLights_;
    mutable std::uint64_t mapPointLightsRevision_ = 0;
    float worldRenderDistance_ = 150.0f;
    int shadowQuality_ = 1;
    SceneShader sceneShader_;
    HeroVisualLibrary heroVisuals_;
    RenderTexture2D heroPreviewTarget_ {};
    // Tangent-space normal maps per block type (alpha = material gloss mask);
    // blocks without a crafted map fall back to the flat neutral texture so a
    // chunk material never leaves the "texture2" sampler unbound.
    std::array<Texture2D, static_cast<std::size_t>(BlockType::Count)> blockNormalTextures_ {};
    Texture2D flatNormalTexture_ {};
    // Sun shadow map state (shadowQuality >= 1); the depth texture lives in
    // shadowTarget_.depth and is sampled by the lighting shader.
    // World bounding box cached alongside the torch scan; sizes the fog
    // shell so large imported maps are not cut by arena-sized fog walls.
    mutable Vector3 worldBoundsMin_ {};
    mutable Vector3 worldBoundsMax_ {};
    mutable bool worldBoundsValid_ = false;
    RenderTexture2D shadowTarget_ {};
    int shadowResolution_ = 0;
    Matrix sunLightViewProj_ {};
    bool sunShadowsValid_ = false;
    bool shadowTargetFailed_ = false;
};

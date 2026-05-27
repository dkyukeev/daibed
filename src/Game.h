#pragma once

#include "AudioSystem.h"
#include "BotAI.h"
#include "CameraController.h"
#include "CombatSystem.h"
#include "Core.h"
#include "Feedback.h"
#include "GameRules.h"
#include "Generator.h"
#include "InputSystem.h"
#include "Network/LocalNetworkMock.h"
#include "Player.h"
#include "Renderer.h"
#include "Shop.h"
#include "Team.h"
#include "World.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

enum class GameScreen
{
    MainMenu,
    Settings,
    Controls,
    Playing,
    Paused
};

enum class MatchMode
{
    SoloVsBots,
    TwoVsTwo,
    FourTeams,
    Duel
};

enum class BotDifficulty
{
    Easy,
    Normal,
    Hard
};

enum class ArenaLayout
{
    Classic,
    Vertical
};

enum class ArenaBiome
{
    Arena,
    Ice,
    Lava,
    Space,
    Ruins
};

class Game
{
public:
    bool Initialize();
    void Shutdown();
    bool ShouldClose() const;

    void HandleInput();
    void Update(float dt);
    void Render();

private:
    void SetupMatch();
    void SetupGenerators();
    void AddVerticalArenaFeatures();
    void StartSelectedMatch();
    void TriggerCoreCollapse();
    void ApplyBotLoadout(Player& bot) const;
    float TerrainSpeedMultiplier(const Player& player) const;
    void ApplyStandingBlockEffects(Player& player, bool localPlayer);
    void DropPlayerResources(Player& player);
    void UseUtilityInputs(Player& player);
    bool UseUtility(Player& player, UtilityType type);
    bool SpendUtilityItem(Player& player, UtilityType type);
    void UseSelectedItem(Player& player);
    void LaunchProjectile(Player& player, UtilityType type);
    void LaunchProjectileDirected(Player& player, UtilityType type, Vector3 direction, bool announce);
    bool BotUseUtility(Player& bot, Team& team, Player* enemy, EnergyCore* enemyCore);
    void DetonateAt(Vector3 position, int ownerTeamId, int ownerPlayerId, float radius, int damage, bool createFireZone);
    void HandleInventoryInput(Player& player);
    bool TryDropInventoryStack(Player& player, int slot, int amount);
    bool TryQuickMoveInventorySlot(Player& player, int slot);
    void HandleDeathInventory(Player& player, int killerId);
    void NoteDamageCredit(int targetId, int attackerId);
    int DeathCreditFor(int targetId) const;
    void UpdateDamageCredits(float dt);
    void TryOpenBaseChest(Player& player);
    void CloseChest();
    bool TryShopPurchase(Player& player, Team& team, int choice, int repeat, std::string& message);
    void UpdateAlarmTraps();
    void RenderKillFeed() const;
    void RenderChestOverlay() const;
    void RenderScoreboard() const;
    void RenderDeathOverlay(const Player& localPlayer) const;
    void RenderSpectatorOverlay() const;
    void RenderCompass(const Player& localPlayer) const;
    void RenderBotDebug() const;
    bool TryBotRepairCoreDefense(Player& bot, Team& team, float dt);
    void UpdateCamera(float dt);
    void UpdateSpectator(float dt);
    void UpdateLocalPlayer(float dt);
    void UpdateBots(float dt);
    void UpdateGenerators(float dt);
    void UpdatePickups(float dt);
    void UpdateDroppedItems(float dt);
    void UpdateBlockHazards(float dt);
    void UpdateExplosives(float dt);
    void UpdateProjectiles(float dt);
    void UpdateHazardZones(float dt);
    void UpdatePassiveRegeneration(float dt);
    void UpdateBaseHealing(float dt);
    void UpdateFeedback(float dt);
    void UpdatePlacementPreview();
    void UpdateCombatPreview();
    void UpdateFastPlacement(float dt);
    void UpdateAttackOrBreak(float dt);
    void ResetBreakProgress();
    void CompleteBreakProgress(Player& player);
    void HandlePlaceBlock();
    void HandleDeathsAndRespawns();
    void SendMockNetworkInput();
    void HandleMenuInput();
    void HandleSettingsInput();
    void HandleControlsInput();
    void HandlePauseInput();
    void RenderMainMenu() const;
    void RenderSettings() const;
    void RenderControls() const;
    void RenderPauseOverlay() const;
    void RenderGameHints(const Player& localPlayer) const;
    void RenderMinimap(const Player& localPlayer) const;
    void RenderCoreCollapseTimer() const;

    PlacementPreview BuildPlacementPreview(const Player& player) const;
    bool CanPlaceBlockAt(const GridPos& pos, const Player& player, std::string* reason) const;
    bool HasAdjacentAnchorBlock(const GridPos& pos) const;
    bool TryPlaceBlockForPlayer(Player& player, const GridPos& pos, bool announce);
    std::optional<BlockType> SelectPlacementBlockForPlayer(const Player& player, const GridPos& pos) const;
    Vector3 ChooseBotWaypoint(const Player& bot, Vector3 finalTarget) const;
    Vector3 ChooseBotPathWaypoint(Player& bot, Vector3 finalTarget, float dt);
    bool TryBotBridgeBlock(Player& bot, Vector3 target);
    bool TryBotBreakCoreDefense(Player& bot, EnergyCore& core, float dt);
    std::optional<GridPos> FindBotBlockingBlock(const Player& bot, Vector3 wish) const;
    bool TryBotBreakBlockingBlock(Player& bot, Vector3 wish, float dt);
    void BotTryShop(Player& bot, Team& team);
    EnergyCore* FindNearestEnemyCore(const Player& player);
    const ResourcePickup* FindBestPickupForBot(const Player& bot) const;
    Player* FindNearbyEnemyPlayer(const Player& player, float maxDistance);
    BotMemory& GetBotMemory(Player& bot);
    std::optional<RaycastHit> RaycastFromAim(const Player& player, float maxDistance) const;
    std::optional<GridPos> FindCoreDefenseBlock(const EnergyCore& core, const Player& bot) const;
    std::optional<GridPos> FindMissingCoreDefenseBlock(const Team& team) const;
    std::optional<GridPos> FindUpgradeableCoreDefenseBlock(const Team& team, const Player& bot) const;
    bool TryBotUpgradeCoreDefense(Player& bot, Team& team, float dt);

    bool IsLocalPlayerInShopZone() const;
    bool WouldBlockOverlapPlayer(const GridPos& pos, int underfootPlayerId = -1) const;
    bool IsVoidThreatAt(Vector3 position) const;
    bool HasBotCoreAccess(const Player& bot, const EnergyCore& core) const;
    bool IsTeamActiveForMode(int teamId) const;
    int TeamCountForMode() const;
    int MaxBotCountForSelection() const;
    std::string TeamSizeName() const;
    std::string BotCountName() const;
    int GetForgeBonusForTeam(int teamId) const;
    bool RepairTeamCore(Player& player, Team& team, std::string& message);

    Player* GetLocalPlayer();
    const Player* GetLocalPlayer() const;
    Player* GetSpectatorTarget();
    const Player* GetSpectatorTarget() const;
    void EnterSpectatorMode();
    void CycleSpectatorTarget(int direction);
    Team* FindTeam(int teamId);
    const Team* FindTeam(int teamId) const;
    EnergyCore* FindCoreByTeam(int teamId);
    EnergyCore* FindCoreAt(const GridPos& pos);
    ItemStack GetSelectedHotbarStack(const Player& player) const;
    std::optional<BlockType> GetSelectedBlockType(const Player& player) const;
    std::optional<WeaponType> GetSelectedWeaponType(const Player& player) const;
    int EffectiveToolLevel(const Player& player) const;

    void SetMessage(std::string message, float seconds = 3.0f);
    void AddEventMessage(std::string message, Color color = WHITE, float seconds = 2.4f);
    void AddWorldEffect(Vector3 position, Color color, float radius = 0.35f, float seconds = 0.35f);
    void AddFloatingText(std::string text, Vector3 position, Color color);
    void AddKillFeed(std::string text, Color color = WHITE, float seconds = 5.0f);
    void RegisterCombatEvent(const CombatEvent& event, const std::string& message);
    struct PlayerMatchScore
    {
        int playerId = -1;
        int kills = 0;
        int deaths = 0;
        int finalDeaths = 0;
        int coreDamage = 0;
    };
    struct DamageCredit
    {
        int targetId = -1;
        int attackerId = -1;
        float timer = 0.0f;
    };
    PlayerMatchScore& GetPlayerScore(int playerId);
    const PlayerMatchScore* FindPlayerScore(int playerId) const;
    void LoadSettings();
    void SaveSettings() const;
    const char* MatchModeName() const;
    const char* BotDifficultyName() const;
    const char* ArenaLayoutName() const;
    const char* ArenaBiomeName() const;
    const char* ResolutionName() const;
    const char* FpsLimitName() const;
    void ApplyWindowSettings();
    void ApplyFrameRateLimit();
    const char* TeamName(int teamId) const;
    Color BiomeSkyColor() const;
    Color BiomeFogColor() const;
    float BiomeFogAlpha() const;
    const char* KeyLabel(int key) const;

    World world_;
    std::vector<Team> teams_;
    std::vector<EnergyCore> cores_;
    std::vector<Player> players_;
    std::vector<Generator> generators_;
    std::vector<ResourcePickup> pickups_;

    Renderer renderer_;
    InputSystem input_;
    Shop shop_;
    CombatSystem combat_;
    AudioSystem audio_;
    GameRules rules_;
    LocalNetworkMock network_;

    PlayerInput currentInput_ {};
    CameraController cameraController_;
    std::optional<int> winnerTeamId_;
    std::string message_;
    float messageTimer_ = 0.0f;
    PlacementPreview placementPreview_;
    BreakProgress breakProgress_;
    CombatPreview combatPreview_;
    std::vector<WorldEffect> worldEffects_;
    std::vector<TimedExplosion> timedExplosions_;
    std::vector<EnergyProjectile> projectiles_;
    std::vector<HazardZone> hazardZones_;
    std::vector<AlarmTrap> alarmTraps_;
    std::vector<DroppedItem> droppedItems_;
    std::vector<FloatingText> floatingTexts_;
    std::vector<EventMessage> eventMessages_;
    std::vector<KillFeedEntry> killFeed_;
    std::vector<PlayerMatchScore> playerScores_;
    std::vector<DamageCredit> damageCredits_;
    std::vector<BotMemory> botMemories_;
    std::array<Inventory, 4> teamChests_;
    Inventory personalChest_;
    MatchStats stats_;
    GameScreen screen_ = GameScreen::MainMenu;
    GameScreen returnScreen_ = GameScreen::MainMenu;
    GameScreen controlsReturnScreen_ = GameScreen::MainMenu;
    MatchMode selectedMode_ = MatchMode::SoloVsBots;
    BotDifficulty botDifficulty_ = BotDifficulty::Normal;
    ArenaLayout arenaLayout_ = ArenaLayout::Classic;
    ArenaBiome arenaBiome_ = ArenaBiome::Arena;
    int selectedTeamId_ = 0;
    int selectedTeamSize_ = 1;
    int selectedBotCount_ = 3;
    int menuIndex_ = 0;
    int settingsIndex_ = 0;
    int controlsIndex_ = 0;
    int pauseIndex_ = 0;
    bool waitingForKey_ = false;
    bool exitRequested_ = false;
    bool coreCollapseTriggered_ = false;
    bool generatorBoostTriggered_ = false;
    bool showControlHints_ = true;
    bool showMinimap_ = true;
    bool showBotDebug_ = false;
    int shopCategoryIndex_ = 0;
    int selectedHotbarSlot_ = 0;
    bool inventoryOpen_ = false;
    int inventoryCursorSlot_ = 0;
    ItemStack heldInventoryStack_ {};
    bool teamChestOpen_ = false;
    bool personalChestOpen_ = false;
    bool attackChargeActive_ = false;
    float attackChargeTimer_ = 0.0f;
    int resolutionIndex_ = 1;
    int fpsLimitIndex_ = 1;
    bool fullscreen_ = false;
    float fov_ = 62.0f;
    float gameplayFov_ = 62.0f;
    float hitMarkerTimer_ = 0.0f;
    float damageFlashTimer_ = 0.0f;
    float matchTime_ = 0.0f;
    float passiveRegenTimer_ = 0.0f;
    float baseHealTimer_ = 0.0f;
    float blockHazardTimer_ = 0.0f;
    float fastPlaceTimer_ = 0.0f;
    float localFallVelocity_ = 0.0f;
    float localAirPeakY_ = 0.0f;
    bool localWasOnGround_ = false;
    bool shopOpen_ = false;
    bool scoreboardHeld_ = false;
    bool spectatorMode_ = false;
    bool spectatorFreeCamera_ = false;
    int spectatorTargetIndex_ = 0;
    Vector3 spectatorPosition_ {};
    int localPlayerId_ = 1;
};

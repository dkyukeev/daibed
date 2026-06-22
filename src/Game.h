#pragma once

#include "AudioSystem.h"
#include "BotAI.h"
#include "CameraController.h"
#include "CombatSystem.h"
#include "Core.h"
#include "Feedback.h"
#include "GameRules.h"
#include "Generator.h"
#include "HeroSystem.h"
#include "InputSystem.h"
#include "MusicSystem.h"
#include "Network/LocalNetworkMock.h"
#include "Player.h"
#include "ParticleSystem.h"
#include "PostProcessor.h"
#include "Renderer.h"
#include "Shop.h"
#include "Team.h"
#include "World.h"

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

enum class GameScreen
{
    MainMenu,
    HeroSelect,
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
    bool Initialize(bool headless = false);
    void Shutdown();
    bool ShouldClose() const;
    bool RunAutomatchBatch(int runs, int ticksPerFrame, int maxMinutes, unsigned int seed = 0);
    void SetSelectedBiome(ArenaBiome biome);
    void SetBotDifficulty(BotDifficulty difficulty);
    void SetBotTuningPath(std::string path);
    void SetAutomatchStatsPath(std::string path);
    void SetProfilingEnabled(bool enabled);
    void PrepareStartupSmoke();
    void ExerciseStartupSmokeMutation(bool place);

    void HandleInput();
    void Update(float dt);
    void Render();

private:
    struct BotTeamFrameContext;
    struct BotFrameContext;
    struct CoreDefenseMonitor
    {
        float checkTimer = 0.0f;
        int missingBlocks = 0;
        int weakBlocks = 0;
        bool critical = false;
    };

    void SetupMatch();
    void StartTutorialMatch();
    void SetupGenerators();
    void AddClassicArenaLayout();
    void AddFrozenRingLayout();
    void AddMoltenLayersLayout();
    void AddOrbitalShardsLayout();
    void AddBrokenCitadelLayout();
    void AddVerticalArenaFeatures();
    void AddRuinsBiomeFeatures();
    void StartSelectedMatch();
    void TriggerCoreCollapse();
    void ApplyBotLoadout(Player& bot) const;
    float TerrainSpeedMultiplier(const Player& player) const;
    float BiomeGravityMultiplier() const;
    float BiomeJumpMultiplier() const;
    float BiomeGroundControlMultiplier(const Player& player) const;
    float BiomeAirControlMultiplier() const;
    float BiomeKnockbackMultiplier() const;
    void ApplyStandingBlockEffects(Player& player, bool localPlayer);
    void DropPlayerResources(Player& player);
    void UseHeroAbilityInputs(Player& player);
    bool UseHeroAbility(Player& player, HeroAbilitySlot slot);
    bool UseRadonAbility(Player& player, HeroAbilitySlot slot);
    bool UseOrbitaAbility(Player& player, HeroAbilitySlot slot);
    bool UseBromAbility(Player& player, HeroAbilitySlot slot);
    bool UseKonvoyAbility(Player& player, HeroAbilitySlot slot);
    bool UseLikhoAbility(Player& player, HeroAbilitySlot slot);
    bool UseSvidetelAbility(Player& player, HeroAbilitySlot slot);
    void UseRadonForcePulse(Player& player, bool pull);
    void UseRadonMolotov(Player& player);
    void UseRadonDestroyedCoreUltimate(Player& player);
    bool TryRadonCoreSacrifice(EnergyCore& core);
    void EmitRadonCoreWave(Vector3 position, int ownerTeamId, int ownerPlayerId, float radius, float damage, float force);
    void UseOrbitaDash(Player& player);
    bool UseOrbitaPhantomBlocks(Player& player);
    bool UseOrbitaTeleport(Player& player);
    bool IsOrbitaCoreRestrictedPosition(Vector3 position, int teamId) const;
    bool IsOrbitaTeleportDestinationSafe(Vector3 position, int teamId, std::string* reason) const;
    bool UseBromVacuumBot(Player& player, bool temporary, float lifetimeSeconds);
    bool UseBromTurretDrone(Player& player, bool temporary, float lifetimeSeconds);
    bool UseBromUltimate(Player& player);
    bool UseKonvoyTrap(Player& player, float lifetimeSeconds);
    bool UseKonvoyHandcuffs(Player& player, float lifetimeSeconds);
    bool UseKonvoyDome(Player& player, float lifetimeSeconds);
    void ApplyBromBlockBreakPassive(Player& player, const Block& block, Vector3 position);
    void SetHeroAnimation(Player& player, HeroAnimationState state, float seconds);
    void UseUtilityInputs(Player& player);
    bool UseUtility(Player& player, UtilityType type);
    bool SpendUtilityItem(Player& player, UtilityType type);
    void UseSelectedItem(Player& player);
    void LaunchProjectile(Player& player, UtilityType type);
    void LaunchProjectileDirected(Player& player, UtilityType type, Vector3 direction, bool announce);
    bool LaunchBlasterShot(Player& player, Vector3 direction, bool aimed, bool announce);
    bool LaunchBowShot(Player& player, Vector3 direction, float drawPower, bool announce);
    bool BotUseUtility(Player& bot, Team& team, Player* enemy, EnergyCore* enemyCore, float dt);
    bool BotUseHeroAbility(Player& bot, Team& team, Player* enemy, EnergyCore* enemyCore);
    bool BotCastHeroAbility(Player& bot, HeroAbilitySlot slot);
    void DetonateAt(Vector3 position, int ownerTeamId, int ownerPlayerId, float radius, int damage, bool createFireZone, bool blueFire = false);
    void HandleInventoryInput(Player& player);
    bool TryDropInventoryStack(Player& player, int slot, int amount);
    bool TryQuickMoveInventorySlot(Player& player, int slot);
    void HandleDeathInventory(Player& player, int killerId);
    void NoteDamageCredit(int targetId, int attackerId, std::string cause = "урон");
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
    void RenderAutomatchOverlay() const;
    bool TryBotRepairCoreDefense(Player& bot, Team& team, float dt);
    void UpdateCamera(float dt);
    void UpdateSpectator(float dt);
    void UpdateLocalPlayer(float dt);
    void UpdateBots(float dt);
    void UpdateSingleBot(Player& bot, Team& team, float dt, const BotFrameContext& frameContext);
    void UpdateMatchSimulation(float dt);
    void StartAutomatch();
    void ConfigureAutomatchMatch();
    void UpdateAutomatch(float dt);
    void SampleAutomatchBots();
    void FinishAutomatchRun(bool timeout);
    void WriteAutomatchStatsJson() const;
    void UpdateGenerators(float dt);
    void UpdatePickups(float dt);
    void UpdateDroppedItems(float dt);
    void UpdateBlockHazards(float dt);
    void UpdateExplosives(float dt);
    void UpdateProjectiles(float dt);
    void UpdateHazardZones(float dt);
    void UpdateHeroPassives(float dt);
    void UpdateHeroTemporaryBlocks(float dt);
    void UpdateBromDevices(float dt);
    void UpdateKonvoyDevices(float dt);
    void UpdateLikhoBleeds(float dt);
    void UpdateSvidetelEffects(float dt);
    bool DamageHeroDeviceAlongSegment(int attackerTeamId, Vector3 start, Vector3 end, int damage, bool toolAttack);
    void UpdatePassiveRegeneration(float dt);
    void UpdateBaseHealing(float dt);
    void UpdateFeedback(float dt);
    void UpdatePlacementPreview();
    void UpdateCombatPreview();
    OrbitaTeleportPreview BuildOrbitaTeleportPreview(const Player& player) const;
    std::vector<HeroDeviceVisual> BuildHeroDeviceVisuals() const;
    void UpdateFastPlacement(float dt);
    void UpdateAttackOrBreak(float dt);
    void ResetBreakProgress();
    void CompleteBreakProgress(Player& player);
    void HandlePlaceBlock();
    void HandleDeathsAndRespawns();
    void SendMockNetworkInput();
    void HandleMenuInput();
    void HandleHeroSelectInput();
    void HandleSettingsInput();
    void HandleControlsInput();
    void HandlePauseInput();
    void RenderMainMenu() const;
    void RenderHeroSelect() const;
    void RenderSettings() const;
    void RenderControls() const;
    void RenderPauseOverlay() const;
    void RenderGameHints(const Player& localPlayer) const;
    void RenderOnboarding(const Player& localPlayer) const;
    void RenderMinimap(const Player& localPlayer) const;
    void RenderCoreCollapseTimer() const;

    PlacementPreview BuildPlacementPreview(const Player& player) const;
    bool CanPlaceBlockAt(const GridPos& pos, const Player& player, std::string* reason) const;
    bool HasAdjacentAnchorBlock(const GridPos& pos) const;
    bool TryPlaceBlockForPlayer(Player& player, const GridPos& pos, bool announce);
    std::optional<BlockType> SelectPlacementBlockForPlayer(const Player& player, const GridPos& pos) const;
    Vector3 ChooseBotWaypoint(const Player& bot, Vector3 finalTarget) const;
    Vector3 ChooseBotPathWaypoint(Player& bot, Vector3 finalTarget, float dt, const BotFrameContext& frameContext);
    bool TryBotBridgeBlock(Player& bot, Vector3 target);
    bool TryBotBreakCoreDefense(Player& bot, EnergyCore& core, float dt);
    std::optional<GridPos> FindBotBlockingBlock(const Player& bot, Vector3 wish, Vector3 target) const;
    bool TryBotBreakBlockingBlock(Player& bot, Vector3 wish, Vector3 target, float dt);
    void BotTryShop(Player& bot, Team& team);
    EnergyCore* SelectBestAttackTarget(
        const Player& player,
        const BotFrameContext& frameContext,
        const TeamCoordinationBus* coordBus);
    Player* FindNearbyEnemyPlayer(const Player& player, float maxDistance);
    BotMemory& GetBotMemory(Player& bot);
    void ApplyBotHitReaction(int playerId);
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
    std::string AutomatchRunCountName() const;
    std::string AutomatchSpeedName() const;
    std::string AutomatchDurationName() const;
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
    bool IsSniperScopeRequested(const Player& player) const;
    std::optional<BlockType> GetSelectedBlockType(const Player& player) const;
    std::optional<WeaponType> GetSelectedWeaponType(const Player& player) const;
    int EffectiveToolLevel(const Player& player) const;

    void SetMessage(std::string message, float seconds = 3.0f);
    void AddEventMessage(std::string message, Color color = WHITE, float seconds = 2.4f);
    void AddWorldEffect(Vector3 position, Color color, float radius = 0.35f, float seconds = 0.35f);
    void AddWorldEffect(Vector3 position, Vector3 direction, Color color, float radius, float seconds, WorldEffectKind kind);
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
        std::string cause;
    };
    struct HeroTemporaryBlock
    {
        GridPos position {};
        int ownerTeamId = -1;
        float timer = 0.0f;
    };
    struct MolotovBlockBurn
    {
        GridPos position {};
        BlockType blockType = BlockType::Air;
        int ownerTeamId = -1;
        float timer = 0.0f;
    };
    struct RadonBurn
    {
        int targetPlayerId = -1;
        int ownerPlayerId = -1;
        int ownerTeamId = -1;
        float lifetime = 0.0f;
        float tickTimer = 0.0f;
        bool blueFire = false;
    };
    struct LikhoBlockCut
    {
        GridPos position {};
        BlockType blockType = BlockType::Air;
        int ownerPlayerId = -1;
        float lifetime = 0.0f;
    };
    struct KonvoyIntruderMark
    {
        int ownerPlayerId = -1;
        int targetPlayerId = -1;
        float exposure = 0.0f;
        float markedTimer = 0.0f;
        float pulseTimer = 0.0f;
    };
    struct BromVacuumBot
    {
        Vector3 position {};
        int ownerPlayerId = -1;
        int ownerTeamId = -1;
        std::array<int, 3> cargo {};
        bool returning = false;
        bool temporary = false;
        float lifetime = 0.0f;
        float pulseTimer = 0.0f;
        int health = 45;
        float invulnerabilityTimer = 0.0f;
        Vector3 navTarget {};
        Vector3 navWaypoint {};
        Vector3 lastPosition {};
        float repathTimer = 0.0f;
        float stuckTimer = 0.0f;
        float targetLockTimer = 0.0f;
        int lockedPickupIndex = -1;
        bool hasNavWaypoint = false;
    };
    struct BromTurretDrone
    {
        Vector3 position {};
        int ownerPlayerId = -1;
        int ownerTeamId = -1;
        bool temporary = false;
        float lifetime = 0.0f;
        float fireCooldown = 0.0f;
        float pulseTimer = 0.0f;
        float shotFlashTimer = 0.0f;
        Vector3 lastShotTarget {};
        int health = 60;
        float invulnerabilityTimer = 0.0f;
        Vector3 navTarget {};
        Vector3 navWaypoint {};
        float repathTimer = 0.0f;
        float targetLockTimer = 0.0f;
        int lockedTargetPlayerId = -1;
    };
    struct KonvoyTrap
    {
        Vector3 position {};
        int ownerPlayerId = -1;
        int ownerTeamId = -1;
        float lifetime = 0.0f;
        float flashTimer = 0.0f;
        int health = 48;
    };
    struct KonvoyTether
    {
        int ownerPlayerId = -1;
        int targetPlayerId = -1;
        int ownerTeamId = -1;
        float lifetime = 0.0f;
        float flashTimer = 0.0f;
        int ownerLastHealth = 100;
        int accumulatedOwnerDamage = 0;
    };
    struct KonvoyDome
    {
        Vector3 position {};
        int ownerPlayerId = -1;
        int ownerTeamId = -1;
        float lifetime = 0.0f;
        float flashTimer = 0.0f;
        int health = 180;
        std::vector<int> initiallyInsideEnemyIds;
        float chargeTimer = 0.0f;
    };
    struct LikhoBleed
    {
        int targetPlayerId = -1;
        int ownerPlayerId = -1;
        int ownerTeamId = -1;
        float lifetime = 0.0f;
        float tickTimer = 0.0f;
        int stacks = 1;
        int successfulHits = 1;
    };
    struct SvidetelEcho
    {
        Vector3 position {};
        int ownerPlayerId = -1;
        int ownerTeamId = -1;
        float lifetime = 0.0f;
        float fireCooldown = 0.0f;
        bool armed = false;
        Vector3 lastTarget {};
        float flashTimer = 0.0f;
        int health = 42;
    };
    struct SvidetelPhaseBlock
    {
        GridPos position {};
        Block block {};
        float timer = 0.0f;
        float suffocationTimer = 0.0f;
    };
    struct AutomatchBotStats
    {
        std::string name;
        int teamId = -1;
        int roleSamples[4] {};
        int intentSamples[10] {};
        int samples = 0;
        int roleChanges = 0;
        int intentChanges = 0;
        int stuckSamples = 0;
        int voidFalls = 0;
        int kills = 0;
        int deaths = 0;
        int finalDeaths = 0;
        int coreDamage = 0;
        int lastRole = -1;
        int lastIntent = -1;
        bool hasMovementSample = false;
        Vector3 lastPosition {};
        Vector3 minPosition {};
        Vector3 maxPosition {};
        float totalDistance = 0.0f;
        float maxDistanceFromBase = 0.0f;
        float earlyMaxDistanceFromBase = 0.0f;
        float maxDistanceFromCenter = 0.0f;
        float averageDistanceFromBase = 0.0f;
        float averageDistanceFromCenter = 0.0f;
    };
    struct AutomatchTeamStats
    {
        int kills = 0;
        int deaths = 0;
        int finalDeaths = 0;
        int coreDamage = 0;
        int roleSamples[4] {};
        int intentSamples[10] {};
        int samples = 0;
        int resourcesHeld[3] {};
        int alivePlayers = 0;
        int eliminatedPlayers = 0;
        bool coreAlive = false;
        int coreHealth = 0;
        int coreMaxHealth = 0;
    };
    struct AutomatchHeroStats
    {
        int appearances = 0;
        int wins = 0;
        int kills = 0;
        int deaths = 0;
        int coreDamage = 0;
    };
    struct AutomatchTimelineEvent
    {
        float time = 0.0f;
        std::string type;
        int teamId = -1;
        int actorTeamId = -1;
        int actorId = -1;
        int targetId = -1;
        int value = 0;
        std::string text;
    };
    struct AutomatchRunStats
    {
        int winnerTeamId = -1;
        float duration = 0.0f;
        bool timeout = false;
        int kills = 0;
        int coreDamage = 0;
        int coreDestroyedCount = 0;
        int finalDeathCount = 0;
        float firstCoreDamageTime = -1.0f;
        std::string finishReason;
        AutomatchTeamStats teamStats[4] {};
        std::vector<AutomatchTimelineEvent> timeline;
    };
    struct AutomatchState
    {
        bool active = false;
        int targetRuns = 5;
        int completedRuns = 0;
        int timeouts = 0;
        int teamWins[4] {};
        float maxMatchSeconds = 720.0f;
        float totalDuration = 0.0f;
        float sampleTimer = 0.0f;
        int totalKills = 0;
        int totalCoreDamage = 0;
        int totalFinalDeaths = 0;
        int totalCoreDestroyed = 0;
        float currentFirstCoreDamageTime = -1.0f;
        AutomatchTeamStats currentTeamStats[4] {};
        std::array<AutomatchHeroStats, HeroSystem::kHeroCount> heroStats {};
        std::vector<AutomatchTimelineEvent> currentTimeline;
        std::vector<AutomatchBotStats> botStats;
        std::vector<AutomatchRunStats> runs;
    };
    PlayerMatchScore& GetPlayerScore(int playerId);
    const PlayerMatchScore* FindPlayerScore(int playerId) const;
    void LoadSettings();
    void SaveSettings() const;
    void LoadBotTuning();
    const BotTuningGenome& BotTuningForTeam(int teamId) const;
    const char* MatchModeName() const;
    const char* BotDifficultyName() const;
    const char* ArenaLayoutName() const;
    const char* ArenaBiomeName() const;
    const char* ResolutionName() const;
    const char* FpsLimitName() const;
    void ApplyWindowSettings();
    void ApplyFrameRateLimit();
    void AddCameraShake(float strength, float seconds);
    void CenterWindowOnCurrentMonitor() const;
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
    PostProcessor postProcessor_;
    InputSystem input_;
    Shop shop_;
    CombatSystem combat_;
    AudioSystem audio_;
    MusicSystem music_;
    GameRules rules_;
    LocalNetworkMock network_;

    PlayerInput currentInput_ {};
    CameraController cameraController_;
    std::optional<int> winnerTeamId_;
    std::optional<int> suddenDeathTiebreakTeamId_;
    std::string message_;
    float messageTimer_ = 0.0f;
    PlacementPreview placementPreview_;
    BreakProgress breakProgress_;
    CombatPreview combatPreview_;
    OrbitaTeleportPreview orbitaTeleportPreview_;
    std::vector<WorldEffect> worldEffects_;
    ParticleSystem particles_;
    std::vector<TimedExplosion> timedExplosions_;
    std::vector<EnergyProjectile> projectiles_;
    std::vector<HazardZone> hazardZones_;
    std::vector<AlarmTrap> alarmTraps_;
    std::vector<HeroTemporaryBlock> heroTemporaryBlocks_;
    std::vector<MolotovBlockBurn> molotovBlockBurns_;
    std::vector<RadonBurn> radonBurns_;
    std::vector<LikhoBlockCut> likhoBlockCuts_;
    std::vector<KonvoyIntruderMark> konvoyIntruderMarks_;
    std::vector<BromVacuumBot> bromVacuumBots_;
    std::vector<BromTurretDrone> bromTurretDrones_;
    std::vector<KonvoyTrap> konvoyTraps_;
    std::vector<KonvoyTether> konvoyTethers_;
    std::vector<KonvoyDome> konvoyDomes_;
    std::vector<LikhoBleed> likhoBleeds_;
    std::vector<SvidetelEcho> svidetelEchoes_;
    std::vector<SvidetelPhaseBlock> svidetelPhaseBlocks_;
    std::vector<DroppedItem> droppedItems_;
    std::vector<FloatingText> floatingTexts_;
    std::vector<EventMessage> eventMessages_;
    std::vector<KillFeedEntry> killFeed_;
    std::vector<PlayerMatchScore> playerScores_;
    std::vector<DamageCredit> damageCredits_;
    std::vector<BotMemory> botMemories_;
    std::unordered_map<int, std::size_t> botMemoryIndexByPlayerId_;
    std::array<TeamCoordinationBus, 4> teamCoordBuses_;
    std::array<CoreDefenseMonitor, 4> coreDefenseMonitors_;
    std::array<BotTuningGenome, 4> botTuningByTeam_ {
        DefaultBotTuningGenome(),
        DefaultBotTuningGenome(),
        DefaultBotTuningGenome(),
        DefaultBotTuningGenome()
    };
    std::string botTuningPath_ = "bot_tuning.json";
    std::string botTuningSource_ = "defaults";
    std::string automatchStatsPath_ = "automatch_stats.json";
    AutomatchState automatch_;
    std::array<Inventory, 4> teamChests_;
    Inventory personalChest_;
    MatchStats stats_;
    GameScreen screen_ = GameScreen::MainMenu;
    GameScreen returnScreen_ = GameScreen::MainMenu;
    GameScreen controlsReturnScreen_ = GameScreen::MainMenu;
    MatchMode selectedMode_ = MatchMode::FourTeams;
    BotDifficulty botDifficulty_ = BotDifficulty::Normal;
    ArenaLayout arenaLayout_ = ArenaLayout::Classic;
    ArenaBiome arenaBiome_ = ArenaBiome::Arena;
    HeroId selectedHeroId_ = HeroId::Radon;
    int selectedTeamId_ = 0;
    int selectedTeamSize_ = 4;
    int selectedBotCount_ = 15;
    int automatchRunTarget_ = 5;
    int automatchTicksPerFrame_ = 4;
    int automatchMaxMinutes_ = 12;
    unsigned int automatchSeed_ = 0;
    int menuIndex_ = 0;
    int heroSelectIndex_ = 0;
    float heroPreviewYaw_ = 204.0f;
    bool heroPreviewDragging_ = false;
    int settingsIndex_ = 0;
    int controlsIndex_ = 0;
    int pauseIndex_ = 0;
    bool waitingForKey_ = false;
    bool exitRequested_ = false;
    bool headless_ = false;
    bool profilingEnabled_ = false;
    bool suppressLocalFeedback_ = false;
    bool coreCollapseTriggered_ = false;
    bool coreCollapseWarned_ = false;
    bool generatorBoostTriggered_ = false;
    bool showControlHints_ = true;
    bool showMinimap_ = true;
    bool showBotDebug_ = false;
    bool tutorialMode_ = false;
    bool reducedCameraShake_ = false;
    bool reducedFlashes_ = false;
    bool postProcessing_ = true;
    bool bloomEnabled_ = true;
    bool vsyncEnabled_ = true;
    int shopCategoryIndex_ = 0;
    int selectedHotbarSlot_ = 0;
    bool inventoryOpen_ = false;
    int inventoryCursorSlot_ = 0;
    ItemStack heldInventoryStack_ {};
    bool teamChestOpen_ = false;
    bool personalChestOpen_ = false;
    bool attackChargeActive_ = false;
    float attackChargeTimer_ = 0.0f;
    bool blasterCharging_ = false;
    int resolutionIndex_ = 1;
    int fpsLimitIndex_ = 1;
    int windowMode_ = 0;
    int renderScaleIndex_ = 3;
    int drawDistanceIndex_ = 2;
    int shadowQuality_ = 1;
    int effectsQuality_ = 2;
    float masterVolume_ = 0.8f;
    float musicVolume_ = 0.65f;
    float sfxVolume_ = 0.9f;
    float ambientVolume_ = 0.7f;
    float renderScale_ = 1.0f;
    float fov_ = 62.0f;
    float gameplayFov_ = 62.0f;
    float sniperScopeBlend_ = 0.0f;
    float sniperMagnification_ = 1.5f;
    float hitMarkerTimer_ = 0.0f;
    float damageFlashTimer_ = 0.0f;
    float hitStopTimer_ = 0.0f;
    float fovKick_ = 0.0f;
    float suddenDeathDecayTimer_ = 0.0f;
    float orbitaTeleportPreviewTimer_ = 0.0f;
    float matchTime_ = 0.0f;
    float pickupMergeTimer_ = 0.0f;
    float droppedItemMergeTimer_ = 0.0f;
    float passiveRegenTimer_ = 0.0f;
    float baseHealTimer_ = 0.0f;
    float blockHazardTimer_ = 0.0f;
    float fastPlaceTimer_ = 0.0f;
    float localFallVelocity_ = 0.0f;
    float localAirPeakY_ = 0.0f;
    bool localWasOnGround_ = false;
    std::size_t simulationOrderOffset_ = 0;
    bool shopOpen_ = false;
    bool scoreboardHeld_ = false;
    bool spectatorMode_ = false;
    bool spectatorFreeCamera_ = false;
    int spectatorTargetIndex_ = 0;
    Vector3 spectatorPosition_ {};
    int localPlayerId_ = 1;
    std::string localDeathKiller_;
    std::string localDeathCause_;
    float localDeathOverlayTimer_ = 0.0f;
    double profileSimulationMs_ = 0.0;
    double profileBotsMs_ = 0.0;
    double profilePathMs_ = 0.0;
    double profileDecisionMs_ = 0.0;
    double profileMovementMs_ = 0.0;
    double profileCombatMs_ = 0.0;
    double profilePerceptionMs_ = 0.0;
    double profilePlanningMs_ = 0.0;
    unsigned long long profileSimulationTicks_ = 0;
    unsigned long long profilePathCalls_ = 0;
    unsigned long long profileDecisionCalls_ = 0;
    unsigned long long profileMovementCalls_ = 0;
    unsigned long long profileCombatCalls_ = 0;
    unsigned long long profilePerceptionCalls_ = 0;
    unsigned long long profilePlanningCalls_ = 0;
};

#include "Game.h"

#include "raylib.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
constexpr float kRenderScales[] { 0.60f, 0.75f, 0.85f, 1.00f };
constexpr float kDrawDistances[] { 64.0f, 96.0f, 150.0f, 220.0f };

struct WindowResolution
{
    int width = 1280;
    int height = 720;
    const char* label = "1280x720";
};

struct FpsLimitOption
{
    int fps = 60;
    const char* label = "60";
};

constexpr WindowResolution kWindowResolutions[] {
    { 1024, 576, "1024x576" },
    { 1280, 720, "1280x720" },
    { 1600, 900, "1600x900" },
    { 1920, 1080, "1920x1080" },
    { 2560, 1440, "2560x1440" }
};

constexpr FpsLimitOption kFpsLimits[] {
    { 30, "30 FPS" },
    { 60, "60 FPS" },
    { 120, "120 FPS" },
    { 144, "144 FPS" },
    { 240, "240 FPS" },
    { 0, "Без ограничений" }
};
}

const char* Game::MatchModeName() const
{
    switch (selectedMode_)
    {
    case MatchMode::SoloVsBots:
        return "Один против ботов";
    case MatchMode::TwoVsTwo:
        return "Две команды";
    case MatchMode::FourTeams:
        return "Четыре команды";
    case MatchMode::Duel:
        return "Дуэль";
    }
    return "Неизвестно";
}

const char* Game::BotDifficultyName() const
{
    switch (botDifficulty_)
    {
    case BotDifficulty::Easy:
        return "Легко";
    case BotDifficulty::Normal:
        return "Нормально";
    case BotDifficulty::Hard:
        return "Сложно";
    }
    return "Неизвестно";
}

const char* Game::BotStrategyProfileName() const
{
    switch (botStrategyProfile_)
    {
    case BotStrategyProfile::Standard:
        return "Обычная";
    case BotStrategyProfile::HypixelRush:
        return "Hypixel Rush";
    }
    return "Неизвестно";
}

const char* Game::ArenaLayoutName() const
{
    switch (arenaLayout_)
    {
    case ArenaLayout::Classic:
        return "Классика";
    case ArenaLayout::Vertical:
        return "Вертикальная";
    }
    return "Неизвестно";
}

const char* Game::ArenaBiomeName() const
{
    switch (arenaBiome_)
    {
    case ArenaBiome::Arena:
        return "Арена";
    case ArenaBiome::Ice:
        return "Лед";
    case ArenaBiome::Lava:
        return "Лава";
    case ArenaBiome::Space:
        return "Космос";
    case ArenaBiome::Ruins:
        return "Руины";
    }
    return "Неизвестно";
}

const char* Game::ResolutionName() const
{
    const int index = std::clamp(resolutionIndex_, 0, static_cast<int>(std::size(kWindowResolutions)) - 1);
    return kWindowResolutions[index].label;
}

const char* Game::FpsLimitName() const
{
    const int index = std::clamp(fpsLimitIndex_, 0, static_cast<int>(std::size(kFpsLimits)) - 1);
    return kFpsLimits[index].label;
}

void Game::ApplyWindowSettings()
{
    const int index = std::clamp(resolutionIndex_, 0, static_cast<int>(std::size(kWindowResolutions)) - 1);
    const WindowResolution& resolution = kWindowResolutions[index];

    const bool borderless = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
    if (windowMode_ == 2)
    {
        if (borderless)
        {
            ToggleBorderlessWindowed();
        }
        if (!IsWindowFullscreen())
        {
            const int monitor = GetCurrentMonitor();
            SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
            ToggleFullscreen();
        }
        return;
    }

    if (IsWindowFullscreen())
    {
        ToggleFullscreen();
    }
    if (windowMode_ == 1)
    {
        if (!IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
        {
            ToggleBorderlessWindowed();
        }
        return;
    }
    if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
    {
        ToggleBorderlessWindowed();
    }
    const int monitor = GetCurrentMonitor();
    const int maxWidth = std::max(640, GetMonitorWidth(monitor) - 80);
    const int maxHeight = std::max(360, GetMonitorHeight(monitor) - 80);
    SetWindowSize(std::min(resolution.width, maxWidth), std::min(resolution.height, maxHeight));
    CenterWindowOnCurrentMonitor();
}

void Game::ApplyFrameRateLimit()
{
    const int index = std::clamp(fpsLimitIndex_, 0, static_cast<int>(std::size(kFpsLimits)) - 1);
    SetTargetFPS(kFpsLimits[index].fps);
}

void Game::AddCameraShake(float strength, float seconds)
{
    cameraController_.AddShake(strength * (reducedCameraShake_ ? 0.25f : 1.0f), seconds);
}

void Game::CenterWindowOnCurrentMonitor() const
{
    if (IsWindowFullscreen())
    {
        return;
    }

    const int monitor = GetCurrentMonitor();
    const Vector2 monitorPosition = GetMonitorPosition(monitor);
    const int monitorWidth = GetMonitorWidth(monitor);
    const int monitorHeight = GetMonitorHeight(monitor);
    const int windowWidth = GetScreenWidth();
    const int windowHeight = GetScreenHeight();
    const int x = static_cast<int>(monitorPosition.x) + std::max(0, (monitorWidth - windowWidth) / 2);
    const int y = static_cast<int>(monitorPosition.y) + std::max(0, (monitorHeight - windowHeight) / 2);
    SetWindowPosition(x, y);
}

void Game::LoadSettings()
{
    std::ifstream file("DaiBed.settings");
    if (!file)
    {
        return;
    }

    std::string key;
    while (file >> key)
    {
        if (key == "mouseSensitivity")
        {
            float value = input_.GetMouseSensitivity();
            file >> value;
            input_.SetMouseSensitivity(std::clamp(value, 0.3f, 2.5f));
        }
        else if (key == "fov")
        {
            file >> fov_;
            fov_ = std::clamp(fov_, 50.0f, 90.0f);
        }
        else if (key == "resolutionIndex")
        {
            file >> resolutionIndex_;
        }
        else if (key == "fpsLimitIndex")
        {
            file >> fpsLimitIndex_;
        }
        else if (key == "fullscreen")
        {
            bool fullscreen = false;
            file >> fullscreen;
            windowMode_ = fullscreen ? 2 : 0;
        }
        else if (key == "windowMode")
        {
            file >> windowMode_;
        }
        else if (key == "masterVolume")
        {
            file >> masterVolume_;
        }
        else if (key == "musicVolume")
        {
            file >> musicVolume_;
        }
        else if (key == "sfxVolume")
        {
            file >> sfxVolume_;
        }
        else if (key == "ambientVolume")
        {
            file >> ambientVolume_;
        }
        else if (key == "vsync")
        {
            file >> vsyncEnabled_;
        }
        else if (key == "renderScaleIndex")
        {
            file >> renderScaleIndex_;
        }
        else if (key == "drawDistanceIndex")
        {
            file >> drawDistanceIndex_;
        }
        else if (key == "shadowQuality")
        {
            file >> shadowQuality_;
        }
        else if (key == "ambientOcclusionQuality")
        {
            file >> ambientOcclusionQuality_;
        }
        else if (key == "effectsQuality")
        {
            file >> effectsQuality_;
        }
        else if (key == "postProcessing")
        {
            file >> postProcessing_;
        }
        else if (key == "bloom")
        {
            file >> bloomEnabled_;
        }
        else if (key == "shader_giQuality") { file >> shaderSettings_.giQuality; }
        else if (key == "shader_giStrength") { file >> shaderSettings_.giStrength; }
        else if (key == "shader_localShadows") { file >> shaderSettings_.localShadows; }
        else if (key == "shader_shadowSoftness") { file >> shaderSettings_.shadowSoftness; }
        else if (key == "shader_skyQuality") { file >> shaderSettings_.skyQuality; }
        else if (key == "shader_materialQuality") { file >> shaderSettings_.materialQuality; }
        else if (key == "shader_volumetricQuality") { file >> shaderSettings_.volumetricQuality; }
        else if (key == "shader_haze") { file >> shaderSettings_.haze; }
        else if (key == "shader_sunIntensity") { file >> shaderSettings_.sunIntensity; }
        else if (key == "shader_exposure") { file >> shaderSettings_.exposure; }
        else if (key == "shader_bloomIntensity") { file >> shaderSettings_.bloomIntensity; }
        else if (key == "shader_saturation") { file >> shaderSettings_.saturation; }
        else if (key == "shaderPreset") { file >> shaderPreset_; }
        else if (key == "bloomQuality")
        {
            file >> bloomQuality_;
        }
        else if (key == "showHints")
        {
            file >> showControlHints_;
        }
        else if (key == "showMinimap")
        {
            file >> showMinimap_;
        }
        else if (key == "reducedCameraShake")
        {
            file >> reducedCameraShake_;
        }
        else if (key == "reducedFlashes")
        {
            file >> reducedFlashes_;
        }
        else if (key == "firstPersonMotion")
        {
            file >> firstPersonMotionMode_;
        }
        else if (key == "selectedMode")
        {
            int value = static_cast<int>(selectedMode_);
            file >> value;
            selectedMode_ = static_cast<MatchMode>(std::clamp(value, 0, 3));
        }
        else if (key == "selectedTeamId")
        {
            file >> selectedTeamId_;
        }
        else if (key == "selectedTeamSize")
        {
            file >> selectedTeamSize_;
        }
        else if (key == "selectedBotCount")
        {
            file >> selectedBotCount_;
        }
        else if (key == "botDifficulty")
        {
            int value = static_cast<int>(botDifficulty_);
            file >> value;
            botDifficulty_ = static_cast<BotDifficulty>(std::clamp(value, 0, 2));
        }
        else if (key == "botStrategyProfile")
        {
            int value = static_cast<int>(botStrategyProfile_);
            file >> value;
            botStrategyProfile_ = static_cast<BotStrategyProfile>(std::clamp(value, 0, 1));
        }
        else if (key == "arenaLayout")
        {
            int value = static_cast<int>(arenaLayout_);
            file >> value;
            arenaLayout_ = static_cast<ArenaLayout>(std::clamp(value, 0, 1));
        }
        else if (key == "arenaBiome")
        {
            int value = static_cast<int>(arenaBiome_);
            file >> value;
            arenaBiome_ = static_cast<ArenaBiome>(std::clamp(value, 0, 4));
        }
        else if (key == "selectedHero")
        {
            int value = static_cast<int>(selectedHeroId_);
            file >> value;
            selectedHeroId_ = HeroSystem::IdFromIndex(value);
        }
        else if (key == "keyMoveForward")
        {
            file >> input_.MutableBindings().moveForward;
        }
        else if (key == "keyMoveBackward")
        {
            file >> input_.MutableBindings().moveBackward;
        }
        else if (key == "keyMoveLeft")
        {
            file >> input_.MutableBindings().moveLeft;
        }
        else if (key == "keyMoveRight")
        {
            file >> input_.MutableBindings().moveRight;
        }
        else if (key == "keyJump")
        {
            file >> input_.MutableBindings().jump;
        }
        else if (key == "keySneak")
        {
            file >> input_.MutableBindings().sneak;
        }
        else if (key == "keySprint")
        {
            file >> input_.MutableBindings().sprint;
        }
        else if (key == "keyAttack")
        {
            file >> input_.MutableBindings().attack;
        }
        else if (key == "keyPlace")
        {
            file >> input_.MutableBindings().place;
        }
        else if (key == "keyInteract")
        {
            file >> input_.MutableBindings().interact;
        }
        else if (key == "keyInventory")
        {
            file >> input_.MutableBindings().inventory;
        }
        else if (key == "keyDrop")
        {
            file >> input_.MutableBindings().drop;
        }
        else if (key == "keyCameraToggle")
        {
            file >> input_.MutableBindings().cameraToggle;
        }
#if DAIBED_DEVELOPER_BUILD
        else if (key == "keyDebugRespawn")
        {
            file >> input_.MutableBindings().debugRespawn;
        }
#else
        else if (key == "keyDebugRespawn")
        {
            int ignored = 0;
            file >> ignored;
        }
#endif
        else if (key == "keyHeroActive1")
        {
            file >> input_.MutableBindings().heroActive1;
        }
        else if (key == "keyHeroActive2")
        {
            file >> input_.MutableBindings().heroActive2;
        }
        else if (key == "keyHeroUltimate")
        {
            file >> input_.MutableBindings().heroUltimate;
        }
        else if (key == "gamepadDeadZone")
        {
            float value = input_.GetGamepadDeadZone();
            file >> value;
            input_.SetGamepadDeadZone(value);
        }
        else if (key == "gamepadSensitivity")
        {
            float value = input_.GetGamepadSensitivity();
            file >> value;
            input_.SetGamepadSensitivity(value);
        }
        else if (key == "padJump") file >> input_.MutableGamepadBindings().jump;
        else if (key == "padSneak") file >> input_.MutableGamepadBindings().sneak;
        else if (key == "padSprint") file >> input_.MutableGamepadBindings().sprint;
        else if (key == "padAttack") file >> input_.MutableGamepadBindings().attack;
        else if (key == "padPlace") file >> input_.MutableGamepadBindings().place;
        else if (key == "padInteract") file >> input_.MutableGamepadBindings().interact;
        else if (key == "padInventory") file >> input_.MutableGamepadBindings().inventory;
        else if (key == "padDrop") file >> input_.MutableGamepadBindings().drop;
        else if (key == "padCamera") file >> input_.MutableGamepadBindings().cameraToggle;
        else if (key == "padAbility1") file >> input_.MutableGamepadBindings().heroActive1;
        else if (key == "padAbility2") file >> input_.MutableGamepadBindings().heroActive2;
        else if (key == "padUltimate") file >> input_.MutableGamepadBindings().heroUltimate;
#if DAIBED_DEVELOPER_BUILD
        else if (key == "keyShoot")
        {
            file >> input_.MutableBindings().shoot;
        }
        else if (key == "keyFireball")
        {
            file >> input_.MutableBindings().fireball;
        }
        else if (key == "keyHeal")
        {
            file >> input_.MutableBindings().heal;
        }
        else if (key == "keyTeleport")
        {
            file >> input_.MutableBindings().teleport;
        }
        else if (key == "keyDash")
        {
            file >> input_.MutableBindings().dash;
        }
        else if (key == "keyMolotov")
        {
            file >> input_.MutableBindings().molotov;
        }
        else if (key == "keyAlarm")
        {
            file >> input_.MutableBindings().alarm;
        }
#else
        else if (key == "keyShoot" || key == "keyFireball" || key == "keyHeal"
            || key == "keyTeleport" || key == "keyDash" || key == "keyMolotov"
            || key == "keyAlarm")
        {
            int ignored = 0;
            file >> ignored;
        }
#endif
        else if (key == "automatchRunTarget")
        {
            file >> automatchRunTarget_;
        }
        else if (key == "automatchTicksPerFrame")
        {
            file >> automatchTicksPerFrame_;
        }
        else if (key == "automatchMaxMinutes")
        {
            file >> automatchMaxMinutes_;
        }
    }

    resolutionIndex_ = std::clamp(resolutionIndex_, 0, static_cast<int>(std::size(kWindowResolutions)) - 1);
    fpsLimitIndex_ = std::clamp(fpsLimitIndex_, 0, static_cast<int>(std::size(kFpsLimits)) - 1);
    windowMode_ = std::clamp(windowMode_, 0, 2);
    masterVolume_ = std::clamp(masterVolume_, 0.0f, 1.0f);
    musicVolume_ = std::clamp(musicVolume_, 0.0f, 1.0f);
    sfxVolume_ = std::clamp(sfxVolume_, 0.0f, 1.0f);
    ambientVolume_ = std::clamp(ambientVolume_, 0.0f, 1.0f);
    shaderSettings_.Clamp();
    shaderPreset_ = std::clamp(shaderPreset_, 0, 3);
    renderScaleIndex_ = std::clamp(renderScaleIndex_, 0, static_cast<int>(std::size(kRenderScales)) - 1);
    drawDistanceIndex_ = std::clamp(drawDistanceIndex_, 0, static_cast<int>(std::size(kDrawDistances)) - 1);
    shadowQuality_ = std::clamp(shadowQuality_, 0, 2);
    ambientOcclusionQuality_ = std::clamp(ambientOcclusionQuality_, 0, 2);
    effectsQuality_ = std::clamp(effectsQuality_, 0, 2);
    bloomQuality_ = std::clamp(bloomQuality_, 0, 2);
    firstPersonMotionMode_ = std::clamp(firstPersonMotionMode_, 0, 2);
    renderScale_ = kRenderScales[renderScaleIndex_];
    selectedTeamSize_ = std::clamp(selectedTeamSize_, 1, 4);
    selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
    selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
    heroSelectIndex_ = HeroSystem::IndexOf(selectedHeroId_);
    automatchRunTarget_ = std::clamp(automatchRunTarget_, 1, 50);
    automatchTicksPerFrame_ = std::clamp(automatchTicksPerFrame_, 1, 32);
    automatchMaxMinutes_ = std::clamp(automatchMaxMinutes_, 3, 30);
}

void Game::LoadBotTuning()
{
    const BotTuningGenome defaultGenome = DefaultBotTuningGenome();
    for (BotTuningGenome& genome : botTuningByTeam_)
    {
        genome = defaultGenome;
    }

    std::string error;
    if (LoadBotTuningGenomeSetFromJsonFile(botTuningPath_, botTuningByTeam_, &error))
    {
        botTuningSource_ = botTuningPath_;
        return;
    }

    botTuningSource_ = "defaults";
    if (botTuningPath_ == "bot_tuning.json")
    {
        WriteBotTuningGenomeJsonFile("bot_tuning.example.json", defaultGenome, nullptr);
    }
}

const BotTuningGenome& Game::BotTuningForTeam(int teamId) const
{
    if (teamId >= 0 && teamId < static_cast<int>(botTuningByTeam_.size()))
    {
        return botTuningByTeam_[teamId];
    }
    return botTuningByTeam_[0];
}

BotTuningGenome Game::ScaledBotTuningForTeam(int teamId) const
{
    BotTuningGenome scaled = BotTuningForTeam(teamId);

    if (botStrategyProfile_ == BotStrategyProfile::HypixelRush)
    {
        // A deliberately fast Bed Wars loop: make a cheap opening purchase,
        // establish a first front, then send short repeat waves. Difficulty
        // still owns reaction time and mechanical execution; this profile
        // changes only strategic appetite and commitment.
        scaled.earlyEconomySeconds = 24.0f;
        scaled.pressurePhaseSeconds = 36.0f;
        scaled.latePressureSeconds = 90.0f;
        scaled.allInSeconds = 170.0f;
        scaled.intentLockScale = 1.12f;
        scaled.roleLockScale = 0.88f;
        scaled.strategicAttackUrgencyScale = 0.72f;
        scaled.lateAttackUrgencyScale = 0.46f;
        scaled.strategicDefenseUrgencyScale = 0.24f;
        scaled.strategicEconomyBonus = 38.0f;
        scaled.personalEconomyBonus = 72.0f;
        scaled.strategicPressureEconomyPenalty = 110.0f;
        scaled.attackSlotBonus = 126.0f;
        scaled.allyAssistWeight = 0.64f;

        BotRoleTuning& defender = scaled.roles[static_cast<int>(BotRole::Defender)];
        BotRoleTuning& rusher = scaled.roles[static_cast<int>(BotRole::Rusher)];
        BotRoleTuning& collector = scaled.roles[static_cast<int>(BotRole::Collector)];
        BotRoleTuning& fighter = scaled.roles[static_cast<int>(BotRole::Fighter)];
        defender.pressureBiasScale *= 1.10f;
        rusher.pressureBiasScale *= 1.35f;
        collector.pressureBiasScale *= 1.22f;
        fighter.pressureBiasScale *= 1.30f;
        rusher.resourceBiasScale *= 0.72f;
        collector.resourceBiasScale *= 0.82f;
        fighter.resourceBiasScale *= 0.76f;
        rusher.aggression *= 1.16f;
        fighter.aggression *= 1.12f;
        rusher.lootReturnValue = std::max(42.0f, rusher.lootReturnValue);
        fighter.lootReturnValue = std::max(42.0f, fighter.lootReturnValue);
        ClampBotTuningGenome(scaled);
        return scaled;
    }

    // The stock timings were tuned around a 12-minute arena. Imported maps can
    // have a much later collapse and far longer travel legs; leaving the stock
    // clock unchanged puts every bot into permanent all-in after three minutes.
    // Blend toward fractions of the map clock only once the arena grows beyond
    // stock size, preserving the existing stock-map baseline byte-for-byte.
    // A map author may extend collapse to leave more room for a tense final
    // phase. That must not also postpone the proven economy/pressure cadence:
    // Castle's 28-minute collapse is overtime after its 26.4-minute strategic
    // timeline, not a reason to defer all-in by another minute.
    constexpr float kMaxStrategicTimelineSeconds = 26.4f * 60.0f;
    const float strategicTimelineSeconds = std::min(coreCollapseSeconds_, kMaxStrategicTimelineSeconds);
    const float longMapBlend = std::clamp(
        (strategicTimelineSeconds - 12.0f * 60.0f) / (12.0f * 60.0f),
        0.0f,
        1.0f);
    const auto blend = [longMapBlend](float stockSeconds, float mapSeconds)
    {
        return stockSeconds + (mapSeconds - stockSeconds) * longMapBlend;
    };

    scaled.earlyEconomySeconds = blend(scaled.earlyEconomySeconds, strategicTimelineSeconds * 0.11f);
    scaled.pressurePhaseSeconds = blend(scaled.pressurePhaseSeconds, strategicTimelineSeconds * 0.14f);
    scaled.latePressureSeconds = blend(scaled.latePressureSeconds, strategicTimelineSeconds * 0.38f);
    scaled.allInSeconds = blend(scaled.allInSeconds, strategicTimelineSeconds * 0.76f);

    // Custom tuning files may contain unusual ordering. Keep the effective
    // clock monotonic without mutating or re-writing the source genome.
    scaled.pressurePhaseSeconds = std::max(scaled.pressurePhaseSeconds, scaled.earlyEconomySeconds + 2.0f);
    scaled.latePressureSeconds = std::max(scaled.latePressureSeconds, scaled.pressurePhaseSeconds + 20.0f);
    scaled.allInSeconds = std::max(scaled.allInSeconds, scaled.latePressureSeconds + 30.0f);
    return scaled;
}

void Game::SaveSettings() const
{
    std::ofstream file("DaiBed.settings", std::ios::trunc);
    if (!file)
    {
        return;
    }

    file << "mouseSensitivity " << input_.GetMouseSensitivity() << "\n";
    file << "fov " << fov_ << "\n";
    file << "resolutionIndex " << resolutionIndex_ << "\n";
    file << "fpsLimitIndex " << fpsLimitIndex_ << "\n";
    file << "windowMode " << windowMode_ << "\n";
    file << "masterVolume " << masterVolume_ << "\n";
    file << "musicVolume " << musicVolume_ << "\n";
    file << "sfxVolume " << sfxVolume_ << "\n";
    file << "ambientVolume " << ambientVolume_ << "\n";
    file << "vsync " << vsyncEnabled_ << "\n";
    file << "renderScaleIndex " << renderScaleIndex_ << "\n";
    file << "drawDistanceIndex " << drawDistanceIndex_ << "\n";
    file << "shadowQuality " << shadowQuality_ << "\n";
    file << "ambientOcclusionQuality " << ambientOcclusionQuality_ << "\n";
    file << "effectsQuality " << effectsQuality_ << "\n";
    file << "postProcessing " << postProcessing_ << "\n";
    file << "bloom " << bloomEnabled_ << "\n";
    file << "shader_giQuality " << shaderSettings_.giQuality << "\n";
    file << "shader_giStrength " << shaderSettings_.giStrength << "\n";
    file << "shader_localShadows " << shaderSettings_.localShadows << "\n";
    file << "shader_shadowSoftness " << shaderSettings_.shadowSoftness << "\n";
    file << "shader_skyQuality " << shaderSettings_.skyQuality << "\n";
    file << "shader_materialQuality " << shaderSettings_.materialQuality << "\n";
    file << "shader_volumetricQuality " << shaderSettings_.volumetricQuality << "\n";
    file << "shader_haze " << shaderSettings_.haze << "\n";
    file << "shader_sunIntensity " << shaderSettings_.sunIntensity << "\n";
    file << "shader_exposure " << shaderSettings_.exposure << "\n";
    file << "shader_bloomIntensity " << shaderSettings_.bloomIntensity << "\n";
    file << "shader_saturation " << shaderSettings_.saturation << "\n";
    file << "shaderPreset " << shaderPreset_ << "\n";
    file << "bloomQuality " << bloomQuality_ << "\n";
    file << "showHints " << showControlHints_ << "\n";
    file << "showMinimap " << showMinimap_ << "\n";
    file << "reducedCameraShake " << reducedCameraShake_ << "\n";
    file << "reducedFlashes " << reducedFlashes_ << "\n";
    file << "firstPersonMotion " << firstPersonMotionMode_ << "\n";
    file << "selectedMode " << static_cast<int>(selectedMode_) << "\n";
    file << "selectedTeamId " << selectedTeamId_ << "\n";
    file << "selectedTeamSize " << selectedTeamSize_ << "\n";
    file << "selectedBotCount " << selectedBotCount_ << "\n";
    file << "botDifficulty " << static_cast<int>(botDifficulty_) << "\n";
    file << "botStrategyProfile " << static_cast<int>(botStrategyProfile_) << "\n";
    file << "arenaLayout " << static_cast<int>(arenaLayout_) << "\n";
    file << "arenaBiome " << static_cast<int>(arenaBiome_) << "\n";
    file << "selectedHero " << HeroSystem::IndexOf(selectedHeroId_) << "\n";
    const KeyBindings& bindings = input_.GetBindings();
    file << "keyMoveForward " << bindings.moveForward << "\n";
    file << "keyMoveBackward " << bindings.moveBackward << "\n";
    file << "keyMoveLeft " << bindings.moveLeft << "\n";
    file << "keyMoveRight " << bindings.moveRight << "\n";
    file << "keyJump " << bindings.jump << "\n";
    file << "keySneak " << bindings.sneak << "\n";
    file << "keySprint " << bindings.sprint << "\n";
    file << "keyAttack " << bindings.attack << "\n";
    file << "keyPlace " << bindings.place << "\n";
    file << "keyInteract " << bindings.interact << "\n";
    file << "keyInventory " << bindings.inventory << "\n";
    file << "keyDrop " << bindings.drop << "\n";
    file << "keyCameraToggle " << bindings.cameraToggle << "\n";
#if DAIBED_DEVELOPER_BUILD
    file << "keyDebugRespawn " << bindings.debugRespawn << "\n";
#endif
    file << "keyHeroActive1 " << bindings.heroActive1 << "\n";
    file << "keyHeroActive2 " << bindings.heroActive2 << "\n";
    file << "keyHeroUltimate " << bindings.heroUltimate << "\n";
    file << "gamepadDeadZone " << input_.GetGamepadDeadZone() << "\n";
    file << "gamepadSensitivity " << input_.GetGamepadSensitivity() << "\n";
    const GamepadBindings& pad = input_.GetGamepadBindings();
    file << "padJump " << pad.jump << "\n";
    file << "padSneak " << pad.sneak << "\n";
    file << "padSprint " << pad.sprint << "\n";
    file << "padAttack " << pad.attack << "\n";
    file << "padPlace " << pad.place << "\n";
    file << "padInteract " << pad.interact << "\n";
    file << "padInventory " << pad.inventory << "\n";
    file << "padDrop " << pad.drop << "\n";
    file << "padCamera " << pad.cameraToggle << "\n";
    file << "padAbility1 " << pad.heroActive1 << "\n";
    file << "padAbility2 " << pad.heroActive2 << "\n";
    file << "padUltimate " << pad.heroUltimate << "\n";
#if DAIBED_DEVELOPER_BUILD
    file << "keyShoot " << bindings.shoot << "\n";
    file << "keyFireball " << bindings.fireball << "\n";
    file << "keyHeal " << bindings.heal << "\n";
    file << "keyTeleport " << bindings.teleport << "\n";
    file << "keyDash " << bindings.dash << "\n";
    file << "keyMolotov " << bindings.molotov << "\n";
    file << "keyAlarm " << bindings.alarm << "\n";
#endif
    file << "automatchRunTarget " << automatchRunTarget_ << "\n";
    file << "automatchTicksPerFrame " << automatchTicksPerFrame_ << "\n";
    file << "automatchMaxMinutes " << automatchMaxMinutes_ << "\n";
}

const char* Game::TeamName(int teamId) const
{
    switch (teamId)
    {
    case 0:
        return "Красные";
    case 1:
        return "Синие";
    case 2:
        return "Зеленые";
    case 3:
        return "Желтые";
    }
    return "Неизвестно";
}

Color Game::BiomeSkyColor() const
{
    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        return Color { 30, 42, 56, 255 };
    case ArenaBiome::Lava:
        return Color { 38, 19, 18, 255 };
    case ArenaBiome::Space:
        return Color { 5, 6, 18, 255 };
    case ArenaBiome::Ruins:
        // Moody overcast sage instead of near-black: the castle map lives in
        // this biome and a pitch-dark sky fought the sunny key light.
        return Color { 52, 61, 56, 255 };
    case ArenaBiome::Arena:
        break;
    }
    return Color { 112, 166, 238, 255 };
}

Color Game::BiomeFogColor() const
{
    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        return Color { 160, 218, 255, 255 };
    case ArenaBiome::Lava:
        return Color { 255, 88, 42, 255 };
    case ArenaBiome::Space:
        return Color { 38, 44, 100, 255 };
    case ArenaBiome::Ruins:
        return Color { 138, 148, 118, 255 };
    case ArenaBiome::Arena:
        break;
    }
    return Color { 80, 90, 108, 255 };
}

float Game::BiomeFogAlpha() const
{
    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        return 0.10f;
    case ArenaBiome::Lava:
        return 0.09f;
    case ArenaBiome::Space:
        return 0.05f;
    case ArenaBiome::Ruins:
        return 0.07f;
    case ArenaBiome::Arena:
        break;
    }
    return 0.0f;
}

const char* Game::KeyLabel(int key) const
{
    if (IsMouseBinding(key))
    {
        switch (MouseButtonFromBinding(key))
        {
        case MOUSE_BUTTON_LEFT:
            return "ЛКМ";
        case MOUSE_BUTTON_RIGHT:
            return "ПКМ";
        case MOUSE_BUTTON_MIDDLE:
            return "СКМ";
        case MOUSE_BUTTON_SIDE:
            return "Мышь сбоку";
        case MOUSE_BUTTON_EXTRA:
            return "Мышь доп.";
        case MOUSE_BUTTON_FORWARD:
            return "Мышь вперед";
        case MOUSE_BUTTON_BACK:
            return "Мышь назад";
        default:
            return "Мышь";
        }
    }
    switch (key)
    {
    case KEY_SPACE:
        return "Пробел";
    case KEY_LEFT_SHIFT:
        return "Лев. Shift";
    case KEY_RIGHT_SHIFT:
        return "Прав. Shift";
    case KEY_LEFT_CONTROL:
        return "Лев. Ctrl";
    case KEY_RIGHT_CONTROL:
        return "Прав. Ctrl";
    case KEY_TAB:
        return "Tab";
    case KEY_ENTER:
        return "Enter";
    case KEY_ESCAPE:
        return "Esc";
    case KEY_F3:
        return "F3";
    case KEY_F5:
        return "F5";
    case KEY_NULL:
        return "-";
    case KEY_A:
        return "A";
    case KEY_B:
        return "B";
    case KEY_C:
        return "C";
    case KEY_D:
        return "D";
    case KEY_E:
        return "E";
    case KEY_F:
        return "F";
    case KEY_G:
        return "G";
    case KEY_H:
        return "H";
    case KEY_I:
        return "I";
    case KEY_J:
        return "J";
    case KEY_K:
        return "K";
    case KEY_L:
        return "L";
    case KEY_M:
        return "M";
    case KEY_N:
        return "N";
    case KEY_O:
        return "O";
    case KEY_P:
        return "P";
    case KEY_Q:
        return "Q";
    case KEY_R:
        return "R";
    case KEY_S:
        return "S";
    case KEY_T:
        return "T";
    case KEY_U:
        return "U";
    case KEY_V:
        return "V";
    case KEY_W:
        return "W";
    case KEY_X:
        return "X";
    case KEY_Y:
        return "Y";
    case KEY_Z:
        return "Z";
    case KEY_ZERO:
        return "0";
    case KEY_ONE:
        return "1";
    case KEY_TWO:
        return "2";
    case KEY_THREE:
        return "3";
    case KEY_FOUR:
        return "4";
    case KEY_FIVE:
        return "5";
    case KEY_SIX:
        return "6";
    case KEY_SEVEN:
        return "7";
    case KEY_EIGHT:
        return "8";
    case KEY_NINE:
        return "9";
    case KEY_UP:
        return "Up";
    case KEY_DOWN:
        return "Down";
    case KEY_LEFT:
        return "Left";
    case KEY_RIGHT:
        return "Right";
    default:
        break;
    }

    return "?";
}

void Game::ApplyShaderPreset(int preset)
{
    shaderPreset_ = std::clamp(preset, 0, 2);
    shaderSettings_ = ShaderSettings {};
    const bool low = shaderPreset_ == 0;
    const bool high = shaderPreset_ == 2;
    shaderSettings_.giQuality = low ? 0 : (high ? 2 : 1);
    shaderSettings_.localShadows = high;
    shaderSettings_.skyQuality = low ? 0 : (high ? 2 : 1);
    shaderSettings_.materialQuality = low ? 0 : (high ? 2 : 1);
    shaderSettings_.volumetricQuality = high ? 2 : 0;
    renderScaleIndex_ = low ? 0 : 3;
    renderScale_ = kRenderScales[renderScaleIndex_];
    drawDistanceIndex_ = low ? 0 : (high ? 3 : 2);
    shadowQuality_ = low ? 0 : (high ? 2 : 1);
    ambientOcclusionQuality_ = low ? 1 : 2;
    effectsQuality_ = low ? 0 : (high ? 2 : 1);
    bloomQuality_ = high ? 2 : 0;
    postProcessing_ = !low;
    bloomEnabled_ = !low;
    renderer_.SetWorldRenderDistance(kDrawDistances[drawDistanceIndex_]);
    renderer_.SetShadowQuality(shadowQuality_);
    renderer_.SetAmbientOcclusionQuality(ambientOcclusionQuality_);
}

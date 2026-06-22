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
        return "Solo vs bots";
    case MatchMode::TwoVsTwo:
        return "2 teams";
    case MatchMode::FourTeams:
        return "4 teams FFA";
    case MatchMode::Duel:
        return "Duel arena";
    }
    return "Unknown";
}

const char* Game::BotDifficultyName() const
{
    switch (botDifficulty_)
    {
    case BotDifficulty::Easy:
        return "Easy";
    case BotDifficulty::Normal:
        return "Normal";
    case BotDifficulty::Hard:
        return "Hard";
    }
    return "Unknown";
}

const char* Game::ArenaLayoutName() const
{
    switch (arenaLayout_)
    {
    case ArenaLayout::Classic:
        return "Classic";
    case ArenaLayout::Vertical:
        return "Vertical";
    }
    return "Unknown";
}

const char* Game::ArenaBiomeName() const
{
    switch (arenaBiome_)
    {
    case ArenaBiome::Arena:
        return "Arena";
    case ArenaBiome::Ice:
        return "Ice";
    case ArenaBiome::Lava:
        return "Lava";
    case ArenaBiome::Space:
        return "Space";
    case ArenaBiome::Ruins:
        return "Ruins";
    }
    return "Unknown";
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
#if DAIBED_DEVELOPER_BUILD
        else if (key == "keyBridgeMode")
        {
            file >> input_.MutableBindings().bridgeMode;
        }
#else
        else if (key == "keyBridgeMode")
        {
            int ignored = 0;
            file >> ignored;
        }
#endif
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
    renderScaleIndex_ = std::clamp(renderScaleIndex_, 0, static_cast<int>(std::size(kRenderScales)) - 1);
    drawDistanceIndex_ = std::clamp(drawDistanceIndex_, 0, static_cast<int>(std::size(kDrawDistances)) - 1);
    shadowQuality_ = std::clamp(shadowQuality_, 0, 2);
    effectsQuality_ = std::clamp(effectsQuality_, 0, 2);
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
    file << "effectsQuality " << effectsQuality_ << "\n";
    file << "postProcessing " << postProcessing_ << "\n";
    file << "bloom " << bloomEnabled_ << "\n";
    file << "showHints " << showControlHints_ << "\n";
    file << "showMinimap " << showMinimap_ << "\n";
    file << "reducedCameraShake " << reducedCameraShake_ << "\n";
    file << "reducedFlashes " << reducedFlashes_ << "\n";
    file << "selectedMode " << static_cast<int>(selectedMode_) << "\n";
    file << "selectedTeamId " << selectedTeamId_ << "\n";
    file << "selectedTeamSize " << selectedTeamSize_ << "\n";
    file << "selectedBotCount " << selectedBotCount_ << "\n";
    file << "botDifficulty " << static_cast<int>(botDifficulty_) << "\n";
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
#if DAIBED_DEVELOPER_BUILD
    file << "keyBridgeMode " << bindings.bridgeMode << "\n";
#endif
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
        return "Red";
    case 1:
        return "Blue";
    case 2:
        return "Green";
    case 3:
        return "Yellow";
    }
    return "Unknown";
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
        return Color { 24, 27, 24, 255 };
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
            return "Mouse L";
        case MOUSE_BUTTON_RIGHT:
            return "Mouse R";
        case MOUSE_BUTTON_MIDDLE:
            return "Mouse M";
        case MOUSE_BUTTON_SIDE:
            return "Mouse Side";
        case MOUSE_BUTTON_EXTRA:
            return "Mouse Extra";
        case MOUSE_BUTTON_FORWARD:
            return "Mouse Fwd";
        case MOUSE_BUTTON_BACK:
            return "Mouse Back";
        default:
            return "Mouse";
        }
    }
    switch (key)
    {
    case KEY_SPACE:
        return "Space";
    case KEY_LEFT_SHIFT:
        return "LShift";
    case KEY_RIGHT_SHIFT:
        return "RShift";
    case KEY_LEFT_CONTROL:
        return "LCtrl";
    case KEY_RIGHT_CONTROL:
        return "RCtrl";
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

#include "Game.h"

#include "HeroSystem.h"
#include "UiText.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#define DrawText DrawTextUtf8
#define MeasureText MeasureTextUtf8

namespace
{
// Must match Game.cpp's kCoreCollapseSeconds (sudden death at the 12th minute).
constexpr float kCoreCollapseSeconds = 12.0f * 60.0f;

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

constexpr float kRenderScales[] { 0.60f, 0.75f, 0.85f, 1.00f };
constexpr float kDrawDistances[] { 64.0f, 96.0f, 150.0f, 220.0f };

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float Length2D(Vector3 value)
{
    return std::sqrt(value.x * value.x + value.z * value.z);
}

Vector3 Normalize2D(Vector3 value)
{
    const float length = Length2D(value);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, 0.0f };
    }

    return Vector3 { value.x / length, 0.0f, value.z / length };
}

void DrawCenteredText(const std::string& text, int y, int fontSize, Color color)
{
    DrawText(text.c_str(), GetScreenWidth() / 2 - MeasureText(text.c_str(), fontSize) / 2, y, fontSize, color);
}

const char* GamepadButtonLabel(int button)
{
    switch (button)
    {
    case GAMEPAD_BUTTON_LEFT_FACE_UP: return "D-Up";
    case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return "D-Right";
    case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return "D-Down";
    case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return "D-Left";
    case GAMEPAD_BUTTON_RIGHT_FACE_UP: return "Y";
    case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return "B";
    case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return "A";
    case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return "X";
    case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return "LB";
    case GAMEPAD_BUTTON_LEFT_TRIGGER_2: return "LT";
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return "RB";
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_2: return "RT";
    case GAMEPAD_BUTTON_MIDDLE_LEFT: return "Back";
    case GAMEPAD_BUTTON_MIDDLE: return "Guide";
    case GAMEPAD_BUTTON_MIDDLE_RIGHT: return "Start";
    case GAMEPAD_BUTTON_LEFT_THUMB: return "L3";
    case GAMEPAD_BUTTON_RIGHT_THUMB: return "R3";
    default: return "-";
    }
}

void DrawTeamMarker(Vector2 position, int teamId, float radius, Color color)
{
    switch ((teamId % 4 + 4) % 4)
    {
    case 0:
        DrawCircleV(position, radius, color);
        break;
    case 1:
        DrawRectangle(
            static_cast<int>(position.x - radius),
            static_cast<int>(position.y - radius),
            static_cast<int>(radius * 2.0f),
            static_cast<int>(radius * 2.0f),
            color);
        break;
    case 2:
        DrawTriangle(
            Vector2 { position.x, position.y - radius * 1.25f },
            Vector2 { position.x - radius, position.y + radius },
            Vector2 { position.x + radius, position.y + radius },
            color);
        break;
    default:
        DrawCircleLines(static_cast<int>(position.x), static_cast<int>(position.y), radius, color);
        DrawCircleV(position, std::max(1.0f, radius * 0.38f), color);
        break;
    }
}

int DrawWrappedText(const std::string& text, int x, int y, int fontSize, int maxWidth, Color color)
{
    std::istringstream stream(text);
    std::string word;
    std::string line;
    const int lineHeight = fontSize + 6;
    int currentY = y;
    while (stream >> word)
    {
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && MeasureText(candidate.c_str(), fontSize) > maxWidth)
        {
            DrawText(line.c_str(), x, currentY, fontSize, color);
            currentY += lineHeight;
            line = word;
        }
        else
        {
            line = candidate;
        }
    }

    if (!line.empty())
    {
        DrawText(line.c_str(), x, currentY, fontSize, color);
        currentY += lineHeight;
    }
    return currentY;
}

int DrawWrappedTextLimited(const std::string& text, int x, int y, int fontSize, int maxWidth, int maxLines, Color color)
{
    std::istringstream stream(text);
    std::string word;
    std::string line;
    const int lineHeight = fontSize + 5;
    int currentY = y;
    int lines = 0;
    while (stream >> word && lines < maxLines)
    {
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && MeasureText(candidate.c_str(), fontSize) > maxWidth)
        {
            DrawText(line.c_str(), x, currentY, fontSize, color);
            currentY += lineHeight;
            ++lines;
            line = word;
        }
        else
        {
            line = candidate;
        }
    }

    if (!line.empty() && lines < maxLines)
    {
        DrawText(line.c_str(), x, currentY, fontSize, color);
        currentY += lineHeight;
    }
    return currentY;
}

std::string FormatTenths(float value)
{
    const int tenths = static_cast<int>(value * 10.0f + 0.5f);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

std::string AbilityMetaText(const HeroAbilityDefinition& ability)
{
    std::string result;
    if (ability.cooldownSeconds > 0.0f)
    {
        result += "Кулдаун: " + FormatTenths(ability.cooldownSeconds) + " с";
    }
    if (ability.durationSeconds > 0.0f)
    {
        if (!result.empty())
        {
            result += " | ";
        }
        result += "Длительность: " + FormatTenths(ability.durationSeconds) + " с";
    }
    return result;
}
}
void Game::HandleMenuInput()
{
#if DAIBED_DEVELOPER_BUILD
    constexpr int kMenuRows = 15;
#else
    constexpr int kMenuRows = 6;
#endif
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        menuIndex_ = (menuIndex_ + 1) % kMenuRows;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        menuIndex_ = (menuIndex_ + kMenuRows - 1) % kMenuRows;
    }

    const int panelWidth = 640;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 116;
    const Vector2 mouse = GetMousePosition();
    const Vector2 mouseMove = GetMouseDelta();
    int hoveredRow = -1;
    for (int i = 0; i < kMenuRows; ++i)
    {
        const Rectangle row {
            static_cast<float>(panelX + 18),
            static_cast<float>(panelY + 24 + i * 42 - 7),
            static_cast<float>(panelWidth - 36),
            30.0f
        };
        if (CheckCollisionPointRec(mouse, row))
        {
            hoveredRow = i;
            break;
        }
    }
    if (hoveredRow >= 0 && (std::fabs(mouseMove.x) > 0.5f || std::fabs(mouseMove.y) > 0.5f))
    {
        menuIndex_ = hoveredRow;
    }
    const float wheel = GetMouseWheelMove();
    if (wheel < -0.01f)
    {
        menuIndex_ = (menuIndex_ + 1) % kMenuRows;
    }
    else if (wheel > 0.01f)
    {
        menuIndex_ = (menuIndex_ + kMenuRows - 1) % kMenuRows;
    }

    const auto isActionRow = [](int row)
    {
#if DAIBED_DEVELOPER_BUILD
        return row == 0 || row == 8 || row == 12 || row == 13 || row == 14;
#else
        return row == 0 || row == 1 || row == 3 || row == 4 || row == 5;
#endif
    };

    int delta = (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) ? 1 : ((IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) ? -1 : 0);
    bool activate = IsKeyPressed(KEY_ENTER);
    if (hoveredRow >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        menuIndex_ = hoveredRow;
        if (isActionRow(hoveredRow))
        {
            activate = true;
        }
        else
        {
            delta = 1;
        }
    }
    if (hoveredRow >= 0 && !isActionRow(hoveredRow) && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
        menuIndex_ = hoveredRow;
        delta = -1;
    }

    if (delta != 0)
    {
#if DAIBED_DEVELOPER_BUILD
        if (menuIndex_ == 1)
        {
            const int value = (static_cast<int>(selectedMode_) + delta + 4) % 4;
            selectedMode_ = static_cast<MatchMode>(value);
            selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
            selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
        }
        else if (menuIndex_ == 2)
        {
            const int teamCount = TeamCountForMode();
            selectedTeamId_ = (selectedTeamId_ + delta + teamCount) % teamCount;
        }
        else if (menuIndex_ == 3)
        {
            selectedTeamSize_ = ((selectedTeamSize_ - 1 + delta + 4) % 4) + 1;
            selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
        }
        else if (menuIndex_ == 4)
        {
            const int maxBots = MaxBotCountForSelection();
            selectedBotCount_ = (selectedBotCount_ + delta + maxBots + 1) % (maxBots + 1);
        }
        else if (menuIndex_ == 5)
        {
            const int value = (static_cast<int>(botDifficulty_) + delta + 3) % 3;
            botDifficulty_ = static_cast<BotDifficulty>(value);
        }
        else if (menuIndex_ == 6)
        {
            const int value = (static_cast<int>(arenaLayout_) + delta + 2) % 2;
            arenaLayout_ = static_cast<ArenaLayout>(value);
        }
        else if (menuIndex_ == 7)
        {
            const int value = (static_cast<int>(arenaBiome_) + delta + 5) % 5;
            arenaBiome_ = static_cast<ArenaBiome>(value);
        }
        else if (menuIndex_ == 9)
        {
            automatchRunTarget_ = std::clamp(automatchRunTarget_ + delta, 1, 50);
        }
        else if (menuIndex_ == 10)
        {
            automatchTicksPerFrame_ = std::clamp(automatchTicksPerFrame_ + delta, 1, 32);
        }
        else if (menuIndex_ == 11)
        {
            automatchMaxMinutes_ = std::clamp(automatchMaxMinutes_ + delta, 3, 30);
        }
#else
        if (menuIndex_ == 2)
        {
            const int value = (static_cast<int>(botDifficulty_) + delta + 3) % 3;
            botDifficulty_ = static_cast<BotDifficulty>(value);
        }
#endif
        SaveSettings();
    }

    if (activate)
    {
#if DAIBED_DEVELOPER_BUILD
        if (menuIndex_ == 0)
        {
            heroSelectIndex_ = HeroSystem::IndexOf(selectedHeroId_);
            screen_ = GameScreen::HeroSelect;
        }
        else if (menuIndex_ == 8)
        {
            StartAutomatch();
        }
        else if (menuIndex_ == 12)
        {
            returnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Settings;
        }
        else if (menuIndex_ == 13)
        {
            controlsReturnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Controls;
        }
        else if (menuIndex_ == 14)
        {
            exitRequested_ = true;
        }
#else
        if (menuIndex_ == 0)
        {
            selectedMode_ = MatchMode::FourTeams;
            selectedTeamSize_ = 4;
            selectedBotCount_ = 15;
            arenaLayout_ = ArenaLayout::Classic;
            arenaBiome_ = ArenaBiome::Arena;
            heroSelectIndex_ = HeroSystem::IndexOf(selectedHeroId_);
            screen_ = GameScreen::HeroSelect;
        }
        else if (menuIndex_ == 1)
        {
            StartTutorialMatch();
        }
        else if (menuIndex_ == 3)
        {
            returnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Settings;
        }
        else if (menuIndex_ == 4)
        {
            controlsReturnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Controls;
        }
        else if (menuIndex_ == 5)
        {
            exitRequested_ = true;
        }
#endif
    }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        exitRequested_ = true;
    }
}
void Game::HandleHeroSelectInput()
{
    constexpr int heroCount = HeroSystem::kHeroCount;
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        heroSelectIndex_ = (heroSelectIndex_ + 1) % heroCount;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        heroSelectIndex_ = (heroSelectIndex_ + heroCount - 1) % heroCount;
    }
    const float wheel = GetMouseWheelMove();
    if (wheel > 0.01f)
    {
        heroSelectIndex_ = (heroSelectIndex_ + heroCount - 1) % heroCount;
    }
    if (wheel < -0.01f)
    {
        heroSelectIndex_ = (heroSelectIndex_ + 1) % heroCount;
    }

    const int panelWidth = std::min(1040, GetScreenWidth() - 72);
    const int panelHeight = std::min(560, GetScreenHeight() - 142);
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 116;
    const int listX = panelX + 22;
    const int listY = panelY + 56;
    const int rowHeight = 54;
    const Rectangle previewRect {
        static_cast<float>(panelX + panelWidth - 214),
        static_cast<float>(panelY + 18),
        184.0f,
        184.0f
    };
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), previewRect))
    {
        heroPreviewDragging_ = true;
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
    {
        heroPreviewDragging_ = false;
    }
    if (heroPreviewDragging_)
    {
        heroPreviewYaw_ += GetMouseDelta().x * 0.85f;
    }
    const Rectangle startButton {
        static_cast<float>(panelX + panelWidth - 276),
        static_cast<float>(panelY + panelHeight - 60),
        128.0f,
        38.0f
    };
    const Rectangle backButton {
        static_cast<float>(panelX + panelWidth - 136),
        static_cast<float>(panelY + panelHeight - 60),
        92.0f,
        38.0f
    };

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        const Vector2 mouse = GetMousePosition();
        for (int i = 0; i < heroCount; ++i)
        {
            const Rectangle row {
                static_cast<float>(listX),
                static_cast<float>(listY + i * rowHeight),
                286.0f,
                static_cast<float>(rowHeight - 8)
            };
            if (CheckCollisionPointRec(mouse, row))
            {
                heroSelectIndex_ = i;
                break;
            }
        }

        if (CheckCollisionPointRec(mouse, startButton))
        {
            selectedHeroId_ = HeroSystem::IdFromIndex(heroSelectIndex_);
            SaveSettings();
            StartSelectedMatch();
            return;
        }
        if (CheckCollisionPointRec(mouse, backButton))
        {
            screen_ = GameScreen::MainMenu;
            return;
        }
    }

    if (IsKeyPressed(KEY_ENTER))
    {
        selectedHeroId_ = HeroSystem::IdFromIndex(heroSelectIndex_);
        SaveSettings();
        StartSelectedMatch();
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        screen_ = GameScreen::MainMenu;
    }
}

void Game::HandleSettingsInput()
{
    constexpr int kSettingsRows = 24;
    constexpr int kVisibleRows = 12;
    const bool pad = IsGamepadAvailable(0);
    const bool down = IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)
        || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN));
    const bool up = IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)
        || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_UP));
    if (down) settingsIndex_ = (settingsIndex_ + 1) % kSettingsRows;
    if (up) settingsIndex_ = (settingsIndex_ + kSettingsRows - 1) % kSettingsRows;

    const int firstVisible = std::clamp(settingsIndex_ - kVisibleRows / 2, 0, kSettingsRows - kVisibleRows);
    const int panelWidth = 680;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 118;
    const Vector2 mouse = GetMousePosition();
    const Vector2 mouseMove = GetMouseDelta();
    int hoveredRow = -1;
    for (int visible = 0; visible < kVisibleRows; ++visible)
    {
        const int index = firstVisible + visible;
        const Rectangle row {
            static_cast<float>(panelX + 20),
            static_cast<float>(panelY + 22 + visible * 38 - 8),
            static_cast<float>(panelWidth - 40),
            31.0f
        };
        if (CheckCollisionPointRec(mouse, row))
        {
            hoveredRow = index;
            break;
        }
    }
    if (hoveredRow >= 0 && (std::fabs(mouseMove.x) > 0.5f || std::fabs(mouseMove.y) > 0.5f))
    {
        settingsIndex_ = hoveredRow;
    }
    const float wheel = GetMouseWheelMove();
    if (wheel < -0.01f) settingsIndex_ = (settingsIndex_ + 1) % kSettingsRows;
    else if (wheel > 0.01f) settingsIndex_ = (settingsIndex_ + kSettingsRows - 1) % kSettingsRows;

    int delta = (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)
        || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))) ? 1
        : ((IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)
            || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT))) ? -1 : 0);
    bool activate = IsKeyPressed(KEY_ENTER)
        || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
    if (hoveredRow >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        settingsIndex_ = hoveredRow;
        if (hoveredRow == 22 || hoveredRow == 23) activate = true;
        else delta = 1;
    }
    if (hoveredRow >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
    {
        settingsIndex_ = hoveredRow;
        delta = -1;
    }

    if (activate && settingsIndex_ < 22)
    {
        delta = 1;
    }

    if (delta != 0)
    {
        switch (settingsIndex_)
        {
        case 0: input_.SetMouseSensitivity(input_.GetMouseSensitivity() + delta * 0.1f); break;
        case 1: input_.SetGamepadSensitivity(input_.GetGamepadSensitivity() + delta * 0.1f); break;
        case 2: input_.SetGamepadDeadZone(input_.GetGamepadDeadZone() + delta * 0.02f); break;
        case 3:
            fov_ = std::clamp(fov_ + delta * 2.0f, 50.0f, 90.0f);
            gameplayFov_ = fov_;
            cameraController_.SetFov(gameplayFov_);
            break;
        case 4:
            resolutionIndex_ = (resolutionIndex_ + delta + static_cast<int>(std::size(kWindowResolutions))) % static_cast<int>(std::size(kWindowResolutions));
            ApplyWindowSettings();
            break;
        case 5: windowMode_ = (windowMode_ + delta + 3) % 3; ApplyWindowSettings(); break;
        case 6:
            vsyncEnabled_ = !vsyncEnabled_;
            if (vsyncEnabled_) SetWindowState(FLAG_VSYNC_HINT); else ClearWindowState(FLAG_VSYNC_HINT);
            break;
        case 7:
            fpsLimitIndex_ = (fpsLimitIndex_ + delta + static_cast<int>(std::size(kFpsLimits))) % static_cast<int>(std::size(kFpsLimits));
            ApplyFrameRateLimit();
            break;
        case 8:
            renderScaleIndex_ = (renderScaleIndex_ + delta + static_cast<int>(std::size(kRenderScales))) % static_cast<int>(std::size(kRenderScales));
            renderScale_ = kRenderScales[renderScaleIndex_];
            break;
        case 9:
            drawDistanceIndex_ = (drawDistanceIndex_ + delta + static_cast<int>(std::size(kDrawDistances))) % static_cast<int>(std::size(kDrawDistances));
            renderer_.SetWorldRenderDistance(kDrawDistances[drawDistanceIndex_]);
            break;
        case 10: shadowQuality_ = (shadowQuality_ + delta + 3) % 3; renderer_.SetShadowQuality(shadowQuality_); break;
        case 11: effectsQuality_ = (effectsQuality_ + delta + 3) % 3; break;
        case 12: postProcessing_ = !postProcessing_; break;
        case 13: bloomEnabled_ = !bloomEnabled_; break;
        case 14: masterVolume_ = std::clamp(masterVolume_ + delta * 0.1f, 0.0f, 1.0f); break;
        case 15: musicVolume_ = std::clamp(musicVolume_ + delta * 0.1f, 0.0f, 1.0f); break;
        case 16: sfxVolume_ = std::clamp(sfxVolume_ + delta * 0.1f, 0.0f, 1.0f); break;
        case 17: ambientVolume_ = std::clamp(ambientVolume_ + delta * 0.1f, 0.0f, 1.0f); break;
        case 18: showControlHints_ = !showControlHints_; break;
        case 19: showMinimap_ = !showMinimap_; break;
        case 20: reducedCameraShake_ = !reducedCameraShake_; break;
        case 21: reducedFlashes_ = !reducedFlashes_; break;
        default: break;
        }
        audio_.SetVolume(masterVolume_);
        audio_.SetCategoryVolumes(sfxVolume_, ambientVolume_);
        music_.SetVolume(masterVolume_, musicVolume_);
        SaveSettings();
    }

    if (activate)
    {
        if (settingsIndex_ == 22)
        {
            controlsReturnScreen_ = GameScreen::Settings;
            screen_ = GameScreen::Controls;
            waitingForKey_ = false;
        }
        else if (settingsIndex_ == 23)
        {
            SaveSettings();
            screen_ = returnScreen_;
            if (screen_ == GameScreen::Playing) DisableCursor();
        }
    }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        SaveSettings();
        screen_ = returnScreen_;
        if (screen_ == GameScreen::Playing)
        {
            DisableCursor();
        }
    }
}

void Game::HandleControlsInput()
{
#if DAIBED_DEVELOPER_BUILD
    constexpr int kActionCount = 25;
#else
    constexpr int kActionCount = 16;
#endif
    constexpr int kControlRows = kActionCount + 1;
    constexpr int kRowsPerColumn = 12;
    KeyBindings& bindings = input_.MutableBindings();
    GamepadBindings& padBindings = input_.MutableGamepadBindings();

    const auto setBinding = [&bindings](int index, int key)
    {
        if (key != KEY_NULL)
        {
            int* values[] {
                &bindings.moveForward,
                &bindings.moveBackward,
                &bindings.moveLeft,
                &bindings.moveRight,
                &bindings.jump,
                &bindings.sneak,
#if DAIBED_DEVELOPER_BUILD
                &bindings.bridgeMode,
#endif
                &bindings.sprint,
                &bindings.attack,
                &bindings.place,
                &bindings.interact,
                &bindings.inventory,
                &bindings.drop,
                &bindings.cameraToggle,
#if DAIBED_DEVELOPER_BUILD
                &bindings.debugRespawn,
#endif
                &bindings.heroActive1,
                &bindings.heroActive2,
                &bindings.heroUltimate,
                &bindings.shoot,
                &bindings.fireball,
                &bindings.heal,
                &bindings.teleport,
                &bindings.dash,
                &bindings.molotov,
                &bindings.alarm
            };
            for (int* value : values)
            {
                if (*value == key)
                {
                    *value = KEY_NULL;
                }
            }
        }

        switch (index)
        {
        case 0:
            bindings.moveForward = key;
            break;
        case 1:
            bindings.moveBackward = key;
            break;
        case 2:
            bindings.moveLeft = key;
            break;
        case 3:
            bindings.moveRight = key;
            break;
        case 4:
            bindings.jump = key;
            break;
        case 5:
            bindings.sneak = key;
            break;
        case 6:
#if DAIBED_DEVELOPER_BUILD
            bindings.bridgeMode = key;
#else
            bindings.sprint = key;
#endif
            break;
        case 7:
#if DAIBED_DEVELOPER_BUILD
            bindings.sprint = key;
#else
            bindings.attack = key;
#endif
            break;
        case 8:
#if DAIBED_DEVELOPER_BUILD
            bindings.attack = key;
#else
            bindings.place = key;
#endif
            break;
        case 9:
#if DAIBED_DEVELOPER_BUILD
            bindings.place = key;
#else
            bindings.interact = key;
#endif
            break;
        case 10:
#if DAIBED_DEVELOPER_BUILD
            bindings.interact = key;
#else
            bindings.inventory = key;
#endif
            break;
        case 11:
#if DAIBED_DEVELOPER_BUILD
            bindings.inventory = key;
#else
            bindings.drop = key;
#endif
            break;
        case 12:
#if DAIBED_DEVELOPER_BUILD
            bindings.drop = key;
#else
            bindings.cameraToggle = key;
#endif
            break;
        case 13:
#if DAIBED_DEVELOPER_BUILD
            bindings.cameraToggle = key;
#else
            bindings.heroActive1 = key;
#endif
            break;
        case 14:
#if DAIBED_DEVELOPER_BUILD
            bindings.debugRespawn = key;
#else
            bindings.heroActive2 = key;
#endif
            break;
        case 15:
#if DAIBED_DEVELOPER_BUILD
            bindings.heroActive1 = key;
#else
            bindings.heroUltimate = key;
#endif
            break;
        case 16:
#if DAIBED_DEVELOPER_BUILD
            bindings.heroActive2 = key;
#endif
            break;
        case 17:
#if DAIBED_DEVELOPER_BUILD
            bindings.heroUltimate = key;
#endif
            break;
        case 18:
#if DAIBED_DEVELOPER_BUILD
            bindings.shoot = key;
#endif
            break;
        case 19:
#if DAIBED_DEVELOPER_BUILD
            bindings.fireball = key;
#endif
            break;
        case 20:
#if DAIBED_DEVELOPER_BUILD
            bindings.heal = key;
#endif
            break;
        case 21:
#if DAIBED_DEVELOPER_BUILD
            bindings.teleport = key;
#endif
            break;
        case 22:
#if DAIBED_DEVELOPER_BUILD
            bindings.dash = key;
#endif
            break;
        case 23:
#if DAIBED_DEVELOPER_BUILD
            bindings.molotov = key;
#endif
            break;
#if DAIBED_DEVELOPER_BUILD
        case 24:
            bindings.alarm = key;
            break;
#endif
        default:
            break;
        }
    };

    const auto setPadBinding = [&padBindings](int index, int button)
    {
        switch (index)
        {
        case 4: padBindings.jump = button; break;
        case 5: padBindings.sneak = button; break;
#if DAIBED_DEVELOPER_BUILD
        case 7: padBindings.sprint = button; break;
        case 8: padBindings.attack = button; break;
        case 9: padBindings.place = button; break;
        case 10: padBindings.interact = button; break;
        case 11: padBindings.inventory = button; break;
        case 12: padBindings.drop = button; break;
        case 13: padBindings.cameraToggle = button; break;
        case 15: padBindings.heroActive1 = button; break;
        case 16: padBindings.heroActive2 = button; break;
        case 17: padBindings.heroUltimate = button; break;
#else
        case 6: padBindings.sprint = button; break;
        case 7: padBindings.attack = button; break;
        case 8: padBindings.place = button; break;
        case 9: padBindings.interact = button; break;
        case 10: padBindings.inventory = button; break;
        case 11: padBindings.drop = button; break;
        case 12: padBindings.cameraToggle = button; break;
        case 13: padBindings.heroActive1 = button; break;
        case 14: padBindings.heroActive2 = button; break;
        case 15: padBindings.heroUltimate = button; break;
#endif
        default: break;
        }
    };

    if (waitingForKey_)
    {
        if (IsKeyPressed(KEY_ESCAPE))
        {
            waitingForKey_ = false;
            return;
        }

        const int key = GetKeyPressed();
        if (key > 0 && key != KEY_ENTER)
        {
            setBinding(controlsIndex_, key);
            waitingForKey_ = false;
            SaveSettings();
            return;
        }

        const int mouseButtons[] {
            MOUSE_BUTTON_LEFT,
            MOUSE_BUTTON_RIGHT,
            MOUSE_BUTTON_MIDDLE,
            MOUSE_BUTTON_SIDE,
            MOUSE_BUTTON_EXTRA,
            MOUSE_BUTTON_FORWARD,
            MOUSE_BUTTON_BACK
        };
        for (int button : mouseButtons)
        {
            if (IsMouseButtonPressed(button))
            {
                setBinding(controlsIndex_, MouseBinding(button));
                waitingForKey_ = false;
                SaveSettings();
                return;
            }
        }
        if (IsGamepadAvailable(0))
        {
            for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
            {
                if (IsGamepadButtonPressed(0, button))
                {
                    setPadBinding(controlsIndex_, button);
                    waitingForKey_ = false;
                    SaveSettings();
                    return;
                }
            }
        }
        return;
    }

    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        controlsIndex_ = (controlsIndex_ + 1) % kControlRows;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        controlsIndex_ = (controlsIndex_ + kControlRows - 1) % kControlRows;
    }
    if (IsKeyPressed(KEY_RIGHT))
    {
        controlsIndex_ = std::min(controlsIndex_ + kRowsPerColumn, kControlRows - 1);
    }
    if (IsKeyPressed(KEY_LEFT))
    {
        controlsIndex_ = std::max(controlsIndex_ - kRowsPerColumn, 0);
    }
    const int panelWidth = 900;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 142;
    const int kColumnWidth = 440;
    const Vector2 mouse = GetMousePosition();
    const Vector2 mouseMove = GetMouseDelta();
    int hoveredRow = -1;
    for (int i = 0; i < kControlRows; ++i)
    {
        const int column = i / kRowsPerColumn;
        const int row = i % kRowsPerColumn;
        const Rectangle bounds {
            static_cast<float>(panelX + 20 + column * kColumnWidth),
            static_cast<float>(panelY + 18 + row * 35 - 7),
            static_cast<float>(kColumnWidth - 28),
            28.0f
        };
        if (CheckCollisionPointRec(mouse, bounds))
        {
            hoveredRow = i;
            break;
        }
    }
    if (hoveredRow >= 0 && (std::fabs(mouseMove.x) > 0.5f || std::fabs(mouseMove.y) > 0.5f))
    {
        controlsIndex_ = hoveredRow;
    }
    const float wheel = GetMouseWheelMove();
    if (wheel < -0.01f)
    {
        controlsIndex_ = (controlsIndex_ + 1) % kControlRows;
    }
    else if (wheel > 0.01f)
    {
        controlsIndex_ = (controlsIndex_ + kControlRows - 1) % kControlRows;
    }

    const bool activate = IsKeyPressed(KEY_ENTER) || (hoveredRow >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
    if (activate)
    {
        if (hoveredRow >= 0)
        {
            controlsIndex_ = hoveredRow;
        }
        if (controlsIndex_ == kActionCount)
        {
            screen_ = controlsReturnScreen_;
            if (screen_ == GameScreen::Playing)
            {
                DisableCursor();
            }
        }
        else
        {
            waitingForKey_ = true;
        }
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        screen_ = controlsReturnScreen_;
        if (screen_ == GameScreen::Playing)
        {
            DisableCursor();
        }
    }
}

void Game::HandlePauseInput()
{
    constexpr int kPauseRows = 5;
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        pauseIndex_ = (pauseIndex_ + 1) % kPauseRows;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        pauseIndex_ = (pauseIndex_ + kPauseRows - 1) % kPauseRows;
    }
    const int panelWidth = 430;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = GetScreenHeight() / 2 - 130;
    const Vector2 mouse = GetMousePosition();
    const Vector2 mouseMove = GetMouseDelta();
    int hoveredRow = -1;
    for (int i = 0; i < kPauseRows; ++i)
    {
        const Rectangle row {
            static_cast<float>(panelX + 18),
            static_cast<float>(panelY + 24 + i * 39 - 7),
            static_cast<float>(panelWidth - 36),
            29.0f
        };
        if (CheckCollisionPointRec(mouse, row))
        {
            hoveredRow = i;
            break;
        }
    }
    if (hoveredRow >= 0 && (std::fabs(mouseMove.x) > 0.5f || std::fabs(mouseMove.y) > 0.5f))
    {
        pauseIndex_ = hoveredRow;
    }
    const float wheel = GetMouseWheelMove();
    if (wheel < -0.01f)
    {
        pauseIndex_ = (pauseIndex_ + 1) % kPauseRows;
    }
    else if (wheel > 0.01f)
    {
        pauseIndex_ = (pauseIndex_ + kPauseRows - 1) % kPauseRows;
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        screen_ = GameScreen::Playing;
        DisableCursor();
        return;
    }
    const bool activate = IsKeyPressed(KEY_ENTER) || (hoveredRow >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
    if (hoveredRow >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        pauseIndex_ = hoveredRow;
    }
    if (!activate)
    {
        return;
    }

    if (pauseIndex_ == 0)
    {
        screen_ = GameScreen::Playing;
        DisableCursor();
    }
    else if (pauseIndex_ == 1)
    {
        StartSelectedMatch();
    }
    else if (pauseIndex_ == 2)
    {
        returnScreen_ = GameScreen::Paused;
        screen_ = GameScreen::Settings;
    }
    else if (pauseIndex_ == 3)
    {
        screen_ = GameScreen::MainMenu;
        EnableCursor();
    }
    else if (pauseIndex_ == 4)
    {
        exitRequested_ = true;
    }
}

void Game::RenderMainMenu() const
{
#if DAIBED_DEVELOPER_BUILD
    const char* labels[] {
        "Start match",
        "Mode",
        "Team",
        "Team size",
        "Bots",
        "Bot difficulty",
        "Arena layout",
        "Biome",
        "Start automatch",
        "Automatch runs",
        "Automatch speed",
        "Automatch max time",
        "Settings",
        "Управление",
        "Quit"
    };
    const std::string values[] {
        "",
        MatchModeName(),
        TeamName(selectedTeamId_),
        TeamSizeName(),
        BotCountName(),
        BotDifficultyName(),
        ArenaLayoutName(),
        ArenaBiomeName(),
        "",
        AutomatchRunCountName(),
        AutomatchSpeedName(),
        AutomatchDurationName(),
        "",
        "",
        ""
    };
#else
    const char* labels[] {
        "Начать матч 4x4x4x4",
        "Обучение",
        "Сложность ботов",
        "Настройки",
        "Управление",
        "Выход"
    };
    const std::string values[] {
        "",
        "",
        BotDifficultyName(),
        "",
        "",
        ""
    };
#endif

    DrawCenteredText("DaiBed " DAIBED_VERSION, 48, 40, WHITE);
#if DAIBED_DEVELOPER_BUILD
    DrawCenteredText("Developer match setup", 92, 18, Fade(WHITE, 0.72f));
#else
    DrawCenteredText("Геройский BedWars против ботов", 92, 18, Fade(WHITE, 0.72f));
#endif

    const int panelWidth = 640;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 116;
    const int rowCount = static_cast<int>(std::size(labels));
    const int panelHeight = std::max(250, 44 + rowCount * 42);
    DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(Color { 8, 10, 14, 255 }, 0.82f));
    DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.20f));

    for (int i = 0; i < rowCount; ++i)
    {
        const int y = panelY + 24 + i * 42;
        const bool selected = i == menuIndex_;
        const Color color = selected ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.78f);
        DrawRectangle(panelX + 18, y - 7, panelWidth - 36, 30, selected ? Fade(Color { 42, 52, 62, 255 }, 0.92f) : Fade(Color { 18, 21, 29, 255 }, 0.42f));
        DrawText(labels[i], panelX + 34, y, 20, color);
        if (!values[i].empty())
        {
            DrawText(("< " + values[i] + " >").c_str(), panelX + 340, y, 20, color);
        }
    }

#if DAIBED_DEVELOPER_BUILD
    std::string biomeHint = "Arena: neutral rules.";
    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        biomeHint = "Ice: faster routes with slippery ground.";
        break;
    case ArenaBiome::Lava:
        biomeHint = "Lava: low ground burns outside base safety.";
        break;
    case ArenaBiome::Space:
        biomeHint = "Space: low gravity and stronger knockback.";
        break;
    case ArenaBiome::Ruins:
        biomeHint = "Ruins: cracked bridges and relic generators.";
        break;
    case ArenaBiome::Arena:
        break;
    }
    DrawCenteredText("Arrows/WASD navigate | Left/Right change | Enter select", GetScreenHeight() - 70, 18, Fade(WHITE, 0.62f));
    DrawCenteredText(biomeHint.c_str(), GetScreenHeight() - 42, 16, Fade(WHITE, 0.48f));
#else
    DrawCenteredText("W/S или стрелки: выбор | Enter: подтвердить", GetScreenHeight() - 70, 18, Fade(WHITE, 0.62f));
    DrawCenteredText("Классическая арена | 4 команды по 4 игрока", GetScreenHeight() - 42, 16, Fade(WHITE, 0.48f));
#endif
}

void Game::RenderHeroSelect() const
{
    const int panelWidth = std::min(1040, GetScreenWidth() - 72);
    const int panelHeight = std::min(560, GetScreenHeight() - 142);
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 116;
    const int listX = panelX + 22;
    const int listY = panelY + 56;
    const int rowHeight = 54;
    const int detailX = panelX + 340;
    const int detailWidth = panelWidth - 382;
    const auto& selected = HeroSystem::GetDefinitionByIndex(heroSelectIndex_);

    DrawCenteredText("Выбор героя", 42, 38, WHITE);
    DrawCenteredText("Стрелки/WASD - выбрать | Enter - начать матч | Esc - назад", 86, 18, Fade(WHITE, 0.66f));
    DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(Color { 8, 10, 14, 255 }, 0.86f));
    DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.22f));
    DrawText("Герои", listX, panelY + 22, 22, WHITE);

    for (int i = 0; i < HeroSystem::kHeroCount; ++i)
    {
        const HeroDefinition& hero = HeroSystem::GetDefinitionByIndex(i);
        const int y = listY + i * rowHeight;
        const bool current = i == heroSelectIndex_;
        DrawRectangle(listX, y, 286, rowHeight - 8, current ? Fade(Color { 54, 66, 80, 255 }, 0.95f) : Fade(Color { 18, 21, 29, 255 }, 0.54f));
        DrawRectangleLines(listX, y, 286, rowHeight - 8, current ? Fade(Color { 255, 235, 142, 255 }, 0.75f) : Fade(WHITE, 0.16f));
        DrawText(hero.name.c_str(), listX + 14, y + 8, 20, current ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.82f));
        DrawText(hero.passiveName.c_str(), listX + 14, y + 30, 14, Fade(WHITE, current ? 0.72f : 0.48f));
    }

    DrawText(selected.name.c_str(), detailX, panelY + 22, 30, Color { 255, 235, 142, 255 });
    const Rectangle previewRect {
        static_cast<float>(panelX + panelWidth - 214),
        static_cast<float>(panelY + 18),
        184.0f,
        184.0f
    };

    renderer_.RenderHeroPreview(selected.id, previewRect, heroPreviewYaw_);
    DrawText("Тяните мышью для вращения", static_cast<int>(previewRect.x) + 43, static_cast<int>(previewRect.y + previewRect.height) - 20, 12, Fade(WHITE, 0.58f));

    int y = panelY + 64;
    const int contentBottom = panelY + panelHeight - 78;
    const auto drawSection = [&y, detailX, detailWidth, contentBottom, previewRect](const std::string& title, const std::string& description, const std::string& meta, int maxLines)
    {
        if (y >= contentBottom)
        {
            return;
        }
        DrawText(title.c_str(), detailX, y, 17, Color { 112, 232, 255, 255 });
        y += 22;
        if (!meta.empty() && y < contentBottom)
        {
            DrawText(meta.c_str(), detailX, y, 14, Color { 255, 235, 142, 255 });
            y += 19;
        }
        const int availableWidth = y < static_cast<int>(previewRect.y + previewRect.height)
            ? detailWidth - static_cast<int>(previewRect.width) - 18
            : detailWidth;
        y = DrawWrappedTextLimited(description, detailX, y, 13, availableWidth, maxLines, Fade(WHITE, 0.76f));
        y += 10;
    };

    drawSection("Пассивка: " + selected.passiveName, selected.passiveDescription, "", 2);
    drawSection(std::string(KeyLabel(input_.GetBindings().heroActive1)) + ": " + selected.active1.name, selected.active1.description, AbilityMetaText(selected.active1), 3);
    drawSection(std::string(KeyLabel(input_.GetBindings().heroActive2)) + ": " + selected.active2.name, selected.active2.description, AbilityMetaText(selected.active2), 3);
    drawSection(std::string(KeyLabel(input_.GetBindings().heroUltimate)) + ": " + selected.ultimate.name, selected.ultimate.description, AbilityMetaText(selected.ultimate), 3);

    if (y < contentBottom - 24)
    {
        DrawText("Заряд ульты", detailX, y, 17, Color { 112, 232, 255, 255 });
        y += 22;
        DrawWrappedTextLimited(selected.ultimateChargeDescription, detailX, y, 13, detailWidth, 2, Fade(WHITE, 0.76f));
    }

    const Rectangle startButton {
        static_cast<float>(panelX + panelWidth - 276),
        static_cast<float>(panelY + panelHeight - 60),
        128.0f,
        38.0f
    };
    const Rectangle backButton {
        static_cast<float>(panelX + panelWidth - 136),
        static_cast<float>(panelY + panelHeight - 60),
        92.0f,
        38.0f
    };
    DrawRectangleRec(startButton, Fade(Color { 54, 66, 80, 255 }, 0.95f));
    DrawRectangleLinesEx(startButton, 1.0f, Fade(Color { 255, 235, 142, 255 }, 0.75f));
    DrawText("Старт", static_cast<int>(startButton.x) + 30, static_cast<int>(startButton.y) + 10, 18, Color { 255, 235, 142, 255 });
    DrawRectangleRec(backButton, Fade(Color { 24, 28, 36, 255 }, 0.95f));
    DrawRectangleLinesEx(backButton, 1.0f, Fade(WHITE, 0.24f));
    DrawText("Назад", static_cast<int>(backButton.x) + 22, static_cast<int>(backButton.y) + 10, 18, Fade(WHITE, 0.82f));
}

void Game::RenderSettings() const
{
    constexpr int kSettingsRows = 24;
    constexpr int kVisibleRows = 12;
    const char* labels[kSettingsRows] {
        "Чувствительность мыши", "Чувствительность геймпада", "Мёртвая зона стиков", "Угол обзора",
        "Разрешение", "Режим окна", "VSync", "Ограничение FPS", "Масштаб рендера", "Дальность прорисовки",
        "Качество теней", "Качество эффектов", "Post-processing", "Bloom", "Общая громкость", "Музыка",
        "Эффекты", "Окружение", "Подсказки управления", "Мини-карта", "Ослабить тряску камеры",
        "Ослабить вспышки", "Настроить управление", "Назад"
    };
    const auto percent = [](float value)
    {
        return std::to_string(static_cast<int>(value * 100.0f + 0.5f)) + "%";
    };
    const auto quality = [](int value)
    {
        return value == 0 ? std::string("Низкое") : (value == 1 ? std::string("Среднее") : std::string("Высокое"));
    };
    const auto toggle = [](bool value) { return value ? std::string("Вкл.") : std::string("Выкл."); };
    std::array<std::string, kSettingsRows> values {
        FormatTenths(input_.GetMouseSensitivity()), FormatTenths(input_.GetGamepadSensitivity()),
        percent(input_.GetGamepadDeadZone()), std::to_string(static_cast<int>(fov_)), ResolutionName(),
        windowMode_ == 0 ? "Оконный" : (windowMode_ == 1 ? "Без рамки" : "Полноэкранный"),
        toggle(vsyncEnabled_), FpsLimitName(), percent(kRenderScales[renderScaleIndex_]),
        std::to_string(static_cast<int>(kDrawDistances[drawDistanceIndex_])) + " м", quality(shadowQuality_),
        quality(effectsQuality_), toggle(postProcessing_), toggle(bloomEnabled_), percent(masterVolume_),
        percent(musicVolume_), percent(sfxVolume_), percent(ambientVolume_), toggle(showControlHints_),
        toggle(showMinimap_), toggle(reducedCameraShake_), toggle(reducedFlashes_), "", ""
    };

    DrawCenteredText("Настройки", 60, 40, WHITE);
    const int panelWidth = 680;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 118;
    const int panelHeight = kVisibleRows * 38 + 24;
    const int firstVisible = std::clamp(settingsIndex_ - kVisibleRows / 2, 0, kSettingsRows - kVisibleRows);
    DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(Color { 8, 10, 14, 255 }, 0.86f));
    DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.20f));
    BeginScissorMode(panelX + 12, panelY + 8, panelWidth - 24, panelHeight - 16);
    for (int visible = 0; visible < kVisibleRows; ++visible)
    {
        const int i = firstVisible + visible;
        const int y = panelY + 22 + visible * 38;
        const bool selected = i == settingsIndex_;
        const Color color = selected ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.78f);
        DrawRectangle(panelX + 20, y - 8, panelWidth - 40, 31, selected ? Fade(Color { 42, 52, 62, 255 }, 0.92f) : Fade(Color { 18, 21, 29, 255 }, 0.44f));
        DrawText(labels[i], panelX + 38, y, 18, color);
        if (!values[i].empty())
        {
            const std::string valueText = "< " + values[i] + " >";
            DrawText(valueText.c_str(), panelX + panelWidth - 46 - MeasureText(valueText.c_str(), 18), y, 18, color);
        }
    }
    EndScissorMode();
    const float scrollFraction = static_cast<float>(firstVisible) / static_cast<float>(kSettingsRows - kVisibleRows);
    const int trackHeight = panelHeight - 28;
    const int thumbHeight = std::max(48, trackHeight * kVisibleRows / kSettingsRows);
    DrawRectangle(panelX + panelWidth - 12, panelY + 14, 4, trackHeight, Fade(WHITE, 0.12f));
    DrawRectangle(panelX + panelWidth - 12, panelY + 14 + static_cast<int>((trackHeight - thumbHeight) * scrollFraction), 4, thumbHeight, Fade(WHITE, 0.52f));
    DrawCenteredText("Стрелки/WASD или геймпад — изменить | Enter/A — выбрать | Esc — назад", GetScreenHeight() - 46, 17, Fade(WHITE, 0.62f));
}

void Game::RenderControls() const
{
    const KeyBindings& bindings = input_.GetBindings();
    const GamepadBindings& padBindings = input_.GetGamepadBindings();
    const char* labels[] {
        "Вперед",
        "Назад",
        "Влево",
        "Вправо",
        "Прыжок",
        "Присесть",
#if DAIBED_DEVELOPER_BUILD
        "Мост",
#endif
        "Спринт",
        "Атака / ломать",
        "Использовать / ставить",
        "Магазин / действие",
        "Инвентарь",
        "Выбросить",
        "Камера",
#if DAIBED_DEVELOPER_BUILD
        "Отладочный респаун",
#endif
        "Активка 1",
        "Активка 2",
        "Ульта",
#if DAIBED_DEVELOPER_BUILD
        "Огненный шар",
        "Быстрый огонь",
        "Лечение",
        "Телепорт",
        "Рывок",
        "Молотов",
        "Сигнал",
#endif
        "Назад"
    };
    const int keys[] {
        bindings.moveForward,
        bindings.moveBackward,
        bindings.moveLeft,
        bindings.moveRight,
        bindings.jump,
        bindings.sneak,
#if DAIBED_DEVELOPER_BUILD
        bindings.bridgeMode,
#endif
        bindings.sprint,
        bindings.attack,
        bindings.place,
        bindings.interact,
        bindings.inventory,
        bindings.drop,
        bindings.cameraToggle,
#if DAIBED_DEVELOPER_BUILD
        bindings.debugRespawn,
#endif
        bindings.heroActive1,
        bindings.heroActive2,
        bindings.heroUltimate,
#if DAIBED_DEVELOPER_BUILD
        bindings.shoot,
        bindings.fireball,
        bindings.heal,
        bindings.teleport,
        bindings.dash,
        bindings.molotov,
        bindings.alarm,
#endif
        KEY_NULL
    };
    const int padButtons[] {
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        padBindings.jump,
        padBindings.sneak,
#if DAIBED_DEVELOPER_BUILD
        GAMEPAD_BUTTON_UNKNOWN,
#endif
        padBindings.sprint,
        padBindings.attack,
        padBindings.place,
        padBindings.interact,
        padBindings.inventory,
        padBindings.drop,
        padBindings.cameraToggle,
#if DAIBED_DEVELOPER_BUILD
        GAMEPAD_BUTTON_UNKNOWN,
#endif
        padBindings.heroActive1,
        padBindings.heroActive2,
        padBindings.heroUltimate,
#if DAIBED_DEVELOPER_BUILD
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
#endif
        GAMEPAD_BUTTON_UNKNOWN
    };
#if DAIBED_DEVELOPER_BUILD
    constexpr int kActionCount = 25;
#else
    constexpr int kActionCount = 16;
#endif
    constexpr int kControlRows = kActionCount + 1;
    constexpr int kRowsPerColumn = 12;
    constexpr int kColumnWidth = 440;

    DrawCenteredText("Управление", 60, 40, WHITE);
    DrawCenteredText(waitingForKey_ ? "Нажмите клавишу или кнопку мыши. Esc отменяет." : "Enter/ЛКМ меняет выбранную клавишу или кнопку мыши.", 106, 18, Fade(WHITE, 0.64f));

    const int panelWidth = 900;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 142;
    const int panelHeight = 456;
    DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(Color { 8, 10, 14, 255 }, 0.86f));
    DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.20f));

    for (int i = 0; i < kControlRows; ++i)
    {
        const int column = i / kRowsPerColumn;
        const int row = i % kRowsPerColumn;
        const int x = panelX + 20 + column * kColumnWidth;
        const int y = panelY + 18 + row * 35;
        const bool selected = i == controlsIndex_;
        const Color color = selected ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.78f);
        DrawRectangle(x, y - 7, kColumnWidth - 28, 28, selected ? Fade(Color { 42, 52, 62, 255 }, 0.92f) : Fade(Color { 18, 21, 29, 255 }, 0.40f));
        DrawText(labels[i], x + 18, y, 18, color);
        if (i < kActionCount)
        {
            const std::string bindingText = std::string(KeyLabel(keys[i])) + " / "
                + (i < 4 ? "LS" : GamepadButtonLabel(padButtons[i]));
            DrawText(bindingText.c_str(), x + 276, y, 16, color);
        }
    }
}

void Game::RenderPauseOverlay() const
{
    const char* labels[] { "Продолжить", "Перезапустить матч", "Настройки", "Главное меню", "Выйти из игры" };
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.52f));
    DrawCenteredText("Пауза", GetScreenHeight() / 2 - 154, 42, WHITE);

    const int panelWidth = 360;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = GetScreenHeight() / 2 - 92;
    DrawRectangle(panelX, panelY, panelWidth, 224, Fade(Color { 8, 10, 14, 255 }, 0.90f));
    DrawRectangleLines(panelX, panelY, panelWidth, 224, Fade(WHITE, 0.22f));
    for (int i = 0; i < 5; ++i)
    {
        const int y = panelY + 24 + i * 39;
        const bool selected = i == pauseIndex_;
        DrawRectangle(panelX + 18, y - 7, panelWidth - 36, 29, selected ? Fade(Color { 42, 52, 62, 255 }, 0.95f) : Fade(Color { 18, 21, 29, 255 }, 0.45f));
        DrawText(labels[i], panelX + 38, y, 20, selected ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.80f));
    }
}

void Game::RenderGameHints(const Player& localPlayer) const
{
    const KeyBindings& bindings = input_.GetBindings();
    const std::string mainHints = std::string(KeyLabel(bindings.interact)) + " магазин"
        + " | " + KeyLabel(bindings.sneak) + " присесть"
#if DAIBED_DEVELOPER_BUILD
        + " | " + KeyLabel(bindings.bridgeMode) + " мост"
#endif
        + " | " + KeyLabel(bindings.sprint) + " / двойной " + KeyLabel(bindings.moveForward) + " спринт"
        + " | " + KeyLabel(bindings.cameraToggle) + " камера"
        + " | Tab таблица";
    const std::string combatHints = std::string(KeyLabel(bindings.heroActive1)) + "/"
        + KeyLabel(bindings.heroActive2) + "/"
        + KeyLabel(bindings.heroUltimate) + " способности"
        + " | " + KeyLabel(bindings.inventory) + " инвентарь"
        + " | " + KeyLabel(bindings.drop) + " выброс"
        + " | RMB использовать/ставить"
        + " | утилиты " + KeyLabel(bindings.shoot) + "/" + KeyLabel(bindings.fireball) + "/" + KeyLabel(bindings.heal)
        + "/" + KeyLabel(bindings.teleport) + "/" + KeyLabel(bindings.dash) + "/" + KeyLabel(bindings.molotov) + "/" + KeyLabel(bindings.alarm)
        + " | Esc пауза";
    const auto drawHintLine = [](const std::string& text, int y)
    {
        const int textWidth = MeasureText(text.c_str(), 14);
        const int x = std::max(12, GetScreenWidth() / 2 - textWidth / 2);
        DrawText(text.c_str(), x, y, 14, Fade(WHITE, 0.56f));
    };
    drawHintLine(mainHints, GetScreenHeight() - 38);
    drawHintLine(combatHints, GetScreenHeight() - 20);

    if (!localPlayer.IsAlive())
    {
        DrawCenteredText("Waiting to respawn", GetScreenHeight() / 2 + 96, 24, ORANGE);
    }
}

void Game::RenderOnboarding(const Player& localPlayer) const
{
    const Team* team = FindTeam(localPlayer.GetTeamId());
    const HeroDefinition& hero = HeroSystem::GetDefinition(localPlayer.GetHeroId());
    std::string title = tutorialMode_ ? "ОБУЧЕНИЕ" : "ПЕРВЫЙ МАТЧ";
    std::string instruction;
    if (matchTime_ < 12.0f)
    {
        instruction = "EnergyCore дает респаун. Защищайте его блоками и не падайте в воид.";
    }
    else if (matchTime_ < 24.0f)
    {
        instruction = "Собирайте железо и золото у генератора. Кристаллы находятся ближе к центру.";
    }
    else if (matchTime_ < 36.0f)
    {
        instruction = std::string(KeyLabel(input_.GetBindings().interact)) + " открывает магазин у любой базы. Купите блоки или улучшение.";
    }
    else if (matchTime_ < 48.0f)
    {
        instruction = "RMB ставит блок. Удерживайте Shift у края для безопасного моста.";
    }
    else if (matchTime_ < 60.0f)
    {
        instruction = "Удерживайте ЛКМ для зарядки лука и бластера. ПКМ включает оптику снайперской винтовки; колесо меняет увеличение.";
    }
    else
    {
        instruction = hero.name + ": " + KeyLabel(input_.GetBindings().heroActive1) + "/"
            + KeyLabel(input_.GetBindings().heroActive2) + " активки, "
            + KeyLabel(input_.GetBindings().heroUltimate) + " ульта. Уничтожьте вражеский Core.";
    }

    if (team != nullptr && !team->coreAlive)
    {
        instruction = "Ваш Core разрушен: следующая смерть финальная. Играйте осторожно.";
    }

    const int width = std::min(760, GetScreenWidth() - 40);
    const int x = GetScreenWidth() / 2 - width / 2;
    const int y = 118;
    DrawRectangle(x, y, width, 66, Fade(Color { 8, 10, 14, 255 }, 0.76f));
    DrawRectangleLines(x, y, width, 66, Fade(Color { 112, 232, 255, 255 }, 0.42f));
    DrawText(title.c_str(), x + 16, y + 10, 16, Color { 112, 232, 255, 255 });
    DrawText(instruction.c_str(), x + 16, y + 36, 16, Fade(WHITE, 0.86f));
}

void Game::RenderMinimap(const Player& localPlayer) const
{
    constexpr int mapSize = 178;
    constexpr float arenaExtent = 48.0f;
    const int x = GetScreenWidth() - mapSize - 18;
    const int y = 50;
    DrawRectangle(x, y, mapSize, mapSize, Fade(Color { 8, 10, 14, 255 }, 0.70f));
    DrawRectangleLines(x, y, mapSize, mapSize, Fade(WHITE, 0.25f));
    DrawText("Map", x + 10, y + 8, 16, Fade(WHITE, 0.70f));

    const auto project = [x, y, mapSize, arenaExtent](Vector3 position)
    {
        const float nx = std::clamp((position.x + arenaExtent) / (arenaExtent * 2.0f), 0.0f, 1.0f);
        const float nz = std::clamp((position.z + arenaExtent) / (arenaExtent * 2.0f), 0.0f, 1.0f);
        return Vector2 {
            static_cast<float>(x + 8) + nx * static_cast<float>(mapSize - 16),
            static_cast<float>(y + 8) + nz * static_cast<float>(mapSize - 16)
        };
    };

    for (const auto& entry : world_.GetBlocks())
    {
        if (entry.first.y < -1 || entry.first.y > 2)
        {
            continue;
        }
        const Vector2 p = project(world_.GridToWorld(entry.first));
        Color color = Fade(Color { 110, 118, 134, 255 }, 0.52f);
        if (entry.second.teamId >= 0)
        {
            const Team* team = FindTeam(entry.second.teamId);
            color = team != nullptr ? Fade(GetTeamColor(team->color), 0.62f) : color;
        }
        DrawRectangle(static_cast<int>(p.x), static_cast<int>(p.y), 2, 2, color);
    }

    for (const EnergyCore& core : cores_)
    {
        if (!core.IsAlive())
        {
            continue;
        }
        const Team* team = FindTeam(core.GetTeamId());
        const Vector2 p = project(world_.GridToWorld(core.GetBlockPosition()));
        DrawTeamMarker(p, core.GetTeamId(), 5.0f, team != nullptr ? GetTeamColor(team->color) : WHITE);
    }

    for (const Player& player : players_)
    {
        if (!player.IsAlive())
        {
            continue;
        }
        const Vector2 p = project(player.GetPosition());
        const Team* team = FindTeam(player.GetTeamId());
        const Color color = player.GetId() == localPlayer.GetId()
            ? WHITE
            : (team != nullptr ? GetTeamColor(team->color) : ORANGE);
        DrawTeamMarker(p, player.GetTeamId(), player.GetId() == localPlayer.GetId() ? 4.5f : 3.5f, color);
    }
}

void Game::RenderCoreCollapseTimer() const
{
    if (winnerTeamId_.has_value())
    {
        return;
    }

    const float remaining = std::max(0.0f, kCoreCollapseSeconds - matchTime_);
    const int minutes = static_cast<int>(remaining) / 60;
    const int seconds = static_cast<int>(remaining) % 60;
    std::string text = "Core collapse ";
    text += std::to_string(minutes) + ":";
    if (seconds < 10)
    {
        text += "0";
    }
    text += std::to_string(seconds);

    const Color color = remaining <= 60.0f ? Color { 255, 118, 118, 255 } : Fade(WHITE, 0.72f);
    // Top-left corner: the centered event feed occupies the top-center column, so a
    // centered timer here collided with it. Left-align to keep both readable.
    DrawText(text.c_str(), 18, 18, 18, color);
}

void Game::RenderKillFeed() const
{
    int y = 92;
    for (auto it = killFeed_.rbegin(); it != killFeed_.rend(); ++it)
    {
        const float t = 1.0f - std::clamp(it->age / std::max(0.001f, it->lifetime), 0.0f, 1.0f);
        const int width = std::min(360, MeasureText(it->text.c_str(), 16) + 18);
        const int x = GetScreenWidth() - width - 18;
        DrawRectangle(x, y, width, 24, Fade(BLACK, 0.46f * t));
        DrawText(it->text.c_str(), x + 9, y + 5, 16, Fade(it->color, t));
        y += 28;
    }
}

void Game::RenderScoreboard() const
{
    const int width = std::min(1040, GetScreenWidth() - 80);
    const int rowHeight = 28;
    const int height = std::min(GetScreenHeight() - 96, 88 + static_cast<int>(players_.size()) * rowHeight);
    const int x = GetScreenWidth() / 2 - width / 2;
    const int y = 58;

    DrawRectangle(x, y, width, height, Fade(Color { 7, 9, 14, 255 }, 0.88f));
    DrawRectangleLines(x, y, width, height, Fade(WHITE, 0.28f));
    const std::string title = std::string("Таблица матча  |  ") + MatchModeName()
        + "  |  " + TeamSizeName()
        + "  |  Боты " + BotCountName();
    DrawText(title.c_str(), x + 18, y + 16, 20, WHITE);

    const int headerY = y + 52;
    DrawRectangle(x + 14, headerY - 7, width - 28, 26, Fade(Color { 30, 36, 46, 255 }, 0.82f));
    DrawText("Команда", x + 26, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("Игрок", x + 118, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("Класс", x + 314, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("HP", x + 492, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("Состояние", x + 582, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("Кор", x + 734, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("K/D", x + 842, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("Урон Кору", x + 910, headerY, 16, Fade(WHITE, 0.70f));

    for (int i = 0; i < static_cast<int>(players_.size()); ++i)
    {
        const Player& player = players_[i];
        const Team* team = FindTeam(player.GetTeamId());
        const PlayerMatchScore* score = FindPlayerScore(player.GetId());
        const int rowY = headerY + 32 + i * rowHeight;
        if (rowY + 22 > y + height - 8)
        {
            break;
        }

        const Color teamColor = team != nullptr ? GetTeamColor(team->color) : Fade(WHITE, 0.70f);
        const bool local = player.GetId() == localPlayerId_;
        DrawRectangle(x + 14, rowY - 5, width - 28, 24, local ? Fade(Color { 56, 64, 76, 255 }, 0.82f) : Fade(Color { 14, 17, 24, 255 }, 0.48f));
        DrawText(team != nullptr ? team->name.c_str() : "?", x + 26, rowY, 16, teamColor);
        DrawText((player.GetName() + (local ? " (вы)" : "")).c_str(), x + 118, rowY, 16, player.IsAlive() ? WHITE : Fade(WHITE, 0.46f));
        const HeroDefinition& hero = HeroSystem::GetDefinition(player.GetHeroId());
        DrawText(hero.name.c_str(), x + 314, rowY, 15, player.IsAlive() ? Fade(WHITE, 0.72f) : Fade(WHITE, 0.38f));

        const std::string hp = player.IsAlive()
            ? std::to_string(player.GetHealth()) + "/" + std::to_string(player.GetMaxHealth())
            : "-";
        DrawText(hp.c_str(), x + 492, rowY, 16, player.IsAlive() ? Color { 128, 238, 166, 255 } : Fade(WHITE, 0.42f));

        std::string state = "Жив";
        Color stateColor = Color { 128, 238, 166, 255 };
        if (player.IsEliminated())
        {
            state = "Финальная смерть";
            stateColor = Color { 255, 118, 118, 255 };
        }
        else if (!player.IsAlive())
        {
            state = "Респаун " + std::to_string(static_cast<int>(std::ceil(player.GetRespawnTimer()))) + "с";
            stateColor = Color { 255, 190, 122, 255 };
        }
        DrawText(state.c_str(), x + 582, rowY, 16, stateColor);

        const bool coreAlive = team != nullptr && team->coreAlive;
        DrawText(coreAlive ? "Цел" : "Сломан", x + 734, rowY, 16, coreAlive ? Color { 112, 232, 255, 255 } : Color { 255, 118, 118, 255 });

        const int kills = score != nullptr ? score->kills : 0;
        const int deaths = score != nullptr ? score->deaths : 0;
        const int coreDamage = score != nullptr ? score->coreDamage : 0;
        DrawText((std::to_string(kills) + "/" + std::to_string(deaths)).c_str(), x + 842, rowY, 16, Fade(WHITE, 0.82f));
        DrawText(std::to_string(coreDamage).c_str(), x + 910, rowY, 16, Fade(WHITE, 0.82f));
    }

    DrawText("Удерживайте Tab для просмотра | Игроки с финальной смертью могут наблюдать", x + 18, y + height - 24, 14, Fade(WHITE, 0.52f));
}

void Game::RenderDeathOverlay(const Player& localPlayer) const
{
    if (localPlayer.IsAlive())
    {
        return;
    }

    const bool finalDeath = localPlayer.IsEliminated();
    if (localDeathOverlayTimer_ <= 0.0f)
    {
        return;
    }
    const int width = 420;
    const int height = finalDeath ? 178 : 164;
    const int x = GetScreenWidth() / 2 - width / 2;
    const int y = GetScreenHeight() / 2 - height / 2;
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, finalDeath ? 0.20f : 0.34f));
    DrawRectangle(x, y, width, height, Fade(Color { 8, 10, 14, 255 }, 0.82f));
    DrawRectangleLines(x, y, width, height, finalDeath ? Fade(RED, 0.55f) : Fade(ORANGE, 0.55f));
    DrawCenteredText(finalDeath ? "FINAL DEATH" : "YOU DIED", y + 22, 28, finalDeath ? Color { 255, 118, 118, 255 } : ORANGE);
    if (finalDeath)
    {
        DrawCenteredText(("Убийца: " + localDeathKiller_).c_str(), y + 62, 18, Fade(WHITE, 0.82f));
        DrawCenteredText(("Причина: " + localDeathCause_).c_str(), y + 88, 18, Fade(WHITE, 0.72f));
        DrawCenteredText("Вы выбыли. Режим наблюдателя включен.", y + 118, 16, Fade(WHITE, 0.70f));
        DrawCenteredText("Left/Right цель | F свободная камера | Tab таблица", y + 146, 14, Fade(WHITE, 0.58f));
    }
    else
    {
        DrawCenteredText(("Убийца: " + localDeathKiller_ + " | " + localDeathCause_).c_str(), y + 62, 17, Fade(WHITE, 0.78f));
        const std::string respawn = "Respawning in "
            + std::to_string(static_cast<int>(std::ceil(localPlayer.GetRespawnTimer()))) + "s";
        DrawCenteredText(respawn.c_str(), y + 100, 20, Fade(WHITE, 0.78f));
    }
}

void Game::RenderSpectatorOverlay() const
{
    const Player* target = GetSpectatorTarget();
    const std::string targetText = spectatorFreeCamera_
        ? "Free camera"
        : (target != nullptr ? "Following " + target->GetName() : "No living players");
    const int width = 520;
    const int height = 58;
    const int x = GetScreenWidth() / 2 - width / 2;
    const int y = GetScreenHeight() - height - 54;
    DrawRectangle(x, y, width, height, Fade(Color { 7, 9, 14, 255 }, 0.70f));
    DrawRectangleLines(x, y, width, height, Fade(WHITE, 0.22f));
    DrawCenteredText(("SPECTATOR  |  " + targetText).c_str(), y + 10, 18, WHITE);
    DrawCenteredText("Left/Right target  |  F free/follow  |  WASD Space Ctrl free camera", y + 34, 14, Fade(WHITE, 0.60f));
}

void Game::RenderChestOverlay() const
{
    if (!teamChestOpen_ && !personalChestOpen_)
    {
        return;
    }

    const Player* player = GetLocalPlayer();
    if (player == nullptr)
    {
        return;
    }
    const Inventory& chest = teamChestOpen_
        ? teamChests_[std::clamp(player->GetTeamId(), 0, static_cast<int>(teamChests_.size()) - 1)]
        : personalChest_;

    const int x = GetScreenWidth() / 2 + 320;
    const int y = GetScreenHeight() / 2 - 150;
    DrawRectangle(x, y, 250, 190, Fade(BLACK, 0.76f));
    DrawRectangleLines(x, y, 250, 190, Fade(WHITE, 0.24f));
    DrawText(teamChestOpen_ ? "Team chest" : "Personal chest", x + 14, y + 14, 20, WHITE);
    const auto& slots = chest.GetHotbarSlots();
    for (int i = 0; i < kHotbarSlotCount; ++i)
    {
        const int sx = x + 14 + (i % 3) * 74;
        const int sy = y + 52 + (i / 3) * 34;
        DrawRectangle(sx, sy, 62, 26, Fade(Color { 24, 28, 36, 255 }, 0.80f));
        DrawRectangleLines(sx, sy, 62, 26, Fade(WHITE, 0.18f));
        if (!slots[i].IsEmpty())
        {
            std::string label = std::string(ItemShortName(slots[i].type)) + " " + std::to_string(slots[i].count);
            DrawText(label.c_str(), sx + 5, sy + 7, 12, WHITE);
        }
    }
}

void Game::RenderCompass(const Player& localPlayer) const
{
    const Player* nearest = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    for (const Player& player : players_)
    {
        if (player.GetId() == localPlayer.GetId()
            || player.GetTeamId() == localPlayer.GetTeamId()
            || !player.IsAlive()
            || player.IsEliminated())
        {
            continue;
        }
        const float distance = DistanceSquared(localPlayer.GetPosition(), player.GetPosition());
        if (distance < bestDistance)
        {
            bestDistance = distance;
            nearest = &player;
        }
    }

    if (nearest == nullptr)
    {
        return;
    }

    const Vector3 toEnemy = Normalize2D(Vector3 {
        nearest->GetPosition().x - localPlayer.GetPosition().x,
        0.0f,
        nearest->GetPosition().z - localPlayer.GetPosition().z
    });
    const Vector3 forward = cameraController_.GetFlatForward();
    const Vector3 right = cameraController_.GetFlatRight();
    const float dotForward = forward.x * toEnemy.x + forward.z * toEnemy.z;
    const float dotRight = right.x * toEnemy.x + right.z * toEnemy.z;
    const float angle = std::atan2(dotRight, dotForward);
    const int cx = GetScreenWidth() / 2;
    const int y = 86;
    const int dx = static_cast<int>(std::sin(angle) * 84.0f);
    DrawRectangle(cx - 110, y - 18, 220, 34, Fade(BLACK, 0.35f));
    DrawLine(cx + dx, y - 10, cx + dx, y + 10, Color { 255, 118, 118, 255 });
    DrawText("Nearest enemy", cx - 54, y - 12, 14, Fade(WHITE, 0.66f));
    const std::string meters = std::to_string(static_cast<int>(std::sqrt(bestDistance))) + "m";
    DrawText(meters.c_str(), cx + 64, y - 12, 14, Color { 255, 118, 118, 255 });
}

void Game::RenderBotDebug() const
{
    for (const Player& player : players_)
    {
        if (player.IsLocal() || !player.IsAlive())
        {
            continue;
        }

        const BotMemory* memory = nullptr;
        for (const BotMemory& candidate : botMemories_)
        {
            if (candidate.playerId == player.GetId())
            {
                memory = &candidate;
                break;
            }
        }
        if (memory == nullptr)
        {
            continue;
        }

        const Vector3 labelPoint {
            player.GetPosition().x,
            player.GetPosition().y + 2.32f,
            player.GetPosition().z
        };
        const Vector2 screen = GetWorldToScreen(labelPoint, cameraController_.GetCamera());
        if (screen.x < -120.0f || screen.x > static_cast<float>(GetScreenWidth()) + 120.0f
            || screen.y < -80.0f || screen.y > static_cast<float>(GetScreenHeight()) + 80.0f)
        {
            continue;
        }

        const std::string label = std::string(ToString(memory->role)) + " / " + ToString(memory->intent)
            + " " + std::to_string(static_cast<int>(memory->intentScore))
            + " / plan: " + ToString(memory->currentPlan.goal)
            + (memory->currentPlan.targetTeamId >= 0 ? "->" + std::to_string(memory->currentPlan.targetTeamId) : "")
            + (memory->intentReason.empty() ? "" : " / " + memory->intentReason)
            + (memory->roleReason.empty() ? "" : " / role: " + memory->roleReason);
        const int width = MeasureText(label.c_str(), 14) + 12;
        DrawRectangle(static_cast<int>(screen.x) - width / 2, static_cast<int>(screen.y) - 4, width, 22, Fade(BLACK, 0.55f));
        DrawText(label.c_str(), static_cast<int>(screen.x) - width / 2 + 6, static_cast<int>(screen.y), 14, Color { 255, 235, 142, 255 });
    }
}

void Game::RenderAutomatchOverlay() const
{
    if (!automatch_.active && automatch_.completedRuns <= 0)
    {
        return;
    }

    const int width = 520;
    const int height = 286;
    const int x = 18;
    const int y = 92;
    DrawRectangle(x, y, width, height, Fade(Color { 7, 9, 14, 255 }, 0.84f));
    DrawRectangleLines(x, y, width, height, Fade(WHITE, 0.24f));

    const std::string title = std::string("Automatch ")
        + std::to_string(automatch_.completedRuns)
        + "/"
        + std::to_string(automatch_.targetRuns)
        + (automatch_.active ? " running" : " complete");
    DrawText(title.c_str(), x + 14, y + 12, 20, WHITE);

    const float avgDuration = automatch_.completedRuns > 0
        ? automatch_.totalDuration / static_cast<float>(automatch_.completedRuns)
        : matchTime_;
    const std::string totals = "Wins R/B/G/Y "
        + std::to_string(automatch_.teamWins[0]) + "/"
        + std::to_string(automatch_.teamWins[1]) + "/"
        + std::to_string(automatch_.teamWins[2]) + "/"
        + std::to_string(automatch_.teamWins[3])
        + " | timeouts " + std::to_string(automatch_.timeouts);
    DrawText(totals.c_str(), x + 14, y + 42, 15, Fade(WHITE, 0.74f));

    const std::string flow = "Kills " + std::to_string(automatch_.totalKills)
        + " | Core dmg " + std::to_string(automatch_.totalCoreDamage)
        + " | Final " + std::to_string(automatch_.totalFinalDeaths)
        + " | Cores " + std::to_string(automatch_.totalCoreDestroyed)
        + " | Avg " + std::to_string(static_cast<int>(avgDuration)) + "s"
        + " | Speed " + AutomatchSpeedName();
    DrawText(flow.c_str(), x + 14, y + 64, 15, Fade(WHITE, 0.74f));

    DrawRectangle(x + 12, y + 90, width - 24, 24, Fade(Color { 30, 36, 46, 255 }, 0.72f));
    DrawText("Bot", x + 22, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("Role", x + 190, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("Intent", x + 272, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("K/D", x + 386, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("Core", x + 444, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("Swap", x + 488, y + 96, 14, Fade(WHITE, 0.70f));

    std::vector<const AutomatchBotStats*> sorted;
    sorted.reserve(automatch_.botStats.size());
    for (const AutomatchBotStats& stats : automatch_.botStats)
    {
        sorted.push_back(&stats);
    }
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const AutomatchBotStats* a, const AutomatchBotStats* b)
        {
            const int aImpact = a->coreDamage + a->kills * 80 + a->samples;
            const int bImpact = b->coreDamage + b->kills * 80 + b->samples;
            return aImpact > bImpact;
        });

    const int rows = std::min(5, static_cast<int>(sorted.size()));
    for (int i = 0; i < rows; ++i)
    {
        const AutomatchBotStats& stats = *sorted[i];
        const int rowY = y + 124 + i * 24;
        int roleIndex = 0;
        int intentIndex = 0;
        for (int r = 1; r < 4; ++r)
        {
            if (stats.roleSamples[r] > stats.roleSamples[roleIndex])
            {
                roleIndex = r;
            }
        }
        for (int t = 1; t < 10; ++t)
        {
            if (stats.intentSamples[t] > stats.intentSamples[intentIndex])
            {
                intentIndex = t;
            }
        }

        const Color teamColor = GetTeamColor(static_cast<TeamColor>(std::clamp(stats.teamId, 0, 3)));
        DrawText(stats.name.c_str(), x + 22, rowY, 14, teamColor);
        DrawText(ToString(static_cast<BotRole>(roleIndex)), x + 190, rowY, 14, Fade(WHITE, 0.82f));
        DrawText(ToString(static_cast<BotIntent>(intentIndex)), x + 272, rowY, 14, Fade(WHITE, 0.82f));
        DrawText((std::to_string(stats.kills) + "/" + std::to_string(stats.deaths)).c_str(), x + 386, rowY, 14, Fade(WHITE, 0.82f));
        DrawText(std::to_string(stats.coreDamage).c_str(), x + 444, rowY, 14, Fade(WHITE, 0.82f));
        DrawText(std::to_string(stats.roleChanges).c_str(), x + 492, rowY, 14, Fade(WHITE, 0.82f));
    }

    if (!automatch_.runs.empty())
    {
        const AutomatchRunStats& last = automatch_.runs.back();
        const std::string lastRun = "Last: "
            + std::string(last.timeout ? "timeout" : TeamName(last.winnerTeamId))
            + " | " + std::to_string(static_cast<int>(last.duration)) + "s"
            + " | kills " + std::to_string(last.kills)
            + " | core " + std::to_string(last.coreDamage);
        DrawText(lastRun.c_str(), x + 14, y + height - 46, 14, Fade(WHITE, 0.62f));
        DrawText(last.finishReason.c_str(), x + 14, y + height - 24, 14, Fade(WHITE, 0.62f));
    }
}

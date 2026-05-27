#include "Game.h"

#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

namespace
{
constexpr float kCoreCollapseSeconds = 30.0f * 60.0f;

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
    { 0, "Unlimited" }
};

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

std::string FormatTenths(float value)
{
    const int tenths = static_cast<int>(value * 10.0f + 0.5f);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}
}
void Game::HandleMenuInput()
{
    constexpr int kMenuRows = 11;
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        menuIndex_ = (menuIndex_ + 1) % kMenuRows;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        menuIndex_ = (menuIndex_ + kMenuRows - 1) % kMenuRows;
    }

    const int delta = (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) ? 1 : ((IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) ? -1 : 0);
    if (delta != 0)
    {
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
        SaveSettings();
    }

    if (IsKeyPressed(KEY_ENTER))
    {
        if (menuIndex_ == 0)
        {
            StartSelectedMatch();
        }
        else if (menuIndex_ == 8)
        {
            returnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Settings;
        }
        else if (menuIndex_ == 9)
        {
            controlsReturnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Controls;
        }
        else if (menuIndex_ == 10)
        {
            exitRequested_ = true;
        }
    }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        exitRequested_ = true;
    }
}

void Game::HandleSettingsInput()
{
    constexpr int kSettingsRows = 9;
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        settingsIndex_ = (settingsIndex_ + 1) % kSettingsRows;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        settingsIndex_ = (settingsIndex_ + kSettingsRows - 1) % kSettingsRows;
    }

    const int delta = (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) ? 1 : ((IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) ? -1 : 0);
    if (delta != 0)
    {
        if (settingsIndex_ == 0)
        {
            const float sensitivity = std::clamp(input_.GetMouseSensitivity() + delta * 0.1f, 0.3f, 2.5f);
            input_.SetMouseSensitivity(sensitivity);
        }
        else if (settingsIndex_ == 1)
        {
            fov_ = std::clamp(fov_ + delta * 2.0f, 50.0f, 90.0f);
            gameplayFov_ = fov_;
            cameraController_.SetFov(gameplayFov_);
        }
        else if (settingsIndex_ == 2)
        {
            resolutionIndex_ = (resolutionIndex_ + delta + static_cast<int>(std::size(kWindowResolutions))) % static_cast<int>(std::size(kWindowResolutions));
            ApplyWindowSettings();
        }
        else if (settingsIndex_ == 3)
        {
            fullscreen_ = !fullscreen_;
            ApplyWindowSettings();
        }
        else if (settingsIndex_ == 4)
        {
            fpsLimitIndex_ = (fpsLimitIndex_ + delta + static_cast<int>(std::size(kFpsLimits))) % static_cast<int>(std::size(kFpsLimits));
            ApplyFrameRateLimit();
        }
        else if (settingsIndex_ == 5)
        {
            showControlHints_ = !showControlHints_;
        }
        else if (settingsIndex_ == 6)
        {
            showMinimap_ = !showMinimap_;
        }
        SaveSettings();
    }

    if (IsKeyPressed(KEY_ENTER))
    {
        if (settingsIndex_ == 3)
        {
            fullscreen_ = !fullscreen_;
            ApplyWindowSettings();
        }
        else if (settingsIndex_ == 5)
        {
            showControlHints_ = !showControlHints_;
        }
        else if (settingsIndex_ == 6)
        {
            showMinimap_ = !showMinimap_;
        }
        else if (settingsIndex_ == 7)
        {
            controlsReturnScreen_ = GameScreen::Settings;
            screen_ = GameScreen::Controls;
            waitingForKey_ = false;
        }
        else if (settingsIndex_ == 8)
        {
            SaveSettings();
            screen_ = returnScreen_;
            if (screen_ == GameScreen::Playing)
            {
                DisableCursor();
            }
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
    constexpr int kActionCount = 11;
    constexpr int kControlRows = kActionCount + 1;
    KeyBindings& bindings = input_.MutableBindings();

    const auto setBinding = [&bindings](int index, int key)
    {
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
            bindings.bridgeMode = key;
            break;
        case 7:
            bindings.sprint = key;
            break;
        case 8:
            bindings.interact = key;
            break;
        case 9:
            bindings.cameraToggle = key;
            break;
        case 10:
            bindings.debugRespawn = key;
            break;
        default:
            break;
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
    if (IsKeyPressed(KEY_ENTER))
    {
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
    if (IsKeyPressed(KEY_ESCAPE))
    {
        screen_ = GameScreen::Playing;
        DisableCursor();
        return;
    }
    if (!IsKeyPressed(KEY_ENTER))
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
    const char* labels[] {
        "Start match",
        "Mode",
        "Team",
        "Team size",
        "Bots",
        "Bot difficulty",
        "Arena layout",
        "Biome",
        "Settings",
        "Controls",
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
        "",
        ""
    };

    DrawCenteredText("DaiBed", 76, 44, WHITE);
    DrawCenteredText("Choose match setup", 126, 20, Fade(WHITE, 0.72f));

    const int panelWidth = 640;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 146;
    DrawRectangle(panelX, panelY, panelWidth, 386, Fade(Color { 8, 10, 14, 255 }, 0.82f));
    DrawRectangleLines(panelX, panelY, panelWidth, 386, Fade(WHITE, 0.20f));

    for (int i = 0; i < 11; ++i)
    {
        const int y = panelY + 22 + i * 32;
        const bool selected = i == menuIndex_;
        const Color color = selected ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.78f);
        DrawRectangle(panelX + 18, y - 7, panelWidth - 36, 30, selected ? Fade(Color { 42, 52, 62, 255 }, 0.92f) : Fade(Color { 18, 21, 29, 255 }, 0.42f));
        DrawText(labels[i], panelX + 34, y, 20, color);
        if (!values[i].empty())
        {
            DrawText(("< " + values[i] + " >").c_str(), panelX + 340, y, 20, color);
        }
    }

    DrawCenteredText("Arrows/WASD navigate | Left/Right change | Enter select", GetScreenHeight() - 70, 18, Fade(WHITE, 0.62f));
    DrawCenteredText("Vertical arena adds towers, lower bridges and an upper center.", GetScreenHeight() - 42, 16, Fade(WHITE, 0.48f));
}

void Game::RenderSettings() const
{
    const std::string labels[] {
        "Mouse sensitivity",
        "FOV",
        "Resolution",
        "Fullscreen",
        "FPS limit",
        "Control hints",
        "Minimap",
        "Open controls",
        "Back"
    };
    const std::string values[] {
        FormatTenths(input_.GetMouseSensitivity()),
        std::to_string(static_cast<int>(fov_)),
        ResolutionName(),
        fullscreen_ ? "On" : "Off",
        FpsLimitName(),
        showControlHints_ ? "On" : "Off",
        showMinimap_ ? "On" : "Off",
        "",
        ""
    };

    DrawCenteredText("Settings", 86, 40, WHITE);
    const int panelWidth = 620;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 132;
    DrawRectangle(panelX, panelY, panelWidth, 420, Fade(Color { 8, 10, 14, 255 }, 0.86f));
    DrawRectangleLines(panelX, panelY, panelWidth, 420, Fade(WHITE, 0.20f));
    for (int i = 0; i < 9; ++i)
    {
        const int y = panelY + 28 + i * 40;
        const bool selected = i == settingsIndex_;
        const Color color = selected ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.78f);
        DrawRectangle(panelX + 20, y - 8, panelWidth - 40, 31, selected ? Fade(Color { 42, 52, 62, 255 }, 0.92f) : Fade(Color { 18, 21, 29, 255 }, 0.44f));
        DrawText(labels[i].c_str(), panelX + 38, y, 20, color);
        if (!values[i].empty())
        {
            DrawText(("< " + values[i] + " >").c_str(), panelX + 340, y, 20, color);
        }
    }
    DrawCenteredText("Left/Right change values | Enter toggles/selects | Esc back", GetScreenHeight() - 56, 18, Fade(WHITE, 0.62f));
}

void Game::RenderControls() const
{
    const KeyBindings& bindings = input_.GetBindings();
    const char* labels[] {
        "Move forward",
        "Move backward",
        "Move left",
        "Move right",
        "Jump",
        "Sneak",
        "Bridge mode",
        "Sprint hold",
        "Shop / interact",
        "Camera toggle",
        "Debug respawn",
        "Back"
    };
    const int keys[] {
        bindings.moveForward,
        bindings.moveBackward,
        bindings.moveLeft,
        bindings.moveRight,
        bindings.jump,
        bindings.sneak,
        bindings.bridgeMode,
        bindings.sprint,
        bindings.interact,
        bindings.cameraToggle,
        bindings.debugRespawn,
        KEY_NULL
    };

    DrawCenteredText("Controls", 60, 40, WHITE);
    DrawCenteredText(waitingForKey_ ? "Press a key for the selected action. Esc cancels." : "Enter starts rebinding selected action.", 106, 18, Fade(WHITE, 0.64f));

    const int panelWidth = 640;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 142;
    DrawRectangle(panelX, panelY, panelWidth, 448, Fade(Color { 8, 10, 14, 255 }, 0.86f));
    DrawRectangleLines(panelX, panelY, panelWidth, 448, Fade(WHITE, 0.20f));

    for (int i = 0; i < 12; ++i)
    {
        const int y = panelY + 22 + i * 37;
        const bool selected = i == controlsIndex_;
        const Color color = selected ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.78f);
        DrawRectangle(panelX + 20, y - 7, panelWidth - 40, 28, selected ? Fade(Color { 42, 52, 62, 255 }, 0.92f) : Fade(Color { 18, 21, 29, 255 }, 0.40f));
        DrawText(labels[i], panelX + 38, y, 18, color);
        if (i < 11)
        {
            DrawText(KeyLabel(keys[i]), panelX + 420, y, 18, color);
        }
    }
}

void Game::RenderPauseOverlay() const
{
    const char* labels[] { "Resume", "Restart match", "Settings", "Main menu", "Quit" };
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.52f));
    DrawCenteredText("Paused", GetScreenHeight() / 2 - 154, 42, WHITE);

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
    const std::string hints = std::string(KeyLabel(bindings.interact)) + " shop"
        + " | " + KeyLabel(bindings.sneak) + " sneak"
        + " | " + KeyLabel(bindings.bridgeMode) + " bridge"
        + " | " + KeyLabel(bindings.sprint) + " / double " + KeyLabel(bindings.moveForward) + " sprint"
        + " | " + KeyLabel(bindings.cameraToggle) + " camera"
        + " | Tab scoreboard"
        + " | E inventory Q drop wheel hotbar F3 bots"
        + " | RMB use/place | B/G/H/F/T/M/N hotbar utility"
        + " | Esc pause";
    const int y = GetScreenHeight() - 20;
    DrawText(hints.c_str(), GetScreenWidth() / 2 - MeasureText(hints.c_str(), 14) / 2, y, 14, Fade(WHITE, 0.56f));

    if (!localPlayer.IsAlive())
    {
        DrawCenteredText("Waiting to respawn", GetScreenHeight() / 2 + 96, 24, ORANGE);
    }
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

    const auto project = [x, y](Vector3 position)
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
        DrawCircle(static_cast<int>(p.x), static_cast<int>(p.y), 5.0f, team != nullptr ? GetTeamColor(team->color) : WHITE);
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
        DrawCircle(static_cast<int>(p.x), static_cast<int>(p.y), player.GetId() == localPlayer.GetId() ? 4.5f : 3.5f, color);
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
    DrawText(text.c_str(), GetScreenWidth() / 2 - MeasureText(text.c_str(), 18) / 2, 52, 18, color);
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
    const int width = std::min(900, GetScreenWidth() - 80);
    const int rowHeight = 28;
    const int height = std::min(GetScreenHeight() - 96, 88 + static_cast<int>(players_.size()) * rowHeight);
    const int x = GetScreenWidth() / 2 - width / 2;
    const int y = 58;

    DrawRectangle(x, y, width, height, Fade(Color { 7, 9, 14, 255 }, 0.88f));
    DrawRectangleLines(x, y, width, height, Fade(WHITE, 0.28f));
    const std::string title = std::string("Match scoreboard  |  ") + MatchModeName()
        + "  |  " + TeamSizeName()
        + "  |  Bots " + BotCountName();
    DrawText(title.c_str(), x + 18, y + 16, 20, WHITE);

    const int headerY = y + 52;
    DrawRectangle(x + 14, headerY - 7, width - 28, 26, Fade(Color { 30, 36, 46, 255 }, 0.82f));
    DrawText("Team", x + 26, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("Player", x + 118, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("HP", x + 328, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("State", x + 420, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("Core", x + 570, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("K/D", x + 690, headerY, 16, Fade(WHITE, 0.70f));
    DrawText("Core dmg", x + 770, headerY, 16, Fade(WHITE, 0.70f));

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
        DrawText((player.GetName() + (local ? " (you)" : "")).c_str(), x + 118, rowY, 16, player.IsAlive() ? WHITE : Fade(WHITE, 0.46f));

        const std::string hp = player.IsAlive()
            ? std::to_string(player.GetHealth()) + "/" + std::to_string(player.GetMaxHealth())
            : "-";
        DrawText(hp.c_str(), x + 328, rowY, 16, player.IsAlive() ? Color { 128, 238, 166, 255 } : Fade(WHITE, 0.42f));

        std::string state = "Alive";
        Color stateColor = Color { 128, 238, 166, 255 };
        if (player.IsEliminated())
        {
            state = "Final death";
            stateColor = Color { 255, 118, 118, 255 };
        }
        else if (!player.IsAlive())
        {
            state = "Respawn " + std::to_string(static_cast<int>(std::ceil(player.GetRespawnTimer()))) + "s";
            stateColor = Color { 255, 190, 122, 255 };
        }
        DrawText(state.c_str(), x + 420, rowY, 16, stateColor);

        const bool coreAlive = team != nullptr && team->coreAlive;
        DrawText(coreAlive ? "Online" : "Broken", x + 570, rowY, 16, coreAlive ? Color { 112, 232, 255, 255 } : Color { 255, 118, 118, 255 });

        const int kills = score != nullptr ? score->kills : 0;
        const int deaths = score != nullptr ? score->deaths : 0;
        const int coreDamage = score != nullptr ? score->coreDamage : 0;
        DrawText((std::to_string(kills) + "/" + std::to_string(deaths)).c_str(), x + 690, rowY, 16, Fade(WHITE, 0.82f));
        DrawText(std::to_string(coreDamage).c_str(), x + 770, rowY, 16, Fade(WHITE, 0.82f));
    }

    DrawText("Hold Tab to view | Final death players can spectate", x + 18, y + height - 24, 14, Fade(WHITE, 0.52f));
}

void Game::RenderDeathOverlay(const Player& localPlayer) const
{
    if (localPlayer.IsAlive())
    {
        return;
    }

    const bool finalDeath = localPlayer.IsEliminated();
    if (finalDeath && spectatorMode_)
    {
        return;
    }
    const int width = 420;
    const int height = finalDeath ? 126 : 112;
    const int x = GetScreenWidth() / 2 - width / 2;
    const int y = GetScreenHeight() / 2 - height / 2;
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, finalDeath ? 0.20f : 0.34f));
    DrawRectangle(x, y, width, height, Fade(Color { 8, 10, 14, 255 }, 0.82f));
    DrawRectangleLines(x, y, width, height, finalDeath ? Fade(RED, 0.55f) : Fade(ORANGE, 0.55f));
    DrawCenteredText(finalDeath ? "FINAL DEATH" : "YOU DIED", y + 22, 28, finalDeath ? Color { 255, 118, 118, 255 } : ORANGE);
    if (finalDeath)
    {
        DrawCenteredText("Spectator mode enabled", y + 62, 18, Fade(WHITE, 0.78f));
        DrawCenteredText("Left/Right switch target | F free/follow | Tab scoreboard", y + 92, 14, Fade(WHITE, 0.58f));
    }
    else
    {
        const std::string respawn = "Respawning in "
            + std::to_string(static_cast<int>(std::ceil(localPlayer.GetRespawnTimer()))) + "s";
        DrawCenteredText(respawn.c_str(), y + 66, 20, Fade(WHITE, 0.78f));
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
            + (memory->intentReason.empty() ? "" : " / " + memory->intentReason);
        const int width = MeasureText(label.c_str(), 14) + 12;
        DrawRectangle(static_cast<int>(screen.x) - width / 2, static_cast<int>(screen.y) - 4, width, 22, Fade(BLACK, 0.55f));
        DrawText(label.c_str(), static_cast<int>(screen.x) - width / 2 + 6, static_cast<int>(screen.y), 14, Color { 255, 235, 142, 255 });
    }
}

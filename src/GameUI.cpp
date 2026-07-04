#include "Game.h"

#include "HeroSystem.h"
#include "UiText.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

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
    case GAMEPAD_BUTTON_LEFT_FACE_UP: return "D-вверх";
    case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return "D-вправо";
    case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return "D-вниз";
    case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return "D-влево";
    case GAMEPAD_BUTTON_RIGHT_FACE_UP: return "Y";
    case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return "B";
    case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return "A";
    case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return "X";
    case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return "LB";
    case GAMEPAD_BUTTON_LEFT_TRIGGER_2: return "LT";
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return "RB";
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_2: return "RT";
    case GAMEPAD_BUTTON_MIDDLE_LEFT: return "Назад";
    case GAMEPAD_BUTTON_MIDDLE: return "Гайд";
    case GAMEPAD_BUTTON_MIDDLE_RIGHT: return "Старт";
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

std::string TrimAscii(const std::string& value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])))
    {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool ParseMultiplayerAddress(
    const std::string& text,
    std::uint16_t defaultPort,
    std::string& host,
    std::uint16_t& port,
    std::string& error)
{
    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty())
    {
        error = "Сначала введите адрес LAN/WAN.";
        return false;
    }

    host = trimmed;
    port = defaultPort;
    const std::size_t colon = trimmed.rfind(':');
    if (colon != std::string::npos)
    {
        host = TrimAscii(trimmed.substr(0, colon));
        const std::string portText = TrimAscii(trimmed.substr(colon + 1));
        char* parseEnd = nullptr;
        const long parsed = std::strtol(portText.c_str(), &parseEnd, 10);
        if (portText.empty() || parseEnd == portText.c_str() || *parseEnd != '\0'
            || parsed < 1 || parsed > 65535)
        {
            error = "Порт должен быть в диапазоне 1..65535.";
            return false;
        }
        port = static_cast<std::uint16_t>(parsed);
    }

    if (host.empty())
    {
        error = "Адрес сервера не может быть пустым.";
        return false;
    }
    return true;
}

void AppendClipboardText(std::string& value, std::size_t maxLength, bool allowSpaces)
{
    const char* clipboard = GetClipboardText();
    if (clipboard == nullptr)
    {
        return;
    }
    for (const char* c = clipboard; *c != '\0' && value.size() < maxLength; ++c)
    {
        const unsigned char ch = static_cast<unsigned char>(*c);
        if (ch >= 32 && ch < 127 && (allowSpaces || !std::isspace(ch)))
        {
            value.push_back(static_cast<char>(ch));
        }
    }
}

void EditAsciiTextField(std::string& value, std::size_t maxLength, bool allowSpaces)
{
    const bool ctrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrlDown && IsKeyPressed(KEY_V))
    {
        AppendClipboardText(value, maxLength, allowSpaces);
    }
    if (IsKeyPressed(KEY_BACKSPACE) && !value.empty())
    {
        value.pop_back();
    }

    for (int key = GetCharPressed(); key > 0; key = GetCharPressed())
    {
        if (value.size() >= maxLength)
        {
            continue;
        }
        if (key >= 32 && key < 127
            && (allowSpaces || !std::isspace(static_cast<unsigned char>(key))))
        {
            value.push_back(static_cast<char>(key));
        }
    }
}

std::string ClipTextToWidth(std::string text, int maxWidth, int fontSize)
{
    if (MeasureText(text.c_str(), fontSize) <= maxWidth)
    {
        return text;
    }
    while (!text.empty())
    {
        text.erase(text.begin());
        const std::string candidate = "..." + text;
        if (MeasureText(candidate.c_str(), fontSize) <= maxWidth)
        {
            return candidate;
        }
    }
    return "...";
}

// ---------------------------------------------------------------------------
// Menu kit: one shared dark theme + rounded widgets so every menu screen draws
// from the same vocabulary instead of ad-hoc rectangles. raylib 5.0's
// DrawRectangleRounded / DrawRectangleRoundedLines back all of these.
// ---------------------------------------------------------------------------

constexpr Color kMenuPanel { 16, 20, 28, 235 };
constexpr Color kMenuField { 11, 14, 20, 255 };
constexpr Color kMenuRowIdle { 18, 21, 29, 150 };
constexpr Color kMenuRowSel { 40, 50, 60, 240 };
constexpr Color kAccentGold { 255, 235, 142, 255 };
constexpr Color kAccentCyan { 112, 232, 255, 255 };
constexpr Color kAccentGreen { 140, 235, 150, 255 };
constexpr Color kAccentRed { 240, 120, 120, 255 };
constexpr Color kTextBright { 231, 236, 242, 255 };
constexpr Color kTextDim { 170, 180, 195, 255 };
constexpr Color kTextFaint { 120, 130, 145, 255 };

enum class MenuButtonStyle { Accent, Primary, Danger, Ghost };

Color MenuButtonAccent(MenuButtonStyle style)
{
    switch (style)
    {
    case MenuButtonStyle::Accent: return kAccentGold;
    case MenuButtonStyle::Primary: return kAccentCyan;
    case MenuButtonStyle::Danger: return kAccentRed;
    case MenuButtonStyle::Ghost: return kTextDim;
    }
    return kTextDim;
}

int CenteredTextY(const Rectangle& rect, int fontSize)
{
    return static_cast<int>(rect.y + (rect.height - static_cast<float>(fontSize)) * 0.5f);
}

void MenuPanel(Rectangle rect)
{
    DrawRectangleRounded(rect, 0.045f, 8, kMenuPanel);
    DrawRectangleRoundedLines(rect, 0.045f, 8, 1.0f, Fade(WHITE, 0.10f));
}

void MenuRow(Rectangle rect, bool selected, Color accent)
{
    DrawRectangleRounded(rect, 0.32f, 6, selected ? kMenuRowSel : kMenuRowIdle);
    if (selected)
    {
        DrawRectangleRoundedLines(rect, 0.32f, 6, 1.0f, Fade(accent, 0.55f));
    }
}

// Label on the left, optional value right-aligned. Used by main menu / settings
// / controls list rows.
void MenuLabelValueRow(Rectangle rect, const char* label, const std::string& value,
                       bool selected, Color accent, int fontSize = 20)
{
    MenuRow(rect, selected, accent);
    const Color color = selected ? accent : Fade(kTextBright, 0.82f);
    const int ty = CenteredTextY(rect, fontSize);
    DrawText(label, static_cast<int>(rect.x) + 22, ty, fontSize, color);
    if (!value.empty())
    {
        const int vw = MeasureText(value.c_str(), fontSize);
        DrawText(value.c_str(), static_cast<int>(rect.x + rect.width) - 22 - vw, ty, fontSize, color);
    }
}

// Label on the left, "< value >" stepper group right-aligned.
void MenuStepperRow(Rectangle rect, const char* label, const std::string& value,
                    bool selected, int fontSize = 20)
{
    MenuRow(rect, selected, kAccentCyan);
    const Color labelColor = selected ? kAccentGold : Fade(kTextBright, 0.82f);
    const Color chevron = selected ? kAccentCyan : Fade(kAccentCyan, 0.55f);
    const int ty = CenteredTextY(rect, fontSize);
    DrawText(label, static_cast<int>(rect.x) + 22, ty, fontSize, labelColor);

    const int right = static_cast<int>(rect.x + rect.width) - 22;
    DrawText(">", right - 10, ty, fontSize, chevron);
    const int vw = MeasureText(value.c_str(), fontSize);
    const int vx = right - 10 - 14 - vw;
    DrawText(value.c_str(), vx, ty, fontSize, labelColor);
    DrawText("<", vx - 18, ty, fontSize, chevron);
}

// Label on the left, a sliding on/off pill on the right.
void MenuTogglePill(Rectangle rect, const char* label, bool on, bool selected, int fontSize = 18)
{
    MenuRow(rect, selected, on ? kAccentGreen : kTextFaint);
    DrawText(label, static_cast<int>(rect.x) + 18, CenteredTextY(rect, fontSize), fontSize,
             selected ? kAccentGold : Fade(kTextBright, 0.82f));

    const float pw = 38.0f;
    const float ph = 20.0f;
    const Rectangle pill {
        rect.x + rect.width - 18.0f - pw,
        rect.y + (rect.height - ph) * 0.5f,
        pw,
        ph
    };
    DrawRectangleRounded(pill, 1.0f, 8, on ? Fade(kAccentGreen, 0.30f) : Fade(WHITE, 0.10f));
    const float knob = ph - 6.0f;
    const float kx = on ? pill.x + pill.width - knob - 3.0f : pill.x + 3.0f;
    DrawCircle(static_cast<int>(kx + knob * 0.5f), static_cast<int>(pill.y + ph * 0.5f),
               knob * 0.5f, on ? kAccentGreen : Fade(WHITE, 0.55f));
}

// Horizontal segmented control (tabs, public/private). Returns nothing; the
// caller owns hit-testing and which segment is active.
void MenuSegmented(Rectangle rect, const char* const* labels, int count, int active,
                   bool selected, Color accent, int fontSize = 16)
{
    DrawRectangleRounded(rect, 0.5f, 6, kMenuField);
    if (selected)
    {
        DrawRectangleRoundedLines(rect, 0.5f, 6, 1.0f, Fade(accent, 0.5f));
    }
    const float pad = 3.0f;
    const float segW = (rect.width - pad * 2.0f) / static_cast<float>(count);
    for (int i = 0; i < count; ++i)
    {
        const Rectangle seg {
            rect.x + pad + segW * static_cast<float>(i),
            rect.y + pad,
            segW,
            rect.height - pad * 2.0f
        };
        const bool isActive = i == active;
        if (isActive)
        {
            DrawRectangleRounded(seg, 0.5f, 6, Fade(accent, 0.16f));
        }
        const Color tc = isActive ? accent : Fade(kTextDim, 0.85f);
        const int tw = MeasureText(labels[i], fontSize);
        DrawText(labels[i], static_cast<int>(seg.x + (seg.width - tw) * 0.5f),
                 CenteredTextY(seg, fontSize), fontSize, tc);
    }
}

// Boxed text field (caller draws its own label). Shows a blink caret when
// focused, a placeholder when empty/unfocused.
void MenuTextField(Rectangle rect, const std::string& value, bool masked, bool focused,
                   bool caretOn, const char* placeholder = "", int fontSize = 16)
{
    DrawRectangleRounded(rect, 0.30f, 6, kMenuField);
    DrawRectangleRoundedLines(rect, 0.30f, 6, 1.0f,
                              focused ? Fade(kAccentGold, 0.70f) : Fade(WHITE, 0.12f));

    std::string shown = masked ? std::string(value.size(), '*') : value;
    const bool empty = shown.empty();
    if (empty && !focused)
    {
        shown = placeholder;
    }
    if (focused && caretOn)
    {
        shown.push_back('|');
    }
    const std::string clipped = ClipTextToWidth(shown, static_cast<int>(rect.width) - 22, fontSize);
    const Color tc = (empty && !focused) ? Fade(kTextFaint, 0.85f) : kTextBright;
    DrawText(clipped.c_str(), static_cast<int>(rect.x) + 12, CenteredTextY(rect, fontSize), fontSize, tc);
}

void MenuButton(Rectangle rect, const char* label, MenuButtonStyle style, bool selected,
                bool enabled, int fontSize = 18)
{
    const Color accent = MenuButtonAccent(style);
    const float fillAlpha = !enabled ? 0.05f : (style == MenuButtonStyle::Ghost ? 0.0f : (selected ? 0.20f : 0.13f));
    if (fillAlpha > 0.001f)
    {
        DrawRectangleRounded(rect, 0.30f, 6, Fade(accent, fillAlpha));
    }
    const float borderAlpha = !enabled ? 0.22f : (selected ? 0.95f : 0.48f);
    DrawRectangleRoundedLines(rect, 0.30f, 6, selected ? 1.6f : 1.0f, Fade(accent, borderAlpha));

    const Color tc = enabled ? accent : Fade(accent, 0.45f);
    const int tw = MeasureText(label, fontSize);
    DrawText(label, static_cast<int>(rect.x + (rect.width - tw) * 0.5f), CenteredTextY(rect, fontSize), fontSize, tc);
}

// Small caption label above a field/control.
void MenuFieldLabel(const char* label, float x, float y)
{
    DrawText(label, static_cast<int>(x), static_cast<int>(y), 13, Fade(kTextDim, 0.85f));
}

std::string CurrentExecutablePath()
{
#if defined(_WIN32)
    std::string directory = GetApplicationDirectory();
    if (!directory.empty() && directory.back() != '\\' && directory.back() != '/')
    {
        directory.push_back('\\');
    }
    return directory + "DaiBed.exe";
#else
    std::vector<char> buffer(4096);
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0)
    {
        return "./DaiBed";
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
#endif
}

// Shared geometry for the Multiplayer screen so RenderMultiplayerMenu and
// HandleMultiplayerInput always agree on where every control sits. Mirrors the
// pattern of BuildLobbyUiLayout.
struct MultiplayerLayout
{
    int tab = 0;
    Rectangle panel {};
    Rectangle tabBar {};
    Rectangle tabJoin {};
    Rectangle tabHost {};
    Rectangle joinAddress {};
    Rectangle joinName {};
    Rectangle joinPassword {};
    Rectangle joinConnect {};
    Rectangle joinBack {};
    Rectangle hostServerName {};
    Rectangle hostBiome {};
    Rectangle hostPort {};
    Rectangle hostMode {};
    Rectangle hostPassword {};
    Rectangle hostTeamSize {};
    Rectangle hostVisibility {};
    Rectangle hostMaxPlayers {};
    Rectangle hostRequireReady {};
    Rectangle hostUniqueHeroes {};
    Rectangle hostStatus {};
    Rectangle hostCreate {};
    Rectangle hostStop {};
    Rectangle hostCopy {};
    Rectangle hostBack {};
};

MultiplayerLayout BuildMultiplayerLayout(int tab)
{
    MultiplayerLayout layout;
    layout.tab = tab;
    const float sw = static_cast<float>(GetScreenWidth());
    const float panelW = 760.0f;
    const float panelX = sw * 0.5f - panelW * 0.5f;

    const float tabW = 280.0f;
    layout.tabBar = { sw * 0.5f - tabW * 0.5f, 120.0f, tabW, 40.0f };
    layout.tabJoin = { layout.tabBar.x, layout.tabBar.y, tabW * 0.5f, layout.tabBar.height };
    layout.tabHost = { layout.tabBar.x + tabW * 0.5f, layout.tabBar.y, tabW * 0.5f, layout.tabBar.height };

    const float panelY = 178.0f;
    const float contentX = panelX + 26.0f;
    const float contentW = panelW - 52.0f;
    const float fieldH = 34.0f;
    const float labelH = 16.0f;
    const float rowPitch = labelH + fieldH + 14.0f;

    if (tab == 0)
    {
        const float gridTop = panelY + 24.0f;
        layout.joinAddress = { contentX, gridTop + labelH, contentW, fieldH };
        layout.joinName = { contentX, gridTop + rowPitch + labelH, contentW, fieldH };
        layout.joinPassword = { contentX, gridTop + rowPitch * 2.0f + labelH, contentW, fieldH };
        const float btnY = gridTop + rowPitch * 3.0f + 4.0f;
        const float btnH = 42.0f;
        layout.joinConnect = { contentX, btnY, contentW - 164.0f, btnH };
        layout.joinBack = { contentX + contentW - 150.0f, btnY, 150.0f, btnH };
        layout.panel = { panelX, panelY, panelW, (btnY + btnH + 22.0f) - panelY };
    }
    else
    {
        const float colGap = 26.0f;
        const float colW = (contentW - colGap) * 0.5f;
        const float col0 = contentX;
        const float col1 = contentX + colW + colGap;
        const float gridTop = panelY + 22.0f;
        const auto fieldRect = [&](float colX, int row) {
            return Rectangle { colX, gridTop + static_cast<float>(row) * rowPitch + labelH, colW, fieldH };
        };
        layout.hostServerName = fieldRect(col0, 0);
        layout.hostBiome = fieldRect(col1, 0);
        layout.hostPort = fieldRect(col0, 1);
        layout.hostMode = fieldRect(col1, 1);
        layout.hostPassword = fieldRect(col0, 2);
        layout.hostTeamSize = fieldRect(col1, 2);
        layout.hostVisibility = fieldRect(col0, 3);
        layout.hostMaxPlayers = fieldRect(col1, 3);
        const float togY = gridTop + 4.0f * rowPitch + labelH - 2.0f;
        const float togH = 40.0f;
        layout.hostRequireReady = { col0, togY, colW, togH };
        layout.hostUniqueHeroes = { col1, togY, colW, togH };
        const float panelH = (togY + togH + 20.0f) - panelY;
        layout.panel = { panelX, panelY, panelW, panelH };
        layout.hostStatus = { panelX, panelY + panelH + 14.0f, panelW, 22.0f };
        const float actY = layout.hostStatus.y + layout.hostStatus.height + 12.0f;
        const float actH = 42.0f;
        layout.hostCreate = { panelX, actY, 158.0f, actH };
        layout.hostStop = { panelX + 170.0f, actY, 120.0f, actH };
        layout.hostCopy = { panelX + 302.0f, actY, 196.0f, actH };
        layout.hostBack = { panelX + panelW - 96.0f, actY, 96.0f, actH };
    }
    return layout;
}

Rectangle PausePanelRect()
{
    constexpr int panelWidth = 360;
    constexpr int panelHeight = 224;
    return Rectangle {
        static_cast<float>(GetScreenWidth() / 2 - panelWidth / 2),
        static_cast<float>(GetScreenHeight() / 2 - 92),
        static_cast<float>(panelWidth),
        static_cast<float>(panelHeight)
    };
}

Rectangle PauseRowRect(int row)
{
    const Rectangle panel = PausePanelRect();
    return Rectangle {
        panel.x + 18.0f,
        panel.y + 24.0f + static_cast<float>(row) * 39.0f - 7.0f,
        panel.width - 36.0f,
        29.0f
    };
}
}
void Game::HandleMenuInput()
{
#if DAIBED_DEVELOPER_BUILD
    constexpr int kMenuRows = 16;
#else
    constexpr int kMenuRows = 7;
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
        return row == 0 || row == 1 || row == 9 || row == 13 || row == 14 || row == 15;
#else
        return row == 0 || row == 1 || row == 2 || row == 4 || row == 5 || row == 6;
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
        if (menuIndex_ == 2)
        {
            const int value = (static_cast<int>(selectedMode_) + delta + 4) % 4;
            selectedMode_ = static_cast<MatchMode>(value);
            selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
            selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
        }
        else if (menuIndex_ == 3)
        {
            const int teamCount = TeamCountForMode();
            selectedTeamId_ = (selectedTeamId_ + delta + teamCount) % teamCount;
        }
        else if (menuIndex_ == 4)
        {
            selectedTeamSize_ = ((selectedTeamSize_ - 1 + delta + 4) % 4) + 1;
            selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
        }
        else if (menuIndex_ == 5)
        {
            const int maxBots = MaxBotCountForSelection();
            selectedBotCount_ = (selectedBotCount_ + delta + maxBots + 1) % (maxBots + 1);
        }
        else if (menuIndex_ == 6)
        {
            const int value = (static_cast<int>(botDifficulty_) + delta + 3) % 3;
            botDifficulty_ = static_cast<BotDifficulty>(value);
        }
        else if (menuIndex_ == 7)
        {
            const int value = (static_cast<int>(arenaLayout_) + delta + 2) % 2;
            arenaLayout_ = static_cast<ArenaLayout>(value);
        }
        else if (menuIndex_ == 8)
        {
            const int value = (static_cast<int>(arenaBiome_) + delta + 5) % 5;
            arenaBiome_ = static_cast<ArenaBiome>(value);
        }
        else if (menuIndex_ == 10)
        {
            automatchRunTarget_ = std::clamp(automatchRunTarget_ + delta, 1, 50);
        }
        else if (menuIndex_ == 11)
        {
            automatchTicksPerFrame_ = std::clamp(automatchTicksPerFrame_ + delta, 1, 32);
        }
        else if (menuIndex_ == 12)
        {
            automatchMaxMinutes_ = std::clamp(automatchMaxMinutes_ + delta, 3, 30);
        }
#else
        if (menuIndex_ == 3)
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
        else if (menuIndex_ == 1)
        {
            multiplayerIndex_ = 0;
            screen_ = GameScreen::Multiplayer;
        }
        else if (menuIndex_ == 9)
        {
            StartAutomatch();
        }
        else if (menuIndex_ == 13)
        {
            returnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Settings;
        }
        else if (menuIndex_ == 14)
        {
            controlsReturnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Controls;
        }
        else if (menuIndex_ == 15)
        {
            exitRequested_ = true;
        }
#else
        if (menuIndex_ == 0)
        {
            multiplayerIndex_ = 0;
            screen_ = GameScreen::Multiplayer;
        }
        else if (menuIndex_ == 1)
        {
            selectedMode_ = MatchMode::FourTeams;
            selectedTeamSize_ = 4;
            selectedBotCount_ = 15;
            arenaLayout_ = ArenaLayout::Classic;
            arenaBiome_ = ArenaBiome::Arena;
            heroSelectIndex_ = HeroSystem::IndexOf(selectedHeroId_);
            screen_ = GameScreen::HeroSelect;
        }
        else if (menuIndex_ == 2)
        {
            StartTutorialMatch();
        }
        else if (menuIndex_ == 4)
        {
            returnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Settings;
        }
        else if (menuIndex_ == 5)
        {
            controlsReturnScreen_ = GameScreen::MainMenu;
            screen_ = GameScreen::Controls;
        }
        else if (menuIndex_ == 6)
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

void Game::StartGuiConnect()
{
    std::string host;
    std::uint16_t port = serverConfig_.port;
    std::string error;
    if (!ParseMultiplayerAddress(multiplayerAddress_, serverConfig_.port, host, port, error))
    {
        multiplayerStatus_ = error;
        return;
    }

    LobbyUpdate lobbyPrefs;
    lobbyPrefs.playerName = TrimAscii(multiplayerPlayerName_);
    lobbyPrefs.ready = false;
    lobbyPrefs.startRequested = false;

    const std::string target = host + ":" + std::to_string(port);
    multiplayerStatus_ = "Подключение к " + target + "...";
    // Stage 4: no nested client loop. The session starts here and the ONE
    // standard main loop drives it from the next frame on; when it ends,
    // StopNetworkClientSession restores the menu state and status text.
    StartNetworkClientSession(host, port, multiplayerPassword_, 0.0, lobbyPrefs);
}

namespace
{
const char* HostBiomeToken(ArenaBiome biome)
{
    switch (biome)
    {
    case ArenaBiome::Arena: return "arena";
    case ArenaBiome::Ice: return "ice";
    case ArenaBiome::Lava: return "lava";
    case ArenaBiome::Space: return "space";
    case ArenaBiome::Ruins: return "ruins";
    }
    return "arena";
}

const char* HostModeToken(MatchMode mode)
{
    switch (mode)
    {
    case MatchMode::SoloVsBots: return "solo";
    case MatchMode::TwoVsTwo: return "2v2";
    case MatchMode::FourTeams: return "ffa";
    case MatchMode::Duel: return "duel";
    }
    return "ffa";
}

const char* HostLayoutToken(ArenaLayout layout)
{
    return layout == ArenaLayout::Vertical ? "vertical" : "classic";
}

std::string JoinCommandFor(const std::string& address, const std::string& password)
{
    std::string command = "DaiBed.exe --connect " + address;
    if (!password.empty())
    {
        command += " --password " + password;
    }
    return command;
}
}

void Game::StopLocalServer()
{
    if (!localServerProcess_.Valid())
    {
        return;
    }
    StopServerProcess(localServerProcess_);
    localServerStartTime_ = 0.0;
    multiplayerStatus_ = "Локальный сервер остановлен.";
}

void Game::StartGuiHostAndConnect()
{
    // hostPortText_ holds a bare port number; parse and range-check it.
    std::uint16_t port = serverConfig_.port;
    {
        char* parseEnd = nullptr;
        const long parsed = std::strtol(hostPortText_.c_str(), &parseEnd, 10);
        if (hostPortText_.empty() || parseEnd == hostPortText_.c_str() || *parseEnd != '\0'
            || parsed < 1 || parsed > 65535)
        {
            multiplayerStatus_ = "Порт должен быть в диапазоне 1..65535.";
            return;
        }
        port = static_cast<std::uint16_t>(parsed);
    }

    // If a previous host is still running, retire it before launching a new one.
    if (localServerProcess_.Valid())
    {
        StopServerProcess(localServerProcess_);
        localServerStartTime_ = 0.0;
    }

    ServerConfig config = serverConfig_;
    config.port = port;
    config.listenAddress = config.privateServer ? "127.0.0.1" : "0.0.0.0";
    config.minPlayersToStart = 1;
    config.teamCount = TeamCountForMode();
    config.maxTeamSize = std::clamp(selectedTeamSize_, 1, 4);
    config.worldBiome = static_cast<int>(arenaBiome_);
    config.worldLayout = static_cast<int>(arenaLayout_);
    config.matchMode = static_cast<int>(selectedMode_);
    if (TrimAscii(config.serverName).empty())
    {
        config.serverName = "Сервер DaiBed";
    }
    serverConfig_ = config;

    std::vector<std::string> args {
        "--host",
        "--listen", config.listenAddress,
        "--port", std::to_string(config.port),
        "--server-name", config.serverName,
        config.privateServer ? "--private" : "--public",
        "--biome", HostBiomeToken(arenaBiome_),
        "--mode", HostModeToken(selectedMode_),
        "--layout", HostLayoutToken(arenaLayout_),
        "--team-size", std::to_string(config.maxTeamSize),
        "--max-players", std::to_string(config.maxPlayers),
        "--min-players", std::to_string(config.minPlayersToStart),
        config.requireAllReady ? "--require-ready" : "--no-require-ready",
        config.enforceUniqueHeroesPerTeam ? "--unique-heroes" : "--no-unique-heroes"
    };
    if (!config.password.empty())
    {
        args.push_back("--password");
        args.push_back(config.password);
    }

    std::string launchError;
    if (!LaunchServerProcess(CurrentExecutablePath(), args, localServerProcess_, launchError))
    {
        multiplayerStatus_ = launchError;
        return;
    }

    localServerStartTime_ = GetTime();
    localServerAddress_ = "127.0.0.1:" + std::to_string(port);
    multiplayerAddress_ = localServerAddress_;
    multiplayerStatus_ = "Сервер запущен на порту " + std::to_string(port) + ".";
    // The join is a non-blocking session now (Stage 4); when it later ends,
    // StopNetworkClientSession restores the menu and sets the status text.
    // The background host process keeps running for reconnects (see
    // StopLocalServer for the explicit shutdown).
    StartGuiConnect();
}

void Game::HandleMultiplayerInput()
{
    if (IsKeyPressed(KEY_ESCAPE))
    {
        screen_ = GameScreen::MainMenu;
        return;
    }

    const MultiplayerLayout layout = BuildMultiplayerLayout(multiplayerTab_);
    const Vector2 mouse = GetMousePosition();

    // --- Tab switching (Q/E or clicking the tab bar). -----------------------
    int newTab = multiplayerTab_;
    if (IsKeyPressed(KEY_Q))
    {
        newTab = 0;
    }
    if (IsKeyPressed(KEY_E))
    {
        newTab = 1;
    }
    bool tabClicked = false;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        if (CheckCollisionPointRec(mouse, layout.tabJoin))
        {
            newTab = 0;
            tabClicked = true;
        }
        else if (CheckCollisionPointRec(mouse, layout.tabHost))
        {
            newTab = 1;
            tabClicked = true;
        }
    }
    if (newTab != multiplayerTab_)
    {
        multiplayerTab_ = newTab;
        multiplayerIndex_ = 0;
        return;
    }
    if (tabClicked)
    {
        return;
    }

    // --- Ordered control list for the active tab. ---------------------------
    std::vector<Rectangle> rects;
    if (multiplayerTab_ == 0)
    {
        rects = { layout.joinAddress, layout.joinName, layout.joinPassword,
                  layout.joinConnect, layout.joinBack };
    }
    else
    {
        rects = { layout.hostServerName, layout.hostBiome, layout.hostPort, layout.hostMode,
                  layout.hostPassword, layout.hostTeamSize, layout.hostVisibility,
                  layout.hostMaxPlayers, layout.hostRequireReady, layout.hostUniqueHeroes,
                  layout.hostCreate, layout.hostStop, layout.hostCopy, layout.hostBack };
    }
    const int count = static_cast<int>(rects.size());
    multiplayerIndex_ = std::clamp(multiplayerIndex_, 0, count - 1);

    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S) || IsKeyPressed(KEY_TAB))
    {
        multiplayerIndex_ = (multiplayerIndex_ + 1) % count;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        multiplayerIndex_ = (multiplayerIndex_ + count - 1) % count;
    }
    const float wheel = GetMouseWheelMove();
    if (wheel < -0.01f)
    {
        multiplayerIndex_ = (multiplayerIndex_ + 1) % count;
    }
    else if (wheel > 0.01f)
    {
        multiplayerIndex_ = (multiplayerIndex_ + count - 1) % count;
    }

    const Vector2 mouseMove = GetMouseDelta();
    int hovered = -1;
    for (int i = 0; i < count; ++i)
    {
        if (CheckCollisionPointRec(mouse, rects[i]))
        {
            hovered = i;
            break;
        }
    }
    if (hovered >= 0 && (std::fabs(mouseMove.x) > 0.5f || std::fabs(mouseMove.y) > 0.5f))
    {
        multiplayerIndex_ = hovered;
    }

    const bool leftClick = hovered >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const bool rightClick = hovered >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    if (leftClick)
    {
        multiplayerIndex_ = hovered;
    }
    const int idx = multiplayerIndex_;
    const bool enter = IsKeyPressed(KEY_ENTER);

    // --- Per-tab text-field editing for the focused control. ----------------
    if (multiplayerTab_ == 0)
    {
        if (idx == 0) EditAsciiTextField(multiplayerAddress_, 64, false);
        else if (idx == 1) EditAsciiTextField(multiplayerPlayerName_, 24, true);
        else if (idx == 2) EditAsciiTextField(multiplayerPassword_, 32, true);
    }
    else
    {
        if (idx == 0)
        {
            EditAsciiTextField(serverConfig_.serverName, 28, true);
        }
        else if (idx == 2)
        {
            EditAsciiTextField(hostPortText_, 5, false);
            std::string digits;
            for (char c : hostPortText_)
            {
                if (c >= '0' && c <= '9') digits.push_back(c);
            }
            hostPortText_ = digits;
        }
        else if (idx == 4)
        {
            EditAsciiTextField(serverConfig_.password, 32, true);
        }
    }

    // --- Value changes (arrows/A-D, left=+1, right=-1) and button presses. --
    int change = (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) ? 1
               : ((IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) ? -1 : 0);
    if (rightClick) change = -1;

    const auto cycle = [](int value, int delta, int modulo) {
        int v = (value + delta) % modulo;
        return v < 0 ? v + modulo : v;
    };

    if (multiplayerTab_ == 0)
    {
        const bool press = enter || leftClick;
        if (idx == 3 && press) StartGuiConnect();
        else if (idx == 4 && press) screen_ = GameScreen::MainMenu;
        return;
    }

    // Host tab. Value controls also respond to a left-click / Enter as "+1".
    if (idx <= 9 && change == 0 && (leftClick || enter))
    {
        change = 1;
    }
    switch (idx)
    {
    case 1:
        if (change) arenaBiome_ = static_cast<ArenaBiome>(cycle(static_cast<int>(arenaBiome_), change, 5));
        break;
    case 3:
        if (change) SetSelectedMode(static_cast<MatchMode>(cycle(static_cast<int>(selectedMode_), change, 4)));
        break;
    case 5:
        if (change) SetSelectedTeamSize(((selectedTeamSize_ - 1 + change + 4) % 4) + 1);
        break;
    case 6:
        if (leftClick)
        {
            serverConfig_.privateServer = mouse.x > layout.hostVisibility.x + layout.hostVisibility.width * 0.5f;
        }
        else if (change)
        {
            serverConfig_.privateServer = !serverConfig_.privateServer;
        }
        break;
    case 7:
        if (change) serverConfig_.maxPlayers = std::clamp(serverConfig_.maxPlayers + change, 2, 32);
        break;
    case 8:
        if (change) serverConfig_.requireAllReady = !serverConfig_.requireAllReady;
        break;
    case 9:
        if (change) serverConfig_.enforceUniqueHeroesPerTeam = !serverConfig_.enforceUniqueHeroesPerTeam;
        break;
    case 10:
        if (enter || leftClick) StartGuiHostAndConnect();
        break;
    case 11:
        if ((enter || leftClick) && IsServerProcessRunning(localServerProcess_)) StopLocalServer();
        break;
    case 12:
        if (enter || leftClick)
        {
            const std::string address = localServerAddress_.empty()
                ? ("127.0.0.1:" + hostPortText_)
                : localServerAddress_;
            SetClipboardText(JoinCommandFor(address, serverConfig_.password).c_str());
            multiplayerStatus_ = "Команда подключения скопирована в буфер обмена.";
        }
        break;
    case 13:
        if (enter || leftClick) screen_ = GameScreen::MainMenu;
        break;
    default:
        break;
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
    const Vector2 mouse = GetMousePosition();
    const Vector2 mouseMove = GetMouseDelta();
    int hoveredRow = -1;
    for (int i = 0; i < kPauseRows; ++i)
    {
        if (CheckCollisionPointRec(mouse, PauseRowRect(i)))
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

bool Game::HandleNetworkPauseInput(bool& requestMainMenu)
{
    constexpr int kNetworkPauseRows = 3;
    pauseIndex_ = std::clamp(pauseIndex_, 0, kNetworkPauseRows - 1);

    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        pauseIndex_ = (pauseIndex_ + 1) % kNetworkPauseRows;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        pauseIndex_ = (pauseIndex_ + kNetworkPauseRows - 1) % kNetworkPauseRows;
    }

    const Vector2 mouse = GetMousePosition();
    const Vector2 mouseMove = GetMouseDelta();
    int hoveredRow = -1;
    for (int i = 0; i < kNetworkPauseRows; ++i)
    {
        if (CheckCollisionPointRec(mouse, PauseRowRect(i)))
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
        pauseIndex_ = (pauseIndex_ + 1) % kNetworkPauseRows;
    }
    else if (wheel > 0.01f)
    {
        pauseIndex_ = (pauseIndex_ + kNetworkPauseRows - 1) % kNetworkPauseRows;
    }

    if (IsKeyPressed(KEY_ESCAPE))
    {
        clientPaused_ = false;
        screen_ = GameScreen::Playing;
        DisableCursor();
        return true;
    }

    const bool mouseActivate = hoveredRow >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (mouseActivate)
    {
        pauseIndex_ = hoveredRow;
    }
    const bool activate = IsKeyPressed(KEY_ENTER) || mouseActivate;
    if (!activate)
    {
        return false;
    }

    if (pauseIndex_ == 0)
    {
        clientPaused_ = false;
        screen_ = GameScreen::Playing;
        DisableCursor();
        return true;
    }
    if (pauseIndex_ == 1)
    {
        returnScreen_ = GameScreen::Paused;
        screen_ = GameScreen::Settings;
        return false;
    }

    requestMainMenu = true;
    return true;
}

void Game::RenderMainMenu() const
{
#if DAIBED_DEVELOPER_BUILD
    const char* labels[] {
        "Начать матч",
        "Сетевая игра",
        "Режим",
        "Команда",
        "Размер команды",
        "Боты",
        "Сложность ботов",
        "План арены",
        "Биом",
        "Запустить автоматч",
        "Прогонов автоматча",
        "Скорость автоматча",
        "Лимит времени автоматча",
        "Настройки",
        "Управление",
        "Выход"
    };
    const std::string values[] {
        "",
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
        "Сетевая игра",
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
        "",
        BotDifficultyName(),
        "",
        "",
        ""
    };
#endif

    DrawCenteredText("DaiBed " DAIBED_VERSION, 48, 40, kTextBright);
#if DAIBED_DEVELOPER_BUILD
    DrawCenteredText("Настройка матча для разработчика", 92, 18, Fade(kTextDim, 0.85f));
#else
    DrawCenteredText("Геройский BedWars против ботов", 92, 18, Fade(kTextDim, 0.85f));
#endif

    const int panelWidth = 640;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 116;
    const int rowCount = static_cast<int>(std::size(labels));
    const int panelHeight = std::max(250, 44 + rowCount * 42);
    MenuPanel(Rectangle { static_cast<float>(panelX), static_cast<float>(panelY),
                          static_cast<float>(panelWidth), static_cast<float>(panelHeight) });

    for (int i = 0; i < rowCount; ++i)
    {
        const int y = panelY + 24 + i * 42;
        const bool selected = i == menuIndex_;
        const Rectangle row { static_cast<float>(panelX + 18), static_cast<float>(y - 7),
                              static_cast<float>(panelWidth - 36), 30.0f };
        const bool isStepper = !values[i].empty();
        if (isStepper)
        {
            MenuStepperRow(row, labels[i], values[i], selected);
        }
        else
        {
            MenuLabelValueRow(row, labels[i], "", selected, kAccentGold);
        }
    }

#if DAIBED_DEVELOPER_BUILD
    std::string biomeHint = "Арена: обычные правила.";
    switch (arenaBiome_)
    {
    case ArenaBiome::Ice:
        biomeHint = "Лед: быстрые маршруты и скользкая поверхность.";
        break;
    case ArenaBiome::Lava:
        biomeHint = "Лава: низины жгут вне безопасной зоны базы.";
        break;
    case ArenaBiome::Space:
        biomeHint = "Космос: низкая гравитация и усиленное отбрасывание.";
        break;
    case ArenaBiome::Ruins:
        biomeHint = "Руины: треснувшие мосты и реликтовые генераторы.";
        break;
    case ArenaBiome::Arena:
        break;
    }
    DrawCenteredText("Стрелки/WASD: выбор | Влево/вправо: изменить | Enter: подтвердить", GetScreenHeight() - 70, 18, Fade(WHITE, 0.62f));
    DrawCenteredText(biomeHint.c_str(), GetScreenHeight() - 42, 16, Fade(WHITE, 0.48f));
#else
    DrawCenteredText("W/S или стрелки: выбор | Enter: подтвердить", GetScreenHeight() - 70, 18, Fade(WHITE, 0.62f));
    DrawCenteredText("Классическая арена | 4 команды по 4 игрока", GetScreenHeight() - 42, 16, Fade(WHITE, 0.48f));
#endif
}

void Game::RenderMultiplayerMenu() const
{
    const MultiplayerLayout layout = BuildMultiplayerLayout(multiplayerTab_);
    const double t = GetTime();
    const bool caretOn = (t - std::floor(t)) < 0.5;
    const auto sel = [this](int control) { return control == multiplayerIndex_; };

    DrawCenteredText("Сетевая игра", 52, 38, kTextBright);
    DrawCenteredText("Создайте сервер со своими правилами или подключитесь по LAN/WAN.", 90, 16,
                     Fade(kTextDim, 0.85f));

    const char* tabLabels[] { "Подключиться", "Создать" };
    MenuSegmented(layout.tabBar, tabLabels, 2, multiplayerTab_, false, kAccentGold, 18);

    MenuPanel(layout.panel);

    if (multiplayerTab_ == 0)
    {
        MenuFieldLabel("Адрес сервера", layout.joinAddress.x, layout.joinAddress.y - 17.0f);
        MenuTextField(layout.joinAddress, multiplayerAddress_, false, sel(0), caretOn, "адрес:порт");
        MenuFieldLabel("Имя игрока", layout.joinName.x, layout.joinName.y - 17.0f);
        MenuTextField(layout.joinName, multiplayerPlayerName_, false, sel(1), caretOn, "Игрок");
        MenuFieldLabel("Пароль (необязательно)", layout.joinPassword.x, layout.joinPassword.y - 17.0f);
        MenuTextField(layout.joinPassword, multiplayerPassword_, true, sel(2), caretOn, "(нет)");
        MenuButton(layout.joinConnect, "Подключиться", MenuButtonStyle::Primary, sel(3), true);
        MenuButton(layout.joinBack, "Назад", MenuButtonStyle::Ghost, sel(4), true);
    }
    else
    {
        // Boxed stepper cell: "< value >" centered, no inner label (caption is
        // drawn above by MenuFieldLabel).
        const auto stepperCell = [](Rectangle r, const std::string& value, bool selected) {
            DrawRectangleRounded(r, 0.30f, 6, kMenuField);
            DrawRectangleRoundedLines(r, 0.30f, 6, 1.0f, selected ? Fade(kAccentGold, 0.70f) : Fade(WHITE, 0.12f));
            const Color chevron = Fade(kAccentCyan, selected ? 1.0f : 0.60f);
            const int ty = CenteredTextY(r, 16);
            DrawText("<", static_cast<int>(r.x) + 12, ty, 16, chevron);
            DrawText(">", static_cast<int>(r.x + r.width) - 12 - MeasureText(">", 16), ty, 16, chevron);
            const int vw = MeasureText(value.c_str(), 16);
            DrawText(value.c_str(), static_cast<int>(r.x + (r.width - vw) * 0.5f), ty, 16, kTextBright);
        };

        MenuFieldLabel("Название сервера", layout.hostServerName.x, layout.hostServerName.y - 16.0f);
        MenuTextField(layout.hostServerName, serverConfig_.serverName, false, sel(0), caretOn, "Сервер DaiBed");
        MenuFieldLabel("Биом", layout.hostBiome.x, layout.hostBiome.y - 16.0f);
        stepperCell(layout.hostBiome, ArenaBiomeName(), sel(1));
        MenuFieldLabel("Порт", layout.hostPort.x, layout.hostPort.y - 16.0f);
        MenuTextField(layout.hostPort, hostPortText_, false, sel(2), caretOn, "7777");
        MenuFieldLabel("Режим", layout.hostMode.x, layout.hostMode.y - 16.0f);
        stepperCell(layout.hostMode, MatchModeName(), sel(3));
        MenuFieldLabel("Пароль", layout.hostPassword.x, layout.hostPassword.y - 16.0f);
        MenuTextField(layout.hostPassword, serverConfig_.password, true, sel(4), caretOn, "(нет)");
        MenuFieldLabel("Размер команды", layout.hostTeamSize.x, layout.hostTeamSize.y - 16.0f);
        stepperCell(layout.hostTeamSize, TeamSizeName(), sel(5));
        MenuFieldLabel("Доступ", layout.hostVisibility.x, layout.hostVisibility.y - 16.0f);
        const char* visLabels[] { "Открытый", "Приватный" };
        MenuSegmented(layout.hostVisibility, visLabels, 2, serverConfig_.privateServer ? 1 : 0, sel(6),
                      kAccentGold, 14);
        MenuFieldLabel("Максимум игроков", layout.hostMaxPlayers.x, layout.hostMaxPlayers.y - 16.0f);
        stepperCell(layout.hostMaxPlayers, std::to_string(serverConfig_.maxPlayers), sel(7));

        MenuTogglePill(layout.hostRequireReady, "Все должны быть готовы", serverConfig_.requireAllReady, sel(8), 16);
        MenuTogglePill(layout.hostUniqueHeroes, "Уникальные герои", serverConfig_.enforceUniqueHeroesPerTeam, sel(9), 16);

        // Status strip.
        const bool running = IsServerProcessRunning(localServerProcess_);
        DrawCircle(static_cast<int>(layout.hostStatus.x) + 7,
                   static_cast<int>(layout.hostStatus.y + layout.hostStatus.height * 0.5f), 5.0f,
                   running ? kAccentGreen : Fade(WHITE, 0.25f));
        std::string statusText;
        if (running)
        {
            const int up = static_cast<int>(std::max(0.0, t - localServerStartTime_));
            char clock[16];
            std::snprintf(clock, sizeof(clock), "%02d:%02d", up / 60, up % 60);
            statusText = "Сервер работает: " + localServerAddress_ + "  |  " + clock;
        }
        else
        {
            statusText = multiplayerStatus_.empty() ? std::string("Локальный сервер не запущен.") : multiplayerStatus_;
        }
        DrawText(statusText.c_str(), static_cast<int>(layout.hostStatus.x) + 22,
                 CenteredTextY(layout.hostStatus, 14), 14, running ? kTextDim : Fade(kTextDim, 0.85f));

        MenuButton(layout.hostCreate, running ? "Перезапуск" : "Создать и войти",
                   MenuButtonStyle::Accent, sel(10), true);
        MenuButton(layout.hostStop, "Остановить", MenuButtonStyle::Danger, sel(11), running);
        MenuButton(layout.hostCopy, "Копировать вход", MenuButtonStyle::Primary, sel(12), true);
        MenuButton(layout.hostBack, "Назад", MenuButtonStyle::Ghost, sel(13), true);
    }

    DrawCenteredText("Q/E: вкладка | W/S/Tab: выбор | </>: изменить | Enter/ЛКМ: действие | Esc: назад",
                     GetScreenHeight() - 48, 15, Fade(kTextDim, 0.70f));
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

    DrawCenteredText("Выбор героя", 42, 38, kTextBright);
    DrawCenteredText("Стрелки/WASD - выбрать | Enter - начать матч | Esc - назад", 86, 18, Fade(kTextDim, 0.80f));
    MenuPanel(Rectangle { static_cast<float>(panelX), static_cast<float>(panelY),
                          static_cast<float>(panelWidth), static_cast<float>(panelHeight) });
    DrawText("Герои", listX, panelY + 22, 22, kTextBright);

    for (int i = 0; i < HeroSystem::kHeroCount; ++i)
    {
        const HeroDefinition& hero = HeroSystem::GetDefinitionByIndex(i);
        const int y = listY + i * rowHeight;
        const bool current = i == heroSelectIndex_;
        const Rectangle rowRect { static_cast<float>(listX), static_cast<float>(y), 286.0f,
                                  static_cast<float>(rowHeight - 8) };
        DrawRectangleRounded(rowRect, 0.22f, 6, current ? kMenuRowSel : kMenuRowIdle);
        if (current)
        {
            DrawRectangleRoundedLines(rowRect, 0.22f, 6, 1.0f, Fade(kAccentGold, 0.70f));
        }
        DrawText(hero.name.c_str(), listX + 14, y + 8, 20, current ? kAccentGold : Fade(kTextBright, 0.82f));
        DrawText(hero.passiveName.c_str(), listX + 14, y + 30, 14, Fade(kTextDim, current ? 0.85f : 0.55f));
    }

    DrawText(selected.name.c_str(), detailX, panelY + 22, 30, kAccentGold);
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
    MenuButton(startButton, "Старт", MenuButtonStyle::Accent, false, true);
    MenuButton(backButton, "Назад", MenuButtonStyle::Ghost, false, true);
}

void Game::RenderSettings() const
{
    constexpr int kSettingsRows = 24;
    constexpr int kVisibleRows = 12;
    const char* labels[kSettingsRows] {
        "Чувствительность мыши", "Чувствительность геймпада", "Мёртвая зона стиков", "Угол обзора",
        "Разрешение", "Режим окна", "VSync", "Ограничение FPS", "Масштаб рендера", "Дальность прорисовки",
        "Качество теней", "Качество эффектов", "Постобработка", "Свечение", "Общая громкость", "Музыка",
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

    DrawCenteredText("Настройки", 60, 40, kTextBright);
    const int panelWidth = 680;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 118;
    const int panelHeight = kVisibleRows * 38 + 24;
    const int firstVisible = std::clamp(settingsIndex_ - kVisibleRows / 2, 0, kSettingsRows - kVisibleRows);
    MenuPanel(Rectangle { static_cast<float>(panelX), static_cast<float>(panelY),
                          static_cast<float>(panelWidth), static_cast<float>(panelHeight) });
    BeginScissorMode(panelX + 12, panelY + 8, panelWidth - 24, panelHeight - 16);
    for (int visible = 0; visible < kVisibleRows; ++visible)
    {
        const int i = firstVisible + visible;
        const int y = panelY + 22 + visible * 38;
        const bool selected = i == settingsIndex_;
        const Rectangle row { static_cast<float>(panelX + 20), static_cast<float>(y - 8),
                              static_cast<float>(panelWidth - 40), 31.0f };
        if (!values[i].empty())
        {
            MenuStepperRow(row, labels[i], values[i], selected, 18);
        }
        else
        {
            MenuLabelValueRow(row, labels[i], "", selected, kAccentGold, 18);
        }
    }
    EndScissorMode();
    const float scrollFraction = static_cast<float>(firstVisible) / static_cast<float>(kSettingsRows - kVisibleRows);
    const int trackHeight = panelHeight - 28;
    const int thumbHeight = std::max(48, trackHeight * kVisibleRows / kSettingsRows);
    DrawRectangle(panelX + panelWidth - 12, panelY + 14, 4, trackHeight, Fade(WHITE, 0.12f));
    DrawRectangle(panelX + panelWidth - 12, panelY + 14 + static_cast<int>((trackHeight - thumbHeight) * scrollFraction), 4, thumbHeight, Fade(kAccentCyan, 0.55f));
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

    DrawCenteredText("Управление", 60, 40, kTextBright);
    DrawCenteredText(waitingForKey_ ? "Нажмите клавишу или кнопку мыши. Esc отменяет." : "Enter/ЛКМ меняет выбранную клавишу или кнопку мыши.", 106, 18, Fade(kTextDim, 0.80f));

    const int panelWidth = 900;
    const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
    const int panelY = 142;
    const int panelHeight = 456;
    MenuPanel(Rectangle { static_cast<float>(panelX), static_cast<float>(panelY),
                          static_cast<float>(panelWidth), static_cast<float>(panelHeight) });

    for (int i = 0; i < kControlRows; ++i)
    {
        const int column = i / kRowsPerColumn;
        const int row = i % kRowsPerColumn;
        const int x = panelX + 20 + column * kColumnWidth;
        const int y = panelY + 18 + row * 35;
        const bool selected = i == controlsIndex_;
        const Color color = selected ? kAccentGold : Fade(kTextBright, 0.80f);
        const Rectangle rowRect { static_cast<float>(x), static_cast<float>(y - 7),
                                  static_cast<float>(kColumnWidth - 28), 28.0f };
        MenuRow(rowRect, selected, kAccentGold);
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
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.55f));
    DrawCenteredText("Пауза", GetScreenHeight() / 2 - 154, 42, kTextBright);

    const Rectangle panel = PausePanelRect();
    MenuPanel(panel);
    for (int i = 0; i < 5; ++i)
    {
        const Rectangle row = PauseRowRect(i);
        const bool selected = i == pauseIndex_;
        MenuRow(row, selected, kAccentGold);
        DrawText(labels[i], static_cast<int>(row.x + 20.0f), static_cast<int>(row.y + 7.0f), 20,
                 selected ? kAccentGold : Fade(kTextBright, 0.80f));
    }
}

void Game::RenderNetworkPauseOverlay() const
{
    const char* labels[] { "Продолжить", "Настройки", "Главное меню" };
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.55f));
    DrawCenteredText("Пауза", GetScreenHeight() / 2 - 154, 42, kTextBright);

    const Rectangle panel = PausePanelRect();
    MenuPanel(panel);
    for (int i = 0; i < 3; ++i)
    {
        const Rectangle row = PauseRowRect(i);
        const bool selected = i == pauseIndex_;
        MenuRow(row, selected, kAccentGold);
        DrawText(labels[i], static_cast<int>(row.x + 20.0f), static_cast<int>(row.y + 7.0f), 20,
                 selected ? kAccentGold : Fade(kTextBright, 0.80f));
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
        + " | ПКМ использовать/ставить"
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
        DrawCenteredText("Ожидание респауна", GetScreenHeight() / 2 + 96, 24, ORANGE);
    }
}

void Game::RenderOnboarding(const Player& localPlayer) const
{
    const Team* team = FindTeam(localPlayer.GetTeamId());
    const HeroDefinition& hero = HeroSystem::GetDefinition(localPlayer.GetHeroId());
    std::string title = tutorialMode_ ? "ОБУЧЕНИЕ" : "ПЕРВЫЙ МАТЧ";
    std::string instruction;
    if (matchSimulation_.MatchTimeSeconds() < 12.0f)
    {
        instruction = "Кор дает респаун. Защищайте его блоками и не падайте в воид.";
    }
    else if (matchSimulation_.MatchTimeSeconds() < 24.0f)
    {
        instruction = "Собирайте железо и золото у генератора. Кристаллы находятся ближе к центру.";
    }
    else if (matchSimulation_.MatchTimeSeconds() < 36.0f)
    {
        instruction = std::string(KeyLabel(input_.GetBindings().interact)) + " открывает магазин у любой базы. Купите блоки или улучшение.";
    }
    else if (matchSimulation_.MatchTimeSeconds() < 48.0f)
    {
        instruction = "ПКМ ставит блок. Удерживайте Shift у края для безопасного моста.";
    }
    else if (matchSimulation_.MatchTimeSeconds() < 60.0f)
    {
        instruction = "Удерживайте ЛКМ для зарядки лука и бластера. ПКМ включает оптику снайперской винтовки; колесо меняет увеличение.";
    }
    else
    {
        instruction = hero.name + ": " + KeyLabel(input_.GetBindings().heroActive1) + "/"
            + KeyLabel(input_.GetBindings().heroActive2) + " активки, "
            + KeyLabel(input_.GetBindings().heroUltimate) + " ульта. Уничтожьте вражеский Кор.";
    }

    if (team != nullptr && !team->coreAlive)
    {
        instruction = "Ваш Кор разрушен: следующая смерть финальная. Играйте осторожно.";
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
    DrawText("Карта", x + 10, y + 8, 16, Fade(WHITE, 0.70f));

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

    for (const EnergyCore& core : matchSimulation_.Cores())
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
    if (matchSimulation_.HasWinner())
    {
        return;
    }

    const float remaining = std::max(0.0f, kCoreCollapseSeconds - matchSimulation_.MatchTimeSeconds());
    const int minutes = static_cast<int>(remaining) / 60;
    const int seconds = static_cast<int>(remaining) % 60;
    std::string text = "Коллапс ядер ";
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
    DrawCenteredText(finalDeath ? "ФИНАЛЬНАЯ СМЕРТЬ" : "ВЫ ПОГИБЛИ", y + 22, 28, finalDeath ? Color { 255, 118, 118, 255 } : ORANGE);
    if (finalDeath)
    {
        DrawCenteredText(("Убийца: " + localDeathKiller_).c_str(), y + 62, 18, Fade(WHITE, 0.82f));
        DrawCenteredText(("Причина: " + localDeathCause_).c_str(), y + 88, 18, Fade(WHITE, 0.72f));
        DrawCenteredText("Вы выбыли. Режим наблюдателя включен.", y + 118, 16, Fade(WHITE, 0.70f));
        DrawCenteredText("Влево/вправо: цель | F: свободная камера | Tab: таблица", y + 146, 14, Fade(WHITE, 0.58f));
    }
    else
    {
        DrawCenteredText(("Убийца: " + localDeathKiller_ + " | " + localDeathCause_).c_str(), y + 62, 17, Fade(WHITE, 0.78f));
        const std::string respawn = "Респаун через "
            + std::to_string(static_cast<int>(std::ceil(localPlayer.GetRespawnTimer()))) + " с";
        DrawCenteredText(respawn.c_str(), y + 100, 20, Fade(WHITE, 0.78f));
    }
}

void Game::RenderSpectatorOverlay() const
{
    const Player* target = GetSpectatorTarget();
    const std::string targetText = spectatorFreeCamera_
        ? "Свободная камера"
        : (target != nullptr ? "Слежение: " + target->GetName() : "Нет живых игроков");
    const int width = 520;
    const int height = 58;
    const int x = GetScreenWidth() / 2 - width / 2;
    const int y = GetScreenHeight() - height - 54;
    DrawRectangle(x, y, width, height, Fade(Color { 7, 9, 14, 255 }, 0.70f));
    DrawRectangleLines(x, y, width, height, Fade(WHITE, 0.22f));
    DrawCenteredText(("НАБЛЮДАТЕЛЬ  |  " + targetText).c_str(), y + 10, 18, WHITE);
    DrawCenteredText("Влево/вправо: цель | F: свободная/слежение | WASD Пробел Ctrl: свободная камера", y + 34, 14, Fade(WHITE, 0.60f));
}

void Game::RenderChestOverlay() const
{
    if (networkMode_ == NetworkMode::LocalClient && personalChestOpen_)
    {
        return;
    }
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


    const int inventorySlotSize = 50;
    const int inventoryGap = 8;
    const int inventoryHotbarWidth = inventorySlotSize * kHotbarSlotCount + inventoryGap * (kHotbarSlotCount - 1);
    const int inventoryPanelWidth = inventoryHotbarWidth + 42;
    const int panelWidth = 250;
    const int panelHeight = 322;
    const int inventoryPanelX = GetScreenWidth() / 2 - inventoryPanelWidth / 2;
    const int preferredX = inventoryPanelX + inventoryPanelWidth + 18;
    const int x = std::min(preferredX, GetScreenWidth() - panelWidth - 16);
    const int y = GetScreenHeight() / 2 - panelHeight / 2;
    const int slotSize = 34;
    const int gap = 5;
    const int cols = 6;
    const int rows = 6;
    const int gridX = x + 14;
    const int gridY = y + 52;
    const Vector2 mouse = GetMousePosition();

    const auto itemColor = [](ItemType type)
    {
        if (const std::optional<ResourceType> resource = ItemToResource(type))
        {
            switch (*resource)
            {
            case ResourceType::Iron:
                return Color { 210, 218, 226, 255 };
            case ResourceType::Gold:
                return Color { 255, 211, 94, 255 };
            case ResourceType::Crystal:
                return Color { 112, 232, 255, 255 };
            }
        }
        if (ItemIsBlock(type))
        {
            return Color { 154, 186, 255, 255 };
        }
        if (ItemIsWeapon(type))
        {
            return Color { 218, 226, 238, 255 };
        }
        if (ItemIsUtility(type))
        {
            return Color { 112, 232, 255, 255 };
        }
        return Color { 210, 180, 96, 255 };
    };

    const auto drawSlot = [slotSize, &itemColor](const ItemStack& stack, int sx, int sy, bool hovered)
    {
        DrawRectangle(sx, sy, slotSize, slotSize, Fade(Color { 24, 28, 36, 255 }, 0.88f));
        DrawRectangleLines(sx, sy, slotSize, slotSize, hovered ? Color { 112, 232, 255, 255 } : Fade(WHITE, 0.18f));
        if (stack.IsEmpty())
        {
            return;
        }

        DrawRectangle(sx + 8, sy + 7, slotSize - 16, slotSize - 17, itemColor(stack.type));
        DrawRectangleLines(sx + 8, sy + 7, slotSize - 16, slotSize - 17, Fade(WHITE, 0.38f));
        DrawText(ItemShortName(stack.type), sx + 4, sy + slotSize - 12, 9, Fade(WHITE, 0.82f));
        if (stack.count > 1)
        {
            const std::string count = std::to_string(stack.count);
            DrawText(count.c_str(), sx + slotSize - MeasureText(count.c_str(), 12) - 3, sy + 3, 12, WHITE);
        }
    };

    DrawRectangle(x, y, panelWidth, panelHeight, Fade(BLACK, 0.76f));
    DrawRectangleLines(x, y, panelWidth, panelHeight, Fade(WHITE, 0.24f));
    DrawText(teamChestOpen_ ? "Team Chest" : "Personal Chest", x + 14, y + 14, 20, WHITE);
    DrawText(teamChestOpen_ ? "LMB: take | Shift+LMB inventory: store" : "Drag stacks between panels", x + 14, y + panelHeight - 28, 11, Fade(WHITE, 0.58f));

    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < cols; ++col)
        {
            const int slot = row * cols + col;
            const int sx = gridX + col * (slotSize + gap);
            const int sy = gridY + row * (slotSize + gap);
            const Rectangle bounds {
                static_cast<float>(sx),
                static_cast<float>(sy),
                static_cast<float>(slotSize),
                static_cast<float>(slotSize)
            };
            drawSlot(chest.GetSlot(slot), sx, sy, CheckCollisionPointRec(mouse, bounds));
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
    DrawText("Ближайший враг", cx - 62, y - 12, 14, Fade(WHITE, 0.66f));
    const std::string meters = std::to_string(static_cast<int>(std::sqrt(bestDistance))) + "м";
    DrawText(meters.c_str(), cx + 64, y - 12, 14, Color { 255, 118, 118, 255 });
}

void Game::RenderBotDebug() const
{
    for (const Player& player : players_)
    {
        if (IsLocallyPredicted(player.GetControlKind()) || !player.IsAlive())
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
            + " / план: " + ToString(memory->currentPlan.goal)
            + (memory->currentPlan.targetTeamId >= 0 ? "->" + std::to_string(memory->currentPlan.targetTeamId) : "")
            + (memory->intentReason.empty() ? "" : " / " + memory->intentReason)
            + (memory->roleReason.empty() ? "" : " / роль: " + memory->roleReason);
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

    const std::string title = std::string("Автоматч ")
        + std::to_string(automatch_.completedRuns)
        + "/"
        + std::to_string(automatch_.targetRuns)
        + (automatch_.active ? " идет" : " завершен");
    DrawText(title.c_str(), x + 14, y + 12, 20, WHITE);

    const float avgDuration = automatch_.completedRuns > 0
        ? automatch_.totalDuration / static_cast<float>(automatch_.completedRuns)
        : matchSimulation_.MatchTimeSeconds();
    const std::string totals = "Победы К/С/З/Ж "
        + std::to_string(automatch_.teamWins[0]) + "/"
        + std::to_string(automatch_.teamWins[1]) + "/"
        + std::to_string(automatch_.teamWins[2]) + "/"
        + std::to_string(automatch_.teamWins[3])
        + " | тайм-ауты " + std::to_string(automatch_.timeouts);
    DrawText(totals.c_str(), x + 14, y + 42, 15, Fade(WHITE, 0.74f));

    const std::string flow = "Убийства " + std::to_string(automatch_.totalKills)
        + " | Урон ядрам " + std::to_string(automatch_.totalCoreDamage)
        + " | Финальные " + std::to_string(automatch_.totalFinalDeaths)
        + " | Ядра " + std::to_string(automatch_.totalCoreDestroyed)
        + " | Среднее " + std::to_string(static_cast<int>(avgDuration)) + "с"
        + " | Скорость " + AutomatchSpeedName();
    DrawText(flow.c_str(), x + 14, y + 64, 15, Fade(WHITE, 0.74f));

    DrawRectangle(x + 12, y + 90, width - 24, 24, Fade(Color { 30, 36, 46, 255 }, 0.72f));
    DrawText("Бот", x + 22, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("Роль", x + 190, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("Цель", x + 272, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("K/D", x + 386, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("Ядро", x + 444, y + 96, 14, Fade(WHITE, 0.70f));
    DrawText("Смена", x + 488, y + 96, 14, Fade(WHITE, 0.70f));

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
        const std::string lastRun = "Последний: "
            + std::string(last.timeout ? "тайм-аут" : TeamName(last.winnerTeamId))
            + " | " + std::to_string(static_cast<int>(last.duration)) + " с"
            + " | убийства " + std::to_string(last.kills)
            + " | Кор " + std::to_string(last.coreDamage);
        DrawText(lastRun.c_str(), x + 14, y + height - 46, 14, Fade(WHITE, 0.62f));
        DrawText(last.finishReason.c_str(), x + 14, y + height - 24, 14, Fade(WHITE, 0.62f));
    }
}

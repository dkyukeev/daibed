#include "Game.h"

#include "HeroSystem.h"
#include "Platform/SteamLobbyService.h"
#include "Platform/SteamRuntime.h"
#include "UiText.h"
#include "VisualTheme.h"
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
    const int x = GetScreenWidth() / 2 - MeasureText(text.c_str(), fontSize) / 2;
    const int off = fontSize >= 30 ? 3 : 2; // pixel drop shadow (MC style)
    DrawText(text.c_str(), x + off, y + off, fontSize, Fade(BLACK, 0.62f));
    DrawText(text.c_str(), x, y, fontSize, color);
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
        result += "КД: " + FormatTenths(ability.cooldownSeconds) + " с";
    }
    if (ability.durationSeconds > 0.0f)
    {
        if (!result.empty())
        {
            result += '\n';
        }
        result += "Длит.: " + FormatTenths(ability.durationSeconds) + " с";
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

std::size_t Utf8Length(const std::string& value)
{
    std::size_t count = 0;
    const char* cursor = value.c_str();
    while (*cursor != '\0')
    {
        int bytes = 0;
        GetCodepointNext(cursor, &bytes);
        cursor += std::max(1, bytes);
        ++count;
    }
    return count;
}

void PopUtf8Codepoint(std::string& value)
{
    if (value.empty())
    {
        return;
    }
    std::size_t start = value.size() - 1;
    while (start > 0
        && (static_cast<unsigned char>(value[start]) & 0xC0u) == 0x80u)
    {
        --start;
    }
    value.resize(start);
}

void EraseFirstUtf8Codepoint(std::string& value)
{
    if (value.empty())
    {
        return;
    }
    int bytes = 0;
    GetCodepointNext(value.c_str(), &bytes);
    value.erase(0, static_cast<std::size_t>(std::max(1, bytes)));
}

bool AppendTextCodepoint(
    std::string& value,
    int codepoint,
    std::size_t maxLength,
    bool allowSpaces,
    bool allowUnicode)
{
    if (codepoint < 32 || codepoint == 127
        || (!allowUnicode && codepoint >= 127)
        || (!allowSpaces && codepoint == ' ')
        || Utf8Length(value) >= maxLength)
    {
        return false;
    }
    int bytes = 0;
    const char* encoded = CodepointToUTF8(codepoint, &bytes);
    if (encoded == nullptr || bytes <= 0)
    {
        return false;
    }
    value.append(encoded, static_cast<std::size_t>(bytes));
    return true;
}

void AppendClipboardText(
    std::string& value,
    std::size_t maxLength,
    bool allowSpaces,
    bool allowUnicode)
{
    const char* clipboard = GetClipboardText();
    if (clipboard == nullptr)
    {
        return;
    }
    for (const char* cursor = clipboard; *cursor != '\0';)
    {
        int bytes = 0;
        const int codepoint = GetCodepointNext(cursor, &bytes);
        AppendTextCodepoint(value, codepoint, maxLength, allowSpaces, allowUnicode);
        cursor += std::max(1, bytes);
    }
}

void EditTextField(
    std::string& value,
    std::size_t maxLength,
    bool allowSpaces,
    bool allowUnicode = false)
{
    const bool ctrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrlDown && IsKeyPressed(KEY_V))
    {
        AppendClipboardText(value, maxLength, allowSpaces, allowUnicode);
    }
    if (IsKeyPressed(KEY_BACKSPACE) && !value.empty())
    {
        PopUtf8Codepoint(value);
    }

    for (int key = GetCharPressed(); key > 0; key = GetCharPressed())
    {
        AppendTextCodepoint(value, key, maxLength, allowSpaces, allowUnicode);
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
        EraseFirstUtf8Codepoint(text);
        const std::string candidate = "..." + text;
        if (MeasureText(candidate.c_str(), fontSize) <= maxWidth)
        {
            return candidate;
        }
    }
    return "...";
}

// ---------------------------------------------------------------------------
// Menu kit: one shared PIXEL theme (Minecraft-style) so every menu screen draws
// from the same vocabulary. Rules of the style: sharp corners only, 2px borders,
// bevelled edges (light top/left, dark bottom/right), drop-shadowed text, and a
// dark translucent panel like modern Minecraft's settings. All sizes chunky.
// ---------------------------------------------------------------------------

constexpr Color kMenuPanel { 17, 18, 22, 240 };
constexpr Color kMenuField { 9, 10, 13, 255 };
constexpr Color kMenuRowIdle { 0, 0, 0, 80 };
constexpr Color kMenuRowSel { 54, 58, 68, 255 };
constexpr Color kAccentGold = VisualTheme::Palette::Objective;
constexpr Color kAccentCyan = VisualTheme::Palette::Energy;
constexpr Color kAccentGreen = VisualTheme::Palette::Healing;
constexpr Color kAccentRed = VisualTheme::Palette::Danger;
constexpr Color kTextBright = VisualTheme::Palette::TextBright;
constexpr Color kTextDim = VisualTheme::Palette::TextDim;
constexpr Color kTextFaint { 120, 130, 145, 255 };
constexpr Color kPixelOutline { 0, 0, 0, 225 };
constexpr Color kButtonTop { 106, 110, 122, 255 };
constexpr Color kButtonBottom { 82, 86, 98, 255 };

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

// 4 side strips — a sharp pixel frame (raylib's DrawRectangleLines is 1px only).
void PixelBorder(Rectangle rect, int thickness, Color color)
{
    const int x = static_cast<int>(rect.x);
    const int y = static_cast<int>(rect.y);
    const int w = static_cast<int>(rect.width);
    const int h = static_cast<int>(rect.height);
    DrawRectangle(x, y, w, thickness, color);
    DrawRectangle(x, y + h - thickness, w, thickness, color);
    DrawRectangle(x, y + thickness, thickness, h - thickness * 2, color);
    DrawRectangle(x + w - thickness, y + thickness, thickness, h - thickness * 2, color);
}

// Classic bevel: raised = light top/left + dark bottom/right; sunken inverts.
void PixelBevel(Rectangle rect, int thickness, bool sunken, float strength = 1.0f)
{
    const Color light = Fade(WHITE, 0.30f * strength);
    const Color dark = Fade(BLACK, 0.45f * strength);
    const int x = static_cast<int>(rect.x);
    const int y = static_cast<int>(rect.y);
    const int w = static_cast<int>(rect.width);
    const int h = static_cast<int>(rect.height);
    DrawRectangle(x, y, w, thickness, sunken ? dark : light);
    DrawRectangle(x, y + thickness, thickness, h - thickness, sunken ? dark : light);
    DrawRectangle(x + thickness, y + h - thickness, w - thickness, thickness, sunken ? light : dark);
    DrawRectangle(x + w - thickness, y + thickness, thickness, h - thickness * 2, sunken ? light : dark);
}

// Minecraft-style drop shadow: a dark copy one "design pixel" down-right.
void DrawTextShadow(const char* text, int x, int y, int fontSize, Color color)
{
    const int off = fontSize >= 30 ? 3 : 2;
    DrawText(text, x + off, y + off, fontSize, Fade(BLACK, 0.62f));
    DrawText(text, x, y, fontSize, color);
}

void MenuPanel(Rectangle rect)
{
    DrawRectangleRec(rect, kMenuPanel);
    PixelBorder(rect, 2, kPixelOutline);
    // A faint inner top highlight sells the "plate" without rounding anything.
    DrawRectangle(static_cast<int>(rect.x) + 2, static_cast<int>(rect.y) + 2,
                  static_cast<int>(rect.width) - 4, 2, Fade(WHITE, 0.07f));
}

void MenuRow(Rectangle rect, bool selected, Color accent)
{
    if (selected)
    {
        DrawRectangleRec(rect, kMenuRowSel);
        PixelBorder(rect, 2, Fade(WHITE, 0.85f)); // MC hover = white frame
        DrawRectangle(static_cast<int>(rect.x) + 2, static_cast<int>(rect.y) + 2,
                      4, static_cast<int>(rect.height) - 4, accent); // accent notch
    }
    else
    {
        DrawRectangleRec(rect, kMenuRowIdle);
    }
}

// Label on the left, optional value right-aligned. Used by main menu / settings
// / controls list rows.
void MenuLabelValueRow(Rectangle rect, const char* label, const std::string& value,
                       bool selected, Color accent, int fontSize = 20)
{
    MenuRow(rect, selected, accent);
    const Color color = selected ? accent : Fade(kTextBright, 0.86f);
    const int ty = CenteredTextY(rect, fontSize);
    DrawTextShadow(label, static_cast<int>(rect.x) + 24, ty, fontSize, color);
    if (!value.empty())
    {
        const int vw = MeasureText(value.c_str(), fontSize);
        DrawTextShadow(value.c_str(), static_cast<int>(rect.x + rect.width) - 22 - vw, ty, fontSize, color);
    }
}

struct MenuStepperGeometry
{
    Rectangle leftArrow {};
    Rectangle value {};
    Rectangle rightArrow {};
};

MenuStepperGeometry BuildMenuStepperGeometry(Rectangle rect)
{
    const float controlWidth = std::min(230.0f, rect.width * 0.46f);
    const float arrowWidth = 34.0f;
    const float inset = 5.0f;
    const float x = rect.x + rect.width - controlWidth - 12.0f;
    return MenuStepperGeometry {
        Rectangle { x, rect.y + inset, arrowWidth, rect.height - inset * 2.0f },
        Rectangle { x + arrowWidth, rect.y + inset,
                    controlWidth - arrowWidth * 2.0f, rect.height - inset * 2.0f },
        Rectangle { x + controlWidth - arrowWidth, rect.y + inset,
                    arrowWidth, rect.height - inset * 2.0f }
    };
}

int MenuStepperClickDelta(Rectangle rect, Vector2 mouse)
{
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return 0;
    }
    const MenuStepperGeometry geometry = BuildMenuStepperGeometry(rect);
    if (CheckCollisionPointRec(mouse, geometry.leftArrow))
    {
        return -1;
    }
    if (CheckCollisionPointRec(mouse, geometry.rightArrow))
    {
        return 1;
    }
    return 0;
}

int MenuCellStepperClickDelta(Rectangle rect, Vector2 mouse)
{
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        return 0;
    }
    const Rectangle left { rect.x, rect.y, std::min(36.0f, rect.width * 0.25f), rect.height };
    const Rectangle right { rect.x + rect.width - left.width, rect.y, left.width, rect.height };
    if (CheckCollisionPointRec(mouse, left)) return -1;
    if (CheckCollisionPointRec(mouse, right)) return 1;
    return 0;
}

// Label on the left, "< value >" stepper group right-aligned.
void MenuStepperRow(Rectangle rect, const char* label, const std::string& value,
                    bool selected, int fontSize = 20)
{
    MenuRow(rect, selected, kAccentCyan);
    const Color labelColor = selected ? kAccentGold : Fade(kTextBright, 0.86f);
    const Color chevron = selected ? kAccentCyan : Fade(kAccentCyan, 0.45f);
    const int ty = CenteredTextY(rect, fontSize);
    DrawTextShadow(label, static_cast<int>(rect.x) + 24, ty, fontSize, labelColor);

    const MenuStepperGeometry geometry = BuildMenuStepperGeometry(rect);
    const int leftWidth = MeasureText("<", fontSize);
    const int rightWidth = MeasureText(">", fontSize);
    const int valueWidth = MeasureText(value.c_str(), fontSize);
    DrawTextShadow("<", static_cast<int>(geometry.leftArrow.x + (geometry.leftArrow.width - leftWidth) * 0.5f),
                   ty, fontSize, chevron);
    DrawTextShadow(value.c_str(), static_cast<int>(geometry.value.x + (geometry.value.width - valueWidth) * 0.5f),
                   ty, fontSize, labelColor);
    DrawTextShadow(">", static_cast<int>(geometry.rightArrow.x + (geometry.rightArrow.width - rightWidth) * 0.5f),
                   ty, fontSize, chevron);
}

// Label on the left, a Minecraft-style checkbox on the right (kept the old
// "pill" name — every toggle row calls this).
void MenuTogglePill(Rectangle rect, const char* label, bool on, bool selected, int fontSize = 18)
{
    MenuRow(rect, selected, on ? kAccentGreen : kTextFaint);
    DrawTextShadow(label, static_cast<int>(rect.x) + 24, CenteredTextY(rect, fontSize), fontSize,
                   selected ? kAccentGold : Fade(kTextBright, 0.86f));

    const float box = 22.0f;
    const Rectangle check {
        rect.x + rect.width - 20.0f - box,
        rect.y + (rect.height - box) * 0.5f,
        box,
        box
    };
    DrawRectangleRec(check, kMenuField);
    PixelBevel(check, 2, true);
    PixelBorder(check, 2, on ? Fade(kAccentGreen, 0.9f) : Fade(WHITE, selected ? 0.55f : 0.28f));
    if (on)
    {
        DrawRectangle(static_cast<int>(check.x) + 6, static_cast<int>(check.y) + 6,
                      static_cast<int>(box) - 12, static_cast<int>(box) - 12, kAccentGreen);
    }
}

// Horizontal segmented control (tabs, public/private). Returns nothing; the
// caller owns hit-testing and which segment is active.
void MenuSegmented(Rectangle rect, const char* const* labels, int count, int active,
                   bool selected, Color accent, int fontSize = 16)
{
    DrawRectangleRec(rect, kMenuField);
    PixelBorder(rect, 2, selected ? Fade(accent, 0.7f) : kPixelOutline);
    const float pad = 4.0f;
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
            DrawRectangleRec(seg, Fade(accent, 0.22f));
            PixelBorder(seg, 2, Fade(accent, 0.85f));
        }
        const Color tc = isActive ? accent : Fade(kTextDim, 0.85f);
        const int tw = MeasureText(labels[i], fontSize);
        DrawTextShadow(labels[i], static_cast<int>(seg.x + (seg.width - tw) * 0.5f),
                       CenteredTextY(seg, fontSize), fontSize, tc);
    }
}

// Boxed text field (caller draws its own label). Shows a blink caret when
// focused, a placeholder when empty/unfocused.
void MenuTextField(Rectangle rect, const std::string& value, bool masked, bool focused,
                   bool caretOn, const char* placeholder = "", int fontSize = 16)
{
    DrawRectangleRec(rect, kMenuField);
    PixelBevel(rect, 2, true);
    PixelBorder(rect, 2, focused ? Fade(kAccentGold, 0.80f) : kPixelOutline);

    std::string shown = masked ? std::string(value.size(), '*') : value;
    const bool empty = shown.empty();
    if (empty && !focused)
    {
        shown = placeholder;
    }
    if (focused && caretOn)
    {
        shown.push_back('_');
    }
    const std::string clipped = ClipTextToWidth(shown, static_cast<int>(rect.width) - 22, fontSize);
    const Color tc = (empty && !focused) ? Fade(kTextFaint, 0.85f) : kTextBright;
    DrawTextShadow(clipped.c_str(), static_cast<int>(rect.x) + 12, CenteredTextY(rect, fontSize), fontSize, tc);
}

Color MixColor(Color a, Color b, float t)
{
    const auto mix = [t](unsigned char x, unsigned char y)
    {
        return static_cast<unsigned char>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t);
    };
    return Color { mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), a.a };
}

// Classic Minecraft beveled button: stone-grey two-tone fill tinted toward the
// style accent, black frame (white when selected/hovered), light/dark bevel.
void MenuButton(Rectangle rect, const char* label, MenuButtonStyle style, bool selected,
                bool enabled, int fontSize = 18)
{
    const Color accent = MenuButtonAccent(style);
    const float tint = style == MenuButtonStyle::Ghost ? 0.0f : 0.14f;
    Color top = MixColor(kButtonTop, accent, tint);
    Color bottom = MixColor(kButtonBottom, accent, tint);
    if (!enabled)
    {
        top = Color { 44, 46, 52, 255 };
        bottom = Color { 38, 40, 46, 255 };
    }
    else if (selected)
    {
        top = MixColor(top, WHITE, 0.10f);
        bottom = MixColor(bottom, WHITE, 0.08f);
    }
    const int halfH = static_cast<int>(rect.height) / 2;
    DrawRectangle(static_cast<int>(rect.x), static_cast<int>(rect.y),
                  static_cast<int>(rect.width), halfH, top);
    DrawRectangle(static_cast<int>(rect.x), static_cast<int>(rect.y) + halfH,
                  static_cast<int>(rect.width), static_cast<int>(rect.height) - halfH, bottom);
    PixelBevel(rect, 2, false, enabled ? 1.0f : 0.4f);
    PixelBorder(rect, 2, selected && enabled ? Color { 245, 245, 245, 255 } : kPixelOutline);

    const Color tc = !enabled ? Fade(kTextDim, 0.5f)
        : (style == MenuButtonStyle::Ghost ? Fade(kTextBright, selected ? 1.0f : 0.8f)
                                           : (selected ? WHITE : Fade(kTextBright, 0.95f)));
    const Color labelColor = enabled && style != MenuButtonStyle::Ghost && selected
        ? MixColor(tc, accent, 0.35f) : tc;
    const int tw = MeasureText(label, fontSize);
    DrawTextShadow(label, static_cast<int>(rect.x + (rect.width - tw) * 0.5f),
                   CenteredTextY(rect, fontSize), fontSize, labelColor);
}

// ---------------------------------------------------------------------------
// Settings screen geometry — one source shared by RenderSettings and
// HandleSettingsInput so mouse hit-testing can never drift from the pixels.
// Layout follows modern Minecraft settings: category sidebar on the left,
// scrollable option rows on the right.
// ---------------------------------------------------------------------------
constexpr int kSettingsVisibleShared = 12;
constexpr int kSettingsSidebarW = 204;

struct SettingsSection
{
    const char* name;
    int firstRow;
};
constexpr SettingsSection kSettingsSections[] {
    { "Ввод", 0 },
    { "Экран", 4 },
    { "Графика", 9 },
    { "Звук", 16 },
    { "Интерфейс", 20 },
    { "Шейдеры", 23 },
};
constexpr int kSettingsSectionCount = static_cast<int>(std::size(kSettingsSections));

Rectangle SettingsPanelRect()
{
    constexpr int width = 952;
    constexpr int height = kSettingsVisibleShared * 38 + 28;
    return Rectangle {
        static_cast<float>(GetScreenWidth() / 2 - width / 2), 118.0f,
        static_cast<float>(width), static_cast<float>(height)
    };
}

Rectangle SettingsRowRect(int visibleIndex)
{
    const Rectangle panel = SettingsPanelRect();
    const float x = panel.x + 14.0f + static_cast<float>(kSettingsSidebarW) + 18.0f;
    return Rectangle {
        x,
        panel.y + 14.0f + static_cast<float>(visibleIndex) * 38.0f,
        panel.x + panel.width - 32.0f - x,
        31.0f
    };
}

Rectangle SettingsSidebarRect(int sectionIndex)
{
    const Rectangle panel = SettingsPanelRect();
    return Rectangle {
        panel.x + 14.0f,
        panel.y + 14.0f + static_cast<float>(sectionIndex) * 42.0f,
        static_cast<float>(kSettingsSidebarW),
        34.0f
    };
}

int SettingsSectionForRow(int row)
{
    int active = 0;
    for (int i = 0; i < kSettingsSectionCount; ++i)
    {
        if (kSettingsSections[i].firstRow <= row)
        {
            active = i;
        }
    }
    return active;
}

constexpr int kHeroMatchSettingCount = 8;

struct HeroSelectLayout
{
    Rectangle panel {};
    Rectangle heroPane {};
    Rectangle stagePane {};
    Rectangle abilityPane {};
    Rectangle matchPane {};
    Rectangle preview {};
    Rectangle startButton {};
    Rectangle backButton {};
};

HeroSelectLayout BuildHeroSelectLayout()
{
    const float width = static_cast<float>(GetScreenWidth());
    const float height = static_cast<float>(GetScreenHeight());
    const float bottomHeight = 112.0f;
    const Rectangle matchPane { 24.0f, height - bottomHeight - 10.0f, width - 48.0f, bottomHeight };
    const Rectangle heroPane { 32.0f, 78.0f, 326.0f, matchPane.y - 92.0f };
    const Rectangle abilityPane { width - 348.0f, 78.0f, 316.0f, matchPane.y - 92.0f };
    const Rectangle stagePane {
        heroPane.x + heroPane.width + 12.0f,
        58.0f,
        abilityPane.x - (heroPane.x + heroPane.width) - 24.0f,
        matchPane.y - 60.0f
    };
    return HeroSelectLayout {
        Rectangle { 0.0f, 0.0f, width, height },
        heroPane,
        stagePane,
        abilityPane,
        matchPane,
        Rectangle { stagePane.x, stagePane.y + 18.0f, stagePane.width, stagePane.height - 18.0f },
        Rectangle { matchPane.x + matchPane.width - 292.0f, matchPane.y + 48.0f, 184.0f, 42.0f },
        Rectangle { matchPane.x + matchPane.width - 98.0f, matchPane.y + 48.0f, 84.0f, 42.0f }
    };
}

Rectangle HeroSelectHeroRow(const HeroSelectLayout& layout, int index)
{
    const float horizontalGap = 8.0f;
    const float rowWidth = (layout.heroPane.width - horizontalGap * 2.0f) / 3.0f;
    // Preserve room for the selected hero's name, role and passive on short
    // displays instead of letting the second row push that text below screen.
    const float rowHeight = std::clamp((layout.heroPane.height - 194.0f) * 0.5f, 78.0f, 108.0f);
    return Rectangle {
        layout.heroPane.x + static_cast<float>(index % 3) * (rowWidth + horizontalGap),
        layout.heroPane.y + 42.0f + static_cast<float>(index / 3) * (rowHeight + 8.0f),
        rowWidth,
        rowHeight
    };
}

float HeroSelectDetailY(const HeroSelectLayout& layout)
{
    const Rectangle lastRow = HeroSelectHeroRow(layout, HeroSystem::kHeroCount - 1);
    return lastRow.y + lastRow.height + 10.0f;
}

Rectangle HeroSelectMatchRow(const HeroSelectLayout& layout, int index)
{
    const float available = layout.matchPane.width - 326.0f;
    const float gap = 6.0f;
    const float cellWidth = (available - gap * static_cast<float>(kHeroMatchSettingCount - 1))
        / static_cast<float>(kHeroMatchSettingCount);
    return Rectangle {
        layout.matchPane.x + 12.0f + static_cast<float>(index) * (cellWidth + gap),
        layout.matchPane.y + 44.0f,
        cellWidth,
        48.0f
    };
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

Rectangle PausePanelRect(int rows = 5)
{
    constexpr int panelWidth = 360;
    const int panelHeight = 29 + rows * 39;
    return Rectangle {
        static_cast<float>(GetScreenWidth() / 2 - panelWidth / 2),
        static_cast<float>(GetScreenHeight() / 2 - 92),
        static_cast<float>(panelWidth),
        static_cast<float>(panelHeight)
    };
}

Rectangle PauseRowRect(int row, int rows = 5)
{
    const Rectangle panel = PausePanelRect(rows);
    return Rectangle {
        panel.x + 18.0f,
        panel.y + 24.0f + static_cast<float>(row) * 39.0f - 7.0f,
        panel.width - 36.0f,
        29.0f
    };
}

constexpr int kCreativeMapBrowserVisibleRows = 8;

int CreativeMapBrowserFirstRow(int itemCount, int selected)
{
    const int visible = std::min(kCreativeMapBrowserVisibleRows, itemCount);
    return std::clamp(selected - visible / 2, 0, std::max(0, itemCount - visible));
}

Rectangle CreativeMapBrowserPanelRect(int rows)
{
    constexpr int panelWidth = 620;
    const int panelHeight = 54 + rows * 39 + 38;
    return Rectangle {
        static_cast<float>(GetScreenWidth() / 2 - panelWidth / 2),
        static_cast<float>(GetScreenHeight() / 2 - panelHeight / 2),
        static_cast<float>(panelWidth),
        static_cast<float>(panelHeight)
    };
}

Rectangle CreativeMapBrowserRowRect(int row, int rows)
{
    const Rectangle panel = CreativeMapBrowserPanelRect(rows);
    return Rectangle { panel.x + 18.0f, panel.y + 48.0f + static_cast<float>(row) * 39.0f,
                       panel.width - 36.0f, 29.0f };
}

// The pause menu differs by session kind: a stock match, the creative editor
// (map test/save/load) and a creative map test (back to the editor). One entry
// list drives both input handling and rendering so they can't drift apart.
enum class PauseEntryAction
{
    Resume,
    RestartMatch,
    TestMap,
    RestartTest,
    SaveMap,
    LoadMap,
    BackToEditor,
    OpenSettings,
    MainMenu,
    ExitGame
};

struct PauseEntry
{
    const char* label;
    PauseEntryAction action;
};

std::vector<PauseEntry> BuildPauseEntries(bool creativeEditor, bool creativeTest)
{
    if (creativeEditor)
    {
        return {
            { "Продолжить", PauseEntryAction::Resume },
            { "Тест карты", PauseEntryAction::TestMap },
            { "Сохранить карту", PauseEntryAction::SaveMap },
            { "Загрузить карту", PauseEntryAction::LoadMap },
            { "Настройки", PauseEntryAction::OpenSettings },
            { "Главное меню", PauseEntryAction::MainMenu },
            { "Выйти из игры", PauseEntryAction::ExitGame }
        };
    }
    if (creativeTest)
    {
        return {
            { "Продолжить", PauseEntryAction::Resume },
            { "Вернуться в редактор", PauseEntryAction::BackToEditor },
            { "Перезапустить тест", PauseEntryAction::RestartTest },
            { "Настройки", PauseEntryAction::OpenSettings },
            { "Главное меню", PauseEntryAction::MainMenu },
            { "Выйти из игры", PauseEntryAction::ExitGame }
        };
    }
    return {
        { "Продолжить", PauseEntryAction::Resume },
        { "Перезапустить матч", PauseEntryAction::RestartMatch },
        { "Настройки", PauseEntryAction::OpenSettings },
        { "Главное меню", PauseEntryAction::MainMenu },
        { "Выйти из игры", PauseEntryAction::ExitGame }
    };
}
}
void Game::HandleMenuInput()
{
#if DAIBED_DEVELOPER_BUILD
    constexpr int kMenuRows = 6;
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

    bool activate = IsKeyPressed(KEY_ENTER);
    if (hoveredRow >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        menuIndex_ = hoveredRow;
        activate = true;
    }

    if (activate)
    {
#if DAIBED_DEVELOPER_BUILD
        if (menuIndex_ == 0)
        {
            heroSelectIndex_ = HeroSystem::IndexOf(selectedHeroId_);
            heroSelectControlIndex_ = heroSelectIndex_;
            screen_ = GameScreen::HeroSelect;
        }
        else if (menuIndex_ == 1)
        {
            multiplayerIndex_ = 0;
            screen_ = GameScreen::Multiplayer;
        }
        else if (menuIndex_ == 2)
        {
            StartCreativeSession();
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
            heroSelectControlIndex_ = heroSelectIndex_;
            screen_ = GameScreen::HeroSelect;
        }
        else if (menuIndex_ == 2)
        {
            StartCreativeSession();
        }
        else if (menuIndex_ == 3)
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

std::string JoinCommandFor(const std::string& address, const std::string& password,
                           NetworkBackend backend)
{
    std::string command = "DaiBed.exe --connect " + address;
    if (backend == NetworkBackend::SteamP2P)
    {
        command += " --network-backend steam";
    }
    if (!password.empty())
    {
        command += " --password " + password;
    }
    return command;
}
}

void Game::StopLocalServer()
{
    const bool hadProcess = localServerProcess_.Valid();
    const bool hadIntegratedServer = IntegratedListenServerRunning();
    if (!hadProcess && !hadIntegratedServer)
    {
        return;
    }
    if (hadProcess)
    {
        StopServerProcess(localServerProcess_);
    }
    if (hadIntegratedServer)
    {
        StopIntegratedListenServer();
    }
    if (steamLobbyService_ != nullptr && steamLobbyService_->OwnsLobby())
    {
        steamLobbyService_->LeaveLobby();
    }
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
    if (localServerProcess_.Valid() || IntegratedListenServerRunning())
    {
        StopLocalServer();
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
    multiplayerPassword_ = config.password;

    if (config.networkBackend == NetworkBackend::SteamP2P)
    {
        if (config.port >= 1000)
        {
            multiplayerStatus_ = "Steam P2P использует виртуальный порт 1..999.";
            return;
        }

        std::string startError;
        if (!StartIntegratedListenServer(config, startError))
        {
            multiplayerStatus_ = "Не удалось запустить Steam-хост: " + startError;
            return;
        }

        // The server transport already holds a process-wide runtime lease;
        // this short lease safely reads the same authenticated local identity.
        SteamRuntimeLease identityLease;
        if (!identityLease.Acquire() || identityLease.LocalSteamId() == 0)
        {
            multiplayerStatus_ = "Не удалось получить Steam ID: " + identityLease.LastError();
            StopIntegratedListenServer();
            return;
        }

        localServerStartTime_ = GetTime();
        localServerAddress_ = std::to_string(identityLease.LocalSteamId()) + ":"
            + std::to_string(config.port);
        multiplayerAddress_ = localServerAddress_;

        std::string lobbyError;
        // A background startup attempt may have observed Steam while it was
        // still reconnecting.  Hosting is an explicit retry point and the
        // server transport now holds a valid Steam runtime lease.
        steamLobbyStartAttempted_ = false;
        steamLobbyRetryAfter_ = 0.0;
        if (!EnsureSteamLobbyService(lobbyError))
        {
            multiplayerStatus_ = "Steam Lobby недоступен: " + lobbyError;
            StopIntegratedListenServer();
            return;
        }
        SteamLobbyHostSettings lobbySettings;
        lobbySettings.serverName = config.serverName;
        lobbySettings.hostSteamId = identityLease.LocalSteamId();
        lobbySettings.virtualPort = config.port;
        lobbySettings.maxPlayers = config.maxPlayers;
        lobbySettings.mode = config.matchMode;
        lobbySettings.biome = config.worldBiome;
        lobbySettings.privateLobby = config.privateServer;
        lobbySettings.passwordProtected = config.HasPassword();
        if (!steamLobbyService_->CreateLobby(lobbySettings))
        {
            multiplayerStatus_ = "Не удалось создать Steam Lobby: "
                + steamLobbyService_->LastError();
            StopIntegratedListenServer();
            return;
        }

        multiplayerStatus_ = "Steam-хост запущен. Создаётся лобби для приглашений...";
        StartGuiConnect();
        return;
    }

    std::vector<std::string> args {
        "--host",
        "--network-backend", config.networkBackend == NetworkBackend::SteamP2P ? "steam" : "udp",
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
    if (steamFriendPickerOpen_)
    {
        HandleSteamFriendPickerInput();
        return;
    }
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
    if (leftClick)
    {
        multiplayerIndex_ = hovered;
    }
    const int idx = multiplayerIndex_;
    const bool enter = IsKeyPressed(KEY_ENTER);

    // --- Per-tab text-field editing for the focused control. ----------------
    if (multiplayerTab_ == 0)
    {
        if (idx == 0) EditTextField(multiplayerAddress_, 64, false);
        else if (idx == 1) EditTextField(multiplayerPlayerName_, 24, true, true);
        else if (idx == 2) EditTextField(multiplayerPassword_, 32, true);
    }
    else
    {
        if (idx == 0)
        {
            EditTextField(serverConfig_.serverName, 28, true, true);
        }
        else if (idx == 2)
        {
            EditTextField(hostPortText_, 5, false);
            std::string digits;
            for (char c : hostPortText_)
            {
                if (c >= '0' && c <= '9') digits.push_back(c);
            }
            hostPortText_ = digits;
        }
        else if (idx == 4)
        {
            EditTextField(serverConfig_.password, 32, true);
        }
    }

    // Value controls change only through keyboard direction keys or their
    // visible arrow hit-zones. Clicking the value itself merely focuses it.
    int change = (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) ? 1
               : ((IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) ? -1 : 0);
    if (leftClick && (idx == 1 || idx == 3 || idx == 5 || idx == 7))
    {
        change = MenuCellStepperClickDelta(rects[static_cast<std::size_t>(idx)], mouse);
    }

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
        if (change || leftClick || enter) serverConfig_.requireAllReady = !serverConfig_.requireAllReady;
        break;
    case 9:
        if (change || leftClick || enter) serverConfig_.enforceUniqueHeroesPerTeam = !serverConfig_.enforceUniqueHeroesPerTeam;
        break;
    case 10:
        if (enter || leftClick) StartGuiHostAndConnect();
        break;
    case 11:
        if ((enter || leftClick)
            && (IsServerProcessRunning(localServerProcess_) || IntegratedListenServerRunning()))
        {
            StopLocalServer();
        }
        break;
    case 12:
        if (enter || leftClick)
        {
            if (serverConfig_.networkBackend == NetworkBackend::SteamP2P)
            {
                OpenSteamInviteDialog();
                break;
            }
            const std::string address = localServerAddress_.empty()
                ? ("127.0.0.1:" + hostPortText_)
                : localServerAddress_;
            SetClipboardText(JoinCommandFor(address, serverConfig_.password,
                                            serverConfig_.networkBackend).c_str());
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
    constexpr int startControl = heroCount + kHeroMatchSettingCount;
    constexpr int backControl = startControl + 1;
    constexpr int controlCount = backControl + 1;
    heroSelectControlIndex_ = std::clamp(heroSelectControlIndex_, 0, controlCount - 1);

    const HeroSelectLayout layout = BuildHeroSelectLayout();
    const Vector2 mouse = GetMousePosition();

    const auto applyMatchChange = [this](int setting, int delta)
    {
        if (delta == 0) return;
        switch (setting)
        {
        case 0:
            selectedMode_ = static_cast<MatchMode>((static_cast<int>(selectedMode_) + delta + 4) % 4);
            selectedTeamId_ = std::clamp(selectedTeamId_, 0, TeamCountForMode() - 1);
            selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
            break;
        case 1:
        {
            const int teamCount = TeamCountForMode();
            selectedTeamId_ = (selectedTeamId_ + delta + teamCount) % teamCount;
            break;
        }
        case 2:
            selectedTeamSize_ = ((selectedTeamSize_ - 1 + delta + 4) % 4) + 1;
            selectedBotCount_ = std::clamp(selectedBotCount_, 0, MaxBotCountForSelection());
            break;
        case 3:
        {
            const int maxBots = MaxBotCountForSelection();
            selectedBotCount_ = (selectedBotCount_ + delta + maxBots + 1) % (maxBots + 1);
            break;
        }
        case 4:
            botDifficulty_ = static_cast<BotDifficulty>((static_cast<int>(botDifficulty_) + delta + 3) % 3);
            break;
        case 5:
            botStrategyProfile_ = static_cast<BotStrategyProfile>((static_cast<int>(botStrategyProfile_) + delta + 2) % 2);
            break;
        case 6:
            arenaLayout_ = static_cast<ArenaLayout>((static_cast<int>(arenaLayout_) + delta + 2) % 2);
            break;
        case 7:
            arenaBiome_ = static_cast<ArenaBiome>((static_cast<int>(arenaBiome_) + delta + 5) % 5);
            break;
        default:
            return;
        }
        SaveSettings();
    };

    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        heroSelectControlIndex_ = (heroSelectControlIndex_ + 1) % controlCount;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        heroSelectControlIndex_ = (heroSelectControlIndex_ + controlCount - 1) % controlCount;
    }
    if (heroSelectControlIndex_ < heroCount)
    {
        heroSelectIndex_ = heroSelectControlIndex_;
    }

    const int keyboardDelta = (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) ? 1
        : ((IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) ? -1 : 0);
    if (heroSelectControlIndex_ >= heroCount && heroSelectControlIndex_ < startControl)
    {
        applyMatchChange(heroSelectControlIndex_ - heroCount, keyboardDelta);
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(mouse, layout.preview))
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
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        for (int i = 0; i < heroCount; ++i)
        {
            if (CheckCollisionPointRec(mouse, HeroSelectHeroRow(layout, i)))
            {
                heroSelectIndex_ = i;
                heroSelectControlIndex_ = i;
                break;
            }
        }

        for (int i = 0; i < kHeroMatchSettingCount; ++i)
        {
            const Rectangle row = HeroSelectMatchRow(layout, i);
            if (CheckCollisionPointRec(mouse, row))
            {
                heroSelectControlIndex_ = heroCount + i;
                applyMatchChange(i, MenuCellStepperClickDelta(row, mouse));
                break;
            }
        }

        if (CheckCollisionPointRec(mouse, layout.startButton))
        {
            heroSelectControlIndex_ = startControl;
            selectedHeroId_ = HeroSystem::IdFromIndex(heroSelectIndex_);
            SaveSettings();
            StartSelectedMatch();
            return;
        }
        if (CheckCollisionPointRec(mouse, layout.backButton))
        {
            heroSelectControlIndex_ = backControl;
            screen_ = GameScreen::MainMenu;
            return;
        }
    }

    if (IsKeyPressed(KEY_ENTER))
    {
        if (heroSelectControlIndex_ == backControl)
        {
            screen_ = GameScreen::MainMenu;
        }
        else if (heroSelectControlIndex_ == startControl || heroSelectControlIndex_ < heroCount)
        {
            selectedHeroId_ = HeroSystem::IdFromIndex(heroSelectIndex_);
            SaveSettings();
            StartSelectedMatch();
        }
    }
    if (IsKeyPressed(KEY_ESCAPE))
    {
        screen_ = GameScreen::MainMenu;
    }
}

void Game::HandleSettingsInput()
{
    constexpr int kSettingsRows = 38;
    constexpr int kVisibleRows = 12;
    constexpr int kMaxFirstVisible = kSettingsRows - kVisibleRows;
    settingsIndex_ = std::clamp(settingsIndex_, 0, kSettingsRows - 1);
    auto clampSettingsScroll = [&]()
    {
        settingsFirstVisible_ = std::clamp(settingsFirstVisible_, 0, kMaxFirstVisible);
    };
    auto keepSelectionVisible = [&]()
    {
        clampSettingsScroll();
        if (settingsIndex_ < settingsFirstVisible_)
        {
            settingsFirstVisible_ = settingsIndex_;
        }
        else if (settingsIndex_ >= settingsFirstVisible_ + kVisibleRows)
        {
            settingsFirstVisible_ = settingsIndex_ - kVisibleRows + 1;
        }
        clampSettingsScroll();
    };
    clampSettingsScroll();

    const bool pad = IsGamepadAvailable(0);
    const bool down = IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)
        || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN));
    const bool up = IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)
        || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_UP));
    if (down)
    {
        settingsIndex_ = (settingsIndex_ + 1) % kSettingsRows;
        keepSelectionVisible();
    }
    if (up)
    {
        settingsIndex_ = (settingsIndex_ + kSettingsRows - 1) % kSettingsRows;
        keepSelectionVisible();
    }

    const int firstVisible = settingsFirstVisible_;
    const Vector2 mouse = GetMousePosition();
    const Vector2 mouseMove = GetMouseDelta();
    int hoveredRow = -1;
    for (int visible = 0; visible < kVisibleRows; ++visible)
    {
        const int index = firstVisible + visible;
        if (CheckCollisionPointRec(mouse, SettingsRowRect(visible)))
        {
            hoveredRow = index;
            break;
        }
    }
    if (hoveredRow >= 0 && (std::fabs(mouseMove.x) > 0.5f || std::fabs(mouseMove.y) > 0.5f))
    {
        settingsIndex_ = hoveredRow;
    }
    // Sidebar: clicking a category jumps the list to its first row.
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        for (int i = 0; i < kSettingsSectionCount; ++i)
        {
            if (CheckCollisionPointRec(mouse, SettingsSidebarRect(i)))
            {
                settingsIndex_ = kSettingsSections[i].firstRow;
                settingsFirstVisible_ = std::clamp(settingsIndex_, 0, kMaxFirstVisible);
                return;
            }
        }
    }
    const float wheel = GetMouseWheelMove();
    if (wheel < -0.01f)
    {
        settingsFirstVisible_ = std::min(settingsFirstVisible_ + 1, kMaxFirstVisible);
        if (settingsIndex_ < settingsFirstVisible_)
        {
            settingsIndex_ = settingsFirstVisible_;
        }
        else if (settingsIndex_ >= settingsFirstVisible_ + kVisibleRows)
        {
            settingsIndex_ = settingsFirstVisible_ + kVisibleRows - 1;
        }
    }
    else if (wheel > 0.01f)
    {
        settingsFirstVisible_ = std::max(settingsFirstVisible_ - 1, 0);
        if (settingsIndex_ < settingsFirstVisible_)
        {
            settingsIndex_ = settingsFirstVisible_;
        }
        else if (settingsIndex_ >= settingsFirstVisible_ + kVisibleRows)
        {
            settingsIndex_ = settingsFirstVisible_ + kVisibleRows - 1;
        }
    }

    int delta = (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)
        || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))) ? 1
        : ((IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)
            || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT))) ? -1 : 0);
    bool activate = IsKeyPressed(KEY_ENTER)
        || (pad && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
    if (hoveredRow >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        settingsIndex_ = hoveredRow;
        if (hoveredRow >= 36)
        {
            activate = true;
        }
        else if (hoveredRow == 6 || hoveredRow == 13 || hoveredRow == 14
                 || hoveredRow == 20 || hoveredRow == 21)
        {
            delta = 1;
        }
        else
        {
            delta = MenuStepperClickDelta(SettingsRowRect(hoveredRow - firstVisible), mouse);
        }
    }

    if (activate && settingsIndex_ < 36)
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
        case 11:
            ambientOcclusionQuality_ = (ambientOcclusionQuality_ + delta + 3) % 3;
            renderer_.SetAmbientOcclusionQuality(ambientOcclusionQuality_);
            break;
        case 12: effectsQuality_ = (effectsQuality_ + delta + 3) % 3; break;
        case 13: postProcessing_ = !postProcessing_; break;
        case 14: bloomEnabled_ = !bloomEnabled_; break;
        case 15: bloomQuality_ = (bloomQuality_ + delta + 3) % 3; break;
        case 16: masterVolume_ = std::clamp(masterVolume_ + delta * 0.1f, 0.0f, 1.0f); break;
        case 17: musicVolume_ = std::clamp(musicVolume_ + delta * 0.1f, 0.0f, 1.0f); break;
        case 18: sfxVolume_ = std::clamp(sfxVolume_ + delta * 0.1f, 0.0f, 1.0f); break;
        case 19: ambientVolume_ = std::clamp(ambientVolume_ + delta * 0.1f, 0.0f, 1.0f); break;
        case 20: reducedCameraShake_ = !reducedCameraShake_; break;
        case 21: reducedFlashes_ = !reducedFlashes_; break;
        case 22:
            firstPersonMotionMode_ = (firstPersonMotionMode_ + delta + 3) % 3;
            firstPersonMotion_.SetMode(static_cast<FirstPersonMotionMode>(firstPersonMotionMode_));
            break;
        case 23: ApplyShaderPreset((shaderPreset_ + delta + 3) % 3); break;
        case 24: shaderSettings_.materialQuality = (shaderSettings_.materialQuality + delta + 3) % 3; break;
        case 25: shaderSettings_.volumetricQuality = (shaderSettings_.volumetricQuality + delta + 4) % 4; break;
        case 26: shaderSettings_.haze += delta * 0.1f; break;
        case 27: shaderSettings_.sunIntensity += delta * 0.1f; break;
        case 28: shaderSettings_.exposure += delta * 0.1f; break;
        case 29: shaderSettings_.bloomIntensity += delta * 0.1f; break;
        case 30: shaderSettings_.saturation += delta * 0.1f; break;
        case 31: shaderSettings_.skyQuality = (shaderSettings_.skyQuality + delta + 3) % 3; break;
        case 32: shaderSettings_.giQuality = (shaderSettings_.giQuality + delta + 3) % 3; break;
        case 33: shaderSettings_.giStrength += delta * 0.1f; break;
        case 34: shaderSettings_.localShadows = !shaderSettings_.localShadows; break;
        case 35: shaderSettings_.shadowSoftness += delta * 0.25f; break;
        default: break;
        }
        shaderSettings_.Clamp();
        if ((settingsIndex_ >= 8 && settingsIndex_ <= 15) || (settingsIndex_ >= 24 && settingsIndex_ <= 35))
            shaderPreset_ = 3;
        audio_.SetVolume(masterVolume_);
        audio_.SetCategoryVolumes(sfxVolume_, ambientVolume_);
        music_.SetVolume(masterVolume_, musicVolume_);
        SaveSettings();
    }

    if (activate)
    {
        if (settingsIndex_ == 36)
        {
            controlsReturnScreen_ = GameScreen::Settings;
            screen_ = GameScreen::Controls;
            waitingForKey_ = false;
        }
        else if (settingsIndex_ == 37)
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
    constexpr int kActionCount = 17;
#else
    constexpr int kActionCount = 16;
#endif
    constexpr int kControlRows = kActionCount;
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
            bindings.sprint = key;
            break;
        case 7:
            bindings.attack = key;
            break;
        case 8:
            bindings.place = key;
            break;
        case 9:
            bindings.interact = key;
            break;
        case 10:
            bindings.inventory = key;
            break;
        case 11:
            bindings.drop = key;
            break;
        case 12:
            bindings.cameraToggle = key;
            break;
        case 13:
#if DAIBED_DEVELOPER_BUILD
            bindings.debugRespawn = key;
#else
            bindings.heroActive1 = key;
#endif
            break;
        case 14:
#if DAIBED_DEVELOPER_BUILD
            bindings.heroActive1 = key;
#else
            bindings.heroActive2 = key;
#endif
            break;
        case 15:
#if DAIBED_DEVELOPER_BUILD
            bindings.heroActive2 = key;
#else
            bindings.heroUltimate = key;
#endif
            break;
        case 16:
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
        case 6: padBindings.sprint = button; break;
        case 7: padBindings.attack = button; break;
        case 8: padBindings.place = button; break;
        case 9: padBindings.interact = button; break;
        case 10: padBindings.inventory = button; break;
        case 11: padBindings.drop = button; break;
        case 12: padBindings.cameraToggle = button; break;
        case 14: padBindings.heroActive1 = button; break;
        case 15: padBindings.heroActive2 = button; break;
        case 16: padBindings.heroUltimate = button; break;
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
        waitingForKey_ = true;
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
    if (creativeMapBrowserOpen_)
    {
        const int itemCount = static_cast<int>(creativeMapBrowserPaths_.size());
        if (itemCount <= 0)
        {
            creativeMapBrowserOpen_ = false;
            return;
        }
        creativeMapBrowserIndex_ = std::clamp(creativeMapBrowserIndex_, 0, itemCount - 1);
        if (IsKeyPressed(KEY_ESCAPE))
        {
            creativeMapBrowserOpen_ = false;
            return;
        }
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
        {
            creativeMapBrowserIndex_ = (creativeMapBrowserIndex_ + 1) % itemCount;
        }
        if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
        {
            creativeMapBrowserIndex_ = (creativeMapBrowserIndex_ + itemCount - 1) % itemCount;
        }
        const float wheel = GetMouseWheelMove();
        if (wheel < -0.01f)
        {
            creativeMapBrowserIndex_ = (creativeMapBrowserIndex_ + 1) % itemCount;
        }
        else if (wheel > 0.01f)
        {
            creativeMapBrowserIndex_ = (creativeMapBrowserIndex_ + itemCount - 1) % itemCount;
        }

        const int visible = std::min(kCreativeMapBrowserVisibleRows, itemCount);
        const int first = CreativeMapBrowserFirstRow(itemCount, creativeMapBrowserIndex_);
        const Vector2 mouse = GetMousePosition();
        int hovered = -1;
        for (int row = 0; row < visible; ++row)
        {
            if (CheckCollisionPointRec(mouse, CreativeMapBrowserRowRect(row, visible)))
            {
                hovered = first + row;
                break;
            }
        }
        if (hovered >= 0 && (std::fabs(GetMouseDelta().x) > 0.5f || std::fabs(GetMouseDelta().y) > 0.5f))
        {
            creativeMapBrowserIndex_ = hovered;
        }
        const bool activate = IsKeyPressed(KEY_ENTER) || (hovered >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
        if (activate)
        {
            if (hovered >= 0)
            {
                creativeMapBrowserIndex_ = hovered;
            }
            if (LoadCreativeMapBrowserSelection())
            {
                screen_ = GameScreen::Playing;
                DisableCursor();
            }
        }
        return;
    }

    const std::vector<PauseEntry> entries = BuildPauseEntries(creativeMode_, creativeTestActive_);
    const int rowCount = static_cast<int>(entries.size());
    pauseIndex_ = std::clamp(pauseIndex_, 0, rowCount - 1);
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S))
    {
        pauseIndex_ = (pauseIndex_ + 1) % rowCount;
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W))
    {
        pauseIndex_ = (pauseIndex_ + rowCount - 1) % rowCount;
    }
    const Vector2 mouse = GetMousePosition();
    const Vector2 mouseMove = GetMouseDelta();
    int hoveredRow = -1;
    for (int i = 0; i < rowCount; ++i)
    {
        if (CheckCollisionPointRec(mouse, PauseRowRect(i, rowCount)))
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
        pauseIndex_ = (pauseIndex_ + 1) % rowCount;
    }
    else if (wheel > 0.01f)
    {
        pauseIndex_ = (pauseIndex_ + rowCount - 1) % rowCount;
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

    switch (entries[static_cast<std::size_t>(pauseIndex_)].action)
    {
    case PauseEntryAction::Resume:
        screen_ = GameScreen::Playing;
        DisableCursor();
        break;
    case PauseEntryAction::RestartMatch:
        StartSelectedMatch();
        break;
    case PauseEntryAction::TestMap:
        StartCreativeMapTest();
        break;
    case PauseEntryAction::RestartTest:
        RestartCreativeMapTest();
        break;
    case PauseEntryAction::SaveMap:
        SaveCreativeMapToFile();
        screen_ = GameScreen::Playing;
        DisableCursor();
        break;
    case PauseEntryAction::LoadMap:
        OpenCreativeMapBrowser();
        break;
    case PauseEntryAction::BackToEditor:
        ReturnToCreativeEditor();
        break;
    case PauseEntryAction::OpenSettings:
        returnScreen_ = GameScreen::Paused;
        screen_ = GameScreen::Settings;
        break;
    case PauseEntryAction::MainMenu:
        screen_ = GameScreen::MainMenu;
        creativeMode_ = false; // leaving a creative session back to the menu
        creativeTestActive_ = false;
        EnableCursor();
        break;
    case PauseEntryAction::ExitGame:
        exitRequested_ = true;
        break;
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
        "Креатив",
        "Настройки",
        "Управление",
        "Выход"
    };
    const std::string values[] {
        "",
        "",
        "",
        "",
        "",
        ""
    };
#else
    const char* labels[] {
        "Сетевая игра",
        "Одиночная игра",
        "Креатив",
        "Обучение",
        "Настройки",
        "Управление",
        "Выход"
    };
    const std::string values[] {
        "",
        "",
        "",
        "",
        "",
        "",
        ""
    };
#endif

    // Minecraft-style title block: big pixel logotype + subtitle, version pinned
    // to the bottom-left corner like MC's splash screen.
    DrawCenteredText("DaiBed", 34, 56, kAccentGold);
#if DAIBED_DEVELOPER_BUILD
    DrawCenteredText("Геройский BedWars на изменяемых аренах", 96, 18, Fade(kTextDim, 0.9f));
#else
    DrawCenteredText("Геройский BedWars против ботов", 96, 18, Fade(kTextDim, 0.9f));
#endif
    DrawTextShadow("DaiBed " DAIBED_VERSION, 10, GetScreenHeight() - 26, 16, Fade(kTextDim, 0.8f));

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
            // Action rows are classic beveled buttons (the reference look).
            MenuButton(row, labels[i], MenuButtonStyle::Accent, selected, true);
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
    (void)biomeHint;
#else
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
        MenuFieldLabel("Отображаемое имя (ID добавит сервер)", layout.joinName.x, layout.joinName.y - 17.0f);
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
        const bool running = IsServerProcessRunning(localServerProcess_)
            || IntegratedListenServerRunning();
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
            if (serverConfig_.networkBackend == NetworkBackend::SteamP2P
                && steamLobbyService_ != nullptr)
            {
                statusText += SteamLobbyReady() ? "  |  Steam Lobby готово"
                                                : "  |  Создание Steam Lobby...";
            }
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
        const bool steamHost = serverConfig_.networkBackend == NetworkBackend::SteamP2P;
        MenuButton(layout.hostCopy, steamHost ? "Пригласить друзей" : "Копировать вход",
                   MenuButtonStyle::Primary, sel(12), !steamHost || SteamLobbyReady());
        MenuButton(layout.hostBack, "Назад", MenuButtonStyle::Ghost, sel(13), true);
    }
    RenderSteamFriendPicker();
}

void Game::RenderHeroSelect() const
{
    constexpr int heroCount = HeroSystem::kHeroCount;
    constexpr int startControl = heroCount + kHeroMatchSettingCount;
    constexpr int backControl = startControl + 1;
    const HeroSelectLayout layout = BuildHeroSelectLayout();
    const Vector2 mouse = GetMousePosition();
    const auto& selected = HeroSystem::GetDefinitionByIndex(heroSelectIndex_);

    // Full-screen hero showcase: broad cinematic bands and angular separators,
    // without the lobby/currency/meta-navigation chrome from the reference.
    DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(),
                           Color { 5, 7, 11, 255 }, Color { 11, 16, 24, 255 });
    DrawRectangleGradientH(0, 58, GetScreenWidth(), GetScreenHeight() - 170,
                           Fade(VisualTheme::HeroAccent(selected.id), 0.07f), Fade(BLACK, 0.15f));
    DrawLineEx(Vector2 { 24.0f, 58.0f }, Vector2 { 424.0f, 58.0f }, 2.0f, Fade(WHITE, 0.24f));
    DrawLineEx(Vector2 { 424.0f, 58.0f }, Vector2 { 454.0f, 78.0f }, 2.0f, Fade(WHITE, 0.24f));
    DrawLineEx(Vector2 { static_cast<float>(GetScreenWidth()) - 24.0f, 58.0f },
               Vector2 { static_cast<float>(GetScreenWidth()) - 424.0f, 58.0f }, 2.0f, Fade(WHITE, 0.24f));
    DrawLineEx(Vector2 { static_cast<float>(GetScreenWidth()) - 424.0f, 58.0f },
               Vector2 { static_cast<float>(GetScreenWidth()) - 454.0f, 78.0f }, 2.0f, Fade(WHITE, 0.24f));
    DrawTextShadow("DaiBed", 22, 22, 16, Fade(kTextDim, 0.76f));
    DrawCenteredText("ВЫБОР ГЕРОЯ", 23, 25, kTextBright);

    DrawTextShadow("ГЕРОИ", static_cast<int>(layout.heroPane.x),
                   static_cast<int>(layout.heroPane.y) + 5, 18, kTextBright);

    for (int i = 0; i < heroCount; ++i)
    {
        const HeroDefinition& hero = HeroSystem::GetDefinitionByIndex(i);
        const Rectangle row = HeroSelectHeroRow(layout, i);
        const bool current = i == heroSelectIndex_;
        const bool hovered = CheckCollisionPointRec(mouse, row);
        const bool focused = heroSelectControlIndex_ == i;
        const Color heroColor = VisualTheme::HeroAccent(hero.id);
        DrawRectangleRec(row, current ? Fade(heroColor, 0.30f)
                                      : (hovered ? Fade(Color { 62, 70, 84, 255 }, 0.90f)
                                                 : Fade(Color { 16, 20, 28, 255 }, 0.82f)));
        DrawRectangleGradientV(static_cast<int>(row.x) + 2, static_cast<int>(row.y) + 2,
                               static_cast<int>(row.width) - 4, static_cast<int>(row.height) - 4,
                               Fade(heroColor, current ? 0.22f : 0.08f), Fade(BLACK, 0.24f));
        if (current || hovered || focused)
        {
            PixelBorder(row, focused ? 2 : 1,
                        current ? Fade(heroColor, 0.96f) : Fade(WHITE, 0.74f));
        }
        DrawCircle(static_cast<int>(row.x + row.width * 0.5f), static_cast<int>(row.y + 31.0f),
                   19.0f, Fade(heroColor, current ? 0.90f : 0.46f));
        DrawCircleLines(static_cast<int>(row.x + row.width * 0.5f), static_cast<int>(row.y + 31.0f),
                        22.0f, Fade(WHITE, current || hovered ? 0.72f : 0.18f));
        DrawRectangle(static_cast<int>(row.x + row.width * 0.5f) - 9,
                      static_cast<int>(row.y) + 23, 18, 18, Fade(Color { 8, 10, 14, 255 }, 0.72f));
        const int nameWidth = MeasureText(hero.name.c_str(), 15);
        DrawTextShadow(hero.name.c_str(), static_cast<int>(row.x + (row.width - nameWidth) * 0.5f),
                       static_cast<int>(row.y) + 62, 15,
                       current ? heroColor : (hovered ? WHITE : Fade(kTextBright, 0.80f)));
    }

    const float detailY = HeroSelectDetailY(layout);
    DrawTextShadow(selected.name.c_str(), static_cast<int>(layout.heroPane.x),
                   static_cast<int>(detailY), 28, WHITE);
    DrawText(selected.role.c_str(), static_cast<int>(layout.heroPane.x),
             static_cast<int>(detailY) + 34, 15, VisualTheme::HeroAccent(selected.id));
    DrawTextShadow(selected.passiveName.c_str(), static_cast<int>(layout.heroPane.x),
                   static_cast<int>(detailY) + 60, 16, kAccentGold);
    const int passiveLines = std::max(2, std::min(5,
        static_cast<int>((layout.matchPane.y - (detailY + 84.0f) - 8.0f) / 18.0f)));
    DrawWrappedTextLimited(selected.passiveDescription,
                           static_cast<int>(layout.heroPane.x),
                           static_cast<int>(detailY) + 84, 13,
                           static_cast<int>(layout.heroPane.width) - 8, passiveLines, Fade(kTextBright, 0.76f));

    renderer_.RenderHeroPreview(selected.id, layout.preview, heroPreviewYaw_);
    const char* rotateHint = "Зажмите и тяните, чтобы повернуть";
    DrawText(rotateHint,
             static_cast<int>(layout.stagePane.x + (layout.stagePane.width - MeasureText(rotateHint, 12)) * 0.5f),
             static_cast<int>(layout.matchPane.y) - 19, 12, Fade(kTextDim, 0.66f));

    DrawTextShadow("СПОСОБНОСТИ", static_cast<int>(layout.abilityPane.x),
                   static_cast<int>(layout.abilityPane.y) + 5, 18, kTextBright);
    int abilityY = static_cast<int>(layout.abilityPane.y) + 42;
    const int abilityHeight = std::max(88, static_cast<int>(
        (layout.matchPane.y - static_cast<float>(abilityY) - 8.0f) / 3.0f));
    const auto drawAbilityBlock = [&](const char* key, const std::string& title,
                                      const std::string& description, const std::string& meta,
                                      Color color, int height)
    {
        const Rectangle block { layout.abilityPane.x, static_cast<float>(abilityY),
                                layout.abilityPane.width, static_cast<float>(height - 7) };
        const bool hovered = CheckCollisionPointRec(mouse, block);
        DrawRectangleRec(block, Fade(Color { 11, 15, 22, 255 }, hovered ? 0.92f : 0.68f));
        if (hovered) PixelBorder(block, 1, Fade(color, 0.62f));
        const Rectangle icon { block.x + 2.0f, block.y + 5.0f, 48.0f, 48.0f };
        DrawRectangleRec(icon, Fade(color, 0.16f));
        PixelBorder(icon, 1, Fade(color, 0.72f));
        const int keyWidth = MeasureText(key, 18);
        DrawTextShadow(key, static_cast<int>(icon.x + (icon.width - keyWidth) * 0.5f),
                       static_cast<int>(icon.y) + 14, 18, color);
        DrawTextShadow(title.c_str(), static_cast<int>(block.x) + 62,
                       static_cast<int>(block.y) + 4, 16, color);
        const int textY = static_cast<int>(block.y) + 28;
        const int metaLineCount = meta.empty()
            ? 0
            : 1 + static_cast<int>(std::count(meta.begin(), meta.end(), '\n'));
        const int reservedMetaHeight = metaLineCount == 0 ? 5 : metaLineCount * 13 + 5;
        const int maxDescriptionLines = std::max(2,
            (static_cast<int>(block.height) - 28 - reservedMetaHeight - 5) / 17);
        DrawWrappedTextLimited(description, static_cast<int>(block.x) + 62, textY, 12,
                               static_cast<int>(block.width) - 68, maxDescriptionLines,
                               Fade(kTextBright, 0.78f));
        if (!meta.empty())
        {
            std::istringstream metaStream(meta);
            std::string metaLine;
            int metaY = static_cast<int>(block.y + block.height) - metaLineCount * 13 - 2;
            while (std::getline(metaStream, metaLine))
            {
                DrawText(metaLine.c_str(), static_cast<int>(block.x) + 62, metaY, 11,
                         Fade(kTextDim, 0.78f));
                metaY += 13;
            }
        }
        abilityY += height;
    };
    drawAbilityBlock(KeyLabel(input_.GetBindings().heroActive1), selected.active1.name,
                     selected.active1.description, AbilityMetaText(selected.active1), kAccentCyan, abilityHeight);
    drawAbilityBlock(KeyLabel(input_.GetBindings().heroActive2), selected.active2.name,
                     selected.active2.description, AbilityMetaText(selected.active2), kAccentCyan, abilityHeight);
    drawAbilityBlock(KeyLabel(input_.GetBindings().heroUltimate), selected.ultimate.name,
                     selected.ultimate.description, AbilityMetaText(selected.ultimate), kAccentGold, abilityHeight);

    DrawRectangleRec(layout.matchPane, Fade(Color { 6, 8, 12, 255 }, 0.94f));
    PixelBorder(layout.matchPane, 2, Fade(WHITE, 0.16f));
    DrawRectangle(static_cast<int>(layout.matchPane.x) + 2, static_cast<int>(layout.matchPane.y) + 2,
                  static_cast<int>(layout.matchPane.width) - 4, 3, Fade(kAccentCyan, 0.62f));
    DrawTextShadow("ПАРАМЕТРЫ МАТЧА", static_cast<int>(layout.matchPane.x) + 12,
                   static_cast<int>(layout.matchPane.y) + 13, 16, kTextBright);
    const char* settingLabels[kHeroMatchSettingCount] {
        "Режим", "Команда", "Размер команды", "Боты", "Сложность", "Стратегия", "План арены", "Биом"
    };
    const std::string settingValues[kHeroMatchSettingCount] {
        MatchModeName(), TeamName(selectedTeamId_), TeamSizeName(), BotCountName(),
        BotDifficultyName(), BotStrategyProfileName(), ArenaLayoutName(), ArenaBiomeName()
    };
    for (int i = 0; i < kHeroMatchSettingCount; ++i)
    {
        const Rectangle row = HeroSelectMatchRow(layout, i);
        const bool hovered = CheckCollisionPointRec(mouse, row);
        const bool focused = heroSelectControlIndex_ == heroCount + i;
        DrawRectangleRec(row, Fade(Color { 20, 24, 32, 255 }, hovered || focused ? 0.96f : 0.72f));
        PixelBorder(row, focused ? 2 : 1,
                    hovered || focused ? Fade(kAccentCyan, 0.78f) : Fade(WHITE, 0.14f));
        const int labelWidth = MeasureText(settingLabels[i], 10);
        DrawText(settingLabels[i], static_cast<int>(row.x + (row.width - labelWidth) * 0.5f),
                 static_cast<int>(row.y) + 5, 10, Fade(kTextDim, 0.82f));
        DrawText("<", static_cast<int>(row.x) + 7, static_cast<int>(row.y) + 24, 14, kAccentCyan);
        DrawText(">", static_cast<int>(row.x + row.width) - 15, static_cast<int>(row.y) + 24, 14, kAccentCyan);
        const std::string clipped = ClipTextToWidth(settingValues[i], static_cast<int>(row.width) - 34, 12);
        const int valueWidth = MeasureText(clipped.c_str(), 12);
        DrawTextShadow(clipped.c_str(), static_cast<int>(row.x + (row.width - valueWidth) * 0.5f),
                       static_cast<int>(row.y) + 25, 12, hovered || focused ? WHITE : Fade(kTextBright, 0.84f));
    }

    MenuButton(layout.startButton, "НАЧАТЬ МАТЧ", MenuButtonStyle::Accent,
               heroSelectControlIndex_ == startControl || CheckCollisionPointRec(mouse, layout.startButton), true, 17);
    MenuButton(layout.backButton, "Назад", MenuButtonStyle::Ghost,
               heroSelectControlIndex_ == backControl || CheckCollisionPointRec(mouse, layout.backButton), true, 16);
}

void Game::RenderSettings() const
{
    constexpr int kSettingsRows = 38;
    constexpr int kVisibleRows = 12;
    const char* labels[kSettingsRows] {
        "Чувствительность мыши", "Чувствительность геймпада", "Мёртвая зона стиков", "Угол обзора",
        "Разрешение", "Режим окна", "VSync", "Ограничение FPS", "Масштаб рендера", "Дальность прорисовки",
        "Качество теней", "Затенение окружения", "Качество эффектов", "Постобработка", "Свечение", "Качество свечения", "Общая громкость", "Музыка",
        "Эффекты", "Окружение", "Ослабить тряску камеры", "Ослабить вспышки",
        "Движение от первого лица", "Профиль шейдеров", "Материалы", "Объёмный свет",
        "Плотность дымки", "Сила солнца", "Экспозиция", "Сила свечения", "Насыщенность", "Небо",
        "Глобальное освещение", "Сила непрямого света", "Тени от ламп", "Мягкость теней",
        "Настроить управление", "Назад"
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
        quality(ambientOcclusionQuality_), quality(effectsQuality_), toggle(postProcessing_), toggle(bloomEnabled_),
        quality(bloomQuality_), percent(masterVolume_),
        percent(musicVolume_), percent(sfxVolume_), percent(ambientVolume_),
        toggle(reducedCameraShake_), toggle(reducedFlashes_),
        firstPersonMotionMode_ == 0 ? "Выкл." : (firstPersonMotionMode_ == 1 ? "Сниженное" : "Полное"),
        shaderPreset_ == 0 ? "Быстрый" : (shaderPreset_ == 1 ? "Баланс" : (shaderPreset_ == 2 ? "Кино" : "Свой")),
        shaderSettings_.materialQuality == 0 ? "Простые" : (shaderSettings_.materialQuality == 1 ? "Рельеф" : "Отражения"),
        shaderSettings_.volumetricQuality == 0 ? "Выкл." : (shadowQuality_ == 0 ? "Нужны тени" :
            (shaderSettings_.volumetricQuality == 1 ? "Низкое" : (shaderSettings_.volumetricQuality == 2 ? "Среднее" : "Высокое"))),
        percent(shaderSettings_.haze), percent(shaderSettings_.sunIntensity), percent(shaderSettings_.exposure),
        percent(shaderSettings_.bloomIntensity), percent(shaderSettings_.saturation),
        shaderSettings_.skyQuality == 0 ? "Простое" : (shaderSettings_.skyQuality == 1 ? "Атмосфера" : "Облака"),
        shaderSettings_.giQuality == 0 ? "Выкл." : (shaderSettings_.giQuality == 1 ? "Среднее" : "Высокое"),
        percent(shaderSettings_.giStrength), toggle(shaderSettings_.localShadows), FormatTenths(shaderSettings_.shadowSoftness),
        "", ""
    };

    DrawCenteredText("Настройки", 56, 40, kTextBright);
    const Rectangle panel = SettingsPanelRect();
    const int firstVisible = std::clamp(settingsFirstVisible_, 0, kSettingsRows - kVisibleRows);
    MenuPanel(panel);

    // Category sidebar (click jumps to the section; highlight follows the
    // selected row).
    const int activeSection = SettingsSectionForRow(settingsIndex_);
    for (int i = 0; i < kSettingsSectionCount; ++i)
    {
        const Rectangle item = SettingsSidebarRect(i);
        const bool active = i == activeSection;
        if (active)
        {
            DrawRectangleRec(item, kMenuRowSel);
            PixelBorder(item, 2, Fade(WHITE, 0.35f));
            DrawRectangle(static_cast<int>(item.x) + 2, static_cast<int>(item.y) + 2,
                          4, static_cast<int>(item.height) - 4, kAccentCyan);
        }
        DrawTextShadow(kSettingsSections[i].name, static_cast<int>(item.x) + 20,
                       CenteredTextY(item, 18), 18, active ? kTextBright : Fade(kTextDim, 0.9f));
    }
    const int sepX = static_cast<int>(panel.x) + 14 + kSettingsSidebarW + 9;
    DrawRectangle(sepX, static_cast<int>(panel.y) + 10, 2,
                  static_cast<int>(panel.height) - 20, Fade(WHITE, 0.08f));

    // Boolean rows draw as checkboxes (reference look), actions as buttons,
    // everything else as < value > steppers.
    const auto isToggleRow = [](int i)
    {
        return i == 6 || i == 13 || i == 14 || i == 20 || i == 21;
    };
    const Rectangle firstRow = SettingsRowRect(0);
    BeginScissorMode(static_cast<int>(firstRow.x) - 4, static_cast<int>(panel.y) + 8,
                     static_cast<int>(firstRow.width) + 8, static_cast<int>(panel.height) - 16);
    for (int visible = 0; visible < kVisibleRows; ++visible)
    {
        const int i = firstVisible + visible;
        const bool selected = i == settingsIndex_;
        const Rectangle row = SettingsRowRect(visible);
        if (i >= 36)
        {
            MenuButton(row, labels[i], MenuButtonStyle::Accent, selected, true, 18);
        }
        else if (isToggleRow(i))
        {
            MenuTogglePill(row, labels[i], values[i] == "Вкл.", selected, 18);
        }
        else
        {
            MenuStepperRow(row, labels[i], values[i], selected, 18);
        }
    }
    EndScissorMode();

    // Pixel scrollbar (right edge of the list, reference style).
    const float scrollFraction = static_cast<float>(firstVisible) / static_cast<float>(kSettingsRows - kVisibleRows);
    const Rectangle track {
        panel.x + panel.width - 26.0f, panel.y + 14.0f,
        12.0f, panel.height - 28.0f
    };
    DrawRectangleRec(track, kMenuField);
    PixelBorder(track, 2, kPixelOutline);
    const float thumbH = std::max(44.0f, track.height * static_cast<float>(kVisibleRows) / static_cast<float>(kSettingsRows));
    const Rectangle thumb {
        track.x + 2.0f,
        track.y + 2.0f + (track.height - 4.0f - thumbH) * scrollFraction,
        track.width - 4.0f,
        thumbH
    };
    DrawRectangleRec(thumb, MixColor(kButtonTop, kAccentCyan, 0.18f));
    PixelBevel(thumb, 2, false);
    const char* hint = "";
    if (settingsIndex_ == 23) hint = "Профиль меняет графику; отдельные параметры можно настроить после.";
    else if (settingsIndex_ == 24) hint = "Рельеф поверхностей, блики и число локальных источников света.";
    else if (settingsIndex_ == 25) hint = "Солнечные лучи в дымке. Требуются тени; высокое качество нагружает GPU.";
    else if (settingsIndex_ == 26) hint = "Атмосферная дымка. Дальние границы мира скрываются даже при 0%.";
    else if (settingsIndex_ == 28 || settingsIndex_ == 30)
        hint = postProcessing_ ? "Цвет сцены меняется без перезапуска. Интерфейс сохраняет свои цвета."
                               : "Для этого эффекта включите постобработку в разделе «Графика».";
    else if (settingsIndex_ == 29)
        hint = postProcessing_ && bloomEnabled_ ? "Интенсивность свечения. При 0% проходы bloom отключаются."
                                               : "Включите постобработку и свечение в разделе «Графика».";
    else if (settingsIndex_ == 31) hint = "Атмосфера: солнце и небосвод. Облака добавляют плавное движение.";
    else if (settingsIndex_ == 32 || settingsIndex_ == 33) hint = "Непрямой свет и цветные отражения блоков. Высокое качество: больше лучей.";
    else if (settingsIndex_ == 34) hint = "Квадратный свет ламп учитывает блоки, полублоки и ступени рядом с камерой.";
    else if (settingsIndex_ == 35) hint = "0: резкие тени. Высокое качество теней учитывает расстояние до препятствия.";
    DrawCenteredText(hint, std::min(GetScreenHeight() - 24, static_cast<int>(panel.y + panel.height) + 12), 14, kTextDim);
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
        "Спринт",
        "Атака / ломать",
        "Использовать / ставить",
        "Магазин / действие",
        "Творческая палитра",
        "Выбросить",
        "Камера",
#if DAIBED_DEVELOPER_BUILD
        "Отладочный респаун",
#endif
        "Активка 1",
        "Активка 2",
        "Ульта"
    };
    const int keys[] {
        bindings.moveForward,
        bindings.moveBackward,
        bindings.moveLeft,
        bindings.moveRight,
        bindings.jump,
        bindings.sneak,
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
        bindings.heroUltimate
    };
    const int padButtons[] {
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        GAMEPAD_BUTTON_UNKNOWN,
        padBindings.jump,
        padBindings.sneak,
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
        padBindings.heroUltimate
    };
#if DAIBED_DEVELOPER_BUILD
    constexpr int kActionCount = 18;
#else
    constexpr int kActionCount = 16;
#endif
    constexpr int kControlRows = kActionCount;
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
    if (creativeMapBrowserOpen_)
    {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.68f));
        const int itemCount = static_cast<int>(creativeMapBrowserPaths_.size());
        const int visible = std::min(kCreativeMapBrowserVisibleRows, itemCount);
        const int first = itemCount > 0 ? CreativeMapBrowserFirstRow(itemCount, creativeMapBrowserIndex_) : 0;
        DrawCenteredText("Загрузить карту", GetScreenHeight() / 2 - 232, 38, kTextBright);
        DrawCenteredText("Enter/ЛКМ — открыть | Esc — отмена", GetScreenHeight() / 2 - 195, 16, Fade(kTextDim, 0.82f));
        const Rectangle panel = CreativeMapBrowserPanelRect(std::max(1, visible));
        MenuPanel(panel);
        if (itemCount == 0)
        {
            DrawCenteredText("В папке maps нет .dbmap карт.", static_cast<int>(panel.y + 56.0f), 18, kTextDim);
            return;
        }
        for (int row = 0; row < visible; ++row)
        {
            const int index = first + row;
            const Rectangle bounds = CreativeMapBrowserRowRect(row, visible);
            const bool selected = index == creativeMapBrowserIndex_;
            MenuRow(bounds, selected, kAccentGold);
            DrawText(creativeMapBrowserPaths_[static_cast<std::size_t>(index)].c_str(),
                     static_cast<int>(bounds.x + 18.0f), static_cast<int>(bounds.y + 7.0f), 18,
                     selected ? kAccentGold : Fade(kTextBright, 0.84f));
        }
        if (itemCount > visible)
        {
            const std::string counter = std::to_string(creativeMapBrowserIndex_ + 1) + " / " + std::to_string(itemCount);
            DrawText(counter.c_str(), static_cast<int>(panel.x + panel.width - 66.0f),
                     static_cast<int>(panel.y + panel.height - 25.0f), 14, Fade(kTextDim, 0.75f));
        }
        return;
    }

    const std::vector<PauseEntry> entries = BuildPauseEntries(creativeMode_, creativeTestActive_);
    const int rowCount = static_cast<int>(entries.size());
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.55f));
    const char* title = creativeMode_ ? "Креатив — пауза" : (creativeTestActive_ ? "Тест карты — пауза" : "Пауза");
    DrawCenteredText(title, GetScreenHeight() / 2 - 154, 42, kTextBright);

    const Rectangle panel = PausePanelRect(rowCount);
    MenuPanel(panel);
    for (int i = 0; i < rowCount; ++i)
    {
        const Rectangle row = PauseRowRect(i, rowCount);
        const bool selected = i == pauseIndex_;
        MenuRow(row, selected, kAccentGold);
        DrawText(entries[static_cast<std::size_t>(i)].label,
                 static_cast<int>(row.x + 20.0f), static_cast<int>(row.y + 7.0f), 20,
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
#endif
        + " | " + KeyLabel(bindings.sprint) + " / двойной " + KeyLabel(bindings.moveForward) + " спринт"
        + " | " + KeyLabel(bindings.cameraToggle) + " камера"
        + " | Tab таблица";
    const std::string combatHints = std::string(KeyLabel(bindings.heroActive1)) + "/"
        + KeyLabel(bindings.heroActive2) + "/"
        + KeyLabel(bindings.heroUltimate) + " способности"
        + " | CapsLock+колесо варианты"
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

    const float remaining = std::max(0.0f, coreCollapseSeconds_ - matchSimulation_.MatchTimeSeconds());
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
        if (stack.count > 0)
        {
            const std::string count = std::to_string(stack.count);
            DrawText(count.c_str(), sx + slotSize - MeasureText(count.c_str(), 12) - 3, sy + 3, 12, WHITE);
        }
    };

    const int hotbarX = inventoryPanelX + 21;
    const int hotbarY = y + 58 + 3 * (inventorySlotSize + inventoryGap) + 14;
    DrawRectangle(inventoryPanelX, hotbarY - 42, inventoryPanelWidth, 108,
        Fade(BLACK, 0.72f));
    DrawRectangleLines(inventoryPanelX, hotbarY - 42, inventoryPanelWidth, 108,
        Fade(WHITE, 0.22f));
    DrawText("Хотбар", inventoryPanelX + 18, hotbarY - 31, 18, WHITE);
    const auto& playerHotbar = player->GetInventory().GetHotbarSlots();
    for (int slot = 0; slot < kHotbarSlotCount; ++slot)
    {
        const int sx = hotbarX + slot * (inventorySlotSize + inventoryGap);
        const Rectangle bounds {
            static_cast<float>(sx), static_cast<float>(hotbarY),
            static_cast<float>(inventorySlotSize), static_cast<float>(inventorySlotSize) };
        DrawRectangle(sx, hotbarY, inventorySlotSize, inventorySlotSize,
            Fade(Color { 24, 28, 36, 255 }, 0.88f));
        DrawRectangleLines(sx, hotbarY, inventorySlotSize, inventorySlotSize,
            CheckCollisionPointRec(mouse, bounds) ? Color { 112, 232, 255, 255 } : Fade(WHITE, 0.18f));
        const ItemStack& stack = playerHotbar[slot];
        if (!stack.IsEmpty())
        {
            DrawRectangle(sx + 12, hotbarY + 10, inventorySlotSize - 24,
                inventorySlotSize - 23, itemColor(stack.type));
            const std::string count = std::to_string(stack.count);
            DrawText(count.c_str(), sx + inventorySlotSize - MeasureText(count.c_str(), 12) - 4,
                hotbarY + 4, 12, WHITE);
        }
    }

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

        std::string navigationLabel;
        const auto controller = botNavigationControllers_.find(player.GetId());
        if (controller != botNavigationControllers_.end())
        {
            const NavigationDebugSnapshot& nav = controller->second.DebugSnapshot();
            navigationLabel = " / nav:" + std::string(ToString(nav.executionStatus))
                + ':' + ToString(nav.phase)
                + " #" + std::to_string(nav.movementIndex)
                + " repath=" + ToString(nav.lastRepath)
                + " stuck=" + std::to_string(static_cast<int>(nav.noProgressSeconds * 10.0f))
                + " threat=" + std::to_string(static_cast<int>(nav.currentThreat));
        }
        const std::string label = std::string(ToString(memory->role)) + " / " + ToString(memory->intent)
            + " " + std::to_string(static_cast<int>(memory->intentScore))
            + " / план: " + ToString(memory->currentPlan.goal)
            + (memory->currentPlan.targetTeamId >= 0 ? "->" + std::to_string(memory->currentPlan.targetTeamId) : "")
            + (memory->intentReason.empty() ? "" : " / " + memory->intentReason)
            + (memory->roleReason.empty() ? "" : " / роль: " + memory->roleReason)
            + navigationLabel;
        const int width = MeasureText(label.c_str(), 14) + 12;
        DrawRectangle(static_cast<int>(screen.x) - width / 2, static_cast<int>(screen.y) - 4, width, 22, Fade(BLACK, 0.55f));
        DrawText(label.c_str(), static_cast<int>(screen.x) - width / 2 + 6, static_cast<int>(screen.y), 14, Color { 255, 235, 142, 255 });
    }
}

void Game::RenderNavigationDebugScene() const
{
    const auto movementColor = [](MovementType type)
    {
        switch (type)
        {
        case MovementType::Walk:
        case MovementType::Sprint: return Color { 82, 232, 118, 230 };
        case MovementType::StepUp:
        case MovementType::JumpUp:
        case MovementType::GapJump:
        case MovementType::DropDown: return Color { 80, 208, 255, 230 };
        case MovementType::SneakBridge:
        case MovementType::PlaceBlock: return Color { 255, 220, 72, 230 };
        case MovementType::BreakBlock: return Color { 255, 82, 82, 230 };
        case MovementType::Wait: return Color { 210, 210, 220, 210 };
        }
        return WHITE;
    };

    for (const auto& entry : botNavigationControllers_)
    {
        const NavigationDebugSnapshot& debug = entry.second.DebugSnapshot();
        const NavigationPath& path = debug.path;
        if (path.movements.empty())
        {
            continue;
        }
        const std::size_t begin = std::min(debug.movementIndex, path.movements.size());
        for (std::size_t index = begin; index < path.movements.size(); ++index)
        {
            const PlannedMovement& movement = path.movements[index];
            const Vector3 from = world_.GridToWorld(movement.from);
            const Vector3 to = world_.GridToWorld(movement.to);
            const Vector3 raisedFrom { from.x, from.y + 1.13f, from.z };
            const Vector3 raisedTo { to.x, to.y + 1.13f, to.z };
            const Color color = debug.pathStale
                ? Color { 148, 148, 158, 190 }
                : movementColor(movement.type);
            DrawLine3D(raisedFrom, raisedTo, color);
            DrawSphere(raisedTo, index == begin ? 0.12f : 0.07f, color);
            if (movement.affectedBlock.has_value())
            {
                const Vector3 block = world_.GridToWorld(*movement.affectedBlock);
                DrawCubeWires(block, 1.04f, 1.04f, 1.04f, color);
            }
        }
        const Vector3 goal = world_.GridToWorld(path.resolvedGoal);
        DrawSphere(Vector3 { goal.x, goal.y + 1.20f, goal.z }, 0.16f, WHITE);
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

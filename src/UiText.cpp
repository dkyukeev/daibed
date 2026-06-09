#include "UiText.h"

#include <vector>

namespace
{
std::vector<int> BuildCodepoints()
{
    std::vector<int> codepoints;
    codepoints.reserve(128 + 0x130);
    for (int codepoint = 32; codepoint <= 126; ++codepoint)
    {
        codepoints.push_back(codepoint);
    }
    for (int codepoint = 0x0400; codepoint <= 0x052F; ++codepoint)
    {
        codepoints.push_back(codepoint);
    }

    const int punctuation[] {
        0x00A0,
        0x00AB,
        0x00BB,
        0x2013,
        0x2014,
        0x2018,
        0x2019,
        0x201C,
        0x201D,
        0x2026
    };
    for (int codepoint : punctuation)
    {
        codepoints.push_back(codepoint);
    }
    return codepoints;
}

Font& UiFont()
{
    static Font font {};
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        const char* candidates[] {
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/calibri.ttf",
            "C:/Windows/Fonts/tahoma.ttf"
        };
        std::vector<int> codepoints = BuildCodepoints();
        for (const char* path : candidates)
        {
            if (!FileExists(path))
            {
                continue;
            }
            font = LoadFontEx(path, 48, codepoints.data(), static_cast<int>(codepoints.size()));
            if (font.texture.id != 0)
            {
                SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
                return font;
            }
        }
        font = GetFontDefault();
    }
    return font;
}
}

void DrawTextUtf8(const char* text, int posX, int posY, int fontSize, Color color)
{
    DrawTextEx(UiFont(), text, Vector2 { static_cast<float>(posX), static_cast<float>(posY) }, static_cast<float>(fontSize), 1.0f, color);
}

int MeasureTextUtf8(const char* text, int fontSize)
{
    return static_cast<int>(MeasureTextEx(UiFont(), text, static_cast<float>(fontSize), 1.0f).x + 0.5f);
}

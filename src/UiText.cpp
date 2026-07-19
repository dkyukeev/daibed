#include "UiText.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

// Pixel UI text. The whole game draws text through DrawTextUtf8/MeasureTextUtf8
// (every TU defines DrawText -> DrawTextUtf8), so the font choice here IS the
// game's typography. The UI font is Monocraft (Minecraft-style pixel font, SIL
// OFL, full Cyrillic): a pixel design only looks crisp when its design pixels
// map to whole screen pixels, so sizes are quantized to the font's 8px grid and
// every quantized size gets its OWN point-filtered atlas — glyphs are never
// scaled at draw time. Falls back to a system TTF when the asset is missing.

namespace
{
constexpr const char* kPixelFontPath = "assets/fonts/Monocraft.ttf";

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

// Snap to the pixel grid: Monocraft's glyphs live on an 8px em grid, so only
// multiples of 8 rasterize without half-pixels. 16 is the readability floor.
int QuantizePixelSize(int fontSize)
{
    const int quantized = ((fontSize + 3) / 8) * 8;
    return std::clamp(quantized, 16, 64);
}

struct UiFontEntry
{
    Font font {};
    bool pixel = false;
};

// The TTF is read from disk once; every per-size atlas rasterizes from the
// same memory blob (7 atlases = 7 disk reads otherwise).
const unsigned char* PixelFontData(int& size)
{
    static unsigned char* data = nullptr;
    static int dataSize = 0;
    static bool attempted = false;
    if (!attempted)
    {
        attempted = true;
        if (FileExists(kPixelFontPath))
        {
            data = LoadFileData(kPixelFontPath, &dataSize);
        }
    }
    size = dataSize;
    return data;
}

UiFontEntry LoadUiFontAtSize(int size)
{
    static const std::vector<int> codepoints = BuildCodepoints();
    UiFontEntry entry;
    int fontDataSize = 0;
    const unsigned char* fontData = PixelFontData(fontDataSize);
    if (fontData != nullptr)
    {
        entry.font = LoadFontFromMemory(".ttf", fontData, fontDataSize, size,
                                        const_cast<int*>(codepoints.data()),
                                        static_cast<int>(codepoints.size()));
        if (entry.font.texture.id != 0)
        {
            SetTextureFilter(entry.font.texture, TEXTURE_FILTER_POINT);
            entry.pixel = true;
            return entry;
        }
    }
    const char* candidates[] {
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/calibri.ttf",
        "C:/Windows/Fonts/tahoma.ttf"
    };
    for (const char* path : candidates)
    {
        if (!FileExists(path))
        {
            continue;
        }
        entry.font = LoadFontEx(path, size,
                                const_cast<int*>(codepoints.data()),
                                static_cast<int>(codepoints.size()));
        if (entry.font.texture.id != 0)
        {
            SetTextureFilter(entry.font.texture, TEXTURE_FILTER_BILINEAR);
            return entry;
        }
    }
    entry.font = GetFontDefault();
    return entry;
}

float SpacingForFont(const UiFontEntry& entry)
{
    // The pixel font carries its own mono advances; extra tracking would break
    // the grid. The smooth fallback keeps the old 1px tracking.
    return entry.pixel ? 0.0f : 1.0f;
}

std::unordered_map<int, UiFontEntry>& FontCache()
{
    static std::unordered_map<int, UiFontEntry> cache;
    return cache;
}

const UiFontEntry& EntryForQuantized(int quantized)
{
    auto& cache = FontCache();
    auto it = cache.find(quantized);
    if (it == cache.end())
    {
        it = cache.emplace(quantized, LoadUiFontAtSize(quantized)).first;
    }
    return it->second;
}

// Returns the atlas entry for the requested size. For the pixel font, drawSize
// is adjusted to the atlas's own size (crisp 1:1 pixels); the fallback TTF
// keeps the requested size and scales smoothly like before.
const UiFontEntry& UiEntryForSize(int requestedSize, int& drawSize)
{
    const int quantized = QuantizePixelSize(requestedSize);
    const UiFontEntry& entry = EntryForQuantized(quantized);
    drawSize = entry.pixel ? quantized : requestedSize;
    return entry;
}
}

void PreloadUiFonts()
{
    for (int size = 16; size <= 64; size += 8)
    {
        EntryForQuantized(size);
    }
}

void DrawTextUtf8(const char* text, int posX, int posY, int fontSize, Color color)
{
    int drawSize = fontSize;
    const UiFontEntry& entry = UiEntryForSize(fontSize, drawSize);
    // Keep the visual box the caller laid out: center the (possibly smaller
    // quantized) glyphs inside the requested line height.
    const float y = static_cast<float>(posY) + static_cast<float>(fontSize - drawSize) * 0.5f;
    DrawTextEx(entry.font, text, Vector2 { static_cast<float>(posX), y },
               static_cast<float>(drawSize), SpacingForFont(entry), color);
}

int MeasureTextUtf8(const char* text, int fontSize)
{
    int drawSize = fontSize;
    const UiFontEntry& entry = UiEntryForSize(fontSize, drawSize);
    return static_cast<int>(
        MeasureTextEx(entry.font, text, static_cast<float>(drawSize), SpacingForFont(entry)).x + 0.5f);
}

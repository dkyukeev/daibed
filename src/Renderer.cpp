#include "Renderer.h"

#include "CombatSystem.h"
#include "HeroSystem.h"
#include "UiText.h"
#include "VisualTheme.h"
#include "VecConvert.h"
#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#define DrawText DrawTextUtf8
#define MeasureText MeasureTextUtf8

namespace
{
constexpr int kTextureSize = 32;
constexpr float kPi = 3.1415926535f;

unsigned char BlendChannel(unsigned char a, unsigned char b, float t)
{
    return static_cast<unsigned char>(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t);
}

Color MixColor(Color a, Color b, float t)
{
    return Color {
        BlendChannel(a.r, b.r, t),
        BlendChannel(a.g, b.g, t),
        BlendChannel(a.b, b.b, t),
        BlendChannel(a.a, b.a, t)
    };
}

float DistanceSquared(Vector3 a, Vector3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

Color ColorForNoise(Color base, int x, int y, int spread)
{
    const int value = ((x * 37 + y * 61 + x * y * 17) % (spread * 2 + 1)) - spread;
    const auto clampChannel = [value](unsigned char channel)
    {
        return static_cast<unsigned char>(std::clamp(static_cast<int>(channel) + value, 0, 255));
    };
    return Color { clampChannel(base.r), clampChannel(base.g), clampChannel(base.b), base.a };
}

Image MakeTextureImage(Color base, Color accent, int variant)
{
    Image image = GenImageColor(kTextureSize, kTextureSize, base);
    for (int y = 0; y < kTextureSize; ++y)
    {
        for (int x = 0; x < kTextureSize; ++x)
        {
            Color pixel = ColorForNoise(base, x + variant * 3, y + variant * 5, 14);
            if ((x + y + variant) % 11 == 0)
            {
                pixel = MixColor(pixel, accent, 0.55f);
            }
            ImageDrawPixel(&image, x, y, pixel);
        }
    }
    return image;
}

Image MakeGrassImage()
{
    Image image = MakeTextureImage(Color { 88, 154, 74, 255 }, Color { 132, 196, 82, 255 }, 1);
    ImageDrawRectangle(&image, 0, 20, kTextureSize, 12, Color { 106, 76, 48, 255 });
    for (int y = 20; y < kTextureSize; ++y)
    {
        for (int x = 0; x < kTextureSize; ++x)
        {
            if ((x * 5 + y * 3) % 9 == 0)
            {
                ImageDrawPixel(&image, x, y, Color { 126, 88, 54, 255 });
            }
        }
    }
    return image;
}

Image MakeWoodImage()
{
    Image image = MakeTextureImage(Color { 145, 92, 48, 255 }, Color { 198, 134, 76, 255 }, 2);
    for (int x = 3; x < kTextureSize; x += 7)
    {
        ImageDrawRectangle(&image, x, 0, 2, kTextureSize, Color { 92, 54, 32, 255 });
    }
    return image;
}

Image MakeTeamChestImage()
{
    Image image = MakeTextureImage(Color { 128, 76, 38, 255 }, Color { 196, 126, 58, 255 }, 15);
    ImageDrawRectangle(&image, 0, 0, kTextureSize, 3, Color { 68, 42, 26, 255 });
    ImageDrawRectangle(&image, 0, kTextureSize - 3, kTextureSize, 3, Color { 68, 42, 26, 255 });
    ImageDrawRectangle(&image, 0, 0, 3, kTextureSize, Color { 68, 42, 26, 255 });
    ImageDrawRectangle(&image, kTextureSize - 3, 0, 3, kTextureSize, Color { 68, 42, 26, 255 });
    ImageDrawRectangle(&image, 0, 14, kTextureSize, 3, Color { 60, 64, 72, 255 });
    ImageDrawRectangle(&image, 14, 0, 4, kTextureSize, Color { 60, 64, 72, 255 });
    ImageDrawRectangle(&image, 11, 12, 10, 10, Color { 220, 176, 76, 255 });
    ImageDrawRectangle(&image, 13, 14, 6, 6, Color { 255, 224, 122, 255 });
    ImageDrawRectangle(&image, 15, 17, 2, 4, Color { 58, 42, 32, 255 });
    return image;
}

Image MakeWoolImage()
{
    Image image = MakeTextureImage(Color { 216, 222, 232, 255 }, Color { 170, 184, 204, 255 }, 3);
    for (int y = 0; y < kTextureSize; y += 8)
    {
        ImageDrawRectangle(&image, 0, y, kTextureSize, 1, Color { 188, 198, 214, 255 });
    }
    return image;
}

Image MakeTntImage()
{
    Image image = MakeTextureImage(Color { 190, 46, 42, 255 }, Color { 246, 196, 74, 255 }, 4);
    ImageDrawRectangle(&image, 0, 12, kTextureSize, 8, Color { 238, 232, 206, 255 });
    ImageDrawRectangle(&image, 5, 14, 4, 4, Color { 40, 34, 32, 255 });
    ImageDrawRectangle(&image, 14, 14, 4, 4, Color { 40, 34, 32, 255 });
    ImageDrawRectangle(&image, 23, 14, 4, 4, Color { 40, 34, 32, 255 });
    return image;
}

Image MakeSmoothStoneImage()
{
    Image image = MakeTextureImage(Color { 176, 182, 190, 255 }, Color { 216, 220, 226, 255 }, 16);
    const Color seam { 118, 124, 136, 255 };
    const Color highlight { 222, 226, 232, 255 };
    ImageDrawRectangle(&image, 0, 15, kTextureSize, 2, seam);
    ImageDrawRectangle(&image, 15, 0, 2, 16, seam);
    ImageDrawRectangle(&image, 23, 16, 2, 16, seam);
    ImageDrawRectangle(&image, 0, 0, kTextureSize, 1, highlight);
    ImageDrawRectangle(&image, 0, 0, 1, kTextureSize, highlight);
    ImageDrawRectangle(&image, 0, kTextureSize - 1, kTextureSize, 1, Color { 104, 110, 122, 255 });
    ImageDrawRectangle(&image, kTextureSize - 1, 0, 1, kTextureSize, Color { 104, 110, 122, 255 });
    for (int y = 3; y < kTextureSize; y += 11)
    {
        for (int x = 2; x < kTextureSize; x += 9)
        {
            if ((x + y) % 3 == 0)
            {
                ImageDrawPixel(&image, x, y, Color { 146, 152, 164, 255 });
            }
        }
    }
    return image;
}

Image MakeBrickImage(Color mortar, Color brickA, Color brickB, int variant)
{
    Image image = GenImageColor(kTextureSize, kTextureSize, mortar);
    constexpr int brickW = 15;
    constexpr int brickH = 8;
    for (int y = 0; y < kTextureSize; y += brickH)
    {
        const int offset = ((y / brickH) % 2) * (brickW / 2);
        for (int x = -offset; x < kTextureSize; x += brickW)
        {
            Color base = ((x + y + variant) % 2 == 0) ? brickA : brickB;
            const int left = std::max(0, x + 1);
            const int top = y + 1;
            const int right = std::min(kTextureSize, x + brickW - 1);
            const int bottom = std::min(kTextureSize, y + brickH - 1);
            for (int py = top; py < bottom; ++py)
            {
                for (int px = left; px < right; ++px)
                {
                    Color pixel = ColorForNoise(base, px + variant * 7, py + variant * 11, 10);
                    if (py == top)
                    {
                        pixel = MixColor(pixel, WHITE, 0.12f);
                    }
                    if (py == bottom - 1)
                    {
                        pixel = MixColor(pixel, BLACK, 0.14f);
                    }
                    ImageDrawPixel(&image, px, py, pixel);
                }
            }
        }
    }
    return image;
}

Image MakeMetalBlockImage()
{
    Image image = GenImageColor(kTextureSize, kTextureSize, Color { 94, 104, 114, 255 });
    for (int y = 0; y < kTextureSize; ++y)
    {
        const Color band = (y / 4) % 2 == 0 ? Color { 116, 128, 138, 255 } : Color { 82, 92, 104, 255 };
        for (int x = 0; x < kTextureSize; ++x)
        {
            Color pixel = ColorForNoise(band, x * 2, y + 19, 7);
            if ((x + y * 3) % 13 == 0)
            {
                pixel = MixColor(pixel, WHITE, 0.22f);
            }
            ImageDrawPixel(&image, x, y, pixel);
        }
    }
    for (int y = 7; y < kTextureSize; y += 8)
    {
        ImageDrawRectangle(&image, 0, y, kTextureSize, 2, Color { 48, 56, 66, 255 });
    }
    const int rivets[][2] { { 5, 5 }, { 26, 5 }, { 5, 26 }, { 26, 26 }, { 16, 16 } };
    for (const auto& rivet : rivets)
    {
        ImageDrawRectangle(&image, rivet[0] - 2, rivet[1] - 2, 4, 4, Color { 44, 50, 58, 255 });
        ImageDrawRectangle(&image, rivet[0] - 1, rivet[1] - 1, 2, 2, Color { 176, 186, 196, 255 });
    }
    ImageDrawRectangle(&image, 0, 0, kTextureSize, 1, Color { 196, 206, 214, 255 });
    ImageDrawRectangle(&image, 0, 0, 1, kTextureSize, Color { 196, 206, 214, 255 });
    ImageDrawRectangle(&image, 0, kTextureSize - 1, kTextureSize, 1, Color { 38, 44, 52, 255 });
    ImageDrawRectangle(&image, kTextureSize - 1, 0, 1, kTextureSize, Color { 38, 44, 52, 255 });
    return image;
}

Image MakeGlowBlockImage()
{
    Image image = GenImageColor(kTextureSize, kTextureSize, Color { 78, 58, 24, 255 });
    const Vector2 center { 15.5f, 15.5f };
    for (int y = 0; y < kTextureSize; ++y)
    {
        for (int x = 0; x < kTextureSize; ++x)
        {
            const float dx = static_cast<float>(x) - center.x;
            const float dy = static_cast<float>(y) - center.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float t = std::clamp(1.0f - distance / 18.0f, 0.0f, 1.0f);
            Color pixel = MixColor(Color { 168, 104, 28, 255 }, Color { 255, 246, 150, 255 }, t);
            if ((x / 4 + y / 4) % 2 == 0)
            {
                pixel = MixColor(pixel, Color { 255, 214, 78, 255 }, 0.18f);
            }
            ImageDrawPixel(&image, x, y, pixel);
        }
    }
    ImageDrawRectangle(&image, 0, 0, kTextureSize, 3, Color { 72, 50, 22, 255 });
    ImageDrawRectangle(&image, 0, kTextureSize - 3, kTextureSize, 3, Color { 72, 50, 22, 255 });
    ImageDrawRectangle(&image, 0, 0, 3, kTextureSize, Color { 72, 50, 22, 255 });
    ImageDrawRectangle(&image, kTextureSize - 3, 0, 3, kTextureSize, Color { 72, 50, 22, 255 });
    ImageDrawRectangle(&image, 14, 4, 4, 24, Color { 255, 250, 176, 255 });
    ImageDrawRectangle(&image, 4, 14, 24, 4, Color { 255, 250, 176, 255 });
    ImageDrawRectangle(&image, 12, 12, 8, 8, Color { 255, 255, 220, 255 });
    return image;
}

Image MakePlankVariantImage()
{
    Image image = GenImageColor(kTextureSize, kTextureSize, Color { 154, 94, 48, 255 });
    for (int y = 0; y < kTextureSize; ++y)
    {
        const Color plank = (y / 8) % 2 == 0 ? Color { 178, 112, 58, 255 } : Color { 132, 78, 42, 255 };
        for (int x = 0; x < kTextureSize; ++x)
        {
            Color pixel = ColorForNoise(plank, x + 23, y * 3, 12);
            if ((x * 5 + y) % 17 == 0)
            {
                pixel = MixColor(pixel, Color { 82, 48, 28, 255 }, 0.34f);
            }
            ImageDrawPixel(&image, x, y, pixel);
        }
    }
    for (int y = 7; y < kTextureSize; y += 8)
    {
        ImageDrawRectangle(&image, 0, y, kTextureSize, 2, Color { 76, 44, 26, 255 });
    }
    for (int y = 0; y < kTextureSize; y += 8)
    {
        const int seamX = (y / 8) % 2 == 0 ? 16 : 8;
        ImageDrawRectangle(&image, seamX, y + 1, 2, 6, Color { 86, 50, 30, 255 });
        ImageDrawRectangle(&image, 4, y + 3, 2, 2, Color { 58, 36, 24, 255 });
        ImageDrawRectangle(&image, 26, y + 3, 2, 2, Color { 58, 36, 24, 255 });
    }
    return image;
}

Image MakeDecorativeTileImage()
{
    Image image = GenImageColor(kTextureSize, kTextureSize, Color { 30, 86, 94, 255 });
    constexpr int tile = 8;
    for (int y = 0; y < kTextureSize; ++y)
    {
        for (int x = 0; x < kTextureSize; ++x)
        {
            if (x % tile == 0 || y % tile == 0)
            {
                ImageDrawPixel(&image, x, y, Color { 216, 222, 210, 255 });
                continue;
            }
            const bool alt = ((x / tile) + (y / tile)) % 2 == 0;
            Color base = alt ? Color { 50, 164, 178, 255 } : Color { 42, 120, 156, 255 };
            const int cx = (x / tile) * tile + tile / 2;
            const int cy = (y / tile) * tile + tile / 2;
            if (std::abs(x - cx) + std::abs(y - cy) <= 3)
            {
                base = MixColor(base, Color { 234, 220, 128, 255 }, 0.48f);
            }
            ImageDrawPixel(&image, x, y, ColorForNoise(base, x, y, 5));
        }
    }
    return image;
}

Image MakeTrimBlockImage()
{
    Image image = MakeTextureImage(Color { 58, 54, 60, 255 }, Color { 98, 88, 72, 255 }, 17);
    const Color trim { 218, 160, 72, 255 };
    const Color trimLight { 250, 210, 116, 255 };
    const Color shadow { 36, 32, 38, 255 };
    ImageDrawRectangle(&image, 0, 0, kTextureSize, 4, trim);
    ImageDrawRectangle(&image, 0, kTextureSize - 4, kTextureSize, 4, trim);
    ImageDrawRectangle(&image, 0, 0, 4, kTextureSize, trim);
    ImageDrawRectangle(&image, kTextureSize - 4, 0, 4, kTextureSize, trim);
    ImageDrawRectangle(&image, 5, 5, 22, 22, shadow);
    ImageDrawRectangle(&image, 7, 7, 18, 18, Color { 72, 66, 72, 255 });
    for (int i = 0; i < kTextureSize; ++i)
    {
        ImageDrawPixel(&image, i, i, trimLight);
        ImageDrawPixel(&image, kTextureSize - 1 - i, i, trimLight);
    }
    ImageDrawRectangle(&image, 13, 13, 6, 6, trimLight);
    return image;
}

Image MakeCobblestoneImage()
{
    const Color mortar { 66, 70, 76, 255 };
    Image image = GenImageColor(kTextureSize, kTextureSize, mortar);
    constexpr int rows[] { 0, 7, 16, 24 };
    constexpr int widths[] { 11, 13, 10, 14 };
    for (int row = 0; row < 4; ++row)
    {
        const int y = rows[row] + 1;
        const int h = (row == 1 || row == 3) ? 7 : 8;
        const int offset = (row % 2 == 0) ? -4 : 2;
        for (int x = offset; x < kTextureSize; x += widths[row])
        {
            const int right = std::min(kTextureSize, x + widths[row] - 1);
            const int left = std::max(0, x + 1);
            if (right <= left || y >= kTextureSize)
            {
                continue;
            }
            const Color stone = ColorForNoise(
                (row + x / std::max(1, widths[row])) % 2 == 0 ? Color { 128, 133, 138, 255 } : Color { 108, 114, 122, 255 },
                x, y, 8);
            ImageDrawRectangle(&image, left, y, right - left, std::min(h, kTextureSize - y), stone);
            ImageDrawRectangle(&image, left, y, right - left, 1, MixColor(stone, WHITE, 0.16f));
            ImageDrawRectangle(&image, left, y + h - 1, right - left, 1, MixColor(stone, BLACK, 0.22f));
        }
    }
    return image;
}

Image MakeAndesiteImage(bool polished)
{
    const Color base = polished ? Color { 126, 132, 140, 255 } : Color { 112, 118, 126, 255 };
    const Color accent = polished ? Color { 168, 174, 182, 255 } : Color { 158, 162, 168, 255 };
    Image image = MakeTextureImage(base, accent, polished ? 34 : 33);
    if (polished)
    {
        ImageDrawRectangle(&image, 0, 15, kTextureSize, 2, Color { 76, 82, 90, 255 });
        ImageDrawRectangle(&image, 15, 0, 2, kTextureSize, Color { 76, 82, 90, 255 });
        ImageDrawRectangle(&image, 0, 0, kTextureSize, 1, Color { 188, 192, 198, 255 });
        ImageDrawRectangle(&image, 0, 0, 1, kTextureSize, Color { 188, 192, 198, 255 });
    }
    else
    {
        for (int y = 3; y < kTextureSize; y += 7)
        {
            for (int x = (y % 3) + 2; x < kTextureSize; x += 9)
            {
                ImageDrawRectangle(&image, x, y, 2, 2, Color { 76, 82, 91, 255 });
            }
        }
    }
    return image;
}

Image MakeChiseledStoneImage()
{
    Image image = MakeTextureImage(Color { 132, 137, 142, 255 }, Color { 172, 176, 180, 255 }, 35);
    const Color groove { 68, 72, 78, 255 };
    ImageDrawRectangle(&image, 2, 2, 28, 28, Color { 150, 155, 160, 255 });
    ImageDrawRectangle(&image, 4, 4, 24, 24, groove);
    ImageDrawRectangle(&image, 6, 6, 20, 20, Color { 126, 132, 138, 255 });
    ImageDrawRectangle(&image, 12, 7, 8, 18, Color { 156, 161, 166, 255 });
    ImageDrawRectangle(&image, 7, 12, 18, 8, Color { 156, 161, 166, 255 });
    ImageDrawRectangle(&image, 14, 9, 4, 14, Color { 96, 101, 108, 255 });
    ImageDrawRectangle(&image, 9, 14, 14, 4, Color { 96, 101, 108, 255 });
    return image;
}

Image MakeBirchPlankImage()
{
    Image image = GenImageColor(kTextureSize, kTextureSize, Color { 210, 188, 132, 255 });
    for (int y = 0; y < kTextureSize; ++y)
    {
        const Color plank = (y / 8) % 2 == 0 ? Color { 222, 202, 148, 255 } : Color { 194, 168, 112, 255 };
        for (int x = 0; x < kTextureSize; ++x)
        {
            Color pixel = ColorForNoise(plank, x + 41, y * 2, 8);
            if ((x * 3 + y * 5) % 29 == 0)
            {
                pixel = MixColor(pixel, Color { 104, 82, 52, 255 }, 0.42f);
            }
            ImageDrawPixel(&image, x, y, pixel);
        }
    }
    for (int y = 7; y < kTextureSize; y += 8)
    {
        ImageDrawRectangle(&image, 0, y, kTextureSize, 2, Color { 104, 82, 52, 255 });
    }
    return image;
}

Image MakeColoredGlassImage()
{
    Image image = GenImageColor(kTextureSize, kTextureSize, Color { 224, 244, 255, 154 });
    for (int i = -8; i < kTextureSize; i += 10)
    {
        for (int t = 0; t < kTextureSize; ++t)
        {
            const int x = i + t;
            if (x >= 0 && x < kTextureSize)
            {
                ImageDrawPixel(&image, x, t, Color { 255, 255, 255, 218 });
            }
        }
    }
    ImageDrawRectangle(&image, 0, 0, kTextureSize, 2, Color { 244, 254, 255, 218 });
    ImageDrawRectangle(&image, 0, 0, 2, kTextureSize, Color { 244, 254, 255, 218 });
    ImageDrawRectangle(&image, 0, kTextureSize - 2, kTextureSize, 2, Color { 86, 124, 154, 166 });
    ImageDrawRectangle(&image, kTextureSize - 2, 0, 2, kTextureSize, Color { 86, 124, 154, 166 });
    return image;
}

Image MakeClayImage()
{
    Image image = GenImageColor(kTextureSize, kTextureSize, Color { 178, 154, 126, 255 });
    for (int y = 0; y < kTextureSize; ++y)
    {
        for (int x = 0; x < kTextureSize; ++x)
        {
            const int band = (x / 7 + y / 9) % 3;
            const Color base = band == 0 ? Color { 196, 170, 138, 255 }
                : (band == 1 ? Color { 158, 132, 108, 255 } : Color { 184, 150, 120, 255 });
            ImageDrawPixel(&image, x, y, ColorForNoise(base, x + 7, y + 19, 5));
        }
    }
    for (int y = 5; y < kTextureSize; y += 10)
    {
        ImageDrawRectangle(&image, 0, y, kTextureSize, 1, Color { 120, 100, 84, 255 });
    }
    return image;
}

Image MakeGemBlockImage(Color base, Color highlight, Color shadow, int variant)
{
    Image image = GenImageColor(kTextureSize, kTextureSize, shadow);
    for (int y = 1; y < kTextureSize; y += 10)
    {
        const int offset = ((y / 10) % 2) * 5;
        for (int x = 1 - offset; x < kTextureSize; x += 10)
        {
            // Offset rows intentionally begin outside the left edge.  Raylib's
            // image drawing helpers do not promise to clip negative pixels, so
            // clamp the primitive ourselves instead of corrupting image memory.
            const int left = std::max(0, x + 1);
            const int right = std::min(kTextureSize, x + 9);
            const int top = std::max(0, y + 1);
            const int bottom = std::min(kTextureSize, y + 9);
            if (left < right && top < bottom)
            {
                const int width = right - left;
                const int height = bottom - top;
                ImageDrawRectangle(&image, left, top, width, height, ColorForNoise(base, x + variant, y + variant * 2, 8));
                ImageDrawRectangle(&image, left, top, width, 1, highlight);
                ImageDrawRectangle(&image, left, bottom - 1, width, 1, shadow);
            }
            const int shineX = x + 3;
            const int shineY = y + 3;
            if (shineX >= 0 && shineX < kTextureSize && shineY >= 0 && shineY < kTextureSize)
            {
                ImageDrawPixel(&image, shineX, shineY, MixColor(highlight, WHITE, 0.28f));
            }
        }
    }
    return image;
}

Image MakeLapisImage()
{
    Image image = MakeGemBlockImage(Color { 28, 74, 178, 255 }, Color { 88, 146, 244, 255 }, Color { 12, 32, 92, 255 }, 40);
    for (int y = 4; y < kTextureSize; y += 9)
    {
        for (int x = (y % 5) + 2; x < kTextureSize; x += 11)
        {
            ImageDrawRectangle(&image, x, y, 2, 2, Color { 246, 196, 74, 255 });
        }
    }
    return image;
}

Image MakeGoldBlockImage()
{
    Image image = MakeGemBlockImage(Color { 226, 168, 42, 255 }, Color { 255, 232, 118, 255 }, Color { 126, 78, 18, 255 }, 44);
    for (int y = 7; y < kTextureSize; y += 10)
    {
        ImageDrawRectangle(&image, 0, y, kTextureSize, 1, Color { 112, 68, 16, 255 });
    }
    return image;
}

Image MakeIronBarsImage()
{
    Image image = MakeMetalBlockImage();
    ImageDrawRectangle(&image, 0, 14, kTextureSize, 4, Color { 48, 56, 66, 255 });
    ImageDrawRectangle(&image, 14, 0, 4, kTextureSize, Color { 48, 56, 66, 255 });
    return image;
}

Image MakeLadderImage()
{
    Image image = MakeTextureImage(Color { 126, 82, 44, 255 }, Color { 186, 130, 70, 255 }, 49);
    ImageDrawRectangle(&image, 3, 0, 4, kTextureSize, Color { 76, 46, 28, 255 });
    ImageDrawRectangle(&image, 25, 0, 4, kTextureSize, Color { 76, 46, 28, 255 });
    for (int y = 4; y < kTextureSize; y += 9)
    {
        ImageDrawRectangle(&image, 4, y, 24, 3, Color { 174, 112, 58, 255 });
    }
    return image;
}

Image MakeTorchBlockImage()
{
    Image image = GenImageColor(kTextureSize, kTextureSize, Color { 116, 70, 36, 255 });
    ImageDrawRectangle(&image, 12, 0, 8, kTextureSize, Color { 132, 80, 40, 255 });
    ImageDrawRectangle(&image, 13, 0, 3, kTextureSize, Color { 180, 112, 52, 255 });
    ImageDrawRectangle(&image, 8, 2, 16, 12, Color { 255, 164, 48, 255 });
    ImageDrawRectangle(&image, 11, 0, 10, 8, Color { 255, 232, 122, 255 });
    return image;
}

void DrawIconLine(Image* image, int x0, int y0, int x1, int y1, int thickness, Color color)
{
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int x = x0;
    int y = y0;

    while (true)
    {
        ImageDrawRectangle(image, x - thickness / 2, y - thickness / 2, thickness, thickness, color);
        if (x == x1 && y == y1)
        {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y += sy;
        }
    }
}

void DrawIconDiamond(Image* image, int cx, int cy, int radius, Color color)
{
    for (int y = -radius; y <= radius; ++y)
    {
        const int width = radius - std::abs(y);
        ImageDrawRectangle(image, cx - width, cy + y, width * 2 + 1, 1, color);
    }
}

Image MakeTransparentIcon()
{
    return GenImageColor(kTextureSize, kTextureSize, BLANK);
}

Image MakeSwordIcon()
{
    Image image = MakeTransparentIcon();
    DrawIconLine(&image, 9, 25, 24, 6, 5, Color { 92, 104, 118, 255 });
    DrawIconLine(&image, 10, 24, 24, 6, 3, Color { 236, 240, 248, 255 });
    DrawIconLine(&image, 8, 21, 13, 26, 3, Color { 246, 196, 74, 255 });
    DrawIconLine(&image, 5, 28, 10, 23, 4, Color { 98, 58, 32, 255 });
    return image;
}

Image MakePickaxeIcon()
{
    Image image = MakeTransparentIcon();
    DrawIconLine(&image, 10, 26, 19, 12, 4, Color { 118, 72, 42, 255 });
    DrawIconLine(&image, 8, 12, 25, 7, 4, Color { 174, 184, 196, 255 });
    DrawIconLine(&image, 20, 8, 27, 14, 3, Color { 128, 138, 150, 255 });
    DrawIconLine(&image, 8, 12, 5, 17, 3, Color { 214, 220, 230, 255 });
    return image;
}

Image MakeAxeIcon()
{
    Image image = MakeTransparentIcon();
    DrawIconLine(&image, 9, 27, 19, 10, 4, Color { 104, 64, 38, 255 });
    DrawIconLine(&image, 10, 26, 20, 10, 2, Color { 166, 104, 58, 255 });
    DrawIconLine(&image, 17, 9, 25, 7, 5, Color { 178, 188, 200, 255 });
    DrawIconLine(&image, 18, 12, 26, 15, 5, Color { 150, 160, 174, 255 });
    DrawIconLine(&image, 23, 7, 27, 11, 3, Color { 222, 228, 238, 255 });
    DrawIconLine(&image, 23, 15, 27, 12, 3, Color { 222, 228, 238, 255 });
    DrawIconLine(&image, 16, 15, 20, 10, 2, Color { 238, 240, 246, 255 });
    return image;
}

Image MakeSpearIcon()
{
    Image image = MakeTransparentIcon();
    DrawIconLine(&image, 7, 27, 24, 8, 3, Color { 122, 78, 48, 255 });
    DrawIconLine(&image, 20, 12, 26, 4, 4, Color { 180, 220, 255, 255 });
    DrawIconLine(&image, 22, 10, 27, 5, 2, WHITE);
    return image;
}

Image MakeArrowIcon()
{
    Image image = MakeTransparentIcon();
    DrawIconLine(&image, 6, 24, 24, 8, 3, Color { 112, 232, 255, 255 });
    DrawIconLine(&image, 19, 8, 25, 8, 3, Color { 220, 252, 255, 255 });
    DrawIconLine(&image, 24, 8, 24, 14, 3, Color { 220, 252, 255, 255 });
    return image;
}

Image MakeFireballIcon()
{
    Image image = MakeTransparentIcon();
    DrawIconDiamond(&image, 18, 16, 10, Color { 255, 118, 42, 255 });
    DrawIconDiamond(&image, 18, 16, 6, Color { 255, 210, 66, 255 });
    DrawIconLine(&image, 7, 21, 17, 12, 5, Color { 220, 46, 34, 230 });
    return image;
}

Image MakeMedKitIcon()
{
    Image image = MakeTransparentIcon();
    ImageDrawRectangle(&image, 7, 9, 18, 16, Color { 238, 242, 246, 255 });
    ImageDrawRectangle(&image, 12, 5, 8, 5, Color { 210, 216, 226, 255 });
    ImageDrawRectangle(&image, 14, 12, 4, 10, Color { 224, 42, 58, 255 });
    ImageDrawRectangle(&image, 11, 15, 10, 4, Color { 224, 42, 58, 255 });
    return image;
}

Image MakeHomeIcon()
{
    Image image = MakeTransparentIcon();
    DrawIconDiamond(&image, 16, 12, 9, Color { 180, 148, 255, 255 });
    ImageDrawRectangle(&image, 9, 14, 14, 12, Color { 82, 62, 132, 255 });
    ImageDrawRectangle(&image, 14, 18, 4, 8, Color { 230, 220, 255, 255 });
    return image;
}

Image MakeDashIcon()
{
    Image image = MakeTransparentIcon();
    DrawIconDiamond(&image, 16, 16, 10, Color { 125, 230, 255, 255 });
    DrawIconLine(&image, 8, 16, 22, 10, 3, WHITE);
    DrawIconLine(&image, 8, 22, 22, 16, 3, Color { 220, 252, 255, 255 });
    return image;
}

Image MakeMolotovIcon()
{
    Image image = MakeTransparentIcon();
    ImageDrawRectangle(&image, 13, 10, 7, 16, Color { 78, 122, 82, 255 });
    ImageDrawRectangle(&image, 14, 6, 5, 5, Color { 110, 82, 54, 255 });
    DrawIconDiamond(&image, 17, 7, 5, Color { 255, 180, 66, 255 });
    DrawIconDiamond(&image, 17, 6, 3, Color { 255, 72, 42, 255 });
    return image;
}

Image MakeAlarmIcon()
{
    Image image = MakeTransparentIcon();
    DrawIconDiamond(&image, 16, 16, 10, Color { 255, 235, 142, 255 });
    ImageDrawRectangle(&image, 14, 9, 4, 12, Color { 70, 52, 32, 255 });
    ImageDrawRectangle(&image, 14, 23, 4, 3, Color { 70, 52, 32, 255 });
    return image;
}

Image MakeResourceIcon(Color color, Color shine)
{
    Image image = MakeTransparentIcon();
    DrawIconDiamond(&image, 16, 16, 11, color);
    DrawIconDiamond(&image, 14, 13, 4, shine);
    ImageDrawRectangle(&image, 11, 23, 10, 2, Fade(BLACK, 0.22f));
    return image;
}

Texture2D LoadProceduralTexture(Image image)
{
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    return texture;
}

// World-block sampling: crisp pixel-art up close (nearest magnification) but
// mipmapped + anisotropic in the distance, which kills the texture shimmer
// on far islands without blurring nearby walls.
void ApplyWorldTextureSampling(Texture2D& texture)
{
    if (texture.id == 0)
    {
        return;
    }
    GenTextureMipmaps(&texture);
    if (texture.mipmaps <= 1)
    {
        // NPOT or context limitation: stay on plain point sampling rather
        // than pointing MIN_FILTER at mip levels that do not exist.
        return;
    }
    rlTextureParameters(texture.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_NEAREST_MIP_LINEAR);
    rlTextureParameters(texture.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_NEAREST);
    rlTextureParameters(texture.id, RL_TEXTURE_FILTER_ANISOTROPIC, 8);
}

std::string FindAssetFile(const std::string& relativePath)
{
    const std::string candidates[] {
        relativePath,
        "../" + relativePath,
        "../../" + relativePath
    };
    for (const std::string& candidate : candidates)
    {
        if (FileExists(candidate.c_str()))
        {
            return candidate;
        }
    }
    return {};
}

Texture2D LoadCustomizableTexture(const char* assetName, Image fallback)
{
    const std::string path = FindAssetFile(std::string("assets/items/") + assetName + ".png");
    if (!path.empty())
    {
        Texture2D texture = LoadTexture(path.c_str());
        UnloadImage(fallback);
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);
        return texture;
    }
    return LoadProceduralTexture(fallback);
}

// Bakes a tangent-space normal map from the albedo treated as a height
// field (dark mortar and grain lines read as recessed).  Alpha packs the
// material gloss mask so shiny blocks need no extra texture.  Edges wrap
// because block textures tile across faces.
Image MakeNormalMapImage(const Image& albedo, float bumpStrength, unsigned char materialAlpha)
{
    const int width = albedo.width;
    const int height = albedo.height;
    Image normalImage = GenImageColor(width, height, Color { 128, 128, 255, materialAlpha });
    if (width <= 0 || height <= 0)
    {
        return normalImage;
    }

    Color* pixels = LoadImageColors(albedo);
    if (pixels == nullptr)
    {
        return normalImage;
    }
    std::vector<float> heights(static_cast<std::size_t>(width) * height);
    for (int i = 0; i < width * height; ++i)
    {
        const Color& pixel = pixels[i];
        heights[i] = (0.2126f * pixel.r + 0.7152f * pixel.g + 0.0722f * pixel.b) / 255.0f;
    }
    UnloadImageColors(pixels);

    const auto heightAt = [&heights, width, height](int x, int y)
    {
        const int wrappedX = (x % width + width) % width;
        const int wrappedY = (y % height + height) % height;
        return heights[static_cast<std::size_t>(wrappedY) * width + wrappedX];
    };

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            // Sobel gradients; image +y runs along the bitangent, so the
            // packed normal matches the face basis built in lighting.fs.
            const float gradientX =
                (heightAt(x + 1, y - 1) + 2.0f * heightAt(x + 1, y) + heightAt(x + 1, y + 1))
                - (heightAt(x - 1, y - 1) + 2.0f * heightAt(x - 1, y) + heightAt(x - 1, y + 1));
            const float gradientY =
                (heightAt(x - 1, y + 1) + 2.0f * heightAt(x, y + 1) + heightAt(x + 1, y + 1))
                - (heightAt(x - 1, y - 1) + 2.0f * heightAt(x, y - 1) + heightAt(x + 1, y - 1));
            const float nx = -gradientX * bumpStrength;
            const float ny = -gradientY * bumpStrength;
            const float inverseLength = 1.0f / std::sqrt(nx * nx + ny * ny + 1.0f);
            const auto pack = [](float component)
            {
                return static_cast<unsigned char>(std::clamp(component * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            ImageDrawPixel(&normalImage, x, y, Color {
                pack(nx * inverseLength),
                pack(ny * inverseLength),
                pack(inverseLength),
                materialAlpha
            });
        }
    }
    return normalImage;
}

// Same override contract as LoadCustomizableTexture, but also derives the
// normal map from whichever albedo actually ends up on screen, so a PNG in
// assets/items/ keeps its relief consistent automatically.
Texture2D LoadCustomizableTextureWithNormal(
    const char* assetName,
    Image fallback,
    float bumpStrength,
    unsigned char materialAlpha,
    Texture2D& normalTexture)
{
    const std::string path = FindAssetFile(std::string("assets/items/") + assetName + ".png");
    Image albedo = fallback;
    if (!path.empty())
    {
        Image loaded = LoadImage(path.c_str());
        if (loaded.data != nullptr)
        {
            UnloadImage(fallback);
            albedo = loaded;
        }
    }

    Image normalImage = MakeNormalMapImage(albedo, bumpStrength, materialAlpha);
    normalTexture = LoadTextureFromImage(normalImage);
    UnloadImage(normalImage);
    if (normalTexture.id != 0)
    {
        SetTextureFilter(normalTexture, TEXTURE_FILTER_POINT);
    }
    return LoadProceduralTexture(albedo);
}

void DrawDepthTestedBillboard(Camera3D camera, Texture2D texture, Vector3 position, float size, Color tint)
{
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
    rlDisableDepthMask();
    DrawBillboard(camera, texture, position, size, tint);
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
}

bool RayIntersectsAabb(Vector3 origin, Vector3 direction, float maxDistance, Vector3 center, Vector3 halfExtents)
{
    float tMin = 0.0f;
    float tMax = maxDistance;
    const float originValues[3] { origin.x, origin.y, origin.z };
    const float directionValues[3] { direction.x, direction.y, direction.z };
    const float minValues[3] {
        center.x - halfExtents.x,
        center.y - halfExtents.y,
        center.z - halfExtents.z
    };
    const float maxValues[3] {
        center.x + halfExtents.x,
        center.y + halfExtents.y,
        center.z + halfExtents.z
    };

    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::fabs(directionValues[axis]) < 0.0001f)
        {
            if (originValues[axis] < minValues[axis] || originValues[axis] > maxValues[axis])
            {
                return false;
            }
            continue;
        }

        float t1 = (minValues[axis] - originValues[axis]) / directionValues[axis];
        float t2 = (maxValues[axis] - originValues[axis]) / directionValues[axis];
        if (t1 > t2)
        {
            std::swap(t1, t2);
        }
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax)
        {
            return false;
        }
    }

    return tMin > 0.05f && tMin < maxDistance - 0.12f;
}

void DrawTexturedCube(Texture2D texture, Vector3 position, float width, float height, float length, Color tint)
{
    const float x = position.x;
    const float y = position.y;
    const float z = position.z;
    const float w = width * 0.5f;
    const float h = height * 0.5f;
    const float l = length * 0.5f;

    rlSetTexture(texture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(tint.r, tint.g, tint.b, tint.a);

    rlNormal3f(0.0f, 0.0f, 1.0f);
    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(x - w, y - h, z + l);
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(x + w, y - h, z + l);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(x + w, y + h, z + l);
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(x - w, y + h, z + l);

    rlNormal3f(0.0f, 0.0f, -1.0f);
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(x - w, y - h, z - l);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(x - w, y + h, z - l);
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(x + w, y + h, z - l);
    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(x + w, y - h, z - l);

    rlNormal3f(1.0f, 0.0f, 0.0f);
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(x + w, y - h, z - l);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(x + w, y + h, z - l);
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(x + w, y + h, z + l);
    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(x + w, y - h, z + l);

    rlNormal3f(-1.0f, 0.0f, 0.0f);
    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(x - w, y - h, z - l);
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(x - w, y - h, z + l);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(x - w, y + h, z + l);
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(x - w, y + h, z - l);

    rlNormal3f(0.0f, 1.0f, 0.0f);
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(x - w, y + h, z - l);
    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(x - w, y + h, z + l);
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(x + w, y + h, z + l);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(x + w, y + h, z - l);

    rlNormal3f(0.0f, -1.0f, 0.0f);
    rlTexCoord2f(1.0f, 1.0f); rlVertex3f(x - w, y - h, z - l);
    rlTexCoord2f(0.0f, 1.0f); rlVertex3f(x + w, y - h, z - l);
    rlTexCoord2f(0.0f, 0.0f); rlVertex3f(x + w, y - h, z + l);
    rlTexCoord2f(1.0f, 0.0f); rlVertex3f(x - w, y - h, z + l);

    rlEnd();
    rlSetTexture(0);
}

void DrawFogWall(float minX, float maxX, float minZ, float maxZ, float minY, float maxY, Color color, unsigned char alpha)
{
    const Color bottom = Fade(color, static_cast<float>(alpha) / 255.0f);
    const Color top = Fade(color, static_cast<float>(alpha) / 510.0f);

    rlSetTexture(0);
    rlBegin(RL_QUADS);

    rlColor4ub(bottom.r, bottom.g, bottom.b, bottom.a);
    rlVertex3f(minX, minY, maxZ);
    rlVertex3f(maxX, minY, maxZ);
    rlColor4ub(top.r, top.g, top.b, top.a);
    rlVertex3f(maxX, maxY, maxZ);
    rlVertex3f(minX, maxY, maxZ);

    rlColor4ub(bottom.r, bottom.g, bottom.b, bottom.a);
    rlVertex3f(maxX, minY, minZ);
    rlVertex3f(minX, minY, minZ);
    rlColor4ub(top.r, top.g, top.b, top.a);
    rlVertex3f(minX, maxY, minZ);
    rlVertex3f(maxX, maxY, minZ);

    rlColor4ub(bottom.r, bottom.g, bottom.b, bottom.a);
    rlVertex3f(maxX, minY, maxZ);
    rlVertex3f(maxX, minY, minZ);
    rlColor4ub(top.r, top.g, top.b, top.a);
    rlVertex3f(maxX, maxY, minZ);
    rlVertex3f(maxX, maxY, maxZ);

    rlColor4ub(bottom.r, bottom.g, bottom.b, bottom.a);
    rlVertex3f(minX, minY, minZ);
    rlVertex3f(minX, minY, maxZ);
    rlColor4ub(top.r, top.g, top.b, top.a);
    rlVertex3f(minX, maxY, maxZ);
    rlVertex3f(minX, maxY, minZ);

    rlEnd();
}

void DrawDistantFog(Color skyColor, Vector3 boundsMin, Vector3 boundsMax, bool boundsValid)
{
    // The fog shell hugs the actual map: on the stock arena these come out
    // close to the legacy hard-coded walls, while a large imported map (the
    // castle spans over +-100) pushes them outward instead of being sliced
    // by translucent quads through its middle.
    if (!boundsValid)
    {
        boundsMin = Vector3 { -50.0f, -4.0f, -50.0f };
        boundsMax = Vector3 { 50.0f, 30.0f, 50.0f };
    }

    const Color fog = MixColor(skyColor, WHITE, 0.16f);
    const unsigned char alphas[] { 34, 52, 74 };
    for (int tier = 0; tier < 3; ++tier)
    {
        const float margin = 20.0f + 13.0f * static_cast<float>(tier);
        DrawFogWall(
            boundsMin.x - margin,
            boundsMax.x + margin,
            boundsMin.z - margin,
            boundsMax.z + margin,
            boundsMin.y - 36.0f - 6.0f * static_cast<float>(tier),
            boundsMax.y + 5.0f + 4.0f * static_cast<float>(tier),
            fog,
            alphas[tier]);
    }

    const float centerX = (boundsMin.x + boundsMax.x) * 0.5f;
    const float centerZ = (boundsMin.z + boundsMax.z) * 0.5f;
    const float spanX = boundsMax.x - boundsMin.x;
    const float spanZ = boundsMax.z - boundsMin.z;
    const float planeSpan = std::max(spanX, spanZ);
    DrawPlane(Vector3 { centerX, boundsMin.y - 14.0f, centerZ }, Vector2 { planeSpan + 60.0f, planeSpan + 60.0f }, Fade(fog, 0.08f));
    DrawPlane(Vector3 { centerX, boundsMin.y - 22.0f, centerZ }, Vector2 { planeSpan + 115.0f, planeSpan + 115.0f }, Fade(fog, 0.10f));
}

void DrawSkyVoid(Color skyColor, float cameraY)
{
    const Color upperVoid = MixColor(skyColor, WHITE, 0.08f);
    const Color deepVoid = MixColor(skyColor, Color { 3, 5, 16, 255 }, 0.72f);
    const float fallDepth = std::clamp((4.0f - cameraY) / 34.0f, 0.0f, 1.0f);

    DrawPlane(Vector3 { 0.0f, -44.0f, 0.0f }, Vector2 { 620.0f, 620.0f }, Fade(upperVoid, 0.92f));
    DrawPlane(Vector3 { 0.0f, -60.0f, 0.0f }, Vector2 { 520.0f, 520.0f }, Fade(MixColor(upperVoid, deepVoid, fallDepth), 0.18f + fallDepth * 0.50f));
    DrawPlane(Vector3 { 0.0f, -76.0f, 0.0f }, Vector2 { 380.0f, 380.0f }, Fade(deepVoid, fallDepth * 0.42f));
}

void DrawBar(Rectangle bounds, float fraction, Color fill, Color back, Color border)
{
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    DrawRectangleRec(bounds, back);
    DrawRectangleRec(Rectangle { bounds.x, bounds.y, bounds.width * clamped, bounds.height }, fill);
    DrawRectangleLinesEx(bounds, 1.0f, border);
}

std::string CostText(const ShopItem& item)
{
    std::string text = std::to_string(item.primaryCost) + " " + ToString(item.primaryType);
    if (item.hasSecondaryCost)
    {
        text += " + " + std::to_string(item.secondaryCost) + " " + ToString(item.secondaryType);
    }

    return text;
}

std::string LevelText(const ShopItem& item, const Inventory& inventory, const Team* team)
{
    switch (item.choice)
    {
    case 101:
        return "Ур " + std::to_string(inventory.GetSwordLevel()) + "/" + std::to_string(item.maxLevel);
    case 102:
        return "Ур " + std::to_string(inventory.GetToolLevel()) + "/" + std::to_string(item.maxLevel);
    case 103:
        return "Ур " + std::to_string(inventory.GetArmorLevel()) + "/" + std::to_string(item.maxLevel);
    case 109:
        return "Ур " + std::to_string(inventory.GetBowUpgradeLevel()) + "/" + std::to_string(item.maxLevel);
    case 402:
        return "Ур " + std::to_string(inventory.GetBlasterRapidFireLevel()) + "/" + std::to_string(item.maxLevel);
    case 403:
        return "Ур " + std::to_string(inventory.GetBlasterDamageLevel()) + "/" + std::to_string(item.maxLevel);
    case 301:
        return "Ур " + std::to_string(team != nullptr ? team->forgeLevel : 0) + "/" + std::to_string(item.maxLevel);
    case 302:
        return "Ур " + std::to_string(team != nullptr ? team->healAuraLevel : 0) + "/" + std::to_string(item.maxLevel);
    case 304:
        return "Ур " + std::to_string(team != nullptr && team->enemyTrackerUnlocked ? 1 : 0) + "/" + std::to_string(item.maxLevel);
    default:
        break;
    }

    return "";
}

bool IsAtMax(const ShopItem& item, const Inventory& inventory, const Team* team)
{
    if (item.maxLevel <= 0)
    {
        return false;
    }

    switch (item.choice)
    {
    case 101:
        return inventory.GetSwordLevel() >= item.maxLevel;
    case 102:
        return inventory.GetToolLevel() >= item.maxLevel;
    case 103:
        return inventory.GetArmorLevel() >= item.maxLevel;
    case 109:
        return inventory.GetBowUpgradeLevel() >= item.maxLevel;
    case 402:
        return inventory.GetBlasterDamageLevel() > 0 || inventory.GetBlasterRapidFireLevel() >= item.maxLevel;
    case 403:
        return inventory.GetBlasterRapidFireLevel() > 0 || inventory.GetBlasterDamageLevel() >= item.maxLevel;
    case 301:
        return team != nullptr && team->forgeLevel >= item.maxLevel;
    case 302:
        return team != nullptr && team->healAuraLevel >= item.maxLevel;
    case 304:
        return team != nullptr && team->enemyTrackerUnlocked;
    default:
        break;
    }

    return false;
}

ItemType ShopIconItem(int choice)
{
    switch (choice)
    {
    case 1:
        return ItemType::LightBlock;
    case 2:
        return ItemType::WoodBlock;
    case 3:
        return ItemType::StoneBlock;
    case 4:
        return ItemType::ObsidianBlock;
    case 5:
        return ItemType::EnergyGlassBlock;
    case 6:
        return ItemType::SpringBlock;
    case 7:
        return ItemType::StickyBlock;
    case 8:
        return ItemType::ExplosiveBlock;
    case 101:
        return ItemType::Sword;
    case 102:
        return ItemType::Pickaxe;
    case 103:
        return ItemType::GoldResource;
    case 104:
        return ItemType::EnergyArrow;
    case 105:
        return ItemType::Fireball;
    case 106:
        return ItemType::Axe;
    case 107:
        return ItemType::Spear;
    case 108:
    case 109:
        return ItemType::Bow;
    case 401:
    case 402:
    case 403:
        return ItemType::Blaster;
    case 201:
        return ItemType::DashPearl;
    case 202:
        return ItemType::CrystalResource;
    case 203:
        return ItemType::MedKit;
    case 204:
        return ItemType::HomeTeleport;
    case 205:
        return ItemType::DashPearl;
    case 206:
        return ItemType::Molotov;
    case 207:
        return ItemType::AlarmTrap;
    case 301:
        return ItemType::IronResource;
    case 302:
        return ItemType::MedKit;
    case 303:
    case 304:
        return ItemType::CrystalResource;
    default:
        break;
    }
    return ItemType::None;
}

float Dot(Vector3 a, Vector3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3 Direction(Vector3 from, Vector3 to)
{
    const Vector3 delta { to.x - from.x, to.y - from.y, to.z - from.z };
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, -1.0f };
    }

    return Vector3 { delta.x / length, delta.y / length, delta.z / length };
}

Vector3 Add(Vector3 a, Vector3 b)
{
    return Vector3 { a.x + b.x, a.y + b.y, a.z + b.z };
}

Vector3 Scale(Vector3 value, float scale)
{
    return Vector3 { value.x * scale, value.y * scale, value.z * scale };
}

Vector3 Cross(Vector3 a, Vector3 b)
{
    return Vector3 {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

Vector3 Normalize(Vector3 value)
{
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 0.0001f)
    {
        return Vector3 { 0.0f, 0.0f, -1.0f };
    }

    return Vector3 { value.x / length, value.y / length, value.z / length };
}

bool IsInFrontOfCamera(Vector3 point, const Camera3D& camera)
{
    return Dot(Direction(camera.position, camera.target), Direction(camera.position, point)) > 0.05f;
}

std::string FormatTenths(float value)
{
    const int tenths = static_cast<int>(value * 10.0f + 0.5f);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

std::string AbilityStateText(const HeroAbilityState& state, bool ultimate, float ultimateCharge, bool primed)
{
    if (ultimate && primed)
    {
        return "включена";
    }
    if (state.activeTimer > 0.0f)
    {
        return "активно " + FormatTenths(state.activeTimer) + "с";
    }
    if (state.cooldownRemaining > 0.0f)
    {
        return FormatTenths(state.cooldownRemaining) + "с";
    }
    if (ultimate && ultimateCharge < 100.0f)
    {
        return std::to_string(static_cast<int>(ultimateCharge)) + "%";
    }
    return "готово";
}

void DrawSoftContactShadow(Vector3 groundCenter, float radius, float opacity, int quality)
{
    // Concentric translucent discs approximate a soft penumbra without a
    // screen-space depth pass. The low preset keeps one cheap disc so actors
    // remain grounded even when cascade maps are disabled or unsupported.
    const int rings = quality >= 2 ? 4 : (quality == 1 ? 2 : 1);
    const int segments = quality >= 2 ? 28 : (quality == 1 ? 16 : 10);
    for (int ring = rings; ring >= 1; --ring)
    {
        const float fraction = static_cast<float>(ring) / static_cast<float>(rings);
        const float ringOpacity = opacity * (1.0f - fraction * 0.62f) / static_cast<float>(rings);
        DrawCylinder(
            Vector3 { groundCenter.x, groundCenter.y + 0.012f + static_cast<float>(ring) * 0.0004f, groundCenter.z },
            radius * fraction,
            radius * fraction,
            0.012f,
            segments,
            Fade(BLACK, ringOpacity));
    }
}

const char* AbilityHudLabel(HeroId hero, HeroAbilitySlot slot)
{
    static constexpr const char* labels[][3] {
        { "Толчок", "Молотов", "Жертва" },
        { "Рывок", "Фантомы", "Телепорт" },
        { "Пылесос", "Камикадзе", "Рой" },
        { "Капкан", "Наручники", "Купол" },
        { "Тихие шаги", "Кровоток", "Маскировка" },
        { "Эхо", "Фаза", "Контуры" },
    };
    const int heroIndex = std::clamp(static_cast<int>(hero), 0, 5);
    const int slotIndex = std::clamp(static_cast<int>(slot), 0, 2);
    return labels[heroIndex][slotIndex];
}

Vector3 RotateFlat(Vector3 direction, float radians)
{
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return Vector3 {
        direction.x * c - direction.z * s,
        0.0f,
        direction.x * s + direction.z * c
    };
}

float HeroAnimationFraction(const HeroRuntimeState& state)
{
    if (state.animationDuration <= 0.001f)
    {
        return 0.0f;
    }
    return 1.0f - std::clamp(state.animationTimer / state.animationDuration, 0.0f, 1.0f);
}

bool IsAbilityCastAnimation(HeroAnimationState state)
{
    return state == HeroAnimationState::Cast
        || state == HeroAnimationState::Ability1
        || state == HeroAnimationState::Ability2
        || state == HeroAnimationState::Ultimate;
}

void DrawRadonPresentationEffect(const WorldEffect& effect)
{
    const float progress = std::clamp(effect.age / std::max(0.001f, effect.lifetime), 0.0f, 1.0f);
    const float t = 1.0f - progress;
    const Vector3 base { effect.position.x, effect.position.y + 0.05f, effect.position.z };

    switch (effect.kind)
    {
    case WorldEffectKind::Ring:
    {
        const float radius = effect.radius * (0.16f + progress * 0.84f);
        DrawCylinder(base, radius, radius, 0.045f, 36, Fade(effect.color, t * 0.20f));
        DrawCylinderWires(base, radius, radius, 0.055f, 36, Fade(effect.color, t * 0.92f));
        break;
    }
    case WorldEffectKind::Cone:
    case WorldEffectKind::Pull:
    {
        const float radius = effect.radius * (0.28f + progress * 0.72f);
        const float halfAngle = effect.kind == WorldEffectKind::Pull ? 0.46f : 0.56f;
        const int segments = 9;
        const Vector3 origin { effect.position.x, effect.position.y + 0.42f, effect.position.z };
        Vector3 previous {};
        for (int i = 0; i <= segments; ++i)
        {
            const float u = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(segments);
            const Vector3 ray = RotateFlat(effect.direction, u * halfAngle);
            const Vector3 end { origin.x + ray.x * radius, origin.y + 0.10f * std::sin(progress * 6.28f), origin.z + ray.z * radius };
            DrawLine3D(origin, end, Fade(effect.color, t * 0.82f));
            if (i > 0)
            {
                DrawLine3D(previous, end, Fade(effect.color, t * 0.58f));
            }
            if (effect.kind == WorldEffectKind::Pull && i % 2 == 0)
            {
                const Vector3 inner { origin.x + ray.x * radius * 0.52f, origin.y, origin.z + ray.z * radius * 0.52f };
                DrawLine3D(end, inner, Fade(Color { 178, 245, 255, 255 }, t * 0.76f));
            }
            previous = end;
        }
        break;
    }
    case WorldEffectKind::Trail:
    {
        const Vector3 tail {
            base.x - effect.direction.x * effect.radius * (1.2f + progress),
            base.y - 0.12f * progress,
            base.z - effect.direction.z * effect.radius * (1.2f + progress)
        };
        DrawCylinderEx(
            tail,
            base,
            std::max(0.008f, effect.radius * 0.08f),
            std::max(0.003f, effect.radius * 0.025f),
            6,
            Fade(effect.color, t * 0.78f));
        DrawLine3D(tail, base, Fade(WHITE, t * 0.55f));
        break;
    }
    case WorldEffectKind::FireZone:
    {
        const float radius = effect.radius * (0.92f + 0.08f * std::sin(effect.age * 14.0f));
        DrawCylinder(base, radius, radius, 0.035f, 32, Fade(effect.color, t * 0.24f));
        DrawCylinderWires(base, radius, radius, 0.045f, 32, Fade(effect.color, t * 0.80f));
        DrawSphere(Vector3 { base.x, base.y + 0.16f + std::sin(effect.age * 18.0f) * 0.05f, base.z }, radius * 0.18f, Fade(effect.color, t * 0.50f));
        break;
    }
    case WorldEffectKind::CorePulse:
    {
        const float radius = effect.radius * (0.65f + 0.25f * std::sin(effect.age * 9.0f));
        DrawSphereWires(Vector3 { base.x, base.y + 0.35f, base.z }, radius, 10, 16, Fade(effect.color, t * 0.86f));
        DrawSphere(Vector3 { base.x, base.y + 0.35f, base.z }, radius * 0.18f, Fade(Color { 178, 245, 255, 255 }, t * 0.50f));
        break;
    }
    case WorldEffectKind::Sacrifice:
    {
        const float radius = effect.radius * (0.20f + progress * 0.90f);
        DrawCylinderWires(base, radius, radius, 0.08f, 40, Fade(Color { 178, 245, 255, 255 }, t));
        DrawSphere(Vector3 { base.x, base.y + 0.42f, base.z }, 0.38f + progress * 0.42f, Fade(effect.color, t * 0.62f));
        break;
    }
    case WorldEffectKind::Burst:
    default:
        DrawSphere(effect.position, effect.radius + effect.age * 1.4f, Fade(effect.color, t * 0.65f));
        DrawSphereWires(effect.position, (effect.radius + effect.age * 1.4f) * 1.08f, 8, 10, Fade(WHITE, t));
        break;
    }
}

void DrawOrbitaTeleportPreview(const OrbitaTeleportPreview& preview)
{
    if (!preview.visible)
    {
        return;
    }

    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(GetTime()) * 9.0f);
    const Color beamColor = preview.valid ? WHITE : Color { 255, 118, 118, 255 };
    Vector3 right { -preview.direction.z, 0.0f, preview.direction.x };
    right = Normalize(right);
    const Vector3 up { 0.0f, 1.0f, 0.0f };
    const Vector3 sideA = Add(Scale(right, 0.045f), Scale(up, 0.012f));
    const Vector3 sideB = Add(Scale(right, -0.045f), Scale(up, -0.012f));

    DrawLine3D(preview.start, preview.destination, Fade(beamColor, preview.valid ? 0.88f : 0.55f));
    DrawLine3D(Add(preview.start, sideA), Add(preview.destination, sideA), Fade(beamColor, 0.48f));
    DrawLine3D(Add(preview.start, sideB), Add(preview.destination, sideB), Fade(beamColor, 0.34f));

    const Vector3 markerBase { preview.destination.x, preview.destination.y - 0.86f, preview.destination.z };
    const Vector3 markerTop { preview.destination.x, preview.destination.y + 3.0f + pulse * 0.55f, preview.destination.z };
    DrawLine3D(markerBase, markerTop, Fade(beamColor, preview.valid ? 0.86f : 0.62f));
    DrawLine3D(Add(markerBase, Scale(right, 0.08f)), Add(markerTop, Scale(right, 0.08f)), Fade(beamColor, 0.40f));
    DrawLine3D(Add(markerBase, Scale(right, -0.08f)), Add(markerTop, Scale(right, -0.08f)), Fade(beamColor, 0.40f));

    const float ringRadius = 0.42f + pulse * 0.16f;
    DrawCylinder(markerBase, ringRadius, ringRadius, 0.055f, 40, Fade(beamColor, preview.valid ? 0.24f : 0.18f));
    DrawCylinderWires(markerBase, ringRadius, ringRadius, 0.07f, 40, Fade(beamColor, preview.valid ? 0.92f : 0.72f));
    DrawSphere(Vector3 { preview.destination.x, preview.destination.y + 0.16f, preview.destination.z }, 0.12f + pulse * 0.05f, Fade(beamColor, 0.74f));
}

void DrawHeroDeviceVisual(const HeroDeviceVisual& device, int localTeamId)
{
    const bool enemy = localTeamId >= 0 && device.teamId != localTeamId;
    const Color relationColor = enemy ? Color { 255, 92, 92, 255 } : Color { 102, 226, 255, 255 };
    const Color bromColor = MixColor(VisualTheme::HeroAccent(HeroId::Brom), relationColor, 0.42f);
    const Color konvoyColor = MixColor(VisualTheme::HeroAccent(HeroId::Konvoy), relationColor, 0.42f);
    const Color svidetelColor = MixColor(VisualTheme::HeroAccent(HeroId::Svidetel), relationColor, 0.42f);
    const float time = static_cast<float>(GetTime());
    const float pulse = 0.5f + 0.5f * std::sin(time * 8.0f + device.position.x * 0.7f + device.position.z * 0.4f);
    Vector3 forward = Normalize(device.direction);
    Vector3 right { -forward.z, 0.0f, forward.x };
    right = Normalize(right);
    if (std::abs(right.x) + std::abs(right.z) <= 0.001f)
    {
        right = Vector3 { 1.0f, 0.0f, 0.0f };
    }

    if (device.kind == HeroDeviceVisualKind::KonvoyTrap)
    {
        const Vector3 base { device.position.x, device.position.y + 0.02f, device.position.z };
        const float radius = std::max(0.20f, device.radius);
        const Color trapColor = device.active ? MixColor(konvoyColor, WHITE, 0.35f) : konvoyColor;
        DrawCylinder(base, radius, radius, 0.035f, 28, Fade(trapColor, 0.26f + pulse * 0.10f));
        DrawCylinderWires(base, radius, radius, 0.05f, 28, Fade(trapColor, 0.76f));
        DrawLine3D(
            Vector3 { base.x - radius * 0.65f, base.y + 0.04f, base.z },
            Vector3 { base.x + radius * 0.65f, base.y + 0.04f, base.z },
            Fade(WHITE, 0.54f));
        DrawLine3D(
            Vector3 { base.x, base.y + 0.04f, base.z - radius * 0.65f },
            Vector3 { base.x, base.y + 0.04f, base.z + radius * 0.65f },
            Fade(WHITE, 0.54f));
        return;
    }
    if (device.kind == HeroDeviceVisualKind::KonvoyTether)
    {
        const float glow = device.active ? 0.92f : 0.62f;
        DrawLine3D(device.position, device.target, Fade(konvoyColor, glow));
        DrawLine3D(Add(device.position, Scale(right, 0.035f)), Add(device.target, Scale(right, 0.035f)), Fade(WHITE, 0.42f));
        DrawSphere(device.position, 0.13f + pulse * 0.03f, Fade(konvoyColor, 0.70f));
        DrawSphere(device.target, 0.13f + pulse * 0.03f, Fade(konvoyColor, 0.70f));
        DrawSphereWires(device.target, std::max(0.55f, device.radius) * 0.18f, 8, 10, Fade(konvoyColor, 0.46f));
        return;
    }
    if (device.kind == HeroDeviceVisualKind::KonvoyDome)
    {
        const float radius = std::max(1.0f, device.radius);
        const Vector3 center { device.position.x, device.position.y + 0.18f, device.position.z };
        DrawCylinder(center, radius, radius, 0.055f, 48, Fade(konvoyColor, 0.13f + pulse * 0.04f));
        DrawCylinderWires(center, radius, radius, 0.075f, 48, Fade(konvoyColor, 0.72f));
        DrawSphereWires(Vector3 { center.x, center.y + 1.25f, center.z }, radius, 12, 20, Fade(konvoyColor, device.active ? 0.66f : 0.38f));
        DrawSphere(Vector3 { center.x, center.y + 1.25f, center.z }, 0.18f + pulse * 0.05f, Fade(WHITE, 0.54f));
        return;
    }
    if (device.kind == HeroDeviceVisualKind::SvidetelEcho)
    {
        const Vector3 center { device.position.x, device.position.y + 0.85f, device.position.z };
        const float alpha = device.temporary ? 0.38f : 0.68f;
        DrawCube(center, 0.52f, 1.45f, 0.52f, Fade(svidetelColor, alpha));
        DrawCubeWires(center, 0.58f, 1.52f, 0.58f, Fade(WHITE, 0.48f));
        DrawSphere(Vector3 { center.x, center.y + 0.92f, center.z }, 0.28f, Fade(svidetelColor, alpha + 0.12f));
        DrawSphereWires(center, std::max(0.45f, device.radius) + pulse * 0.08f, 8, 12, Fade(svidetelColor, 0.54f));
        if (device.active)
        {
            DrawLine3D(center, Vector3 { device.target.x, device.target.y + 0.7f, device.target.z }, Fade(WHITE, 0.82f));
        }
        return;
    }

    if (device.kind == HeroDeviceVisualKind::BromVacuumBot)
    {
        const Vector3 base { device.position.x, device.position.y + 0.08f, device.position.z };
        const Color body = device.temporary ? MixColor(bromColor, WHITE, 0.22f) : bromColor;
        DrawCube(base, 0.72f, 0.28f, 0.86f, Fade(body, 0.92f));
        DrawCubeWires(base, 0.75f, 0.31f, 0.89f, Fade(WHITE, 0.58f));
        DrawSphere(Vector3 { base.x, base.y + 0.24f + pulse * 0.035f, base.z }, 0.16f, Fade(Color { 178, 245, 255, 255 }, 0.86f));

        const Vector3 wheelA = Add(Add(base, Scale(right, 0.43f)), Vector3 { 0.0f, -0.17f, 0.0f });
        const Vector3 wheelB = Add(Add(base, Scale(right, -0.43f)), Vector3 { 0.0f, -0.17f, 0.0f });
        DrawSphere(wheelA, 0.12f, Color { 26, 32, 34, 255 });
        DrawSphere(wheelB, 0.12f, Color { 26, 32, 34, 255 });

        if (device.cargoUnits > 0 && device.cargoCapacity > 0)
        {
            const float cargoFraction = std::clamp(static_cast<float>(device.cargoUnits) / static_cast<float>(device.cargoCapacity), 0.0f, 1.0f);
            DrawCylinderWires(Vector3 { base.x, base.y + 0.32f, base.z }, 0.28f + cargoFraction * 0.18f, 0.28f + cargoFraction * 0.18f, 0.055f, 28, Fade(Color { 255, 224, 122, 255 }, 0.82f));
            DrawSphere(Vector3 { base.x, base.y + 0.42f, base.z }, 0.08f + cargoFraction * 0.07f, Fade(Color { 255, 224, 122, 255 }, 0.82f));
        }
        if (device.returning)
        {
            DrawLine3D(Vector3 { base.x, base.y + 0.30f, base.z }, Vector3 { device.target.x, device.target.y + 0.25f, device.target.z }, Fade(Color { 255, 224, 122, 255 }, 0.52f));
        }
    }
    else
    {
        const Vector3 center { device.position.x, device.position.y + 0.10f + pulse * 0.05f, device.position.z };
        const Color core = device.temporary ? MixColor(bromColor, WHITE, 0.26f) : bromColor;
        DrawSphere(center, 0.26f, Fade(core, 0.92f));
        DrawSphereWires(center, 0.42f + pulse * 0.05f, 8, 12, Fade(core, 0.56f));
        for (int i = 0; i < 4; ++i)
        {
            const Vector3 arm = RotateFlat(forward, kPi * 0.5f * static_cast<float>(i));
            const Vector3 end = Add(center, Scale(arm, 0.48f));
            DrawLine3D(center, end, Fade(WHITE, 0.46f));
            DrawSphere(end, 0.09f, Fade(Color { 178, 245, 255, 255 }, 0.80f));
        }

        if (device.active)
        {
            DrawLine3D(center, device.target, Fade(Color { 178, 245, 255, 255 }, 0.92f));
            DrawLine3D(Add(center, Scale(right, 0.035f)), Add(device.target, Scale(right, 0.035f)), Fade(bromColor, 0.82f));
            DrawSphere(device.target, 0.15f + pulse * 0.04f, Fade(Color { 178, 245, 255, 255 }, 0.72f));
        }
        else
        {
            DrawLine3D(center, Add(center, Scale(forward, 0.62f)), Fade(core, 0.42f));
        }
    }

    if (device.temporary)
    {
        const float radius = 0.72f + (1.0f - device.lifetimeFraction) * 0.12f;
        DrawSphereWires(Vector3 { device.position.x, device.position.y + 0.20f, device.position.z }, radius, 8, 10, Fade(Color { 178, 245, 255, 255 }, 0.28f + device.lifetimeFraction * 0.32f));
    }
}

Color ItemUiColor(ItemType type)
{
    switch (type)
    {
    case ItemType::Sword:
        return Color { 238, 238, 248, 255 };
    case ItemType::Axe:
        return Color { 255, 190, 122, 255 };
    case ItemType::Spear:
        return Color { 180, 220, 255, 255 };
    case ItemType::Pickaxe:
        return Color { 188, 198, 210, 255 };
    case ItemType::Bow:
        return Color { 224, 170, 92, 255 };
    case ItemType::Blaster:
        return Color { 98, 245, 255, 255 };
    case ItemType::SniperRifle:
        return Color { 126, 220, 255, 255 };
    case ItemType::EnergyArrow:
        return Color { 112, 232, 255, 255 };
    case ItemType::Fireball:
    case ItemType::Molotov:
        return Color { 255, 118, 70, 255 };
    case ItemType::MedKit:
        return Color { 128, 238, 166, 255 };
    case ItemType::HomeTeleport:
        return Color { 180, 148, 255, 255 };
    case ItemType::DashPearl:
        return Color { 125, 230, 255, 255 };
    case ItemType::AlarmTrap:
        return Color { 255, 235, 142, 255 };
    case ItemType::IronResource:
        return Color { 188, 198, 210, 255 };
    case ItemType::GoldResource:
        return Color { 246, 196, 74, 255 };
    case ItemType::CrystalResource:
        return Color { 112, 232, 255, 255 };
    default:
        break;
    }
    return Color { 220, 224, 235, 255 };
}

Color MinecraftDyeColor(int variant)
{
    // Minecraft 1.12 dye IDs: white, orange, magenta, light-blue, yellow,
    // lime, pink, gray, light-gray, cyan, purple, blue, brown, green, red,
    // black.  Import keeps this in Block::variant rather than team ownership.
    static constexpr Color kDyes[] {
        Color { 222, 222, 218, 255 }, Color { 224, 124, 42, 255 },
        Color { 198, 90, 210, 255 }, Color { 102, 172, 222, 255 },
        Color { 232, 208, 62, 255 }, Color { 114, 202, 58, 255 },
        Color { 236, 142, 172, 255 }, Color { 72, 76, 82, 255 },
        Color { 156, 164, 170, 255 }, Color { 46, 152, 166, 255 },
        Color { 122, 72, 172, 255 }, Color { 54, 86, 194, 255 },
        Color { 108, 72, 44, 255 }, Color { 68, 126, 62, 255 },
        Color { 202, 54, 48, 255 }, Color { 34, 38, 44, 255 }
    };
    return kDyes[std::clamp(variant, 0, 15)];
}

bool IsTransparentBlock(BlockType type, Color color)
{
    return color.a < 255
        || type == BlockType::EnergyGlassBlock
        || type == BlockType::ColoredGlassBlock
        || type == BlockType::IceBlock;
}

ItemStack VisibleHeldItemForPlayer(const Player& player, const ItemStack& localHeldItem)
{
    if (IsLocallyPredicted(player.GetControlKind()))
    {
        return localHeldItem;
    }

    const auto& hotbar = player.GetInventory().GetHotbarSlots();
    for (const ItemStack& stack : hotbar)
    {
        if (!stack.IsEmpty() && (ItemIsWeapon(stack.type) || ItemIsPickaxe(stack.type)))
        {
            return stack;
        }
    }
    for (const ItemStack& stack : hotbar)
    {
        if (!stack.IsEmpty())
        {
            return stack;
        }
    }
    return ItemStack {};
}

void DrawHeldItemModel(const ItemStack& stack, Vector3 hand, Vector3 forward, Vector3 right, Vector3 modelUp, float scale, const Texture2D* itemTexture, const Texture2D* blockTexture, Color tint, bool firstPerson = false, float rangedChargeFraction = 0.0f)
{
    if (stack.IsEmpty())
    {
        return;
    }

    forward = Normalize(forward);
    right = Normalize(right);
    modelUp = Normalize(modelUp);
    const Vector3 worldUp { 0.0f, 1.0f, 0.0f };
    const bool stableToolView = firstPerson && (ItemIsWeapon(stack.type) || ItemIsPickaxe(stack.type) || stack.type == ItemType::EnergyArrow);
    const Vector3 up = stableToolView ? modelUp : worldUp;
    const Vector3 grip = Add(hand, Scale(right, 0.03f * scale));
    const Vector3 outward = firstPerson
        ? Normalize(Add(Add(Scale(right, -0.58f), Scale(up, 0.96f)), Scale(forward, 0.18f)))
        : Normalize(Add(Scale(forward, 0.68f), Scale(up, 0.32f)));
    const bool iconHeldItem = itemTexture != nullptr
        && ((ItemIsWeapon(stack.type) && stack.type != ItemType::Bow && !ItemIsBlasterWeapon(stack.type))
            || ItemIsPickaxe(stack.type)
            || stack.type == ItemType::EnergyArrow
            || stack.type == ItemType::Fireball
            || stack.type == ItemType::Molotov);
    if (iconHeldItem)
    {
        const Vector3 center = Add(grip, Scale(outward, 0.34f * scale));
        const Camera3D itemCamera {
            Add(center, Scale(forward, firstPerson ? -2.0f : -2.8f)),
            center,
            up,
            45.0f,
            CAMERA_PERSPECTIVE
        };
        DrawDepthTestedBillboard(itemCamera, *itemTexture, center, 0.46f * scale, WHITE);
        return;
    }

    switch (stack.type)
    {
    case ItemType::Sword:
    {
        const Vector3 guard = Add(grip, Scale(outward, 0.22f * scale));
        const Vector3 tip = Add(guard, Scale(outward, 0.72f * scale));
        DrawCylinderEx(grip, guard, 0.035f * scale, 0.035f * scale, 8, Color { 92, 58, 34, 255 });
        DrawCylinderEx(Add(guard, Scale(right, -0.16f * scale)), Add(guard, Scale(right, 0.16f * scale)), 0.026f * scale, 0.026f * scale, 8, Color { 246, 196, 74, 255 });
        DrawCylinderEx(guard, tip, 0.045f * scale, 0.018f * scale, 8, Color { 232, 236, 244, 255 });
        break;
    }
    case ItemType::Pickaxe:
    {
        const Vector3 top = Add(grip, Scale(outward, 0.62f * scale));
        DrawCylinderEx(grip, top, 0.034f * scale, 0.034f * scale, 8, Color { 118, 72, 42, 255 });
        DrawCylinderEx(Add(top, Scale(right, -0.30f * scale)), Add(top, Scale(right, 0.30f * scale)), 0.045f * scale, 0.020f * scale, 8, Color { 178, 188, 200, 255 });
        DrawCylinderEx(Add(top, Scale(right, 0.20f * scale)), Add(Add(top, Scale(right, 0.36f * scale)), Scale(outward, -0.12f * scale)), 0.024f * scale, 0.0f, 8, Color { 208, 216, 226, 255 });
        break;
    }
    case ItemType::Axe:
    {
        const Vector3 top = Add(grip, Scale(outward, 0.58f * scale));
        DrawCylinderEx(grip, top, 0.035f * scale, 0.035f * scale, 8, Color { 118, 72, 42, 255 });
        const Vector3 headLeft = Add(top, Scale(right, -0.10f * scale));
        const Vector3 headRight = Add(top, Scale(right, 0.28f * scale));
        DrawCylinderEx(headLeft, headRight, 0.075f * scale, 0.115f * scale, 8, Color { 176, 186, 198, 255 });
        DrawCylinderEx(headRight, Add(headRight, Scale(outward, -0.16f * scale)), 0.115f * scale, 0.0f, 8, Color { 214, 220, 230, 255 });
        break;
    }
    case ItemType::Spear:
    {
        const Vector3 tipBase = Add(grip, Scale(outward, 0.86f * scale));
        const Vector3 tip = Add(tipBase, Scale(outward, 0.18f * scale));
        DrawCylinderEx(grip, tipBase, 0.022f * scale, 0.022f * scale, 8, Color { 122, 78, 48, 255 });
        DrawCylinderEx(tipBase, tip, 0.060f * scale, 0.0f, 8, Color { 180, 220, 255, 255 });
        break;
    }
    case ItemType::EnergyArrow:
        DrawCylinderEx(grip, Add(grip, Scale(outward, 0.70f * scale)), 0.020f * scale, 0.010f * scale, 8, Color { 112, 232, 255, 255 });
        break;
    case ItemType::Bow:
    {
        const float draw = std::clamp(rangedChargeFraction, 0.0f, 1.0f);
        const Vector3 center = Add(grip, Scale(outward, 0.28f * scale));
        const Vector3 upper = Add(Add(center, Scale(up, 0.48f * scale)), Scale(outward, 0.10f * scale));
        const Vector3 lower = Add(Add(center, Scale(up, -0.48f * scale)), Scale(outward, 0.10f * scale));
        const Vector3 stringGrip = Add(center, Scale(outward, -0.34f * scale * draw));
        DrawCylinderEx(lower, center, 0.035f * scale, 0.045f * scale, 7, Color { 155, 92, 46, 255 });
        DrawCylinderEx(center, upper, 0.045f * scale, 0.035f * scale, 7, Color { 205, 142, 70, 255 });
        DrawLine3D(upper, stringGrip, Color { 232, 236, 244, 255 });
        DrawLine3D(stringGrip, lower, Color { 232, 236, 244, 255 });
        if (draw > 0.02f)
        {
            const Vector3 arrowTip = Add(center, Scale(outward, (0.65f + draw * 0.20f) * scale));
            DrawCylinderEx(stringGrip, arrowTip, 0.018f * scale, 0.009f * scale, 6, Color { 112, 232, 255, 255 });
        }
        break;
    }
    case ItemType::Blaster:
    case ItemType::SniperRifle:
    {
        const bool sniper = stack.type == ItemType::SniperRifle;
        const Vector3 muzzle = Add(grip, Scale(outward, (sniper ? 0.86f : 0.62f) * scale));
        DrawCylinderEx(grip, muzzle, (sniper ? 0.085f : 0.10f) * scale, (sniper ? 0.052f : 0.075f) * scale, 10, Color { 50, 68, 92, 255 });
        DrawCylinderEx(Add(grip, Scale(outward, 0.18f * scale)), muzzle, 0.060f * scale, 0.045f * scale, 10, tint);
        DrawSphere(muzzle, (0.075f + rangedChargeFraction * 0.035f) * scale, tint);
        DrawSphereWires(Add(grip, Scale(outward, 0.34f * scale)), 0.13f * scale, 5, 8, Fade(tint, 0.86f));
        if (sniper)
        {
            const Vector3 scopeStart = Add(Add(grip, Scale(outward, 0.24f * scale)), Scale(up, 0.13f * scale));
            const Vector3 scopeEnd = Add(scopeStart, Scale(outward, 0.38f * scale));
            DrawCylinderEx(scopeStart, scopeEnd, 0.068f * scale, 0.068f * scale, 12, Color { 28, 38, 54, 255 });
            DrawCylinderEx(Add(scopeStart, Scale(outward, -0.035f * scale)), scopeStart, 0.088f * scale, 0.068f * scale, 12, Color { 106, 210, 245, 255 });
            DrawCylinderEx(scopeEnd, Add(scopeEnd, Scale(outward, 0.035f * scale)), 0.068f * scale, 0.088f * scale, 12, Color { 106, 210, 245, 255 });
        }
        break;
    }
    case ItemType::Fireball:
    case ItemType::Molotov:
        DrawSphere(Add(grip, Scale(outward, 0.24f * scale)), 0.15f * scale, Color { 255, 118, 70, 255 });
        break;
    default:
    {
        const Vector3 center = Add(grip, Scale(outward, 0.26f * scale));
        if (const std::optional<ResourceType> resource = ItemToResource(stack.type))
        {
            const Color resourceColor = *resource == ResourceType::Iron
                ? Color { 188, 198, 210, 255 }
                : (*resource == ResourceType::Gold ? Color { 246, 196, 74, 255 } : Color { 112, 232, 255, 255 });
            if (*resource == ResourceType::Crystal)
            {
                DrawCylinderEx(
                    Add(center, Scale(up, -0.13f * scale)),
                    Add(center, Scale(up, 0.13f * scale)),
                    0.11f * scale,
                    0.04f * scale,
                    6,
                    resourceColor);
                DrawCylinderEx(
                    Add(center, Scale(up, 0.13f * scale)),
                    Add(center, Scale(up, 0.22f * scale)),
                    0.04f * scale,
                    0.0f,
                    6,
                    Color { 220, 252, 255, 255 });
            }
            else
            {
                DrawCube(center, 0.24f * scale, 0.13f * scale, 0.18f * scale, resourceColor);
                DrawCubeWires(center, 0.25f * scale, 0.14f * scale, 0.19f * scale, Fade(WHITE, 0.52f));
            }
        }
        else if (blockTexture != nullptr)
        {
            DrawTexturedCube(*blockTexture, center, 0.28f * scale, 0.28f * scale, 0.28f * scale, tint);
        }
        else if (itemTexture != nullptr)
        {
            DrawDepthTestedBillboard(Camera3D { Add(center, Scale(forward, -2.0f)), center, up, 45.0f, CAMERA_PERSPECTIVE }, *itemTexture, center, 0.34f * scale, WHITE);
        }
        else
        {
            DrawCube(center, 0.24f * scale, 0.24f * scale, 0.24f * scale, tint);
        }
        break;
    }
    }
}
}

bool Renderer::Initialize()
{
    if (texturesReady_)
    {
        return true;
    }

    // Key opaque materials also bake a normal map + gloss mask (see
    // MakeNormalMapImage); the bump scale and mask are per-material knobs.
    const auto loadBlockWithNormal = [this](
        BlockType type, const char* assetName, Image fallback, float bump, unsigned char material)
    {
        Texture2D normalTexture {};
        Texture2D albedo = LoadCustomizableTextureWithNormal(assetName, fallback, bump, material, normalTexture);
        ApplyWorldTextureSampling(albedo);
        ApplyWorldTextureSampling(normalTexture);
        blockNormalTextures_[static_cast<std::size_t>(type)] = normalTexture;
        return albedo;
    };
    grassTexture_ = loadBlockWithNormal(BlockType::GrassBlock, "grass_block", MakeGrassImage(), 1.6f, 10);
    dirtTexture_ = loadBlockWithNormal(BlockType::DirtBlock, "dirt_block", MakeTextureImage(Color { 112, 78, 52, 255 }, Color { 86, 58, 38, 255 }, 5), 1.8f, 8);
    leafTexture_ = LoadCustomizableTexture("leaf_block", MakeTextureImage(Color { 64, 132, 62, 255 }, Color { 112, 178, 86, 255 }, 6));
    woodTexture_ = loadBlockWithNormal(BlockType::WoodBlock, "wood_block", MakeWoodImage(), 2.4f, 26);
    teamChestTexture_ = loadBlockWithNormal(BlockType::TeamChestBlock, "team_chest_block", MakeTeamChestImage(), 2.0f, 60);
    woolTexture_ = loadBlockWithNormal(BlockType::WoolBlock, "wool_block", MakeWoolImage(), 1.2f, 8);
    stoneTexture_ = loadBlockWithNormal(BlockType::StoneBlock, "stone_block", MakeTextureImage(Color { 132, 138, 148, 255 }, Color { 88, 94, 108, 255 }, 7), 2.2f, 55);
    smoothStoneTexture_ = loadBlockWithNormal(BlockType::SmoothStoneBlock, "smooth_stone_block", MakeSmoothStoneImage(), 1.6f, 120);
    darkBrickTexture_ = loadBlockWithNormal(BlockType::DarkBrickBlock, "dark_brick_block", MakeBrickImage(Color { 42, 38, 46, 255 }, Color { 84, 72, 88, 255 }, Color { 62, 54, 70, 255 }, 18), 2.6f, 42);
    lightBrickTexture_ = loadBlockWithNormal(BlockType::LightBrickBlock, "light_brick_block", MakeBrickImage(Color { 166, 150, 122, 255 }, Color { 224, 204, 166, 255 }, Color { 194, 170, 132, 255 }, 19), 2.6f, 46);
    metalBlockTexture_ = loadBlockWithNormal(BlockType::MetalBlock, "metal_block", MakeMetalBlockImage(), 2.0f, 230);
    // 250+ is reserved by lighting.fs as an explicit emissive material tag;
    // ordinary polished stone, glass and metals remain reflective only.
    glowBlockTexture_ = loadBlockWithNormal(BlockType::GlowBlock, "glow_block", MakeGlowBlockImage(), 1.6f, 255);
    plankVariantTexture_ = loadBlockWithNormal(BlockType::PlankBlock, "plank_block", MakePlankVariantImage(), 2.4f, 32);
    decorativeTileTexture_ = loadBlockWithNormal(BlockType::DecorativeTileBlock, "decorative_tile_block", MakeDecorativeTileImage(), 2.2f, 130);
    trimBlockTexture_ = loadBlockWithNormal(BlockType::TrimBlock, "trim_block", MakeTrimBlockImage(), 2.0f, 90);
    cobblestoneTexture_ = loadBlockWithNormal(BlockType::CobblestoneBlock, "cobblestone_block", MakeCobblestoneImage(), 3.1f, 36);
    andesiteTexture_ = loadBlockWithNormal(BlockType::AndesiteBlock, "andesite_block", MakeAndesiteImage(false), 1.9f, 54);
    polishedAndesiteTexture_ = loadBlockWithNormal(BlockType::PolishedAndesiteBlock, "polished_andesite_block", MakeAndesiteImage(true), 1.35f, 142);
    stoneBrickTexture_ = loadBlockWithNormal(BlockType::StoneBrickBlock, "stone_brick_block", MakeBrickImage(Color { 76, 82, 90, 255 }, Color { 142, 148, 154, 255 }, Color { 116, 123, 132, 255 }, 37), 2.8f, 46);
    chiseledStoneBrickTexture_ = loadBlockWithNormal(BlockType::ChiseledStoneBrickBlock, "chiseled_stone_brick_block", MakeChiseledStoneImage(), 3.3f, 74);
    birchPlankTexture_ = loadBlockWithNormal(BlockType::BirchPlankBlock, "birch_plank_block", MakeBirchPlankImage(), 2.6f, 34);
    coloredGlassTexture_ = LoadCustomizableTexture("colored_glass_block", MakeColoredGlassImage());
    coloredClayTexture_ = loadBlockWithNormal(BlockType::ColoredClayBlock, "colored_clay_block", MakeClayImage(), 1.8f, 22);
    lapisTexture_ = loadBlockWithNormal(BlockType::LapisBlock, "lapis_block", MakeLapisImage(), 2.3f, 194);
    diamondTexture_ = loadBlockWithNormal(BlockType::DiamondBlock, "diamond_block", MakeGemBlockImage(Color { 74, 202, 214, 255 }, Color { 176, 255, 252, 255 }, Color { 24, 108, 122, 255 }, 41), 2.0f, 232);
    emeraldTexture_ = loadBlockWithNormal(BlockType::EmeraldBlock, "emerald_block", MakeGemBlockImage(Color { 38, 168, 92, 255 }, Color { 142, 255, 170, 255 }, Color { 14, 78, 42, 255 }, 42), 2.0f, 218);
    goldTexture_ = loadBlockWithNormal(BlockType::GoldBlock, "gold_block", MakeGoldBlockImage(), 2.4f, 240);
    ironBarsTexture_ = loadBlockWithNormal(BlockType::IronBarsBlock, "iron_bars_block", MakeIronBarsImage(), 2.7f, 186);
    ladderTexture_ = loadBlockWithNormal(BlockType::LadderBlock, "ladder_block", MakeLadderImage(), 2.6f, 30);
    torchTexture_ = loadBlockWithNormal(BlockType::TorchBlock, "torch_block", MakeTorchBlockImage(), 1.5f, 253);
    obsidianTexture_ = loadBlockWithNormal(BlockType::ObsidianBlock, "obsidian_block", MakeTextureImage(Color { 38, 28, 54, 255 }, Color { 90, 52, 132, 255 }, 8), 1.8f, 160);
    glassTexture_ = LoadCustomizableTexture("energy_glass_block", MakeTextureImage(Color { 112, 232, 255, 150 }, Color { 220, 252, 255, 210 }, 9));
    springTexture_ = loadBlockWithNormal(BlockType::SpringBlock, "spring_block", MakeTextureImage(Color { 92, 196, 124, 255 }, Color { 255, 235, 142, 255 }, 10), 1.6f, 60);
    stickyTexture_ = loadBlockWithNormal(BlockType::StickyBlock, "sticky_block", MakeTextureImage(Color { 92, 184, 118, 255 }, Color { 40, 112, 72, 255 }, 11), 1.6f, 80);
    tntTexture_ = loadBlockWithNormal(BlockType::ExplosiveBlock, "explosive_block", MakeTntImage(), 1.8f, 24);
    spikeTexture_ = loadBlockWithNormal(BlockType::SpikeBlock, "spike_block", MakeTextureImage(Color { 148, 148, 158, 255 }, Color { 236, 236, 244, 255 }, 12), 1.8f, 140);
    lavaTexture_ = loadBlockWithNormal(BlockType::LavaBlock, "lava_block", MakeTextureImage(Color { 230, 70, 28, 255 }, Color { 255, 210, 66, 255 }, 13), 2.2f, 251);
    iceTexture_ = LoadCustomizableTexture("ice_block", MakeTextureImage(Color { 150, 225, 255, 190 }, Color { 230, 250, 255, 230 }, 14));
    // Blocks without a normal map still want the distance-mip sampling.
    ApplyWorldTextureSampling(leafTexture_);
    ApplyWorldTextureSampling(coloredGlassTexture_);
    ApplyWorldTextureSampling(glassTexture_);
    ApplyWorldTextureSampling(iceTexture_);

    // Neutral flat normal so chunk materials always have a valid map bound:
    // an unbound "texture2" sampler would read whatever unit 2 last held.
    {
        Image flatNormal = GenImageColor(4, 4, Color { 128, 128, 255, 30 });
        flatNormalTexture_ = LoadTextureFromImage(flatNormal);
        UnloadImage(flatNormal);
        if (flatNormalTexture_.id != 0)
        {
            SetTextureFilter(flatNormalTexture_, TEXTURE_FILTER_POINT);
        }
    }
    swordIcon_ = LoadCustomizableTexture("sword", MakeSwordIcon());
    axeIcon_ = LoadCustomizableTexture("axe", MakeAxeIcon());
    spearIcon_ = LoadCustomizableTexture("spear", MakeSpearIcon());
    pickaxeIcon_ = LoadCustomizableTexture("pickaxe", MakePickaxeIcon());
    arrowIcon_ = LoadCustomizableTexture("energy_arrow", MakeArrowIcon());
    fireballIcon_ = LoadCustomizableTexture("fireball", MakeFireballIcon());
    medKitIcon_ = LoadCustomizableTexture("med_kit", MakeMedKitIcon());
    homeIcon_ = LoadCustomizableTexture("home_teleport", MakeHomeIcon());
    dashIcon_ = LoadCustomizableTexture("dash_pearl", MakeDashIcon());
    molotovIcon_ = LoadCustomizableTexture("molotov", MakeMolotovIcon());
    alarmIcon_ = LoadCustomizableTexture("alarm_trap", MakeAlarmIcon());
    ironIcon_ = LoadCustomizableTexture("iron_resource", MakeResourceIcon(Color { 188, 198, 210, 255 }, WHITE));
    goldIcon_ = LoadCustomizableTexture("gold_resource", MakeResourceIcon(Color { 246, 196, 74, 255 }, Color { 255, 246, 180, 255 }));
    crystalIcon_ = LoadCustomizableTexture("crystal_resource", MakeResourceIcon(Color { 112, 232, 255, 255 }, Color { 225, 252, 255, 255 }));
    sceneShader_.Initialize();
    heroVisuals_.Initialize();
    heroPreviewTarget_ = LoadRenderTexture(384, 384);
    if (heroPreviewTarget_.texture.id != 0)
    {
        SetTextureFilter(heroPreviewTarget_.texture, TEXTURE_FILTER_POINT);
    }

    texturesReady_ = true;
    return true;
}

void Renderer::Shutdown()
{
    if (!texturesReady_)
    {
        return;
    }

    chunkRenderer_.Shutdown();
    sceneShader_.Shutdown();
    UnloadTexture(grassTexture_);
    UnloadTexture(dirtTexture_);
    UnloadTexture(leafTexture_);
    UnloadTexture(woodTexture_);
    UnloadTexture(teamChestTexture_);
    UnloadTexture(woolTexture_);
    UnloadTexture(stoneTexture_);
    UnloadTexture(smoothStoneTexture_);
    UnloadTexture(darkBrickTexture_);
    UnloadTexture(lightBrickTexture_);
    UnloadTexture(metalBlockTexture_);
    UnloadTexture(glowBlockTexture_);
    UnloadTexture(plankVariantTexture_);
    UnloadTexture(decorativeTileTexture_);
    UnloadTexture(trimBlockTexture_);
    UnloadTexture(cobblestoneTexture_);
    UnloadTexture(andesiteTexture_);
    UnloadTexture(polishedAndesiteTexture_);
    UnloadTexture(stoneBrickTexture_);
    UnloadTexture(chiseledStoneBrickTexture_);
    UnloadTexture(birchPlankTexture_);
    UnloadTexture(coloredGlassTexture_);
    UnloadTexture(coloredClayTexture_);
    UnloadTexture(lapisTexture_);
    UnloadTexture(diamondTexture_);
    UnloadTexture(emeraldTexture_);
    UnloadTexture(goldTexture_);
    UnloadTexture(ironBarsTexture_);
    UnloadTexture(ladderTexture_);
    UnloadTexture(torchTexture_);
    UnloadTexture(obsidianTexture_);
    UnloadTexture(glassTexture_);
    UnloadTexture(springTexture_);
    UnloadTexture(stickyTexture_);
    UnloadTexture(tntTexture_);
    UnloadTexture(spikeTexture_);
    UnloadTexture(lavaTexture_);
    UnloadTexture(iceTexture_);
    UnloadTexture(swordIcon_);
    UnloadTexture(axeIcon_);
    UnloadTexture(spearIcon_);
    UnloadTexture(pickaxeIcon_);
    UnloadTexture(arrowIcon_);
    UnloadTexture(fireballIcon_);
    UnloadTexture(medKitIcon_);
    UnloadTexture(homeIcon_);
    UnloadTexture(dashIcon_);
    UnloadTexture(molotovIcon_);
    UnloadTexture(alarmIcon_);
    UnloadTexture(ironIcon_);
    UnloadTexture(goldIcon_);
    UnloadTexture(crystalIcon_);
    for (Texture2D& normalTexture : blockNormalTextures_)
    {
        if (normalTexture.id != 0)
        {
            UnloadTexture(normalTexture);
            normalTexture = {};
        }
    }
    if (flatNormalTexture_.id != 0)
    {
        UnloadTexture(flatNormalTexture_);
        flatNormalTexture_ = {};
    }
    UnloadShadowTargets();
    heroVisuals_.Shutdown();
    if (heroPreviewTarget_.id != 0)
    {
        UnloadRenderTexture(heroPreviewTarget_);
        heroPreviewTarget_ = {};
    }
    texturesReady_ = false;
}

void Renderer::SetWorldRenderDistance(float distance)
{
    worldRenderDistance_ = std::clamp(distance, 48.0f, 240.0f);
}

void Renderer::SetShadowQuality(int quality)
{
    shadowQuality_ = std::clamp(quality, 0, 2);
    if (shadowQuality_ == 0)
    {
        UnloadShadowTargets();
    }
}

void Renderer::SetAmbientOcclusionQuality(int quality)
{
    ambientOcclusionQuality_ = std::clamp(quality, 0, 2);
}

void Renderer::SyncChunks(const World& world, const std::vector<Team>& teams) const
{
    chunkRenderer_.Sync(
        world,
        [this, &teams](const Block& block) { return GetBlockColor(block, teams); },
        [this](BlockType type) { return GetBlockTexture(type); },
        [](const Block& block, Color color) { return IsTransparentBlock(block.type, color); },
        [this](BlockType type) { return GetBlockNormalTexture(type); },
        // Without the lighting shader the vertex alpha reaches the default
        // pipeline as real transparency, so AO may only be baked when the
        // scene shader is live.
        sceneShader_.IsReady());
}

void Renderer::UpdateMapPointLights(const World& world) const
{
    const std::uint64_t revision = world.GetRenderRevision();
    if (mapPointLightsRevision_ == revision)
    {
        return;
    }

    mapPointLights_.clear();
    // The same full-block scan also caches the world bounding box; the fog
    // walls and void planes size themselves from it instead of assuming the
    // stock arena footprint.
    worldBoundsValid_ = false;
    for (const auto& [pos, block] : world.GetBlocks())
    {
        const Vector3 blockCenter = world.GridToWorld(pos);
        if (!worldBoundsValid_)
        {
            worldBoundsMin_ = blockCenter;
            worldBoundsMax_ = blockCenter;
            worldBoundsValid_ = true;
        }
        else
        {
            worldBoundsMin_.x = std::min(worldBoundsMin_.x, blockCenter.x);
            worldBoundsMin_.y = std::min(worldBoundsMin_.y, blockCenter.y);
            worldBoundsMin_.z = std::min(worldBoundsMin_.z, blockCenter.z);
            worldBoundsMax_.x = std::max(worldBoundsMax_.x, blockCenter.x);
            worldBoundsMax_.y = std::max(worldBoundsMax_.y, blockCenter.y);
            worldBoundsMax_.z = std::max(worldBoundsMax_.z, blockCenter.z);
        }
        if (block.type != BlockType::TorchBlock)
        {
            continue;
        }
        // Wall torches (legacy Minecraft data 1..4) hang off the wall plane;
        // the light source sits at the flame tip, not the cell center.
        float flameX = 0.0f;
        float flameZ = 0.0f;
        float flameY = 0.27f;
        switch (block.variant & 0x07)
        {
        case 1: flameX = -0.14f; flameY = 0.34f; break;
        case 2: flameX = 0.14f; flameY = 0.34f; break;
        case 3: flameZ = -0.14f; flameY = 0.34f; break;
        case 4: flameZ = 0.14f; flameY = 0.34f; break;
        default: break;
        }
        const Vector3 center = world.GridToWorld(pos);
        mapPointLights_.push_back(ScenePointLight {
            Vector3 { center.x + flameX, center.y + flameY, center.z + flameZ },
            Color { 255, 188, 92, 255 },
            7.5f,
            1.18f
        });
    }
    mapPointLightsRevision_ = revision;
}

void Renderer::SetNearestPointLights(
    const Camera3D& camera,
    const std::vector<ScenePointLight>& dynamicLights) const
{
    std::vector<ScenePointLight> nearest = mapPointLights_;
    nearest.insert(nearest.end(), dynamicLights.begin(), dynamicLights.end());
    std::sort(nearest.begin(), nearest.end(), [&camera](const ScenePointLight& a, const ScenePointLight& b)
    {
        return DistanceSquared(a.position, camera.position) < DistanceSquared(b.position, camera.position);
    });
    constexpr std::size_t kMaxSceneLights = 16;
    if (nearest.size() > kMaxSceneLights)
    {
        nearest.resize(kMaxSceneLights);
    }
    sceneShader_.SetPointLights(nearest);
}

bool Renderer::EnsureShadowTargets(
    const std::array<int, SceneShadowParams::kCascadeCount>& resolutions)
{
    bool matches = true;
    for (int cascade = 0; cascade < SceneShadowParams::kCascadeCount; ++cascade)
    {
        matches = matches
            && shadowTargets_[cascade].id != 0
            && shadowResolutions_[cascade] == resolutions[cascade];
    }
    if (matches)
    {
        return true;
    }
    UnloadShadowTargets();
    if (shadowTargetFailed_)
    {
        // The driver rejected a depth-only framebuffer once; keep the blob
        // fallback instead of hammering FBO creation every frame.
        return false;
    }

    // Depth-only render target, mirroring raylib's shadowmap example: no
    // color attachment, the depth texture is what the lighting shader reads.
    for (int cascade = 0; cascade < SceneShadowParams::kCascadeCount; ++cascade)
    {
        const int resolution = resolutions[cascade];
        RenderTexture2D target {};
        target.id = rlLoadFramebuffer(resolution, resolution);
        if (target.id != 0)
        {
            target.texture.width = resolution;
            target.texture.height = resolution;
            rlEnableFramebuffer(target.id);
            target.depth.id = rlLoadTextureDepth(resolution, resolution, false);
            target.depth.width = resolution;
            target.depth.height = resolution;
            target.depth.format = 19;
            target.depth.mipmaps = 1;
            rlFramebufferAttach(target.id, target.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
            const bool complete = rlFramebufferComplete(target.id);
            rlDisableFramebuffer();
            if (!complete || target.depth.id == 0)
            {
                rlUnloadFramebuffer(target.id);
                target = {};
            }
        }
        if (target.id == 0)
        {
            TraceLog(LOG_WARNING, "SHADOW: cascade %d framebuffer failed; sun shadows disabled", cascade);
            UnloadShadowTargets();
            shadowTargetFailed_ = true;
            return false;
        }
        rlTextureParameters(target.depth.id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
        rlTextureParameters(target.depth.id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
        shadowTargets_[cascade] = target;
        shadowResolutions_[cascade] = resolution;
    }
    return true;
}

void Renderer::UnloadShadowTargets()
{
    for (RenderTexture2D& target : shadowTargets_)
    {
        if (target.id != 0)
        {
            // rlUnloadFramebuffer also deletes the attached depth texture.
            rlUnloadFramebuffer(target.id);
            target = {};
        }
    }
    shadowResolutions_.fill(0);
    sunShadowsValid_ = false;
}

void Renderer::PrepareSunShadows(const World& world, const std::vector<Team>& teams, const Camera3D& camera, const std::vector<Player>& players)
{
    sunShadowsValid_ = false;
    if (shadowQuality_ <= 0 || !texturesReady_ || !sceneShader_.IsReady())
    {
        return;
    }
    // Far shadows cover the draw distance. High also increases far resolution
    // so extending the coverage does not erase block-sized silhouettes.
    const std::array<int, SceneShadowParams::kCascadeCount> resolutions = shadowQuality_ >= 2
        ? std::array<int, SceneShadowParams::kCascadeCount> { 2048, 2048 }
        : std::array<int, SceneShadowParams::kCascadeCount> { 1024, 1024 };
    if (!EnsureShadowTargets(resolutions))
    {
        return;
    }

    SyncChunks(world, teams);
    UpdateMapPointLights(world);

    const Vector3 up { 0.0f, 1.0f, 0.0f };
    const Matrix lightRotation = MatrixLookAt(Vector3 { 0.0f, 0.0f, 0.0f }, kSceneSunDirection, up);
    const Matrix inverseLightRotation = MatrixInvert(lightRotation);
    sunShadowDistance_ = worldRenderDistance_;
    const std::array<float, SceneShadowParams::kCascadeCount> radii {
        shadowQuality_ >= 2 ? 48.0f : 40.0f,
        sunShadowDistance_ / 0.86f
    };
    // Both ends of the blend fit inside the near map's fully covered region.
    sunShadowSplitDistance_ = std::min(radii[0] * 0.65f, sunShadowDistance_ * 0.4f);

    for (int cascade = 0; cascade < SceneShadowParams::kCascadeCount; ++cascade)
    {
        const float radius = radii[cascade];
        Vector3 focus = camera.position;
        // Stabilize each cascade independently in light space. Snapping both
        // axes (including the light-space vertical axis) removes crawling on
        // floors and walls during camera translation and pitch changes.
        const float worldPerTexel = (radius * 2.0f) / static_cast<float>(resolutions[cascade]);
        sunShadowWorldPerTexel_[cascade] = worldPerTexel;
        Vector3 lightSpaceFocus = Vector3Transform(focus, lightRotation);
        lightSpaceFocus.x = std::floor(lightSpaceFocus.x / worldPerTexel + 0.5f) * worldPerTexel;
        lightSpaceFocus.y = std::floor(lightSpaceFocus.y / worldPerTexel + 0.5f) * worldPerTexel;
        focus = Vector3Transform(lightSpaceFocus, inverseLightRotation);

        Camera3D lightCamera {};
        const float lightOffset = radius + 128.0f;
        lightCamera.position = Vector3Subtract(focus, Vector3Scale(kSceneSunDirection, lightOffset));
        lightCamera.target = focus;
        lightCamera.up = up;
        lightCamera.fovy = radius * 2.0f;
        lightCamera.projection = CAMERA_ORTHOGRAPHIC;

        BeginTextureMode(shadowTargets_[cascade]);
        ClearBackground(WHITE);
        BeginMode3D(lightCamera);
        const Matrix lightView = rlGetMatrixModelview();
        const Matrix lightProjection = rlGetMatrixProjection();
        chunkRenderer_.Draw(lightCamera, lightOffset + radius * 2.0f, Shader {});
        // Actors must cast into both cascades, including the first-person player.
        for (const Player& player : players)
        {
            if (!player.IsAlive()) continue;
            const Vector3 position = player.GetPosition();
            if (Vector3Distance(position, focus) > radius + 10.0f) continue;
            const auto& state = player.GetHeroState();
            const HeroId casterHero = player.GetHeroId() == HeroId::Likho && state.ultimate.active
                ? state.likhoDisguiseHeroId : player.GetHeroId();
            const HeroVisualAsset* visual = heroVisuals_.Find(casterHero);
            if (visual != nullptr)
            {
                heroVisuals_.ApplyAnimation(casterHero, state.animationState,
                    HeroAnimationFraction(state), static_cast<float>(GetTime()) + player.GetId() * 0.071f);
                const Vector3 actorForward = player.Forward();
                const float yaw = std::atan2(actorForward.x, actorForward.z) * 180.0f / kPi + visual->yawOffsetDegrees;
                Vector3 scale = visual->scale;
                if (player.IsSneaking()) scale.y *= 0.92f;
                DrawModelEx(visual->model, Vector3Add(position, visual->offset), Vector3 { 0, 1, 0 }, yaw, scale, WHITE);
            }
            else DrawCube(position, 0.6f, 1.8f, 0.6f, WHITE);
        }
        EndMode3D();
        EndTextureMode();
        sunLightViewProj_[cascade] = MatrixMultiply(lightView, lightProjection);
    }
    sunShadowsValid_ = true;
}

const ChunkRenderStats& Renderer::GetChunkRenderStats() const
{
    return chunkRenderer_.GetStats();
}

void Renderer::RenderHeroPreview(HeroId heroId, Rectangle destination, float yawDegrees, bool portrait) const
{
    const HeroVisualAsset* visual = heroVisuals_.Find(heroId);
    if (visual == nullptr || heroPreviewTarget_.id == 0)
    {
        DrawRectangleRec(destination, Fade(Color { 12, 15, 22, 255 }, 0.92f));
        DrawRectangleLinesEx(destination, 1.0f, Fade(WHITE, 0.18f));
        const char* fallback = "model fallback";
        DrawText(fallback,
            static_cast<int>(destination.x + destination.width * 0.5f) - MeasureText(fallback, 14) / 2,
            static_cast<int>(destination.y + destination.height * 0.5f) - 7,
            14,
            Fade(WHITE, 0.56f));
        return;
    }

    const BoundingBox bounds = GetModelBoundingBox(visual->model);
    const Vector3 center {
        (bounds.min.x + bounds.max.x) * 0.5f,
        (bounds.min.y + bounds.max.y) * 0.5f,
        (bounds.min.z + bounds.max.z) * 0.5f
    };
    const float width = std::max(0.2f, bounds.max.x - bounds.min.x);
    const float height = std::max(0.2f, bounds.max.y - bounds.min.y);
    const float depth = std::max(0.2f, bounds.max.z - bounds.min.z);
    const float radius = std::max({ width, height * 0.72f, depth, 1.0f });
    const float yaw = yawDegrees * kPi / 180.0f;
    // Keep the showcase silhouette comfortably inside its panel.  A small
    // preview gets extra breathing room, while larger screens can use more of
    // the stage without the model becoming a wall of pixels.
    const float previewExtent = std::max(1.0f, std::min(destination.width, destination.height));
    const float compactPreview = std::clamp(420.0f / previewExtent, 0.0f, 0.38f);
    const float cameraDistance = radius * (portrait ? 1.68f : 5.20f + compactPreview * 0.60f);
    const Vector3 cameraTarget {
        center.x,
        center.y + (portrait ? height * 0.20f : 0.0f),
        center.z
    };
    Camera3D previewCamera {
        Vector3 { center.x + std::sin(yaw) * cameraDistance,
                  center.y + height * (portrait ? 0.22f : 0.12f),
                  center.z + std::cos(yaw) * cameraDistance },
        cameraTarget,
        Vector3 { 0.0f, 1.0f, 0.0f },
        34.0f,
        CAMERA_PERSPECTIVE
    };

    BeginTextureMode(heroPreviewTarget_);
    ClearBackground(Color { 7, 9, 14, 255 });
    BeginMode3D(previewCamera);
    const Color heroColor = VisualTheme::HeroAccent(heroId);
    if (!portrait)
    {
        const Vector3 podiumCenter { center.x, bounds.min.y - 0.15f, center.z };
        DrawCylinder(podiumCenter, radius * 0.60f, radius * 0.54f, 0.18f, 48,
                     Fade(Color { 38, 42, 52, 255 }, 0.98f));
        DrawCylinderWires(podiumCenter, radius * 0.60f, radius * 0.54f, 0.18f, 48,
                          Fade(heroColor, 0.76f));
        DrawCylinder(Vector3 { center.x, bounds.min.y - 0.055f, center.z },
                     radius * 0.48f, radius * 0.48f, 0.025f, 48, Fade(heroColor, 0.30f));
    }
    heroVisuals_.ApplyAnimation(heroId, HeroAnimationState::Idle, 0.0f, static_cast<float>(GetTime()));
    DrawModel(visual->model, Vector3 { 0.0f, 0.0f, 0.0f }, 1.0f, WHITE);
    if (!portrait)
    {
        DrawCylinderWires(Vector3 { center.x, bounds.min.y - 0.02f, center.z },
                          radius * 0.44f, radius * 0.44f, 0.025f, 48, Fade(heroColor, 0.88f));
    }
    EndMode3D();
    EndTextureMode();

    // The preview render target is square.  Stretching it to the (usually
    // wide) hero stage deforms every model horizontally, so preserve its
    // aspect ratio and letterbox the unused part of the stage instead.
    const float previewSide = std::min(destination.width, destination.height);
    const Rectangle previewDestination {
        destination.x + (destination.width - previewSide) * 0.5f,
        destination.y + (destination.height - previewSide) * 0.5f,
        previewSide,
        previewSide
    };
    DrawRectangleRec(destination, Color { 7, 9, 14, 255 });
    DrawTexturePro(
        heroPreviewTarget_.texture,
        Rectangle { 0.0f, 0.0f, static_cast<float>(heroPreviewTarget_.texture.width), -static_cast<float>(heroPreviewTarget_.texture.height) },
        previewDestination,
        Vector2 { 0.0f, 0.0f },
        0.0f,
        WHITE);
}

void Renderer::RenderScene(
    const World& world,
    const std::vector<Team>& teams,
    const std::vector<EnergyCore>& cores,
    const std::vector<Player>& players,
        const std::vector<Generator>& generators,
        const std::vector<ResourcePickup>& pickups,
        const std::vector<DroppedItem>& droppedItems,
        const std::vector<HeroDeviceVisual>& heroDevices,
        const OrbitaTeleportPreview& orbitaTeleportPreview,
        const PlacementPreview& placementPreview,
    const std::vector<EnergyProjectile>& projectiles,
    const std::vector<TimedExplosion>& explosives,
    const std::vector<WorldEffect>& worldEffects,
    const ParticleSystem& particles,
    const std::vector<FloatingText>& floatingTexts,
    const Camera3D& camera,
    const ItemStack& localHeldItem,
    const FirstPersonMotionPose& firstPersonPose,
    Color skyColor,
    bool hideLocalPlayer) const
{
    int localTeamId = -1;
    const Player* localPlayer = nullptr;
    for (const Player& player : players)
    {
        if (IsLocallyPredicted(player.GetControlKind()))
        {
            localTeamId = player.GetTeamId();
            localPlayer = &player;
            break;
        }
    }
    const bool svidetelContours = localPlayer != nullptr
        && localPlayer->GetHeroId() == HeroId::Svidetel
        && localPlayer->GetHeroState().ultimate.active;
#if DAIBED_DEVELOPER_BUILD
    static bool showHeroDebug = false;
    if (IsKeyPressed(KEY_F8))
    {
        showHeroDebug = !showHeroDebug;
    }
#else
    bool showHeroDebug = false;
#endif

    SyncChunks(world, teams);
    UpdateMapPointLights(world);

    sceneShader_.UpdateVoxelLighting(world, camera,
        [this, &teams](const Block& block) { return GetBlockColor(block, teams); });

    BeginMode3D(camera);

    const bool atmosphericSky = sceneShader_.DrawSky(camera, skyColor);
    DrawSkyVoid(skyColor, camera.position.y);
    for (int i = 0; !atmosphericSky && i < 10; ++i)
    {
        const float x = -44.0f + static_cast<float>((i * 17) % 88);
        const float z = -38.0f + static_cast<float>((i * 29) % 76);
        const float width = 5.5f + static_cast<float>(i % 3) * 2.0f;
        const float depth = 1.4f + static_cast<float>((i + 1) % 3) * 0.8f;
        DrawCube(Vector3 { x, 28.0f + static_cast<float>(i % 2) * 2.4f, z }, width, 0.10f, depth, Fade(WHITE, 0.22f));
        DrawCube(Vector3 { x + width * 0.35f, 28.0f + static_cast<float>(i % 2) * 2.4f, z + depth * 0.65f }, width * 0.55f, 0.10f, depth * 0.85f, Fade(WHITE, 0.16f));
    }

    SceneShadowParams shadowParams {};
    const SceneShadowParams* activeShadow = nullptr;
    if (sunShadowsValid_ && shadowQuality_ > 0
        && shadowTargets_[0].depth.id != 0
        && shadowTargets_[1].depth.id != 0)
    {
        shadowParams.lightViewProj = sunLightViewProj_;
        for (int cascade = 0; cascade < SceneShadowParams::kCascadeCount; ++cascade)
        {
            shadowParams.depthTextureId[cascade] = shadowTargets_[cascade].depth.id;
            shadowParams.texelSize[cascade] = 1.0f / static_cast<float>(shadowResolutions_[cascade]);
            shadowParams.worldPerTexel[cascade] = sunShadowWorldPerTexel_[cascade];
        }
        shadowParams.strength = shadowQuality_ >= 2 ? 0.72f : 0.62f;
        shadowParams.splitDistance = sunShadowSplitDistance_;
        shadowParams.distance = sunShadowDistance_;
        shadowParams.widePcf = shadowQuality_ >= 2;
        activeShadow = &shadowParams;
    }

    std::vector<ScenePointLight> dynamicLights;
    dynamicLights.reserve(cores.size() + generators.size() + heroDevices.size()
        + projectiles.size() + worldEffects.size() + explosives.size());
    for (const EnergyCore& core : cores)
    {
        if (!core.IsAlive()) continue;
        const Team* team = FindTeam(teams, core.GetTeamId());
        const Color color = team != nullptr ? GetTeamColor(team->color) : Color { 112, 232, 255, 255 };
        const Vector3 center = world.GridToWorld(core.GetBlockPosition());
        dynamicLights.push_back(ScenePointLight {
            Vector3 { center.x, center.y + 0.28f, center.z }, color, 7.5f, 1.05f });
    }
    for (const Generator& generator : generators)
    {
        Color color = GetResourceColor(generator.GetType());
        const Vector3 position = ToVector3(generator.GetPosition());
        dynamicLights.push_back(ScenePointLight {
            Vector3 { position.x, position.y + 0.48f, position.z }, color, 4.2f, 0.48f });
    }
    for (const HeroDeviceVisual& device : heroDevices)
    {
        const Team* team = FindTeam(teams, device.teamId);
        Color color = team != nullptr ? GetTeamColor(team->color) : Color { 112, 232, 255, 255 };
        if (device.kind == HeroDeviceVisualKind::KonvoyTrap
            || device.kind == HeroDeviceVisualKind::KonvoyTether)
        {
            color = Color { 255, 204, 92, 255 };
        }
        dynamicLights.push_back(ScenePointLight {
            Vector3 { device.position.x, device.position.y + 0.38f, device.position.z },
            color, device.active ? 6.2f : 4.0f, device.active ? 0.92f : 0.46f });
    }
    for (const EnergyProjectile& projectile : projectiles)
    {
        if (projectile.kind == ProjectileKind::Fireball || projectile.kind == ProjectileKind::Molotov)
        {
            dynamicLights.push_back(ScenePointLight { projectile.position,
                projectile.blueFire ? Color { 86, 176, 255, 255 } : Color { 255, 112, 52, 255 },
                5.5f, 1.05f });
        }
        else if (projectile.kind == ProjectileKind::Blaster)
        {
            dynamicLights.push_back(ScenePointLight {
                projectile.position, Color { 92, 238, 255, 255 }, 4.0f, 0.82f });
        }
    }
    for (const TimedExplosion& explosive : explosives)
    {
        dynamicLights.push_back(ScenePointLight {
            explosive.position, Color { 255, 126, 56, 255 }, 4.8f,
            explosive.timer < 0.8f ? 1.1f : 0.46f });
    }
    for (const WorldEffect& effect : worldEffects)
    {
        if (effect.kind == WorldEffectKind::FireZone
            || effect.kind == WorldEffectKind::CorePulse
            || effect.color.g > effect.color.r + 20)
        {
            const float remaining = 1.0f - std::clamp(effect.age / std::max(0.01f, effect.lifetime), 0.0f, 1.0f);
            dynamicLights.push_back(ScenePointLight {
                Vector3 { effect.position.x, effect.position.y + 0.25f, effect.position.z },
                effect.color, std::clamp(effect.radius * 2.3f, 3.0f, 8.0f), 0.35f + remaining * 0.78f });
        }
    }
    std::vector<ParticleGlowLight> particleGlowLights;
    particles.AppendGlowLights(particleGlowLights);
    for (const ParticleGlowLight& light : particleGlowLights)
    {
        dynamicLights.push_back(ScenePointLight {
            light.position, light.color, light.radius, light.intensity });
    }
    sceneShader_.Begin(camera, skyColor, 0.0045f, worldRenderDistance_, activeShadow);
    sceneShader_.SetAmbientOcclusionQuality(ambientOcclusionQuality_);
    sceneShader_.SetEmissiveStrength(0.0f);
    SetNearestPointLights(camera, dynamicLights);
    // Baked AO and normal maps exist only on chunk meshes; the gates must
    // close again before players/devices/transparent blocks hit the batch.
    sceneShader_.SetChunkPassFeatures(true);
    chunkRenderer_.Draw(camera, worldRenderDistance_, sceneShader_.GetShader());
    sceneShader_.SetChunkPassFeatures(false);

    for (const Team& team : teams)
    {
        DrawCylinder(team.shopPosition, 1.7f, 1.7f, 0.08f, 24, Fade(GetTeamColor(team.color), 0.35f));
        DrawCylinderWires(team.shopPosition, 1.7f, 1.7f, 0.08f, 24, GetTeamColor(team.color));

        const Block* chestBlock = world.GetBlock(team.teamChestBlock);
        if (chestBlock != nullptr && chestBlock->type == BlockType::TeamChestBlock)
        {
            const Vector3 chestCenter = world.GridToWorld(team.teamChestBlock);
            const Color teamColor = GetTeamColor(team.color);
            DrawCube(Vector3 { chestCenter.x, chestCenter.y + 0.18f, chestCenter.z }, 0.86f, 0.18f, 0.86f, Fade(teamColor, 0.86f));
            DrawCube(Vector3 { chestCenter.x, chestCenter.y + 0.05f, chestCenter.z - 0.47f }, 0.28f, 0.22f, 0.05f, Color { 255, 224, 122, 255 });
            DrawCubeWires(chestCenter, 1.02f, 1.02f, 1.02f, Fade(teamColor, 0.92f));
        }
    }

    for (const Generator& generator : generators)
    {
        const Vector3 pos = ToVector3(generator.GetPosition());
        DrawSoftContactShadow(Vector3 { pos.x, pos.y - 0.01f, pos.z }, 0.56f, 0.24f, shadowQuality_);
        sceneShader_.SetEmissiveStrength(0.42f);
        DrawCube(Vector3 { pos.x, pos.y + 0.18f, pos.z }, 0.85f, 0.35f, 0.85f, GetResourceColor(generator.GetType()));
        DrawSphere(Vector3 { pos.x, pos.y + 0.62f, pos.z }, 0.24f, WHITE);
        sceneShader_.SetEmissiveStrength(0.0f);
    }

    for (const EnergyCore& core : cores)
    {
        if (!core.IsAlive())
        {
            continue;
        }

        const Team* team = FindTeam(teams, core.GetTeamId());
        const Color teamColor = team != nullptr ? GetTeamColor(team->color) : WHITE;
        bool radonPrimedForCore = false;
        for (const Player& player : players)
        {
            if (player.GetTeamId() == core.GetTeamId()
                && player.GetHeroId() == HeroId::Radon
                && player.GetHeroState().ultimatePrimed
                && player.IsAlive())
            {
                radonPrimedForCore = true;
                break;
            }
        }
        const Vector3 pos = world.GridToWorld(core.GetBlockPosition());
        const float healthFraction = static_cast<float>(core.GetHealth()) / static_cast<float>(std::max(1, core.GetMaxHealth()));
        const float stageScale = 0.72f + healthFraction * 0.28f;
        const Vector3 coreCenter { pos.x, pos.y - 0.06f, pos.z };
        DrawSoftContactShadow(Vector3 { pos.x, pos.y - 0.49f, pos.z },
            0.72f * stageScale, 0.34f, shadowQuality_);
        sceneShader_.SetEmissiveStrength(1.0f);
        DrawCube(coreCenter, 0.92f * stageScale, 0.78f * stageScale, 0.92f * stageScale, Fade(teamColor, 0.70f + healthFraction * 0.22f));
        DrawSphere(Vector3 { pos.x, pos.y + 0.40f * stageScale, pos.z }, 0.18f + 0.08f * healthFraction, Color { 178, 245, 255, 255 });
        DrawCubeWires(coreCenter, 0.96f * stageScale, 0.82f * stageScale, 0.96f * stageScale, WHITE);
        if (radonPrimedForCore)
        {
            const float pulse = 1.02f + 0.12f * std::sin(static_cast<float>(GetTime()) * 6.0f);
            DrawSphereWires(Vector3 { pos.x, pos.y + 0.18f, pos.z }, pulse, 12, 18, Fade(Color { 92, 164, 255, 255 }, 0.82f));
            DrawCylinderWires(Vector3 { pos.x, pos.y - 0.47f, pos.z }, pulse * 1.18f, pulse * 1.18f, 0.05f, 36, Fade(Color { 178, 245, 255, 255 }, 0.62f));
        }
        const int cracks = healthFraction < 0.70f ? (healthFraction < 0.35f ? 6 : 3) : 0;
        for (int i = 0; i < cracks; ++i)
        {
            const float side = (i % 2 == 0) ? -0.53f * stageScale : 0.53f * stageScale;
            const float z = -0.34f * stageScale + static_cast<float>(i % 3) * 0.34f * stageScale;
            DrawLine3D(
                Vector3 { pos.x + side, pos.y + 0.02f + static_cast<float>(i) * 0.09f, pos.z + z },
                Vector3 { pos.x + side, pos.y + 0.52f + static_cast<float>(i % 2) * 0.12f, pos.z + z + 0.18f },
                Color { 20, 24, 32, 230 });
        }
        sceneShader_.SetEmissiveStrength(0.0f);
    }

    for (const Player& player : players)
    {
        const HeroRuntimeState& renderHeroState = player.GetHeroState();
        const bool dying = !player.IsAlive()
            && (renderHeroState.animationState == HeroAnimationState::Death
                || renderHeroState.animationState == HeroAnimationState::DeathSacrifice)
            && renderHeroState.animationTimer > 0.0f;
        if (!player.IsAlive() && !dying)
        {
            continue;
        }
        if (hideLocalPlayer && IsLocallyPredicted(player.GetControlKind()))
        {
            continue;
        }

        const Team* team = FindTeam(teams, player.GetTeamId());
        const HeroRuntimeState& heroState = player.GetHeroState();
        const Team* disguiseTeam = heroState.ultimate.active && heroState.likhoDisguiseTeamId >= 0
            ? FindTeam(teams, heroState.likhoDisguiseTeamId)
            : nullptr;
        const Color baseColor = disguiseTeam != nullptr
            ? GetTeamColor(disguiseTeam->color)
            : (team != nullptr ? GetTeamColor(team->color) : WHITE);
        Color color = baseColor;
        const bool enemy = localTeamId >= 0 && player.GetTeamId() != localTeamId;
        const Vector3 pos = player.GetPosition();
        if (player.IsOnGround())
        {
            DrawSoftContactShadow(Vector3 { pos.x, pos.y - 0.89f, pos.z },
                0.50f, dying ? 0.15f : 0.34f, shadowQuality_);
        }
        const bool radon = player.GetHeroId() == HeroId::Radon;
        const bool orbita = player.GetHeroId() == HeroId::Orbita;
        const float anim = HeroAnimationFraction(heroState);
        float bodyPulse = 1.0f;
        float bodyLift = 0.0f;
        float shoulderLean = 0.0f;
        if (radon)
        {
            if (heroState.radonProtected)
            {
                color = MixColor(color, Color { 92, 164, 255, 255 }, 0.28f + 0.10f * std::sin(static_cast<float>(GetTime()) * 5.0f));
                bodyPulse += 0.025f * std::sin(static_cast<float>(GetTime()) * 6.0f);
            }
            if (heroState.radonOverloaded || heroState.animationState == HeroAnimationState::Overloaded)
            {
                color = MixColor(color, Color { 255, 118, 70, 255 }, 0.34f + 0.12f * std::sin(static_cast<float>(GetTime()) * 9.0f));
                bodyPulse += 0.045f * std::sin(static_cast<float>(GetTime()) * 11.0f);
            }
            if (heroState.ultimatePrimed || heroState.animationState == HeroAnimationState::UltPrimed)
            {
                color = MixColor(color, Color { 178, 245, 255, 255 }, 0.38f);
                bodyPulse += 0.05f * std::sin(static_cast<float>(GetTime()) * 7.0f);
            }
            if (heroState.animationState == HeroAnimationState::WindUp)
            {
                shoulderLean = -0.16f * (1.0f - anim);
                bodyPulse += 0.07f * anim;
            }
            else if (IsAbilityCastAnimation(heroState.animationState))
            {
                shoulderLean = 0.20f * std::sin(anim * 3.14159f);
                bodyPulse += 0.10f * (1.0f - anim);
                bodyLift = 0.04f * std::sin(anim * 3.14159f);
            }
            else if (heroState.animationState == HeroAnimationState::Recovery)
            {
                bodyPulse -= 0.03f * (1.0f - anim);
            }
        }
        if (orbita)
        {
            const float pulseFraction = std::clamp(heroState.orbitaPulseTimer / 3.0f, 0.0f, 1.0f);
            if (pulseFraction > 0.0f || IsAbilityCastAnimation(heroState.animationState))
            {
                color = MixColor(color, Color { 255, 170, 150, 255 }, 0.18f + pulseFraction * 0.18f);
                bodyPulse += 0.04f * std::sin(static_cast<float>(GetTime()) * 12.0f) + pulseFraction * 0.06f;
                bodyLift += 0.05f * std::sin(anim * kPi);
            }
            if (IsAbilityCastAnimation(heroState.animationState))
            {
                shoulderLean = 0.24f * std::sin(anim * kPi);
                bodyPulse += 0.08f * (1.0f - anim * 0.5f);
            }
        }
        if (player.GetHeroId() == HeroId::Likho && heroState.ultimate.active)
        {
            color = MixColor(color, VisualTheme::HeroAccent(HeroId::Likho), 0.18f + 0.10f * std::sin(static_cast<float>(GetTime()) * 13.0f));
        }
        if (svidetelContours && enemy)
        {
            const bool distorted = player.GetHeroId() == HeroId::Likho && heroState.ultimate.active;
            rlDisableDepthTest();
            DrawSphereWires(Vector3 { pos.x, pos.y + 0.25f, pos.z }, 1.15f, 10, 16, Fade(VisualTheme::HeroAccent(HeroId::Svidetel), distorted ? 0.58f : 0.86f));
            if (distorted)
            {
                const float jitter = 0.10f + 0.05f * std::sin(static_cast<float>(GetTime()) * 17.0f);
                DrawSphereWires(Vector3 { pos.x + jitter, pos.y + 0.32f, pos.z - jitter }, 1.08f, 8, 13, Fade(Color { 255, 92, 210, 255 }, 0.48f));
                DrawSphereWires(Vector3 { pos.x - jitter, pos.y + 0.18f, pos.z + jitter }, 1.22f, 7, 11, Fade(VisualTheme::HeroAccent(HeroId::Likho), 0.42f));
            }
            rlEnableDepthTest();
        }
        // Procedural character animation: walk cycle, jump stretch, sneak
        // crouch, hurt flash and attack swing, all derived from gameplay
        // state so bots and the local player read identically.
        const Vector3 velocity = player.GetVelocity();
        const float horizontalSpeed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
        const float walkAmplitude = std::clamp(horizontalSpeed / 6.5f, 0.0f, 1.0f) * (player.IsOnGround() ? 1.0f : 0.25f);
        const float walkPhase = static_cast<float>(GetTime()) * (player.IsSprinting() ? 10.5f : 7.5f)
            + static_cast<float>(player.GetId()) * 1.7f;
        const float swingSin = std::sin(walkPhase) * walkAmplitude;
        const float bobLift = std::fabs(std::sin(walkPhase * 2.0f)) * 0.035f * walkAmplitude;
        const float stretch = player.IsOnGround()
            ? 1.0f
            : std::clamp(1.0f + std::fabs(velocity.y) * 0.016f, 1.0f, 1.12f);
        const bool sneaking = player.IsSneaking();
        const float sneakDrop = sneaking ? 0.14f : 0.0f;
        const float hurtFlash = std::clamp(player.GetInvulnerabilityTimer() / 0.45f, 0.0f, 1.0f);
        if (hurtFlash > 0.0f)
        {
            color = MixColor(color, Color { 255, 84, 84, 255 }, 0.55f * hurtFlash);
        }
        float attackSwing = 0.0f;
        const float attackCdDuration = player.GetAttackCooldownDuration();
        const float attackCdRemaining = player.GetAttackCooldownRemaining();
        if (attackCdDuration > 0.01f && attackCdRemaining > 0.0f)
        {
            attackSwing = 1.0f - std::clamp((attackCdDuration - attackCdRemaining) / 0.26f, 0.0f, 1.0f);
        }

        const Vector3 animForward = player.Forward();
        const float torsoLift = bodyLift + bobLift - sneakDrop;
        const HeroId renderedHeroId = player.GetHeroId() == HeroId::Likho && heroState.ultimate.active
            ? heroState.likhoDisguiseHeroId
            : player.GetHeroId();
        const HeroVisualAsset* visual = heroVisuals_.Find(renderedHeroId);
        if (visual != nullptr)
        {
            const float idleLift = horizontalSpeed < 0.1f
                ? std::sin(static_cast<float>(GetTime()) * (player.GetHeroId() == HeroId::Likho ? 5.5f : 2.8f) + static_cast<float>(player.GetId()))
                    * (player.GetHeroId() == HeroId::Likho || player.GetHeroId() == HeroId::Svidetel ? 0.055f : 0.015f)
                : 0.0f;
            Vector3 modelPosition {
                pos.x + visual->offset.x,
                pos.y + visual->offset.y + torsoLift + idleLift,
                pos.z + visual->offset.z
            };
            Vector3 modelScale {
                visual->scale.x * bodyPulse,
                visual->scale.y * stretch * (sneaking ? 0.92f : 1.0f),
                visual->scale.z * bodyPulse
            };
            if (dying)
            {
                const float collapse = std::clamp(anim, 0.0f, 1.0f);
                modelPosition.y -= collapse * 0.52f;
                modelScale.y *= 1.0f - collapse * 0.72f;
                modelScale.x *= 1.0f + collapse * 0.18f;
                modelScale.z *= 1.0f + collapse * 0.18f;
            }
            Color modelTint = WHITE;
            if (hurtFlash > 0.0f)
            {
                modelTint = MixColor(modelTint, Color { 255, 84, 84, 255 }, 0.48f * hurtFlash);
            }
            if (heroState.radonOverloaded)
            {
                modelTint = MixColor(modelTint, Color { 255, 118, 70, 255 }, 0.22f);
            }
            else if (heroState.ultimatePrimed)
            {
                modelTint = MixColor(modelTint, Color { 178, 245, 255, 255 }, 0.16f);
            }
            const float yaw = std::atan2(animForward.x, animForward.z) * 180.0f / kPi
                + visual->yawOffsetDegrees
                + swingSin * 1.8f;
            heroVisuals_.ApplyAnimation(
                player.GetHeroId(),
                heroState.animationState,
                anim,
                static_cast<float>(GetTime()) + static_cast<float>(player.GetId()) * 0.071f);
            DrawModelEx(visual->model, modelPosition, Vector3 { 0.0f, 1.0f, 0.0f }, yaw, modelScale, modelTint);
        }
        else
        {
            const Vector3 animRight { -animForward.z, 0.0f, animForward.x };
            const Color limbColor = MixColor(color, Color { 20, 22, 30, 255 }, 0.35f);

            // Procedural fallback used when a runtime model is absent or invalid.
            DrawCube(
                Vector3 { pos.x, pos.y + 0.28f + torsoLift, pos.z },
                0.60f * bodyPulse,
                (sneaking ? 0.84f : 0.98f) * stretch * (0.98f + (bodyPulse - 1.0f) * 0.45f),
                0.44f * bodyPulse,
                color);

            const float legSwing = swingSin * 0.30f;
            const float legHeight = (sneaking ? 0.66f : 0.78f) * stretch;
            DrawCube(
                Vector3 { pos.x + animRight.x * 0.17f + animForward.x * legSwing, pos.y - 0.46f, pos.z + animRight.z * 0.17f + animForward.z * legSwing },
                0.20f, legHeight, 0.20f, limbColor);
            DrawCube(
                Vector3 { pos.x - animRight.x * 0.17f - animForward.x * legSwing, pos.y - 0.46f, pos.z - animRight.z * 0.17f - animForward.z * legSwing },
                0.20f, legHeight, 0.20f, limbColor);

            const float armSwing = -swingSin * 0.26f;
            const Vector3 leftArm {
                pos.x - animRight.x * 0.42f - animForward.x * armSwing,
                pos.y + 0.30f + torsoLift,
                pos.z - animRight.z * 0.42f - animForward.z * armSwing
            };
            Vector3 rightArm {
                pos.x + animRight.x * 0.42f + animForward.x * armSwing,
                pos.y + 0.30f + torsoLift,
                pos.z + animRight.z * 0.42f + animForward.z * armSwing
            };
            if (attackSwing > 0.0f)
            {
                rightArm = Vector3 {
                    pos.x + animRight.x * 0.34f + animForward.x * (0.30f + attackSwing * 0.28f),
                    pos.y + 0.38f + attackSwing * 0.22f + torsoLift,
                    pos.z + animRight.z * 0.34f + animForward.z * (0.30f + attackSwing * 0.28f)
                };
            }
            DrawCube(leftArm, 0.16f, 0.60f, 0.16f, limbColor);
            DrawCube(rightArm, 0.16f, 0.60f, 0.16f, limbColor);

            Color headColor = Color { 238, 230, 210, 255 };
            if (radon)
            {
                headColor = MixColor(headColor, Color { 178, 245, 255, 255 }, heroState.ultimatePrimed ? 0.32f : 0.0f);
            }
            else if (orbita)
            {
                headColor = MixColor(headColor, Color { 255, 210, 202, 255 }, std::clamp(heroState.orbitaPulseTimer / 3.0f, 0.0f, 0.45f));
            }
            if (hurtFlash > 0.0f)
            {
                headColor = MixColor(headColor, Color { 255, 84, 84, 255 }, 0.45f * hurtFlash);
            }
            DrawSphere(Vector3 { pos.x, pos.y + 1.08f + torsoLift, pos.z }, 0.36f * ((radon || orbita) ? bodyPulse : 1.0f), headColor);
        }
        if (showHeroDebug)
        {
            const HeroHitboxProfile& hitbox = HeroSystem::GetHitboxProfile(player.GetHeroId());
            const float bottom = pos.y + hitbox.headEnd - hitbox.bodyHeight;
            const float top = pos.y + hitbox.headEnd;
            DrawCylinderWiresEx(
                Vector3 { pos.x, bottom, pos.z },
                Vector3 { pos.x, top, pos.z },
                hitbox.bodyRadius,
                hitbox.bodyRadius,
                16,
                Color { 255, 220, 72, 230 });
            if (visual != nullptr)
            {
                const BoundingBox localBounds = GetModelBoundingBox(visual->model);
                DrawBoundingBox(BoundingBox {
                    Vector3 {
                        pos.x + visual->offset.x + localBounds.min.x * visual->scale.x,
                        pos.y + visual->offset.y + localBounds.min.y * visual->scale.y,
                        pos.z + visual->offset.z + localBounds.min.z * visual->scale.z },
                    Vector3 {
                        pos.x + visual->offset.x + localBounds.max.x * visual->scale.x,
                        pos.y + visual->offset.y + localBounds.max.y * visual->scale.y,
                        pos.z + visual->offset.z + localBounds.max.z * visual->scale.z }
                }, Color { 104, 238, 255, 210 });
            }
        }
        if (dying)
        {
            continue;
        }
        if (enemy)
        {
            DrawCylinderWires(Vector3 { pos.x, pos.y - 0.84f, pos.z }, 0.62f, 0.62f, 0.04f, 24, Color { 255, 118, 118, 220 });
        }
        if (player.HasShield())
        {
            DrawSphereWires(Vector3 { pos.x, pos.y + 0.26f, pos.z }, 1.02f, 10, 12, Color { 112, 232, 255, 180 });
        }

        const Vector3 forward = player.Forward();
        const Vector3 right { -forward.z, 0.0f, forward.x };
        if (radon)
        {
            const Color radonColor = heroState.radonOverloaded ? Color { 255, 118, 70, 255 } : Color { 92, 164, 255, 255 };
            if (heroState.radonProtected || heroState.ultimatePrimed)
            {
                const float ringRadius = heroState.ultimatePrimed ? 1.10f + 0.10f * std::sin(static_cast<float>(GetTime()) * 7.0f) : 0.86f;
                DrawSphereWires(Vector3 { pos.x, pos.y + 0.22f, pos.z }, ringRadius, 10, 14, Fade(radonColor, heroState.ultimatePrimed ? 0.80f : 0.36f));
            }
            if (heroState.radonOverloaded)
            {
                DrawSphereWires(Vector3 { pos.x, pos.y + 0.38f, pos.z }, 0.78f + 0.08f * std::sin(static_cast<float>(GetTime()) * 12.0f), 8, 12, Fade(Color { 255, 118, 70, 255 }, 0.58f));
            }
            if (heroState.animationState == HeroAnimationState::WindUp || IsAbilityCastAnimation(heroState.animationState))
            {
                const float glow = heroState.animationState == HeroAnimationState::WindUp ? anim : 1.0f - anim * 0.55f;
                const Vector3 leftHand { pos.x - right.x * 0.48f + forward.x * (0.16f + shoulderLean), pos.y + 0.26f + bodyLift, pos.z - right.z * 0.48f + forward.z * (0.16f + shoulderLean) };
                const Vector3 rightHand { pos.x + right.x * 0.48f + forward.x * (0.16f + shoulderLean), pos.y + 0.26f + bodyLift, pos.z + right.z * 0.48f + forward.z * (0.16f + shoulderLean) };
                DrawSphere(leftHand, 0.13f + glow * 0.08f, Fade(radonColor, 0.70f));
                DrawSphere(rightHand, 0.13f + glow * 0.08f, Fade(radonColor, 0.70f));
                DrawLine3D(leftHand, Vector3 { leftHand.x + forward.x * (0.6f + glow), leftHand.y, leftHand.z + forward.z * (0.6f + glow) }, Fade(radonColor, 0.70f));
                DrawLine3D(rightHand, Vector3 { rightHand.x + forward.x * (0.6f + glow), rightHand.y, rightHand.z + forward.z * (0.6f + glow) }, Fade(radonColor, 0.70f));
            }
        }
        if (orbita)
        {
            const Color orbitaColor = VisualTheme::HeroAccent(HeroId::Orbita);
            const float pulseFraction = std::clamp(heroState.orbitaPulseTimer / 3.0f, 0.0f, 1.0f);
            if (pulseFraction > 0.0f || IsAbilityCastAnimation(heroState.animationState))
            {
                const float ringRadius = 0.78f + pulseFraction * 0.34f + 0.06f * std::sin(static_cast<float>(GetTime()) * 11.0f);
                DrawCylinderWires(Vector3 { pos.x, pos.y - 0.70f, pos.z }, ringRadius, ringRadius, 0.055f, 36, Fade(WHITE, 0.58f + pulseFraction * 0.20f));
                DrawSphereWires(Vector3 { pos.x, pos.y + 0.28f + bodyLift, pos.z }, 0.72f + pulseFraction * 0.20f, 8, 12, Fade(orbitaColor, 0.34f + pulseFraction * 0.34f));
            }
            if (IsAbilityCastAnimation(heroState.animationState))
            {
                const float glow = 1.0f - anim * 0.45f;
                const Vector3 leftHand { pos.x - right.x * 0.46f + forward.x * (0.12f + shoulderLean), pos.y + 0.28f + bodyLift, pos.z - right.z * 0.46f + forward.z * (0.12f + shoulderLean) };
                const Vector3 rightHand { pos.x + right.x * 0.46f + forward.x * (0.12f + shoulderLean), pos.y + 0.28f + bodyLift, pos.z + right.z * 0.46f + forward.z * (0.12f + shoulderLean) };
                DrawSphere(leftHand, 0.12f + glow * 0.07f, Fade(WHITE, 0.80f));
                DrawSphere(rightHand, 0.12f + glow * 0.07f, Fade(orbitaColor, 0.76f));
                DrawLine3D(leftHand, Vector3 { leftHand.x + forward.x * (0.7f + glow * 0.35f), leftHand.y + 0.05f, leftHand.z + forward.z * (0.7f + glow * 0.35f) }, Fade(WHITE, 0.72f));
                DrawLine3D(rightHand, Vector3 { rightHand.x + forward.x * (0.7f + glow * 0.35f), rightHand.y - 0.04f, rightHand.z + forward.z * (0.7f + glow * 0.35f) }, Fade(orbitaColor, 0.66f));
            }
        }
        const ItemStack heldItem = VisibleHeldItemForPlayer(player, localHeldItem);
        if (!heldItem.IsEmpty())
        {
            const std::optional<BlockType> block = ItemToBlock(heldItem.type);
            const Texture2D* blockTexture = block.has_value() ? GetBlockTexture(*block) : nullptr;
            Color itemTint = ItemUiColor(heldItem.type);
            float rangedChargeFraction = 0.0f;
            if (heldItem.type == ItemType::Bow)
            {
                rangedChargeFraction = BowDrawPower(player.GetBowDrawTimer());
            }
            else if (ItemIsBlasterWeapon(heldItem.type))
            {
                const float required = BlasterChargeSeconds(player.GetInventory().GetBlasterRapidFireLevel());
                rangedChargeFraction = player.GetBlasterState() == CrossbowState::Loaded
                    ? 1.0f
                    : std::clamp(player.GetBlasterLoadTimer() / required, 0.0f, 1.0f);
                itemTint = player.GetBlasterState() == CrossbowState::Loaded
                    ? Color { 104, 255, 128, 255 }
                    : (rangedChargeFraction >= 0.5f ? Color { 255, 220, 72, 255 } : Color { 255, 82, 68, 255 });
            }
            if (block.has_value())
            {
                itemTint = GetBlockColor(Block { *block, player.GetTeamId(), true }, teams);
            }
            // The held item follows the attack swing: forward lunge plus a
            // downward tilt right after the strike.
            const Vector3 swingForward {
                forward.x,
                -attackSwing * 0.85f,
                forward.z
            };
            DrawHeldItemModel(
                heldItem,
                Vector3 {
                    pos.x + right.x * 0.42f + forward.x * (0.14f + attackSwing * 0.40f),
                    pos.y + 0.36f + attackSwing * 0.16f,
                    pos.z + right.z * 0.42f + forward.z * (0.14f + attackSwing * 0.40f)
                },
                attackSwing > 0.0f ? swingForward : forward,
                right,
                Vector3 { 0.0f, 1.0f, 0.0f },
                0.72f,
                GetItemTexture(heldItem.type),
                blockTexture,
                itemTint,
                false,
                rangedChargeFraction);
        }
        if (IsLocallyPredicted(player.GetControlKind()))
        {
            const float range = CombatSystem::AttackRangeForSword(player.GetInventory().GetSwordLevel());
            const Color reachColor = player.GetAttackCooldownRemaining() <= 0.0f
                ? Color { 255, 224, 122, 170 }
                : Color { 150, 150, 160, 110 };
            const Vector3 start { pos.x, pos.y + 0.78f, pos.z };
            DrawLine3D(start, Vector3 { pos.x + forward.x * range, start.y, pos.z + forward.z * range }, reachColor);
        }
    }

    if (hideLocalPlayer && !localHeldItem.IsEmpty())
    {
        const Vector3 handForward = Normalize(Vector3 {
            camera.target.x - camera.position.x,
            camera.target.y - camera.position.y,
            camera.target.z - camera.position.z
        });
        const Vector3 baseRight = Normalize(Cross(handForward, camera.up));
        const Vector3 baseUp = Normalize(Cross(baseRight, handForward));
        const float roll = firstPersonPose.viewmodelRollDegrees * DEG2RAD;
        const Vector3 cameraRight = Add(Scale(baseRight, std::cos(roll)), Scale(baseUp, std::sin(roll)));
        const Vector3 cameraUp = Add(Scale(baseUp, std::cos(roll)), Scale(baseRight, -std::sin(roll)));
        const Vector3 hand = Add(
            Add(camera.position, Scale(handForward, 0.52f + firstPersonPose.viewmodelOffset.z)),
            Add(
                Scale(cameraRight, 0.44f + firstPersonPose.viewmodelOffset.x),
                Scale(cameraUp, -0.34f + firstPersonPose.viewmodelOffset.y)));
        const std::optional<BlockType> block = ItemToBlock(localHeldItem.type);
        const Texture2D* blockTexture = block.has_value() ? GetBlockTexture(*block) : nullptr;
        Color itemTint = ItemUiColor(localHeldItem.type);
        float rangedChargeFraction = 0.0f;
        if (localPlayer != nullptr && localHeldItem.type == ItemType::Bow)
        {
            rangedChargeFraction = BowDrawPower(localPlayer->GetBowDrawTimer());
        }
        else if (localPlayer != nullptr && ItemIsBlasterWeapon(localHeldItem.type))
        {
            const float required = BlasterChargeSeconds(localPlayer->GetInventory().GetBlasterRapidFireLevel());
            rangedChargeFraction = localPlayer->GetBlasterState() == CrossbowState::Loaded
                ? 1.0f
                : std::clamp(localPlayer->GetBlasterLoadTimer() / required, 0.0f, 1.0f);
            itemTint = localPlayer->GetBlasterState() == CrossbowState::Loaded
                ? Color { 104, 255, 128, 255 }
                : (rangedChargeFraction >= 0.5f ? Color { 255, 220, 72, 255 } : Color { 255, 82, 68, 255 });
        }
        if (block.has_value() && localTeamId >= 0)
        {
            itemTint = GetBlockColor(Block { *block, localTeamId, true }, teams);
        }
        const float firstPersonScale = ItemIsBlock(localHeldItem.type) ? 0.54f : 0.66f;
        DrawHeldItemModel(localHeldItem, hand, handForward, cameraRight, cameraUp, firstPersonScale, GetItemTexture(localHeldItem.type), blockTexture, itemTint, true, rangedChargeFraction);
    }
    if (hideLocalPlayer
        && localPlayer != nullptr
        && localPlayer->GetHeroId() == HeroId::Radon)
    {
        const HeroRuntimeState& heroState = localPlayer->GetHeroState();
        if (heroState.animationState == HeroAnimationState::WindUp
            || IsAbilityCastAnimation(heroState.animationState)
            || heroState.ultimatePrimed
            || heroState.radonOverloaded)
        {
            const Vector3 heroForward = Normalize(Vector3 {
                camera.target.x - camera.position.x,
                camera.target.y - camera.position.y,
                camera.target.z - camera.position.z
            });
            const Vector3 cameraRight = Normalize(Cross(heroForward, camera.up));
            const Vector3 cameraUp = Normalize(Cross(cameraRight, heroForward));
            const float anim = HeroAnimationFraction(heroState);
            const Color radonColor = heroState.radonOverloaded ? Color { 255, 118, 70, 255 } : Color { 92, 164, 255, 255 };
            const Vector3 motionOffset = Add(
                Scale(cameraRight, firstPersonPose.viewmodelOffset.x),
                Add(Scale(cameraUp, firstPersonPose.viewmodelOffset.y), Scale(heroForward, firstPersonPose.viewmodelOffset.z)));
            const Vector3 leftHand = Add(Add(Add(camera.position, motionOffset), Scale(heroForward, 0.58f + anim * 0.12f)), Add(Scale(cameraRight, -0.32f), Scale(cameraUp, -0.30f)));
            const Vector3 rightHand = Add(Add(Add(camera.position, motionOffset), Scale(heroForward, 0.58f + anim * 0.12f)), Add(Scale(cameraRight, 0.32f), Scale(cameraUp, -0.30f)));
            const float pulse = 0.09f + 0.05f * std::sin(static_cast<float>(GetTime()) * 12.0f);
            DrawSphere(leftHand, pulse, Fade(radonColor, 0.62f));
            DrawSphere(rightHand, pulse, Fade(radonColor, 0.62f));
            DrawLine3D(leftHand, Add(leftHand, Scale(heroForward, 0.55f + anim * 0.35f)), Fade(radonColor, 0.74f));
            DrawLine3D(rightHand, Add(rightHand, Scale(heroForward, 0.55f + anim * 0.35f)), Fade(radonColor, 0.74f));
        }
    }
    if (hideLocalPlayer
        && localPlayer != nullptr
        && localPlayer->GetHeroId() == HeroId::Orbita)
    {
        const HeroRuntimeState& heroState = localPlayer->GetHeroState();
        if (IsAbilityCastAnimation(heroState.animationState) || heroState.orbitaPulseTimer > 0.0f)
        {
            const Vector3 heroForward = Normalize(Vector3 {
                camera.target.x - camera.position.x,
                camera.target.y - camera.position.y,
                camera.target.z - camera.position.z
            });
            const Vector3 cameraRight = Normalize(Cross(heroForward, camera.up));
            const Vector3 cameraUp = Normalize(Cross(cameraRight, heroForward));
            const float anim = HeroAnimationFraction(heroState);
            const float pulse = std::clamp(heroState.orbitaPulseTimer / 3.0f, 0.0f, 1.0f);
            const Color orbitaColor = VisualTheme::HeroAccent(HeroId::Orbita);
            const Vector3 motionOffset = Add(
                Scale(cameraRight, firstPersonPose.viewmodelOffset.x),
                Add(Scale(cameraUp, firstPersonPose.viewmodelOffset.y), Scale(heroForward, firstPersonPose.viewmodelOffset.z)));
            const Vector3 leftHand = Add(Add(Add(camera.position, motionOffset), Scale(heroForward, 0.62f + anim * 0.10f)), Add(Scale(cameraRight, -0.34f), Scale(cameraUp, -0.30f)));
            const Vector3 rightHand = Add(Add(Add(camera.position, motionOffset), Scale(heroForward, 0.62f + anim * 0.10f)), Add(Scale(cameraRight, 0.34f), Scale(cameraUp, -0.30f)));
            DrawSphere(leftHand, 0.08f + pulse * 0.06f, Fade(WHITE, 0.72f));
            DrawSphere(rightHand, 0.08f + pulse * 0.06f, Fade(orbitaColor, 0.70f));
            DrawLine3D(leftHand, Add(leftHand, Scale(heroForward, 0.72f + pulse * 0.36f)), Fade(WHITE, 0.74f));
            DrawLine3D(rightHand, Add(rightHand, Scale(heroForward, 0.72f + pulse * 0.36f)), Fade(orbitaColor, 0.64f));
        }
    }

    for (const HeroDeviceVisual& device : heroDevices)
    {
        DrawSoftContactShadow(Vector3 { device.position.x, device.position.y - 0.02f, device.position.z },
            std::clamp(device.radius * 0.42f, 0.34f, 0.90f), 0.30f, shadowQuality_);
        sceneShader_.SetEmissiveStrength(device.active ? 0.82f : 0.44f);
        DrawHeroDeviceVisual(device, localTeamId);
        sceneShader_.SetEmissiveStrength(0.0f);
    }
    DrawOrbitaTeleportPreview(orbitaTeleportPreview);

    for (const ResourcePickup& pickup : pickups)
    {
        if (pickup.collected)
        {
            continue;
        }

        const float bob = std::sin(pickup.age * 4.0f) * 0.08f;
        const Vector3 pos { pickup.position.x, pickup.position.y + bob, pickup.position.z };
        if (const Texture2D* texture = GetItemTexture(ItemFromResource(pickup.type)))
        {
            DrawDepthTestedBillboard(camera, *texture, pos, 0.46f, WHITE);
        }
        else
        {
            DrawCube(pos, 0.32f, 0.32f, 0.32f, GetResourceColor(pickup.type));
            DrawCubeWires(pos, 0.34f, 0.34f, 0.34f, WHITE);
        }
    }

    for (const DroppedItem& dropped : droppedItems)
    {
        if (dropped.collected || dropped.stack.IsEmpty())
        {
            continue;
        }

        const float bob = std::sin(dropped.age * 4.4f) * 0.06f;
        const Vector3 pos { dropped.position.x, dropped.position.y + bob, dropped.position.z };
        Color itemColor = ItemUiColor(dropped.stack.type);
        if (const std::optional<BlockType> block = ItemToBlock(dropped.stack.type))
        {
            itemColor = GetBlockColor(Block { *block, -1, true }, teams);
        }
        if (const std::optional<ResourceType> resource = ItemToResource(dropped.stack.type))
        {
            itemColor = GetResourceColor(*resource);
        }
        if (const Texture2D* texture = GetItemTexture(dropped.stack.type))
        {
            DrawDepthTestedBillboard(camera, *texture, pos, 0.46f, WHITE);
        }
        else
        {
            DrawCube(pos, 0.28f, 0.28f, 0.28f, itemColor);
            DrawCubeWires(pos, 0.32f, 0.32f, 0.32f, WHITE);
        }
    }

    for (const TimedExplosion& explosive : explosives)
    {
        DrawSoftContactShadow(Vector3 { explosive.position.x, explosive.position.y - 0.47f, explosive.position.z },
            0.58f, 0.30f, shadowQuality_);
        const bool flashing = explosive.timer < 0.8f;
        const float flash = flashing ? 0.58f + 0.42f * std::sin(static_cast<float>(GetTime()) * 20.0f) : 1.0f;
        sceneShader_.SetEmissiveStrength(flashing ? 0.72f : 0.12f);
        DrawCube(explosive.position, 0.90f, 0.90f, 0.90f, Fade(Color { 212, 52, 44, 255 }, flash));
        DrawCube(Vector3 { explosive.position.x, explosive.position.y, explosive.position.z - 0.456f },
            0.68f, 0.18f, 0.012f, Fade(WHITE, flash));
        DrawCubeWires(explosive.position, 0.94f, 0.94f, 0.94f,
            flashing ? Fade(Color { 255, 224, 122, 255 }, flash) : Fade(BLACK, 0.68f));
        sceneShader_.SetEmissiveStrength(0.0f);
    }

    if (placementPreview.visible && placementPreview.valid)
    {
        const Vector3 previewCenter = world.GridToWorld(placementPreview.position);
        DrawCubeWires(previewCenter, 1.04f, 1.04f, 1.04f, BLACK);
    }

    sceneShader_.SetEmissiveStrength(0.92f);
    for (const WorldEffect& effect : worldEffects)
    {
        DrawRadonPresentationEffect(effect);
    }
    sceneShader_.SetEmissiveStrength(0.0f);
    particles.Draw(false);
    sceneShader_.SetEmissiveStrength(0.90f);
    particles.Draw(true);
    sceneShader_.SetEmissiveStrength(0.0f);

    rlDrawRenderBatchActive();
    rlEnableDepthTest();
    rlDisableDepthMask();
    chunkRenderer_.DrawTransparent(camera, worldRenderDistance_, sceneShader_.GetShader());
    rlDrawRenderBatchActive();
    rlEnableDepthMask();

    rlDrawRenderBatchActive();
    rlEnableDepthTest();
    DrawDistantFog(skyColor, worldBoundsMin_, worldBoundsMax_, worldBoundsValid_);

    // Projectiles are real geometry, not only short-lived particles. This
    // keeps fast arrows and energy bolts readable even at high frame rates.
    for (const EnergyProjectile& projectile : projectiles)
    {
        sceneShader_.SetEmissiveStrength(
            projectile.kind == ProjectileKind::Arrow ? (projectile.critical ? 0.28f : 0.0f) : 0.95f);
        const Vector3 direction = Normalize(projectile.velocity);
        if (projectile.kind == ProjectileKind::Arrow)
        {
            const Color shaftColor = projectile.arrowVariant == ArrowVariant::Breacher
                ? Color { 255, 194, 86, 255 }
                : (projectile.arrowVariant == ArrowVariant::Impulse
                    ? Color { 186, 126, 255, 255 }
                    : Color { 206, 245, 255, 255 });
            const Color headColor = projectile.arrowVariant == ArrowVariant::Breacher
                ? Color { 255, 232, 154, 255 }
                : (projectile.arrowVariant == ArrowVariant::Impulse
                    ? Color { 228, 194, 255, 255 }
                    : Color { 230, 255, 255, 255 });
            const Vector3 tail = Add(projectile.position, Scale(direction, -2.20f));
            DrawCylinderEx(tail, projectile.position, 0.070f, 0.025f, 8, shaftColor);
            DrawSphere(projectile.position, 0.13f, headColor);
            DrawLine3D(Add(tail, Vector3 { 0.0f, 0.12f, 0.0f }), projectile.position, shaftColor);
            if (projectile.critical)
            {
                DrawSphereWires(projectile.position, 0.18f, 5, 7, Color { 255, 226, 96, 235 });
            }
        }
        else if (projectile.kind == ProjectileKind::Blaster)
        {
            const Vector3 tail = Add(projectile.position, Scale(direction, -3.20f));
            DrawCylinderEx(tail, projectile.position, 0.25f, 0.11f, 12, Color { 98, 245, 255, 255 });
            DrawSphere(projectile.position, 0.36f, Color { 220, 255, 255, 255 });
            DrawSphereWires(projectile.position, 0.52f, 8, 12, Fade(Color { 104, 255, 128, 255 }, 0.92f));
            DrawLine3D(Add(tail, Vector3 { 0.06f, 0.0f, 0.0f }), projectile.position, Fade(WHITE, 0.76f));
            DrawLine3D(Add(tail, Vector3 { -0.06f, 0.0f, 0.0f }), projectile.position, Fade(Color { 98, 245, 255, 255 }, 0.72f));
        }
        if (showHeroDebug)
        {
            DrawLine3D(projectile.previousPosition, projectile.position, Color { 255, 80, 220, 255 });
            DrawSphereWires(projectile.position, projectile.radius + 0.08f, 6, 8, Color { 255, 80, 220, 220 });
            Vector3 predicted = projectile.position;
            Vector3 velocity = projectile.velocity;
            for (int step = 0; step < 18; ++step)
            {
                const Vector3 next {
                    predicted.x + velocity.x * 0.05f,
                    predicted.y + velocity.y * 0.05f,
                    predicted.z + velocity.z * 0.05f };
                DrawLine3D(predicted, next, Fade(Color { 255, 226, 96, 255 }, 0.58f));
                predicted = next;
                velocity.y -= projectile.gravity * 0.05f;
            }
        }
    }

    sceneShader_.SetEmissiveStrength(0.0f);

    sceneShader_.End();
    EndMode3D();

    // A screen-space glow guarantees that very fast projectiles remain
    // readable instead of shrinking below one pixel at combat distance.
    for (const EnergyProjectile& projectile : projectiles)
    {
        if (projectile.kind != ProjectileKind::Arrow && projectile.kind != ProjectileKind::Blaster)
        {
            continue;
        }
        if (!IsInFrontOfCamera(projectile.position, camera))
        {
            continue;
        }
        const Vector3 direction = Normalize(projectile.velocity);
        const Vector3 tracerTail = Add(projectile.position, Scale(
            direction,
            projectile.kind == ProjectileKind::Blaster ? -3.2f : -2.2f));
        const Vector2 head = GetWorldToScreen(projectile.position, camera);
        const Vector2 tail = GetWorldToScreen(tracerTail, camera);
        if (head.x < -32.0f || head.x > static_cast<float>(GetScreenWidth()) + 32.0f
            || head.y < -32.0f || head.y > static_cast<float>(GetScreenHeight()) + 32.0f)
        {
            continue;
        }
        const bool blaster = projectile.kind == ProjectileKind::Blaster;
        const Color glow = blaster
            ? Color { 98, 245, 255, 255 }
            : (projectile.arrowVariant == ArrowVariant::Breacher
                ? Color { 255, 194, 86, 255 }
                : (projectile.arrowVariant == ArrowVariant::Impulse
                    ? Color { 186, 126, 255, 255 }
                    : Color { 178, 245, 255, 255 }));
        DrawLineEx(tail, head, blaster ? 7.0f : 4.0f, Fade(glow, 0.34f));
        DrawLineEx(tail, head, blaster ? 3.0f : 1.7f, glow);
        DrawCircleV(head, blaster ? 7.0f : 4.0f, Fade(WHITE, 0.92f));
        DrawCircleLines(static_cast<int>(head.x), static_cast<int>(head.y), blaster ? 10.0f : 6.0f, glow);
    }

    for (const FloatingText& text : floatingTexts)
    {
        const float t = 1.0f - std::clamp(text.age / std::max(0.001f, text.lifetime), 0.0f, 1.0f);
        const Vector3 lifted {
            text.position.x,
            text.position.y + text.age * 1.1f,
            text.position.z
        };
        const Vector2 screen = GetWorldToScreen(lifted, camera);
        if (screen.x >= -80.0f && screen.x <= static_cast<float>(GetScreenWidth()) + 80.0f
            && screen.y >= -40.0f && screen.y <= static_cast<float>(GetScreenHeight()) + 40.0f)
        {
            DrawText(text.text.c_str(), static_cast<int>(screen.x), static_cast<int>(screen.y), 18, Fade(text.color, t));
        }
    }

    for (const Player& player : players)
    {
        if (!player.IsAlive() || IsLocallyPredicted(player.GetControlKind()))
        {
            continue;
        }

        const Vector3 labelPoint {
            player.GetPosition().x,
            player.GetPosition().y + 1.85f,
            player.GetPosition().z
        };
        if (!IsInFrontOfCamera(labelPoint, camera))
        {
            continue;
        }
        const Vector3 toLabel {
            labelPoint.x - camera.position.x,
            labelPoint.y - camera.position.y,
            labelPoint.z - camera.position.z
        };
        const float labelDistance = std::sqrt(toLabel.x * toLabel.x + toLabel.y * toLabel.y + toLabel.z * toLabel.z);
        const Vector3 labelDirection = Normalize(toLabel);
        const std::optional<RaycastHit> terrainOccluder = world.Raycast(camera.position, labelDirection, std::max(0.0f, labelDistance - 0.18f));
        if (terrainOccluder.has_value())
        {
            continue;
        }

        bool occludedByPlayer = false;
        for (const Player& occluder : players)
        {
            if (occluder.GetId() == player.GetId()
                || !occluder.IsAlive()
                || (hideLocalPlayer && IsLocallyPredicted(occluder.GetControlKind())))
            {
                continue;
            }

            const Vector3 occluderCenter {
                occluder.GetPosition().x,
                occluder.GetPosition().y + 0.18f,
                occluder.GetPosition().z
            };
            if (RayIntersectsAabb(camera.position, labelDirection, labelDistance, occluderCenter, Vector3 { 0.42f, 1.02f, 0.42f }))
            {
                occludedByPlayer = true;
                break;
            }
        }
        if (occludedByPlayer)
        {
            continue;
        }

        const Vector2 screen = GetWorldToScreen(labelPoint, camera);
        if (screen.x < -120.0f || screen.x > static_cast<float>(GetScreenWidth()) + 120.0f
            || screen.y < -60.0f || screen.y > static_cast<float>(GetScreenHeight()) + 80.0f)
        {
            continue;
        }

        const bool enemy = localTeamId >= 0 && player.GetTeamId() != localTeamId;
        const Team* team = FindTeam(teams, player.GetTeamId());
        const Color teamColor = team != nullptr ? GetTeamColor(team->color) : WHITE;
        const Color relationColor = enemy ? Color { 255, 118, 118, 255 } : Color { 128, 238, 166, 255 };
        const std::string label = std::string(enemy ? "ENEMY " : "ALLY ") + player.GetName();
        const int labelWidth = MeasureText(label.c_str(), 16);
        const int x = static_cast<int>(screen.x) - labelWidth / 2;
        const int y = static_cast<int>(screen.y);

        DrawRectangle(x - 7, y - 5, labelWidth + 14, 42, Fade(BLACK, 0.46f));
        DrawText(label.c_str(), x, y, 16, relationColor);
        DrawRectangle(x, y + 20, labelWidth, 6, Fade(RED, 0.42f));
        DrawRectangle(x, y + 20, static_cast<int>(labelWidth * static_cast<float>(player.GetHealth()) / static_cast<float>(std::max(1, player.GetMaxHealth()))), 6, teamColor);
        DrawRectangleLines(x, y + 20, labelWidth, 6, Fade(WHITE, 0.42f));
        if (player.HasShield())
        {
            DrawText("ЩИТ", x, y + 29, 10, Color { 112, 232, 255, 255 });
        }
    }

    if (showHeroDebug && localPlayer != nullptr)
    {
        const HeroVisualAsset* visual = heroVisuals_.Find(localPlayer->GetHeroId());
        const HeroHitboxProfile& hitbox = HeroSystem::GetHitboxProfile(localPlayer->GetHeroId());
        const std::string path = visual != nullptr ? visual->sourcePath : "procedural fallback";
        DrawRectangle(12, 12, 820, projectiles.empty() ? 101 : 124, Fade(BLACK, 0.76f));
        DrawRectangleLines(12, 12, 820, projectiles.empty() ? 101 : 124, Fade(Color { 104, 238, 255, 255 }, 0.72f));
        DrawText((std::string("F8 HERO DEBUG | state: ") + HeroAnimationStateName(localPlayer->GetHeroState().animationState)).c_str(), 22, 20, 17, WHITE);
        DrawText((std::string("asset: ") + path).c_str(), 22, 43, 15, Color { 178, 235, 255, 255 });
        DrawText((std::string("hitbox r=") + FormatTenths(hitbox.bodyRadius) + " h=" + FormatTenths(hitbox.bodyHeight)).c_str(), 22, 64, 14, Color { 255, 220, 72, 255 });
        const ChunkRenderStats& chunkStats = chunkRenderer_.GetStats();
        const std::string performanceLine = "FPS=" + std::to_string(GetFPS())
            + " draw=" + std::to_string(chunkStats.drawCalls)
            + " chunks=" + std::to_string(chunkStats.visibleChunks)
            + " tris=" + std::to_string(chunkStats.triangles)
            + " rebuild=" + FormatTenths(chunkStats.lastRebuildMilliseconds) + "ms";
        DrawText(performanceLine.c_str(), 22, 83, 14, Color { 128, 238, 166, 255 });
        if (!projectiles.empty())
        {
            const EnergyProjectile& projectile = projectiles.back();
            const float speed = std::sqrt(
                projectile.velocity.x * projectile.velocity.x
                + projectile.velocity.y * projectile.velocity.y
                + projectile.velocity.z * projectile.velocity.z);
            const std::string projectileLine = "projectile speed=" + FormatTenths(speed)
                + " dmg=" + std::to_string(projectile.damage)
                + " dist=" + FormatTenths(projectile.distanceTraveled)
                + " owner/team=" + std::to_string(projectile.ownerId) + "/" + std::to_string(projectile.ownerTeamId)
                + (projectile.critical ? " CRIT" : "");
            DrawText(projectileLine.c_str(), 22, 103, 14, Color { 255, 120, 220, 255 });
        }
    }
}

void Renderer::RenderUI(
    const Player& localPlayer,
    const std::vector<Team>& teams,
    bool shopOpen,
    bool inShopZone,
    int shopCategoryIndex,
    const Shop& shop,
    const std::string& message,
    bool chatInputOpen,
    const std::string& chatInput,
    const PlacementPreview& placementPreview,
    const BreakProgress& breakProgress,
    const CombatPreview& combatPreview,
    const OrbitaTeleportPreview& orbitaTeleportPreview,
    int selectedHotbarSlot,
    bool inventoryOpen,
    int inventoryCursorSlot,
    const ItemStack& heldInventoryStack,
    const char* cameraModeText,
    const std::vector<EventMessage>& chatMessages,
    const MatchStats& stats,
    float hitMarkerTimer,
    float damageFlashTimer,
    float matchTime,
    const char* heroActive1KeyText,
    const char* heroActive2KeyText,
    const char* heroUltimateKeyText,
    float sniperScopeBlend,
    float sniperMagnification,
    std::optional<int> winnerTeamId) const
{
    const Team* playerTeam = FindTeam(teams, localPlayer.GetTeamId());
    const Inventory& inventory = localPlayer.GetInventory();
    (void)cameraModeText;
    (void)inShopZone;
    (void)message;

    if (damageFlashTimer > 0.0f)
    {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(RED, std::min(0.28f, damageFlashTimer * 0.28f)));
    }

    if (shopOpen)
    {
        const int panelWidth = 790;
        const int panelHeight = 420;
        const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
        const int panelY = GetScreenHeight() / 2 - panelHeight / 2;
        DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(Color { 8, 10, 14, 255 }, 0.88f));
        DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.22f));
        DrawText("Командный магазин", panelX + 24, panelY + 20, 28, WHITE);
        const int tabWidth = 118;
        const int tabHeight = 26;
        const int tabY = panelY + 52;
        for (int category = 0; category < shop.GetCategoryCount(); ++category)
        {
            const bool active = category == shopCategoryIndex;
            const Rectangle tab {
                static_cast<float>(panelX + 24 + category * (tabWidth + 8)),
                static_cast<float>(tabY),
                static_cast<float>(tabWidth),
                static_cast<float>(tabHeight)
            };
            DrawRectangleRec(tab, Fade(active ? Color { 68, 82, 96, 255 } : Color { 28, 32, 40, 255 }, 0.90f));
            DrawRectangleLinesEx(tab, 1.0f, Fade(active ? Color { 255, 235, 142, 255 } : WHITE, active ? 0.80f : 0.22f));
            DrawText(shop.GetCategoryName(category), static_cast<int>(tab.x) + 12, static_cast<int>(tab.y) + 6, 15, active ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.72f));
        }
        DrawText(shop.GetMenuText().c_str(), panelX + 536, panelY + 58, 14, Fade(WHITE, 0.62f));

        int rowY = panelY + 104;
        const std::vector<ShopItem> visibleItems = shop.GetItemsForCategory(shopCategoryIndex, inventory);
        for (int row = 0; row < static_cast<int>(visibleItems.size()); ++row)
        {
            const ShopItem& item = visibleItems[row];
            const bool maxed = IsAtMax(item, inventory, playerTeam);
            const bool affordable = shop.CanAfford(inventory, item) && !maxed;
            const Color rowColor = affordable ? WHITE : Fade(WHITE, 0.38f);
            DrawRectangle(panelX + 22, rowY - 6, panelWidth - 44, 30, Fade(affordable ? Color { 42, 52, 62, 255 } : Color { 28, 30, 36, 255 }, 0.72f));
            const ItemType iconItem = ShopIconItem(item.choice);
            const Rectangle iconRect {
                static_cast<float>(panelX + 34),
                static_cast<float>(rowY - 3),
                24.0f,
                24.0f
            };
            DrawRectangleRec(Rectangle { iconRect.x - 3.0f, iconRect.y - 3.0f, iconRect.width + 6.0f, iconRect.height + 6.0f }, Fade(Color { 4, 6, 10, 255 }, 0.42f));
            if (const Texture2D* texture = GetItemTexture(iconItem))
            {
                DrawTexturePro(
                    *texture,
                    Rectangle { 0.0f, 0.0f, static_cast<float>(texture->width), static_cast<float>(texture->height) },
                    iconRect,
                    Vector2 { 0.0f, 0.0f },
                    0.0f,
                    WHITE);
            }
            else
            {
                DrawCircle(static_cast<int>(iconRect.x + 12.0f), static_cast<int>(iconRect.y + 12.0f), 9.0f, ItemUiColor(iconItem));
            }
            DrawText(std::to_string(row + 1).c_str(), panelX + 70, rowY, 17, rowColor);
            DrawText(item.name.c_str(), panelX + 104, rowY, 17, rowColor);
            DrawText(item.description.c_str(), panelX + 252, rowY, 16, Fade(rowColor, 0.88f));
            DrawText(LevelText(item, inventory, playerTeam).c_str(), panelX + 548, rowY, 16, Fade(rowColor, 0.78f));
            DrawText(maxed ? "МАКС" : CostText(item).c_str(), panelX + 618, rowY, 16, affordable ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 });
            rowY += 34;
        }
    }

    const int centerX = GetScreenWidth() / 2;
    const int centerY = GetScreenHeight() / 2;
    if (!shopOpen && !inventoryOpen)
    {
        const float scopeBlend = std::clamp(sniperScopeBlend, 0.0f, 1.0f);
        if (scopeBlend > 0.01f)
        {
            const float minDimension = static_cast<float>(std::min(GetScreenWidth(), GetScreenHeight()));
            const float lensRadius = minDimension * (0.48f - scopeBlend * 0.055f);
            const float outerRadius = static_cast<float>(std::max(GetScreenWidth(), GetScreenHeight()));
            DrawRing(
                Vector2 { static_cast<float>(centerX), static_cast<float>(centerY) },
                lensRadius,
                outerRadius,
                0.0f,
                360.0f,
                128,
                Fade(BLACK, scopeBlend * 0.96f));
            DrawCircleLines(centerX, centerY, lensRadius, Fade(Color { 120, 232, 255, 255 }, scopeBlend * 0.76f));
            DrawCircleLines(centerX, centerY, lensRadius - 3.0f, Fade(BLACK, scopeBlend * 0.92f));
            const Color reticle = Fade(Color { 180, 245, 255, 255 }, scopeBlend * 0.88f);
            DrawLine(centerX - static_cast<int>(lensRadius), centerY, centerX - 12, centerY, reticle);
            DrawLine(centerX + 12, centerY, centerX + static_cast<int>(lensRadius), centerY, reticle);
            DrawLine(centerX, centerY - static_cast<int>(lensRadius), centerX, centerY - 12, reticle);
            DrawLine(centerX, centerY + 12, centerX, centerY + static_cast<int>(lensRadius), reticle);
            DrawCircleLines(centerX, centerY, 3.0f, reticle);
            const std::string zoomText = "x" + FormatTenths(sniperMagnification) + "  колесо мыши";
            DrawText(
                zoomText.c_str(),
                centerX - MeasureText(zoomText.c_str(), 18) / 2,
                centerY + static_cast<int>(lensRadius) - 32,
                18,
                Fade(Color { 180, 245, 255, 255 }, scopeBlend));
        }
        Color crosshairColor = placementPreview.visible
            ? (placementPreview.valid ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 })
            : WHITE;
        if (!placementPreview.visible && combatPreview.targetInRange)
        {
            crosshairColor = combatPreview.ready ? Color { 255, 235, 142, 255 } : Color { 188, 198, 210, 255 };
        }
        const float normalCrosshairAlpha = 1.0f - scopeBlend;
        DrawCircleLines(centerX, centerY, 6.0f, Fade(crosshairColor, normalCrosshairAlpha));
        DrawLine(centerX - 17, centerY, centerX - 9, centerY, Fade(crosshairColor, 0.72f * normalCrosshairAlpha));
        DrawLine(centerX + 9, centerY, centerX + 17, centerY, Fade(crosshairColor, 0.72f * normalCrosshairAlpha));
        DrawLine(centerX, centerY - 17, centerX, centerY - 9, Fade(crosshairColor, 0.72f * normalCrosshairAlpha));
        DrawLine(centerX, centerY + 9, centerX, centerY + 17, Fade(crosshairColor, 0.72f * normalCrosshairAlpha));
        if (selectedHotbarSlot >= 0 && selectedHotbarSlot < kHotbarSlotCount)
        {
            const ItemStack& selected = inventory.GetHotbarSlots()[selectedHotbarSlot];
            if (selected.type == ItemType::Bow && localPlayer.GetBowDrawTimer() > 0.0f)
            {
                const float fraction = BowDrawPower(localPlayer.GetBowDrawTimer());
                const Color bowColor = fraction >= 0.999f ? Color { 255, 226, 96, 255 } : Color { 112, 232, 255, 255 };
                DrawBar(Rectangle { static_cast<float>(centerX - 76), static_cast<float>(centerY + 31), 152.0f, 10.0f },
                    fraction, bowColor, Fade(BLACK, 0.72f), Fade(bowColor, 0.92f));
            }
        }
        if (hitMarkerTimer > 0.0f)
        {
            const float alpha = std::min(1.0f, hitMarkerTimer * 5.0f);
            const Color marker = Fade(Color { 255, 245, 150, 255 }, alpha);
            DrawLine(centerX - 18, centerY - 18, centerX - 8, centerY - 8, marker);
            DrawLine(centerX + 18, centerY - 18, centerX + 8, centerY - 8, marker);
            DrawLine(centerX - 18, centerY + 18, centerX - 8, centerY + 8, marker);
            DrawLine(centerX + 18, centerY + 18, centerX + 8, centerY + 8, marker);
        }

        if (breakProgress.visible)
        {
            const std::string label = breakProgress.label + " " + std::to_string(static_cast<int>(breakProgress.fraction * 100.0f)) + "%";
            DrawText(label.c_str(), centerX - MeasureText(label.c_str(), 18) / 2, centerY + 28, 18, Color { 255, 235, 142, 255 });
            DrawBar(Rectangle { static_cast<float>(centerX - 74), static_cast<float>(centerY + 52), 148.0f, 10.0f },
                breakProgress.fraction,
                breakProgress.isCore ? Color { 112, 232, 255, 255 } : Color { 255, 210, 94, 255 },
                Fade(WHITE, 0.14f),
                Fade(WHITE, 0.32f));
        }
    }

    const auto drawSlot = [this, &teams, &localPlayer](const ItemStack& stack, int x, int y, int slotSize, bool selected, bool cursor)
    {
        const Color border = cursor
            ? Color { 112, 232, 255, 255 }
            : (selected ? Color { 255, 235, 142, 255 } : Fade(WHITE, 0.22f));
        DrawRectangle(x, y, slotSize, slotSize, Fade(Color { 8, 10, 14, 255 }, selected ? 0.86f : 0.66f));
        DrawRectangleLinesEx(
            Rectangle { static_cast<float>(x), static_cast<float>(y), static_cast<float>(slotSize), static_cast<float>(slotSize) },
            cursor ? 3.0f : (selected ? 2.5f : 1.0f),
            border);

        if (stack.IsEmpty())
        {
            return;
        }

        const std::optional<BlockType> block = ItemToBlock(stack.type);
        Color itemColor = ItemUiColor(stack.type);
        if (block.has_value())
        {
            const int variant = stack.type == ItemType::LightBlock
                    && localPlayer.GetSelectedWoolVariant() >= 0
                ? 16 + localPlayer.GetSelectedWoolVariant()
                : 0;
            itemColor = GetBlockColor(
                Block { *block, localPlayer.GetTeamId(), true, variant }, teams);
        }

        if (const Texture2D* texture = GetItemTexture(stack.type))
        {
            const Rectangle source { 0.0f, 0.0f, static_cast<float>(texture->width), static_cast<float>(texture->height) };
            const Rectangle dest {
                static_cast<float>(x + 9),
                static_cast<float>(y + 6),
                static_cast<float>(slotSize - 18),
                static_cast<float>(slotSize - 18)
            };
            DrawTexturePro(*texture, source, dest, Vector2 { 0.0f, 0.0f }, 0.0f, block.has_value() ? itemColor : WHITE);
        }
        else
        {
            DrawRectangle(x + 14, y + 12, slotSize - 28, slotSize - 28, itemColor);
            DrawRectangleLines(x + 14, y + 12, slotSize - 28, slotSize - 28, Fade(WHITE, 0.42f));
        }

        if (stack.count > 0)
        {
            const std::string count = std::to_string(stack.count);
            DrawText(count.c_str(), x + slotSize - MeasureText(count.c_str(), 14) - 5, y + slotSize - 16, 14, WHITE);
        }
    };

    const int slotSize = 50;
    const int gap = 8;
    const int hotbarWidth = slotSize * kHotbarSlotCount + gap * (kHotbarSlotCount - 1);
    const int hotbarX = centerX - hotbarWidth / 2;
    const int hotbarY = GetScreenHeight() - 96;
    const float healthFraction = static_cast<float>(localPlayer.GetHealth()) / static_cast<float>(std::max(1, localPlayer.GetMaxHealth()));
    const int hudRowHeight = 50;
    const int hudRowY = hotbarY;
    const int hpWidth = hotbarWidth;
    const int hpHeight = 27;
    const int hpY = hotbarY - hpHeight - 10;
    const Rectangle hpBar {
        static_cast<float>(centerX - hpWidth / 2),
        static_cast<float>(hpY),
        static_cast<float>(hpWidth),
        static_cast<float>(hpHeight)
    };
    const int hpX = static_cast<int>(hpBar.x);
    const int hudColumnGap = 10;
    const int chatPanelX = 14;
    const int chatPanelWidth = std::max(0, hpX - hudColumnGap - chatPanelX);
    const int abilityPanelX = hpX + hpWidth + hudColumnGap;
    const int abilityPanelWidth = std::max(0, GetScreenWidth() - abilityPanelX - 14);

    struct HudMessageLine { std::string text; Color color; };
    std::vector<HudMessageLine> hudLines;
    for (const EventMessage& event : chatMessages)
    {
        const float remaining = 1.0f - std::clamp(
            event.age / std::max(0.001f, event.lifetime), 0.0f, 1.0f);
        hudLines.push_back(HudMessageLine { event.text, Fade(event.color, 0.55f + remaining * 0.45f) });
    }
    const bool showChatPanel = chatInputOpen || !hudLines.empty();
    if (showChatPanel && chatPanelWidth >= 80)
    {
        DrawRectangle(chatPanelX, hudRowY, chatPanelWidth, hudRowHeight,
            Fade(Color { 8, 10, 14, 255 }, chatInputOpen ? 0.84f : 0.62f));
        DrawRectangleLines(chatPanelX, hudRowY, chatPanelWidth, hudRowHeight,
            Fade(chatInputOpen ? VisualTheme::Palette::Energy : WHITE, chatInputOpen ? 0.72f : 0.16f));
        BeginScissorMode(chatPanelX + 8, hudRowY + 4, chatPanelWidth - 16, hudRowHeight - 8);
        if (chatInputOpen)
        {
            if (!hudLines.empty())
            {
                const HudMessageLine& previous = hudLines.back();
                DrawText(previous.text.c_str(), chatPanelX + 10, hudRowY + 5, 14, Fade(previous.color, 0.62f));
            }
            const bool cursorVisible = std::fmod(static_cast<float>(GetTime()), 1.0f) < 0.56f;
            const std::string inputLine = "> " + chatInput + (cursorVisible ? "_" : "");
            DrawText(inputLine.c_str(), chatPanelX + 10, hudRowY + 27, 16, WHITE);
        }
        else
        {
            const int firstLine = std::max(0, static_cast<int>(hudLines.size()) - 2);
            int lineY = hudRowY + 7;
            for (int i = firstLine; i < static_cast<int>(hudLines.size()); ++i)
            {
                DrawText(hudLines[i].text.c_str(), chatPanelX + 10, lineY, 15, hudLines[i].color);
                lineY += 20;
            }
        }
        EndScissorMode();
    }
    DrawBar(
        hpBar,
        healthFraction,
        Color { 224, 42, 58, 255 },
        Fade(Color { 92, 8, 18, 255 }, 0.72f),
        Fade(WHITE, 0.34f));
    if (orbitaTeleportPreview.visible && orbitaTeleportPreview.valid && orbitaTeleportPreview.healthCost > 0)
    {
        const float maxHealth = static_cast<float>(std::max(1, localPlayer.GetMaxHealth()));
        const float currentFraction = std::clamp(static_cast<float>(localPlayer.GetHealth()) / maxHealth, 0.0f, 1.0f);
        const float afterFraction = std::clamp(static_cast<float>(localPlayer.GetHealth() - orbitaTeleportPreview.healthCost) / maxHealth, 0.0f, currentFraction);
        const float costFraction = std::max(0.0f, currentFraction - afterFraction);
        if (costFraction > 0.001f)
        {
            const float blink = 0.5f + 0.5f * std::sin(static_cast<float>(GetTime()) * 11.5f);
            const Rectangle costSegment {
                hpBar.x + hpBar.width * afterFraction,
                hpBar.y,
                hpBar.width * costFraction,
                hpBar.height
            };
            DrawRectangleRec(costSegment, Fade(WHITE, 0.18f + blink * 0.42f));
            DrawRectangleLinesEx(costSegment, 1.0f, Fade(Color { 255, 224, 224, 255 }, 0.58f + blink * 0.30f));
            const std::string costText = "-" + std::to_string(orbitaTeleportPreview.healthCost) + " хп";
            DrawText(costText.c_str(), static_cast<int>(hpBar.x + hpBar.width + 8.0f), hpY + 2, 15, Fade(WHITE, 0.72f + blink * 0.24f));
        }
    }
    const std::string hpText = std::to_string(localPlayer.GetHealth()) + "/" + std::to_string(localPlayer.GetMaxHealth());
    constexpr int hpFontSize = 16;
    const int hpTextX = static_cast<int>(hpBar.x)
        + (static_cast<int>(hpBar.width) - MeasureText(hpText.c_str(), hpFontSize)) / 2;
    const int hpTextY = static_cast<int>(hpBar.y)
        + (static_cast<int>(hpBar.height) - hpFontSize) / 2;
    DrawText(hpText.c_str(), hpTextX, hpTextY, hpFontSize, WHITE);
    const auto& hotbar = inventory.GetHotbarSlots();
    for (int i = 0; i < kHotbarSlotCount; ++i)
    {
        const int x = hotbarX + i * (slotSize + gap);
        drawSlot(hotbar[i], x, hotbarY, slotSize, i == selectedHotbarSlot, inventoryOpen && inventoryCursorSlot == i);
    }
    const std::string currency = "Fe " + std::to_string(inventory.GetResource(ResourceType::Iron))
        + "   Au " + std::to_string(inventory.GetResource(ResourceType::Gold))
        + "   Cr " + std::to_string(inventory.GetResource(ResourceType::Crystal));
    DrawText(currency.c_str(), hotbarX, hotbarY + slotSize + 7, 15, Fade(WHITE, 0.82f));
    if (selectedHotbarSlot >= 0 && selectedHotbarSlot < kHotbarSlotCount)
    {
        const ItemStack& selected = hotbar[selectedHotbarSlot];
        std::string variantStatus;
        Color variantColor = Fade(WHITE, 0.82f);
        if (selected.type == ItemType::Bow)
        {
            const ArrowVariant variant = localPlayer.GetArrowVariant();
            const float reload = localPlayer.GetArrowReloadTimer(variant);
            variantStatus = std::string(ArrowVariantName(variant)) + " "
                + std::to_string(localPlayer.GetArrowAmmo(variant)) + "/"
                + std::to_string(ArrowQuiverCapacity(variant));
            if (reload > 0.0f)
            {
                variantStatus += " · " + FormatTenths(reload) + " с";
            }
            variantColor = variant == ArrowVariant::Breacher
                ? Color { 255, 194, 86, 255 }
                : (variant == ArrowVariant::Impulse
                    ? Color { 186, 126, 255, 255 }
                    : Color { 178, 245, 255, 255 });
        }
        else if (selected.type == ItemType::LightBlock)
        {
            variantStatus = "Шерсть: цвет "
                + std::to_string(std::max(0, localPlayer.GetSelectedWoolVariant()) + 1)
                + "/16";
        }
        if (!variantStatus.empty())
        {
            DrawText(variantStatus.c_str(),
                hotbarX + hotbarWidth - MeasureText(variantStatus.c_str(), 15),
                hotbarY + slotSize + 7, 15, variantColor);
        }
    }

    const HeroDefinition& hero = HeroSystem::GetDefinition(localPlayer.GetHeroId());
    const HeroRuntimeState& heroState = localPlayer.GetHeroState();
    const Color heroColor = VisualTheme::HeroAccent(hero.id);
    const int abilityGap = 6;
    const int abilityCardWidth = (abilityPanelWidth - abilityGap * 2) / 3;
    const int abilityLabelSize = abilityCardWidth < 105 ? 10 : 12;
    const auto drawAbility = [hudRowY, hudRowHeight, heroColor, abilityCardWidth, abilityLabelSize](
        const char* key, const char* label, const std::string& stateText, int x)
    {
        DrawRectangle(x, hudRowY, abilityCardWidth, hudRowHeight, Fade(Color { 12, 15, 21, 255 }, 0.70f));
        DrawRectangleLines(x, hudRowY, abilityCardWidth, hudRowHeight, Fade(heroColor, 0.36f));
        DrawText(key, x + 6, hudRowY + 8, 15, heroColor);
        DrawText(label, x + 27, hudRowY + 6, abilityLabelSize, Fade(WHITE, 0.92f));
        DrawText(stateText.c_str(), x + 27, hudRowY + 27, 11, Fade(WHITE, 0.64f));
    };
    const int abilityX = abilityPanelX;
    drawAbility(heroActive1KeyText, AbilityHudLabel(hero.id, HeroAbilitySlot::Active1),
        AbilityStateText(heroState.active1, false, heroState.ultimateCharge, false), abilityX);
    drawAbility(heroActive2KeyText, AbilityHudLabel(hero.id, HeroAbilitySlot::Active2),
        AbilityStateText(heroState.active2, false, heroState.ultimateCharge, false), abilityX + abilityCardWidth + abilityGap);
    drawAbility(heroUltimateKeyText, AbilityHudLabel(hero.id, HeroAbilitySlot::Ultimate),
        AbilityStateText(heroState.ultimate, true, heroState.ultimateCharge, heroState.ultimatePrimed), abilityX + (abilityCardWidth + abilityGap) * 2);

    if (inventoryOpen)
    {
        const int panelWidth = hotbarWidth + 42;
        const int panelHeight = 322;
        const int panelX = centerX - panelWidth / 2;
        const int panelY = GetScreenHeight() / 2 - panelHeight / 2;
        DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(Color { 8, 10, 14, 255 }, 0.90f));
        DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.22f));
        DrawText("Инвентарь", panelX + 20, panelY + 16, 24, WHITE);

        const auto& mainSlots = inventory.GetMainSlots();
        const int gridX = panelX + 21;
        const int gridY = panelY + 58;
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 9; ++col)
            {
                const int mainIndex = row * 9 + col;
                const int slot = kHotbarSlotCount + mainIndex;
                const int x = gridX + col * (slotSize + gap);
                const int y = gridY + row * (slotSize + gap);
                drawSlot(mainSlots[mainIndex], x, y, slotSize, false, inventoryCursorSlot == slot);
            }
        }

        const int inventoryHotbarY = gridY + 3 * (slotSize + gap) + 14;
        for (int i = 0; i < kHotbarSlotCount; ++i)
        {
            const int x = gridX + i * (slotSize + gap);
            drawSlot(hotbar[i], x, inventoryHotbarY, slotSize, i == selectedHotbarSlot, inventoryCursorSlot == i);
        }

        if (!heldInventoryStack.IsEmpty())
        {
            const Vector2 mouse = GetMousePosition();
            drawSlot(heldInventoryStack, static_cast<int>(mouse.x) + 12, static_cast<int>(mouse.y) + 12, slotSize, false, true);
        }
    }

    if (winnerTeamId.has_value())
    {
        const Team* winner = FindTeam(teams, *winnerTeamId);
        const bool localVictory = localPlayer.GetTeamId() == *winnerTeamId;
        const std::string title = localVictory ? "ПОБЕДА" : "ПОРАЖЕНИЕ";
        const std::string winnerText = (winner != nullptr ? winner->name : "Неизвестная команда") + " побеждает";
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.45f));
        DrawText(title.c_str(), GetScreenWidth() / 2 - MeasureText(title.c_str(), 48) / 2, GetScreenHeight() / 2 - 68, 48, localVictory ? Color { 142, 255, 170, 255 } : Color { 255, 138, 138, 255 });
        DrawText(winnerText.c_str(), GetScreenWidth() / 2 - MeasureText(winnerText.c_str(), 24) / 2, GetScreenHeight() / 2 - 18, 24, WHITE);
        const std::string combatStats = "Время " + std::to_string(static_cast<int>(matchTime)) + "с"
            + " | K/D " + std::to_string(stats.kills) + "/" + std::to_string(stats.deaths)
            + " | Попадания " + std::to_string(stats.hitsDealt)
            + " | Урон " + std::to_string(stats.damageDealt)
            + " | Кор " + std::to_string(stats.coreDamageDealt);
        const std::string economyStats = "Коры " + std::to_string(stats.coresDestroyed)
            + " | Блоки " + std::to_string(stats.blocksPlaced) + "/" + std::to_string(stats.blocksBroken)
            + " | Ресурсы " + std::to_string(stats.resourcesPicked);
        DrawText(combatStats.c_str(), GetScreenWidth() / 2 - MeasureText(combatStats.c_str(), 22) / 2, GetScreenHeight() / 2 + 20, 22, Color { 220, 220, 220, 255 });
        DrawText(economyStats.c_str(), GetScreenWidth() / 2 - MeasureText(economyStats.c_str(), 22) / 2, GetScreenHeight() / 2 + 50, 22, Color { 220, 220, 220, 255 });
        const char* actions = "Enter: новый матч | Esc: главное меню";
        DrawText(actions, GetScreenWidth() / 2 - MeasureText(actions, 24) / 2, GetScreenHeight() / 2 + 88, 24, Color { 220, 220, 220, 255 });
    }

}

Color Renderer::GetBlockColor(const Block& block, const std::vector<Team>& teams) const
{
    switch (block.type)
    {
    case BlockType::Solid:
        return Color { 92, 98, 112, 255 };
    case BlockType::GrassBlock:
        return Color { 94, 156, 76, 255 };
    case BlockType::DirtBlock:
        return Color { 112, 78, 52, 255 };
    case BlockType::LeafBlock:
        // Fully opaque on purpose: leaves used to ride the no-depth-write
        // transparent pass at alpha 245, which made everything behind a tree
        // sort incorrectly.  Solid leaves write depth and receive baked AO.
        return Color { 70, 134, 62, 255 };
    case BlockType::TeamBlock:
    case BlockType::WoolBlock:
    {
        if (block.variant >= 16 && block.variant < 32)
        {
            return MinecraftDyeColor(block.variant - 16);
        }
        const Team* team = FindTeam(teams, block.teamId);
        if (team != nullptr)
        {
            return GetTeamColor(team->color);
        }
        // Imported neutral wool carries a legacy dye ID in variant.  Keep
        // regular unowned gameplay wool's historical gray fallback.
        return block.teamId < 0 && !block.breakable
            ? MinecraftDyeColor(block.variant)
            : Color { 150, 150, 150, 255 };
    }
    case BlockType::WoodBlock:
        return Color { 146, 101, 62, 255 };
    case BlockType::StoneBlock:
    {
        const Team* team = FindTeam(teams, block.teamId);
        const Color tint = team != nullptr ? GetTeamColor(team->color) : Color { 160, 166, 180, 255 };
        return Color {
            static_cast<unsigned char>((static_cast<int>(tint.r) + 130) / 2),
            static_cast<unsigned char>((static_cast<int>(tint.g) + 134) / 2),
            static_cast<unsigned char>((static_cast<int>(tint.b) + 146) / 2),
            255
        };
    }
    case BlockType::ObsidianBlock:
        return Color { 38, 28, 58, 255 };
    case BlockType::EnergyGlassBlock:
        return Color { 112, 232, 255, 170 };
    case BlockType::SpringBlock:
        return Color { 128, 238, 166, 255 };
    case BlockType::StickyBlock:
        return Color { 118, 92, 168, 255 };
    case BlockType::ExplosiveBlock:
        return Color { 220, 56, 50, 255 };
    case BlockType::SpikeBlock:
        return Color { 190, 200, 212, 255 };
    case BlockType::LavaBlock:
        return Color { 255, 88, 42, 255 };
    case BlockType::IceBlock:
        return Color { 142, 220, 255, 210 };
    case BlockType::SmoothStoneBlock:
        return Color { 184, 190, 198, 255 };
    case BlockType::DarkBrickBlock:
        return Color { 70, 62, 74, 255 };
    case BlockType::LightBrickBlock:
        return Color { 214, 194, 154, 255 };
    case BlockType::MetalBlock:
        return Color { 112, 126, 136, 255 };
    case BlockType::GlowBlock:
        return Color { 255, 232, 116, 255 };
    case BlockType::PlankBlock:
        return Color { 176, 118, 66, 255 };
    case BlockType::DecorativeTileBlock:
        return Color { 78, 170, 184, 255 };
    case BlockType::TrimBlock:
        return Color { 218, 164, 76, 255 };
    case BlockType::CobblestoneBlock:
        return Color { 128, 133, 138, 255 };
    case BlockType::AndesiteBlock:
        return Color { 126, 132, 140, 255 };
    case BlockType::PolishedAndesiteBlock:
        return Color { 142, 148, 156, 255 };
    case BlockType::StoneBrickBlock:
    case BlockType::ChiseledStoneBrickBlock:
    case BlockType::StoneBrickSlabBlock:
    case BlockType::StoneBrickStairsBlock:
        return Color { 144, 150, 158, 255 };
    case BlockType::StoneSlabBlock:
        return Color { 154, 160, 168, 255 };
    case BlockType::BirchPlankBlock:
    case BlockType::BirchSlabBlock:
    case BlockType::BirchStairsBlock:
        return Color { 224, 204, 150, 255 };
    case BlockType::ColoredGlassBlock:
    {
        Color dye = MinecraftDyeColor(block.variant);
        dye.a = 156;
        return dye;
    }
    case BlockType::ColoredClayBlock:
        return MinecraftDyeColor(block.variant);
    case BlockType::LapisBlock:
        return Color { 66, 122, 226, 255 };
    case BlockType::DiamondBlock:
        return Color { 92, 224, 224, 255 };
    case BlockType::EmeraldBlock:
        return Color { 64, 204, 112, 255 };
    case BlockType::GoldBlock:
        return Color { 246, 196, 74, 255 };
    case BlockType::IronBarsBlock:
        return Color { 138, 150, 162, 255 };
    case BlockType::LadderBlock:
        return Color { 176, 116, 58, 255 };
    case BlockType::TorchBlock:
        return WHITE;
    case BlockType::BarrierBlock:
        return BLANK;
    case BlockType::ResourceGenerator:
        return Color { 82, 82, 92, 255 };
    case BlockType::TeamChestBlock:
    {
        const Team* team = FindTeam(teams, block.teamId);
        const Color tint = team != nullptr ? GetTeamColor(team->color) : Color { 180, 150, 86, 255 };
        return Color {
            static_cast<unsigned char>((static_cast<int>(tint.r) + 108) / 2),
            static_cast<unsigned char>((static_cast<int>(tint.g) + 74) / 2),
            static_cast<unsigned char>((static_cast<int>(tint.b) + 42) / 2),
            255
        };
    }
    case BlockType::EnergyCoreBlock:
        return WHITE;
    case BlockType::Air:
    case BlockType::Count:
        break;
    }

    return BLANK;
}

const Texture2D* Renderer::GetBlockTexture(BlockType type) const
{
    if (!texturesReady_)
    {
        return nullptr;
    }

    switch (type)
    {
    case BlockType::GrassBlock:
        return &grassTexture_;
    case BlockType::DirtBlock:
        return &dirtTexture_;
    case BlockType::LeafBlock:
        return &leafTexture_;
    case BlockType::TeamBlock:
    case BlockType::WoolBlock:
        return &woolTexture_;
    case BlockType::WoodBlock:
        return &woodTexture_;
    case BlockType::PlankBlock:
        return &plankVariantTexture_;
    case BlockType::TeamChestBlock:
        return &teamChestTexture_;
    case BlockType::Solid:
    case BlockType::StoneBlock:
        return &stoneTexture_;
    case BlockType::SmoothStoneBlock:
        return &smoothStoneTexture_;
    case BlockType::DarkBrickBlock:
        return &darkBrickTexture_;
    case BlockType::LightBrickBlock:
        return &lightBrickTexture_;
    case BlockType::MetalBlock:
        return &metalBlockTexture_;
    case BlockType::DecorativeTileBlock:
        return &decorativeTileTexture_;
    case BlockType::TrimBlock:
        return &trimBlockTexture_;
    case BlockType::CobblestoneBlock:
        return &cobblestoneTexture_;
    case BlockType::AndesiteBlock:
        return &andesiteTexture_;
    case BlockType::PolishedAndesiteBlock:
        return &polishedAndesiteTexture_;
    case BlockType::StoneBrickBlock:
    case BlockType::StoneBrickSlabBlock:
    case BlockType::StoneBrickStairsBlock:
        return &stoneBrickTexture_;
    case BlockType::ChiseledStoneBrickBlock:
        return &chiseledStoneBrickTexture_;
    case BlockType::StoneSlabBlock:
        return &smoothStoneTexture_;
    case BlockType::BirchPlankBlock:
    case BlockType::BirchSlabBlock:
    case BlockType::BirchStairsBlock:
        return &birchPlankTexture_;
    case BlockType::ColoredGlassBlock:
        return &coloredGlassTexture_;
    case BlockType::ColoredClayBlock:
        return &coloredClayTexture_;
    case BlockType::LapisBlock:
        return &lapisTexture_;
    case BlockType::DiamondBlock:
        return &diamondTexture_;
    case BlockType::EmeraldBlock:
        return &emeraldTexture_;
    case BlockType::GoldBlock:
        return &goldTexture_;
    case BlockType::IronBarsBlock:
        return &ironBarsTexture_;
    case BlockType::LadderBlock:
        return &ladderTexture_;
    case BlockType::TorchBlock:
        return &torchTexture_;
    case BlockType::ObsidianBlock:
        return &obsidianTexture_;
    case BlockType::EnergyGlassBlock:
        return &glassTexture_;
    case BlockType::GlowBlock:
        return &glowBlockTexture_;
    case BlockType::SpringBlock:
        return &springTexture_;
    case BlockType::StickyBlock:
        return &stickyTexture_;
    case BlockType::ExplosiveBlock:
        return &tntTexture_;
    case BlockType::SpikeBlock:
        return &spikeTexture_;
    case BlockType::LavaBlock:
        return &lavaTexture_;
    case BlockType::IceBlock:
        return &iceTexture_;
    default:
        break;
    }

    return nullptr;
}

const Texture2D* Renderer::GetBlockNormalTexture(BlockType type) const
{
    if (!texturesReady_)
    {
        return nullptr;
    }

    // Types that share an albedo texture share its normal map too.
    BlockType canonical = type;
    switch (type)
    {
    case BlockType::Solid:
        canonical = BlockType::StoneBlock;
        break;
    case BlockType::TeamBlock:
        canonical = BlockType::WoolBlock;
        break;
    case BlockType::StoneSlabBlock:
        canonical = BlockType::SmoothStoneBlock;
        break;
    case BlockType::StoneBrickSlabBlock:
    case BlockType::StoneBrickStairsBlock:
        canonical = BlockType::StoneBrickBlock;
        break;
    case BlockType::BirchSlabBlock:
    case BlockType::BirchStairsBlock:
        canonical = BlockType::BirchPlankBlock;
        break;
    default:
        break;
    }

    const Texture2D& normalTexture = blockNormalTextures_[static_cast<std::size_t>(canonical)];
    if (normalTexture.id != 0)
    {
        return &normalTexture;
    }
    // Every chunk material must carry some normal map so the shader's
    // "texture2" sampler never reads an unbound unit.
    return flatNormalTexture_.id != 0 ? &flatNormalTexture_ : nullptr;
}

const Texture2D* Renderer::GetItemTexture(ItemType type) const
{
    if (!texturesReady_)
    {
        return nullptr;
    }

    switch (type)
    {
    case ItemType::Sword:
        return &swordIcon_;
    case ItemType::Axe:
        return &axeIcon_;
    case ItemType::Spear:
        return &spearIcon_;
    case ItemType::Pickaxe:
        return &pickaxeIcon_;
    case ItemType::EnergyArrow:
        return &arrowIcon_;
    case ItemType::Bow:
        return &arrowIcon_;
    case ItemType::Blaster:
        return &arrowIcon_;
    case ItemType::SniperRifle:
        return &arrowIcon_;
    case ItemType::Fireball:
        return &fireballIcon_;
    case ItemType::MedKit:
        return &medKitIcon_;
    case ItemType::HomeTeleport:
        return &homeIcon_;
    case ItemType::DashPearl:
        return &dashIcon_;
    case ItemType::Molotov:
        return &molotovIcon_;
    case ItemType::AlarmTrap:
        return &alarmIcon_;
    case ItemType::IronResource:
        return &ironIcon_;
    case ItemType::GoldResource:
        return &goldIcon_;
    case ItemType::CrystalResource:
        return &crystalIcon_;
    default:
        break;
    }

    if (const std::optional<BlockType> block = ItemToBlock(type))
    {
        return GetBlockTexture(*block);
    }

    return nullptr;
}

Color Renderer::GetResourceColor(ResourceType type) const
{
    switch (type)
    {
    case ResourceType::Iron:
        return VisualTheme::Palette::ResourceIron;
    case ResourceType::Gold:
        return VisualTheme::Palette::ResourceGold;
    case ResourceType::Crystal:
        return VisualTheme::Palette::ResourceCrystal;
    }

    return WHITE;
}

const Team* Renderer::FindTeam(const std::vector<Team>& teams, int teamId) const
{
    for (const Team& team : teams)
    {
        if (team.id == teamId)
        {
            return &team;
        }
    }

    return nullptr;
}

const EnergyCore* Renderer::FindCore(const std::vector<EnergyCore>& cores, int teamId) const
{
    for (const EnergyCore& core : cores)
    {
        if (core.GetTeamId() == teamId)
        {
            return &core;
        }
    }

    return nullptr;
}

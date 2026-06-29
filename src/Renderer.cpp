#include "Renderer.h"

#include "CombatSystem.h"
#include "HeroSystem.h"
#include "UiText.h"
#include "VecConvert.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
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

void DrawDistantFog(Color skyColor)
{
    const Color fog = MixColor(skyColor, WHITE, 0.16f);
    DrawFogWall(-70.0f, 70.0f, -70.0f, 70.0f, -36.0f, 34.0f, fog, 34);
    DrawFogWall(-82.0f, 82.0f, -82.0f, 82.0f, -42.0f, 38.0f, fog, 52);
    DrawFogWall(-96.0f, 96.0f, -96.0f, 96.0f, -48.0f, 42.0f, fog, 74);

    DrawPlane(Vector3 { 0.0f, -12.0f, 0.0f }, Vector2 { 156.0f, 156.0f }, Fade(fog, 0.08f));
    DrawPlane(Vector3 { 0.0f, -20.0f, 0.0f }, Vector2 { 210.0f, 210.0f }, Fade(fog, 0.10f));
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
        return ItemType::WoodBlock;
    case 2:
        return ItemType::LightBlock;
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

Color HeroUiColor(HeroId id)
{
    switch (id)
    {
    case HeroId::Radon:
        return Color { 92, 164, 255, 255 };
    case HeroId::Orbita:
        return Color { 255, 96, 82, 255 };
    case HeroId::Brom:
        return Color { 96, 202, 118, 255 };
    case HeroId::Konvoy:
        return Color { 92, 210, 255, 255 };
    case HeroId::Likho:
        return Color { 104, 238, 92, 255 };
    case HeroId::Svidetel:
        return Color { 180, 104, 255, 255 };
    }
    return WHITE;
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
        DrawLine3D(tail, base, Fade(effect.color, t * 0.95f));
        DrawSphere(base, effect.radius * (0.42f + t * 0.24f), Fade(effect.color, t * 0.70f));
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
    const Color bromColor = MixColor(HeroUiColor(HeroId::Brom), relationColor, 0.42f);
    const Color konvoyColor = MixColor(HeroUiColor(HeroId::Konvoy), relationColor, 0.42f);
    const Color svidetelColor = MixColor(HeroUiColor(HeroId::Svidetel), relationColor, 0.42f);
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

bool IsTransparentBlock(BlockType type, Color color)
{
    return color.a < 255
        || type == BlockType::EnergyGlassBlock
        || type == BlockType::IceBlock
        || type == BlockType::LeafBlock;
}

ItemStack VisibleHeldItemForPlayer(const Player& player, const ItemStack& localHeldItem)
{
    if (player.IsLocal())
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

    grassTexture_ = LoadCustomizableTexture("grass_block", MakeGrassImage());
    dirtTexture_ = LoadCustomizableTexture("dirt_block", MakeTextureImage(Color { 112, 78, 52, 255 }, Color { 86, 58, 38, 255 }, 5));
    leafTexture_ = LoadCustomizableTexture("leaf_block", MakeTextureImage(Color { 64, 132, 62, 245 }, Color { 112, 178, 86, 255 }, 6));
    woodTexture_ = LoadCustomizableTexture("wood_block", MakeWoodImage());
    woolTexture_ = LoadCustomizableTexture("wool_block", MakeWoolImage());
    stoneTexture_ = LoadCustomizableTexture("stone_block", MakeTextureImage(Color { 132, 138, 148, 255 }, Color { 88, 94, 108, 255 }, 7));
    obsidianTexture_ = LoadCustomizableTexture("obsidian_block", MakeTextureImage(Color { 38, 28, 54, 255 }, Color { 90, 52, 132, 255 }, 8));
    glassTexture_ = LoadCustomizableTexture("energy_glass_block", MakeTextureImage(Color { 112, 232, 255, 150 }, Color { 220, 252, 255, 210 }, 9));
    springTexture_ = LoadCustomizableTexture("spring_block", MakeTextureImage(Color { 92, 196, 124, 255 }, Color { 255, 235, 142, 255 }, 10));
    stickyTexture_ = LoadCustomizableTexture("sticky_block", MakeTextureImage(Color { 92, 184, 118, 255 }, Color { 40, 112, 72, 255 }, 11));
    tntTexture_ = LoadCustomizableTexture("explosive_block", MakeTntImage());
    spikeTexture_ = LoadCustomizableTexture("spike_block", MakeTextureImage(Color { 148, 148, 158, 255 }, Color { 236, 236, 244, 255 }, 12));
    lavaTexture_ = LoadCustomizableTexture("lava_block", MakeTextureImage(Color { 230, 70, 28, 255 }, Color { 255, 210, 66, 255 }, 13));
    iceTexture_ = LoadCustomizableTexture("ice_block", MakeTextureImage(Color { 150, 225, 255, 190 }, Color { 230, 250, 255, 230 }, 14));
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
    UnloadTexture(woolTexture_);
    UnloadTexture(stoneTexture_);
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
}

const ChunkRenderStats& Renderer::GetChunkRenderStats() const
{
    return chunkRenderer_.GetStats();
}

void Renderer::RenderHeroPreview(HeroId heroId, Rectangle destination, float yawDegrees) const
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
    Camera3D previewCamera {
        Vector3 { center.x + std::sin(yaw) * radius * 2.15f, center.y + height * 0.12f, center.z + std::cos(yaw) * radius * 2.15f },
        center,
        Vector3 { 0.0f, 1.0f, 0.0f },
        34.0f,
        CAMERA_PERSPECTIVE
    };

    BeginTextureMode(heroPreviewTarget_);
    ClearBackground(Color { 9, 12, 18, 255 });
    BeginMode3D(previewCamera);
    heroVisuals_.ApplyAnimation(heroId, HeroAnimationState::Idle, 0.0f, static_cast<float>(GetTime()));
    DrawModel(visual->model, Vector3 { 0.0f, 0.0f, 0.0f }, 1.0f, WHITE);
    DrawCylinderWires(Vector3 { center.x, bounds.min.y - 0.02f, center.z }, radius * 0.52f, radius * 0.52f, 0.025f, 32, Fade(HeroUiColor(heroId), 0.54f));
    EndMode3D();
    EndTextureMode();

    DrawTexturePro(
        heroPreviewTarget_.texture,
        Rectangle { 0.0f, 0.0f, static_cast<float>(heroPreviewTarget_.texture.width), -static_cast<float>(heroPreviewTarget_.texture.height) },
        destination,
        Vector2 { 0.0f, 0.0f },
        0.0f,
        WHITE);
    DrawRectangleLinesEx(destination, 1.0f, Fade(HeroUiColor(heroId), 0.62f));
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
    const std::vector<WorldEffect>& worldEffects,
    const ParticleSystem& particles,
    const std::vector<FloatingText>& floatingTexts,
    const Camera3D& camera,
    const ItemStack& localHeldItem,
    Color skyColor,
    bool hideLocalPlayer) const
{
    int localTeamId = -1;
    const Player* localPlayer = nullptr;
    for (const Player& player : players)
    {
        if (player.IsLocal())
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

    chunkRenderer_.Sync(
        world,
        [this, &teams](const Block& block) { return GetBlockColor(block, teams); },
        [this](BlockType type) { return GetBlockTexture(type); },
        [](const Block& block, Color color) { return IsTransparentBlock(block.type, color); });

    BeginMode3D(camera);

    DrawSkyVoid(skyColor, camera.position.y);
    for (int i = 0; i < 10; ++i)
    {
        const float x = -44.0f + static_cast<float>((i * 17) % 88);
        const float z = -38.0f + static_cast<float>((i * 29) % 76);
        const float width = 5.5f + static_cast<float>(i % 3) * 2.0f;
        const float depth = 1.4f + static_cast<float>((i + 1) % 3) * 0.8f;
        DrawCube(Vector3 { x, 28.0f + static_cast<float>(i % 2) * 2.4f, z }, width, 0.10f, depth, Fade(WHITE, 0.22f));
        DrawCube(Vector3 { x + width * 0.35f, 28.0f + static_cast<float>(i % 2) * 2.4f, z + depth * 0.65f }, width * 0.55f, 0.10f, depth * 0.85f, Fade(WHITE, 0.16f));
    }

    const Vector3 cameraForward = Normalize(Vector3 {
        camera.target.x - camera.position.x,
        camera.target.y - camera.position.y,
        camera.target.z - camera.position.z
    });
    const auto shouldDrawWorldBlock = [this, &camera, cameraForward](Vector3 center)
    {
        const Vector3 toBlock {
            center.x - camera.position.x,
            center.y - camera.position.y,
            center.z - camera.position.z
        };
        const float distanceSq = toBlock.x * toBlock.x + toBlock.y * toBlock.y + toBlock.z * toBlock.z;
        if (distanceSq > worldRenderDistance_ * worldRenderDistance_)
        {
            return false;
        }
        return Dot(cameraForward, toBlock) > -2.0f;
    };

    const auto drawWorldBlock = [this, &teams](Vector3 center, const Block& block)
    {
        const Color color = GetBlockColor(block, teams);
        if (const Texture2D* texture = GetBlockTexture(block.type))
        {
            DrawTexturedCube(*texture, center, 1.0f, 1.0f, 1.0f, color);
        }
        else
        {
            DrawCube(center, 1.0f, 1.0f, 1.0f, color);
        }
        DrawCubeWires(center, 1.01f, 1.01f, 1.01f, Color { 24, 28, 36, 180 });
    };

    transparentBlocks_.clear();
    std::vector<TransparentBlockDraw>& transparentBlocks = transparentBlocks_;
    sceneShader_.Begin(camera, skyColor, 0.0045f);
    chunkRenderer_.Draw(camera, worldRenderDistance_, sceneShader_.GetShader());
    for (const auto& entry : world.GetBlocks())
    {
        const GridPos& pos = entry.first;
        const Block& block = entry.second;
        if (block.type == BlockType::EnergyCoreBlock)
        {
            continue;
        }

        const Vector3 center = world.GridToWorld(pos);
        if (!shouldDrawWorldBlock(center))
        {
            continue;
        }

        const Color color = GetBlockColor(block, teams);
        if (!IsTransparentBlock(block.type, color))
        {
            continue;
        }
        transparentBlocks.push_back(TransparentBlockDraw {
            pos,
            center,
            block,
            DistanceSquared(center, camera.position)
        });
    }

    for (const Team& team : teams)
    {
        DrawCylinder(team.shopPosition, 1.7f, 1.7f, 0.08f, 24, Fade(GetTeamColor(team.color), 0.35f));
        DrawCylinderWires(team.shopPosition, 1.7f, 1.7f, 0.08f, 24, GetTeamColor(team.color));
    }

    for (const Generator& generator : generators)
    {
        const Vector3 pos = ToVector3(generator.GetPosition());
        DrawCube(Vector3 { pos.x, pos.y + 0.18f, pos.z }, 0.85f, 0.35f, 0.85f, GetResourceColor(generator.GetType()));
        DrawSphere(Vector3 { pos.x, pos.y + 0.62f, pos.z }, 0.24f, WHITE);
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
        if (hideLocalPlayer && player.IsLocal())
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
        if (shadowQuality_ > 0 && player.IsOnGround())
        {
            const int segments = shadowQuality_ > 1 ? 28 : 14;
            DrawCylinder(
                Vector3 { pos.x, pos.y - 0.875f, pos.z },
                0.46f,
                0.46f,
                0.018f,
                segments,
                Fade(BLACK, shadowQuality_ > 1 ? 0.25f : 0.17f));
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
            color = MixColor(color, HeroUiColor(HeroId::Likho), 0.18f + 0.10f * std::sin(static_cast<float>(GetTime()) * 13.0f));
        }
        if (svidetelContours && enemy)
        {
            const bool distorted = player.GetHeroId() == HeroId::Likho && heroState.ultimate.active;
            rlDisableDepthTest();
            DrawSphereWires(Vector3 { pos.x, pos.y + 0.25f, pos.z }, 1.15f, 10, 16, Fade(HeroUiColor(HeroId::Svidetel), distorted ? 0.58f : 0.86f));
            if (distorted)
            {
                const float jitter = 0.10f + 0.05f * std::sin(static_cast<float>(GetTime()) * 17.0f);
                DrawSphereWires(Vector3 { pos.x + jitter, pos.y + 0.32f, pos.z - jitter }, 1.08f, 8, 13, Fade(Color { 255, 92, 210, 255 }, 0.48f));
                DrawSphereWires(Vector3 { pos.x - jitter, pos.y + 0.18f, pos.z + jitter }, 1.22f, 7, 11, Fade(HeroUiColor(HeroId::Likho), 0.42f));
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
            const Color orbitaColor = HeroUiColor(HeroId::Orbita);
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
        if (player.IsLocal())
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
        const Vector3 cameraRight = Normalize(Cross(handForward, camera.up));
        const Vector3 cameraUp = Normalize(Cross(cameraRight, handForward));
        const Vector3 hand = Add(
            Add(camera.position, Scale(handForward, 0.52f)),
            Add(Scale(cameraRight, 0.44f), Scale(cameraUp, -0.34f)));
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
            const Vector3 leftHand = Add(Add(camera.position, Scale(heroForward, 0.58f + anim * 0.12f)), Add(Scale(cameraRight, -0.32f), Scale(cameraUp, -0.30f)));
            const Vector3 rightHand = Add(Add(camera.position, Scale(heroForward, 0.58f + anim * 0.12f)), Add(Scale(cameraRight, 0.32f), Scale(cameraUp, -0.30f)));
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
            const Color orbitaColor = HeroUiColor(HeroId::Orbita);
            const Vector3 leftHand = Add(Add(camera.position, Scale(heroForward, 0.62f + anim * 0.10f)), Add(Scale(cameraRight, -0.34f), Scale(cameraUp, -0.30f)));
            const Vector3 rightHand = Add(Add(camera.position, Scale(heroForward, 0.62f + anim * 0.10f)), Add(Scale(cameraRight, 0.34f), Scale(cameraUp, -0.30f)));
            DrawSphere(leftHand, 0.08f + pulse * 0.06f, Fade(WHITE, 0.72f));
            DrawSphere(rightHand, 0.08f + pulse * 0.06f, Fade(orbitaColor, 0.70f));
            DrawLine3D(leftHand, Add(leftHand, Scale(heroForward, 0.72f + pulse * 0.36f)), Fade(WHITE, 0.74f));
            DrawLine3D(rightHand, Add(rightHand, Scale(heroForward, 0.72f + pulse * 0.36f)), Fade(orbitaColor, 0.64f));
        }
    }

    for (const HeroDeviceVisual& device : heroDevices)
    {
        DrawHeroDeviceVisual(device, localTeamId);
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

    if (placementPreview.visible && placementPreview.valid)
    {
        const Vector3 previewCenter = world.GridToWorld(placementPreview.position);
        DrawCubeWires(previewCenter, 1.04f, 1.04f, 1.04f, BLACK);
    }

    for (const WorldEffect& effect : worldEffects)
    {
        DrawRadonPresentationEffect(effect);
    }
    particles.Draw();

    std::sort(
        transparentBlocks.begin(),
        transparentBlocks.end(),
        [](const TransparentBlockDraw& a, const TransparentBlockDraw& b)
        {
            return a.distance > b.distance;
        });
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
    rlDisableDepthMask();
    for (const TransparentBlockDraw& entry : transparentBlocks)
    {
        drawWorldBlock(entry.center, entry.block);
    }
    rlDrawRenderBatchActive();
    rlEnableDepthMask();

    rlDrawRenderBatchActive();
    rlEnableDepthTest();
    DrawDistantFog(skyColor);

    // Projectiles are real geometry, not only short-lived particles. This
    // keeps fast arrows and energy bolts readable even at high frame rates.
    for (const EnergyProjectile& projectile : projectiles)
    {
        const Vector3 direction = Normalize(projectile.velocity);
        if (projectile.kind == ProjectileKind::Arrow)
        {
            const Vector3 tail = Add(projectile.position, Scale(direction, -2.20f));
            DrawCylinderEx(tail, projectile.position, 0.070f, 0.025f, 8, Color { 206, 245, 255, 255 });
            DrawSphere(projectile.position, 0.13f, Color { 230, 255, 255, 255 });
            DrawLine3D(Add(tail, Vector3 { 0.0f, 0.12f, 0.0f }), projectile.position, Color { 112, 232, 255, 255 });
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
        const Color glow = blaster ? Color { 98, 245, 255, 255 } : Color { 178, 245, 255, 255 };
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
        if (!player.IsAlive() || player.IsLocal())
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
                || (hideLocalPlayer && occluder.IsLocal()))
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
    const PlacementPreview& placementPreview,
    const BreakProgress& breakProgress,
    const CombatPreview& combatPreview,
    const OrbitaTeleportPreview& orbitaTeleportPreview,
    int selectedHotbarSlot,
    bool inventoryOpen,
    int inventoryCursorSlot,
    const ItemStack& heldInventoryStack,
    const char* cameraModeText,
    const std::vector<EventMessage>& eventMessages,
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

    if (damageFlashTimer > 0.0f)
    {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(RED, std::min(0.28f, damageFlashTimer * 0.28f)));
    }

    DrawRectangle(14, GetScreenHeight() - 106, 500, 82, Fade(Color { 8, 10, 14, 255 }, 0.62f));
    DrawRectangleLines(14, GetScreenHeight() - 106, 500, 82, Fade(WHITE, 0.16f));
    DrawText(message.c_str(), 28, GetScreenHeight() - 92, 20, Color { 245, 218, 112, 255 });
    if (placementPreview.visible && !placementPreview.reason.empty())
    {
        DrawText(placementPreview.reason.c_str(), 28, GetScreenHeight() - 66, 18, placementPreview.valid ? Color { 88, 242, 150, 255 } : Color { 255, 118, 118, 255 });
    }
    if (inShopZone)
    {
        DrawText("Зона магазина: R", 28, GetScreenHeight() - 40, 18, Color { 125, 230, 255, 255 });
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
        const std::vector<ShopItem> visibleItems = shop.GetItemsForCategory(shopCategoryIndex);
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
            itemColor = GetBlockColor(Block { *block, localPlayer.GetTeamId(), true }, teams);
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

        const char* label = ItemShortName(stack.type);
        const int labelSize = 10;
        DrawText(label, x + slotSize / 2 - MeasureText(label, labelSize) / 2, y + slotSize - 18, labelSize, Fade(WHITE, 0.86f));
        if (stack.count > 1)
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
    const int hpWidth = hotbarWidth;
    const int hpHeight = 19;
    const int hpY = hotbarY - 31;
    const Rectangle hpBar {
        static_cast<float>(centerX - hpWidth / 2),
        static_cast<float>(hpY),
        static_cast<float>(hpWidth),
        static_cast<float>(hpHeight)
    };
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
    DrawText(hpText.c_str(), centerX - MeasureText(hpText.c_str(), 16) / 2, hpY + 2, 16, WHITE);
    const auto& hotbar = inventory.GetHotbarSlots();
    for (int i = 0; i < kHotbarSlotCount; ++i)
    {
        const int x = hotbarX + i * (slotSize + gap);
        drawSlot(hotbar[i], x, hotbarY, slotSize, i == selectedHotbarSlot, inventoryOpen && inventoryCursorSlot == i);
    }

    const HeroDefinition& hero = HeroSystem::GetDefinition(localPlayer.GetHeroId());
    const HeroRuntimeState& heroState = localPlayer.GetHeroState();
    const Color heroColor = HeroUiColor(hero.id);
    const int abilityPanelWidth = 360;
    const int abilityPanelHeight = 64;
    const int abilityPanelX = GetScreenWidth() - abilityPanelWidth - 18;
    const int abilityPanelY = hotbarY - abilityPanelHeight - 12;
    DrawRectangle(abilityPanelX, abilityPanelY, abilityPanelWidth, abilityPanelHeight, Fade(Color { 8, 10, 14, 255 }, 0.66f));
    DrawRectangleLines(abilityPanelX, abilityPanelY, abilityPanelWidth, abilityPanelHeight, Fade(WHITE, 0.16f));
    DrawText(hero.name.c_str(), abilityPanelX + 12, abilityPanelY + 8, 18, heroColor);

    const auto drawAbility = [abilityPanelY, heroColor](const char* key, const char* label, const std::string& stateText, int x)
    {
        DrawRectangle(x, abilityPanelY + 26, 106, 32, Fade(Color { 18, 21, 29, 255 }, 0.74f));
        DrawRectangleLines(x, abilityPanelY + 26, 106, 32, Fade(heroColor, 0.36f));
        DrawText(key, x + 7, abilityPanelY + 32, 15, heroColor);
        DrawText(label, x + 28, abilityPanelY + 30, 11, Fade(WHITE, 0.82f));
        DrawText(stateText.c_str(), x + 28, abilityPanelY + 44, 10, Fade(WHITE, 0.62f));
    };
    const int abilityX = abilityPanelX + 12;
    drawAbility(heroActive1KeyText, "Активка 1", AbilityStateText(heroState.active1, false, heroState.ultimateCharge, false), abilityX);
    drawAbility(heroActive2KeyText, "Активка 2", AbilityStateText(heroState.active2, false, heroState.ultimateCharge, false), abilityX + 116);
    drawAbility(heroUltimateKeyText, "Ульта", AbilityStateText(heroState.ultimate, true, heroState.ultimateCharge, heroState.ultimatePrimed), abilityX + 232);

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

    int feedY = 18;
    for (const EventMessage& event : eventMessages)
    {
        const float t = 1.0f - std::clamp(event.age / std::max(0.001f, event.lifetime), 0.0f, 1.0f);
        const int width = MeasureText(event.text.c_str(), 20) + 28;
        const int x = GetScreenWidth() / 2 - width / 2;
        DrawRectangle(x, feedY - 4, width, 28, Fade(BLACK, 0.42f * t));
        DrawText(event.text.c_str(), x + 14, feedY, 20, Fade(event.color, t));
        feedY += 32;
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
        return Color { 70, 134, 62, 245 };
    case BlockType::TeamBlock:
    case BlockType::WoolBlock:
    {
        const Team* team = FindTeam(teams, block.teamId);
        return team != nullptr ? GetTeamColor(team->color) : Color { 150, 150, 150, 255 };
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
    case BlockType::ResourceGenerator:
        return Color { 82, 82, 92, 255 };
    case BlockType::EnergyCoreBlock:
        return WHITE;
    case BlockType::Air:
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
    case BlockType::Solid:
    case BlockType::StoneBlock:
        return &stoneTexture_;
    case BlockType::ObsidianBlock:
        return &obsidianTexture_;
    case BlockType::EnergyGlassBlock:
        return &glassTexture_;
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
        return Color { 188, 198, 210, 255 };
    case ResourceType::Gold:
        return Color { 246, 196, 74, 255 };
    case ResourceType::Crystal:
        return Color { 112, 232, 255, 255 };
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

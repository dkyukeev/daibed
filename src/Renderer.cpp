#include "Renderer.h"

#include "CombatSystem.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
constexpr int kTextureSize = 32;

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
        return "Lv " + std::to_string(inventory.GetSwordLevel()) + "/" + std::to_string(item.maxLevel);
    case 102:
        return "Lv " + std::to_string(inventory.GetToolLevel()) + "/" + std::to_string(item.maxLevel);
    case 103:
        return "Lv " + std::to_string(inventory.GetArmorLevel()) + "/" + std::to_string(item.maxLevel);
    case 301:
        return "Lv " + std::to_string(team != nullptr ? team->forgeLevel : 0) + "/" + std::to_string(item.maxLevel);
    case 302:
        return "Lv " + std::to_string(team != nullptr ? team->healAuraLevel : 0) + "/" + std::to_string(item.maxLevel);
    case 304:
        return "Lv " + std::to_string(team != nullptr && team->enemyTrackerUnlocked ? 1 : 0) + "/" + std::to_string(item.maxLevel);
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

void DrawHeldItemModel(const ItemStack& stack, Vector3 hand, Vector3 forward, Vector3 right, Vector3 modelUp, float scale, const Texture2D* itemTexture, const Texture2D* blockTexture, Color tint, bool firstPerson = false)
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
            DrawBillboard(Camera3D { Add(center, Scale(forward, -2.0f)), center, up, 45.0f, CAMERA_PERSPECTIVE }, *itemTexture, center, 0.34f * scale, WHITE);
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

    grassTexture_ = LoadProceduralTexture(MakeGrassImage());
    dirtTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 112, 78, 52, 255 }, Color { 86, 58, 38, 255 }, 5));
    leafTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 64, 132, 62, 245 }, Color { 112, 178, 86, 255 }, 6));
    woodTexture_ = LoadProceduralTexture(MakeWoodImage());
    woolTexture_ = LoadProceduralTexture(MakeWoolImage());
    stoneTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 132, 138, 148, 255 }, Color { 88, 94, 108, 255 }, 7));
    obsidianTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 38, 28, 54, 255 }, Color { 90, 52, 132, 255 }, 8));
    glassTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 112, 232, 255, 150 }, Color { 220, 252, 255, 210 }, 9));
    springTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 92, 196, 124, 255 }, Color { 255, 235, 142, 255 }, 10));
    stickyTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 92, 184, 118, 255 }, Color { 40, 112, 72, 255 }, 11));
    tntTexture_ = LoadProceduralTexture(MakeTntImage());
    spikeTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 148, 148, 158, 255 }, Color { 236, 236, 244, 255 }, 12));
    lavaTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 230, 70, 28, 255 }, Color { 255, 210, 66, 255 }, 13));
    iceTexture_ = LoadProceduralTexture(MakeTextureImage(Color { 150, 225, 255, 190 }, Color { 230, 250, 255, 230 }, 14));
    swordIcon_ = LoadProceduralTexture(MakeSwordIcon());
    axeIcon_ = LoadProceduralTexture(MakeAxeIcon());
    spearIcon_ = LoadProceduralTexture(MakeSpearIcon());
    pickaxeIcon_ = LoadProceduralTexture(MakePickaxeIcon());
    arrowIcon_ = LoadProceduralTexture(MakeArrowIcon());
    fireballIcon_ = LoadProceduralTexture(MakeFireballIcon());
    medKitIcon_ = LoadProceduralTexture(MakeMedKitIcon());
    homeIcon_ = LoadProceduralTexture(MakeHomeIcon());
    dashIcon_ = LoadProceduralTexture(MakeDashIcon());
    molotovIcon_ = LoadProceduralTexture(MakeMolotovIcon());
    alarmIcon_ = LoadProceduralTexture(MakeAlarmIcon());
    ironIcon_ = LoadProceduralTexture(MakeResourceIcon(Color { 188, 198, 210, 255 }, WHITE));
    goldIcon_ = LoadProceduralTexture(MakeResourceIcon(Color { 246, 196, 74, 255 }, Color { 255, 246, 180, 255 }));
    crystalIcon_ = LoadProceduralTexture(MakeResourceIcon(Color { 112, 232, 255, 255 }, Color { 225, 252, 255, 255 }));

    texturesReady_ = true;
    return true;
}

void Renderer::Shutdown()
{
    if (!texturesReady_)
    {
        return;
    }

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
    texturesReady_ = false;
}

void Renderer::RenderScene(
    const World& world,
    const std::vector<Team>& teams,
    const std::vector<EnergyCore>& cores,
    const std::vector<Player>& players,
        const std::vector<Generator>& generators,
        const std::vector<ResourcePickup>& pickups,
        const std::vector<DroppedItem>& droppedItems,
        const PlacementPreview& placementPreview,
    const std::vector<WorldEffect>& worldEffects,
    const std::vector<FloatingText>& floatingTexts,
    const Camera3D& camera,
    const ItemStack& localHeldItem,
    Color skyColor,
    bool hideLocalPlayer) const
{
    int localTeamId = -1;
    for (const Player& player : players)
    {
        if (player.IsLocal())
        {
            localTeamId = player.GetTeamId();
            break;
        }
    }

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

    for (const auto& entry : world.GetBlocks())
    {
        const GridPos& pos = entry.first;
        const Block& block = entry.second;
        if (block.type == BlockType::EnergyCoreBlock)
        {
            continue;
        }

        const Vector3 center = world.GridToWorld(pos);
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
    }

    for (const Team& team : teams)
    {
        DrawCylinder(team.shopPosition, 1.7f, 1.7f, 0.08f, 24, Fade(GetTeamColor(team.color), 0.35f));
        DrawCylinderWires(team.shopPosition, 1.7f, 1.7f, 0.08f, 24, GetTeamColor(team.color));
    }

    for (const Generator& generator : generators)
    {
        const Vector3 pos = generator.GetPosition();
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
        const Vector3 pos = world.GridToWorld(core.GetBlockPosition());
        const float healthFraction = static_cast<float>(core.GetHealth()) / static_cast<float>(std::max(1, core.GetMaxHealth()));
        const float stageScale = 0.72f + healthFraction * 0.28f;
        const Vector3 coreCenter { pos.x, pos.y + 0.15f, pos.z };
        DrawCube(coreCenter, 1.0f * stageScale, 1.3f * stageScale, 1.0f * stageScale, Fade(teamColor, 0.70f + healthFraction * 0.22f));
        DrawSphere(Vector3 { pos.x, pos.y + 0.95f * stageScale, pos.z }, 0.28f + 0.10f * healthFraction, Color { 178, 245, 255, 255 });
        DrawCubeWires(coreCenter, 1.05f * stageScale, 1.35f * stageScale, 1.05f * stageScale, WHITE);
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
        if (!player.IsAlive())
        {
            continue;
        }
        if (hideLocalPlayer && player.IsLocal())
        {
            continue;
        }

        const Team* team = FindTeam(teams, player.GetTeamId());
        const Color color = team != nullptr ? GetTeamColor(team->color) : WHITE;
        const bool enemy = localTeamId >= 0 && player.GetTeamId() != localTeamId;
        const Vector3 pos = player.GetPosition();
        DrawCube(pos, 0.68f, 1.7f, 0.68f, color);
        DrawSphere(Vector3 { pos.x, pos.y + 1.08f, pos.z }, 0.36f, Color { 238, 230, 210, 255 });
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
        const ItemStack heldItem = VisibleHeldItemForPlayer(player, localHeldItem);
        if (!heldItem.IsEmpty())
        {
            const std::optional<BlockType> block = ItemToBlock(heldItem.type);
            const Texture2D* blockTexture = block.has_value() ? GetBlockTexture(*block) : nullptr;
            Color itemTint = ItemUiColor(heldItem.type);
            if (block.has_value())
            {
                itemTint = GetBlockColor(Block { *block, player.GetTeamId(), true }, teams);
            }
            DrawHeldItemModel(
                heldItem,
                Vector3 { pos.x + right.x * 0.42f + forward.x * 0.14f, pos.y + 0.36f, pos.z + right.z * 0.42f + forward.z * 0.14f },
                forward,
                right,
                Vector3 { 0.0f, 1.0f, 0.0f },
                0.72f,
                GetItemTexture(heldItem.type),
                blockTexture,
                itemTint);
        }
        DrawLine3D(
            Vector3 { pos.x, pos.y + 0.35f, pos.z },
            Vector3 { pos.x + forward.x * 1.2f, pos.y + 0.35f, pos.z + forward.z * 1.2f },
            enemy ? Color { 255, 118, 118, 255 } : WHITE);

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
        const Vector3 cameraForward = Normalize(Vector3 {
            camera.target.x - camera.position.x,
            camera.target.y - camera.position.y,
            camera.target.z - camera.position.z
        });
        const Vector3 cameraRight = Normalize(Cross(cameraForward, camera.up));
        const Vector3 cameraUp = Normalize(Cross(cameraRight, cameraForward));
        const Vector3 hand = Add(
            Add(camera.position, Scale(cameraForward, 0.52f)),
            Add(Scale(cameraRight, 0.44f), Scale(cameraUp, -0.34f)));
        const std::optional<BlockType> block = ItemToBlock(localHeldItem.type);
        const Texture2D* blockTexture = block.has_value() ? GetBlockTexture(*block) : nullptr;
        Color itemTint = ItemUiColor(localHeldItem.type);
        if (block.has_value() && localTeamId >= 0)
        {
            itemTint = GetBlockColor(Block { *block, localTeamId, true }, teams);
        }
        const float firstPersonScale = ItemIsBlock(localHeldItem.type) ? 0.54f : 0.66f;
        DrawHeldItemModel(localHeldItem, hand, cameraForward, cameraRight, cameraUp, firstPersonScale, GetItemTexture(localHeldItem.type), blockTexture, itemTint, true);
    }

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
            rlDisableDepthMask();
            DrawBillboard(camera, *texture, pos, 0.46f, WHITE);
            rlEnableDepthMask();
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
            rlDisableDepthMask();
            DrawBillboard(camera, *texture, pos, 0.46f, WHITE);
            rlEnableDepthMask();
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
        const float t = 1.0f - std::clamp(effect.age / std::max(0.001f, effect.lifetime), 0.0f, 1.0f);
        const float radius = effect.radius + effect.age * 1.4f;
        DrawSphere(effect.position, radius, Fade(effect.color, t * 0.65f));
        DrawSphereWires(effect.position, radius * 1.08f, 8, 10, Fade(WHITE, t));
    }

    DrawDistantFog(skyColor);

    EndMode3D();

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
            DrawText("SHIELD", x, y + 29, 10, Color { 112, 232, 255, 255 });
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
        DrawText("Shop zone: R", 28, GetScreenHeight() - 40, 18, Color { 125, 230, 255, 255 });
    }

    if (shopOpen)
    {
        const int panelWidth = 790;
        const int panelHeight = 420;
        const int panelX = GetScreenWidth() / 2 - panelWidth / 2;
        const int panelY = GetScreenHeight() / 2 - panelHeight / 2;
        DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(Color { 8, 10, 14, 255 }, 0.88f));
        DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.22f));
        DrawText("Team Shop", panelX + 24, panelY + 20, 28, WHITE);
        DrawText((std::string("Category: < ") + shop.GetCategoryName(shopCategoryIndex) + " >").c_str(), panelX + 24, panelY + 54, 18, Color { 255, 235, 142, 255 });
        DrawText(shop.GetMenuText().c_str(), panelX + 330, panelY + 56, 15, Fade(WHITE, 0.68f));

        int rowY = panelY + 86;
        const std::vector<ShopItem> visibleItems = shop.GetItemsForCategory(shopCategoryIndex);
        for (int row = 0; row < static_cast<int>(visibleItems.size()); ++row)
        {
            const ShopItem& item = visibleItems[row];
            const bool maxed = IsAtMax(item, inventory, playerTeam);
            const bool affordable = shop.CanAfford(inventory, item) && !maxed;
            const Color rowColor = affordable ? WHITE : Fade(WHITE, 0.38f);
            DrawRectangle(panelX + 22, rowY - 6, panelWidth - 44, 28, Fade(affordable ? Color { 42, 52, 62, 255 } : Color { 28, 30, 36, 255 }, 0.72f));
            DrawText((std::to_string(row + 1) + "  " + item.category).c_str(), panelX + 34, rowY, 17, rowColor);
            DrawText(item.name.c_str(), panelX + 138, rowY, 17, rowColor);
            DrawText(item.description.c_str(), panelX + 286, rowY, 16, Fade(rowColor, 0.88f));
            DrawText(LevelText(item, inventory, playerTeam).c_str(), panelX + 548, rowY, 16, Fade(rowColor, 0.78f));
            DrawText(maxed ? "MAX" : CostText(item).c_str(), panelX + 618, rowY, 16, affordable ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 });
            rowY += 32;
        }
    }

    const int centerX = GetScreenWidth() / 2;
    const int centerY = GetScreenHeight() / 2;
    if (!shopOpen && !inventoryOpen)
    {
        Color crosshairColor = placementPreview.visible
            ? (placementPreview.valid ? Color { 128, 238, 166, 255 } : Color { 255, 130, 130, 255 })
            : WHITE;
        if (!placementPreview.visible && combatPreview.targetInRange)
        {
            crosshairColor = combatPreview.ready ? Color { 255, 235, 142, 255 } : Color { 188, 198, 210, 255 };
        }
        DrawCircleLines(centerX, centerY, 6.0f, crosshairColor);
        DrawLine(centerX - 17, centerY, centerX - 9, centerY, Fade(crosshairColor, 0.72f));
        DrawLine(centerX + 9, centerY, centerX + 17, centerY, Fade(crosshairColor, 0.72f));
        DrawLine(centerX, centerY - 17, centerX, centerY - 9, Fade(crosshairColor, 0.72f));
        DrawLine(centerX, centerY + 9, centerX, centerY + 17, Fade(crosshairColor, 0.72f));
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
    DrawBar(
        Rectangle { static_cast<float>(centerX - hpWidth / 2), static_cast<float>(hpY), static_cast<float>(hpWidth), static_cast<float>(hpHeight) },
        healthFraction,
        Color { 224, 42, 58, 255 },
        Fade(Color { 92, 8, 18, 255 }, 0.72f),
        Fade(WHITE, 0.34f));
    const std::string hpText = std::to_string(localPlayer.GetHealth()) + "/" + std::to_string(localPlayer.GetMaxHealth());
    DrawText(hpText.c_str(), centerX - MeasureText(hpText.c_str(), 16) / 2, hpY + 2, 16, WHITE);
    const auto& hotbar = inventory.GetHotbarSlots();
    for (int i = 0; i < kHotbarSlotCount; ++i)
    {
        const int x = hotbarX + i * (slotSize + gap);
        drawSlot(hotbar[i], x, hotbarY, slotSize, i == selectedHotbarSlot, inventoryOpen && inventoryCursorSlot == i);
    }

    if (inventoryOpen)
    {
        const int panelWidth = hotbarWidth + 42;
        const int panelHeight = 322;
        const int panelX = centerX - panelWidth / 2;
        const int panelY = GetScreenHeight() / 2 - panelHeight / 2;
        DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(Color { 8, 10, 14, 255 }, 0.90f));
        DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.22f));
        DrawText("Inventory", panelX + 20, panelY + 16, 24, WHITE);

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
        const std::string title = (winner != nullptr ? winner->name : "Unknown") + " team wins!";
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.45f));
        DrawText(title.c_str(), GetScreenWidth() / 2 - MeasureText(title.c_str(), 42) / 2, GetScreenHeight() / 2 - 42, 42, WHITE);
        const std::string combatStats = "Time " + std::to_string(static_cast<int>(matchTime)) + "s"
            + " | K/D " + std::to_string(stats.kills) + "/" + std::to_string(stats.deaths)
            + " | Hits " + std::to_string(stats.hitsDealt)
            + " | Damage " + std::to_string(stats.damageDealt)
            + " | Core " + std::to_string(stats.coreDamageDealt);
        const std::string economyStats = "Cores " + std::to_string(stats.coresDestroyed)
            + " | Blocks " + std::to_string(stats.blocksPlaced) + "/" + std::to_string(stats.blocksBroken)
            + " | Resources " + std::to_string(stats.resourcesPicked);
        DrawText(combatStats.c_str(), GetScreenWidth() / 2 - MeasureText(combatStats.c_str(), 22) / 2, GetScreenHeight() / 2 + 8, 22, Color { 220, 220, 220, 255 });
        DrawText(economyStats.c_str(), GetScreenWidth() / 2 - MeasureText(economyStats.c_str(), 22) / 2, GetScreenHeight() / 2 + 38, 22, Color { 220, 220, 220, 255 });
        DrawText("Enter restart | ESC quit", GetScreenWidth() / 2 - 132, GetScreenHeight() / 2 + 76, 24, Color { 220, 220, 220, 255 });
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

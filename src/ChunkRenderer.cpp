#include "ChunkRenderer.h"

#include "raymath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

namespace
{
struct MeshBuilder
{
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<float> texcoords;
    std::vector<unsigned char> colors;
    Texture2D texture {};
    Texture2D normalTexture {};
};

struct FaceDefinition
{
    GridPos neighbor;
    Vector3 normal;
    std::array<Vector3, 4> corners;
};

constexpr std::array<FaceDefinition, 6> kFaces {
    FaceDefinition { GridPos { 1, 0, 0 }, Vector3 { 1, 0, 0 }, { Vector3 { .5f, -.5f, -.5f }, Vector3 { .5f, .5f, -.5f }, Vector3 { .5f, .5f, .5f }, Vector3 { .5f, -.5f, .5f } } },
    FaceDefinition { GridPos { -1, 0, 0 }, Vector3 { -1, 0, 0 }, { Vector3 { -.5f, -.5f, .5f }, Vector3 { -.5f, .5f, .5f }, Vector3 { -.5f, .5f, -.5f }, Vector3 { -.5f, -.5f, -.5f } } },
    FaceDefinition { GridPos { 0, 1, 0 }, Vector3 { 0, 1, 0 }, { Vector3 { -.5f, .5f, -.5f }, Vector3 { -.5f, .5f, .5f }, Vector3 { .5f, .5f, .5f }, Vector3 { .5f, .5f, -.5f } } },
    FaceDefinition { GridPos { 0, -1, 0 }, Vector3 { 0, -1, 0 }, { Vector3 { -.5f, -.5f, .5f }, Vector3 { -.5f, -.5f, -.5f }, Vector3 { .5f, -.5f, -.5f }, Vector3 { .5f, -.5f, .5f } } },
    FaceDefinition { GridPos { 0, 0, 1 }, Vector3 { 0, 0, 1 }, { Vector3 { .5f, -.5f, .5f }, Vector3 { .5f, .5f, .5f }, Vector3 { -.5f, .5f, .5f }, Vector3 { -.5f, -.5f, .5f } } },
    FaceDefinition { GridPos { 0, 0, -1 }, Vector3 { 0, 0, -1 }, { Vector3 { -.5f, -.5f, -.5f }, Vector3 { -.5f, .5f, -.5f }, Vector3 { .5f, .5f, -.5f }, Vector3 { .5f, -.5f, -.5f } } }
};

// Two triangulations of the face quad.  The flipped variant is chosen when
// baked AO is stronger across the 1-3 diagonal, which removes the classic
// "bent quad" interpolation artifact in dark corners.
constexpr int kTriangleIndices[] { 0, 1, 2, 0, 2, 3 };
constexpr int kTriangleIndicesFlipped[] { 1, 2, 3, 1, 3, 0 };
constexpr Vector2 kFaceUv[] {
    Vector2 { 0.0f, 1.0f }, Vector2 { 0.0f, 0.0f },
    Vector2 { 1.0f, 0.0f }, Vector2 { 1.0f, 1.0f }
};

// Occlusion level 0 (two solid edge neighbors) .. 3 (fully open corner).
constexpr float kCornerAoLevels[] { 0.52f, 0.70f, 0.86f, 1.0f };

void AppendFace(
    MeshBuilder& builder,
    const GridPos& pos,
    const FaceDefinition& face,
    Color color,
    const std::array<float, 4>& cornerAo)
{
    const bool flip = cornerAo[0] + cornerAo[2] < cornerAo[1] + cornerAo[3];
    const int* indices = flip ? kTriangleIndicesFlipped : kTriangleIndices;
    for (int i = 0; i < 6; ++i)
    {
        const int cornerIndex = indices[i];
        const Vector3& corner = face.corners[cornerIndex];
        builder.vertices.push_back(static_cast<float>(pos.x) + corner.x);
        builder.vertices.push_back(static_cast<float>(pos.y) + corner.y);
        builder.vertices.push_back(static_cast<float>(pos.z) + corner.z);
        builder.normals.push_back(face.normal.x);
        builder.normals.push_back(face.normal.y);
        builder.normals.push_back(face.normal.z);
        builder.texcoords.push_back(kFaceUv[cornerIndex].x);
        builder.texcoords.push_back(kFaceUv[cornerIndex].y);
        builder.colors.push_back(color.r);
        builder.colors.push_back(color.g);
        builder.colors.push_back(color.b);
        // The lighting shader reads the chunk vertex alpha as an ambient
        // occlusion factor, not as coverage; opaque chunk faces always
        // start from alpha 255.
        builder.colors.push_back(static_cast<unsigned char>(
            static_cast<float>(color.a) * cornerAo[cornerIndex] + 0.5f));
    }
}

struct Box
{
    float minX = -0.5f;
    float minY = -0.5f;
    float minZ = -0.5f;
    float maxX = 0.5f;
    float maxY = 0.5f;
    float maxZ = 0.5f;
};

void AppendQuad(
    MeshBuilder& builder,
    const GridPos& pos,
    const std::array<Vector3, 4>& corners,
    Vector3 normal,
    Color color)
{
    for (const int cornerIndex : kTriangleIndices)
    {
        const Vector3& corner = corners[cornerIndex];
        builder.vertices.push_back(static_cast<float>(pos.x) + corner.x);
        builder.vertices.push_back(static_cast<float>(pos.y) + corner.y);
        builder.vertices.push_back(static_cast<float>(pos.z) + corner.z);
        builder.normals.push_back(normal.x);
        builder.normals.push_back(normal.y);
        builder.normals.push_back(normal.z);
        builder.texcoords.push_back(kFaceUv[cornerIndex].x);
        builder.texcoords.push_back(kFaceUv[cornerIndex].y);
        builder.colors.push_back(color.r);
        builder.colors.push_back(color.g);
        builder.colors.push_back(color.b);
        builder.colors.push_back(color.a);
    }
}

// Unlike full voxel faces, the fixtures below use several small cuboids.  A
// box helper keeps their normals and UVs consistent with the regular chunk
// mesh (including its tangent-space normal-map basis in lighting.fs).
void AppendBox(MeshBuilder& builder, const GridPos& pos, const Box& box, Color color)
{
    const Vector3 p000 { box.minX, box.minY, box.minZ };
    const Vector3 p001 { box.minX, box.minY, box.maxZ };
    const Vector3 p010 { box.minX, box.maxY, box.minZ };
    const Vector3 p011 { box.minX, box.maxY, box.maxZ };
    const Vector3 p100 { box.maxX, box.minY, box.minZ };
    const Vector3 p101 { box.maxX, box.minY, box.maxZ };
    const Vector3 p110 { box.maxX, box.maxY, box.minZ };
    const Vector3 p111 { box.maxX, box.maxY, box.maxZ };

    AppendQuad(builder, pos, { p100, p110, p111, p101 }, Vector3 { 1.0f, 0.0f, 0.0f }, color);
    AppendQuad(builder, pos, { p001, p011, p010, p000 }, Vector3 { -1.0f, 0.0f, 0.0f }, color);
    AppendQuad(builder, pos, { p010, p011, p111, p110 }, Vector3 { 0.0f, 1.0f, 0.0f }, color);
    AppendQuad(builder, pos, { p001, p000, p100, p101 }, Vector3 { 0.0f, -1.0f, 0.0f }, color);
    AppendQuad(builder, pos, { p101, p111, p011, p001 }, Vector3 { 0.0f, 0.0f, 1.0f }, color);
    AppendQuad(builder, pos, { p000, p010, p110, p100 }, Vector3 { 0.0f, 0.0f, -1.0f }, color);
}

bool IsInvisibleVisual(BlockType type)
{
    return type == BlockType::BarrierBlock;
}

bool IsPartialVisual(BlockType type)
{
    switch (type)
    {
    case BlockType::StoneSlabBlock:
    case BlockType::StoneBrickSlabBlock:
    case BlockType::StoneBrickStairsBlock:
    case BlockType::BirchSlabBlock:
    case BlockType::BirchStairsBlock:
    case BlockType::IronBarsBlock:
    case BlockType::LadderBlock:
    case BlockType::TorchBlock:
        return true;
    default:
        return false;
    }
}

bool IsFullCubeVisual(BlockType type)
{
    return type != BlockType::Air
        && type != BlockType::EnergyCoreBlock
        && !IsInvisibleVisual(type)
        && !IsPartialVisual(type);
}

void AppendLadder(MeshBuilder& builder, const GridPos& pos, int variant, Color color)
{
    // Legacy ladder data: 2=N, 3=S, 4=W, 5=E.  A malformed/old value still
    // receives a readable north-facing ladder rather than vanishing.
    const int direction = variant & 0x07;
    const bool alongX = direction != 4 && direction != 5;
    const float plane = direction == 3 || direction == 5 ? 0.455f : -0.455f;
    constexpr float rail = 0.045f;
    constexpr float depth = 0.040f;
    if (alongX)
    {
        AppendBox(builder, pos, Box { -0.36f, -0.50f, plane - depth, -0.27f, 0.50f, plane + depth }, color);
        AppendBox(builder, pos, Box { 0.27f, -0.50f, plane - depth, 0.36f, 0.50f, plane + depth }, color);
        for (const float y : { -0.30f, 0.00f, 0.30f })
        {
            AppendBox(builder, pos, Box { -0.36f, y - rail, plane - depth, 0.36f, y + rail, plane + depth }, color);
        }
    }
    else
    {
        AppendBox(builder, pos, Box { plane - depth, -0.50f, -0.36f, plane + depth, 0.50f, -0.27f }, color);
        AppendBox(builder, pos, Box { plane - depth, -0.50f, 0.27f, plane + depth, 0.50f, 0.36f }, color);
        for (const float y : { -0.30f, 0.00f, 0.30f })
        {
            AppendBox(builder, pos, Box { plane - depth, y - rail, -0.36f, plane + depth, y + rail, 0.36f }, color);
        }
    }
}

void AppendIronBars(MeshBuilder& builder, const GridPos& pos, const World& world, Color color)
{
    constexpr float rod = 0.045f;
    // Bars connect toward adjacent bars and toward any full solid block, the
    // way Minecraft panes join into continuous fences.
    const auto connects = [&world, &pos](int dx, int dz)
    {
        const Block* neighbor = world.GetBlock(GridPos { pos.x + dx, pos.y, pos.z + dz });
        return neighbor != nullptr
            && (neighbor->type == BlockType::IronBarsBlock || IsFullCubeVisual(neighbor->type));
    };
    const bool connectNorth = connects(0, -1);
    const bool connectSouth = connects(0, 1);
    const bool connectWest = connects(-1, 0);
    const bool connectEast = connects(1, 0);

    if (!connectNorth && !connectSouth && !connectWest && !connectEast)
    {
        // A lone block keeps the legacy free-standing cross.
        for (const float offset : { -0.34f, 0.0f, 0.34f })
        {
            AppendBox(builder, pos, Box { offset - rod, -0.5f, -rod, offset + rod, 0.5f, rod }, color);
            AppendBox(builder, pos, Box { -rod, -0.5f, offset - rod, rod, 0.5f, offset + rod }, color);
        }
        for (const float y : { -0.34f, 0.34f })
        {
            AppendBox(builder, pos, Box { -0.43f, y - rod, -rod, 0.43f, y + rod, rod }, color);
            AppendBox(builder, pos, Box { -rod, y - rod, -0.43f, rod, y + rod, 0.43f }, color);
        }
        return;
    }

    // Center post plus, per connected side, a half-pane of two vertical rods
    // and two rails running all the way to the cell edge so adjacent bars
    // meet seamlessly.
    AppendBox(builder, pos, Box { -rod, -0.5f, -rod, rod, 0.5f, rod }, color);
    const auto appendSegment = [&](int dx, int dz)
    {
        for (const float along : { 0.19f, 0.36f })
        {
            const float centerX = static_cast<float>(dx) * along;
            const float centerZ = static_cast<float>(dz) * along;
            AppendBox(builder, pos, Box {
                centerX - rod, -0.5f, centerZ - rod,
                centerX + rod, 0.5f, centerZ + rod }, color);
        }
        const float farX = dx == 0 ? rod : (dx < 0 ? -0.5f : 0.5f);
        const float nearX = dx == 0 ? -rod : (dx < 0 ? -rod : rod);
        const float farZ = dz == 0 ? rod : (dz < 0 ? -0.5f : 0.5f);
        const float nearZ = dz == 0 ? -rod : (dz < 0 ? -rod : rod);
        for (const float y : { -0.34f, 0.34f })
        {
            AppendBox(builder, pos, Box {
                std::min(nearX, farX), y - rod, std::min(nearZ, farZ),
                std::max(nearX, farX), y + rod, std::max(nearZ, farZ) }, color);
        }
    };
    if (connectNorth) appendSegment(0, -1);
    if (connectSouth) appendSegment(0, 1);
    if (connectWest) appendSegment(-1, 0);
    if (connectEast) appendSegment(1, 0);
}

GridPos StairRaisedDirection(int variant)
{
    switch (variant & 0x03)
    {
    case 0: return GridPos { -1, 0, 0 }; // east-facing front, raised west
    case 1: return GridPos { 1, 0, 0 };  // west-facing front, raised east
    case 2: return GridPos { 0, 0, -1 }; // south-facing front, raised north
    default: return GridPos { 0, 0, 1 }; // north-facing front, raised south
    }
}

bool IsStairsBlock(BlockType type)
{
    return type == BlockType::StoneBrickStairsBlock || type == BlockType::BirchStairsBlock;
}

// Horizontal footprint that starts as the full cell and gets clipped to the
// half toward a direction; intersecting two perpendicular halves yields the
// quarter used by corner stairs.
struct FootprintXZ
{
    float minX = -0.5f;
    float maxX = 0.5f;
    float minZ = -0.5f;
    float maxZ = 0.5f;
};

void ClipTowards(FootprintXZ& footprint, const GridPos& direction)
{
    if (direction.x < 0) footprint.maxX = 0.0f;
    else if (direction.x > 0) footprint.minX = 0.0f;
    if (direction.z < 0) footprint.maxZ = 0.0f;
    else if (direction.z > 0) footprint.minZ = 0.0f;
}

void AppendStairs(MeshBuilder& builder, const GridPos& pos, const World& world, int variant, Color color)
{
    const bool upsideDown = (variant & 0x04) != 0;
    const float fullMinY = upsideDown ? 0.0f : -0.5f;
    const float fullMaxY = upsideDown ? 0.5f : 0.0f;
    const float stepMinY = upsideDown ? -0.5f : 0.0f;
    const float stepMaxY = upsideDown ? 0.0f : 0.5f;
    AppendBox(builder, pos, Box { -0.5f, fullMinY, -0.5f, 0.5f, fullMaxY, 0.5f }, color);

    // The legacy four-bit payload has no corner blockstate, so corners are
    // derived the way Minecraft does it: a perpendicular stair behind the
    // raised half shrinks it to a quarter (outer corner), a perpendicular
    // stair in front adds an extra quarter (inner corner).
    const GridPos raisedDir = StairRaisedDirection(variant);
    const auto perpendicularStair = [&world, upsideDown, &raisedDir](const GridPos& at, GridPos& outRaisedDir)
    {
        const Block* neighbor = world.GetBlock(at);
        if (neighbor == nullptr || !IsStairsBlock(neighbor->type))
        {
            return false;
        }
        if (((neighbor->variant & 0x04) != 0) != upsideDown)
        {
            return false;
        }
        const GridPos otherDir = StairRaisedDirection(neighbor->variant);
        if (otherDir.x * raisedDir.x + otherDir.z * raisedDir.z != 0)
        {
            return false;
        }
        outRaisedDir = otherDir;
        return true;
    };

    FootprintXZ raised {};
    ClipTowards(raised, raisedDir);
    GridPos behindDir {};
    if (perpendicularStair(GridPos { pos.x + raisedDir.x, pos.y, pos.z + raisedDir.z }, behindDir))
    {
        // Outer corner: only the shared quarter stays raised.
        ClipTowards(raised, behindDir);
    }
    AppendBox(builder, pos, Box { raised.minX, stepMinY, raised.minZ, raised.maxX, stepMaxY, raised.maxZ }, color);

    GridPos frontDir {};
    if (perpendicularStair(GridPos { pos.x - raisedDir.x, pos.y, pos.z - raisedDir.z }, frontDir))
    {
        // Inner corner: an extra quarter on the low half completes the L.
        FootprintXZ extra {};
        ClipTowards(extra, GridPos { -raisedDir.x, 0, -raisedDir.z });
        ClipTowards(extra, frontDir);
        AppendBox(builder, pos, Box { extra.minX, stepMinY, extra.minZ, extra.maxX, stepMaxY, extra.maxZ }, color);
    }
}

bool AppendPartialVisual(MeshBuilder& builder, const GridPos& pos, const Block& block, Color color, const World& world)
{
    switch (block.type)
    {
    case BlockType::StoneSlabBlock:
    case BlockType::StoneBrickSlabBlock:
    case BlockType::BirchSlabBlock:
        if ((block.variant & 0x08) != 0)
        {
            AppendBox(builder, pos, Box { -0.5f, 0.0f, -0.5f, 0.5f, 0.5f, 0.5f }, color);
        }
        else
        {
            AppendBox(builder, pos, Box { -0.5f, -0.5f, -0.5f, 0.5f, 0.0f, 0.5f }, color);
        }
        return true;
    case BlockType::StoneBrickStairsBlock:
    case BlockType::BirchStairsBlock:
        AppendStairs(builder, pos, world, block.variant, color);
        return true;
    case BlockType::IronBarsBlock:
        AppendIronBars(builder, pos, world, color);
        return true;
    case BlockType::LadderBlock:
        AppendLadder(builder, pos, block.variant, color);
        return true;
    case BlockType::TorchBlock:
    {
        // Legacy Minecraft torch data: 1..4 = wall bracket leaning
        // +X/-X/+Z/-Z away from the wall behind it, 0/5 = standing.
        float leanX = 0.0f;
        float leanZ = 0.0f;
        switch (block.variant & 0x07)
        {
        case 1: leanX = 1.0f; break;
        case 2: leanX = -1.0f; break;
        case 3: leanZ = 1.0f; break;
        case 4: leanZ = -1.0f; break;
        default: break;
        }
        if (leanX == 0.0f && leanZ == 0.0f)
        {
            // A narrow wood stem plus a small flame cube makes torches
            // legible from a distance and leaves open space around them for
            // the shader point light to illuminate.
            AppendBox(builder, pos, Box { -0.055f, -0.50f, -0.055f, 0.055f, 0.18f, 0.055f }, color);
            AppendBox(builder, pos, Box { -0.14f, 0.14f, -0.14f, 0.14f, 0.39f, 0.14f }, color);
            return true;
        }
        // Wall torch: three stem boxes step out of the wall plane and up,
        // approximating the classic tilted bracket, with the flame at the
        // tip.  "Along" runs from the wall (-0.44) toward the room.
        constexpr float kStemHalf = 0.058f;
        for (int step = 0; step < 3; ++step)
        {
            const float along = -0.44f + 0.15f * static_cast<float>(step);
            const float centerY = -0.10f + 0.13f * static_cast<float>(step);
            const float centerX = leanX * along;
            const float centerZ = leanZ * along;
            const float halfX = leanX != 0.0f ? 0.075f : kStemHalf;
            const float halfZ = leanZ != 0.0f ? 0.075f : kStemHalf;
            AppendBox(builder, pos, Box {
                centerX - halfX, centerY - 0.11f, centerZ - halfZ,
                centerX + halfX, centerY + 0.11f, centerZ + halfZ }, color);
        }
        const float flameX = leanX * -0.14f;
        const float flameZ = leanZ * -0.14f;
        AppendBox(builder, pos, Box {
            flameX - 0.115f, 0.22f, flameZ - 0.115f,
            flameX + 0.115f, 0.45f, flameZ + 0.115f }, color);
        return true;
    }
    default:
        return false;
    }
}

template<typename T>
T* CopyToRaylib(const std::vector<T>& values)
{
    if (values.empty())
    {
        return nullptr;
    }
    T* result = static_cast<T*>(MemAlloc(static_cast<unsigned int>(values.size() * sizeof(T))));
    std::memcpy(result, values.data(), values.size() * sizeof(T));
    return result;
}

}

ChunkRenderer::~ChunkRenderer()
{
    Shutdown();
}

void ChunkRenderer::Sync(
    const World& world,
    const ColorResolver& colorResolver,
    const TextureResolver& textureResolver,
    const TransparencyResolver& transparencyResolver,
    const TextureResolver& normalMapResolver,
    bool bakeAmbientOcclusion) const
{
    if (syncedRevision_ == world.GetRenderRevision())
    {
        return;
    }

    std::vector<GridPos> dirty = world.TakeDirtyRenderChunks();
    if (syncedRevision_ == 0 && dirty.empty())
    {
        for (const auto& entry : world.GetBlocks())
        {
            dirty.push_back(World::RenderChunkForBlock(entry.first));
        }
        std::sort(dirty.begin(), dirty.end(), [](const GridPos& a, const GridPos& b)
        {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            return a.z < b.z;
        });
        dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
    }

    const double start = GetTime();
    for (const GridPos& chunk : dirty)
    {
        RebuildChunk(chunk, world, colorResolver, textureResolver, transparencyResolver, normalMapResolver, bakeAmbientOcclusion);
    }
    stats_.lastRebuildMilliseconds = static_cast<float>((GetTime() - start) * 1000.0);
    syncedRevision_ = world.GetRenderRevision();
}

void ChunkRenderer::Draw(const Camera3D& camera, float drawDistance, Shader shader) const
{
    stats_.visibleChunks = 0;
    stats_.drawCalls = 0;
    stats_.triangles = 0;
    for (const auto& entry : chunks_)
    {
        const ChunkMesh& chunk = entry.second;
        const Vector3 toChunk = Vector3Subtract(chunk.center, camera.position);
        const float radius = static_cast<float>(World::kRenderChunkSize) * 0.9f;
        const float maxDistance = std::max(24.0f, drawDistance) + radius;
        const Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        if (Vector3LengthSqr(toChunk) > maxDistance * maxDistance
            || Vector3DotProduct(forward, toChunk) <= -radius)
        {
            continue;
        }
        ++stats_.visibleChunks;
        stats_.triangles += chunk.triangles;
        for (const SubMesh& subMesh : chunk.opaque)
        {
            Material material = subMesh.material;
            if (shader.id != 0)
            {
                material.shader = shader;
            }
            DrawMesh(subMesh.mesh, material, MatrixIdentity());
            ++stats_.drawCalls;
        }
    }
}

void ChunkRenderer::DrawTransparent(const Camera3D& camera, float drawDistance, Shader shader) const
{
    std::vector<const ChunkMesh*> visible;
    visible.reserve(chunks_.size());
    const Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    const float radius = static_cast<float>(World::kRenderChunkSize) * 0.9f;
    const float maxDistance = std::max(24.0f, drawDistance) + radius;
    const float maxDistanceSq = maxDistance * maxDistance;
    for (const auto& entry : chunks_)
    {
        const ChunkMesh& chunk = entry.second;
        if (chunk.transparent.empty())
        {
            continue;
        }
        const Vector3 toChunk = Vector3Subtract(chunk.center, camera.position);
        if (Vector3LengthSqr(toChunk) > maxDistanceSq
            || Vector3DotProduct(forward, toChunk) <= -radius)
        {
            continue;
        }
        visible.push_back(&chunk);
    }

    // Alpha blending needs back-to-front order.  Per-chunk ordering is a
    // deliberate compromise: it removes the expensive per-block path for
    // large stained-glass walls while remaining stable at normal play ranges.
    std::sort(visible.begin(), visible.end(), [&camera](const ChunkMesh* a, const ChunkMesh* b)
    {
        return Vector3DistanceSqr(a->center, camera.position) > Vector3DistanceSqr(b->center, camera.position);
    });
    for (const ChunkMesh* chunk : visible)
    {
        for (const SubMesh& subMesh : chunk->transparent)
        {
            Material material = subMesh.material;
            if (shader.id != 0)
            {
                material.shader = shader;
            }
            DrawMesh(subMesh.mesh, material, MatrixIdentity());
        }
    }
}

void ChunkRenderer::Shutdown()
{
    for (auto& entry : chunks_)
    {
        UnloadChunk(entry.second);
    }
    chunks_.clear();
    syncedRevision_ = 0;
    stats_ = {};
}

const ChunkRenderStats& ChunkRenderer::GetStats() const
{
    return stats_;
}

void ChunkRenderer::RebuildChunk(
    const GridPos& chunk,
    const World& world,
    const ColorResolver& colorResolver,
    const TextureResolver& textureResolver,
    const TransparencyResolver& transparencyResolver,
    const TextureResolver& normalMapResolver,
    bool bakeAmbientOcclusion) const
{
    ChunkMesh previous;
    const auto existing = chunks_.find(chunk);
    if (existing != chunks_.end())
    {
        previous = std::move(existing->second);
        chunks_.erase(existing);
    }

    const auto occludes = [&world, &colorResolver, &transparencyResolver](const GridPos& pos)
    {
        const Block* block = world.GetBlock(pos);
        return block != nullptr
            && IsFullCubeVisual(block->type)
            && !transparencyResolver(*block, colorResolver(*block));
    };

    std::map<int, MeshBuilder> opaqueBuilders;
    std::map<int, MeshBuilder> transparentBuilders;
    const int minX = chunk.x * World::kRenderChunkSize;
    const int minY = chunk.y * World::kRenderChunkSize;
    const int minZ = chunk.z * World::kRenderChunkSize;
    const int maxX = minX + World::kRenderChunkSize;
    const int maxY = minY + World::kRenderChunkSize;
    const int maxZ = minZ + World::kRenderChunkSize;

    for (int x = minX; x < maxX; ++x)
    {
        for (int y = minY; y < maxY; ++y)
        {
            for (int z = minZ; z < maxZ; ++z)
            {
                const GridPos pos { x, y, z };
                const Block* block = world.GetBlock(pos);
                if (block == nullptr
                    || block->type == BlockType::EnergyCoreBlock
                    || IsInvisibleVisual(block->type))
                {
                    continue;
                }
                const Color color = colorResolver(*block);
                const bool transparent = transparencyResolver(*block, color);
                MeshBuilder& builder = (transparent ? transparentBuilders : opaqueBuilders)[static_cast<int>(block->type)];
                if (const Texture2D* texture = textureResolver(block->type))
                {
                    builder.texture = *texture;
                }
                if (normalMapResolver)
                {
                    if (const Texture2D* normalTexture = normalMapResolver(block->type))
                    {
                        builder.normalTexture = *normalTexture;
                    }
                }

                if (transparent)
                {
                    // Internal faces of a run of equal transparent blocks are
                    // invisible but expensive.  Different materials keep
                    // their separator face so stained panes remain readable.
                    for (const FaceDefinition& face : kFaces)
                    {
                        const GridPos neighborPos {
                            pos.x + face.neighbor.x,
                            pos.y + face.neighbor.y,
                            pos.z + face.neighbor.z
                        };
                        const Block* neighbor = world.GetBlock(neighborPos);
                        // The variant check keeps the separator face between
                        // differently dyed panes of the same glass type.
                        const bool sharesTransparentMaterial = neighbor != nullptr
                            && neighbor->type == block->type
                            && neighbor->variant == block->variant
                            && transparencyResolver(*neighbor, colorResolver(*neighbor));
                        if (!sharesTransparentMaterial)
                        {
                            AppendFace(builder, pos, face, color, { 1.0f, 1.0f, 1.0f, 1.0f });
                        }
                    }
                    continue;
                }

                if (AppendPartialVisual(builder, pos, *block, color, world))
                {
                    continue;
                }
                for (const FaceDefinition& face : kFaces)
                {
                    const GridPos neighborPos {
                        pos.x + face.neighbor.x,
                        pos.y + face.neighbor.y,
                        pos.z + face.neighbor.z
                    };
                    if (occludes(neighborPos))
                    {
                        continue;
                    }

                    std::array<float, 4> cornerAo { 1.0f, 1.0f, 1.0f, 1.0f };
                    if (bakeAmbientOcclusion)
                    {
                        // Low-frequency cavity term complements corner AO:
                        // roofs, tight corridors and recessed openings stay
                        // darker even when no blocker touches this exact
                        // vertex. It is baked only when a chunk changes.
                        bool roofed = false;
                        for (int step = 1; step <= 8; ++step)
                        {
                            if (occludes(GridPos { neighborPos.x, neighborPos.y + step, neighborPos.z }))
                            {
                                roofed = true;
                                break;
                            }
                        }
                        constexpr GridPos enclosureDirections[] {
                            GridPos { 1, 0, 0 }, GridPos { -1, 0, 0 },
                            GridPos { 0, 0, 1 }, GridPos { 0, 0, -1 },
                            GridPos { 0, 1, 0 }
                        };
                        int enclosure = 0;
                        for (const GridPos& direction : enclosureDirections)
                        {
                            bool blocked = false;
                            for (int step = 1; step <= 2; ++step)
                            {
                                const GridPos sample {
                                    neighborPos.x + direction.x * step,
                                    neighborPos.y + direction.y * step,
                                    neighborPos.z + direction.z * step
                                };
                                // Do not count the face's own source block as
                                // enclosure when looking out from a wall.
                                if (sample == pos) continue;
                                if (occludes(sample))
                                {
                                    blocked = true;
                                    break;
                                }
                            }
                            enclosure += blocked ? 1 : 0;
                        }
                        const float cavityAo = (roofed ? 0.78f : 1.0f)
                            * (1.0f - std::min(0.20f, static_cast<float>(enclosure) * 0.045f));

                        // Minecraft-style corner AO: the two edge neighbors
                        // and the diagonal one layer above the face decide
                        // how dark each vertex gets.
                        const int normalAxis = face.neighbor.x != 0 ? 0 : (face.neighbor.y != 0 ? 1 : 2);
                        const int axisU = normalAxis == 0 ? 1 : 0;
                        const int axisV = normalAxis == 2 ? 1 : 2;
                        for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex)
                        {
                            const Vector3& corner = face.corners[cornerIndex];
                            const float cornerComponents[3] { corner.x, corner.y, corner.z };
                            int deltaU[3] { 0, 0, 0 };
                            int deltaV[3] { 0, 0, 0 };
                            deltaU[axisU] = cornerComponents[axisU] > 0.0f ? 1 : -1;
                            deltaV[axisV] = cornerComponents[axisV] > 0.0f ? 1 : -1;
                            const GridPos above {
                                neighborPos.x,
                                neighborPos.y,
                                neighborPos.z
                            };
                            const GridPos sideU { above.x + deltaU[0], above.y + deltaU[1], above.z + deltaU[2] };
                            const GridPos sideV { above.x + deltaV[0], above.y + deltaV[1], above.z + deltaV[2] };
                            const GridPos diagonal {
                                above.x + deltaU[0] + deltaV[0],
                                above.y + deltaU[1] + deltaV[1],
                                above.z + deltaU[2] + deltaV[2]
                            };
                            const bool occludedU = occludes(sideU);
                            const bool occludedV = occludes(sideV);
                            const int level = (occludedU && occludedV)
                                ? 0
                                : 3 - (static_cast<int>(occludedU) + static_cast<int>(occludedV) + static_cast<int>(occludes(diagonal)));
                            cornerAo[cornerIndex] = kCornerAoLevels[level] * cavityAo;
                        }
                    }
                    AppendFace(builder, pos, face, color, cornerAo);
                }
            }
        }
    }

    ChunkMesh result;
    result.coordinate = chunk;
    result.center = Vector3 {
        static_cast<float>(minX) + World::kRenderChunkSize * 0.5f,
        static_cast<float>(minY) + World::kRenderChunkSize * 0.5f,
        static_cast<float>(minZ) + World::kRenderChunkSize * 0.5f
    };
    const auto finalizeBuilders = [&result](
        std::map<int, MeshBuilder>& builders,
        std::vector<SubMesh>& reusableMeshes,
        std::vector<SubMesh>& destination,
        bool countAsOpaque)
    {
        for (auto& entry : builders)
        {
            MeshBuilder& builder = entry.second;
            if (builder.vertices.empty())
            {
                continue;
            }

            SubMesh subMesh;
            subMesh.mesh.vertexCount = static_cast<int>(builder.vertices.size() / 3);
            subMesh.mesh.triangleCount = subMesh.mesh.vertexCount / 3;
            subMesh.materialKey = entry.first;
            auto reusable = std::find_if(reusableMeshes.begin(), reusableMeshes.end(), [&subMesh](const SubMesh& candidate)
            {
                return candidate.materialKey == subMesh.materialKey
                    && candidate.mesh.vertexCount == subMesh.mesh.vertexCount
                    && candidate.mesh.vaoId != 0;
            });
            if (reusable != reusableMeshes.end())
            {
                subMesh = std::move(*reusable);
                std::memcpy(subMesh.mesh.vertices, builder.vertices.data(), builder.vertices.size() * sizeof(float));
                std::memcpy(subMesh.mesh.normals, builder.normals.data(), builder.normals.size() * sizeof(float));
                std::memcpy(subMesh.mesh.texcoords, builder.texcoords.data(), builder.texcoords.size() * sizeof(float));
                std::memcpy(subMesh.mesh.colors, builder.colors.data(), builder.colors.size() * sizeof(unsigned char));
                UpdateMeshBuffer(subMesh.mesh, 0, builder.vertices.data(), static_cast<int>(builder.vertices.size() * sizeof(float)), 0);
                UpdateMeshBuffer(subMesh.mesh, 1, builder.texcoords.data(), static_cast<int>(builder.texcoords.size() * sizeof(float)), 0);
                UpdateMeshBuffer(subMesh.mesh, 2, builder.normals.data(), static_cast<int>(builder.normals.size() * sizeof(float)), 0);
                UpdateMeshBuffer(subMesh.mesh, 3, builder.colors.data(), static_cast<int>(builder.colors.size() * sizeof(unsigned char)), 0);
                reusable->mesh = {};
                reusable->material = {};
            }
            else
            {
                subMesh.mesh.vertices = CopyToRaylib(builder.vertices);
                subMesh.mesh.normals = CopyToRaylib(builder.normals);
                subMesh.mesh.texcoords = CopyToRaylib(builder.texcoords);
                subMesh.mesh.colors = CopyToRaylib(builder.colors);
                UploadMesh(&subMesh.mesh, true);
                subMesh.material = LoadMaterialDefault();
            }
            if (builder.texture.id != 0)
            {
                subMesh.material.maps[MATERIAL_MAP_DIFFUSE].texture = builder.texture;
            }
            if (builder.normalTexture.id != 0)
            {
                // DrawMesh binds this to texture unit 2 and points the shader's
                // "texture2" sampler at it; the lighting shader reads it only
                // while the chunk-pass gate is up.
                subMesh.material.maps[MATERIAL_MAP_NORMAL].texture = builder.normalTexture;
            }
            subMesh.triangles = subMesh.mesh.triangleCount;
            if (countAsOpaque)
            {
                result.triangles += subMesh.triangles;
            }
            destination.push_back(std::move(subMesh));
        }
    };
    finalizeBuilders(opaqueBuilders, previous.opaque, result.opaque, true);
    finalizeBuilders(transparentBuilders, previous.transparent, result.transparent, false);

    if (!result.opaque.empty() || !result.transparent.empty())
    {
        chunks_.emplace(chunk, std::move(result));
    }
    UnloadChunk(previous);
}

void ChunkRenderer::UnloadChunk(ChunkMesh& chunk)
{
    const auto unloadMeshes = [](std::vector<SubMesh>& meshes)
    {
        for (SubMesh& subMesh : meshes)
        {
            if (subMesh.mesh.vaoId != 0 || subMesh.mesh.vertices != nullptr)
            {
                UnloadMesh(subMesh.mesh);
            }
            if (subMesh.material.maps != nullptr)
            {
                // Block textures are borrowed from Renderer. UnloadMaterial()
                // would destroy those shared GPU textures when a dirty chunk is
                // rebuilt, leaving every other chunk with stale texture IDs.
                MemFree(subMesh.material.maps);
                subMesh.material.maps = nullptr;
            }
        }
        meshes.clear();
    };
    unloadMeshes(chunk.opaque);
    unloadMeshes(chunk.transparent);
}

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

constexpr int kTriangleIndices[] { 0, 1, 2, 0, 2, 3 };
constexpr Vector2 kFaceUv[] {
    Vector2 { 0.0f, 1.0f }, Vector2 { 0.0f, 0.0f },
    Vector2 { 1.0f, 0.0f }, Vector2 { 1.0f, 1.0f }
};

void AppendFace(MeshBuilder& builder, const GridPos& pos, const FaceDefinition& face, Color color)
{
    for (int cornerIndex : kTriangleIndices)
    {
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
        builder.colors.push_back(color.a);
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
    const TransparencyResolver& transparencyResolver) const
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
        RebuildChunk(chunk, world, colorResolver, textureResolver, transparencyResolver);
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
    const TransparencyResolver& transparencyResolver) const
{
    ChunkMesh previous;
    const auto existing = chunks_.find(chunk);
    if (existing != chunks_.end())
    {
        previous = std::move(existing->second);
        chunks_.erase(existing);
    }

    std::map<int, MeshBuilder> builders;
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
                if (block == nullptr || block->type == BlockType::EnergyCoreBlock)
                {
                    continue;
                }
                const Color color = colorResolver(*block);
                if (transparencyResolver(*block, color))
                {
                    continue;
                }

                MeshBuilder& builder = builders[static_cast<int>(block->type)];
                if (const Texture2D* texture = textureResolver(block->type))
                {
                    builder.texture = *texture;
                }
                for (const FaceDefinition& face : kFaces)
                {
                    const GridPos neighborPos {
                        pos.x + face.neighbor.x,
                        pos.y + face.neighbor.y,
                        pos.z + face.neighbor.z
                    };
                    const Block* neighbor = world.GetBlock(neighborPos);
                    const bool exposed = neighbor == nullptr
                        || neighbor->type == BlockType::EnergyCoreBlock
                        || transparencyResolver(*neighbor, colorResolver(*neighbor));
                    if (exposed)
                    {
                        AppendFace(builder, pos, face, color);
                    }
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
        auto reusable = std::find_if(previous.opaque.begin(), previous.opaque.end(), [&subMesh](const SubMesh& candidate)
        {
            return candidate.materialKey == subMesh.materialKey
                && candidate.mesh.vertexCount == subMesh.mesh.vertexCount
                && candidate.mesh.vaoId != 0;
        });
        if (reusable != previous.opaque.end())
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
        subMesh.triangles = subMesh.mesh.triangleCount;
        result.triangles += subMesh.triangles;
        result.opaque.push_back(std::move(subMesh));
    }

    if (!result.opaque.empty())
    {
        chunks_.emplace(chunk, std::move(result));
    }
    UnloadChunk(previous);
}

void ChunkRenderer::UnloadChunk(ChunkMesh& chunk)
{
    for (SubMesh& subMesh : chunk.opaque)
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
    chunk.opaque.clear();
}

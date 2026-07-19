#pragma once

#include "World.h"

#include "raylib.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

struct ChunkRenderStats
{
    int visibleChunks = 0;
    int drawCalls = 0;
    int triangles = 0;
    float lastRebuildMilliseconds = 0.0f;
};

class ChunkRenderer
{
public:
    using ColorResolver = std::function<Color(const Block&)>;
    using TextureResolver = std::function<const Texture2D*(BlockType)>;
    using TransparencyResolver = std::function<bool(const Block&, Color)>;

    ~ChunkRenderer();

    void Sync(
        const World& world,
        const ColorResolver& colorResolver,
        const TextureResolver& textureResolver,
        const TransparencyResolver& transparencyResolver,
        const TextureResolver& normalMapResolver = {},
        bool bakeAmbientOcclusion = false) const;
    void Draw(const Camera3D& camera, float drawDistance, Shader shader = {}) const;
    // Transparent geometry is batched per chunk as well, then sorted at the
    // chunk level.  This keeps large imported glass structures out of the
    // legacy per-block DrawCube path while retaining normal alpha blending.
    void DrawTransparent(const Camera3D& camera, float drawDistance, Shader shader = {}) const;
    void Shutdown();

    const ChunkRenderStats& GetStats() const;

private:
    struct SubMesh
    {
        Mesh mesh {};
        Material material {};
        int triangles = 0;
        int materialKey = 0;
    };

    struct ChunkMesh
    {
        GridPos coordinate {};
        Vector3 center {};
        std::vector<SubMesh> opaque;
        std::vector<SubMesh> transparent;
        int triangles = 0;
    };

    void RebuildChunk(
        const GridPos& chunk,
        const World& world,
        const ColorResolver& colorResolver,
        const TextureResolver& textureResolver,
        const TransparencyResolver& transparencyResolver,
        const TextureResolver& normalMapResolver,
        bool bakeAmbientOcclusion) const;
    static void UnloadChunk(ChunkMesh& chunk);

    mutable std::unordered_map<GridPos, ChunkMesh, GridPosHash> chunks_;
    mutable std::uint64_t syncedRevision_ = 0;
    mutable ChunkRenderStats stats_ {};
};

#pragma once

#include "raylib.h"
#include "ShaderSettings.h"
#include "World.h"
#include <functional>

#include <array>
#include <vector>

// Shared with the shadow-map pass: the depth camera must look along the very
// same vector the fragment shader shades with.  Pre-normalized on the CPU.
inline constexpr Vector3 kSceneSunDirection { -0.480264f, -0.820451f, 0.310171f };

// Everything the lighting shader needs to sample a sun shadow map.  Built by
// Renderer::PrepareSunShadows; pass nullptr to SceneShader::Begin to shade
// without shadows (menus, previews, shadowQuality 0).
struct SceneShadowParams
{
    static constexpr int kCascadeCount = 2;
    std::array<Matrix, kCascadeCount> lightViewProj {};
    std::array<unsigned int, kCascadeCount> depthTextureId {};
    std::array<float, kCascadeCount> texelSize {};
    std::array<float, kCascadeCount> worldPerTexel {};
    float strength = 0.0f;
    float splitDistance = 0.0f;
    float distance = 0.0f;
    bool widePcf = false;
};

// Compact CPU-selected local lights.  The renderer keeps just the closest
// torches/lamps each frame, avoiding a world-sized uniform upload.
struct ScenePointLight
{
    Vector3 position {};
    Color color { 255, 214, 126, 255 };
    float radius = 7.0f;
    float intensity = 1.0f;
};

class SceneShader
{
public:
    void SetSettings(const ShaderSettings& settings) { settings_ = settings; settings_.Clamp(); }
    bool Initialize();
    bool DrawSky(const Camera3D& camera, Color skyColor) const;
    void UpdateVoxelLighting(const World& world, const Camera3D& camera,
        const std::function<Color(const Block&)>& colorResolver) const;
    void Shutdown();
    // renderDistance drives the far fog veil (fogStart/fogEnd uniforms):
    // geometry dissolves into the sky right before the draw-distance cull
    // instead of popping.
    void Begin(
        const Camera3D& camera,
        Color fogColor,
        float fogDensity,
        float renderDistance,
        const SceneShadowParams* shadow = nullptr) const;
    void End() const;
    void SetPointLights(const std::vector<ScenePointLight>& lights) const;
    void SetAmbientOcclusionQuality(int quality) const;
    // Explicit emissive classification prevents bright wool/glass/stone from
    // entering bloom merely because its albedo is saturated. Flushes the
    // raylib batch before changing the uniform so adjacent draw groups cannot
    // inherit one another's material class.
    void SetEmissiveStrength(float strength) const;
    // Chunk meshes carry baked AO in the vertex alpha and a normal map in
    // MATERIAL_MAP_NORMAL; batched draws carry neither.  Toggle around
    // ChunkRenderer::Draw only.
    void SetChunkPassFeatures(bool enabled) const;
    Shader GetShader() const;
    bool IsReady() const;

private:
    ShaderSettings settings_;
    mutable Texture2D voxelTexture_ {};
    mutable GridPos voxelOrigin_ {};
    mutable int voxelSize_ = 0;
    mutable std::uint64_t voxelRevision_ = 0;
    std::array<int, 7> voxelLocations_ {};
    Shader skyShader_ {};
    std::array<int, 6> skyLocations_ {};
    int materialQualityLocation_ = -1;
    int volumeStepsLocation_ = -1;
    Shader shader_ {};
    int viewPositionLocation_ = -1;
    int lightDirectionLocation_ = -1;
    int lightColorLocation_ = -1;
    int ambientLocation_ = -1;
    int fogColorLocation_ = -1;
    int fogDensityLocation_ = -1;
    int fogStartLocation_ = -1;
    int fogEndLocation_ = -1;
    int timeLocation_ = -1;
    int normalMapGateLocation_ = -1;
    int aoGateLocation_ = -1;
    std::array<int, SceneShadowParams::kCascadeCount> lightViewProjLocations_ { -1, -1 };
    std::array<int, SceneShadowParams::kCascadeCount> shadowMapLocations_ { -1, -1 };
    int shadowStrengthLocation_ = -1;
    std::array<int, SceneShadowParams::kCascadeCount> shadowTexelLocations_ { -1, -1 };
    int shadowSplitLocation_ = -1;
    int shadowDistanceLocation_ = -1;
    int shadowPcfExtraLocation_ = -1;
    int shadowWorldTexelLocation_ = -1;
    int aoStrengthLocation_ = -1;
    int emissiveStrengthLocation_ = -1;
    int pointLightCountLocation_ = -1;
    int pointLightPositionRadiusLocation_ = -1;
    int pointLightColorIntensityLocation_ = -1;
    bool ready_ = false;
};

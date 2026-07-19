#pragma once

#include "raylib.h"

#include <vector>

// Shared with the shadow-map pass: the depth camera must look along the very
// same vector the fragment shader shades with.  Pre-normalized on the CPU.
inline constexpr Vector3 kSceneSunDirection { -0.480264f, -0.820451f, 0.310171f };

// Everything the lighting shader needs to sample a sun shadow map.  Built by
// Renderer::PrepareSunShadows; pass nullptr to SceneShader::Begin to shade
// without shadows (menus, previews, shadowQuality 0).
struct SceneShadowParams
{
    Matrix lightViewProj {};
    unsigned int depthTextureId = 0;
    float texelSize = 0.0f;
    float strength = 0.0f;
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
    bool Initialize();
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
    // Chunk meshes carry baked AO in the vertex alpha and a normal map in
    // MATERIAL_MAP_NORMAL; batched draws carry neither.  Toggle around
    // ChunkRenderer::Draw only.
    void SetChunkPassFeatures(bool enabled) const;
    Shader GetShader() const;
    bool IsReady() const;

private:
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
    int lightViewProjLocation_ = -1;
    int shadowMapLocation_ = -1;
    int shadowStrengthLocation_ = -1;
    int shadowTexelLocation_ = -1;
    int shadowDistanceLocation_ = -1;
    int shadowPcfExtraLocation_ = -1;
    int pointLightCountLocation_ = -1;
    int pointLightPositionRadiusLocation_ = -1;
    int pointLightColorIntensityLocation_ = -1;
    bool ready_ = false;
};

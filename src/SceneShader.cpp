#include "SceneShader.h"

#include "rlgl.h"

#include <algorithm>
#include <array>
#include <string>

namespace
{
// Texture unit for the sun shadow map.  Slots 0-2 are material maps bound by
// DrawMesh (diffuse/specular/normal) and slots 1-4 are recycled by raylib's
// SetShaderValueTexture batch bookkeeping, so the depth texture lives well
// above both ranges.  GL 3.3 guarantees at least 16 units.
constexpr int kShadowMapSlot = 10;
constexpr int kMaxPointLights = 12;

std::string FindAssetFile(const std::string& relative)
{
    const std::string candidates[] { relative, "../" + relative, "../../" + relative };
    for (const std::string& candidate : candidates)
    {
        if (FileExists(candidate.c_str()))
        {
            return candidate;
        }
    }
    return {};
}

void SetVector(Shader shader, int location, const float* value, int type)
{
    if (location >= 0)
    {
        SetShaderValue(shader, location, value, type);
    }
}
}

bool SceneShader::Initialize()
{
    if (ready_)
    {
        return true;
    }
    const std::string vertexPath = FindAssetFile("assets/shaders/lighting.vs");
    const std::string fragmentPath = FindAssetFile("assets/shaders/lighting.fs");
    if (vertexPath.empty() || fragmentPath.empty())
    {
        TraceLog(LOG_WARNING, "SHADER: scene lighting files missing; default renderer enabled");
        return false;
    }

    shader_ = LoadShader(vertexPath.c_str(), fragmentPath.c_str());
    if (shader_.id == 0)
    {
        TraceLog(LOG_WARNING, "SHADER: scene lighting failed; default renderer enabled");
        return false;
    }
    viewPositionLocation_ = GetShaderLocation(shader_, "viewPos");
    lightDirectionLocation_ = GetShaderLocation(shader_, "lightDirection");
    lightColorLocation_ = GetShaderLocation(shader_, "lightColor");
    ambientLocation_ = GetShaderLocation(shader_, "ambientColor");
    fogColorLocation_ = GetShaderLocation(shader_, "fogColor");
    fogDensityLocation_ = GetShaderLocation(shader_, "fogDensity");
    fogStartLocation_ = GetShaderLocation(shader_, "fogStart");
    fogEndLocation_ = GetShaderLocation(shader_, "fogEnd");
    timeLocation_ = GetShaderLocation(shader_, "time");
    normalMapGateLocation_ = GetShaderLocation(shader_, "normalMapGate");
    aoGateLocation_ = GetShaderLocation(shader_, "aoGate");
    lightViewProjLocation_ = GetShaderLocation(shader_, "lightViewProj");
    shadowMapLocation_ = GetShaderLocation(shader_, "shadowMap");
    shadowStrengthLocation_ = GetShaderLocation(shader_, "shadowStrength");
    shadowTexelLocation_ = GetShaderLocation(shader_, "shadowTexel");
    shadowDistanceLocation_ = GetShaderLocation(shader_, "shadowDistance");
    shadowPcfExtraLocation_ = GetShaderLocation(shader_, "shadowPcfExtra");
    pointLightCountLocation_ = GetShaderLocation(shader_, "pointLightCount");
    pointLightPositionRadiusLocation_ = GetShaderLocation(shader_, "pointLightPositionRadius[0]");
    pointLightColorIntensityLocation_ = GetShaderLocation(shader_, "pointLightColorIntensity[0]");
    ready_ = true;
    return true;
}

void SceneShader::Shutdown()
{
    if (ready_ && shader_.id != 0)
    {
        UnloadShader(shader_);
    }
    shader_ = {};
    ready_ = false;
}

void SceneShader::Begin(
    const Camera3D& camera,
    Color fogColor,
    float fogDensity,
    float renderDistance,
    const SceneShadowParams* shadow) const
{
    if (!ready_)
    {
        return;
    }
    const float view[] { camera.position.x, camera.position.y, camera.position.z };
    const float lightDirection[] { kSceneSunDirection.x, kSceneSunDirection.y, kSceneSunDirection.z };
    const float lightColor[] { 1.00f, 0.96f, 0.86f, 1.0f };
    const float ambient[] { 0.43f, 0.49f, 0.58f, 1.0f };
    const float fog[] {
        static_cast<float>(fogColor.r) / 255.0f,
        static_cast<float>(fogColor.g) / 255.0f,
        static_cast<float>(fogColor.b) / 255.0f,
        1.0f
    };
    SetVector(shader_, viewPositionLocation_, view, SHADER_UNIFORM_VEC3);
    SetVector(shader_, lightDirectionLocation_, lightDirection, SHADER_UNIFORM_VEC3);
    SetVector(shader_, lightColorLocation_, lightColor, SHADER_UNIFORM_VEC4);
    SetVector(shader_, ambientLocation_, ambient, SHADER_UNIFORM_VEC4);
    SetVector(shader_, fogColorLocation_, fog, SHADER_UNIFORM_VEC4);
    if (fogDensityLocation_ >= 0)
    {
        SetShaderValue(shader_, fogDensityLocation_, &fogDensity, SHADER_UNIFORM_FLOAT);
    }
    // Fog veil window: clear out to ~55% of the render distance (combat
    // stays readable), then dissolve into the sky just before the cull pops
    // geometry.
    const float fogStart = renderDistance * 0.55f;
    const float fogEnd = renderDistance * 0.98f;
    if (fogStartLocation_ >= 0)
    {
        SetShaderValue(shader_, fogStartLocation_, &fogStart, SHADER_UNIFORM_FLOAT);
    }
    if (fogEndLocation_ >= 0)
    {
        SetShaderValue(shader_, fogEndLocation_, &fogEnd, SHADER_UNIFORM_FLOAT);
    }
    const float time = static_cast<float>(GetTime());
    if (timeLocation_ >= 0)
    {
        SetShaderValue(shader_, timeLocation_, &time, SHADER_UNIFORM_FLOAT);
    }

    float shadowStrength = 0.0f;
    if (shadow != nullptr
        && shadow->depthTextureId != 0
        && shadow->strength > 0.0f
        && shadowMapLocation_ >= 0
        && lightViewProjLocation_ >= 0)
    {
        shadowStrength = shadow->strength;
        // Manual slot binding instead of SetShaderValueTexture: the batch
        // slot pool (units 1-4) is shared with the post-processing bloom
        // bind, and DrawMesh owns units 0-2 for material maps.
        rlActiveTextureSlot(kShadowMapSlot);
        rlEnableTexture(shadow->depthTextureId);
        rlActiveTextureSlot(0);
        const int slot = kShadowMapSlot;
        SetShaderValue(shader_, shadowMapLocation_, &slot, SHADER_UNIFORM_INT);
        SetShaderValueMatrix(shader_, lightViewProjLocation_, shadow->lightViewProj);
        const float texel[] { shadow->texelSize, shadow->texelSize };
        SetVector(shader_, shadowTexelLocation_, texel, SHADER_UNIFORM_VEC2);
        if (shadowDistanceLocation_ >= 0)
        {
            SetShaderValue(shader_, shadowDistanceLocation_, &shadow->distance, SHADER_UNIFORM_FLOAT);
        }
        if (shadowPcfExtraLocation_ >= 0)
        {
            const float widePcf = shadow->widePcf ? 1.0f : 0.0f;
            SetShaderValue(shader_, shadowPcfExtraLocation_, &widePcf, SHADER_UNIFORM_FLOAT);
        }
    }
    if (shadowStrengthLocation_ >= 0)
    {
        SetShaderValue(shader_, shadowStrengthLocation_, &shadowStrength, SHADER_UNIFORM_FLOAT);
    }

    BeginShaderMode(shader_);
}

void SceneShader::End() const
{
    if (ready_)
    {
        EndShaderMode();
    }
}

void SceneShader::SetPointLights(const std::vector<ScenePointLight>& lights) const
{
    if (!ready_)
    {
        return;
    }
    const int count = std::min(static_cast<int>(lights.size()), kMaxPointLights);
    if (pointLightCountLocation_ >= 0)
    {
        SetShaderValue(shader_, pointLightCountLocation_, &count, SHADER_UNIFORM_INT);
    }
    if (count <= 0)
    {
        return;
    }

    std::array<float, kMaxPointLights * 4> positions {};
    std::array<float, kMaxPointLights * 4> colors {};
    for (int i = 0; i < count; ++i)
    {
        const ScenePointLight& light = lights[static_cast<std::size_t>(i)];
        const int offset = i * 4;
        positions[offset + 0] = light.position.x;
        positions[offset + 1] = light.position.y;
        positions[offset + 2] = light.position.z;
        positions[offset + 3] = std::max(0.1f, light.radius);
        colors[offset + 0] = static_cast<float>(light.color.r) / 255.0f;
        colors[offset + 1] = static_cast<float>(light.color.g) / 255.0f;
        colors[offset + 2] = static_cast<float>(light.color.b) / 255.0f;
        colors[offset + 3] = std::max(0.0f, light.intensity);
    }
    if (pointLightPositionRadiusLocation_ >= 0)
    {
        SetShaderValueV(shader_, pointLightPositionRadiusLocation_, positions.data(), SHADER_UNIFORM_VEC4, count);
    }
    if (pointLightColorIntensityLocation_ >= 0)
    {
        SetShaderValueV(shader_, pointLightColorIntensityLocation_, colors.data(), SHADER_UNIFORM_VEC4, count);
    }
}

void SceneShader::SetChunkPassFeatures(bool enabled) const
{
    if (!ready_)
    {
        return;
    }
    const float gate = enabled ? 1.0f : 0.0f;
    if (normalMapGateLocation_ >= 0)
    {
        SetShaderValue(shader_, normalMapGateLocation_, &gate, SHADER_UNIFORM_FLOAT);
    }
    if (aoGateLocation_ >= 0)
    {
        SetShaderValue(shader_, aoGateLocation_, &gate, SHADER_UNIFORM_FLOAT);
    }
}

Shader SceneShader::GetShader() const
{
    return ready_ ? shader_ : Shader {};
}

bool SceneShader::IsReady() const
{
    return ready_;
}

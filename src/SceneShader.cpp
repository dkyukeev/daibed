#include "SceneShader.h"
#include "VoxelLightShape.h"

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
constexpr int kMaxPointLights = 16;

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
    if (shader_.id == 0 || shader_.id == rlGetShaderIdDefault())
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
    lightViewProjLocations_[0] = GetShaderLocation(shader_, "lightViewProjNear");
    lightViewProjLocations_[1] = GetShaderLocation(shader_, "lightViewProjFar");
    shadowMapLocations_[0] = GetShaderLocation(shader_, "shadowMapNear");
    shadowMapLocations_[1] = GetShaderLocation(shader_, "shadowMapFar");
    shadowStrengthLocation_ = GetShaderLocation(shader_, "shadowStrength");
    shadowTexelLocations_[0] = GetShaderLocation(shader_, "shadowTexelNear");
    shadowTexelLocations_[1] = GetShaderLocation(shader_, "shadowTexelFar");
    shadowSplitLocation_ = GetShaderLocation(shader_, "shadowSplitDistance");
    shadowDistanceLocation_ = GetShaderLocation(shader_, "shadowDistance");
    shadowPcfExtraLocation_ = GetShaderLocation(shader_, "shadowPcfExtra");
    shadowWorldTexelLocation_ = GetShaderLocation(shader_, "shadowWorldTexel");
    aoStrengthLocation_ = GetShaderLocation(shader_, "aoStrength");
    emissiveStrengthLocation_ = GetShaderLocation(shader_, "emissiveStrength");
    pointLightCountLocation_ = GetShaderLocation(shader_, "pointLightCount");
    pointLightPositionRadiusLocation_ = GetShaderLocation(shader_, "pointLightPositionRadius[0]");
    pointLightColorIntensityLocation_ = GetShaderLocation(shader_, "pointLightColorIntensity[0]");
    materialQualityLocation_ = GetShaderLocation(shader_, "materialQuality");
    volumeStepsLocation_ = GetShaderLocation(shader_, "volumeSteps");
    const std::string skyVertex = FindAssetFile("assets/shaders/sky.vs");
    const std::string skyFragment = FindAssetFile("assets/shaders/sky.fs");
    if (!skyVertex.empty() && !skyFragment.empty())
    {
        skyShader_ = LoadShader(skyVertex.c_str(), skyFragment.c_str());
        if (skyShader_.id == rlGetShaderIdDefault()) skyShader_ = {};
        if (skyShader_.id != 0)
        {
            const char* names[] { "eye", "skyTint", "sunDirection", "sunIntensity", "time", "quality" };
            for (int i = 0; i < 6; ++i) skyLocations_[i] = GetShaderLocation(skyShader_, names[i]);
        }
    }
    const char* voxelNames[] { "voxelAtlas", "voxelOrigin", "voxelSize", "giQuality", "giStrength", "localShadows", "shadowSoftness" };
    for (int i = 0; i < 7; ++i) voxelLocations_[i] = GetShaderLocation(shader_, voxelNames[i]);
    ready_ = true;
    return true;
}

void SceneShader::Shutdown()
{
    if (ready_ && shader_.id != 0)
    {
        UnloadShader(shader_);
    }
    if (skyShader_.id != 0) UnloadShader(skyShader_);
    skyShader_ = {};
    if (voxelTexture_.id != 0) UnloadTexture(voxelTexture_);
    voxelTexture_ = {};
    voxelSize_ = 0;
    voxelRevision_ = 0;
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
    fogDensity *= settings_.haze;
    const int steps[] { 0, 6, 12, 24 };
    const int volumeSteps = shadow != nullptr ? steps[settings_.volumetricQuality] : 0;
    SetShaderValue(shader_, materialQualityLocation_, &settings_.materialQuality, SHADER_UNIFORM_INT);
    SetShaderValue(shader_, volumeStepsLocation_, &volumeSteps, SHADER_UNIFORM_INT);
    const float view[] { camera.position.x, camera.position.y, camera.position.z };
    const float lightDirection[] { kSceneSunDirection.x, kSceneSunDirection.y, kSceneSunDirection.z };
    const float skyR = static_cast<float>(fogColor.r) / 255.0f;
    const float skyG = static_cast<float>(fogColor.g) / 255.0f;
    const float skyB = static_cast<float>(fogColor.b) / 255.0f;
    const float skyLuminance = skyR * 0.2126f + skyG * 0.7152f + skyB * 0.0722f;
    const float daylight = std::clamp(0.25f + skyLuminance * 1.5f, 0.28f, 1.0f);
    const float dawnWarmth = std::clamp((skyR - skyB) * 2.5f, 0.0f, 1.0f);
    const float lightColor[] {
        daylight * settings_.sunIntensity,
        daylight * settings_.sunIntensity * (0.96f - dawnWarmth * 0.12f),
        daylight * settings_.sunIntensity * (0.88f - dawnWarmth * 0.24f),
        1.0f
    };
    const float ambient[] {
        (0.25f + skyR * 0.22f) * daylight,
        (0.28f + skyG * 0.22f) * daylight,
        (0.34f + skyB * 0.22f) * daylight,
        1.0f
    };
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
        && shadow->depthTextureId[0] != 0
        && shadow->depthTextureId[1] != 0
        && shadow->strength > 0.0f
        && shadowMapLocations_[0] >= 0
        && shadowMapLocations_[1] >= 0
        && lightViewProjLocations_[0] >= 0
        && lightViewProjLocations_[1] >= 0)
    {
        shadowStrength = 1.0f;
        SetShaderValue(shader_, shadowWorldTexelLocation_, shadow->worldPerTexel.data(), SHADER_UNIFORM_VEC2);
        // Manual slot binding instead of SetShaderValueTexture: the batch
        // slot pool (units 1-4) is shared with the post-processing bloom
        // bind, and DrawMesh owns units 0-2 for material maps.
        for (int cascade = 0; cascade < SceneShadowParams::kCascadeCount; ++cascade)
        {
            rlActiveTextureSlot(kShadowMapSlot + cascade);
            rlEnableTexture(shadow->depthTextureId[cascade]);
        }
        rlActiveTextureSlot(0);
        for (int cascade = 0; cascade < SceneShadowParams::kCascadeCount; ++cascade)
        {
            const int slot = kShadowMapSlot + cascade;
            SetShaderValue(shader_, shadowMapLocations_[cascade], &slot, SHADER_UNIFORM_INT);
            SetShaderValueMatrix(shader_, lightViewProjLocations_[cascade], shadow->lightViewProj[cascade]);
            const float texel[] { shadow->texelSize[cascade], shadow->texelSize[cascade] };
            SetVector(shader_, shadowTexelLocations_[cascade], texel, SHADER_UNIFORM_VEC2);
        }
        if (shadowSplitLocation_ >= 0)
        {
            SetShaderValue(shader_, shadowSplitLocation_, &shadow->splitDistance, SHADER_UNIFORM_FLOAT);
        }
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

    const int voxelSlot = 12;
    const float origin[] { float(voxelOrigin_.x) - 0.5f, float(voxelOrigin_.y) - 0.5f, float(voxelOrigin_.z) - 0.5f };
    const int size = voxelTexture_.id != 0 ? voxelSize_ : 0;
    const int giQuality = size > 0 && settings_.giStrength > 0.001f ? settings_.giQuality : 0;
    const int localShadows = size > 0 && settings_.localShadows ? 1 : 0;
    if (size > 0)
    {
        rlActiveTextureSlot(voxelSlot);
        rlEnableTexture(voxelTexture_.id);
        rlActiveTextureSlot(0);
    }
    SetShaderValue(shader_, voxelLocations_[0], &voxelSlot, SHADER_UNIFORM_INT);
    SetShaderValue(shader_, voxelLocations_[1], origin, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, voxelLocations_[2], &size, SHADER_UNIFORM_INT);
    SetShaderValue(shader_, voxelLocations_[3], &giQuality, SHADER_UNIFORM_INT);
    SetShaderValue(shader_, voxelLocations_[4], &settings_.giStrength, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, voxelLocations_[5], &localShadows, SHADER_UNIFORM_INT);
    SetShaderValue(shader_, voxelLocations_[6], &settings_.shadowSoftness, SHADER_UNIFORM_FLOAT);
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
    const int budgets[] { 4, 8, kMaxPointLights };
    const int count = std::min(static_cast<int>(lights.size()), budgets[settings_.materialQuality]);
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

void SceneShader::SetAmbientOcclusionQuality(int quality) const
{
    if (!ready_ || aoStrengthLocation_ < 0)
    {
        return;
    }
    const float strengths[] { 0.0f, 0.72f, 1.0f };
    const float strength = strengths[std::clamp(quality, 0, 2)];
    SetShaderValue(shader_, aoStrengthLocation_, &strength, SHADER_UNIFORM_FLOAT);
}

void SceneShader::SetEmissiveStrength(float strength) const
{
    if (!ready_ || emissiveStrengthLocation_ < 0)
    {
        return;
    }
    rlDrawRenderBatchActive();
    strength = std::clamp(strength, 0.0f, 2.0f);
    SetShaderValue(shader_, emissiveStrengthLocation_, &strength, SHADER_UNIFORM_FLOAT);
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

bool SceneShader::DrawSky(const Camera3D& camera, Color skyColor) const
{
    if (skyShader_.id == 0 || settings_.skyQuality == 0) return false;
    const float eye[] { camera.position.x, camera.position.y, camera.position.z };
    const float tint[] { skyColor.r / 255.0f, skyColor.g / 255.0f, skyColor.b / 255.0f };
    const float sun[] { -kSceneSunDirection.x, -kSceneSunDirection.y, -kSceneSunDirection.z };
    const float time = static_cast<float>(GetTime());
    SetShaderValue(skyShader_, skyLocations_[0], eye, SHADER_UNIFORM_VEC3);
    SetShaderValue(skyShader_, skyLocations_[1], tint, SHADER_UNIFORM_VEC3);
    SetShaderValue(skyShader_, skyLocations_[2], sun, SHADER_UNIFORM_VEC3);
    SetShaderValue(skyShader_, skyLocations_[3], &settings_.sunIntensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(skyShader_, skyLocations_[4], &time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(skyShader_, skyLocations_[5], &settings_.skyQuality, SHADER_UNIFORM_INT);
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    BeginShaderMode(skyShader_);
    DrawSphereEx(camera.position, 400.0f, 16, 32, WHITE);
    EndShaderMode();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
    return true;
}

void SceneShader::UpdateVoxelLighting(const World& world, const Camera3D& camera,
    const std::function<Color(const Block&)>& colorResolver) const
{
    if (!ready_ || ((settings_.giQuality == 0 || settings_.giStrength <= 0.001f) && !settings_.localShadows))
    {
        if (voxelTexture_.id != 0) UnloadTexture(voxelTexture_);
        voxelTexture_ = {};
        voxelSize_ = 0;
        return;
    }
    const int size = settings_.giQuality >= 2 ? 48 : 32;
    // Recenter in four-block increments, avoiding an upload on every step.
    const GridPos origin {
        int(std::floor(camera.position.x / 4.0f)) * 4 - size / 2,
        int(std::floor(camera.position.y / 4.0f)) * 4 - size / 2,
        int(std::floor(camera.position.z / 4.0f)) * 4 - size / 2 };
    if (voxelTexture_.id != 0 && voxelSize_ == size && voxelOrigin_ == origin
        && voxelRevision_ == world.GetRenderRevision()) return;
    if (voxelTexture_.id != 0 && voxelSize_ == size && voxelOrigin_ == origin)
    {
        const auto changes = world.QueryNavigationChanges(voxelRevision_, origin,
            GridPos { origin.x + size - 1, origin.y + size - 1, origin.z + size - 1 }, 1);
        if (changes == NavigationChangeQuery::Disjoint || changes == NavigationChangeQuery::NoChanges)
        {
            voxelRevision_ = world.GetRenderRevision();
            return;
        }
    }
    std::vector<Color> voxels(size * size * size, Color { 0, 0, 0, 0 });
    for (int y = 0; y < size; ++y)
    for (int z = 0; z < size; ++z)
    for (int x = 0; x < size; ++x)
    {
        const Block* block = world.GetBlock(GridPos { origin.x + x, origin.y + y, origin.z + z });
        if (block == nullptr) continue;
        switch (block->type)
        {
        case BlockType::Air: case BlockType::BarrierBlock:
        case BlockType::ColoredGlassBlock: case BlockType::EnergyGlassBlock:
        case BlockType::IceBlock: case BlockType::LadderBlock: case BlockType::IronBarsBlock:
            continue;
        default: break;
        }
        Color color = colorResolver(*block);
        if (block->type == BlockType::TorchBlock) color = Color { 255, 177, 75, 255 };
        const bool emissive = block->type == BlockType::GlowBlock || block->type == BlockType::LavaBlock
            || block->type == BlockType::TorchBlock || block->type == BlockType::EnergyCoreBlock;
        color.a = emissive ? 128 : VoxelLightShape::Occupancy(world,
            GridPos { origin.x + x, origin.y + y, origin.z + z }, *block);
        voxels[(y * size + z) * size + x] = color;
    }
    if (voxelTexture_.id != 0 && voxelSize_ != size)
    {
        UnloadTexture(voxelTexture_);
        voxelTexture_ = {};
    }
    if (voxelTexture_.id == 0)
    {
        Image atlas { voxels.data(), size * size, size, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        voxelTexture_ = LoadTextureFromImage(atlas);
        if (voxelTexture_.id != 0) SetTextureFilter(voxelTexture_, TEXTURE_FILTER_POINT);
    }
    else UpdateTexture(voxelTexture_, voxels.data());
    voxelOrigin_ = origin;
    voxelSize_ = size;
    voxelRevision_ = world.GetRenderRevision();
}

#include "PostProcessor.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
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
}

bool PostProcessor::Initialize()
{
    const std::string fragmentPath = FindAssetFile("assets/shaders/post.fs");
    if (!fragmentPath.empty())
    {
        shader_ = LoadShader(nullptr, fragmentPath.c_str());
        shaderReady_ = shader_.id != 0;
    }
    if (shaderReady_)
    {
        sourceSizeLocation_ = GetShaderLocation(shader_, "sourceSize");
        timeLocation_ = GetShaderLocation(shader_, "time");
        bloomLocation_ = GetShaderLocation(shader_, "bloomStrength");
        vignetteLocation_ = GetShaderLocation(shader_, "vignetteStrength");
        damageLocation_ = GetShaderLocation(shader_, "damageFlash");
        scopeLocation_ = GetShaderLocation(shader_, "scopeBlend");
    }
    else
    {
        TraceLog(LOG_WARNING, "SHADER: post-processing unavailable; texture copy fallback enabled");
    }
    return EnsureTarget(1.0f);
}

void PostProcessor::Shutdown()
{
    if (target_.id != 0)
    {
        UnloadRenderTexture(target_);
        target_ = {};
    }
    if (shaderReady_ && shader_.id != 0)
    {
        UnloadShader(shader_);
        shader_ = {};
    }
    shaderReady_ = false;
    frameActive_ = false;
    targetWidth_ = 0;
    targetHeight_ = 0;
}

bool PostProcessor::BeginFrame(float renderScale)
{
    if (!EnsureTarget(renderScale))
    {
        return false;
    }
    BeginTextureMode(target_);
    frameActive_ = true;
    return true;
}

void PostProcessor::EndFrameAndDraw(
    bool effectsEnabled,
    bool bloomEnabled,
    float damageFlash,
    float scopeBlend,
    bool reducedFlashes)
{
    if (!frameActive_)
    {
        return;
    }
    EndTextureMode();
    frameActive_ = false;

    const Rectangle source {
        0.0f,
        0.0f,
        static_cast<float>(target_.texture.width),
        -static_cast<float>(target_.texture.height)
    };
    const Rectangle destination {
        0.0f,
        0.0f,
        static_cast<float>(GetScreenWidth()),
        static_cast<float>(GetScreenHeight())
    };
    if (!effectsEnabled || !shaderReady_)
    {
        DrawTexturePro(target_.texture, source, destination, Vector2 {}, 0.0f, WHITE);
        return;
    }

    const float sourceSize[] { static_cast<float>(targetWidth_), static_cast<float>(targetHeight_) };
    const float time = static_cast<float>(GetTime());
    const float bloom = bloomEnabled ? 0.48f : 0.0f;
    const float vignette = 0.28f;
    const float flash = std::clamp(damageFlash * (reducedFlashes ? 0.22f : 0.72f), 0.0f, 1.0f);
    const float scope = std::clamp(scopeBlend, 0.0f, 1.0f);
    SetShaderValue(shader_, sourceSizeLocation_, sourceSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader_, timeLocation_, &time, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, bloomLocation_, &bloom, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, vignetteLocation_, &vignette, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, damageLocation_, &flash, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, scopeLocation_, &scope, SHADER_UNIFORM_FLOAT);
    BeginShaderMode(shader_);
    DrawTexturePro(target_.texture, source, destination, Vector2 {}, 0.0f, WHITE);
    EndShaderMode();
}

bool PostProcessor::EnsureTarget(float renderScale)
{
    const float scale = std::clamp(renderScale, 0.55f, 1.0f);
    const int width = std::max(1, static_cast<int>(std::round(GetScreenWidth() * scale)));
    const int height = std::max(1, static_cast<int>(std::round(GetScreenHeight() * scale)));
    if (target_.id != 0 && targetWidth_ == width && targetHeight_ == height)
    {
        return true;
    }
    if (target_.id != 0)
    {
        UnloadRenderTexture(target_);
        target_ = {};
    }
    target_ = LoadRenderTexture(width, height);
    targetWidth_ = target_.id != 0 ? width : 0;
    targetHeight_ = target_.id != 0 ? height : 0;
    if (target_.id != 0)
    {
        SetTextureFilter(target_.texture, scale < 0.99f ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
    }
    return target_.id != 0;
}

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

Rectangle FlippedSourceRect(const RenderTexture2D& target)
{
    return Rectangle {
        0.0f,
        0.0f,
        static_cast<float>(target.texture.width),
        -static_cast<float>(target.texture.height)
    };
}
}

bool PostProcessor::Initialize()
{
    const std::string fragmentPath = FindAssetFile("assets/shaders/post.fs");
    const std::string bloomFragmentPath = FindAssetFile("assets/shaders/bloom.fs");
    const std::string bloomBlurFragmentPath = FindAssetFile("assets/shaders/bloom_blur.fs");
    if (!fragmentPath.empty())
    {
        shader_ = LoadShader(nullptr, fragmentPath.c_str());
        shaderReady_ = shader_.id != 0;
    }
    if (shaderReady_)
    {
        bloomLocation_ = GetShaderLocation(shader_, "bloomStrength");
        bloomTextureLocation_ = GetShaderLocation(shader_, "bloomTexture");
        vignetteLocation_ = GetShaderLocation(shader_, "vignetteStrength");
        damageLocation_ = GetShaderLocation(shader_, "damageFlash");
        scopeLocation_ = GetShaderLocation(shader_, "scopeBlend");
        texelSizeLocation_ = GetShaderLocation(shader_, "texelSize");
    }
    else
    {
        TraceLog(LOG_WARNING, "SHADER: post-processing unavailable; texture copy fallback enabled");
    }

    if (!bloomFragmentPath.empty())
    {
        bloomShader_ = LoadShader(nullptr, bloomFragmentPath.c_str());
        bloomShaderReady_ = bloomShader_.id != 0;
    }
    if (bloomShaderReady_)
    {
        bloomSourceSizeLocation_ = GetShaderLocation(bloomShader_, "sourceSize");
        bloomQualityLocation_ = GetShaderLocation(bloomShader_, "quality");
    }
    else
    {
        TraceLog(LOG_WARNING, "SHADER: compact bloom unavailable; bloom disabled");
    }

    if (!bloomBlurFragmentPath.empty())
    {
        bloomBlurShader_ = LoadShader(nullptr, bloomBlurFragmentPath.c_str());
        bloomBlurShaderReady_ = bloomBlurShader_.id != 0;
    }
    if (bloomBlurShaderReady_)
    {
        bloomBlurStepLocation_ = GetShaderLocation(bloomBlurShader_, "blurStep");
    }
    else
    {
        // The bright pass alone still produces a usable halo through the
        // bilinear upsample, so bloom stays enabled without the blur.
        TraceLog(LOG_WARNING, "SHADER: bloom blur unavailable; falling back to single-pass bloom");
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
    if (bloomTarget_.id != 0)
    {
        UnloadRenderTexture(bloomTarget_);
        bloomTarget_ = {};
    }
    if (bloomScratchTarget_.id != 0)
    {
        UnloadRenderTexture(bloomScratchTarget_);
        bloomScratchTarget_ = {};
    }
    if (shaderReady_ && shader_.id != 0)
    {
        UnloadShader(shader_);
        shader_ = {};
    }
    if (bloomShaderReady_ && bloomShader_.id != 0)
    {
        UnloadShader(bloomShader_);
        bloomShader_ = {};
    }
    if (bloomBlurShaderReady_ && bloomBlurShader_.id != 0)
    {
        UnloadShader(bloomBlurShader_);
        bloomBlurShader_ = {};
    }
    shaderReady_ = false;
    bloomShaderReady_ = false;
    bloomBlurShaderReady_ = false;
    frameActive_ = false;
    targetWidth_ = 0;
    targetHeight_ = 0;
    bloomWidth_ = 0;
    bloomHeight_ = 0;
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
    int effectsQuality,
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

    const Rectangle source = FlippedSourceRect(target_);
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

    const int quality = std::clamp(effectsQuality, 0, 2);
    const float bloomScales[] { 0.25f, 0.35f, 0.50f };
    const float bloomStrengths[] { 0.30f, 0.40f, 0.52f };
    float bloom = 0.0f;
    if (bloomEnabled && bloomShaderReady_ && EnsureBloomTargets(bloomScales[quality]))
    {
        const Rectangle bloomDestination {
            0.0f,
            0.0f,
            static_cast<float>(bloomWidth_),
            static_cast<float>(bloomHeight_)
        };

        // Pass 1: bright-pass downsample from the scene target.
        const float sourceSize[] { static_cast<float>(targetWidth_), static_cast<float>(targetHeight_) };
        const float shaderQuality = static_cast<float>(quality);
        BeginTextureMode(bloomTarget_);
        ClearBackground(BLACK);
        SetShaderValue(bloomShader_, bloomSourceSizeLocation_, sourceSize, SHADER_UNIFORM_VEC2);
        SetShaderValue(bloomShader_, bloomQualityLocation_, &shaderQuality, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(bloomShader_);
        DrawTexturePro(target_.texture, source, bloomDestination, Vector2 {}, 0.0f, WHITE);
        EndShaderMode();
        EndTextureMode();

        // Passes 2-3: separable Gaussian ping-pong inside the tiny bloom
        // targets.  Each blit flips vertically; two passes cancel out, so
        // the final texture keeps the orientation post.fs expects.
        if (bloomBlurShaderReady_ && bloomScratchTarget_.id != 0)
        {
            const float horizontalStep[] { 1.0f / static_cast<float>(std::max(1, bloomWidth_)), 0.0f };
            const float verticalStep[] { 0.0f, 1.0f / static_cast<float>(std::max(1, bloomHeight_)) };

            BeginTextureMode(bloomScratchTarget_);
            ClearBackground(BLACK);
            SetShaderValue(bloomBlurShader_, bloomBlurStepLocation_, horizontalStep, SHADER_UNIFORM_VEC2);
            BeginShaderMode(bloomBlurShader_);
            DrawTexturePro(bloomTarget_.texture, FlippedSourceRect(bloomTarget_), bloomDestination, Vector2 {}, 0.0f, WHITE);
            EndShaderMode();
            EndTextureMode();

            BeginTextureMode(bloomTarget_);
            ClearBackground(BLACK);
            SetShaderValue(bloomBlurShader_, bloomBlurStepLocation_, verticalStep, SHADER_UNIFORM_VEC2);
            BeginShaderMode(bloomBlurShader_);
            DrawTexturePro(bloomScratchTarget_.texture, FlippedSourceRect(bloomScratchTarget_), bloomDestination, Vector2 {}, 0.0f, WHITE);
            EndShaderMode();
            EndTextureMode();
        }

        SetShaderValueTexture(shader_, bloomTextureLocation_, bloomTarget_.texture);
        bloom = bloomStrengths[quality];
    }
    const float vignette = 0.28f;
    const float flash = std::clamp(damageFlash * (reducedFlashes ? 0.22f : 0.72f), 0.0f, 1.0f);
    const float scope = std::clamp(scopeBlend, 0.0f, 1.0f);
    // FXAA reads neighbors through this; zero disables it on the low preset
    // where the five extra fetches are not worth it.
    const float fxaaTexel[] {
        quality >= 1 && targetWidth_ > 0 ? 1.0f / static_cast<float>(targetWidth_) : 0.0f,
        quality >= 1 && targetHeight_ > 0 ? 1.0f / static_cast<float>(targetHeight_) : 0.0f
    };
    if (texelSizeLocation_ >= 0)
    {
        SetShaderValue(shader_, texelSizeLocation_, fxaaTexel, SHADER_UNIFORM_VEC2);
    }
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

bool PostProcessor::EnsureBloomTargets(float scale)
{
    const int width = std::max(1, static_cast<int>(std::round(static_cast<float>(targetWidth_) * scale)));
    const int height = std::max(1, static_cast<int>(std::round(static_cast<float>(targetHeight_) * scale)));
    if (bloomTarget_.id != 0 && bloomWidth_ == width && bloomHeight_ == height)
    {
        return true;
    }
    if (bloomTarget_.id != 0)
    {
        UnloadRenderTexture(bloomTarget_);
        bloomTarget_ = {};
    }
    if (bloomScratchTarget_.id != 0)
    {
        UnloadRenderTexture(bloomScratchTarget_);
        bloomScratchTarget_ = {};
    }
    bloomTarget_ = LoadRenderTexture(width, height);
    bloomWidth_ = bloomTarget_.id != 0 ? width : 0;
    bloomHeight_ = bloomTarget_.id != 0 ? height : 0;
    if (bloomTarget_.id != 0)
    {
        // Bilinear upsampling turns the compact blurred pass into a broad,
        // smooth halo instead of spending samples at full scene resolution.
        // The Gaussian's bilinear-offset taps also depend on this filter.
        SetTextureFilter(bloomTarget_.texture, TEXTURE_FILTER_BILINEAR);
        bloomScratchTarget_ = LoadRenderTexture(width, height);
        if (bloomScratchTarget_.id != 0)
        {
            SetTextureFilter(bloomScratchTarget_.texture, TEXTURE_FILTER_BILINEAR);
        }
    }
    return bloomTarget_.id != 0;
}

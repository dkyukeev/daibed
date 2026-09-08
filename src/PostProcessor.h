#pragma once

#include "raylib.h"
#include "ShaderSettings.h"

class PostProcessor
{
public:
    void SetSettings(const ShaderSettings& settings) { settings_ = settings; settings_.Clamp(); }
    bool Initialize();
    void Shutdown();
    bool BeginFrame(float renderScale);
    void EndFrameAndDraw(
        bool effectsEnabled,
        bool bloomEnabled,
        int effectsQuality,
        int bloomQuality,
        float damageFlash,
        float scopeBlend,
        bool reducedFlashes);

private:
    bool EnsureTarget(float renderScale);
    bool EnsureBloomTargets(float scale);

    ShaderSettings settings_;
    int exposureLocation_ = -1;
    int saturationLocation_ = -1;
    RenderTexture2D target_ {};
    RenderTexture2D bloomTarget_ {};
    RenderTexture2D bloomScratchTarget_ {};
    Shader shader_ {};
    Shader bloomShader_ {};
    Shader bloomBlurShader_ {};
    int bloomLocation_ = -1;
    int bloomTextureLocation_ = -1;
    int vignetteLocation_ = -1;
    int damageLocation_ = -1;
    int scopeLocation_ = -1;
    int texelSizeLocation_ = -1;
    int bloomSourceSizeLocation_ = -1;
    int bloomQualityLocation_ = -1;
    int bloomBlurStepLocation_ = -1;
    int targetWidth_ = 0;
    int targetHeight_ = 0;
    int bloomWidth_ = 0;
    int bloomHeight_ = 0;
    bool shaderReady_ = false;
    bool bloomShaderReady_ = false;
    bool bloomBlurShaderReady_ = false;
    bool frameActive_ = false;
};

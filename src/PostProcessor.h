#pragma once

#include "raylib.h"

class PostProcessor
{
public:
    bool Initialize();
    void Shutdown();
    bool BeginFrame(float renderScale);
    void EndFrameAndDraw(
        bool effectsEnabled,
        bool bloomEnabled,
        float damageFlash,
        float scopeBlend,
        bool reducedFlashes);

private:
    bool EnsureTarget(float renderScale);

    RenderTexture2D target_ {};
    Shader shader_ {};
    int sourceSizeLocation_ = -1;
    int timeLocation_ = -1;
    int bloomLocation_ = -1;
    int vignetteLocation_ = -1;
    int damageLocation_ = -1;
    int scopeLocation_ = -1;
    int targetWidth_ = 0;
    int targetHeight_ = 0;
    bool shaderReady_ = false;
    bool frameActive_ = false;
};

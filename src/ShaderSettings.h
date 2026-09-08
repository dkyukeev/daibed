#pragma once

#include <algorithm>
#include <cmath>

// Shared by the settings UI, scene lighting and final composite.
struct ShaderSettings
{
    int materialQuality = 1;
    int skyQuality = 1;
    int volumetricQuality = 0;
    int giQuality = 0;
    bool localShadows = false;
    float giStrength = 1.0f;
    float shadowSoftness = 1.5f;
    float haze = 1.0f;
    float sunIntensity = 1.0f;
    float exposure = 1.0f;
    float bloomIntensity = 1.0f;
    float saturation = 1.0f;

    void Clamp()
    {
        materialQuality = std::clamp(materialQuality, 0, 2);
        skyQuality = std::clamp(skyQuality, 0, 2);
        volumetricQuality = std::clamp(volumetricQuality, 0, 3);
        giQuality = std::clamp(giQuality, 0, 2);
        const auto finiteClamp = [](float v, float lo, float hi) {
            return std::isfinite(v) ? std::clamp(v, lo, hi) : 1.0f;
        };
        haze = finiteClamp(haze, 0.0f, 2.0f);
        giStrength = finiteClamp(giStrength, 0.0f, 2.0f);
        shadowSoftness = finiteClamp(shadowSoftness, 0.0f, 3.0f);
        sunIntensity = finiteClamp(sunIntensity, 0.0f, 2.0f);
        exposure = finiteClamp(exposure, 0.5f, 2.0f);
        bloomIntensity = finiteClamp(bloomIntensity, 0.0f, 2.0f);
        saturation = finiteClamp(saturation, 0.0f, 1.5f);
    }
};

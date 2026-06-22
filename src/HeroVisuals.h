#pragma once

#include "Hero.h"

#include "raylib.h"

#include <array>
#include <string>

struct HeroVisualAsset
{
    Model model {};
    Vector3 scale { 1.0f, 1.0f, 1.0f };
    Vector3 offset { 0.0f, -0.86f, 0.0f };
    float yawOffsetDegrees = 180.0f;
    ModelAnimation* animations = nullptr;
    int animationCount = 0;
    static constexpr int kAnimationStateCount = 17;
    std::array<int, kAnimationStateCount> animationByState {};
    std::string sourcePath;
    bool loaded = false;
};

class HeroVisualLibrary
{
public:
    bool Initialize();
    void Shutdown();

    const HeroVisualAsset* Find(HeroId id) const;
    bool ApplyAnimation(HeroId id, HeroAnimationState state, float stateProgress, float timeSeconds) const;

private:
    static constexpr int kHeroCount = 6;
    mutable std::array<HeroVisualAsset, kHeroCount> assets_ {};
    bool initialized_ = false;
};

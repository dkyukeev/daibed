#include "HeroVisuals.h"

#include "HeroSystem.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace
{
struct HeroVisualConfig
{
    HeroId id;
    const char* path;
    float yawOffsetDegrees;
};

constexpr std::array<HeroVisualConfig, HeroSystem::kHeroCount> kHeroVisualConfigs {
    HeroVisualConfig { HeroId::Radon, "assets/heroes/radon/Radon.glb", 180.0f },
    HeroVisualConfig { HeroId::Orbita, "assets/heroes/orbita/Orbita.glb", 180.0f },
    HeroVisualConfig { HeroId::Brom, "assets/heroes/brom/Brom.glb", 180.0f },
    HeroVisualConfig { HeroId::Konvoy, "assets/heroes/konvoy/Konvoy.glb", 180.0f },
    HeroVisualConfig { HeroId::Likho, "assets/heroes/likho/Likho.glb", 180.0f },
    HeroVisualConfig { HeroId::Svidetel, "assets/heroes/witness/Witness.glb", 180.0f }
};

std::string FindAssetFile(const std::string& relativePath)
{
    const std::string candidates[] {
        relativePath,
        "../" + relativePath,
        "../../" + relativePath
    };
    for (const std::string& candidate : candidates)
    {
        if (FileExists(candidate.c_str()))
        {
            return candidate;
        }
    }
    return {};
}

std::string Lowercase(const char* value)
{
    std::string result = value != nullptr ? value : "";
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::vector<const char*> AnimationNames(HeroAnimationState state)
{
    switch (state)
    {
    case HeroAnimationState::Idle: return { "idle" };
    case HeroAnimationState::Walk: return { "walk" };
    case HeroAnimationState::Run: return { "run", "sprint", "walk" };
    case HeroAnimationState::Jump: return { "jump" };
    case HeroAnimationState::Fall: return { "fall", "jump" };
    case HeroAnimationState::Attack: return { "attack", "shoot", "use" };
    case HeroAnimationState::Hurt: return { "hurt", "hit", "damage" };
    case HeroAnimationState::Death: return { "death", "die" };
    case HeroAnimationState::Ability1: return { "ability1", "ability_1", "cast" };
    case HeroAnimationState::Ability2: return { "ability2", "ability_2", "cast" };
    case HeroAnimationState::Ultimate: return { "ultimate", "ult", "cast" };
    case HeroAnimationState::WindUp: return { "windup", "wind_up", "cast" };
    case HeroAnimationState::Cast: return { "cast", "ability" };
    case HeroAnimationState::Recovery: return { "recovery", "recover", "idle" };
    case HeroAnimationState::Overloaded: return { "overloaded", "overload", "ability" };
    case HeroAnimationState::UltPrimed: return { "ultprimed", "ult_primed", "idle" };
    case HeroAnimationState::DeathSacrifice: return { "deathsacrifice", "sacrifice", "death" };
    }
    return { "idle" };
}

int AnimationStateIndex(HeroAnimationState state)
{
    return std::clamp(static_cast<int>(state), 0, HeroVisualAsset::kAnimationStateCount - 1);
}

bool Loops(HeroAnimationState state)
{
    return state == HeroAnimationState::Idle
        || state == HeroAnimationState::Walk
        || state == HeroAnimationState::Run
        || state == HeroAnimationState::Fall
        || state == HeroAnimationState::Overloaded
        || state == HeroAnimationState::UltPrimed;
}
}

bool HeroVisualLibrary::Initialize()
{
    if (initialized_)
    {
        return true;
    }

    bool anyLoaded = false;
    for (const HeroVisualConfig& config : kHeroVisualConfigs)
    {
        HeroVisualAsset& asset = assets_[HeroSystem::IndexOf(config.id)];
        asset.animationByState.fill(-1);
        const HeroHitboxProfile& hitbox = HeroSystem::GetHitboxProfile(config.id);
        asset.scale = Vector3 { hitbox.visualScale, hitbox.visualScale, hitbox.visualScale };
        asset.offset = hitbox.visualOffset;
        asset.yawOffsetDegrees = config.yawOffsetDegrees;
        asset.sourcePath = FindAssetFile(config.path);
        if (asset.sourcePath.empty())
        {
            TraceLog(LOG_WARNING, "HERO MODEL: %s not found; procedural fallback enabled", config.path);
            continue;
        }

        asset.model = LoadModel(asset.sourcePath.c_str());
        asset.loaded = asset.model.meshCount > 0;
        if (!asset.loaded)
        {
            TraceLog(LOG_WARNING, "HERO MODEL: failed to load %s; procedural fallback enabled", asset.sourcePath.c_str());
            UnloadModel(asset.model);
            asset.model = {};
            continue;
        }

        for (int materialIndex = 0; materialIndex < asset.model.materialCount; ++materialIndex)
        {
            const Texture2D texture = asset.model.materials[materialIndex].maps[MATERIAL_MAP_DIFFUSE].texture;
            if (texture.id != 0)
            {
                SetTextureFilter(texture, TEXTURE_FILTER_POINT);
            }
        }
        if (asset.model.boneCount > 0)
        {
            asset.animations = LoadModelAnimations(asset.sourcePath.c_str(), &asset.animationCount);
            for (int stateIndex = 0; stateIndex < HeroVisualAsset::kAnimationStateCount; ++stateIndex)
            {
                const HeroAnimationState state = static_cast<HeroAnimationState>(stateIndex);
                for (const char* candidate : AnimationNames(state))
                {
                    for (int animationIndex = 0; animationIndex < asset.animationCount; ++animationIndex)
                    {
                        const ModelAnimation& animation = asset.animations[animationIndex];
                        if (!IsModelAnimationValid(asset.model, animation))
                        {
                            continue;
                        }
                        const std::string name = Lowercase(animation.name);
                        if (name.find(candidate) != std::string::npos)
                        {
                            asset.animationByState[stateIndex] = animationIndex;
                            break;
                        }
                    }
                    if (asset.animationByState[stateIndex] >= 0)
                    {
                        break;
                    }
                }
            }
        }
        TraceLog(LOG_INFO, "HERO MODEL: loaded %s", asset.sourcePath.c_str());
        anyLoaded = true;
    }

    initialized_ = true;
    return anyLoaded;
}

void HeroVisualLibrary::Shutdown()
{
    if (!initialized_)
    {
        return;
    }

    for (HeroVisualAsset& asset : assets_)
    {
        if (asset.animations != nullptr)
        {
            UnloadModelAnimations(asset.animations, asset.animationCount);
            asset.animations = nullptr;
            asset.animationCount = 0;
        }
        if (asset.loaded)
        {
            UnloadModel(asset.model);
            asset.model = {};
            asset.loaded = false;
        }
        asset.sourcePath.clear();
    }
    initialized_ = false;
}

const HeroVisualAsset* HeroVisualLibrary::Find(HeroId id) const
{
    const HeroVisualAsset& asset = assets_[HeroSystem::IndexOf(id)];
    return asset.loaded ? &asset : nullptr;
}

bool HeroVisualLibrary::ApplyAnimation(
    HeroId id,
    HeroAnimationState state,
    float stateProgress,
    float timeSeconds) const
{
    HeroVisualAsset& asset = assets_[HeroSystem::IndexOf(id)];
    if (!asset.loaded || asset.animations == nullptr || asset.animationCount <= 0)
    {
        return false;
    }

    const int stateIndex = AnimationStateIndex(state);
    int animationIndex = asset.animationByState[stateIndex];
    if (animationIndex < 0)
    {
        animationIndex = asset.animationByState[AnimationStateIndex(HeroAnimationState::Idle)];
    }
    if (animationIndex < 0 || animationIndex >= asset.animationCount)
    {
        return false;
    }

    const ModelAnimation& animation = asset.animations[animationIndex];
    if (!IsModelAnimationValid(asset.model, animation) || animation.frameCount <= 0)
    {
        return false;
    }

    int frame = 0;
    if (Loops(state))
    {
        frame = static_cast<int>(std::max(0.0f, timeSeconds) * 30.0f) % animation.frameCount;
    }
    else
    {
        frame = static_cast<int>(std::clamp(stateProgress, 0.0f, 1.0f)
            * static_cast<float>(std::max(0, animation.frameCount - 1)));
    }
    UpdateModelAnimation(asset.model, animation, frame);
    return true;
}

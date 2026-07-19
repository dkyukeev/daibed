#include "AudioSystem.h"

#include "HeroSystem.h"
#include "raymath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace
{
constexpr float kPi = 3.1415926535f;
constexpr unsigned int kSampleRate = 22050;
constexpr float kSpatialNear = 3.0f;
constexpr float kSpatialFar = 46.0f;
constexpr double kHeroVoiceGlobalCooldown = 0.60;
constexpr double kHeroVoiceEventCooldown = 2.20;

float Envelope(float t)
{
    return std::sin(std::min(1.0f, t * 8.0f) * kPi * 0.5f) * (1.0f - t);
}

std::string FindAssetFile(const char* relativePath)
{
    const std::string relative = relativePath != nullptr ? relativePath : "";
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

float CueCooldown(int cue)
{
    switch (cue)
    {
    case 0: return 0.035f;
    case 5:
    case 6:
    case 7: return 0.045f;
    case 1:
    case 3: return 0.060f;
    default: return 0.025f;
    }
}

const char* HeroVoiceEventKey(HeroVoiceEvent event)
{
    switch (event)
    {
    case HeroVoiceEvent::Damage: return "damage";
    case HeroVoiceEvent::Kill: return "kill";
    case HeroVoiceEvent::VoidFall: return "void_fall";
    case HeroVoiceEvent::Active1: return "active1";
    case HeroVoiceEvent::Active1CoreDestroyed: return "active1_core_destroyed";
    case HeroVoiceEvent::Active1OverchargeFail: return "active1_overcharge_fail";
    case HeroVoiceEvent::Active2: return "active2";
    case HeroVoiceEvent::Active2CoreDestroyed: return "active2_core_destroyed";
    case HeroVoiceEvent::Ultimate: return "ultimate";
    case HeroVoiceEvent::UltimateCoreAlive: return "ultimate_core_alive";
    case HeroVoiceEvent::UltimateCoreDestroyed: return "ultimate_core_destroyed";
    case HeroVoiceEvent::UltimateRevealed: return "ultimate_revealed";
    case HeroVoiceEvent::UltimateLikhoDetected: return "ultimate_likho_detected";
    case HeroVoiceEvent::Count: break;
    }
    return "";
}

const char* HeroVoiceSlug(HeroId hero)
{
    switch (hero)
    {
    case HeroId::Radon: return "radon";
    case HeroId::Orbita: return "orbita";
    case HeroId::Brom: return "brom";
    case HeroId::Konvoy: return "konvoy";
    case HeroId::Likho: return "likho";
    case HeroId::Svidetel: return "witness";
    }
    return "";
}
}

bool AudioSystem::Initialize()
{
    if (ready_)
    {
        return true;
    }
    InitAudioDevice();
    ready_ = IsAudioDeviceReady();
    if (!ready_)
    {
        return false;
    }

    cues_[static_cast<int>(Cue::Hit)] = LoadCue("assets/audio/sfx/hit.wav", 420.0f, 0.08f, 0.35f, 900.0f);
    cues_[static_cast<int>(Cue::CoreHit)] = LoadCue("assets/audio/sfx/core_hit.wav", 180.0f, 0.16f, 0.42f, -90.0f);
    cues_[static_cast<int>(Cue::CoreDestroyed)] = LoadCue("assets/audio/sfx/core_destroyed.ogg", 96.0f, 0.45f, 0.55f, -42.0f);
    cues_[static_cast<int>(Cue::Pickup)] = LoadCue("assets/audio/sfx/pickup.wav", 720.0f, 0.09f, 0.28f, 520.0f);
    cues_[static_cast<int>(Cue::Purchase)] = LoadCue("assets/audio/sfx/purchase.wav", 860.0f, 0.10f, 0.30f, 380.0f);
    cues_[static_cast<int>(Cue::Build)] = LoadCue("assets/audio/sfx/build.wav", 260.0f, 0.07f, 0.32f, 120.0f);
    cues_[static_cast<int>(Cue::PlaceBlock)] = LoadCue("assets/audio/sfx/place_block.wav", 310.0f, 0.055f, 0.30f, -60.0f);
    cues_[static_cast<int>(Cue::BreakBlock)] = LoadCue("assets/audio/sfx/break_block.wav", 190.0f, 0.075f, 0.34f, -120.0f);
    cues_[static_cast<int>(Cue::Death)] = LoadCue("assets/audio/sfx/death.ogg", 120.0f, 0.24f, 0.42f, -70.0f);
    cues_[static_cast<int>(Cue::Denied)] = LoadCue("assets/audio/sfx/denied.wav", 110.0f, 0.12f, 0.35f, -40.0f);
    cues_[static_cast<int>(Cue::Landing)] = LoadCue("assets/audio/sfx/landing.wav", 150.0f, 0.10f, 0.28f, -70.0f);
    cues_[static_cast<int>(Cue::Victory)] = LoadCue("assets/audio/sfx/victory.ogg", 520.0f, 0.38f, 0.40f, 360.0f);
    lastPlayed_.fill(-100.0);
    for (auto& heroVoiceTimes : lastHeroVoicePlayed_)
    {
        heroVoiceTimes.fill(-100.0);
    }
    lastAnyHeroVoicePlayed_ = -100.0;
    LoadHeroVoices();
    SetMasterVolume(muted_ ? 0.0f : volume_);
    return true;
}

void AudioSystem::Shutdown()
{
    if (!ready_)
    {
        return;
    }
    for (Sound& sound : cues_)
    {
        if (sound.frameCount > 0)
        {
            UnloadSound(sound);
            sound = {};
        }
    }
    for (auto& heroVoiceEvents : heroVoices_)
    {
        for (std::vector<Sound>& variants : heroVoiceEvents)
        {
            for (Sound& sound : variants)
            {
                if (sound.frameCount > 0)
                {
                    UnloadSound(sound);
                    sound = {};
                }
            }
            variants.clear();
        }
    }
    CloseAudioDevice();
    ready_ = false;
}

bool AudioSystem::IsReady() const
{
    return ready_;
}

void AudioSystem::PlayHit() const { Play(Cue::Hit, AudioCategory::Sfx); }
void AudioSystem::PlayCoreHit() const { Play(Cue::CoreHit, AudioCategory::Sfx); }
void AudioSystem::PlayCoreDestroyed() const { Play(Cue::CoreDestroyed, AudioCategory::Sfx); }
void AudioSystem::PlayPickup() const { Play(Cue::Pickup, AudioCategory::Sfx); }
void AudioSystem::PlayPurchase() const { Play(Cue::Purchase, AudioCategory::Sfx); }
void AudioSystem::PlayBuild() const { Play(Cue::Build, AudioCategory::Sfx); }
void AudioSystem::PlayPlaceBlock() const { Play(Cue::PlaceBlock, AudioCategory::Sfx); }
void AudioSystem::PlayBreakBlock() const { Play(Cue::BreakBlock, AudioCategory::Sfx); }
void AudioSystem::PlayDeath() const { Play(Cue::Death, AudioCategory::Sfx); }
void AudioSystem::PlayDenied() const { Play(Cue::Denied, AudioCategory::Sfx); }
void AudioSystem::PlayLanding() const { Play(Cue::Landing, AudioCategory::Sfx); }
void AudioSystem::PlayVictory() const { Play(Cue::Victory, AudioCategory::Sfx); }

bool AudioSystem::PlayHeroVoice(HeroId hero, HeroVoiceEvent event, float gain) const
{
    if (!ready_ || muted_)
    {
        return false;
    }

    const int heroIndex = HeroSystem::IndexOf(hero);
    const int eventIndex = static_cast<int>(event);
    if (heroIndex < 0 || heroIndex >= static_cast<int>(heroVoices_.size())
        || eventIndex < 0 || eventIndex >= static_cast<int>(HeroVoiceEvent::Count))
    {
        return false;
    }

    const std::vector<Sound>& variants = heroVoices_[heroIndex][eventIndex];
    if (variants.empty())
    {
        return false;
    }

    const double now = GetTime();
    if (now - lastAnyHeroVoicePlayed_ < kHeroVoiceGlobalCooldown
        || now - lastHeroVoicePlayed_[heroIndex][eventIndex] < kHeroVoiceEventCooldown)
    {
        return false;
    }
    if (IsAnyHeroVoicePlaying())
    {
        return false;
    }

    const int choice = GetRandomValue(0, static_cast<int>(variants.size()) - 1);
    const Sound& sound = variants[static_cast<std::size_t>(choice)];
    if (sound.frameCount == 0)
    {
        return false;
    }

    SetSoundVolume(sound, std::clamp(gain * sfxVolume_ * 0.92f, 0.0f, 1.0f));
    SetSoundPan(sound, 0.5f);
    SetSoundPitch(sound, 1.0f);
    PlaySound(sound);
    lastHeroVoicePlayed_[heroIndex][eventIndex] = now;
    lastAnyHeroVoicePlayed_ = now;
    return true;
}

bool AudioSystem::IsAnyHeroVoicePlaying() const
{
    for (const auto& heroVoiceEvents : heroVoices_)
    {
        for (const std::vector<Sound>& variants : heroVoiceEvents)
        {
            for (const Sound& sound : variants)
            {
                if (sound.frameCount > 0 && IsSoundPlaying(sound))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

void AudioSystem::PlayHitAt(Vector3 position) const { PlayAt(Cue::Hit, position, AudioCategory::Sfx); }
void AudioSystem::PlayBuildAt(Vector3 position) const { PlayAt(Cue::Build, position, AudioCategory::Sfx); }
void AudioSystem::PlayPlaceBlockAt(Vector3 position) const { PlayAt(Cue::PlaceBlock, position, AudioCategory::Sfx); }
void AudioSystem::PlayBreakBlockAt(Vector3 position) const { PlayAt(Cue::BreakBlock, position, AudioCategory::Sfx); }

void AudioSystem::SetListener(Vector3 position, Vector3 right)
{
    listenerPosition_ = position;
    listenerRight_ = Vector3LengthSqr(right) > 0.0001f ? Vector3Normalize(right) : Vector3 { 1.0f, 0.0f, 0.0f };
}

Sound AudioSystem::CreateTone(float frequency, float duration, float volume, float slide) const
{
    const unsigned int frameCount = static_cast<unsigned int>(duration * static_cast<float>(kSampleRate));
    short* samples = static_cast<short*>(MemAlloc(static_cast<unsigned int>(frameCount * sizeof(short))));
    float phase = 0.0f;
    for (unsigned int i = 0; i < frameCount; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(frameCount);
        const float hz = std::max(20.0f, frequency + slide * t);
        phase += hz / static_cast<float>(kSampleRate);
        const float shaped = std::sin(phase * kPi * 2.0f) * Envelope(t) * volume;
        samples[i] = static_cast<short>(std::clamp(shaped, -1.0f, 1.0f) * 32767.0f);
    }
    Wave wave { frameCount, kSampleRate, 16, 1, samples };
    Sound sound = LoadSoundFromWave(wave);
    UnloadWave(wave);
    return sound;
}

Sound AudioSystem::LoadCue(
    const char* relativePath,
    float frequency,
    float duration,
    float volume,
    float slide) const
{
    const std::string path = FindAssetFile(relativePath);
    if (!path.empty())
    {
        Sound external = LoadSound(path.c_str());
        if (external.frameCount > 0)
        {
            TraceLog(LOG_INFO, "AUDIO: loaded %s", path.c_str());
            return external;
        }
    }
    return CreateTone(frequency, duration, volume, slide);
}

void AudioSystem::LoadHeroVoices()
{
    for (int heroIndex = 0; heroIndex < HeroSystem::kHeroCount; ++heroIndex)
    {
        const HeroId hero = HeroSystem::IdFromIndex(heroIndex);
        const char* heroSlug = HeroVoiceSlug(hero);
        for (int eventIndex = 0; eventIndex < static_cast<int>(HeroVoiceEvent::Count); ++eventIndex)
        {
            const char* eventKey = HeroVoiceEventKey(static_cast<HeroVoiceEvent>(eventIndex));
            LoadHeroVoiceSet(heroIndex, eventIndex, heroSlug, eventKey);
        }
    }
}

void AudioSystem::LoadHeroVoiceSet(int heroIndex, int eventIndex, const char* heroSlug, const char* eventKey)
{
    if (heroSlug == nullptr || eventKey == nullptr || heroSlug[0] == '\0' || eventKey[0] == '\0')
    {
        return;
    }

    std::vector<Sound>& variants = heroVoices_[heroIndex][eventIndex];
    for (int variant = 1; variant <= 24; ++variant)
    {
        char relativePath[160] {};
        std::snprintf(
            relativePath,
            sizeof(relativePath),
            "assets/audio/voice/%s/%s_%02d.wav",
            heroSlug,
            eventKey,
            variant);
        const std::string path = FindAssetFile(relativePath);
        if (path.empty())
        {
            continue;
        }

        Sound sound = LoadSound(path.c_str());
        if (sound.frameCount > 0)
        {
            variants.push_back(sound);
        }
    }
}

void AudioSystem::SetMuted(bool muted)
{
    muted_ = muted;
    if (ready_)
    {
        SetMasterVolume(muted_ ? 0.0f : volume_);
    }
}

bool AudioSystem::IsMuted() const
{
    return muted_;
}

void AudioSystem::SetVolume(float volume)
{
    volume_ = std::clamp(volume, 0.0f, 1.0f);
    if (ready_)
    {
        SetMasterVolume(muted_ ? 0.0f : volume_);
    }
}

void AudioSystem::SetCategoryVolumes(float sfx, float ambient)
{
    sfxVolume_ = std::clamp(sfx, 0.0f, 1.0f);
    ambientVolume_ = std::clamp(ambient, 0.0f, 1.0f);
}

void AudioSystem::Play(Cue cue, AudioCategory category, float gain, float pan, float pitch) const
{
    if (!ready_ || muted_)
    {
        return;
    }
    const int index = static_cast<int>(cue);
    const Sound& sound = cues_[index];
    if (sound.frameCount == 0)
    {
        return;
    }
    const double now = GetTime();
    if (now - lastPlayed_[index] < CueCooldown(index) && IsSoundPlaying(sound))
    {
        return;
    }
    const float categoryVolume = category == AudioCategory::Ambient ? ambientVolume_ : sfxVolume_;
    SetSoundVolume(sound, std::clamp(gain * categoryVolume, 0.0f, 1.0f));
    SetSoundPan(sound, std::clamp(pan, 0.0f, 1.0f));
    SetSoundPitch(sound, std::clamp(pitch, 0.75f, 1.35f));
    PlaySound(sound);
    lastPlayed_[index] = now;
}

void AudioSystem::PlayAt(Cue cue, Vector3 position, AudioCategory category, float gain) const
{
    const Vector3 offset = Vector3Subtract(position, listenerPosition_);
    const float distance = Vector3Length(offset);
    if (distance >= kSpatialFar)
    {
        return;
    }
    const float distanceFraction = std::clamp((distance - kSpatialNear) / (kSpatialFar - kSpatialNear), 0.0f, 1.0f);
    const float attenuation = (1.0f - distanceFraction) * (1.0f - distanceFraction);
    const Vector3 direction = distance > 0.001f ? Vector3Scale(offset, 1.0f / distance) : Vector3 {};
    const float pan = 0.5f + Vector3DotProduct(direction, listenerRight_) * 0.42f;
    const float variation = 1.0f + std::sin(static_cast<float>(GetTime()) * 91.7f + static_cast<float>(static_cast<int>(cue))) * 0.025f;
    Play(cue, category, gain * attenuation, pan, variation);
}

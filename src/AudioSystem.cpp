#include "AudioSystem.h"

#include "raymath.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
constexpr float kPi = 3.1415926535f;
constexpr unsigned int kSampleRate = 22050;
constexpr float kSpatialNear = 3.0f;
constexpr float kSpatialFar = 46.0f;

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

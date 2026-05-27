#include "AudioSystem.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kPi = 3.1415926535f;
constexpr unsigned int kSampleRate = 22050;

float Envelope(float t)
{
    return std::sin(std::min(1.0f, t * 8.0f) * kPi * 0.5f) * (1.0f - t);
}
}

bool AudioSystem::Initialize()
{
    InitAudioDevice();
    ready_ = IsAudioDeviceReady();
    if (!ready_)
    {
        return false;
    }

    hit_ = CreateTone(420.0f, 0.08f, 0.35f, 900.0f);
    coreHit_ = CreateTone(180.0f, 0.16f, 0.42f, -90.0f);
    coreDestroyed_ = CreateTone(96.0f, 0.45f, 0.55f, -42.0f);
    pickup_ = CreateTone(720.0f, 0.09f, 0.28f, 520.0f);
    purchase_ = CreateTone(860.0f, 0.10f, 0.30f, 380.0f);
    build_ = CreateTone(260.0f, 0.07f, 0.32f, 120.0f);
    placeBlock_ = CreateTone(310.0f, 0.055f, 0.30f, -60.0f);
    breakBlock_ = CreateTone(190.0f, 0.075f, 0.34f, -120.0f);
    death_ = CreateTone(120.0f, 0.24f, 0.42f, -70.0f);
    denied_ = CreateTone(110.0f, 0.12f, 0.35f, -40.0f);
    landing_ = CreateTone(150.0f, 0.10f, 0.28f, -70.0f);
    victory_ = CreateTone(520.0f, 0.38f, 0.40f, 360.0f);
    return true;
}

void AudioSystem::Shutdown()
{
    if (!ready_)
    {
        return;
    }

    UnloadSound(hit_);
    UnloadSound(coreHit_);
    UnloadSound(coreDestroyed_);
    UnloadSound(pickup_);
    UnloadSound(purchase_);
    UnloadSound(build_);
    UnloadSound(placeBlock_);
    UnloadSound(breakBlock_);
    UnloadSound(death_);
    UnloadSound(denied_);
    UnloadSound(landing_);
    UnloadSound(victory_);
    CloseAudioDevice();
    ready_ = false;
}

void AudioSystem::PlayHit() const
{
    Play(hit_);
}

void AudioSystem::PlayCoreHit() const
{
    Play(coreHit_);
}

void AudioSystem::PlayCoreDestroyed() const
{
    Play(coreDestroyed_);
}

void AudioSystem::PlayPickup() const
{
    Play(pickup_);
}

void AudioSystem::PlayPurchase() const
{
    Play(purchase_);
}

void AudioSystem::PlayBuild() const
{
    Play(build_);
}

void AudioSystem::PlayPlaceBlock() const
{
    Play(placeBlock_);
}

void AudioSystem::PlayBreakBlock() const
{
    Play(breakBlock_);
}

void AudioSystem::PlayDeath() const
{
    Play(death_);
}

void AudioSystem::PlayDenied() const
{
    Play(denied_);
}

void AudioSystem::PlayLanding() const
{
    Play(landing_);
}

void AudioSystem::PlayVictory() const
{
    Play(victory_);
}

Sound AudioSystem::CreateTone(float frequency, float duration, float volume, float slide) const
{
    const unsigned int frameCount = static_cast<unsigned int>(duration * static_cast<float>(kSampleRate));
    short* samples = static_cast<short*>(MemAlloc(frameCount * sizeof(short)));
    float phase = 0.0f;

    for (unsigned int i = 0; i < frameCount; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(frameCount);
        const float hz = std::max(20.0f, frequency + slide * t);
        phase += hz / static_cast<float>(kSampleRate);

        const float tone = std::sin(phase * kPi * 2.0f);
        const float shaped = tone * Envelope(t) * volume;
        samples[i] = static_cast<short>(std::clamp(shaped, -1.0f, 1.0f) * 32767.0f);
    }

    Wave wave {
        frameCount,
        kSampleRate,
        16,
        1,
        samples
    };
    Sound sound = LoadSoundFromWave(wave);
    UnloadWave(wave);
    return sound;
}

void AudioSystem::Play(const Sound& sound) const
{
    if (ready_)
    {
        PlaySound(sound);
    }
}

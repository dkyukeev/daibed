#pragma once

#include "raylib.h"

#include <array>

enum class MusicMood
{
    Menu,
    Match,
    Intense,
    Victory,
    Count
};

class MusicSystem
{
public:
    bool Initialize(bool audioDeviceReady);
    void Shutdown();
    void Update(MusicMood mood);
    void SetVolume(float master, float music);

private:
    std::array<Music, static_cast<int>(MusicMood::Count)> tracks_ {};
    std::array<bool, static_cast<int>(MusicMood::Count)> loaded_ {};
    MusicMood currentMood_ = MusicMood::Count;
    float masterVolume_ = 1.0f;
    float musicVolume_ = 0.7f;
    bool ready_ = false;
};

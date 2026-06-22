#include "MusicSystem.h"

#include <algorithm>
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
}

bool MusicSystem::Initialize(bool audioDeviceReady)
{
    if (ready_ || !audioDeviceReady)
    {
        return ready_;
    }
    const char* paths[] {
        "assets/audio/music/menu.ogg",
        "assets/audio/music/match.ogg",
        "assets/audio/music/intense.ogg",
        "assets/audio/music/victory.ogg"
    };
    bool anyLoaded = false;
    for (int i = 0; i < static_cast<int>(MusicMood::Count); ++i)
    {
        const std::string path = FindAssetFile(paths[i]);
        if (path.empty())
        {
            continue;
        }
        tracks_[i] = LoadMusicStream(path.c_str());
        loaded_[i] = tracks_[i].frameCount > 0;
        if (loaded_[i])
        {
            tracks_[i].looping = i != static_cast<int>(MusicMood::Victory);
            anyLoaded = true;
            TraceLog(LOG_INFO, "MUSIC: loaded %s", path.c_str());
        }
    }
    ready_ = true;
    return anyLoaded;
}

void MusicSystem::Shutdown()
{
    if (!ready_)
    {
        return;
    }
    for (int i = 0; i < static_cast<int>(MusicMood::Count); ++i)
    {
        if (loaded_[i])
        {
            StopMusicStream(tracks_[i]);
            UnloadMusicStream(tracks_[i]);
            tracks_[i] = {};
            loaded_[i] = false;
        }
    }
    currentMood_ = MusicMood::Count;
    ready_ = false;
}

void MusicSystem::Update(MusicMood mood)
{
    if (!ready_)
    {
        return;
    }
    int requested = static_cast<int>(mood);
    if (requested < 0 || requested >= static_cast<int>(MusicMood::Count) || !loaded_[requested])
    {
        requested = static_cast<int>(MusicMood::Match);
        if (!loaded_[requested])
        {
            return;
        }
        mood = MusicMood::Match;
    }
    if (currentMood_ != mood)
    {
        const int previous = static_cast<int>(currentMood_);
        if (previous >= 0 && previous < static_cast<int>(MusicMood::Count) && loaded_[previous])
        {
            StopMusicStream(tracks_[previous]);
        }
        currentMood_ = mood;
        SetMusicVolume(tracks_[requested], masterVolume_ * musicVolume_);
        PlayMusicStream(tracks_[requested]);
    }
    SetMusicVolume(tracks_[requested], masterVolume_ * musicVolume_);
    UpdateMusicStream(tracks_[requested]);
}

void MusicSystem::SetVolume(float master, float music)
{
    masterVolume_ = std::clamp(master, 0.0f, 1.0f);
    musicVolume_ = std::clamp(music, 0.0f, 1.0f);
}

#pragma once

#include "Hero.h"

#include "raylib.h"

#include <array>
#include <vector>

enum class AudioCategory
{
    Sfx,
    Ambient
};

enum class HeroVoiceEvent
{
    Damage,
    Kill,
    VoidFall,
    Active1,
    Active1CoreDestroyed,
    Active1OverchargeFail,
    Active2,
    Active2CoreDestroyed,
    Ultimate,
    UltimateCoreAlive,
    UltimateCoreDestroyed,
    UltimateRevealed,
    UltimateLikhoDetected,
    Count
};

class AudioSystem
{
public:
    bool Initialize();
    void Shutdown();
    bool IsReady() const;

    void PlayHit() const;
    void PlayCoreHit() const;
    void PlayCoreDestroyed() const;
    void PlayPickup() const;
    void PlayPurchase() const;
    void PlayBuild() const;
    void PlayPlaceBlock() const;
    void PlayBreakBlock() const;
    void PlayDeath() const;
    void PlayDenied() const;
    void PlayLanding() const;
    void PlayVictory() const;
    bool PlayHeroVoice(HeroId hero, HeroVoiceEvent event, float gain = 1.0f) const;

    void PlayHitAt(Vector3 position) const;
    void PlayBuildAt(Vector3 position) const;
    void PlayPlaceBlockAt(Vector3 position) const;
    void PlayBreakBlockAt(Vector3 position) const;
    void SetListener(Vector3 position, Vector3 right);

    bool IsMuted() const;
    void SetMuted(bool muted);
    void SetVolume(float volume);
    void SetCategoryVolumes(float sfx, float ambient);

private:
    enum class Cue : int
    {
        Hit,
        CoreHit,
        CoreDestroyed,
        Pickup,
        Purchase,
        Build,
        PlaceBlock,
        BreakBlock,
        Death,
        Denied,
        Landing,
        Victory,
        Count
    };

    Sound CreateTone(float frequency, float duration, float volume, float slide) const;
    Sound LoadCue(const char* relativePath, float frequency, float duration, float volume, float slide) const;
    void LoadHeroVoices();
    void LoadHeroVoiceSet(int heroIndex, int eventIndex, const char* heroSlug, const char* eventKey);
    bool IsAnyHeroVoicePlaying() const;
    void Play(Cue cue, AudioCategory category, float gain = 1.0f, float pan = 0.5f, float pitch = 1.0f) const;
    void PlayAt(Cue cue, Vector3 position, AudioCategory category, float gain = 1.0f) const;

    bool ready_ = false;
    bool muted_ = false;
    float volume_ = 1.0f;
    float sfxVolume_ = 1.0f;
    float ambientVolume_ = 0.75f;
    Vector3 listenerPosition_ {};
    Vector3 listenerRight_ { 1.0f, 0.0f, 0.0f };
    std::array<Sound, static_cast<int>(Cue::Count)> cues_ {};
    mutable std::array<double, static_cast<int>(Cue::Count)> lastPlayed_ {};
    std::array<std::array<std::vector<Sound>, static_cast<int>(HeroVoiceEvent::Count)>, 6> heroVoices_ {};
    mutable std::array<std::array<double, static_cast<int>(HeroVoiceEvent::Count)>, 6> lastHeroVoicePlayed_ {};
    mutable double lastAnyHeroVoicePlayed_ = -100.0;
};

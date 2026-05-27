#pragma once

#include "raylib.h"

class AudioSystem
{
public:
    bool Initialize();
    void Shutdown();

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

private:
    Sound CreateTone(float frequency, float duration, float volume, float slide) const;
    void Play(const Sound& sound) const;

    bool ready_ = false;
    Sound hit_ {};
    Sound coreHit_ {};
    Sound coreDestroyed_ {};
    Sound pickup_ {};
    Sound purchase_ {};
    Sound build_ {};
    Sound placeBlock_ {};
    Sound breakBlock_ {};
    Sound death_ {};
    Sound denied_ {};
    Sound landing_ {};
    Sound victory_ {};
};

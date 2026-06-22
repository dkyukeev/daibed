#include "Hero.h"

const char* HeroIdKey(HeroId id)
{
    switch (id)
    {
    case HeroId::Radon:
        return "radon";
    case HeroId::Orbita:
        return "orbita";
    case HeroId::Brom:
        return "brom";
    case HeroId::Konvoy:
        return "konvoy";
    case HeroId::Likho:
        return "likho";
    case HeroId::Svidetel:
        return "svidetel";
    }
    return "radon";
}

const char* HeroAbilitySlotName(HeroAbilitySlot slot)
{
    switch (slot)
    {
    case HeroAbilitySlot::Active1:
        return "1 активка";
    case HeroAbilitySlot::Active2:
        return "2 активка";
    case HeroAbilitySlot::Ultimate:
        return "Ульта";
    }
    return "Способность";
}

const char* HeroAnimationStateName(HeroAnimationState state)
{
    switch (state)
    {
    case HeroAnimationState::Idle: return "Idle";
    case HeroAnimationState::Walk: return "Walk";
    case HeroAnimationState::Run: return "Run";
    case HeroAnimationState::Jump: return "Jump";
    case HeroAnimationState::Fall: return "Fall";
    case HeroAnimationState::Attack: return "Attack";
    case HeroAnimationState::Hurt: return "Hurt";
    case HeroAnimationState::Death: return "Death";
    case HeroAnimationState::Ability1: return "Ability1";
    case HeroAnimationState::Ability2: return "Ability2";
    case HeroAnimationState::Ultimate: return "Ultimate";
    case HeroAnimationState::WindUp: return "WindUp";
    case HeroAnimationState::Cast: return "Cast";
    case HeroAnimationState::Recovery: return "Recovery";
    case HeroAnimationState::Overloaded: return "Overloaded";
    case HeroAnimationState::UltPrimed: return "UltPrimed";
    case HeroAnimationState::DeathSacrifice: return "DeathSacrifice";
    }
    return "Unknown";
}

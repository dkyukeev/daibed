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
    case HeroAnimationState::Idle: return "Покой";
    case HeroAnimationState::Walk: return "Ходьба";
    case HeroAnimationState::Run: return "Бег";
    case HeroAnimationState::Jump: return "Прыжок";
    case HeroAnimationState::Fall: return "Падение";
    case HeroAnimationState::Attack: return "Атака";
    case HeroAnimationState::Hurt: return "Урон";
    case HeroAnimationState::Death: return "Смерть";
    case HeroAnimationState::Ability1: return "Способность 1";
    case HeroAnimationState::Ability2: return "Способность 2";
    case HeroAnimationState::Ultimate: return "Ульта";
    case HeroAnimationState::WindUp: return "Замах";
    case HeroAnimationState::Cast: return "Каст";
    case HeroAnimationState::Recovery: return "Восстановление";
    case HeroAnimationState::Overloaded: return "Перегрузка";
    case HeroAnimationState::UltPrimed: return "Ульта готова";
    case HeroAnimationState::DeathSacrifice: return "Жертва";
    }
    return "Неизвестно";
}

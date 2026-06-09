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

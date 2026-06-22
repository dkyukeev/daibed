#pragma once

#include "Hero.h"

#include <array>

class HeroSystem
{
public:
    static constexpr int kHeroCount = 6;

    static const std::array<HeroDefinition, kHeroCount>& Definitions();
    static const HeroDefinition& GetDefinition(HeroId id);
    static const HeroDefinition& GetDefinitionByIndex(int index);
    static const HeroHitboxProfile& GetHitboxProfile(HeroId id);
    static HeroId IdFromIndex(int index);
    static int IndexOf(HeroId id);
};

#pragma once

#include "Block.h"
#include "raylib.h"

#include <string>

enum class TeamColor
{
    Red,
    Blue,
    Green,
    Yellow
};

struct Team
{
    int id = -1;
    TeamColor color = TeamColor::Red;
    std::string name;
    Vector3 spawnPoint {};
    GridPos coreBlock {};
    Vector3 shopPosition {};
    bool coreAlive = true;
    int forgeLevel = 0;
    int healAuraLevel = 0;
    bool enemyTrackerUnlocked = false;
};

Color GetTeamColor(TeamColor color);
const char* ToString(TeamColor color);

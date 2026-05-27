#include "Team.h"

Color GetTeamColor(TeamColor color)
{
    switch (color)
    {
    case TeamColor::Red:
        return Color { 230, 74, 74, 255 };
    case TeamColor::Blue:
        return Color { 74, 135, 230, 255 };
    case TeamColor::Green:
        return Color { 64, 190, 110, 255 };
    case TeamColor::Yellow:
        return Color { 236, 202, 72, 255 };
    }

    return WHITE;
}

const char* ToString(TeamColor color)
{
    switch (color)
    {
    case TeamColor::Red:
        return "Red";
    case TeamColor::Blue:
        return "Blue";
    case TeamColor::Green:
        return "Green";
    case TeamColor::Yellow:
        return "Yellow";
    }

    return "Unknown";
}


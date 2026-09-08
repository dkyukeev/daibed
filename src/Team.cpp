#include "Team.h"
#include "VisualTheme.h"

Color GetTeamColor(TeamColor color)
{
    return VisualTheme::TeamIdentity(color);
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


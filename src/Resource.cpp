#include "Resource.h"

const char* ToString(ResourceType type)
{
    switch (type)
    {
    case ResourceType::Iron:
        return "Iron";
    case ResourceType::Gold:
        return "Gold";
    case ResourceType::Crystal:
        return "Crystal";
    }

    return "Unknown";
}


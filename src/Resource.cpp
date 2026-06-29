#include "Resource.h"

const char* ToString(ResourceType type)
{
    switch (type)
    {
    case ResourceType::Iron:
        return "Железо";
    case ResourceType::Gold:
        return "Золото";
    case ResourceType::Crystal:
        return "Кристалл";
    }

    return "Неизвестно";
}


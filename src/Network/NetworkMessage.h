#pragma once

#include <cstdint>
#include <string>

enum class NetworkMessageType
{
    Connect,
    Disconnect,
    Input,
    Snapshot,
    Event
};

struct NetworkMessage
{
    NetworkMessageType type = NetworkMessageType::Event;
    std::uint32_t senderId = 0;
    std::string payload;
};


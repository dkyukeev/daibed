#pragma once

#include "NetworkMessage.h"

#include <vector>

class Client
{
public:
    virtual ~Client() = default;

    virtual void Connect() = 0;
    virtual void Disconnect() = 0;
    virtual void SendToServer(const NetworkMessage& message) = 0;
    virtual std::vector<NetworkMessage> ConsumeIncoming() = 0;
};


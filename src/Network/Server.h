#pragma once

#include "NetworkMessage.h"

#include <vector>

class Server
{
public:
    virtual ~Server() = default;

    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual void ReceiveFromClient(const NetworkMessage& message) = 0;
    virtual std::vector<NetworkMessage> ConsumeOutgoing() = 0;
};


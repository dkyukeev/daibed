#pragma once

#include "Client.h"
#include "Server.h"

#include <queue>
#include <vector>

class LocalNetworkMock final : public Server, public Client
{
public:
    void Start() override;
    void Stop() override;
    void ReceiveFromClient(const NetworkMessage& message) override;
    std::vector<NetworkMessage> ConsumeOutgoing() override;

    void Connect() override;
    void Disconnect() override;
    void SendToServer(const NetworkMessage& message) override;
    std::vector<NetworkMessage> ConsumeIncoming() override;

    void Pump();
    bool IsRunning() const;
    bool IsConnected() const;

private:
    bool running_ = false;
    bool connected_ = false;
    std::queue<NetworkMessage> clientToServer_;
    std::queue<NetworkMessage> serverToClient_;
};


#include "Network/LocalNetworkMock.h"

namespace
{
std::vector<NetworkMessage> Drain(std::queue<NetworkMessage>& queue)
{
    std::vector<NetworkMessage> messages;
    while (!queue.empty())
    {
        messages.push_back(queue.front());
        queue.pop();
    }
    return messages;
}
}

void LocalNetworkMock::Start()
{
    running_ = true;
}

void LocalNetworkMock::Stop()
{
    running_ = false;
    connected_ = false;
}

void LocalNetworkMock::ReceiveFromClient(const NetworkMessage& message)
{
    if (!running_)
    {
        return;
    }

    serverToClient_.push(NetworkMessage { NetworkMessageType::Event, 0, "server_ack:" + message.payload });
}

std::vector<NetworkMessage> LocalNetworkMock::ConsumeOutgoing()
{
    return Drain(serverToClient_);
}

void LocalNetworkMock::Connect()
{
    if (!running_)
    {
        Start();
    }

    connected_ = true;
    clientToServer_.push(NetworkMessage { NetworkMessageType::Connect, 1, "local_client" });
}

void LocalNetworkMock::Disconnect()
{
    connected_ = false;
    clientToServer_.push(NetworkMessage { NetworkMessageType::Disconnect, 1, "local_client" });
}

void LocalNetworkMock::SendToServer(const NetworkMessage& message)
{
    if (connected_)
    {
        clientToServer_.push(message);
    }
}

std::vector<NetworkMessage> LocalNetworkMock::ConsumeIncoming()
{
    return Drain(serverToClient_);
}

void LocalNetworkMock::Pump()
{
    while (!clientToServer_.empty())
    {
        ReceiveFromClient(clientToServer_.front());
        clientToServer_.pop();
    }
}

bool LocalNetworkMock::IsRunning() const
{
    return running_;
}

bool LocalNetworkMock::IsConnected() const
{
    return connected_;
}


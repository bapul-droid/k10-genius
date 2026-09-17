#pragma once

#include <memory>
#include <string>

#include <web_socket.h>

class GeniusDeviceChannel {
public:
    static GeniusDeviceChannel& GetInstance();

    void Start();
    void Stop();

private:
    GeniusDeviceChannel() = default;

    void Run();
    bool Connect();
    void HandleText(const char* data, size_t len);

    bool SendHello();
    bool SendResult(
        const std::string& command_id,
        bool recognized,
        bool accepted,
        const std::string& error
    );

    std::unique_ptr<WebSocket> websocket_;

    bool running_ = false;
    bool connected_ = false;
};

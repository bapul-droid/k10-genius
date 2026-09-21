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
    bool SendHeartbeat();
    bool SendResponse(
        const std::string& request_id,
        bool accepted
    );
    bool SendError(
        const std::string& request_id,
        const std::string& code,
        const std::string& message
    );

    std::unique_ptr<WebSocket> websocket_;

    bool running_ = false;
    bool connected_ = false;
};

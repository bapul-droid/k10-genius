#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <web_socket.h>

class PropertyList;
struct cJSON;

class GeniusDeviceChannel {
public:
    static GeniusDeviceChannel& GetInstance();

    void Start();
    void Stop();

private:
    struct PendingSkillCall {
        SemaphoreHandle_t done = nullptr;
        std::string result;
        std::string error;
    };

    GeniusDeviceChannel() = default;

    void Run();
    bool Connect();
    void HandleText(const char* data, size_t len);
    void RegisterSkillCatalog(const cJSON* data);

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
    bool SendSkillCall(
        const std::string& request_id,
        const std::string& name,
        const PropertyList& properties
    );
    std::string CallSkill(
        const std::string& name,
        const PropertyList& properties,
        std::string& error
    );
    void HandleSkillResult(const cJSON* data);

    std::unique_ptr<WebSocket> websocket_;

    bool running_ = false;
    bool connected_ = false;
    bool catalog_registered_ = false;

    std::atomic<uint32_t> skill_request_counter_{0};
    std::mutex pending_mutex_;
    std::string pending_request_id_;
    std::shared_ptr<PendingSkillCall> pending_skill_call_;
};

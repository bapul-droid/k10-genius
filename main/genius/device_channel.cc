#include "device_channel.h"

#include <cJSON.h>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "boards/common/board.h"
#include "command_handler.h"
#include "system_info.h"

namespace {

static const char* TAG = "GeniusDeviceChannel";
static const char* kUrl = "wss://genius.minjiai.my.id/device/ws";

constexpr TickType_t kInitialDelay = pdMS_TO_TICKS(3000);
constexpr TickType_t kLoopDelay = pdMS_TO_TICKS(1000);
constexpr TickType_t kHeartbeatInterval = pdMS_TO_TICKS(30000);

std::string JsonString(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    cJSON_free(text);
    return result;
}

}  // namespace

GeniusDeviceChannel& GeniusDeviceChannel::GetInstance() {
    static GeniusDeviceChannel instance;
    return instance;
}

void GeniusDeviceChannel::Start() {
    if (running_) {
        return;
    }

    running_ = true;
    auto result = xTaskCreate(
        [](void* arg) {
            static_cast<GeniusDeviceChannel*>(arg)->Run();
            vTaskDelete(nullptr);
        },
        "genius_channel",
        6144,
        this,
        3,
        nullptr
    );

    if (result != pdPASS) {
        running_ = false;
        ESP_LOGE(TAG, "Failed to create channel task");
    }
}

void GeniusDeviceChannel::Stop() {
    running_ = false;
    connected_ = false;
    websocket_.reset();
}

void GeniusDeviceChannel::Run() {
    ESP_LOGI(TAG, "Genius V2 channel task started");
    vTaskDelay(kInitialDelay);

    TickType_t last_heartbeat = 0;

    while (running_) {
        if (!connected_) {
            if (Connect()) {
                last_heartbeat = xTaskGetTickCount();
            } else {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if ((now - last_heartbeat) >= kHeartbeatInterval) {
            if (!SendHeartbeat()) {
                ESP_LOGW(TAG, "Heartbeat failed");
                connected_ = false;
                websocket_.reset();
            } else {
                last_heartbeat = now;
            }
        }

        vTaskDelay(kLoopDelay);
    }

    connected_ = false;
    websocket_.reset();
    ESP_LOGI(TAG, "Genius V2 channel task stopped");
}

bool GeniusDeviceChannel::Connect() {
    websocket_.reset();
    connected_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);

    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    websocket_->OnData(
        [this](const char* data, size_t len, bool binary) {
            if (!binary) {
                HandleText(data, len);
            }
        }
    );

    websocket_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Disconnected");
        connected_ = false;
    });

    ESP_LOGI(TAG, "Connecting V2: %s", kUrl);
    auto result = websocket_->Connect(kUrl);

    if (!result) {
        ESP_LOGW(TAG, "Connect failed: %s", result.error().ToString().c_str());
        websocket_.reset();
        return false;
    }

    connected_ = true;
    ESP_LOGI(TAG, "Connected to Genius V2");

    if (!SendHello()) {
        ESP_LOGW(TAG, "Failed to send V2 hello");
        connected_ = false;
        websocket_.reset();
        return false;
    }

    ESP_LOGI(TAG, "V2 hello sent");
    return true;
}

bool GeniusDeviceChannel::SendHello() {
    if (websocket_ == nullptr || !connected_) {
        return false;
    }

    const auto device_id = SystemInfo::GetMacAddress();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "name", "DFRobot K10");

    cJSON* metadata = cJSON_AddObjectToObject(root, "metadata");
    cJSON_AddStringToObject(metadata, "board", "df-k10");
    cJSON_AddStringToObject(metadata, "firmware", "2.5.0");
    cJSON_AddStringToObject(metadata, "genius", "v2");

    auto text = JsonString(root);
    cJSON_Delete(root);

    return !text.empty() && websocket_->Send(text);
}

bool GeniusDeviceChannel::SendHeartbeat() {
    if (websocket_ == nullptr || !connected_) {
        return false;
    }

    const auto device_id = SystemInfo::GetMacAddress();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());

    auto text = JsonString(root);
    cJSON_Delete(root);

    return !text.empty() && websocket_->Send(text);
}

void GeniusDeviceChannel::HandleText(const char* data, size_t len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Invalid JSON from Genius V2");
        return;
    }

    auto protocol = cJSON_GetObjectItemCaseSensitive(root, "protocol");
    auto type = cJSON_GetObjectItemCaseSensitive(root, "type");

    if (!cJSON_IsNumber(protocol) || protocol->valueint != 1 || !cJSON_IsString(type)) {
        ESP_LOGW(TAG, "Invalid Genius V2 envelope");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "event") == 0) {
        auto event = cJSON_GetObjectItemCaseSensitive(root, "event");
        if (cJSON_IsString(event) && strcmp(event->valuestring, "skill.catalog") == 0) {
            auto data_object = cJSON_GetObjectItemCaseSensitive(root, "data");
            auto skills = cJSON_IsObject(data_object)
                ? cJSON_GetObjectItemCaseSensitive(data_object, "skills")
                : nullptr;
            ESP_LOGI(TAG, "Skill catalog received: %d skills",
                     cJSON_IsArray(skills) ? cJSON_GetArraySize(skills) : 0);
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "command") != 0) {
        cJSON_Delete(root);
        return;
    }

    auto request_id_item = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    auto command_item = cJSON_GetObjectItemCaseSensitive(root, "command");
    auto arguments = cJSON_GetObjectItemCaseSensitive(root, "arguments");

    if (!cJSON_IsString(request_id_item) || !cJSON_IsString(command_item)) {
        ESP_LOGW(TAG, "Invalid V2 command");
        cJSON_Delete(root);
        return;
    }

    const std::string request_id = request_id_item->valuestring;
    const std::string command = command_item->valuestring;

    // Adapt Device Protocol V1 command names to the existing proven K10
    // command executor. This keeps playback/alarm hardware code untouched.
    cJSON* adapted = cJSON_CreateObject();

    if (command == "media.play_url") {
        cJSON_AddStringToObject(adapted, "action", "play_stream");
    } else if (command == "media.stop") {
        cJSON_AddStringToObject(adapted, "action", "stop_stream");
    } else if (command == "alarm_sync" || command == "alarm_cancel") {
        cJSON_AddStringToObject(adapted, "action", command.c_str());
    } else {
        SendError(request_id, "unsupported_command", "Unsupported device command");
        cJSON_Delete(adapted);
        cJSON_Delete(root);
        return;
    }

    if (cJSON_IsObject(arguments)) {
        cJSON_AddItemToObject(adapted, "payload", cJSON_Duplicate(arguments, true));
    } else {
        cJSON_AddItemToObject(adapted, "payload", cJSON_CreateObject());
    }

    auto result = DispatchGeniusCommand(adapted);
    cJSON_Delete(adapted);

    ESP_LOGI(TAG, "V2 command %s (%s): recognized=%d accepted=%d",
             request_id.c_str(), command.c_str(), result.recognized, result.accepted);

    if (result.recognized && result.accepted) {
        SendResponse(request_id, true);
    } else {
        SendError(
            request_id,
            result.recognized ? "execution_failed" : "unsupported_command",
            result.error.empty() ? "Device command rejected" : result.error
        );
    }

    cJSON_Delete(root);
}

bool GeniusDeviceChannel::SendResponse(
    const std::string& request_id,
    bool accepted
) {
    if (websocket_ == nullptr || !connected_) {
        return false;
    }

    const auto device_id = SystemInfo::GetMacAddress();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "request_id", request_id.c_str());

    cJSON* result = cJSON_AddObjectToObject(root, "result");
    cJSON_AddBoolToObject(result, "accepted", accepted);

    auto text = JsonString(root);
    cJSON_Delete(root);

    return !text.empty() && websocket_->Send(text);
}

bool GeniusDeviceChannel::SendError(
    const std::string& request_id,
    const std::string& code,
    const std::string& message
) {
    if (websocket_ == nullptr || !connected_) {
        return false;
    }

    const auto device_id = SystemInfo::GetMacAddress();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "request_id", request_id.c_str());
    cJSON_AddStringToObject(root, "code", code.c_str());
    cJSON_AddStringToObject(root, "message", message.c_str());

    auto text = JsonString(root);
    cJSON_Delete(root);

    return !text.empty() && websocket_->Send(text);
}

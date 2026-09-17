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

static const char* kUrl =
    "wss://genius.minjiai.my.id/api/device-channel";

constexpr TickType_t kInitialDelay =
    pdMS_TO_TICKS(3000);

constexpr TickType_t kReconnectDelay =
    pdMS_TO_TICKS(5000);


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
    ESP_LOGI(TAG, "Channel task started");

    // StartNetwork() is asynchronous. Give Wi-Fi/DHCP/DNS
    // a short head start before the first connection attempt.
    vTaskDelay(kInitialDelay);

    while (running_) {
        if (!connected_) {
            Connect();
        }

        vTaskDelay(kReconnectDelay);
    }

    connected_ = false;
    websocket_.reset();

    ESP_LOGI(TAG, "Channel task stopped");
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

    ESP_LOGI(TAG, "Connecting: %s", kUrl);

    auto result = websocket_->Connect(kUrl);

    if (!result) {
        ESP_LOGW(
            TAG,
            "Connect failed: %s",
            result.error().ToString().c_str()
        );

        websocket_.reset();
        return false;
    }

    connected_ = true;

    ESP_LOGI(TAG, "Connected");

    if (!SendHello()) {
        ESP_LOGW(TAG, "Failed to send hello");

        connected_ = false;
        websocket_.reset();

        return false;
    }

    ESP_LOGI(TAG, "Hello sent");

    return true;
}


bool GeniusDeviceChannel::SendHello() {
    if (websocket_ == nullptr || !connected_) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "type", "hello");

    cJSON_AddStringToObject(
        root,
        "device_id",
        SystemInfo::GetMacAddress().c_str()
    );

    cJSON_AddStringToObject(
        root,
        "firmware",
        "2.5.0"
    );

    cJSON* capabilities =
        cJSON_AddObjectToObject(
            root,
            "capabilities"
        );

    cJSON_AddBoolToObject(
        capabilities,
        "media",
        true
    );

    cJSON_AddBoolToObject(
        capabilities,
        "alarm",
        true
    );

    cJSON_AddBoolToObject(
        capabilities,
        "ews",
        true
    );

    auto text = JsonString(root);
    cJSON_Delete(root);

    if (text.empty()) {
        return false;
    }

    return websocket_->Send(text);
}


void GeniusDeviceChannel::HandleText(
    const char* data,
    size_t len
) {
    cJSON* root =
        cJSON_ParseWithLength(data, len);

    if (root == nullptr) {
        ESP_LOGW(TAG, "Invalid JSON");
        return;
    }

    auto type =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "type"
        );

    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        ESP_LOGI(
            TAG,
            "Server hello received"
        );

        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "command") != 0) {
        cJSON_Delete(root);
        return;
    }

    std::string command_id;

    auto id =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "command_id"
        );

    if (cJSON_IsString(id)) {
        command_id = id->valuestring;
    }

    auto result =
        DispatchGeniusCommand(root);

    ESP_LOGI(
        TAG,
        "Command %s: recognized=%d accepted=%d",
        command_id.c_str(),
        result.recognized,
        result.accepted
    );

    SendResult(
        command_id,
        result.recognized,
        result.accepted,
        result.error
    );

    cJSON_Delete(root);
}


bool GeniusDeviceChannel::SendResult(
    const std::string& command_id,
    bool recognized,
    bool accepted,
    const std::string& error
) {
    if (websocket_ == nullptr || !connected_) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(
        root,
        "type",
        "result"
    );

    cJSON_AddStringToObject(
        root,
        "command_id",
        command_id.c_str()
    );

    cJSON_AddBoolToObject(
        root,
        "recognized",
        recognized
    );

    cJSON_AddBoolToObject(
        root,
        "accepted",
        accepted
    );

    if (!error.empty()) {
        cJSON_AddStringToObject(
            root,
            "error",
            error.c_str()
        );
    }

    auto text = JsonString(root);
    cJSON_Delete(root);

    if (text.empty()) {
        return false;
    }

    return websocket_->Send(text);
}

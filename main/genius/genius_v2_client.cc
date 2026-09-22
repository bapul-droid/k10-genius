#include "genius_v2_client.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_app_desc.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "application.h"
#include "boards/common/board.h"
#include "command_handler.h"
#include "mcp_server.h"
#include "system_info.h"
#include "settings.h"

namespace {

static const char* TAG = "GeniusV2Client";
static const char* kDefaultDomain = "genius.minjiai.my.id";

constexpr TickType_t kInitialDelay = pdMS_TO_TICKS(3000);
constexpr TickType_t kLoopDelay = pdMS_TO_TICKS(1000);
constexpr TickType_t kHeartbeatInterval = pdMS_TO_TICKS(30000);
constexpr TickType_t kSkillTimeout = pdMS_TO_TICKS(15000);

std::string JsonString(cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    cJSON_free(text);
    return result;
}

bool IsRequired(const cJSON* required, const char* name) {
    if (!cJSON_IsArray(required)) {
        return false;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, required) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, name) == 0) {
            return true;
        }
    }
    return false;
}

Property MakeProperty(const char* name, const cJSON* schema, bool required) {
    auto type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    const char* type_name = cJSON_IsString(type) ? type->valuestring : "string";
    auto default_value = cJSON_GetObjectItemCaseSensitive(schema, "default");

    if (strcmp(type_name, "boolean") == 0) {
        if (!required) {
            return Property(name, kPropertyTypeBoolean,
                            cJSON_IsBool(default_value) ? cJSON_IsTrue(default_value) : false);
        }
        return Property(name, kPropertyTypeBoolean);
    }

    if (strcmp(type_name, "integer") == 0) {
        if (!required) {
            return Property(name, kPropertyTypeInteger,
                            cJSON_IsNumber(default_value) ? default_value->valueint : 0);
        }
        return Property(name, kPropertyTypeInteger);
    }

    if (strcmp(type_name, "number") == 0) {
        if (!required) {
            return Property(name, kPropertyTypeNumber,
                            cJSON_IsNumber(default_value) ? default_value->valuedouble : 0.0);
        }
        return Property(name, kPropertyTypeNumber);
    }

    std::string fallback;
    if (cJSON_IsString(default_value)) {
        fallback = default_value->valuestring;
    } else if (!required && strcmp(name, "location") == 0) {
        // time.now has a server-side Jakarta default but no JSON-schema default.
        fallback = "Jakarta";
    }

    Property property = required
        ? Property(name, kPropertyTypeString)
        : Property(name, kPropertyTypeString, fallback);

    auto max_length = cJSON_GetObjectItemCaseSensitive(schema, "maxLength");
    if (cJSON_IsNumber(max_length) && max_length->valueint > 0) {
        property.SetMaxLength(static_cast<size_t>(max_length->valueint));
    }
    return property;
}

}  // namespace

GeniusV2Client& GeniusV2Client::GetInstance() {
    static GeniusV2Client instance;
    return instance;
}

void GeniusV2Client::Start() {
    if (running_) {
        return;
    }

    if (catalog_ready_ == nullptr) {
        catalog_ready_ = xSemaphoreCreateBinary();
    }

    running_ = true;
    auto result = xTaskCreate(
        [](void* arg) {
            static_cast<GeniusV2Client*>(arg)->Run();
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

bool GeniusV2Client::WaitForCatalog(uint32_t timeout_ms) {
    if (catalog_registered_) {
        return true;
    }
    if (catalog_ready_ == nullptr) {
        return false;
    }
    return xSemaphoreTake(catalog_ready_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ||
           catalog_registered_;
}

void GeniusV2Client::Stop() {
    running_ = false;
    connected_ = false;
    websocket_.reset();
}

void GeniusV2Client::Run() {
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

bool GeniusV2Client::Connect() {
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

    Settings settings("genius");
    std::string domain = settings.GetString("domain");
    if (domain.empty()) {
        domain = kDefaultDomain;
    }

    const std::string url = "wss://" + domain + "/device/ws";

    ESP_LOGI(TAG, "Connecting V2: %s", url.c_str());
    auto result = websocket_->Connect(url.c_str());

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

bool GeniusV2Client::SendHello() {
    if (websocket_ == nullptr || !connected_) {
        return false;
    }

    const auto device_id = SystemInfo::GetMacAddress();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "name", BOARD_NAME);

    cJSON* metadata = cJSON_AddObjectToObject(root, "metadata");
    cJSON_AddStringToObject(metadata, "board", BOARD_TYPE);
    cJSON_AddStringToObject(metadata, "firmware", esp_app_get_description()->version);
    cJSON_AddStringToObject(metadata, "genius", "v2");

    auto text = JsonString(root);
    cJSON_Delete(root);

    return !text.empty() && websocket_->Send(text);
}

bool GeniusV2Client::SendHeartbeat() {
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

void GeniusV2Client::HandleText(const char* data, size_t len) {
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
        auto data_object = cJSON_GetObjectItemCaseSensitive(root, "data");

        if (cJSON_IsString(event) && strcmp(event->valuestring, "skill.catalog") == 0) {
            auto skills = cJSON_IsObject(data_object)
                ? cJSON_GetObjectItemCaseSensitive(data_object, "skills")
                : nullptr;
            ESP_LOGI(TAG, "Skill catalog received: %d skills",
                     cJSON_IsArray(skills) ? cJSON_GetArraySize(skills) : 0);
            RegisterSkillCatalog(data_object);
        } else if (cJSON_IsString(event) &&
                   strcmp(event->valuestring, "skill.result") == 0) {
            HandleSkillResult(data_object);
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

void GeniusV2Client::RegisterSkillCatalog(const cJSON* data) {
    if (catalog_registered_ || !cJSON_IsObject(data)) {
        return;
    }

    auto skills = cJSON_GetObjectItemCaseSensitive(data, "skills");
    if (!cJSON_IsArray(skills)) {
        return;
    }

    // Genius V2 owns the unified skill catalog and all intent routing.
    // XiaoZhi only sees one stable gateway tool regardless of how many
    // internal Genius skills exist.
    const cJSON* gateway = nullptr;
    const cJSON* skill = nullptr;

    cJSON_ArrayForEach(skill, skills) {
        auto name_item = cJSON_GetObjectItemCaseSensitive(skill, "name");
        if (cJSON_IsString(name_item) &&
            strcmp(name_item->valuestring, "genius_tool") == 0) {
            gateway = skill;
            break;
        }
    }

    if (gateway == nullptr) {
        ESP_LOGE(TAG, "Genius gateway tool not found in skill catalog");
        return;
    }

    auto description_item =
        cJSON_GetObjectItemCaseSensitive(gateway, "description");
    auto input_schema =
        cJSON_GetObjectItemCaseSensitive(gateway, "input_schema");

    if (!cJSON_IsObject(input_schema)) {
        ESP_LOGE(TAG, "Genius gateway has invalid input schema");
        return;
    }

    const std::string description = cJSON_IsString(description_item)
        ? description_item->valuestring
        : "Send a request to Genius V2";

    PropertyList properties;
    auto schema_properties =
        cJSON_GetObjectItemCaseSensitive(input_schema, "properties");
    auto required =
        cJSON_GetObjectItemCaseSensitive(input_schema, "required");

    if (cJSON_IsObject(schema_properties)) {
        const cJSON* schema = nullptr;
        cJSON_ArrayForEach(schema, schema_properties) {
            if (schema->string == nullptr || !cJSON_IsObject(schema)) {
                continue;
            }

            // device_id is injected by the Genius V2 WebSocket session.
            // It must never be supplied by the XiaoZhi model.
            if (strcmp(schema->string, "device_id") == 0) {
                continue;
            }

            properties.AddProperty(
                MakeProperty(
                    schema->string,
                    schema,
                    IsRequired(required, schema->string)
                )
            );
        }
    }

    McpServer::GetInstance().AddTool(
        "genius_tool",
        description,
        properties,
        [this](const PropertyList& arguments) -> ToolResult {
            std::string error;
            auto result = CallSkill("genius_tool", arguments, error);
            if (!error.empty()) {
                return std::unexpected(error);
            }
            return result;
        }
    );

    catalog_registered_ = true;
    ESP_LOGI(TAG, "Registered Genius gateway as XiaoZhi MCP tool");

    if (catalog_ready_ != nullptr) {
        xSemaphoreGive(catalog_ready_);
    }

    Application::GetInstance().SendMcpMessage(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}"
    );
}

bool GeniusV2Client::SendSkillCall(
    const std::string& request_id,
    const std::string& name,
    const PropertyList& properties
) {
    if (websocket_ == nullptr || !connected_) {
        return false;
    }

    const auto device_id = SystemInfo::GetMacAddress();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "device_id", device_id.c_str());
    cJSON_AddStringToObject(root, "event", "skill.call");

    cJSON* data = cJSON_AddObjectToObject(root, "data");
    cJSON_AddStringToObject(data, "request_id", request_id.c_str());
    cJSON_AddStringToObject(data, "name", name.c_str());
    cJSON* arguments = cJSON_AddObjectToObject(data, "arguments");

    for (const auto& property : properties) {
        if (property.type() == kPropertyTypeBoolean) {
            cJSON_AddBoolToObject(arguments, property.name().c_str(), property.value<bool>());
        } else if (property.type() == kPropertyTypeInteger) {
            cJSON_AddNumberToObject(arguments, property.name().c_str(), property.value<int>());
        } else if (property.type() == kPropertyTypeNumber) {
            cJSON_AddNumberToObject(arguments, property.name().c_str(), property.value<double>());
        } else if (property.type() == kPropertyTypeString) {
            cJSON_AddStringToObject(
                arguments,
                property.name().c_str(),
                property.value<std::string>().c_str()
            );
        }
    }

    auto text = JsonString(root);
    cJSON_Delete(root);

    return !text.empty() && websocket_->Send(text);
}

std::string GeniusV2Client::CallSkill(
    const std::string& name,
    const PropertyList& properties,
    std::string& error
) {
    if (!connected_) {
        error = "Genius V2 is not connected";
        return {};
    }

    auto pending = std::make_shared<PendingSkillCall>();
    pending->done = xSemaphoreCreateBinary();
    if (pending->done == nullptr) {
        error = "Unable to allocate Genius skill wait handle";
        return {};
    }

    const auto counter = ++skill_request_counter_;
    const std::string request_id =
        "genius-" + std::to_string(xTaskGetTickCount()) + "-" + std::to_string(counter);

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_skill_call_ != nullptr) {
            vSemaphoreDelete(pending->done);
            error = "Another Genius skill call is still running";
            return {};
        }
        pending_request_id_ = request_id;
        pending_skill_call_ = pending;
    }

    ESP_LOGI(TAG, "Calling Genius skill: %s (%s)", name.c_str(), request_id.c_str());

    if (!SendSkillCall(request_id, name, properties)) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_request_id_.clear();
        pending_skill_call_.reset();
        vSemaphoreDelete(pending->done);
        error = "Failed to send Genius skill call";
        return {};
    }

    const bool completed = xSemaphoreTake(pending->done, kSkillTimeout) == pdTRUE;

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_skill_call_ == pending) {
            pending_request_id_.clear();
            pending_skill_call_.reset();
        }
    }

    vSemaphoreDelete(pending->done);
    pending->done = nullptr;

    if (!completed) {
        error = "Genius skill call timed out";
        return {};
    }

    if (!pending->error.empty()) {
        error = pending->error;
        return {};
    }

    return pending->result;
}

void GeniusV2Client::HandleSkillResult(const cJSON* data) {
    if (!cJSON_IsObject(data)) {
        return;
    }

    auto request_id_item = cJSON_GetObjectItemCaseSensitive(data, "request_id");
    auto ok_item = cJSON_GetObjectItemCaseSensitive(data, "ok");

    if (!cJSON_IsString(request_id_item) || !cJSON_IsBool(ok_item)) {
        return;
    }

    std::shared_ptr<PendingSkillCall> pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_skill_call_ == nullptr ||
            pending_request_id_ != request_id_item->valuestring) {
            return;
        }
        pending = pending_skill_call_;
    }

    if (cJSON_IsTrue(ok_item)) {
        auto result = cJSON_GetObjectItemCaseSensitive(data, "result");
        if (cJSON_IsString(result)) {
            pending->result = result->valuestring;
        } else if (result != nullptr) {
            char* encoded = cJSON_PrintUnformatted(result);
            pending->result = encoded ? encoded : "";
            cJSON_free(encoded);
        } else {
            pending->result = "ok";
        }
        ESP_LOGI(TAG, "Genius skill result: %s", request_id_item->valuestring);
    } else {
        auto error_item = cJSON_GetObjectItemCaseSensitive(data, "error");
        pending->error = cJSON_IsString(error_item)
            ? error_item->valuestring
            : "Genius skill failed";
        ESP_LOGW(TAG, "Genius skill error: %s: %s",
                 request_id_item->valuestring, pending->error.c_str());
    }

    xSemaphoreGive(pending->done);
}

bool GeniusV2Client::SendResponse(
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

bool GeniusV2Client::SendError(
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

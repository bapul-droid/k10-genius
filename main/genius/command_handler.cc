#include "command_handler.h"
#include <cstring>
#include "application.h"
#include "native_services.h"

namespace {
bool Text(const cJSON* object, const char* name, size_t limit, std::string& output,
          bool required = false) {
    auto value = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!value)
        return !required;
    if (!cJSON_IsString(value) || strlen(value->valuestring) > limit ||
        (required && !*value->valuestring))
        return false;
    output = value->valuestring;
    return true;
}
}  // namespace

GeniusCommandResult DispatchGeniusCommand(const cJSON* command) {
    GeniusCommandResult result;
    std::string action;
    if (!cJSON_IsObject(command) || !Text(command, "action", 48, action, true))
        return result;
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
    if (!payload)
        payload = command;
    auto& app = Application::GetInstance();
    if (action == "play_stream") {
        result.recognized = true;
        std::string url, title = "Media", kind = "media", text, id, source;
        if (!cJSON_IsObject(payload) || !Text(payload, "audio_url", 2048, url) ||
            (url.empty() && !Text(payload, "url", 2048, url)) ||
            !Text(payload, "title", 128, title) || !Text(payload, "kind", 32, kind) ||
            !Text(payload, "text", 1024, text) || !Text(payload, "request_id", 64, id) ||
            !Text(payload, "source", 32, source)) {
            result.error = "Invalid play_stream payload";
            return result;
        }
        if (!id.empty())
            result.accepted = app.AcceptGeniusSpeech(id, url);
        else if (kind == "media")
            result.accepted = app.RequestGeniusPlayback(url, title);
        else if (kind == "alarm")
            result.accepted = app.RequestGeniusAlert("alarm", text.empty() ? title : text);
        else if (kind == "ews" && source == "BMKG" && !text.empty())
            result.accepted = app.RequestGeniusAlert("ews", text, url);
        else
            result.error = "Unsupported stream context or missing BMKG attribution";
    } else if (action == "stop_stream") {
        result.recognized = true;
        app.StopGeniusPlayback();
        result.accepted = true;
    } else if (action == "alarm_sync") {
        result.recognized = true;
        result.accepted = GeniusNativeServices::GetInstance().SetAlarm(payload, result.error);
    } else if (action == "alarm_cancel") {
        result.recognized = true;
        std::string id;
        if (Text(payload, "alarm_id", 48, id, true))
            result.accepted = GeniusNativeServices::GetInstance().CancelAlarm(id, result.error);
    }
    if (result.recognized && !result.accepted && result.error.empty())
        result.error = "Device busy or invalid command";
    return result;
}

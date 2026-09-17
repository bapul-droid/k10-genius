#include "application.h"
#include "mcp_server.h"
#include "native_services.h"
#include "portal.h"

void RegisterGeniusTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.media.play_url",
                "Play a direct MP3, AAC/ADTS, WAV/PCM or mono Ogg/Opus radio/music URL supplied by "
                "Genius. Starts after the "
                "current spoken reply drains. Genius resolves music URLs; decoding and playback "
                "run natively on the device.",
                PropertyList({Property("url", kPropertyTypeString),
                              Property("title", kPropertyTypeString, std::string("Media"))}),
                [](const PropertyList& properties) -> ReturnValue {
                    return Application::GetInstance().RequestGeniusPlayback(
                        properties["url"].value<std::string>(),
                        properties["title"].value<std::string>());
                });
    mcp.AddTool("self.media.stop", "Stop local media, alarm sound, or notification playback.",
                PropertyList(), [](const PropertyList&) -> ReturnValue {
                    Application::GetInstance().StopGeniusPlayback();
                    return true;
                });
    mcp.AddUserOnlyTool(
        "self.notification.play_sound",
        "Play the built-in notification or alarm sound without needing a network audio URL. This "
        "does not schedule an alarm.",
        PropertyList({Property("sound", kPropertyTypeString, std::string("notification"))}),
        [](const PropertyList& properties) -> ReturnValue {
            auto sound = properties["sound"].value<std::string>();
            return Application::GetInstance().RequestGeniusPlayback("builtin:" + sound, sound);
        });
    mcp.AddTool(
        "self.alarm.set",
        "Persist a native alarm. due_epoch is Unix UTC seconds; repeat_seconds0 means one-shot. "
        "Local ringing uses embedded audio, independent of Wi-Fi after clock sync.",
        PropertyList({Property("alarm_id", kPropertyTypeString),
                      Property("due_epoch", kPropertyTypeInteger, 1700000000, 2147483647),
                      Property("label", kPropertyTypeString, std::string("Alarm")),
                      Property("repeat_seconds", kPropertyTypeInteger, 0, 0, 604800)}),
        [](const PropertyList& properties) -> ToolResult {
            auto json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "alarm_id",
                                    properties["alarm_id"].value<std::string>().c_str());
            cJSON_AddStringToObject(json, "label",
                                    properties["label"].value<std::string>().c_str());
            cJSON_AddNumberToObject(json, "due_epoch", properties["due_epoch"].value<int>());
            cJSON_AddNumberToObject(json, "repeat_seconds",
                                    properties["repeat_seconds"].value<int>());
            std::string error;
            bool ok = GeniusNativeServices::GetInstance().SetAlarm(json, error);
            cJSON_Delete(json);
            if (!ok)
                return std::unexpected(error);
            return ReturnValue(true);
        });
    mcp.AddTool("self.alarm.cancel", "Delete a native alarm by alarm_id.",
                PropertyList({Property("alarm_id", kPropertyTypeString)}),
                [](const PropertyList& properties) -> ToolResult {
                    std::string error;
                    if (!GeniusNativeServices::GetInstance().CancelAlarm(
                            properties["alarm_id"].value<std::string>(), error))
                        return std::unexpected(error);
                    return ReturnValue(true);
                });
    mcp.AddTool("self.alarm.list", "List persisted native alarms and clock readiness.",
                PropertyList(), [](const PropertyList&) -> ReturnValue {
                    return cJSON_Parse(GeniusNativeServices::GetInstance().StatusJson().c_str());
                });
    mcp.AddUserOnlyTool("self.alarm.ring",
                        "Test the native embedded alarm; use self.media.stop or BOOT to stop.",
                        PropertyList(), [](const PropertyList&) -> ReturnValue {
                            return Application::GetInstance().RequestGeniusAlert("alarm", "Alarm");
                        });
    mcp.AddTool("self.ews.status",
                "Read latest official BMKG earthquake information and native EWS status.",
                PropertyList(), [](const PropertyList&) -> ReturnValue {
                    return cJSON_Parse(GeniusNativeServices::GetInstance().StatusJson().c_str());
                });
    mcp.AddUserOnlyTool("self.ews.configure", "Enable/disable native BMKG earthquake polling.",
                        PropertyList({Property("enabled", kPropertyTypeBoolean)}),
                        [](const PropertyList& properties) -> ToolResult {
                            std::string error;
                            if (!GeniusNativeServices::GetInstance().ConfigureEws(
                                    properties["enabled"].value<bool>(), error))
                                return std::unexpected(error);
                            return ReturnValue(true);
                        });
    mcp.AddTool("self.ews.check",
                "Request a fresh official BMKG check. Does not announce stale earthquakes.",
                PropertyList(), [](const PropertyList&) -> ReturnValue {
                    GeniusNativeServices::GetInstance().CheckEwsNow();
                    return true;
                });
}

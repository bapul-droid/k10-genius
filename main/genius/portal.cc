#include "portal.h"
#include <esp_log.h>
#include <esp_random.h>
#include <cstring>
#include <memory>
#include "application.h"
#include "device_state_machine.h"
#include "native_services.h"
#include "portal_page.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

namespace {
esp_err_t Reply(httpd_req_t* req, const char* status, const std::string& json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, json.data(), json.size());
}
esp_err_t Error(httpd_req_t* req, const char* status, const std::string& message) {
    auto json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message.c_str());
    char* text = cJSON_PrintUnformatted(json);
    std::string body = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(json);
    return Reply(req, status, body);
}
bool String(const cJSON* value, size_t max) {
    return cJSON_IsString(value) && strlen(value->valuestring) <= max;
}
}  // namespace

void GeniusPortal::Start() {
    if (server_)
        return;
    char token[33];
    snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
    token_ = token;
    esp_timer_create_args_t timer = {};
    timer.callback = [](void*) {
        Application::GetInstance().Schedule([]() { Application::GetInstance().Reboot(); });
    };
    timer.name = "genius_reboot";
    ESP_ERROR_CHECK(esp_timer_create(&timer, &reboot_timer_));
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.server_port = 8080;
    server_config.ctrl_port = 32770;
    server_config.max_uri_handlers = 12;
    server_config.stack_size = 8192;
    server_config.lru_purge_enable = true;
    if (httpd_start(&server_, &server_config) != ESP_OK) {
        ESP_LOGE("GeniusPortal", "Unable to start device configuration server");
        return;
    }
    for (const char* uri : {"/", "/api/config"}) {
        httpd_uri_t handler = {};
        handler.uri = uri;
        handler.method = HTTP_GET;
        handler.handler = Handle;
        handler.user_ctx = this;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &handler));
    }
    for (const char* uri :
         {"/api/validate", "/api/provision", "/api/media", "/api/alarm", "/api/ews"}) {
        httpd_uri_t handler = {};
        handler.uri = uri;
        handler.method = HTTP_POST;
        handler.handler = Handle;
        handler.user_ctx = this;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &handler));
    }
    ESP_LOGI("GeniusPortal", "Genius device portal listening on port 8080");
}

esp_err_t GeniusPortal::Handle(httpd_req_t* req) {
    return static_cast<GeniusPortal*>(req->user_ctx)->Dispatch(req);
}

esp_err_t GeniusPortal::Dispatch(httpd_req_t* req) {
    auto& app = Application::GetInstance();
    if (req->method == HTTP_GET && strcmp(req->uri, "/") == 0) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
        return httpd_resp_send(req, kGeniusPortalPage, sizeof(kGeniusPortalPage) - 1);
    }
    if (req->method == HTTP_GET) {
        cJSON* json = cJSON_CreateObject();
        cJSON_AddItemToObject(json, "config", cJSON_Parse(config_.ToJson().c_str()));
        cJSON_AddItemToObject(json, "preset", cJSON_Parse(GeniusDeviceConfig{}.ToJson().c_str()));
        cJSON_AddStringToObject(json, "token", token_.c_str());
        cJSON_AddStringToObject(json, "state",
                                DeviceStateMachine::GetStateName(app.GetDeviceState()));
        cJSON_AddBoolToObject(json, "config_mode", WifiManager::GetInstance().IsConfigMode());
        auto caps = cJSON_AddObjectToObject(json, "capabilities");
        cJSON_AddBoolToObject(caps, "media", true);
        cJSON_AddBoolToObject(caps, "alarm", true);
        cJSON_AddBoolToObject(caps, "ews", true);
        cJSON_AddStringToObject(json, "firmware", "Genius Generic 2.5.0");
        cJSON_AddItemToObject(json, "playback", cJSON_Parse(app.GeniusStatusJson().c_str()));
        cJSON_AddItemToObject(
            json, "native", cJSON_Parse(GeniusNativeServices::GetInstance().StatusJson().c_str()));
        char* text = cJSON_PrintUnformatted(json);
        std::string body = text ? text : "{}";
        cJSON_free(text);
        cJSON_Delete(json);
        return Reply(req, "200 OK", body);
    }
    char token[40] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Genius-Token", token, sizeof(token)) != ESP_OK ||
        token_ != token)
        return Error(req, "403 Forbidden",
                     "Muat ulang halaman perangkat sebelum mengirim perubahan.");
    if (req->content_len == 0 || req->content_len > 8192)
        return Error(req, "413 Content Too Large", "Ukuran permintaan tidak valid.");
    std::string body(req->content_len, '\0');
    size_t position = 0;
    while (position < body.size()) {
        int received = httpd_req_recv(req, body.data() + position, body.size() - position);
        if (received <= 0)
            return Error(req, "408 Request Timeout", "Permintaan tidak lengkap.");
        position += received;
    }
    const char* end = nullptr;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(
        cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, &end, true), cJSON_Delete);
    if (!json || !cJSON_IsObject(json.get()) || end != body.c_str() + body.size())
        return Error(req, "400 Bad Request", "JSON tidak valid.");
    if (strcmp(req->uri, "/api/alarm") == 0 || strcmp(req->uri, "/api/ews") == 0) {
        auto action = cJSON_GetObjectItemCaseSensitive(json.get(), "action");
        if (!String(action, 16))
            return Error(req, "400 Bad Request", "Aksi tidak valid.");
        auto& native = GeniusNativeServices::GetInstance();
        std::string error;
        if (strcmp(req->uri, "/api/alarm") == 0) {
            if (strcmp(action->valuestring, "set") == 0) {
                if (!native.SetAlarm(json.get(), error))
                    return Error(req, "400 Bad Request", error);
            } else if (strcmp(action->valuestring, "cancel") == 0) {
                auto id = cJSON_GetObjectItemCaseSensitive(json.get(), "alarm_id");
                if (!String(id, 48) || !native.CancelAlarm(id->valuestring, error))
                    return Error(req, "400 Bad Request",
                                 error.empty() ? "ID alarm tidak valid." : error);
            } else if (strcmp(action->valuestring, "ring") == 0) {
                auto label = cJSON_GetObjectItemCaseSensitive(json.get(), "label");
                if ((label && !String(label, 96)) ||
                    !app.RequestGeniusAlert("alarm", label ? label->valuestring : "Alarm"))
                    return Error(req, "409 Conflict", "Perangkat sibuk.");
            } else
                return Error(req, "400 Bad Request", "Aksi alarm tidak valid.");
        } else {
            if (strcmp(action->valuestring, "check") == 0)
                native.CheckEwsNow();
            else if (strcmp(action->valuestring, "configure") == 0) {
                auto enabled = cJSON_GetObjectItemCaseSensitive(json.get(), "enabled");
                if (!cJSON_IsBool(enabled) || !native.ConfigureEws(cJSON_IsTrue(enabled), error))
                    return Error(req, "400 Bad Request",
                                 error.empty() ? "Pilihan tidak valid." : error);
            } else if (strcmp(action->valuestring, "test") == 0) {
                std::unique_ptr<cJSON, decltype(&cJSON_Delete)> state(
                    cJSON_Parse(native.StatusJson().c_str()), cJSON_Delete);
                auto text = cJSON_GetObjectItemCaseSensitive(state.get(), "bmkg_text");
                auto audio = cJSON_GetObjectItemCaseSensitive(json.get(), "audio_url");
                if (!String(text, 1024) || !*text->valuestring || (audio && !String(audio, 2048)) ||
                    !app.RequestGeniusAlert("ews", text->valuestring,
                                            audio ? audio->valuestring : ""))
                    return Error(req, "409 Conflict",
                                 "Info BMKG belum tersedia atau perangkat sibuk.");
            } else
                return Error(req, "400 Bad Request", "Aksi EWS tidak valid.");
        }
        return Reply(req, "200 OK", "{\"accepted\":true}");
    }
    if (strcmp(req->uri, "/api/media") == 0) {
        auto action = cJSON_GetObjectItemCaseSensitive(json.get(), "action");
        if (!String(action, 16))
            return Error(req, "400 Bad Request", "Aksi media tidak valid.");
        if (strcmp(action->valuestring, "stop") == 0) {
            app.StopGeniusPlayback();
            return Reply(req, "202 Accepted", "{\"accepted\":true}");
        }
        auto url = cJSON_GetObjectItemCaseSensitive(json.get(), "url");
        auto title = cJSON_GetObjectItemCaseSensitive(json.get(), "title");
        if (strcmp(action->valuestring, "play") != 0 || !String(url, 2048) ||
            (title && !String(title, 128)))
            return Error(req, "400 Bad Request", "URL atau judul media tidak valid.");
        if (!app.RequestGeniusPlayback(url->valuestring, title ? title->valuestring : "Media"))
            return Error(req, "409 Conflict", "Media tidak didukung atau perangkat sedang sibuk.");
        return Reply(req, "202 Accepted", "{\"accepted\":true}");
    }
    GeniusDeviceConfig next;
    std::string error;
    if (!GeniusDeviceConfig::Parse(cJSON_GetObjectItemCaseSensitive(json.get(), "config"), next,
                                   error))
        return Error(req, "400 Bad Request", error);
    if (strcmp(req->uri, "/api/validate") == 0)
        return Reply(req, "200 OK", "{\"valid\":true}");
    return Error(req, "403 Forbidden",
                 "Provisioning GPIO ditunda ke tahap berikutnya; gunakan portal Wi-Fi upstream.");
}

#include "native_services.h"
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>
#include "application.h"
#include "board.h"
#include "http.h"

namespace {
std::string Encode(cJSON* json) {
    char* text = cJSON_PrintUnformatted(json);
    std::string value = text ? text : "{}";
    cJSON_free(text);
    cJSON_Delete(json);
    return value;
}
bool Text(const cJSON* json, const char* key, size_t max, std::string& result) {
    auto item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsString(item) || strlen(item->valuestring) > max)
        return false;
    result = item->valuestring;
    return !result.empty();
}
bool Integer(const cJSON* json, const char* key, int64_t minimum, int64_t maximum,
             int64_t& result) {
    auto item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
        std::floor(item->valuedouble) != item->valuedouble || item->valuedouble < minimum ||
        item->valuedouble > maximum)
        return false;
    result = item->valuedouble;
    return true;
}
bool Store(const char* key, const std::string& value) {
    nvs_handle_t handle;
    if (nvs_open("genius_native", NVS_READWRITE, &handle) != ESP_OK)
        return false;
    auto status = nvs_set_str(handle, key, value.c_str());
    if (status == ESP_OK)
        status = nvs_commit(handle);
    nvs_close(handle);
    return status == ESP_OK;
}
std::string Read(const char* key) {
    nvs_handle_t handle;
    if (nvs_open("genius_native", NVS_READONLY, &handle) != ESP_OK)
        return {};
    size_t size = 0;
    std::string result;
    if (nvs_get_str(handle, key, nullptr, &size) == ESP_OK && size > 0 && size <= 4096) {
        std::vector<char> bytes(size);
        if (nvs_get_str(handle, key, bytes.data(), &size) == ESP_OK)
            result = bytes.data();
    }
    nvs_close(handle);
    return result;
}
int64_t EventEpoch(const std::string& iso) {
    int y, m, d, h, minute, second;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &m, &d, &h, &minute, &second) != 6 ||
        y < 2020 || y > 2100 || h < 0 || h > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59)
        return 0;
    using namespace std::chrono;
    year_month_day date{year{y}, month{unsigned(m)}, day{unsigned(d)}};
    if (!date.ok() || !(iso.ends_with("+00:00") || iso.ends_with("Z")))
        return 0;
    return duration_cast<seconds>(
               (sys_days{date} + hours{h} + minutes{minute} + seconds{second}).time_since_epoch())
        .count();
}
}  // namespace
GeniusNativeServices& GeniusNativeServices::GetInstance() {
    static GeniusNativeServices instance;
    return instance;
}
void GeniusNativeServices::Load() {
    auto encoded = Read("alarms_v1");
    auto json = cJSON_Parse(encoded.c_str());
    if (cJSON_IsArray(json) && cJSON_GetArraySize(json) <= 8) {
        int slot = 0;
        const cJSON* item;
        cJSON_ArrayForEach (item, json) {
            Alarm alarm;
            int64_t repeat = 0;
            if (Text(item, "alarm_id", 48, alarm.id) && Text(item, "label", 96, alarm.label) &&
                Integer(item, "due_epoch", 1700000000, 4102444800LL, alarm.due) &&
                Integer(item, "repeat_seconds", 0, 604800, repeat) &&
                (repeat == 0 || repeat >= 60)) {
                alarm.repeat = repeat;
                alarms_[slot++] = std::move(alarm);
            }
        }
    }
    cJSON_Delete(json);
    seen_ = Read("ews_seen");
    ews_enabled_ = Read("ews_enabled") != "false";
}
void GeniusNativeServices::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_)
        return;
    Load();
    setenv("TZ", "WIB-7", 1);
    tzset();
    esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    clock.wait_for_sync = false;
    auto clock_status = esp_netif_sntp_init(&clock);
    if (clock_status != ESP_OK && clock_status != ESP_ERR_INVALID_STATE)
        ESP_LOGW("GeniusNative", "SNTP unavailable");
    if (xTaskCreate([](void* self) { static_cast<GeniusNativeServices*>(self)->Run(); },
                    "genius_native", 8192, this, 2, nullptr) == pdPASS)
        started_ = true;
    else
        ESP_LOGE("GeniusNative", "Native scheduler task creation failed");

}
bool GeniusNativeServices::SaveAlarms(const std::array<Alarm, 8>& alarms, std::string& error) {
    auto json = cJSON_CreateArray();
    for (const auto& alarm : alarms)
        if (!alarm.id.empty()) {
            auto item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "alarm_id", alarm.id.c_str());
            cJSON_AddStringToObject(item, "label", alarm.label.c_str());
            cJSON_AddNumberToObject(item, "due_epoch", alarm.due);
            cJSON_AddNumberToObject(item, "repeat_seconds", alarm.repeat);
            cJSON_AddItemToArray(json, item);
        }
    if (!Store("alarms_v1", Encode(json))) {
        error = "Alarm gagal disimpan.";
        return false;
    }
    return true;
}
bool GeniusNativeServices::SetAlarm(const cJSON* payload, std::string& error) {
    Alarm alarm;
    int64_t repeat = 0;
    if (!cJSON_IsObject(payload) || !Text(payload, "alarm_id", 48, alarm.id) ||
        !Integer(payload, "due_epoch", 1700000000, 4102444800LL, alarm.due)) {
        error = "alarm_id dan due_epoch tidak valid.";
        return false;
    }
    auto label = cJSON_GetObjectItemCaseSensitive(payload, "label");
    alarm.label = "Alarm";
    if (label && !Text(payload, "label", 96, alarm.label)) {
        error = "Label alarm tidak valid.";
        return false;
    }
    auto repeat_value = cJSON_GetObjectItemCaseSensitive(payload, "repeat_seconds");
    if (repeat_value &&
        (!Integer(payload, "repeat_seconds", 0, 604800, repeat) || (repeat > 0 && repeat < 60))) {
        error = "Repeat alarm minimal60 detik, maksimal7 hari.";
        return false;
    }
    alarm.repeat = repeat;
    auto now = time(nullptr);

    if (now > 1700000000 && alarm.due <= now) {
        error = "Alarm harus dijadwalkan di masa depan.";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto updated = alarms_;
    Alarm* slot = nullptr;
    for (auto& item : updated)
        if (item.id == alarm.id) {
            slot = &item;
            break;
        }
    if (!slot)
        for (auto& item : updated)
            if (item.id.empty()) {
                slot = &item;
                break;
            }
    if (!slot) {
        error = "Maksimal8 alarm.";
        return false;
    }
    *slot = std::move(alarm);
    if (!SaveAlarms(updated, error))
        return false;
    alarms_ = std::move(updated);
    return true;
}
bool GeniusNativeServices::CancelAlarm(const std::string& id, std::string& error) {
    if (id.empty() || id.size() > 48) {
        error = "alarm_id tidak valid.";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto updated = alarms_;
    for (auto& alarm : updated)
        if (alarm.id == id)
            alarm = Alarm{};
    if (!SaveAlarms(updated, error))
        return false;
    alarms_ = std::move(updated);
    return true;
}
bool GeniusNativeServices::ConfigureEws(bool enabled, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Store("ews_enabled", enabled ? "true" : "false")) {
        error = "EWS gagal disimpan.";
        return false;
    }
    ews_enabled_ = enabled;
    check_now_ = enabled;
    return true;
}
void GeniusNativeServices::CheckEwsNow() { check_now_ = true; }
std::string GeniusNativeServices::StatusJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "scheduler_running", started_);
    cJSON_AddBoolToObject(json, "clock_ready", time(nullptr) > 1700000000);
    cJSON_AddNumberToObject(json, "epoch", time(nullptr));
    cJSON_AddBoolToObject(json, "ews_enabled", ews_enabled_);
    cJSON_AddStringToObject(json, "source", "BMKG");
    cJSON_AddStringToObject(json, "bmkg_text", last_bmkg_.c_str());
    cJSON_AddStringToObject(json, "ews_error", last_error_.c_str());
    cJSON_AddNumberToObject(json, "last_check_epoch", last_check_);
    auto alarms = cJSON_AddArrayToObject(json, "alarms");
    for (const auto& alarm : alarms_)
        if (!alarm.id.empty()) {
            auto item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "alarm_id", alarm.id.c_str());
            cJSON_AddStringToObject(item, "label", alarm.label.c_str());
            cJSON_AddNumberToObject(item, "due_epoch", alarm.due);
            cJSON_AddNumberToObject(item, "repeat_seconds", alarm.repeat);
            cJSON_AddItemToArray(alarms, item);
        }
    return Encode(json);
}
void GeniusNativeServices::Run() {
    for (;;) {
        auto now = time(nullptr);
        std::vector<Alarm> fire;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto updated = alarms_;
            bool changed = false;
            if (now > 1700000000)
                for (auto& alarm : updated)
                    if (!alarm.id.empty() && alarm.due <= now) {
                        auto state = Application::GetInstance().GetDeviceState();
                        const bool ready =
                            state == kDeviceStateIdle || state == kDeviceStateListening ||
                            state == kDeviceStateSpeaking || state == kDeviceStateNotifying ||
                            state == kDeviceStateConnecting || state == kDeviceStateWifiConfiguring;
                        if (now - alarm.due <= 300 &&
                            (!ready || Application::GetInstance().GeniusPlaybackPriority() > 1))
                            continue;
                        changed = true;
                        if (now - alarm.due <= 300)
                            fire.push_back(alarm);
                        if (alarm.repeat)
                            alarm.due += ((now - alarm.due) / alarm.repeat + 1) * alarm.repeat;
                        else
                            alarm = Alarm{};
                    }
            std::string error;
            if (changed) {
                if (SaveAlarms(updated, error))
                    alarms_ = std::move(updated);
                else {
                    fire.clear();
                    last_error_ = error;
                }
            }
        }
        for (const auto& alarm : fire)
            Application::GetInstance().RequestGeniusAlert("alarm", alarm.label, "");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void GeniusNativeServices::PollBmkg() {
    auto* network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(3) : nullptr;
    std::string error, body;
    if (http) {
        http->SetTimeout(5000);
        http->SetKeepAlive(false);
        http->SetHeader("Accept", "application/json");
        if (http->Open("GET", "https://data.bmkg.go.id/DataMKG/TEWS/autogempa.json")) {
            auto status = http->GetStatusCode();
            if (status && *status == 200) {
                char chunk[512];
                while (body.size() <= 4096) {
                    auto count = http->Read(chunk, sizeof(chunk));
                    if (!count) {
                        error = "BMKG read failed";
                        break;
                    }
                    if (*count == 0)
                        break;
                    body.append(chunk, *count);
                }
                if (body.size() > 4096)
                    error = "BMKG response too large";
            } else
                error = "BMKG HTTP unavailable";
        } else
            error = "BMKG network unavailable";
        http->Close();
    } else
        error = "Network unavailable";
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_Parse(body.c_str()), cJSON_Delete);
    auto info = json ? cJSON_GetObjectItemCaseSensitive(json.get(), "Infogempa") : nullptr;
    auto gempa = info ? cJSON_GetObjectItemCaseSensitive(info, "gempa") : nullptr;
    std::string id, magnitude, depth, region, potential, date, hour;
    if (error.empty() &&
        !(Text(gempa, "DateTime", 48, id) && Text(gempa, "Magnitude", 16, magnitude) &&
          Text(gempa, "Kedalaman", 32, depth) && Text(gempa, "Wilayah", 256, region) &&
          Text(gempa, "Potensi", 256, potential) && Text(gempa, "Tanggal", 48, date) &&
          Text(gempa, "Jam", 48, hour)))
        error = "Invalid BMKG data";
    bool announce = false;
    std::string speech;
    auto now = time(nullptr);
    if (error.empty()) {
        speech = "Informasi gempa dari BMKG. Tanggal " + date + ", pukul " + hour + ". Magnitudo " +
                 magnitude + ". Kedalaman " + depth + ". Lokasi " + region + ". " + potential + ".";
        int64_t event = EventEpoch(id);
        char* end = nullptr;
        double mag = strtod(magnitude.c_str(), &end);
        if (!end || *end || !std::isfinite(mag) || mag < 0 || mag > 10 || !event)
            error = "Invalid BMKG timestamp/magnitude";
        std::lock_guard<std::mutex> lock(mutex_);
        if (error.empty()) {
            last_bmkg_ = speech;
            if (now > 1700000000 && id != seen_) {
                // Never replay stale/latest historical events on boot. Persist
                // deduplication before sounding a newly received recent event.
                announce = ews_enabled_ && now > 1700000000 && now >= event && now - event <= 600 &&
                           mag >= 5.0;
                if (Store("ews_seen", id))
                    seen_ = id;
                else {
                    announce = false;
                    error = "EWS deduplication persistence failed";
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_check_ = now;
        last_error_ = error;
    }
    if (announce)
        Application::GetInstance().RequestGeniusAlert("ews", speech, "");
}

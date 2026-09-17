#include "device_config.h"

#include <esp_log.h>
#include <nvs.h>
#include <cmath>
#include <cstdlib>
#include <set>
#include <vector>

namespace {
struct IntField {
    const char* name;
    int GeniusDeviceConfig::* member;
};
struct BoolField {
    const char* name;
    bool GeniusDeviceConfig::* member;
};
constexpr IntField integers[] = {{"mic_ws", &GeniusDeviceConfig::mic_ws},
                                 {"mic_bclk", &GeniusDeviceConfig::mic_bclk},
                                 {"mic_din", &GeniusDeviceConfig::mic_din},
                                 {"spk_bclk", &GeniusDeviceConfig::spk_bclk},
                                 {"spk_ws", &GeniusDeviceConfig::spk_ws},
                                 {"spk_dout", &GeniusDeviceConfig::spk_dout},
                                 {"boot", &GeniusDeviceConfig::boot},
                                 {"volume_up", &GeniusDeviceConfig::volume_up},
                                 {"volume_down", &GeniusDeviceConfig::volume_down},
                                 {"led", &GeniusDeviceConfig::led},
                                 {"lcd_mosi", &GeniusDeviceConfig::lcd_mosi},
                                 {"lcd_clk", &GeniusDeviceConfig::lcd_clk},
                                 {"lcd_dc", &GeniusDeviceConfig::lcd_dc},
                                 {"lcd_rst", &GeniusDeviceConfig::lcd_rst},
                                 {"lcd_cs", &GeniusDeviceConfig::lcd_cs},
                                 {"lcd_bl", &GeniusDeviceConfig::lcd_bl},
                                 {"width", &GeniusDeviceConfig::width},
                                 {"height", &GeniusDeviceConfig::height},
                                 {"offset_x", &GeniusDeviceConfig::offset_x},
                                 {"offset_y", &GeniusDeviceConfig::offset_y}};
constexpr BoolField booleans[] = {{"display", &GeniusDeviceConfig::display},
                                  {"mirror_x", &GeniusDeviceConfig::mirror_x},
                                  {"mirror_y", &GeniusDeviceConfig::mirror_y},
                                  {"swap_xy", &GeniusDeviceConfig::swap_xy},
                                  {"invert", &GeniusDeviceConfig::invert},
                                  {"bgr", &GeniusDeviceConfig::bgr},
                                  {"backlight_invert", &GeniusDeviceConfig::backlight_invert}};
}  // namespace

std::string GeniusDeviceConfig::ToJson() const {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "schema", 1);
    for (const auto& field : integers)
        cJSON_AddNumberToObject(json, field.name, this->*field.member);
    for (const auto& field : booleans)
        cJSON_AddBoolToObject(json, field.name, this->*field.member);
    char* encoded = cJSON_PrintUnformatted(json);
    std::string result = encoded ? encoded : "{}";
    cJSON_free(encoded);
    cJSON_Delete(json);
    return result;
}

bool GeniusDeviceConfig::Parse(const cJSON* json, GeniusDeviceConfig& config, std::string& error) {
    if (!cJSON_IsObject(json)) {
        error = "Konfigurasi harus berupa objek JSON.";
        return false;
    }
    // Require complete profiles: no accidental mix of new and old pins.
    auto schema = cJSON_GetObjectItemCaseSensitive(json, "schema");
    if (!cJSON_IsNumber(schema) || schema->valuedouble != 1) {
        error = "Versi konfigurasi tidak didukung.";
        return false;
    }
    std::set<std::string> names{"schema"};
    for (const auto& field : integers) {
        names.insert(field.name);
        auto value = cJSON_GetObjectItemCaseSensitive(json, field.name);
        if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
            value->valuedouble != std::floor(value->valuedouble) || value->valuedouble < -1 ||
            value->valuedouble > 480) {
            error = std::string("Nilai tidak valid: ") + field.name;
            return false;
        }
        config.*field.member = value->valueint;
    }
    for (const auto& field : booleans) {
        names.insert(field.name);
        auto value = cJSON_GetObjectItemCaseSensitive(json, field.name);
        if (!cJSON_IsBool(value)) {
            error = std::string("Pilihan tidak valid: ") + field.name;
            return false;
        }
        config.*field.member = cJSON_IsTrue(value);
    }
    std::set<std::string> seen;
    const cJSON* item;
    cJSON_ArrayForEach (item, json) {
        if (!item->string || !names.count(item->string) || !seen.insert(item->string).second) {
            error = "Ada kolom tidak dikenal atau duplikat.";
            return false;
        }
    }
    return config.Validate(error);
}

bool GeniusDeviceConfig::Validate(std::string& error) const {
    if (width < 64 || width > 320 || height < 64 || height > 480 || offset_x < 0 ||
        offset_x > 240 || offset_y < 0 || offset_y > 320) {
        error = "Ukuran atau offset LCD di luar batas.";
        return false;
    }
    if (display &&
        (width + offset_x > (swap_xy ? 320 : 240) || height + offset_y > (swap_xy ? 240 : 320))) {
        error = "Ukuran dan offset melebihi area layar yang didukung.";
        return false;
    }
    std::set<int> used;
    struct Pin {
        const char* name;
        int value;
        bool required;
    };
    std::vector<Pin> pins = {{"mic_ws", mic_ws, true},
                             {"mic_bclk", mic_bclk, true},
                             {"mic_din", mic_din, true},
                             {"spk_bclk", spk_bclk, true},
                             {"spk_ws", spk_ws, true},
                             {"spk_dout", spk_dout, true},
                             {"boot", boot, false},
                             {"volume_up", volume_up, false},
                             {"volume_down", volume_down, false},
                             {"led", led, false}};
    if (display) {
        pins.insert(pins.end(), {{"lcd_mosi", lcd_mosi, true},
                                 {"lcd_clk", lcd_clk, true},
                                 {"lcd_dc", lcd_dc, true},
                                 {"lcd_rst", lcd_rst, false},
                                 {"lcd_cs", lcd_cs, false},
                                 {"lcd_bl", lcd_bl, false}});
    }
    for (const auto& pin : pins) {
        if (pin.value == -1 && !pin.required)
            continue;
        const bool boot_zero = std::string(pin.name) == "boot" && pin.value == 0;
        const bool reset_45 = std::string(pin.name) == "lcd_rst" && pin.value == 45;
        const bool regular = pin.value >= 1 && pin.value <= 48 && pin.value != 3 &&
                             pin.value != 19 && pin.value != 20 &&
                             !(pin.value >= 22 && pin.value <= 37) && pin.value != 43 &&
                             pin.value != 44 && pin.value != 45 && pin.value != 46;
        if (!regular && !boot_zero && !reset_45) {
            error = std::string("GPIO tidak tersedia untuk ") + pin.name;
            return false;
        }
        if (!used.insert(pin.value).second) {
            error = "GPIO dipakai oleh lebih dari satu fungsi.";
            return false;
        }
    }
    return true;
}

GeniusDeviceConfig GeniusDeviceConfig::Load() {
    GeniusDeviceConfig config;
#ifdef CONFIG_GENIUS_DEFAULT_HEADLESS
    config.display = false;
#endif
    nvs_handle_t handle;
    if (nvs_open("genius_hw", NVS_READONLY, &handle) != ESP_OK)
        return config;
    size_t length = 0;
    if (nvs_get_str(handle, "profile", nullptr, &length) == ESP_OK && length > 0 &&
        length <= 4096) {
        std::vector<char> buffer(length);
        if (nvs_get_str(handle, "profile", buffer.data(), &length) == ESP_OK) {
            cJSON* json = cJSON_Parse(buffer.data());
            GeniusDeviceConfig stored;
            std::string error;
            if (Parse(json, stored, error))
                config = stored;
            else
                ESP_LOGW("GeniusConfig", "Invalid saved profile; using firmware preset");
            cJSON_Delete(json);
        }
    }
    nvs_close(handle);
    return config;
}

bool GeniusDeviceConfig::Save(std::string& error) const {
    if (!Validate(error))
        return false;
    nvs_handle_t handle;
    auto status = nvs_open("genius_hw", NVS_READWRITE, &handle);
    if (status != ESP_OK) {
        error = "Penyimpanan konfigurasi tidak tersedia.";
        return false;
    }
    status = nvs_set_str(handle, "profile", ToJson().c_str());
    if (status == ESP_OK)
        status = nvs_commit(handle);
    nvs_close(handle);
    if (status != ESP_OK) {
        error = "Konfigurasi gagal disimpan.";
        return false;
    }
    return true;
}

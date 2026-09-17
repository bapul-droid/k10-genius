#pragma once
#include <esp_http_server.h>
#include <esp_timer.h>
#include "device_config.h"

void RegisterGeniusTools();

class GeniusPortal {
public:
    explicit GeniusPortal(const GeniusDeviceConfig& config) : config_(config) {}
    void Start();

private:
    GeniusDeviceConfig config_;
    std::string token_;
    httpd_handle_t server_ = nullptr;
    esp_timer_handle_t reboot_timer_ = nullptr;
    static esp_err_t Handle(httpd_req_t* request);
    esp_err_t Dispatch(httpd_req_t* request);
};

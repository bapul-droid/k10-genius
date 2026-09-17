#pragma once
#include <cJSON.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>

class GeniusNativeServices {
public:
    static GeniusNativeServices& GetInstance();
    void Start();
    bool SetAlarm(const cJSON* payload, std::string& error);
    bool CancelAlarm(const std::string& id, std::string& error);
    std::string StatusJson();
    bool ConfigureEws(bool enabled, std::string& error);
    void CheckEwsNow();

private:
    struct Alarm {
        std::string id, label;
        int64_t due = 0;
        int repeat = 0;
    };
    std::array<Alarm, 8> alarms_;
    std::mutex mutex_;
    bool started_ = false, ews_enabled_ = true;
    std::atomic<bool> check_now_{false};
    std::string seen_, last_bmkg_, last_error_;
    int64_t last_check_ = 0;
    void Run();
    bool SaveAlarms(const std::array<Alarm, 8>& alarms, std::string& error);
    void Load();
    void PollBmkg();
};

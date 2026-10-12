#pragma once
#include "esp_lvgl_port.h"
#include "board.h"
#include "application.h"
#include "camera.h"
#include "display.h"
#include "display/lcd_display.h"
#include <lvgl.h>
#include <cstdio>
#include <atomic>
#include <esp_timer.h>
#include "esp_video.h"
#include <driver/spi_common.h>
#include <driver/sdspi_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <dirent.h>
#include <cctype>
#include <vector>
#include <string>
#include <algorithm>
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "k10_recorder.h"

inline std::atomic<bool> k10_launcher_active{false};

// Overlay-only launcher. Existing XiaoZhi camera/audio drivers remain untouched.
class K10Launcher {
    enum class Page { Home, Recorder, Camera, Tools };
    Page page_ = Page::Home;
    lv_obj_t* panel_ = nullptr;
    lv_obj_t* label_ = nullptr;
    unsigned index_ = 0;
    std::atomic<bool> photo_mode_{false};
    std::atomic<bool> photo_frozen_{false};
    std::atomic<bool> capture_pending_{false};
    esp_timer_handle_t camera_timer_ = nullptr;
    sdmmc_card_t* sd_card_ = nullptr;
    bool sd_bus_ready_ = false;
    unsigned photo_sequence_ = 0;
    std::atomic<bool> gallery_mode_{false};
    std::vector<std::string> gallery_files_;
    size_t gallery_index_ = 0;
    K10Recorder recorder_app_;
    void CameraStatus(const char* text) {
        auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
        if (lcd) lcd->SetK10CameraStatus(text);
    }
    bool MountSd() {
        if (sd_card_) return true;
        constexpr spi_host_device_t host = SPI2_HOST; // dedicated SD bus (DFRobot official pins); LCD SPI3 untouched
        spi_bus_config_t bus = {};
        bus.mosi_io_num = GPIO_NUM_42;
        bus.miso_io_num = GPIO_NUM_41;
        bus.sclk_io_num = GPIO_NUM_44;
        bus.quadwp_io_num = GPIO_NUM_NC;
        bus.quadhd_io_num = GPIO_NUM_NC;
        bus.max_transfer_sz = 4096;
        esp_err_t err = spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE("K10Camera", "SD bus: %s (0x%x)", esp_err_to_name(err), unsigned(err));
            char sd_error[72];
            snprintf(sd_error, sizeof(sd_error), "SPI: %s (0x%x)", esp_err_to_name(err), unsigned(err));
            CameraStatus(sd_error);
            return false;
        }
        sd_bus_ready_ = (err == ESP_OK);
        ESP_LOGI("K10Camera", "SD independent SPI2 diagnostic");
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.host_id = host;
        slot.gpio_cs = GPIO_NUM_40;
        esp_vfs_fat_sdmmc_mount_config_t cfg = {};
        cfg.format_if_mount_failed = false;
        cfg.max_files = 5;
        cfg.allocation_unit_size = 16 * 1024;
        sdmmc_host_t sdhost = SDSPI_HOST_DEFAULT();
        sdhost.slot = host;
        sdhost.max_freq_khz = 1000; // conservative SPI diagnostic clock
        ESP_LOGI("K10Camera", "SD probe SPI2 CS40 MOSI42 MISO41 SCLK44 1000kHz");
        err = esp_vfs_fat_sdspi_mount("/sdcard", &sdhost, &slot, &cfg, &sd_card_);
        if (err != ESP_OK) {
            ESP_LOGE("K10Camera", "SD mount: %s (0x%x)", esp_err_to_name(err), unsigned(err));
            char sd_error[72];
            snprintf(sd_error, sizeof(sd_error), "SD: %s (0x%x)", esp_err_to_name(err), unsigned(err));
            CameraStatus(sd_error);
            if (sd_bus_ready_) spi_bus_free(host);
            sd_bus_ready_ = false;
            return false;
        }
        mkdir("/sdcard/DCIM", 0775);
        ESP_LOGI("K10Camera", "SD mounted");
        CameraStatus("SD READY");
        return true;
    }
    void UnmountSd() {
        if (sd_card_) {
            esp_vfs_fat_sdcard_unmount("/sdcard", sd_card_);
            sd_card_ = nullptr;
        }
        if (sd_bus_ready_) { spi_bus_free(SPI2_HOST); sd_bus_ready_ = false; }
    }
    void EnsurePhotoSequence() {
        if (photo_sequence_ != 0 || !sd_card_) return;
        DIR* d = opendir("/sdcard/DCIM");
        if (!d) return;
        unsigned max_seq = 0;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            unsigned n = 0;
            if (sscanf(e->d_name, "K10_%6u.jpg", &n) == 1 ||
                sscanf(e->d_name, "K10_%6u.JPG", &n) == 1) {
                if (n > max_seq) max_seq = n;
            }
        }
        closedir(d);
        photo_sequence_ = max_seq;
    }
    void SavePhoto() {
        Application::GetInstance().Schedule([this]() {
            if (!photo_mode_.load() || !photo_frozen_.load()) return;
            if (!sd_card_) {
                CameraStatus("SD NOT MOUNTED");
                return;
            }
            EnsurePhotoSequence();
            char path[96];
            snprintf(path, sizeof(path), "/sdcard/DCIM/K10_%06u.jpg", ++photo_sequence_);
            auto* camera = static_cast<EspVideo*>(Board::GetInstance().GetCamera());
            bool ok = camera && camera->SaveK10Jpeg(path);
            CameraStatus(ok ? "JPEG SAVED" : "JPEG SAVE FAILED");
        });
    }
    void StartLive() {
        photo_frozen_.store(false);
        if (!camera_timer_) {
            esp_timer_create_args_t args = {};
            args.callback = [](void* arg) {
                auto* self = static_cast<K10Launcher*>(arg);
                if (self->photo_mode_.load() && !self->photo_frozen_.load()) self->TakePhoto();
            };
            args.arg = this;
            args.name = "k10_live_cam";
            if (esp_timer_create(&args, &camera_timer_) != ESP_OK) return;
        }
        esp_timer_stop(camera_timer_);
        esp_timer_start_periodic(camera_timer_, 150000);
        TakePhoto();
    }
    void TakePhoto() {
        if (capture_pending_.exchange(true)) return;
        Application::GetInstance().Schedule([this]() {
            if (photo_mode_.load() && !photo_frozen_.load()) {
                auto* camera = Board::GetInstance().GetCamera();
                if (!camera || !camera->Capture()) {
                    Board::GetInstance().GetDisplay()->ShowNotification("Camera capture failed");
                }
            }
            capture_pending_.store(false);
        });
    }
    static constexpr const char* home_[5] = {"XiaoZhi", "Recorder", "Camera", "Tools", "Exit"};
    static constexpr const char* recorder_[3] = {"Start Recorder", "Rec Gallery", "Back"};
    static constexpr const char* camera_[3] = {"Take photo", "Gallery", "Back"};
    static constexpr const char* tools_[3] = {"System info", "About", "Back"};
    unsigned Count() const { return page_ == Page::Home ? 5 : page_ == Page::Tools ? 3 : page_ == Page::Camera ? 3 : page_ == Page::Recorder ? 3 : 2; }
    const char* Name(unsigned i) const {
        switch (page_) {
            case Page::Recorder: return recorder_[i];
            case Page::Camera: return camera_[i];
            case Page::Tools: return tools_[i];
            default: return home_[i];
        }
    }
    const char* Title() const {
        switch (page_) {
            case Page::Recorder: return "RECORDER";
            case Page::Camera: return "CAMERA";
            case Page::Tools: return "TOOLS";
            default: return "K10 APPS";
        }
    }
    void Render(const char* note = nullptr) {
        if (!label_) return;
        char buf[320];
        int used = snprintf(buf, sizeof(buf), "%s\n\n", Title());
        for (unsigned i = 0; i < Count() && used < (int)sizeof(buf); ++i) {
            used += snprintf(buf + used, sizeof(buf) - used, "%s %s\n", index_ == i ? ">" : " ", Name(i));
        }
        if (note && used < (int)sizeof(buf)) snprintf(buf + used, sizeof(buf) - used, "\n%s", note);
        lv_label_set_text(label_, buf);
    }
public:
    // Read-only MCP SD card probe. Reuses the existing mount and never unmounts an active app.
    std::string ListRecordingsForMcp() {
        if (recorder_app_.IsActive() || photo_mode_.load() || gallery_mode_.load()) {
            return "SD card busy: close Recorder or Camera first";
        }
        const bool already_mounted = sd_card_ != nullptr;
        if (!MountSd()) return "SD card mount failed";
        DIR* dir = opendir("/sdcard/RECORD");
        if (!dir) {
            if (!already_mounted) UnmountSd();
            return "No RECORD folder or recordings found";
        }
        std::vector<std::string> files;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name.size() >= 4 && name.substr(name.size() - 4) == ".wav") {
                files.push_back(name);
            }
        }
        closedir(dir);
        std::sort(files.begin(), files.end());
        if (!already_mounted) UnmountSd();
        std::string result = "Recordings (" + std::to_string(files.size()) + "):";
        for (size_t i = 0; i < files.size() && i < 20; ++i) result += " " + files[i];
        if (files.size() > 20) result += " ...";
        return result;
    }
    bool IsOpen() const { return panel_ != nullptr; }
    bool IsPhotoMode() const { return photo_mode_.load(); }
    void ExitPhoto() {
        photo_mode_.store(false);
        gallery_mode_.store(false);
        gallery_files_.clear();
        if (camera_timer_) esp_timer_stop(camera_timer_);
        if (auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay())) lcd->SetK10CameraMode(false);
        UnmountSd();
        k10_launcher_active.store(false);
    }
    // ---- Recorder: standalone audio recorder ----
    bool IsRecorderMode() const { return recorder_app_.IsActive(); }
    void EnterRecorder() {
        Close();
        k10_launcher_active.store(true);
        // Recorder takes over LCD status line; reuse camera overlay for fullscreen status.
        if (auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay())) lcd->SetK10CameraMode(true);
        recorder_app_.Enter([this]() { return MountSd(); }, [this]() { UnmountSd(); });
    }
    void ExitRecorder() {
        recorder_app_.Exit();
        if (auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay())) lcd->SetK10CameraMode(false);
        k10_launcher_active.store(false);
    }
    void RecorderToggleRecord() { recorder_app_.ToggleRecord(); }
    void RecorderTogglePlayback() { recorder_app_.TogglePlayback(); }
    // ---- Rec Gallery passthrough ----
    bool IsRecGalleryMode() const { return recorder_app_.IsGalleryMode(); }
    void EnterRecGallery() {
        Close();
        k10_launcher_active.store(true);
        if (auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay())) lcd->SetK10CameraMode(true);
        recorder_app_.EnterGallery([this]() { return MountSd(); }, [this]() { UnmountSd(); });
    }
    void ExitRecGallery() {
        recorder_app_.ExitGallery();
        if (auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay())) lcd->SetK10CameraMode(false);
        k10_launcher_active.store(false);
    }
    void RecGalleryNext() { recorder_app_.GalleryNext(); }
    void RecGalleryPrev() { recorder_app_.GalleryPrev(); }
    void RecGalleryTogglePlay() { recorder_app_.GalleryTogglePlay(); }
    void RetakePhoto() { if (IsPhotoMode()) StartLive(); }
    void FreezePhoto() {
        if (!IsPhotoMode()) return;
        photo_frozen_.store(true);
        if (camera_timer_) esp_timer_stop(camera_timer_);
        SavePhoto();
    }
    // ---- Gallery: view JPEG photos stored in /sdcard/DCIM ----
    void EnterGallery() {
        photo_mode_.store(true);
        gallery_mode_.store(true);
        k10_launcher_active.store(true);
        if (auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay())) lcd->SetK10CameraMode(true);
        if (!MountSd()) return;
        GalleryRescan();
        GalleryShow(0);
    }
    bool IsGalleryMode() const { return gallery_mode_.load(); }
    void GalleryRescan() {
        gallery_files_.clear();
        gallery_index_ = 0;
        DIR* d = opendir("/sdcard/DCIM");
        if (!d) { CameraStatus("NO DCIM DIR"); return; }
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string low = e->d_name;
            for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
            bool jpg = low.size() >= 4 && low.compare(low.size() - 4, 4, ".jpg") == 0;
            bool jpeg = low.size() >= 5 && low.compare(low.size() - 5, 5, ".jpeg") == 0;
            if (jpg || jpeg) gallery_files_.push_back(e->d_name);
        }
        closedir(d);
        std::sort(gallery_files_.begin(), gallery_files_.end());
        if (gallery_files_.empty()) CameraStatus("NO PHOTOS");
    }
    void GalleryShow(size_t i) {
        if (!IsGalleryMode() || gallery_files_.empty()) return;
        size_t idx = i % gallery_files_.size();
        gallery_index_ = idx;
        Application::GetInstance().Schedule([this, idx]() {
            if (!gallery_mode_.load() || idx >= gallery_files_.size()) return;
            std::string path = "/sdcard/DCIM/" + gallery_files_[idx];
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) { CameraStatus("READ FAILED"); return; }
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (len <= 0 || len > 4 * 1024 * 1024) { fclose(f); CameraStatus("BAD FILE"); return; }
            uint8_t* jpg = (uint8_t*)heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!jpg) { fclose(f); CameraStatus("NO MEM"); return; }
            size_t got = fread(jpg, 1, (size_t)len, f);
            fclose(f);
            if (got != (size_t)len) { heap_caps_free(jpg); CameraStatus("READ FAILED"); return; }
            uint8_t* rgb = nullptr;
            size_t rgb_len = 0, w = 0, h = 0, stride = 0;
            esp_err_t err = jpeg_to_image(jpg, (size_t)len, &rgb, &rgb_len, &w, &h, &stride);
            heap_caps_free(jpg);
            if (err != ESP_OK || !rgb) { CameraStatus("DECODE FAILED"); return; }
            auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
            if (lcd) {
                lcd->SetPreviewImage(std::make_unique<LvglAllocatedImage>(
                    rgb, rgb_len, (int)w, (int)h, (int)stride, LV_COLOR_FORMAT_RGB565));
                char s[96];
                snprintf(s, sizeof(s), "%u/%u %s", (unsigned)(idx + 1),
                         (unsigned)gallery_files_.size(), gallery_files_[idx].c_str());
                CameraStatus(s);
            } else {
                heap_caps_free(rgb);
            }
        });
    }
    void GalleryNext() {
        if (IsGalleryMode() && !gallery_files_.empty()) GalleryShow(gallery_index_ + 1);
    }
    void GalleryPrev() {
        if (!IsGalleryMode() || gallery_files_.empty()) return;
        GalleryShow(gallery_index_ == 0 ? gallery_files_.size() - 1 : gallery_index_ - 1);
    }
    void Open() {
        if (IsOpen() || !lvgl_port_lock(1000)) return;
        panel_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(panel_, lv_pct(100), lv_pct(100));
        lv_obj_center(panel_);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(0x152238), 0);
        lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(panel_, 0, 0);
        label_ = lv_label_create(panel_);
        lv_obj_set_style_text_color(label_, lv_color_white(), 0);
        lv_obj_center(label_);
        page_ = Page::Home;
        index_ = 0;
        k10_launcher_active.store(true);
        Board::GetInstance().SetScreenBrightness(100);
        Render();
        lvgl_port_unlock();
    }
    void Next() {
        if (!IsOpen() || !lvgl_port_lock(1000)) return;
        index_ = (index_ + 1) % Count();
        Render();
        lvgl_port_unlock();
    }
    void Select() {
        if (!IsOpen()) return;
        if (page_ == Page::Home && (index_ == 0 || index_ == 4)) { Close(); return; }
        if (page_ == Page::Camera && index_ == 0) {
            Close();
            photo_mode_.store(true);
            k10_launcher_active.store(true);
            if (auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay())) lcd->SetK10CameraMode(true);
            MountSd();
            StartLive();
            return;
        }
        if (page_ == Page::Camera && index_ == 1) {
            Close();
            EnterGallery();
            return;
        }
        if (page_ == Page::Recorder && index_ == 0) {
            EnterRecorder();
            return;
        }
        if (page_ == Page::Recorder && index_ == 1) {
            EnterRecGallery();
            return;
        }
        if (!lvgl_port_lock(1000)) return;
        if (page_ == Page::Home) {
            page_ = index_ == 1 ? Page::Recorder : index_ == 2 ? Page::Camera : Page::Tools;
            index_ = 0;
            Render();
        } else if (index_ == Count() - 1) {
            page_ = Page::Home;
            index_ = 0;
            Render();
        } else if (page_ == Page::Recorder) {
            Render("Audio capture not enabled yet");
        } else if (page_ == Page::Camera) {
            Render("XiaoZhi vision stays active");
        } else {
            Render(index_ == 0 ? "K10 / ESP32-S3 / XiaoZhi" : "K10 app launcher v0.2");
        }
        lvgl_port_unlock();
    }
    void Close() {
        if (!IsOpen() || !lvgl_port_lock(1000)) return;
        lv_obj_delete(panel_);
        panel_ = nullptr;
        label_ = nullptr;
        k10_launcher_active.store(false);
        page_ = Page::Home;
        lvgl_port_unlock();
    }
};

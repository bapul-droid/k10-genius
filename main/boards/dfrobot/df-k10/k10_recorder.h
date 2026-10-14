#pragma once
// K10 Recorder v1 - standalone audio recorder app.
// - Records WAV PCM mono 16-bit 16kHz to /sdcard/RECORD/REC_%06u.wav
// - Uses AudioService::ReadAudioData (safe shared mic tap, no I2S conflict)
// - Playback via AudioService::PushPcmToPlaybackQueue (safe shared speaker path)
// - Disables wake word while active; restores on exit.
// Does NOT touch Camera App, Gallery, or camera tuning.

#include "board.h"
#include "application.h"
#include "display/lcd_display.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <dirent.h>
#include <algorithm>
#include <sys/stat.h>

static const char* REC_TAG = "K10Recorder";

class K10Recorder {
public:
    enum class State { Idle, Recording, Playing };

    K10Recorder() = default;
    ~K10Recorder() { Exit(); }

    bool IsActive() const { return active_.load(); }
    State GetState() const { return state_.load(); }

    // ---- Lifecycle (called from K10Launcher) ----
    // mount_sd_fn / unmount_sd_fn: reuse K10Launcher's SD SPI2 mount.
    bool Enter(std::function<bool()> mount_sd_fn, std::function<void()> unmount_sd_fn) {
        if (active_.load()) return true;
        mount_sd_ = mount_sd_fn;
        unmount_sd_ = unmount_sd_fn;
        if (!mount_sd_ || !mount_sd_()) {
            ShowStatus("SD MOUNT FAILED");
            return false;
        }
        EnsureRecordSequence();
        // Suspend Alexa voice trigger while Recorder owns the UI.
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
        active_.store(true);
        state_.store(State::Idle);
        UpdateUi();
        ESP_LOGI(REC_TAG, "Recorder entered, wake word disabled");
        return true;
    }

    void Exit() {
        if (!active_.load()) return;
        StopRecord();   // also finalizes WAV header
        StopPlayback();
        active_.store(false);
        state_.store(State::Idle);
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
        if (unmount_sd_) unmount_sd_();
        ESP_LOGI(REC_TAG, "Recorder exited, wake word restored");
    }

    // ---- Button actions ----
    // A: toggle record. Ignored while playing.
    void ToggleRecord() {
        if (!active_.load()) return;
        if (state_.load() == State::Playing) return;  // prevent conflict
        if (state_.load() == State::Recording) StopRecord();
        else StartRecord();
    }
    // B: toggle playback of last recording. Ignored while recording.
    void TogglePlayback() {
        if (!active_.load()) return;
        if (state_.load() == State::Recording) return;  // prevent conflict
        if (state_.load() == State::Playing) StopPlayback();
        else StartPlayback();
    }

    // ---- Gallery: browse and play saved recordings ----
    bool IsGalleryMode() const { return gallery_active_.load(); }

    bool EnterGallery(std::function<bool()> mount_sd_fn, std::function<void()> unmount_sd_fn) {
        if (gallery_active_.load()) return true;
        mount_sd_ = mount_sd_fn;
        unmount_sd_ = unmount_sd_fn;
        if (!mount_sd_ || !mount_sd_()) {
            ShowStatus("SD MOUNT FAILED");
            return false;
        }
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
        gallery_active_.store(true);
        active_.store(true);  // gallery shares active flag for button routing
        state_.store(State::Idle);
        GalleryRescan();
        GalleryShow(0);
        ESP_LOGI(REC_TAG, "Rec gallery entered");
        return true;
    }

    void ExitGallery() {
        if (!gallery_active_.load()) return;
        StopPlayback();
        gallery_active_.store(false);
        gallery_files_.clear();
        active_.store(false);
        state_.store(State::Idle);
        Application::GetInstance().GetAudioService().EnableWakeWordDetection(true);
        if (unmount_sd_) unmount_sd_();
        ESP_LOGI(REC_TAG, "Rec gallery exited");
    }

    void GalleryNext() {
        if (!gallery_active_.load() || gallery_files_.empty()) return;
        if (state_.load() == State::Playing) StopPlayback();
        GalleryShow(gallery_index_ + 1);
    }

    void GalleryPrev() {
        if (!gallery_active_.load() || gallery_files_.empty()) return;
        if (state_.load() == State::Playing) StopPlayback();
        GalleryShow(gallery_index_ == 0 ? gallery_files_.size() - 1 : gallery_index_ - 1);
    }

    void GalleryTogglePlay() {
        if (!gallery_active_.load() || gallery_files_.empty()) return;
        if (state_.load() == State::Playing) {
            StopPlayback();
            GalleryShow(gallery_index_);  // refresh to READY
        } else {
            // Play selected file
            std::string path = "/sdcard/RECORD/" + gallery_files_[gallery_index_];
            last_path_ = path;
            StartPlayback();
            // Update UI to show PLAYING with file info
            GalleryShow(gallery_index_);
        }
    }

    bool MountSdIfNeeded() {
        if (mount_sd_) return mount_sd_();
        return false;
    }

    std::string FindLatestRecording() {
        unsigned max_n = 0;
        std::string latest;
        DIR* d = opendir("/sdcard/RECORD");
        if (!d) return latest;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            unsigned n = 0;
            char extra = 0;
            if (sscanf(e->d_name, "REC_%06u.wav%c", &n, &extra) == 1 && n > max_n) {
                max_n = n;
                latest = e->d_name;
            }
        }
        closedir(d);
        return latest;
    }

    // ---- Delete: two-phase confirmed deletion via MCP ----
    // Phase 1: DeleteRequest returns file info + token (does NOT delete).
    // Phase 2: DeleteConfirm validates token + safety checks, then deletes.
    // Token binds confirmation to exact file state (name+size), preventing
    // TOCTOU issues if file changes between request and confirm.
    struct DeleteInfo {
        bool valid = false;
        std::string filename;
        long size_bytes = 0;
        long duration_sec = 0;
        std::string token;
        std::string error;
    };

    // Validate filename: must be REC_%06u.wav (prevents path traversal)
    static bool IsValidRecFilename(const std::string& name) {
        // "REC_000001.wav" = 4 + 6 + 4 = 14 chars
        if (name.size() != 14) return false;
        unsigned n = 0;
        // Strict: sscanf must consume entire string
        char extra = 0;
        if (sscanf(name.c_str(), "REC_%06u.wav%c", &n, &extra) != 1) return false;
        return true;
    }

    // NOTE: Caller must ensure SD card is mounted before calling DeleteRequest.
    // See K10Launcher::RecorderDeleteRequest for the mount/unmount wrapper.
    DeleteInfo DeleteRequest(const std::string& target) {
        DeleteInfo info;
        std::string filename = target;
        // Resolve "latest" to actual filename
        if (target == "latest") {
            filename = FindLatestRecording();
            if (filename.empty()) {
                info.error = "No recordings found";
                return info;
            }
        } else if (target == "all") {
            return DeleteRequestAll();
        }
        if (!IsValidRecFilename(filename)) {
            info.error = "Invalid filename (must be REC_XXXXXX.wav)";
            return info;
        }
        std::string path = "/sdcard/RECORD/" + filename;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            info.error = "File not found: " + filename;
            return info;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fclose(f);
        // Safety: refuse if file is currently being recorded or played
        if (state_.load() == State::Recording && rec_path_ == path) {
            info.error = "Cannot delete: file is currently being recorded";
            return info;
        }
        if (state_.load() == State::Playing && last_path_ == path) {
            info.error = "Cannot delete: file is currently playing";
            return info;
        }
        info.valid = true;
        info.filename = filename;
        info.size_bytes = len;
        info.duration_sec = len > 44 ? (len - 44) / (kSampleRate * kChannels * kBitsPerSample / 8) : 0;
        // Token: simple hash of filename + size (binds to exact file state)
        char tok[32];
        unsigned h = 0;
        for (char c : filename) h = h * 31 + (unsigned char)c;
        h ^= (unsigned)len;
        snprintf(tok, sizeof(tok), "%08X", h);
        info.token = tok;
        return info;
    }

    DeleteInfo DeleteRequestAll() {
        DeleteInfo info;
        // Count files and total size for confirmation
        DIR* d = opendir("/sdcard/RECORD");
        if (!d) {
            info.error = "Cannot open RECORD directory";
            return info;
        }
        int count = 0;
        long total = 0;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (!IsValidRecFilename(name)) continue;
            std::string path = "/sdcard/RECORD/" + name;
            // Skip active files in count
            if (state_.load() == State::Recording && rec_path_ == path) continue;
            if (state_.load() == State::Playing && last_path_ == path) continue;
            FILE* f = fopen(path.c_str(), "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                total += ftell(f);
                fclose(f);
                count++;
            }
        }
        closedir(d);
        if (count == 0) {
            info.error = "No recordings to delete";
            return info;
        }
        info.valid = true;
        info.filename = "all";
        info.size_bytes = total;
        info.duration_sec = count;  // reuse: count stored in duration_sec for "all"
        char tok[32];
        snprintf(tok, sizeof(tok), "ALL%05d", count * 7919 + (int)(total % 100000));
        info.token = tok;
        return info;
    }

    std::string DeleteConfirm(const std::string& target, const std::string& token) {
        if (target == "all") {
            return DeleteConfirmAll(token);
        }
        std::string filename = target;
        if (target == "latest") {
            filename = FindLatestRecording();
            if (filename.empty()) return "No recordings found";
        }
        if (!IsValidRecFilename(filename)) return "Invalid filename";
        // Re-validate via DeleteRequest to get fresh token
        DeleteInfo info = DeleteRequest(filename);
        if (!info.valid) return info.error;
        if (info.token != token) {
            return "Confirmation expired: file changed since request. Please request deletion again.";
        }
        std::string path = "/sdcard/RECORD/" + filename;
        // Final safety check (redundant but critical)
        if (state_.load() == State::Recording && rec_path_ == path) {
            return "Cannot delete: file is currently being recorded";
        }
        if (state_.load() == State::Playing && last_path_ == path) {
            return "Cannot delete: file is currently playing";
        }
        if (remove(path.c_str()) != 0) {
            return "Delete failed: " + filename;
        }
        ESP_LOGI(REC_TAG, "Deleted recording: %s", filename.c_str());
        // Clear last_path_ if it was the deleted file
        if (last_path_ == path) last_path_.clear();
        // Refresh gallery if active
        if (gallery_active_.load()) {
            GalleryRescan();
            if (!gallery_files_.empty()) {
                GalleryShow(gallery_index_ < gallery_files_.size() ? gallery_index_ : 0);
            } else {
                ShowStatus("NO RECORDINGS");
            }
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Deleted %s", filename.c_str());
        return std::string(msg);
    }

    std::string DeleteConfirmAll(const std::string& token) {
        DeleteInfo info = DeleteRequestAll();
        if (!info.valid) return info.error;
        if (info.token != token) {
            return "Confirmation expired: recordings changed since request. Please request again.";
        }
        int deleted = 0, skipped = 0;
        DIR* d = opendir("/sdcard/RECORD");
        if (!d) return "Cannot open RECORD directory";
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (!IsValidRecFilename(name)) continue;
            std::string path = "/sdcard/RECORD/" + name;
            if (state_.load() == State::Recording && rec_path_ == path) { skipped++; continue; }
            if (state_.load() == State::Playing && last_path_ == path) { skipped++; continue; }
            if (remove(path.c_str()) == 0) deleted++;
            else skipped++;
        }
        closedir(d);
        last_path_.clear();
        if (gallery_active_.load()) {
            GalleryRescan();
            ShowStatus(gallery_files_.empty() ? "NO RECORDINGS" : "GALLERY UPDATED");
            if (!gallery_files_.empty()) GalleryShow(0);
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "Deleted %d recordings%s", deleted,
                 skipped > 0 ? " (skipped active files)" : "");
        ESP_LOGI(REC_TAG, "%s", msg);
        return std::string(msg);
    }

private:
    std::atomic<bool> active_{false};
    std::atomic<State> state_{State::Idle};
    std::function<bool()> mount_sd_;
    std::function<void()> unmount_sd_;

    // Recording
    FILE* rec_file_ = nullptr;
    std::string rec_path_;
    uint32_t rec_data_bytes_ = 0;
    uint32_t rec_start_ms_ = 0;
    TaskHandle_t rec_task_ = nullptr;
    std::atomic<bool> rec_stop_{false};
    unsigned rec_sequence_ = 0;

    // Playback
    TaskHandle_t play_task_ = nullptr;
    std::atomic<bool> play_stop_{false};
    std::string last_path_;  // last saved recording

    // Gallery
    std::atomic<bool> gallery_active_{false};
    std::vector<std::string> gallery_files_;
    size_t gallery_index_ = 0;

    void GalleryRescan() {
        gallery_files_.clear();
        gallery_index_ = 0;
        DIR* d = opendir("/sdcard/RECORD");
        if (!d) { ShowStatus("NO RECORD DIR"); return; }
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            std::string low = name;
            for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
            if (low.size() >= 4 && low.compare(low.size() - 4, 4, ".wav") == 0) {
                gallery_files_.push_back(name);
            }
        }
        closedir(d);
        std::sort(gallery_files_.begin(), gallery_files_.end());
        if (gallery_files_.empty()) ShowStatus("NO RECORDINGS");
    }

    void GalleryShow(size_t i) {
        if (!gallery_active_.load() || gallery_files_.empty()) return;
        size_t idx = i % gallery_files_.size();
        gallery_index_ = idx;
        std::string path = "/sdcard/RECORD/" + gallery_files_[idx];
        // Get duration from file size
        long dur_sec = 0;
        FILE* f = fopen(path.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fclose(f);
            if (len > 44) dur_sec = (len - 44) / (kSampleRate * kChannels * kBitsPerSample / 8);
        }
        char buf[128];
        const char* st = (state_.load() == State::Playing) ? "PLAYING" : "READY";
        snprintf(buf, sizeof(buf), "%s\n%u/%u %s\n%ld:%02ld",
                 st, (unsigned)(idx + 1), (unsigned)gallery_files_.size(),
                 gallery_files_[idx].c_str(), dur_sec / 60, dur_sec % 60);
        ShowStatus(buf);
    }

    static constexpr int kSampleRate = 24000;  // Must match AUDIO_INPUT/OUTPUT_SAMPLE_RATE (24kHz)
    static constexpr int kChannels = 1;
    static constexpr int kBitsPerSample = 16;
    static constexpr int kChunkSamples = 1600;  // 100ms @16kHz

    void ShowStatus(const char* text) {
        auto* lcd = static_cast<LcdDisplay*>(Board::GetInstance().GetDisplay());
        if (lcd) lcd->SetK10CameraStatus(text);
    }

    void UpdateUi() {
        char buf[64];
        State s = state_.load();
        if (s == State::Recording) {
            uint32_t secs = (xTaskGetTickCount() * portTICK_PERIOD_MS - rec_start_ms_) / 1000;
            snprintf(buf, sizeof(buf), "RECORDING %02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
        } else if (s == State::Playing) {
            snprintf(buf, sizeof(buf), "PLAYING");
        } else {
            if (!last_path_.empty()) {
                const char* base = strrchr(last_path_.c_str(), '/');
                snprintf(buf, sizeof(buf), "READY %s", base ? base + 1 : last_path_.c_str());
            } else {
                snprintf(buf, sizeof(buf), "READY");
            }
        }
        ShowStatus(buf);
    }

    void EnsureRecordSequence() {
        rec_sequence_ = 0;
        mkdir("/sdcard/RECORD", 0777);
        DIR* d = opendir("/sdcard/RECORD");
        if (!d) return;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            unsigned n = 0;
            if (sscanf(e->d_name, "REC_%06u.wav", &n) == 1 && n > rec_sequence_) {
                rec_sequence_ = n;
            }
        }
        closedir(d);
        ESP_LOGI(REC_TAG, "Record sequence starts at %u", rec_sequence_ + 1);
    }

    void WriteWavHeader(FILE* f, uint32_t data_bytes) {
        uint32_t byte_rate = kSampleRate * kChannels * kBitsPerSample / 8;
        uint16_t block_align = kChannels * kBitsPerSample / 8;
        uint32_t chunk_size = 36 + data_bytes;
        fseek(f, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, f);
        fwrite(&chunk_size, 4, 1, f);
        fwrite("WAVE", 1, 4, f);
        fwrite("fmt ", 1, 4, f);
        uint32_t fmt_len = 16;
        fwrite(&fmt_len, 4, 1, f);
        uint16_t audio_fmt = 1;  // PCM
        fwrite(&audio_fmt, 2, 1, f);
        uint16_t ch = kChannels;
        fwrite(&ch, 2, 1, f);
        uint32_t sr = kSampleRate;
        fwrite(&sr, 4, 1, f);
        fwrite(&byte_rate, 4, 1, f);
        fwrite(&block_align, 2, 1, f);
        uint16_t bps = kBitsPerSample;
        fwrite(&bps, 2, 1, f);
        fwrite("data", 1, 4, f);
        fwrite(&data_bytes, 4, 1, f);
        fflush(f);
    }

    void StartRecord() {
        if (state_.load() != State::Idle) return;
        rec_sequence_++;
        char path[64];
        snprintf(path, sizeof(path), "/sdcard/RECORD/REC_%06u.wav", rec_sequence_);
        rec_path_ = path;
        rec_file_ = fopen(path, "wb");
        if (!rec_file_) {
            ShowStatus("FILE OPEN FAILED");
            ESP_LOGE(REC_TAG, "Failed to open %s", path);
            rec_sequence_--;  // don't burn a number on failure
            return;
        }
        // Placeholder header; finalized on StopRecord().
        uint8_t placeholder[44] = {0};
        if (fwrite(placeholder, 1, 44, rec_file_) != 44) {
            fclose(rec_file_);
            rec_file_ = nullptr;
            ShowStatus("SD WRITE FAILED");
            ESP_LOGE(REC_TAG, "Failed to write WAV header placeholder");
            rec_sequence_--;
            return;
        }
        rec_data_bytes_ = 0;
        rec_start_ms_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
        rec_stop_.store(false);
        state_.store(State::Recording);
        xTaskCreate(RecordTaskEntry, "k10_rec", 4096, this, 5, &rec_task_);
        UpdateUi();
        ESP_LOGI(REC_TAG, "Recording started: %s", path);
    }

    void StopRecord() {
        if (state_.load() != State::Recording) return;
        rec_stop_.store(true);
        // Wait for task to finish (max 2s).
        for (int i = 0; i < 200 && rec_task_ != nullptr; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (rec_file_) {
            WriteWavHeader(rec_file_, rec_data_bytes_);
            fclose(rec_file_);
            rec_file_ = nullptr;
            last_path_ = rec_path_;
            ESP_LOGI(REC_TAG, "Recording saved: %s (%u bytes audio)", rec_path_.c_str(), rec_data_bytes_);
        }
        state_.store(State::Idle);
        UpdateUi();
        ShowStatus("SAVED");
        vTaskDelay(pdMS_TO_TICKS(800));
        UpdateUi();
    }

    static void RecordTaskEntry(void* arg) {
        static_cast<K10Recorder*>(arg)->RecordTask();
    }

    void RecordTask() {
        auto& audio = Application::GetInstance().GetAudioService();
        std::vector<int16_t> data;
        std::vector<int16_t> mono;
        mono.reserve(kChunkSamples);
        while (!rec_stop_.load()) {
            data.clear();
            if (!audio.ReadAudioData(data, kSampleRate, kChunkSamples)) {
                ESP_LOGW(REC_TAG, "ReadAudioData failed");
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            // data is interleaved stereo (input_channels=2); take ch0 for mono WAV.
            // Digital gain x4 (+12dB) with saturation: Recorder-only boost, voice path untouched.
            size_t frames = data.size() / 2;
            mono.clear();
            for (size_t i = 0; i < frames; i++) {
                int32_t v = (int32_t)data[i * 2] * 4;
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                mono.push_back((int16_t)v);
            }
            if (rec_file_) {
                size_t written = fwrite(mono.data(), sizeof(int16_t), mono.size(), rec_file_);
                if (written != mono.size()) {
                    ESP_LOGE(REC_TAG, "SD write failed (disk full?)");
                    rec_stop_.store(true);
                    break;
                }
                rec_data_bytes_ += written * sizeof(int16_t);
            }
            // Update duration display every ~500ms.
            static int ticks = 0;
            if (++ticks % 5 == 0) UpdateUi();
        }
        // Flush any buffered data.
        if (rec_file_) fflush(rec_file_);
        rec_task_ = nullptr;
        vTaskDelete(nullptr);
    }

    void StartPlayback() {
        if (state_.load() != State::Idle) return;
        if (last_path_.empty()) {
            ShowStatus("NO RECORDING");
            return;
        }
        // Verify file exists and has valid WAV header.
        FILE* f = fopen(last_path_.c_str(), "rb");
        if (!f) {
            ShowStatus("FILE NOT FOUND");
            return;
        }
        uint8_t hdr[44];
        bool ok = fread(hdr, 1, 44, f) == 44 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0;
        fclose(f);
        if (!ok) {
            ShowStatus("BAD WAV FILE");
            ESP_LOGE(REC_TAG, "Invalid WAV: %s", last_path_.c_str());
            return;
        }
        play_stop_.store(false);
        state_.store(State::Playing);
        xTaskCreate(PlaybackTaskEntry, "k10_play", 4096, this, 5, &play_task_);
        UpdateUi();
        ESP_LOGI(REC_TAG, "Playback started: %s", last_path_.c_str());
    }

    void StopPlayback() {
        if (state_.load() != State::Playing) return;
        play_stop_.store(true);
        for (int i = 0; i < 200 && play_task_ != nullptr; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        state_.store(State::Idle);
        UpdateUi();
    }

    static void PlaybackTaskEntry(void* arg) {
        static_cast<K10Recorder*>(arg)->PlaybackTask();
    }

    void PlaybackTask() {
        FILE* f = fopen(last_path_.c_str(), "rb");
        if (!f) {
            play_task_ = nullptr;
            state_.store(State::Idle);
            vTaskDelete(nullptr);
            return;
        }
        fseek(f, 44, SEEK_SET);  // skip WAV header
        auto& audio = Application::GetInstance().GetAudioService();
        uint32_t generation = audio.GetPlaybackGeneration();
        std::vector<int16_t> pcm(kChunkSamples);
        while (!play_stop_.load()) {
            size_t n = fread(pcm.data(), sizeof(int16_t), kChunkSamples, f);
            if (n == 0) break;  // EOF
            pcm.resize(n);
            // PushPcmToPlaybackQueue takes ownership via move.
            std::vector<int16_t> chunk = pcm;
            if (!audio.PushPcmToPlaybackQueue(std::move(chunk), generation)) {
                ESP_LOGW(REC_TAG, "Playback queue push failed");
                break;
            }
            pcm.resize(kChunkSamples);
        }
        fclose(f);
        play_task_ = nullptr;
        if (!play_stop_.load()) {
            // Natural EOF: return to idle.
            state_.store(State::Idle);
            UpdateUi();
        }
        vTaskDelete(nullptr);
    }
};

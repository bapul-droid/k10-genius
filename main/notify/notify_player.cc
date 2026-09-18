#include "notify_player.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include <esp_log.h>

#include "assets/lang_config.h"
#include "board.h"
#include "http.h"
#include "ogg_demuxer.h"

#ifdef CONFIG_GENIUS_DEVICE_CORE
#include <cstdlib>
#include "genius/media_decoder.h"
extern const char genius_alarm_start[] asm("_binary_alarm_ogg_start");
extern const char genius_alarm_end[] asm("_binary_alarm_ogg_end");
extern const char genius_ews_start[] asm("_binary_ews_ogg_start");
extern const char genius_ews_end[] asm("_binary_ews_ogg_end");
#endif

namespace {
constexpr int kHttpTimeoutMs = 5000;
constexpr size_t kHttpReadBufferSize = 1024;
constexpr uint32_t kNotifyTaskStackSize = 8192;
constexpr UBaseType_t kNotifyTaskPriority = 2;
const char* TAG = "NotifyPlayer";

std::string_view BuiltinSound(const std::string& url) {
#ifdef CONFIG_GENIUS_DEVICE_CORE
    if (url == "builtin:ews")
        return std::string_view(genius_ews_start, genius_ews_end - genius_ews_start);
    if (url == "builtin:alarm")
        return std::string_view(genius_alarm_start, genius_alarm_end - genius_alarm_start);
    if (url == "builtin:notification")
        return Lang::Sounds::OGG_POPUP;
#endif
    return {};
}

bool IsSupportedUrl(const std::string& url) {
    return !BuiltinSound(url).empty() || url.compare(0, 7, "http://") == 0 ||
           url.compare(0, 8, "https://") == 0;
}
}  // namespace

NotifyPlayer::NotifyPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

NotifyPlayer::~NotifyPlayer() { Stop(); }

bool NotifyPlayer::Start(std::string audio_url, std::vector<NotifySubtitle> subtitles,
                         uint32_t playback_id, SubtitleCallback subtitle_callback,
                         FinishedCallback finished_callback) {
    if (playback_id == 0 || !IsSupportedUrl(audio_url)) {
        return false;
    }

    std::stable_sort(subtitles.begin(), subtitles.end(),
                     [](const NotifySubtitle& left, const NotifySubtitle& right) {
                         return left.start_ms < right.start_ms;
                     });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ || worker_running_) {
            return false;
        }
        audio_url_ = std::move(audio_url);
        subtitles_ = std::move(subtitles);
        displayed_text_.clear();
        subtitle_callback_ = std::move(subtitle_callback);
        finished_callback_ = std::move(finished_callback);
        playback_id_ = playback_id;
        audio_generation_ = audio_service_.GetPlaybackGeneration();
        last_playback_position_ms_ = 0;
        underrun_count_ = 0;
        next_subtitle_index_ = 0;
        active_ = true;
        worker_running_ = true;
        cancelled_ = false;
        http_finished_ = false;
        stream_started_ = false;
        playback_drained_ = false;
        completion_reported_ = false;
    }

    BaseType_t created = xTaskCreate(WorkerEntry, "notify_http", kNotifyTaskStackSize, this,
                                     kNotifyTaskPriority, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create notification HTTP task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
        cancelled_ = true;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void NotifyPlayer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    active_ = false;
    subtitles_.clear();
    subtitle_callback_ = nullptr;
    finished_callback_ = nullptr;
}

bool NotifyPlayer::IsActive(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && (playback_id == 0 || playback_id == playback_id_);
}

bool NotifyPlayer::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ || worker_running_;
}

bool NotifyPlayer::IsCancelled(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_ || !active_ || playback_id != playback_id_;
}

void NotifyPlayer::OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms) {
    SubtitleCallback callback;
    std::string text;
    bool subtitle_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || playback_id != playback_id_) {
            return;
        }
        last_playback_position_ms_ = media_position_ms;
        while (next_subtitle_index_ < subtitles_.size() &&
               subtitles_[next_subtitle_index_].start_ms <= media_position_ms) {
            text = subtitles_[next_subtitle_index_].text;
            ++next_subtitle_index_;
            subtitle_changed = true;
        }
        if (!subtitle_changed || text == displayed_text_) {
            return;
        }
        displayed_text_ = text;
        callback = subtitle_callback_;
    }
    if (callback) {
        callback(playback_id, text);
    }
}

NotifyPlayer::FinishedCallback NotifyPlayer::CompleteLocked(uint32_t& playback_id) {
    if (completion_reported_ || !active_) {
        return nullptr;
    }
    completion_reported_ = true;
    active_ = false;
    playback_id = playback_id_;
    return finished_callback_;
}

void NotifyPlayer::OnPlaybackDrained() {
    FinishedCallback callback;
    uint32_t playback_id = 0;
    uint32_t underrun_count = 0;
    uint32_t media_position_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
if (!active_) {
            return;
        }
        playback_drained_ = true;
        if (http_finished_) {
            callback = CompleteLocked(playback_id);
        } else if (stream_started_) {
            underrun_count = ++underrun_count_;
            media_position_ms = last_playback_position_ms_;
        }
    }
    if (underrun_count != 0) {
        ESP_LOGW(TAG, "Notification playback underrun #%lu at %lu ms",
                 static_cast<unsigned long>(underrun_count),
                 static_cast<unsigned long>(media_position_ms));
    }
    if (callback) {
        callback(playback_id, true);
    }
}

void NotifyPlayer::WorkerEntry(void* arg) {
    auto* player = static_cast<NotifyPlayer*>(arg);
    player->WorkerTask();
    vTaskDelete(nullptr);
}

void NotifyPlayer::WorkerTask() {
    std::string audio_url;
    uint32_t playback_id = 0, generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_url = audio_url_;
        playback_id = playback_id_;
        generation = audio_generation_;
    }
    bool success = false;
    auto demuxer = std::make_unique<OggDemuxer>();
    uint32_t media_position_ms = 0;
    bool packet_error = false;
    demuxer->OnPacket(
        [this, playback_id, generation, &media_position_ms, &packet_error](
            const uint8_t* data, int sample_rate, int frame_duration_ms, size_t size) {
            if (packet_error || IsCancelled(playback_id)) {
                packet_error = true;
                return;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = frame_duration_ms;
            packet->playback_id = playback_id;
            packet->media_position_ms = media_position_ms;
            packet->payload.assign(data, data + size);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancelled_ || !active_ || playback_id != playback_id_) {
                    packet_error = true;
                    return;
                }
                playback_drained_ = false;
            }
            // An old HTTP worker cannot inject audio after Stop/reset, even if it
            // was waiting for queue space or a network read when cancellation ran.
            if (!audio_service_.PushPacketToDecodeQueue(std::move(packet), true, generation)) {
                packet_error = true;
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stream_started_ = true;
            }
            media_position_ms += frame_duration_ms;
        });
    const auto builtin = BuiltinSound(audio_url);
    if (!builtin.empty()) {
        size_t offset = 0;
        while (offset < builtin.size() && !packet_error && !IsCancelled(playback_id)) {
            size_t count = std::min(kHttpReadBufferSize, builtin.size() - offset);
            demuxer->Process(reinterpret_cast<const uint8_t*>(builtin.data() + offset), count);
            if (demuxer->HasError())
                packet_error = true;
            offset += count;
        }
        success = !packet_error && !IsCancelled(playback_id) && demuxer->Finish();
    } else {
        auto* network = Board::GetInstance().GetNetwork();
        auto http = network ? network->CreateHttp(2) : nullptr;
        if (http) {
            http->SetTimeout(kHttpTimeoutMs);
            http->SetKeepAlive(false);
            http->SetHeader("Accept", "audio/ogg, audio/mpeg, audio/aac, audio/wav");
            http->SetHeader("Icy-MetaData", "0");
            http->SetHeader("Accept-Encoding", "identity");
            std::string current_url = audio_url;
            auto opened = http->Open("GET", current_url);
            for (int redirect = 0; redirect < 5 && opened && !IsCancelled(playback_id);
                 ++redirect) {
                auto status = http->GetStatusCode();
                if (!status || (*status != 301 && *status != 302 && *status != 303 &&
                                *status != 307 && *status != 308))
                    break;
                auto location = http->GetResponseHeader("Location");
                if (location.empty())
                    location = http->GetResponseHeader("location");
                if (location.empty() || location.size() > 2048 ||
                    location.find_first_of("\r\n\t ") != std::string::npos)
                    break;
                auto host_end = current_url.find('/', current_url.starts_with("https://") ? 8 : 7);
                auto origin = current_url.substr(0, host_end);
                if (location.starts_with("//"))
                    location =
                        (current_url.starts_with("https://") ? "https:" : "http:") + location;
                else if (location.starts_with("/"))
                    location = origin + location;
                else if (!location.starts_with("http://") && !location.starts_with("https://")) {
                    auto slash = current_url.substr(0, current_url.find('?')).rfind('/');
                    location = slash < origin.size() ? origin + "/" + location
                                                     : current_url.substr(0, slash + 1) + location;
                }
                if (!IsSupportedUrl(location))
                    break;
                http->Close();
                current_url = std::move(location);
                opened = http->Open("GET", current_url);
            }
            audio_url = current_url;
            if (opened) {
                auto status = http->GetStatusCode();
                if (status && *status >= 200 && *status < 300) {
                    std::array<char, kHttpReadBufferSize> buffer;
#ifdef CONFIG_GENIUS_DEVICE_CORE
                    std::unique_ptr<GeniusMediaDecoder> pcm_decoder;
                    std::vector<uint8_t> prefix;
                    bool identified = false, ogg = true;
                    auto mime = http->GetResponseHeader("Content-Type");
                    if (mime.empty())
                        mime = http->GetResponseHeader("content-type");
                    const auto rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
                    uint64_t pcm_samples = 0;
                    auto feed = [&](const uint8_t* data, size_t count) {
                        if (!identified) {
                            prefix.insert(prefix.end(), data, data + count);
                            if (prefix.size() < 4)
                                return;
                            identified = true;
                            ogg = memcmp(prefix.data(), "OggS", 4) == 0;
                            if (!ogg) {
                                pcm_decoder = std::make_unique<GeniusMediaDecoder>(
                                    rate, [this, playback_id, generation, rate,
                                           &pcm_samples](std::vector<int16_t>&& pcm) {
                                        if (IsCancelled(playback_id))
                                            return false;
                                        uint32_t position = pcm_samples * 1000 / rate;
                                        pcm_samples += pcm.size();
                                        {
                                            std::lock_guard<std::mutex> lock(mutex_);
                                            playback_drained_ = false;
                                        }
                                        if (!audio_service_.PushPcmToPlaybackQueue(
                                                std::move(pcm), generation, playback_id, position))
                                            return false;
                                        {
                                            std::lock_guard<std::mutex> lock(mutex_);
                                            stream_started_ = true;
                                        }
                                        return true;
                                    });
                                if (!pcm_decoder->Open(mime, audio_url, prefix.data(),
                                                       prefix.size())) {
                                    packet_error = true;
                                    return;
                                }
                            }
                            data = prefix.data();
                            count = prefix.size();
                        }
                        if (ogg) {
                            demuxer->Process(data, count);
                            packet_error = demuxer->HasError();
                        } else if (!pcm_decoder->Process(data, count))
                            packet_error = true;
                        prefix.clear();
                    };
                    auto interval = http->GetResponseHeader("icy-metaint");
                    size_t meta_interval = 0, remaining = 0, skip = 0;
                    if (!interval.empty()) {
                        char* end = nullptr;
                        meta_interval = strtoul(interval.c_str(), &end, 10);
                        if (!end || *end || meta_interval == 0 || meta_interval > 1048576)
                            packet_error = true;
                        remaining = meta_interval;
                    }
#endif
                    while (!packet_error && !IsCancelled(playback_id)) {
                        auto size = http->Read(buffer.data(), buffer.size());
                        if (!size) {
                            ESP_LOGE(TAG, "Audio HTTP read failed");
                            break;
                        }
                        if (*size == 0) {
#ifdef CONFIG_GENIUS_DEVICE_CORE
                            success = identified && (ogg ? demuxer->Finish()
                                                         : pcm_decoder->Process(nullptr, 0, true) &&
                                                               pcm_decoder->samples() > 0);
#else
                            success = demuxer->Finish();
#endif
                            break;
                        }
#ifdef CONFIG_GENIUS_DEVICE_CORE
                        auto* data = reinterpret_cast<const uint8_t*>(buffer.data());
                        if (meta_interval) {
                            size_t offset = 0;
                            while (offset < size_t(*size) && !packet_error) {
                                if (skip) {
                                    auto count = std::min(skip, size_t(*size) - offset);
                                    skip -= count;
                                    offset += count;
                                    if (!skip)
                                        remaining = meta_interval;
                                } else if (!remaining) {
                                    skip = size_t(data[offset++]) * 16;
                                    if (!skip)
                                        remaining = meta_interval;
                                } else {
                                    auto count = std::min(remaining, size_t(*size) - offset);
                                    feed(data + offset, count);
                                    offset += count;
                                    remaining -= count;
                                }
                            }
                        } else
                            feed(data, *size);
#else
                        demuxer->Process(reinterpret_cast<const uint8_t*>(buffer.data()), *size);
                        if (demuxer->HasError())
                            packet_error = true;
#endif
                    }
                } else
                    ESP_LOGE(TAG, "Audio HTTP response is not successful");
            } else
                ESP_LOGE(TAG, "Audio HTTP connection failed");
            http->Close();
            http.reset();
        }
    }
    FinishedCallback callback;
    uint32_t completed_playback_id = 0;
    bool report_success = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        task_handle_ = nullptr;
        if (!cancelled_ && active_ && playback_id == playback_id_) {
            if (!success || packet_error)
                callback = CompleteLocked(completed_playback_id);
            else {
                http_finished_ = true;
if (playback_drained_ && audio_service_.IsPlaybackIdle()) {
                    callback = CompleteLocked(completed_playback_id);
                    report_success = true;
                }
            }
        }
    }
    if (callback)
        callback(completed_playback_id, report_success);
}

#include "media_decoder.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include "esp_aac_dec.h"
#include "esp_mp3_dec.h"
#include "esp_pcm_dec.h"
#include "impl/esp_wav_dec.h"

GeniusMediaDecoder::GeniusMediaDecoder(int rate, Output output)
    : output_rate_(rate), output_(std::move(output)), pcm_buffer_(8192) {}
GeniusMediaDecoder::~GeniusMediaDecoder() {
    if (decoder_)
        esp_audio_simple_dec_close(decoder_);
    if (resampler_)
        esp_ae_rate_cvt_close(resampler_);
}
bool GeniusMediaDecoder::Open(const std::string& type, const std::string& url,
                              const uint8_t* prefix, size_t size) {
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, []() {
        registered = esp_mp3_dec_register() == ESP_AUDIO_ERR_OK &&
                     esp_aac_dec_register() == ESP_AUDIO_ERR_OK &&
                     esp_pcm_dec_register() == ESP_AUDIO_ERR_OK &&
                     esp_wav_dec_register() == ESP_AUDIO_ERR_OK;
    });
    if (!registered)
        return false;
    esp_audio_simple_dec_cfg_t cfg = {};
    auto path = url.substr(0, url.find('?'));
    if ((size >= 4 && memcmp(prefix, "RIFF", 4) == 0) || type.find("wav") != std::string::npos ||
        path.ends_with(".wav"))
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    else if (type.find("aac") != std::string::npos || path.ends_with(".aac") ||
             (size >= 2 && prefix[0] == 0xff && (prefix[1] & 0xf6) == 0xf0))
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    else if (type.find("mpeg") != std::string::npos || path.ends_with(".mp3") ||
             (size >= 3 && memcmp(prefix, "ID3", 3) == 0) ||
             (size >= 2 && prefix[0] == 0xff && (prefix[1] & 0xe0) == 0xe0))
        cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    else
        return false;
    return esp_audio_simple_dec_open(&cfg, &decoder_) == ESP_AUDIO_ERR_OK;
}
bool GeniusMediaDecoder::OutputPcm(size_t bytes) {
    esp_audio_simple_dec_info_t info = {};
    if (esp_audio_simple_dec_get_info(decoder_, &info) != ESP_AUDIO_ERR_OK ||
        info.bits_per_sample != 16 || info.channel < 1 || info.channel > 2 ||
        info.sample_rate < 8000 || info.sample_rate > 96000 || bytes % (2 * info.channel))
        return false;
    auto* pcm = reinterpret_cast<const int16_t*>(pcm_buffer_.data());
    std::vector<int16_t> mono(bytes / (2 * info.channel));
    for (size_t i = 0; i < mono.size(); ++i)
        mono[i] = info.channel == 1 ? pcm[i] : (int32_t(pcm[2 * i]) + pcm[2 * i + 1]) / 2;
    if (source_rate_ != info.sample_rate) {
        if (resampler_)
            esp_ae_rate_cvt_close(resampler_);
        resampler_ = nullptr;
        source_rate_ = info.sample_rate;
        if (source_rate_ != uint32_t(output_rate_)) {
            esp_ae_rate_cvt_cfg_t cfg = {};
            cfg.src_rate = source_rate_;
            cfg.dest_rate = output_rate_;
            cfg.channel = 1;
            cfg.bits_per_sample = 16;
            cfg.complexity = 2;
            cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
            if (esp_ae_rate_cvt_open(&cfg, &resampler_) != ESP_AE_ERR_OK)
                return false;
        }
    }
    if (resampler_) {
        uint32_t capacity = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(resampler_, mono.size(), &capacity);
        if (capacity > 32768)
            return false;
        std::vector<int16_t> converted(capacity);
        if (esp_ae_rate_cvt_process(resampler_, (esp_ae_sample_t)mono.data(), mono.size(),
                                    (esp_ae_sample_t)converted.data(), &capacity) != ESP_AE_ERR_OK)
            return false;
        converted.resize(capacity);
        mono = std::move(converted);
    }
    samples_ += mono.size();
    // Keep individual output writes below40ms so STOP cannot leave a long PCM
    // block in flight. Queue generation rejects any cancelled producer.
    const size_t chunk = output_rate_ / 25;
    for (size_t start = 0; start < mono.size(); start += chunk) {
        auto end = std::min(start + chunk, mono.size());
        if (!output_(std::vector<int16_t>(mono.begin() + start, mono.begin() + end)))
            return false;
    }
    return true;
}
bool GeniusMediaDecoder::Process(const uint8_t* data, size_t size, bool eos) {
    if (!decoder_ || size > 8192)
        return false;
    if (size == 0)
        return eos && samples_ > 0;
    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = const_cast<uint8_t*>(data);
    raw.len = size;
    raw.eos = eos;
    do {
        esp_audio_simple_dec_out_t out = {};
        out.buffer = pcm_buffer_.data();
        out.len = pcm_buffer_.size();
        auto result = esp_audio_simple_dec_process(decoder_, &raw, &out);
        if (raw.consumed > raw.len)
            return false;
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
        if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (out.needed_size <= pcm_buffer_.size() || out.needed_size > 32768)
                return false;
            pcm_buffer_.resize(out.needed_size);
            continue;
        }
        if (result != ESP_AUDIO_ERR_OK)
            return false;
        if (out.decoded_size && !OutputPcm(out.decoded_size))
            return false;
        if (raw.len && raw.consumed == 0 && out.decoded_size == 0)
            return false;
    } while (raw.len);
    return true;
}

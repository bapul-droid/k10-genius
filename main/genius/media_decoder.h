#pragma once
#include <functional>
#include <string>
#include <vector>
#include "esp_ae_rate_cvt.h"
#include "esp_audio_simple_dec.h"

class GeniusMediaDecoder {
public:
    using Output = std::function<bool(std::vector<int16_t>&&)>;
    explicit GeniusMediaDecoder(int output_rate, Output output);
    ~GeniusMediaDecoder();
    bool Open(const std::string& content_type, const std::string& url, const uint8_t* prefix,
              size_t size);
    bool Process(const uint8_t* data, size_t size, bool eos = false);
    size_t samples() const { return samples_; }

private:
    int output_rate_;
    Output output_;
    esp_audio_simple_dec_handle_t decoder_ = nullptr;
    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    uint32_t source_rate_ = 0;
    size_t samples_ = 0;
    std::vector<uint8_t> pcm_buffer_;
    bool OutputPcm(size_t bytes);
};

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct StreamInfo {
    int index = -1;

    std::string type;
    std::string codecName;
    std::string profile;

    int width = 0;
    int height = 0;
    double fps = 0.0;
    std::string pixelFormat;    // 像素格式，值可能为 AV_PIX_FMT_YUV420P

    int sampleRate = 0;
    int channels = 0;
    std::string sampleFormat;   // 音频采样格式，值可能为 AV_SAMPLE_FMT_FLTP

    int64_t bitRate = 0;
    double durationSec = 0.0;
};

struct MediaInfo {
    std::string url;
    std::string formatName;
    std::string formatLongName;

    double durationSec = 0.0;
    int64_t bitRate = 0;

    std::vector<StreamInfo> streams;
};

class MediaProbe {
public:
    std::optional<MediaInfo> probe(const std::string& url, std::string* error = nullptr) const;
};

void printMediaInfo(const MediaInfo& info);

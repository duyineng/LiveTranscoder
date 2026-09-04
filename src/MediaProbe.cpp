#include "MediaProbe.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
}

#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

namespace {

    std::string avErrorToString(int errnum) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        av_strerror(errnum, buffer, sizeof(buffer));
        return std::string(buffer);
    }

    double durationToSeconds(int64_t duration, AVRational timeBase) {
        if (duration == AV_NOPTS_VALUE || duration <= 0) {
            return 0.0;
        }
        return static_cast<double>(duration) * av_q2d(timeBase);
    }

    double globalDurationToSeconds(int64_t duration) {
        if (duration == AV_NOPTS_VALUE || duration <= 0) {
            return 0.0;
        }
        return static_cast<double>(duration) / AV_TIME_BASE;
    }

    struct FormatContextDeleter {
        void operator()(AVFormatContext* ctx) const {
            if (ctx) {
                avformat_close_input(&ctx);
            }
        }
    };

    using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;

} // namespace

std::optional<MediaInfo> MediaProbe::probe(const std::string& url, std::string* error) const {
    AVFormatContext* rawCtx = nullptr;

    // 打开输入
    int ret = avformat_open_input(&rawCtx, url.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (error) {
            *error = "avformat_open_input failed: " + avErrorToString(ret);
        }
        return std::nullopt;
    }

    FormatContextPtr fmtCtx(rawCtx);

    // 查找流信息
    ret = avformat_find_stream_info(fmtCtx.get(), nullptr);
    if (ret < 0) {
        if (error) {
            *error = "avformat_find_stream_info failed: " + avErrorToString(ret);
        }
        return std::nullopt;
    }

    MediaInfo info;
    info.url = url;

    if (fmtCtx->iformat) {
        if (fmtCtx->iformat->name) {
            info.formatName = fmtCtx->iformat->name;
        }
        if (fmtCtx->iformat->long_name) {
            info.formatLongName = fmtCtx->iformat->long_name;
        }
    }

    info.durationSec = globalDurationToSeconds(fmtCtx->duration);
    info.bitRate = fmtCtx->bit_rate;

    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        AVStream* stream = fmtCtx->streams[i];
        AVCodecParameters* codecpar = stream->codecpar;

        StreamInfo s;
        s.index = static_cast<int>(i);

        const char* typeName = av_get_media_type_string(codecpar->codec_type);  // 可能是 "video"，"audio"
        s.type = typeName ? typeName : "unknown";

        s.codecName = avcodec_get_name(codecpar->codec_id); // 可能是 "h264"，"aac"

        const char* profile = avcodec_profile_name(codecpar->codec_id, codecpar->profile);  // 编码档次，可能是 "Main"，"HE-AAC"
        if (profile) {
            s.profile = profile;
        }

        s.bitRate = codecpar->bit_rate;
        s.durationSec = durationToSeconds(stream->duration, stream->time_base);

        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            // 读取视频宽高，表示分辨率
            s.width = codecpar->width;
            s.height = codecpar->height;

            // 平均帧率，numerator 分子，denominator 分母
            if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
                s.fps = av_q2d(stream->avg_frame_rate); // q 代表有理数
            }

            if (codecpar->format != -1) {   // codecpar->format 值可能为 AV_PIX_FMT_YUV420P
                const char* pixFmt = av_get_pix_fmt_name(static_cast<AVPixelFormat>(codecpar->format));
                if (pixFmt) {
                    s.pixelFormat = pixFmt;
                }
            }
        }
        else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            s.sampleRate = codecpar->sample_rate;   // 采样率

#if LIBAVUTIL_VERSION_MAJOR >= 57
            s.channels = codecpar->ch_layout.nb_channels;   // 音频声道数，1：单声道，2：立体声
#else
            s.channels = codecpar->channels;
#endif

            if (codecpar->format != -1) {
                const char* sampleFmt = av_get_sample_fmt_name(static_cast<AVSampleFormat>(codecpar->format));
                if (sampleFmt) {
                    s.sampleFormat = sampleFmt; // 音频采样格式
                }
            }
        }

        info.streams.push_back(std::move(s));
    }

    return info;
}

void printMediaInfo(const MediaInfo& info) {
    std::cout << "========== Media Info ==========\n";
    std::cout << "url      : " << info.url << "\n";
    std::cout << "format   : " << info.formatName;

    if (!info.formatLongName.empty()) {
        std::cout << " (" << info.formatLongName << ")";
    }

    std::cout << "\n";
    std::cout << "duration : " << std::fixed << std::setprecision(3)
        << info.durationSec << " sec\n";
    std::cout << "bitrate  : " << info.bitRate << " bps\n";
    std::cout << "streams  : " << info.streams.size() << "\n\n";

    for (const auto& s : info.streams) {
        std::cout << "[" << s.index << "] "
            << s.type << " / " << s.codecName;

        if (!s.profile.empty()) {
            std::cout << " / " << s.profile;
        }

        std::cout << "\n";

        if (s.type == "video") {
            std::cout << "    size     : " << s.width << "x" << s.height << "\n";
            std::cout << "    fps      : " << std::fixed << std::setprecision(3) << s.fps << "\n";

            if (!s.pixelFormat.empty()) {
                std::cout << "    pix_fmt  : " << s.pixelFormat << "\n";
            }
        }
        else if (s.type == "audio") {
            std::cout << "    sample   : " << s.sampleRate << " Hz\n";
            std::cout << "    channels : " << s.channels << "\n";

            if (!s.sampleFormat.empty()) {
                std::cout << "    fmt      : " << s.sampleFormat << "\n";
            }
        }

        std::cout << "    duration : " << std::fixed << std::setprecision(3)
            << s.durationSec << " sec\n";
        std::cout << "    bitrate  : " << s.bitRate << " bps\n\n";
    }
}

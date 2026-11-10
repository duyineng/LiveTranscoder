#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

inline std::string errorString(int errnum) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errnum, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

struct InputFormatContextDeleter {
    void operator()(AVFormatContext* context) const {
        if (context) {
            avformat_close_input(&context);
        }
    }
};

struct OutputFormatContextDeleter {
    void operator()(AVFormatContext* context) const {
        if (context) {
            if (context->pb) {
                avio_closep(&context->pb);
            }
            avformat_free_context(context);
        }
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const {
        avcodec_free_context(&context);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

struct SwsContextDeleter {
    void operator()(SwsContext* context) const { sws_freeContext(context); }
};

struct SwrContextDeleter {
    void operator()(SwrContext* context) const { swr_free(&context); }
};

struct AudioFifoDeleter {
    void operator()(AVAudioFifo* fifo) const { av_audio_fifo_free(fifo); }
};

using InputFormatContextPtr = std::unique_ptr<AVFormatContext, InputFormatContextDeleter>;
using OutputFormatContextPtr = std::unique_ptr<AVFormatContext, OutputFormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

inline int getCodecConfig(const AVCodec* codec, AVCodecConfig config, const void** outConfigs) {
    *outConfigs = nullptr;
    return avcodec_get_supported_config(nullptr, codec, config, 0, outConfigs, nullptr);
}

inline int chooseSampleRate(const AVCodec* codec, int requested, int fallback) {
    const void* configs = nullptr;
    if (getCodecConfig(codec, AV_CODEC_CONFIG_SAMPLE_RATE, &configs) < 0 || !configs) {
        return requested > 0 ? requested : fallback;
    }

    const int* rates = static_cast<const int*>(configs);
    int best = rates[0];
    int bestDistance = std::abs(best - requested);
    for (const int* rate = rates; *rate; ++rate) {
        const int distance = std::abs(*rate - requested);
        if (distance < bestDistance) {
            best = *rate;
            bestDistance = distance;
        }
    }
    return best;
}

inline AVSampleFormat chooseSampleFormat(const AVCodec* codec) {
    const void* configs = nullptr;
    if (getCodecConfig(codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, &configs) < 0 || !configs) {
        return AV_SAMPLE_FMT_FLTP;
    }

    const auto* formats = static_cast<const AVSampleFormat*>(configs);
    for (const AVSampleFormat* format = formats; *format != AV_SAMPLE_FMT_NONE; ++format) {
        if (*format == AV_SAMPLE_FMT_FLTP) {
            return *format;
        }
    }
    return formats[0];
}

inline bool codecAcceptsYuv420p(const AVCodec* codec) {
    const void* configs = nullptr;
    if (getCodecConfig(codec, AV_CODEC_CONFIG_PIX_FORMAT, &configs) < 0) {
        return false;
    }
    if (!configs) {
        return true;
    }

    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_YUV420P) {
            return true;
        }
    }
    return false;
}

inline const AVCodec* findSoftwareH264Encoder() {
    // Prefer libx264 when the FFmpeg package was built with it.
    // The generic H.264 lookup may otherwise select a hardware-only encoder (for example h264_d3d12va) that cannot accept the CPU YUV420P frames used here.
    if (const AVCodec* codec = avcodec_find_encoder_by_name("libx264")) {
        return codec;
    }
    if (const AVCodec* codec = avcodec_find_encoder_by_name("h264_mf")) {
        return codec;
    }

    void* opaque = nullptr;
    const AVCodec* codec = nullptr;
    while ((codec = av_codec_iterate(&opaque)) != nullptr) {
        if (!av_codec_is_encoder(codec) || codec->id != AV_CODEC_ID_H264) {
            continue;
        }
        if (codecAcceptsYuv420p(codec)) {
            return codec;
        }
    }
    return nullptr;
}

inline bool openInput(const std::string& url, InputFormatContextPtr& input, std::string& error) {
    AVFormatContext* raw = nullptr;
    const int openRet = avformat_open_input(&raw, url.c_str(), nullptr, nullptr);
    if (openRet < 0) {
        error = "Failed to open input: " + url + ": " + errorString(openRet);
        return false;
    }

    input.reset(raw);
    const int infoRet = avformat_find_stream_info(input.get(), nullptr);
    if (infoRet < 0) {
        error = "Failed to find input stream info: " + errorString(infoRet);
        input.reset();
        return false;
    }
    return true;
}

#include "EncoderSetup.h"

#include <string>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace {

std::string failText(const std::string& message, int errnum) {
    if (errnum >= 0) {
        return message;
    }
    return message + ": " + errorString(errnum);
}

} // namespace

bool openH264Encoder(const VideoEncoderConfig& config, CodecContextPtr& encoder, std::string& error) {
    const AVCodec* videoEncoder = nullptr;
    if (config.closedGop) {
        videoEncoder = avcodec_find_encoder_by_name("libx264");
    }
    if (!videoEncoder) {
        videoEncoder = findSoftwareH264Encoder();
    }
    if (!videoEncoder) {
        error = "H.264 encoder is not available";
        return false;
    }

    encoder.reset(avcodec_alloc_context3(videoEncoder));
    if (!encoder) {
        error = "Failed to allocate video encoder context";
        return false;
    }

    AVRational frameRate = config.frameRate;
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = AVRational{30, 1};
    }

    encoder->framerate = frameRate;
    encoder->time_base = av_inv_q(frameRate);
    encoder->width = config.width;
    encoder->height = config.height;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->gop_size = config.gopSize;
    encoder->max_b_frames = config.maxBFrames;
    encoder->bit_rate = config.bitrate;

    const bool isLibx264 = videoEncoder->name && std::string(videoEncoder->name) == "libx264";
    if (videoEncoder->id == AV_CODEC_ID_H264 && encoder->priv_data) {
        av_opt_set(encoder->priv_data, "preset", "veryfast", 0);
        if (config.zeroLatency) {
            av_opt_set(encoder->priv_data, "tune", "zerolatency", 0);
        }
        if (config.closedGop && isLibx264) {
            const std::string x264Params =
                "keyint=" + std::to_string(config.gopSize) +
                ":min-keyint=" + std::to_string(config.gopSize) +
                ":scenecut=0:open-gop=0";
            av_opt_set(encoder->priv_data, "forced-idr", "1", 0);
            const int optRet = av_opt_set(encoder->priv_data, "x264-params", x264Params.c_str(), 0);
            if (optRet < 0) {
                error = failText("Failed to set closed GOP parameters", optRet);
                encoder.reset();
                return false;
            }
        }
    }

    if (config.globalHeader) {
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    const int ret = avcodec_open2(encoder.get(), videoEncoder, nullptr);
    if (ret < 0) {
        error = failText("Failed to open H.264 encoder", ret);
        encoder.reset();
        return false;
    }
    return true;
}

bool openAacEncoder(
    const AVCodec* audioEncoder,
    const AudioEncoderConfig& config,
    const AVChannelLayout& decoderLayout,
    CodecContextPtr& encoder,
    std::string& error
) {
    if (!audioEncoder) {
        error = "AAC encoder is not available";
        return false;
    }

    encoder.reset(avcodec_alloc_context3(audioEncoder));
    if (!encoder) {
        error = "Failed to allocate audio encoder context";
        return false;
    }

    const int requestedRate = config.sampleRate > 0 ? config.sampleRate : 48'000;
    encoder->sample_rate = chooseSampleRate(audioEncoder, requestedRate, 48'000);
    encoder->sample_fmt = chooseSampleFormat(audioEncoder);
    encoder->bit_rate = config.bitrate;
    encoder->time_base = AVRational{1, encoder->sample_rate};

    const int channels = decoderLayout.nb_channels > 0
        ? decoderLayout.nb_channels
        : (config.channels > 0 ? config.channels : 2);
    av_channel_layout_default(&encoder->ch_layout, channels);

    if (config.globalHeader) {
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    const int ret = avcodec_open2(encoder.get(), audioEncoder, nullptr);
    if (ret < 0) {
        error = failText("Failed to open AAC encoder", ret);
        encoder.reset();
        return false;
    }
    return true;
}

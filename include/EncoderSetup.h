#pragma once

#include "FFmpegSupport.h"

extern "C" {
#include <libavutil/channel_layout.h>
}

struct VideoEncoderConfig {
    int width = 0;
    int height = 0;
    int bitrate = 0;
    AVRational frameRate{30, 1};
    int gopSize = 60;
    int maxBFrames = 2;
    bool zeroLatency = false;
    bool globalHeader = false;
    bool closedGop = false;
};

struct AudioEncoderConfig {
    int bitrate = 128'000;
    int sampleRate = 48'000;
    int channels = 2;
    bool globalHeader = false;
};

bool openH264Encoder(const VideoEncoderConfig& config, CodecContextPtr& encoder, std::string& error);

bool openAacEncoder(
    const AVCodec* audioEncoder,
    const AudioEncoderConfig& config,
    const AVChannelLayout& decoderLayout,
    CodecContextPtr& encoder,
    std::string& error
);

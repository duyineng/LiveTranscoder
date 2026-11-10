#include "HlsVod.h"

#include "EncoderSetup.h"
#include "FFmpegSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

namespace {

struct VariantOutput {
    HlsVariant spec;
    SwsContextPtr scaler;
    CodecContextPtr videoEncoder;
    OutputFormatContextPtr format;
    AVStream* videoStream = nullptr;
    AVStream* audioStream = nullptr;
};

std::string ffmpegPath(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).generic_string();
}

bool setHlsOption(AVFormatContext* format, const char* key, const std::string& value, std::string& error) {
    const int ret = av_opt_set(format->priv_data, key, value.c_str(), 0);
    if (ret < 0) {
        error = "Failed to set HLS option " + std::string(key) + ": " + errorString(ret);
        return false;
    }
    return true;
}

bool relativizeSegmentNames(const std::filesystem::path& playlist, std::string& error) {
    std::ifstream input(playlist);
    if (!input) {
        error = "Failed to read playlist: " + playlist.string();
        return false;
    }

    std::string rewritten;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#') {
            line = std::filesystem::path(line).filename().string();
        }
        rewritten += line;
        rewritten += '\n';
    }
    input.close();

    std::ofstream output(playlist, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Failed to rewrite playlist: " + playlist.string();
        return false;
    }
    output << rewritten;
    if (!output.good()) {
        error = "Failed to rewrite playlist: " + playlist.string();
        return false;
    }
    return true;
}

bool writeMasterPlaylist(
    const std::filesystem::path& outputDir,
    const std::vector<VariantOutput>& outputs,
    bool hasAudio,
    int audioBitrate,
    std::string& error
) {
    std::vector<const VariantOutput*> ordered;
    ordered.reserve(outputs.size());
    for (const VariantOutput& output : outputs) {
        ordered.push_back(&output);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const VariantOutput* left, const VariantOutput* right) {
        return left->spec.videoBitrate > right->spec.videoBitrate;
    });

    const std::filesystem::path playlist = outputDir / "master.m3u8";
    std::ofstream output(playlist, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Failed to write master playlist: " + playlist.string();
        return false;
    }

    output << "#EXTM3U\n";
    output << "#EXT-X-VERSION:3\n";
    for (const VariantOutput* variant : ordered) {
        const int bandwidth = variant->spec.videoBitrate + (hasAudio ? audioBitrate : 0);
        output << "#EXT-X-STREAM-INF:BANDWIDTH=" << bandwidth
               << ",RESOLUTION=" << variant->spec.width << "x" << variant->spec.height << "\n";
        output << variant->spec.name << "/index.m3u8\n";
    }
    if (!output.good()) {
        error = "Failed to write master playlist: " + playlist.string();
        return false;
    }
    return true;
}

} // namespace

bool hlsVod(
    const std::string& inputUrl,
    const std::string& outputDir,
    const HlsRequest& request,
    std::string& error
) {
    auto fail = [&](const std::string& message, int errnum = 0) -> bool {
        error = message;
        if (errnum < 0) {
            error += ": ";
            error += errorString(errnum);
        }
        return false;
    };

    if (request.variants.empty()) {
        return fail("HLS requires at least one variant");
    }
    if (request.segmentDurationSeconds <= 0) {
        return fail("HLS segment duration must be positive");
    }
    for (const HlsVariant& variant : request.variants) {
        if (variant.width <= 0 || variant.height <= 0 || variant.videoBitrate <= 0 || variant.name.empty()) {
            return fail("HLS variant is incomplete");
        }
        if ((variant.width % 2) != 0 || (variant.height % 2) != 0) {
            return fail("HLS width and height must be even");
        }
    }

    InputFormatContextPtr input;
    if (!openInput(inputUrl, input, error)) {
        return false;
    }

    int ret = 0;
    const int videoIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        return fail("Input has no video stream", videoIndex);
    }
    const int audioIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const bool hasAudio = audioIndex >= 0;

    const AVStream* inputVideoStream = input->streams[videoIndex];
    const AVStream* inputAudioStream = hasAudio ? input->streams[audioIndex] : nullptr;
    const AVCodec* videoDecoder = avcodec_find_decoder(inputVideoStream->codecpar->codec_id);
    const AVCodec* audioDecoder = hasAudio ? avcodec_find_decoder(inputAudioStream->codecpar->codec_id) : nullptr;
    if (!videoDecoder || (hasAudio && !audioDecoder)) {
        return fail("Input decoder is not available");
    }

    CodecContextPtr videoDecoderCtx(avcodec_alloc_context3(videoDecoder));
    CodecContextPtr audioDecoderCtx(hasAudio ? avcodec_alloc_context3(audioDecoder) : nullptr);
    if (!videoDecoderCtx || (hasAudio && !audioDecoderCtx)) {
        return fail("Failed to allocate decoder context");
    }
    ret = avcodec_parameters_to_context(videoDecoderCtx.get(), inputVideoStream->codecpar);
    if (ret < 0) {
        return fail("Failed to configure video decoder", ret);
    }
    if (hasAudio) {
        ret = avcodec_parameters_to_context(audioDecoderCtx.get(), inputAudioStream->codecpar);
        if (ret < 0) {
            return fail("Failed to configure audio decoder", ret);
        }
    }
    ret = avcodec_open2(videoDecoderCtx.get(), videoDecoder, nullptr);
    if (ret < 0) {
        return fail("Failed to open video decoder", ret);
    }
    if (hasAudio) {
        ret = avcodec_open2(audioDecoderCtx.get(), audioDecoder, nullptr);
        if (ret < 0) {
            return fail("Failed to open audio decoder", ret);
        }
    }

    AVRational frameRate = inputVideoStream->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = AVRational{30, 1};
    }
    const int fps = std::max(1, static_cast<int>(std::lround(av_q2d(frameRate))));
    const int keyint = fps * request.segmentDurationSeconds;    // 关键帧间隔

    const std::filesystem::path root(outputDir);
    std::vector<VariantOutput> outputs; // 按 request.variants 里的每一档，把「这一档的 HLS 输出壳子」搭好，推进 outputs 里
    outputs.reserve(request.variants.size());

    for (const HlsVariant& spec : request.variants) {
        const std::filesystem::path variantDir = root / spec.name;
        std::error_code createError;
        std::filesystem::create_directories(variantDir, createError);
        if (createError) {
            return fail("Failed to create directory: " + variantDir.string() + ": " + createError.message());
        }

        const std::filesystem::path playlist = variantDir / "index.m3u8";
        const std::filesystem::path segmentPattern = variantDir / "seg_%03d.ts";
        AVFormatContext* raw = nullptr;
        ret = avformat_alloc_output_context2(&raw, nullptr, "hls", ffmpegPath(playlist).c_str());
        if (ret < 0 || !raw) {
            return fail("Failed to create HLS context: " + playlist.string(), ret);
        }

        VariantOutput variant;
        variant.spec = spec;    
        variant.format.reset(raw);
        // Cut on the next keyframe after this time. Keeping it under the GOP
        // length avoids waiting another GOP when a keyframe lands just before
        // the nominal segment boundary.
        char hlsTime[32];
        std::snprintf(
            hlsTime,
            sizeof(hlsTime),
            "%.3f",
            std::max(0.5, static_cast<double>(request.segmentDurationSeconds) - 0.5)
        );
        if (!setHlsOption(variant.format.get(), "hls_time", hlsTime, error)
            || !setHlsOption(variant.format.get(), "hls_playlist_type", "vod", error)
            || !setHlsOption(variant.format.get(), "hls_list_size", "0", error)
            || !setHlsOption(variant.format.get(), "hls_flags", "independent_segments", error)
            || !setHlsOption(variant.format.get(), "hls_segment_filename", ffmpegPath(segmentPattern), error)) {
            return false;
        }
        outputs.push_back(std::move(variant));
    }

    const bool globalHeader = (outputs.front().format->oformat->flags & AVFMT_GLOBALHEADER) != 0;
    const AVCodec* audioEncoderCodec = hasAudio ? avcodec_find_encoder(AV_CODEC_ID_AAC) : nullptr;
    CodecContextPtr audioEncoderCtx;
    SwrContextPtr resamplerCtx;
    AudioFifoPtr audioFifo;
    int audioFrameSize = 1024;
    if (hasAudio) {
        AudioEncoderConfig audioConfig;
        audioConfig.bitrate = request.audioBitrate;
        audioConfig.sampleRate = request.audioSampleRate > 0 ? request.audioSampleRate : audioDecoderCtx->sample_rate;
        audioConfig.globalHeader = globalHeader;
        if (!openAacEncoder(audioEncoderCodec, audioConfig, audioDecoderCtx->ch_layout, audioEncoderCtx, error)) {
            return false;
        }
        audioFrameSize = audioEncoderCtx->frame_size > 0 ? audioEncoderCtx->frame_size : 1024;

        AVChannelLayout audioDecoderChLayout = audioDecoderCtx->ch_layout;
        if (audioDecoderChLayout.nb_channels <= 0) {
            av_channel_layout_default(&audioDecoderChLayout, 2);
        }
        SwrContext* swrCtxRaw = nullptr;
        ret = swr_alloc_set_opts2(
            &swrCtxRaw,
            &audioEncoderCtx->ch_layout, audioEncoderCtx->sample_fmt, audioEncoderCtx->sample_rate,
            &audioDecoderChLayout, audioDecoderCtx->sample_fmt, audioDecoderCtx->sample_rate,
            0, nullptr
        );
        resamplerCtx.reset(swrCtxRaw);
        if (ret < 0 || !resamplerCtx) {
            return fail("Failed to create audio resampler", ret);
        }
        ret = swr_init(resamplerCtx.get());
        if (ret < 0) {
            return fail("Failed to initialize audio resampler", ret);
        }
        audioFifo.reset(av_audio_fifo_alloc(audioEncoderCtx->sample_fmt, audioEncoderCtx->ch_layout.nb_channels, 1));
        if (!audioFifo) {
            return fail("Failed to allocate audio FIFO");
        }
    }

    for (VariantOutput& variant : outputs) {
        VideoEncoderConfig videoConfig;
        videoConfig.width = variant.spec.width;
        videoConfig.height = variant.spec.height;
        videoConfig.bitrate = variant.spec.videoBitrate;
        videoConfig.frameRate = frameRate;
        videoConfig.gopSize = keyint;
        videoConfig.maxBFrames = 2;
        videoConfig.zeroLatency = false;
        videoConfig.closedGop = true;
        videoConfig.globalHeader = globalHeader;
        if (!openH264Encoder(videoConfig, variant.videoEncoder, error)) {
            return false;
        }

        variant.scaler.reset(sws_getContext(
            videoDecoderCtx->width, videoDecoderCtx->height, videoDecoderCtx->pix_fmt,
            variant.videoEncoder->width, variant.videoEncoder->height, variant.videoEncoder->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        ));
        if (!variant.scaler) {
            return fail("Failed to create video scaler");
        }

        variant.videoStream = avformat_new_stream(variant.format.get(), nullptr);
        if (!variant.videoStream) {
            return fail("Failed to create HLS video stream");
        }
        variant.videoStream->time_base = variant.videoEncoder->time_base;
        ret = avcodec_parameters_from_context(variant.videoStream->codecpar, variant.videoEncoder.get());
        if (ret < 0) {
            return fail("Failed to copy video encoder parameters", ret);
        }

        if (hasAudio) {
            variant.audioStream = avformat_new_stream(variant.format.get(), nullptr);
            if (!variant.audioStream) {
                return fail("Failed to create HLS audio stream");
            }
            variant.audioStream->time_base = audioEncoderCtx->time_base;
            ret = avcodec_parameters_from_context(variant.audioStream->codecpar, audioEncoderCtx.get());
            if (ret < 0) {
                return fail("Failed to copy audio encoder parameters", ret);
            }
        }

        if (!(variant.format->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&variant.format->pb, variant.format->url, AVIO_FLAG_WRITE);
            if (ret < 0) {
                return fail("Failed to open HLS output", ret);
            }
        }
        ret = avformat_write_header(variant.format.get(), nullptr);
        if (ret < 0) {
            return fail("Failed to write HLS header", ret);
        }
    }

    PacketPtr packet(av_packet_alloc());
    PacketPtr encodedPacket(av_packet_alloc());
    FramePtr decodedFrame(av_frame_alloc());
    if (!packet || !encodedPacket || !decodedFrame) {
        return fail("Failed to allocate FFmpeg frame or packet");
    }

    int64_t nextAudioPts = 0;
    int64_t videoFrames = 0;
    int64_t audioFrames = 0;

    auto writeToStream = [&](AVFormatContext* format, AVStream* stream, AVCodecContext* encoder, AVPacket* encoded) -> bool {
        av_packet_rescale_ts(encoded, encoder->time_base, stream->time_base);
        encoded->stream_index = stream->index;
        ret = av_interleaved_write_frame(format, encoded);
        av_packet_unref(encoded);
        return ret >= 0;
    };

    auto writeAudioToVariants = [&](AVPacket* encoded) -> bool {
        for (VariantOutput& variant : outputs) {
            if (!variant.audioStream) {
                continue;
            }
            AVPacket* copy = av_packet_clone(encoded);
            if (!copy) {
                av_packet_unref(encoded);
                return false;
            }
            const bool ok = writeToStream(variant.format.get(), variant.audioStream, audioEncoderCtx.get(), copy);
            av_packet_free(&copy);
            if (!ok) {
                av_packet_unref(encoded);
                return false;
            }
        }
        av_packet_unref(encoded);
        return true;
    };

    auto drainVideo = [&](VariantOutput& variant) -> bool {
        while (true) {
            ret = avcodec_receive_packet(variant.videoEncoder.get(), encodedPacket.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                return false;
            }
            // h264_mf leaves duration at 0. One encoder tick is one frame when time_base is 1/fps.
            if (encodedPacket->duration <= 0) {
                encodedPacket->duration = 1;
            }
            if (!writeToStream(variant.format.get(), variant.videoStream, variant.videoEncoder.get(), encodedPacket.get())) {
                return false;
            }
        }
    };

    auto drainAudio = [&]() -> bool {
        while (true) {
            ret = avcodec_receive_packet(audioEncoderCtx.get(), encodedPacket.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                return false;
            }
            if (!writeAudioToVariants(encodedPacket.get())) {
                return false;
            }
        }
    };

    auto encodeVideoFrame = [&](AVFrame* decoderOutputFrame) -> bool {
        int64_t decoderOutputPts = decoderOutputFrame->best_effort_timestamp;
        if (decoderOutputPts == AV_NOPTS_VALUE) {
            decoderOutputPts = videoFrames;
        }
        ++videoFrames;

        for (VariantOutput& variant : outputs) {
            FramePtr encoderInputFrame(av_frame_alloc());
            if (!encoderInputFrame) {
                return false;
            }
            encoderInputFrame->format = variant.videoEncoder->pix_fmt;
            encoderInputFrame->width = variant.videoEncoder->width;
            encoderInputFrame->height = variant.videoEncoder->height;
            if ((ret = av_frame_get_buffer(encoderInputFrame.get(), 32)) < 0) {
                return false;
            }
            ret = sws_scale(
                variant.scaler.get(),
                decoderOutputFrame->data,
                decoderOutputFrame->linesize,
                0,
                decoderOutputFrame->height,
                encoderInputFrame->data,
                encoderInputFrame->linesize
            );
            if (ret <= 0) {
                return false;
            }
            encoderInputFrame->pts = av_rescale_q(
                decoderOutputPts, inputVideoStream->time_base, variant.videoEncoder->time_base
            );
            if (keyint > 0 && ((videoFrames - 1) % keyint) == 0) {
                encoderInputFrame->pict_type = AV_PICTURE_TYPE_I;
                encoderInputFrame->flags |= AV_FRAME_FLAG_KEY;
            }
            ret = avcodec_send_frame(variant.videoEncoder.get(), encoderInputFrame.get());
            if (ret < 0 || !drainVideo(variant)) {
                return false;
            }
        }
        return true;
    };

    auto encodeAudioFrames = [&](bool flush) -> bool {
        while (
            av_audio_fifo_size(audioFifo.get()) >= audioFrameSize
            || (flush && av_audio_fifo_size(audioFifo.get()) > 0)
        ) {
            const int available = av_audio_fifo_size(audioFifo.get());
            const int samples = flush ? std::min(audioFrameSize, available) : audioFrameSize;
            FramePtr audioFrame(av_frame_alloc());
            if (!audioFrame) {
                return false;
            }
            audioFrame->nb_samples = audioFrameSize;
            audioFrame->format = audioEncoderCtx->sample_fmt;
            audioFrame->sample_rate = audioEncoderCtx->sample_rate;
            ret = av_channel_layout_copy(&audioFrame->ch_layout, &audioEncoderCtx->ch_layout);
            if (ret < 0 || (ret = av_frame_get_buffer(audioFrame.get(), 0)) < 0) {
                return false;
            }
            ret = av_audio_fifo_read(audioFifo.get(), reinterpret_cast<void**>(audioFrame->data), samples);
            if (ret != samples) {
                return false;
            }
            if (samples < audioFrameSize) {
                av_samples_set_silence(
                    audioFrame->data,
                    samples,
                    audioFrameSize - samples,
                    audioEncoderCtx->ch_layout.nb_channels,
                    audioEncoderCtx->sample_fmt
                );
            }
            audioFrame->pts = nextAudioPts;
            nextAudioPts += audioFrameSize;
            ++audioFrames;
            ret = avcodec_send_frame(audioEncoderCtx.get(), audioFrame.get());
            if (ret < 0 || !drainAudio()) {
                return false;
            }
            if (!flush && av_audio_fifo_size(audioFifo.get()) < audioFrameSize) {
                break;
            }
        }
        return true;
    };

    auto resampleDecodedAudio = [&](AVFrame* decodedAudioFrame) -> bool {
        const int outputSamples = static_cast<int>(
            av_rescale_rnd(
                swr_get_delay(resamplerCtx.get(), audioDecoderCtx->sample_rate) + decodedAudioFrame->nb_samples,
                audioEncoderCtx->sample_rate,
                audioDecoderCtx->sample_rate,
                AV_ROUND_UP
            )
        );
        FramePtr converted(av_frame_alloc());
        if (!converted) {
            return false;
        }
        converted->nb_samples = outputSamples;
        converted->format = audioEncoderCtx->sample_fmt;
        converted->sample_rate = audioEncoderCtx->sample_rate;
        if (
            (ret = av_channel_layout_copy(&converted->ch_layout, &audioEncoderCtx->ch_layout)) < 0
            || (ret = av_frame_get_buffer(converted.get(), 0)) < 0
        ) {
            return false;
        }
        const int convertedSamples = swr_convert(
            resamplerCtx.get(),
            converted->data,
            outputSamples,
            const_cast<const uint8_t**>(decodedAudioFrame->extended_data),
            decodedAudioFrame->nb_samples
        );
        if (convertedSamples < 0) {
            ret = convertedSamples;
            return false;
        }
        ret = av_audio_fifo_realloc(audioFifo.get(), av_audio_fifo_size(audioFifo.get()) + convertedSamples);
        if (
            ret < 0
            || av_audio_fifo_write(audioFifo.get(), reinterpret_cast<void**>(converted->data), convertedSamples) < convertedSamples
        ) {
            return false;
        }
        return encodeAudioFrames(false);
    };

    auto decodePacket = [&](AVPacket* inputPacket, AVCodecContext* decoderCtx, bool isVideo) -> bool {
        ret = avcodec_send_packet(decoderCtx, inputPacket);
        if (ret < 0) {
            return false;
        }
        while (true) {
            ret = avcodec_receive_frame(decoderCtx, decodedFrame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                return false;
            }
            const bool ok = isVideo ? encodeVideoFrame(decodedFrame.get()) : resampleDecodedAudio(decodedFrame.get());
            av_frame_unref(decodedFrame.get());
            if (!ok) {
                return false;
            }
        }
    };

    while ((ret = av_read_frame(input.get(), packet.get())) >= 0) {
        const bool isVideo = packet->stream_index == videoIndex;
        const bool isAudio = hasAudio && packet->stream_index == audioIndex;
        if ((isVideo || isAudio) && !decodePacket(packet.get(), isVideo ? videoDecoderCtx.get() : audioDecoderCtx.get(), isVideo)) {
            return fail("Failed during decode or encode", ret);
        }
        av_packet_unref(packet.get());
    }
    if (ret != AVERROR_EOF) {
        return fail("Failed while reading input packet", ret);
    }

    if (!decodePacket(nullptr, videoDecoderCtx.get(), true)) {
        return fail("Failed to flush video decoder", ret);
    }
    for (VariantOutput& variant : outputs) {
        ret = avcodec_send_frame(variant.videoEncoder.get(), nullptr);
        if (ret < 0 || !drainVideo(variant)) {
            return fail("Failed to flush video encoder", ret);
        }
    }

    if (hasAudio) {
        if (!decodePacket(nullptr, audioDecoderCtx.get(), false)) {
            return fail("Failed to flush audio decoder", ret);
        }

        auto flushResampler = [&]() -> bool {
            while (true) {
                const int delayed = swr_get_delay(resamplerCtx.get(), audioDecoderCtx->sample_rate);
                if (delayed <= 0) {
                    return true;
                }
                const int outputSamples = static_cast<int>(
                    av_rescale_rnd(delayed, audioEncoderCtx->sample_rate, audioDecoderCtx->sample_rate, AV_ROUND_UP)
                );
                FramePtr converted(av_frame_alloc());
                if (!converted) {
                    return false;
                }
                converted->nb_samples = outputSamples;
                converted->format = audioEncoderCtx->sample_fmt;
                converted->sample_rate = audioEncoderCtx->sample_rate;
                if (
                    (ret = av_channel_layout_copy(&converted->ch_layout, &audioEncoderCtx->ch_layout)) < 0
                    || (ret = av_frame_get_buffer(converted.get(), 0)) < 0
                ) {
                    return false;
                }
                const int convertedSamples = swr_convert(resamplerCtx.get(), converted->data, outputSamples, nullptr, 0);
                if (convertedSamples < 0) {
                    ret = convertedSamples;
                    return false;
                }
                if (convertedSamples == 0) {
                    return true;
                }
                ret = av_audio_fifo_realloc(audioFifo.get(), av_audio_fifo_size(audioFifo.get()) + convertedSamples);
                if (
                    ret < 0
                    || av_audio_fifo_write(audioFifo.get(), reinterpret_cast<void**>(converted->data), convertedSamples) < convertedSamples
                ) {
                    return false;
                }
            }
        };

        if (!flushResampler()) {
            return fail("Failed to flush audio resampler", ret);
        }
        if (!encodeAudioFrames(true)) {
            return fail("Failed to flush audio FIFO", ret);
        }
        ret = avcodec_send_frame(audioEncoderCtx.get(), nullptr);
        if (ret < 0 || !drainAudio()) {
            return fail("Failed to flush AAC encoder", ret);
        }
    }

    for (VariantOutput& variant : outputs) {
        ret = av_write_trailer(variant.format.get());
        if (ret < 0) {
            return fail("Failed to write HLS trailer", ret);
        }
        const std::filesystem::path playlist = root / variant.spec.name / "index.m3u8";
        if (!relativizeSegmentNames(playlist, error)) {
            return false;
        }
    }

    if (!writeMasterPlaylist(root, outputs, hasAudio, request.audioBitrate, error)) {
        return false;
    }
    if (request.verbose) {
        std::cout << "HLS finished. Video frames: " << videoFrames
                  << ", audio frames: " << audioFrames << "\n"
                  << "Output directory: " << outputDir << "\n";
    }
    return true;
}

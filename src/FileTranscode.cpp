#include "FileTranscode.h"

#include "EncoderSetup.h"
#include "FFmpegSupport.h"

#include <algorithm>
#include <iostream>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

bool fileTranscode(
    AVFormatContext* input,
    const std::string& outputUrl,
    const TranscodeOptions& options,
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

    int ret = 0;
    const int videoIndex = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        return fail("Input has no video stream", videoIndex);
    }
    const int audioIndex = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
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

    const AVCodec* audioEncoder = hasAudio ? avcodec_find_encoder(AV_CODEC_ID_AAC) : nullptr;
    if (hasAudio && !audioEncoder) {
        return fail("AAC encoder is not available");
    }

    AVFormatContext* outputFmtCtxRaw = nullptr;
    ret = avformat_alloc_output_context2(&outputFmtCtxRaw, nullptr, nullptr, outputUrl.c_str());
    if (ret < 0 || !outputFmtCtxRaw) {
        return fail("Failed to create output context: " + outputUrl, ret);
    }
    OutputFormatContextPtr outputFmtCtx(outputFmtCtxRaw);

    const bool globalHeader = (outputFmtCtx->oformat->flags & AVFMT_GLOBALHEADER) != 0;
    AVRational frameRate = inputVideoStream->avg_frame_rate;
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = AVRational{30, 1};
    }

    VideoEncoderConfig videoConfig;
    videoConfig.width = options.width;
    videoConfig.height = options.height;
    videoConfig.bitrate = options.videoBitrate;
    videoConfig.frameRate = frameRate;
    videoConfig.gopSize = options.gopSize;
    videoConfig.maxBFrames = 2;
    videoConfig.zeroLatency = true;
    videoConfig.closedGop = false;
    videoConfig.globalHeader = globalHeader;

    CodecContextPtr videoEncoderCtx;
    if (!openH264Encoder(videoConfig, videoEncoderCtx, error)) {
        return false;
    }

    CodecContextPtr audioEncoderCtx;
    if (hasAudio) {
        AudioEncoderConfig audioConfig;
        audioConfig.bitrate = options.audioBitrate;
        audioConfig.sampleRate = options.audioSampleRate > 0 ? options.audioSampleRate : audioDecoderCtx->sample_rate;
        audioConfig.globalHeader = globalHeader;
        if (!openAacEncoder(audioEncoder, audioConfig, audioDecoderCtx->ch_layout, audioEncoderCtx, error)) {
            return false;
        }
    }

    SwsContextPtr scalerCtx(
        sws_getContext(
            videoDecoderCtx->width, videoDecoderCtx->height, videoDecoderCtx->pix_fmt,
            videoEncoderCtx->width, videoEncoderCtx->height, videoEncoderCtx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        )
    );
    if (!scalerCtx) {
        return fail("Failed to create video scaler");
    }

    SwrContextPtr resamplerCtx;
    if (hasAudio) {
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
    }

    AVStream* outputVideoStream = avformat_new_stream(outputFmtCtx.get(), nullptr);
    if (!outputVideoStream) {
        return fail("Failed to create output streams");
    }
    outputVideoStream->time_base = videoEncoderCtx->time_base;
    ret = avcodec_parameters_from_context(outputVideoStream->codecpar, videoEncoderCtx.get());
    if (ret < 0) {
        return fail("Failed to copy video encoder parameters", ret);
    }

    AVStream* outputAudioStream = nullptr;
    if (hasAudio) {
        outputAudioStream = avformat_new_stream(outputFmtCtx.get(), nullptr);
        if (!outputAudioStream) {
            return fail("Failed to create output streams");
        }
        outputAudioStream->time_base = audioEncoderCtx->time_base;
        ret = avcodec_parameters_from_context(outputAudioStream->codecpar, audioEncoderCtx.get());
        if (ret < 0) {
            return fail("Failed to copy audio encoder parameters", ret);
        }
    }

    if (!(outputFmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputFmtCtx->pb, outputUrl.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            return fail("Failed to open output: " + outputUrl, ret);
        }
    }
    ret = avformat_write_header(outputFmtCtx.get(), nullptr);
    if (ret < 0) {
        return fail("Failed to write output header", ret);
    }

    const int audioFrameSize = hasAudio && audioEncoderCtx->frame_size > 0 ? audioEncoderCtx->frame_size : 1024;
    AudioFifoPtr audioFifo;
    if (hasAudio) {
        audioFifo.reset(av_audio_fifo_alloc(
            audioEncoderCtx->sample_fmt, audioEncoderCtx->ch_layout.nb_channels, 1
        ));
        if (!audioFifo) {
            return fail("Failed to allocate audio FIFO");
        }
    }

    PacketPtr packet(av_packet_alloc());
    FramePtr decodedFrame(av_frame_alloc());
    if (!packet || !decodedFrame) {
        return fail("Failed to allocate FFmpeg frame or packet");
    }

    int64_t nextAudioPts = 0;
    int64_t videoFrames = 0;
    int64_t audioFrames = 0;

    auto writeEncodedPackets = [&](AVCodecContext* encoderCtx, AVStream* outputFmtStream) -> bool {
        while (true) {
            ret = avcodec_receive_packet(encoderCtx, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                return false;
            }
            av_packet_rescale_ts(packet.get(), encoderCtx->time_base, outputFmtStream->time_base);
            packet->stream_index = outputFmtStream->index;
            ret = av_interleaved_write_frame(outputFmtCtx.get(), packet.get());
            av_packet_unref(packet.get());
            if (ret < 0) {
                return false;
            }
        }
    };

    auto encodeVideoFrame = [&](AVFrame* decoderOutputFrame) -> bool {
        FramePtr encoderInputFrame(av_frame_alloc());
        if (!encoderInputFrame) {
            return false;
        }
        encoderInputFrame->format = videoEncoderCtx->pix_fmt;
        encoderInputFrame->width = videoEncoderCtx->width;
        encoderInputFrame->height = videoEncoderCtx->height;
        if ((ret = av_frame_get_buffer(encoderInputFrame.get(), 32)) < 0) {
            return false;
        }
        ret = sws_scale(
            scalerCtx.get(),
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

        int64_t decoderOutputPts = decoderOutputFrame->best_effort_timestamp;
        if (decoderOutputPts == AV_NOPTS_VALUE) {
            decoderOutputPts = videoFrames;
        }
        encoderInputFrame->pts = av_rescale_q(decoderOutputPts, inputVideoStream->time_base, videoEncoderCtx->time_base);
        ++videoFrames;
        ret = avcodec_send_frame(videoEncoderCtx.get(), encoderInputFrame.get());
        return ret >= 0 && writeEncodedPackets(videoEncoderCtx.get(), outputVideoStream);
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
            if (ret < 0 || !writeEncodedPackets(audioEncoderCtx.get(), outputAudioStream)) {
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

    while ((ret = av_read_frame(input, packet.get())) >= 0) {
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
        return fail("Failed to flush video decoder or encoder", ret);
    }
    ret = avcodec_send_frame(videoEncoderCtx.get(), nullptr);
    if (ret < 0 || !writeEncodedPackets(videoEncoderCtx.get(), outputVideoStream)) {
        return fail("Failed to flush video encoder", ret);
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
        if (ret < 0 || !writeEncodedPackets(audioEncoderCtx.get(), outputAudioStream)) {
            return fail("Failed to flush AAC encoder", ret);
        }
    }

    ret = av_write_trailer(outputFmtCtx.get());
    if (ret < 0) {
        return fail("Failed to write output trailer", ret);
    }
    if (options.verbose) {
        std::cout << "Transcode finished. Video frames: " << videoFrames
                  << ", audio frames: " << audioFrames << "\n"
                  << "Output file: " << outputUrl << "\n";
    }
    return true;
}

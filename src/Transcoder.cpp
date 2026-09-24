#include "Transcoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace {

std::string errorString(int errnum) {
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

int getCodecConfig(const AVCodec* codec, AVCodecConfig config, const void** outConfigs) {
    *outConfigs = nullptr;
    return avcodec_get_supported_config(nullptr, codec, config, 0, outConfigs, nullptr);
}

int chooseSampleRate(const AVCodec* codec, int requested, int fallback) {
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

AVSampleFormat chooseSampleFormat(const AVCodec* codec) {
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

bool codecAcceptsYuv420p(const AVCodec* codec) {
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

const AVCodec* findSoftwareH264Encoder() {
    // Prefer libx264 when the FFmpeg package was built with it. 
    // The generic H.264 lookup may otherwise select a hardware-only encoder (for example h264_d3d12va) that cannot accept the CPU YUV420P frames used here.
    if (const AVCodec* codec = avcodec_find_encoder_by_name("libx264")) {   // 按名称寻找 libx264 编码器
        return codec;
    }
    if (const AVCodec* codec = avcodec_find_encoder_by_name("h264_mf")) {   // Windows Media Foundation 的 H.264 编码器
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

} // namespace

std::string Transcoder::makeError(int errnum) {
    return errorString(errnum);
}

bool Transcoder::transcode(
    const std::string& inputUrl,
    const std::string& outputUrl,
    const TranscodeOptions& options,
    std::string* error
) {
    auto fail = [&](const std::string& message, int errnum = 0) {
        if (error) {
            *error = message;
            if (errnum < 0) {
                *error += ": ";
                *error += makeError(errnum);
            }
        }
        return false;
    };

    if (options.width <= 0 || options.height <= 0) {
        return fail("Output dimensions must be positive");  // 输出尺寸必须是正数
    }

    AVFormatContext* inputCtxRaw = nullptr;    // 容器格式上下文，例如 MP4、FLV
    int ret = avformat_open_input(&inputCtxRaw, inputUrl.c_str(), nullptr, nullptr);   // 分配并初始化 inputCtxRaw
    if (ret < 0) {
        return fail("Failed to open input: " + inputUrl, ret);
    }
    InputFormatContextPtr inputCtx(inputCtxRaw);
    inputCtxRaw = nullptr;

    ret = avformat_find_stream_info(inputCtx.get(), nullptr);  // 直接在上下文 inputCtx 中补充更多信息，例如音视频流信息
    if (ret < 0) {
        return fail("Failed to find input stream info", ret);
    }

    // 最普通的 MP4 可能只有：streams[0] H.264 视频，streams[1] AAC 音频
    // 但实际媒体也可能有：
    // streams[0]  视频  1080p 主画面
    // streams[1]  音频  中文
    // streams[2]  视频  360p 预览
    // streams[3]  音频  英文
    // streams[4]  字幕
    const int videoIndex = av_find_best_stream(inputCtx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);    // 寻找“最佳的视频流”，并返回它们在 inputCtx->streams 数组中的索引
    const int audioIndex = av_find_best_stream(inputCtx.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);    // 寻找“最佳的音频流”，并返回它们在 inputCtx->streams 数组中的索引
    if (videoIndex < 0) {
        return fail("Input has no video stream", videoIndex);
    }
    if (audioIndex < 0) {
        return fail("Input has no audio stream", audioIndex);
    }

    // AVStream 主要保存流的描述信息，例如媒体类型（视频 / 音频）、编码格式（H.264、AAC）、时间基 time_base（1 个 PTS 单位有多长，如1/30 秒）、视频分辨率或音频采样率等
    // 在媒体容器中，“流”又通常可理解为“轨道”
    const AVStream* inputVideoStream = inputCtx->streams[videoIndex];
    const AVStream* inputAudioStream = inputCtx->streams[audioIndex];

    // AVCodec 主要保存解码器的描述信息
    const AVCodec* videoDecoder = avcodec_find_decoder(inputVideoStream->codecpar->codec_id);   // codec_id 为编码格式 id，常见如 AV_CODEC_ID_H264，AV_CODEC_ID_HEVC（H.265）
    const AVCodec* audioDecoder = avcodec_find_decoder(inputAudioStream->codecpar->codec_id);   // codec_id 为编码格式 id，常见如 AV_CODEC_ID_AAC
    if (!videoDecoder || !audioDecoder) {
        return fail("Input decoder is not available");
    }

    // 分配并初始化解码器上下文，但还没有流的编码参数
    CodecContextPtr videoDecoderCtx(avcodec_alloc_context3(videoDecoder));
    CodecContextPtr audioDecoderCtx(avcodec_alloc_context3(audioDecoder));
    if (!videoDecoderCtx || !audioDecoderCtx) {
        return fail("Failed to allocate decoder context");
    }
    
    // 把输入流中的编码参数，复制到解码器上下文中
    ret = avcodec_parameters_to_context(videoDecoderCtx.get(), inputVideoStream->codecpar); 
    if (ret < 0) {
        return fail("Failed to configure video decoder", ret);
    }
    ret = avcodec_parameters_to_context(audioDecoderCtx.get(), inputAudioStream->codecpar);
    if (ret < 0) {
        return fail("Failed to configure audio decoder", ret);
    }

    // 通过解码器描述信息，真正的初始化并打开解码器上下文
    ret = avcodec_open2(videoDecoderCtx.get(), videoDecoder, nullptr);
    if (ret < 0) {
        return fail("Failed to open video decoder", ret);
    }
    ret = avcodec_open2(audioDecoderCtx.get(), audioDecoder, nullptr);
    if (ret < 0) {
        return fail("Failed to open audio decoder", ret);
    }

    // 编码器的描述信息对象
    const AVCodec* videoEncoder = findSoftwareH264Encoder();
    const AVCodec* audioEncoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!videoEncoder || !audioEncoder) {
        return fail("H.264 or AAC encoder is not available");
    }

    // 分配并初始化编码器上下文，但还没有流的编码参数
    CodecContextPtr videoEncoderCtx(avcodec_alloc_context3(videoEncoder));
    CodecContextPtr audioEncoderCtx(avcodec_alloc_context3(audioEncoder));
    if (!videoEncoderCtx || !audioEncoderCtx) {
        return fail("Failed to allocate encoder context");
    }

    AVRational frameRate = inputVideoStream->avg_frame_rate;    // 输入视频的平均帧率
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = AVRational{30, 1};
    }
    videoEncoderCtx->width = options.width;             // 送入编码器的那一帧画面的宽
    videoEncoderCtx->height = options.height;           // 送入编码器的那一帧画面的高
    videoEncoderCtx->pix_fmt = AV_PIX_FMT_YUV420P;      // 送入编码器未压缩视频帧的像素格式
    videoEncoderCtx->time_base = av_inv_q(frameRate);   
    videoEncoderCtx->framerate = frameRate;             
    videoEncoderCtx->bit_rate = options.videoBitrate;   // 码率，压缩后的视频流平均每秒应该流动多少比特数据量，单位是 bps
    videoEncoderCtx->gop_size = options.gopSize;        // Group Of Pictures，表示两个关键帧（I 帧）之间最多间隔多少帧
    videoEncoderCtx->max_b_frames = 2;                  // 非 B 帧和下一非 B 帧之间，最多插 2 个 B 帧
    if (videoEncoder->id == AV_CODEC_ID_H264) { 
        av_opt_set(videoEncoderCtx->priv_data, "preset", "veryfast", 0);    // 配置编码器的私有选项，编码较快，压缩效率较低，CPU占用较低，适合实时转码
        av_opt_set(videoEncoderCtx->priv_data, "tune", "zerolatency", 0);   // 配置编码器的私有选项，低延迟，通常会关掉 B 帧
    }

    const int requestedRate = options.audioSampleRate > 0 ? options.audioSampleRate : audioDecoderCtx->sample_rate;
    audioEncoderCtx->sample_rate = chooseSampleRate(audioEncoder, requestedRate, 48'000); // 采样率，一秒钟采多少次声音，例如 48000 Hz
    audioEncoderCtx->sample_fmt = chooseSampleFormat(audioEncoder); // 采样格式，例如 AV_SAMPLE_FMT_FLTP
    audioEncoderCtx->bit_rate = options.audioBitrate;
    audioEncoderCtx->time_base = AVRational{1, audioEncoderCtx->sample_rate};
    av_channel_layout_default(  // 给音频设置声道布局
        &audioEncoderCtx->ch_layout,    // 声道布局变量
        audioDecoderCtx->ch_layout.nb_channels > 0 ? audioDecoderCtx->ch_layout.nb_channels : 2
    );
    ret = avcodec_open2(videoEncoderCtx.get(), videoEncoder, nullptr);
    if (ret < 0) {
        return fail("Failed to open H.264 encoder", ret);
    }
    ret = avcodec_open2(audioEncoderCtx.get(), audioEncoder, nullptr);
    if (ret < 0) {
        return fail("Failed to open AAC encoder", ret);
    }

    SwsContextPtr scalerCtx(
        sws_getContext(
            videoDecoderCtx->width, videoDecoderCtx->height, videoDecoderCtx->pix_fmt,  // 解码器输出的视频格式，例如：568x320，YUV420P
            videoEncoderCtx->width, videoEncoderCtx->height, videoEncoderCtx->pix_fmt,  // 编码器输出的视频格式，例如：1280x720，YUV420P
            SWS_BILINEAR, nullptr, nullptr, nullptr // 表示缩放时使用“双线性插值”算法
        )
    );
    if (!scalerCtx) {
        return fail("Failed to create video scaler");
    }

    AVChannelLayout inputLayout = audioDecoderCtx->ch_layout;
    if (inputLayout.nb_channels <= 0) {
        av_channel_layout_default(&inputLayout, 2);
    }
    SwrContext* swrCtxRaw = nullptr;
    ret = swr_alloc_set_opts2(
        &swrCtxRaw,
        &audioEncoderCtx->ch_layout, audioEncoderCtx->sample_fmt, audioEncoderCtx->sample_rate, // 声道布局，采样格式，采样率
        &inputLayout, audioDecoderCtx->sample_fmt, audioDecoderCtx->sample_rate,
        0, nullptr
    );
    SwrContextPtr resamplerCtx(swrCtxRaw);
    if (ret < 0 || !resamplerCtx) {
        return fail("Failed to create audio resampler", ret);
    }
    ret = swr_init(resamplerCtx.get());
    if (ret < 0) {
        return fail("Failed to initialize audio resampler", ret);
    }

    AVFormatContext* outputCtxRaw = nullptr;   // 输出容器
    ret = avformat_alloc_output_context2(&outputCtxRaw, nullptr, "mp4", outputUrl.c_str());
    if (ret < 0 || !outputCtxRaw) {
        return fail("Failed to create MP4 output context", ret);
    }
    OutputFormatContextPtr outputCtx(outputCtxRaw);
    AVStream* outputVideoStream = avformat_new_stream(outputCtx.get(), nullptr);   // 主要是用来描述一条视频轨道的信息，真正的一帧帧视频数据保存在 AVPacket
    AVStream* outputAudioStream = avformat_new_stream(outputCtx.get(), nullptr);   // 主要是用来描述一条音频轨道的信息
    if (!outputVideoStream || !outputAudioStream) {
        return fail("Failed to create output streams");
    }
    outputVideoStream->time_base = videoEncoderCtx->time_base;
    outputAudioStream->time_base = audioEncoderCtx->time_base;
    ret = avcodec_parameters_from_context(outputVideoStream->codecpar, videoEncoderCtx.get());  // 把编码器上下文中的静态编码参数复制到输出流的 codecpar 中
    if (ret < 0) {
        return fail("Failed to copy video encoder parameters", ret);
    }
    ret = avcodec_parameters_from_context(outputAudioStream->codecpar, audioEncoderCtx.get());
    if (ret < 0) {
        return fail("Failed to copy audio encoder parameters", ret);
    }
    // outputCtx->oformat->flags，输出容器格式的标志集合
    // AVFMT_NOFILE，该格式不需要 FFmpeg 手动打开文件 IO
    if (!(outputCtx->oformat->flags & AVFMT_NOFILE)) { 
        ret = avio_open(&outputCtx->pb, outputUrl.c_str(), AVIO_FLAG_WRITE);   // avio_open() 会打开输出地址，并创建一个 AVIOContext，保存到 outputCtx->pb，为 mp4 文件的写入通道
        if (ret < 0) {
            return fail("Failed to open output: " + outputUrl, ret);
        }
    }
    ret = avformat_write_header(outputCtx.get(), nullptr); // 向输出文件写入“容器文件头”
    if (ret < 0) {
        return fail("Failed to write MP4 header", ret);
    }

    // audioEncoderCtx->frame_size 为每个声道一帧中应有的采样点数
    const int audioFrameSize = audioEncoderCtx->frame_size > 0 ? audioEncoderCtx->frame_size : 1024;
    AudioFifoPtr audioFifo(
        av_audio_fifo_alloc(audioEncoderCtx->sample_fmt, audioEncoderCtx->ch_layout.nb_channels, 1)
    );
    if (!audioFifo) {
        return fail("Failed to allocate audio FIFO");
    }

    PacketPtr packet(av_packet_alloc());
    FramePtr decodedFrame(av_frame_alloc());
    if (!packet || !decodedFrame) {
        return fail("Failed to allocate FFmpeg frame or packet");
    }
    int64_t nextAudioPts = 0;
    int64_t videoFrames = 0;
    int64_t audioFrames = 0;

    auto writeEncodedPackets = [&ret, &packet, &outputCtx](AVCodecContext* encoder, AVStream* stream) -> bool {
        while (true) {
            ret = avcodec_receive_packet(encoder, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                return false;
            }
            av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;
            ret = av_interleaved_write_frame(outputCtx.get(), packet.get());
            av_packet_unref(packet.get());
            if (ret < 0) {
                return false;
            }
        }
    };

    auto encodeVideoFrame = [&](AVFrame* source) -> bool {
        FramePtr converted(av_frame_alloc());
        if (!converted) {
            return false;
        }
        converted->format = videoEncoderCtx->pix_fmt;
        converted->width = videoEncoderCtx->width;
        converted->height = videoEncoderCtx->height;
        if ((ret = av_frame_get_buffer(converted.get(), 32)) < 0) {
            return false;
        }
        ret = sws_scale(
            scalerCtx.get(),       // SwsContext*
            source->data,       // 输入图像各个 plane 的数据的指针数组
            source->linesize,   // 每个 plane 每一行占用的字节数
            0,                  // 从输入图像的第 0 行开始处理
            source->height,     // 本次要处理的输入图像高度，也就是输入切片包含多少行
            converted->data,    // 输出图像各个 plane 的数据指针数组
            converted->linesize
        );
        if (ret <= 0) {
            return false;
        }   
        int64_t sourcePts = source->best_effort_timestamp;
        if (sourcePts == AV_NOPTS_VALUE) {
            sourcePts = videoFrames;
        }
        converted->pts = av_rescale_q(sourcePts, inputVideoStream->time_base, videoEncoderCtx->time_base);
        ++videoFrames;
        ret = avcodec_send_frame(videoEncoderCtx.get(), converted.get());
        return ret >= 0 && writeEncodedPackets(videoEncoderCtx.get(), outputVideoStream);
    };

    auto encodeAudioFrames = [&](bool flush) -> bool {
        while (
            av_audio_fifo_size(audioFifo.get()) >= audioFrameSize   // 音频 FIFO 中当前缓存的采样点数量，是否已经达到编码器一帧所需要的采样点数量
            || (flush && av_audio_fifo_size(audioFifo.get()) > 0)   // 如果现在处于“收尾/清空缓冲区”阶段，并且 FIFO 里还残留音频采样，就继续处理它
        ) {
            const int available = av_audio_fifo_size(audioFifo.get());
            const int samples = flush ? std::min(audioFrameSize, available) : audioFrameSize;
            FramePtr audioFrame(av_frame_alloc());
            if (!audioFrame) {
                return false;
            } 
            // 采样点/采样率 = 音频帧持续时间
            audioFrame->nb_samples = audioFrameSize;    // 音频帧中每个声道的采样点
            audioFrame->format = audioEncoderCtx->sample_fmt;   
            audioFrame->sample_rate = audioEncoderCtx->sample_rate; // 采样率
            ret = av_channel_layout_copy(&audioFrame->ch_layout, &audioEncoderCtx->ch_layout);  // 复制通道布局
            if (ret < 0 || (ret = av_frame_get_buffer(audioFrame.get(), 0)) < 0) {
                return false;
            }
            // 从音频 FIFO 中取出 samples 个采样，复制到 audioFrame 的音频缓冲区中，并从 FIFO 中移除这些数据
            ret = av_audio_fifo_read(
                audioFifo.get(),    // 音频 FIFO 缓冲区
                reinterpret_cast<void**>(audioFrame->data), // 把读取出来的音频采样放到这里
                samples             // 表示本次读取多少个采样
            );
            if (ret != samples) {
                return false;
            }
            // 给“最后一帧不完整的音频”补静音
            if (samples < audioFrameSize) {
                av_samples_set_silence( 
                    audioFrame->data, 
                    samples,    // 起始偏移量，也就是从第几个采样开始补静音
                    audioFrameSize - samples,   // 要补多少个采样
                    audioEncoderCtx->ch_layout.nb_channels, 
                    audioEncoderCtx->sample_fmt
                );
            }
            audioFrame->pts = nextAudioPts;
            nextAudioPts += audioFrameSize; // 下一帧音频的 PTS，要在当前帧 PTS 的基础上向后移动“这一帧包含的采样数”
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

    auto convertAudio = [&](AVFrame* source) -> bool {
        const int outputSamples = static_cast<int>(
            av_rescale_rnd(
                swr_get_delay(resamplerCtx.get(), audioDecoderCtx->sample_rate) + source->nb_samples,  // 重采样器内部尚未输出的延迟采样数 + 每个声道包含的采样数
                audioEncoderCtx->sample_rate,   // 目标采样率，例如 48000 Hz
                audioDecoderCtx->sample_rate,   // 输入采样率，例如 44100 Hz
                AV_ROUND_UP // 向上取整
            )
        );
        FramePtr converted(av_frame_alloc());
        if (!converted) {
            return false;
        }
        converted->nb_samples = outputSamples;  // 每个声道的采样数
        converted->format = audioEncoderCtx->sample_fmt;
        converted->sample_rate = audioEncoderCtx->sample_rate;
        if ((ret = av_channel_layout_copy(&converted->ch_layout, &audioEncoderCtx->ch_layout)) < 0 ||
            (ret = av_frame_get_buffer(converted.get(), 0)) < 0) {
            return false;
        }
        const int convertedSamples = swr_convert(   // convertedSamples 表示这次实际生成的输出采样数，也是“每个声道”的数量
            resamplerCtx.get(), 
            converted->data,    // 输出音频缓冲区，转换后的音频数据会被写入这里
            outputSamples,      // 输出缓冲区最多可以容纳的采样数
            const_cast<const uint8_t**>(source->extended_data), // 指向当前解码音频帧的各个声道数据
            source->nb_samples  // 当前输入音频帧中每个声道的采样数
        );
        if (convertedSamples < 0) {
            return false;
        }
        // 把 FIFO 扩容
        ret = av_audio_fifo_realloc(audioFifo.get(), av_audio_fifo_size(audioFifo.get()) + convertedSamples);
        // 把 converted->data 中的音频数据写入 FIFO
        if (ret < 0 || av_audio_fifo_write(audioFifo.get(), reinterpret_cast<void**>(converted->data), convertedSamples) < convertedSamples) {
            return false;
        }
        return encodeAudioFrames(false);
    };

    auto flushResampler = [&]() -> bool {
        while (true) {
            const int delayed = swr_get_delay(resamplerCtx.get(), audioDecoderCtx->sample_rate);   // 查询重采样器里还有多少未输出的“输入采样等效量”
            if (delayed <= 0) {
                return true;
            }
            const int outputSamples = static_cast<int>(
                av_rescale_rnd(delayed, audioEncoderCtx->sample_rate, audioDecoderCtx->sample_rate, AV_ROUND_UP)
            );
            FramePtr converted(av_frame_alloc());   // 分配一个输出音频帧
            if (!converted) {
                return false;
            }
            converted->nb_samples = outputSamples;
            converted->format = audioEncoderCtx->sample_fmt;
            converted->sample_rate = audioEncoderCtx->sample_rate;
            if ((ret = av_channel_layout_copy(&converted->ch_layout, &audioEncoderCtx->ch_layout)) < 0 ||
                (ret = av_frame_get_buffer(converted.get(), 0)) < 0) {
                return false;
            }
            // nullptr, 0 表示不再传入新的输入音频，只请求重采样器输出内部缓存
            const int convertedSamples = swr_convert(resamplerCtx.get(), converted->data, outputSamples, nullptr, 0);  // 返回这次从内部缓存真正取出的音频样本数
            if (convertedSamples < 0) {
                return false;
            }
            if (convertedSamples == 0) {
                return true;
            }
            ret = av_audio_fifo_realloc(audioFifo.get(), av_audio_fifo_size(audioFifo.get()) + convertedSamples);
            if (ret < 0 || av_audio_fifo_write(audioFifo.get(), reinterpret_cast<void**>(converted->data), convertedSamples) < convertedSamples) {
                return false;
            }
        }
    };

    auto decodePacket = [&](AVPacket* inputPacket, AVCodecContext* decoder, bool isVideo) -> bool {
        // 把一个压缩的音频或视频数据包交给解码器，让解码器开始处理它
        // 成功返回只表示：解码器接收了这个 packet
        ret = avcodec_send_packet(decoder, inputPacket);    
        if (ret < 0) {
            return false;
        }
        while (true) {
            ret = avcodec_receive_frame(decoder, decodedFrame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return true;
            }
            if (ret < 0) {
                return false;
            }
            const bool ok = isVideo ? encodeVideoFrame(decodedFrame.get()) : convertAudio(decodedFrame.get());
            av_frame_unref(decodedFrame.get());
            if (!ok) {
                return false;
            }
        }
    };

    while ((ret = av_read_frame(inputCtx.get(), packet.get())) >= 0) {
        const bool isVideo = packet->stream_index == videoIndex;
        const bool isAudio = packet->stream_index == audioIndex;
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
    if (!decodePacket(nullptr, audioDecoderCtx.get(), false)) {
        return fail("Failed to flush audio decoder", ret);
    }
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

    ret = av_write_trailer(outputCtx.get());
    if (ret < 0) {
        return fail("Failed to write MP4 trailer", ret);
    }
    if (options.verbose) {
        std::cout << "Transcode finished. Video frames: " << videoFrames
                  << ", audio frames: " << audioFrames << "\n"
                  << "Output file: " << outputUrl << "\n";
    }
    return true;
}

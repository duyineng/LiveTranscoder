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

struct FormatInputDeleter {
    void operator()(AVFormatContext* context) const {
        if (context) {
            avformat_close_input(&context);
        }
    }
};

struct FormatOutputDeleter {
    void operator()(AVFormatContext* context) const {
        if (context) {
            if (context->pb) {
                avio_closep(&context->pb);
            }
            avformat_free_context(context);
        }
    }
};

struct CodecDeleter {
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

struct SwsDeleter {
    void operator()(SwsContext* context) const { sws_freeContext(context); }
};

struct SwrDeleter {
    void operator()(SwrContext* context) const { swr_free(&context); }
};

struct AudioFifoDeleter {
    void operator()(AVAudioFifo* fifo) const { av_audio_fifo_free(fifo); }
};

using InputPtr = std::unique_ptr<AVFormatContext, FormatInputDeleter>;
using OutputPtr = std::unique_ptr<AVFormatContext, FormatOutputDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using FifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

int chooseSampleRate(const AVCodec* codec, int requested, int fallback) {
    if (!codec->supported_samplerates) {
        return requested > 0 ? requested : fallback;
    }

    int best = codec->supported_samplerates[0];
    int bestDistance = std::abs(best - requested);
    for (const int* rate = codec->supported_samplerates; *rate; ++rate) {
        const int distance = std::abs(*rate - requested);
        if (distance < bestDistance) {
            best = *rate;
            bestDistance = distance;
        }
    }
    return best;
}

AVSampleFormat chooseSampleFormat(const AVCodec* codec) {
    if (!codec->sample_fmts) {
        return AV_SAMPLE_FMT_FLTP;
    }
    for (const AVSampleFormat* format = codec->sample_fmts; *format != AV_SAMPLE_FMT_NONE; ++format) {
        if (*format == AV_SAMPLE_FMT_FLTP) {
            return *format;
        }
    }
    return codec->sample_fmts[0];
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
        if (!av_codec_is_encoder(codec) || codec->id != AV_CODEC_ID_H264 || !codec->pix_fmts) {
            continue;
        }
        bool acceptsYuv420p = false;
        for (const AVPixelFormat* format = codec->pix_fmts; *format != AV_PIX_FMT_NONE; ++format) {
            acceptsYuv420p = acceptsYuv420p || *format == AV_PIX_FMT_YUV420P;
        }
        if (acceptsYuv420p) {
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

    AVFormatContext* inputRaw = nullptr;    // 容器格式上下文，例如 MP4、FLV
    int ret = avformat_open_input(&inputRaw, inputUrl.c_str(), nullptr, nullptr);   // 分配并初始化 inputRaw
    if (ret < 0) {
        return fail("Failed to open input: " + inputUrl, ret);
    }
    InputPtr input(inputRaw);
    inputRaw = nullptr;

    ret = avformat_find_stream_info(input.get(), nullptr);  // 继续探测，在上下文中补充音视频流信息
    if (ret < 0) {
        return fail("Failed to find input stream info", ret);
    }

    // 最普通的 MP4 可能只有：[0] AAC 音频，[1] H.264 视频，此时自然分别选中唯一的音频流和视频流
    // 但实际媒体也可能有：
    // 视频流 0：1080p 主画面
    // 视频流 1：360p 预览画面
    // 音频流 0：中文
    // 音频流 1：英文
    // 音频流 2：导演评论音轨
    const int videoIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);    // 寻找“最佳的视频流”，并返回它们在 input->streams 数组中的索引
    const int audioIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);    // 寻找“最佳的音频流”，并返回它们在 input->streams 数组中的索引
    if (videoIndex < 0) {
        return fail("Input has no video stream", videoIndex);
    }
    if (audioIndex < 0) {
        return fail("Input has no audio stream", audioIndex);
    }

    // 获取选中的视频流和音频流对象，在媒体容器中，“流”又通常可理解为“轨道”
    // AVStream 主要保存流的描述信息，例如媒体类型、编码格式、时间基、视频分辨率或音频采样率等
    // 具体的压缩数据由 AVPacket 逐个承载
    const AVStream* inputVideoStream = input->streams[videoIndex];
    const AVStream* inputAudioStream = input->streams[audioIndex];

    // 获取解码器描述信息对象，不是已经运行的解码器实例
    const AVCodec* videoDecoder = avcodec_find_decoder(inputVideoStream->codecpar->codec_id);   // codec_id 为编码格式 id，常见如 AV_CODEC_ID_H264，AV_CODEC_ID_HEVC
    const AVCodec* audioDecoder = avcodec_find_decoder(inputAudioStream->codecpar->codec_id);
    if (!videoDecoder || !audioDecoder) {
        return fail("Input decoder is not available");
    }

    // 创建空的解码器上下文，并关联 AVCodec* 所代表的解码器类型
    CodecPtr videoDecoderContext(avcodec_alloc_context3(videoDecoder));
    CodecPtr audioDecoderContext(avcodec_alloc_context3(audioDecoder));
    if (!videoDecoderContext || !audioDecoderContext) {
        return fail("Failed to allocate decoder context");
    }
    
    ret = avcodec_parameters_to_context(videoDecoderContext.get(), inputVideoStream->codecpar); // 把输入视频流中的编码参数，复制到实际的视频解码器上下文中
    if (ret < 0) {
        return fail("Failed to configure video decoder", ret);
    }
    ret = avcodec_parameters_to_context(audioDecoderContext.get(), inputAudioStream->codecpar);
    if (ret < 0) {
        return fail("Failed to configure audio decoder", ret);
    }
    // 真正的初始化并打开解码器上下文
    ret = avcodec_open2(videoDecoderContext.get(), videoDecoder, nullptr);
    if (ret < 0) {
        return fail("Failed to open video decoder", ret);
    }
    ret = avcodec_open2(audioDecoderContext.get(), audioDecoder, nullptr);
    if (ret < 0) {
        return fail("Failed to open audio decoder", ret);
    }

    // 编码器描述信息对象
    const AVCodec* videoEncoder = findSoftwareH264Encoder();
    const AVCodec* audioEncoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!videoEncoder || !audioEncoder) {
        return fail("H.264 or AAC encoder is not available");
    }

    // 创建空的编码器上下文，并关联 AVCodec* 所代表的编码器类型
    CodecPtr videoEncoderContext(avcodec_alloc_context3(videoEncoder));
    CodecPtr audioEncoderContext(avcodec_alloc_context3(audioEncoder));
    if (!videoEncoderContext || !audioEncoderContext) {
        return fail("Failed to allocate encoder context");
    }

    AVRational frameRate = inputVideoStream->avg_frame_rate;    // 输入视频的平均帧率
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        frameRate = AVRational{30, 1};
    }
    videoEncoderContext->width = options.width;
    videoEncoderContext->height = options.height;
    videoEncoderContext->pix_fmt = AV_PIX_FMT_YUV420P;      // 输入视频帧的像素格式
    videoEncoderContext->time_base = av_inv_q(frameRate);   // 编码器时间戳的单位是 1 / 30 秒
    videoEncoderContext->framerate = frameRate;             // 编码器的目标帧率
    videoEncoderContext->bit_rate = options.videoBitrate;   // 编码器的目标比特率，如 2.5 Mbps，
    videoEncoderContext->gop_size = options.gopSize;        // Group Of Pictures，表示一组连续视频帧，gop_size，表示两个关键帧（I 帧）之间最多间隔多少帧
    videoEncoderContext->max_b_frames = 2;
    if (videoEncoder->id == AV_CODEC_ID_H264) { // 判断当前找到的编码器是不是 H.264 编码器
        av_opt_set(videoEncoderContext->priv_data, "preset", "veryfast", 0);    // 编码较快，压缩效率较低，CPU占用较低，适合实时转码
        av_opt_set(videoEncoderContext->priv_data, "tune", "zerolatency", 0);
    }

    const int requestedRate = options.audioSampleRate > 0 ? options.audioSampleRate : audioDecoderContext->sample_rate;
    audioEncoderContext->sample_rate = chooseSampleRate(audioEncoder, requestedRate, 48'000);
    audioEncoderContext->sample_fmt = chooseSampleFormat(audioEncoder);
    audioEncoderContext->bit_rate = options.audioBitrate;
    audioEncoderContext->time_base = AVRational{1, audioEncoderContext->sample_rate};
    av_channel_layout_default(  // 给音频设置声道布局
        &audioEncoderContext->ch_layout,    // 声道布局变量
        audioDecoderContext->ch_layout.nb_channels > 0 ? audioDecoderContext->ch_layout.nb_channels : 2
    );
    ret = avcodec_open2(videoEncoderContext.get(), videoEncoder, nullptr);
    if (ret < 0) {
        return fail("Failed to open H.264 encoder", ret);
    }
    ret = avcodec_open2(audioEncoderContext.get(), audioEncoder, nullptr);
    if (ret < 0) {
        return fail("Failed to open AAC encoder", ret);
    }

    SwsPtr scaler(
        sws_getContext(
            videoDecoderContext->width, videoDecoderContext->height, videoDecoderContext->pix_fmt,  // 解码器输出的视频格式，例如：568x320，YUV420P
            videoEncoderContext->width, videoEncoderContext->height, videoEncoderContext->pix_fmt,  // 编码器输出的视频格式，例如：1280x720，YUV420P
            SWS_BILINEAR, nullptr, nullptr, nullptr // 表示缩放时使用“双线性插值”算法
        )
    );
    if (!scaler) {
        return fail("Failed to create video scaler");
    }

    AVChannelLayout inputLayout = audioDecoderContext->ch_layout;
    if (inputLayout.nb_channels <= 0) {
        av_channel_layout_default(&inputLayout, 2);
    }
    SwrContext* swrRaw = nullptr;
    ret = swr_alloc_set_opts2(
        &swrRaw,
        &audioEncoderContext->ch_layout, audioEncoderContext->sample_fmt, audioEncoderContext->sample_rate, // 声道布局，采样格式，采样率
        &inputLayout, audioDecoderContext->sample_fmt, audioDecoderContext->sample_rate,
        0, nullptr
    );
    SwrPtr resampler(swrRaw);
    if (ret < 0 || !resampler) {
        return fail("Failed to create audio resampler", ret);
    }
    ret = swr_init(resampler.get());
    if (ret < 0) {
        return fail("Failed to initialize audio resampler", ret);
    }

    AVFormatContext* outputRaw = nullptr;   // 输出容器
    ret = avformat_alloc_output_context2(&outputRaw, nullptr, "mp4", outputUrl.c_str());
    if (ret < 0 || !outputRaw) {
        return fail("Failed to create MP4 output context", ret);
    }
    OutputPtr output(outputRaw);
    AVStream* outputVideoStream = avformat_new_stream(output.get(), nullptr);   // 主要是用来描述一条视频轨道的信息，真正的一帧帧视频数据保存在 AVPacket
    AVStream* outputAudioStream = avformat_new_stream(output.get(), nullptr);   // 主要是用来描述一条音频轨道的信息
    if (!outputVideoStream || !outputAudioStream) {
        return fail("Failed to create output streams");
    }
    outputVideoStream->time_base = videoEncoderContext->time_base;
    outputAudioStream->time_base = audioEncoderContext->time_base;
    ret = avcodec_parameters_from_context(outputVideoStream->codecpar, videoEncoderContext.get());  // 把编码器上下文中的静态编码参数复制到输出流的 codecpar 中
    if (ret < 0) {
        return fail("Failed to copy video encoder parameters", ret);
    }
    ret = avcodec_parameters_from_context(outputAudioStream->codecpar, audioEncoderContext.get());
    if (ret < 0) {
        return fail("Failed to copy audio encoder parameters", ret);
    }
    // output->oformat->flags，输出容器格式的标志集合
    // AVFMT_NOFILE，该格式不需要 FFmpeg 手动打开文件 IO
    if (!(output->oformat->flags & AVFMT_NOFILE)) { 
        ret = avio_open(&output->pb, outputUrl.c_str(), AVIO_FLAG_WRITE);   // avio_open() 会打开输出地址，并创建一个 AVIOContext，保存到 output->pb，为 mp4 文件的写入通道
        if (ret < 0) {
            return fail("Failed to open output: " + outputUrl, ret);
        }
    }
    ret = avformat_write_header(output.get(), nullptr); // 向输出文件写入“容器文件头”
    if (ret < 0) {
        return fail("Failed to write MP4 header", ret);
    }

    // audioEncoderContext->frame_size 为每个声道一帧中应有的采样点数
    const int audioFrameSize = audioEncoderContext->frame_size > 0 ? audioEncoderContext->frame_size : 1024;
    FifoPtr audioFifo(
        av_audio_fifo_alloc(audioEncoderContext->sample_fmt, audioEncoderContext->ch_layout.nb_channels, 1)
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

    auto writeEncodedPackets = [&ret, &packet, &output](AVCodecContext* encoder, AVStream* stream) -> bool {
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
            ret = av_interleaved_write_frame(output.get(), packet.get());
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
        converted->format = videoEncoderContext->pix_fmt;
        converted->width = videoEncoderContext->width;
        converted->height = videoEncoderContext->height;
        if ((ret = av_frame_get_buffer(converted.get(), 32)) < 0) {
            return false;
        }
        ret = sws_scale(
            scaler.get(),       // SwsContext*
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
        converted->pts = av_rescale_q(sourcePts, inputVideoStream->time_base, videoEncoderContext->time_base);
        ++videoFrames;
        ret = avcodec_send_frame(videoEncoderContext.get(), converted.get());
        return ret >= 0 && writeEncodedPackets(videoEncoderContext.get(), outputVideoStream);
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
            audioFrame->format = audioEncoderContext->sample_fmt;   
            audioFrame->sample_rate = audioEncoderContext->sample_rate; // 采样率
            ret = av_channel_layout_copy(&audioFrame->ch_layout, &audioEncoderContext->ch_layout);  // 复制通道布局
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
                    audioEncoderContext->ch_layout.nb_channels, 
                    audioEncoderContext->sample_fmt
                );
            }
            audioFrame->pts = nextAudioPts;
            nextAudioPts += audioFrameSize; // 下一帧音频的 PTS，要在当前帧 PTS 的基础上向后移动“这一帧包含的采样数”
            ++audioFrames;
            ret = avcodec_send_frame(audioEncoderContext.get(), audioFrame.get());
            if (ret < 0 || !writeEncodedPackets(audioEncoderContext.get(), outputAudioStream)) {
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
                swr_get_delay(resampler.get(), audioDecoderContext->sample_rate) + source->nb_samples,  // 重采样器内部尚未输出的延迟采样数 + 每个声道包含的采样数
                audioEncoderContext->sample_rate,   // 目标采样率，例如 48000 Hz
                audioDecoderContext->sample_rate,   // 输入采样率，例如 44100 Hz
                AV_ROUND_UP // 向上取整
            )
        );
        FramePtr converted(av_frame_alloc());
        if (!converted) {
            return false;
        }
        converted->nb_samples = outputSamples;  // 每个声道的采样数
        converted->format = audioEncoderContext->sample_fmt;
        converted->sample_rate = audioEncoderContext->sample_rate;
        if ((ret = av_channel_layout_copy(&converted->ch_layout, &audioEncoderContext->ch_layout)) < 0 ||
            (ret = av_frame_get_buffer(converted.get(), 0)) < 0) {
            return false;
        }
        const int convertedSamples = swr_convert(   // convertedSamples 表示这次实际生成的输出采样数，也是“每个声道”的数量
            resampler.get(), 
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
            const int delayed = swr_get_delay(resampler.get(), audioDecoderContext->sample_rate);   // 查询重采样器里还有多少未输出的“输入采样等效量”
            if (delayed <= 0) {
                return true;
            }
            const int outputSamples = static_cast<int>(
                av_rescale_rnd(delayed, audioEncoderContext->sample_rate, audioDecoderContext->sample_rate, AV_ROUND_UP)
            );
            FramePtr converted(av_frame_alloc());   // 分配一个输出音频帧
            if (!converted) {
                return false;
            }
            converted->nb_samples = outputSamples;
            converted->format = audioEncoderContext->sample_fmt;
            converted->sample_rate = audioEncoderContext->sample_rate;
            if ((ret = av_channel_layout_copy(&converted->ch_layout, &audioEncoderContext->ch_layout)) < 0 ||
                (ret = av_frame_get_buffer(converted.get(), 0)) < 0) {
                return false;
            }
            // nullptr, 0 表示不再传入新的输入音频，只请求重采样器输出内部缓存
            const int convertedSamples = swr_convert(resampler.get(), converted->data, outputSamples, nullptr, 0);  // 返回这次从内部缓存真正取出的音频样本数
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

    while ((ret = av_read_frame(input.get(), packet.get())) >= 0) {
        const bool isVideo = packet->stream_index == videoIndex;
        const bool isAudio = packet->stream_index == audioIndex;
        if ((isVideo || isAudio) && !decodePacket(packet.get(), isVideo ? videoDecoderContext.get() : audioDecoderContext.get(), isVideo)) {
            return fail("Failed during decode or encode", ret);
        }
        av_packet_unref(packet.get());
    }
    if (ret != AVERROR_EOF) {
        return fail("Failed while reading input packet", ret);
    }

    if (!decodePacket(nullptr, videoDecoderContext.get(), true)) {
        return fail("Failed to flush video decoder or encoder", ret);
    }
    ret = avcodec_send_frame(videoEncoderContext.get(), nullptr);
    if (ret < 0 || !writeEncodedPackets(videoEncoderContext.get(), outputVideoStream)) {
        return fail("Failed to flush video encoder", ret);
    }
    if (!decodePacket(nullptr, audioDecoderContext.get(), false)) {
        return fail("Failed to flush audio decoder", ret);
    }
    if (!flushResampler()) {
        return fail("Failed to flush audio resampler", ret);
    }
    if (!encodeAudioFrames(true)) {
        return fail("Failed to flush audio FIFO", ret);
    }
    ret = avcodec_send_frame(audioEncoderContext.get(), nullptr);
    if (ret < 0 || !writeEncodedPackets(audioEncoderContext.get(), outputAudioStream)) {
        return fail("Failed to flush AAC encoder", ret);
    }

    ret = av_write_trailer(output.get());
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

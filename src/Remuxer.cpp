#include "Remuxer.h"

#include <array>
#include <iostream>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/codec_par.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

Remuxer::~Remuxer() {
    close();
    close();
}

std::string Remuxer::makeError(int errnum) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errnum, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

bool Remuxer::shouldCopyStream(int mediaType, const RemuxOptions& options) const {
    if (mediaType == AVMEDIA_TYPE_VIDEO) {
        return options.copyVideo;
    }

    if (mediaType == AVMEDIA_TYPE_AUDIO) {
        return options.copyAudio;
    }

    // 第一版先不处理字幕、附件、数据流。
    return false;
}

bool Remuxer::remux(const std::string& inputUrl, const std::string& outputUrl, const RemuxOptions& options, std::string* error) {
    close();

    int ret = 0;
    std::vector<int> streamMapping;
    int outputStreamIndex = 0;

    auto setError = [&](const std::string& message, int errnum = 0) {
        if (!error) {
            return;
        }

        *error = message;

        if (errnum < 0) {
            *error += ": ";
            *error += makeError(errnum);
        }
    };

    packet_ = av_packet_alloc();
    if (!packet_) {
        setError("Failed to allocate AVPacket");
        close();
        return false;
    }

    // 打开输入文件，例如 input.mp4
    ret = avformat_open_input(&inputCtx_, inputUrl.c_str(), nullptr, nullptr);
    if (ret < 0) {
        setError("Failed to open input: " + inputUrl, ret);
        close();
        return false;
    }

    // 读取输入流信息，例如视频流、音频流、编码格式、time_base
    ret = avformat_find_stream_info(inputCtx_, nullptr);
    if (ret < 0) {
        setError("Failed to find input stream info", ret);
        close();
        return false;
    }

    if (options.verbose) {
        std::cout << "\n========== Input Format ==========\n";
        av_dump_format(inputCtx_, 0, inputUrl.c_str(), 0);  // 打印信息
    }

    // 根据 outputUrl 后缀创建输出封装，output.flv 会自动选择 flv muxer
    ret = avformat_alloc_output_context2(&outputCtx_, nullptr, nullptr, outputUrl.c_str());
    if (ret < 0 || !outputCtx_) {
        setError("Failed to create output context: " + outputUrl, ret);
        close();
        return false;
    }

    const AVOutputFormat* outputFormat = outputCtx_->oformat;   // 输出格式，例如 flv

    streamMapping.assign(inputCtx_->nb_streams, -1);

    // 遍历输入流，为输出文件创建对应流
    for (unsigned int i = 0; i < inputCtx_->nb_streams; ++i) {
        AVStream* inStream = inputCtx_->streams[i];
        AVCodecParameters* inCodecPar = inStream->codecpar;

        if (!shouldCopyStream(inCodecPar->codec_type, options)) {
            streamMapping[i] = -1;
            continue;
        }

        AVStream* outStream = avformat_new_stream(outputCtx_, nullptr);
        if (!outStream) {
            setError("Failed to allocate output stream");
            close();
            return false;
        }

        streamMapping[i] = outputStreamIndex++;

        // 直接复制编码参数
        // 例如 H.264 / AAC 的 codec id、extradata、分辨率、采样率等
        ret = avcodec_parameters_copy(outStream->codecpar, inCodecPar);
        if (ret < 0) {
            setError("Failed to copy codec parameters", ret);
            close();
            return false;
        }

        // 不复制原容器里的 codec_tag
        // 例如 MP4 里的 avc1 tag 不一定适合 FLV
        outStream->codecpar->codec_tag = 0;

        // 先请求输出流使用输入流的 time_base。
        // 注意：avformat_write_header 后，muxer 可能会调整它
        outStream->time_base = inStream->time_base;
    }

    if (outputStreamIndex == 0) {
        setError("No audio/video stream found to remux");
        close();
        return false;
    }

    if (options.verbose) {
        std::cout << "\n========== Output Format ==========\n";
        av_dump_format(outputCtx_, 0, outputUrl.c_str(), 1);
    }

    // 如果输出格式不带 AVFMT_NOFILE 标志
    // 说明 FFmpeg 需要自己打开输出 IO，则调用 avio_open()
    if (!(outputFormat->flags & AVFMT_NOFILE)) {   
        ret = avio_open(&outputCtx_->pb, outputUrl.c_str(), AVIO_FLAG_WRITE);   // 成功后，outputCtx_->pb 指向一个可写的 AVIOContext
        if (ret < 0) {
            setError("Failed to open output: " + outputUrl, ret);
            close();
            return false;
        }
    }

    // 写输出文件头
    ret = avformat_write_header(outputCtx_, nullptr);   // 内部通过 outputCtx_->pb 往文件中写入容器头信息
    if (ret < 0) {
        setError("Failed to write output header", ret);
        close();
        return false;
    }

    int64_t packetCount = 0;

    // 主循环：读取输入 packet，转换时间戳，写到输出。
    while (true) {
        ret = av_read_frame(inputCtx_, packet_);    // AVPacket 可能是 H.264 编码视频包，AAC 编码音频包
        if (ret < 0) {
            break;
        }

        const int inputStreamIndex = packet_->stream_index; // 每个 packet 都属于某一条输入流，记录它来自哪条流，例如：0：视频流，1：音频流

        if (
            inputStreamIndex < 0 ||
            inputStreamIndex >= static_cast<int>(streamMapping.size()) ||
            streamMapping[inputStreamIndex] < 0     // 表示输入流没有对应的输出流
        ) {
            av_packet_unref(packet_);
            continue;
        }

        AVStream* inStream = inputCtx_->streams[inputStreamIndex];

        const int mappedOutputIndex = streamMapping[inputStreamIndex];
        AVStream* outStream = outputCtx_->streams[mappedOutputIndex];

        packet_->stream_index = mappedOutputIndex;

        // 关键点：把 packet 的 pts / dts / duration 从输入 time_base 转成输出 time_base。
        av_packet_rescale_ts(
            packet_,
            inStream->time_base,
            outStream->time_base
        );

        packet_->pos = -1;

        // 使用 av_interleaved_write_frame，让 FFmpeg 帮我们处理音视频交错写入
        ret = av_interleaved_write_frame(outputCtx_, packet_);
        if (ret < 0) {
            setError("Failed to write packet", ret);
            close();
            return false;
        }

        ++packetCount;
    }

    if (ret != AVERROR_EOF) {
        setError("Failed while reading input packet", ret);
        close();
        return false;
    }

    // 写 trailer，完成输出文件
    ret = av_write_trailer(outputCtx_);
    if (ret < 0) {
        setError("Failed to write output trailer", ret);
        close();
        return false;
    }

    if (options.verbose) {
        std::cout << "\nRemux finished. Packet count: " << packetCount << "\n";
        std::cout << "Output file: " << outputUrl << "\n";
    }

    close();
    return true;
}

void Remuxer::close() {
    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

    if (inputCtx_) {
        avformat_close_input(&inputCtx_);
        inputCtx_ = nullptr;
    }

    if (outputCtx_) {
        const AVOutputFormat* outputFormat = outputCtx_->oformat;

        if (
            outputFormat &&
            !(outputFormat->flags & AVFMT_NOFILE) &&
            outputCtx_->pb
            ) {
            avio_closep(&outputCtx_->pb);
        }

        avformat_free_context(outputCtx_);
        outputCtx_ = nullptr;
    }
}

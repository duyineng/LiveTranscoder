#include "Remux.h"

#include "FFmpegSupport.h"

#include <iostream>
#include <vector>

bool remux(AVFormatContext* input, const std::string& outputUrl, std::string& error) {
    auto fail = [&](const std::string& message, int errnum = 0) -> bool {
        error = message;
        if (errnum < 0) {
            error += ": ";
            error += errorString(errnum);
        }
        return false;
    };

    AVFormatContext* outputFmtCtxRaw = nullptr;
    int ret = avformat_alloc_output_context2(&outputFmtCtxRaw, nullptr, nullptr, outputUrl.c_str());
    if (ret < 0 || !outputFmtCtxRaw) {
        return fail("Failed to create output context: " + outputUrl, ret);
    }
    OutputFormatContextPtr outputFmtCtx(outputFmtCtxRaw);

    std::vector<int> streamMapping(input->nb_streams, -1);
    int outputStreamIndex = 0;
    for (unsigned int i = 0; i < input->nb_streams; ++i) {
        AVStream* inputStream = input->streams[i];
        const AVMediaType mediaType = inputStream->codecpar->codec_type;
        if (mediaType != AVMEDIA_TYPE_VIDEO && mediaType != AVMEDIA_TYPE_AUDIO) {
            continue;
        }

        AVStream* outputStream = avformat_new_stream(outputFmtCtx.get(), nullptr);
        if (!outputStream) {
            return fail("Failed to create output stream");
        }
        streamMapping[i] = outputStreamIndex++;
        ret = avcodec_parameters_copy(outputStream->codecpar, inputStream->codecpar);
        if (ret < 0) {
            return fail("Failed to copy stream parameters", ret);
        }
        // 清掉原容器的 codec_tag，例如 MP4 的 avc1 不一定适合 FLV。
        outputStream->codecpar->codec_tag = 0;
        outputStream->time_base = inputStream->time_base;
    }
    if (outputStreamIndex == 0) {
        return fail("No audio or video stream to copy");
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

    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        return fail("Failed to allocate packet");
    }
    int64_t packetCount = 0;
    while ((ret = av_read_frame(input, packet.get())) >= 0) {
        const int inputStreamIndex = packet->stream_index;
        if (
            inputStreamIndex < 0
            || inputStreamIndex >= static_cast<int>(streamMapping.size())
            || streamMapping[inputStreamIndex] < 0
        ) {
            av_packet_unref(packet.get());
            continue;
        }

        AVStream* inputStream = input->streams[inputStreamIndex];
        AVStream* outputStream = outputFmtCtx->streams[streamMapping[inputStreamIndex]];
        packet->stream_index = streamMapping[inputStreamIndex];
        av_packet_rescale_ts(packet.get(), inputStream->time_base, outputStream->time_base);
        packet->pos = -1;
        ret = av_interleaved_write_frame(outputFmtCtx.get(), packet.get());
        if (ret < 0) {
            return fail("Failed to write packet", ret);
        }
        ++packetCount;
    }
    if (ret != AVERROR_EOF) {
        return fail("Failed while reading input packet", ret);
    }
    ret = av_write_trailer(outputFmtCtx.get());
    if (ret < 0) {
        return fail("Failed to write output trailer", ret);
    }
    std::cout << "Container copy finished. Packet count: " << packetCount << "\n"
              << "Output file: " << outputUrl << "\n";
    return true;
}

#include "Transcoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <array>
#include <iostream>
#include <string>

class FFmpegNetworkGuard {
public:
    FFmpegNetworkGuard()
        : result_(avformat_network_init()) {
    }

    ~FFmpegNetworkGuard() {
        if (result_ >= 0) {
            avformat_network_deinit();
        }
    }

    FFmpegNetworkGuard(const FFmpegNetworkGuard&) = delete;
    FFmpegNetworkGuard& operator=(const FFmpegNetworkGuard&) = delete;

    bool isInitialized() const {
        return result_ == 0;
    }

    std::string errorMessage() const {
        if (result_ == 0) {
            return {};
        }

        std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
        const int ret = av_strerror(result_, buffer.data(), buffer.size());
        if (ret < 0) {
            return "Unknown FFmpeg error: " + std::to_string(result_);
        }

        return std::string(buffer.data());
    }

private:
    int result_ = 0;
};


namespace {

    void printUsage() {
        std::cout
            << "Usage:\n"
            << "  LiveTranscoder transcode <input_file_or_url> <output_file> [width height]\n"
            << "  LiveTranscoder hls <input_file> <output_dir> [width height]\n"
            << "\nExamples:\n"
            << "  LiveTranscoder transcode test.mp4 output.flv\n"
            << "  LiveTranscoder transcode test.mp4 output.flv 1280 720\n"
            << "  LiveTranscoder hls test.mp4 out_dir\n"
            << "  LiveTranscoder hls test.mp4 out_dir 1280 720\n";
    }

    void printVersion() {
        std::cout << "FFmpeg: " << av_version_info() << "\n";
        std::cout << "libavformat: " << LIBAVFORMAT_VERSION_MAJOR << "." << LIBAVFORMAT_VERSION_MINOR << "\n";
        std::cout << "libavcodec : " << LIBAVCODEC_VERSION_MAJOR << "." << LIBAVCODEC_VERSION_MINOR << "\n";
        std::cout << "libavutil  : " << LIBAVUTIL_VERSION_MAJOR << "." << LIBAVUTIL_VERSION_MINOR << "\n";
    }

} // namespace

int main(int argc, char* argv[]) {
    FFmpegNetworkGuard networkGuard;
    if (!networkGuard.isInitialized()) {
        std::cerr << "Failed to initialize FFmpeg network: "<< networkGuard.errorMessage()<< "\n";
        return 1;
    }

    printVersion();
    std::cout << "\n";

    if (argc < 2) {
        printUsage();
        return 0;
    }

    std::string command = argv[1];

    if (command == "transcode") {
        if (argc < 4) {
            std::cerr << "Missing input or output.\n\n";
            printUsage();
            return 1;
        }

        std::optional<TranscodeOptions> transcodeOptions;
        if (argc >= 5) {
            TranscodeOptions options;
            options.width = std::stoi(argv[4]);
            if (argc >= 6) {
                options.height = std::stoi(argv[5]);
            }
            transcodeOptions = options;
        }

        Transcoder transcoder;
        std::string error;
        if (!transcoder.transcode(argv[2], argv[3], transcodeOptions, error)) {
            std::cerr << "Transcode failed: " << error << "\n";
            return 1;
        }
        std::cout << (transcodeOptions ? "Transcode success.\n" : "Container copy success.\n");
        return 0;
    }

    if (command == "hls") {
        if (argc < 4) {
            std::cerr << "Missing input or output directory.\n\n";
            printUsage();
            return 1;
        }
        if (argc == 5) {
            std::cerr << "Width and height must both be provided.\n\n";
            printUsage();
            return 1;
        }

        HlsRequest request;
        if (argc >= 6) {
            HlsVariant variant;
            variant.width = std::stoi(argv[4]);
            variant.height = std::stoi(argv[5]);
            variant.videoBitrate = TranscodeOptions{}.videoBitrate;
            variant.name = std::to_string(variant.height) + "p";
            request.variants.push_back(std::move(variant));
        } else {
            request.variants = {
                {1920, 1080, 5'000'000, "1080p"},
                {1280, 720, 2'500'000, "720p"},
                {854, 480, 1'000'000, "480p"},
            };
        }

        Transcoder transcoder;
        std::string error;
        if (!transcoder.transcodeHls(argv[2], argv[3], request, error)) {
            std::cerr << "HLS failed: " << error << "\n";
            return 1;
        }
        std::cout << "HLS success.\n";
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n\n";
    printUsage();
    return 1;
}

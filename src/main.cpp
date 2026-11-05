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
            << "\nExamples:\n"
            << "  LiveTranscoder transcode test.mp4 output.mp4 1280 720\n"
            << "  LiveTranscoder transcode test.mp4 output.flv\n";
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

        TranscodeOptions transcodeOptions;
        if (argc >= 5) {
            transcodeOptions.width = std::stoi(argv[4]);
        }
        if (argc >= 6) {
            transcodeOptions.height = std::stoi(argv[5]);
        }

        Transcoder transcoder;
        std::string error;
        if (!transcoder.transcode(argv[2], argv[3], transcodeOptions, error)) {
            std::cerr << "Transcode failed: " << error << "\n";
            return 1;
        }
        std::cout << "Transcode success.\n";
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n\n";
    printUsage();
    return 1;
}

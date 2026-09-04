#include "MediaProbe.h"
#include "Remuxer.h"
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
            << "  LiveTranscoder version\n"
            << "  LiveTranscoder probe <input_file_or_url>\n"
            << "  LiveTranscoder remux <input_file_or_url> <output_file>\n"
            << "  LiveTranscoder transcode <input_file_or_url> <output.mp4> [width height]\n"
            << "\nExamples:\n"
        << "  LiveTranscoder probe D:\\\\videos\\\\test.mp4\n"
            << "  LiveTranscoder probe http://example.com/test.mp4\n"
            << "  LiveTranscoder transcode test.mp4 output.mp4 1280 720\n";
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

    if (command == "version") {
        return 0;
    }

    if (command == "probe") {
        if (argc < 3) {
            std::cerr << "Error: missing input file or url.\n\n";
            printUsage();
            return 1;
        }

        std::string input = argv[2];

        MediaProbe probe;
        std::string error;

        auto info = probe.probe(input, &error);
        if (!info) {
            std::cerr << "Probe failed: " << error << "\n";
            return 2;
        }

        printMediaInfo(*info);
        return 0;
    }

    if (command == "remux") {
        if (argc < 4) {
            std::cerr << "Missing input or output.\n\n";
            printUsage();
            return 1;
        }

        const std::string inputUrl = argv[2];
        const std::string outputUrl = argv[3];

        Remuxer remuxer;
        RemuxOptions options;
        options.copyVideo = true;
        options.copyAudio = true;
        options.verbose = true;

        std::string error;
        if (!remuxer.remux(inputUrl, outputUrl, options, &error)) {
            std::cerr << "Remux failed: " << error << "\n";
            return 1;
        }

        std::cout << "Remux success.\n";
        return 0;
    }

    if (command == "transcode") {
        if (argc < 4) {
            std::cerr << "Missing input or output.\n\n";
            printUsage();
            return 1;
        }

        TranscodeOptions options;
        if (argc >= 5) {
            options.width = std::stoi(argv[4]);
        }
        if (argc >= 6) {
            options.height = std::stoi(argv[5]);
        }

        Transcoder transcoder;
        std::string error;
        if (!transcoder.transcode(argv[2], argv[3], options, &error)) {
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

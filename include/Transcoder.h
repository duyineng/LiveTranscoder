#pragma once

#include <string>

struct TranscodeOptions {
    int width = 1280;
    int height = 720;
    int videoBitrate = 2'500'000;
    int audioBitrate = 128'000;
    int audioSampleRate = 48'000;
    int gopSize = 60;
    bool verbose = true;
};

class Transcoder {
public:
    bool transcode(
        const std::string& inputUrl,
        const std::string& outputUrl,
        const TranscodeOptions& options = {},
        std::string* error = nullptr
    );

private:
    static std::string makeError(int errnum);
};

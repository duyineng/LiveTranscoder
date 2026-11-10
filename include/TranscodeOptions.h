#pragma once

#include <string>
#include <vector>

struct TranscodeOptions {
    int width = 1280;
    int height = 720;
    int videoBitrate = 2'500'000;
    int audioBitrate = 128'000;
    int audioSampleRate = 48'000;
    int gopSize = 60;
    bool verbose = true;
};

struct HlsVariant {
    int width = 0;
    int height = 0;
    int videoBitrate = 0;
    std::string name;
};

struct HlsRequest {
    std::vector<HlsVariant> variants;
    int audioBitrate = 128'000;
    int audioSampleRate = 48'000;
    int segmentDurationSeconds = 2;
    bool verbose = true;
};

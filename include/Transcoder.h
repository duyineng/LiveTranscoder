#pragma once

#include "TranscodeOptions.h"

#include <optional>
#include <string>

class Transcoder {
public:
    bool transcode(
        const std::string& inputUrl,
        const std::string& outputUrl,
        std::optional<TranscodeOptions> transcodeOptions,
        std::string& error
    );

    bool transcodeHls(
        const std::string& inputUrl,
        const std::string& outputDir,
        const HlsRequest& request,
        std::string& error
    );
};

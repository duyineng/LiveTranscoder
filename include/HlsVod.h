#pragma once

#include "TranscodeOptions.h"

#include <string>

bool hlsVod(
    const std::string& inputUrl,
    const std::string& outputDir,
    const HlsRequest& request,
    std::string& error
);

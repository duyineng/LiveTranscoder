#pragma once

#include "TranscodeOptions.h"

#include <string>

struct AVFormatContext;

bool fileTranscode(
    AVFormatContext* input,
    const std::string& outputUrl,
    const TranscodeOptions& options,
    std::string& error
);

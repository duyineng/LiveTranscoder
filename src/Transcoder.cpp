#include "Transcoder.h"

#include "FFmpegSupport.h"
#include "FileTranscode.h"
#include "HlsVod.h"
#include "Remux.h"

bool Transcoder::transcode(
    const std::string& inputUrl,
    const std::string& outputUrl,
    std::optional<TranscodeOptions> transcodeOptions,
    std::string& error
) {
    InputFormatContextPtr input;
    if (!openInput(inputUrl, input, error)) {
        return false;
    }

    if (!transcodeOptions) {
        return remux(input.get(), outputUrl, error);
    }
    if (transcodeOptions->width <= 0 || transcodeOptions->height <= 0) {
        error = "Encoder input frame dimensions must be positive";
        return false;
    }
    return fileTranscode(input.get(), outputUrl, *transcodeOptions, error);
}

bool Transcoder::transcodeHls(
    const std::string& inputUrl,
    const std::string& outputDir,
    const HlsRequest& request,
    std::string& error
) {
    return hlsVod(inputUrl, outputDir, request, error);
}

#pragma once

#include <string>

struct RemuxOptions {
    bool copyVideo = true;
    bool copyAudio = true;
    bool verbose = true;
};

class Remuxer {
public:
    Remuxer() = default;
    ~Remuxer();

    Remuxer(const Remuxer&) = delete;
    Remuxer& operator=(const Remuxer&) = delete;

    bool remux(const std::string& inputUrl, const std::string& outputUrl, const RemuxOptions& options = {}, std::string* error = nullptr);

private:
    static std::string makeError(int errnum);

    bool shouldCopyStream(int mediaType, const RemuxOptions& options) const;
    void close();

private:
    struct AVFormatContext* inputCtx_ = nullptr;
    struct AVFormatContext* outputCtx_ = nullptr;
    struct AVPacket* packet_ = nullptr;
};

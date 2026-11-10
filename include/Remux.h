#pragma once

#include <string>

struct AVFormatContext;

bool remux(AVFormatContext* input, const std::string& outputUrl, std::string& error);

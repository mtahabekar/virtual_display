#pragma once
extern "C" {
#include <libavcodec/avcodec.h>
}
namespace quest { void configureNvenc(AVCodecContext *context); }
